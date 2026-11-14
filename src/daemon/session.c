/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"
#include <arpa/inet.h>
#include <grp.h>
#include <pwd.h>
#include <stddef.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "internal.h"

static void drain_fd(int fd, int conn, uint8_t type) {
  char buf[DS_IOBUF];
  int fl = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
      break;
    ds_send_frame(conn, type, buf, (uint32_t)n);
  }
  fcntl(fd, F_SETFL, fl);
}

static void close_pipe(int p[2]) {
  if (p[0] >= 0)
    close(p[0]);
  if (p[1] >= 0)
    close(p[1]);
  p[0] = p[1] = -1;
}

static void drain_bytes(int fd, uint32_t len) {
  char buf[DS_IOBUF];
  while (len > 0) {
    uint32_t chunk = len < sizeof(buf) ? len : (uint32_t)sizeof(buf);
    if (ds_read_exact(fd, buf, chunk) < 0)
      return;
    len -= chunk;
  }
}

static int reap_child_timeout(pid_t pid, int timeout_ms, int *status) {
  int waited = 0;
  for (;;) {
    pid_t r = waitpid(pid, status, WNOHANG);
    if (r == pid)
      return 0;
    if (r < 0 && errno != EINTR)
      return -1;
    if (waited >= timeout_ms)
      return -1;
    usleep(20000);
    waited += 20;
  }
}

/* 0 when the child is known to be gone, with *status filled in. A child that
 * outlives the timeout gets SIGKILL; an already reaped pid is not an error. */
static void stop_child(pid_t child, int master, int *status) {
  if (master >= 0)
    close(master);
  if (reap_child_timeout(child, 2000, status) < 0) {
    kill(child, SIGKILL);
    if (waitpid(child, status, 0) < 0)
      *status = 0;
  }
}

/* Allocate the host-side pty and start the command that owns the other end.
 * Shared by the ordinary pty sessions and by enter, so both get the same
 * controlling terminal, umask and DS_NO_PROXY guard. */
static int spawn_pty_child(ds_req_t *r, int *master_out, pid_t *child_out) {
  int master = -1, slave = -1;
  if (ds_openpty("/dev/ptmx", &master, &slave, NULL) < 0)
    return -1;

  struct winsize ws = {r->rows, r->cols, 0, 0};
  (void)ioctl(master, TIOCSWINSZ, &ws);
  (void)fcntl(master, F_SETFD, FD_CLOEXEC);

  char **av = make_exec_argv(r);
  if (!av) {
    close(master);
    close(slave);
    return -1;
  }

  pid_t child = fork();
  if (child < 0) {
    free(av);
    close(master);
    close(slave);
    return -1;
  }

  if (child == 0) {
    close(master);
    setsid();
    (void)ioctl(slave, TIOCSCTTY, 0);
    (void)dup2(slave, STDIN_FILENO);
    (void)dup2(slave, STDOUT_FILENO);
    (void)dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO)
      close(slave);
    /* The command runs as if the client had run it, like lxc-attach does in
     * the caller's own process. A client too old to send its umask gets the
     * daemon's 022. */
    if (r->flags & REQ_FLAG_UMASK)
      umask((r->flags >> REQ_UMASK_SHIFT) & 0777);
    /* Prevent the spawned child from proxying back to the daemon */
    setenv("DS_NO_PROXY", "1", 1);
    reexec(av);
  }

  free(av);
  close(slave);
  *master_out = master;
  *child_out = child;
  return 0;
}

/* generic session handler for pipe mode and for pty sessions that do not
 * survive a client disconnect. Interactive enter has its own keeper below.
 *
 * note on pipe hangs: start_rootfs leaks descriptors to the intermediate
 * monitor process. waiting for epollhup would block forever. we poll waitpid
 * instead and do a final non-blocking drain when the direct child exits. */

static void handle_session_inner(int conn, ds_req_t *r) {
  int is_pty = (r->flags & REQ_FLAG_PTY);
  int master = -1;
  int out[2] = {-1, -1}, err[2] = {-1, -1}, in[2] = {-1, -1};
  pid_t child = -1;
  char buf[DS_IOBUF];

  if (is_pty) {
    if (spawn_pty_child(r, &master, &child) < 0) {
      const char *msg = "daemon: pty allocation failed\n";
      ds_send_frame(conn, MSG_ERR, msg, (uint32_t)strlen(msg));
      send_exit(conn, 1);
      return;
    }
  } else {
    /*
     * Create pipes with O_CLOEXEC only - NOT O_NONBLOCK.
     *
     * O_NONBLOCK is a file-description flag (shared across dup'd fds).
     * If we set it in pipe2(), the child's stdout/stderr (dup2'd from
     * the write ends) inherit the flag, causing EAGAIN / short writes for
     * programs that write more than PIPE_BUF in one call.
     *
     * We set O_NONBLOCK on the READ ends only, in the parent, after the
     * fork and after the write ends have been closed.  The child's write
     * ends are a separate file description and remain blocking.
     */
    /* Without REQ_FLAG_STDIN the child reads EOF from /dev/null, as every
     * caller that shares its stdin with a command shell (libsu) relies on. */
    if (pipe2(out, O_CLOEXEC) < 0 || pipe2(err, O_CLOEXEC) < 0 ||
        ((r->flags & REQ_FLAG_STDIN) && pipe2(in, O_CLOEXEC) < 0)) {
      const char *msg = "daemon: pipe2 failed\n";
      ds_send_frame(conn, MSG_ERR, msg, (uint32_t)strlen(msg));
      send_exit(conn, 1);
      close_pipe(out);
      close_pipe(err);
      close_pipe(in);
      return;
    }

    char **av = make_exec_argv(r);
    if (!av) {
      close_pipe(out);
      close_pipe(err);
      close_pipe(in);
      send_exit(conn, 1);
      return;
    }

    child = fork();
    if (child < 0) {
      free(av);
      close_pipe(out);
      close_pipe(err);
      close_pipe(in);
      const char *msg = "daemon: fork failed\n";
      ds_send_frame(conn, MSG_ERR, msg, (uint32_t)strlen(msg));
      send_exit(conn, 1);
      return;
    }

    if (child == 0) {
      close(conn);
      close(out[0]);
      close(err[0]);
      int dn = in[0] >= 0 ? in[0] : open("/dev/null", O_RDWR);
      if (dn >= 0) {
        dup2(dn, STDIN_FILENO);
        if (dn > STDERR_FILENO)
          close(dn);
      }
      if (in[1] >= 0)
        close(in[1]);
      dup2(out[1], STDOUT_FILENO);
      dup2(err[1], STDERR_FILENO);
      close(out[1]);
      close(err[1]);
      if (r->flags & REQ_FLAG_UMASK)
        umask((r->flags >> REQ_UMASK_SHIFT) & 0777);
      setenv("DS_NO_PROXY", "1", 1);
      reexec(av);
    }
    free(av);
  }

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    if (is_pty) {
      close(master);
    } else {
      close_pipe(out);
      close_pipe(err);
      close_pipe(in);
    }
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    send_exit(conn, 1);
    return;
  }

  /* signalfd for zero-latency SIGCHLD: avoids 100ms poll-waitpid loop */
  sigset_t ss;
  sigemptyset(&ss);
  sigaddset(&ss, SIGCHLD);
  sigprocmask(SIG_BLOCK, &ss, NULL);
  int sfd = signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC);

  struct epoll_event ev, events[8];
  int active_reads = 0;

  if (is_pty) {
    int fl = fcntl(master, F_GETFL);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);
    ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.fd = master;
    epoll_ctl(epfd, EPOLL_CTL_ADD, master, &ev);
    active_reads = 1;
  } else {
    close(out[1]);
    close(err[1]);
    out[1] = err[1] = -1;
    /* Read ends are now exclusively owned by the parent.  Set O_NONBLOCK
     * here so epoll's edge reads can drain without blocking. */
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    fcntl(err[0], F_SETFL, O_NONBLOCK);
    if (in[0] >= 0) {
      close(in[0]);
      in[0] = -1;
      fcntl(in[1], F_SETFL, O_NONBLOCK);
    }
    ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.fd = out[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, out[0], &ev);
    ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.fd = err[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, err[0], &ev);
    active_reads = 2;
  }

  /* Where client input goes: the pty master, the command's stdin pipe, or
   * nowhere (-1) when the client did not ask to relay stdin. */
  int in_fd = is_pty ? master : in[1];
  int in_eof = 0; /* pipe mode: close in_fd once the write buffer drains */

  if (sfd >= 0) {
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);
  }

  /* watch the connection for dead clients and client input */
  ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
  ev.data.fd = conn;
  epoll_ctl(epfd, EPOLL_CTL_ADD, conn, &ev);

  int exit_code = EXIT_PENDING;
  int child_done = 0; /* 0=running, 1=killed, 2=normal-exit-seen */
  int st = 0;

  /* Non-blocking write buffer for client input on its way to in_fd.
   * Prevents blocking the event loop when the pty or pipe is full (a large
   * paste, a slow reader). Excess bytes are queued here and flushed via
   * EPOLLOUT on in_fd. */
  uint8_t *pty_wbuf = NULL;
  size_t pty_wbuf_len = 0;
  size_t pty_wbuf_cap = 0;
  int conn_suspended = 0; /* 1 = conn EPOLLIN removed (wbuf high-water) */

  for (;;) {
    int nfds = epoll_wait(epfd, events, 8, -1);
    if (nfds < 0 && errno != EINTR)
      break;

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;

      if (sfd >= 0 && fd == sfd) {
        /* drain all pending SIGCHLD notifications */
        struct signalfd_siginfo si;
        while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
          if (!child_done) {
            if (waitpid(child, &st, WNOHANG) == child) {
              exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
              child_done = is_pty ? 2 : 1;
            }
          }
        }
      } else if (fd == conn) {
        if (events[i].events & (EPOLLHUP | EPOLLERR)) {
          /* Client died: a session that is not kept has nobody left to serve.
           * Closing the master first lets the command see EOF and unwind
           * before it is signalled; waiting with the master still open was
           * what left orphaned sessions behind. */
          goto client_gone;
        }
        if (events[i].events & EPOLLIN) {
          uint8_t type;
          uint32_t mlen;
          if (ds_recv_frame_hdr(conn, &type, &mlen) < 0)
            goto client_gone;
          if (type == MSG_OUT && mlen <= (uint32_t)sizeof(buf)) {
            /* Input with nowhere to go is read off the wire and dropped. */
            int got = ds_read_exact(conn, buf, mlen) == 0 && in_fd >= 0;
            if (got && mlen == 0) {
              /* The client's stdin hit EOF. A pty has no EOF to pass on, so
               * only the stdin pipe is closed, once what is queued for it
               * has drained. */
              if (!is_pty && pty_wbuf_len > 0) {
                in_eof = 1;
              } else if (!is_pty) {
                close(in_fd);
                in_fd = in[1] = -1;
              }
            } else if (got) {
              /* Non-blocking write toward the child; queue overflow into
               * wbuf. */
              size_t written = 0;
              if (pty_wbuf_len == 0) {
                /* Fast path: no pending data, try direct write first. */
                ssize_t w = write(in_fd, buf, mlen);
                if (w > 0)
                  written = (size_t)w;
              }
              size_t rem = mlen - written;
              if (rem > 0) {
                /* Buffer remainder; grow wbuf if needed (cap at PTY_WBUF_MAX).
                 */
                if (pty_wbuf_len + rem <= PTY_WBUF_MAX) {
                  if (pty_wbuf_len + rem > pty_wbuf_cap) {
                    /* Double capacity without overflow: clamp before shifting.
                     */
                    size_t ncap = pty_wbuf_cap ? pty_wbuf_cap : DS_IOBUF;
                    while (ncap < pty_wbuf_len + rem) {
                      if (ncap > PTY_WBUF_MAX / 2) {
                        ncap = PTY_WBUF_MAX;
                        break;
                      }
                      ncap *= 2;
                    }
                    if (ncap > PTY_WBUF_MAX)
                      ncap = PTY_WBUF_MAX;
                    /* Preserve old pointer on realloc failure to avoid leak. */
                    uint8_t *nb = realloc(pty_wbuf, ncap);
                    if (nb) {
                      pty_wbuf = nb;
                      pty_wbuf_cap = ncap;
                    }
                    /* If realloc failed, pty_wbuf unchanged; fall through to
                     * the capacity check below which will drop the frame. */
                  }
                  if (pty_wbuf_len + rem <= pty_wbuf_cap) {
                    memcpy(pty_wbuf + pty_wbuf_len, buf + written, rem);
                    pty_wbuf_len += rem;
                    /* Arm EPOLLOUT on in_fd to flush wbuf. The pipe end is
                     * only registered while a flush is pending, so EEXIST on
                     * a re-arm is harmless. */
                    ev.events = EPOLLOUT | EPOLLHUP | EPOLLERR;
                    if (is_pty)
                      ev.events |= EPOLLIN;
                    ev.data.fd = in_fd;
                    epoll_ctl(epfd, is_pty ? EPOLL_CTL_MOD : EPOLL_CTL_ADD,
                              in_fd, &ev);
                    /* High-watermark: suspend conn input to apply backpressure.
                     * Prevents unbounded growth; conn resumes at low-watermark.
                     */
                    if (!conn_suspended && pty_wbuf_len >= PTY_WBUF_HIGH) {
                      ev.events = EPOLLHUP | EPOLLERR; /* no EPOLLIN */
                      ev.data.fd = conn;
                      epoll_ctl(epfd, EPOLL_CTL_MOD, conn, &ev);
                      conn_suspended = 1;
                    }
                  }
                }
                /* wbuf full and conn suspended: no data lost, client stalls. */
              }
            }
          } else if (type == MSG_WINCH && mlen == 4) {
            uint16_t wd[2];
            if (ds_read_exact(conn, wd, 4) == 0 && is_pty) {
              struct winsize nws = {ntohs(wd[0]), ntohs(wd[1]), 0, 0};
              ioctl(master, TIOCSWINSZ, &nws);
              kill(child, SIGWINCH);
            }
          } else {
            /* drain unknown frames so we don't stall the pipe */
            drain_bytes(conn, mlen);
          }
        }
      } else if (fd == in_fd && (!is_pty || (events[i].events & EPOLLOUT))) {
        /* Flush pending wbuf toward the child. */
        while (pty_wbuf_len > 0) {
          ssize_t w = write(in_fd, pty_wbuf, pty_wbuf_len);
          if (w > 0) {
            pty_wbuf_len -= (size_t)w;
            if (pty_wbuf_len > 0)
              memmove(pty_wbuf, pty_wbuf + w, pty_wbuf_len);
          } else if (w < 0 && errno == EAGAIN) {
            break; /* still full; wait for the next EPOLLOUT */
          } else {
            /* Reader gone (EIO on the master, EPIPE on the pipe). Nothing is
             * left to flush to, and keeping the bytes would spin on EPOLLERR.
             */
            pty_wbuf_len = 0;
            break;
          }
        }
        if (pty_wbuf_len == 0) {
          /* All flushed; stop watching for writability. */
          if (is_pty) {
            ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
            ev.data.fd = in_fd;
            epoll_ctl(epfd, EPOLL_CTL_MOD, in_fd, &ev);
          } else {
            epoll_ctl(epfd, EPOLL_CTL_DEL, in_fd, NULL);
            if (in_eof) {
              close(in_fd);
              in_fd = in[1] = -1;
            }
          }
        }
        /* Low-watermark: resume conn input once pressure drops. */
        if (conn_suspended && pty_wbuf_len < PTY_WBUF_LOW) {
          ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
          ev.data.fd = conn;
          epoll_ctl(epfd, EPOLL_CTL_MOD, conn, &ev);
          conn_suspended = 0;
        }
        /* Also service any pending output from master in this iteration. */
        if (is_pty && (events[i].events & EPOLLIN)) {
          for (;;) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
              if (ds_send_frame(conn, MSG_OUT, buf, (uint32_t)n) < 0)
                goto client_gone;
            } else {
              if (errno == EINTR)
                continue;
              break;
            }
          }
        }
      } else {
        /* this is a read fd from the child */
        if (events[i].events & (EPOLLIN | EPOLLHUP)) {
          int drained = 0;
          for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
              uint8_t t = (fd == err[0]) ? MSG_ERR : MSG_OUT;
              if (ds_send_frame(conn, t, buf, (uint32_t)n) < 0)
                goto client_gone;
            } else if (n == 0) {
              drained = 1;
              break;
            } else {
              if (errno == EINTR)
                continue;
              if (errno == EAGAIN)
                break;
              drained = 1;
              break;
            }
          }

          if (drained) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
            if (fd == master)
              master = -1;
            else if (fd == out[0])
              out[0] = -1;
            else if (fd == err[0])
              err[0] = -1;
            if (--active_reads <= 0)
              child_done = 2;
          }
        }
      }
    }

    /* wait for eofs on pipes even if the child exited, because the monitor
     * might still be holding it */
    if (child_done == 1) {
      if (out[0] >= 0)
        drain_fd(out[0], conn, MSG_OUT);
      if (err[0] >= 0)
        drain_fd(err[0], conn, MSG_ERR);
      break;
    } else if (child_done == 2) {
      if (is_pty && master >= 0)
        drain_fd(master, conn, MSG_OUT);
      break;
    }
  }

  if (exit_code == EXIT_PENDING) {
    if (is_pty) {
      if (reap_child_timeout(child, 2000, &st) < 0) {
        kill(child, SIGKILL);
        waitpid(child, &st, 0);
      }
    } else {
      waitpid(child, &st, 0);
    }
    exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
  }
  goto session_end;

client_gone:
  /* Close the channel to the child before waiting: the command may only
   * notice that its client is gone from the EOF. */
  if (is_pty) {
    if (master >= 0) {
      close(master);
      master = -1;
    }
  } else if (in_fd >= 0) {
    close(in_fd);
    in_fd = in[1] = -1;
  }
  stop_child(child, -1, &st);
  exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;

session_end:
  free(pty_wbuf);
  if (sfd >= 0) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, sfd, NULL);
    close(sfd);
  }
  sigprocmask(SIG_UNBLOCK, &ss, NULL);
  close(epfd);
  if (master >= 0)
    close(master);
  if (in[1] >= 0)
    close(in[1]);
  if (out[0] >= 0)
    close(out[0]);
  if (err[0] >= 0)
    close(err[0]);
  send_exit(conn, exit_code == EXIT_PENDING ? 0 : exit_code);
}

void handle_session(int *conn, ds_req_t *r) {
  int c = *conn;
  handle_session_inner(c, r);
  close(c);
  *conn = -1;
}

/* A detached enter session keeps the container pty and a bounded scrollback.
 * The client is optional: losing it only swaps the socket out of the epoll
 * set, it never closes the master. */

struct ds_scroll {
  uint8_t *buf;
  size_t cap;
  size_t len;
  int truncated;
};

static void scroll_init(struct ds_scroll *s, size_t cap) {
  s->buf = malloc(cap);
  s->cap = s->buf ? cap : 0;
  s->len = 0;
  s->truncated = 0;
}

static void scroll_free(struct ds_scroll *s) {
  free(s->buf);
  s->buf = NULL;
  s->cap = s->len = 0;
}

static void scroll_append(struct ds_scroll *s, const void *data, size_t n) {
  if (!s->buf || n == 0)
    return;
  if (n >= s->cap) {
    memcpy(s->buf, (const uint8_t *)data + (n - s->cap), s->cap);
    s->len = s->cap;
    s->truncated = 1;
    return;
  }
  if (s->len + n > s->cap) {
    size_t drop = s->len + n - s->cap;
    memmove(s->buf, s->buf + drop, s->len - drop);
    s->len -= drop;
    s->truncated = 1;
  }
  memcpy(s->buf + s->len, data, n);
  s->len += n;
}

static int write_scroll(int conn, const struct ds_scroll *s) {
  if (s->truncated) {
    /* A tail that starts mid escape sequence leaves the new emulator in an
     * unknown state; reset first and say why the screen is partial. */
    const char *reset = "\033[0m\033[2J\033[H";
    const char *note = "[session scrollback truncated]\r\n";
    if (ds_send_frame(conn, MSG_OUT, reset, (uint32_t)strlen(reset)) < 0 ||
        ds_send_frame(conn, MSG_OUT, note, (uint32_t)strlen(note)) < 0)
      return -1;
  }
  for (size_t off = 0; off < s->len;) {
    uint32_t chunk = (uint32_t)(s->len - off);
    if (chunk > DS_IOBUF)
      chunk = DS_IOBUF;
    if (ds_send_frame(conn, MSG_OUT, s->buf + off, chunk) < 0)
      return -1;
    off += chunk;
  }
  return 0;
}

static int session_socket_name(const char *id, char *buf, size_t size) {
  int n = snprintf(buf, size, "%s%s", DS_SESSION_SOCK_PREFIX, id);
  return (n > 0 && (size_t)n < size) ? 0 : -1;
}

static int session_listen(const char *id) {
  char name[sizeof(DS_SESSION_SOCK_PREFIX) + DS_SESSION_ID_LEN];
  if (session_socket_name(id, name, sizeof(name)) < 0)
    return -1;

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un addr;
  socklen_t alen = ds_abstract_addr(&addr, name);
  if (bind(fd, (struct sockaddr *)&addr, alen) < 0 || listen(fd, 4) < 0) {
    close(fd);
    return -1;
  }
  (void)fcntl(fd, F_SETFL, O_NONBLOCK);
  return fd;
}

/* Returns 0 when the session has no listener to come back through: the
 * caller must end it instead of leaving an unreachable shell behind. */
static int enter_detach(int epfd, int *conn, int *suspended, int persistent) {
  if (*conn < 0)
    return 1;
  epoll_ctl(epfd, EPOLL_CTL_DEL, *conn, NULL);
  close(*conn);
  *conn = -1;
  *suspended = 0;
  return persistent;
}

static void append_wbuf(uint8_t **buf, size_t *len, size_t *cap,
                        const void *data, size_t n) {
  if (n == 0 || *len + n > PTY_WBUF_MAX)
    return;
  if (*len + n > *cap) {
    size_t ncap = *cap ? *cap : DS_IOBUF;
    while (ncap < *len + n)
      ncap *= 2;
    if (ncap > PTY_WBUF_MAX)
      ncap = PTY_WBUF_MAX;
    uint8_t *nb = realloc(*buf, ncap);
    if (!nb)
      return;
    *buf = nb;
    *cap = ncap;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
}

/* Read the first frame of a new connection. The keeper only accepts a client
 * on its abstract socket after the same peer check the daemon uses: the socket
 * has no filesystem permissions of its own. Returns 1 when the caller asked to
 * end the session, 0 when a client attached, -1 when accept itself failed. */
static int enter_accept(int epfd, int lsock, int *conn, int *suspended,
                        int master, pid_t child,
                        const struct ds_scroll *scroll) {
  int nfd = accept4(lsock, NULL, NULL, SOCK_CLOEXEC);
  if (nfd < 0)
    return -1;

  if (!ds_peer_authorized(nfd, "droidspaces")) {
    const char *msg = "permission denied\n";
    ds_send_frame(nfd, MSG_ERR, msg, (uint32_t)strlen(msg));
    send_exit(nfd, 1);
    close(nfd);
    return 0;
  }

  struct timeval tv = {2, 0};
  (void)setsockopt(nfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t type;
  uint32_t mlen;
  uint16_t wd[2];
  if (ds_recv_frame_hdr(nfd, &type, &mlen) < 0 ||
      (type != MSG_WINCH && type != MSG_SESSION_KILL) ||
      (type == MSG_WINCH && mlen != 4)) {
    const char *msg = "bad attach handshake\n";
    ds_send_frame(nfd, MSG_ERR, msg, (uint32_t)strlen(msg));
    send_exit(nfd, 1);
    close(nfd);
    return 0;
  }

  if (type == MSG_SESSION_KILL) {
    /* Kill is allowed while attached: stop has to close the sessions an app
     * still holds. Tell the current client first, then hand the request to
     * enter_end, which waits for the shell and answers this one. */
    drain_bytes(nfd, mlen);
    if (*conn >= 0) {
      send_exit(*conn, 0);
      epoll_ctl(epfd, EPOLL_CTL_DEL, *conn, NULL);
      close(*conn);
    }
    *conn = nfd;
    *suspended = 0;
    return 1;
  }

  if (ds_read_exact(nfd, wd, 4) < 0) {
    const char *msg = "bad attach handshake\n";
    ds_send_frame(nfd, MSG_ERR, msg, (uint32_t)strlen(msg));
    send_exit(nfd, 1);
    close(nfd);
    return 0;
  }
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  (void)setsockopt(nfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (*conn >= 0) {
    const char *msg = "session is already attached\n";
    ds_send_frame(nfd, MSG_ERR, msg, (uint32_t)strlen(msg));
    send_exit(nfd, 1);
    close(nfd);
    return 0;
  }

  struct winsize ws = {ntohs(wd[0]), ntohs(wd[1]), 0, 0};
  if (!ws.ws_row)
    ws.ws_row = 24;
  if (!ws.ws_col)
    ws.ws_col = 80;
  ioctl(master, TIOCSWINSZ, &ws);
  kill(child, SIGWINCH);

  *conn = nfd;
  *suspended = 0;
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
  ev.data.fd = nfd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, nfd, &ev) < 0) {
    close(nfd);
    *conn = -1;
    return -1;
  }

  if (write_scroll(nfd, scroll) < 0) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, nfd, NULL);
    close(nfd);
    *conn = -1;
  }
  return 0;
}

/* The command in req is enter, and its user argument follows it. Container
 * names may contain spaces, so they are copied as one argument, never
 * re-tokenized. The app may also name the session it is creating, so the id
 * its tab stores is the id of the keeper it can later kill. */
static void parse_enter_labels(ds_req_t *r, char *container, size_t csize,
                               char *user, size_t usize, char *session_id,
                               size_t session_size) {
  container[0] = '\0';
  safe_strncpy(user, "root", usize);
  session_id[0] = '\0';

  char **av = make_exec_argv(r);
  if (!av)
    return;

  const char *user_arg = NULL;
  int seen_enter = 0;
  optind = 0;
  int opt;
  while ((opt = getopt_long(r->argc + 1, av, "-" DS_OPTSTRING, long_options,
                            NULL)) != -1) {
    if (opt == 'n') {
      safe_strncpy(container, optarg, csize);
    } else if (opt == DS_OPT_SESSION_ID) {
      safe_strncpy(session_id, optarg, session_size);
    } else if (opt == 1) {
      if (!seen_enter && strcmp(optarg, "enter") == 0)
        seen_enter = 1;
      else if (seen_enter && !user_arg)
        user_arg = optarg;
    }
  }
  optind = 0;

  if (user_arg)
    safe_strncpy(user, user_arg, usize);
  free(av);
}

static void handle_enter_inner(int *connp, ds_req_t *r) {
  int master = -1;
  pid_t child = -1;
  if (spawn_pty_child(r, &master, &child) < 0) {
    const char *msg = "daemon: pty allocation failed\n";
    ds_send_frame(*connp, MSG_ERR, msg, (uint32_t)strlen(msg));
    send_exit(*connp, 1);
    return;
  }

  char id[DS_SESSION_ID_LEN + 1] = "";
  char requested_id[DS_SESSION_ID_LEN + 1] = "";
  char container[256];
  char user[256];
  parse_enter_labels(r, container, sizeof(container), user, sizeof(user),
                     requested_id, sizeof(requested_id));

  int lsock = -1;
  if (ds_session_id_valid(requested_id)) {
    /* The app already put this id in its tab. Using a different one would leave
     * the tab unable to end the session it opened. */
    safe_strncpy(id, requested_id, sizeof(id));
    lsock = session_listen(id);
    if (lsock < 0) {
      const char *msg = "daemon: session id is already in use\n";
      ds_send_frame(*connp, MSG_ERR, msg, (uint32_t)strlen(msg));
      send_exit(*connp, 1);
      int st;
      stop_child(child, master, &st);
      return;
    }
  } else if (generate_uuid(id, sizeof(id)) == 0) {
    lsock = session_listen(id);
    if (lsock < 0 && generate_uuid(id, sizeof(id)) == 0)
      lsock = session_listen(id);
  }

  int registered = 0;
  if (lsock >= 0) {
    struct ds_session_record rec;
    memset(&rec, 0, sizeof(rec));
    safe_strncpy(rec.id, id, sizeof(rec.id));
    safe_strncpy(rec.container, container, sizeof(rec.container));
    safe_strncpy(rec.user, user, sizeof(rec.user));
    rec.pid = getpid();
    rec.start = proc_starttime(getpid());
    rec.created = (long long)time(NULL);
    registered = (ds_session_record_write(&rec) == 0);
    if (!registered)
      ds_warn("session: cannot write the record for %s", id);
    else
      ds_log("Terminal session %s started for '%s'", id,
             container[0] ? container : "(unnamed)");
  } else {
    ds_warn("session: cannot create the attach socket; this terminal will "
            "not survive a disconnect");
  }

  /* Without a listener and a record nobody can find this session again. End
   * it with the client rather than leave a shell with no way back. */
  int persistent = (lsock >= 0 && registered);
  if (!persistent && lsock >= 0) {
    close(lsock);
    lsock = -1;
  }

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    if (lsock >= 0)
      close(lsock);
    int st;
    stop_child(child, master, &st);
    send_exit(*connp, 1);
    if (registered)
      ds_session_record_unlink(id);
    return;
  }

  sigset_t ss;
  sigemptyset(&ss);
  sigaddset(&ss, SIGCHLD);
  sigprocmask(SIG_BLOCK, &ss, NULL);
  int sfd = signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC);

  struct epoll_event ev, events[10];
  int fl = fcntl(master, F_GETFL);
  fcntl(master, F_SETFL, fl | O_NONBLOCK);
  ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
  ev.data.fd = master;
  epoll_ctl(epfd, EPOLL_CTL_ADD, master, &ev);

  if (sfd >= 0) {
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);
  }
  if (*connp >= 0) {
    ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.fd = *connp;
    epoll_ctl(epfd, EPOLL_CTL_ADD, *connp, &ev);
  }
  if (lsock >= 0) {
    ev.events = EPOLLIN;
    ev.data.fd = lsock;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lsock, &ev);
  }

  struct ds_scroll scroll;
  scroll_init(&scroll, DS_SESSION_SCROLLBACK);

  uint8_t *wbuf = NULL;
  size_t wlen = 0, wcap = 0;
  int conn_suspended = 0;
  int child_done = 0;
  int kill_requested = 0;
  int exit_code = EXIT_PENDING;
  char buf[DS_IOBUF];

  for (;;) {
    int nfds = epoll_wait(epfd, events, 10, -1);
    if (nfds < 0 && errno != EINTR)
      break;

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;

      if (sfd >= 0 && fd == sfd) {
        struct signalfd_siginfo si;
        while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
          if (!child_done) {
            int st;
            if (waitpid(child, &st, WNOHANG) == child) {
              exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
              child_done = 1;
            }
          }
        }
      } else if (fd == *connp) {
        if (events[i].events & EPOLLIN) {
          uint8_t type;
          uint32_t mlen;
          if (ds_recv_frame_hdr(*connp, &type, &mlen) < 0) {
            if (!enter_detach(epfd, connp, &conn_suspended, persistent))
              goto enter_end;
            continue;
          }
          if (type == MSG_OUT && mlen <= (uint32_t)sizeof(buf)) {
            if (ds_read_exact(*connp, buf, mlen) < 0) {
              if (!enter_detach(epfd, connp, &conn_suspended, persistent))
                goto enter_end;
              continue;
            }
            if (mlen == 0)
              continue;
            size_t written = 0;
            if (wlen == 0 && master >= 0) {
              ssize_t w = write(master, buf, mlen);
              if (w > 0)
                written = (size_t)w;
            }
            if (written < mlen)
              append_wbuf(&wbuf, &wlen, &wcap, buf + written, mlen - written);
            if (wlen > 0 && master >= 0) {
              ev.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR;
              ev.data.fd = master;
              epoll_ctl(epfd, EPOLL_CTL_MOD, master, &ev);
            }
            if (!conn_suspended && wlen >= PTY_WBUF_HIGH) {
              ev.events = EPOLLHUP | EPOLLERR;
              ev.data.fd = *connp;
              epoll_ctl(epfd, EPOLL_CTL_MOD, *connp, &ev);
              conn_suspended = 1;
            }
          } else if (type == MSG_WINCH && mlen == 4) {
            uint16_t wd[2];
            if (ds_read_exact(*connp, wd, 4) == 0 && master >= 0) {
              struct winsize nws = {ntohs(wd[0]), ntohs(wd[1]), 0, 0};
              ioctl(master, TIOCSWINSZ, &nws);
              kill(child, SIGWINCH);
            }
          } else if (type == MSG_SESSION_KILL) {
            drain_bytes(*connp, mlen);
            kill_requested = 1;
            goto enter_end;
          } else {
            drain_bytes(*connp, mlen);
          }
        }
        if (*connp >= 0 && (events[i].events & (EPOLLHUP | EPOLLERR)) &&
            !enter_detach(epfd, connp, &conn_suspended, persistent))
          goto enter_end;
      } else if (fd == lsock) {
        if (enter_accept(epfd, lsock, connp, &conn_suspended, master, child,
                         &scroll) == 1) {
          kill_requested = 1;
          goto enter_end;
        }
      } else if (fd == master) {
        if (events[i].events & EPOLLOUT) {
          while (wlen > 0) {
            ssize_t w = write(master, wbuf, wlen);
            if (w > 0) {
              wlen -= (size_t)w;
              if (wlen > 0)
                memmove(wbuf, wbuf + w, wlen);
            } else if (w < 0 && errno == EAGAIN) {
              break;
            } else {
              wlen = 0;
              break;
            }
          }
          if (wlen == 0) {
            ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
            ev.data.fd = master;
            epoll_ctl(epfd, EPOLL_CTL_MOD, master, &ev);
          }
          if (conn_suspended && wlen < PTY_WBUF_LOW) {
            ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
            ev.data.fd = *connp;
            epoll_ctl(epfd, EPOLL_CTL_MOD, *connp, &ev);
            conn_suspended = 0;
          }
        }
        if (events[i].events & EPOLLIN) {
          for (;;) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
              scroll_append(&scroll, buf, (size_t)n);
              if (*connp >= 0 &&
                  ds_send_frame(*connp, MSG_OUT, buf, (uint32_t)n) < 0 &&
                  !enter_detach(epfd, connp, &conn_suspended, persistent))
                goto enter_end;
            } else {
              if (errno == EINTR)
                continue;
              break;
            }
          }
        }
        if (master >= 0 && (events[i].events & (EPOLLHUP | EPOLLERR))) {
          close(master);
          master = -1;
          goto enter_end;
        }
      }
    }

    if (child_done) {
      /* The shell is gone; the last lines are still queued on the master. */
      if (master >= 0) {
        for (;;) {
          ssize_t n = read(master, buf, sizeof(buf));
          if (n <= 0) {
            if (n < 0 && errno == EINTR)
              continue;
            break;
          }
          scroll_append(&scroll, buf, (size_t)n);
          if (*connp >= 0 &&
              ds_send_frame(*connp, MSG_OUT, buf, (uint32_t)n) < 0 &&
              !enter_detach(epfd, connp, &conn_suspended, persistent))
            goto enter_end;
        }
        close(master);
        master = -1;
      }
      break;
    }
  }

enter_end:
  if (master >= 0) {
    close(master);
    master = -1;
  }

  if (exit_code == EXIT_PENDING) {
    int st = 0;
    if (reap_child_timeout(child, 1000, &st) < 0) {
      kill(child, SIGKILL);
      if (waitpid(child, &st, 0) < 0)
        st = 0;
    }
    exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
  }
  if (kill_requested)
    exit_code = 0;

  if (*connp >= 0)
    send_exit(*connp, exit_code);

  if (lsock >= 0)
    close(lsock);
  if (registered)
    ds_session_record_unlink(id);
  scroll_free(&scroll);
  free(wbuf);
  if (sfd >= 0) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, sfd, NULL);
    close(sfd);
  }
  sigprocmask(SIG_UNBLOCK, &ss, NULL);
  close(epfd);
}

void handle_session_enter(int *conn, ds_req_t *r) {
  handle_enter_inner(conn, r);
  if (*conn >= 0) {
    close(*conn);
    *conn = -1;
  }
}
