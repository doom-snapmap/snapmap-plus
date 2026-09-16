#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include "package_sources.h"

#pragma comment(lib, "bcrypt.lib")

typedef struct ps_scan {
    sh_package_sources *out;
    size_t file_capacity, component_capacity;
    char *error;
    size_t error_capacity;
} ps_scan;

static int ps_error(char *out, size_t capacity, const char *format, ...)
{
    va_list args;
    if (out && capacity) {
        va_start(args, format); vsnprintf(out, capacity, format, args); va_end(args);
    }
    return 0;
}

static char *ps_copy(const char *text)
{
    size_t length = strlen(text) + 1u;
    char *out = (char *)malloc(length);
    if (out) memcpy(out, text, length);
    return out;
}

static char *ps_join(const char *parent, const char *child)
{
    size_t a = strlen(parent), b = strlen(child);
    char *out;
    if (a + b + 2u >= SH_PACKAGE_SOURCE_PATH_CAP) return NULL;
    out = (char *)malloc(a + b + 2u);
    if (out) snprintf(out, a + b + 2u, "%s%s%s", parent, a ? "/" : "", child);
    return out;
}

const char *sh_package_source_identity(const sh_package_sources *sources, size_t package)
{
    size_t i;
    if (!sources || package >= sources->package_count) return NULL;
    for (i = 0; i < sources->component_count; i++) {
        const sh_package_component *component = &sources->components[i];
        if (component->owner == package && component->relative && !component->relative[0])
            return component->descriptor.id;
    }
    return NULL;
}

int sh_package_source_delivered(const sh_package_sources *sources, size_t package)
{
    const char *name;
    size_t i;
    if (!sources || package >= sources->package_count) return 0;
    name = sources->packages[package].name;
    if (!name || strncmp(name, "map-", 4)) return 0;
    for (i = 4; i < 20; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return name[20] == '/';
}

char *sh_package_engine_path(const char *path)
{
    char *out, *part, *p;
    size_t length;
    if (!path || !(length = strlen(path)) || length >= SH_PACKAGE_SOURCE_PATH_CAP ||
        path[0] == '/' || path[0] == '\\') return NULL;
    out = ps_copy(path);
    if (!out) return NULL;
    for (p = out; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') *p = '/';
        else if (c >= 'A' && c <= 'Z') *p += 'a' - 'A';
        else if (c < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' ||
                 c == '<' || c == '>' || c == '|') goto bad;
    }
    part = out;
    for (p = out; ; p++) if (*p == '/' || !*p) {
        size_t n = (size_t)(p - part), stem = 0;
        if (!n || part[n - 1] == '.' || part[n - 1] == ' ') goto bad;
        while (stem < n && part[stem] != '.') stem++;
        if ((stem == 3 && (!memcmp(part, "con", 3) || !memcmp(part, "prn", 3) ||
                          !memcmp(part, "aux", 3) || !memcmp(part, "nul", 3))) ||
            (stem == 4 && (!memcmp(part, "com", 3) || !memcmp(part, "lpt", 3)) &&
             part[3] >= '1' && part[3] <= '9')) goto bad;
        if (!*p) break;
        part = p + 1;
    }
    return out;
bad:
    free(out); return NULL;
}

wchar_t *sh_package_source_wide_path(const char *absolute)
{
    wchar_t *out;
    int length;
    size_t i;
    /* Package roots are absolute local paths. Do not reinterpret relative,
     * device or UNC paths through the engine's working directory. */
    if (!absolute || !((absolute[0] >= 'A' && absolute[0] <= 'Z') ||
                       (absolute[0] >= 'a' && absolute[0] <= 'z')) ||
        absolute[1] != ':' || (absolute[2] != '/' && absolute[2] != '\\')) return NULL;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, absolute, -1, NULL, 0);
    if (!length || (unsigned)length + 4u >= SH_PACKAGE_SOURCE_PATH_CAP) return NULL;
    out = (wchar_t *)malloc(((size_t)length + 4u) * sizeof(wchar_t));
    if (!out) return NULL;
    memcpy(out, L"\\\\?\\", 4u * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, absolute, -1, out + 4, length)) {
        free(out); return NULL;
    }
    for (i = 4; out[i]; i++) if (out[i] == L'/') out[i] = L'\\';
    return out;
}

static char *ps_utf8(const wchar_t *text)
{
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    char *out = length ? (char *)malloc((size_t)length) : NULL;
    if (out && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out, length, NULL, NULL)) {
        free(out); out = NULL;
    }
    return out;
}

static DWORD ps_attributes(const char *path)
{
    wchar_t *wide = sh_package_source_wide_path(path);
    DWORD attributes;
    if (!wide) { SetLastError(ERROR_INVALID_NAME); return INVALID_FILE_ATTRIBUTES; }
    attributes = GetFileAttributesW(wide); free(wide); return attributes;
}

/* The handle denies writers for the entire read/hash. The caller either gets
 * every byte and its hash or a failure, including a changed source size. */
static int ps_read_to(const char *path, unsigned char **bytes, size_t limit,
                       uint64_t *length, unsigned char digest[32], HANDLE destination,
                       HANDLE *retained)
{
    wchar_t *wide = sh_package_source_wide_path(path);
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    unsigned char chunk[65536], *body = NULL;
    uint64_t total = 0;
    int ok = 0;
    if (bytes) *bytes = NULL;
    if (retained) *retained = INVALID_HANDLE_VALUE;
    if (!wide) return 0;
    file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0) goto done;
    if (bytes) {
        if ((uint64_t)size.QuadPart > limit || (uint64_t)size.QuadPart >= SIZE_MAX) goto done;
        body = (unsigned char *)malloc((size_t)size.QuadPart + 1u);
        if (!body) goto done;
    }
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, chunk, sizeof(chunk), &got, NULL)) goto done;
        if (!got) break;
        if (total + got > (uint64_t)size.QuadPart || BCryptHashData(hash, chunk, got, 0) < 0) goto done;
        if (body) memcpy(body + (size_t)total, chunk, got);
        if (destination != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!WriteFile(destination, chunk, got, &written, NULL) || written != got) goto done;
        }
        total += got;
    }
    if (total != (uint64_t)size.QuadPart || BCryptFinishHash(hash, digest, 32, 0) < 0) goto done;
    if (retained) {
        LARGE_INTEGER start = {0};
        if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN)) goto done;
        *retained = file; file = INVALID_HANDLE_VALUE;
    }
    *length = total;
    if (body) { body[(size_t)total] = 0; *bytes = body; body = NULL; }
    ok = 1;
done:
    free(body);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static int ps_read(const char *path, unsigned char **bytes, size_t limit,
                    uint64_t *length, unsigned char digest[32])
{
    return ps_read_to(path, bytes, limit, length, digest, INVALID_HANDLE_VALUE, NULL);
}

FILE *sh_package_source_open(const sh_package_source_file *file,
    char *error, size_t error_capacity)
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    FILE *stream = NULL;
    uint64_t length = 0;
    unsigned char digest[32];
    int descriptor;
    if (file && !file->directory && ps_read_to(file->absolute, NULL, 0, &length,
            digest, INVALID_HANDLE_VALUE, &handle) && length == file->length &&
        !memcmp(digest, file->digest, 32)) {
        descriptor = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
        if (descriptor != -1) {
            handle = INVALID_HANDLE_VALUE; /* CRT now owns the verified handle. */
            stream = _fdopen(descriptor, "rb");
            if (!stream) _close(descriptor);
        }
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (!stream) ps_error(error, error_capacity, "source changed or cannot open a stable stream: %s",
        file ? file->relative : "(null)");
    return stream;
}

struct sh_package_file {
    HANDLE handle;
    uint64_t length;
    unsigned char digest[32];
    volatile LONG references;
};

int sh_package_bytes_identity(const void *body, size_t length, sh_package_file_identity *out)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    size_t offset = 0;
    int ok = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if ((!body && length) || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    while (offset < length) {
        size_t amount = length - offset;
        if (amount > 1048576) amount = 1048576;
        if (BCryptHashData(hash, (PUCHAR)body + offset, (ULONG)amount, 0) < 0) goto done;
        offset += amount;
    }
    if (BCryptFinishHash(hash, out->digest, sizeof(out->digest), 0) < 0) goto done;
    out->length = length; ok = 1;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

sh_package_file *sh_package_file_capture(const char *absolute, char *error, size_t capacity)
{
    sh_package_file *out = calloc(1, sizeof(*out));
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (out && absolute && ps_read_to(absolute, NULL, 0, &out->length, out->digest,
            INVALID_HANDLE_VALUE, &handle)) {
        out->handle = handle; out->references = 1;
        if (error && capacity) error[0] = 0;
        return out;
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    free(out); ps_error(error, capacity, "cannot capture original resource file: %s", absolute ? absolute : "(null)");
    return NULL;
}

int sh_package_file_get_identity(const sh_package_file *file, sh_package_file_identity *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!file) return 0;
    out->length = file->length; memcpy(out->digest, file->digest, sizeof(out->digest)); return 1;
}

sh_package_file *sh_package_file_seal(const sh_package_source_file *file,
    char *error, size_t capacity)
{
    sh_package_file *out = (sh_package_file *)calloc(1, sizeof(*out));
    uint64_t length = 0;
    unsigned char digest[32];
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (out && file && !file->directory && ps_read_to(file->absolute, NULL, 0,
            &length, digest, INVALID_HANDLE_VALUE, &handle) && length == file->length &&
            !memcmp(digest, file->digest, sizeof(digest))) {
        out->handle = handle; out->length = length; out->references = 1;
        memcpy(out->digest, digest, sizeof(out->digest));
        return out;
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    free(out);
    ps_error(error, capacity, "cannot seal verified resource cache: %s", file ? file->relative : "(null)");
    return NULL;
}
sh_package_file *sh_package_file_retain(sh_package_file *file)
{ if (file) InterlockedIncrement(&file->references); return file; }
void sh_package_file_release(sh_package_file *file)
{
    if (file && !InterlockedDecrement(&file->references)) { CloseHandle(file->handle); free(file); }
}
FILE *sh_package_file_open(const sh_package_file *file, uint64_t *length,
    char *error, size_t capacity)
{
    HANDLE handle;
    FILE *stream = NULL;
    int descriptor;
    if (length) *length = 0;
    if (!file || !length) { ps_error(error, capacity, "missing sealed resource"); return NULL; }
    /* DuplicateHandle alone shares a file position. ReOpenFile retains the
     * same protected file identity while giving CRT readers independent seeks. */
    handle = ReOpenFile(file->handle, GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_SEQUENTIAL_SCAN);
    if (handle != INVALID_HANDLE_VALUE) {
        descriptor = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
        if (descriptor != -1) {
            handle = INVALID_HANDLE_VALUE;
            stream = _fdopen(descriptor, "rb");
            if (!stream) _close(descriptor);
        }
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
    if (stream) *length = file->length;
    else ps_error(error, capacity, "cannot open sealed resource stream");
    return stream;
}
unsigned char *sh_package_file_read(const sh_package_file *file, size_t limit,
    size_t *length, char *error, size_t capacity)
{
    FILE *stream;
    uint64_t size = 0;
    unsigned char *body;
    if (length) *length = 0;
    if (!file || !length || file->length > limit || file->length >= SIZE_MAX) {
        ps_error(error, capacity, "sealed resource exceeds requested read size"); return NULL;
    }
    stream = sh_package_file_open(file, &size, error, capacity);
    if (!stream) return NULL;
    body = (unsigned char *)malloc((size_t)size+1);
    if (!body || fread(body, 1, (size_t)size, stream) != size) {
        free(body); fclose(stream); ps_error(error, capacity, "sealed resource read failed"); return NULL;
    }
    fclose(stream); body[(size_t)size] = 0; *length = (size_t)size; return body;
}

int sh_package_source_verify(const sh_package_source_file *file)
{
    uint64_t length = 0;
    unsigned char digest[32];
    return file && !file->directory && ps_read(file->absolute, NULL, 0, &length, digest) &&
        length == file->length && !memcmp(digest, file->digest, 32);
}

int sh_package_source_copy(const sh_package_source_file *file, const char *destination)
{
    wchar_t *wide = sh_package_source_wide_path(destination);
    HANDLE output = INVALID_HANDLE_VALUE;
    uint64_t length = 0;
    unsigned char digest[32];
    int ok = 0;
    if (!file || file->directory || !wide) { free(wide); return 0; }
    output = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (output != INVALID_HANDLE_VALUE) {
        ok = ps_read_to(file->absolute, NULL, 0, &length, digest, output, NULL) &&
            length == file->length && !memcmp(digest, file->digest, 32) && FlushFileBuffers(output);
        CloseHandle(output);
        if (!ok) DeleteFileW(wide); /* Only the exact file created by this call. */
    }
    free(wide); return ok;
}

unsigned char *sh_package_source_read(const sh_package_source_file *file,
                                      size_t limit, size_t *length,
                                      char *error, size_t error_capacity)
{
    unsigned char *body = NULL, digest[32];
    uint64_t got = 0;
    if (length) *length = 0;
    if (!file || file->directory || !length || file->length > limit ||
        !ps_read(file->absolute, &body, limit, &got, digest) || got != file->length ||
        memcmp(file->digest, digest, sizeof(digest))) {
        free(body);
        ps_error(error, error_capacity, "source changed, unreadable or exceeds its limit: %s",
                  file ? file->relative : "(null)");
        return NULL;
    }
    *length = (size_t)got; return body;
}

static int ps_grow(void **array, size_t *capacity, size_t count, size_t width)
{
    size_t next, maximum;
    void *grown;
    if (!width) return 0;
    maximum = SIZE_MAX / width;
    if (count >= maximum || *capacity > maximum) return 0;
    if (count < *capacity) return 1;
    next = *capacity ? *capacity : (maximum < 32u ? maximum : 32u);
    while (next <= count) next = next > maximum / 2u ? maximum : next * 2u;
    grown = realloc(*array, next * width);
    if (!grown) return 0;
    memset((char *)grown + *capacity * width, 0, (next - *capacity) * width);
    *array = grown; *capacity = next; return 1;
}

static int ps_component(ps_scan *scan, size_t owner, const char *relative,
                         const char *root, size_t *index)
{
    sh_package_component *component;
    unsigned char *body = NULL, digest[32];
    uint64_t length = 0;
    char *marker = ps_join(root, "package.json");
    int ok = 0;
    if (!marker || !ps_grow((void **)&scan->out->components, &scan->component_capacity,
        scan->out->component_count, sizeof(*component))) goto done;
    *index = scan->out->component_count++;
    component = &scan->out->components[*index];
    component->owner = owner; component->relative = ps_copy(relative); component->root = ps_copy(root);
    if (!component->relative || !component->root ||
        !ps_read(marker, &body, (size_t)PTRDIFF_MAX, &length, digest)) goto done;
    if (!sh_package_descriptor_parse((const char *)body, (size_t)length,
                                     &component->descriptor, scan->error, scan->error_capacity)) goto done;
    memcpy(component->descriptor_digest, digest, sizeof(digest));
    ok = 1;
done:
    if (!ok && scan->error && scan->error_capacity && !scan->error[0])
        ps_error(scan->error, scan->error_capacity, "cannot read package descriptor: %s", marker ? marker : root);
    free(marker); free(body); return ok;
}

static char *ps_engine(const sh_package_component *component, const char *relative)
{
    const char *local = relative;
    size_t n = strlen(component->relative);
    if (n) local += n + 1u;
    if (!_strnicmp(local, "assets/", 7)) return sh_package_engine_path(local + 7);
    return NULL;
}

typedef struct ps_directory {
    size_t component;
    /* Strings belong to the inventory and survive vector reallocation. */
    const char *root, *relative;
} ps_directory;

typedef struct ps_pending {
    ps_directory *items;
    size_t count, capacity;
} ps_pending;

static int ps_enqueue(ps_pending *pending, size_t component,
                       const char *root, const char *relative)
{
    ps_directory *item;
    if (!ps_grow((void **)&pending->items, &pending->capacity,
                   pending->count, sizeof(*item))) return 0;
    item = &pending->items[pending->count++];
    item->component = component; item->root = root; item->relative = relative;
    return 1;
}

static int ps_walk_directory(ps_scan *scan, size_t owner, size_t component_index,
                              const char *root, const char *relative, ps_pending *pending)
{
    char *pattern = NULL;
    wchar_t *wide = NULL;
    WIN32_FIND_DATAW found;
    HANDLE search = INVALID_HANDLE_VALUE;
    DWORD end_error;
    int ok = 0;
    pattern = ps_join(root, "*");
    if (!pattern || !(wide = sh_package_source_wide_path(pattern))) goto done;
    search = FindFirstFileW(wide, &found);
    if (search == INVALID_HANDLE_VALUE) {
        ok = GetLastError() == ERROR_FILE_NOT_FOUND; goto done;
    }
    do {
        char *name = NULL, *path = NULL, *rel = NULL, *canonical = NULL;
        sh_package_source_file *file;
        int entry_ok = 0;
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        name = ps_utf8(found.cFileName);
        if (!name || !(canonical = sh_package_engine_path(name)) ||
            !(path = ps_join(root, name)) || !(rel = ps_join(relative, name)) ||
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) goto entry_done;
        if (!ps_grow((void **)&scan->out->files, &scan->file_capacity, scan->out->file_count,
                      sizeof(*file))) goto entry_done;
        file = &scan->out->files[scan->out->file_count++];
        file->owner = owner; file->component = component_index;
        file->relative = rel; rel = NULL; file->absolute = path; path = NULL;
        file->directory = !!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (file->directory) {
            entry_ok = ps_enqueue(pending, component_index, file->absolute, file->relative);
        } else {
            const sh_package_component *component = &scan->out->components[component_index];
            const char *local = file->relative + strlen(component->relative);
            if (*local == '/') local++;
            file->engine_path = ps_engine(component, file->relative);
            if (!_strnicmp(local, "assets/", 7) && !file->engine_path) goto entry_done;
            entry_ok = ps_read(file->absolute, NULL, 0, &file->length, file->digest);
            if (!_stricmp(local, "package.json") && memcmp(file->digest, component->descriptor_digest, 32))
                entry_ok = 0;
        }
entry_done:
        if (!entry_ok && scan->error && scan->error_capacity && !scan->error[0])
            ps_error(scan->error, scan->error_capacity, "invalid, unreadable or unallocatable package source: %s/%s", root, name ? name : "?");
        free(name); free(path); free(rel); free(canonical);
        if (!entry_ok) goto done;
    } while (FindNextFileW(search, &found));
    end_error = GetLastError();
    ok = end_error == ERROR_NO_MORE_FILES;
done:
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    free(wide); free(pattern);
    if (!ok && scan->error && scan->error_capacity && !scan->error[0])
        ps_error(scan->error, scan->error_capacity, "package source enumeration failed: %s", root);
    return ok;
}

static int ps_walk(ps_scan *scan, size_t owner, size_t component_index,
                    const char *root, const char *relative)
{
    ps_pending pending = {0};
    int ok = ps_enqueue(&pending, component_index, root, relative);
    /* One search handle at a time, with no C-stack cost per authored folder. */
    while (ok && pending.count) {
        ps_directory current = pending.items[--pending.count];
        const sh_package_component *parent = &scan->out->components[current.component];
        const char *local = current.relative + strlen(parent->relative);
        size_t begin = pending.count, end;
        if (*local == '/') local++;
        if (*local && _stricmp(local, "assets") && _strnicmp(local, "assets/", 7)) {
            char *marker = ps_join(current.root, "package.json");
            DWORD attributes, error;
            if (!marker) { ok = 0; break; }
            attributes = ps_attributes(marker); error = GetLastError();
            free(marker);
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                ok = !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                    ps_component(scan, owner, current.relative, current.root, &current.component);
            } else ok = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if (ok) ok = ps_walk_directory(scan, owner, current.component,
                                        current.root, current.relative, &pending);
        if (!ok && scan->error && scan->error_capacity && !scan->error[0])
            ps_error(scan->error, scan->error_capacity, "package directory scan failed: %s", current.root);
        /* Keep depth-first component discovery order after closing the parent. */
        end = pending.count;
        while (begin < end && begin < --end) {
            ps_directory swap = pending.items[begin];
            pending.items[begin++] = pending.items[end]; pending.items[end] = swap;
        }
    }
    free(pending.items); return ok;
}

static int ps_file_compare(const void *a, const void *b)
{
    const sh_package_source_file *left = (const sh_package_source_file *)a;
    const sh_package_source_file *right = (const sh_package_source_file *)b;
    int ci;
    if (left->owner != right->owner) return left->owner < right->owner ? -1 : 1;
    ci = _stricmp(left->relative, right->relative);
    return ci ? ci : strcmp(left->relative, right->relative);
}

static int ps_fingerprints(sh_package_sources *sources)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    size_t i, owner;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    sources->fingerprints = (unsigned char (*)[32])calloc(sources->package_count ? sources->package_count : 1u, 32u);
    if (!sources->fingerprints) goto done;
    for (owner = 0; owner < sources->package_count; owner++) {
        if (BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
        for (i = 0; i < sources->file_count; i++) {
            const sh_package_source_file *file = &sources->files[i];
            unsigned char kind = file->directory ? 'd' : 'f';
            if (file->owner != owner) continue;
            if (BCryptHashData(hash, &kind, 1, 0) < 0 ||
                BCryptHashData(hash, (PUCHAR)file->relative, (ULONG)strlen(file->relative) + 1u, 0) < 0 ||
                (!file->directory && BCryptHashData(hash, (PUCHAR)file->digest, 32, 0) < 0)) goto done;
        }
        if (BCryptFinishHash(hash, sources->fingerprints[owner], 32, 0) < 0) goto done;
        BCryptDestroyHash(hash); hash = NULL;
    }
    ok = 1;
done:
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0); return ok;
}

static sh_package_sources *ps_inventory(const char *data_root, const char *single_root,
                                        char *error, size_t error_capacity)
{
    sh_package_sources *out = (sh_package_sources *)calloc(1, sizeof(*out));
    ps_scan scan = {out, 0, 0, error, error_capacity};
    size_t i;
    if (error && error_capacity) error[0] = 0;
    if (!out) goto bad;
    if (single_root) {
        out->packages = (sh_package *)calloc(1, sizeof(sh_package));
        if (!out->packages) goto bad;
        if (strcpy_s(out->packages[0].root, sizeof(out->packages[0].root), single_root)) goto bad;
        out->package_count = 1;
    } else if (!sh_packages_enumerate(data_root, &out->packages, &out->package_count)) {
        ps_error(error, error_capacity, "package directory enumeration failed"); goto bad;
    }
    for (i = 0; i < out->package_count; i++) {
        size_t component;
        if (!ps_component(&scan, i, "", out->packages[i].root, &component) ||
            !ps_walk(&scan, i, component, out->packages[i].root, "")) goto bad;
        if (single_root && strcpy_s(out->packages[i].name, sizeof(out->packages[i].name),
                                    out->components[component].descriptor.id)) goto bad;
    }
    if (out->file_count) qsort(out->files, out->file_count, sizeof(*out->files), ps_file_compare);
    for (i = 1; i < out->file_count; i++) if (out->files[i - 1].owner == out->files[i].owner &&
        !_stricmp(out->files[i - 1].relative, out->files[i].relative)) {
        ps_error(error, error_capacity, "case-insensitive duplicate source path: %s", out->files[i].relative); goto bad;
    }
    if (!ps_fingerprints(out)) goto bad;
    return out;
bad:
    if (error && error_capacity && !error[0]) ps_error(error, error_capacity, "package inventory allocation or hashing failed");
    sh_package_sources_free(out); return NULL;
}

sh_package_sources *sh_package_sources_scan(const char *data_root, char *error, size_t capacity)
{ return ps_inventory(data_root, NULL, error, capacity); }

sh_package_sources *sh_package_sources_scan_directory(const char *root, char *error, size_t capacity)
{ return root && *root ? ps_inventory(NULL, root, error, capacity) : NULL; }

/* Move an already complete unit into growing vectors; do not repeatedly clone
 * earlier packages when a library contains many small delivery units. */
static int ps_append_unit(sh_package_sources *out, sh_package_sources *unit,
    size_t *packages, size_t *components, size_t *files, size_t *fingerprints)
{
    size_t p = out->package_count, c = out->component_count, f = out->file_count, i;
    if (unit->package_count != 1 || unit->component_count > SIZE_MAX - c ||
        unit->file_count > SIZE_MAX - f || p == SIZE_MAX ||
        !ps_grow((void **)&out->packages, packages, p, sizeof(*out->packages)) ||
        !ps_grow((void **)&out->fingerprints, fingerprints, p, sizeof(*out->fingerprints)) ||
        (unit->component_count && !ps_grow((void **)&out->components, components,
            c + unit->component_count - 1, sizeof(*out->components))) ||
        (unit->file_count && !ps_grow((void **)&out->files, files,
            f + unit->file_count - 1, sizeof(*out->files)))) return 0;
    out->packages[p] = unit->packages[0];
    memcpy(out->fingerprints[p], unit->fingerprints[0], 32);
    for (i = 0; i < unit->component_count; i++) {
        out->components[c + i] = unit->components[i]; out->components[c + i].owner = p;
    }
    for (i = 0; i < unit->file_count; i++) {
        out->files[f + i] = unit->files[i]; out->files[f + i].owner = p;
        out->files[f + i].component += c;
    }
    out->package_count++; out->component_count += unit->component_count; out->file_count += unit->file_count;
    unit->package_count = unit->component_count = unit->file_count = 0;
    return 1;
}

sh_package_sources *sh_package_sources_scan_local(const char *data_root,
    sh_package_source_rejected rejected, void *context, char *error, size_t capacity)
{
    sh_package *packages = NULL;
    sh_package_sources *out = NULL, *unit = NULL;
    size_t count = 0, i, pc = 0, cc = 0, fc = 0, hc = 0;
    char detail[2048];
    if (error && capacity) error[0] = 0;
    if (!rejected || !sh_packages_enumerate(data_root, &packages, &count)) {
        ps_error(error, capacity, "local package directory enumeration failed"); goto bad;
    }
    out = calloc(1, sizeof(*out));
    if (!out) goto bad;
    for (i = 0; i < count; i++) {
        errno = 0; SetLastError(ERROR_SUCCESS);
        unit = sh_package_sources_scan_directory(packages[i].root, detail, sizeof(detail));
        if (!unit) {
            DWORD system_error = GetLastError();
            if (errno == ENOMEM || system_error == ERROR_NOT_ENOUGH_MEMORY ||
                system_error == ERROR_OUTOFMEMORY || system_error == ERROR_NO_SYSTEM_RESOURCES ||
                !detail[0] || !rejected(context, &packages[i], detail)) {
                ps_error(error, capacity, "local package scan failed: %s: %s", packages[i].name, detail); goto bad;
            }
            continue;
        }
        /* Keep discovery's folder-derived name, including map-* provenance. */
        unit->packages[0] = packages[i];
        if (!ps_append_unit(out, unit, &pc, &cc, &fc, &hc)) goto bad;
        sh_package_sources_free(unit); unit = NULL;
    }
    free(packages); return out;
bad:
    if (error && capacity && !error[0]) ps_error(error, capacity, "cannot allocate complete local package inventory");
    free(packages); sh_package_sources_free(unit); sh_package_sources_free(out); return NULL;
}

int sh_package_sources_remove(sh_package_sources *sources, size_t package)
{
    size_t *map, i, kept = 0;
    if (!sources || package >= sources->package_count || sources->component_count > SIZE_MAX / sizeof(*map)) return 0;
    map = malloc((sources->component_count ? sources->component_count : 1) * sizeof(*map));
    if (!map) return 0;
    for (i = 0; i < sources->component_count; i++) {
        sh_package_component *item = &sources->components[i];
        if (item->owner == package) {
            map[i] = SIZE_MAX; free(item->relative); free(item->root);
            sh_package_descriptor_free(&item->descriptor);
        } else {
            map[i] = kept;
            if (item->owner > package) item->owner--;
            sources->components[kept++] = *item;
        }
    }
    sources->component_count = kept; kept = 0;
    for (i = 0; i < sources->file_count; i++) {
        sh_package_source_file *item = &sources->files[i];
        if (item->owner == package) {
            free(item->relative); free(item->absolute); free(item->engine_path);
        } else {
            item->component = map[item->component];
            if (item->owner > package) item->owner--;
            sources->files[kept++] = *item;
        }
    }
    sources->file_count = kept; free(map);
    memmove(sources->packages + package, sources->packages + package + 1,
        (sources->package_count - package - 1) * sizeof(*sources->packages));
    memmove(sources->fingerprints + package, sources->fingerprints + package + 1,
        (sources->package_count - package - 1) * sizeof(*sources->fingerprints));
    sources->package_count--; return 1;
}

sh_package_sources *sh_package_sources_join(const sh_package_sources *local,
    const sh_package_sources *map, char *error, size_t capacity)
{
    sh_package_sources *out = NULL;
    const sh_package_sources *inputs[] = {local, map};
    size_t side, i, packages = 0, components = 0, files = 0;
    if (error && capacity) error[0] = 0;
    if (!local || !map) goto bad;
    for (side = 0; side < 2; side++) {
        const sh_package_sources *in = inputs[side];
        if ((in->package_count && (!in->packages || !in->fingerprints)) ||
            (in->component_count && !in->components) || (in->file_count && !in->files) ||
            in->package_count > SIZE_MAX / sizeof(*in->packages) - packages ||
            in->package_count > SIZE_MAX / 32u - packages ||
            in->component_count > SIZE_MAX / sizeof(*in->components) - components ||
            in->file_count > SIZE_MAX / sizeof(*in->files) - files) goto bad;
        packages += in->package_count; components += in->component_count; files += in->file_count;
    }
    out = calloc(1, sizeof(*out));
    if (!out) goto bad;
    out->packages = calloc(packages ? packages : 1, sizeof(*out->packages));
    out->fingerprints = calloc(packages ? packages : 1, sizeof(*out->fingerprints));
    out->components = calloc(components ? components : 1, sizeof(*out->components));
    out->files = calloc(files ? files : 1, sizeof(*out->files));
    if (!out->packages || !out->fingerprints || !out->components || !out->files) goto bad;
    for (side = 0; side < 2; side++) {
        const sh_package_sources *in = inputs[side];
        size_t owner_offset = out->package_count, component_offset = out->component_count;
        if (in->package_count) {
            memcpy(out->packages + owner_offset, in->packages, in->package_count * sizeof(*in->packages));
            memcpy(out->fingerprints + owner_offset, in->fingerprints, in->package_count * sizeof(*in->fingerprints));
        }
        out->package_count += in->package_count;
        for (i = 0; i < in->component_count; i++) {
            const sh_package_component *from = &in->components[i];
            sh_package_component *to = &out->components[out->component_count++];
            char *json; size_t length;
            if (from->owner >= in->package_count || !from->root || !from->relative) goto bad;
            to->owner = from->owner + owner_offset;
            to->root = _strdup(from->root); to->relative = _strdup(from->relative);
            memcpy(to->descriptor_digest, from->descriptor_digest, sizeof(to->descriptor_digest));
            json = sh_json_serialize_object(&from->descriptor.fields, 0, &length);
            if (!json) goto bad;
            int valid = sh_package_descriptor_parse(json, length, &to->descriptor, error, capacity);
            free(json);
            if (!valid || !to->root || !to->relative) goto bad;
        }
        for (i = 0; i < in->file_count; i++) {
            const sh_package_source_file *from = &in->files[i];
            sh_package_source_file *to = &out->files[out->file_count++];
            if (from->owner >= in->package_count || from->component >= in->component_count ||
                in->components[from->component].owner != from->owner || !from->relative || !from->absolute) goto bad;
            to->owner = from->owner + owner_offset; to->component = from->component + component_offset;
            to->length = from->length; to->directory = from->directory;
            memcpy(to->digest, from->digest, sizeof(to->digest));
            to->relative = _strdup(from->relative); to->absolute = _strdup(from->absolute);
            to->engine_path = from->engine_path ? _strdup(from->engine_path) : NULL;
            if (!to->relative || !to->absolute || (from->engine_path && !to->engine_path)) goto bad;
        }
    }
    return out;
bad:
    if (error && capacity && !error[0]) ps_error(error, capacity, "cannot retain complete package source inventories");
    sh_package_sources_free(out); return NULL;
}

void sh_package_sources_free(sh_package_sources *sources)
{
    size_t i;
    if (!sources) return;
    for (i = 0; i < sources->file_count; i++) {
        free(sources->files[i].relative); free(sources->files[i].absolute); free(sources->files[i].engine_path);
    }
    for (i = 0; i < sources->component_count; i++) {
        free(sources->components[i].relative); free(sources->components[i].root);
        sh_package_descriptor_free(&sources->components[i].descriptor);
    }
    free(sources->components); free(sources->files); free(sources->packages); free(sources->fingerprints); free(sources);
}
