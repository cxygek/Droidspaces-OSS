/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * The host's cable into a container LAN. See net/hostlan.h for what it is for.
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <stdarg.h>
#include <stddef.h>
#include <poll.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"
#include "dhcp/internal.h"
#include "net/hostlan.h"

extern char **environ;

#define HOSTLAN_CONF "hostlan.conf"
#define HOSTLAN_FORWARDS "forwards.conf"
#define HOSTLAN_FORWARD_SWITCH "forwarding.conf"

#define PF_CHAIN_NAT "DSPF_NAT"
#define PF_CHAIN_FWD "DSPF_FWD"
#define PF_CHAIN_LOCAL "DSPF_LOCAL"
#define PF_TAG "dspaces:net"
#define HOSTLAN_LEASE "hostlan.lease"
#define HOSTLAN_PID "hostlan.pid"
#define HOSTLAN_LOCK "hostlan.lock"

/* The lease keeper re-acquires instead of renewing in place: a DISCOVER on a
 * fixed MAC always comes back with the same address, and one code path is one
 * place for a bug to live. Half the lease, capped so a long lease still gets
 * an hourly confirmation that the cable is still plugged in. */
#define HOSTLAN_RENEW_MAX_SEC 1800
#define HOSTLAN_RETRY_SEC 3
/* One DISCOVER window. The gateway answers a broadcast in milliseconds, so a
 * long wait only delays the retry; three short rounds beat one long one. */
#define HOSTLAN_DISCOVER_WAIT_MS 900
#define HOSTLAN_ACQUIRE_TRIES 3
#define HOSTLAN_ACQUIRE_GAP_MS 200

/* The host end of a gateway's LAN cable, as net/naming.c names it. Its presence
 * in a bridge is what makes that bridge a container LAN and not the NAT one. */
#define GATEWAY_HOST_VETH_PREFIX "ds-g"

/* RFC 2132 option 55: the options a client would like back. */
#define OPT_PARAM_LIST 55

struct hostlan_intent {
  char bridge[IFNAMSIZ];
  char iface[IFNAMSIZ];
  char peer[IFNAMSIZ];
};

struct hostlan_lease {
  uint32_t ip_be;
  uint32_t mask_be;
  uint32_t gw_be;
};

static void net_file(const char *name, char *buf, size_t size) {
  snprintf(buf, size, "%s/%s", get_net_dir(), name);
}

static int write_line_file(const char *path, const char *text) {
  return write_file(path, text);
}

static int
intent_read(struct hostlan_intent *in) {
  char path[PATH_MAX], buf[512] = {0};
  memset(in, 0, sizeof(*in));
  net_file(HOSTLAN_CONF, path, sizeof(path));
  if (read_file(path, buf, sizeof(buf)) <= 0)
    return 0;

  snprintf(in->bridge, sizeof(in->bridge), "%s", DS_HOSTLAN_BRIDGE);
  snprintf(in->iface, sizeof(in->iface), "%s", DS_HOSTLAN_IFACE);
  snprintf(in->peer, sizeof(in->peer), "%s", DS_HOSTLAN_PEER);

  for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
    char *eq = strchr(line, '=');
    if (!eq || line[0] == '#')
      continue;
    *eq = '\0';
    const char *key = line;
    const char *val = eq + 1;
    if (strcmp(key, "bridge") == 0)
      snprintf(in->bridge, sizeof(in->bridge), "%s", val);
    else if (strcmp(key, "iface") == 0)
      snprintf(in->iface, sizeof(in->iface), "%s", val);
    else if (strcmp(key, "peer") == 0)
      snprintf(in->peer, sizeof(in->peer), "%s", val);
  }
  return 1;
}

static int intent_write(const struct hostlan_intent *in) {
  char path[PATH_MAX], body[512];
  mkdir_p(get_net_dir(), 0755);
  net_file(HOSTLAN_CONF, path, sizeof(path));
  snprintf(body, sizeof(body),
           "# Written by 'droidspaces net join'; 'net leave' removes it.\n"
           "bridge=%s\niface=%s\npeer=%s\n",
           in->bridge, in->iface, in->peer);
  return write_line_file(path, body);
}

static int lease_read(struct hostlan_lease *l) {
  char path[PATH_MAX], buf[256] = {0};
  memset(l, 0, sizeof(*l));
  net_file(HOSTLAN_LEASE, path, sizeof(path));
  if (read_file(path, buf, sizeof(buf)) <= 0)
    return 0;
  unsigned a = 0, b = 0, c = 0, d = 0, g1 = 0, g2 = 0, g3 = 0, g4 = 0;
  int prefix = 0;
  int fields = sscanf(buf, "%u.%u.%u.%u %d %u.%u.%u.%u", &a, &b, &c, &d,
                      &prefix, &g1, &g2, &g3, &g4);
  if (fields < 5)
    return 0;
  if (fields == 9) {
    char gw[INET_ADDRSTRLEN];
    snprintf(gw, sizeof(gw), "%u.%u.%u.%u", g1, g2, g3, g4);
    if (inet_pton(AF_INET, gw, &l->gw_be) != 1)
      l->gw_be = 0;
  }
  if (prefix <= 0 || prefix > 32)
    return 0;
  char ip[INET_ADDRSTRLEN];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a, b, c, d);
  if (inet_pton(AF_INET, ip, &l->ip_be) != 1)
    return 0;
  l->mask_be = htonl(0xFFFFFFFFu << (32 - prefix));
  return 1;
}

static void lease_write(uint32_t ip_be, uint8_t prefix, uint32_t gw_be) {
  char path[PATH_MAX], body[96];
  char ip[INET_ADDRSTRLEN], gw[INET_ADDRSTRLEN] = "";
  if (!inet_ntop(AF_INET, &ip_be, ip, sizeof(ip)))
    return;
  if (gw_be && !inet_ntop(AF_INET, &gw_be, gw, sizeof(gw)))
    gw[0] = '\0';
  net_file(HOSTLAN_LEASE, path, sizeof(path));
  snprintf(body, sizeof(body), "%s %u %s\n", ip, (unsigned)prefix, gw);
  write_line_file(path, body);
}

static void lease_clear(void) {
  char path[PATH_MAX];
  net_file(HOSTLAN_LEASE, path, sizeof(path));
  unlink(path);
}

/* One writer at a time. Join, leave and every reconcile take it, so two
 * monitors waking on the same tick cannot race a veth into existence twice. */
static int state_lock(void) {
  char path[PATH_MAX];
  mkdir_p(get_net_dir(), 0755);
  net_file(HOSTLAN_LOCK, path, sizeof(path));
  int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0)
    return -1;
  if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void state_unlock(int fd) {
  if (fd < 0)
    return;
  flock(fd, LOCK_UN);
  close(fd);
}

static pid_t keeper_pid(void) {
  char path[PATH_MAX], buf[32] = {0};
  net_file(HOSTLAN_PID, path, sizeof(path));
  if (read_file(path, buf, sizeof(buf)) <= 0)
    return 0;
  return (pid_t)atoi(buf);
}

static int keeper_alive(void) {
  pid_t p = keeper_pid();
  if (p <= 0)
    return 0;
  if (kill(p, 0) != 0 && errno != EPERM)
    return 0;
  /* A keeper that exited stays a zombie until it is reaped, and kill() answers
   * 0 for zombies: without this, reconcile would believe a dead keeper is
   * still running and the lease would never be renewed again. Reaping here
   * also keeps the zombie from sitting in ps. waitpid() fails with ECHILD when
   * the keeper is not our child (a CLI asking for status), so the kill()
   * answer above is all there is in that case. */
  if (waitpid(p, NULL, WNOHANG) == p)
    return 0;
  return 1;
}

static int keeper_spawn(void) {
  /* Every join used to spawn one more keeper; they all ask for the same lease
   * on the same MAC, so the extras are pure noise in ps. One is enough. */
  if (keeper_alive())
    return 0;

  /* Spawned from the monitor loop too, so this must not be a live readlink:
   * after a binary swap it would name a deleted file. */
  char exe[PATH_MAX];
  safe_strncpy(exe, ds_self_path(), sizeof(exe));
  if (!exe[0]) {
    ds_warn("[NET] Host LAN: cannot resolve our own binary");
    return -1;
  }

  posix_spawnattr_t attr;
  posix_spawn_file_actions_t fa;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);

  char *argv[] = {exe, "net", "dhcp", NULL};
  pid_t pid = 0;
  int rc = posix_spawn(&pid, exe, &fa, &attr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    ds_warn("[NET] Host LAN: cannot start the lease keeper (%d)", rc);
    return -1;
  }

  char path[PATH_MAX], body[32];
  net_file(HOSTLAN_PID, path, sizeof(path));
  snprintf(body, sizeof(body), "%d\n", (int)pid);
  write_line_file(path, body);
  return 0;
}

static void keeper_stop(void) {
  pid_t p = keeper_pid();
  if (p > 0 && kill(p, 0) == 0) {
    kill(p, SIGTERM);
    for (int i = 0; i < 20; i++) {
      if (kill(p, 0) != 0)
        break;
      usleep(50 * 1000);
    }
    if (kill(p, 0) == 0)
      kill(p, SIGKILL);
  }
  char path[PATH_MAX];
  net_file(HOSTLAN_PID, path, sizeof(path));
  unlink(path);
}

static int link_up(ds_nl_ctx_t *ctx, const char *name) {
  return name && *name && ds_nl_link_exists(ctx, name) > 0;
}

/* Create whatever is missing so the host has a cable into `in->bridge`.
 * Returns 0 when the cable is in place. */
static int links_ensure(ds_nl_ctx_t *ctx, const struct hostlan_intent *in) {
  if (!link_up(ctx, in->bridge)) {
    if (ds_nl_create_bridge(ctx, in->bridge) < 0) {
      ds_warn("[NET] Host LAN: cannot create bridge %s", in->bridge);
      return -1;
    }
    ds_nl_link_up(ctx, in->bridge);
    ds_log("[NET] Host LAN: created bridge %s", in->bridge);
  }

  int have_host = link_up(ctx, in->iface);
  int have_peer = link_up(ctx, in->peer);
  if (!have_host || !have_peer) {
    /* Half a veth pair cannot be completed: the kernel destroys both ends
     * together, so a survivor means the other end went away with its netns.
     * Start from nothing. */
    if (have_host)
      ds_nl_del_link(ctx, in->iface);
    if (have_peer)
      ds_nl_del_link(ctx, in->peer);

    if (ds_nl_create_veth(ctx, in->iface, in->peer) < 0) {
      ds_warn("[NET] Host LAN: cannot create %s <-> %s", in->iface, in->peer);
      return -1;
    }
    uint8_t mac[6];
    ds_derive_mac(in->iface, "ds-hl:", mac);
    ds_nl_set_mac(ctx, in->iface, mac);
    ds_derive_mac(in->peer, "ds-hl:", mac);
    ds_nl_set_mac(ctx, in->peer, mac);
    ds_log("[NET] Host LAN: veth %s <-> %s", in->iface, in->peer);
  }

  if (ds_nl_set_master(ctx, in->peer, in->bridge) < 0) {
    ds_warn("[NET] Host LAN: cannot enslave %s to %s", in->peer, in->bridge);
    return -1;
  }
  ds_nl_link_up(ctx, in->iface);
  ds_nl_link_up(ctx, in->peer);
  return 0;
}

static void links_teardown(const struct hostlan_intent *in) {
  ds_nl_ctx_t *ctx = ds_nl_open();
  if (!ctx)
    return;
  if (link_up(ctx, in->iface))
    ds_nl_del_link(ctx, in->iface);
  if (link_up(ctx, in->peer))
    ds_nl_del_link(ctx, in->peer);
  ds_nl_close(ctx);
}

/* Bridges on offer --------------------------------------------------------- */

struct lan_bridge {
  char name[IFNAMSIZ];
  int members;
};

/* 1 when /sys/class/net/<br>/brif holds the host end of a gateway's cable. */
static int bridge_carries_gateway(const char *br, int *members) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/sys/class/net/%s/brif", br);
  DIR *d = opendir(path);
  if (!d)
    return 0;

  int count = 0, gateway = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    count++;
    if (strncmp(e->d_name, GATEWAY_HOST_VETH_PREFIX,
                strlen(GATEWAY_HOST_VETH_PREFIX)) == 0)
      gateway = 1;
  }
  closedir(d);
  if (members)
    *members = count;
  return gateway;
}

/* Every container LAN the host could plug into, in name order. */
static int lan_bridge_scan(struct lan_bridge *out, int max) {
  DIR *d = opendir("/sys/class/net");
  if (!d)
    return 0;

  int n = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (n >= max)
      break;
    if (strncmp(e->d_name, "ds-", 3) != 0)
      continue;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/net/%s/bridge", e->d_name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
      continue;

    int members = 0;
    if (!bridge_carries_gateway(e->d_name, &members))
      continue;
    snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
    out[n].members = members;
    n++;
  }
  closedir(d);

  /* readdir order is whatever the hash says; the picker should not shuffle. */
  for (int i = 1; i < n; i++) {
    struct lan_bridge key = out[i];
    int j = i - 1;
    while (j >= 0 && strcmp(out[j].name, key.name) > 0) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = key;
  }
  return n;
}

/* DHCP client ------------------------------------------------------------- */

static uint16_t ip_checksum(const void *data, size_t len) {
  const uint16_t *p = (const uint16_t *)data;
  uint32_t sum = 0;
  while (len > 1) {
    sum += *p++;
    len -= 2;
  }
  if (len)
    sum += *(const uint8_t *)p;
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

static int dhcp_send(int sock, int ifindex, const uint8_t mac[6],
                     const struct dhcp_pkt *pkt, int pkt_len) {
  uint8_t frame[1600];
  int total = (int)(sizeof(struct ethhdr) + sizeof(struct iphdr) +
                    sizeof(struct udphdr) + (size_t)pkt_len);
  if (total > (int)sizeof(frame))
    return -1;

  struct ethhdr *eth = (struct ethhdr *)frame;
  struct iphdr *ip = (struct iphdr *)(frame + sizeof(struct ethhdr));
  struct udphdr *udp =
      (struct udphdr *)(frame + sizeof(struct ethhdr) + sizeof(struct iphdr));
  uint8_t *payload = frame + sizeof(struct ethhdr) + sizeof(struct iphdr) +
                     sizeof(struct udphdr);

  memset(eth->h_dest, 0xFF, 6);
  memcpy(eth->h_source, mac, 6);
  eth->h_proto = htons(ETH_P_IP);

  memset(ip, 0, sizeof(*ip));
  ip->ihl = 5;
  ip->version = 4;
  ip->tot_len = htons((uint16_t)(sizeof(struct iphdr) +
                                 sizeof(struct udphdr) + (size_t)pkt_len));
  ip->ttl = 64;
  ip->protocol = IPPROTO_UDP;
  ip->saddr = 0;
  ip->daddr = htonl(INADDR_BROADCAST);
  ip->check = ip_checksum(ip, sizeof(*ip));

  udp->source = htons(DHCP_CLIENT_PORT);
  udp->dest = htons(DHCP_SERVER_PORT);
  udp->len = htons((uint16_t)(sizeof(struct udphdr) + (size_t)pkt_len));
  udp->check = 0;

  memcpy(payload, pkt, (size_t)pkt_len);

  struct sockaddr_ll dst;
  memset(&dst, 0, sizeof(dst));
  dst.sll_family = AF_PACKET;
  dst.sll_ifindex = ifindex;
  dst.sll_halen = 6;
  memset(dst.sll_addr, 0xFF, 6);

  return sendto(sock, frame, (size_t)total, 0, (struct sockaddr *)&dst,
                sizeof(dst)) < 0
             ? -1
             : 0;
}

/* Wait for one DHCP reply of `want_type` for `xid` and copy its payload. */
static int dhcp_wait(int sock, int ifindex, uint32_t xid, const uint8_t mac[6],
                     uint8_t want_type, struct dhcp_pkt *out, int timeout_ms) {
  struct pollfd pfd = {.fd = sock, .events = POLLIN, .revents = 0};
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                        (now.tv_nsec - start.tv_nsec) / 1000000);
    if (elapsed >= timeout_ms)
      return -1;

    int rc = poll(&pfd, 1, timeout_ms - elapsed);
    if (rc <= 0) {
      if (rc < 0 && errno == EINTR)
        continue;
      return -1;
    }

    uint8_t buf[2048];
    struct sockaddr_ll from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from,
                         &fromlen);
    if (n < (ssize_t)(sizeof(struct ethhdr) + sizeof(struct iphdr) +
                      sizeof(struct udphdr)) ||
        (int)from.sll_ifindex != ifindex)
      continue;

    const struct iphdr *ip = (const struct iphdr *)(buf + sizeof(struct ethhdr));
    size_t ip_len = (size_t)ip->ihl * 4;
    if (ip->protocol != IPPROTO_UDP ||
        n < (ssize_t)(sizeof(struct ethhdr) + ip_len + sizeof(struct udphdr)))
      continue;
    const struct udphdr *udp = (const struct udphdr *)(buf + sizeof(struct ethhdr) + ip_len);
    if (ntohs(udp->dest) != DHCP_CLIENT_PORT)
      continue;

    const uint8_t *payload = (const uint8_t *)udp + sizeof(struct udphdr);
    size_t plen = n - (size_t)(sizeof(struct ethhdr) + ip_len + sizeof(struct udphdr));
    if (plen < offsetof(struct dhcp_pkt, options))
      continue;

    const struct dhcp_pkt *pkt = (const struct dhcp_pkt *)payload;
    if (ntohl(pkt->xid) != xid || ntohl(pkt->magic) != DHCP_MAGIC)
      continue;
    if (memcmp(pkt->chaddr, mac, 6) != 0)
      continue;

    uint8_t type = 0;
    int opts_len = (int)(plen - offsetof(struct dhcp_pkt, options));
    if (opt_get(pkt->options, opts_len, OPT_MSG_TYPE, &type, 1) < 1)
      continue;
    if (type != want_type)
      continue;

    memcpy(out, pkt, plen < sizeof(*out) ? plen : sizeof(*out));
    return 0;
  }
}

/* Send `pkt` and wait for one reply, resending a few times: a fresh veth can
 * swallow the first broadcast while the bridge learns where the port is, and
 * one long wait would spend the whole window on that lost packet. */
static int dhcp_xchg(int sock, int ifindex, const uint8_t mac[6],
                     const struct dhcp_pkt *pkt, int pkt_len, uint32_t xid,
                     uint8_t want_type, struct dhcp_pkt *out) {
  const int rounds = 4;
  for (int i = 0; i < rounds; i++) {
    if (dhcp_send(sock, ifindex, mac, pkt, pkt_len) < 0)
      return -1;
    if (dhcp_wait(sock, ifindex, xid, mac, want_type, out,
                  HOSTLAN_DISCOVER_WAIT_MS / rounds) == 0)
      return 0;
  }
  return -1;
}

static void dhcp_put_option(struct dhcp_pkt *pkt, int *pos, uint8_t code,
                            const void *val, int len) {
  if (*pos + 2 + len >= (int)sizeof(pkt->options))
    return;
  pkt->options[(*pos)++] = code;
  pkt->options[(*pos)++] = (uint8_t)len;
  memcpy(pkt->options + *pos, val, (size_t)len);
  *pos += len;
}

/* Ask the gateway for an address. Fills ip/mask/gw and returns 0 on success. */
static int dhcp_acquire(int sock, int ifindex, const uint8_t mac[6],
                        uint32_t *ip_be, uint32_t *mask_be, uint32_t *gw_be) {
  uint32_t xid = (uint32_t)(time(NULL) ^ (getpid() << 16) ^ (uintptr_t)mac);
  struct dhcp_pkt pkt;
  struct dhcp_pkt reply;

  memset(&pkt, 0, sizeof(pkt));
  pkt.op = BOOTP_REQUEST;
  pkt.htype = HTYPE_ETHERNET;
  pkt.hlen = 6;
  pkt.xid = htonl(xid);
  pkt.flags = htons(0x8000); /* broadcast: the interface has no address yet */
  memcpy(pkt.chaddr, mac, 6);
  pkt.magic = htonl(DHCP_MAGIC);

  int pos = 0;
  uint8_t discover = DHCPDISCOVER;
  dhcp_put_option(&pkt, &pos, OPT_MSG_TYPE, &discover, 1);
  uint8_t prl[] = {OPT_SUBNET_MASK, OPT_ROUTER, OPT_LEASE_TIME};
  dhcp_put_option(&pkt, &pos, OPT_PARAM_LIST, prl, (int)sizeof(prl));
  pkt.options[pos] = OPT_END;

  if (dhcp_xchg(sock, ifindex, mac, &pkt,
                (int)(offsetof(struct dhcp_pkt, options) + (size_t)pos + 1), xid,
                DHCPOFFER, &reply) < 0)
    return -1;

  uint8_t server_id[4];
  memset(server_id, 0, sizeof(server_id));
  (void)opt_get(reply.options, (int)sizeof(reply.options), OPT_SERVER_ID,
                server_id, 4);

  /* Request exactly what was offered. */
  memset(&pkt, 0, sizeof(pkt));
  pkt.op = BOOTP_REQUEST;
  pkt.htype = HTYPE_ETHERNET;
  pkt.hlen = 6;
  pkt.xid = htonl(xid);
  pkt.flags = htons(0x8000);
  memcpy(pkt.chaddr, mac, 6);
  pkt.magic = htonl(DHCP_MAGIC);
  pos = 0;
  uint8_t request = DHCPREQUEST;
  dhcp_put_option(&pkt, &pos, OPT_MSG_TYPE, &request, 1);
  dhcp_put_option(&pkt, &pos, 50, &reply.yiaddr, 4);
  if (server_id[0] || server_id[1] || server_id[2] || server_id[3])
    dhcp_put_option(&pkt, &pos, OPT_SERVER_ID, server_id, 4);
  pkt.options[pos] = OPT_END;

  if (dhcp_xchg(sock, ifindex, mac, &pkt,
                (int)(offsetof(struct dhcp_pkt, options) + (size_t)pos + 1), xid,
                DHCPACK, &reply) < 0)
    return -1;

  int opts_len = (int)sizeof(reply.options);
  uint8_t mask[4] = {255, 255, 255, 0}, gw[4] = {0};
  if (opt_get(reply.options, opts_len, OPT_SUBNET_MASK, mask, 4) < 4)
    memset(mask, 0, sizeof(mask));
  if (opt_get(reply.options, opts_len, OPT_ROUTER, gw, 4) < 4)
    memset(gw, 0, sizeof(gw));

  *ip_be = reply.yiaddr;
  memcpy(mask_be, mask, 4);
  memcpy(gw_be, gw, 4);
  return 0;
}

static uint8_t mask_to_prefix(uint32_t mask_be) {
  uint32_t m = ntohl(mask_be);
  uint8_t bits = 0;
  while (m & 0x80000000u) {
    bits++;
    m <<= 1;
  }
  return bits;
}

/* Put the lease on the interface and make the LAN reachable even while a VPN
 * owns the default route. No default route: the host keeps its own uplink. */
static int lease_apply(const char *iface, uint32_t ip_be, uint32_t mask_be) {
  uint8_t prefix = mask_to_prefix(mask_be);
  if (prefix == 0)
    return -1;

  ds_nl_ctx_t *ctx = ds_nl_open();
  if (!ctx)
    return -1;
  int rc = ds_nl_add_addr4(ctx, iface, ip_be, prefix);
  if (rc < 0 && rc != -EEXIST)
    ds_warn("[NET] Host LAN: cannot add the lease address");
  ds_nl_add_rule4(ctx, 0, 0, ip_be & mask_be, prefix, 254 /* main table */,
                  DS_HOSTLAN_RULE_PRIO);
  ds_nl_close(ctx);
  return 0;
}

static int iface_mac(const char *iface, uint8_t mac[6]) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
  if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
    close(fd);
    return -1;
  }
  memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
  close(fd);
  return 0;
}

/* Ask for a lease on the interface, put it on and record it. One DISCOVER round
 * per try: a gateway answers in milliseconds, so a short window plus a retry
 * beats a long single wait. Returns 0 when the host now has an address. */
static int lease_take(int sock, const struct hostlan_intent *in) {
  unsigned ifindex = if_nametoindex(in->iface);
  if (!ifindex)
    return -1;

  struct sockaddr_ll sa;
  memset(&sa, 0, sizeof(sa));
  sa.sll_family = AF_PACKET;
  sa.sll_protocol = htons(ETH_P_IP);
  sa.sll_ifindex = (int)ifindex;
  if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    return -1;

  uint8_t mac[6];
  if (iface_mac(in->iface, mac) < 0)
    return -1;

  for (int try = 0; try < HOSTLAN_ACQUIRE_TRIES; try++) {
    uint32_t ip = 0, mask = 0, gw = 0;
    if (dhcp_acquire(sock, (int)ifindex, mac, &ip, &mask, &gw) == 0 && ip != 0) {
      uint8_t prefix = mask_to_prefix(mask);
      if (prefix > 0 && lease_apply(in->iface, ip, mask) == 0) {
        lease_write(ip, prefix, gw);
        char ip_str[INET_ADDRSTRLEN] = "?", gw_str[INET_ADDRSTRLEN] = "none";
        inet_ntop(AF_INET, &ip, ip_str, sizeof(ip_str));
        if (gw)
          inet_ntop(AF_INET, &gw, gw_str, sizeof(gw_str));
        ds_log("[NET] Host LAN: %s is %s/%u via %s", in->iface, ip_str,
               (unsigned)prefix, gw_str);
        return 0;
      }
    }
    if (try + 1 < HOSTLAN_ACQUIRE_TRIES)
      usleep(HOSTLAN_ACQUIRE_GAP_MS * 1000);
  }
  lease_clear();
  return -1;
}

/* lease_take with a socket of its own. */
static int lease_take_once(const struct hostlan_intent *in) {
  int sock = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_IP));
  if (sock < 0) {
    ds_warn("[NET] Host LAN: packet socket: %s", strerror(errno));
    return -1;
  }
  int rc = lease_take(sock, in);
  close(sock);
  return rc;
}

int ds_hostlan_dhcp_run(void) {
  struct hostlan_intent in;
  if (!intent_read(&in))
    return 1;

  int sock = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_IP));
  if (sock < 0) {
    ds_warn("[NET] Host LAN: packet socket: %s", strerror(errno));
    return 1;
  }

  for (;;) {
    if (!intent_read(&in))
      break; /* net leave removed the intent */

    if (!if_nametoindex(in.iface)) {
      /* The veth is gone; a monitor rebuilds it and we pick it up again. */
      sleep(HOSTLAN_RETRY_SEC);
      continue;
    }

    if (lease_take(sock, &in) == 0) {
      sleep(HOSTLAN_RENEW_MAX_SEC);
      continue;
    }
    sleep(HOSTLAN_RETRY_SEC);
  }

  close(sock);
  return 0;
}

/* The app owns the saved list, the kernel only gets rules derived from it.
 * Boot restores them before the app is open, so this reads the same file and
 * writes the same chains as HostNetworkManager. */
static int pf_sh(const char *fmt, ...) {
  char cmd[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cmd, sizeof(cmd), fmt, ap);
  va_end(ap);
  char *const argv[] = {"sh", "-c", cmd, NULL};
  return run_command_quiet(argv);
}

static int pf_enabled(void) {
  char path[PATH_MAX], buf[128] = {0};
  net_file(HOSTLAN_FORWARD_SWITCH, path, sizeof(path));
  if (read_file(path, buf, sizeof(buf)) <= 0)
    return 1;
  const char *value = strchr(buf, '=');
  if (!value)
    return 1;
  value++;
  while (*value == ' ')
    value++;
  if (*value == '0' || strncmp(value, "off", 3) == 0 ||
      strncmp(value, "false", 5) == 0 || strncmp(value, "no", 2) == 0)
    return 0;
  return 1;
}

static int all_digits(const char *text) {
  if (!*text)
    return 0;
  for (; *text; text++)
    if (*text < '0' || *text > '9')
      return 0;
  return 1;
}

/* "8080" or "2000:2010" to (first, count). A legacy hyphen span still loads. */
static int parse_span(const char *text, int *first, int *count) {
  char copy[32];
  if (!text || strlen(text) >= sizeof(copy))
    return -1;
  snprintf(copy, sizeof(copy), "%s", text);

  char *end = strpbrk(copy, ":-");
  if (end)
    *end++ = '\0';
  if (!all_digits(copy))
    return -1;
  long from = strtol(copy, NULL, 10);
  long to = from;
  if (end) {
    if (!all_digits(end))
      return -1;
    to = strtol(end, NULL, 10);
  }
  if (from < 1 || to < from || to > 65535)
    return -1;
  *first = (int)from;
  *count = (int)(to - from + 1);
  return 0;
}

static void pf_ensure_chains(void) {
  pf_sh("iptables -w -t nat -N " PF_CHAIN_NAT " 2>/dev/null");
  pf_sh("iptables -w -t nat -N " PF_CHAIN_LOCAL " 2>/dev/null");
  pf_sh("iptables -w -N " PF_CHAIN_FWD " 2>/dev/null");
  pf_sh("iptables -w -t nat -C PREROUTING -j " PF_CHAIN_NAT " >/dev/null 2>&1 || "
        "iptables -w -t nat -I PREROUTING 1 -j " PF_CHAIN_NAT);
  pf_sh("iptables -w -t nat -C OUTPUT -j " PF_CHAIN_LOCAL " >/dev/null 2>&1 || "
        "iptables -w -t nat -I OUTPUT 1 -j " PF_CHAIN_LOCAL);
  pf_sh("iptables -w -C FORWARD -j " PF_CHAIN_FWD " >/dev/null 2>&1 || "
        "iptables -w -I FORWARD 1 -j " PF_CHAIN_FWD);
}

static void pf_flush_chains(void) {
  pf_sh("iptables -w -t nat -F " PF_CHAIN_NAT " 2>/dev/null");
  pf_sh("iptables -w -t nat -F " PF_CHAIN_LOCAL " 2>/dev/null");
  pf_sh("iptables -w -F " PF_CHAIN_FWD " 2>/dev/null");
}

static void pf_subnet(char *out, size_t size) {
  out[0] = '\0';
  struct hostlan_lease lease;
  if (!lease_read(&lease) || !lease.ip_be || !lease.mask_be)
    return;
  uint32_t net_be = lease.ip_be & lease.mask_be;
  uint8_t prefix = mask_to_prefix(lease.mask_be);
  snprintf(out, size, "%u.%u.%u.%u/%u", (net_be >> 24) & 0xFF,
           (net_be >> 16) & 0xFF, (net_be >> 8) & 0xFF, net_be & 0xFF,
           (unsigned)prefix);
}

/* All values reaching these commands were parsed as a protocol, port or IPv4
 * address. Nothing from the note column is ever used here. */
static void pf_install(const char *proto, int host_port, int host_count,
                       const char *ip, int container_port) {
  char dport[32], dest[32], inside[32];
  if (host_count > 1) {
    snprintf(dport, sizeof(dport), "%d:%d", host_port, host_port + host_count - 1);
    snprintf(dest, sizeof(dest), "%d-%d", container_port,
             container_port + host_count - 1);
    snprintf(inside, sizeof(inside), "%d:%d", container_port,
             container_port + host_count - 1);
  } else {
    snprintf(dport, sizeof(dport), "%d", host_port);
    snprintf(dest, sizeof(dest), "%d", container_port);
    snprintf(inside, sizeof(inside), "%d", container_port);
  }

  pf_sh("iptables -w -t nat -A " PF_CHAIN_NAT " -p %s --dport %s "
        "-m comment --comment " PF_TAG " -j DNAT --to-destination %s:%s",
        proto, dport, ip, dest);
  pf_sh("iptables -w -A " PF_CHAIN_FWD " -p %s -d %s --dport %s "
        "-m comment --comment " PF_TAG " -j ACCEPT", proto, ip, inside);
  /* A browser on the phone sends to its own LAN address, which never passes
   * PREROUTING. Loopback is excluded: its reply would come back to 127.0.0.1. */
  pf_sh("iptables -w -t nat -A " PF_CHAIN_LOCAL
        " -m addrtype --dst-type LOCAL ! -d 127.0.0.0/8 -p %s --dport %s "
        "-m comment --comment " PF_TAG " -j DNAT --to-destination %s:%s",
        proto, dport, ip, dest);
}

int ds_hostnet_forwards_apply(void) {
  pf_ensure_chains();

  if (!pf_enabled()) {
    pf_flush_chains();
    ds_log("[NET] Port forwarding is off; rules cleared.");
    return 0;
  }

  char path[PATH_MAX], buf[16384];
  net_file(HOSTLAN_FORWARDS, path, sizeof(path));
  if (read_file(path, buf, sizeof(buf)) <= 0) {
    pf_flush_chains();
    return 0;
  }

  pf_flush_chains();
  char subnet[32];
  pf_subnet(subnet, sizeof(subnet));

  int applied = 0;
  char *save_line = NULL;
  for (char *line = strtok_r(buf, "\n", &save_line); line;
       line = strtok_r(NULL, "\n", &save_line)) {
    while (*line == ' ' || *line == '\r')
      line++;
    if (!*line || *line == '#')
      continue;

    /* The note column is deliberately not read. Splitting a copy keeps the
     * outer line iterator intact. */
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", line);
    char *fields[6] = {0};
    int n = 0;
    char *save_field = NULL;
    for (char *f = strtok_r(copy, "|", &save_field); f && n < 6;
         f = strtok_r(NULL, "|", &save_field))
      fields[n++] = f;
    if (n != 5 && n != 6)
      continue;

    if (strcmp(fields[0], "tcp") != 0 && strcmp(fields[0], "udp") != 0)
      continue;
    int host_port = 0, host_count = 0, container_port = 0, container_count = 0;
    if (parse_span(fields[1], &host_port, &host_count) != 0 ||
        parse_span(fields[4], &container_port, &container_count) != 0)
      continue;
    if (container_count > 1 && container_count != host_count)
      continue;

    struct in_addr addr;
    char ip[INET_ADDRSTRLEN];
    if (inet_pton(AF_INET, fields[3], &addr) != 1)
      continue;
    if (!inet_ntop(AF_INET, &addr, ip, sizeof(ip)))
      continue;

    pf_install(fields[0], host_port, host_count, ip, container_port);
    applied++;
  }

  if (subnet[0]) {
    pf_sh("iptables -w -t nat -C POSTROUTING -s %s ! -d %s "
          "-m comment --comment " PF_TAG ":masq -j MASQUERADE >/dev/null 2>&1 || "
          "iptables -w -t nat -A POSTROUTING -s %s ! -d %s "
          "-m comment --comment " PF_TAG ":masq -j MASQUERADE",
          subnet, subnet, subnet, subnet);
  }

  /* Android's tethering rules drop forwarded packets even after DNAT. */
  for (int i = 0; i < 8; i++)
    if (pf_sh("iptables -w -D tetherctrl_FORWARD -j DROP 2>/dev/null") != 0)
      break;

  ds_log("[NET] Port forwards: %d rule%s from disk", applied,
         applied == 1 ? "" : "s");
  return applied;
}

/* Join / leave ------------------------------------------------------------ */

int ds_hostlan_join(const char *bridge) {
  struct hostlan_intent in;
  memset(&in, 0, sizeof(in));
  snprintf(in.bridge, sizeof(in.bridge), "%s",
           (bridge && *bridge) ? bridge : DS_HOSTLAN_BRIDGE);
  snprintf(in.iface, sizeof(in.iface), "%s", DS_HOSTLAN_IFACE);
  snprintf(in.peer, sizeof(in.peer), "%s", DS_HOSTLAN_PEER);

  if (intent_write(&in) != 0) {
    ds_error("[NET] Host LAN: cannot write %s", HOSTLAN_CONF);
    return 1;
  }

  int lock = state_lock();
  if (lock >= 0) {
    ds_nl_ctx_t *ctx = ds_nl_open();
    if (!ctx || links_ensure(ctx, &in) < 0) {
      if (ctx)
        ds_nl_close(ctx);
      state_unlock(lock);
      ds_error("[NET] Host LAN: cannot plug into %s", in.bridge);
      return 1;
    }
    ds_nl_close(ctx);
    state_unlock(lock);
  }

  /* Take the lease here, not in the keeper: whoever ran join wants an address
   * back, and waiting a keeper's first iteration just to learn it is a
   * second of nothing happening. The keeper then only has to renew. */
  if (lease_take_once(&in) != 0)
    ds_warn("[NET] Host LAN: no lease yet, the keeper will keep asking");

  keeper_spawn();
  ds_log("[NET] Host LAN: joined %s as %s", in.bridge, in.iface);
  return 0;
}

int ds_hostlan_leave(void) {
  struct hostlan_intent in;
  if (!intent_read(&in)) {
    ds_log("[NET] Host LAN: not joined");
    return 0;
  }

  int lock = state_lock();
  keeper_stop();
  links_teardown(&in);

  struct hostlan_lease lease;
  if (lease_read(&lease)) {
    ds_nl_ctx_t *ctx = ds_nl_open();
    if (ctx) {
      uint8_t prefix = mask_to_prefix(lease.mask_be);
      ds_nl_del_rule4(ctx, 0, 0, lease.ip_be & lease.mask_be, prefix,
                      254 /* main table */, DS_HOSTLAN_RULE_PRIO);
      ds_nl_close(ctx);
    }
  }

  char path[PATH_MAX];
  net_file(HOSTLAN_CONF, path, sizeof(path));
  unlink(path);
  lease_clear();
  state_unlock(lock);

  ds_log("[NET] Host LAN: left %s", in.bridge);
  return 0;
}

/* Status ------------------------------------------------------------------ */

int ds_hostlan_status(int json) {
  struct hostlan_intent in;
  int joined = intent_read(&in);
  int bridge_up = 0, iface_up = 0, peer_up = 0, dhcp = keeper_alive();
  struct hostlan_lease lease;
  int have_lease = joined && lease_read(&lease);

  /* What the host could plug into, whether or not it is plugged in now. The
   * app needs this before join: an off switch with nothing to turn on and an
   * off switch with a LAN waiting for it are not the same card. */
  struct lan_bridge lans[8];
  int lan_count = lan_bridge_scan(lans, 8);

  if (joined) {
    ds_nl_ctx_t *ctx = ds_nl_open();
    if (ctx) {
      bridge_up = link_up(ctx, in.bridge);
      iface_up = link_up(ctx, in.iface);
      peer_up = link_up(ctx, in.peer);
      ds_nl_close(ctx);
    }
  }

  char ip[INET_ADDRSTRLEN] = "", gw[INET_ADDRSTRLEN] = "";
  uint8_t prefix = 0;
  if (have_lease) {
    inet_ntop(AF_INET, &lease.ip_be, ip, sizeof(ip));
    inet_ntop(AF_INET, &lease.gw_be, gw, sizeof(gw));
    prefix = mask_to_prefix(lease.mask_be);
  }

  if (json) {
    int first = 1;
    printf("{");
    ds_json_int("joined", joined, &first);
    ds_json_str("bridge", joined ? in.bridge : "", &first);
    ds_json_int("bridge_up", bridge_up, &first);
    ds_json_str("iface", joined ? in.iface : "", &first);
    ds_json_int("iface_up", iface_up, &first);
    ds_json_int("peer_up", peer_up, &first);
    ds_json_str("ip", ip, &first);
    ds_json_int("prefix", prefix, &first);
    ds_json_str("gateway", gw, &first);
    ds_json_int("dhcp", dhcp, &first);
    printf("%s\"bridges\":[", first ? "" : ",");
    for (int i = 0; i < lan_count; i++)
      printf("%s{\"name\":\"%s\",\"members\":%d}", i ? "," : "",
             lans[i].name, lans[i].members);
    printf("]}\n");
    return 0;
  }

  if (!joined) {
    printf("Host LAN : not joined\n");
    if (lan_count == 0) {
      printf("  bridges: none (start a gateway container first)\n");
    } else {
      printf("  bridges: %d available\n", lan_count);
      for (int i = 0; i < lan_count; i++)
        printf("    %-8s %d member%s\n", lans[i].name, lans[i].members,
               lans[i].members == 1 ? "" : "s");
    }
    return 0;
  }
  printf("Host LAN : %s\n", in.bridge);
  printf("  bridge : %s\n", bridge_up ? "up" : "missing");
  printf("  veth   : %s/%s %s\n", in.iface, in.peer,
         (iface_up && peer_up) ? "up" : "missing");
  printf("  address: %s\n", have_lease ? ip : "none");
  if (have_lease) {
    printf("  prefix : %u\n", (unsigned)prefix);
    printf("  gateway: %s\n", gw[0] ? gw : "none");
  }
  printf("  lease  : %s\n", dhcp ? "keeper running" : "keeper not running");
  return 0;
}

/* Reconcile --------------------------------------------------------------- */

int ds_hostlan_configured(void) {
  struct hostlan_intent in;
  return intent_read(&in);
}

void ds_hostlan_reconcile(void) {
  struct hostlan_intent in;
  if (!intent_read(&in))
    return;

  int lock = state_lock();
  if (lock < 0)
    return; /* another monitor is already on it */

  ds_nl_ctx_t *ctx = ds_nl_open();
  if (!ctx) {
    state_unlock(lock);
    return;
  }

  /* While the gateway is down its bridge is gone; there is nothing to plug
   * into, and re-creating it here would fight the gateway for ownership. */
  if (link_up(ctx, in.bridge) &&
      (!link_up(ctx, in.iface) || !link_up(ctx, in.peer)))
    links_ensure(ctx, &in);

  ds_nl_close(ctx);
  if (!keeper_alive())
    keeper_spawn();
  state_unlock(lock);
}
