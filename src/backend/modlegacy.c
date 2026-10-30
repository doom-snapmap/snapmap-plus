/* Legacy carrier conversion and cleaning (DESIGN.md section 11). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */

#include "modlegacy.h"
#include "modmanifest.h"
#include "modbundle.h"
#include "modserve.h"
#include "map_legacy_payload.h"
#include "map_shards.h"
#include "raw_deflate.h"
#include "config_json.h"
#include "backend_log.h"

#define LG_ERR(buf, cap, ...) do { if ((buf) && (cap)) _snprintf_s((buf), (cap), _TRUNCATE, __VA_ARGS__); } while (0)

/* ==================================================================== */
/* text helpers                                                          */
/* ==================================================================== */

typedef struct lg_text { char *p; size_t n, cap; int failed; } lg_text;

static void lg_put(lg_text *t, const char *s, size_t n)
{
    if (t->failed) return;
    if (t->n + n + 1 > t->cap) {
        size_t next = t->cap ? t->cap * 2 : 256;
        char *grown;
        while (next < t->n + n + 1) next *= 2;
        grown = (char *)realloc(t->p, next);
        if (!grown) { t->failed = 1; return; }
        t->p = grown;
        t->cap = next;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = 0;
}

static void lg_puts(lg_text *t, const char *s) { lg_put(t, s, strlen(s)); }

/* A JSON string literal for `s`; malloc'd. */
static char *lg_quote(const char *s)
{
    static const char hex[] = "0123456789abcdef";
    lg_text t = {0};
    size_t i, n = strlen(s);
    lg_put(&t, "\"", 1);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; lg_put(&t, e, 2); }
        else if (c < 0x20) {
            char e[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
            lg_put(&t, e, 6);
        } else lg_put(&t, (const char *)&c, 1);
    }
    lg_put(&t, "\"", 1);
    if (t.failed) { free(t.p); return NULL; }
    return t.p;
}

/* Decode a JSON string literal into a malloc'd string (strict UTF-8). */
static char *lg_text_of(const char *literal)
{
    size_t n, got = 0;
    char *out;
    if (!literal || literal[0] != '"') return NULL;
    n = strlen(literal);
    out = (char *)malloc(n);
    if (!out) return NULL;
    if (!sh_json_decode_string(literal, n, out, n, &got) || strlen(out) != got) { free(out); return NULL; }
    return out;
}

static int lg_prefix(const char *text, const char *prefix)
{
    return _strnicmp(text, prefix, strlen(prefix)) == 0;
}

static int lg_suffix(const char *text, const char *suffix)
{
    size_t n = strlen(text), s = strlen(suffix);
    return n >= s && !_stricmp(text + n - s, suffix);
}

/* ==================================================================== */
/* carriers                                                              */
/* ==================================================================== */

void sh_modlegacy_carriers_free(sh_modlegacy_carrier *carriers, size_t count)
{
    size_t i;
    if (!carriers) return;
    for (i = 0; i < count; i++) free(carriers[i].chunks);
    free(carriers);
}

static void lg_problem(sh_modlegacy_carrier *c, const char *fmt, ...)
{
    va_list ap;
    if (!c->consistent) return;
    c->consistent = 0;
    va_start(ap, fmt);
    _vsnprintf_s(c->problem, sizeof c->problem, _TRUNCATE, fmt, ap);
    va_end(ap);
}

size_t sh_modlegacy_collect(const char *json, size_t length, sh_modlegacy_carrier **out)
{
    sh_modlegacy_carrier *list = NULL;
    size_t count = 0, capacity = 0, pos = 0, i;
    const char *hdr, *chunk;
    size_t hdr_len, chunk_len;
    if (out) *out = NULL;
    if (!json || !length || !out) return 0;
    if (!sh_shard_find(json, length, SH_LEGACY_MAGIC, SH_LEGACY_MAGIC_LEN)) return 0;
    while (sh_shard_next(json, length, SH_LEGACY_MAGIC, SH_LEGACY_MAGIC_LEN, &pos,
                         &hdr, &hdr_len, &chunk, &chunk_len)) {
        sh_legacy_header h;
        sh_modlegacy_carrier *c;
        if (!sh_map_legacy_payload_parse(hdr, hdr_len, &h)) continue;
        for (i = 0; i < count; i++)
            if (!strcmp(list[i].header.key, h.key) && !strcmp(list[i].header.digest, h.digest)) break;
        if (i == count) {
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 8;
                sh_modlegacy_carrier *grown = (sh_modlegacy_carrier *)realloc(list, next * sizeof *grown);
                if (!grown) break;
                list = grown;
                capacity = next;
            }
            c = &list[count++];
            memset(c, 0, sizeof *c);
            c->header = h;
            c->consistent = 1;
            if (h.total > SH_MODLEGACY_MAX_SHARDS) {
                lg_problem(c, "shard total %u is over the writer bound %u", h.total,
                           (unsigned)SH_MODLEGACY_MAX_SHARDS);
            } else {
                c->chunks = (sh_shard_chunk *)calloc(h.total, sizeof *c->chunks);
                if (!c->chunks) lg_problem(c, "out of memory for %u shards", h.total);
            }
        }
        c = &list[i];
        if (!c->chunks) continue;
        if (h.total != c->header.total) { lg_problem(c, "shard totals disagree (%u and %u)", c->header.total, h.total); continue; }
        if (h.index >= c->header.total) { lg_problem(c, "shard index %u is out of range for %u shards", h.index, c->header.total); continue; }
        if (!chunk) { lg_problem(c, "shard %u has an unreadable value", h.index); continue; }
        if (c->chunks[h.index].filled) { lg_problem(c, "shard %u appears twice", h.index); continue; }
        c->chunks[h.index].p = chunk;
        c->chunks[h.index].n = chunk_len;
        c->chunks[h.index].filled = 1;
        c->present++;
    }
    *out = list;
    return count;
}

unsigned char *sh_modlegacy_payload(const sh_modlegacy_carrier *carrier, size_t *out_length,
                                    char *reason, size_t reason_cap)
{
    unsigned char *payload;
    size_t payload_len = 0;
    int why = SH_SHARD_B64_MALFORMED;
    char got[SH_SHARD_DIGEST_CHARS + 1];
    if (out_length) *out_length = 0;
    if (reason && reason_cap) reason[0] = 0;
    if (!carrier || !out_length) return NULL;
    if (!carrier->consistent) { LG_ERR(reason, reason_cap, "inconsistent shards: %s", carrier->problem); return NULL; }
    if (carrier->present != carrier->header.total) {
        LG_ERR(reason, reason_cap, "incomplete: %u of %u shards", carrier->present, carrier->header.total);
        return NULL;
    }
    payload = sh_shard_b64_decode(carrier->chunks, carrier->header.total, SH_MODLEGACY_MAX_PAYLOAD,
                                  &payload_len, &why);
    if (!payload) {
        LG_ERR(reason, reason_cap, "%s", why == SH_SHARD_B64_TOO_BIG ? "payload exceeds the writer bound"
                                       : why == SH_SHARD_B64_NOMEM ? "out of memory decoding the payload"
                                       : "invalid base64 in its shards");
        return NULL;
    }
    sh_shard_digest16(payload, payload_len, got);
    if (strcmp(got, carrier->header.digest)) {
        LG_ERR(reason, reason_cap, "digest mismatch: the header says %s, the payload is %s",
               carrier->header.digest, got);
        HeapFree(GetProcessHeap(), 0, payload);
        return NULL;
    }
    *out_length = payload_len;
    return payload;
}

/* ==================================================================== */
/* ZIP                                                                   */
/* ==================================================================== */

static unsigned lg_r16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned lg_r32(const unsigned char *p) { return lg_r16(p) | (lg_r16(p + 2) << 16); }
static uint64_t lg_r64(const unsigned char *p) { return lg_r32(p) | ((uint64_t)lg_r32(p + 4) << 32); }

static unsigned lg_crc32(const unsigned char *p, size_t n)
{
    static unsigned table[256];
    static volatile LONG ready = 0;
    unsigned c = 0xffffffffu;
    if (!InterlockedCompareExchange(&ready, 0, 0)) {
        unsigned i, j;
        for (i = 0; i < 256; i++) {
            unsigned v = i;
            for (j = 0; j < 8; j++) v = (v >> 1) ^ ((v & 1) ? 0xedb88320u : 0);
            table[i] = v;
        }
        InterlockedExchange(&ready, 1);
    }
    while (n--) c = table[(c ^ *p++) & 255u] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

void sh_modlegacy_zip_free(sh_modlegacy_zip *zip)
{
    size_t i;
    if (!zip) return;
    for (i = 0; i < zip->count; i++) free(zip->entries[i].name);
    free(zip->entries);
    memset(zip, 0, sizeof *zip);
}

/* ZIP64 extended information: present fields follow the sentinel order. */
static int lg_zip64_extra(const unsigned char *extra, size_t length,
                          uint64_t *size, uint64_t *packed, uint64_t *offset)
{
    while (length >= 4) {
        unsigned tag = lg_r16(extra), n = lg_r16(extra + 2);
        if (n > length - 4) return 0;
        if (tag == 1) {
            const unsigned char *f = extra + 4;
            size_t left = n;
            if (*size == 0xffffffffu)   { if (left < 8) return 0; *size = lg_r64(f); f += 8; left -= 8; }
            if (*packed == 0xffffffffu) { if (left < 8) return 0; *packed = lg_r64(f); f += 8; left -= 8; }
            if (*offset == 0xffffffffu) { if (left < 8) return 0; *offset = lg_r64(f); f += 8; left -= 8; }
            return 1;
        }
        extra += 4 + n;
        length -= 4 + n;
    }
    return *size != 0xffffffffu && *packed != 0xffffffffu && *offset != 0xffffffffu;
}

int sh_modlegacy_zip_open(const unsigned char *bytes, size_t length, sh_modlegacy_zip *zip,
                          char *err, size_t err_cap)
{
    const unsigned char *eocd = NULL, *c, *cd_end;
    uint64_t entries, cd_offset, cd_size;
    size_t i, scan_floor;
    if (err && err_cap) err[0] = 0;
    if (!zip) return 0;
    memset(zip, 0, sizeof *zip);
    if (!bytes || length < 22) { LG_ERR(err, err_cap, "not a ZIP (too short)"); return 0; }

    scan_floor = length > 22 + 65535 ? length - 22 - 65535 : 0;
    for (i = length - 22 + 1; i-- > scan_floor;) {
        if (lg_r32(bytes + i) == 0x06054b50u && i + 22 + lg_r16(bytes + i + 20) == length) {
            eocd = bytes + i;
            break;
        }
    }
    if (!eocd) { LG_ERR(err, err_cap, "not a ZIP (no end-of-directory record)"); return 0; }
    if (lg_r16(eocd + 4) || lg_r16(eocd + 6) || lg_r16(eocd + 8) != lg_r16(eocd + 10)) {
        LG_ERR(err, err_cap, "multi-disk ZIPs are not supported"); return 0;
    }
    entries = lg_r16(eocd + 10);
    cd_size = lg_r32(eocd + 12);
    cd_offset = lg_r32(eocd + 16);
    cd_end = eocd;
    if (eocd - bytes >= 20 && lg_r32(eocd - 20) == 0x07064b50u) {
        uint64_t at = lg_r64(eocd - 20 + 8);
        const unsigned char *rec;
        if (at > (uint64_t)(eocd - 20 - bytes) || (uint64_t)(eocd - 20 - bytes) - at < 56) {
            LG_ERR(err, err_cap, "ZIP64 end record is out of range"); return 0;
        }
        rec = bytes + (size_t)at;
        if (lg_r32(rec) != 0x06064b50u) { LG_ERR(err, err_cap, "ZIP64 end record is missing"); return 0; }
        entries = lg_r64(rec + 32);
        cd_size = lg_r64(rec + 40);
        cd_offset = lg_r64(rec + 48);
        cd_end = rec;
    }
    if (cd_offset > (uint64_t)(cd_end - bytes) || cd_size > (uint64_t)(cd_end - bytes) - cd_offset ||
        entries == 0 || entries > cd_size / 46u || entries > SIZE_MAX / sizeof(sh_modlegacy_zip_entry)) {
        LG_ERR(err, err_cap, "the ZIP directory is out of range"); return 0;
    }
    zip->entries = (sh_modlegacy_zip_entry *)calloc((size_t)entries, sizeof *zip->entries);
    if (!zip->entries) { LG_ERR(err, err_cap, "out of memory reading the ZIP"); return 0; }
    c = bytes + (size_t)cd_offset;
    cd_end = c + (size_t)cd_size;
    for (i = 0; i < (size_t)entries; i++) {
        sh_modlegacy_zip_entry *e = &zip->entries[i];
        const unsigned char *lh;
        unsigned flags, n, extra, comment;
        uint64_t size, packed, offset, data_at;
        if ((size_t)(cd_end - c) < 46 || lg_r32(c) != 0x02014b50u) {
            LG_ERR(err, err_cap, "the ZIP directory is malformed at entry %zu", i); goto bad;
        }
        flags = lg_r16(c + 8);
        e->method = lg_r16(c + 10);
        e->crc = lg_r32(c + 16);
        packed = lg_r32(c + 20);
        size = lg_r32(c + 24);
        n = lg_r16(c + 28);
        extra = lg_r16(c + 30);
        comment = lg_r16(c + 32);
        offset = lg_r32(c + 42);
        if ((size_t)(cd_end - c) - 46 < (size_t)n + extra + comment || n == 0) {
            LG_ERR(err, err_cap, "the ZIP directory is malformed at entry %zu", i); goto bad;
        }
        if (flags & 0x41u) { LG_ERR(err, err_cap, "encrypted ZIP members are not supported"); goto bad; }
        if (e->method != 0 && e->method != 8) {
            LG_ERR(err, err_cap, "ZIP member uses compression method %u", e->method); goto bad;
        }
        if (!lg_zip64_extra(c + 46 + n, extra, &size, &packed, &offset)) {
            LG_ERR(err, err_cap, "ZIP64 sizes are malformed at entry %zu", i); goto bad;
        }
        zip->count = i + 1;
        e->name = (char *)malloc((size_t)n + 1);
        if (!e->name) { LG_ERR(err, err_cap, "out of memory reading the ZIP"); goto bad; }
        memcpy(e->name, c + 46, n);
        e->name[n] = 0;
        if (strlen(e->name) != n || strchr(e->name, '\\') ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, e->name, -1, NULL, 0)) {
            LG_ERR(err, err_cap, "ZIP member %zu has an unusable name", i); goto bad;
        }
        e->directory = e->name[n - 1] == '/';
        if (e->directory) {
            if (size || n == 1) { LG_ERR(err, err_cap, "ZIP directory member '%s' is malformed", e->name); goto bad; }
            e->name[n - 1] = 0;
        }
        if (!e->method && packed != size) { LG_ERR(err, err_cap, "stored member '%s' has two sizes", e->name); goto bad; }
        if (!e->directory) {
            if (size > SH_MODLEGACY_MAX_FILE_BYTES) {
                LG_ERR(err, err_cap, "member '%s' is over the 64 MiB file cap", e->name); goto bad;
            }
            zip->expanded += size;
            if (zip->expanded > SH_MODLEGACY_MAX_TREE_BYTES) {
                LG_ERR(err, err_cap, "the package is over the 256 MiB tree cap"); goto bad;
            }
            if (++zip->files > SH_MODLEGACY_MAX_FILES) {
                LG_ERR(err, err_cap, "the package is over the 4096-file cap"); goto bad;
            }
        }
        if (offset > (uint64_t)length || (uint64_t)length - offset < 30) {
            LG_ERR(err, err_cap, "member '%s' points outside the ZIP", e->name); goto bad;
        }
        lh = bytes + (size_t)offset;
        if (lg_r32(lh) != 0x04034b50u || lg_r16(lh + 8) != e->method || lg_r16(lh + 26) != n ||
            memcmp(lh + 30, c + 46, n)) {
            LG_ERR(err, err_cap, "member '%s' has no matching local header", e->name); goto bad;
        }
        data_at = offset + 30 + n + lg_r16(lh + 28);
        if (data_at > (uint64_t)length || packed > (uint64_t)length - data_at) {
            LG_ERR(err, err_cap, "member '%s' data runs past the ZIP", e->name); goto bad;
        }
        e->size = size;
        e->packed = packed;
        e->data = bytes + (size_t)data_at;
        c += 46 + n + extra + comment;
    }
    return 1;
bad:
    sh_modlegacy_zip_free(zip);
    return 0;
}

unsigned char *sh_modlegacy_zip_read(const sh_modlegacy_zip_entry *entry, size_t *out_length,
                                     char *err, size_t err_cap)
{
    unsigned char *out;
    if (out_length) *out_length = 0;
    if (!entry || entry->directory) { LG_ERR(err, err_cap, "not a file member"); return NULL; }
    out = (unsigned char *)malloc((size_t)entry->size + 1);
    if (!out) { LG_ERR(err, err_cap, "out of memory reading '%s'", entry->name); return NULL; }
    if (entry->method == 0) memcpy(out, entry->data, (size_t)entry->size);
    else if (!sh_inflate_raw_exact(entry->data, (size_t)entry->packed, out, (size_t)entry->size)) {
        LG_ERR(err, err_cap, "member '%s' does not inflate", entry->name);
        free(out);
        return NULL;
    }
    if (lg_crc32(out, (size_t)entry->size) != entry->crc) {
        LG_ERR(err, err_cap, "member '%s' fails its CRC", entry->name);
        free(out);
        return NULL;
    }
    out[entry->size] = 0;
    if (out_length) *out_length = (size_t)entry->size;
    return out;
}

const sh_modlegacy_zip_entry *sh_modlegacy_zip_find(const sh_modlegacy_zip *zip, const char *name)
{
    size_t i;
    if (!zip || !name) return NULL;
    for (i = 0; i < zip->count; i++)
        if (!zip->entries[i].directory && !strcmp(zip->entries[i].name, name)) return &zip->entries[i];
    return NULL;
}

/* ==================================================================== */
/* paths and layout                                                      */
/* ==================================================================== */

#define LG_PATH_MAX 512

static int lg_reserved_segment(const char *seg, size_t n)
{
    static const char *const names[] = { "con", "prn", "aux", "nul" };
    size_t stem = 0, i;
    while (stem < n && seg[stem] != '.') stem++;
    for (i = 0; i < 4; i++) if (stem == 3 && !_strnicmp(seg, names[i], 3)) return 1;
    if (stem == 4 && (!_strnicmp(seg, "com", 3) || !_strnicmp(seg, "lpt", 3)) &&
        seg[3] >= '1' && seg[3] <= '9') return 1;
    return 0;
}

int sh_modlegacy_safe_path(const char *path)
{
    const char *seg, *p;
    size_t n;
    if (!path || !*path || path[0] == '/' || strlen(path) > LG_PATH_MAX) return 0;
    for (seg = p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == 0) {
            n = (size_t)(p - seg);
            if (n == 0 || (n == 1 && seg[0] == '.') || (n == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
            if (seg[n - 1] == '.' || seg[n - 1] == ' ') return 0;
            if (lg_reserved_segment(seg, n)) return 0;
            if (!c) return 1;
            seg = p + 1;
            continue;
        }
        if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') return 0;
    }
}

static int lg_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    if (na + nb + 1 > cap) return 0;
    memcpy(out, a, na);
    memcpy(out + na, b, nb + 1);
    return 1;
}

int sh_modlegacy_map_path(const char *relative, char *target, size_t target_cap)
{
    const char *slash;
    if (target && target_cap) target[0] = 0;
    if (!relative || !*relative) return SH_MODLEGACY_PATH_DROP;
    if (!_stricmp(relative, "package.json")) return SH_MODLEGACY_PATH_DESCRIPTOR;
    if (lg_prefix(relative, "assets/"))
        return lg_join(target, target_cap, "assets/", relative + 7) ? SH_MODLEGACY_PATH_ASSET : SH_MODLEGACY_PATH_DROP;
    if (lg_prefix(relative, "decls/"))
        return lg_join(target, target_cap, "assets/generated/decls/", relative + 6) ? SH_MODLEGACY_PATH_ASSET : SH_MODLEGACY_PATH_DROP;
    if (lg_prefix(relative, "images/"))
        return lg_join(target, target_cap, "assets/generated/image/", relative + 7) ? SH_MODLEGACY_PATH_ASSET : SH_MODLEGACY_PATH_DROP;
    if (lg_prefix(relative, "shaders/generated/spirv/"))
        return lg_join(target, target_cap, "assets/generated/spirv/", relative + 24) ? SH_MODLEGACY_PATH_ASSET : SH_MODLEGACY_PATH_DROP;
    if (lg_prefix(relative, "shaders/generated/renderprogs/"))
        return lg_join(target, target_cap, "assets/generated/renderprogs/", relative + 30) ? SH_MODLEGACY_PATH_ASSET : SH_MODLEGACY_PATH_DROP;
    /* Policy files live one level deep. */
    slash = strchr(relative, '/');
    if (slash && !strchr(slash + 1, '/') && slash[1]) {
        if (lg_prefix(relative, "resources/") && lg_suffix(relative, ".manifest")) return SH_MODLEGACY_PATH_MANIFEST;
        if (lg_prefix(relative, "requirements/") && lg_suffix(relative, ".requirements")) return SH_MODLEGACY_PATH_REQUIREMENTS;
        if (lg_prefix(relative, "strings/") && lg_suffix(relative, ".json")) return SH_MODLEGACY_PATH_STRINGS;
        if (!_stricmp(relative, "hud/weapons.json")) return SH_MODLEGACY_PATH_HUD;
    }
    return SH_MODLEGACY_PATH_DROP;
}

int sh_modlegacy_normalize_id(const char *text, char *out, size_t cap)
{
    size_t n, i;
    const char *p;
    if (!text || !out || cap < 2) return 0;
    out[0] = 0;
    if (!memcmp(text, "\xef\xbb\xbf", 3)) text += 3;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n')) n--;
    if (n == 0 || n >= cap || n > 64) return 0;
    for (i = 0, p = text; i < n; i++, p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[n] = 0;
    if (!sh_modmanifest_valid_id(out)) { out[0] = 0; return 0; }
    return 1;
}

/* ==================================================================== */
/* data root                                                             */
/* ==================================================================== */

static SRWLOCK g_lg_lock = SRWLOCK_INIT;
static char g_root_override[MAX_PATH];

void sh_modlegacy_set_data_root(const char *root)
{
    AcquireSRWLockExclusive(&g_lg_lock);
    if (root) strncpy_s(g_root_override, sizeof g_root_override, root, _TRUNCATE);
    else g_root_override[0] = 0;
    ReleaseSRWLockExclusive(&g_lg_lock);
}

static int lg_data_root(char *out, size_t cap)
{
    char base[MAX_PATH];
    AcquireSRWLockShared(&g_lg_lock);
    strncpy_s(out, cap, g_root_override, _TRUNCATE);
    ReleaseSRWLockShared(&g_lg_lock);
    if (out[0]) return 1;
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus", base);
    return 1;
}

/* An ANSI path to a wide one, with the long-path prefix for absolute drive paths. */
static wchar_t *lg_wide_path(const char *ansi, const char *utf8_tail)
{
    int na = MultiByteToWideChar(CP_ACP, 0, ansi, -1, NULL, 0);
    int nt = utf8_tail ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_tail, -1, NULL, 0) : 1;
    int prefix = (ansi[0] && ansi[1] == ':' && ansi[2] == '\\') ? 4 : 0;
    wchar_t *w, *p;
    if (na <= 0 || nt <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(prefix + na + nt + 1));
    if (!w) return NULL;
    if (prefix) memcpy(w, L"\\\\?\\", 4 * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, ansi, -1, w + prefix, na);
    if (utf8_tail) {
        size_t at = (size_t)prefix + (size_t)na - 1;
        w[at++] = L'\\';
        MultiByteToWideChar(CP_UTF8, 0, utf8_tail, -1, w + at, nt);
    }
    for (p = w + prefix; *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}

static char *lg_read_small_file(const char *folder, const char *leaf, size_t *out_len)
{
    wchar_t *w = lg_wide_path(folder, leaf);
    HANDLE h;
    LARGE_INTEGER size;
    char *buf = NULL;
    DWORD got = 0;
    if (out_len) *out_len = 0;
    if (!w) return NULL;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (16 << 20)) {
        buf = (char *)malloc((size_t)size.QuadPart + 1);
        if (buf && ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart) {
            buf[got] = 0;
            if (out_len) *out_len = got;
        } else { free(buf); buf = NULL; }
    }
    CloseHandle(h);
    return buf;
}

static int lg_file_exists(const char *folder, const char *leaf)
{
    wchar_t *w = lg_wide_path(folder, leaf);
    DWORD a;
    if (!w) return 0;
    a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Create `dir` and its parents; existing folders are fine. */
static int lg_ensure_dir(const char *dir)
{
    char path[MAX_PATH];
    size_t i, n;
    strncpy_s(path, sizeof path, dir, _TRUNCATE);
    n = strlen(path);
    for (i = 3; i <= n; i++) {
        if (path[i] == '\\' || path[i] == 0) {
            char keep = path[i];
            path[i] = 0;
            if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                DWORD a = GetFileAttributesA(path);
                if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
            }
            path[i] = keep;
        }
    }
    return 1;
}

/* ==================================================================== */
/* the unpacked tree                                                     */
/* ==================================================================== */

typedef struct lg_written { wchar_t **files; size_t nfiles, capf; wchar_t **dirs; size_t ndirs, capd; } lg_written;

static int lg_track(wchar_t ***list, size_t *n, size_t *cap, wchar_t *item)
{
    if (*n == *cap) {
        size_t next = *cap ? *cap * 2 : 64;
        wchar_t **grown = (wchar_t **)realloc(*list, next * sizeof *grown);
        if (!grown) return 0;
        *list = grown;
        *cap = next;
    }
    (*list)[(*n)++] = item;
    return 1;
}

/* Remove exactly what this conversion created, files first, folders deepest first. */
static void lg_undo(lg_written *w)
{
    size_t i;
    for (i = 0; i < w->nfiles; i++) { DeleteFileW(w->files[i]); free(w->files[i]); }
    for (i = w->ndirs; i-- > 0;) { RemoveDirectoryW(w->dirs[i]); free(w->dirs[i]); }
    free(w->files);
    free(w->dirs);
    memset(w, 0, sizeof *w);
}

static void lg_forget(lg_written *w)
{
    size_t i;
    for (i = 0; i < w->nfiles; i++) free(w->files[i]);
    for (i = 0; i < w->ndirs; i++) free(w->dirs[i]);
    free(w->files);
    free(w->dirs);
    memset(w, 0, sizeof *w);
}

static int lg_mkdir_tracked(lg_written *w, const wchar_t *path)
{
    wchar_t *copy;
    if (CreateDirectoryW(path, NULL)) {
        size_t n = wcslen(path) + 1;
        copy = (wchar_t *)malloc(n * sizeof *copy);
        if (!copy) return 0;
        memcpy(copy, path, n * sizeof *copy);
        return lg_track(&w->dirs, &w->ndirs, &w->capd, copy);
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        DWORD a = GetFileAttributesW(path);
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }
    return 0;
}

/* Write `relative` (UTF-8, '/'-separated) under `folder`, creating parents. */
static int lg_write_member(lg_written *w, const char *folder, const char *relative,
                           const unsigned char *body, size_t length, char *err, size_t err_cap)
{
    wchar_t *full = lg_wide_path(folder, relative), *p, *base_end;
    HANDLE h;
    size_t done = 0;
    if (!full) { LG_ERR(err, err_cap, "cannot form a path for '%s'", relative); return 0; }
    base_end = full + wcslen(full) - (size_t)MultiByteToWideChar(CP_UTF8, 0, relative, -1, NULL, 0) + 1;
    for (p = base_end; *p; p++) {
        if (*p == L'\\') {
            *p = 0;
            if (!lg_mkdir_tracked(w, full)) {
                *p = L'\\';
                LG_ERR(err, err_cap, "cannot create the folder for '%s'", relative);
                free(full);
                return 0;
            }
            *p = L'\\';
        }
    }
    h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        LG_ERR(err, err_cap, "cannot create '%s' (error %lu)", relative, GetLastError());
        free(full);
        return 0;
    }
    if (!lg_track(&w->files, &w->nfiles, &w->capf, full)) {
        CloseHandle(h);
        DeleteFileW(full);
        free(full);
        LG_ERR(err, err_cap, "out of memory");
        return 0;
    }
    while (done < length) {
        DWORD part = (DWORD)((length - done) > (1u << 30) ? (1u << 30) : (length - done)), wrote = 0;
        if (!WriteFile(h, body + done, part, &wrote, NULL) || wrote != part) {
            CloseHandle(h);
            LG_ERR(err, err_cap, "cannot write '%s'", relative);
            return 0;
        }
        done += part;
    }
    CloseHandle(h);
    return 1;
}

/* ==================================================================== */
/* descriptor migration                                                  */
/* ==================================================================== */

typedef struct lg_plan_file {
    const sh_modlegacy_zip_entry *entry;
    char target[LG_PATH_MAX + 32];
} lg_plan_file;

typedef struct lg_plan {
    sh_json_object root;           /* the root package.json */
    sh_json_object cvars, english, weapons;
    lg_plan_file *files;
    size_t count, cap;
    unsigned dropped, imported, policies, nested;
    char **providers;              /* full ZIP names named by resources manifests */
    size_t nproviders;
    int legacy_descriptor;
} lg_plan;

static void lg_plan_free(lg_plan *p)
{
    size_t i;
    sh_json_object_free(&p->root);
    sh_json_object_free(&p->cvars);
    sh_json_object_free(&p->english);
    sh_json_object_free(&p->weapons);
    free(p->files);
    for (i = 0; i < p->nproviders; i++) free(p->providers[i]);
    free(p->providers);
    memset(p, 0, sizeof *p);
}

static int lg_section(const sh_json_object *parent, const char *key, sh_json_object *out)
{
    const char *raw = sh_json_object_get(parent, key);
    return !raw || sh_json_parse_object(raw, strlen(raw), 24, out);
}

static const char *const LG_SUPPORTED_GATES[] = { "g_useResourceBlackList", "g_useImageBlackList" };

static int lg_supported_gate(const char *name)
{
    return !strcmp(name, LG_SUPPORTED_GATES[0]) || !strcmp(name, LG_SUPPORTED_GATES[1]);
}

/* Merge members of `from` into `into` when absent; a differing duplicate is kept as first seen. */
static void lg_merge_absent(sh_json_object *into, const sh_json_object *from)
{
    size_t i;
    for (i = 0; i < from->count; i++)
        if (!sh_json_object_get(into, from->members[i].key))
            sh_json_object_set_n(into, from->members[i].key, from->members[i].key_length,
                                 from->members[i].value_json, 24);
}

static void lg_policy_requirements(lg_plan *p, const char *body)
{
    const char *line = body;
    while (line && *line) {
        const char *next = strchr(line, '\n');
        size_t n = next ? (size_t)(next - line) : strlen(line);
        char row[256], *a, *b;
        if (n && n < sizeof row) {
            memcpy(row, line, n);
            row[n] = 0;
            while (n && (row[n - 1] == '\r' || row[n - 1] == ' ')) row[--n] = 0;
            if (row[0] != '#' && (a = strchr(row, '\t')) != NULL) {
                *a++ = 0;
                b = strchr(a, '\t');
                if (b) {
                    *b++ = 0;
                    if (!strcmp(row, "cvar") && lg_supported_gate(a) && !strcmp(b, "0") &&
                        !sh_json_object_get(&p->cvars, a))
                        sh_json_object_set(&p->cvars, a, "0", 2);
                }
            }
        }
        line = next ? next + 1 : NULL;
    }
    p->policies++;
}

static void lg_policy_manifest(lg_plan *p, const char *component, const char *body)
{
    const char *line = body;
    while (line && *line) {
        const char *next = strchr(line, '\n');
        size_t n = next ? (size_t)(next - line) : strlen(line);
        char row[1024], *a, *b, *provider;
        if (n && n < sizeof row) {
            memcpy(row, line, n);
            row[n] = 0;
            while (n && (row[n - 1] == '\r' || row[n - 1] == ' ')) row[--n] = 0;
            if (row[0] != '#' && (a = strchr(row, '\t')) != NULL) {
                *a++ = 0;
                b = strchr(a, '\t');
                provider = b ? b + 1 : a;
                if (b) *b = 0;
                if (!*provider) provider = a;
                if (sh_modlegacy_safe_path(provider)) {
                    char full[LG_PATH_MAX + 64];
                    char **grown;
                    _snprintf_s(full, sizeof full, _TRUNCATE, "%s%s", component, provider);
                    grown = (char **)realloc(p->providers, (p->nproviders + 1) * sizeof *grown);
                    if (grown) {
                        p->providers = grown;
                        p->providers[p->nproviders] = _strdup(full);
                        if (p->providers[p->nproviders]) p->nproviders++;
                    }
                }
            }
        }
        line = next ? next + 1 : NULL;
    }
    p->policies++;
}

static void lg_policy_json(lg_plan *p, const char *body, size_t length, int hud)
{
    sh_json_object object = {0}, weapons = {0};
    if (length >= 3 && !memcmp(body, "\xef\xbb\xbf", 3)) { body += 3; length -= 3; }
    if (!sh_json_parse_object(body, length, 8, &object)) return;
    if (hud) {
        if (lg_section(&object, "weapons", &weapons)) lg_merge_absent(&p->weapons, &weapons);
    } else lg_merge_absent(&p->english, &object);
    sh_json_object_free(&weapons);
    sh_json_object_free(&object);
    p->policies++;
}

static int lg_target_cmp(const void *a, const void *b)
{
    return _stricmp(((const lg_plan_file *)a)->target, ((const lg_plan_file *)b)->target);
}

static int lg_provider_listed(const lg_plan *p, const char *name)
{
    size_t i;
    for (i = 0; i < p->nproviders; i++) if (!_stricmp(p->providers[i], name)) return 1;
    return 0;
}

/* The component (nested package folder) owning `name`: the longest "X/"
 * prefix with an "X/package.json" member; "" for the root. */
static size_t lg_component(const sh_modlegacy_zip *zip, const char *name)
{
    size_t best = 0, i;
    for (i = 0; i < zip->count; i++) {
        const sh_modlegacy_zip_entry *e = &zip->entries[i];
        size_t n = strlen(e->name);
        if (e->directory || n <= 13 || strcmp(e->name + n - 13, "/package.json")) continue;
        n -= 12;   /* keep the slash */
        if (n > best && !strncmp(name, e->name, n)) best = n;
    }
    return best;
}

static int lg_plan_build(lg_plan *p, const sh_modlegacy_zip *zip, char *err, size_t err_cap)
{
    const sh_modlegacy_zip_entry *descriptor = sh_modlegacy_zip_find(zip, "package.json");
    unsigned char *body;
    size_t length, i, pass;
    sh_json_object section = {0}, inner = {0};
    if (!descriptor) { LG_ERR(err, err_cap, "the ZIP has no package.json at its root"); return 0; }
    body = sh_modlegacy_zip_read(descriptor, &length, err, err_cap);
    if (!body) return 0;
    {
        const char *t = (const char *)body;
        size_t n = length;
        if (n >= 3 && !memcmp(t, "\xef\xbb\xbf", 3)) { t += 3; n -= 3; }
        while (n && sh_shard_is_ws(*t)) { t++; n--; }
        if (!n || !sh_json_parse_object(t, n, 24, &p->root)) {
            free(body);
            LG_ERR(err, err_cap, "package.json is not a JSON object");
            return 0;
        }
    }
    free(body);
    {
        char *schema = lg_text_of(sh_json_object_get(&p->root, "schema"));
        p->legacy_descriptor = !sh_json_object_get(&p->root, "id") ||
                               (schema && !strncmp(schema, "snapmap-plus.override-package.", 30));
        free(schema);
    }
    if (lg_section(&p->root, "requirements", &section)) {
        lg_section(&section, "cvars", &p->cvars);
        sh_json_object_free(&section);
    }
    if (lg_section(&p->root, "strings", &section)) {
        lg_section(&section, "en", &p->english);
        sh_json_object_free(&section);
    }
    if (lg_section(&p->root, "hud", &section)) {
        lg_section(&section, "weapons", &p->weapons);
        sh_json_object_free(&section);
    }

    /* Pass 0 reads policy files (manifests name providers); pass 1 places files. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < zip->count; i++) {
            const sh_modlegacy_zip_entry *e = &zip->entries[i];
            size_t comp = lg_component(zip, e->name);
            const char *relative = e->name + comp;
            char target[LG_PATH_MAX + 32], component[LG_PATH_MAX + 2];
            int kind;
            if (e->directory) continue;
            if (!sh_modlegacy_safe_path(e->name)) {
                LG_ERR(err, err_cap, "the ZIP member '%s' is not a safe path", e->name);
                return 0;
            }
            if (comp >= sizeof component) comp = sizeof component - 1;
            memcpy(component, e->name, comp);
            component[comp] = 0;
            kind = sh_modlegacy_map_path(relative, target, sizeof target);
            if (pass == 0) {
                if (kind == SH_MODLEGACY_PATH_DESCRIPTOR && comp) {
                    /* A nested component: its cvar requirements join the bundle's. */
                    unsigned char *text = sh_modlegacy_zip_read(e, &length, NULL, 0);
                    if (text && sh_json_parse_object((const char *)text, length, 24, &inner)) {
                        sh_json_object req = {0}, cv = {0};
                        if (lg_section(&inner, "requirements", &req) && lg_section(&req, "cvars", &cv))
                            lg_merge_absent(&p->cvars, &cv);
                        sh_json_object_free(&cv);
                        sh_json_object_free(&req);
                    }
                    sh_json_object_free(&inner);
                    free(text);
                    p->nested++;
                } else if (kind >= SH_MODLEGACY_PATH_MANIFEST) {
                    unsigned char *text = sh_modlegacy_zip_read(e, &length, NULL, 0);
                    if (text) {
                        if (kind == SH_MODLEGACY_PATH_MANIFEST) lg_policy_manifest(p, component, (const char *)text);
                        else if (kind == SH_MODLEGACY_PATH_REQUIREMENTS) lg_policy_requirements(p, (const char *)text);
                        else lg_policy_json(p, (const char *)text, length, kind == SH_MODLEGACY_PATH_HUD);
                    }
                    free(text);
                }
                continue;
            }
            if (kind == SH_MODLEGACY_PATH_DROP && lg_provider_listed(p, e->name) &&
                lg_join(target, sizeof target, "assets/", relative)) {
                kind = SH_MODLEGACY_PATH_ASSET;
                p->imported++;
            }
            if (kind != SH_MODLEGACY_PATH_ASSET) {
                if (kind == SH_MODLEGACY_PATH_DROP) p->dropped++;
                continue;
            }
            if (!sh_modlegacy_safe_path(target)) {
                LG_ERR(err, err_cap, "'%s' maps to an unsafe path", e->name);
                return 0;
            }
            if (p->count == p->cap) {
                size_t next = p->cap ? p->cap * 2 : 64;
                lg_plan_file *grown = (lg_plan_file *)realloc(p->files, next * sizeof *grown);
                if (!grown) { LG_ERR(err, err_cap, "out of memory planning the tree"); return 0; }
                p->files = grown;
                p->cap = next;
            }
            p->files[p->count].entry = e;
            strcpy_s(p->files[p->count].target, sizeof p->files[p->count].target, target);
            p->count++;
        }
    }
    if (!p->count) { LG_ERR(err, err_cap, "the package has no files under assets/ after migration"); return 0; }
    /* Windows folds case: two members on one path must hold the same bytes. */
    qsort(p->files, p->count, sizeof *p->files, lg_target_cmp);
    {
        size_t w = 1;
        for (i = 1; i < p->count; i++) {
            if (!_stricmp(p->files[i].target, p->files[w - 1].target)) {
                const sh_modlegacy_zip_entry *a = p->files[w - 1].entry, *b = p->files[i].entry;
                if (a->size != b->size || a->crc != b->crc) {
                    LG_ERR(err, err_cap, "'%s' and '%s' land on one path with different bytes", a->name, b->name);
                    return 0;
                }
                continue;
            }
            p->files[w++] = p->files[i];
        }
        p->count = w;
    }
    return 1;
}

/* The request path of a target ("assets/x/y" -> "x/y"), lowercased. */
static void lg_request(const char *target, char *out, size_t cap)
{
    size_t i;
    strncpy_s(out, cap, target + 7, _TRUNCATE);
    for (i = 0; out[i]; i++) if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
}

static int lg_set_text(sh_json_object *o, const char *key, const char *value)
{
    char *q = lg_quote(value);
    int ok = q && sh_json_object_set(o, key, q, 2);
    free(q);
    return ok;
}

static int lg_set_object(sh_json_object *o, const char *key, const sh_json_object *value)
{
    char *raw = sh_json_serialize_object(value, 0, NULL);
    int ok = raw && sh_json_object_set(o, key, raw, 24);
    free(raw);
    return ok;
}

/* mod.json for the converted bundle. */
static char *lg_descriptor(lg_plan *p, const char *id, const sh_legacy_header *h, unsigned *shadows,
                           size_t *out_len)
{
    sh_json_object d = {0};
    lg_text list = {0};
    char version[SH_MODMANIFEST_VERSION_CAP], ns[96];
    char *name = lg_text_of(sh_json_object_get(&p->root, "name")), *out = NULL;
    const char *raw;
    size_t i;
    *shadows = 0;
    _snprintf_s(version, sizeof version, _TRUNCATE, "legacy-%s", h->digest);
    _snprintf_s(ns, sizeof ns, _TRUNCATE, "mods/%s/", id);
    if (!lg_set_text(&d, "id", id) || !lg_set_text(&d, "version", version) ||
        !lg_set_text(&d, "name", (name && *name) ? name : id)) goto done;
    raw = sh_json_object_get(&p->root, "description");
    if (raw && raw[0] == '"') sh_json_object_set(&d, "description", raw, 2);
    raw = sh_json_object_get(&p->root, "author");
    if (raw && raw[0] == '"') sh_json_object_set(&d, "author", raw, 2);
    raw = sh_json_object_get(&p->root, "homepage");
    if (raw && raw[0] == '"') sh_json_object_set(&d, "homepage", raw, 2);
    if (p->cvars.count) {
        sh_json_object req = {0};
        if (!lg_set_object(&req, "cvars", &p->cvars) || !lg_set_object(&d, "requirements", &req)) {
            sh_json_object_free(&req); goto done;
        }
        sh_json_object_free(&req);
    }
    {
        sh_json_object strings = {0};
        lg_section(&p->root, "strings", &strings);
        if (p->english.count) lg_set_object(&strings, "en", &p->english);
        if (strings.count && !lg_set_object(&d, "strings", &strings)) { sh_json_object_free(&strings); goto done; }
        sh_json_object_free(&strings);
    }
    if (p->weapons.count) {
        sh_json_object hud = {0};
        if (!lg_set_object(&hud, "weapons", &p->weapons) || !lg_set_object(&d, "hud", &hud)) {
            sh_json_object_free(&hud); goto done;
        }
        sh_json_object_free(&hud);
    }
    /* First-generation packages never used a mods/<id>/ namespace, so every
     * name they carry is admitted as a shadow (section 11) and reported. */
    lg_put(&list, "[", 1);
    for (i = 0; i < p->count; i++) {
        char request[LG_PATH_MAX + 32];
        char *q;
        lg_request(p->files[i].target, request, sizeof request);
        if (strstr(request, ns)) continue;
        q = lg_quote(request);
        if (!q) { list.failed = 1; break; }
        if (*shadows) lg_put(&list, ",", 1);
        lg_puts(&list, q);
        free(q);
        (*shadows)++;
    }
    lg_put(&list, "]", 1);
    if (list.failed || !sh_json_object_set(&d, "shadows", list.p, 4)) goto done;
    {
        sh_json_object legacy = {0};
        int ok = lg_set_text(&legacy, "key", h->key) && lg_set_text(&legacy, "digest16", h->digest) &&
                 lg_set_text(&legacy, "keyForm", h->key_form == SH_LEGACY_KEY_DELIVERY ? "delivery"
                                               : h->key_form == SH_LEGACY_KEY_DESCRIPTOR ? "descriptor" : "folder") &&
                 lg_set_text(&legacy, "layout", p->legacy_descriptor ? "B" : "C") &&
                 lg_set_object(&d, "legacy", &legacy);
        sh_json_object_free(&legacy);
        if (!ok) goto done;
    }
    out = sh_json_serialize_object(&d, 0, out_len);
done:
    free(name);
    free(list.p);
    sh_json_object_free(&d);
    return out;
}

/* ==================================================================== */
/* conversion                                                            */
/* ==================================================================== */

/* Read the bundle.json the builder wrote; fill the verdict. */
static int lg_take_bundle(const char *folder, sh_modlegacy_verdict *v, sh_modmanifest_row *info)
{
    size_t length = 0;
    char *text = lg_read_small_file(folder, "bundle.json", &length);
    char err[160] = "";
    int ok;
    if (!text) { LG_ERR(v->reason, sizeof v->reason, "bundle.json was not written"); return 0; }
    ok = sh_modmanifest_read_bundle_json(text, length, info, err, sizeof err);
    free(text);
    if (!ok) { LG_ERR(v->reason, sizeof v->reason, "bundle.json does not read: %s", err); return 0; }
    strcpy_s(v->id, sizeof v->id, info->id);
    strcpy_s(v->version, sizeof v->version, info->version);
    strcpy_s(v->bundle, sizeof v->bundle, info->bundle);
    v->size = info->size;
    strncpy_s(v->folder, sizeof v->folder, folder, _TRUNCATE);
    return 1;
}

/* An installed <anything>@<digest16> folder under legacy\ with a bundle.json. */
static int lg_find_existing(const char *legacy_dir, const char *digest, char *folder, size_t cap)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int found = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*@%s", legacy_dir, digest);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        _snprintf_s(folder, cap, _TRUNCATE, "%s\\%s", legacy_dir, fd.cFileName);
        if (lg_file_exists(folder, "bundle.json")) { found = 1; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static int lg_convert(const unsigned char *zipbytes, size_t length, const sh_legacy_header *h,
                      sh_modlegacy_verdict *v, sh_modmanifest_row *info)
{
    sh_modlegacy_zip zip;
    lg_plan plan;
    lg_written written;
    char root[MAX_PATH], legacy_dir[MAX_PATH], folder[MAX_PATH], tmp[MAX_PATH];
    char err[200] = "", cause[200] = "", id[SH_MODMANIFEST_ID_CAP];
    char *descriptor = NULL, *idtext = NULL;
    size_t descriptor_len = 0, i;
    unsigned shadows = 0;
    int ok = 0, have_folder = 0;
    ULONGLONG started = GetTickCount64();

    memset(&zip, 0, sizeof zip);
    memset(&plan, 0, sizeof plan);
    memset(&written, 0, sizeof written);
    if (!lg_data_root(root, sizeof root)) { LG_ERR(v->reason, sizeof v->reason, "no data folder"); return 0; }
    _snprintf_s(legacy_dir, sizeof legacy_dir, _TRUNCATE, "%s\\mods\\legacy", root);

    if (lg_find_existing(legacy_dir, h->digest, folder, sizeof folder)) {
        ok = lg_take_bundle(folder, v, info);
        if (ok) LG_ERR(v->reason, sizeof v->reason, "%s", "");
        return ok;
    }
    if (!sh_modlegacy_zip_open(zipbytes, length, &zip, err, sizeof err)) {
        LG_ERR(v->reason, sizeof v->reason, "undecodable ZIP: %s", err);
        return 0;
    }
    if (!lg_plan_build(&plan, &zip, err, sizeof err)) {
        LG_ERR(v->reason, sizeof v->reason, "%s", err);
        goto done;
    }
    v->layout = plan.legacy_descriptor ? 'B' : 'C';
    for (i = 0; i < plan.count && !plan.legacy_descriptor; i++)
        if (_strnicmp(plan.files[i].entry->name + lg_component(&zip, plan.files[i].entry->name), "assets/", 7))
            v->layout = 'B';

    idtext = lg_text_of(sh_json_object_get(&plan.root, "id"));
    if (!(idtext && sh_modlegacy_normalize_id(idtext, id, sizeof id)) &&
        !(h->key_form != SH_LEGACY_KEY_DELIVERY && sh_modlegacy_normalize_id(h->key, id, sizeof id)))
        strcpy_s(id, sizeof id, "legacy");
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\%s@%s", legacy_dir, id, h->digest);
    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s\\.tmp-%s-%lu-%llu", legacy_dir, h->digest,
                GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    if (strlen(folder) + 32 >= MAX_PATH || strlen(tmp) + 32 >= MAX_PATH) {
        LG_ERR(v->reason, sizeof v->reason, "the data folder path is too long"); goto done;
    }

    descriptor = lg_descriptor(&plan, id, h, &shadows, &descriptor_len);
    if (!descriptor) { LG_ERR(v->reason, sizeof v->reason, "the descriptor could not be converted"); goto done; }

    {
        DWORD a = GetFileAttributesA(folder);
        have_folder = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }
    if (!have_folder) {
        if (!lg_ensure_dir(legacy_dir)) { LG_ERR(v->reason, sizeof v->reason, "cannot create %s", legacy_dir); goto done; }
        {
            wchar_t *wtmp = lg_wide_path(tmp, NULL);
            int made = wtmp && lg_mkdir_tracked(&written, wtmp);
            free(wtmp);
            if (!made) { LG_ERR(v->reason, sizeof v->reason, "cannot create the unpack folder"); goto done; }
        }
        for (i = 0; i < plan.count; i++) {
            size_t n = 0;
            unsigned char *body = sh_modlegacy_zip_read(plan.files[i].entry, &n, err, sizeof err);
            int wrote = body && lg_write_member(&written, tmp, plan.files[i].target, body, n, err, sizeof err);
            free(body);
            if (!wrote) { LG_ERR(v->reason, sizeof v->reason, "unpack failed: %s", err); lg_undo(&written); goto done; }
        }
        if (!lg_write_member(&written, tmp, "mod.json", (const unsigned char *)descriptor, descriptor_len,
                             err, sizeof err)) {
            LG_ERR(v->reason, sizeof v->reason, "unpack failed: %s", err);
            lg_undo(&written);
            goto done;
        }
        if (!MoveFileExA(tmp, folder, 0)) {
            DWORD a = GetFileAttributesA(folder);
            lg_undo(&written);
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                LG_ERR(v->reason, sizeof v->reason, "cannot move the unpacked tree into place (error %lu)", GetLastError());
                goto done;
            }
            /* Another load installed the same digest meanwhile: use it. */
        } else lg_forget(&written);
    }

    if (!lg_file_exists(folder, "bundle.json") && !modbundle_build_folder(folder, cause, sizeof cause)) {
        LG_ERR(v->reason, sizeof v->reason, "the bundle builder refused %s: %s", folder,
               cause[0] ? cause : "no cause given");
        strncpy_s(v->folder, sizeof v->folder, folder, _TRUNCATE);
        goto done;
    }
    ok = lg_take_bundle(folder, v, info);
    if (ok) {
        char line[MAX_PATH + 200];
        v->files = (unsigned)plan.count;
        v->shadows = shadows;
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "LEGACY: unpacked %zu file(s) into %s in %llu ms (%u not an input and not kept, %u imported by a "
            "resources manifest, %u policy file(s) folded into mod.json, %u nested component(s))",
            plan.count, folder, (unsigned long long)(GetTickCount64() - started), plan.dropped,
            plan.imported, plan.policies, plan.nested);
        backend_log(line);
    }
done:
    free(idtext);
    free(descriptor);
    lg_plan_free(&plan);
    sh_modlegacy_zip_free(&zip);
    return ok;
}

int sh_modlegacy_convert_payload(const unsigned char *zip, size_t length,
                                 const sh_legacy_header *header, sh_modlegacy_verdict *verdict)
{
    sh_modmanifest_row info;
    int ok;
    if (!verdict || !header) return 0;
    memset(&info, 0, sizeof info);
    strcpy_s(verdict->key, sizeof verdict->key, header->key);
    strcpy_s(verdict->digest, sizeof verdict->digest, header->digest);
    verdict->key_form = header->key_form;
    ok = lg_convert(zip, length, header, verdict, &info);
    verdict->verdict = ok ? SH_MODLEGACY_CONVERT : SH_MODLEGACY_CLEAN;
    sh_modmanifest_row_free(&info);
    return ok;
}

/* ==================================================================== */
/* memo, report, process                                                 */
/* ==================================================================== */

typedef struct lg_memo {
    char key[SH_LEGACY_KEY_CAP];
    char digest[17];
    unsigned total;
    unsigned long long chars;
    sh_modlegacy_verdict verdict;
    sh_modmanifest_row info;   /* the converted bundle's provides names */
} lg_memo;

#define LG_MEMO_MAX 64
static lg_memo g_memo[LG_MEMO_MAX];
static size_t g_memo_count;
static sh_modlegacy_verdict *g_report;
static size_t g_report_count;

void sh_modlegacy_reset(void)
{
    size_t i;
    AcquireSRWLockExclusive(&g_lg_lock);
    for (i = 0; i < g_memo_count; i++) sh_modmanifest_row_free(&g_memo[i].info);
    g_memo_count = 0;
    free(g_report);
    g_report = NULL;
    g_report_count = 0;
    ReleaseSRWLockExclusive(&g_lg_lock);
}

size_t sh_modlegacy_last_report(sh_modlegacy_verdict *out, size_t cap)
{
    size_t n;
    AcquireSRWLockShared(&g_lg_lock);
    n = g_report_count;
    if (out && cap) memcpy(out, g_report, (n < cap ? n : cap) * sizeof *out);
    ReleaseSRWLockShared(&g_lg_lock);
    return n;
}

static void lg_set_report(sh_modlegacy_verdict *list, size_t count)
{
    AcquireSRWLockExclusive(&g_lg_lock);
    free(g_report);
    g_report = list;
    g_report_count = count;
    ReleaseSRWLockExclusive(&g_lg_lock);
}

static unsigned long long lg_chars(const sh_modlegacy_carrier *c)
{
    unsigned long long n = 0;
    unsigned i;
    if (!c->chunks) return 0;
    for (i = 0; i < c->header.total; i++) n += c->chunks[i].n;
    return n;
}

/* Main thread only, like the detour that calls it; the memo is not locked
 * against a second converting thread. */
static lg_memo *lg_memo_find(const sh_modlegacy_carrier *c)
{
    size_t i;
    unsigned long long chars = lg_chars(c);
    for (i = 0; i < g_memo_count; i++)
        if (!strcmp(g_memo[i].key, c->header.key) && !strcmp(g_memo[i].digest, c->header.digest) &&
            g_memo[i].total == c->header.total && g_memo[i].chars == chars) return &g_memo[i];
    return NULL;
}

static void lg_memo_store(const sh_modlegacy_carrier *c, const sh_modlegacy_verdict *v,
                          const sh_modmanifest_row *info)
{
    lg_memo *m;
    size_t i;
    if (g_memo_count == LG_MEMO_MAX) {
        sh_modmanifest_row_free(&g_memo[0].info);
        memmove(g_memo, g_memo + 1, (LG_MEMO_MAX - 1) * sizeof *g_memo);
        g_memo_count--;
    }
    m = &g_memo[g_memo_count++];
    memset(m, 0, sizeof *m);
    strcpy_s(m->key, sizeof m->key, c->header.key);
    strcpy_s(m->digest, sizeof m->digest, c->header.digest);
    m->total = c->header.total;
    m->chars = lg_chars(c);
    m->verdict = *v;
    if (info) {
        m->info = *info;
        m->info.names = NULL;
        m->info.name_count = 0;
        for (i = 0; i < info->name_count; i++) sh_modmanifest_row_add_name(&m->info, info->names[i]);
    }
}

static int lg_stricmp_ptr(const void *a, const void *b)
{
    return _stricmp(*(const char *const *)a, *(const char *const *)b);
}

static void lg_log_verdict(const sh_modlegacy_verdict *v, int memo)
{
    char line[1024];
    if (v->verdict == SH_MODLEGACY_CONVERT)
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "LEGACY: smpkg.%s digest %s (%u/%u shards, %s): CONVERT -> %s as %s@%s, bundle %.16s, "
            "layout era %c, %u file(s), %u shadow name(s)%s",
            v->key, v->digest, v->present, v->total, sh_map_legacy_key_form_name(v->key_form),
            v->folder, v->id, v->version, v->bundle, v->layout ? v->layout : '?', v->files, v->shadows,
            memo ? " [converted earlier this session]" : "");
    else
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "LEGACY: smpkg.%s digest %s (%u/%u shards, %s): CLEAN -- %s; its rows are removed and the "
            "map loads without it",
            v->key, v->digest, v->present, v->total, sh_map_legacy_key_form_name(v->key_form),
            v->reason[0] ? v->reason : "unknown");
    backend_log(line);
}

char *sh_modlegacy_process(const char *json, size_t length, size_t *out_length)
{
    sh_modlegacy_carrier *carriers = NULL;
    sh_modlegacy_verdict *report = NULL;
    sh_modmanifest_list rows = {0};
    char **placed = NULL;
    size_t ncarriers, nplaced = 0, i, stripped_len = 0, final_len = 0;
    unsigned converted = 0, cleaned = 0, package_rows = 0, unreleased = 0, unindexed = 0;
    int placed_ready = 0, placed_failed = 0, doc_failed = 0;
    char *stripped = NULL, *final = NULL;
    char line[512];

    if (out_length) *out_length = 0;
    if (!json || !length ||
        (!sh_shard_find(json, length, SH_LEGACY_MAGIC, SH_LEGACY_MAGIC_LEN) &&
         !sh_shard_find(json, length, "smpbind.", 8) && !sh_shard_find(json, length, "smpref.", 7))) {
        lg_set_report(NULL, 0);
        return NULL;
    }

    ncarriers = sh_modlegacy_collect(json, length, &carriers);
    if (ncarriers) report = (sh_modlegacy_verdict *)calloc(ncarriers, sizeof *report);
    for (i = 0; i < ncarriers && report; i++) {
        sh_modlegacy_carrier *c = &carriers[i];
        sh_modlegacy_verdict *v = &report[i];
        sh_modmanifest_row info;
        lg_memo *memo = lg_memo_find(c);
        int from_memo = memo != NULL;
        memset(&info, 0, sizeof info);
        if (memo) {
            *v = memo->verdict;
            if (v->verdict == SH_MODLEGACY_CONVERT) {
                size_t k;
                info = memo->info;
                info.names = NULL;
                info.name_count = 0;
                for (k = 0; k < memo->info.name_count; k++) sh_modmanifest_row_add_name(&info, memo->info.names[k]);
            }
        } else {
            size_t zip_len = 0;
            unsigned char *zip;
            strcpy_s(v->key, sizeof v->key, c->header.key);
            strcpy_s(v->digest, sizeof v->digest, c->header.digest);
            v->key_form = c->header.key_form;
            v->total = c->header.total;
            v->present = c->present;
            zip = sh_modlegacy_payload(c, &zip_len, v->reason, sizeof v->reason);
            if (zip) {
                v->verdict = lg_convert(zip, zip_len, &c->header, v, &info) ? SH_MODLEGACY_CONVERT
                                                                            : SH_MODLEGACY_CLEAN;
                HeapFree(GetProcessHeap(), 0, zip);
            } else v->verdict = SH_MODLEGACY_CLEAN;
            v->total = c->header.total;
            v->present = c->present;
            lg_memo_store(c, v, v->verdict == SH_MODLEGACY_CONVERT ? &info : NULL);
        }
        lg_log_verdict(v, from_memo);
        if (v->verdict == SH_MODLEGACY_CONVERT) {
            sh_modmanifest_row row;
            size_t k;
            converted++;
            /* A bundle converted now (or installed after boot) is not in the
             * serving index yet; the activation that follows in this parse
             * reads the index. */
            if (sh_modserve_bundle_state(v->bundle, NULL, 0) != 1) unindexed++;
            if (!placed_ready) {
                nplaced = sh_modmanifest_placed_names(json, length, &placed, &placed_failed);
                placed_ready = 1;
            }
            memset(&row, 0, sizeof row);
            strcpy_s(row.id, sizeof row.id, v->id);
            strcpy_s(row.version, sizeof row.version, v->version);
            strcpy_s(row.bundle, sizeof row.bundle, v->bundle);
            row.size = v->size;
            for (k = 0; k < info.name_count && nplaced; k++) {
                const char *key = info.names[k];
                if (bsearch(&key, placed, nplaced, sizeof *placed, lg_stricmp_ptr))
                    sh_modmanifest_row_add_name(&row, key);
            }
            if (!rows.count || strcmp(rows.rows[rows.count - 1].bundle, row.bundle)) {
                size_t r;
                int dup = 0;
                for (r = 0; r < rows.count; r++) if (!strcmp(rows.rows[r].bundle, row.bundle)) dup = 1;
                if (!dup) sh_modmanifest_list_add(&rows, &row);
            }
            sh_modmanifest_row_free(&row);
        } else cleaned++;
        sh_modmanifest_row_free(&info);
    }
    sh_modlegacy_carriers_free(carriers, ncarriers);
    sh_modmanifest_names_free(placed, nplaced);
    if (unindexed) {
        int active = sh_modserve_index_rescan();
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "LEGACY: %u converted bundle(s) not in the serving index; rescan %s (%d bundle(s) active)",
            unindexed, active < 0 ? "refused" : "done", active < 0 ? 0 : active);
        backend_log(line);
    }

    stripped = sh_map_legacy_payload_strip_all(json, length, &stripped_len, &package_rows, &unreleased,
                                               &doc_failed);
    if (!stripped) {
        if (doc_failed)
            backend_log("LEGACY: the legacy rows could not be removed (the map's JSON did not read "
                        "cleanly); the map loads with them as inert string variables");
        sh_modmanifest_list_free(&rows);
        lg_set_report(report, report ? ncarriers : 0);
        return NULL;
    }
    final = stripped;
    final_len = stripped_len;
    if (rows.count) {
        /* Keep any manifest rows the map already carries; add the converted ones. */
        sh_modmanifest_list merged = {0};
        char err[192] = "";
        size_t r;
        sh_modmanifest_read(stripped, stripped_len, &merged, NULL);
        for (r = 0; r < rows.count; r++) {
            size_t m, dup = 0;
            for (m = 0; m < merged.count; m++) if (!strcmp(merged.rows[m].bundle, rows.rows[r].bundle)) dup = 1;
            if (!dup) sh_modmanifest_list_add(&merged, &rows.rows[r]);
        }
        final = sh_modmanifest_write(stripped, stripped_len, &merged, &final_len, err, sizeof err);
        sh_modmanifest_list_free(&merged);
        if (final) HeapFree(GetProcessHeap(), 0, stripped);
        else {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "LEGACY: the converted manifest rows could not be written (%s); the converted bundles are "
                "installed but this load does not activate them", err[0] ? err : "unknown");
            backend_log(line);
            final = stripped;
            final_len = stripped_len;
        }
    }
    if (ncarriers) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "LEGACY: %zu carrier(s): %u converted, %u cleaned; %u package row(s)%s removed before parse, "
            "%zu -> %zu bytes%s",
            ncarriers, converted, cleaned, package_rows, unreleased ? " and unreleased v2 rows" : "",
            length, final_len,
            converted ? "; the map is modified in memory only and the next save persists the "
                        "conversion as manifest rows" : "");
        backend_log(line);
    }
    sh_modmanifest_list_free(&rows);
    lg_set_report(report, report ? ncarriers : 0);
    if (out_length) *out_length = final_len;
    return final;
}
