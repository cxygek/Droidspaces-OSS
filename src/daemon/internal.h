/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_DAEMON_INTERNAL_H
#define DS_DAEMON_INTERNAL_H

#define DS_SOCK_NAME "droidspaces"
#define DS_BACKLOG SOMAXCONN
#define DS_MAX_ARGC 64
#define DS_MAX_ARG 8192
#define DS_IOBUF 8192
/* Client input queued toward the child: the pty master in PTY mode, the
 * command's stdin pipe in pipe mode. */
#define PTY_WBUF_MAX                                                           \
  (256 * 1024)                     /* absolute buffer cap for PTY master input \
                                    */
#define PTY_WBUF_HIGH (192 * 1024) /* suspend conn EPOLLIN above this */
#define PTY_WBUF_LOW (64 * 1024)   /* resume  conn EPOLLIN below this */

#define MSG_OUT ((uint8_t)0x01)
#define MSG_ERR ((uint8_t)0x02)
#define MSG_WINCH ((uint8_t)0x03)
#define MSG_SESSION_KILL ((uint8_t)0x04)
#define MSG_EXIT ((uint8_t)0xFF)

/* Interactive enter sessions. The pty master and the shell belong to a keeper
 * process, not to the client connection, so an app that dies does not take the
 * session with it. Each keeper publishes one record under Sessions/ and
 * listens on its own abstract socket; connect and send MSG_WINCH to attach. */
#define DS_SESSION_SOCK_PREFIX "droidspaces/session/"
#define DS_SESSION_ID_LEN DS_UUID_LEN
#define DS_SESSION_FILE_EXT ".session"
/* Kept so a reattached terminal can redraw what it missed. Bounded because a
 * detached session may run for hours. */
#define DS_SESSION_SCROLLBACK (256 * 1024)

struct ds_session_record {
  char id[DS_SESSION_ID_LEN + 1];
  char container[256];
  char user[256];
  pid_t pid; /* the keeper process */
  unsigned long long start;
  long long created;
};

#define REQ_FLAG_PTY (1u << 0)
#define REQ_FLAG_STDIN (1u << 1) /* pipe mode: client relays its stdin */
/* The client's umask rides in bits 16-24 of the flags word, not in bytes of
 * its own: after a live binary swap an old daemon parses a new client's
 * request, and it ignores unknown flag bits but would read extra bytes as the
 * start of the stdin stream. */
#define REQ_FLAG_UMASK (1u << 2)
#define REQ_UMASK_SHIFT 16
#define EXIT_PENDING (-1)

typedef struct {
  uint32_t flags;
  int argc;
  char *argv[DS_MAX_ARGC + 1];
  uint16_t rows, cols;
} ds_req_t;

void daemon_log_tee(const char *prefix, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#undef ds_log
#define ds_log(fmt, ...)                                                       \
  do {                                                                         \
    ds_log_internal("+", C_GREEN, 0, fmt, ##__VA_ARGS__);                      \
    daemon_log_tee("+", fmt, ##__VA_ARGS__);                                   \
  } while (0)

#undef ds_warn
#define ds_warn(fmt, ...)                                                      \
  do {                                                                         \
    ds_log_internal("!", C_YELLOW, 1, fmt, ##__VA_ARGS__);                     \
    daemon_log_tee("!", fmt, ##__VA_ARGS__);                                   \
  } while (0)

#undef ds_error
#define ds_error(fmt, ...)                                                     \
  do {                                                                         \
    ds_log_internal("-", C_RED, 1, fmt, ##__VA_ARGS__);                        \
    daemon_log_tee("-", fmt, ##__VA_ARGS__);                                   \
  } while (0)

extern char g_self_path[PATH_MAX];
void send_exit(int fd, int code);
void free_req(ds_req_t *r);
int recv_req(int fd, ds_req_t *r);
void reexec(char **argv);
char **make_exec_argv(ds_req_t *r);
/* Both close the connection and leave *conn at -1. */
void handle_session(int *conn, ds_req_t *r);
void handle_session_enter(int *conn, ds_req_t *r);

/* client.c: relay this terminal to an already connected session socket. */
int ds_terminal_relay(int socket, int interactive, int relay_stdin);

/* sessions.c, keeper side. */
int ds_session_id_valid(const char *id);
int ds_session_record_write(const struct ds_session_record *rec);
void ds_session_record_unlink(const char *id);

#endif /* DS_DAEMON_INTERNAL_H */
