/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_SESSION_SESSION_H
#define DS_SESSION_SESSION_H

/* Terminal sessions that outlive the app that opened them.
 *
 * An interactive enter is held by a keeper process, not by the connection that
 * started it: the keeper owns the host pty, a bounded scrollback and an
 * abstract socket, so losing the client only detaches. The keeper is a child
 * of the container's monitor, which already survives everything the app does
 * and lives exactly as long as the container, so nothing here depends on the
 * daemon being up.
 *
 * One record per session under Sessions/ names its keeper, so a later command
 * can find what is still running. */

#define DS_SESSION_SOCK_PREFIX "droidspaces/session/"
#define DS_SESSION_ID_LEN DS_UUID_LEN
#define DS_SESSION_FILE_EXT ".session"
/* Kept so a reattached terminal can redraw what it missed. Bounded because a
 * detached session may run for hours. */
#define DS_SESSION_SCROLLBACK (256 * 1024)
/* Keepers one monitor hosts. A phone has no more terminals than this open, and
 * a fixed table keeps the monitor's poll loop and reaping static. */
#define DS_SESSION_MAX 16
/* What the keeper calls itself: the client checks the peer by this name, and
 * it is what `session list` shows as the keeper. */
#define DS_SESSION_PROC "[ds-session]"
/* The control socket's leaf name, next to the monitor's "command" one. */
#define DS_SESSION_CTL_LEAF "session"
/* The keeper's control fd: the monitor passes one end of a socketpair here. */
#define DS_SESSION_CTL_FD 3

/* Set for the enter command a keeper starts behind its own pty. That command
 * already has a session; asking for another one would put a keeper behind every
 * keeper, and the chain would only end when the monitor runs out of slots. */
#define DS_ENV_NO_SESSION "DS_NO_SESSION"

/* Requests on a monitor's session socket. The monitor answers with an id or a
 * reason, and the client then connects to the keeper's own socket. */
#define DS_SESS_OPEN ((uint8_t)'S')
#define DS_SESS_OK ((uint8_t)'K')
#define DS_SESS_FAIL ((uint8_t)'F')

struct ds_session_record {
  char id[DS_SESSION_ID_LEN + 1];
  char container[256];
  char user[256];
  pid_t pid; /* the keeper process */
  unsigned long long start;
  long long created;
};

/* What a client asks a monitor for, and what the monitor hands the keeper it
 * forks. The container name is the monitor's own, checked against this. term is
 * the client's terminal type: the shell behind this session must speak the
 * terminal that asked for it, not whatever the container was started from. */
struct ds_session_open {
  char container[256];
  char user[64];
  char term[32];
  char id[DS_SESSION_ID_LEN + 1];
  uint16_t rows, cols;
};

struct ds_session_answer {
  int32_t ok;
  char id[DS_SESSION_ID_LEN + 1];
  char msg[160];
};

/* registry.c: the CLI side, and the keeper's records. */
int ds_session_id_valid(const char *id);
int ds_session_record_write(const struct ds_session_record *rec);
void ds_session_record_unlink(const char *id);
int ds_session_list(struct ds_config *cfg);
int ds_session_attach(const char *id);
int ds_session_kill(const char *id);
void ds_session_kill_container(const char *name);

/* enter: ask the container's monitor for a keeper, then attach to it. Returns
 * the session's exit code, or DS_SESSION_NO_KEEPER when no monitor of ours
 * answers, so the caller falls back to a plain enter that keeps nothing. */
#define DS_SESSION_NO_KEEPER (-2)
int ds_session_enter(struct ds_config *cfg, const char *user,
                     const char *id_hint);

/* The keeper process the monitor forks. Never returns. */
void ds_session_keeper_run(void);

#endif /* DS_SESSION_SESSION_H */
