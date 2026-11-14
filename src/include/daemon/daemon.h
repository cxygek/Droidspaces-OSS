/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_DAEMON_DAEMON_H
#define DS_DAEMON_DAEMON_H

/* client.c */
/* cmd is the sub-command main() discovered, so a payload word like "run"
 * inside the command does not decide whether the session is interactive.
 * want_stdin is --stdin: relay our stdin to a pipe-mode run instead of
 * handing it /dev/null. */
int ds_client_run(int argc, char **argv, const char *cmd, int want_stdin);

/* server.c */
int ds_daemon_run(int foreground, char **argv);
int ds_daemon_probe(void);

/* sessions.c */
/* The live terminal sessions of one container (all when cfg->container_name
 * is empty), pruned of keepers that are gone. Human table or cfg->json_output
 * JSON. */
int ds_session_list(struct ds_config *cfg);
/* Reattach this process's terminal to a detached session. Returns its exit
 * code, or 1 when the session cannot be reached. */
int ds_session_attach(const char *id);
/* Ask one session's keeper to end it. */
int ds_session_kill(const char *id);
/* End every session of a container that is stopping. */
void ds_session_kill_container(const char *name);

#endif /* DS_DAEMON_DAEMON_H */
