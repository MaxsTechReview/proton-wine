/*
 * Shared memory utility functions
 *
 * Copyright (C) 2018 Zebediah Figura
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_SERVER_SHM_UTILS_H
#define __WINE_SERVER_SHM_UTILS_H

#ifdef __ANDROID__

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* WinNative: Bionic has no working shm_open/shm_unlink. Redirect to plain
 * file I/O under a guaranteed-writable directory.
 *
 * CRITICAL: every Wine process in the same container MUST resolve to the
 * SAME directory. Otherwise cross-process ESync events fail silently
 * (services.exe waits at path A, rpcss.exe signals at path B → 30-second
 * RpcSs timeout, no shell services, no start menu).
 *
 * Modern Android (14+) denies app-uid writes to /data/local/tmp, so we
 * CANNOT use that. We use $WINEPREFIX which is inherited by every Wine
 * process in a container — guaranteed identical and always writable by
 * the app's own UID (it's inside /data/data/<app>/files/... territory).
 *
 * Priority:
 *   1. $WINEPREFIX/.esync-shm-dir      — primary, deterministic
 *   2. $WINEPREFIX/..                  — one dir up, for wineserver before
 *                                        the prefix itself exists
 *   3. $HOME                           — fallback if $WINEPREFIX isn't set
 *   4. $TMPDIR                         — last resort if caller sets it
 * "/data/local/tmp" is NOT used — fails with EACCES on Android 14+.
 * "/tmp" is never used — doesn't exist on Android.                       */
static inline const char *winnative_shm_dir(void) {
    static char cached_path[512] = {0};
    static int probed = 0;
    static char prefix_shm[512];
    static char prefix_parent[512];
    const char *candidates[6];
    int n = 0;
    const char *env;
    int i;
    size_t len;

    if (probed) return cached_path[0] ? cached_path : NULL;
    probed = 1;

    if ((env = getenv("WINEPREFIX")) && env[0]) {
        snprintf(prefix_shm, sizeof(prefix_shm), "%s/.esync-shm-dir", env);
        candidates[n++] = prefix_shm;
        snprintf(prefix_parent, sizeof(prefix_parent), "%s/..", env);
        candidates[n++] = prefix_parent;
    }
    if ((env = getenv("HOME")) && env[0])     candidates[n++] = env;
    if ((env = getenv("TMPDIR")) && env[0])   candidates[n++] = env;
    candidates[n] = NULL;

    for (i = 0; candidates[i]; i++) {
        mkdir(candidates[i], 0700);
        if (access(candidates[i], R_OK | W_OK | X_OK) == 0) {
            len = strlen(candidates[i]);
            if (len >= sizeof(cached_path)) len = sizeof(cached_path) - 1;
            memcpy(cached_path, candidates[i], len);
            cached_path[len] = '\0';
            return cached_path;
        }
    }
    return NULL;
}

static inline int shm_open(const char *name, int oflag, mode_t mode) {
    const char *tmpdir = winnative_shm_dir();
    char *fname = NULL;
    int fd;

    if (!tmpdir) { errno = ENOSPC; return -1; }

    asprintf(&fname, "%s/%s", tmpdir, name);
    if (!fname) { errno = ENOMEM; return -1; }
    fd = open(fname, oflag, mode);
    free(fname);
    return fd;
}

static inline int shm_unlink(const char *name) {
    const char *tmpdir = winnative_shm_dir();
    char *fname = NULL;
    int ret;

    if (!tmpdir) { errno = ENOENT; return -1; }

    asprintf(&fname, "%s/%s", tmpdir, name);
    if (!fname) { errno = ENOMEM; return -1; }
    ret = unlink(fname);
    free(fname);
    return ret;
}

#endif /* __ANDROID__ */

#endif /* __WINE_SERVER_SHM_UTILS_H */
