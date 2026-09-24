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
typedef struct na_shader_file {
    struct na_shader_file *next;
    char *path;
    unsigned char *body;
    size_t length;
} na_shader_file;
struct sh_package_shader_preparation {
    sh_package_native_assets *assets;
    na_shader_request *requests;
    size_t count;
    int ran;
};
struct sh_package_native_assets {
    char *root;
    volatile LONG references;
    SRWLOCK lock;
    na_file *files;
    size_t count, capacity;
    unsigned char pages[32];
    int page_state;
    sh_package_source_shader shader;
    sh_native_resource_registry_source shader_registry;
    sh_package_baseline_reader shader_original;
    void *shader_original_context;
    na_shader_original *shader_originals;
    sh_resource_catalog *shader_catalog;
    int shader_configured;
    int (*consumers_ready)(const char *family);
    na_shader_request *shader_requests;
    size_t shader_request_count;
    uint64_t next_shader_task;
    na_shader_file *shader_files;
    /* One compilation's shader ABI checks share this identity snapshot. Each
     * check revalidates what it read; leaving the scope revalidates it all. */
    sh_native_resource_registry *shader_snapshot;
    int shader_scope, shader_scope_changed;
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
    while (a->shader_originals) {
        na_shader_original *entry = a->shader_originals; a->shader_originals = entry->next;
        free(entry->path); free(entry->body); free(entry);
    }
    while (a->shader_requests) {
        na_shader_request *request = a->shader_requests; a->shader_requests = request->next;
        free((void *)request->value.family); free((void *)request->value.name);
        free((void *)request->value.source); free(request);
    }
    while (a->shader_files) { na_shader_file *file = a->shader_files; a->shader_files = file->next;
        free(file->path); free(file->body); free(file); }
    sh_native_resource_registry_close(a->shader_snapshot);
    sh_resource_catalog_close(a->shader_catalog);
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
    sh_package_native_assets *a = context; int result = 0;
    *body = NULL; *length = 0;
    if (!a || !path) return 0;
    AcquireSRWLockShared(&a->lock);
    for (na_shader_file *file = a->shader_files; file; file = file->next) if (!strcmp(file->path, path)) {
        *body = malloc(file->length + 1);
        if (*body) { memcpy(*body, file->body, file->length); (*body)[file->length] = 0; *length = file->length; result = 1; }
        else result = -1;
        break;
    }
    ReleaseSRWLockShared(&a->lock); return result;
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
static na_shader_original *na_shader_source(sh_package_native_assets *a, const char *family,
    const char *name, char *error, size_t capacity)
{
    char path[512]; unsigned char *body = NULL; size_t length = 0;
    int n = snprintf(path, sizeof(path), "generated/decls/%s/%s.decl", family, name);
    if (n < 0 || n >= (int)sizeof(path)) return NULL;
    char *canonical = sh_package_engine_path(path);
    if (!canonical || strcmp(path, canonical)) { free(canonical); return NULL; }
    for (na_shader_original *entry = a->shader_originals; entry; entry = entry->next)
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
    entry->next = a->shader_originals; a->shader_originals = entry; return entry;
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
            na_shader_request_add(a, family, name, source, &identity, original))
            na_shader_error(error, capacity, "shader environment needs verified native source preparation", name);
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
            na_shader_request_add(a, family, name, source, &identity, original))
            na_shader_error(error, capacity, "shader environment needs verified native source preparation", name);
        else na_shader_error(error, capacity, "native shader ABI needs exact retained text or a successful source receipt", name);
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
done:
    sh_native_resource_identity_clear(&identity); sh_native_resource_registry_close(registry);
    ReleaseSRWLockExclusive(&a->lock); return 0;
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
    a->shader_requests = NULL; a->shader_request_count = 0;
    ReleaseSRWLockExclusive(&a->lock);
    if (!batch->count) { free(batch); return NULL; }
    batch->assets = sh_package_native_assets_retain(a); return batch;
}
size_t sh_package_shader_preparation_count(const sh_package_shader_preparation *batch)
{ return batch ? batch->count : 0; }
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
} na_shader_collect;
static void na_shader_files_free(na_shader_file *files)
{
    while (files) { na_shader_file *next = files->next;
        free(files->path); free(files->body); free(files); files = next; }
}
void sh_package_shader_preparation_revoke(sh_package_shader_preparation *batch)
{
    if (!batch) return;
    AcquireSRWLockExclusive(&batch->assets->lock);
    na_shader_file *files = batch->assets->shader_files; batch->assets->shader_files = NULL;
    ReleaseSRWLockExclusive(&batch->assets->lock); na_shader_files_free(files);
}
static int na_shader_file_add(na_shader_collect *collect, const char *path, const void *body, size_t length)
{
    for (na_shader_file *file = collect->files; file; file = file->next) if (!strcmp(file->path, path))
        return file->length == length && !memcmp(file->body, body, length);
    na_shader_file *file = calloc(1, sizeof(*file));
    if (file) { file->path = _strdup(path); file->body = malloc(length ? length : 1); }
    if (!file || !file->path || !file->body) {
        if (file) { free(file->path); free(file->body); free(file); } return 0;
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
        return na_shader_file_add(collect, path, collect->declaration, collect->declaration_length);
    if (kind == SH_PACKAGE_SHADER_BINARY)
        return na_shader_file_add(collect, path, collect->binary, collect->binary_length);
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
    return na_shader_file_add(collect, path, input->body, input->length);
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
    sh_package_shader_capture *saved_capture = collect->capture; unsigned char *saved_binary = collect->binary;
    size_t saved_length = collect->binary_length, saved_declaration_length = collect->declaration_length;
    const void *saved_declaration = collect->declaration;
    collect->capture = &capture; collect->binary = binary; collect->binary_length = capture.metadata_length + 8;
    collect->declaration = expected ? expected : capture.source;
    collect->declaration_length = expected ? length : capture.source_length;
    ok = sh_package_shader_paths(SH_PACKAGE_SHADER_OPENGL, name, name, &program, na_shader_collect_path,
        collect, collect->error, collect->capacity) == SH_PACKAGE_SHADER_OK;
    collect->capture = saved_capture; collect->binary = saved_binary; collect->binary_length = saved_length;
    collect->declaration = saved_declaration; collect->declaration_length = saved_declaration_length;
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
        ReleaseSRWLockExclusive(&a->lock);
    }
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
        if (error && capacity) snprintf(error, capacity, "private native page provider requires preparation: %s", page_file);
        return 0;
    }
    AcquireSRWLockExclusive(&a->lock);
    int result = na_pages(a);
    if (result == 1) result = na_original_pages(a, compiled);
    if (result < 0) {
        if (error && capacity) snprintf(error, capacity, "%s", a->error);
    } else if (!result) {
        if (error && capacity) snprintf(error, capacity, "scoped virtual texture page data requires a private provider");
    } else memcpy(out->content, a->pages, 32);
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
