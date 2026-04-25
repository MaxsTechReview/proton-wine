/*
 * Server-side event management
 *
 * Copyright (C) 1998 Alexandre Julliard
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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"

#include "handle.h"
#include "thread.h"
#include "request.h"
#include "security.h"
#include "esync.h"

static const WCHAR event_name[] = {'E','v','e','n','t'};

struct type_descr event_type =
{
    { event_name, sizeof(event_name) },   /* name */
    EVENT_ALL_ACCESS,                     /* valid_access */
    {                                     /* mapping */
        STANDARD_RIGHTS_READ | EVENT_QUERY_STATE,
        STANDARD_RIGHTS_WRITE | EVENT_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        EVENT_ALL_ACCESS
    },
};

struct event_sync
{
    struct object  obj;             /* object header */
    unsigned int   manual : 1;      /* is it a manual reset event? */
    unsigned int   signaled : 1;    /* event has been signaled */
    int            esync_fd;        /* WinNative: eventfd for ESync waiters */
};

static void event_sync_dump( struct object *obj, int verbose );
static int event_sync_signaled( struct object *obj, struct wait_queue_entry *entry );
static void event_sync_satisfied( struct object *obj, struct wait_queue_entry *entry );
static int event_sync_signal( struct object *obj, unsigned int access, int signal );
static int event_sync_get_esync_fd( struct object *obj, enum esync_type *type );
static void event_sync_destroy( struct object *obj );

static const struct object_ops event_sync_ops =
{
    sizeof(struct event_sync), /* size */
    &no_type,                  /* type */
    event_sync_dump,           /* dump */
    add_queue,                 /* add_queue */
    remove_queue,              /* remove_queue */
    event_sync_signaled,       /* signaled */
    event_sync_satisfied,      /* satisfied */
    event_sync_signal,         /* signal */
    no_get_fd,                 /* get_fd */
    default_get_sync,          /* get_sync */
    default_map_access,        /* map_access */
    default_get_sd,            /* get_sd */
    default_set_sd,            /* set_sd */
    default_get_full_name,     /* get_full_name */
    no_lookup_name,            /* lookup_name */
    directory_link_name,       /* link_name */
    default_unlink_name,       /* unlink_name */
    no_open_file,              /* open_file */
    no_kernel_obj_list,        /* get_kernel_obj_list */
    no_close_handle,           /* close_handle */
    event_sync_destroy,        /* destroy */
    event_sync_get_esync_fd,   /* get_esync_fd */
};

static struct object *create_event_sync( int manual, int signaled )
{
    struct event_sync *event;

    if (get_inproc_device_fd() >= 0) return (struct object *)create_inproc_event_sync( manual, signaled );

    if (!(event = alloc_object( &event_sync_ops ))) return NULL;
    event->manual   = manual;
    event->signaled = signaled;
    event->esync_fd = -1;

    if (do_esync())
        event->esync_fd = esync_create_fd( signaled, 0 );

    return &event->obj;
}

struct event_sync *create_server_internal_sync( int manual, int signaled )
{
    struct event_sync *event;

    if (!(event = alloc_object( &event_sync_ops ))) return NULL;
    event->manual   = manual;
    event->signaled = signaled;
    event->esync_fd = -1;

    if (do_esync())
        event->esync_fd = esync_create_fd( signaled, 0 );

    return event;
}

static int event_sync_get_esync_fd( struct object *obj, enum esync_type *type )
{
    struct event_sync *event = (struct event_sync *)obj;
    *type = event->manual ? ESYNC_MANUAL_SERVER : ESYNC_AUTO_SERVER;
    return event->esync_fd;
}

/* WinNative: helper for ALL wrapper objects (thread, process, queue,
 * completion, console, device_manager, timer, fd) whose get_esync_fd
 * callbacks previously returned their own wrapper->esync_fd that was never
 * signaled. Wine 11 routes signaling through wrapper->sync (an event_sync),
 * which now carries its own esync_fd. Wrappers should redirect clients to
 * sync->esync_fd so client/server agree on the same kernel eventfd. */
int sync_get_esync_fd( struct object *sync, enum esync_type *type )
{
    if (sync && sync->ops == &event_sync_ops)
    {
        struct event_sync *es = (struct event_sync *)sync;
        if (type) *type = es->manual ? ESYNC_MANUAL_SERVER : ESYNC_AUTO_SERVER;
        return es->esync_fd;
    }
    return -1;
}

static void event_sync_destroy( struct object *obj )
{
    struct event_sync *event = (struct event_sync *)obj;
    if (do_esync() && event->esync_fd != -1)
        close( event->esync_fd );
}

struct object *create_internal_sync( int manual, int signaled )
{
    if (get_inproc_device_fd() >= 0) return (struct object *)create_inproc_internal_sync( manual, signaled );
    return (struct object *)create_server_internal_sync( manual, signaled );
}

static void event_sync_dump( struct object *obj, int verbose )
{
    struct event_sync *event = (struct event_sync *)obj;
    assert( obj->ops == &event_sync_ops );
    fprintf( stderr, "Event manual=%d signaled=%d\n",
             event->manual, event->signaled );
}

static int event_sync_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct event_sync *event = (struct event_sync *)obj;
    assert( obj->ops == &event_sync_ops );
    return event->signaled;
}

static void event_sync_satisfied( struct object *obj, struct wait_queue_entry *entry )
{
    struct event_sync *event = (struct event_sync *)obj;
    assert( obj->ops == &event_sync_ops );
    /* Reset if it's an auto-reset event */
    if (!event->manual)
    {
        event->signaled = 0;
        /* WinNative: clear the eventfd too so subsequent esync waits block. */
        if (do_esync() && event->esync_fd != -1)
            esync_clear( event->esync_fd );
    }
}

static int wn_sync_trace_enabled(void)
{
    static int cached = -1;
    if (cached == -1)
    {
        const char *e = getenv( "WN_SYNC_TRACE" );
        cached = e && atoi( e ) > 0;
    }
    return cached;
}

static int event_sync_signal( struct object *obj, unsigned int access, int signal )
{
    struct event_sync *event = (struct event_sync *)obj;
    int prev;
    assert( obj->ops == &event_sync_ops );

    prev = event->signaled;
    if ((event->signaled = !!signal))
    {
        /* wake up all waiters if manual reset, a single one otherwise */
        wake_up( &event->obj, !event->manual );
        /* WinNative: also signal the eventfd so ESync waiters polling on it
         * wake up. Without this, any WaitForSingleObject that went through
         * the ESync client path (do_esync()=1) never sees the server-side
         * signal_sync() completion → boot_event timeout, RpcSs timeout,
         * nodrv_CreateWindow, etc. fd guard in esync_wake_fd handles -1. */
        if (do_esync() && event->esync_fd != -1)
            esync_wake_fd( event->esync_fd );
    }
    else if (do_esync() && event->esync_fd != -1)
    {
        /* reset path — clear the eventfd */
        esync_clear( event->esync_fd );
    }
    if (wn_sync_trace_enabled())
        fprintf( stderr, "WN_SYNC_TRACE: event_sync_signal obj=%p fd=%d manual=%d prev=%d signal=%d\n",
                 event, event->esync_fd, event->manual, prev, signal );
    return 1;
}

struct event
{
    struct object      obj;             /* object header */
    struct object     *sync;            /* event sync object */
    struct list        kernel_object;   /* list of kernel object pointers */
    int                manual_reset;    /* is it a manual reset event? */
    int                esync_fd;        /* esync file descriptor */
};

static void event_dump( struct object *obj, int verbose );
static struct object *event_get_sync( struct object *obj );
static int event_get_esync_fd( struct object *obj, enum esync_type *type );
static int event_signal( struct object *obj, unsigned int access, int signal );
static struct list *event_get_kernel_obj_list( struct object *obj );
static void event_destroy( struct object *obj );

static const struct object_ops event_ops =
{
    sizeof(struct event),      /* size */
    &event_type,               /* type */
    event_dump,                /* dump */
    NULL,                      /* add_queue */
    NULL,                      /* remove_queue */
    NULL,                      /* signaled */
    NULL,                      /* satisfied */
    event_signal,              /* signal */
    no_get_fd,                 /* get_fd */
    event_get_sync,            /* get_sync */
    default_map_access,        /* map_access */
    default_get_sd,            /* get_sd */
    default_set_sd,            /* set_sd */
    default_get_full_name,     /* get_full_name */
    no_lookup_name,            /* lookup_name */
    directory_link_name,       /* link_name */
    default_unlink_name,       /* unlink_name */
    no_open_file,              /* open_file */
    event_get_kernel_obj_list, /* get_kernel_obj_list */
    no_close_handle,           /* close_handle */
    event_destroy,             /* destroy */
    /* Reverted back to event_get_esync_fd — leaving this NULL caused
     * __esync_wait_objects mixed-wait fixmes (events got STATUS_NOT_IMPLEMENTED
     * from server's get_esync_fd handler, other object types returned esync
     * fds, client hit the "can't wait on esync and server at the same time"
     * degraded path). All object types must expose an eventfd for the wake_up
     * bridge + fd guards to handle everything uniformly. */
    event_get_esync_fd,        /* get_esync_fd */
};


static const WCHAR keyed_event_name[] = {'K','e','y','e','d','E','v','e','n','t'};

struct type_descr keyed_event_type =
{
    { keyed_event_name, sizeof(keyed_event_name) },   /* name */
    KEYEDEVENT_ALL_ACCESS | SYNCHRONIZE,              /* valid_access */
    {                                                 /* mapping */
        STANDARD_RIGHTS_READ | KEYEDEVENT_WAIT,
        STANDARD_RIGHTS_WRITE | KEYEDEVENT_WAKE,
        STANDARD_RIGHTS_EXECUTE,
        KEYEDEVENT_ALL_ACCESS
    },
};

struct keyed_event
{
    struct object  obj;             /* object header */
};

static void keyed_event_dump( struct object *obj, int verbose );
static int keyed_event_signaled( struct object *obj, struct wait_queue_entry *entry );

static const struct object_ops keyed_event_ops =
{
    sizeof(struct keyed_event),  /* size */
    &keyed_event_type,           /* type */
    keyed_event_dump,            /* dump */
    add_queue,                   /* add_queue */
    remove_queue,                /* remove_queue */
    keyed_event_signaled,        /* signaled */
    no_satisfied,                /* satisfied */
    no_signal,                   /* signal */
    no_get_fd,                   /* get_fd */
    default_get_sync,            /* get_sync */
    default_map_access,          /* map_access */
    default_get_sd,              /* get_sd */
    default_set_sd,              /* set_sd */
    default_get_full_name,       /* get_full_name */
    no_lookup_name,              /* lookup_name */
    directory_link_name,         /* link_name */
    default_unlink_name,         /* unlink_name */
    no_open_file,                /* open_file */
    no_kernel_obj_list,          /* get_kernel_obj_list */
    no_close_handle,             /* close_handle */
    no_destroy,                  /* destroy */
    NULL,                        /* get_esync_fd */
};


struct event *create_event( struct object *root, const struct unicode_str *name,
                            unsigned int attr, int manual_reset, int initial_state,
                            const struct security_descriptor *sd )
{
    struct event *event;

    if ((event = create_named_object( root, &event_ops, name, attr, sd )))
    {
        if (get_error() != STATUS_OBJECT_NAME_EXISTS)
        {
            /* initialize it if it didn't already exist */
            event->sync = NULL;
            event->manual_reset = manual_reset;
            event->esync_fd = -1;
            list_init( &event->kernel_object );

            if (!(event->sync = create_event_sync( manual_reset, initial_state )))
            {
                release_object( event );
                return NULL;
            }

            if (do_esync())
                event->esync_fd = esync_create_fd( initial_state, 0 );
        }
    }
    return event;
}

struct event *get_event_obj( struct process *process, obj_handle_t handle, unsigned int access )
{
    struct object *obj;

    if (do_esync() && (obj = get_handle_obj( process, handle, access, &esync_ops )))
        return (struct event *)obj; /* even though it's not an event */

    return (struct event *)get_handle_obj( process, handle, access, &event_ops );
}

void set_event( struct event *event )
{
    if (do_esync() && event->obj.ops == &esync_ops)
    {
        esync_set_event( (struct esync *)event );
        return;
    }

    signal_sync( event->sync );
    /* WinNative: bridge for ESync waiters. Wine 11's signal_sync only wakes
     * event_sync/inproc_sync waiters; ESync waiters are polling on
     * event->esync_fd and need an explicit eventfd write. Without this, any
     * client doing WaitForSingleObject(boot_event) via esync_wait_objects
     * never wakes when wineboot signals "I'm done" — the boot_event wait
     * timeout bug. */
    if (do_esync()) esync_wake_up( &event->obj );
}

void reset_event( struct event *event )
{
    if (do_esync() && event->obj.ops == &esync_ops)
    {
        esync_reset_event( (struct esync *)event );
        return;
    }

    reset_sync( event->sync );

    if (do_esync())
        esync_clear( event->esync_fd );
}

static void event_dump( struct object *obj, int verbose )
{
    struct event *event = (struct event *)obj;
    assert( obj->ops == &event_ops );
    event->sync->ops->dump( event->sync, verbose );
}

static struct object *event_get_sync( struct object *obj )
{
    struct event *event = (struct event *)obj;
    assert( obj->ops == &event_ops );
    return grab_object( event->sync );
}

static int event_get_esync_fd( struct object *obj, enum esync_type *type )
{
    struct event *event = (struct event *)obj;
    *type = event->manual_reset ? ESYNC_MANUAL_SERVER : ESYNC_AUTO_SERVER;
    /* WinNative: Wine 11 routes all signaling through event->sync (event_sync
     * or inproc_sync). We return THAT object's esync_fd so the client polls
     * the same kernel eventfd that the server writes to via signal_sync →
     * event_sync_signal → esync_wake_fd. Fall back to event's own esync_fd
     * if sync isn't an event_sync_ops (e.g. inproc_sync on NTSync systems,
     * which shouldn't happen when WINEESYNC=1 and WINENTSYNC=0 but guard
     * anyway). */
    if (event->sync && event->sync->ops == &event_sync_ops)
    {
        struct event_sync *sync = (struct event_sync *)event->sync;
        if (sync->esync_fd != -1) return sync->esync_fd;
    }
    return event->esync_fd;
}

static int event_signal( struct object *obj, unsigned int access, int signal )
{
    struct event *event = (struct event *)obj;
    assert( obj->ops == &event_ops );

    assert( event->sync->ops == &event_sync_ops ); /* never called with inproc syncs */
    assert( signal == -1 ); /* always called from signal_object */

    if (!(access & EVENT_MODIFY_STATE))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }

    return event_sync_signal( event->sync, 0, 1 );
}

static struct list *event_get_kernel_obj_list( struct object *obj )
{
    struct event *event = (struct event *)obj;
    return &event->kernel_object;
}

static void event_destroy( struct object *obj )
{
    struct event *event = (struct event *)obj;
    assert( obj->ops == &event_ops );

    if (event->sync) release_object( event->sync );
    if (do_esync())
        close( event->esync_fd );
}

struct keyed_event *create_keyed_event( struct object *root, const struct unicode_str *name,
                                        unsigned int attr, const struct security_descriptor *sd )
{
    struct keyed_event *event;

    if ((event = create_named_object( root, &keyed_event_ops, name, attr, sd )))
    {
        if (get_error() != STATUS_OBJECT_NAME_EXISTS)
        {
            /* initialize it if it didn't already exist */
        }
    }
    return event;
}

struct keyed_event *get_keyed_event_obj( struct process *process, obj_handle_t handle, unsigned int access )
{
    return (struct keyed_event *)get_handle_obj( process, handle, access, &keyed_event_ops );
}

static void keyed_event_dump( struct object *obj, int verbose )
{
    fputs( "Keyed event\n", stderr );
}

static enum select_opcode matching_op( enum select_opcode op )
{
    return op ^ (SELECT_KEYED_EVENT_WAIT ^ SELECT_KEYED_EVENT_RELEASE);
}

static int keyed_event_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct wait_queue_entry *ptr;
    struct process *process;
    enum select_opcode select_op;

    assert( obj->ops == &keyed_event_ops );

    process = get_wait_queue_thread( entry )->process;
    select_op = get_wait_queue_select_op( entry );
    if (select_op != SELECT_KEYED_EVENT_WAIT && select_op != SELECT_KEYED_EVENT_RELEASE) return 1;

    LIST_FOR_EACH_ENTRY( ptr, &obj->wait_queue, struct wait_queue_entry, entry )
    {
        if (ptr == entry) continue;
        if (get_wait_queue_thread( ptr )->process != process) continue;
        if (get_wait_queue_select_op( ptr ) != matching_op( select_op )) continue;
        if (get_wait_queue_key( ptr ) != get_wait_queue_key( entry )) continue;
        if (wake_thread_queue_entry( ptr )) return 1;
    }
    return 0;
}

/* create an event */
DECL_HANDLER(create_event)
{
    struct event *event;
    struct unicode_str name;
    struct object *root;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, &root );

    if (!objattr) return;

    if ((event = create_event( root, &name, objattr->attributes,
                               req->manual_reset, req->initial_state, sd )))
    {
        if (get_error() == STATUS_OBJECT_NAME_EXISTS)
            reply->handle = alloc_handle( current->process, event, req->access, objattr->attributes );
        else
            reply->handle = alloc_handle_no_access_check( current->process, event,
                                                          req->access, objattr->attributes );
        release_object( event );
    }

    if (root) release_object( root );
}

/* open a handle to an event */
DECL_HANDLER(open_event)
{
    struct unicode_str name = get_req_unicode_str();

    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &event_ops, &name, req->attributes );
}

/* do an event operation */
DECL_HANDLER(event_op)
{
    struct event_sync *sync;
    struct event *event;

    if (!(event = get_event_obj( current->process, req->handle, EVENT_MODIFY_STATE ))) return;
    assert( event->sync->ops == &event_sync_ops ); /* never called with inproc syncs */
    sync = (struct event_sync *)event->sync;

    reply->state = sync->signaled;
    switch(req->op)
    {
    case PULSE_EVENT:
        set_event( event );
        reset_event( event );
        break;
    case SET_EVENT:
        set_event( event );
        break;
    case RESET_EVENT:
        reset_event( event );
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        break;
    }
    release_object( event );
}

/* return details about the event */
DECL_HANDLER(query_event)
{
    struct event_sync *sync;
    struct event *event;

    if (!(event = get_event_obj( current->process, req->handle, EVENT_QUERY_STATE ))) return;
    assert( event->sync->ops == &event_sync_ops ); /* never called with inproc syncs */
    sync = (struct event_sync *)event->sync;

    reply->manual_reset = sync->manual;
    reply->state = sync->signaled;

    release_object( event );
}

/* create a keyed event */
DECL_HANDLER(create_keyed_event)
{
    struct keyed_event *event;
    struct unicode_str name;
    struct object *root;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, &root );

    if (!objattr) return;

    if ((event = create_keyed_event( root, &name, objattr->attributes, sd )))
    {
        if (get_error() == STATUS_OBJECT_NAME_EXISTS)
            reply->handle = alloc_handle( current->process, event, req->access, objattr->attributes );
        else
            reply->handle = alloc_handle_no_access_check( current->process, event,
                                                          req->access, objattr->attributes );
        release_object( event );
    }
    if (root) release_object( root );
}

/* open a handle to a keyed event */
DECL_HANDLER(open_keyed_event)
{
    struct unicode_str name = get_req_unicode_str();

    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &keyed_event_ops, &name, req->attributes );
}
