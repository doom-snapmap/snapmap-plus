/* mod_store_stub.h -- an in-memory mod store on 127.0.0.1 for the store
 * client tests (modnet_test, modsync_test and the flows above them).
 *
 * A small Winsock HTTP/1.1 server on a thread, one request per connection
 * (Connection: close), implementing the routes of the Worker
 * (snapmap-web api/mods_store.js, ONE-SHAPE rule 5) the client uses:
 * HEAD, PUT and GET /mods/build/<hash>, POST /mods/claim and /mods/release.
 * It checks what the Worker checks that the flows can reach: Content-Type
 * application/zip on a build PUT (415), Content-Length present (411), at most
 * 96 MiB (413), and the archive itself (422): a ZIP the product's reader
 * opens, holding mod.json whose build is the path's hash and one
 * assets/<request> entry per carried file its block lists, nothing else. A
 * claim names stored builds (409), a re-claim needs the secret (403), a
 * release of an unknown claim is 404; a release deletes every build no live
 * claim names any more. Counters and fault switches let the tests observe
 * the exchange and inject failures.
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

#define STUB_MAX_OBJECTS 256
#define STUB_MAX_CLAIMS  64
#define STUB_MAX_BODY    (100u * 1024u * 1024u)
#define STUB_MAX_ARCHIVE (96u * 1024u * 1024u)

typedef struct stub_obj { char hash[65]; unsigned char *bytes; size_t len; } stub_obj;
typedef struct stub_claim { char id[33], ref[260], secret[65]; char builds[32][65]; int nb; int live; } stub_claim;

typedef struct stub_state {
    SOCKET listener;
    HANDLE thread;
    int port;
    volatile LONG stop;
    CRITICAL_SECTION cs;
    stub_obj builds[STUB_MAX_OBJECTS];
    int nbuilds;
    stub_claim claims[STUB_MAX_CLAIMS];
    int nclaims;
    unsigned counter;
    /* counters */
    int requests, head_calls, build_puts, build_created, build_gets, claim_calls, claim_with_secret, release_calls,
        builds_deleted;
    /* the last request with a body */
    int last_cl_headers;          /* Content-Length headers seen */
    long long last_cl_value;
    char last_method[8], last_path[160], last_ctype[64];
    char last_refusal[200];       /* the last 422's error text */
    /* faults */
    char fail_prefix[48];         /* the next request whose "METHOD path" starts with this ... */
    int fail_status;              /* ... gets this status and a JSON error */
    int corrupt_build_gets;       /* GET build answers with one byte flipped in the middle */
    char trail[8192];             /* "claim <ref>" and "release <ref>" lines, in the order the stub took them */
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

/* Every quoted 64-hex string in the array following "key":[ . */
static int stub_hex_list(const char *body, const char *key, char out[][65], int max)
{
    char needle[64];
    const char *p, *end;
    int n = 0;
    _snprintf_s(needle, sizeof needle, _TRUNCATE, "\"%s\":[", key);
    p = strstr(body, needle);
    if (!p) return -1;
    p += strlen(needle);
    end = strchr(p, ']');
    if (!end) return -1;
    while (p < end) {
        const char *q = strchr(p, '"');
        if (!q || q >= end) break;
        if (q + 66 <= end && q[65] == '"' && stub_is_hex(q + 1, 64)) {
            if (n < max) { memcpy(out[n], q + 1, 64); out[n][64] = 0; }
            n++;
            p = q + 66;
        } else return -1;
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
                        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                        status, reason, ctype ? ctype : "application/json", len);
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
static const char *stub_check_archive(const char *hash, const unsigned char *body, size_t len, char *why, size_t cap)
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
    text = idx < 0 ? NULL : sh_zip_reader_read(&zr, idx, 1024 * 1024, &tlen, cause, sizeof cause);
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

static void stub_route(SOCKET s, const char *method, const char *path, const char *ctype, int cl_headers,
                       const unsigned char *body, size_t blen)
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
    if (!strncmp(path, "/mods/build/", 12) && strlen(path) == 76 && stub_is_hex(path + 12, 64)) {
        const char *hash = path + 12;
        stub_obj *o = stub_find(g_stub.builds, g_stub.nbuilds, hash);
        if (!strcmp(method, "HEAD")) {
            g_stub.head_calls++;
            if (!o) { stub_send_ex(s, 404, "application/json", "{\"error\":\"not found\"}", 21, 1); return; }
            stub_send_ex(s, 200, "application/zip", o->bytes, o->len, 1);
            return;
        }
        if (!strcmp(method, "GET")) {
            g_stub.build_gets++;
            if (!o) { stub_json(s, 404, "{\"error\":\"not found\"}"); return; }
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
            const char *bad;
            char why[300];
            g_stub.build_puts++;
            if (_stricmp(ctype, "application/zip") != 0) { stub_json(s, 415, "{\"error\":\"content-type must be application/zip\"}"); return; }
            if (cl_headers == 0) { stub_json(s, 411, "{\"error\":\"content-length required\"}"); return; }
            if (blen > STUB_MAX_ARCHIVE) { stub_json(s, 413, "{\"error\":\"the archive is over 96 MiB\"}"); return; }
            if (o) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"build\":\"%s\",\"size\":%zu}", hash, o->len);
                stub_json(s, 200, out);
                return;
            }
            bad = stub_check_archive(hash, body, blen, why, sizeof why);
            if (bad) {
                strncpy_s(g_stub.last_refusal, sizeof g_stub.last_refusal, bad, _TRUNCATE);
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"%s\"}", bad);
                stub_json(s, 422, out);
                return;
            }
            o = &g_stub.builds[g_stub.nbuilds++];
            strcpy_s(o->hash, sizeof o->hash, hash);
            o->bytes = (unsigned char *)malloc(blen ? blen : 1);
            if (blen) memcpy(o->bytes, body, blen);
            o->len = blen;
            g_stub.build_created++;
            _snprintf_s(out, sizeof out, _TRUNCATE, "{\"build\":\"%s\",\"size\":%zu}", hash, blen);
            stub_json(s, 201, out);
            return;
        }
        stub_json(s, 405, "{\"error\":\"method not allowed\"}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/mods/claim")) {
        char ref[260], secret[80], list[32][65];
        int n = stub_hex_list((const char *)body, "builds", list, 32), i, has_secret;
        stub_claim *c = NULL;
        g_stub.claim_calls++;
        if (!stub_str((const char *)body, "ref", ref, sizeof ref)) { stub_json(s, 400, "{\"error\":\"bad ref\"}"); return; }
        if (n <= 0) { stub_json(s, 400, "{\"error\":\"no builds\"}"); return; }
        has_secret = stub_str((const char *)body, "secret", secret, sizeof secret);
        if (has_secret) {
            g_stub.claim_with_secret++;
            for (i = 0; i < g_stub.nclaims && !c; i++)
                if (g_stub.claims[i].live && !strcmp(g_stub.claims[i].ref, ref) && !strcmp(g_stub.claims[i].secret, secret)) c = &g_stub.claims[i];
            if (!c) { stub_json(s, 403, "{\"error\":\"no claim matches this ref and secret\"}"); return; }
        }
        for (i = 0; i < n; i++)
            if (!stub_find(g_stub.builds, g_stub.nbuilds, list[i])) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"unknown builds\",\"missing\":[\"%s\"]}", list[i]);
                stub_json(s, 409, out);
                return;
            }
        if (!c) {
            c = &g_stub.claims[g_stub.nclaims++];
            memset(c, 0, sizeof *c);
            c->live = 1;
            strcpy_s(c->ref, sizeof c->ref, ref);
            stub_hex_of(++g_stub.counter, "claim", c->id, 32);
            stub_hex_of(++g_stub.counter, "secret", c->secret, 64);
        }
        c->nb = n < 32 ? n : 32;
        for (i = 0; i < c->nb; i++) strcpy_s(c->builds[i], 65, list[i]);
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
    stub_route(s, method, path, ctype, cl_headers, body, blen);
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
    if (!o && g_stub.nbuilds < STUB_MAX_OBJECTS) { o = &g_stub.builds[g_stub.nbuilds++]; strcpy_s(o->hash, sizeof o->hash, hash); }
    else if (o) free(o->bytes);
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
