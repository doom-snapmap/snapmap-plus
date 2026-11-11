/* map_old.c -- the map shapes earlier Snapmap+ builds wrote, read and
 * converted for the store tool (map_old.h). Store tool only: never linked
 * into the game DLL. The variable envelope, the smpmod1 row reader, the
 * settings rows and the smnav1 header grammar moved here unchanged from
 * map_shards.c, modmanifest.c, map_settings.c and navmesh.c. */
#include <limits.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_old.h"
#include "map_shards.h"
#include "map_carrier.h"
#include "map_legacy_payload.h"
#include "modmanifest.h"
#include "modbundle.h"
#include "modsalt.h"
#include "navmesh.h"
#include "config_json.h"

#define MO_NAV_MAGIC      "smnav1."
#define MO_NAV_MAGIC_LEN  7

/* ==================================================================== */
/* the shard scanner                                                     */
/* ==================================================================== */

/* Bound the name-to-initialValue search window; engine layouts usually
 * separate them by 100-350 bytes.
 */
#define SHARD_VALUE_WINDOW 4096

/* Is the string starting at `quote` (the opening '"' of the header) the value
 * of a "name" key?  ..."name" <ws> : <ws> "smpkg... */
static int shard_is_name_value(const char *json, const char *quote)
{
    const char *r = quote - 1;
    while (r >= json && sh_shard_is_ws(*r)) r--;
    if (r < json || *r != ':') return 0;
    r--;
    while (r >= json && sh_shard_is_ws(*r)) r--;
    if (r - 5 < json) return 0;
    return memcmp(r - 5, "\"name\"", 6) == 0;
}

/* Find the base64 span after a header. Return 1 with chunk and chunk_len,
 * including empty chunks; 0 means unreadable.
 */
static int shard_find_chunk(const char *hdr_end, const char *end,
                            const char **chunk, size_t *chunk_len)
{
    size_t window = (size_t)(end - hdr_end);
    const char *key, *v;
    size_t n = 0;
    if (window > SHARD_VALUE_WINDOW) window = SHARD_VALUE_WINDOW;
    key = sh_shard_find(hdr_end, window, "\"initialValue\"", 14);
    if (!key) return 0;
    v = key + 14;
    while (v < end && sh_shard_is_ws(*v)) v++;
    if (v >= end || *v != ':') return 0;
    v++;
    while (v < end && sh_shard_is_ws(*v)) v++;
    if (v >= end || *v != '"') return 0;
    v++;
    *chunk = v;
    while (v + n < end && n <= SH_SHARD_MAX_CHUNK) {
        char c = v[n];
        if (c == '"') { *chunk_len = n; return 1; }
        if (!sh_shard_is_b64(c)) return 0;   /* incl '\\': never legal base64 */
        n++;
    }
    return 0;   /* no closing quote within the cap */
}

int sh_shard_next(const char *json, size_t len, const char *magic, size_t magic_len,
                  size_t *pos, const char **hdr, size_t *hdr_len,
                  const char **chunk, size_t *chunk_len)
{
    const char *end;
    if (!json || !magic || !pos || !hdr || !hdr_len || !chunk || !chunk_len) return 0;
    end = json + len;
    while (*pos < len) {
        const char *p = sh_shard_find(json + *pos, len - *pos, magic, magic_len);
        const char *q;
        size_t window, n;
        if (!p) return 0;
        *pos = (size_t)(p - json) + 1;   /* resume past this occurrence next time */
        if (p == json || p[-1] != '"') continue;    /* magic must start the string */
        if (!shard_is_name_value(json, p - 1)) continue;

        /* the closing quote, within the header cap */
        window = (size_t)(end - p);
        if (window > SH_SHARD_HEADER_MAX) window = SH_SHARD_HEADER_MAX;
        q = (const char *)memchr(p, '"', window);
        if (!q) continue;
        n = (size_t)(q - p);

        /* Require the nearby snapVarInfo_t marker to reject ordinary entity
         * names resembling shard headers.
         */
        {
            size_t w = (size_t)(end - (q + 1));
            if (w > 256) w = 256;
            if (!sh_shard_find(q + 1, w, "snapVarInfo_t", 13)) continue;
        }

        *hdr = p;
        *hdr_len = n;
        *chunk = NULL;
        *chunk_len = 0;
        if (!shard_find_chunk(q + 1, end, chunk, chunk_len)) *chunk = NULL;
        *pos = (size_t)(q + 1 - json);
        return 1;
    }
    return 0;
}

/* ==================================================================== */
/* strip                                                                 */
/* ==================================================================== */

typedef struct shard_cut { size_t from, to; } shard_cut;

static int shard_cut_cmp(const void *a, const void *b)
{
    const shard_cut *x = (const shard_cut *)a, *y = (const shard_cut *)b;
    if (x->from < y->from) return -1;
    if (x->from > y->from) return 1;
    return 0;
}

char *sh_shard_strip(const char *json, size_t len, const char *magic, size_t magic_len,
                     sh_shard_filter_fn filter, void *ctx, size_t max_cuts,
                     size_t *out_len, unsigned *elements_out, unsigned *runs_out,
                     int *doc_failed)
{
    sh_shard_doc doc;
    shard_cut *cuts = NULL;
    size_t cut_count = 0, pos = 0, i, w = 0, elements = 0;
    const char *hdr, *chunk;
    size_t hdr_len, chunk_len;
    char *out = NULL;

    if (out_len) *out_len = 0;
    if (elements_out) *elements_out = 0;
    if (runs_out) *runs_out = 0;
    if (doc_failed) *doc_failed = 0;
    if (!json || len == 0 || max_cuts == 0) return NULL;

    /* Skip the structural pass for payload-free maps. */
    if (!sh_shard_find(json, len, magic, magic_len)) return NULL;
    if (!sh_shard_doc_build(json, len, &doc)) {
        if (doc_failed) *doc_failed = 1;
        return NULL;
    }

    cuts = (shard_cut *)HeapAlloc(GetProcessHeap(), 0, max_cuts * sizeof(shard_cut));
    if (!cuts) { sh_shard_doc_free(&doc); return NULL; }

    while (cut_count < max_cuts &&
           sh_shard_next(json, len, magic, magic_len, &pos, &hdr, &hdr_len, &chunk, &chunk_len)) {
        int el;
        size_t from, to;
        if (filter && !filter(hdr, hdr_len, ctx)) continue;
        el = sh_shard_doc_array_element(&doc, sh_shard_doc_innermost(&doc, pos));
        if (el < 0) continue;
        from = doc.c[el].open;
        to   = doc.c[el].close + 1;
        /* The scanner advances in source order; repeated headers inside one
         * variable are consecutive. Avoid quadratic duplicate searches. */
        if (cut_count && cuts[cut_count - 1].from == from) continue;
        if (cut_count && cuts[cut_count - 1].from > from) {
            if (doc_failed) *doc_failed = 1;
            HeapFree(GetProcessHeap(), 0, cuts); sh_shard_doc_free(&doc); return NULL;
        }

        cuts[cut_count].from = from;
        cuts[cut_count].to = to;
        cut_count++;
        elements++;
    }

    if (cut_count == 0) {
        HeapFree(GetProcessHeap(), 0, cuts);
        sh_shard_doc_free(&doc);
        return NULL;
    }

    qsort(cuts, cut_count, sizeof(shard_cut), shard_cut_cmp);

    /* Merge adjacent removal spans so each run claims one delimiter. */
    {
        size_t w2 = 0;
        for (i = 1; i < cut_count; i++) {
            size_t g = cuts[w2].to;
            while (g < len && sh_shard_is_ws(json[g])) g++;
            if (g < len && json[g] == ',') {
                g++;
                while (g < len && sh_shard_is_ws(json[g])) g++;
                if (g == cuts[i].from) { cuts[w2].to = cuts[i].to; continue; }
            }
            cuts[++w2] = cuts[i];
        }
        cut_count = w2 + 1;
    }

    /* Remove the preceding comma, or the following comma for an initial run. */
    for (i = 0; i < cut_count; i++) {
        size_t b = cuts[i].from;
        while (b > 0 && sh_shard_is_ws(json[b - 1])) b--;
        if (b > 0 && json[b - 1] == ',') {
            cuts[i].from = b - 1;
        } else {
            size_t a = cuts[i].to;
            while (a < len && sh_shard_is_ws(json[a])) a++;
            if (a < len && json[a] == ',') cuts[i].to = a + 1;
        }
    }
    /* Reject overlapping cuts rather than corrupting JSON. */
    for (i = 1; i < cut_count; i++) {
        if (cuts[i].from < cuts[i - 1].to) {
            if (doc_failed) *doc_failed = 1;
            HeapFree(GetProcessHeap(), 0, cuts);
            sh_shard_doc_free(&doc);
            return NULL;
        }
    }

    out = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (!out) {
        HeapFree(GetProcessHeap(), 0, cuts);
        sh_shard_doc_free(&doc);
        return NULL;
    }
    pos = 0;
    for (i = 0; i < cut_count; i++) {
        size_t run = cuts[i].from - pos;
        memcpy(out + w, json + pos, run);
        w += run;
        pos = cuts[i].to;
    }
    memcpy(out + w, json + pos, len - pos);
    w += len - pos;
    out[w] = '\0';

    HeapFree(GetProcessHeap(), 0, cuts);
    sh_shard_doc_free(&doc);
    /* Delivery variables are not map state. Keep the engine's string count
     * consistent after removing them, including a previously empty bucket. */
    {
        sh_shard_doc remaining;
        int variables, bucket, alloc;
        size_t from, to;
        if (!sh_shard_doc_build(out, w, &remaining)) {
            if (doc_failed) *doc_failed = 1;
            HeapFree(GetProcessHeap(), 0, out); return NULL;
        }
        variables = sh_shard_doc_member(out, w, &remaining, 0, "variables");
        bucket = variables >= 0 ? sh_shard_doc_member(out, w, &remaining, variables, "string") : -1;
        alloc = variables >= 0 ? sh_shard_doc_member(out, w, &remaining, variables, "allocCount") : -1;
        if (bucket >= 0 && alloc >= 0 && sh_shard_doc_flat_element(out, &remaining, alloc, 4, &from, &to)) {
            char number[16];
            int digits = _snprintf_s(number, sizeof(number), _TRUNCATE, "%u", sh_shard_doc_array_count(out, &remaining, bucket));
            if (digits < 0 || w - (to - from) + (size_t)digits > len) {
                sh_shard_doc_free(&remaining); HeapFree(GetProcessHeap(), 0, out);
                if (doc_failed) *doc_failed = 1;
                return NULL;
            }
            memmove(out + from + digits, out + to, w - to + 1);
            memcpy(out + from, number, (size_t)digits); w = w - (to - from) + (size_t)digits;
        }
        sh_shard_doc_free(&remaining);
    }
    if (out_len) *out_len = w;
    if (elements_out) *elements_out = (unsigned)elements;
    if (runs_out) *runs_out = (unsigned)cut_count;
    return out;
}

/* ==================================================================== */
/* insert                                                                */
/* ==================================================================== */

/* Write the compact snapVarString_t envelope. */
static size_t shard_write_var(char *out, const char *header, const char *chunk, size_t chunk_len)
{
    static const char PRE[] =
        "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,"
        "\"~type\":\"|pointer\"},\"name\":\"";
    static const char MID[] = "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"";
    static const char POST[] = "\",\"~type\":\"snapVarString_t\"}";
    size_t w = 0, n;
    n = sizeof PRE - 1;      if (out) memcpy(out + w, PRE, n);      w += n;
    n = strlen(header);      if (out) memcpy(out + w, header, n);   w += n;
    n = sizeof MID - 1;      if (out) memcpy(out + w, MID, n);      w += n;
    if (out) memcpy(out + w, chunk, chunk_len);                     w += chunk_len;
    n = sizeof POST - 1;     if (out) memcpy(out + w, POST, n);     w += n;
    return w;
}

static void shard_err(char *err, size_t cap, const char *msg)
{
    if (!err || cap == 0) return;
    strncpy_s(err, cap, msg, _TRUNCATE);
}

char *sh_shard_insert(const char *json, size_t len,
                      const sh_shard_out *shards, size_t count,
                      size_t *out_len, char *err, size_t err_cap)
{
    sh_shard_doc doc;
    char *out = NULL;
    size_t i, insert, w = 0, need;
    int vars, bucket, alloc;
    unsigned existing;

    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = '\0';
    if (!json || len == 0 || !shards || count == 0) {
        shard_err(err, err_cap, "insert called with nothing to insert");
        return NULL;
    }

    if (!sh_shard_doc_build(json, len, &doc)) {
        shard_err(err, err_cap, "map JSON did not read cleanly; nothing embedded");
        return NULL;
    }
    vars = sh_shard_doc_member(json, len, &doc, 0, "variables");
    if (vars < 0 || doc.c[vars].kind != '{') {
        shard_err(err, err_cap, "map has no variables block");
        goto fail;
    }
    bucket = sh_shard_doc_member(json, len, &doc, vars, "string");
    if (bucket < 0 || doc.c[bucket].kind != '[') {
        shard_err(err, err_cap, "map has no variables.string list");
        goto fail;
    }
    alloc = sh_shard_doc_member(json, len, &doc, vars, "allocCount");
    if (alloc < 0 || doc.c[alloc].kind != '[') {
        shard_err(err, err_cap, "map has no variables.allocCount list");
        goto fail;
    }

    existing = sh_shard_doc_array_count(json, &doc, bucket);
    if (len > INT_MAX || count > (size_t)INT_MAX - existing) {
        shard_err(err, err_cap, "map exceeds the native string or variable-count representation"); goto fail;
    }
    need = len;
    for (i = 0; i < count; i++) {
        size_t added;
        if (!shards[i].header || !shards[i].chunk || strlen(shards[i].header) > SH_SHARD_HEADER_MAX ||
            shards[i].chunk_len > SH_SHARD_MAX_CHUNK) {
            shard_err(err, err_cap, "invalid shard header or chunk"); goto fail;
        }
        added = shard_write_var(NULL, shards[i].header, shards[i].chunk, shards[i].chunk_len) + (existing || i ? 1u : 0u);
        if (added > (size_t)INT_MAX - need) {
            shard_err(err, err_cap, "complete map with embedded packages exceeds the native 2 GiB string representation"); goto fail;
        }
        need += added;
    }
    {
        size_t from, to, final_length;
        char count_text[16]; int digits;
        if (!sh_shard_doc_flat_element(json, &doc, alloc, 4, &from, &to)) {
            shard_err(err, err_cap, "map has no variables.allocCount[4]"); goto fail;
        }
        digits = _snprintf_s(count_text, sizeof(count_text), _TRUNCATE, "%u", (unsigned)(existing + count));
        final_length = need - (to - from) + (size_t)digits;
        if (digits < 0 || final_length > INT_MAX) {
            shard_err(err, err_cap, "complete map exceeds the native string representation"); goto fail;
        }
    }
    out = (char *)HeapAlloc(GetProcessHeap(), 0, need + 1);
    if (!out) { shard_err(err, err_cap, "out of memory building the map"); goto fail; }

    insert = doc.c[bucket].close;       /* just before the ']' */
    memcpy(out, json, insert);
    w = insert;
    for (i = 0; i < count; i++) {
        if (existing || i) out[w++] = ',';
        w += shard_write_var(out + w, shards[i].header, shards[i].chunk, shards[i].chunk_len);
    }
    memcpy(out + w, json + insert, len - insert);
    w += len - insert;
    out[w] = '\0';
    sh_shard_doc_free(&doc);

    /* Update allocCount[STRING] after insertion, which moves its byte offset. */
    {
        sh_shard_doc doc2;
        int vars2, alloc2;
        size_t from, to;
        if (!sh_shard_doc_build(out, w, &doc2)) {
            shard_err(err, err_cap, "the embedded map did not read back cleanly");
            HeapFree(GetProcessHeap(), 0, out);
            return NULL;
        }
        vars2 = sh_shard_doc_member(out, w, &doc2, 0, "variables");
        alloc2 = vars2 >= 0 ? sh_shard_doc_member(out, w, &doc2, vars2, "allocCount") : -1;
        if (alloc2 < 0 || !sh_shard_doc_flat_element(out, &doc2, alloc2, 4, &from, &to)) {
            sh_shard_doc_free(&doc2);
            shard_err(err, err_cap, "map has no variables.allocCount[4] slot to update");
            HeapFree(GetProcessHeap(), 0, out);
            return NULL;
        }
        {
            char text[16];
            int n = _snprintf_s(text, sizeof text, _TRUNCATE, "%u",
                                (unsigned)(existing + count));
            size_t tail = w - to;
            char *fin = (char *)HeapAlloc(GetProcessHeap(), 0, from + (size_t)n + tail + 1);
            if (!fin) {
                sh_shard_doc_free(&doc2);
                shard_err(err, err_cap, "out of memory writing the variable count");
                HeapFree(GetProcessHeap(), 0, out);
                return NULL;
            }
            memcpy(fin, out, from);
            memcpy(fin + from, text, (size_t)n);
            memcpy(fin + from + n, out + to, tail);
            w = from + (size_t)n + tail;
            fin[w] = '\0';
            HeapFree(GetProcessHeap(), 0, out);
            out = fin;
        }
        sh_shard_doc_free(&doc2);
    }

    if (out_len) *out_len = w;
    return out;

fail:
    sh_shard_doc_free(&doc);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    return NULL;
}

/* ==================================================================== */
/* small JSON helpers (as modmanifest.c had them)                        */
/* ==================================================================== */

/* ==================================================================== */

static const char *mo_ws(const char *p, const char *end)
{
    while (p < end && sh_shard_is_ws(*p)) p++;
    return p;
}

/* Past one string literal starting at its opening quote; NULL if unterminated. */
static const char *mo_skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') return NULL;
    for (p++; p < end; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

/* Past one JSON value of any kind; NULL on malformed input. The value is
 * validated properly by the caller's parser; this only finds its extent. */
static const char *mo_skip_value(const char *p, const char *end)
{
    int depth = 0;
    p = mo_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return mo_skip_string(p, end);
    if (*p != '{' && *p != '[') {
        while (p < end && *p != ',' && *p != '}' && *p != ']' && !sh_shard_is_ws(*p)) p++;
        return p;
    }
    for (; p < end; p++) {
        if (*p == '"') {
            p = mo_skip_string(p, end);
            if (!p) return NULL;
            p--;
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') {
            if (--depth == 0) return p + 1;
        }
    }
    return NULL;
}

/* Iterate the elements of an array value. *cursor starts at '[' (or before it
 * with whitespace). Returns 1 with the next element span, 0 at the end, -1 on
 * malformed input. */
static int mo_array_next(const char **cursor, const char *end, const char **elem, size_t *elem_len)
{
    const char *p = mo_ws(*cursor, end), *after;
    if (p >= end) return -1;
    if (*p == '[') {
        p = mo_ws(p + 1, end);
        if (p < end && *p == ']') { *cursor = p + 1; return 0; }
    } else if (*p == ',') {
        p = mo_ws(p + 1, end);
    } else if (*p == ']') {
        *cursor = p + 1;
        return 0;
    } else return -1;
    after = mo_skip_value(p, end);
    if (!after) return -1;
    *elem = p;
    *elem_len = (size_t)(after - p);
    *cursor = mo_ws(after, end);
    if (*cursor >= end || (**cursor != ',' && **cursor != ']')) return -1;
    return 1;
}

/* Decode a string literal (quotes included) into a malloc'd NUL-terminated
 * string. Refuses embedded NULs. */
static char *mo_decode(const char *literal, size_t length)
{
    char *out;
    size_t got = 0;
    if (!literal || length < 2 || literal[0] != '"') return NULL;
    out = (char *)malloc(length);
    if (!out) return NULL;
    if (!sh_native_json_decode_string(literal, length, out, length, &got) || strlen(out) != got) {
        free(out);
        return NULL;
    }
    return out;
}

/* Copy a decoded string member of `object` into a fixed buffer. */
static int mo_member_text(const sh_json_object *object, const char *key, char *out, size_t cap,
                          int required)
{
    const char *raw = sh_json_object_get(object, key);
    char *text;
    size_t n;
    if (!raw) { if (out && cap) out[0] = 0; return !required; }
    text = mo_decode(raw, strlen(raw));
    if (!text) return 0;
    n = strlen(text);
    if (n >= cap) { free(text); return 0; }
    memcpy(out, text, n + 1);
    free(text);
    return 1;
}

static int mo_member_uint(const sh_json_object *object, const char *key, unsigned long long *out)
{
    const char *raw = sh_json_object_get(object, key), *p;
    unsigned long long v = 0;
    if (!raw || !*raw) return 0;
    for (p = raw; *p; p++) {
        if (!sh_shard_is_digit(*p)) return 0;
        if (v > (ULLONG_MAX - 9) / 10) return 0;
        v = v * 10 + (unsigned long long)(*p - '0');
    }
    *out = v;
    return 1;
}

/* A slot as the manifest stores it: printable, no quote, tab or backslash. */
static int mo_slot_ok(const char *slot)
{
    size_t i, n = slot ? strlen(slot) : 0;
    if (!n || n >= SH_MODMANIFEST_SLOT_MAX) return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)slot[i] <= 0x20 || slot[i] == '"' || slot[i] == '\\' || (unsigned char)slot[i] >= 0x7f) return 0;
    return 1;
}

/* The key for `slot` in "slot\tkey\n" lines, copied to key (when not NULL). 1 found. */
static int mo_choice_find(const char *lines, const char *slot, char *key, size_t cap)
{
    const char *p = lines;
    size_t sn = slot ? strlen(slot) : 0;
    while (p && *p) {
        const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        if (!tab || !nl || tab > nl) break;
        if ((size_t)(tab - p) == sn && !strncmp(p, slot, sn)) {
            if (key && cap) {
                size_t kn = (size_t)(nl - tab - 1);
                if (kn >= cap) kn = cap - 1;
                memcpy(key, tab + 1, kn);
                key[kn] = 0;
            }
            return 1;
        }
        p = nl + 1;
    }
    return 0;
}

/* Append "slot\tkey\n" unless the slot is there. 0 when it does not fit. */
static int mo_choice_add(char *lines, size_t cap, const char *slot, const char *key)
{
    size_t at = strlen(lines), need = strlen(slot) + strlen(key) + 2;
    if (mo_choice_find(lines, slot, NULL, 0)) return 1;
    if (at + need + 1 > cap) return 0;
    _snprintf_s(lines + at, cap - at, _TRUNCATE, "%s\t%s\n", slot, key);
    return 1;
}

/* ==================================================================== */
/* the settings variables (as map_settings.c had them)                   */
/* ==================================================================== */

#define MS_RENDER 0
#define MS_BOUNDS 1
#define MS_PRODUCT (-1)
#define MS_AUTHOR (-2)
#define MS_RENDER_VARIABLE "smp.render.v1"
#define MS_BOUNDS_VARIABLE "smp.bounds.v1"

static const char *const ms_names[2] = {MS_RENDER_VARIABLE, MS_BOUNDS_VARIABLE};
/* Rows Snapmap+ wrote and removed itself; no logic node binds to them. */
static const char *const ms_product_prefixes[] = {"smp.", "smnav1.", "smpmod1.", "smpprod1.", "smpkg."};

typedef struct ms_elem {
    size_t from, to;          /* the element, '{' to '}' inclusive-exclusive */
    size_t vfrom, vto;        /* initialValue body (no quotes); vto == 0 when absent */
    int which;                /* MS_RENDER, MS_BOUNDS, MS_PRODUCT or MS_AUTHOR */
} ms_elem;

typedef struct ms_list {
    sh_shard_doc doc;
    int bucket;
    ms_elem *e;
    size_t n;
} ms_list;

static void ms_list_free(ms_list *l)
{
    sh_shard_doc_free(&l->doc);
    if (l->e) HeapFree(GetProcessHeap(), 0, l->e);
    memset(l, 0, sizeof *l);
}

/* The body span of the string value of member `key` directly inside `parent`. */
static int ms_member_string(const char *json, size_t len, const sh_shard_doc *doc, int parent,
                            const char *key, size_t *from, size_t *to)
{
    size_t klen = strlen(key), at, stop, v, q;
    if (parent < 0 || (size_t)parent >= doc->count) return 0;
    at = doc->c[parent].open;
    stop = doc->c[parent].close;
    while (at < stop) {
        const char *k = sh_shard_find(json + at, stop - at, key, klen);
        size_t koff;
        if (!k) return 0;
        koff = (size_t)(k - json);
        at = koff + 1;
        if (koff == 0 || json[koff - 1] != '"' || koff + klen >= len || json[koff + klen] != '"') continue;
        if (sh_shard_doc_innermost(doc, koff) != parent) continue;
        v = koff + klen + 1;
        while (v < stop && sh_shard_is_ws(json[v])) v++;
        if (v >= stop || json[v] != ':') continue;
        v++;
        while (v < stop && sh_shard_is_ws(json[v])) v++;
        if (v >= stop || json[v] != '"') return 0;
        for (q = v + 1; q < stop; q++) {
            if (json[q] == '\\') { q++; continue; }
            if (json[q] == '"') break;
        }
        if (q >= stop) return 0;
        *from = v + 1;
        *to = q;
        return 1;
    }
    return 0;
}

static int ms_classify(const char *body, size_t n)
{
    size_t i;
    for (i = 0; i < 2; i++)
        if (n == strlen(ms_names[i]) && !memcmp(body, ms_names[i], n)) return (int)i;
    for (i = 0; i < sizeof ms_product_prefixes / sizeof ms_product_prefixes[0]; i++) {
        size_t m = strlen(ms_product_prefixes[i]);
        if (n >= m && !memcmp(body, ms_product_prefixes[i], m)) return MS_PRODUCT;
    }
    return MS_AUTHOR;
}

/* Every element of variables.string[], classified by its name. 0 when the
 * text does not read as a map with that list. */
static int ms_list_read(const char *json, size_t len, ms_list *l)
{
    int vars;
    size_t i, cap = 0;
    unsigned expected;
    memset(l, 0, sizeof *l);
    l->bucket = -1;
    if (!sh_shard_doc_build(json, len, &l->doc)) return 0;
    vars = sh_shard_doc_member(json, len, &l->doc, 0, "variables");
    if (vars < 0 || l->doc.c[vars].kind != '{') goto fail;
    l->bucket = sh_shard_doc_member(json, len, &l->doc, vars, "string");
    if (l->bucket < 0 || l->doc.c[l->bucket].kind != '[') goto fail;
    expected = sh_shard_doc_array_count(json, &l->doc, l->bucket);
    for (i = (size_t)l->bucket + 1; i < l->doc.count && l->doc.c[i].open < l->doc.c[l->bucket].close; i++) {
        ms_elem *e;
        int info;
        size_t nf, nt;
        if (l->doc.c[i].parent != l->bucket) continue;
        if (l->doc.c[i].kind != '{') goto fail;
        if (l->n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            ms_elem *grown = l->e ? (ms_elem *)HeapReAlloc(GetProcessHeap(), 0, l->e, ncap * sizeof *grown)
                                  : (ms_elem *)HeapAlloc(GetProcessHeap(), 0, ncap * sizeof *grown);
            if (!grown) goto fail;
            l->e = grown;
            cap = ncap;
        }
        e = &l->e[l->n++];
        memset(e, 0, sizeof *e);
        e->from = l->doc.c[i].open;
        e->to = l->doc.c[i].close + 1;
        e->which = MS_AUTHOR;
        info = sh_shard_doc_member(json, len, &l->doc, (int)i, "info");
        if (info >= 0 && ms_member_string(json, len, &l->doc, info, "name", &nf, &nt))
            e->which = ms_classify(json + nf, nt - nf);
        if (!ms_member_string(json, len, &l->doc, (int)i, "initialValue", &e->vfrom, &e->vto))
            e->vfrom = e->vto = 0;
    }
    /* Anything in the list that is not an object is not a map we understand. */
    if (l->n != expected) goto fail;
    return 1;
fail:
    ms_list_free(l);
    return 0;
}

/* Only Snapmap+ rows follow element k. */
static int ms_removable(const ms_list *l, size_t k)
{
    size_t j;
    for (j = k + 1; j < l->n; j++) if (l->e[j].which == MS_AUTHOR) return 0;
    return 1;
}

/* Rebuild the text with element k dropped when drop[k], and element k's value
 * body replaced by value[k] when value[k] is not NULL. allocCount[4] follows
 * the new count when anything was dropped. */
static char *ms_rebuild(const char *json, size_t len, const ms_list *l, const unsigned char *drop,
                        const char *const *value, size_t *out_len)
{
    size_t need = len + 1, w = 0, k, open = l->doc.c[l->bucket].open, close = l->doc.c[l->bucket].close;
    int first = 1, dropped = 0;
    char *out;
    for (k = 0; k < l->n; k++) if (value[k]) need += strlen(value[k]);
    out = (char *)HeapAlloc(GetProcessHeap(), 0, need + 16);
    if (!out) return NULL;
#define MS_EMIT(p, n) do { memcpy(out + w, (p), (n)); w += (n); } while (0)
    MS_EMIT(json, open + 1);
    for (k = 0; k < l->n; k++) {
        const ms_elem *e = &l->e[k];
        if (drop[k]) { dropped = 1; continue; }
        /* The leading space when it is now the first, else the separator
         * that preceded it originally (every separator is a comma). */
        if (first) MS_EMIT(json + open + 1, l->e[0].from - (open + 1));
        else MS_EMIT(json + l->e[k - 1].to, e->from - l->e[k - 1].to);
        first = 0;
        if (value[k] && e->vto) {
            MS_EMIT(json + e->from, e->vfrom - e->from);
            MS_EMIT(value[k], strlen(value[k]));
            MS_EMIT(json + e->vto, e->to - e->vto);
        } else MS_EMIT(json + e->from, e->to - e->from);
    }
    if (l->n) MS_EMIT(json + l->e[l->n - 1].to, close - l->e[l->n - 1].to);
    else MS_EMIT(json + open + 1, close - (open + 1));
    MS_EMIT(json + close, len - close);
#undef MS_EMIT
    out[w] = 0;
    if (dropped) {
        sh_shard_doc d;
        int vars, bucket, alloc;
        size_t from, to;
        char number[16];
        int digits;
        if (!sh_shard_doc_build(out, w, &d)) { HeapFree(GetProcessHeap(), 0, out); return NULL; }
        vars = sh_shard_doc_member(out, w, &d, 0, "variables");
        bucket = vars >= 0 ? sh_shard_doc_member(out, w, &d, vars, "string") : -1;
        alloc = vars >= 0 ? sh_shard_doc_member(out, w, &d, vars, "allocCount") : -1;
        if (bucket >= 0 && alloc >= 0 && sh_shard_doc_flat_element(out, &d, alloc, 4, &from, &to)) {
            digits = _snprintf_s(number, sizeof number, _TRUNCATE, "%u", sh_shard_doc_array_count(out, &d, bucket));
            if (digits < 0 || w - (to - from) + (size_t)digits > need + 15) {
                sh_shard_doc_free(&d); HeapFree(GetProcessHeap(), 0, out); return NULL;
            }
            memmove(out + from + digits, out + to, w - to + 1);
            memcpy(out + from, number, (size_t)digits);
            w = w - (to - from) + (size_t)digits;
        }
        sh_shard_doc_free(&d);
    }
    if (out_len) *out_len = w;
    return out;
}

static int ms_mentions(const char *json, size_t len)
{
    return sh_shard_find(json, len, MS_RENDER_VARIABLE, strlen(MS_RENDER_VARIABLE)) ||
           sh_shard_find(json, len, MS_BOUNDS_VARIABLE, strlen(MS_BOUNDS_VARIABLE));
}

/* ==================================================================== */
/* the smnav1 header grammar (as navmesh.c had it)                       */
/* ==================================================================== */

typedef struct nav_hdr {
    char     module[SH_SMNAV_MODULE_CAP];
    char     cls[SH_SMNAV_CLASS_CAP];
    char     digest[SH_SMNAV_DIGEST_CHARS + 1];
    char     geom[SH_SMNAV_GEOM_CHARS + 1];
    unsigned idx, total;
} nav_hdr;

static int nav_is_class_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

static int nav_is_module_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '/';
}

/* The module field is `category/module`, and may carry further '/' segments.
 * It must not be able to construct a path: no '.', no backslash, no empty
 * segment, no leading or trailing '/', and at least one '/' -- a bare module
 * name cannot reconstruct the resource names. */
static int nav_copy_module(const char *p, size_t n, char *out, size_t cap)
{
    size_t i;
    int slashes = 0;
    if (n == 0 || n >= cap) return 0;
    if (p[0] == '/' || p[n - 1] == '/') return 0;
    for (i = 0; i < n; i++) {
        if (!nav_is_module_char(p[i])) return 0;
        if (p[i] == '/') {
            if (i && p[i - 1] == '/') return 0;
            slashes++;
        }
    }
    if (slashes == 0) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int nav_copy_class(const char *p, size_t n, char *out, size_t cap)
{
    size_t i;
    if (n == 0 || n >= cap) return 0;
    for (i = 0; i < n; i++) if (!nav_is_class_char(p[i])) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int nav_copy_hex(const char *p, size_t n, char *out, size_t want)
{
    size_t i;
    if (n != want) return 0;
    for (i = 0; i < n; i++) if (!sh_shard_is_hex(p[i])) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int nav_parse_uint(const char *p, size_t n, unsigned *out)
{
    unsigned long v = 0;
    size_t i;
    if (n == 0 || n > 7) return 0;
    for (i = 0; i < n; i++) {
        if (!sh_shard_is_digit(p[i])) return 0;
        v = v * 10 + (unsigned long)(p[i] - '0');
    }
    *out = (unsigned)v;
    return 1;
}

/* Parse one header text (magic included, quotes excluded).
 *
 * FIVE trailing dot-separated fields are taken FROM THE RIGHT, and everything
 * between the magic and them is the module field -- which is exactly why a
 * module name containing '/' is safe here and one containing '.' is not
 * representable. Returns 1 only when every field is well formed. */
static int nav_parse_header(const char *p, size_t plen, nav_hdr *h)
{
    const char *f[5];
    size_t fl[5];
    size_t end = plen;
    int i;

    if (plen <= MO_NAV_MAGIC_LEN) return 0;
    if (memcmp(p, MO_NAV_MAGIC, MO_NAV_MAGIC_LEN) != 0) return 0;

    for (i = 4; i >= 0; i--) {
        size_t d = end;
        while (d > 0 && p[d - 1] != '.') d--;
        if (d == 0) return 0;            /* ran out of separators */
        f[i] = p + d;
        fl[i] = end - d;
        end = d - 1;                     /* step over the '.' itself */
    }
    if (end <= MO_NAV_MAGIC_LEN) return 0;

    if (!nav_copy_module(p + MO_NAV_MAGIC_LEN, end - MO_NAV_MAGIC_LEN,
                         h->module, sizeof h->module)) return 0;
    if (!nav_copy_class(f[0], fl[0], h->cls, sizeof h->cls)) return 0;
    if (!nav_parse_uint(f[1], fl[1], &h->idx)) return 0;
    if (!nav_parse_uint(f[2], fl[2], &h->total)) return 0;
    if (h->total == 0 || h->total > SH_SMNAV_MAX_SHARDS) return 0;
    if (!nav_copy_hex(f[3], fl[3], h->digest, SH_SMNAV_DIGEST_CHARS)) return 0;
    if (!nav_copy_hex(f[4], fl[4], h->geom, SH_SMNAV_GEOM_CHARS)) return 0;
    return 1;
}

/* ==================================================================== */
/* old rows                                                              */
/* ==================================================================== */

#define MO_ROWS_MAGIC     "smpmod1."
#define MO_ROWS_MAGIC_LEN 8
#define MO_PRODUCT_MAGIC     "smpprod1."
#define MO_PRODUCT_MAGIC_LEN 9
#define MO_VALUE_WINDOW   4096
#define MO_MAX_ROWS       SH_MODMANIFEST_MAX_ROWS

static void mo_row_free(sh_map_old_row *row)
{
    size_t i;
    if (!row) return;
    for (i = 0; i < row->name_count; i++) free(row->names[i]);
    free(row->names);
    row->names = NULL;
    row->name_count = 0;
}

static int mo_row_add_name(sh_map_old_row *row, const char *name)
{
    size_t i, n;
    char **grown, *copy;
    if (!name || !*name) return 0;
    for (i = 0; i < row->name_count; i++)
        if (!_stricmp(row->names[i], name)) return 1;
    n = strlen(name);
    copy = (char *)malloc(n + 1);
    if (!copy) return 0;
    memcpy(copy, name, n + 1);
    grown = (char **)realloc(row->names, (row->name_count + 1) * sizeof *grown);
    if (!grown) { free(copy); return 0; }
    row->names = grown;
    row->names[row->name_count++] = copy;
    return 1;
}

/* An old key: lowercase [a-z0-9][a-z0-9._-]*, no consecutive dots, no
 * trailing dot, at most 64 characters (the grammar earlier builds wrote). */
static int mo_valid_key(const char *id)
{
    size_t i, n;
    if (!id) return 0;
    n = strlen(id);
    if (n == 0 || n > 64) return 0;
    if (!((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9'))) return 0;
    for (i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
        if (c == '.' && (i + 1 == n || id[i + 1] == '.')) return 0;
    }
    return 1;
}

/* One old row object {"id","version","bundle","size","attached"?,"choices"?,"names"}. */
static int mo_parse_row(const char *json, size_t length, sh_map_old_row *out)
{
    sh_json_object object = {0};
    const char *names;
    int ok = 0;
    memset(out, 0, sizeof *out);
    if (!json || !length || !sh_native_json_parse_object(json, length, 4, &object)) return 0;
    if (!mo_member_text(&object, "id", out->key, sizeof out->key, 1) || !mo_valid_key(out->key)) goto done;
    if (!mo_member_text(&object, "version", out->version, sizeof out->version, 1)) goto done;
    if (!mo_member_text(&object, "bundle", out->bundle, sizeof out->bundle, 1) ||
        !sh_modmanifest_valid_hash(out->bundle, strlen(out->bundle))) goto done;
    if (!mo_member_uint(&object, "size", &out->size)) goto done;
    {
        const char *attached = sh_json_object_get(&object, "attached");
        out->attached = attached && !strcmp(attached, "true");
    }
    {
        const char *choices = sh_json_object_get(&object, "choices");
        sh_json_object co = {0};
        size_t c;
        if (choices && *choices == '{' && sh_native_json_parse_object(choices, strlen(choices), 2, &co)) {
            for (c = 0; c < co.count && c < SH_MODMANIFEST_CHOICES_MAX; c++) {
                char *key = mo_decode(co.members[c].value_json, strlen(co.members[c].value_json));
                char slot[SH_MODMANIFEST_SLOT_MAX];
                if (key && co.members[c].key_length < sizeof slot) {
                    memcpy(slot, co.members[c].key, co.members[c].key_length);
                    slot[co.members[c].key_length] = 0;
                    if (mo_slot_ok(slot) && mo_valid_key(key))
                        (void)mo_choice_add(out->choices, sizeof out->choices, slot, key);
                }
                free(key);
            }
            sh_json_object_free(&co);
        }
    }
    names = sh_json_object_get(&object, "names");
    if (names) {
        const char *cursor = names, *end = names + strlen(names), *elem;
        size_t elem_len;
        int r;
        while ((r = mo_array_next(&cursor, end, &elem, &elem_len)) == 1) {
            char *name;
            if (*elem != '"' || out->name_count >= SH_MODMANIFEST_MAX_NAMES) continue;
            name = mo_decode(elem, elem_len);
            if (name && strlen(name) <= SH_MODMANIFEST_NAME_MAX && !mo_row_add_name(out, name)) r = -1;
            free(name);
            if (r < 0) break;
        }
        if (r < 0) goto done;
    }
    ok = 1;
done:
    sh_json_object_free(&object);
    if (!ok) mo_row_free(out);
    return ok;
}

/* Add a parsed row, merging one of the same bundle hash. */
static int mo_add_row(sh_map_old *o, sh_map_old_row *row)
{
    size_t i;
    sh_map_old_row *grown;
    for (i = 0; i < o->row_count; i++) {
        if (strcmp(o->rows[i].bundle, row->bundle)) continue;
        if (row->attached) o->rows[i].attached = 1;
        for (size_t k = 0; k < row->name_count; k++) (void)mo_row_add_name(&o->rows[i], row->names[k]);
        mo_row_free(row);
        return 1;
    }
    if (o->row_count >= MO_MAX_ROWS) { mo_row_free(row); return 0; }
    grown = (sh_map_old_row *)realloc(o->rows, (o->row_count + 1) * sizeof *grown);
    if (!grown) { mo_row_free(row); return 0; }
    o->rows = grown;
    o->rows[o->row_count++] = *row;
    memset(row, 0, sizeof *row);
    return 1;
}

static int mo_digits(const char **p, const char *end, unsigned *value)
{
    unsigned long v = 0;
    int digits = 0;
    while (*p < end && sh_shard_is_digit(**p)) {
        if (++digits > 7) return 0;
        v = v * 10 + (unsigned long)(**p - '0');
        (*p)++;
    }
    if (!digits) return 0;
    *value = (unsigned)v;
    return 1;
}

/* "smpmod1.<index>.<count>", 1-7 digits each, count at least 1, index below count. */
static int mo_rows_header(const char *header, size_t length)
{
    const char *p, *end;
    unsigned i = 0, n = 0;
    if (!header || length <= MO_ROWS_MAGIC_LEN || memcmp(header, MO_ROWS_MAGIC, MO_ROWS_MAGIC_LEN)) return 0;
    p = header + MO_ROWS_MAGIC_LEN;
    end = header + length;
    if (!mo_digits(&p, end, &i) || p >= end || *p != '.') return 0;
    p++;
    if (!mo_digits(&p, end, &n) || p != end) return 0;
    return n != 0 && i < n;
}

static int mo_rows_filter(const char *hdr, size_t hdr_len, void *ctx)
{
    (void)ctx;
    return mo_rows_header(hdr, hdr_len);
}

static int mo_product_filter(const char *hdr, size_t hdr_len, void *ctx)
{
    (void)ctx;
    return hdr_len >= MO_PRODUCT_MAGIC_LEN && !memcmp(hdr, MO_PRODUCT_MAGIC, MO_PRODUCT_MAGIC_LEN);
}

/* The JSON string literal that is a row variable's initialValue, quotes
 * included (a row's value is an escaped JSON object, not base64). */
static int mo_find_value(const char *after_header, const char *end, const char **lit, size_t *lit_len)
{
    size_t window = (size_t)(end - after_header);
    const char *key, *v, *close;
    if (window > MO_VALUE_WINDOW) window = MO_VALUE_WINDOW;
    key = sh_shard_find(after_header, window, "\"initialValue\"", 14);
    if (!key) return 0;
    v = mo_ws(key + 14, end);
    if (v >= end || *v != ':') return 0;
    v = mo_ws(v + 1, end);
    if (v >= end || *v != '"') return 0;
    close = mo_skip_string(v, (size_t)(end - v) > SH_SHARD_MAX_CHUNK + 2 ? v + SH_SHARD_MAX_CHUNK + 2 : end);
    if (!close) return 0;
    *lit = v;
    *lit_len = (size_t)(close - v);
    return 1;
}

/* The smpmod1 rows. Returns how many there are (read or not). */
static unsigned mo_read_row_variables(const char *json, size_t length, sh_map_old *o, int collect)
{
    size_t pos = 0;
    const char *hdr, *chunk;
    size_t hdr_len, chunk_len;
    unsigned seen = 0;
    while (sh_shard_next(json, length, MO_ROWS_MAGIC, MO_ROWS_MAGIC_LEN, &pos, &hdr, &hdr_len, &chunk, &chunk_len)) {
        const char *lit;
        size_t lit_len;
        char *value;
        sh_map_old_row row;
        if (!mo_rows_header(hdr, hdr_len)) continue;
        seen++;
        if (!collect) continue;
        if (!mo_find_value(hdr + hdr_len + 1, json + length, &lit, &lit_len) || !(value = mo_decode(lit, lit_len))) {
            o->rows_rejected++;
            continue;
        }
        if (!mo_parse_row(value, strlen(value), &row)) o->rows_rejected++;
        else if (!mo_add_row(o, &row)) o->rows_rejected++;
        free(value);
    }
    return seen;
}

/* The member's "mods" rows that name an "id": read into o. Returns how many
 * old-shape rows there are. */
static unsigned mo_read_member_rows(const char *json, size_t from, size_t to, sh_map_old *o)
{
    const char *cursor = json + from, *end = json + to, *elem;
    size_t elem_len;
    unsigned seen = 0;
    while (sh_carrier_array_next(&cursor, end, &elem, &elem_len) == 1) {
        sh_map_old_row row;
        if (*elem != '{') { o->rows_rejected++; continue; }
        if (sh_carrier_object_member(elem, elem + elem_len, "id", NULL, NULL) != 1 &&
            sh_carrier_object_member(elem, elem + elem_len, "bundle", NULL, NULL) != 1)
            continue;   /* a row in this shape: the writer keeps nothing of it */
        seen++;
        if (!mo_parse_row(elem, elem_len, &row)) o->rows_rejected++;
        else if (!mo_add_row(o, &row)) o->rows_rejected++;
    }
    return seen;
}

/* 1 when the text holds a variable whose header starts with `magic` and
 * `grammar` (when given) accepts. */
static int mo_has_family(const char *json, size_t len, const char *magic, int (*grammar)(const char *, size_t))
{
    size_t pos = 0;
    const char *hdr, *chunk;
    size_t hdr_len, chunk_len;
    while (sh_shard_next(json, len, magic, strlen(magic), &pos, &hdr, &hdr_len, &chunk, &chunk_len))
        if (!grammar || grammar(hdr, hdr_len)) return 1;
    return 0;
}

/* ==================================================================== */
/* old settings rows                                                     */
/* ==================================================================== */

/* A settings value as earlier builds wrote it: digits and separators. The
 * game decodes and validates it when it reads the member (map_settings.c). */
static int mo_setting_text(const char *body, size_t n, char *out, size_t cap)
{
    size_t i;
    if (n == 0 || n > SH_MAP_OLD_VALUE_MAX || n >= cap) return 0;
    for (i = 0; i < n; i++) {
        char c = body[i];
        if (!((c >= '0' && c <= '9') || c == ';' || c == '.' || c == ',' || c == '-')) return 0;
    }
    memcpy(out, body, n);
    out[n] = 0;
    return 1;
}

/* The settings: the member's part when it has one, else the variables (a
 * duplicated or unreadable row leaves that setting off). Sets the family bit
 * when the variables exist. 0 when the text does not read. */
static int mo_read_settings(const char *json, size_t len, sh_map_old *o)
{
    size_t from = 0, to = 0, k;
    int member = sh_carrier_part(json, len, "settings", &from, &to) == 1;
    if (member) {
        static const char *const keys[2] = {"render", "bounds"};
        int i;
        for (i = 0; i < 2; i++) {
            const char *body = NULL;
            size_t n = 0;
            char *dst = i ? o->bounds : o->render;
            int got = sh_carrier_object_string(json + from, json + to, keys[i], &body, &n);
            if (got == 1 && !mo_setting_text(body, n, dst, SH_MAP_OLD_VALUE_MAX + 1)) o->settings_refused++;
            else if (got < 0) o->settings_refused++;
        }
    }
    if (ms_mentions(json, len)) {
        ms_list l;
        unsigned count[2] = {0, 0}, good[2] = {0, 0};
        char text[2][SH_MAP_OLD_VALUE_MAX + 1] = {"", ""};
        if (!ms_list_read(json, len, &l)) return 0;
        for (k = 0; k < l.n; k++) {
            int which = l.e[k].which;
            if (which != MS_RENDER && which != MS_BOUNDS) continue;
            count[which]++;
            if (l.e[k].vto && !memchr(json + l.e[k].vfrom, '\\', l.e[k].vto - l.e[k].vfrom) &&
                mo_setting_text(json + l.e[k].vfrom, l.e[k].vto - l.e[k].vfrom, text[which], sizeof text[which]))
                good[which]++;
        }
        ms_list_free(&l);
        if (count[0] || count[1]) o->families |= SH_MAP_OLD_F_SETTINGS;
        if (!member) {
            int i;
            for (i = 0; i < 2; i++) {
                char *dst = i ? o->bounds : o->render;
                if (count[i] == 1 && good[i]) strcpy_s(dst, SH_MAP_OLD_VALUE_MAX + 1, text[i]);
                else if (count[i]) o->settings_refused += count[i];
            }
        }
    }
    return 1;
}

/* The text without its settings rows: a row only Snapmap+ rows follow is
 * removed, one an author's variable follows is kept and rewritten as off.
 * Process heap, or NULL when nothing changed (*failed when it did not read). */
static char *mo_strip_settings(const char *json, size_t len, size_t *out_len, int *failed)
{
    static const char *const off_text[2] = {"4;8192", "1;0"};
    ms_list l;
    unsigned char *drop;
    const char **rewrite;
    size_t k;
    int changed = 0;
    char *out = NULL;
    *failed = 0;
    if (!ms_mentions(json, len)) return NULL;
    if (!ms_list_read(json, len, &l)) { *failed = 1; return NULL; }
    drop = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l.n ? l.n : 1);
    rewrite = (const char **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (l.n ? l.n : 1) * sizeof *rewrite);
    if (!drop || !rewrite) { *failed = 1; goto done; }
    for (k = 0; k < l.n; k++) {
        int which = l.e[k].which;
        size_t n;
        if (which != MS_RENDER && which != MS_BOUNDS) continue;
        if (ms_removable(&l, k)) { drop[k] = 1; changed = 1; continue; }
        n = l.e[k].vto - l.e[k].vfrom;
        if (!l.e[k].vto || (n == strlen(off_text[which]) && !memcmp(json + l.e[k].vfrom, off_text[which], n))) continue;
        rewrite[k] = off_text[which];
        changed = 1;
    }
    if (changed) {
        out = ms_rebuild(json, len, &l, drop, rewrite, out_len);
        if (!out) *failed = 1;
    }
done:
    if (drop) HeapFree(GetProcessHeap(), 0, drop);
    if (rewrite) HeapFree(GetProcessHeap(), 0, (void *)rewrite);
    ms_list_free(&l);
    return out;
}

/* ==================================================================== */
/* old navigation variables                                              */
/* ==================================================================== */

typedef struct mo_nav_set {
    nav_hdr h;
    sh_shard_chunk *chunks;      /* [total], pointers into the map text */
    unsigned present;
    int consistent;
} mo_nav_set;

static int mo_nav_filter(const char *hdr, size_t hdr_len, void *ctx)
{
    nav_hdr parsed;
    (void)ctx;
    return nav_parse_header(hdr, hdr_len, &parsed);
}

/* The smnav1 sets, reassembled and their digests checked, as the member's
 * "nav" array (map_carrier.h) of the sets that survive. Returns 1 with
 * o->nav set (NULL when no set survived), 0 out of memory. */
static int mo_read_nav_variables(const char *json, size_t len, sh_map_old *o)
{
    mo_nav_set *sets = NULL;
    size_t nsets = 0, i, pos = 0, need = 2, w = 0;
    const char *hdr, *chunk;
    size_t hdr_len, chunk_len;
    nav_hdr h;
    int ok = 1;
    while (sh_shard_next(json, len, MO_NAV_MAGIC, MO_NAV_MAGIC_LEN, &pos, &hdr, &hdr_len, &chunk, &chunk_len)) {
        mo_nav_set *s = NULL;
        if (!nav_parse_header(hdr, hdr_len, &h)) continue;
        o->families |= SH_MAP_OLD_F_NAV;
        for (i = 0; i < nsets && !s; i++)
            if (!strcmp(sets[i].h.module, h.module) && !strcmp(sets[i].h.cls, h.cls)) s = &sets[i];
        if (!s) {
            mo_nav_set *grown;
            if (nsets >= SH_SMNAV_MAX_SETS) { o->nav_refused++; continue; }
            grown = (mo_nav_set *)realloc(sets, (nsets + 1) * sizeof *grown);
            if (!grown) { ok = 0; goto done; }
            sets = grown;
            s = &sets[nsets++];
            memset(s, 0, sizeof *s);
            s->h = h;
            s->consistent = 1;
            s->chunks = (sh_shard_chunk *)calloc(h.total, sizeof *s->chunks);
            if (!s->chunks) { ok = 0; goto done; }
        }
        if (s->h.total != h.total || strcmp(s->h.digest, h.digest) || strcmp(s->h.geom, h.geom) ||
            h.idx >= s->h.total || !chunk || s->chunks[h.idx].filled) {
            s->consistent = 0;
            continue;
        }
        s->chunks[h.idx].p = chunk;
        s->chunks[h.idx].n = chunk_len;
        s->chunks[h.idx].filled = 1;
        s->present++;
    }
    for (i = 0; i < nsets; i++) {
        mo_nav_set *s = &sets[i];
        if (s->consistent && s->present == s->h.total) {
            size_t plen = 0;
            int reason = 0;
            char got[SH_SHARD_DIGEST_CHARS + 1];
            unsigned char *payload = sh_shard_b64_decode(s->chunks, s->h.total, SH_SMNAV_MAX_PAYLOAD, &plen, &reason);
            if (payload) {
                sh_shard_digest16(payload, plen, got);
                HeapFree(GetProcessHeap(), 0, payload);
                if (strcmp(got, s->h.digest)) s->consistent = 0;
            } else s->consistent = 0;
        } else s->consistent = 0;
        if (!s->consistent) { o->nav_refused++; continue; }
        need += 96 + strlen(s->h.module) + strlen(s->h.cls);
        for (unsigned c = 0; c < s->h.total; c++) need += s->chunks[c].n + 3;
    }
    for (i = 0; i < nsets; i++) if (sets[i].consistent) break;
    if (i == nsets) goto done;
    o->nav = (char *)malloc(need + 1);
    if (!o->nav) { ok = 0; goto done; }
    o->nav[w++] = '[';
    for (i = 0; i < nsets; i++) {
        mo_nav_set *s = &sets[i];
        int n;
        if (!s->consistent) continue;
        n = _snprintf_s(o->nav + w, need + 1 - w, _TRUNCATE,
                        "%s{\"module\":\"%s\",\"class\":\"%s\",\"digest\":\"%s\",\"geom\":\"%s\",\"data\":[",
                        o->nav_sets ? "," : "", s->h.module, s->h.cls, s->h.digest, s->h.geom);
        if (n < 0) { ok = 0; goto done; }
        w += (size_t)n;
        for (unsigned c = 0; c < s->h.total; c++) {
            if (c) o->nav[w++] = ',';
            o->nav[w++] = '"';
            memcpy(o->nav + w, s->chunks[c].p, s->chunks[c].n);
            w += s->chunks[c].n;
            o->nav[w++] = '"';
        }
        o->nav[w++] = ']';
        o->nav[w++] = '}';
        o->nav_sets++;
    }
    o->nav[w++] = ']';
    o->nav[w] = 0;
    o->nav_len = w;
done:
    for (i = 0; i < nsets; i++) free(sets[i].chunks);
    free(sets);
    if (!ok) { free(o->nav); o->nav = NULL; o->nav_len = 0; o->nav_sets = 0; }
    return ok;
}

/* ==================================================================== */
/* the API                                                               */
/* ==================================================================== */

#define MO_ERR(...) do { if (err && err_cap) _snprintf_s(err, err_cap, _TRUNCATE, __VA_ARGS__); } while (0)

static int mo_package_header(const char *h, size_t n) { return sh_map_legacy_payload_header(h, n); }
static int mo_unreleased_header(const char *h, size_t n) { return sh_map_legacy_unreleased_header(h, n); }

void sh_map_old_free(sh_map_old *old)
{
    size_t i;
    if (!old) return;
    for (i = 0; i < old->row_count; i++) mo_row_free(&old->rows[i]);
    free(old->rows);
    free(old->nav);
    memset(old, 0, sizeof *old);
}

int sh_map_old_read(const char *json, size_t len, sh_map_old *out, char *err, size_t err_cap)
{
    sh_carrier_view view;
    size_t from = 0, to = 0;
    int member_rows = 0;
    if (err && err_cap) err[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!json || !len || !sh_carrier_view_read(json, len, &view)) { MO_ERR("the map text is not a JSON object"); return 0; }

    if (sh_carrier_part(json, len, "mods", &from, &to) == 1) {
        member_rows = 1;
        if (mo_read_member_rows(json, from, to, out)) out->families |= SH_MAP_OLD_F_MEMBER_ROWS;
    }
    /* The member's rows win over the variables, as earlier builds read them. */
    if (mo_read_row_variables(json, len, out, !member_rows)) out->families |= SH_MAP_OLD_F_ROWS;
    if (mo_has_family(json, len, MO_PRODUCT_MAGIC, NULL)) out->families |= SH_MAP_OLD_F_PRODUCT;
    if (mo_has_family(json, len, SH_LEGACY_MAGIC, mo_package_header)) out->families |= SH_MAP_OLD_F_PACKAGE;
    if (mo_has_family(json, len, "smpbind.", mo_unreleased_header) ||
        mo_has_family(json, len, "smpref.", mo_unreleased_header))
        out->families |= SH_MAP_OLD_F_UNRELEASED;
    if (!mo_read_settings(json, len, out)) {
        sh_map_old_free(out);
        MO_ERR("the map's variables do not read");
        return 0;
    }
    if (sh_carrier_part(json, len, "nav", &from, &to) == 1) {
        /* The member's navigation wins; the variables are only removed. */
        out->nav = (char *)malloc(to - from + 1);
        if (!out->nav) { sh_map_old_free(out); MO_ERR("out of memory"); return 0; }
        memcpy(out->nav, json + from, to - from);
        out->nav[to - from] = 0;
        out->nav_len = to - from;
        if (mo_has_family(json, len, MO_NAV_MAGIC, NULL)) {
            size_t pos = 0;
            const char *hdr, *chunk;
            size_t hdr_len, chunk_len;
            nav_hdr h;
            while (sh_shard_next(json, len, MO_NAV_MAGIC, MO_NAV_MAGIC_LEN, &pos, &hdr, &hdr_len, &chunk, &chunk_len))
                if (nav_parse_header(hdr, hdr_len, &h)) { out->families |= SH_MAP_OLD_F_NAV; break; }
        }
    } else if (!mo_read_nav_variables(json, len, out)) {
        sh_map_old_free(out);
        MO_ERR("out of memory");
        return 0;
    }
    return 1;
}

/* Replace *cur (process heap, or NULL meaning `json`) with `next` (process heap). */
static void mo_take(char **cur, size_t *cur_len, char *next, size_t next_len)
{
    if (!next) return;
    if (*cur) HeapFree(GetProcessHeap(), 0, *cur);
    *cur = next;
    *cur_len = next_len;
}

static const sh_map_old_salt *mo_salt_of(const sh_map_old_salt *map, size_t n, const char *key)
{
    size_t i;
    for (i = 0; i < n; i++) if (!strcmp(map[i].key, key)) return &map[i];
    return NULL;
}

/* `name` with mods/<key>/ rewritten to mods/<salt>/, malloc'd. */
static char *mo_rekey_name(const char *name, const sh_map_old_salt *m)
{
    char *out = NULL, *copy;
    size_t out_len = 0, n = strlen(name), got = sh_mb_rekey_text(name, n, m->key, m->salt, &out, &out_len);
    if (got == (size_t)-1) return NULL;
    if (got && out) return out;
    copy = (char *)malloc(n + 1);
    if (copy) memcpy(copy, name, n + 1);
    return copy;
}

char *sh_map_old_write(const char *json, size_t len, const sh_map_old *old,
                       const sh_map_old_salt *map, size_t map_count,
                       size_t *out_len, char *err, size_t err_cap)
{
    char *cur = NULL, *next;
    size_t cur_len = len, next_len = 0, i;
    unsigned elements = 0, runs = 0;
    int failed = 0;
    sh_modmanifest_list rows;
    char perr[192] = "";
    memset(&rows, 0, sizeof rows);
    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = 0;
    if (!json || !len || !old) { MO_ERR("no map text"); return NULL; }
    if (old->families & SH_MAP_OLD_F_PACKAGE) {
        MO_ERR("the map carries a package (smpkg. variables), which is converted first");
        return NULL;
    }
    for (i = 0; i < map_count; i++)
        if (!mo_valid_key(map[i].key) || !sh_salt_valid(map[i].salt, strlen(map[i].salt)) ||
            !sh_modmanifest_valid_hash(map[i].build, strlen(map[i].build))) {
            MO_ERR("the salt recorded for '%s' is not a salt and a build", map[i].key);
            return NULL;
        }
    for (i = 0; i < old->row_count; i++)
        if (!mo_salt_of(map, map_count, old->rows[i].key)) {
            MO_ERR("no salt is recorded for '%s'", old->rows[i].key);
            return NULL;
        }

    /* 1. Every earlier variable goes. */
#define MO_STRIP(magic, filter) do { \
        next = sh_shard_strip(cur ? cur : json, cur_len, magic, strlen(magic), filter, NULL, \
                              (size_t)SH_MODMANIFEST_MAX_ROWS * 64u, &next_len, &elements, &runs, &failed); \
        if (failed) { MO_ERR("the map's variables do not read"); goto fail; } \
        mo_take(&cur, &cur_len, next, next_len); \
    } while (0)
    MO_STRIP(MO_ROWS_MAGIC, mo_rows_filter);
    MO_STRIP(MO_PRODUCT_MAGIC, mo_product_filter);
    MO_STRIP(MO_NAV_MAGIC, mo_nav_filter);
#undef MO_STRIP
    {
        unsigned packages = 0, unreleased = 0;
        next = sh_map_legacy_payload_strip_all(cur ? cur : json, cur_len, &next_len, &packages, &unreleased, &failed);
        if (failed) { MO_ERR("the map's variables do not read"); goto fail; }
        mo_take(&cur, &cur_len, next, next_len);
    }
    next = mo_strip_settings(cur ? cur : json, cur_len, &next_len, &failed);
    if (failed) { MO_ERR("the map's settings variables do not read"); goto fail; }
    mo_take(&cur, &cur_len, next, next_len);

    /* 2. mods/<key>/ becomes mods/<salt>/ wherever the text names it. */
    for (i = 0; i < map_count; i++) {
        char *rk = NULL;
        size_t rk_len = 0, got = sh_mb_rekey_text(cur ? cur : json, cur_len, map[i].key, map[i].salt, &rk, &rk_len);
        if (got == (size_t)-1) { MO_ERR("out of memory"); goto fail; }
        if (!got || !rk) continue;
        next = (char *)HeapAlloc(GetProcessHeap(), 0, rk_len + 1);
        if (!next) { free(rk); MO_ERR("out of memory"); goto fail; }
        memcpy(next, rk, rk_len);
        next[rk_len] = 0;
        free(rk);
        mo_take(&cur, &cur_len, next, rk_len);
    }

    /* 3. The rows, by salt. */
    for (i = 0; i < old->row_count; i++) {
        const sh_map_old_row *o = &old->rows[i];
        const sh_map_old_salt *m = mo_salt_of(map, map_count, o->key);
        sh_modmanifest_row row;
        const char *p;
        size_t k;
        memset(&row, 0, sizeof row);
        strcpy_s(row.salt, sizeof row.salt, m->salt);
        strcpy_s(row.build, sizeof row.build, m->build);
        strncpy_s(row.name, sizeof row.name, m->name[0] ? m->name : o->key, _TRUNCATE);
        row.size = m->size ? m->size : o->size;
        row.attached = o->attached;
        for (p = o->choices; *p;) {
            const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
            char slot[SH_MODMANIFEST_SLOT_MAX], key[SH_MAP_OLD_KEY_CAP];
            const sh_map_old_salt *cm;
            if (!tab || !nl || tab > nl || (size_t)(tab - p) >= sizeof slot || (size_t)(nl - tab - 1) >= sizeof key) break;
            memcpy(slot, p, (size_t)(tab - p)); slot[tab - p] = 0;
            memcpy(key, tab + 1, (size_t)(nl - tab - 1)); key[nl - tab - 1] = 0;
            cm = mo_salt_of(map, map_count, key);
            if (cm) (void)mo_choice_add(row.choices, sizeof row.choices, slot, cm->salt);
            p = nl + 1;
        }
        if (!sh_modmanifest_list_add(&rows, &row)) { MO_ERR("out of memory"); goto fail; }
        for (k = 0; k < o->name_count; k++) {
            char *name = mo_rekey_name(o->names[k], m);
            int added = name && sh_modmanifest_row_add_name(&rows.rows[rows.count - 1], name);
            free(name);
            if (!added) { MO_ERR("out of memory"); goto fail; }
        }
    }
    /* A member whose rows are all in this shape keeps them. */
    if (rows.count || (old->families & SH_MAP_OLD_F_MEMBER_ROWS)) {
        if (rows.count) next = sh_modmanifest_write(cur ? cur : json, cur_len, &rows, &next_len, perr, sizeof perr);
        else next = sh_carrier_put(cur ? cur : json, cur_len, "mods", NULL, 0, &next_len, perr, sizeof perr);
        if (!next && perr[0]) { MO_ERR("the rows could not be written: %s", perr); goto fail; }
        mo_take(&cur, &cur_len, next, next_len);
    }

    /* 4. Settings and navigation. */
    {
        char value[2 * SH_MAP_OLD_VALUE_MAX + 48];
        size_t vn = 0;
        if (old->render[0] || old->bounds[0])
            vn = (size_t)_snprintf_s(value, sizeof value, _TRUNCATE, "{%s%s%s%s%s%s%s}",
                                     old->render[0] ? "\"render\":\"" : "", old->render, old->render[0] ? "\"" : "",
                                     old->render[0] && old->bounds[0] ? "," : "",
                                     old->bounds[0] ? "\"bounds\":\"" : "", old->bounds, old->bounds[0] ? "\"" : "");
        perr[0] = 0;
        next = sh_carrier_put(cur ? cur : json, cur_len, "settings", vn ? value : NULL, vn, &next_len, perr, sizeof perr);
        if (!next && perr[0]) { MO_ERR("the settings could not be written: %s", perr); goto fail; }
        mo_take(&cur, &cur_len, next, next_len);
    }
    perr[0] = 0;
    next = sh_carrier_put(cur ? cur : json, cur_len, "nav", old->nav, old->nav ? old->nav_len : 0, &next_len, perr, sizeof perr);
    if (!next && perr[0]) { MO_ERR("the navigation could not be written: %s", perr); goto fail; }
    mo_take(&cur, &cur_len, next, next_len);

    if (!cur) {
        cur = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
        if (!cur) { MO_ERR("out of memory"); goto fail; }
        memcpy(cur, json, len);
        cur[len] = 0;
        cur_len = len;
    }
    sh_modmanifest_list_free(&rows);
    if (out_len) *out_len = cur_len;
    return cur;
fail:
    sh_modmanifest_list_free(&rows);
    if (cur) HeapFree(GetProcessHeap(), 0, cur);
    return NULL;
}
