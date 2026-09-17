#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "resource_catalog.h"
#include "raw_deflate.h"

#define RC_INDEX_LIMIT (64u * 1024u * 1024u)
#define RC_ROW_LIMIT 1000000u
#define RC_FIELD_LIMIT 4096u
#define RC_RESOURCE_LIMIT ((size_t)INT_MAX)

struct sh_resource_catalog {
    sh_resource_catalog_entry *rows;
    size_t count, capacity;
    sh_resource_catalog_entry **paths, **identities, **legacy_paths;
    HANDLE archives[4];
    uint64_t archive_sizes[4];
    SRWLOCK archive_lock;
};

static uint32_t rc_be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t rc_le32(const unsigned char *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static char *rc_field(const unsigned char *bytes, size_t length, size_t *position)
{
    size_t n, i;
    char *out;
    if (*position > length || length - *position < 4u) return NULL;
    n = rc_le32(bytes + *position); *position += 4;
    if (n >= RC_FIELD_LIMIT || n > length - *position) return NULL;
    out = (char *)malloc(n + 1u);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = bytes[*position + i];
        if (c < 0x20 || c > 0x7e) { free(out); return NULL; }
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '\\') c = '/';
        out[i] = (char)c;
    }
    out[n] = 0; *position += n; return out;
}

static int rc_parse(sh_resource_catalog *catalog, const unsigned char *bytes, size_t length, unsigned archive)
{
    size_t count, position = 0x24u, i;
    sh_resource_catalog_entry *grown;
    if (length < position || memcmp(bytes, "\x05SER", 4) || rc_be32(bytes + 4) != length - 0x20u) return 0;
    count = rc_be32(bytes + 0x20u);
    if (count > RC_ROW_LIMIT - catalog->count || count > (length - position) / 37u) return 0;
    grown = (sh_resource_catalog_entry *)realloc(catalog->rows, (catalog->count + count + 1u) * sizeof(*grown));
    if (!grown) return 0;
    catalog->rows = grown; catalog->capacity = catalog->count + count;
    for (i = 0; i < count; i++) {
        sh_resource_catalog_entry *row = &catalog->rows[catalog->count++];
        memset(row, 0, sizeof(*row));
        if (position > length || length - position < 4u) return 0;
        position += 4u; /* Row ordinal is not an identity. */
        row->type = rc_field(bytes, length, &position);
        row->name = rc_field(bytes, length, &position);
        row->path = rc_field(bytes, length, &position);
        if (!row->type || !row->name || !row->path || !row->type[0] || length - position < 21u) return 0;
        row->offset = (uint64_t)rc_be32(bytes + position) << 32 | rc_be32(bytes + position + 4u);
        row->size = rc_be32(bytes + position + 8u);
        row->stored_size = rc_be32(bytes + position + 12u);
        if (bytes[position + 20u] > 1u) return 0;
        row->archive = (unsigned char)(archive + bytes[position + 20u]);
        position += 21u;
    }
    return position == length;
}

static unsigned char *rc_read_file(const char *path, size_t *length)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    unsigned char *bytes = NULL;
    DWORD got;
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > RC_INDEX_LIMIT) goto done;
    bytes = (unsigned char *)malloc((size_t)size.QuadPart + 1u);
    if (!bytes) goto done;
    if (!ReadFile(file, bytes, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart) {
        free(bytes); bytes = NULL; goto done;
    }
    *length = got;
done:
    CloseHandle(file); return bytes;
}

static int rc_path_cmp(const void *a, const void *b)
{
    const sh_resource_catalog_entry *left = *(const sh_resource_catalog_entry *const *)a;
    const sh_resource_catalog_entry *right = *(const sh_resource_catalog_entry *const *)b;
    int c = strcmp(left->path, right->path);
    if (c) return c;
    if (left->archive / 2u != right->archive / 2u) return left->archive < right->archive ? -1 : 1;
    c = strcmp(left->type, right->type);
    return c ? c : strcmp(left->name, right->name);
}

static int rc_identity_cmp(const void *a, const void *b)
{
    const sh_resource_catalog_entry *left = *(const sh_resource_catalog_entry *const *)a;
    const sh_resource_catalog_entry *right = *(const sh_resource_catalog_entry *const *)b;
    int c = strcmp(left->type, right->type);
    if (!c) c = strcmp(left->name, right->name);
    return c ? c : rc_path_cmp(a, b);
}

static const char *rc_legacy_provider(const sh_resource_catalog_entry *row)
{ return row->path[0] ? row->path : row->name; }

static int rc_legacy_path_cmp(const void *a, const void *b)
{
    const sh_resource_catalog_entry *left = *(const sh_resource_catalog_entry *const *)a;
    const sh_resource_catalog_entry *right = *(const sh_resource_catalog_entry *const *)b;
    return strcmp(rc_legacy_provider(left), rc_legacy_provider(right));
}

sh_resource_catalog *sh_resource_catalog_open(const char *doom_base, char *error, size_t error_capacity)
{
    static const char *const stems[] = {"snap_gameresources", "gameresources"};
    sh_resource_catalog *catalog = (sh_resource_catalog *)calloc(1, sizeof(*catalog));
    char path[4096];
    size_t i;
    if (error && error_capacity) error[0] = 0;
    if (!catalog || !doom_base || !doom_base[0]) goto bad;
    for (i = 0; i < 4; i++) catalog->archives[i] = INVALID_HANDLE_VALUE;
    InitializeSRWLock(&catalog->archive_lock);
    for (i = 0; i < 2; i++) {
        size_t length = 0;
        unsigned char *bytes;
        if (snprintf(path, sizeof(path), "%s/%s.pindex", doom_base, stems[i]) >= sizeof(path)) goto bad;
        bytes = rc_read_file(path, &length);
        if (!bytes) goto bad;
        if (!rc_parse(catalog, bytes, length, (unsigned)i * 2u)) { free(bytes); goto bad; }
        free(bytes);
    }
    for (i = 0; i < 4; i++) {
        LARGE_INTEGER size;
        BY_HANDLE_FILE_INFORMATION info;
        if (snprintf(path, sizeof(path), "%s/%s.%s", doom_base, stems[i / 2u], i % 2u ? "patch" : "resources") >= sizeof(path)) goto bad;
        catalog->archives[i] = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (catalog->archives[i] == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(catalog->archives[i], &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(catalog->archives[i], &size) || size.QuadPart < 0) goto bad;
        catalog->archive_sizes[i] = (uint64_t)size.QuadPart;
    }
    catalog->paths = (sh_resource_catalog_entry **)calloc(catalog->count ? catalog->count : 1u, sizeof(*catalog->paths));
    catalog->identities = (sh_resource_catalog_entry **)calloc(catalog->count ? catalog->count : 1u, sizeof(*catalog->identities));
    catalog->legacy_paths = (sh_resource_catalog_entry **)calloc(catalog->count ? catalog->count : 1u, sizeof(*catalog->legacy_paths));
    if (!catalog->paths || !catalog->identities || !catalog->legacy_paths) goto bad;
    for (i = 0; i < catalog->count; i++) {
        const sh_resource_catalog_entry *row = &catalog->rows[i];
        uint64_t archive_size = catalog->archive_sizes[row->archive];
        if (row->offset > archive_size || row->stored_size > archive_size - row->offset ||
            ((!row->size) != (!row->stored_size))) goto bad;
        catalog->legacy_paths[i] = catalog->paths[i] = catalog->identities[i] = &catalog->rows[i];
    }
    qsort(catalog->paths, catalog->count, sizeof(*catalog->paths), rc_path_cmp);
    qsort(catalog->identities, catalog->count, sizeof(*catalog->identities), rc_identity_cmp);
    qsort(catalog->legacy_paths, catalog->count, sizeof(*catalog->legacy_paths), rc_legacy_path_cmp);
    return catalog;
bad:
    if (error && error_capacity) snprintf(error, error_capacity, "installed resource catalog or archive validation failed under %s", doom_base ? doom_base : "(null)");
    sh_resource_catalog_close(catalog); return NULL;
}

void sh_resource_catalog_close(sh_resource_catalog *catalog)
{
    size_t i;
    if (!catalog) return;
    for (i = 0; i < 4; i++) if (catalog->archives[i] && catalog->archives[i] != INVALID_HANDLE_VALUE) CloseHandle(catalog->archives[i]);
    for (i = 0; i < catalog->count; i++) { free(catalog->rows[i].type); free(catalog->rows[i].name); free(catalog->rows[i].path); }
    free(catalog->legacy_paths); free(catalog->paths); free(catalog->identities); free(catalog->rows); free(catalog);
}

size_t sh_resource_catalog_count(const sh_resource_catalog *catalog) { return catalog ? catalog->count : 0; }
const sh_resource_catalog_entry *sh_resource_catalog_at(const sh_resource_catalog *catalog, size_t index)
{
    return catalog && index < catalog->count ? &catalog->rows[index] : NULL;
}

static int rc_read_payload(sh_resource_catalog *catalog,
    const sh_resource_catalog_entry *row, unsigned char *stored)
{
    DWORD got = 0;
    LARGE_INTEGER position;
    int ok;
    if (!row->stored_size) return 1;
    position.QuadPart = (LONGLONG)row->offset;
    AcquireSRWLockExclusive(&catalog->archive_lock);
    ok = SetFilePointerEx(catalog->archives[row->archive], position, NULL, FILE_BEGIN) &&
         ReadFile(catalog->archives[row->archive], stored, row->stored_size, &got, NULL) && got == row->stored_size;
    ReleaseSRWLockExclusive(&catalog->archive_lock);
    return ok;
}

static int rc_read(sh_resource_catalog *catalog, const sh_resource_catalog_entry *row,
    unsigned char **body, size_t *length, unsigned char **encoded)
{
    unsigned char *stored = NULL, *decoded = NULL;
    int ok = 0;
    if (encoded) *encoded = NULL;
    if (row->size > RC_RESOURCE_LIMIT || row->stored_size > RC_RESOURCE_LIMIT) return 0;
    decoded = (unsigned char *)malloc((size_t)row->size + 1u);
    if (!decoded) goto done;
    decoded[row->size] = 0;
    if (!row->size) { ok = 1; goto done; }
    stored = row->size == row->stored_size ? decoded : (unsigned char *)malloc(row->stored_size);
    if (!stored) goto done;
    ok = rc_read_payload(catalog, row, stored);
    if (ok && stored != decoded) ok = sh_inflate_raw(stored, row->stored_size, decoded, row->size) == row->size;
done:
    if (stored != decoded) {
        if (ok && encoded) *encoded = stored;
        else free(stored);
    }
    if (!ok) { free(decoded); return 0; }
    *body = decoded; *length = row->size; return 1;
}

int sh_resource_catalog_read_entry(sh_resource_catalog *catalog, size_t index, unsigned char **body, size_t *length)
{
    if (body) *body = NULL;
    if (length) *length = 0;
    return catalog && index < catalog->count && body && length && rc_read(catalog, &catalog->rows[index], body, length, NULL);
}

static char *rc_key(const char *text)
{
    size_t i, n;
    char *out;
    if (!text || (n = strlen(text)) >= RC_FIELD_LIMIT) return NULL;
    out = (char *)malloc(n + 1u);
    if (!out) return NULL;
    for (i = 0; i <= n; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        out[i] = c == '\\' ? '/' : c;
    }
    return out;
}

int sh_resource_catalog_read_path(void *context, const char *path, unsigned char **body, size_t *length)
{
    sh_resource_catalog *catalog = (sh_resource_catalog *)context;
    size_t low = 0, high, i;
    int result = 0;
    char *key = rc_key(path);
    unsigned char *first = NULL, *encoded = NULL, *comparison = NULL;
    const sh_resource_catalog_entry *initial;
    size_t first_length = 0;
    if (body) *body = NULL;
    if (length) *length = 0;
    if (!catalog || !key || !key[0] || !body || !length) { free(key); return -1; }
    high = catalog->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        if (strcmp(catalog->paths[mid]->path, key) < 0) low = mid + 1u; else high = mid;
    }
    if (low == catalog->count || strcmp(catalog->paths[low]->path, key)) goto done;
    initial = catalog->paths[low];
    if (!rc_read(catalog, initial, &first, &first_length, &encoded)) { result = -1; goto done; }
    for (i = low + 1u; i < catalog->count && !strcmp(catalog->paths[i]->path, key) &&
        catalog->paths[i]->archive / 2u == catalog->paths[low]->archive / 2u; i++) {
        unsigned char *other = NULL;
        size_t other_length = 0;
        const sh_resource_catalog_entry *row = catalog->paths[i];
        int same;
        if (row->size != initial->size) { result = -1; goto done; }
        if (row->stored_size == initial->stored_size) {
            /* Shader includes can have thousands of archive copies. Equal
             * encoded slices share the already-validated decode; different
             * encodings still require a decoded comparison. Archive handles
             * deny writes while this catalog is alive. */
            if (row->archive == initial->archive && row->offset == initial->offset) continue;
            if (!row->stored_size) continue;
            if (!comparison && !(comparison = (unsigned char *)malloc(row->stored_size))) {
                result = -1; goto done;
            }
            if (!rc_read_payload(catalog, row, comparison)) { result = -1; goto done; }
            if (!memcmp(encoded ? encoded : first, comparison, row->stored_size)) continue;
        }
        same = rc_read(catalog, row, &other, &other_length, NULL) &&
            first_length == other_length && (!first_length || !memcmp(first, other, first_length));
        free(other);
        if (!same) { result = -1; goto done; }
    }
    result = catalog->paths[low]->archive / 2u ? 2 : 1;
    *body = first; *length = first_length; first = NULL;
done:
    free(first); free(encoded); free(comparison); free(key); return result;
}

size_t sh_resource_catalog_find_path(const sh_resource_catalog *catalog,
    const char *path, const sh_resource_catalog_entry *const **entries)
{
    size_t low = 0, high, end;
    char *key = rc_key(path);
    if (entries) *entries = NULL;
    if (!catalog || !key || !*key || !entries) { free(key); return 0; }
    high = catalog->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        if (strcmp(catalog->paths[mid]->path, key) < 0) low = mid + 1u; else high = mid;
    }
    for (end = low; end < catalog->count && !strcmp(catalog->paths[end]->path, key); end++) {}
    if (end > low) *entries = (const sh_resource_catalog_entry *const *)(catalog->paths + low);
    free(key); return end - low;
}

size_t sh_resource_catalog_find(const sh_resource_catalog *catalog, const char *type, const char *name,
                                 const sh_resource_catalog_entry *const **entries)
{
    size_t low = 0, high, end;
    char *type_key = rc_key(type), *name_key = rc_key(name);
    if (entries) *entries = NULL;
    if (!catalog || !type_key || !name_key || !entries) { free(type_key); free(name_key); return 0; }
    high = catalog->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        int c = strcmp(catalog->identities[mid]->type, type_key);
        if (!c) c = strcmp(catalog->identities[mid]->name, name_key);
        if (c < 0) low = mid + 1u; else high = mid;
    }
    for (end = low; end < catalog->count && !strcmp(catalog->identities[end]->type, type_key) &&
         !strcmp(catalog->identities[end]->name, name_key); end++) {}
    if (end > low) *entries = (const sh_resource_catalog_entry *const *)(catalog->identities + low);
    free(type_key); free(name_key); return end - low;
}

static size_t rc_find_legacy_path(const sh_resource_catalog *catalog, const char *path,
    const sh_resource_catalog_entry *const **entries)
{
    size_t low = 0, high = catalog->count, end;
    while (low < high) {
        size_t mid = low + (high - low) / 2u;
        if (_stricmp(rc_legacy_provider(catalog->legacy_paths[mid]), path) < 0) low = mid + 1u;
        else high = mid;
    }
    for (end = low; end < catalog->count && !_stricmp(rc_legacy_provider(catalog->legacy_paths[end]), path); end++) {}
    *entries = (const sh_resource_catalog_entry *const *)(catalog->legacy_paths + low);
    return end - low;
}

int sh_resource_catalog_legacy_read(void *context, const char *type,
    const char *name, const char *path, unsigned char **body, size_t *length,
    char *error, size_t capacity)
{
    sh_resource_catalog *catalog = context;
    const sh_resource_catalog_entry *const *rows = NULL;
    unsigned char *first = NULL;
    size_t count, i, size = 0;
    int result = 0, stock = 0, same_stock = 1;
    const char *failure = "installed game resource catalog is unavailable";
    if (body) *body = NULL;
    if (length) *length = 0;
    if (!catalog || !path) goto done;
    if (!type) {
        const char *slash = strchr(path, '/'), *second = slash ? strchr(slash + 1, '/') : NULL;
        size_t n = second ? (size_t)(second - path) : strlen(path);
        if (!name && rc_find_legacy_path(catalog, path, &rows)) return 1;
        /* Match disk migration: an installed first-two-segment namespace
         * preserves new authored files too, including shader includes. */
        if (slash && (second || (name && !strcmp(name, "directory"))))
            for (i = 0; i < catalog->count; i++) {
                const char *provider = rc_legacy_provider(&catalog->rows[i]);
                if (!_strnicmp(provider, path, n) && provider[n] == '/') return 1;
            }
        return 0;
    }
    if (!body || !length || !name) goto done;
    count = sh_resource_catalog_find(catalog, type, name, &rows);
    failure = "legacy manifest resource is missing from the installed campaign";
    for (i = 0; i < count; i++) if (rows[i]->archive / 2u == 1u && !_stricmp(rc_legacy_provider(rows[i]), path)) {
        unsigned char *other = NULL;
        size_t other_size;
        failure = "legacy manifest resource could not be read";
        if (!rc_read(catalog, rows[i], &other, &other_size, NULL)) goto done;
        if (!first) { first = other; size = other_size; }
        else {
            int same = size == other_size && (!size || !memcmp(first, other, size));
            free(other); failure = "legacy manifest resource has conflicting installed providers";
            if (!same) goto done;
        }
    }
    if (!first) goto done;
    count = rc_find_legacy_path(catalog, path, &rows);
    for (i = 0; i < count; i++) if (rows[i]->archive / 2u == 0u) {
        unsigned char *other = NULL;
        size_t other_size;
        stock = 1; failure = "shipped SnapMap resource could not be verified";
        if (!rc_read(catalog, rows[i], &other, &other_size, NULL)) goto done;
        if (size != other_size || (size && memcmp(first, other, size))) same_stock = 0;
        free(other);
    }
    if (stock && same_stock) result = 2;
    else { *body = first; *length = size; first = NULL; result = 1; }
done:
    free(first);
    if (!result && error && capacity) snprintf(error, capacity, "%s: %s/%s (%s)",
        failure, type ? type : "path", name ? name : "", path ? path : "");
    return !type && !catalog ? -1 : result;
}
