/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_TTY_TTY_H
#define DS_TTY_TTY_H

/* console.c */
int ds_console_send(int sock, int master, int slave);
int ds_console_recv(int sock, int *master, int *slave);
int console_monitor_loop(int console_sock, pid_t monitor_pid,
                         struct ds_config *cfg);

/* proxy.c */
int ds_terminal_proxy(int master_fd);

/* relay.c */
/* Relay this process's terminal to a socket that speaks the session frames:
 * MSG_OUT/MSG_ERR to write, MSG_EXIT to finish, MSG_OUT from stdin to send.
 * Returns the exit code the far end reported. */
int ds_terminal_relay(int socket, int interactive, int relay_stdin);

/* pty.c */
int ds_openpty(const char *ptmx, int *master, int *slave, char *name);
int ds_terminal_create(struct ds_tty_info *tty);
int ds_terminal_set_stdfds(int fd);
int ds_terminal_make_controlling(int fd);
int ds_setup_tios(int fd, struct termios *old);

#endif /* DS_TTY_TTY_H */
