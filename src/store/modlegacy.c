/* modlegacy.c -- see modlegacy.h. Store tool only. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modlegacy.h"
#include "store_text.h"
#include "modmanifest.h"
#include "modbundle.h"
#include "map_legacy_payload.h"
#include "map_shards.h"
#include "map_old.h"
#include "raw_deflate.h"
#include "config_json.h"
#include "backend_log.h"
#include "plain_reason.h"

#define LG_ERR(buf, cap, ...) do { if ((buf) && (cap)) st_putf((buf), (cap), __VA_ARGS__); } while (0)

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
    for (i = 0; i < zip->count; i++) { free(zip->entries[i].name); free(zip->entries[i].source); }
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
            zip->expanded += size;
            zip->files++;
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
    if (entry->source) {
        /* A folder tree's file: read it whole; it must still be the size walked. */
        HANDLE h = CreateFileW(entry->source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        LARGE_INTEGER size;
        size_t done = 0;
        int ok = h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && (uint64_t)size.QuadPart == entry->size;
        while (ok && done < (size_t)entry->size) {
            DWORD part = (DWORD)(((size_t)entry->size - done) > (1u << 30) ? (1u << 30) : ((size_t)entry->size - done));
            DWORD got = 0;
            ok = ReadFile(h, out + done, part, &got, NULL) && got == part;
            done += got;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!ok) {
            LG_ERR(err, err_cap, "'%s' is unreadable or changed size", entry->name);
            free(out);
            return NULL;
        }
        out[entry->size] = 0;
        if (out_length) *out_length = (size_t)entry->size;
        return out;
    }
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

#define LG_PATH_MAX 65535   /* a ZIP entry name's longest: the format's 16-bit length */

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
    {
        char norm[SH_MB_ID_MAX + 1];
        if (!sh_mb_id_normalize(out, n, norm) || strcmp(norm, out)) { out[0] = 0; return 0; }
    }
    return 1;
}

static int lg_is_hex64(const char *s)
{
    size_t i;
    for (i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return s[64] == 0;
}

/* ==================================================================== */
/* data root                                                             */
/* ==================================================================== */

#define LG_PATH ST_PATH   /* a UTF-8 path */
static SRWLOCK g_lg_lock = SRWLOCK_INIT;
static char g_root_override[LG_PATH];

void sh_modlegacy_set_data_root(const char *root)
{
    AcquireSRWLockExclusive(&g_lg_lock);
    if (root) st_put(g_root_override, sizeof g_root_override, root);
    else g_root_override[0] = 0;
    ReleaseSRWLockExclusive(&g_lg_lock);
}

static int lg_data_root(char *out, size_t cap)
{
    AcquireSRWLockShared(&g_lg_lock);
    st_cpy(out, cap, g_root_override);
    ReleaseSRWLockShared(&g_lg_lock);
    if (out[0]) return 1;
    return st_path(out, cap, sh_mb_user_root());
}

/* A UTF-8 path (every path in this file is, as the store spells them) to a
 * wide one, with the long-path prefix for absolute drive paths. */
static wchar_t *lg_wide_path(const char *ansi, const char *utf8_tail)
{
    int na = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ansi, -1, NULL, 0);
    int nt = utf8_tail ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_tail, -1, NULL, 0) : 1;
    int prefix = (ansi[0] && ansi[1] == ':' && ansi[2] == '\\') ? 4 : 0;
    wchar_t *w, *p;
    if (na <= 0 || nt <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(prefix + na + nt + 1));
    if (!w) return NULL;
    if (prefix) memcpy(w, L"\\\\?\\", 4 * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, ansi, -1, w + prefix, na);
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

/* GetFileAttributes of a UTF-8 path. */
static DWORD lg_attr(const char *path)
{
    wchar_t *w = lg_wide_path(path, NULL);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a;
}

/* Create `dir` and its parents; existing folders are fine. */
static int lg_ensure_dir(const char *dir)
{
    char path[LG_PATH];
    size_t i, n;
    st_cpy(path, sizeof path, dir);
    n = strlen(path);
    for (i = 3; i <= n; i++) {
        if (path[i] == '\\' || path[i] == 0) {
            char keep = path[i];
            wchar_t *w;
            BOOL made;
            path[i] = 0;
            w = lg_wide_path(path, NULL);
            made = w && CreateDirectoryW(w, NULL);
            free(w);
            if (!made && GetLastError() != ERROR_ALREADY_EXISTS) {
                DWORD a = lg_attr(path);
                if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
            }
            path[i] = keep;
        }
    }
    return 1;
}

/* A directory listing of a UTF-8 folder: FindFirstFileW on <dir>\*, each
 * name handed back in UTF-8 through lg_find_name. */
static HANDLE lg_find_first(const char *dir, WIN32_FIND_DATAW *fd)
{
    wchar_t *w = lg_wide_path(dir, "*");
    HANDLE h = w ? FindFirstFileW(w, fd) : INVALID_HANDLE_VALUE;
    free(w);
    return h;
}

static int lg_find_name(const WIN32_FIND_DATAW *fd, char *out, size_t cap)
{
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, fd->cFileName, -1, out, (int)cap, NULL, NULL) > 0;
}

/* ==================================================================== */
/* folder trees (the retired overrides\ folders, modmigrate.c)           */
/* ==================================================================== */

typedef struct lg_walk {
    sh_modlegacy_zip *zip;
    size_t cap;
    char *err;
    size_t err_cap;
} lg_walk;

static int lg_walk_add(lg_walk *w, const char *rel, const wchar_t *full, int directory, uint64_t size)
{
    sh_modlegacy_zip_entry *e;
    if (w->zip->count == w->cap) {
        size_t next = w->cap ? w->cap * 2 : 128;
        sh_modlegacy_zip_entry *grown = (sh_modlegacy_zip_entry *)realloc(w->zip->entries, next * sizeof *grown);
        if (!grown) { LG_ERR(w->err, w->err_cap, "out of memory listing the folder"); return 0; }
        w->zip->entries = grown;
        w->cap = next;
    }
    e = &w->zip->entries[w->zip->count];
    memset(e, 0, sizeof *e);
    e->name = _strdup(rel);
    e->directory = directory;
    e->size = e->packed = directory ? 0 : size;
    if (!directory) {
        size_t n = wcslen(full) + 1;
        e->source = (wchar_t *)malloc(n * sizeof(wchar_t));
        if (e->source) memcpy(e->source, full, n * sizeof(wchar_t));
    }
    if (!e->name || (!directory && !e->source)) {
        free(e->name);
        free(e->source);
        LG_ERR(w->err, w->err_cap, "out of memory listing the folder");
        return 0;
    }
    w->zip->count++;
    if (!directory) {
        w->zip->files++;
        w->zip->expanded += size;
    }
    return 1;
}

static int lg_walk_dir(lg_walk *w, const wchar_t *dir, const char *prefix, unsigned depth)
{
    WIN32_FIND_DATAW fd;
    size_t n = wcslen(dir);
    wchar_t *pattern = (wchar_t *)malloc((n + 3) * sizeof(wchar_t));
    HANDLE h;
    int ok = 1;
    if (!pattern) { LG_ERR(w->err, w->err_cap, "out of memory listing the folder"); return 0; }
    memcpy(pattern, dir, n * sizeof(wchar_t));
    memcpy(pattern + n, L"\\*", 3 * sizeof(wchar_t));
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return 1;
        LG_ERR(w->err, w->err_cap, "cannot list '%s': %s", prefix[0] ? prefix : ".", sh_reason_win32(GetLastError(), prefix[0] ? prefix : "the folder", 0));
        return 0;
    }
    do {
        char *name, *rel;
        wchar_t *full;
        size_t fn;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) { LG_ERR(w->err, w->err_cap, "a file name under '%s' is not valid Unicode", prefix); ok = 0; break; }
        rel = sh_textf("%s%s%s", prefix, prefix[0] ? "/" : "", name);
        free(name);
        if (!rel) { LG_ERR(w->err, w->err_cap, "out of memory listing the folder"); ok = 0; break; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            LG_ERR(w->err, w->err_cap, "links and junctions are not supported: %s", rel);
            free(rel);
            ok = 0;
            break;
        }
        fn = wcslen(fd.cFileName);
        full = (wchar_t *)malloc((n + fn + 2) * sizeof(wchar_t));
        if (!full) { LG_ERR(w->err, w->err_cap, "out of memory listing the folder"); free(rel); ok = 0; break; }
        memcpy(full, dir, n * sizeof(wchar_t));
        full[n] = L'\\';
        memcpy(full + n + 1, fd.cFileName, (fn + 1) * sizeof(wchar_t));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = lg_walk_add(w, rel, full, 1, 0) && lg_walk_dir(w, full, rel, depth + 1);
        } else {
            uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            ok = lg_walk_add(w, rel, full, 0, size);
        }
        free(full);
        free(rel);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* beta.19 ps_file_compare: case-insensitive, then by bytes. */
static int lg_entry_cmp(const void *a, const void *b)
{
    const sh_modlegacy_zip_entry *x = (const sh_modlegacy_zip_entry *)a, *y = (const sh_modlegacy_zip_entry *)b;
    int c = _stricmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

static int lg_hex_bytes(const char *hex, unsigned char out[32])
{
    size_t i;
    for (i = 0; i < 32; i++) {
        unsigned v = 0, k;
        for (k = 0; k < 2; k++) {
            char c = hex[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return 0;
        }
        out[i] = (unsigned char)v;
    }
    return 1;
}

int sh_modlegacy_zip_tree(const sh_modlegacy_zip *zip, char tree_hex[65], char *err, size_t err_cap)
{
    size_t i;
    if (err && err_cap) err[0] = 0;
    if (!zip || !tree_hex) return 0;
    tree_hex[0] = 0;
    /* The beta.19 tree fingerprint over every entry, files hashed here. */
    lg_text stream = {0};
    for (i = 0; i < zip->count && !stream.failed; i++) {
        const sh_modlegacy_zip_entry *e = &zip->entries[i];
        lg_put(&stream, e->directory ? "d" : "f", 1);
        lg_put(&stream, e->name, strlen(e->name) + 1);
        if (!e->directory) {
            char hex[SH_MB_HASH_HEX + 1], *path = sh_mb_utf8(e->source);
            unsigned long long size = 0;
            unsigned char raw[32];
            int hashed = path && sh_mb_sha256_file(path, &size, hex) && size == e->size && lg_hex_bytes(hex, raw);
            free(path);
            if (!hashed) {
                LG_ERR(err, err_cap, "cannot hash '%s' (unreadable or changed size)", e->name);
                free(stream.p);
                return 0;
            }
            lg_put(&stream, (const char *)raw, 32);
        }
    }
    if (stream.failed || !stream.p) {
        free(stream.p);
        LG_ERR(err, err_cap, "out of memory fingerprinting the folder");
        return 0;
    }
    sh_mb_sha256_hex(stream.p, stream.n, tree_hex);
    free(stream.p);
    return 1;
}

unsigned long long sh_modlegacy_zip_carried_bytes(const sh_modlegacy_zip *zip, unsigned *files)
{
    unsigned long long bytes = 0;
    unsigned n = 0;
    size_t i;
    for (i = 0; zip && i < zip->count; i++) {
        char target[LG_PATH_MAX + 32];
        const sh_modlegacy_zip_entry *e = &zip->entries[i];
        if (e->directory || sh_modlegacy_map_path(e->name, target, sizeof target) != SH_MODLEGACY_PATH_ASSET) continue;
        if (!sh_mb_extension_allowed(target + 7) || !e->size) continue;
        bytes += e->size;
        n++;
    }
    if (files) *files = n;
    return bytes;
}

int sh_modlegacy_zip_from_folder(const char *folder, sh_modlegacy_zip *zip, char tree_hex[65], char *err,
                                 size_t err_cap)
{
    lg_walk w;
    wchar_t *root;
    DWORD a;
    size_t i;
    if (err && err_cap) err[0] = 0;
    if (tree_hex) tree_hex[0] = 0;
    if (!zip) return 0;
    memset(zip, 0, sizeof *zip);
    root = folder ? sh_mb_wpath(folder) : NULL;
    if (!root) { LG_ERR(err, err_cap, "no folder given"); return 0; }
    a = GetFileAttributesW(root);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        free(root);
        LG_ERR(err, err_cap, "%s is not a folder", folder);
        return 0;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
        free(root);
        LG_ERR(err, err_cap, "the folder is a link or junction");
        return 0;
    }
    memset(&w, 0, sizeof w);
    w.zip = zip;
    w.err = err;
    w.err_cap = err_cap;
    if (!lg_walk_dir(&w, root, "", 0)) { free(root); sh_modlegacy_zip_free(zip); return 0; }
    free(root);
    if (!zip->count) { LG_ERR(err, err_cap, "the folder is empty"); sh_modlegacy_zip_free(zip); return 0; }
    qsort(zip->entries, zip->count, sizeof *zip->entries, lg_entry_cmp);
    for (i = 1; i < zip->count; i++)
        if (!_stricmp(zip->entries[i - 1].name, zip->entries[i].name)) {
            LG_ERR(err, err_cap, "two paths differ only in case: %s", zip->entries[i].name);
            sh_modlegacy_zip_free(zip);
            return 0;
        }
    if (tree_hex && !sh_modlegacy_zip_tree(zip, tree_hex, err, err_cap)) { sh_modlegacy_zip_free(zip); return 0; }
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
        LG_ERR(err, err_cap, "cannot create '%s': %s", relative, sh_reason_win32(GetLastError(), relative, 0));
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
    char *target;                  /* malloc'd */
} lg_plan_file;

typedef struct lg_plan {
    sh_json_object root;           /* the root package.json */
    sh_json_object cvars, english, weapons;
    lg_plan_file *files;
    size_t count, cap;
    unsigned dropped, imported, policies, nested;
    unsigned unsupported;          /* files under assets/ a bundle cannot carry, left out */
    unsigned movies;               /* of them, .bswf movies (SWF content is not served by bundles) */
    char first_unsupported[LG_PATH_MAX + 96];
    unsigned gates_dropped;        /* requirements a bundle does not support, left out */
    char **providers;              /* full ZIP names named by resources manifests */
    size_t nproviders;
    int legacy_descriptor;
} lg_plan;

static void lg_request(const char *target, char *out, size_t cap);

static void lg_plan_free(lg_plan *p)
{
    size_t i;
    sh_json_object_free(&p->root);
    sh_json_object_free(&p->cvars);
    sh_json_object_free(&p->english);
    sh_json_object_free(&p->weapons);
    for (i = 0; i < p->count; i++) free(p->files[i].target);
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
                    st_fmt(full, sizeof full, "%s%s", component, provider);
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
    /* A carrier always held package.json; a folder in overrides\ may hold mod.json. */
    if (!descriptor) descriptor = sh_modlegacy_zip_find(zip, "mod.json");
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
            /* A first-generation package served every file under assets/; a
             * bundle carries only what Contract 1 admits. A file it cannot
             * carry (an unlisted type such as a .materials.txt sidecar, an
             * empty file, a path the request rules refuse) is left out and
             * counted, so the rest of the package still converts. */
            {
                char request[LG_PATH_MAX + 32], *why = NULL;
                lg_request(target, request, sizeof request);
                if (!sh_mb_extension_allowed(request)) why = _strdup("a file type mods do not carry");
                else if (!e->size) why = _strdup("empty file");
                else if (!sh_mb_check_request(request, &why) && !why) why = _strdup("unusable request path");
                if (why) {
                    size_t rn = strlen(request);
                    if (rn > 5 && !_stricmp(request + rn - 5, ".bswf")) p->movies++;
                    if (!p->unsupported++) st_putf(p->first_unsupported, sizeof p->first_unsupported, "%s (%s)", e->name, why);
                    free(why);
                    continue;
                }
            }
            if (p->count == p->cap) {
                size_t next = p->cap ? p->cap * 2 : 64;
                lg_plan_file *grown = (lg_plan_file *)realloc(p->files, next * sizeof *grown);
                if (!grown) { LG_ERR(err, err_cap, "out of memory planning the tree"); return 0; }
                p->files = grown;
                p->cap = next;
            }
            p->files[p->count].entry = e;
            if (!(p->files[p->count].target = _strdup(target))) { LG_ERR(err, err_cap, "out of memory planning the tree"); return 0; }
            p->count++;
        }
    }
    if (!p->count) { LG_ERR(err, err_cap, "the mod has no files under assets/ after its conversion"); return 0; }
    /* Windows folds case: two members on one path must hold the same bytes. */
    qsort(p->files, p->count, sizeof *p->files, lg_target_cmp);
    {
        size_t w = 1;
        for (i = 1; i < p->count; i++) {
            if (!_stricmp(p->files[i].target, p->files[w - 1].target)) {
                const sh_modlegacy_zip_entry *a = p->files[w - 1].entry, *b = p->files[i].entry;
                if (a->size != b->size || a->crc != b->crc) {
                    LG_ERR(err, err_cap, "'%s' and '%s' land on one path with different bytes", a->name, b->name);
                    for (; i < p->count; i++) free(p->files[i].target);   /* [0, w) are the kept ones */
                    p->count = w;
                    return 0;
                }
                free(p->files[i].target);
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
    st_cpy(out, cap, target + 7);
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

/* A display string the builder accepts: trimmed, whole. */
static int lg_set_display(sh_json_object *o, const char *key, const char *text)
{
    char *copy;
    size_t n;
    int ok;
    if (!text) return 1;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    copy = _strdup(text);
    if (!copy) return 0;
    n = strlen(copy);
    while (n && (copy[n - 1] == ' ' || copy[n - 1] == '\t' || copy[n - 1] == '\r' || copy[n - 1] == '\n')) copy[--n] = 0;
    ok = !n || lg_set_text(o, key, copy);
    free(copy);
    return ok;
}

/* Where a converted tree came from when it is not a map carrier: the
 * migration of a retired overrides\ folder (modmigrate.c). */
typedef struct lg_origin {
    const char *source;      /* the migration source: an authored or a delivered package */
    const char *folder;      /* overrides-relative folder, '/'-separated */
    const char *tree;        /* the beta.16-19 tree fingerprint (64 hex), or NULL */
    int (*build)(const char *folder_utf8, char **first_cause);   /* NULL: modbundle_build_folder */
} lg_origin;

/* Descriptor keys the converter writes itself; any other key of an
 * authored descriptor is kept for the builder to preserve under "extra". */
static int lg_handled_key(const char *key)
{
    /* The converter's own keys, and the keys this version retired (an id, a
     * key, shadows, stock, scope, player, extends, replaces, legacy,
     * composes, schema), which the one shape never carries. */
    static const char *const keys[] = { "id", "key", "name", "version", "description", "author", "homepage",
                                        "requirements", "strings", "hud", "shadows", "stock", "scope", "player",
                                        "extends", "replaces", "legacy", "composes", "schema", "files",
                                        "bundle", "format", SH_MB_BLOCK_KEY, NULL };
    size_t i;
    for (i = 0; keys[i]; i++) if (!strcmp(keys[i], key)) return 1;
    return 0;
}

/* mod.json for the converted bundle. */
static char *lg_descriptor(lg_plan *p, const char *id, const sh_legacy_header *h, const lg_origin *origin,
                           unsigned *shadows, size_t *out_len)
{
    sh_json_object d = {0};
    char ns[96];
    char *name = lg_text_of(sh_json_object_get(&p->root, "name")), *out = NULL, *text;
    const char *raw;
    size_t i;
    (void)h;
    *shadows = 0;
    /* A mod has no version (the package's own is dropped). */
    (void)origin;
    st_fmt(ns, sizeof ns, "mods/%s/", id);
    {
        char *trimmed = name;
        while (trimmed && (*trimmed == ' ' || *trimmed == '\t')) trimmed++;
        if (!lg_set_display(&d, "name", (trimmed && *trimmed) ? trimmed : id)) goto done;
    }
    raw = sh_json_object_get(&p->root, "description");
    if (raw && raw[0] == '"') sh_json_object_set(&d, "description", raw, 2);
    text = lg_text_of(sh_json_object_get(&p->root, "author"));
    if (!lg_set_display(&d, "author", text)) { free(text); goto done; }
    free(text);
    /* The builder refuses anything but an http(s) address; a first-generation
     * descriptor never had to meet that, so another value is not carried. */
    text = lg_text_of(sh_json_object_get(&p->root, "homepage"));
    if (text && (!_strnicmp(text, "https://", 8) || !_strnicmp(text, "http://", 7)) &&
        !strpbrk(text, " \t\r\n\"<>"))
        lg_set_text(&d, "homepage", text);
    free(text);
    if (p->cvars.count) {
        /* Only the supported gates set to 0 are bundle requirements; any other
         * cvar row of a first-generation package is left out and counted. */
        sh_json_object req = {0}, kept = {0};
        for (i = 0; i < p->cvars.count; i++) {
            const char *value = p->cvars.members[i].value_json;
            if (lg_supported_gate(p->cvars.members[i].key) && value &&
                (!strcmp(value, "0") || !strcmp(value, "\"0\"")))
                sh_json_object_set(&kept, p->cvars.members[i].key, "\"0\"", 2);
            else p->gates_dropped++;
        }
        if (kept.count && (!lg_set_object(&req, "cvars", &kept) || !lg_set_object(&d, "requirements", &req))) {
            sh_json_object_free(&kept); sh_json_object_free(&req); goto done;
        }
        sh_json_object_free(&kept);
        sh_json_object_free(&req);
    }
    if (origin) {
        /* An authored descriptor keeps its other keys (composes, an era B
         * schema, ...); the builder preserves them under "extra". */
        for (i = 0; i < p->root.count; i++)
            if (!lg_handled_key(p->root.members[i].key))
                sh_json_object_set(&d, p->root.members[i].key, p->root.members[i].value_json, 24);
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
    /* First-generation packages never used a mods/<id>/ namespace: the names
     * they carry outside it are game names (counted; the builder classifies
     * each under the current rules) or new names it refuses. The author
     * part is all the converter writes: the salt and the build are the
     * product's (sh_ms_assign_salt, the builder). */
    for (i = 0; i < p->count; i++) {
        char request[LG_PATH_MAX + 32];
        lg_request(p->files[i].target, request, sizeof request);
        if (!strstr(request, ns)) (*shadows)++;
    }
    out = sh_json_serialize_object(&d, 0, out_len);
done:
    free(name);
    sh_json_object_free(&d);
    return out;
}

static sh_modlegacy_salt_fn g_lg_salter;
void sh_modlegacy_set_salter(sh_modlegacy_salt_fn fn) { g_lg_salter = fn; }

/* ==================================================================== */
/* conversion                                                            */
/* ==================================================================== */

/* Read the mod.json the builder completed; fill the verdict. */
static int lg_take_bundle(const char *folder, sh_modlegacy_verdict *v, sh_modmanifest_row *info)
{
    size_t length = 0;
    char *text = lg_read_small_file(folder, "mod.json", &length);
    char err[160] = "";
    int ok;
    if (!text) { LG_ERR(v->reason, sizeof v->reason, "mod.json was not written"); return 0; }
    ok = sh_modmanifest_read_mod_json(text, length, info, err, sizeof err);
    free(text);
    if (!ok) { LG_ERR(v->reason, sizeof v->reason, "mod.json does not read: %s", err); return 0; }
    strcpy_s(v->salt, sizeof v->salt, info->salt);
    strcpy_s(v->build, sizeof v->build, info->build);
    v->size = info->size;
    st_cpy(v->folder, sizeof v->folder, folder);
    return 1;
}

/* Remove a folder tree this conversion created (a \\?\ path). */
static void lg_rmtree_w(const wchar_t *dir)
{
    size_t n = wcslen(dir) + MAX_PATH + 4;
    wchar_t *pattern = (wchar_t *)malloc(sizeof(wchar_t) * n), *child = (wchar_t *)malloc(sizeof(wchar_t) * n);
    WIN32_FIND_DATAW fd;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (pattern && child) {
        _snwprintf_s(pattern, n, _TRUNCATE, L"%s\\*", dir);
        h = FindFirstFileW(pattern, &fd);
    }
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf_s(child, n, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                lg_rmtree_w(child);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(child);
            else DeleteFileW(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(pattern);
    free(child);
    RemoveDirectoryW(dir);
}

/* The same for a UTF-8 path. */
static void lg_rmtree(const char *dir)
{
    wchar_t *w = lg_wide_path(dir, NULL);
    if (w) lg_rmtree_w(w);
    free(w);
}

/* An installed root mod whose carried files are exactly what the folder
 * just built carries (the same request paths with the same bytes): a package
 * converted before, or a mod that already serves it. The new folder is then
 * not kept and that mod answers the carrier. Returns 1 with its folder. */
static int lg_same_files(const sh_mb_bundle *a, const sh_mb_bundle *b)
{
    size_t i, j, na = 0, nb = 0;
    for (i = 0; i < a->file_count; i++) if (!a->files[i].stock) na++;
    for (i = 0; i < b->file_count; i++) if (!b->files[i].stock) nb++;
    if (!na || na != nb) return 0;
    for (i = 0; i < a->file_count; i++) {
        int hit = 0;
        if (a->files[i].stock) continue;
        for (j = 0; j < b->file_count && !hit; j++)
            hit = !b->files[j].stock && !strcmp(a->files[i].request, b->files[j].request) &&
                  !strcmp(a->files[i].blob, b->files[j].blob);
        if (!hit) return 0;
    }
    return 1;
}

static int lg_installed_copy(const char *mods, const char *built, char *out, size_t cap)
{
    char name[LG_PATH], other[LG_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    sh_mb_bundle b;
    int hit = 0;
    if (!sh_mb_load_folder(built, &b)) { sh_mb_free(&b); return 0; }
    h = lg_find_first(mods, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            sh_mb_bundle o;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !lg_find_name(&fd, name, sizeof name) || name[0] == '.' ||
                !_stricmp(name, "maps"))
                continue;
            st_fmt(other, sizeof other, "%s\\%s", mods, name);
            if (!_stricmp(other, built)) continue;
            if (sh_mb_load_folder(other, &o) && o.validated && (!strcmp(o.hash, b.hash) || lg_same_files(&o, &b))) {
                st_cpy(out, cap, other);
                hit = 1;
            }
            sh_mb_free(&o);
        } while (!hit && FindNextFileW(h, &fd));
        FindClose(h);
    }
    sh_mb_free(&b);
    return hit;
}

/* A free folder for a converted package: <id>, else "<id> (2)" ... "(9)".
 * The folder name is the author's; identity is the salt. */
static int lg_pick_folder(const char *mods, const char *id, char *out, size_t cap)
{
    int round;
    for (round = 1; round <= 9; round++) {
        if (round == 1) st_fmt(out, cap, "%s\\%s", mods, id);
        else st_fmt(out, cap, "%s\\%s (%d)", mods, id, round);
        if (lg_attr(out) == INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}


/* Plan a tree, write mod.json and assets\ into a fresh `tmp` folder and move
 * it to `folder`. `written` keeps what was created for the undo. Returns 1. */
static int lg_unpack(const sh_modlegacy_zip *zip, const lg_plan *plan, const char *descriptor, size_t descriptor_len,
                     const char *tmp, const char *folder, sh_modlegacy_verdict *v)
{
    lg_written written;
    char err[200] = "";
    size_t i;
    memset(&written, 0, sizeof written);
    (void)zip;
    {
        wchar_t *wtmp = lg_wide_path(tmp, NULL);
        int made = wtmp && lg_mkdir_tracked(&written, wtmp);
        free(wtmp);
        if (!made) { LG_ERR(v->reason, sizeof v->reason, "cannot create the unpack folder"); return 0; }
    }
    for (i = 0; i < plan->count; i++) {
        size_t n = 0;
        unsigned char *body = sh_modlegacy_zip_read(plan->files[i].entry, &n, err, sizeof err);
        int wrote = body && lg_write_member(&written, tmp, plan->files[i].target, body, n, err, sizeof err);
        free(body);
        if (!wrote) { LG_ERR(v->reason, sizeof v->reason, "unpack failed: %s", err); lg_undo(&written); return 0; }
    }
    if (!lg_write_member(&written, tmp, "mod.json", (const unsigned char *)descriptor, descriptor_len, err, sizeof err)) {
        LG_ERR(v->reason, sizeof v->reason, "unpack failed: %s", err);
        lg_undo(&written);
        return 0;
    }
    {
        wchar_t *wfrom = lg_wide_path(tmp, NULL), *wto = lg_wide_path(folder, NULL);
        BOOL moved = wfrom && wto && MoveFileExW(wfrom, wto, 0);
        DWORD error = GetLastError();
        free(wfrom);
        free(wto);
        if (moved) { lg_forget(&written); return 1; }
        SetLastError(error);
    }
    {
        DWORD error = GetLastError();
        DWORD a = lg_attr(folder);
        lg_undo(&written);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            LG_ERR(v->reason, sizeof v->reason, "cannot move the unpacked tree into place: %s", sh_reason_win32(error, folder, 0));
            return 0;
        }
        /* Another load installed the same digest meanwhile: use it. */
    }
    return 1;
}

/* The bundle id of a planned tree: the descriptor's id, else the carrier's
 * folder or descriptor key, else the origin's fallback, else "legacy". */
static void lg_pick_id(const lg_plan *plan, const sh_legacy_header *h, const char *fallback, char *id, size_t cap)
{
    char *idtext = lg_text_of(sh_json_object_get(&plan->root, "id"));
    if (!(idtext && sh_modlegacy_normalize_id(idtext, id, cap)) &&
        !(h->key_form != SH_LEGACY_KEY_DELIVERY && h->key_form != SH_LEGACY_KEY_OVERRIDES &&
          sh_modlegacy_normalize_id(h->key, id, cap)) &&
        !(fallback && sh_modlegacy_normalize_id(fallback, id, cap)))
        strcpy_s(id, cap, "legacy");
    free(idtext);
}

static char lg_layout(const lg_plan *plan, const sh_modlegacy_zip *zip)
{
    size_t i;
    if (plan->legacy_descriptor) return 'B';
    for (i = 0; i < plan->count; i++)
        if (_strnicmp(plan->files[i].entry->name + lg_component(zip, plan->files[i].entry->name), "assets/", 7))
            return 'B';
    return 'C';
}

static void lg_log_unpacked(const lg_plan *plan, const char *folder, ULONGLONG started)
{
    char *line = sh_textf(
        "LEGACY: unpacked %zu file(s) into %s in %llu ms (%u not an input and not kept, %u imported by a "
        "resources manifest, %u policy file(s) folded into mod.json, %u nested component(s), %u file(s) under "
        "assets/ a bundle cannot carry (%u of them .bswf movies)%s%s%s)",
        plan->count, folder, (unsigned long long)(GetTickCount64() - started), plan->dropped,
        plan->imported, plan->policies, plan->nested, plan->unsupported, plan->movies,
        plan->unsupported ? ", first: " : "", plan->first_unsupported,
        plan->gates_dropped ? "; unsupported cvar requirements left out" : "");
    backend_log(line ? line : "LEGACY: unpacked a package (out of memory for its line)");
    free(line);
}

/* Convert an opened tree into <data>\mods\<id> (else "<id> (2)", ...), salt
 * it from the package id, build it and take its bundle. A copy whose files
 * an installed mod already serves is not kept: the installed mod answers. A
 * conversion that fails leaves no folder. */
static int lg_convert_tree(const sh_modlegacy_zip *zip, const sh_legacy_header *h, const lg_origin *origin,
                           const char *fallback_id, sh_modlegacy_verdict *v, sh_modmanifest_row *info)
{
    lg_plan plan;
    char root[LG_PATH], mods[LG_PATH], folder[LG_PATH], tmp[LG_PATH], kept[LG_PATH];
    char err[200] = "", *cause = NULL, id[SH_MODLEGACY_ID_CAP], salt[SH_MODMANIFEST_SALT_CAP] = "";
    char *descriptor = NULL;
    size_t descriptor_len = 0;
    unsigned shadows = 0;
    int ok = 0, unpacked = 0;
    ULONGLONG started = GetTickCount64();

    memset(&plan, 0, sizeof plan);
    if (!lg_data_root(root, sizeof root)) { LG_ERR(v->reason, sizeof v->reason, "no data folder"); return 0; }
    st_fmt(mods, sizeof mods, "%s\\mods", root);
    if (!lg_plan_build(&plan, zip, err, sizeof err)) {
        LG_ERR(v->reason, sizeof v->reason, "%s", err);
        goto done;
    }
    v->layout = lg_layout(&plan, zip);
    lg_pick_id(&plan, h, fallback_id, id, sizeof id);
    strcpy_s(v->id, sizeof v->id, id);
    if (!lg_pick_folder(mods, id, folder, sizeof folder)) {
        LG_ERR(v->reason, sizeof v->reason, "no free folder for %s under %s", id, mods); goto done;
    }
    st_fmt(tmp, sizeof tmp, "%s\\.tmp-%s-%lu-%llu", mods, h->digest,
                GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    if (strlen(folder) + 32 >= LG_PATH || strlen(tmp) + 32 >= LG_PATH) {
        LG_ERR(v->reason, sizeof v->reason, "the data folder path is too long"); goto done;
    }

    descriptor = lg_descriptor(&plan, id, h, origin, &shadows, &descriptor_len);
    if (!descriptor) { LG_ERR(v->reason, sizeof v->reason, "the descriptor could not be converted"); goto done; }

    if (!lg_ensure_dir(mods)) { LG_ERR(v->reason, sizeof v->reason, "cannot create %s", mods); goto done; }
    if (!lg_unpack(zip, &plan, descriptor, descriptor_len, tmp, folder, v)) goto done;
    unpacked = 1;

    if (!g_lg_salter || g_lg_salter(folder, id, salt, &cause) < 0) {
        st_putf(v->reason, sizeof v->reason, "%s could not take a salt: %s", folder,
                g_lg_salter ? (cause && cause[0] ? cause : "no cause given") : "no salting is set");
        lg_rmtree(folder);
        goto done;
    }
    free(cause);
    cause = NULL;
    if (!((origin && origin->build) ? origin->build : modbundle_build_folder)(folder, &cause)) {
        st_putf(v->reason, sizeof v->reason, "it does not build under the current rules: %s",
                cause && cause[0] ? cause : "no cause given");
        lg_rmtree(folder);
        goto done;
    }
    if (lg_installed_copy(mods, folder, kept, sizeof kept)) {
        char line[2 * LG_PATH + 160];
        st_putf(line, sizeof line, "LEGACY: the installed %s serves this package; the converted copy is not kept", kept);
        backend_log(line);
        lg_rmtree(folder);
        strcpy_s(folder, sizeof folder, kept);
        unpacked = 0;
    }
    ok = lg_take_bundle(folder, v, info);
    if (!ok && unpacked) lg_rmtree(folder);
    if (ok) {
        v->files = (unsigned)plan.count;
        v->shadows = shadows;
        v->skipped = plan.unsupported;
        v->movies = plan.movies;
        st_cpy(v->first_skipped, sizeof v->first_skipped, plan.first_unsupported);
        if (unpacked) lg_log_unpacked(&plan, folder, started);
    }
done:
    free(cause);
    free(descriptor);
    lg_plan_free(&plan);
    return ok;
}

/* The folder the overrides\ migration made from the same tree, as its record
 * (mods\snapmap-plus.json "migration", "trees": {"<64 hex>": "<folder under
 * mods\>"}) lists it: a carrier "delivery.<tree>" is answered by it. */
static int lg_find_migrated(const char *root, const char *tree, char *folder, size_t cap)
{
    char mods[LG_PATH];
    char *text = NULL, *relative = NULL;
    sh_json_object record = {0}, trees = {0};
    int hit = 0;
    st_fmt(mods, sizeof mods, "%s\\mods", root);
    if (sh_mb_record_get(mods, "migration", &text) != 1 || !text) { free(text); return 0; }
    if (sh_json_parse_object(text, strlen(text), 24, &record) && lg_section(&record, "trees", &trees) &&
        (relative = lg_text_of(sh_json_object_get(&trees, tree))) != NULL && sh_modlegacy_safe_path(relative)) {
        size_t i;
        st_fmt(folder, cap, "%s\\%s", mods, relative);
        for (i = strlen(mods); folder[i]; i++) if (folder[i] == '/') folder[i] = '\\';
        hit = lg_file_exists(folder, "mod.json");
    }
    free(relative);
    sh_json_object_free(&trees);
    sh_json_object_free(&record);
    free(text);
    return hit;
}

static int lg_convert(const unsigned char *zipbytes, size_t length, const sh_legacy_header *h,
                      sh_modlegacy_verdict *v, sh_modmanifest_row *info)
{
    sh_modlegacy_zip zip;
    char err[200] = "", root[LG_PATH], folder[LG_PATH];
    int ok;

    memset(&zip, 0, sizeof zip);
    if (!strncmp(h->key, "delivery.", 9) && lg_is_hex64(h->key + 9) && lg_data_root(root, sizeof root) &&
        lg_find_migrated(root, h->key + 9, folder, sizeof folder)) {
        ok = lg_take_bundle(folder, v, info);
        if (ok) LG_ERR(v->reason, sizeof v->reason, "%s", "");
        return ok;
    }
    if (!sh_modlegacy_zip_open(zipbytes, length, &zip, err, sizeof err)) {
        LG_ERR(v->reason, sizeof v->reason, "undecodable ZIP: %s", err);
        return 0;
    }
    ok = lg_convert_tree(&zip, h, NULL, NULL, v, info);
    sh_modlegacy_zip_free(&zip);
    return ok;
}

static void lg_origin_of(const sh_modlegacy_origin *in, lg_origin *out)
{
    memset(out, 0, sizeof *out);
    if (!in) return;
    out->source = in->source;
    out->folder = in->folder;
    out->tree = in->tree;
    out->build = in->build;
}

int sh_modlegacy_convert_folder(const sh_modlegacy_zip *zip, const char *tree_hex,
                                const sh_modlegacy_origin *origin, sh_modlegacy_verdict *v)
{
    sh_legacy_header h;
    sh_modmanifest_row info;
    lg_origin o;
    int ok;
    if (!v) return 0;
    memset(v, 0, sizeof *v);
    v->verdict = SH_MODLEGACY_CLEAN;
    if (!zip || !tree_hex || !lg_is_hex64(tree_hex)) { LG_ERR(v->reason, sizeof v->reason, "no tree fingerprint"); return 0; }
    memset(&h, 0, sizeof h);
    st_fmt(h.key, sizeof h.key, "delivery.%s", tree_hex);
    memcpy(h.digest, tree_hex, 16);
    h.digest[16] = 0;
    h.key_form = SH_LEGACY_KEY_DELIVERY;
    strcpy_s(v->key, sizeof v->key, h.key);
    strcpy_s(v->digest, sizeof v->digest, h.digest);
    v->key_form = h.key_form;
    memset(&info, 0, sizeof info);
    /* The folder an earlier pass made of this tree answers (the migration
     * record's trees), else an installed mod with the same files does
     * (lg_installed_copy). */
    {
        char root[LG_PATH], folder[LG_PATH];
        if (lg_data_root(root, sizeof root) && lg_find_migrated(root, tree_hex, folder, sizeof folder)) {
            ok = lg_take_bundle(folder, v, &info);
            v->verdict = ok ? SH_MODLEGACY_CONVERT : SH_MODLEGACY_CLEAN;
            sh_modmanifest_row_free(&info);
            if (ok) return 1;
            memset(&info, 0, sizeof info);
        }
    }
    lg_origin_of(origin, &o);
    if (!o.tree) o.tree = tree_hex;
    ok = lg_convert_tree(zip, &h, &o, origin ? origin->fallback_id : NULL, v, &info);
    v->verdict = ok ? SH_MODLEGACY_CONVERT : SH_MODLEGACY_CLEAN;
    sh_modmanifest_row_free(&info);
    return ok;
}

int sh_modlegacy_stage_folder(const sh_modlegacy_zip *zip, const sh_modlegacy_origin *origin,
                              const char *dest, sh_modlegacy_verdict *v)
{
    sh_legacy_header h;
    lg_plan plan;
    lg_origin o;
    char err[200] = "", id[SH_MODLEGACY_ID_CAP], tmp[LG_PATH];
    char *descriptor = NULL;
    size_t descriptor_len = 0;
    unsigned shadows = 0;
    int ok = 0;
    ULONGLONG started = GetTickCount64();
    if (!v) return 0;
    memset(v, 0, sizeof *v);
    v->verdict = SH_MODLEGACY_CLEAN;
    if (!zip || !dest || strlen(dest) + 40 >= LG_PATH) { LG_ERR(v->reason, sizeof v->reason, "no destination"); return 0; }
    memset(&h, 0, sizeof h);
    memset(&plan, 0, sizeof plan);
    lg_origin_of(origin, &o);
    st_fmt(h.key, sizeof h.key, "overrides/%s", (origin && origin->folder) ? origin->folder : "");
    if (o.tree && lg_is_hex64(o.tree)) { memcpy(h.digest, o.tree, 16); h.digest[16] = 0; }
    h.key_form = SH_LEGACY_KEY_OVERRIDES;
    strcpy_s(v->key, sizeof v->key, h.key);
    strcpy_s(v->digest, sizeof v->digest, h.digest);
    v->key_form = h.key_form;
    if (!lg_plan_build(&plan, zip, err, sizeof err)) { LG_ERR(v->reason, sizeof v->reason, "%s", err); goto done; }
    v->layout = lg_layout(&plan, zip);
    lg_pick_id(&plan, &h, origin ? origin->fallback_id : NULL, id, sizeof id);
    descriptor = lg_descriptor(&plan, id, &h, &o, &shadows, &descriptor_len);
    if (!descriptor) { LG_ERR(v->reason, sizeof v->reason, "the descriptor could not be converted"); goto done; }
    st_fmt(tmp, sizeof tmp, "%s.tmp-%lu", dest, GetCurrentProcessId());
    if (!lg_unpack(zip, &plan, descriptor, descriptor_len, tmp, dest, v)) goto done;
    strcpy_s(v->id, sizeof v->id, id);
    st_cpy(v->folder, sizeof v->folder, dest);
    v->files = (unsigned)plan.count;
    v->shadows = shadows;
    v->skipped = plan.unsupported;
    v->movies = plan.movies;
    st_cpy(v->first_skipped, sizeof v->first_skipped, plan.first_unsupported);
    lg_log_unpacked(&plan, dest, started);
    ok = 1;
done:
    free(descriptor);
    lg_plan_free(&plan);
    v->verdict = ok ? SH_MODLEGACY_CONVERT : SH_MODLEGACY_CLEAN;
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

/* One converting thread (the store tool's); the memo is not locked. */
static lg_memo *lg_memo_find(const sh_modlegacy_carrier *c)
{
    size_t i;
    unsigned long long chars = lg_chars(c);
    for (i = 0; i < g_memo_count; i++)
        if (!strcmp(g_memo[i].key, c->header.key) && !strcmp(g_memo[i].digest, c->header.digest) &&
            g_memo[i].total == c->header.total && g_memo[i].chars == chars) return &g_memo[i];
    return NULL;
}

/* A row's own copy of `from`: its names, name and choices duplicated. */
static void lg_row_copy(sh_modmanifest_row *to, const sh_modmanifest_row *from)
{
    size_t i;
    *to = *from;
    to->names = NULL;
    to->name_count = 0;
    to->name = NULL;
    to->choices = from->choices ? _strdup(from->choices) : NULL;
    if (from->name) (void)sh_modmanifest_row_set_name(to, from->name);
    for (i = 0; i < from->name_count; i++) sh_modmanifest_row_add_name(to, from->names[i]);
}

static void lg_memo_store(const sh_modlegacy_carrier *c, const sh_modlegacy_verdict *v,
                          const sh_modmanifest_row *info)
{
    lg_memo *m;
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
        lg_row_copy(&m->info, info);
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
        st_putf(line, sizeof line,
            "LEGACY: smpkg.%s digest %s (%u/%u shards, %s): CONVERT -> %s, package %s, salt %s, "
            "build %.16s, layout era %c, %u file(s), %u name(s) outside mods/<salt>/%s",
            v->key, v->digest, v->present, v->total, sh_map_legacy_key_form_name(v->key_form),
            v->folder, v->id, v->salt, v->build, v->layout ? v->layout : '?', v->files, v->shadows,
            memo ? " [converted earlier this session]" : "");
    else
        st_putf(line, sizeof line,
            "LEGACY: smpkg.%s digest %s (%u/%u shards, %s): CLEAN -- %s; its rows are removed from the map",
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
    unsigned converted = 0, cleaned = 0, package_rows = 0, unreleased = 0;
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
                lg_row_copy(&info, &memo->info);
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
            if (!placed_ready) {
                nplaced = sh_modmanifest_placed_names(json, length, &placed, &placed_failed);
                placed_ready = 1;
            }
            memset(&row, 0, sizeof row);
            strcpy_s(row.salt, sizeof row.salt, v->salt);
            strcpy_s(row.build, sizeof row.build, v->build);
            (void)sh_modmanifest_row_set_name(&row, sh_modmanifest_row_name(&info)[0] ? sh_modmanifest_row_name(&info) : v->id);
            row.size = v->size;
            for (k = 0; k < info.name_count && nplaced; k++) {
                const char *key = info.names[k];
                if (bsearch(&key, placed, nplaced, sizeof *placed, lg_stricmp_ptr))
                    sh_modmanifest_row_add_name(&row, key);
            }
            if (!rows.count || strcmp(rows.rows[rows.count - 1].build, row.build)) {
                size_t r;
                int dup = 0;
                for (r = 0; r < rows.count; r++) if (!strcmp(rows.rows[r].build, row.build)) dup = 1;
                if (!dup) sh_modmanifest_list_add(&rows, &row);
            }
            sh_modmanifest_row_free(&row);
        } else cleaned++;
        sh_modmanifest_row_free(&info);
    }
    sh_modlegacy_carriers_free(carriers, ncarriers);
    sh_modmanifest_names_free(placed, nplaced);

    stripped = sh_map_legacy_payload_strip_all(json, length, &stripped_len, &package_rows, &unreleased,
                                               &doc_failed);
    if (!stripped) {
        if (doc_failed) backend_log("LEGACY: the package rows could not be removed: the map's text does not read cleanly");
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
            for (m = 0; m < merged.count; m++) if (!strcmp(merged.rows[m].build, rows.rows[r].build)) dup = 1;
            if (!dup) sh_modmanifest_list_add(&merged, &rows.rows[r]);
        }
        final = sh_modmanifest_write(stripped, stripped_len, &merged, &final_len, err, sizeof err);
        sh_modmanifest_list_free(&merged);
        if (final) HeapFree(GetProcessHeap(), 0, stripped);
        else {
            st_putf(line, sizeof line,
                "LEGACY: the rows of the converted mods could not be written into the map (%s); the mods are in the "
                "mods folder", err[0] ? err : "unknown");
            backend_log(line);
            final = stripped;
            final_len = stripped_len;
        }
    }
    if (ncarriers) {
        st_putf(line, sizeof line,
            "LEGACY: %zu carrier(s): %u converted, %u cleaned; %u package row(s)%s removed, %zu -> %zu bytes",
            ncarriers, converted, cleaned, package_rows, unreleased ? " and unreleased v2 rows" : "",
            length, final_len);
        backend_log(line);
    }
    sh_modmanifest_list_free(&rows);
    lg_set_report(report, report ? ncarriers : 0);
    if (out_length) *out_length = final_len;
    return final;
}
