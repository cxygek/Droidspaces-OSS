/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_MONITOR_MONITOR_H
#define DS_MONITOR_MONITOR_H

/* monitor.c */
/* What a monitor calls itself, and what the host's peer checks look for. */
#define DS_MONITOR_PROC "[ds-monitor]"

void ds_monitor_run(struct ds_config *cfg, int sync_pipe_write);

/* session.c, the monitor side. A monitor hosts the container's terminal
 * sessions: it answers on droidspaces/<name>/session, forks a keeper per
 * request, and takes them all down again when this boot's init is gone. */
void ds_session_host_start(const char *container_name);
int ds_session_host_fd(void);
void ds_session_host_accept(void);
void ds_session_host_reap(void);
void ds_session_host_end_all(void);

#endif /* DS_MONITOR_MONITOR_H */
