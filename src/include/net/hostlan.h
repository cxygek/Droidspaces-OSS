/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_NET_HOSTLAN_H
#define DS_NET_HOSTLAN_H

/*
 * The host's own cable into a container LAN.
 *
 * A gateway container (an OpenWrt image, typically) owns the segment: it
 * builds the bridge, runs the DHCP server and routes. This module plugs the
 * Android host into that same bridge with a veth pair, so the host can reach
 * every container by address and the containers can reach the host, without
 * the host owning a second identity per container.
 *
 * There is no daemon and no config file of knobs: join writes the intent (one
 * small file), a short-lived keeper renews the DHCP lease, and the container
 * monitors re-assert the cable from their heartbeat. Everything else is read
 * back from the kernel.
 */

#define DS_HOSTLAN_BRIDGE "ds-lan"
#define DS_HOSTLAN_IFACE "veth-host"
#define DS_HOSTLAN_PEER "veth-ds"

/* After the NAT routing rules (6090-6100) and before Android's VPN range
 * (10000): a joined LAN must stay reachable while a VPN owns the default
 * route, and the rule must never outrank the container subnet rules. */
#define DS_HOSTLAN_RULE_PRIO 6105

/* Plug the host into the LAN on bridge (NULL for DS_HOSTLAN_BRIDGE). Creates
 * the bridge and the veth pair when they are missing, persists the intent and
 * starts the lease keeper. Returns 0 on success. */
int ds_hostlan_join(const char *bridge);

/* Unplug and forget the intent. */
int ds_hostlan_leave(void);

/* Print the current state as JSON (json != 0) or as a short human report. */
int ds_hostlan_status(int json);

/* 1 when a join intent exists: the host is supposed to be on a LAN. Lets the
 * monitors skip the work entirely when nobody ever joined. */
int ds_hostlan_configured(void);

/* Re-assert the cable after a gateway restart, a lost veth or a dead lease
 * keeper. Idempotent and cheap; called from the container monitors. */
void ds_hostlan_reconcile(void);

/* Internal: the lease keeper, spawned by join as "net dhcp". */
int ds_hostlan_dhcp_run(void);

#endif /* DS_NET_HOSTLAN_H */
