/* Map manifest rows: read, write, and the save-time closure. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modmanifest.h"
#include "modregistry.h"   /* SH_MR_PIN: a ported demon's pinned type */
#include "sha256.h"
#include "modserve.h"
#include "modsalt.h"
#include "map_carrier.h"
#include "config_json.h"
#include "backend_log.h"
#include "../common/text.h"

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
    free(row->name);
    free(row->choices);
    free(row->modules);
    if (row->roots) for (i = 0; i < row->unit_count; i++) free(row->roots[i]);
    free(row->roots);
    free(row->units);
    row->names = NULL;
    row->name_count = 0;
    row->name = NULL;
    row->choices = NULL;
    row->modules = NULL;
    row->roots = NULL;
    row->units = NULL;
    row->unit_count = 0;
    row->pinned = 0;
}

static int mm_hash_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

int sh_modmanifest_composition_hash(const char *salt, const char *const *units, size_t count,
                                    char out[SH_MODMANIFEST_HASH_CAP])
{
    const char **sorted;
    sh_sha256 s;
    size_t i;
    if (!salt || !out || (count && !units)) return 0;
    if (count && !(sorted = (const char **)malloc(count * sizeof *sorted))) return 0;
    else if (!count) sorted = NULL;
    for (i = 0; i < count; i++) sorted[i] = units[i];
    if (count) qsort((void *)sorted, count, sizeof *sorted, mm_hash_cmp);
    sh_sha256_init(&s);
    sh_sha256_update(&s, "snapmap-plus/composition/1\n1\n", 29);
    sh_sha256_update(&s, salt, strlen(salt));
    sh_sha256_update(&s, "\n", 1);
    for (i = 0; i < count; i++) {
        if (i && !strcmp(sorted[i], sorted[i - 1])) continue;
        sh_sha256_update(&s, sorted[i], strlen(sorted[i]));
        sh_sha256_update(&s, "\n", 1);
    }
    sh_sha256_hex(&s, out, SH_MOD_HASH_CHARS);
    free((void *)sorted);
    return 1;
}

int sh_modmanifest_row_pin(sh_modmanifest_row *row, const char *const *roots, const char *const *units, size_t n)
{
    char (*u)[SH_MOD_HASH_CHARS + 1] = NULL, **r = NULL, chash[SH_MODMANIFEST_HASH_CAP];
    size_t i;
    if (!row || (n && (!roots || !units))) return 0;
    if (n && (!(u = calloc(n, sizeof *u)) || !(r = (char **)calloc(n, sizeof *r)))) { free(u); free(r); return 0; }
    for (i = 0; i < n; i++) {
        if (!units[i] || !sh_modmanifest_valid_hash(units[i], strlen(units[i])) || !roots[i] || !roots[i][0] ||
            !(r[i] = sh_text(roots[i]))) {
            while (i) free(r[--i]);
            free(r);
            free(u);
            return 0;
        }
        strcpy_s(u[i], sizeof u[i], units[i]);
    }
    if (!sh_modmanifest_composition_hash(row->salt, units, n, chash)) {
        for (i = 0; i < n; i++) free(r[i]);
        free(r);
        free(u);
        return 0;
    }
    if (row->roots) for (i = 0; i < row->unit_count; i++) free(row->roots[i]);
    free(row->roots);
    free(row->units);
    row->roots = r;
    row->units = u;
    row->unit_count = n;
    row->pinned = 1;
    strcpy_s(row->build, sizeof row->build, chash);
    return 1;
}

const char *sh_modmanifest_row_name(const sh_modmanifest_row *row)
{
    return row && row->name ? row->name : "";
}

int sh_modmanifest_row_set_name(sh_modmanifest_row *row, const char *name)
{
    char *copy = NULL;
    if (!row) return 0;
    if (name && name[0] && !(copy = sh_text(name))) return 0;
    free(row->name);
    row->name = copy;
    return 1;
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
    slot->name = NULL;
    slot->choices = NULL;
    slot->modules = NULL;
    slot->roots = NULL;
    slot->units = NULL;
    slot->unit_count = 0;
    slot->pinned = 0;
    if ((row->name && !(slot->name = sh_text(row->name))) || (row->choices && !(slot->choices = sh_text(row->choices))) ||
        (row->modules && !(slot->modules = sh_text(row->modules)))) {
        sh_modmanifest_row_free(slot);
        return 0;
    }
    if (row->pinned) {
        const char **units = row->unit_count ? (const char **)calloc(row->unit_count, sizeof *units) : NULL;
        int ok = !row->unit_count || units;
        for (i = 0; ok && i < row->unit_count; i++) units[i] = row->units[i];
        ok = ok && sh_modmanifest_row_pin(slot, (const char *const *)row->roots, units, row->unit_count);
        free((void *)units);
        if (!ok) { sh_modmanifest_row_free(slot); return 0; }
    }
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

/* The decoded string member of `object`, whole, in *out (malloc'd; NULL when
 * absent). 0 when it does not decode, or is absent and required. */
static int mm_member_heap(const sh_json_object *object, const char *key, char **out, int required)
{
    const char *raw = sh_json_object_get(object, key);
    *out = NULL;
    if (!raw) return !required;
    *out = mm_decode(raw, strlen(raw));
    return *out != NULL;
}

/* Copy a decoded string member of a fixed-width form (a salt, a hash) into
 * out; a longer value is not that form and does not read. */
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
    if (!n) return 0;
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

/* Append "slot\tkey\n" to *lines (malloc'd, NULL for none) unless the slot
 * is there. 0 when memory runs out. */
static int mm_choice_add(char **lines, const char *slot, const char *key)
{
    if (mm_choice_find(*lines, slot, NULL, 0)) return 1;
    return sh_text_set(lines, "%s%s\t%s\n", *lines ? *lines : "", slot, key);
}

/* One "slot\tkey\n" line at *p: the slot and key whole (malloc'd), *p past
 * it. 0 at the end or a malformed line. */
static int mm_choice_next(const char **p, char **slot, char **key)
{
    const char *tab, *nl;
    *slot = *key = NULL;
    if (!*p || !**p) return 0;
    tab = strchr(*p, '\t');
    nl = strchr(*p, '\n');
    if (!tab || !nl || tab > nl) return 0;
    *slot = sh_textn(*p, (size_t)(tab - *p));
    *key = sh_textn(tab + 1, (size_t)(nl - tab - 1));
    *p = nl + 1;
    if (!*slot || !*key) { free(*slot); free(*key); *slot = *key = NULL; return 0; }
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

/* A row object of an earlier Snapmap+ shape: an "id" or "bundle" member (before units existed; it names no unit). */
static int mm_earlier_shape(const sh_json_object *object)
{
    const char *mod;
    if (sh_json_object_get(object, "id") || sh_json_object_get(object, "bundle")) return 1;
    /* Format 1 (before units): "mod": "<salt>@<build>"; it names no unit either. */
    mod = sh_json_object_get(object, "mod");
    return mod && mod[0] == '"' && strchr(mod, '@') != NULL;
}

/* The same test on a row's text, for the reader's count of earlier rows. */
static int mm_earlier_row(const char *json, size_t length)
{
    sh_json_object object = {0};
    int earlier = sh_native_json_parse_object(json, length, 8, &object) && mm_earlier_shape(&object);
    sh_json_object_free(&object);
    return earlier;
}

int sh_modmanifest_parse_value(const char *json, size_t length, sh_modmanifest_row *out,
                               char *err, size_t err_cap)
{
    sh_json_object object = {0};
    const char *names;
    int ok = 0;
    if (err && err_cap) err[0] = 0;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    /* Depth 8: a module record (D-A30) nests its portals' bounds five levels below the row. */
    if (!json || !length || !sh_native_json_parse_object(json, length, 8, &object)) {
        MM_ERR("row value is not a JSON object");
        return 0;
    }
    /* An "id" or "bundle" row, or format 1's "<salt>@<build>" (Q4): the installer converts them. */
    if (mm_earlier_shape(&object)) {
        MM_ERR("the map was saved by an earlier Snapmap+; which this version does not read yet");
        goto done;
    }
    {
        char mod[SH_MODMANIFEST_SALT_CAP + SH_MODMANIFEST_HASH_CAP + 1];
        size_t ml;
        if (!mm_member_text(&object, "mod", mod, sizeof mod, 1)) { MM_ERR("row has no valid mod"); goto done; }
        ml = strlen(mod);
        if (ml == SH_MODMANIFEST_SALT_LEN) {
            /* A local row follows its folder (D6a): the build it was saved from is not part of it. */
            memcpy(out->salt, mod, SH_MODMANIFEST_SALT_LEN + 1);
            strcpy_s(out->build, sizeof out->build, SH_MODMANIFEST_LOCAL_BUILD);
        } else {
            MM_ERR("row has no valid mod");
            goto done;
        }
        if (!mm_salt_ok(out->salt) || !sh_modmanifest_valid_hash(out->build, strlen(out->build))) {
            MM_ERR("row has no valid mod");
            goto done;
        }
        if (ml == SH_MODMANIFEST_SALT_LEN) {
            /* Format 2: a pinned row names its objects' units (and "whole"); a local row says "local": true. */
            const char *local = sh_json_object_get(&object, "local"), *objects = sh_json_object_get(&object, "objects"),
                       *whole = sh_json_object_get(&object, "whole");
            if (!(local && !strcmp(local, "true"))) {
                sh_json_object oo = {0};
                const char **roots = NULL, **units = NULL;
                char **owned = NULL;
                size_t n = 0, c, k;
                int good = 1;
                if (objects && (*objects != '{' || !sh_native_json_parse_object(objects, strlen(objects), 2, &oo))) good = 0;
                if (good) {
                    size_t cap = oo.count + 1;
                    roots = (const char **)calloc(cap, sizeof *roots);
                    units = (const char **)calloc(cap, sizeof *units);
                    owned = (char **)calloc(cap * 2, sizeof *owned);
                    good = roots && units && owned;
                }
                for (c = 0; good && c < oo.count; c++) {
                    char *root = sh_textn(oo.members[c].key, oo.members[c].key_length);
                    char *unit = mm_decode(oo.members[c].value_json, strlen(oo.members[c].value_json));
                    owned[2 * n] = root;
                    owned[2 * n + 1] = unit;
                    if (!root || !unit || !root[0] || !sh_modmanifest_valid_hash(unit, strlen(unit))) { good = 0; break; }
                    roots[n] = root;
                    units[n] = unit;
                    n++;
                }
                if (good && whole) {
                    char *unit = mm_decode(whole, strlen(whole));
                    owned[2 * n] = NULL;
                    owned[2 * n + 1] = unit;
                    if (!unit || !sh_modmanifest_valid_hash(unit, strlen(unit))) good = 0;
                    else { roots[n] = "*"; units[n] = unit; n++; }
                }
                if (good && !n) good = 0;
                if (good && !sh_modmanifest_row_pin(out, roots, units, n)) good = 0;
                for (k = 0; owned && k < (oo.count + 1) * 2; k++) free(owned[k]);
                free(owned);
                free((void *)roots);
                free((void *)units);
                sh_json_object_free(&oo);
                if (!good) { MM_ERR("row '%s' names its objects' units wrongly", out->salt); goto done; }
                /* What a module stand-in needs (D-A30): kept as written; one that does not read is dropped and the
                 * row stays (an absent module is then left out, as before). */
                {
                    const char *modules = sh_json_object_get(&object, "modules");
                    if (modules && *modules == '[') out->modules = sh_text(modules);
                }
            }
        }
    }
    if (!mm_member_heap(&object, "name", &out->name, 0)) {
        MM_ERR("row '%s' has no valid name", out->salt); goto done;
    }
    if (!mm_member_uint(&object, "size", &out->size)) {
        MM_ERR("row '%s' has no valid size", out->salt); goto done;
    }
    {
        const char *attached = sh_json_object_get(&object, "attached");
        out->attached = attached && !strcmp(attached, "true");
        /* A pinned row used map-wide names "whole" (SPEC 2.7). */
        for (size_t u = 0; out->pinned && u < out->unit_count; u++)
            if (!strcmp(out->roots[u], "*")) out->attached = 1;
    }
    {
        /* Registry choices: entries that do not read are skipped (the row stays). */
        const char *choices = sh_json_object_get(&object, "choices");
        sh_json_object co = {0};
        size_t c;
        if (choices && *choices == '{' && sh_native_json_parse_object(choices, strlen(choices), 2, &co)) {
            for (c = 0; c < co.count; c++) {
                char *key = mm_decode(co.members[c].value_json, strlen(co.members[c].value_json));
                char *slot = key ? sh_textn(co.members[c].key, co.members[c].key_length) : NULL;
                if (slot && mm_slot_ok(slot) && mm_salt_ok(key)) (void)mm_choice_add(&out->choices, slot, key);
                free(slot);
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
            if (*elem != '"') continue;
            name = mm_decode(elem, elem_len);
            if (name) sh_modmanifest_row_add_name(out, name);
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
    /* Format 2 (SPEC-distribution 2.7): the namespace, then a pinned row's units or a local row's placed names. */
    mm_put(&t, "{\"mod\":\"", 8);
    mm_put(&t, row->salt, strlen(row->salt));
    mm_put(&t, "\",\"name\":", 9);
    mm_put_quoted(&t, sh_modmanifest_row_name(row));
    _snprintf_s(number, sizeof number, _TRUNCATE, ",\"size\":%llu", row->size);
    mm_put(&t, number, strlen(number));
    if (row->pinned) {
        size_t objects = 0;
        const char *whole = NULL;
        mm_put(&t, ",\"objects\":{", 12);
        for (i = 0; i < row->unit_count; i++) {
            if (!strcmp(row->roots[i], "*")) { whole = row->units[i]; continue; }
            if (objects++) mm_put(&t, ",", 1);
            mm_put_quoted(&t, row->roots[i]);
            mm_put(&t, ":", 1);
            mm_put_quoted(&t, row->units[i]);
        }
        mm_put(&t, "}", 1);
        if (whole) { mm_put(&t, ",\"whole\":", 9); mm_put_quoted(&t, whole); }
        if (row->modules && row->modules[0] == '[') { mm_put(&t, ",\"modules\":", 11); mm_put(&t, row->modules, strlen(row->modules)); }
    } else {
        mm_put(&t, ",\"local\":true", 13);
        mm_put(&t, ",\"names\":[", 10);
        for (i = 0; i < row->name_count; i++) {
            if (i) mm_put(&t, ",", 1);
            mm_put_quoted(&t, row->names[i]);
        }
        mm_put(&t, "]", 1);
        if (row->attached) mm_put(&t, ",\"attached\":true", 16);
    }
    if (mm_choice_count(row->choices)) {
        char *slot, *key;
        mm_put(&t, ",\"choices\":{", 12);
        for (c = 0, p = row->choices; mm_choice_next(&p, &slot, &key); c++) {
            if (c) mm_put(&t, ",", 1);
            mm_put_quoted(&t, slot);
            mm_put(&t, ":", 1);
            mm_put_quoted(&t, key);
            free(slot);
            free(key);
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

/* The product's own entry (modmanifest.h SH_MODMANIFEST_PRODUCT): what the map uses of Snapmap+ itself, by word. */
static const struct { unsigned bit; const char *word; } MM_USES[] = {
    { SH_MODMANIFEST_USES_MODULE_SIZES, "module-sizes" },
    { SH_MODMANIFEST_USES_NAVIGATION, "navigation" },
};

/* 1 when the element is the product's entry, its words' bits added to *uses. */
static int mm_product_entry(const char *json, size_t length, unsigned *uses)
{
    sh_json_object object = {0};
    char mod[32];
    const char *list;
    size_t i;
    int is = 0;
    if (!sh_native_json_parse_object(json, length, 2, &object)) return 0;
    if (mm_member_text(&object, "mod", mod, sizeof mod, 1) && !strcmp(mod, SH_MODMANIFEST_PRODUCT)) {
        is = 1;
        if ((list = sh_json_object_get(&object, "uses")) != NULL && *list == '[') {
            for (i = 0; i < sizeof MM_USES / sizeof MM_USES[0]; i++) {
                char quoted[48];
                _snprintf_s(quoted, sizeof quoted, _TRUNCATE, "\"%s\"", MM_USES[i].word);
                if (strstr(list, quoted)) *uses |= MM_USES[i].bit;
            }
        }
    }
    sh_json_object_free(&object);
    return is;
}

/* The member's "mods" array [from, to): one row object per element, and the product's entry. */
static size_t mm_read_member(const char *from, const char *to, sh_modmanifest_list *out, unsigned *bad)
{
    const char *cursor = from, *elem;
    size_t elem_len, read = 0;
    while (sh_carrier_array_next(&cursor, to, &elem, &elem_len) == 1) {
        sh_modmanifest_row row;
        if (*elem == '{' && mm_product_entry(elem, elem_len, &out->uses)) continue;
        if (*elem != '{' || !sh_modmanifest_parse_value(elem, elem_len, &row, NULL, 0)) {
            (*bad)++;
            if (*elem == '{' && mm_earlier_row(elem, elem_len)) out->earlier++;
            continue;
        }
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
    size_t src_len = length, i, count = rows ? rows->count + (rows->uses ? 1 : 0) : 0, w = 0;
    mm_text value = {0};
    if (out_length) *out_length = 0;
    if (err && err_cap) err[0] = 0;
    if (!json || !length) { MM_ERR("no map text"); return NULL; }
    if (count) {
        mm_put(&value, "[", 1);
        for (i = 0; i < rows->count; i++) {
            size_t vlen = 0;
            char *row = sh_modmanifest_format_value(&rows->rows[i], &vlen);
            if (!row) { MM_ERR("The mod %s could not be recorded in the map; the map was saved without its mod list", rows->rows[i].salt); goto done; }
            if (i) mm_put(&value, ",", 1);
            mm_put(&value, row, vlen);
            free(row);
        }
        if (rows->uses) {
            size_t k, words = 0;
            if (rows->count) mm_put(&value, ",", 1);
            static const char head[] = "{\"mod\":\"" SH_MODMANIFEST_PRODUCT "\",\"name\":\"Snapmap+\",\"uses\":[";
            mm_put(&value, head, sizeof head - 1);
            for (k = 0; k < sizeof MM_USES / sizeof MM_USES[0]; k++) {
                if (!(rows->uses & MM_USES[k].bit)) continue;
                if (words++) mm_put(&value, ",", 1);
                mm_put_quoted(&value, MM_USES[k].word);
            }
            mm_put(&value, "]}", 2);
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

static void mm_names_add(mm_names *s, const char *lit, size_t lit_len)
{
    char *name;
    size_t body = lit_len - 2;
    if (s->failed || body == 0) return;
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
            char *name = NULL;
            if (mm_member_heap(&provides, "name", &name, 1) && name[0] && !sh_modmanifest_row_add_name(out, name)) r = -1;
            free(name);
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
        /* The name as the builder reads it: trimmed of ASCII whitespace, whole. */
        char *name = NULL;
        size_t a = 0, n;
        if (!mm_member_heap(&object, "name", &name, 1)) { free(name); name = NULL; }
        n = name ? strlen(name) : 0;
        while (a < n && mm_space(name[a])) a++;
        while (n > a && mm_space(name[n - 1])) n--;
        if (n == a) {
            free(name);
            MM_ERR("mod.json has no name; give the mod a name and press Refresh");
            goto done;
        }
        out->name = sh_textn(name + a, n - a);
        free(name);
        if (!out->name) { MM_ERR("Out of memory while reading mod.json"); goto done; }
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

/* The open map's registry choices ("slot\tkey\n" lines, malloc'd, NULL for
 * none), under g_current_lock. */
static char *g_choices;

/* Give the choices to the serving index (its choice input of the listing). */
static int mm_choices_push(int recompute)
{
    char *copy, *p;
    const char **slots = NULL, **keys = NULL;
    size_t lines;
    int n = 0, r;
    AcquireSRWLockShared(&g_current_lock);
    copy = g_choices ? sh_text(g_choices) : NULL;
    ReleaseSRWLockShared(&g_current_lock);
    lines = mm_choice_count(copy);
    if (lines && (!(slots = (const char **)calloc(lines, sizeof *slots)) || !(keys = (const char **)calloc(lines, sizeof *keys)))) {
        free((void *)slots);
        free(copy);
        return -1;
    }
    for (p = copy; p && *p && (size_t)n < lines;) {
        char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        if (!tab || !nl || tab > nl) break;
        *tab = 0;
        *nl = 0;
        slots[n] = p;
        keys[n++] = tab + 1;
        p = nl + 1;
    }
    r = sh_modserve_scope_set_choices(slots, keys, n, recompute);
    free((void *)slots);
    free((void *)keys);
    free(copy);
    return r;
}

size_t sh_modmanifest_rows_choices(const sh_modmanifest_list *rows, char **out)
{
    size_t i;
    if (!out) return 0;
    *out = NULL;
    for (i = 0; rows && i < rows->count; i++) {
        const char *p = rows->rows[i].choices;
        char *slot, *key;
        while (mm_choice_next(&p, &slot, &key)) {
            (void)mm_choice_add(out, slot, key);
            free(slot);
            free(key);
        }
    }
    return mm_choice_count(*out);
}

int sh_modmanifest_current_choose(const char *slot, const char *key)
{
    char *next = NULL, *one, *k;
    const char *p;
    int ok = 1;
    if (!mm_slot_ok(slot) || (key && key[0] && !mm_salt_ok(key))) return -1;
    AcquireSRWLockExclusive(&g_current_lock);
    /* Every other slot as it was, then this one (the order of choosing). */
    for (p = g_choices; ok && mm_choice_next(&p, &one, &k);) {
        if (strcmp(one, slot)) ok = mm_choice_add(&next, one, k);
        free(one);
        free(k);
    }
    if (ok && key && key[0]) ok = mm_choice_add(&next, slot, key);
    if (ok) { free(g_choices); g_choices = next; }
    else free(next);
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

size_t sh_modmanifest_current_choices(char **out)
{
    size_t n;
    if (!out) return 0;
    AcquireSRWLockShared(&g_current_lock);
    *out = sh_text(g_choices ? g_choices : "");
    n = mm_choice_count(g_choices);
    ReleaseSRWLockShared(&g_current_lock);
    return *out ? n : 0;
}

/* The choices of a loaded map, set without a recompute: the listing that
 * follows (the scope hook) computes the registries with them. */
static void mm_choices_load(const sh_modmanifest_list *rows)
{
    char *lines = NULL, *old;
    (void)sh_modmanifest_rows_choices(rows, &lines);
    AcquireSRWLockExclusive(&g_current_lock);
    old = g_choices;
    g_choices = lines;
    ReleaseSRWLockExclusive(&g_current_lock);
    free(old);
    (void)mm_choices_push(0);
}

/* A ported demon's type the listing gave, pinned on the row of its mod (modregistry.h SH_MR_PIN): a row the map
 * does not have pins nothing (its demon is not in the map). */
static void mm_pin_one(void *context, const char *salt, const char *slot)
{
    sh_modmanifest_list *rows = (sh_modmanifest_list *)context;
    size_t i;
    for (i = 0; rows && salt && slot && i < rows->count; i++)
        if (!strcmp(rows->rows[i].salt, salt)) { (void)mm_choice_add(&rows->rows[i].choices, slot, salt); return; }
}

static int mm_local_absent(const sh_modmanifest_row *r);

/* Whether a pinned row's composition is on disk whole (modcache.h sh_cache_composed), bound by the product; unbound
 * (the store tool, tests that do not bind it) a pinned row counts as whole. */
static int (*g_whole)(const char *build);
void sh_modmanifest_set_whole(int (*fn)(const char *build)) { g_whole = fn; }

/* 1 when the listing cannot say which types a row's ported demons have: a local row whose folder no local root serves
 * (D6a: it stands in), or a pinned row whose units are not all here (D60). Its recorded pins are then kept. */
static int mm_row_cannot_pin(const sh_modmanifest_row *r)
{
    if (r->pinned) return g_whole && !g_whole(r->build);
    return mm_local_absent(r);
}

/* The encounter a pin names (SH_MR_PIN "<bit>/<encounter>"), NULL for another slot. */
static const char *mm_pin_encounter(const char *slot)
{
    size_t pl = strlen(SH_MR_PIN);
    const char *e;
    if (!slot || _strnicmp(slot, SH_MR_PIN, pl)) return NULL;
    e = strchr(slot + pl, '/');
    return e ? e + 1 : NULL;
}

/* 1 when a row's choices already pin `encounter` (the listing gave it a type). */
static int mm_row_pins(const sh_modmanifest_row *r, const char *encounter)
{
    const char *p = r->choices;
    char *slot, *key;
    int found = 0;
    while (!found && mm_choice_next(&p, &slot, &key)) {
        const char *e = mm_pin_encounter(slot);
        found = e && !_stricmp(e, encounter);
        free(slot);
        free(key);
    }
    return found;
}

/* At a save: each choice on the row of the salt it names, and every ported demon's type of the listing pinned on
 * its mod's row (the choices' earlier pins give way to the types the map has now). A row whose content is not here
 * keeps the pins it was loaded with, as it keeps its module records (D60a): the listing served its stand-ins, not its
 * demons, so it cannot say their types, and a pin dropped here would give the type to another port on the next load
 * (R-LB1, R-LV8 L-F2..F4). A demon the listing pins wins over the recorded pin. */
static void mm_choices_save(sh_modmanifest_list *rows)
{
    char *lines, *slot, *key;
    const char *p;
    size_t i;
    unsigned char *keep = rows->count ? (unsigned char *)calloc(rows->count, 1) : NULL;
    AcquireSRWLockShared(&g_current_lock);
    lines = g_choices ? sh_text(g_choices) : NULL;
    ReleaseSRWLockShared(&g_current_lock);
    for (i = 0; i < rows->count; i++) {
        free(rows->rows[i].choices);
        rows->rows[i].choices = NULL;
        if (keep) keep[i] = (unsigned char)mm_row_cannot_pin(&rows->rows[i]);
    }
    (void)sh_modserve_spawn_pins(mm_pin_one, rows);
    for (p = lines; mm_choice_next(&p, &slot, &key);) {
        int placed = 0;
        const char *encounter = mm_pin_encounter(slot);
        if (encounter) {
            for (i = 0; i < rows->count && !placed; i++) {
                if (strcmp(rows->rows[i].salt, key) || !keep || !keep[i]) continue;
                placed = 1;   /* this row's pins are decided here, kept or given way to */
                if (!mm_row_pins(&rows->rows[i], encounter) && mm_choice_add(&rows->rows[i].choices, slot, key)) {
                    char *line = sh_textf("MODS: save manifest: %s keeps the pin %s: its content is not here to say its type",
                                          key, slot);
                    backend_log(line ? line : "MODS: save manifest: a row whose content is not here keeps its pin");
                    free(line);
                }
            }
            free(slot);
            free(key);
            continue;
        }
        for (i = 0; i < rows->count && !placed; i++)
            if (!strcmp(rows->rows[i].salt, key))
                placed = mm_choice_add(&rows->rows[i].choices, slot, key);
        if (!placed) {
            char *line = sh_textf("MODS: save manifest: the choice %s -> %s is not saved: the map declares no build of %s",
                                  slot, key, key);
            backend_log(line ? line : "MODS: save manifest: a choice is not saved: the map declares no build of its mod");
            free(line);
        }
        free(slot);
        free(key);
    }
    free(lines);
    free(keep);
}

void sh_modmanifest_reset(void)
{
    mm_set_current(NULL);
    AcquireSRWLockExclusive(&g_current_lock);
    free(g_choices);
    g_choices = NULL;
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

int sh_modmanifest_current_add_row(const sh_modmanifest_row *row)
{
    sh_modmanifest_row *have;
    size_t k;
    int r = 1;
    if (!row || !mm_salt_ok(row->salt)) return -1;
    AcquireSRWLockExclusive(&g_current_lock);
    have = mm_list_find(&g_current, row->salt, row->build);
    if (have) {
        for (k = 0; k < row->name_count && r > 0; k++) if (!sh_modmanifest_row_add_name(have, row->names[k])) r = -1;
    } else if (!sh_modmanifest_list_add(&g_current, row)) r = -1;
    ReleaseSRWLockExclusive(&g_current_lock);
    return r;
}

int sh_modmanifest_current_make_local(const char *salt, const char *build, const char *const *names, size_t n)
{
    size_t i, k;
    int r = 0;
    if (!salt || !mm_salt_ok(salt) || !build || !sh_modmanifest_valid_hash(build, strlen(build))) return -1;
    AcquireSRWLockExclusive(&g_current_lock);
    for (i = 0; i < g_current.count; i++) {
        sh_modmanifest_row *row = &g_current.rows[i];
        if (strcmp(row->salt, salt) || !row->pinned) continue;
        /* the pinned row becomes the local row of the installed build: its placed names, no unit */
        if (row->roots) for (k = 0; k < row->unit_count; k++) free(row->roots[k]);
        free(row->roots);
        free(row->units);
        free(row->modules);
        row->roots = NULL;
        row->units = NULL;
        row->modules = NULL;
        row->unit_count = 0;
        row->pinned = 0;
        strcpy_s(row->build, sizeof row->build, build);
        for (k = 0; k < n; k++)
            if (!sh_modmanifest_row_add_name(row, names[k])) { r = -1; break; }
        if (r == 0) r = 1;
        break;
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
            (void)sh_modmanifest_row_set_name(r, to->name);
            strcpy_s(r->build, sizeof r->build, to->build);
            r->size = to->size;
            break;
        }
    ReleaseSRWLockExclusive(&g_current_lock);
}

/* Substitutions of the open map (sh_modmanifest_substituted), under g_current_lock. */
typedef struct mm_subst { char from[SH_MODMANIFEST_HASH_CAP], to[SH_MODMANIFEST_HASH_CAP]; } mm_subst;
static mm_subst *g_subst;   /* malloc'd; g_nsubst used of g_subst_cap */
static int g_nsubst, g_subst_cap;
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

/* The held cause sentences of the last editor load (sh_modmanifest_load_notes_flush):
 * malloc'd lines, g_nnotes used of g_notes_cap. */
static char **g_notes;
static int g_nnotes, g_notes_cap;
static char *g_refusal;   /* the first note whose row the placed entities name (malloc'd), or NULL */

/* Forget the held notes and the refusal (under g_current_lock, exclusive). */
static void mm_notes_clear(void)
{
    int i;
    for (i = 0; i < g_nnotes; i++) free(g_notes[i]);
    g_nnotes = 0;
    free(g_refusal);
    g_refusal = NULL;
}

/* Hold one note line, taking `line` (under g_current_lock, exclusive). */
static void mm_note_hold(char *line)
{
    if (!line) return;
    if (g_nnotes == g_notes_cap) {
        int next = g_notes_cap ? g_notes_cap * 2 : 8;
        char **grown = (char **)realloc(g_notes, (size_t)next * sizeof *grown);
        if (!grown) { free(line); return; }
        g_notes = grown;
        g_notes_cap = next;
    }
    g_notes[g_nnotes++] = line;
}

int sh_modmanifest_load_refusal(char **out)
{
    int have;
    if (out) *out = NULL;
    AcquireSRWLockShared(&g_current_lock);
    have = g_refusal && g_refusal[0];
    if (have && out) *out = sh_text(g_refusal);
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
    char **lines;
    int n, i;
    AcquireSRWLockExclusive(&g_current_lock);
    n = g_nnotes;
    lines = g_notes;
    g_notes = NULL;
    g_nnotes = g_notes_cap = 0;
    free(g_refusal);
    g_refusal = NULL;
    ReleaseSRWLockExclusive(&g_current_lock);
    for (i = 0; i < n; i++) {
        if (say) backend_log(lines[i]);
        free(lines[i]);
    }
    free(lines);
    return n;
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
    mm_notes_clear();
    ReleaseSRWLockExclusive(&g_current_lock);
    if (!list || !editor || !sub) return;
    for (i = 0; i < list->count; i++) {
        const sh_modmanifest_row *r = &list->rows[i];
        sh_modserve_bundle_ref ref;
        char to[SH_MODMANIFEST_HASH_CAP] = "", *why = NULL, *line;
        int placed;
        memset(&ref, 0, sizeof ref);
        placed = sh_modserve_row_build(r, &ref);
        sh_modserve_bundle_ref_free(&ref);
        if (placed) continue;   /* a place or the root serves it */
        /* The placed names, read once, for the rows nothing serves. */
        if (!named && json && length) {
            int failed = 0;
            named = 1;
            count = sh_modmanifest_placed_names(json, length, &names, &failed);
            if (failed) { names = NULL; count = 0; }
        }
        placed = mm_row_placed(r, names, count);
        if (sub(r, to, sizeof to, placed, &why) == 1 && sh_modmanifest_valid_hash(to, strlen(to)) && strcmp(to, r->build)) {
            AcquireSRWLockExclusive(&g_current_lock);
            if (g_nsubst == g_subst_cap) {
                int next = g_subst_cap ? g_subst_cap * 2 : 8;
                mm_subst *grown = (mm_subst *)realloc(g_subst, (size_t)next * sizeof *grown);
                if (grown) { g_subst = grown; g_subst_cap = next; }
            }
            if (g_nsubst < g_subst_cap) {
                strcpy_s(g_subst[g_nsubst].from, sizeof g_subst[g_nsubst].from, r->build);
                strcpy_s(g_subst[g_nsubst].to, sizeof g_subst[g_nsubst].to, to);
                g_nsubst++;
            }
            ReleaseSRWLockExclusive(&g_current_lock);
            line = sh_textf("MODS: map: %s: %.16s is not installed; the installed %.16s serves it in the editor and "
                            "the next save records it", r->salt, r->build, to);
            backend_log(line ? line : "MODS: map: an installed build serves a declared one in the editor");
            free(line);
        } else if (why && why[0]) {
            /* Held until the gate decides: for a published map this
             * parse was not recognised as, the sentence is not the cause. */
            AcquireSRWLockExclusive(&g_current_lock);
            mm_note_hold(sh_textf("MODS: map: %s: %s", r->salt, why));
            if (placed && !g_refusal) g_refusal = sh_text(why);
            ReleaseSRWLockExclusive(&g_current_lock);
        }
        free(why);
    }
    sh_modmanifest_names_free(names, count);
}

/* The map scope's hook and its arming (sh_modmanifest_set_scope). */
static sh_modmanifest_scope_fn volatile g_scope;
static int (*volatile g_scope_armed)(void);
static sh_modmanifest_resolve_fn volatile g_resolve;

static sh_modmanifest_pin_fn g_pin;
void sh_modmanifest_set_pin(sh_modmanifest_pin_fn pin) { g_pin = pin; }

void sh_modmanifest_set_scope(sh_modmanifest_scope_fn scope, int (*armed)(void))
{
    g_scope = scope;
    g_scope_armed = armed;
}

void sh_modmanifest_set_resolver(sh_modmanifest_resolve_fn resolve) { g_resolve = resolve; }
static sh_modmanifest_content_fn volatile g_content;
void sh_modmanifest_set_content(sh_modmanifest_content_fn fn) { g_content = fn; }
static sh_modmanifest_uses_fn volatile g_uses;
void sh_modmanifest_set_uses(sh_modmanifest_uses_fn fn) { g_uses = fn; }


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

/* "salt@build16, salt@build16, ..." for a log line (malloc'd; every row). */
static char *mm_describe(const sh_modmanifest_list *list)
{
    mm_text t = {0};
    size_t i;
    char one[SH_MODMANIFEST_SALT_CAP + 20];
    mm_put(&t, "", 0);
    for (i = 0; i < list->count; i++) {
        _snprintf_s(one, sizeof one, _TRUNCATE, "%s%s@%.16s", i ? ", " : "", list->rows[i].salt, list->rows[i].build);
        mm_put(&t, one, strlen(one));
    }
    if (t.failed) { free(t.p); return NULL; }
    return t.p;
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
        sh_modserve_bundle_ref_free(&ref);
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
    char *choices;      /* malloc'd, or NULL */
    mm_subst *subst;    /* malloc'd, nsubst of them */
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
    free(g_prev.choices);
    g_prev.choices = g_choices ? sh_text(g_choices) : NULL;
    free(g_prev.subst);
    g_prev.subst = NULL;
    g_prev.nsubst = 0;
    if (g_nsubst && (g_prev.subst = (mm_subst *)malloc((size_t)g_nsubst * sizeof *g_prev.subst)) != NULL) {
        memcpy(g_prev.subst, g_subst, (size_t)g_nsubst * sizeof *g_prev.subst);
        g_prev.nsubst = g_nsubst;
    }
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
    free(g_choices);
    g_choices = g_prev.choices;
    g_prev.choices = NULL;
    free(g_subst);
    g_subst = g_prev.subst;
    g_nsubst = g_subst_cap = g_prev.nsubst;
    g_prev.subst = NULL;
    g_prev.nsubst = 0;
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
    char *line;
    if (!sentence || !sentence[0]) return;
    AcquireSRWLockExclusive(&g_current_lock);
    mm_notes_clear();
    mm_note_hold(sh_textf("MODS: map: %s", sentence));
    g_refusal = sh_text(sentence);
    ReleaseSRWLockExclusive(&g_current_lock);
    line = sh_textf("MODS: map refused before parse: %s", sentence);
    backend_log(line ? line : "MODS: map refused before parse");
    free(line);
}

/* The map text of the content step running on this thread (sh_modmanifest_content_text). */
static __declspec(thread) const char *t_content_text;
static __declspec(thread) size_t t_content_length;

const char *sh_modmanifest_content_text(size_t *length)
{
    if (length) *length = t_content_text ? t_content_length : 0;
    return t_content_text;
}

/* 1 when `r` is a local row whose namespace no local folder serves (D6a): nothing at the root serves it (its stand-in
 * composition from an earlier load counts as nothing). */
static int mm_local_absent(const sh_modmanifest_row *r)
{
    sh_modserve_bundle_ref ref;
    int here;
    if (r->pinned || !strcmp(r->salt, SH_MODMANIFEST_PRODUCT)) return 0;
    memset(&ref, 0, sizeof ref);
    here = sh_modserve_row_build(r, &ref) && ref.place && !ref.place[0];
    sh_modserve_bundle_ref_free(&ref);
    return !here;
}

void sh_modmanifest_content_for(const char *json, size_t length, const sh_modmanifest_list *rows)
{
    sh_modmanifest_content_fn content = g_content;
    sh_modmanifest_list rows_here;
    size_t i, placed_count = 0;
    int failed = 0;
    char **placed = NULL;
    if (!content || !rows) return;
    /* The pinned rows, and each local row whose folder is not here as a row of no units under the composition hash
     * of none (manager Q8, D6a): the cache composes its stand-ins (D60) and says the one sentence. */
    memset(&rows_here, 0, sizeof rows_here);
    for (i = 0; i < rows->count; i++) {
        const sh_modmanifest_row *r = &rows->rows[i];
        if (r->pinned) { (void)sh_modmanifest_list_add(&rows_here, r); continue; }
        if (!mm_local_absent(r)) continue;
        {
            sh_modmanifest_row none;
            memset(&none, 0, sizeof none);
            strcpy_s(none.salt, sizeof none.salt, r->salt);
            if (!sh_modmanifest_composition_hash(r->salt, NULL, 0, none.build)) continue;
            none.name = r->name;   /* copied by the add */
            none.size = r->size;
            none.pinned = 1;
            none.local_broken = sh_modserve_salt_installed(r->salt);
            (void)sh_modmanifest_list_add(&rows_here, &none);
        }
    }
    if (!rows_here.count) { sh_modmanifest_list_free(&rows_here); return; }
    placed_count = sh_modmanifest_placed_names(json, length, &placed, &failed);
    t_content_text = json;
    t_content_length = length;
    content(&rows_here, placed, failed ? 0 : placed_count);
    t_content_text = NULL;
    t_content_length = 0;
    sh_modmanifest_names_free(placed, placed_count);
    sh_modmanifest_list_free(&rows_here);
}

static char *mm_prepare_rows(const char *json, size_t length, size_t *out_length)
{
    sh_modmanifest_list list = {0};
    unsigned rejected = 0;
    int active;
    char *line, *names;

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
    /* Pinned rows: the content cache composes what it can before the rows resolve (modcache.h). */
    sh_modmanifest_content_for(json, length, &list);

    names = mm_describe(&list);
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
        line = sh_textf("MODS: map manifest names %zu build(s)%s%s; %s", list.count, list.count ? ": " : "",
                        names ? names : "", what);
    }
    backend_log(line ? line : "MODS: map manifest read");
    free(line);
    free(names);
    if (rejected) {
        char note[96];
        _snprintf_s(note, sizeof note, _TRUNCATE, "MODS: %u manifest row(s) did not parse and were dropped", rejected);
        backend_log(note);
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
    if (name && !sh_modmanifest_row_add_name(row, name))
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
        if (!sh_modmanifest_row_name(r)[0] && resolve(r->build, r->salt, &served) == 1 && !strcmp(served.build, r->build) &&
            !strcmp(served.salt, r->salt) && !sh_modmanifest_row_set_name(r, served.name))
            ctx->failed = 1;
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
        /* A view of the attached row (its strings borrowed, not owned):
         * sh_modmanifest_list_add copies what it keeps. */
        sh_modmanifest_row row = attached->rows[i], served;
        sh_modmanifest_row *have;
        int ok = 1;
        memset(&served, 0, sizeof served);
        row.names = NULL;
        row.name_count = 0;
        if (resolve && resolve(row.build, row.salt, &served) == 1 && !strcmp(served.salt, row.salt) &&
            sh_modmanifest_valid_hash(served.build, strlen(served.build))) {
            mm_current_rename(row.build, &served);
            row.name = served.name;
            strcpy_s(row.build, sizeof row.build, served.build);
            row.size = served.size;
        }
        row.attached = 1;
        have = mm_list_find(&ctx->list, row.salt, row.build);
        if (have) have->attached = 1;
        else ok = sh_modmanifest_list_add(&ctx->list, &row);
        sh_modmanifest_row_free(&served);
        if (!ok) return 0;
    }
    return 1;
}

/* The rows of the open map that the save's closure did not name although its
 * entities still place names under their salt (the mod failed to build, or
 * its folder is gone): kept as they are, so the next load still declares what
 * the map places. 0 when memory runs out. */
static int mm_save_keep_unserved(mm_save_ctx *ctx, char **names, size_t count)
{
    sh_modmanifest_list cur;
    size_t i, k;
    int ok = 1;
    memset(&cur, 0, sizeof cur);
    if (!sh_modmanifest_current(&cur)) { sh_modmanifest_list_free(&cur); return 1; }
    for (i = 0; ok && i < cur.count; i++) {
        const sh_modmanifest_row *r = &cur.rows[i];
        int named = 0;
        for (k = 0; k < ctx->list.count && !named; k++) named = !strcmp(ctx->list.rows[k].salt, r->salt);
        if (named || !mm_row_placed(r, names, count)) continue;
        ok = sh_modmanifest_list_add(&ctx->list, r);
        if (ok) {
            char *line = sh_textf("MODS: save manifest: %s is placed but no installed build serves it; its row is kept as it was", r->salt);
            backend_log(line ? line : "MODS: save manifest: a placed mod no build serves keeps its row");
            free(line);
        }
    }
    sh_modmanifest_list_free(&cur);
    return ok;
}

/* A row the save's closure found served by a composition (its build is the composition hash of one of the open
 * map's pinned rows) keeps that row's units: a downloaded map saved here keeps the units its rows name, and no unit
 * is computed at a save (D-A15). A row served by a local folder is written as a local row. 0 when memory runs out. */
/* The version rule's notice (S9, D23a), raised by the bound sink (modpublished.c's notice queue). */
static void (*g_notice)(const char *text);

/* An object's name for a sentence from its placed name ("mods/<salt>/demons/marauder_boss" -> "Marauder boss"). */
static char *mm_object_title(const char *name)
{
    const char *leaf = strrchr(name, '/');
    char *out = sh_text(leaf ? leaf + 1 : name), *c;
    if (!out) return NULL;
    for (c = out; *c; c++) if (*c == '_') *c = ' ';
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    return out;
}
void sh_modmanifest_set_notice(void (*fn)(const char *text)) { g_notice = fn; }

/* Whether the installed version of a salt carries a placed name (modserve.h sh_modserve_root_carries), bound by the
 * product; without it the version rule never moves a mod (the store tool, tests that do not bind it). */
static int (*g_carries)(const char *salt, const char *name);
void sh_modmanifest_set_carries(int (*fn)(const char *salt, const char *name)) { g_carries = fn; }

/* D23, the version rule (5.2, 5.10): a map holds one version of each mod. When the save finds a mod both in a pinned
 * row (names its units serve) and in a local row (names only the installed version serves: an object placed from it,
 * which the editor's overlay shows, D26), every placed object of that mod moves to the installed version: the two rows
 * become one local row, with S9. D23a: when the installed version lacks an object the pinned row places, the move does
 * not happen yet and the notice says why (owner question Q9: how the kept object's files travel in a local row). With
 * `say` 0 (a publish asking what a save would write) nothing is said. Returns 0 when memory ran out. */
static int mm_save_version(mm_save_ctx *ctx, int say)
{
    size_t i, j, k;
    if (!g_carries) return 1;   /* unbound (the store tool, a test): no version rule */
    for (i = 0; i < ctx->list.count; i++) {
        sh_modmanifest_row *p = &ctx->list.rows[i], *l = NULL;
        size_t lat = 0, missing = 0;
        const char *gone = NULL;
        char *line = NULL;
        if (!p->pinned) continue;
        for (j = 0; j < ctx->list.count && !l; j++)
            if (j != i && !ctx->list.rows[j].pinned && !strcmp(ctx->list.rows[j].salt, p->salt)) { l = &ctx->list.rows[j]; lat = j; }
        if (!l) continue;
        for (k = 0; k < p->name_count; k++)
            if (!g_carries || g_carries(p->salt, p->names[k]) != 1) { missing++; if (!gone) gone = p->names[k]; }
        if (missing) {
            if (say) {
                char *object = gone ? mm_object_title(gone) : NULL;
                line = sh_textf("%s was not moved to the version you have installed: the %s is not in it. Objects placed "
                                "from your version show in the editor only, until your version has the %s again or "
                                "you remove it from the map.", sh_modmanifest_row_name(p)[0] ? p->name : "A mod",
                                object ? object : "object", object ? object : "object");
                free(object);
            }
        } else {
            size_t moved;
            for (k = 0; k < p->name_count; k++)
                if (!sh_modmanifest_row_add_name(l, p->names[k])) return 0;
            if (p->attached) l->attached = 1;
            if (!sh_modmanifest_row_name(l)[0] && p->name && !sh_modmanifest_row_set_name(l, p->name)) return 0;
            moved = l->name_count;
            if (say)
                line = moved == 1
                    ? sh_textf("This map now uses the version of %s you have installed for its object.",
                               sh_modmanifest_row_name(l)[0] ? l->name : "this mod")
                    : sh_textf("This map now uses the version of %s you have installed for all %zu of its objects.",
                               sh_modmanifest_row_name(l)[0] ? l->name : "this mod", moved);
            sh_modmanifest_row_free(p);
            memmove(p, p + 1, (ctx->list.count - i - 1) * sizeof *p);
            ctx->list.count--;
            (void)lat;
            i--;
        }
        if (line) {
            char *log = sh_textf("MODS: version: %s", line);
            backend_log(log ? log : line);
            free(log);
            if (g_notice) g_notice(line);
            free(line);
        }
    }
    return 1;
}

static int mm_save_pins(mm_save_ctx *ctx)
{
    sh_modmanifest_list cur;
    size_t i, k;
    int ok = 1;
    memset(&cur, 0, sizeof cur);
    (void)sh_modmanifest_current(&cur);
    for (i = 0; ok && i < ctx->list.count; i++) {
        sh_modmanifest_row *r = &ctx->list.rows[i];
        for (k = 0; k < cur.count; k++) {
            const sh_modmanifest_row *c = &cur.rows[k];
            const char **units;
            size_t u;
            if (!c->pinned || strcmp(c->build, r->build) || strcmp(c->salt, r->salt)) continue;
            units = c->unit_count ? (const char **)calloc(c->unit_count, sizeof *units) : NULL;
            if (c->unit_count && !units) { ok = 0; break; }
            for (u = 0; u < c->unit_count; u++) units[u] = c->units[u];
            ok = sh_modmanifest_row_pin(r, (const char *const *)c->roots, units, c->unit_count);
            free((void *)units);
            if (ok && !sh_modmanifest_row_name(r)[0] && c->name) ok = sh_modmanifest_row_set_name(r, c->name);
            /* The module records travel with the units (D-A30): a save keeps them as the load read them. */
            if (ok && c->modules && !r->modules) ok = (r->modules = sh_text(c->modules)) != NULL;
            if (ok && c->size) r->size = c->size;
            if (ok && c->attached) r->attached = 1;
            break;
        }
    }
    sh_modmanifest_list_free(&cur);
    return ok;
}

/* A local row whose folder is not here is served by its stand-ins (D6a), whose bundle's size the closure gave it: it
 * keeps the size and name the open map's row was loaded with, which say what the mod is (D6), as it stays a local
 * row. */
static void mm_save_keep_absent(mm_save_ctx *ctx)
{
    sh_modmanifest_list cur;
    size_t i, k;
    memset(&cur, 0, sizeof cur);
    if (!sh_modmanifest_current(&cur)) { sh_modmanifest_list_free(&cur); return; }
    for (i = 0; i < ctx->list.count; i++) {
        sh_modmanifest_row *r = &ctx->list.rows[i];
        if (r->pinned || !mm_local_absent(r)) continue;
        for (k = 0; k < cur.count; k++) {
            const sh_modmanifest_row *c = &cur.rows[k];
            if (c->pinned || strcmp(c->salt, r->salt)) continue;
            if (c->size) r->size = c->size;
            if (!sh_modmanifest_row_name(r)[0] && c->name) (void)sh_modmanifest_row_set_name(r, c->name);
            break;
        }
    }
    sh_modmanifest_list_free(&cur);
}

/* A save step failed (why): the save keeps the rows the open map has, and the product features it uses, so a map is
 * never written without its declaration. NULL when it has none or they could not be written either. */
static char *mm_save_fallback(const char *json, size_t length, size_t *out_length, unsigned uses, const char *why)
{
    sh_modmanifest_list rows;
    char err[192] = "", *out = NULL, *line;
    size_t written = 0;
    memset(&rows, 0, sizeof rows);
    (void)sh_modmanifest_current(&rows);
    rows.uses = uses;
    if (rows.count || rows.uses) out = sh_modmanifest_write(json, length, &rows, &written, err, sizeof err);
    if (out) line = sh_textf("MODS: save manifest: %s; the save keeps the %zu row(s) the open map has", why, rows.count);
    else if (rows.count) line = sh_textf("MODS: save manifest FAILED: %s, and the open map's %zu row(s) could not be written (%s)", why, rows.count, err[0] ? err : "unknown");
    else line = sh_textf("MODS: save manifest: %s; the open map has no rows", why);
    backend_log(line ? line : "MODS: save manifest: a step failed");
    free(line);
    if (out && out_length) *out_length = written;
    sh_modmanifest_list_free(&rows);
    return out;
}

char *sh_modmanifest_on_save(const char *json, size_t length, size_t *out_length)
{
    char **names = NULL;
    size_t count, i, written = 0;
    int failed = 0, stale;
    mm_save_ctx ctx;
    sh_modmanifest_list attached;
    sh_modmanifest_scope_fn scope = g_scope;
    sh_modmanifest_uses_fn uses_fn = g_uses;
    char *out = NULL;
    char err[192] = "", *line, *described;

    if (out_length) *out_length = 0;
    if (!json || !length) return NULL;
    memset(&ctx, 0, sizeof ctx);
    stale = mm_carries_rows(json, length);
    /* No installed bundle serves anything, the map carries no rows and no
     * map-level mod, and nothing asks what it uses of the product: the save is
     * the engine's own bytes, without walking the map. */
    (void)sh_modmanifest_current_attached(&attached);
    if (!stale && !attached.count && !sh_modserve_active() && !uses_fn) {
        if (scope && g_scope_armed && g_scope_armed() > 0) scope(&ctx.list, NULL, 0, SH_MODSERVE_AT_SAVE);
        return NULL;
    }
    count = sh_modmanifest_placed_names(json, length, &names, &failed);
    if (failed) {
        sh_modmanifest_list_free(&attached);
        return mm_save_fallback(json, length, out_length, 0, "the saved map's JSON did not read cleanly");
    }
    /* What the map uses of Snapmap+ itself (a resized module, its own navigation) flags it as modded: a game without
     * Snapmap+ cannot load it (PLATFORM, Vanilla stays vanilla). */
    ctx.list.uses = uses_fn ? uses_fn(json, length, (const char *const *)names, count) : 0;
    if (!stale && !attached.count && !sh_modserve_active() && !ctx.list.uses) {
        sh_modmanifest_names_free(names, count);
        sh_modmanifest_list_free(&attached);
        if (scope && g_scope_armed && g_scope_armed() > 0) scope(&ctx.list, NULL, 0, SH_MODSERVE_AT_SAVE);
        return NULL;
    }
    if (count) modserve_bundles_for_names_visit((const char *const *)names, count, mm_save_visit, &ctx);
    if (!ctx.failed) mm_save_names(&ctx);
    if (!ctx.failed && attached.count && !mm_save_attached(&ctx, &attached)) ctx.failed = 1;
    if (!ctx.failed && !mm_save_keep_unserved(&ctx, names, count)) ctx.failed = 1;
    if (!ctx.failed && !mm_save_pins(&ctx)) ctx.failed = 1;
    if (!ctx.failed) mm_save_keep_absent(&ctx);
    if (!ctx.failed && !mm_save_version(&ctx, 1)) ctx.failed = 1;
    sh_modmanifest_names_free(names, count);
    sh_modmanifest_list_free(&attached);
    if (ctx.failed) {
        unsigned uses = ctx.list.uses;
        sh_modmanifest_list_free(&ctx.list);
        return mm_save_fallback(json, length, out_length, uses, "the serving layer reported an invalid mod, or memory ran out");
    }
    /* What the map now lists: placed entities may have been added since the load. */
    if (scope) scope(&ctx.list, NULL, 0, SH_MODSERVE_AT_SAVE);
    if (ctx.list.count == 0 && !ctx.list.uses && !stale) return NULL;

    qsort(ctx.list.rows, ctx.list.count, sizeof *ctx.list.rows, mm_row_order);
    mm_choices_save(&ctx.list);
    for (i = 0; i < ctx.list.count; i++)
        if (ctx.list.rows[i].name_count > 1)
            qsort(ctx.list.rows[i].names, ctx.list.rows[i].name_count, sizeof(char *), mm_cstr_cmp);
    {
        /* A publish's save writes its rows pinned (modpin.h, bound by modpublish.c while a publish is armed); the open
         * map keeps the rows it has, so its session goes on serving what it served. */
        sh_modmanifest_pin_fn pin = g_pin;
        sh_modmanifest_list pinned;
        size_t k;
        int use_pinned = 0;
        memset(&pinned, 0, sizeof pinned);
        if (pin) {
            pinned.uses = ctx.list.uses;
            for (k = 0; k < ctx.list.count && sh_modmanifest_list_add(&pinned, &ctx.list.rows[k]); k++) {}
            use_pinned = k == ctx.list.count && pin(&pinned, json, length) > 0;
        }
        out = sh_modmanifest_write(json, length, use_pinned ? &pinned : &ctx.list, &written, err, sizeof err);
        sh_modmanifest_list_free(&pinned);
    }
    described = mm_describe(&ctx.list);
    if (out) {
        line = sh_textf("MODS: save manifest wrote %zu row(s) into the snapmap-plus member%s%s%s; %zu -> %zu bytes",
                        ctx.list.count, ctx.list.count ? ": " : "", described ? described : "",
                        stale ? " (stale rows replaced)" : "", length, written);
        if (out_length) *out_length = written;
    } else {
        line = sh_textf("MODS: save manifest: the rows for %s could not be written (%s)",
                        described && described[0] ? described : "its mods", err[0] ? err : "unknown");
    }
    backend_log(line ? line : out ? "MODS: save manifest wrote its rows" : "MODS: save manifest FAILED");
    free(line);
    free(described);
    /* The open map's record is what it now saves: `sh_mods map`, the Mods tab
     * and the next listing read these rows, not the ones it was loaded with
     * (a map opened from a rawmap had none). */
    if (out) mm_set_current(&ctx.list);
    sh_modmanifest_list_free(&ctx.list);
    if (!out) out = mm_save_fallback(json, length, out_length, ctx.list.uses, "the new rows could not be written");
    return out;
}

/* ------------------------------------------------------------ the rows a save would write */

int sh_modmanifest_rows_for(const char *const *names, size_t count, sh_modmanifest_list *out)
{
    mm_save_ctx ctx;
    sh_modmanifest_list attached;
    size_t i;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    memset(&ctx, 0, sizeof ctx);
    (void)sh_modmanifest_current_attached(&attached);
    if (count) modserve_bundles_for_names_visit(names, count, mm_save_visit, &ctx);
    if (!ctx.failed) mm_save_names(&ctx);
    if (!ctx.failed && attached.count && !mm_save_attached(&ctx, &attached)) ctx.failed = 1;
    if (!ctx.failed && !mm_save_keep_unserved(&ctx, (char **)names, count)) ctx.failed = 1;
    if (!ctx.failed && !mm_save_pins(&ctx)) ctx.failed = 1;
    if (!ctx.failed) mm_save_keep_absent(&ctx);
    if (!ctx.failed && !mm_save_version(&ctx, 0)) ctx.failed = 1;
    sh_modmanifest_list_free(&attached);
    if (ctx.failed) { sh_modmanifest_list_free(&ctx.list); return -1; }
    for (i = 0; i < ctx.list.count; i++)
        if (ctx.list.rows[i].name_count > 1)
            qsort(ctx.list.rows[i].names, ctx.list.rows[i].name_count, sizeof(char *), mm_cstr_cmp);
    qsort(ctx.list.rows, ctx.list.count, sizeof *ctx.list.rows, mm_row_order);
    *out = ctx.list;
    return (int)out->count;
}

int sh_modmanifest_version_rule(const char *const *names, size_t count)
{
    mm_save_ctx ctx;
    sh_modmanifest_list cur;
    size_t i, k;
    int moved = 0;
    if (!names || !count) return 0;
    memset(&ctx, 0, sizeof ctx);
    memset(&cur, 0, sizeof cur);
    (void)sh_modmanifest_current(&cur);
    for (i = 0, k = 0; i < cur.count && !k; i++) k = cur.rows[i].pinned;
    if (!k) { sh_modmanifest_list_free(&cur); return 0; }   /* nothing pinned: nothing to move */
    modserve_bundles_for_names_visit(names, count, mm_save_visit, &ctx);
    if (!ctx.failed) mm_save_names(&ctx);
    if (!ctx.failed && !mm_save_pins(&ctx)) ctx.failed = 1;
    if (!ctx.failed && !mm_save_version(&ctx, 1)) ctx.failed = 1;
    /* A salt the open map has pinned and the rule made local: the open map's row follows. */
    for (i = 0; !ctx.failed && i < ctx.list.count; i++) {
        const sh_modmanifest_row *r = &ctx.list.rows[i];
        size_t j;
        int still = 0;
        if (r->pinned) continue;
        /* the rule merged this salt only when no pinned row of it is left (D23a keeps both) */
        for (j = 0; j < ctx.list.count && !still; j++) still = ctx.list.rows[j].pinned && !strcmp(ctx.list.rows[j].salt, r->salt);
        if (still) continue;
        for (k = 0; k < cur.count; k++)
            if (cur.rows[k].pinned && !strcmp(cur.rows[k].salt, r->salt) &&
                sh_modmanifest_current_make_local(r->salt, r->build, (const char *const *)r->names, r->name_count) > 0) {
                moved++;
                break;
            }
    }
    sh_modmanifest_list_free(&ctx.list);
    sh_modmanifest_list_free(&cur);
    return ctx.failed ? -1 : moved;
}

/* ------------------------------------------------------------ the listing (one rule for every mode) */

typedef struct mm_listing {
    char (*hashes)[SH_MODMANIFEST_HASH_CAP];
    size_t n, cap;
    char (*ids)[SH_MODMANIFEST_SALT_CAP];
    size_t ni, capi;
    int oom;
} mm_listing;

static void mm_listing_push(mm_listing *l, const char *hash)
{
    size_t i;
    if (!hash || strlen(hash) != SH_MOD_HASH_CHARS) return;
    for (i = 0; i < l->n; i++) if (!_stricmp(l->hashes[i], hash)) return;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        char (*na)[SH_MODMANIFEST_HASH_CAP] = realloc(l->hashes, nc * sizeof *na);
        if (!na) { l->oom = 1; return; }
        l->hashes = na;
        l->cap = nc;
    }
    strcpy_s(l->hashes[l->n++], SH_MODMANIFEST_HASH_CAP, hash);
}

static void mm_listing_id(mm_listing *l, const char *salt)
{
    if (!salt || !salt[0] || strlen(salt) >= SH_MODMANIFEST_SALT_CAP) return;
    if (l->ni == l->capi) {
        size_t nc = l->capi ? l->capi * 2 : 16;
        char (*na)[SH_MODMANIFEST_SALT_CAP] = realloc(l->ids, nc * sizeof *na);
        if (!na) { l->oom = 1; return; }
        l->ids = na;
        l->capi = nc;
    }
    strcpy_s(l->ids[l->ni++], SH_MODMANIFEST_SALT_CAP, salt);
}

/* The build the row resolver names for `row` when it is not the row's own: 1 with its hash in `out`. */
static int mm_row_served_by(const sh_modmanifest_row *row, char out[SH_MODMANIFEST_HASH_CAP])
{
    sh_modserve_bundle_ref ref;
    int other;
    memset(&ref, 0, sizeof ref);
    other = sh_modserve_row_build(row, &ref) && strlen(ref.hash) == SH_MOD_HASH_CHARS && _stricmp(ref.hash, row->build);
    if (other) strcpy_s(out, SH_MODMANIFEST_HASH_CAP, ref.hash);
    sh_modserve_bundle_ref_free(&ref);
    return other;
}

/* The build a placed name (or its closure) lists. The closure resolves each name under the listing in force, which at
 * a load is the previous map's, so the build it reports may be one that map declared; the listing instead takes, per
 * salt, what the new map serves: the build its rows declare (already listed), else the build the row resolver names
 * for that salt in the open map's context (the root folder's for the player's own map). */
static void mm_listing_visit(void *context, const char *bundle_id, const char *bundle_hash, unsigned long long size,
                             const char *name)
{
    mm_listing *l = (mm_listing *)context;
    sh_modmanifest_row probe;
    char served[SH_MODMANIFEST_HASH_CAP];
    size_t i;
    (void)size; (void)name;
    for (i = 0; bundle_id && i < l->ni; i++) if (!_stricmp(l->ids[i], bundle_id)) return;   /* a row declares the salt */
    memset(&probe, 0, sizeof probe);
    if (bundle_id && strlen(bundle_id) < sizeof probe.salt && bundle_hash && strlen(bundle_hash) < sizeof probe.build) {
        strcpy_s(probe.salt, sizeof probe.salt, bundle_id);
        strcpy_s(probe.build, sizeof probe.build, bundle_hash);
        if (mm_row_served_by(&probe, served)) { mm_listing_push(l, served); return; }
    }
    mm_listing_push(l, bundle_hash);
}

int sh_modmanifest_listing(const sh_modmanifest_list *rows, const char *const *names, size_t name_count,
                           char (**out)[SH_MODMANIFEST_HASH_CAP])
{
    mm_listing l;
    size_t i;
    if (out) *out = NULL;
    memset(&l, 0, sizeof l);
    for (i = 0; rows && i < rows->count; i++) {
        const sh_modmanifest_row *r = &rows->rows[i];
        char sub[SH_MODMANIFEST_HASH_CAP];
        if (!_stricmp(r->salt, SH_MODMANIFEST_PRODUCT)) continue;
        if (sh_modmanifest_substituted(r->build, sub, sizeof sub) || mm_row_served_by(r, sub)) mm_listing_push(&l, sub);
        else mm_listing_push(&l, r->build);
        mm_listing_id(&l, r->salt);
    }
    if (names && name_count && sh_modserve_active())
        (void)modserve_bundles_for_names_visit(names, name_count, mm_listing_visit, &l);
    free(l.ids);
    if (l.oom) { free(l.hashes); return -1; }
    if (out) *out = l.hashes;
    else free(l.hashes);
    return (int)l.n;
}
