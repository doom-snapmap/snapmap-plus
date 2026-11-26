#include <windows.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "../common/text.h"
#include "config_json.h"
#include "local_map_copy.h"
#include "map_carrier.h"
#include "modbundle.h"

static int plain(const char *path, int directory)
{
    wchar_t *w = path ? sh_mb_wpath(path) : NULL;
    DWORD attrs = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) &&
           !!(attrs & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

static int string_field(const sh_json_object *obj, const char *key, char *out, size_t cap)
{
    const char *v = sh_json_object_get(obj, key);
    size_t n;
    return v && sh_json_decode_string(v, strlen(v), out, cap, &n) && n < cap && n == strlen(out);
}

static int map_leaf(const char *name)
{
    size_t n = strlen(name);
    if (n <= 5 || _stricmp(name + n - 5, ".json") || strpbrk(name, "<>:\"/\\|?*")) return 0;
    for (size_t i = 0; i < n; i++) if ((unsigned char)name[i] < 32) return 0;
    return 1;
}

/* Check a separate map view so comments and escaped keys read correctly. Hash and load the original bytes. */
static char *map_view(const char *s, size_t *length)
{
    size_t n = *length;
    char *v = sh_textn(s, n);
    if (!v) return NULL;
    for (size_t i = 0; i < n;) {
        if (v[i] == '"') {
            for (i++; i < n && v[i] != '"'; i++) if (v[i] == '\\' && i + 1 < n) i++;
            if (i < n) i++;
        } else if (v[i] == '/' && i + 1 < n && (v[i+1] == '/' || v[i+1] == '*')) {
            size_t start = i;
            int block = v[i+1] == '*';
            i += 2;
            if (block) {
                while (i + 1 < n && (v[i] != '*' || v[i+1] != '/')) i++;
                if (i + 1 >= n) { free(v); return NULL; }
                i += 2;
            } else {
                while (i < n && v[i] != '\r' && v[i] != '\n') i++;
            }
            memset(v + start, ' ', i - start);
        } else i++;
    }
    /* The carrier reader uses literal ASCII keys. JSON also admits the same letters as Unicode escapes. */
    size_t w = 0;
    int quoted = 0;
    for (size_t i = 0; i < n;) {
        char ch = v[i++];
        if (quoted && ch == '\\' && i < n) {
            if (i + 4 < n && v[i] == 'u' && v[i+1] == '0' && v[i+2] == '0' &&
                isxdigit((unsigned char)v[i+3]) && isxdigit((unsigned char)v[i+4])) {
                char h[3] = { v[i+3], v[i+4], 0 };
                char ascii = (char)strtol(h, NULL, 16);
                if (ascii >= 32 && ascii <= 126 && ascii != '"' && ascii != '\\') {
                    v[w++] = ascii;
                    i += 5;
                    continue;
                }
            }
            v[w++] = ch;
            v[w++] = v[i++];
        } else {
            v[w++] = ch;
            if (ch == '"') quoted = !quoted;
        }
    }
    v[w] = 0;
    *length = w;
    return v;
}

static int map_document(const char *text, size_t n, sh_json_object *map)
{
    if (!sh_native_json_parse_object(text, n, 256, map)) return 0;
    const char *entities = sh_json_object_get(map, "entities");
    char type[32];
    return (!entities || entities[0] == '[') && (sh_json_object_get(map, "~type") ?
        string_field(map, "~type", type, sizeof type) && !strcmp(type, "idSnapMap") :
        entities && sh_json_object_get(map, "instances") && sh_json_object_get(map, "instanceEntities") &&
        sh_json_object_get(map, "version"));
}

static int early_delivery(const char *vars)
{
    const char *rows, *end, *item;
    size_t n;
    if (!vars || sh_carrier_object_member(vars, vars + strlen(vars), "string", &rows, &n) != 1) return 0;
    end = rows + n;
    while (sh_carrier_array_next(&rows, end, &item, &n) == 1) {
        const char *info, *name, *value, *type;
        size_t in, nn, vn, tn;
        if (sh_carrier_object_member(item, item + n, "info", &info, &in) == 1 &&
            sh_carrier_object_string(info, info + in, "~type", &type, &tn) == 1 &&
            tn == 13 && !memcmp(type, "snapVarInfo_t", tn) &&
            sh_carrier_object_string(item, item + n, "initialValue", &value, &vn) == 1 && vn &&
            sh_carrier_object_string(info, info + in, "name", &name, &nn) == 1 &&
            ((nn == 97 && !memcmp(name, "smpref.delivery.", 16) && name[80] == '.' &&
              strspn(name + 16, "0123456789abcdef") == 64 && strspn(name + 81, "0123456789abcdef") == 16) ||
             (nn >= 28 && !memcmp(name, "smpbind.", 8) && name[nn-17] == '.' &&
              strspn(name + nn-16, "0123456789abcdef") == 16))) return 1;
    }
    return 0;
}

/* Surviving placements keep their native identities and order. Room identity includes its transform. */
static int same_placement(const char *a, size_t an, const char *b, size_t bn, int room)
{
    static const char *const room_keys[] = {"moduleName", "origin", "orientation"};
    const size_t count = room ? ARRAYSIZE(room_keys) : 1;
    if (an == bn && !memcmp(a, b, an)) return 1;
    for (size_t i = 0; i < count; i++) {
        const char *key = room ? room_keys[i] : "uniqueId", *av, *bv;
        size_t al, bl;
        int has = sh_carrier_object_member(a, a + an, key, &av, &al);
        if (has < 0 || (!i && !has) || sh_carrier_object_member(b, b + bn, key, &bv, &bl) != has ||
            (has && (al != bl || memcmp(av, bv, al)))) return 0;
    }
    return 1;
}

static int surviving_placements(const char *a, const char *b, int room)
{
    const char *ae = a + strlen(a), *be = b + strlen(b), *av, *bv;
    size_t an, bn;
    int rc;
    if (*a != '[' || *b != '[') return 0;
    while ((rc = sh_carrier_array_next(&b, be, &bv, &bn)) == 1) {
        do {
            if (sh_carrier_array_next(&a, ae, &av, &an) != 1) return 0;
        } while (!same_placement(av, an, bv, bn, room));
    }
    return rc == 0;
}

static int vanilla(const char *source, size_t src_len, const char *body, size_t n)
{
    sh_json_object src = {0}, dst = {0};
    if (memchr(source, 0, src_len) || memchr(body, 0, n)) return 0;
    char *original = map_view(source, &src_len), *copy = map_view(body, &n);
    const char *a = original, *b = copy;
    if (a && src_len >= 3 && !memcmp(a, "\xef\xbb\xbf", 3)) { a += 3; src_len -= 3; }
    if (b && n >= 3 && !memcmp(b, "\xef\xbb\xbf", 3)) { b += 3; n -= 3; }
    int valid = a && b && map_document(a, src_len, &src) && map_document(b, n, &dst) &&
        !sh_json_object_get(&dst, "snapmap-plus") && !sh_json_object_get(&dst, "snapmapPlus") &&
        sh_map_old_shape(b, n) == SH_MAP_OLD_NONE;
    if (valid && sh_map_old_shape(a, src_len) == SH_MAP_OLD_NONE && !early_delivery(sh_json_object_get(&src, "variables")))
        valid = 0;
    for (size_t i = 0; valid && i < src.count; i++) {
        const char *key = src.members[i].key, *old = src.members[i].value_json, *next = sh_json_object_get(&dst, key);
        if (!strcmp(key, "snapmap-plus")) continue;
        if (!next) { valid = 0; break; }
        if (!strcmp(old, next)) continue;
        if (!strcmp(key, "instances") || !strcmp(key, "entities"))
            valid = surviving_placements(old, next, !strcmp(key, "instances"));
        else valid = !strcmp(key, "variables") || !strcmp(key, "instanceEntities") || !strcmp(key, "references") ||
                     !strcmp(key, "targets") || !strcmp(key, "doorsAndCaps");
    }
    for (size_t i = 0; valid && i < dst.count; i++) valid = sh_json_object_get(&src, dst.members[i].key) != NULL;
    free(copy); free(original);
    sh_json_object_free(&dst); sh_json_object_free(&src);
    return valid;
}

char *sh_local_map_copy(const char *root, const char *account, const char *slot,
                        const char *source, size_t length, size_t *out_length)
{
    char digest[65], recorded[65], expected[65], name[1024], id[21];
    char *rawmaps = NULL, *copies = NULL, *local = NULL, *owner = NULL, *dir = NULL, *index = NULL, *path = NULL;
    char *record = NULL, *body = NULL;
    size_t n;
    sh_json_object fields = {0};
    if (out_length) *out_length = 0;
    if (!root || !account || !*account || strlen(account) > 20 || strspn(account, "0123456789") != strlen(account) ||
        !slot || strlen(slot) != 20 || !source || !length || length > INT_MAX) return NULL;
    for (size_t i = 0; i < 20; i++) {
        char ch = slot[i];
        if (ch >= 'A' && ch <= 'F') ch += 'a' - 'A';
        if (!strchr("0123456789abcdef", ch)) return NULL;
        id[i] = ch;
    }
    id[20] = 0;
    rawmaps = sh_textf("%s\\rawmaps", root);
    if (!plain(rawmaps, 1)) goto done;
    copies = sh_textf("%s\\Updated saves", rawmaps);
    if (!plain(copies, 1)) goto done;
    local = sh_textf("%s\\.local", copies);
    if (!plain(local, 1)) goto done;
    owner = sh_textf("%s\\%s", local, account);
    if (!plain(owner, 1)) goto done;
    dir = sh_textf("%s\\%s", owner, id);
    if (!plain(dir, 1)) goto done;

    sh_mb_sha256_hex(source, length, digest);
    index = sh_textf("%s\\%s.json", dir, digest);
    if (!plain(index, 0)) goto done;
    record = (char *)sh_mb_read_file(index, 8192, &n);
    if (!record || !sh_json_parse_object(record, n, 2, &fields)) goto done;
    const char *format = sh_json_object_get(&fields, "format");
    if (fields.count != 4 || !format || strcmp(format, "1") ||
        !string_field(&fields, "source", recorded, sizeof recorded) || strcmp(recorded, digest) ||
        !string_field(&fields, "sha256", expected, sizeof expected) || strlen(expected) != 64 ||
        strspn(expected, "0123456789abcdef") != 64 || !strcmp(expected, digest) ||
        !string_field(&fields, "map", name, sizeof name) || !map_leaf(name)) goto done;
    path = sh_textf("%s\\%s", copies, name);
    if (!plain(path, 0)) goto done;
    body = (char *)sh_mb_read_file(path, INT_MAX, &n);
    if (body) {
        sh_mb_sha256_hex(body, n, digest);
        if (strcmp(expected, digest) || !vanilla(source, length, body, n)) {
            free(body);
            body = NULL;
        } else if (out_length) *out_length = n;
    }
done:
    sh_json_object_free(&fields);
    free(record); free(path); free(index); free(dir); free(owner); free(local); free(copies); free(rawmaps);
    return body;
}
