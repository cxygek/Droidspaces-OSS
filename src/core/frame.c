/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"
#include <sys/socket.h>
#include <sys/un.h>

/* The wire format the daemon and the monitor command socket share: a type
 * byte, a big-endian u32 length, then the payload. */

int ds_read_exact(int fd, void *buf, size_t n) {
  uint8_t *p = (uint8_t *)buf;
  while (n) {
    ssize_t r = read(fd, p, n);
    if (r <= 0)
      return -1;
    p += r;
    n -= (size_t)r;
  }
  return 0;
}

int ds_send_frame(int fd, uint8_t type, const void *data, uint32_t len) {
  uint8_t hdr[5];
  uint32_t nl = htonl(len);
  hdr[0] = type;
  memcpy(hdr + 1, &nl, 4);
  if (write_all(fd, hdr, 5) < 0)
    return -1;
  if (len && write_all(fd, data, len) < 0)
    return -1;
  return 0;
}

int ds_recv_frame_hdr(int fd, uint8_t *type_out, uint32_t *len_out) {
  uint8_t hdr[5];
  if (ds_read_exact(fd, hdr, 5) < 0)
    return -1;
  *type_out = hdr[0];
  uint32_t nl;
  memcpy(&nl, hdr + 1, 4);
  *len_out = ntohl(nl);
  return 0;
}

void ds_drain_frame(int fd, uint32_t len) {
  char buf[DS_IOBUF];
  while (len > 0) {
    uint32_t chunk = len < sizeof(buf) ? len : (uint32_t)sizeof(buf);
    if (ds_read_exact(fd, buf, chunk) < 0)
      return;
    len -= chunk;
  }
}

/* An abstract unix address for name. The length is exact, with no trailing
 * NUL, so "droidspaces" and "droidspaces/x/command" never collide. */
socklen_t ds_abstract_addr(struct sockaddr_un *addr, const char *name) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  size_t nlen = strlen(name);
  if (nlen > sizeof(addr->sun_path) - 1)
    nlen = sizeof(addr->sun_path) - 1;
  memcpy(addr->sun_path + 1, name, nlen);
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nlen);
}

/* The next listener on an abstract name in the kernel's table of unix
 * sockets: its name without the '@', and its inode. Lines are
 * Num RefCount Protocol Flags Type St Inode Path. Listening sockets carry
 * __SO_ACCEPTCON in Flags, 00010000; their accepted connections show the same
 * path and are skipped. Only our network namespace's sockets are listed, and
 * an abstract name has one listener in it. */
int ds_listener_next(FILE *f, char *name, size_t size, unsigned long *ino) {
  char line[512], flags[16], path[256];
  while (fgets(line, sizeof(line), f)) {
    unsigned long inode;
    if (sscanf(line, "%*s %*s %*s %15s %*s %*s %lu %255s", flags, &inode,
               path) == 3 &&
        strcmp(flags, "00010000") == 0 && path[0] == '@' &&
        strlen(path + 1) < size) {
      strcpy(name, path + 1);
      *ino = inode;
      return 1;
    }
  }
  return 0;
}

unsigned long ds_listener_inode(const char *name) {
  FILE *f = fopen("/proc/net/unix", "re");
  if (!f)
    return 0;
  char found[256];
  unsigned long ino = 0, want = 0;
  while (ds_listener_next(f, found, sizeof(found), &ino))
    if (strcmp(found, name) == 0) {
      want = ino;
      break;
    }
  fclose(f);
  return want;
}

/* 1 if pid holds the socket with inode ino among its open fds */
int ds_pid_holds_socket(pid_t pid, unsigned long ino) {
  char dir[64], want[64];
  if (pid <= 1 || ino == 0)
    return 0;
  snprintf(dir, sizeof(dir), "/proc/%d/fd", (int)pid);
  snprintf(want, sizeof(want), "socket:[%lu]", ino);
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  int held = 0;
  struct dirent *e;
  while (!held && (e = readdir(d)) != NULL) {
    char path[320], link[64];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n > 0) {
      link[n] = '\0';
      held = strcmp(link, want) == 0;
    }
  }
  closedir(d);
  return held;
}
