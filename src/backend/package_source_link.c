#include "package_source_link.h"
#include "package_keys.h"
#include "decl_entity_class.h"
#include "package_virtual_material.h"
#include "package_font.h"
#include "package_movie.h"
#include "package_audio_program.h"
#include "package_fsm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <intrin.h>

typedef struct sl_shader_program {
    char *name;
    size_t parent;
    uint64_t mask;
    sh_package_shader_program decoded;
} sl_shader_program;
typedef struct sl_shader_file {
    const sh_compiled_resource *file;
    size_t program;
    sh_package_shader_path_kind kind;
} sl_shader_file;
/* conditional: a state-machine selection (sh_package_link_unit.conditional). */
typedef struct sl_implicit { char *slot; struct sl_unit *target; int conditional; } sl_implicit;
/* One reference site discovery bound, in bind order (see sl_reference). */
typedef struct sl_site { const sh_compiled_resource *file; char *slot; struct sl_unit *target; } sl_site;
typedef struct sl_unit {
    sh_package_link_unit unit;
    size_t scope, index, capacity;
    struct sl_unit **dependencies;
    size_t dependency_count, dependency_capacity;
    sl_shader_program *programs;
    size_t program_count;
    sl_shader_file *shader_files;
    size_t shader_file_count;
    int material_members;
    sl_implicit *implicit;
    size_t implicit_count;
    sh_package_link_edge *implicit_edges; /* unit.implicit, then unit.conditional; owned */
    char *fsm_config; /* The layer configuration an aibehavior names (sl_fsm_edges). */
    size_t reverse_gap; /* Observation only: 1 + index of its accounted reader gap. */
    struct sl_unit *parent; /* Unit whose discovery first requested it; NULL for seeds. */
    char *seed; /* Why a unit without a same-scope parent entered the capture. */
    int nominated; /* Seeded as a stock root affected by its scope's changes. */
    int movie_captured; /* Constructor requests of a cooked movie were enumerated. */
    int replayed; /* Discovery replayed another scope's recorded reads (sl_replay_apply). */
    /* Created by seeding (the scope's supplied files), and its witness count
     * then: reverse nomination reads seeded units only, as seeded. */
    int seeded;
    size_t seeded_witnesses;
    sh_package_link_file_identity *identities; /* Owned per-file identities (movie pairs). */
    sh_compiled_resource prepared; /* Borrowed metadata; owns only body. */
    const sh_compiled_resource *unprepared; /* Exact original certificate input. */
    /* Diagnostics: why this unit's own binding is not certified original. */
    char *original_note;
    int preparation_checked;
    /* Sites each discovered file bound, and those files; after freezing,
     * the same sites as (slot, unit index) edges (sh_package_source_link_sites). */
    sl_site *sites;
    size_t site_count, site_capacity;
    const sh_compiled_resource **discovered;
    size_t discovered_count;
    sh_package_link_edge *site_edges;
} sl_unit;
typedef struct sl_fsm_mask {
    const sh_compiled_resource *file; /* the declaration's source in some scope */
    char *config; /* NULL: the union of its graph masks; else this configuration's */
    uint32_t mask;
    int status; /* 1 found, 0 absent (no graphs, or no such configuration) */
} sl_fsm_mask;
typedef struct sl_original {
    sh_compiled_resource resource;
    int status;
    int identified; /* identity holds the payload identity of resource.body */
    sh_package_file_identity identity;
} sl_original;
/* One recorded discovery a later discovery may replay (sl_replay_*): the
 * reads a scope's reader made to prepare, bind and request a unit, and what
 * they read. Each file, and each scoped path the reader consulted, keeps how
 * it resolved: supplied by the scope (state -1) or the capture's original
 * (its status; 0 is absent), with its content identity when a memo keeps the
 * recording. valid is cleared when recording ran out of memory. */
typedef struct sl_replay_ref { char *slot, *family, *name; int request; } sl_replay_ref;
typedef struct sl_replay_input { char *path; int state; sh_package_file_identity identity; } sl_replay_input;
typedef struct sl_replay {
    struct sl_replay *next; /* other recordings of this family and name */
    char *family, *name;
    unsigned long long capture; /* the capture that recorded it, and its scope */
    size_t scope;
    sl_replay_input *files;
    size_t file_count;
    unsigned char *prepared; /* the native preparation output, when prepared */
    size_t prepared_length;
    int was_prepared, valid;
    sl_replay_input *lookups; /* every scoped path consulted */
    size_t lookup_count, lookup_capacity;
    sl_replay_ref *refs; /* per file: its sites, then its requests, in call order */
    size_t ref_count, ref_capacity;
    size_t *file_refs; /* refs of file j end at file_refs[j] */
} sl_replay;
/* Recordings by family and name (open addressing over list heads). */
typedef struct sl_store { sl_replay **slots; size_t count, capacity, records; } sl_store;
struct sh_package_source_memo {
    volatile long busy; /* borrowed by one capture */
    int keyed;
    unsigned char key[32]; /* sl_memo_key of the captures that recorded */
    unsigned long long captures;
    sl_store store;
};
struct sh_package_source_link {
    const sh_package_compilation **scopes;
    sh_package_source_reader **readers;
    size_t scope_count;
    const sh_resource_catalog *catalog;
    sh_package_baseline_reader original;
    void *original_context;
    sh_package_source_link_native native;
    sh_package_source_shader shader;
    sl_unit **units, **unit_slots;
    size_t count, capacity, unit_slot_count;
    sl_original **originals, **original_slots;
    size_t original_count, original_capacity, original_slot_count;
    sh_package_link_inputs inputs;
    unsigned char *shared;
    int frozen, failed, observe;
    char *error;
    size_t error_capacity;
    sh_package_reverse_gap *reverse_gaps;
    size_t reverse_gap_count, reverse_gap_capacity;
    int reader_gap; /* The last discovery stopped in a registered reader, not I/O. */
    int preparing; /* Last unit has an explicitly retained native program request. */
    char preparation_reason[1024]; /* First deferred request in this frontier. */
    int malformed; /* That reader could not parse the text in the native grammar. */
    int environment; /* A failure came from I/O, allocation or configuration. */
    /* sl_defer_failure: the first failure a scope alone answers for, kept
     * while the other scopes continue, and the scopes withdrawn for one. */
    struct { const sl_unit *unit; int malformed, reader_gap; char error[1024]; } deferred;
    unsigned char *withdrawn;
    sl_unit *discovering; /* Unit whose readers are requesting new units. */
    const sl_unit *failed_unit; /* Unit whose strict discovery failed, if known. */
    size_t failed_scope; /* Input scope whose supplied files failed seeding. */
    char seed_note[320]; /* Current seeding site, copied into units it creates. */
    char **player_hands; /* Names of animation webs a player's handsAnimWeb names. */
    size_t player_hand_count;
    /* AI state machines (sl_fsm_edges): the catalog's SnapMap declaration
     * names, and masks by the exact source file each scope resolves. */
    char **fsm_names;
    size_t fsm_name_count;
    int fsm_names_ready;
    struct sl_fsm_mask *fsm_masks;
    size_t fsm_mask_count, fsm_mask_capacity;
    unsigned long long phase_ms[9]; /* Wall-clock capture phases, see sl_phase_names. */
    /* Discovery replay: the discovery being recorded, and the recordings:
     * this capture's own, or those of a borrowed memo. */
    sl_replay *recording;
    sl_store own, *store;
    sh_package_source_memo *memo;
    unsigned long long capture;
    /* Diagnostics only (sh_package_source_link_counters). */
    struct {
        size_t units, binds, original_binds, originals, replayed, recorded, declined, reverse_units;
        size_t retained; /* replays of an earlier capture's recording */
        int memo; /* 0 none, 1 kept, 2 renewed for a different key, 3 busy */
        unsigned long long original_bytes;
        unsigned long long select_us, read_us, construct_us; /* discovery sub-phases */
        size_t replay_tries; /* recorded reads validated against a scope */
        unsigned long long applies_us, apply_us; /* within read_us: replay validation, application */
        unsigned long long fresh_us; /* within read_us: reads of units not replayed */
        unsigned long long original_us, identify_us; /* capturing and hashing originals */
        unsigned long long companion_us; /* within select_us: material companion requests */
    } census;
};
static int g_sl_replay = 1;
void sh_package_source_link_replay(int enabled) { g_sl_replay = enabled != 0; }
static int g_sl_fsm = 1;
void sh_package_source_link_fsm(int enabled) { g_sl_fsm = enabled != 0; }
static const char *const sl_phase_names[9] = {
    "readers", "seeds", "reverse roots", "discovery", "shaders", "audio", "certification", "owners", "inputs"
};
static unsigned long long sl_now_ms(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000u + (unsigned long long)now.tv_nsec / 1000000u : 0;
}
static unsigned long long sl_now_us(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000000u + (unsigned long long)now.tv_nsec / 1000u : 0;
}
static void sl_phase(sh_package_source_link *s, int phase, unsigned long long *mark)
{
    unsigned long long now = sl_now_ms();
    s->phase_ms[phase] += now >= *mark ? now - *mark : 0; *mark = now;
}

static uint64_t sl_hash_text(uint64_t hash, const char *text)
{
    for (; *text; text++) { hash ^= (unsigned char)*text; hash *= UINT64_C(1099511628211); }
    return (hash ^ 0xff) * UINT64_C(1099511628211);
}
static uint64_t sl_unit_hash(size_t scope, const char *family, const char *name)
{ return sl_hash_text(sl_hash_text(UINT64_C(14695981039346656037) ^ scope, family), name); }
static int sl_fail(sh_package_source_link *s, const char *reason, const char *name)
{
    s->failed = 1;
    if (s->error && s->error_capacity && !*s->error)
        snprintf(s->error, s->error_capacity, "%s%s%s", reason, name ? ": " : "", name ? name : "");
    return 0;
}
/* I/O, allocation, internal state or configuration: never evidence against
 * the input whose closure was being read. */
static int sl_env_fail(sh_package_source_link *s, const char *reason, const char *name)
{
    s->environment = 1;
    return sl_fail(s, reason, name);
}
/* Continue discovering independent units, but never certify or publish this
 * incomplete frontier. A fresh pass must consume the prepared native output. */
static int sl_defer_preparation(sh_package_source_link *s)
{
    s->preparing = 1;
    if (!*s->preparation_reason) snprintf(s->preparation_reason, sizeof(s->preparation_reason), "%s",
        s->error && *s->error ? s->error : "shader environment requires native preparation");
    if (s->error && s->error_capacity) *s->error = 0;
    return 0;
}
/* Missing product coverage for a source that was read successfully. Ordinary
 * linking still refuses it. Reverse observation of stock roots accounts it as
 * a gap in that root instead; only I/O and allocation failures stay fatal. */
static int sl_gap_fail(sh_package_source_link *s, const char *reason, const char *name)
{
    if (!s->observe) return sl_fail(s, reason, name);
    s->reader_gap = 1;
    if (s->error && s->error_capacity && !*s->error)
        snprintf(s->error, s->error_capacity, "%s%s%s", reason, name ? ": " : "", name ? name : "");
    return 0;
}
static int sl_grow(void ***items, size_t *capacity, size_t count)
{
    if (count < *capacity) return 1;
    size_t next = *capacity ? *capacity * 2 : 32;
    void **grown;
    if (next <= *capacity || next > SIZE_MAX / sizeof(*grown) || !(grown = realloc(*items, next * sizeof(*grown)))) return 0;
    *items = grown; *capacity = next; return 1;
}
static int sl_unit_rehash(sh_package_source_link *s)
{
    size_t next = s->unit_slot_count ? s->unit_slot_count * 2 : 64;
    sl_unit **slots;
    if (next <= s->unit_slot_count || next > SIZE_MAX / sizeof(*slots) || !(slots = calloc(next, sizeof(*slots)))) return 0;
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        size_t at = (size_t)sl_unit_hash(unit->scope, unit->unit.family, unit->unit.name) & (next - 1);
        while (slots[at]) at = (at + 1) & (next - 1);
        slots[at] = unit;
    }
    free(s->unit_slots); s->unit_slots = slots; s->unit_slot_count = next; return 1;
}
static sl_unit *sl_find(sh_package_source_link *s, size_t scope, const char *family, const char *name, int add)
{
    size_t at;
    if (scope >= s->scope_count || !family || !*family || !name || !*name) {
        sl_fail(s, "resource reference has no scoped identity", name); return NULL;
    }
    if (!s->unit_slot_count || (add && s->count >= s->unit_slot_count / 2))
        if (!sl_unit_rehash(s)) goto bad;
    at = (size_t)sl_unit_hash(scope, family, name) & (s->unit_slot_count - 1);
    while (s->unit_slots[at]) {
        sl_unit *unit = s->unit_slots[at];
        if (unit->scope == scope && !strcmp(unit->unit.family, family) && !strcmp(unit->unit.name, name)) return unit;
        at = (at + 1) & (s->unit_slot_count - 1);
    }
    if (!add || s->frozen) return NULL;
    if (!sl_grow((void ***)&s->units, &s->capacity, s->count)) goto bad;
    sl_unit *unit = calloc(1, sizeof(*unit));
    if (!unit) goto bad;
    unit->unit.family = _strdup(family); unit->unit.name = _strdup(name);
    if (!unit->unit.family || !unit->unit.name) {
        free((char *)unit->unit.family); free((char *)unit->unit.name); free(unit); goto bad;
    }
    unit->scope = scope; unit->unit.compilation = s->scopes[scope];
    unit->parent = s->discovering && s->discovering->scope == scope ? s->discovering : NULL;
    if (!unit->parent) {
        /* Diagnostics only: a failed copy leaves the reason unknown. */
        char note[400];
        if (s->discovering)
            snprintf(note, sizeof(note), "requested from scope %zu by %s %s", s->discovering->scope,
                s->discovering->unit.family, s->discovering->unit.name);
        else snprintf(note, sizeof(note), "%s", s->seed_note[0] ? s->seed_note : "unrecorded seed");
        unit->seed = _strdup(note);
    }
    unit->index = s->count; s->units[s->count++] = unit; s->unit_slots[at] = unit; return unit;
bad:
    sl_env_fail(s, "cannot retain scoped resource identity", name); return NULL;
}
static int sl_original_rehash(sh_package_source_link *s)
{
    size_t next = s->original_slot_count ? s->original_slot_count * 2 : 64;
    sl_original **slots;
    if (next <= s->original_slot_count || next > SIZE_MAX / sizeof(*slots) || !(slots = calloc(next, sizeof(*slots)))) return 0;
    for (size_t i = 0; i < s->original_count; i++) {
        sl_original *file = s->originals[i];
        size_t at = (size_t)sl_hash_text(0, file->resource.engine_path) & (next - 1);
        while (slots[at]) at = (at + 1) & (next - 1);
        slots[at] = file;
    }
    free(s->original_slots); s->original_slots = slots; s->original_slot_count = next; return 1;
}
static sl_original *sl_original_get(sh_package_source_link *s, const char *path)
{
    size_t at;
    if (!s->original_slot_count || (!s->frozen && s->original_count >= s->original_slot_count / 2))
        if (!sl_original_rehash(s)) goto bad;
    at = (size_t)sl_hash_text(0, path) & (s->original_slot_count - 1);
    while (s->original_slots[at]) {
        sl_original *file = s->original_slots[at];
        if (!strcmp(file->resource.engine_path, path)) return file;
        at = (at + 1) & (s->original_slot_count - 1);
    }
    if (s->frozen) { sl_env_fail(s, "source binding requested an uncaptured original", path); return NULL; }
    if (!sl_grow((void ***)&s->originals, &s->original_capacity, s->original_count)) goto bad;
    sl_original *file = calloc(1, sizeof(*file));
    if (!file) goto bad;
    file->resource.engine_path = _strdup(path); file->resource.source = SIZE_MAX;
    if (!file->resource.engine_path) { free(file); goto bad; }
    unsigned long long reading = sl_now_us();
    file->status = s->original ? s->original(s->original_context, path,
        &file->resource.body, &file->resource.body_length) : 0;
    s->census.original_us += sl_now_us() - reading;
    s->census.originals++;
    if (file->status > 0 && file->resource.body) s->census.original_bytes += file->resource.body_length;
    s->originals[s->original_count++] = file; s->original_slots[at] = file;
    if (file->status < 0 || file->status > 3 || (file->status && !file->resource.body) ||
        file->resource.body_length == SIZE_MAX) {
        file->status = -1; sl_env_fail(s, "original resource could not be captured", path);
    } else if (!file->status) {
        free(file->resource.body); file->resource.body = NULL; file->resource.body_length = 0;
    }
    file->resource.baseline_known = file->status > 0 ? file->status : 0;
    file->resource.native_original = file->status == 1;
    return file;
bad:
    sl_env_fail(s, "cannot allocate original resource snapshot", path); return NULL;
}
/* The captured original of path, without capturing one. */
static sl_original *sl_original_peek(const sh_package_source_link *s, const char *path)
{
    if (!s->original_slot_count || !path) return NULL;
    size_t at = (size_t)sl_hash_text(0, path) & (s->original_slot_count - 1);
    while (s->original_slots[at]) {
        sl_original *file = s->original_slots[at];
        if (!strcmp(file->resource.engine_path, path)) return file;
        at = (at + 1) & (s->original_slot_count - 1);
    }
    return NULL;
}
static int sl_read_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    sh_package_source_link *s = context;
    sl_original *file = sl_original_get(s, path);
    *body = NULL; *length = 0;
    if (!file || file->status < 0) return -1;
    if (!file->status) return 0;
    *body = malloc(file->resource.body_length + 1);
    if (!*body) { sl_env_fail(s, "cannot copy captured original", path); return -1; }
    memcpy(*body, file->resource.body, file->resource.body_length); (*body)[file->resource.body_length] = 0;
    *length = file->resource.body_length; return file->status;
}
/* Diagnostics only: keep the first reason a unit's binding is not original. */
static void sl_note_original(sl_unit *unit, const char *note)
{
    if (!unit->original_note) unit->original_note = _strdup(note);
}
static int sl_member(sh_package_source_link *s, sl_unit *unit, const sh_compiled_resource *file)
{
    for (size_t i = 0; i < unit->unit.file_count; i++)
        if (!strcmp(unit->unit.files[i]->engine_path, file->engine_path)) return 1;
    if (!sl_grow((void ***)&unit->unit.files, &unit->capacity, unit->unit.file_count))
        return sl_env_fail(s, "cannot retain native resource permutations", unit->unit.name);
    ((const sh_compiled_resource **)unit->unit.files)[unit->unit.file_count++] = file; return 1;
}
/* Capture availability separately from membership. Optional consumers may
 * omit a campaign-only original, but never substitute a peer package or turn
 * an I/O failure into absence. The original snapshot retains either result. */
static int sl_source_file(sh_package_source_link *s, size_t scope_index,
    const char *path, int optional, const sh_compiled_resource **out)
{
    const sh_package_compilation *scope = s->scopes[scope_index];
    char *key = scope->canonical_path ? scope->canonical_path(path) : sh_package_engine_path(path);
    const sh_compiled_resource *file;
    int result = -1;
    *out = NULL;
    if (!key) { sl_fail(s, "invalid native dependency path", path); return -1; }
    file = sh_package_compilation_find(scope, key);
    if (file) { *out = file; result = 1; }
    else {
        sl_original *original = sl_original_get(s, key);
        if (original && original->status == 2) {
            /* Campaign-only data is absent from SnapMap. Reverse observation
             * treats that like any proven absence: a boundary, not admission. */
            if (optional || s->observe) result = 0;
            else sl_fail(s, "campaign dependency is absent from its authored scope", key);
        } else if (original && original->status > 0) { *out = &original->resource; result = 1; }
        else if (original && !original->status) result = 0;
    }
    free(key); return result;
}
/* 1 supplied, 0 proven absent, -1 failed. No peer compilation is consulted. */
static int sl_path(sh_package_source_link *s, sl_unit *unit, const char *path)
{
    const sh_compiled_resource *file;
    int status = sl_source_file(s, unit->scope, path, 0, &file);
    return status == 1 ? (sl_member(s, unit, file) ? 1 : -1) : status;
}
static char *sl_decl_path(const char *family, const char *name)
{
    size_t a = strlen(family), b = strlen(name);
    char *path;
    if (a > SIZE_MAX - 24 || b > SIZE_MAX - 24 - a || !(path = malloc(a + b + 24))) return NULL;
    snprintf(path, a + b + 24, "generated/decls/%s/%s.decl", family, name); return path;
}
static size_t sl_shader_program_add(sh_package_source_link *s, sl_unit *unit,
    const char *name, size_t parent, uint64_t mask)
{
    for (size_t i = 0; i < unit->program_count; i++) if (!strcmp(unit->programs[i].name, name)) return i;
    if (unit->program_count >= SIZE_MAX / sizeof(*unit->programs)) return SIZE_MAX;
    char *copy = _strdup(name);
    sl_shader_program *grown = copy ? realloc(unit->programs, (unit->program_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); sl_env_fail(s, "cannot retain shader permutation identities", name); return SIZE_MAX; }
    unit->programs = grown;
    unit->programs[unit->program_count] = (sl_shader_program){copy, parent, mask, {0}};
    return unit->program_count++;
}
typedef struct sl_shader_collect {
    sh_package_source_link *source;
    sl_unit *unit;
    size_t program, permutation;
    const sh_package_shader_program *decoded;
    const unsigned char *binary;
    int probe, declaration, cooked;
} sl_shader_collect;
static int sl_shader_collect_path(void *context, sh_package_shader_path_kind kind,
    const char *original, const char *linked)
{
    sl_shader_collect *walk = context; sh_package_source_link *s = walk->source; sl_unit *unit = walk->unit;
    (void)linked;
    if (kind == SH_PACKAGE_SHADER_PERMUTATION) {
        if (walk->permutation >= walk->decoded->permutation_count) return 0;
        return sl_shader_program_add(s, unit, original, walk->program,
            walk->decoded->permutations[walk->permutation++]) != SIZE_MAX;
    }
    int status = sl_path(s, unit, original);
    if (walk->probe) {
        if (kind == SH_PACKAGE_SHADER_DECLARATION) walk->declaration = status;
        if (kind == SH_PACKAGE_SHADER_BINARY) walk->cooked = status;
        return status >= 0;
    }
    if (status != 1) return status < 0 ? 0 : sl_gap_fail(s, "required shader companion is absent", original);
    const sh_compiled_resource *file = NULL;
    for (size_t i = 0; i < unit->unit.file_count; i++) if (!strcmp(unit->unit.files[i]->engine_path, original)) {
        file = unit->unit.files[i]; break;
    }
    if (!file) return sl_fail(s, "shader companion was not retained", original);
    if (kind == SH_PACKAGE_SHADER_SPIRV) {
        size_t length = 0; unsigned char *body = sh_package_compilation_read(unit->unit.compilation, file,
            SIZE_MAX, &length, s->error, s->error_capacity);
        const char *extension = strrchr(original, '.');
        size_t stage = extension && !strcmp(extension, ".vspv") ? 0 :
            extension && !strcmp(extension, ".fspv") ? 1 : 2;
        const sh_package_shader_stage *source = walk->decoded->stages + stage;
        int ok = body && sh_package_shader_spirv(body, length, walk->binary + source->offset,
            source->length, s->error, s->error_capacity) == SH_PACKAGE_SHADER_OK;
        free(body); if (!ok) return 0;
    }
    for (size_t i = 0; i < unit->shader_file_count; i++) if (unit->shader_files[i].file == file) return 1;
    if (unit->shader_file_count >= SIZE_MAX / sizeof(*unit->shader_files)) return 0;
    sl_shader_file *grown = realloc(unit->shader_files, (unit->shader_file_count + 1) * sizeof(*grown));
    if (!grown) return sl_env_fail(s, "cannot retain shader file roles", original);
    unit->shader_files = grown;
    unit->shader_files[unit->shader_file_count++] = (sl_shader_file){file, walk->program, kind}; return 1;
}
static int sl_shader_files(sh_package_source_link *s, sl_unit *unit)
{
    sh_package_shader_program empty = {0};
    sl_shader_collect probe = {s, unit, 0, 0, &empty, NULL, 1, 0, 0};
    if (!s->native.shader) return sl_env_fail(s, "shader unit requires the current renderer and fixed ABI contract", unit->unit.name);
    /* The selected renderer's companions define publication. Supplied files for
     * other renderers remain in their intact source bundle, never substituted. */
    unit->unit.file_count = 0;
    if (sh_package_shader_paths(s->native.shader->renderer, unit->unit.name, unit->unit.name, &empty,
        sl_shader_collect_path, &probe, s->error, s->error_capacity) != SH_PACKAGE_SHADER_OK) return 0;
    if (!probe.declaration && !probe.cooked) {
        /* A generic declaration pointer offers every manager as a candidate;
         * like an absent declaration, an absent program is only a boundary. */
        if (s->observe) return 1;
        int status = s->native.admit ? s->native.admit(s->native.context, unit->scope,
            unit->unit.family, unit->unit.name, unit->unit.semantics, s->error, s->error_capacity) : 0;
        if (status != 1) return sl_gap_fail(s, "source-free shader has no verified native admission", unit->unit.name);
        unit->unit.original_binding = 1; return 1;
    }
    if (probe.declaration == 1 && !probe.cooked && s->native.shader->program) {
        unsigned char *declaration = NULL, *binary = NULL; size_t declaration_length = 0, binary_length = 0;
        sh_package_shader_program decoded = {0};
        sh_package_source_shader_status status = sh_package_source_shader_open(s->readers[unit->scope], unit->unit.name,
            &declaration, &declaration_length, &binary, &binary_length, &decoded, s->error, s->error_capacity);
        free(declaration); free(binary); sh_package_shader_program_clear(&decoded);
        if (status == SH_PACKAGE_SOURCE_SHADER_PREPARING) return sl_defer_preparation(s);
    }
    if (probe.declaration != 1 || probe.cooked != 1)
        return sl_gap_fail(s, "shader declaration and selected renderer binary must both be supplied", unit->unit.name);
    if (sl_shader_program_add(s, unit, unit->unit.name, SIZE_MAX, 0) == SIZE_MAX) return 0;
    for (size_t p = 0; p < unit->program_count; p++) {
        unsigned char *declaration = NULL, *binary = NULL; size_t declaration_length = 0, binary_length = 0;
        sh_package_shader_program decoded = {0};
        const char *name = unit->programs[p].name;
        sh_package_source_shader_status status = sh_package_source_shader_open(s->readers[unit->scope], name,
            &declaration, &declaration_length, &binary, &binary_length, &decoded, s->error, s->error_capacity);
        if (status == SH_PACKAGE_SOURCE_SHADER_PREPARING) return sl_defer_preparation(s);
        if (status != SH_PACKAGE_SOURCE_SHADER_READY) return 0;
        sl_shader_collect walk = {s, unit, p, 0, &decoded, binary, 0, 0, 0};
        int ok = sh_package_shader_paths(s->native.shader->renderer, name, name, &decoded,
            sl_shader_collect_path, &walk, s->error, s->error_capacity) == SH_PACKAGE_SHADER_OK;
        free(declaration); free(binary);
        if (!ok) { sh_package_shader_program_clear(&decoded); return 0; }
        unit->programs[p].decoded = decoded;
    }
    return 1;
}
typedef struct sl_material_members { sh_package_source_link *source; sl_unit *unit; } sl_material_members;
static int sl_material_member(void *context, const char *role, const sh_compiled_resource *file)
{
    sl_material_members *walk = context; sh_package_link_unit *unit = &walk->unit->unit;
    if (!role || !*role || !file || !file->engine_path ||
        unit->witness_count >= SIZE_MAX / sizeof(*unit->witnesses))
        return sl_fail(walk->source, "invalid material companion witness", role);
    for (size_t i = 0; i < unit->witness_count; i++) if (!strcmp(role, unit->witnesses[i].role))
        return sl_fail(walk->source, "material companion repeats a native role", role);
    char *copy = _strdup(role);
    sh_package_link_witness *grown = copy ? realloc((void *)unit->witnesses,
        (unit->witness_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return sl_env_fail(walk->source, "cannot retain material companions", role); }
    unit->witnesses = grown; grown[unit->witness_count++] = (sh_package_link_witness){copy, file}; return 1;
}
static int sl_movie_files(sh_package_source_link *s, sl_unit *unit);
static unsigned char *sl_movie_bind(sl_unit *unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_binding_resolver reference, void *context,
    size_t *output_length, char *error, size_t capacity);
static int sl_files(sh_package_source_link *s, sl_unit *unit)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t count;
    if (!strcmp(unit->unit.family, "renderprog")) return sl_shader_files(s, unit);
    if (sh_package_movie_owner(unit->unit.family, unit->unit.name)) return sl_movie_files(s, unit);
    if (!strcmp(unit->unit.family, "font")) {
        /* Native selection is between these forms, including an empty selected
         * file's 12.dat fallback. Catalog order is not a selection policy. */
        for (unsigned i = 0; i < 3; i++) {
            sh_font_binary_paths paths;
            const sh_compiled_resource *file;
            if (!sh_font_binary_paths_for(unit->unit.name, (sh_font_binary_variant)i, &paths))
                return sl_env_fail(s, "cannot construct font source paths", unit->unit.name);
            int status = sl_source_file(s, unit->scope, paths.file, 1, &file);
            sh_font_binary_paths_clear(&paths);
            if (status < 0 || (status == 1 && !sl_member(s, unit, file))) return 0;
        }
        return unit->unit.file_count || s->observe || sl_fail(s, "font has no captured glyph source", unit->unit.name);
    }
    /* Supplied declarations already have a verified source identity. Native
     * cooked cache paths are not additional declaration text envelopes. */
    if (unit->unit.file_count && unit->unit.files[0]->type) goto companions;
    count = sh_resource_catalog_find(s->catalog, unit->unit.family, unit->unit.name, &rows);
    if (count) {
        for (size_t i = 0; i < count; i++) if (*rows[i]->path) {
            int status = sl_path(s, unit, rows[i]->path);
            if (status != 1 && !(s->observe && status == 0))
                return status < 0 ? 0 : sl_fail(s, "native resource permutation is absent", rows[i]->path);
        }
    } else if (!unit->unit.file_count) {
        char *path = sh_package_cooked_path(unit->unit.family, unit->unit.name);
        if (!path) path = sl_decl_path(unit->unit.family, unit->unit.name);
        if (!path) return sl_env_fail(s, "cannot construct native source path", unit->unit.name);
        int status = sl_path(s, unit, path); free(path);
        if (status < 0) return 0;
    }
    if (!unit->unit.file_count) {
        if (s->observe) return 1;
        /* SnapMap registers some identities with no data. Loading one reports
         * "No data" through the non-fatal load reporter and defaults it (MD6
         * anim, skeleton and mesh loaders, Vulkan 0x1414865e0 / 0x1414ab4b0 /
         * 0x14149c950); stock declarations name such rows and load normally.
         * The registered name is the whole identity: nothing to deliver. */
        int registered = 0, status;
        for (size_t i = 0; i < count; i++) registered |= !*rows[i]->path && rows[i]->archive < 2;
        if (registered) {
            char identity[512]; sh_package_file_identity digest;
            int n = snprintf(identity, sizeof(identity), "doom2016:engine-registered-fileless:%s:%s",
                unit->unit.family, unit->unit.name);
            status = n > 0 && (size_t)n < sizeof(identity) &&
                sh_package_bytes_identity(identity, (size_t)n, &digest) ? 1 : -1;
            if (status == 1) memcpy(unit->unit.semantics, digest.digest, 32);
        } else status = s->native.admit ? s->native.admit(s->native.context, unit->scope,
            unit->unit.family, unit->unit.name, unit->unit.semantics, s->error, s->error_capacity) : 0;
        if (status != 1) return sl_fail(s, "resource has no admitted native source", unit->unit.name);
        unit->unit.original_binding = 1;
    }
companions:
    if (unit->unit.file_count && !unit->material_members && !strcmp(unit->unit.family, "material") && s->native.material_companions) {
        sl_material_members walk = {s, unit};
        unsigned long long asking = sl_now_us();
        int ok = s->native.material_companions(s->native.context, unit->scope, unit->unit.name,
            sl_material_member, &walk, s->error, s->error_capacity);
        s->census.companion_us += sl_now_us() - asking;
        if (ok) unit->material_members = 1;
        return ok;
    }
    return 1;
}

static unsigned char *sl_bind_member(sh_package_source_link *s, sl_unit *unit,
    const sh_compiled_resource *file, const unsigned char *body, size_t length,
    sh_package_binding_resolver reference, void *context, size_t *output_length,
    char *error, size_t capacity)
{
    if (sh_package_movie_owner(unit->unit.family, unit->unit.name))
        return sl_movie_bind(unit, file, body, length, reference, context, output_length, error, capacity);
    if (!strcmp(unit->unit.family, "font")) {
        /* Name-derived references belong to the complete constructor plan,
         * not to offsets in glyph bytes. Empty members are native fallbacks. */
        if ((length && !sh_font_binary_read(body, length, NULL, error, capacity)) || length == SIZE_MAX) return NULL;
        unsigned char *out = malloc(length + 1);
        if (out) { memcpy(out, body, length); out[length] = 0; *output_length = length; }
        return out;
    }
    if (!strcmp(unit->unit.family, "renderprog")) {
        for (size_t i = 0; i < unit->shader_file_count; i++) {
            const sl_shader_file *member = unit->shader_files + i;
            if (member->file != file) continue;
            if (member->kind == SH_PACKAGE_SHADER_SPIRV ||
                member->kind == SH_PACKAGE_SHADER_SOURCE_INCLUDE) {
                /* Discovery already validated this companion against its
                 * captured program. It has no relocatable reference sites.
                 * Verify the exact retained member rather than reopening,
                 * decoding and checking every companion of the whole program
                 * once for each stage/include on every identity pass. */
                size_t expected_length = 0;
                unsigned char *expected = sh_package_compilation_read(unit->unit.compilation,
                    file, SIZE_MAX, &expected_length, error, capacity);
                if (!expected) return NULL;
                if (length != expected_length || memcmp(body, expected, length)) {
                    free(expected);
                    snprintf(error, capacity, "shader companion differs from its captured source unit");
                    return NULL;
                }
                *output_length = expected_length; return expected;
            }
            else
                return sh_package_source_shader_bind(s->readers[unit->scope],
                    unit->programs[member->program].name, file->engine_path,
                    body, length, reference, context, output_length, error, capacity);
        }
        snprintf(error, capacity, "file is outside its captured shader unit"); return NULL;
    }
    sh_package_source_binding result;
    unsigned char *out = (s->observe ? sh_package_source_observe : sh_package_source_bind)(s->readers[unit->scope],
        unit->unit.family, unit->unit.name, body, length, reference, context, output_length, &result, error, capacity);
    if (!out && result.malformed) s->malformed = 1;
    return out;
}

/* Physical shader paths supply seeds; the selected renderer's verified reader
 * later accounts for all companions. Catalog family labels cannot substitute
 * for paths: some renderparm files are catalogued beneath their owning shader. */
static int sl_shader_seed(const sh_compiled_resource *file, char **name)
{
    const char *path = file->engine_path, *start = NULL; size_t length = strlen(path), suffix = 0;
    *name = NULL;
    if (!strncmp(path, "generated/decls/renderprog/", 27) && length > 32 && !strcmp(path + length - 5, ".decl")) {
        start = path + 27; suffix = 5;
    } else if (!strncmp(path, "generated/renderprogs/", 22)) {
        if (length > 14 && !strcmp(path + length - 14, "_pc_vulkan.bin")) suffix = 14;
        else if (length > 7 && !strcmp(path + length - 7, "_pc.bin")) suffix = 7;
        if (suffix && strncmp(path + 22, "permutations/", 13)) start = path + 22;
        else return 1;
    } else if (!strncmp(path, "generated/spirv/", 16) || !strncmp(path, "decls/renderprogs/", 18) ||
        !strncmp(path, "generated/glsl/", 15) || !strncmp(path, "generated/glslbinaries/", 23)) return 1;
    else return 0;
    size_t n = length - (size_t)(start - path) - suffix;
    if (!n || !(*name = malloc(n + 1))) return -1;
    memcpy(*name, start, n); (*name)[n] = 0; return 1;
}
static size_t sl_font_stem(const char *path)
{
    static const char *const suffixes[] = {"/48.dat", "/64_df.dat", "/12.dat"};
    if (strncmp(path, "fonts/", 6)) return 0;
    size_t n = strlen(path);
    for (unsigned i = 0; i < 3; i++) {
        size_t suffix = strlen(suffixes[i]);
        if (n > 6 + suffix && !strcmp(path + n - suffix, suffixes[i])) return n - 6 - suffix;
    }
    return 0;
}
static const char *sl_deferred_binary_family(sh_package_source_link *s, const sh_compiled_resource *file)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t n = strlen(file->engine_path);
    if (file->type || n <= 7 ||
        sh_resource_catalog_find_path(s->catalog, file->engine_path, &rows)) return NULL;
    if (sl_font_stem(file->engine_path)) return "font";
    if (!strcmp(file->engine_path + n - 7, ".bimage"))
        return !strncmp(file->engine_path, "generated/transsortatlas/", 25) ? "transsortatlas" : "image";
    if (!strcmp(file->engine_path + n - 7, ".dmodel")) return "discreteanimation";
    if (n >= 17 && !strncmp(file->engine_path, "generated/", 10) &&
        !strcmp(file->engine_path + n - 7, ".pmodel")) return "staticparticlemodel";
    return NULL;
}
typedef struct sl_material_seed { sh_package_source_link *source; size_t scope; } sl_material_seed;
static int sl_seed_family(sl_material_seed *walk, const char *family, const char *name)
{
    sl_unit *unit = sl_find(walk->source, walk->scope, family, name, 1);
    if (unit && sl_files(walk->source, unit)) return 1;
    if (unit) walk->source->failed_unit = unit;
    return 0;
}
static int sl_seed_material(void *context, const char *name)
{ return sl_seed_family(context, "material", name); }
static int sl_seed_sound(void *context, const char *name)
{ return sl_seed_family(context, "sound", name); }
/* A byte-identical SnapMap include contributes no change to catalogued programs.
 * Keep it in the authored delivery and bind it normally when a selected program
 * reads it, but do not expand the entire stock shader library just for its copy. */
static int sl_shader_stock_include(sh_package_source_link *s, size_t scope,
    const sh_compiled_resource *file)
{
    if (strncmp(file->engine_path, "decls/renderprogs/", 18)) return 0;
    sl_original *original = sl_original_get(s, file->engine_path);
    if (!original || original->status < 0) return -1;
    if (original->status != 1) return 0;
    size_t length = 0;
    unsigned char *body = sh_package_compilation_read(s->scopes[scope], file, SIZE_MAX,
        &length, s->error, s->error_capacity);
    if (!body) { sl_env_fail(s, "shader include could not be read", file->engine_path); return -1; }
    int same = length == original->resource.body_length &&
        !memcmp(body, original->resource.body, length);
    free(body); return same;
}
static int sl_seed_inputs(sh_package_source_link *s)
{
    for (size_t scope = 0; scope < s->scope_count; scope++) {
        const sh_package_compilation *original = s->scopes[scope];
        sh_package_compilation filtered = *original; sh_package_link_inputs roots = {0};
        filtered.resources = calloc(original->resource_count ? original->resource_count : 1, sizeof(*filtered.resources));
        filtered.resource_count = 0;
        if (!filtered.resources) return sl_env_fail(s, "cannot enumerate shader source inputs", NULL);
        int ok = 1;
        for (size_t i = 0; ok && i < original->resource_count; i++) {
            const sh_compiled_resource *file = original->resources + i; char *name = NULL;
            if (s->native.audio && sh_package_audio_path_kind(file->engine_path)) continue;
            /* A cooked movie and its atlas are one constructor unit, whichever
             * member this scope supplies; the other resolves in scope then originals. */
            sh_package_movie_names movie;
            if (sh_package_movie_names_for_path(file->engine_path, &movie) == 1) {
                sl_unit *unit = sl_find(s, scope, "file", movie.movie, 1);
                if (!unit || !sl_member(s, unit, file)) { ok = 0; break; }
                continue;
            }
            /* A typed consumer can name this file through another native
             * spelling. Give that exact identity first ownership of the file. */
            if (sl_deferred_binary_family(s, file)) continue;
            int auxiliary = sh_package_virtual_material_auxiliary(original, file, s->error, s->error_capacity);
            if (auxiliary < 0) { ok = 0; break; }
            if (auxiliary) {
                if (!s->native.material_companions) ok = sl_fail(s, "virtual texture source has no material binding adapter", file->engine_path);
                continue;
            }
            int shader = sl_shader_seed(file, &name);
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu shader file %s", scope, file->engine_path);
            if (shader < 0) ok = 0;
            else if (name) ok = sl_find(s, scope, "renderprog", name, 1) != NULL;
            else if (!shader) filtered.resources[filtered.resource_count++] = *file;
            else {
                int stock = sl_shader_stock_include(s, scope, file);
                if (stock < 0) { ok = 0; break; }
                if (stock) continue;
                /* A shared shader source reaches the SnapMap programs catalogued
                 * beneath it. Campaign-only programs are not SnapMap resources;
                 * a scope that needs one supplies its declaration, which seeds it. */
                const sh_resource_catalog_entry *const *rows = NULL;
                size_t count = sh_resource_catalog_find_path(s->catalog, file->engine_path, &rows);
                for (size_t r = 0; ok && r < count; r++)
                    if (rows[r]->archive < 2 && !strcmp(rows[r]->type, "renderprog") && *rows[r]->name)
                        ok = sl_find(s, scope, "renderprog", rows[r]->name, 1) != NULL;
            }
            free(name);
        }
        const sh_package_compilation *one = &filtered;
        if (ok) ok = sh_package_link_inputs_open(&one, 1, s->catalog, &roots, s->error, s->error_capacity);
        for (size_t i = 0; ok && i < roots.count; i++) {
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu supplied %s", scope,
                roots.units[i].file_count ? roots.units[i].files[0]->engine_path : roots.units[i].name);
            sl_unit *unit = sl_find(s, scope, roots.units[i].family, roots.units[i].name, 1);
            if (!unit) { ok = 0; break; }
            for (size_t f = 0; ok && f < roots.units[i].file_count; f++) {
                const sh_compiled_resource *file = sh_package_compilation_find(original, roots.units[i].files[f]->engine_path);
                ok = file && sl_member(s, unit, file);
            }
        }
        sh_package_link_inputs_free(&roots); free(filtered.resources);
        if (ok && s->native.material_seeds) {
            sl_material_seed walk = {s, scope};
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu material companion", scope);
            ok = s->native.material_seeds(s->native.context, scope, sl_seed_material, &walk,
                s->error, s->error_capacity);
        }
        if (ok && s->native.audio) {
            sl_material_seed walk = {s, scope};
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu audio program", scope);
            ok = sh_package_audio_program_seed(s->native.audio, scope, s->catalog, sl_seed_sound, &walk,
                s->error, s->error_capacity);
        }
        if (!ok) { s->failed_scope = scope; return 0; }
    }
    return 1;
}
static int sl_seed_orphan_binaries(sh_package_source_link *s)
{
    for (size_t scope = 0; scope < s->scope_count; scope++) {
        const sh_package_compilation *compiled = s->scopes[scope];
        for (size_t f = 0; f < compiled->resource_count; f++) {
            const sh_compiled_resource *file = compiled->resources + f;
            const char *family = sl_deferred_binary_family(s, file);
            if (!family) continue;
            snprintf(s->seed_note, sizeof(s->seed_note), "scope %zu unowned binary %s", scope, file->engine_path);
            int owned = 0;
            for (size_t i = 0; !owned && i < s->count; i++) if (s->units[i]->scope == scope)
                for (size_t j = 0; j < s->units[i]->unit.file_count; j++)
                    if (s->units[i]->unit.files[j] == file) { owned = 1; break; }
            if (owned) continue;
            const char *inverse_family = NULL; char *name = NULL;
            int inverse;
            if (!strcmp(family, "font")) {
                size_t n = sl_font_stem(file->engine_path);
                name = malloc(n + 1);
                if (name) { memcpy(name, file->engine_path + 6, n); name[n] = 0; }
                inverse_family = family; inverse = name ? 1 : -1;
            } else inverse = sh_package_cooked_identity(file->engine_path, &inverse_family, &name);
            if (inverse != 1 ||
                !inverse_family || strcmp(inverse_family, family)) {
                free(name); s->failed_scope = scope;
                return sl_fail(s, "unreferenced binary has no native identity", file->engine_path);
            }
            sl_unit *unit = sl_find(s, scope, family, name, 1); free(name);
            if (!unit || !sl_member(s, unit, file)) { s->failed_scope = scope; return 0; }
        }
    }
    return 1;
}
static int sl_shader_binary_path(void *context, sh_package_shader_path_kind kind,
    const char *original, const char *linked)
{
    (void)linked;
    if (kind == SH_PACKAGE_SHADER_BINARY) snprintf(context, 256, "%s", original);
    return 1;
}
/* The other renderer can generate a different permutation set from the same
 * root (for example, platform extension defines). Its unselected declaration
 * remains delivery data alongside its renderer-specific binary. This only
 * accounts the dormant pair; it never certifies or publishes that program.
 * Selected permutations have already passed complete discovery above. */
static int sl_shader_inactive_source(sh_package_source_link *s, size_t scope,
    const sh_compiled_resource *file)
{
    const char *path = file->engine_path;
    size_t n = strlen(path);
    if (!s->native.shader || strncmp(path, "generated/renderprogs/permutations/", 35) ||
        n <= 40 || strcmp(path + n - 5, ".decl")) return 0;
    char binary_path[256] = ""; sh_package_shader_program empty = {0};
    sh_package_shader_renderer other = s->native.shader->renderer == SH_PACKAGE_SHADER_OPENGL ?
        SH_PACKAGE_SHADER_VULKAN : SH_PACKAGE_SHADER_OPENGL;
    if (sh_package_shader_paths(other, path, path, &empty, sl_shader_binary_path, binary_path,
        s->error, s->error_capacity) != SH_PACKAGE_SHADER_OK) return -1;
    const sh_compiled_resource *binary_file = NULL;
    int status = sl_source_file(s, scope, binary_path, 1, &binary_file);
    if (status != 1) return status;
    size_t declaration_length = 0, binary_length = 0;
    unsigned char *declaration = sh_package_compilation_read(s->scopes[scope], file, SIZE_MAX,
        &declaration_length, s->error, s->error_capacity);
    unsigned char *binary = declaration ? sh_package_compilation_read(s->scopes[scope], binary_file, SIZE_MAX,
        &binary_length, s->error, s->error_capacity) : NULL;
    int paired = 0;
    if (!declaration || !binary) { sl_env_fail(s, "inactive shader pair could not be read", path); paired = -1; }
    else if (binary_length >= 8 && !memcmp(binary, "\xb6\x88\x00\xad", 4)) {
        uint32_t checksum = (uint32_t)binary[4] << 24 | (uint32_t)binary[5] << 16 |
            (uint32_t)binary[6] << 8 | binary[7];
        paired = checksum == sh_package_shader_declaration_checksum(declaration, declaration_length);
    }
    free(declaration); free(binary); return paired;
}
static int sl_shader_accounted(sh_package_source_link *s)
{
    for (size_t scope = 0; scope < s->scope_count; scope++) for (size_t f = 0; f < s->scopes[scope]->resource_count; f++) {
        const sh_compiled_resource *file = s->scopes[scope]->resources + f;
        const char *path = file->engine_path; char *name = NULL;
        int shader = sl_shader_seed(file, &name); free(name);
        if (shader < 0) return 0;
        if (!shader) continue;
        /* The other renderer and driver cache/debug files remain intact source
         * artifacts; only the chosen renderer has a publication obligation. */
        size_t n = strlen(path);
        if (!strncmp(path, "generated/glsl/", 15) || !strncmp(path, "generated/glslbinaries/", 23) ||
            (s->native.shader && s->native.shader->renderer == SH_PACKAGE_SHADER_OPENGL &&
             (!strncmp(path, "generated/spirv/", 16) || (n >= 14 && !strcmp(path + n - 14, "_pc_vulkan.bin")))) ||
            (s->native.shader && s->native.shader->renderer == SH_PACKAGE_SHADER_VULKAN && n >= 7 && !strcmp(path + n - 7, "_pc.bin"))) continue;
        int found = 0;
        for (size_t u = 0; !found && u < s->count; u++) if (s->units[u]->scope == scope)
            for (size_t m = 0; !found && m < s->units[u]->unit.file_count; m++) found = s->units[u]->unit.files[m] == file;
        if (!found) {
            int stock = sl_shader_stock_include(s, scope, file);
            if (stock < 0) { s->failed_scope = scope; return 0; }
            if (stock) continue;
            int dormant = sl_shader_inactive_source(s, scope, file);
            if (dormant < 0) { s->failed_scope = scope; return 0; }
            if (!dormant) { s->failed_scope = scope; return sl_fail(s, "shader source input has no verified companion owner", path); }
        }
    }
    return 1;
}
/* file: the captured file being bound, whose sites are recorded; NULL otherwise. */
typedef struct sl_discover {
    sh_package_source_link *source; sl_unit *unit; const sh_compiled_resource *file;
    sl_replay *record; /* discovery replay: collects this walk's requests in order */
} sl_discover;
/* Record one resolved request of the discovery being recorded. */
static void sl_replay_ref_add(sl_replay *r, const char *slot, const char *family, const char *name, int request)
{
    if (!r || !r->valid) return;
    if (r->ref_count == r->ref_capacity) {
        size_t next = r->ref_capacity ? r->ref_capacity * 2 : 16;
        sl_replay_ref *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(r->refs, next * sizeof(*grown)) : NULL;
        if (!grown) { r->valid = 0; return; }
        r->refs = grown; r->ref_capacity = next;
    }
    sl_replay_ref ref = {_strdup(slot), _strdup(family), _strdup(name), request};
    if (!ref.slot || !ref.family || !ref.name) {
        free(ref.slot); free(ref.family); free(ref.name); r->valid = 0; return;
    }
    r->refs[r->ref_count++] = ref;
}
static int sl_player_hands_add(sh_package_source_link *s, const char *name)
{
    for (size_t i = 0; i < s->player_hand_count; i++) if (!strcmp(s->player_hands[i], name)) return 1;
    char *copy = _strdup(name);
    char **grown = copy && s->player_hand_count < SIZE_MAX / sizeof(*grown) - 1 ?
        realloc(s->player_hands, (s->player_hand_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return 0; }
    s->player_hands = grown; s->player_hands[s->player_hand_count++] = copy; return 1;
}
static int sl_reference(void *context, const char *slot, const char *family, const char *name, const char **replacement)
{
    sl_discover *walk = context;
    sl_unit *unit = walk->unit;
    sl_unit *target = sl_find(walk->source, unit->scope, family, name, 1);
    *replacement = NULL;
    if (!target) return -1;
    if (!sl_grow((void ***)&unit->dependencies, &unit->dependency_capacity, unit->dependency_count)) {
        sl_env_fail(walk->source, "cannot retain scoped dependency", name); return -1;
    }
    unit->dependencies[unit->dependency_count++] = target;
    if (walk->file) {
        char *copy = slot ? _strdup(slot) : NULL;
        if (copy && unit->site_count == unit->site_capacity) {
            size_t next = unit->site_capacity ? unit->site_capacity * 2 : 8;
            sl_site *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(unit->sites, next * sizeof(*grown)) : NULL;
            if (grown) { unit->sites = grown; unit->site_capacity = next; }
            else { free(copy); copy = NULL; }
        }
        if (!copy) { sl_env_fail(walk->source, "cannot retain a bound reference site", name); return -1; }
        unit->sites[unit->site_count++] = (sl_site){walk->file, copy, target};
    }
    if (walk->record) sl_replay_ref_add(walk->record, slot, family, name, 0);
    return 0;
}
static int sl_implicit_add(sl_discover *walk, const char *slot, const char *family, const char *name, int conditional)
{
    sl_unit *unit = walk->unit;
    if (!slot || !*slot) return sl_fail(walk->source, "implicit native request has no semantic slot", name);
    sl_unit *target = sl_find(walk->source, unit->scope, family, name, 1);
    if (!target) return 0;
    for (size_t i = 0; i < unit->implicit_count; i++) if (!strcmp(unit->implicit[i].slot, slot)) {
        if (unit->implicit[i].target != target || unit->implicit[i].conditional != conditional)
            return sl_fail(walk->source, "implicit native slot has contradictory targets", slot);
        if (walk->record) sl_replay_ref_add(walk->record, slot, family, name, 1);
        return 1;
    }
    if (unit->implicit_count >= SIZE_MAX / sizeof(*unit->implicit))
        return sl_env_fail(walk->source, "too many implicit native requests", name);
    char *copy = _strdup(slot);
    sl_implicit *grown = copy ? realloc(unit->implicit, (unit->implicit_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return sl_env_fail(walk->source, "cannot retain implicit native request", name); }
    unit->implicit = grown;
    unit->implicit[unit->implicit_count++] = (sl_implicit){copy, target, conditional};
    if (!sl_grow((void ***)&unit->dependencies, &unit->dependency_capacity, unit->dependency_count))
        return sl_env_fail(walk->source, "cannot retain implicit native dependency", name);
    unit->dependencies[unit->dependency_count++] = target;
    if (walk->record) sl_replay_ref_add(walk->record, slot, family, name, 1);
    return 1;
}
static int sl_implicit_reference(void *context, const char *slot, const char *family, const char *name)
{
    return sl_implicit_add(context, slot, family, name, 0);
}
static int sl_font_material(void *context, const char *slot, const char *material, int optional)
{
    sl_discover *walk = context; sh_package_source_link *s = walk->source;
    char *name = sh_package_engine_path(material);
    char *path = name ? sl_decl_path("material", name) : NULL;
    const sh_compiled_resource *file = NULL;
    int status = path ? sl_source_file(s, walk->unit->scope, path, optional, &file) : -1;
    if (status == 1) {
        sl_unit *target = sl_find(s, walk->unit->scope, "material", name, 1);
        if (!target || !sl_member(s, target, file) || !sl_implicit_reference(walk, slot, "material", name)) status = -1;
    }
    free(path); free(name); return status;
}
static int sl_font_capture(sh_package_source_link *s, sl_unit *unit)
{
    sh_package_font_member members[3] = {0}; size_t count = unit->unit.file_count;
    sl_discover walk = {s, unit}; int ok = 0;
    if (!count && s->observe) return 1;
    if (!count || count > 3) return sl_gap_fail(s, "font has invalid captured glyph forms", unit->unit.name);
    for (size_t i = 0; i < count; i++) {
        members[i].path = unit->unit.files[i]->engine_path;
        members[i].body = sh_package_compilation_read(unit->unit.compilation, unit->unit.files[i],
            SIZE_MAX, &members[i].length, s->error, s->error_capacity);
        if (!members[i].body) goto done;
    }
    /* Exact installed originals keep the engine's own behavior, including a
     * shipped envelope its loader rejects: no request is captured or routed. */
    int native_owned = 1, rejected = 0;
    for (size_t i = 0; i < count; i++) {
        native_owned &= unit->unit.files[i]->native_original != 0;
        rejected |= sh_font_binary_native_rejected(members[i].body, members[i].length);
    }
    if (native_owned && rejected) { ok = 1; goto done; }
    ok = sh_package_font_capture(unit->unit.name, members, count, sl_font_material, &walk, s->error, s->error_capacity);
    if (!ok && !s->failed) s->reader_gap = 1;
done:
    for (size_t i = 0; i < count; i++) free((void *)members[i].body);
    return ok;
}
/* Cooked UI movies. The idFileResource movie and the constructor-derived atlas
 * image form one unit; dictionary fonts, the language resolver's fallback
 * fonts and the fixed GUI materials are constructor requests bound in this
 * scope. All of them are native create-or-default lookups, so a positively
 * absent request is retained as absence rather than borrowed from a peer. */
static int sl_movie_request(void *context, const char *slot, const char *family, const char *name)
{
    sl_discover *walk = context; sh_package_source_link *s = walk->source;
    const sh_compiled_resource *file = NULL;
    sl_unit *target;
    int status;
    if (!strcmp(family, "font")) {
        const sh_compiled_resource *forms[3] = {0}; int present = 0;
        for (unsigned i = 0; i < 3; i++) {
            sh_font_binary_paths paths;
            if (!sh_font_binary_paths_for(name, (sh_font_binary_variant)i, &paths)) {
                sl_env_fail(s, "cannot construct font source paths", name); return -1;
            }
            status = sl_source_file(s, walk->unit->scope, paths.file, 1, forms + i);
            sh_font_binary_paths_clear(&paths);
            if (status < 0) return -1;
            present |= status == 1;
        }
        if (!present) return 0;
        if (!(target = sl_find(s, walk->unit->scope, "font", name, 1))) return -1;
        for (unsigned i = 0; i < 3; i++) if (forms[i] && !sl_member(s, target, forms[i])) return -1;
    } else {
        char *path = sl_decl_path(family, name);
        status = path ? sl_source_file(s, walk->unit->scope, path, 1, &file) : -1;
        free(path);
        if (status != 1) return status;
        if (!(target = sl_find(s, walk->unit->scope, family, name, 1)) || !sl_member(s, target, file)) return -1;
    }
    return sl_implicit_reference(walk, slot, family, name) == 1 ? 1 : -1;
}
static int sl_movie_files(sh_package_source_link *s, sl_unit *unit)
{
    sh_package_movie_names names;
    const sh_compiled_resource *movie = NULL, *atlas = NULL;
    if (sh_package_movie_names_for_path(unit->unit.name, &names) != 1)
        return sl_gap_fail(s, "cooked movie pair cannot be derived from its native name", unit->unit.name);
    int movie_status = sl_source_file(s, unit->scope, names.movie, 0, &movie);
    int atlas_status = movie_status < 0 ? -1 : sl_source_file(s, unit->scope, names.atlas, 0, &atlas);
    if (movie_status < 0 || atlas_status < 0) return 0;
    if (!movie_status || !atlas_status) {
        /* Observation keeps an absent stock pair as a boundary. A delivered
         * half pair cannot construct: the constructor loads both files. */
        if (s->observe && !movie_status && !atlas_status) return 1;
        return sl_gap_fail(s, "cooked movie and its atlas must both be available", unit->unit.name);
    }
    if (!sl_member(s, unit, movie) || !sl_member(s, unit, atlas)) return 0;
    if (unit->movie_captured) return 1;
    size_t length = 0;
    unsigned char *body = sh_package_compilation_read(unit->unit.compilation, movie, SIZE_MAX,
        &length, s->error, s->error_capacity);
    if (!body) return sl_env_fail(s, "cooked movie could not be read", names.movie);
    sl_discover walk = {s, unit};
    int ok = sh_package_movie_capture(body, length, sl_movie_request, &walk, s->error, s->error_capacity);
    free(body);
    if (!ok) return s->failed ? 0 : sl_gap_fail(s, "cooked movie constructor requests are unsupported", unit->unit.name);
    unit->movie_captured = 1; return 1;
}
typedef struct sl_movie_sites { sh_package_binding_resolver reference; void *context; size_t record; } sl_movie_sites;
static int sl_movie_resolve(void *context, const char *family, const char *name, const char **replacement)
{
    sl_movie_sites *sites = context; char slot[48], normalized[256];
    *replacement = NULL;
    if (!sh_package_movie_native_name(name, strlen(name), normalized, sizeof(normalized))) return -1;
    snprintf(slot, sizeof(slot), "movie/dictionary/%zu", sites->record++);
    return sites->reference(sites->context, slot, family, normalized, replacement);
}
static unsigned char *sl_movie_bind(sl_unit *unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_binding_resolver reference, void *context,
    size_t *output_length, char *error, size_t capacity)
{
    sh_package_movie_names names;
    if (sh_package_movie_names_for_path(unit->unit.name, &names) != 1 || !file || !file->engine_path) {
        snprintf(error, capacity, "cooked movie unit lost its native pair"); return NULL;
    }
    if (!strcmp(file->engine_path, names.atlas)) {
        /* The atlas is pixel data; its identity is bound by name derivation. */
        unsigned char *out = length == SIZE_MAX ? NULL : malloc(length + 1);
        if (out) { if (length) memcpy(out, body, length); out[length] = 0; *output_length = length; }
        return out;
    }
    if (strcmp(file->engine_path, names.movie)) {
        snprintf(error, capacity, "file is outside its captured movie unit"); return NULL;
    }
    sl_movie_sites sites = {reference, context, 0}; size_t changed = 0;
    return sh_resource_rebind_with(sh_package_movie_material_sites, NULL, SH_BINDING_IDSTR,
        body, length, sl_movie_resolve, &sites, output_length, &changed, error, capacity);
}
/* Paired native identities follow the frozen, path-sorted member order. */
static int sl_movie_identities(sh_package_source_link *s, sl_unit *unit)
{
    sh_package_movie_names names;
    if (!sh_package_movie_owner(unit->unit.family, unit->unit.name) || !unit->unit.file_count) return 1;
    if (unit->unit.file_count != 2 || sh_package_movie_names_for_path(unit->unit.name, &names) != 1)
        return sl_fail(s, "cooked movie unit does not hold exactly its native pair", unit->unit.name);
    sh_package_link_file_identity *identities = calloc(2, sizeof(*identities));
    char *atlas = _strdup(names.atlas);
    if (!identities || !atlas) { free(identities); free(atlas); return sl_env_fail(s, "cannot retain movie identities", unit->unit.name); }
    for (size_t i = 0; i < 2; i++) {
        const char *path = unit->unit.files[i]->engine_path;
        if (!strcmp(path, names.atlas)) { identities[i].family = "image"; identities[i].name = atlas; atlas = NULL; }
        else if (strcmp(path, names.movie)) {
            free((char *)identities[0].name); free(identities); free(atlas);
            return sl_fail(s, "cooked movie unit holds an unrelated file", path);
        }
    }
    if (atlas) { free(identities); free(atlas); return sl_fail(s, "cooked movie unit lost its atlas", unit->unit.name); }
    if (unit->identities) for (size_t i = 0; i < 2; i++) free((char *)unit->identities[i].name);
    free(unit->identities);
    unit->identities = identities; unit->unit.file_identities = identities; return 1;
}
static int sl_discover_unit_requests(sh_package_source_link *s, sl_unit *unit);
static int sl_discover_unit(sh_package_source_link *s, sl_unit *unit)
{
    sl_unit *previous = s->discovering;
    s->discovering = unit;
    int ok = sl_discover_unit_requests(s, unit);
    s->discovering = previous; return ok;
}
/* ---- Discovery replay ---------------------------------------------------
 * Every package scope rediscovers the stock units its content reaches, most
 * of them unchanged, and a later compile rediscovers every unit again. A
 * reader's discovery of a unit depends only on the unit's files and on the
 * paths it looks up (sh_package_source_reader_lookups: the scope's file,
 * else the one captured original), beyond what the memo key covers. So the
 * reads one discovery made (its native preparation output, reference sites
 * and material requests, in order) are exactly the reads another discovery
 * of the same family and name would make when every one of those files and
 * paths resolves to the same content. Replay then resolves each recorded
 * request in its own scope, as a fresh bind would, and still runs everything
 * scope-dependent itself: file selection, material companions and native
 * constructor requests. Within one capture an unsupplied path is the same
 * captured original everywhere; across captures its identity is compared. */
static void sl_replay_free(sl_replay *r)
{
    if (!r) return;
    for (size_t i = 0; r->files && i < r->file_count; i++) free(r->files[i].path);
    for (size_t i = 0; i < r->lookup_count; i++) free(r->lookups[i].path);
    for (size_t i = 0; i < r->ref_count; i++) { free(r->refs[i].slot); free(r->refs[i].family); free(r->refs[i].name); }
    free(r->lookups); free(r->refs); free(r->file_refs); free(r->files); free(r->prepared);
    free(r->family); free(r->name); free(r);
}
static void sl_lookup(void *context, const char *path, int supplied)
{
    sh_package_source_link *s = context; sl_replay *r = s->recording;
    if (!r || !r->valid) return;
    if (!path) { r->valid = 0; return; }
    for (size_t i = 0; i < r->lookup_count; i++) if (!strcmp(r->lookups[i].path, path)) return;
    if (r->lookup_count == r->lookup_capacity) {
        size_t next = r->lookup_capacity ? r->lookup_capacity * 2 : 8;
        sl_replay_input *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(r->lookups, next * sizeof(*grown)) : NULL;
        if (!grown) { r->valid = 0; return; }
        r->lookups = grown; r->lookup_capacity = next;
    }
    sl_replay_input *input = r->lookups + r->lookup_count;
    memset(input, 0, sizeof(*input));
    if (!(input->path = _strdup(path))) { r->valid = 0; return; }
    input->state = supplied ? -1 : 0; r->lookup_count++;
}
static void sl_store_clear(sl_store *store)
{
    for (size_t i = 0; i < store->capacity; i++)
        for (sl_replay *r = store->slots[i], *next; r; r = next) { next = r->next; sl_replay_free(r); }
    free(store->slots); memset(store, 0, sizeof(*store));
}
static size_t sl_store_slot(sl_replay *const *slots, size_t capacity, const char *family, const char *name)
{
    size_t at = (size_t)sl_hash_text(sl_hash_text(UINT64_C(14695981039346656037), family), name) & (capacity - 1);
    while (slots[at] && (strcmp(slots[at]->family, family) || strcmp(slots[at]->name, name))) at = (at + 1) & (capacity - 1);
    return at;
}
static sl_replay *sl_store_find(const sl_store *store, const sl_unit *unit)
{
    return store && store->capacity ? store->slots[sl_store_slot(store->slots, store->capacity,
        unit->unit.family, unit->unit.name)] : NULL;
}
/* Recordings retained per family and name, newest first; and in all. */
#define SL_REPLAY_VARIANTS 8u
#define SL_REPLAY_RECORDS 262144u
static void sl_store_add(sh_package_source_link *s, sl_replay *r)
{
    sl_store *store = s->store;
    if (store->records >= SL_REPLAY_RECORDS) { sl_replay_free(r); return; } /* Replay is optional. */
    if (store->count + 1 > store->capacity / 2) {
        size_t capacity = store->capacity ? store->capacity * 2 : 1024;
        sl_replay **slots = capacity < SIZE_MAX / sizeof(*slots) ? calloc(capacity, sizeof(*slots)) : NULL;
        if (!slots) { sl_replay_free(r); return; }
        for (size_t i = 0; i < store->capacity; i++) if (store->slots[i])
            slots[sl_store_slot(slots, capacity, store->slots[i]->family, store->slots[i]->name)] = store->slots[i];
        free(store->slots); store->slots = slots; store->capacity = capacity;
    }
    size_t at = sl_store_slot(store->slots, store->capacity, r->family, r->name), kept = 1;
    if (!store->slots[at]) store->count++;
    r->next = store->slots[at]; store->slots[at] = r; store->records++; s->census.recorded++;
    for (sl_replay *p = r; p->next;) {
        if (++kept <= SL_REPLAY_VARIANTS) { p = p->next; continue; }
        sl_replay *drop = p->next; p->next = drop->next; sl_replay_free(drop); store->records--;
    }
}
/* file when the unit's scope compilation supplies it (one of its resources). */
static const sh_compiled_resource *sl_supplied_file(const sh_package_source_link *s, size_t scope,
    const sh_compiled_resource *file)
{
    const sh_package_compilation *compiled = s->scopes[scope];
    uintptr_t at = (uintptr_t)file, first = (uintptr_t)compiled->resources;
    return compiled->resource_count && at >= first &&
        at < first + compiled->resource_count * sizeof(*compiled->resources) ? file : NULL;
}
static int sl_original_identify(sh_package_source_link *s, sl_original *original, sh_package_file_identity *out)
{
    if (!original->identified) {
        unsigned long long hashing = sl_now_us();
        if (!sh_package_bytes_identity(original->resource.body, original->resource.body_length, &original->identity))
            return sl_env_fail(s, "cannot identify captured original", original->resource.engine_path);
        original->identified = 1;
        s->census.identify_us += sl_now_us() - hashing;
    }
    *out = original->identity; return 1;
}
/* How path resolves for a reader of scope in this capture: supplied is the
 * scope's own file, else the captured original (captured now if needed, as
 * the reader itself would). 0 when that original cannot be captured. */
static int sl_input_resolve(sh_package_source_link *s, size_t scope, const char *path,
    const sh_compiled_resource *supplied, int identify, sl_replay_input *out)
{
    memset(&out->identity, 0, sizeof(out->identity));
    if (supplied) {
        out->state = -1;
        return !identify || sh_package_compilation_identity(s->scopes[scope], supplied, &out->identity, NULL, 0);
    }
    sl_original *original = sl_original_get(s, path);
    if (!original || original->status < 0) return 0;
    out->state = original->status;
    return !identify || !original->status || sl_original_identify(s, original, &out->identity);
}
/* A recorded file or path resolves the same way now. This capture's own
 * recordings read the same captured originals, so only supplied content is
 * compared; an earlier capture's are compared in full. */
static int sl_input_same(sh_package_source_link *s, size_t scope, const sl_replay_input *input,
    const sh_compiled_resource *supplied, int same_capture)
{
    sl_replay_input now;
    if ((input->state == -1) != (supplied != NULL)) return 0;
    if (same_capture && !supplied) return 1;
    if (!sl_input_resolve(s, scope, input->path, supplied, 1, &now)) return 0;
    return now.state == input->state && now.identity.length == input->identity.length &&
        !memcmp(now.identity.digest, input->identity.digest, sizeof(now.identity.digest));
}
/* A unit whose every file is its scope's own or a captured original. Shader
 * programs and flags (native shader ABI), fonts and cooked movies keep their
 * own capture paths. */
static int sl_replay_eligible(const sh_package_source_link *s, const sl_unit *unit)
{
    if (!g_sl_replay || s->observe || !s->store || !unit->unit.file_count || unit->preparation_checked ||
        !strcmp(unit->unit.family, "renderprog") || !strcmp(unit->unit.family, "renderprogflag") ||
        !strcmp(unit->unit.family, "font") || sh_package_movie_owner(unit->unit.family, unit->unit.name)) return 0;
    for (size_t i = 0; i < unit->unit.file_count; i++) {
        const sh_compiled_resource *file = unit->unit.files[i];
        const sl_original *original = file ? sl_original_peek(s, file->engine_path) : NULL;
        if (!file || (!sl_supplied_file(s, unit->scope, file) && (!original || &original->resource != file))) return 0;
    }
    return 1;
}
static sl_replay *sl_replay_open(sh_package_source_link *s, const sl_unit *unit)
{
    sl_replay *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->family = _strdup(unit->unit.family); r->name = _strdup(unit->unit.name);
    r->scope = unit->scope; r->capture = s->capture;
    r->files = calloc(unit->unit.file_count, sizeof(*r->files));
    r->file_refs = calloc(unit->unit.file_count, sizeof(*r->file_refs));
    if (!r->family || !r->name || !r->files || !r->file_refs) { sl_replay_free(r); return NULL; }
    r->file_count = unit->unit.file_count;
    for (size_t i = 0; i < r->file_count; i++) {
        const sh_compiled_resource *file = unit->unit.files[i];
        if (!(r->files[i].path = _strdup(file->engine_path))) { sl_replay_free(r); return NULL; }
        r->files[i].state = sl_supplied_file(s, unit->scope, file) ? -1 : sl_original_peek(s, file->engine_path)->status;
    }
    r->valid = 1;
    return r;
}
/* Finish a recording: how every consulted path resolved, and each input's
 * content identity when a memo will keep it for later captures. */
static int sl_replay_complete(sh_package_source_link *s, sl_replay *r)
{
    const sh_package_compilation *scope = s->scopes[r->scope];
    int identify = s->memo != NULL;
    /* Without a memo, recordings serve other scopes of this capture only:
     * one that read supplied content is not kept. */
    for (size_t i = 0; !identify && r->valid && i < r->file_count; i++) if (r->files[i].state == -1) r->valid = 0;
    for (size_t i = 0; !identify && r->valid && i < r->lookup_count; i++) if (r->lookups[i].state == -1) r->valid = 0;
    for (size_t i = 0; r->valid && i < r->file_count; i++) {
        sl_replay_input *input = r->files + i;
        const sh_compiled_resource *supplied = input->state == -1 ? sh_package_compilation_find(scope, input->path) : NULL;
        if ((input->state == -1 && !supplied) || !sl_input_resolve(s, r->scope, input->path, supplied, identify, input)) r->valid = 0;
    }
    for (size_t i = 0; r->valid && i < r->lookup_count; i++) {
        sl_replay_input *input = r->lookups + i;
        const sh_compiled_resource *supplied = input->state == -1 ? sh_package_compilation_find(scope, input->path) : NULL;
        if ((input->state == -1 && !supplied) || !sl_input_resolve(s, r->scope, input->path, supplied, identify, input)) r->valid = 0;
    }
    return r->valid;
}
/* The same files, and every recorded path resolving to the same content. */
static int sl_replay_applies(sh_package_source_link *s, const sl_unit *unit, const sl_replay *r)
{
    const sh_package_compilation *scope = s->scopes[unit->scope];
    int same_capture = r->capture == s->capture;
    if (!r->valid || r->file_count != unit->unit.file_count || (same_capture && r->scope == unit->scope)) return 0;
    for (size_t i = 0; i < r->file_count; i++) {
        const sh_compiled_resource *file = unit->unit.files[i];
        if (strcmp(file->engine_path, r->files[i].path) ||
            !sl_input_same(s, unit->scope, r->files + i, sl_supplied_file(s, unit->scope, file), same_capture)) return 0;
    }
    for (size_t i = 0; i < r->lookup_count; i++)
        if (!sl_input_same(s, unit->scope, r->lookups + i,
            sh_package_compilation_find(scope, r->lookups[i].path), same_capture)) return 0;
    return 1;
}
/* What a memo's recordings depend on besides the paths they validate: the
 * catalog rows readers consult directly, the native managers and reflection
 * they bind through (read afresh), the product's script capture (immutable,
 * owned by the product) and the preparation callback whose output a
 * recording keeps. */
static int sl_memo_key(sh_package_source_link *s, sh_decl_registry_source registry, unsigned char key[32])
{
    struct { char magic[32]; unsigned char catalog[32], native[32]; uint64_t scripts, prepare; } material;
    sh_package_file_identity identity;
    memset(&material, 0, sizeof(material));
    memcpy(material.magic, "snapmap-plus source memo 1", 26);
    if ((s->catalog && !sh_resource_catalog_digest(s->catalog, material.catalog)) ||
        !s->scope_count || !sh_package_source_reader_native_digest(s->readers[0], material.native)) return 0;
    material.scripts = (uint64_t)(uintptr_t)registry.scripts;
    material.prepare = (uint64_t)(uintptr_t)s->native.prepare;
    if (!sh_package_bytes_identity(&material, sizeof(material), &identity)) return 0;
    memcpy(key, identity.digest, 32); return 1;
}
sh_package_source_memo *sh_package_source_memo_open(void)
{
    return calloc(1, sizeof(sh_package_source_memo));
}
void sh_package_source_memo_close(sh_package_source_memo *memo)
{
    if (!memo) return;
    sl_store_clear(&memo->store); free(memo);
}
static int sl_discovered_add(sh_package_source_link *s, sl_unit *unit, const sh_compiled_resource *file)
{
    const sh_compiled_resource **grown = unit->discovered_count < SIZE_MAX / sizeof(*grown) - 1 ?
        realloc((void *)unit->discovered, (unit->discovered_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) return sl_env_fail(s, "cannot retain a discovered file", unit->unit.name);
    unit->discovered = grown; unit->discovered[unit->discovered_count++] = file; return 1;
}
static int sl_replay_apply(sh_package_source_link *s, sl_unit *unit, const sl_replay *r)
{
    if (s->native.prepare && unit->unit.file_count == 1) {
        if (r->was_prepared) {
            unsigned char *body = malloc(r->prepared_length + 1);
            if (!body) return sl_env_fail(s, "cannot copy a prepared source", unit->unit.name);
            memcpy(body, r->prepared, r->prepared_length); body[r->prepared_length] = 0;
            unit->unprepared = unit->unit.files[0];
            unit->prepared = *unit->unprepared;
            unit->prepared.body = body; unit->prepared.body_length = r->prepared_length;
            ((const sh_compiled_resource **)unit->unit.files)[0] = &unit->prepared;
        }
        unit->preparation_checked = 1;
    }
    size_t at = 0;
    for (size_t j = 0; j < unit->unit.file_count; j++) {
        sl_discover walk = {s, unit, unit->unit.files[j], NULL};
        for (; at < r->file_refs[j]; at++) {
            const sl_replay_ref *ref = r->refs + at;
            const char *replacement = NULL;
            if (ref->request ? sl_implicit_reference(&walk, ref->slot, ref->family, ref->name) != 1 :
                sl_reference(&walk, ref->slot, ref->family, ref->name, &replacement) != 0) return 0;
        }
        if (!sl_discovered_add(s, unit, walk.file)) return 0;
    }
    return !s->failed;
}
/* Prepare, bind and request every file of a unit: the reads of its
 * discovery. record, when set, collects them for replay. */
static int sl_discover_files(sh_package_source_link *s, sl_unit *unit, sl_replay *record)
{
    if (!unit->preparation_checked && s->native.prepare && unit->unit.file_count == 1) {
        unsigned char *body = NULL; size_t length = 0;
        int status = s->native.prepare(s->native.context, s->readers[unit->scope],
            &unit->unit, &body, &length, s->error, s->error_capacity);
        if (status < 0) {
            free(body);
            if (status == -2) s->malformed = s->reader_gap = 1;
            else if (status == -3) s->reader_gap = 1;
            else s->environment = 1;
            return 0;
        }
        if (status == 1 && body) {
            if (record && record->valid) {
                if ((record->prepared = malloc(length + 1))) {
                    memcpy(record->prepared, body, length); record->prepared[length] = 0;
                    record->prepared_length = length; record->was_prepared = 1;
                } else record->valid = 0;
            }
            unit->unprepared = unit->unit.files[0];
            unit->prepared = *unit->unprepared;
            unit->prepared.body = body; unit->prepared.body_length = length;
            ((const sh_compiled_resource **)unit->unit.files)[0] = &unit->prepared;
        } else if (status || body) {
            free(body); return sl_env_fail(s, "invalid prepared source contract", unit->unit.name);
        }
        unit->preparation_checked = 1;
    }
    for (size_t j = 0; j < unit->unit.file_count; j++) {
        size_t length = 0, bound_length = 0;
        /* Diagnostics: an unsupplied original has no input source files. */
        int original = !unit->unit.files[j]->sources && unit->unit.files[j]->source == SIZE_MAX;
        unsigned char *body = sh_package_compilation_read(unit->unit.compilation, unit->unit.files[j],
            SIZE_MAX, &length, s->error, s->error_capacity), *bound = NULL;
        /* Linking binds this file with the same reader over the same bytes:
         * keep its sites for the first identity capture (observation none). */
        sl_discover walk = {s, unit, s->observe ? NULL : unit->unit.files[j], record};
        if (body) bound = sl_bind_member(s, unit, unit->unit.files[j],
            body, length, sl_reference, &walk, &bound_length, s->error, s->error_capacity);
        if (body && !bound && !s->failed && sh_package_source_reader_preparing(s->readers[unit->scope])) {
            /* A shader parameter or flag awaits native preparation. */
            free(body); if (record) record->valid = 0;
            return sl_defer_preparation(s);
        }
        int ok = body && bound && length == bound_length && !memcmp(body, bound, length) && !s->failed;
        if (ok && walk.file && !sl_discovered_add(s, unit, walk.file)) { free(body); free(bound); return 0; }
        if (ok && !strcmp(unit->unit.family, "material") &&
            !strncmp(unit->unit.files[j]->engine_path, "generated/decls/material/", 24))
            ok = sh_package_source_material_requests(s->readers[unit->scope],
                (sh_decl_source){(const char *)body, length}, s->observe,
                sl_implicit_reference, &walk, s->error, s->error_capacity);
        if (!ok && body && bound && !s->failed) s->reader_gap = 1;
        if (!ok && body && !bound && !s->failed) s->reader_gap = 1;
        if (!body) s->environment = 1; /* An unreadable captured file blames no input. */
        free(body); free(bound);
        s->census.binds++;
        if (original) s->census.original_binds++;
        if (!ok) return 0;
        if (record) record->file_refs[j] = record->ref_count;
    }
    return 1;
}
/* ---- AI state-machine webs -------------------------------------------------
 * Each AI builds its state-machine web at spawn from every aifsmmanager
 * declaration whose graph layers meet the configuration its behavior names
 * (edit.aiFSMManagerDeclLayersConfig), and reads that configuration from the
 * first declaration, states/alertcycle, whether or not its own graphs are
 * selected (fsm_web.h; Vulkan 0x108BFB0, OpenGL 0x107DB20). A behavior that
 * names a configuration therefore depends on states/alertcycle, and on every
 * declaration the configuration selects in its scope. Both are conditional
 * (sh_package_link_unit): they bind a behavior only to a changed declaration,
 * so an unchanged web leaves its identity as it was. */
static int sl_fsm_catalog(sh_package_source_link *s)
{
    if (s->fsm_names_ready) return 1;
    for (size_t i = 0; s->catalog && i < sh_resource_catalog_count(s->catalog); i++) {
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(s->catalog, i);
        int seen = 0;
        if (row->archive >= 2 || strcmp(row->type, "aifsmmanager") || !*row->name) continue;
        for (size_t k = 0; k < s->fsm_name_count && !seen; k++) seen = !strcmp(s->fsm_names[k], row->name);
        if (seen) continue;
        char **grown = realloc(s->fsm_names, (s->fsm_name_count + 1) * sizeof(*grown));
        if (!grown) return sl_env_fail(s, "cannot list state-machine declarations", row->name);
        s->fsm_names = grown;
        if (!(s->fsm_names[s->fsm_name_count] = _strdup(row->name)))
            return sl_env_fail(s, "cannot list state-machine declarations", row->name);
        s->fsm_name_count++;
    }
    s->fsm_names_ready = 1; return 1;
}
static int sl_fsm_listed(const sh_package_source_link *s, const char *name)
{
    for (size_t i = 0; i < s->fsm_name_count; i++) if (!strcmp(s->fsm_names[i], name)) return 1;
    return 0;
}
/* The declaration source a scope resolves (its own file, else the captured
 * original): 1 with the file and, when text is given, malloc-owned bytes;
 * 0 absent; -1 failed (reported). */
static int sl_fsm_source(sh_package_source_link *s, size_t scope, const char *family, const char *name,
    const sh_compiled_resource **file, char **text, size_t *length)
{
    char *path = sl_decl_path(family, name);
    if (!path) { sl_env_fail(s, "cannot construct declaration path", name); return -1; }
    int status = sl_source_file(s, scope, path, 1, file);
    free(path);
    if (status != 1 || !text) return status;
    *text = (char *)sh_package_compilation_read(s->scopes[scope], *file, SIZE_MAX, length, s->error, s->error_capacity);
    if (!*text) { sl_env_fail(s, "cannot read declaration source", name); return -1; }
    return 1;
}
/* The mask of one state-machine declaration's graphs (config NULL), or of one
 * configuration in it, as its scope resolves it. 1 found, 0 absent (no such
 * declaration or configuration, or no graphs), -1 failed (reported). */
static int sl_fsm_layers(sh_package_source_link *s, size_t scope, const char *name, const char *config, uint32_t *mask)
{
    const sh_compiled_resource *file = NULL;
    char *text = NULL, error[256] = "";
    size_t length = 0;
    *mask = 0;
    int status = sl_fsm_source(s, scope, "aifsmmanager", name, &file, NULL, NULL);
    if (status != 1) return status;
    for (size_t i = 0; i < s->fsm_mask_count; i++) {
        const sl_fsm_mask *known = s->fsm_masks + i;
        if (known->file == file && !known->config == !config && (!config || !strcmp(known->config, config))) {
            *mask = known->mask; return known->status;
        }
    }
    if (sl_fsm_source(s, scope, "aifsmmanager", name, &file, &text, &length) != 1) return -1;
    status = config ? sh_package_fsm_config_mask((sh_decl_source){text, length}, config, mask, error, sizeof(error)) :
        sh_package_fsm_graph_mask((sh_decl_source){text, length}, mask, error, sizeof(error));
    free(text);
    if (status < 0) {
        char reason[400];
        snprintf(reason, sizeof(reason), "state-machine %s cannot be read (%s)", config ? "layer configuration" : "graphs", error);
        sl_gap_fail(s, reason, name); return -1;
    }
    if (s->fsm_mask_count == s->fsm_mask_capacity) {
        size_t next = s->fsm_mask_capacity ? s->fsm_mask_capacity * 2 : 64;
        sl_fsm_mask *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(s->fsm_masks, next * sizeof(*grown)) : NULL;
        if (!grown) { sl_env_fail(s, "cannot retain state-machine layers", name); return -1; }
        s->fsm_masks = grown; s->fsm_mask_capacity = next;
    }
    sl_fsm_mask known = {file, config ? _strdup(config) : NULL, *mask, status};
    if (config && !known.config) { sl_env_fail(s, "cannot retain state-machine layers", name); return -1; }
    s->fsm_masks[s->fsm_mask_count++] = known;
    return status;
}
typedef struct sl_fsm_parents { sh_package_source_link *source; size_t scope; } sl_fsm_parents;
static int sl_fsm_parent(void *context, const char *name, sh_decl_source *out)
{
    sl_fsm_parents *parents = context;
    const sh_compiled_resource *file = NULL;
    char *text = NULL; size_t length = 0;
    int status = sl_fsm_source(parents->source, parents->scope, "aibehavior", name, &file, &text, &length);
    if (status == 1) { out->text = text; out->length = length; }
    return status;
}
typedef struct sl_fsm_walk { sh_package_source_link *source; sl_discover *discover; } sl_fsm_walk;
static int sl_fsm_exists(void *context, const char *declaration)
{
    sl_fsm_walk *walk = context;
    const sh_compiled_resource *file = NULL;
    return sl_fsm_source(walk->source, walk->discover->unit->scope, "aifsmmanager", declaration, &file, NULL, NULL);
}
static int sl_fsm_mask_of(void *context, const char *declaration, const char *config, uint32_t *mask)
{
    sl_fsm_walk *walk = context;
    return sl_fsm_layers(walk->source, walk->discover->unit->scope, declaration, config, mask);
}
static int sl_fsm_visit(void *context, const char *name)
{
    sl_fsm_walk *walk = context;
    char slot[600];
    snprintf(slot, sizeof(slot), "fsm:%s", name);
    return sl_implicit_add(walk->discover, slot, "aifsmmanager", name, 1) == 1;
}
static int sl_fsm_edges(sh_package_source_link *s, sl_unit *unit)
{
    const sh_package_compilation *scope = s->scopes[unit->scope];
    sl_fsm_parents parents = {s, unit->scope};
    sh_decl_entity_class_source source = {&parents, sl_fsm_parent, NULL};
    sl_discover discover = {s, unit};
    sl_fsm_walk walk = {s, &discover};
    sh_package_fsm_layers layers = {&walk, sl_fsm_exists, sl_fsm_mask_of};
    const char **names = NULL;
    char *config = NULL, error[512] = "";
    size_t length = 0, count = 0;
    int status, ok;
    if (!g_sl_fsm || strcmp(unit->unit.family, "aibehavior") || !unit->unit.file_count) return 1;
    unsigned char *body = sh_package_compilation_read(unit->unit.compilation, unit->unit.files[0], SIZE_MAX, &length,
        s->error, s->error_capacity);
    if (!body) return sl_env_fail(s, "cannot read behavior declaration", unit->unit.name);
    status = sh_package_fsm_behavior_config((sh_decl_source){(char *)body, length}, &source, &config, error, sizeof(error));
    free(body);
    if (s->failed) { free(config); return 0; }
    if (status < 0) {
        char reason[640];
        snprintf(reason, sizeof(reason), "behavior layer configuration cannot be resolved (%s)", error);
        return sl_gap_fail(s, reason, unit->unit.name);
    }
    if (!status) return 1;
    /* Candidates: the catalog's declarations, then the ones this scope adds. */
    if (!sl_fsm_catalog(s) || !(names = calloc(s->fsm_name_count + scope->resource_count + 1, sizeof(*names)))) {
        free(config); return s->failed ? 0 : sl_env_fail(s, "cannot list state-machine declarations", unit->unit.name);
    }
    for (size_t i = 0; i < s->fsm_name_count; i++) names[count++] = s->fsm_names[i];
    for (size_t i = 0; i < scope->resource_count; i++) {
        const sh_compiled_resource *file = scope->resources + i;
        if (file->type && !strcmp(file->type, "aifsmmanager") && file->name && *file->name && !sl_fsm_listed(s, file->name))
            names[count++] = file->name;
    }
    ok = sh_package_fsm_select(config, &layers, names, count, sl_fsm_visit, &walk);
    free((void *)names);
    if (ok) { free(unit->fsm_config); unit->fsm_config = config; } else free(config);
    return ok && !s->failed;
}
static int sl_discover_unit_requests(sh_package_source_link *s, sl_unit *unit)
{
    unsigned long long began = sl_now_us(), selected, read;
    if (!sl_files(s, unit)) {
        if (s->observe && !s->failed && !s->preparing) s->reader_gap = 1;
        return 0;
    }
    selected = sl_now_us(); s->census.select_us += selected - began;
    sl_replay *record = NULL;
    if (sl_replay_eligible(s, unit)) {
        int known = 0;
        for (const sl_replay *r = sl_store_find(s->store, unit); r; r = r->next) {
            if (!r->valid) continue;
            known = 1; s->census.replay_tries++;
            unsigned long long checking = sl_now_us();
            int applies = sl_replay_applies(s, unit, r);
            unsigned long long applying = sl_now_us();
            s->census.applies_us += applying - checking;
            if (!applies) {
                if (s->failed) return 0; /* A read that failed while validating. */
                continue;
            }
            int applied = sl_replay_apply(s, unit, r);
            s->census.apply_us += sl_now_us() - applying;
            if (!applied) return 0;
            unit->replayed = 1; s->census.replayed++;
            if (r->capture != s->capture) s->census.retained++;
            goto constructed;
        }
        if (known) s->census.declined++;
        record = sl_replay_open(s, unit); /* NULL: optional, bind without recording */
    }
    s->recording = record;
    unsigned long long fresh = sl_now_us();
    int ok = sl_discover_files(s, unit, record);
    s->census.fresh_us += sl_now_us() - fresh;
    s->recording = NULL;
    if (record && ok && record->valid && sl_replay_complete(s, record)) sl_store_add(s, record);
    else sl_replay_free(record);
    if (s->failed) return 0;
    if (!ok) return 0;
constructed:
    read = sl_now_us(); s->census.read_us += read - selected;
    if (!strcmp(unit->unit.family, "font") && !sl_font_capture(s, unit)) return 0;
    sl_discover walk = {s, unit};
    if (s->native.implicit && s->native.implicit(s->native.context, unit->scope, s->observe,
        &unit->unit, sl_implicit_reference, &walk, s->error, s->error_capacity) != 1)
        return sl_fail(s, "native constructor dependencies could not be captured", unit->unit.name);
    if (!sl_fsm_edges(s, unit)) return 0;
    s->census.construct_us += sl_now_us() - read;
    return !s->failed;
}
/* These are the proven terminal families of the source reader. Reverse
 * reachability needs their identities, not a second copy of large leaf bytes.
 * Ordinary selected-unit linking still verifies their physical source. */
static int sl_reverse_leaf(const char *family)
{
    return !strcmp(family, "image") || !strcmp(family, "skeleton") || !strcmp(family, "cm") ||
        !strcmp(family, "aas") || !strcmp(family, "binaryfile") || !strcmp(family, "md6rig") ||
        !strcmp(family, "transsortatlas") || !strcmp(family, "fga");
}
/* Account one reverse-discovery coverage gap. scope is SIZE_MAX for the stock
 * unit whose reader stopped, or the discovery scope whose changed dependency
 * reaches an incompletely read stock root. Owned copies survive the walk. */
static int sl_reverse_gap_add(sh_package_source_link *s, size_t scope, const sl_unit *unit, const char *reason)
{
    for (size_t i = 0; i < s->reverse_gap_count; i++) {
        const sh_package_reverse_gap *gap = s->reverse_gaps + i;
        if (gap->scope == scope && !strcmp(gap->family, unit->unit.family) && !strcmp(gap->name, unit->unit.name)) return 1;
    }
    if (s->reverse_gap_count == s->reverse_gap_capacity) {
        size_t next = s->reverse_gap_capacity ? s->reverse_gap_capacity * 2 : 16;
        sh_package_reverse_gap *grown = next > s->reverse_gap_capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(s->reverse_gaps, next * sizeof(*grown)) : NULL;
        if (!grown) return sl_env_fail(s, "cannot account reverse discovery coverage", unit->unit.name);
        s->reverse_gaps = grown; s->reverse_gap_capacity = next;
    }
    sh_package_reverse_gap gap = {scope, _strdup(unit->unit.family), _strdup(unit->unit.name),
        _strdup(reason && *reason ? reason : "reader coverage is incomplete")};
    if (!gap.family || !gap.name || !gap.reason) {
        free((char *)gap.family); free((char *)gap.name); free((char *)gap.reason);
        return sl_env_fail(s, "cannot account reverse discovery coverage", unit->unit.name);
    }
    s->reverse_gaps[s->reverse_gap_count++] = gap; return 1;
}
/* First incompletely read unit reachable from root, for attribution only. */
static const sl_unit *sl_reverse_gap_reached(sh_package_source_link *base, size_t root, unsigned char *seen, size_t *stack)
{
    const sl_unit *found = NULL; size_t depth = 0;
    memset(seen, 0, base->count);
    stack[depth++] = root; seen[root] = 1;
    while (depth && !found) {
        const sl_unit *unit = base->units[stack[--depth]];
        if (unit->reverse_gap) { found = unit; break; }
        for (size_t d = 0; d < unit->dependency_count; d++) {
            size_t next = unit->dependencies[d]->index;
            if (!seen[next]) { seen[next] = 1; stack[depth++] = next; }
        }
    }
    return found;
}
typedef struct sl_reverse_native { sh_package_source_link *source; size_t baseline; } sl_reverse_native;
static int sl_reverse_admit(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    sl_reverse_native *walk = context;
    (void)scope;
    return walk->source->native.admit ? walk->source->native.admit(walk->source->native.context,
        walk->baseline, family, name, semantics, error, capacity) : 0;
}
static int sl_reverse_implicit(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visit_context,
    char *error, size_t capacity)
{
    sl_reverse_native *walk = context; (void)scope;
    return !walk->source->native.implicit || walk->source->native.implicit(walk->source->native.context,
        walk->baseline, observing, unit, visit, visit_context, error, capacity) == 1;
}
static int sl_reverse_prepare(void *context, sh_package_source_reader *reader,
    const sh_package_link_unit *unit, unsigned char **body, size_t *length, char *error, size_t capacity)
{
    sl_reverse_native *walk = context;
    return walk->source->native.prepare(walk->source->native.context, reader, unit, body, length, error, capacity);
}
static int sl_reverse_file_changed(sh_package_source_link *s, size_t scope, const char *path)
{
    const sh_compiled_resource *file = path && *path ? sh_package_compilation_find(s->scopes[scope], path) : NULL;
    return file && sh_package_owners_count(&file->gameplay_owners) != 0;
}
static int sl_reverse_changed(sh_package_source_link *s, size_t scope, const sl_unit *unit)
{
    if (!strcmp(unit->unit.family, "sound") &&
        sh_package_audio_program_affected(s->native.audio, scope, unit->unit.name)) return 1;
    /* What this scope supplies under that identity: a seeded unit as seeded.
     * Seed discovery runs first (sl_discover_through) and may already have
     * added stock dependencies and material companions; neither is supplied. */
    const sl_unit *supplied = sl_find(s, scope, unit->unit.family, unit->unit.name, 0);
    if (supplied && !supplied->seeded) supplied = NULL;
    for (size_t i = 0; supplied && i < supplied->unit.file_count; i++)
        if (sh_package_owners_count(&supplied->unit.files[i]->gameplay_owners)) return 1;
    for (size_t i = 0; supplied && i < supplied->seeded_witnesses; i++)
        if (sh_package_owners_count(&supplied->unit.witnesses[i].resource->gameplay_owners)) return 1;
    /* Catalog aliases need not spell the same logical name as a typed field.
     * The inspected unit's physical members still select exactly its source. */
    for (size_t i = 0; i < unit->unit.file_count; i++)
        if (sl_reverse_file_changed(s, scope, unit->unit.files[i]->engine_path)) return 1;
    for (size_t i = 0; i < unit->unit.witness_count; i++)
        if (sl_reverse_file_changed(s, scope, unit->unit.witnesses[i].resource->engine_path)) return 1;
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t count = sh_resource_catalog_find(s->catalog, unit->unit.family, unit->unit.name, &rows);
    for (size_t i = 0; i < count; i++) if (sl_reverse_file_changed(s, scope, rows[i]->path)) return 1;
    /* Terminal families did not need their large payloads read for reverse
     * reachability. Use only the existing verified native path adapter. */
    char *path = sh_package_cooked_path(unit->unit.family, unit->unit.name);
    int changed = sl_reverse_file_changed(s, scope, path); free(path); return changed;
}
/* ---- The player hands web --------------------------------------------------
 * idPlayer::handsAnimWeb (and its _D1 alternate) names the one animation web
 * the hands animator resolves every weapon's subweb in by name (Vulkan
 * FUN_1403c0e30 / FUN_1403c16f0: subweb lookup in the animator's single web).
 * The engine spawns the co-op rules entity mp/modes/coop/game_challenge and
 * the player from its playerSpawnDef, defaulting to player/mp
 * (CreateDefaultChallengeEntityFromSession, Vulkan FUN_14030cb20; the same
 * constant is at OpenGL 0x142017bc0); SnapMap plays the co-op mode. So that
 * web is one global registry of weapon subwebs, whether or not any placed
 * object reaches the player. The stock rules and player decide it. */
typedef struct sl_stock_reader { sh_package_baseline_reader read; void *context; } sl_stock_reader;
static int sl_stock_parent(void *context, const char *name, sh_decl_source *source)
{
    sl_stock_reader *reader = context;
    unsigned char *body = NULL; size_t length = 0;
    char *path = sl_decl_path("entitydef", name);
    int status = path ? reader->read(reader->context, path, &body, &length) : -1;
    free(path);
    if (status <= 0) { free(body); return status; }
    source->text = (const char *)body; source->length = length; return 1;
}
/* One field of a stock entity's expanded edit state; NULL when absent. */
static char *sl_stock_field(sl_stock_reader *reader, const char *entity, const char *field, int *failed)
{
    sh_decl_source source = {0};
    sh_decl_entity_class_source parents = {reader, sl_stock_parent, NULL};
    const char *value = NULL; size_t length = 0;
    char *out = NULL;
    int status = sl_stock_parent(reader, entity, &source);
    if (status < 0) *failed = 1;
    if (status <= 0) return NULL;
    sh_decl_node *tree = sh_decl_tree_parse(source, NULL, 0);
    free((char *)source.text);
    sh_decl_node *state = tree ? sh_decl_entity_expanded_state(tree, &parents, NULL, 0) : NULL;
    const sh_decl_node *edit = state ? sh_decl_tree_member(state, "edit") : NULL;
    if (edit && sh_decl_tree_literal(sh_decl_tree_member(edit, field), &value, &length) && length) {
        if ((out = malloc(length + 1))) { memcpy(out, value, length); out[length] = 0; }
        else *failed = 1;
    }
    sh_decl_tree_free(state); sh_decl_tree_free(tree); return out;
}
int sh_package_player_hands_webs(sh_package_baseline_reader original, void *context,
    char ***names, size_t *count)
{
    static const char *const fields[] = {"handsAnimWeb", "handsAnimWeb_D1"};
    sl_stock_reader reader = {original, context};
    int failed = 0;
    *names = NULL; *count = 0;
    if (!original) return 1;
    char *player = sl_stock_field(&reader, "mp/modes/coop/game_challenge", "playerSpawnDef", &failed);
    for (size_t i = 0; !failed && i < sizeof(fields) / sizeof(fields[0]); i++) {
        char *web = sl_stock_field(&reader, player ? player : "player/mp", fields[i], &failed);
        int seen = 0;
        for (size_t k = 0; web && k < *count; k++) seen |= !strcmp((*names)[k], web);
        if (web && !seen) {
            char **grown = realloc(*names, (*count + 1) * sizeof(*grown));
            if (!grown) { free(web); failed = 1; break; }
            *names = grown; (*names)[(*count)++] = web; web = NULL;
        }
        free(web);
    }
    free(player);
    if (!failed) return 1;
    for (size_t k = 0; k < *count; k++) free((*names)[k]);
    free(*names); *names = NULL; *count = 0; return 0;
}
static int sl_player_contract(sh_package_source_link *s)
{
    int supplied = 0;
    /* Only a supplied animation web can change the hands web. */
    for (size_t scope = 0; scope < s->scope_count && !supplied; scope++)
        for (size_t i = 0; i < s->scopes[scope]->resource_count && !supplied; i++) {
            const sh_compiled_resource *file = s->scopes[scope]->resources + i;
            supplied = file->type && !strcmp(file->type, "animweb") && file->source_count;
        }
    if (!s->original || !supplied) return 1;
    char **names = NULL; size_t count = 0;
    int failed = !sh_package_player_hands_webs(sl_read_original, s, &names, &count);
    for (size_t i = 0; i < count; i++) {
        if (!failed && !sl_player_hands_add(s, names[i])) failed = 1;
        free(names[i]);
    }
    free(names);
    return failed ? sl_env_fail(s, "cannot read the stock player hands web", "mp/modes/coop/game_challenge") : 1;
}
static int sl_reverse_roots(sh_package_source_link *s, const sh_package_source_root_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection)
{
    snprintf(s->seed_note, sizeof(s->seed_note), "reverse root discovery");
    sh_package_source_link *base = NULL;
    sh_package_link_node *nodes = NULL;
    sh_package_link_edge *edges = NULL;
    unsigned char *local = NULL, *unchanged = NULL, *complete = NULL, *seen = NULL;
    size_t *stack = NULL;
    size_t roots = 0, edge_count = 0, at = 0;
    int needed = 0, ok = 0;
    sh_package_compilation empty = {0};
    sl_reverse_native admission = {s, plan ? plan->baseline_scope : SIZE_MAX};
    if (!plan) return 1;
    if (plan->baseline_scope >= s->scope_count)
        return sl_env_fail(s, "reverse authoring roots require an original-only scope", NULL);
    /* A saved derived root need not have an authored declaration file. It must
     * still bind in exactly the recorded source context, never current local. */
    if (plan->recorded) for (size_t i = 0; i < plan->recorded->binding_count; i++) {
        const sh_package_provenance_binding *binding = plan->recorded->bindings + i;
        /* A registration key names no resource (package_keys.h). */
        if (!strcmp(binding->family, SH_PACKAGE_KEY_FAMILY)) continue;
        for (size_t j = 0; j < binding->context_count; j++) {
            size_t scope = binding->contexts[j];
            if (scope >= plan->recorded->context_count || scope >= s->scope_count ||
                !sl_find(s, scope, binding->family, binding->original, 1)) return 0;
        }
    }
    if (plan->discover) for (size_t scope = 0; scope < s->scope_count; scope++) {
        if (!plan->discover[scope]) continue;
        for (size_t f = 0; f < s->scopes[scope]->resource_count; f++)
            if (sh_package_owners_count(&s->scopes[scope]->resources[f].gameplay_owners)) needed = 1;
    }
    if (!needed || (!plan->candidates && !sh_resource_catalog_count(s->catalog))) return 1;
    base = calloc(1, sizeof(*base)); if (!base) goto done;
    base->scopes = calloc(1, sizeof(*base->scopes)); base->readers = calloc(1, sizeof(*base->readers));
    if (!base->scopes || !base->readers) goto done;
    empty.sources = s->scopes[plan->baseline_scope]->sources;
    empty.canonical_path = s->scopes[plan->baseline_scope]->canonical_path;
    base->scopes[0] = &empty; base->scope_count = 1;
    base->observe = 1;
    base->catalog = s->catalog; base->original = sl_read_original; base->original_context = s;
    base->error = s->error; base->error_capacity = s->error_capacity;
    base->native.context = &admission; base->native.admit = sl_reverse_admit;
    base->native.implicit = sl_reverse_implicit;
    if (s->native.prepare) base->native.prepare = sl_reverse_prepare;
    base->native.shader = s->native.shader;
    base->readers[0] = sh_package_source_reader_open(&empty, s->catalog, sl_read_original, s,
        registry, reflection, s->error, s->error_capacity);
    if (!base->readers[0] || (s->native.shader &&
        !sh_package_source_reader_shader(base->readers[0], s->native.shader))) goto done;
    /* PaletteBuild enumerates the snapEditorEntityDef manager. Campaign-only
     * entries and generic entity declarations are not additional palette roots.
     * A legacy document instead names its own roots; a name with no stock
     * source is an empty leaf here and is never nominated. */
    if (plan->candidates) for (size_t i = 0; i < plan->candidates->count; i++) {
        const sh_package_reference *root = plan->candidates->items + i;
        if (!root->type || !*root->type || !root->name || !*root->name ||
            !strcmp(root->type, SH_PACKAGE_KEY_FAMILY)) continue;
        if (!sl_find(base, 0, root->type, root->name, 1)) goto done;
    } else for (size_t i = 0; i < sh_resource_catalog_count(s->catalog); i++) {
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(s->catalog, i);
        if (row->archive < 2 && !strcmp(row->type, "snapeditorentitydef") && *row->name &&
            !sl_find(base, 0, row->type, row->name, 1)) goto done;
    }
    roots = base->count;
    for (size_t i = 0; i < base->count; i++) {
        sl_unit *unit = base->units[i];
        base->reader_gap = base->preparing = 0;
        if (!sl_reverse_leaf(unit->unit.family) && !sl_discover_unit(base, unit)) {
            if (base->preparing) continue;
            /* Unreadable originals, allocation and constructor capture failures
             * stop discovery. A stock source beyond the registered readers is a
             * product coverage gap in that root, not in an unrelated package:
             * account it, keep its proven edges and never nominate across it. */
            char reason[768];
            if (base->failed || !base->reader_gap) goto done;
            snprintf(reason, sizeof(reason), "%s", s->error && *s->error ? s->error : "");
            if (s->error && s->error_capacity) *s->error = 0;
            if (!sl_reverse_gap_add(s, SIZE_MAX, unit, reason)) goto done;
            unit->reverse_gap = s->reverse_gap_count;
        }
        if (unit->dependency_count > SIZE_MAX - edge_count) goto done;
        edge_count += unit->dependency_count;
    }
    if (*base->preparation_reason) {
        sl_env_fail(base, base->preparation_reason, NULL); goto done;
    }
    if (base->count > SIZE_MAX / sizeof(*nodes) || base->count > SIZE_MAX / sizeof(*stack) ||
        edge_count > SIZE_MAX / sizeof(*edges)) goto done;
    nodes = calloc(base->count ? base->count : 1, sizeof(*nodes));
    edges = calloc(edge_count ? edge_count : 1, sizeof(*edges));
    local = malloc(base->count ? base->count : 1); unchanged = malloc(base->count ? base->count : 1);
    complete = malloc(base->count ? base->count : 1); seen = malloc(base->count ? base->count : 1);
    stack = malloc((base->count ? base->count : 1) * sizeof(*stack));
    if (!nodes || !edges || !local || !unchanged || !complete || !seen || !stack) goto done;
    for (size_t i = 0; i < base->count; i++) {
        const sl_unit *unit = base->units[i];
        nodes[i].edges = edges + at; nodes[i].edge_count = unit->dependency_count;
        for (size_t j = 0; j < unit->dependency_count; j++) edges[at++].target = unit->dependencies[j]->index;
        local[i] = !unit->reverse_gap;
    }
    /* A root is completely observed only when no reachable unit stopped early. */
    if (!sh_package_link_originals(nodes, base->count, local, complete, s->error, s->error_capacity)) goto done;
    for (size_t scope = 0; scope < s->scope_count; scope++) if (plan->discover[scope]) {
        memset(local, 1, base->count);
        /* A player hands web changes globally (the library composes it under
         * its stock name), so reaching it gives a stock root no variant. */
        for (size_t i = 0; i < base->count; i++) {
            const sl_unit *unit = base->units[i];
            if (!strcmp(unit->unit.family, "animweb") && sh_package_source_link_player_hands(s, unit->unit.name)) continue;
            if (sl_reverse_changed(s, scope, unit)) local[i] = 0;
        }
        /* Reverse propagation is candidate discovery only. Final scoped graph
         * equality/certification still decides whether a private root exists. */
        if (!sh_package_link_originals(nodes, base->count, local, unchanged, s->error, s->error_capacity)) goto done;
        for (size_t i = 0; i < roots; i++) if (!unchanged[i]) {
            const sl_unit *unit = base->units[i];
            if (!complete[i]) {
                /* This scope changes a stock entry whose remaining closure the
                 * readers cannot bind faithfully. Expose no guessed variant;
                 * attribute the missing coverage to this scope and continue. */
                const sl_unit *gap = sl_reverse_gap_reached(base, i, seen, stack);
                if (!sl_reverse_gap_add(s, scope, unit, gap && gap->reverse_gap ?
                    s->reverse_gaps[gap->reverse_gap - 1].reason : NULL)) goto done;
                continue;
            }
            size_t before = s->count;
            sl_unit *root = sl_find(s, scope, unit->unit.family, unit->unit.name, 1);
            if (!root || !sl_find(s, plan->baseline_scope, unit->unit.family, unit->unit.name, 1)) goto done;
            if (s->count > before && s->units[before] == root) root->nominated = 1;
        }
    }
    ok = 1;
done:
    if (!ok) {
        char detail[768]; snprintf(detail, sizeof(detail), "%s", s->error && *s->error ? s->error : "allocation failed");
        if (s->error && s->error_capacity) snprintf(s->error, s->error_capacity,
            "baseline palette reverse dependency coverage is incomplete: %s", detail);
        s->failed = 1;
    }
    free(nodes); free(edges); free(local); free(unchanged); free(complete); free(seen); free(stack);
    if (base) s->census.reverse_units = base->count;
    sh_package_source_link_close(base); return ok;
}
static int sl_unit_order(const void *a, const void *b)
{
    const sl_unit *x = *(sl_unit *const *)a, *y = *(sl_unit *const *)b;
    if (x->scope != y->scope) return x->scope < y->scope ? -1 : 1;
    int order = strcmp(x->unit.family, y->unit.family);
    return order ? order : strcmp(x->unit.name, y->unit.name);
}
static int sl_file_order(const void *a, const void *b)
{ return strcmp((*(const sh_compiled_resource *const *)a)->engine_path, (*(const sh_compiled_resource *const *)b)->engine_path); }

/* Editor presentation inherits the scope of the gameplay it can reach. This
 * uses the exact reader edges already discovered above, including inherited
 * definitions and cycles. It is not a package-ID or filename classification.
 * A shared result is only a composition eligibility decision, never proof that
 * the native identity can be retained or a map does not need a dependency. */
static int sl_shared(sh_package_source_link *s)
{
    sh_package_link_node *nodes = NULL;
    sh_package_link_edge *edges = NULL;
    size_t edge_count = 0, at = 0;
    int ok = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->units[i]->dependency_count > SIZE_MAX - edge_count) goto done;
        edge_count += s->units[i]->dependency_count;
    }
    if (s->count > SIZE_MAX / sizeof(*nodes) || edge_count > SIZE_MAX / sizeof(*edges)) goto done;
    nodes = calloc(s->count ? s->count : 1, sizeof(*nodes));
    edges = calloc(edge_count ? edge_count : 1, sizeof(*edges));
    s->shared = calloc(s->count ? s->count : 1, 1);
    if (!nodes || !edges || !s->shared) goto done;
    for (size_t i = 0; i < s->count; i++) {
        const sl_unit *unit = s->units[i];
        s->shared[i] = 1;
        for (size_t f = 0; f < unit->unit.file_count; f++)
            if (sh_package_owners_count(&unit->unit.files[f]->gameplay_owners)) s->shared[i] = 0;
        for (size_t f = 0; f < unit->unit.witness_count; f++)
            if (sh_package_owners_count(&unit->unit.witnesses[f].resource->gameplay_owners)) s->shared[i] = 0;
        if (unit->unit.implicit_gameplay_owners && sh_package_owners_count(unit->unit.implicit_gameplay_owners)) s->shared[i] = 0;
        nodes[i].edges = edges + at; nodes[i].edge_count = unit->dependency_count;
        for (size_t d = 0; d < unit->dependency_count; d++)
            edges[at++].target = unit->dependencies[d]->index;
    }
    ok = sh_package_link_originals(nodes, s->count, s->shared, s->shared, s->error, s->error_capacity);
done:
    free(nodes); free(edges);
    if (!ok) return sl_env_fail(s, "cannot classify scoped source dependencies", NULL);
    return 1;
}

/* A unit carries content its scope supplied, not only product built-ins
 * (which have no input files) or captured originals. */
static int sl_unit_supplied(const sl_unit *unit)
{
    for (size_t i = 0; i < unit->unit.file_count; i++) if (unit->unit.files[i]->source_count) return 1;
    for (size_t i = 0; i < unit->unit.witness_count; i++)
        if (unit->unit.witnesses[i].resource->source_count) return 1;
    return 0;
}
static int sl_scope_supplies(const sh_package_source_link *s, size_t scope)
{
    const sh_package_compilation *compiled = scope < s->scope_count ? s->scopes[scope] : NULL;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++)
        if (compiled->resources[i].source_count) return 1;
    return 0;
}
/* Classify a failed closure by the content that reached it (see the header).
 * The chain follows first requesters from the failing unit back to its seed. */
static void sl_attribute(const sh_package_source_link *s, sh_package_source_failure *failure)
{
    const sl_unit *failed = s->failed_unit, *supplier = NULL, *root = NULL;
    size_t used = 0;
    failure->kind = SH_PACKAGE_SOURCE_FAILURE_NONE; failure->scope = SIZE_MAX;
    failure->family[0] = failure->name[0] = failure->chain[0] = 0;
    if (s->environment) { failure->kind = SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT; return; }
    if (!failed) {
        if (s->failed_scope != SIZE_MAX && sl_scope_supplies(s, s->failed_scope)) {
            failure->kind = SH_PACKAGE_SOURCE_FAILURE_COVERAGE; failure->scope = s->failed_scope;
        }
        return;
    }
    snprintf(failure->family, sizeof(failure->family), "%s", failed->unit.family);
    snprintf(failure->name, sizeof(failure->name), "%s", failed->unit.name);
    for (const sl_unit *unit = failed; unit; unit = unit->parent) {
        if (!supplier && sl_unit_supplied(unit)) supplier = unit;
        root = unit;
        if (used + 1 < sizeof(failure->chain)) {
            int n = snprintf(failure->chain + used, sizeof(failure->chain) - used, "%s%s %s",
                unit == failed ? "" : " <- ", unit->unit.family, unit->unit.name);
            if (n < 0 || (size_t)n >= sizeof(failure->chain) - used) {
                /* Keep the nearest requesters; mark the omitted seed side. */
                size_t keep = sizeof(failure->chain) - 8;
                if (used > keep) used = keep;
                snprintf(failure->chain + used, sizeof(failure->chain) - used, " <- ...");
                used = sizeof(failure->chain);
            } else used += (size_t)n;
        }
    }
    if (root && root->seed && used + 1 < sizeof(failure->chain)) {
        int n = snprintf(failure->chain + used, sizeof(failure->chain) - used, " [%s]", root->seed);
        used = n < 0 || (size_t)n >= sizeof(failure->chain) - used ? sizeof(failure->chain) : used + (size_t)n;
    }
    if (supplier) {
        failure->kind = supplier == failed && s->malformed ?
            SH_PACKAGE_SOURCE_FAILURE_SOURCE : SH_PACKAGE_SOURCE_FAILURE_COVERAGE;
        failure->scope = failed->scope;
    } else if (root && root->nominated) {
        failure->kind = SH_PACKAGE_SOURCE_FAILURE_COVERAGE; failure->scope = failed->scope;
    } else failure->kind = SH_PACKAGE_SOURCE_FAILURE_PRODUCT;
}

/* Discover every unit from *inspected on, including those it requests. A
 * unit waiting for native preparation is left for a fresh pass; any other
 * failure stops the capture with that unit (sl_attribute). */
/* A failure its scope alone answers for (SOURCE or COVERAGE, sl_attribute):
 * keep the first, withdraw that scope from this capture and let the other
 * scopes continue, so the same attempt also meets every other scope's
 * failures and preparation requests. sl_phase_complete reports it. 0 when
 * the failure is not such (it stops the capture now). */
static int sl_withdrawal_add(sh_package_source_withdrawals *w, const sh_package_source_failure *failure,
    const char *reason)
{
    if (w->count == w->capacity) {
        size_t capacity = w->capacity ? w->capacity * 2 : 4;
        sh_package_source_withdrawal *grown = realloc(w->items, capacity * sizeof(*grown));
        if (!grown) return 0;
        w->items = grown; w->capacity = capacity;
    }
    sh_package_source_withdrawal *item = w->items + w->count++;
    item->failure = *failure;
    snprintf(item->reason, sizeof(item->reason), "%s", reason ? reason : "");
    return 1;
}
void sh_package_source_withdrawals_free(sh_package_source_withdrawals *withdrawals)
{
    if (!withdrawals) return;
    free(withdrawals->items);
    withdrawals->items = NULL; withdrawals->count = withdrawals->capacity = 0;
}
static int sl_defer_failure(sh_package_source_link *s)
{
    sh_package_source_failure failure;
    if (s->environment) return 0;
    sl_attribute(s, &failure);
    if ((failure.kind != SH_PACKAGE_SOURCE_FAILURE_SOURCE && failure.kind != SH_PACKAGE_SOURCE_FAILURE_COVERAGE) ||
        failure.scope >= s->scope_count) return 0;
    if (!s->withdrawn && !(s->withdrawn = calloc(s->scope_count, 1))) return 0;
    /* Every withdrawal is kept for the caller, not only the first: each
     * independent scope that fails on its own is reported by this one capture. */
    if (s->native.withdrawals && !sl_withdrawal_add(s->native.withdrawals, &failure,
            s->error && *s->error ? s->error : NULL)) return 0;
    if (!s->deferred.unit) {
        s->deferred.unit = s->failed_unit; s->deferred.malformed = s->malformed; s->deferred.reader_gap = s->reader_gap;
        snprintf(s->deferred.error, sizeof(s->deferred.error), "%s", s->error && *s->error ? s->error : "");
    }
    s->withdrawn[failure.scope] = 1;
    s->failed = 0; s->failed_unit = NULL;
    if (s->error && s->error_capacity) *s->error = 0;
    return 1;
}
/* End of a discovery phase: report a kept failure (as it was attributed), or
 * fail for the preparation requests retained so far. */
static int sl_phase_complete(sh_package_source_link *s)
{
    if (s->deferred.unit) {
        s->failed = 1; s->failed_unit = s->deferred.unit;
        s->malformed = s->deferred.malformed; s->reader_gap = s->deferred.reader_gap;
        if (s->error && s->error_capacity) snprintf(s->error, s->error_capacity, "%s", s->deferred.error);
        return 0;
    }
    if (*s->preparation_reason) return sl_env_fail(s, s->preparation_reason, NULL);
    return 1;
}
/* Discover every unit from *inspected on, including those it requests. A
 * unit waiting for native preparation is left for a fresh pass; a failure its
 * scope alone answers for withdraws that scope (sl_defer_failure); any other
 * failure stops the capture with that unit (sl_attribute), though a failure
 * kept earlier is reported before a later one that is not environmental. */
static int sl_discover_through(sh_package_source_link *s, size_t *inspected)
{
    for (; *inspected < s->count; (*inspected)++) {
        sl_unit *unit = s->units[*inspected];
        if (s->withdrawn && s->withdrawn[unit->scope]) continue;
        s->reader_gap = s->malformed = s->preparing = 0;
        int discovered = sl_discover_unit(s, unit);
        s->census.units++;
        if (discovered) {
            if (unit->unit.file_count > 1) qsort((void *)unit->unit.files, unit->unit.file_count, sizeof(*unit->unit.files), sl_file_order);
            if (sl_movie_identities(s, unit)) continue;
        } else if (s->preparing) continue;
        s->failed_unit = unit;
        if (sl_defer_failure(s)) continue;
        if (s->deferred.unit && !s->environment) return sl_phase_complete(s);
        return 0;
    }
    return 1;
}
void sh_package_source_link_close(sh_package_source_link *s)
{
    if (!s) return;
    sl_store_clear(&s->own);
    if (s->memo) _InterlockedExchange(&s->memo->busy, 0);
    for (size_t i = 0; s->readers && i < s->scope_count; i++) sh_package_source_reader_close(s->readers[i]);
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        free(unit->seed); free(unit->original_note);
        free((char *)unit->unit.family); free((char *)unit->unit.name); free((void *)unit->unit.files);
        for (size_t w = 0; w < unit->unit.witness_count; w++) free((char *)unit->unit.witnesses[w].role);
        free((void *)unit->unit.witnesses);
        for (size_t j = 0; j < unit->implicit_count; j++) free(unit->implicit[j].slot);
        free(unit->implicit); free(unit->implicit_edges); free(unit->fsm_config);
        for (size_t p = 0; p < unit->program_count; p++) {
            free(unit->programs[p].name); sh_package_shader_program_clear(&unit->programs[p].decoded);
        }
        free(unit->programs); free(unit->shader_files);
        if (unit->identities) for (size_t j = 0; j < unit->unit.file_count; j++) free((char *)unit->identities[j].name);
        free(unit->identities);
        free(unit->prepared.body);
        for (size_t j = 0; j < unit->site_count; j++) free(unit->sites[j].slot);
        free(unit->sites); free((void *)unit->discovered); free(unit->site_edges);
        free(unit->dependencies); free(unit);
    }
    for (size_t i = 0; i < s->original_count; i++) {
        free((void *)s->originals[i]->resource.engine_path); free(s->originals[i]->resource.body); free(s->originals[i]);
    }
    for (size_t i = 0; i < s->reverse_gap_count; i++) {
        free((char *)s->reverse_gaps[i].family); free((char *)s->reverse_gaps[i].name);
        free((char *)s->reverse_gaps[i].reason);
    }
    free(s->reverse_gaps);
    for (size_t i = 0; i < s->player_hand_count; i++) free(s->player_hands[i]);
    free(s->player_hands);
    for (size_t i = 0; i < s->fsm_name_count; i++) free(s->fsm_names[i]);
    free(s->fsm_names);
    for (size_t i = 0; i < s->fsm_mask_count; i++) free(s->fsm_masks[i].config);
    free(s->fsm_masks);
    free(s->withdrawn);
    free(s->originals); free(s->original_slots); free(s->units); free(s->unit_slots);
    free(s->inputs.units); free(s->inputs.scopes); free(s->readers); free(s->scopes); free(s->shared); free(s);
}
int sh_package_source_link_player_hands(const sh_package_source_link *s, const char *name)
{
    for (size_t i = 0; s && name && i < s->player_hand_count; i++) if (!strcmp(s->player_hands[i], name)) return 1;
    return 0;
}
size_t sh_package_source_link_reverse_gaps(const sh_package_source_link *source,
    const sh_package_reverse_gap **gaps)
{
    if (gaps) *gaps = source ? source->reverse_gaps : NULL;
    return source ? source->reverse_gap_count : 0;
}
sh_package_source_link *sh_package_source_link_open_attributed(const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_source_link_native *native,
    const sh_package_source_root_plan *root_plan, sh_package_source_failure *failure,
    char *error, size_t capacity)
{
    sh_package_source_link *s = calloc(1, sizeof(*s));
    sh_package_link_inputs roots = {0};
    char fallback[1024];
    unsigned long long mark = sl_now_ms();
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (failure) {
        failure->kind = SH_PACKAGE_SOURCE_FAILURE_NONE; failure->scope = SIZE_MAX;
        failure->family[0] = failure->name[0] = failure->chain[0] = 0;
    }
    if (!s) {
        if (failure) failure->kind = SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT;
        snprintf(error, capacity, "cannot allocate source linker"); return NULL;
    }
    s->failed_scope = SIZE_MAX;
    s->error = error; s->error_capacity = capacity; s->catalog = catalog;
    s->original = original; s->original_context = original_context;
    if (native) {
        s->native = *native;
        if (native->shader) { s->shader = *native->shader; s->native.shader = &s->shader; }
    }
    if ((count && !scopes) || count > SIZE_MAX / sizeof(*s->scopes) || count > SIZE_MAX / sizeof(*s->readers)) {
        s->environment = 1; goto failed;
    }
    s->scopes = calloc(count ? count : 1, sizeof(*s->scopes));
    s->readers = calloc(count ? count : 1, sizeof(*s->readers));
    if (!s->scopes || !s->readers) { s->environment = 1; goto failed; }
    s->scope_count = count;
    for (size_t i = 0; i < count; i++) {
        s->scopes[i] = scopes[i];
        s->readers[i] = sh_package_source_reader_open(scopes[i], catalog, sl_read_original, s,
            registry, reflection, error, capacity);
        if (!s->readers[i]) { s->environment = 1; goto failed; }
        sh_package_source_reader_lookups(s->readers[i], sl_lookup, s);
        if (s->native.shader && !sh_package_source_reader_shader(s->readers[i], s->native.shader)) {
            s->environment = 1; goto failed;
        }
    }
    s->store = &s->own; s->capture = 1;
    if (s->native.memo && g_sl_replay && count) {
        sh_package_source_memo *memo = s->native.memo;
        unsigned char key[32];
        if (_InterlockedCompareExchange(&memo->busy, 1, 0)) s->census.memo = 3;
        else if (!sl_memo_key(s, registry, key)) {
            /* A capture that cannot establish the key records only for itself. */
            _InterlockedExchange(&memo->busy, 0);
            if (s->error && s->error_capacity) *s->error = 0;
        } else {
            s->census.memo = 1;
            if (!memo->keyed || memcmp(memo->key, key, sizeof(key))) {
                if (memo->keyed) s->census.memo = 2;
                sl_store_clear(&memo->store); memcpy(memo->key, key, sizeof(key)); memo->keyed = 1;
            }
            s->memo = memo; s->store = &memo->store; s->capture = ++memo->captures;
        }
    }
    sl_phase(s, 0, &mark);
    if (!sl_seed_inputs(s)) goto failed;
    for (size_t i = 0; i < s->count; i++) { s->units[i]->seeded = 1; s->units[i]->seeded_witnesses = s->units[i]->unit.witness_count; }
    sl_phase(s, 1, &mark);
    if (!sl_player_contract(s)) { s->environment = 1; goto failed; }
    /* Supplied content first: a supplied file its reader cannot read, or a
     * lookup that needs native preparation, fails here instead of after the
     * whole stock reverse discovery. The closure captured is the same. */
    size_t inspected = 0;
    if (!sl_discover_through(s, &inspected)) goto failed;
    if (s->deferred.unit || *s->preparation_reason) {
        /* This attempt fails. When native preparation is pending anyway, first
         * let the stock observation queue what it needs too, so one
         * preparation round serves the next attempt; its own outcome is not
         * what this attempt reports. With nothing to prepare, a failure stops
         * here, before any stock root is read. */
        if (*s->preparation_reason) {
            char kept[1024];
            snprintf(kept, sizeof(kept), "%s", s->error ? s->error : "");
            (void)sl_reverse_roots(s, root_plan, registry, reflection);
            s->failed = 0; s->environment = 0;
            if (s->error && s->error_capacity) snprintf(s->error, s->error_capacity, "%s", kept);
            sl_phase(s, 2, &mark);
        }
        (void)sl_phase_complete(s); goto failed;
    }
    sl_phase(s, 3, &mark);
    /* Stock reverse discovery refuses only for I/O, allocation or setup. */
    if (!sl_reverse_roots(s, root_plan, registry, reflection)) { s->environment = 1; goto failed; }
    sl_phase(s, 2, &mark);
    for (int pass = 0; pass < 2; pass++) {
        if (pass && !sl_seed_orphan_binaries(s)) goto failed;
        if (!sl_discover_through(s, &inspected) || !sl_phase_complete(s)) goto failed;
    }
    s->native.withdrawals = NULL; /* Borrowed for discovery only. */
    sl_phase(s, 3, &mark);
    if (!sl_shader_accounted(s)) goto failed;
    sl_phase(s, 4, &mark);
    if (s->native.audio && !sh_package_audio_program_prepare(s->native.audio, error, capacity)) {
        /* The capture of one scope's sounds and banks failed: that scope's
         * supplied audio is responsible, not its peers. */
        s->failed_scope = sh_package_audio_program_failed_scope(s->native.audio);
        goto failed;
    }
    sl_phase(s, 5, &mark);
    /* Complete explicit source closure before preparing potentially large
     * implicit renderer/audio data. A missing reference or malformed source
     * must fail before any expensive native-provider capture. */
    for (size_t i = 0; s->native.semantics && i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (!unit->unit.file_count) continue;
        int original_bytes = s->native.semantics(s->native.context, unit->scope, unit->unit.family,
            unit->unit.name, unit->unit.semantics, error, capacity);
        if (original_bytes < 0 || original_bytes > 1) { s->failed_unit = unit; goto failed; }
        if (!original_bytes) sl_note_original(unit, "native semantics are not original");
        int campaign = 0, certified = original_bytes;
        for (size_t j = 0; original_bytes && j < unit->unit.file_count; j++) {
            sl_original *original = sl_original_get(s, unit->unit.files[j]->engine_path);
            size_t length = 0;
            if (!original || original->status < 0) goto failed;
            if (original->status != 1 && original->status != 2) {
                char note[400];
                snprintf(note, sizeof(note), "original %s has status %d", unit->unit.files[j]->engine_path, original->status);
                sl_note_original(unit, note);
                original_bytes = 0; break;
            }
            campaign |= original->status == 2;
            const sh_compiled_resource *certificate = unit->unprepared ? unit->unprepared : unit->unit.files[j];
            /* The captured original itself: its own bytes, nothing to compare. */
            if (certificate == &original->resource) continue;
            unsigned char *body = sh_package_compilation_read(unit->unit.compilation, certificate,
                SIZE_MAX, &length, error, capacity);
            if (!body) goto failed;
            original_bytes = original->resource.body_length == length && !memcmp(original->resource.body, body, length);
            if (!original_bytes && !unit->original_note) {
                /* The first differing offset and a sanitized window of both sides. */
                size_t at = 0, shorter = length < original->resource.body_length ? length : original->resource.body_length;
                while (at < shorter && body[at] == original->resource.body[at]) at++;
                char mine[49], theirs[49], note[512];
                size_t from = at > 16 ? at - 16 : 0;
                for (size_t k = 0; k < 48; k++) {
                    unsigned char a = from + k < length ? body[from + k] : ' ';
                    unsigned char o = from + k < original->resource.body_length ? original->resource.body[from + k] : ' ';
                    mine[k] = a >= 32 && a < 127 ? (char)a : '.'; theirs[k] = o >= 32 && o < 127 ? (char)o : '.';
                }
                mine[48] = theirs[48] = 0;
                snprintf(note, sizeof(note), "%s differs from its captured original (%zu vs %zu bytes, %s%s%s, %u sources, "
                    "first at %zu: [%s] vs [%s])", unit->unit.files[j]->engine_path, length, original->resource.body_length,
                    unit->unprepared ? "prepared" : "as bound", certificate->engine_path &&
                    strcmp(certificate->engine_path, unit->unit.files[j]->engine_path) ? " from " : "",
                    certificate->engine_path && strcmp(certificate->engine_path, unit->unit.files[j]->engine_path) ?
                    certificate->engine_path : "", (unsigned)certificate->source_count, at, mine, theirs);
                sl_note_original(unit, note);
            }
            free(body);
        }
        /* Campaign-only bytes a scope supplies keep their native name (the
         * engine derives some names from others), but they are that scope's
         * content, not SnapMap's: another scope may rely on their absence (an
         * engine default), so a SnapMap unit reaching them is not original. */
        if (original_bytes && campaign) original_bytes = 2;
        else if (!original_bytes && certified && !strcmp(unit->unit.family, "renderprog")) {
            /* A render program's name is its native identity and cannot be
             * relocated yet. One SnapMap does not ship (a re-cooked campaign
             * program, say) keeps the name its scope gives it the same way. */
            char *path = sl_decl_path("renderprog", unit->unit.name);
            if (!path) { sl_env_fail(s, "cannot construct render program path", unit->unit.name); goto failed; }
            sl_original *declaration = sl_original_get(s, path);
            free(path);
            if (!declaration || declaration->status < 0) goto failed;
            if (declaration->status != 1 && declaration->status != 3) original_bytes = 2;
        }
        unit->unit.original_binding = original_bytes;
    }
    sl_phase(s, 6, &mark);
    for (size_t i = 0; s->native.owners && i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (unit->unit.file_count && s->native.owners(s->native.context, unit->scope,
            unit->unit.family, unit->unit.name, &unit->unit.implicit_owners,
            &unit->unit.implicit_gameplay_owners, error, capacity) != 1) { s->failed_unit = unit; goto failed; }
    }
    sl_phase(s, 7, &mark);
    if (s->count > 1) qsort(s->units, s->count, sizeof(*s->units), sl_unit_order);
    if (s->count > SIZE_MAX / sizeof(*s->inputs.units) || s->count > SIZE_MAX / sizeof(*s->inputs.scopes)) goto failed;
    s->inputs.units = calloc(s->count ? s->count : 1, sizeof(*s->inputs.units));
    s->inputs.scopes = calloc(s->count ? s->count : 1, sizeof(*s->inputs.scopes));
    if (!s->inputs.units || !s->inputs.scopes) goto failed;
    for (size_t i = 0; i < s->count; i++) s->units[i]->index = i;
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        if (unit->site_count) {
            if (unit->site_count > SIZE_MAX / sizeof(*unit->site_edges) ||
                !(unit->site_edges = calloc(unit->site_count, sizeof(*unit->site_edges)))) goto failed;
            for (size_t j = 0; j < unit->site_count; j++)
                unit->site_edges[j] = (sh_package_link_edge){unit->sites[j].slot, unit->sites[j].target->index};
        }
        if (unit->implicit_count) {
            if (unit->implicit_count > SIZE_MAX / sizeof(sh_package_link_edge)) goto failed;
            sh_package_link_edge *edges = calloc(unit->implicit_count, sizeof(*edges));
            if (!edges) goto failed;
            size_t plain = 0, conditional = unit->implicit_count;
            /* Implicit first, conditional last, in one allocation. */
            for (size_t j = 0; j < unit->implicit_count; j++) {
                sh_package_link_edge edge = {unit->implicit[j].slot, unit->implicit[j].target->index};
                if (unit->implicit[j].conditional) edges[--conditional] = edge;
                else edges[plain++] = edge;
            }
            unit->implicit_edges = edges;
            unit->unit.implicit = plain ? edges : NULL; unit->unit.implicit_count = plain;
            unit->unit.conditional = plain < unit->implicit_count ? edges + plain : NULL;
            unit->unit.conditional_count = unit->implicit_count - plain;
        }
        s->inputs.units[i] = unit->unit; s->inputs.scopes[i] = unit->scope;
    }
    if (!sl_shared(s)) goto failed;
    sl_phase(s, 8, &mark);
    s->inputs.count = s->count; s->frozen = 1; s->error = NULL; s->error_capacity = 0;
    sh_package_link_inputs_free(&roots); return s;
failed:
    if (!*error) snprintf(error, capacity, "scoped source dependency closure could not be completed");
    if (failure) sl_attribute(s, failure);
    sh_package_link_inputs_free(&roots); sh_package_source_link_close(s); return NULL;
}
sh_package_source_link *sh_package_source_link_open_roots(const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_source_link_native *native,
    const sh_package_source_root_plan *root_plan, char *error, size_t capacity)
{
    return sh_package_source_link_open_attributed(scopes, count, catalog, original, original_context,
        registry, reflection, native, root_plan, NULL, error, capacity);
}
sh_package_source_link *sh_package_source_link_open(const sh_package_compilation *const *scopes, size_t count,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_source_link_native *native,
    char *error, size_t capacity)
{
    return sh_package_source_link_open_roots(scopes, count, catalog, original, original_context,
        registry, reflection, native, NULL, error, capacity);
}
const sh_package_link_inputs *sh_package_source_link_inputs(const sh_package_source_link *source)
{ return source ? &source->inputs : NULL; }
const char *sh_package_source_link_counters(const sh_package_source_link *s, char *out, size_t capacity)
{
    if (!out || !capacity) return out;
    *out = 0;
    if (!s) return out;
    int n = snprintf(out, capacity, "source link %zu units discovered, %zu replayed from another scope "
        "(%zu recorded, %zu declined); %zu file binds, %zu of unsupplied originals; %zu originals captured "
        "(%llu MB); reverse base %zu units; discovery: file selection %llu ms, reads %llu ms, "
        "constructor requests %llu ms; %zu replayed from earlier compiles (memo %s)", s->census.units,
        s->census.replayed, s->census.recorded,
        s->census.declined, s->census.binds, s->census.original_binds, s->census.originals,
        s->census.original_bytes >> 20, s->census.reverse_units, s->census.select_us / 1000u,
        s->census.read_us / 1000u, s->census.construct_us / 1000u, s->census.retained,
        s->census.memo == 1 ? "kept" : s->census.memo == 2 ? "renewed" : s->census.memo == 3 ? "busy" : "unused");
    /* The discovery sub-phases, on a line of their own. */
    if (n > 0 && (size_t)n < capacity)
        snprintf(out + n, capacity - (size_t)n, "\nsource link detail: material companions %llu ms within file "
            "selection; within reads, replay validation %llu ms over %zu recordings, replay application %llu ms, "
            "fresh reads %llu ms; originals captured in %llu ms and hashed in %llu ms",
            s->census.companion_us / 1000u, s->census.applies_us / 1000u, s->census.replay_tries,
            s->census.apply_us / 1000u, s->census.fresh_us / 1000u, s->census.original_us / 1000u,
            s->census.identify_us / 1000u);
    return out;
}
int sh_package_source_link_unit_replayed(const sh_package_source_link *s, size_t index)
{ return s && s->frozen && index < s->count && s->units[index]->replayed; }
int sh_package_source_link_identity(sh_package_source_link *s, size_t index,
    const sh_compiled_resource *file, sh_package_file_identity *out, char *error, size_t capacity)
{
    sl_original *original = s && file ? sl_original_peek(s, file->engine_path) : NULL;
    if (original && &original->resource == file && original->resource.body) {
        if (!original->identified) {
            if (!sh_package_bytes_identity(original->resource.body, original->resource.body_length, &original->identity)) {
                if (out) memset(out, 0, sizeof(*out));
                if (error && capacity) snprintf(error, capacity, "cannot identify captured original: %s", file->engine_path);
                return 0;
            }
            original->identified = 1;
        }
        *out = original->identity; return 1;
    }
    if (!s || index >= s->count) {
        if (out) memset(out, 0, sizeof(*out));
        if (error && capacity) snprintf(error, capacity, "resource identity has no source unit");
        return 0;
    }
    return sh_package_compilation_identity(s->units[index]->unit.compilation, file, out, error, capacity);
}
const char *sh_package_source_link_phases(const sh_package_source_link *source, char *out, size_t capacity)
{
    size_t used = 0;
    if (!out || !capacity) return out;
    *out = 0;
    for (int i = 0; source && i < 9 && used < capacity; i++) {
        int n = snprintf(out + used, capacity - used, "%s%s %llu ms", i ? ", " : "", sl_phase_names[i], source->phase_ms[i]);
        if (n < 0) break;
        used += (size_t)n;
    }
    return out;
}
const unsigned char *sh_package_source_link_shared(const sh_package_source_link *source)
{ return source && source->frozen ? source->shared : NULL; }
const char *sh_package_source_link_original_note(const sh_package_source_link *source, size_t index)
{
    if (!source || index >= source->count || !source->units[index] || !source->units[index]->original_note) return "";
    return source->units[index]->original_note;
}
typedef struct sl_compile { sh_package_source_link *source; const sh_package_link_adapter *native; } sl_compile;
int sh_package_source_link_sites(const sh_package_source_link *s, size_t index,
    const sh_compiled_resource *file, const sh_package_link_edge **sites, size_t *count)
{
    size_t k = 0, begin = 0, end;
    if (sites) *sites = NULL;
    if (count) *count = 0;
    if (!s || !s->frozen || index >= s->count || !file || !sites || !count) return 0;
    const sl_unit *unit = s->units[index];
    while (k < unit->discovered_count && unit->discovered[k] != file) k++;
    if (k == unit->discovered_count) return 0;
    /* A file's sites were recorded contiguously while it was bound. */
    while (begin < unit->site_count && unit->sites[begin].file != file) begin++;
    for (end = begin; end < unit->site_count && unit->sites[end].file == file; end++);
    *sites = end > begin ? unit->site_edges + begin : NULL; *count = end - begin;
    return 1;
}
const char *sh_package_source_link_fsm_config(const sh_package_source_link *s, size_t unit)
{
    return s && s->frozen && unit < s->count ? s->units[unit]->fsm_config : NULL;
}
int sh_package_source_link_fsm_stock(const sh_package_source_link *s, const char *name)
{
    return s && name && s->fsm_names_ready && sl_fsm_listed(s, name);
}
int sh_package_source_link_find_unit(sh_package_source_link *s, size_t scope,
    const char *family, const char *name, size_t *unit)
{
    sl_unit *found = s && s->frozen && scope < s->scope_count && family && *family && name && *name ?
        sl_find(s, scope, family, name, 0) : NULL;
    if (unit) *unit = found ? found->index : SIZE_MAX;
    return found != NULL;
}
int sh_package_source_link_resolve_unit(sh_package_source_link *s, size_t unit,
    const char *family, const char *name, size_t *target, char *error, size_t capacity)
{
    char fallback[1024]; sl_unit *found = NULL;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (target) *target = SIZE_MAX;
    if (s && s->frozen && target && unit < s->count && family && *family && name && *name)
        found = sl_find(s, s->units[unit]->scope, family, name, 0);
    if (!found) {
        snprintf(error, capacity, "binding is outside the captured source scope: %s/%s",
            family ? family : "", name ? name : ""); return 0;
    }
    *target = found->index; return 1;
}

unsigned char *sh_package_source_link_bind_derived(sh_package_source_link *s, size_t index,
    const unsigned char *body, size_t length, sh_package_binding_resolver reference,
    void *reference_context, sh_package_bound_exists exists, void *exists_context,
    size_t *output_length, char *error, size_t capacity)
{
    char fallback[1024]; unsigned char *out;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output_length) *output_length = 0;
    if (!s || !s->frozen || index >= s->count || !body || !reference || !output_length) {
        snprintf(error, capacity, "binding requires a captured source unit"); return NULL;
    }
    sl_unit *unit = s->units[index];
    char *old_error = s->error; size_t old_capacity = s->error_capacity;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    out = sh_package_source_bind_derived(s->readers[unit->scope], unit->unit.family, unit->unit.name,
        body, length, reference, reference_context, exists, exists_context, output_length, NULL, error, capacity);
    if (s->failed) { free(out); out = NULL; *output_length = 0; }
    s->error = old_error; s->error_capacity = old_capacity; return out;
}

unsigned char *sh_package_source_link_bind_unit(sh_package_source_link *s, size_t index,
    const unsigned char *body, size_t length, sh_package_binding_resolver reference,
    void *reference_context, size_t *output_length, char *error, size_t capacity)
{
    return sh_package_source_link_bind_derived(s, index, body, length, reference,
        reference_context, NULL, NULL, output_length, error, capacity);
}

int sh_package_source_link_key_space(sh_package_source_link *s, sh_package_key_space *space)
{
    if (!s || !s->scope_count || !s->readers || !s->readers[0]) return -1;
    return sh_package_source_key_space(s->readers[0], space);
}

int sh_package_source_link_bind_keys(sh_package_source_link *s, size_t index,
    const unsigned char *body, size_t length, sh_package_key_site_resolver resolver, void *context,
    unsigned char **output, size_t *output_length, char *error, size_t capacity)
{
    char fallback[1024]; int status;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output) *output = NULL;
    if (output_length) *output_length = 0;
    if (!s || !s->frozen || index >= s->count || !body || !resolver || !output || !output_length) {
        snprintf(error, capacity, "registration keys require a captured source unit"); return -1;
    }
    sl_unit *unit = s->units[index];
    char *old_error = s->error; size_t old_capacity = s->error_capacity;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    status = sh_package_source_bind_keys(s->readers[unit->scope], unit->unit.family, unit->unit.name,
        body, length, resolver, context, output, output_length, NULL, error, capacity);
    if (s->failed && status == 1) { free(*output); *output = NULL; *output_length = 0; status = -1; }
    s->error = old_error; s->error_capacity = old_capacity; return status;
}

unsigned char *sh_package_source_link_bind_strings(sh_package_source_link *s, size_t index,
    const unsigned char *body, size_t length, sh_package_string_site_resolver reference,
    void *context, size_t *output_length, char *error, size_t capacity)
{
    char fallback[1024]; unsigned char *out;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (output_length) *output_length = 0;
    if (!s || !s->frozen || index >= s->count || !body || !reference || !output_length) {
        snprintf(error, capacity, "localization requires a captured source unit"); return NULL;
    }
    sl_unit *unit = s->units[index];
    char *old_error = s->error; size_t old_capacity = s->error_capacity;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    out = sh_package_source_bind_strings(s->readers[unit->scope], unit->unit.family, unit->unit.name,
        body, length, reference, context, output_length, NULL, error, capacity);
    if (s->failed) { free(out); out = NULL; *output_length = 0; }
    s->error = old_error; s->error_capacity = old_capacity; return out;
}

unsigned char *sh_package_source_link_bind_file(sh_package_source_link *s, size_t index,
    const sh_compiled_resource *file, const unsigned char *body, size_t length,
    sh_package_binding_resolver reference, void *context, size_t *output_length,
    char *error, size_t capacity)
{
    char fallback[1024]; unsigned char *out = NULL;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0; if (output_length) *output_length = 0;
    if (!s || !s->frozen || index >= s->count || !file || !body || !reference || !output_length) {
        snprintf(error, capacity, "file binding requires a captured source unit"); return NULL;
    }
    sl_unit *unit = s->units[index]; int member = 0;
    for (size_t i = 0; i < unit->unit.file_count; i++) if (unit->unit.files[i] == file) member = 1;
    if (!member) { snprintf(error, capacity, "file is outside its captured source unit"); return NULL; }
    char *old_error = s->error; size_t old_capacity = s->error_capacity;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    out = sl_bind_member(s, unit, file, body, length, reference, context, output_length, error, capacity);
    if (s->failed) { free(out); out = NULL; *output_length = 0; }
    s->error = old_error; s->error_capacity = old_capacity; return out;
}
typedef struct sl_shader_linked_paths {
    sl_unit *unit;
    char **names;
    sh_package_shader_path_visitor visitor;
    void *context;
} sl_shader_linked_paths;
static int sl_shader_linked_path(void *context, sh_package_shader_path_kind kind,
    const char *original, const char *linked)
{
    sl_shader_linked_paths *walk = context;
    if (kind == SH_PACKAGE_SHADER_PERMUTATION) {
        size_t p;
        for (p = 0; p < walk->unit->program_count; p++) if (!strcmp(walk->unit->programs[p].name, original)) break;
        if (p == walk->unit->program_count) return 0;
        if (walk->names[p] && strcmp(walk->names[p], linked)) return 0;
        if (!walk->names[p] && !(walk->names[p] = _strdup(linked))) return 0;
    }
    return walk->visitor(walk->context, kind, original, linked);
}
int sh_package_source_link_shader_paths(sh_package_source_link *s, size_t index,
    const char *name, sh_package_shader_path_visitor visitor, void *context, char *error, size_t capacity)
{
    char fallback[1024]; int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!s || !s->frozen || index >= s->count || !name || !visitor || !s->native.shader ||
        strcmp(s->units[index]->unit.family, "renderprog") || !s->units[index]->program_count) {
        snprintf(error, capacity, "shader paths require a captured file-backed program unit"); return 0;
    }
    sl_unit *unit = s->units[index];
    char **names = calloc(unit->program_count, sizeof(*names));
    if (!names) goto done;
    names[0] = _strdup(name);
    sl_shader_linked_paths walk = {unit, names, visitor, context};
    for (size_t p = 0; p < unit->program_count; p++) {
        if (!names[p] || sh_package_shader_paths(s->native.shader->renderer, unit->programs[p].name,
            names[p], &unit->programs[p].decoded, sl_shader_linked_path, &walk, error, capacity) != SH_PACKAGE_SHADER_OK) goto done;
    }
    ok = 1;
done:
    if (names) for (size_t p = 0; p < unit->program_count; p++) free(names[p]);
    free(names);
    if (!ok && !*error) snprintf(error, capacity, "shader paths exceed captured identity or allocation bounds");
    return ok;
}

static int sl_resolve(void *context, size_t unit, const char *family, const char *name,
    size_t *target, char *error, size_t capacity)
{
    sl_compile *compile = context;
    return sh_package_source_link_resolve_unit(compile->source, unit, family, name, target, error, capacity);
}
static unsigned char *sl_bind(void *context, size_t index, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_link_reference reference, void *reference_context,
    size_t *output_length, char *error, size_t capacity)
{
    sl_compile *compile = context;
    return sh_package_source_link_bind_file(compile->source, index, file, body, length,
        reference, reference_context, output_length, error, capacity);
}

int sh_package_source_link_identify(sh_package_source_link *s,
    sh_package_link_classes *out, char *error, size_t capacity)
{
    char fallback[1024];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!s || !s->frozen) {
        sh_package_link_classes_free(out);
        snprintf(error, capacity, "resource identity requires a captured source scope"); return 0;
    }
    sl_compile compile = {s, NULL};
    sh_package_link_adapter adapter = {&compile, sl_resolve, sl_bind, NULL, NULL};
    return sh_package_link_identify_units(s->inputs.units, s->inputs.count, &adapter, out, error, capacity);
}
static int sl_assign(void *context, size_t unit, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths, char *error, size_t capacity)
{
    sl_compile *compile = context;
    return compile->native->assign(compile->native->context, unit, identity, original_binding,
        files, count, name, paths, error, capacity);
}
static int sl_assign_members(void *context, size_t unit, const char *name,
    const sh_compiled_resource *const *files, const char *const *paths, size_t count,
    char ***names, char *error, size_t capacity)
{
    sl_compile *compile = context;
    return compile->native->assign_members(compile->native->context, unit, name,
        files, paths, count, names, error, capacity);
}
int sh_package_source_link_compile(sh_package_source_link *s, const sh_package_sources *inventory,
    const sh_package_link_adapter *native, const sh_package_policy *policy, char *(*canonical_path)(const char *),
    sh_package_link_output *out, char *error, size_t capacity)
{
    char fallback[1024];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!s || !s->frozen || !native || !native->assign) {
        sh_package_link_output_free(out); snprintf(error, capacity, "source linker has no admitted native assignment"); return 0;
    }
    sl_compile compile = {s, native};
    sh_package_link_adapter adapter = {&compile, sl_resolve, sl_bind, sl_assign, native->registry};
    if (native->assign_members) adapter.assign_members = sl_assign_members;
    s->error = error; s->error_capacity = capacity; s->failed = 0;
    int ok = sh_package_link(inventory, s->inputs.units, s->inputs.count, &adapter, policy,
        canonical_path, out, error, capacity);
    s->error = NULL; s->error_capacity = 0; return ok;
}
