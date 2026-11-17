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
#include <sys/un.h>
#include <sys/wait.h>

#include "internal.h"

/* connect to the daemon and relay our command */

static int send_request(int sock, int argc, char **argv, uint32_t flags) {
  uint32_t nf = htonl(flags), na = htonl((uint32_t)argc);
  if (write_all(sock, &nf, 4) < 0 || write_all(sock, &na, 4) < 0)
    return -1;
  for (int i = 0; i < argc; i++) {
    uint32_t al = (uint32_t)strlen(argv[i]), nal = htonl(al);
    if (write_all(sock, &nal, 4) < 0)
      return -1;
    if (al && write_all(sock, argv[i], al) < 0)
      return -1;
  }
  if (flags & REQ_FLAG_PTY) {
    struct winsize ws = {24, 80, 0, 0};
    ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    uint16_t wd[2] = {htons(ws.ws_row), htons(ws.ws_col)};
    if (write_all(sock, wd, 4) < 0)
      return -1;
  }
  return 0;
}

int ds_client_run(int argc, char **argv, const char *cmd, int want_stdin) {
  if (argc < 1)
    return -2;
  if (!cmd)
    cmd = "";

  int is_enter = strcmp(cmd, "enter") == 0;
  int is_boot = strcmp(cmd, "start") == 0 || strcmp(cmd, "restart") == 0;
  int interactive = is_enter || is_boot || strcmp(cmd, "run") == 0;
  int has_tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

  if (interactive && !has_tty) {
    if (is_enter) {
      ds_error("Interactive terminal is required for the enter command\n");
      return 1;
    }
    /* Strip -f/--foreground; start_rootfs() will warn and flip the switch. */
    if (is_boot) {
      for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 ||
            strcmp(argv[i], "--foreground") == 0) {
          for (int j = i; j < argc - 1; j++)
            argv[j] = argv[j + 1];
          argv[--argc] = NULL;
          break;
        }
      }
    }
    interactive = 0;
  }

  /* --stdin on a pipe-mode run: relay our stdin to the command the way lxc
   * exec does. Never by default, because libsu style callers share their
   * stdin with the shell that is feeding them commands and we would eat the
   * next line. A terminal session relays stdin anyway. */
  int relay_stdin = want_stdin && !interactive && strcmp(cmd, "run") == 0;

  /* try to connect - single pass for execution path, retry loop only for 1st
   * connect */
  int sock = -1;
  struct sockaddr_un addr;
  socklen_t alen = ds_abstract_addr(&addr, DS_SOCK_NAME);

  int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0) {
    fprintf(stderr, "client: socket: %s\n", strerror(errno));
    return 1;
  }
  if (connect(s, (struct sockaddr *)&addr, alen) < 0) {
    int err = errno;
    close(s);
    if (err == ECONNREFUSED || err == ENOENT)
      return -2; /* Daemon not listening */
    fprintf(stderr, "[-] Connection to daemon failed: %s\n", strerror(err));
    return 1;
  }
  sock = s;

  /* The daemon may reject us and close before we finish writing the request.
   * Without this the kernel kills us with SIGPIPE: exit 141, no output. */
  signal(SIGPIPE, SIG_IGN);

  /* EPIPE means the daemon already closed. Its verdict (an error frame and
   * the exit code) is queued on our socket, so fall through and relay it. */
  mode_t mask = umask(0); /* musl has no getumask(), read it by swapping */
  umask(mask);
  uint32_t flags = (interactive ? REQ_FLAG_PTY : 0u) |
                   (relay_stdin ? REQ_FLAG_STDIN : 0u) | REQ_FLAG_UMASK |
                   ((uint32_t)(mask & 0777) << REQ_UMASK_SHIFT);
  if (send_request(sock, argc, argv, flags) < 0 && errno != EPIPE)
    goto send_err;

  return ds_terminal_relay(sock, interactive, relay_stdin);

send_err:
  fprintf(stderr, "client: send failed: %s\n", strerror(errno));
  close(sock);
  return 1;
}
