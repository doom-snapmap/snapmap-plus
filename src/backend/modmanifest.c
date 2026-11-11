/* Map manifest rows: read, write, and the save-time closure. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modmanifest.h"
#include "modserve.h"
#include "modsalt.h"
#include "map_carrier.h"
#include "config_json.h"
#include "backend_log.h"

#define MM_DEPTH_MAX 256u   /* JSON nesting a manifest scan follows */
static int mm_space(char c) { return c == 0x20 || c == 0x09 || c == 0x0d || c == 0x0a; }
static int mm_isdigit(char c) { return c >= '0' && c <= '9'; }
static int mm_ishex(char c) { return mm_isdigit(c) || (c >= 'a' && c <= 'f'); }

/* A NUL-terminated salt (modsalt.h). */
static int mm_salt_ok(const char *salt) { return salt && sh_salt_valid(salt, strlen(salt)); }

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

/* The row of `salt` at `build`; NULL salt matches the build alone. */
static sh_modmanifest_row *mm_list_find(sh_modmanifest_list *list, const char *salt, const char *build)
{
    size_t i;
    for (i = 0; i < list->count; i++)
        if (!strcmp(list->rows[i].build, build) && (!salt || !strcmp(list->rows[i].salt, salt)))
            return &list->rows[i];
    return NULL;
}

/* ==================================================================== */
/* grammar                                                               */
/* ==================================================================== */

int sh_modmanifest_valid_hash(const char *text, size_t length)
{
    size_t i;
    if (!text || length != SH_MOD_HASH_CHARS) return 0;
    for (i = 0; i < length; i++) if (!mm_ishex(text[i])) return 0;
    return 1;
}

/* ==================================================================== */
/* small JSON helpers                                                    */
/* ==================================================================== */

static const char *mm_ws(const char *p, const char *end)
{
    while (p < end && mm_space(*p)) p++;
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
        while (p < end && *p != ',' && *p != '}' && *p != ']' && !mm_space(*p)) p++;
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
        if (!mm_isdigit(*p)) return 0;
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
    if (sh_json_object_get(&object, "id") || sh_json_object_get(&object, "bundle")) {
        MM_ERR("row is in an earlier shape (it names an id or a bundle)");
        goto done;
    }
    {
        char mod[SH_MODMANIFEST_SALT_CAP + SH_MODMANIFEST_HASH_CAP + 1];
        if (!mm_member_text(&object, "mod", mod, sizeof mod, 1) || strlen(mod) != SH_MODMANIFEST_SALT_LEN + 1 + SH_MOD_HASH_CHARS ||
            mod[SH_MODMANIFEST_SALT_LEN] != '@') {
            MM_ERR("row has no valid mod");
            goto done;
        }
        memcpy(out->salt, mod, SH_MODMANIFEST_SALT_LEN);
        out->salt[SH_MODMANIFEST_SALT_LEN] = 0;
        memcpy(out->build, mod + SH_MODMANIFEST_SALT_LEN + 1, SH_MOD_HASH_CHARS + 1);
        if (!mm_salt_ok(out->salt) || !sh_modmanifest_valid_hash(out->build, strlen(out->build))) {
            MM_ERR("row has no valid mod");
            goto done;
        }
    }
    if (!mm_member_text(&object, "name", out->name, sizeof out->name, 0)) {
        MM_ERR("row '%s' has no valid name", out->salt); goto done;
    }
    if (!mm_member_uint(&object, "size", &out->size)) {
        MM_ERR("row '%s' has no valid size", out->salt); goto done;
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
                    if (mm_slot_ok(slot) && mm_salt_ok(key))
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
        if (r < 0) { MM_ERR("row '%s' has a malformed names list", out->salt); goto done; }
    }
    ok = 1;
done:
    sh_json_object_free(&object);
    if (!ok) sh_modmanifest_row_free(out);
    return ok;
}

char *sh_modmanifest_format_value(const sh_modmanifest_row *row, size_t *out_length)
{
    size_t i, c;
    mm_text t = {0};
    char number[32];
    const char *p;
    if (out_length) *out_length = 0;
    if (!row) return NULL;
    if (!mm_salt_ok(row->salt) || !sh_modmanifest_valid_hash(row->build, strlen(row->build)))
        return NULL;
    mm_put(&t, "{\"mod\":\"", 8);
    mm_put(&t, row->salt, strlen(row->salt));
    mm_put(&t, "@", 1);
    mm_put(&t, row->build, strlen(row->build));
    mm_put(&t, "\",\"name\":", 9);
    mm_put_quoted(&t, row->name);
    _snprintf_s(number, sizeof number, _TRUNCATE, ",\"size\":%llu", row->size);
    mm_put(&t, number, strlen(number));
    mm_put(&t, ",\"names\":[", 10);
    for (i = 0; i < row->name_count; i++) {
        if (i) mm_put(&t, ",", 1);
        mm_put_quoted(&t, row->names[i]);
    }
    mm_put(&t, "]", 1);
    if (row->attached) mm_put(&t, ",\"attached\":true", 16);
    if (mm_choice_count(row->choices)) {
        mm_put(&t, ",\"choices\":{", 12);
        for (c = 0, p = row->choices; p && *p; c++) {
            const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
            char slot[SH_MODMANIFEST_SLOT_MAX], key[SH_MODMANIFEST_SALT_CAP];
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
    mm_put(&t, "}", 1);
    if (t.failed) { free(t.p); return NULL; }
    if (out_length) *out_length = t.n;
    return t.p;
}

/* ==================================================================== */
/* map text                                                              */
/* ==================================================================== */

/* Add a parsed row to `out`, merging a row of the same salt and build. */
static int mm_merge_row(sh_modmanifest_list *out, sh_modmanifest_row *row)
{
    sh_modmanifest_row *existing = mm_list_find(out, row->salt, row->build);
    int ok = 1;
    if (existing) {
        size_t i;
        if (row->attached) existing->attached = 1;
        for (i = 0; i < row->name_count; i++) sh_modmanifest_row_add_name(existing, row->names[i]);
    } else ok = sh_modmanifest_list_add(out, row);
    sh_modmanifest_row_free(row);
    return ok;
}

/* The member's "mods" array [from, to): one row object per element. */
static size_t mm_read_member(const char *from, const char *to, sh_modmanifest_list *out, unsigned *bad)
{
    const char *cursor = from, *elem;
    size_t elem_len, read = 0;
    while (out->count < SH_MODMANIFEST_MAX_ROWS &&
           sh_carrier_array_next(&cursor, to, &elem, &elem_len) == 1) {
        sh_modmanifest_row row;
        if (*elem != '{' || !sh_modmanifest_parse_value(elem, elem_len, &row, NULL, 0)) { (*bad)++; continue; }
        if (!mm_merge_row(out, &row)) (*bad)++;
        read++;
    }
    return read;
}

size_t sh_modmanifest_read(const char *json, size_t length, sh_modmanifest_list *out,
                           unsigned *rejected)
{
    size_t from = 0, to = 0, read = 0;
    unsigned bad = 0;
    if (rejected) *rejected = 0;
    if (!json || !length || !out) return 0;
    if (sh_carrier_part(json, length, "mods", &from, &to) != 1) return 0;
    read = mm_read_member(json + from, json + to, out, &bad);
    if (rejected) *rejected = bad;
    return read;
}

char *sh_modmanifest_write(const char *json, size_t length, const sh_modmanifest_list *rows,
                           size_t *out_length, char *err, size_t err_cap)
{
    char *out = NULL;
    const char *src = json;
    size_t src_len = length, i, count = rows ? rows->count : 0, w = 0;
    mm_text value = {0};
    if (out_length) *out_length = 0;
    if (err && err_cap) err[0] = 0;
    if (!json || !length) { MM_ERR("no map text"); return NULL; }
    if (count > SH_MODMANIFEST_MAX_ROWS) { MM_ERR("This map uses more than 4096 mod builds; it was saved without its mod list"); goto done; }
    if (count) {
        mm_put(&value, "[", 1);
        for (i = 0; i < count; i++) {
            size_t vlen = 0;
            char *row = sh_modmanifest_format_value(&rows->rows[i], &vlen);
            if (!row) { MM_ERR("The mod %s could not be recorded in the map; the map was saved without its mod list", rows->rows[i].name[0] ? rows->rows[i].name : rows->rows[i].salt); goto done; }
            if (i) mm_put(&value, ",", 1);
            mm_put(&value, row, vlen);
            free(row);
        }
        mm_put(&value, "]", 1);
        if (value.failed) { MM_ERR("Out of memory while recording the map's mods; the map was saved without its mod list"); goto done; }
    }
    out = sh_carrier_put(src, src_len, "mods", count ? value.p : NULL, value.n, &w, err, err_cap);
    if (out) {
        if (out_length) *out_length = w;
    } else if (err && err_cap && err[0]) {
        /* failed; err says why */
    } else if (count) {
        /* Already exactly this: a copy, so a caller always gets the text it asked for. */
        out = (char *)HeapAlloc(GetProcessHeap(), 0, src_len + 1);
        if (!out) MM_ERR("Out of memory while recording the map's mods; the map was saved without its mod list");
        else {
            memcpy(out, src, src_len);
            out[src_len] = 0;
            if (out_length) *out_length = src_len;
        }
    } else MM_ERR("nothing to write");
done:
    free(value.p);
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
    mm_frame stack[MM_DEPTH_MAX];
    unsigned depth = 0;
    mm_names s = {0};
    const char *p = json, *end = json + length;
    size_t i, w;
    if (out_names) *out_names = NULL;
    if (failed) *failed = 0;
    if (!json || !length || !out_names) { if (failed) *failed = 1; return 0; }

    while (p < end) {
        char c = *p;
        if (mm_space(c) || c == ':') { p++; continue; }
        if (c == '{' || c == '[') {
            int edit = 0;
            if (depth >= MM_DEPTH_MAX) goto bad;
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
    int ok = 0;
    if (err && err_cap) err[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (text && length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) { text += 3; length -= 3; }
    while (length && mm_space(*text)) { text++; length--; }
    if (!text || !length || !sh_json_parse_object(text, length, 24, &object)) {
        MM_ERR("mod.json is not a JSON object");
        return 0;
    }
    {
        /* The name as the builder reads it: trimmed of ASCII whitespace. */
        char name[4096];
        size_t a = 0, n;
        if (!mm_member_text(&object, "name", name, sizeof name, 1)) name[0] = 0;
        n = strlen(name);
        while (a < n && mm_space(name[a])) a++;
        while (n > a && mm_space(name[n - 1])) n--;
        if (n == a || n - a >= sizeof out->name) {
            MM_ERR("mod.json has no name of 1 to 80 characters; give the mod a shorter name and press Refresh");
            goto done;
        }
        memcpy(out->name, name + a, n - a);
        out->name[n - a] = 0;
    }

    raw = sh_json_object_get(&object, "snapmap-plus");
    if (!raw || *raw != '{' || !sh_json_parse_object(raw, strlen(raw), 24, &block)) {
        MM_ERR("This mod has not been built yet; press Refresh in the Mods tab"); goto done;
    }
    if (!mm_member_text(&block, "salt", out->salt, sizeof out->salt, 1) || !mm_salt_ok(out->salt)) {
        MM_ERR("This mod folder has not been scanned yet; press Refresh in the Mods tab"); goto done;
    }
    if (!mm_member_text(&block, "build", out->build, sizeof out->build, 1) ||
        !sh_modmanifest_valid_hash(out->build, strlen(out->build))) {
        MM_ERR("mod.json has not been built (no build hash)"); goto done;
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
            char slot[SH_MODMANIFEST_SLOT_MAX], key[SH_MODMANIFEST_SALT_CAP];
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
    if (!mm_slot_ok(slot) || (key && key[0] && !mm_salt_ok(key))) return -1;
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

/* At a save: each choice on the row of the salt it names. */
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
            if (!strcmp(rows->rows[i].salt, tab + 1))
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
    /* The serving index forgets them too: a closed map's choice never applies to the next listing. */
    (void)mm_choices_push(0);
    (void)sh_modmanifest_load_notes_flush(0);
}

static void mm_subst_load(const sh_modmanifest_list *list, const char *json, size_t length);

void sh_modmanifest_current_close(void)
{
    sh_modmanifest_reset();
    mm_subst_load(NULL, NULL, 0);
}

static void mm_subst_drop(const char *from);

size_t sh_modmanifest_current_remove(const char *bundle)
{
    size_t i, w, dropped = 0;
    if (!bundle || !bundle[0]) return 0;
    AcquireSRWLockExclusive(&g_current_lock);
    for (i = 0, w = 0; i < g_current.count; i++) {
        if (!strcmp(g_current.rows[i].build, bundle)) { sh_modmanifest_row_free(&g_current.rows[i]); dropped++; continue; }
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
    if (!row || !mm_salt_ok(row->salt) || !sh_modmanifest_valid_hash(row->build, strlen(row->build))) return -1;
    AcquireSRWLockExclusive(&g_current_lock);
    have = mm_list_find(&g_current, row->salt, row->build);
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
    if (!strcmp(from, to->build)) return;
    AcquireSRWLockExclusive(&g_current_lock);
    if (!mm_list_find(&g_current, to->salt, to->build))
        for (i = 0; i < g_current.count; i++) {
            sh_modmanifest_row *r = &g_current.rows[i];
            if (strcmp(r->build, from)) continue;
            strcpy_s(r->salt, sizeof r->salt, to->salt);
            strcpy_s(r->name, sizeof r->name, to->name);
            strcpy_s(r->build, sizeof r->build, to->build);
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

/* The held cause sentences of the last editor load (sh_modmanifest_load_notes_flush). */
#define MM_NOTES_MAX 16
static char g_notes[MM_NOTES_MAX][700];
static int g_nnotes, g_notes_over;
static char g_refusal[700];   /* the first note whose row the placed entities name */

int sh_modmanifest_load_refusal(char *out, size_t cap)
{
    int have;
    AcquireSRWLockShared(&g_current_lock);
    have = g_refusal[0] != 0;
    if (out && cap) strncpy_s(out, cap, g_refusal, _TRUNCATE);
    ReleaseSRWLockShared(&g_current_lock);
    return have;
}

int sh_modmanifest_load_notes_pending(void)
{
    int n;
    AcquireSRWLockShared(&g_current_lock);
    n = g_nnotes;
    ReleaseSRWLockShared(&g_current_lock);
    return n;
}

int sh_modmanifest_load_notes_flush(int say)
{
    char lines[MM_NOTES_MAX][700];
    int n, over, i;
    AcquireSRWLockExclusive(&g_current_lock);
    n = g_nnotes;
    over = g_notes_over;
    memcpy(lines, g_notes, sizeof lines[0] * (size_t)n);
    g_nnotes = g_notes_over = 0;
    g_refusal[0] = 0;
    ReleaseSRWLockExclusive(&g_current_lock);
    for (i = 0; say && i < n; i++) backend_log(lines[i]);
    if (say && over) {
        char line[96];
        _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: map: %d more row(s) nothing serves", over);
        backend_log(line);
    }
    return n + over;
}

/* 1 when a placed name lies in the row's namespace "mods/<salt>/" (a salt has
 * one spelling: no '-' for the engine to cook). */
static int mm_row_placed(const sh_modmanifest_row *r, char **names, size_t count)
{
    char a[SH_MODMANIFEST_SALT_CAP + 8];
    size_t i, n;
    if (!r->salt[0] || !names) return 0;
    _snprintf_s(a, sizeof a, _TRUNCATE, "mods/%s/", r->salt);
    n = strlen(a);
    for (i = 0; i < count; i++)
        if (names[i] && !_strnicmp(names[i], a, n)) return 1;
    return 0;
}

/* At a load: which declared builds the editor serves through another build. */
static void mm_subst_load(const sh_modmanifest_list *list, const char *json, size_t length)
{
    sh_modmanifest_substitute_fn sub = g_substitute;
    int editor = InterlockedCompareExchange(&g_editor_load, 0, 0) != 0;
    char **names = NULL;
    size_t i, count = 0;
    int named = 0;
    AcquireSRWLockExclusive(&g_current_lock);
    g_nsubst = 0;
    g_nnotes = g_notes_over = 0;
    g_refusal[0] = 0;
    ReleaseSRWLockExclusive(&g_current_lock);
    if (!list || !editor || !sub) return;
    for (i = 0; i < list->count; i++) {
        const sh_modmanifest_row *r = &list->rows[i];
        sh_modserve_bundle_ref ref;
        char to[SH_MODMANIFEST_HASH_CAP] = "", why[400] = "", line[700];
        int placed;
        memset(&ref, 0, sizeof ref);
        if (sh_modserve_row_build(r, &ref)) continue;   /* a place or the root serves it */
        /* The placed names, read once, for the rows nothing serves. */
        if (!named && json && length) {
            int failed = 0;
            named = 1;
            count = sh_modmanifest_placed_names(json, length, &names, &failed);
            if (failed) { names = NULL; count = 0; }
        }
        placed = mm_row_placed(r, names, count);
        if (sub(r, to, sizeof to, placed, why, sizeof why) == 1 && sh_modmanifest_valid_hash(to, strlen(to)) && strcmp(to, r->build)) {
            AcquireSRWLockExclusive(&g_current_lock);
            if (g_nsubst < MM_SUBST_MAX) {
                strcpy_s(g_subst[g_nsubst].from, sizeof g_subst[g_nsubst].from, r->build);
                strcpy_s(g_subst[g_nsubst].to, sizeof g_subst[g_nsubst].to, to);
                g_nsubst++;
            }
            ReleaseSRWLockExclusive(&g_current_lock);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MODS: map: %s: %.16s is not installed; the installed %.16s serves it in the editor and "
                        "the next save records it", r->salt, r->build, to);
        } else if (why[0]) {
            /* Held until the gate decides: for a published map this
             * parse was not recognised as, the sentence is not the cause. */
            AcquireSRWLockExclusive(&g_current_lock);
            if (g_nnotes < MM_NOTES_MAX)
                _snprintf_s(g_notes[g_nnotes++], sizeof g_notes[0], _TRUNCATE, "MODS: map: %s: %s", r->salt, why);
            else g_notes_over++;
            if (placed && !g_refusal[0]) strcpy_s(g_refusal, sizeof g_refusal, why);
            ReleaseSRWLockExclusive(&g_current_lock);
            continue;
        } else continue;
        backend_log(line);
    }
    sh_modmanifest_names_free(names, count);
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

/* "salt@build16, salt@build16, ..." for a log line. */
static void mm_describe(const sh_modmanifest_list *list, char *out, size_t cap)
{
    size_t i, w = 0;
    if (!cap) return;
    out[0] = 0;
    for (i = 0; i < list->count && w + 1 < cap; i++) {
        int n = _snprintf_s(out + w, cap - w, _TRUNCATE, "%s%s@%.16s", i ? ", " : "",
                            list->rows[i].salt, list->rows[i].build);
        if (n < 0) break;
        w += (size_t)n;
    }
}

/* The rows' builds (as the asset rule serves them), then each of `extra`
 * that none of them is: one activation pass for all of them. */
static int mm_activate(const sh_modmanifest_list *rows, char (*extra)[SH_MODMANIFEST_HASH_CAP], int nextra,
                       int *added, int *first)
{
    const char **hashes;
    char (*served)[SH_MODMANIFEST_HASH_CAP];
    size_t i, nrows = rows ? rows->count : 0, n = 0, k;
    int e, active;
    if (added) *added = 0;
    if (first) *first = -1;
    if (nextra < 0) nextra = 0;
    if (!nrows && !nextra) return modserve_activate_bundles(NULL, 0);
    hashes = (const char **)calloc(nrows + (size_t)nextra, sizeof *hashes);
    served = (char (*)[SH_MODMANIFEST_HASH_CAP])calloc(nrows ? nrows : 1, sizeof *served);
    if (!hashes || !served) { free((void *)hashes); free(served); return -1; }
    /* A row is activated through the build that serves it now
     * (sh_modserve_row_build), else the one the editor substitutes, else its
     * own hash. */
    for (i = 0; i < nrows; i++) {
        sh_modserve_bundle_ref ref;
        memset(&ref, 0, sizeof ref);
        if (sh_modserve_row_build(&rows->rows[i], &ref) && strlen(ref.hash) == SH_MOD_HASH_CHARS)
            strcpy_s(served[i], sizeof served[i], ref.hash);
        else if (!sh_modmanifest_substituted(rows->rows[i].build, served[i], sizeof served[i]))
            strcpy_s(served[i], sizeof served[i], rows->rows[i].build);
        hashes[n++] = served[i];
    }
    for (e = 0; e < nextra; e++) {
        int seen = !extra[e][0];
        for (k = 0; k < n && !seen; k++) seen = !_stricmp(hashes[k], extra[e]);
        for (i = 0; i < nrows && !seen; i++) seen = !_stricmp(rows->rows[i].build, extra[e]);
        if (seen) continue;
        hashes[n++] = extra[e];
        if (added) (*added)++;
        if (first && *first < 0) *first = e;
    }
    active = modserve_activate_bundles(hashes, (int)n);
    free((void *)hashes);
    free(served);
    return active;
}

int sh_modmanifest_activate(const sh_modmanifest_list *rows)
{
    return mm_activate(rows, NULL, 0, NULL, NULL);
}

/* The map scope's listing (sh_modmanifest_set_listed). */
static sh_modmanifest_listed_fn volatile g_listed;
void sh_modmanifest_set_listed(sh_modmanifest_listed_fn listed) { g_listed = listed; }

/* A load's activation: its rows and every build its listing declares
 * beyond them, which is the placed names' closure the scope hook just
 * listed (a rawmap opened without rows places mods no row names). A My Maps load of the same map, saved, has a row for each. */
static int mm_activate_load(const sh_modmanifest_list *rows)
{
    sh_modmanifest_listed_fn listed = g_listed;
    char (*extra)[SH_MODMANIFEST_HASH_CAP] = NULL;
    char line[256];
    int n = listed ? listed(NULL, 0) : 0, added = 0, first = -1, active;
    if (n > 0 && (extra = (char (*)[SH_MODMANIFEST_HASH_CAP])calloc((size_t)n, sizeof *extra)) != NULL) {
        int got = listed(extra, n);
        n = got < 0 ? 0 : got < n ? got : n;
    } else n = 0;
    active = mm_activate(rows, extra, n, &added, &first);
    if (added) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: map: %d bundle(s) its entities place without a row were activated with its rows (first %.16s)",
            added, first >= 0 ? extra[first] : "");
        backend_log(line);
    }
    free(extra);
    return active;
}

static char *mm_prepare_rows(const char *json, size_t length, size_t *out_length);

/* 1 when the text carries rows: the member's "mods" part. */
static int mm_carries_rows(const char *json, size_t length)
{
    if (!json || !length) return 0;
    return sh_carrier_part(json, length, "mods", NULL, NULL) == 1;
}

/* The open map's state that the last load replaced: its rows, registry
 * choices and substitutions. A launch the gate refuses
 * (sh_modmanifest_current_restore_previous) puts it back, so the editor's
 * map keeps its rows when the map whose parse replaced them never ran. */
static struct {
    int valid;
    sh_modmanifest_list rows;
    char choices[SH_MODMANIFEST_CHOICES_CAP];
    mm_subst subst[MM_SUBST_MAX];
    int nsubst;
} g_prev;

static void mm_prev_take(void)
{
    sh_modmanifest_list old;
    size_t i;
    AcquireSRWLockExclusive(&g_current_lock);
    old = g_prev.rows;
    memset(&g_prev.rows, 0, sizeof g_prev.rows);
    for (i = 0; i < g_current.count; i++)
        if (!sh_modmanifest_list_add(&g_prev.rows, &g_current.rows[i])) break;
    memcpy(g_prev.choices, g_choices, sizeof g_prev.choices);
    memcpy(g_prev.subst, g_subst, sizeof g_prev.subst);
    g_prev.nsubst = g_nsubst;
    g_prev.valid = 1;
    ReleaseSRWLockExclusive(&g_current_lock);
    sh_modmanifest_list_free(&old);
}

int sh_modmanifest_current_restore_previous(void)
{
    sh_modmanifest_list rows;
    int n;
    AcquireSRWLockExclusive(&g_current_lock);
    if (!g_prev.valid) { ReleaseSRWLockExclusive(&g_current_lock); return -1; }
    rows = g_prev.rows;
    memset(&g_prev.rows, 0, sizeof g_prev.rows);
    memcpy(g_choices, g_prev.choices, sizeof g_choices);
    memcpy(g_subst, g_prev.subst, sizeof g_subst);
    g_nsubst = g_prev.nsubst;
    g_prev.valid = 0;
    ReleaseSRWLockExclusive(&g_current_lock);
    n = (int)rows.count;
    mm_set_current(&rows);
    sh_modmanifest_list_free(&rows);
    (void)mm_choices_push(0);
    return n;
}

char *sh_modmanifest_prepare_load(const char *json, size_t length, size_t *out_length)
{
    mm_prev_take();
    return mm_prepare_rows(json, length, out_length);
}

void sh_modmanifest_load_refuse(const char *sentence)
{
    char line[760];
    if (!sentence || !sentence[0]) return;
    AcquireSRWLockExclusive(&g_current_lock);
    _snprintf_s(g_notes[0], sizeof g_notes[0], _TRUNCATE, "MODS: map: %s", sentence);
    g_nnotes = 1;
    g_notes_over = 0;
    strncpy_s(g_refusal, sizeof g_refusal, sentence, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_current_lock);
    _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: map refused before parse: %s", sentence);
    backend_log(line);
}

static char *mm_prepare_rows(const char *json, size_t length, size_t *out_length)
{
    sh_modmanifest_list list = {0};
    unsigned rejected = 0;
    int active;
    char line[512], names[320];

    if (out_length) *out_length = 0;
    if (!mm_carries_rows(json, length)) {
        mm_set_current(NULL);
        mm_subst_load(NULL, NULL, 0);
        mm_choices_load(NULL);
        mm_scope_load(json, length, NULL);
        (void)mm_activate_load(NULL);
        return NULL;
    }
    sh_modmanifest_read(json, length, &list, &rejected);

    mm_describe(&list, names, sizeof names);
    /* The map scope first: the bundles' shadows and requirements are live
     * before their gameplay decls register; the registry choices before the
     * listing that computes the registries. */
    mm_subst_load(&list, json, length);
    mm_choices_load(&list);
    mm_scope_load(json, length, &list);
    active = mm_activate_load(&list);
    /* `active` counts the registration steps the activation found ready (new
     * names; a bundle of shadows and requirements has none), not bundles. */
    {
        char what[64];
        if (active < 0) strcpy_s(what, sizeof what, "activation refused (see above)");
        else if (!active) strcpy_s(what, sizeof what, "no new decls to register");
        else _snprintf_s(what, sizeof what, _TRUNCATE, "%d registration step(s) ready", active);
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: map manifest names %zu build(s)%s%s; %s",
            list.count, list.count ? ": " : "", names, what);
    }
    backend_log(line);
    if (rejected) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: %u manifest row(s) did not parse and were dropped", rejected);
        backend_log(line);
    }
    mm_set_current(&list);
    sh_modmanifest_list_free(&list);
    /* Nothing to strip: the member goes with the parse detour's last step. */
    return NULL;
}

typedef struct mm_save_ctx { sh_modmanifest_list list; int failed; } mm_save_ctx;

static void mm_save_visit(void *context, const char *bundle_id, const char *bundle_hash, unsigned long long size,
                          const char *name)
{
    mm_save_ctx *ctx = (mm_save_ctx *)context;
    sh_modmanifest_row *row;
    if (!ctx || ctx->failed || !bundle_id || !bundle_hash) return;
    if (!mm_salt_ok(bundle_id) ||
        !sh_modmanifest_valid_hash(bundle_hash, strlen(bundle_hash))) {
        ctx->failed = 1;
        return;
    }
    row = mm_list_find(&ctx->list, bundle_id, bundle_hash);
    if (!row) {
        sh_modmanifest_row fresh;
        memset(&fresh, 0, sizeof fresh);
        strcpy_s(fresh.salt, sizeof fresh.salt, bundle_id);
        strcpy_s(fresh.build, sizeof fresh.build, bundle_hash);
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
    int c = strcmp(x->salt, y->salt);
    return c ? c : strcmp(x->build, y->build);
}

static int mm_cstr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The placed closure's rows take their display name from the
 * build that serves them, which the closure visit does not give. */
static void mm_save_names(mm_save_ctx *ctx)
{
    sh_modmanifest_resolve_fn resolve = g_resolve;
    size_t i;
    for (i = 0; resolve && i < ctx->list.count; i++) {
        sh_modmanifest_row *r = &ctx->list.rows[i], served;
        memset(&served, 0, sizeof served);
        if (!r->name[0] && resolve(r->build, r->salt, &served) == 1 && !strcmp(served.build, r->build) &&
            !strcmp(served.salt, r->salt)) {
            strcpy_s(r->name, sizeof r->name, served.name);
        }
        sh_modmanifest_row_free(&served);
    }
}

/* Add the map-level rows to the save's rows: a build the placed closure
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
        if (resolve && resolve(row.build, row.salt, &served) == 1 && !strcmp(served.salt, row.salt) &&
            sh_modmanifest_valid_hash(served.build, strlen(served.build))) {
            mm_current_rename(row.build, &served);
            strcpy_s(row.name, sizeof row.name, served.name);
            strcpy_s(row.build, sizeof row.build, served.build);
            row.size = served.size;
        }
        sh_modmanifest_row_free(&served);
        row.attached = 1;
        have = mm_list_find(&ctx->list, row.salt, row.build);
        if (have) { have->attached = 1; continue; }
        if (!sh_modmanifest_list_add(&ctx->list, &row)) return 0;
    }
    return 1;
}

char *sh_modmanifest_on_save(const char *json, size_t length, size_t *out_length)
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
    stale = mm_carries_rows(json, length);
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
    if (!ctx.failed) mm_save_names(&ctx);
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
    for (i = 0; i < ctx.list.count; i++)
        if (ctx.list.rows[i].name_count > 1)
            qsort(ctx.list.rows[i].names, ctx.list.rows[i].name_count, sizeof(char *), mm_cstr_cmp);
    out = sh_modmanifest_write(json, length, &ctx.list, &written, err, sizeof err);
    mm_describe(&ctx.list, described, sizeof described);
    if (out) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "MODS: save manifest wrote %zu row(s) into the snapmap-plus member%s%s%s; %zu -> %zu bytes",
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
