#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "virtual_texture_provider.h"
#include "resource_instances.h"
#include "hook.h"
#include "backend_log.h"

/* Provider fields, equal in both renderers: the selector compares +0x1a0,
 * tests +0x3d8 and scans +0x70; the per-material bind copies +0x2c0 and +0.
 * SetSource creates the VMTR owner at +8 for kind 2. */
#define VTP_KIND 0x00
#define VTP_OWNER 0x08
#define VTP_PATH 0x70
#define VTP_NAME 0x1a0
#define VTP_SLOT 0x2c0
#define VTP_MODE 0x3d8
/* Material fields written by the native per-material bind in both renderers. */
#define VTM_SLOT 0xd8
#define VTM_NAME 0xe0

typedef struct vtp_owned { int held; char name[32]; } vtp_owned;
typedef void (*vtp_init_fn)(void *);
static sh_vt_native g_vt;
static vtp_owned g_owned[SH_VT_PROVIDER_SLOTS];
static SRWLOCK g_vt_lock = SRWLOCK_INIT;
static volatile LONG g_vt_ready, g_vt_attempted;
static sh_vt_provider_wanted_fn g_vt_wanted;
static vtp_init_fn g_vt_init;
static unsigned char (*g_vt_set_source)(void *,const char *,const char *,int,int,unsigned char,void *);
static void (*g_vt_release)(void *);
static int g_vt_native_busy;

/* The fixed table supplies object identity; source generations are actual
 * construction witnesses, including slot1. Metadata locks never span origin,
 * package, observer or native calls. Resource-instances owns the source map
 * and the provider's live payload share, so no second lease scheme is needed. */
typedef struct vtp_generation { uint64_t serial, token; int busy, complete; } vtp_generation;
typedef struct vtp_source {
    struct vtp_source *parent, *retained_next;
    void *provider;
    uint64_t serial;
    sh_resource_instance_origin **origins;
    size_t count, capacity, observations, released_observations;
    int slot, released, failed, recordable;
} vtp_source;
static SRWLOCK g_vt_origin_lock = SRWLOCK_INIT;
static vtp_generation g_vt_generation[SH_VT_PROVIDER_SLOTS];
static vtp_source *g_vt_retained;
static __declspec(thread) vtp_source *g_vt_source;
static volatile LONG64 g_vt_serial;
static volatile LONG g_vt_provenance;

static int vtp_slot(void *provider)
{
    uintptr_t at=(uintptr_t)provider, start=(uintptr_t)g_vt.system;
    if (!provider || !start || !g_vt.stride || at<start ||
        (at-start)%g_vt.stride || (at-start)/g_vt.stride>=SH_VT_PROVIDER_SLOTS) return -1;
    return (int)((at-start)/g_vt.stride);
}
static uint64_t vtp_serial(void)
{ LONG64 value=InterlockedIncrement64(&g_vt_serial); return value>0 ? (uint64_t)value : 0; }
static int vtp_generation_matches(int slot,uint64_t serial)
{
    int match=0;
    AcquireSRWLockShared(&g_vt_origin_lock);
    if(slot>=0 && slot<SH_VT_PROVIDER_SLOTS) {
        const vtp_generation *g=g_vt_generation+slot;
        match=g->serial==serial && !g->busy && g->complete;
    }
    ReleaseSRWLockShared(&g_vt_origin_lock); return match;
}
static void vtp_source_free(vtp_source *source)
{
    for(size_t i=0;i<source->count;i++) sh_resource_instances_origin_unhold(source->origins[i]);
    free(source->origins); free(source);
}
static void vtp_retired_storage(void *provider)
{
    vtp_source *retired=NULL;
    AcquireSRWLockExclusive(&g_vt_origin_lock);
    vtp_source **slot=&g_vt_retained;
    while(*slot) {
        vtp_source *source=*slot;
        if(source->provider==provider) { *slot=source->retained_next; source->retained_next=retired; retired=source; }
        else slot=&source->retained_next;
    }
    ReleaseSRWLockExclusive(&g_vt_origin_lock);
    while(retired) { vtp_source *next=retired->retained_next; vtp_source_free(retired); retired=next; }
}
int sh_vt_provider_source_recording(void)
{ return InterlockedCompareExchange(&g_vt_provenance,0,0) && g_vt_source && g_vt_source->released; }
int sh_vt_provider_source_observed(sh_resource_instance_origin *origin)
{
    vtp_source *source=g_vt_source;
    if(!sh_vt_provider_source_recording()) return 0;
    if(!source->recordable || source->observations==SIZE_MAX) { source->failed=1; return 0; }
    source->observations++;
    if(!origin) { source->failed=1; return 0; }
    for(size_t i=0;i<source->count;i++) if(source->origins[i]==origin) return 1;
    if(source->count==source->capacity) {
        size_t capacity=source->capacity ? source->capacity*2 : 8;
        if(capacity<source->capacity || capacity>SIZE_MAX/sizeof(*source->origins)) { source->failed=1; return 0; }
        void *next=realloc(source->origins,capacity*sizeof(*source->origins));
        if(!next) { source->failed=1; return 0; }
        source->origins=next; source->capacity=capacity;
    }
    if(!sh_resource_instances_origin_hold(origin)) { source->failed=1; return 0; }
    source->origins[source->count++]=origin; return 1;
}
int sh_vt_provider_provenance_ready(void)
{ return InterlockedCompareExchange(&g_vt_provenance,0,0)!=0; }
int sh_vt_provider_read_begin(void *provider,sh_vt_provider_read *out)
{
    if(out) memset(out,0,sizeof(*out));
    int slot=vtp_slot(provider); uint64_t serial=0;
    if(!out || !sh_vt_provider_provenance_ready() || slot<0) return 0;
    AcquireSRWLockShared(&g_vt_origin_lock);
    if(!g_vt_generation[slot].busy && g_vt_generation[slot].complete) serial=g_vt_generation[slot].serial;
    ReleaseSRWLockShared(&g_vt_origin_lock);
    if(!serial) return 0;
    sh_resource_instance_origin *origin=sh_resource_instances_lookup(provider);
    int held=origin && sh_resource_instances_origin_hold(origin);
    sh_resource_instances_origin_release(origin);
    if(!held) return 0;
    if(!vtp_generation_matches(slot,serial)) { sh_resource_instances_origin_unhold(origin); return 0; }
    *out=(sh_vt_provider_read){provider,serial,origin,1}; return 1;
}
int sh_vt_provider_read_finish(sh_vt_provider_read *receipt,int returned,int ordered)
{
    if(!receipt || !receipt->active) return 0;
    int complete=returned && ordered && vtp_generation_matches(vtp_slot(receipt->provider),receipt->generation);
    sh_resource_instance_origin *origin=receipt->origin;
    memset(receipt,0,sizeof(*receipt)); sh_resource_instances_origin_unhold(origin); return complete;
}
static void vtp_native_release(void *provider)
{
    int slot=vtp_slot(provider), returned=0; uint64_t serial=0, token=0;
    vtp_source *source=g_vt_source;
    int constructing=source && source->provider==provider;
    if(slot>=0) {
        AcquireSRWLockExclusive(&g_vt_origin_lock);
        vtp_generation *g=g_vt_generation+slot;
        if(!constructing) g->serial=vtp_serial();
        g->complete=0; g->busy=1; serial=g->serial; token=g->token;
        ReleaseSRWLockExclusive(&g_vt_origin_lock);
    }
    __try { g_vt_release(provider); returned=1; }
    __finally {
        if(returned && slot>=0) {
            sh_resource_instances_forget(provider);
            sh_resource_instances_destroyed(provider,token,1);
            vtp_retired_storage(provider);
            token=sh_resource_instances_construct(provider,1);
            AcquireSRWLockExclusive(&g_vt_origin_lock);
            if(g_vt_generation[slot].serial==serial) {
                g_vt_generation[slot].token=token; g_vt_generation[slot].busy=constructing;
            }
            ReleaseSRWLockExclusive(&g_vt_origin_lock);
            if(constructing) { source->released=1; source->released_observations=source->observations; }
        }
        else if(slot>=0) {
            AcquireSRWLockExclusive(&g_vt_origin_lock);
            if(g_vt_generation[slot].serial==serial) g_vt_generation[slot].busy=0;
            ReleaseSRWLockExclusive(&g_vt_origin_lock);
        }
    }
}
static unsigned char vtp_native_set_source(void *provider,const char *name,const char *path,
    int mode,int kind,unsigned char direct,void *reader)
{
    vtp_source fallback={0}, *source=calloc(1,sizeof(*source));
    int allocated=source!=NULL, returned=0; unsigned char result=0;
    if(!source) { source=&fallback; source->failed=1; }
    source->recordable=allocated;
    source->provider=provider; source->slot=vtp_slot(provider); source->parent=g_vt_source;
    source->serial=vtp_serial(); source->failed|=reader!=NULL || source->slot<0 || !source->serial;
    if(source->slot>=0) {
        AcquireSRWLockExclusive(&g_vt_origin_lock);
        g_vt_generation[source->slot].serial=source->serial;
        g_vt_generation[source->slot].complete=0; g_vt_generation[source->slot].busy=1;
        ReleaseSRWLockExclusive(&g_vt_origin_lock);
    }
    g_vt_source=source;
    __try { result=g_vt_set_source(provider,name,path,mode,kind,direct,reader); returned=1; }
    __finally {
        int ordered=g_vt_source==source, complete=0; uint64_t token=0;
        if(ordered) g_vt_source=source->parent;
        if(source->slot>=0) {
            AcquireSRWLockShared(&g_vt_origin_lock);
            if(g_vt_generation[source->slot].serial==source->serial) token=g_vt_generation[source->slot].token;
            ReleaseSRWLockShared(&g_vt_origin_lock);
        }
        if(returned && result && ordered && source->released && !source->failed && source->count && token) {
            sh_resource_instance_origin *origin=sh_resource_instances_origin_composite(source->origins,source->count);
            if(origin) {
                complete=sh_resource_instances_payload(provider,token,origin) && sh_resource_instances_publish(provider,origin);
                sh_resource_instances_origin_release(origin);
            }
        }
        if(source->slot>=0) {
            AcquireSRWLockExclusive(&g_vt_origin_lock);
            if(g_vt_generation[source->slot].serial==source->serial) {
                g_vt_generation[source->slot].complete=complete; g_vt_generation[source->slot].busy=0;
            } else complete=0;
            ReleaseSRWLockExclusive(&g_vt_origin_lock);
        }
        /* A verified later Release already destroyed every observed input.
         * Otherwise a partial/failed call keeps known input shares until that
         * storage's actual Release; never drop them on an exception. */
        int released_inputs=returned && !result && source->released &&
            source->released_observations==source->observations;
        if(allocated && source->count && !complete && !released_inputs) {
            AcquireSRWLockExclusive(&g_vt_origin_lock);
            source->retained_next=g_vt_retained; g_vt_retained=source;
            ReleaseSRWLockExclusive(&g_vt_origin_lock);
        } else if(allocated) vtp_source_free(source);
    }
    return result;
}
typedef struct vtp_init_generation { uint64_t serial, token; int complete; } vtp_init_generation;
static void vtp_init_origins_begin(vtp_init_generation saved[SH_VT_PROVIDER_SLOTS])
{
    AcquireSRWLockExclusive(&g_vt_origin_lock);
    for(int slot=0;slot<SH_VT_PROVIDER_SLOTS;slot++) {
        vtp_generation *g=g_vt_generation+slot;
        saved[slot].token=g->token; saved[slot].complete=g->complete && !g->busy;
        saved[slot].serial=g->serial=vtp_serial();
        g->busy=1; g->complete=0;
    }
    ReleaseSRWLockExclusive(&g_vt_origin_lock);
}
static void vtp_init_origins_finish(const vtp_init_generation saved[SH_VT_PROVIDER_SLOTS],int returned)
{
    AcquireSRWLockExclusive(&g_vt_origin_lock);
    for(int slot=0;slot<SH_VT_PROVIDER_SLOTS;slot++) {
        vtp_generation *g=g_vt_generation+slot;
        if(!returned) {
            g->serial=vtp_serial(); g->complete=0; g->busy=0;
        } else if(g->serial==saved[slot].serial && g->token==saved[slot].token) {
            /* Paired Init resets numerical page-cache nodes, not these
             * providers' source storage. Only actual Release/SetSource may
             * supersede its token/serial. Keep its exact construction origin
             * while rejecting every receipt that straddled Init. */
            g->complete=saved[slot].complete && g->serial!=0; g->busy=0;
        }
    }
    ReleaseSRWLockExclusive(&g_vt_origin_lock);
    /* No source payload is released here. A fresh SetSource inside successful
     * Init has its own serial and remains complete; failed Init retains every
     * old/new payload but cannot issue a complete read receipt. */
}

static int vtp_fail(char *error, size_t capacity, const char *reason, const char *name)
{
    if (error && capacity) snprintf(error, capacity, "%s%s%s", reason, name ? ": " : "", name ? name : "");
    return 0;
}
static int vtp_nibble(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
/* Space-separated hex bytes; ?? matches any byte. */
static int vtp_match(const uint8_t *code, const char *pattern)
{
    for (;;) {
        int high, low;
        while (*pattern == ' ') pattern++;
        if (!*pattern) return 1;
        if (pattern[0] == '?') { pattern += 2; code++; continue; }
        high = vtp_nibble(pattern[0]); low = high < 0 ? -1 : vtp_nibble(pattern[1]);
        if (low < 0 || *code != (uint8_t)(high << 4 | low)) return 0;
        pattern += 2; code++;
    }
}
static const uint8_t *vtp_target(const uint8_t *instruction, size_t displacement, size_t length)
{
    int32_t value; memcpy(&value, instruction + displacement, 4);
    return instruction + length + value;
}
static const uint8_t *vtp_signature(const sig_result *results, size_t count, const char *name)
{
    for (size_t i = 0; results && i < count; i++)
        if (results[i].name && !strcmp(results[i].name, name) && results[i].addr &&
            (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)) return (const uint8_t *)results[i].addr;
    return NULL;
}

int sh_vt_provider_decode(const sig_result *results, size_t count, void *const *resources, sh_vt_native *out)
{
    /* The native per-material bind is identical in both renderers apart from
     * its system displacement and selector call. */
    static const char bind[] = "40 53 48 83 EC 20 48 83 B9 E0 00 00 00 00 48 8B D9 74 54 48 8B D1 "
        "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 74 40 48 83 B8 D0 02 00 00 00 74 36 "
        "8B 88 C0 02 00 00 89 8B D8 00 00 00 8B 08 89 8B DC 00 00 00";
    /* Selector heads through the exact _vmtr return. OpenGL alone turns a
     * nonzero material mode into sparse mode 4 under two conditions. */
    static const char vulkan[] = "41 55 41 57 48 81 EC 88 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
        "48 89 84 24 50 01 00 00 4C 8B BA E0 00 00 00 4C 8B E9 48 89 54 24 40 4D 85 FF "
        "0F 84 ?? ?? ?? ?? 41 80 3F 00 0F 84 ?? ?? ?? ?? 4C 89 A4 24 68 01 00 00 49 8B CF "
        "44 8B A2 E8 00 00 00 48 8D 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 75 0C 49 8B 85 ?? ?? ?? ?? E9";
    static const char opengl[] = "41 54 41 55 48 81 EC 88 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
        "48 89 84 24 50 01 00 00 4C 8B A2 E0 00 00 00 4C 8B E9 48 89 54 24 40 4D 85 E4 "
        "0F 84 ?? ?? ?? ?? 41 80 3C 24 00 0F 84 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? 00 "
        "4C 89 BC 24 60 01 00 00 44 8B BA E8 00 00 00 74 15 83 3D ?? ?? ?? ?? 00 74 0C "
        "45 85 FF B8 04 00 00 00 44 0F 45 F8 48 8D 15 ?? ?? ?? ?? 49 8B CC E8 ?? ?? ?? ?? "
        "85 C0 75 0C 49 8B 85 ?? ?? ?? ?? E9";
    /* Existing-name reuse: name +0x1a0, prepared mode object +0x3d8. */
    static const char reuse[] = "49 8B 1E 49 8B ?? 48 8B 8B A0 01 00 00 E8 ?? ?? ?? ?? 85 C0 75 17 "
        "48 83 BB D8 03 00 00 00 74 0D 48 8B CB E8";
    /* Missing name: release slot 31 when its path is nonempty, then scan +0x70. */
    static const char evict[] = "49 8B 8D ?? ?? ?? ?? 48 8B 41 70 80 38 00 74 05 E8 ?? ?? ?? ?? "
        "48 8B D5 48 8B 07 48 8B 48 70 80 39 00 74 0F";
    /* The pages-path SetSource call: kind = 2 - (mode != 0), then two zeroes. */
    static const char construct[] = "4C 8D 44 24 50 1B C9 49 8B ?? 83 C1 02 89 4C 24 20 48 8B CF E8";
    static const char set_vulkan[] = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 A8 BE FF FF B8 58 42 00 00 E8";
    static const char set_opengl[] = "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B8 BE FF FF B8 48 42 00 00 E8";
    static const char release[] = "48 8B F1 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CE E8";
    sh_vt_native native = {0};
    const uint8_t *rebind, *material, *selector, *set = NULL, *released = NULL;
    size_t reuse_at, evict_at, table;
    int gl;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    rebind = vtp_signature(results, count, "MaterialVirtualTextureRebind");
    if (!resources || !rebind) return 0;
    __try {
        /* The rebind walk passes one material to its bind helper at +0x59. */
        if (!vtp_match(rebind + 0x50, "48 8B 4C 24 28 48 8B 0C 0F E8")) return 0;
        material = vtp_target(rebind + 0x59, 1, 5);
        if (!vtp_match(material, bind)) return 0;
        native.system = (unsigned char *)vtp_target(material + 0x16, 3, 7);
        selector = vtp_target(material + 0x1d, 1, 5);
        gl = vtp_match(selector, opengl);
        if (!gl && !vtp_match(selector, vulkan)) return 0;
        {
            int32_t displacement; memcpy(&displacement, selector + (gl ? 0x84 : 0x64), 4);
            if (displacement < 8) return 0;
            table = (size_t)displacement - 8;
        }
        if (table % SH_VT_PROVIDER_SLOTS) return 0;
        native.stride = table / SH_VT_PROVIDER_SLOTS;
        if (native.stride < 0x400 || native.stride > 0x800) return 0;
        reuse_at = gl ? 0xd0 : 0xb0; evict_at = gl ? 0x10a : 0xea;
        if (!vtp_match(selector + reuse_at, reuse) || !vtp_match(selector + evict_at, evict)) return 0;
        {
            int32_t last; memcpy(&last, selector + evict_at + 3, 4);
            if ((size_t)last != table + (SH_VT_PROVIDER_SLOTS - 1) * sizeof(void *)) return 0;
        }
        released = vtp_target(selector + evict_at + 16, 1, 5);
        for (size_t at = evict_at + 0x100; at < evict_at + 0x220; at++)
            if (vtp_match(selector + at, construct)) {
                if (set) return 0;
                set = vtp_target(selector + at + 20, 1, 5);
            }
        if (!set || !vtp_match(set, gl ? set_opengl : set_vulkan)) return 0;
        {
            const uint8_t *site = NULL;
            for (size_t at = 0x40; at < 0x60; at++) if (vtp_match(set + at, release)) { site = set + at; break; }
            if (!site || vtp_target(site + 3, 3, 7) != native.system ||
                vtp_target(site + 18, 1, 5) != released) return 0;
            native.quiesce = (void (*)(void *))vtp_target(site + 10, 1, 5);
        }
        native.set_source = (unsigned char (*)(void *, const char *, const char *, int, int, unsigned char, void *))set;
        native.release = (void (*)(void *))released;
        if (gl) {
            native.sparse_function = (const volatile uint64_t *)vtp_target(selector + 0x40, 3, 8);
            native.sparse_enabled = (const volatile int32_t *)vtp_target(selector + 0x59, 2, 7);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    native.resources = resources;
    *out = native; return 1;
}

static unsigned char *vtp_provider(int slot) { return g_vt.system + (size_t)slot * g_vt.stride; }
static const char *vtp_text(const unsigned char *provider, size_t offset)
{ return *(const char *const *)(provider + offset); }
static int vtp_empty(const unsigned char *provider)
{
    const char *path = vtp_text(provider, VTP_PATH), *name = vtp_text(provider, VTP_NAME);
    return path && name && !*path && !*name;
}
/* The table follows the 32 inline providers and addresses each of them. */
static int vtp_table(void)
{
    void *const *table = (void *const *)(g_vt.system + SH_VT_PROVIDER_SLOTS * g_vt.stride);
    for (int slot = 1; slot < SH_VT_PROVIDER_SLOTS; slot++)
        if (table[slot] != vtp_provider(slot)) return 0;
    return 1;
}
static int vtp_mode(void)
{
    return g_vt.sparse_function && g_vt.sparse_enabled && *g_vt.sparse_function && *g_vt.sparse_enabled ? 4 : 2;
}
static int vtp_constructed(int slot, const char *name, int mode)
{
    const unsigned char *provider = vtp_provider(slot);
    const char *current = vtp_text(provider, VTP_NAME);
    const int *object = *(const int *const *)(provider + VTP_MODE);
    return current && !_stricmp(current, name) && *(const int *)(provider + VTP_KIND) == 2 &&
        *(void *const *)(provider + VTP_OWNER) && object && *object == mode &&
        *(const int *)(provider + VTP_SLOT) == slot;
}
static void vtp_release(int slot)
{
    void *provider=vtp_provider(slot), *system=g_vt.system;
    void (*quiesce)(void *)=g_vt.quiesce, (*release)(void *)=g_vt.release;
    g_vt_native_busy++;
    ReleaseSRWLockExclusive(&g_vt_lock);
    __try { quiesce(system); release(provider); }
    __finally { AcquireSRWLockExclusive(&g_vt_lock); g_vt_native_busy--; }
}
static unsigned char vtp_set_source(int slot,const char *name,const char *path,int mode)
{
    void *provider=vtp_provider(slot); unsigned char result=0;
    unsigned char (*set)(void *,const char *,const char *,int,int,unsigned char,void *)=g_vt.set_source;
    g_vt_native_busy++;
    ReleaseSRWLockExclusive(&g_vt_lock);
    __try { result=set(provider,name,path,mode,2,0,NULL); }
    __finally { AcquireSRWLockExclusive(&g_vt_lock); g_vt_native_busy--; }
    return result;
}
/* 1 when a parsed material still selects this slot, 0 when none does, -1
 * when the material list cannot be proven complete. A cleared material has
 * no provider name; the engine's material reset writes both fields. */
static int vtp_selected(int slot)
{
    void *list = g_vt.resources ? *g_vt.resources : NULL;
    unsigned lists = 0;
    while (list && lists++ < 4096) {
        const unsigned char *manager = list;
        const char *type = *(const char *const *)(manager + 8);
        if (type && !_stricmp(type, "material")) {
            void *const *items = *(void *const *const *)(manager + 0x20);
            int count = *(const int *)(manager + 0x28);
            if (count < 0 || (count && !items)) return -1;
            for (int i = 0; i < count; i++) {
                const unsigned char *item = items[i];
                if (item && *(const int *)(item + VTM_SLOT) == slot && *(void *const *)(item + VTM_NAME)) return 1;
            }
        }
        list = *(void *const *)(manager + 0x18);
    }
    return list ? -1 : 0;
}

int sh_vt_provider_name_valid(const char *name)
{
    size_t length = name ? strnlen(name, 32) : 32;
    if (!length || length > 31) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x21 || c > 0x7e || c == '"' || c == '\\') return 0;
    }
    for (size_t i = 0; i + 5 <= length; i++) if (!_strnicmp(name + i, "_vmtr", 5)) return 0;
    return 1;
}
int sh_vt_provider_bind(const sh_vt_native *native)
{
    if (!native || !native->system || !native->stride || !native->set_source || !native->release ||
        !native->quiesce || !native->resources || (!native->sparse_function) != (!native->sparse_enabled)) return 0;
    AcquireSRWLockExclusive(&g_vt_lock);
    g_vt = *native; memset(g_owned, 0, sizeof(g_owned));
    ReleaseSRWLockExclusive(&g_vt_lock);
    InterlockedExchange(&g_vt_ready, 1); return 1;
}
int sh_vt_provider_ready(void) { return InterlockedCompareExchange(&g_vt_ready, 0, 0) != 0; }
int sh_vt_provider_initialized(void)
{
    int initialized = 0;
    if (!sh_vt_provider_ready()) return 0;
    __try {
        const unsigned char *global = vtp_provider(1);
        const char *name = vtp_text(global, VTP_NAME);
        initialized = vtp_table() && name && *name && *(void *const *)(global + VTP_MODE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { initialized = 0; }
    return initialized;
}

static int vtp_acquire_locked(const char *const *names, size_t count, int *created, char *error, size_t capacity)
{
    int mode = vtp_mode();
    for (size_t i = 0; i < count; i++) {
        const char *name = names[i];
        int slot = 0;
        char path[64];
        if (!sh_vt_provider_name_valid(name)) return vtp_fail(error, capacity, "invalid private page provider name", name);
        for (int s = SH_VT_PRIVATE_FIRST; s <= SH_VT_PRIVATE_LAST && !slot; s++)
            if (g_owned[s].held && !_stricmp(g_owned[s].name, name)) slot = s;
        if (slot) {
            if (!vtp_constructed(slot, name, mode))
                return vtp_fail(error, capacity, "owned page provider no longer matches its construction", name);
            continue;
        }
        for (int s = 1; s < SH_VT_PROVIDER_SLOTS; s++) {
            const char *existing = vtp_text(vtp_provider(s), VTP_NAME);
            if (existing && !_stricmp(existing, name))
                return vtp_fail(error, capacity, "an engine page provider already uses this private name", name);
        }
        for (int s = SH_VT_PRIVATE_FIRST; s <= SH_VT_PRIVATE_LAST && !slot; s++)
            if (!g_owned[s].held && vtp_empty(vtp_provider(s)) && *(const int *)(vtp_provider(s) + VTP_SLOT) == s) slot = s;
        if (!slot) return vtp_fail(error, capacity, "no empty page provider slot remains below the engine's eviction slot", name);
        snprintf(path, sizeof(path), "virtualtextures/%s.pages", name);
        /* Kind 2 builds the VMTR owner the row lookup needs. Mode 2 names its
         * page table after this private path; mode 0 would take the global
         * atlas's literal page-table image. */
        if (!vtp_set_source(slot, name, path, mode) ||
            !vtp_constructed(slot, name, mode)) {
            if (!vtp_empty(vtp_provider(slot))) vtp_release(slot);
            return vtp_fail(error, capacity, "native page provider construction failed", name);
        }
        g_owned[slot].held = 1; created[slot] = 1;
        memcpy(g_owned[slot].name, name, strlen(name) + 1);
    }
    return 1;
}
int sh_vt_provider_acquire(const char *const *names, size_t count, char *error, size_t capacity)
{
    int created[SH_VT_PROVIDER_SLOTS] = {0}, ok = 0;
    if (error && capacity) *error = 0;
    if (!count) return 1;
    if (!names || !sh_vt_provider_ready())
        return vtp_fail(error, capacity, "private virtual texture page providers are unavailable", NULL);
    /* Before engine initialization the VT init hook acquires wanted names. */
    if (!sh_vt_provider_initialized()) return 1;
    AcquireSRWLockExclusive(&g_vt_lock);
    if(g_vt_native_busy) { ReleaseSRWLockExclusive(&g_vt_lock);
        return vtp_fail(error,capacity,"native page provider operation is in progress",NULL); }
    __try { ok = vtp_acquire_locked(names, count, created, error, capacity); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = vtp_fail(error, capacity, "native page provider construction raised an exception", NULL);
    }
    if (!ok) for (int s = SH_VT_PRIVATE_FIRST; s <= SH_VT_PRIVATE_LAST; s++) if (created[s]) {
        __try { vtp_release(s); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        g_owned[s].held = 0;
    }
    ReleaseSRWLockExclusive(&g_vt_lock);
    return ok;
}
size_t sh_vt_provider_collect(int (*keep)(void *, const char *), void *context)
{
    size_t held = 0;
    if (!sh_vt_provider_ready()) return 0;
    AcquireSRWLockExclusive(&g_vt_lock);
    if(g_vt_native_busy) {
        for(int s=SH_VT_PRIVATE_FIRST;s<=SH_VT_PRIVATE_LAST;s++) held+=g_owned[s].held!=0;
        ReleaseSRWLockExclusive(&g_vt_lock); return held;
    }
    for (int s = SH_VT_PRIVATE_FIRST; s <= SH_VT_PRIVATE_LAST; s++) if (g_owned[s].held) {
        int selected = -1;
        if (keep && keep(context, g_owned[s].name)) { held++; continue; }
        __try {
            selected = vtp_selected(s);
            if (!selected) { vtp_release(s); g_owned[s].held = 0; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { selected = -1; }
        if (g_owned[s].held) held++;
    }
    ReleaseSRWLockExclusive(&g_vt_lock);
    return held;
}
void sh_vt_provider_reinitialized(void)
{
    if (!sh_vt_provider_ready()) return;
    AcquireSRWLockExclusive(&g_vt_lock);
    if(g_vt_native_busy) { ReleaseSRWLockExclusive(&g_vt_lock); return; }
    for (int s = SH_VT_PRIVATE_FIRST; s <= SH_VT_PRIVATE_LAST; s++) if (g_owned[s].held) {
        __try {
            const char *name = vtp_text(vtp_provider(s), VTP_NAME);
            if (name && !_stricmp(name, g_owned[s].name)) vtp_release(s);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        g_owned[s].held = 0;
    }
    ReleaseSRWLockExclusive(&g_vt_lock);
}
void sh_vt_provider_on_initialize(sh_vt_provider_wanted_fn wanted) { g_vt_wanted = wanted; }

static void vtp_init_hook(void *system)
{
    vtp_init_generation saved[SH_VT_PROVIDER_SLOTS];
    int tracking=(unsigned char *)system==g_vt.system && sh_vt_provider_provenance_ready(), returned=0;
    if(tracking) vtp_init_origins_begin(saved);
    __try { g_vt_init(system); returned=1; }
    __finally { if(tracking) vtp_init_origins_finish(saved,returned); }
    if ((unsigned char *)system != g_vt.system) return;
    sh_vt_provider_reinitialized();
    if (g_vt_wanted) {
        char error[512] = "";
        if (!g_vt_wanted(error, sizeof(error)))
            backend_log(*error ? error : "private virtual texture page providers were not rebuilt");
    }
}
int sh_vt_provider_install(const sig_result *results, size_t count, void *const *resources)
{
    sh_vt_native native;
    const uint8_t *init;
    if (InterlockedCompareExchange(&g_vt_attempted, 1, 0)) return sh_vt_provider_ready();
    if (!sh_vt_provider_decode(results, count, resources, &native)) goto failed;
    init = vtp_signature(results, count, "VirtualTextureSystemInit");
    if (!init) goto failed;
    /* Five pushes and the frame setup: 18 complete bytes with no relative
     * operand, ending before the stack probe call in both renderers. */
    g_vt_init = (vtp_init_fn)hook_prepare((void *)init, (void *)vtp_init_hook, 18);
    /* The SetSource prefix ends at its stack-frame LEA, before MOV EAX and
     * the stack-probe call. Release ends at its stack allocation. */
    const uint8_t *released=(const uint8_t *)native.release;
    size_t release_length=0;
    if(vtp_match(released,"48 8B C4 55 57 41 56 48 8D 6C 24 90 48 81 EC 70 01 00 00")) release_length=19;
    else if(vtp_match(released,"48 8B C4 55 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00")) release_length=20;
    if(!release_length) goto failed;
    g_vt_set_source=(unsigned char (*)(void *,const char *,const char *,int,int,unsigned char,void *))
        hook_prepare((void *)native.set_source,vtp_native_set_source,21);
    g_vt_release=(void (*)(void *))hook_prepare((void *)native.release,vtp_native_release,release_length);
    if (!g_vt_init || !g_vt_set_source || !g_vt_release || !sh_vt_provider_bind(&native) ||
        hook_commit((void *)g_vt_set_source)!=B2_PATCH_OK || hook_commit((void *)g_vt_release)!=B2_PATCH_OK ||
        hook_commit((void *)g_vt_init)!=B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_vt_provenance,1);
    return 1;
failed:
    InterlockedExchange(&g_vt_provenance,0);
    if(g_vt_init) (void)hook_unpatch((void *)g_vt_init);
    if(g_vt_release) (void)hook_unpatch((void *)g_vt_release);
    if(g_vt_set_source) (void)hook_unpatch((void *)g_vt_set_source);
    InterlockedExchange(&g_vt_ready,0);
    backend_log("private virtual texture page providers unavailable"); return 0;
}

#ifdef SH_VT_PROVIDER_TESTING
void sh_vt_provider_test_reset(void)
{
    for(int slot=0;slot<SH_VT_PROVIDER_SLOTS;slot++) {
        void *provider=g_vt.system ? vtp_provider(slot) : NULL;
        if(provider) { sh_resource_instances_forget(provider);
            sh_resource_instances_destroyed(provider,g_vt_generation[slot].token,1); vtp_retired_storage(provider); }
    }
    memset(g_vt_generation,0,sizeof(g_vt_generation));
    InterlockedExchange(&g_vt_provenance,0);
    memset(&g_vt, 0, sizeof(g_vt)); memset(g_owned, 0, sizeof(g_owned));
    g_vt_wanted = NULL; g_vt_init = NULL;
    InterlockedExchange(&g_vt_ready, 0); InterlockedExchange(&g_vt_attempted, 0);
}
int sh_vt_provider_test_provenance(void)
{
    if(!sh_vt_provider_ready() || sh_vt_provider_provenance_ready()) return 0;
    g_vt_set_source=g_vt.set_source; g_vt_release=g_vt.release;
    g_vt.set_source=vtp_native_set_source; g_vt.release=vtp_native_release;
    InterlockedExchange(&g_vt_provenance,1); return 1;
}
void sh_vt_provider_test_release(void *provider) { vtp_native_release(provider); }
unsigned char sh_vt_provider_test_set_source(void *provider,const char *name,const char *path,
    int mode,int kind,unsigned char direct,void *reader)
{ return vtp_native_set_source(provider,name,path,mode,kind,direct,reader); }
void sh_vt_provider_test_init(void (*initialize)(void *))
{ g_vt_init=initialize; vtp_init_hook(g_vt.system); }
#endif
