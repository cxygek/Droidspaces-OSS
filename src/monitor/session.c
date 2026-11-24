/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"
#include <spawn.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

extern char **environ;

/* Terminal sessions, hosted by the monitor.
 *
 * The monitor outlives everything the app does and lives exactly as long as
 * the container, so it owns the keepers: one child per session, each holding
 * the host pty of an enter, its scrollback and an abstract socket. A kill of
 * the app then costs the user nothing but a detach.
 *
 * Control plane: droidspaces/<name>/session, answered here in the monitor's
 * own loop. Data plane: the keeper's own socket, one per session. */

static int g_ctl = -1;
static char g_name[256];
static pid_t g_keepers[DS_SESSION_MAX];

void ds_session_host_start(const char *container_name) {
  safe_strncpy(g_name, container_name, sizeof(g_name));

  char sock[128];
  ds_monitor_sock_name(g_name, DS_SESSION_CTL_LEAF, sock, sizeof(sock));
  struct sockaddr_un addr;
  socklen_t alen = ds_abstract_addr(&addr, sock);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return;
  if (bind(fd, (struct sockaddr *)&addr, alen) < 0 || listen(fd, 4) < 0) {
    /* A squatter or our own previous cycle. Sessions still work for one that
     * already exists; new ones fall back to a plain enter, which is what a
     * build without this does. */
    write_monitor_debug_log(g_name,
                            "Session socket @%s busy (%s); new enters will "
                            "not outlive their client",
                            sock, strerror(errno));
    close(fd);
    return;
  }
  g_ctl = fd;
}

int ds_session_host_fd(void) { return g_ctl; }

/* Reap keepers that ended on their own. Their pids are tracked, never reaped
 * with waitpid(-1): the status of the boot's intermediate is read by pid and
 * must not be taken here. */
void ds_session_host_reap(void) {
  for (int i = 0; i < DS_SESSION_MAX; i++) {
    if (g_keepers[i] <= 0)
      continue;
    pid_t r = waitpid(g_keepers[i], NULL, WNOHANG);
    /* ECHILD means somebody else already collected it, so the slot is free
     * either way; leaving it set would cost this container a session. */
    if (r == g_keepers[i] || (r < 0 && errno == ECHILD))
      g_keepers[i] = 0;
  }
}

/* Every keeper of this boot, gone. Called before a reboot cycle and before the
 * container's mounts are torn down: a live keeper still holds the pty of a
 * shell inside the container, and an unmount behind it fails. */
void ds_session_host_end_all(void) {
  int live = 0;
  for (int i = 0; i < DS_SESSION_MAX; i++) {
    if (g_keepers[i] <= 0)
      continue;
    kill(g_keepers[i], SIGTERM);
    live++;
  }

  /* The keeper's death closes the pty, which is what ends the shell behind it
   * (exec.c handles that hangup). Give it a moment before the stick. */
  for (int waited = 0; live > 0 && waited < 2000; waited += 50) {
    ds_session_host_reap();
    live = 0;
    for (int i = 0; i < DS_SESSION_MAX; i++)
      if (g_keepers[i] > 0)
        live++;
    if (live)
      usleep(50000);
  }

  for (int i = 0; i < DS_SESSION_MAX; i++) {
    if (g_keepers[i] <= 0)
      continue;
    kill(g_keepers[i], SIGKILL);
    (void)waitpid(g_keepers[i], NULL, 0);
    g_keepers[i] = 0;
  }
}

static int keeper_slot(void) {
  for (int i = 0; i < DS_SESSION_MAX; i++)
    if (g_keepers[i] <= 0)
      return i;
  return -1;
}

static void answer(int fd, const struct ds_session_answer *ans) {
  ds_send_frame(fd, ans->ok ? DS_SESS_OK : DS_SESS_FAIL, ans, sizeof(*ans));
}

static void refuse(int fd, const char *why) {
  struct ds_session_answer ans;
  memset(&ans, 0, sizeof(ans));
  safe_strncpy(ans.msg, why, sizeof(ans.msg));
  answer(fd, &ans);
}

/* One enter arrives: fork a keeper for it and hand the client the id it
 * published. The keeper binds its own socket before it answers, so the client
 * that reads the id can connect to it at once. */
void ds_session_host_accept(void) {
  int c = accept4(g_ctl, NULL, NULL, SOCK_CLOEXEC);
  if (c < 0)
    return;

  /* A keeper that died since the last heartbeat must not cost this request a
   * slot. */
  ds_session_host_reap();

  /* Bounded so a client that connects and says nothing cannot wedge the loop
   * that also drains the console. Root only, so this is hygiene, not a trust
   * boundary. */
  struct timeval tv = {2, 0};
  (void)setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  /* Root, from this pid namespace: a container sharing the host network
   * namespace can reach this name, and it must not get a keeper. */
  if (!ds_peer_authorized(c, NULL)) {
    refuse(c, "permission denied");
    close(c);
    return;
  }

  struct ds_session_open req;
  uint8_t type = 0;
  uint32_t len = 0;
  if (ds_recv_frame_hdr(c, &type, &len) < 0 || type != DS_SESS_OPEN ||
      len != sizeof(req) || ds_read_exact(c, &req, sizeof(req)) < 0) {
    close(c);
    return;
  }
  /* The fields arrive as bytes, so none of them is trusted to be terminated:
   * everything below indexes them as C strings. */
  req.container[sizeof(req.container) - 1] = '\0';
  req.user[sizeof(req.user) - 1] = '\0';
  req.term[sizeof(req.term) - 1] = '\0';
  req.id[sizeof(req.id) - 1] = '\0';
  if (!same_container_name(req.container, g_name)) {
    refuse(c, "this monitor hosts another container");
    close(c);
    return;
  }
  /* From here the name that counts is the one this monitor booted: the client
   * may have spelled it with spaces where the config has dashes, and the
   * keeper's `enter` has to resolve it again. */
  safe_strncpy(req.container, g_name, sizeof(req.container));
  if (!req.user[0])
    safe_strncpy(req.user, "root", sizeof(req.user));

  int slot = keeper_slot();
  if (slot < 0) {
    refuse(c, "too many terminal sessions");
    close(c);
    return;
  }

  char exe[PATH_MAX];
  safe_strncpy(exe, ds_self_path(), sizeof(exe));
  if (!exe[0]) {
    refuse(c, "cannot find the droidspaces binary");
    close(c);
    return;
  }

  /* The monitor is multi-threaded, so the keeper is spawned rather than
   * forked: between fork and exec only async-signal-safe calls are allowed,
   * and posix_spawn is the one that gets that right. */
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) < 0) {
    refuse(c, "socketpair failed");
    close(c);
    return;
  }
  /* The keeper's end must land on DS_SESSION_CTL_FD with that flag cleared,
   * and dup2() from a number to itself does not clear close-on-exec. Move it
   * out of the way first, so the file action below is always a real dup. */
  if (sp[1] == DS_SESSION_CTL_FD) {
    int alt = fcntl(sp[1], F_DUPFD, DS_SESSION_CTL_FD + 1);
    if (alt < 0) {
      refuse(c, "cannot prepare the session control fd");
      close(sp[0]);
      close(sp[1]);
      close(c);
      return;
    }
    close(sp[1]);
    sp[1] = alt;
  }

  pid_t pid = 0;
  char *const kav[] = {exe, "session", "keeper", NULL};
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, sp[1], DS_SESSION_CTL_FD);
  if (sp[1] != DS_SESSION_CTL_FD)
    posix_spawn_file_actions_addclose(&fa, sp[1]);
  int rc = posix_spawn(&pid, exe, &fa, NULL, kav, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(sp[1]);

  if (rc != 0) {
    refuse(c, strerror(rc));
    close(sp[0]);
    close(c);
    return;
  }
  g_keepers[slot] = pid;

  /* The keeper's answer is the readiness of its socket, so the client never
   * races the bind. Bounded: a keeper that hangs must not hold the monitor's
   * loop, which also drives the console and the network handshakes. */
  struct ds_session_answer ans;
  struct timeval ktv = {3, 0};
  (void)setsockopt(sp[0], SOL_SOCKET, SO_RCVTIMEO, &ktv, sizeof(ktv));
  int ok = ds_send_frame(sp[0], DS_SESS_OPEN, &req, sizeof(req)) == 0 &&
           ds_recv_frame_hdr(sp[0], &type, &len) == 0 &&
           (type == DS_SESS_OK || type == DS_SESS_FAIL) && len == sizeof(ans) &&
           ds_read_exact(sp[0], &ans, sizeof(ans)) == 0;
  close(sp[0]);

  if (!ok) {
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    g_keepers[slot] = 0;
    refuse(c, "the session keeper failed to start");
    close(c);
    return;
  }

  answer(c, &ans);
  close(c);
  ds_session_host_reap();
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

static int keeper_socket_name(const char *id, char *buf, size_t size) {
  if (!ds_session_id_valid(id))
    return -1;
  int n = snprintf(buf, size, "%s%s", DS_SESSION_SOCK_PREFIX, id);
  return (n > 0 && (size_t)n < size) ? 0 : -1;
}

static int keeper_listen(const char *id) {
  char name[sizeof(DS_SESSION_SOCK_PREFIX) + DS_SESSION_ID_LEN];
  if (keeper_socket_name(id, name, sizeof(name)) < 0)
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

static void keeper_detach(int epfd, int *conn, int *suspended) {
  if (*conn < 0)
    return;
  epoll_ctl(epfd, EPOLL_CTL_DEL, *conn, NULL);
  close(*conn);
  *conn = -1;
  *suspended = 0;
}

/* The frame a client reads its exit code out of, in the same layout the
 * daemon's sessions use. */
static void keeper_send_exit(int fd, int code) {
  uint32_t c = htonl((uint32_t)code);
  ds_send_frame(fd, MSG_EXIT, &c, sizeof(c));
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
static int keeper_accept(int epfd, int lsock, int *conn, int *suspended,
                         int master, pid_t child,
                         const struct ds_scroll *scroll) {
  int nfd = accept4(lsock, NULL, NULL, SOCK_CLOEXEC);
  if (nfd < 0)
    return -1;

  if (!ds_peer_authorized(nfd, "droidspaces")) {
    const char *msg = "permission denied\n";
    ds_send_frame(nfd, MSG_ERR, msg, (uint32_t)strlen(msg));
    keeper_send_exit(nfd, 1);
    close(nfd);
    return 0;
  }

  struct timeval tv = {2, 0};
  (void)setsockopt(nfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t type = 0;
  uint32_t mlen = 0;
  uint16_t wd[2];
  if (ds_recv_frame_hdr(nfd, &type, &mlen) < 0 ||
      (type != MSG_WINCH && type != MSG_SESSION_KILL) ||
      (type == MSG_WINCH && mlen != 4)) {
    const char *msg = "bad attach handshake\n";
    ds_send_frame(nfd, MSG_ERR, msg, (uint32_t)strlen(msg));
    keeper_send_exit(nfd, 1);
    close(nfd);
    return 0;
  }

  if (type == MSG_SESSION_KILL) {
    /* Kill is allowed while attached: stop has to close the sessions an app
     * still holds. Tell the current client first, then hand the request to
     * the caller, which ends the shell and answers this one. */
    ds_drain_frame(nfd, mlen);
    if (*conn >= 0) {
      keeper_send_exit(*conn, 0);
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
    keeper_send_exit(nfd, 1);
    close(nfd);
    return 0;
  }
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  (void)setsockopt(nfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (*conn >= 0) {
    const char *msg = "session is already attached\n";
    ds_send_frame(nfd, MSG_ERR, msg, (uint32_t)strlen(msg));
    keeper_send_exit(nfd, 1);
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

/* Run the container's enter command behind this session's pty. The command
 * rediscovers the container from its name, exactly as the client would have:
 * the keeper holds no config of its own. The two names are char * and not
 * const only because execv() takes char *const argv[], which cannot carry a
 * const pointer without a cast this tree forbids. */
static int keeper_spawn_shell(int *master_out, pid_t *child_out,
                              char *container, char *user, const char *term,
                              uint16_t rows, uint16_t cols) {
  int master = -1, slave = -1;
  if (ds_openpty("/dev/ptmx", &master, &slave, NULL) < 0)
    return -1;

  struct winsize ws = {rows ? rows : 24, cols ? cols : 80, 0, 0};
  (void)ioctl(master, TIOCSWINSZ, &ws);
  (void)fcntl(master, F_SETFD, FD_CLOEXEC);

  char exe[PATH_MAX];
  safe_strncpy(exe, ds_self_path(), sizeof(exe));
  if (!exe[0]) {
    close(master);
    close(slave);
    return -1;
  }

  char *av[] = {exe, "-n", container, "enter", user, NULL};

  pid_t child = fork();
  if (child < 0) {
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
    /* The command must do the work itself, not hand it back to a daemon that
     * may not even be running, and not ask the monitor for a session of its
     * own: this pty is that session. TERM is the client's terminal, so the
     * shell speaks what actually attached rather than what started the
     * container. */
    setenv("DS_NO_PROXY", "1", 1);
    setenv(DS_ENV_NO_SESSION, "1", 1);
    if (term && term[0])
      setenv("TERM", term, 1);
    execv(exe, av);
    _exit(127);
  }

  close(slave);
  *master_out = master;
  *child_out = child;
  return 0;
}

/* ds_session_keeper_run - hold one terminal session for as long as its shell
 * runs. Never returns: the monitor's forked copy of it exits here. */
void ds_session_keeper_run(void) {
  setsid();
  prctl(PR_SET_NAME, DS_SESSION_PROC, 0, 0, 0);

  /* A keeper inherits the monitor's ignored signals, and it must be killable:
   * its death is what closes the pty and ends the shell behind it. It also
   * inherits the monitor's signal mask, which the heartbeat loop keeps SIGCHLD
   * blocked in, and the shell it starts must not. */
  signal(SIGTERM, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGHUP, SIG_DFL);
  signal(SIGPIPE, SIG_IGN);
  sigset_t empty;
  sigemptyset(&empty);
  sigprocmask(SIG_SETMASK, &empty, NULL);

  int ctl = DS_SESSION_CTL_FD;
  struct ds_session_open req;
  uint8_t type = 0;
  uint32_t len = 0;
  if (ds_recv_frame_hdr(ctl, &type, &len) < 0 || type != DS_SESS_OPEN ||
      len != sizeof(req) || ds_read_exact(ctl, &req, sizeof(req)) < 0)
    _exit(1);

  struct ds_session_answer ans;
  memset(&ans, 0, sizeof(ans));

  char id[DS_SESSION_ID_LEN + 1] = "";
  int lsock = -1;
  if (ds_session_id_valid(req.id)) {
    /* The app already put this id in its tab. Using a different one would
     * leave the tab unable to end the session it opened. */
    safe_strncpy(id, req.id, sizeof(id));
    lsock = keeper_listen(id);
    if (lsock < 0) {
      safe_strncpy(ans.msg, "the session id is already in use",
                   sizeof(ans.msg));
      ds_send_frame(ctl, DS_SESS_FAIL, &ans, sizeof(ans));
      _exit(1);
    }
  } else {
    for (int i = 0; i < 2 && lsock < 0; i++) {
      if (generate_uuid(id, sizeof(id)) < 0)
        break;
      lsock = keeper_listen(id);
    }
    if (lsock < 0) {
      safe_strncpy(ans.msg, "cannot create an attach socket", sizeof(ans.msg));
      ds_send_frame(ctl, DS_SESS_FAIL, &ans, sizeof(ans));
      _exit(1);
    }
  }

  int master = -1;
  pid_t shell = -1;
  if (keeper_spawn_shell(&master, &shell, req.container, req.user, req.term,
                         req.rows, req.cols) < 0) {
    safe_strncpy(ans.msg, "cannot allocate a pty for the session",
                 sizeof(ans.msg));
    ds_send_frame(ctl, DS_SESS_FAIL, &ans, sizeof(ans));
    close(lsock);
    _exit(1);
  }

  struct ds_session_record rec;
  memset(&rec, 0, sizeof(rec));
  safe_strncpy(rec.id, id, sizeof(rec.id));
  safe_strncpy(rec.container, req.container, sizeof(rec.container));
  safe_strncpy(rec.user, req.user, sizeof(rec.user));
  rec.pid = getpid();
  rec.start = proc_starttime(getpid());
  rec.created = (long long)time(NULL);
  int registered = (ds_session_record_write(&rec) == 0);
  if (!registered)
    write_monitor_debug_log(req.container,
                            "Cannot write the record for session %s", id);

  /* Everything the client needs is in place, so it may attach now. The
   * control fd goes here: from now on the session socket is the only way in. */
  ans.ok = 1;
  safe_strncpy(ans.id, id, sizeof(ans.id));
  ds_send_frame(ctl, DS_SESS_OK, &ans, sizeof(ans));
  close(ctl);

  write_monitor_debug_log(req.container, "Terminal session %s started for %s",
                          id, req.user);

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  sigset_t ss;
  sigemptyset(&ss);
  sigaddset(&ss, SIGCHLD);
  sigprocmask(SIG_BLOCK, &ss, NULL);
  int sfd = epfd >= 0 ? signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC) : -1;

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
  ev.events = EPOLLIN;
  ev.data.fd = lsock;
  epoll_ctl(epfd, EPOLL_CTL_ADD, lsock, &ev);

  struct ds_scroll scroll;
  scroll_init(&scroll, DS_SESSION_SCROLLBACK);

  uint8_t *wbuf = NULL;
  size_t wlen = 0, wcap = 0;
  int conn = -1;
  int conn_suspended = 0;
  int child_done = 0;
  int kill_requested = 0;
  int exit_code = EXIT_PENDING;
  char buf[DS_IOBUF];

  if (epfd < 0) {
    child_done = 1;
  }

  while (!child_done) {
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
            if (waitpid(shell, &st, WNOHANG) == shell) {
              exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
              child_done = 1;
            }
          }
        }
      } else if (fd == conn) {
        if (events[i].events & EPOLLIN) {
          uint8_t mtype = 0;
          uint32_t mlen = 0;
          if (ds_recv_frame_hdr(conn, &mtype, &mlen) < 0) {
            keeper_detach(epfd, &conn, &conn_suspended);
            continue;
          }
          if (mtype == MSG_OUT && mlen <= (uint32_t)sizeof(buf)) {
            if (ds_read_exact(conn, buf, mlen) < 0) {
              keeper_detach(epfd, &conn, &conn_suspended);
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
              ev.data.fd = conn;
              epoll_ctl(epfd, EPOLL_CTL_MOD, conn, &ev);
              conn_suspended = 1;
            }
          } else if (mtype == MSG_WINCH && mlen == 4) {
            uint16_t wd[2];
            if (ds_read_exact(conn, wd, 4) == 0 && master >= 0) {
              struct winsize nws = {ntohs(wd[0]), ntohs(wd[1]), 0, 0};
              ioctl(master, TIOCSWINSZ, &nws);
              kill(shell, SIGWINCH);
            }
          } else if (mtype == MSG_SESSION_KILL) {
            /* stop, or the app closing its tab: the shell and its jobs go
             * with the session, never before it answers. */
            ds_drain_frame(conn, mlen);
            kill_requested = 1;
            child_done = 1;
            break;
          } else {
            ds_drain_frame(conn, mlen);
          }
        }
        if (conn >= 0 && (events[i].events & (EPOLLHUP | EPOLLERR)))
          keeper_detach(epfd, &conn, &conn_suspended);
      } else if (fd == lsock) {
        if (keeper_accept(epfd, lsock, &conn, &conn_suspended, master, shell,
                          &scroll) == 1) {
          kill_requested = 1;
          child_done = 1;
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
          if (conn_suspended && conn >= 0 && wlen < PTY_WBUF_LOW) {
            ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
            ev.data.fd = conn;
            epoll_ctl(epfd, EPOLL_CTL_MOD, conn, &ev);
            conn_suspended = 0;
          }
        }
        if (events[i].events & EPOLLIN) {
          for (;;) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
              scroll_append(&scroll, buf, (size_t)n);
              if (conn >= 0 &&
                  ds_send_frame(conn, MSG_OUT, buf, (uint32_t)n) < 0)
                keeper_detach(epfd, &conn, &conn_suspended);
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
          child_done = 1;
        }
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
        if (conn >= 0)
          ds_send_frame(conn, MSG_OUT, buf, (uint32_t)n);
      }
      close(master);
      master = -1;
    }
  }

  if (exit_code == EXIT_PENDING) {
    int st = 0;
    if (ds_reap_child(shell, 1000, &st) < 0) {
      kill(shell, SIGKILL);
      if (waitpid(shell, &st, 0) < 0)
        st = 0;
    }
    exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
  }
  /* A session ended on request is not a command that failed. */
  if (kill_requested)
    exit_code = 0;

  if (conn >= 0) {
    keeper_send_exit(conn, exit_code);
    close(conn);
  }

  if (sfd >= 0)
    close(sfd);
  close(epfd);
  close(lsock);
  if (registered)
    ds_session_record_unlink(id);
  scroll_free(&scroll);
  free(wbuf);

  write_monitor_debug_log(req.container, "Terminal session %s ended", id);
  _exit(exit_code);
}
