/* folder_watch.h -- the debounce behind the window's folder watchers.
 *
 * The window watches %LOCALAPPDATA%\snapmap-plus\prefabs\ and mods\ (one
 * ReadDirectoryChangesW thread, subtree, file and folder names, writes and
 * sizes) so the Prefabs list and the Mods tab follow the disk. A watcher
 * thread notes each change here; the window's think loop asks whether a
 * refresh is due:
 *
 *   prefabs  due once 300 ms pass without a change (a burst of writes is one
 *            list refresh);
 *   mods     due once 1500 ms pass without a change; a refresh the backend
 *            defers (Play, a map load, a store write) is retried later.
 *
 * The product's own store writes fire the watcher too. A change seen while
 * the store lock is held, or within the grace after a hold ended (the
 * trailing notifications of that write), is the product's own and is dropped;
 * the watcher re-arms with the next change after it. Pure C, no Windows
 * calls: the caller passes the time and the store's state, so the native
 * tests run it with a fake clock. */
#ifndef SNAPMAP_PLUS_FOLDER_WATCH_H
#define SNAPMAP_PLUS_FOLDER_WATCH_H

#define SH_FW_PREFABS_QUIET_MS 300ull
#define SH_FW_MODS_QUIET_MS    1500ull
#define SH_FW_STORE_GRACE_MS   1000ull
#define SH_FW_RETRY_MS         2000ull

typedef struct sh_fw_debounce {
    unsigned long long quiet_ms;   /* quiet time before a refresh is due */
    unsigned long long last;       /* time of the last change kept */
    unsigned long long not_before; /* a deferred refresh is not due before this */
    unsigned long long grace_until;/* changes before this are the product's own */
    unsigned events;               /* changes kept since the last refresh */
    unsigned dropped;              /* changes dropped as the product's own */
    unsigned store_gen;            /* the store lock's released holds last seen */
    int pending;
    int store_seen;                /* store_gen holds a value */
} sh_fw_debounce;

static __inline void sh_fw_init(sh_fw_debounce *d, unsigned long long quiet_ms)
{
    d->quiet_ms = quiet_ms;
    d->last = d->not_before = d->grace_until = 0;
    d->events = d->dropped = d->store_gen = 0;
    d->pending = d->store_seen = 0;
}

/* The store's state at `now` (the think loop asks every tick): while a hold
 * lasts, and for the grace after one ends, changes are the product's own. */
static __inline void sh_fw_store(sh_fw_debounce *d, unsigned long long now, int store_busy, unsigned store_gen)
{
    if (store_busy || (d->store_seen && store_gen != d->store_gen)) d->grace_until = now + SH_FW_STORE_GRACE_MS;
    d->store_gen = store_gen;
    d->store_seen = 1;
}

/* One change at `now`; store_busy is the store lock's state when it was seen
 * (0 for the prefabs folder). Returns 1 kept, 0 dropped as the product's own
 * write. */
static __inline int sh_fw_note(sh_fw_debounce *d, unsigned long long now, int store_busy)
{
    if (store_busy) d->grace_until = now + SH_FW_STORE_GRACE_MS;
    if (store_busy || now < d->grace_until) { d->dropped++; return 0; }
    d->last = now;
    d->events++;
    d->pending = 1;
    return 1;
}

/* 1 when the refresh is due at `now`: a change is pending, the quiet time
 * has passed since the last one, and no deferral holds it back. */
static __inline int sh_fw_due(const sh_fw_debounce *d, unsigned long long now)
{
    return d->pending && now >= d->last + d->quiet_ms && now >= d->not_before;
}

/* The refresh ran: start over. Returns the changes it covered. */
static __inline unsigned sh_fw_done(sh_fw_debounce *d)
{
    unsigned n = d->events;
    d->pending = 0;
    d->events = 0;
    d->not_before = 0;
    return n;
}

/* The refresh could not run now (Play, a load, a store write, another job):
 * keep it pending and ask again after `delay_ms`. */
static __inline void sh_fw_retry(sh_fw_debounce *d, unsigned long long now, unsigned long long delay_ms)
{
    d->pending = 1;
    d->not_before = now + delay_ms;
}

#endif /* SNAPMAP_PLUS_FOLDER_WATCH_H */
