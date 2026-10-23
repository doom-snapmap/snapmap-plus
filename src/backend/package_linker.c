#include "package_linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct pl_file {
    const sh_compiled_resource *resource;
    const char *family, *name;
    sh_package_file_identity identity;
    size_t edge_begin, edge_count;
} pl_file;
typedef struct pl_unit {
    pl_file *files;
    pl_file *witnesses;
    const sh_package_link_witness **witness_order;
    sh_package_link_edge *edges;
    size_t edge_count, edge_capacity;
    sh_package_link_edge *implicit;
    size_t implicit_count, implicit_begin;
    char **paths;
    char **names;
    /* The representative's derived publications (sh_package_link_adapter.derive):
     * target units, their implicit slots (borrowed), paths and native names. */
    size_t *derived_targets;
    const char **derived_slots;
    char **derived_paths, **derived_names;
    unsigned char *derived_alias; /* derive returned 2 */
    size_t derived_count;
    /* Restored from, or captured into, the reuse cache: edges and implicit hold
     * that cache's slot strings. The arrays belong to the unit unless shared. */
    int borrowed;
    /* Arrays of a restored unit that are the cache entry's own (PL_SHARED_*):
     * read-only for this pass and never freed here. */
    unsigned shared;
    /* While captured (R-348): its first `interned` edges hold the reuse cache's slot texts (pl_intern_edges), never
     * the unit's own. */
    size_t interned;
} pl_unit;
enum { PL_SHARED_FILES = 1, PL_SHARED_BINDINGS = 2 };
static unsigned long long pl_now_us(void);
/* Meters only (diagnostics, never a result): one FNV-1a step over bytes. */
static uint64_t pl_fnv(uint64_t hash, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    for (size_t i = 0; i < length; i++) { hash ^= p[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
/* pl_occurrence's counts over one file's edges [occ_begin, occ_end) of
 * occ_unit: entries of an older generation are empty. */
typedef struct pl_occ_entry { const char *key; size_t length, count, generation; uint64_t hash; } pl_occ_entry;
typedef struct pl_work {
    /* Witness resources whose bytes this pass already opened and verified. A
     * private page family names the same members for every one of its
     * materials; a source-backed member is hashed on every open. */
    const sh_compiled_resource **verified_witnesses;
    size_t verified_witness_count, verified_witness_capacity;
    const sh_package_link_unit *units;
    const sh_package_link_adapter *adapter;
    sh_package_link_output output;
    pl_unit *work;
    sh_package_link_node *nodes;
    size_t *member_offsets, *members;
    unsigned char *originals;
    unsigned char *carried; /* per unit: its class is carried and changed (sh_package_link_unit.carried) */
    unsigned char *effective; /* certified originals deciding conditional bindings */
    size_t count, unit, file, next_edge;
    size_t restored; /* Diagnostics: units this pass restored from the reuse cache. */
    /* Meters (diagnostics only): units captured afresh, how many sticky-invalid
     * recaptures reproduced their previous capture, and pl_conditional's work. */
    size_t fresh, again_first, again_same, again_changed;
    size_t cond_units, cond_edges, cond_removed, cond_nonlocal, cond_certified, cond_walk_edges, cond_compares;
    uint64_t cond_digest;
    unsigned long long fresh_us, cond_us, cond_view_us, cond_walk_us, cond_filter_us, alloc_us;
    int replay, failed;
    sh_package_link_history *history;
    char *error;
    size_t capacity;
    const struct pl_unit *occ_unit;
    size_t occ_begin, occ_end, occ_used, occ_capacity, occ_generation;
    pl_occ_entry *occ;
    /* pl_slot_index scratch: one unit's slot names sorted, reused unit to unit. */
    const char **slot_index;
    size_t slot_capacity;
    /* pl_reference's renamed bound references in the current projection bind,
     * and how many of them name another family than image. */
    size_t rebound_references, rebound_non_image;
    /* Capture templates (R-348, pl_template_find): the one the current stable file took or made, and the pass's
     * meters (files that took one, templates made, edges taken, single-file content digests taken). */
    struct pl_template *file_template;
    size_t template_files, template_built, template_edges, template_digests, template_restored;
    /* Microseconds binding stable files' sites: from a template, from an offered record's template, and captured
     * without one. */
    unsigned long long template_hit_us, template_restore_us, template_build_us;
} pl_work;
typedef struct pl_assignment {
    const char *family, *name;
    const unsigned char *identity;
    size_t group;
    int original;
} pl_assignment;
typedef struct pl_native_identity {
    const char *family, *name, *original_name;
    uint32_t id;
    int original, primary;
    size_t unit;
} pl_native_identity;

typedef struct pl_registry {
    size_t *names, *ids, capacity;
    const char **paths;
    size_t *path_units, *path_files;
    size_t path_capacity;
    pl_native_identity *identities;
    size_t identity_count;
} pl_registry;
typedef struct pl_input {
    const unsigned char *body;
    FILE *stream;
    uint64_t length, position;
    /* A detached original (R-342): its bytes are read again through the shared body at the first read only, so
     * an availability check reads nothing. owned holds them; pl_input_close frees it. */
    sh_package_body *detached;
    unsigned char *owned;
} pl_input;
static void pl_input_close(pl_input *input)
{
    if (input->stream) fclose(input->stream);
    free(input->owned);
    input->stream = NULL; input->owned = NULL;
}

#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
static int g_pl_after_projection_refusal;
void sh_package_link_test_after_projection_refusal(int enabled) { g_pl_after_projection_refusal = enabled != 0; }
#endif
static void *pl_alloc(size_t count, size_t size)
{ return size && count <= SIZE_MAX / size ? calloc(count ? count : 1, size) : NULL; }
static int pl_fail(pl_work *w, const char *reason)
{
    w->failed = 1;
    if (!*w->error) snprintf(w->error, w->capacity, "%s", reason);
    return 0;
}
static int pl_file_order(const void *a, const void *b)
{ return strcmp(((const pl_file *)a)->resource->engine_path, ((const pl_file *)b)->resource->engine_path); }
static int pl_witness_order(const void *a, const void *b)
{
    const sh_package_link_witness *x = *(const sh_package_link_witness *const *)a;
    const sh_package_link_witness *y = *(const sh_package_link_witness *const *)b;
    int order = strcmp(x->role, y->role);
    return order ? order : strcmp(x->resource->engine_path, y->resource->engine_path);
}
static int pl_input_open_resource(pl_work *w, size_t unit, const pl_file *captured, pl_input *out);
/* A witness's bytes are available with its captured identity length: open each
 * resource once per pass (the resources are immutable while it runs). */
static int pl_witness_available(pl_work *w, size_t unit, const pl_file *file)
{
    for (size_t i = 0; i < w->verified_witness_count; i++)
        if (w->verified_witnesses[i] == file->resource) return 1;
    pl_input stream = {0};
    int available = pl_input_open_resource(w, unit, file, &stream);
    pl_input_close(&stream);
    if (!available) return 0;
    if (w->verified_witness_count == w->verified_witness_capacity) {
        size_t next = w->verified_witness_capacity ? w->verified_witness_capacity * 2 : 32;
        const sh_compiled_resource **grown = next <= SIZE_MAX / sizeof(*grown) ?
            realloc((void *)w->verified_witnesses, next * sizeof(*grown)) : NULL;
        if (!grown) return 1; /* Verified now; only the memo is lost. */
        w->verified_witnesses = grown; w->verified_witness_capacity = next;
    }
    w->verified_witnesses[w->verified_witness_count++] = file->resource;
    return 1;
}
/* A stable-name view's filtered nodes and their saved edges (pl_stable_view). */
typedef struct pl_view {
    struct pl_view_saved { size_t node; const sh_package_link_edge *edges; size_t edge_count; const uint32_t *order; } *saved;
    size_t saved_count;
    sh_package_link_edge *kept;
} pl_view;
static sh_package_link_node *pl_stable_view(pl_work *w, pl_view *held);
static void pl_stable_view_end(pl_work *w, pl_view *held);
static int pl_assignment_order(const void *a, const void *b)
{
    const pl_assignment *x = a, *y = b;
    int order = y->original - x->original;
    if (!order) order = strcmp(x->family, y->family);
    if (!order) order = memcmp(x->identity, y->identity, 32);
    return order ? order : strcmp(x->name, y->name);
}

void sh_package_link_output_free(sh_package_link_output *out)
{
    if (!out) return;
    for (size_t i = 0; out->row_provenance && i < out->row_provenance_count; i++) sh_package_owners_free(out->row_provenance + i);
    free(out->row_provenance); out->row_provenance = NULL; out->row_provenance_count = 0;
    free((void *)out->row_provenance_paths); out->row_provenance_paths = NULL;
    sh_package_compilation_free(out->compiled);
    if (out->names) for (size_t i = 0; i < out->classes.count; i++) free(out->names[i]);
    for (size_t i = 0; i < out->implicit_count; i++) free(out->implicit[i].slot);
    free(out->implicit);
    for (size_t i = 0; i < out->derived_count; i++) {
        free(out->derived[i].slot); free(out->derived[i].path); free(out->derived[i].name);
    }
    free(out->derived); free(out->carried); free(out->originals);
    if (out->provenance) for (size_t i = 0; i < out->classes.count; i++) sh_package_owners_free(out->provenance + i);
    free(out->provenance);
    free(out->names); sh_package_link_classes_free(&out->classes); memset(out, 0, sizeof(*out));
}

static void pl_derived_clear(pl_unit *u)
{
    for (size_t j = 0; j < u->derived_count; j++) { free(u->derived_paths[j]); free(u->derived_names[j]); }
    free(u->derived_targets); free((void *)u->derived_slots); free(u->derived_paths); free(u->derived_names);
    free(u->derived_alias);
    u->derived_targets = NULL; u->derived_slots = NULL; u->derived_paths = u->derived_names = NULL; u->derived_alias = NULL;
    u->derived_count = 0;
}
static void pl_clear(pl_work *w)
{
    free((void *)w->verified_witnesses); w->verified_witnesses = NULL;
    w->verified_witness_count = w->verified_witness_capacity = 0;
    if (w->work) for (size_t i = 0; i < w->count; i++) {
        pl_unit *u = &w->work[i];
        if (!u->borrowed) {
            for (size_t j = u->interned; j < u->edge_count; j++) free((char *)u->edges[j].slot);
            for (size_t j = 0; j < u->implicit_count; j++) free((char *)u->implicit[j].slot);
        }
        if (u->paths) for (size_t j = 0; j < w->units[i].file_count; j++) free(u->paths[j]);
        if (u->names) for (size_t j = 0; j < w->units[i].file_count; j++) free(u->names[j]);
        free(u->names); free(u->paths); pl_derived_clear(u);
        if (!(u->shared & PL_SHARED_BINDINGS)) { free(u->edges); free(u->implicit); }
        if (!(u->shared & PL_SHARED_FILES)) { free(u->files); free(u->witnesses); free(u->witness_order); }
    }
    free(w->work); free(w->nodes); free(w->member_offsets); free(w->members); free(w->originals); free(w->effective);
    free(w->carried);
    free(w->occ); free(w->slot_index);
    sh_package_link_history_close(w->history);
    sh_package_link_output_free(&w->output);
}

/* One unit's capture, kept between identity passes while the unit is valid. ordered: the edges block holds, after
 * edge_count edges, their positions in slot order (pl_reuse_order), which the pass's node hands to the partition. */
typedef struct pl_reuse_unit {
    unsigned char captured, invalid, ordered;
    pl_file *files, *witnesses;
    const sh_package_link_witness **witness_order;
    sh_package_link_edge *edges, *implicit;
    size_t file_count, witness_count, edge_count, implicit_count, implicit_begin;
    uint64_t length;
    unsigned char content[32];
} pl_reuse_unit;
struct sh_package_link_reuse {
    pl_reuse_unit *units;
    size_t count;
    sh_package_link_order order; /* the last identify pass's shape order */
    uint64_t *proven; /* open addressing: (left + 1) << 32 | (right + 1), 0 empty */
    size_t proven_count, proven_capacity;
    /* Diagnostics only: the identity passes that used this cache, and the
     * final link's member, history, assignment and projection steps. */
    struct {
        size_t units, restored, classes, ordered;
        unsigned long long collect_us, partition_us, view_us, shapes_us, refine_us, identity_us, clear_us;
        size_t fresh, again_first, again_same, again_changed;
        size_t cond_units, cond_edges, cond_removed, cond_nonlocal, cond_certified, cond_walk_edges, cond_compares;
        uint64_t cond_digest;
        unsigned long long fresh_us, cond_us, cond_view_us, cond_walk_us, cond_filter_us, alloc_us;
        size_t template_files, template_built, template_edges, template_digests, template_restored;
        unsigned long long template_hit_us, template_restore_us, template_build_us;
    } passes[16];
    size_t pass_count;
    /* Restores that borrowed an entry's arrays, and the binding arrays
     * pl_conditional then copied before editing them. */
    unsigned long long borrowed_restores, unshared;
    /* Tests only (sh_package_link_reuse_test_mode): restore by copying the
     * entry's arrays, and fail the n-th next copy-on-write (0 never). */
    int copy_restore;
    size_t fail_unshare;
    /* Meters only (pl_meter_again): each sticky-invalid unit's last fresh capture
     * folded to one word (0 not seen). Tests may fail its allocation. */
    int fail_meter;
    /* Tests only: pl_slot_index declines, so slot membership is searched linearly. */
    int fail_index;
    uint64_t *fp;
    size_t fp_count;
    /* Exact verification of equal fingerprints: pairs compared, members held
     * as one captured object by both units, bytes compared, time. */
    unsigned long long verify_pairs, verify_shared, verify_bytes, verify_us;
    unsigned long long link_us[4];
    int linked;
    /* One copy of each distinct slot text of the kept edges (open addressing,
     * owned here until close): units reading them share the pointers. */
    char **texts;
    size_t text_count, text_capacity;
    /* Meter (R-348): the bytes of those texts, and of the slot keys the templates made hold (the string share a
     * template kept across compiles would need). */
    unsigned long long text_bytes, template_key_bytes;
    /* Capture templates by file (R-348, open addressing; pl_template_find), owned here until close. */
    struct pl_template *templates;
    size_t template_count, template_capacity;
    /* Templates an earlier compile recorded (R-348, sh_package_link_reuse_offer_templates): the record's bytes, its
     * slot texts (in those bytes) and its templates by path hash (open addressing; at is a template's offset + 1, 0
     * empty), from the offer until the first pass capturing with templates has captured (pl_offer_release). State 0
     * none offered, 1 offered, 2 released, -1 refused; counts of templates offered, taken and refused, the record's
     * bytes and the time indexing it. */
    unsigned char *offer;
    size_t offer_length;
    const char **offer_slots;
    size_t offer_slot_count;
    struct pl_offer_entry { uint64_t hash; size_t at; } *offer_index;
    size_t offer_capacity;
    int offer_state;
    size_t offer_templates, offer_taken, offer_refused;
    unsigned long long offer_bytes, offer_us;
};
/* What a first capture made of one immutable file's sites (R-348): the sites' slots as the adapter passed them (the
 * source capture's interned texts, compared by address), the slot keys they became (texts above) and their positions
 * in slot order, and the content digest of a unit made of this file alone (digested). A unit binding the same file,
 * with the same identity and the same sites' slots, takes the keys, the order and the digest instead of capturing
 * them again; its targets are its own sites'. */
typedef struct pl_template {
    const sh_compiled_resource *file;
    sh_package_file_identity identity;
    size_t count;
    const char **slots, **keys;
    uint32_t *order;
    unsigned char digest[32];
    int digested;
} pl_template;
static uint64_t pl_occ_hash(const char *key, size_t length);
/* The texts' place for text (length bytes): its copy, or the empty place where it belongs. SIZE_MAX when the table
 * cannot grow. */
static size_t pl_reuse_text_place(sh_package_link_reuse *reuse, const char *text, size_t length)
{
    if (reuse->text_count + 1 > reuse->text_capacity / 2) {
        size_t capacity = reuse->text_capacity ? reuse->text_capacity * 2 : 4096;
        char **grown = capacity <= SIZE_MAX / 2 / sizeof(*grown) ? calloc(capacity, sizeof(*grown)) : NULL;
        if (!grown) return SIZE_MAX;
        for (size_t i = 0; i < reuse->text_capacity; i++) if (reuse->texts[i]) {
            size_t at = (size_t)pl_occ_hash(reuse->texts[i], strlen(reuse->texts[i])) & (capacity - 1);
            while (grown[at]) at = (at + 1) & (capacity - 1);
            grown[at] = reuse->texts[i];
        }
        free(reuse->texts); reuse->texts = grown; reuse->text_capacity = capacity;
    }
    size_t at = (size_t)pl_occ_hash(text, length) & (reuse->text_capacity - 1);
    while (reuse->texts[at] && strcmp(reuse->texts[at], text)) at = (at + 1) & (reuse->text_capacity - 1);
    return at;
}
static const char *pl_reuse_text(sh_package_link_reuse *reuse, const char *text)
{
    size_t length = strlen(text), at = pl_reuse_text_place(reuse, text, length);
    if (at == SIZE_MAX) return NULL;
    if (!reuse->texts[at]) {
        if (!(reuse->texts[at] = _strdup(text))) return NULL;
        reuse->text_count++; reuse->text_bytes += length + 1;
    }
    return reuse->texts[at];
}
/* As pl_reuse_text for a text the caller allocated (length bytes): it becomes the copy when there is none, else it is
 * freed for the copy (R-348: an offered template's keys). NULL, text freed, when the table cannot grow. */
static const char *pl_reuse_text_take(sh_package_link_reuse *reuse, char *text, size_t length)
{
    size_t at = pl_reuse_text_place(reuse, text, length);
    if (at == SIZE_MAX) { free(text); return NULL; }
    if (reuse->texts[at]) { free(text); return reuse->texts[at]; }
    reuse->texts[at] = text; reuse->text_count++; reuse->text_bytes += length + 1;
    return text;
}
static void pl_reuse_unit_clear(pl_reuse_unit *u)
{
    unsigned char invalid = u->invalid;
    free(u->edges); free(u->implicit);
    free(u->files); free(u->witnesses); free(u->witness_order);
    memset(u, 0, sizeof(*u)); u->invalid = invalid;
}
sh_package_link_reuse *sh_package_link_reuse_open(size_t count)
{
    sh_package_link_reuse *reuse = calloc(1, sizeof(*reuse));
    if (!reuse || count >= UINT32_MAX || !(reuse->units = pl_alloc(count, sizeof(*reuse->units)))) {
        free(reuse); return NULL;
    }
    reuse->count = count; return reuse;
}
int sh_package_link_reuse_reserve(sh_package_link_reuse *reuse, size_t count)
{
    if (!reuse || count >= UINT32_MAX || count > SIZE_MAX / sizeof(*reuse->units)) return 0;
    if (count <= reuse->count) return 1;
    pl_reuse_unit *grown = calloc(count, sizeof(*grown));
    if (!grown) return 0;
    memcpy(grown, reuse->units, reuse->count * sizeof(*grown));
    free(reuse->units); reuse->units = grown; reuse->count = count;
    return 1;
}
void sh_package_link_reuse_invalidate(sh_package_link_reuse *reuse, size_t unit)
{
    if (!reuse || unit >= reuse->count) return;
    pl_reuse_unit_clear(reuse->units + unit); reuse->units[unit].invalid = 1;
}
void sh_package_link_reuse_close(sh_package_link_reuse *reuse)
{
    if (!reuse) return;
    for (size_t i = 0; i < reuse->count; i++) pl_reuse_unit_clear(reuse->units + i);
    for (size_t i = 0; i < reuse->template_capacity; i++) {
        free(reuse->templates[i].slots); free(reuse->templates[i].keys); free(reuse->templates[i].order);
    }
    free(reuse->templates);
    free(reuse->offer); free((void *)reuse->offer_slots); free(reuse->offer_index);
    for (size_t i = 0; i < reuse->text_capacity; i++) free(reuse->texts[i]);
    sh_package_link_order_free(&reuse->order);
    free(reuse->texts); free(reuse->units); free(reuse->proven); free(reuse->fp); free(reuse);
}
/* The slot order an entry keeps after its edges, else NULL. */
static const uint32_t *pl_reuse_order(const pl_reuse_unit *c)
{ return c->ordered && c->edge_count ? (const uint32_t *)(c->edges + c->edge_count) : NULL; }
/* qsort_s: two binding positions of one unit (context: its edges) by slot. */
static int pl_position_order(void *context, const void *left, const void *right)
{
    const sh_package_link_edge *edges = context;
    return strcmp(edges[*(const uint32_t *)left].slot, edges[*(const uint32_t *)right].slot);
}
/* ---- Capture templates (R-348) ----
 * Most units of a library are scope copies binding the same captured original, with the same sites; their first
 * captures differ only in their targets. A capture with a reuse cache in its own mode keeps a template of each stable
 * file it captures (pl_template_add) and gives every later unit binding that file with the same identity and the same
 * sites' slots the template's keys, order and single-file digest (pl_template_find), its targets its own. */
#if defined(SH_PACKAGE_LINKER_TESTING) || defined(SH_PACKAGE_LIBRARY_TESTING) || \
    defined(SH_PACKAGE_SOURCE_LINK_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
static int g_pl_templates = 1;
void sh_package_link_test_templates(int enabled) { g_pl_templates = enabled != 0; }
#define PL_TEMPLATES() g_pl_templates
static int g_pl_offer_mutation;
void sh_package_link_test_offer_mutation(int mutation) { g_pl_offer_mutation = mutation; }
#define PL_OFFER_MUTATED(m) (g_pl_offer_mutation == (m))
#else
#define PL_TEMPLATES() 1
#define PL_OFFER_MUTATED(m) 0
#endif
static int pl_templating(const pl_work *w)
{ return PL_TEMPLATES() && w->adapter && w->adapter->reuse && !w->adapter->reuse->copy_restore && !w->replay; }
static size_t pl_template_at(const sh_package_link_reuse *reuse, const sh_compiled_resource *file)
{
    uint64_t h = ((uint64_t)(uintptr_t)file >> 4) * UINT64_C(11400714819323198485);
    size_t mask = reuse->template_capacity - 1, at = (size_t)(h ^ (h >> 29)) & mask;
    while (reuse->templates[at].file && reuse->templates[at].file != file) at = (at + 1) & mask;
    return at;
}
static pl_template *pl_template_find(pl_work *w, const sh_compiled_resource *file, const sh_package_file_identity *identity,
    const sh_package_link_edge *sites, size_t count)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    if (!reuse->template_capacity) return NULL;
    pl_template *t = reuse->templates + pl_template_at(reuse, file);
    if (!t->file || t->count != count || t->identity.length != identity->length ||
        memcmp(t->identity.digest, identity->digest, 32)) return NULL;
    for (size_t i = 0; i < count; i++) if (t->slots[i] != sites[i].slot) return NULL;
    return t;
}
/* The empty place for file's template, the table grown first; NULL when it cannot grow or the file has one. */
static pl_template *pl_template_place(sh_package_link_reuse *reuse, const sh_compiled_resource *file)
{
    if (reuse->template_count + 1 > reuse->template_capacity / 2) {
        size_t capacity = reuse->template_capacity ? reuse->template_capacity * 2 : 1024, previous = reuse->template_capacity;
        pl_template *old = reuse->templates, *grown = capacity <= SIZE_MAX / 2 / sizeof(*grown) ? calloc(capacity, sizeof(*grown)) : NULL;
        if (!grown) return NULL;
        reuse->templates = grown; reuse->template_capacity = capacity;
        for (size_t i = 0; i < previous; i++) if (old[i].file) reuse->templates[pl_template_at(reuse, old[i].file)] = old[i];
        free(old);
    }
    pl_template *t = reuse->templates + pl_template_at(reuse, file);
    return t->file ? NULL : t;
}
/* Keep what this capture made of a file: its sites' slots and its edges' keys (already this cache's texts). The
 * file's first capture keeps the place. Optional: NULL when refused. */
static pl_template *pl_template_add(pl_work *w, const sh_compiled_resource *file, const sh_package_file_identity *identity,
    const sh_package_link_edge *sites, const sh_package_link_edge *edges, size_t count)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    if (count > UINT32_MAX) return NULL;
    pl_template *t = pl_template_place(reuse, file);
    if (!t) return NULL;
    pl_template next = {file, *identity, count};
    next.slots = pl_alloc(count, sizeof(*next.slots)); next.keys = pl_alloc(count, sizeof(*next.keys));
    next.order = pl_alloc(count, sizeof(*next.order));
    if (!next.slots || !next.keys || !next.order) { free(next.slots); free(next.keys); free(next.order); return NULL; }
    for (size_t i = 0; i < count; i++) {
        next.slots[i] = sites[i].slot; next.keys[i] = edges[i].slot; next.order[i] = (uint32_t)i;
        reuse->template_key_bytes += strlen(edges[i].slot) + 1;
    }
    if (count > 1) qsort_s(next.order, count, sizeof(*next.order), pl_position_order, (void *)edges);
    *t = next; reuse->template_count++;
    return t;
}
/* ---- Capture templates kept across compiles (R-348) ----
 * A stable file's keys are "<path length>:<path>", the site's slot and, for a repeated slot, "#n" with n counted over
 * the file's earlier slots (pl_slot, pl_occurrence); its template's order sorts those keys, which share the prefix, by
 * what follows it. Both are a function of the path and the slots alone, so what one compile made of a file, recorded
 * per site as its slot, its occurrence and its order position, holds for any later compile's file with the same path
 * whose sites' slots are the recorded ones, one by one: the keys are built again from that file's own path. Record
 * (sh_package_link_reuse_templates_encode): "SMPIT1", u64 slot count, each slot (varint length, bytes, NUL), u64
 * template count, each template (u64 path hash, varint site count, then per site varint slot index, occurrence (0 none,
 * else 2 up to its position + 1) and order position); integers little-endian. */
typedef struct pl_bytes { unsigned char *bytes; size_t length, capacity; int failed; } pl_bytes;
static void pl_bytes_put(pl_bytes *b, const void *data, size_t length)
{
    if (b->failed || !length) return;
    if (length > b->capacity - b->length) {
        size_t next = b->capacity ? b->capacity : 65536;
        while (next - b->length < length) {
            if (next > SIZE_MAX / 2) { b->failed = 1; return; }
            next *= 2;
        }
        unsigned char *grown = realloc(b->bytes, next);
        if (!grown) { b->failed = 1; return; }
        b->bytes = grown; b->capacity = next;
    }
    memcpy(b->bytes + b->length, data, length); b->length += length;
}
static void pl_bytes_u64(pl_bytes *b, uint64_t value)
{
    unsigned char v[8];
    for (int i = 0; i < 8; i++) v[i] = (unsigned char)(value >> (8 * i));
    pl_bytes_put(b, v, sizeof(v));
}
static void pl_bytes_varint(pl_bytes *b, uint64_t value)
{
    unsigned char v[10];
    size_t n = 0;
    do { v[n] = (unsigned char)(value & 0x7f); value >>= 7; if (value) v[n] |= 0x80; n++; } while (value);
    pl_bytes_put(b, v, n);
}
typedef struct pl_cursor { const unsigned char *at, *end; } pl_cursor;
static int pl_cursor_u64(pl_cursor *c, uint64_t *out)
{
    if (c->end - c->at < 8) return 0;
    *out = 0;
    for (int i = 7; i >= 0; i--) *out = *out << 8 | c->at[i];
    c->at += 8; return 1;
}
static int pl_cursor_varint(pl_cursor *c, uint64_t *out)
{
    uint64_t value = 0;
    for (unsigned shift = 0; c->at < c->end && shift < 64; shift += 7) {
        unsigned char byte = *c->at++;
        if (shift == 63 && byte > 1) return 0;
        value |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *out = value; return 1; }
    }
    return 0;
}
/* A slot text's record index while encoding, by the text's address (a capture's slots are its interned texts; an
 * equal text at another address is only recorded twice). */
typedef struct pl_slot_id { const char *slot; uint32_t index; } pl_slot_id;
static size_t pl_slot_id_at(const pl_slot_id *ids, size_t capacity, const char *slot)
{
    uint64_t h = ((uint64_t)(uintptr_t)slot >> 3) * UINT64_C(11400714819323198485);
    size_t mask = capacity - 1, at = (size_t)(h ^ (h >> 29)) & mask;
    while (ids[at].slot && ids[at].slot != slot) at = (at + 1) & mask;
    return at;
}
int sh_package_link_reuse_templates_encode(const sh_package_link_reuse *reuse, unsigned char **bytes, size_t *length)
{
    pl_bytes b = {0};
    pl_slot_id *ids = NULL;
    const char **slots = NULL;
    size_t id_capacity = 0, slot_count = 0, template_count = 0, count_at;
    if (bytes) *bytes = NULL;
    if (length) *length = 0;
    if (!reuse || !bytes || !length) return 0;
    for (size_t i = 0; i < reuse->template_capacity; i++) {
        const pl_template *t = reuse->templates + i;
        for (size_t k = 0; t->file && k < t->count; k++) {
            if (slot_count + 1 > id_capacity / 2) {
                size_t next = id_capacity ? id_capacity * 2 : 4096;
                pl_slot_id *grown = next <= SIZE_MAX / 2 / sizeof(*grown) ? calloc(next, sizeof(*grown)) : NULL;
                const char **listed = grown ? realloc((void *)slots, next / 2 * sizeof(*slots)) : NULL;
                if (!listed) { free(grown); goto fail; }
                slots = listed;
                for (size_t j = 0; j < id_capacity; j++) if (ids[j].slot) grown[pl_slot_id_at(grown, next, ids[j].slot)] = ids[j];
                free(ids); ids = grown; id_capacity = next;
            }
            size_t at = pl_slot_id_at(ids, id_capacity, t->slots[k]);
            if (!ids[at].slot) {
                if (slot_count >= UINT32_MAX) goto fail;
                ids[at] = (pl_slot_id){t->slots[k], (uint32_t)slot_count}; slots[slot_count++] = t->slots[k];
            }
        }
    }
    pl_bytes_put(&b, "SMPIT1", 6);
    pl_bytes_u64(&b, slot_count);
    for (size_t i = 0; i < slot_count; i++) {
        size_t n = strlen(slots[i]);
        pl_bytes_varint(&b, n); pl_bytes_put(&b, slots[i], n + 1);
    }
    count_at = b.length;
    pl_bytes_u64(&b, 0); /* the template count, once known */
    for (size_t i = 0; i < reuse->template_capacity; i++) {
        const pl_template *t = reuse->templates + i;
        if (!t->file || !t->count) continue;
        const char *path = t->file->engine_path;
        size_t path_length = strlen(path), start = b.length;
        char prefix[32];
        int digits = snprintf(prefix, sizeof(prefix), "%zu:", path_length), kept = digits > 0;
        pl_bytes_u64(&b, pl_occ_hash(path, path_length));
        pl_bytes_varint(&b, t->count);
        /* Each key read back as the prefix, the slot and its occurrence; a key that does not read so (none should)
         * leaves its file unrecorded. */
        for (size_t k = 0; kept && k < t->count; k++) {
            const char *key = t->keys[k], *slot = t->slots[k], *rest;
            size_t slot_length = strlen(slot);
            uint64_t occurrence = 0;
            kept = !strncmp(key, prefix, (size_t)digits) && !strncmp(key + digits, path, path_length) &&
                !strncmp(key + digits + path_length, slot, slot_length);
            rest = kept ? key + digits + path_length + slot_length : "";
            if (*rest) {
                kept = *rest++ == '#' && *rest >= '1' && *rest <= '9';
                for (; kept && *rest; rest++) {
                    kept = *rest >= '0' && *rest <= '9' && occurrence <= (UINT64_MAX - 9) / 10;
                    occurrence = occurrence * 10 + (uint64_t)(*rest - '0');
                }
                kept = kept && occurrence >= 2 && occurrence <= k + 1;
            }
            if (!kept) break;
            pl_bytes_varint(&b, ids[pl_slot_id_at(ids, id_capacity, slot)].index);
            pl_bytes_varint(&b, occurrence); pl_bytes_varint(&b, t->order[k]);
        }
        if (!kept) { b.length = start; continue; }
        template_count++;
    }
    if (b.failed) goto fail;
    for (int i = 0; i < 8; i++) b.bytes[count_at + i] = (unsigned char)((uint64_t)template_count >> (8 * i));
    free(ids); free((void *)slots);
    *bytes = b.bytes; *length = b.length;
    return 1;
fail:
    free(ids); free((void *)slots); free(b.bytes);
    return 0;
}
static void pl_offer_release(sh_package_link_reuse *reuse)
{
    free(reuse->offer); free((void *)reuse->offer_slots); free(reuse->offer_index);
    reuse->offer = NULL; reuse->offer_slots = NULL; reuse->offer_index = NULL;
    reuse->offer_length = reuse->offer_slot_count = reuse->offer_capacity = 0;
    if (reuse->offer_state == 1) reuse->offer_state = 2;
}
/* Index an offered record: every count, index, occurrence and order is checked against what remains and the table it
 * names before anything is kept; a malformed record is dropped whole (state -1). */
int sh_package_link_reuse_offer_templates(sh_package_link_reuse *reuse, unsigned char *bytes, size_t length)
{
    unsigned long long began = pl_now_us();
    unsigned char *seen = NULL;
    size_t seen_capacity = 0;
    uint64_t slots = 0, templates = 0;
    int result = 0;
    if (!reuse || reuse->offer_state) { free(bytes); return 0; }
    pl_cursor c = {bytes, bytes + length};
    if (!bytes || length < 6 || memcmp(bytes, "SMPIT1", 6)) goto done;
    c.at += 6;
    if (!pl_cursor_u64(&c, &slots) || slots > (uint64_t)(c.end - c.at) / 2) goto done;
    if (!(reuse->offer_slots = pl_alloc((size_t)slots, sizeof(*reuse->offer_slots)))) { result = -1; goto done; }
    for (size_t i = 0; i < slots; i++) {
        uint64_t n;
        if (!pl_cursor_varint(&c, &n) || n >= (uint64_t)(c.end - c.at) || c.at[n] || memchr(c.at, 0, (size_t)n)) goto done;
        reuse->offer_slots[i] = (const char *)c.at; c.at += n + 1;
    }
    reuse->offer_slot_count = (size_t)slots;
    if (!pl_cursor_u64(&c, &templates) || templates > (uint64_t)(c.end - c.at) / 12) goto done;
    size_t capacity = 16;
    while (capacity / 2 < templates) capacity *= 2;
    if (!(reuse->offer_index = pl_alloc(capacity, sizeof(*reuse->offer_index)))) { result = -1; goto done; }
    reuse->offer_capacity = capacity;
    for (size_t i = 0; i < templates; i++) {
        size_t at = (size_t)(c.at - bytes);
        uint64_t hash, count;
        if (!pl_cursor_u64(&c, &hash) || !pl_cursor_varint(&c, &count) || !count || count > UINT32_MAX ||
            count > (uint64_t)(c.end - c.at) / 3) goto done;
        if (count > seen_capacity) {
            unsigned char *grown = realloc(seen, (size_t)count);
            if (!grown) { result = -1; goto done; }
            seen = grown; seen_capacity = (size_t)count;
        }
        memset(seen, 0, (size_t)count);
        for (uint64_t k = 0; k < count; k++) {
            uint64_t slot, occurrence, position;
            if (!pl_cursor_varint(&c, &slot) || !pl_cursor_varint(&c, &occurrence) || !pl_cursor_varint(&c, &position) ||
                slot >= slots || (occurrence && (occurrence < 2 || occurrence > k + 1)) || position >= count || seen[position])
                goto done;
            seen[position] = 1;
        }
        size_t mask = capacity - 1, to = (size_t)hash & mask;
        while (reuse->offer_index[to].at) to = (to + 1) & mask;
        reuse->offer_index[to] = (struct pl_offer_entry){hash, at + 1};
    }
    if (c.at != c.end) goto done;
    reuse->offer = bytes; bytes = NULL; reuse->offer_length = length;
    reuse->offer_templates = (size_t)templates; reuse->offer_state = 1;
    result = 1;
done:
    free(seen); free(bytes);
    if (result != 1) {
        free((void *)reuse->offer_slots); free(reuse->offer_index);
        reuse->offer_slots = NULL; reuse->offer_index = NULL; reuse->offer_slot_count = reuse->offer_capacity = 0;
        reuse->offer_state = -1;
    }
    reuse->offer_bytes = length; reuse->offer_us = pl_now_us() - began;
    return result;
}
void sh_package_link_reuse_offer_counts(const sh_package_link_reuse *reuse, size_t *offered, size_t *taken,
    size_t *refused)
{
    if (offered) *offered = reuse ? reuse->offer_templates : 0;
    if (taken) *taken = reuse ? reuse->offer_taken : 0;
    if (refused) *refused = reuse ? reuse->offer_refused : 0;
}
/* A template for file from the offered record: the first recorded under its path's hash whose slots equal these
 * sites' slots, one by one, gives the occurrences and the order; the keys are built from file's own path into this
 * cache's texts (pl_reuse_text_take: the allocations a fresh capture makes). NULL when none applies (those under the
 * hash whose slots differ counted refused), the file has a template, or an allocation fails (it is then captured
 * afresh). */
static pl_template *pl_template_offered(pl_work *w, const sh_compiled_resource *file,
    const sh_package_file_identity *identity, const sh_package_link_edge *sites, size_t count)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    if (!count || count > UINT32_MAX || (reuse->template_capacity && reuse->templates[pl_template_at(reuse, file)].file))
        return NULL;
    for (size_t k = 0; k < count; k++) if (!sites[k].slot || !*sites[k].slot) return NULL;
    const char *path = file->engine_path;
    size_t path_length = strlen(path), mask = reuse->offer_capacity - 1;
    uint64_t hash = pl_occ_hash(path, path_length), value;
    if (path_length > SIZE_MAX / 4) return NULL;
    for (size_t at = (size_t)hash & mask; reuse->offer_index[at].at; at = (at + 1) & mask) {
        if (reuse->offer_index[at].hash != hash) continue;
        pl_cursor c = {reuse->offer + reuse->offer_index[at].at - 1 + 8, reuse->offer + reuse->offer_length};
        if (!pl_cursor_varint(&c, &value) || value != count) continue;
        const unsigned char *recorded = c.at;
        int same = 1;
        for (size_t k = 0; k < count && same; k++) {
            uint64_t slot, occurrence, position;
            same = pl_cursor_varint(&c, &slot) && pl_cursor_varint(&c, &occurrence) && pl_cursor_varint(&c, &position) &&
                (PL_OFFER_MUTATED(1) || !strcmp(reuse->offer_slots[slot], sites[k].slot));
        }
        if (!same) { reuse->offer_refused++; continue; }
        pl_template *t = pl_template_place(reuse, file);
        if (!t) return NULL;
        pl_template next = {file, *identity, count};
        char prefix[32];
        int digits = snprintf(prefix, sizeof(prefix), "%zu:", path_length), ok = digits > 0;
        next.slots = pl_alloc(count, sizeof(*next.slots)); next.keys = pl_alloc(count, sizeof(*next.keys));
        next.order = pl_alloc(count, sizeof(*next.order));
        ok = ok && next.slots && next.keys && next.order;
        c.at = recorded;
        for (size_t k = 0; ok && k < count; k++) {
            uint64_t slot, occurrence, position;
            char suffix[24];
            (void)pl_cursor_varint(&c, &slot); (void)pl_cursor_varint(&c, &occurrence); (void)pl_cursor_varint(&c, &position);
            int extra = occurrence && !PL_OFFER_MUTATED(2) ?
                snprintf(suffix, sizeof(suffix), "#%llu", (unsigned long long)occurrence) : 0;
            size_t slot_length = strlen(sites[k].slot);
            char *key = extra >= 0 && slot_length <= SIZE_MAX / 4 ?
                malloc((size_t)digits + path_length + slot_length + (size_t)extra + 1) : NULL;
            if (!key) { ok = 0; break; }
            size_t total = 0;
            memcpy(key, prefix, (size_t)digits); total += (size_t)digits;
            memcpy(key + total, path, path_length); total += path_length;
            memcpy(key + total, sites[k].slot, slot_length); total += slot_length;
            memcpy(key + total, suffix, (size_t)extra); total += (size_t)extra; key[total] = 0;
            if (!(next.keys[k] = pl_reuse_text_take(reuse, key, total))) { ok = 0; break; }
            next.slots[k] = sites[k].slot; next.order[k] = PL_OFFER_MUTATED(3) ? (uint32_t)k : (uint32_t)position;
            reuse->template_key_bytes += total + 1;
        }
        if (!ok) { free((void *)next.slots); free((void *)next.keys); free(next.order); return NULL; }
        *t = next; reuse->template_count++; reuse->offer_taken++;
        return t;
    }
    return NULL;
}
/* The unit's edges after its interned prefix take this cache's slot texts, as pl_reuse_store would give them. 0 when
 * a text is refused: the prefix ends there, the rest staying the unit's own. */
static int pl_intern_edges(pl_work *w, pl_unit *u)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    if (u->interned < u->edge_count && w->occ_unit == u) w->occ_unit = NULL; /* its table points at the keys freed here */
    for (; u->interned < u->edge_count; u->interned++) {
        const char *text = pl_reuse_text(reuse, u->edges[u->interned].slot);
        if (!text) return 0;
        free((char *)u->edges[u->interned].slot); u->edges[u->interned].slot = text;
    }
    return 1;
}
static int pl_reuse_valid(const sh_package_link_reuse *reuse, size_t unit)
{ return reuse && unit < reuse->count && reuse->units[unit].captured && !reuse->units[unit].invalid; }
static uint64_t pl_reuse_pair(size_t left, size_t right)
{
    if (left > right) { size_t swap = left; left = right; right = swap; }
    return (uint64_t)(left + 1) << 32 | (uint64_t)(right + 1);
}
static size_t pl_reuse_slot(const sh_package_link_reuse *reuse, uint64_t key)
{
    uint64_t mixed = key * UINT64_C(0x9e3779b97f4a7c15);
    size_t at = (size_t)(mixed ^ (mixed >> 29)) & (reuse->proven_capacity - 1);
    while (reuse->proven[at] && reuse->proven[at] != key) at = (at + 1) & (reuse->proven_capacity - 1);
    return at;
}
/* Exact bytes of two valid captures were compared equal on an earlier pass. */
static int pl_reuse_proven(const sh_package_link_reuse *reuse, size_t left, size_t right)
{
    if (!pl_reuse_valid(reuse, left) || !pl_reuse_valid(reuse, right) || !reuse->proven_capacity) return 0;
    uint64_t key = pl_reuse_pair(left, right);
    return reuse->proven[pl_reuse_slot(reuse, key)] == key;
}
static void pl_reuse_prove(sh_package_link_reuse *reuse, size_t left, size_t right)
{
    if (!pl_reuse_valid(reuse, left) || !pl_reuse_valid(reuse, right)) return;
    if (reuse->proven_count + 1 > reuse->proven_capacity / 2) {
        size_t capacity = reuse->proven_capacity ? reuse->proven_capacity * 2 : 64;
        uint64_t *old = reuse->proven, *grown = capacity <= SIZE_MAX / sizeof(*grown) ? calloc(capacity, sizeof(*grown)) : NULL;
        size_t old_capacity = reuse->proven_capacity;
        if (!grown) return; /* Reuse is optional: the next pass compares again. */
        reuse->proven = grown; reuse->proven_capacity = capacity;
        for (size_t i = 0; i < old_capacity; i++) if (old[i]) reuse->proven[pl_reuse_slot(reuse, old[i])] = old[i];
        free(old);
    }
    uint64_t key = pl_reuse_pair(left, right);
    size_t at = pl_reuse_slot(reuse, key);
    if (!reuse->proven[at]) { reuse->proven[at] = key; reuse->proven_count++; }
}

static char *pl_slot(const char *path, const char *slot)
{
    size_t p = strlen(path), s = strlen(slot);
    char *out;
    if (p > SIZE_MAX - 32 || s > SIZE_MAX - 32 - p) return NULL;
    out = malloc(p + s + 32);
    if (out) snprintf(out, p + s + 32, "%zu:%s%s", p, path, slot);
    return out;
}

static uint64_t pl_occ_hash(const char *key, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) { hash ^= (unsigned char)key[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
/* The live entry for key, else the empty slot where it belongs. */
static size_t pl_occ_find(const pl_work *w, const char *key, size_t length, uint64_t hash)
{
    size_t mask = w->occ_capacity - 1, at = (size_t)hash & mask;
    while (w->occ[at].generation == w->occ_generation && (w->occ[at].hash != hash ||
        w->occ[at].length != length || memcmp(w->occ[at].key, key, length))) at = (at + 1) & mask;
    return at;
}
static int pl_occ_add(pl_work *w, const char *key, size_t length)
{
    if (w->occ_used + 1 > w->occ_capacity / 2) {
        size_t capacity = w->occ_capacity ? w->occ_capacity * 2 : 256, previous = w->occ_capacity;
        pl_occ_entry *old = w->occ, *grown = capacity <= SIZE_MAX / 2 / sizeof(*grown) ? pl_alloc(capacity, sizeof(*grown)) : NULL;
        if (!grown) return 0;
        w->occ = grown; w->occ_capacity = capacity;
        for (size_t i = 0; i < previous; i++) if (old[i].generation == w->occ_generation)
            w->occ[pl_occ_find(w, old[i].key, old[i].length, old[i].hash)] = old[i];
        free(old);
    }
    uint64_t hash = pl_occ_hash(key, length);
    size_t at = pl_occ_find(w, key, length, hash);
    if (w->occ[at].generation == w->occ_generation) w->occ[at].count++;
    else { w->occ[at] = (pl_occ_entry){key, length, 1, w->occ_generation, hash}; w->occ_used++; }
    return 1;
}
/* A stored slot counts as a repeat of itself and of each prefix that ends
 * just before a '#', exactly the keys the scan in pl_occurrence matches. */
static int pl_occ_slot(pl_work *w, const char *slot)
{
    size_t n = strlen(slot);
    if (!pl_occ_add(w, slot, n)) return 0;
    for (size_t j = 0; j < n; j++) if (slot[j] == '#' && !pl_occ_add(w, slot, j)) return 0;
    return 1;
}
/* A file can assign one field more than once; the native reflected reader
 * resolves each assignment (loading its target) and keeps the last. Number the
 * repeats so every binding keeps a unique, replayable slot. Owns key. A file
 * with many bindings counts them in a table kept while its edges grow, rather
 * than scanning them all for each new one. */
#define PL_OCCURRENCE_TABLE 32u
static char *pl_occurrence(pl_work *w, const pl_unit *u, size_t begin, size_t end, char *key)
{
    size_t seen = 0, n = strlen(key);
    int counted = 0;
    if (end - begin >= PL_OCCURRENCE_TABLE) {
        if (w->occ_unit != u || w->occ_begin != begin || w->occ_end > end) {
            w->occ_generation++; w->occ_used = 0;
            w->occ_unit = u; w->occ_begin = w->occ_end = begin;
        }
        counted = 1;
        for (; counted && w->occ_end < end; w->occ_end++) counted = pl_occ_slot(w, u->edges[w->occ_end].slot);
        if (counted) {
            size_t at = pl_occ_find(w, key, n, pl_occ_hash(key, n));
            seen = w->occ[at].generation == w->occ_generation ? w->occ[at].count : 0;
        } else w->occ_unit = NULL; /* out of memory: scan instead */
    }
    if (!counted)
        for (size_t i = begin; i < end; i++)
            if (!strncmp(u->edges[i].slot, key, n) && (!u->edges[i].slot[n] || u->edges[i].slot[n] == '#')) seen++;
    if (!seen) return key;
    char *out = n <= SIZE_MAX - 24 ? malloc(n + 24) : NULL;
    if (out) snprintf(out, n + 24, "%s#%zu", key, seen + 1);
    free(key); return out;
}
static int pl_edge_add(pl_work *w, char *key, size_t target)
{
    pl_unit *u = &w->work[w->unit];
    /* Edges are added only while capturing the current unit (pl_collect after
     * pl_reuse_restore declined it); replay compares. A restored unit borrows
     * the reuse cache's slot strings and is never extended. */
    if (u->borrowed) { free(key); return pl_fail(w, "a restored resource binding cannot gain bindings"); }
    if (u->edge_count == u->edge_capacity) {
        size_t next = u->edge_capacity ? u->edge_capacity * 2 : 8;
        sh_package_link_edge *grown;
        if (next < u->edge_capacity || next > SIZE_MAX / sizeof(*grown) ||
            !(grown = realloc(u->edges, next * sizeof(*grown)))) {
            free(key); return pl_fail(w, "cannot allocate resource bindings");
        }
        u->edges = grown; u->edge_capacity = next;
    }
    u->edges[u->edge_count++] = (sh_package_link_edge){key, target};
    return 1;
}
static int pl_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_work *w = context;
    pl_unit *u = &w->work[w->unit];
    pl_file *file = &u->files[w->file];
    size_t target = SIZE_MAX;
    char *key = NULL;
    if (replacement) *replacement = NULL;
    if (w->failed) return -1;
    if (!replacement || !slot || !*slot || !family || !*family || !name || !*name ||
        !w->adapter->resolve(w->adapter->context, w->unit, family, name, &target, w->error, w->capacity) ||
        target >= w->count || strcmp(w->units[target].family, family) || strcmp(w->units[target].name, name) ||
        !(key = pl_slot(file->resource->engine_path, slot)) ||
        !(key = pl_occurrence(w, u, file->edge_begin, w->replay ? w->next_edge : u->edge_count, key))) {
        pl_fail(w, "resource binding did not resolve to its scoped native identity"); return -1;
    }
    if (w->replay) {
        size_t end = file->edge_begin + file->edge_count;
        if (w->next_edge >= end || u->edges[w->next_edge].target != target ||
            strcmp(u->edges[w->next_edge].slot, key)) {
            free(key); pl_fail(w, "resource bindings changed during linking"); return -1;
        }
        w->next_edge++; free(key);
        *replacement = w->output.names[w->output.classes.nodes[target]];
        if (!strcmp(*replacement, name)) return 0;
        w->rebound_references++;
        if (strcmp(family, "image")) w->rebound_non_image++;
        return 1;
    }
    return pl_edge_add(w, key, target) ? 0 : -1;
}

static int pl_implicit_order(const void *a, const void *b)
{
    const sh_package_link_edge *x = a, *y = b;
    int order = strcmp(x->slot, y->slot);
    return order ? order : x->target == y->target ? 0 : x->target < y->target ? -1 : 1;
}
static void pl_implicit_free(sh_package_link_edge *edges, size_t count)
{
    for (size_t i = 0; i < count; i++) free((char *)edges[i].slot);
    free(edges);
}
static int pl_slot_order(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }
/* One unit's binding slot names sorted for pl_slot_member, in the work's
 * scratch (valid until the next call). NULL when there are none, the scratch
 * cannot grow, a slot is missing or tests decline; callers then compare
 * linearly, with the same answers. */
static const char **pl_slot_index(pl_work *w, const sh_package_link_edge *edges, size_t count)
{
    const sh_package_link_reuse *reuse = w->adapter ? w->adapter->reuse : NULL;
    if (!count || (reuse && reuse->fail_index)) return NULL;
    if (count > w->slot_capacity) {
        const char **grown = count <= SIZE_MAX / sizeof(*grown) ? realloc(w->slot_index, count * sizeof(*grown)) : NULL;
        if (!grown) return NULL;
        w->slot_index = grown; w->slot_capacity = count;
    }
    for (size_t i = 0; i < count; i++) if (!(w->slot_index[i] = edges[i].slot)) return NULL;
    if (count > 1) qsort(w->slot_index, count, sizeof(*w->slot_index), pl_slot_order);
    return w->slot_index;
}
/* Whether slot is in a sorted index, adding the comparisons made to *compares. */
static int pl_slot_member(const char *const *index, size_t count, const char *slot, size_t *compares)
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(slot, index[middle]);
        ++*compares;
        if (!order) return 1;
        if (order < 0) high = middle; else low = middle + 1;
    }
    return 0;
}
/* A conditional binding is absent once its target is certified original
 * (pl_conditional); until then it is read like any implicit binding. Whether
 * slot is one of the unit's conditional slots: searched in index (its
 * conditional slots, pl_slot_index) or, without one, compared one by one. */
static int pl_conditional_slot(pl_work *w, const sh_package_link_unit *source, const char *const *index, const char *slot)
{
    if (index) return pl_slot_member(index, source->conditional_count, slot, &w->cond_compares);
    for (size_t i = 0; i < source->conditional_count; i++)
        if (!strcmp(source->conditional[i].slot, slot)) { w->cond_compares += i + 1; return 1; }
    w->cond_compares += source->conditional_count; /* meter: slot comparisons made */
    return 0;
}
static int pl_implicit_read(pl_work *w)
{
    const sh_package_link_unit *source = w->units + w->unit;
    pl_unit *unit = w->work + w->unit;
    sh_package_link_edge *edges = NULL;
    const char **implicit = NULL;
    size_t total = 0, read = 0, kept = 0, compares = 0;
    int ok = 0;
    if ((source->implicit_count && !source->implicit) || (source->conditional_count && !source->conditional))
        return pl_fail(w, "implicit resource bindings are missing");
    if (source->implicit_count > SIZE_MAX / sizeof(*edges) - source->conditional_count)
        return pl_fail(w, "too many implicit resource bindings");
    total = source->implicit_count + source->conditional_count;
    if (total) {
        edges = pl_alloc(total, sizeof(*edges));
        if (!edges) return pl_fail(w, "cannot capture implicit resource bindings");
    }
    for (size_t i = 0; i < total; i++) {
        int conditional = i >= source->implicit_count;
        const sh_package_link_edge *edge = conditional ? source->conditional + (i - source->implicit_count) :
            source->implicit + i;
        if (!edge->slot || !*edge->slot || edge->target >= w->count) {
            pl_fail(w, "implicit resource binding has an invalid slot or target"); goto done;
        }
        if (conditional) {
            /* Every implicit slot is checked by now; index them once per unit. */
            if (i == source->implicit_count) implicit = pl_slot_index(w, source->implicit, source->implicit_count);
            if (implicit ? pl_slot_member(implicit, source->implicit_count, edge->slot, &compares) : 0) {
                pl_fail(w, "a resource binding slot is both conditional and implicit"); goto done;
            }
            if (!implicit) for (size_t j = 0; j < source->implicit_count; j++)
                if (source->implicit[j].slot && !strcmp(source->implicit[j].slot, edge->slot)) {
                    pl_fail(w, "a resource binding slot is both conditional and implicit"); goto done;
                }
            if (w->effective && w->effective[edge->target]) continue;
        }
        edges[read].slot = _strdup(edge->slot); edges[read].target = edge->target;
        if (!edges[read++].slot) { pl_fail(w, "cannot capture implicit resource binding slot"); goto done; }
    }
    if (read > 1) qsort(edges, read, sizeof(*edges), pl_implicit_order);
    for (size_t i = 0; i < read; i++) {
        if (kept && !strcmp(edges[kept - 1].slot, edges[i].slot)) {
            if (edges[kept - 1].target != edges[i].target) {
                pl_fail(w, "implicit resource slot has contradictory captured targets"); goto done;
            }
            free((char *)edges[i].slot); edges[i].slot = NULL;
        } else {
            if (kept != i) { edges[kept] = edges[i]; edges[i].slot = NULL; }
            kept++;
        }
    }
    if (w->replay) {
        if (kept != unit->implicit_count) { pl_fail(w, "implicit resource bindings changed during linking"); goto done; }
        for (size_t i = 0; i < kept; i++)
            if (strcmp(edges[i].slot, unit->implicit[i].slot) || edges[i].target != unit->implicit[i].target) {
                pl_fail(w, "implicit resource bindings changed during linking"); goto done;
            }
    } else {
        unit->implicit_begin = unit->edge_count;
        for (size_t i = 0; i < kept; i++) {
            char *key = pl_slot("", edges[i].slot);
            if (!key) { pl_fail(w, "cannot retain implicit resource binding key"); goto done; }
            if (!pl_edge_add(w, key, edges[i].target)) goto done;
        }
        unit->implicit = edges; unit->implicit_count = kept; edges = NULL;
    }
    ok = 1;
done:
    if (edges) pl_implicit_free(edges, total);
    return ok;
}

/* Give a restored unit its own binding arrays before they are edited; the
 * cache entry keeps the complete capture. On failure nothing changes. */
static int pl_unshare(pl_work *w, pl_unit *u)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    sh_package_link_edge *edges = NULL, *implicit = NULL;
    if (!(reuse && reuse->fail_unshare && !--reuse->fail_unshare)) {
        edges = pl_alloc(u->edge_count, sizeof(*edges));
        implicit = pl_alloc(u->implicit_count, sizeof(*implicit));
    }
    if (!edges || !implicit) { free(edges); free(implicit); return 0; }
    if (u->edge_count) memcpy(edges, u->edges, u->edge_count * sizeof(*edges));
    if (u->implicit_count) memcpy(implicit, u->implicit, u->implicit_count * sizeof(*implicit));
    u->edges = edges; u->implicit = implicit; u->edge_capacity = u->edge_count;
    u->shared &= ~(unsigned)PL_SHARED_BINDINGS;
    if (reuse) reuse->unshared++;
    return 1;
}

/* Remove the conditional bindings whose targets are certified original.
 * Certification over the complete graph, these bindings included, equals
 * certification over the bindings that remain: an original target reaches
 * nothing uncertified, so no binding to one changes any unit's certification.
 * Unit-level, from each unit's own original binding, over the graph pl_assign
 * certifies (stable names excluded). Replay (pl_implicit_read) removes the
 * same bindings; captures stay complete in the reuse cache. */
static int pl_conditional(pl_work *w)
{
    size_t conditional = 0;
    unsigned long long began = pl_now_us(), mark;
    for (size_t i = 0; i < w->count; i++) {
        conditional |= w->units[i].conditional_count;
        if (w->units[i].conditional_count) { w->cond_units++; w->cond_edges += w->units[i].conditional_count; }
    }
    if (!conditional) return 1;
    unsigned char *local = pl_alloc(w->count, 1);
    pl_view held = {0};
    sh_package_link_node *view = NULL;
    w->effective = pl_alloc(w->count, 1);
    mark = pl_now_us();
    if (local && w->effective) view = pl_stable_view(w, &held);
    w->cond_view_us = pl_now_us() - mark;
    if (!view) { free(local); return pl_fail(w, "cannot certify conditional resource bindings"); }
    for (size_t i = 0; i < w->count; i++) {
        local[i] = w->units[i].original_binding == 1;
        w->cond_nonlocal += !local[i]; w->cond_walk_edges += view[i].edge_count;
    }
    mark = pl_now_us();
    int ok = sh_package_link_originals(view, w->count, local, w->effective, w->error, w->capacity);
    w->cond_walk_us = pl_now_us() - mark;
    pl_stable_view_end(w, &held);
    free(local);
    if (!ok) return 0;
    {   /* Meter: the whole certification result as one word, comparable between passes. */
        uint64_t digest = UINT64_C(14695981039346656037);
        for (size_t i = 0; i < w->count; i++) {
            w->cond_certified += w->effective[i];
            digest = (digest ^ w->effective[i]) * UINT64_C(1099511628211);
        }
        w->cond_digest = digest;
    }
    mark = pl_now_us();
    for (size_t i = 0; i < w->count; i++) {
        const sh_package_link_unit *source = w->units + i;
        pl_unit *u = w->work + i;
        size_t kept = 0;
        if (!source->conditional_count) continue;
        if (u->implicit_begin > u->edge_count || u->edge_count - u->implicit_begin != u->implicit_count)
            return pl_fail(w, "implicit resource bindings are not the unit's last bindings");
        /* The slot index only once an implicit target is certified original (R-348: most units have none, and every
         * identity pass asked again). */
        const char *const *index = NULL;
        int indexed = 0;
        size_t removed = 0;
        for (size_t k = 0; k < u->implicit_count && !removed; k++) {
            if (!w->effective[u->implicit[k].target]) continue;
            if (!indexed) { index = pl_slot_index(w, source->conditional, source->conditional_count); indexed = 1; }
            removed = pl_conditional_slot(w, source, index, u->implicit[k].slot);
        }
        if (!removed) continue;
        if ((u->shared & PL_SHARED_BINDINGS) && !pl_unshare(w, u))
            return pl_fail(w, "cannot copy conditional resource bindings");
        for (size_t k = 0; k < u->implicit_count; k++) {
            sh_package_link_edge *edge = u->implicit + k, *bound = u->edges + u->implicit_begin + k;
            if (w->effective[edge->target] && pl_conditional_slot(w, source, index, edge->slot)) {
                if (!u->borrowed) { free((char *)edge->slot); free((char *)bound->slot); }
                w->cond_removed++;
                continue;
            }
            u->implicit[kept] = *edge; u->edges[u->implicit_begin + kept] = *bound; kept++;
        }
        u->edge_count = u->implicit_begin + kept; u->implicit_count = kept;
        w->nodes[i].edges = u->edges; w->nodes[i].edge_count = u->edge_count; w->nodes[i].slot_order = NULL;
    }
    w->cond_filter_us = pl_now_us() - mark; w->cond_us = pl_now_us() - began;
    return 1;
}

/* A first identity capture of a stable unit needs no bytes: its sites come
 * from discovery (adapter->sites) and its identity from the compilation's own
 * record of the resource (captured digest, sealed file or retained body), the
 * same bytes a read would return and verify. 1 captured, 0 not stable (bind
 * the bytes), -1 failed. */
/* A captured file's payload identity, from the adapter's shared cache when
 * it keeps one (sh_package_link_adapter.identity). */
static int pl_file_identity(pl_work *w, const sh_package_link_unit *source, const sh_compiled_resource *file,
    sh_package_file_identity *out)
{
    return w->adapter->identity ? w->adapter->identity(w->adapter->context, w->unit, file, out, w->error, w->capacity) :
        sh_package_compilation_identity(source->compilation, file, out, w->error, w->capacity);
}
static int pl_bind_stable(pl_work *w)
{
    const sh_package_link_unit *source = &w->units[w->unit];
    pl_unit *u = &w->work[w->unit];
    pl_file *file = &u->files[w->file];
    const sh_package_link_edge *sites = NULL; size_t count = 0;
    sh_package_file_identity identity;
    if (w->replay || !w->adapter->sites) return 0;
    int supplied = w->adapter->sites(w->adapter->context, w->unit, file->resource, &sites, &count);
    if (supplied < 0) { pl_fail(w, "resource binding sites are unavailable"); return -1; }
    if (!supplied) return 0;
    if (!pl_file_identity(w, source, file->resource, &identity)) {
        pl_fail(w, "resource identity is unavailable"); return -1;
    }
    file->identity = identity; file->edge_begin = u->edge_count;
    unsigned long long began = pl_now_us();
    int templating = pl_templating(w) && u->interned == u->edge_count, restored = 0;
    pl_template *t = templating ? pl_template_find(w, file->resource, &identity, sites, count) : NULL;
    if (!t && templating && w->adapter->reuse->offer_state == 1)
        restored = (t = pl_template_offered(w, file->resource, &identity, sites, count)) != NULL;
    if (t) {
        /* The file's template: its keys, with these sites' targets. */
        if (count > u->edge_capacity - u->edge_count) {
            size_t next = u->edge_capacity ? u->edge_capacity : 8;
            while (next - u->edge_count < count && next <= SIZE_MAX / 2 / sizeof(*u->edges)) next *= 2;
            sh_package_link_edge *grown = next - u->edge_count >= count ? realloc(u->edges, next * sizeof(*grown)) : NULL;
            if (!grown) { pl_fail(w, "cannot allocate resource bindings"); return -1; }
            u->edges = grown; u->edge_capacity = next;
        }
        for (size_t i = 0; i < count; i++) {
            if (sites[i].target >= w->count) {
                pl_fail(w, "resource binding did not resolve to its scoped native identity"); return -1;
            }
            u->edges[u->edge_count++] = (sh_package_link_edge){t->keys[i], sites[i].target};
        }
        u->interned = u->edge_count; file->edge_count = count; w->file_template = t;
        if (restored) { w->template_restored++; w->template_restore_us += pl_now_us() - began; return 1; }
        w->template_files++; w->template_edges += count;
        w->template_hit_us += pl_now_us() - began;
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        char *key = NULL;
        if (!sites[i].slot || !*sites[i].slot || sites[i].target >= w->count ||
            !(key = pl_slot(file->resource->engine_path, sites[i].slot)) ||
            !(key = pl_occurrence(w, u, file->edge_begin, u->edge_count, key))) {
            pl_fail(w, "resource binding did not resolve to its scoped native identity"); return -1;
        }
        if (!pl_edge_add(w, key, sites[i].target)) return -1;
    }
    file->edge_count = u->edge_count - file->edge_begin;
    if (templating && pl_intern_edges(w, u) &&
        (w->file_template = pl_template_add(w, file->resource, &identity, sites, u->edges + file->edge_begin, count)) != NULL)
        w->template_built++;
    w->template_build_us += pl_now_us() - began;
    return 1;
}
/* Renamed only through what it binds: the unit's own binding is a certified
 * original and no package supplies any of its files, yet its class (c) is
 * served under another name than the unit's own. */
static int pl_rebound_original(const pl_work *w, size_t c)
{
    const sh_package_link_unit *unit = &w->units[w->unit];
    if (unit->original_binding != 1 || !w->output.names || !w->output.names[c] || !unit->name) return 0;
    for (size_t f = 0; f < unit->file_count; f++) if (!unit->files[f] || unit->files[f]->source_count) return 0;
    return strcmp(w->output.names[c], unit->name) != 0;
}
static unsigned char *pl_bind(pl_work *w, size_t *length, int *unchanged)
{
    const sh_package_link_unit *source = &w->units[w->unit];
    pl_unit *u = &w->work[w->unit];
    pl_file *file = &u->files[w->file];
    size_t size = 0;
    unsigned char *body = sh_package_compilation_read(source->compilation,
        file->resource, SIZE_MAX, &size, w->error, w->capacity), *bound = NULL;
    sh_package_file_identity identity;
    if (unchanged) *unchanged = 0;
    if (!body || !sh_package_bytes_identity(body, size, &identity)) goto done;
    if (w->replay) {
        if (identity.length != file->identity.length || memcmp(identity.digest, file->identity.digest, 32)) {
            pl_fail(w, "resource bytes changed during linking"); goto done;
        }
        w->next_edge = file->edge_begin;
    } else {
        file->identity = identity; file->edge_begin = u->edge_count;
        if (w->adapter->sites) {
            /* Sites the caller already bound from these bytes, in bind order:
             * record them exactly as pl_reference would, without the walk. */
            const sh_package_link_edge *sites = NULL; size_t count = 0;
            int supplied = w->adapter->sites(w->adapter->context, w->unit, file->resource, &sites, &count);
            if (supplied < 0) { pl_fail(w, "resource binding sites are unavailable"); goto done; }
            if (supplied) {
                for (size_t i = 0; i < count; i++) {
                    char *key = NULL;
                    if (!sites[i].slot || !*sites[i].slot || sites[i].target >= w->count ||
                        !(key = pl_slot(file->resource->engine_path, sites[i].slot)) ||
                        !(key = pl_occurrence(w, u, file->edge_begin, u->edge_count, key))) {
                        pl_fail(w, "resource binding did not resolve to its scoped native identity"); goto done;
                    }
                    if (!pl_edge_add(w, key, sites[i].target)) goto done;
                }
                file->edge_count = u->edge_count - file->edge_begin;
                if (unchanged) *unchanged = 1;
                *length = size; bound = body; body = NULL;
                goto done;
            }
        }
    }
    bound = w->adapter->bind(w->adapter->context, w->unit, file->resource,
        body, size, pl_reference, w, length, w->error, w->capacity);
    if (!bound || w->failed) goto done;
    if (unchanged) *unchanged = size == *length && !memcmp(body, bound, size);
    if (w->replay) {
        if (w->next_edge != file->edge_begin + file->edge_count) {
            pl_fail(w, "resource bindings disappeared during linking"); goto done;
        }
    } else {
        if (size != *length || memcmp(body, bound, size)) {
            pl_fail(w, "resource inspection changed source bytes"); goto done;
        }
        file->edge_count = u->edge_count - file->edge_begin;
    }
done:
    free(body);
    if (!bound || w->failed) {
        free(bound); bound = NULL;
        pl_fail(w, "resource binding inspection is incomplete");
    }
    return bound;
}

static void pl_u64(unsigned char **cursor, uint64_t value)
{
    for (size_t i = 0; i < 8; i++) (*cursor)[i] = (unsigned char)(value >> (i * 8));
    *cursor += 8;
}
/* Meter: a sticky-invalid unit is captured afresh on every pass. Fold what that capture
 * holds into one word and compare it with the unit's previous capture, so the pass detail
 * can say how many recaptures reproduced the same facts and how many found a change. Never
 * changes a result; an allocation failure only skips the meter. */
static uint64_t pl_capture_fp(const pl_unit *u, const sh_package_link_node *node)
{
    uint64_t h = UINT64_C(14695981039346656037);
    h = pl_fnv(h, &node->length, sizeof(node->length)); h = pl_fnv(h, node->content, 32);
    h = pl_fnv(h, &u->edge_count, sizeof(u->edge_count));
    for (size_t i = 0; i < u->edge_count; i++) {
        h = pl_fnv(h, u->edges[i].slot, strlen(u->edges[i].slot) + 1);
        h = pl_fnv(h, &u->edges[i].target, sizeof(u->edges[i].target));
    }
    h = pl_fnv(h, &u->implicit_count, sizeof(u->implicit_count));
    for (size_t i = 0; i < u->implicit_count; i++) {
        h = pl_fnv(h, u->implicit[i].slot, strlen(u->implicit[i].slot) + 1);
        h = pl_fnv(h, &u->implicit[i].target, sizeof(u->implicit[i].target));
    }
    return h | 1;
}
static void pl_meter_again(pl_work *w, const pl_unit *u, const sh_package_link_node *node)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    if (!reuse || w->unit >= reuse->count || !reuse->units[w->unit].invalid) return;
    if (reuse->fp_count < reuse->count) {
        uint64_t *grown = reuse->fail_meter ? NULL : calloc(reuse->count, sizeof(*grown));
        if (!grown) return;
        if (reuse->fp_count) memcpy(grown, reuse->fp, reuse->fp_count * sizeof(*grown));
        free(reuse->fp); reuse->fp = grown; reuse->fp_count = reuse->count;
    }
    uint64_t fp = pl_capture_fp(u, node), *seen = reuse->fp + w->unit;
    if (!*seen) w->again_first++; else if (*seen == fp) w->again_same++; else w->again_changed++;
    *seen = fp;
}
static int pl_semantics(pl_work *w, const sh_package_link_unit *source, sh_package_link_node *node)
{
    memcpy(node->semantics, source->semantics, 32);
    if (source->fixed_name) {
        static const char domain[] = "snapmap-plus/shared-native-name";
        unsigned char bytes[sizeof(domain) + 32]; sh_package_file_identity identity;
        memcpy(bytes, domain, sizeof(domain)); memcpy(bytes + sizeof(domain), source->semantics, 32);
        if (!sh_package_bytes_identity(bytes, sizeof(bytes), &identity))
            return pl_fail(w, "cannot identify native name binding contract");
        memcpy(node->semantics, identity.digest, 32);
    }
    return 1;
}

/* Restore a valid earlier capture of this unit. Return 1 when restored, 0 to
 * capture afresh; an incomplete copy is discarded and captured afresh. The
 * unit borrows the entry's arrays: nothing writes them during this pass
 * (pl_conditional copies first), and the entry outlives it, since only
 * captured (not restored) units store and invalidation, growth and close
 * wait for the pass to end. */
static int pl_reuse_restore(pl_work *w, const sh_package_link_unit *source, pl_unit *u, sh_package_link_node *node)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    if (!pl_reuse_valid(reuse, w->unit)) return 0;
    const pl_reuse_unit *c = reuse->units + w->unit;
    if (c->file_count != source->file_count || c->witness_count != source->witness_count) return 0;
    if (reuse->copy_restore) {
        u->files = pl_alloc(c->file_count, sizeof(*u->files));
        u->edges = pl_alloc(c->edge_count, sizeof(*u->edges));
        u->implicit = pl_alloc(c->implicit_count, sizeof(*u->implicit));
        if (c->witness_count) {
            u->witnesses = pl_alloc(c->witness_count, sizeof(*u->witnesses));
            u->witness_order = pl_alloc(c->witness_count, sizeof(*u->witness_order));
        }
        if (!u->files || !u->edges || !u->implicit || (c->witness_count && (!u->witnesses || !u->witness_order))) {
            free(u->edges); free(u->implicit);
            free(u->files); free(u->witnesses); free(u->witness_order);
            memset(u, 0, sizeof(*u)); return 0;
        }
        if (c->edge_count) memcpy(u->edges, c->edges, c->edge_count * sizeof(*u->edges));
        if (c->implicit_count) memcpy(u->implicit, c->implicit, c->implicit_count * sizeof(*u->implicit));
        memcpy(u->files, c->files, c->file_count * sizeof(*u->files));
        if (c->witness_count) {
            memcpy(u->witnesses, c->witnesses, c->witness_count * sizeof(*u->witnesses));
            memcpy((void *)u->witness_order, c->witness_order, c->witness_count * sizeof(*u->witness_order));
        }
    } else {
        u->files = c->files; u->witnesses = c->witnesses; u->witness_order = c->witness_order;
        u->edges = c->edges; u->implicit = c->implicit;
        u->shared = PL_SHARED_FILES | PL_SHARED_BINDINGS;
        reuse->borrowed_restores++;
    }
    u->borrowed = 1;
    u->edge_count = u->edge_capacity = c->edge_count;
    u->implicit_count = c->implicit_count; u->implicit_begin = c->implicit_begin;
    node->family = source->family; node->path = source->name; node->length = c->length;
    memcpy(node->content, c->content, 32); memcpy(node->semantics, source->semantics, 32);
    node->edges = u->edges; node->edge_count = u->edge_count;
    node->slot_order = u->edges == c->edges ? pl_reuse_order(c) : NULL;
    return 1;
}
/* Tests only (copy mode): kept edges read the cache's shared slot texts; only the array is owned. */
static sh_package_link_edge *pl_edges_copy(sh_package_link_reuse *reuse, const sh_package_link_edge *edges, size_t count)
{
    sh_package_link_edge *out = pl_alloc(count, sizeof(*out));
    for (size_t i = 0; out && i < count; i++) {
        out[i].target = edges[i].target;
        if (!(out[i].slot = pl_reuse_text(reuse, edges[i].slot))) { free(out); out = NULL; }
    }
    return out;
}
/* Tests only (copy mode, the reference borrowed_restore_equivalence compares
 * with): the entry copies the capture and the unit keeps its own arrays, reading
 * the cache's shared slot texts. */
static void pl_reuse_store_copied(pl_work *w, const sh_package_link_unit *source, pl_unit *u,
    const sh_package_link_node *node)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    pl_reuse_unit next = {0};
    next.files = pl_alloc(source->file_count, sizeof(*next.files));
    next.edges = pl_edges_copy(reuse, u->edges, u->edge_count);
    next.implicit = pl_edges_copy(reuse, u->implicit, u->implicit_count);
    if (source->witness_count) {
        next.witnesses = pl_alloc(source->witness_count, sizeof(*next.witnesses));
        next.witness_order = pl_alloc(source->witness_count, sizeof(*next.witness_order));
    }
    next.edge_count = u->edge_count; next.implicit_count = u->implicit_count;
    if (!next.files || !next.edges || !next.implicit ||
        (source->witness_count && (!next.witnesses || !next.witness_order))) {
        pl_reuse_unit_clear(&next); return; /* Reuse is optional: capture again next pass. */
    }
    memcpy(next.files, u->files, source->file_count * sizeof(*next.files));
    if (source->witness_count) {
        memcpy(next.witnesses, u->witnesses, source->witness_count * sizeof(*next.witnesses));
        memcpy((void *)next.witness_order, u->witness_order, source->witness_count * sizeof(*next.witness_order));
    }
    next.file_count = source->file_count; next.witness_count = source->witness_count;
    next.implicit_begin = u->implicit_begin; next.length = node->length;
    memcpy(next.content, node->content, 32); next.captured = 1;
    pl_reuse_unit_clear(reuse->units + w->unit); reuse->units[w->unit] = next;
    /* pl_occurrence's table points at the slot strings freed here. */
    if (w->occ_unit == u) w->occ_unit = NULL;
    for (size_t i = 0; i < u->edge_count; i++) { free((char *)u->edges[i].slot); u->edges[i].slot = next.edges[i].slot; }
    for (size_t i = 0; i < u->implicit_count; i++) {
        free((char *)u->implicit[i].slot); u->implicit[i].slot = next.implicit[i].slot;
    }
    u->borrowed = 1;
}
/* Keep this unit's completed capture for later passes, unless invalidated.
 * The entry takes the unit's own arrays, its binding array shrunk to the
 * count and its slots replaced by the cache's shared texts, and the unit
 * borrows them back like a restored unit (pl_unshare copies them before an
 * edit), so each capture is held once while the pass runs. A refused text
 * keeps the unit as it was and stores nothing: reuse is optional, and the
 * unit is captured again next pass. */
static void pl_reuse_store(pl_work *w, const sh_package_link_unit *source, pl_unit *u,
    const sh_package_link_node *node)
{
    sh_package_link_reuse *reuse = w->adapter->reuse;
    pl_reuse_unit *entry;
    const char **texts;
    size_t at = 0;
    if (!reuse || w->unit >= reuse->count || reuse->units[w->unit].invalid || u->shared) return;
    if (reuse->copy_restore) { pl_reuse_store_copied(w, source, u, node); return; }
    if (u->implicit_count > SIZE_MAX / sizeof(*texts) - u->edge_count ||
        !(texts = pl_alloc(u->edge_count + u->implicit_count, sizeof(*texts)))) return;
    /* An interned prefix holds this cache's texts already (pl_intern_edges). */
    for (size_t i = 0; i < u->edge_count; i++)
        if (!(texts[at++] = i < u->interned ? u->edges[i].slot : pl_reuse_text(reuse, u->edges[i].slot))) { free(texts); return; }
    for (size_t i = 0; i < u->implicit_count; i++)
        if (!(texts[at++] = pl_reuse_text(reuse, u->implicit[i].slot))) { free(texts); return; }
    /* pl_occurrence's table points at the slot strings freed here. */
    if (w->occ_unit == u) w->occ_unit = NULL;
    at = 0;
    for (size_t i = 0; i < u->edge_count; i++) {
        if (i >= u->interned) free((char *)u->edges[i].slot);
        u->edges[i].slot = texts[at++];
    }
    for (size_t i = 0; i < u->implicit_count; i++) {
        free((char *)u->implicit[i].slot); u->implicit[i].slot = texts[at++];
    }
    free(texts);
    /* Fit the edges, then their slot order after them (R-343): the partition reads the edges in place through it
     * instead of copying and sorting them on every pass. Without room the entry keeps no order; the partition then
     * orders this unit's edges itself. */
    unsigned char ordered = 0;
    if (u->edge_count) {
        size_t n = u->edge_count;
        sh_package_link_edge *fitted = n <= UINT32_MAX && n <= SIZE_MAX / (sizeof(*fitted) + sizeof(uint32_t)) ?
            realloc(u->edges, n * (sizeof(*fitted) + sizeof(uint32_t))) : NULL;
        if (fitted) {
            uint32_t *order = (uint32_t *)(fitted + n);
            /* A unit of one templated file without implicit edges orders exactly its template's keys. */
            const pl_template *t = w->file_template;
            if (t && source->file_count == 1 && !u->implicit_count && t->count == n) memcpy(order, t->order, n * sizeof(*order));
            else {
                for (size_t i = 0; i < n; i++) order[i] = (uint32_t)i;
                qsort_s(order, n, sizeof(*order), pl_position_order, fitted);
            }
            u->edges = fitted; ordered = 1;
        } else if (u->edge_capacity > n && (fitted = realloc(u->edges, n * sizeof(*fitted))) != NULL) u->edges = fitted;
    }
    entry = reuse->units + w->unit;
    pl_reuse_unit_clear(entry);
    entry->files = u->files; entry->witnesses = u->witnesses; entry->witness_order = u->witness_order;
    entry->edges = u->edges; entry->implicit = u->implicit;
    entry->file_count = source->file_count; entry->witness_count = source->witness_count;
    entry->edge_count = u->edge_count; entry->implicit_count = u->implicit_count;
    entry->implicit_begin = u->implicit_begin; entry->length = node->length;
    memcpy(entry->content, node->content, 32); entry->captured = 1; entry->ordered = ordered;
    u->edge_capacity = u->edge_count;
    u->shared = PL_SHARED_FILES | PL_SHARED_BINDINGS;
    u->borrowed = 1;
}

static int pl_collect(pl_work *w)
{
    for (w->unit = 0; w->unit < w->count; w->unit++) {
        const sh_package_link_unit *source = &w->units[w->unit];
        pl_unit *u = &w->work[w->unit];
        sh_package_link_node *node = &w->nodes[w->unit];
        if (pl_reuse_restore(w, source, u, node)) {
            if (!pl_semantics(w, source, node)) return 0;
            w->restored++;
            continue;
        }
        unsigned long long fresh_began = pl_now_us();
        size_t manifest_size = 8, companions = 0, primary_members = 0;
        w->file_template = NULL;
        unsigned char *manifest, *cursor;
        sh_package_file_identity identity;
        if (!source->family || !*source->family || !source->name || !*source->name ||
            (source->file_count && (!source->files || !source->compilation)) ||
            (source->witness_count && (!source->witnesses || !source->file_count)) ||
            (!source->file_count && (source->implicit_owners || source->implicit_gameplay_owners)))
            return pl_fail(w, "incomplete resource unit");
        u->files = pl_alloc(source->file_count, sizeof(*u->files));
        if (!u->files) return pl_fail(w, "cannot allocate source resource units");
        for (size_t i = 0; i < source->file_count; i++) {
            if (!source->files[i] || !source->files[i]->engine_path || !*source->files[i]->engine_path)
                return pl_fail(w, "resource unit has an unnamed physical file");
            u->files[i].resource = source->files[i];
            const sh_package_link_file_identity *member = source->file_identities ? source->file_identities + i : NULL;
            if (member && (member->family || member->name) &&
                (!member->family || !*member->family || !member->name || !*member->name))
                return pl_fail(w, "resource member has an incomplete native identity");
            u->files[i].family = member && member->family ? member->family : source->family;
            u->files[i].name = member && member->name ? member->name : source->name;
            if (!strcmp(u->files[i].family, source->family) && !strcmp(u->files[i].name, source->name)) primary_members++;
        }
        if (source->file_count && !primary_members)
            return pl_fail(w, "resource unit has no primary native member");
        if (source->file_count > 1) qsort(u->files, source->file_count, sizeof(*u->files), pl_file_order);
        for (w->file = 0; w->file < source->file_count; w->file++) {
            pl_file *f = &u->files[w->file];
            const char *path = f->resource->engine_path;
            size_t length = 0, path_size = strlen(path);
            unsigned char *body;
            if (w->file && !strcmp(path, u->files[w->file - 1].resource->engine_path))
                return pl_fail(w, "resource unit repeats a physical file");
            w->file_template = NULL;
            int stable = pl_bind_stable(w);
            if (stable < 0) return 0;
            if (!stable) {
                body = pl_bind(w, &length, NULL);
                if (!body) return 0;
                free(body);
                /* Its keys are this cache's texts too, so later files of the unit may take templates. */
                if (pl_templating(w) && u->interned == f->edge_begin) (void)pl_intern_edges(w, u);
            }
            if (path_size > SIZE_MAX - 48 || manifest_size > SIZE_MAX - 48 - path_size ||
                f->identity.length > UINT64_MAX - node->length)
                return pl_fail(w, "resource unit identity exceeds addressable size");
            manifest_size += 48 + path_size; node->length += f->identity.length;
            if (strcmp(f->family, source->family) || strcmp(f->name, source->name)) {
                size_t family = strlen(f->family), name = strlen(f->name);
                if (family > SIZE_MAX - 24 || name > SIZE_MAX - 24 - family ||
                    manifest_size > SIZE_MAX - 24 - family - name)
                    return pl_fail(w, "resource member identity exceeds addressable size");
                manifest_size += 24 + family + name; companions++;
            }
        }
        if (companions) {
            if (manifest_size > SIZE_MAX - 16) return pl_fail(w, "resource member identity exceeds addressable size");
            manifest_size += 16;
        }
        if (source->witness_count) {
            if (manifest_size > SIZE_MAX - 16) return pl_fail(w, "resource witness identity exceeds addressable size");
            manifest_size += 16;
            u->witnesses = pl_alloc(source->witness_count, sizeof(*u->witnesses));
            u->witness_order = pl_alloc(source->witness_count, sizeof(*u->witness_order));
            if (!u->witnesses || !u->witness_order) return pl_fail(w, "cannot retain resource witnesses");
            for (size_t i = 0; i < source->witness_count; i++) {
                const sh_package_link_witness *witness = source->witnesses + i;
                if (!witness->role || !*witness->role || !witness->resource ||
                    !witness->resource->engine_path || !*witness->resource->engine_path)
                    return pl_fail(w, "resource witness has no exact role or source path");
                u->witness_order[i] = witness;
            }
            qsort(u->witness_order, source->witness_count, sizeof(*u->witness_order), pl_witness_order);
            for (size_t i = 0; i < source->witness_count; i++) {
                const sh_package_link_witness *witness = u->witness_order[i];
                pl_file *file = u->witnesses + i;
                size_t role = strlen(witness->role), path = strlen(witness->resource->engine_path);
                if (i && !strcmp(witness->role, u->witness_order[i - 1]->role))
                    return pl_fail(w, "resource witness repeats a native role");
                file->resource = witness->resource;
                if (!pl_file_identity(w, source, file->resource, &file->identity))
                    return pl_fail(w, "resource witness identity is unavailable");
                if (!pl_witness_available(w, w->unit, file)) return 0;
                if (role > SIZE_MAX - 56 || path > SIZE_MAX - 56 - role ||
                    manifest_size > SIZE_MAX - 56 - role - path || file->identity.length > UINT64_MAX - node->length)
                    return pl_fail(w, "resource witness identity exceeds addressable size");
                manifest_size += 56 + role + path; node->length += file->identity.length;
            }
        }
        if (!pl_implicit_read(w)) return 0;
        /* A unit of one stable file alone: its content digest is a function of that file's path and identity, which
         * its template holds once a unit of that file alone was identified. */
        pl_template *single = source->file_count == 1 && !companions && !source->witness_count ? w->file_template : NULL;
        if (single && single->digested) {
            memcpy(identity.digest, single->digest, 32); w->template_digests++;
            goto identified;
        }
        manifest = cursor = malloc(manifest_size);
        if (!manifest) return pl_fail(w, "cannot allocate resource unit identity");
        pl_u64(&cursor, source->file_count);
        for (size_t i = 0; i < source->file_count; i++) {
            const pl_file *f = &u->files[i];
            size_t size = strlen(f->resource->engine_path);
            pl_u64(&cursor, size); memcpy(cursor, f->resource->engine_path, size); cursor += size;
            pl_u64(&cursor, f->identity.length); memcpy(cursor, f->identity.digest, 32); cursor += 32;
        }
        if (companions) {
            memcpy(cursor, "members\0", 8); cursor += 8; pl_u64(&cursor, companions);
            for (size_t i = 0; i < source->file_count; i++) {
                const pl_file *f = u->files + i;
                if (!strcmp(f->family, source->family) && !strcmp(f->name, source->name)) continue;
                size_t family = strlen(f->family), name = strlen(f->name);
                pl_u64(&cursor, i);
                pl_u64(&cursor, family); memcpy(cursor, f->family, family); cursor += family;
                pl_u64(&cursor, name); memcpy(cursor, f->name, name); cursor += name;
            }
        }
        if (source->witness_count) {
            memcpy(cursor, "witness\0", 8); cursor += 8; pl_u64(&cursor, source->witness_count);
            for (size_t i = 0; i < source->witness_count; i++) {
                const pl_file *file = u->witnesses + i;
                const sh_package_link_witness *witness = u->witness_order[i];
                size_t role = strlen(witness->role), path = strlen(file->resource->engine_path);
                pl_u64(&cursor, role); memcpy(cursor, witness->role, role); cursor += role;
                pl_u64(&cursor, path); memcpy(cursor, file->resource->engine_path, path); cursor += path;
                pl_u64(&cursor, file->identity.length); memcpy(cursor, file->identity.digest, 32); cursor += 32;
            }
        }
        int ok = sh_package_bytes_identity(manifest, manifest_size, &identity); free(manifest);
        if (!ok) return pl_fail(w, "cannot identify resource unit bytes");
        if (single) { memcpy(single->digest, identity.digest, 32); single->digested = 1; }
identified:
        node->family = source->family; node->path = source->name;
        memcpy(node->content, identity.digest, 32);
        if (!pl_semantics(w, source, node)) return 0;
        node->edges = u->edges; node->edge_count = u->edge_count;
        pl_reuse_store(w, source, u, node);
        node->edges = u->edges; /* the store may have shrunk the array */
        node->slot_order = (u->shared & PL_SHARED_BINDINGS) && w->adapter->reuse ?
            pl_reuse_order(w->adapter->reuse->units + w->unit) : NULL;
        w->fresh++; w->fresh_us += pl_now_us() - fresh_began;
        pl_meter_again(w, u, node);
    }
    return 1;
}

static int pl_input_open_resource(pl_work *w, size_t unit, const pl_file *captured, pl_input *out)
{
    const sh_package_link_unit *source = &w->units[unit];
    const sh_compiled_resource *resource = captured->resource;
    memset(out, 0, sizeof(*out));
    if (resource->body) {
        out->body = resource->body; out->length = resource->body_length;
    } else if (resource->body_owner) {
        out->detached = resource->body_owner; out->length = resource->body_length;
    } else if (resource->cache_file) {
        out->stream = sh_package_file_open(resource->cache_file, &out->length, w->error, w->capacity);
    } else if (source->compilation && source->compilation->sources &&
        source->compilation->sources->files && resource->source < source->compilation->sources->file_count) {
        sh_package_source_file entry = source->compilation->sources->files[resource->source];
        if (resource->cache_path) entry.absolute = resource->cache_path;
        out->length = entry.length;
        out->stream = sh_package_source_open(&entry, w->error, w->capacity);
    }
    if ((!out->body && !out->stream && !out->detached) || out->length != captured->identity.length)
        return pl_fail(w, "resource bytes are unavailable during exact supplier verification");
    return 1;
}
static int pl_input_open(pl_work *w, size_t unit, size_t file, pl_input *out)
{ return pl_input_open_resource(w, unit, &w->work[unit].files[file], out); }

static int pl_input_read(pl_input *input, unsigned char *bytes, size_t length)
{
    if (input->detached && !input->body) {
        size_t got = 0;
        if (!(input->owned = sh_package_body_read(input->detached, &got)) || got != input->length) return 0;
        input->body = input->owned;
    }
    if (input->body) memcpy(bytes, input->body + (size_t)input->position, length);
    else if (fread(bytes, 1, length, input->stream) != length) return 0;
    input->position += length; return 1;
}

/* Physical-path sharing is separate from logical resource equivalence. Even
 * original aliases must prove exact source bytes, not merely matching hashes. */
static int pl_original_file_equal(pl_work *w, size_t left, size_t left_file,
    size_t right, size_t right_file)
{
    pl_input a = {0}, b = {0}; unsigned char x[16384], y[16384]; int equal = -1;
    if (!pl_input_open(w, left, left_file, &a) || !pl_input_open(w, right, right_file, &b)) goto done;
    if (a.length != b.length) { equal = 0; goto done; }
    /* One captured resource object on both sides is one immutable byte sequence (held in memory, or a detached
     * original its body reads again exactly): comparing it with itself is reflexive, as pl_verify_members treats
     * it. Streams are still read, so a file changed under them is noticed. */
    if (w->work[left].files[left_file].resource == w->work[right].files[right_file].resource && (a.body || a.detached)) {
        equal = 1; goto done;
    }
    while (a.position < a.length) {
        uint64_t remaining = a.length - a.position;
        size_t length = remaining < sizeof(x) ? (size_t)remaining : sizeof(x);
        if (!pl_input_read(&a, x, length) || !pl_input_read(&b, y, length)) {
            pl_fail(w, "original alias source read failed"); goto done;
        }
        if (memcmp(x, y, length)) { equal = 0; goto done; }
    }
    if ((a.stream && (fgetc(a.stream) != EOF || ferror(a.stream))) ||
        (b.stream && (fgetc(b.stream) != EOF || ferror(b.stream)))) {
        pl_fail(w, "original alias source length changed"); goto done;
    }
    equal = 1;
done:
    pl_input_close(&a); pl_input_close(&b);
    return equal;
}

static unsigned long long pl_now_us(void);
static int pl_verify_members(pl_work *w, size_t left, size_t right);
/* Fingerprints select comparison candidates; only exact bytes certify sharing.
 * Streams retain their independent positions and hold the immutable source
 * handles throughout the comparison. No whole opaque file is allocated here. */
static int pl_verify_equal(void *context, size_t left, size_t right, char *error, size_t capacity)
{
    pl_work *w = context;
    sh_package_link_reuse *reuse = w->adapter->reuse;
    (void)error; (void)capacity;
    if (pl_reuse_proven(reuse, left, right)) return 1;
    unsigned long long began = reuse ? pl_now_us() : 0;
    int ok = pl_verify_members(w, left, right);
    if (reuse) { reuse->verify_pairs++; reuse->verify_us += pl_now_us() - began; }
    return ok;
}
static int pl_verify_members(pl_work *w, size_t left, size_t right)
{
    unsigned char a[16384], b[16384];
    sh_package_link_reuse *reuse = w->adapter->reuse;
    if (w->units[left].file_count != w->units[right].file_count ||
        w->units[left].witness_count != w->units[right].witness_count)
        return pl_fail(w, "resource fingerprint collision: physical file sets differ");
    if (w->units[left].file_count > SIZE_MAX - w->units[left].witness_count)
        return pl_fail(w, "resource comparison count exceeds addressable size");
    for (size_t i = 0; i < w->units[left].file_count + w->units[left].witness_count; i++) {
        pl_input first = {0}, second = {0};
        int ok = 0;
        int witness = i >= w->units[left].file_count;
        size_t index = witness ? i - w->units[left].file_count : i;
        const pl_file *x = witness ? w->work[left].witnesses + index : w->work[left].files + index;
        const pl_file *y = witness ? w->work[right].witnesses + index : w->work[right].files + index;
        if ((witness && strcmp(w->work[left].witness_order[index]->role, w->work[right].witness_order[index]->role)) ||
            strcmp(x->resource->engine_path, y->resource->engine_path))
            return pl_fail(w, "resource fingerprint collision: physical paths differ");
        if (!witness && (strcmp(x->family, y->family) || strcmp(x->name, y->name)))
            return pl_fail(w, "resource fingerprint collision: native member identities differ");
        /* Both units hold this member as one captured resource object with
         * one captured identity, read through the same supplier: comparing
         * it with itself is reflexive, so only its availability and captured
         * length are checked. */
        if (x->resource == y->resource && x->identity.length == y->identity.length &&
            !memcmp(x->identity.digest, y->identity.digest, sizeof(x->identity.digest)) &&
            (x->resource->body || x->resource->body_owner || x->resource->cache_file ||
             w->units[left].compilation == w->units[right].compilation)) {
            if (!pl_input_open_resource(w, left, x, &first)) goto done;
            if (reuse) reuse->verify_shared++;
            ok = 1; goto done;
        }
        if (!pl_input_open_resource(w, left, x, &first) || !pl_input_open_resource(w, right, y, &second)) goto done;
        if (first.length != second.length) {
            pl_fail(w, "resource fingerprint collision: exact supplier lengths differ"); goto done;
        }
        while (first.position < first.length) {
            uint64_t remaining = first.length - first.position;
            size_t chunk = remaining < sizeof(a) ? (size_t)remaining : sizeof(a);
            if (!pl_input_read(&first, a, chunk) || !pl_input_read(&second, b, chunk)) {
                pl_fail(w, "resource read failed during exact supplier verification"); goto done;
            }
            if (memcmp(a, b, chunk)) {
                pl_fail(w, "resource fingerprint collision: exact supplier bytes differ"); goto done;
            }
            if (reuse) reuse->verify_bytes += chunk;
        }
        if ((first.stream && (fgetc(first.stream) != EOF || ferror(first.stream))) ||
            (second.stream && (fgetc(second.stream) != EOF || ferror(second.stream)))) {
            pl_fail(w, "resource length changed during exact supplier verification"); goto done;
        }
        ok = 1;
done:
        pl_input_close(&first); pl_input_close(&second);
        if (!ok) return 0;
    }
    pl_reuse_prove(reuse, left, right);
    return 1;
}

static int pl_members(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    size_t *cursor = NULL;
    if (classes->count == SIZE_MAX) return pl_fail(w, "too many resource equivalence classes");
    w->member_offsets = pl_alloc(classes->count + 1, sizeof(*w->member_offsets));
    w->members = pl_alloc(w->count, sizeof(*w->members));
    cursor = pl_alloc(classes->count, sizeof(*cursor));
    if (!w->member_offsets || !w->members || !cursor) {
        free(cursor); return pl_fail(w, "cannot allocate resource equivalence members");
    }
    for (size_t i = 0; i < w->count; i++) w->member_offsets[classes->nodes[i] + 1]++;
    for (size_t i = 0; i < classes->count; i++) {
        w->member_offsets[i + 1] += w->member_offsets[i]; cursor[i] = w->member_offsets[i];
    }
    for (size_t i = 0; i < w->count; i++) w->members[cursor[classes->nodes[i]]++] = i;
    free(cursor); return 1;
}

/* This hash indexes host-side strings only. Native ID computation always
 * belongs to the registry callback, which knows the actual native type name. */
static uint64_t pl_name_hash(const char *family, const char *name)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const char *parts[2] = {family, name};
    for (size_t i = 0; i < 2; i++) {
        const unsigned char *p = (const unsigned char *)parts[i];
        do { hash = (hash ^ *p) * UINT64_C(1099511628211); } while (*p++);
    }
    return hash;
}

static size_t pl_table_capacity(size_t count)
{
    size_t capacity = 8;
    if (count > SIZE_MAX / 2) return 0;
    while (capacity < count * 2) {
        if (capacity > SIZE_MAX / 2) return 0;
        capacity *= 2;
    }
    return capacity;
}

static size_t pl_name_slot(const pl_registry *registry, const pl_native_identity *identity, int native)
{
    size_t *table = native ? registry->ids : registry->names;
    uint64_t hash = native ? pl_name_hash(identity->family, "") ^ identity->id :
        pl_name_hash(identity->family, identity->name);
    size_t slot = (size_t)hash & (registry->capacity - 1);
    while (table[slot]) {
        const pl_native_identity *other = registry->identities + table[slot] - 1;
        if (!strcmp(identity->family, other->family) && (native ? identity->id == other->id :
            !strcmp(identity->name, other->name))) break;
        slot = (slot + 1) & (registry->capacity - 1);
    }
    return slot;
}

static size_t pl_path_slot(const pl_registry *registry, const char *path)
{
    size_t slot = (size_t)pl_name_hash("", path) & (registry->path_capacity - 1);
    while (registry->paths[slot] && strcmp(registry->paths[slot], path))
        slot = (slot + 1) & (registry->path_capacity - 1);
    return slot;
}

static void pl_assignment_clear(pl_work *w, size_t group)
{
    size_t index = w->output.classes.representatives[group];
    pl_unit *unit = &w->work[index];
    free(w->output.names[group]); w->output.names[group] = NULL;
    if (unit->paths) for (size_t i = 0; i < w->units[index].file_count; i++) free(unit->paths[i]);
    free(unit->paths); unit->paths = NULL;
    if (unit->names) for (size_t i = 0; i < w->units[index].file_count; i++) free(unit->names[i]);
    free(unit->names); unit->names = NULL;
    pl_derived_clear(unit);
}

static int pl_same_original(pl_work *w, const pl_native_identity *a, const pl_native_identity *b)
{
    /* Different coupled groups may reach the same certified baseline object.
     * Never infer this from equal member bytes in an otherwise modified group. */
    if (!a->original || !b->original || (a->primary && b->primary) || strcmp(a->family, b->family) ||
        strcmp(a->name, b->name) || strcmp(a->original_name, b->original_name)) return 0;
    size_t left = 0, right = 0, matched = 0;
    size_t acount = w->units[a->unit].file_count, bcount = w->units[b->unit].file_count;
    const pl_file *afiles = w->work[a->unit].files, *bfiles = w->work[b->unit].files;
    for (;;) {
        while (left < acount && (strcmp(afiles[left].family, a->family) || strcmp(afiles[left].name, a->original_name))) left++;
        while (right < bcount && (strcmp(bfiles[right].family, b->family) || strcmp(bfiles[right].name, b->original_name))) right++;
        if (left == acount || right == bcount) return left == acount && right == bcount && matched != 0;
        if (strcmp(afiles[left].resource->engine_path, bfiles[right].resource->engine_path)) return 0;
        int equal = pl_original_file_equal(w, a->unit, left, b->unit, right);
        if (equal <= 0) return equal;
        left++; right++; matched++;
    }
}

static int pl_assign_members(pl_work *w, size_t index, size_t group,
    const sh_compiled_resource *const *files, pl_native_identity *identities, size_t *used)
{
    const sh_package_link_unit *source = w->units + index;
    pl_unit *unit = w->work + index;
    size_t count = source->file_count;
    int coupled = 0;
    identities[0] = (pl_native_identity){source->family, w->output.names[group], source->name, 0, w->originals[index], 1, index};
    *used = 1;
    for (size_t i = 0; i < count; i++) if (strcmp(unit->files[i].family, source->family) ||
        strcmp(unit->files[i].name, source->name)) coupled = 1;
    if (!coupled) return 1;
    if (!w->adapter->assign_members ||
        !w->adapter->assign_members(w->adapter->context, index, w->output.names[group],
            files, (const char *const *)unit->paths, count, &unit->names, w->error, w->capacity) || !unit->names)
        return pl_fail(w, "coupled resource members require native name assignment");
    for (size_t i = 0; i < count; i++) {
        const pl_file *file = unit->files + i;
        if (!unit->names[i] || !*unit->names[i]) return pl_fail(w, "native resource member has no assigned name");
        size_t j;
        for (j = 0; j < *used; j++) if (!strcmp(file->family, identities[j].family) &&
            !strcmp(file->name, identities[j].original_name)) break;
        if (j < *used) {
            if (strcmp(unit->names[i], identities[j].name))
                return pl_fail(w, "native resource member assignment splits one authored identity");
        } else identities[(*used)++] = (pl_native_identity){file->family, unit->names[i], file->name, 0, w->originals[index], 0, index};
    }
    return 1;
}

static int pl_name_token(const unsigned char identity[32], size_t attempt, unsigned char token[32])
{
    static const char domain[] = "snapmap-plus/native-name-collision";
    unsigned char bytes[sizeof(domain) + 32 + 8], *cursor = bytes;
    sh_package_file_identity hashed;
    if (!attempt) { memcpy(token, identity, 32); return 1; }
    memcpy(cursor, domain, sizeof(domain)); cursor += sizeof(domain);
    memcpy(cursor, identity, 32); cursor += 32; pl_u64(&cursor, attempt);
    if (!sh_package_bytes_identity(bytes, sizeof(bytes), &hashed)) return 0;
    memcpy(token, hashed.digest, 32); return 1;
}

/* Native managers outlive individual providers. Keep every private name/ID we
 * issue for that same lifetime, with the binding identity it was issued for
 * and the record of what that identity was derived from: the unit's family,
 * name, semantics, name contract, every member and witness file's engine path
 * and payload identity (length and SHA-256), and its bound dependencies'
 * identities, in the order sh_package_link_identify_units hashes them. A
 * repeated identity must come with an equal record; a different record behind
 * the same identity is refused rather than reused. Records live in memory only:
 * nothing about the history is written to the cache. */
typedef struct pl_record {
    volatile LONG references;
    size_t length;
    unsigned char bytes[1];
} pl_record;
typedef struct pl_history_node {
    unsigned char identity[32];
    pl_record *record;
} pl_history_node;
typedef struct pl_history_claim {
    char *family, *name;
    uint32_t id;
    size_t node;
} pl_history_claim;
struct sh_package_link_history {
    pl_history_node *nodes;
    pl_history_claim *claims;
    size_t count, claim_count, capacity, claim_capacity;
    size_t *index, *names, *ids, index_capacity, table_capacity;
};
static pl_record *pl_record_new(const unsigned char *bytes, size_t length)
{
    pl_record *record;
    if (length > SIZE_MAX - sizeof(*record)) return NULL;
    record = malloc(sizeof(*record) + length);
    if (!record) return NULL;
    record->references = 1; record->length = length;
    if (length) memcpy(record->bytes, bytes, length);
    return record;
}
static pl_record *pl_record_retain(pl_record *record)
{ if (record) InterlockedIncrement(&record->references); return record; }
static void pl_record_release(pl_record *record)
{ if (record && !InterlockedDecrement(&record->references)) free(record); }
sh_package_link_history *sh_package_link_history_open(void)
{
    return calloc(1, sizeof(sh_package_link_history));
}
void sh_package_link_history_close(sh_package_link_history *h)
{
    if (!h) return;
    for (size_t i = 0; i < h->count; i++) pl_record_release(h->nodes[i].record);
    for (size_t i = 0; i < h->claim_count; i++) { free(h->claims[i].family); free(h->claims[i].name); }
    free(h->nodes); free(h->claims);
    free(h->index); free(h->names); free(h->ids); free(h);
}
size_t sh_package_link_history_count(const sh_package_link_history *h)
{ return h ? h->count : 0; }
static size_t pl_history_node_slot(const sh_package_link_history *h, const unsigned char identity[32]);
static size_t pl_history_claim_slot(const sh_package_link_history *h, const char *family,
    const char *name, uint32_t id, int numeric);
sh_package_link_history_mark sh_package_link_history_mark_now(const sh_package_link_history *h)
{
    sh_package_link_history_mark mark = {h ? h->count : 0, h ? h->claim_count : 0};
    return mark;
}
int sh_package_link_history_rollback(sh_package_link_history *h, sh_package_link_history_mark mark)
{
    if (!h || mark.nodes > h->count || mark.claims > h->claim_count) return 0;
    for (size_t i = mark.claims; i < h->claim_count; i++) { free(h->claims[i].family); free(h->claims[i].name); }
    for (size_t i = mark.nodes; i < h->count; i++) pl_record_release(h->nodes[i].record);
    h->claim_count = mark.claims; h->count = mark.nodes;
    /* The tables index the arrays: rebuilt over what stays. */
    if (h->index) memset(h->index, 0, h->index_capacity * sizeof(*h->index));
    if (h->names) memset(h->names, 0, h->table_capacity * sizeof(*h->names));
    if (h->ids) memset(h->ids, 0, h->table_capacity * sizeof(*h->ids));
    for (size_t i = 0; h->index && i < h->count; i++) h->index[pl_history_node_slot(h, h->nodes[i].identity)] = i + 1;
    for (size_t i = 0; h->names && h->ids && i < h->claim_count; i++) {
        const pl_history_claim *claim = h->claims + i;
        h->names[pl_history_claim_slot(h, claim->family, claim->name, claim->id, 0)] = i + 1;
        h->ids[pl_history_claim_slot(h, claim->family, claim->name, claim->id, 1)] = i + 1;
    }
    return 1;
}
static size_t pl_history_node_slot(const sh_package_link_history *h, const unsigned char identity[32])
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < 32; i++) hash = (hash ^ identity[i]) * UINT64_C(1099511628211);
    size_t slot = (size_t)hash & (h->index_capacity - 1);
    while (h->index[slot] && memcmp(h->nodes[h->index[slot] - 1].identity, identity, 32))
        slot = (slot + 1) & (h->index_capacity - 1);
    return slot;
}
static size_t pl_history_claim_slot(const sh_package_link_history *h, const char *family,
    const char *name, uint32_t id, int numeric)
{
    uint64_t hash = pl_name_hash(family, numeric ? "" : name);
    if (numeric) hash = (hash ^ id) * UINT64_C(1099511628211);
    size_t slot = (size_t)hash & (h->table_capacity - 1);
    const size_t *table = numeric ? h->ids : h->names;
    while (table[slot]) {
        const pl_history_claim *c = h->claims + table[slot] - 1;
        if (!strcmp(c->family, family) && (numeric ? c->id == id : !strcmp(c->name, name))) break;
        slot = (slot + 1) & (h->table_capacity - 1);
    }
    return slot;
}
static int pl_history_claim_add(sh_package_link_history *h, const char *family,
    const char *name, uint32_t id, size_t node)
{
    size_t named = pl_history_claim_slot(h, family, name, id, 0);
    size_t numeric = pl_history_claim_slot(h, family, name, id, 1);
    if (h->names[named]) {
        const pl_history_claim *c = h->claims + h->names[named] - 1;
        return c->node == node && c->id == id;
    }
    if (h->ids[numeric] || h->claim_count >= h->claim_capacity) return 0;
    pl_history_claim c = {_strdup(family), _strdup(name), id, node};
    if (!c.family || !c.name) { free(c.family); free(c.name); return 0; }
    h->claims[h->claim_count++] = c;
    h->names[named] = h->ids[numeric] = h->claim_count;
    return 1;
}
/* An empty history's tables, sized for nodes and claims (at least one each). */
static int pl_history_reserve(sh_package_link_history *h, size_t nodes, size_t claims)
{
    h->capacity = nodes; h->claim_capacity = claims;
    h->index_capacity = pl_table_capacity(nodes ? nodes : 1); h->table_capacity = pl_table_capacity(claims ? claims : 1);
    if (!h->index_capacity || !h->table_capacity) return 0;
    h->nodes = pl_alloc(nodes ? nodes : 1, sizeof(*h->nodes));
    h->claims = pl_alloc(claims ? claims : 1, sizeof(*h->claims));
    h->index = pl_alloc(h->index_capacity, sizeof(*h->index));
    h->names = pl_alloc(h->table_capacity, sizeof(*h->names));
    h->ids = pl_alloc(h->table_capacity, sizeof(*h->ids));
    return h->nodes && h->claims && h->index && h->names && h->ids;
}
static sh_package_link_history *pl_history_copy(const sh_package_link_history *old,
    size_t nodes, size_t claims)
{
    sh_package_link_history *h = sh_package_link_history_open();
    if (!h || nodes > SIZE_MAX - old->count || claims > SIZE_MAX - old->claim_count) goto failed;
    if (!pl_history_reserve(h, old->count + nodes, old->claim_count + claims)) goto failed;
    for (size_t i = 0; i < old->count; i++) {
        h->nodes[i] = old->nodes[i]; h->nodes[i].record = pl_record_retain(old->nodes[i].record);
        h->count++; h->index[pl_history_node_slot(h, h->nodes[i].identity)] = i + 1;
    }
    for (size_t i = 0; i < old->claim_count; i++) {
        const pl_history_claim *c = old->claims + i;
        if (!pl_history_claim_add(h, c->family, c->name, c->id, c->node)) goto failed;
    }
    return h;
failed:
    sh_package_link_history_close(h); return NULL;
}
/* ---- Export and staged import (R-344 stage B) ----
 * A warm start rebuilds the history a cold start's library link leaves: the same nodes with their records
 * and the same claims, in order. The records are data here: the caller binds them to inputs it validated
 * in this process and re-checks every claim against the native registry before committing the history. */
int sh_package_link_history_node(const sh_package_link_history *h, size_t index, unsigned char identity[32],
    uint64_t *length)
{
    if (!h || index >= h->count || !h->nodes[index].record) return 0;
    memcpy(identity, h->nodes[index].identity, 32); *length = h->nodes[index].record->length; return 1;
}
int sh_package_link_history_record(const sh_package_link_history *h, size_t index,
    int (*write)(void *, const void *, size_t), void *sink, char *error, size_t capacity)
{
    if (!h || index >= h->count || !h->nodes[index].record || !write) return 0;
    const pl_record *record = h->nodes[index].record;
    for (size_t at = 0; at < record->length;) {
        size_t n = record->length - at < 65536 ? record->length - at : 65536;
        if (!write(sink, record->bytes + at, n)) {
            if (error && capacity && !*error) snprintf(error, capacity, "cannot export a native binding record");
            return 0;
        }
        at += n;
    }
    return 1;
}
size_t sh_package_link_history_claims(const sh_package_link_history *h) { return h ? h->claim_count : 0; }
int sh_package_link_history_claim(const sh_package_link_history *h, size_t index,
    const char **family, const char **name, uint32_t *id, size_t *node)
{
    if (!h || index >= h->claim_count) return 0;
    *family = h->claims[index].family; *name = h->claims[index].name;
    *id = h->claims[index].id; *node = h->claims[index].node; return 1;
}
int sh_package_link_history_import(sh_package_link_history *h, const unsigned char (*identities)[32],
    const uint64_t *lengths, size_t count, const unsigned char *records, uint64_t record_length,
    const sh_package_link_history_row *claims, size_t claim_count, char *error, size_t capacity)
{
    uint64_t total = 0;
    if (error && capacity) *error = 0;
    if (!h || h->count || h->claim_count || (count && (!identities || !lengths)) || (claim_count && !claims)) {
        if (error && capacity) snprintf(error, capacity, "a native binding history imports only into an empty history");
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (lengths[i] > UINT64_MAX - total || lengths[i] > SIZE_MAX) goto malformed;
        total += lengths[i];
    }
    if (total != record_length || (record_length && !records)) goto malformed;
    if (!count) return claim_count == 0 ? 1 : (snprintf(error, capacity, "native binding claims name no record"), 0);
    if (!pl_history_reserve(h, count, claim_count)) goto memory;
    uint64_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        size_t slot = pl_history_node_slot(h, identities[i]);
        if (h->index[slot]) goto malformed; /* one node per binding fingerprint */
        pl_history_node *node = h->nodes + h->count;
        memcpy(node->identity, identities[i], 32);
        node->record = pl_record_new(records + offset, (size_t)lengths[i]); offset += lengths[i];
        if (!node->record) goto memory;
        h->index[slot] = ++h->count;
    }
    for (size_t i = 0; i < claim_count; i++)
        if (!claims[i].family || !*claims[i].family || !claims[i].name || !*claims[i].name || claims[i].node >= h->count ||
            !pl_history_claim_add(h, claims[i].family, claims[i].name, claims[i].id, claims[i].node) ||
            h->claim_count != i + 1) goto malformed;
    return 1;
malformed:
    if (error && capacity && !*error) snprintf(error, capacity, "native binding history record is inconsistent");
    goto failed;
memory:
    if (error && capacity && !*error) snprintf(error, capacity, "cannot retain native binding history");
failed:
    for (size_t i = 0; i < h->count; i++) pl_record_release(h->nodes[i].record);
    for (size_t i = 0; i < h->claim_count; i++) { free(h->claims[i].family); free(h->claims[i].name); }
    free(h->nodes); free(h->claims); free(h->index); free(h->names); free(h->ids);
    h->nodes = NULL; h->claims = NULL; h->index = h->names = h->ids = NULL;
    h->count = h->claim_count = h->capacity = h->claim_capacity = h->index_capacity = h->table_capacity = 0;
    return 0;
}
/* The record of one class representative, composed into a reusable buffer. */
typedef struct pl_record_buffer { unsigned char *bytes; size_t length, capacity; } pl_record_buffer;
static int pl_record_put(pl_record_buffer *b, const void *bytes, size_t length)
{
    if (length > b->capacity - b->length) {
        size_t wanted = b->length + length, capacity = b->capacity ? b->capacity : 4096;
        if (wanted < b->length) return 0;
        while (capacity < wanted) { if (capacity > SIZE_MAX / 2) { capacity = wanted; break; } capacity *= 2; }
        unsigned char *grown = realloc(b->bytes, capacity);
        if (!grown) return 0;
        b->bytes = grown; b->capacity = capacity;
    }
    if (length) memcpy(b->bytes + b->length, bytes, length);
    b->length += length;
    return 1;
}
static int pl_record_u64(pl_record_buffer *b, uint64_t value)
{
    unsigned char bytes[8], *p = bytes; pl_u64(&p, value);
    return pl_record_put(b, bytes, sizeof(bytes));
}
static int pl_record_string(pl_record_buffer *b, const char *value)
{
    size_t length = strlen(value);
    return pl_record_u64(b, length) && pl_record_put(b, value, length);
}
static int pl_record_file(pl_record_buffer *b, const pl_file *file)
{
    return pl_record_string(b, file->resource->engine_path) && pl_record_u64(b, file->identity.length) &&
        pl_record_put(b, file->identity.digest, 32);
}
static int pl_record_compose(pl_work *w, size_t unit, pl_record_buffer *b)
{
    const sh_package_link_unit *u = w->units + unit;
    const pl_unit *captured = w->work + unit;
    sh_package_link_edge *edges = pl_alloc(captured->edge_count, sizeof(*edges));
    size_t count = 0; int ok = 0;
    if (!edges) return 0;
    b->length = 0;
    if (!pl_record_string(b, "native-binding-record") || !pl_record_string(b, u->family) ||
        !pl_record_string(b, u->name) || !pl_record_put(b, u->semantics, 32) ||
        !pl_record_u64(b, u->fixed_name != 0) ||
        !pl_record_u64(b, u->file_count) || !pl_record_u64(b, u->witness_count)) goto done;
    for (size_t i = 0; i < u->file_count; i++) {
        const pl_file *file = captured->files + i;
        if (!pl_record_string(b, file->family) || !pl_record_string(b, file->name) || !pl_record_file(b, file)) goto done;
    }
    for (size_t i = 0; i < u->witness_count; i++)
        if (!pl_record_string(b, captured->witness_order[i]->role) || !pl_record_file(b, captured->witnesses + i)) goto done;
    for (size_t i = 0; i < captured->edge_count; i++)
        if (!w->units[captured->edges[i].target].stable_name) edges[count++] = captured->edges[i];
    if (count > 1) qsort(edges, count, sizeof(*edges), pl_implicit_order);
    if (!pl_record_u64(b, count)) goto done;
    for (size_t i = 0; i < count; i++) {
        size_t target = w->output.classes.nodes[edges[i].target];
        if (!pl_record_string(b, edges[i].slot) ||
            !pl_record_put(b, w->output.classes.identities[target], 32)) goto done;
    }
    ok = 1;
done:
    free(edges); return ok;
}
/* Every class of this link either matches a held node's record exactly or is
 * recorded as a new node. The record is the identity's own derivation, so a
 * held node with another record means two bindings share one fingerprint. */
static int pl_history_prepare(pl_work *w)
{
    const sh_package_link_history *old = w->adapter->history;
    if (!old) return 1;
    size_t claims = w->output.classes.count;
    for (size_t i = 0; i < w->output.classes.count; i++) {
        size_t unit = w->output.classes.representatives[i];
        if (w->units[unit].file_count > SIZE_MAX - claims) return pl_fail(w, "native reservations exceed addressable size");
        claims += w->units[unit].file_count;
    }
    w->history = pl_history_copy(old, w->output.classes.count, claims);
    if (!w->history) return pl_fail(w, "cannot retain native binding history");
    sh_package_link_history *h = w->history;
    pl_record_buffer buffer = {0}; int ok = 0;
    for (size_t i = 0; i < w->output.classes.count; i++) {
        const unsigned char *identity = w->output.classes.identities[i];
        size_t slot = pl_history_node_slot(h, identity), held = h->index[slot];
        if (!pl_record_compose(w, w->output.classes.representatives[i], &buffer)) {
            pl_fail(w, "cannot compose the native binding record"); goto done;
        }
        if (held) {
            const pl_record *record = h->nodes[held - 1].record;
            if (!record || record->length != buffer.length || memcmp(record->bytes, buffer.bytes, buffer.length)) {
                pl_fail(w, "distinct bindings share a native binding fingerprint"); goto done;
            }
        } else {
            pl_history_node *node = h->nodes + h->count;
            memcpy(node->identity, identity, 32);
            node->record = pl_record_new(buffer.bytes, buffer.length);
            if (!node->record) { pl_fail(w, "cannot retain the native binding record"); goto done; }
            h->index[slot] = ++h->count;
        }
    }
    ok = 1;
done:
    free(buffer.bytes); return ok;
}
/* 1 is an exact previously reserved name, 0 is free, -1 is a different binding
 * or numeric identity. This covers issued but not yet materialized resources. */
static int pl_history_identity(pl_work *w, const pl_native_identity *identity, size_t group)
{
    const sh_package_link_history *h = w->history;
    if (!h) return 0;
    size_t node = h->index[pl_history_node_slot(h, w->output.classes.identities[group])] - 1;
    size_t named = h->names[pl_history_claim_slot(h, identity->family, identity->name, identity->id, 0)];
    size_t numeric = h->ids[pl_history_claim_slot(h, identity->family, identity->name, identity->id, 1)];
    if (!named && !numeric) return 0;
    return named && named == numeric && h->claims[named - 1].node == node ? 1 : -1;
}

/* Developer report of a refused native identity (adapter note, bounded): both
 * claimants with their classes, certification, assigned names, payload
 * identities and bound dependencies, the dependencies that differ between
 * them marked. It changes nothing the link decides. */
static void pl_hex8(const unsigned char *bytes, char out[17])
{ for (size_t i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", bytes[i]); }
static void pl_collision_unit(pl_work *w, size_t u, const char *role, char *line, size_t capacity)
{
    const sh_package_link_adapter *a = w->adapter;
    const sh_package_link_classes *classes = &w->output.classes;
    const sh_package_link_unit *unit = w->units + u;
    size_t group = classes->nodes[u];
    char meta[640] = "", content[17], semantics[17], identity[17];
    if (a->describe) a->describe(a->context, u, meta, sizeof(meta));
    pl_hex8(classes->identities[group], identity);
    pl_hex8(w->nodes[u].content, content); pl_hex8(w->nodes[u].semantics, semantics);
    snprintf(line, capacity, "collision %s: unit %zu %s %s: linker class %zu (%s..., %zu members), fixed %d, "
        "stable %d, original bytes %d, certified original %d, assigned %s; content %s... semantics %s...",
        role, u, unit->family, unit->name, group, identity, w->member_offsets[group + 1] - w->member_offsets[group],
        unit->fixed_name, unit->stable_name, unit->original_binding, w->originals ? w->originals[u] : -1,
        w->output.names && w->output.names[group] ? w->output.names[group] : "(none yet)", content, semantics);
    a->note(a->context, line);
    if (*meta) { snprintf(line, capacity, "collision %s source: %s", role, meta); a->note(a->context, line); }
    for (size_t f = 0; f < unit->file_count && f < 4; f++) {
        const pl_file *file = w->work[u].files + f;
        char digest[17]; pl_hex8(file->identity.digest, digest);
        snprintf(line, capacity, "collision %s file %s: %llu bytes, identity %s..., path %s", role,
            file->resource->engine_path ? file->resource->engine_path : "(none)",
            (unsigned long long)file->identity.length, digest,
            w->work[u].paths && w->work[u].paths[f] ? w->work[u].paths[f] : "(unassigned)");
        a->note(a->context, line);
    }
    size_t used = 0, header;
    int n = snprintf(line, capacity, "collision %s edges (%zu):", role, w->work[u].edge_count);
    header = used = n > 0 ? (size_t)n : 0;
    for (size_t e = 0; e < w->work[u].edge_count && e < 48 && used < capacity; e++) {
        const sh_package_link_edge *edge = w->work[u].edges + e;
        size_t t = edge->target;
        if (t >= w->count) continue;
        n = snprintf(line + used, capacity - used, " [%s -> %zu %s %s class %zu%s%s%s]",
            edge->slot ? edge->slot : "?", t, w->units[t].family, w->units[t].name, classes->nodes[t],
            w->units[t].stable_name ? " stable" : "", w->units[t].fixed_name ? " fixed" : "",
            w->originals && w->originals[t] ? " original" : "");
        if (n < 0 || (size_t)n >= capacity - used) {
            line[used] = 0;
            if (used == header) continue; /* An entry longer than a line is skipped. */
            a->note(a->context, line);
            n = snprintf(line, capacity, "collision %s edges (cont.):", role);
            header = used = n > 0 ? (size_t)n : 0; e--; continue;
        }
        used += (size_t)n;
    }
    a->note(a->context, line);
}
/* One pair: both units, the dependencies that differ between them, the first
 * differing payload bytes; then, two levels down, the first differing
 * dependency pairs themselves, so the report reaches the root difference. */
static void pl_collision_pair(pl_work *w, size_t index, size_t other, int depth, char *line, size_t capacity)
{
    const sh_package_link_adapter *a = w->adapter;
    const sh_package_link_classes *classes = &w->output.classes;
    char role_x[32], role_y[32];
    snprintf(role_x, sizeof(role_x), depth ? "claimant dep%d" : "claimant", depth);
    snprintf(role_y, sizeof(role_y), depth ? "holder dep%d" : "holder", depth);
    pl_collision_unit(w, index, role_x, line, capacity);
    if (other >= w->count) { a->note(a->context, "collision holder: none recorded"); return; }
    pl_collision_unit(w, other, role_y, line, capacity);
    /* The bound dependencies that differ: the same slot reaching another class. */
    const pl_unit *x = w->work + index, *y = w->work + other;
    size_t shown = 0, used = 0, next[2][2], found = 0, wanted = depth ? 1 : 2;
    int n = snprintf(line, capacity, "collision %s differs (%zu vs %zu edges):", role_x, x->edge_count, y->edge_count);
    used = n > 0 ? (size_t)n : 0;
    for (size_t e = 0; e < x->edge_count && e < y->edge_count && shown < 24; e++) {
        const sh_package_link_edge *p = x->edges + e, *q = y->edges + e;
        int same_slot = p->slot && q->slot && !strcmp(p->slot, q->slot);
        int valid = p->target < w->count && q->target < w->count;
        if (same_slot && valid && classes->nodes[p->target] == classes->nodes[q->target]) continue;
        if (valid && found < wanted && p->target != index && q->target != other) {
            next[found][0] = p->target; next[found][1] = q->target; found++;
        }
        n = snprintf(line + used, capacity - used, " [%s: %zu %s class %zu%s | %s: %zu %s class %zu%s]",
            p->slot ? p->slot : "?", p->target, p->target < w->count ? w->units[p->target].name : "?",
            p->target < w->count ? classes->nodes[p->target] : SIZE_MAX,
            p->target < w->count && w->units[p->target].stable_name ? " stable" : "",
            q->slot ? q->slot : "?", q->target, q->target < w->count ? w->units[q->target].name : "?",
            q->target < w->count ? classes->nodes[q->target] : SIZE_MAX,
            q->target < w->count && w->units[q->target].stable_name ? " stable" : "");
        if (n < 0 || (size_t)n >= capacity - used) { line[used] = 0; break; }
        used += (size_t)n; shown++;
    }
    a->note(a->context, line);
    /* The payloads where both are held in memory (a detached original's read again): the first differing byte. */
    if (w->units[index].file_count && w->units[other].file_count) {
        const sh_compiled_resource *p = w->work[index].files[0].resource, *q = w->work[other].files[0].resource;
        size_t ignored = 0;
        unsigned char *p_read = !p->body && p->body_owner && (q->body || q->body_owner) ?
            sh_package_body_read(p->body_owner, &ignored) : NULL;
        unsigned char *q_read = !q->body && q->body_owner && (p->body || p_read) ?
            sh_package_body_read(q->body_owner, &ignored) : NULL;
        const unsigned char *p_body = p->body ? p->body : p_read, *q_body = q->body ? q->body : q_read;
        if (p_body && q_body) {
            size_t at = 0, shorter = p->body_length < q->body_length ? p->body_length : q->body_length;
            while (at < shorter && p_body[at] == q_body[at]) at++;
            char mine[97], theirs[97];
            size_t from = at > 32 ? at - 32 : 0;
            for (size_t k = 0; k < 96; k++) {
                unsigned char c1 = from + k < p->body_length ? p_body[from + k] : ' ';
                unsigned char c2 = from + k < q->body_length ? q_body[from + k] : ' ';
                mine[k] = c1 >= 32 && c1 < 127 ? (char)c1 : '.'; theirs[k] = c2 >= 32 && c2 < 127 ? (char)c2 : '.';
            }
            mine[96] = theirs[96] = 0;
            snprintf(line, capacity, "collision %s payloads: %zu vs %zu bytes, %s at %zu: [%s] vs [%s]", role_x,
                p->body_length, q->body_length, at == shorter && p->body_length == q->body_length ? "equal" :
                "first difference", at, mine, theirs);
            a->note(a->context, line);
        }
        free(p_read); free(q_read);
    }
    for (size_t k = 0; depth < 2 && k < found; k++) pl_collision_pair(w, next[k][0], next[k][1], depth + 1, line, capacity);
}
static void pl_collision_note(pl_work *w, size_t index, size_t other)
{
    char line[960]; /* One developer-log line each. */
    if (!w->adapter->note || index >= w->count) return;
    pl_collision_pair(w, index, other, 0, line, sizeof(line));
}

static int pl_assign_one(pl_work *w, pl_registry *registry, size_t group)
{
    const sh_package_link_registry *native = w->adapter->registry;
    sh_package_link_classes *classes = &w->output.classes;
    size_t index = classes->representatives[group], count = w->units[index].file_count;
    pl_unit *unit = &w->work[index];
    size_t derivable = w->adapter->derive && !w->originals[index] ? unit->implicit_count : 0;
    const sh_compiled_resource **files = pl_alloc(count, sizeof(*files));
    pl_native_identity *identities = count < SIZE_MAX - 1 - derivable ?
        pl_alloc(count + 1 + derivable, sizeof(*identities)) : NULL;
    int ok = 0;
    if (!files || !identities) { pl_fail(w, "cannot allocate native path assignment"); goto done; }
    for (size_t i = 0; i < count; i++) files[i] = unit->files[i].resource;
    /* A changed carried class is served only where its owners derive it: it
     * keeps its own spelling as a label and reserves no path or identity. */
    if (w->carried[index]) {
        w->output.names[group] = _strdup(w->units[index].name);
        unit->paths = pl_alloc(count, sizeof(*unit->paths));
        if (!w->output.names[group] || !unit->paths) { pl_fail(w, "cannot allocate native path assignment"); goto done; }
        for (size_t i = 0; i < count; i++) if (!(unit->paths[i] = _strdup(files[i]->engine_path))) {
            pl_fail(w, "cannot allocate native path assignment"); goto done;
        }
        ok = 1; goto done;
    }
    char cause[384] = "";
    size_t holder = SIZE_MAX; /* Diagnostics: the unit holding the first collision. */
    for (size_t attempt = 0; attempt < 1024; attempt++) {
        unsigned char token[32];
        size_t identity_count = 0;
        int collision = 0;
        cause[0] = 0; holder = SIZE_MAX;
        *w->error = 0;
        if (!pl_name_token(classes->identities[group], attempt, token) ||
            !w->adapter->assign(w->adapter->context, index, token, w->originals[index],
                files, count, &w->output.names[group], &unit->paths, w->error, w->capacity) ||
            !w->output.names[group] || !*w->output.names[group] || (count && !unit->paths) ||
            ((!count || w->units[index].fixed_name) && strcmp(w->output.names[group], w->units[index].name))) {
            pl_fail(w, "native resource identity cannot be assigned safely"); goto done;
        }
        for (size_t i = 0; i < count; i++) if (!unit->paths[i] || !*unit->paths[i]) {
            pl_fail(w, "native resource identity is missing a physical path"); goto done;
        }
        if (!pl_assign_members(w, index, group, files, identities, &identity_count)) goto done;
        if (derivable) {
            unit->derived_targets = pl_alloc(derivable, sizeof(*unit->derived_targets));
            unit->derived_slots = pl_alloc(derivable, sizeof(*unit->derived_slots));
            unit->derived_paths = pl_alloc(derivable, sizeof(*unit->derived_paths));
            unit->derived_names = pl_alloc(derivable, sizeof(*unit->derived_names));
            unit->derived_alias = pl_alloc(derivable, sizeof(*unit->derived_alias));
            if (!unit->derived_targets || !unit->derived_slots || !unit->derived_paths || !unit->derived_names ||
                !unit->derived_alias) {
                pl_fail(w, "cannot allocate derived resource publication"); goto done;
            }
        }
        for (size_t e = 0; e < derivable; e++) {
            char *path = NULL, *name = NULL;
            size_t target = unit->implicit[e].target;
            int status = w->adapter->derive(w->adapter->context, index, w->output.names[group],
                unit->implicit[e].slot, target, &path, &name, w->error, w->capacity);
            if ((status != 1 && status != 2) || !path || !*path || !name || !*name) {
                free(path); free(name);
                if (!status) continue;
                pl_fail(w, "derived resource publication cannot be assigned safely"); goto done;
            }
            if (w->units[target].file_count != 1) {
                free(path); free(name);
                pl_fail(w, "derived resource publication requires a single-file target"); goto done;
            }
            size_t k = unit->derived_count++;
            unit->derived_targets[k] = target; unit->derived_slots[k] = unit->implicit[e].slot;
            unit->derived_paths[k] = path; unit->derived_names[k] = name; unit->derived_alias[k] = status == 2;
            identities[identity_count++] = (pl_native_identity){w->work[target].files[0].family, name,
                w->work[target].files[0].name, 0, 0, 0, index};
        }
        for (size_t i = 0; i < count; i++) {
            size_t slot = pl_path_slot(registry, unit->paths[i]);
            if (registry->paths[slot]) {
                size_t other = registry->path_units[slot], file = registry->path_files[slot];
                int equal = 0;
                if (w->originals[index] && w->originals[other] && file != SIZE_MAX &&
                    !strcmp(unit->files[i].resource->engine_path, w->work[other].files[file].resource->engine_path)) {
                    equal = pl_original_file_equal(w, index, i, other, file);
                    if (equal < 0) goto done;
                }
                if (!equal && !collision++) {
                    snprintf(cause, sizeof(cause), "path %s is already bound by %s %s",
                        unit->paths[i], w->units[other].family, w->units[other].name);
                    holder = other;
                }
            }
            for (size_t j = 0; j < i; j++) if (!strcmp(unit->paths[i], unit->paths[j]) && !collision++)
                snprintf(cause, sizeof(cause), "path %s is assigned twice", unit->paths[i]);
        }
        for (size_t d = 0; d < unit->derived_count; d++) {
            const char *path = unit->derived_paths[d];
            size_t slot = pl_path_slot(registry, path);
            if (registry->paths[slot] && !collision++) {
                snprintf(cause, sizeof(cause), "derived path %s is already bound by %s %s",
                    path, w->units[registry->path_units[slot]].family, w->units[registry->path_units[slot]].name);
                holder = registry->path_units[slot];
            }
            for (size_t i = 0; i < count; i++) if (!strcmp(path, unit->paths[i]) && !collision++)
                snprintf(cause, sizeof(cause), "derived path %s is also an assigned path", path);
            for (size_t j = 0; j < d; j++) if (!strcmp(path, unit->derived_paths[j]) && !collision++)
                snprintf(cause, sizeof(cause), "derived path %s is assigned twice", path);
        }
        for (size_t i = 0; i < identity_count; i++) {
            pl_native_identity *identity = identities + i;
            size_t occupied = registry->names[pl_name_slot(registry, identity, 0)];
            if (occupied) {
                const pl_native_identity *held = registry->identities + occupied - 1;
                int shared = pl_same_original(w, identity, held);
                if (shared < 0) goto done;
                if (!shared && !collision++) {
                    snprintf(cause, sizeof(cause), "%s %s is already held by %s %s",
                        identity->family, identity->name, w->units[held->unit].family, w->units[held->unit].name);
                    holder = held->unit;
                }
            }
            if (native) {
                int state = native->check(native->context, identity->family, identity->name,
                    classes->identities[group], w->originals[index], &identity->id, w->error, w->capacity);
                if (state < SH_PACKAGE_LINK_REGISTRY_COLLISION ||
                    state > SH_PACKAGE_LINK_REGISTRY_OCCUPIED || !identity->id) {
                    pl_fail(w, "native resource registry verification is incomplete"); goto done;
                }
                if (state == SH_PACKAGE_LINK_REGISTRY_REPLACE_SHARED &&
                    (w->originals[index] || !count || strcmp(identity->name, identity->original_name))) {
                    pl_fail(w, "native shared replacement requires a changed same-name binding"); goto done;
                }
                occupied = registry->ids[pl_name_slot(registry, identity, 1)];
                int retained = pl_history_identity(w, identity, group);
                if ((state == SH_PACKAGE_LINK_REGISTRY_COLLISION || retained < 0 ||
                    (state == SH_PACKAGE_LINK_REGISTRY_OCCUPIED && retained != 1)) && !collision++)
                    snprintf(cause, sizeof(cause), "the native registry holds %s %s differently", identity->family, identity->name);
                if (occupied) {
                    const pl_native_identity *held = registry->identities + occupied - 1;
                    int shared = pl_same_original(w, identity, held);
                    if (shared < 0) goto done;
                    if (!shared && !collision++) {
                        snprintf(cause, sizeof(cause), "native id of %s %s is already held by %s %s",
                            identity->family, identity->name, w->units[held->unit].family, w->units[held->unit].name);
                        holder = held->unit;
                    }
                }
            }
            /* Candidates in this group are not in the shared index yet. */
            for (size_t j = 0; j < i; j++) if (!strcmp(identity->family, identities[j].family) &&
                (!strcmp(identity->name, identities[j].name) || (native && identity->id == identities[j].id)) && !collision++)
                snprintf(cause, sizeof(cause), "members share %s %s", identity->family, identity->name);
        }
        if (!collision) {
            for (size_t i = 0; w->history && native && i < identity_count; i++) {
                const pl_native_identity *identity = identities + i;
                if (w->originals[index] || !strcmp(identity->name, identity->original_name)) continue;
                size_t node = w->history->index[pl_history_node_slot(w->history, classes->identities[group])] - 1;
                if (!pl_history_claim_add(w->history, identity->family, identity->name, identity->id, node)) {
                    pl_fail(w, "cannot retain private native identity reservation"); goto done;
                }
            }
            for (size_t i = 0; i < identity_count; i++) {
                size_t name_slot = pl_name_slot(registry, identities + i, 0);
                if (registry->names[name_slot]) continue; /* Certified original shared by groups. */
                size_t key = ++registry->identity_count;
                registry->identities[key - 1] = identities[i]; registry->names[name_slot] = key;
                if (native) registry->ids[pl_name_slot(registry, identities + i, 1)] = key;
            }
            for (size_t i = 0; i < count; i++) {
                size_t slot = pl_path_slot(registry, unit->paths[i]);
                if (!registry->paths[slot]) {
                    registry->paths[slot] = unit->paths[i]; registry->path_units[slot] = index; registry->path_files[slot] = i;
                }
            }
            for (size_t d = 0; d < unit->derived_count; d++) {
                size_t slot = pl_path_slot(registry, unit->derived_paths[d]);
                registry->paths[slot] = unit->derived_paths[d]; registry->path_units[slot] = index;
                registry->path_files[slot] = SIZE_MAX;
            }
            *w->error = 0; ok = 1; break;
        }
        if (w->originals[index] || !count) {
            snprintf(w->error, w->capacity, "certified original resource has a native identity collision: %s %s: %s",
                w->units[index].family, w->units[index].name, cause);
            pl_collision_note(w, index, holder);
            w->failed = 1; goto done;
        }
        pl_assignment_clear(w, group);
    }
    if (!ok) {
        snprintf(w->error, w->capacity, "native identity collision allocation exhausted: %s %s: %s",
            w->units[index].family, w->units[index].name, cause);
        pl_collision_note(w, index, holder);
        w->failed = 1;
    }
done:
    free(files); free(identities); return ok;
}

/* The graph as dependents see it: a stable-name unit is reached by its one
 * native name in every scope (sh_package_link_unit.stable_name), so neither
 * its per-scope content nor its changed bytes enter a dependent's identity or
 * original certification; the dependent's own bytes carry the name. The unit
 * keeps its own edges. NULL on allocation failure. */
/* The nodes with every edge to a stable-named unit left out, for one walk: w->nodes itself, where only a node
 * with such an edge reads a filtered copy of its edges until pl_stable_view_end restores it. NULL when the copies
 * cannot be allocated (nothing changed). */
static sh_package_link_node *pl_stable_view(pl_work *w, pl_view *held)
{
    size_t affected = 0, kept = 0, at = 0;
    int stable = 0;
    memset(held, 0, sizeof(*held));
    for (size_t i = 0; i < w->count; i++) stable |= w->units[i].stable_name != 0;
    if (!stable) return w->nodes;
    for (size_t i = 0; i < w->count; i++) {
        size_t dropped = 0;
        for (size_t e = 0; e < w->nodes[i].edge_count; e++)
            dropped += w->units[w->nodes[i].edges[e].target].stable_name != 0;
        if (dropped) { affected++; kept += w->nodes[i].edge_count - dropped; }
    }
    if (!affected) return w->nodes;
    held->saved = pl_alloc(affected, sizeof(*held->saved));
    held->kept = pl_alloc(kept, sizeof(*held->kept));
    if (!held->saved || !held->kept) { free(held->saved); free(held->kept); memset(held, 0, sizeof(*held)); return NULL; }
    for (size_t i = 0; i < w->count; i++) {
        sh_package_link_node *node = &w->nodes[i];
        size_t e = 0;
        while (e < node->edge_count && !w->units[node->edges[e].target].stable_name) e++;
        if (e == node->edge_count) continue;
        held->saved[held->saved_count++] = (struct pl_view_saved){i, node->edges, node->edge_count, node->slot_order};
        size_t begin = at;
        for (e = 0; e < node->edge_count; e++)
            if (!w->units[node->edges[e].target].stable_name) held->kept[at++] = node->edges[e];
        node->edges = held->kept + begin; node->edge_count = at - begin; node->slot_order = NULL;
    }
    return w->nodes;
}
static void pl_stable_view_end(pl_work *w, pl_view *held)
{
    for (size_t k = 0; k < held->saved_count; k++) {
        w->nodes[held->saved[k].node].edges = held->saved[k].edges;
        w->nodes[held->saved[k].node].edge_count = held->saved[k].edge_count;
        w->nodes[held->saved[k].node].slot_order = held->saved[k].order;
    }
    free(held->saved); free(held->kept); memset(held, 0, sizeof(*held));
}

static int pl_assign(pl_work *w)
{
    sh_package_link_classes *classes = &w->output.classes;
    pl_assignment *order = NULL;
    pl_registry registry = {0};
    size_t path_count = 0, identity_count = classes->count;
    int ok = 0;
    w->output.names = pl_alloc(classes->count, sizeof(*w->output.names));
    if (!w->output.names) return pl_fail(w, "cannot allocate linked resource identities");
    w->originals = pl_alloc(w->count, sizeof(*w->originals));
    w->carried = pl_alloc(w->count, sizeof(*w->carried));
    if (!w->originals || !w->carried) return pl_fail(w, "cannot allocate original binding certification");
    for (size_t c = 0; c < classes->count; c++) {
        unsigned char original = 1;
        for (size_t i = w->member_offsets[c]; i < w->member_offsets[c + 1]; i++)
            if (!w->units[w->members[i]].original_binding) original = 0;
        for (size_t i = w->member_offsets[c]; i < w->member_offsets[c + 1]; i++)
            w->originals[w->members[i]] = original;
    }
    /* Campaign-only data a scope supplies (original_binding 2) keeps the name
     * the renderer or audio derives from it, but it is not SnapMap's: it never
     * makes a dependent original, and keeps its own name only while every
     * dependency it binds keeps its name too. */
    unsigned char *own = pl_alloc(w->count, 1);
    pl_view held = {0};
    sh_package_link_node *view = own ? pl_stable_view(w, &held) : NULL;
    if (!own || !view) { free(own); return pl_fail(w, "cannot allocate original binding certification"); }
    memcpy(own, w->originals, w->count);
    for (size_t i = 0; i < w->count; i++) if (w->units[i].original_binding == 2) w->originals[i] = 0;
    if (!sh_package_link_originals(view, w->count, w->originals, w->originals, w->error, w->capacity)) {
        pl_stable_view_end(w, &held);
        free(own); return 0;
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < w->count; i++) {
            if (w->units[i].original_binding != 2 || !own[i]) continue;
            int kept = 1;
            for (size_t e = 0; kept && e < view[i].edge_count; e++) {
                size_t target = view[i].edges[e].target;
                kept = w->units[target].original_binding == 2 ? own[target] : w->originals[target];
            }
            if (!kept) { own[i] = 0; changed = 1; }
        }
    }
    for (size_t i = 0; i < w->count; i++) if (w->units[i].original_binding == 2) w->originals[i] = own[i];
    pl_stable_view_end(w, &held);
    free(own);
    /* A class is carried when every unit of it is; a certified original one
     * keeps its native name and path like any original. */
    for (size_t c = 0; c < classes->count; c++) {
        unsigned char carried = !w->originals[classes->representatives[c]] && w->adapter->derive;
        for (size_t i = w->member_offsets[c]; carried && i < w->member_offsets[c + 1]; i++)
            if (!w->units[w->members[i]].carried) carried = 0;
        for (size_t i = w->member_offsets[c]; i < w->member_offsets[c + 1]; i++) w->carried[w->members[i]] = carried;
    }
    order = pl_alloc(classes->count, sizeof(*order));
    if (!order) goto allocation;
    for (size_t c = 0; c < classes->count; c++) {
        size_t index = classes->representatives[c];
        if (w->units[index].file_count > SIZE_MAX - path_count) goto allocation;
        path_count += w->units[index].file_count;
        if (w->adapter->derive) {
            size_t derivable = w->work[index].implicit_count;
            if (derivable > SIZE_MAX - path_count || derivable > SIZE_MAX - identity_count) goto allocation;
            path_count += derivable; identity_count += derivable;
        }
        for (size_t i = 0; i < w->units[index].file_count; i++) {
            const pl_file *file = w->work[index].files + i;
            if (strcmp(file->family, w->units[index].family) || strcmp(file->name, w->units[index].name)) {
                if (identity_count == SIZE_MAX) goto allocation;
                identity_count++;
            }
        }
        order[c] = (pl_assignment){w->units[index].family, w->units[index].name,
            classes->identities[c], c, w->originals[index]};
    }
    registry.capacity = pl_table_capacity(identity_count); registry.path_capacity = pl_table_capacity(path_count);
    if (!registry.capacity || !registry.path_capacity) goto allocation;
    registry.names = pl_alloc(registry.capacity, sizeof(*registry.names));
    registry.ids = pl_alloc(registry.capacity, sizeof(*registry.ids));
    registry.paths = pl_alloc(registry.path_capacity, sizeof(*registry.paths));
    registry.path_units = pl_alloc(registry.path_capacity, sizeof(*registry.path_units));
    registry.path_files = pl_alloc(registry.path_capacity, sizeof(*registry.path_files));
    registry.identities = pl_alloc(identity_count, sizeof(*registry.identities));
    if (!registry.names || !registry.ids || !registry.paths || !registry.path_units || !registry.path_files || !registry.identities) goto allocation;
    if (classes->count > 1) qsort(order, classes->count, sizeof(*order), pl_assignment_order);
    for (size_t i = 1; i < classes->count; i++) if (!pl_assignment_order(order + i - 1, order + i)) {
        pl_fail(w, "distinct resource bindings have the same binding fingerprint"); goto done;
    }
    for (size_t i = 0; i < classes->count; i++) if (!pl_assign_one(w, &registry, order[i].group)) goto done;
    ok = 1; goto done;
allocation:
    pl_fail(w, "cannot allocate native identity index");
done:
    free(order); free(registry.names); free(registry.ids); free(registry.paths); free(registry.identities);
    free(registry.path_units); free(registry.path_files);
    return ok;
}

static int pl_dependency(pl_work *w, sh_compiled_resource *resource, const char *path)
{
    if (!strcmp(resource->engine_path, path)) return 1;
    for (size_t i = 0; i < resource->bound_dependency_count; i++)
        if (!strcmp(resource->bound_dependencies[i], path)) return 1;
    if (resource->bound_dependency_count == SIZE_MAX / sizeof(*resource->bound_dependencies))
        return pl_fail(w, "too many linked dependency paths");
    char *copy = _strdup(path);
    char **grown = copy ? realloc(resource->bound_dependencies,
        (resource->bound_dependency_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return pl_fail(w, "cannot retain linked dependency paths"); }
    resource->bound_dependencies = grown; grown[resource->bound_dependency_count++] = copy;
    return 1;
}

/* What resource retains of the class index's file f: its direct and (for the
 * first file) unit-wide targets, through source-free nodes and changed carried
 * classes, which are published only at derived paths. A target class retains
 * its own derived publications too. */
static int pl_retain(pl_work *w, sh_compiled_resource *resource, size_t index, size_t f,
    size_t *queue, size_t *seen, size_t generation)
{
    const sh_package_link_classes *classes = &w->output.classes;
    const pl_unit *unit = &w->work[index];
    const pl_file *file = unit->files + f;
    size_t queued = 0, visited = 0;
    /* Unit-wide dependencies need one retaining edge per companion
     * cycle. File-local edges retain their original owning member. */
    for (unsigned pass = 0; pass < (f ? 1u : 2u); pass++) {
        size_t begin = pass ? unit->implicit_begin : file->edge_begin;
        size_t end = begin + (pass ? unit->implicit_count : file->edge_count);
        for (size_t e = begin; e < end; e++) {
            size_t target = classes->nodes[unit->edges[e].target];
            if (seen[target] != generation) { seen[target] = generation; queue[queued++] = target; }
        }
    }
    while (visited < queued) {
        size_t target = classes->representatives[queue[visited++]];
        const pl_unit *reached = w->work + target;
        for (size_t d = 0; d < reached->derived_count; d++)
            if (!pl_dependency(w, resource, reached->derived_paths[d])) return 0;
        if (w->units[target].file_count && !w->carried[target]) {
            if (!pl_dependency(w, resource, reached->paths[0])) return 0;
        } else {
            /* An admitted native object may point to supplied data.
             * Preserve that reachability through source-free nodes,
             * including cycles, instead of dropping their edges. A changed
             * carried class is retained where its owner derives it. */
            for (size_t e = 0; e < reached->edge_count; e++) {
                size_t next = classes->nodes[reached->edges[e].target];
                if (seen[next] != generation) { seen[next] = generation; queue[queued++] = next; }
            }
        }
    }
    return 1;
}
static int pl_dependencies(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    size_t *queue = pl_alloc(classes->count, sizeof(*queue));
    size_t *seen = pl_alloc(classes->count, sizeof(*seen)), generation = 0;
    int ok = 0;
    if (!queue || !seen) { pl_fail(w, "cannot retain implicit dependency traversal"); goto done; }
    for (size_t i = 0; i < w->output.compiled->resource_count; i++) {
        sh_compiled_resource *resource = w->output.compiled->resources + i;
        for (size_t j = 0; j < resource->bound_dependency_count; j++) free(resource->bound_dependencies[j]);
        free(resource->bound_dependencies); resource->bound_dependencies = NULL; resource->bound_dependency_count = 0;
    }
    for (size_t c = 0; c < classes->count; c++) {
        size_t index = classes->representatives[c], count = w->units[index].file_count;
        const pl_unit *unit = &w->work[index];
        for (size_t f = 0; !w->carried[index] && f < count; f++) {
            sh_compiled_resource *resource = (sh_compiled_resource *)sh_package_compilation_find(
                w->output.compiled, unit->paths[f]);
            if (!resource) { pl_fail(w, "linked dependency owner is absent"); goto done; }
            /* A resource manager unit can contain multiple physical files.
             * One cycle retains all companions without a quadratic edge set. */
            if (count > 1 && !pl_dependency(w, resource, unit->paths[(f + 1) % count])) goto done;
            if (!++generation) { memset(seen, 0, classes->count * sizeof(*seen)); generation = 1; }
            if (!pl_retain(w, resource, index, f, queue, seen, generation)) goto done;
            for (size_t d = 0; !f && d < unit->derived_count; d++)
                if (!pl_dependency(w, resource, unit->derived_paths[d])) goto done;
        }
        /* Each derived publication retains what its target's own bytes reach;
         * the owner's first file retains the publication itself. */
        for (size_t d = 0; d < unit->derived_count; d++) {
            sh_compiled_resource *resource = (sh_compiled_resource *)sh_package_compilation_find(
                w->output.compiled, unit->derived_paths[d]);
            size_t target = classes->representatives[classes->nodes[unit->derived_targets[d]]];
            if (!resource) { pl_fail(w, "derived resource publication is absent"); goto done; }
            if (!++generation) { memset(seen, 0, classes->count * sizeof(*seen)); generation = 1; }
            if (!pl_retain(w, resource, target, 0, queue, seen, generation)) goto done;
        }
    }
    ok = 1;
done:
    free(queue); free(seen); return ok;
}

static int pl_implicit_output(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    size_t count = 0;
    for (size_t c = 0; c < classes->count; c++) {
        size_t n = w->work[classes->representatives[c]].implicit_count;
        if (n > SIZE_MAX - count) return pl_fail(w, "too many linked implicit bindings");
        count += n;
    }
    if (!count) return 1;
    w->output.implicit = pl_alloc(count, sizeof(*w->output.implicit));
    if (!w->output.implicit) return pl_fail(w, "cannot retain linked implicit bindings");
    for (size_t c = 0; c < classes->count; c++) {
        const pl_unit *unit = w->work + classes->representatives[c];
        for (size_t i = 0; i < unit->implicit_count; i++) {
            sh_package_link_implicit_binding *binding = w->output.implicit + w->output.implicit_count++;
            binding->owner = c; binding->target = classes->nodes[unit->implicit[i].target];
            binding->slot = _strdup(unit->implicit[i].slot);
            if (!binding->slot) return pl_fail(w, "cannot retain linked implicit binding slot");
        }
    }
    return 1;
}

static int pl_derived_output(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    size_t count = 0;
    w->output.carried = pl_alloc(classes->count, sizeof(*w->output.carried));
    w->output.originals = pl_alloc(classes->count, sizeof(*w->output.originals));
    if (!w->output.carried || !w->output.originals) return pl_fail(w, "cannot retain carried resource classes");
    for (size_t c = 0; c < classes->count; c++) {
        size_t n = w->work[classes->representatives[c]].derived_count;
        w->output.carried[c] = w->carried[classes->representatives[c]];
        w->output.originals[c] = w->originals[classes->representatives[c]];
        if (n > SIZE_MAX - count) return pl_fail(w, "too many derived resource publications");
        count += n;
    }
    if (!count) return 1;
    w->output.derived = pl_alloc(count, sizeof(*w->output.derived));
    if (!w->output.derived) return pl_fail(w, "cannot retain derived resource publications");
    for (size_t c = 0; c < classes->count; c++) {
        const pl_unit *owner = w->work + classes->representatives[c];
        for (size_t d = 0; d < owner->derived_count; d++) {
            sh_package_link_derived *out = w->output.derived + w->output.derived_count++;
            out->owner = c; out->target = classes->nodes[owner->derived_targets[d]];
            out->slot = _strdup(owner->derived_slots[d]); out->path = _strdup(owner->derived_paths[d]);
            out->name = _strdup(owner->derived_names[d]);
            if (!out->slot || !out->path || !out->name) return pl_fail(w, "cannot retain a derived resource publication");
        }
    }
    return 1;
}

/* A placed entity retains the editor template that explicitly describes it.
 * This is a lifetime edge after equivalence/name assignment, not a native
 * source dependency or a reason to merge two otherwise different bindings. */
static int pl_editor_companions(pl_work *w)
{
    const sh_package_link_classes *classes = &w->output.classes;
    for (size_t c = 0; c < classes->count; c++) {
        size_t selector = classes->representatives[c], cursor = selector, target = SIZE_MAX;
        if (strcmp(w->units[selector].family, "snapeditorentitydef") || !w->units[selector].file_count) continue;
        for (size_t depth = 0; depth < classes->count; depth++) {
            const pl_unit *unit = w->work + cursor;
            size_t parent = SIZE_MAX; int direct = 0;
            for (size_t f = 0; f < w->units[cursor].file_count; f++) {
                const pl_file *file = unit->files + f;
                const sh_compiled_resource *bound = sh_package_compilation_find(w->output.compiled, unit->paths[f]);
                if (!bound || !bound->body) return pl_fail(w, "editor companion has no bound declaration body");
                /* Read in order: a shipped row may repeat a field, as
                 * fx/ambient_embers states fxInfo twice (package_editor_rows.c). */
                sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){(char *)bound->body, bound->body_length},
                    w->error, w->capacity);
                if (!tree) return 0;
                for (const sh_decl_node *m = tree->children; m; m = m->next)
                    if (m->key && (!strcmp(m->key, "edit.entityDef") ||
                        (!strcmp(m->key, "edit") && sh_decl_tree_member(m, "entityDef")))) direct = 1;
                sh_decl_tree_free(tree);
                for (size_t e = file->edge_begin; e < file->edge_begin + file->edge_count; e++) {
                    const sh_package_link_edge *edge = unit->edges + e;
                    const char *slot = strchr(edge->slot, ':');
                    if (!slot) return pl_fail(w, "editor companion edge lost its typed site");
                    slot += 1 + strlen(file->resource->engine_path);
                    size_t resolved = classes->representatives[classes->nodes[edge->target]];
                    if (!strcmp(slot, "edit.entityDef") && !strcmp(w->units[resolved].family, "entitydef")) target = resolved;
                    if (!strcmp(slot, "inherit") && !strcmp(w->units[resolved].family, "snapeditorentitydef")) parent = resolved;
                }
            }
            if (target != SIZE_MAX || direct || parent == SIZE_MAX) break;
            cursor = parent;
        }
        if (target != SIZE_MAX && w->units[target].file_count) {
            sh_compiled_resource *entity = (sh_compiled_resource *)sh_package_compilation_find(w->output.compiled, w->work[target].paths[0]);
            if (!entity || !pl_dependency(w, entity, w->work[selector].paths[0])) return 0;
        }
    }
    return 1;
}

static void pl_projection_body_free(sh_package_resource_projection *p)
{
    if (p->body_owner) sh_package_body_release(p->body_owner); else free((void *)p->body);
    p->body_owner = NULL; p->body = NULL;
}
static int pl_projection_order(const void *a, const void *b)
{
    const sh_package_resource_projection *x = a, *y = b;
    int order = strcmp(x->engine_path, y->engine_path);
    if (!order) order = strcmp(x->native_type, y->native_type);
    return order ? order : strcmp(x->native_name, y->native_name);
}
static int pl_projection_equal(pl_work *w, const sh_package_sources *sources,
    const sh_package_resource_projection *a, const sh_package_resource_projection *b)
{
    /* NULL bodies positively mean unchanged replay. Assignment already proved
     * exact original bytes for every class reusing this physical path. */
    if (!a->body && !b->body) return 1;
    sh_package_compilation source = {0}; source.sources = sources;
    unsigned char *left = NULL, *right = NULL;
    size_t alen = a->length, blen = b->length;
    const unsigned char *x = a->body, *y = b->body;
    if (!x) x = left = sh_package_compilation_read(&source, a->inputs[0], SIZE_MAX, &alen, w->error, w->capacity);
    if (!y) y = right = sh_package_compilation_read(&source, b->inputs[0], SIZE_MAX, &blen, w->error, w->capacity);
    int equal = x && y && alen == blen && !memcmp(x, y, alen);
    free(left); free(right);
    return equal ? 1 : pl_fail(w, "logical resource aliases emit different bytes at one physical path");
}
typedef struct pl_project_alias { const char *path, *type, *name; } pl_project_alias;
/* A stable unit (adapter contract) whose files are all held in memory binds
 * immutable bytes through a resolution frozen for the whole link: replaying
 * it could only repeat its capture. */
static int pl_project_immutable(pl_work *w, size_t unit)
{
    if (!w->adapter->stable || !w->adapter->stable(w->adapter->context, unit)) return 0;
    for (size_t f = 0; f < w->units[unit].file_count; f++) {
        /* A detached original's bytes are as immutable as held ones: its body reads them again exactly. */
        const sh_compiled_resource *file = w->work[unit].files[f].resource;
        if (!file->body && !file->body_owner) return 0;
    }
    return 1;
}
/* Such a unit projects to its own bytes when every reference site keeps its
 * target's name: pl_reference rewrites a site only to a different name. */
static int pl_project_unchanged(pl_work *w, size_t unit)
{
    const pl_unit *u = &w->work[unit];
    if (!pl_project_immutable(w, unit)) return 0;
    for (size_t f = 0; f < w->units[unit].file_count; f++)
        for (size_t e = u->files[f].edge_begin; e < u->files[f].edge_begin + u->files[f].edge_count; e++) {
            size_t target = u->edges[e].target;
            if (strcmp(w->output.names[w->output.classes.nodes[target]], w->units[target].name)) return 0;
        }
    return 1;
}
static int pl_project(pl_work *w, const sh_package_sources *sources, const sh_package_policy *policy,
    char *(*canonical_path)(const char *))
{
    sh_package_link_classes *classes = &w->output.classes;
    size_t count = 0, used = 0;
    sh_package_resource_projection *projections = NULL;
    pl_project_alias *aliases = NULL;
    int ok = 0;
    for (size_t c = 0; c < classes->count; c++) {
        size_t files = w->units[classes->representatives[c]].file_count;
        size_t derived = w->work[classes->representatives[c]].derived_count;
        if (files > SIZE_MAX - count || derived > SIZE_MAX - count - files) return pl_fail(w, "too many linked resource files");
        count += files + derived;
    }
    projections = pl_alloc(count, sizeof(*projections));
    aliases = pl_alloc(count, sizeof(*aliases));
    if (!projections || !aliases) { free(projections); free(aliases); return pl_fail(w, "cannot allocate linked resource provider"); }
    w->replay = 1;
    for (size_t c = 0; c < classes->count; c++) {
        size_t start = w->member_offsets[c], end = w->member_offsets[c + 1];
        size_t members = end - start;
        w->unit = classes->representatives[c];
        pl_unit *u = &w->work[w->unit];
        /* A changed carried class is served only at its owners' derived paths. */
        if (w->carried[w->unit]) continue;
        int itself = pl_project_unchanged(w, w->unit);
        /* An unchanged carried member (an installed module's file) keeps no
         * copy of its installed bytes: it stays the installed file. */
        int installed = w->adapter->derive && w->originals[w->unit] && w->units[w->unit].carried &&
            w->units[w->unit].file_count == 1 && u->files[0].resource->native_original &&
            u->files[0].resource->baseline_known == 1 && !u->files[0].resource->source_count &&
            !u->files[0].resource->generated && !u->files[0].resource->type;
        for (w->file = 0; w->file < w->units[w->unit].file_count; w->file++) {
            sh_package_resource_projection *p = &projections[used++];
            int unchanged = 0;
            const sh_compiled_resource **inputs = pl_alloc(members, sizeof(*inputs));
            p->inputs = inputs;
            if (!inputs) goto done;
            for (size_t i = start; i < end; i++) {
                size_t index = w->members[i];
                if (w->units[index].file_count != w->units[w->unit].file_count ||
                    strcmp(w->work[index].files[w->file].resource->engine_path, u->files[w->file].resource->engine_path))
                    { pl_fail(w, "equivalent resource files do not match"); goto done; }
                inputs[p->input_count++] = w->work[index].files[w->file].resource;
            }
            p->engine_path = u->paths[w->file];
            p->native_type = u->files[w->file].family;
            p->native_name = u->names ? u->names[w->file] : w->output.names[c];
            if (installed) { p->installed = 1; p->installed_identity = u->files[w->file].identity; continue; }
            if (itself) continue; /* Unchanged: its inputs supply the bytes. */
            w->rebound_references = w->rebound_non_image = 0;
            p->body = pl_bind(w, &p->length, &unchanged);
            if (!p->body) goto done;
            p->rebound_original = pl_rebound_original(w, c);
            p->rebound_references = w->rebound_references; p->rebound_non_image = w->rebound_non_image;
            /* Unchanged opaque files keep their immutable source/cache handle;
             * linking must not retain a second in-memory copy of every model. */
            if (unchanged) {
                free((void *)p->body); p->body = NULL; p->length = 0;
            } else {
                /* The rebound bytes move to the output rather than being copied (a refused owner copies them). */
                sh_package_body *owner = sh_package_body_adopt((unsigned char *)p->body, p->length);
                if (owner) { p->body_owner = owner; p->body = sh_package_body_bytes(owner); }
            }
        }
    }
    /* Derived publications: each target's bound bytes, as its own class would
     * project them, at the path its owner's name derives. */
    for (size_t c = 0; c < classes->count; c++) {
        const pl_unit *owner = &w->work[classes->representatives[c]];
        for (size_t d = 0; d < owner->derived_count; d++) {
            size_t target = classes->nodes[owner->derived_targets[d]];
            size_t start = w->member_offsets[target], end = w->member_offsets[target + 1];
            sh_package_resource_projection *p = &projections[used++];
            int unchanged = 0;
            const sh_compiled_resource **inputs = pl_alloc(end - start, sizeof(*inputs));
            p->inputs = inputs;
            if (!inputs) goto done;
            w->unit = classes->representatives[target]; w->file = 0;
            for (size_t i = start; i < end; i++) inputs[p->input_count++] = w->work[w->members[i]].files[0].resource;
            p->engine_path = owner->derived_paths[d];
            p->native_type = w->work[w->unit].files[0].family;
            p->native_name = owner->derived_names[d];
            /* A certified installed file is served from the installed copy. */
            const sh_compiled_resource *installed = w->work[w->unit].files[0].resource;
            if (owner->derived_alias[d] && w->originals[w->unit] && installed->native_original &&
                installed->baseline_known == 1 && !installed->source_count && !installed->generated) {
                p->installed = 1; p->installed_identity = w->work[w->unit].files[0].identity; continue;
            }
            if (pl_project_unchanged(w, w->unit)) continue;
            p->body = pl_bind(w, &p->length, &unchanged);
            if (!p->body) goto done;
            if (unchanged) { free((void *)p->body); p->body = NULL; p->length = 0; }
        }
    }
    count = used;
    /* Equivalent sources still have independent lifetimes. Do not let a
     * changed duplicate or unstable scope hide behind its representative;
     * only an immutable stable unit cannot change. */
    for (w->unit = 0; w->unit < w->count; w->unit++)
        if (classes->representatives[classes->nodes[w->unit]] != w->unit && !pl_project_immutable(w, w->unit))
            for (w->file = 0; w->file < w->units[w->unit].file_count; w->file++) {
                size_t length = 0;
                unsigned char *body = pl_bind(w, &length, NULL);
                if (!body) goto done;
                free(body);
            }
    for (w->unit = 0; w->unit < w->count; w->unit++)
        if (!pl_implicit_read(w)) goto done;
    /* Every witness is opened and verified again after binding, once each. */
    w->verified_witness_count = 0;
    for (size_t unit = 0; unit < w->count; unit++) for (size_t i = 0; i < w->units[unit].witness_count; i++) {
        const pl_file *captured = w->work[unit].witnesses + i; sh_package_file_identity current;
        if (!sh_package_compilation_identity(w->units[unit].compilation, captured->resource,
            &current, w->error, w->capacity) || current.length != captured->identity.length ||
            memcmp(current.digest, captured->identity.digest, 32)) {
            pl_fail(w, "resource witness bytes changed during linking"); goto done;
        }
        if (!pl_witness_available(w, unit, captured)) goto done;
    }
    if (count > 1) qsort(projections, count, sizeof(*projections), pl_projection_order);
    size_t unique = 0;
    for (size_t i = 0; i < count; i++) {
        sh_package_resource_projection *p = projections + i;
        aliases[i] = (pl_project_alias){p->engine_path, p->native_type, p->native_name};
        if (unique && !strcmp(projections[unique - 1].engine_path, p->engine_path)) {
            sh_package_resource_projection *prior = projections + unique - 1;
            if (!pl_projection_equal(w, sources, prior, p) ||
                p->input_count > SIZE_MAX / sizeof(*p->inputs) - prior->input_count) goto done;
            size_t members = prior->input_count + p->input_count;
            const sh_compiled_resource **inputs = realloc((void *)prior->inputs, members * sizeof(*inputs));
            if (!inputs) { pl_fail(w, "cannot retain original alias suppliers"); goto done; }
            memcpy(inputs + prior->input_count, p->inputs, p->input_count * sizeof(*inputs));
            prior->inputs = inputs; prior->input_count = members;
            free((void *)p->inputs); pl_projection_body_free(p); memset(p, 0, sizeof(*p));
        } else {
            if (unique != i) { projections[unique] = *p; memset(p, 0, sizeof(*p)); }
            unique++;
        }
    }
    w->output.compiled = sh_package_compilation_project(sources, projections, unique,
        policy, canonical_path, w->error, w->capacity);
    if (!w->output.compiled) goto done;
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    if (g_pl_after_projection_refusal) { pl_fail(w, "test: link refused after its projection"); goto done; }
#endif
    for (size_t i = 0; i < count; i++) {
        sh_compiled_resource *resource = (sh_compiled_resource *)sh_package_compilation_find(w->output.compiled, aliases[i].path);
        if (!resource || !sh_package_resource_native_add(resource, aliases[i].type, aliases[i].name)) {
            pl_fail(w, "cannot retain a shared physical resource's logical identity"); goto done;
        }
    }
    /* Source-record ownership is independent of equality: two equal bindings
     * can have different suppliers. Keep the complete delivery union without
     * pretending a bank/media input is this declaration's physical file. */
    for (size_t i = 0; i < w->count; i++) {
        const sh_package_link_unit *unit = w->units + i;
        if ((unit->implicit_owners && !sh_package_owners_within(unit->implicit_owners, sources->package_count)) ||
            (unit->implicit_gameplay_owners && !sh_package_owners_within(unit->implicit_gameplay_owners, sources->package_count))) {
            pl_fail(w, "implicit resource ownership is outside the captured inventory"); goto done;
        }
        const pl_unit *representative = w->work + classes->representatives[classes->nodes[i]];
        for (size_t f = 0; !w->carried[i] && f < unit->file_count; f++) {
            sh_compiled_resource *resource = (sh_compiled_resource *)sh_package_compilation_find(
                w->output.compiled, representative->paths[f]);
            if (!resource || (unit->implicit_owners && !sh_package_owners_union(&resource->owners, unit->implicit_owners)) ||
                (unit->implicit_gameplay_owners && !sh_package_owners_union(&resource->gameplay_owners, unit->implicit_gameplay_owners))) {
                pl_fail(w, "cannot retain implicit resource ownership"); goto done;
            }
        }
        /* A carried unit's ownership travels with every publication of its class. */
        if (unit->implicit_owners || unit->implicit_gameplay_owners)
            for (size_t c = 0; c < classes->count; c++) {
                const pl_unit *owner = &w->work[classes->representatives[c]];
                for (size_t d = 0; d < owner->derived_count; d++) {
                    if (classes->nodes[owner->derived_targets[d]] != classes->nodes[i]) continue;
                    sh_compiled_resource *resource = (sh_compiled_resource *)sh_package_compilation_find(
                        w->output.compiled, owner->derived_paths[d]);
                    if (!resource || (unit->implicit_owners && !sh_package_owners_union(&resource->owners, unit->implicit_owners)) ||
                        (unit->implicit_gameplay_owners &&
                         !sh_package_owners_union(&resource->gameplay_owners, unit->implicit_gameplay_owners))) {
                        pl_fail(w, "cannot retain implicit resource ownership"); goto done;
                    }
                }
            }
    }
    /* Each published row's identity provenance: the classes projected at its path (R-397). */
    if (w->output.provenance) {
        size_t rows = w->output.compiled->resource_count;
        sh_package_owners *table = pl_alloc(rows, sizeof(*table));
        if (!table) { pl_fail(w, "cannot record identity provenance"); goto done; }
        w->output.row_provenance = table; w->output.row_provenance_count = rows;
        w->output.row_provenance_paths = pl_alloc(rows, sizeof(*w->output.row_provenance_paths));
        if (!w->output.row_provenance_paths) { pl_fail(w, "cannot record identity provenance"); goto done; }
        for (size_t r = 0; r < rows; r++) w->output.row_provenance_paths[r] = w->output.compiled->resources[r].engine_path;
        for (size_t c = 0; c < classes->count; c++) {
            const pl_unit *u = &w->work[classes->representatives[c]];
            for (size_t f = 0; !w->carried[classes->representatives[c]] && f < w->units[classes->representatives[c]].file_count; f++) {
                const sh_compiled_resource *resource = sh_package_compilation_find(w->output.compiled, u->paths[f]);
                if (resource && !sh_package_owners_union(table + (resource - w->output.compiled->resources),
                    w->output.provenance + c)) { pl_fail(w, "cannot record identity provenance"); goto done; }
            }
            for (size_t d = 0; d < u->derived_count; d++) {
                const sh_compiled_resource *resource = sh_package_compilation_find(w->output.compiled, u->derived_paths[d]);
                size_t target = classes->nodes[u->derived_targets[d]];
                if (resource && (!sh_package_owners_union(table + (resource - w->output.compiled->resources),
                    w->output.provenance + target) || !sh_package_owners_union(table + (resource - w->output.compiled->resources),
                    w->output.provenance + c))) { pl_fail(w, "cannot record identity provenance"); goto done; }
            }
        }
    }
    ok = pl_dependencies(w) && pl_editor_companions(w) && pl_implicit_output(w) && pl_derived_output(w);
done:
    for (size_t i = 0; i < used; i++) { free((void *)projections[i].inputs); pl_projection_body_free(projections + i); }
    free(projections); free(aliases); return ok;
}

static unsigned long long pl_now_us(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000000u + (unsigned long long)now.tv_nsec / 1000u : 0;
}
const char *sh_package_link_reuse_counters(const sh_package_link_reuse *reuse, char *out, size_t capacity)
{
    size_t used = 0;
    if (!out || !capacity) return out;
    *out = 0;
    if (!reuse) return out;
    int n = snprintf(out, capacity, "%zu identity passes", reuse->pass_count);
    /* With each pass's capture templates (R-348): files that took one, made, edges and single-file digests taken, and
     * those made from an offered record. */
    for (size_t i = 0; n > 0 && (used += (size_t)n) < capacity && i < reuse->pass_count && i < 16; i++)
        n = snprintf(out + used, capacity - used, "%s %zu units %zu restored, collect %llu ms, partition %llu ms, clear %llu ms, "
            "templates %zu/%zu/%zu/%zu, %zu from the record (stable sites bound in %llu ms from templates, %llu ms from the "
            "record, %llu ms without)", i ? ";" : ":",
            reuse->passes[i].units, reuse->passes[i].restored,
            reuse->passes[i].collect_us / 1000u, reuse->passes[i].partition_us / 1000u, reuse->passes[i].clear_us / 1000u,
            reuse->passes[i].template_files, reuse->passes[i].template_built, reuse->passes[i].template_edges,
            reuse->passes[i].template_digests, reuse->passes[i].template_restored, reuse->passes[i].template_hit_us / 1000u,
            reuse->passes[i].template_restore_us / 1000u, reuse->passes[i].template_build_us / 1000u);
    /* Each on its own line: a log line holds about 1,200 characters, which nine passes fill. */
    if (n > 0 && used < capacity)
        n = snprintf(out + used, capacity - used, "\nidentity exact verification: %llu pairs, %llu shared members, "
            "%llu MB compared, %llu ms; slot texts %zu (%llu KB), template keys %llu KB, %zu templates", reuse->verify_pairs,
            reuse->verify_shared, reuse->verify_bytes >> 20, reuse->verify_us / 1000u, reuse->text_count,
            reuse->text_bytes >> 10, reuse->template_key_bytes >> 10, reuse->template_count);
    if (n > 0 && reuse->offer_state && (used += (size_t)n) < capacity)
        n = snprintf(out + used, capacity - used, "\nidentity templates from the record: %s, %zu offered (%llu KB, indexed "
            "in %llu ms), %zu taken, %zu refused", reuse->offer_state < 0 ? "malformed, dropped" : "offered",
            reuse->offer_templates, reuse->offer_bytes >> 10, reuse->offer_us / 1000u, reuse->offer_taken,
            reuse->offer_refused);
    /* Each pass's partition, on its own line. */
    for (size_t i = 0; n > 0 && (used += (size_t)n) < capacity && i < reuse->pass_count && i < 16; i++)
        n = snprintf(out + used, capacity - used, "%s %zu: view %llu, shapes %llu, refine %llu, identities %llu ms, "
            "%zu classes, %zu read in capture order", i ? ";" : "\nidentity partitions by pass:", i + 1, reuse->passes[i].view_us / 1000u,
            reuse->passes[i].shapes_us / 1000u, reuse->passes[i].refine_us / 1000u,
            reuse->passes[i].identity_us / 1000u, reuse->passes[i].classes, reuse->passes[i].ordered);
    return out;
}
/* Each pass's meters on one line: what it captured afresh (and whether sticky-invalid
 * recaptures reproduced their previous capture), and what pl_conditional's
 * certification walked, removed and produced. */
#define PL_TENTHS(us) (us) / 1000u, (us) % 1000u / 100u
const char *sh_package_link_reuse_detail(const sh_package_link_reuse *reuse, char *out, size_t capacity)
{
    size_t used = 0;
    if (!out || !capacity) return out;
    *out = 0;
    for (size_t i = 0; reuse && i < reuse->pass_count && i < 16; i++) {
#define PL_PASS reuse->passes[i]
        int n = snprintf(out + used, capacity - used,
            "%s %zu: fresh %zu in %llu.%llu ms, again %zu first %zu same %zu changed, conditional %llu.%llu ms "
            "(%zu units %zu bindings %zu removed %zu slot compares, view %llu.%llu, walk %llu.%llu over %zu edges %zu not original "
            "%zu certified digest %08llx, filter %llu.%llu), alloc %llu.%llu ms",
            i ? ";" : "identity pass detail by pass:", i + 1, PL_PASS.fresh, PL_TENTHS(PL_PASS.fresh_us),
            PL_PASS.again_first, PL_PASS.again_same, PL_PASS.again_changed, PL_TENTHS(PL_PASS.cond_us),
            PL_PASS.cond_units, PL_PASS.cond_edges, PL_PASS.cond_removed, PL_PASS.cond_compares, PL_TENTHS(PL_PASS.cond_view_us),
            PL_TENTHS(PL_PASS.cond_walk_us), PL_PASS.cond_walk_edges, PL_PASS.cond_nonlocal, PL_PASS.cond_certified,
            (unsigned long long)(PL_PASS.cond_digest & 0xffffffffu), PL_TENTHS(PL_PASS.cond_filter_us),
            PL_TENTHS(PL_PASS.alloc_us));
#undef PL_PASS
        if (n < 0 || (size_t)n >= capacity - used) { out[used] = 0; break; }
        used += (size_t)n;
    }
    return out;
}
#undef PL_TENTHS
#ifdef SH_PACKAGE_LINKER_TESTING
void sh_package_link_reuse_test_mode(sh_package_link_reuse *reuse, int copy, size_t fail_unshare)
{
    if (reuse) { reuse->copy_restore = copy; reuse->fail_unshare = fail_unshare; }
}
void sh_package_link_reuse_test_counts(const sh_package_link_reuse *reuse,
    unsigned long long *borrowed, unsigned long long *unshared)
{
    *borrowed = reuse ? reuse->borrowed_restores : 0; *unshared = reuse ? reuse->unshared : 0;
}
int sh_package_link_reuse_test_meters(const sh_package_link_reuse *reuse, size_t pass, sh_package_link_pass_meters *out)
{
    if (!reuse || !out || pass >= reuse->pass_count || pass >= 16) return 0;
    out->fresh = reuse->passes[pass].fresh; out->again_first = reuse->passes[pass].again_first;
    out->again_same = reuse->passes[pass].again_same; out->again_changed = reuse->passes[pass].again_changed;
    out->cond_units = reuse->passes[pass].cond_units; out->cond_edges = reuse->passes[pass].cond_edges;
    out->cond_removed = reuse->passes[pass].cond_removed; out->cond_nonlocal = reuse->passes[pass].cond_nonlocal;
    out->cond_certified = reuse->passes[pass].cond_certified; out->cond_walk_edges = reuse->passes[pass].cond_walk_edges;
    out->cond_compares = reuse->passes[pass].cond_compares;
    out->cond_digest = reuse->passes[pass].cond_digest;
    out->ordered = reuse->passes[pass].ordered;
    out->template_files = reuse->passes[pass].template_files; out->template_built = reuse->passes[pass].template_built;
    out->template_edges = reuse->passes[pass].template_edges; out->template_digests = reuse->passes[pass].template_digests;
    out->template_restored = reuse->passes[pass].template_restored;
    return 1;
}
void sh_package_link_reuse_test_fail_meters(sh_package_link_reuse *reuse, int fail)
{
    if (reuse) reuse->fail_meter = fail;
}
void sh_package_link_reuse_test_fail_index(sh_package_link_reuse *reuse, int fail)
{
    if (reuse) reuse->fail_index = fail;
}
size_t sh_package_link_reuse_test_slots(const sh_package_link_reuse *reuse, size_t unit,
    const char **slots, size_t capacity)
{
    if (!reuse || unit >= reuse->count || !reuse->units[unit].captured) return 0;
    for (size_t i = 0; i < reuse->units[unit].edge_count && i < capacity; i++) slots[i] = reuse->units[unit].edges[i].slot;
    return reuse->units[unit].edge_count;
}
#endif
const char *sh_package_link_reuse_link_steps(const sh_package_link_reuse *reuse, char *out, size_t capacity)
{
    if (!out || !capacity) return out;
    *out = 0;
    if (reuse && reuse->linked)
        snprintf(out, capacity, "link members %llu ms, history %llu ms, assignment %llu ms, projection %llu ms",
            reuse->link_us[0] / 1000u, reuse->link_us[1] / 1000u, reuse->link_us[2] / 1000u, reuse->link_us[3] / 1000u);
    return out;
}
/* Free the pass's work, timing it as part of the pass this call recorded. */
static void pl_clear_timed(pl_work *w, size_t pass)
{
    sh_package_link_reuse *reuse = w->adapter ? w->adapter->reuse : NULL;
    unsigned long long began = pl_now_us();
    pl_clear(w);
    if (reuse && reuse->pass_count == pass + 1 && pass < 16) reuse->passes[pass].clear_us = pl_now_us() - began;
}
static int pl_identify(pl_work *w)
{
    unsigned long long entered = pl_now_us();
    if ((w->count && !w->units) || !w->adapter || !w->adapter->resolve || !w->adapter->bind)
        return pl_fail(w, "resource identity inputs require resolved source readers");
    /* Check every identity before a resolver can reference a later unit. */
    for (size_t i = 0; i < w->count; i++) if (!w->units[i].family || !*w->units[i].family ||
        !w->units[i].name || !*w->units[i].name)
        return pl_fail(w, "resource identity inputs contain an unnamed unit");
    w->work = pl_alloc(w->count, sizeof(*w->work));
    w->nodes = pl_alloc(w->count, sizeof(*w->nodes));
    unsigned long long began = pl_now_us();
    w->alloc_us = began - entered;
    if (!w->work || !w->nodes) return 0;
    int captured = pl_collect(w);
    /* An offered record serves the first pass that captures with templates (R-348). */
    if (w->adapter->reuse && w->adapter->reuse->offer_state == 1 && pl_templating(w)) pl_offer_release(w->adapter->reuse);
    if (!captured || !pl_conditional(w)) return 0;
    unsigned long long collected = pl_now_us();
    pl_view held = {0};
    sh_package_link_node *view = pl_stable_view(w, &held);
    unsigned long long viewed = pl_now_us();
    if (!view) return pl_fail(w, "cannot allocate resource identity view");
    int ok = sh_package_link_identify_ordered(view, w->count, pl_verify_equal, w,
        w->adapter->reuse ? &w->adapter->reuse->order : NULL, &w->output.classes, w->error, w->capacity);
    pl_stable_view_end(w, &held);
    sh_package_link_reuse *reuse = w->adapter->reuse;
    if (reuse && reuse->pass_count < 16) {
        reuse->passes[reuse->pass_count].units = w->count; reuse->passes[reuse->pass_count].restored = w->restored;
        reuse->passes[reuse->pass_count].collect_us = collected - began;
        reuse->passes[reuse->pass_count].partition_us = pl_now_us() - collected;
        reuse->passes[reuse->pass_count].view_us = viewed - collected;
        reuse->passes[reuse->pass_count].classes = ok ? w->output.classes.count : 0;
        reuse->passes[reuse->pass_count].ordered = ok ? w->output.classes.ordered : 0;
        reuse->passes[reuse->pass_count].shapes_us = ok ? w->output.classes.shapes_us : 0;
        reuse->passes[reuse->pass_count].refine_us = ok ? w->output.classes.refine_us : 0;
        reuse->passes[reuse->pass_count].identity_us = ok ? w->output.classes.identity_us : 0;
#define PL_METER(field) reuse->passes[reuse->pass_count].field = w->field
        PL_METER(fresh); PL_METER(again_first); PL_METER(again_same); PL_METER(again_changed);
        PL_METER(cond_units); PL_METER(cond_edges); PL_METER(cond_removed); PL_METER(cond_nonlocal);
        PL_METER(cond_certified); PL_METER(cond_walk_edges); PL_METER(cond_compares); PL_METER(cond_digest);
        PL_METER(fresh_us); PL_METER(cond_us); PL_METER(cond_view_us); PL_METER(cond_walk_us);
        PL_METER(cond_filter_us); PL_METER(alloc_us);
        PL_METER(template_files); PL_METER(template_built); PL_METER(template_edges); PL_METER(template_digests);
        PL_METER(template_restored); PL_METER(template_hit_us); PL_METER(template_restore_us); PL_METER(template_build_us);
#undef PL_METER
    }
    if (reuse) reuse->pass_count++;
    return ok;
}

int sh_package_link_identify_units(const sh_package_link_unit *units, size_t count,
    const sh_package_link_adapter *adapter, sh_package_link_classes *out,
    char *error, size_t capacity)
{
    char fallback[1024]; pl_work w = {0}; int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!out) { snprintf(error, capacity, "resource identity output is required"); return 0; }
    sh_package_link_classes_free(out);
    w.units = units; w.count = count; w.adapter = adapter; w.error = error; w.capacity = capacity;
    size_t pass = adapter && adapter->reuse ? adapter->reuse->pass_count : 0;
    if (pl_identify(&w)) {
        *out = w.output.classes; memset(&w.output.classes, 0, sizeof(w.output.classes)); ok = 1;
    }
    if (!ok && !*error) snprintf(error, capacity, "resource identities could not be captured");
    pl_clear_timed(&w, pass); return ok;
}

/* Identity provenance (R-397): per class, every package whose content entered its identity or bytes. A class
 * starts from its members' files' owners and gameplay owners and their units' provenance, and takes each class its
 * identity edges reach (the stable view's edges: an edge the identity drops carries no content into it). Classes
 * grow monotonically to a fixed point, so cycles and shared stock close too. */
static int pl_provenance_close(pl_work *w)
{
    sh_package_link_classes *classes = &w->output.classes;
    size_t count = classes->count, edges = 0;
    int any = 0;
    for (size_t i = 0; i < w->count && !any; i++) any = w->units[i].provenance != NULL;
    if (!any || !count) return 1;
    sh_package_owners *set = pl_alloc(count, sizeof(*set));
    size_t *offsets = pl_alloc(count + 1, sizeof(*offsets)), *incoming = NULL, *queue = NULL, *cursor = NULL;
    unsigned char *queued = pl_alloc(count, 1);
    if (!set || !offsets || !queued) goto failed;
    for (size_t i = 0; i < w->count; i++) {
        const sh_package_link_unit *unit = w->units + i;
        size_t c = classes->nodes[i];
        /* A certified original binding is the installed one in every scope: its scope adds nothing. */
        if (unit->provenance && !w->originals[i] && !sh_package_owners_union(set + c, unit->provenance)) goto failed;
        for (size_t f = 0; f < unit->file_count; f++)
            if (unit->files[f] && (!sh_package_owners_union(set + c, &unit->files[f]->owners) ||
                !sh_package_owners_union(set + c, &unit->files[f]->gameplay_owners))) goto failed;
        if ((unit->implicit_owners && !sh_package_owners_union(set + c, unit->implicit_owners)) ||
            (unit->implicit_gameplay_owners && !sh_package_owners_union(set + c, unit->implicit_gameplay_owners))) goto failed;
        for (size_t e = 0; e < w->nodes[i].edge_count; e++) {
            size_t t = w->nodes[i].edges[e].target;
            if (t < w->count && !w->units[t].stable_name) { offsets[classes->nodes[t] + 1]++; edges++; }
        }
    }
    /* incoming[target class] lists the classes whose identity reaches it. */
    for (size_t c = 0; c < count; c++) offsets[c + 1] += offsets[c];
    incoming = pl_alloc(edges, sizeof(*incoming)); cursor = pl_alloc(count, sizeof(*cursor));
    queue = pl_alloc(count, sizeof(*queue));
    if (!incoming || !cursor || !queue) goto failed;
    for (size_t c = 0; c < count; c++) cursor[c] = offsets[c];
    for (size_t i = 0; i < w->count; i++)
        for (size_t e = 0; e < w->nodes[i].edge_count; e++) {
            size_t t = w->nodes[i].edges[e].target;
            if (t < w->count && !w->units[t].stable_name) incoming[cursor[classes->nodes[t]]++] = classes->nodes[i];
        }
    size_t head = 0, tail = 0, pending = 0;
    for (size_t c = 0; c < count; c++) if (sh_package_owners_count(set + c)) { queue[tail++] = c; queued[c] = 1; pending++; }
    tail %= count;
    while (pending) {
        size_t c = queue[head]; head = (head + 1) % count; queued[c] = 0; pending--;
        if (!sh_package_owners_count(set + c)) continue;
        for (size_t k = offsets[c]; k < offsets[c + 1]; k++) {
            size_t from = incoming[k], had = sh_package_owners_count(set + from);
            if (from == c) continue;
            if (!sh_package_owners_union(set + from, set + c)) goto failed;
            if (sh_package_owners_count(set + from) != had && !queued[from]) {
                queue[tail] = from; tail = (tail + 1) % count; queued[from] = 1; pending++;
            }
        }
    }
    free(offsets); free(incoming); free(cursor); free(queue); free(queued);
    w->output.provenance = set; return 1;
failed:
    if (set) for (size_t c = 0; c < count; c++) sh_package_owners_free(set + c);
    free(set); free(offsets); free(incoming); free(cursor); free(queue); free(queued);
    return pl_fail(w, "cannot record identity provenance");
}

int sh_package_link(const sh_package_sources *sources, const sh_package_link_unit *units,
    size_t count, const sh_package_link_adapter *adapter, const sh_package_policy *policy,
    char *(*canonical_path)(const char *), sh_package_link_output *out, char *error, size_t capacity)
{
    char fallback[1024];
    pl_work w = {0};
    int ok = 0;
    size_t pass = SIZE_MAX;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!out) { snprintf(error, capacity, "linked provider output is required"); return 0; }
    sh_package_link_output_free(out);
    w.units = units; w.count = count; w.adapter = adapter; w.error = error; w.capacity = capacity;
    if (!sources || (count && !units) || !adapter || !adapter->resolve || !adapter->bind || !adapter->assign ||
        (adapter->registry && !adapter->registry->check) || (adapter->history && !adapter->registry))
        goto done;
    unsigned long long steps[5];
    pass = adapter->reuse ? adapter->reuse->pass_count : 0;
    if (!pl_identify(&w)) goto done;
    if (adapter->reuse && adapter->release_order) sh_package_link_order_free(&adapter->reuse->order);
    steps[0] = pl_now_us();
    if (!pl_members(&w)) goto done;
    steps[1] = pl_now_us();
    if (!pl_history_prepare(&w)) goto done;
    steps[2] = pl_now_us();
    if (!pl_assign(&w)) goto done;
    if (!pl_provenance_close(&w)) goto done;
    steps[3] = pl_now_us();
    if (!pl_project(&w, sources, policy, canonical_path)) goto done;
    steps[4] = pl_now_us();
    if (adapter->reuse) {
        for (int i = 0; i < 4; i++) adapter->reuse->link_us[i] = steps[i + 1] - steps[i];
        adapter->reuse->linked = 1;
    }
    if (w.history) {
        sh_package_link_history previous = *adapter->history;
        *adapter->history = *w.history; *w.history = previous;
    }
    *out = w.output; memset(&w.output, 0, sizeof(w.output)); ok = 1;
done:
    if (!ok && !*error) snprintf(error, capacity, "resource linker input is invalid or unavailable");
    pl_clear_timed(&w, pass); return ok;
}

int sh_package_link_provenance(const sh_package_link_output *linked,
    const sh_package_link_unit *units, size_t count,
    const sh_package_provenance *contexts, const size_t *unit_contexts,
    const size_t *roots, size_t root_count, sh_package_provenance *out,
    char *error, size_t capacity)
{
    sh_package_provenance result = {0}; unsigned char *selected = NULL; int ok = 0;
    const char *reason = "map binding selection does not match its linked source snapshot";
    if (error && capacity) error[0] = 0;
    if (!out || out == contexts) return 0;
    sh_package_provenance_free(out);
    if (!linked || !linked->compiled || !contexts || count != linked->classes.node_count ||
        (count && (!units || !unit_contexts || !linked->classes.nodes)) ||
        (linked->classes.count && (!linked->names || !linked->classes.representatives)) ||
        (root_count && !roots)) goto done;
    selected = calloc(linked->classes.count ? linked->classes.count : 1, 1);
    if (!selected) { reason = "cannot retain selected map binding classes"; goto done; }
    for (size_t i = 0; i < count; i++)
        if (linked->classes.nodes[i] >= linked->classes.count) goto done;
    for (size_t i = 0; i < root_count; i++) {
        if (roots[i] >= count) goto done;
        selected[linked->classes.nodes[roots[i]]] = 1;
    }
    for (size_t i = 0; i < count; i++) {
        size_t c = linked->classes.nodes[i], context = unit_contexts[i], key;
        const sh_package_provenance_context *source;
        if (!selected[c]) continue;
        if (!units[i].family || !units[i].name || !linked->names[c]) goto done;
        if (context == SIZE_MAX) {
            if (units[i].file_count || strcmp(units[i].name, linked->names[c])) goto done;
            continue;
        }
        if (context >= contexts->context_count || !contexts->contexts) goto done;
        source = contexts->contexts + context;
        key = sh_package_provenance_context_add(&result, source->mode, source->trees, source->tree_count, source->component);
        if (key == SIZE_MAX || !sh_package_provenance_bind(&result, units[i].family,
            units[i].name, linked->names[c], &key, 1)) goto done;
    }
    *out = result; memset(&result, 0, sizeof(result)); ok = 1;
done:
    free(selected); sh_package_provenance_free(&result);
    if (!ok && error && capacity) snprintf(error, capacity, "%s", reason);
    return ok;
}
