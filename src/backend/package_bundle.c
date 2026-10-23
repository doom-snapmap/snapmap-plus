#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_bundle.h"

#pragma comment(lib, "bcrypt.lib")

struct sh_package_bundle {
    HANDLE archive;
    size_t length;
    unsigned char fingerprint[32];
    char legacy_seed[SH_PACKAGE_ID_CAP];
    volatile LONG references;
    SRWLOCK read_lock;
};

static int pb_error(char *error, size_t capacity, const char *reason)
{
    if (error && capacity) snprintf(error, capacity, "%s", reason);
    return 0;
}
static int pb_errorf(char *error, size_t capacity, const char *format, ...)
{
    va_list arguments;
    if (!error || !capacity) return 0;
    va_start(arguments, format);
    _vsnprintf_s(error, capacity, _TRUNCATE, format, arguments);
    va_end(arguments);
    return 0;
}
static wchar_t *pb_directory(const char *root)
{
    wchar_t *wide = sh_package_source_wide_path(root), *grown;
    size_t length;
    if (!wide) return NULL;
    /* Match the source/cache path boundary: no redirected ancestor, including
     * the configured root, can redirect retained authored bytes elsewhere. */
    for (size_t i = 7; ; i++) if (!wide[i] || wide[i] == L'\\') {
        wchar_t saved = wide[i]; DWORD attributes;
        wide[i] = 0; attributes = GetFileAttributesW(wide); wide[i] = saved;
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { free(wide); return NULL; }
        if (!saved) break;
    }
    length = wcslen(wide);
    if (length > SIZE_MAX / sizeof(*wide) - 24u ||
        !(grown = realloc(wide, (length + 24u) * sizeof(*wide)))) { free(wide); return NULL; }
    wide = grown;
    for (size_t i = 0; i < 2; i++) {
        DWORD attributes;
        wcscat_s(wide, length + 24u, i ? L"\\bundles" : L"\\package-cache");
        if (!CreateDirectoryW(wide, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { free(wide); return NULL; }
        attributes = GetFileAttributesW(wide);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { free(wide); return NULL; }
    }
    return wide;
}

static sh_package_bundle *pb_store(const unsigned char *bytes, size_t length,
    const unsigned char fingerprint[32], const char *root, char *error, size_t capacity)
{
    sh_package_bundle *bundle = NULL;
    wchar_t *directory = NULL, *path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    size_t offset = 0, path_capacity;
    const char *reason = "cannot create complete package snapshot cache";
    if (!root || !(directory = pb_directory(root))) goto done;
    path_capacity = wcslen(directory) + 40u;
    if (path_capacity > SIZE_MAX / sizeof(*path) || !(path = malloc(path_capacity * sizeof(*path)))) goto done;
    for (size_t attempt = 0; attempt < 16; attempt++) {
        unsigned char random[16]; wchar_t token[33];
        if (BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) goto done;
        for (size_t i = 0; i < 16; i++) swprintf_s(token + i * 2u, 3, L"%02x", random[i]);
        swprintf_s(path, path_capacity, L"%ls\\%ls.zip", directory, token);
        /* This private handle is never exposed or written after capture.
         * Sharing denies external writes/deletion; delete-on-close gives each
         * snapshot bounded disk lifetime, including process termination. */
        file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
            NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE |
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) goto done;
    }
    if (file == INVALID_HANDLE_VALUE) goto done;
    reason = "cannot write complete package snapshot";
    while (offset < length) {
        DWORD wrote = 0, amount = length - offset > 1048576u ? 1048576u : (DWORD)(length - offset);
        if (!WriteFile(file, bytes + offset, amount, &wrote, NULL) || wrote != amount) goto done;
        offset += amount;
    }
    if (!FlushFileBuffers(file)) goto done;
    bundle = calloc(1, sizeof(*bundle));
    if (!bundle) { reason = "cannot retain complete package snapshot"; goto done; }
    bundle->archive = file; file = INVALID_HANDLE_VALUE;
    bundle->length = length; bundle->references = 1;
    memcpy(bundle->fingerprint, fingerprint, 32); InitializeSRWLock(&bundle->read_lock);
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(path); free(directory);
    if (!bundle) pb_error(error, capacity, reason);
    return bundle;
}

sh_package_bundle *sh_package_bundle_from_archive(const unsigned char *bytes,
    size_t length, const char *root, char *error, size_t capacity)
{
    unsigned char fingerprint[32];
    if (error && capacity) error[0] = 0;
    if (!sh_package_archive_tree_identity(bytes, length, fingerprint, error, capacity)) return NULL;
    return pb_store(bytes, length, fingerprint, root, error, capacity);
}

sh_package_bundle *sh_package_bundle_from_legacy_archive(const unsigned char *bytes,
    size_t length, const char *seed, const char *root, char *error, size_t capacity)
{
    char canonical[SH_PACKAGE_ID_CAP];
    sh_package_bundle *bundle;
    if (!seed || !sh_package_id_normalize(seed, canonical) || strcmp(seed, canonical)) {
        pb_error(error, capacity, "legacy source snapshot has no canonical migration seed"); return NULL;
    }
    bundle = sh_package_bundle_from_archive(bytes, length, root, error, capacity);
    if (bundle) strcpy_s(bundle->legacy_seed, sizeof(bundle->legacy_seed), seed);
    return bundle;
}

static int pb_hex(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

int sh_package_bundle_parse_key(const char *key, unsigned char fingerprint[32],
    char seed[SH_PACKAGE_ID_CAP])
{
    unsigned char tree[32];
    char decoded[SH_PACKAGE_ID_CAP] = "", canonical[SH_PACKAGE_ID_CAP];
    size_t prefix, length;
    int legacy;
    if (!key) return 0;
    legacy = !strncmp(key, "delivery.legacy.", 16);
    prefix = legacy ? 16u : 9u; length = strlen(key);
    if ((!legacy && strncmp(key, "delivery.", 9)) || length < prefix + 64u) return 0;
    for (size_t i = 0; i < 32; i++) {
        int a = pb_hex(key[prefix + i * 2u]), b = pb_hex(key[prefix + i * 2u + 1u]);
        if (a < 0 || b < 0) return 0;
        tree[i] = (unsigned char)(a * 16 + b);
    }
    if (legacy) {
        size_t bytes = length - prefix - 64u;
        if (bytes < 3u || key[prefix + 64u] != '.' || !(bytes & 1u) ||
            bytes / 2u >= sizeof(decoded)) return 0;
        for (size_t i = 0; i < bytes / 2u; i++) {
            int a = pb_hex(key[prefix + 65u + i * 2u]), b = pb_hex(key[prefix + 66u + i * 2u]);
            if (a < 0 || b < 0 || (!a && !b)) return 0;
            decoded[i] = (char)(a * 16 + b);
        }
        if (!sh_package_id_normalize(decoded, canonical) || strcmp(decoded, canonical)) return 0;
    } else if (length != prefix + 64u) return 0;
    if (fingerprint) memcpy(fingerprint, tree, 32);
    if (seed) strcpy_s(seed, SH_PACKAGE_ID_CAP, decoded);
    return 1;
}

void sh_package_bundle_delivery_key(const sh_package_bundle *bundle,
    char out[SH_PACKAGE_BUNDLE_KEY_CAP])
{
    size_t at = 16u;
    if (!out) return;
    out[0] = 0; if (!bundle) return;
    if (!bundle->legacy_seed[0]) { sh_package_archive_delivery_id(bundle->fingerprint, out); return; }
    memcpy(out, "delivery.legacy.", at);
    for (size_t i = 0; i < 32; i++, at += 2u) snprintf(out + at, 3, "%02x", bundle->fingerprint[i]);
    out[at++] = '.';
    for (size_t i = 0; bundle->legacy_seed[i]; i++, at += 2u)
        snprintf(out + at, 3, "%02x", (unsigned char)bundle->legacy_seed[i]);
}

sh_package_bundle *sh_package_bundle_capture(const sh_package_sources *sources,
    size_t owner, const char *root, char *error, size_t capacity)
{
    sh_package_bundle *bundle = NULL;
    unsigned char *bytes, fingerprint[32]; size_t length;
    if (error && capacity) error[0] = 0;
    if (!sources || owner >= sources->package_count || !sources->fingerprints) {
        pb_error(error, capacity, "complete package snapshot source is unavailable"); return NULL;
    }
    bytes = sh_package_archive_pack(sources, owner, &length, error, capacity);
    if (!bytes) return NULL;
    if (sh_package_archive_tree_identity(bytes, length, fingerprint, error, capacity)) {
        if (!memcmp(fingerprint, sources->fingerprints[owner], 32))
            bundle = pb_store(bytes, length, fingerprint, root, error, capacity);
        else pb_error(error, capacity, "complete package snapshot does not match its source inventory");
    }
    free(bytes); return bundle;
}

sh_package_bundle *sh_package_bundle_retain(sh_package_bundle *bundle)
{ if (bundle) InterlockedIncrement(&bundle->references); return bundle; }
void sh_package_bundle_release(sh_package_bundle *bundle)
{
    if (bundle && !InterlockedDecrement(&bundle->references)) {
        CloseHandle(bundle->archive); free(bundle);
    }
}
static char g_bundle_cache_root[MAX_PATH];
void sh_package_bundle_set_cache_root(const char *root)
{
    if (!root || !root[0]) { g_bundle_cache_root[0] = 0; return; }
    strncpy_s(g_bundle_cache_root, sizeof(g_bundle_cache_root), root, _TRUNCATE);
}
const char *sh_package_bundle_cache_root(void)
{ return g_bundle_cache_root[0] ? g_bundle_cache_root : NULL; }

/* Append one JSON string literal at *at; 0 when it does not fit. */
static int pb_json_string(char *out, size_t cap, size_t *at, const char *text)
{
    size_t i = *at;
    if (i + 1 >= cap) return 0;
    out[i++] = '"';
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        char escape[8];
        size_t n = 1;
        if (c == '"' || c == '\\') { escape[0] = '\\'; escape[1] = (char)c; n = 2; }
        else if (c < 0x20) { snprintf(escape, sizeof escape, "\\u%04x", c); n = 6; }
        else escape[0] = (char)c;
        if (i + n + 1 >= cap) return 0;
        memcpy(out + i, escape, n); i += n;
    }
    out[i++] = '"'; out[i] = 0; *at = i;
    return 1;
}

static int pb_inside(const char *path, const char *component, size_t prefix)
{ return !strncmp(path, component, prefix) && path[prefix] == '/' && path[prefix + 1]; }

sh_package_bundle *sh_package_bundle_component(sh_package_bundle *outer, const char *component,
    const char *const *enclosing, size_t enclosing_count, const char *root,
    char *error, size_t capacity)
{
    sh_package_archive_files files = {0}, delivery = {0};
    sh_package_descriptor descriptor = {0};
    sh_package_bundle *result = NULL;
    unsigned char *bytes = NULL, *archive = NULL;
    char *metadata = NULL;
    size_t length = 0, archive_length = 0, prefix, i, e, kept = 0, component_count = 0, metadata_cap, at = 0;
    size_t outer_descriptor = SIZE_MAX;
    int parsed = 0, component_descriptor = 0;
    if (error && capacity) error[0] = 0;
    if (!outer || !component || !component[0] || component[strlen(component) - 1] == '/' ||
        (enclosing_count && !enclosing)) {
        pb_error(error, capacity, "a component delivery needs the component's path"); return NULL;
    }
    prefix = strlen(component);
    bytes = sh_package_bundle_read(outer, &length, error, capacity);
    if (!bytes || !sh_package_archive_read(bytes, length, &files, error, capacity)) goto done;
    if (files.count > SIZE_MAX / sizeof(*delivery.items) - enclosing_count - 1 ||
        !(delivery.items = calloc(files.count + enclosing_count + 1, sizeof(*delivery.items)))) {
        pb_error(error, capacity, "cannot allocate the component delivery"); goto done;
    }
    for (i = 0; i < files.count; i++) {
        const sh_package_archive_file *file = files.items + i;
        if (!file->directory && !strcmp(file->name, "package.json")) outer_descriptor = i;
        if (!pb_inside(file->name, component, prefix)) continue;
        if (!(delivery.items[kept].name = _strdup(file->name + prefix + 1))) {
            pb_error(error, capacity, "cannot allocate the component delivery"); goto done;
        }
        delivery.items[kept].body = file->body; delivery.items[kept].length = file->length;
        delivery.items[kept].directory = file->directory;
        if (!file->directory && !strcmp(delivery.items[kept].name, "package.json")) component_descriptor = 1;
        if (!file->directory && !strcmp(delivery.items[kept].name, SH_PACKAGE_DELIVERY_METADATA)) {
            pb_error(error, capacity, "the component already carries a delivery record"); goto done;
        }
        kept++;
    }
    if (outer_descriptor == SIZE_MAX) { pb_error(error, capacity, "the enclosing delivery has no root package.json"); goto done; }
    if (!component_descriptor) { pb_error(error, capacity, "the path is not a component: it has no package.json"); goto done; }
    parsed = sh_package_descriptor_parse((const char *)files.items[outer_descriptor].body,
        files.items[outer_descriptor].length, &descriptor, error, capacity);
    if (!parsed) goto done;
    component_count = kept;
    for (e = 0; e < enclosing_count; e++) {
        const char *path = enclosing[e];
        size_t found = SIZE_MAX, d;
        if (!path || !path[0]) { pb_error(error, capacity, "an enclosing file has no path"); goto done; }
        if (pb_inside(path, component, prefix) || !strcmp(path, component)) {
            pb_errorf(error, capacity, "an enclosing file lies inside the component: %s", path); goto done;
        }
        for (d = 0; d < kept; d++) if (!strcmp(delivery.items[d].name, path)) break;
        if (d < component_count) {
            pb_errorf(error, capacity, "an enclosing file collides with a component member: %s", path); goto done;
        }
        if (d < kept) continue; /* the same enclosing file named twice */
        for (i = 0; i < files.count; i++)
            if (!files.items[i].directory && !strcmp(files.items[i].name, path)) { found = i; break; }
        if (found == SIZE_MAX) { pb_errorf(error, capacity, "an enclosing file is not in the delivery: %s", path); goto done; }
        if (!(delivery.items[kept].name = _strdup(path))) { pb_error(error, capacity, "cannot allocate the component delivery"); goto done; }
        delivery.items[kept].body = files.items[found].body; delivery.items[kept].length = files.items[found].length;
        delivery.items[kept].directory = 0;
        kept++;
    }
    /* The record: who authored the enclosing files, and which they are. */
    metadata_cap = 160 + strlen(descriptor.id) * 2 + strlen(descriptor.name) * 2 + prefix * 2;
    for (e = 0; e < enclosing_count; e++) metadata_cap += strlen(enclosing[e]) * 2 + 4;
    if (!(metadata = malloc(metadata_cap))) { pb_error(error, capacity, "cannot allocate the component delivery"); goto done; }
    at = (size_t)snprintf(metadata, metadata_cap, "{\"enclosing\":{\"id\":");
    if (!pb_json_string(metadata, metadata_cap, &at, descriptor.id)) goto record_failed;
    at += (size_t)snprintf(metadata + at, metadata_cap - at, ",\"name\":");
    if (!pb_json_string(metadata, metadata_cap, &at, descriptor.name)) goto record_failed;
    at += (size_t)snprintf(metadata + at, metadata_cap - at, "},\"component\":");
    if (!pb_json_string(metadata, metadata_cap, &at, component)) goto record_failed;
    at += (size_t)snprintf(metadata + at, metadata_cap - at, ",\"files\":[");
    for (e = 0, i = 0; e < enclosing_count; e++) {
        size_t d;
        for (d = 0; d < e; d++) if (!strcmp(enclosing[d], enclosing[e])) break;
        if (d < e) continue;
        if (i++ && at + 1 < metadata_cap) metadata[at++] = ',';
        if (!pb_json_string(metadata, metadata_cap, &at, enclosing[e])) goto record_failed;
    }
    if (at + 3 >= metadata_cap) goto record_failed;
    memcpy(metadata + at, "]}", 3); at += 2;
    delivery.items[kept].name = _strdup(SH_PACKAGE_DELIVERY_METADATA);
    if (!delivery.items[kept].name) { pb_error(error, capacity, "cannot allocate the component delivery"); goto done; }
    delivery.items[kept].body = (unsigned char *)metadata; delivery.items[kept].length = at;
    delivery.items[kept].directory = 0; kept++;
    delivery.count = kept;
    archive = sh_package_archive_write(&delivery, &archive_length, error, capacity);
    if (!archive) goto done;
    result = sh_package_bundle_from_archive(archive, archive_length, root, error, capacity);
    goto done;
record_failed:
    pb_error(error, capacity, "the component delivery record does not fit");
done:
    if (delivery.items) for (i = 0; i < kept; i++) free(delivery.items[i].name);
    free(delivery.items); free(metadata); free(archive);
    if (parsed) sh_package_descriptor_free(&descriptor);
    sh_package_archive_files_free(&files); free(bytes);
    return result;
}

const unsigned char *sh_package_bundle_fingerprint(const sh_package_bundle *bundle)
{ return bundle ? bundle->fingerprint : NULL; }
unsigned char *sh_package_bundle_read(sh_package_bundle *bundle, size_t *length,
    char *error, size_t capacity)
{
    unsigned char *bytes = NULL;
    size_t offset = 0;
    LARGE_INTEGER beginning;
    int ok = 0;
    if (length) *length = 0;
    if (error && capacity) error[0] = 0;
    if (!bundle || !length || bundle->length == SIZE_MAX || !(bytes = malloc(bundle->length + 1u))) goto done;
    beginning.QuadPart = 0;
    AcquireSRWLockExclusive(&bundle->read_lock);
    if (SetFilePointerEx(bundle->archive, beginning, NULL, FILE_BEGIN)) {
        while (offset < bundle->length) {
            DWORD got = 0, amount = bundle->length - offset > 1048576u ? 1048576u : (DWORD)(bundle->length - offset);
            if (!ReadFile(bundle->archive, bytes + offset, amount, &got, NULL) || got != amount) break;
            offset += amount;
        }
        ok = offset == bundle->length;
    }
    ReleaseSRWLockExclusive(&bundle->read_lock);
done:
    if (!ok) { free(bytes); pb_error(error, capacity, "cannot read retained complete package snapshot"); return NULL; }
    bytes[bundle->length] = 0; *length = bundle->length; return bytes;
}
