/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"

/* Workspace / Paths */

const char *get_workspace_dir(void) {
  return is_android() ? DS_WORKSPACE_ANDROID : DS_WORKSPACE_LINUX;
}

const char *get_pids_dir(void) {
  static char pids_path[PATH_MAX];
  snprintf(pids_path, sizeof(pids_path), "%s/%s", get_workspace_dir(),
           DS_PIDS_SUBDIR);
  return pids_path;
}

const char *get_net_dir(void) {
  static char net_path[PATH_MAX];
  snprintf(net_path, sizeof(net_path), "%s/%s", get_workspace_dir(),
           DS_NET_SUBDIR);
  return net_path;
}

const char *get_logs_dir(void) {
  static char logs_path[PATH_MAX];
  snprintf(logs_path, sizeof(logs_path), "%s/%s", get_workspace_dir(),
           DS_LOGS_SUBDIR);
  return logs_path;
}

const char *get_sessions_dir(void) {
  static char sessions_path[PATH_MAX];
  snprintf(sessions_path, sizeof(sessions_path), "%s/%s", get_workspace_dir(),
           DS_SESSIONS_SUBDIR);
  return sessions_path;
}

/* Self path
 *
 * /proc/self/exe is only trustworthy until the file it names is replaced. A
 * droidspaces update swaps the binary with mv, and from then on the symlink
 * reads "<path> (deleted)". Anything that exec's that string dies with ENOENT,
 * which is how a running monitor lost the ability to open terminals or restart
 * the DHCP keeper. Pinning the string at startup keeps it pointing at whatever
 * now lives at that path. */
static char g_self_path[PATH_MAX];

void ds_self_path_pin(void) {
  ssize_t n = readlink("/proc/self/exe", g_self_path, sizeof(g_self_path) - 1);
  g_self_path[n > 0 ? n : 0] = '\0';
}

const char *ds_self_path(void) {
  if (g_self_path[0])
    return g_self_path;
  /* Not pinned: read it now. A short-lived process is still clean, and a
   * stale answer is no worse than the symlink it replaces. */
  static char live[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", live, sizeof(live) - 1);
  live[n > 0 ? n : 0] = '\0';
  return live;
}

int ensure_workspace(void) {
  mkdir_p(get_workspace_dir(), 0755);
  mkdir_p(get_pids_dir(), 0755);
  mkdir_p(get_net_dir(), 0755);
  mkdir_p(get_logs_dir(), 0755);
  /* Root only: a session record names the container and the user of a live
   * root shell. */
  mkdir_p(get_sessions_dir(), 0700);

  return 0;
}

/* Relative-path resolution
 *
 * The daemon calls chdir("/") inside daemonize(), so any relative path
 * captured from the user's CWD must be made absolute BEFORE we reach the
 * daemonize()/reexec() boundary.  ds_resolve_argv_paths() is called once
 * in main() while CWD is still the user's directory.
 *
 * Strategy:
 *   1. Try realpath(3) - handles .., symlinks, and canonicalises the path.
 *      This works for paths that already exist on disk.
 *   2. For paths that do not exist yet (e.g. a new rootfs image being
 *      created), fall back to a plain cwd-join.  We still strip leading ./
 *      sequences so the result is always absolute. */

char *ds_resolve_path_arg(const char *path) {
  if (!path || !*path)
    return strdup("");

  /* A ~ still reaches us inside a comma list or an =value the shell did not
   * expand. */
  char home[PATH_MAX];
  if (path[0] == '~' && (path[1] == '/' || !path[1]) && getenv("HOME")) {
    snprintf(home, sizeof(home), "%s%s", getenv("HOME"), path + 1);
    path = home;
  }

  if (path[0] == '/') {
    char *res = strdup(path);
    for (size_t len = res ? strlen(res) : 0; len > 1 && res[len - 1] == '/';)
      res[--len] = '\0';
    return res;
  }

  /* Fast path: realpath handles .., symlinks, and validates existence. */
  char resolved[PATH_MAX];
  if (realpath(path, resolved))
    return strdup(resolved);

  /* Not there yet (a rootfs image about to be created, say): join it to the
   * cwd, minus any leading ./ noise. */
  const char *suffix = path;
  while (suffix[0] == '.' && suffix[1] == '/')
    suffix += 2;
  char cwd[PATH_MAX], *out = NULL;
  if (!getcwd(cwd, sizeof(cwd)))
    return strdup(path);
  if (!*suffix)
    return strdup(cwd);
  return asprintf(&out, "%s/%s", cwd, suffix) < 0 ? strdup(path) : out;
}

int is_subpath(const char *parent, const char *child) {
  char *real_parent = ds_resolve_path_arg(parent);
  char *real_child = ds_resolve_path_arg(child);

  if (!real_parent || !real_child || !real_parent[0] || !real_child[0]) {
    free(real_parent);
    free(real_child);
    return 0;
  }

  size_t len = strlen(real_parent);

  /* Special case for the root directory */
  if (len == 1 && real_parent[0] == '/') {
    free(real_parent);
    free(real_child);
    return 1;
  }

  int result = 0;
  if (strncmp(real_parent, real_child, len) == 0) {
    if (real_child[len] == '\0' || real_child[len] == '/')
      result = 1;
  }

  free(real_parent);
  free(real_child);
  return result;
}
