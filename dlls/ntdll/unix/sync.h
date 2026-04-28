/*
 * Internal interface between esync and the inproc-sync refcount cache
 * in dlls/ntdll/unix/sync.c. Lets esync route eventfd lifetime through
 * the same per-handle refcount that protects inproc-sync FDs against
 * close-while-waiting races: a closing thread cannot free an fd while
 * another thread is poll()ing on it.
 *
 * Not part of the public esync/fsync ABI — declarations only, layout
 * of struct inproc_sync stays opaque to esync.c.
 */

#ifndef __WINE_NTDLL_UNIX_SYNC_H
#define __WINE_NTDLL_UNIX_SYNC_H

#include <windef.h>

struct inproc_sync;

/* Bump the refcount of the cached entry for `handle`. Returns NULL if
 * the handle is not in the cache or its refcount has already reached
 * zero (the underlying fd was freed). Callers must pair every non-NULL
 * return with release_inproc_sync(). */
extern struct inproc_sync *get_cached_inproc_sync( HANDLE handle );

/* Drop a reference grabbed by get_cached_inproc_sync() (or transferred
 * out of cache_inproc_sync()). Closes the underlying fd when refcount
 * hits zero. */
extern void release_inproc_sync( struct inproc_sync *sync );

/* Register an esync fd into the inproc-sync refcount cache so its
 * lifetime is tied to outstanding waiter references. Caller MUST hold
 * fd_cache_mutex — this serializes registration against close_handle
 * and prevents a leaked fd if a close races with the install. After
 * the call the cache holds exactly one reference for the handle;
 * close goes through close_inproc_sync()'s do_esync() branch.
 * Idempotent: calling twice for the same live handle leaves the cache
 * state unchanged. */
extern void esync_register_inproc( HANDLE handle, int fd );

#endif /* __WINE_NTDLL_UNIX_SYNC_H */
