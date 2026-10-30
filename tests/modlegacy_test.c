/* modlegacy_test -- first-generation carriers under the section 11 policy:
 * header grammar per era, completeness and digest checks, ZIP reading
 * (stored and deflate), the era B to era C mapping table, conversion into
 * an ordinary mod folder <data>\mods\<id>\, cleaning with logged verdicts, the
 * smpmod1 rows written for converted bundles, and an integration pass over
 * the campaign's three carrier maps when they are present.
 *
 * The bundle builder (modbundle_build_folder) is replaced by a double that
 * completes mod.json's product block from the unpacked tree, keeping the
 * converter's seed (legacy record, shadows, origin). The carrier maps are not in
 * the repository (their packages hold game files); set SH_MODLEGACY_FIXTURES
 * to their folder to add the integration pass; it is skipped otherwise. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modlegacy.h"
#include "../src/backend/modmanifest.h"

/* mod_seam_stub.c: the serving layer's stand-ins. */
extern int g_mod_stub_rescans;
#include "../src/backend/map_legacy_payload.h"
#include "../src/backend/map_shards.h"
#include "../src/backend/config_json.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ---- log capture --------------------------------------------------------------------------- */

static int g_legacy_lines, g_convert_lines, g_clean_lines, g_memo_lines, g_echo;
static char g_last_clean[1024];
void backend_log(const char *line)
{
    if (!line) return;
    if (!strncmp(line, "LEGACY: smpkg.", 14)) {
        g_legacy_lines++;
        if (strstr(line, "): CONVERT -> ")) g_convert_lines++;
        if (strstr(line, "): CLEAN -- ")) { g_clean_lines++; strncpy_s(g_last_clean, sizeof g_last_clean, line, _TRUNCATE); }
        if (strstr(line, "[converted earlier this session]")) g_memo_lines++;
    }
    if (g_echo && !strncmp(line, "LEGACY:", 7)) printf("    %s\n", line);
}

static void reset_counts(void)
{
    g_legacy_lines = g_convert_lines = g_clean_lines = g_memo_lines = 0;
    g_last_clean[0] = 0;
}

/* ---- builder double ------------------------------------------------------------------------ */

static int g_build_calls, g_build_refuse;

typedef struct tb { char *p; size_t n, cap; } tb;
static void tb_put(tb *t, const char *s)
{
    size_t n = strlen(s);
    if (t->n + n + 1 > t->cap) {
        t->cap = (t->cap + n + 1) * 2;
        t->p = (char *)realloc(t->p, t->cap);
    }
    memcpy(t->p + t->n, s, n + 1);
    t->n += n;
}

static void walk_assets(const char *folder, const char *relative, tb *out, int *first)
{
    char pattern[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\%s\\*", folder, relative);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char child[MAX_PATH * 2];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        _snprintf_s(child, sizeof child, _TRUNCATE, "%s/%s", relative, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walk_assets(folder, child, out, first); continue; }
        {
            char row[MAX_PATH * 4], name[MAX_PATH * 2], *p;
            const char *request = child + 7;
            strcpy_s(name, sizeof name, request);
            for (p = name; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
            if (!_strnicmp(request, "generated/decls/", 16) && strchr(request + 16, '/')) {
                strcpy_s(name, sizeof name, strchr(request + 16, '/') + 1);
                if (strlen(name) > 5 && !_stricmp(name + strlen(name) - 5, ".decl")) name[strlen(name) - 5] = 0;
            }
            _snprintf_s(row, sizeof row, _TRUNCATE,
                "%s{\"path\":\"%s\",\"size\":%lu,\"provides\":{\"type\":\"file\",\"name\":\"%s\"}}",
                *first ? "" : ",", child, fd.nFileSizeLow, name);
            *first = 0;
            tb_put(out, row);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f;
    char *b;
    long n;
    if (fopen_s(&f, path, "rb")) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) { b[n] = 0; if (len) *len = (size_t)n; }
    else { free(b); b = NULL; }
    fclose(f);
    return b;
}

static int write_all(const char *path, const char *data, size_t n)
{
    FILE *f;
    if (fopen_s(&f, path, "wb")) return 0;
    fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

static char *member_text(const sh_json_object *o, const char *key)
{
    const char *raw = sh_json_object_get(o, key);
    char *out;
    size_t n, got = 0;
    if (!raw) return NULL;
    n = strlen(raw);
    out = (char *)malloc(n);
    if (!out || !sh_json_decode_string(raw, n, out, n, &got)) { free(out); return NULL; }
    return out;
}

int modbundle_build_folder(const char *folder, char *first_cause, size_t n)
{
    char path[MAX_PATH * 2], hash[65], salted[MAX_PATH * 2];
    char *desc, *id, *version;
    size_t len = 0;
    sh_json_object o = {0};
    tb out = {0};
    int first = 1, i;
    g_build_calls++;
    if (g_build_refuse) { strncpy_s(first_cause, n, "test builder refuses this folder", _TRUNCATE); return 0; }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", folder);
    desc = read_all(path, &len);
    if (!desc || !sh_json_parse_object(desc, len, 24, &o)) {
        free(desc);
        strncpy_s(first_cause, n, "no readable mod.json", _TRUNCATE);
        return 0;
    }
    id = member_text(&o, "id");
    version = member_text(&o, "version");
    for (i = 0; i < 4; i++) {
        _snprintf_s(salted, sizeof salted, _TRUNCATE, "%d:%s", i, folder);
        sh_shard_digest16((const unsigned char *)salted, strlen(salted), hash + i * 16);
    }
    hash[64] = 0;
    tb_put(&out, "[");
    walk_assets(folder, "assets", &out, &first);
    tb_put(&out, "]");
    {
        sh_json_object block = {0};
        const char *seed = sh_json_object_get(&o, "snapmap-plus");
        char quoted[80], *block_text, *text;
        size_t text_len = 0;
        if (seed) sh_json_parse_object(seed, strlen(seed), 24, &block);
        _snprintf_s(quoted, sizeof quoted, _TRUNCATE, "\"%s\"", hash);
        sh_json_object_set(&block, "format", "1", 4);
        sh_json_object_set(&block, "build", "1", 4);
        sh_json_object_set(&block, "bundle", quoted, 4);
        sh_json_object_set(&block, "files", out.p, 8);
        block_text = sh_json_serialize_object(&block, 2, NULL);
        sh_json_object_set(&o, "snapmap-plus", block_text, 24);
        text = sh_json_serialize_object(&o, 0, &text_len);
        write_all(path, text, text_len);
        free(text);
        free(block_text);
        sh_json_object_free(&block);
    }
    free(out.p);
    free(id);
    free(version);
    free(desc);
    sh_json_object_free(&o);
    return 1;
}

/* ---- ZIP and deflate builders -------------------------------------------------------------- */

static unsigned crc32_of(const unsigned char *p, size_t n)
{
    unsigned c = 0xffffffffu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
    }
    return c ^ 0xffffffffu;
}

typedef struct bw { unsigned char *p; size_t n; unsigned acc; int bits; } bw;
static void bw_bits(bw *w, unsigned v, int n)
{
    while (n--) {
        w->acc |= (v & 1u) << w->bits;
        v >>= 1;
        if (++w->bits == 8) { w->p[w->n++] = (unsigned char)w->acc; w->acc = 0; w->bits = 0; }
    }
}
static void bw_huff(bw *w, unsigned code, int len)
{
    int i;
    for (i = len - 1; i >= 0; i--) bw_bits(w, (code >> i) & 1u, 1);
}

/* One final fixed-Huffman block of literals: a real deflate stream. */
static unsigned char *deflate_fixed(const unsigned char *src, size_t n, size_t *out_n)
{
    bw w = {0};
    size_t i;
    w.p = (unsigned char *)malloc(n * 9 / 8 + 16);
    bw_bits(&w, 1, 1);
    bw_bits(&w, 1, 2);
    for (i = 0; i < n; i++) {
        if (src[i] < 144) bw_huff(&w, 0x30u + src[i], 8);
        else bw_huff(&w, 0x190u + (src[i] - 144u), 9);
    }
    bw_huff(&w, 0, 7);
    if (w.bits) w.p[w.n++] = (unsigned char)w.acc;
    *out_n = w.n;
    return w.p;
}

typedef struct zmember { const char *name; const char *data; int deflate; } zmember;

static void put16(tb *t, unsigned v) { char b[3] = { (char)(v & 255), (char)(v >> 8), 0 }; if (t->n + 3 > t->cap) { t->cap = (t->cap + 3) * 2; t->p = (char *)realloc(t->p, t->cap); } t->p[t->n++] = b[0]; t->p[t->n++] = b[1]; }
static void put32(tb *t, unsigned v) { put16(t, v & 0xffff); put16(t, v >> 16); }
static void putb(tb *t, const void *d, size_t n)
{
    if (t->n + n + 1 > t->cap) { t->cap = (t->cap + n + 1) * 2; t->p = (char *)realloc(t->p, t->cap); }
    memcpy(t->p + t->n, d, n);
    t->n += n;
}

static unsigned char *zip_build(const zmember *m, size_t count, unsigned flags, size_t *out_n)
{
    tb z = {0}, cd = {0};
    size_t i;
    unsigned *offsets = (unsigned *)calloc(count, sizeof *offsets);
    for (i = 0; i < count; i++) {
        size_t n = m[i].data ? strlen(m[i].data) : 0, packed_n = n;
        unsigned char *packed = NULL;
        unsigned crc = crc32_of((const unsigned char *)(m[i].data ? m[i].data : ""), n);
        unsigned method = m[i].deflate ? 8 : 0;
        if (m[i].deflate) packed = deflate_fixed((const unsigned char *)m[i].data, n, &packed_n);
        offsets[i] = (unsigned)z.n;
        put32(&z, 0x04034b50u); put16(&z, 20); put16(&z, flags); put16(&z, method); put16(&z, 0); put16(&z, 0);
        put32(&z, crc); put32(&z, (unsigned)packed_n); put32(&z, (unsigned)n);
        put16(&z, (unsigned)strlen(m[i].name)); put16(&z, 0);
        putb(&z, m[i].name, strlen(m[i].name));
        putb(&z, packed ? (const char *)packed : (m[i].data ? m[i].data : ""), packed_n);
        put32(&cd, 0x02014b50u); put16(&cd, 20); put16(&cd, 20); put16(&cd, flags); put16(&cd, method);
        put16(&cd, 0); put16(&cd, 0); put32(&cd, crc); put32(&cd, (unsigned)packed_n); put32(&cd, (unsigned)n);
        put16(&cd, (unsigned)strlen(m[i].name)); put16(&cd, 0); put16(&cd, 0); put16(&cd, 0); put16(&cd, 0);
        put32(&cd, 0); put32(&cd, offsets[i]);
        putb(&cd, m[i].name, strlen(m[i].name));
        free(packed);
    }
    {
        unsigned cd_at = (unsigned)z.n;
        putb(&z, cd.p, cd.n);
        put32(&z, 0x06054b50u); put16(&z, 0); put16(&z, 0); put16(&z, (unsigned)count); put16(&z, (unsigned)count);
        put32(&z, (unsigned)cd.n); put32(&z, cd_at); put16(&z, 0);
    }
    free(cd.p);
    free(offsets);
    *out_n = z.n;
    return (unsigned char *)z.p;
}

/* ---- map builder --------------------------------------------------------------------------- */

#define VAR(name, value) \
    "{\"info\":{\"customIcon\":{\"targetType\":\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"}," \
    "\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\",\"~type\":\"snapVarString_t\"}"

static const char BASE_MAP[] =
    "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"acceptance/r137/speaker_offset\","
    "\"name\":\"\",\"state\":{\"edit\":{\"props\":{\"model\":\"testpack/foo\"}}}}}],"
    "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[" VAR("door_label", "Open") "]}}";

/* `key` shards of `zip` (chunk characters each) appended to `base`; `skip` drops one index,
 * `dup` repeats one, `digest_override` replaces the header digest. HeapFree the result. */
static char *carrier_map(const char *base, const char *key, const unsigned char *zip, size_t zip_n,
                         size_t chunk, int skip, int dup, const char *digest_override, size_t *out_n)
{
    char digest[17], err[160];
    size_t b64n = ((zip_n + 2) / 3) * 4, shards = (b64n + chunk - 1) / chunk, i, k = 0;
    char *b64 = (char *)malloc(b64n + 1), *headers = (char *)calloc(shards + 1, 256), *out;
    sh_shard_out *parts = (sh_shard_out *)calloc(shards + 1, sizeof *parts);
    sh_shard_b64_encode(zip, zip_n, b64);
    sh_shard_digest16(zip, zip_n, digest);
    if (digest_override) strcpy_s(digest, sizeof digest, digest_override);
    for (i = 0; i < shards; i++) {
        size_t len = b64n - i * chunk < chunk ? b64n - i * chunk : chunk;
        if ((int)i == skip) continue;
        _snprintf_s(headers + k * 256, 256, _TRUNCATE, "smpkg.%s.%u.%u.%s", key, (unsigned)i, (unsigned)shards, digest);
        parts[k].header = headers + k * 256;
        parts[k].chunk = b64 + i * chunk;
        parts[k].chunk_len = len;
        k++;
        if ((int)i == dup) {
            parts[k] = parts[k - 1];
            k++;
        }
    }
    out = sh_shard_insert(base, strlen(base), parts, k, out_n, err, sizeof err);
    if (!out) fprintf(stderr, "carrier_map: %s\n", err);
    free(parts);
    free(headers);
    free(b64);
    return out;
}

/* ---- temp data root ------------------------------------------------------------------------ */

static char g_root[MAX_PATH];

/* Remove a tree this test created under its own unique temp folder. Converted
 * trees pass MAX_PATH, so this walks with long wide paths. */
static void remove_tree_w(const wchar_t *dir)
{
    wchar_t pattern[1024];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf_s(pattern, 1024, _TRUNCATE, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[1024];
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf_s(child, 1024, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree_w(child);
            else DeleteFileW(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void remove_tree(const char *dir)
{
    wchar_t wide[1024] = L"\\\\?\\";
    if (!g_root[0] || _strnicmp(dir, g_root, strlen(g_root))) return;
    if (!MultiByteToWideChar(CP_ACP, 0, dir, -1, wide + 4, 1020)) return;
    remove_tree_w(wide);
}

static int file_exists_at(const char *folder, const char *relative)
{
    char path[MAX_PATH * 2], *p;
    DWORD a;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, relative);
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned count_rows(const char *json, const char *quoted_prefix)
{
    unsigned n = 0;
    const char *p = json;
    while ((p = strstr(p, quoted_prefix)) != NULL) { n++; p++; }
    return n;
}

static unsigned string_rows(const char *json)
{
    sh_shard_doc doc;
    int vars, bucket;
    unsigned n = 0xffffffffu;
    if (!sh_shard_doc_build(json, strlen(json), &doc)) return n;
    vars = sh_shard_doc_member(json, strlen(json), &doc, 0, "variables");
    bucket = vars >= 0 ? sh_shard_doc_member(json, strlen(json), &doc, vars, "string") : -1;
    if (bucket >= 0) n = sh_shard_doc_array_count(json, &doc, bucket);
    sh_shard_doc_free(&doc);
    return n;
}

static int alloc4(const char *json, unsigned *value)
{
    const char *p = strstr(json, "\"allocCount\":[");
    int i;
    if (!p) return 0;
    p += 14;
    for (i = 0; i < 4; i++) { p = strchr(p, ','); if (!p) return 0; p++; }
    *value = (unsigned)strtoul(p, NULL, 10);
    return 1;
}

/* ---- tests --------------------------------------------------------------------------------- */

static void header_grammar_per_era(void)
{
    sh_legacy_header h;
    static const char delivery[] =
        "smpkg.delivery.330a75494d7bb0d1fcd2d40bbf2bc9b62bb43de5d1d4c21c2ff5d4d3a3b0f1e2.12.1033.b6bc5027a7e03af0";
    printf("-- legacy header grammar per era\n");
    CHECK(sh_map_legacy_payload_parse("smpkg.my_pack-2.0.3.0123456789abcdef", 36, &h));
    CHECK(!strcmp(h.key, "my_pack-2") && h.index == 0 && h.total == 3 && h.key_form == SH_LEGACY_KEY_FOLDER &&
          !strcmp(h.digest, "0123456789abcdef"));
    CHECK(sh_map_legacy_payload_parse("smpkg.local.history-pack.4.5.fedcba9876543210", 45, &h));
    CHECK(!strcmp(h.key, "local.history-pack") && h.index == 4 && h.total == 5 &&
          h.key_form == SH_LEGACY_KEY_DESCRIPTOR);
    CHECK(sh_map_legacy_payload_parse(delivery, sizeof delivery - 1, &h));
    CHECK(h.key_form == SH_LEGACY_KEY_DELIVERY && h.index == 12 && h.total == 1033 && strlen(h.key) == 73);
    /* A dotted key that only resembles a delivery key is a descriptor id. */
    CHECK(sh_map_legacy_payload_parse("smpkg.delivery.abc.0.1.0123456789abcdef", 39, &h) &&
          h.key_form == SH_LEGACY_KEY_DESCRIPTOR);
    /* Out-of-range index is a header; completeness rejects it later. */
    CHECK(sh_map_legacy_payload_parse("smpkg.a.7.2.0123456789abcdef", 28, &h) && h.index == 7);
    CHECK(!sh_map_legacy_payload_parse("smpkg.a.0.0.0123456789abcdef", 28, &h));   /* total 0 */
    CHECK(!sh_map_legacy_payload_header("smpkg.a.0.1.0123456789ABCDEF", 28));
    CHECK(!sh_map_legacy_payload_header("smpkg..0.1.0123456789abcdef", 27));
    CHECK(!sh_map_legacy_payload_header("smpkg.a.0.12345678.0123456789abcdef", 35));
    CHECK(!sh_map_legacy_payload_header("smpkg.notes", 11));
    CHECK(!strcmp(sh_map_legacy_key_form_name(SH_LEGACY_KEY_DELIVERY), "delivery key (beta.16-19)"));
    CHECK(sh_map_legacy_unreleased_header("smpbind.0.1.2cf482ed5e4749cc", 28));
    CHECK(sh_map_legacy_unreleased_header("smpref.delivery.abc.1e6351e40eb33760", 36));
    CHECK(!sh_map_legacy_unreleased_header("smpref.1e6351e40eb33760", 23));
    CHECK(!sh_map_legacy_unreleased_header("smpbind.notes", 13));
}

static void zip_reading(void)
{
    static const char big[] =
        "entityDef acceptance/r137/speaker_offset { inherit = \"speaker\"; editorVars { } "
        "edit = { spawnPosition = { x = 1; y = 2; z = 3; } } } \xe9\xff high bytes too";
    zmember m[] = {
        { "assets/", NULL, 0 },
        { "assets/generated/decls/entitydef/a.decl", "stored body", 0 },
        { "assets/generated/decls/entitydef/b.decl", big, 1 },
        { "assets/empty.bin", "", 1 },
        { "package.json", "{\"id\":\"t\",\"name\":\"T\"}", 0 },
    };
    unsigned char *zip, *body;
    size_t n, len;
    sh_modlegacy_zip z;
    char err[200];
    const sh_modlegacy_zip_entry *e;
    printf("-- ZIP reading: stored and deflate\n");
    zip = zip_build(m, 5, 0, &n);
    CHECK(sh_modlegacy_zip_open(zip, n, &z, err, sizeof err));
    CHECK(z.count == 5 && z.files == 4 && z.entries[0].directory && !strcmp(z.entries[0].name, "assets"));
    e = sh_modlegacy_zip_find(&z, "assets/generated/decls/entitydef/b.decl");
    CHECK(e && e->method == 8 && e->packed != e->size);
    body = e ? sh_modlegacy_zip_read(e, &len, err, sizeof err) : NULL;
    CHECK(body && len == sizeof big - 1 && !memcmp(body, big, len));
    free(body);
    e = sh_modlegacy_zip_find(&z, "assets/generated/decls/entitydef/a.decl");
    body = e ? sh_modlegacy_zip_read(e, &len, err, sizeof err) : NULL;
    CHECK(body && len == 11 && !memcmp(body, "stored body", 11));
    free(body);
    e = sh_modlegacy_zip_find(&z, "assets/empty.bin");
    body = e ? sh_modlegacy_zip_read(e, &len, err, sizeof err) : NULL;
    CHECK(body && len == 0);
    free(body);
    sh_modlegacy_zip_free(&z);

    /* A corrupted stored byte fails its CRC; a corrupted directory fails to open. */
    {
        unsigned char *bad = (unsigned char *)malloc(n);
        unsigned char *hit;
        memcpy(bad, zip, n);
        for (hit = bad; hit + 11 <= bad + n && memcmp(hit, "stored body", 11); hit++) {}
        if (hit + 11 > bad + n) hit = NULL;
        if (hit) hit[0] ^= 1;
        CHECK(sh_modlegacy_zip_open(bad, n, &z, err, sizeof err));
        e = sh_modlegacy_zip_find(&z, "assets/generated/decls/entitydef/a.decl");
        CHECK(e && sh_modlegacy_zip_read(e, &len, err, sizeof err) == NULL && strstr(err, "CRC"));
        sh_modlegacy_zip_free(&z);
        memcpy(bad, zip, n);
        bad[n - 22] = 0;   /* end-of-directory signature */
        CHECK(!sh_modlegacy_zip_open(bad, n, &z, err, sizeof err));
        CHECK(!sh_modlegacy_zip_open((const unsigned char *)"not a zip at all, really not", 28, &z, err, sizeof err));
        free(bad);
    }
    free(zip);
    /* Encrypted members are refused. */
    zip = zip_build(m + 1, 1, 1, &n);
    CHECK(!sh_modlegacy_zip_open(zip, n, &z, err, sizeof err) && strstr(err, "encrypted"));
    free(zip);
    /* A backslash name is refused. */
    {
        zmember w[] = { { "assets\\x.decl", "x", 0 } };
        zip = zip_build(w, 1, 0, &n);
        CHECK(!sh_modlegacy_zip_open(zip, n, &z, err, sizeof err));
        free(zip);
    }
}

static void migration_table(void)
{
    char t[600], id[80];
    printf("-- era B to era C mapping table\n");
    CHECK(sh_modlegacy_map_path("package.json", t, sizeof t) == SH_MODLEGACY_PATH_DESCRIPTOR);
    CHECK(sh_modlegacy_map_path("assets/generated/decls/entitydef/x.decl", t, sizeof t) == SH_MODLEGACY_PATH_ASSET &&
          !strcmp(t, "assets/generated/decls/entitydef/x.decl"));
    CHECK(sh_modlegacy_map_path("Assets/cooked/anim/a.bmd6anim", t, sizeof t) == SH_MODLEGACY_PATH_ASSET &&
          !strcmp(t, "assets/cooked/anim/a.bmd6anim"));
    CHECK(sh_modlegacy_map_path("decls/entitydef/monster/imp.decl", t, sizeof t) == SH_MODLEGACY_PATH_ASSET &&
          !strcmp(t, "assets/generated/decls/entitydef/monster/imp.decl"));
    CHECK(sh_modlegacy_map_path("images/models/imp_d.bimage", t, sizeof t) == SH_MODLEGACY_PATH_ASSET &&
          !strcmp(t, "assets/generated/image/models/imp_d.bimage"));
    CHECK(sh_modlegacy_map_path("shaders/generated/spirv/a.vspv", t, sizeof t) == SH_MODLEGACY_PATH_ASSET &&
          !strcmp(t, "assets/generated/spirv/a.vspv"));
    CHECK(sh_modlegacy_map_path("shaders/generated/renderprogs/a.mrpr", t, sizeof t) == SH_MODLEGACY_PATH_ASSET &&
          !strcmp(t, "assets/generated/renderprogs/a.mrpr"));
    CHECK(sh_modlegacy_map_path("resources/cyber.manifest", t, sizeof t) == SH_MODLEGACY_PATH_MANIFEST);
    CHECK(sh_modlegacy_map_path("requirements/cyber.requirements", t, sizeof t) == SH_MODLEGACY_PATH_REQUIREMENTS);
    CHECK(sh_modlegacy_map_path("strings/english.json", t, sizeof t) == SH_MODLEGACY_PATH_STRINGS);
    CHECK(sh_modlegacy_map_path("hud/weapons.json", t, sizeof t) == SH_MODLEGACY_PATH_HUD);
    CHECK(sh_modlegacy_map_path("resources/sub/x.manifest", t, sizeof t) == SH_MODLEGACY_PATH_DROP);
    CHECK(sh_modlegacy_map_path("smpkg.digest", t, sizeof t) == SH_MODLEGACY_PATH_DROP);
    CHECK(sh_modlegacy_map_path("shader_includes/common.inc", t, sizeof t) == SH_MODLEGACY_PATH_DROP);
    CHECK(sh_modlegacy_map_path("readme.txt", t, sizeof t) == SH_MODLEGACY_PATH_DROP);

    CHECK(sh_modlegacy_safe_path("assets/generated/decls/x.decl"));
    CHECK(!sh_modlegacy_safe_path("../x") && !sh_modlegacy_safe_path("a/./b") && !sh_modlegacy_safe_path("/a") &&
          !sh_modlegacy_safe_path("a//b") && !sh_modlegacy_safe_path("a/b.") && !sh_modlegacy_safe_path("c:/x") &&
          !sh_modlegacy_safe_path("a/con.txt") && !sh_modlegacy_safe_path("a/COM1") && !sh_modlegacy_safe_path("a/x?") &&
          !sh_modlegacy_safe_path(""));
    CHECK(sh_modlegacy_safe_path("a/console.decl") && sh_modlegacy_safe_path("a/com10"));

    CHECK(sh_modlegacy_normalize_id("  Local.Cyberdemon \r\n", id, sizeof id) && !strcmp(id, "local.cyberdemon"));
    CHECK(sh_modlegacy_normalize_id("\xef\xbb\xbfmy_pack", id, sizeof id) && !strcmp(id, "my_pack"));
    CHECK(!sh_modlegacy_normalize_id("_leading", id, sizeof id) && !sh_modlegacy_normalize_id("a..b", id, sizeof id));
    CHECK(!sh_modlegacy_normalize_id("delivery.330a75494d7bb0d1fcd2d40bbf2bc9b62bb43de5d1d4c21c2ff5d4d3a3b0f1e2", id, sizeof id));
}

static unsigned char *era_c_zip(size_t *n)
{
    zmember m[] = {
        { "assets/", NULL, 0 },
        { "assets/generated/decls/entitydef/acceptance/r137/speaker_offset.decl", "entityDef speaker_offset { }", 1 },
        { "assets/generated/decls/snapeditorentitydef/acceptance/r137/speaker_offset.decl", "snapEditorEntityDef { }", 0 },
        { "assets/generated/image/mods/acceptance.r137.mixed-rows/icon.bimage", "IMG", 0 },
        { "notes/readme.txt", "not an input", 0 },
        { "package.json", "{\"id\": \"Acceptance.R137.Mixed-Rows\", \"name\": \"R137 mixed-rows\"}", 1 },
    };
    return zip_build(m, 6, 0, n);
}

static void completeness_and_digest(void)
{
    unsigned char *zip;
    size_t zn, mn, plen;
    char *map, reason[256];
    sh_modlegacy_carrier *c = NULL;
    size_t count;
    unsigned char *payload;
    printf("-- completeness and digest\n");
    zip = era_c_zip(&zn);

    map = carrier_map(BASE_MAP, "testpack", zip, zn, 64, -1, -1, NULL, &mn);
    count = sh_modlegacy_collect(map, mn, &c);
    CHECK(count == 1 && c[0].consistent && c[0].present == c[0].header.total && c[0].header.total > 3);
    payload = count ? sh_modlegacy_payload(&c[0], &plen, reason, sizeof reason) : NULL;
    CHECK(payload && plen == zn && !memcmp(payload, zip, zn));
    if (payload) HeapFree(GetProcessHeap(), 0, payload);
    sh_modlegacy_carriers_free(c, count);
    HeapFree(GetProcessHeap(), 0, map);

    map = carrier_map(BASE_MAP, "testpack", zip, zn, 64, 2, -1, NULL, &mn);
    count = sh_modlegacy_collect(map, mn, &c);
    CHECK(count == 1 && c[0].present + 1 == c[0].header.total);
    CHECK(count && !sh_modlegacy_payload(&c[0], &plen, reason, sizeof reason) && strstr(reason, "incomplete"));
    sh_modlegacy_carriers_free(c, count);
    HeapFree(GetProcessHeap(), 0, map);

    map = carrier_map(BASE_MAP, "testpack", zip, zn, 64, -1, 1, NULL, &mn);
    count = sh_modlegacy_collect(map, mn, &c);
    CHECK(count == 1 && !c[0].consistent && strstr(c[0].problem, "appears twice"));
    CHECK(count && !sh_modlegacy_payload(&c[0], &plen, reason, sizeof reason) && strstr(reason, "inconsistent"));
    sh_modlegacy_carriers_free(c, count);
    HeapFree(GetProcessHeap(), 0, map);

    map = carrier_map(BASE_MAP, "testpack", zip, zn, 64, -1, -1, "0000000000000000", &mn);
    count = sh_modlegacy_collect(map, mn, &c);
    CHECK(count == 1 && c[0].consistent);
    CHECK(count && !sh_modlegacy_payload(&c[0], &plen, reason, sizeof reason) && strstr(reason, "digest mismatch"));
    sh_modlegacy_carriers_free(c, count);
    HeapFree(GetProcessHeap(), 0, map);

    /* Two contents under one key are two carriers, each judged alone. */
    {
        char *one = carrier_map(BASE_MAP, "testpack", zip, zn, 4096, -1, -1, NULL, &mn);
        char *two = carrier_map(one, "testpack", zip, zn - 1, 4096, -1, -1, NULL, &mn);
        count = sh_modlegacy_collect(two, mn, &c);
        CHECK(count == 2 && strcmp(c[0].header.digest, c[1].header.digest));
        sh_modlegacy_carriers_free(c, count);
        HeapFree(GetProcessHeap(), 0, one);
        HeapFree(GetProcessHeap(), 0, two);
    }
    free(zip);
}

/* Files a bundle cannot carry are left out, not fatal; a delivery key
 * memoises its conversion across ZIP encodings. */
static void left_out_members(void)
{
    static const zmember m[] = {
        { "package.json", "{\"id\": \"lo.pack\", \"name\": \"Left out\", \"requirements\": {\"cvars\": "
                          "{\"g_useImageBlackList\": 0, \"sv_cheats\": \"1\"}}}", 0 },
        { "assets/generated/decls/entitydef/lo/kept.decl", "{\r\n\tinherit = \"monster/imp\";\r\n}", 1 },
        { "assets/cooked/model/models/lo/door.materials.txt", "textures/door\n", 0 },
        { "assets/generated/decls/entitydef/lo/empty.decl", "", 0 },
    };
    static const char KEY[] = "delivery.1111111111111111111111111111111111111111111111111111111111111111";
    unsigned char *zip, *zip2;
    size_t zn, zn2, mn, on = 0;
    char *map, *out, path[MAX_PATH * 2], *text;
    sh_modlegacy_verdict v[2], first;
    int builds;
    printf("-- members a bundle cannot carry are left out; a delivery key memoises across encodings\n");
    sh_modlegacy_reset();
    reset_counts();
    zip = zip_build(m, 4, 0, &zn);
    map = carrier_map(BASE_MAP, KEY, zip, zn, 8192, -1, -1, NULL, &mn);
    g_build_calls = 0;
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && sh_modlegacy_last_report(v, 2) == 1 && v[0].verdict == SH_MODLEGACY_CONVERT);
    CHECK(v[0].files == 1 && v[0].skipped == 2 && strstr(v[0].first_skipped, "materials.txt"));
    CHECK(file_exists_at(v[0].folder, "assets/generated/decls/entitydef/lo/kept.decl") &&
          !file_exists_at(v[0].folder, "assets/cooked/model/models/lo/door.materials.txt") &&
          !file_exists_at(v[0].folder, "assets/generated/decls/entitydef/lo/empty.decl"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", v[0].folder);
    text = read_all(path, NULL);
    CHECK(text && strstr(text, "\"g_useImageBlackList\": \"0\"") && !strstr(text, "sv_cheats"));
    free(text);
    first = v[0];
    if (out) HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, map);
    /* The same tree in another ZIP (the decl stored instead of deflated): a new
     * digest, the same key; a fresh process finds the folder by its key. */
    {
        zmember m2[4];
        memcpy(m2, m, sizeof m2);
        m2[1].deflate = 0;
        zip2 = zip_build(m2, 4, 0, &zn2);
    }
    sh_modlegacy_reset();
    builds = g_build_calls;
    map = carrier_map(BASE_MAP, KEY, zip2, zn2, 8192, -1, -1, NULL, &mn);
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && sh_modlegacy_last_report(v, 2) == 1 && v[0].verdict == SH_MODLEGACY_CONVERT &&
          strcmp(v[0].digest, first.digest) && !strcmp(v[0].folder, first.folder) && !strcmp(v[0].bundle, first.bundle));
    CHECK(g_build_calls == builds);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, map);
    free(zip);
    free(zip2);
}

static void convert_era_c(void)
{
    unsigned char *zip;
    size_t zn, mn, on = 0;
    char *map, *out, folder[MAX_PATH], digest[17];
    sh_modlegacy_verdict v[4];
    unsigned count = 0;
    int rescans;
    printf("-- era C package converts\n");
    sh_modlegacy_reset();
    reset_counts();
    zip = era_c_zip(&zn);
    sh_shard_digest16(zip, zn, digest);
    map = carrier_map(BASE_MAP, "delivery.c6918a65f2ddbf6b25ebff1564fcf3bc26de6ade3bfc89ba2a472a6790a547ed",
                      zip, zn, 8192, -1, -1, NULL, &mn);
    g_build_calls = 0;
    rescans = g_mod_stub_rescans;
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out != NULL && g_legacy_lines == 1 && g_convert_lines == 1 && g_build_calls == 1);
    /* The converted bundle is not indexed yet: the serving index is rebuilt
     * once so the activation later in the same parse can see it. */
    CHECK(g_mod_stub_rescans == rescans + 1);
    CHECK(sh_modlegacy_last_report(v, 4) == 1 && v[0].verdict == SH_MODLEGACY_CONVERT && v[0].layout == 'C');
    CHECK(!strcmp(v[0].id, "acceptance.r137.mixed-rows") && !strncmp(v[0].version, "legacy-", 7) &&
          !strcmp(v[0].version + 7, digest) && v[0].key_form == SH_LEGACY_KEY_DELIVERY && v[0].files == 3);
    CHECK(v[0].shadows == 2);   /* the namespaced icon is not a shadow */
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\mods\\acceptance.r137.mixed-rows", g_root);
    (void)digest;
    CHECK(!_stricmp(v[0].folder, folder));
    CHECK(file_exists_at(folder, "mod.json") && !file_exists_at(folder, "bundle.json"));
    CHECK(file_exists_at(folder, "assets/generated/decls/entitydef/acceptance/r137/speaker_offset.decl"));
    CHECK(!file_exists_at(folder, "notes/readme.txt") && !file_exists_at(folder, "package.json"));
    {
        char path[MAX_PATH * 2], *text;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", folder);
        text = read_all(path, NULL);
        CHECK(text && strstr(text, "\"id\": \"acceptance.r137.mixed-rows\"") && strstr(text, "\"name\": \"R137 mixed-rows\""));
        CHECK(text && strstr(text, "\"shadows\": [\"generated/decls/entitydef/acceptance/r137/speaker_offset.decl\""));
        CHECK(text && strstr(text, "\"keyForm\": \"delivery\"") && strstr(text, "\"layout\": \"C\""));
        free(text);
    }
    if (out) {
        sh_modmanifest_list rows = {0};
        CHECK(!strstr(out, "\"smpkg.") && count_rows(out, "\"smpmod1.") == 1);
        CHECK(string_rows(out) == 2 && alloc4(out, &count) && count == 2);
        CHECK(strstr(out, "door_label") < strstr(out, "\"smpmod1.0.1\""));
        CHECK(sh_modmanifest_read(out, on, &rows, NULL) == 1 && rows.count == 1 && !strcmp(rows.rows[0].bundle, v[0].bundle));
        /* names: the placed inherit the bundle provides, nothing else */
        CHECK(rows.count == 1 && rows.rows[0].name_count == 1 &&
              !strcmp(rows.rows[0].names[0], "acceptance/r137/speaker_offset"));
        sh_modmanifest_list_free(&rows);
        HeapFree(GetProcessHeap(), 0, out);
    }

    /* The same map again: the memo answers, nothing is rebuilt. */
    reset_counts();
    g_build_calls = 0;
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && g_memo_lines == 1 && g_build_calls == 0);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    /* A new session: the installed folder is reused, nothing is rebuilt. */
    sh_modlegacy_reset();
    reset_counts();
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && g_convert_lines == 1 && g_build_calls == 0 && count_rows(out, "\"smpmod1.") == 1);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    /* A map that already names the bundle keeps one row for it. */
    {
        size_t n2 = 0;
        char *first = sh_modlegacy_process(map, mn, &on);
        char *mixed = first ? carrier_map(first, "delivery.c6918a65f2ddbf6b25ebff1564fcf3bc26de6ade3bfc89ba2a472a6790a547ed",
                                          zip, zn, 8192, -1, -1, NULL, &n2) : NULL;
        char *again = mixed ? sh_modlegacy_process(mixed, n2, &on) : NULL;
        CHECK(again && count_rows(again, "\"smpmod1.") == 1 && !strstr(again, "\"smpkg."));
        if (first) HeapFree(GetProcessHeap(), 0, first);
        if (mixed) HeapFree(GetProcessHeap(), 0, mixed);
        if (again) HeapFree(GetProcessHeap(), 0, again);
    }
    HeapFree(GetProcessHeap(), 0, map);
    free(zip);
}

static void convert_era_b(void)
{
    zmember m[] = {
        { "package.json", "{\"schema\": \"snapmap-plus.override-package.v1\", \"name\": \"\", \"version\": \"1.0.0\", "
                          "\"description\": \"old pack\", \"priority\": 3}", 0 },
        { "decls/entitydef/testpack/foo.decl", "entityDef foo { }", 1 },
        { "images/testpack/foo_d.bimage", "IMG", 0 },
        { "shaders/generated/spirv/testpack/a.vspv", "SPV", 0 },
        { "shaders/generated/renderprogs/testpack/a.mrpr", "RP", 0 },
        { "requirements/foo.requirements", "# gates\ncvar\tg_useResourceBlackList\t0\ncvar\tsv_cheats\t1\n", 0 },
        { "strings/english.json", "{\"testpack_title\": \"Foo\"}", 0 },
        { "hud/weapons.json", "{\"schema\": \"snapmap-plus.weapon-hud.v1\", \"weapons\": "
                              "{\"weapon/testpack/gun\": {\"ammo_display\": \"engine\"}}}", 0 },
        { "resources/foo.manifest", "baseModel\tmd6/testpack/foo.md6mesh\tgenerated/basemodel/testpack/foo.bmd6model\n", 0 },
        { "generated/basemodel/testpack/foo.bmd6model", "MESH", 0 },
        { "smpkg.digest", "0123456789abcdef", 0 },
        { "readme.txt", "hi", 0 },
    };
    unsigned char *zip;
    size_t zn, mn, on = 0;
    char *map, *out, folder[MAX_PATH], digest[17];
    sh_modlegacy_verdict v[2];
    printf("-- era B package migrates and converts\n");
    sh_modlegacy_reset();
    reset_counts();
    zip = zip_build(m, sizeof m / sizeof m[0], 0, &zn);
    sh_shard_digest16(zip, zn, digest);
    map = carrier_map(BASE_MAP, "testpack", zip, zn, 100, -1, -1, NULL, &mn);
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && g_convert_lines == 1);
    CHECK(sh_modlegacy_last_report(v, 2) == 1 && v[0].verdict == SH_MODLEGACY_CONVERT && v[0].layout == 'B' &&
          !strcmp(v[0].id, "testpack") && v[0].key_form == SH_LEGACY_KEY_FOLDER && v[0].files == 5);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\mods\\testpack", g_root);
    CHECK(file_exists_at(folder, "assets/generated/decls/entitydef/testpack/foo.decl"));
    CHECK(file_exists_at(folder, "assets/generated/image/testpack/foo_d.bimage"));
    CHECK(file_exists_at(folder, "assets/generated/spirv/testpack/a.vspv"));
    CHECK(file_exists_at(folder, "assets/generated/renderprogs/testpack/a.mrpr"));
    CHECK(file_exists_at(folder, "assets/generated/basemodel/testpack/foo.bmd6model"));   /* imported by the manifest */
    CHECK(!file_exists_at(folder, "smpkg.digest") && !file_exists_at(folder, "readme.txt") &&
          !file_exists_at(folder, "requirements/foo.requirements") && !file_exists_at(folder, "decls/entitydef/testpack/foo.decl"));
    {
        char path[MAX_PATH * 2], *text;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", folder);
        text = read_all(path, NULL);
        CHECK(text && strstr(text, "\"id\": \"testpack\"") && strstr(text, "\"name\": \"testpack\""));
        CHECK(text && strstr(text, "\"g_useResourceBlackList\": \"0\"") && !strstr(text, "sv_cheats"));
        CHECK(text && strstr(text, "\"testpack_title\": \"Foo\""));
        CHECK(text && strstr(text, "\"weapon/testpack/gun\""));
        CHECK(text && strstr(text, "\"description\": \"old pack\"") && !strstr(text, "priority") &&
              !strstr(text, "override-package") && strstr(text, "\"layout\": \"B\""));
        free(text);
    }
    /* The placed edit value names the migrated decl: it is the row's name. */
    if (out) {
        sh_modmanifest_list rows = {0};
        CHECK(sh_modmanifest_read(out, on, &rows, NULL) == 1 && rows.count == 1 && rows.rows[0].name_count == 1 &&
              !strcmp(rows.rows[0].names[0], "testpack/foo"));
        sh_modmanifest_list_free(&rows);
        HeapFree(GetProcessHeap(), 0, out);
    }
    HeapFree(GetProcessHeap(), 0, map);
    free(zip);
}

static void clean_verdicts(void)
{
    unsigned char *zip;
    size_t zn, mn, on = 0;
    char *map, *out;
    sh_modlegacy_verdict v[4];
    unsigned count = 0;
    printf("-- cleaned carriers: verdicts and logging\n");
    zip = era_c_zip(&zn);

    /* Digest mismatch. */
    sh_modlegacy_reset();
    reset_counts();
    map = carrier_map(BASE_MAP, "local.broken", zip, zn, 64, -1, -1, "00000000000000ff", &mn);
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && g_legacy_lines == 1 && g_clean_lines == 1 && strstr(g_last_clean, "digest mismatch"));
    CHECK(strstr(g_last_clean, "smpkg.local.broken digest 00000000000000ff") &&
          strstr(g_last_clean, "descriptor-id key (beta.14-15)"));
    CHECK(out && !strstr(out, "\"smpkg.") && !strstr(out, "smpmod1.") && string_rows(out) == 1 &&
          alloc4(out, &count) && count == 1);
    CHECK(sh_modlegacy_last_report(v, 4) == 1 && v[0].verdict == SH_MODLEGACY_CLEAN && strstr(v[0].reason, "digest"));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, map);

    /* Incomplete (the beta.6-7 overflow left gaps), plus a second complete carrier: one each. */
    sh_modlegacy_reset();
    reset_counts();
    {
        char *one = carrier_map(BASE_MAP, "gappy", zip, zn, 64, 3, -1, NULL, &mn);
        map = carrier_map(one, "local.ok", zip, zn, 4096, -1, -1, NULL, &mn);
        HeapFree(GetProcessHeap(), 0, one);
    }
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && g_legacy_lines == 2 && g_clean_lines == 1 && g_convert_lines == 1 && strstr(g_last_clean, "incomplete"));
    CHECK(sh_modlegacy_last_report(v, 4) == 2);
    CHECK(out && !strstr(out, "\"smpkg.") && count_rows(out, "\"smpmod1.") == 1);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, map);

    /* Valid digest over bytes that are not a ZIP. */
    sh_modlegacy_reset();
    reset_counts();
    {
        static const unsigned char junk[] = "these bytes hash correctly but are not a package";
        map = carrier_map(BASE_MAP, "junk", junk, sizeof junk - 1, 64, -1, -1, NULL, &mn);
    }
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && g_clean_lines == 1 && strstr(g_last_clean, "undecodable ZIP"));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, map);

    /* The builder refuses: cleaned, the unpacked tree is left for the author, the reason names it. */
    sh_modlegacy_reset();
    reset_counts();
    g_build_refuse = 1;
    {
        zmember m[] = { { "package.json", "{\"id\":\"local.refused\",\"name\":\"R\"}", 0 },
                        { "assets/generated/decls/entitydef/r.decl", "x", 0 } };
        unsigned char *z2;
        size_t z2n;
        z2 = zip_build(m, 2, 0, &z2n);
        map = carrier_map(BASE_MAP, "local.refused", z2, z2n, 4096, -1, -1, NULL, &mn);
        free(z2);
    }
    out = sh_modlegacy_process(map, mn, &on);
    g_build_refuse = 0;
    CHECK(out && g_clean_lines == 1 && strstr(g_last_clean, "test builder refuses this folder"));
    CHECK(sh_modlegacy_last_report(v, 4) == 1 && v[0].folder[0] && file_exists_at(v[0].folder, "mod.json"));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, map);

    /* No package.json at the root. */
    sh_modlegacy_reset();
    reset_counts();
    {
        zmember m[] = { { "assets/generated/decls/entitydef/r.decl", "x", 0 } };
        unsigned char *z2;
        size_t z2n;
        z2 = zip_build(m, 1, 0, &z2n);
        map = carrier_map(BASE_MAP, "nodesc", z2, z2n, 4096, -1, -1, NULL, &mn);
        free(z2);
    }
    out = sh_modlegacy_process(map, mn, &on);
    CHECK(out && g_clean_lines == 1 && strstr(g_last_clean, "no package.json"));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, map);
    free(zip);
}

static void unreleased_rows(void)
{
    static const char map[] =
        "{\"variables\":{\"allocCount\":[0,0,0,0,3],\"string\":["
        VAR("smpbind.0.1.2cf482ed5e4749cc", "U01QQklORAIB") ","
        VAR("door_label", "Open") ","
        VAR("smpref.delivery.437af66e107ad5b5c9469edf8656cb5043aa2201492d9b460b3a7cd9f0bc2222.1e6351e40eb33760", "eyJpZCI6")
        "]}}";
    size_t on = 0;
    char *out;
    unsigned count = 0;
    printf("-- unreleased smpbind./smpref. rows are stripped silently\n");
    sh_modlegacy_reset();
    reset_counts();
    out = sh_modlegacy_process(map, sizeof map - 1, &on);
    CHECK(out && g_legacy_lines == 0 && sh_modlegacy_last_report(NULL, 0) == 0);
    CHECK(out && !strstr(out, "smpbind.") && !strstr(out, "smpref.") && strstr(out, "door_label") &&
          string_rows(out) == 1 && alloc4(out, &count) && count == 1);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    CHECK(sh_modlegacy_process(BASE_MAP, sizeof BASE_MAP - 1, &on) == NULL);
}

static void fixtures(void)
{
    static const struct { const char *file; unsigned carriers; } maps[] = {
        { "carrier-1-shard_map.decl", 1 },
        { "carrier-21-shards_map.decl", 1 },
        { "carrier-1054-shards_map.decl", 2 },
    };
    char dir[MAX_PATH], path[MAX_PATH * 2];
    size_t i, k;
    DWORD got = GetEnvironmentVariableA("SH_MODLEGACY_FIXTURES", dir, sizeof dir);
    printf("-- integration over the carrier maps\n");
    if (!got || got >= sizeof dir) {
        printf("  [skip] SH_MODLEGACY_FIXTURES does not name the carrier maps' folder\n");
        return;
    }
    sh_modlegacy_reset();
    /* A data root of its own: the synthetic carriers above reuse a real
     * delivery key, and a delivery key memoises its conversion. */
    {
        char own[MAX_PATH];
        _snprintf_s(own, sizeof own, _TRUNCATE, "%s\\fixtures", g_root);
        CreateDirectoryA(own, NULL);
        sh_modlegacy_set_data_root(own);
    }
    g_echo = 1;
    for (i = 0; i < sizeof maps / sizeof maps[0]; i++) {
        size_t n = 0, on = 0;
        char *text, *out;
        sh_modlegacy_verdict v[8];
        size_t verdicts;
        unsigned converted = 0, cleaned = 0, count = 0;
        ULONGLONG t0;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, maps[i].file);
        text = read_all(path, &n);
        if (!text) { printf("  [skip] %s is not present\n", maps[i].file); continue; }
        reset_counts();
        t0 = GetTickCount64();
        out = sh_modlegacy_process(text, n, &on);
        verdicts = sh_modlegacy_last_report(v, 8);
        for (k = 0; k < verdicts && k < 8; k++) {
            if (v[k].verdict == SH_MODLEGACY_CONVERT) converted++; else cleaned++;
        }
        printf("  %s: %zu bytes -> %zu, %zu carrier(s): %u converted, %u cleaned, %llu ms\n", maps[i].file, n, on,
               verdicts, converted, cleaned, (unsigned long long)(GetTickCount64() - t0));
        CHECK(verdicts == maps[i].carriers && converted == maps[i].carriers && g_legacy_lines == (int)maps[i].carriers);
        CHECK(out != NULL);
        if (out) {
            CHECK(!strstr(out, "\"smpkg.") && !strstr(out, "\"smpbind.") && !strstr(out, "\"smpref."));
            CHECK(count_rows(out, "\"smpmod1.") == converted);
            CHECK(alloc4(out, &count) && count == string_rows(out));
            /* What the engine then parses: the manifest step keeps the rows and strips them. */
            {
                size_t en = 0;
                sh_modmanifest_list current = {0};
                char *engine = sh_modmanifest_prepare_load(out, on, &en);
                CHECK(engine && !strstr(engine, "smpmod1.") && alloc4(engine, &count) && count == string_rows(engine));
                CHECK(sh_modmanifest_current(&current) == converted);
                sh_modmanifest_list_free(&current);
                if (engine) HeapFree(GetProcessHeap(), 0, engine);
            }
            HeapFree(GetProcessHeap(), 0, out);
        }
        free(text);
    }
    g_echo = 0;
    sh_modlegacy_set_data_root(g_root);
}

int main(void)
{
    char tmp[MAX_PATH];
    printf("modlegacy_test\n");
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_root, sizeof g_root, _TRUNCATE, "%ssmp_modlegacy_test_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryA(g_root, NULL);
    sh_modlegacy_set_data_root(g_root);

    header_grammar_per_era();
    zip_reading();
    migration_table();
    completeness_and_digest();
    convert_era_c();
    convert_era_b();
    left_out_members();
    clean_verdicts();
    unreleased_rows();
    fixtures();

    sh_modlegacy_reset();
    sh_modlegacy_set_data_root(NULL);
    remove_tree(g_root);
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
