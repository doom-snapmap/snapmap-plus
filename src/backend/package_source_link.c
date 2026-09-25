#include "package_source_link.h"
#include "package_keys.h"
#include "package_virtual_material.h"
#include "package_font.h"
#include "package_movie.h"
#include "package_audio_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
typedef struct sl_implicit { char *slot; struct sl_unit *target; } sl_implicit;
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
    size_t reverse_gap; /* Observation only: 1 + index of its accounted reader gap. */
    struct sl_unit *parent; /* Unit whose discovery first requested it; NULL for seeds. */
    char *seed; /* Why a unit without a same-scope parent entered the capture. */
    int nominated; /* Seeded as a stock root affected by its scope's changes. */
    int movie_captured; /* Constructor requests of a cooked movie were enumerated. */
    sh_package_link_file_identity *identities; /* Owned per-file identities (movie pairs). */
} sl_unit;
typedef struct sl_original {
    sh_compiled_resource resource;
    int status;
} sl_original;
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
    int malformed; /* That reader could not parse the text in the native grammar. */
    int environment; /* A failure came from I/O, allocation or configuration. */
    sl_unit *discovering; /* Unit whose readers are requesting new units. */
    const sl_unit *failed_unit; /* Unit whose strict discovery failed, if known. */
    size_t failed_scope; /* Input scope whose supplied files failed seeding. */
    char seed_note[320]; /* Current seeding site, copied into units it creates. */
    unsigned long long phase_ms[9]; /* Wall-clock capture phases, see sl_phase_names. */
};
static const char *const sl_phase_names[9] = {
    "readers", "seeds", "reverse roots", "discovery", "shaders", "audio", "certification", "owners", "inputs"
};
static unsigned long long sl_now_ms(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000u + (unsigned long long)now.tv_nsec / 1000000u : 0;
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
    file->status = s->original ? s->original(s->original_context, path,
        &file->resource.body, &file->resource.body_length) : 0;
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
        (void)sh_package_source_shader_open(s->readers[unit->scope], unit->unit.name,
            &declaration, &declaration_length, &binary, &binary_length, &decoded, s->error, s->error_capacity);
        free(declaration); free(binary); sh_package_shader_program_clear(&decoded);
    }
    if (probe.declaration != 1 || probe.cooked != 1)
        return sl_gap_fail(s, "shader declaration and selected renderer binary must both be supplied", unit->unit.name);
    if (sl_shader_program_add(s, unit, unit->unit.name, SIZE_MAX, 0) == SIZE_MAX) return 0;
    for (size_t p = 0; p < unit->program_count; p++) {
        unsigned char *declaration = NULL, *binary = NULL; size_t declaration_length = 0, binary_length = 0;
        sh_package_shader_program decoded = {0};
        const char *name = unit->programs[p].name;
        if (!sh_package_source_shader_open(s->readers[unit->scope], name, &declaration, &declaration_length,
            &binary, &binary_length, &decoded, s->error, s->error_capacity)) return 0;
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
        int ok = s->native.material_companions(s->native.context, unit->scope, unit->unit.name,
            sl_material_member, &walk, s->error, s->error_capacity);
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
            if (member->file == file)
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
int sh_package_source_link_ancillary(const char *path)
{
    return path && !strncmp(path, "generated/transsortatlas/", 25);
}
static const char *sl_deferred_binary_family(sh_package_source_link *s, const sh_compiled_resource *file)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    size_t n = strlen(file->engine_path);
    if (file->type || n <= 7 || sh_package_source_link_ancillary(file->engine_path) ||
        sh_resource_catalog_find_path(s->catalog, file->engine_path, &rows)) return NULL;
    if (sl_font_stem(file->engine_path)) return "font";
    if (!strcmp(file->engine_path + n - 7, ".bimage")) return "image";
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
            if (sh_package_source_link_ancillary(file->engine_path)) continue;
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
        if (!found) { s->failed_scope = scope; return sl_fail(s, "shader source input has no verified companion owner", path); }
    }
    return 1;
}
typedef struct sl_discover { sh_package_source_link *source; sl_unit *unit; } sl_discover;
static int sl_reference(void *context, const char *slot, const char *family, const char *name, const char **replacement)
{
    sl_discover *walk = context;
    sl_unit *unit = walk->unit;
    sl_unit *target = sl_find(walk->source, unit->scope, family, name, 1);
    (void)slot; *replacement = NULL;
    if (!target) return -1;
    if (!sl_grow((void ***)&unit->dependencies, &unit->dependency_capacity, unit->dependency_count)) {
        sl_env_fail(walk->source, "cannot retain scoped dependency", name); return -1;
    }
    unit->dependencies[unit->dependency_count++] = target;
    return 0;
}
static int sl_implicit_reference(void *context, const char *slot, const char *family, const char *name)
{
    sl_discover *walk = context; sl_unit *unit = walk->unit;
    if (!slot || !*slot) return sl_fail(walk->source, "implicit native request has no semantic slot", name);
    sl_unit *target = sl_find(walk->source, unit->scope, family, name, 1);
    if (!target) return 0;
    for (size_t i = 0; i < unit->implicit_count; i++) if (!strcmp(unit->implicit[i].slot, slot))
        return unit->implicit[i].target == target ? 1 :
            sl_fail(walk->source, "implicit native slot has contradictory targets", slot);
    if (unit->implicit_count >= SIZE_MAX / sizeof(*unit->implicit))
        return sl_env_fail(walk->source, "too many implicit native requests", name);
    char *copy = _strdup(slot);
    sl_implicit *grown = copy ? realloc(unit->implicit, (unit->implicit_count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(copy); return sl_env_fail(walk->source, "cannot retain implicit native request", name); }
    unit->implicit = grown;
    unit->implicit[unit->implicit_count++] = (sl_implicit){copy, target};
    if (!sl_grow((void ***)&unit->dependencies, &unit->dependency_capacity, unit->dependency_count))
        return sl_env_fail(walk->source, "cannot retain implicit native dependency", name);
    unit->dependencies[unit->dependency_count++] = target;
    return 1;
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
static int sl_discover_unit_requests(sh_package_source_link *s, sl_unit *unit)
{
    if (!sl_files(s, unit)) {
        if (s->observe && !s->failed) s->reader_gap = 1;
        return 0;
    }
    for (size_t j = 0; j < unit->unit.file_count; j++) {
        size_t length = 0, bound_length = 0;
        unsigned char *body = sh_package_compilation_read(unit->unit.compilation, unit->unit.files[j],
            SIZE_MAX, &length, s->error, s->error_capacity), *bound = NULL;
        sl_discover walk = {s, unit};
        if (body) bound = sl_bind_member(s, unit, unit->unit.files[j],
            body, length, sl_reference, &walk, &bound_length, s->error, s->error_capacity);
        int ok = body && bound && length == bound_length && !memcmp(body, bound, length) && !s->failed;
        if (!ok && body && !bound && !s->failed) s->reader_gap = 1;
        if (!body) s->environment = 1; /* An unreadable captured file blames no input. */
        free(body); free(bound);
        if (!ok) return 0;
    }
    if (!strcmp(unit->unit.family, "font") && !sl_font_capture(s, unit)) return 0;
    sl_discover walk = {s, unit};
    if (s->native.implicit && s->native.implicit(s->native.context, unit->scope, s->observe,
        &unit->unit, sl_implicit_reference, &walk, s->error, s->error_capacity) != 1)
        return sl_fail(s, "native constructor dependencies could not be captured", unit->unit.name);
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
static int sl_reverse_file_changed(sh_package_source_link *s, size_t scope, const char *path)
{
    const sh_compiled_resource *file = path && *path ? sh_package_compilation_find(s->scopes[scope], path) : NULL;
    return file && sh_package_owners_count(&file->gameplay_owners) != 0;
}
static int sl_reverse_changed(sh_package_source_link *s, size_t scope, const sl_unit *unit)
{
    if (!strcmp(unit->unit.family, "sound") &&
        sh_package_audio_program_affected(s->native.audio, scope, unit->unit.name)) return 1;
    const sl_unit *supplied = sl_find(s, scope, unit->unit.family, unit->unit.name, 0);
    for (size_t i = 0; supplied && i < supplied->unit.file_count; i++)
        if (sh_package_owners_count(&supplied->unit.files[i]->gameplay_owners)) return 1;
    for (size_t i = 0; supplied && i < supplied->unit.witness_count; i++)
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
    if (!needed || !sh_resource_catalog_count(s->catalog)) return 1;
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
    base->native.shader = s->native.shader;
    base->readers[0] = sh_package_source_reader_open(&empty, s->catalog, sl_read_original, s,
        registry, reflection, s->error, s->error_capacity);
    if (!base->readers[0] || (s->native.shader &&
        !sh_package_source_reader_shader(base->readers[0], s->native.shader))) goto done;
    /* PaletteBuild enumerates the snapEditorEntityDef manager. Campaign-only
     * entries and generic entity declarations are not additional palette roots. */
    for (size_t i = 0; i < sh_resource_catalog_count(s->catalog); i++) {
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(s->catalog, i);
        if (row->archive < 2 && !strcmp(row->type, "snapeditorentitydef") && *row->name &&
            !sl_find(base, 0, row->type, row->name, 1)) goto done;
    }
    roots = base->count;
    for (size_t i = 0; i < base->count; i++) {
        sl_unit *unit = base->units[i];
        base->reader_gap = 0;
        if (!sl_reverse_leaf(unit->unit.family) && !sl_discover_unit(base, unit)) {
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
        for (size_t i = 0; i < base->count; i++)
            if (sl_reverse_changed(s, scope, base->units[i])) local[i] = 0;
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

void sh_package_source_link_close(sh_package_source_link *s)
{
    if (!s) return;
    for (size_t i = 0; s->readers && i < s->scope_count; i++) sh_package_source_reader_close(s->readers[i]);
    for (size_t i = 0; i < s->count; i++) {
        sl_unit *unit = s->units[i];
        free(unit->seed);
        free((char *)unit->unit.family); free((char *)unit->unit.name); free((void *)unit->unit.files);
        for (size_t w = 0; w < unit->unit.witness_count; w++) free((char *)unit->unit.witnesses[w].role);
        free((void *)unit->unit.witnesses);
        for (size_t j = 0; j < unit->implicit_count; j++) free(unit->implicit[j].slot);
        free(unit->implicit); free((void *)unit->unit.implicit);
        for (size_t p = 0; p < unit->program_count; p++) {
            free(unit->programs[p].name); sh_package_shader_program_clear(&unit->programs[p].decoded);
        }
        free(unit->programs); free(unit->shader_files);
        if (unit->identities) for (size_t j = 0; j < unit->unit.file_count; j++) free((char *)unit->identities[j].name);
        free(unit->identities);
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
    free(s->originals); free(s->original_slots); free(s->units); free(s->unit_slots);
    free(s->inputs.units); free(s->inputs.scopes); free(s->readers); free(s->scopes); free(s->shared); free(s);
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
        if (s->native.shader && !sh_package_source_reader_shader(s->readers[i], s->native.shader)) {
            s->environment = 1; goto failed;
        }
    }
    sl_phase(s, 0, &mark);
    if (!sl_seed_inputs(s)) goto failed;
    sl_phase(s, 1, &mark);
    /* Stock reverse discovery refuses only for I/O, allocation or setup. */
    if (!sl_reverse_roots(s, root_plan, registry, reflection)) { s->environment = 1; goto failed; }
    sl_phase(s, 2, &mark);
    size_t inspected = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass && !sl_seed_orphan_binaries(s)) goto failed;
        for (; inspected < s->count; inspected++) {
            sl_unit *unit = s->units[inspected];
            s->reader_gap = s->malformed = 0;
            if (!sl_discover_unit(s, unit)) { s->failed_unit = unit; goto failed; }
            if (unit->unit.file_count > 1) qsort((void *)unit->unit.files, unit->unit.file_count, sizeof(*unit->unit.files), sl_file_order);
            if (!sl_movie_identities(s, unit)) { s->failed_unit = unit; goto failed; }
        }
    }
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
        int campaign = 0, certified = original_bytes;
        for (size_t j = 0; original_bytes && j < unit->unit.file_count; j++) {
            sl_original *original = sl_original_get(s, unit->unit.files[j]->engine_path);
            size_t length = 0;
            if (!original || original->status < 0) goto failed;
            if (original->status != 1 && original->status != 2) { original_bytes = 0; break; }
            campaign |= original->status == 2;
            unsigned char *body = sh_package_compilation_read(unit->unit.compilation, unit->unit.files[j],
                SIZE_MAX, &length, error, capacity);
            if (!body) goto failed;
            original_bytes = original->resource.body_length == length && !memcmp(original->resource.body, body, length);
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
        if (unit->implicit_count) {
            if (unit->implicit_count > SIZE_MAX / sizeof(sh_package_link_edge)) goto failed;
            sh_package_link_edge *edges = calloc(unit->implicit_count, sizeof(*edges));
            if (!edges) goto failed;
            unit->unit.implicit = edges; unit->unit.implicit_count = unit->implicit_count;
            for (size_t j = 0; j < unit->implicit_count; j++)
                edges[j] = (sh_package_link_edge){unit->implicit[j].slot, unit->implicit[j].target->index};
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
typedef struct sl_compile { sh_package_source_link *source; const sh_package_link_adapter *native; } sl_compile;
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
