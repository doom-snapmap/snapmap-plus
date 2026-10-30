/* mod_store_stub.h -- an in-memory mod store on 127.0.0.1 for the store
 * client tests (modnet_test, modsync_test, modexport_test).
 *
 * A small Winsock HTTP/1.1 server on a thread, one request per connection
 * (Connection: close), implementing the Contract 4 routes the client uses
 * with the status codes and bodies of the Worker
 * (snapmap-web api/mods_store.js): POST /mods/missing, PUT and GET
 * /mods/blob/<sha>, PUT and GET /mods/bundle/<hash>, POST /mods/claim and
 * /mods/release. It checks what the Worker checks that the flows can reach:
 * the blob body hashes to its path (422), Content-Type on blob PUT (415),
 * Content-Length present (411), the bundle body hashes to its path (422),
 * unknown top-level manifest keys (422), blobs a manifest names exist (409),
 * claim secrets (403) and unknown claims (404). Validation of names and
 * paths is not repeated. Counters and fault switches let the tests observe
 * negotiation and inject failures.
 *
 * Requires ws2_32.lib and the SHA-256 in modbundle.c. */
#ifndef SNAPMAP_PLUS_MOD_STORE_STUB_H
#define SNAPMAP_PLUS_MOD_STORE_STUB_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modbundle.h"

#pragma comment(lib, "ws2_32.lib")

#define STUB_MAX_OBJECTS 512
#define STUB_MAX_CLAIMS  64
#define STUB_MAX_BODY    (70u * 1024u * 1024u)

typedef struct stub_obj { char hash[65]; unsigned char *bytes; size_t len; } stub_obj;
typedef struct stub_claim { char id[33], ref[260], secret[65]; char bundles[32][65]; int nb; int live; } stub_claim;

typedef struct stub_state {
    SOCKET listener;
    HANDLE thread;
    int port;
    volatile LONG stop;
    CRITICAL_SECTION cs;
    stub_obj blobs[STUB_MAX_OBJECTS];
    int nblobs;
    stub_obj bundles[STUB_MAX_OBJECTS];
    int nbundles;
    stub_claim claims[STUB_MAX_CLAIMS];
    int nclaims;
    unsigned counter;
    /* counters */
    int requests, missing_calls, blob_puts, blob_created, blob_gets, bundle_puts, bundle_created, bundle_gets,
        claim_calls, claim_with_secret, release_calls;
    /* the last request with a body */
    int last_cl_headers;          /* Content-Length headers seen */
    long long last_cl_value;
    char last_method[8], last_path[160], last_ctype[64];
    /* faults */
    char fail_prefix[48];         /* the next request whose "METHOD path" starts with this ... */
    int fail_status;              /* ... gets this status and a JSON error */
    int corrupt_blob_gets;        /* GET blob answers with one byte flipped */
    char corrupt_bundle[65];      /* GET bundle for this hash answers with altered text */
    int serve_bundle_member;      /* GET bundle answers with "bundle" inserted, as the Worker may store it */
    char drop_blob_before_bundle[65];  /* removed just before the next bundle PUT (a purge race) */
} stub_state;

static stub_state g_stub;

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

/* The Worker's TOP_KEYS check: the first top-level key it does not know, or NULL. */
static const char *stub_unknown_top_key(const char *body, char *buf, size_t cap)
{
    static const char *const known[] = { "format", "id", "build", "version", "name", "author", "homepage",
                                         "requirements", "shadows", "files", "bundle", NULL };
    int depth = 0, instr = 0, esc = 0, i;
    const char *p, *start = NULL;
    for (p = body; *p; p++) {
        if (instr) {
            if (esc) esc = 0;
            else if (*p == '\\') esc = 1;
            else if (*p == '"') {
                instr = 0;
                if (depth == 1 && start && p[1] == ':') {
                    size_t n = (size_t)(p - start);
                    int ok = 0;
                    if (n >= cap) n = cap - 1;
                    memcpy(buf, start, n);
                    buf[n] = 0;
                    for (i = 0; known[i]; i++) if (strcmp(known[i], buf) == 0) ok = 1;
                    if (!ok) return buf;
                }
                start = NULL;
            }
            continue;
        }
        if (*p == '"') { instr = 1; start = p + 1; }
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
    }
    return NULL;
}

static void stub_hex_of(unsigned v, const char *salt, char *out, size_t chars)
{
    char seed[96], h[65];
    _snprintf_s(seed, sizeof seed, _TRUNCATE, "%s-%u-%lu", salt, v, GetTickCount());
    sh_mb_sha256_hex(seed, strlen(seed), h);
    memcpy(out, h, chars);
    out[chars] = 0;
}

static void stub_send(SOCKET s, int status, const char *ctype, const void *body, size_t len)
{
    char head[256];
    const char *reason = status == 200 ? "OK" : status == 201 ? "Created" : status == 204 ? "No Content" : "Status";
    int n = _snprintf_s(head, sizeof head, _TRUNCATE,
                        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                        status, reason, ctype ? ctype : "application/json", len);
    send(s, head, n, 0);
    while (len) {
        int chunk = (int)(len > (1u << 20) ? (1u << 20) : len);
        int sent = send(s, (const char *)body, chunk, 0);
        if (sent <= 0) break;
        body = (const char *)body + sent;
        len -= (size_t)sent;
    }
}

static void stub_json(SOCKET s, int status, const char *json)
{
    stub_send(s, status, "application/json", json, strlen(json));
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
        stub_json(s, st, out);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/mods/missing")) {
        static char list[4096][65];
        int n = stub_hex_list((const char *)body, "blobs", list, 4096), i, w = 0;
        char *big;
        size_t cap = 64 + (size_t)(n > 0 ? n : 0) * 68;
        g_stub.missing_calls++;
        if (n < 0) { stub_json(s, 400, "{\"error\":\"expected an array of SHA-256 hex strings\"}"); return; }
        big = (char *)malloc(cap);
        w = _snprintf_s(big, cap, _TRUNCATE, "{\"missing\":[");
        for (i = 0; i < n; i++) {
            int k, dup = 0;
            for (k = 0; k < i; k++) dup |= strcmp(list[k], list[i]) == 0;
            if (dup || stub_find(g_stub.blobs, g_stub.nblobs, list[i])) continue;
            w += _snprintf_s(big + w, cap - (size_t)w, _TRUNCATE, "%s\"%s\"", big[w - 1] == '[' ? "" : ",", list[i]);
        }
        _snprintf_s(big + w, cap - (size_t)w, _TRUNCATE, "]}");
        stub_json(s, 200, big);
        free(big);
        return;
    }
    if (!strncmp(path, "/mods/blob/", 11) && strlen(path) == 75 && stub_is_hex(path + 11, 64)) {
        const char *hash = path + 11;
        stub_obj *o = stub_find(g_stub.blobs, g_stub.nblobs, hash);
        if (!strcmp(method, "GET")) {
            g_stub.blob_gets++;
            if (!o) { stub_json(s, 404, "{\"error\":\"not found\"}"); return; }
            if (g_stub.corrupt_blob_gets && o->len) {
                unsigned char *copy = (unsigned char *)malloc(o->len);
                memcpy(copy, o->bytes, o->len);
                copy[0] ^= 0x5a;
                stub_send(s, 200, "application/octet-stream", copy, o->len);
                free(copy);
                return;
            }
            stub_send(s, 200, "application/octet-stream", o->bytes, o->len);
            return;
        }
        if (!strcmp(method, "PUT")) {
            char actual[65];
            g_stub.blob_puts++;
            if (_stricmp(ctype, "application/octet-stream") != 0) { stub_json(s, 415, "{\"error\":\"content-type must be application/octet-stream\"}"); return; }
            if (cl_headers == 0) { stub_json(s, 411, "{\"error\":\"content-length required\"}"); return; }
            sh_mb_sha256_hex(body ? (const void *)body : "", blen, actual);
            if (strcmp(actual, hash) != 0) { stub_json(s, 422, "{\"error\":\"body does not hash to the path\"}"); return; }
            if (o) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"blob\":\"%s\",\"size\":%zu}", hash, o->len);
                stub_json(s, 200, out);
                return;
            }
            o = &g_stub.blobs[g_stub.nblobs++];
            strcpy_s(o->hash, sizeof o->hash, hash);
            o->bytes = (unsigned char *)malloc(blen ? blen : 1);
            if (blen) memcpy(o->bytes, body, blen);
            o->len = blen;
            g_stub.blob_created++;
            _snprintf_s(out, sizeof out, _TRUNCATE, "{\"blob\":\"%s\",\"size\":%zu}", hash, blen);
            stub_json(s, 201, out);
            return;
        }
        stub_json(s, 405, "{\"error\":\"method not allowed\"}");
        return;
    }
    if (!strncmp(path, "/mods/bundle/", 13) && strlen(path) == 77 && stub_is_hex(path + 13, 64)) {
        const char *hash = path + 13;
        stub_obj *o = stub_find(g_stub.bundles, g_stub.nbundles, hash);
        if (!strcmp(method, "GET")) {
            g_stub.bundle_gets++;
            if (!o) { stub_json(s, 404, "{\"error\":\"not found\"}"); return; }
            if (!strcmp(g_stub.corrupt_bundle, hash)) {
                char *copy = (char *)malloc(o->len + 1);
                memcpy(copy, o->bytes, o->len);
                copy[o->len] = 0;
                { char *n = strstr(copy, "\"name\":\""); if (n) n[8] ^= 0x01; }
                stub_send(s, 200, "application/json; charset=utf-8", copy, o->len);
                free(copy);
                return;
            }
            if (g_stub.serve_bundle_member) {
                /* canonicalText(manifest, true): "bundle" in sorted key order, after "build". */
                char *copy = (char *)malloc(o->len + 96), *at;
                memcpy(copy, o->bytes, o->len);
                copy[o->len] = 0;
                at = strstr(copy, ",\"extra\":");
                if (!at) at = strstr(copy, ",\"files\":");
                if (at) {
                    size_t head = (size_t)(at - copy);
                    char *joined = (char *)malloc(o->len + 96);
                    memcpy(joined, copy, head);
                    _snprintf_s(joined + head, 96, _TRUNCATE, ",\"bundle\":\"%s\"", hash);
                    memcpy(joined + head + 76, copy + head, o->len - head);
                    stub_send(s, 200, "application/json; charset=utf-8", joined, o->len + 76);
                    free(joined);
                } else stub_send(s, 200, "application/json; charset=utf-8", o->bytes, o->len);
                free(copy);
                return;
            }
            stub_send(s, 200, "application/json; charset=utf-8", o->bytes, o->len);
            return;
        }
        if (!strcmp(method, "PUT")) {
            char actual[65], key[64];
            const char *bad;
            const char *p;
            char miss[4096];
            int nmiss = 0, w;
            g_stub.bundle_puts++;
            if (g_stub.drop_blob_before_bundle[0]) {
                stub_obj *d = stub_find(g_stub.blobs, g_stub.nblobs, g_stub.drop_blob_before_bundle);
                if (d) { free(d->bytes); *d = g_stub.blobs[--g_stub.nblobs]; }
                g_stub.drop_blob_before_bundle[0] = 0;
            }
            sh_mb_sha256_hex(body ? (const void *)body : "", blen, actual);
            if ((bad = stub_unknown_top_key((const char *)body, key, sizeof key)) != NULL) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"unknown manifest key %s\"}", bad);
                stub_json(s, 422, out);
                return;
            }
            if (strcmp(actual, hash) != 0) { stub_json(s, 422, "{\"error\":\"canonical manifest does not hash to the path\"}"); return; }
            w = _snprintf_s(miss, sizeof miss, _TRUNCATE, "[");
            for (p = strstr((const char *)body, "\"blob\":\""); p; p = strstr(p + 8, "\"blob\":\"")) {
                char h[65];
                memcpy(h, p + 8, 64);
                h[64] = 0;
                if (!stub_find(g_stub.blobs, g_stub.nblobs, h)) {
                    w += _snprintf_s(miss + w, sizeof miss - (size_t)w, _TRUNCATE, "%s\"%s\"", nmiss ? "," : "", h);
                    nmiss++;
                }
            }
            if (nmiss) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"missing blobs\",\"missing\":%s],\"missingCount\":%d}", miss, nmiss);
                stub_json(s, 409, out);
                return;
            }
            _snprintf_s(out, sizeof out, _TRUNCATE, "{\"bundle\":\"%s\"}", hash);
            if (o) { stub_json(s, 200, out); return; }
            o = &g_stub.bundles[g_stub.nbundles++];
            strcpy_s(o->hash, sizeof o->hash, hash);
            o->bytes = (unsigned char *)malloc(blen + 1);
            memcpy(o->bytes, body, blen);
            o->bytes[blen] = 0;
            o->len = blen;
            g_stub.bundle_created++;
            stub_json(s, 201, out);
            return;
        }
        stub_json(s, 405, "{\"error\":\"method not allowed\"}");
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/mods/claim")) {
        char ref[260], secret[80], list[32][65];
        int n = stub_hex_list((const char *)body, "bundles", list, 32), i, has_secret;
        stub_claim *c = NULL;
        g_stub.claim_calls++;
        if (!stub_str((const char *)body, "ref", ref, sizeof ref)) { stub_json(s, 400, "{\"error\":\"bad ref\"}"); return; }
        if (n <= 0) { stub_json(s, 400, "{\"error\":\"no bundles\"}"); return; }
        has_secret = stub_str((const char *)body, "secret", secret, sizeof secret);
        if (has_secret) {
            g_stub.claim_with_secret++;
            for (i = 0; i < g_stub.nclaims && !c; i++)
                if (g_stub.claims[i].live && !strcmp(g_stub.claims[i].ref, ref) && !strcmp(g_stub.claims[i].secret, secret)) c = &g_stub.claims[i];
            if (!c) { stub_json(s, 403, "{\"error\":\"no claim matches this ref and secret\"}"); return; }
        }
        for (i = 0; i < n; i++)
            if (!stub_find(g_stub.bundles, g_stub.nbundles, list[i])) {
                _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"unknown bundles\",\"missing\":[\"%s\"],\"missingCount\":1}", list[i]);
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
        for (i = 0; i < c->nb; i++) strcpy_s(c->bundles[i], 65, list[i]);
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
    if (strcmp(method, "GET") != 0) {
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
    for (i = 0; i < g_stub.nblobs; i++) free(g_stub.blobs[i].bytes);
    for (i = 0; i < g_stub.nbundles; i++) free(g_stub.bundles[i].bytes);
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

static int stub_has_blob(const char *hash)
{
    int r;
    EnterCriticalSection(&g_stub.cs);
    r = stub_find(g_stub.blobs, g_stub.nblobs, hash) != NULL;
    LeaveCriticalSection(&g_stub.cs);
    return r;
}

static int stub_has_bundle(const char *hash)
{
    int r;
    EnterCriticalSection(&g_stub.cs);
    r = stub_find(g_stub.bundles, g_stub.nbundles, hash) != NULL;
    LeaveCriticalSection(&g_stub.cs);
    return r;
}

/* The live claim for ref: its bundle count, or -1. */
static int stub_claim_bundles(const char *ref, char id_out[33])
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
