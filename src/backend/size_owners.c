#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "size_owners.h"
#include "execution_context.h"
#include "resource_graph.h"

/* idSWFSpriteInstance, both renderers (R-309): vtable +0, movie +0x20, material +0x60, width +0x68 and height
 * +0x6a as shorts. Script MovieClip objects hold their sprite at +0xc0 (the paired material setters and getters).
 * The codex content update is vtable slot 3 of both widget classes on both renderers. */
enum { SO_MOVIE = 0x20, SO_MATERIAL = 0x60, SO_WIDTH = 0x68, SO_HEIGHT = 0x6a, SO_SCRIPT_SPRITE = 0xc0 };
enum { SO_WIDGET_UPDATE_SLOT = 3 };

typedef struct so_names { char **items; size_t count; } so_names;
typedef struct so_axis { int derived; void *material; short value; so_names images; } so_axis;
typedef struct so_sprite { void *sprite, *vtable, *movie, *material; so_axis axis[2]; } so_sprite;
/* Copies held outside a sprite's own fields: a font (by name), a hotBox, a codex widget, a credits item,
 * and the script state of a movie that read a derived axis. */
enum { SO_FONT, SO_HOTBOX, SO_WIDGET, SO_CREDITS, SO_SCRIPT, SO_KINDS };
typedef struct so_owner { void *object, *vtable, *movie; char *name; so_names images; } so_owner;
typedef struct so_list { so_owner *items; size_t count, capacity; } so_list;
static const char *const g_kind_names[SO_KINDS] = {"font", "hotBox geometry", "codex widget", "credits item",
    "movie script"};

static SRWLOCK g_lock = SRWLOCK_INIT;
static so_sprite *g_sprites;
static size_t g_sprite_count, g_sprite_capacity;
static so_list g_owners[SO_KINDS];
static volatile LONG g_enabled;
static sh_size_measure g_measure;
/* In-flight attribution, per execution context (size_owners.h). Within one context brackets nest strictly; a fiber
 * keeps its open brackets and its pending forward reads (the MP medal's sizes, waiting for the SetMaterial its
 * caller makes next) while another fiber runs on the thread, or after it resumes on another. Records are this
 * module's memory, so a context that retires releases them without its stack. */
typedef struct so_frame {
    uint64_t serial;
    sh_size_bracket_kind kind;
    void *object;
    char *owner;                    /* FONT: the font's resource name */
    const void *frame;              /* SPRITE: the caller's frame, which matches forward reads */
    int width, height;              /* SPRITE: the explicit-axis arguments (negative derives that axis) */
    sh_size_read *reads, *forward;
    size_t count, capacity, forward_count;
    void *movie;                    /* WIDGET / CREDITS: the movie of a sprite the layout wrote */
    unsigned axis;                  /* COLLECT: the getter's axis (object is its material) */
    int failed;
    struct so_frame *next;          /* taken out of its context: the record it was opened inside, if taken too */
} so_frame;
typedef struct so_context {
    uint64_t id;
    so_frame **open;                /* innermost last */
    size_t count, capacity;
    sh_size_read *pending;
    size_t pending_count, pending_capacity;
    const void *pending_frame;
} so_context;
static SRWLOCK g_context_lock = SRWLOCK_INIT;
static so_context *g_contexts;
static size_t g_context_count, g_context_capacity;
static volatile LONG64 g_serial;
/* Open collections in every context: a find outside all of them looks nothing up. */
static volatile LONG g_collecting;
/* A context entry or bracket record could not be allocated: no attribution can be shown any more (fail closed). */
static volatile LONG g_context_damaged;
static void so_retire(uint64_t id);
static sh_execution_owner g_execution = {so_retire};
#ifdef SH_SIZE_OWNERS_TESTING
static volatile LONG g_frames;      /* records not yet released */
#define SO_FRAMES(delta) InterlockedAdd(&g_frames, (delta))
#else
#define SO_FRAMES(delta) ((void)0)
#endif

/* The 14 direct callers of each getter: call RVAs from the paired census (getter-call-sites.json), each
 * checked at bind time to be E8 rel32 to the resolved getter. */
typedef struct so_site { uint32_t rva; char getter; sh_size_site site; } so_site;
#define SO_T SH_SIZE_SITE_TRANSIENT
#define SO_S SH_SIZE_SITE_SPRITE
#define SO_F SH_SIZE_SITE_FORWARD
#define SO_W SH_SIZE_SITE_WIDGET
#define SO_C SH_SIZE_SITE_CREDITS
#define SO_H SH_SIZE_SITE_HOTBOX
#define SO_N SH_SIZE_SITE_FONT
static const so_site g_vk_sites[] = {
    {0x292c89, 'h', SO_T}, {0x292c9a, 'w', SO_T},   /* SWF shape draw 0x1402926b0 */
    {0x2ae388, 'w', SO_S}, {0x2ae395, 'h', SO_S},   /* script material setter 0x1402ae2d0 */
    {0x2ae8fb, 'w', SO_S}, {0x2ae908, 'h', SO_S},   /* SetMaterial 0x1402ae8b0 */
    {0x4cfc67, 'w', SO_W}, {0x4cfc71, 'h', SO_W},   /* Dossier codex layout 0x1404cfbd0 */
    {0x4cfe8f, 'w', SO_W}, {0x4cfea9, 'h', SO_W},   /* 0x1404cfe00 */
    {0x4d052f, 'w', SO_W}, {0x4d0549, 'h', SO_W},   /* 0x1404d04a0 */
    {0x4d0901, 'w', SO_W}, {0x4d091b, 'h', SO_W},   /* 0x1404d0750 */
    {0x4d0d5d, 'w', SO_W}, {0x4d0d77, 'h', SO_W},   /* 0x1404d0a40 */
    {0xac78b2, 'w', SO_F}, {0xac78bc, 'h', SO_F},   /* MP medal 0x140ac7700 */
    {0xdf800e, 'w', SO_T}, {0xdf8019, 'h', SO_T},   /* idRender::RenderDebugTools 0x140df7e20 */
    {0xe477f1, 'w', SO_C}, {0xe47801, 'h', SO_C},   /* credits item 0x140e476e0 */
    {0x12e0350, 'w', SO_W}, {0x12e035a, 'h', SO_W}, /* Snap codex info box 0x1412e0290 */
    {0x1746480, 'w', SO_H}, {0x174649e, 'h', SO_H}, /* hotBox geometry 0x1417460f0 */
    {0x181a145, 'w', SO_N}, {0x181a159, 'h', SO_N}, /* FontRead 0x141819c30 */
};
static const so_site g_gl_sites[] = {
    {0x291f19, 'h', SO_T}, {0x291f2a, 'w', SO_T},   /* 0x140291940 */
    {0x2ad318, 'w', SO_S}, {0x2ad325, 'h', SO_S},   /* 0x1402ad260 */
    {0x2ad88b, 'w', SO_S}, {0x2ad898, 'h', SO_S},   /* 0x1402ad840 */
    {0x4cf727, 'w', SO_W}, {0x4cf731, 'h', SO_W},   /* 0x1404cf690 */
    {0x4cf94f, 'w', SO_W}, {0x4cf969, 'h', SO_W},   /* 0x1404cf8c0 */
    {0x4cffef, 'w', SO_W}, {0x4d0009, 'h', SO_W},   /* 0x1404cff60 */
    {0x4d03c1, 'w', SO_W}, {0x4d03db, 'h', SO_W},   /* 0x1404d0210 */
    {0x4d081d, 'w', SO_W}, {0x4d0837, 'h', SO_W},   /* 0x1404d0500 */
    {0xac6ad2, 'w', SO_F}, {0xac6adc, 'h', SO_F},   /* 0x140ac6920 */
    {0xdec07e, 'w', SO_T}, {0xdec089, 'h', SO_T},   /* 0x140debe90 */
    {0xe396d1, 'w', SO_C}, {0xe396e1, 'h', SO_C},   /* 0x140e395c0 */
    {0x12d2500, 'w', SO_W}, {0x12d250a, 'h', SO_W}, /* 0x1412d2440 */
    {0x1739120, 'w', SO_H}, {0x173913e, 'h', SO_H}, /* 0x141738d90 */
    {0x180cad5, 'w', SO_N}, {0x180cae9, 'h', SO_N}, /* 0x14180c5c0 */
};
enum { SO_SITE_LIMIT = 64 };
static uintptr_t g_returns[SO_SITE_LIMIT];
static sh_size_site g_classes[SO_SITE_LIMIT];
static size_t g_return_count;
static uintptr_t g_own_lo, g_own_hi;

static void so_names_free(so_names *names)
{
    for (size_t i = 0; i < names->count; i++) free(names->items[i]);
    free(names->items); names->items = NULL; names->count = 0;
}
/* Resource names compare as the resource graph folds them: ASCII case and '\\' as '/'. */
static int so_name_equal(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a == '\\' ? '/' : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b == '\\' ? '/' : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int so_names_has(const so_names *names, const char *image)
{
    for (size_t i = 0; i < names->count; i++) if (so_name_equal(names->items[i], image)) return 1;
    return 0;
}
/* 0 on allocation failure: the caller treats the reads as unattributed. */
static int so_names_add(so_names *names, const char *image)
{
    if (so_names_has(names, image)) return 1;
    char **grown = (char **)realloc(names->items, (names->count + 1) * sizeof(*grown));
    if (!grown) return 0;
    names->items = grown;
    if (!(names->items[names->count] = _strdup(image))) return 0;
    names->count++;
    return 1;
}
static int so_names_meet(const so_names *names, const char *const *images, size_t count)
{
    for (size_t i = 0; i < count; i++) if (images[i] && so_names_has(names, images[i])) return 1;
    return 0;
}
static void so_reads_free(sh_size_read *reads, size_t count)
{
    for (size_t i = 0; i < count; i++) free(reads[i].image);
    free(reads);
}
static int so_reads_add(sh_size_read **reads, size_t *count, size_t *capacity, const char *image, void *material,
    unsigned axis)
{
    if (*count == *capacity) {
        size_t grown_capacity = *capacity ? *capacity * 2 : 4;
        sh_size_read *grown = (sh_size_read *)realloc(*reads, grown_capacity * sizeof(*grown));
        if (!grown) return 0;
        *reads = grown; *capacity = grown_capacity;
    }
    char *copy = _strdup(image);
    if (!copy) return 0;
    (*reads)[(*count)++] = (sh_size_read){copy, material, axis};
    return 1;
}

int sh_size_owners_bind_sites(const uint8_t *base, int vulkan, const void *height, const void *width,
    const void *own_base, size_t own_size)
{
    const so_site *sites = vulkan ? g_vk_sites : g_gl_sites;
    size_t count = vulkan ? sizeof(g_vk_sites) / sizeof(g_vk_sites[0]) : sizeof(g_gl_sites) / sizeof(g_gl_sites[0]);
    uintptr_t returns[SO_SITE_LIMIT];
    if (!base || !height || !width || count > SO_SITE_LIMIT) return 0;
    __try {
        for (size_t i = 0; i < count; i++) {
            const uint8_t *call = base + sites[i].rva;
            int32_t displacement;
            if (call[0] != 0xe8) return 0;
            memcpy(&displacement, call + 1, sizeof(displacement));
            if (call + 5 + displacement != (const uint8_t *)(sites[i].getter == 'h' ? height : width)) return 0;
            returns[i] = (uintptr_t)(call + 5);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < count; i++) { g_returns[i] = returns[i]; g_classes[i] = sites[i].site; }
    g_return_count = count;
    g_own_lo = (uintptr_t)own_base; g_own_hi = (uintptr_t)own_base + own_size;
    ReleaseSRWLockExclusive(&g_lock);
    return 1;
}
void sh_size_owners_enable(sh_size_measure measure)
{
    g_measure = measure;
    InterlockedExchange(&g_enabled, measure != NULL && g_return_count != 0);
}
int sh_size_owners_enabled(void) { return InterlockedCompareExchange(&g_enabled, 0, 0) != 0; }
sh_size_site sh_size_owners_site(const void *return_address)
{
    uintptr_t at = (uintptr_t)return_address;
    if (!sh_size_owners_enabled()) return SH_SIZE_SITE_UNKNOWN;
    /* The table is written once before enable; reads need no lock. */
    for (size_t i = 0; i < g_return_count; i++) if (g_returns[i] == at) return g_classes[i];
    /* This module's own calls (the typeinfo material diagnostic) only report a value. */
    if (g_own_lo && at >= g_own_lo && at < g_own_hi) return SH_SIZE_SITE_TRANSIENT;
    return SH_SIZE_SITE_UNKNOWN;
}

static void so_escape_reads(const sh_size_read *reads, size_t count);
/* Under g_context_lock. */
static so_context *so_context_find(uint64_t id, int create)
{
    for (size_t i = 0; i < g_context_count; i++) if (g_contexts[i].id == id) return g_contexts + i;
    if (!create) return NULL;
    if (g_context_count == g_context_capacity) {
        size_t capacity = g_context_capacity ? g_context_capacity * 2 : 16;
        so_context *grown = (so_context *)realloc(g_contexts, capacity * sizeof(*grown));
        if (!grown) { InterlockedExchange(&g_context_damaged, 1); return NULL; }
        g_contexts = grown; g_context_capacity = capacity;
    }
    so_context *context = g_contexts + g_context_count++;
    memset(context, 0, sizeof(*context));
    context->id = id;
    return context;
}
static void so_context_release(so_context *context)
{
    if (context->count || context->pending_count) return;
    free(context->open);
    free(context->pending);
    *context = g_contexts[--g_context_count];
}
static void so_pending_take(so_context *context, sh_size_read **reads, size_t *count)
{
    *reads = context->pending; *count = context->pending_count;
    context->pending = NULL; context->pending_count = context->pending_capacity = 0; context->pending_frame = NULL;
}
static void so_frame_free(so_frame *frame)
{
    so_reads_free(frame->reads, frame->count);
    so_reads_free(frame->forward, frame->forward_count);
    free(frame->owner);
    free(frame);
    SO_FRAMES(-1);
}
/* The context entry of an open bracket when the calling context (id) began it. Under g_context_lock. */
static so_context *so_context_of(const sh_size_bracket *bracket, uint64_t id)
{
    return id && bracket->serial && id == bracket->context ? so_context_find(id, 0) : NULL;
}
/* Take a bracket's record out of its context together with every record opened above it, which belong to frames
 * that already returned. The list runs innermost first, each record's next being the one it was opened inside, and
 * ends with the bracket's own record. movie, the movie of the sprite the bracket wrote, goes to the record it was
 * opened inside when that one has none. 1 when it was the innermost. Under g_context_lock. */
static int so_unlink(so_context *context, uint64_t serial, void *movie, so_frame **taken)
{
    size_t i = context->count;
    *taken = NULL;
    while (i && context->open[i - 1]->serial != serial) i--;
    if (!i) return 0;
    int in_order = i == context->count;
    if (movie && i > 1 && !context->open[i - 2]->movie) context->open[i - 2]->movie = movie;
    for (size_t j = i - 1; j < context->count; j++) {
        so_frame *frame = context->open[j];
        if (frame->kind == SH_SIZE_BRACKET_COLLECT) InterlockedDecrement(&g_collecting);
        frame->next = *taken;
        *taken = frame;
    }
    context->count = i - 1;
    return in_order;
}
static void so_begin(sh_size_bracket *bracket, sh_size_bracket_kind kind, void *object, const char *owner,
    const void *frame, int width, int height, unsigned axis)
{
    /* The identifier first: no FLS call is made under this module's lock. */
    uint64_t id = sh_execution_context(&g_execution, 1);
    so_frame *record = id ? (so_frame *)calloc(1, sizeof(*record)) : NULL;
    sh_size_read *dropped = NULL;
    size_t dropped_count = 0;
    memset(bracket, 0, sizeof(*bracket));
    bracket->kind = kind; bracket->object = object; bracket->axis = axis;
    /* No identifier: this context records nothing, so none of its reads can reach an owner. */
    if (!id) return;
    if (record) {
        SO_FRAMES(1);
        record->kind = kind; record->object = object; record->frame = frame;
        record->width = width; record->height = height; record->axis = axis;
        if (owner && !(record->owner = _strdup(owner))) record->failed = 1;
    }
    AcquireSRWLockExclusive(&g_context_lock);
    so_context *context = record ? so_context_find(id, 1) : NULL;
    if (context && context->count == context->capacity) {
        size_t capacity = context->capacity ? context->capacity * 2 : 8;
        so_frame **grown = (so_frame **)realloc(context->open, capacity * sizeof(*grown));
        if (grown) { context->open = grown; context->capacity = capacity; }
    }
    if (!context || context->count == context->capacity || !(record->serial = sh_execution_next(&g_serial))) {
        /* An unrecorded routine's reads would reach an outer owner of this context: none is shown any more. */
        InterlockedExchange(&g_context_damaged, 1);
        if (context) so_context_release(context);
    } else {
        if (kind == SH_SIZE_BRACKET_SPRITE && context->pending_count) {
            /* The medal reads both sizes, then passes them to SetMaterial from the same frame. */
            if (context->pending_frame == frame) so_pending_take(context, &record->forward, &record->forward_count);
            else so_pending_take(context, &dropped, &dropped_count);
        }
        context->open[context->count++] = record;
        if (kind == SH_SIZE_BRACKET_COLLECT) InterlockedIncrement(&g_collecting);
        bracket->context = id; bracket->serial = record->serial;
        record = NULL;
    }
    ReleaseSRWLockExclusive(&g_context_lock);
    if (record) so_frame_free(record);
    /* Forward reads another frame's SetMaterial did not take cannot be shown to stop at a refreshable copy. */
    so_escape_reads(dropped, dropped_count);
    so_reads_free(dropped, dropped_count);
}
void sh_size_owners_begin(sh_size_bracket *bracket, sh_size_bracket_kind kind, void *object, const char *owner,
    const void *frame, int width, int height)
{
    so_begin(bracket, kind, object, owner, frame, width, height, 0);
}
/* Reads no owner can take stay with the image's own history. */
static void so_escape_reads(const sh_size_read *reads, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (reads[i].image) sh_resource_graph_history_mark("image", reads[i].image, SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
}

/* Sprite records are found by pointer through an open-addressed index (slot value: record index + 1), kept
 * under half full, with backward-shift deletion: GUI movies hold many sprites, and each destruction looks one
 * up. Under g_lock. */
static size_t *g_index;
static size_t g_index_capacity;
static size_t so_hash(const void *sprite)
{
    unsigned long long x = (unsigned long long)(uintptr_t)sprite >> 4;
    x ^= x >> 17; x *= 0x9e3779b97f4a7c15ull;
    return (size_t)(x >> 13);
}
static size_t so_slot_of(const void *sprite)
{
    if (!g_index_capacity) return SIZE_MAX;
    for (size_t slot = so_hash(sprite) & (g_index_capacity - 1);; slot = (slot + 1) & (g_index_capacity - 1)) {
        size_t value = g_index[slot];
        if (!value) return SIZE_MAX;
        if (g_sprites[value - 1].sprite == sprite) return slot;
    }
}
static void so_index_put(size_t record)
{
    size_t slot = so_hash(g_sprites[record].sprite) & (g_index_capacity - 1);
    while (g_index[slot]) slot = (slot + 1) & (g_index_capacity - 1);
    g_index[slot] = record + 1;
}
/* 0 on allocation failure; the previous index stays valid. */
static int so_index_reserve(size_t records)
{
    size_t capacity = g_index_capacity ? g_index_capacity : 128;
    while (capacity < records * 2 + 2) capacity *= 2;
    if (capacity == g_index_capacity) return 1;
    size_t *grown = (size_t *)calloc(capacity, sizeof(*grown));
    if (!grown) return 0;
    free(g_index); g_index = grown; g_index_capacity = capacity;
    for (size_t i = 0; i < g_sprite_count; i++) so_index_put(i);
    return 1;
}
static void so_index_delete(size_t slot)
{
    size_t mask = g_index_capacity - 1, hole = slot;
    g_index[hole] = 0;
    for (size_t next = (hole + 1) & mask; g_index[next]; next = (next + 1) & mask) {
        size_t home = so_hash(g_sprites[g_index[next] - 1].sprite) & mask;
        /* Move the entry back when the hole lies between its home slot and its current slot. */
        if (((next - home) & mask) >= ((next - hole) & mask)) { g_index[hole] = g_index[next]; g_index[next] = 0; hole = next; }
    }
}
static size_t so_sprite_find(const void *sprite)
{
    size_t slot = so_slot_of(sprite);
    return slot == SIZE_MAX ? SIZE_MAX : g_index[slot] - 1;
}
static void so_sprite_free(so_sprite *record)
{
    so_names_free(&record->axis[0].images); so_names_free(&record->axis[1].images);
}
/* Take record i out of the table and the index; the last record moves into its place. */
static so_sprite so_sprite_take(size_t i)
{
    so_sprite taken = g_sprites[i];
    so_index_delete(so_slot_of(taken.sprite));
    size_t last = --g_sprite_count;
    if (i != last) {
        size_t slot = so_slot_of(g_sprites[last].sprite);
        g_sprites[i] = g_sprites[last];
        g_index[slot] = i + 1;
    }
    return taken;
}
static void so_sprite_remove(size_t i)
{
    so_sprite taken = so_sprite_take(i);
    so_sprite_free(&taken);
}
static size_t so_owner_find(const so_list *list, const void *object, const char *name)
{
    for (size_t i = 0; i < list->count; i++)
        if (name ? (list->items[i].name && so_name_equal(list->items[i].name, name)) : list->items[i].object == object)
            return i;
    return SIZE_MAX;
}
static void so_owner_remove(so_list *list, size_t i)
{
    so_names_free(&list->items[i].images); free(list->items[i].name);
    list->items[i] = list->items[--list->count];
}
/* Store or replace one owner's copies. merge keeps the earlier images too (a routine that raised, or the
 * accumulating script state of a movie). 0 on allocation failure. */
static int so_owner_store(so_list *list, void *object, const char *name, void *vtable, void *movie,
    const sh_size_read *reads, size_t count, int merge)
{
    size_t i = so_owner_find(list, object, name);
    if (i == SIZE_MAX) {
        if (list->count == list->capacity) {
            size_t capacity = list->capacity ? list->capacity * 2 : 16;
            so_owner *grown = (so_owner *)realloc(list->items, capacity * sizeof(*grown));
            if (!grown) return 0;
            list->items = grown; list->capacity = capacity;
        }
        i = list->count++;
        memset(list->items + i, 0, sizeof(list->items[i]));
        list->items[i].object = object;
        if (name && !(list->items[i].name = _strdup(name))) { list->count--; return 0; }
    } else if (!merge) so_names_free(&list->items[i].images);
    so_owner *owner = list->items + i;
    owner->vtable = vtable;
    if (movie) owner->movie = movie;
    for (size_t r = 0; r < count; r++) if (!so_names_add(&owner->images, reads[r].image)) return 0;
    return 1;
}

typedef struct so_sprite_state { void *vtable, *movie, *material; short width, height; } so_sprite_state;
static int so_sprite_read(void *sprite, so_sprite_state *state)
{
    __try {
        const unsigned char *p = (const unsigned char *)sprite;
        state->vtable = *(void *const *)p;
        state->movie = *(void *const *)(p + SO_MOVIE);
        state->material = *(void *const *)(p + SO_MATERIAL);
        state->width = *(const short *)(p + SO_WIDTH);
        state->height = *(const short *)(p + SO_HEIGHT);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void *so_script_sprite(void *object)
{
    if (!object) return NULL;
    __try { return *(void **)((unsigned char *)object + SO_SCRIPT_SPRITE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
/* A SetMaterial or script material setter completed: the sprite's provenance is now exactly what it set. */
static int so_sprite_written(const so_frame *bracket, void *sprite)
{
    so_sprite_state state;
    so_sprite record = {0};
    if (!sprite || !so_sprite_read(sprite, &state)) return 0;
    record.sprite = sprite; record.vtable = state.vtable; record.movie = state.movie; record.material = state.material;
    for (unsigned a = 0; a < 2; a++) {
        unsigned axis = a ? SH_SIZE_AXIS_HEIGHT : SH_SIZE_AXIS_WIDTH;
        int argument = bracket->kind == SH_SIZE_BRACKET_SPRITE ? (a ? bracket->height : bracket->width) : -1;
        const sh_size_read *reads = argument < 0 ? bracket->reads : bracket->forward;
        size_t count = argument < 0 ? bracket->count : bracket->forward_count;
        so_axis *slot = record.axis + a;
        slot->value = a ? state.height : state.width;
        /* A negative argument keeps the derived size; a forward read made the explicit argument a copy of
         * the getter's value. Any other explicit value is the caller's own. */
        for (size_t r = 0; r < count; r++) if (reads[r].axis == axis) {
            slot->derived = 1; slot->material = reads[r].material;
            if (!so_names_add(&slot->images, reads[r].image)) { so_sprite_free(&record); return 0; }
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    size_t i = so_sprite_find(sprite);
    if (i != SIZE_MAX) so_sprite_remove(i);
    int keep = state.material && ((record.axis[0].derived && record.axis[0].images.count) ||
        (record.axis[1].derived && record.axis[1].images.count));
    int stored = 1;
    if (keep) {
        if (g_sprite_count == g_sprite_capacity) {
            size_t capacity = g_sprite_capacity ? g_sprite_capacity * 2 : 64;
            so_sprite *grown = (so_sprite *)realloc(g_sprites, capacity * sizeof(*grown));
            if (grown) { g_sprites = grown; g_sprite_capacity = capacity; }
        }
        if (g_sprite_count < g_sprite_capacity && so_index_reserve(g_sprite_count + 1)) {
            g_sprites[g_sprite_count] = record;
            so_index_put(g_sprite_count++);
        } else stored = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!keep || !stored) so_sprite_free(&record);
    return stored;
}
/* What a bracket's reads become once its record is out of its context: its owner's copies, or reads that escape.
 * ok 0: the routine raised, did not end in order, or its context retired. */
static void so_frame_finish(so_frame *bracket, int ok)
{
    ok = ok && !bracket->failed;
    /* The script setter rebinds its sprite even when no getter runs (an empty or non-string material). */
    void *sprite = bracket->kind == SH_SIZE_BRACKET_SPRITE ? bracket->object :
        bracket->kind == SH_SIZE_BRACKET_SCRIPT ? so_script_sprite(bracket->object) : NULL;
    /* A layout's movie is the movie of the sprites it wrote (here, a layout taken out together with it). */
    if (sprite && bracket->next && !bracket->next->movie) {
        so_sprite_state state;
        if (so_sprite_read(sprite, &state)) bracket->next->movie = state.movie;
    }
    if (bracket->kind == SH_SIZE_BRACKET_SPRITE || bracket->kind == SH_SIZE_BRACKET_SCRIPT) {
        if (!ok || (bracket->count + bracket->forward_count && !so_sprite_written(bracket, sprite))) {
            so_escape_reads(bracket->reads, bracket->count);
            so_escape_reads(bracket->forward, bracket->forward_count);
            if (sprite) sh_size_owners_sprite_destroyed(sprite); /* Its provenance is no longer known. */
        } else if (!bracket->count && !bracket->forward_count && sprite) {
            /* No image read (a null or image-free material): nothing it holds depends on an image. */
            AcquireSRWLockExclusive(&g_lock);
            size_t i = so_sprite_find(sprite);
            if (i != SIZE_MAX) so_sprite_remove(i);
            ReleaseSRWLockExclusive(&g_lock);
        }
    } else if (bracket->kind == SH_SIZE_BRACKET_COLLECT) {
        /* A getter call that did not reach its own end: its reads stay with each image. */
        so_escape_reads(bracket->reads, bracket->count);
    } else if (bracket->count) {
        int kind = bracket->kind == SH_SIZE_BRACKET_FONT ? SO_FONT : bracket->kind == SH_SIZE_BRACKET_HOTBOX ?
            SO_HOTBOX : bracket->kind == SH_SIZE_BRACKET_WIDGET ? SO_WIDGET : SO_CREDITS;
        void *vtable = NULL, *movie = bracket->movie;
        if (bracket->object && kind != SO_FONT)
            __try { vtable = *(void **)bracket->object; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        if (kind == SO_CREDITS && !movie) {
            so_sprite_state state;
            if (so_sprite_read(bracket->object, &state)) movie = state.movie;
        }
        AcquireSRWLockExclusive(&g_lock);
        /* A routine that raised may have copied part of what it read: keep the earlier copies too. */
        int stored = (kind != SO_FONT || bracket->owner) && so_owner_store(&g_owners[kind],
            kind == SO_FONT ? NULL : bracket->object, kind == SO_FONT ? bracket->owner : NULL, vtable, movie,
            bracket->reads, bracket->count, !ok);
        ReleaseSRWLockExclusive(&g_lock);
        if (!stored) so_escape_reads(bracket->reads, bracket->count);
    }
}
/* Records opened above a bracket (all of a taken list but its last) end as routines that raised; the bracket's own
 * record, the last, is returned. */
static so_frame *so_frames_above(so_frame *taken)
{
    while (taken && taken->next) {
        so_frame *next = taken->next;
        so_frame_finish(taken, 0);
        so_frame_free(taken);
        taken = next;
    }
    return taken;
}
void sh_size_owners_end(sh_size_bracket *bracket, int completed)
{
    uint64_t id = bracket->serial ? sh_execution_context(NULL, 0) : 0;
    void *sprite = bracket->kind == SH_SIZE_BRACKET_SPRITE ? bracket->object :
        bracket->kind == SH_SIZE_BRACKET_SCRIPT ? so_script_sprite(bracket->object) : NULL;
    void *movie = NULL;
    so_frame *taken = NULL, *record;
    int in_order = 0;
    so_sprite_state state;
    if (sprite && so_sprite_read(sprite, &state)) movie = state.movie;
    AcquireSRWLockExclusive(&g_context_lock);
    so_context *context = so_context_of(bracket, id);
    if (context) {
        in_order = so_unlink(context, bracket->serial, movie, &taken);
        so_context_release(context);
        bracket->context = bracket->serial = 0;
    }
    ReleaseSRWLockExclusive(&g_context_lock);
    if ((record = so_frames_above(taken)) != NULL) {
        so_frame_finish(record, in_order && completed);
        so_frame_free(record);
    } else if (sprite) {
        /* Not open on this context (never recorded, already ended, or its context retired): the sprite's
         * provenance is no longer known. */
        sh_size_owners_sprite_destroyed(sprite);
    }
}
/* The context retired: its open brackets end as routines that raised, innermost first, and its pending forward
 * reads escape. Nothing is read from its stack. */
static void so_retire(uint64_t id)
{
    so_frame *taken = NULL, *record;
    sh_size_read *pending = NULL;
    size_t pending_count = 0;
    AcquireSRWLockExclusive(&g_context_lock);
    so_context *context = so_context_find(id, 0);
    if (context) {
        if (context->count) so_unlink(context, context->open[0]->serial, NULL, &taken);
        so_pending_take(context, &pending, &pending_count);
        so_context_release(context);
    }
    ReleaseSRWLockExclusive(&g_context_lock);
    if ((record = so_frames_above(taken)) != NULL) {
        so_frame_finish(record, 0);
        so_frame_free(record);
    }
    so_escape_reads(pending, pending_count);
    so_reads_free(pending, pending_count);
}
static sh_size_bracket_kind so_site_bracket(sh_size_site site)
{
    switch (site) {
    case SH_SIZE_SITE_SPRITE: return SH_SIZE_BRACKET_SPRITE;
    case SH_SIZE_SITE_WIDGET: return SH_SIZE_BRACKET_WIDGET;
    case SH_SIZE_SITE_CREDITS: return SH_SIZE_BRACKET_CREDITS;
    case SH_SIZE_SITE_HOTBOX: return SH_SIZE_BRACKET_HOTBOX;
    case SH_SIZE_SITE_FONT: return SH_SIZE_BRACKET_FONT;
    default: return 0;
    }
}
int sh_size_owners_read(const void *return_address, const void *frame, void *material, unsigned axis,
    char *const *images, size_t count)
{
    sh_size_site site = sh_size_owners_site(return_address);
    sh_size_read *dropped = NULL;
    size_t dropped_count = 0;
    int taken = 0;
    if (site == SH_SIZE_SITE_TRANSIENT || !count) return 1;
    if (InterlockedCompareExchange(&g_context_damaged, 0, 0)) return 0;
    sh_size_bracket_kind want = so_site_bracket(site);
    uint64_t id = sh_execution_context(&g_execution, site == SH_SIZE_SITE_FORWARD);
    if (!id) return 0;
    AcquireSRWLockExclusive(&g_context_lock);
    so_context *context = so_context_find(id, site == SH_SIZE_SITE_FORWARD);
    if (context && site == SH_SIZE_SITE_FORWARD) {
        if (context->pending_count && context->pending_frame != frame)
            so_pending_take(context, &dropped, &dropped_count);
        context->pending_frame = frame;
        taken = 1;
        for (size_t i = 0; taken && i < count; i++)
            if (!so_reads_add(&context->pending, &context->pending_count, &context->pending_capacity, images[i],
                    material, axis)) taken = 0;
        if (!taken) {
            so_reads_free(dropped, dropped_count);
            so_pending_take(context, &dropped, &dropped_count);
        }
        so_context_release(context);
    } else if (context && want) {
        /* The innermost bracket that is not a getter call. */
        so_frame *bracket = NULL;
        for (size_t i = context->count; !bracket && i-- > 0;)
            if (context->open[i]->kind != SH_SIZE_BRACKET_COLLECT) bracket = context->open[i];
        /* The script setter's getter calls are the sprite-writer sites as well. */
        if (bracket && (bracket->kind == want ||
                (want == SH_SIZE_BRACKET_SPRITE && bracket->kind == SH_SIZE_BRACKET_SCRIPT))) {
            taken = 1;
            for (size_t i = 0; taken && i < count; i++)
                if (!so_reads_add(&bracket->reads, &bracket->count, &bracket->capacity, images[i], material, axis)) {
                    bracket->failed = 1; taken = 0;
                }
        }
    }
    ReleaseSRWLockExclusive(&g_context_lock);
    so_escape_reads(dropped, dropped_count);
    so_reads_free(dropped, dropped_count);
    return taken;
}
void sh_size_owners_collect_begin(sh_size_bracket *call, void *material, unsigned axis)
{
    so_begin(call, SH_SIZE_BRACKET_COLLECT, material, NULL, NULL, -1, -1, axis);
}
int sh_size_owners_collecting(void) { return InterlockedCompareExchange(&g_collecting, 0, 0) != 0; }
void sh_size_owners_note(const char *image)
{
    if (!sh_size_owners_collecting()) return;
    uint64_t id = sh_execution_context(NULL, 0);
    if (!id) return;
    AcquireSRWLockExclusive(&g_context_lock);
    so_context *context = so_context_find(id, 0);
    /* A collection is always the innermost record while its getter runs. */
    so_frame *call = context && context->count ? context->open[context->count - 1] : NULL;
    if (call && call->kind == SH_SIZE_BRACKET_COLLECT) {
        int seen = 0;
        for (size_t i = 0; image && i < call->count; i++) if (so_name_equal(call->reads[i].image, image)) seen = 1;
        if (!image) call->failed = 1;
        else if (!seen && !so_reads_add(&call->reads, &call->count, &call->capacity, image, call->object, call->axis))
            call->failed = 1;
    }
    ReleaseSRWLockExclusive(&g_context_lock);
}
int sh_size_owners_collect_end(sh_size_bracket *call, int completed, const void *return_address, const void *frame,
    char ***names, size_t *count)
{
    uint64_t id = call->serial ? sh_execution_context(NULL, 0) : 0;
    so_frame *taken = NULL, *record;
    int in_order = 0, ok, done = 0;
    char **list = NULL;
    if (names) { *names = NULL; *count = 0; }
    AcquireSRWLockExclusive(&g_context_lock);
    so_context *context = so_context_of(call, id);
    if (context) {
        in_order = so_unlink(context, call->serial, NULL, &taken);
        so_context_release(context);
        call->context = call->serial = 0;
    }
    ReleaseSRWLockExclusive(&g_context_lock);
    if (!(record = so_frames_above(taken))) return 0;
    ok = in_order && completed && !record->failed;
    if (ok && record->count && !(list = (char **)calloc(record->count, sizeof(*list)))) ok = 0;
    for (size_t i = 0; ok && i < record->count; i++) list[i] = record->reads[i].image;
    if (ok && names) {
        /* The measurement owns the names now. */
        for (size_t i = 0; i < record->count; i++) record->reads[i].image = NULL;
        *names = list; *count = record->count; list = NULL;
        done = 1;
    } else if (ok) done = sh_size_owners_read(return_address, frame, record->object, record->axis, list, record->count);
    if (!done) so_escape_reads(record->reads, record->count);
    free(list);
    so_frame_free(record);
    return done;
}
/* The refresh's measured reads, added to its own open bracket; reads that bracket cannot hold escape. */
static void so_bracket_reads(const sh_size_bracket *bracket, char *const *images, size_t count, void *material,
    unsigned axis)
{
    uint64_t id = bracket->serial ? sh_execution_context(NULL, 0) : 0;
    so_frame *record = NULL;
    AcquireSRWLockExclusive(&g_context_lock);
    so_context *context = so_context_of(bracket, id);
    for (size_t i = 0; context && !record && i < context->count; i++)
        if (context->open[i]->serial == bracket->serial) record = context->open[i];
    for (size_t r = 0; record && r < count; r++)
        if (!so_reads_add(&record->reads, &record->count, &record->capacity, images[r], material, axis))
            record->failed = 1;
    ReleaseSRWLockExclusive(&g_context_lock);
    for (size_t r = 0; !record && r < count; r++)
        if (images[r]) sh_resource_graph_history_mark("image", images[r], SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
}
void sh_size_owners_explicit(void *script_object, unsigned axis, int completed)
{
    void *sprite = so_script_sprite(script_object);
    if (!sprite) return;
    so_sprite dropped = {0};
    AcquireSRWLockExclusive(&g_lock);
    size_t i = so_sprite_find(sprite);
    if (i != SIZE_MAX && !completed) {
        /* Taken out of the table; its images are marked after the lock is released. */
        dropped = so_sprite_take(i);
    } else if (i != SIZE_MAX) {
        so_axis *slot = g_sprites[i].axis + (axis == SH_SIZE_AXIS_HEIGHT);
        slot->derived = 0; slot->material = NULL; so_names_free(&slot->images);
        if (!g_sprites[i].axis[0].derived && !g_sprites[i].axis[1].derived) so_sprite_remove(i);
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (int a = 0; a < 2; a++) for (size_t r = 0; r < dropped.axis[a].images.count; r++)
        sh_resource_graph_history_mark("image", dropped.axis[a].images.items[r], SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    so_sprite_free(&dropped);
}
void sh_size_owners_script_read(void *script_object, unsigned axis)
{
    void *sprite = so_script_sprite(script_object);
    if (!sprite) return;
    so_names unstored = {0};
    AcquireSRWLockExclusive(&g_lock);
    size_t i = so_sprite_find(sprite);
    if (i != SIZE_MAX) {
        const so_axis *slot = g_sprites[i].axis + (axis == SH_SIZE_AXIS_HEIGHT);
        if (slot->derived && slot->images.count) {
            /* The value is now in the movie's script state, which no native routine re-derives. */
            sh_size_read *reads = (sh_size_read *)calloc(slot->images.count, sizeof(*reads));
            int stored = 0;
            if (reads) {
                for (size_t r = 0; r < slot->images.count; r++) reads[r].image = slot->images.items[r];
                stored = so_owner_store(&g_owners[SO_SCRIPT], g_sprites[i].movie, NULL, NULL, g_sprites[i].movie,
                    reads, slot->images.count, 1);
                free(reads);
            }
            if (!stored) for (size_t r = 0; r < slot->images.count; r++) so_names_add(&unstored, slot->images.items[r]);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (size_t r = 0; r < unstored.count; r++)
        sh_resource_graph_history_mark("image", unstored.items[r], SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    so_names_free(&unstored);
}
static void so_journal_retire_locked(const void *sprite, const void *movie);
static void so_owners_retire(int kind, const void *object)
{
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = g_owners[kind].count; i-- > 0;)
        if (g_owners[kind].items[i].object == object) so_owner_remove(&g_owners[kind], i);
    ReleaseSRWLockExclusive(&g_lock);
}
void sh_size_owners_sprite_destroyed(void *sprite)
{
    if (!sprite) return;
    AcquireSRWLockExclusive(&g_lock);
    size_t i = so_sprite_find(sprite);
    if (i != SIZE_MAX) so_sprite_remove(i);
    for (size_t j = g_owners[SO_CREDITS].count; j-- > 0;)
        if (g_owners[SO_CREDITS].items[j].object == sprite) so_owner_remove(&g_owners[SO_CREDITS], j);
    so_journal_retire_locked(sprite, NULL);
    ReleaseSRWLockExclusive(&g_lock);
}
void sh_size_owners_hotbox_destroyed(void *hotbox) { if (hotbox) so_owners_retire(SO_HOTBOX, hotbox); }
void sh_size_owners_widget_destroyed(void *widget) { if (widget) so_owners_retire(SO_WIDGET, widget); }
void sh_size_owners_movie_destroyed(void *movie)
{
    if (!movie) return;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = g_sprite_count; i-- > 0;) if (g_sprites[i].movie == movie) so_sprite_remove(i);
    for (int kind = SO_WIDGET; kind < SO_KINDS; kind++)
        for (size_t i = g_owners[kind].count; i-- > 0;)
            if (g_owners[kind].items[i].movie == movie) so_owner_remove(&g_owners[kind], i);
    so_journal_retire_locked(NULL, movie);
    ReleaseSRWLockExclusive(&g_lock);
}

int sh_size_owners_plan(const char *const *images, size_t count, char ***fonts, size_t *font_count,
    char *blocker, size_t capacity)
{
    char **found = NULL;
    size_t found_count = 0;
    int ok = 1;
    *fonts = NULL; *font_count = 0;
    if (blocker && capacity) blocker[0] = 0;
    if (!count) return 1;
    AcquireSRWLockShared(&g_lock);
    for (int kind = SO_CREDITS; ok && kind <= SO_SCRIPT; kind++)
        for (size_t i = 0; i < g_owners[kind].count; i++) {
            const so_owner *owner = g_owners[kind].items + i;
            for (size_t j = 0; j < count; j++) if (images[j] && so_names_has(&owner->images, images[j])) {
                if (blocker && capacity) snprintf(blocker, capacity, "image:%s has its size copied into a %s "
                    "(%p) that has no native refresh; the rebuild waits until that owner is destroyed", images[j],
                    g_kind_names[kind], owner->object);
                ok = 0; break;
            }
            if (!ok) break;
        }
    for (size_t i = 0; ok && i < g_owners[SO_FONT].count; i++) {
        const so_owner *owner = g_owners[SO_FONT].items + i;
        if (!so_names_meet(&owner->images, images, count)) continue;
        char **grown = (char **)realloc(found, (found_count + 1) * sizeof(*grown));
        if (!grown || !(grown[found_count] = _strdup(owner->name))) {
            if (grown) found = grown;
            if (blocker && capacity) snprintf(blocker, capacity, "copied-size owners could not be listed");
            ok = 0; break;
        }
        found = grown; found_count++;
    }
    ReleaseSRWLockShared(&g_lock);
    if (!ok) {
        for (size_t i = 0; i < found_count; i++) free(found[i]);
        free(found);
        return 0;
    }
    *fonts = found; *font_count = found_count;
    return 1;
}

static int so_write_axis(void *sprite, unsigned axis, short value)
{
    __try { *(short *)((unsigned char *)sprite + (axis == SH_SIZE_AXIS_HEIGHT ? SO_HEIGHT : SO_WIDTH)) = value; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int so_error(char *error, size_t capacity, const char *text, const void *object)
{
    if (error && capacity) snprintf(error, capacity, "copied-size refresh: %s (%p)", text, object);
    return 0;
}
typedef struct so_due { int kind; void *object, *vtable, *movie, *material; int derived[2]; short values[2];
    void *materials[2]; } so_due;
/* Sprite values a forward pass wrote, so that its restoring pass can write the earlier values back exactly.
 * Entries stay until the pass commits or a restoration succeeds; undo needs no allocation. Under g_lock. */
enum { SO_UNDO_PENDING, SO_UNDO_RESTORED, SO_UNDO_OBSOLETE, SO_UNDO_PRESERVED };
typedef struct so_undo { void *sprite, *vtable, *movie, *material; int axis, state; short before, after; } so_undo;
static so_undo *g_journal;
static size_t g_journal_count, g_journal_capacity;
void sh_size_owners_journal_clear(void)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_journal); g_journal = NULL; g_journal_count = g_journal_capacity = 0;
    ReleaseSRWLockExclusive(&g_lock);
}
static size_t so_journal_pending_locked(void)
{
    size_t pending = 0;
    for (size_t i = 0; i < g_journal_count; i++) pending += g_journal[i].state == SO_UNDO_PENDING;
    return pending;
}
size_t sh_size_owners_journal_count(void)
{
    AcquireSRWLockShared(&g_lock);
    size_t count = g_journal_count;
    ReleaseSRWLockShared(&g_lock);
    return count;
}
size_t sh_size_owners_journal_pending(void)
{
    AcquireSRWLockShared(&g_lock);
    size_t pending = so_journal_pending_locked();
    ReleaseSRWLockShared(&g_lock);
    return pending;
}
/* A destroyed sprite or movie takes its copies with it: nothing is left to write back. */
static void so_journal_retire_locked(const void *sprite, const void *movie)
{
    for (size_t i = 0; i < g_journal_count; i++)
        if (g_journal[i].state == SO_UNDO_PENDING &&
            ((sprite && g_journal[i].sprite == sprite) || (movie && g_journal[i].movie == movie)))
            g_journal[i].state = SO_UNDO_OBSOLETE;
}
static int so_journal_add(const so_undo *entry)
{
    int ok = 1;
    AcquireSRWLockExclusive(&g_lock);
    if (g_journal_count == g_journal_capacity) {
        size_t capacity = g_journal_capacity ? g_journal_capacity * 2 : 32;
        so_undo *grown = (so_undo *)realloc(g_journal, capacity * sizeof(*grown));
        if (grown) { g_journal = grown; g_journal_capacity = capacity; }
        else ok = 0;
    }
    if (ok) { g_journal[g_journal_count] = *entry; g_journal[g_journal_count++].state = SO_UNDO_PENDING; }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}
/* Restoring pass, under g_lock, newest first. Returns the entries still pending (their writes failed). */
static size_t so_journal_undo_locked(const void **failed, size_t *preserved)
{
    size_t pending = 0;
    for (size_t i = g_journal_count; i-- > 0;) {
        so_undo *entry = g_journal + i;
        so_sprite_state state;
        short current;
        if (entry->state != SO_UNDO_PENDING) continue;
        if (!so_sprite_read(entry->sprite, &state) || state.vtable != entry->vtable || state.movie != entry->movie) {
            entry->state = SO_UNDO_OBSOLETE; /* another object at the address: the copy went with its sprite */
            continue;
        }
        current = entry->axis ? state.height : state.width;
        if (state.material != entry->material || (current != entry->after && current != entry->before)) {
            entry->state = SO_UNDO_PRESERVED; (*preserved)++; /* a later writer's value is kept, not overwritten */
            continue;
        }
        if (current != entry->before && !so_write_axis(entry->sprite, entry->axis ? SH_SIZE_AXIS_HEIGHT : SH_SIZE_AXIS_WIDTH,
                entry->before)) {
            pending++; *failed = entry->sprite;
            continue;
        }
        entry->state = SO_UNDO_RESTORED;
        size_t record = so_sprite_find(entry->sprite);
        if (record != SIZE_MAX) g_sprites[record].axis[entry->axis].value = entry->before;
    }
    return pending;
}
static int so_journaled_locked(const void *sprite)
{
    for (size_t i = 0; i < g_journal_count; i++) if (g_journal[i].sprite == sprite) return 1;
    return 0;
}
/* hotBox geometry (R-321): its update (VK 0x141747fe0 / GL 0x14173ac80) rebuilds when the current scale differs
 * from the saved one. The saved scale's offset differs by renderer (VK +0xfc0, GL +0xf40) and is decoded from
 * that body at install. */
static size_t g_hotbox_saved;
void sh_size_owners_hotbox_layout(size_t saved_scale_offset) { g_hotbox_saved = saved_scale_offset; }
static int so_hotbox_invalidate(void *hotbox)
{
    if (!g_hotbox_saved) return 0;
    __try {
        /* NaN never compares equal (ucomiss, then jp): the next update rebuilds from the current sizes and
         * saves the current scale again, exactly as it does after any scale change. */
        static const unsigned nan_bits = 0x7fc00000u;
        for (int i = 0; i < 3; i++) memcpy((unsigned char *)hotbox + g_hotbox_saved + 4 * i, &nan_bits, 4);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* -1: the object no longer carries the recorded vtable (its destruction was not observed). */
static int so_widget_update(void *widget, void *vtable)
{
    __try {
        if (*(void **)widget != vtable) return -1;
        void (*update)(void *) = ((void (**)(void *))vtable)[SO_WIDGET_UPDATE_SLOT];
        update(widget);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* One sprite: measure every derived axis first, then write them all and replace the record. A failed
 * measurement writes nothing and keeps the record, so a later pass (or the rollback) still knows its
 * provenance. An axis whose value no longer matches the recorded one was written by a routine this module
 * does not see: it is kept as written and becomes explicit. */
static int so_refresh_sprite(const so_due *due, int journal, char *error, size_t capacity)
{
    so_sprite_state state;
    int values[2] = {0, 0}, measured[2] = {0, 0};
    char **read[2] = {NULL, NULL};
    size_t read_count[2] = {0, 0};
    int ok = 1;
    if (!so_sprite_read(due->object, &state) || state.vtable != due->vtable || state.movie != due->movie) {
        /* Not the recorded sprite any more: a destruction this module did not observe. Its copy is gone. */
        sh_size_owners_sprite_destroyed(due->object);
        return 1;
    }
    if (state.material != due->material)
        return so_error(error, capacity, "a sprite's material changed outside its recorded writers", due->object);
    for (int a = 0; ok && a < 2; a++) {
        short current = a ? state.height : state.width;
        if (!due->derived[a] || current != due->values[a]) continue;
        if (!g_measure || !g_measure(due->materials[a], a ? SH_SIZE_AXIS_HEIGHT : SH_SIZE_AXIS_WIDTH,
                values + a, read + a, read_count + a))
            ok = so_error(error, capacity, "a size getter could not be measured", due->object);
        else measured[a] = 1;
    }
    /* Journal before writing: a write the restoring pass could not undo must not happen. */
    for (int a = 0; ok && journal && a < 2; a++) {
        so_undo entry = {.sprite = due->object, .vtable = state.vtable, .movie = state.movie,
            .material = state.material, .axis = a, .state = SO_UNDO_PENDING,
            .before = a ? state.height : state.width, .after = (short)values[a]};
        if (measured[a] && entry.before != entry.after && !so_journal_add(&entry))
            ok = so_error(error, capacity, "the refresh journal could not grow", due->object);
    }
    if (ok) {
        sh_size_bracket bracket;
        sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, due->object, NULL, NULL,
            measured[0] ? -1 : state.width, measured[1] ? -1 : state.height);
        for (int a = 0; a < 2; a++) {
            unsigned axis = a ? SH_SIZE_AXIS_HEIGHT : SH_SIZE_AXIS_WIDTH;
            if (!measured[a]) continue;
            so_bracket_reads(&bracket, read[a], read_count[a], due->materials[a], axis);
            if (ok && !so_write_axis(due->object, axis, (short)values[a]))
                ok = so_error(error, capacity, "a sprite size could not be written", due->object);
        }
        sh_size_owners_end(&bracket, ok);
    }
    for (int a = 0; a < 2; a++) {
        for (size_t r = 0; r < read_count[a]; r++) free(read[a][r]);
        free(read[a]);
    }
    return ok;
}
int sh_size_owners_refresh(const char *const *images, size_t count, int restoring, char *error, size_t capacity)
{
    so_due *due = NULL;
    size_t due_count = 0, pending = 0, preserved = 0;
    const void *failed = NULL;
    int ok = 1;
    if (error && capacity) error[0] = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (!restoring) {
        /* Values a failed restoration could not write back keep their obligation: no new pass on top of them. */
        pending = so_journal_pending_locked();
        if (!pending) { free(g_journal); g_journal = NULL; g_journal_count = g_journal_capacity = 0; }
    } else pending = so_journal_undo_locked(&failed, &preserved);
    ReleaseSRWLockExclusive(&g_lock);
    if (pending && !restoring) {
        if (error && capacity) snprintf(error, capacity, "copied-size refresh: a failed restoration still owns %zu "
            "sprite value(s)", pending);
        return 0;
    }
    if (pending) {
        if (error && capacity) snprintf(error, capacity, "copied-size rollback could not write back %zu sprite value(s) "
            "(sprite %p); they stay pending for the next restoration", pending, failed);
        return 0;
    }
    if (!count) return 1;
    /* Snapshot what to refresh: refreshing re-enters the owner hooks, which update these records. */
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_sprite_count; i++)
        due_count += so_names_meet(&g_sprites[i].axis[0].images, images, count) ||
            so_names_meet(&g_sprites[i].axis[1].images, images, count);
    for (int kind = SO_HOTBOX; kind <= SO_WIDGET; kind++)
        for (size_t i = 0; i < g_owners[kind].count; i++)
            due_count += so_names_meet(&g_owners[kind].items[i].images, images, count);
    if (due_count && !(due = (so_due *)calloc(due_count, sizeof(*due)))) ok = 0;
    due_count = 0;
    for (size_t i = 0; ok && i < g_sprite_count; i++) {
        const so_sprite *record = g_sprites + i;
        if (!so_names_meet(&record->axis[0].images, images, count) && !so_names_meet(&record->axis[1].images, images, count))
            continue;
        so_due *item = due + due_count++;
        /* The restoring pass's undo already decided every sprite the failed pass wrote. */
        if (restoring && so_journaled_locked(record->sprite)) { item->kind = -2; continue; }
        item->kind = -1; item->object = record->sprite; item->vtable = record->vtable; item->movie = record->movie;
        item->material = record->material;
        for (int a = 0; a < 2; a++) {
            item->derived[a] = record->axis[a].derived;
            item->values[a] = record->axis[a].value;
            item->materials[a] = record->axis[a].material;
        }
    }
    for (int kind = SO_HOTBOX; ok && kind <= SO_WIDGET; kind++)
        for (size_t i = 0; i < g_owners[kind].count; i++) {
            const so_owner *owner = g_owners[kind].items + i;
            if (!so_names_meet(&owner->images, images, count)) continue;
            so_due *item = due + due_count++;
            item->kind = kind; item->object = owner->object; item->vtable = owner->vtable; item->movie = owner->movie;
        }
    ReleaseSRWLockShared(&g_lock);
    if (!ok) return so_error(error, capacity, "could not allocate", NULL);
    for (size_t i = 0; ok && i < due_count; i++) {
        so_due *item = due + i;
        if (item->kind == -2) continue;
        if (item->kind < 0) ok = so_refresh_sprite(item, !restoring, error, capacity);
        else if (item->kind == SO_HOTBOX) {
            void *vtable = NULL;
            __try { vtable = *(void **)item->object; } __except (EXCEPTION_EXECUTE_HANDLER) { vtable = NULL; }
            if (vtable != item->vtable) { sh_size_owners_hotbox_destroyed(item->object); continue; }
            if (!so_hotbox_invalidate(item->object))
                ok = so_error(error, capacity, "hotBox geometry could not be returned for its rebuild", item->object);
        } else {
            int updated = so_widget_update(item->object, item->vtable);
            if (updated < 0) { sh_size_owners_widget_destroyed(item->object); continue; }
            if (!updated) ok = so_error(error, capacity, "a codex widget update raised", item->object);
        }
    }
    free(due);
    (void)preserved;
    return ok;
}
size_t sh_size_owners_count(void)
{
    AcquireSRWLockShared(&g_lock);
    size_t count = g_sprite_count;
    for (int kind = 0; kind < SO_KINDS; kind++) count += g_owners[kind].count;
    ReleaseSRWLockShared(&g_lock);
    return count;
}
#ifdef SH_SIZE_OWNERS_TESTING
void sh_size_owners_test_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    while (g_sprite_count) so_sprite_remove(g_sprite_count - 1);
    free(g_sprites); g_sprites = NULL; g_sprite_capacity = 0;
    free(g_index); g_index = NULL; g_index_capacity = 0;
    free(g_journal); g_journal = NULL; g_journal_count = g_journal_capacity = 0;
    for (int kind = 0; kind < SO_KINDS; kind++) {
        while (g_owners[kind].count) so_owner_remove(&g_owners[kind], g_owners[kind].count - 1);
        free(g_owners[kind].items); memset(&g_owners[kind], 0, sizeof(g_owners[kind]));
    }
    g_return_count = 0; g_own_lo = g_own_hi = 0; g_hotbox_saved = 0;
    ReleaseSRWLockExclusive(&g_lock);
    AcquireSRWLockExclusive(&g_context_lock);
    for (size_t i = 0; i < g_context_count; i++) {
        for (size_t j = 0; j < g_contexts[i].count; j++) so_frame_free(g_contexts[i].open[j]);
        free(g_contexts[i].open);
        so_reads_free(g_contexts[i].pending, g_contexts[i].pending_count);
    }
    free(g_contexts); g_contexts = NULL; g_context_count = g_context_capacity = 0;
    ReleaseSRWLockExclusive(&g_context_lock);
    InterlockedExchange(&g_enabled, 0); InterlockedExchange(&g_collecting, 0); InterlockedExchange(&g_context_damaged, 0);
    g_measure = NULL;
}
size_t sh_size_owners_test_contexts(void)
{
    AcquireSRWLockShared(&g_context_lock);
    size_t count = g_context_count;
    ReleaseSRWLockShared(&g_context_lock);
    return count;
}
size_t sh_size_owners_test_records(void) { return (size_t)InterlockedCompareExchange(&g_frames, 0, 0); }
void sh_size_owners_test_sites(const void *const *returns, const sh_size_site *classes, size_t count)
{
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < count && i < SO_SITE_LIMIT; i++) { g_returns[i] = (uintptr_t)returns[i]; g_classes[i] = classes[i]; }
    g_return_count = count < SO_SITE_LIMIT ? count : SO_SITE_LIMIT;
    ReleaseSRWLockExclusive(&g_lock);
}
#endif
