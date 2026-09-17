#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "package_archive.h"
#include "raw_deflate_encode.h"
#include "raw_deflate.h"

typedef struct pa_entry {
    char *name, *key;
    unsigned crc, packed, size, method;
    size_t offset;
    int directory;
    unsigned char digest[32];
    const unsigned char *data;
} pa_entry;
typedef struct pa_zip {
    pa_entry *entries;
    size_t count;
    unsigned files;
    uint64_t expanded;
    char id[SH_PACKAGE_ID_CAP];
    unsigned char fingerprint[32];
} pa_zip;
typedef struct pa_buffer { unsigned char *data; size_t size, capacity; } pa_buffer;

static int pa_error(char *out, size_t capacity, const char *format, ...)
{
    va_list args;
    if (out && capacity) { va_start(args, format); vsnprintf(out, capacity, format, args); va_end(args); }
    return 0;
}
static unsigned pa_r16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned pa_r32(const unsigned char *p) { return pa_r16(p) | (pa_r16(p + 2) << 16); }
static uint64_t pa_r64(const unsigned char *p) { return pa_r32(p) | ((uint64_t)pa_r32(p + 4) << 32); }
static void pa_w16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void pa_w32(unsigned char *p, unsigned v) { pa_w16(p, v); pa_w16(p + 2, v >> 16); }
static void pa_w64(unsigned char *p, uint64_t v) { pa_w32(p, (unsigned)v); pa_w32(p + 4, (unsigned)(v >> 32)); }
static unsigned pa_crc(const unsigned char *p, size_t n)
{
    unsigned table[256], c = 0xffffffffu, i, j;
    for (i = 0; i < 256; i++) {
        unsigned v = i;
        for (j = 0; j < 8; j++) v = (v >> 1) ^ ((v & 1) ? 0xedb88320u : 0);
        table[i] = v;
    }
    while (n--) c = table[(c ^ *p++) & 255u] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
static unsigned char *pa_append(pa_buffer *b, size_t n)
{
    unsigned char *p;
    size_t cap;
    if (n > SIZE_MAX - b->size) return NULL;
    if (b->size + n > b->capacity) {
        cap = b->capacity > SIZE_MAX / 2u ? SIZE_MAX : b->capacity ? b->capacity * 2u : 65536u;
        if (cap < b->size + n) cap = b->size + n;
        p = (unsigned char *)realloc(b->data, cap);
        if (!p) return NULL;
        b->data = p; b->capacity = cap;
    }
    p = b->data + b->size; memset(p, 0, n); b->size += n; return p;
}
static void pa_free(pa_zip *z)
{
    size_t i;
    for (i = 0; i < z->count; i++) { free(z->entries[i].name); free(z->entries[i].key); }
    free(z->entries); memset(z, 0, sizeof(*z));
}

static unsigned char *pa_pack(const sh_package_sources *s, size_t owner,
    const sh_package_archive_files *files, size_t *length, char *error, size_t capacity)
{
    pa_zip z = {0}; pa_buffer b = {0};
    size_t i, cd, cd_size;
    unsigned char *raw = NULL, *deflated = NULL, *p;
    if (length) *length = 0;
    if (error && capacity) error[0] = 0;
    size_t count = files ? files->count : s ? s->file_count : 0;
    if ((!files && (!s || owner >= s->package_count)) || !length) goto bad;
    z.entries = (pa_entry *)calloc(count ? count : 1, sizeof(pa_entry));
    if (!z.entries) goto bad;
    for (i = 0; i < count; i++) {
        sh_package_source_file memory = {0};
        const sh_package_source_file *f;
        pa_entry *e;
        size_t n, got = 0, packed = 0;
        if (files) {
            memory.relative = files->items[i].name; memory.length = files->items[i].length;
            memory.directory = files->items[i].directory; memory.owner = owner;
            f = &memory;
        } else f = &s->files[i];
        if (f->owner != owner) continue;
        if (f->length > SH_PACKAGE_ARCHIVE_MAX_FILE_BYTES) goto bad;
        e = &z.entries[z.count++]; n = strlen(f->relative) + (f->directory ? 1u : 0u);
        if (n > 65535u || !(e->name = (char *)malloc(n + 1))) goto bad;
        strcpy_s(e->name, n + 1, f->relative);
        if (f->directory) strcat_s(e->name, n + 1, "/");
        e->directory = f->directory; e->offset = b.size;
        if (!f->directory) {
            if (files) {
                got = (size_t)f->length; raw = malloc(got + 1);
                if (raw) { if (got) memcpy(raw, files->items[i].body, got); raw[got] = 0; }
            } else raw = sh_package_source_read(f, SH_PACKAGE_ARCHIVE_MAX_FILE_BYTES, &got, error, capacity);
            if (!raw) goto bad;
            deflated = sh_deflate_raw(raw, got, &packed);
            if (!deflated) goto bad;
            e->crc = pa_crc(raw, got); e->size = (unsigned)got;
            e->method = packed < got ? 8 : 0;
            e->packed = (unsigned)(e->method ? packed : got);
        }
        p = pa_append(&b, 30 + n + e->packed);
        if (!p) goto bad;
        pa_w32(p, 0x04034b50u); pa_w16(p + 4, 20); pa_w16(p + 6, 0x800);
        pa_w16(p + 8, e->method); pa_w16(p + 12, 0x21); pa_w32(p + 14, e->crc);
        pa_w32(p + 18, e->packed); pa_w32(p + 22, e->size); pa_w16(p + 26, (unsigned)n);
        memcpy(p + 30, e->name, n);
        if (e->packed) memcpy(p + 30 + n, e->method ? deflated : raw, e->packed);
        free(raw); raw = NULL; free(deflated); deflated = NULL;
    }
    cd = b.size;
    for (i = 0; i < z.count; i++) {
        pa_entry *e = &z.entries[i]; size_t n = strlen(e->name);
        unsigned extra = e->offset >= UINT32_MAX ? 12u : 0u;
        p = pa_append(&b, 46 + n + extra); if (!p) goto bad;
        pa_w32(p, 0x02014b50u); pa_w16(p + 4, extra ? 45 : 20); pa_w16(p + 6, extra ? 45 : 20); pa_w16(p + 8, 0x800);
        pa_w16(p + 10, e->method); pa_w16(p + 14, 0x21); pa_w32(p + 16, e->crc);
        pa_w32(p + 20, e->packed); pa_w32(p + 24, e->size); pa_w16(p + 28, (unsigned)n);
        pa_w16(p + 30, extra);
        pa_w32(p + 38, e->directory ? 0x10u : 0); pa_w32(p + 42, extra ? UINT32_MAX : (unsigned)e->offset);
        memcpy(p + 46, e->name, n);
        if (extra) { pa_w16(p + 46 + n, 1); pa_w16(p + 48 + n, 8); pa_w64(p + 50 + n, e->offset); }
    }
    cd_size = b.size - cd;
    if (z.count >= 65535u || cd >= UINT32_MAX || cd_size >= UINT32_MAX) {
        size_t offset = b.size;
        p = pa_append(&b, 76); if (!p) goto bad;
        pa_w32(p, 0x06064b50u); pa_w64(p + 4, 44);
        pa_w16(p + 12, 45); pa_w16(p + 14, 45);
        pa_w64(p + 24, z.count); pa_w64(p + 32, z.count);
        pa_w64(p + 40, cd_size); pa_w64(p + 48, cd);
        pa_w32(p + 56, 0x07064b50u); pa_w64(p + 64, offset); pa_w32(p + 72, 1);
    }
    p = pa_append(&b, 22); if (!p) goto bad;
    pa_w32(p, 0x06054b50u);
    pa_w16(p + 8, z.count >= 65535u ? 65535u : (unsigned)z.count);
    pa_w16(p + 10, z.count >= 65535u ? 65535u : (unsigned)z.count);
    pa_w32(p + 12, cd_size >= UINT32_MAX ? UINT32_MAX : (unsigned)cd_size);
    pa_w32(p + 16, cd >= UINT32_MAX ? UINT32_MAX : (unsigned)cd);
    pa_free(&z); *length = b.size; return b.data;
bad:
    if (error && capacity && !error[0]) pa_error(error, capacity, "package exceeds archive limits or cannot be read completely");
    free(raw); free(deflated); pa_free(&z); free(b.data); return NULL;
}

unsigned char *sh_package_archive_pack(const sh_package_sources *s, size_t owner,
    size_t *length, char *error, size_t capacity)
{ return pa_pack(s, owner, NULL, length, error, capacity); }

static unsigned char *pa_decode(const pa_entry *e)
{
    unsigned char *out = (unsigned char *)malloc((size_t)e->size + 1);
    if (!out) return NULL;
    if (!e->method) memcpy(out, e->data, e->size);
    else if (!sh_inflate_raw_exact(e->data, e->packed, out, e->size)) { free(out); return NULL; }
    if (pa_crc(out, e->size) != e->crc) { free(out); return NULL; }
    out[e->size] = 0; return out;
}
static int pa_key_order(const void *a, const void *b)
{ return strcmp((*(const pa_entry *const *)a)->key, (*(const pa_entry *const *)b)->key); }

typedef struct pa_tree_member {
    char *name;
    const unsigned char *digest; /* NULL for a directory */
    int allocated;
} pa_tree_member;

static int pa_tree_order(const void *a, const void *b)
{
    const char *left = ((const pa_tree_member *)a)->name, *right = ((const pa_tree_member *)b)->name;
    int ci = _stricmp(left, right);
    return ci ? ci : strcmp(left, right);
}

static int pa_fingerprint(pa_zip *z, BCRYPT_ALG_HANDLE algorithm)
{
    pa_tree_member *tree = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    size_t count = z->count, used = 0, i;
    int ok = 0;
    /* Materialize the same tree that extraction creates. Empty directories
     * remain significant; ZIP tools need not list nonempty parents explicitly. */
    for (i = 0; i < z->count; i++) {
        const char *p;
        for (p = z->entries[i].name; *p; p++) if (*p == '/') {
            if (count == SIZE_MAX) goto done;
            count++;
        }
    }
    if (count > SIZE_MAX / sizeof(*tree) || !(tree = (pa_tree_member *)calloc(count, sizeof(*tree)))) goto done;
    for (i = 0; i < z->count; i++) {
        pa_entry *e = &z->entries[i];
        const char *p;
        tree[used].name = e->name; tree[used++].digest = e->directory ? NULL : e->digest;
        for (p = e->name; *p; p++) if (*p == '/') {
            size_t n = (size_t)(p - e->name);
            tree[used].name = (char *)malloc(n + 1);
            if (!tree[used].name) goto done;
            memcpy(tree[used].name, e->name, n); tree[used].name[n] = 0; tree[used++].allocated = 1;
        }
    }
    qsort(tree, used, sizeof(*tree), pa_tree_order);
    if (BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    for (i = 0; i < used; i++) {
        unsigned char kind = tree[i].digest ? 'f' : 'd';
        if (i && !_stricmp(tree[i - 1].name, tree[i].name)) {
            /* Windows cannot faithfully reconstruct two spellings of one
             * directory, even if neither spelling has an explicit ZIP row. */
            if (strcmp(tree[i - 1].name, tree[i].name) || tree[i - 1].digest || tree[i].digest) goto done;
            continue;
        }
        if (BCryptHashData(hash, &kind, 1, 0) < 0 ||
            BCryptHashData(hash, (PUCHAR)tree[i].name, (ULONG)strlen(tree[i].name) + 1u, 0) < 0 ||
            (tree[i].digest && BCryptHashData(hash, (PUCHAR)tree[i].digest, 32, 0) < 0)) goto done;
    }
    ok = BCryptFinishHash(hash, z->fingerprint, 32, 0) >= 0;
done:
    if (hash) BCryptDestroyHash(hash);
    for (i = 0; i < used; i++) if (tree[i].allocated) free(tree[i].name);
    free(tree); return ok;
}

/* Resolve the standard end records before allocating any per-member state. */
static int pa_directory_record(const unsigned char *bytes, size_t length,
                                size_t *count, size_t *cd, const unsigned char **end)
{
    const unsigned char *classic, *limit;
    uint64_t entries, offset, size;
    if (!bytes || length < 22) return 0;
    classic = bytes + length - 22; limit = classic;
    if (pa_r32(classic) != 0x06054b50u || pa_r16(classic + 4) ||
        pa_r16(classic + 6) || pa_r16(classic + 20) ||
        pa_r16(classic + 8) != pa_r16(classic + 10)) return 0;
    entries = pa_r16(classic + 10); offset = pa_r32(classic + 16); size = pa_r32(classic + 12);
    if (length >= 42 && pa_r32(classic - 20) == 0x07064b50u) {
        const unsigned char *locator = classic - 20, *record;
        uint64_t at = pa_r64(locator + 8);
        size_t available = length - 42;
        if (pa_r32(locator + 4) || pa_r32(locator + 16) != 1 ||
            at > available || available - (size_t)at < 56) return 0;
        record = bytes + (size_t)at;
        if (pa_r32(record) != 0x06064b50u || pa_r64(record + 4) != available - (size_t)at - 12 ||
            pa_r16(record + 14) != 45 || pa_r32(record + 16) || pa_r32(record + 20) ||
            pa_r64(record + 24) != pa_r64(record + 32)) return 0;
        entries = pa_r64(record + 32); size = pa_r64(record + 40); offset = pa_r64(record + 48);
        if ((pa_r16(classic + 10) != 65535u && pa_r16(classic + 10) != entries) ||
            (pa_r32(classic + 12) != UINT32_MAX && pa_r32(classic + 12) != size) ||
            (pa_r32(classic + 16) != UINT32_MAX && pa_r32(classic + 16) != offset)) return 0;
        limit = record;
    }
    if (!entries || offset > (uint64_t)(limit - bytes) ||
        size != (uint64_t)(limit - bytes) - offset || entries > size / 46u ||
        entries > SIZE_MAX / sizeof(pa_entry) || entries > SIZE_MAX / sizeof(pa_entry *)) return 0;
    *count = (size_t)entries; *cd = (size_t)offset; *end = limit; return 1;
}

/* ZIP64 values occur in size, packed-size, offset, disk order, only for the
 * corresponding sentinel fields. Unknown extras remain bounded and ignored. */
static int pa_zip64_extra(const unsigned char *extra, size_t length,
                           uint64_t *size, uint64_t *packed, uint64_t *offset, uint64_t *disk)
{
    uint64_t *values[] = {size, packed, offset, disk};
    unsigned needed = 0, found = 0, i;
    for (i = 0; i < 4; i++) if (*values[i] == (i == 3 ? 65535u : UINT32_MAX)) needed |= 1u << i;
    while (length) {
        unsigned tag, n;
        const unsigned char *field;
        size_t left;
        if (length < 4) return 0;
        tag = pa_r16(extra); n = pa_r16(extra + 2);
        if (n > length - 4u) return 0;
        field = extra + 4; left = n;
        if (tag == 1) {
            if (found++) return 0;
            for (i = 0; i < 4; i++) if (needed & (1u << i)) {
                size_t width = i == 3 ? 4u : 8u;
                if (left < width) return 0;
                *values[i] = i == 3 ? pa_r32(field) : pa_r64(field);
                field += width; left -= width;
            }
            if (left) return 0;
        }
        extra += 4u + n; length -= 4u + n;
    }
    return !needed || found;
}

static int pa_open(const unsigned char *bytes, size_t length, pa_zip *z,
    int descriptor_required, char *error, size_t capacity)
{
    const unsigned char *end, *c;
    size_t count, cd, i, local_end = 0;
    pa_entry **ordered = NULL;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    int marker = 0, ok = 0;
    memset(z, 0, sizeof(*z));
    if (!pa_directory_record(bytes, length, &count, &cd, &end)) goto done;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) goto done;
    z->entries = (pa_entry *)calloc(count, sizeof(pa_entry));
    ordered = (pa_entry **)calloc(count, sizeof(*ordered));
    if (!z->entries || !ordered) goto done;
    z->count = count; c = bytes + cd;
    for (i = 0; i < count; i++) {
        pa_entry *e = &z->entries[i];
        const unsigned char *lh;
        size_t n, extras, data_off;
        uint64_t size, packed, offset, disk, local_size, local_packed, unused_offset = 0, unused_disk = 0;
        unsigned char *decoded;
        if ((size_t)(end - c) < 46 || pa_r32(c) != 0x02014b50u || (pa_r16(c + 8) & ~0x800u)) goto done;
        n = pa_r16(c + 28); extras = (size_t)pa_r16(c + 30) + pa_r16(c + 32);
        if (!n || n >= SH_PACKAGE_SOURCE_PATH_CAP || n + extras > (size_t)(end - c) - 46) goto done;
        e->method = pa_r16(c + 10); e->crc = pa_r32(c + 16);
        packed = pa_r32(c + 20); size = pa_r32(c + 24); offset = pa_r32(c + 42); disk = pa_r16(c + 34);
        if (!pa_zip64_extra(c + 46 + n, pa_r16(c + 30), &size, &packed, &offset, &disk) ||
            disk || size > SH_PACKAGE_ARCHIVE_MAX_FILE_BYTES || packed > UINT32_MAX || offset > cd) goto done;
        e->size = (unsigned)size; e->packed = (unsigned)packed; e->offset = (size_t)offset;
        if ((e->method != 0 && e->method != 8) || (!e->method && e->packed != e->size) ||
            e->size > SH_PACKAGE_ARCHIVE_MAX_FILE_BYTES || e->offset != local_end ||
            e->offset > cd || cd - e->offset < 30) goto done;
        if (e->size > UINT64_MAX - z->expanded) goto done;
        z->expanded += e->size;
        e->name = (char *)malloc(n + 1); if (!e->name) goto done;
        memcpy(e->name, c + 46, n); e->name[n] = 0;
        if (strlen(e->name) != n || strchr(e->name, '\\')) goto done;
        e->directory = e->name[n - 1] == '/';
        if (e->directory) { if (e->size || e->packed || n == 1) goto done; e->name[n - 1] = 0; }
        e->key = sh_package_engine_path(e->name);
        if (!e->key) { pa_error(error, capacity, "unsafe member path in package: %s", e->name); goto done; }
        /* Reject invalid UTF-8 before touching disk. */
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, e->name, -1, NULL, 0)) goto done;
        lh = bytes + e->offset;
        if (pa_r32(lh) != 0x04034b50u || pa_r16(lh + 6) != pa_r16(c + 8) ||
            pa_r16(lh + 8) != e->method || pa_r32(lh + 14) != e->crc ||
            pa_r16(lh + 26) != n || n + (size_t)pa_r16(lh + 28) > cd - e->offset - 30) goto done;
        local_packed = pa_r32(lh + 18); local_size = pa_r32(lh + 22);
        if ((local_size == UINT32_MAX) != (local_packed == UINT32_MAX) ||
            !pa_zip64_extra(lh + 30 + n, pa_r16(lh + 28), &local_size, &local_packed, &unused_offset, &unused_disk) ||
            local_packed != e->packed || local_size != e->size) goto done;
        data_off = e->offset + 30 + n + pa_r16(lh + 28);
        if (e->packed > cd - data_off || memcmp(lh + 30, c + 46, n)) goto done;
        e->data = bytes + data_off; local_end = data_off + e->packed;
        c += 46 + n + extras; ordered[i] = e;
        if (e->directory) continue;
        decoded = pa_decode(e);
        if (!decoded) { pa_error(error, capacity, "package member failed decompression or CRC: %s", e->name); goto done; }
        if (BCryptHash(algorithm, NULL, 0, decoded, e->size, e->digest, 32) < 0) { free(decoded); goto done; }
        if (!strcmp(e->name, "package.json")) {
            sh_package_descriptor descriptor;
            marker = !descriptor_required || sh_package_descriptor_parse((char *)decoded, e->size, &descriptor, error, capacity);
            if (marker && descriptor_required) { strcpy_s(z->id, sizeof(z->id), descriptor.id); sh_package_descriptor_free(&descriptor); }
            if (!marker) { free(decoded); goto done; }
        }
        free(decoded);
        if (z->files == UINT_MAX) goto done;
        z->files++;
    }
    if (!marker || c != end || local_end != cd) goto done;
    qsort(ordered, count, sizeof(*ordered), pa_key_order);
    for (i = 0; i < count; i++) {
        pa_entry probe = {0}, *key = &probe;
        char *part;
        if (i && !strcmp(ordered[i - 1]->key, ordered[i]->key)) goto done;
        probe.key = _strdup(ordered[i]->key); if (!probe.key) goto done;
        for (part = probe.key; *part; part++) if (*part == '/') {
            pa_entry **found;
            *part = 0; found = (pa_entry **)bsearch(&key, ordered, count, sizeof(*ordered), pa_key_order);
            *part = '/';
            if (found && !(*found)->directory) { free(probe.key); goto done; }
        }
        free(probe.key);
    }
    ok = pa_fingerprint(z, algorithm);
done:
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    free(ordered);
    if (!ok) { if (error && capacity && !error[0]) pa_error(error, capacity, "invalid package archive, descriptor, or duplicate member path"); pa_free(z); }
    return ok;
}

int sh_package_archive_inspect(const unsigned char *bytes, size_t length,
                               char id[SH_PACKAGE_ID_CAP], unsigned *files, char *error, size_t capacity)
{
    pa_zip z;
    if (error && capacity) error[0] = 0;
    if (!pa_open(bytes, length, &z, 1, error, capacity)) return 0;
    if (id) strcpy_s(id, SH_PACKAGE_ID_CAP, z.id);
    if (files) *files = z.files;
    pa_free(&z); return 1;
}

int sh_package_archive_identity(const unsigned char *bytes, size_t length,
                                char id[SH_PACKAGE_ID_CAP], unsigned char fingerprint[32],
                                char *error, size_t capacity)
{
    pa_zip z;
    if (error && capacity) error[0] = 0;
    if (!pa_open(bytes, length, &z, 1, error, capacity)) return 0;
    if (id) strcpy_s(id, SH_PACKAGE_ID_CAP, z.id);
    if (fingerprint) memcpy(fingerprint, z.fingerprint, 32);
    pa_free(&z); return 1;
}

void sh_package_archive_files_free(sh_package_archive_files *files)
{
    size_t i;
    if (!files) return;
    for (i = 0; i < files->count; i++) { free(files->items[i].name); free(files->items[i].body); }
    free(files->items); memset(files, 0, sizeof(*files));
}

int sh_package_archive_read(const unsigned char *bytes, size_t length,
    sh_package_archive_files *out, char *error, size_t capacity)
{
    pa_zip z;
    size_t i;
    if (error && capacity) error[0] = 0;
    if (!out) return pa_error(error, capacity, "missing archive conversion output");
    memset(out, 0, sizeof(*out));
    if (!pa_open(bytes, length, &z, 0, error, capacity)) return 0;
    out->items = calloc(z.count, sizeof(*out->items));
    if (!out->items) goto bad;
    for (i = 0; i < z.count; i++) {
        sh_package_archive_file *file = &out->items[out->count++];
        file->name = _strdup(z.entries[i].name); file->directory = z.entries[i].directory;
        file->length = z.entries[i].size;
        if (!file->name || (!file->directory && !(file->body = pa_decode(&z.entries[i])))) goto bad;
    }
    pa_free(&z); return 1;
bad:
    pa_free(&z); sh_package_archive_files_free(out);
    return pa_error(error, capacity, "cannot read complete archive for conversion");
}

unsigned char *sh_package_archive_write(const sh_package_archive_files *files,
    size_t *length, char *error, size_t capacity)
{
    pa_zip verified;
    unsigned char *bytes = pa_pack(NULL, 0, files, length, error, capacity);
    if (bytes && !pa_open(bytes, *length, &verified, 0, error, capacity)) {
        free(bytes); *length = 0; return NULL;
    }
    if (bytes) pa_free(&verified);
    return bytes;
}

static int pa_directory(wchar_t *path)
{
    DWORD attrs;
    if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

int sh_package_archive_unpack(const unsigned char *bytes, size_t length, const char *destination,
                              unsigned *files, char *error, size_t capacity)
{
    pa_zip z;
    wchar_t *base = NULL, *path = NULL;
    size_t base_length, i;
    int ok = 0;
    if (files) *files = 0;
    if (error && capacity) error[0] = 0;
    if (!pa_open(bytes, length, &z, 1, error, capacity)) return 0;
    base = sh_package_source_wide_path(destination); if (!base) goto done;
    base_length = wcslen(base);
    /* Check every existing ancestor; extraction may not traverse a junction. */
    for (i = 7; i < base_length; i++) if (base[i] == L'\\') {
        DWORD attrs; wchar_t old = base[i]; base[i] = 0;
        attrs = GetFileAttributesW(base); base[i] = old;
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) goto done;
    }
    {
        ULARGE_INTEGER available;
        wchar_t *separator = wcsrchr(base, L'\\');
        int queried;
        if (!separator || separator <= base + 6) goto done;
        *separator = 0; queried = GetDiskFreeSpaceExW(base, &available, NULL, NULL); *separator = L'\\';
        if (!queried) goto done;
        if (z.expanded > available.QuadPart) {
            pa_error(error, capacity, "package needs %llu bytes on disk; %llu bytes are available",
                     (unsigned long long)z.expanded, (unsigned long long)available.QuadPart); goto done;
        }
    }
    if (!CreateDirectoryW(base, NULL)) { pa_error(error, capacity, "package destination already exists or cannot be created"); goto done; }
    for (i = 0; i < z.count; i++) {
        pa_entry *e = &z.entries[i];
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, e->name, -1, NULL, 0);
        size_t j;
        unsigned char *decoded;
        HANDLE file;
        DWORD written;
        if (!n || base_length + (size_t)n + 1 >= SH_PACKAGE_SOURCE_PATH_CAP) goto done;
        path = (wchar_t *)calloc(base_length + (size_t)n + 1, sizeof(wchar_t)); if (!path) goto done;
        memcpy(path, base, base_length * sizeof(wchar_t)); path[base_length] = L'\\';
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, e->name, -1, path + base_length + 1, n)) goto done;
        for (j = base_length + 1; path[j]; j++) if (path[j] == L'/') {
            path[j] = 0; if (!pa_directory(path)) goto done; path[j] = L'\\';
        }
        if (e->directory) { if (!pa_directory(path)) goto done; }
        else {
            decoded = pa_decode(e); if (!decoded) goto done;
            file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            if (file == INVALID_HANDLE_VALUE) { free(decoded); goto done; }
            ok = WriteFile(file, decoded, e->size, &written, NULL) && written == e->size && FlushFileBuffers(file);
            CloseHandle(file); free(decoded);
            if (!ok) goto done;
            ok = 0;
        }
        free(path); path = NULL;
    }
    if (files) *files = z.files;
    ok = 1;
done:
    if (!ok && error && capacity && !error[0]) pa_error(error, capacity, "cannot extract complete package into staging directory");
    free(base); free(path); pa_free(&z); return ok;
}
