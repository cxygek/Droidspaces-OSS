/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"
#include <arpa/inet.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

/* Terminal session registry
 *
 * A keeper process owns the pty of an interactive enter session, not the
 * client connection, so the session outlives the app that opened it. Which
 * session is which cannot live only in the keeper's memory: a new command
 * needs a name to ask for, and stop needs to find the sessions of the
 * container it is tearing down. One record per session under Sessions/.
 */

int ds_session_id_valid(const char *id) {
  if (!id || strlen(id) != DS_SESSION_ID_LEN)
    return 0;
  for (size_t i = 0; i < DS_SESSION_ID_LEN; i++)
    if (!isxdigit((unsigned char)id[i]))
      return 0;
  return 1;
}

static int session_record_path(const char *id, char *buf, size_t size) {
  if (!ds_session_id_valid(id))
    return -1;
  int n = snprintf(buf, size, "%s/%s%s", get_sessions_dir(), id,
                   DS_SESSION_FILE_EXT);
  return (n > 0 && (size_t)n < size) ? 0 : -1;
}

/* One line per field, so a partial write cannot make a record look valid. The
 * values only reach this with a container name or a user name, neither of
 * which may carry a newline. */
static void session_field(const char *in, char *out, size_t size) {
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 1 < size; i++) {
    unsigned char c = (unsigned char)in[i];
    out[j++] = (c == '\n' || c == '\r') ? ' ' : (char)c;
  }
  out[j] = '\0';
}

int ds_session_record_write(const struct ds_session_record *rec) {
  if (!rec || !ds_session_id_valid(rec->id) || rec->pid <= 1)
    return -1;

  char path[PATH_MAX];
  if (session_record_path(rec->id, path, sizeof(path)) < 0)
    return -1;

  char container[sizeof(rec->container)];
  char user[sizeof(rec->user)];
  session_field(rec->container, container, sizeof(container));
  session_field(rec->user, user, sizeof(user));

  char content[1024];
  int n = snprintf(content, sizeof(content),
                   "id=%s\ncontainer=%s\nuser=%s\npid=%d\nstart=%llu\n"
                   "created=%lld\n",
                   rec->id, container, user, (int)rec->pid, rec->start,
                   rec->created);
  if (n < 0 || (size_t)n >= sizeof(content))
    return -1;

  mkdir_p(get_sessions_dir(), 0700);
  return write_file_atomic(path, content);
}

void ds_session_record_unlink(const char *id) {
  char path[PATH_MAX];
  if (session_record_path(id, path, sizeof(path)) == 0)
    unlink(path);
}

static int session_record_read(const char *path,
                               struct ds_session_record *rec) {
  char buf[1024];
  if (read_file(path, buf, sizeof(buf)) < 0)
    return -1;

  memset(rec, 0, sizeof(*rec));
  char *save = NULL;
  for (char *line = strtok_r(buf, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *val = eq + 1;
    size_t len = strlen(val);
    while (len > 0 && (val[len - 1] == '\r' || val[len - 1] == '\n'))
      val[--len] = '\0';

    if (strcmp(line, "id") == 0) {
      safe_strncpy(rec->id, val, sizeof(rec->id));
    } else if (strcmp(line, "container") == 0) {
      safe_strncpy(rec->container, val, sizeof(rec->container));
    } else if (strcmp(line, "user") == 0) {
      safe_strncpy(rec->user, val, sizeof(rec->user));
    } else if (strcmp(line, "pid") == 0) {
      char *end = NULL;
      long v = strtol(val, &end, 10);
      if (end && *end == '\0' && v > 1)
        rec->pid = (pid_t)v;
    } else if (strcmp(line, "start") == 0) {
      char *end = NULL;
      unsigned long long v = strtoull(val, &end, 10);
      if (end && *end == '\0')
        rec->start = v;
    } else if (strcmp(line, "created") == 0) {
      char *end = NULL;
      long long v = strtoll(val, &end, 10);
      if (end && *end == '\0')
        rec->created = v;
    }
  }

  return (ds_session_id_valid(rec->id) && rec->pid > 1) ? 0 : -1;
}

/* A pid alone can be recycled; the start time of that pid pins the keeper. */
static int session_alive(const struct ds_session_record *rec) {
  if (rec->pid <= 1)
    return 0;
  if (kill(rec->pid, 0) < 0 && errno == ESRCH)
    return 0;
  if (rec->start && proc_starttime(rec->pid) != rec->start)
    return 0;
  return 1;
}

/* The abstract name one session's keeper listens on. */
static int session_sock_name(const char *id, char *buf, size_t size) {
  if (!ds_session_id_valid(id))
    return -1;
  int n = snprintf(buf, size, "%s%s", DS_SESSION_SOCK_PREFIX, id);
  return (n > 0 && (size_t)n < size) ? 0 : -1;
}

/* Connect to an abstract name with a bounded wait: a listener whose owner is
 * wedged must not hang the command that asked. */
static int session_connect(const char *name, int seconds) {
  struct sockaddr_un addr;
  socklen_t alen = ds_abstract_addr(&addr, name);
  int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0)
    return -1;
  struct timeval tv = {seconds, 0};
  (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(s, (struct sockaddr *)&addr, alen) < 0) {
    close(s);
    return -1;
  }
  return s;
}

static int session_send_kill(const char *id, int wait_exit) {
  char name[sizeof(DS_SESSION_SOCK_PREFIX) + DS_SESSION_ID_LEN];
  if (session_sock_name(id, name, sizeof(name)) < 0)
    return -1;

  int sock = session_connect(name, 2);
  if (sock < 0)
    return -1;
  /* A squatter that bound this name would be handed the kill request and the
   * answer that follows, so the peer must be the keeper itself. */
  if (!ds_socket_peer_is(sock, name, DS_SESSION_PROC)) {
    close(sock);
    return -1;
  }

  signal(SIGPIPE, SIG_IGN);
  if (ds_send_frame(sock, MSG_SESSION_KILL, NULL, 0) < 0) {
    close(sock);
    return -1;
  }

  if (wait_exit) {
    /* The keeper answers with its exit frame once the shell is gone. Without
     * this the process would close the socket first and the keeper could read
     * the hangup before the kill frame. A timeout is a failure: the caller
     * must not report a session terminated when its keeper never answered. */
    int got_exit = 0, got_error = 0;
    struct timeval tv = {2, 0};
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
      uint8_t type = 0;
      uint32_t len = 0;
      if (ds_recv_frame_hdr(sock, &type, &len) < 0)
        break;
      if (type == MSG_EXIT)
        got_exit = 1;
      else if (type == MSG_ERR)
        got_error = 1;
      ds_drain_frame(sock, len);
      if (got_exit)
        break;
    }
    close(sock);
    return (got_exit && !got_error) ? 0 : -1;
  }

  close(sock);
  return 0;
}

int ds_session_kill(const char *id) {
  if (!ds_session_id_valid(id)) {
    ds_error("Invalid session id: '%s'", id);
    return 1;
  }
  if (session_send_kill(id, 1) < 0) {
    ds_error("No terminal session with id '%s'.", id);
    return 1;
  }
  ds_log("Terminated terminal session %s", id);
  return 0;
}

int ds_session_attach(const char *id) {
  if (!ds_session_id_valid(id)) {
    ds_error("Invalid session id: '%s'", id);
    return 1;
  }
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    ds_error("Interactive terminal is required for the session attach "
             "command");
    return 1;
  }

  char name[sizeof(DS_SESSION_SOCK_PREFIX) + DS_SESSION_ID_LEN];
  if (session_sock_name(id, name, sizeof(name)) < 0) {
    ds_error("Invalid session id: '%s'", id);
    return 1;
  }
  int sock = session_connect(name, 2);
  if (sock < 0) {
    ds_error("No terminal session with id '%s'.", id);
    return 1;
  }
  if (!ds_socket_peer_is(sock, name, DS_SESSION_PROC)) {
    close(sock);
    ds_error("No terminal session with id '%s'.", id);
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  struct winsize ws = {24, 80, 0, 0};
  (void)ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
  if (!ws.ws_row)
    ws.ws_row = 24;
  if (!ws.ws_col)
    ws.ws_col = 80;
  uint16_t wd[2] = {htons(ws.ws_row), htons(ws.ws_col)};
  if (ds_send_frame(sock, MSG_WINCH, wd, 4) < 0) {
    close(sock);
    ds_error("Failed to talk to session '%s'.", id);
    return 1;
  }

  /* The connect timeouts were there to bound the handshake. A session that
   * sits idle for an hour must not be ended by them. */
  struct timeval off = {0, 0};
  (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &off, sizeof(off));
  (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &off, sizeof(off));

  return ds_terminal_relay(sock, 1, 0);
}

/* enter: ask the container's monitor for a keeper, then attach to the session
 * it publishes. A container whose monitor does not answer, one started by a
 * build that predates this, keeps the old behaviour: the caller runs a plain
 * enter that survives nothing. */
int ds_session_enter(struct ds_config *cfg, const char *user,
                     const char *id_hint) {
  if (!cfg->container_name[0])
    return DS_SESSION_NO_KEEPER;
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    ds_error("Interactive terminal is required for the enter command");
    return 1;
  }

  char sock_name[128];
  ds_monitor_sock_name(cfg->container_name, DS_SESSION_CTL_LEAF, sock_name,
                       sizeof(sock_name));
  int fd = session_connect(sock_name, 5);
  if (fd < 0)
    return DS_SESSION_NO_KEEPER;
  /* Any process may bind an abstract name. The request that follows describes
   * a root shell, so the listener has to be this container's monitor. */
  if (!ds_socket_peer_is(fd, sock_name, DS_MONITOR_PROC)) {
    close(fd);
    return DS_SESSION_NO_KEEPER;
  }

  struct ds_session_open req;
  memset(&req, 0, sizeof(req));
  safe_strncpy(req.container, cfg->container_name, sizeof(req.container));
  safe_strncpy(req.user, user ? user : "root", sizeof(req.user));
  /* The shell behind this session speaks this terminal. Without it the guest
   * inherited the TERM of whatever started the container, which is a bare adb
   * or libsu shell when nobody started it from a terminal. Only a terminfo
   * name is accepted, so nothing else rides into the keeper's environment. */
  {
    const char *term = getenv("TERM");
    size_t n = 0;
    for (; term && term[n] && n + 1 < sizeof(req.term); n++) {
      unsigned char c = (unsigned char)term[n];
      if (!(isalnum(c) || c == '-' || c == '_' || c == '+' || c == '.'))
        break;
      req.term[n] = (char)c;
    }
    req.term[n] = '\0';
    if (!req.term[0])
      safe_strncpy(req.term, "xterm-256color", sizeof(req.term));
  }
  if (id_hint && id_hint[0])
    safe_strncpy(req.id, id_hint, sizeof(req.id));
  struct winsize ws = {24, 80, 0, 0};
  (void)ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
  req.rows = ws.ws_row ? ws.ws_row : 24;
  req.cols = ws.ws_col ? ws.ws_col : 80;

  signal(SIGPIPE, SIG_IGN);

  struct ds_session_answer ans;
  uint8_t type = 0;
  uint32_t len = 0;
  int ok = ds_send_frame(fd, DS_SESS_OPEN, &req, sizeof(req)) == 0 &&
           ds_recv_frame_hdr(fd, &type, &len) == 0 &&
           (type == DS_SESS_OK || type == DS_SESS_FAIL) && len == sizeof(ans) &&
           ds_read_exact(fd, &ans, sizeof(ans)) == 0;
  close(fd);
  if (!ok)
    return DS_SESSION_NO_KEEPER;
  if (!ans.ok) {
    ds_error("%s", ans.msg[0] ? ans.msg : "The monitor refused the session.");
    return 1;
  }
  if (!ds_session_id_valid(ans.id)) {
    ds_error("The monitor published an unusable session id.");
    return 1;
  }
  return ds_session_attach(ans.id);
}

void ds_session_kill_container(const char *name) {
  DIR *d = opendir(get_sessions_dir());
  if (!d)
    return;

  struct dirent *ent;
  const size_t ext_len = strlen(DS_SESSION_FILE_EXT);
  while ((ent = readdir(d)) != NULL) {
    size_t nlen = strlen(ent->d_name);
    if (nlen != DS_SESSION_ID_LEN + ext_len ||
        strcmp(ent->d_name + DS_SESSION_ID_LEN, DS_SESSION_FILE_EXT) != 0)
      continue;

    char id[DS_SESSION_ID_LEN + 1];
    memcpy(id, ent->d_name, DS_SESSION_ID_LEN);
    id[DS_SESSION_ID_LEN] = '\0';
    if (!ds_session_id_valid(id))
      continue;

    char path[PATH_MAX];
    if (session_record_path(id, path, sizeof(path)) < 0)
      continue;

    struct ds_session_record rec;
    if (session_record_read(path, &rec) < 0) {
      unlink(path);
      continue;
    }
    if (!session_alive(&rec)) {
      unlink(path);
      continue;
    }
    /* A record without a container name cannot be proven to belong to the
     * one being stopped; the cgroup teardown still takes its shell with the
     * container. */
    if (name && name[0] &&
        (!rec.container[0] || !same_container_name(rec.container, name)))
      continue;

    (void)session_send_kill(rec.id, 1);
  }
  closedir(d);
}

static int session_created_cmp(const void *a, const void *b) {
  const struct ds_session_record *ra = a;
  const struct ds_session_record *rb = b;
  if (ra->created < rb->created)
    return -1;
  if (ra->created > rb->created)
    return 1;
  return 0;
}

int ds_session_list(struct ds_config *cfg) {
  ensure_workspace();

  struct ds_session_record *list = NULL;
  size_t count = 0, cap = 0;

  DIR *d = opendir(get_sessions_dir());
  if (d) {
    struct dirent *ent;
    const size_t ext_len = strlen(DS_SESSION_FILE_EXT);
    while ((ent = readdir(d)) != NULL) {
      size_t nlen = strlen(ent->d_name);
      if (nlen != DS_SESSION_ID_LEN + ext_len ||
          strcmp(ent->d_name + DS_SESSION_ID_LEN, DS_SESSION_FILE_EXT) != 0)
        continue;

      char id[DS_SESSION_ID_LEN + 1];
      memcpy(id, ent->d_name, DS_SESSION_ID_LEN);
      id[DS_SESSION_ID_LEN] = '\0';
      if (!ds_session_id_valid(id))
        continue;

      char path[PATH_MAX];
      if (session_record_path(id, path, sizeof(path)) < 0)
        continue;

      struct ds_session_record rec;
      if (session_record_read(path, &rec) < 0 || strcmp(rec.id, id) != 0 ||
          !session_alive(&rec)) {
        unlink(path);
        continue;
      }
      if (cfg->container_name[0] && rec.container[0] &&
          !same_container_name(rec.container, cfg->container_name))
        continue;

      if (count == cap) {
        size_t ncap = cap ? cap * 2 : 8;
        struct ds_session_record *nl = realloc(list, ncap * sizeof(*nl));
        if (!nl)
          break;
        list = nl;
        cap = ncap;
      }
      list[count++] = rec;
    }
    closedir(d);
  }

  if (count > 1)
    qsort(list, count, sizeof(*list), session_created_cmp);

  if (cfg->json_output) {
    printf("{\"sessions\":[");
    for (size_t i = 0; i < count; i++) {
      int first = 1;
      printf(i ? ",{" : "{");
      ds_json_str("id", list[i].id, &first);
      ds_json_str("container", list[i].container, &first);
      ds_json_str("user", list[i].user, &first);
      ds_json_int("pid", (long long)list[i].pid, &first);
      ds_json_int("created", list[i].created, &first);
      printf("}");
    }
    printf("]}\n");
    free(list);
    return 0;
  }

  if (count == 0) {
    printf("\n(No terminal sessions running)\n\n");
    free(list);
    return 0;
  }

  printf("\n%-32s  %-24s  %-10s  %-8s  %s\n", "ID", "CONTAINER", "USER",
         "KEEPER", "CREATED");
  for (size_t i = 0; i < count; i++) {
    char when[32] = "-";
    time_t created = (time_t)list[i].created;
    struct tm tmv;
    if (created > 0 && localtime_r(&created, &tmv))
      (void)strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
    printf("%-32s  %-24s  %-10s  %-8d  %s\n", list[i].id,
           list[i].container[0] ? list[i].container : "-",
           list[i].user[0] ? list[i].user : "-", (int)list[i].pid, when);
  }
  printf("\n");
  free(list);
  return 0;
}
