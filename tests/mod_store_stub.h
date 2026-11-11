/* mod_store_stub.h -- an in-memory mod store on 127.0.0.1 for the store
 * client tests (modnet_test, modsync_test and the flows above them).
 *
 * A small Winsock HTTP/1.1 server on a thread, one request per connection
 * (Connection: close), implementing the routes of the Worker
 * (snapmap-web api/mods_store.js, ONE-SHAPE rule 5) the client uses:
 * HEAD, PUT and GET /mods/build/<hash>, the upload in parts (POST
 * /mods/build/<hash>/parts, PUT .../parts/<n> with X-Build-Size, POST
 * /mods/build/<hash>/complete), POST /mods/claim and /mods/release. It checks
 * what the Worker checks that the flows can reach: Content-Type
 * application/zip on a build PUT (415), Content-Length present (411), one
 * request at most one part (413), parts of exactly the part size but the
 * last, and the archive itself (422): a ZIP the product's reader opens,
 * holding mod.json whose build is the path's hash and one assets/<request>
 * entry per carried file its block lists, nothing else. GET and HEAD say how
 * many scripts a build carries (X-Mod-Scripts). A claim names stored builds
 * (409), any number of them; a re-claim needs the secret (403), a release of
 * an unknown claim is 404; a release deletes every build no live claim names
 * any more. Counters and fault switches let the tests observe the exchange
 * and inject failures.
 *
 * Requires ws2_32.lib, modzip.c (and raw_deflate.c) and modbundle.c. */
#ifndef SNAPMAP_PLUS_MOD_STORE_STUB_H
#define SNAPMAP_PLUS_MOD_STORE_STUB_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"
#include "../src/backend/modzip.h"

#pragma comment(lib, "ws2_32.lib")

#define STUB_MAX_BODY    (100u * 1000u * 1000u)          /* the platform's request body limit */
#define STUB_PART_SIZE   (95ull * 1024u * 1024u)          /* the Worker's PART_SIZE */

typedef struct stub_obj { char hash[65]; unsigned char *bytes; size_t len; unsigned scripts; } stub_obj;
typedef struct stub_claim { char id[33], ref[260], secret[65]; char (*builds)[65]; int nb; int live; } stub_claim;
/* An upload in parts: the archive being assembled and which parts arrived. */
typedef struct stub_upload { char hash[65]; unsigned long long size; unsigned parts; unsigned char *bytes, *have; } stub_upload;

typedef struct stub_state {
    SOCKET listener;
    HANDLE thread;
    int port;
    volatile LONG stop;
    CRITICAL_SECTION cs;
    stub_obj *builds;
    int nbuilds, cap_builds;
    stub_claim *claims;
    int nclaims, cap_claims;
    stub_upload upload;           /* one upload in parts at a time */
    unsigned counter;
    /* counters */
    int requests, head_calls, build_puts, build_created, build_gets, claim_calls, claim_with_secret, release_calls,
        builds_deleted, part_starts, part_puts, part_completes;
    int part_sent[64];            /* how often each part (1-based, up to 63) was sent */
    /* the last request with a body */
    int last_cl_headers;          /* Content-Length headers seen */
    long long last_cl_value;
    char last_method[8], last_path[160], last_ctype[64];
    char last_refusal[200];       /* the last 422's error text */
    /* faults */
    char fail_prefix[48];         /* the next request whose "METHOD path" starts with this ... */
    int fail_status;              /* ... gets this status and a JSON error */
    int corrupt_build_gets;       /* GET build answers with one byte flipped in the middle */
    int fail_part;                /* the next PUT of this part number answers 503 */
    char trail[8192];             /* "claim <ref>" and "release <ref>" lines, in the order the stub took them */
    unsigned scripts_header;      /* X-Mod-Scripts of the next answer */
} stub_state;

static stub_state g_stub;

/* One trail line; a full trail keeps its last lines out (tests clear it first). */
static void stub_trail(const char *verb, const char *ref)
{
    size_t used = strlen(g_stub.trail);
    if (used + strlen(verb) + strlen(ref) + 3 < sizeof g_stub.trail)
        _snprintf_s(g_stub.trail + used, sizeof g_stub.trail - used, _TRUNCATE, "%s %s\n", verb, ref);
}

static stub_obj *stub_find(stub_obj *v, int n, const char *hash)
{
    int i;
    for (i = 0; i < n; i++) if (strcmp(v[i].hash, hash) == 0) return &v[i];
    return NULL;
}

static int stub_is_hex(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

/* Every quoted 64-hex string in the array following "key":[ ; *out
 * (malloc'd) receives them. */
static int stub_hex_list(const char *body, const char *key, char (**outp)[65])
{
    char (*out)[65];
    int max;
    char needle[64];
    const char *p, *end;
    int n = 0;
    _snprintf_s(needle, sizeof needle, _TRUNCATE, "\"%s\":[", key);
    *outp = NULL;
    p = strstr(body, needle);
    if (!p) return -1;
    p += strlen(needle);
    end = strchr(p, ']');
    if (!end) return -1;
    max = (int)((end - p) / 66) + 1;
    out = (char (*)[65])calloc((size_t)max, 65);
    if (!out) return -1;
    *outp = out;
    while (p < end) {
        const char *q = strchr(p, '"');
        if (!q || q >= end) break;
        if (q + 66 <= end && q[65] == '"' && stub_is_hex(q + 1, 64)) {
            if (n < max) { memcpy(out[n], q + 1, 64); out[n][64] = 0; }
            n++;
            p = q + 66;
        } else { free(out); *outp = NULL; return -1; }
        while (p < end && (*p == ',' || *p == ' ')) p++;
    }
    return n;
}

/* A plain string member (no escapes in the tests' values). */
static int stub_str(const char *body, const char *key, char *out, size_t cap)
{
    char needle[64];
    const char *p, *q;
    _snprintf_s(needle, sizeof needle, _TRUNCATE, "\"%s\":\"", key);
    p = strstr(body, needle);
    if (!p) return 0;
    p += strlen(needle);
    q = strchr(p, '"');
    if (!q || (size_t)(q - p) >= cap) return 0;
    memcpy(out, p, (size_t)(q - p));
    out[q - p] = 0;
    return 1;
}

static void stub_hex_of(unsigned v, const char *salt, char *out, size_t chars)
{
    char seed[96], h[65];
    _snprintf_s(seed, sizeof seed, _TRUNCATE, "%s-%u-%lu", salt, v, GetTickCount());
    sh_mb_sha256_hex(seed, strlen(seed), h);
    memcpy(out, h, chars);
    out[chars] = 0;
}

static void stub_send_ex(SOCKET s, int status, const char *ctype, const void *body, size_t len, int head_only)
{
    char head[256];
    const char *reason = status == 200 ? "OK" : status == 201 ? "Created" : status == 204 ? "No Content" : "Status";
    int n = _snprintf_s(head, sizeof head, _TRUNCATE,
                        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nX-Mod-Scripts: %u\r\nConnection: close\r\n\r\n",
                        status, reason, ctype ? ctype : "application/json", len, g_stub.scripts_header);
    g_stub.scripts_header = 0;
    send(s, head, n, 0);
    if (head_only) return;
    while (len) {
        int chunk = (int)(len > (1u << 20) ? (1u << 20) : len);
        int sent = send(s, (const char *)body, chunk, 0);
        if (sent <= 0) break;
        body = (const char *)body + sent;
        len -= (size_t)sent;
    }
}

static void stub_send(SOCKET s, int status, const char *ctype, const void *body, size_t len)
{
    stub_send_ex(s, status, ctype, body, len, 0);
}

static void stub_json(SOCKET s, int status, const char *json)
{
    stub_send(s, status, "application/json", json, strlen(json));
}

/* The Worker's archive check, on the product's own reader: NULL when the
 * archive is the build `hash`, else the first cause (static text). */
static const char *stub_check_archive(const char *hash, const unsigned char *body, size_t len, char *why, size_t cap,
                                      unsigned *scripts)
{
    char path[MAX_PATH], dir[MAX_PATH], cause[256];
    sh_zip_reader zr;
    sh_mb_bundle b;
    unsigned char *text;
    size_t tlen = 0, i, carried = 0;
    FILE *f;
    long idx;
    const char *bad = NULL;
    GetTempPathA(sizeof dir, dir);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%sstub-archive-%lu.zip", dir, GetCurrentProcessId());
    if (fopen_s(&f, path, "wb") || !f) return "the stub cannot stage the archive";
    fwrite(body, 1, len, f);
    fclose(f);
    memset(&b, 0, sizeof b);
    if (!sh_zip_reader_open(&zr, path, cause, sizeof cause)) {
        DeleteFileA(path);
        _snprintf_s(why, cap, _TRUNCATE, "not a stored ZIP: %s", cause);
        return why;
    }
    idx = sh_zip_reader_find(&zr, "mod.json");
    *scripts = 0;
    text = idx < 0 ? NULL : sh_zip_reader_read(&zr, idx, (size_t)-1, &tlen, cause, sizeof cause);
    if (!text) bad = "the archive has no mod.json";
    else if (!sh_mb_load_text((const char *)text, tlen, &b)) {
        _snprintf_s(why, cap, _TRUNCATE, "mod.json: %s", b.first_cause);
        bad = why;
    } else if (strcmp(b.hash, hash) != 0) bad = "mod.json build does not match the path";
    for (i = 0; !bad && i < b.file_count; i++) {
        char entry[512];
        if (b.files[i].stock) continue;
        carried++;
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "assets/%s", b.files[i].request);
        if (sh_zip_reader_find(&zr, entry) < 0) bad = "a file mod.json lists is not in the archive";
    }
    if (!bad && zr.n != carried + 1) bad = "the archive holds entries mod.json does not list";
    if (!bad) *scripts = b.script_files;
    free(text);
    sh_mb_free(&b);
    sh_zip_reader_close(&zr);
    DeleteFileA(path);
    return bad;
}

/* Builds no live claim names are deleted at once (release). */
static void stub_drop_unclaimed(void)
{
    int i = 0;
    while (i < g_stub.nbuilds) {
        int c, k, held = 0;
        for (c = 0; c < g_stub.nclaims && !held; c++)
            for (k = 0; g_stub.claims[c].live && k < g_stub.claims[c].nb && !held; k++)
                held = !strcmp(g_stub.claims[c].builds[k], g_stub.builds[i].hash);
        if (held) { i++; continue; }
        free(g_stub.builds[i].bytes);
        g_stub.builds[i] = g_stub.builds[--g_stub.nbuilds];
        g_stub.builds_deleted++;
    }
}

/* A build object, the table grown as needed. */
static stub_obj *stub_new_build(const char *hash)
{
    stub_obj *o;
    if (g_stub.nbuilds == g_stub.cap_builds) {
        int cap = g_stub.cap_builds ? g_stub.cap_builds * 2 : 64;
        stub_obj *grown = (stub_obj *)realloc(g_stub.builds, sizeof *grown * (size_t)cap);
        if (!grown) return NULL;
        g_stub.builds = grown;
        g_stub.cap_builds = cap;
    }
    o = &g_stub.builds[g_stub.nbuilds++];
    memset(o, 0, sizeof *o);
    strcpy_s(o->hash, sizeof o->hash, hash);
    return o;
}

static void stub_upload_free(void)
{
    free(g_stub.upload.bytes);
    free(g_stub.upload.have);
    memset(&g_stub.upload, 0, sizeof g_stub.upload);
}

/* The size an upload in parts names: "size":N in a body. */
static unsigned long long stub_size_of(const char *body)
{
    const char *p = strstr(body, "\"size\":");
    return p ? _strtoui64(p + 7, NULL, 10) : 0;
}

/* Checks and stores a whole archive; answers 201 or 422. */
static void stub_store(SOCKET s, const char *hash, const unsigned char *body, size_t blen)
{
    char out[512], why[300];
    unsigned scripts = 0;
    const char *bad = stub_check_archive(hash, body, blen, why, sizeof why, &scripts);
    stub_obj *o;
    if (bad) {
        strncpy_s(g_stub.last_refusal, sizeof g_stub.last_refusal, bad, _TRUNCATE);
        _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"%s\"}", bad);
        stub_json(s, 422, out);
        return;
    }
    o = stub_new_build(hash);
    if (!o) { stub_json(s, 503, "{\"error\":\"the stub ran out of memory\"}"); return; }
    o->bytes = (unsigned char *)malloc(blen ? blen : 1);
    if (blen) memcpy(o->bytes, body, blen);
    o->len = blen;
    o->scripts = scripts;
    g_stub.build_created++;
    _snprintf_s(out, sizeof out, _TRUNCATE, "{\"build\":\"%s\",\"size\":%zu,\"scripts\":%u}", hash, blen, scripts);
    stub_json(s, 201, out);
}

/* The state of the upload in parts, as the Worker answers it. */
static void stub_parts_state(SOCKET s, int status)
{
    char out[1024];
    unsigned i;
    size_t n = (size_t)_snprintf_s(out, sizeof out, _TRUNCATE, "{\"build\":\"%s\",\"size\":%llu,\"partSize\":%llu,\"parts\":%u,\"have\":[",
                                   g_stub.upload.hash, g_stub.upload.size, STUB_PART_SIZE, g_stub.upload.parts);
    for (i = 0; i < g_stub.upload.parts; i++)
        if (g_stub.upload.have[i]) n += (size_t)_snprintf_s(out + n, sizeof out - n, _TRUNCATE, "%s%u", out[n - 1] == '[' ? "" : ",", i + 1);
    _snprintf_s(out + n, sizeof out - n, _TRUNCATE, "]}");
    stub_json(s, status, out);
}

static void stub_parts(SOCKET s, const char *method, const char *hash, const char *rest, const char *ctype,
                       unsigned long long build_size, const unsigned char *body, size_t blen)
{
    stub_upload *u = &g_stub.upload;
    char out[512];
    stub_obj *o = stub_find(g_stub.builds, g_stub.nbuilds, hash);
    if (!strcmp(rest, "/parts") && !strcmp(method, "POST")) {
        unsigned long long size = stub_size_of((const char *)body);
        g_stub.part_starts++;
        if (size < 22) { stub_json(s, 400, "{\"error\":\"The upload names no archive size.\"}"); return; }
        if (o) {
            _snprintf_s(out, sizeof out, _TRUNCATE, "{\"build\":\"%s\",\"size\":%zu,\"scripts\":%u,\"stored\":true}", hash,
                        o->len, o->scripts);
            stub_json(s, 200, out);
            return;
        }
        if (u->bytes && !strcmp(u->hash, hash) && u->size == size) { stub_parts_state(s, 200); return; }
        stub_upload_free();
        strcpy_s(u->hash, sizeof u->hash, hash);
        u->size = size;
        u->parts = (unsigned)((size + STUB_PART_SIZE - 1) / STUB_PART_SIZE);
        u->bytes = (unsigned char *)malloc((size_t)size);
        u->have = (unsigned char *)calloc(u->parts, 1);
        if (!u->bytes || !u->have) { stub_upload_free(); stub_json(s, 503, "{\"error\":\"the stub ran out of memory\"}"); return; }
        stub_parts_state(s, 201);
        return;
    }
    if (!strncmp(rest, "/parts/", 7) && !strcmp(method, "PUT")) {
        unsigned n = (unsigned)strtoul(rest + 7, NULL, 10);
        unsigned long long at, len;
        g_stub.part_puts++;
        if (_stricmp(ctype, "application/octet-stream") != 0) { stub_json(s, 415, "{\"error\":\"A part must be sent as application/octet-stream.\"}"); return; }
        if (!u->bytes || strcmp(u->hash, hash) || u->size != build_size) {
            stub_json(s, 404, "{\"error\":\"The mod store has no upload of this build in progress; start it again.\"}");
            return;
        }
        if (n < 1 || n > u->parts) { stub_json(s, 400, "{\"error\":\"The upload has no part with that number.\"}"); return; }
        if (n < 64) g_stub.part_sent[n]++;
        if (g_stub.fail_part == (int)n) { g_stub.fail_part = 0; stub_json(s, 503, "{\"error\":\"injected failure 503\"}"); return; }
        at = STUB_PART_SIZE * (n - 1);
        len = n < u->parts ? STUB_PART_SIZE : u->size - at;
        if (blen != len) { stub_json(s, 400, "{\"error\":\"The part is not the length its place in the archive needs.\"}"); return; }
        memcpy(u->bytes + at, body, blen);
        u->have[n - 1] = 1;
        stub_parts_state(s, 200);
        return;
    }
    if (!strcmp(rest, "/complete") && !strcmp(method, "POST")) {
        unsigned long long size = stub_size_of((const char *)body);
        unsigned i;
        g_stub.part_completes++;
        if (!u->bytes || strcmp(u->hash, hash) || u->size != size) {
            if (o) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"build\":\"%s\",\"size\":%zu,\"scripts\":%u}", hash, o->len, o->scripts);
                stub_json(s, 200, out);
                return;
            }
            stub_json(s, 404, "{\"error\":\"The mod store has no upload of this build in progress; start it again.\"}");
            return;
        }
        for (i = 0; i < u->parts; i++)
            if (!u->have[i]) { stub_parts_state(s, 409); return; }
        stub_store(s, hash, u->bytes, (size_t)u->size);
        stub_upload_free();
        return;
    }
    stub_json(s, 405, "{\"error\":\"This address of the mod store does not take that request.\"}");
}

static void stub_route(SOCKET s, const char *method, const char *path, const char *ctype, int cl_headers,
                       unsigned long long build_size, const unsigned char *body, size_t blen)
{
    char out[4096], probe[48];
    _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s %s", method, path);
    if (g_stub.fail_prefix[0] && strncmp(probe, g_stub.fail_prefix, strlen(g_stub.fail_prefix)) == 0) {
        int st = g_stub.fail_status;
        g_stub.fail_prefix[0] = 0;
        _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"injected failure %d\"}", st);
        stub_send_ex(s, st, "application/json", out, strlen(out), !strcmp(method, "HEAD"));
        return;
    }
    if (!strncmp(path, "/mods/build/", 12) && strlen(path) > 76 && stub_is_hex(path + 12, 64)) {
        char hash[65];
        memcpy(hash, path + 12, 64);
        hash[64] = 0;
        stub_parts(s, method, hash, path + 76, ctype, build_size, body, blen);
        return;
    }
    if (!strncmp(path, "/mods/build/", 12) && strlen(path) == 76 && stub_is_hex(path + 12, 64)) {
        const char *hash = path + 12;
        stub_obj *o = stub_find(g_stub.builds, g_stub.nbuilds, hash);
        if (!strcmp(method, "HEAD")) {
            g_stub.head_calls++;
            if (!o) { stub_send_ex(s, 404, "application/json", "{\"error\":\"not found\"}", 21, 1); return; }
            g_stub.scripts_header = o->scripts;
            stub_send_ex(s, 200, "application/zip", o->bytes, o->len, 1);
            return;
        }
        if (!strcmp(method, "GET")) {
            g_stub.build_gets++;
            if (!o) { stub_json(s, 404, "{\"error\":\"not found\"}"); return; }
            g_stub.scripts_header = o->scripts;
            if (g_stub.corrupt_build_gets && o->len) {
                unsigned char *copy = (unsigned char *)malloc(o->len);
                memcpy(copy, o->bytes, o->len);
                copy[o->len / 2] ^= 0x5a;
                stub_send(s, 200, "application/zip", copy, o->len);
                free(copy);
                return;
            }
            stub_send(s, 200, "application/zip", o->bytes, o->len);
            return;
        }
        if (!strcmp(method, "PUT")) {
            g_stub.build_puts++;
            if (_stricmp(ctype, "application/zip") != 0) { stub_json(s, 415, "{\"error\":\"content-type must be application/zip\"}"); return; }
            if (cl_headers == 0) { stub_json(s, 411, "{\"error\":\"content-length required\"}"); return; }
            if (blen > STUB_PART_SIZE) { stub_json(s, 413, "{\"error\":\"An archive over 95 MiB goes to the mod store in parts.\"}"); return; }
            if (o) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"build\":\"%s\",\"size\":%zu,\"scripts\":%u}", hash, o->len, o->scripts);
                stub_json(s, 200, out);
                return;
            }
            stub_store(s, hash, body, blen);
            return;
        }
        stub_json(s, 405, "{\"error\":\"method not allowed\"}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/mods/claim")) {
        char ref[260], secret[80], (*list)[65] = NULL;
        int n = stub_hex_list((const char *)body, "builds", &list), i, has_secret;
        stub_claim *c = NULL;
        g_stub.claim_calls++;
        if (!stub_str((const char *)body, "ref", ref, sizeof ref)) { free(list); stub_json(s, 400, "{\"error\":\"bad ref\"}"); return; }
        if (n <= 0) { free(list); stub_json(s, 400, "{\"error\":\"no builds\"}"); return; }
        has_secret = stub_str((const char *)body, "secret", secret, sizeof secret);
        if (has_secret) {
            g_stub.claim_with_secret++;
            for (i = 0; i < g_stub.nclaims && !c; i++)
                if (g_stub.claims[i].live && !strcmp(g_stub.claims[i].ref, ref) && !strcmp(g_stub.claims[i].secret, secret)) c = &g_stub.claims[i];
            if (!c) { free(list); stub_json(s, 403, "{\"error\":\"no claim matches this ref and secret\"}"); return; }
        }
        for (i = 0; i < n; i++)
            if (!stub_find(g_stub.builds, g_stub.nbuilds, list[i])) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"unknown builds\",\"missing\":[\"%s\"]}", list[i]);
                free(list);
                stub_json(s, 409, out);
                return;
            }
        if (!c) {
            if (g_stub.nclaims == g_stub.cap_claims) {
                int cap = g_stub.cap_claims ? g_stub.cap_claims * 2 : 16;
                stub_claim *grown = (stub_claim *)realloc(g_stub.claims, sizeof *grown * (size_t)cap);
                if (!grown) { free(list); stub_json(s, 503, "{\"error\":\"the stub ran out of memory\"}"); return; }
                g_stub.claims = grown;
                g_stub.cap_claims = cap;
            }
            c = &g_stub.claims[g_stub.nclaims++];
            memset(c, 0, sizeof *c);
            c->live = 1;
            strcpy_s(c->ref, sizeof c->ref, ref);
            stub_hex_of(++g_stub.counter, "claim", c->id, 32);
            stub_hex_of(++g_stub.counter, "secret", c->secret, 64);
        }
        free(c->builds);
        c->builds = list;   /* the claim keeps the list */
        c->nb = n;
        stub_trail("claim", ref);
        _snprintf_s(out, sizeof out, _TRUNCATE, "{\"claim\":\"%s\",\"secret\":\"%s\"}", c->id, c->secret);
        stub_json(s, has_secret ? 200 : 201, out);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/mods/release")) {
        char id[64], secret[80];
        int i;
        g_stub.release_calls++;
        if (!stub_str((const char *)body, "claim", id, sizeof id) || strlen(id) != 32) { stub_json(s, 400, "{\"error\":\"bad claim\"}"); return; }
        if (!stub_str((const char *)body, "secret", secret, sizeof secret) || strlen(secret) != 64) { stub_json(s, 400, "{\"error\":\"bad secret\"}"); return; }
        for (i = 0; i < g_stub.nclaims; i++) {
            stub_claim *c = &g_stub.claims[i];
            if (!c->live || strcmp(c->id, id) != 0) continue;
            if (strcmp(c->secret, secret) != 0) { stub_json(s, 403, "{\"error\":\"wrong secret\"}"); return; }
            c->live = 0;
            stub_trail("release", c->ref);
            stub_drop_unclaimed();
            stub_send(s, 204, "application/json", "", 0);
            return;
        }
        stub_json(s, 404, "{\"error\":\"no such claim\"}");
        return;
    }
    stub_json(s, 404, "{\"error\":\"not found\"}");
}

static void stub_serve_one(SOCKET s)
{
    char *head = (char *)malloc(16384);
    size_t got = 0;
    char *end = NULL, method[8] = "", path[200] = "", ctype[64] = "";
    unsigned char *body = NULL;
    long long cl = -1;
    unsigned long long build_size = 0;
    int cl_headers = 0;
    size_t have, blen = 0;
    if (!head) return;
    while (got < 16383) {
        int r = recv(s, head + got, (int)(16383 - got), 0);
        if (r <= 0) break;
        got += (size_t)r;
        head[got] = 0;
        if ((end = strstr(head, "\r\n\r\n")) != NULL) break;
    }
    if (!end) { free(head); return; }
    *end = 0;
    sscanf_s(head, "%7s %199s", method, (unsigned)sizeof method, path, (unsigned)sizeof path);
    {
        char *line = strstr(head, "\r\n");
        while (line) {
            char *next;
            line += 2;
            next = strstr(line, "\r\n");
            if (next) *next = 0;
            if (!_strnicmp(line, "Content-Length:", 15)) { cl_headers++; cl = _atoi64(line + 15); }
            else if (!_strnicmp(line, "X-Build-Size:", 13)) build_size = _strtoui64(line + 13, NULL, 10);
            else if (!_strnicmp(line, "Content-Type:", 13)) {
                const char *v = line + 13;
                while (*v == ' ') v++;
                strncpy_s(ctype, sizeof ctype, v, _TRUNCATE);
            }
            if (!next) break;
            *next = '\r';
            line = next;
        }
    }
    have = got - (size_t)(end + 4 - head);
    if (cl > 0) {
        if ((unsigned long long)cl > STUB_MAX_BODY) { stub_json(s, 413, "{\"error\":\"too large\"}"); free(head); return; }
        body = (unsigned char *)malloc((size_t)cl + 1);
        memcpy(body, end + 4, have);
        blen = have;
        while (blen < (size_t)cl) {
            int r = recv(s, (char *)body + blen, (int)((size_t)cl - blen), 0);
            if (r <= 0) break;
            blen += (size_t)r;
        }
        body[blen] = 0;
    } else {
        body = (unsigned char *)malloc(1);
        body[0] = 0;
    }
    EnterCriticalSection(&g_stub.cs);
    g_stub.requests++;
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        g_stub.last_cl_headers = cl_headers;
        g_stub.last_cl_value = cl;
        strcpy_s(g_stub.last_method, sizeof g_stub.last_method, method);
        strncpy_s(g_stub.last_path, sizeof g_stub.last_path, path, _TRUNCATE);
        strcpy_s(g_stub.last_ctype, sizeof g_stub.last_ctype, ctype);
    }
    stub_route(s, method, path, ctype, cl_headers, build_size, body, blen);
    LeaveCriticalSection(&g_stub.cs);
    free(body);
    free(head);
}

static DWORD WINAPI stub_thread(LPVOID unused)
{
    (void)unused;
    while (!InterlockedCompareExchange(&g_stub.stop, 0, 0)) {
        SOCKET c = accept(g_stub.listener, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        stub_serve_one(c);
        shutdown(c, SD_SEND);
        closesocket(c);
    }
    return 0;
}

/* Start the stub on an ephemeral loopback port. Returns the port, or 0. */
static int stub_start(void)
{
    WSADATA wsa;
    struct sockaddr_in a;
    int len = sizeof a;
    memset(&g_stub, 0, sizeof g_stub);
    InitializeCriticalSection(&g_stub.cs);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    g_stub.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_stub.listener == INVALID_SOCKET) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(g_stub.listener, (struct sockaddr *)&a, sizeof a) != 0 || listen(g_stub.listener, 16) != 0 ||
        getsockname(g_stub.listener, (struct sockaddr *)&a, &len) != 0)
        return 0;
    g_stub.port = ntohs(a.sin_port);
    g_stub.thread = CreateThread(NULL, 0, stub_thread, NULL, 0, NULL);
    return g_stub.thread ? g_stub.port : 0;
}

static void stub_stop(void)
{
    int i;
    InterlockedExchange(&g_stub.stop, 1);
    closesocket(g_stub.listener);
    if (g_stub.thread) { WaitForSingleObject(g_stub.thread, 5000); CloseHandle(g_stub.thread); }
    for (i = 0; i < g_stub.nbuilds; i++) free(g_stub.builds[i].bytes);
    for (i = 0; i < g_stub.nclaims; i++) free(g_stub.claims[i].builds);
    free(g_stub.builds);
    free(g_stub.claims);
    stub_upload_free();
    WSACleanup();
}

static void stub_url(char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "http://127.0.0.1:%d", g_stub.port);
}

static void stub_fail_next(const char *prefix, int status)
{
    EnterCriticalSection(&g_stub.cs);
    strncpy_s(g_stub.fail_prefix, sizeof g_stub.fail_prefix, prefix, _TRUNCATE);
    g_stub.fail_status = status;
    LeaveCriticalSection(&g_stub.cs);
}

static int stub_has_build(const char *hash)
{
    int r;
    EnterCriticalSection(&g_stub.cs);
    r = stub_find(g_stub.builds, g_stub.nbuilds, hash) != NULL;
    LeaveCriticalSection(&g_stub.cs);
    return r;
}

/* Store bytes under a hash without the PUT's checks (a forged archive, or a
 * test's own). */
static void stub_put_raw(const char *hash, const void *bytes, size_t len)
{
    stub_obj *o;
    EnterCriticalSection(&g_stub.cs);
    o = stub_find(g_stub.builds, g_stub.nbuilds, hash);
    if (!o) o = stub_new_build(hash);
    else free(o->bytes);
    if (o) {
        o->bytes = (unsigned char *)malloc(len ? len : 1);
        if (len) memcpy(o->bytes, bytes, len);
        o->len = len;
    }
    LeaveCriticalSection(&g_stub.cs);
}

/* The live claim for ref: its build count, or -1. */
static int stub_claim_builds(const char *ref, char id_out[33])
{
    int i, n = -1;
    EnterCriticalSection(&g_stub.cs);
    for (i = 0; i < g_stub.nclaims; i++)
        if (g_stub.claims[i].live && !strcmp(g_stub.claims[i].ref, ref)) {
            n = g_stub.claims[i].nb;
            if (id_out) strcpy_s(id_out, 33, g_stub.claims[i].id);
        }
    LeaveCriticalSection(&g_stub.cs);
    return n;
}

#endif /* SNAPMAP_PLUS_MOD_STORE_STUB_H */
