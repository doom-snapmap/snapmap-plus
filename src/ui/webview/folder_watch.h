/* folder_watch.h -- the debounce behind the prefabs folder watcher: the watcher thread notes each change and
 * the think loop refreshes the Prefabs list once 300 ms pass without one, so a burst of writes is one refresh.
 * Pure C with no Windows calls; the caller passes the time. */
#ifndef SNAPMAP_PLUS_FOLDER_WATCH_H
#define SNAPMAP_PLUS_FOLDER_WATCH_H

#define SH_FW_PREFABS_QUIET_MS 300ull

typedef struct sh_fw_debounce {
    unsigned long long quiet_ms;   /* quiet time before a refresh is due */
    unsigned long long last;       /* time of the last change */
    unsigned events;               /* changes since the last refresh */
    int pending;
} sh_fw_debounce;

static __inline void sh_fw_init(sh_fw_debounce *d, unsigned long long quiet_ms)
{
    d->quiet_ms = quiet_ms;
    d->last = 0;
    d->events = 0;
    d->pending = 0;
}

static __inline void sh_fw_note(sh_fw_debounce *d, unsigned long long now)
{
    d->last = now;
    d->events++;
    d->pending = 1;
}

/* 1 when a change is pending and the quiet time has passed since the last one. */
static __inline int sh_fw_due(const sh_fw_debounce *d, unsigned long long now)
{
    return d->pending && now >= d->last + d->quiet_ms;
}

/* The refresh ran: start over. Returns the changes it covered. */
static __inline unsigned sh_fw_done(sh_fw_debounce *d)
{
    unsigned n = d->events;
    d->pending = 0;
    d->events = 0;
    return n;
}

#endif /* SNAPMAP_PLUS_FOLDER_WATCH_H */
