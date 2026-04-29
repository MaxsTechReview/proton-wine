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

#include "config.h"


#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#ifdef HAVE_SYS_EVENTFD_H
# include <sys/eventfd.h>
#endif
#include <sys/mman.h>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#include <unistd.h>
#ifdef __linux__
# include <sys/syscall.h>
# include <linux/futex.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "handle.h"
#include "request.h"
#include "file.h"
#include "esync.h"
#include "fsync.h"
#ifdef __ANDROID__
#include "../android/shm_utils/shm_utils.h"
#endif

#ifdef HAVE_SYS_EVENTFD_H
/* File scope so esync_init() can clear it on init failure, downgrading the
 * whole process to server-side sync without leaving callers to spin in
 * esync_alloc_shm() against zeroed globals. */
static int do_esync_cached = -1;
#endif

int do_esync(void)
{
#ifdef HAVE_SYS_EVENTFD_H
    if (do_esync_cached == -1)
    {
        const char *env = getenv("WINEESYNC");
        do_esync_cached = env && atoi(env) > 0 && !do_fsync();
    }

    return do_esync_cached;
#else
    return 0;
#endif
}

static char shm_name[29];
static int shm_fd;
static off_t shm_size;
static void **shm_addrs;
static int shm_addrs_size;  /* length of the allocated shm_addrs array */
static long pagesize;
/* Set to 1 only after esync_init() has fully populated shm_name, shm_fd,
 * shm_size, and pagesize. esync_alloc_shm() refuses to run without this —
 * its grow loop assumes pagesize > 0 and would spin forever otherwise. */
static int is_esync_initialized;

static void shm_cleanup(void)
{
    close( shm_fd );
    if (shm_unlink( shm_name ) == -1)
        perror( "shm_unlink" );
}

void esync_init(void)
{
    struct stat st;

    /* Idempotent: open_master_socket() now calls us early (right after
     * create_server_dir() sets config_dir_fd) so the master socket fd's
     * internal sync object can route through esync. main.c still calls
     * esync_init() afterwards as a fallback; bail here so we don't double-
     * unlink/recreate the shm file. */
    if (is_esync_initialized) return;

    if (fstat( config_dir_fd, &st ) == -1)
        fatal_error( "cannot stat config dir\n" );

    if (st.st_ino != (unsigned long)st.st_ino)
        sprintf( shm_name, "/wine-%lx%08lx-esync", (unsigned long)((unsigned long long)st.st_ino >> 32), (unsigned long)st.st_ino );
    else
        sprintf( shm_name, "/wine-%lx-esync", (unsigned long)st.st_ino );

    shm_unlink( shm_name );

    shm_fd = shm_open( shm_name, O_RDWR | O_CREAT | O_EXCL, 0644 );
    if (shm_fd == -1)
    {
        perror( "shm_open" );
        fprintf( stderr, "esync: shm_open failed — disabling ESync, falling back to server-side sync.\n" );
#ifdef HAVE_SYS_EVENTFD_H
        do_esync_cached = 0;
#endif
        shm_name[0] = '\0';
        return;
    }

    pagesize = sysconf( _SC_PAGESIZE );
    if (pagesize <= 0)
    {
        fprintf( stderr, "esync: sysconf(_SC_PAGESIZE) returned %ld — disabling ESync.\n", pagesize );
        close( shm_fd );
        shm_unlink( shm_name );
        shm_fd = -1;
        pagesize = 0;
#ifdef HAVE_SYS_EVENTFD_H
        do_esync_cached = 0;
#endif
        shm_name[0] = '\0';
        return;
    }

    shm_addrs = calloc( 128, sizeof(shm_addrs[0]) );
    shm_addrs_size = 128;

    shm_size = pagesize;
    if (ftruncate( shm_fd, shm_size ) == -1)
    {
        perror( "ftruncate" );
        fprintf( stderr, "esync: initial ftruncate failed — disabling ESync.\n" );
        close( shm_fd );
        shm_unlink( shm_name );
        shm_fd = -1;
        shm_size = 0;
        pagesize = 0;
#ifdef HAVE_SYS_EVENTFD_H
        do_esync_cached = 0;
#endif
        shm_name[0] = '\0';
        return;
    }

    is_esync_initialized = 1;
    fprintf( stderr, "esync: up and running.\n" );

    atexit( shm_cleanup );
}

/* WinNative: expose the shm_fd so init_first_thread can hand it to clients
 * via send_client_fd() as part of the ESYNC_USED_BY_SERVER reply path. */
int esync_get_shm_fd(void)
{
    return shm_fd;
}

static struct list mutex_list = LIST_INIT(mutex_list);

struct esync
{
    struct object   obj;            /* object header */
    int             fd;             /* eventfd file descriptor */
    enum esync_type type;
    unsigned int    shm_idx;        /* index into the shared memory section */
    struct list     mutex_entry;    /* entry in the mutex list (if applicable) */
};

static void esync_dump( struct object *obj, int verbose );
static int esync_get_esync_fd( struct object *obj, enum esync_type *type );
static unsigned int esync_map_access( struct object *obj, unsigned int access );
static void esync_destroy( struct object *obj );

const struct object_ops esync_ops =
{
    sizeof(struct esync),      /* size */
    &no_type,                  /* type */
    esync_dump,                /* dump */
    no_add_queue,              /* add_queue */
    NULL,                      /* remove_queue */
    NULL,                      /* signaled */
    NULL,                      /* satisfied */
    no_signal,                 /* signal */
    no_get_fd,                 /* get_fd */
    default_get_sync,          /* get_sync */
    esync_map_access,          /* map_access */
    default_get_sd,            /* get_sd */
    default_set_sd,            /* set_sd */
    default_get_full_name,     /* get_full_name */
    no_lookup_name,            /* lookup_name */
    directory_link_name,       /* link_name */
    default_unlink_name,       /* unlink_name */
    no_open_file,              /* open_file */
    no_kernel_obj_list,        /* get_kernel_obj_list */
    no_close_handle,           /* close_handle */
    esync_destroy,             /* destroy */
    esync_get_esync_fd,        /* get_esync_fd */
};

static void esync_dump( struct object *obj, int verbose )
{
    struct esync *esync = (struct esync *)obj;
    assert( obj->ops == &esync_ops );
    fprintf( stderr, "esync fd=%d\n", esync->fd );
}

static int esync_get_esync_fd( struct object *obj, enum esync_type *type )
{
    struct esync *esync = (struct esync *)obj;
    *type = esync->type;
    return esync->fd;
}

static unsigned int esync_map_access( struct object *obj, unsigned int access )
{
    /* Sync objects have the same flags. */
    if (access & GENERIC_READ)    access |= STANDARD_RIGHTS_READ | EVENT_QUERY_STATE;
    if (access & GENERIC_WRITE)   access |= STANDARD_RIGHTS_WRITE | EVENT_MODIFY_STATE;
    if (access & GENERIC_EXECUTE) access |= STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE;
    if (access & GENERIC_ALL)     access |= STANDARD_RIGHTS_ALL | EVENT_QUERY_STATE | EVENT_MODIFY_STATE;
    return access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
}

static void esync_destroy( struct object *obj )
{
    struct esync *esync = (struct esync *)obj;
    if (esync->type == ESYNC_MUTEX)
        list_remove( &esync->mutex_entry );
    close( esync->fd );
}

static int type_matches( enum esync_type type1, enum esync_type type2 )
{
    return (type1 == type2) ||
           ((type1 == ESYNC_AUTO_EVENT || type1 == ESYNC_MANUAL_EVENT) &&
            (type2 == ESYNC_AUTO_EVENT || type2 == ESYNC_MANUAL_EVENT));
}

static void *get_shm( unsigned int idx )
{
    int entry  = (idx * 8) / pagesize;
    int offset = (idx * 8) % pagesize;

    if (entry >= shm_addrs_size)
    {
        int new_size = max(shm_addrs_size * 2, entry + 1);

        if (!(shm_addrs = realloc( shm_addrs, new_size * sizeof(shm_addrs[0]) )))
            fprintf( stderr, "esync: couldn't expand shm_addrs array to size %d\n", entry + 1 );

        memset( shm_addrs + shm_addrs_size, 0, (new_size - shm_addrs_size) * sizeof(shm_addrs[0]) );

        shm_addrs_size = new_size;
    }

    if (!shm_addrs[entry])
    {
        void *addr = mmap( NULL, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, entry * pagesize );
        if (addr == (void *)-1)
        {
            fprintf( stderr, "esync: failed to map page %d (offset %#lx): ", entry, entry * pagesize );
            perror( "mmap" );
        }

        if (debug_level)
            fprintf( stderr, "esync: Mapping page %d at %p.\n", entry, addr );

        if (__sync_val_compare_and_swap( &shm_addrs[entry], 0, addr ))
            munmap( addr, pagesize ); /* someone beat us to it */
    }

    return (void *)((unsigned long)shm_addrs[entry] + offset);
}

struct semaphore
{
    int max;
    int count;
};
C_ASSERT(sizeof(struct semaphore) == 8);

struct mutex
{
    DWORD tid;
    int count;    /* recursion count */
};
C_ASSERT(sizeof(struct mutex) == 8);

struct event
{
    int signaled;
    int locked;
};
C_ASSERT(sizeof(struct event) == 8);

/* Allocate and initialize a shm slot for an eventfd-backed object. Factored
 * out of create_esync() so server/inproc_sync.c can drive the same code
 * for server-internal sync objects. The 'initval'/'max' params have
 * type-specific meanings (see esync.h doc):
 *   ESYNC_SEMAPHORE: initval=count, max=max
 *   ESYNC_*_EVENT / ESYNC_*_SERVER: initval=signaled, max ignored
 *   ESYNC_MUTEX: initval=owner_tid (0 if unowned), max=recursion count
 */
unsigned int esync_alloc_shm( int fd, enum esync_type type, int initval, int max )
{
    unsigned int shm_idx;

    /* If init didn't run (e.g., env disagreed across the do_esync_cached/
     * is_esync_initialized boundary, or shm_open failed), bail with a
     * one-shot warning. The grow loop below relies on pagesize > 0 and
     * would spin forever flooding stderr otherwise — see the 143 MB log
     * incident on Android ARM64EC where this manifested. */
    if (!is_esync_initialized)
    {
        static int warned;
        if (!warned++)
            fprintf( stderr, "esync: esync_alloc_shm called without esync_init — returning slot 0\n" );
        return 0;
    }

    /* Use the fd as index. fd is unique within the server process and
     * gets reused after close, so the slot can also be reused. */
    shm_idx = fd + 1; /* keep index 0 reserved */

    while (shm_idx * 8 >= shm_size)
    {
        shm_size += pagesize;
        if (ftruncate( shm_fd, shm_size ) == -1)
        {
            fprintf( stderr, "esync: couldn't expand %s to size %ld: ",
                     shm_name, (long)shm_size );
            perror( "ftruncate" );
        }
    }

    /* Initialize the shared memory portion server-side so a client that
     * opens the object before the creator returns sees consistent state. */
    switch (type)
    {
    case ESYNC_SEMAPHORE:
    {
        struct semaphore *semaphore = get_shm( shm_idx );
        semaphore->max = max;
        semaphore->count = initval;
        break;
    }
    case ESYNC_AUTO_EVENT:
    case ESYNC_MANUAL_EVENT:
    case ESYNC_AUTO_SERVER:
    case ESYNC_MANUAL_SERVER:
    {
        struct event *event = get_shm( shm_idx );
        event->signaled = initval ? 1 : 0;
        event->locked = 0;
        break;
    }
    case ESYNC_MUTEX:
    {
        struct mutex *mutex = get_shm( shm_idx );
        mutex->tid = initval;     /* owner_tid */
        mutex->count = max;       /* recursion count */
        break;
    }
    default:
        break;
    }

    return shm_idx;
}

struct esync *create_esync( struct object *root, const struct unicode_str *name,
                            unsigned int attr, int initval, int max, enum esync_type type,
                            const struct security_descriptor *sd )
{
#ifdef HAVE_SYS_EVENTFD_H
    struct esync *esync;

    if ((esync = create_named_object( root, &esync_ops, name, attr, sd )))
    {
        if (get_error() != STATUS_OBJECT_NAME_EXISTS)
        {
            int flags = EFD_CLOEXEC | EFD_NONBLOCK;

            if (type == ESYNC_SEMAPHORE)
                flags |= EFD_SEMAPHORE;

            /* initialize it if it didn't already exist */
            esync->fd = eventfd( initval, flags );
            if (esync->fd == -1)
            {
                perror( "eventfd" );
                file_set_error();
                release_object( esync );
                return NULL;
            }
            esync->type = type;

            /* Allocate the shm slot. For mutexes the alloc helper takes
             * (owner_tid, count); create_esync's user-visible mutex is
             * created either un-owned (initval=1: tid=0,count=0) or held
             * by the creator (initval=0: tid=current->id,count=1). Mirror
             * that here, then keep mutex_entry list bookkeeping local. */
            if (type == ESYNC_MUTEX)
            {
                esync->shm_idx = esync_alloc_shm( esync->fd, ESYNC_MUTEX,
                                                  initval ? 0 : current->id,
                                                  initval ? 0 : 1 );
                list_add_tail( &mutex_list, &esync->mutex_entry );
            }
            else
            {
                esync->shm_idx = esync_alloc_shm( esync->fd, type, initval, max );
            }
        }
        else
        {
            /* validate the type */
            if (!type_matches( type, esync->type ))
            {
                release_object( &esync->obj );
                set_error( STATUS_OBJECT_TYPE_MISMATCH );
                return NULL;
            }
        }
    }
    return esync;
#else
    /* FIXME: Provide a fallback implementation using pipe(). */
    set_error( STATUS_NOT_IMPLEMENTED );
    return NULL;
#endif
}

/* Create a file descriptor for an existing handle.
 * Caller must close the handle when it's done; it's not linked to an esync
 * server object in any way. */
int esync_create_fd( int initval, int flags )
{
#ifdef HAVE_SYS_EVENTFD_H
    int fd;

    fd = eventfd( initval, flags | EFD_CLOEXEC | EFD_NONBLOCK );
    if (fd == -1)
        perror( "eventfd" );

    return fd;
#else
    return -1;
#endif
}

/* Wake up a specific fd. */
void esync_wake_fd( int fd )
{
    static const uint64_t value = 1;

    /* WinNative: guard against -1 or 0. The agent-added object initializers
     * sometimes leave esync_fd at its memset/malloc default until the explicit
     * `= -1` assignment a few lines later; a wake_up() landing in that window
     * would otherwise write to stdin (fd 0) or garbage.  Silently no-op. */
    if (fd <= 0) return;

    if (write( fd, &value, sizeof(value) ) == -1)
        perror( "esync: write" );
}

/* Wake up a server-side esync object. */
void esync_wake_up( struct object *obj )
{
    enum esync_type dummy;
    int fd;

    if (obj->ops->get_esync_fd)
    {
        fd = obj->ops->get_esync_fd( obj, &dummy );
        if (fd > 0) esync_wake_fd( fd );
    }
}

void esync_clear( int fd )
{
    uint64_t value;

    /* guard: don't read from stdin/invalid fds. */
    if (fd <= 0) return;

    /* we don't care about the return value */
    read( fd, &value, sizeof(value) );
}

#ifndef __linux__
/* Only referenced by the non-Linux fallback in event_lock(). */
static inline void small_pause(void)
{
#ifdef __i386__
    __asm__ __volatile__( "rep;nop" : : : "memory" );
#else
    __asm__ __volatile__( "" : : : "memory" );
#endif
}
#endif

/* Cross-process futex lock for the manual-reset event 'locked' word in shm.
 * Mirrors the client-side helper in dlls/ntdll/unix/esync.c — both sides MUST
 * use the same protocol or waiters will never be woken. */
static inline void event_lock( struct event *event )
{
#ifdef __linux__
    int c;
    if ((c = __sync_val_compare_and_swap( &event->locked, 0, 1 )) != 0)
    {
        if (c != 2)
            c = __atomic_exchange_n( &event->locked, 2, __ATOMIC_SEQ_CST );
        while (c != 0)
        {
            syscall( __NR_futex, &event->locked, FUTEX_WAIT, 2, NULL, NULL, 0 );
            c = __atomic_exchange_n( &event->locked, 2, __ATOMIC_SEQ_CST );
        }
    }
#else
    while (__sync_val_compare_and_swap( &event->locked, 0, 1 ))
        small_pause();
#endif
}

static inline void event_unlock( struct event *event )
{
#ifdef __linux__
    if (__atomic_exchange_n( &event->locked, 0, __ATOMIC_SEQ_CST ) == 2)
        syscall( __NR_futex, &event->locked, FUTEX_WAKE, 1, NULL, NULL, 0 );
#else
    event->locked = 0;
#endif
}

/* Primitive-arg core of esync_set_event so server/inproc_sync.c can call
 * the same logic without owning a struct esync. The struct-esync wrapper
 * below just forwards. */
void esync_inproc_set_event( int fd, unsigned int shm_idx, enum esync_type type )
{
    static const uint64_t value = 1;
    struct event *event = get_shm( shm_idx );

    assert( event != NULL );

    if (debug_level)
        fprintf( stderr, "esync_inproc_set_event() fd=%d\n", fd );

    if (type == ESYNC_MANUAL_EVENT || type == ESYNC_MANUAL_SERVER)
        event_lock( event );

    if (!__atomic_exchange_n( &event->signaled, 1, __ATOMIC_SEQ_CST ))
    {
        if (write( fd, &value, sizeof(value) ) == -1)
            perror( "esync: write" );
    }

    if (type == ESYNC_MANUAL_EVENT || type == ESYNC_MANUAL_SERVER)
        event_unlock( event );
}

/* Server-side event support. */
void esync_set_event( struct esync *esync )
{
    assert( esync->obj.ops == &esync_ops );
    esync_inproc_set_event( esync->fd, esync->shm_idx, esync->type );
}

void esync_inproc_reset_event( int fd, unsigned int shm_idx, enum esync_type type )
{
    static uint64_t value = 1;
    struct event *event = get_shm( shm_idx );

    assert( event != NULL );

    if (debug_level)
        fprintf( stderr, "esync_inproc_reset_event() fd=%d\n", fd );

    if (type == ESYNC_MANUAL_EVENT || type == ESYNC_MANUAL_SERVER)
        event_lock( event );

    /* Only bother signaling the fd if we weren't already signaled. */
    if (__atomic_exchange_n( &event->signaled, 0, __ATOMIC_SEQ_CST ))
    {
        /* we don't care about the return value */
        read( fd, &value, sizeof(value) );
    }

    if (type == ESYNC_MANUAL_EVENT || type == ESYNC_MANUAL_SERVER)
        event_unlock( event );
}

void esync_reset_event( struct esync *esync )
{
    assert( esync->obj.ops == &esync_ops );
    esync_inproc_reset_event( esync->fd, esync->shm_idx, esync->type );
}

void esync_abandon_mutexes( struct thread *thread )
{
    struct esync *esync;

    LIST_FOR_EACH_ENTRY( esync, &mutex_list, struct esync, mutex_entry )
    {
        struct mutex *mutex = get_shm( esync->shm_idx );

        if (mutex->tid == thread->id)
        {
            if (debug_level)
                fprintf( stderr, "esync_abandon_mutexes() fd=%d\n", esync->fd );
            mutex->tid = ~0;
            mutex->count = 0;
            esync_wake_fd( esync->fd );
        }
    }
}

DECL_HANDLER(create_esync)
{
    struct esync *esync;
    struct unicode_str name;
    struct object *root;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, &root );

    if (!do_esync())
    {
        set_error( STATUS_NOT_IMPLEMENTED );
        return;
    }

    if (!req->type)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!objattr) return;

    if ((esync = create_esync( root, &name, objattr->attributes, req->initval, req->max, req->type, sd )))
    {
        if (get_error() == STATUS_OBJECT_NAME_EXISTS)
            reply->handle = alloc_handle( current->process, esync, req->access, objattr->attributes );
        else
            reply->handle = alloc_handle_no_access_check( current->process, esync,
                                                          req->access, objattr->attributes );

        reply->type = esync->type;
        reply->shm_idx = esync->shm_idx;
        send_client_fd( current->process, esync->fd, reply->handle );
        release_object( esync );
    }

    if (root) release_object( root );
}

DECL_HANDLER(open_esync)
{
    struct unicode_str name = get_req_unicode_str();

    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &esync_ops, &name, req->attributes );

    /* send over the fd */
    if (reply->handle)
    {
        struct esync *esync;

        if (!(esync = (struct esync *)get_handle_obj( current->process, reply->handle,
                                                      0, &esync_ops )))
            return;

        if (!type_matches( req->type, esync->type ))
        {
            set_error( STATUS_OBJECT_TYPE_MISMATCH );
            release_object( esync );
            return;
        }

        reply->type = esync->type;
        reply->shm_idx = esync->shm_idx;

        send_client_fd( current->process, esync->fd, reply->handle );
        release_object( esync );
    }
}

/* Retrieve a file descriptor for an esync object which will be signaled by the
 * server. The client should only read from (i.e. wait on) this object. */
DECL_HANDLER(get_esync_fd)
{
    struct object *obj;
    enum esync_type type;
    int fd;

    if (!(obj = get_handle_obj( current->process, req->handle, SYNCHRONIZE, NULL )))
        return;

    if (obj->ops->get_esync_fd)
    {
        fd = obj->ops->get_esync_fd( obj, &type );
        reply->type = type;
        if (obj->ops == &esync_ops)
        {
            struct esync *esync = (struct esync *)obj;
            reply->shm_idx = esync->shm_idx;
        }
        else
            reply->shm_idx = 0;
        send_client_fd( current->process, fd, req->handle );
    }
    else
    {
        if (debug_level)
        {
            fprintf( stderr, "%04x: esync: can't wait on object: ", current->id );
            obj->ops->dump( obj, 0 );
        }
        set_error( STATUS_NOT_IMPLEMENTED );
    }

    release_object( obj );
}

/* Return the fd used for waiting on user APCs. */
DECL_HANDLER(get_esync_apc_fd)
{
    send_client_fd( current->process, current->esync_apc_fd, current->id );
}
