/* Map manifest rows: read, strip, write, and the save-time closure. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modmanifest.h"
#include "modserve.h"
#include "map_shards.h"
#include "config_json.h"
#include "backend_log.h"
#ifdef SH_HAVE_PRODUCT_VERSION
#include "product_version.gen.h"   /* SH_PRODUCT_VERSION, written by src/backend/build.ps1 */
#endif

/* ==================================================================== */
/* rows                                                                  */
/* ==================================================================== */

#define MM_ERR(...) do { if (err && err_cap) _snprintf_s(err, err_cap, _TRUNCATE, __VA_ARGS__); } while (0)

void sh_modmanifest_row_free(sh_modmanifest_row *row)
{
    size_t i;
    if (!row) return;
    for (i = 0; i < row->name_count; i++) free(row->names[i]);
    free(row->names);
    row->names = NULL;
    row->name_count = 0;
}

void sh_modmanifest_list_free(sh_modmanifest_list *list)
{
    size_t i;
    if (!list) return;
    for (i = 0; i < list->count; i++) sh_modmanifest_row_free(&list->rows[i]);
    free(list->rows);
    memset(list, 0, sizeof *list);
}

int sh_modmanifest_row_add_name(sh_modmanifest_row *row, const char *name)
{
    size_t i, n;
    char **grown, *copy;
    if (!row || !name || !*name) return 0;
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

int sh_modmanifest_list_add(sh_modmanifest_list *list, const sh_modmanifest_row *row)
{
    sh_modmanifest_row *slot;
    size_t i;
    if (!list || !row) return 0;
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2 : 8;
        sh_modmanifest_row *grown = (sh_modmanifest_row *)realloc(list->rows, next * sizeof *grown);
        if (!grown) return 0;
        list->rows = grown;
        list->capacity = next;
    }
    slot = &list->rows[list->count];
    *slot = *row;
    slot->names = NULL;
    slot->name_count = 0;
    for (i = 0; i < row->name_count; i++) {
        if (!sh_modmanifest_row_add_name(slot, row->names[i])) {
            sh_modmanifest_row_free(slot);
            return 0;
        }
    }
    list->count++;
    return 1;
}

static sh_modmanifest_row *mm_list_find(sh_modmanifest_list *list, const char *bundle)
{
    size_t i;
    for (i = 0; i < list->count; i++)
        if (!strcmp(list->rows[i].bundle, bundle)) return &list->rows[i];
    return NULL;
}

/* ==================================================================== */
/* grammar                                                               */
/* ==================================================================== */

int sh_modmanifest_valid_id(const char *id)
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

int sh_modmanifest_valid_hash(const char *text, size_t length)
{
    size_t i;
    if (!text || length != SH_MOD_HASH_CHARS) return 0;
    for (i = 0; i < length; i++) if (!sh_shard_is_hex(text[i])) return 0;
    return 1;
}

static int mm_digits(const char **p, const char *end, unsigned *value)
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

int sh_modmanifest_header(const char *header, size_t length, unsigned *index, unsigned *count)
{
    const char *p, *end;
    unsigned i = 0, n = 0;
    if (!header || length <= SH_MODMANIFEST_MAGIC_LEN ||
        memcmp(header, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN)) return 0;
    p = header + SH_MODMANIFEST_MAGIC_LEN;
    end = header + length;
    if (!mm_digits(&p, end, &i) || p >= end || *p != '.') return 0;
    p++;
    if (!mm_digits(&p, end, &n) || p != end) return 0;
    if (n == 0 || i >= n) return 0;
    if (index) *index = i;
    if (count) *count = n;
    return 1;
}

/* ==================================================================== */
/* small JSON helpers                                                    */
/* ==================================================================== */

static const char *mm_ws(const char *p, const char *end)
{
    while (p < end && sh_shard_is_ws(*p)) p++;
    return p;
}

/* Past one string literal starting at its opening quote; NULL if unterminated. */
static const char *mm_skip_string(const char *p, const char *end)
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
static const char *mm_skip_value(const char *p, const char *end)
{
    int depth = 0;
    p = mm_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') return mm_skip_string(p, end);
    if (*p != '{' && *p != '[') {
        while (p < end && *p != ',' && *p != '}' && *p != ']' && !sh_shard_is_ws(*p)) p++;
        return p;
    }
    for (; p < end; p++) {
        if (*p == '"') {
            p = mm_skip_string(p, end);
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
static int mm_array_next(const char **cursor, const char *end, const char **elem, size_t *elem_len)
{
    const char *p = mm_ws(*cursor, end), *after;
    if (p >= end) return -1;
    if (*p == '[') {
        p = mm_ws(p + 1, end);
        if (p < end && *p == ']') { *cursor = p + 1; return 0; }
    } else if (*p == ',') {
        p = mm_ws(p + 1, end);
    } else if (*p == ']') {
        *cursor = p + 1;
        return 0;
    } else return -1;
    after = mm_skip_value(p, end);
    if (!after) return -1;
    *elem = p;
    *elem_len = (size_t)(after - p);
    *cursor = mm_ws(after, end);
    if (*cursor >= end || (**cursor != ',' && **cursor != ']')) return -1;
    return 1;
}

/* Decode a string literal (quotes included) into a malloc'd NUL-terminated
 * string. Refuses embedded NULs. */
static char *mm_decode(const char *literal, size_t length)
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
static int mm_member_text(const sh_json_object *object, const char *key, char *out, size_t cap,
                          int required)
{
    const char *raw = sh_json_object_get(object, key);
    char *text;
    size_t n;
    if (!raw) { if (out && cap) out[0] = 0; return !required; }
    text = mm_decode(raw, strlen(raw));
    if (!text) return 0;
    n = strlen(text);
    if (n >= cap) { free(text); return 0; }
    memcpy(out, text, n + 1);
    free(text);
    return 1;
}

static int mm_member_uint(const sh_json_object *object, const char *key, unsigned long long *out)
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

/* JSON string body escaping: quotes, backslashes and control bytes. High
 * bytes pass through (native map strings are byte strings). Returns the
 * length written, or the length needed when out is NULL. */
static size_t mm_escape(char *out, const char *src, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i, w = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (out) { out[w] = '\\'; out[w + 1] = (char)c; }
            w += 2;
        } else if (c < 0x20) {
            if (out) {
                out[w] = '\\'; out[w + 1] = 'u'; out[w + 2] = '0'; out[w + 3] = '0';
                out[w + 4] = hex[c >> 4]; out[w + 5] = hex[c & 15];
            }
            w += 6;
        } else {
            if (out) out[w] = (char)c;
            w++;
        }
    }
    return w;
}

typedef struct mm_text { char *p; size_t n, cap; int failed; } mm_text;

static void mm_put(mm_text *t, const char *s, size_t n)
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

static void mm_put_quoted(mm_text *t, const char *s)
{
    size_t n = strlen(s), need = mm_escape(NULL, s, n);
    char stack[256], *buf = need <= sizeof stack ? stack : (char *)malloc(need);
    if (!buf) { t->failed = 1; return; }
    mm_escape(buf, s, n);
    mm_put(t, "\"", 1);
    mm_put(t, buf, need);
    mm_put(t, "\"", 1);
    if (buf != stack) free(buf);
}

/* ==================================================================== */
/* registry choices                                                      */
/* ==================================================================== */

/* A slot as the manifest stores it: printable, no quote, tab or backslash. */
static int mm_slot_ok(const char *slot)
{
    size_t i, n = slot ? strlen(slot) : 0;
    if (!n || n >= SH_MODMANIFEST_SLOT_MAX) return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)slot[i] <= 0x20 || slot[i] == '"' || slot[i] == '\\' || (unsigned char)slot[i] >= 0x7f) return 0;
    return 1;
}

/* The key for `slot` in "slot\tkey\n" lines, copied to key (when not NULL). 1 found. */
static int mm_choice_find(const char *lines, const char *slot, char *key, size_t cap)
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
static int mm_choice_add(char *lines, size_t cap, const char *slot, const char *key)
{
    size_t at = strlen(lines), need = strlen(slot) + strlen(key) + 2;
    if (mm_choice_find(lines, slot, NULL, 0)) return 1;
    if (at + need + 1 > cap) return 0;
    _snprintf_s(lines + at, cap - at, _TRUNCATE, "%s\t%s\n", slot, key);
    return 1;
}

static size_t mm_choice_count(const char *lines)
{
    size_t n = 0;
    for (; lines && *lines; ++lines) n += *lines == '\n';
    return n;
}

/* ==================================================================== */
/* row value                                                             */
/* ==================================================================== */

int sh_modmanifest_parse_value(const char *json, size_t length, sh_modmanifest_row *out,
                               char *err, size_t err_cap)
{
    sh_json_object object = {0};
    const char *names;
    int ok = 0;
    if (err && err_cap) err[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!json || !length || !sh_native_json_parse_object(json, length, 4, &object)) {
        MM_ERR("row value is not a JSON object");
        return 0;
    }
    if (!mm_member_text(&object, "id", out->id, sizeof out->id, 1) ||
        !sh_modmanifest_valid_id(out->id)) { MM_ERR("row has no valid id"); goto done; }
    if (!mm_member_text(&object, "version", out->version, sizeof out->version, 1)) {
        MM_ERR("row '%s' has no valid version", out->id); goto done;
    }
    if (!mm_member_text(&object, "bundle", out->bundle, sizeof out->bundle, 1) ||
        !sh_modmanifest_valid_hash(out->bundle, strlen(out->bundle))) {
        MM_ERR("row '%s' has no valid bundle hash", out->id); goto done;
    }
    if (!mm_member_uint(&object, "size", &out->size)) {
        MM_ERR("row '%s' has no valid size", out->id); goto done;
    }
    {
        const char *attached = sh_json_object_get(&object, "attached");
        out->attached = attached && !strcmp(attached, "true");
    }
    {
        /* Registry choices: entries that do not read are skipped (the row stays). */
        const char *choices = sh_json_object_get(&object, "choices");
        sh_json_object co = {0};
        size_t c;
        if (choices && *choices == '{' && sh_native_json_parse_object(choices, strlen(choices), 2, &co)) {
            for (c = 0; c < co.count && c < SH_MODMANIFEST_CHOICES_MAX; c++) {
                char *key = mm_decode(co.members[c].value_json, strlen(co.members[c].value_json));
                char slot[SH_MODMANIFEST_SLOT_MAX];
                if (key && co.members[c].key_length < sizeof slot) {
                    memcpy(slot, co.members[c].key, co.members[c].key_length);
                    slot[co.members[c].key_length] = 0;
                    if (mm_slot_ok(slot) && sh_modmanifest_valid_id(key))
                        (void)mm_choice_add(out->choices, sizeof out->choices, slot, key);
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
        while ((r = mm_array_next(&cursor, end, &elem, &elem_len)) == 1) {
            char *name;
            if (*elem != '"' || out->name_count >= SH_MODMANIFEST_MAX_NAMES) continue;
            name = mm_decode(elem, elem_len);
            if (name && strlen(name) <= SH_MODMANIFEST_NAME_MAX) sh_modmanifest_row_add_name(out, name);
            free(name);
        }
        if (r < 0) { MM_ERR("row '%s' has a malformed names list", out->id); goto done; }
    }
    ok = 1;
done:
    sh_json_object_free(&object);
    if (!ok) sh_modmanifest_row_free(out);
    return ok;
}

char *sh_modmanifest_format_value(const sh_modmanifest_row *row, size_t *out_length)
{
    size_t keep, i, kc, nc;
    if (out_length) *out_length = 0;
    if (!row) return NULL;
    nc = mm_choice_count(row->choices);
    /* Drop names from the end until the row fits its budget, then choices. */
    for (kc = nc + 1; kc-- > 0;)
    for (keep = row->name_count + 1; keep-- > 0;) {
        mm_text t = {0};
        char number[32];
        mm_put(&t, "{\"id\":", 6);
        mm_put_quoted(&t, row->id);
        mm_put(&t, ",\"version\":", 11);
        mm_put_quoted(&t, row->version);
        mm_put(&t, ",\"bundle\":", 10);
        mm_put_quoted(&t, row->bundle);
        _snprintf_s(number, sizeof number, _TRUNCATE, ",\"size\":%llu", row->size);
        mm_put(&t, number, strlen(number));
        if (row->attached) mm_put(&t, ",\"attached\":true", 16);
        if (kc) {
            const char *p = row->choices;
            size_t c;
            mm_put(&t, ",\"choices\":{", 12);
            for (c = 0; c < kc && p && *p; c++) {
                const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
                char slot[SH_MODMANIFEST_SLOT_MAX], key[SH_MODMANIFEST_ID_CAP];
                if (!tab || !nl || tab > nl || (size_t)(tab - p) >= sizeof slot || (size_t)(nl - tab - 1) >= sizeof key) break;
                memcpy(slot, p, (size_t)(tab - p)); slot[tab - p] = 0;
                memcpy(key, tab + 1, (size_t)(nl - tab - 1)); key[nl - tab - 1] = 0;
                if (c) mm_put(&t, ",", 1);
                mm_put_quoted(&t, slot);
                mm_put(&t, ":", 1);
                mm_put_quoted(&t, key);
                p = nl + 1;
            }
            mm_put(&t, "}", 1);
        }
        mm_put(&t, ",\"names\":[", 10);
        for (i = 0; i < keep; i++) {
            if (i) mm_put(&t, ",", 1);
            mm_put_quoted(&t, row->names[i]);
        }
        mm_put(&t, "]}", 2);
        if (t.failed) { free(t.p); return NULL; }
        if (t.n <= SH_MODMANIFEST_ROW_MAX) {
            if (out_length) *out_length = t.n;
            return t.p;
        }
        free(t.p);
    }
    return NULL;
}

/* ==================================================================== */
/* map text                                                              */
/* ==================================================================== */

#define MM_VALUE_WINDOW 4096

/* The JSON string literal that is the row's initialValue, quotes included.
 * The shard scanner only reads base64 values; a manifest row's value is an
 * escaped JSON object. */
static int mm_find_value(const char *after_header, const char *end, const char **lit, size_t *lit_len)
{
    size_t window = (size_t)(end - after_header);
    const char *key, *v, *close;
    if (window > MM_VALUE_WINDOW) window = MM_VALUE_WINDOW;
    key = sh_shard_find(after_header, window, "\"initialValue\"", 14);
    if (!key) return 0;
    v = mm_ws(key + 14, end);
    if (v >= end || *v != ':') return 0;
    v = mm_ws(v + 1, end);
    if (v >= end || *v != '"') return 0;
    close = mm_skip_string(v, (size_t)(end - v) > SH_SHARD_MAX_CHUNK + 2 ? v + SH_SHARD_MAX_CHUNK + 2 : end);
    if (!close) return 0;
    *lit = v;
    *lit_len = (size_t)(close - v);
    return 1;
}

size_t sh_modmanifest_read(const char *json, size_t length, sh_modmanifest_list *out,
                           unsigned *rejected)
{
    size_t pos = 0, read = 0;
    const char *hdr, *chunk;
    size_t hdr_len, chunk_len;
    unsigned bad = 0;
    if (rejected) *rejected = 0;
    if (!json || !length || !out) return 0;
    if (!sh_shard_find(json, length, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN)) return 0;
    while (out->count < SH_MODMANIFEST_MAX_ROWS &&
           sh_shard_next(json, length, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN, &pos,
                         &hdr, &hdr_len, &chunk, &chunk_len)) {
        const char *lit;
        size_t lit_len;
        char *value;
        sh_modmanifest_row row;
        sh_modmanifest_row *existing;
        if (!sh_modmanifest_header(hdr, hdr_len, NULL, NULL)) continue;
        if (!mm_find_value(hdr + hdr_len + 1, json + length, &lit, &lit_len)) { bad++; continue; }
        value = mm_decode(lit, lit_len);
        if (!value) { bad++; continue; }
        if (!sh_modmanifest_parse_value(value, strlen(value), &row, NULL, 0)) {
            free(value); bad++; continue;
        }
        free(value);
        existing = mm_list_find(out, row.bundle);
        if (existing) {
            size_t i;
            if (row.attached) existing->attached = 1;
            for (i = 0; i < row.name_count; i++) sh_modmanifest_row_add_name(existing, row.names[i]);
            sh_modmanifest_row_free(&row);
        } else {
            if (!sh_modmanifest_list_add(out, &row)) bad++;
            sh_modmanifest_row_free(&row);
        }
        read++;
    }
    if (rejected) *rejected = bad;
    return read;
}

static int mm_strip_filter(const char *hdr, size_t hdr_len, void *ctx)
{
    (void)ctx;
    return sh_modmanifest_header(hdr, hdr_len, NULL, NULL);
}

char *sh_modmanifest_strip(const char *json, size_t length, size_t *out_length,
                           unsigned *removed, int *doc_failed)
{
    unsigned elements = 0, runs = 0;
    int failed = 0;
    size_t written = 0;
    char *out;
    if (out_length) *out_length = 0;
    if (removed) *removed = 0;
    if (doc_failed) *doc_failed = 0;
    if (!json || !length ||
        !sh_shard_find(json, length, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN)) return NULL;
    out = sh_shard_strip(json, length, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN,
                         mm_strip_filter, NULL, SH_MODMANIFEST_MAX_ROWS * 2u,
                         &written, &elements, &runs, &failed);
    if (doc_failed) *doc_failed = failed;
    if (!out) return NULL;
    if (out_length) *out_length = written;
    if (removed) *removed = elements;
    return out;
}

char *sh_modmanifest_write(const char *json, size_t length, const sh_modmanifest_list *rows,
                           size_t *out_length, char *err, size_t err_cap)
{
    char *base = NULL, *out = NULL;
    const char *src;
    size_t src_len = 0, i, count = rows ? rows->count : 0, w = 0;
    int failed = 0;
    sh_shard_out *parts = NULL;
    char *headers = NULL, **values = NULL;
    if (out_length) *out_length = 0;
    if (err && err_cap) err[0] = 0;
    if (!json || !length) { MM_ERR("no map text"); return NULL; }

    base = sh_modmanifest_strip(json, length, &src_len, NULL, &failed);
    if (failed) { MM_ERR("the map's existing manifest rows could not be removed"); return NULL; }
    src = base ? base : json;
    if (!base) src_len = length;
    if (count == 0) {
        if (!base) MM_ERR("nothing to write");
        if (out_length && base) *out_length = src_len;
        return base;
    }
    if (count > SH_MODMANIFEST_MAX_ROWS) { MM_ERR("too many manifest rows"); goto done; }

    parts = (sh_shard_out *)calloc(count, sizeof *parts);
    headers = (char *)calloc(count, 32);
    values = (char **)calloc(count, sizeof *values);
    if (!parts || !headers || !values) { MM_ERR("out of memory building the manifest"); goto done; }
    for (i = 0; i < count; i++) {
        size_t vlen = 0, elen;
        char *value = sh_modmanifest_format_value(&rows->rows[i], &vlen);
        if (!value) { MM_ERR("manifest row for '%s' could not be formatted", rows->rows[i].id); goto done; }
        elen = mm_escape(NULL, value, vlen);
        values[i] = (char *)malloc(elen + 1);
        if (!values[i]) { free(value); MM_ERR("out of memory building the manifest"); goto done; }
        mm_escape(values[i], value, vlen);
        values[i][elen] = 0;
        free(value);
        _snprintf_s(headers + i * 32, 32, _TRUNCATE, "%s%u.%u", SH_MODMANIFEST_MAGIC,
                    (unsigned)i, (unsigned)count);
        parts[i].header = headers + i * 32;
        parts[i].chunk = values[i];
        parts[i].chunk_len = elen;
    }
    out = sh_shard_insert(src, src_len, parts, count, &w, err, err_cap);
    if (out && out_length) *out_length = w;
done:
    if (values) for (i = 0; i < count; i++) free(values[i]);
    free(values);
    free(headers);
    free(parts);
    if (base) HeapFree(GetProcessHeap(), 0, base);
    return out;
}

/* ==================================================================== */
/* placed names                                                          */
/* ==================================================================== */

enum { MM_KEY_OTHER = 0, MM_KEY_INHERIT, MM_KEY_MODULE, MM_KEY_EDIT };

typedef struct mm_frame { char kind; char expect_key; char in_edit; char key; } mm_frame;

typedef struct mm_names { char **v; size_t n, cap; int failed; } mm_names;

#define MM_MAX_PLACED (1u << 20)

static void mm_names_add(mm_names *s, const char *lit, size_t lit_len)
{
    char *name;
    size_t body = lit_len - 2;
    if (s->failed || body == 0 || body > SH_MODMANIFEST_NAME_MAX) return;
    if (s->n >= MM_MAX_PLACED) return;
    if (!memchr(lit + 1, '\\', body)) {
        size_t i;
        for (i = 1; i <= body; i++) if ((unsigned char)lit[i] < 0x20) return;
        name = (char *)malloc(body + 1);
        if (!name) { s->failed = 1; return; }
        memcpy(name, lit + 1, body);
        name[body] = 0;
    } else {
        name = mm_decode(lit, lit_len);
        if (!name) return;
        if (!*name) { free(name); return; }
    }
    if (s->n == s->cap) {
        size_t next = s->cap ? s->cap * 2 : 256;
        char **grown = (char **)realloc(s->v, next * sizeof *grown);
        if (!grown) { free(name); s->failed = 1; return; }
        s->v = grown;
        s->cap = next;
    }
    s->v[s->n++] = name;
}

static int mm_name_cmp(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int c = _stricmp(x, y);
    return c ? c : strcmp(x, y);
}

static int mm_key_class(const char *lit, size_t lit_len)
{
    if (lit_len == 9 && !memcmp(lit, "\"inherit\"", 9)) return MM_KEY_INHERIT;
    if (lit_len == 12 && !memcmp(lit, "\"moduleName\"", 12)) return MM_KEY_MODULE;
    if (lit_len == 6 && !memcmp(lit, "\"edit\"", 6)) return MM_KEY_EDIT;
    return MM_KEY_OTHER;
}

void sh_modmanifest_names_free(char **names, size_t count)
{
    size_t i;
    if (!names) return;
    for (i = 0; i < count; i++) free(names[i]);
    free(names);
}

size_t sh_modmanifest_placed_names(const char *json, size_t length, char ***out_names, int *failed)
{
    mm_frame stack[SH_SHARD_MAX_DEPTH];
    unsigned depth = 0;
    mm_names s = {0};
    const char *p = json, *end = json + length;
    size_t i, w;
    if (out_names) *out_names = NULL;
    if (failed) *failed = 0;
    if (!json || !length || !out_names) { if (failed) *failed = 1; return 0; }

    while (p < end) {
        char c = *p;
        if (sh_shard_is_ws(c) || c == ':') { p++; continue; }
        if (c == '{' || c == '[') {
            int edit = 0;
            if (depth >= SH_SHARD_MAX_DEPTH) goto bad;
            if (depth) {
                mm_frame *top = &stack[depth - 1];
                edit = top->in_edit || (top->kind == '{' && top->key == MM_KEY_EDIT);
            }
            stack[depth].kind = c;
            stack[depth].expect_key = (char)(c == '{');
            stack[depth].in_edit = (char)edit;
            stack[depth].key = MM_KEY_OTHER;
            depth++;
            p++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (!depth || stack[depth - 1].kind != (c == '}' ? '{' : '[')) goto bad;
            depth--;
            p++;
            continue;
        }
        if (c == ',') {
            if (depth && stack[depth - 1].kind == '{') stack[depth - 1].expect_key = 1;
            p++;
            continue;
        }
        if (c == '"') {
            const char *close = mm_skip_string(p, end);
            mm_frame *top = depth ? &stack[depth - 1] : NULL;
            if (!close) goto bad;
            if (top && top->kind == '{' && top->expect_key) {
                top->key = (char)mm_key_class(p, (size_t)(close - p));
                top->expect_key = 0;
            } else if (top) {
                int take = top->in_edit ||
                           (top->kind == '{' && (top->key == MM_KEY_INHERIT || top->key == MM_KEY_MODULE));
                if (take) mm_names_add(&s, p, (size_t)(close - p));
            }
            p = close;
            continue;
        }
        p++;   /* numbers and literals */
    }
    if (depth || s.failed) goto bad;

    if (s.n > 1) {
        qsort(s.v, s.n, sizeof *s.v, mm_name_cmp);
        for (i = 1, w = 1; i < s.n; i++) {
            if (!_stricmp(s.v[i], s.v[w - 1])) { free(s.v[i]); continue; }
            s.v[w++] = s.v[i];
        }
        s.n = w;
    }
    *out_names = s.v;
    return s.n;
bad:
    sh_modmanifest_names_free(s.v, s.n);
    if (failed) *failed = 1;
    return 0;
}

/* ==================================================================== */
/* mod.json                                                              */
/* ==================================================================== */

/* The provides names and sizes of one product-block list into `out`. */
static int mm_block_list(const char *list, sh_modmanifest_row *out)
{
    const char *cursor = list, *end = list + strlen(list), *elem;
    size_t elem_len;
    int r;
    while ((r = mm_array_next(&cursor, end, &elem, &elem_len)) == 1) {
        sh_json_object file = {0}, provides = {0};
        unsigned long long size = 0;
        const char *raw;
        if (!sh_json_parse_object(elem, elem_len, 8, &file)) { r = -1; break; }
        if (mm_member_uint(&file, "size", &size)) {
            if (size > ULLONG_MAX - out->size) size = ULLONG_MAX - out->size;
            out->size += size;
        }
        raw = sh_json_object_get(&file, "provides");
        if (raw && sh_json_parse_object(raw, strlen(raw), 4, &provides)) {
            char name[SH_MODMANIFEST_NAME_MAX + 1];
            if (mm_member_text(&provides, "name", name, sizeof name, 1) && name[0] &&
                !sh_modmanifest_row_add_name(out, name)) r = -1;
        }
        sh_json_object_free(&provides);
        sh_json_object_free(&file);
        if (r < 0) break;
    }
    return r >= 0;
}

int sh_modmanifest_read_mod_json(const char *text, size_t length, sh_modmanifest_row *out,
                                 char *err, size_t err_cap)
{
    sh_json_object object = {0}, block = {0};
    const char *raw, *files, *stock;
    size_t i;
    int ok = 0;
    if (err && err_cap) err[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (text && length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) { text += 3; length -= 3; }
    while (length && sh_shard_is_ws(*text)) { text++; length--; }
    if (!text || !length || !sh_json_parse_object(text, length, 24, &object)) {
        MM_ERR("mod.json is not a JSON object");
        return 0;
    }
    if (!mm_member_text(&object, "id", out->id, sizeof out->id, 1)) { MM_ERR("mod.json has no id"); goto done; }
    for (i = 0; out->id[i]; i++) if (out->id[i] >= 'A' && out->id[i] <= 'Z') out->id[i] = (char)(out->id[i] + 32);
    if (!sh_modmanifest_valid_id(out->id)) { MM_ERR("mod.json has no valid id"); goto done; }
    if (!mm_member_text(&object, "version", out->version, sizeof out->version, 1)) strcpy_s(out->version, sizeof out->version, "0.0.0");
    raw = sh_json_object_get(&object, "snapmap-plus");
    if (!raw || *raw != '{' || !sh_json_parse_object(raw, strlen(raw), 16, &block)) {
        MM_ERR("mod.json has not been built (no snapmap-plus block)"); goto done;
    }
    if (!mm_member_text(&block, "bundle", out->bundle, sizeof out->bundle, 1) ||
        !sh_modmanifest_valid_hash(out->bundle, strlen(out->bundle))) {
        MM_ERR("mod.json has no bundle hash"); goto done;
    }
    files = sh_json_object_get(&block, "files");
    stock = sh_json_object_get(&block, "stock");
    if (!files && !stock) { MM_ERR("mod.json has no files list"); goto done; }
    if ((files && !mm_block_list(files, out)) || (stock && !mm_block_list(stock, out))) {
        MM_ERR("mod.json has a malformed files list"); goto done;
    }
    ok = 1;
done:
    sh_json_object_free(&block);
    sh_json_object_free(&object);
    if (!ok) sh_modmanifest_row_free(out);
    return ok;
}

/* ==================================================================== */
/* hooks                                                                 */
/* ==================================================================== */

static SRWLOCK g_current_lock = SRWLOCK_INIT;
static sh_modmanifest_list g_current;

static void mm_set_current(sh_modmanifest_list *list)
{
    sh_modmanifest_list old;
    AcquireSRWLockExclusive(&g_current_lock);
    old = g_current;
    if (list) { g_current = *list; memset(list, 0, sizeof *list); }
    else memset(&g_current, 0, sizeof g_current);
    ReleaseSRWLockExclusive(&g_current_lock);
    sh_modmanifest_list_free(&old);
}

size_t sh_modmanifest_current(sh_modmanifest_list *out)
{
    size_t i, n;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    AcquireSRWLockShared(&g_current_lock);
    for (i = 0; i < g_current.count; i++)
        if (!sh_modmanifest_list_add(out, &g_current.rows[i])) break;
    ReleaseSRWLockShared(&g_current_lock);
    n = out->count;
    return n;
}

/* The open map's registry choices ("slot\tkey\n" lines), under g_current_lock. */
static char g_choices[SH_MODMANIFEST_CHOICES_CAP];

/* Give the choices to the serving index (its choice input of the listing). */
static int mm_choices_push(int recompute)
{
    char copy[SH_MODMANIFEST_CHOICES_CAP], *p;
    const char *slots[SH_MODMANIFEST_CHOICES_MAX], *keys[SH_MODMANIFEST_CHOICES_MAX];
    int n = 0;
    AcquireSRWLockShared(&g_current_lock);
    memcpy(copy, g_choices, sizeof copy);
    ReleaseSRWLockShared(&g_current_lock);
    for (p = copy; *p && n < SH_MODMANIFEST_CHOICES_MAX;) {
        char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        if (!tab || !nl || tab > nl) break;
        *tab = 0;
        *nl = 0;
        slots[n] = p;
        keys[n++] = tab + 1;
        p = nl + 1;
    }
    return sh_modserve_scope_set_choices(slots, keys, n, recompute);
}

size_t sh_modmanifest_rows_choices(const sh_modmanifest_list *rows, char *out, size_t cap)
{
    size_t i;
    if (!out || !cap) return 0;
    out[0] = 0;
    for (i = 0; rows && i < rows->count; i++) {
        const char *p = rows->rows[i].choices;
        while (*p) {
            const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
            char slot[SH_MODMANIFEST_SLOT_MAX], key[SH_MODMANIFEST_ID_CAP];
            if (!tab || !nl || tab > nl || (size_t)(tab - p) >= sizeof slot || (size_t)(nl - tab - 1) >= sizeof key) break;
            memcpy(slot, p, (size_t)(tab - p)); slot[tab - p] = 0;
            memcpy(key, tab + 1, (size_t)(nl - tab - 1)); key[nl - tab - 1] = 0;
            (void)mm_choice_add(out, cap, slot, key);
            p = nl + 1;
        }
    }
    return mm_choice_count(out);
}

int sh_modmanifest_current_choose(const char *slot, const char *key)
{
    char next[SH_MODMANIFEST_CHOICES_CAP], *p;
    size_t at = 0;
    int ok = 1;
    if (!mm_slot_ok(slot) || (key && key[0] && !sh_modmanifest_valid_id(key))) return -1;
    next[0] = 0;
    AcquireSRWLockExclusive(&g_current_lock);
    /* Every other slot as it was, then this one (the order of choosing). */
    for (p = g_choices; *p;) {
        char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        size_t len;
        if (!tab || !nl || tab > nl) break;
        len = (size_t)(nl - p) + 1;
        if (!((size_t)(tab - p) == strlen(slot) && !strncmp(p, slot, strlen(slot))) && at + len < sizeof next) {
            memcpy(next + at, p, len);
            at += len;
            next[at] = 0;
        }
        p = nl + 1;
    }
    if (key && key[0]) ok = mm_choice_add(next, sizeof next, slot, key);
    if (ok) memcpy(g_choices, next, sizeof g_choices);
    ReleaseSRWLockExclusive(&g_current_lock);
    if (!ok) return -1;
    return mm_choices_push(1);
}

int sh_modmanifest_current_choice(const char *slot, char *key, size_t cap)
{
    int r;
    if (key && cap) key[0] = 0;
    if (!slot) return 0;
    AcquireSRWLockShared(&g_current_lock);
    r = mm_choice_find(g_choices, slot, key, cap);
    ReleaseSRWLockShared(&g_current_lock);
    return r;
}

size_t sh_modmanifest_current_choices(char *out, size_t cap)
{
    size_t n;
    if (!out || !cap) return 0;
    AcquireSRWLockShared(&g_current_lock);
    strncpy_s(out, cap, g_choices, _TRUNCATE);
    n = mm_choice_count(g_choices);
    ReleaseSRWLockShared(&g_current_lock);
    return n;
}

/* The choices of a loaded map, set without a recompute: the listing that
 * follows (the scope hook) computes the registries with them. */
static void mm_choices_load(const sh_modmanifest_list *rows)
{
    char lines[SH_MODMANIFEST_CHOICES_CAP];
    (void)sh_modmanifest_rows_choices(rows, lines, sizeof lines);
    AcquireSRWLockExclusive(&g_current_lock);
    memcpy(g_choices, lines, sizeof g_choices);
    ReleaseSRWLockExclusive(&g_current_lock);
    (void)mm_choices_push(0);
}

/* At a save: each choice on the row of the key it names. */
static void mm_choices_save(sh_modmanifest_list *rows)
{
    char lines[SH_MODMANIFEST_CHOICES_CAP], *p, line[400];
    size_t i;
    AcquireSRWLockShared(&g_current_lock);
    memcpy(lines, g_choices, sizeof lines);
    ReleaseSRWLockShared(&g_current_lock);
    for (i = 0; i < rows->count; i++) rows->rows[i].choices[0] = 0;
    for (p = lines; *p;) {
        char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        int placed = 0;
        if (!tab || !nl || tab > nl) break;
        *tab = 0;
        *nl = 0;
        for (i = 0; i < rows->count && !placed; i++)
            if (!strcmp(rows->rows[i].id, tab + 1))
                placed = mm_choice_add(rows->rows[i].choices, sizeof rows->rows[i].choices, p, tab + 1);
        if (!placed) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "MODS: save manifest: the choice %s -> %s is not saved: the map declares no build of %s", p, tab + 1, tab + 1);
            backend_log(line);
        }
        p = nl + 1;
    }
}

void sh_modmanifest_reset(void)
{
    mm_set_current(NULL);
    AcquireSRWLockExclusive(&g_current_lock);
    g_choices[0] = 0;
    ReleaseSRWLockExclusive(&g_current_lock);
}

static void mm_subst_drop(const char *from);

size_t sh_modmanifest_current_remove(const char *bundle)
{
    size_t i, w, dropped = 0;
    if (!bundle || !bundle[0]) return 0;
    AcquireSRWLockExclusive(&g_current_lock);
    for (i = 0, w = 0; i < g_current.count; i++) {
        if (!strcmp(g_current.rows[i].bundle, bundle)) { sh_modmanifest_row_free(&g_current.rows[i]); dropped++; continue; }
        if (w != i) g_current.rows[w] = g_current.rows[i];
        w++;
    }
    g_current.count = w;
    ReleaseSRWLockExclusive(&g_current_lock);
    if (dropped) mm_subst_drop(bundle);   /* the editor no longer serves it through another build */
    return dropped;
}

int sh_modmanifest_current_attach(const sh_modmanifest_row *row)
{
    sh_modmanifest_row fresh;
    sh_modmanifest_row *have;
    int r;
    if (!row || !sh_modmanifest_valid_id(row->id) || !sh_modmanifest_valid_hash(row->bundle, strlen(row->bundle))) return -1;
    AcquireSRWLockExclusive(&g_current_lock);
    have = mm_list_find(&g_current, row->bundle);
    if (have) {
        r = have->attached ? 0 : 1;
        have->attached = 1;
    } else {
        fresh = *row;
        fresh.names = NULL;
        fresh.name_count = 0;
        fresh.attached = 1;
        r = sh_modmanifest_list_add(&g_current, &fresh) ? 1 : -1;
    }
    ReleaseSRWLockExclusive(&g_current_lock);
    return r;
}

size_t sh_modmanifest_current_attached(sh_modmanifest_list *out)
{
    size_t i;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    AcquireSRWLockShared(&g_current_lock);
    for (i = 0; i < g_current.count; i++)
        if (g_current.rows[i].attached && !sh_modmanifest_list_add(out, &g_current.rows[i])) break;
    ReleaseSRWLockShared(&g_current_lock);
    return out->count;
}

/* A save renamed a map-level row to the build that serves it: the current
 * manifest follows, so the Mods tab and the next save name that build. */
static void mm_current_rename(const char *from, const sh_modmanifest_row *to)
{
    size_t i;
    if (!strcmp(from, to->bundle)) return;
    AcquireSRWLockExclusive(&g_current_lock);
    if (!mm_list_find(&g_current, to->bundle))
        for (i = 0; i < g_current.count; i++) {
            sh_modmanifest_row *r = &g_current.rows[i];
            if (strcmp(r->bundle, from)) continue;
            strcpy_s(r->id, sizeof r->id, to->id);
            strcpy_s(r->version, sizeof r->version, to->version);
            strcpy_s(r->bundle, sizeof r->bundle, to->bundle);
            r->size = to->size;
            break;
        }
    ReleaseSRWLockExclusive(&g_current_lock);
}

/* Substitutions of the open map (sh_modmanifest_substituted), under g_current_lock. */
#define MM_SUBST_MAX 32
typedef struct mm_subst { char from[SH_MODMANIFEST_HASH_CAP], to[SH_MODMANIFEST_HASH_CAP]; } mm_subst;
static mm_subst g_subst[MM_SUBST_MAX];
static int g_nsubst;
static sh_modmanifest_substitute_fn volatile g_substitute;
static volatile LONG g_editor_load;

void sh_modmanifest_set_substitute(sh_modmanifest_substitute_fn substitute) { g_substitute = substitute; }
void sh_modmanifest_set_editor_load(int editor) { InterlockedExchange(&g_editor_load, editor ? 1 : 0); }

int sh_modmanifest_substituted(const char *declared, char *out, size_t cap)
{
    int i, hit = 0;
    if (!declared) return 0;
    AcquireSRWLockShared(&g_current_lock);
    for (i = 0; i < g_nsubst && !hit; i++)
        if (!strcmp(g_subst[i].from, declared)) {
            hit = 1;
            if (out && cap) strcpy_s(out, cap, g_subst[i].to);
        }
    ReleaseSRWLockShared(&g_current_lock);
    return hit;
}

static void mm_subst_drop(const char *from)
{
    int i;
    AcquireSRWLockExclusive(&g_current_lock);
    for (i = 0; i < g_nsubst; i++)
        if (!strcmp(g_subst[i].from, from)) { g_subst[i] = g_subst[--g_nsubst]; break; }
    ReleaseSRWLockExclusive(&g_current_lock);
}

/* At a load: which declared builds the editor serves through another build. */
static void mm_subst_load(const sh_modmanifest_list *list)
{
    sh_modmanifest_substitute_fn sub = g_substitute;
    int editor = InterlockedCompareExchange(&g_editor_load, 0, 0) != 0;
    size_t i;
    AcquireSRWLockExclusive(&g_current_lock);
    g_nsubst = 0;
    ReleaseSRWLockExclusive(&g_current_lock);
    if (!list || !editor || !sub) return;
    for (i = 0; i < list->count; i++) {
        const sh_modmanifest_row *r = &list->rows[i];
        sh_modserve_bundle_ref ref;
        char to[SH_MODMANIFEST_HASH_CAP] = "", why[400] = "", line[700];
        if (sh_modserve_bundle_serving(r->bundle, NULL, &ref)) continue;   /* its own build serves */
        if (sub(r, to, sizeof to, why, sizeof why) == 1 && sh_modmanifest_valid_hash(to, strlen(to)) && strcmp(to, r->bundle)) {
            AcquireSRWLockExclusive(&g_current_lock);
            if (g_nsubst < MM_SUBST_MAX) {
                strcpy_s(g_subst[g_nsubst].from, sizeof g_subst[g_nsubst].from, r->bundle);
                strcpy_s(g_subst[g_nsubst].to, sizeof g_subst[g_nsubst].to, to);
                g_nsubst++;
            }
            ReleaseSRWLockExclusive(&g_current_lock);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MODS: map: %s: declared at build %.16s, installed build %.16s serves it in the editor; "
                        "'sh_mods map upgrade \"%s\"' (the Mods tab's Upgrade this map) moves the map to it",
                        r->id, r->bundle, to, r->id);
        } else if (why[0]) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: map: %s: %s", r->id, why);
        } else continue;
        backend_log(line);
    }
}

int sh_modmanifest_current_rename(const char *from, const sh_modmanifest_row *to)
{
    int found = 0;
    size_t i;
    if (!from || !to || !sh_modmanifest_valid_hash(to->bundle, strlen(to->bundle)) || !sh_modmanifest_valid_id(to->id))
        return 0;
    AcquireSRWLockShared(&g_current_lock);
    for (i = 0; i < g_current.count && !found; i++) found = !strcmp(g_current.rows[i].bundle, from);
    if (found && mm_list_find(&g_current, to->bundle)) found = 0;
    ReleaseSRWLockShared(&g_current_lock);
    if (found) { mm_current_rename(from, to); mm_subst_drop(from); }
    return found;
}

/* The map scope's hook and its arming (sh_modmanifest_set_scope). */
static sh_modmanifest_scope_fn volatile g_scope;
static int (*volatile g_scope_armed)(void);
static sh_modmanifest_resolve_fn volatile g_resolve;

void sh_modmanifest_set_scope(sh_modmanifest_scope_fn scope, int (*armed)(void))
{
    g_scope = scope;
    g_scope_armed = armed;
}

void sh_modmanifest_set_resolver(sh_modmanifest_resolve_fn resolve) { g_resolve = resolve; }

/* The load's listing: the rows, and the placed names when the hook asks. */
static void mm_scope_load(const char *json, size_t length, const sh_modmanifest_list *rows)
{
    sh_modmanifest_scope_fn scope = g_scope;
    int (*armed)(void) = g_scope_armed;
    sh_modmanifest_list none;
    char **names = NULL;
    size_t count = 0;
    int failed = 0;
    if (!scope) return;
    memset(&none, 0, sizeof none);
    if (armed && armed() > 0 && json && length) {
        count = sh_modmanifest_placed_names(json, length, &names, &failed);
        if (failed) { names = NULL; count = 0; }
    }
    scope(rows ? rows : &none, (const char *const *)names, count, SH_MODSERVE_AT_LOAD);
    sh_modmanifest_names_free(names, count);
}

/* "id@version, id@version, ..." for a log line. */
static void mm_describe(const sh_modmanifest_list *list, char *out, size_t cap)
{
    size_t i, w = 0;
    if (!cap) return;
    out[0] = 0;
    for (i = 0; i < list->count && w + 1 < cap; i++) {
        int n = _snprintf_s(out + w, cap - w, _TRUNCATE, "%s%s@%s", i ? ", " : "",
                            list->rows[i].id, list->rows[i].version);
        if (n < 0) break;
        w += (size_t)n;
    }
}

int sh_modmanifest_activate(const sh_modmanifest_list *rows)
{
    const char **hashes;
    char (*served)[SH_MODMANIFEST_HASH_CAP];
    size_t i;
    int active;
    if (!rows || !rows->count) return modserve_activate_bundles(NULL, 0);
    hashes = (const char **)calloc(rows->count, sizeof *hashes);
    served = (char (*)[SH_MODMANIFEST_HASH_CAP])calloc(rows->count, sizeof *served);
    if (!hashes || !served) { free((void *)hashes); free(served); return -1; }
    /* The asset rule: a row is activated through the build that serves its
     * id now (its own hash or an alias of it, else the installed build of
     * the id), whatever hash the row pins. */
    for (i = 0; i < rows->count; i++) {
        sh_modserve_bundle_ref ref;
        if (sh_modserve_bundle_serving(rows->rows[i].bundle, rows->rows[i].id, &ref) &&
            strlen(ref.hash) == SH_MOD_HASH_CHARS)
            strcpy_s(served[i], sizeof served[i], ref.hash);
        else if (!sh_modmanifest_substituted(rows->rows[i].bundle, served[i], sizeof served[i]))
            strcpy_s(served[i], sizeof served[i], rows->rows[i].bundle);
        hashes[i] = served[i];
    }
    active = modserve_activate_bundles(hashes, (int)rows->count);
    free((void *)hashes);
    free(served);
    return active;
}

/* ==================================================================== */
/* product version                                                       */
/* ==================================================================== */

int sh_modmanifest_version_valid(const char *v)
{
    size_t i, n = v ? strlen(v) : 0;
    if (!n || n >= SH_MODMANIFEST_VERSION_CAP) return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)v[i] <= 0x20 || (unsigned char)v[i] >= 0x7f || v[i] == '"' || v[i] == '\\') return 0;
    return 1;
}

/* MAJOR.MINOR.PATCH and the prerelease text after '-', or 0. */
static int mm_version_parts(const char *v, unsigned long nums[3], const char **pre)
{
    int k;
    if (!v) return 0;
    if (*v == 'v' || *v == 'V') v++;
    for (k = 0; k < 3; k++) {
        char *end;
        if (!sh_shard_is_digit(*v)) return 0;
        nums[k] = strtoul(v, &end, 10);
        v = end;
        if (k < 2) { if (*v != '.') return 0; v++; }
    }
    if (*v == '-' && v[1]) { *pre = v + 1; return 1; }
    if (*v == '+' || !*v) { *pre = NULL; return 1; }
    return 0;
}

int sh_modmanifest_version_compare(const char *a, const char *b)
{
    unsigned long na[3], nb[3];
    const char *pa, *pb;
    int k;
    if (!mm_version_parts(a, na, &pa) || !mm_version_parts(b, nb, &pb)) return -2;
    for (k = 0; k < 3; k++) if (na[k] != nb[k]) return na[k] < nb[k] ? -1 : 1;
    if (!pa || !pb) return !pa && !pb ? 0 : (!pa ? 1 : -1);   /* a release is newer than its prereleases */
    while (*pa || *pb) {
        const char *ea = pa, *eb = pb;
        int da = 1, db = 1, c;
        size_t la, lb;
        while (*ea && *ea != '.' && *ea != '+') { da &= sh_shard_is_digit(*ea) ? 1 : 0; ea++; }
        while (*eb && *eb != '.' && *eb != '+') { db &= sh_shard_is_digit(*eb) ? 1 : 0; eb++; }
        la = (size_t)(ea - pa);
        lb = (size_t)(eb - pb);
        if (!la || !lb) return !la && !lb ? 0 : (!la ? -1 : 1);   /* fewer parts is older */
        if (da && db) {
            unsigned long x = strtoul(pa, NULL, 10), y = strtoul(pb, NULL, 10);
            if (x != y) return x < y ? -1 : 1;
        } else if (da != db) {
            return da ? -1 : 1;   /* numeric parts sort before text */
        } else {
            c = strncmp(pa, pb, la < lb ? la : lb);
            if (c) return c < 0 ? -1 : 1;
            if (la != lb) return la < lb ? -1 : 1;
        }
        pa = *ea == '.' ? ea + 1 : ea;
        pb = *eb == '.' ? eb + 1 : eb;
        if (*pa == '+') pa += strlen(pa);
        if (*pb == '+') pb += strlen(pb);
    }
    return 0;
}

int sh_modmanifest_product_read(const char *json, size_t length, char *out, size_t cap)
{
    size_t pos = 0, hdr_len, chunk_len;
    const char *hdr, *chunk;
    if (out && cap) out[0] = 0;
    if (!json || !length || !out || !cap) return 0;
    if (!sh_shard_find(json, length, SH_MODMANIFEST_PRODUCT_MAGIC, SH_MODMANIFEST_PRODUCT_MAGIC_LEN)) return 0;
    while (sh_shard_next(json, length, SH_MODMANIFEST_PRODUCT_MAGIC, SH_MODMANIFEST_PRODUCT_MAGIC_LEN, &pos,
                         &hdr, &hdr_len, &chunk, &chunk_len)) {
        char v[SH_MODMANIFEST_VERSION_CAP];
        size_t n = hdr_len - SH_MODMANIFEST_PRODUCT_MAGIC_LEN;
        if (hdr_len <= SH_MODMANIFEST_PRODUCT_MAGIC_LEN || n >= sizeof v) continue;
        memcpy(v, hdr + SH_MODMANIFEST_PRODUCT_MAGIC_LEN, n);
        v[n] = 0;
        if (!sh_modmanifest_version_valid(v)) continue;
        strncpy_s(out, cap, v, _TRUNCATE);
        return 1;
    }
    return 0;
}

static int mm_product_filter(const char *hdr, size_t hdr_len, void *ctx)
{
    (void)ctx;
    return hdr_len > SH_MODMANIFEST_PRODUCT_MAGIC_LEN &&
           !memcmp(hdr, SH_MODMANIFEST_PRODUCT_MAGIC, SH_MODMANIFEST_PRODUCT_MAGIC_LEN);
}

char *sh_modmanifest_product_put(const char *json, size_t length, const char *version, size_t *out_length)
{
    char *base = NULL, *out = NULL, header[SH_MODMANIFEST_PRODUCT_MAGIC_LEN + SH_MODMANIFEST_VERSION_CAP];
    size_t base_len = 0, w = 0;
    unsigned elements = 0, runs = 0;
    int failed = 0, valid = sh_modmanifest_version_valid(version);
    sh_shard_out part;
    if (out_length) *out_length = 0;
    if (!json || !length) return NULL;
    if (sh_shard_find(json, length, SH_MODMANIFEST_PRODUCT_MAGIC, SH_MODMANIFEST_PRODUCT_MAGIC_LEN)) {
        base = sh_shard_strip(json, length, SH_MODMANIFEST_PRODUCT_MAGIC, SH_MODMANIFEST_PRODUCT_MAGIC_LEN,
                              mm_product_filter, NULL, 64, &base_len, &elements, &runs, &failed);
        if (failed) { if (base) HeapFree(GetProcessHeap(), 0, base); return NULL; }
    }
    if (!valid) {
        if (base && out_length) *out_length = base_len;
        return base;
    }
    _snprintf_s(header, sizeof header, _TRUNCATE, "%s%s", SH_MODMANIFEST_PRODUCT_MAGIC, version);
    part.header = header;
    part.chunk = version;             /* a valid version needs no escaping */
    part.chunk_len = strlen(version);
    out = sh_shard_insert(base ? base : json, base ? base_len : length, &part, 1, &w, NULL, 0);
    if (base) HeapFree(GetProcessHeap(), 0, base);
    if (out && out_length) *out_length = w;
    return out;
}

static SRWLOCK g_product_lock = SRWLOCK_INIT;
static char g_product_override[SH_MODMANIFEST_VERSION_CAP];
static int g_product_overridden;
static char g_product_map[SH_MODMANIFEST_VERSION_CAP];   /* the open map's */
static sh_modmanifest_product_need_fn volatile g_product_need;

void sh_modmanifest_set_product_version(const char *version)
{
    AcquireSRWLockExclusive(&g_product_lock);
    g_product_overridden = version != NULL;
    strncpy_s(g_product_override, sizeof g_product_override, version ? version : "", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_product_lock);
}

void sh_modmanifest_set_product_need(sh_modmanifest_product_need_fn need) { g_product_need = need; }

int sh_modmanifest_product_version(char *out, size_t cap)
{
    char v[SH_MODMANIFEST_VERSION_CAP] = "";
    if (!out || !cap) return 0;
    out[0] = 0;
    AcquireSRWLockShared(&g_product_lock);
    if (g_product_overridden) strcpy_s(v, sizeof v, g_product_override);
#ifdef SH_PRODUCT_VERSION
    else strncpy_s(v, sizeof v, SH_PRODUCT_VERSION, _TRUNCATE);
#endif
    ReleaseSRWLockShared(&g_product_lock);
    if (!sh_modmanifest_version_valid(v)) return 0;
    strncpy_s(out, cap, v, _TRUNCATE);
    return 1;
}

static void mm_product_set_map(const char *v)
{
    AcquireSRWLockExclusive(&g_product_lock);
    strncpy_s(g_product_map, sizeof g_product_map, v ? v : "", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_product_lock);
}

int sh_modmanifest_product(char *out, size_t cap)
{
    if (!out || !cap) return 0;
    AcquireSRWLockShared(&g_product_lock);
    strncpy_s(out, cap, g_product_map, _TRUNCATE);
    ReleaseSRWLockShared(&g_product_lock);
    return out[0] != 0;
}

static char *mm_prepare_rows(const char *json, size_t length, size_t *out_length);

char *sh_modmanifest_prepare_load(const char *json, size_t length, size_t *out_length)
{
    char v[SH_MODMANIFEST_VERSION_CAP], line[160];
    char *bare, *rows;
    size_t bare_len = 0, rows_len = 0;
    if (out_length) *out_length = 0;
    if (!json || !length || !sh_shard_find(json, length, SH_MODMANIFEST_PRODUCT_MAGIC, SH_MODMANIFEST_PRODUCT_MAGIC_LEN)) {
        mm_product_set_map(NULL);
        return mm_prepare_rows(json, length, out_length);
    }
    (void)sh_modmanifest_product_read(json, length, v, sizeof v);
    mm_product_set_map(v);
    if (v[0]) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: the map needs Snapmap+ %s or later", v);
        backend_log(line);
    }
    bare = sh_modmanifest_product_put(json, length, NULL, &bare_len);
    rows = mm_prepare_rows(bare ? bare : json, bare ? bare_len : length, &rows_len);
    if (rows) {
        if (bare) HeapFree(GetProcessHeap(), 0, bare);
        if (out_length) *out_length = rows_len;
        return rows;
    }
    if (bare && out_length) *out_length = bare_len;
    return bare;
}

static char *mm_prepare_rows(const char *json, size_t length, size_t *out_length)
{
    sh_modmanifest_list list = {0};
    unsigned rejected = 0, removed = 0;
    int doc_failed = 0, active;
    size_t stripped_len = 0;
    char *stripped = NULL;
    char line[512], names[320];

    if (out_length) *out_length = 0;
    if (!json || !length ||
        !sh_shard_find(json, length, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN)) {
        mm_set_current(NULL);
        mm_subst_load(NULL);
        mm_choices_load(NULL);
        mm_scope_load(json, length, NULL);
        modserve_activate_bundles(NULL, 0);
        return NULL;
    }
    sh_modmanifest_read(json, length, &list, &rejected);
    stripped = sh_modmanifest_strip(json, length, &stripped_len, &removed, &doc_failed);

    mm_describe(&list, names, sizeof names);
    /* The map scope first: the bundles' shadows and requirements are live
     * before their gameplay decls register; the registry choices before the
     * listing that computes the registries. */
    mm_subst_load(&list);
    mm_choices_load(&list);
    mm_scope_load(json, length, &list);
    active = sh_modmanifest_activate(&list);
    /* `active` counts the registration steps the activation found ready (new
     * names; a bundle of shadows and requirements has none), not bundles. */
    {
        char what[64];
        if (active < 0) strcpy_s(what, sizeof what, "activation refused (see above)");
        else if (!active) strcpy_s(what, sizeof what, "no new decls to register");
        else _snprintf_s(what, sizeof what, _TRUNCATE, "%d registration step(s) ready", active);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: map manifest names %zu bundle(s)%s%s; %u row(s) removed before parse; %s",
            list.count, list.count ? ": " : "", names, removed, what);
    }
    backend_log(line);
    if (rejected) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: %u manifest row(s) did not parse and were dropped", rejected);
        backend_log(line);
    }
    if (doc_failed)
        backend_log("MODS: the manifest rows could not be removed (the map's JSON did not read "
                    "cleanly); they stay as inert string variables");
    mm_set_current(&list);
    sh_modmanifest_list_free(&list);
    if (stripped && out_length) *out_length = stripped_len;
    return stripped;
}

typedef struct mm_save_ctx { sh_modmanifest_list list; int failed; } mm_save_ctx;

static void mm_save_visit(void *context, const char *bundle_id, const char *version,
                          const char *bundle_hash, unsigned long long size, const char *name)
{
    mm_save_ctx *ctx = (mm_save_ctx *)context;
    sh_modmanifest_row *row;
    if (!ctx || ctx->failed || !bundle_id || !bundle_hash) return;
    if (!sh_modmanifest_valid_id(bundle_id) ||
        !sh_modmanifest_valid_hash(bundle_hash, strlen(bundle_hash)) ||
        (version && strlen(version) >= SH_MODMANIFEST_VERSION_CAP)) {
        ctx->failed = 1;
        return;
    }
    row = mm_list_find(&ctx->list, bundle_hash);
    if (!row) {
        sh_modmanifest_row fresh;
        memset(&fresh, 0, sizeof fresh);
        strcpy_s(fresh.id, sizeof fresh.id, bundle_id);
        strcpy_s(fresh.version, sizeof fresh.version, version ? version : "");
        strcpy_s(fresh.bundle, sizeof fresh.bundle, bundle_hash);
        fresh.size = size;
        if (!sh_modmanifest_list_add(&ctx->list, &fresh)) { ctx->failed = 1; return; }
        row = &ctx->list.rows[ctx->list.count - 1];
    }
    if (name && strlen(name) <= SH_MODMANIFEST_NAME_MAX && !sh_modmanifest_row_add_name(row, name))
        ctx->failed = 1;
}

static int mm_row_order(const void *a, const void *b)
{
    const sh_modmanifest_row *x = (const sh_modmanifest_row *)a, *y = (const sh_modmanifest_row *)b;
    int c = strcmp(x->id, y->id);
    return c ? c : strcmp(x->bundle, y->bundle);
}

static int mm_cstr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Add the map-level rows to the save's rows: a bundle the placed closure
 * already names gets its row marked, any other is added, renamed to the build
 * that serves it when the resolver knows one. */
static int mm_save_attached(mm_save_ctx *ctx, const sh_modmanifest_list *attached)
{
    sh_modmanifest_resolve_fn resolve = g_resolve;
    size_t i;
    for (i = 0; i < attached->count; i++) {
        sh_modmanifest_row row = attached->rows[i], served;
        sh_modmanifest_row *have;
        memset(&served, 0, sizeof served);
        row.names = NULL;
        row.name_count = 0;
        if (resolve && resolve(row.bundle, row.id, &served) == 1 && sh_modmanifest_valid_id(served.id) &&
            sh_modmanifest_valid_hash(served.bundle, strlen(served.bundle))) {
            mm_current_rename(row.bundle, &served);
            strcpy_s(row.id, sizeof row.id, served.id);
            strcpy_s(row.version, sizeof row.version, served.version);
            strcpy_s(row.bundle, sizeof row.bundle, served.bundle);
            row.size = served.size;
        }
        sh_modmanifest_row_free(&served);
        row.attached = 1;
        have = mm_list_find(&ctx->list, row.bundle);
        if (have) { have->attached = 1; continue; }
        if (!sh_modmanifest_list_add(&ctx->list, &row)) return 0;
    }
    return 1;
}

static char *mm_save_rows(const char *json, size_t length, size_t *out_length, int *need);

/* A product-only feature in the saved text: a Grid Room variant module. */
static int mm_product_feature(const char *json, size_t length)
{
    return sh_shard_find(json, length, "maps/modules/smpgrid/", 21) != NULL;
}

char *sh_modmanifest_on_save(const char *json, size_t length, size_t *out_length)
{
    char *rows, *out, v[SH_MODMANIFEST_VERSION_CAP], have[SH_MODMANIFEST_VERSION_CAP], line[200];
    const char *base;
    size_t rows_len = 0, base_len, out_len = 0;
    int need = 0, stale;
    if (out_length) *out_length = 0;
    rows = mm_save_rows(json, length, &rows_len, &need);
    if (!json || !length) return rows;
    base = rows ? rows : json;
    base_len = rows ? rows_len : length;
    if (!need) need = mm_product_feature(base, base_len);
    stale = sh_modmanifest_product_read(base, base_len, have, sizeof have) ||
            sh_shard_find(base, base_len, SH_MODMANIFEST_PRODUCT_MAGIC, SH_MODMANIFEST_PRODUCT_MAGIC_LEN) != NULL;
    if (need && !sh_modmanifest_product_version(v, sizeof v)) {
        backend_log("MODS: save: the map uses Snapmap+ features, but this build has no product version; none is recorded");
        need = 0;
    }
    if (!need) v[0] = 0;
    mm_product_set_map(v);
    if (!need && !stale) {
        if (out_length) *out_length = rows_len;
        return rows;
    }
    if (need && stale && !strcmp(have, v)) {
        if (out_length) *out_length = rows_len;
        return rows;
    }
    out = sh_modmanifest_product_put(base, base_len, need ? v : NULL, &out_len);
    if (!out) {
        if (out_length) *out_length = rows_len;
        return rows;
    }
    if (rows) HeapFree(GetProcessHeap(), 0, rows);
    if (need) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: save: the map records that it needs Snapmap+ %s or later", v);
        backend_log(line);
    }
    if (out_length) *out_length = out_len;
    return out;
}

static char *mm_save_rows(const char *json, size_t length, size_t *out_length, int *need)
{
    char **names = NULL;
    size_t count, i, written = 0;
    int failed = 0, stale;
    mm_save_ctx ctx;
    sh_modmanifest_list attached;
    sh_modmanifest_scope_fn scope = g_scope;
    char *out = NULL;
    char err[192] = "", line[512], described[320];

    if (out_length) *out_length = 0;
    if (!json || !length) return NULL;
    memset(&ctx, 0, sizeof ctx);
    stale = sh_shard_find(json, length, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN) != NULL;
    /* No installed bundle serves anything, the map carries no rows and no
     * map-level mod: the save is the engine's own bytes, without walking the
     * map. */
    (void)sh_modmanifest_current_attached(&attached);
    if (!stale && !attached.count && !sh_modserve_active()) {
        if (scope && g_scope_armed && g_scope_armed() > 0) scope(&ctx.list, NULL, 0, SH_MODSERVE_AT_SAVE);
        return NULL;
    }
    count = sh_modmanifest_placed_names(json, length, &names, &failed);
    if (failed) {
        backend_log("MODS: save manifest skipped -- the saved map's JSON did not read cleanly; "
                    "the save carries no manifest rows");
        sh_modmanifest_list_free(&attached);
        return NULL;
    }
    if (count) modserve_bundles_for_names_visit((const char *const *)names, count, mm_save_visit, &ctx);
    sh_modmanifest_names_free(names, count);
    if (!ctx.failed && attached.count && !mm_save_attached(&ctx, &attached)) ctx.failed = 1;
    sh_modmanifest_list_free(&attached);
    if (ctx.failed) {
        backend_log("MODS: save manifest skipped -- the serving layer reported an invalid bundle");
        sh_modmanifest_list_free(&ctx.list);
        return NULL;
    }
    /* What the map now lists: placed entities may have been added since the load. */
    if (scope) scope(&ctx.list, NULL, 0, SH_MODSERVE_AT_SAVE);
    if (ctx.list.count == 0 && !stale) return NULL;

    qsort(ctx.list.rows, ctx.list.count, sizeof *ctx.list.rows, mm_row_order);
    mm_choices_save(&ctx.list);
    {
        /* The product version a map needs: a registry choice, or a declared
         * mod with registry intent or surfaced content (modui's hook). */
        sh_modmanifest_product_need_fn pn = g_product_need;
        for (i = 0; i < ctx.list.count && !*need; i++) *need = ctx.list.rows[i].choices[0] != 0;
        if (!*need && pn && ctx.list.count) *need = pn(&ctx.list) > 0;
    }
    for (i = 0; i < ctx.list.count; i++)
        if (ctx.list.rows[i].name_count > 1)
            qsort(ctx.list.rows[i].names, ctx.list.rows[i].name_count, sizeof(char *), mm_cstr_cmp);
    out = sh_modmanifest_write(json, length, &ctx.list, &written, err, sizeof err);
    mm_describe(&ctx.list, described, sizeof described);
    if (out) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: save manifest wrote %zu bundle row(s)%s%s%s; %zu -> %zu bytes",
            ctx.list.count, ctx.list.count ? ": " : "", described,
            stale ? " (stale rows replaced)" : "", length, written);
        if (out_length) *out_length = written;
    } else {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: save manifest FAILED (%s); the save carries no manifest rows for %s",
            err[0] ? err : "unknown", described[0] ? described : "its bundles");
    }
    backend_log(line);
    /* The open map's record is what it now saves: `sh_mods map`, the Mods tab
     * and the next listing read these rows, not the ones it was loaded with
     * (a map opened from a rawmap had none). */
    if (out) mm_set_current(&ctx.list);
    sh_modmanifest_list_free(&ctx.list);
    return out;
}
