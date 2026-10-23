#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_native_assets.h"
#include "decl_material.h"
#include "package_shader_receipt.h"
#include "audio_banks.h"
#include "package_virtual_pages.h"
#include "package_movie.h"
#include "native_resource_id.h"
#include "swf_text_font.h"

typedef struct na_file {
    char *path;
    sh_package_file *file;
    int status;
} na_file;
typedef struct na_shader_original {
    struct na_shader_original *next;
    char *path;
    unsigned char *body;
    size_t length;
    uint64_t task_id;
} na_shader_original;
typedef struct na_shader_request {
    struct na_shader_request *next;
    sh_package_shader_request value;
} na_shader_request;
/* Set of "family\tname" keys (open addressing, power-of-two capacity). */
typedef struct na_names { char **slots; size_t count, capacity; } na_names;
static uint64_t na_names_hash(const char *family, const char *name)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const char *p = family; *p; p++) { hash ^= (unsigned char)*p; hash *= UINT64_C(1099511628211); }
    hash ^= '\t'; hash *= UINT64_C(1099511628211);
    for (const char *p = name; *p; p++) { hash ^= (unsigned char)*p; hash *= UINT64_C(1099511628211); }
    return hash;
}
static int na_names_key_equal(const char *key, const char *family, const char *name)
{
    size_t n = strlen(family);
    return !strncmp(key, family, n) && key[n] == '\t' && !strcmp(key + n + 1, name);
}
static int na_names_contains(const na_names *set, const char *family, const char *name)
{
    if (!set->capacity) return 0;
    for (size_t at = (size_t)na_names_hash(family, name) & (set->capacity - 1); set->slots[at];
        at = (at + 1) & (set->capacity - 1))
        if (na_names_key_equal(set->slots[at], family, name)) return 1;
    return 0;
}
/* Best effort: this bookkeeping is a hint and never fails a check. */
static void na_names_add(na_names *set, const char *family, const char *name)
{
    if (na_names_contains(set, family, name)) return;
    if (set->count + 1 > set->capacity / 2) {
        size_t capacity = set->capacity ? set->capacity * 2 : 64;
        char **slots = capacity <= SIZE_MAX / sizeof(*slots) ? calloc(capacity, sizeof(*slots)) : NULL;
        if (!slots) return;
        for (size_t i = 0; i < set->capacity; i++) if (set->slots[i]) {
            const char *key = set->slots[i], *tab = strchr(key, '\t');
            uint64_t hash = UINT64_C(14695981039346656037);
            for (const char *p = key; p < tab; p++) { hash ^= (unsigned char)*p; hash *= UINT64_C(1099511628211); }
            hash ^= '\t'; hash *= UINT64_C(1099511628211);
            for (const char *p = tab + 1; *p; p++) { hash ^= (unsigned char)*p; hash *= UINT64_C(1099511628211); }
            size_t at = (size_t)hash & (capacity - 1);
            while (slots[at]) at = (at + 1) & (capacity - 1);
            slots[at] = set->slots[i];
        }
        free(set->slots); set->slots = slots; set->capacity = capacity;
    }
    size_t f = strlen(family), n = strlen(name);
    char *key = malloc(f + n + 2);
    if (!key) return;
    memcpy(key, family, f); key[f] = '\t'; memcpy(key + f + 1, name, n + 1);
    size_t at = (size_t)na_names_hash(family, name) & (set->capacity - 1);
    while (set->slots[at]) at = (at + 1) & (set->capacity - 1);
    set->slots[at] = key; set->count++;
}
static void na_names_free(na_names *set)
{
    for (size_t i = 0; i < set->capacity; i++) free(set->slots[i]);
    free(set->slots); memset(set, 0, sizeof(*set));
}
typedef struct na_shader_file {
    struct na_shader_file *next;
    char *path;
    unsigned char *body;
    size_t length;
    char *program; /* The requested program whose cooked binary this is, else NULL. */
} na_shader_file;
struct sh_package_shader_preparation {
    sh_package_native_assets *assets;
    na_shader_request *requests;
    size_t count;
    int ran;
    char first_need[384];
};
/* Level-0 geometry of a mega2 header: page texels, grid origin and size, first
 * index cell and the index offset. */
typedef struct na_level0 { uint32_t texels, x, y, width, height, first; uint64_t index; } na_level0;
typedef struct na_cells { na_level0 geometry; uint32_t *cells; unsigned char identity[32]; unsigned slot; int state; } na_cells;
struct sh_package_native_assets {
    char *root;
    volatile LONG references;
    SRWLOCK lock;
    na_file *files;
    size_t count, capacity;
    unsigned char pages[32];
    int page_state;
    /* Captured private page families, keyed by the scope's authored members.
     * Every material of one scope shares its family; installed originals are
     * fixed, so equal authored members describe the same capture. */
    struct { unsigned char key[32]; sh_virtual_page_source *pages; } page_sources[4];
    size_t page_source_next;
    /* Private page families refused because a member omits pages that a bound
     * material's row reads (na_page_row). Their materials keep the engine's
     * provider, so the authored members serve nothing. Keyed by family identity. */
    unsigned char (*page_refused)[32];
    size_t page_refused_count;
    /* Level-0 index cells for na_page_row: installed members by slot (state 1
     * read, 2 none), effective members of private families by identity. */
    na_cells page_installed[16], page_effective[4];
    size_t page_effective_next;
    sh_package_source_shader shader;
    sh_native_resource_registry_source shader_registry;
    sh_package_baseline_reader shader_original;
    void *shader_original_context;
    na_shader_original *shader_originals;
    /* The same originals by path (open addressing; R-348): each shader ABI check finds its source at once. */
    na_shader_original **shader_original_index;
    size_t shader_original_capacity, shader_original_count;
    sh_resource_catalog *shader_catalog;
    int shader_configured;
    int (*consumers_ready)(const char *family);
    na_shader_request *shader_requests;
    size_t shader_request_count;
    /* The queue's first request reason with its native state, kept where it
     * is recorded because later checks reuse the caller's buffer; diagnostics
     * only, moved into the batch that takes the queue. */
    char shader_first_need[384];
    uint64_t next_shader_task;
    na_shader_file *shader_files;
    /* One compilation's shader ABI checks share this identity snapshot. Each
     * check revalidates what it read; leaving the scope revalidates it all. */
    sh_native_resource_registry *shader_snapshot;
    int shader_scope, shader_scope_changed;
    /* The stock shader objects the recorded compilation needed: those it
     * requested preparation for and cooked programs it read (a later
     * session's preparation hint, sh_package_native_assets_shader_needs). */
    na_names shader_used;
    /* Objects this instance's native preparation produced and verified. */
    na_names shader_prepared;
    /* Programs named by programs cooked here, not yet followed. */
    na_names shader_follow;
    int shader_recording, shader_prewarming;
    char error[1024];
};
static int na_error(sh_package_native_assets *a, const char *reason, const char *path)
{
    snprintf(a->error, sizeof(a->error), "%s%s%s", reason, path ? ": " : "", path ? path : "");
    return -1;
}
sh_package_native_assets *sh_package_native_assets_open(const char *root)
{
    sh_package_native_assets *a = calloc(1, sizeof(*a));
    wchar_t *wide = sh_package_source_wide_path(root);
    if (a) { a->references = 1; InitializeSRWLock(&a->lock); }
    if (!a || !wide || !(a->root = _strdup(root))) {
        free(wide); sh_package_native_assets_close(a); return NULL;
    }
    free(wide); return a;
}
sh_package_native_assets *sh_package_native_assets_retain(sh_package_native_assets *a)
{ if (a) InterlockedIncrement(&a->references); return a; }
void sh_package_native_assets_close(sh_package_native_assets *a)
{
    if (!a || InterlockedDecrement(&a->references)) return;
    for (size_t i = 0; i < a->count; i++) { free(a->files[i].path); sh_package_file_release(a->files[i].file); }
    for (size_t i = 0; i < sizeof(a->page_sources) / sizeof(a->page_sources[0]); i++)
        sh_virtual_page_source_release(a->page_sources[i].pages);
    free(a->page_refused);
    for (size_t i = 0; i < 16; i++) free(a->page_installed[i].cells);
    for (size_t i = 0; i < sizeof(a->page_effective) / sizeof(a->page_effective[0]); i++) free(a->page_effective[i].cells);
    while (a->shader_originals) {
        na_shader_original *entry = a->shader_originals; a->shader_originals = entry->next;
        free(entry->path); free(entry->body); free(entry);
    }
    free(a->shader_original_index);
    while (a->shader_requests) {
        na_shader_request *request = a->shader_requests; a->shader_requests = request->next;
        free((void *)request->value.family); free((void *)request->value.name);
        free((void *)request->value.source); free(request);
    }
    while (a->shader_files) { na_shader_file *file = a->shader_files; a->shader_files = file->next;
        free(file->path); free(file->body); free(file->program); free(file); }
    sh_native_resource_registry_close(a->shader_snapshot);
    sh_resource_catalog_close(a->shader_catalog);
    na_names_free(&a->shader_used); na_names_free(&a->shader_follow); na_names_free(&a->shader_prepared);
    free(a->files); free(a->root); free(a);
}
void sh_package_native_assets_shader_scope_begin(sh_package_native_assets *a)
{
    if (!a) return;
    AcquireSRWLockExclusive(&a->lock);
    if (!a->shader_scope++) a->shader_scope_changed = 0;
    ReleaseSRWLockExclusive(&a->lock);
}
int sh_package_native_assets_shader_scope_end(sh_package_native_assets *a, char *error, size_t capacity)
{
    int ok = 1;
    if (error && capacity) *error = 0;
    if (!a) return 1;
    AcquireSRWLockExclusive(&a->lock);
    if (a->shader_scope && !--a->shader_scope) {
        if (a->shader_snapshot && !sh_native_resource_registry_validate(a->shader_snapshot, error, capacity))
            a->shader_scope_changed = 1;
        ok = !a->shader_scope_changed;
        if (!ok && error && capacity && !*error)
            snprintf(error, capacity, "native shader identities changed during package compilation");
        sh_native_resource_registry_close(a->shader_snapshot); a->shader_snapshot = NULL;
    }
    ReleaseSRWLockExclusive(&a->lock); return ok;
}
/* Caller holds the lock. Outside a scope every check owns a fresh snapshot. */
static sh_native_resource_registry *na_shader_registry(sh_package_native_assets *a, char *error, size_t capacity)
{
    if (a->shader_scope && a->shader_snapshot) return a->shader_snapshot;
    sh_native_resource_registry *registry = sh_native_resource_registry_open(a->shader_registry, error, capacity);
    if (registry && a->shader_scope) a->shader_snapshot = registry;
    return registry;
}
static void na_shader_registry_release(sh_package_native_assets *a, sh_native_resource_registry *registry)
{ if (registry != a->shader_snapshot) sh_native_resource_registry_close(registry); }
static int na_shader_registry_valid(sh_package_native_assets *a, sh_native_resource_registry *registry,
    const sh_native_resource_identity *identity, char *error, size_t capacity)
{
    if (registry != a->shader_snapshot) return sh_native_resource_registry_validate(registry, error, capacity);
    if (sh_native_resource_registry_validate_identity(registry, identity, error, capacity)) return 1;
    /* A changed snapshot cannot serve later checks; the scope reports it. The
     * caller's release now owns closing this detached snapshot. */
    a->shader_scope_changed = 1; a->shader_snapshot = NULL;
    return 0;
}
static na_file *na_capture(sh_package_native_assets *a, const char *path)
{
    /* Caller holds the cache lock through use of the returned array entry. */
    char *canonical = sh_package_engine_path(path), *absolute = NULL;
    wchar_t *wide = NULL;
    na_file *entry = NULL;
    if (!canonical || strcmp(canonical, path) || strncmp(path, "virtualtextures/", 16)) goto done;
    for (size_t i = 0; i < a->count; i++) if (!strcmp(a->files[i].path, path)) {
        entry = a->files + i;
        if (entry->status >= 0) goto done;
        break; /* Failed preparations can retry an operational read failure. */
    }
    if (!entry && a->count == a->capacity) {
        size_t next = a->capacity ? a->capacity * 2 : 32;
        na_file *grown = next > a->capacity && next <= SIZE_MAX / sizeof(*grown) ?
            realloc(a->files, next * sizeof(*grown)) : NULL;
        if (!grown) goto done;
        a->files = grown; a->capacity = next;
    }
    size_t n = strlen(a->root), m = strlen(path);
    if (n > SIZE_MAX - m - 2 || !(absolute = malloc(n + m + 2))) goto done;
    snprintf(absolute, n + m + 2, "%s/%s", a->root, path);
    wide = sh_package_source_wide_path(absolute); if (!wide) goto done;
    if (!entry) {
        entry = a->files + a->count++; memset(entry, 0, sizeof(*entry));
        entry->path = canonical; canonical = NULL;
    }
    entry->status = 0;
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            entry->status = na_error(a, "cannot inspect native companion", path);
    } else if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        entry->status = na_error(a, "native companion is not a regular file", path);
    } else {
        entry->file = sh_package_file_pin(absolute, a->error, sizeof(a->error));
        entry->status = entry->file ? 1 : -1;
    }
done:
    free(canonical); free(absolute); free(wide);
    if (!entry) na_error(a, "invalid native companion path or allocation failure", path);
    return entry;
}
int sh_package_native_assets_read(void *context, const char *path, unsigned char **body, size_t *length)
{
    sh_package_native_assets *a = context;
    *body = NULL; *length = 0;
    size_t n = path ? strlen(path) : 0;
    if (!a || n < 21 || strncmp(path, "virtualtextures/", 16) || strcmp(path + n - 5, ".vmtr")) return 0;
    AcquireSRWLockExclusive(&a->lock);
    na_file *entry = na_capture(a, path);
    int result = entry ? entry->status : -1;
    if (result == 1) {
        *body = sh_package_file_read(entry->file, SIZE_MAX, length, a->error, sizeof(a->error));
        result = *body ? 1 : -1;
    }
    ReleaseSRWLockExclusive(&a->lock);
    return result;
}
int sh_package_native_assets_shader_read(void *context, const char *path, unsigned char **body, size_t *length)
{
    sh_package_native_assets *a = context; int result = 0, recording;
    *body = NULL; *length = 0;
    if (!a || !path) return 0;
    AcquireSRWLockShared(&a->lock);
    recording = a->shader_recording;
    ReleaseSRWLockShared(&a->lock);
    /* A recorded compilation's read of a cooked program is a use to keep. */
    if (recording) AcquireSRWLockExclusive(&a->lock); else AcquireSRWLockShared(&a->lock);
    for (na_shader_file *file = a->shader_files; file; file = file->next) if (!strcmp(file->path, path)) {
        *body = malloc(file->length + 1);
        if (*body) { memcpy(*body, file->body, file->length); (*body)[file->length] = 0; *length = file->length; result = 1; }
        else result = -1;
        if (result == 1 && recording && a->shader_recording && !a->shader_prewarming && file->program)
            na_names_add(&a->shader_used, "renderprog", file->program);
        break;
    }
    if (recording) ReleaseSRWLockExclusive(&a->lock); else ReleaseSRWLockShared(&a->lock);
    return result;
}
int sh_package_native_assets_shader_original_read(void *context, const char *path,
    unsigned char **body, size_t *length)
{
    sh_package_native_assets *a = context;
    sh_package_baseline_reader reader = NULL; void *reader_context = NULL;
    *body = NULL; *length = 0;
    if (!a || !path) return -1;
    AcquireSRWLockShared(&a->lock);
    reader = a->shader_original; reader_context = a->shader_original_context;
    ReleaseSRWLockShared(&a->lock);
    /* Configuration is immutable once shared. Do not hold an asset lock while
     * invoking the installed reader; native preparation runs outside them. */
    return reader ? reader(reader_context, path, body, length) : -1;
}

/* _vmtr selects one engine-owned provider, not one provider per package. Its
 * nominal identity is enough to establish that all untouched bindings select
 * the same external object. Pin original files without reading gigabytes of
 * unrelated pages. Authored replacements must still compare complete bytes;
 * they never acquire this identity from path or metadata equality alone. */
static int na_pages(sh_package_native_assets *a)
{
    static const char original[] = "doom2016:engine-owned-page-provider:_vmtr";
    size_t present = 0;
    sh_package_file_identity identity;
    if (a->page_state) return a->page_state;
    for (unsigned kind = 0; kind < 2; kind++) for (unsigned slot = 1; slot <= (kind ? 4u : 16u); slot++) {
        char path[96];
        snprintf(path, sizeof(path), "virtualtextures/_vmtr_%sq%u.mega2", kind ? "" : "s", slot);
        na_file *entry = na_capture(a, path);
        if (!entry || entry->status < 0) goto failed;
        if (entry->status) present += sh_package_file_length(entry->file) != 0;
    }
    if (!present) { na_error(a, "the original virtual texture provider has no page data", NULL); goto failed; }
    if (!sh_package_bytes_identity(original, sizeof(original) - 1, &identity)) goto failed;
    memcpy(a->pages, identity.digest, 32); a->page_state = 1; return 1;
failed:
    if (!*a->error) na_error(a, "cannot capture original virtual texture data", NULL);
    return -1;
}
typedef struct na_page_stream { const unsigned char *body; uint64_t length; FILE *file; } na_page_stream;
static int na_page_read(void *context, uint64_t offset, void *body, size_t length)
{
    na_page_stream *stream = context;
    if (offset > stream->length || length > stream->length - offset) return 0;
    if (stream->body) { memcpy(body, stream->body + (size_t)offset, length); return 1; }
    return stream->file && offset <= INT64_MAX && !_fseeki64(stream->file, (int64_t)offset, SEEK_SET) &&
        fread(body, 1, length, stream->file) == length;
}
static int na_page_equal(sh_package_native_assets *a, const sh_package_compilation *compiled,
    const sh_compiled_resource *resource, sh_package_file *original)
{
    na_page_stream authored = {0}, base = {0}; int result = -1;
    base.file = sh_package_file_open(original, &base.length, a->error, sizeof(a->error));
    if (!base.file) return -1;
    if (resource->body) { authored.body = resource->body; authored.length = resource->body_length; }
    else if (resource->cache_file)
        authored.file = sh_package_file_open(resource->cache_file, &authored.length, a->error, sizeof(a->error));
    else if (compiled->sources && resource->source < compiled->sources->file_count) {
        sh_package_source_file file = compiled->sources->files[resource->source];
        if (resource->cache_path) file.absolute = resource->cache_path;
        authored.length = file.length;
        authored.file = sh_package_source_open(&file, a->error, sizeof(a->error));
    }
    if (authored.body || authored.file)
        result = sh_package_virtual_page_equal(authored.length, na_page_read, &authored,
            base.length, na_page_read, &base);
    if (authored.file) fclose(authored.file);
    fclose(base.file);
    if (result < 0) na_error(a, "cannot positively compare native page bytes", resource->engine_path);
    return result;
}
static int na_original_pages(sh_package_native_assets *a, const sh_package_compilation *compiled)
{
    for (unsigned kind = 0; kind < 2; kind++) for (unsigned slot = 1; slot <= (kind ? 4u : 16u); slot++) {
        char path[96]; sh_package_file_identity authored, original;
        snprintf(path, sizeof(path), "virtualtextures/_vmtr_%sq%u.mega2", kind ? "" : "s", slot);
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, path);
        if (!resource) continue;
        na_file *entry = na_capture(a, path);
        if (!entry || entry->status < 0) return -1;
        if (!entry->status) return 0;
        if (!sh_package_compilation_identity(compiled, resource, &authored, a->error, sizeof(a->error))) return -1;
        if (authored.length != sh_package_file_length(entry->file)) return 0;
        if (!sh_package_file_get_identity(entry->file, &original))
            return na_error(a, "cannot compare original page bytes", path);
        if (memcmp(authored.digest, original.digest, 32)) return 0;
        int exact = na_page_equal(a, compiled, resource, entry->file);
        if (exact != 1) return exact;
    }
    return 1;
}
/* Page-source contract: a retained installed file, or 0 when it is absent. */
static int na_page_original(void *context, const char *path, sh_package_file **file,
    char *error, size_t capacity)
{
    sh_package_native_assets *a = context;
    na_file *entry = na_capture(a, path);
    *file = NULL;
    if (!entry || entry->status < 0) {
        if (error && capacity) snprintf(error, capacity, "cannot capture original page data: %s", path);
        return -1;
    }
    if (!entry->status) return 0;
    *file = sh_package_file_retain(entry->file);
    return *file ? 1 : -1;
}
/* Identify the scope's authored page members, counting absent ones too. */
static int na_page_key(const sh_package_compilation *compiled, unsigned char key[32],
    char *error, size_t capacity)
{
    unsigned char fields[21 * 41] = {0};
    sh_package_file_identity identity;
    for (unsigned slot = 0; slot < 21; slot++) {
        char *path = sh_package_virtual_page_companion("virtualtextures/_vmtr", slot);
        const sh_compiled_resource *resource = path ? sh_package_compilation_find(compiled, path) : NULL;
        free(path);
        if (!resource) continue;
        if (!sh_package_compilation_identity(compiled, resource, &identity, error, capacity)) return 0;
        fields[slot * 41] = 1;
        for (size_t j = 0; j < 8; j++) fields[slot * 41 + 1 + j] = (unsigned char)(identity.length >> (j * 8));
        memcpy(fields + slot * 41 + 9, identity.digest, 32);
    }
    if (!sh_package_bytes_identity(fields, sizeof(fields), &identity)) return 0;
    memcpy(key, identity.digest, 32); return 1;
}
/* Caller holds the lock. Every material of one scope shares the captured
 * family; the owned reference is released by the atlas caller. */
static sh_virtual_page_source *na_private_pages(sh_package_native_assets *a,
    const sh_package_compilation *compiled, char *error, size_t capacity)
{
    const size_t count = sizeof(a->page_sources) / sizeof(a->page_sources[0]);
    unsigned char key[32];
    sh_virtual_page_source *pages;
    if (!na_page_key(compiled, key, error, capacity)) return NULL;
    for (size_t i = 0; i < count; i++)
        if (a->page_sources[i].pages && !memcmp(a->page_sources[i].key, key, 32))
            return sh_virtual_page_source_retain(a->page_sources[i].pages);
    pages = sh_virtual_page_source_open(compiled, "virtualtextures/_vmtr", na_page_original, a, error, capacity);
    if (!pages) return NULL;
    size_t slot = a->page_source_next++ % count;
    sh_virtual_page_source_release(a->page_sources[slot].pages);
    memcpy(a->page_sources[slot].key, key, 32);
    a->page_sources[slot].pages = sh_virtual_page_source_retain(pages);
    return pages;
}
/* Refusals across every asset handle: the library compile loop compiles again
 * when this count grows, and reports the last reason. */
static SRWLOCK g_na_refusal_lock = SRWLOCK_INIT;
static uint64_t g_na_refusals;
static char g_na_refusal[1024];
/* The refused members by supplying package and engine path, for input accounting. */
typedef struct na_refusal { char package[260], path[96], reason[1024]; } na_refusal;
static na_refusal g_na_refused[32];
static size_t g_na_refused_count;
int sh_package_native_assets_page_refusal(const char *package, const char *engine_path, char *reason, size_t capacity)
{
    int found = 0;
    if (!package || !engine_path) return 0;
    AcquireSRWLockShared(&g_na_refusal_lock);
    for (size_t i = g_na_refused_count; !found && i > 0; i--)
        if (!strcmp(g_na_refused[i - 1].package, package) && !_stricmp(g_na_refused[i - 1].path, engine_path)) {
            found = 1;
            if (reason && capacity) snprintf(reason, capacity, "%s", g_na_refused[i - 1].reason);
        }
    ReleaseSRWLockShared(&g_na_refusal_lock);
    return found;
}
uint64_t sh_package_native_assets_page_refusals(char *reason, size_t capacity)
{
    AcquireSRWLockShared(&g_na_refusal_lock);
    uint64_t count = g_na_refusals;
    if (reason && capacity) snprintf(reason, capacity, "%s", g_na_refusal);
    ReleaseSRWLockShared(&g_na_refusal_lock);
    return count;
}
/* Caller holds the lock. */
static int na_page_refused(const sh_package_native_assets *a, const sh_virtual_page_source *pages)
{
    const unsigned char *identity = sh_virtual_page_source_identity(pages);
    for (size_t i = 0; identity && i < a->page_refused_count; i++)
        if (!memcmp(a->page_refused[i], identity, 32)) return 1;
    return 0;
}
static uint32_t na_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int na_level0_read(const unsigned char header[0x170], na_level0 *out)
{
    uint32_t extent = na_u32(header + 0x10), grid = na_u32(header + 0x18);
    if (na_u32(header) != 0xa63fbb21u || na_u32(header + 4) != 2 || !grid || extent % grid) return 0;
    *out = (na_level0){extent / grid, na_u32(header + 0x50), na_u32(header + 0x54), na_u32(header + 0x58),
        na_u32(header + 0x5c), na_u32(header + 0x60), (uint64_t)na_u32(header + 0x40) | (uint64_t)na_u32(header + 0x44) << 32};
    return out->texels && out->width && out->height;
}
/* The level-0 index cells of one member, read once: installed members by
 * slot (immutable), effective members by family identity and slot. */
static void na_cells_free(na_cells *cells)
{ free(cells->cells); memset(cells, 0, sizeof(*cells)); }
/* Caller holds the lock. 1 read, 0 absent/empty/unusable header, -1 failure. */
static int na_installed_cells(sh_package_native_assets *a, unsigned slot)
{
    na_cells *out = a->page_installed + slot;
    if (out->state) return out->state == 1;
    char path[96]; unsigned char header[0x170];
    snprintf(path, sizeof(path), "virtualtextures/_vmtr_sq%u.mega2", slot + 1);
    na_file *entry = na_capture(a, path);
    if (!entry || entry->status < 0) return -1;
    out->state = 2;
    if (!entry->status) return 0;
    na_page_stream base = {0};
    if (!(base.file = sh_package_file_open(entry->file, &base.length, a->error, sizeof(a->error)))) { out->state = 0; return -1; }
    int ok = base.length >= sizeof(header) && na_page_read(&base, 0, header, sizeof(header)) && na_level0_read(header, &out->geometry);
    size_t count = ok ? (size_t)out->geometry.width * out->geometry.height : 0;
    if (ok && (!(out->cells = malloc(count * 4)) ||
        !na_page_read(&base, out->geometry.index + 4 * (uint64_t)out->geometry.first, out->cells, count * 4))) {
        fclose(base.file); na_cells_free(out); return -1;
    }
    fclose(base.file);
    if (ok) out->state = 1;
    return ok;
}
static int na_effective_cells(sh_package_native_assets *a, const sh_virtual_page_source *pages, unsigned slot,
    na_cells **found)
{
    const unsigned char *identity = sh_virtual_page_source_identity(pages);
    const size_t count = sizeof(a->page_effective) / sizeof(a->page_effective[0]);
    for (size_t i = 0; i < count; i++)
        if (a->page_effective[i].state && a->page_effective[i].slot == slot && !memcmp(a->page_effective[i].identity, identity, 32)) {
            *found = a->page_effective + i; return 1;
        }
    na_cells *out = a->page_effective + a->page_effective_next++ % count;
    na_cells_free(out);
    unsigned char header[0x170];
    int present = sh_virtual_page_source_member_read(pages, slot, 0, header, sizeof(header), a->error, sizeof(a->error));
    if (present < 0) return -1;
    if (present && na_level0_read(header, &out->geometry)) {
        size_t cells = (size_t)out->geometry.width * out->geometry.height;
        if (!(out->cells = malloc(cells * 4))) return -1;
        if (sh_virtual_page_source_member_read(pages, slot, out->geometry.index + 4 * (uint64_t)out->geometry.first,
            out->cells, cells * 4, a->error, sizeof(a->error)) != 1) { na_cells_free(out); return -1; }
    }
    memcpy(out->identity, identity, 32); out->slot = slot; out->state = 1;
    *found = out; return 1;
}
/* Row completeness (sh_package_virtual_atlas.row_complete): every level-0 page
 * the installed member holds inside the row must also be in the effective
 * member. Only members the row lies in are compared, a member captured from
 * the installed original passes unread, and each member's level-0 index is
 * read once. A refusal is remembered for the family, so the next compile
 * binds its materials to the engine's provider and the authored members serve
 * nothing. */
static int na_page_row(void *context, const sh_package_compilation *compiled, const sh_virtual_page_source *pages,
    const sh_virtual_material_record *row, const char *material, char *error, size_t capacity)
{
    sh_package_native_assets *a = context;
    if (!a || !pages || !row || !material) return -1;
    if (row->width <= 0 || row->height <= 0 || row->x < 0 || row->y < 0) return 1;
    int result = 1;
    AcquireSRWLockExclusive(&a->lock);
    for (unsigned slot = 0; result == 1 && slot < 16; slot++) {
        if (sh_virtual_page_source_member_installed(pages, slot)) continue;
        int status = na_installed_cells(a, slot);
        if (status < 0) { result = -1; break; }
        if (!status) continue;
        const na_level0 *installed = &a->page_installed[slot].geometry;
        int64_t t = installed->texels;
        int64_t x0 = row->x / t, y0 = row->y / t, x1 = ((int64_t)row->x + row->width - 1) / t,
            y1 = ((int64_t)row->y + row->height - 1) / t;
        if (x0 < installed->x) x0 = installed->x;
        if (y0 < installed->y) y0 = installed->y;
        if (x1 >= (int64_t)installed->x + installed->width) x1 = (int64_t)installed->x + installed->width - 1;
        if (y1 >= (int64_t)installed->y + installed->height) y1 = (int64_t)installed->y + installed->height - 1;
        if (x0 > x1 || y0 > y1) continue; /* The row lies in another member. */
        na_cells *effective = NULL;
        if (na_effective_cells(a, pages, slot, &effective) < 0) { result = -1; break; }
        /* The effective member answers only with the same grid and the same placement. */
        const na_level0 *authored = &effective->geometry;
        int same = effective->cells && authored->texels == installed->texels && authored->x == installed->x &&
            authored->y == installed->y && authored->width == installed->width && authored->height == installed->height;
        size_t needed = 0, missing = 0;
        for (int64_t gy = y0; gy <= y1; gy++)
            for (int64_t gx = x0; gx <= x1; gx++) {
                size_t at = (size_t)(gy - installed->y) * installed->width + (size_t)(gx - installed->x);
                if (a->page_installed[slot].cells[at] == UINT32_MAX) continue;
                needed++;
                if (!same || effective->cells[at] == UINT32_MAX) missing++;
            }
        if (!missing) continue;
        /* The supplying package, by the authored member's owners. */
        char path[96];
        snprintf(path, sizeof(path), "virtualtextures/_vmtr_sq%u.mega2", slot + 1);
        const sh_compiled_resource *member = sh_virtual_page_source_member(pages, slot);
        const char *package = "";
        for (size_t o = 0; member && compiled && compiled->sources && o < compiled->sources->package_count && !*package; o++)
            if (sh_package_owners_contains(&member->owners, o)) package = compiled->sources->packages[o].name;
        char reason[1024];
        snprintf(reason, sizeof(reason), "virtual texture member refused: package %s supplies %s without row %d %d %d %d "
            "that material %s reads (%zu of %zu level-0 pages missing); the package's virtual texture member is left out "
            "and its other files install", *package ? package : "(unknown)", path,
            (int)row->x, (int)row->y, (int)row->width, (int)row->height, material, missing, needed);
        const unsigned char *identity = sh_virtual_page_source_identity(pages);
        if (identity && !na_page_refused(a, pages)) {
            unsigned char (*grown)[32] = realloc(a->page_refused, (a->page_refused_count + 1) * 32);
            if (!grown) { result = -1; break; }
            a->page_refused = grown; memcpy(a->page_refused[a->page_refused_count++], identity, 32);
        }
        AcquireSRWLockExclusive(&g_na_refusal_lock);
        g_na_refusals++; snprintf(g_na_refusal, sizeof(g_na_refusal), "%s", reason);
        {
            na_refusal *entry = g_na_refused + (g_na_refused_count < 32 ? g_na_refused_count++ : 31);
            snprintf(entry->package, sizeof(entry->package), "%s", package);
            snprintf(entry->path, sizeof(entry->path), "%s", path);
            snprintf(entry->reason, sizeof(entry->reason), "%s", reason);
        }
        ReleaseSRWLockExclusive(&g_na_refusal_lock);
        if (error && capacity) snprintf(error, capacity, "%s", reason);
        result = 0;
    }
    if (result < 0 && error && capacity && !*error)
        snprintf(error, capacity, "%s", *a->error ? a->error : "cannot check virtual texture row pages");
    ReleaseSRWLockExclusive(&a->lock);
    return result;
}
static int na_admit(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    /* Both supported renderers register these intrinsic images at startup.
     * This is a native ABI set, not a filename-prefix or missing-file rule.
     * The caller has already excluded authored files and failed source reads. */
    static const char *const images[] = {
        "_default", "_default3d", "_white", "_borderedpointwhite", "_black",
        "_defaultcube", "_nofalloff", "_dither2", "_dither4", "_dither16",
        "_dither256", "_fresnel", "_blackcube"
    };
    /* Renderer initialization in both renderers also creates these by name:
     * the global VMTR page table (its virtual texture's mode-0 construction),
     * the Vmtr physical page cache, the transparency and light image atlases
     * and the augment target. DeclFind reserves _emptyName for an empty name.
     * Stock material text binds them by these names. */
    static const char *const created[] = {
        "_vmtrpagetable", "_physicalvmtrpages0", "_physicalvmtrpages3", "_physicalvmtrmappings",
        "_transsortimageatlas", "_lightimageatlas", "_augment", "_emptyname"
    };
    const char *kind = NULL;
    (void)context; (void)scope;
    memset(semantics, 0, 32);
    if (!family || !name || strcmp(family, "image")) return 0;
    for (size_t i = 0; !kind && i < sizeof(images) / sizeof(*images); i++)
        if (!strcmp(name, images[i])) kind = "intrinsic-image";
    for (size_t i = 0; !kind && i < sizeof(created) / sizeof(*created); i++)
        if (!strcmp(name, created[i])) kind = "renderer-image";
    if (!kind) return 0;
    char identity[128]; sh_package_file_identity digest;
    int n = snprintf(identity, sizeof(identity), "doom2016:engine-owned-%s:%s", kind, name);
    if (n < 0 || (size_t)n >= sizeof(identity) ||
        !sh_package_bytes_identity(identity, (size_t)n, &digest)) {
        if (error && capacity) snprintf(error, capacity, "cannot identify native engine image: %s", name);
        return -1;
    }
    memcpy(semantics, digest.digest, 32); return 1;
}

static int na_semantics(void *context, size_t scope, const char *family, const char *name,
    unsigned char semantics[32], char *error, size_t capacity)
{
    (void)context; (void)scope;
    memset(semantics, 0, 32);
    if (!family || !*family || !name || !*name) return 0;
    if (!strcmp(family, "sound")) {
        uint32_t event = 0; char identity[64]; sh_package_file_identity digest;
        if (!sh_audio_event_identity(name, &event)) {
            if (error && capacity) snprintf(error, capacity,
                "sound name cannot be represented by native event conversion: %s", name);
            return -1;
        }
        int length = snprintf(identity, sizeof(identity), "doom2016:wwise-event:%08x", event);
        if (length < 0 || (size_t)length >= sizeof(identity) ||
            !sh_package_bytes_identity(identity, (size_t)length, &digest)) return -1;
        memcpy(semantics, digest.digest, 32);
        /* The event is derived from the resource name after parsing. Exact
         * original sound bytes and dependencies must keep that name. This
         * certifies the implicit name binding, not event/bank availability;
         * changed audio still needs the separate audio dependency adapter. */
        return 1;
    }
    if (!strcmp(family, "font")) {
        /* A font's family meaning is the native hash of its resource name
         * (idFont+0x4c; both text typings read it, R-309), not its bytes or
         * files: two names that load the same font directory hash apart when
         * they differ beyond ASCII case. A renamed private form carries the
         * hash of the request it serves, so forms are deduplicated only within
         * one hash; equal bytes under another family stay a separate form. */
        char identity[64]; sh_package_file_identity digest;
        int length = snprintf(identity, sizeof(identity), "doom2016:font-name-hash:%08x",
            (unsigned)sh_swf_font_name_hash(name));
        if (length < 0 || (size_t)length >= sizeof(identity) ||
            !sh_package_bytes_identity(identity, (size_t)length, &digest)) return -1;
        memcpy(semantics, digest.digest, 32);
        return 1;
    }
    /* This permits retaining the same native name, not renaming an original
     * or declaring its parse name-independent. The source driver has completed
     * typed closure and still requires every file to equal the exact installed
     * original; the linker then requires original bound dependencies. Family
     * readers and shader/atlas adapters retain their additional ABI checks.
     * Source-free identities take na_admit instead of this certificate. */
    return 1;
}

static int na_shader_error(char *error, size_t capacity, const char *reason, const char *name)
{ if (error && capacity) snprintf(error, capacity, "%s: %s", reason, name); return 0; }
/* Append the native state that requires preparation to its request reason;
 * diagnostics only. */
static void na_shader_pending_state(char *error, size_t capacity, const char *state)
{
    size_t used = error && capacity ? strlen(error) : 0;
    if (used && used + 4 < capacity) snprintf(error + used, capacity - used, " (%s)", state);
}
/* Keep the queue's first request reason (caller holds the lock). */
static void na_shader_note_need(sh_package_native_assets *a, const char *reason)
{
    if (!a->shader_first_need[0] && reason && *reason)
        snprintf(a->shader_first_need, sizeof(a->shader_first_need), "%s", reason);
}
static size_t na_shader_original_at(na_shader_original *const *index, size_t capacity, const char *path)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char *c = (const unsigned char *)path; *c; c++) h = (h ^ *c) * UINT64_C(1099511628211);
    size_t mask = capacity - 1, at = (size_t)(h ^ (h >> 31)) & mask;
    while (index[at] && strcmp(index[at]->path, path)) at = (at + 1) & mask;
    return at;
}
/* Index an original (caller holds the lock); when the index cannot grow it is dropped and lookups walk the list. */
static void na_shader_original_index(sh_package_native_assets *a, na_shader_original *entry)
{
    if (a->shader_original_count + 1 > a->shader_original_capacity / 2) {
        size_t capacity = a->shader_original_capacity ? a->shader_original_capacity * 2 : 256, count = 0;
        na_shader_original **grown = capacity <= SIZE_MAX / 2 / sizeof(*grown) ? calloc(capacity, sizeof(*grown)) : NULL;
        if (!grown) {
            free(a->shader_original_index); a->shader_original_index = NULL;
            a->shader_original_capacity = a->shader_original_count = 0; return;
        }
        for (na_shader_original *other = a->shader_originals; other; other = other->next)
            if (other != entry) { grown[na_shader_original_at(grown, capacity, other->path)] = other; count++; }
        free(a->shader_original_index); a->shader_original_index = grown;
        a->shader_original_capacity = capacity; a->shader_original_count = count;
    }
    size_t at = na_shader_original_at(a->shader_original_index, a->shader_original_capacity, entry->path);
    if (!a->shader_original_index[at]) { a->shader_original_index[at] = entry; a->shader_original_count++; }
}
static na_shader_original *na_shader_source(sh_package_native_assets *a, const char *family,
    const char *name, char *error, size_t capacity)
{
    char path[512]; unsigned char *body = NULL; size_t length = 0;
    int n = snprintf(path, sizeof(path), "generated/decls/%s/%s.decl", family, name);
    if (n < 0 || n >= (int)sizeof(path)) return NULL;
    char *canonical = sh_package_engine_path(path);
    if (!canonical || strcmp(path, canonical)) { free(canonical); return NULL; }
    if (a->shader_original_capacity) {
        na_shader_original *known = a->shader_original_index[na_shader_original_at(a->shader_original_index,
            a->shader_original_capacity, path)];
        if (known) { free(canonical); return known; }
    } else for (na_shader_original *entry = a->shader_originals; entry; entry = entry->next)
        if (!strcmp(entry->path, path)) { free(canonical); return entry; }
    int status = a->shader_original(a->shader_original_context, path, &body, &length);
    if ((status != 1 && status != 2) || !body || length == SIZE_MAX) {
        free(canonical); free(body);
        na_shader_error(error, capacity, status < 0 ? "original shader ABI source is unreadable" :
            "shader ABI has no verified installed original source", name); return NULL;
    }
    na_shader_original *entry = calloc(1, sizeof(*entry));
    if (!entry) { free(canonical); free(body); return NULL; }
    entry->path = canonical; entry->body = body; entry->length = length;
    entry->next = a->shader_originals; a->shader_originals = entry;
    na_shader_original_index(a, entry);
    return entry;
}
static int na_shader_index(int kind, uint32_t index)
{
    static const uint16_t low[] = {0,0x400,0x400,0x400,0x400,0x400,0x400,0x400,0x400,0x400,0x600,0x680,0x800,0x880,0x800,0x8c0,0x900,0x900,0x900};
    static const uint16_t high[] = {0x400,0x600,0x600,0x600,0x600,0x600,0x600,0x600,0x600,0x600,0x680,0x800,0x880,0x8c0,0x880,0x900,0x93f,0x93f,0x93f};
    return kind >= 0 && kind < 19 && index >= low[kind] && index < high[kind];
}
static int na_shader_request_add(sh_package_native_assets *a, const char *family,
    const char *name, sh_decl_source source, const sh_native_resource_identity *identity, na_shader_original *original)
{
    if (a->shader_recording && !a->shader_prewarming) na_names_add(&a->shader_used, family, name);
    for (na_shader_request *request = a->shader_requests; request; request = request->next)
        if (!strcmp(request->value.family, family) && !strcmp(request->value.name, name))
            return request->value.source_length == source.length &&
                !memcmp(request->value.source, source.text, source.length) &&
                request->value.native_id == identity->id && request->value.manager == identity->manager;
    na_shader_request *request = calloc(1, sizeof(*request));
    if (!request) return 0;
    request->value = (sh_package_shader_request){_strdup(family), _strdup(name),
        malloc(source.length ? source.length : 1), source.length, identity->manager, identity->resource, identity->id};
    if (!request->value.family || !request->value.name || !request->value.source) {
        free((void *)request->value.family); free((void *)request->value.name);
        free((void *)request->value.source); free(request); return 0;
    }
    memcpy((void *)request->value.source, source.text, source.length);
    if (!original->task_id) original->task_id = ++a->next_shader_task;
    request->value.task_id = original->task_id;
    request->next = a->shader_requests; a->shader_requests = request; a->shader_request_count++; return 1;
}
static int na_shader_fixed(sh_package_native_assets *a, const char *family, const char *name,
    sh_decl_source source, int *kind, uint32_t *index, uint64_t *mask, int request_prepare, char *error, size_t capacity)
{
    unsigned char before[0x90], after[0x90], *native_text = NULL;
    sh_native_resource_registry *registry = NULL; sh_native_resource_identity identity = {0};
    int ok = 0, source_kind = -1; uintptr_t text = 0, native_error = 0;
    int32_t length = -1; size_t bytes = kind ? sizeof(before) : 0x60;
    if (error && capacity) *error = 0;
    if (!a || !a->shader_configured || !name || !*name || !source.text || source.length == SIZE_MAX ||
        (kind ? !index : !mask))
        return na_shader_error(error, capacity, "shader ABI requires a physically supplied original source", name ? name : "");
    AcquireSRWLockExclusive(&a->lock);
    na_shader_original *original = na_shader_source(a, family, name, error, capacity);
    if (!original) goto done;
    if (source.length != original->length || memcmp(source.text, original->body, source.length)) {
        na_shader_error(error, capacity, "shader ABI source differs from its installed original", name); goto done;
    }
    if (kind && !sh_decl_material_parameter_kind(source, &source_kind, error, capacity)) goto done;
    /* A name without native identity semantics is this check's own failure;
     * rejecting it first keeps a shared snapshot's errors meaning a change. */
    char *identity_name = sh_native_resource_name(name);
    if (!identity_name) { na_shader_error(error, capacity, "shader ABI name has no native identity", name); goto done; }
    free(identity_name);
    registry = na_shader_registry(a, error, capacity);
    int identity_status = registry ? sh_native_resource_registry_identity(registry, family, name, &identity, error, capacity) : SH_NATIVE_RESOURCE_ID_ERROR;
    if (identity_status == SH_NATIVE_RESOURCE_ID_ERROR && registry && registry == a->shader_snapshot) {
        /* Native identities changed under the shared snapshot. Detach it; the
         * release below closes it and the scope end reports the change. */
        a->shader_scope_changed = 1; a->shader_snapshot = NULL;
    }
    if (identity_status == SH_NATIVE_RESOURCE_ID_FREE) {
        if (request_prepare && na_shader_registry_valid(a, registry, &identity, error, capacity) &&
            na_shader_request_add(a, family, name, source, &identity, original)) {
            na_shader_error(error, capacity, "shader environment needs verified native source preparation", name);
            na_shader_pending_state(error, capacity, "no native object");
            na_shader_note_need(a, error);
            ok = 2;
        }
        goto done;
    }
    if (identity_status != SH_NATIVE_RESOURCE_ID_SAME_NAME || !identity.resource) goto done;
    if (!a->shader_registry.read(a->shader_registry.context, identity.resource, before, bytes)) goto done;
    memcpy(&native_error, before + 0x18, 8); memcpy(&text, before + 0x38, 8); memcpy(&length, before + 0x40, 4);
    size_t trimmed = source.length;
    while (trimmed && (source.text[trimmed - 1] == ' ' || source.text[trimmed - 1] == '\r' || source.text[trimmed - 1] == '\n')) trimmed--;
    if (native_error || (before[0x2c] & 7)) {
        na_shader_error(error, capacity, "native shader ABI is pending or defaulted", name); goto done;
    }
    int receipt = sh_package_shader_receipt_matches(identity.resource, family, name,
        source.text, source.length, before, bytes);
    if (!receipt && (!text || length < 0 || (size_t)length != trimmed ||
        !(native_text = malloc(trimmed + 1)) ||
        !a->shader_registry.read(a->shader_registry.context, text, native_text, trimmed + 1) || native_text[trimmed] ||
        memcmp(native_text, source.text, trimmed))) {
        if (request_prepare && na_shader_registry_valid(a, registry, &identity, error, capacity) &&
            na_shader_request_add(a, family, name, source, &identity, original)) {
            char state[192], refusal[96];
            na_shader_error(error, capacity, "shader environment needs verified native source preparation", name);
            sh_package_shader_receipt_refusal(identity.resource, family, name, source.text, source.length,
                before, bytes, refusal, sizeof(refusal));
            if (!text) snprintf(state, sizeof(state), "%s; native text absent", refusal);
            else if (length < 0 || (size_t)length != trimmed)
                snprintf(state, sizeof(state), "%s; native text has %d bytes, source %zu", refusal, (int)length, trimmed);
            else snprintf(state, sizeof(state), "%s; native text differs or is unreadable", refusal);
            na_shader_pending_state(error, capacity, state);
            na_shader_note_need(a, error);
            ok = 2;
        } else na_shader_error(error, capacity, "native shader ABI needs exact retained text or a successful source receipt", name);
        goto done;
    }
    if (kind) {
        int actual_kind; uint32_t actual_index;
        memcpy(&actual_index, before + 0x68, 4); memcpy(&actual_kind, before + 0x6c, 4);
        if (actual_kind != source_kind || !na_shader_index(actual_kind, actual_index)) {
            na_shader_error(error, capacity, "native shader parameter kind/index is incompatible", name); goto done;
        }
        *kind = actual_kind; *index = actual_index;
    } else memcpy(mask, before + 0x58, 8);
    /* Source/default/layout fields and native membership must stay stable over
     * the read. The source linker separately retains the fixed dependencies and
     * refuses changed bindings; this callback is not payload equality by name. */
    if (!a->shader_registry.read(a->shader_registry.context, identity.resource, after, bytes) || memcmp(before, after, bytes) ||
        (receipt ? !sh_package_shader_receipt_matches(identity.resource, family, name, source.text, source.length, after, bytes) :
         (!a->shader_registry.read(a->shader_registry.context, text, native_text, trimmed + 1) || native_text[trimmed] ||
          memcmp(native_text, source.text, trimmed))) || !na_shader_registry_valid(a, registry, &identity, error, capacity)) {
        na_shader_error(error, capacity, "native shader ABI changed during its snapshot", name); goto done;
    }
    ok = 1;
    /* Ready because this session prepared it: a later session needs that
     * preparation again, so a recorded compilation keeps it. A receipt alone
     * does not show this; the engine's own loads produce receipts too. */
    if (a->shader_recording && !a->shader_prewarming && na_names_contains(&a->shader_prepared, family, name))
        na_names_add(&a->shader_used, family, name);
done:
    if (!ok && error && capacity && !*error) na_shader_error(error, capacity, "shader ABI has no compatible ready native object", name);
    free(native_text); sh_native_resource_identity_clear(&identity); na_shader_registry_release(a, registry);
    ReleaseSRWLockExclusive(&a->lock); return ok;
}
static int na_shader_parameter(void *context, const char *name, sh_decl_source source,
    int *kind, uint32_t *index, char *error, size_t capacity)
{ return na_shader_fixed(context, "renderparm", name, source, kind, index, NULL, 1, error, capacity); }
static int na_shader_flag(void *context, const char *name, sh_decl_source source,
    uint64_t *mask, char *error, size_t capacity)
{ return na_shader_fixed(context, "renderprogflag", name, source, NULL, NULL, mask, 1, error, capacity); }
static int na_shader_program(void *context, const char *name, sh_decl_source source, char *error, size_t capacity)
{
    sh_package_native_assets *a = context; sh_native_resource_identity identity = {0};
    sh_native_resource_registry *registry = NULL; int queued = 0;
    if (!a || a->shader.renderer != SH_PACKAGE_SHADER_OPENGL || !source.text || !source.length)
        return na_shader_error(error, capacity, "shader source cooking requires the OpenGL native preparation contract", name);
    AcquireSRWLockExclusive(&a->lock);
    na_shader_original *original = na_shader_source(a, "renderprog", name, error, capacity);
    if (!original || source.length != original->length || memcmp(source.text, original->body, source.length)) {
        na_shader_error(error, capacity, "source-only shader differs from its installed original", name); goto done;
    }
    registry = sh_native_resource_registry_open(a->shader_registry, error, capacity);
    int status = registry ? sh_native_resource_registry_identity(registry, "renderprog", name, &identity, error, capacity) : SH_NATIVE_RESOURCE_ID_ERROR;
    if ((status == SH_NATIVE_RESOURCE_ID_SAME_NAME || status == SH_NATIVE_RESOURCE_ID_FREE) &&
        sh_native_resource_registry_validate(registry, error, capacity))
        queued = na_shader_request_add(a, "renderprog", name, source, &identity, original);
    na_shader_error(error, capacity, queued ? "shader environment needs verified native source cooking" :
        "source-only shader has no stable native preparation identity", name);
    if (queued) na_shader_note_need(a, error);
done:
    sh_native_resource_identity_clear(&identity); sh_native_resource_registry_close(registry);
    ReleaseSRWLockExclusive(&a->lock); return queued ? 1 : 0;
}
void sh_package_native_assets_shader_record(sh_package_native_assets *a, int on)
{
    if (!a) return;
    AcquireSRWLockExclusive(&a->lock);
    if (on) na_names_free(&a->shader_used);
    a->shader_recording = on != 0;
    ReleaseSRWLockExclusive(&a->lock);
}
typedef struct na_binary_path { char path[512]; } na_binary_path;
static int na_binary_path_visit(void *context, sh_package_shader_path_kind kind, const char *original, const char *linked)
{
    (void)linked;
    if (kind == SH_PACKAGE_SHADER_BINARY) snprintf(((na_binary_path *)context)->path, sizeof(((na_binary_path *)context)->path), "%s", original);
    return 1;
}
/* 1 when this program's cooked binary was captured here: a compilation then
 * reads it and requests no preparation. */
static int na_shader_captured(sh_package_native_assets *a, const char *name)
{
    na_binary_path binary = {{0}}; sh_package_shader_program empty = {0}; char error[256]; int found = 0;
    if (sh_package_shader_paths(a->shader.renderer, name, name, &empty, na_binary_path_visit, &binary,
        error, sizeof(error)) != SH_PACKAGE_SHADER_OK || !*binary.path) return 0;
    AcquireSRWLockShared(&a->lock);
    for (na_shader_file *file = a->shader_files; file && !found; file = file->next) found = !strcmp(file->path, binary.path);
    ReleaseSRWLockShared(&a->lock); return found;
}
int sh_package_native_assets_shader_prewarm(sh_package_native_assets *a, const char *family, const char *name)
{
    char error[256]; int kind; uint32_t index; uint64_t mask; int ready;
    if (!a || !family || !name || !*name || !a->shader_configured) return -1;
    int program = !strcmp(family, "renderprog"), parameter = !strcmp(family, "renderparm");
    if (!program && !parameter && strcmp(family, "renderprogflag")) return 0;
    if (program && na_shader_captured(a, name)) return 0;
    AcquireSRWLockExclusive(&a->lock);
    na_shader_original *original = na_shader_source(a, family, name, error, sizeof(error));
    size_t before = a->shader_request_count;
    if (original) a->shader_prewarming = 1;
    ReleaseSRWLockExclusive(&a->lock);
    if (!original) return -1;
    /* The same checks a compilation makes: a ready object queues nothing.
     * Originals are never freed while the assets live. */
    sh_decl_source source = {(const char *)original->body, original->length};
    if (program) ready = a->shader.renderer == SH_PACKAGE_SHADER_OPENGL ?
        na_shader_program(a, name, source, error, sizeof(error)) : 0;
    else ready = na_shader_fixed(a, family, name, source, parameter ? &kind : NULL, parameter ? &index : NULL,
        parameter ? NULL : &mask, 1, error, sizeof(error));
    (void)ready;
    AcquireSRWLockExclusive(&a->lock);
    a->shader_prewarming = 0;
    int queued = a->shader_request_count > before;
    ReleaseSRWLockExclusive(&a->lock);
    return queued;
}
static int na_names_order(const void *x, const void *y)
{ return strcmp(*(const char *const *)x, *(const char *const *)y); }
char *sh_package_native_assets_shader_needs(sh_package_native_assets *a)
{
    if (!a) return NULL;
    AcquireSRWLockShared(&a->lock);
    size_t length = 1, count = 0;
    const char **keys = calloc(a->shader_used.count ? a->shader_used.count : 1, sizeof(*keys));
    for (size_t i = 0; keys && i < a->shader_used.capacity; i++) if (a->shader_used.slots[i]) {
        keys[count++] = a->shader_used.slots[i]; length += strlen(a->shader_used.slots[i]) + 1;
    }
    char *out = keys ? malloc(length) : NULL, *at = out;
    if (out) {
        qsort(keys, count, sizeof(*keys), na_names_order);
        for (size_t i = 0; i < count; i++) { size_t n = strlen(keys[i]); memcpy(at, keys[i], n); at[n] = '\n'; at += n + 1; }
        *at = 0;
    }
    ReleaseSRWLockShared(&a->lock);
    free(keys); return out;
}
size_t sh_package_native_assets_shader_follow(sh_package_native_assets *a)
{
    char error[256];
    if (!a || !a->shader_configured || a->shader.renderer != SH_PACKAGE_SHADER_OPENGL) return 0;
    AcquireSRWLockExclusive(&a->lock);
    na_names follow = a->shader_follow; memset(&a->shader_follow, 0, sizeof(a->shader_follow));
    size_t before = a->shader_request_count;
    ReleaseSRWLockExclusive(&a->lock);
    for (size_t i = 0; i < follow.capacity; i++) if (follow.slots[i]) {
        const char *name = strchr(follow.slots[i], '\t') + 1;
        if (strncmp(follow.slots[i], "renderprog\t", 11)) {
            /* A parameter or flag a cooked program checks: the same check
             * and request a compilation makes on reaching it. */
            char family[32]; size_t n = (size_t)(name - 1 - follow.slots[i]);
            if (n < sizeof(family)) {
                memcpy(family, follow.slots[i], n); family[n] = 0;
                (void)sh_package_native_assets_shader_prewarm(a, family, name);
            }
            continue;
        }
        if (na_shader_captured(a, name)) continue;
        AcquireSRWLockExclusive(&a->lock);
        na_shader_original *original = na_shader_source(a, "renderprog", name, error, sizeof(error));
        ReleaseSRWLockExclusive(&a->lock);
        /* The request a compilation makes on reaching this program without a
         * cooked binary; a name with no installed original is not followed. */
        if (original) (void)na_shader_program(a, name,
            (sh_decl_source){(const char *)original->body, original->length}, error, sizeof(error));
    }
    na_names_free(&follow);
    AcquireSRWLockShared(&a->lock);
    size_t queued = a->shader_request_count > before ? a->shader_request_count - before : 0;
    ReleaseSRWLockShared(&a->lock);
    return queued;
}
/* Hint text: the owner's exact header, a digest line for the body, then the
 * body's "family\tname" lines. */
static uint64_t na_hint_digest(const char *body, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) { hash ^= (unsigned char)body[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
char *sh_package_framed_format(const char *header, const char *body)
{
    if (!header || !body) return NULL;
    size_t h = strlen(header), n = strlen(body), capacity = h + n + 64;
    char *out = malloc(capacity);
    if (!out) return NULL;
    int line = snprintf(out, capacity, "%sbody %016llx %zu\n", header,
        (unsigned long long)na_hint_digest(body, n), n);
    if (line < 0 || (size_t)line + n >= capacity) { free(out); return NULL; }
    memcpy(out + line, body, n + 1); return out;
}
const char *sh_package_framed_body(const char *text, size_t length, const char *header, size_t *body_length)
{
    size_t h = header ? strlen(header) : 0;
    if (body_length) *body_length = 0;
    if (!text || !header || !body_length || length < h || memcmp(text, header, h)) return NULL;
    const char *at = text + h, *end = text + length, *newline = memchr(at, '\n', (size_t)(end - at));
    char line[80], canonical[80]; unsigned long long digest = 0; size_t declared = 0;
    if (!newline || (size_t)(newline - at) >= sizeof(line)) return NULL;
    memcpy(line, at, (size_t)(newline - at)); line[newline - at] = 0;
    if (sscanf_s(line, "body %16llx %zu", &digest, &declared) != 2) return NULL;
    snprintf(canonical, sizeof(canonical), "body %016llx %zu", digest, declared);
    if (strcmp(canonical, line)) return NULL;
    at = newline + 1;
    if ((size_t)(end - at) != declared || na_hint_digest(at, declared) != digest) return NULL;
    *body_length = declared; return at;
}
/* A hint names objects only. Their authority stays native and current:
 * prewarming reads each name's installed original and makes the compile's own
 * identity and ABI checks (a name without an installed original is skipped),
 * and the needs a compile records exclude prewarming and replace the previous
 * ones (sh_package_native_assets_shader_record), so a name the current compile
 * does not need is warmed for at most one session and leaves the next hint.
 * The header keeps what decides those checks: the renderer, the game image
 * (build_identity's "game" line; any "product" line and after is not part of
 * it) and this contract's revision, raised whenever the meaning of a name or
 * of its preparation changes. */
int sh_package_shader_hint_header(char *out, size_t capacity, int vulkan, const char *build_identity)
{
    const char *product = build_identity ? strstr(build_identity, "product ") : NULL;
    size_t game = build_identity ? (product ? (size_t)(product - build_identity) : strlen(build_identity)) : 0;
    if (!out || !capacity || !game || game > 200) return 0;
    int n = snprintf(out, capacity, "snapmap-plus shader preparation v3\nrenderer %s\n%.*s",
        vulkan ? "vulkan" : "opengl", (int)game, build_identity);
    return n > 0 && (size_t)n < capacity && out[n - 1] == '\n';
}
char *sh_package_shader_hint_format(const char *header, const char *needs)
{ return sh_package_framed_format(header, needs); }
int sh_package_shader_hint_read(const char *text, size_t length, const char *header,
    int (*visit)(void *context, const char *family, const char *name), void *context)
{
    static const char *const families[] = {"renderprog", "renderparm", "renderprogflag"};
    size_t declared = 0;
    const char *at = visit ? sh_package_framed_body(text, length, header, &declared) : NULL, *end = at + declared;
    if (!at) return -1;
    /* Validate every line before any visit: a hint is used whole or not at all. */
    int count = 0;
    for (int pass = 0; pass < 2; pass++) for (const char *p = at; p < end; ) {
        const char *stop = memchr(p, '\n', (size_t)(end - p)), *tab;
        char family[32], name[256];
        if (!stop || !(tab = memchr(p, '\t', (size_t)(stop - p))) || tab == p ||
            (size_t)(tab - p) >= sizeof(family) || stop - tab - 1 <= 0 || (size_t)(stop - tab - 1) >= sizeof(name)) return -1;
        memcpy(family, p, (size_t)(tab - p)); family[tab - p] = 0;
        memcpy(name, tab + 1, (size_t)(stop - tab - 1)); name[stop - tab - 1] = 0;
        int known = 0;
        for (size_t f = 0; f < sizeof(families) / sizeof(*families); f++) known |= !strcmp(family, families[f]);
        for (const char *c = name; *c; c++) if ((unsigned char)*c < 0x21 || (unsigned char)*c > 0x7e) known = 0;
        if (!known) return -1;
        if (pass) { if (!visit(context, family, name)) return -1; count++; }
        p = stop + 1;
    }
    return count;
}
size_t sh_package_native_assets_shader_pending(sh_package_native_assets *a)
{
    if (!a) return 0; AcquireSRWLockShared(&a->lock);
    size_t count = a->shader_request_count; ReleaseSRWLockShared(&a->lock); return count;
}
sh_package_shader_preparation *sh_package_native_assets_shader_take(sh_package_native_assets *a)
{
    if (!a) return NULL;
    sh_package_shader_preparation *batch = calloc(1, sizeof(*batch)); if (!batch) return NULL;
    AcquireSRWLockExclusive(&a->lock);
    batch->requests = a->shader_requests; batch->count = a->shader_request_count;
    memcpy(batch->first_need, a->shader_first_need, sizeof(batch->first_need));
    a->shader_requests = NULL; a->shader_request_count = 0; a->shader_first_need[0] = 0;
    ReleaseSRWLockExclusive(&a->lock);
    if (!batch->count) { free(batch); return NULL; }
    batch->assets = sh_package_native_assets_retain(a); return batch;
}
size_t sh_package_shader_preparation_count(const sh_package_shader_preparation *batch)
{ return batch ? batch->count : 0; }
const char *sh_package_shader_preparation_first_need(const sh_package_shader_preparation *batch)
{ return batch ? batch->first_need : ""; }
sh_package_native_assets *sh_package_shader_preparation_assets(const sh_package_shader_preparation *batch)
{ return batch ? batch->assets : NULL; }
const sh_package_shader_request *sh_package_shader_preparation_request(const sh_package_shader_preparation *batch, size_t index)
{
    na_shader_request *request = batch ? batch->requests : NULL;
    while (request && index--) request = request->next;
    return request ? &request->value : NULL;
}
void sh_package_shader_preparation_free(sh_package_shader_preparation *batch)
{
    if (!batch) return;
    while (batch->requests) {
        na_shader_request *request = batch->requests; batch->requests = request->next;
        free((void *)request->value.family); free((void *)request->value.name);
        free((void *)request->value.source); free(request);
    }
    sh_package_native_assets_close(batch->assets); free(batch);
}
static int na_request_receipted(sh_package_native_assets *a, const sh_package_shader_request *value,
    char *error, size_t capacity)
{
    sh_native_resource_identity identity = {0}; unsigned char header[0x90];
    size_t bytes = !strcmp(value->family, "renderparm") ? 0x90 : !strcmp(value->family, "renderprog") ? 0x78 : 0x60;
    sh_native_resource_registry *registry = sh_native_resource_registry_open(a->shader_registry, error, capacity);
    int status = registry ? sh_native_resource_registry_identity(registry, value->family, value->name,
        &identity, error, capacity) : SH_NATIVE_RESOURCE_ID_ERROR;
    int ok = (status == SH_NATIVE_RESOURCE_ID_SAME_NAME || (!strcmp(value->family, "renderprog") && status == SH_NATIVE_RESOURCE_ID_FREE)) &&
        identity.id == value->native_id &&
        identity.manager == value->manager && (!value->resource || identity.resource == value->resource) &&
        sh_native_resource_registry_validate(registry, error, capacity);
    if (ok && !strcmp(value->family, "renderprog")) {
        sh_package_shader_capture capture = {0};
        ok = sh_package_shader_cook_copy(a->shader.renderer, value->name, value->source, value->source_length, &capture);
        sh_package_shader_capture_clear(&capture);
    } else if (ok) ok = a->shader_registry.read(a->shader_registry.context, identity.resource, header, bytes) &&
        sh_package_shader_receipt_matches(identity.resource, value->family, value->name,
            value->source, value->source_length, header, bytes);
    sh_native_resource_identity_clear(&identity); sh_native_resource_registry_close(registry); return ok;
}
typedef struct na_shader_collect {
    sh_package_native_assets *assets;
    sh_native_resource_registry *registry;
    na_shader_file *files;
    sh_package_shader_capture *capture;
    const void *declaration; size_t declaration_length;
    unsigned char *binary;
    size_t binary_length, program_count;
    uint64_t ticket;
    char *error; size_t capacity;
    const char *program; /* The requested program while its own paths are emitted. */
    na_names follow;
} na_shader_collect;
static void na_shader_files_free(na_shader_file *files)
{
    while (files) { na_shader_file *next = files->next;
        free(files->path); free(files->body); free(files->program); free(files); files = next; }
}
void sh_package_shader_preparation_revoke(sh_package_shader_preparation *batch)
{
    if (!batch) return;
    AcquireSRWLockExclusive(&batch->assets->lock);
    na_shader_file *files = batch->assets->shader_files; batch->assets->shader_files = NULL;
    ReleaseSRWLockExclusive(&batch->assets->lock); na_shader_files_free(files);
}
static int na_shader_file_add(na_shader_collect *collect, const char *path, const void *body, size_t length,
    const char *program)
{
    for (na_shader_file *file = collect->files; file; file = file->next) if (!strcmp(file->path, path))
        return file->length == length && !memcmp(file->body, body, length);
    na_shader_file *file = calloc(1, sizeof(*file));
    if (file) {
        file->path = _strdup(path); file->body = malloc(length ? length : 1);
        file->program = program ? _strdup(program) : NULL;
    }
    if (!file || !file->path || !file->body || (program && !file->program)) {
        if (file) { free(file->path); free(file->body); free(file->program); free(file); } return 0;
    }
    memcpy(file->body, body, length); file->length = length;
    file->next = collect->files; collect->files = file; return 1;
}
/* Native serialization already selected record widths from this parameter.
 * This snapshot establishes only that decoding schema, not its source/default
 * ownership. The ordinary source linker still requires the full fixed ABI proof. */
static int na_shader_captured_kind(void *context, const char *name, int *kind)
{
    na_shader_collect *collect = context; sh_native_resource_identity identity = {0};
    unsigned char before[0x90], after[0x90]; uintptr_t error = 0; uint32_t index = 0;
    sh_native_resource_registry_source source = collect->assets->shader_registry;
    int ok = sh_native_resource_registry_identity(collect->registry, "renderparm", name,
        &identity, collect->error, collect->capacity) == SH_NATIVE_RESOURCE_ID_SAME_NAME &&
        source.read(source.context, identity.resource, before, sizeof(before));
    if (ok) {
        memcpy(&error, before + 0x18, 8); memcpy(&index, before + 0x68, 4); memcpy(kind, before + 0x6c, 4);
        ok = !error && !(before[0x2c] & 7) && na_shader_index(*kind, index) &&
            source.read(source.context, identity.resource, after, sizeof(after)) && !memcmp(before, after, sizeof(before));
    }
    sh_native_resource_identity_clear(&identity); return ok ? 1 : -1;
}
static int na_shader_collect_program(na_shader_collect *collect, const char *name, const void *expected, size_t length);
static int na_shader_collect_path(void *context, sh_package_shader_path_kind kind,
    const char *path, const char *linked)
{
    na_shader_collect *collect = context; (void)linked;
    if (kind == SH_PACKAGE_SHADER_DECLARATION)
        return na_shader_file_add(collect, path, collect->declaration, collect->declaration_length, NULL);
    if (kind == SH_PACKAGE_SHADER_BINARY)
        return na_shader_file_add(collect, path, collect->binary, collect->binary_length, collect->program);
    if (kind == SH_PACKAGE_SHADER_PERMUTATION) return na_shader_collect_program(collect, path, NULL, 0);
    if (kind != SH_PACKAGE_SHADER_SOURCE_INCLUDE) return 0;
    sh_package_shader_input *input = collect->capture->inputs;
    while (input && strcmp(input->path, path)) input = input->next;
    if (!input) return na_shader_error(collect->error, collect->capacity, "native shader cook did not witness complete include bytes", path);
    unsigned char *original = NULL; size_t original_length = 0;
    int status = collect->assets->shader_original(collect->assets->shader_original_context, path, &original, &original_length);
    int ok = (status == 1 || status == 2) && original && original_length == input->length &&
        !memcmp(original, input->body, input->length);
    free(original);
    if (!ok) return na_shader_error(collect->error, collect->capacity, "native shader include differs from the immutable installed source", path);
    return na_shader_file_add(collect, path, input->body, input->length, NULL);
}
static int na_shader_collect_program(na_shader_collect *collect, const char *name, const void *expected, size_t length)
{
    sh_package_shader_capture capture = {0}; sh_package_shader_program program = {0};
    sh_package_shader_schema schema = {collect, na_shader_captured_kind};
    unsigned char *binary = NULL; int ok = 0;
    if (++collect->program_count > 4096) goto done;
    if (!sh_package_shader_cook_copy(collect->assets->shader.renderer, name, expected, length, &capture) ||
        (collect->ticket && collect->ticket != capture.ticket)) goto done;
    if (!collect->ticket) collect->ticket = capture.ticket;
    /* Includes reached through native inheritance are input too, even when the
     * child's final serialized include list does not repeat its parent's decl.
     * Compare every complete captured input against the immutable catalog. */
    for (sh_package_shader_input *input = capture.inputs; input; input = input->next) {
        unsigned char *original = NULL; size_t original_length = 0;
        int status = collect->assets->shader_original(collect->assets->shader_original_context,
            input->path, &original, &original_length);
        int same = (status == 1 || status == 2) && original && original_length == input->length &&
            !memcmp(original, input->body, input->length);
        free(original);
        if (!same) { na_shader_error(collect->error, collect->capacity,
            "native shader cook input differs from the immutable installed source", input->path); goto done; }
    }
    if (capture.metadata_length > SIZE_MAX - 8 || !(binary = malloc(capture.metadata_length + 8))) goto done;
    const unsigned char magic[] = {0xb6,0x88,0,0xad}; memcpy(binary, magic, 4);
    uint32_t checksum = sh_package_shader_declaration_checksum(capture.source, capture.source_length);
    for (unsigned i = 0; i < 4; i++) binary[7 - i] = (unsigned char)(checksum >> (8 * i));
    memcpy(binary + 8, capture.metadata, capture.metadata_length);
    if (sh_package_shader_decode(binary, capture.metadata_length + 8, &schema, &program,
        collect->error, collect->capacity) != SH_PACKAGE_SHADER_OK) goto done;
    /* Cooking decides which programs this one names (program-typed
     * parameters), and which parameters and flags its binary checks; a
     * compilation reaches them only through this output. */
    for (size_t i = 0; i < program.reference_count; i++) {
        const sh_package_shader_reference *ref = program.references + i;
        if (!ref->name || !*ref->name) continue;
        if (ref->kind == SH_PACKAGE_SHADER_RESOURCE && ref->family && !strcmp(ref->family, "renderprog") &&
            strcmp(ref->name, name)) na_names_add(&collect->follow, "renderprog", ref->name);
        else if (ref->kind == SH_PACKAGE_SHADER_PARAMETER) na_names_add(&collect->follow, "renderparm", ref->name);
        else if (ref->kind == SH_PACKAGE_SHADER_FLAG) na_names_add(&collect->follow, "renderprogflag", ref->name);
    }
    sh_package_shader_capture *saved_capture = collect->capture; unsigned char *saved_binary = collect->binary;
    size_t saved_length = collect->binary_length, saved_declaration_length = collect->declaration_length;
    const void *saved_declaration = collect->declaration; const char *saved_program = collect->program;
    collect->capture = &capture; collect->binary = binary; collect->binary_length = capture.metadata_length + 8;
    collect->declaration = expected ? expected : capture.source;
    collect->declaration_length = expected ? length : capture.source_length;
    collect->program = expected ? name : NULL;
    ok = sh_package_shader_paths(SH_PACKAGE_SHADER_OPENGL, name, name, &program, na_shader_collect_path,
        collect, collect->error, collect->capacity) == SH_PACKAGE_SHADER_OK;
    collect->capture = saved_capture; collect->binary = saved_binary; collect->binary_length = saved_length;
    collect->declaration = saved_declaration; collect->declaration_length = saved_declaration_length;
    collect->program = saved_program;
done:
    if (!ok && collect->error && collect->capacity && !*collect->error)
        na_shader_error(collect->error, collect->capacity, "native source cook lacks a coherent complete source certificate", name);
    free(binary); sh_package_shader_capture_clear(&capture); sh_package_shader_program_clear(&program);
    return ok;
}
static int na_shader_capture_unit(sh_package_native_assets *a, const sh_package_shader_request *request,
    char *error, size_t capacity)
{
    na_shader_collect collect = {0}; collect.assets = a; collect.error = error; collect.capacity = capacity;
    collect.registry = sh_native_resource_registry_open(a->shader_registry, error, capacity);
    int ok = collect.registry && na_shader_collect_program(&collect, request->name, request->source, request->source_length) &&
        sh_native_resource_registry_validate(collect.registry, error, capacity);
    if (ok) {
        AcquireSRWLockExclusive(&a->lock);
        for (na_shader_file *file = collect.files; file && ok; file = file->next)
            for (na_shader_file *old = a->shader_files; old; old = old->next) if (!strcmp(old->path, file->path) &&
                (old->length != file->length || memcmp(old->body, file->body, file->length))) {
                ok = na_shader_error(error, capacity, "shader preparation changed an immutable captured companion", file->path); break;
            }
        if (ok) while (collect.files) {
            na_shader_file *file = collect.files; collect.files = file->next;
            file->next = a->shader_files; a->shader_files = file;
        }
        for (size_t i = 0; ok && i < collect.follow.capacity; i++) if (collect.follow.slots[i]) {
            const char *slot = collect.follow.slots[i], *tab = strchr(slot, '\t');
            char family[32];
            if (!tab || (size_t)(tab - slot) >= sizeof(family)) continue;
            memcpy(family, slot, (size_t)(tab - slot)); family[tab - slot] = 0;
            na_names_add(&a->shader_follow, family, tab + 1);
        }
        ReleaseSRWLockExclusive(&a->lock);
    }
    na_names_free(&collect.follow);
    na_shader_files_free(collect.files); sh_native_resource_registry_close(collect.registry); return ok;
}
int sh_package_shader_preparation_run(sh_package_shader_preparation *batch,
    sh_package_shader_prepare_one prepare, sh_package_shader_recover recover,
    void *context, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!batch || batch->ran || !prepare || !recover) return -1;
    batch->ran = 1;
    for (na_shader_request *request = batch->requests; request; request = request->next) {
        const sh_package_shader_request *value = &request->value;
        sh_native_resource_identity identity = {0};
        sh_native_resource_registry *registry = sh_native_resource_registry_open(batch->assets->shader_registry, error, capacity);
        int status = registry ? sh_native_resource_registry_identity(registry, value->family, value->name,
            &identity, error, capacity) : SH_NATIVE_RESOURCE_ID_ERROR;
        int valid = (status == SH_NATIVE_RESOURCE_ID_FREE || status == SH_NATIVE_RESOURCE_ID_SAME_NAME) &&
            identity.manager == value->manager && identity.id == value->native_id &&
            (!value->resource || identity.resource == value->resource) &&
            sh_native_resource_registry_validate(registry, error, capacity);
        sh_native_resource_identity_clear(&identity); sh_native_resource_registry_close(registry);
        if (!valid || prepare(context, value, error, capacity) != 1 ||
            !na_request_receipted(batch->assets, value, error, capacity)) goto failed;
        if (!strcmp(value->family, "renderprog")) {
            if (!na_shader_capture_unit(batch->assets, value, error, capacity)) goto failed;
            continue;
        }
        int kind; uint32_t index; uint64_t mask;
        int parameter = !strcmp(value->family, "renderparm");
        if (!na_shader_fixed(batch->assets, value->family, value->name,
            (sh_decl_source){(const char *)value->source, value->source_length},
            parameter ? &kind : NULL, parameter ? &index : NULL, parameter ? NULL : &mask, 0, error, capacity)) goto failed;
    }
    AcquireSRWLockExclusive(&batch->assets->lock);
    for (na_shader_request *request = batch->requests; request; request = request->next)
        na_names_add(&batch->assets->shader_prepared, request->value.family, request->value.name);
    ReleaseSRWLockExclusive(&batch->assets->lock);
    return 1;
failed:
    {
        char primary[1024], recovery[1024] = "";
        snprintf(primary, sizeof(primary), "%s", error && capacity && *error ? error : "native shader preparation did not produce verified input");
        int restored = recover(context, batch, recovery, sizeof(recovery)) == 1;
        if (error && capacity) snprintf(error, capacity, "%s; %s%s%s", primary,
            restored ? "prior provider recovered" : "prior provider recovery failed",
            *recovery ? ": " : "", recovery);
        return restored ? 0 : -1;
    }
}
int sh_package_native_assets_configure_shader(sh_package_native_assets *a,
    sh_package_shader_renderer renderer, sh_native_resource_registry_source registry,
    sh_package_baseline_reader original, void *original_context, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!a || (renderer != SH_PACKAGE_SHADER_VULKAN && renderer != SH_PACKAGE_SHADER_OPENGL) ||
        !registry.read || !registry.head_slot || !original)
        return na_shader_error(error, capacity, "invalid shader native configuration", "");
    AcquireSRWLockExclusive(&a->lock);
    int same = !a->shader_configured || (a->shader.renderer == renderer && a->shader_registry.context == registry.context &&
        a->shader_registry.read == registry.read && a->shader_registry.head_slot == registry.head_slot &&
        a->shader_original == original && a->shader_original_context == original_context);
    if (same && !a->shader_configured) {
        a->shader = (sh_package_source_shader){renderer, a, na_shader_parameter, na_shader_flag, na_shader_program};
        a->shader_registry = registry; a->shader_original = original; a->shader_original_context = original_context;
        a->shader_configured = 1;
    }
    ReleaseSRWLockExclusive(&a->lock);
    return same ? 1 : na_shader_error(error, capacity, "shader native configuration cannot change on a shared handle", "");
}
int sh_package_native_assets_configure_shader_catalog(sh_package_native_assets *a,
    sh_package_shader_renderer renderer, sh_native_resource_registry_source registry,
    const char *doom_base, char *error, size_t capacity)
{
    if (!a || a->shader_configured || !doom_base || !*doom_base)
        return na_shader_error(error, capacity, "shader catalog configuration requires a fresh asset handle", "");
    sh_resource_catalog *catalog = sh_resource_catalog_open(doom_base, error, capacity);
    if (!catalog) return 0;
    if (!sh_package_native_assets_configure_shader(a, renderer, registry, sh_resource_catalog_read_path,
        catalog, error, capacity)) { sh_resource_catalog_close(catalog); return 0; }
    a->shader_catalog = catalog; return 1;
}
int sh_package_native_assets_configure_shader_catalog_shared(sh_package_native_assets *a,
    sh_package_shader_renderer renderer, sh_native_resource_registry_source registry,
    sh_resource_catalog *catalog, char *error, size_t capacity)
{
    if (!a || a->shader_configured || !catalog)
        return na_shader_error(error, capacity, "shader catalog configuration requires a fresh asset handle", "");
    sh_resource_catalog *held = sh_resource_catalog_retain(catalog);
    if (!sh_package_native_assets_configure_shader(a, renderer, registry, sh_resource_catalog_read_path,
        held, error, capacity)) { sh_resource_catalog_close(held); return 0; }
    a->shader_catalog = held; return 1;
}
static int na_atlas(void *context, size_t scope, const sh_package_compilation *compiled,
    const char *material, const char *page_file, sh_package_virtual_atlas *out, char *error, size_t capacity)
{
    sh_package_native_assets *a = context;
    (void)scope;
    if (!a || !compiled || !material || !page_file || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->name = "_vmtr"; out->source = "virtualtextures/_vmtr"; out->original = 1;
    /* Generic materials bypass VMTR lookup, including page-file selection. */
    if (!strncmp(material, "generic", 7)) return 1;
    if (*page_file && _stricmp(page_file, "_vmtr")) {
        /* Any other source names the engine's own provider for
         * virtualtextures/<source>.pages (VirtualTextureForMaterial VK
         * 140e1ead0 / GL 140e109f0 format "%s/%s.pages", or "_v%03d.pages"),
         * which Material Parse selects by that name (VK 140d76bb0 / GL
         * 140d76df0): a palette's megatexture, maps/modules/palettes/<p>/mega.
         * While no scope supplies a file of that family, every binding selects
         * that one external object, like _vmtr (na_pages): its nominal identity
         * certifies it, without reading its gigabytes of pages. Authored page
         * data still needs a private provider of its own. */
        static __declspec(thread) char source[1024];
        char family[1024];
        int written = snprintf(family, sizeof(family), "virtualtextures/%s", page_file);
        if (written < 0 || (size_t)written >= sizeof(family)) {
            if (error && capacity) snprintf(error, capacity, "native page source name is too long: %s", page_file);
            return -1;
        }
        size_t n = (size_t)written;
        for (char *p = family; *p; p++) if (*p == '\\') *p = '/';
        memcpy(source, family, n + 1);
        for (size_t i = 0; i < compiled->resource_count; i++) {
            const char *path = compiled->resources[i].engine_path;
            if (path && !_strnicmp(path, family, n) && (path[n] == '.' || path[n] == '_')) {
                if (error && capacity) snprintf(error, capacity, "private native page provider requires preparation: %s", page_file);
                return 0;
            }
        }
        char nominal[1100];
        sh_package_file_identity identity;
        snprintf(nominal, sizeof(nominal), "doom2016:engine-owned-page-provider:%s", page_file);
        if (!sh_package_bytes_identity(nominal, strlen(nominal), &identity)) return -1;
        out->name = page_file; out->source = source; memcpy(out->content, identity.digest, 32);
        return 1;
    }
    AcquireSRWLockExclusive(&a->lock);
    int result = na_pages(a);
    if (result == 1) result = na_original_pages(a, compiled);
    if (!result) {
        /* Changed page data: the scope's complete family, served by a private
         * kind-2 provider in the engine's unique-VMTR page mode. */
        char reason[1024] = "";
        out->pages = na_private_pages(a, compiled, reason, sizeof(reason));
        if (!out->pages) {
            na_error(a, *reason ? reason : "cannot capture private virtual texture page data", NULL); result = -1;
        } else if (na_page_refused(a, out->pages)) {
            /* A refused family serves nothing: the engine's own provider and pages. */
            sh_virtual_page_source_release(out->pages); out->pages = NULL;
            memcpy(out->content, a->pages, 32); result = 1;
        } else {
            out->original = 0; out->page_kind = 2; out->page_mode = 2;
            out->row_complete = na_page_row; out->row_context = a;
            memcpy(out->content, sh_virtual_page_source_identity(out->pages), 32); result = 1;
        }
    } else if (result == 1) memcpy(out->content, a->pages, 32);
    if (result < 0 && error && capacity) snprintf(error, capacity, "%s", a->error);
    ReleaseSRWLockExclusive(&a->lock);
    return result;
}
static int na_implicit(void *context, size_t scope, int observing, const sh_package_link_unit *unit,
    int (*visit)(void *, const char *, const char *, const char *), void *visit_context,
    char *error, size_t capacity)
{
    sh_package_native_assets *a = context;
    (void)scope; (void)visit; (void)visit_context;
    /* Source capture alone cannot enable a family whose native constructor
     * still derives unbound names. Keep production admission closed until its
     * consumer adapter is installed; offline linking can inspect the graph. */
    if (!observing && !strcmp(unit->family, "font") &&
        (!a || !a->consumers_ready || !a->consumers_ready(unit->family))) {
        if (error && capacity) snprintf(error, capacity, "font native consumer adapter is not configured");
        return 0;
    }
    /* A UI movie resolves its fonts and fixed GUI materials inside its native
     * constructor; bound variants of those requests need the movie route. */
    if (!observing && sh_package_movie_owner(unit->family, unit->name) &&
        (!a || !a->consumers_ready || !a->consumers_ready(unit->family))) {
        if (error && capacity) snprintf(error, capacity, "UI movie native consumer adapter is not configured");
        return 0;
    }
    return 1;
}
int sh_package_native_assets_configure_consumers(sh_package_native_assets *a, int (*ready)(const char *))
{
    if (!a || !ready) return 0;
    AcquireSRWLockExclusive(&a->lock);
    int same = !a->consumers_ready || a->consumers_ready == ready;
    if (!a->consumers_ready) a->consumers_ready = ready;
    ReleaseSRWLockExclusive(&a->lock); return same;
}
sh_package_library_native sh_package_native_assets_adapter(sh_package_native_assets *a)
{
    sh_package_library_native out = {{a, na_admit, na_semantics,
        a && a->shader_configured ? &a->shader : NULL}, na_atlas};
    out.source.implicit = na_implicit; return out;
}
