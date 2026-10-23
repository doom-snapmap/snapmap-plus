#include "package_library.h"
#include "package_keys.h"
#include "package_spawn_registry.h"
#include "package_native_names.h"
#include "decl_server_path.h"
#include "package_native_registry.h"
#include "package_hud.h"
#include "package_sound.h"
#include "decl_native_rebind.h"
#include "decl_block_compose.h"
#include "package_movie.h"
#include "swf_binary.h"
#include "decl_tree.h"
#include "decl_entity_class.h"
#include <ctype.h>
#include "package_editor_rows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct pl_original {
    char *path;
    const unsigned char *body; /* owner's bytes; NULL when none, or while this entry is detached (below) */
    size_t length;
    int status;
    sh_package_body *owner; /* one reference to the kept body (sh_package_body) */
} pl_original;
/* The reader again, also naming where its bytes can be read again exactly (sh_package_library_native). */
typedef int (*pl_backed_reader)(void *context, const char *path, unsigned char **body, size_t *length,
    sh_package_backing *backing);
typedef struct pl_originals {
    sh_package_baseline_reader read;
    void *context;
    pl_backed_reader backed; /* or NULL */
    pl_original *files;
    size_t count, capacity;
    struct pl_originals *peer; /* the compile's other snapshot slot, or NULL */
} pl_originals;
typedef struct pl_contributions pl_contributions;
static void pl_contributions_close(pl_contributions *c);
struct pl_contributions;
static int pl_unit_keep(struct pl_contributions *c, size_t unit);

#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests: refuse the nth body the snapshot would keep (0: none), as an exhausted heap would. */
static long g_pl_original_refusal;
void sh_package_library_test_original_refusal(long nth) { g_pl_original_refusal = nth; }
long sh_package_library_test_original_refusal_left(void) { return g_pl_original_refusal; }
#define PL_ORIGINAL_REFUSED() (g_pl_original_refusal > 0 && !--g_pl_original_refusal)
#else
#define PL_ORIGINAL_REFUSED() 0
#endif
/* Tests: 0 keeps every private copy the compile made before R-341 (snapshot slots, the capture's originals, scope
 * compilations and their views, staged contributions); the same result. */
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
static int g_pl_share = 1;
static size_t g_pl_shared, g_pl_views_shared;
void sh_package_library_test_share(int enabled) { g_pl_share = enabled != 0; }
size_t sh_package_library_test_shared(void) { return g_pl_shared; }
size_t sh_package_library_test_views_shared(void) { return g_pl_views_shared; }
#define PL_SHARE g_pl_share
#else
#define PL_SHARE 1
#endif
/* One original snapshot covers both library passes and native material
 * inspection. Retain absence and failures too: retries inside this compilation
 * must not observe a different original/schema halfway through deduplication.
 * Returns the snapshot of path, read on first use, or NULL when it cannot be
 * retained. A kept body is NUL-terminated and held as a shared body: what serves
 * it unchanged takes a reference instead of a copy, and the bytes the other slot
 * keeps at the same path are shared when they are exactly equal.
 * A slot's backed reader also names where the bytes it returned can be read
 * again exactly (an installed archive row the compile's catalog pins): a
 * non-declaration body keeps that backing, and once the capture and the
 * snapshot only read it through the body (pl_originals_detach) its bytes go
 * until a reader needs them again (R-342). Declarations stay resident. */
static void pl_backing_release(sh_package_backing *backing)
{
    if (backing->release) backing->release(backing->context);
    memset(backing, 0, sizeof(*backing));
}
static int pl_original_releasable(const char *path)
{
    size_t n = strlen(path);
    return n < 5 || _stricmp(path + n - 5, ".decl");
}
static pl_original *pl_original_at(pl_originals *s, const char *path);
static pl_original *pl_original_find(pl_originals *s, const char *path)
{
    size_t low = 0, high = s->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (strcmp(s->files[mid].path, path) < 0) low = mid + 1; else high = mid;
    }
    if (low == s->count || strcmp(s->files[low].path, path)) {
        pl_original file = {0};
        if (s->count == s->capacity) {
            size_t next = s->capacity ? s->capacity * 2 : 64;
            pl_original *grown = next > s->capacity && next <= SIZE_MAX / sizeof(*grown) ?
                realloc(s->files, next * sizeof(*grown)) : NULL;
            if (!grown) return NULL;
            s->files = grown; s->capacity = next;
        }
        unsigned char *bytes = NULL;
        sh_package_backing backing = {0};
        file.path = _strdup(path); if (!file.path) return NULL;
        file.status = s->backed ? s->backed(s->context, path, &bytes, &file.length, &backing) :
            s->read ? s->read(s->context, path, &bytes, &file.length) : 0;
        if (file.status < 0 || file.status > 3 || (file.status && !bytes) || file.length == SIZE_MAX)
            file.status = -1;
        if (file.status <= 0) { free(bytes); file.length = 0; pl_backing_release(&backing); }
        else {
            const pl_original *twin = s->peer ? pl_original_at(s->peer, path) : NULL;
            /* The twin's bytes while any holder keeps them (its own entry may be detached). */
            const unsigned char *shared = twin && twin->owner ? sh_package_body_bytes(twin->owner) : NULL;
            if (PL_ORIGINAL_REFUSED()) { free(bytes); free(file.path); pl_backing_release(&backing); return NULL; }
            if (PL_SHARE && shared && twin->length == file.length && !memcmp(shared, bytes, file.length)) {
                free(bytes); file.owner = sh_package_body_retain(twin->owner);
            } else if (!(file.owner = sh_package_body_adopt(bytes, file.length))) {
                free(bytes); free(file.path); pl_backing_release(&backing); return NULL;
            }
            /* The backing read exactly these bytes, equal to the body's (the same read, or the twin compare). */
            if (backing.read && pl_original_releasable(path)) sh_package_body_back(file.owner, backing);
            else pl_backing_release(&backing);
            /* Borrowers read it as the copies were: NUL-terminated (sh_package_body_adopt). */
            file.body = sh_package_body_bytes(file.owner);
        }
        memmove(s->files + low + 1, s->files + low, (s->count - low) * sizeof(*s->files));
        s->files[low] = file; s->count++;
    }
    return s->files + low;
}
/* The snapshot of path when one was read, without reading it. */
static pl_original *pl_original_at(pl_originals *s, const char *path)
{
    size_t low = 0, high = s->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (strcmp(s->files[mid].path, path) < 0) low = mid + 1; else high = mid;
    }
    return low < s->count && !strcmp(s->files[low].path, path) ? s->files + low : NULL;
}
static int pl_read_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const pl_original *file = pl_original_find(context, path);
    *body = NULL; *length = 0;
    if (!file) return -1;
    if (file->status <= 0) return file->status;
    /* A detached entry reads its body again (a failed read is recorded on it: pl_originals_failed). */
    if (!file->body) return (*body = sh_package_body_read(file->owner, length)) != NULL ? file->status : -1;
    *body = malloc(file->length + 1); if (!*body) return -1;
    memcpy(*body, file->body, file->length); (*body)[file->length] = 0;
    *length = file->length; return file->status;
}
/* Room for count units and the contribution phase's own: clones and registration copies append a few hundred to a
 * complete library of about a million (R-339), so the unit-indexed tables grow by a sixty-fourth plus 1024 units
 * rather than doubling; pl_spawn_reserve grows them the same way when appended units outrun the room. */
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
static size_t g_pl_unit_room = SIZE_MAX, g_pl_unit_growths;
void sh_package_library_test_unit_room(size_t extra) { g_pl_unit_room = extra; }
size_t sh_package_library_test_unit_growths(void) { return g_pl_unit_growths; }
#define PL_UNIT_EXTRA(count) (g_pl_unit_room != SIZE_MAX ? g_pl_unit_room : (count) / 64 + 1024)
#define PL_UNIT_GREW() (g_pl_unit_growths++)
#else
#define PL_UNIT_EXTRA(count) ((count) / 64 + 1024)
#define PL_UNIT_GREW() ((void)0)
#endif
static size_t pl_unit_room(size_t count)
{
    size_t extra = PL_UNIT_EXTRA(count);
    return count <= SIZE_MAX / 2 / sizeof(sh_package_link_unit) - extra ? count + extra : 0;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
static int g_pl_release_order = 1;
void sh_package_library_test_release_order(int enabled) { g_pl_release_order = enabled != 0; }
#define PL_RELEASE_ORDER g_pl_release_order
#else
#define PL_RELEASE_ORDER 1
#endif
/* The borrowing form for the source link (sh_package_source_link_native.original_view): the snapshot's own
 * bytes, unchanged until pl_close releases the snapshot after closing the link, and their shared owner. What
 * serves a captured original unchanged, at its own path or a private one, then takes a reference to these bytes
 * instead of a copy of them, and keeps them after pl_close; replacing them (pv_select_pages) drops only its own
 * reference. */
static int pl_view_original(void *context, const char *path, const unsigned char **body, size_t *length,
    sh_package_body **owner)
{
    pl_original *file = pl_original_find(context, path);
    *body = NULL; *length = 0; *owner = NULL;
    if (!file) return -1;
    if (file->status <= 0) return file->status;
    /* A detached entry holds its bytes again for the borrower (until the next release point). */
    if (!file->body && !(file->body = sh_package_body_attach(file->owner))) return -1;
    *body = file->body; *length = file->length; *owner = PL_SHARE ? file->owner : NULL;
    return file->status;
}
/* A snapshot read as the reader that serves path would (pl_read_original over the snapshot, or another reader), with
 * the snapshot's bytes shared rather than copied: into resource's body, body_length and body_owner. */
static int pl_original_resource(sh_package_baseline_reader original, void *context, const char *path,
    sh_compiled_resource *resource)
{
    if (original != pl_read_original || !PL_SHARE)
        return original ? original(context, path, &resource->body, &resource->body_length) : 0;
    const pl_original *file = pl_original_find(context, path);
    if (!file) return -1;
    if (file->status <= 0) return file->status;
    if (file->body) {
        resource->body_owner = sh_package_body_retain(file->owner);
        resource->body = (unsigned char *)file->body;
    } else {
        /* A detached entry: the resource holds the bytes itself, read again when they were let go. */
        resource->body_owner = sh_package_body_retain_detached(file->owner);
        if (!(resource->body = (unsigned char *)sh_package_body_attach(file->owner))) {
            sh_package_body_drop(resource->body_owner, NULL); resource->body_owner = NULL; return -1;
        }
    }
    resource->body_length = file->length;
    return file->status;
}
static void pl_resource_body_free(sh_compiled_resource *resource)
{
    if (resource->body_owner) sh_package_body_drop(resource->body_owner, resource->body); else free(resource->body);
    resource->body_owner = NULL; resource->body = NULL; resource->body_length = 0;
}
/* A release point (R-342): every snapshot entry holding a backed body's bytes stops holding them, after the
 * capture's own originals did (sh_package_source_link_native.originals_released). A body whose every holder then
 * reads only through it lets its bytes go; readers read them again from its backing. */
static void pl_originals_detach(void *context)
{
    pl_originals *slots = context;
    /* Without shared owners the capture borrows the snapshot's bytes with no reference of its own. */
    if (!PL_SHARE) return;
    for (size_t k = 0; k < 2; k++)
        for (size_t i = 0; i < slots[k].count; i++) {
            pl_original *file = slots[k].files + i;
            if (!file->body || !file->owner || !sh_package_body_backed(file->owner)) continue;
            file->body = NULL; sh_package_body_detach(file->owner);
        }
}
/* The capture let one binary original go after its first recorded discovery (R-344 stage A;
 * sh_package_source_link_native.original_released): the snapshot stops holding its bytes too. */
static void pl_original_detach_path(void *context, const char *path)
{
    pl_originals *slots = context;
    if (!PL_SHARE) return;
    for (size_t k = 0; k < 2; k++) {
        pl_original *file = pl_original_at(slots + k, path);
        if (!file || !file->body || !file->owner || !sh_package_body_backed(file->owner)) continue;
        file->body = NULL; sh_package_body_detach(file->owner);
    }
}
/* 1 with the error when a captured original could not be read again exactly: an environment failure, whichever
 * reader asked and however it reported it, which blames no package. */
static int pl_originals_failed(const pl_originals *slots, char *error, size_t capacity)
{
    for (size_t k = 0; k < 2; k++)
        for (size_t i = 0; i < slots[k].count; i++) {
            const pl_original *file = slots[k].files + i;
            if (!file->owner || !sh_package_body_failed(file->owner)) continue;
            if (error && capacity)
                snprintf(error, capacity, "installed original %s could not be read again from its pinned archive",
                    file->path);
            return 1;
        }
    return 0;
}

typedef struct pl_build {
    /* Identity provenance bases per contribution unit (R-397): the packages of the compositions it took part in. */
    sh_package_owners *unit_prov;
    size_t unit_prov_count;
    sh_package_scope *views;
    sh_package_compilation **scopes;
    size_t count, authored_count;
    sh_package_source_link *reader;
    sh_package_scope shared_view;
    sh_package_compilation *shared;
    sh_package_link_output linked;
    const sh_package_library_native *native;
    sh_package_source_reader **material_readers;
    sh_package_virtual_materials *materials;
    sh_package_audio_program *audio;
    pl_originals originals[2];
    const sh_package_provenance *contexts;
    sh_package_native_registry *identities;
    pl_contributions *contributions;
    /* Packages whose own contribution failed (pl_contribution_blame), in the
     * order found: the first is reported as invalid_package or
     * unserved_package, the others as more_rejections. Batched only when the
     * caller accepts more_rejections (collect_contributions); otherwise the
     * first ends the compile. withdrawn[scope] marks every scope of a package
     * found so; unit_state[unit] records how each editor or registration
     * group member was judged (PL_UNIT_*, pl_group_judge). */
    sh_package_rejections contribution_rejections;
    unsigned char *withdrawn, *unit_state;
    size_t unit_state_count;
    int collect_contributions;
    /* 1 while the last failure of the stage is a judged one (a finding, a gap
     * blaming nobody, or the end of a stage that found packages); only then are
     * the findings reported. It is cleared whenever the stage goes on after a
     * finding, so an environmental or otherwise unjudged failure blames
     * nobody. */
    int contribution_verdict;
    /* Each package's registration keys: the bit it authored and the bit it
     * holds in this library (see pl_key_phase). */
    struct pl_key_binding { size_t scope; unsigned original; uint32_t current; } *keys;
    size_t key_count;
    /* Accounted registration notes (shared keys, shadowed modifiers). */
    char *key_notes;
    /* Group codes this compile's relocated values use (package_keys.h). */
    sh_package_key_groups *key_groups;
    /* The spawn-type enum (key_space_read) and whether the private key
     * contract serves this compile, as the key phase found them. */
    sh_package_key_space key_space;
    int key_space_read, private_keys;
    /* Wall-clock milliseconds of the contribution phase's steps (PL_STEP_*). */
    unsigned long long step_ms[9];
    /* Diagnostics: why an unowned stock unit a scope only references lost its
     * original name (document step reasons per unit, then the accounted line). */
    const char **stock_reasons;
    size_t stock_reason_count, private_stock;
    char private_stock_notes[3072];
    /* Diagnostics: the document step's work (pl_document_shared): units walked, shared declarations compared with
     * their stock original (bytes read), files whose edges came from the frozen sites, files bound again, and the
     * milliseconds in body reads, original reads and binds. */
    struct {
        size_t units, shared, sites, binds, bodies;
        unsigned long long original_bytes, read_ms, original_ms, bind_ms;
    } document;
    /* Global string keys admitted to the native reservation (pl_reserve_string_globals). */
    int string_globals_reserved;
    /* Diagnostic parse census (decl_tree.h) per library phase and step. */
    int census_owned;
    /* Diagnostics: key-site scans made and reused, units left unrewritten. */
    size_t key_scans, key_scan_hits, key_skipped;
    /* Diagnostics (R-348): editor contribution groups by phase (1-3) as pl_contribution_group judged them: calls,
     * members, those on the composing path (contributed) and their members, staged members, and the parses made in
     * all, on the composing path, and while binding and reading the members' parents first. */
    struct pl_editor_census {
        size_t groups, members, contributed, contributed_members, staged;
        unsigned long long parses, contributed_parses, first_parses, form_parses, rebase_parses, compose_parses;
    } editor_census[4];
    /* Unchanged-scope reuse (R-348): the record offered and the one this compile writes (it keeps only what a
     * completed compile recorded), and whether this compile used the offered one (a failure then compiles again
     * without it). Settled editor groups (pl_group_settled_key) by phase: keyed, settled from the offered record,
     * recorded, and judged without a key (a member a contribution rewrote, say). */
    const sh_package_source_reuse *reuse_in;
    sh_package_source_reuse *reuse_out;
    int *reuse_attempted;
    size_t settled_keyed[4], settled_hits[4], settled_recorded[4], settled_unkeyed[4];
    size_t settled_hit_members[4], settled_restaged[4];
    /* Identity templates (R-348): the offered record's, read and offered to the identity cache (1), absent (0) or
     * unreadable (-1), with their bytes and read time; the bytes this compile recorded and the time encoding them. */
    int templates_read;
    size_t templates_read_bytes, templates_recorded_bytes;
    unsigned long long templates_read_ms, templates_encode_ms;
    /* Diagnostics (R-348): the key phase's parses reading registration rows and relocating scopes, and the scopes
     * relocated. */
    unsigned long long key_row_parses, key_relocation_parses, key_relocation_ms;
    size_t key_relocated_scopes;
    sh_decl_tree_census census_mark, census_phase[SH_PACKAGE_LIBRARY_PHASES], census_step[9];
    /* Per scope: 0 unknown, 1 its own policy requests g_useImageBlackList 0, 2 not (R-309). */
    unsigned char *image_gate;
    /* Each image's availability answer in a gate-opening scope (pl_availability), once per compile: it depends on
     * the name, the installed bytes this compile serves and the native exclusion state, not on which gate-opening
     * scope asks (R-348). Open addressing by name; names owned. */
    struct pl_available { char *name; int excluded; } *available;
    size_t available_count, available_capacity;
    /* Diagnostics: availability questions asked, and answered from that table; the questions about images state of a
     * scope without the gate authored names (pl_availability_authored), those answered excluded, and those unknown. */
    size_t available_asked, available_reused, authored_asked, authored_excluded, authored_unknown;
    /* Shared stock (R-344 stage A): the scopes this compile does not let share (from the caller's earlier rounds,
     * pl_compile), and those it found needing their own copies (unshare, b.count + 2 bytes; pl_share_boundary). */
    const unsigned char *unshared;
    int share_off;
    unsigned char *unshare;
    size_t unshare_count;
} pl_build;
enum {
    PL_STEP_COVERAGE, PL_STEP_LOCALIZE, PL_STEP_TOKENS, PL_STEP_KEYS, PL_STEP_EDITOR,
    PL_STEP_REGISTRATION, PL_STEP_HANDS, PL_STEP_SHARED, PL_STEP_DOCUMENT, PL_STEPS
};

static void pl_native_close(pl_build *b)
{
    sh_package_audio_program_close(b->audio); b->audio = NULL;
    sh_package_virtual_material_close(b->materials); b->materials = NULL;
    if (b->material_readers) for (size_t i = 0; i < b->count; i++)
        sh_package_source_reader_close(b->material_readers[i]);
    free(b->material_readers); b->material_readers = NULL;
}

static int pl_native_open(pl_build *b, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, char *error, size_t capacity)
{
    if (b->native && b->native->audio) {
        b->audio = sh_package_audio_program_open((const sh_package_compilation *const *)b->scopes,
            b->count, b->native->audio, error, capacity);
        if (!b->audio) return 0;
    }
    if (!b->native || !b->native->atlas) return 1;
    b->materials = sh_package_virtual_material_open((const sh_package_compilation *const *)b->scopes,
        b->count, original, original_context);
    b->material_readers = calloc(b->count, sizeof(*b->material_readers));
    if (!b->materials || !b->material_readers) return 0;
    for (size_t i = 0; i < b->count; i++) {
        b->material_readers[i] = sh_package_source_reader_open_shared(i ? b->material_readers[0] : NULL,
            b->scopes[i], catalog, original, original_context, registry, reflection, error, capacity);
        if (!b->material_readers[i]) return 0;
    }
    return 1;
}

static int pl_admit(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    pl_build *b = context;
    return b->native && b->native->source.admit ?
        b->native->source.admit(b->native->source.context, scope, family, name, semantics, error, capacity) : 0;
}

/* Whether this scope's own compiled policy requests the image blacklist gate open. Requirements
 * are declared by the packages a scope compiles, so only their consumers are affected (R-309). */
static int pl_scope_opens_image_gate(pl_build *b, size_t scope)
{
    if (!b->scopes || scope >= b->count) return 0;
    if (!b->image_gate && !(b->image_gate = calloc(b->count, 1))) return -1;
    if (!b->image_gate[scope]) {
        const sh_package_compilation *compiled = b->scopes[scope];
        const char *raw = compiled ? sh_json_object_get(&compiled->policy.requirements, "cvars") : NULL;
        sh_json_object cvars = {0}; int opens = 0;
        if (raw && sh_json_parse_object(raw, strlen(raw), 2, &cvars)) {
            const char *value = sh_json_object_get(&cvars, "g_useImageBlackList");
            opens = value && !strcmp(value, "0");
        }
        sh_json_object_free(&cvars);
        b->image_gate[scope] = opens ? 1 : 2;
    }
    return b->image_gate[scope] == 1;
}

/* The availability table's slot for name (its entry or the empty slot it belongs in), SIZE_MAX without a table. */
static size_t pl_available_at(const pl_build *b, const char *name)
{
    if (!b->available_capacity) return SIZE_MAX;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) hash = (hash ^ *p) * UINT64_C(1099511628211);
    size_t mask = b->available_capacity - 1, at = (size_t)(hash ^ (hash >> 31)) & mask;
    while (b->available[at].name && strcmp(b->available[at].name, name)) at = (at + 1) & mask;
    return at;
}
static void pl_available_keep(pl_build *b, const char *name, int excluded)
{
    if (b->available_count + 1 > b->available_capacity / 2) {
        size_t capacity = b->available_capacity ? b->available_capacity * 2 : 1024, previous = b->available_capacity;
        struct pl_available *old = b->available, *grown = capacity <= SIZE_MAX / 2 / sizeof(*grown) ?
            calloc(capacity, sizeof(*grown)) : NULL;
        if (!grown) return;
        b->available = grown; b->available_capacity = capacity;
        for (size_t i = 0; i < previous; i++) if (old[i].name) b->available[pl_available_at(b, old[i].name)] = old[i];
        free(old);
    }
    size_t at = pl_available_at(b, name);
    if (b->available[at].name || !(b->available[at].name = _strdup(name))) return;
    b->available[at].excluded = excluded; b->available_count++;
}
/* name's native exclusion answer (image_exclusion.h): 1 placeholder, 0 not, -1 unknown. It depends on the name, the
 * installed bytes this compile serves and the native state, not on which scope asks, so it is kept once per compile
 * (an unknown answer is asked again). */
static int pl_image_excluded(pl_build *b, const char *name)
{
    b->available_asked++;
    size_t at = pl_available_at(b, name);
    if (at != SIZE_MAX && b->available[at].name) { b->available_reused++; return b->available[at].excluded; }
    char *path = sh_package_cooked_path("image", name);
    unsigned char *installed = NULL; size_t length = 0;
    int status = path ? pl_read_original(&b->originals[1], path, &installed, &length) : -1;
    free(path);
    int excluded = status < 0 ? -1 : b->native->image_excluded(b->native->image_excluded_context,
        name, status > 0 ? installed : NULL, status > 0 ? length : 0);
    free(installed);
    if (excluded >= 0) pl_available_keep(b, name, excluded); /* optional: asked again when refused */
    return excluded;
}
/* What a private stock copy of an exclusion placeholder is certified with: one identity for every scope that isolates
 * the name, so equal copies are one served image. */
static int pl_placeholder_semantics(unsigned char semantics[32])
{
    static const char identity[] = "doom2016:image-availability:native-exclusion-placeholder";
    sh_package_file_identity digest;
    if (!sh_package_bytes_identity(identity, sizeof(identity) - 1, &digest)) return 0;
    memcpy(semantics, digest.digest, 32);
    return 1;
}
/* The source link's availability callback (package_source_link.h). Only a scope whose own compiled
 * policy opens the image gate asks; the installed binary this compile serves for the name decides
 * whether a binary read could have produced the native state (image_exclusion.h). 1 reports an
 * exclusion placeholder with the semantics its private copy is certified with; the link decides
 * whether that name is recovered in place instead (a fixed native ABI binding reaches it). */
static int pl_availability(void *context, size_t scope, const char *name, unsigned char semantics[32],
    char *error, size_t capacity)
{
    pl_build *b = context;
    int opens = pl_scope_opens_image_gate(b, scope);
    if (opens < 0) return -1;
    if (!opens || !b->native || !b->native->image_excluded) return 0;
    int excluded = pl_image_excluded(b, name);
    if (excluded < 0) {
        if (error && capacity && !*error)
            snprintf(error, capacity, "native image availability could not be read: %s", name);
        return -1;
    }
    if (!excluded) return 0;
    /* The installed bytes are exact, but this name's registered native image is a placeholder
     * the image blacklist built before this scope's declared requirement opened the gate, and it
     * is never reloaded natively. A private stock copy loads under a name no blacklist entry
     * matches; the scope's consumers follow it. Stock consumers and usable images are untouched. */
    return pl_placeholder_semantics(semantics) ? 1 : -1;
}
/* The source link's availability_authored callback (package_source_link.h, R-389): an image that state of a scope
 * without the gate authored names. The engine's image blacklist (VK 0x14031cf20 / GL 0x14031c7d0; always
 * fonts/korataki_rg/ and four other font directories, in SnapMap 46 more names) makes the registered image of such a
 * name an 8 x 8 placeholder without reading its binary, so what the package's own text binds draws nothing: a font
 * caches the placeholder's size for its glyph coordinates. A private stock copy loads under a name no blacklist entry
 * matches, for this scope only; the gate, the stock image and every other scope are untouched. An unknown answer keeps
 * the stock binding, exactly what such a scope bound before this question; it is counted, never served as usable. */
static int pl_availability_authored(void *context, size_t scope, const char *name, unsigned char semantics[32],
    char *error, size_t capacity)
{
    pl_build *b = context;
    (void)error; (void)capacity;
    if (pl_scope_opens_image_gate(b, scope) != 0 || !b->native || !b->native->image_excluded) return 0;
    b->authored_asked++;
    int excluded = pl_image_excluded(b, name);
    if (excluded < 0) { b->authored_unknown++; return 0; }
    if (!excluded || !pl_placeholder_semantics(semantics)) return 0;
    b->authored_excluded++;
    return 1;
}
static int pl_semantics(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    pl_build *b = context;
    if (!strcmp(family, "sound") && b->audio) {
        sh_audio_graph_root root;
        unsigned char combined[64] = {0}; sh_package_file_identity identity;
        int original = b->native->source.semantics ?
            b->native->source.semantics(b->native->source.context, scope, family, name,
                combined, error, capacity) : 0;
        if (original >= 0 && original <= 1 && !sh_package_audio_program_contains(b->audio, scope, name)) {
            memcpy(semantics, combined, 32); return original; /* Native event binding. */
        }
        if (original < 0 || original > 1 || !sh_package_audio_program_prepare(b->audio, error, capacity) ||
            !sh_package_audio_program_root(b->audio, scope, name, &root, error, capacity)) return -1;
        /* An exact native audio graph keeps the plain native certificate, the
         * same one an uncollected SnapMap sound carries in another scope. */
        if (original && root.original) { memcpy(semantics, combined, 32); return 1; }
        memcpy(combined + 32, root.identity, 32);
        if (!sh_package_bytes_identity(combined, sizeof(combined), &identity)) return -1;
        memcpy(semantics, identity.digest, 32); return 0;
    }
    if (!strcmp(family, "material") && b->materials) {
        sh_package_virtual_atlas atlas = {0}; int original = 0;
        if (sh_package_virtual_material_cached(b->materials, scope, name, semantics, &original)) return original;
        char *page_file = sh_package_source_material_path(b->material_readers[scope], name,
            "landPageFile", error, capacity);
        if (!page_file) return -1;
        int status = b->native->atlas(b->native->source.context, scope, b->scopes[scope], name,
            page_file, &atlas, error, capacity);
        if (status != 1) {
            if (!*error) snprintf(error, capacity, "material page provider is unavailable: %s", name);
            free(page_file);
            sh_virtual_page_source_release(atlas.pages);
            return -1;
        }
        status = sh_package_virtual_material_bind(b->materials, scope, name, &atlas,
            semantics, &original, error, capacity);
        sh_virtual_page_source_release(atlas.pages);
        free(page_file);
        if (status < 0) return -1;
        return original;
    }
    return b->native && b->native->source.semantics ?
        b->native->source.semantics(b->native->source.context, scope, family, name, semantics, error, capacity) : 0;
}

static int pl_material_companions(void *context, size_t scope, const char *material,
    int (*visit)(void *, const char *, const sh_compiled_resource *), void *visit_context,
    char *error, size_t capacity)
{
    pl_build *b = context; unsigned char semantics[32];
    return !b->materials || (pl_semantics(context, scope, "material", material, semantics, error, capacity) >= 0 &&
        sh_package_virtual_material_witnesses(b->materials, scope, material, visit, visit_context, error, capacity));
}
static int pl_material_seeds(void *context, size_t scope,
    int (*visit)(void *, const char *), void *visit_context, char *error, size_t capacity)
{
    pl_build *b = context;
    return !b->materials || sh_package_virtual_material_seeds(b->materials, scope,
        visit, visit_context, error, capacity);
}

static int pl_shared_declaration(const char *type)
{
    /* Entity definitions qualify only after the dependency walk proves no
     * gameplay contribution. This retains product defaults and editorVars-only
     * extensions without aliasing their vanilla gameplay identity. */
    return type && (!strcmp(type, "entitydef") || !strncmp(type, "snapeditor", 10) ||
        !strncmp(type, "snappropertyinspector", 21));
}
static int pl_implicit(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visit_context,
    char *error, size_t capacity)
{
    pl_build *b = context;
    if (b->audio && !observing && unit->file_count && !strcmp(unit->family, "sound")) {
        /* Only a declaration a scope changed, or a SnapMap sound its banks
         * affect, needs an audio program root. Other SnapMap sounds keep their
         * native event binding, including events no bank implements (the
         * stock "default" sound posts play_default, which nothing plays). */
        int member = sh_package_audio_program_affected(b->audio, scope, unit->name);
        for (size_t f = 0; !member && f < unit->file_count; f++)
            member = unit->files[f]->source_count && !unit->files[f]->native_original;
        if (member && !sh_package_audio_program_collect(b->audio, scope, unit->name, error, capacity)) return 0;
    }
    return !b->native || !b->native->source.implicit ||
        b->native->source.implicit(b->native->source.context, scope, observing, unit,
            visit, visit_context, error, capacity) == 1;
}

static void pl_close(pl_build *b)
{
    for (size_t i = 0; b->unit_prov && i < b->unit_prov_count; i++) sh_package_owners_free(b->unit_prov + i);
    free(b->unit_prov); b->unit_prov = NULL; b->unit_prov_count = 0;
    free(b->keys); b->keys = NULL; b->key_count = 0;
    free(b->key_notes); b->key_notes = NULL;
    free(b->key_groups); b->key_groups = NULL;
    free((void *)b->stock_reasons); b->stock_reasons = NULL; b->stock_reason_count = 0;
    sh_package_rejections_free(&b->contribution_rejections);
    free(b->withdrawn); b->withdrawn = NULL; free(b->unit_state); b->unit_state = NULL; b->unit_state_count = 0;
    free(b->image_gate); b->image_gate = NULL;
    for (size_t i = 0; i < b->available_capacity; i++) free(b->available[i].name);
    free(b->available); b->available = NULL; b->available_count = b->available_capacity = 0;
    sh_package_native_registry_close(b->identities);
    pl_contributions_close(b->contributions);
    sh_package_source_link_close(b->reader);
    pl_native_close(b);
    for (size_t i = 0; i < b->count; i++) {
        sh_package_compilation_free(b->scopes ? b->scopes[i] : NULL);
        if (b->views) sh_package_scope_close(b->views + i);
    }
    free(b->scopes); free(b->views);
    sh_package_compilation_free(b->shared); sh_package_scope_close(&b->shared_view);
    sh_package_link_output_free(&b->linked);
    /* Bodies a kept output still serves stay with it; the rest go now. */
    for (size_t k = 0; k < 2; k++) {
        pl_originals *s = b->originals + k;
        for (size_t i = 0; i < s->count; i++) { free(s->files[i].path); sh_package_body_drop(s->files[i].owner, s->files[i].body); }
        free(s->files);
    }
}

static int pl_source_owners(void *context, size_t scope, const char *family, const char *name,
    const sh_package_owners **owners, const sh_package_owners **gameplay, char *error, size_t capacity)
{
    pl_build *b = context;
    *owners = *gameplay = NULL;
    if (!b->audio || strcmp(family, "sound"))
        return !b->native || !b->native->source.owners ||
            b->native->source.owners(b->native->source.context, scope, family, name,
                owners, gameplay, error, capacity) == 1;
    if (!sh_package_audio_program_contains(b->audio, scope, name)) return 1; /* Native binding. */
    if (sh_package_audio_program_owners(b->audio, scope, name, owners, gameplay)) return 1;
    snprintf(error, capacity, "sound has no captured dependency ownership: %s", name); return 0;
}

static unsigned long long pl_now_ms(void)
{
    struct timespec now;
    return timespec_get(&now, TIME_UTC) ? (unsigned long long)now.tv_sec * 1000u + (unsigned long long)now.tv_nsec / 1000000u : 0;
}
/* Charge the parses since the last mark to into and restart the mark. */
static void pl_census(pl_build *b, sh_decl_tree_census *into)
{
    sh_decl_tree_census now;
    sh_decl_tree_census_read(&now);
    into->parses += now.parses - b->census_mark.parses; into->bytes += now.bytes - b->census_mark.bytes;
    b->census_mark = now;
}
/* Charge the time since *mark to phase and restart the mark. */
static void pl_phase(pl_build *b, sh_package_library *out, int phase, unsigned long long *mark)
{
    unsigned long long now = pl_now_ms();
    out->phase_ms[phase] += now >= *mark ? now - *mark : 0; *mark = now;
    pl_census(b, b->census_phase + phase);
    sh_package_memory_read(out->phase_memory_mb[phase]);
}
/* The same for one step of the contribution phase. */
static void pl_step(pl_build *b, int step, unsigned long long *mark)
{
    unsigned long long now = pl_now_ms();
    b->step_ms[step] += now >= *mark ? now - *mark : 0; *mark = now;
    pl_census(b, b->census_step + step);
}
static void pl_steps(const pl_build *b, char *out, size_t capacity)
{
    static const char *const names[PL_STEPS] = {
        "coverage", "localize", "tokens", "keys", "editor", "registration", "hands", "shared", "document"
    };
    size_t used = 0;
    *out = 0;
    for (int i = 0; i < PL_STEPS && used < capacity; i++) {
        int n = snprintf(out + used, capacity - used, "%s%s %llu ms", i ? ", " : "", names[i], b->step_ms[i]);
        if (n < 0) break;
        used += (size_t)n;
    }
}
/* "phase private/working MB" at each phase end, then the process peaks and the source capture's own phases;
 * empty without a meter. */
const char *sh_package_library_memory(const sh_package_library *library, char *out, size_t capacity)
{
    static const char *const names[SH_PACKAGE_LIBRARY_PHASES] = {
        "scopes", "native", "source link", "contributions", "identities", "link", "outputs", "audio"
    };
    size_t used = 0;
    unsigned long long peak_private = 0, peak_working = 0;
    if (!out || !capacity) return out;
    *out = 0;
    for (int i = 0; library && i < SH_PACKAGE_LIBRARY_PHASES && used < capacity; i++) {
        const unsigned long long *mb = library->phase_memory_mb[i];
        if (!mb[0] && !mb[1]) continue;
        int n = snprintf(out + used, capacity - used, "%s%s %llu/%llu", used ? ", " : "", names[i], mb[0], mb[1]);
        if (n < 0) break;
        used += (size_t)n;
        if (mb[2] > peak_private) peak_private = mb[2];
        if (mb[3] > peak_working) peak_working = mb[3];
    }
    if (used && used < capacity) {
        int n = snprintf(out + used, capacity - used, "; process peaks %llu/%llu%s%s%s", peak_private, peak_working,
            library->source_memory[0] ? " (source link: " : "", library->source_memory,
            library->source_memory[0] ? ")" : "");
        if (n > 0) used += (size_t)n;
    }
    return out;
}
const char *sh_package_library_phases(const sh_package_library *library, char *out, size_t capacity)
{
    static const char *const names[SH_PACKAGE_LIBRARY_PHASES] = {
        "scopes", "native", "source link", "contributions", "identities", "link", "outputs", "audio"
    };
    size_t used = 0;
    if (!out || !capacity) return out;
    *out = 0;
    for (int i = 0; library && i < SH_PACKAGE_LIBRARY_PHASES && used < capacity; i++) {
        int n = snprintf(out + used, capacity - used, "%s%s %llu ms", i ? ", " : "", names[i], library->phase_ms[i]);
        if (n < 0) break;
        used += (size_t)n;
    }
    return out;
}

void sh_package_library_free(sh_package_library *library)
{
    if (!library) return;
    sh_package_audio_program_close(library->audio);
    sh_package_compilation_free(library->compiled);
    sh_virtual_material_runtime_retire(&library->virtual_materials);
    sh_package_consumers_free(library->consumers);
    sh_package_fsm_webs_free(library->fsm_webs);
    free(library->key_groups);
    for (size_t i = 0; library->row_provenance && i < library->row_provenance_count; i++) {
        sh_package_owners_free(library->row_provenance + i); free(library->row_provenance_paths[i]);
    }
    free(library->row_provenance); free(library->row_provenance_paths);
    free(library->coverage); free(library->link_diagnostics);
    for (size_t i = 0; library->shader_inputs && i < library->shader_input_count; i++)
        free(library->shader_inputs[i].pair_path);
    free(library->shader_inputs);
    for (size_t i = 0; library->composed_inputs && i < library->composed_input_count; i++)
        free(library->composed_inputs[i].peers);
    free(library->composed_inputs);
    for (size_t i = 0; library->recovered_images && i < library->recovered_image_count; i++)
        free(library->recovered_images[i]);
    free(library->recovered_images);
    sh_package_provenance_free(&library->bindings); memset(library, 0, sizeof(*library));
}

/* The authored file a scope resource reads, or SIZE_MAX for product or
 * installed-original data. */
static size_t pl_input_file(const sh_package_sources *sources, const sh_compiled_resource *file)
{
    size_t source = file->source != SIZE_MAX ? file->source : file->source_count ? file->sources[0] : SIZE_MAX;
    return sources && source < sources->file_count ? source : SIZE_MAX;
}
/* Keep the link's unpublished shader inputs by source file. Diagnostics only:
 * a record that cannot be kept marks the set lost. */
static void pl_shader_inputs(const pl_build *b, const sh_package_sources *sources, sh_package_library *out)
{
    const sh_package_shader_unpublished *records = NULL;
    int lost = 0;
    size_t count = sh_package_source_link_shader_unpublished(b->reader, &records, &lost);
    out->shader_inputs_lost = lost;
    if (!count) return;
    if (!(out->shader_inputs = calloc(count, sizeof(*out->shader_inputs)))) { out->shader_inputs_lost = 1; return; }
    for (size_t i = 0; i < count; i++) {
        size_t file = pl_input_file(sources, records[i].file);
        size_t pair = records[i].pair ? pl_input_file(sources, records[i].pair) : SIZE_MAX;
        if (file == SIZE_MAX) continue; /* nothing authored to account */
        sh_package_unpublished_shader *row = out->shader_inputs + out->shader_input_count;
        *row = (sh_package_unpublished_shader){file, pair, NULL, (int)records[i].reason, records[i].renderer,
            records[i].checksum};
        if (records[i].pair && pair == SIZE_MAX && !(row->pair_path = _strdup(records[i].pair->engine_path))) {
            out->shader_inputs_lost = 1; continue;
        }
        out->shader_input_count++;
    }
}

/* Describe accounted reverse-discovery gaps with their supplying delivery. The
 * stock summary is one line; each affected scope names its package. */
static int pl_coverage(const pl_build *b, const sh_package_sources *sources, char **out)
{
    const sh_package_reverse_gap *gaps = NULL;
    size_t count = sh_package_source_link_reverse_gaps(b->reader, &gaps), stock = 0, capacity = 0;
    const sh_package_reverse_gap *first = NULL;
    char *text = NULL;
    *out = NULL;
    for (size_t i = 0; i < count; i++) if (gaps[i].scope == SIZE_MAX) { stock++; if (!first) first = gaps + i; }
    /* Measure, then write the identical text. */
    for (size_t pass = 0; count && pass < 2; pass++) {
        size_t used = 0;
        if (first) {
            int n = snprintf(text ? text + used : NULL, text ? capacity - used : 0,
                "Toybox variant discovery skipped %zu stock SnapMap resource%s whose format is not yet supported; "
                "package edits reachable only through them add no variant (first: %s %s: %s).\n",
                stock, stock == 1 ? "" : "s", first->family, first->name, first->reason);
            if (n < 0) return 0;
            used += (size_t)n;
        }
        for (size_t i = 0; i < count; i++) if (gaps[i].scope != SIZE_MAX) {
            const char *label = "map source", *id = "";
            if (!b->contexts && gaps[i].scope < sources->component_count) {
                const sh_package_component *component = sources->components + gaps[i].scope;
                label = component->descriptor.name[0] ? component->descriptor.name :
                    sources->packages[component->owner].name;
                id = component->descriptor.id;
            }
            /* A module card is reached by its module's name; a card no
             * installed module has keeps its stock presentation. */
            int n = gaps[i].variant ? snprintf(text ? text + used : NULL, text ? capacity - used : 0,
                "%s%s%s%s: stock %s %s keeps its own presentation: %s.\n",
                label, *id ? " [" : "", id, *id ? "]" : "", gaps[i].family, gaps[i].name, gaps[i].reason) :
                snprintf(text ? text + used : NULL, text ? capacity - used : 0,
                "%s%s%s%s: no toybox variant for stock %s %s because its format is not yet supported (%s).\n",
                label, *id ? " [" : "", id, *id ? "]" : "", gaps[i].family, gaps[i].name, gaps[i].reason);
            if (n < 0) return 0;
            used += (size_t)n;
        }
        if (!pass) {
            if (!used) break;
            if (used == SIZE_MAX || !(text = malloc(used + 1))) return 0;
            capacity = used + 1; *text = 0;
        }
    }
    *out = text; return 1;
}

/* An authored event edit SnapMap never runs is reported, not repaired: the
 * native registration order keeps the earlier bank's copy (R039), exactly as
 * replacing that bank file in the game would. */
typedef struct pl_shadow_notes {
    const pl_build *build;
    const sh_package_sources *sources;
    char *text;
} pl_shadow_notes;
static int pl_shadow_note(void *context, size_t scope, uint32_t event, const char *sound,
    const char *bank, const char *first)
{
    static const char format[] = "%s%s%s%s: audio event %u%s%s%s edited in %s never plays in SnapMap: "
        "%s registers that event first, so this edit has no effect.\n";
    pl_shadow_notes *notes = context;
    const char *label = "map source", *id = "", *leaf = bank ? strrchr(bank, '/') : NULL, *earlier = first ? strrchr(first, '/') : NULL;
    if (!notes->build->contexts && scope < notes->sources->component_count) {
        const sh_package_component *component = notes->sources->components + scope;
        label = component->descriptor.name[0] ? component->descriptor.name : notes->sources->packages[component->owner].name;
        id = component->descriptor.id;
    }
    leaf = leaf ? leaf + 1 : bank ? bank : "?"; earlier = earlier ? earlier + 1 : first ? first : "?";
    int n = snprintf(NULL, 0, format, label, *id ? " [" : "", id, *id ? "]" : "", event,
        sound ? " (" : "", sound ? sound : "", sound ? ")" : "", leaf, earlier);
    size_t used = notes->text ? strlen(notes->text) : 0;
    if (n < 0 || (size_t)n > SIZE_MAX - used - 1) return 0;
    char *grown = realloc(notes->text, used + (size_t)n + 1); if (!grown) return 0;
    notes->text = grown;
    snprintf(grown + used, (size_t)n + 1, format, label, *id ? " [" : "", id, *id ? "]" : "", event,
        sound ? " (" : "", sound ? sound : "", sound ? ")" : "", leaf, earlier);
    return 1;
}

/* Policy and product defaults have their own view. Peer declaration bytes are
 * never passed through the unbound whole-file composer. */
static int pl_shared_sources(pl_build *b, const sh_package_sources *sources,
    char *error, size_t capacity)
{
    size_t *owners = NULL;
    if (sources->package_count > SIZE_MAX / sizeof(*owners) ||
        !(owners = calloc(sources->package_count ? sources->package_count : 1, sizeof(*owners)))) return 0;
    for (size_t i = 0; i < sources->package_count; i++) owners[i] = i;
    int ok = sh_package_scope_composed(sources, owners, sources->package_count,
        &b->shared_view, error, capacity);
    free(owners); if (!ok) return 0;
    for (size_t i = 0; i < sources->file_count; i++) b->shared_view.sources.files[i].engine_path = NULL;
    return 1;
}

/* 1 when an authored entity declaration states registration lists (a
 * conductor's aiTypeList or globalAIMods), 0 when not, -1 when unreadable. A
 * cheap byte check precedes parsing: few declarations register AI. */
static int pl_registration_file(const sh_package_compilation *compiled, const sh_compiled_resource *file,
    char *error, size_t capacity)
{
    size_t length = 0;
    int stated = 0;
    if (!file->type || strcmp(file->type, "entitydef") || !file->source_count) return 0;
    unsigned char *body = sh_package_compilation_read(compiled, file, SIZE_MAX, &length, error, capacity);
    if (!body) return -1;
    for (size_t at = 0; !stated && at + 10 <= length; at++)
        stated = !memcmp(body + at, "aiTypeList", 10) || (at + 12 <= length && !memcmp(body + at, "globalAIMods", 12));
    free(body); return stated;
}

static int pl_builtin_has_authored_editor(const sh_package_sources *scope, const char *path)
{
    char family[256];
    if (strncmp(path, "generated/decls/", 16)) return 0;
    const char *end = strchr(path + 16, '/');
    if (!end || end - path - 16 >= sizeof(family)) return 0;
    memcpy(family, path + 16, (size_t)(end - path - 16)); family[end - path - 16] = 0;
    if (!sh_package_editor_contribution_phase(family)) return 0;
    for (size_t i = 0; i < scope->file_count; i++)
        if (scope->files[i].engine_path && !strcmp(scope->files[i].engine_path, path)) return 1;
    return 0;
}

static int pl_product_scope(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    size_t count = b->shared->resource_count;
    if (!count) return 1;
    sh_package_resource_projection *projections = calloc(count, sizeof(*projections));
    const sh_compiled_resource **members = calloc(count, sizeof(*members));
    int ok = 0;
    if (!projections || !members) goto done;
    for (size_t i = 0; i < count; i++) {
        members[i] = b->shared->resources + i;
        projections[i] = (sh_package_resource_projection){members[i]->engine_path, members + i, 1};
    }
    b->scopes[b->count] = sh_package_compilation_project(sources, projections, count, NULL,
        b->shared->canonical_path, error, capacity);
    if (!b->scopes[b->count]) goto done;
    if (PL_SHARE) sh_package_compilation_share(b->scopes[b->count]);
    b->count++; ok = 1;
done:
    free(projections); free(members); return ok;
}

static int pl_inherit_name(sh_decl_source text, char **out);
#define PL_EDITOR_PREFIX "generated/decls/snapeditorentitydef/"
static const sh_editor_row *pl_editor_row(const sh_editor_rows *rows, const char *name)
{
    for (size_t low = 0, high = rows->count; low < high;) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(rows->items[middle].name, name);
        if (!order) return rows->items + middle;
        if (order < 0) low = middle + 1; else high = middle;
    }
    return NULL;
}
/* 1 when some package's own text for a stock editor entry leaves an inherited
 * control at another slot than the shipped entry does (a control inserted,
 * removed or moved, or another parent), 0 when none does, -1 on failure. */
static int pl_moves_controls(pl_build *b, const char *path, const unsigned char *stock, size_t stock_length,
    sh_package_baseline_reader original, void *context, char *error, size_t capacity)
{
    sh_decl_source parents[32], shipped = {(const char *)stock, stock_length};
    size_t parent_count = 0;
    char *parent = NULL, *authored_parent = NULL;
    int moves = 0;
    if (!pl_inherit_name(shipped, &parent)) return 0;
    while (parent && parent_count < 32) {
        size_t length = strlen(parent) + sizeof(PL_EDITOR_PREFIX) + 5;
        char *parent_path = malloc(length);
        unsigned char *body = NULL; size_t size = 0;
        int status = -1;
        if (parent_path) {
            snprintf(parent_path, length, PL_EDITOR_PREFIX "%s.decl", parent);
            status = original(context, parent_path, &body, &size);
        }
        free(parent_path); free(parent); parent = NULL;
        if (status < 0) { free(body); moves = -1; goto done; }
        if (status != 1 && status != 3) { free(body); break; }
        parents[parent_count++] = (sh_decl_source){(const char *)body, size};
        if (!pl_inherit_name(parents[parent_count - 1], &parent)) break;
    }
    free(parent); parent = NULL;
    if (!pl_inherit_name(shipped, &parent)) goto done;
    for (size_t scope = 0; scope < b->count && !moves; scope++) {
        const sh_compiled_resource *file = sh_package_compilation_find(b->scopes[scope], path);
        size_t length = 0;
        /* A frozen legacy context composes only its AI registrations. */
        if (b->contexts && scope < b->authored_count && b->contexts->contexts[scope].mode == SH_PACKAGE_CONTEXT_COMPOSED)
            continue;
        if (!file || !file->source_count || file->native_original) continue;
        unsigned char *body = sh_package_compilation_read(b->scopes[scope], file, SIZE_MAX, &length, error, capacity);
        if (!body) { moves = -1; break; }
        sh_decl_source text = {(const char *)body, length};
        if (!pl_inherit_name(text, &authored_parent)) moves = 0;
        else if ((parent == NULL) != (authored_parent == NULL) || (parent && strcmp(parent, authored_parent))) moves = 1;
        else moves = sh_package_editor_moves_controls("snapeditorentitydef", text, shipped, parents, parent_count);
        /* A malformed collection decides nothing here: the phase that reads it
         * as its owner's content leaves that package out, and a healthy peer's
         * relay is still found. */
        if (moves == -2) moves = 0;
        free(authored_parent); authored_parent = NULL; free(body);
    }
done:
    for (size_t i = 0; i < parent_count; i++) free((char *)parents[i].text);
    free(parent); free(authored_parent);
    if (moves < 0 && !*error) snprintf(error, capacity, "cannot compare an editor entry's layout: %s", path);
    return moves;
}

/* The names of the stock editor entries whose inherited controls some
 * package's own text moves (pl_moves_controls), each once. 0 on failure. */
static int pl_editor_relays(pl_build *b, sh_package_baseline_reader original, void *context,
    char ***out, size_t *count, char *error, size_t capacity)
{
    size_t prefix = sizeof(PL_EDITOR_PREFIX) - 1;
    *out = NULL; *count = 0;
    if (!original) return 1;
    for (size_t scope = 0; scope < b->count; scope++) {
        if (b->contexts && scope < b->authored_count && b->contexts->contexts[scope].mode == SH_PACKAGE_CONTEXT_COMPOSED)
            continue;
        for (size_t i = 0; i < b->scopes[scope]->resource_count; i++) {
            const sh_compiled_resource *file = b->scopes[scope]->resources + i;
            const char *path = file->engine_path;
            size_t n = path ? strlen(path) : 0, seen = 0;
            if (!file->source_count || file->native_original || !file->type || strcmp(file->type, "snapeditorentitydef") ||
                n < prefix + 6 || strncmp(path, PL_EDITOR_PREFIX, prefix) || strcmp(path + n - 5, ".decl")) continue;
            for (size_t r = 0; r < *count && !seen; r++)
                seen = strlen((*out)[r]) == n - prefix - 5 && !strncmp((*out)[r], path + prefix, n - prefix - 5);
            if (seen) continue;
            unsigned char *stock = NULL; size_t length = 0;
            int status = original(context, path, &stock, &length);
            if (status < 0) { free(stock); return 0; }
            int moves = status == 1 || status == 3 ?
                pl_moves_controls(b, path, stock, length, original, context, error, capacity) : 0;
            free(stock);
            if (moves < 0) return 0;
            if (!moves) continue;
            char **grown = realloc(*out, (*count + 1) * sizeof(*grown));
            char *name = grown ? malloc(n - prefix - 4) : NULL;
            if (grown) *out = grown;
            if (!name) { snprintf(error, capacity, "editor layout allocation failed"); return 0; }
            memcpy(name, path + prefix, n - prefix - 5); name[n - prefix - 5] = 0;
            (*out)[(*count)++] = name;
        }
    }
    return 1;
}

/* Baseline references must bind in an original-only context. Resolving a stock
 * inspector through a package which replaces it would misclassify additions as
 * unchanged. Product defaults have their own contribution scope. Seed only known
 * original editor identities; source_link
 * captures their closure from the verified original reader, never another pack.
 * A stock editor entry whose inherited controls a package moves also seeds its
 * stock descendants: their shipped patches address its slots, so each needs a
 * rebase onto the published entry in every library, whether a package reaches
 * it or not (an installation cannot restate a shipped entry later). */
static int pl_baseline_scope(pl_build *b, const sh_package_sources *sources,
    sh_package_baseline_reader original, void *context, char *error, size_t capacity)
{
    sh_package_resource_projection *projections = NULL;
    sh_compiled_resource *seeds = NULL;
    const sh_compiled_resource **members = NULL;
    size_t maximum = b->shared->resource_count, count = 0, hand_count = 0, owned_count = 0, relay_count = 0;
    char **hands = NULL, **owned = NULL, **relays = NULL;
    const sh_editor_rows *rows = NULL;
    int ok = 0;
    for (size_t i = 0; i < b->count; i++) {
        if (b->scopes[i]->resource_count > SIZE_MAX - maximum) return 0;
        maximum += b->scopes[i]->resource_count;
    }
    /* The stock editor entries some package relays, and then the shipped roster. */
    if (!pl_editor_relays(b, original, context, &relays, &relay_count, error, capacity)) goto done;
    if (relay_count && b->native && b->native->editor_rows) {
        /* The stock descendants are required, never partial: a roster that is
         * unavailable or missing an entry fails this compile, blaming nobody. */
        int read = b->native->editor_rows(b->native->editor_rows_context, &rows);
        if (read <= 0 || !rows) {
            snprintf(error, capacity, "SnapMap's shipped editor entries %s, and a package moves the inherited "
                "controls of %s", read < 0 ? "could not all be read" : "are unavailable", relays[0]);
            rows = NULL; goto done;
        }
    }
    if (rows && rows->count > SIZE_MAX - maximum) goto done;
    if (rows) maximum += rows->count;
    if (maximum > SIZE_MAX / sizeof(*seeds) || maximum > SIZE_MAX / sizeof(*projections)) return 0;
    projections = calloc(maximum ? maximum : 1, sizeof(*projections));
    members = calloc(maximum ? maximum : 1, sizeof(*members));
    seeds = calloc(maximum ? maximum : 1, sizeof(*seeds));
    if (!projections || !members || !seeds) goto done;
    if (rows && !(owned = calloc(rows->count + 1, sizeof(*owned)))) goto done;
    /* The player hands web composes against its stock original whenever a
     * package supplies it, even alone: a package copy aligns before serving. */
    for (size_t scope = 0; scope < b->count && !hands; scope++)
        for (size_t i = 0; i < b->scopes[scope]->resource_count; i++) {
            const sh_compiled_resource *file = b->scopes[scope]->resources + i;
            if (!file->type || strcmp(file->type, "animweb") || !file->source_count) continue;
            if (!sh_package_player_hands_webs(original, context, &hands, &hand_count)) goto done;
            if (!hands) hands = calloc(1, sizeof(*hands));
            if (!hands) goto done;
            break;
        }
    for (size_t scope = 0; scope < b->count; scope++) {
        int legacy = b->contexts && scope < b->authored_count &&
            b->contexts->contexts[scope].mode == SH_PACKAGE_CONTEXT_COMPOSED;
        const sh_package_compilation *compiled = b->scopes[scope];
        for (size_t i = 0; i < compiled->resource_count; i++) {
            const sh_compiled_resource *file = compiled->resources + i;
            size_t prior;
            int several = 0;
            /* A frozen legacy context composes only its AI registrations. */
            if (legacy) {
                int stated = pl_registration_file(compiled, file, error, capacity);
                if (stated < 0) goto done;
                if (!stated) continue;
            }
            if (file->type && !strcmp(file->type, "animweb")) {
                for (size_t other = 0; other < b->count && !several; other++)
                    several = other != scope && sh_package_compilation_find(b->scopes[other], file->engine_path) != NULL;
                for (size_t h = 0; h < hand_count && !several; h++) {
                    char path[512];
                    snprintf(path, sizeof(path), "generated/decls/animweb/%s.decl", hands[h]);
                    several = !_stricmp(path, file->engine_path);
                }
            }
            if (!pl_shared_declaration(file->type) && !several) continue;
            for (prior = 0; prior < count; prior++) if (!strcmp(projections[prior].engine_path, file->engine_path)) break;
            if (prior < count) continue;
            sh_compiled_resource *seed = seeds + count;
            int status = pl_original_resource(original, context, file->engine_path, seed);
            if (status < 0 || (status && !seed->body)) goto done;
            if (status != 1 && status != 3) { pl_resource_body_free(seed); memset(seed, 0, sizeof(*seed)); continue; }
            seed->engine_path = file->engine_path; seed->source = SIZE_MAX;
            seed->baseline_known = status; seed->native_original = status == 1;
            members[count] = seed;
            projections[count] = (sh_package_resource_projection){file->engine_path, members + count, 1};
            count++;
        }
    }
    /* Stock descendants of the entries whose inherited controls a package moves. */
    for (size_t i = 0; rows && i < rows->count; i++) {
        int descends = 0;
        const sh_editor_row *up = rows->items + i;
        for (unsigned depth = 0; !descends && up && up->parent && depth < 32; depth++) {
            for (size_t r = 0; r < relay_count && !descends; r++) descends = !strcmp(relays[r], up->parent);
            up = pl_editor_row(rows, up->parent);
        }
        if (!descends) continue;
        size_t length = strlen(rows->items[i].name) + sizeof(PL_EDITOR_PREFIX) + 5, prior;
        char *path = malloc(length);
        if (!path) goto done;
        snprintf(path, length, PL_EDITOR_PREFIX "%s.decl", rows->items[i].name);
        for (prior = 0; prior < count; prior++) if (!strcmp(projections[prior].engine_path, path)) break;
        if (prior < count) { free(path); continue; }
        owned[owned_count++] = path;
        sh_compiled_resource *seed = seeds + count;
        int status = pl_original_resource(original, context, path, seed);
        if (status < 0 || (status && !seed->body)) goto done;
        if (status != 1 && status != 3) { pl_resource_body_free(seed); memset(seed, 0, sizeof(*seed)); continue; }
        seed->engine_path = path; seed->source = SIZE_MAX;
        seed->baseline_known = status; seed->native_original = status == 1;
        members[count] = seed;
        projections[count] = (sh_package_resource_projection){path, members + count, 1};
        count++;
    }
    b->scopes[b->count] = sh_package_compilation_project(sources, projections, count, NULL,
        b->shared->canonical_path, error, capacity);
    if (!b->scopes[b->count]) goto done;
    if (PL_SHARE) sh_package_compilation_share(b->scopes[b->count]);
    b->count++; ok = 1;
done:
    if (seeds) for (size_t i = 0; i < maximum; i++) pl_resource_body_free(seeds + i);
    for (size_t h = 0; h < hand_count; h++) free(hands[h]);
    for (size_t i = 0; i < owned_count; i++) free(owned[i]);
    for (size_t i = 0; i < relay_count; i++) free(relays[i]);
    free(owned); free(relays); free(hands); free(seeds); free(projections); free(members); return ok;
}

typedef struct pl_assignment {
    const sh_package_link_inputs *inputs;
    const unsigned char *shared;
    int materials_prepared;
    const unsigned char *global; /* player hands webs (pl_hands_phase) */
} pl_assignment;

static int pl_assign(void *context, size_t index, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths,
    char *error, size_t capacity)
{
    pl_assignment *a = context;
    const sh_package_link_unit *unit = a->inputs->units + index;
    if ((a->shared[index] && pl_shared_declaration(unit->family)) || (a->global && a->global[index])) {
        /* This is the one composed editor object, not a claim that its bytes
         * equal vanilla. Its private dependencies would have disqualified it. */
        *name = _strdup(unit->name); *paths = calloc(count ? count : 1, sizeof(**paths));
        if (!*name || !*paths) return 0;
        for (size_t i = 0; i < count; i++) if (!((*paths)[i] = _strdup(files[i]->engine_path))) return 0;
        return 1;
    }
    /* A non-generic material's renamed identity needs an inspected native atlas
     * binding, including a proven no-row result for a nonvirtual material. */
    if (!a->materials_prepared && !original_binding && !strcmp(unit->family, "material") && strncmp(unit->name, "generic", 7)) {
        snprintf(error, capacity, "linked material requires its prepared native atlas binding: %s", unit->name);
        return 0;
    }
    return sh_package_native_names_assign((void *)a->inputs, index, identity, original_binding,
        files, count, name, paths, error, capacity);
}

typedef struct pl_contribution_edge { char *slot; size_t target; } pl_contribution_edge;
/* The registration-key sites one binding class reports (pl_key_scan_of): each
 * site's slot and value, in the reader's order. */
typedef struct pl_key_site { char *slot, *value; size_t length; } pl_key_site;
typedef struct pl_key_scan {
    unsigned char identity[32];
    int token_text; /* scanned the token text (relocation), else the captured bytes (spawn capture) */
    int status;     /* sh_package_source_link_bind_keys status of the scan */
    pl_key_site *sites;
    size_t count;
} pl_key_scan;
typedef struct pl_contribution_unit {
    sh_package_compilation *owned;
    const sh_compiled_resource *file;
    pl_contribution_edge *edges;
    size_t count, capacity;
} pl_contribution_unit;
/* Compiler-only exact localization witnesses and the immutable late binding.
 * Mapping bytes never become provider files or new native resource roots. */
typedef struct pl_movie_string {
    size_t unit;
    char *slot;
    const char *key;
    sh_compiled_resource witness;
} pl_movie_string;
struct pl_contributions {
    sh_package_source_link *source;
    /* inputs.units is the capture's unit table, lent (R-343): its first captured rows are the capture's own records
     * until edited; pl_unit_keep keeps the capture's copy of a row before its first edit. The capture frees it. */
    sh_package_link_inputs inputs;
    size_t captured;
    pl_contribution_unit *units;
    size_t *origins, *scopes; /* Private registry units borrow their source reader. */
    size_t unit_capacity;
    sh_package_link_edge **implicit;
    sh_package_link_witness **witnesses;
    pl_movie_string **movie_strings;
    size_t movie_string_count;
    sh_package_link_classes classes;
    char **tokens;
    size_t *token_order;
    size_t token_count, pending_unit, pending_target;
    unsigned char *shared;
    unsigned char *global; /* members of a composed player hands web */
    pl_assignment *assignment;
    int dirty;
    /* Identity captures of untouched units, shared by every pass of this
     * build: the contribution phases and the final link bind them alike. */
    sh_package_link_reuse *reuse;
    /* Registration-key site scans by class identity and text (open addressing). */
    pl_key_scan **key_scans;
    size_t key_scan_count, key_scan_capacity;
    /* Developer diagnostics: each unit's shared-name certification (1 named
     * by the one-class rule, 2 denied by pl_shared_certify), the build for
     * scope labels, and the bounded report of a refused native identity. */
    unsigned char *certification;
    struct pl_build *build;
    char *notes;
    size_t notes_used, note_count;
    /* Meters only: pl_tokens' own time outside the identity pass, by step. */
    unsigned long long tok_calls, tok_names_ms, tok_format_ms, tok_order_ms;
    /* While one group stages its members (pl_stage_pool_open), the exact staged bytes they share. */
    struct pl_stage_pool *stage_pool;
};
/* The staged bodies of one group's members: a member staged with exactly the bytes another member of the group
 * staged takes a reference to them instead of a copy (pl_stage_body). The pool holds a reference to each until the
 * group closes it. */
typedef struct pl_stage_pool { sh_package_body **bodies; size_t count, capacity; } pl_stage_pool;
static void pl_stage_pool_open(pl_contributions *c, pl_stage_pool *pool)
{ memset(pool, 0, sizeof(*pool)); c->stage_pool = pool; }
static void pl_stage_pool_close(pl_contributions *c, pl_stage_pool *pool)
{
    for (size_t i = 0; i < pool->count; i++) sh_package_body_release(pool->bodies[i]);
    free(pool->bodies); memset(pool, 0, sizeof(*pool));
    if (c->stage_pool == pool) c->stage_pool = NULL;
}
/* Staged bytes as a shared body the projection takes a reference to: the open group's equal bytes (*bytes is freed),
 * else *bytes itself (adopted, *bytes is NULL), else NULL with *bytes the caller's (a refused owner: copy them). */
static sh_package_body *pl_stage_body(pl_contributions *c, unsigned char **bytes, size_t length)
{
    pl_stage_pool *pool = c->stage_pool;
    if (!PL_SHARE) return NULL;
    for (size_t i = 0; pool && i < pool->count; i++)
        if (sh_package_body_length(pool->bodies[i]) == length && !memcmp(sh_package_body_bytes(pool->bodies[i]), *bytes, length)) {
            free(*bytes); *bytes = NULL; return sh_package_body_retain(pool->bodies[i]);
        }
    sh_package_body *owner = sh_package_body_adopt(*bytes, length);
    if (!owner) return NULL;
    *bytes = NULL;
    if (pool && pool->count == pool->capacity) {
        size_t next = pool->capacity ? pool->capacity * 2 : 8;
        sh_package_body **grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(pool->bodies, next * sizeof(*grown)) : NULL;
        if (grown) { pool->bodies = grown; pool->capacity = next; }
    }
    /* A pool that cannot grow only misses sharing with later members. */
    if (pool && pool->count < pool->capacity) pool->bodies[pool->count++] = sh_package_body_retain(owner);
    return owner;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* sh_package_library_test_snapshot_twins: each slot reads its own column (context: 0 or 1). */
static int pl_twin_read(void *context, const char *path, unsigned char **body, size_t *length)
{
    static const char *const rows[][3] = {
        {"decls/twin/equal.decl", "{ same bytes }", "{ same bytes }"},
        {"decls/twin/unequal.decl", "{ slot zero }", "{ slot 0ne! }"},
        {"decls/twin/one.decl", NULL, "{ only one }"},
    };
    size_t column = *(const size_t *)context;
    *body = NULL; *length = 0;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) if (!strcmp(rows[i][0], path)) {
        const char *text = rows[i][column + 1];
        if (!text) return 0;
        *length = strlen(text);
        if (!(*body = malloc(*length + 1))) return -1;
        memcpy(*body, text, *length + 1); return 1;
    }
    return 0;
}
int sh_package_library_test_snapshot_twins(char *error, size_t capacity)
{
    static const char *const paths[] = {"decls/twin/equal.decl", "decls/twin/unequal.decl", "decls/twin/one.decl"};
    static const char *const expected[2][3] = {{"{ same bytes }", "{ slot zero }", NULL}, {"{ same bytes }", "{ slot 0ne! }", "{ only one }"}};
    long live = sh_package_body_test_live();
    for (int order = 0; order < 2; order++) {
        size_t columns[2] = {0, 1};
        pl_originals slots[2] = {{pl_twin_read, columns}, {pl_twin_read, columns + 1}};
        const char *reason = NULL;
        slots[0].peer = slots + 1; slots[1].peer = slots;
        for (int k = 0; k < 2 && !reason; k++)
            for (size_t p = 0; p < 3 && !reason; p++)
                if (!pl_original_find(slots + (order ? 1 - k : k), paths[p])) reason = "a snapshot read failed";
        /* Entries move as each slot grows: look them up again. */
        const pl_original *at[2][3];
        for (int k = 0; k < 2; k++) for (size_t p = 0; p < 3; p++) at[k][p] = pl_original_at(slots + k, paths[p]);
        for (int k = 0; k < 2 && !reason; k++)
            for (size_t p = 0; p < 3 && !reason; p++) {
                const pl_original *file = at[k][p];
                const char *want = expected[k][p];
                if (!file || (want ? file->status != 1 || !file->owner || file->length != strlen(want) ||
                        memcmp(file->body, want, file->length + 1) || file->body != sh_package_body_bytes(file->owner) :
                        file->status != 0 || file->owner || file->body)) reason = "a slot does not keep its own answer";
            }
        if (!reason && at[0][0]->owner != at[1][0]->owner) reason = "exactly equal bytes at one path are not shared";
        if (!reason && at[0][1]->owner == at[1][1]->owner) reason = "different bytes of one length are shared";
        if (!reason && sh_package_body_test_live() != live + 4) reason = "the slots hold other than four bodies";
        for (int k = 0; k < 2; k++) {
            for (size_t i = 0; i < slots[k].count; i++) { free(slots[k].files[i].path); sh_package_body_release(slots[k].files[i].owner); }
            free(slots[k].files);
        }
        if (!reason && sh_package_body_test_live() != live) reason = "a released snapshot leaves bodies behind";
        if (reason) { snprintf(error, capacity, "snapshot twins (%s first): %s", order ? "slot 1" : "slot 0", reason); return 0; }
    }
    return 1;
}
int sh_package_library_test_stage_pool(char *error, size_t capacity)
{
    static const char *const texts[] = {"alpha", "alpha", "alphb", "alphas"};
    pl_contributions c;
    pl_stage_pool pool;
    sh_package_body *owners[4] = {0};
    const char *reason = NULL;
    long live = sh_package_body_test_live();
    memset(&c, 0, sizeof(c));
    pl_stage_pool_open(&c, &pool);
    for (size_t i = 0; i < 4 && !reason; i++) {
        size_t length = strlen(texts[i]);
        unsigned char *bytes = malloc(length + 1);
        if (!bytes) { reason = "test allocation failed"; break; }
        memcpy(bytes, texts[i], length + 1);
        owners[i] = pl_stage_body(&c, &bytes, length);
        if (!owners[i] || bytes) { free(bytes); reason = "staged bytes did not move to a body"; break; }
        if (sh_package_body_length(owners[i]) != length || memcmp(sh_package_body_bytes(owners[i]), texts[i], length + 1))
            reason = "a staged body does not hold its bytes";
    }
    if (!reason && owners[0] != owners[1]) reason = "exactly equal staged bytes are not one body";
    if (!reason && (owners[2] == owners[0] || owners[3] == owners[0] || owners[3] == owners[2]))
        reason = "different staged bytes share a body";
    if (!reason && sh_package_body_test_live() != live + 3) reason = "the group holds other than three bodies";
    if (!reason) {
        /* A refused owner leaves the bytes the caller's, to copy. */
        unsigned char *bytes = malloc(5);
        if (!bytes) reason = "test allocation failed";
        else {
            memcpy(bytes, "beta", 5);
            sh_package_body_test_refusal(1);
            sh_package_body *none = pl_stage_body(&c, &bytes, 4);
            sh_package_body_test_refusal(0);
            if (none || !bytes || memcmp(bytes, "beta", 5)) { sh_package_body_release(none); reason = "a refused owner took the bytes"; }
            free(bytes);
        }
    }
    for (size_t i = 0; i < 4; i++) sh_package_body_release(owners[i]);
    pl_stage_pool_close(&c, &pool);
    if (!reason && (c.stage_pool || sh_package_body_test_live() != live)) reason = "a closed group leaves bodies behind";
    if (reason) { snprintf(error, capacity, "staging pool: %s", reason); return 0; }
    return 1;
}
#endif
static void pl_key_scan_free(pl_key_scan *scan)
{
    if (!scan) return;
    for (size_t i = 0; i < scan->count; i++) { free(scan->sites[i].slot); free(scan->sites[i].value); }
    free(scan->sites); free(scan);
}

static void pl_contribution_unit_free(pl_contribution_unit *unit)
{
    for (size_t i = 0; i < unit->count; i++) free(unit->edges[i].slot);
    free(unit->edges); sh_package_compilation_free(unit->owned); memset(unit, 0, sizeof(*unit));
}
static void pl_tokens_free(pl_contributions *c)
{
    for (size_t i = 0; i < c->token_count; i++) free(c->tokens[i]);
    free(c->tokens); c->tokens = NULL; c->token_count = 0;
    free(c->token_order); c->token_order = NULL;
}
/* Readers probe ordinary native names as well as contribution tokens. Keep
 * the exact token lookup logarithmic for both hits and misses; a miss must
 * still follow the captured source scope. Class indexes do not change. */
static int pl_token_order(void *context, const void *left, const void *right)
{
    const pl_contributions *c = context;
    return strcmp(c->tokens[*(const size_t *)left], c->tokens[*(const size_t *)right]);
}
static size_t pl_token_find(const pl_contributions *c, const char *name)
{
    size_t low = 0, high = c->token_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2, index = c->token_order[mid];
        int cmp = strcmp(c->tokens[index], name);
        if (!cmp) return index;
        if (cmp < 0) low = mid + 1; else high = mid;
    }
    return SIZE_MAX;
}
static void pl_contributions_close(pl_contributions *c)
{
    if (!c) return;
    if (c->units) for (size_t i = 0; i < c->inputs.count; i++) pl_contribution_unit_free(c->units + i);
    for (size_t i = 0; i < c->inputs.count; i++) {
        if (c->implicit && c->implicit[i]) {
            for (size_t j = 0; j < c->inputs.units[i].implicit_count; j++) free((char *)c->implicit[i][j].slot);
            free(c->implicit[i]);
        }
        if (c->witnesses) free(c->witnesses[i]);
    }
    pl_tokens_free(c); sh_package_link_classes_free(&c->classes);
    sh_package_link_reuse_close(c->reuse);
    for (size_t i = 0; i < c->key_scan_capacity; i++) pl_key_scan_free(c->key_scans[i]);
    free(c->key_scans);
    for (size_t i = 0; i < c->movie_string_count; i++) {
        pl_movie_string *value = c->movie_strings[i];
        free(value->slot); free(value);
    }
    free(c->movie_strings);
    free(c->origins); free(c->scopes); free(c->implicit); free(c->witnesses);
    free(c->certification); free(c->notes);
    free(c->units); free(c->shared); free(c->global); free(c); /* inputs.units is the capture's (R-343) */
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
/* Mutation controls (tests): 1 no boundary (neither the check nor the edit refusal), 2 no duplicate count of
 * occurrences. */
static int g_pl_share_mutation, g_pl_mutation_default;
#define PL_MUTATED(clause) (g_pl_share_mutation == (clause))
/* A whole-suite mutation run: SMP_SHARE_MUTATION l1-l2 (s1-s8 are the source link's), read before the first compile. */
static void pl_share_environment(void)
{
    static volatile long read;
    char *mutation = NULL;
    size_t length = 0;
    if (_InterlockedCompareExchange(&read, 1, 0)) return;
    if (!_dupenv_s(&mutation, &length, "SMP_SHARE_MUTATION") && mutation && mutation[0] == 'l' &&
        mutation[1] >= '1' && mutation[1] <= '2') g_pl_mutation_default = mutation[1] - '0';
    free(mutation);
    g_pl_share_mutation = g_pl_mutation_default;
}
void sh_package_library_test_share_mutation(int clause)
{ pl_share_environment(); g_pl_share_mutation = clause < 0 ? g_pl_mutation_default : clause; }
#else
#define PL_MUTATED(clause) 0
#define pl_share_environment() ((void)0)
#endif
/* ---- Shared stock boundary (R-344 stage A) -----------------------------------
 * The source link binds a package scope's proven stock copies once in the baseline (sh_package_source_link_share).
 * What a scope's contributions select or edit for that scope must be that scope's own unit. Before the contribution
 * phase, every scope's occurrences of shared stock are checked against what the phase selects by scope
 * (pl_share_boundary); at an edit, a row a package scope shares is never edited (pl_unit_keep). Either way the scopes
 * that share it are recorded, this compile ends before publication without blaming anyone, and pl_compile compiles
 * again with those scopes unshared: their own copies, exactly as without sharing. */
static void pl_unshare_scope(pl_build *b, size_t scope)
{
    if (!b->unshare || scope >= b->count || b->unshare[scope]) return;
    b->unshare[scope] = 1; b->unshare_count++;
}
/* Record every package scope whose occurrences include shared row unit; how many share it. */
static size_t pl_unshare_row(pl_build *b, size_t unit)
{
    const sh_package_link_unit *row = sh_package_source_link_unit(b->reader, unit);
    size_t sharers = 0;
    for (size_t scope = 0; row && scope < b->count; scope++) {
        size_t found = SIZE_MAX;
        if (sh_package_source_link_scope_find(b->reader, scope, row->family, row->name, &found) && found == unit) {
            pl_unshare_scope(b, scope); sharers++;
        }
    }
    return sharers;
}
/* Before the first edit of captured row unit, keep the capture's copy of its record (R-343); appended rows have none.
 * 0 when the copy cannot be allocated: the caller leaves the row as it is and fails. The edit is for one scope: a
 * shared stock row binds every scope that shares it, so it is not edited, and the scopes sharing it are recorded
 * instead (0, as a failure the compile then discards). */
static int pl_unit_keep(pl_contributions *c, size_t unit)
{
    if (unit < c->captured && sh_package_source_link_shared_stock(c->source, unit) && !PL_MUTATED(1) &&
        pl_unshare_row(c->build, unit)) return 0;
    return unit >= c->captured || sh_package_source_link_preserve_unit(c->source, unit);
}
/* The class-wide naming edit (fixed and stable names) is the same for every copy of a class: a shared row takes it. */
static int pl_unit_keep_named(pl_contributions *c, size_t unit)
{ return unit >= c->captured || sh_package_source_link_preserve_unit(c->source, unit); }
static int pl_modern(const pl_build *b, size_t scope);
/* What a scope-specific contribution step selects of a shared row in some scope: 1 in any scope (weapons, player
 * hands webs, module catalog cards, cooked movies, AI registrations, registration-key sites, or text it cannot read),
 * 2 in a modern scope (editor collections), 3 never. */
static unsigned char pl_share_selected(pl_build *b, size_t unit)
{
    const sh_package_link_unit *row = sh_package_source_link_unit(b->reader, unit);
    int keys = -1, conductor = -1;
    if (!row || !strcmp(row->family, "weapon") || !strcmp(row->family, "snapmoduleinfo") ||
        sh_package_movie_owner(row->family, row->name) ||
        (!strcmp(row->family, "animweb") && sh_package_source_link_player_hands(b->reader, row->name))) return 1;
    if (!sh_package_source_link_stock_facts(b->reader, unit, &keys, &conductor) || keys || conductor) return 1;
    return row->file_count == 1 && sh_package_editor_contribution_phase(row->family) ? 2 : 3;
}
/* Before the contribution phase: each package scope whose occurrences of shared stock a scope-specific step would
 * select needs its own copies (recorded; the compile ends here and pl_compile compiles again). 1 when none needs. */
static int pl_share_boundary(pl_build *b, char *error, size_t capacity)
{
    if (PL_MUTATED(1)) return 1;
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(b->reader);
    unsigned char *state = calloc(inputs->count ? inputs->count : 1, 1);
    if (!state) { snprintf(error, capacity, "cannot check shared stock"); return 0; }
    for (size_t scope = 0; scope < b->count; scope++) {
        size_t n = sh_package_source_link_scope_shared(b->reader, scope);
        for (size_t i = 0; i < n && !b->unshare[scope]; i++) {
            size_t unit = sh_package_source_link_scope_shared_unit(b->reader, scope, i);
            if (unit >= inputs->count) continue;
            if (!state[unit]) state[unit] = pl_share_selected(b, unit);
            if (state[unit] == 1 || (state[unit] == 2 && pl_modern(b, scope))) pl_unshare_scope(b, scope);
        }
    }
    free(state);
    if (!b->unshare_count) return 1;
    snprintf(error, capacity, "%zu package scope(s) need their own copies of shared stock", b->unshare_count);
    return 0;
}
/* A composed or localized unit binds through the current tokens, so only
 * untouched units may reuse an earlier identity capture. */
static void pl_reuse_refresh(pl_contributions *c)
{
    for (size_t i = 0; c->reuse && i < c->inputs.count; i++)
        if (c->units[i].owned) sh_package_link_reuse_invalidate(c->reuse, i);
}

/* Borrowed-string set for exact collision checks over many names. */
typedef struct pl_names { const char **slots; size_t capacity; } pl_names;
static uint64_t pl_names_hash(const char *text)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *text; text++) { hash ^= (unsigned char)*text; hash *= UINT64_C(1099511628211); }
    return hash;
}
static int pl_names_open(pl_names *set, size_t count)
{
    size_t capacity = 64;
    while (capacity < count * 2 + 1) { if (capacity > SIZE_MAX / 4) return 0; capacity *= 2; }
    set->slots = calloc(capacity, sizeof(*set->slots)); set->capacity = capacity;
    return set->slots != NULL;
}
/* Return 1 when text is present; otherwise add it when add is set. */
static int pl_names_seen(pl_names *set, const char *text, int add)
{
    size_t at = (size_t)pl_names_hash(text) & (set->capacity - 1);
    while (set->slots[at]) {
        if (!strcmp(set->slots[at], text)) return 1;
        at = (at + 1) & (set->capacity - 1);
    }
    if (add) set->slots[at] = text;
    return 0;
}

/* A merged field is resolved by its retained slot binding, not by looking up
 * its restored spelling in the receiving unit. Unmodified fields retain their
 * original scoped reader, including native entity inheritance/class lookup. */
static int pl_bound_target(pl_contributions *c, size_t unit, const char *slot,
    const char *family, const char *name, size_t *target, char *error, size_t capacity)
{
    pl_contribution_unit *bound = c->units + unit;
    if (bound->owned) {
        for (size_t i = 0; i < bound->count; i++) if (!strcmp(bound->edges[i].slot, slot)) {
            *target = bound->edges[i].target;
            const sh_package_link_unit *value = c->inputs.units + *target;
            if (!strcmp(value->family, family) && !strcmp(value->name, name)) return 1;
            break;
        }
        snprintf(error, capacity, "composed editor binding changed at %s/%s: %s", family, name, slot);
        return 0;
    }
    return sh_package_source_link_resolve_unit(c->source, c->origins[unit], family, name, target, error, capacity);
}

/* A composed unit's slot with no captured edge holds an entry its restore left
 * as authored because this unit's own scope supplies no such declaration
 * (pl_restore_reference): the engine defaults it by name in this scope, with no
 * binding. Another slot of the same text may spell the same name for a
 * declaration of another owner's scope; that capture says nothing about this
 * entry, since contributions run before identities give owners' variants
 * distinct names. 1 for such a default slot, 0 otherwise (bind strictly). */
static int pl_default_slot(pl_contributions *c, size_t unit, const char *slot, const char *family, const char *name)
{
    const pl_contribution_unit *bound = c->units + unit;
    size_t ignored; char error[256];
    if (!bound->owned) return 0;
    for (size_t i = 0; i < bound->count; i++) if (!strcmp(bound->edges[i].slot, slot)) return 0;
    return !sh_package_source_link_resolve_unit(c->source, c->origins[unit], family, name, &ignored, error, sizeof(error));
}

/* A single-unit rewrite keeps every reference it does not change bound to the exact unit its own
 * scope bound before the rewrite. pl_token_reference records each reference's slot, the class token
 * that replaced it and that target; pl_restore_reference restores an unchanged token to the recorded
 * unit instead of the class representative. The members of a class are equal only until a later phase
 * (key relocation of the capsules) distinguishes them, so the representative is not the unit a scope
 * authored against (R-150). */
typedef struct pl_kept_ref { char *slot; size_t token, target; } pl_kept_ref;
typedef struct pl_kept_refs { pl_kept_ref *items; size_t count, capacity; } pl_kept_refs;
static void pl_kept_free(pl_kept_refs *kept)
{
    for (size_t i = 0; i < kept->count; i++) free(kept->items[i].slot);
    free(kept->items); memset(kept, 0, sizeof(*kept));
}
static int pl_kept_add(pl_kept_refs *kept, const char *slot, size_t token, size_t target)
{
    if (kept->count == kept->capacity) {
        size_t next = kept->capacity ? kept->capacity * 2 : 16;
        pl_kept_ref *grown = next > kept->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(kept->items, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        kept->items = grown; kept->capacity = next;
    }
    char *copy = _strdup(slot);
    if (!copy) return 0;
    kept->items[kept->count++] = (pl_kept_ref){copy, token, target}; return 1;
}
/* The unit recorded for a restored token: the one at the same slot, else the one every recorded
 * reference of that token shares (a row split moves a reference to another slot); SIZE_MAX when
 * the token was not recorded or its recorded references name different units. */
static size_t pl_kept_find(const pl_kept_refs *kept, const char *slot, size_t token)
{
    size_t shared = SIZE_MAX;
    int agree = 1;
    for (size_t i = 0; kept && i < kept->count; i++) {
        if (kept->items[i].token != token) continue;
        if (!strcmp(kept->items[i].slot, slot)) return kept->items[i].target;
        if (shared == SIZE_MAX) shared = kept->items[i].target;
        else if (shared != kept->items[i].target) agree = 0;
    }
    return agree ? shared : SIZE_MAX;
}

typedef struct pl_contribution_walk {
    pl_contributions *graph;
    size_t unit;
    sh_package_link_reference reference;
    void *context;
    pl_contribution_unit *capture;
    char *error;
    size_t capacity;
    pl_kept_refs *kept;
    struct pl_occurrence *seen; /* pl_walk_slot */
    size_t seen_count, seen_capacity, qualified_capacity;
    char *qualified;
} pl_contribution_walk;

/* The library key of a walk's reference at slot. A statement stated again in
 * one text (a later update the native reader applies in order) repeats the slot
 * paths of the one before it; each occurrence is a distinct authored site with
 * its own binding, so edges and kept references key the n-th repeat "slot#n"
 * and the first the plain path. Every walk over a text visits its references
 * in document order, so walks over one text agree. NULL when allocation fails. */
typedef struct pl_occurrence { char *slot; size_t count; } pl_occurrence;
static const char *pl_walk_slot(pl_contribution_walk *walk, const char *slot)
{
    if (walk->seen_count * 2 >= walk->seen_capacity) {
        size_t next = walk->seen_capacity ? walk->seen_capacity * 2 : 64;
        pl_occurrence *grown = next <= SIZE_MAX / sizeof(*grown) ? calloc(next, sizeof(*grown)) : NULL;
        if (!grown) return NULL;
        for (size_t i = 0; i < walk->seen_capacity; i++) if (walk->seen[i].slot) {
            size_t at = (size_t)pl_names_hash(walk->seen[i].slot) & (next - 1);
            while (grown[at].slot) at = (at + 1) & (next - 1);
            grown[at] = walk->seen[i];
        }
        free(walk->seen); walk->seen = grown; walk->seen_capacity = next;
    }
    size_t at = (size_t)pl_names_hash(slot) & (walk->seen_capacity - 1);
    while (walk->seen[at].slot && strcmp(walk->seen[at].slot, slot)) at = (at + 1) & (walk->seen_capacity - 1);
    if (!walk->seen[at].slot) {
        if (!(walk->seen[at].slot = _strdup(slot))) return NULL;
        walk->seen_count++;
    }
    size_t repeat = walk->seen[at].count++;
    if (!repeat) return slot;
    size_t need = strlen(slot) + 24;
    if (need > walk->qualified_capacity) {
        char *grown = realloc(walk->qualified, need);
        if (!grown) return NULL;
        walk->qualified = grown; walk->qualified_capacity = need;
    }
    snprintf(walk->qualified, need, "%s#%zu", slot, repeat); return walk->qualified;
}
static void pl_walk_close(pl_contribution_walk *walk)
{
    for (size_t i = 0; i < walk->seen_capacity; i++) free(walk->seen[i].slot);
    free(walk->seen); free(walk->qualified);
    walk->seen = NULL; walk->qualified = NULL; walk->seen_count = walk->seen_capacity = walk->qualified_capacity = 0;
}

static int pl_derived_exists(void *context, const char *family, const char *name, int *exists)
{
    pl_contribution_walk *walk = context;
    pl_contributions *c = walk->graph;
    size_t i = pl_token_find(c, name);
    if (i != SIZE_MAX) {
        const sh_package_link_unit *target = c->inputs.units + c->classes.representatives[i];
        *exists = !strcmp(target->family, family); return 1;
    }
    pl_contribution_unit *bound = c->units + walk->unit;
    int captured = 0;
    for (size_t i = 0; i < bound->count; i++) {
        const sh_package_link_unit *target = c->inputs.units + bound->edges[i].target;
        if (strcmp(target->name, name)) continue;
        captured = 1;
        if (!strcmp(target->family, family)) { *exists = 1; return 1; }
    }
    if (captured) { *exists = 0; return 1; }
    return 0;
}

static int pl_bound_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    pl_contributions *c = walk->graph;
    size_t target;
    const char *key = pl_walk_slot(walk, slot);
    if (!key) { snprintf(walk->error, walk->capacity, "editor contribution allocation failed"); return -1; }
    if (pl_default_slot(c, walk->unit, key, family, name)) return 0;
    if (!pl_bound_target(c, walk->unit, key, family, name, &target, walk->error, walk->capacity)) return -1;
    c->pending_unit = walk->unit; c->pending_target = target;
    int result = walk->reference(walk->context, slot, family, c->inputs.units[target].name, replacement);
    c->pending_unit = c->pending_target = SIZE_MAX;
    return result;
}
static int pl_bound_resolve(void *context, size_t unit, const char *family, const char *name,
    size_t *target, char *error, size_t capacity)
{
    pl_contributions *c = context;
    if (c->pending_unit == unit && c->pending_target < c->inputs.count) {
        const sh_package_link_unit *value = c->inputs.units + c->pending_target;
        if (!strcmp(value->family, family) && !strcmp(value->name, name)) { *target = c->pending_target; return 1; }
    }
    snprintf(error, capacity, "editor binding replay has no captured target: %s/%s", family, name); return 0;
}
static unsigned char *pl_bound_bind(void *context, size_t unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, sh_package_link_reference reference, void *reference_context,
    size_t *output_length, char *error, size_t capacity)
{
    pl_contributions *c = context;
    pl_contribution_walk walk = {c, unit, reference, reference_context, NULL, error, capacity};
    unsigned char *out = !c->units[unit].owned ?
        sh_package_source_link_bind_file(c->source, c->origins[unit], file, body, length,
            pl_bound_reference, &walk, output_length, error, capacity) :
        sh_package_source_link_bind_derived(c->source, c->origins[unit], body, length,
            pl_bound_reference, &walk, pl_derived_exists, &walk, output_length, error, capacity);
    pl_walk_close(&walk); return out;
}
/* A unit the contributions did not compose binds through the source reader's
 * frozen scoped resolution alone; a composed unit binds through its captured
 * slots and the current tokens (pl_bound_bind). */
static int pl_bound_stable(void *context, size_t unit)
{
    const pl_contributions *c = context;
    return unit < c->inputs.count && !c->units[unit].owned;
}
/* Such a unit's files were bound by discovery with that same reader and
 * resolution; their sites seed its first identity capture. */
static int pl_bound_sites(void *context, size_t unit, const sh_compiled_resource *file,
    const sh_package_link_edge **sites, size_t *count)
{
    const pl_contributions *c = context;
    if (!pl_bound_stable(context, unit)) return 0;
    return sh_package_source_link_sites(c->source, c->origins[unit], file, sites, count);
}
/* An untouched unit's files are the source capture's; its unsupplied
 * originals are identified once for all the scopes that bind them. A composed
 * unit identifies its own files. */
static int pl_bound_identity(void *context, size_t unit, const sh_compiled_resource *file,
    sh_package_file_identity *out, char *error, size_t capacity)
{
    const pl_contributions *c = context;
    if (unit < c->inputs.count && !c->units[unit].owned)
        return sh_package_source_link_identity(c->source, c->origins[unit], file, out, error, capacity);
    return sh_package_compilation_identity(c->inputs.units[unit].compilation, file, out, error, capacity);
}
static int pl_bound_assign(void *context, size_t unit, const unsigned char identity[32], int original_binding,
    const sh_compiled_resource *const *files, size_t count, char **name, char ***paths,
    char *error, size_t capacity)
{
    pl_contributions *c = context;
    return pl_assign(c->assignment, unit, identity, original_binding, files, count, name, paths, error, capacity);
}
/* Developer diagnostics for a refused native identity: this build's own facts
 * about one unit (its scope and owner, composition and certification state). */
static void pl_bound_describe(void *context, size_t unit, char *out, size_t capacity)
{
    const pl_contributions *c = context;
    const pl_build *b = c->build;
    if (!b || unit >= c->inputs.count) { snprintf(out, capacity, "unit outside the contributions"); return; }
    size_t scope = c->inputs.scopes[unit], origin = c->origins[unit];
    const char *label = "(unlabelled)";
    if (scope == b->count - 1) label = "baseline";
    else if (scope < b->authored_count && b->views[scope].component < b->views[scope].sources.component_count) {
        const char *root = b->views[scope].sources.components[b->views[scope].component].root;
        size_t n = root ? strlen(root) : 0;
        label = root ? (n > 72 ? root + n - 72 : root) : label;
    } else if (scope >= b->authored_count) label = "context";
    const sh_package_link_inputs *captured = sh_package_source_link_inputs(c->source);
    snprintf(out, capacity, "scope %zu (%s) of %zu, origin %zu, contribution class %zu, shared %d, global %d, "
        "certification %d, composed %d, captured original bytes %d, gameplay-free %d, files %zu",
        scope, label, b->count, origin, unit < c->classes.node_count ? c->classes.nodes[unit] : SIZE_MAX,
        c->shared[unit], c->global[unit], c->certification ? c->certification[unit] : -1,
        c->units[unit].owned != NULL,
        origin < captured->count ? sh_package_source_link_unit(c->source, origin)->original_binding : -1,
        origin < captured->count ? sh_package_source_link_shared(c->source)[origin] : -1,
        c->inputs.units[unit].file_count);
}
static void pl_bound_note(void *context, const char *line)
{
    pl_contributions *c = context;
    size_t length = strlen(line);
    if (c->note_count >= 256 || c->notes_used + length + 2 > 256 * 1024) return;
    char *grown = realloc(c->notes, c->notes_used + length + 2);
    if (!grown) return;
    c->notes = grown; memcpy(c->notes + c->notes_used, line, length);
    c->notes_used += length; c->notes[c->notes_used++] = '\n'; c->notes[c->notes_used] = 0; c->note_count++;
}
static int pl_bound_assign_members(void *context, size_t unit, const char *name,
    const sh_compiled_resource *const *files, const char *const *paths, size_t count,
    char ***names, char *error, size_t capacity)
{
    pl_contributions *c = context;
    return sh_package_native_names_assign_members((void *)c->assignment->inputs, unit, name,
        files, paths, count, names, error, capacity);
}
/* A module variant publishes each name-derived member (sh_package_module_member)
 * where the engine derives it from the variant's own path, exactly as it derives
 * the installed module's: maps/modules/<R> becomes maps/modules/smp/<token>/<R>
 * and the basename <B> is unchanged. A card is the declaration of that derived
 * name, navigation keeps its manager name, every other member is its path. */
static int pl_bound_derive(void *context, size_t unit, const char *name, const char *slot, size_t target,
    char **path, char **native_name, char *error, size_t capacity)
{
    pl_contributions *c = context;
    const sh_package_link_unit *owner = unit < c->inputs.count ? c->inputs.units + unit : NULL;
    const sh_package_link_unit *member = target < c->inputs.count ? c->inputs.units + target : NULL;
    char from[1024], to[1024], type[SH_DECL_SERVER_TYPE_CAP], declared[SH_DECL_SERVER_NAME_CAP];
    char source[SH_DECL_SERVER_SOURCE_CAP];
    const char *role, *original, *reason = NULL;
    size_t a, b;
    *path = *native_name = NULL;
    if (!owner || !member || strcmp(owner->family, "snapmodule") || !slot || strncmp(slot, "module/", 7) ||
        !name || !strcmp(name, owner->name)) return 0;
    role = slot + 7; original = strchr(role, '/');
    a = strlen(owner->name); b = strlen(name);
    if (!original || a <= 5 || b <= 5 || a - 5 >= sizeof(from) || b - 5 >= sizeof(to)) {
        snprintf(error, capacity, "module member slot has no derivable path: %s", slot); return -1;
    }
    memcpy(from, owner->name, a - 5); from[a - 5] = 0;
    memcpy(to, name, b - 5); to[b - 5] = 0;
    if (!(*path = sh_package_module_member_rename(original + 1, from, to))) {
        snprintf(error, capacity, "module member %s does not follow module %s", original + 1, owner->name); return -1;
    }
    if (!strcmp(member->family, "snapmoduleinfo")) {
        if (strncmp(*path, "generated/decls/", 16) || !sh_decl_server_identity_from_relative(*path + 16, type, sizeof(type),
            declared, sizeof(declared), source, sizeof(source), &reason) || strcmp(type, "snapmoduleinfo")) {
            snprintf(error, capacity, "module variant card %s is not a declaration identity: %s", *path, reason ? reason : "");
            return -1;
        }
        *native_name = _strdup(declared);
    } else if (!strcmp(member->family, "aas")) {
        const char *kind = NULL;
        if (sh_package_cooked_identity(*path, &kind, native_name) != 1 || !kind || strcmp(kind, "aas")) {
            snprintf(error, capacity, "module variant navigation %s has no manager name", *path); return -1;
        }
    } else *native_name = _strdup(*path);
    if (!*native_name) { snprintf(error, capacity, "cannot allocate a module variant member name"); return -1; }
    /* Binary members may be served from the installed copy (a card registers
     * its derived name from its own published text). */
    return strcmp(member->family, "snapmoduleinfo") ? 2 : 1;
}

#ifdef SH_PACKAGE_LIBRARY_TESTING
static long g_token_failures, g_rejudge_failures;
void sh_package_library_test_token_failures(long count) { g_token_failures = count; }
void sh_package_library_test_rejudge_failures(long count) { g_rejudge_failures = count; }
#endif
static int pl_tokens(pl_contributions *c, char *error, size_t capacity)
{
    pl_names captured = {0}, issued = {0};
    int ok = 0;
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if (g_token_failures > 0 && c->build && c->build->contribution_rejections.count && g_token_failures--) {
        snprintf(error, capacity, "contribution tokens could not be rebound"); return 0;
    }
#endif
    if (!c->dirty) return 1;
    pl_tokens_free(c);
    pl_reuse_refresh(c);
    sh_package_link_adapter adapter = {c, pl_bound_resolve, pl_bound_bind};
    adapter.reuse = c->reuse; adapter.sites = pl_bound_sites; adapter.identity = pl_bound_identity;
    if (!sh_package_link_identify_units(c->inputs.units, c->inputs.count, &adapter, &c->classes, error, capacity)) return 0;
    unsigned long long meter = pl_now_ms();
    c->tokens = calloc(c->classes.count ? c->classes.count : 1, sizeof(*c->tokens));
    c->token_order = calloc(c->classes.count ? c->classes.count : 1, sizeof(*c->token_order));
    if (!c->tokens || !c->token_order || !pl_names_open(&captured, c->inputs.count) || !pl_names_open(&issued, c->classes.count)) goto done;
    for (size_t j = 0; j < c->inputs.count; j++) (void)pl_names_seen(&captured, c->inputs.units[j].name, 1);
    c->tok_names_ms += pl_now_ms() - meter; meter = pl_now_ms();
    c->token_count = c->classes.count;
    for (size_t i = 0; i < c->token_count; i++) {
        /* A token must read back through its field's reader: a movie field
         * names a cooked pair by its generated/swf path, and model fields
         * dispatch on the extension (an md6 definition by .md6 or .decl, a
         * breakable declaration by .decl). Keep the class's own extension. */
        const sh_package_link_unit *represented = c->inputs.units + c->classes.representatives[i];
        char token[256] = "smp-contribution/", alias[128];
        size_t prefix = strlen(token);
        for (size_t k = 0; k < 32; k++) snprintf(token + prefix + k * 2, 3, "%02x", c->classes.identities[i][k]);
        const char *slash = strrchr(represented->name, '/'), *dot = strrchr(represented->name, '.');
        if (sh_package_movie_owner(represented->family, represented->name)) {
            /* The generated movie path partitions a long stem like the native
             * constructor does; derive it rather than spell it. */
            sh_swf_binary_paths linked;
            snprintf(alias, sizeof(alias), "swf/%s.swf", token);
            if (!sh_swf_binary_paths_for(alias, &linked) || strlen(linked.movie) >= sizeof(token)) {
                snprintf(error, capacity, "editor binding token has no movie spelling"); goto done;
            }
            strcpy_s(token, sizeof(token), linked.movie);
        } else if (dot && dot > (slash ? slash : represented->name) && dot[1] && strlen(dot) < 16)
            strcat_s(token, sizeof(token), dot);
        /* These tokens exist only while composing. Verify their disjointness
         * from captured native names and from every unequal exact class. */
        if (pl_names_seen(&captured, token, 0)) {
            snprintf(error, capacity, "editor binding token collides with a captured resource"); goto done;
        }
        if (!(c->tokens[i] = _strdup(token))) goto done;
        c->token_order[i] = i;
        if (pl_names_seen(&issued, c->tokens[i], 1)) {
            snprintf(error, capacity, "unequal editor bindings have colliding identities"); goto done;
        }
    }
    c->tok_format_ms += pl_now_ms() - meter; meter = pl_now_ms();
    qsort_s(c->token_order, c->token_count, sizeof(*c->token_order), pl_token_order, c);
    c->tok_order_ms += pl_now_ms() - meter; c->tok_calls++;
    c->dirty = 0; ok = 1;
done:
    free((void *)captured.slots); free((void *)issued.slots);
    return ok;
}

typedef struct pl_original_walk {
    pl_contributions *contributions;
    sh_package_link_edge *edges;
    size_t count, capacity;
} pl_original_walk;

static int pl_original_edge(pl_original_walk *walk, size_t target)
{
    if (target >= walk->contributions->inputs.count) return 0;
    if (walk->count == walk->capacity) {
        size_t next = walk->capacity ? walk->capacity * 2 : 16;
        sh_package_link_edge *grown = next > walk->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(walk->edges, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        walk->edges = grown; walk->capacity = next;
    }
    /* Only dependency reachability is used by the originals walk. */
    walk->edges[walk->count++] = (sh_package_link_edge){NULL, target};
    return 1;
}
static int pl_original_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_original_walk *walk = context;
    (void)slot; (void)family; (void)name; (void)replacement;
    return pl_original_edge(walk, walk->contributions->pending_target) ? 0 : -1;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests: 0 takes the frozen sites without binding the body beside them (timing runs); 1, the default, checks. */
static int g_pl_document_oracle = 1;
void sh_package_library_test_document_oracle(int enabled) { g_pl_document_oracle = enabled != 0; }
static int pl_size_order(const void *a, const void *b)
{ size_t x = *(const size_t *)a, y = *(const size_t *)b; return x < y ? -1 : x > y; }
/* Tests: the targets a fresh bind of file's body reaches (pl_original_reference) are the frozen sites' targets, as
 * sets; the compile fails otherwise. body may be NULL (read here). */
static int pl_document_sites_checked(pl_contributions *c, size_t unit, const sh_compiled_resource *file,
    const unsigned char *body, size_t length, const sh_package_link_edge *sites, size_t site_count,
    char *error, size_t capacity)
{
    pl_original_walk walk = {c};
    size_t bound_length = 0, *bound_targets = NULL, *site_targets = NULL, n = 0, m = 0;
    unsigned char *own = NULL, *bound;
    int ok = 0;
    if (!body) {
        own = sh_package_compilation_read(c->inputs.units[unit].compilation, file, SIZE_MAX, &length, error, capacity);
        if (!own) return 0;
        body = own;
    }
    bound = pl_bound_bind(c, unit, file, body, length, pl_original_reference, &walk, &bound_length, error, capacity);
    free(own);
    if (!bound) { free(walk.edges); return 0; }
    free(bound);
    bound_targets = malloc((walk.count ? walk.count : 1) * sizeof(*bound_targets));
    site_targets = malloc((site_count ? site_count : 1) * sizeof(*site_targets));
    if (!bound_targets || !site_targets) { snprintf(error, capacity, "document sites oracle allocation failed"); goto done; }
    for (size_t k = 0; k < walk.count; k++) bound_targets[k] = walk.edges[k].target;
    for (size_t k = 0; k < site_count; k++) site_targets[k] = sites[k].target;
    qsort(bound_targets, walk.count, sizeof(size_t), pl_size_order);
    qsort(site_targets, site_count, sizeof(size_t), pl_size_order);
    for (size_t k = 0; k < walk.count; k++) if (!n || bound_targets[n - 1] != bound_targets[k]) bound_targets[n++] = bound_targets[k];
    for (size_t k = 0; k < site_count; k++) if (!m || site_targets[m - 1] != site_targets[k]) site_targets[m++] = site_targets[k];
    ok = n == m && (!n || !memcmp(bound_targets, site_targets, n * sizeof(size_t)));
    if (!ok) snprintf(error, capacity, "document sites oracle: %s %s binds %zu target(s) where its frozen sites name %zu (%s)",
        c->inputs.units[unit].family, c->inputs.units[unit].name, n, m, file->engine_path ? file->engine_path : "?");
done:
    free(bound_targets); free(site_targets); free(walk.edges);
    return ok;
}
#endif

/* An imported document may reuse an unchanged native editor slot, but must not
 * replace an existing shared selector/inspector with its authored extensions.
 * This is same-name, exact-source reuse, not a name-independent reader claim.
 * A changed dependency also makes its unchanged parent private. */
static int pl_document_shared(pl_build *b, sh_package_baseline_reader original,
    void *original_context, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    size_t count = c->inputs.count;
    sh_package_link_node *nodes = calloc(count ? count : 1, sizeof(*nodes));
    unsigned char *local = calloc(count ? count : 1, 1), *retained = calloc(count ? count : 1, 1);
    int ok = 0;
    if (!nodes || !local || !retained) goto done;
    for (size_t i = 0; i < count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        int shared = c->shared[i] && pl_shared_declaration(unit->family);
        local[i] = unit->original_binding || shared;
        if (!local[i]) continue;
        b->document.units++; b->document.shared += shared != 0;
        pl_original_walk walk = {c};
        /* A unit the contributions did not compose was bound by discovery with the same reader and scoped
         * resolution pl_bound_bind would use, and every reference of its files is a site the capture froze
         * (pl_bound_sites): those sites are its dependency edges, without reading and binding the body again.
         * A composed unit, or a file the capture did not record, binds as before. */
        int stable = pl_bound_stable(c, i) && c->origins[i] == i;
        for (size_t f = 0; f < unit->file_count; f++) {
            const sh_compiled_resource *file = unit->files[f];
            const sh_package_link_edge *sites = NULL;
            size_t length = 0, bound_length = 0, site_count = 0;
            int from_sites = stable && sh_package_source_link_sites(c->source, c->origins[i], file, &sites, &site_count);
            unsigned char *body = NULL;
            unsigned long long mark = pl_now_ms();
            if (shared || !from_sites) {
                body = sh_package_compilation_read(unit->compilation, file, SIZE_MAX, &length, error, capacity);
                b->document.bodies++; b->document.read_ms += pl_now_ms() - mark; mark = pl_now_ms();
                if (!body) { free(walk.edges); goto done; }
            }
            if (shared) {
                unsigned char *base = NULL; size_t base_length = 0;
                int status = original ? original(original_context, file->engine_path, &base, &base_length) : 0;
                if (status < 0) { free(base); free(body); free(walk.edges); goto done; }
                if ((status != 1 && status != 2) || !base || length != base_length || memcmp(body, base, length))
                    local[i] = 0;
                b->document.original_bytes += base_length; b->document.original_ms += pl_now_ms() - mark; mark = pl_now_ms();
                free(base);
                if (!local[i]) { free(body); break; }
            }
            if (from_sites) {
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
                /* Tests: the frozen sites must reach exactly the targets a fresh bind of the body would. */
                if (g_pl_document_oracle && !pl_document_sites_checked(c, i, file, body, length, sites, site_count, error, capacity)) {
                    free(body); free(walk.edges); goto done;
                }
#endif
                free(body);
                b->document.sites++;
                for (size_t k = 0; k < site_count; k++)
                    if (!pl_original_edge(&walk, sites[k].target)) { free(walk.edges); goto done; }
                continue;
            }
            unsigned char *bound = pl_bound_bind(c, i, file, body, length,
                pl_original_reference, &walk, &bound_length, error, capacity);
            free(body);
            b->document.binds++; b->document.bind_ms += pl_now_ms() - mark;
            if (!bound) { free(walk.edges); goto done; }
            free(bound);
        }
        for (size_t j = 0; j < unit->implicit_count; j++)
            if (!pl_original_edge(&walk, unit->implicit[j].target)) { free(walk.edges); goto done; }
        /* Every conditional binding: one to a retained unit changes nothing. */
        for (size_t j = 0; j < unit->conditional_count; j++)
            if (!pl_original_edge(&walk, unit->conditional[j].target)) { free(walk.edges); goto done; }
        nodes[i].edges = walk.edges; nodes[i].edge_count = walk.count;
    }
    if (!sh_package_link_originals(nodes, count, local, retained, error, capacity)) goto done;
    if (!b->stock_reasons) {
        b->stock_reasons = calloc(count ? count : 1, sizeof(*b->stock_reasons));
        b->stock_reason_count = b->stock_reasons ? count : 0;
    }
    for (size_t i = 0; b->stock_reasons && i < count && i < b->stock_reason_count; i++) {
        if (!c->shared[i] || retained[i]) continue;
        if (!local[i]) { b->stock_reasons[i] = "its source differs from the original"; continue; }
        b->stock_reasons[i] = "it reaches a unit that keeps no original name";
        for (size_t e = 0; e < nodes[i].edge_count; e++) {
            size_t t = nodes[i].edges[e].target;
            if (t < count && !retained[t]) { b->stock_reasons[i] = local[t] ?
                "it reaches a unit that keeps no original name" : "it reaches a changed unit"; break; }
        }
    }
    for (size_t i = 0; i < count; i++) c->shared[i] &= retained[i];
    ok = 1;
done:
    if (nodes) for (size_t i = 0; i < count; i++) free((void *)nodes[i].edges);
    free(nodes); free(local); free(retained); return ok;
}
/* Contribution headers stay lexical; their captured scope is unchanged. The
 * entity reader resolves its inherited native class before walking fields.
 * An inspector or editor definition's sparse inherited list indexes its parent
 * declaration's list in whichever bound variant a scope holds, so composition
 * compares parents by name; a parent differing only in other fields (a
 * placement flag) is not a different layout. Restore re-resolves each scope. */
static int pl_lexical_inherit(const pl_contributions *c, size_t unit, const char *slot)
{ return !strcmp(slot, "inherit") && sh_package_editor_contribution_phase(c->inputs.units[unit].family); }
static int pl_token_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    size_t target;
    const char *key = pl_walk_slot(walk, slot);
    if (!key) { snprintf(walk->error, walk->capacity, "editor contribution allocation failed"); return -1; }
    if (pl_default_slot(walk->graph, walk->unit, key, family, name)) return 0;
    if (!pl_bound_target(walk->graph, walk->unit, key, family, name, &target, walk->error, walk->capacity)) return -1;
    if (pl_lexical_inherit(walk->graph, walk->unit, slot)) return 0;
    if (walk->kept && !pl_kept_add(walk->kept, key, walk->graph->classes.nodes[target], target)) {
        snprintf(walk->error, walk->capacity, "editor contribution allocation failed"); return -1;
    }
    *replacement = walk->graph->tokens[walk->graph->classes.nodes[target]]; return 1;
}
static int pl_capture_edge(pl_contribution_unit *capture, const char *slot, size_t target)
{
    for (size_t i = 0; i < capture->count; i++) if (!strcmp(capture->edges[i].slot, slot)) return 0;
    if (capture->count == capture->capacity) {
        size_t next = capture->capacity ? capture->capacity * 2 : 16;
        pl_contribution_edge *grown = next > capture->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(capture->edges, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        capture->edges = grown; capture->capacity = next;
    }
    char *key = _strdup(slot); if (!key) return 0;
    capture->edges[capture->count++] = (pl_contribution_edge){key, target}; return 1;
}
static int pl_restore_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    pl_contributions *c = walk->graph;
    pl_contribution_unit *capture = walk->capture;
    const char *key = pl_walk_slot(walk, slot);
    if (!key) { snprintf(walk->error, walk->capacity, "editor contribution allocation failed"); return -1; }
    size_t token = pl_token_find(c, name);
    size_t target = token != SIZE_MAX ? c->classes.representatives[token] : SIZE_MAX;
    /* A single-unit rewrite keeps a reference it did not change bound to the unit its own scope bound,
     * not to the class representative (pl_kept_refs). Composition never supplies a record. */
    size_t exact = token != SIZE_MAX && walk->kept ? pl_kept_find(walk->kept, key, token) : SIZE_MAX;
    if (exact != SIZE_MAX) target = exact;
    if (target == SIZE_MAX) {
        /* Tokenizing a member (pl_contribution_group) captures every reference
         * its own scope supplies; one its scope lacks stays as authored for the
         * engine's default by name (sg_bind_typed), as stock text does. Such an
         * entry in a shared composition reaches every member's scope. Where
         * this scope supplies the name the engine reads it as that declaration,
         * so it binds as this scope binds the name; where it does not, it stays
         * a default here too (pl_default_slot), whatever another slot spelled
         * the same way captured. An inherited header stays lexical; a clone
         * keeps its original binding. */
        if (pl_lexical_inherit(c, walk->unit, slot) ||
            (c->origins[walk->unit] != walk->unit && !c->units[walk->unit].owned)) {
            if (!pl_bound_target(c, walk->unit, key, family, name, &target, walk->error, walk->capacity)) {
                snprintf(walk->error, walk->capacity, "editor contribution contains an uncaptured reference: %s", key); return -1;
            }
        } else {
            char ignored[256];
            if (!sh_package_source_link_resolve_unit(c->source, c->origins[walk->unit], family, name,
                    &target, ignored, sizeof(ignored))) return 0;
        }
    }
    /* A compiler clone (spawn registry or classifier copy) keeps its original binding: a reference
     * it captures through a class token goes to that class's member in the clone's own scope when it
     * has one, not to whichever scope's unit represents the class (R-150). */
    if (exact == SIZE_MAX && token != SIZE_MAX && c->origins[walk->unit] != walk->unit) {
        size_t local = SIZE_MAX;
        char ignored[256];
        const sh_package_link_unit *representative = c->inputs.units + target;
        if (sh_package_source_link_resolve_unit(c->source, c->origins[walk->unit], representative->family,
                representative->name, &local, ignored, sizeof(ignored)) &&
            local < c->classes.node_count && c->classes.nodes[local] == token) target = local;
    }
    const sh_package_link_unit *value = c->inputs.units + target;
    if (strcmp(value->family, family)) return -1;
    if (!pl_capture_edge(capture, key, target)) return -1;
    if (!strcmp(name, value->name)) return 0;
    *replacement = value->name; return 1;
}

static int pl_modern(const pl_build *b, size_t scope)
{
    return scope >= b->authored_count || !b->contexts ||
        b->contexts->contexts[scope].mode != SH_PACKAGE_CONTEXT_COMPOSED;
}

typedef struct pl_string_site {
    char *slot;
    sh_package_string_request request;
    size_t unit;
    int late;
} pl_string_site;
typedef struct pl_localization {
    pl_build *build;
    pl_string_site *sites;
    size_t count, capacity, unit, component;
    int late;
    sh_package_strings *strings;
    char *error;
    size_t error_capacity;
} pl_localization;

static int pl_string_bind(void *context, const char *slot, const char *key,
    size_t length, const char **replacement)
{
    pl_localization *p = context;
    if (memchr(key, 0, length)) return 0; /* No authored JSON key can match it. */
    if (p->strings) {
        for (size_t i = 0; i < p->count; i++) if (p->sites[i].unit == p->unit &&
            !strcmp(p->sites[i].slot, slot) && strlen(p->sites[i].request.key) == length &&
            !memcmp(p->sites[i].request.key, key, length)) {
            const char *bound = sh_package_strings_key(p->strings, i);
            if (!bound) return -1;
            if (strlen(bound) == length && !memcmp(bound, key, length)) return 0;
            *replacement = bound; return 1;
        }
        return 0; /* An explicit product-only field has no authored source site. */
    }
    if (length == SIZE_MAX) return -1;
    for (size_t i = 0; i < p->count; i++)
        if (p->sites[i].unit == p->unit && !strcmp(p->sites[i].slot, slot)) return -1;
    if (p->count == p->capacity) {
        size_t next = p->capacity ? p->capacity * 2 : 32;
        pl_string_site *grown = next > p->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(p->sites, next * sizeof(*grown)) : NULL;
        if (!grown) return -1;
        p->sites = grown; p->capacity = next;
    }
    char *saved = malloc(length + 1), *path = _strdup(slot);
    if (!saved || !path) { free(saved); free(path); return -1; }
    memcpy(saved, key, length); saved[length] = 0;
    p->sites[p->count++] = (pl_string_site){path, {p->component, saved}, p->unit, p->late}; return 0;
}

/* Enumerate descriptor keys, not movie literals. Translation selection stays
 * entirely in package_strings (per-locale nearest-ancestor precedence). */
static int pl_movie_string_requests(pl_localization *p, const sh_package_sources *sources, size_t unit)
{
    pl_build *b = p->build; pl_contributions *c = b->contributions;
    size_t scope = c->inputs.scopes[unit];
    if (scope >= b->authored_count || !pl_modern(b, scope)) return 1;
    size_t component = b->views[scope].component;
    if (component >= sources->component_count) return 0;
    const sh_package_component *child = sources->components + component;
    p->unit = unit; p->component = component; p->late = 1;
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *parent = sources->components + i;
        size_t depth = strlen(parent->relative);
        if (parent->owner != child->owner || (depth && (strncmp(parent->relative, child->relative, depth) ||
            (child->relative[depth] && child->relative[depth] != '/')))) continue;
        const char *raw = sh_package_descriptor_section(&parent->descriptor, "strings");
        if (!raw) continue;
        sh_json_object locales = {0};
        if (!sh_json_parse_object(raw, strlen(raw), 24, &locales)) return 0;
        int ok = 1;
        for (size_t j = 0; ok && j < locales.count; j++) {
            sh_json_object pairs = {0};
            const char *map = locales.members[j].value_json;
            ok = sh_json_parse_object(map, strlen(map), 24, &pairs);
            for (size_t k = 0; ok && k < pairs.count; k++) {
                const sh_json_member *value = pairs.members + k;
                size_t n = value->key_length;
                char *key = n <= SIZE_MAX - 6 ? malloc(n + 6) : NULL;
                if (!key || strlen(value->key) != n) { free(key); ok = 0; break; }
                memcpy(key, "#str_", 5); memcpy(key + 5, value->key, n + 1);
                char *slot = sh_package_movie_string_slot(key); const char *unused = NULL;
                if (!slot) { free(key); ok = 0; break; }
                size_t seen;
                for (seen = 0; seen < p->count; seen++)
                    if (p->sites[seen].unit == unit && !strcmp(p->sites[seen].slot, slot)) break;
                if (seen == p->count) ok = pl_string_bind(p, slot, key, strlen(key), &unused) == 0;
                free(slot); free(key);
            }
            sh_json_object_free(&pairs);
        }
        sh_json_object_free(&locales); if (!ok) return 0;
    }
    p->late = 0; return 1;
}
static int pl_movie_string_capture(pl_contributions *c, const pl_string_site *site,
    const sh_package_localized_string *mapping)
{
    sh_package_link_unit *unit = c->inputs.units + site->unit;
    if (c->movie_string_count >= SIZE_MAX / sizeof(*c->movie_strings) ||
        unit->witness_count >= SIZE_MAX / sizeof(*unit->witnesses) || !pl_unit_keep(c, site->unit)) return 0;
    pl_movie_string *value = calloc(1, sizeof(*value));
    sh_package_link_witness *witnesses = calloc(unit->witness_count + 1, sizeof(*witnesses));
    if (!value || !witnesses) { free(value); free(witnesses); return 0; }
    value->unit = site->unit; value->slot = _strdup(site->slot); value->key = mapping->key;
    value->witness.engine_path = value->slot;
    /* pl_close releases contributions before b.shared. Borrow its one owned
     * canonical mapping/key instead of copying locale payload per movie. */
    value->witness.body = (unsigned char *)mapping->mapping; value->witness.body_length = strlen(mapping->mapping);
    if (!value->slot) goto failed;
    pl_movie_string **grown = realloc(c->movie_strings, (c->movie_string_count + 1) * sizeof(*grown));
    if (!grown) goto failed;
    c->movie_strings = grown; c->movie_strings[c->movie_string_count++] = value;
    if (unit->witness_count) memcpy(witnesses, unit->witnesses, unit->witness_count * sizeof(*witnesses));
    witnesses[unit->witness_count++] = (sh_package_link_witness){value->slot, &value->witness};
    free(c->witnesses[site->unit]); c->witnesses[site->unit] = witnesses; unit->witnesses = witnesses;
    unit->original_binding = 0; c->shared[site->unit] = 0;
    return 1;
failed:
    free(value->slot); free(value); free(witnesses); return 0;
}

/* Admit the policy's historical/global string keys into this compilation's
 * native reservation once, before any private key is allocated. */
static int pl_reserve_string_globals(pl_build *b)
{
    const sh_package_string_reservations *native = &b->native->strings;
    if (b->string_globals_reserved) return 1;
    const sh_json_object *strings = &b->shared->policy.strings;
    if (strings->count && !native->reserve_global) return 0;
    for (size_t i = 0; i < strings->count; i++) {
        const char *raw = strings->members[i].value_json; sh_json_object pairs = {0};
        if (!sh_json_parse_object(raw, strlen(raw), 2, &pairs)) return 0;
        int ok = 1;
        for (size_t j = 0; ok && j < pairs.count; j++) {
            const char *id = pairs.members[j].key; size_t length = strlen(id);
            char *global = length <= SIZE_MAX - 6 ? malloc(length + 6) : NULL;
            if (global) {
                memcpy(global, "#str_", 5); memcpy(global + 5, id, length + 1);
                ok = native->reserve_global(native->context, global) == 1;
            } else ok = 0;
            free(global);
        }
        sh_json_object_free(&pairs); if (!ok) return 0;
    }
    b->string_globals_reserved = 1;
    return 1;
}
static int pl_string_reserve(void *context, const char *key, const unsigned char identity[32], const char *mapping)
{
    pl_localization *p = context;
    const sh_package_string_reservations *native = &p->build->native->strings;
    if (!pl_reserve_string_globals(p->build)) return -1;
    int result = native->reserve(native->context, key, identity, mapping);
    if (result == 1 && !sh_package_policy_localization(&p->build->shared->policy, key, mapping,
        NULL, p->error, p->error_capacity)) return -1;
    return result;
}

static int pl_string_capture_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context; size_t target;
    const char *key = pl_walk_slot(walk, slot);
    (void)replacement;
    if (!key) { snprintf(walk->error, walk->capacity, "editor contribution allocation failed"); return -1; }
    if (pl_default_slot(walk->graph, walk->unit, key, family, name)) return 0;
    return pl_bound_target(walk->graph, walk->unit, key, family, name, &target,
        walk->error, walk->capacity) && pl_capture_edge(walk->capture, key, target) ? 0 : -1;
}

static int pl_component_strings(const sh_package_sources *sources, size_t component)
{
    const sh_package_component *child = sources->components + component;
    for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *parent = sources->components + i;
        size_t length = strlen(parent->relative);
        if (parent->owner == child->owner && (!length ||
            (!strncmp(parent->relative, child->relative, length) &&
             (!child->relative[length] || child->relative[length] == '/'))) &&
            sh_package_descriptor_section(&parent->descriptor, "strings")) return 1;
    }
    return 0;
}

/* Bind explicit authored typed sites before field union. Walk the authored
 * source for attribution, then replay only its exact slots/values in the
 * compiled body; inherited and product-only fields keep their own contexts. */
static int pl_localize(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    pl_localization p = {0}; sh_package_string_request *requests = NULL;
    pl_contributions *c = b->contributions;
    p.build = b; p.error = error; p.error_capacity = capacity;
    int ok = 0;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        if (sh_package_movie_owner(unit->family, unit->name) && !pl_movie_string_requests(&p, sources, i)) goto done;
        if (!pl_modern(b, c->inputs.scopes[i]) || unit->file_count != 1) continue;
        const sh_compiled_resource *file = unit->files[0];
        if (!file->type || !file->source_count) continue;
        /* These registered custom grammars contain resource/path operands,
         * not reflected idStrId leaves. Binary/catalog leaves likewise have
         * no declaration type. Other reflected families use the typed pass. */
        if (!strcmp(unit->family, "renderprog") || sh_decl_native_binding_family(unit->family, NULL)) continue;
        if (file->sources[0] >= sources->file_count) goto done;
        const sh_package_source_file *authored = sources->files + file->sources[0];
        if (authored->component >= sources->component_count) goto done;
        if (!pl_component_strings(sources, authored->component)) continue;
        /* Isolated scopes select exactly one nearest authored physical file. */
        if (file->source_count != 1) {
            snprintf(error, capacity, "localized declaration has multiple uncaptured source contributions: %s", file->engine_path);
            goto done;
        }
        size_t length = 0, bound_length = 0;
        unsigned char *raw = sh_package_source_read(authored, SIZE_MAX, &length, error, capacity);
        if (!raw) goto done;
        p.unit = i; p.component = authored->component; p.late = 0;
        unsigned char *observed = sh_package_source_link_bind_strings(b->reader, i, raw, length,
            pl_string_bind, &p, &bound_length, error, capacity);
        free(raw); if (!observed) goto done; free(observed);
    }
    if (!p.count) { ok = 1; goto done; }
    requests = calloc(p.count, sizeof(*requests)); if (!requests) goto done;
    for (size_t i = 0; i < p.count; i++) requests[i] = p.sites[i].request;
    sh_package_string_reservations reservations = {0};
    if (b->native && b->native->strings.reserve) {
        reservations = b->native->strings; reservations.reserve = pl_string_reserve; reservations.context = &p;
    }
    p.strings = sh_package_strings_compile(sources, requests, p.count, &reservations, error, capacity);
    if (!p.strings) goto done;
    for (size_t i = 0; i < p.count; i++) if (sh_package_strings_identity(p.strings, i)) {
        const char *key = sh_package_strings_key(p.strings, i); size_t j;
        for (j = 0; j < b->shared->policy.localized_count; j++) {
            sh_package_localized_string *value = b->shared->policy.localized + j;
            if (!strcmp(value->key, key)) {
                if (!sh_package_owners_add(&value->owners, sources->components[p.sites[i].request.component].owner)) goto done;
                if (p.sites[i].late && !pl_movie_string_capture(c, p.sites + i, value)) goto done;
                break;
            }
        }
        if (j == b->shared->policy.localized_count) goto done;
    }
    for (size_t i = 0; i < c->inputs.count; i++) {
        size_t site;
        for (site = 0; site < p.count; site++) if (p.sites[site].unit == i && !p.sites[site].late) break;
        if (site == p.count) continue;
        sh_package_link_unit *unit = c->inputs.units + i;
        const sh_compiled_resource *file = unit->files[0];
        size_t length = 0, bound_length = 0, inspected = 0;
        unsigned char *raw = sh_package_compilation_read(unit->compilation, file, SIZE_MAX, &length, error, capacity);
        if (!raw) goto done;
        p.unit = i;
        unsigned char *bound = sh_package_source_link_bind_strings(b->reader, i, raw, length,
            pl_string_bind, &p, &bound_length, error, capacity);
        if (!bound) { free(raw); goto done; }
        if (length == bound_length && !memcmp(raw, bound, length)) { free(raw); free(bound); continue; }
        free(raw);
        pl_contribution_unit pending = {0};
        pl_contribution_walk walk = {c, i, NULL, NULL, &pending, error, capacity};
        unsigned char *checked = sh_package_source_link_bind_unit(b->reader, i, bound, bound_length,
            pl_string_capture_reference, &walk, &inspected, error, capacity);
        pl_walk_close(&walk);
        if (!checked) { free(bound); pl_contribution_unit_free(&pending); goto done; }
        free(checked);
        sh_package_body *shared = pl_stage_body(c, &bound, bound_length);
        sh_package_resource_projection projection = {file->engine_path, &file, 1,
            shared ? sh_package_body_bytes(shared) : bound, bound_length};
        projection.body_owner = shared;
        pending.owned = sh_package_compilation_project(sources, &projection, 1, NULL,
            b->shared->canonical_path, error, capacity);
        sh_package_body_release(shared); free(bound);
        if (!pending.owned) { pl_contribution_unit_free(&pending); goto done; }
        if (!pl_unit_keep(c, i)) { pl_contribution_unit_free(&pending); snprintf(error, capacity, "%s", "cannot keep a captured unit's record"); goto done; }
        pending.file = pending.owned->resources; c->units[i] = pending;
        unit->compilation = pending.owned; unit->files = &c->units[i].file; unit->original_binding = 0;
    }
    ok = 1;
done:
    for (size_t i = 0; i < p.count; i++) { free(p.sites[i].slot); free((char *)p.sites[i].request.key); }
    free(p.sites); free(requests); sh_package_strings_free(p.strings);
    if (!ok && !*error) snprintf(error, capacity, "cannot bind source-scoped localization");
    return ok;
}

static int pl_edges_same(pl_contributions *c, size_t unit, const pl_contribution_unit *capture)
{
    if (c->units[unit].owned && c->units[unit].count != capture->count) return 0;
    for (size_t i = 0; i < capture->count; i++) {
        const pl_contribution_edge *edge = capture->edges + i;
        const sh_package_link_unit *value = c->inputs.units + edge->target;
        size_t prior; char ignored[256];
        if (!pl_bound_target(c, unit, edge->slot, value->family, value->name, &prior, ignored, sizeof(ignored)) ||
            c->classes.nodes[prior] != c->classes.nodes[edge->target]) return 0;
    }
    return 1;
}

/* The package supplying a member's own authored bytes; SIZE_MAX for stock,
 * product content and byte-identical copies of an installed original. */
static size_t pl_scope_owner(const pl_build *b, const sh_package_sources *sources, size_t scope);
static size_t pl_member_owner(const pl_build *b, const sh_package_sources *sources, size_t unit)
{
    const sh_package_link_unit *value = sh_package_source_link_unit(b->contributions->source, unit);
    size_t scope = b->contributions->inputs.scopes[unit];
    int authored = 0;
    if (!value) return SIZE_MAX;
    for (size_t f = 0; f < value->file_count; f++)
        if (value->files[f]->source_count && !value->files[f]->native_original) authored = 1;
    return authored ? pl_scope_owner(b, sources, scope) : SIZE_MAX;
}

/* A package whose own contribution failed, with the reason (error): invalid
 * when it is malformed in its own context, otherwise content this build cannot
 * serve yet. Its scopes are withdrawn from later judgements, and its first
 * finding stands. 1 when the compile goes on to find others, 0 when this
 * finding ends it. */
static int pl_contribution_blame(pl_build *b, const sh_package_sources *sources, size_t owner, int invalid,
    char *error, size_t capacity)
{
    sh_package_rejections *found = &b->contribution_rejections;
    b->contribution_verdict = 1;
    for (size_t i = 0; i < found->count; i++) if (found->items[i].package == owner) return b->collect_contributions;
    if (found->count == found->capacity) {
        size_t grown_capacity = found->capacity ? found->capacity * 2 : 4;
        sh_package_rejection *grown = realloc(found->items, grown_capacity * sizeof(*grown));
        if (!grown) {
            snprintf(error, capacity, "cannot account further rejected packages");
            b->contribution_verdict = 0; return 0;
        }
        found->items = grown; found->capacity = grown_capacity;
    }
    sh_package_rejection *r = found->items + found->count++;
    memset(r, 0, sizeof(*r));
    r->package = owner; r->invalid = invalid;
    snprintf(r->reason, sizeof(r->reason), "%s", error);
    for (size_t scope = 0; b->withdrawn && scope < b->authored_count; scope++)
        if (pl_scope_owner(b, sources, scope) == owner) b->withdrawn[scope] = 1;
    return b->collect_contributions;
}

/* The end of a stage that found packages: they fail this compile together,
 * reported with the first finding's reason. 1 when none was found. */
static int pl_contribution_found(pl_build *b, char *error, size_t capacity)
{
    if (!b->contribution_rejections.count) return 1;
    snprintf(error, capacity, "%s", b->contribution_rejections.items[0].reason);
    b->contribution_verdict = 1; return 0;
}

/* A group the product cannot compose names its declaration. A package's own
 * invalid source rejects that package. A gap in one member's own content
 * leaves its package unserved for this candidate. A gap only in combining
 * individually valid contributions is product coverage, not evidence against
 * any owner: its participants are named (pl_joint_participants) and nobody is
 * excluded, so the candidate fails and the previous usable generation stays.
 * Stock or product content alone blames nobody, and environment failures never
 * judge content. 1 when the owner was found and the compile goes on finding
 * others (pl_contribution_blame), 0 when this failure ends it. */
static int pl_contribution_failure(pl_build *b, const sh_package_sources *sources,
    const sh_package_link_unit *input, int kind, size_t culprit, char *error, size_t capacity)
{
    char reason[768];
    size_t owner;
    snprintf(reason, sizeof(reason), "%s", error);
    snprintf(error, capacity, "%s %s: %s", input->family, input->name, reason);
    if (kind == SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT) { b->contribution_verdict = 0; return 0; }
    if (culprit == SIZE_MAX || (owner = pl_member_owner(b, sources, culprit)) == SIZE_MAX) {
        b->contribution_verdict = 1; return 0;
    }
    return pl_contribution_blame(b, sources, owner, kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE, error, capacity);
}

/* How a group member was judged: composed (its content entered its group's
 * output), excluded (its package was found before the group composed),
 * withheld (its group was left for the next compile). */
enum { PL_UNIT_OPEN, PL_UNIT_COMPOSED, PL_UNIT_EXCLUDED, PL_UNIT_WITHHELD };
typedef struct pl_group { size_t first, count, parent; } pl_group;
typedef int (*pl_group_compose)(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, char *error, size_t capacity);
static void pl_units_mark(pl_build *b, const size_t *units, size_t count, unsigned char state)
{
    for (size_t k = 0; k < count; k++) if (units[k] < b->unit_state_count) b->unit_state[units[k]] = state;
}
/* Whether these units' current content depends on a found package: a group
 * that was left aside, or (composed_taints) a composition a package found
 * since took part in. */
static int pl_units_aside(const pl_build *b, const size_t *units, size_t count, int composed_taints)
{
    if (!b->contribution_rejections.count) return 0;
    for (size_t k = 0; k < count; k++) {
        unsigned char state = units[k] < b->unit_state_count ? b->unit_state[units[k]] : PL_UNIT_OPEN;
        if (state == PL_UNIT_WITHHELD || (composed_taints && state == PL_UNIT_COMPOSED &&
            b->withdrawn && b->withdrawn[b->contributions->inputs.scopes[units[k]]])) return 1;
    }
    return 0;
}
/* A group, or a parent it rebases onto, holding such content. */
static int pl_group_aside(const pl_build *b, const pl_group *groups, size_t group_count, size_t g, const size_t *order)
{
    for (size_t at = g, steps = 0; at != SIZE_MAX && steps <= group_count; at = groups[at].parent, steps++)
        if (pl_units_aside(b, order + groups[at].first, groups[at].count, 1)) return 1;
    return 0;
}
static size_t pl_scope_owner(const pl_build *b, const sh_package_sources *sources, size_t scope);
/* One unit's identity provenance base (R-397), grown on demand; NULL on allocation failure. */
static sh_package_owners *pl_unit_prov(pl_build *b, size_t unit)
{
    if (unit >= b->unit_prov_count) {
        size_t count = unit + 1 > b->unit_prov_count * 2 ? unit + 1 : b->unit_prov_count * 2;
        sh_package_owners *grown = realloc(b->unit_prov, count * sizeof(*grown));
        if (!grown) return NULL;
        memset(grown + b->unit_prov_count, 0, (count - b->unit_prov_count) * sizeof(*grown));
        b->unit_prov = grown; b->unit_prov_count = count;
    }
    return b->unit_prov + unit;
}
/* A composed group: each member's content is built from every member, so each carries every member's package. */
static int pl_group_prov(pl_build *b, const sh_package_sources *sources, const size_t *units, size_t count)
{
    sh_package_owners members = {0};
    int ok = 1;
    (void)sources;
    for (size_t k = 0; ok && k < count; k++) {
        /* A member contributes the packages that own its files: a member every scope copies alike (the
         * product's change to a stock row) names none. What a member's file reaches enters through the
         * composed row's identity edges. */
        const sh_package_link_unit *member = b->contributions->inputs.units + units[k];
        for (size_t f = 0; ok && f < member->file_count; f++)
            if (member->files[f]) ok = sh_package_owners_union(&members, &member->files[f]->owners) &&
                sh_package_owners_union(&members, &member->files[f]->gameplay_owners);
    }
    for (size_t k = 0; ok && sh_package_owners_count(&members) && k < count; k++) {
        sh_package_owners *set = pl_unit_prov(b, units[k]);
        ok = set && sh_package_owners_union(set, &members);
    }
    sh_package_owners_free(&members); return ok;
}
/* Judge one editor (groups) or registration group apart from every package
 * found so far. Members of a found package are left out before composing, and
 * a composition that finds a package is judged again without it, so several
 * packages' independent defects in one group are all found while its valid
 * members stay unblamed. A group whose inputs another found package's content
 * shaped is left aside for the next compile instead of being judged here: an
 * editor group rebasing onto a parent composed with that content, or one of
 * whose members a left-aside group holds. Registration lists read no editor
 * field a phase-2 composition wrote, so a registration group is left aside
 * only for a left-aside member. 1 composed or left aside, 0 failed. */
static int pl_group_judge(pl_build *b, const sh_package_sources *sources, const pl_group *groups,
    size_t group_count, size_t g, const size_t *order, const size_t *units, size_t count,
    pl_group_compose compose, char *error, size_t capacity)
{
    const pl_contributions *c = b->contributions;
    size_t *kept = malloc((count ? count : 1) * sizeof(*kept));
    int result = 1;
    if (!kept) {
        snprintf(error, capacity, "editor contribution allocation failed");
        b->contribution_verdict = 0; return 0;
    }
    for (;;) {
        size_t n = 0, found = b->contribution_rejections.count;
        if (groups ? pl_group_aside(b, groups, group_count, g, order) : pl_units_aside(b, units, count, 0)) {
            pl_units_mark(b, units, count, PL_UNIT_WITHHELD); b->contribution_verdict = 0; break;
        }
        for (size_t k = 0; k < count; k++) {
            if (b->withdrawn && b->withdrawn[c->inputs.scopes[units[k]]]) pl_units_mark(b, units + k, 1, PL_UNIT_EXCLUDED);
            else kept[n++] = units[k];
        }
        if (!n) break;
        int composed = compose(b, sources, kept, n, error, capacity);
        if (composed > 0 && !pl_group_prov(b, sources, kept, n)) {
            snprintf(error, capacity, "editor contribution allocation failed"); result = 0; break;
        }
        if (composed > 0) { pl_units_mark(b, kept, n, PL_UNIT_COMPOSED); break; }
        if (!composed) { result = 0; break; }
        /* A package found: the others are judged afresh without it. */
        b->contribution_verdict = 0;
        if (b->contribution_rejections.count == found) { pl_units_mark(b, units, count, PL_UNIT_WITHHELD); break; }
#ifdef SH_PACKAGE_LIBRARY_TESTING
        if (g_rejudge_failures > 0 && g_rejudge_failures--) {
            snprintf(error, capacity, "editor contribution allocation failed");
            result = 0; break;
        }
#endif
    }
    free(kept); return result;
}

/* Whether contributions compose: 1 yes, 0 no, -1 environment. selected[k] is
 * parts[k]'s index among the group's contributions. */
typedef int (*pl_compose_probe)(void *context, const sh_decl_source *parts, const size_t *selected, size_t count);
static void pl_joint_participants(const pl_build *b, const sh_package_sources *sources,
    const size_t *members, const size_t *part_member, const sh_decl_source *parts, size_t part_count,
    pl_compose_probe probe, void *context, char *error, size_t capacity);
/* A registration probe composes with the same resolved states as the group
 * (states: base, each of part_count contributions, target), so a diagnostic
 * subset never applies another state rule than the composition it explains. */
typedef struct pl_collection_probe {
    const char *family;
    int registration, private_binding;
    sh_decl_source base, target;
    const sh_decl_collection_order *order;
    const char *const *states;
    size_t part_count;
} pl_collection_probe;
#ifdef SH_PACKAGE_LIBRARY_TESTING
static long g_probe_failures;
void sh_package_library_test_probe_failures(long count) { g_probe_failures = count; }
static int g_editor_memo = 1;
void sh_package_library_test_editor_memo(int enabled) { g_editor_memo = enabled != 0; }
static int g_key_memo = 1;
void sh_package_library_test_key_memo(int enabled) { g_key_memo = enabled != 0; }
#endif
static int pl_collection_composes(void *context, const sh_decl_source *parts, const size_t *selected, size_t count)
{
    const pl_collection_probe *p = context;
    sh_package_editor_failure failure;
    char error[512];
    size_t length = 0;
    char *body = NULL;
    const char **states = NULL;
    if (p->registration && p->states) {
        if (count > SIZE_MAX / sizeof(*states) - 2 || !(states = calloc(count + 2, sizeof(*states)))) return -1;
        states[0] = p->states[0]; states[count + 1] = p->states[p->part_count + 1];
        for (size_t k = 0; k < count; k++) {
            if (selected[k] >= p->part_count) { free((void *)states); return -1; }
            states[k + 1] = p->states[selected[k] + 1];
        }
    }
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if (g_probe_failures > 0 && g_probe_failures--) failure.kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT;
    else
#endif
    body = p->registration ?
        sh_package_registration_contributions_states(p->base, parts, count, p->target, p->order, states, &length,
            &failure, error, sizeof(error)) :
        sh_package_editor_contributions_report(p->family, p->base, parts, count, p->target, p->private_binding,
            &length, &failure, error, sizeof(error));
    free((void *)states);
    if (body) { free(body); return 1; }
    return failure.kind == SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT ? -1 : 0;
}

/* 1 with the inherit literal of a contribution text (NULL when absent). A
 * shipped row may repeat a field (fx/ambient_embers states fxInfo twice), which
 * the native reader applies in order, so the text is read in order. Only a
 * leading inherit is a parent: entityDef headers are positional
 * (sh_decl_entity_split) and the generic typeinfo Parse reads an optional
 * leading inherit, so a repeated or later inherit is never one. */
static int pl_inherit_name(sh_decl_source text, char **out)
{
    sh_decl_node *tree = sh_decl_tree_parse_ordered(text, NULL, 0);
    sh_decl_entity_headers headers;
    const sh_decl_node *inherit;
    const char *value; size_t length;
    *out = NULL;
    if (!tree) return 0;
    sh_decl_entity_split(tree, &headers);
    inherit = headers.inherit;
    if (inherit && sh_decl_tree_literal(inherit, &value, &length)) {
        if (!(*out = malloc(length + 1))) { sh_decl_tree_free(tree); return 0; }
        memcpy(*out, value, length); (*out)[length] = 0;
    }
    sh_decl_tree_free(tree); return 1;
}
static char *pl_token_text_kept(pl_contributions *c, size_t unit, size_t *length, pl_kept_refs *kept,
    char *error, size_t capacity)
{
    const sh_package_link_unit *input = c->inputs.units + unit;
    size_t size = 0;
    unsigned char *body = sh_package_compilation_read(input->compilation, input->files[0], SIZE_MAX, &size, error, capacity);
    if (!body) return NULL;
    pl_contribution_walk walk = {c, unit, NULL, NULL, NULL, error, capacity, kept};
    unsigned char *text = sh_package_source_link_bind_derived(c->source, c->origins[unit], body, size,
        pl_token_reference, &walk, pl_derived_exists, &walk, length, error, capacity);
    pl_walk_close(&walk); free(body); return (char *)text;
}
static char *pl_token_text(pl_contributions *c, size_t unit, size_t *length, char *error, size_t capacity)
{ return pl_token_text_kept(c, unit, length, NULL, error, capacity); }
/* The unit's original text as its author wrote it, bound in its own scope. */
static int pl_original_token_reference(void *context, const char *slot, const char *family,
    const char *name, const char **replacement)
{
    pl_contribution_walk *walk = context;
    pl_contributions *c = walk->graph;
    size_t target;
    if (!sh_package_source_link_resolve_unit(c->source, walk->unit, family, name, &target, walk->error, walk->capacity)) return -1;
    if (pl_lexical_inherit(c, walk->unit, slot)) return 0;
    *replacement = c->tokens[c->classes.nodes[target]]; return 1;
}
static char *pl_original_text(pl_contributions *c, size_t unit, size_t *length, char *error, size_t capacity)
{
    const sh_package_link_unit *input = sh_package_source_link_unit(c->source, unit);
    size_t size = 0;
    if (!input) { snprintf(error, capacity, "contribution unit has no captured original"); return NULL; }
    unsigned char *body = sh_package_compilation_read(input->compilation, input->files[0], SIZE_MAX, &size, error, capacity);
    if (!body) return NULL;
    pl_contribution_walk walk = {c, unit, NULL, NULL, NULL, error, capacity};
    unsigned char *text = sh_package_source_link_bind_file(c->source, unit, input->files[0], body, size,
        pl_original_token_reference, &walk, length, error, capacity);
    free(body); return (char *)text;
}
typedef struct pl_chain { sh_decl_source *texts; size_t count; } pl_chain;
static void pl_chain_free(pl_chain *chain)
{
    for (size_t i = 0; chain->texts && i < chain->count; i++) free((char *)chain->texts[i].text);
    free(chain->texts); memset(chain, 0, sizeof(*chain));
}
/* Parent texts, nearest first: published follows the bound (possibly composed)
 * parents; otherwise the original parents the unit's author saw. */
static int pl_parent_chain(pl_contributions *c, size_t unit, sh_decl_source own, int published,
    pl_chain *out, int *kind, char *error, size_t capacity);
/* The inheritance a registration text's state follows over its bound parents
 * (sh_decl_entity_merges): "" without a stateful ancestor, else the root-most
 * stateful ancestor's first state statement's name. A first statement that is
 * not a block assignment is outside the merge model: remaining product
 * coverage (*kind COVERAGE), never a text-local substitute. Every registration
 * reader (projection, keys, capture, composition, probes) uses this state. */
static int pl_registration_state(pl_contributions *c, size_t unit, sh_decl_source text,
    char **state, int *kind, char *error, size_t capacity)
{
    pl_chain chain;
    int ok = 1, decided = 0;
    *state = NULL;
    if (!pl_parent_chain(c, unit, text, 1, &chain, kind, error, capacity)) return 0;
    for (size_t i = chain.count; ok && !decided && i-- > 0;) {
        sh_decl_node *tree = sh_decl_tree_parse_ordered(chain.texts[i], error, capacity);
        sh_decl_entity_headers headers;
        if (!tree) { ok = 0; break; }
        sh_decl_entity_split(tree, &headers);
        if (headers.state) {
            decided = 1;
            if (!headers.state->compound || !headers.state->assignment) {
                *kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE; ok = 0;
                snprintf(error, capacity, "inherited prepared state does not begin with a block assignment "
                    "(product coverage)");
            } else if (!(*state = _strdup(headers.state->key))) ok = 0;
        }
        sh_decl_tree_free(tree);
    }
    if (ok && !decided && !(*state = _strdup(""))) ok = 0;
    if (!ok && !*error) snprintf(error, capacity, "registration state allocation failed");
    pl_chain_free(&chain); return ok;
}
/* A rebased restatement is read natively over its published parents slot by
 * slot; where a restated control would keep a field of the slot's previous
 * occupant, it is respelled to cut the list first
 * (sh_package_editor_native_restatement). *text is replaced when respelled.
 * 1 done, 0 failed (*kind and error set). */
static int pl_native_restatement(const char *family, sh_decl_source *text, const pl_chain *published,
    int *kind, char *error, size_t capacity)
{
    char *exact = NULL; size_t exact_length = 0;
    int encoded = sh_package_editor_native_restatement(family, *text, published->texts, published->count,
        &exact, &exact_length);
    if (encoded == -1) { *kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT; snprintf(error, capacity, "editor contribution allocation failed"); return 0; }
    if (encoded < 0) {
        *kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
        snprintf(error, capacity, "a restated editor list could not be proven to read natively as it states");
        return 0;
    }
    if (encoded) { free((char *)text->text); *text = (sh_decl_source){exact, exact_length}; }
    return 1;
}
static int pl_parent_chain(pl_contributions *c, size_t unit, sh_decl_source own, int published,
    pl_chain *out, int *kind, char *error, size_t capacity)
{
    const char *family = c->inputs.units[unit].family;
    sh_decl_source text = own;
    size_t current = unit;
    memset(out, 0, sizeof(*out));
    *kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT;
    for (size_t depth = 0;; depth++) {
        char *parent = NULL; size_t target, length = 0;
        if (!pl_inherit_name(text, &parent)) { snprintf(error, capacity, "editor definition text cannot be read"); goto failed; }
        if (!parent) return 1;
        if (depth >= 32) {
            *kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
            snprintf(error, capacity, "inherit '%s': editor inheritance is circular or deeper than the native limit", parent);
            free(parent); goto failed;
        }
        int resolved = published ? pl_bound_target(c, current, "inherit", family, parent, &target, error, capacity) :
            sh_package_source_link_resolve_unit(c->source, current, family, parent, &target, error, capacity);
        free(parent);
        if (!resolved) { *kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE; goto failed; }
        char *next = published ? pl_token_text(c, target, &length, error, capacity) :
            pl_original_text(c, target, &length, error, capacity);
        if (!next) goto failed;
        sh_decl_source *grown = out->count < SIZE_MAX / sizeof(*grown) - 1 ?
            realloc(out->texts, (out->count + 1) * sizeof(*grown)) : NULL;
        if (!grown) { free(next); goto failed; }
        out->texts = grown; out->texts[out->count++] = (sh_decl_source){next, length};
        text = out->texts[out->count - 1]; current = target;
    }
failed:
    pl_chain_free(out); return 0;
}
static int pl_stock_authored(const pl_contributions *c, size_t unit)
{
    const sh_package_link_unit *value = sh_package_source_link_unit(c->source, unit);
    for (size_t f = 0; value && f < value->file_count; f++)
        if (value->files[f]->source_count && !value->files[f]->native_original) return 0;
    return 1;
}
static int pl_same_parent(const char *a, const char *b)
{ return (!a && !b) || (a && b && !strcmp(a, b)); }

/* Restore captured references in a composed or rebased text and stage it as
 * this unit's owned file, unless it equals the current one. */
static int pl_contribution_stage_kept(pl_build *b, const sh_package_sources *sources, size_t unit,
    const char *body, size_t length, const sh_compiled_resource *const *peers, size_t peer_count,
    pl_kept_refs *kept, pl_contribution_unit *pending, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + unit;
    const sh_compiled_resource **owners = calloc(peer_count + 1, sizeof(*owners));
    size_t native_length = 0;
    pl_contribution_walk walk = {c, unit, NULL, NULL, pending, error, capacity, kept};
    unsigned char *native;
    if (!owners) { snprintf(error, capacity, "editor contribution allocation failed"); return 0; }
    native = sh_package_source_link_bind_derived(c->source, c->origins[unit], (const unsigned char *)body,
        length, pl_restore_reference, &walk, pl_derived_exists, &walk, &native_length, error, capacity);
    pl_walk_close(&walk);
    if (!native) { free((void *)owners); return 0; }
    if (c->origins[unit] == unit && input->files[0]->body && native_length == input->files[0]->body_length &&
        !memcmp(native, input->files[0]->body, native_length) && pl_edges_same(c, unit, pending)) {
        free(native); free((void *)owners); pl_contribution_unit_free(pending); return 1;
    }
    owners[0] = input->files[0];
    for (size_t j = 0; j < peer_count; j++) owners[j + 1] = peers[j];
    /* The staged bytes move to the projection rather than being copied, and equal ones of one group are one body. */
    sh_package_body *shared = pl_stage_body(c, &native, native_length);
    sh_package_resource_projection projection = {input->files[0]->engine_path, owners, peer_count + 1,
        shared ? sh_package_body_bytes(shared) : native, native_length, NULL, NULL, 1};
    projection.body_owner = shared;
    pending->owned = sh_package_compilation_project(sources, &projection, 1, NULL,
        b->shared->canonical_path, error, capacity);
    sh_package_body_release(shared); free(native); free((void *)owners);
    if (!pending->owned) return 0;
    pending->file = pending->owned->resources; return 1;
}
static int pl_contribution_stage(pl_build *b, const sh_package_sources *sources, size_t unit,
    const char *body, size_t length, const sh_compiled_resource *const *peers, size_t peer_count,
    pl_contribution_unit *pending, char *error, size_t capacity)
{
    return pl_contribution_stage_kept(b, sources, unit, body, length, peers, peer_count, NULL, pending, error, capacity);
}

#ifdef SH_PACKAGE_LIBRARY_TESTING
static const char *g_contribution_defects[8];
static size_t g_contribution_defect_count;
void sh_package_library_test_contribution_defects(const char *const *packages, size_t count)
{
    g_contribution_defect_count = packages && count < 8 ? count : packages ? 8 : 0;
    for (size_t i = 0; i < g_contribution_defect_count; i++) g_contribution_defects[i] = packages[i];
}
/* The first member owned by a package named defective ("package", or
 * "package:declaration" for that declaration only), or SIZE_MAX. */
static size_t pl_test_defect(pl_build *b, const sh_package_sources *sources, const size_t *members, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        size_t owner = pl_member_owner(b, sources, members[i]);
        for (size_t d = 0; owner != SIZE_MAX && d < g_contribution_defect_count; d++) {
            const char *name = g_contribution_defects[d], *colon = strchr(name, ':');
            size_t length = colon ? (size_t)(colon - name) : strlen(name);
            if (strlen(sources->packages[owner].name) == length && !strncmp(sources->packages[owner].name, name, length) &&
                (!colon || !strcmp(b->contributions->inputs.units[members[i]].name, colon + 1))) return members[i];
        }
    }
    return SIZE_MAX;
}
static long g_contribution_read_failures;
static const char *g_contribution_read_name;
/* Make the next editor contribution reads fail like an unreadable source. */
void sh_package_library_test_contribution_read_failures(const char *name, long count)
{ g_contribution_read_name = name; g_contribution_read_failures = count; }
#endif

/* 1 when two declaration texts of one family state the same tree (spelling
 * aside), 0 when they differ or one does not parse, -1 on allocation failure.
 * Composition writes an entity's editorVars block with its direct members
 * sorted and moves an editor entry's collection fields to the tail of edit,
 * while shipped declarations state them in any order: the same uniquely keyed
 * members either way, so an unchanged stock declaration keeps its exact bytes
 * and native name (sh_package_editor_reordered_blocks selects the block). */
static int pl_same_declaration(const char *family, sh_decl_source left, sh_decl_source right)
{
    /* Native reads keep a repeated member's occurrences in order, so texts
     * with repeats compare in sequence throughout. */
    sh_decl_node *a = sh_decl_tree_parse_ordered(left, NULL, 0), *z = sh_decl_tree_parse_ordered(right, NULL, 0);
    size_t a_length = 0, z_length = 0;
    int repeats = a && z ? sh_decl_tree_repeats(a) : 0;
    if (!repeats && a && z) repeats = sh_decl_tree_repeats(z);
    if (repeats < 0) { sh_decl_tree_free(a); sh_decl_tree_free(z); return -1; }
    int same = a && z ? sh_decl_tree_equal(a, z, repeats ? NULL : sh_package_editor_reordered_blocks(family)) : 0;
    if (same) { sh_decl_tree_free(a); sh_decl_tree_free(z); return same; }
    char *x = a ? sh_decl_tree_write(a, &a_length) : NULL, *y = z ? sh_decl_tree_write(z, &z_length) : NULL;
    same = x && y ? a_length == z_length && !memcmp(x, y, a_length) : (a && !x) || (z && !y) ? -1 : 0;
    free(x); free(y); sh_decl_tree_free(a); sh_decl_tree_free(z);
    return same;
}

/* 1 when a member is an explicit contribution: text a package supplied, or
 * the product's own built-in (the product scope), whose content differs from
 * the shipped member's (the captured texts, spelling aside). A byte- or
 * tree-faithful copy a package bundles is the shipped declaration as that
 * scope reached it, like one it did not supply. Without a shipped member
 * every supplied text contributes. -1 on allocation failure. */
static int pl_contributes(const pl_contributions *c, const char *family, size_t unit, size_t shipped)
{
    const sh_package_link_unit *value = sh_package_source_link_unit(c->source, unit);
    const sh_package_link_unit *shipped_unit = shipped == SIZE_MAX ? NULL : sh_package_source_link_unit(c->source, shipped);
    const pl_build *b = c->build;
    if (!value || (shipped != SIZE_MAX && !shipped_unit)) return -1;
    int supplied = b && b->shared && b->shared->resource_count && b->authored_count < b->count - 1 &&
        c->inputs.scopes[unit] == b->authored_count;
    for (size_t f = 0; f < value->file_count; f++) if (value->files[f]->source_count) supplied = 1;
    if (!supplied) return 0;
    if (shipped == SIZE_MAX || value->file_count != 1 || shipped_unit->file_count != 1) return 1;
    const sh_compiled_resource *mine = value->files[0], *stock = shipped_unit->files[0];
    if (mine->native_original) return 0;
    if (!mine->body || !stock->body) return 1;
    int same = pl_same_declaration(family, (sh_decl_source){(const char *)mine->body, mine->body_length},
        (sh_decl_source){(const char *)stock->body, stock->body_length});
    return same < 0 ? -1 : !same;
}

#ifdef SH_PACKAGE_LIBRARY_TESTING
static int pl_editor_memo(void) { return g_editor_memo; }
#else
static int pl_editor_memo(void) { return 1; }
#endif
/* One declaration's editor contributions. Members naming the reference
 * member's parent compose into one shared surface; a member whose author gave
 * it another parent is that author's own variant and is only rebased. Every
 * member's inherited sparse lists are first rebased from the parent layout
 * its author saw onto the published parent's, parents composing first.
 * 1 composed; 0 failed, ending the compile; -1 failed with its owner found
 * while the compile goes on finding others (pl_contribution_failure). */
/* Whether text states needle anywhere. */
static int pl_states(sh_decl_source text, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t at = 0; text.text && at + n <= text.length; at++)
        if (text.text[at] == *needle && !memcmp(text.text + at, needle, n)) return 1;
    return 0;
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
/* Tests: 1 settles every keyed group the offered record holds any key for (the record trusted), 2 keys groups without
 * their members' content, 3 settles a recorded group without staging what it staged (mutation controls: a case must
 * then differ from its fresh compile). */
static int g_settled_mutation;
void sh_package_library_test_settled_mutation(int mutation) { g_settled_mutation = mutation; }
#define PL_SETTLED_MUTATED(m) (g_settled_mutation == (m))
/* SMP_SETTLED_DEBUG=1: each keyed editor group's settling on stderr (tests). */
static int pl_settled_debug(void)
{
    static volatile long read; static int on;
    if (!_InterlockedCompareExchange(&read, 1, 0)) {
        char *value = NULL; size_t length = 0;
        on = !_dupenv_s(&value, &length, "SMP_SETTLED_DEBUG") && value && *value == '1';
        free(value);
    }
    return on;
}
#define PL_SETTLED_DEBUG() pl_settled_debug()
#else
#define PL_SETTLED_MUTATED(m) 0
#define PL_SETTLED_DEBUG() 0
#endif
/* The settled key of an editor contribution group (R-348): every input its composition reads, by content. A group
 * reads its members' token-bound texts (the bound bytes are their class identities' function: a token is its target
 * class's identity, pl_tokens), its members' captured texts and their source flags (the contribution test,
 * pl_contributes), which member is the baseline's or the product's (roles, not scope positions), each member's sharing
 * (its private binding), and in the inspector and editor phases each member's parent chain, published and as its
 * author saw it (the units its inherit sites name, discovery's bindings, whose class identities the phase's tokens
 * still hold while no contribution rewrote them), and nothing else of other groups. A group that composed with nothing
 * staged under a key settles again under an equal key, without binding or composing. 1 keyed; 0 not keyed: a member
 * or a parent a contribution rewrote (its default slots read its scope's resolution; a parent rewritten earlier in the
 * phase is newer than its class identity) or derived, a chain deeper than the native limit, no record offered or kept,
 * a test's injected failures, or an identity that cannot be read. */
static int pl_group_settled_key(pl_build *b, const size_t *members, size_t count, int phase, unsigned char key[32])
{
    pl_contributions *c = b->contributions;
    if ((!b->reuse_in && !b->reuse_out) || phase < 1 || phase > 3 || !count || !c->classes.identities) return 0;
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if (g_contribution_defect_count || g_contribution_read_failures > 0 || g_probe_failures > 0 || g_rejudge_failures > 0)
        return 0;
#endif
    const unsigned char *shared = sh_package_source_link_shared(c->source);
    const sh_package_link_unit *first = c->inputs.units + members[0];
    int product = b->shared && b->shared->resource_count && b->authored_count < b->count - 1;
    size_t family = strlen(first->family), name = strlen(first->name);
    size_t room = 64 + family + name + count * 160, used = 0;
    unsigned char *bytes = malloc(room);
    int ok = 0;
    if (!bytes) return 0;
#define PL_PUT(data, n) do { if (used + (n) > room) goto done; memcpy(bytes + used, (data), (n)); used += (n); } while (0)
    {
        uint64_t head[6] = {(uint64_t)phase, family, name, count, (uint64_t)product, (uint64_t)pl_editor_memo()};
        PL_PUT("snapmap-plus settled editor group 1", 35);
        PL_PUT(head, sizeof(head)); PL_PUT(first->family, family); PL_PUT(first->name, name);
    }
    for (size_t k = 0; k < count; k++) {
        size_t unit = members[k];
        const sh_package_link_unit *u = c->inputs.units + unit;
        const sh_package_link_unit *captured = sh_package_source_link_unit(c->source, unit);
        sh_package_file_identity current, own;
        if (unit >= c->captured || c->origins[unit] != unit || c->units[unit].owned || u->file_count != 1 || !captured ||
            unit >= c->classes.node_count) goto done;
        size_t scope = c->inputs.scopes[unit];
        unsigned char role[2] = {(unsigned char)(scope == b->count - 1 ? 2 : product && scope == b->authored_count ? 1 : 0),
            (unsigned char)(shared && shared[unit] ? 1 : 0)};
        PL_PUT(role, 2);
        PL_PUT(PL_SETTLED_MUTATED(2) ? (const unsigned char *)"################################" :
            c->classes.identities[c->classes.nodes[unit]], 32);
        if (!sh_package_compilation_identity(u->compilation, u->files[0], &current, NULL, 0)) goto done;
        if (PL_SETTLED_MUTATED(2)) memset(&current, 0, sizeof(current));
        PL_PUT(&current.length, sizeof(current.length)); PL_PUT(current.digest, 32);
        if (phase != 2) {
            /* The parent chain by inherit sites, each parent as the phase began. */
            size_t at = unit;
            uint64_t depth = 0;
            for (;;) {
                const sh_package_link_edge *sites = NULL;
                size_t n = 0, parent = SIZE_MAX;
                const sh_package_link_unit *holder = c->inputs.units + at;
                if (holder->file_count != 1 || !sh_package_source_link_sites(c->source, at, holder->files[0], &sites, &n))
                    goto done;
                for (size_t s = 0; s < n && parent == SIZE_MAX; s++)
                    if (sites[s].slot && !strcmp(sites[s].slot, "inherit")) parent = sites[s].target;
                if (parent == SIZE_MAX) break;
                if (++depth > 32 || parent >= c->captured || c->origins[parent] != parent || c->units[parent].owned ||
                    parent >= c->classes.node_count) goto done;
                PL_PUT(&depth, sizeof(depth));
                PL_PUT(PL_SETTLED_MUTATED(2) ? (const unsigned char *)"################################" :
                    c->classes.identities[c->classes.nodes[parent]], 32);
                at = parent;
            }
            PL_PUT(&depth, sizeof(depth));
        }
        uint64_t files = captured->file_count;
        PL_PUT(&files, sizeof(files));
        for (size_t f = 0; f < captured->file_count; f++) {
            const sh_compiled_resource *file = captured->files[f];
            if (file == u->files[0]) own = current;
            else if (!sh_package_compilation_identity(captured->compilation, file, &own, NULL, 0)) goto done;
            if (PL_SETTLED_MUTATED(2)) memset(&own, 0, sizeof(own));
            unsigned char flags[2] = {(unsigned char)(file->source_count != 0), (unsigned char)(file->native_original != 0)};
            PL_PUT(&own.length, sizeof(own.length)); PL_PUT(own.digest, 32); PL_PUT(flags, 2);
        }
    }
#undef PL_PUT
    {
        sh_package_file_identity identity;
        ok = sh_package_bytes_identity(bytes, used, &identity);
        if (ok) memcpy(key, identity.digest, 32);
    }
done:
    free(bytes); return ok;
}
/* Commit a group's staged members together (pl_contribution_group, pl_group_restage): every one is kept first, then
 * each takes its staged unit. 0 when a captured record cannot be kept: nothing is installed. */
static int pl_group_commit(pl_build *b, const size_t *members, size_t count, pl_contribution_unit *pending,
    struct pl_editor_census *census, size_t *staged)
{
    pl_contributions *c = b->contributions;
    for (size_t i = 0; i < count; i++) if (pending[i].owned && !pl_unit_keep(c, members[i])) return 0;
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        if (!pending[i].owned) continue;
        c->dirty = 1;
        if (census) census->staged++;
        ++*staged;
        pl_contribution_unit_free(c->units + unit); c->units[unit] = pending[i]; memset(pending + i, 0, sizeof(*pending));
        c->inputs.units[unit].compilation = c->units[unit].owned;
        c->inputs.units[unit].files = &c->units[unit].file;
        const sh_package_link_unit *original = sh_package_source_link_unit(c->source, unit);
        c->inputs.units[unit].original_binding = original && original->original_binding &&
            original->files[0]->body && original->files[0]->body_length == c->units[unit].file->body_length &&
            !memcmp(original->files[0]->body, c->units[unit].file->body, c->units[unit].file->body_length);
    }
    return 1;
}
/* What one member of a keyed group staged (R-348): the text it staged and its peers by member index, kept with the
 * group's key when the group commits (pl_group_outputs), and staged again from there (pl_group_restage). */
typedef struct pl_staged_note { char *text; size_t length; uint32_t *peers; size_t peer_count; } pl_staged_note;
static void pl_staged_notes_free(pl_staged_note *notes, size_t count)
{
    for (size_t i = 0; notes && i < count; i++) { free(notes[i].text); free(notes[i].peers); }
    free(notes);
}
/* Member i's note: its staged text, and with peers its peers by member index, chosen as the group chose them (every
 * other included member outside the baseline scope; members may share one captured file, so never by file). */
static int pl_group_note(const pl_build *b, pl_staged_note *note, const size_t *members, size_t count, size_t i,
    const char *text, size_t length, const unsigned char *included, int peers)
{
    const pl_contributions *c = b->contributions;
    if (count > UINT32_MAX || !(note->text = malloc(length ? length : 1)) ||
        (peers && !(note->peers = malloc(count * sizeof(*note->peers))))) return 0;
    memcpy(note->text, text, length); note->length = length; note->peer_count = 0;
    for (size_t j = 0; peers && j < count; j++)
        if (j != i && included[j] && c->inputs.scopes[members[j]] != b->count - 1) note->peers[note->peer_count++] = (uint32_t)j;
    return 1;
}
/* The committed members' notes as the record keeps them: u32 entries, then for each member index u32, its peers
 * (u32 count, u32 indices) and its text (u64 length, bytes), little-endian. NULL on allocation failure. */
static unsigned char *pl_group_outputs(const pl_staged_note *notes, const unsigned char *committed, size_t count,
    size_t *length)
{
    size_t size = 4, entries = 0;
    for (size_t i = 0; i < count; i++) if (committed[i]) {
        if (!notes[i].text || notes[i].peer_count > UINT32_MAX) return NULL;
        size += 16 + 4 * notes[i].peer_count + notes[i].length; entries++;
    }
    unsigned char *out = malloc(size), *at = out;
    if (!out) return NULL;
#define PL_U32(v) do { uint32_t x_ = (uint32_t)(v); for (int k_ = 0; k_ < 4; k_++) *at++ = (unsigned char)(x_ >> (8 * k_)); } while (0)
    PL_U32(entries);
    for (size_t i = 0; i < count; i++) if (committed[i]) {
        PL_U32(i); PL_U32(notes[i].peer_count);
        for (size_t k = 0; k < notes[i].peer_count; k++) PL_U32(notes[i].peers[k]);
        uint64_t n = notes[i].length;
        for (int k = 0; k < 8; k++) *at++ = (unsigned char)(n >> (8 * k));
        memcpy(at, notes[i].text, notes[i].length); at += notes[i].length;
    }
#undef PL_U32
    *length = size; return out;
}
/* A settled group's recorded staging done again (R-348): each recorded member is staged from its recorded text with
 * its recorded peers, and the group commits as its composition did. 1 done (staged counted); 0 when the record does
 * not apply here (a malformed entry, or a member whose staging no longer produces a unit of its own): nothing is
 * installed and the group composes afresh. */
static int pl_group_restage(pl_build *b, const sh_package_sources *sources, const size_t *members, size_t count,
    const unsigned char *outputs, size_t length, struct pl_editor_census *census, size_t *staged)
{
    pl_contributions *c = b->contributions;
    *staged = 0;
    if (!length) return 1;
    const unsigned char *at = outputs, *end = outputs + length;
    pl_contribution_unit *pending = calloc(count, sizeof(*pending));
    const sh_compiled_resource **peers = calloc(count + 1, sizeof(*peers));
    unsigned char *seen = calloc(count ? count : 1, 1);
    char error[512];
    pl_stage_pool pool;
    pl_stage_pool_open(c, &pool);
#define PL_TAKE(out, n) ((size_t)(end - at) >= (n) ? (memcpy((out), at, (n)), at += (n), 1) : 0)
    uint32_t entries = 0;
    unsigned char word[8];
    int ok = pending && peers && seen && PL_TAKE(word, 4);
    if (ok) entries = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
    ok = ok && entries && entries <= count;
    for (uint32_t e = 0; ok && e < entries; e++) {
        uint32_t i = 0, n = 0;
        uint64_t text_length = 0;
        ok = PL_TAKE(word, 4);
        if (ok) i = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
        ok = ok && i < count && !seen[i] && PL_TAKE(word, 4);
        if (ok) n = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
        ok = ok && n <= count;
        for (uint32_t k = 0; ok && k < n; k++) {
            uint32_t j = 0;
            ok = PL_TAKE(word, 4);
            if (ok) j = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
            ok = ok && j < count && j != i;
            if (ok) peers[k] = c->inputs.units[members[j]].files[0];
        }
        ok = ok && PL_TAKE(word, 8);
        if (ok) for (int k = 7; k >= 0; k--) text_length = text_length << 8 | word[k];
        ok = ok && text_length <= (uint64_t)(end - at);
        if (!ok) break;
        const char *text = (const char *)at; at += (size_t)text_length;
        seen[i] = 1;
        ok = pl_contribution_stage(b, sources, members[i], text, (size_t)text_length, peers, n, pending + i, error,
            sizeof(error));
        if (PL_SETTLED_DEBUG() && !ok) fprintf(stderr, "settled restage: member %u: %s\n", i, error);
        if (PL_SETTLED_DEBUG() && ok && !pending[i].owned) fprintf(stderr, "settled restage: member %u stages nothing\n", i);
        ok = ok && pending[i].owned;
    }
    if (PL_SETTLED_DEBUG() && (!ok || at != end))
        fprintf(stderr, "settled restage: %u entries for %zu members, stopped %s\n", entries, count,
            at != end ? "before the end" : "at an entry");
#undef PL_TAKE
    ok = ok && at == end && pl_group_commit(b, members, count, pending, census, staged);
    for (size_t i = 0; pending && i < count; i++) pl_contribution_unit_free(pending + i);
    free(pending); free((void *)peers); free(seen);
    pl_stage_pool_close(c, &pool);
    return ok;
}
static int pl_contribution_group(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + members[0];
    int phase = sh_package_editor_contribution_phase(input->family);
    sh_decl_tree_census census_began = {0}, census_first = {0}, census_formed = {0}, census_rebased = {0},
        census_composed = {0}, census_ended = {0};
    struct pl_editor_census *census = phase >= 1 && phase <= 3 ? b->editor_census + phase : NULL;
    int census_contributed = 0;
    if (b->census_owned) sh_decl_tree_census_read(&census_began);
    census_first = census_formed = census_rebased = census_composed = census_began;
    /* Unchanged-scope reuse (R-348): a group whose inputs equal those under which it composed with nothing staged
     * settles again (its members stay as they are, composed). */
    unsigned char settled_key[32];
    int settling = pl_group_settled_key(b, members, count, phase, settled_key);
    size_t staged_here = 0;
    if (phase >= 1 && phase <= 3) {
        if (settling) b->settled_keyed[phase]++; else if (b->reuse_in || b->reuse_out) b->settled_unkeyed[phase]++;
    }
    const unsigned char *settled_outputs = NULL;
    size_t settled_length = 0;
    int recorded = settling && b->reuse_in &&
        sh_package_source_reuse_settled(b->reuse_in, settled_key, &settled_outputs, &settled_length);
    if (!recorded && settling && b->reuse_in && PL_SETTLED_MUTATED(1) && sh_package_source_reuse_settled_count(b->reuse_in))
        recorded = 1;
    if (recorded && PL_SETTLED_MUTATED(3)) settled_length = 0;
    if (recorded) {
        size_t restaged = 0;
        if (pl_group_restage(b, sources, members, count, settled_outputs, settled_length, census, &restaged)) {
            b->settled_hits[phase]++; b->settled_hit_members[phase] += count; b->settled_restaged[phase] += restaged;
            if (b->reuse_attempted) *b->reuse_attempted = 1;
            if (b->reuse_out && !sh_package_source_reuse_add_settled(b->reuse_out, settled_key, settled_outputs,
                    settled_length)) {
                snprintf(error, capacity, "editor contribution allocation failed"); return 0;
            }
            b->settled_recorded[phase]++;
            return 1;
        }
        /* The record's staging does not apply here: the group composes afresh. */
        if (PL_SETTLED_DEBUG()) fprintf(stderr, "settled group: phase %d %s %s: recorded staging (%zu bytes) does not apply\n",
            phase, input->family, input->name, settled_length);
    }
    pl_staged_note *notes = settling && b->reuse_out ? calloc(count ? count : 1, sizeof(*notes)) : NULL;
    unsigned char *committed = NULL;
#define PL_NOTE(i, text, length, with_peers) do { \
        if (notes && pending[i].owned && !pl_group_note(b, notes + (i), members, count, (i), (text), (length), included, \
            (with_peers))) { pl_staged_notes_free(notes, count); notes = NULL; } } while (0)
    sh_decl_source *texts = calloc(count, sizeof(*texts)), *parts = calloc(count + 1, sizeof(*parts));
    const sh_compiled_resource **peers = calloc(count + 1, sizeof(*peers));
    pl_contribution_unit *pending = calloc(count, sizeof(*pending));
    char **parents = calloc(count, sizeof(*parents));
    unsigned char *included = calloc(count, 1), *rebased = calloc(count, 1);
    size_t *part_member = calloc(count + 1, sizeof(*part_member));
    /* Per distinct member input: its composed body, binding and unchanged
     * check (composed[i] set only for the first member with that input). */
    char **composed = calloc(count, sizeof(*composed));
    size_t *composed_length = calloc(count, sizeof(*composed_length));
    signed char *composed_private = calloc(count, 1), *same_known = calloc(count, 1), *same_value = calloc(count, 1);
    sh_decl_source base = {"{}", 2}, *targets = calloc(count, sizeof(*targets)), *outputs = calloc(count, sizeof(*outputs));
    unsigned char *presented = NULL, *like_base = NULL;
    char *shared_text = NULL; size_t shared_length = 0;
    size_t source_count = 0, base_member = SIZE_MAX, reference, culprit = SIZE_MAX;
    int ok = 0, kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT, any_rebased = 0;
    pl_chain published = {0}, authored_base = {0};
    /* Each member's text before its rebase, and the namespace the rebase's
     * identities for controls without one live in (sh_package_editor_slots). */
    sh_decl_source *unrebased = calloc(count, sizeof(*unrebased));
    char space[48] = "";
    unsigned space_serial = 0;
    pl_stage_pool pool;
    pl_stage_pool_open(c, &pool);
    if (!texts || !parts || !peers || !pending || !parents || !included || !rebased || !part_member ||
        !composed || !composed_length || !composed_private || !same_known || !same_value || !targets || !outputs ||
        !unrebased) {
        snprintf(error, capacity, "editor contribution allocation failed"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        const sh_compiled_resource *file = c->inputs.units[unit].files[0];
        size_t length = 0;
        unsigned char *body = sh_package_compilation_read(c->inputs.units[unit].compilation, file,
            SIZE_MAX, &length, error, capacity);
#ifdef SH_PACKAGE_LIBRARY_TESTING
        if (body && g_contribution_read_failures > 0 && (!g_contribution_read_name ||
                !strcmp(c->inputs.units[unit].name, g_contribution_read_name)) && g_contribution_read_failures--) {
            free(body); body = NULL; snprintf(error, capacity, "editor contribution source is unreadable");
        }
#endif
        if (!body) goto failed;
        pl_contribution_walk walk = {c, unit, NULL, NULL, NULL, error, capacity};
        texts[i].text = (char *)sh_package_source_link_bind_derived(c->source, unit, body, length,
            pl_token_reference, &walk, pl_derived_exists, &walk, &texts[i].length, error, capacity);
        pl_walk_close(&walk); free(body); if (!texts[i].text) goto failed;
        if (c->inputs.scopes[unit] == b->count - 1) base_member = i;
        if (phase != 2 && !pl_inherit_name(texts[i], parents + i)) {
            snprintf(error, capacity, "editor definition text cannot be read"); goto failed;
        }
    }
    if (b->census_owned) sh_decl_tree_census_read(&census_first);
    /* A member stating a field, block or list more than once is read again,
     * in order, as the native reader reads it (B3 evidence F2, F10): it
     * composes as its native form, one statement per member that reads the
     * same over the parents its author saw. One with no such form is a gap in
     * this model, never its author's defect; one whose own fields change
     * between a value and a block over its parents is its own text's. */
    for (size_t i = 0; phase != 2 && i < count; i++) {
        pl_chain authored_chain = {0};
        char *form = NULL; size_t form_length = 0;
        int repeats = sh_package_editor_repeats_type(c->inputs.units[members[i]].family, texts[i]), formed;
        if (repeats < 0) { snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
        if (repeats && parents[i] && !pl_parent_chain(c, members[i], texts[i], 0, &authored_chain, &kind, error, capacity)) {
            culprit = members[i]; goto failed;
        }
        formed = sh_package_editor_native_form_type(c->inputs.units[members[i]].family, texts[i], authored_chain.texts, authored_chain.count, &form, &form_length);
        pl_chain_free(&authored_chain);
        if (formed == -1) { kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT; snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
        if (formed == -2) {
            kind = SH_PACKAGE_EDITOR_FAILURE_SOURCE; culprit = members[i];
            snprintf(error, capacity, "the editor declaration has a malformed member or changes between a value and a block over its parents");
            goto failed;
        }
        if (formed < 0) {
            kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
            snprintf(error, capacity, "the editor declaration's native collection form is not modelled");
            goto failed;
        }
        if (formed) { free((char *)texts[i].text); texts[i] = (sh_decl_source){form, form_length}; }
    }
    if (b->census_owned) sh_decl_tree_census_read(&census_formed);
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if ((culprit = pl_test_defect(b, sources, members, count)) != SIZE_MAX) {
        kind = SH_PACKAGE_EDITOR_FAILURE_SOURCE; snprintf(error, capacity, "test contribution defect"); goto failed;
    }
#endif
    reference = base_member != SIZE_MAX ? base_member : 0;
    for (size_t i = 0; i < count; i++) included[i] = phase == 2 || pl_same_parent(parents[i], parents[reference]);
    /* Only a package's own text contributes. Without a baseline member no
     * package authors this declaration: each member is the stock declaration a
     * scope reached, and there is nothing to compose onto it (composing it over
     * an empty baseline would only respell it). */
    int contributed = base_member != SIZE_MAX;
    for (size_t i = 0; !contributed && i < count; i++) contributed = !pl_stock_authored(c, members[i]);
    census_contributed = contributed;
    /* Rebase inherited collections: first only where the published parent's
     * layout differs, then, if any member changed, every member of the shared
     * surface, so all of them state complete lists. The rebase gives a control
     * without an identity field one under a namespace no member states, and
     * checks its parent texts too; when one states it (-2), every member
     * returns to its unrebased text and the rebase starts again under the
     * next namespace. */
    /* A control without an identity field composes only by the identity a
     * rebase gives it, so when a member appends one (in its author's frame)
     * every member of a contributed group is rebased into the published frame,
     * even where no parent layout moved; pages the published parent lacks are
     * never stated. */
    int pathless = 0;
    /* Members stating one inherited collection differently, one patching its
     * parent's slots and another stating it whole, compose only in one frame:
     * every member of the contributed group is rebased into the published
     * frame, each from its own author's frame. */
    int mixed = 0;
    if (phase != 2 && contributed) {
        unsigned forms = 0;
        for (size_t i = 0; i < count; i++) if (included[i] && parents[i]) {
            unsigned own = 0;
            if (sh_package_editor_list_forms(input->family, texts[i], &own) < 0) {
                snprintf(error, capacity, "editor contribution allocation failed"); goto failed;
            }
            forms |= own;
        }
        for (unsigned bit = 0; bit < 32 && !mixed; bit += 2) mixed = (forms >> bit & 3u) == 3u;
    }
rebase:
    for (int taken = 1; taken; space_serial++) {
        snprintf(space, sizeof(space), "#snapmap-plus-slot-%u:", space_serial);
        taken = 0;
        for (size_t i = 0; i < count && !taken; i++) taken = pl_states(texts[i], space);
    }
    for (int pass = 0; phase != 2 && pass < 2 && (pass == 0 || ((any_rebased || pathless || mixed) && contributed)); pass++)
        for (size_t i = 0; i < count; i++) {
            pl_chain own_published = {0}, own_authored = {0};
            const pl_chain *authored = &own_authored, *target = included[i] ? &published : &own_published;
            char *out = NULL; size_t out_length = 0;
            sh_package_editor_failure failure;
            int result;
            if (!parents[i] || (pass && (!included[i] || rebased[i]))) continue;
            if (included[i] && !published.texts && !published.count &&
                !pl_parent_chain(c, members[reference], texts[reference], 1, &published, &kind, error, capacity)) {
                culprit = members[reference]; goto failed;
            }
            if (!included[i] && !pl_parent_chain(c, members[i], texts[i], 1, &own_published, &kind, error, capacity)) {
                culprit = members[i]; goto failed;
            }
            if (pl_stock_authored(c, members[i]) && base_member != SIZE_MAX && pl_same_parent(parents[i], parents[base_member])) {
                if (!authored_base.count &&
                    !pl_parent_chain(c, members[base_member], texts[base_member], 0, &authored_base, &kind, error, capacity)) {
                    pl_chain_free(&own_published); culprit = members[base_member]; goto failed;
                }
                authored = &authored_base;
            } else if (!pl_parent_chain(c, members[i], texts[i], 0, &own_authored, &kind, error, capacity)) {
                pl_chain_free(&own_published); culprit = members[i]; goto failed;
            }
            sh_package_editor_slots slots = {space, base_member == SIZE_MAX ? (sh_decl_source){NULL, 0} :
                unrebased[base_member].text ? unrebased[base_member] : texts[base_member], 0};
            result = sh_package_editor_rebase(input->family, texts[i], authored->texts, authored->count,
                target->texts, target->count, pass, &slots, &out, &out_length, &failure, error, capacity);
            if (slots.appends && included[i]) pathless = 1;
            pl_chain_free(&own_published); pl_chain_free(&own_authored);
            if (result == -2) {
                for (size_t k = 0; k < count; k++) if (unrebased[k].text) {
                    free((char *)texts[k].text); texts[k] = unrebased[k]; unrebased[k] = (sh_decl_source){NULL, 0};
                }
                memset(rebased, 0, count); any_rebased = 0; pathless = 0;
                goto rebase;
            }
            if (result < 0) { kind = failure.kind; culprit = members[i]; goto failed; }
            if (result) {
                if (unrebased[i].text) free((char *)texts[i].text);
                else unrebased[i] = texts[i];
                texts[i] = (sh_decl_source){out, out_length};
                rebased[i] = 1; if (included[i]) any_rebased = 1;
            }
        }
    /* Complete lists composed without a rebase (a declaration restated whole)
     * give their controls without an identity field one relative to the
     * shipped declaration's, under the same namespace, every member against
     * the shipped text as written. */
    if (b->census_owned) sh_decl_tree_census_read(&census_rebased);
    int framed = 0;
    if (phase != 2 && !any_rebased && contributed) {
        sh_decl_source shipped = base_member != SIZE_MAX ? texts[base_member] : (sh_decl_source){"{}", 2};
        sh_decl_source *framed_texts = calloc(count, sizeof(*framed_texts));
        if (!framed_texts) { snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
        for (size_t i = 0; i < count; i++) if (included[i]) {
            char *out = NULL; size_t out_length = 0;
            int given = sh_package_editor_frame_slots(input->family, texts[i], shipped, published.texts, published.count,
                space, &out, &out_length);
            if (given < 0) {
                for (size_t k = 0; k < count; k++) free((char *)framed_texts[k].text);
                free(framed_texts);
                if (given == -2) { snprintf(error, capacity, "editor slot namespace occurs in a member"); goto failed; }
                snprintf(error, capacity, "editor contribution allocation failed"); goto failed;
            }
            if (given) framed_texts[i] = (sh_decl_source){out, out_length};
        }
        for (size_t i = 0; i < count; i++) if (framed_texts[i].text) {
            free((char *)texts[i].text); texts[i] = framed_texts[i]; framed = 1;
        }
        free(framed_texts);
    }
    /* Only a package's own change contributes (pl_contributes). A stock member
     * a scope merely reaches, or a faithful copy a package bundles, takes the
     * composed entry where its scope binds what the shipped member binds;
     * where that scope's graph binds a dependency otherwise (a picker whose
     * allowed entry shows an overridden inspector, a gameplay variant it
     * places) it is a distinct variant of the entry, never an edit to it: two
     * scopes binding one stock control differently are two valid variants,
     * not a contradiction. */
    like_base = calloc(count, 1);
    if (!like_base) { snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
    for (size_t i = 0; i < count; i++) if (included[i] && contributed) {
        if (i == base_member) base = texts[i];
        else {
            int own_change = pl_contributes(c, input->family, members[i], base_member != SIZE_MAX ? members[base_member] : SIZE_MAX);
            if (own_change < 0) { snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
            if (own_change) { part_member[source_count] = i; parts[source_count++] = texts[i]; continue; }
            like_base[i] = base_member != SIZE_MAX && texts[i].length == texts[base_member].length &&
                !memcmp(texts[i].text, texts[base_member].text, texts[i].length);
        }
    }
    /* A stock child of a relaid parent republishes its rebased lists. */
    if (!source_count && base_member != SIZE_MAX && rebased[base_member]) {
        part_member[source_count] = base_member; parts[source_count++] = base;
    }
    /* Presentation a package's own entry changes (fields the native editor
     * reads for display only) composes into the shared entry: the shipped
     * member and every part differing from it only in presentation compose
     * onto that shared text and so bind one entry; a part with any other
     * difference stays its author's own variant. */
    if (source_count && base_member != SIZE_MAX) {
        if (!(presented = calloc(source_count, 1))) { snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
        if (sh_package_editor_presentation(input->family, base, parts, source_count, presented,
                &shared_text, &shared_length) < 0) {
            snprintf(error, capacity, "editor contribution allocation failed"); goto failed;
        }
    }
    for (size_t i = 0; i < count && source_count; i++) {
        size_t unit = members[i], length = 0, peer_count = 0, prior = SIZE_MAX;
        sh_package_editor_failure failure;
        if (!included[i]) continue;
        sh_decl_source target = texts[i];
        if (shared_text) {
            int shares = i == base_member || like_base[i];
            for (size_t k = 0; k < source_count && !shares; k++) shares = part_member[k] == i && presented[k];
            if (shares) target = (sh_decl_source){shared_text, shared_length};
        }
        targets[i] = target;
        /* The body is a function of the shared baseline and parts and of this
         * member's target text and binding: repeats reuse the first result. */
        signed char private_binding = !sh_package_source_link_shared(c->source)[unit];
        for (size_t k = 0; pl_editor_memo() && k < i && prior == SIZE_MAX; k++)
            if (composed[k] && composed_private[k] == private_binding && targets[k].length == target.length &&
                !memcmp(targets[k].text, target.text, target.length) && texts[k].length == texts[i].length &&
                !memcmp(texts[k].text, texts[i].text, texts[i].length)) prior = k;
        const char *body = prior != SIZE_MAX ? composed[prior] : NULL;
        if (body) length = composed_length[prior];
        else {
            body = composed[i] = sh_package_editor_contributions_report(input->family, base, parts, source_count,
                target, private_binding, &length, &failure, error, capacity);
            if (body && (any_rebased || framed)) {
                char *plain = NULL; size_t plain_length = 0;
                int dropped = sh_package_editor_strip_slots(input->family, space, (sh_decl_source){body, length},
                    &plain, &plain_length);
                if (dropped < 0) { snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
                if (dropped) { free(composed[i]); body = composed[i] = plain; length = plain_length; }
            }
            if (body && any_rebased && published.count) {
                sh_decl_source restated = {body, length};
                if (!pl_native_restatement(input->family, &restated, &published, &kind, error, capacity)) {
                    if (restated.text != body) free((char *)restated.text);
                    goto failed;
                }
                body = composed[i] = (char *)restated.text; length = restated.length;
            }
        }
        if (!body) {
            kind = failure.kind;
            if (kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE && failure.source < source_count)
                culprit = members[part_member[failure.source]];
            else if (kind == SH_PACKAGE_EDITOR_FAILURE_COVERAGE) {
                pl_collection_probe probe = {input->family, 0, !sh_package_source_link_shared(c->source)[unit],
                    base, target};
                pl_joint_participants(b, sources, members, part_member, parts, source_count,
                    pl_collection_composes, &probe, error, capacity);
            }
            goto failed;
        }
        /* A member the union leaves as it was keeps its exact bytes, and with
         * them its native identity, whatever else the group holds: only a
         * recomposed spelling would differ. */
        if (prior == SIZE_MAX) { composed_length[i] = length; composed_private[i] = private_binding; }
        outputs[i] = (sh_decl_source){body, length};
        /* A member composing onto the shared presentation stages the shared
         * text even when its own states the same, so every such member binds
         * the one entry; others keep their exact bytes when left as they were. */
        if (!rebased[i] && target.text == texts[i].text) {
            int same = prior != SIZE_MAX && same_known[prior] ? same_value[prior] :
                pl_same_declaration(input->family, texts[i], (sh_decl_source){body, length});
            if (same < 0) { snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
            if (prior == SIZE_MAX) { same_known[i] = 1; same_value[i] = (signed char)same; }
            if (same) continue;
        }
        for (size_t j = 0; j < count; j++) if (j != i && included[j] && c->inputs.scopes[members[j]] != b->count - 1)
            peers[peer_count++] = c->inputs.units[members[j]].files[0];
        if (!pl_contribution_stage(b, sources, unit, body, length, peers, peer_count, pending + i, error, capacity))
            goto failed;
        PL_NOTE(i, body, length, 1);
    }
    /* A member with another parent is its author's own variant, and a stock
     * member nothing contributes to is itself: either is only rebased. */
    for (size_t i = 0; i < count; i++) if ((!included[i] || !contributed || !source_count) && rebased[i] &&
        !pending[i].owned) {
        char *plain = NULL; size_t plain_length = 0;
        int dropped = sh_package_editor_strip_slots(input->family, space, texts[i], &plain, &plain_length);
        if (dropped < 0) { snprintf(error, capacity, "editor contribution allocation failed"); goto failed; }
        if (dropped) { free((char *)texts[i].text); texts[i] = (sh_decl_source){plain, plain_length}; }
        {
            pl_chain own = {0};
            const pl_chain *chain = included[i] ? &published : &own;
            int restated;
            if (!included[i] && !pl_parent_chain(c, members[i], texts[i], 1, &own, &kind, error, capacity)) {
                culprit = members[i]; goto failed;
            }
            restated = !chain->count || pl_native_restatement(input->family, texts + i, chain, &kind, error, capacity);
            pl_chain_free(&own);
            if (!restated) goto failed;
        }
        if (!pl_contribution_stage(b, sources, members[i], texts[i].text, texts[i].length, NULL, 0, pending + i, error, capacity))
            goto failed;
        PL_NOTE(i, texts[i].text, texts[i].length, 0);
    }
    /* A member whose composition yields exactly the shared member's output
     * binds that one entry. When the baseline member stages its output, every
     * such member stages it too, even one whose own text already stated the
     * same: its scope bound some dependency another way when composition ran
     * (a class that later compositions merge), so keeping its captured bytes
     * would leave one declaration as two entries with equal trees and equal
     * final bindings. Each keeps its own owners; members whose output differs
     * keep their own composition or bytes. */
    if (base_member != SIZE_MAX && included[base_member] && pending[base_member].owned && outputs[base_member].text)
        for (size_t i = 0; i < count; i++) {
            size_t peer_count = 0;
            if (i == base_member || !included[i] || pending[i].owned || !outputs[i].text ||
                outputs[i].length != outputs[base_member].length ||
                memcmp(outputs[i].text, outputs[base_member].text, outputs[i].length)) continue;
            for (size_t j = 0; j < count; j++) if (j != i && included[j] && c->inputs.scopes[members[j]] != b->count - 1)
                peers[peer_count++] = c->inputs.units[members[j]].files[0];
            if (!pl_contribution_stage(b, sources, members[i], outputs[i].text, outputs[i].length, peers, peer_count,
                    pending + i, error, capacity)) goto failed;
            PL_NOTE(i, outputs[i].text, outputs[i].length, 1);
        }
#undef PL_NOTE
    if (b->census_owned) sh_decl_tree_census_read(&census_composed);
    /* Commit the phase together: no later supplier observes a partly composed
     * peer, and failure leaves the previous captured graph intact. */
    if (notes && (committed = calloc(count ? count : 1, 1)) != NULL)
        for (size_t i = 0; i < count; i++) committed[i] = pending[i].owned != NULL;
    if (!pl_group_commit(b, members, count, pending, census, &staged_here)) {
        kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT; culprit = SIZE_MAX;
        snprintf(error, capacity, "%s", "cannot keep a captured unit's record"); goto failed;
    }
    ok = 1;
    if (PL_SETTLED_DEBUG()) fprintf(stderr, "settled group: phase %d %s %s: composed, %zu staged, keyed %d, notes %s\n",
        phase, input->family, input->name, staged_here, settling, notes ? "kept" : "none");
    /* Recorded: what it staged (none, or each committed member's text and peers) under its key. A group whose notes
     * could not be kept is not recorded: it composes again next time. */
    if (settling && b->reuse_out && (!staged_here || (notes && committed))) {
        size_t length = 0;
        unsigned char *kept = staged_here ? pl_group_outputs(notes, committed, count, &length) : NULL;
        if ((staged_here && !kept) || !sh_package_source_reuse_add_settled(b->reuse_out, settled_key, kept, length)) {
            free(kept); snprintf(error, capacity, "editor contribution allocation failed"); ok = 0; goto done;
        }
        free(kept);
        b->settled_recorded[phase]++;
    }
    goto done;
failed:
    ok = pl_contribution_failure(b, sources, input, kind, culprit, error, capacity) ? -1 : 0;
done:
    if (census && b->census_owned) {
        sh_decl_tree_census_read(&census_ended);
        census->groups++; census->members += count; census->parses += census_ended.parses - census_began.parses;
        census->first_parses += census_first.parses - census_began.parses;
        /* A stage the group left early (failed) counts nothing past where it stopped. */
        if (census_formed.parses >= census_first.parses) census->form_parses += census_formed.parses - census_first.parses;
        if (census_rebased.parses >= census_formed.parses && census_rebased.parses != census_began.parses)
            census->rebase_parses += census_rebased.parses - census_formed.parses;
        if (census_composed.parses >= census_rebased.parses && census_composed.parses != census_began.parses &&
            census_rebased.parses != census_began.parses) census->compose_parses += census_composed.parses - census_rebased.parses;
        if (census_contributed) {
            census->contributed++; census->contributed_members += count;
            census->contributed_parses += census_ended.parses - census_began.parses;
        }
    }
    pl_stage_pool_close(c, &pool);
    pl_staged_notes_free(notes, count); free(committed);
    if (texts) for (size_t i = 0; i < count; i++) free((char *)texts[i].text);
    if (unrebased) for (size_t i = 0; i < count; i++) free((char *)unrebased[i].text);
    free(unrebased);
    if (pending) for (size_t i = 0; i < count; i++) pl_contribution_unit_free(pending + i);
    if (parents) for (size_t i = 0; i < count; i++) free(parents[i]);
    if (composed) for (size_t i = 0; i < count; i++) free(composed[i]);
    pl_chain_free(&published); pl_chain_free(&authored_base);
    free(texts); free(parts); free((void *)peers); free(pending); free(parents); free(included); free(rebased); free(part_member);
    free(composed); free(composed_length); free(composed_private); free(same_known); free(same_value);
    free(targets); free(outputs); free(presented); free(like_base); free(shared_text);
    return ok;
}

/* ---- Registration keys ------------------------------------------------------
 * A conductor row registers its AI under the spawn-type bits of its aiType
 * (package_keys.h). A bit no constant names is a key a package chose for its
 * own AI, and independent packages can choose the same one: in stable source
 * order the first keeps it, each later distinct registration moves to a free
 * key, and every spawn-type site of that package's declarations follows in
 * derived output (authored files are never rewritten). Equal registrations
 * share their key.
 *
 * With the native private-key contract (native->private_keys) stock requests
 * never select a private key. Every package row that is not exactly a stock
 * row takes one: a changed stock row becomes IMP's bits with its key (one
 * number, the only spelling of an unnamed bit the engine reads) beside the
 * kept stock row, so the variant stays an imp to every type consumer, the
 * package's own IMP requests move to its key, and stock content (native-only
 * maps included) keeps its stock AI. A row the package adds with named bits
 * only takes a key that the package's requests for those bits include. Stock
 * rows a package leaves out stay for everyone. Past bits 19-27 keys are
 * marked multi-bit keys, so a later registration never shares one.
 *
 * Without the contract the engine's intersection decides: packages changing
 * one stock row differently each gain an unnamed bit beside the stock type,
 * an uncontended change applies to the stock key as authored, and past the
 * unnamed bits a later registration keeps its authored key and shares its
 * dispatch, which the library accounts. */
/* One custom bit of a package row, its bound AI token and the named bits
 * beside it in that row. */
typedef struct pl_key_row { size_t scope; unsigned bit; char *identity; uint32_t named; } pl_key_row;
typedef struct pl_keys { pl_key_row *rows; size_t count, capacity; } pl_keys;
/* A package conductor row with named bits only that is not a stock row: a
 * changed stock row (its aiType names one) or a new row. identifying holds
 * the bits only that stock row carries, target the set of keys it receives.
 * Each authored bit keeps its own identity even when several select this row;
 * saved map flags must remain independently reversible. */
typedef struct pl_key_change {
    size_t scope, unit;
    char *key, *identity;
    uint32_t bits, identifying, target;
    int replaces;
    uint32_t targets[SH_PACKAGE_KEY_BITS];
} pl_key_change;
typedef struct pl_key_changes { pl_key_change *rows; size_t count, capacity; } pl_key_changes;
static void pl_keys_free(pl_keys *keys)
{
    for (size_t i = 0; i < keys->count; i++) free(keys->rows[i].identity);
    free(keys->rows); memset(keys, 0, sizeof(*keys));
}
static void pl_key_changes_free(pl_key_changes *changes)
{
    for (size_t i = 0; i < changes->count; i++) { free(changes->rows[i].key); free(changes->rows[i].identity); }
    free(changes->rows); memset(changes, 0, sizeof(*changes));
}
static int pl_keys_add(pl_keys *keys, size_t scope, unsigned bit, const char *identity, size_t length,
    uint32_t named)
{
    char *copy;
    if (keys->count == keys->capacity) {
        size_t next = keys->capacity ? keys->capacity * 2 : 16;
        pl_key_row *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(keys->rows, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        keys->rows = grown; keys->capacity = next;
    }
    if (length == SIZE_MAX || !(copy = malloc(length + 1))) return 0;
    memcpy(copy, identity, length); copy[length] = 0;
    keys->rows[keys->count++] = (pl_key_row){scope, bit, copy, named}; return 1;
}
static int pl_key_note(pl_build *b, const char *line)
{
    size_t used = b->key_notes ? strlen(b->key_notes) : 0, add = strlen(line);
    char *grown = add < SIZE_MAX - used - 2 ? realloc(b->key_notes, used + add + 2) : NULL;
    if (!grown) return 0;
    memcpy(grown + used, line, add); grown[used + add] = '\n'; grown[used + add + 1] = 0;
    b->key_notes = grown; return 1;
}
typedef struct pl_key_order { const pl_build *build; const sh_package_sources *sources; } pl_key_order;
/* Stable source order: delivery tree bytes, then component path. */
static int pl_scope_order(void *context, const void *left, const void *right)
{
    const pl_key_order *order = context;
    const pl_build *b = order->build;
    size_t x = *(const size_t *)left, y = *(const size_t *)right;
    const unsigned char *a = NULL, *z = NULL;
    const char *ra = "", *rz = "";
    int result;
    if (b->contexts) {
        const sh_package_provenance_context *p = b->contexts->contexts + x, *q = b->contexts->contexts + y;
        if (p->tree_count) a = p->trees[0];
        if (q->tree_count) z = q->trees[0];
        if (p->component) ra = p->component;
        if (q->component) rz = q->component;
    } else {
        const sh_package_component *p = order->sources->components + b->views[x].component;
        const sh_package_component *q = order->sources->components + b->views[y].component;
        a = sh_package_source_tree(order->sources, p->owner); z = sh_package_source_tree(order->sources, q->owner);
        if (p->relative) ra = p->relative;
        if (q->relative) rz = q->relative;
    }
    result = a && z ? memcmp(a, z, 32) : a ? -1 : z ? 1 : 0;
    if (!result) result = strcmp(ra, rz);
    return result ? result : (x < y ? -1 : x > y);
}
static void pl_scope_sort(const pl_build *b, const sh_package_sources *sources, size_t *order, size_t count,
    const size_t *scopes)
{
    pl_key_order context = {b, sources};
    for (size_t k = 1; k < count; k++)
        for (size_t m = k; m > 0 && pl_scope_order(&context, scopes + order[m - 1], scopes + order[m]) > 0; m--) {
            size_t swap = order[m]; order[m] = order[m - 1]; order[m - 1] = swap;
        }
}
static const char *pl_scope_label(const pl_build *b, const sh_package_sources *sources, size_t scope);
/* Append the contributions a combination gap involves to its diagnostic.
 * Product and stock parts compose first, then contributions one at a time in
 * stable source order; the first that cannot join those before it takes part,
 * with the earlier contribution it cannot compose with alone when there is
 * one. This names causes only: no owner is judged by it. */
static void pl_joint_participants(const pl_build *b, const sh_package_sources *sources,
    const size_t *members, const size_t *part_member, const sh_decl_source *parts, size_t part_count,
    pl_compose_probe probe, void *context, char *error, size_t capacity)
{
    size_t *order = calloc(part_count ? part_count : 1, sizeof(*order));
    size_t *scopes = calloc(part_count ? part_count : 1, sizeof(*scopes));
    sh_decl_source *trial = calloc(part_count ? part_count : 1, sizeof(*trial));
    size_t *picked = calloc(part_count ? part_count : 1, sizeof(*picked));
    size_t product = 0, authored, accepted = 0, later = SIZE_MAX, earlier = SIZE_MAX;
    if (!order || !scopes || !trial || !picked) goto done;
    for (size_t k = 0; k < part_count; k++) {
        scopes[k] = b->contributions->inputs.scopes[members[part_member[k]]];
        if (pl_member_owner(b, sources, members[part_member[k]]) == SIZE_MAX) order[product++] = k;
    }
    authored = product;
    for (size_t k = 0; k < part_count; k++)
        if (pl_member_owner(b, sources, members[part_member[k]]) != SIZE_MAX) order[authored++] = k;
    {
        pl_key_order stable = {b, sources};
        for (size_t k = product + 1; k < part_count; k++)
            for (size_t m = k; m > product && pl_scope_order(&stable, scopes + order[m - 1], scopes + order[m]) > 0; m--) {
                size_t swap = order[m]; order[m] = order[m - 1]; order[m - 1] = swap;
            }
    }
    for (size_t i = 0; i < part_count; i++) {
        trial[accepted] = parts[order[i]]; picked[accepted] = order[i];
        int status = probe(context, trial, picked, accepted + 1);
        if (status > 0) { accepted++; continue; }
        if (!status && i >= product) later = i;
        break;
    }
    /* The earlier contribution the later one cannot compose with alone. */
    for (size_t i = product; later != SIZE_MAX && i < later && earlier == SIZE_MAX; i++) {
        size_t n = 0;
        for (size_t k = 0; k < product; k++) { picked[n] = order[k]; trial[n++] = parts[order[k]]; }
        picked[n] = order[i]; trial[n++] = parts[order[i]];
        picked[n] = order[later]; trial[n++] = parts[order[later]];
        if (!probe(context, trial, picked, n)) earlier = i;
    }
    if (later != SIZE_MAX) {
        size_t used = strlen(error);
        const char *second = pl_scope_label(b, sources, scopes[order[later]]);
        if (earlier != SIZE_MAX)
            snprintf(error + used, capacity - used, "; %s and %s cannot compose together (product coverage)",
                pl_scope_label(b, sources, scopes[order[earlier]]), second);
        else
            snprintf(error + used, capacity - used, "; %s cannot compose with earlier contributions (product coverage)",
                second);
    }
done:
    free(order); free(scopes); free(trial); free(picked);
}
static const char *pl_scope_label(const pl_build *b, const sh_package_sources *sources, size_t scope)
{
    if (b->contexts || scope >= b->authored_count || b->views[scope].component >= sources->component_count)
        return "a map source";
    const sh_package_component *component = sources->components + b->views[scope].component;
    return component->descriptor.name[0] ? component->descriptor.name : sources->packages[component->owner].name;
}
/* Composition tokens are private graph identities, not useful author-facing
 * names. Prefer this source's spelling when an exact class has several names. */
static const char *pl_authored_name(const pl_contributions *c, size_t scope, const char *family, const char *name)
{
    for (size_t k = 0; k < c->token_count; k++) if (!strcmp(c->tokens[k], name)) {
        const sh_package_link_unit *unit = c->inputs.units + c->classes.representatives[k];
        const sh_package_link_unit *own = NULL, *shared = NULL;
        if (strcmp(unit->family, family)) return name;
        for (size_t i = 0; !own && i < c->inputs.count; i++)
            if (c->inputs.scopes[i] == scope && c->classes.nodes[i] == k) own = c->inputs.units + i;
        /* Or a shared stock unit the scope binds instead of its own copy (R-344 stage A): the first in name order. */
        for (size_t j = 0, n = sh_package_source_link_scope_shared(c->source, scope); !shared && j < n; j++) {
            size_t i = sh_package_source_link_scope_shared_unit(c->source, scope, j);
            if (i < c->classes.node_count && c->classes.nodes[i] == k) shared = c->inputs.units + i;
        }
        if (own && shared) {
            int order = strcmp(own->family, shared->family);
            return (order ? order : strcmp(own->name, shared->name)) < 0 ? own->name : shared->name;
        }
        return own ? own->name : shared ? shared->name : unit->name;
    }
    return name;
}
/* One registration row's fields as text. Bound references are already class
 * tokens, so equal text is an equal registration. */
static char *pl_row_text(const sh_decl_node *item)
{
    sh_decl_node view = {0};
    size_t length = 0;
    view.compound = 1; view.children = item->children;
    return sh_decl_tree_write(&view, &length);
}
static int pl_row_key(const sh_decl_node *item, const char **key, size_t *length)
{ return item->compound && sh_decl_tree_literal(sh_decl_tree_member(item, "aiType"), key, length); }
static sh_decl_node *pl_row_find(const sh_decl_node *list, const char *key, size_t length)
{
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next) {
        const char *value = NULL;
        size_t value_length = 0;
        if (pl_row_key(item, &value, &value_length) && value_length == length && !memcmp(value, key, length))
            return (sh_decl_node *)item;
    }
    return NULL;
}
/* A list of the statement the text's resolved state selects (pl_registration_state). */
static sh_decl_node *pl_list(const sh_decl_node *tree, const char *state, const char *name)
{
    int update = 0;
    const sh_decl_node *edit = tree ? sh_decl_entity_state_statement(tree, state, &update) : NULL;
    if (edit && !edit->compound) edit = NULL;
    const sh_decl_node *list = edit ? sh_decl_tree_member(edit, name) : NULL;
    return list && list->compound ? (sh_decl_node *)list : NULL;
}
static sh_decl_node *pl_node_clone(const sh_decl_node *node)
{
    sh_decl_node *copy = calloc(1, sizeof(*copy)), **tail;
    if (!copy) return NULL;
    copy->compound = node->compound; copy->reset = node->reset; copy->assignment = node->assignment;
    if ((node->key && !(copy->key = _strdup(node->key))) || (node->value && !(copy->value = _strdup(node->value))) ||
        (node->annotation && !(copy->annotation = _strdup(node->annotation)))) { sh_decl_tree_free(copy); return NULL; }
    tail = &copy->children;
    for (const sh_decl_node *child = node->children; child; child = child->next) {
        if (!(*tail = pl_node_clone(child))) { sh_decl_tree_free(copy); return NULL; }
        tail = &(*tail)->next;
    }
    return copy;
}
/* Restate a list's item keys and num after rows move. */
static int pl_list_renumber(sh_decl_node *list)
{
    sh_decl_node *num = NULL;
    size_t index = 0;
    char text[32], *copy;
    for (sh_decl_node *item = list->children; item; item = item->next) {
        if (!strcmp(item->key, "num")) { num = item; continue; }
        snprintf(text, sizeof(text), "item[%zu]", index++);
        if (!(copy = _strdup(text))) return 0;
        free(item->key); item->key = copy;
    }
    if (!num) {
        if (!(num = calloc(1, sizeof(*num))) || !(num->key = _strdup("num"))) { free(num); return 0; }
        num->assignment = 1; num->next = list->children; list->children = num;
    }
    snprintf(text, sizeof(text), "%zu", index);
    if (!(copy = _strdup(text))) return 0;
    free(num->value); num->value = copy; return 1;
}
/* The stock member of a package declaration's group, when the library has one. */
static size_t pl_key_base(const pl_build *b, size_t unit)
{
    const pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + unit;
    size_t found = SIZE_MAX;
    /* Compiler clones follow the source link's frozen units, and a clone
     * keeps its original's scope and name: the first match is the original,
     * which the link's own index finds. */
    return sh_package_source_link_find_unit(c->source, b->count - 1, input->family, input->name, &found) ?
        found : SIZE_MAX;
}

/* Conductor texts are entity declarations read ordered: a native-valid repeat
 * elsewhere keeps its occurrences and order. A repeated edit block or
 * registration list has no modelled composition and stays unread here (the
 * contribution projection reports it as coverage). */
static sh_decl_node *pl_entity_parse(sh_decl_source text, const char *state, char *error, size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse_ordered(text, error, capacity);
    int repeated = 0, update = 0;
    /* The registration statement is the one the resolved state selects
     * (sh_decl_entity_state_statement). In update mode every statement merges
     * in order by first match, so the effective rows are the collapsed view
     * (sh_decl_entity_state_collapse); a rewrite publishes into it and the
     * authored bytes stay in their source unit. In the first stateful layer
     * later statements are inert, but a list repeated inside the first one is
     * an appended sequence, which is not modelled here. */
    int collapsed = tree ? sh_decl_entity_state_collapse(tree, state) : 1;
    if (collapsed != 1) {
        if (error && capacity) snprintf(error, capacity, collapsed ? "registration state allocation failed" :
            "a repeated registration state mixes spellings or value kinds (product coverage)");
        sh_decl_tree_free(tree); return NULL;
    }
    const sh_decl_node *member = tree ? sh_decl_entity_state_statement(tree, state, &update) : NULL;
    for (const sh_decl_node *list = member && member->compound && !update ? member->children : NULL; list && !repeated; list = list->next)
        for (const sh_decl_node *later = list->next; later && !repeated; later = later->next)
            repeated = !strcmp(later->key, list->key) &&
                (!strcmp(list->key, "aiTypeList") || !strcmp(list->key, "globalAIMods"));
    if (repeated) {
        if (error && capacity) snprintf(error, capacity, "a registration list repeated in a first stateful layer is not modelled");
        sh_decl_tree_free(tree); return NULL;
    }
    return tree;
}

/* A registration table is a dependency even though a request carries only a
 * numeric enum. Keep an immutable private copy before the shared editor table
 * composes. Its AI edges and the reverse classification edges form an ordinary
 * linker component, so equal programs dedupe and different programs cannot
 * classify each other's AI. The copy is compiler output, never an authored file. */
static int pl_spawn_edge(pl_contributions *c, size_t unit, const char *slot, size_t target)
{
    sh_package_link_unit *input = c->inputs.units + unit;
    for (size_t i = 0; i < input->implicit_count; i++)
        if (!strcmp(input->implicit[i].slot, slot)) return input->implicit[i].target == target;
    size_t count = input->implicit_count;
    if (count >= SIZE_MAX / sizeof(sh_package_link_edge) - 1) return 0;
    sh_package_link_edge *edges = calloc(count + 1, sizeof(*edges));
    if (!edges) return 0;
    for (size_t i = 0; i <= count; i++) {
        edges[i].slot = _strdup(i == count ? slot : input->implicit[i].slot);
        edges[i].target = i == count ? target : input->implicit[i].target;
        if (!edges[i].slot) {
            for (size_t j = 0; j < i; j++) free((char *)edges[j].slot);
            free(edges); return 0;
        }
    }
    if (c->implicit[unit]) {
        for (size_t i = 0; i < count; i++) free((char *)c->implicit[unit][i].slot);
        free(c->implicit[unit]);
    }
    if (!pl_unit_keep(c, unit)) {
        for (size_t i = 0; i <= count; i++) free((char *)edges[i].slot);
        free(edges); return 0;
    }
    c->implicit[unit] = edges; input->implicit = edges; input->implicit_count++;
    sh_package_link_reuse_invalidate(c->reuse, unit); c->dirty = 1; return 1;
}
static int pl_spawn_site(void *context, const char *slot, const char *value, size_t length, const char **replacement)
{
    (void)slot; (void)value; (void)length; (void)replacement;
    *(int *)context = 1; return 0;
}
static int pl_key_declaration(const sh_package_link_unit *unit)
{
    if (unit->file_count != 1 || !strcmp(unit->family, "renderprog") ||
        sh_decl_native_binding_family(unit->family, NULL)) return 0;
    /* Captured stock text has no compiler type field; its canonical decl path
     * still identifies a declaration, with the reader selected by unit family. */
    return unit->files[0]->type || !strncmp(unit->files[0]->engine_path, "generated/decls/", 16);
}
static int pl_spawn_reserve(pl_contributions *c, size_t wanted)
{
    if (wanted <= c->unit_capacity) return 1;
    size_t n = pl_unit_room(wanted), count = c->inputs.count;
    if (!n) return 0;
    pl_contribution_unit *units = calloc(n, sizeof(*units));
    size_t *origins = calloc(n, sizeof(*origins)), *scopes = calloc(n, sizeof(*scopes));
    sh_package_link_edge **implicit = calloc(n, sizeof(*implicit));
    sh_package_link_witness **witnesses = calloc(n, sizeof(*witnesses));
    unsigned char *shared = calloc(n, 1), *global = calloc(n, 1);
    sh_package_link_unit *inputs = units && origins && scopes && implicit && witnesses && shared && global &&
        sh_package_link_reuse_reserve(c->reuse, n) ? sh_package_source_link_lend_units(c->source, n) : NULL;
    if (!inputs) {
        free(units); free(origins); free(scopes); free(implicit); free(witnesses); free(shared); free(global);
        return 0;
    }
    /* Original input sources retain their addresses and unit indices. Owned
     * contribution slots move with this array, so only those captures expire. */
    pl_reuse_refresh(c);
#define PL_GROW(field, next) memcpy(next, c->field, count * sizeof(*next)); free(c->field); c->field = next
    c->inputs.units = inputs; /* the capture grew its table (R-343) */
    PL_GROW(units, units); PL_GROW(origins, origins); PL_GROW(scopes, scopes);
    PL_GROW(implicit, implicit); PL_GROW(witnesses, witnesses); PL_GROW(shared, shared); PL_GROW(global, global);
#undef PL_GROW
    c->inputs.scopes = c->scopes; c->unit_capacity = n;
    for (size_t i = 0; i < count; i++) if (c->units[i].owned) c->inputs.units[i].files = &c->units[i].file;
    PL_UNIT_GREW();
    return 1;
}
static size_t pl_spawn_clone(pl_build *b, const sh_package_sources *sources, size_t origin,
    const char *body, size_t length, const char *role, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    if (!pl_spawn_reserve(c, c->inputs.count + 1)) return SIZE_MAX;
    static unsigned char marker[] = "immutable spawn registration";
    static sh_compiled_resource witness = {.engine_path = "compiler/spawn-registration", .body = marker,
        .body_length = sizeof(marker) - 1, .source = SIZE_MAX};
    size_t clone = c->inputs.count++;
    const sh_package_link_unit *input = c->inputs.units + origin;
    c->origins[clone] = origin; c->scopes[clone] = c->scopes[origin]; c->inputs.units[clone] = *input;
    sh_package_link_unit *copy = c->inputs.units + clone;
    copy->original_binding = 0; copy->stable_name = 0;
    c->witnesses[clone] = calloc(input->witness_count + 1, sizeof(*c->witnesses[clone]));
    if (!c->witnesses[clone]) return SIZE_MAX;
    if (input->witness_count) memcpy(c->witnesses[clone], input->witnesses,
        input->witness_count * sizeof(*input->witnesses));
    c->witnesses[clone][input->witness_count] = (sh_package_link_witness){role, &witness};
    copy->witnesses = c->witnesses[clone]; copy->witness_count++;
    if (!pl_contribution_stage(b, sources, clone, body, length, NULL, 0, c->units + clone, error, capacity)) return SIZE_MAX;
    copy->compilation = c->units[clone].owned; copy->files = &c->units[clone].file;
    c->dirty = 1; return clone;
}
#ifdef SH_PACKAGE_LIBRARY_TESTING
static int pl_key_memo(void) { return g_key_memo; }
#else
static int pl_key_memo(void) { return 1; }
#endif
typedef struct pl_key_recording { pl_key_scan *scan; size_t capacity; int failed; } pl_key_recording;
static int pl_key_record(void *context, const char *slot, const char *value, size_t length, const char **replacement)
{
    pl_key_recording *r = context;
    (void)replacement;
    if (r->scan->count == r->capacity) {
        size_t next = r->capacity ? r->capacity * 2 : 4;
        pl_key_site *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(r->scan->sites, next * sizeof(*grown)) : NULL;
        if (!grown) { r->failed = 1; return -1; }
        r->scan->sites = grown; r->capacity = next;
    }
    pl_key_site site = {slot ? _strdup(slot) : NULL, length < SIZE_MAX ? malloc(length + 1) : NULL, length};
    if ((slot && !site.slot) || !site.value) { free(site.slot); free(site.value); r->failed = 1; return -1; }
    if (length) memcpy(site.value, value, length);
    site.value[length] = 0;
    r->scan->sites[r->scan->count++] = site; return 0;
}
static size_t pl_key_scan_slot(pl_key_scan *const *slots, size_t capacity, const unsigned char identity[32], int token_text)
{
    uint64_t hash; memcpy(&hash, identity, sizeof(hash));
    size_t at = (size_t)(hash ^ (uint64_t)token_text) & (capacity - 1);
    while (slots[at] && (slots[at]->token_text != token_text || memcmp(slots[at]->identity, identity, 32)))
        at = (at + 1) & (capacity - 1);
    return at;
}
static int pl_key_scan_store(pl_contributions *c, pl_key_scan *scan)
{
    if (c->key_scan_count + 1 > c->key_scan_capacity / 2) {
        size_t capacity = c->key_scan_capacity ? c->key_scan_capacity * 2 : 1024;
        pl_key_scan **slots = capacity < SIZE_MAX / sizeof(*slots) ? calloc(capacity, sizeof(*slots)) : NULL;
        if (!slots) return 0;
        for (size_t i = 0; i < c->key_scan_capacity; i++) if (c->key_scans[i])
            slots[pl_key_scan_slot(slots, capacity, c->key_scans[i]->identity, c->key_scans[i]->token_text)] = c->key_scans[i];
        free(c->key_scans); c->key_scans = slots; c->key_scan_capacity = capacity;
    }
    c->key_scans[pl_key_scan_slot(c->key_scans, c->key_scan_capacity, scan->identity, scan->token_text)] = scan;
    c->key_scan_count++; return 1;
}
/* The registration-key sites of an untouched unit's token text (token_text)
 * or captured bytes, scanned once for its whole binding class. Units of one
 * class have the same bytes and the same bound target classes, hence the same
 * token text, and parents of the same class name (composition never changes
 * class or inherit fields), so the reader reports the same sites in the same
 * order for each. NULL when there is no reusable scan (a composed unit, or a
 * scan that failed or could not be recorded): the caller binds the unit itself
 * exactly as without the memo, and reports any failure the same way. */
static const pl_key_scan *pl_key_scan_of(pl_build *b, size_t unit, int token_text)
{
    pl_contributions *c = b->contributions;
    char error[512];
    if (!pl_key_memo() || unit >= c->inputs.count || c->units[unit].owned || unit >= c->classes.node_count ||
        !c->classes.identities) return NULL;
    const unsigned char *identity = c->classes.identities[c->classes.nodes[unit]];
    if (c->key_scan_capacity) {
        pl_key_scan *found = c->key_scans[pl_key_scan_slot(c->key_scans, c->key_scan_capacity, identity, token_text)];
        if (found) { b->key_scan_hits++; return found; }
    }
    pl_key_scan *scan = calloc(1, sizeof(*scan));
    if (!scan) return NULL;
    memcpy(scan->identity, identity, 32); scan->token_text = token_text;
    const sh_package_link_unit *input = c->inputs.units + unit;
    size_t length = 0, output_length = 0;
    unsigned char *output = NULL;
    unsigned char *body = token_text ? (unsigned char *)pl_token_text(c, unit, &length, error, sizeof(error)) :
        sh_package_compilation_read(input->compilation, input->files[0], SIZE_MAX, &length, error, sizeof(error));
    pl_key_recording recording = {scan, 0, 0};
    scan->status = body ? sh_package_source_link_bind_keys(c->source, c->origins[unit], body, length,
        pl_key_record, &recording, &output, &output_length, error, sizeof(error)) : -1;
    free(body); free(output);
    if (scan->status < 0 || recording.failed || !pl_key_scan_store(c, scan)) { pl_key_scan_free(scan); return NULL; }
    b->key_scans++;
    return scan;
}
static int pl_spawn_capture(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    size_t original_count = c->inputs.count;
    sh_package_key_space space = {0};
    int reflected = sh_package_source_link_key_space(c->source, &space);
    if (reflected < 0) return 0;
    if (!reflected || !b->native || !b->native->private_keys || !sh_package_key_space_private(&space)) return 1;
    for (size_t i = 0; i < original_count; i++) {
        sh_package_link_unit *input = c->inputs.units + i;
        size_t scope = c->inputs.scopes[i], length = 0, baseline;
        if (scope >= b->authored_count || input->file_count != 1 || strcmp(input->family, "entitydef")) continue;
        baseline = pl_key_base(b, i);
        if (baseline != SIZE_MAX && c->classes.nodes[baseline] == c->classes.nodes[i]) continue;
        unsigned char *body = sh_package_compilation_read(input->compilation, input->files[0], SIZE_MAX,
            &length, error, capacity);
        if (!body) return 0;
        int candidate = 0;
        for (size_t at = 0; !candidate && at + 10 <= length; at++) candidate = !memcmp(body + at, "aiTypeList", 10);
        if (!candidate) { free(body); continue; }
        /* Each text's rows live in the statement its own inheritance selects. */
        char *state = NULL, *base_state = NULL;
        int kind = 0;
        if (!pl_registration_state(c, i, (sh_decl_source){(char *)body, length}, &state, &kind, error, capacity)) {
            free(body); return 0;
        }
        sh_package_spawn_registry registry = {0};
        int read = sh_package_spawn_registry_read_state((sh_decl_source){(char *)body, length}, state, &space,
            &registry, error, capacity);
        if (read <= 0) { free(body); free(state); if (read < 0) return 0; continue; }
        if (baseline != SIZE_MAX) {
            size_t own_length = 0, base_length = 0;
            char *own_text = pl_token_text(c, i, &own_length, error, capacity);
            char *base_text = pl_token_text(c, baseline, &base_length, error, capacity);
            sh_package_spawn_registry own = {0}, stock = {0};
            int stated = base_text && pl_registration_state(c, baseline, (sh_decl_source){base_text, base_length},
                &base_state, &kind, error, capacity);
            int own_read = own_text ? sh_package_spawn_registry_read_state((sh_decl_source){own_text, own_length},
                state, &space, &own, error, capacity) : -1;
            int base_read = stated ? sh_package_spawn_registry_read_state((sh_decl_source){base_text, base_length},
                base_state, &space, &stock, error, capacity) : -1;
            int same = own_read == 1 && base_read == 1 && sh_package_spawn_registry_equivalent(&own, &stock);
            free(own_text); free(base_text); sh_package_spawn_registry_free(&own); sh_package_spawn_registry_free(&stock);
            if (same || own_read < 0 || base_read < 0) {
                free(body); free(state); free(base_state); sh_package_spawn_registry_free(&registry);
                if (same) continue;
                return 0;
            }
        }
        size_t clone = pl_spawn_clone(b, sources, i, (char *)body, length, "spawn.registry.private", error, capacity);
        int ok = clone != SIZE_MAX;
        if (ok) {
            const char *name = c->inputs.units[i].name;
            size_t slot_length = strlen(name) + sizeof("spawn.classifier:");
            char *slot = malloc(slot_length);
            if (!slot) ok = 0;
            else {
                snprintf(slot, slot_length, "spawn.classifier:%s", name);
                for (size_t row = 0; ok && row < registry.count; row++) {
                    if (sh_package_spawn_registry_type(&registry, row) != row) continue;
                    size_t ai, class_length = 0;
                    ok = sh_package_source_link_resolve_unit(c->source, i, "entitydef", registry.rows[row].entity,
                        &ai, error, capacity);
                    if (!ok) break;
                    char *classification = sh_package_spawn_registry_classification_state(
                        (sh_decl_source){(char *)body, length}, state, &space, registry.rows[row].entity,
                        &class_length, error, capacity);
                    if (!classification) { ok = 0; break; }
                    size_t original_ai = pl_key_base(b, ai);
                    int same = 0;
                    if (baseline != SIZE_MAX && original_ai != SIZE_MAX && c->classes.nodes[ai] == c->classes.nodes[original_ai]) {
                        const sh_package_link_unit *stock = c->inputs.units + baseline;
                        size_t n = 0, projected_length = 0;
                        unsigned char *text = sh_package_compilation_read(stock->compilation, stock->files[0], SIZE_MAX,
                            &n, error, capacity);
                        char ignored[256] = "";
                        int kept = 0;
                        if (text && !base_state) kept = pl_registration_state(c, baseline,
                            (sh_decl_source){(char *)text, n}, &base_state, &kind, ignored, sizeof(ignored));
                        char *projected = text && (base_state || kept) ? sh_package_spawn_registry_classification_state(
                            (sh_decl_source){(char *)text, n}, base_state, &space, registry.rows[row].entity,
                            &projected_length, ignored, sizeof(ignored)) : NULL;
                        same = projected && class_length == projected_length && !memcmp(classification, projected, class_length);
                        free(text); free(projected);
                    }
                    if (!same) {
                        size_t capsule = pl_spawn_clone(b, sources, i, classification, class_length,
                            "spawn.classifier.private", error, capacity);
                        ok = capsule != SIZE_MAX && pl_spawn_edge(c, ai, slot, capsule);
                    }
                    free(classification);
                }
                snprintf(slot, slot_length, "spawn.registry:%s", name);
                for (size_t unit = 0; ok && unit < original_count; unit++) {
                    const sh_package_link_unit *owner = c->inputs.units + unit;
                    if (unit == i || c->inputs.scopes[unit] != scope || !pl_key_declaration(owner)) continue;
                    int uses = 0, status;
                    const pl_key_scan *scan = pl_key_scan_of(b, unit, 0);
                    if (scan) { status = scan->status; uses = scan->count != 0; }
                    else {
                        size_t n = 0, output_length = 0; unsigned char *output = NULL;
                        unsigned char *text = sh_package_compilation_read(owner->compilation, owner->files[0],
                            SIZE_MAX, &n, error, capacity);
                        status = text ? sh_package_source_link_bind_keys(c->source, unit, text, n,
                            pl_spawn_site, &uses, &output, &output_length, error, capacity) : -1;
                        free(text); free(output);
                    }
                    ok = status >= 0 && (!uses || pl_spawn_edge(c, unit, slot, clone));
                }
                free(slot);
            }
        }
        free(body); free(state); free(base_state);
        sh_package_spawn_registry_free(&registry);
        if (!ok) { if (!*error) snprintf(error, capacity, "cannot retain scoped spawn registrations"); return 0; }
        c->dirty = 1;
    }
    return 1;
}
static int pl_key_change_add(pl_key_changes *changes, const sh_package_key_space *space, size_t scope, size_t unit,
    const sh_decl_node *item, const sh_decl_node *stock, const sh_decl_node *stock_list, const char *key,
    size_t key_length, uint32_t bits)
{
    char *row = pl_row_text(item), *base = stock ? pl_row_text(stock) : NULL, *copy = NULL;
    uint32_t others = 0;
    int ok = 0;
    if (!row || (stock && !base)) goto done;
    ok = 1;
    if (base && !strcmp(row, base)) goto done;
    /* Only bits no other stock row carries identify a stock row's AI; group
     * bits (any zombie) stay shared by every row that carries them. */
    for (const sh_decl_node *other = stock ? stock_list->children : NULL; other; other = other->next) {
        const char *value = NULL;
        size_t length = 0;
        uint32_t other_bits = 0;
        if (other == stock || !pl_row_key(other, &value, &length)) continue;
        if (sh_package_key_bits(space, value, length, &other_bits)) others |= other_bits;
    }
    ok = 0;
    if (!(copy = malloc(key_length + 1))) goto done;
    memcpy(copy, key, key_length); copy[key_length] = 0;
    if (changes->count == changes->capacity) {
        size_t next = changes->capacity ? changes->capacity * 2 : 8;
        pl_key_change *grown = next <= SIZE_MAX / sizeof(*grown) ? realloc(changes->rows, next * sizeof(*grown)) : NULL;
        if (!grown) goto done;
        changes->rows = grown; changes->capacity = next;
    }
    changes->rows[changes->count++] = (pl_key_change){scope, unit, copy, row, bits,
        stock ? bits & ~others & space->named : 0, 0, stock != NULL};
    copy = row = NULL; ok = 1;
done:
    free(row); free(base); free(copy);
    return ok;
}
/* Custom-bit rows of every package conductor, each with the bound AI token,
 * and every package row with named bits only that is not a stock row of the
 * same declaration (new rows count only under the private contract). Complete
 * composed contexts are recorded separately for intersection-preserving key
 * substitution; authored numeric values are never a global namespace. */
static int pl_key_rows(pl_build *b, const sh_package_key_space *space, int private_keys, pl_keys *keys,
    pl_key_changes *changes, unsigned char *conductors,
    unsigned char *legacy_present, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        size_t scope = c->inputs.scopes[i], length = 0, base, base_length = 0;
        int stated = 0, ok = 1, legacy = scope < b->authored_count && !pl_modern(b, scope);
        if (c->origins[i] != i || scope >= b->authored_count || unit->file_count != 1 ||
            strcmp(unit->family, "entitydef")) continue;
        unsigned char *body = sh_package_compilation_read(unit->compilation, unit->files[0], SIZE_MAX, &length, error, capacity);
        if (!body) return 0;
        /* A cheap byte check precedes parsing: few declarations register AI. */
        for (size_t at = 0; !stated && at + 10 <= length; at++)
            stated = !memcmp(body + at, "aiTypeList", 10) || (at + 12 <= length && !memcmp(body + at, "globalAIMods", 12));
        free(body);
        if (!stated) continue;
        if (legacy) {
            char *text = pl_token_text(c, i, &length, error, capacity), *state = NULL;
            int kind = 0;
            if (!text) return 0;
            if (!pl_registration_state(c, i, (sh_decl_source){text, length}, &state, &kind, error, capacity)) {
                free(text); return 0;
            }
            sh_package_spawn_registry registrations = {0};
            int captured = sh_package_spawn_registry_read_state((sh_decl_source){text, length}, state, space,
                &registrations, error, capacity);
            free(state);
            if (captured == 1 && private_keys) {
                legacy_present[scope] = 1;
            }
            sh_package_spawn_registry_free(&registrations); free(text);
            if (captured < 0) return 0;
            continue;
        }
        conductors[i] = 1;
        char *text = pl_token_text(c, i, &length, error, capacity), *base_text = NULL;
        if (!text) return 0;
        if ((base = pl_key_base(b, i)) != SIZE_MAX && !(base_text = pl_token_text(c, base, &base_length, error, capacity))) {
            free(text); return 0;
        }
        char *state = NULL, *base_state = NULL;
        int kind = 0;
        if (!pl_registration_state(c, i, (sh_decl_source){text, length}, &state, &kind, error, capacity) ||
            (base_text && !pl_registration_state(c, base, (sh_decl_source){base_text, base_length}, &base_state,
                &kind, error, capacity))) {
            free(state); free(text); free(base_text); return 0;
        }
        sh_decl_node *tree = pl_entity_parse((sh_decl_source){text, length}, state, NULL, 0);
        sh_decl_node *stock = base_text ? pl_entity_parse((sh_decl_source){base_text, base_length}, base_state, NULL, 0) : NULL;
        const sh_decl_node *list = pl_list(tree, state, "aiTypeList"), *stock_list = pl_list(stock, base_state, "aiTypeList");
        for (const sh_decl_node *item = list ? list->children : NULL; ok && item; item = item->next) {
            const char *value = NULL, *entity = "";
            size_t value_length = 0, entity_length = 0;
            uint32_t bits = 0, all = 0;
            if (!pl_row_key(item, &value, &value_length)) continue;
            const sh_decl_node *original = pl_row_find(stock_list, value, value_length);
            if (!sh_package_key_custom_bits(space, value, value_length, &bits) ||
                !sh_package_key_bits(space, value, value_length, &all)) continue;
            if (original || (private_keys && !bits && all)) {
                ok = pl_key_change_add(changes, space, scope, i, item, original, stock_list, value, value_length,
                    all & space->named);
                continue;
            }
            if (!bits) continue;
            if (!sh_decl_tree_literal(sh_decl_tree_member(item, "entityDef"), &entity, &entity_length)) entity_length = 0;
            for (unsigned bit = 0; ok && bit < SH_PACKAGE_KEY_BITS; bit++)
                if (bits >> bit & 1u) ok = pl_keys_add(keys, scope, bit, entity, entity_length, all & space->named);
        }
        sh_decl_tree_free(tree); sh_decl_tree_free(stock); free(text); free(base_text); free(state); free(base_state);
        if (!ok) { snprintf(error, capacity, "registration key allocation failed"); return 0; }
    }
    return 1;
}
static int pl_key_row_order(const void *left, const void *right)
{
    const pl_key_row *x = left, *y = right;
    if (x->bit != y->bit) return x->bit < y->bit ? -1 : 1;
    if (x->scope != y->scope) return x->scope < y->scope ? -1 : 1;
    return strcmp(x->identity, y->identity);
}
static int pl_key_change_order(const void *left, const void *right)
{
    const pl_key_change *x = left, *y = right;
    int result = strcmp(x->key, y->key);
    if (!result) result = strcmp(x->identity, y->identity);
    return result ? result : (x->scope < y->scope ? -1 : x->scope > y->scope);
}
/* One scope's registration of one key: its sorted distinct AI tokens. */
static int pl_key_same(const pl_keys *keys, size_t first, size_t first_end, size_t second, size_t second_end)
{
    size_t i = first, j = second;
    while (i < first_end && j < second_end) {
        if (strcmp(keys->rows[i].identity, keys->rows[j].identity)) return 0;
        const char *a = keys->rows[i].identity, *z = keys->rows[j].identity;
        while (i < first_end && !strcmp(keys->rows[i].identity, a)) i++;
        while (j < second_end && !strcmp(keys->rows[j].identity, z)) j++;
    }
    return i == first_end && j == second_end;
}
/* The package whose component a scope compiles: a library scope, or an
 * ISOLATED context (one tree, one component; an installation increment
 * compiles each new component so). Composed and document contexts combine
 * trees and name no single owner. */
static size_t pl_scope_owner(const pl_build *b, const sh_package_sources *sources, size_t scope)
{
    if (scope >= b->authored_count || b->views[scope].component >= sources->component_count) return SIZE_MAX;
    if (b->contexts && (scope >= b->contexts->context_count ||
        b->contexts->contexts[scope].mode != SH_PACKAGE_CONTEXT_ISOLATED ||
        b->contexts->contexts[scope].tree_count != 1)) return SIZE_MAX;
    return sources->components[b->views[scope].component].owner;
}
/* Failures may name owners: the installed library, or contexts that are all
 * isolated components (an installation increment). */
static int pl_attributable(const sh_package_provenance *contexts)
{
    for (size_t i = 0; contexts && i < contexts->context_count; i++)
        if (contexts->contexts[i].mode != SH_PACKAGE_CONTEXT_ISOLATED || contexts->contexts[i].tree_count != 1) return 0;
    return 1;
}
void sh_package_rejections_free(sh_package_rejections *rejections)
{
    if (!rejections) return;
    free(rejections->items);
    rejections->items = NULL; rejections->count = rejections->capacity = 0;
}
/* Owners of the withdrawals other than the reported one (first_owner), each
 * once, with its typed attribution: SOURCE is malformed in its own bytes,
 * COVERAGE is content this build cannot serve yet. 0 only on allocation
 * failure. */
static int pl_more_rejections(const pl_build *b, const sh_package_sources *sources, size_t first_owner,
    const sh_package_source_withdrawals *withdrawals, sh_package_rejections *out)
{
    if (!out || !withdrawals) return 1;
    for (size_t i = 0; i < withdrawals->count; i++) {
        const sh_package_source_withdrawal *w = withdrawals->items + i;
        size_t owner = pl_scope_owner(b, sources, w->failure.scope);
        int seen = owner == SIZE_MAX || owner == first_owner;
        for (size_t j = 0; !seen && j < out->count; j++) seen = out->items[j].package == owner;
        if (seen) continue;
        if (out->count == out->capacity) {
            size_t capacity = out->capacity ? out->capacity * 2 : 4;
            sh_package_rejection *grown = realloc(out->items, capacity * sizeof(*grown));
            if (!grown) return 0;
            out->items = grown; out->capacity = capacity;
        }
        sh_package_rejection *r = out->items + out->count++;
        r->package = owner;
        r->invalid = r->own_bytes = w->failure.kind == SH_PACKAGE_SOURCE_FAILURE_SOURCE;
        snprintf(r->reason, sizeof(r->reason), "%s%s%s%s", w->reason, w->failure.chain[0] ? " (via " : "",
            w->failure.chain, w->failure.chain[0] ? ")" : "");
    }
    return 1;
}
/* The packages the contribution stage found (pl_contribution_blame), when a
 * judged failure ended it: the first as invalid_package or unserved_package,
 * with its reason as the compile's error, the others as more_rejections. None
 * is remembered as decided by its own bytes: each judgement read stock or
 * composed content too. An environmental or unjudged failure blames nobody. */
static void pl_contribution_report(const pl_build *b, const sh_package_compile_environment *environment,
    char *error, size_t capacity)
{
    const sh_package_rejections *found = &b->contribution_rejections;
    if (!found->count || !b->contribution_verdict) return;
    const sh_package_rejection *first = found->items;
    size_t *slot = first->invalid ? environment->invalid_package : environment->unserved_package;
    sh_package_rejections *more = environment->more_rejections;
    if (!slot) return;
    for (size_t i = 1; more && i < found->count; i++) {
        if (more->count == more->capacity) {
            size_t grown_capacity = more->capacity ? more->capacity * 2 : 4;
            sh_package_rejection *grown = realloc(more->items, grown_capacity * sizeof(*grown));
            if (!grown) { more->count = 0; snprintf(error, capacity, "cannot account further rejected packages"); return; }
            more->items = grown; more->capacity = grown_capacity;
        }
        more->items[more->count++] = found->items[i];
    }
    *slot = first->package;
    snprintf(error, capacity, "%s", first->reason);
}
/* Free keys: private keys in allocation order under the contract, otherwise
 * the lowest unnamed bit. 0 when none is left. A peer's multi-bit keys, group
 * codes and group members are held; no key touches a historical bit. */
typedef struct pl_key_pool {
    int private_keys; uint32_t pool, used, historical; size_t cursor;
    const sh_package_key_set *held;
    sh_package_key_set taken; /* multi-bit keys issued by this compile */
} pl_key_pool;
/* A multi-bit code sharing a bit with a frozen legacy registration. */
static int pl_key_historical(const pl_key_pool *p, uint32_t code)
{ return (code & p->historical) != 0 || (p->historical & SH_PACKAGE_KEY_MARKER && code & SH_PACKAGE_KEY_MARKER); }
static uint32_t pl_key_take(pl_key_pool *p)
{
    if (!p->private_keys) {
        uint32_t free_bits = p->pool & ~p->used, bit = free_bits & (~free_bits + 1u);
        p->used |= bit; return bit;
    }
    for (uint32_t key; (key = sh_package_key_private(p->cursor)) != 0;) {
        p->cursor++;
        /* Marked keys come only from here; single bits skip authored ones. */
        if (key & SH_PACKAGE_KEY_MARKER) {
            if (pl_key_historical(p, key) || (p->held && sh_package_key_set_has(p->held, key))) continue;
            sh_package_key_set_add(&p->taken, key); return key;
        }
        if (!(p->used & key)) { p->used |= key; return key; }
    }
    return 0;
}
/* A peer library's keys, by source tree, component and authored bit. */
typedef struct pl_key_pin { const sh_package_provenance_context *context; uint32_t original, current; } pl_key_pin;
/* held: every multi-bit code a peer keeps (binding keys, group codes and
 * group members); members: its group members that are single bits. */
typedef struct pl_key_pins { pl_key_pin *pins; size_t count; sh_package_key_set held; uint32_t members; } pl_key_pins;
static void pl_key_pins_free(pl_key_pins *pins) { free(pins->pins); memset(pins, 0, sizeof(*pins)); }
static int pl_key_number(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long number;
    if (!text || *text < '0' || *text > '9') return 0;
    number = strtoul(text, &end, 10);
    if (!end || *end || number > 0xfffffffful) return 0;
    *value = (uint32_t)number; return 1;
}
static int pl_key_pins_open(const sh_package_provenance *peer, const sh_package_key_groups *groups,
    pl_key_pins *out)
{
    size_t total = 0;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; peer && i < peer->binding_count; i++)
        if (!strcmp(peer->bindings[i].family, SH_PACKAGE_KEY_FAMILY)) {
            if (peer->bindings[i].context_count > SIZE_MAX - total) return 0;
            total += peer->bindings[i].context_count;
        }
    if (total && (total > SIZE_MAX / sizeof(*out->pins) || !(out->pins = calloc(total, sizeof(*out->pins))))) return 0;
    /* A peer's group codes stay its sets' (equal sets reuse them), and its
     * group members stay its scopes' keys: a changed row's own key appears
     * only inside the group its named bit's binding records. */
    for (uint32_t pattern = 1; groups && pattern < SH_PACKAGE_KEY_PATTERNS; pattern++) {
        if (!groups->defined[pattern]) continue;
        sh_package_key_set_add(&out->held, SH_PACKAGE_KEY_MARKER | (pattern << 19));
        for (size_t index = 0; index < SH_PACKAGE_KEY_CODES; index++) {
            uint32_t member = sh_package_key_code(index);
            if (!sh_package_key_set_has(groups->members + pattern, member)) continue;
            if (member & SH_PACKAGE_KEY_MARKER) sh_package_key_set_add(&out->held, member);
            else out->members |= member;
        }
    }
    for (size_t i = 0; peer && i < peer->binding_count; i++) {
        const sh_package_provenance_binding *binding = peer->bindings + i;
        uint32_t original = 0, current = 0, held;
        if (strcmp(binding->family, SH_PACKAGE_KEY_FAMILY) || !pl_key_number(binding->original, &original) ||
            !pl_key_number(binding->name, &current) || !original || (original & (original - 1u))) continue;
        for (size_t c = 0; c < binding->context_count; c++) {
            const sh_package_provenance_context *context = binding->contexts[c] < peer->context_count ?
                peer->contexts + binding->contexts[c] : NULL;
            if (!context || !context->tree_count) continue;
            out->pins[out->count++] = (pl_key_pin){context, original, current};
        }
        /* The key itself: a custom bit's whole key, a named bit's key beside it. */
        held = original & SH_PACKAGE_KEY_UNNAMED ? current : current & ~original;
        if (held & SH_PACKAGE_KEY_MARKER) sh_package_key_set_add(&out->held, held);
    }
    return 1;
}
static const unsigned char *pl_scope_tree(const pl_build *b, const sh_package_sources *sources, size_t scope,
    const char **component)
{
    if (b->contexts) {
        if (scope >= b->contexts->context_count || b->contexts->contexts[scope].tree_count != 1) return NULL;
        *component = b->contexts->contexts[scope].component ? b->contexts->contexts[scope].component : "";
        return b->contexts->contexts[scope].trees[0];
    }
    if (scope >= b->authored_count || b->views[scope].component >= sources->component_count) return NULL;
    const sh_package_component *owner = sources->components + b->views[scope].component;
    if (!sources->fingerprints || owner->owner >= sources->package_count) return NULL;
    *component = owner->relative ? owner->relative : "";
    return sh_package_source_tree(sources, owner->owner);
}
/* The key a peer holds for this scope's authored bit, when it holds one. */
static int pl_key_pinned(const pl_build *b, const sh_package_sources *sources, const pl_key_pins *pins,
    size_t scope, uint32_t original, uint32_t *current)
{
    const char *component = NULL;
    const unsigned char *tree = pins->count ? pl_scope_tree(b, sources, scope, &component) : NULL;
    for (size_t i = 0; i < pins->count; i++) {
        const sh_package_provenance_context *held = pins->pins[i].context;
        if (pins->pins[i].original != original) continue;
        if (b->contexts) {
            if (scope >= b->contexts->context_count) continue;
            const sh_package_provenance_context *wanted = b->contexts->contexts + scope;
            if (held->mode != wanted->mode || held->tree_count != wanted->tree_count ||
                memcmp(held->trees, wanted->trees, wanted->tree_count * 32) ||
                strcmp(held->component ? held->component : "", wanted->component ? wanted->component : "")) continue;
        } else if (!tree || held->mode != SH_PACKAGE_CONTEXT_ISOLATED || held->tree_count != 1 ||
            memcmp(held->trees[0], tree, 32) || strcmp(held->component ? held->component : "", component)) continue;
        *current = pins->pins[i].current; return 1;
    }
    return 0;
}
static void pl_key_set_remove(sh_package_key_set *set, uint32_t code)
{
    size_t index = sh_package_key_index(code);
    if (index != SIZE_MAX) set->words[index / 64] &= ~(1ull << (index % 64));
}
/* A changed row's own key inside the value its named bit's peer binding
 * records: a group the peer defines, or an unmarked union of single-bit keys
 * (what a library holding free single bits records, which names each of
 * them). Either way the members without the keys this context holds under its
 * other authored bits; exactly one valid member must remain, held directly by
 * no peer binding (another context's key is never borrowed); otherwise 0.
 * A group member is refused when any unmarked binding overlaps it; a union
 * member only when some binding's whole value is that key, because every
 * context sharing the row repeats its key inside its own union. */
static uint32_t pl_key_row_member(const pl_build *b, const sh_package_sources *sources, const pl_key_pins *pins,
    const pl_key_pool *pool, const sh_package_key_groups *peer, size_t scope, unsigned named, uint32_t group)
{
    uint32_t pattern = (group & SH_PACKAGE_KEY_SINGLE) >> 19, found = 0;
    size_t remaining = 0;
    int marked = (group & SH_PACKAGE_KEY_MARKER) != 0;
    sh_package_key_set candidates;
    if (marked) {
        if (!peer || !pattern || !peer->defined[pattern]) return 0;
        candidates = peer->members[pattern];
    } else {
        if (!(group & SH_PACKAGE_KEY_SINGLE)) return 0;
        sh_package_key_decode(NULL, group, &candidates);
    }
    for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
        uint32_t other = 0;
        if (bit == named || !pl_key_pinned(b, sources, pins, scope, 1u << bit, &other)) continue;
        if (other & SH_PACKAGE_KEY_MARKER) pl_key_set_remove(&candidates, other & SH_PACKAGE_KEY_PRIVATE);
        else for (unsigned single = 19; single < 28; single++)
            if (other >> single & 1u) pl_key_set_remove(&candidates, 1u << single);
    }
    for (size_t index = 0; index < SH_PACKAGE_KEY_CODES; index++) {
        uint32_t code = sh_package_key_code(index);
        if (sh_package_key_set_has(&candidates, code)) { found = code; remaining++; }
    }
    if (remaining != 1 || !sh_package_key_private_valid(found) || pl_key_historical(pool, found) ||
        ((found & SH_PACKAGE_KEY_MARKER) && peer && peer->defined[(found & SH_PACKAGE_KEY_SINGLE) >> 19])) return 0;
    for (size_t i = 0; i < pins->count; i++) {
        uint32_t held = pins->pins[i].current & SH_PACKAGE_KEY_PRIVATE;
        if (!marked ? held == found :
            (held & SH_PACKAGE_KEY_MARKER) ? held == found : !(found & SH_PACKAGE_KEY_MARKER) && (held & found)) return 0;
    }
    return found;
}
/* Group codes for sets of several keys that are not single-bit keys only:
 * an equal set keeps the code this compile or its peer gave it; a new set
 * takes the first multi-bit code no key, held code or group uses. */
typedef struct pl_key_groups {
    pl_build *build;
    const pl_key_pool *pool;
    const sh_package_key_groups *peer;
} pl_key_groups;
static int pl_key_group(void *context, const sh_package_key_set *keys, uint32_t *code)
{
    pl_key_groups *g = context;
    pl_build *b = g->build;
    uint32_t found;
    if (!b->key_groups && !(b->key_groups = calloc(1, sizeof(*b->key_groups)))) return -1;
    if ((found = sh_package_key_groups_find(b->key_groups, keys))) { *code = found; return 1; }
    if ((found = sh_package_key_groups_find(g->peer, keys)) && pl_key_historical(g->pool, found)) found = 0;
    for (size_t n = 9; !found; n++) {
        uint32_t candidate = sh_package_key_private(n), pattern;
        if (!candidate) return 0;
        pattern = (candidate & SH_PACKAGE_KEY_SINGLE) >> 19;
        if (sh_package_key_set_has(&g->pool->taken, candidate) || b->key_groups->defined[pattern] ||
            pl_key_historical(g->pool, candidate) ||
            (g->pool->held && sh_package_key_set_has(g->pool->held, candidate))) continue;
        found = candidate;
    }
    if (!sh_package_key_groups_define(b->key_groups, found, keys)) return -1;
    *code = found; return 1;
}
typedef struct pl_key_edit {
    const sh_package_key_space *space;
    const sh_package_key_move *rows, *requests;
    const sh_package_key_encoder *encoder;
    const char *family;
    char *owned;
    int composed;
} pl_key_edit;
/* Sites that name AI types to classify AI rather than to ask for one: the AI
 * filter's mask, the conductor's allowed-AI mask and a kill score's type.
 * Every AI of a named type meets them, stock rows and package variants alike
 * (a variant row keeps its named bits), so they keep the named bits a
 * package's own requests trade for its key. */
static int pl_key_classifies(const char *family, const char *slot)
{
    const char *field = slot;
    for (const char *at = slot; at && *at; at++) if (*at == '.' || *at == ':' || *at == '/') field = at + 1;
    if (!field) return 0;
    return !strcmp(field, "aiFilter") || !strcmp(field, "allowedAIMask") ||
        (family && !strcmp(family, "snapaiscore") && !strcmp(field, "aiType"));
}
/* Requests and modifier masks name this scope's registrations. Classification
 * retains native categories; pl_key_split adds changed named rows' keys. */
static int pl_key_rewrite(void *context, const char *slot, const char *value, size_t length, const char **replacement)
{
    pl_key_edit *edit = context;
    size_t out_length = 0; int failed = 0;
    int row = slot && (strstr(slot, "aiTypeList") || (!edit->encoder && strstr(slot, "globalAIMods")));
    char *moved = sh_package_key_relocate(edit->space, value, length,
        row || pl_key_classifies(edit->family, slot) ? edit->rows : edit->requests, edit->encoder,
        &out_length, &failed);
    if (failed) return -1;
    if (edit->composed && slot && strstr(slot, "aiTypeList")) {
        uint32_t original = 0, relocated = 0;
        if (!sh_package_key_bits(edit->space, value, length, &original) ||
            !sh_package_key_bits(edit->space, moved ? moved : value,
                moved ? out_length : length, &relocated)) { free(moved); return -1; }
        /* Native score/category consumers still see the authored named bits.
         * Requests and modifier/filter masks use only the scoped key sets. */
        char *classified = malloc(16);
        if (!classified) { free(moved); return -1; }
        snprintf(classified, 16, "%u", relocated | (original & edit->space->named));
        free(moved); moved = classified;
    }
    if (!moved) return 0;
    free(edit->owned); edit->owned = moved; *replacement = moved; return 1;
}
/* Insert copies of the stock rows no package row carries, so a package never
 * removes a stock registration or modifier for anyone else. Each follows the
 * stock row before it (the first leads the rows), as in the stock list: a
 * partial list's own rows stay after the stock rows, like a complete list's. */
static sh_decl_node *pl_row_find_mask(const sh_decl_node *list, const sh_package_key_space *space,
    const char *key, size_t length)
{
    uint32_t wanted = 0;
    if (!space || !sh_package_key_bits(space, key, length, &wanted)) return pl_row_find(list, key, length);
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next) {
        const char *value = NULL;
        size_t value_length = 0;
        uint32_t mask = 0;
        if (pl_row_key(item, &value, &value_length) &&
            sh_package_key_bits(space, value, value_length, &mask) && mask == wanted)
            return (sh_decl_node *)item;
    }
    return NULL;
}
static int pl_key_restore(sh_decl_node *list, const sh_decl_node *stock,
    const sh_package_key_space *space, int *changed)
{
    sh_decl_node *previous = NULL;
    for (const sh_decl_node *row = stock ? stock->children : NULL; row; row = row->next) {
        const char *key = NULL;
        size_t length = 0;
        sh_decl_node *held;
        if (!pl_row_key(row, &key, &length)) continue;
        /* A numeric mask and its reflected names designate the same row. A
         * restored symbolic spelling must not precede that authored winner. */
        if ((held = pl_row_find_mask(list, space, key, length))) { previous = held; continue; }
        sh_decl_node *copy = pl_node_clone(row), **link = previous ? &previous->next : &list->children;
        if (!copy) return 0;
        copy->next = *link; *link = copy; previous = copy; *changed = 1;
    }
    return 1;
}
/* Add named rows' private keys beside their native classification bits and
 * restore stock rows. Private modifier masks were already translated through
 * the scope's complete membership; copying them per row would broaden their
 * meaning. The non-private contract still splits changed stock modifiers. */
static int pl_key_split(const sh_package_key_space *space, int private_keys, int restore, const char *text, size_t length,
    const char *state, const char *base, size_t base_length, const char *base_state, const pl_key_changes *changes,
    size_t unit, char **out, size_t *out_length, char *error, size_t capacity)
{
    sh_decl_node *tree = pl_entity_parse((sh_decl_source){text, length}, state, error, capacity);
    sh_decl_node *stock = pl_entity_parse((sh_decl_source){base, base_length}, base_state, error, capacity);
    sh_decl_node *list = pl_list(tree, state, "aiTypeList"), *mods = pl_list(tree, state, "globalAIMods");
    const sh_decl_node *stock_list = pl_list(stock, base_state, "aiTypeList");
    const sh_decl_node *stock_mods = pl_list(stock, base_state, "globalAIMods");
    int changed = 0, failed = !tree || !stock;
    *out = NULL; *out_length = 0;
    for (size_t i = 0; !failed && list && i < changes->count; i++) {
        const pl_key_change *change = changes->rows + i;
        size_t key_length = strlen(change->key), size;
        sh_decl_node *row = change->unit == unit && change->target ? pl_row_find(list, change->key, key_length) : NULL;
        char spelled[16], *value;
        uint32_t stock_bits = 0;
        if (!row) continue;
        snprintf(spelled, sizeof(spelled), "%u", change->target);
        sh_decl_node *type = (sh_decl_node *)sh_decl_tree_member(row, "aiType");
        /* The engine reads unnamed bits only as one number: the row's own bits
         * and its key together. */
        sh_package_key_bits(space, change->key, key_length, &stock_bits);
        size = 16;
        if (!(value = malloc(size))) { failed = 1; break; }
        snprintf(value, size, "\"%u\"", stock_bits | change->target);
        free(type->value); type->value = value; changed = 1;
        if (private_keys) continue;
        for (sh_decl_node *mod = mods ? mods->children : NULL; mod; mod = mod->next) {
            const char *key = NULL;
            size_t mod_length = 0;
            uint32_t mask = 0;
            if (!pl_row_key(mod, &key, &mod_length) || !sh_package_key_bits(space, key, mod_length, &mask) ||
                !(mask & stock_bits & space->named)) continue;
            {
                if (!change->identifying || (mask & ~change->identifying)) continue;
                const sh_decl_node *stock_mod = pl_row_find(stock_mods, key, mod_length);
                char *mine = pl_row_text(mod), *theirs = stock_mod ? pl_row_text(stock_mod) : NULL;
                int same = mine && theirs && !strcmp(mine, theirs);
                failed = !mine || (stock_mod && !theirs);
                free(mine); free(theirs);
                if (failed) break;
                if (same) continue;
                sh_decl_node *kept = stock_mod ? pl_node_clone(stock_mod) : NULL;
                size = strlen(spelled) + 3;
                if ((stock_mod && !kept) || !(value = malloc(size))) { sh_decl_tree_free(kept); failed = 1; break; }
                snprintf(value, size, "\"%s\"", spelled);
                type = (sh_decl_node *)sh_decl_tree_member(mod, "aiType");
                free(type->value); type->value = value;
                if (kept) { kept->next = mod->next; mod->next = kept; }
                break;
            }
        }
    }
    if (!failed && restore && ((list && !pl_key_restore(list, stock_list, space, &changed)) ||
        (mods && !pl_key_restore(mods, stock_mods, space, &changed)))) failed = 1;
    if (!failed && changed && ((list && !pl_list_renumber(list)) || (mods && !pl_list_renumber(mods)) ||
        !(*out = sh_decl_tree_write(tree, out_length)))) failed = 1;
    if (failed && error && capacity && !*error) snprintf(error, capacity, "registration rows could not be split");
    sh_decl_tree_free(tree); sh_decl_tree_free(stock);
    if (failed) { free(*out); *out = NULL; *out_length = 0; }
    return !failed;
}
/* Rewrite every spawn-type site of one package scope through its moves. */
static int pl_key_relocate_scope(pl_build *b, const sh_package_sources *sources, size_t scope,
    const sh_package_key_space *space, int private_keys, const sh_package_key_move *rows,
    const sh_package_key_move *requests, const sh_package_key_encoder *encoder, const pl_key_changes *changes,
    const unsigned char *conductors, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    int moves = 0;
    for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++)
        if (rows[bit].kind != SH_PACKAGE_KEY_KEEP || requests[bit].kind != SH_PACKAGE_KEY_KEEP) moves = 1;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        pl_contribution_unit pending = {0};
        pl_key_edit edit = {space, rows, requests, encoder, unit->family, NULL,
            private_keys && !pl_modern(b, scope)};
        unsigned char *out = NULL;
        size_t length = 0, out_length = 0, base = SIZE_MAX;
        int split = 0;
        if (c->inputs.scopes[i] != scope || unit->file_count != 1) continue;
        /* Derived stock declarations can reference the package's changed AI
         * too; their registration and request sites need the same relocation. */
        if (!pl_key_declaration(unit)) continue;
        for (size_t k = 0; k < changes->count; k++)
            if (changes->rows[k].unit == c->origins[i] && changes->rows[k].target) split = 1;
        /* Without moves only a conductor copy needs its stock rows restored. */
        if (private_keys && conductors[i]) split = 1;
        if (!moves && !split) continue;
        if (split && (base = pl_key_base(b, i)) == SIZE_MAX) split = 0;
        /* An untouched unit none of whose key sites this scope's moves change
         * keeps its text: the full rewrite below would return the same bytes.
         * The same resolver sees each site in the same order, so key groups
         * are defined as before. Splits and bitflag inspectors rewrite more. */
        if (!split && strcmp(unit->family, "snappropertyinspector_bitflag")) {
            const pl_key_scan *scan = pl_key_scan_of(b, i, 1);
            if (scan) {
                int moved = 0;
                for (size_t k = 0; k < scan->count && !moved; k++) {
                    const char *replacement = NULL;
                    moved = pl_key_rewrite(&edit, scan->sites[k].slot, scan->sites[k].value, scan->sites[k].length,
                        &replacement) != 0;
                }
                free(edit.owned); edit.owned = NULL;
                if (!moved) { b->key_skipped++; continue; }
            }
        }
        pl_kept_refs kept = {0};
        char *text = pl_token_text_kept(c, i, &length, &kept, error, capacity), *inspected = NULL, *divided = NULL;
        if (!text) { pl_kept_free(&kept); return 0; }
        int status = sh_package_source_link_bind_keys(b->reader, c->origins[i], (const unsigned char *)text, length,
            pl_key_rewrite, &edit, &out, &out_length, error, capacity);
        free(edit.owned);
        if (status < 0) {
            /* A key site the readers cannot walk in this package's own
             * declaration leaves that package unserved; peers still compile.
             * Keys are allocated across every package at once, so this ends
             * the compile: packages failing later are found by the next one. */
            if (!*error) snprintf(error, capacity, "registration key sites could not be rewritten");
            size_t owner = strstr(error, "allocation") ? SIZE_MAX : pl_scope_owner(b, sources, scope);
            if (owner != SIZE_MAX) pl_contribution_blame(b, sources, owner, 0, error, capacity);
            free(text); pl_kept_free(&kept); return 0;
        }
        const char *current = status == 1 ? (const char *)out : text;
        size_t current_length = status == 1 ? out_length : length;
        if (split && sh_package_registration_lists((sh_decl_source){current, current_length})) {
            size_t base_length = 0, divided_length = 0;
            char *base_text = pl_token_text(c, base, &base_length, error, capacity), *state = NULL, *base_state = NULL;
            int kind = 0;
            int divided_ok = base_text &&
                pl_registration_state(c, i, (sh_decl_source){current, current_length}, &state, &kind, error, capacity) &&
                pl_registration_state(c, base, (sh_decl_source){base_text, base_length}, &base_state, &kind, error, capacity) &&
                pl_key_split(space, private_keys, private_keys && c->origins[i] == i, current, current_length, state,
                    base_text, base_length, base_state, changes, c->origins[i], &divided, &divided_length, error, capacity);
            free(base_text); free(state); free(base_state);
            if (!divided_ok) {
                if (!*error) snprintf(error, capacity, "registration rows could not be split");
                free(text); free(out); pl_kept_free(&kept); return 0;
            }
            if (divided) { current = divided; current_length = divided_length; }
        }
        if (!strcmp(unit->family, "snappropertyinspector_bitflag")) {
            /* Its choices set AI filters, which classify: custom moves only. */
            size_t inspected_length = 0; int failed = 0;
            inspected = sh_package_key_relocate_inspector(space, current, current_length, rows, encoder,
                &inspected_length, &failed);
            if (failed) {
                snprintf(error, capacity, failed == 2 ? "registration key group codes are exhausted" :
                    "registration key inspector allocation failed");
                free(text); free(out); free(divided); pl_kept_free(&kept); return 0;
            }
            if (inspected) { current = inspected; current_length = inspected_length; }
        }
        int changed = current_length != length || memcmp(current, text, length);
        free(text);
        if (!changed) { free(out); free(inspected); free(divided); pl_kept_free(&kept); continue; }
        int staged = pl_contribution_stage_kept(b, sources, i, current, current_length, NULL, 0, &kept, &pending,
            error, capacity);
        free(out); free(inspected); free(divided); pl_kept_free(&kept);
        if (!staged) { pl_contribution_unit_free(&pending); return 0; }
        if (!pending.owned) continue;
        if (!pl_unit_keep(c, i)) { pl_contribution_unit_free(&pending); snprintf(error, capacity, "%s", "cannot keep a captured unit's record"); return 0; }
        c->dirty = 1;
        pl_contribution_unit_free(c->units + i); c->units[i] = pending;
        c->inputs.units[i].compilation = c->units[i].owned;
        c->inputs.units[i].files = &c->units[i].file;
        c->inputs.units[i].original_binding = 0;
    }
    return 1;
}
/* The key one custom bit moved to (its own bit when kept). */
static uint32_t pl_move_key(const sh_package_key_move *move, unsigned bit, int private_keys)
{
    if (move->kind != SH_PACKAGE_KEY_REPLACE) return 1u << bit;
    for (size_t i = 0; private_keys && i < SH_PACKAGE_KEY_CODES; i++)
        if (sh_package_key_set_has(&move->keys, sh_package_key_code(i))) return sh_package_key_code(i);
    return move->bits;
}
static int pl_key_bind(pl_build *b, size_t scope, unsigned bit, uint32_t current)
{
    for (size_t k = 0; k < b->key_count; k++) if (b->keys[k].scope == scope && b->keys[k].original == bit) return 1;
    b->keys[b->key_count++] = (struct pl_key_binding){scope, bit, current};
    return 1;
}
static int pl_key_phase(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    sh_package_key_space space;
    pl_keys keys = {0};
    pl_key_changes changes = {0};
    sh_package_key_moves *rows_to = NULL, *to = NULL;
    pl_key_groups group_state = {b, NULL, NULL};
    sh_package_key_encoder encoder = {pl_key_group, &group_state};
    size_t *scopes = NULL, *starts = NULL, *order = NULL, most;
    pl_key_pool pool = {0};
    pl_key_pins pins = {0};
    unsigned char *conductors = NULL, *legacy_present = NULL;
    uint32_t pinned_bits = 0, historical = 0;
    char line[768];
    sh_decl_tree_census census_began = {0}, census_rows = {0}, census_ended = {0};
    int status = sh_package_source_link_key_space(b->reader, &space), ok = 0, private_keys;
    if (status < 0) { snprintf(error, capacity, "spawn-type reflection is unreadable"); return 0; }
    if (status) {
        b->key_space = space; b->key_space_read = 1;
        b->private_keys = b->native && b->native->private_keys && sh_package_key_space_private(&space);
    }
    if (!status || !b->authored_count) return 1;
    private_keys = b->private_keys;
    pool.private_keys = private_keys; pool.pool = space.pool;
    if (!(conductors = calloc(b->contributions->inputs.count + 1, 1)) ||
        !(legacy_present = calloc(b->authored_count, 1))) goto allocation;
    if (b->census_owned) sh_decl_tree_census_read(&census_began);
    if (!pl_key_rows(b, &space, private_keys, &keys, &changes, conductors,
        legacy_present, error, capacity)) goto done;
    if (b->census_owned) {
        sh_decl_tree_census_read(&census_rows); b->key_row_parses += census_rows.parses - census_began.parses;
    }
    unsigned long long relocation_began = pl_now_ms();
    for (size_t i = 0; i < b->contributions->inputs.count; i++)
        if (b->contributions->origins[i] != i) conductors[i] = conductors[b->contributions->origins[i]];
    int copies = 0;
    for (size_t i = 0; private_keys && !copies && i < b->contributions->inputs.count; i++) copies = conductors[i];
    for (size_t i = 0; !copies && i < b->authored_count; i++) copies = legacy_present[i];
    if (!keys.count && !changes.count && !copies) { ok = 1; goto done; }
    if (!pl_key_pins_open(b->native ? b->native->keys : NULL, b->native ? b->native->key_groups : NULL, &pins))
        goto allocation;
    group_state.pool = &pool; group_state.peer = b->native ? b->native->key_groups : NULL;
    qsort(keys.rows, keys.count, sizeof(*keys.rows), pl_key_row_order);
    for (size_t i = 0; i < keys.count; i++) pool.used |= 1u << keys.rows[i].bit;
    /* A peer's keys stay its scopes' own: none is issued to another scope. */
    for (size_t i = 0; i < pins.count; i++) {
        uint32_t held = pins.pins[i].original & SH_PACKAGE_KEY_UNNAMED ? pins.pins[i].current :
            pins.pins[i].current & ~pins.pins[i].original;
        if (!(held & SH_PACKAGE_KEY_MARKER)) { pool.used |= held; pinned_bits |= held; }
    }
    /* A peer group's single-bit member is held like a peer's single-bit key. */
    pool.used |= pins.members; pinned_bits |= pins.members;
    pool.held = &pins.held;
    most = (keys.count > changes.count ? keys.count : changes.count) + 1;
    rows_to = calloc(b->authored_count, sizeof(*rows_to)); to = calloc(b->authored_count, sizeof(*to));
    scopes = calloc(most, sizeof(*scopes)); starts = calloc(most, sizeof(*starts)); order = calloc(most, sizeof(*order));
    if (!rows_to || !to || !scopes || !starts || !order) goto allocation;
    for (size_t i = 0; i < keys.count;) {
        unsigned bit = keys.rows[i].bit;
        size_t end = i, count = 0;
        /* Under the contract bits 28 and 29 are reserved: no package keeps them. */
        int keepable = !private_keys || ((1u << bit) & SH_PACKAGE_KEY_SINGLE);
        while (end < keys.count && keys.rows[end].bit == bit) end++;
        /* Scopes registering this key, each with its run of rows. */
        for (size_t k = i; k < end; k++)
            if (k == i || keys.rows[k].scope != keys.rows[k - 1].scope) { scopes[count] = keys.rows[k].scope; starts[count++] = k; }
        starts[count] = end;
        for (size_t k = 0; k < count; k++) order[k] = k;
        pl_scope_sort(b, sources, order, count, scopes);
        /* Scopes a peer keys come first and keep its keys; the bit itself
         * stays with the first scope only while no peer key holds it. */
        size_t pinned = 0;
        int kept = (pinned_bits & (1u << bit)) != 0 || (historical & (1u << bit)) != 0;
        for (size_t k = 0; k < count; k++) {
            uint32_t current = 0;
            /* A peer's key on a historical bit is not this generation's to keep. */
            if (!pl_key_pinned(b, sources, &pins, scopes[order[k]], 1u << bit, &current) ||
                pl_key_historical(&pool, current)) continue;
            size_t moved = order[k];
            memmove(order + pinned + 1, order + pinned, (k - pinned) * sizeof(*order));
            order[pinned++] = moved;
            if (current == 1u << bit) kept = 1;
            else {
                sh_package_key_move_to(rows_to[scopes[moved]] + bit, private_keys, current);
                sh_package_key_move_to(to[scopes[moved]] + bit, private_keys, current);
            }
        }
        for (size_t k = pinned; k < count; k++) {
            size_t run = order[k], prior;
            uint32_t target = 1u << bit;
            for (prior = 0; prior < k; prior++)
                if (pl_key_same(&keys, starts[order[prior]], starts[order[prior] + 1], starts[run], starts[run + 1])) break;
            if (prior < k) target = pl_move_key(rows_to[scopes[order[prior]]] + bit, bit, private_keys);
            else if (kept || !keepable) {
                if (!(target = pl_key_take(&pool))) {
                    if (private_keys) { snprintf(error, capacity, "registration key namespace is exhausted"); goto done; }
                    /* No key is left, so no request can tell this AI apart:
                     * it keeps its authored key and shares it. */
                    snprintf(line, sizeof(line), "%s: AI registration key %u shares its spawn requests with %s; every "
                        "free spawn-type key already holds another package's AI, so either AI can answer them",
                        pl_scope_label(b, sources, scopes[run]), 1u << bit, pl_scope_label(b, sources, scopes[order[0]]));
                    if (!pl_key_note(b, line)) goto allocation;
                    continue;
                }
            }
            if (target == 1u << bit) kept = 1;
            else {
                sh_package_key_move_to(rows_to[scopes[run]] + bit, private_keys, target);
                sh_package_key_move_to(to[scopes[run]] + bit, private_keys, target);
            }
        }
        i = end;
    }
    /* Under the contract a custom bit a scope keeps is still that scope's key:
     * state it as a move, so the scope's composites (ALL) name it too. */
    for (size_t i = 0; private_keys && i < keys.count; i++) {
        const pl_key_row *row = keys.rows + i;
        if (rows_to[row->scope][row->bit].kind != SH_PACKAGE_KEY_KEEP) continue;
        sh_package_key_move_to(rows_to[row->scope] + row->bit, 1, 1u << row->bit);
        sh_package_key_move_to(to[row->scope] + row->bit, 1, 1u << row->bit);
    }
    /* Named rows: under the contract every distinct row takes a key; without
     * it only stock rows several packages change differently do. Classes go
     * in stable source order of their first package. */
    qsort(changes.rows, changes.count, sizeof(*changes.rows), pl_key_change_order);
    for (size_t i = 0; i < changes.count;) {
        size_t end = i, classes = 0;
        while (end < changes.count && !strcmp(changes.rows[end].key, changes.rows[i].key)) end++;
        for (size_t k = i; k < end; k++)
            if (k == i || strcmp(changes.rows[k].identity, changes.rows[k - 1].identity)) starts[classes++] = k;
        starts[classes] = end;
        /* Without the contract a row no bit identifies has no request of its own. */
        if (private_keys || (classes > 1 && changes.rows[i].identifying && changes.rows[i].replaces)) {
            pl_key_order context = {b, sources};
            for (size_t k = 0; k < classes; k++) {
                size_t first = starts[k];
                for (size_t m = starts[k] + 1; m < starts[k + 1]; m++)
                    if (pl_scope_order(&context, &changes.rows[m].scope, &changes.rows[first].scope) < 0) first = m;
                scopes[k] = changes.rows[first].scope; order[k] = k;
            }
            pl_scope_sort(b, sources, order, classes, scopes);
            for (size_t k = 0; k < classes; k++) {
                size_t set = order[k];
                uint32_t fallback = 0;
                uint32_t distinct = changes.rows[starts[set]].identifying;
                if (!distinct) distinct = changes.rows[starts[set]].bits & (0u - changes.rows[starts[set]].bits);
                for (unsigned named = 0; named < SH_PACKAGE_KEY_BITS; named++) {
                    if (!(distinct >> named & 1u)) continue;
                    uint32_t target = 0, current = 0;
                    /* Equal rows reuse each flag's key. A peer group names
                     * several rows and cannot identify this row on its own. */
                    for (size_t m = starts[set]; !target && m < starts[set + 1]; m++)
                        if (pl_key_pinned(b, sources, &pins, changes.rows[m].scope, 1u << named, &current)) {
                            uint32_t pinned = current & ~(1u << named) & SH_PACKAGE_KEY_UNNAMED;
                            const sh_package_key_groups *peer = b->native ? b->native->key_groups : NULL;
                            if (!private_keys ? pinned != 0 : sh_package_key_private_valid(pinned) &&
                                !pl_key_historical(&pool, pinned) &&
                                !((pinned & SH_PACKAGE_KEY_MARKER) && peer &&
                                    peer->defined[(pinned & SH_PACKAGE_KEY_SINGLE) >> 19])) target = pinned;
                            /* A group names this row's own key beside the keys
                             * of the context's other authored bits. */
                            else if (private_keys) target = pl_key_row_member(b, sources, &pins, &pool, peer,
                                changes.rows[m].scope, named, pinned);
                        }
                    if (!target) target = !private_keys && fallback ? fallback : pl_key_take(&pool);
                    if (!target) { snprintf(error, capacity, "registration key namespace is exhausted"); goto done; }
                    if (!fallback) fallback = target;
                    for (size_t m = starts[set]; m < starts[set + 1]; m++)
                        changes.rows[m].targets[named] = target;
                }
                /* Group flags keep their original bit beside the row key,
                 * so that original identity is already reversible. */
                for (size_t m = starts[set]; m < starts[set + 1]; m++)
                    for (unsigned named = 0; named < SH_PACKAGE_KEY_BITS; named++)
                        if ((changes.rows[m].bits >> named & 1u) && !changes.rows[m].targets[named])
                            changes.rows[m].targets[named] = fallback;
            }
        }
        i = end;
    }
    /* A changed stock row's identifying bits move to its key; under the
     * contract its other named bits (and a new row's) gain the key beside
     * them, so the package's own group requests still include its AI. */
    for (size_t i = 0; i < changes.count; i++) {
        const pl_key_change *change = changes.rows + i;
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++)
            if (change->targets[bit] && (change->identifying >> bit & 1u))
                sh_package_key_move_to(to[change->scope] + bit, private_keys, change->targets[bit]);
    }
    for (size_t i = 0; private_keys && i < changes.count; i++) {
        const pl_key_change *change = changes.rows + i;
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
            if (!(change->bits >> bit & 1u) || (change->identifying >> bit & 1u)) continue;
            if (change->targets[bit]) sh_package_key_move_add(to[change->scope] + bit, bit, private_keys, change->targets[bit]);
        }
    }
    /* Likewise a custom row's named bits (IMP's bit in "524292") gain its key. */
    for (size_t i = 0; private_keys && i < keys.count; i++) {
        const pl_key_row *row = keys.rows + i;
        uint32_t key = pl_move_key(rows_to[row->scope] + row->bit, row->bit, private_keys);
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
            if (!(row->named >> bit & 1u)) continue;
            sh_package_key_move_add(to[row->scope] + bit, bit, private_keys, key);
        }
    }
    /* A historical context is a complete native registration program. Give
     * each authored bit a private key; every request, modifier and
     * filter applies the same substitution. This preserves native intersection
     * and first-match behavior without exposing its literal bits to peers.
     * Bits absent from its table still have no registration, including through
     * ALL. Keeping them distinct also preserves later map-authored masks. */
    for (size_t scope = 0; private_keys && scope < b->authored_count; scope++) {
        if (!legacy_present[scope]) continue;
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
            uint32_t key = 0;
            if (!pl_key_pinned(b, sources, &pins, scope, 1u << bit, &key) || !sh_package_key_private_valid(key))
                key = pl_key_take(&pool);
            if (!key) { snprintf(error, capacity, "registration key namespace is exhausted"); goto done; }
            sh_package_key_move_to(rows_to[scope] + bit, 1, key);
            sh_package_key_move_to(to[scope] + bit, 1, key);
        }
    }
    /* Allocate all row keys before group codes, so no subsequent key can
     * reuse a code already assigned to a set. */
    for (size_t i = 0; i < changes.count; i++) {
        pl_key_change *change = changes.rows + i;
        sh_package_key_move move = {0};
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++)
            if (change->targets[bit]) sh_package_key_move_to(&move, private_keys, change->targets[bit]);
        if (move.kind == SH_PACKAGE_KEY_KEEP) continue;
        change->target = sh_package_key_move_code(&move, 0, b->key_groups);
        if (!change->target) {
            int status = pl_key_group(&group_state, &move.keys, &change->target);
            if (status <= 0) {
                if (!status) snprintf(error, capacity, "registration key group codes are exhausted");
                goto done;
            }
        }
    }
    /* A bit whose move names several keys needs its group code before the
     * provenance below records it (relocation reuses the same code). */
    for (size_t scope = 0; private_keys && scope < b->authored_count; scope++)
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
            uint32_t code = 0;
            int status;
            if (to[scope][bit].kind != SH_PACKAGE_KEY_REPLACE || sh_package_key_set_count(&to[scope][bit].keys) < 2 ||
                sh_package_key_move_code(to[scope] + bit, bit, b->key_groups)) continue;
            if ((status = pl_key_group(&group_state, &to[scope][bit].keys, &code)) <= 0) {
                if (!status) snprintf(error, capacity, "registration key group codes are exhausted");
                goto done;
            }
        }
    /* Retain every package key, moved or kept, for map provenance. */
    free(b->keys); b->key_count = 0;
    size_t key_capacity = SIZE_MAX / sizeof(*b->keys);
    if (keys.count >= key_capacity || changes.count > (key_capacity - keys.count - 1) / SH_PACKAGE_KEY_BITS) goto allocation;
    key_capacity = keys.count + changes.count * SH_PACKAGE_KEY_BITS + 1;
    if (b->authored_count > (SIZE_MAX / sizeof(*b->keys) - key_capacity) / SH_PACKAGE_KEY_BITS ||
        !(b->keys = calloc(key_capacity + b->authored_count * SH_PACKAGE_KEY_BITS, sizeof(*b->keys)))) goto allocation;
    for (size_t i = 0; i < keys.count; i++) {
        const pl_key_row *row = keys.rows + i;
        pl_key_bind(b, row->scope, row->bit, sh_package_key_move_code(to[row->scope] + row->bit, row->bit, b->key_groups));
    }
    for (size_t i = 0; i < changes.count; i++)
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++) {
            const pl_key_change *change = changes.rows + i;
            const sh_package_key_move *move = to[change->scope] + bit;
            if (!(change->bits >> bit & 1u) || (move->kind == SH_PACKAGE_KEY_KEEP && !(change->identifying >> bit & 1u))) continue;
            pl_key_bind(b, change->scope, bit, sh_package_key_move_code(move, bit, b->key_groups));
        }
    for (size_t scope = 0; scope < b->authored_count; scope++) if (legacy_present[scope])
        for (unsigned bit = 0; bit < SH_PACKAGE_KEY_BITS; bit++)
            pl_key_bind(b, scope, bit, sh_package_key_move_code(to[scope] + bit, bit, b->key_groups));
    for (size_t scope = 0; scope < b->authored_count; scope++) {
        /* Under the contract a scope's conductor copy restores stock rows. */
        int moves = 0;
        for (size_t i = 0; private_keys && !moves && i < b->contributions->inputs.count; i++)
            moves = conductors[i] && b->contributions->inputs.scopes[i] == scope;
        for (unsigned bit = 0; !moves && bit < SH_PACKAGE_KEY_BITS; bit++) moves = to[scope][bit].kind != SH_PACKAGE_KEY_KEEP;
        for (size_t i = 0; !moves && i < changes.count; i++)
            moves = changes.rows[i].scope == scope && changes.rows[i].target;
        if (moves && !pl_key_relocate_scope(b, sources, scope, &space, private_keys, rows_to[scope], to[scope],
            private_keys ? &encoder : NULL, &changes, conductors, error, capacity)) goto done;
        b->key_relocated_scopes += moves != 0;
    }
    if (b->census_owned) {
        sh_decl_tree_census_read(&census_ended); b->key_relocation_parses += census_ended.parses - census_rows.parses;
    }
    b->key_relocation_ms += pl_now_ms() - relocation_began;
    ok = 1;
    goto done;
allocation:
    snprintf(error, capacity, "registration key allocation failed");
done:
    pl_keys_free(&keys); pl_key_changes_free(&changes); pl_key_pins_free(&pins);
    free(rows_to); free(to); free(scopes); free(starts); free(order); free(conductors);
    free(legacy_present); return ok;
}
/* The engine applies the first global modifier row whose mask meets an AI's
 * type, so one mask has one effective row in one table. When packages give
 * one mask different rows, the first in stable source order keeps it in the
 * shared table and the others' rows revert to the stock row (or leave); the
 * library accounts the note. preferred (a member, or SIZE_MAX) keeps its own
 * rows first: an isolated member's own context serves its authored rows. */
static int pl_modifier_precedence(pl_build *b, const sh_package_sources *sources, const size_t *members,
    size_t count, sh_decl_source *texts, char *const *states, size_t preferred, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    sh_decl_node **trees = calloc(count ? count : 1, sizeof(*trees)), *stock = NULL;
    size_t *order = calloc(count ? count : 1, sizeof(*order)), *scope_of = calloc(count ? count : 1, sizeof(*scope_of));
    int *dirty = calloc(count ? count : 1, sizeof(*dirty)), ok = 0;
    size_t ordered = 0;
    const char *stock_state = NULL;
    char line[768];
    if (!trees || !order || !scope_of || !dirty) goto allocation;
    for (size_t i = 0; i < count; i++) {
        scope_of[i] = c->inputs.scopes[members[i]];
        if (scope_of[i] == b->count - 1) {
            sh_decl_tree_free(stock); stock = pl_entity_parse(texts[i], states[i], NULL, 0); stock_state = states[i];
            continue;
        }
        trees[i] = pl_entity_parse(texts[i], states[i], NULL, 0);
        if (pl_list(trees[i], states[i], "globalAIMods")) order[ordered++] = i;
    }
    pl_scope_sort(b, sources, order, ordered, scope_of);
    for (size_t k = 1; preferred != SIZE_MAX && k < ordered; k++) if (order[k] == preferred) {
        memmove(order + 1, order, k * sizeof(*order)); order[0] = preferred; break;
    }
    const sh_decl_node *stock_mods = pl_list(stock, stock_state, "globalAIMods");
    for (size_t k = 1; k < ordered; k++) {
        size_t i = order[k];
        sh_decl_node *mods = pl_list(trees[i], states[i], "globalAIMods");
        for (sh_decl_node **link = &mods->children; *link;) {
            sh_decl_node *item = *link;
            const char *key = NULL;
            size_t key_length = 0, winner = SIZE_MAX;
            if (!item->compound || !sh_decl_tree_literal(sh_decl_tree_member(item, "aiType"), &key, &key_length)) {
                link = &item->next; continue;
            }
            const sh_decl_node *original = pl_row_find(stock_mods, key, key_length);
            char *mine = pl_row_text(item), *base = original ? pl_row_text(original) : NULL;
            if (!mine || (original && !base)) { free(mine); free(base); goto allocation; }
            for (size_t p = 0; (!base || strcmp(mine, base)) && p < k; p++) {
                const sh_decl_node *theirs = pl_row_find(pl_list(trees[order[p]], states[order[p]], "globalAIMods"),
                    key, key_length);
                char *text = theirs ? pl_row_text(theirs) : NULL;
                if (theirs && !text) { free(mine); free(base); goto allocation; }
                int changed = text && (!base || strcmp(text, base));
                if (changed && strcmp(text, mine)) winner = order[p];
                free(text);
                if (changed) break;
            }
            free(mine); free(base);
            if (winner == SIZE_MAX) { link = &item->next; continue; }
            if (preferred == SIZE_MAX) snprintf(line, sizeof(line), "%s: its global AI modifier for %.*s differs from %s's; the engine applies "
                "only the first matching modifier row, so %s's applies", pl_scope_label(b, sources, scope_of[i]),
                (int)key_length, key, pl_scope_label(b, sources, scope_of[winner]), pl_scope_label(b, sources, scope_of[winner]));
            if (preferred == SIZE_MAX && !pl_key_note(b, line)) goto allocation;
            dirty[i] = 1;
            if (original) {
                sh_decl_node *kept = pl_node_clone(original);
                if (!kept) goto allocation;
                sh_decl_tree_free(item->children); item->children = kept->children;
                kept->children = NULL; sh_decl_tree_free(kept);
                link = &item->next;
            } else {
                *link = item->next; item->next = NULL; sh_decl_tree_free(item);
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        size_t length = 0;
        char *text;
        if (!dirty[i]) continue;
        if (!pl_list_renumber(pl_list(trees[i], states[i], "globalAIMods")) || !(text = sh_decl_tree_write(trees[i], &length)))
            goto allocation;
        free((char *)texts[i].text); texts[i].text = text; texts[i].length = length;
    }
    ok = 1;
    goto done;
allocation:
    snprintf(error, capacity, "registration contribution allocation failed");
done:
    if (trees) for (size_t i = 0; i < count; i++) sh_decl_tree_free(trees[i]);
    sh_decl_tree_free(stock); free(trees); free(order); free(scope_of); free(dirty);
    return ok;
}

/* First-match winners of a conductor's lists. An AI takes the key of the first
 * aiTypeList row registering its definition, and the first globalAIMods row
 * whose mask meets that type applies (Vulkan 0x141124470, 0x14110faa0; the
 * private-key detour keeps the same first match). Those winners are the only
 * order either list has: each stays first among the rows answering its
 * definition or type, and every other order is presentation, so rows an
 * earlier one shadows compose in any order. A source keeps each winner it
 * changes from the stock list's. When sources change one winner differently,
 * or a change would contradict winners already kept, the earlier source keeps
 * its choice (a frozen legacy context before packages, then stable source
 * order, as for equal modifier masks) and the other's is accounted. */
typedef struct pl_first_list {
    size_t count;
    char **types, **defs;   /* each row's aiType and (aiTypeList) entityDef text */
    uint32_t *bits;
    unsigned char *before;  /* count x count: row a stays before row b */
    /* The rows by their key (aiType, and entityDef for aiTypeList), open addressing over 1 + row (R-348): the merge's
     * comparisons find their rows at once. index_capacity 0: none (the rows are scanned). */
    size_t *index, index_capacity;
} pl_first_list;
typedef struct pl_first_match {
    pl_build *build;
    const sh_package_sources *sources;
    pl_first_list lists[2]; /* aiTypeList, globalAIMods */
    size_t **rows, *row_count; /* per source and list: its rows in authored order */
    size_t *scopes, source_count; /* the stock (or empty) original first, then contributions */
} pl_first_match;
static const char *const pl_first_names[2] = {"aiTypeList", "globalAIMods"};
static int pl_first_literal(const sh_decl_node *row, const char *field, const char **text, size_t *length)
{ return row && row->compound && sh_decl_tree_literal(sh_decl_tree_member(row, field), text, length); }
static int pl_first_equal(const pl_first_list *l, size_t i, int registration, const char *type, size_t type_length,
    const char *def, size_t def_length)
{
    return strlen(l->types[i]) == type_length && !memcmp(l->types[i], type, type_length) &&
        (!registration || (strlen(l->defs[i]) == def_length && !memcmp(l->defs[i], def, def_length)));
}
/* The index slot of a key: its row's, or the empty one it belongs in. */
static size_t pl_first_slot(const size_t *index, size_t capacity, const pl_first_list *l, int registration,
    const char *type, size_t type_length, const char *def, size_t def_length)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < type_length; i++) h = (h ^ (unsigned char)type[i]) * UINT64_C(1099511628211);
    h = (h ^ 0xff) * UINT64_C(1099511628211);
    for (size_t i = 0; registration && i < def_length; i++) h = (h ^ (unsigned char)def[i]) * UINT64_C(1099511628211);
    size_t mask = capacity - 1, at = (size_t)(h ^ (h >> 31)) & mask;
    while (index[at] && !pl_first_equal(l, index[at] - 1, registration, type, type_length, def, def_length))
        at = (at + 1) & mask;
    return at;
}
static size_t pl_first_find(const pl_first_list *l, int registration, const sh_decl_node *row)
{
    const char *type, *def = NULL;
    size_t type_length, def_length = 0;
    if (!pl_first_literal(row, "aiType", &type, &type_length) ||
        (registration && !pl_first_literal(row, "entityDef", &def, &def_length))) return SIZE_MAX;
    if (l->index_capacity) {
        size_t at = pl_first_slot(l->index, l->index_capacity, l, registration, type, type_length, def, def_length);
        return l->index[at] ? l->index[at] - 1 : SIZE_MAX;
    }
    for (size_t i = 0; i < l->count; i++)
        if (pl_first_equal(l, i, registration, type, type_length, def, def_length)) return i;
    return SIZE_MAX;
}
/* Index row index of l (just added); when the index cannot grow it is dropped and pl_first_find scans. */
static void pl_first_index(pl_first_list *l, int registration, size_t row)
{
    if (l->count > l->index_capacity / 2) {
        size_t capacity = l->index_capacity ? l->index_capacity * 2 : 256;
        size_t *grown = capacity <= SIZE_MAX / 2 / sizeof(*grown) ? calloc(capacity, sizeof(*grown)) : NULL;
        free(l->index); l->index = grown; l->index_capacity = grown ? capacity : 0;
        for (size_t i = 0; grown && i < l->count; i++) if (i != row) {
            const char *def = registration ? l->defs[i] : NULL;
            grown[pl_first_slot(grown, capacity, l, registration, l->types[i], strlen(l->types[i]), def,
                def ? strlen(def) : 0)] = i + 1;
        }
    }
    if (!l->index_capacity) return;
    const char *def = registration ? l->defs[row] : NULL;
    size_t at = pl_first_slot(l->index, l->index_capacity, l, registration, l->types[row], strlen(l->types[row]), def,
        def ? strlen(def) : 0);
    if (!l->index[at]) l->index[at] = row + 1;
}
static char *pl_first_copy(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy) { memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}
/* The row's index, adding it when new; SIZE_MAX for a row without the fields. */
static int pl_first_add(pl_first_match *m, int list, const sh_decl_node *row, size_t *index)
{
    pl_first_list *l = m->lists + list;
    const char *type, *def = NULL;
    size_t type_length, def_length = 0;
    *index = SIZE_MAX;
    if (!pl_first_literal(row, "aiType", &type, &type_length) ||
        (!list && !pl_first_literal(row, "entityDef", &def, &def_length))) return 1;
    if ((*index = pl_first_find(l, !list, row)) != SIZE_MAX) return 1;
    char **types = realloc(l->types, (l->count + 1) * sizeof(*types));
    if (types) l->types = types;
    char **defs = types ? realloc(l->defs, (l->count + 1) * sizeof(*defs)) : NULL;
    if (defs) l->defs = defs;
    uint32_t *bits = defs ? realloc(l->bits, (l->count + 1) * sizeof(*bits)) : NULL;
    if (!bits) return 0;
    l->bits = bits;
    l->types[l->count] = pl_first_copy(type, type_length);
    l->defs[l->count] = list ? NULL : pl_first_copy(def, def_length);
    l->bits[l->count] = 0;
    if (!l->types[l->count] || (!list && !l->defs[l->count])) {
        free(l->types[l->count]); free(l->defs[l->count]); return 0;
    }
    sh_package_key_bits(&m->build->key_space, type, type_length, l->bits + l->count);
    *index = l->count++;
    pl_first_index(l, !list, *index);
    return 1;
}
static void pl_first_close(pl_first_match *m)
{
    for (int k = 0; k < 2; k++) {
        pl_first_list *l = m->lists + k;
        for (size_t i = 0; i < l->count; i++) { free(l->types[i]); free(l->defs[i]); }
        free(l->types); free(l->defs); free(l->bits); free(l->before); free(l->index);
    }
    if (m->rows) for (size_t i = 0; i < m->source_count * 2; i++) free(m->rows[i]);
    free(m->rows); free(m->row_count); free(m->scopes);
    memset(m, 0, sizeof(*m));
}
/* Whether row answers query: a definition's registrations, or the modifier
 * rows meeting a type (under the private-key contract when it serves). */
static int pl_first_answers(const pl_first_match *m, int list, size_t row, const void *query)
{
    const pl_first_list *l = m->lists + list;
    if (!list) return !strcmp(l->defs[row], (const char *)query);
    uint32_t type = *(const uint32_t *)query;
    return m->build->private_keys ? sh_package_key_modifier_matches(m->build->key_groups, type, l->bits[row]) :
        (type & l->bits[row]) != 0;
}
static size_t pl_first_winner(const pl_first_match *m, int list, size_t source, const void *query)
{
    const size_t *rows = m->rows[source * 2 + list];
    int scoped = 0;
    if (list && m->build->private_keys && (*(const uint32_t *)query & SH_PACKAGE_KEY_PRIVATE))
        for (size_t i = 0; !scoped && i < m->lists[list].count; i++)
            scoped = (m->lists[list].bits[i] & SH_PACKAGE_KEY_PRIVATE) && pl_first_answers(m, list, i, query);
    for (size_t i = 0; i < m->row_count[source * 2 + list]; i++)
        if (rows[i] != SIZE_MAX && (!scoped || (m->lists[list].bits[rows[i]] & SH_PACKAGE_KEY_PRIVATE)) &&
            pl_first_answers(m, list, rows[i], query)) return rows[i];
    return SIZE_MAX;
}
/* Whether a path of kept orders already leads from row to target. */
static int pl_first_reaches(const pl_first_list *l, size_t row, size_t target, unsigned char *seen, size_t *stack)
{
    size_t depth = 0;
    memset(seen, 0, l->count);
    stack[depth++] = row; seen[row] = 1;
    while (depth) {
        size_t at = stack[--depth];
        if (at == target) return 1;
        for (size_t next = 0; next < l->count; next++)
            if (l->before[at * l->count + next] && !seen[next]) { seen[next] = 1; stack[depth++] = next; }
    }
    return 0;
}
/* Keep winner first among the rows answering query unless that contradicts
 * the orders already kept. */
static int pl_first_keep(pl_first_match *m, int list, size_t winner, const void *query, unsigned char *seen, size_t *stack)
{
    pl_first_list *l = m->lists + list;
    for (size_t row = 0; row < l->count; row++)
        if (row != winner && pl_first_answers(m, list, row, query) && pl_first_reaches(l, row, winner, seen, stack))
            return 0;
    for (size_t row = 0; row < l->count; row++)
        if (row != winner && pl_first_answers(m, list, row, query)) l->before[winner * l->count + row] = 1;
    return 1;
}
/* Decide one list's winners: every contribution's changed winner in
 * precedence, then the stock winner where nobody changed it. */
static int pl_first_decide(pl_first_match *m, int list, const void *queries, size_t query_count, size_t *winners)
{
    pl_first_list *l = m->lists + list;
    unsigned char *seen = malloc(l->count ? l->count : 1);
    size_t *stack = malloc((l->count ? l->count : 1) * sizeof(*stack)), *keeper = malloc((query_count ? query_count : 1) * sizeof(*keeper));
    char line[768];
    int ok = 0;
    if (!seen || !stack || !keeper) goto done;
    for (size_t q = 0; q < query_count; q++) { winners[q] = SIZE_MAX; keeper[q] = SIZE_MAX; }
    for (size_t s = 1; s < m->source_count; s++)
        for (size_t q = 0; q < query_count; q++) {
            const void *query = list ? (const void *)((const uint32_t *)queries + q) : ((const char *const *)queries)[q];
            size_t stock = pl_first_winner(m, list, 0, query), mine = pl_first_winner(m, list, s, query);
            if (mine == SIZE_MAX || mine == stock || mine == winners[q]) continue;
            const char *who = pl_scope_label(m->build, m->sources, m->scopes[s]);
            if (winners[q] == SIZE_MAX && pl_first_keep(m, list, mine, query, seen, stack)) {
                winners[q] = mine; keeper[q] = s; continue;
            }
            const char *kept = keeper[q] != SIZE_MAX ? pl_scope_label(m->build, m->sources, m->scopes[keeper[q]]) :
                "earlier packages";
            if (list)
                snprintf(line, sizeof(line), "%s: its first global AI modifier for type %lu differs from %s's; the "
                    "engine applies only the first matching row, so %s's applies", who,
                    (unsigned long)*(const uint32_t *)query, kept, kept);
            else
                snprintf(line, sizeof(line), "%s: its first registration of %s differs from %s's; the engine "
                    "types an AI by its definition's first registration, so %s's applies", who,
                    pl_authored_name(m->build->contributions, m->scopes[s], "entitydef", query), kept, kept);
            if (!pl_key_note(m->build, line)) goto done;
        }
    for (size_t q = 0; q < query_count; q++) {
        const void *query = list ? (const void *)((const uint32_t *)queries + q) : ((const char *const *)queries)[q];
        size_t stock = pl_first_winner(m, list, 0, query);
        if (winners[q] == SIZE_MAX && stock != SIZE_MAX && pl_first_keep(m, list, stock, query, seen, stack))
            winners[q] = stock;
    }
    ok = 1;
done:
    free(seen); free(stack); free(keeper); return ok;
}
static int pl_first_relation(void *context, const char *path, const sh_decl_node *left, const sh_decl_node *right)
{
    const pl_first_match *m = context;
    int list = !strcmp(path, "edit.aiTypeList") ? 0 : !strcmp(path, "edit.globalAIMods") ? 1 : -1;
    if (list < 0) return 0;
    const pl_first_list *l = m->lists + list;
    size_t a = pl_first_find(l, !list, left), z = pl_first_find(l, !list, right);
    if (a == SIZE_MAX || z == SIZE_MAX || a == z || !l->before) return 0;
    return l->before[a * l->count + z] ? -1 : l->before[z * l->count + a] ? 1 : 0;
}
/* Kept winners are the modifier list's behavior; the rest is presentation. */
static int pl_first_significant(void *context, const char *path, const sh_decl_node *left, const sh_decl_node *right)
{
    (void)context; (void)left; (void)right;
    return strcmp(path, "edit.globalAIMods") ? -1 : 0;
}
static int pl_first_precedes(const pl_build *b, const sh_package_sources *sources, size_t x, size_t y)
{
    int hx = x < b->authored_count && !pl_modern(b, x), hy = y < b->authored_count && !pl_modern(b, y);
    pl_key_order order = {b, sources};
    if (hx != hy) return hx ? -1 : 1;
    return pl_scope_order(&order, &x, &y);
}
/* Read every source's rows and decide both lists' winners. base is the stock
 * member's index in texts (SIZE_MAX: none). */
static int pl_first_open(pl_build *b, const sh_package_sources *sources, const size_t *members,
    const sh_decl_source *texts, char *const *states, size_t count, size_t base, pl_first_match *m,
    char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    size_t *order = calloc(count + 1, sizeof(*order)), *winners = NULL, def_count = 0, type_count = 0;
    const char **defs = NULL;
    uint32_t *types = NULL;
    int ok = 0;
    m->build = b; m->sources = sources;
    m->source_count = count + (base == SIZE_MAX);
    m->rows = calloc(m->source_count * 2, sizeof(*m->rows));
    m->row_count = calloc(m->source_count * 2, sizeof(*m->row_count));
    m->scopes = calloc(m->source_count, sizeof(*m->scopes));
    if (!order || !m->rows || !m->row_count || !m->scopes) goto allocation;
    /* Source 0 is the original; the rest in precedence. */
    size_t ordered = 0;
    for (size_t i = 0; i < count; i++) if (i != base) order[ordered++] = i;
    for (size_t k = 1; k < ordered; k++)
        for (size_t j = k; j > 0 && pl_first_precedes(b, sources, c->inputs.scopes[members[order[j - 1]]],
            c->inputs.scopes[members[order[j]]]) > 0; j--) {
            size_t swap = order[j]; order[j] = order[j - 1]; order[j - 1] = swap;
        }
    for (size_t s = 0; s < m->source_count; s++) {
        size_t text = s ? order[s - 1] : base;
        sh_decl_node *tree = text == SIZE_MAX ? NULL : pl_entity_parse(texts[text], states[text], NULL, 0);
        m->scopes[s] = text == SIZE_MAX ? SIZE_MAX : c->inputs.scopes[members[text]];
        for (int list = 0; list < 2; list++) {
            const sh_decl_node *rows = pl_list(tree, text == SIZE_MAX ? NULL : states[text], pl_first_names[list]);
            size_t n = 0;
            for (const sh_decl_node *row = rows ? rows->children : NULL; row; row = row->next) n++;
            if (!(m->rows[s * 2 + list] = malloc((n ? n : 1) * sizeof(size_t)))) { sh_decl_tree_free(tree); goto allocation; }
            for (const sh_decl_node *row = rows ? rows->children : NULL; row; row = row->next) {
                size_t index;
                if (!pl_first_add(m, list, row, &index)) { sh_decl_tree_free(tree); goto allocation; }
                if (index != SIZE_MAX) m->rows[s * 2 + list][m->row_count[s * 2 + list]++] = index;
            }
        }
        sh_decl_tree_free(tree);
    }
    for (int list = 0; list < 2; list++) {
        size_t n = m->lists[list].count;
        if (n && !(m->lists[list].before = calloc(n, n))) goto allocation;
    }
    /* Registrations first: their winners give the types modifiers answer. */
    if (!(defs = calloc(m->lists[0].count + 1, sizeof(*defs)))) goto allocation;
    for (size_t i = 0; i < m->lists[0].count; i++) {
        size_t k = 0;
        while (k < def_count && strcmp(defs[k], m->lists[0].defs[i])) k++;
        if (k == def_count) defs[def_count++] = m->lists[0].defs[i];
    }
    if (!(winners = calloc(def_count + 1, sizeof(*winners))) ||
        !pl_first_decide(m, 0, defs, def_count, winners)) goto allocation;
    if (!(types = calloc(def_count + 1, sizeof(*types)))) goto allocation;
    for (size_t q = 0; q < def_count; q++) {
        uint32_t bits = winners[q] == SIZE_MAX ? 0 : m->lists[0].bits[winners[q]];
        size_t k = 0;
        while (k < type_count && types[k] != bits) k++;
        if (bits && k == type_count) types[type_count++] = bits;
    }
    free(winners);
    if (!(winners = calloc(type_count + 1, sizeof(*winners))) ||
        !pl_first_decide(m, 1, types, type_count, winners)) goto allocation;
    ok = 1;
    goto done;
allocation:
    snprintf(error, capacity, "registration contribution allocation failed");
done:
    free(order); free(winners); free(defs); free(types);
    return ok;
}

/* Shared gameplay registration lists (a conductor's aiTypeList): every source
 * of the declaration contributes its rows, each bound in its own scope, and
 * every member receives the union keyed by the native aiType and bound AI
 * (the spawn manager picks among several rows of one type). The stock-named
 * declaration therefore registers each package's AI variants for spawning;
 * colliding package keys were relocated first (pl_key_phase), modifier rows
 * sharing one mask keep the first in stable source order, and both lists keep
 * their first-match winners (pl_first_open). A frozen legacy
 * context's copy joins with its authored keys and without restored stock rows
 * (they stay removed, as in its whole-file conductor); the result keeps the
 * stock name in a map document too, where the engine still finds it by name.
 * Only the stock member and historical copies bind that name: an isolated
 * member beside them keeps its own effective copy in its own context, as in a
 * library without historical contexts. Returns as pl_contribution_group. */
/* Whether the map's own record makes a historical scope an authority for one
 * fixed identity. Its document-level (HISTORICAL) uses are the map's captured
 * source: when one names this identity's binding, exactly the contexts it
 * selects claim the name; otherwise any context such a use selects does. A
 * context only exact occurrence uses select (objects copied in from another
 * map) is not the map's own source, and a recipe without USES semantics is
 * wholly the map's own legacy record. */
static int pl_document_authority(const pl_build *b, const char *family, const char *name, size_t scope)
{
    const sh_package_provenance *p = b->contexts;
    int named = 0, selected = 0, document = 0;
    if (!p || p->semantics != SH_PACKAGE_PROVENANCE_USES) return 1;
    for (size_t u = 0; u < p->use_count; u++) {
        const sh_package_provenance_use *use = p->uses + u;
        if (use->kind != SH_PACKAGE_USE_HISTORICAL || use->binding >= p->binding_count) continue;
        const sh_package_provenance_binding *binding = p->bindings + use->binding;
        int same = !_stricmp(binding->family, family) && !_stricmp(binding->original, name);
        named |= same;
        for (size_t c = 0; c < use->context_count; c++)
            if (use->contexts[c] == scope) { document = 1; selected |= same; }
    }
    return named ? selected : document;
}
static int pl_registration_group(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + members[0];
    sh_decl_source *texts = calloc(count, sizeof(*texts)), *parts = calloc(count + 1, sizeof(*parts));
    const sh_compiled_resource **peers = calloc(count + 1, sizeof(*peers));
    pl_contribution_unit *pending = calloc(count, sizeof(*pending));
    size_t *part_member = calloc(count + 1, sizeof(*part_member));
    char **state_of = calloc(count, sizeof(*state_of));
    const char **states = calloc(count + 2, sizeof(*states));
    /* A member of a historical (composed) context: 1, or 2 when the map's own
     * record makes it an authority for this identity (pl_document_authority). */
    unsigned char *frozen = calloc(count + 1, 1);
    sh_decl_source base = {"{}", 2};
    size_t source_count = 0, culprit = SIZE_MAX, *lengths = NULL;
    char **bodies = NULL;
    sh_decl_source *own = NULL, *mine = NULL, *mine_parts = NULL;
    int ok = 0, kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT, stated = 0, historical = 0, isolated = 0;
    pl_first_match first = {0};
    sh_decl_collection_order order = {&first, pl_first_relation, pl_first_significant};
    const sh_decl_collection_order *ordering = NULL;
    size_t base_index = SIZE_MAX;
    if (!texts || !parts || !peers || !pending || !part_member || !state_of || !states || !frozen) {
        snprintf(error, capacity, "registration contribution allocation failed"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t scope = c->inputs.scopes[members[i]];
        if (scope < b->authored_count && !pl_modern(b, scope))
            historical = 1, frozen[i] = (unsigned char)(1 + pl_document_authority(b, input->family, input->name, scope));
        else if (scope != b->count - 1) isolated = 1;
    }
    for (size_t i = 0; i < count; i++) {
        if (!(texts[i].text = pl_token_text(c, members[i], &texts[i].length, error, capacity))) goto failed;
        if (sh_package_registration_lists(texts[i])) stated = 1;
    }
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if ((culprit = pl_test_defect(b, sources, members, count)) != SIZE_MAX) {
        kind = SH_PACKAGE_EDITOR_FAILURE_SOURCE; snprintf(error, capacity, "test contribution defect"); goto failed;
    }
#endif
    /* Each text's lists live in the statement its inheritance selects; every
     * reader below (precedence, first-match, composition, probes) uses it. */
    for (size_t i = 0; stated && i < count; i++)
        if (!pl_registration_state(c, members[i], texts[i], state_of + i, &kind, error, capacity)) goto failed;
    /* Isolated members keep their own texts: each one's own context composes
     * with its own modifier rows first (pl_modifier_precedence). */
    if (stated && isolated && count) {
        if (!(own = calloc(count, sizeof(*own))) || !(mine = calloc(count, sizeof(*mine))) ||
            !(mine_parts = calloc(count, sizeof(*mine_parts)))) {
            snprintf(error, capacity, "registration contribution allocation failed"); goto failed;
        }
        for (size_t i = 0; i < count; i++) {
            if (!(own[i].text = malloc(texts[i].length + 1))) {
                snprintf(error, capacity, "registration contribution allocation failed"); goto failed;
            }
            memcpy((char *)own[i].text, texts[i].text, texts[i].length); ((char *)own[i].text)[texts[i].length] = 0;
            own[i].length = texts[i].length;
        }
    }
    if (stated && !pl_modifier_precedence(b, sources, members, count, texts, state_of, SIZE_MAX, error, capacity)) goto failed;
    for (size_t i = 0; i < count; i++) {
        if (c->inputs.scopes[members[i]] == b->count - 1) { base = texts[i]; base_index = i; }
        else { part_member[source_count] = i; parts[source_count++] = texts[i]; }
    }
    if (!stated || !source_count) { ok = 1; goto done; }
    states[0] = base_index != SIZE_MAX ? state_of[base_index] : NULL;
    for (size_t i = 0; i < source_count; i++) states[i + 1] = state_of[part_member[i]];
    if (b->key_space_read) {
        if (!pl_first_open(b, sources, members, texts, state_of, count, base_index, &first, error, capacity)) goto failed;
        ordering = &order;
    }
    if (!(bodies = calloc(count, sizeof(*bodies))) || !(lengths = calloc(count, sizeof(*lengths)))) {
        snprintf(error, capacity, "registration contribution allocation failed"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        sh_package_editor_failure failure;
        const sh_decl_source *use_parts = parts;
        sh_decl_source use_target = texts[i];
        states[source_count + 1] = state_of[i];
        if (own && i != base_index && !frozen[i]) {
            for (size_t k = 0; k < count; k++) {
                free((char *)mine[k].text);
                if (!(mine[k].text = malloc(own[k].length + 1))) {
                    snprintf(error, capacity, "registration contribution allocation failed"); goto failed;
                }
                memcpy((char *)mine[k].text, own[k].text, own[k].length); ((char *)mine[k].text)[own[k].length] = 0;
                mine[k].length = own[k].length;
            }
            if (!pl_modifier_precedence(b, sources, members, count, mine, state_of, i, error, capacity)) goto failed;
            for (size_t k = 0; k < source_count; k++) mine_parts[k] = mine[part_member[k]];
            use_parts = mine_parts; use_target = mine[i];
        }
        bodies[i] = sh_package_registration_contributions_states(base, use_parts, source_count, use_target,
            ordering, states, lengths + i, &failure, error, capacity);
        if (!bodies[i]) {
            kind = failure.kind;
            if (kind == SH_PACKAGE_EDITOR_FAILURE_SOURCE && failure.source < source_count)
                culprit = members[part_member[failure.source]];
            else if (kind == SH_PACKAGE_EDITOR_FAILURE_COVERAGE) {
                pl_collection_probe probe = {"entitydef", 1, 0, base, use_target, ordering, states, source_count};
                pl_joint_participants(b, sources, members, part_member, use_parts, source_count,
                    pl_collection_composes, &probe, error, capacity);
            }
            goto failed;
        }
    }
    /* A composed context keeps the stock name (global, fixed below), so the
     * group publishes one declaration at that identity. A historical copy
     * replaces the stock file in its context, so the stock member carries the
     * map's captured composition: the union in the statement the copy's own
     * state selects, stock rows keeping their original binding. An isolated
     * copy, or a historical one only copied objects use, is not a claimant: it
     * binds its own context's copy (the union keeps its rows). Copies the map's
     * own record names that compose different declarations compete for the
     * one identity; neither source order nor the stock form decides that
     * (product coverage). */
    if (historical && base_index != SIZE_MAX) {
        size_t authority = SIZE_MAX;
        for (size_t i = 0; i < count; i++) {
            if (i == base_index || frozen[i] != 2) continue;
            if (authority == SIZE_MAX) { authority = i; continue; }
            if (lengths[i] != lengths[authority] || memcmp(bodies[i], bodies[authority], lengths[i])) {
                kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
                snprintf(error, capacity, "entitydef %s: authored copies compose different declarations at its "
                    "one native identity (product coverage)", input->name);
                goto failed;
            }
        }
        char *shared = authority != SIZE_MAX ? malloc(lengths[authority] + 1) : NULL;
        if (authority != SIZE_MAX && !shared) {
            snprintf(error, capacity, "registration contribution allocation failed"); goto failed;
        }
        if (shared) {
            memcpy(shared, bodies[authority], lengths[authority]); shared[lengths[authority]] = 0;
            free(bodies[base_index]); bodies[base_index] = shared; lengths[base_index] = lengths[authority];
        }
    }
    for (size_t i = 0; i < count; i++) {
        size_t peer_count = 0;
        for (size_t j = 0; j < count; j++) if (j != i && c->inputs.scopes[members[j]] != b->count - 1)
            peers[peer_count++] = c->inputs.units[members[j]].files[0];
        if (!pl_contribution_stage(b, sources, members[i], bodies[i], lengths[i], peers, peer_count, pending + i,
            error, capacity)) goto failed;
    }
    for (size_t i = 0; i < count; i++) if (pending[i].owned && !pl_unit_keep(c, members[i])) {
        kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT; culprit = SIZE_MAX;
        snprintf(error, capacity, "%s", "cannot keep a captured unit's record"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        if (historical && (frozen[i] == 2 || i == base_index)) c->global[unit] = 1;
        if (!pending[i].owned) continue;
        c->dirty = 1;
        pl_contribution_unit_free(c->units + unit); c->units[unit] = pending[i]; memset(pending + i, 0, sizeof(*pending));
        c->inputs.units[unit].compilation = c->units[unit].owned;
        c->inputs.units[unit].files = &c->units[unit].file;
        const sh_package_link_unit *original = sh_package_source_link_unit(c->source, unit);
        c->inputs.units[unit].original_binding = original && original->original_binding &&
            original->files[0]->body && original->files[0]->body_length == c->units[unit].file->body_length &&
            !memcmp(original->files[0]->body, c->units[unit].file->body, c->units[unit].file->body_length);
    }
    ok = 1;
    goto done;
failed:
    ok = pl_contribution_failure(b, sources, input, kind, culprit, error, capacity) ? -1 : 0;
done:
    if (texts) for (size_t i = 0; i < count; i++) free((char *)texts[i].text);
    if (pending) for (size_t i = 0; i < count; i++) pl_contribution_unit_free(pending + i);
    if (state_of) for (size_t i = 0; i < count; i++) free(state_of[i]);
    if (bodies) for (size_t i = 0; i < count; i++) free(bodies[i]);
    for (size_t i = 0; i < count; i++) {
        if (own) free((char *)own[i].text);
        if (mine) free((char *)mine[i].text);
    }
    free(own); free(mine); free(mine_parts); free(frozen);
    free(bodies); free(lengths); free(state_of); free((void *)states);
    free(texts); free(parts); free((void *)peers); free(pending); free(part_member); pl_first_close(&first);
    return ok;
}
/* Entity groups whose sources state registration lists; a cheap byte check
 * precedes parsing because most entity declarations have none. */
static int pl_registration_phase(pl_build *b, const sh_package_sources *sources, const size_t *order,
    size_t order_count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    for (size_t i = 0; i < order_count;) {
        const sh_package_link_unit *unit = c->inputs.units + order[i];
        size_t end = i + 1;
        int candidate = 0;
        while (end < order_count && !strcmp(c->inputs.units[order[end]].family, unit->family) &&
            !strcmp(c->inputs.units[order[end]].name, unit->name)) end++;
        if (!strcmp(unit->family, "entitydef") && end - i > 1)
            for (size_t k = i; k < end && !candidate; k++) {
                const sh_compiled_resource *file = c->inputs.units[order[k]].files[0];
                size_t length = 0;
                unsigned char *body = sh_package_compilation_read(c->inputs.units[order[k]].compilation, file,
                    SIZE_MAX, &length, error, capacity);
                if (!body) return 0;
                for (size_t at = 0; !candidate && at + 12 <= length; at++)
                    candidate = !memcmp(body + at, "aiTypeList", 10) || (at + 12 <= length && !memcmp(body + at, "globalAIMods", 12));
                free(body);
            }
        if (candidate && !pl_group_judge(b, sources, NULL, 0, SIZE_MAX, NULL, order + i, end - i,
            pl_registration_group, error, capacity)) return 0;
        i = end;
    }
    return 1;
}

/* ---- The player hands web -------------------------------------------------
 * The web idPlayer::handsAnimWeb names is one global registry: the hands
 * animator looks every weapon's subweb up in it by name (Vulkan FUN_140cbd0c0).
 * A package changing it (adding its weapon's subweb and switch edges) changes
 * that global web, so package copies align against the stock original and
 * compose by native records into the stock-named declaration every player
 * uses (sh_decl_animweb_prepare): edges into a package's new subweb join the
 * stock subwebs, while a stock subweb a package changes otherwise, or a new
 * subweb an earlier package defines differently, becomes that package's own
 * copy, which the package's weapons then name. */
/* A weapon of this scope naming a subweb the scope now owns under its own name
 * names the copy (subweb_normal, handsHitReactionSubweb). */
static int pl_hands_weapons(pl_build *b, const sh_package_sources *sources, size_t scope,
    const char *from, const char *to, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        pl_contribution_unit pending = {0};
        size_t length = 0, out_length = 0;
        int changed = 0;
        if (c->inputs.scopes[i] != scope || unit->file_count != 1 || strcmp(unit->family, "weapon")) continue;
        char *text = pl_token_text(c, i, &length, error, capacity);
        if (!text) return 0;
        char *out = sh_decl_weapon_subweb_rename((sh_decl_source){text, length}, from, to, &out_length,
            &changed, error, capacity);
        free(text);
        if (!changed) { if (*error) return 0; continue; }
        int staged = pl_contribution_stage(b, sources, i, out, out_length, NULL, 0, &pending, error, capacity);
        free(out);
        if (!staged) { pl_contribution_unit_free(&pending); return 0; }
        if (!pending.owned) continue;
        if (!pl_unit_keep(c, i)) { pl_contribution_unit_free(&pending); snprintf(error, capacity, "%s", "cannot keep a captured unit's record"); return 0; }
        c->dirty = 1;
        pl_contribution_unit_free(c->units + i); c->units[i] = pending;
        c->inputs.units[i].compilation = c->units[i].owned;
        c->inputs.units[i].files = &c->units[i].file;
        c->inputs.units[i].original_binding = 0;
    }
    return 1;
}
static int pl_hands_group(pl_build *b, const sh_package_sources *sources,
    const size_t *members, size_t count, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_unit *input = c->inputs.units + members[0];
    sh_decl_source *texts = calloc(count, sizeof(*texts)), *parts = calloc(count + 1, sizeof(*parts));
    const sh_compiled_resource **peers = calloc(count + 1, sizeof(*peers));
    pl_contribution_unit *pending = calloc(count, sizeof(*pending));
    size_t *part_member = calloc(count + 1, sizeof(*part_member)), *part_scope = calloc(count + 1, sizeof(*part_scope));
    char **owned = calloc(count + 1, sizeof(*owned));
    sh_decl_animweb_move *moves = NULL;
    size_t base_member = SIZE_MAX, part_count = 0, culprit = SIZE_MAX, composed_length = 0, move_count = 0;
    char *composed = NULL, *stock_text = NULL;
    size_t stock_length = 0;
    sh_decl_source base_text = {0};
    int ok = 0, kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT;
    /* Every scope's copy of the web stages the one composition: equal bound bytes are one body. */
    pl_stage_pool pool;
    pl_stage_pool_open(c, &pool);
    if (!texts || !parts || !peers || !pending || !part_member || !part_scope || !owned) {
        snprintf(error, capacity, "player hands web allocation failed"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        if (!(texts[i].text = pl_token_text(c, unit, &texts[i].length, error, capacity))) goto failed;
        if (c->inputs.scopes[unit] == b->count - 1) base_member = i;
        else if (!pl_stock_authored(c, unit)) part_member[part_count++] = i;
    }
    if (!part_count) { ok = 1; goto done; }
    /* R-401: content names need the stock web to tell added subwebs from stock ones, also in a scope set that
     * holds no stock member (a map context): read it as the library reads every original. */
    if (base_member == SIZE_MAX && sh_decl_animweb_content_names_enabled() && b->originals[0].read) {
        unsigned char *body = NULL; size_t n = 0;
        if (b->originals[0].read(b->originals[0].context, input->files[0]->engine_path, &body, &n) > 0) {
            stock_text = (char *)body; stock_length = n;
        } else free(body);
    }
    base_text = base_member != SIZE_MAX ? texts[base_member] : (sh_decl_source){stock_text, stock_length};
    /* Package copies align in stable source order, so the earlier package
     * keeps a contested subweb name whatever order they are discovered in. */
    for (size_t k = 0; k < part_count; k++) part_scope[k] = c->inputs.scopes[members[part_member[k]]];
    {
        pl_key_order order = {b, sources};
        for (size_t k = 1; k < part_count; k++)
            for (size_t m = k; m > 0 && pl_scope_order(&order, part_scope + m - 1, part_scope + m) > 0; m--) {
                size_t swap = part_member[m]; part_member[m] = part_member[m - 1]; part_member[m - 1] = swap;
                swap = part_scope[m]; part_scope[m] = part_scope[m - 1]; part_scope[m - 1] = swap;
            }
    }
    for (size_t k = 0; k < part_count; k++) parts[k] = texts[part_member[k]];
    if (base_member == SIZE_MAX && !stock_text) {
        if (part_count > 1) {
            kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
            snprintf(error, capacity, "%s and %s change the player hands web without its captured stock original",
                pl_scope_label(b, sources, part_scope[0]), pl_scope_label(b, sources, part_scope[1]));
            goto failed;
        }
        if (!(composed = malloc(parts[0].length + 1))) { snprintf(error, capacity, "player hands web allocation failed"); goto failed; }
        memcpy(composed, parts[0].text, parts[0].length); composed[parts[0].length] = 0; composed_length = parts[0].length;
    } else {
        sh_decl_conflict conflict = {SIZE_MAX, SIZE_MAX, 0, 0};
        if (!sh_decl_animweb_prepare(base_text, parts, part_count, owned, &moves, &move_count, error, capacity)) {
            if (!strstr(error, "allocation")) kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
            goto failed;
        }
        if (part_count == 1 && !owned[0] && !move_count) {
            /* One package copy the stock web needs no alignment for is served whole. */
            if (!(composed = malloc(parts[0].length + 1))) { snprintf(error, capacity, "player hands web allocation failed"); goto failed; }
            memcpy(composed, parts[0].text, parts[0].length); composed[parts[0].length] = 0; composed_length = parts[0].length;
        } else if (!(composed = sh_decl_block_compose("animweb", base_text, parts, part_count, &composed_length,
            error, capacity, &conflict))) {
            if (!strstr(error, "allocation")) {
                size_t used = strlen(error);
                kind = SH_PACKAGE_EDITOR_FAILURE_COVERAGE;
                if (conflict.first < part_count && conflict.second < part_count && used < capacity)
                    snprintf(error + used, capacity - used, "; %s and %s cannot compose together (product coverage)",
                        pl_scope_label(b, sources, part_scope[conflict.first]),
                        pl_scope_label(b, sources, part_scope[conflict.second]));
            }
            goto failed;
        }
    }
    for (size_t i = 0; i < count; i++) {
        size_t peer_count = 0;
        for (size_t j = 0; j < count; j++) if (j != i && c->inputs.scopes[members[j]] != b->count - 1)
            peers[peer_count++] = c->inputs.units[members[j]].files[0];
        if (!pl_contribution_stage(b, sources, members[i], composed, composed_length, peers, peer_count,
            pending + i, error, capacity)) goto failed;
    }
    for (size_t i = 0; i < count; i++) if (pending[i].owned && !pl_unit_keep(c, members[i])) {
        kind = SH_PACKAGE_EDITOR_FAILURE_ENVIRONMENT; culprit = SIZE_MAX;
        snprintf(error, capacity, "%s", "cannot keep a captured unit's record"); goto failed;
    }
    for (size_t i = 0; i < count; i++) {
        size_t unit = members[i];
        c->global[unit] = 1;
        if (!pending[i].owned) continue;
        c->dirty = 1;
        pl_contribution_unit_free(c->units + unit); c->units[unit] = pending[i]; memset(pending + i, 0, sizeof(*pending));
        c->inputs.units[unit].compilation = c->units[unit].owned;
        c->inputs.units[unit].files = &c->units[unit].file;
        c->inputs.units[unit].original_binding = 0;
    }
    /* Each package's weapons name the copies it now owns. */
    for (size_t m = 0; m < move_count; m++)
        if (!pl_hands_weapons(b, sources, part_scope[moves[m].source], moves[m].from, moves[m].to, error, capacity)) goto failed;
    ok = 1;
    goto done;
failed:
    pl_contribution_failure(b, sources, input, kind, culprit, error, capacity);
done:
    pl_stage_pool_close(c, &pool);
    free(stock_text);
    if (texts) for (size_t i = 0; i < count; i++) free((char *)texts[i].text);
    if (pending) for (size_t i = 0; i < count; i++) pl_contribution_unit_free(pending + i);
    if (owned) for (size_t i = 0; i <= count; i++) free(owned[i]);
    sh_decl_animweb_moves_free(moves, move_count);
    free(texts); free(parts); free((void *)peers); free(pending); free(part_member); free(part_scope); free(owned); free(composed);
    return ok;
}
static int pl_hands_phase(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    unsigned char *seen = calloc(c->inputs.count ? c->inputs.count : 1, 1);
    size_t *members = calloc(c->inputs.count ? c->inputs.count : 1, sizeof(*members));
    int ok = 0;
    if (!seen || !members) { snprintf(error, capacity, "player hands web allocation failed"); goto done; }
    /* Every scope reaches the web by its stock name, so what reaches it binds
     * alike in every scope (a model it names can name it again). Rebind the
     * tokens that way before composing, so equal records compare equal. */
    for (size_t i = 0; i < c->inputs.count; i++) {
        sh_package_link_unit *unit = c->inputs.units + i;
        if (unit->file_count == 1 && !strcmp(unit->family, "animweb") &&
            sh_package_source_link_player_hands(b->reader, unit->name) && !unit->stable_name) {
            if (!pl_unit_keep(c, i)) { snprintf(error, capacity, "%s", "cannot keep a captured unit's record"); goto done; }
            unit->stable_name = 1; c->dirty = 1;
        }
    }
    if (!pl_tokens(c, error, capacity)) goto done;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        size_t count = 0;
        if (seen[i] || unit->file_count != 1 || strcmp(unit->family, "animweb") ||
            !sh_package_source_link_player_hands(b->reader, unit->name)) continue;
        for (size_t j = i; j < c->inputs.count; j++) {
            const sh_package_link_unit *peer = c->inputs.units + j;
            if (seen[j] || peer->file_count != 1 || strcmp(peer->family, unit->family) || strcmp(peer->name, unit->name)) continue;
            seen[j] = 1; members[count++] = j;
        }
        if (!pl_hands_group(b, sources, members, count, error, capacity)) goto done;
    }
    ok = 1;
done:
    free(seen); free(members); return ok;
}

static int pl_unit_name_order(void *context, const void *left, const void *right)
{
    const sh_package_link_unit *units = context;
    const sh_package_link_unit *a = units + *(const size_t *)left, *b = units + *(const size_t *)right;
    int order = strcmp(a->family, b->family);
    if (!order) order = strcmp(a->name, b->name);
    return order ? order : (*(const size_t *)left < *(const size_t *)right ? -1 : 1);
}
/* Compose one phase's groups (runs of equal family and name in order), each
 * after the group of the parent it names, so a child rebases onto its
 * published parent. A cycle cannot order; the chain reader refuses it. */
static int pl_contribution_phase(pl_build *b, const sh_package_sources *sources, const size_t *order,
    size_t order_count, int phase, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    pl_group *groups = NULL;
    size_t group_count = 0, group_capacity = 0, *stack = NULL, depth = 0;
    unsigned char *state = NULL;
    int ok = 0;
    for (size_t i = 0; i < order_count;) {
        const sh_package_link_unit *unit = c->inputs.units + order[i];
        size_t end = i + 1;
        while (end < order_count && !strcmp(c->inputs.units[order[end]].family, unit->family) &&
            !strcmp(c->inputs.units[order[end]].name, unit->name)) end++;
        if (sh_package_editor_contribution_phase(unit->family) == phase) {
            if (group_count == group_capacity) {
                size_t grown_capacity = group_capacity ? group_capacity * 2 : 64;
                pl_group *grown = realloc(groups, grown_capacity * sizeof(*grown));
                if (!grown) { snprintf(error, capacity, "editor contribution allocation failed"); goto done; }
                groups = grown; group_capacity = grown_capacity;
            }
            groups[group_count++] = (pl_group){i, end - i, SIZE_MAX};
        }
        i = end;
    }
    if (!(state = calloc(group_count + 1, 1)) || !(stack = calloc(group_count + 1, sizeof(*stack)))) {
        snprintf(error, capacity, "editor contribution allocation failed"); goto done;
    }
    if (phase != 2) for (size_t g = 0; g < group_count; g++) {
        /* The parent the reference member (the stock one when present) names. */
        size_t reference = order[groups[g].first], length = 0;
        for (size_t k = 0; k < groups[g].count; k++)
            if (c->inputs.scopes[order[groups[g].first + k]] == b->count - 1) reference = order[groups[g].first + k];
        const sh_package_link_unit *unit = c->inputs.units + reference;
        unsigned char *body = sh_package_compilation_read(unit->compilation, unit->files[0], SIZE_MAX, &length, error, capacity);
        char *parent = NULL;
        if (!body) goto done;
        int read = pl_inherit_name((sh_decl_source){(const char *)body, length}, &parent);
        free(body);
        if (!read) continue; /* composition reports the unreadable text */
        for (size_t low = 0, high = group_count; parent && low < high;) {
            size_t middle = low + (high - low) / 2;
            const sh_package_link_unit *probe = c->inputs.units + order[groups[middle].first];
            int cmp = strcmp(probe->family, unit->family);
            if (!cmp) cmp = strcmp(probe->name, parent);
            if (!cmp) { groups[g].parent = middle; break; }
            if (cmp < 0) low = middle + 1; else high = middle;
        }
        free(parent);
    }
    for (size_t g = 0; g < group_count; g++) {
        if (state[g]) continue;
        stack[depth++] = g;
        while (depth) {
            size_t top = stack[depth - 1], parent = groups[top].parent;
            if (!state[top]) {
                state[top] = 1;
                if (parent != SIZE_MAX && !state[parent]) { stack[depth++] = parent; continue; }
            }
            depth--;
            if (state[top] == 2) continue;
            state[top] = 2;
            if (!pl_group_judge(b, sources, groups, group_count, top, order, order + groups[top].first,
                groups[top].count, pl_contribution_group, error, capacity)) goto done;
        }
    }
    ok = 1;
done:
    free(groups); free(stack); free(state); return ok;
}

/* The sites one unit binds, as reachability edges: discovery's own for a unit
 * the contributions left untouched, a replay of its binding otherwise. */
static int pl_certify_edges(pl_contributions *c, size_t i, pl_original_walk *walk, char *error, size_t capacity)
{
    const sh_package_link_unit *unit = c->inputs.units + i;
    for (size_t f = 0; f < unit->file_count; f++) {
        const sh_compiled_resource *file = unit->files[f];
        const sh_package_link_edge *sites = NULL;
        size_t n = 0, length = 0, bound_length = 0;
        if (pl_bound_sites(c, i, file, &sites, &n)) {
            for (size_t k = 0; k < n; k++) if (!pl_original_edge(walk, sites[k].target)) goto failed;
            continue;
        }
        unsigned char *body = sh_package_compilation_read(unit->compilation, file, SIZE_MAX, &length, error, capacity);
        if (!body) return 0;
        unsigned char *bound = pl_bound_bind(c, i, file, body, length, pl_original_reference, walk,
            &bound_length, error, capacity);
        free(body);
        if (!bound) return 0;
        free(bound);
    }
    for (size_t j = 0; j < unit->implicit_count; j++) if (!pl_original_edge(walk, unit->implicit[j].target)) goto failed;
    for (size_t j = 0; j < unit->conditional_count; j++) if (!pl_original_edge(walk, unit->conditional[j].target)) goto failed;
    return 1;
failed:
    snprintf(error, capacity, "cannot certify the shared name of %s", unit->name); return 0;
}
/* A shipped declaration no package authors keeps one class under its native
 * name only while that class binds nothing private. The native editor lists
 * every shipped entry and later contexts reach it by that name, so a class
 * reaching a package's own variant (a base row whose placement that package
 * changed, say) would publish the variant to all of them, and would lose the
 * name as soon as another context reached the shipped binding: the rescan
 * binds it as its scope's own, and so does this library. Composed entries
 * (those with a baseline member), package names and the player hands web
 * keep their names; the walk is the linker's original certification over
 * those fixed names. fallback marks the classes the one-class rule named. */
static int pl_shared_certify(pl_build *b, unsigned char *fallback, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    size_t count = c->inputs.count, pending = 0;
    for (size_t i = 0; i < count; i++) pending += fallback[i];
    if (!pending) return 1;
    sh_package_link_node *nodes = calloc(count, sizeof(*nodes));
    unsigned char *local = calloc(count, 1), *retained = calloc(count, 1), *seen = calloc(count, 1);
    unsigned char *denied = calloc(c->classes.count ? c->classes.count : 1, 1);
    size_t *queue = calloc(count, sizeof(*queue));
    int ok = 0;
    if (!nodes || !local || !retained || !seen || !denied || !queue) {
        snprintf(error, capacity, "cannot certify shared declarations"); goto done;
    }
    /* Only the named classes' closures decide; other units keep no edges. */
    pending = 0;
    for (size_t i = 0; i < count; i++) if (fallback[i]) { seen[i] = 1; queue[pending++] = i; }
    for (size_t at = 0; at < pending; at++) {
        size_t i = queue[at];
        const sh_package_link_unit *unit = c->inputs.units + i;
        int fixed = (c->shared[i] && pl_shared_declaration(unit->family) && !fallback[i]) || c->global[i];
        local[i] = fixed || fallback[i] || unit->original_binding == 1;
        if (!local[i] || fixed) continue;
        pl_original_walk walk = {c};
        if (!pl_certify_edges(c, i, &walk, error, capacity)) { free(walk.edges); goto done; }
        nodes[i].edges = walk.edges; nodes[i].edge_count = walk.count;
        for (size_t e = 0; e < walk.count; e++)
            if (!seen[walk.edges[e].target]) { seen[walk.edges[e].target] = 1; queue[pending++] = walk.edges[e].target; }
    }
    if (!sh_package_link_originals(nodes, count, local, retained, error, capacity)) goto done;
    /* Equal classes take one certification: any member reaching a private
     * unit keeps the whole class its scopes' own. */
    for (size_t i = 0; i < count; i++) if (fallback[i] && !retained[i]) denied[c->classes.nodes[i]] = 1;
    for (size_t i = 0; i < count; i++) if (fallback[i] && denied[c->classes.nodes[i]]) {
        c->shared[i] = 0; fallback[i] |= 2;
    }
    ok = 1;
done:
    for (size_t i = 0; nodes && i < count; i++) free((void *)nodes[i].edges);
    free(nodes); free(local); free(retained); free(seen); free(denied); free(queue); return ok;
}

static int pl_contributions_open(pl_build *b, const sh_package_sources *sources, char *error, size_t capacity)
{
    pl_contributions *c = calloc(1, sizeof(*c));
    const sh_package_link_inputs *original = sh_package_source_link_inputs(b->reader);
    size_t *members = NULL, *registrations = NULL, eligible = 0, registration_count;
    unsigned char *done = NULL, *named = NULL;
    if (!c) return 0;
    b->contributions = c; c->source = b->reader; c->inputs.count = original->count; c->build = b;
    c->dirty = 1;
    c->pending_unit = c->pending_target = SIZE_MAX;
    size_t reserved = pl_unit_room(original->count);
    if (!reserved) goto bad;
    c->unit_capacity = reserved;
    c->reuse = sh_package_link_reuse_open(reserved);
    /* The contribution phase extends and edits the capture's frozen unit table instead of a copy of it; the
     * captured records stay readable through sh_package_source_link_unit (R-343: the capture keeps owning the table
     * and keeps its own copy of a row before the phase first edits it). */
    c->inputs.units = sh_package_source_link_lend_units(b->reader, reserved); c->captured = original->count;
    c->units = calloc(reserved, sizeof(*c->units));
    c->shared = calloc(reserved, 1); c->global = calloc(reserved, 1);
    c->origins = calloc(reserved, sizeof(*c->origins)); c->scopes = calloc(reserved, sizeof(*c->scopes));
    c->implicit = calloc(reserved, sizeof(*c->implicit)); c->witnesses = calloc(reserved, sizeof(*c->witnesses));
    members = calloc(original->count ? original->count : 1, sizeof(*members));
    registrations = calloc(original->count ? original->count : 1, sizeof(*registrations));
    done = calloc(original->count ? original->count : 1, 1);
    if (!c->reuse || !c->inputs.units || !c->units || !c->shared || !c->global || !members || !registrations ||
        !done || !c->origins || !c->scopes || !c->implicit || !c->witnesses) goto bad;
    c->inputs.scopes = c->scopes;
    for (size_t i = 0; i < original->count; i++) { c->origins[i] = i; c->scopes[i] = original->scopes[i]; }
    unsigned long long mark = pl_now_ms();
    if (!pl_localize(b, sources, error, capacity)) goto bad;
    pl_step(b, PL_STEP_LOCALIZE, &mark);
    /* Group members once: eligible units sorted by family and name. */
    for (size_t i = 0; i < original->count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        if (unit->file_count == 1 && pl_modern(b, original->scopes[i]) && sh_package_editor_contribution_phase(unit->family))
            members[eligible++] = i;
    }
    qsort_s(members, eligible, sizeof(*members), pl_unit_name_order, c->inputs.units);
    /* A legacy context's conductor copy registers AI too: the engine finds the
     * conductor by its stock name, so it composes like a package copy. */
    memcpy(registrations, members, eligible * sizeof(*members));
    registration_count = eligible;
    for (size_t i = 0; i < original->count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        if (unit->file_count != 1 || pl_modern(b, original->scopes[i])) continue;
        int stated = pl_registration_file(unit->compilation, unit->files[0], error, capacity);
        if (stated < 0) goto bad;
        if (stated) registrations[registration_count++] = i;
    }
    qsort_s(registrations, registration_count, sizeof(*registrations), pl_unit_name_order, c->inputs.units);
    (void)done;
    /* Package keys move before any shared collection composes, so inspector
     * choices and registration rows already carry each package's final key. */
    pl_step(b, PL_STEP_REGISTRATION, &mark);
    if (b->reuse_in) {
        /* The offered record's identity templates (R-348), read only now: the identity cache's first pass takes them
         * and releases them; an unreadable or malformed section only leaves every template made afresh. */
        unsigned long long began = pl_now_ms();
        unsigned char *templates = NULL; size_t length = 0;
        b->templates_read = sh_package_source_reuse_read_templates(b->reuse_in, &templates, &length);
        if (b->templates_read > 0) {
            b->templates_read_bytes = length;
            /* Offered: a failure of this compile is retried without the record, as for any reuse. */
            if (sh_package_link_reuse_offer_templates(c->reuse, templates, length) == 1 && b->reuse_attempted)
                *b->reuse_attempted = 1;
        }
        b->templates_read_ms = pl_now_ms() - began;
    }
    if (!pl_tokens(c, error, capacity)) goto bad;
    pl_step(b, PL_STEP_TOKENS, &mark);
    if (!pl_spawn_capture(b, sources, error, capacity) || !pl_tokens(c, error, capacity)) goto bad;
    if (!pl_key_phase(b, sources, error, capacity)) goto bad;
    pl_step(b, PL_STEP_KEYS, &mark);
    b->unit_state_count = c->inputs.count;
    if (!(b->unit_state = calloc(b->unit_state_count ? b->unit_state_count : 1, 1))) {
        snprintf(error, capacity, "editor contribution allocation failed"); goto bad;
    }
    for (int phase = 1; phase <= 3; phase++) {
        if (!pl_tokens(c, error, capacity)) goto bad;
        pl_step(b, PL_STEP_TOKENS, &mark);
        if (!pl_contribution_phase(b, sources, members, eligible, phase, error, capacity)) goto bad;
        pl_step(b, PL_STEP_EDITOR, &mark);
    }
    if (!pl_tokens(c, error, capacity)) goto bad;
    pl_step(b, PL_STEP_TOKENS, &mark);
    if (!pl_registration_phase(b, sources, registrations, registration_count, error, capacity)) goto bad;
    pl_step(b, PL_STEP_REGISTRATION, &mark);
    if (!pl_contribution_found(b, error, capacity)) goto bad;
    if (!pl_tokens(c, error, capacity)) goto bad;
    pl_step(b, PL_STEP_TOKENS, &mark);
    if (!pl_hands_phase(b, sources, error, capacity)) goto bad;
    pl_step(b, PL_STEP_HANDS, &mark);
    if (!pl_tokens(c, error, capacity)) goto bad;
    pl_step(b, PL_STEP_TOKENS, &mark);
    /* Keep a verified baseline selector/bank slot. Other bound variants get
     * private native identities; historical contexts are never overwritten by
     * the modern field union. With no baseline, only one equal shared class can
     * occupy the authored name, a shipped name only while that class binds
     * nothing private (pl_shared_certify). Registry allocation still verifies
     * native IDs.
     * Units sharing a family and name are visited as one run of the sorted
     * index, in unit order. */
    size_t named_count = 0;
    named = calloc(c->inputs.count ? c->inputs.count : 1, 1);
    if (!named) goto bad;
    for (size_t i = 0; i < original->count; i++)
        if (pl_shared_declaration(c->inputs.units[i].family)) registrations[named_count++] = i;
    qsort_s(registrations, named_count, sizeof(*registrations), pl_unit_name_order, c->inputs.units);
    for (size_t start = 0, end; start < named_count; start = end) {
        const sh_package_link_unit *unit = c->inputs.units + registrations[start];
        size_t selected = SIZE_MAX, first = SIZE_MAX; int equal = 1, eligible = 0;
        for (end = start; end < named_count; end++) {
            size_t j = registrations[end];
            const sh_package_link_unit *peer = c->inputs.units + j;
            if (strcmp(unit->family, peer->family) || strcmp(unit->name, peer->name)) break;
            size_t binding = c->classes.nodes[j];
            if (first == SIZE_MAX) first = binding; else if (first != binding) equal = 0;
            /* A shared stock unit no baseline unit reaches is not a baseline member (R-344 stage A). */
            if (original->scopes[j] == b->count - 1 && !sh_package_source_link_stock_only(b->reader, j)) selected = binding;
            if (sh_package_source_link_shared(b->reader)[j]) eligible = 1;
        }
        int fallback = selected == SIZE_MAX && equal && eligible;
        if (fallback) selected = first;
        for (size_t k = start; k < end; k++) {
            size_t j = registrations[k];
            c->shared[j] = c->classes.nodes[j] == selected;
            if (fallback && c->shared[j] && c->inputs.units[j].original_binding) named[j] = 1;
        }
    }
    if (!pl_shared_certify(b, named, error, capacity)) goto bad;
    c->certification = named; named = NULL;
    pl_step(b, PL_STEP_SHARED, &mark);
    pl_tokens_free(c); free(members); free(registrations); free(done); free(named); return 1;
bad:
    if (error && capacity && !*error) snprintf(error, capacity, "cannot prepare the contribution phase's units");
    free(members); free(registrations); free(done); free(named); return 0;
}

/* Registration keys are bound like typed identities: the bit a package
 * authored (original) and the key it holds here (name, a private key or the
 * bit with keys added beside it), in its source context, so a map-authored
 * key keeps its package across differently keyed libraries. */
static int pl_key_provenance(const pl_build *b, const sh_package_sources *sources,
    sh_package_provenance *out, char *error, size_t capacity)
{
    for (size_t i = 0; i < b->key_count; i++) {
        const struct pl_key_binding *key = b->keys + i;
        char original[16], current[16];
        size_t selected;
        if (key->scope >= b->count) return 0;
        if (b->contexts) {
            const sh_package_provenance_context *recorded = b->contexts->contexts + key->scope;
            selected = sh_package_provenance_context_add(out, recorded->mode,
                recorded->trees, recorded->tree_count, recorded->component);
        } else {
            const sh_package_component *component = sources->components + b->views[key->scope].component;
            if (!sources->fingerprints || component->owner >= sources->package_count) return 0;
            selected = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_ISOLATED,
                (const unsigned char (*)[32])sh_package_source_tree(sources, component->owner),
                1, component->relative);
        }
        snprintf(original, sizeof(original), "%u", 1u << key->original);
        snprintf(current, sizeof(current), "%u", key->current);
        if (selected == SIZE_MAX ||
            !sh_package_provenance_bind(out, SH_PACKAGE_KEY_FAMILY, original, current, &selected, 1)) {
            snprintf(error, capacity, "cannot retain registration key %s as %s in context %zu", original, current, key->scope);
            return 0;
        }
    }
    return 1;
}

/* A module catalog card changed only through its blueprint models is one
 * global presentation slot under its native name (sl_presentation_slot). Of the
 * scopes that present one card, the earliest in library order keeps the name.
 * Every other authored scope's copy of that card, presented or original, is an
 * ordinary private identity, so the native name serves one presentation; the
 * coverage text records the choice. Equal presentations are one class. */
static int pl_presentation_note(char **coverage, const char *line)
{
    size_t used = *coverage ? strlen(*coverage) : 0, add = strlen(line);
    char *grown = add < SIZE_MAX - used ? realloc(*coverage, used + add + 1) : NULL;
    if (!grown) return 0;
    memcpy(grown + used, line, add + 1); *coverage = grown; return 1;
}
static int pl_presentation_slots(pl_build *b, const sh_package_sources *sources, char **coverage)
{
    pl_contributions *c = b->contributions;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        int first = 1;
        if (!sh_package_source_link_presentation(c->source, c->origins[i])) continue;
        for (size_t j = 0; j < c->inputs.count && first; j++)
            if (j != i && sh_package_source_link_presentation(c->source, c->origins[j]) &&
                !strcmp(c->inputs.units[j].family, unit->family) && !strcmp(c->inputs.units[j].name, unit->name) &&
                c->inputs.scopes[j] < c->inputs.scopes[i]) first = 0;
        if (!first) continue;
        size_t kept_owner = pl_scope_owner(b, sources, c->inputs.scopes[i]);
        const char *kept_name = kept_owner < sources->package_count ? sources->packages[kept_owner].name : "its scope";
        char line[1024];
        c->global[i] = 1;
        snprintf(line, sizeof(line), "%s %s: blueprint presentation from %s, under the card's native name\n",
            unit->family, unit->name, kept_name);
        if (!pl_presentation_note(coverage, line)) return 0;
        for (size_t j = 0; j < c->inputs.count; j++) {
            sh_package_link_unit *other = c->inputs.units + j;
            if (j == i || c->inputs.scopes[j] == c->inputs.scopes[i] ||
                strcmp(other->family, unit->family) || strcmp(other->name, unit->name)) continue;
            /* No other scope's card keeps a claim on the native name. A card
             * its module's variant carries (changed beyond its presentation:
             * carried, not stable) is published where that variant derives it
             * and nowhere else; any other copy is its own private publication.
             * A stock card the slot now presents stays a private stock copy. */
            if (!pl_unit_keep(c,j)) return 0;
            other = c->inputs.units+j;
            int variant = other->carried && !other->stable_name;
            other->original_binding = 0;
            if (!variant) other->carried = 0;
            if (c->inputs.scopes[j] >= b->authored_count) continue;
            size_t owner = pl_scope_owner(b, sources, c->inputs.scopes[j]);
            int presented = sh_package_source_link_presentation(c->source, c->origins[j]);
            /* Its module, when the scope has one, reaches a private copy by the
             * native name only (stable), so it shows the presentation. */
            if (variant)
                snprintf(line, sizeof(line), "%s %s: %s's changed card stays with its module variant; the card's "
                    "native name shows %s's\n", unit->family, unit->name,
                    owner < sources->package_count ? sources->packages[owner].name : "another scope", kept_name);
            else snprintf(line, sizeof(line), "%s %s: %s's %s stays a private copy; the card shows %s's\n",
                unit->family, unit->name, owner < sources->package_count ? sources->packages[owner].name : "another scope",
                presented ? "blueprint presentation" : "unchanged card", kept_name);
            if (!pl_presentation_note(coverage, line)) return 0;
        }
    }
    return 1;
}

/* A shared stock unit records no binding (R-344 stage A): its files are captured originals, its closure has no
 * gameplay owner and it keeps its original name, so each scope's bindings are those of its own units, in order. */
static int pl_provenance(const pl_build *b, const sh_package_sources *sources,
    sh_package_provenance *out)
{
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(b->reader);
    for (size_t i = 0; i < inputs->count; i++) {
        const sh_package_link_unit *unit = sh_package_source_link_unit(b->reader, i);
        size_t scope = inputs->scopes[i], selected;
        int authored = 0;
        int private_binding = strcmp(unit->name, b->linked.names[b->linked.classes.nodes[i]]) != 0;
        if (!b->contexts && sh_package_source_link_shared(b->reader)[i] && !private_binding) continue;
        for (size_t f = 0; f < unit->file_count; f++) if (unit->files[f]->source_count) authored = 1;
        /* A reverse-discovered selector/entity can keep original source text
         * while reaching a changed scoped dependency. Its recipe belongs to
         * that authored context even though its own file has no source owner. */
        if (!authored && (scope >= b->authored_count ||
            (sh_package_source_link_shared(b->reader)[i] && !private_binding))) continue;
        if (scope >= b->count) return 0;
        if (b->contexts) {
            const sh_package_provenance_context *recorded = b->contexts->contexts + scope;
            selected = sh_package_provenance_context_add(out, recorded->mode,
                recorded->trees, recorded->tree_count, recorded->component);
        } else {
            const sh_package_component *component = sources->components + b->views[scope].component;
            if (!sources->fingerprints || component->owner >= sources->package_count) return 0;
            selected = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_ISOLATED,
                (const unsigned char (*)[32])sh_package_source_tree(sources, component->owner),
                1, component->relative);
        }
        if (selected == SIZE_MAX || !sh_package_provenance_bind(out, unit->family, unit->name,
            b->linked.names[b->linked.classes.nodes[i]], &selected, 1)) return 0;
    }
    return 1;
}

/* Descriptor weapon names resolve in the same lexical component as assets.
 * An absent file keeps its typed original weapon name; policy alone never
 * manufactures a resource, dependency or gameplay owner. */
static int pl_hud(pl_build *b, char *error, size_t capacity)
{
    sh_package_policy *policy = &b->linked.compiled->policy;
    const sh_package_link_inputs *inputs = &b->contributions->inputs;
    policy->hud_bound = 1;
    for (size_t scope = 0; scope < b->authored_count; scope++) {
        const sh_package_sources *sources = &b->views[scope].sources;
        for (size_t component = 0; component < sources->component_count; component++) {
            const sh_package_component *owner = sources->components + component;
            const char *raw = sh_package_descriptor_section(&owner->descriptor, "hud");
            sh_json_object hud = {0}, rows = {0}; int ok = 0;
            if (!raw) continue;
            if (!sh_json_parse_object(raw, strlen(raw), 5, &hud)) goto row_done;
            if (!hud.count) { ok = 1; goto row_done; }
            raw = sh_json_object_get(&hud, "weapons");
            if (!raw || !sh_json_parse_object(raw, strlen(raw), 3, &rows)) goto row_done;
            for (size_t r = 0; r < rows.count; r++) {
                const sh_json_member *row = rows.members + r;
                int mode = 0, shadowed = 0;
                if (!sh_package_hud_mode(row->value_json, &mode)) goto row_done;
                /* A nested component's explicit row overrides its inherited
                 * parent policy in that scope, not its sibling's weapon. */
                if (b->views[scope].component != SIZE_MAX)
                    for (size_t j = 0; !shadowed && j < sources->component_count; j++) {
                        const sh_package_component *nearer = sources->components + j;
                        if (nearer->owner != owner->owner || strlen(nearer->relative) <= strlen(owner->relative)) continue;
                        const char *next_raw = sh_package_descriptor_section(&nearer->descriptor, "hud");
                        sh_json_object next = {0}, weapons = {0};
                        if (next_raw && sh_json_parse_object(next_raw, strlen(next_raw), 5, &next)) {
                            next_raw = sh_json_object_get(&next, "weapons");
                            if (next_raw && sh_json_parse_object(next_raw, strlen(next_raw), 3, &weapons))
                                shadowed = sh_json_object_get(&weapons, row->key) != NULL;
                        }
                        sh_json_object_free(&next); sh_json_object_free(&weapons);
                    }
                if (shadowed) continue;
                const char *name = row->key;
                size_t unit = sh_package_link_inputs_find(inputs, scope, "weapon", name);
                /* With shared stock the captured order need not ascend by scope (sh_package_source_link_inputs): the
                 * scope's own captured weapon by its index, or the shared stock weapon it binds (R-344 stage A). */
                if (unit == SIZE_MAX && !sh_package_source_link_find_unit(b->reader, scope, "weapon", name, &unit))
                    sh_package_source_link_scope_find(b->reader, scope, "weapon", name, &unit);
                if (unit != SIZE_MAX) name = b->linked.names[b->linked.classes.nodes[unit]];
                sh_package_owners supplier = {0};
                int bound = sh_package_owners_add(&supplier, owner->owner) && sh_package_hud_bind(policy, name, mode, &supplier);
                sh_package_owners_free(&supplier); if (!bound) goto row_done;
            }
            ok = 1;
row_done:
            sh_json_object_free(&hud); sh_json_object_free(&rows);
            if (!ok) { if (!*error) snprintf(error, capacity, "cannot bind scoped weapon HUD policy"); return 0; }
        }
    }
    return sh_package_hud_publish(policy, 0, 0);
}

static int pl_consumers(const pl_contributions *c, const sh_package_link_output *linked,
    sh_package_consumers **out, char *error, size_t capacity)
{
    const sh_package_link_inputs *inputs = &c->inputs;
    if (!linked->implicit_count && !c->movie_string_count) return 1;
    sh_package_consumer *owners = NULL;
    sh_package_consumer_target *targets = NULL;
    size_t count = 0, total, *sizes = NULL, *indexes = NULL;
    if (c->movie_string_count > SIZE_MAX - linked->implicit_count) goto done;
    total = linked->implicit_count + c->movie_string_count;
    if (total > SIZE_MAX / sizeof(*targets) || linked->classes.count > SIZE_MAX / sizeof(*owners)) goto done;
    owners = calloc(linked->classes.count, sizeof(*owners)); targets = calloc(total, sizeof(*targets));
    sizes = calloc(linked->classes.count, sizeof(*sizes)); indexes = calloc(linked->classes.count, sizeof(*indexes));
    if (!owners || !targets || !sizes || !indexes) goto done;
    for (size_t i = 0; i < linked->implicit_count; i++) sizes[linked->implicit[i].owner]++;
    for (size_t i = 0; i < c->movie_string_count; i++) sizes[linked->classes.nodes[c->movie_strings[i]->unit]]++;
    size_t offset = 0;
    for (size_t i = 0; i < linked->classes.count; i++) if (sizes[i]) {
        const sh_package_link_unit *owner = inputs->units + linked->classes.representatives[i];
        indexes[i] = count;
        owners[count++] = (sh_package_consumer){owner->family, linked->names[i], targets + offset, 0};
        offset += sizes[i];
    }
    for (size_t i = 0; i < linked->implicit_count; i++) {
        const sh_package_link_implicit_binding *binding = linked->implicit + i;
        const sh_package_link_unit *target = inputs->units + linked->classes.representatives[binding->target];
        sh_package_consumer *owner = owners + indexes[binding->owner];
        ((sh_package_consumer_target *)owner->targets)[owner->count++] =
            (sh_package_consumer_target){binding->slot, target->family, linked->names[binding->target]};
    }
    for (size_t i = 0; i < c->movie_string_count; i++) {
        const pl_movie_string *value = c->movie_strings[i];
        sh_package_consumer *owner = owners + indexes[linked->classes.nodes[value->unit]];
        ((sh_package_consumer_target *)owner->targets)[owner->count++] =
            (sh_package_consumer_target){value->slot, "string", value->key};
    }
    *out = sh_package_consumers_create(owners, count, error, capacity);
done:
    free(owners); free(targets); free(sizes); free(indexes);
    if (!*out && error && capacity && !*error) snprintf(error, capacity, "cannot retain compiled native consumer bindings");
    return *out != NULL;
}

/* AI state-machine webs (fsm_web.h, package_fsm.h). A linked behavior keeps
 * conditional state-machine bindings only to declarations that changed in its
 * scope; each such behavior builds a private web: its scope's first
 * declaration (generated, with a private configuration copying the one it
 * names), the changed stock declarations' linked replacements and the
 * declarations its scope adds. Every product state-machine declaration of
 * this link is owned, so no other web meets it; the generated declarations
 * and the placeholder are added to the provider. */
static const sh_compiled_resource *pl_fsm_declaration(const sh_package_compilation *compiled, const char *name)
{
    for (size_t i = 0; i < compiled->resource_count; i++)
        if (sh_package_resource_native_matches(compiled->resources + i, "aifsmmanager", name)) return compiled->resources + i;
    return NULL;
}
static char *pl_fsm_original(sh_package_baseline_reader original, void *context, const char *name, size_t *length)
{
    unsigned char *body = NULL;
    size_t size = strlen(name) + 40;
    char *path = malloc(size);
    if (!path) return NULL;
    snprintf(path, size, "generated/decls/aifsmmanager/%s.decl", name);
    int status = original ? original(context, path, &body, length) : 0;
    free(path);
    if (status != 1) { free(body); return NULL; }
    return (char *)body;
}
static int pl_fsm_webs(pl_build *b, sh_package_baseline_reader original, void *original_context,
    sh_package_fsm_webs **out, char *error, size_t capacity)
{
    pl_contributions *c = b->contributions;
    const sh_package_link_output *linked = &b->linked;
    const sh_package_link_inputs *inputs = &c->inputs;
    sh_package_fsm_bound *bound = NULL;
    const char **owned = NULL, **pairs = NULL;
    char **firsts = NULL;
    size_t bound_count = 0, owned_count = 0, classes = linked->classes.count;
    int ok = 0;
    *out = NULL;
    bound = calloc(classes ? classes : 1, sizeof(*bound));
    owned = calloc(classes ? classes : 1, sizeof(*owned));
    firsts = calloc(classes ? classes : 1, sizeof(*firsts));
    /* from, to and extra per bound behavior, carved from one array. */
    pairs = calloc(3 * (linked->implicit_count + 1), sizeof(*pairs));
    if (!bound || !owned || !firsts || !pairs) { snprintf(error, capacity, "cannot allocate state-machine webs"); goto done; }
    for (size_t k = 0; k < classes; k++) {
        const sh_package_link_unit *unit = inputs->units + linked->classes.representatives[k];
        if (!strcmp(unit->family, "aifsmmanager") && strcmp(linked->names[k], unit->name)) owned[owned_count++] = linked->names[k];
    }
    for (size_t i = 0, used = 0; i < linked->implicit_count;) {
        size_t owner = linked->implicit[i].owner, end = i, first_target = SIZE_MAX, n = 0, extras = 0;
        const sh_package_link_unit *unit = inputs->units + linked->classes.representatives[owner];
        while (end < linked->implicit_count && linked->implicit[end].owner == owner) end++;
        const char **from = pairs + used, **to = from + (end - i), **extra = to + (end - i);
        for (size_t j = i; j < end; j++) {
            const sh_package_link_implicit_binding *binding = linked->implicit + j;
            if (strncmp(binding->slot, "fsm:", 4) || strcmp(unit->family, "aibehavior")) continue;
            const char *name = binding->slot + 4, *target = linked->names[binding->target];
            if (!strcmp(name, SH_PACKAGE_FSM_FIRST)) first_target = binding->target;
            /* The first declaration is always replaced (its generated copy carries the configuration). */
            if (!strcmp(name, SH_PACKAGE_FSM_FIRST) || sh_package_source_link_fsm_stock(c->source, name)) {
                from[n] = name; to[n++] = target;
            } else extra[extras++] = target;
        }
        used += 3 * (end - i); i = end;
        if (!n && !extras) continue;
        sh_package_fsm_bound *entry = bound + bound_count;
        size_t origin = c->origins[linked->classes.representatives[owner]];
        entry->behavior = linked->names[owner];
        entry->config = sh_package_source_link_fsm_config(c->source, origin);
        if (!entry->config) {
            snprintf(error, capacity, "behavior %s binds state machines without a layer configuration", unit->name); goto done;
        }
        /* Its scope's first declaration as linked: the private copy when it
         * changed there, else the installed original. */
        size_t length = 0;
        if (first_target != SIZE_MAX) {
            const sh_compiled_resource *first = pl_fsm_declaration(linked->compiled, linked->names[first_target]);
            firsts[bound_count] = first ? (char *)sh_package_compilation_read(linked->compiled, first, SIZE_MAX,
                &length, error, capacity) : NULL;
        } else firsts[bound_count] = pl_fsm_original(original, original_context, SH_PACKAGE_FSM_FIRST, &length);
        if (!firsts[bound_count]) {
            if (!*error) snprintf(error, capacity, "%s is unavailable for behavior %s", SH_PACKAGE_FSM_FIRST, unit->name);
            goto done;
        }
        entry->first = (sh_decl_source){firsts[bound_count], length};
        entry->from = from; entry->to = to; entry->count = n; entry->extra = extra; entry->extra_count = extras;
        bound_count++;
    }
    *out = sh_package_fsm_webs_build(bound, bound_count, owned, owned_count, error, capacity);
    if (!*out) goto done;
    if (!(*out)->count && !(*out)->owned_count) { sh_package_fsm_webs_free(*out); *out = NULL; ok = 1; goto done; }
    for (size_t i = 0; i < (*out)->generated_count; i++) {
        const sh_package_fsm_declaration *generated = (*out)->generated + i;
        if (!sh_package_compilation_add_declaration(linked->compiled, "aifsmmanager", generated->name,
            (const unsigned char *)generated->text, generated->length, 0, error, capacity)) goto done;
    }
    if ((*out)->owned_count) {
        size_t stock_length = 0, length = 0;
        char *stock = pl_fsm_original(original, original_context, SH_PACKAGE_FSM_PLACEHOLDER_SOURCE, &stock_length);
        char *placeholder = stock ? sh_package_fsm_placeholder((sh_decl_source){stock, stock_length}, &length, error, capacity) : NULL;
        int added = placeholder && sh_package_compilation_add_declaration(linked->compiled, "aifsmmanager",
            SH_PACKAGE_FSM_PLACEHOLDER, (const unsigned char *)placeholder, length, 3, error, capacity);
        if (!stock && !*error) snprintf(error, capacity, "%s is unavailable for the state-machine placeholder", SH_PACKAGE_FSM_PLACEHOLDER_SOURCE);
        free(stock); free(placeholder);
        if (!added) goto done;
    }
    ok = 1;
done:
    for (size_t i = 0; firsts && i < (classes ? classes : 1); i++) free(firsts[i]);
    free(firsts); free(bound); free((void *)owned); free((void *)pairs);
    if (!ok) { sh_package_fsm_webs_free(*out); *out = NULL; }
    return ok;
}

/* Project native registration meaning without changing the authored source.
 * This same preparation applies to explicit files and later discovered roots. */
static int pl_registration_prepare(sh_package_source_reader *reader, const char *name,
    const unsigned char *body, size_t length, unsigned char **output, size_t *output_length,
    char *error, size_t capacity)
{
    static const char *const fields[] = {"aiTypeList", "globalAIMods"};
    unsigned char *expanded = NULL; size_t expanded_length = 0;
    char *state = NULL;
    int status = 0;
    *output = NULL; *output_length = 0;
    if (strstr((const char *)body, "inherit")) {
        if (!reader) return -1;
        status = sh_package_source_entity_fields_state(reader, name, body, length, fields, 2,
            &expanded, &expanded_length, &state, error, capacity);
        if (status < 0) { free(state); return status == -2 ? -3 : status; }
    }
    const unsigned char *current = expanded ? expanded : body;
    size_t current_length = expanded ? expanded_length : length;
    if (strstr((const char *)current, "aiTypeList") || strstr((const char *)current, "globalAIMods")) {
        char *prepared = NULL; size_t prepared_length = 0;
        status = sh_package_spawn_registry_prepare_state((sh_decl_source){(const char *)current, current_length},
            state, &prepared, &prepared_length, error, capacity);
        free(expanded); expanded = (unsigned char *)prepared; expanded_length = prepared_length;
        if (status < 0) { free(state); return status; }
    }
    free(state);
    if (!expanded) return 0;
    if (expanded_length == length && !memcmp(expanded, body, length)) { free(expanded); return 0; }
    *output = expanded; *output_length = expanded_length; return 1;
}
static int pl_prepare_discovered(void *context, sh_package_source_reader *reader,
    const sh_package_link_unit *unit, unsigned char **output, size_t *output_length,
    char *error, size_t capacity)
{
    (void)context;
    if (strcmp(unit->family, "entitydef") || unit->file_count != 1) return 0;
    size_t length = 0;
    unsigned char *body = sh_package_compilation_read(unit->compilation, unit->files[0], SIZE_MAX,
        &length, error, capacity);
    if (!body) return -1;
    int status = pl_registration_prepare(reader, unit->name, body, length, output, output_length, error, capacity);
    free(body); return status;
}

/* Prepare inherited registration state before restoring original rows. A child
 * may change only one modifier field and inherit the list count and AI keys.
 * The projection is compiler-owned; delivery bytes and other edit fields stay
 * untouched. Every parent resolves in this scope and the captured originals. */
static int pl_registration_inheritance(pl_build *b, const sh_package_sources *sources, size_t begin,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, char *error, size_t capacity)
{
    if (!b->native || !b->native->private_keys) return 1;
    for (size_t scope = begin; scope < b->count; scope++) {
        sh_package_compilation *source = b->scopes[scope];
        sh_package_source_reader *reader = NULL;
        size_t count = source->resource_count;
        sh_package_resource_projection *projections = calloc(count ? count : 1, sizeof(*projections));
        const sh_compiled_resource **inputs = calloc(count ? count : 1, sizeof(*inputs));
        unsigned char **owned = calloc(count ? count : 1, sizeof(*owned));
        int ok = projections && inputs && owned, changed = 0, found = 0;
        for (size_t i = 0; ok && i < count; i++) {
            const sh_compiled_resource *r = source->resources + i;
            inputs[i] = r; projections[i] = (sh_package_resource_projection){r->engine_path, inputs + i, 1,
                NULL, 0, r->native_type, r->native_name};
            if (!r->type || strcmp(r->type, "entitydef")) continue;
            size_t length = 0;
            unsigned char *body = sh_package_compilation_read(source, r, SIZE_MAX, &length, error, capacity);
            if (!body) { ok = 0; break; }
            if (strstr((const char *)body, "inherit")) {
                if (!reader) reader = sh_package_source_reader_open(source, catalog, original, original_context,
                    registry, reflection, error, capacity);
            }
            int status = pl_registration_prepare(reader, r->name, body, length, owned + i,
                &projections[i].length, error, capacity);
            ok = status >= 0;
            /* A package's own input (-2 malformed, -3 not covered) decides
             * for that package alone; stock and product input blames nobody. */
            if (status == -2 || status == -3) {
                size_t owner = r->source_count ? pl_scope_owner(b, sources, scope) : SIZE_MAX;
                if (owner == SIZE_MAX) b->contribution_verdict = 1;
                else found = pl_contribution_blame(b, sources, owner, status == -2, error, capacity);
            }
            if (status == 1) { projections[i].body = owned[i]; changed = 1; }
            free(body);
        }
        sh_package_source_reader_close(reader);
        sh_package_compilation *next = ok && changed ? sh_package_compilation_project(source->sources,
            projections, count, &source->policy, source->canonical_path, error, capacity) : NULL;
        if (ok && changed) ok = next != NULL;
        if (owned) for (size_t i = 0; i < count; i++) free(owned[i]);
        free(owned); free(inputs); free(projections);
        if (!ok) {
            sh_package_compilation_free(next);
            /* Each scope is prepared on its own: the next one is judged apart. */
            if (found) { b->contribution_verdict = 0; continue; }
            return 0;
        }
        if (next) {
            next->independent_sources = source->independent_sources;
            next->map_overlay = source->map_overlay; next->map_owner_begin = source->map_owner_begin;
            next->map_independent_sources = source->map_independent_sources;
            if (PL_SHARE) sh_package_compilation_share(next);
            b->scopes[scope] = next; sh_package_compilation_free(source);
        }
    }
    return pl_contribution_found(b, error, capacity);
}

/* Modern registration contributions extend the native table. Materialize
 * that effective input before dependency discovery: a modifier-only package
 * still refers to the AI its modifier classifies. Historical complete tables
 * retain their recorded omissions and first-match behavior. */
/* Parents of a scope's compiled declaration before linking: by declaration
 * name in that scope, then stock, then the captured originals, which hold a
 * stock parent no scope names, as registration inheritance resolves it
 * (sh_decl_entity_class_source.read). */
typedef struct pl_scope_parents {
    const sh_package_compilation *scope, *stock;
    sh_package_baseline_reader original; void *original_context;
} pl_scope_parents;
static int pl_scope_name_equal(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = *a == '\\' ? '/' : tolower((unsigned char)*a), y = *b == '\\' ? '/' : tolower((unsigned char)*b);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int pl_scope_parent_read(void *context, const char *name, sh_decl_source *out)
{
    const pl_scope_parents *p = context;
    for (int pass = 0; pass < 2; pass++) {
        const sh_package_compilation *compiled = pass ? p->stock : p->scope;
        for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
            const sh_compiled_resource *r = compiled->resources + i;
            char ignored[256];
            size_t length = 0;
            if (!r->type || !r->name || strcmp(r->type, "entitydef") || !pl_scope_name_equal(r->name, name)) continue;
            unsigned char *body = sh_package_compilation_read(compiled, r, SIZE_MAX, &length, ignored, sizeof(ignored));
            if (!body) return -1;
            *out = (sh_decl_source){(char *)body, length}; return 1;
        }
    }
    if (!p->original) return 0;
    unsigned char *body = NULL; size_t length = 0;
    size_t size = strlen(name) + sizeof("generated/decls/entitydef/.decl");
    char *raw = malloc(size), *path = NULL;
    if (raw) { snprintf(raw, size, "generated/decls/entitydef/%s.decl", name); path = sh_package_engine_path(raw); }
    free(raw);
    if (!path) return -1;
    int read = p->original(p->original_context, path, &body, &length);
    free(path);
    if (read > 0 && body) { *out = (sh_decl_source){(char *)body, length}; return 1; }
    free(body); return read < 0 ? -1 : 0;
}
/* The resolved state (pl_registration_state's contract) of a text read in
 * one scope; 0 with error when undecided (malformed, missing or coverage). */
static int pl_scope_state(const sh_package_compilation *scope, const sh_package_compilation *stock,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_source text, char **state, char *error, size_t capacity)
{
    pl_scope_parents parents = {scope, stock, original, original_context};
    sh_decl_entity_class_source source = {&parents, pl_scope_parent_read, NULL};
    sh_decl_entity_merge_failure failure = SH_DECL_ENTITY_MERGE_DECIDED;
    sh_decl_node *tree = sh_decl_tree_parse_ordered(text, error, capacity);
    int merges = tree ? sh_decl_entity_merges_classified(tree, &source, state, &failure, error, capacity) : -1;
    sh_decl_tree_free(tree);
    if (merges == 0) { free(*state); *state = _strdup(""); }
    if (merges >= 0 && *state) return 1;
    free(*state); *state = NULL;
    if (merges >= 0) snprintf(error, capacity, "registration state allocation failed");
    else if (failure == SH_DECL_ENTITY_MERGE_COVERAGE && !strstr(error, "product coverage")) {
        size_t used = strlen(error);
        snprintf(error + used, capacity - used, " (product coverage)");
    }
    return 0;
}
static int pl_registration_sources(pl_build *b, const sh_package_sources *sources,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, char *error, size_t capacity)
{
    if (!b->native || !b->native->private_keys || !b->authored_count) return 1;
    sh_package_key_space space = {0};
    sh_decl_native_schema *schema = sh_decl_native_schema_open(
        (sh_decl_native_source){registry.context, registry.read, reflection, registry.scripts},
        (sh_decl_dependency_observer){0}, error, capacity);
    if (!schema) return 0;
    int reflected = sh_package_key_space_read(schema, &space);
    sh_decl_native_schema_close(schema);
    if (reflected < 0) return 0;
    const sh_package_compilation *stock = b->scopes[b->count - 1];
    for (size_t scope = 0; scope < b->authored_count; scope++) {
        if (!pl_modern(b, scope)) continue;
        sh_package_compilation *source = b->scopes[scope];
        size_t count = source->resource_count;
        sh_package_resource_projection *projections = calloc(count ? count : 1, sizeof(*projections));
        const sh_compiled_resource **inputs = calloc(count ? count : 1, sizeof(*inputs));
        unsigned char **owned = calloc(count ? count : 1, sizeof(*owned));
        int ok = projections && inputs && owned, changed = 0, found = 0;
        for (size_t i = 0; ok && i < count; i++) {
            const sh_compiled_resource *r = source->resources + i;
            inputs[i] = r; projections[i] = (sh_package_resource_projection){r->engine_path, inputs + i, 1,
                NULL, 0, r->native_type, r->native_name};
            if (!r->type || strcmp(r->type, "entitydef")) continue;
            const sh_compiled_resource *base = sh_package_compilation_find(stock, r->engine_path);
            if (!base || !base->body || (!strstr((char *)base->body, "aiTypeList") &&
                !strstr((char *)base->body, "globalAIMods"))) continue;
            size_t length = 0;
            unsigned char *body = sh_package_compilation_read(source, r, SIZE_MAX, &length, error, capacity);
            /* Both texts read the statement their own inheritance selects. */
            char *state = NULL, *base_state = NULL;
            int stated = body && pl_scope_state(source, stock, original, original_context,
                (sh_decl_source){(char *)body, length}, &state, error, capacity) &&
                pl_scope_state(stock, stock, original, original_context,
                (sh_decl_source){(char *)base->body, base->body_length}, &base_state, error, capacity);
            sh_decl_node *tree = stated ? pl_entity_parse((sh_decl_source){(char *)body, length}, state, error, capacity) : NULL;
            sh_decl_node *original = stated ? pl_entity_parse((sh_decl_source){(char *)base->body, base->body_length},
                base_state, error, capacity) : NULL;
            int update = 0;
            sh_decl_node *edit = tree ? (sh_decl_node *)sh_decl_entity_state_statement(tree, state, &update) : NULL;
            int restored = 0;
            size_t owner = !tree && body && !strstr(error, "allocation") ? pl_scope_owner(b, sources, scope) : SIZE_MAX;
            if (owner != SIZE_MAX) found = pl_contribution_blame(b, sources, owner, stated, error, capacity);
            if (tree && !edit) {
                edit = calloc(1, sizeof(*edit));
                if (edit) edit->key = _strdup(*state ? state : "edit");
                if (!edit || !edit->key) { sh_decl_tree_free(edit); edit = NULL; }
                else {
                    /* After every statement: the positional headers stay first. */
                    sh_decl_node **tail = &tree->children;
                    while (*tail) tail = &(*tail)->next;
                    edit->assignment = edit->compound = 1; *tail = edit;
                }
            }
            ok = tree && original && edit && edit->compound;
            const char *lists[] = {"aiTypeList", "globalAIMods"};
            for (size_t j = 0; ok && j < 2; j++) {
                const sh_decl_node *from = pl_list(original, base_state, lists[j]);
                sh_decl_node *into = pl_list(tree, state, lists[j]);
                if (!from) continue;
                if (!into) {
                    into = pl_node_clone(from);
                    if (!into) { ok = 0; break; }
                    sh_decl_node **tail = &edit->children;
                    while (*tail) tail = &(*tail)->next;
                    *tail = into; restored = 1;
                } else ok = pl_key_restore(into, from, reflected ? &space : NULL, &restored) && pl_list_renumber(into);
            }
            if (ok && restored) {
                owned[i] = (unsigned char *)sh_decl_tree_write(tree, &projections[i].length);
                projections[i].body = owned[i]; ok = owned[i] != NULL; changed = 1;
            }
            sh_decl_tree_free(tree); sh_decl_tree_free(original); free(body); free(state); free(base_state);
        }
        sh_package_compilation *next = ok && changed ? sh_package_compilation_project(source->sources,
            projections, count, &source->policy, source->canonical_path, error, capacity) : NULL;
        if (ok && changed) ok = next != NULL;
        if (owned) for (size_t i = 0; i < count; i++) free(owned[i]);
        free(owned); free(inputs); free(projections);
        if (!ok) {
            sh_package_compilation_free(next);
            if (found) { b->contribution_verdict = 0; continue; }
            return 0;
        }
        if (next) {
            next->independent_sources = source->independent_sources;
            next->map_overlay = source->map_overlay; next->map_owner_begin = source->map_owner_begin;
            next->map_independent_sources = source->map_independent_sources;
            if (PL_SHARE) sh_package_compilation_share(next);
            b->scopes[scope] = next; sh_package_compilation_free(source);
        }
    }
    return pl_contribution_found(b, error, capacity);
}

/* Diagnostics: stock units no package supplies (every file an unsupplied
 * original) that a scope's binding serves under a private name, with the
 * first few reasons. Accounting only; it never changes a binding. */
/* A unit whose bound text left its native semantics undecided (the source
 * binding's coverage) marks every resource of its linked class, so no later
 * consumer reads served-as-authored bytes as a decided native state. */
static void pl_semantic_coverage(pl_build *b)
{
    const pl_contributions *c = b->contributions;
    if (!c || !c->source || !b->linked.compiled || !b->linked.names) return;
    for (size_t i = 0; i < c->inputs.count; i++) {
        const sh_package_link_unit *unit = c->inputs.units + i;
        if (!sh_package_source_link_coverage(c->source, c->origins[i], c->units[i].owned != NULL)) continue;
        const char *name = b->linked.names[b->linked.classes.nodes[i]];
        for (size_t r = 0; name && r < b->linked.compiled->resource_count; r++) {
            sh_compiled_resource *resource = b->linked.compiled->resources + r;
            if (resource->type && resource->name && !strcmp(resource->type, unit->family) &&
                !_stricmp(resource->name, name)) resource->semantic_coverage = 1;
        }
    }
}
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
/* Strict materialization oracle (sh_package_source_link_share mode 2): every unit the capture would share ends with
 * its representative's class and linked name, both unedited beyond their class-wide fixed and stable names. */
static size_t g_pl_share_checked, g_pl_share_violations;
void sh_package_library_test_share_checks(size_t *checked, size_t *violations)
{ *checked = g_pl_share_checked; *violations = g_pl_share_violations; }
static int pl_row_unedited(const sh_package_link_unit *a, const sh_package_link_unit *b)
{
    return a->family == b->family && a->name == b->name && a->compilation == b->compilation && a->files == b->files &&
        a->file_count == b->file_count && !memcmp(a->semantics, b->semantics, 32) &&
        a->original_binding == b->original_binding && a->carried == b->carried && a->witnesses == b->witnesses &&
        a->witness_count == b->witness_count && a->file_identities == b->file_identities && a->implicit == b->implicit &&
        a->implicit_count == b->implicit_count && a->conditional == b->conditional &&
        a->conditional_count == b->conditional_count && a->implicit_owners == b->implicit_owners &&
        a->implicit_gameplay_owners == b->implicit_gameplay_owners;
}
static void pl_share_check(const pl_build *b)
{
    const pl_contributions *c = b->contributions;
    for (size_t i = 0; c && i < c->captured; i++) {
        size_t rep = sh_package_source_link_test_share_rep(c->source, i);
        if (rep == SIZE_MAX || rep == i || rep >= c->captured || sh_package_source_link_test_share_rep(c->source, rep) != rep)
            continue;
        g_pl_share_checked++;
        size_t x = b->linked.classes.nodes[i], y = b->linked.classes.nodes[rep];
        if (x == y && !strcmp(b->linked.names[x], b->linked.names[y]) &&
            pl_row_unedited(c->inputs.units + i, sh_package_source_link_unit(c->source, i)) &&
            pl_row_unedited(c->inputs.units + rep, sh_package_source_link_unit(c->source, rep))) continue;
        g_pl_share_violations++;
        fprintf(stderr, "would-be shared %s %s (row %zu, scope %zu) differs from its representative (row %zu, scope %zu)\n",
            c->inputs.units[i].family, c->inputs.units[i].name, i, c->inputs.scopes[i], rep, c->inputs.scopes[rep]);
        char *strict = NULL; size_t length = 0;
        if (!_dupenv_s(&strict, &length, "SMP_SHARE_STRICT") && strict) abort(); /* a suite-wide strict run (SMP_SHARE_MODE=2) */
    }
}
#endif
/* Identical duplicates (sh_package_compilation.duplicate_count) count each class member's file beyond the first. A
 * package scope binding a shared stock unit (R-344 stage A) is no member of its own, as its copy was: each such
 * occurrence still counts one duplicate per file, less the shared unit's own place when no baseline unit reached it. */
static int pl_share_duplicates(pl_build *b)
{
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(b->reader);
    size_t *sharers = NULL, extra = 0, any = 0;
    for (size_t scope = 0; scope < b->count; scope++) any += sh_package_source_link_scope_shared(b->reader, scope);
    if (!any || !b->linked.compiled || PL_MUTATED(2)) return 1;
    if (!(sharers = calloc(inputs->count ? inputs->count : 1, sizeof(*sharers)))) return 0;
    for (size_t scope = 0; scope < b->count; scope++)
        for (size_t i = 0, n = sh_package_source_link_scope_shared(b->reader, scope); i < n; i++) {
            size_t unit = sh_package_source_link_scope_shared_unit(b->reader, scope, i);
            if (unit < inputs->count) sharers[unit]++;
        }
    for (size_t unit = 0; unit < inputs->count; unit++) if (sharers[unit]) {
        size_t occurrences = sharers[unit] - (sh_package_source_link_stock_only(b->reader, unit) ? 1 : 0);
        extra += occurrences * sh_package_source_link_unit(b->reader, unit)->file_count;
    }
    free(sharers);
    b->linked.compiled->duplicate_count += extra;
    return 1;
}
static int pl_module_variants(pl_build *b, const sh_package_sources *sources, char **coverage)
{
    const sh_package_link_output *linked = &b->linked;
    const sh_package_link_inputs *inputs = b->contributions ? &b->contributions->inputs : NULL;
    if (!inputs || !linked->names) return 1;
    for (size_t c = 0; c < linked->classes.count; c++) {
        const sh_package_link_unit *unit = inputs->units + linked->classes.representatives[c];
        char owners[512] = "", changed[1024] = "", line[2400];
        size_t members = 0, differ = 0, used = 0;
        if (strcmp(unit->family, "snapmodule") || !strcmp(linked->names[c], unit->name)) continue;
        for (size_t i = 0; i < inputs->count; i++) {
            if (linked->classes.nodes[i] != c) continue;
            size_t owner = pl_scope_owner(b, sources, inputs->scopes[i]);
            const char *name = owner < sources->package_count ? sources->packages[owner].name : "a map scope";
            if (strstr(owners, name)) continue;
            int n = snprintf(owners + used, sizeof(owners) - used, "%s%s", used ? ", " : "", name);
            if (n > 0 && (size_t)n < sizeof(owners) - used) used += (size_t)n;
        }
        used = 0;
        if (!unit->original_binding) {
            int n = snprintf(changed, sizeof(changed), "its declaration");
            if (n > 0) used = (size_t)n;
            differ++;
        }
        for (size_t d = 0; d < linked->derived_count; d++) {
            const sh_package_link_derived *derived = linked->derived + d;
            if (derived->owner != c) continue;
            members++;
            /* A member changes the module when its binding is not certified
             * original (its own bytes, or anything it binds, differ from the
             * installed ones). A stable card (unchanged, or only its
             * presentation) is no change of the module, though a presentation
             * slot elsewhere makes it private. */
            const sh_package_link_unit *target = inputs->units + linked->classes.representatives[derived->target];
            if (!linked->originals || linked->originals[derived->target] || target->stable_name) continue;
            const char *path = strchr(derived->slot + 7, '/');
            differ++;
            int n = snprintf(changed + used, sizeof(changed) - used, "%s%s", used ? ", " : "", path ? path + 1 : derived->slot);
            if (n > 0 && (size_t)n < sizeof(changed) - used) used += (size_t)n;
        }
        snprintf(line, sizeof(line), "snapmodule %s: module variant %s from %s; %zu members under its path, %zu changed%s%s\n",
            unit->name, linked->names[c], *owners ? owners : "its scope", members, differ, differ ? ": " : "", changed);
        if (!pl_presentation_note(coverage, line)) return 0;
    }
    return 1;
}
static void pl_private_stock(pl_build *b)
{
    const sh_package_link_inputs *inputs = b->contributions ? &b->contributions->inputs : NULL;
    const unsigned char *eligible = b->reader ? sh_package_source_link_shared(b->reader) : NULL;
    size_t used = 0, shown = 0;
    b->private_stock = 0; b->private_stock_notes[0] = 0;
    for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; inputs && i < inputs->count; i++) {
        const sh_package_link_unit *unit = inputs->units + i;
        const char *name = b->linked.names ? b->linked.names[b->linked.classes.nodes[i]] : NULL;
        int supplied = 0;
        if (!unit->file_count || !name || !strcmp(name, unit->name)) continue;
        for (size_t f = 0; f < unit->file_count; f++) if (unit->files[f]->source_count) supplied = 1;
        if (supplied || (!unit->original_binding) != !pass) continue;
        b->private_stock++;
        if (shown >= 6 || used >= sizeof(b->private_stock_notes) - 1) continue;
        const char *note = !unit->original_binding && b->reader && b->contributions->origins ?
            sh_package_source_link_original_note(b->reader, b->contributions->origins[i]) : "";
        const char *reason = !unit->original_binding ? (*note ? note : "its own binding is not certified original") :
            i < b->stock_reason_count && b->stock_reasons[i] ? b->stock_reasons[i] :
            eligible && !eligible[i] ? "its scope binds it with gameplay it reaches" :
            "a dependency keeps no original name";
        int n = snprintf(b->private_stock_notes + used, sizeof(b->private_stock_notes) - used,
            "\nprivate stock copy: %s %s in scope %zu: %s", unit->family, unit->name,
            inputs->scopes[i], reason);
        if (n > 0) used += (size_t)n < sizeof(b->private_stock_notes) - used ? (size_t)n :
            sizeof(b->private_stock_notes) - used - 1;
        shown++;
    }
}
/* Diagnostic lines of repeated work (sh_package_library.repeated_work). */
static void pl_repeated_work(pl_build *b, sh_package_library *out)
{
    static const char *const phases[SH_PACKAGE_LIBRARY_PHASES] = {
        "scopes", "native", "source link", "contributions other", "identities", "link", "outputs", "audio"
    };
    static const char *const steps[9] = {
        "coverage", "localize", "tokens", "keys", "editor", "registration", "hands", "shared", "document"
    };
    char *text = out->repeated_work; size_t capacity = sizeof(out->repeated_work), used = 0;
    int n;
    *text = 0;
#define PL_WROTE(n) (used += (n) < 0 ? 0 : (size_t)(n) < capacity - used ? (size_t)(n) : capacity - used - 1)
    if (b->census_owned) {
        sh_decl_tree_census total;
        sh_decl_tree_census_read(&total);
        n = snprintf(text, capacity, "parses %llu (%llu MB):", total.parses, total.bytes >> 20);
        PL_WROTE(n);
        for (int i = 0; i < SH_PACKAGE_LIBRARY_PHASES + 9; i++) {
            const sh_decl_tree_census *c = i < SH_PACKAGE_LIBRARY_PHASES ? b->census_phase + i :
                b->census_step + (i - SH_PACKAGE_LIBRARY_PHASES);
            if (!c->parses) continue;
            n = snprintf(text + used, capacity - used, " %s %llu,",
                i < SH_PACKAGE_LIBRARY_PHASES ? phases[i] : steps[i - SH_PACKAGE_LIBRARY_PHASES], c->parses);
            PL_WROTE(n);
        }
    }
    if (b->reader) {
        char line[4096];
        n = snprintf(text + used, capacity - used, "%s%s", used ? "\n" : "",
            sh_package_source_link_counters(b->reader, line, sizeof(line)));
        PL_WROTE(n);
    }
    if (b->private_stock) {
        n = snprintf(text + used, capacity - used, "%sprivate stock copies: %zu%s", used ? "\n" : "",
            b->private_stock, b->private_stock_notes);
        PL_WROTE(n);
    }
    if (b->document.units) {
        n = snprintf(text + used, capacity - used, "%sdocument step: %zu units (%zu shared declarations compared with their original, "
            "%llu MB read in %llu ms), %zu files from the frozen sites, %zu bound again in %llu ms, %zu bodies read in %llu ms",
            used ? "\n" : "", b->document.units, b->document.shared, b->document.original_bytes >> 20, b->document.original_ms,
            b->document.sites, b->document.binds, b->document.bind_ms, b->document.bodies, b->document.read_ms);
        PL_WROTE(n);
    }
    if (b->key_scans || b->key_scan_hits || b->key_skipped) {
        n = snprintf(text + used, capacity - used, "%skey sites: %zu scans, %zu reused, %zu units kept unbound",
            used ? "\n" : "", b->key_scans, b->key_scan_hits, b->key_skipped);
        PL_WROTE(n);
    }
    if (b->reuse_in || b->reuse_out) {
        n = snprintf(text + used, capacity - used, "%seditor groups settled:", used ? "\n" : "");
        PL_WROTE(n);
        for (int p = 1; p <= 3; p++) {
            n = snprintf(text + used, capacity - used, " %d: %zu keyed, %zu settled from the record (%zu members, %zu "
                "staged again), %zu recorded, %zu not keyed;", p, b->settled_keyed[p], b->settled_hits[p],
                b->settled_hit_members[p], b->settled_restaged[p], b->settled_recorded[p], b->settled_unkeyed[p]);
            PL_WROTE(n);
        }
    }
    if (b->key_row_parses || b->key_relocation_parses) {
        n = snprintf(text + used, capacity - used, "%skey phase: %llu parses reading registration rows, %llu relocating "
            "%zu scopes in %llu ms", used ? "\n" : "", b->key_row_parses, b->key_relocation_parses, b->key_relocated_scopes,
            b->key_relocation_ms);
        PL_WROTE(n);
    }
    if (b->editor_census[1].groups || b->editor_census[2].groups || b->editor_census[3].groups) {
        n = snprintf(text + used, capacity - used, "%seditor groups by phase:", used ? "\n" : "");
        PL_WROTE(n);
        for (int p = 1; p <= 3; p++) {
            const struct pl_editor_census *e = b->editor_census + p;
            n = snprintf(text + used, capacity - used, " %d: %zu judged (%zu composing), %zu members (%zu), %zu staged, "
                "%llu parses (%llu composing; %llu binding and first parent reads, %llu forms, %llu rebases, %llu "
                "compositions);", p, e->groups, e->contributed, e->members, e->contributed_members, e->staged, e->parses,
                e->contributed_parses, e->first_parses, e->form_parses, e->rebase_parses, e->compose_parses);
            PL_WROTE(n);
        }
    }
    if (b->available_asked) {
        n = snprintf(text + used, capacity - used, "%simage availability: %zu asked, %zu answered again from this "
            "compile's earlier answer for the image (%zu images); in scopes without the gate, %zu asked about images "
            "their own state names, %zu exclusion placeholders answered, %zu unknown kept stock", used ? "\n" : "",
            b->available_asked, b->available_reused, b->available_count, b->authored_asked, b->authored_excluded,
            b->authored_unknown);
        PL_WROTE(n);
    }
    if (b->reuse_in || b->reuse_out) {
        n = snprintf(text + used, capacity - used, "%sidentity templates: offered record's %s (%zu bytes read in %llu ms), "
            "%zu bytes recorded in %llu ms", used ? "\n" : "", b->templates_read > 0 ? "read" : b->templates_read < 0 ?
            "unreadable" : "none", b->templates_read_bytes, b->templates_read_ms, b->templates_recorded_bytes,
            b->templates_encode_ms);
        PL_WROTE(n);
    }
    if (b->contributions && b->contributions->reuse) {
        char line[8192];
        n = snprintf(text + used, capacity - used, "%s%s", used ? "\n" : "",
            sh_package_link_reuse_counters(b->contributions->reuse, line, sizeof(line)));
        PL_WROTE(n);
        if (*sh_package_link_reuse_link_steps(b->contributions->reuse, line, sizeof(line))) {
            n = snprintf(text + used, capacity - used, "\n%s", line);
            PL_WROTE(n);
        }
        {
            char detail[4096];
            if (*sh_package_link_reuse_detail(b->contributions->reuse, detail, sizeof(detail))) {
                n = snprintf(text + used, capacity - used, "\n%s", detail);
                PL_WROTE(n);
            }
        }
        if (b->contributions->tok_calls) {
            n = snprintf(text + used, capacity - used,
                "\ntokens detail: %llu passes, names %llu ms, token text %llu ms, order %llu ms",
                b->contributions->tok_calls, b->contributions->tok_names_ms, b->contributions->tok_format_ms,
                b->contributions->tok_order_ms);
            PL_WROTE(n);
        }
    }
#undef PL_WROTE
}
/* ---- Toybox label aliases (display-only source qualifiers) ----------------
 * README: "When different variants have identical labels, add a display-only
 * source qualifier." Whether two tiles look alike depends on the language the
 * player selected and on every other tile, so the decision belongs to string
 * publication (strids.h, sh_strids_palette). Here, after identities are
 * assigned, each placeable editor row or logic selector (an Add Logic input or
 * output, which states no meta category) a package supplies under a private name
 * gets its own label alias: a private string whose mapping keeps the row's
 * effective displayNameTag as its base (native key, or the authored locales of
 * a localized key) and adds the row's published name, effective category and
 * meta-category keys and its sources (package name and id, component, authored
 * row). Only the served text's displayNameTag changes, to that alias: row and
 * entityDef identities, bindings, categories, images and saved references are
 * assigned before this and stay as they are. Exact copies share one published
 * row and so one alias naming all their sources; stock and product rows keep
 * their own labels. A package-authored row of a composed (historical) context, or
 * of a scope with no single owning package, is aliased too: it combines several
 * packages' trees, so its sources are the package files its own compiled source
 * list names, each by that file's package, component and the row's authored name.
 * A context row whose source list names a file no package supplied adds no
 * source, and a group left with none stays unaliased. A downstream-only variant
 * (a shipped row its package's scope republishes under a private name because
 * an image, material or model it reaches changed there, R043) has no authored
 * row of its own: it is aliased from that scope's package, by the scope's
 * component and the shipped row's name, so it is told apart from the shipped
 * tile it copies. That package is named only when its own files are listed by a
 * row the variant reaches and the shipped row it copies does not (pl_label_closure):
 * a shipped row every scope republishes alike, for a difference no package
 * supplies, names none; nor does a scope with no single owning package. Without
 * a native string reservation nothing is aliased. */
#define PL_LABEL_DEPTH 32u
typedef struct pl_label_source { const char *name, *id, *component, *row; } pl_label_source;
/* context: the row has no single owning package; its sources come from its own source list.
 * derived: a downstream-only variant of a shipped row; owner is its scope's package. */
typedef struct pl_label_row { const char *published; size_t unit, owner; int context, derived; } pl_label_row;
static int pl_label_row_order(const void *a, const void *b)
{
    const pl_label_row *x = a, *y = b;
    int order = strcmp(x->published, y->published);
    return order ? order : x->unit < y->unit ? -1 : x->unit > y->unit;
}
static int pl_label_source_order(const void *a, const void *b)
{
    const pl_label_source *x = a, *y = b;
    int order = strcmp(x->id, y->id);
    if (!order) order = strcmp(x->component, y->component);
    if (!order) order = strcmp(x->row, y->row);
    return order ? order : strcmp(x->name, y->name);
}
/* A JSON string literal for raw UTF-8 bytes; malloc-owned. */
static char *pl_json_quote(const char *raw)
{
    size_t length = strlen(raw), used = 0;
    char *out = length <= (SIZE_MAX - 3) / 6 ? malloc(length * 6 + 3) : NULL;
    if (!out) return NULL;
    out[used++] = '"';
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        if (*p == '"' || *p == '\\') { out[used++] = '\\'; out[used++] = (char)*p; }
        else if (*p < 0x20) used += (size_t)snprintf(out + used, 7, "\\u%04x", *p);
        else out[used++] = (char)*p;
    }
    out[used++] = '"'; out[used] = 0;
    return out;
}
/* The published row's effective toybox fields through its parents, nearest
 * first: compiled rows of this library (their authored bodies), then SnapMap's
 * shipped declarations. Each is projected as the served roster projects rows
 * (sh_editor_rows_add_text): an ordered parse, since shipped rows may repeat a
 * field, and the last-written member effective. A parent that does not read
 * ends the chain. Owned copies (NULL when unset); 1 read, 0 on failure. */
static int pl_label_fields(pl_build *b, sh_package_baseline_reader original, void *original_context,
    const sh_compiled_resource *row, char **fields)
{
    sh_editor_rows chain = {0};
    char error[256];
    int ok = 0;
    for (int f = 0; f < 4; f++) fields[f] = NULL;
    size_t length = 0;
    unsigned char *text = sh_package_compilation_read(b->linked.compiled, row, SIZE_MAX / 2, &length, error, sizeof(error));
    int added = text ? sh_editor_rows_add_text(&chain, "", (const char *)text, length) : 0;
    free(text);
    if (added < 0) goto done;
    while (chain.count && chain.count < PL_LABEL_DEPTH) {
        const char *parent = chain.items[chain.count - 1].parent;
        if (!parent) break;
        size_t size = strlen(parent) + sizeof(PL_EDITOR_PREFIX) + 5;
        char *path = malloc(size);
        if (!path) goto done;
        snprintf(path, size, PL_EDITOR_PREFIX "%s.decl", parent);
        const sh_compiled_resource *compiled = sh_package_compilation_find(b->linked.compiled, path);
        unsigned char *body = NULL; size_t body_length = 0;
        int status = compiled ? ((body = sh_package_compilation_read(b->linked.compiled, compiled, SIZE_MAX / 2,
            &body_length, error, sizeof(error))) != NULL) : original ? original(original_context, path, &body, &body_length) : 0;
        free(path);
        if (status < 0) { free(body); goto done; }
        size_t before = chain.count;
        added = status > 0 ? sh_editor_rows_add_text(&chain, parent, (const char *)body, body_length) : 0;
        free(body);
        if (added < 0) goto done;
        if (chain.count == before) break;
    }
    for (size_t i = 0; i < chain.count; i++) {
        const sh_editor_row *item = chain.items + i;
        const char *values[4] = {item->label, item->category, item->meta, item->entity_def};
        for (int f = 0; f < 4; f++)
            if (!fields[f] && values[f] && !(fields[f] = _strdup(values[f]))) goto done;
    }
    ok = 1;
done:
    if (!ok) for (int f = 0; f < 4; f++) { free(fields[f]); fields[f] = NULL; }
    sh_editor_rows_free(&chain);
    return ok;
}
/* Serve the alias as the row's displayNameTag (its presented text); the body,
 * which every semantic reader uses, and every other member stay as they are. */
static int pl_label_decorate(sh_compiled_resource *row, const char *key)
{
    /* Ordered: a served row may state a list twice (a native restatement),
     * and the native reader lets the last edit block and the last label win. */
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){(const char *)row->body, row->body_length}, NULL, 0);
    size_t length = 0, quoted_length = strlen(key) + 3;
    char *quoted = malloc(quoted_length), *text = NULL;
    int ok = 0;
    if (!tree || !quoted) goto done;
    snprintf(quoted, quoted_length, "\"%s\"", key);
    sh_decl_node *edit = NULL, *member = NULL;
    for (sh_decl_node *field = tree->children; field; field = field->next) if (!strcmp(field->key, "edit")) edit = field;
    if (!edit || !edit->compound) goto done;
    for (sh_decl_node *field = edit->children; field; field = field->next)
        if (!strcmp(field->key, "displayNameTag")) member = field;
    if (member) {
        if (member->compound) goto done;
        free(member->value); member->value = quoted; quoted = NULL;
    } else {
        if (!(member = calloc(1, sizeof(*member))) || !(member->key = _strdup("displayNameTag"))) {
            if (member) free(member);
            goto done;
        }
        member->value = quoted; quoted = NULL; member->assignment = 1;
        sh_decl_node **tail = &edit->children;
        while (*tail) tail = &(*tail)->next;
        *tail = member;
    }
    if (!(text = sh_decl_tree_write(tree, &length))) goto done;
    free(row->presented); row->presented = (unsigned char *)text; row->presented_length = length; ok = 1;
done:
    free(quoted); sh_decl_tree_free(tree); return ok;
}
/* The root component of an outer package (the one whose descriptor names it), or NULL. */
static const sh_package_component *pl_label_root(const sh_package_sources *sources, size_t owner)
{
    for (size_t c = 0; c < sources->component_count; c++)
        if (sources->components[c].owner == owner && !*sources->components[c].relative) return sources->components + c;
    return NULL;
}
/* An upper bound on one group's source entries: one per library row, and the length of the
 * source list per context row. */
static size_t pl_label_capacity(const sh_package_source_link *source, const pl_label_row *rows, size_t start, size_t end)
{
    size_t total = 1;
    for (size_t r = start; r < end; r++) {
        const sh_package_link_unit *unit = sh_package_source_link_unit(source, rows[r].unit);
        total += rows[r].context && unit && unit->file_count ? unit->files[0]->source_count : 1;
    }
    return total;
}
/* The rows a published row reaches through bound dependencies, itself included,
 * except those exclude marks (when given): a malloc-owned mark per library row,
 * or NULL on allocation failure. With sources and reached, the packages whose
 * files those rows list are added to reached (a failed add returns NULL too).
 * An entityDef's dependency on an editor row is the lifetime edge to a template
 * describing it (pl_editor_companions), not a source dependency, and is not
 * followed: through it the shipped row would reach every variant placing the
 * same entityDef, an icon-only one among them, and leave that variant nothing
 * of its own. */
static int pl_label_companion(const sh_compiled_resource *from, const sh_compiled_resource *to)
{
    return from->type && to->type && !strcmp(from->type, "entitydef") && !strcmp(to->type, "snapeditorentitydef");
}
/* Every resource's bound dependencies the closure follows, resolved once for all the closures of one compile (R-348):
 * the targets of resource r are targets[first[r] .. first[r + 1]), in dependency order, unresolved ones and
 * companion edges left out. */
typedef struct pl_label_graph { size_t *first, *targets; } pl_label_graph;
static void pl_label_graph_free(pl_label_graph *g) { free(g->first); free(g->targets); memset(g, 0, sizeof(*g)); }
static int pl_label_graph_open(pl_label_graph *g, const sh_package_compilation *compiled)
{
    size_t total = compiled->resource_count, edges = 0;
    memset(g, 0, sizeof(*g));
    for (size_t r = 0; r < total; r++) edges += compiled->resources[r].bound_dependency_count;
    if (total >= SIZE_MAX / sizeof(*g->first) - 1 || edges >= SIZE_MAX / sizeof(*g->targets) ||
        !(g->first = malloc((total + 1) * sizeof(*g->first))) || !(g->targets = malloc((edges ? edges : 1) * sizeof(*g->targets)))) {
        pl_label_graph_free(g); return 0;
    }
    size_t used = 0;
    for (size_t r = 0; r < total; r++) {
        const sh_compiled_resource *from = compiled->resources + r;
        g->first[r] = used;
        for (size_t d = 0; d < from->bound_dependency_count; d++) {
            const sh_compiled_resource *next = sh_package_compilation_find(compiled, from->bound_dependencies[d]);
            size_t at = next ? (size_t)(next - compiled->resources) : SIZE_MAX;
            if (at < total && !pl_label_companion(from, next)) g->targets[used++] = at;
        }
    }
    g->first[total] = used;
    return 1;
}
static unsigned char *pl_label_closure(const sh_package_compilation *compiled, const pl_label_graph *graph,
    const sh_compiled_resource *row, const unsigned char *exclude, const sh_package_sources *sources,
    sh_package_owners *reached)
{
    size_t total = compiled->resource_count, head = 0, tail = 0, start = (size_t)(row - compiled->resources);
    size_t *queue = malloc((total ? total : 1) * sizeof(*queue));
    unsigned char *seen = calloc(total ? total : 1, 1);
    int ok = queue && seen;
    if (ok && start < total && !(exclude && exclude[start])) { queue[tail++] = start; seen[start] = 1; }
    while (ok && head < tail) {
        size_t from = queue[head++];
        const sh_compiled_resource *r = compiled->resources + from;
        for (size_t s = 0; ok && reached && s < r->source_count; s++)
            if (r->sources[s] < sources->file_count) ok = sh_package_owners_add(reached, sources->files[r->sources[s]].owner);
        for (size_t e = graph->first[from]; ok && e < graph->first[from + 1]; e++) {
            size_t at = graph->targets[e];
            if (!seen[at] && !(exclude && exclude[at])) { seen[at] = 1; queue[tail++] = at; }
        }
    }
    free(queue);
    if (!ok) { free(seen); return NULL; }
    return seen;
}
/* The sources of a context row: one entry per file its compiled source list names, by that
 * file's package (its root component's descriptor), component and the row's authored name;
 * those packages join owners. 1 added, 0 when a named file or package is missing (nothing is
 * added), -1 on allocation failure. */
static int pl_label_context_sources(const sh_package_sources *sources, const sh_compiled_resource *file,
    const char *row, pl_label_source *found, size_t *used, sh_package_owners *owners)
{
    if (!file || !file->source_count) return 0;
    for (size_t k = 0; k < file->source_count; k++) {
        if (file->sources[k] >= sources->file_count) return 0;
        const sh_package_source_file *supplier = sources->files + file->sources[k];
        if (supplier->component >= sources->component_count || !pl_label_root(sources, supplier->owner)) return 0;
    }
    for (size_t k = 0; k < file->source_count; k++) {
        const sh_package_source_file *supplier = sources->files + file->sources[k];
        const sh_package_component *root = pl_label_root(sources, supplier->owner);
        found[(*used)++] = (pl_label_source){root->descriptor.name, root->descriptor.id,
            sources->components[supplier->component].relative, row};
        if (!sh_package_owners_add(owners, supplier->owner)) return -1;
    }
    return 1;
}
/* A composed context keeps its packages' native keys (pl_localize binds modern scopes only),
 * and its compile merges their historical strings into policy.strings: one key's values join
 * across locales, and conflicting values fail the compile. A label key that policy states takes
 * its base from it, as the row showed before it had an alias; a key no locale states keeps the
 * native fallback. 1 resolved or absent, 0 when the policy cannot be read (nothing is kept). */
static int pl_label_policy_base(const sh_package_policy *policy, const char *label, char **native, char **locales)
{
    sh_json_object stated = {0};
    int ok = 1;
    if (_strnicmp(label, "#str_", 5) || !label[5]) return 1;
    for (size_t i = 0; ok && i < policy->strings.count; i++) {
        const sh_json_member *locale = policy->strings.members + i;
        sh_json_object pairs = {0};
        ok = sh_json_parse_object(locale->value_json, strlen(locale->value_json), 2, &pairs);
        const char *value = ok ? sh_json_object_get(&pairs, label + 5) : NULL;
        if (value) ok = sh_json_object_set(&stated, locale->key, value, 1);
        sh_json_object_free(&pairs);
    }
    if (ok && stated.count) ok = (*native = pl_json_quote(label)) && (*locales = sh_json_serialize_object(&stated, 0, NULL));
    if (!ok) { free(*native); free(*locales); *native = *locales = NULL; }
    sh_json_object_free(&stated);
    return ok;
}
static int pl_label_aliases(pl_build *b, const sh_package_sources *sources,
    sh_package_baseline_reader original, void *original_context, char *error, size_t capacity)
{
    const sh_package_string_reservations *native = b->native ? &b->native->strings : NULL;
    const sh_package_link_inputs *inputs = sh_package_source_link_inputs(b->contributions->source);
    pl_label_row *rows = NULL;
    pl_label_graph graph = {0};
    size_t count = 0;
    int ok = 0;
    if (!native || !native->reserve || !native->max_attempts || !b->linked.compiled) return 1;
    if (!(rows = calloc(b->contributions->inputs.count ? b->contributions->inputs.count : 1, sizeof(*rows)))) goto failed;
    for (size_t i = 0; i < b->contributions->inputs.count; i++) {
        const sh_package_link_unit *unit = b->contributions->inputs.units + i;
        size_t scope = b->contributions->inputs.scopes[i];
        if (strcmp(unit->family, "snapeditorentitydef") || unit->file_count != 1 || i >= inputs->count) continue;
        const char *published = b->linked.names[b->linked.classes.nodes[i]];
        if (!published || !strcmp(published, unit->name)) continue;
        /* A package-authored row: of a library or isolated scope it names its owning package; of a composed
         * or ownerless scope it names none. Stock, product and installed-original bytes are not authored:
         * published privately they are a downstream-only variant, named by its scope's package, which only
         * a library or isolated scope has (pl_scope_owner); without one it stays unaliased. */
        const sh_package_link_unit *captured = sh_package_source_link_unit(b->contributions->source, i);
        if (captured->file_count != 1) continue;
        int authored = captured->files[0]->source_count && !captured->files[0]->native_original;
        size_t owner = !authored ? pl_scope_owner(b, sources, scope) :
            pl_modern(b, scope) ? pl_member_owner(b, sources, i) : SIZE_MAX;
        if (!authored && owner == SIZE_MAX) continue;
        rows[count++] = (pl_label_row){published, i, owner, owner == SIZE_MAX, !authored};
    }
    qsort(rows, count, sizeof(*rows), pl_label_row_order);
    for (size_t start = 0, end; start < count; start = end) {
        for (end = start; end < count && !strcmp(rows[end].published, rows[start].published); end++) {}
        size_t size = strlen(rows[start].published) + sizeof(PL_EDITOR_PREFIX) + 5;
        char *path = malloc(size), *label = NULL, *category = NULL, *meta = NULL, *entity = NULL;
        pl_label_source *found = calloc(pl_label_capacity(b->contributions->source, rows, start, end), sizeof(*found));
        sh_json_object canonical = {0}, label_object = {0}, list = {0};
        sh_package_owners owners = {0}, reached = {0};
        char *base_native = NULL, *base_locales = NULL, *serialized = NULL, *label_json = NULL, *list_json = NULL;
        char *quoted_row = NULL, *quoted_category = NULL, *quoted_meta = NULL, key[128];
        int step = 0;
        if (!path || !found) goto group_done;
        snprintf(path, size, PL_EDITOR_PREFIX "%s.decl", rows[start].published);
        sh_compiled_resource *row = (sh_compiled_resource *)sh_package_compilation_find(b->linked.compiled, path);
        if (!row || !row->body) { step = 1; goto group_done; }
        {
            char *fields[4];
            if (!pl_label_fields(b, original, original_context, row, fields)) goto group_done;
            label = fields[0]; category = fields[1]; meta = fields[2]; entity = fields[3];
        }
        /* A selector row (an Add Logic input or output) states a category but no meta
         * category anywhere in its chain: its meta is the empty text, as the served
         * roster lists it. Not a tile or selector, or a label, category, entity or meta
         * its author left empty (stated "", not inherited): nothing to alias. */
        {
            int meta_stated = meta != NULL;
            if (!meta && label && category && entity && !(meta = _strdup(""))) goto group_done;
            if (!label || !category || !meta || !entity || !*label || !*category || !*entity ||
                (meta_stated && !*meta)) {
                step = 1; goto group_done;
            }
        }
        /* The base: an authored localized key keeps its mapping's native key and
         * locales; any other key (or literal) is its own native fallback. */
        for (size_t i = 0; i < b->linked.compiled->policy.localized_count && !base_native; i++) {
            const sh_package_localized_string *value = b->linked.compiled->policy.localized + i;
            sh_json_object mapping = {0};
            if (_stricmp(value->key, label)) continue;
            if (!sh_json_parse_object(value->mapping, strlen(value->mapping), 4, &mapping)) goto group_done;
            const char *n = sh_json_object_get(&mapping, "native"), *l = sh_json_object_get(&mapping, "locales");
            if (n && l) { base_native = _strdup(n); base_locales = _strdup(l); }
            sh_json_object_free(&mapping);
            if (!base_native || !base_locales) goto group_done;
        }
        if (!base_native && !pl_label_policy_base(&b->linked.compiled->policy, label, &base_native, &base_locales)) {
            snprintf(error, capacity, "the string policy of a toybox label key cannot be read: %s", label);
            goto group_done;
        }
        if (!base_native && (!(base_native = pl_json_quote(label)) || !(base_locales = _strdup("{}")))) goto group_done;
        /* Sources: every package unit this published row stands for. */
        size_t sources_found = 0;
        const char *reached_name = NULL;
        for (size_t r = start; r < end; r++) {
            const sh_package_link_unit *authored = sh_package_source_link_unit(b->contributions->source, rows[r].unit);
            const sh_compiled_resource *file = authored && authored->file_count ? authored->files[0] : NULL;
            if (rows[r].derived) {
                /* A downstream-only variant names its scope's package only for its own files' sake: those the
                 * variant reaches and the shipped row it copies (if the library publishes it) does not. */
                const char *name = b->contributions->inputs.units[rows[r].unit].name;
                if (!reached_name || strcmp(reached_name, name)) {
                    size_t shipped_size = strlen(name) + sizeof(PL_EDITOR_PREFIX) + 5;
                    char *shipped_path = malloc(shipped_size);
                    if (!shipped_path) goto group_done;
                    snprintf(shipped_path, shipped_size, PL_EDITOR_PREFIX "%s.decl", name);
                    const sh_compiled_resource *shipped = sh_package_compilation_find(b->linked.compiled, shipped_path);
                    free(shipped_path);
                    sh_package_owners_free(&reached);
                    if (!graph.first && !pl_label_graph_open(&graph, b->linked.compiled)) goto group_done;
                    unsigned char *copied = shipped && shipped != row ?
                        pl_label_closure(b->linked.compiled, &graph, shipped, NULL, NULL, NULL) : NULL;
                    unsigned char *own = (!shipped || shipped == row || copied) ?
                        pl_label_closure(b->linked.compiled, &graph, row, copied, sources, &reached) : NULL;
                    free(copied); free(own);
                    if (!own) goto group_done;
                    reached_name = name;
                }
                if (!sh_package_owners_contains(&reached, rows[r].owner)) continue;
            }
            if (rows[r].context) {
                /* A row of a composed or ownerless scope: its own source list, or no entry for it. */
                int supplied = pl_label_context_sources(sources, file, b->contributions->inputs.units[rows[r].unit].name,
                    found, &sources_found, &owners);
                if (supplied < 0) goto group_done;
                continue;
            }
            size_t component = rows[r].derived ? b->views[b->contributions->inputs.scopes[rows[r].unit]].component :
                file && file->source_count && file->sources[0] < sources->file_count ?
                sources->files[file->sources[0]].component : SIZE_MAX;
            const sh_package_component *root_component = NULL;
            for (size_t c = 0; c < sources->component_count; c++)
                if (sources->components[c].owner == rows[r].owner && !*sources->components[c].relative) {
                    root_component = sources->components + c; break;
                }
            if (!root_component || component >= sources->component_count) { step = 1; goto group_done; }
            found[sources_found++] = (pl_label_source){root_component->descriptor.name, root_component->descriptor.id,
                sources->components[component].relative, b->contributions->inputs.units[rows[r].unit].name};
            if (!sh_package_owners_add(&owners, rows[r].owner)) goto group_done;
        }
        if (!sources_found) { step = 1; goto group_done; } /* no supplier is known: nothing to name */
        qsort(found, sources_found, sizeof(*found), pl_label_source_order);
        for (size_t s = 0, n = 0; s < sources_found; s++) {
            if (s && !pl_label_source_order(found + s, found + s - 1)) continue;
            char index[24], *name = pl_json_quote(found[s].name), *id = pl_json_quote(found[s].id);
            char *component = pl_json_quote(found[s].component), *authored = pl_json_quote(found[s].row), *entry_json = NULL;
            sh_json_object entry = {0};
            int added = name && id && component && authored &&
                sh_json_object_set(&entry, "name", name, 1) && sh_json_object_set(&entry, "id", id, 1) &&
                sh_json_object_set(&entry, "component", component, 1) && sh_json_object_set(&entry, "row", authored, 1) &&
                (entry_json = sh_json_serialize_object(&entry, 0, NULL)) != NULL;
            snprintf(index, sizeof(index), "%zu", n++);
            added = added && sh_json_object_set(&list, index, entry_json, 8);
            free(name); free(id); free(component); free(authored); free(entry_json); sh_json_object_free(&entry);
            if (!added) goto group_done;
        }
        if (!(quoted_row = pl_json_quote(rows[start].published)) || !(quoted_category = pl_json_quote(category)) ||
            !(quoted_meta = pl_json_quote(meta)) || !(list_json = sh_json_serialize_object(&list, 0, NULL)) ||
            !sh_json_object_set(&label_object, "row", quoted_row, 1) ||
            !sh_json_object_set(&label_object, "category", quoted_category, 1) ||
            !sh_json_object_set(&label_object, "meta", quoted_meta, 1) ||
            !sh_json_object_set(&label_object, "sources", list_json, 8) ||
            !(label_json = sh_json_serialize_object(&label_object, 0, NULL)) ||
            !sh_json_object_set(&canonical, "native", base_native, 1) ||
            !sh_json_object_set(&canonical, "locales", base_locales, 8) ||
            !sh_json_object_set(&canonical, "label", label_json, 8) ||
            !(serialized = sh_json_serialize_object(&canonical, 0, NULL))) goto group_done;
        {
            static const char hex[] = "0123456789abcdef";
            sh_package_file_identity identity; char token[65];
            if (!sh_package_bytes_identity(serialized, strlen(serialized), &identity)) goto group_done;
            for (size_t j = 0; j < 32; j++) {
                token[j * 2u] = hex[identity.digest[j] >> 4]; token[j * 2u + 1u] = hex[identity.digest[j] & 15];
            }
            token[64] = 0;
            if (!pl_reserve_string_globals(b)) goto group_done;
            int reserved = 0;
            for (size_t attempt = 0; attempt < native->max_attempts && reserved != 1; attempt++) {
                if (!attempt) snprintf(key, sizeof(key), "#str_smp_loc_%s", token);
                else snprintf(key, sizeof(key), "#str_smp_loc_%s_%zu", token, attempt);
                reserved = native->reserve(native->context, key, identity.digest, serialized);
                if (reserved < 0) goto group_done;
            }
            if (reserved != 1) { snprintf(error, capacity, "native string ID collision attempts exhausted for a toybox label"); goto group_done; }
            if (!sh_package_policy_localization(&b->linked.compiled->policy, key, serialized, &owners, error, capacity) ||
                !pl_label_decorate(row, key)) goto group_done;
        }
        step = 1;
group_done:
        free(path); free(label); free(category); free(meta); free(entity); free(found);
        free(base_native); free(base_locales); free(serialized); free(label_json); free(list_json);
        free(quoted_row); free(quoted_category); free(quoted_meta);
        sh_json_object_free(&canonical); sh_json_object_free(&label_object); sh_json_object_free(&list);
        sh_package_owners_free(&owners); sh_package_owners_free(&reached);
        if (!step) goto failed;
    }
    ok = 1;
failed:
    free(rows); pl_label_graph_free(&graph);
    if (!ok && !*error) snprintf(error, capacity, "cannot publish toybox label aliases");
    return ok;
}

/* Keep the authored files whose contributions the library composed with other
 * packages' (editor fields, registration lists), by source file. A staged
 * contribution's projection lists every input it read, so its sources beyond
 * the unit's own file and package are the peers it composed with. A unit whose
 * own text already equals the composition stays unstaged and is not recorded:
 * its bytes are what is served. Diagnostics only: a record that cannot be kept
 * marks the set lost. */
static void pl_composed_inputs(const pl_build *b, const sh_package_sources *sources, sh_package_library *out)
{
    const pl_contributions *c = b->contributions;
    const sh_package_link_inputs *original = c && c->source ? sh_package_source_link_inputs(c->source) : NULL;
    if (!c || !original || !sources || !c->units) return;
    for (size_t u = 0; u < c->inputs.count && u < original->count; u++) {
        const sh_compiled_resource *staged = c->units[u].owned ? c->units[u].file : NULL;
        const sh_package_link_unit *captured = sh_package_source_link_unit(c->source, u);
        const sh_compiled_resource *own = captured && captured->file_count ? captured->files[0] : NULL;
        size_t file = staged && own && staged->source_count > 1 ? pl_input_file(sources, own) : SIZE_MAX;
        if (file == SIZE_MAX) continue; /* nothing composed, or nothing authored to account */
        size_t *peers = malloc(staged->source_count * sizeof(*peers)), peer_count = 0;
        if (!peers) { out->composed_inputs_lost = 1; continue; }
        /* Peers with this file's exact bytes are copies, not other contributions:
         * a group of exact copies stays a shared copy, not a composition. */
        const sh_package_source_file *mine_file = sources->files + file;
        for (size_t k = 0; k < staged->source_count; k++) {
            size_t s = staged->sources[k];
            int mine = s >= sources->file_count || sources->files[s].owner == mine_file->owner ||
                (sources->files[s].length == mine_file->length && !memcmp(sources->files[s].digest, mine_file->digest, 32));
            for (size_t j = 0; j < own->source_count && !mine; j++) mine = own->sources[j] == s;
            if (!mine) peers[peer_count++] = s;
        }
        if (!peer_count) { free(peers); continue; }
        if (!out->composed_inputs && !(out->composed_inputs = calloc(original->count, sizeof(*out->composed_inputs)))) {
            free(peers); out->composed_inputs_lost = 1; return;
        }
        out->composed_inputs[out->composed_input_count++] = (sh_package_composed_input){file, peers, peer_count};
    }
}
/* One compile. unshared (count bytes, or NULL) and share_off say which scopes may not share stock; *needs receives
 * the scopes this compile found needing their own copies (pl_share_boundary), when it ends for that reason before
 * publishing anything (pl_compile then compiles again). */
static int pl_compile_once(const sh_package_sources *sources, const sh_package_provenance *contexts,
    const sh_package_compile_environment *environment, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_library_native *native, sh_package_library *out,
    const unsigned char *unshared, size_t unshared_count, int share_off, unsigned char **needs, size_t *needs_count,
    size_t *reuse_bound, int *reuse_attempted, char *error, size_t capacity)
{
    pl_build b = {0};
    b.unshared = unshared; b.share_off = share_off;
    sh_package_source_link_native source_native = {&b, pl_admit, pl_semantics,
        native ? native->source.shader : NULL, pl_material_companions, pl_material_seeds, pl_implicit, pl_source_owners};
    if (native && native->private_keys) source_native.prepare = pl_prepare_discovered;
    source_native.memo = native ? native->source.memo : NULL;
    /* Retained reverse graph (R-386): the caller's assertion that the stock originals are production's. */
    source_native.reverse_retain = native ? native->source.reverse_retain : 0;
    /* Unchanged-scope reuse (R-348): the record is kept only from a compile that completes. */
    sh_package_source_reuse *reuse_pending = NULL;
    source_native.reuse_in = native ? native->source.reuse_in : NULL;
    source_native.reuse_context = native ? native->source.reuse_context : NULL;
    source_native.reuse_bound = reuse_bound; source_native.reuse_attempted = reuse_attempted;
    /* Named, never positional: fields appended to sh_package_source_link_native (R-309). */
    if (native && native->image_excluded) {
        source_native.availability = pl_availability;
        source_native.availability_authored = pl_availability_authored;
    }
    sh_package_compile_environment env;
    sh_package_hands_adapter hands = {0};
    char **hand_names = NULL, fallback[1024]; size_t hand_name_count = 0; int ok = 0;
    unsigned long long mark = pl_now_ms();
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    if (!out) return 0;
    sh_package_library_free(out);
    if (environment && environment->invalid_package) *environment->invalid_package = SIZE_MAX;
    if (environment && environment->invalid_own_bytes) *environment->invalid_own_bytes = 0;
    if (environment && environment->unserved_package) *environment->unserved_package = SIZE_MAX;
    if (environment && environment->more_rejections) environment->more_rejections->count = 0;
    if (environment && environment->conflicted_packages) sh_package_owners_free(environment->conflicted_packages);
    if (!sources || !environment || (contexts && !contexts->context_count)) goto done;
    if (!contexts && !sources->component_count) {
        out->compiled = sh_package_compile_with(sources, environment, error, capacity);
        if (out->compiled && native && native->source.reuse_out) {
            /* No package scope: an empty record. */
            sh_package_source_reuse *none = sh_package_source_reuse_open();
            if (none) sh_package_source_reuse_move(native->source.reuse_out, none);
            sh_package_source_reuse_close(none);
        }
        return out->compiled != NULL;
    }
    b.census_owned = sh_decl_tree_census_open();
    sh_decl_tree_census_read(&b.census_mark);
    b.count = contexts ? contexts->context_count : sources->component_count; b.authored_count = b.count; b.native = native;
    b.contexts = contexts;
    env = *environment; env.previous = NULL; env.independent_sources = 1; env.player_hands = NULL;
    b.originals[0].read = environment->baseline; b.originals[0].context = environment->baseline_context;
    b.originals[1].read = original; b.originals[1].context = original_context;
    if (native && environment->baseline) b.originals[0].backed = native->baseline_backed;
    if (native && original) b.originals[1].backed = native->original_backed;
    b.originals[0].peer = b.originals + 1; b.originals[1].peer = b.originals;
    if (environment->baseline) { env.baseline = pl_read_original; env.baseline_context = b.originals; }
    size_t original_slot = original == environment->baseline && original_context == environment->baseline_context ? 0 : 1;
    original = pl_read_original; original_context = b.originals + original_slot;
    if (b.count > SIZE_MAX / sizeof(*b.views) - 2 || b.count > SIZE_MAX / sizeof(*b.scopes) - 2) goto done;
    b.views = calloc(b.count + 2, sizeof(*b.views)); b.scopes = calloc(b.count + 2, sizeof(*b.scopes));
    b.withdrawn = calloc(b.count + 2, 1); b.unshare = calloc(b.count + 2, 1);
    if (!b.views || !b.scopes || !b.withdrawn || !b.unshare) goto done;
    b.collect_contributions = pl_attributable(contexts) && environment->invalid_package &&
        environment->unserved_package && environment->more_rejections;
    /* A composed context's packages share one scope, so their copies of the
     * player hands web align there package by package (pl_hands_group aligns
     * copies of separate scopes). */
    int composed = 0, animweb = 0;
    for (size_t i = 0; contexts && i < b.count; i++) composed |= contexts->contexts[i].mode == SH_PACKAGE_CONTEXT_COMPOSED;
    for (size_t i = 0; composed && !animweb && i < sources->file_count; i++)
        animweb = sources->files[i].engine_path && !_strnicmp(sources->files[i].engine_path, "generated/decls/animweb/", 24);
    if (animweb) {
        if (!sh_package_player_hands_webs(original, original_context, &hand_names, &hand_name_count)) {
            snprintf(error, capacity, "the stock player's hands web contract is unreadable"); goto done;
        }
        hands = (sh_package_hands_adapter){(const char *const *)hand_names, hand_name_count,
            sh_decl_animweb_prepare, sh_decl_animweb_moves_free, sh_decl_weapon_subweb_rename};
    }
    /* Packages whose own bytes fail in their isolated scope: every one of this
     * pass is collected (the first reported, the others as more_rejections)
     * before the compile fails, rather than one compile at a time. */
    size_t own_first = SIZE_MAX;
    char own_reason[1024] = "";
    int collect_own = pl_attributable(contexts) && environment->invalid_package && environment->more_rejections;
    for (size_t i = 0; i < b.count; i++) {
        if (!(contexts ? sh_package_provenance_scope(contexts, i, sources, b.views + i, error, capacity) :
            sh_package_scope_open(sources, i, b.views + i, error, capacity))) goto done;
        sh_package_compile_environment scoped = env;
        if (contexts && contexts->contexts[i].mode == SH_PACKAGE_CONTEXT_COMPOSED) {
            scoped.independent_sources = 0;
            if (hand_name_count) scoped.player_hands = &hands;
        }
        sh_package_builtin *builtins = NULL;
        if (scoped.independent_sources && scoped.builtin_count) {
            builtins = calloc(scoped.builtin_count, sizeof(*builtins));
            if (!builtins) goto done;
            scoped.builtin_count = 0;
            for (size_t j = 0; j < env.builtin_count; j++)
                if (!pl_builtin_has_authored_editor(&b.views[i].sources, env.builtins[j].engine_path))
                    builtins[scoped.builtin_count++] = env.builtins[j];
            scoped.builtins = builtins;
        }
        size_t scope_invalid = SIZE_MAX; int scope_own = 0;
        if (collect_own) { scoped.invalid_package = &scope_invalid; scoped.invalid_own_bytes = &scope_own; }
        b.scopes[i] = sh_package_compile_with(&b.views[i].sources, &scoped, error, capacity);
        free(builtins);
        /* What serves a scope's bytes unchanged shares them (a refused owner leaves them the scope's own), and its
         * compile was the last lookup through its view's own file table. */
        if (PL_SHARE) sh_package_compilation_share(b.scopes[i]);
        if (PL_SHARE && b.scopes[i]) sh_package_scope_share_files(b.views + i, sources);
        if (!b.scopes[i] && collect_own && scope_invalid != SIZE_MAX && scope_own &&
            scope_invalid < sources->package_count) {
            if (own_first == SIZE_MAX) {
                own_first = scope_invalid; snprintf(own_reason, sizeof(own_reason), "%s", error);
            } else if (scope_invalid != own_first) {
                sh_package_rejections *more = environment->more_rejections;
                int seen = 0;
                for (size_t j = 0; j < more->count; j++) seen |= more->items[j].package == scope_invalid;
                if (!seen) {
                    if (more->count == more->capacity) {
                        size_t grown_capacity = more->capacity ? more->capacity * 2 : 4;
                        sh_package_rejection *grown = realloc(more->items, grown_capacity * sizeof(*grown));
                        if (!grown) { snprintf(error, capacity, "cannot account further invalid packages"); goto done; }
                        more->items = grown; more->capacity = grown_capacity;
                    }
                    sh_package_rejection *r = more->items + more->count++;
                    r->package = scope_invalid; r->invalid = r->own_bytes = 1;
                    snprintf(r->reason, sizeof(r->reason), "%s", error);
                }
            }
            error[0] = 0;
            continue;
        }
        if (!b.scopes[i]) {
            /* Not a decision on its own bytes: stop now, as before. Own-byte
             * packages already found this pass are still reported together;
             * this failure is judged again on the next compile. */
            if (own_first == SIZE_MAX) {
                if (collect_own) {
                    *environment->invalid_package = scope_invalid;
                    if (environment->invalid_own_bytes) *environment->invalid_own_bytes = scope_own;
                }
            } else {
                *environment->invalid_package = own_first;
                if (environment->invalid_own_bytes) *environment->invalid_own_bytes = 1;
                snprintf(error, capacity, "%s", own_reason);
            }
            goto done;
        }
    }
    if (own_first != SIZE_MAX) {
        *environment->invalid_package = own_first;
        if (environment->invalid_own_bytes) *environment->invalid_own_bytes = 1;
        snprintf(error, capacity, "%s", own_reason);
        goto done;
    }
    if (!pl_shared_sources(&b, sources, error, capacity)) goto done;
    b.shared = sh_package_compile_with(&b.shared_view.sources, &env, error, capacity);
    if (!b.shared) goto done;
    if (PL_SHARE) { sh_package_compilation_share(b.shared); sh_package_scope_share_files(&b.shared_view, sources); }
    if (contexts) {
        sh_package_owners all = {0}; sh_package_policy policy = {0};
        for (size_t i = 0; i < sources->package_count; i++)
            if (!sh_package_owners_add(&all, i)) { sh_package_owners_free(&all); goto done; }
        for (size_t i = 0; i < b.authored_count; i++) if (!pl_modern(&b, i))
            for (size_t j = 0; j < b.views[i].sources.component_count; j++) {
                const sh_package_component *component = b.views[i].sources.components + j;
                if (component->descriptor.id[0] &&
                    !sh_package_owners_add(&b.shared->policy.legacy_string_owners, component->owner)) {
                    sh_package_owners_free(&all); goto done;
                }
            }
        int prepared = sh_package_compilation_policy(b.shared, &all, &policy, error, capacity);
        sh_package_owners_free(&all);
        if (!prepared) goto done;
        sh_package_policy_free(&b.shared->policy); b.shared->policy = policy;
    }
    pl_phase(&b, out, SH_PACKAGE_LIBRARY_PHASE_SCOPES, &mark);
    if (!pl_product_scope(&b, sources, error, capacity)) goto done;
    if (!pl_registration_inheritance(&b, sources, 0, catalog, original, original_context,
        registry, reflection, error, capacity)) goto done;
    if (!pl_baseline_scope(&b, sources, env.baseline ? env.baseline : original,
        env.baseline ? env.baseline_context : original_context, error, capacity)) goto done;
    if (!pl_registration_inheritance(&b, sources, b.count - 1, catalog,
        env.baseline ? env.baseline : original, env.baseline ? env.baseline_context : original_context,
        registry, reflection, error, capacity)) goto done;
    if (!pl_registration_sources(&b, sources, env.baseline ? env.baseline : original,
        env.baseline ? env.baseline_context : original_context, registry, reflection, error, capacity)) goto done;
    if (!pl_native_open(&b, catalog, original, original_context, registry, reflection, error, capacity)) goto done;
    pl_phase(&b, out, SH_PACKAGE_LIBRARY_PHASE_NATIVE, &mark);
    source_native.audio = b.audio;
    unsigned char *discover = calloc(b.count ? b.count : 1, 1);
    if (!discover) goto done;
    if (!contexts || !contexts->binding_count) for (size_t i = 0; i < b.authored_count; i++)
        discover[i] = !contexts || contexts->contexts[i].mode == SH_PACKAGE_CONTEXT_ISOLATED;
    else if (native && native->document_roots) for (size_t i = 0; i < b.authored_count; i++)
        discover[i] = contexts->contexts[i].mode == SH_PACKAGE_CONTEXT_COMPOSED;
    sh_package_source_root_plan root_plan = {discover, b.count - 1, contexts,
        contexts && native ? native->document_roots : NULL};
    /* Shared stock (R-344 stage A): authored modern scopes of their own (an isolated context, or a library scope) bind
     * their proven stock copies once in the baseline, except those an earlier round found needing their own. */
    unsigned char *fold = b.share_off ? NULL : calloc(b.count ? b.count : 1, 1);
    if (!b.share_off && !fold) { free(discover); goto done; }
    for (size_t i = 0; fold && i < b.authored_count; i++)
        fold[i] = pl_modern(&b, i) && (!contexts || contexts->contexts[i].mode == SH_PACKAGE_CONTEXT_ISOLATED) &&
            !(unshared && i < unshared_count && unshared[i]);
    root_plan.fold = fold;
    sh_package_source_failure failure;
    sh_package_source_withdrawals withdrawals = {0};
    source_native.withdrawals = &withdrawals;
    source_native.produce = env.produce; source_native.producer_context = env.producer_context;
    source_native.produce_derived = env.produce_derived;
    /* The link borrows the snapshot's bytes rather than copying each one, and what serves them shares them;
     * pl_close releases the snapshot after the link (R-321, R-341 memory). */
    source_native.original_view = pl_view_original; source_native.original_view_context = original_context;
    /* At the capture's release points the snapshot stops holding backed bytes too (R-342). */
    source_native.originals_released = pl_originals_detach; source_native.originals_released_context = b.originals;
    /* And for each binary original the capture lets go after its first recorded discovery (R-344 stage A). */
    source_native.original_released = pl_original_detach_path; source_native.original_released_context = b.originals;
    if (native && native->source.reuse_out && !(reuse_pending = sh_package_source_reuse_open())) {
        free(discover); free(fold); snprintf(error, capacity, "cannot record scope reuse"); goto done;
    }
    source_native.reuse_out = reuse_pending;
    b.reuse_in = source_native.reuse_in; b.reuse_out = reuse_pending; b.reuse_attempted = reuse_attempted;
    source_native.produce_module = env.produce_module;
    b.reader = sh_package_source_link_open_attributed((const sh_package_compilation *const *)b.scopes,
        b.count, catalog, original, original_context, registry, reflection, &source_native,
        &root_plan, &failure, error, capacity);
    source_native.withdrawals = NULL;
    free(discover); free(fold);
    if (!b.reader && withdrawals.stopped) {
        /* Discovery ran scope by scope (shared stock) and a failure other than an environment one stopped it: what it
         * found before the stop depends on that order, so this compile is made again without sharing, which reports
         * exactly what the capture's creation order finds. Nothing is blamed here. */
        for (size_t i = 0; i < b.count; i++) pl_unshare_scope(&b, i);
        b.share_off = 1;
        sh_package_source_withdrawals_free(&withdrawals);
        goto done;
    }
    pl_phase(&b, out, SH_PACKAGE_LIBRARY_PHASE_SOURCE_LINK, &mark);
    if (b.reader) sh_package_source_link_phases(b.reader, out->source_phases, sizeof(out->source_phases));
    if (b.reader) sh_package_source_link_memory(b.reader, out->source_memory, sizeof(out->source_memory));
    if (!b.reader) {
        /* Only content a local component supplied makes it responsible: its own
         * unit refused by a reader is malformed source; content reaching a unit
         * the product cannot link leaves its complete delivery unserved for this
         * candidate. Healthy peers then compile without it. Product/stock-only
         * chains, environment, map contexts and product/baseline scopes keep
         * refusing the whole candidate and blame no package. */
        size_t used = strlen(error);
        if (failure.chain[0] && used < capacity)
            snprintf(error + used, capacity - used, " (via %s%s)", failure.chain,
                failure.kind == SH_PACKAGE_SOURCE_FAILURE_PRODUCT ? "; reached only from product and stock content" : "");
        /* An original that could not be read again is environmental, whatever the capture was reading. */
        size_t owner = pl_attributable(contexts) && !pl_originals_failed(b.originals, error, capacity) ?
            pl_scope_owner(&b, sources, failure.scope) : SIZE_MAX;
        int reported = 0;
        if (owner != SIZE_MAX) {
            if (failure.kind == SH_PACKAGE_SOURCE_FAILURE_SOURCE && environment->invalid_package) {
                /* Its own supplied file, which the reader could not parse in
                 * the native grammar (sl_attribute): its bytes alone. */
                *environment->invalid_package = owner;
                if (environment->invalid_own_bytes) *environment->invalid_own_bytes = 1;
                reported = 1;
            }
            else if (failure.kind == SH_PACKAGE_SOURCE_FAILURE_COVERAGE && environment->unserved_package) {
                *environment->unserved_package = owner;
                reported = 1;
            }
        }
        /* A capture stopped at its admission boundary says so in its phases. */
        if (withdrawals.admission_units)
            snprintf(out->source_phases, sizeof(out->source_phases),
                "admission stopped after %zu supplied units with %zu withdrawn scope(s); stock closure not discovered",
                withdrawals.admission_units, withdrawals.count);
        /* The same capture withdrew further scopes that fail on their own
         * content and kept discovering the rest: report their owners too, so
         * they are left out together rather than found one compile at a time.
         * Only when this failure is itself such a withdrawal: a capture ending
         * in an environmental or product failure blames nobody. */
        if (reported && !pl_more_rejections(&b, sources, owner, &withdrawals, environment->more_rejections)) {
            snprintf(error, capacity, "cannot account further withdrawn packages");
            if (environment->invalid_package) *environment->invalid_package = SIZE_MAX;
            if (environment->unserved_package) *environment->unserved_package = SIZE_MAX;
            if (environment->invalid_own_bytes) *environment->invalid_own_bytes = 0;
        }
        sh_package_source_withdrawals_free(&withdrawals);
        goto done;
    }
    sh_package_source_withdrawals_free(&withdrawals);
    /* Shared stock the contribution phase would select for one scope: that scope's own copies (pl_compile). */
    if (!pl_share_boundary(&b, error, capacity)) goto done;
    unsigned long long step_mark = pl_now_ms();
    if (!pl_coverage(&b, sources, &out->coverage)) {
        snprintf(error, capacity, "cannot account package coverage"); goto done;
    }
    pl_shader_inputs(&b, sources, out);
    pl_step(&b, PL_STEP_COVERAGE, &step_mark);
    int opened = pl_contributions_open(&b, sources, error, capacity);
    pl_steps(&b, out->contribution_phases, sizeof(out->contribution_phases));
    if (!opened) goto done;
    if (b.key_notes) {
        size_t used = out->coverage ? strlen(out->coverage) : 0, add = strlen(b.key_notes);
        char *grown = realloc(out->coverage, used + add + 1);
        if (!grown) { snprintf(error, capacity, "cannot account package coverage"); goto done; }
        memcpy(grown + used, b.key_notes, add + 1); out->coverage = grown;
    }
    step_mark = pl_now_ms();
    if (contexts && !pl_document_shared(&b, original, original_context, error, capacity)) goto done;
    pl_step(&b, PL_STEP_DOCUMENT, &step_mark);
    pl_steps(&b, out->contribution_phases, sizeof(out->contribution_phases));
    pl_phase(&b, out, SH_PACKAGE_LIBRARY_PHASE_CONTRIBUTIONS, &mark);
    /* Authored documents carry their own selector/inspector behavior. Changed
     * editor units receive ordinary bound aliases just like their gameplay
     * dependencies, so importing them never rewrites the open editor's shared
     * objects. Certified vanilla bindings still retain native names. Local
     * library compilation continues composing its one shared editor surface. */
    if (!pl_presentation_slots(&b, sources, &out->coverage)) {
        snprintf(error, capacity, "cannot resolve module presentation slots"); goto done;
    }
    pl_assignment assignment = {&b.contributions->inputs, b.contributions->shared,
        b.materials != NULL, b.contributions->global};
    b.contributions->assignment = &assignment;
    for (size_t i = 0; i < assignment.inputs->count; i++) {
        sh_package_link_unit *unit = b.contributions->inputs.units + i;
        int fixed = (assignment.shared[i] && pl_shared_declaration(unit->family)) || assignment.global[i];
        int stable = unit->stable_name || (assignment.shared[i] && pl_shared_declaration(unit->family));
        if (fixed == unit->fixed_name && stable == unit->stable_name) continue;
        if (!pl_unit_keep_named(b.contributions, i)) { snprintf(error, capacity, "%s", "cannot keep a captured unit's record"); goto done; }
        unit->fixed_name = fixed;
        /* A shared declaration bound under its one native name is a contract
         * every consumer reaches by that name; the shared publication selects
         * its composed payload. Like the player hands web, neither that payload
         * nor its per-scope content enters a consumer's identity, so a full
         * scan, an installation increment and a map's own context bind one
         * consumer alike. Its own bytes and edges are identified as usual, and
         * a private copy of the same bytes remains an ordinary edge. */
        unit->stable_name = stable;
    }
    sh_package_link_registry identity_adapter = {0};
    if (native && native->identities.head_slot) {
        b.identities = sh_package_native_registry_open(native->identities, catalog, error, capacity);
        if (!b.identities) goto done;
        sh_package_native_registry_observe(b.identities, native->identity_journal);
        for (size_t i = 0; i < assignment.inputs->count; i++) {
            const sh_package_link_unit *unit = assignment.inputs->units + i;
            if (((assignment.shared[i] && pl_shared_declaration(unit->family)) || assignment.global[i]) &&
                !sh_package_native_registry_share(b.identities, unit->family, unit->name)) goto done;
        }
        identity_adapter = sh_package_native_registry_adapter(b.identities);
    }
    pl_phase(&b, out, SH_PACKAGE_LIBRARY_PHASE_IDENTITIES, &mark);
    sh_package_link_adapter adapter = {b.contributions, pl_bound_resolve, pl_bound_bind, pl_bound_assign,
        b.identities ? &identity_adapter : NULL, pl_bound_assign_members, b.contributions->reuse};
    adapter.history = b.identities && native ? native->history : NULL;
    adapter.stable = pl_bound_stable; adapter.sites = pl_bound_sites; adapter.identity = pl_bound_identity;
    adapter.describe = pl_bound_describe; adapter.note = pl_bound_note;
    /* The compile's last identity pass: its retained order and shapes are not read again. */
    adapter.release_order = PL_RELEASE_ORDER;
    adapter.derive = pl_bound_derive;
    pl_reuse_refresh(b.contributions);
    /* Identity provenance bases (R-397): the compositions each unit took part in. */
    if (assignment.inputs->count && !pl_unit_prov(&b, assignment.inputs->count - 1)) {
        snprintf(error, capacity, "cannot record identity provenance"); goto done;
    }
    /* A scope adds no provenance of its own: its packages reach a unit only through the files its identity
     * hashes (their owners), and a difference every scope makes alike names no package (as its label does). */
    for (size_t i = 0; i < assignment.inputs->count; i++) b.contributions->inputs.units[i].provenance = b.unit_prov + i;
    int link_ok = sh_package_link(sources, assignment.inputs->units, assignment.inputs->count, &adapter, &b.shared->policy,
        environment->canonical_path, &b.linked, error, capacity);
    /* The bases are this build's: no later pass reads them through a kept unit. */
    for (size_t i = 0; i < assignment.inputs->count; i++) b.contributions->inputs.units[i].provenance = NULL;
    if (!link_ok || (b.identities && !sh_package_native_registry_validate(b.identities, error, capacity))) goto done;
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
    pl_share_check(&b);
#endif
    if (!pl_share_duplicates(&b)) { snprintf(error, capacity, "cannot count shared stock duplicates"); goto done; }
    pl_private_stock(&b);
    pl_semantic_coverage(&b);
    if (!pl_module_variants(&b, sources, &out->coverage)) {
        snprintf(error, capacity, "cannot account module variants"); goto done;
    }
    if (!pl_label_aliases(&b, sources, original, original_context, error, capacity)) goto done;
    pl_phase(&b, out, SH_PACKAGE_LIBRARY_PHASE_LINK, &mark);
    if (!pl_provenance(&b, sources, &out->bindings) || !pl_key_provenance(&b, sources, &out->bindings, error, capacity) ||
        !pl_hud(&b, error, capacity) ||
        !sh_package_sound_prepare(b.linked.compiled, error, capacity) ||
        !pl_consumers(b.contributions, &b.linked, &out->consumers, error, capacity) ||
        !pl_fsm_webs(&b, original, original_context, &out->fsm_webs, error, capacity)) goto done;
    if (b.materials) {
        out->virtual_materials = sh_package_virtual_material_prepare(b.materials, assignment.inputs,
            &b.linked, error, capacity);
        if (!out->virtual_materials) goto done;
    }
    pl_phase(&b, out, SH_PACKAGE_LIBRARY_PHASE_OUTPUTS, &mark);
    if (b.audio) {
        if (!sh_package_audio_program_prepare(b.audio, error, capacity)) goto done;
        for (size_t i = 0; i < assignment.inputs->count; i++) {
            const sh_package_link_unit *unit = assignment.inputs->units + i;
            if (unit->file_count && !strcmp(unit->family, "sound") &&
                sh_package_audio_program_contains(b.audio, assignment.inputs->scopes[i], unit->name) &&
                !sh_package_audio_program_output(b.audio, assignment.inputs->scopes[i], unit->name,
                    b.linked.names[b.linked.classes.nodes[i]], error, capacity)) goto done;
        }
        pl_shadow_notes notes = {&b, sources, NULL};
        if (!sh_package_audio_program_shadowed(b.audio, pl_shadow_note, &notes)) {
            free(notes.text); snprintf(error, capacity, "cannot account ineffective audio edits"); goto done;
        }
        if (notes.text) {
            size_t used = out->coverage ? strlen(out->coverage) : 0, add = strlen(notes.text);
            char *grown = add < SIZE_MAX - used ? realloc(out->coverage, used + add + 1) : NULL;
            if (!grown) { free(notes.text); snprintf(error, capacity, "cannot account ineffective audio edits"); goto done; }
            memcpy(grown + used, notes.text, add + 1); out->coverage = grown; free(notes.text);
        }
        out->audio = b.audio; b.audio = NULL;
    }
    pl_phase(&b, out, SH_PACKAGE_LIBRARY_PHASE_AUDIO, &mark);
    out->key_groups = b.key_groups; b.key_groups = NULL;
    {   /* In-place recovery set and the declared image gate, for the publication (R-309). */
        const char *const *recovered = NULL;
        size_t count = sh_package_source_link_recovered(b.reader, &recovered);
        if (count && !(out->recovered_images = calloc(count, sizeof(*out->recovered_images)))) {
            snprintf(error, capacity, "cannot retain recovered image names"); goto done;
        }
        for (size_t i = 0; i < count; i++) {
            if (!(out->recovered_images[i] = _strdup(recovered[i]))) {
                snprintf(error, capacity, "cannot retain recovered image names"); goto done;
            }
            out->recovered_image_count = i + 1;
        }
        for (size_t i = 0; i < b.count; i++) {
            int opens = pl_scope_opens_image_gate(&b, i);
            if (opens < 0) { snprintf(error, capacity, "cannot read declared image requirements"); goto done; }
            if (opens) out->image_gate = 1;
        }
    }
    /* Each published resource with undecided native semantics is accounted
     * coverage: served as authored, its state is not claimed decided. */
    for (size_t i = 0; b.linked.compiled && i < b.linked.compiled->resource_count; i++) {
        const sh_compiled_resource *r = b.linked.compiled->resources + i;
        if (!r->semantic_coverage) continue;
        char line[1280];
        int n = snprintf(line, sizeof(line), "%s %s (%s): served as authored; its native state selection is "
            "product coverage\n", r->type ? r->type : "resource", r->name ? r->name : "?",
            r->original_path ? r->original_path : r->engine_path);
        size_t used = out->coverage ? strlen(out->coverage) : 0, add = n > 0 ? strlen(line) : 0;
        char *grown = add && add < SIZE_MAX - used ? realloc(out->coverage, used + add + 1) : NULL;
        if (!grown) { snprintf(error, capacity, "cannot account package coverage"); goto done; }
        memcpy(grown + used, line, add + 1); out->coverage = grown;
    }
    pl_composed_inputs(&b, sources, out);
    /* Every captured original read again reproduced its bytes, or the candidate fails here. */
    if (pl_originals_failed(b.originals, error, capacity)) goto done;
    out->compiled = b.linked.compiled; b.linked.compiled = NULL;
    out->compiled->independent_sources = 1; ok = 1;
    /* The rows' identity provenance, by path: every row the link recorded with any package in it (R-397). */
    if (b.linked.row_provenance) {
        size_t listed = 0;
        for (size_t i = 0; i < b.linked.row_provenance_count; i++) listed += sh_package_owners_count(b.linked.row_provenance + i) != 0;
        out->row_provenance = calloc(listed ? listed : 1, sizeof(*out->row_provenance));
        out->row_provenance_paths = calloc(listed ? listed : 1, sizeof(*out->row_provenance_paths));
        if (!out->row_provenance || !out->row_provenance_paths) {
            free(out->row_provenance); free(out->row_provenance_paths); out->row_provenance = NULL; out->row_provenance_paths = NULL;
        } else {
            for (size_t i = 0; i < b.linked.row_provenance_count; i++) {
                if (!sh_package_owners_count(b.linked.row_provenance + i)) continue;
                char *path = _strdup(b.linked.row_provenance_paths[i]);
                if (!path) break;
                out->row_provenance_paths[out->row_provenance_count] = path;
                out->row_provenance[out->row_provenance_count++] = b.linked.row_provenance[i];
                memset(b.linked.row_provenance + i, 0, sizeof(b.linked.row_provenance[i]));
            }
            if (out->row_provenance_count != listed) {
                for (size_t i = 0; i < out->row_provenance_count; i++) {
                    sh_package_owners_free(out->row_provenance + i); free(out->row_provenance_paths[i]);
                }
                free(out->row_provenance); free(out->row_provenance_paths);
                out->row_provenance = NULL; out->row_provenance_paths = NULL; out->row_provenance_count = 0;
            }
        }
    }
    out->reuse_settled = b.settled_hits[1] + b.settled_hits[2] + b.settled_hits[3];
    sh_package_link_reuse_offer_counts(b.contributions->reuse, NULL, &out->reuse_templates, NULL);
    if (reuse_pending) {
        /* The identity cache's capture templates (R-348), kept with the record; without them it is still a record. */
        unsigned long long began = pl_now_ms();
        unsigned char *templates = NULL; size_t length = 0;
        if (sh_package_link_reuse_templates_encode(b.contributions->reuse, &templates, &length)) {
            sh_package_source_reuse_set_templates(reuse_pending, templates, length);
            b.templates_recorded_bytes = length;
        }
        b.templates_encode_ms = pl_now_ms() - began;
        sh_package_source_reuse_move(native->source.reuse_out, reuse_pending);
    }
done:
    sh_package_source_reuse_close(reuse_pending);
    if (!ok && pl_originals_failed(b.originals, error, capacity)) {
        /* Environmental: no contribution verdict blames a package for it. */
    } else if (!ok && b.unshare_count) {
        /* Shared stock a scope needs its own copies of: nothing is blamed or published; pl_compile compiles again. */
        *needs = b.unshare; *needs_count = b.count; b.unshare = NULL;
        if (b.share_off) *needs_count = SIZE_MAX; /* again without sharing at all */
    } else if (!ok && pl_attributable(contexts) && environment) pl_contribution_report(&b, environment, error, capacity);
    free(b.unshare); b.unshare = NULL;
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    /* Resources the kept output serves with the snapshot's own bytes, and views that read the inventory's table. */
    g_pl_shared = 0; g_pl_views_shared = 0;
    for (size_t i = 0; b.views && i < b.count; i++) g_pl_views_shared += b.views[i].files_shared != 0;
    for (size_t i = 0; ok && i < out->compiled->resource_count; i++) {
        const sh_compiled_resource *r = out->compiled->resources + i;
        const pl_original *file = r->body_owner ? pl_original_at((pl_originals *)original_context,
            r->original_path ? r->original_path : r->engine_path) : NULL;
        g_pl_shared += file && file->owner == r->body_owner;
    }
#endif
    if (out) pl_repeated_work(&b, out);
    if (out && b.contributions && b.contributions->notes) {
        free(out->link_diagnostics); out->link_diagnostics = b.contributions->notes; b.contributions->notes = NULL;
    }
    if (b.census_owned) sh_decl_tree_census_close();
    pl_close(&b);
    if (!ok) {
        /* Accounted coverage and phase timings survive a later failure as
         * diagnostics only. */
        char *coverage = out->coverage, *link_diagnostics = out->link_diagnostics;
        out->coverage = NULL; out->link_diagnostics = NULL;
        unsigned long long phases[SH_PACKAGE_LIBRARY_PHASES]; char source_phases[sizeof(out->source_phases)];
        char contribution_phases[sizeof(out->contribution_phases)], repeated_work[sizeof(out->repeated_work)];
        memcpy(phases, out->phase_ms, sizeof(phases)); memcpy(source_phases, out->source_phases, sizeof(source_phases));
        memcpy(contribution_phases, out->contribution_phases, sizeof(contribution_phases));
        memcpy(repeated_work, out->repeated_work, sizeof(repeated_work));
        sh_package_library_free(out); out->coverage = coverage; out->link_diagnostics = link_diagnostics;
        memcpy(out->phase_ms, phases, sizeof(phases)); memcpy(out->source_phases, source_phases, sizeof(source_phases));
        memcpy(out->contribution_phases, contribution_phases, sizeof(contribution_phases));
        memcpy(out->repeated_work, repeated_work, sizeof(repeated_work));
        if (!*error) snprintf(error, capacity, "scoped package library could not be prepared");
    }
    for (size_t i = 0; i < hand_name_count; i++) free(hand_names[i]);
    free(hand_names);
    return ok;
}

/* A compile, again with more scopes keeping their own copies of stock while one ends needing that (pl_share_boundary,
 * pl_unit_keep), or without sharing at all after an order-dependent stop: each round unshares at least one more scope,
 * so it ends, and with none sharing it is the compile without shared stock. */
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_RESOURCE_GRAPH_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
/* Test builds: with SMP_SHARE_REPORT set, the process prints its shared-stock totals at exit (suite-wide runs). */
static size_t g_pl_compiles, g_pl_rounds, g_pl_unshared_scopes, g_pl_share_off;
static void pl_share_report(void)
{
    size_t totals[7], checked = 0, violations = 0;
    sh_package_source_link_test_share_totals(totals);
    sh_package_library_test_share_checks(&checked, &violations);
    fprintf(stderr, "shared stock report: %zu library compiles, %zu fallback rounds (%zu scopes unshared, %zu without "
        "sharing); %zu captures decided %zu units (%zu joined, %zu became stock), %zu given back; strict checks %zu, "
        "%zu differed; %zu originals streamed, %zu read again\n", g_pl_compiles, g_pl_rounds, g_pl_unshared_scopes,
        g_pl_share_off, totals[0], totals[1], totals[2], totals[3], totals[4], checked, violations, totals[5], totals[6]);
}
static void pl_share_report_once(void)
{
    static volatile long registered;
    char *report = NULL; size_t length = 0;
    if (_InterlockedCompareExchange(&registered, 1, 0)) return;
    if (!_dupenv_s(&report, &length, "SMP_SHARE_REPORT") && report) atexit(pl_share_report);
    free(report);
}
#define PL_SHARE_REPORT(rounds, unshared, off) (pl_share_report_once(), g_pl_compiles++, g_pl_rounds += (rounds), \
    g_pl_unshared_scopes += (unshared), g_pl_share_off += (off) != 0)
/* Test builds: with SMP_SHARE_DIGEST naming a file, every library compile appends one line: its result and a digest
 * of its outputs (resources with their bytes, owners and identities, counts, policy, provenance, consumers, state-
 * machine webs, key groups, coverage and accounting). The same suite run in two modes must append the same lines. */
static void pl_digest_text(uint64_t *h, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "\x01(null)");
    do { *h = (*h ^ *p) * UINT64_C(1099511628211); } while (*p++);
}
static void pl_digest_bytes(uint64_t *h, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    for (size_t i = 0; p && i < length; i++) *h = (*h ^ p[i]) * UINT64_C(1099511628211);
    *h = (*h ^ (uint64_t)length) * UINT64_C(1099511628211);
}
static void pl_digest_owners(uint64_t *h, const sh_package_owners *o)
{
    pl_digest_bytes(h, &o->bits, sizeof(o->bits));
    for (size_t i = 0; o->more && i < o->words; i++) pl_digest_bytes(h, o->more + i, sizeof(o->more[i]));
}
static void pl_digest_json(uint64_t *h, const sh_json_object *object)
{
    size_t length = 0;
    char *text = sh_json_serialize_object(object, 0, &length);
    pl_digest_text(h, text); free(text);
}
static void pl_share_digest(int ok, int injected, const sh_package_library *out, const char *error)
{
    char *path = NULL; size_t length = 0;
    if (_dupenv_s(&path, &length, "SMP_SHARE_DIGEST") || !path) return;
    uint64_t h = UINT64_C(14695981039346656037);
    const sh_package_compilation *c = out ? out->compiled : NULL;
    size_t sizes[4] = {c ? c->resource_count : 0, c ? c->duplicate_count : 0, c ? c->composed_count : 0,
        out ? out->bindings.binding_count : 0};
    pl_digest_bytes(&h, sizes, sizeof(sizes));
    for (size_t i = 0; c && i < c->resource_count; i++) {
        const sh_compiled_resource *r = c->resources + i;
        sh_package_file_identity identity = {0};
        pl_digest_text(&h, r->engine_path); pl_digest_text(&h, r->type); pl_digest_text(&h, r->name);
        pl_digest_text(&h, r->native_type); pl_digest_text(&h, r->native_name); pl_digest_text(&h, r->original_path);
        int flags[11] = {r->baseline_known, r->composed, r->native_original, r->restored_original, r->semantic_coverage,
            r->generated, r->generated_layer, r->rebound_original, (int)r->sound_authored_event, (int)r->sound_event,
            (int)r->source_count};
        pl_digest_bytes(&h, flags, sizeof(flags));
        pl_digest_owners(&h, &r->owners); pl_digest_owners(&h, &r->gameplay_owners);
        /* Its bytes: resident ones directly, a file or cache by its recorded identity; a shared body whose bytes are
         * let go by its length only, since reading it again would change the run (backing reads, injected failures). */
        if (r->body) pl_digest_bytes(&h, r->body, r->body_length);
        else if (!r->body_owner && sh_package_compilation_identity(c, r, &identity, NULL, 0))
            pl_digest_bytes(&h, &identity, sizeof(identity));
        else pl_digest_bytes(&h, &r->body_length, sizeof(r->body_length));
        pl_digest_bytes(&h, r->presented, r->presented_length);
        for (size_t k = 0; k < r->source_count; k++) pl_digest_bytes(&h, r->sources + k, sizeof(r->sources[k]));
        for (size_t k = 0; k < r->generated_input_count; k++) pl_digest_text(&h, r->generated_inputs[k]);
        for (size_t k = 0; k < r->bound_dependency_count; k++) pl_digest_text(&h, r->bound_dependencies[k]);
        for (size_t k = 0; k < r->native_alias_count; k++) {
            pl_digest_text(&h, r->native_aliases[k].type); pl_digest_text(&h, r->native_aliases[k].name);
        }
    }
    if (c) {
        pl_digest_json(&h, &c->policy.requirements); pl_digest_json(&h, &c->policy.strings); pl_digest_json(&h, &c->policy.hud);
        for (size_t i = 0; i < c->policy.localized_count; i++) {
            pl_digest_text(&h, c->policy.localized[i].key); pl_digest_text(&h, c->policy.localized[i].mapping);
            pl_digest_owners(&h, &c->policy.localized[i].owners);
        }
        for (size_t i = 0; i < c->policy.hud_binding_count; i++) {
            pl_digest_text(&h, c->policy.hud_bindings[i].name);
            pl_digest_bytes(&h, &c->policy.hud_bindings[i].weapon_mode, sizeof(c->policy.hud_bindings[i].weapon_mode));
            pl_digest_owners(&h, &c->policy.hud_bindings[i].owners);
        }
    }
    const sh_package_provenance *p = out ? &out->bindings : NULL;
    for (size_t i = 0; p && i < p->context_count; i++) {
        pl_digest_bytes(&h, &p->contexts[i].mode, sizeof(p->contexts[i].mode)); pl_digest_text(&h, p->contexts[i].component);
        for (size_t k = 0; k < p->contexts[i].tree_count; k++) pl_digest_bytes(&h, p->contexts[i].trees[k], 32);
    }
    for (size_t i = 0; p && i < p->binding_count; i++) {
        pl_digest_text(&h, p->bindings[i].family); pl_digest_text(&h, p->bindings[i].original); pl_digest_text(&h, p->bindings[i].name);
        pl_digest_bytes(&h, p->bindings[i].contexts, p->bindings[i].context_count * sizeof(*p->bindings[i].contexts));
    }
    for (size_t i = 0; p && i < p->use_count; i++) {
        pl_digest_text(&h, p->uses[i].owner); pl_digest_text(&h, p->uses[i].reference); pl_digest_text(&h, p->uses[i].row);
        pl_digest_bytes(&h, &p->uses[i].binding, sizeof(p->uses[i].binding));
        pl_digest_bytes(&h, p->uses[i].contexts, p->uses[i].context_count * sizeof(*p->uses[i].contexts));
    }
    size_t consumers = out ? sh_package_consumers_count(out->consumers) : 0;
    for (size_t i = 0; i < consumers; i++) {
        const sh_package_consumer *owner = sh_package_consumers_at(out->consumers, i);
        pl_digest_text(&h, owner->family); pl_digest_text(&h, owner->name);
        for (size_t k = 0; k < owner->count; k++) {
            pl_digest_text(&h, owner->targets[k].slot); pl_digest_text(&h, owner->targets[k].family);
            pl_digest_text(&h, owner->targets[k].name);
        }
    }
    const sh_package_fsm_webs *webs = out ? out->fsm_webs : NULL;
    for (size_t i = 0; webs && i < webs->count; i++) {
        pl_digest_text(&h, webs->webs[i].config);
        for (size_t k = 0; k < webs->webs[i].behavior_count; k++) pl_digest_text(&h, webs->webs[i].behaviors[k]);
        for (size_t k = 0; k < webs->webs[i].count; k++) { pl_digest_text(&h, webs->webs[i].from[k]); pl_digest_text(&h, webs->webs[i].to[k]); }
        for (size_t k = 0; k < webs->webs[i].extra_count; k++) pl_digest_text(&h, webs->webs[i].extra[k]);
    }
    for (size_t i = 0; webs && i < webs->generated_count; i++) {
        pl_digest_text(&h, webs->generated[i].name); pl_digest_bytes(&h, webs->generated[i].text, webs->generated[i].length);
    }
    if (out && out->key_groups) pl_digest_bytes(&h, out->key_groups, sizeof(*out->key_groups));
    if (out) {
        pl_digest_text(&h, out->coverage);
        for (size_t i = 0; i < out->shader_input_count; i++) pl_digest_bytes(&h, out->shader_inputs + i, sizeof(size_t) * 2);
        for (size_t i = 0; i < out->recovered_image_count; i++) pl_digest_text(&h, out->recovered_images[i]);
        int tail[3] = {out->image_gate, out->virtual_materials != NULL, out->audio != NULL};
        pl_digest_bytes(&h, tail, sizeof(tail));
    }
    FILE *file = NULL;
    if (!fopen_s(&file, path, "ab") && file) {
        fprintf(file, "%c %d %016llx %zu %zu %zu %.200s\n", injected ? 'I' : 'N', ok, (unsigned long long)h, sizes[0],
            sizes[1], sizes[3], ok ? "" : (error ? error : ""));
        fclose(file);
    }
    free(path);
}
#define PL_SHARE_DIGEST(ok, out, error) pl_share_digest(ok, injected, out, error)
/* Whether a test armed an injected failure countdown before this compile: such compiles fail or pass at an
 * allocation or read whose position depends on order and on what is read again, so the differential compares them
 * apart (pl_share_digest tags them). */
static int pl_injection_armed(void)
{
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING)
    long intern = 0, prepared = 0;
    sh_package_source_link_test_refusals_left(&intern, &prepared);
    if (intern > 0 || prepared > 0 || sh_package_source_link_test_blocks_left() > 0 ||
        sh_package_source_link_test_extra_refusal_left() > 0 || sh_package_source_link_test_take_refusal_left() > 0 ||
        sh_package_source_link_test_preserve_refusal_left() > 0 || sh_package_source_link_test_share_refusal_left() > 0 ||
        sh_package_library_test_original_refusal_left() > 0 || sh_package_compilation_test_project_refusal_left() > 0) return 1;
#endif
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || defined(SH_PACKAGE_RUNTIME_TESTING)
    if (sh_package_body_test_refusal_left() > 0 || sh_package_body_test_backing_failure_left() > 0) return 1;
#endif
#ifdef SH_PACKAGE_LIBRARY_TESTING
    if (g_token_failures > 0 || g_rejudge_failures > 0 || g_contribution_read_failures > 0) return 1;
#endif
    return 0;
}
#else
#define PL_SHARE_REPORT(rounds, unshared, off) ((void)0)
#define PL_SHARE_DIGEST(ok, out, error) ((void)0)
#define pl_injection_armed() 0
#endif
static int pl_compile(const sh_package_sources *sources, const sh_package_provenance *contexts,
    const sh_package_compile_environment *environment, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection,
    const sh_package_library_native *native, sh_package_library *out,
    char *error, size_t capacity)
{
    unsigned char *unshared = NULL;
    size_t unshared_count = 0, rounds = 0, reuse_bound = 0, bound_total = 0;
    int share_off = 0, ok, injected = pl_injection_armed(), fallback = 0, attempted = 0;
    (void)injected;
    pl_share_environment();
    /* Unchanged-scope reuse (R-348): a compile that binds recorded shared stock and then fails is made again without
     * reuse, from the link history as it was before it. */
    sh_package_library_native fresh;
    const sh_package_library_native *active = native;
    sh_package_link_history_mark mark = sh_package_link_history_mark_now(native ? native->history : NULL);
    for (;;) {
        unsigned char *needs = NULL;
        size_t needs_count = 0;
        reuse_bound = 0;
        ok = pl_compile_once(sources, contexts, environment, catalog, original, original_context, registry, reflection,
            active, out, unshared, unshared_count, share_off, &needs, &needs_count, &reuse_bound, &attempted, error,
            capacity);
        bound_total += reuse_bound;
        /* Whatever failed, the fallback judges it as a compile without reuse would. */
        if (!ok && attempted && active->source.reuse_in) {
            free(needs);
            if (native->history && !sh_package_link_history_rollback(native->history, mark)) {
                snprintf(error, capacity, "cannot restore the native binding history after a failed reuse"); break;
            }
            if (native->reuse_fallback_outside) { fallback = 1; break; }
            fresh = *native; fresh.source.reuse_in = NULL; active = &fresh;
            free(unshared); unshared = NULL; unshared_count = 0; share_off = 0; rounds = 0; bound_total = 0; attempted = 0;
            *error = 0; continue;
        }
        if (!needs) break; /* a compile without sharing never asks again */
        int grown = 0;
        if (needs_count != SIZE_MAX && needs_count > unshared_count) {
            unsigned char *wider = realloc(unshared, needs_count);
            if (wider) {
                memset(wider + unshared_count, 0, needs_count - unshared_count);
                unshared = wider; unshared_count = needs_count;
            }
        }
        for (size_t i = 0; needs_count != SIZE_MAX && i < needs_count && i < unshared_count; i++)
            if (needs[i] && !unshared[i]) { unshared[i] = 1; grown = 1; }
        free(needs);
        /* An order-dependent stop, or nothing new to unshare: the compile without sharing. */
        if (needs_count == SIZE_MAX || !grown) share_off = 1;
        rounds++;
    }
    {
        size_t unshared_scopes = 0;
        for (size_t i = 0; i < unshared_count; i++) unshared_scopes += unshared[i] != 0;
        PL_SHARE_REPORT(rounds, unshared_scopes, share_off);
        (void)unshared_scopes;
    }
    PL_SHARE_DIGEST(ok, out, error);
    if (out && rounds) {
        size_t used = strlen(out->repeated_work);
        if (used + 1 < sizeof(out->repeated_work))
            snprintf(out->repeated_work + used, sizeof(out->repeated_work) - used, "%sshared stock fallback rounds: %zu%s",
                used ? "\n" : "", rounds, share_off ? " (the last without sharing)" : "");
    }
    free(unshared);
    if (out) { out->reuse_bindings = bound_total; out->reuse_fallback = fallback; }
    return ok;
}

int sh_package_library_compile(const sh_package_sources *sources,
    const sh_package_compile_environment *environment, const sh_resource_catalog *catalog,
    sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_library_native *native,
    sh_package_library *out, char *error, size_t capacity)
{
    return pl_compile(sources, NULL, environment, catalog, original, original_context,
        registry, reflection, native, out, error, capacity);
}

sh_package_compilation *sh_package_library_compile_composed(const sh_package_sources *sources,
    const sh_package_compile_environment *environment, char *error, size_t capacity)
{
    sh_package_compile_environment env = *environment;
    sh_package_hands_adapter hands;
    char **hand_names = NULL; size_t hand_name_count = 0;
    int animweb = 0;
    for (size_t i = 0; env.baseline && !animweb && i < sources->file_count; i++)
        animweb = sources->files[i].engine_path && !_strnicmp(sources->files[i].engine_path, "generated/decls/animweb/", 24);
    if (animweb) {
        if (!sh_package_player_hands_webs(env.baseline, env.baseline_context, &hand_names, &hand_name_count)) {
            snprintf(error, capacity, "the stock player's hands web contract is unreadable"); return NULL;
        }
        hands = (sh_package_hands_adapter){(const char *const *)hand_names, hand_name_count,
            sh_decl_animweb_prepare, sh_decl_animweb_moves_free, sh_decl_weapon_subweb_rename};
        if (hand_name_count) env.player_hands = &hands;
    }
    sh_package_compilation *compiled = sh_package_compile_with(sources, &env, error, capacity);
    for (size_t i = 0; i < hand_name_count; i++) free(hand_names[i]);
    free(hand_names);
    return compiled;
}

int sh_package_library_compile_contexts(const sh_package_sources *sources,
    const sh_package_provenance *contexts, const sh_package_compile_environment *environment,
    const sh_resource_catalog *catalog, sh_package_baseline_reader original, void *original_context,
    sh_decl_registry_source registry, uintptr_t reflection, const sh_package_library_native *native,
    sh_package_library *out, char *error, size_t capacity)
{
    if (!contexts) {
        if (out) sh_package_library_free(out);
        if (error && capacity) snprintf(error, capacity, "map package contexts are unavailable");
        return 0;
    }
    return pl_compile(sources, contexts, environment, catalog, original, original_context,
        registry, reflection, native, out, error, capacity);
}
