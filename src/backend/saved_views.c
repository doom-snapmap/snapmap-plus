/* Native room views are regenerated. An explicitly loaded or pasted view has its own gameplay declaration. */
#include "saved_views.h"
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef SH_SAVED_VIEWS_TEST
#include "backend_log.h"
#include "hook.h"
#include "signatures.h"
#endif

#define SV_MODE 0x3c8
#define SV_VIEW 3
#define SV_DEFINITION 0x150
#define SV_BUCKETS 127

typedef struct sv_entry {
    struct sv_entry *next;
    const void *definition;
} sv_entry;

static CRITICAL_SECTION sv_lock;
static sv_entry *sv_buckets[SV_BUCKETS];
static volatile LONG sv_count;
static volatile LONG sv_ready;

static sv_entry **sv_find(const void *def)
{
    sv_entry **p = &sv_buckets[((uintptr_t)def >> 4) % SV_BUCKETS];
    while (*p && (*p)->definition != def) p = &(*p)->next;
    return p;
}

static void sv_add(const void *def)
{
    if (!def) return;
    EnterCriticalSection(&sv_lock);
    sv_entry **p = sv_find(def);
    if (!*p) {
        sv_entry *e = calloc(1, sizeof(*e));
        if (e) {
            e->definition = def;
            *p = e;
            InterlockedIncrement(&sv_count);
        }
#ifndef SH_SAVED_VIEWS_TEST
        else backend_log("MAP: out of memory tracking an explicit room-view placement");
#endif
    }
    LeaveCriticalSection(&sv_lock);
}

static int sv_registered(const void *def)
{
    int found;
    if (!def || !sv_count) return 0;
    EnterCriticalSection(&sv_lock);
    found = *sv_find(def) != NULL;
    LeaveCriticalSection(&sv_lock);
    return found;
}

static void sv_drop(const void *def)
{
    if (!sv_count) return;
    EnterCriticalSection(&sv_lock);
    sv_entry **p = sv_find(def);
    if (*p) {
        sv_entry *e = *p;
        *p = e->next;
        free(e);
        InterlockedDecrement(&sv_count);
    }
    LeaveCriticalSection(&sv_lock);
}

typedef void (*sv_bind_fn)(void *palette, void *settings, void *source, void *dest);
typedef void *(*sv_copy_fn)(void **dest, void **source);
typedef void *(*sv_destroy_fn)(void *def, unsigned flags);
typedef void (*sv_filter_fn)(void *store);
static sv_bind_fn sv_bind;
static sv_copy_fn sv_copy, sv_assign;
static sv_destroy_fn sv_destroy;
static sv_filter_fn sv_filter;

static void sv_bound(void *palette, void *settings, void *source, void *dest)
{
    sv_bind(palette, settings, source, dest);
    const unsigned char *row = *(const unsigned char **)dest;
    if (sv_ready && row && *(const int *)(row + SV_MODE) == SV_VIEW)
        sv_add(*(void **)((unsigned char *)dest + SV_DEFINITION));
}

/* Moves exchange the two owned pointers. Deep copies construct a different declaration. */
static void *sv_copy_with(sv_copy_fn copy, void **dest, void **source)
{
    int keep = sv_registered(*source);
    void *r = copy(dest, source);
    if (keep) sv_add(*dest);
    return r;
}
static void *sv_copied(void **dest, void **source) { return sv_copy_with(sv_copy,dest,source); }
static void *sv_assigned(void **dest, void **source) { return sv_copy_with(sv_assign,dest,source); }
static void *sv_destroyed(void *def, unsigned flags)
{
    sv_drop(def);
    return sv_destroy(def,flags);
}

/* The filter reads slot->row->moduleAutomation and only dereferences an entry further when removing it.
 * Scope this one decision to the private snapshot. Its real entries are back before EntityClone and wiring. */
static void sv_filtered(void *store)
{
    if (!sv_count) { sv_filter(store); return; }
    void **slots = *(void ***)((unsigned char *)store + 0xc0);
    int slot_count = *(int *)((unsigned char *)store + 0xc8);
    const int *ids = *(const int **)((unsigned char *)store + 0x128);
    int member_count = *(int *)((unsigned char *)store + 0x130);
    struct saved_slot { int index; void *original; } local[16], *restore = local;
    size_t saved = 0;
    struct { uintptr_t refs; void *row; } filter_slot;
    int filter_row[SV_MODE / sizeof(int) + 1];
    __try {
        EnterCriticalSection(&sv_lock);
        __try {
            for (int m = 0; m < member_count; m++) {
                int i = ids[m];
                if (i < 0 || i >= slot_count) continue;
                unsigned char *entry = slots[i];
                const unsigned char *row = entry ? *(const unsigned char **)(entry + 8) : NULL;
                if (!row || *(const int *)(row + SV_MODE) != SV_VIEW) continue;
                if (!*sv_find(*(void **)(entry + 8 + SV_DEFINITION))) continue;
                if (saved == 0) {
                    memcpy(&filter_slot, entry, sizeof(filter_slot));
                    memcpy(filter_row, row, sizeof(filter_row));
                    filter_row[SV_MODE / sizeof(int)] = 1;  /* SNAP_MODULE_AUTO_EDIT */
                    filter_slot.row = filter_row;
                }
                if (saved == ARRAYSIZE(local)) {
                    size_t bytes = (size_t)member_count * sizeof(*restore);
                    struct saved_slot *more = HeapAlloc(GetProcessHeap(), HEAP_GENERATE_EXCEPTIONS, bytes);
                    memcpy(more, local, sizeof(local));
                    restore = more;
                }
                restore[saved].index = i;
                restore[saved++].original = entry;
                slots[i] = &filter_slot;
            }
        } __finally { LeaveCriticalSection(&sv_lock); }
        sv_filter(store);
    } __finally {
        /* The private snapshot still owns its reference to each original block. */
        slots = *(void ***)((unsigned char *)store + 0xc0);
        while (saved) {
            --saved;
            slots[restore[saved].index] = restore[saved].original;
        }
        if (restore != local) HeapFree(GetProcessHeap(), 0, restore);
    }
}

#ifndef SH_SAVED_VIEWS_TEST
int sh_saved_views_install(const uint8_t *base)
{
    static LONG started;
    static const sig_entry targets[] = {
        {"SavedViewBind","40 53 55 56 57 48 81 EC 38 07 00 00 48 C7 44 24 20 FE FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 20 07 00 00 49 8B E9 49 8B F8",0x12020d0,0x12014a0},
        {"SavedViewCopy","40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 89 74 24 50 48 8B F2 48 8B D9 33 FF 48 89 39 48 39 3A 74 51",0x119b890,0x119aac0},
        {"SavedViewAssign","40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 89 74 24 50 48 8B F2 48 8B F9 33 DB 48 39 1A 74 4E",0x119ba50,0x119ac80},
        {"SavedViewDestroy","48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F1 48 89 01 8B FA 48 81 C1 30 01 00 00",0x1563640,0x1564920},
        {"SavedViewFilter","4C 8B DC 56 48 83 EC 50 8B 81 48 01 00 00 48 8B F1 FF C8 33 C9 48 98 48 89 44 24 68 48 89 4C 24 60 48 85 C0",0x1200ff0,0x12003c0},
    };
    void *detours[] = {(void *)sv_bound, (void *)sv_copied, (void *)sv_assigned,
                       (void *)sv_destroyed, (void *)sv_filtered};
    const size_t stolen[] = {21,15,15,15,14};
    void *tramp[5] = {0};
    unsigned i;
    if (InterlockedCompareExchange(&started,1,0)) return sv_ready != 0;
    InitializeCriticalSection(&sv_lock);
    for (i = 0; i < 5; i++) {
        sig_result r;
        if (sig_resolve_one(base,&targets[i],&r) != SIG_OK) goto fail;
        tramp[i] = hook_prepare((void *)r.addr,detours[i],stolen[i]);
        if (!tramp[i]) goto fail;
    }
    sv_bind = (sv_bind_fn)tramp[0];
    sv_copy = (sv_copy_fn)tramp[1];
    sv_assign = (sv_copy_fn)tramp[2];
    sv_destroy = (sv_destroy_fn)tramp[3];
    sv_filter = (sv_filter_fn)tramp[4];
    for (i = 0; i < 5; i++) {
        if (hook_commit(tramp[i]) != B2_PATCH_OK) {
            /* An installed forwarder may already be executing. Its trampoline remains owned. */
            for (unsigned k = i+1; k < 5; k++) (void)hook_unpatch(tramp[k]);
            return 0;
        }
    }
    InterlockedExchange(&sv_ready,1);
    return 1;
fail:
    for (i = 5; i > 0; i--) if (tramp[i-1]) (void)hook_unpatch(tramp[i-1]);
    return 0;
}
#endif
