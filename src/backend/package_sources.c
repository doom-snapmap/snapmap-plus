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

/* One SHA-256 provider for the process. Opening a provider per digest cost
 * more than hashing the small inputs most identities cover; the handle may
 * create hash objects on any thread and stays open until process exit. */
static INIT_ONCE g_ps_sha256_once = INIT_ONCE_STATIC_INIT;
static BCRYPT_ALG_HANDLE g_ps_sha256;
static BOOL CALLBACK ps_sha256_open(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    (void)once; (void)parameter; (void)context;
    return BCryptOpenAlgorithmProvider(&g_ps_sha256, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0;
}
static BCRYPT_ALG_HANDLE ps_sha256(void)
{ return InitOnceExecuteOnce(&g_ps_sha256_once, ps_sha256_open, NULL, NULL) ? g_ps_sha256 : NULL; }

typedef struct ps_scan {
    sh_package_sources *out;
    size_t file_capacity, component_capacity;
    char *error;
    size_t error_capacity;
    /* The failure is the package's own content, not storage, memory or a
     * change during the scan. Only content failures may isolate a package. */
    int content;
} ps_scan;

static int ps_error(char *out, size_t capacity, const char *format, ...)
{
    va_list args;
    if (out && capacity) {
        va_start(args, format); vsnprintf(out, capacity, format, args); va_end(args);
    }
    return 0;
}

static int ps_refuse(ps_scan *scan, const char *format, ...)
{
    va_list args;
    scan->content = 1;
    if (scan->error && scan->error_capacity && !scan->error[0]) {
        va_start(args, format); vsnprintf(scan->error, scan->error_capacity, format, args); va_end(args);
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

/* 1 converted, 0 not valid Unicode, -1 allocation failure. */
static int ps_utf8(const wchar_t *text, char **out)
{
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    *out = NULL;
    if (!length) return 0;
    *out = (char *)malloc((size_t)length);
    if (!*out) return -1;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, *out, length, NULL, NULL)) {
        free(*out); *out = NULL; return 0;
    }
    return 1;
}

/* The per-name rules of sh_package_engine_path, without allocating, so an
 * unusable authored name is not mistaken for an allocation failure. */
static int ps_name_valid(const char *name, size_t n)
{
    size_t i, stem = 0;
    if (!n || name[n - 1] == '.' || name[n - 1] == ' ') return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|' || c == '/' || c == '\\') return 0;
    }
    while (stem < n && name[stem] != '.') stem++;
    if (stem == 3 && (!_strnicmp(name, "con", 3) || !_strnicmp(name, "prn", 3) ||
                      !_strnicmp(name, "aux", 3) || !_strnicmp(name, "nul", 3))) return 0;
    return !(stem == 4 && (!_strnicmp(name, "com", 3) || !_strnicmp(name, "lpt", 3)) &&
             name[3] >= '1' && name[3] <= '9');
}

static int ps_path_valid(const char *path)
{
    const char *part = path;
    size_t length = strlen(path);
    if (!length || length >= SH_PACKAGE_SOURCE_PATH_CAP) return 0;
    for (;;) {
        size_t n = strcspn(part, "/\\");
        if (!ps_name_valid(part, n)) return 0;
        if (!part[n]) return 1;
        part += n + 1;
    }
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
    if (!(algorithm = ps_sha256()) ||
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
    SRWLOCK identity_lock;
    int identity_ready;
};

int sh_package_reader_identity(void *context, uint64_t offset, uint64_t length,
    int (*read)(void *, uint64_t, void *, size_t), sh_package_file_identity *out)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char bytes[65536]; uint64_t position = 0;
    int ok = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!read || length > UINT64_MAX - offset || !(algorithm = ps_sha256()) ||
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    while (position < length) {
        size_t amount = length - position > sizeof(bytes) ? sizeof(bytes) : (size_t)(length - position);
        if (read(context, offset + position, bytes, amount) != 1 || BCryptHashData(hash, bytes, (ULONG)amount, 0) < 0) goto done;
        position += amount;
    }
    if (BCryptFinishHash(hash, out->digest, sizeof(out->digest), 0) < 0) goto done;
    out->length = length; ok = 1;
done:
    if (hash) BCryptDestroyHash(hash);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}
static int ps_identity_memory(void *context, uint64_t offset, void *out, size_t length)
{ if (length) memcpy(out, (const unsigned char *)context + (size_t)offset, length); return 1; }
int sh_package_bytes_identity(const void *body, size_t length, sh_package_file_identity *out)
{
    if (!body && length) { if (out) memset(out, 0, sizeof(*out)); return 0; }
    return sh_package_reader_identity((void *)body, 0, length, ps_identity_memory, out);
}

sh_package_file *sh_package_file_pin(const char *absolute, char *error, size_t capacity)
{
    sh_package_file *out = calloc(1, sizeof(*out));
    wchar_t *wide = sh_package_source_wide_path(absolute);
    HANDLE handle = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL) : INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info; LARGE_INTEGER size;
    free(wide);
    if (out && handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        GetFileSizeEx(handle, &size) && size.QuadPart >= 0) {
        out->handle = handle; out->references = 1; out->length = (uint64_t)size.QuadPart;
        InitializeSRWLock(&out->identity_lock);
        if (error && capacity) error[0] = 0;
        return out;
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    free(out); ps_error(error, capacity, "cannot capture original resource file: %s", absolute ? absolute : "(null)");
    return NULL;
}

sh_package_file *sh_package_file_capture(const char *absolute, char *error, size_t capacity)
{
    sh_package_file *out = sh_package_file_pin(absolute, error, capacity);
    sh_package_file_identity identity;
    if (out && !sh_package_file_get_identity(out, &identity)) {
        sh_package_file_release(out); out = NULL;
        ps_error(error, capacity, "cannot hash original resource file: %s", absolute);
    }
    return out;
}

int sh_package_file_get_identity(const sh_package_file *file, sh_package_file_identity *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!file) return 0;
    /* The pinned bytes are immutable; only their memoized digest is mutable.
     * Open an independent stream so a concurrent payload reader cannot change
     * the hashing position. Failed reads leave the identity retryable. */
    sh_package_file *mutable = (sh_package_file *)file;
    AcquireSRWLockExclusive(&mutable->identity_lock);
    if (!file->identity_ready) {
        BCRYPT_ALG_HANDLE algorithm = NULL; BCRYPT_HASH_HANDLE hash = NULL;
        uint64_t length = 0, total = 0;
        FILE *stream = sh_package_file_open(file, &length, NULL, 0);
        unsigned char chunk[65536];
        int ok = stream && (algorithm = ps_sha256()) != NULL &&
            BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0;
        while (ok && total < length) {
            size_t amount = length - total < sizeof(chunk) ? (size_t)(length - total) : sizeof(chunk);
            size_t got = fread(chunk, 1, amount, stream);
            if (got != amount || BCryptHashData(hash, chunk, (ULONG)got, 0) < 0) ok = 0;
            total += got;
        }
        if (ok && BCryptFinishHash(hash, mutable->digest, sizeof(mutable->digest), 0) >= 0)
            mutable->identity_ready = 1;
        if (hash) BCryptDestroyHash(hash);
        if (stream) fclose(stream);
    }
    int ok = file->identity_ready;
    if (ok) { out->length = file->length; memcpy(out->digest, file->digest, sizeof(out->digest)); }
    ReleaseSRWLockExclusive(&mutable->identity_lock); return ok;
}

uint64_t sh_package_file_length(const sh_package_file *file)
{ return file ? file->length : 0; }

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
        InitializeSRWLock(&out->identity_lock); out->identity_ready = 1;
        memcpy(out->digest, digest, sizeof(out->digest));
        return out;
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    free(out);
    ps_error(error, capacity, "cannot seal verified resource cache: %s", file ? file->relative : "(null)");
    return NULL;
}
typedef struct ps_output {
    HANDLE file;
    BCRYPT_HASH_HANDLE hash;
    uint64_t length, written;
    int failed;
} ps_output;

static int ps_output_write(void *context, const void *bytes, size_t length)
{
    ps_output *out = context;
    const unsigned char *at = bytes;
    if (out->failed) return 0;
    if ((length && !bytes) || length > out->length - out->written) { out->failed = 1; return 0; }
    while (length) {
        DWORD span = length > 65536 ? 65536 : (DWORD)length, written = 0;
        if (!WriteFile(out->file, at, span, &written, NULL) || written != span ||
            BCryptHashData(out->hash, (PUCHAR)at, span, 0) < 0) { out->failed = 1; return 0; }
        out->written += span; at += span; length -= span;
    }
    return 1;
}

int sh_package_file_equal(const sh_package_file *left, const sh_package_file *right)
{
    uint64_t a_length = 0, b_length = 0;
    FILE *a = sh_package_file_open(left, &a_length, NULL, 0);
    FILE *b = sh_package_file_open(right, &b_length, NULL, 0);
    unsigned char x[32768], y[32768];
    int result = !a || !b ? -1 : a_length == b_length;
    for (uint64_t at = 0; result == 1 && at < a_length;) {
        size_t n = a_length - at < sizeof(x) ? (size_t)(a_length - at) : sizeof(x);
        result = fread(x, 1, n, a) != n || fread(y, 1, n, b) != n ? -1 : !memcmp(x, y, n);
        at += n;
    }
    if (a) fclose(a);
    if (b) fclose(b);
    return result;
}

sh_package_file *sh_package_file_generate(const char *directory, uint64_t length,
    const unsigned char *expected, sh_package_file_producer produce, void *context,
    char **path, char *error, size_t capacity)
{
    static LONG serial;
    static SRWLOCK publication = SRWLOCK_INIT;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    ps_output output = {INVALID_HANDLE_VALUE, NULL, length, 0, 0};
    sh_package_file *file = NULL, *staged = NULL;
    sh_package_source_file source = {0};
    char leaf[160], hex[65], *temporary = NULL, *destination = NULL;
    wchar_t *wide = NULL, *target = NULL, *folder = NULL;
    DWORD attributes;
    int owned = 0, locked = 0;
    uint64_t collision = 0;
    if (path) *path = NULL;
    if (error && capacity) *error = 0;
    if (!path || !directory || !produce || !(folder = sh_package_source_wide_path(directory))) goto done;
    attributes = GetFileAttributesW(folder);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        attributes & FILE_ATTRIBUTE_REPARSE_POINT) goto done;
    for (;;) {
        snprintf(leaf, sizeof(leaf), ".building.%lu.%llu.%lu", GetCurrentProcessId(),
            (unsigned long long)GetTickCount64(), (unsigned long)InterlockedIncrement(&serial));
        temporary = ps_join(directory, leaf);
        wide = temporary ? sh_package_source_wide_path(temporary) : NULL;
        if (!wide) goto done;
        output.file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (output.file != INVALID_HANDLE_VALUE) { owned = 1; break; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) goto done;
        free(wide); wide = NULL; free(temporary); temporary = NULL;
    }
    if (!(algorithm = ps_sha256()) ||
        BCryptCreateHash(algorithm, &output.hash, NULL, 0, NULL, 0, 0) < 0 ||
        !produce(context, ps_output_write, &output, error, capacity) || output.failed ||
        output.written != length || BCryptFinishHash(output.hash, source.digest, 32, 0) < 0 ||
        (expected && memcmp(expected, source.digest, 32)) || !FlushFileBuffers(output.file)) goto done;
    CloseHandle(output.file); output.file = INVALID_HANDLE_VALUE;
    source.absolute = temporary; source.relative = temporary; source.length = length;
    staged = sh_package_file_seal(&source, error, capacity);
    if (!staged) goto done;
    for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", source.digest[i]);
    snprintf(leaf, sizeof(leaf), "%s", hex);
    /* Concurrent rename/open windows can transiently deny a reader and make a
     * healthy occupant appear corrupt. Serialize only publication/comparison;
     * producers still stream independently and existing readers never lock. */
    AcquireSRWLockExclusive(&publication); locked = 1;
    for (;;) {
        destination = ps_join(directory, leaf);
        target = destination ? sh_package_source_wide_path(destination) : NULL;
        if (!target) goto done;
        attributes = GetFileAttributesW(target);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (!(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                source.absolute = destination;
                file = sh_package_file_seal(&source, NULL, 0);
                if (file && sh_package_file_equal(staged, file) == 1) break;
                sh_package_file_release(file); file = NULL;
            }
            /* Never replace a pinned or mismatching cache entry. Private
             * storage disagreement is not a reason to reject healthy sources. */
            if (collision == UINT64_MAX) goto done;
            snprintf(leaf, sizeof(leaf), "%s.%llu", hex, (unsigned long long)++collision);
        } else {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) goto done;
            sh_package_file_release(staged); staged = NULL;
            if (MoveFileExW(wide, target, MOVEFILE_WRITE_THROUGH)) {
                owned = 0; source.absolute = destination;
                file = sh_package_file_seal(&source, error, capacity);
                break;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) goto done;
            /* A concurrent compiler published this name. Re-pin our source
             * before comparing rather than overwriting its publication. */
            source.absolute = temporary;
            staged = sh_package_file_seal(&source, error, capacity);
            if (!staged) goto done;
        }
        free(destination); destination = NULL; free(target); target = NULL;
    }
    if (file) { *path = destination; destination = NULL; if (error && capacity) *error = 0; }
done:
    if (locked) ReleaseSRWLockExclusive(&publication);
    if (output.file != INVALID_HANDLE_VALUE) CloseHandle(output.file);
    if (output.hash) BCryptDestroyHash(output.hash);
    sh_package_file_release(staged);
    if (owned) DeleteFileW(wide); /* Only the exact CREATE_NEW file from this call. */
    free(temporary); free(destination); free(wide); free(target); free(folder);
    if (!file && error && capacity && !*error) ps_error(error, capacity, "cannot prepare immutable resource cache output");
    return file;
}

static int ps_cache_source(void *context, int (*write)(void *, const void *, size_t),
    void *sink, char *error, size_t capacity)
{
    const sh_package_source_file *source = context;
    sh_package_file *input = sh_package_file_pin(source->absolute, error, capacity);
    uint64_t length = 0, at = 0;
    FILE *stream = input ? sh_package_file_open(input, &length, error, capacity) : NULL;
    unsigned char buffer[65536];
    int ok = stream && length == source->length;
    while (ok && at < length) {
        size_t n = length - at < sizeof(buffer) ? (size_t)(length - at) : sizeof(buffer);
        ok = fread(buffer, 1, n, stream) == n && write(sink, buffer, n); at += n;
    }
    if (stream) fclose(stream);
    sh_package_file_release(input); return ok;
}

sh_package_file *sh_package_source_cache(const sh_package_source_file *source,
    const char *directory, char **path, char *error, size_t capacity)
{
    char digest[65];
    if (path) *path = NULL;
    if (!source || source->directory || !directory || !path) {
        ps_error(error, capacity, "invalid resource cache source"); return NULL;
    }
    /* Preserve the ordinary source-cache fast path. The scan already fixed
     * this source identity; a verified cached snapshot need not be recopied. */
    for (size_t i = 0; i < 32; ++i) snprintf(digest + i * 2, 3, "%02x", source->digest[i]);
    char *cached_path = ps_join(directory, digest);
    if (cached_path) {
        sh_package_source_file cached = *source;
        cached.absolute = cached_path;
        sh_package_file *file = sh_package_file_seal(&cached, NULL, 0);
        if (file) { *path = cached_path; if (error && capacity) *error = 0; return file; }
        free(cached_path);
    }
    return sh_package_file_generate(directory, source->length, source->digest,
        ps_cache_source, (void *)source, path, error, capacity);
}

sh_package_file *sh_package_file_retain(sh_package_file *file)
{ if (file) InterlockedIncrement(&file->references); return file; }
void sh_package_file_release(sh_package_file *file)
{
    if (file && !InterlockedDecrement(&file->references)) { CloseHandle(file->handle); free(file); }
}

struct sh_package_body {
    volatile LONG references;
    size_t length;
    unsigned char *bytes; /* length bytes and a NUL; NULL while released to the backing */
    /* Backed bodies only (backing.read set): the holders that read through the body, the lock that orders
     * releasing and reading the bytes again, and whether a backing read ever failed. */
    sh_package_backing backing;
    LONG detached;
    SRWLOCK lock;
    volatile LONG failed;
};
#if defined(SH_PACKAGE_LIBRARY_TESTING) || defined(SH_PACKAGE_SOURCE_LINK_TESTING) || \
    defined(SH_PACKAGE_RUNTIME_TESTING)
static volatile LONG g_ps_body_refusal, g_ps_body_live, g_ps_backing_failure, g_ps_backing_reads, g_ps_released,
    g_ps_releases;
void sh_package_body_test_refusal(long nth) { g_ps_body_refusal = nth; }
long sh_package_body_test_refusal_left(void) { return g_ps_body_refusal; }
long sh_package_body_test_live(void) { return g_ps_body_live; }
void sh_package_body_test_backing_failure(long nth) { g_ps_backing_failure = nth; }
long sh_package_body_test_backing_failure_left(void) { return g_ps_backing_failure; }
long sh_package_body_test_backing_reads(void) { return g_ps_backing_reads; }
long sh_package_body_test_released(void) { return g_ps_released; }
long sh_package_body_test_releases(void) { return g_ps_releases; }
#define PS_BODY_REFUSED() (g_ps_body_refusal > 0 && !InterlockedDecrement(&g_ps_body_refusal))
#define PS_BODY_LIVE(step) InterlockedExchangeAdd(&g_ps_body_live, (step))
#define PS_BACKING_FAILED() (InterlockedIncrement(&g_ps_backing_reads), \
    g_ps_backing_failure > 0 && !InterlockedDecrement(&g_ps_backing_failure))
#define PS_RELEASED(step) (InterlockedExchangeAdd(&g_ps_released, (step)), \
    (step) > 0 ? InterlockedIncrement(&g_ps_releases) : 0)
#else
#define PS_BODY_REFUSED() 0
#define PS_BODY_LIVE(step) ((void)0)
#define PS_BACKING_FAILED() 0
#define PS_RELEASED(step) ((void)0)
#endif
sh_package_body *sh_package_body_adopt(unsigned char *bytes, size_t length)
{
    if (!bytes || length == SIZE_MAX || PS_BODY_REFUSED()) return NULL;
    sh_package_body *body = calloc(1, sizeof(*body));
    if (!body) return NULL;
    unsigned char *ended = realloc(bytes, length + 1);
    if (!ended) { free(body); return NULL; }
    ended[length] = 0;
    body->references = 1; body->length = length; body->bytes = ended;
    InitializeSRWLock(&body->lock);
    PS_BODY_LIVE(1);
    return body;
}
sh_package_body *sh_package_body_retain(sh_package_body *body)
{ if (body) InterlockedIncrement(&body->references); return body; }
/* Under the lock: a backed body whose every holder is detached lets its bytes go. */
static void ps_body_settle(sh_package_body *body)
{
    if (body->bytes && body->backing.read && body->detached == body->references) {
        free(body->bytes); body->bytes = NULL; PS_RELEASED(1);
    }
}
void sh_package_body_release(sh_package_body *body)
{
    if (!body) return;
    /* A body's backing is given before any other thread holds it and never changes. Unbacked bodies release as
     * they always did; a backed one decides under its lock, which a concurrent last release also takes before it
     * frees the body, so nothing here touches a body another thread has freed. */
    if (!body->backing.read) {
        if (!InterlockedDecrement(&body->references)) { free(body->bytes); free(body); PS_BODY_LIVE(-1); }
        return;
    }
    AcquireSRWLockExclusive(&body->lock);
    LONG left = InterlockedDecrement(&body->references);
    if (left) ps_body_settle(body);
    ReleaseSRWLockExclusive(&body->lock);
    if (left) return;
    if (!body->bytes) PS_RELEASED(-1);
    free(body->bytes);
    if (body->backing.release) body->backing.release(body->backing.context);
    free(body); PS_BODY_LIVE(-1);
}
const unsigned char *sh_package_body_bytes(const sh_package_body *body) { return body ? body->bytes : NULL; }
size_t sh_package_body_length(const sh_package_body *body) { return body ? body->length : 0; }
int sh_package_body_back(sh_package_body *body, sh_package_backing backing)
{
    if (!body || !backing.read || body->backing.read || backing.length != body->length) {
        if (backing.release) backing.release(backing.context);
        return 0;
    }
    AcquireSRWLockExclusive(&body->lock); body->backing = backing; ps_body_settle(body); ReleaseSRWLockExclusive(&body->lock);
    return 1;
}
int sh_package_body_backed(const sh_package_body *body) { return body && body->backing.read; }
/* A backing never changes once given (sh_package_body_release), so no lock is needed to compare two. */
int sh_package_body_same_backing(const sh_package_body *body, const sh_package_body *other)
{
    return body && other && body->backing.read && body->backing.same && body->backing.read == other->backing.read &&
        body->backing.same == other->backing.same && body->length == other->length &&
        (body == other || body->backing.same(body->backing.context, other->backing.context) == 1);
}
int sh_package_body_failed(const sh_package_body *body) { return body && body->failed; }
/* Under the lock: the backing's bytes, exactly the body's length, or NULL with the failure recorded. */
static unsigned char *ps_body_reread(sh_package_body *body)
{
    size_t length = 0;
    unsigned char *bytes = PS_BACKING_FAILED() ? NULL : body->backing.read(body->backing.context, &length);
    if (bytes && length == body->length) return bytes;
    free(bytes); InterlockedExchange(&body->failed, 1); return NULL;
}
void sh_package_body_detach(sh_package_body *body)
{
    if (!body) return;
    AcquireSRWLockExclusive(&body->lock); body->detached++; ps_body_settle(body); ReleaseSRWLockExclusive(&body->lock);
}
const unsigned char *sh_package_body_attach(sh_package_body *body)
{
    const unsigned char *bytes = NULL;
    if (!body) return NULL;
    AcquireSRWLockExclusive(&body->lock);
    if (body->detached > 0) {
        if (!body->bytes && body->backing.read && (body->bytes = ps_body_reread(body)) != NULL) PS_RELEASED(-1);
        if (body->bytes) { bytes = body->bytes; body->detached--; }
    }
    ReleaseSRWLockExclusive(&body->lock);
    return bytes;
}
sh_package_body *sh_package_body_retain_detached(sh_package_body *body)
{
    if (!body) return NULL;
    AcquireSRWLockExclusive(&body->lock);
    InterlockedIncrement(&body->references); body->detached++;
    ReleaseSRWLockExclusive(&body->lock);
    return body;
}
void sh_package_body_drop(sh_package_body *body, const void *held)
{
    if (!body) return;
    if (!held) { AcquireSRWLockExclusive(&body->lock); body->detached--; ReleaseSRWLockExclusive(&body->lock); }
    sh_package_body_release(body);
}
unsigned char *sh_package_body_read(sh_package_body *body, size_t *length)
{
    unsigned char *out = NULL;
    if (length) *length = 0;
    if (!body || !length || body->length == SIZE_MAX) return NULL;
    AcquireSRWLockExclusive(&body->lock);
    if (body->bytes) {
        if ((out = malloc(body->length + 1)) != NULL) { memcpy(out, body->bytes, body->length); out[body->length] = 0; }
    } else if (body->backing.read) out = ps_body_reread(body);
    ReleaseSRWLockExclusive(&body->lock);
    if (out) *length = body->length;
    return out;
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
    /* The JSON reader reports a failed allocation like invalid text. The C
     * runtime allocators record that failure, and it is not the package's. */
    errno = 0;
    if (!sh_package_descriptor_parse((const char *)body, (size_t)length,
                                     &component->descriptor, scan->error, scan->error_capacity)) {
        scan->content = errno != ENOMEM; goto done;
    }
    memcpy(component->descriptor_digest, digest, sizeof(digest));
    ok = 1;
done:
    if (!ok && scan->error && scan->error_capacity && !scan->error[0])
        ps_error(scan->error, scan->error_capacity, "cannot read package descriptor: %s", marker ? marker : root);
    free(marker); free(body); return ok;
}

static const char *ps_local(const sh_package_component *component, const char *relative)
{
    const char *local = relative + strlen(component->relative);
    return *local == '/' ? local + 1 : local;
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
        char *name = NULL, *path = NULL, *rel = NULL;
        sh_package_source_file *file;
        const char *local;
        int entry_ok = 0, converted;
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        converted = ps_utf8(found.cFileName, &name);
        if (converted <= 0) {
            if (!converted) ps_refuse(scan, "a file name is not valid Unicode in %s", root);
            goto entry_done;
        }
        if (!ps_name_valid(name, strlen(name))) {
            ps_refuse(scan, "name is not usable in a package: %s/%s", root, name); goto entry_done;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            ps_refuse(scan, "links and reparse points are not supported in packages: %s/%s", root, name); goto entry_done;
        }
        if (strlen(root) + strlen(name) + 2u >= SH_PACKAGE_SOURCE_PATH_CAP) {
            ps_refuse(scan, "package path is too long: %s/%s", root, name); goto entry_done;
        }
        if (!(path = ps_join(root, name)) || !(rel = ps_join(relative, name))) goto entry_done;
        if (!ps_grow((void **)&scan->out->files, &scan->file_capacity, scan->out->file_count,
                      sizeof(*file))) goto entry_done;
        file = &scan->out->files[scan->out->file_count++];
        file->owner = owner; file->component = component_index;
        file->relative = rel; rel = NULL; file->absolute = path; path = NULL;
        file->directory = !!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (file->directory) {
            entry_ok = ps_enqueue(pending, component_index, file->absolute, file->relative);
        } else {
            local = ps_local(&scan->out->components[component_index], file->relative);
            if (!_strnicmp(local, "assets/", 7)) {
                if (!ps_path_valid(local + 7)) {
                    ps_refuse(scan, "engine path is not usable in a package: %s", file->relative); goto entry_done;
                }
                if (!(file->engine_path = sh_package_engine_path(local + 7))) goto entry_done;
            }
            entry_ok = ps_read(file->absolute, NULL, 0, &file->length, file->digest);
            if (!entry_ok && scan->error && scan->error_capacity && !scan->error[0])
                ps_error(scan->error, scan->error_capacity, "cannot read package source: %s", file->absolute);
            if (entry_ok && !_stricmp(local, "package.json") &&
                memcmp(file->digest, scan->out->components[component_index].descriptor_digest, 32)) {
                ps_error(scan->error, scan->error_capacity, "package descriptor changed during the scan: %s", file->absolute);
                entry_ok = 0;
            }
        }
entry_done:
        if (!entry_ok && scan->error && scan->error_capacity && !scan->error[0])
            ps_error(scan->error, scan->error_capacity, "cannot record package source in %s", root);
        free(name); free(path); free(rel);
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
                ok = (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ?
                    ps_refuse(scan, "package.json must be a regular file: %s", current.root) :
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
    if (!(algorithm = ps_sha256())) return 0;
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
    return ok;
}

static sh_package_sources *ps_inventory(const char *data_root, const char *single_root,
                                        char *error, size_t error_capacity, int *content)
{
    sh_package_sources *out = (sh_package_sources *)calloc(1, sizeof(*out));
    ps_scan scan = {out, 0, 0, error, error_capacity, 0};
    size_t i;
    if (error && error_capacity) error[0] = 0;
    if (content) *content = 0;
    if (!out) goto bad;
    if (single_root) {
        out->packages = (sh_package *)calloc(1, sizeof(sh_package));
        if (!out->packages) goto bad;
        if (strlen(single_root) >= sizeof(out->packages[0].root)) {
            ps_refuse(&scan, "package path is too long: %s", single_root); goto bad;
        }
        strcpy_s(out->packages[0].root, sizeof(out->packages[0].root), single_root);
        out->package_count = 1;
    } else if (!sh_packages_enumerate(data_root, &out->packages, &out->package_count)) {
        ps_error(error, error_capacity, "package directory enumeration failed"); goto bad;
    }
    for (i = 0; i < out->package_count; i++) {
        size_t component;
        if (out->packages[i].problem) {
            ps_refuse(&scan, "%s: %s", out->packages[i].root, out->packages[i].problem); goto bad;
        }
        if (!ps_component(&scan, i, "", out->packages[i].root, &component) ||
            !ps_walk(&scan, i, component, out->packages[i].root, "")) goto bad;
        if (single_root && strcpy_s(out->packages[i].name, sizeof(out->packages[i].name),
                                    out->components[component].descriptor.id)) goto bad;
    }
    if (out->file_count) qsort(out->files, out->file_count, sizeof(*out->files), ps_file_compare);
    for (i = 1; i < out->file_count; i++) if (out->files[i - 1].owner == out->files[i].owner &&
        !_stricmp(out->files[i - 1].relative, out->files[i].relative)) {
        ps_refuse(&scan, "case-insensitive duplicate source path: %s", out->files[i].relative); goto bad;
    }
    if (!ps_fingerprints(out)) goto bad;
    return out;
bad:
    if (error && error_capacity && !error[0]) ps_error(error, error_capacity, "package inventory allocation or hashing failed");
    if (content) *content = scan.content;
    sh_package_sources_free(out); return NULL;
}

sh_package_sources *sh_package_sources_scan(const char *data_root, char *error, size_t capacity)
{ return ps_inventory(data_root, NULL, error, capacity, NULL); }

sh_package_sources *sh_package_sources_scan_directory(const char *root, char *error, size_t capacity)
{ return root && *root ? ps_inventory(NULL, root, error, capacity, NULL) : NULL; }

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
        int content = 0;
        const char *reason = packages[i].problem;
        if (!reason) {
            unit = ps_inventory(NULL, packages[i].root, detail, sizeof(detail), &content);
            if (!unit && !content) {
                /* Storage, memory and concurrent changes are not package
                 * defects; publishing the remaining units would hide them. */
                ps_error(error, capacity, "local package scan failed: %s: %s", packages[i].name, detail); goto bad;
            }
            reason = detail;
        }
        if (!unit) {
            if (!rejected(context, &packages[i], reason)) {
                ps_error(error, capacity, "cannot record the skipped local package %s", packages[i].name); goto bad;
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
    if (sources->original_fingerprints) memmove(sources->original_fingerprints + package,
        sources->original_fingerprints + package + 1,
        (sources->package_count - package - 1) * sizeof(*sources->original_fingerprints));
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
    if (local->original_fingerprints || map->original_fingerprints) {
        out->original_fingerprints = calloc(packages ? packages : 1, sizeof(*out->original_fingerprints));
        if (!out->original_fingerprints) goto bad;
    }
    out->components = calloc(components ? components : 1, sizeof(*out->components));
    out->files = calloc(files ? files : 1, sizeof(*out->files));
    if (!out->packages || !out->fingerprints || !out->components || !out->files) goto bad;
    for (side = 0; side < 2; side++) {
        const sh_package_sources *in = inputs[side];
        size_t owner_offset = out->package_count, component_offset = out->component_count;
        if (in->package_count) {
            memcpy(out->packages + owner_offset, in->packages, in->package_count * sizeof(*in->packages));
            memcpy(out->fingerprints + owner_offset, in->fingerprints, in->package_count * sizeof(*in->fingerprints));
            if (out->original_fingerprints) memcpy(out->original_fingerprints + owner_offset,
                in->original_fingerprints ? in->original_fingerprints : in->fingerprints,
                in->package_count * sizeof(*out->original_fingerprints));
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
    free(sources->components); free(sources->files); free(sources->packages);
    free(sources->fingerprints); free(sources->original_fingerprints); free(sources);
}
