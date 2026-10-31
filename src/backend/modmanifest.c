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
    size_t keep, i;
    if (out_length) *out_length = 0;
    if (!row) return NULL;
    /* Drop names from the end until the row fits its budget. */
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
        mm_put(&t, ",\"names\":[", 10);
        for (i = 0; i < keep; i++) {
            if (i) mm_put(&t, ",", 1);
            mm_put_quoted(&t, row->names[i]);
        }
        mm_put(&t, "]}", 2);
        if (t.failed) { free(t.p); return NULL; }
        if (t.n <= SH_MODMANIFEST_ROW_MAX || keep == 0) {
            if (t.n > SH_MODMANIFEST_ROW_MAX) { free(t.p); return NULL; }
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

void sh_modmanifest_reset(void) { mm_set_current(NULL); }

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
    scope(rows ? rows : &none, (const char *const *)names, count, SH_MODSERVE_RESTORE_LOAD);
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

char *sh_modmanifest_prepare_load(const char *json, size_t length, size_t *out_length)
{
    sh_modmanifest_list list = {0};
    const char **hashes = NULL;
    unsigned rejected = 0, removed = 0;
    int doc_failed = 0, active;
    size_t i, stripped_len = 0;
    char *stripped = NULL;
    char line[512], names[320];

    if (out_length) *out_length = 0;
    if (!json || !length ||
        !sh_shard_find(json, length, SH_MODMANIFEST_MAGIC, SH_MODMANIFEST_MAGIC_LEN)) {
        mm_set_current(NULL);
        mm_scope_load(json, length, NULL);
        modserve_activate_bundles(NULL, 0);
        return NULL;
    }
    sh_modmanifest_read(json, length, &list, &rejected);
    stripped = sh_modmanifest_strip(json, length, &stripped_len, &removed, &doc_failed);

    if (list.count) {
        hashes = (const char **)calloc(list.count, sizeof *hashes);
        if (hashes) for (i = 0; i < list.count; i++) hashes[i] = list.rows[i].bundle;
    }
    mm_describe(&list, names, sizeof names);
    /* The map scope first: the bundles' shadows and requirements are live
     * before their gameplay decls register. */
    mm_scope_load(json, length, &list);
    active = modserve_activate_bundles(hashes, hashes ? (int)list.count : 0);
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "MODS: map manifest names %zu bundle(s)%s%s; %u row(s) removed before parse; %d active",
        list.count, list.count ? ": " : "", names, removed, active);
    backend_log(line);
    if (rejected) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: %u manifest row(s) did not parse and were dropped", rejected);
        backend_log(line);
    }
    if (doc_failed)
        backend_log("MODS: the manifest rows could not be removed (the map's JSON did not read "
                    "cleanly); they stay as inert string variables");
    free((void *)hashes);
    mm_set_current(&list);
    sh_modmanifest_list_free(&list);
    if (stripped && out_length) *out_length = stripped_len;
    return stripped;
}

typedef struct mm_save_ctx { sh_modmanifest_list list; int failed; } mm_save_ctx;

static int (*volatile g_restore)(const char *const *, int, const char *const *, size_t, int);
static int (*volatile g_restore_armed)(void);

void sh_modmanifest_set_restore(int (*restore)(const char *const *hashes, int hash_count, const char *const *names,
                                               size_t name_count, int when), int (*armed)(void))
{
    g_restore = restore;
    g_restore_armed = armed;
}

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

char *sh_modmanifest_on_save(const char *json, size_t length, size_t *out_length)
{
    char **names = NULL;
    size_t count, i, written = 0;
    int failed = 0, stale, restore;
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
    restore = g_restore && g_restore_armed && g_restore_armed() > 0;
    (void)sh_modmanifest_current_attached(&attached);
    if (!stale && !restore && !attached.count && !sh_modserve_active()) {
        if (scope && g_scope_armed && g_scope_armed() > 0) scope(&ctx.list, NULL, 0, SH_MODSERVE_RESTORE_SAVE);
        return NULL;
    }
    count = sh_modmanifest_placed_names(json, length, &names, &failed);
    if (failed) {
        backend_log("MODS: save manifest skipped -- the saved map's JSON did not read cleanly; "
                    "the save carries no manifest rows");
        sh_modmanifest_list_free(&attached);
        return NULL;
    }
    /* An uninstalled mod the map still places, or a map-level mod that was
     * uninstalled, is reinstated first (its folder back in mods\, the index
     * rebuilt), so the save writes its row from the build that serves. */
    if (restore && (count || attached.count)) {
        const char **hashes = attached.count ? (const char **)calloc(attached.count, sizeof *hashes) : NULL;
        for (i = 0; hashes && i < attached.count; i++) hashes[i] = attached.rows[i].bundle;
        (void)g_restore(hashes, hashes ? (int)attached.count : 0, count ? (const char *const *)names : NULL, count,
                        SH_MODSERVE_RESTORE_SAVE);
        free((void *)hashes);
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
    if (scope) scope(&ctx.list, NULL, 0, SH_MODSERVE_RESTORE_SAVE);
    if (ctx.list.count == 0 && !stale) return NULL;

    qsort(ctx.list.rows, ctx.list.count, sizeof *ctx.list.rows, mm_row_order);
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
    sh_modmanifest_list_free(&ctx.list);
    return out;
}
