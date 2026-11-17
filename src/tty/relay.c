/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>

/* The terminal relay both the daemon client and session attach use. The
 * socket already speaks the frame protocol: MSG_OUT/MSG_ERR to write, MSG_EXIT
 * to finish, MSG_OUT from stdin to send. */
int ds_terminal_relay(int socket, int interactive, int relay_stdin) {
  int sock = socket;
  int has_tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
  struct termios orig;
  int raw_tty_active = 0;
  int winch_sfd = -1;

  signal(SIGPIPE, SIG_IGN);

  if (interactive && has_tty && tcgetattr(STDIN_FILENO, &orig) == 0) {
    raw_tty_active = 1;
    struct termios raw = orig;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    /* signalfd for SIGWINCH: no handler, no 200ms timeout needed */
    sigset_t ws;
    sigemptyset(&ws);
    sigaddset(&ws, SIGWINCH);
    sigprocmask(SIG_BLOCK, &ws, NULL);
    winch_sfd = signalfd(-1, &ws, SFD_NONBLOCK | SFD_CLOEXEC);
  }

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event ev, events[4];

  /* A regular file on stdin cannot be polled (EPERM), so it is pumped one
   * chunk per loop turn instead. */
  int stdin_file = 0;
  /* Set while stdin has data but the socket is too full to take a frame
   * without blocking: a blocked client cannot drain the daemon's output, and
   * with both directions full the two would deadlock. Wake on EPOLLOUT. */
  int wait_out = 0;
  if (raw_tty_active || relay_stdin) {
    ev.events = EPOLLIN;
    ev.data.fd = STDIN_FILENO;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) < 0) {
      if (relay_stdin && errno == EPERM)
        stdin_file = 1;
      else if (relay_stdin)
        ds_send_frame(sock, MSG_OUT, NULL, 0); /* no stdin at all: EOF now */
    }
  }
  if (winch_sfd >= 0) {
    ev.events = EPOLLIN;
    ev.data.fd = winch_sfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, winch_sfd, &ev);
  }
  ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
  ev.data.fd = sock;
  epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);

  int exit_code = 0;
  char buf[DS_IOBUF];

  for (;;) {
    int nfds = epoll_wait(epfd, events, 4, (stdin_file && !wait_out) ? 0 : -1);
    if (nfds < 0 && errno != EINTR)
      break;

    int done = 0, stdin_ready = stdin_file;
    for (int i = 0; i < nfds && !done; i++) {
      int fd = events[i].data.fd;

      if (winch_sfd >= 0 && fd == winch_sfd) {
        struct signalfd_siginfo si;
        while (read(winch_sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
          struct winsize nws = {24, 80, 0, 0};
          ioctl(STDIN_FILENO, TIOCGWINSZ, &nws);
          uint16_t wd2[2] = {htons(nws.ws_row), htons(nws.ws_col)};
          ds_send_frame(sock, MSG_WINCH, wd2, 4);
        }
      } else if (fd == STDIN_FILENO) {
        stdin_ready = 1;
      } else if (fd == sock) {
        if (wait_out && (events[i].events & EPOLLOUT)) {
          wait_out = 0;
          ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
          ev.data.fd = sock;
          epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev);
          /* A file is always readable; a pipe may be empty, so let its own
           * EPOLLIN drive the next read instead of blocking in read(). */
          if (stdin_file) {
            stdin_ready = 1;
          } else {
            ev.events = EPOLLIN;
            ev.data.fd = STDIN_FILENO;
            epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, &ev);
          }
        }
        if (events[i].events & EPOLLIN) {
          /* drain all available frames without a redundant poll() probe */
          for (;;) {
            uint8_t type;
            uint32_t mlen;
            if (ds_recv_frame_hdr(sock, &type, &mlen) < 0) {
              done = 1;
              break;
            }

            if (type == MSG_EXIT) {
              uint32_t nc = 0;
              if (mlen >= 4)
                ds_read_exact(sock, &nc, 4);
              exit_code = (int)ntohl(nc);
              done = 1;
              break;
            }

            FILE *dest = (type == MSG_ERR) ? stderr : stdout;
            uint32_t rem = mlen;
            while (rem) {
              uint32_t c =
                  (rem < (uint32_t)sizeof(buf)) ? rem : (uint32_t)sizeof(buf);
              if (ds_read_exact(sock, buf, c) < 0) {
                done = 1;
                break;
              }
              fwrite(buf, 1, c, dest);
              rem -= c;
            }
            fflush(dest);
            if (done)
              break;

            /* check for more data without blocking */
            struct pollfd pfd = {sock, POLLIN, 0};
            if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
              break;
          }
        }

        if (!done && (events[i].events & (EPOLLHUP | EPOLLERR))) {
          done = 1;
          break;
        }
      }
    }

    /* A unix socket reports POLLOUT only with most of its send buffer free,
     * far more than one frame, so a write after a positive poll never blocks.
     */
    struct pollfd pw = {sock, POLLOUT, 0};
    if (!done && stdin_ready &&
        (poll(&pw, 1, 0) <= 0 || !(pw.revents & POLLOUT))) {
      wait_out = 1;
      stdin_ready = 0;
      ev.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR;
      ev.data.fd = sock;
      epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &ev);
      /* Level-triggered: a readable stdin would wake us every turn. */
      if (!stdin_file)
        epoll_ctl(epfd, EPOLL_CTL_DEL, STDIN_FILENO, NULL);
    }
    if (!done && stdin_ready) {
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      int stop = 0;
      if (n > 0) {
        /* A failed send means the daemon stopped reading, normally because
         * the command is gone. Its exit frame is still queued for us, so
         * stop relaying and let the socket branch collect it. */
        stop = ds_send_frame(sock, MSG_OUT, buf, (uint32_t)n) < 0;
      } else if (raw_tty_active) {
        done = 1;
      } else {
        /* EOF: tell the daemon, then keep going for output and the exit code.
         */
        ds_send_frame(sock, MSG_OUT, NULL, 0);
        stop = 1;
      }
      if (stop) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, STDIN_FILENO, NULL);
        stdin_file = 0;
      }
    }
    if (done)
      break;
  }

  if (raw_tty_active) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
    if (winch_sfd >= 0) {
      sigset_t ws;
      sigemptyset(&ws);
      sigaddset(&ws, SIGWINCH);
      sigprocmask(SIG_UNBLOCK, &ws, NULL);
      close(winch_sfd);
    }
  }
  close(epfd);
  close(sock);
  return exit_code;
}
