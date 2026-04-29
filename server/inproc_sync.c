/*
 * In-process synchronization primitives
 *
 * Copyright (C) 2021-2022 Elizabeth Figura for CodeWeavers
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

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "file.h"
#include "handle.h"
#include "request.h"
#include "thread.h"
#include "user.h"

#include "fsync.h"
#include "esync.h"

#include "ntsync_tmp.h"

#ifdef HAVE_SYS_EVENTFD_H
# include <sys/eventfd.h>
#endif

#ifdef NTSYNC_IOC_EVENT_READ

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

int get_inproc_device_fd(void)
{
    static int fd = -2;
    if (fd == -2)
    {
        /* Sync-mode selection. Strict opt-in semantics — we never enable
         * a sync backend the user did not explicitly request:
         *  - WINENTSYNC=1: probe /dev/ntsync. If it opens, NTSync wins
         *                  (client disables ESync). If it doesn't, fall
         *                  through — caller will check WINEESYNC=1 for
         *                  the ESync fallback.
         *  - WINENTSYNC unset/0: skip the probe entirely.
         *  - WINEFSYNC=1: server still consults do_fsync() for futex_waitv
         *                 if NTSync isn't taking the slot. Independent flag.
         *  - PROTON_NO_NTSYNC=1: legacy hard block on NTSync regardless of
         *                       WINENTSYNC. Kept for upstream Proton parity.
         * If neither WINENTSYNC nor WINEESYNC is set, the client uses
         * server-side sync only. See do_esync() in dlls/ntdll/unix/esync.c. */
        const char *ntsync_env = getenv( "WINENTSYNC" );
        int ntsync_requested  = ntsync_env && atoi( ntsync_env ) > 0;
        int ntsync_blocked    = getenv( "PROTON_NO_NTSYNC" ) && atoi( getenv( "PROTON_NO_NTSYNC" ) );

        if (ntsync_requested && !ntsync_blocked)
            fd = open( "/dev/ntsync", O_CLOEXEC | O_RDONLY );
        else
            fd = -1;

        if (fd >= 0)
        {
            do_fsync_cached = 0;
            fprintf( stderr, "ntsync: up and running (WINENTSYNC=1).\n" );
        }
        else if (ntsync_requested && ntsync_blocked)
            fprintf( stderr, "wineserver: WINENTSYNC=1 but PROTON_NO_NTSYNC blocks it — checking ESync/FSync fallback.\n" );
        else if (ntsync_requested)
            fprintf( stderr, "wineserver: WINENTSYNC=1 but /dev/ntsync unavailable — checking ESync/FSync fallback.\n" );
        else if (do_fsync()) fd = FSYNC_USED_BY_SERVER;
        /* WinNative: do NOT route server-internal sync (master socket fd,
         * thread alert events, internal events) through ESync. ESync is
         * for client-facing objects only — `create_esync` (server/esync.c)
         * has its own correct init order and shm slot management. Routing
         * server-internal sync here created a chicken-and-egg bootstrap:
         * acquire_lock() needs the master socket's internal sync object
         * before esync_init() can run, so esync_alloc_shm() returned the
         * defensive slot 0 sentinel and every subsequent server-internal
         * sync collided on that same slot, corrupting state and tripping
         * the inproc_sync_signal() type assertion later. The init_first_thread
         * reply path (server/thread.c) still sends ESYNC_USED_BY_SERVER +
         * the shm fd to clients so client-side ESync remains active. */
    }
    return fd;
}

struct inproc_sync
{
    struct object          obj;  /* object header */
    enum inproc_sync_type  type;
    int                    fd;
    /* Index into the ESync shm region. Valid only when do_esync() is in
     * effect at create time and 'type' is one of the ESYNC_* values; the
     * NTSync and FSync paths leave it as 0 and never consult it. */
    unsigned int           esync_shm_idx;
    struct list            entry;
};

static struct list inproc_mutexes = LIST_INIT( inproc_mutexes );

static void inproc_sync_dump( struct object *obj, int verbose );
static int inproc_sync_signal( struct object *obj, unsigned int access, int signal );
static void inproc_sync_destroy( struct object *obj );

static const struct object_ops inproc_sync_ops =
{
    sizeof(struct inproc_sync), /* size */
    &no_type,                   /* type */
    inproc_sync_dump,           /* dump */
    no_add_queue,               /* add_queue */
    NULL,                       /* remove_queue */
    NULL,                       /* signaled */
    NULL,                       /* satisfied */
    inproc_sync_signal,         /* signal */
    no_get_fd,                  /* get_fd */
    default_get_sync,           /* get_sync */
    default_map_access,         /* map_access */
    default_get_sd,             /* get_sd */
    default_set_sd,             /* set_sd */
    default_get_full_name,      /* get_full_name */
    no_lookup_name,             /* lookup_name */
    directory_link_name,        /* link_name */
    default_unlink_name,        /* unlink_name */
    no_open_file,               /* open_file */
    no_kernel_obj_list,         /* get_kernel_obj_list */
    no_close_handle,            /* close_handle */
    inproc_sync_destroy,        /* destroy */
};

int get_inproc_sync_fd( struct inproc_sync *sync )
{
    if (!sync) return -1;
    return sync->fd;
}

struct inproc_sync *create_inproc_internal_sync( int manual, int signaled )
{
    struct ntsync_event_args args = {.signaled = signaled, .manual = manual};
    struct inproc_sync *event;

    if (!(event = alloc_object( &inproc_sync_ops ))) return NULL;
    event->esync_shm_idx = 0;
    if (do_fsync())
    {
        event->type = manual ? FSYNC_MANUAL_SERVER : FSYNC_AUTO_SERVER;
        event->fd = fsync_alloc_shm( signaled, 0xdeadbeef );
    }
    else if (do_esync())
    {
        event->type = manual ? ESYNC_MANUAL_SERVER : ESYNC_AUTO_SERVER;
        event->fd = eventfd( signaled ? 1 : 0, EFD_CLOEXEC | EFD_NONBLOCK );
        if (event->fd != -1)
            event->esync_shm_idx = esync_alloc_shm( event->fd, event->type, signaled, 0 );
    }
    else
    {
        event->type = INPROC_SYNC_INTERNAL;
        event->fd   = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_EVENT, &args );
    }
    list_init( &event->entry );

    if (event->fd == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( event );
        return NULL;
    }
    return event;
}

struct inproc_sync *create_inproc_event_sync( int manual, int signaled )
{
    struct ntsync_event_args args = {.signaled = signaled, .manual = manual};
    struct inproc_sync *event;

    if (!(event = alloc_object( &inproc_sync_ops ))) return NULL;
    event->esync_shm_idx = 0;
    if (do_fsync())
    {
        event->type = manual ? FSYNC_MANUAL_EVENT : FSYNC_AUTO_EVENT;
        event->fd = fsync_alloc_shm( signaled, 0xdeadbeef );
    }
    else if (do_esync())
    {
        event->type = manual ? ESYNC_MANUAL_EVENT : ESYNC_AUTO_EVENT;
        event->fd = eventfd( signaled ? 1 : 0, EFD_CLOEXEC | EFD_NONBLOCK );
        if (event->fd != -1)
            event->esync_shm_idx = esync_alloc_shm( event->fd, event->type, signaled, 0 );
    }
    else
    {
        event->type = INPROC_SYNC_EVENT;
        event->fd   = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_EVENT, &args );
    }
    list_init( &event->entry );

    if (event->fd == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( event );
        return NULL;
    }
    return event;
}

struct inproc_sync *create_inproc_mutex_sync( thread_id_t owner, unsigned int count )
{
    struct ntsync_mutex_args args = {.owner = owner, .count = count};
    struct inproc_sync *mutex;

    if (!(mutex = alloc_object( &inproc_sync_ops ))) return NULL;
    mutex->esync_shm_idx = 0;
    if (do_fsync())
    {
        mutex->type = FSYNC_MUTEX;
        mutex->fd = fsync_alloc_shm( owner, count );
    }
    else if (do_esync())
    {
        /* eventfd starts signaled iff the mutex is unowned. The shm slot
         * carries (owner_tid, count) per esync_alloc_shm()'s mutex
         * convention. */
        mutex->type = ESYNC_MUTEX;
        mutex->fd = eventfd( owner == 0 ? 1 : 0, EFD_CLOEXEC | EFD_NONBLOCK );
        if (mutex->fd != -1)
            mutex->esync_shm_idx = esync_alloc_shm( mutex->fd, mutex->type, owner, count );
    }
    else
    {
        mutex->type = INPROC_SYNC_MUTEX;
        mutex->fd   = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_MUTEX, &args );
    }
    list_add_tail( &inproc_mutexes, &mutex->entry );

    if (mutex->fd == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( mutex );
        return NULL;
    }
    return mutex;
}

struct inproc_sync *create_inproc_semaphore_sync( unsigned int initial, unsigned int max )
{
    struct ntsync_sem_args args = {.count = initial, .max = max};
    struct inproc_sync *sem;

    if (!(sem = alloc_object( &inproc_sync_ops ))) return NULL;
    sem->esync_shm_idx = 0;

    if (do_fsync())
    {
        sem->type = FSYNC_SEMAPHORE;
        sem->fd = fsync_alloc_shm( initial, max );
    }
    else if (do_esync())
    {
        sem->type = ESYNC_SEMAPHORE;
        sem->fd = eventfd( initial, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE );
        if (sem->fd != -1)
            sem->esync_shm_idx = esync_alloc_shm( sem->fd, sem->type, initial, max );
    }
    else
    {
        sem->type = INPROC_SYNC_SEMAPHORE;
        sem->fd   = ioctl( get_inproc_device_fd(), NTSYNC_IOC_CREATE_SEM, &args );
    }
    list_init( &sem->entry );

    if (sem->fd == -1)
    {
        set_error( STATUS_TOO_MANY_OPENED_FILES );
        release_object( sem );
        return NULL;
    }
    return sem;
}

static void inproc_sync_dump( struct object *obj, int verbose )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    fprintf( stderr, "Inproc sync type=%d, fd=%d\n", sync->type, sync->fd );
}

void signal_inproc_sync( struct inproc_sync *sync )
{
    __u32 count;
    if (debug_level) fprintf( stderr, "set_inproc_event %d\n", sync->fd );
    if (do_fsync()) fsync_set_event( sync->fd );
    else if (do_esync()) esync_inproc_set_event( sync->fd, sync->esync_shm_idx, sync->type );
    else            ioctl( sync->fd, NTSYNC_IOC_EVENT_SET, &count );
}

void reset_inproc_sync( struct inproc_sync *sync )
{
    __u32 count;
    if (debug_level) fprintf( stderr, "reset_inproc_event %d\n", sync->fd );
    if (do_fsync()) fsync_reset_event( sync->fd );
    else if (do_esync()) esync_inproc_reset_event( sync->fd, sync->esync_shm_idx, sync->type );
    else            ioctl( sync->fd, NTSYNC_IOC_EVENT_RESET, &count );
}

static int inproc_sync_signal( struct object *obj, unsigned int access, int signal )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );

    if (!do_fsync() && !do_esync())
        assert( sync->type == INPROC_SYNC_INTERNAL || sync->type == INPROC_SYNC_EVENT ); /* never called for mutex / semaphore */
    /* WinNative: when ESync is the active backend, sync->type holds an
     * enum esync_type value (ESYNC_AUTO_EVENT/MANUAL_EVENT/AUTO_SERVER/
     * MANUAL_SERVER) set by create_inproc_event_sync()/create_inproc_internal_sync().
     * The signal_inproc_sync()/reset_inproc_sync() dispatchers know how to
     * handle those, so just skip the NTSync-only assertion. */
    assert( signal == 0 || signal == 1 ); /* never called from signal_object */

    if (signal) signal_inproc_sync( sync );
    else reset_inproc_sync( sync );
    return 1;
}

static void inproc_sync_destroy( struct object *obj )
{
    struct inproc_sync *sync = (struct inproc_sync *)obj;
    assert( obj->ops == &inproc_sync_ops );
    list_remove( &sync->entry );
    if (do_fsync()) fsync_free_shm_idx( sync->fd );
    else            close( sync->fd );
}

void abandon_inproc_mutexes( thread_id_t tid )
{
    struct inproc_sync *mutex;

    if (do_fsync())
    {
        LIST_FOR_EACH_ENTRY( mutex, &inproc_mutexes, struct inproc_sync, entry )
            fsync_abandon_mutex( mutex->fd, tid );
        return;
    }

    LIST_FOR_EACH_ENTRY( mutex, &inproc_mutexes, struct inproc_sync, entry )
        ioctl( mutex->fd, NTSYNC_IOC_MUTEX_KILL, &tid );
}

static int get_obj_inproc_sync( struct object *obj, int *type )
{
    struct object *sync;
    int fd = -1;

    if (!(sync = get_obj_sync( obj ))) return -1;
    if (sync->ops == &inproc_sync_ops)
    {
        struct inproc_sync *inproc = (struct inproc_sync *)sync;
        *type = inproc->type;
        fd = inproc->fd;
    }

    release_object( sync );
    return fd;
}

#else /* NTSYNC_IOC_EVENT_READ */

int get_inproc_device_fd(void)
{
    return -1;
}

int get_inproc_sync_fd( struct inproc_sync *sync )
{
    return -1;
}

struct inproc_sync *create_inproc_internal_sync( int manual, int signaled )
{
    return NULL;
}

struct inproc_sync *create_inproc_event_sync( int manual, int signaled )
{
    return NULL;
}

struct inproc_sync *create_inproc_mutex_sync( thread_id_t owner, unsigned int count )
{
    return NULL;
}

struct inproc_sync *create_inproc_semaphore_sync( unsigned int initial, unsigned int max )
{
    return NULL;
}

void signal_inproc_sync( struct inproc_sync *sync )
{
}

void reset_inproc_sync( struct inproc_sync *sync )
{
}

void abandon_inproc_mutexes( thread_id_t tid )
{
}

static int get_obj_inproc_sync( struct object *obj, int *type )
{
    return -1;
}

#endif /* NTSYNC_IOC_EVENT_READ */

DECL_HANDLER(get_inproc_sync_fd)
{
    struct object *obj;
    int fd;

    if (!(obj = get_handle_obj( current->process, req->handle, 0, NULL ))) return;

    reply->access = get_handle_access( current->process, req->handle );

    if ((fd = get_obj_inproc_sync( obj, &reply->type )) < 0) set_error( STATUS_NOT_IMPLEMENTED );
    else if (do_fsync()) reply->fsync_shm_idx = fsync_grab_shm_idx( fd );
    else send_client_fd( current->process, fd, req->handle );

    release_object( obj );
}
