/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_CORE_CORE_H
#define DS_CORE_CORE_H

/* exec.c */
int run_command_quiet(char *const argv[]);
int run_command_log(char *const argv[]);
/* 0 when the child was reaped within timeout_ms, -1 when it still runs. */
int ds_reap_child(pid_t pid, int timeout_ms, int *status);
void ds_oom_protect(void);
void ds_oom_unprotect(void);
int ds_thread_create(pthread_t *tid, void *(*fn)(void *), void *arg);

/* fdpass.c */
#define DS_MAX_PASSED_FDS 8
int ds_send_fds(int sock, const void *data, size_t len, const int *fds,
                int count);
ssize_t ds_recv_fds(int sock, void *data, size_t len, int *fds, int max,
                    int *count);
int ds_send_fd(int sock, int fd);
int ds_recv_fd(int sock);

/* frame.c */
struct sockaddr_un;

#define DS_IOBUF 8192
/* Client input queued toward a pty master: the daemon's pty sessions and a
 * session keeper's both use this, and both suspend the client above HIGH. */
#define PTY_WBUF_MAX (256 * 1024)
#define PTY_WBUF_HIGH (192 * 1024)
#define PTY_WBUF_LOW (64 * 1024)

/* What a terminal session's socket carries. The daemon's per-command sessions,
 * a keeper's session socket and the CLI that talks to either speak these. */
#define MSG_OUT ((uint8_t)0x01)
#define MSG_ERR ((uint8_t)0x02)
#define MSG_WINCH ((uint8_t)0x03)
#define MSG_SESSION_KILL ((uint8_t)0x04)
#define MSG_EXIT ((uint8_t)0xFF)
/* No exit code yet: the command behind the session still runs. */
#define EXIT_PENDING (-1)

int ds_read_exact(int fd, void *buf, size_t n);
int ds_send_frame(int fd, uint8_t type, const void *data, uint32_t len);
int ds_recv_frame_hdr(int fd, uint8_t *type_out, uint32_t *len_out);
/* Read and throw away the payload of a frame this end does not handle. */
void ds_drain_frame(int fd, uint32_t len);
socklen_t ds_abstract_addr(struct sockaddr_un *addr, const char *name);

/* Listening abstract sockets, read from /proc/net/unix: the kernel's own
 * table, which nothing inside a container can write. */
int ds_listener_next(FILE *f, char *name, size_t size, unsigned long *ino);
unsigned long ds_listener_inode(const char *name);
int ds_pid_holds_socket(pid_t pid, unsigned long ino);

/* fs.c */
int is_ramfs(const char *path);
int write_file(const char *path, const char *content);
int read_file(const char *path, char *buf, size_t size);
int write_file_atomic(const char *path, const char *content);
ssize_t write_all(int fd, const void *buf, size_t count);
int generate_uuid(char *buf, size_t size);
int mkdir_p(const char *path, mode_t mode);
int remove_recursive(const char *path);
int grep_file(const char *path, const char *pattern);
int count_folders(const char *path);

/* json.c */
/* Flat JSON members, `,"key":value`. *first suppresses the leading comma. */
void ds_json_str(const char *key, const char *val, int *first);
void ds_json_int(const char *key, long long val, int *first);

/* log.c */
/* Logging macros & Centralized Engine */

extern int ds_log_silent;
extern char ds_log_container_name[256];
extern int ds_log_container_fd;

void ds_log_internal(const char *prefix, const char *color, int is_err,
                     const char *fmt, ...);
void ds_die_internal(const char *fmt, ...);
/* Stamp log lines in the phone's timezone, not UTC. */
void ds_log_local_timezone(void);
void rotate_log(const char *path, size_t max_size);

#define ds_log(fmt, ...) ds_log_internal("+", C_GREEN, 0, fmt, ##__VA_ARGS__)
#define ds_warn(fmt, ...) ds_log_internal("!", C_YELLOW, 1, fmt, ##__VA_ARGS__)
#define ds_error(fmt, ...) ds_log_internal("-", C_RED, 1, fmt, ##__VA_ARGS__)
#define ds_die(fmt, ...) ds_die_internal(fmt, ##__VA_ARGS__)

void write_monitor_debug_log(const char *name, const char *fmt, ...);
void ds_open_container_log(struct ds_config *cfg);
void ds_close_container_log(void);
void container_log_path(const char *name, const char *file, char *buf,
                        size_t size);

/* paths.c */
char *ds_resolve_path_arg(const char *path);
int is_subpath(const char *parent, const char *child);

const char *get_workspace_dir(void);
const char *get_pids_dir(void);
const char *get_net_dir(void);
const char *get_logs_dir(void);
const char *get_sessions_dir(void);
int ensure_workspace(void);

/* platform.c */
int is_android(void);
void android_native_personality(void);

/* proc.c */
long ds_clk_tck(void);
long ds_get_container_uptime(pid_t pid);

/* Field 22 of /proc/<pid>/stat in clock ticks since boot, 0 if unreadable. */
unsigned long long proc_starttime(pid_t pid);
const char *proc_stat_field(pid_t pid, int n, char *buf, size_t size);
pid_t proc_ppid(pid_t pid);
uid_t proc_uid(pid_t pid);
int get_kernel_version(int *major, int *minor);
int collect_pids(pid_t **pids_out, size_t *count_out);
int build_proc_root_path(pid_t pid, const char *suffix, char *buf, size_t size);
int ds_open_in_root(pid_t pid, const char *path, int flags);
void check_kernel_recommendation(void);

/* string.c */
void safe_strncpy(char *dst, const char *src, size_t size);
int ds_parse_iface_csv(const char *val, char ifaces[][IFNAMSIZ], int *count,
                       int max);
void ds_format_uptime(long uptime_sec, char *buf, size_t size);
int ds_split_flags(const char *str, char ***out_argv, int *out_argc);
void ds_free_split_flags(char **argv, int argc);
long long ds_parse_size(const char *str);
void ds_format_size(long long bytes, char *buf, size_t sz);

#endif /* DS_CORE_CORE_H */
