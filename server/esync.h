/*
 * eventfd-based synchronization objects
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

#include <unistd.h>

extern int do_esync(void);
void esync_init(void);
int esync_create_fd( int initval, int flags );
void esync_wake_fd( int fd );
void esync_wake_up( struct object *obj );
void esync_clear( int fd );

/* WinNative: expose the eventfd-shm bootstrap for the ESYNC_USED_BY_SERVER
 * protocol path. esync_get_shm_fd() returns the fd that
 * dlls/ntdll/unix/server.c hands to the client at init_first_thread time so
 * the client can mmap the same memfd-backed region without going through
 * shm_open()/shm_utils. esync_alloc_shm() factors out the per-object shm
 * slot allocation that was inline in create_esync() so server-side
 * inproc-sync internal objects can reuse it. */
int esync_get_shm_fd(void);
unsigned int esync_alloc_shm( int fd, enum esync_type type, int initval, int max );

/* WinNative: primitive-arg variants of esync_set_event/esync_reset_event so
 * server/inproc_sync.c can drive eventfd-backed sync without owning a
 * struct esync. The full struct esync versions below stay for the existing
 * named-object path. */
void esync_inproc_set_event( int fd, unsigned int shm_idx, enum esync_type type );
void esync_inproc_reset_event( int fd, unsigned int shm_idx, enum esync_type type );

struct esync;

extern const struct object_ops esync_ops;
void esync_set_event( struct esync *esync );
void esync_reset_event( struct esync *esync );
void esync_abandon_mutexes( struct thread *thread );

struct esync *create_esync( struct object *root, const struct unicode_str *name,
                            unsigned int attr, int initval, int max, enum esync_type type,
                            const struct security_descriptor *sd );

/* WinNative: return the eventfd stored in a sync object (event_sync_ops).
 * Wrapper types (thread, process, queue, completion, console, device_manager,
 * timer, fd) route signaling through a struct object *sync field. On ESync
 * (no NTSync) that sync is an event_sync_ops carrying the shared eventfd.
 * Wrappers' get_esync_fd should return this fd so client and server agree
 * on the same kernel eventfd. Returns -1 if sync isn't event_sync_ops. */
int sync_get_esync_fd( struct object *sync, enum esync_type *type );
