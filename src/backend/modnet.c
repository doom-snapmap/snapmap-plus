/* modnet.c -- see modnet.h. WinHTTP client for the mod store. */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "modnet.h"
#include "modbundle.h"
#include "mp_json.h"
#include "config.h"

#pragma comment(lib, "winhttp.lib")

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#define MN_AGENT        L"snapmap-plus-mods/1"
#define MN_CHUNK        (256u * 1024u)
#define MN_ERROR_BODY   (64u * 1024u)
#define MN_SMALL_BODY   (64u * 1024u)
#define MN_MISSING_BODY (512u * 1024u)
#define MN_RESOLVE_MS   15000
#define MN_CONNECT_MS   15000
#define MN_SEND_MS      30000
#define MN_RECEIVE_MS   30000

/* ------------------------------------------------------------ session ---- */

static SRWLOCK   g_lock = SRWLOCK_INIT;
static HINTERNET g_session;
static char      g_override[MODNET_URL_MAX];
static int       g_override_set;
static wchar_t   g_host[256];
static INTERNET_PORT g_port;
static int       g_secure;
static wchar_t   g_prefix[256];      /* base path without a trailing '/' */

static double mn_now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static int mn_fail(modnet_result *r, int code, const char *fmt, ...)
{
    if (r) {
        r->code = code;
        if (fmt) {
            va_list ap;
            va_start(ap, fmt);
            _vsnprintf_s(r->message, sizeof r->message, _TRUNCATE, fmt, ap);
            va_end(ap);
        } else {
            strncpy_s(r->message, sizeof r->message, modnet_code_text(code), _TRUNCATE);
        }
    }
    return code;
}

const char *modnet_code_text(int code)
{
    switch (code) {
    case MODNET_OK:            return "ok";
    case MODNET_E_ARG:         return "bad argument";
    case MODNET_E_NOMEM:       return "out of memory";
    case MODNET_E_CONFIG:      return "the mod store address is not usable";
    case MODNET_E_NET:         return "network error";
    case MODNET_E_TIMEOUT:     return "timed out";
    case MODNET_E_CANCELLED:   return "cancelled";
    case MODNET_E_HTTP:        return "unexpected response";
    case MODNET_E_PROTOCOL:    return "malformed response";
    case MODNET_E_HASH:        return "content did not match its hash";
    case MODNET_E_TOO_LARGE:   return "too large";
    case MODNET_E_NOT_FOUND:   return "not found";
    case MODNET_E_FORBIDDEN:   return "wrong claim secret";
    case MODNET_E_CONFLICT:    return "the store lacks content the request names";
    case MODNET_E_REFUSED:     return "refused by the store";
    case MODNET_E_RATE:        return "rate limited";
    case MODNET_E_BLOCKED:     return "blocked by takedown";
    case MODNET_E_UNAVAILABLE: return "the mod store is unavailable";
    default:                   return "error";
    }
}

int modnet_transient(int code)
{
    switch (code) {
    case MODNET_E_NET: case MODNET_E_TIMEOUT: case MODNET_E_CANCELLED: case MODNET_E_HTTP:
    case MODNET_E_PROTOCOL: case MODNET_E_HASH: case MODNET_E_CONFLICT: case MODNET_E_RATE:
    case MODNET_E_UNAVAILABLE: case MODNET_E_NOMEM:
        return 1;
    default:
        return 0;
    }
}

int modnet_valid_hash(const char *s)
{
    int i;
    if (!s) return 0;
    for (i = 0; i < MODNET_HASH_HEX; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return s[MODNET_HASH_HEX] == 0;
}

static int mn_valid_hex(const char *s, size_t n)
{
    size_t i;
    if (!s || strlen(s) != n) return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

int modnet_valid_ref(const char *ref)
{
    int units, wn;
    const unsigned char *p;
    wchar_t w[MODNET_REF_MAX + 2];
    if (!ref || !ref[0] || strlen(ref) > 4 * MODNET_REF_MAX) return 0;
    for (p = (const unsigned char *)ref; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return 0;
    units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ref, -1, NULL, 0);
    if (units <= 1 || units - 1 > MODNET_REF_MAX) return 0;
    wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ref, -1, w, MODNET_REF_MAX + 2);
    if (wn <= 0) return 0;
    for (units = 0; w[units]; units++)
        if (w[units] >= 0x80 && w[units] <= 0x9f) return 0;   /* C1 controls, as the store refuses */
    return 1;
}

static int mn_is_loopback(const wchar_t *host)
{
    return _wcsicmp(host, L"127.0.0.1") == 0 || _wcsicmp(host, L"localhost") == 0 ||
           _wcsicmp(host, L"[::1]") == 0 || _wcsicmp(host, L"::1") == 0;
}

int modnet_base_url(char *out, size_t cap)
{
    char tmp[MODNET_URL_MAX];
    if (!out || !cap) return 0;
    out[0] = 0;
    AcquireSRWLockShared(&g_lock);
    if (g_override_set) strncpy_s(tmp, sizeof tmp, g_override, _TRUNCATE);
    else tmp[0] = 0;
    ReleaseSRWLockShared(&g_lock);
    if (!tmp[0]) {
        int n = sh_config_get_string("mods.store", tmp, (int)sizeof tmp);
        if (n <= 0) strcpy_s(tmp, sizeof tmp, MODNET_DEFAULT_STORE);
    }
    if (strlen(tmp) >= cap) return 0;
    strcpy_s(out, cap, tmp);
    return 1;
}

/* Parse a base URL into host, port, scheme and path prefix. */
static int mn_parse_url(const char *url, wchar_t *host, size_t host_cap, INTERNET_PORT *port, int *secure,
                        wchar_t *prefix, size_t prefix_cap, char *why, size_t why_cap)
{
    wchar_t wurl[MODNET_URL_MAX], whost[256], wpath[512];
    URL_COMPONENTS uc;
    size_t n;
    if (!url || !url[0] || strlen(url) >= MODNET_URL_MAX ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, wurl, MODNET_URL_MAX)) {
        strncpy_s(why, why_cap, "the mod store address is empty or not text", _TRUNCATE);
        return 0;
    }
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = whost;
    uc.dwHostNameLength = (DWORD)(sizeof whost / sizeof whost[0]);
    uc.lpszUrlPath = wpath;
    uc.dwUrlPathLength = (DWORD)(sizeof wpath / sizeof wpath[0]);
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !whost[0]) {
        strncpy_s(why, why_cap, "the mod store address is not a URL", _TRUNCATE);
        return 0;
    }
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) *secure = 1;
    else if (uc.nScheme == INTERNET_SCHEME_HTTP && mn_is_loopback(whost)) *secure = 0;
    else {
        strncpy_s(why, why_cap, "the mod store address must use https (http only for a loopback test host)", _TRUNCATE);
        return 0;
    }
    if (wcschr(wpath, L'?') || wcschr(wpath, L'#')) {
        strncpy_s(why, why_cap, "the mod store address must not carry a query", _TRUNCATE);
        return 0;
    }
    n = wcslen(wpath);
    while (n && wpath[n - 1] == L'/') wpath[--n] = 0;
    if (wcslen(whost) >= host_cap || n >= prefix_cap) {
        strncpy_s(why, why_cap, "the mod store address is too long", _TRUNCATE);
        return 0;
    }
    wcscpy_s(host, host_cap, whost);
    wcscpy_s(prefix, prefix_cap, wpath);
    *port = uc.nPort;
    return 1;
}

static void mn_close_session_locked(void)
{
    if (g_session) WinHttpCloseHandle(g_session);
    g_session = NULL;
    g_host[0] = 0;
    g_prefix[0] = 0;
}

int modnet_set_base_url(const char *url)
{
    wchar_t host[256], prefix[256];
    INTERNET_PORT port = 0;
    int secure = 0, ok = 1;
    char why[160];
    if (url && url[0]) ok = mn_parse_url(url, host, 256, &port, &secure, prefix, 256, why, sizeof why);
    if (!ok) return 0;
    AcquireSRWLockExclusive(&g_lock);   /* waits for requests in flight */
    mn_close_session_locked();
    if (url && url[0]) { strncpy_s(g_override, sizeof g_override, url, _TRUNCATE); g_override_set = 1; }
    else { g_override[0] = 0; g_override_set = 0; }
    ReleaseSRWLockExclusive(&g_lock);
    return 1;
}

void modnet_shutdown(void)
{
    AcquireSRWLockExclusive(&g_lock);
    mn_close_session_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

/* Create the session under the exclusive lock. */
static int mn_open_session_locked(char *why, size_t why_cap)
{
    char url[MODNET_URL_MAX];
    DWORD protocols;
    if (g_session) return 1;
    /* modnet_base_url takes the shared lock; read the override directly. */
    if (g_override_set) strncpy_s(url, sizeof url, g_override, _TRUNCATE);
    else {
        int n = sh_config_get_string("mods.store", url, (int)sizeof url);
        if (n <= 0) strcpy_s(url, sizeof url, MODNET_DEFAULT_STORE);
    }
    if (!mn_parse_url(url, g_host, 256, &g_port, &g_secure, g_prefix, 256, why, why_cap)) return 0;
    if (mn_is_loopback(g_host)) {
        g_session = WinHttpOpen(MN_AGENT, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
        /* The system's proxy configuration; the static WinHTTP setting on
         * systems without automatic proxy support. */
        g_session = WinHttpOpen(MN_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_session)
            g_session = WinHttpOpen(MN_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!g_session) {
        _snprintf_s(why, why_cap, _TRUNCATE, "the HTTP session could not be opened (error %lu)", GetLastError());
        g_host[0] = 0;
        return 0;
    }
    WinHttpSetTimeouts(g_session, MN_RESOLVE_MS, MN_CONNECT_MS, MN_SEND_MS, MN_RECEIVE_MS);
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    return 1;
}

/* Take the shared lock with an open session. Returns 1 holding the lock. */
static int mn_acquire(char *why, size_t why_cap)
{
    for (;;) {
        AcquireSRWLockShared(&g_lock);
        if (g_session) return 1;
        ReleaseSRWLockShared(&g_lock);
        AcquireSRWLockExclusive(&g_lock);
        if (!mn_open_session_locked(why, why_cap)) { ReleaseSRWLockExclusive(&g_lock); return 0; }
        ReleaseSRWLockExclusive(&g_lock);
    }
}

/* ------------------------------------------------------------ exchange --- */

typedef struct mn_req {
    const wchar_t *verb;
    const char *route;             /* "/mods/..." */
    const char *content_type;      /* NULL: no body */
    const unsigned char *body;
    size_t body_len;
    unsigned long long max_response;
    unsigned long long expect_total;   /* download progress total when no Content-Length */
} mn_req;

static int mn_cancelled(const modnet_io *io)
{
    return io && io->cancel && InterlockedCompareExchange(io->cancel, 0, 0) != 0;
}

static int mn_winhttp_error(modnet_result *r, const char *step)
{
    DWORD e = GetLastError();
    if (e == ERROR_WINHTTP_TIMEOUT) return mn_fail(r, MODNET_E_TIMEOUT, "%s timed out", step);
    if (e == ERROR_WINHTTP_OPERATION_CANCELLED) return mn_fail(r, MODNET_E_CANCELLED, NULL);
    if (e == ERROR_WINHTTP_SECURE_FAILURE) return mn_fail(r, MODNET_E_NET, "%s: TLS failure", step);
    if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) return mn_fail(r, MODNET_E_NET, "%s: the store host could not be resolved", step);
    if (e == ERROR_WINHTTP_CANNOT_CONNECT) return mn_fail(r, MODNET_E_NET, "%s: cannot connect to the store", step);
    return mn_fail(r, MODNET_E_NET, "%s failed (winhttp %lu)", step, e);
}

/* One HTTP exchange. MODNET_OK means a complete response arrived (any
 * status, in r->status); the body is returned malloc'd and NUL-terminated. */
static int mn_exchange(const mn_req *q, unsigned char **resp, size_t *resp_len, const modnet_io *io, modnet_result *r)
{
    HINTERNET conn = NULL, req = NULL;
    wchar_t path[1024], wroute[512], headers[256];
    char why[160];
    DWORD status = 0, size, policy, features;
    unsigned char *buf = NULL;
    size_t got = 0, cap = 0;
    unsigned long long declared = 0, cap_bytes;
    int code = MODNET_OK, have_declared = 0;

    *resp = NULL;
    *resp_len = 0;
    r->status = 0;
    if (mn_cancelled(io)) return mn_fail(r, MODNET_E_CANCELLED, NULL);
    if (!MultiByteToWideChar(CP_UTF8, 0, q->route, -1, wroute, 512)) return mn_fail(r, MODNET_E_ARG, "bad route");
    why[0] = 0;
    if (!mn_acquire(why, sizeof why)) return mn_fail(r, MODNET_E_CONFIG, "%s", why[0] ? why : "no mod store address");
    _snwprintf_s(path, 1024, _TRUNCATE, L"%s%s", g_prefix, wroute);
    conn = WinHttpConnect(g_session, g_host, g_port, 0);
    if (!conn) { code = mn_winhttp_error(r, "connect"); goto done; }
    req = WinHttpOpenRequest(conn, q->verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             g_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { code = mn_winhttp_error(r, "open request"); goto done; }
    /* The store never redirects; following one could send content elsewhere. */
    policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    features = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
    if (q->content_type) {
        wchar_t ct[64];
        MultiByteToWideChar(CP_UTF8, 0, q->content_type, -1, ct, 64);
        /* Content-Length is written by WinHTTP from the total length below,
         * zero included, for every request that has a body. */
        _snwprintf_s(headers, 256, _TRUNCATE, L"Content-Type: %s\r\nAccept: application/json, application/octet-stream\r\n", ct);
    } else {
        _snwprintf_s(headers, 256, _TRUNCATE, L"Accept: application/json, application/octet-stream\r\n");
    }
    r->requests++;
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0,
                            q->content_type ? (DWORD)q->body_len : 0, 0)) {
        code = mn_winhttp_error(r, "send");
        goto done;
    }
    if (q->content_type && q->body_len) {
        size_t off = 0;
        while (off < q->body_len) {
            DWORD chunk = (DWORD)((q->body_len - off) > MN_CHUNK ? MN_CHUNK : (q->body_len - off)), wrote = 0;
            if (mn_cancelled(io)) { code = mn_fail(r, MODNET_E_CANCELLED, NULL); goto done; }
            if (!WinHttpWriteData(req, q->body + off, chunk, &wrote) || wrote == 0) {
                code = mn_winhttp_error(r, "upload");
                goto done;
            }
            off += wrote;
            r->sent += wrote;
            if (io && io->progress) io->progress(io->context, off, q->body_len);
        }
    }
    if (!WinHttpReceiveResponse(req, NULL)) { code = mn_winhttp_error(r, "response"); goto done; }
    size = sizeof status;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        code = mn_fail(r, MODNET_E_PROTOCOL, "no status line");
        goto done;
    }
    r->status = (int)status;
    {
        wchar_t cl[32];
        DWORD n = sizeof cl;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &n,
                                WINHTTP_NO_HEADER_INDEX)) {
            wchar_t *end = NULL;
            declared = _wcstoui64(cl, &end, 10);
            have_declared = end && end != cl && *end == 0;
        }
    }
    /* Refusals carry a short JSON error; nothing more is read from them. */
    cap_bytes = status >= 300 ? MN_ERROR_BODY : q->max_response;
    if (status < 300 && have_declared && declared > cap_bytes) {
        code = mn_fail(r, MODNET_E_TOO_LARGE, "the response is larger than %llu bytes", cap_bytes);
        goto done;
    }
    cap = (size_t)(have_declared && declared < cap_bytes ? declared : (cap_bytes < 65536 ? cap_bytes : 65536)) + 1;
    buf = (unsigned char *)malloc(cap);
    if (!buf) { code = mn_fail(r, MODNET_E_NOMEM, NULL); goto done; }
    for (;;) {
        DWORD avail = 0, rd = 0;
        if (mn_cancelled(io)) { code = mn_fail(r, MODNET_E_CANCELLED, NULL); goto done; }
        if (!WinHttpQueryDataAvailable(req, &avail)) { code = mn_winhttp_error(r, "download"); goto done; }
        if (avail == 0) break;
        if ((unsigned long long)got + avail > cap_bytes) {
            if (status >= 300) break;   /* enough of a refusal to read its cause */
            code = mn_fail(r, MODNET_E_TOO_LARGE, "the response is larger than %llu bytes", cap_bytes);
            goto done;
        }
        if (got + avail + 1 > cap) {
            size_t want = cap * 2;
            unsigned char *grown;
            if (want < got + avail + 1) want = got + avail + 1;
            if ((unsigned long long)want > cap_bytes + 1) want = (size_t)cap_bytes + 1;
            grown = (unsigned char *)realloc(buf, want);
            if (!grown) { code = mn_fail(r, MODNET_E_NOMEM, NULL); goto done; }
            buf = grown;
            cap = want;
        }
        if (!WinHttpReadData(req, buf + got, avail, &rd)) { code = mn_winhttp_error(r, "download"); goto done; }
        if (rd == 0) break;
        got += rd;
        r->received += rd;
        /* Progress follows the body that is the transfer: the upload for a
         * request with a body, else the download. */
        if (io && io->progress && status < 300 && !q->content_type)
            io->progress(io->context, got, have_declared ? declared : q->expect_total);
    }
    if (status < 300 && have_declared && declared != got) {
        code = mn_fail(r, MODNET_E_PROTOCOL, "the response ended early (%zu of %llu bytes)", got, declared);
        goto done;
    }
    buf[got] = 0;
    *resp = buf;
    *resp_len = got;
    buf = NULL;
done:
    free(buf);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    ReleaseSRWLockShared(&g_lock);
    if (code == MODNET_OK) r->code = MODNET_OK;
    return code;
}

/* The store's {"error": "..."} text, printable ASCII only. */
static void mn_error_text(const unsigned char *body, size_t len, char *out, size_t cap)
{
    mpj *root;
    const mpj *e;
    size_t i, w = 0;
    out[0] = 0;
    if (!body || !len || len > MN_ERROR_BODY) return;
    root = mpj_parse((const char *)body, len);
    if (!root) return;
    e = root->type == MPJ_OBJ ? mpj_get(root, "error") : NULL;
    if (e && e->type == MPJ_STR && e->text) {
        for (i = 0; e->text[i] && w + 1 < cap && w < 160; i++) {
            unsigned char c = (unsigned char)e->text[i];
            if (c >= 0x20 && c < 0x7f) out[w++] = (char)c;
        }
        out[w] = 0;
    }
    mpj_free(root);
}

/* Map a refusal status onto a code with the store's cause. */
static int mn_status_fail(modnet_result *r, const unsigned char *body, size_t len, const char *what)
{
    char cause[176];
    int code;
    int s = r->status;
    mn_error_text(body, len, cause, sizeof cause);
    if (s == 400 || s == 411 || s == 415 || s == 422) code = MODNET_E_REFUSED;
    else if (s == 403) code = MODNET_E_FORBIDDEN;
    else if (s == 404) code = MODNET_E_NOT_FOUND;
    else if (s == 409) code = MODNET_E_CONFLICT;
    else if (s == 413) code = MODNET_E_TOO_LARGE;
    else if (s == 429) code = MODNET_E_RATE;
    else if (s == 451) code = MODNET_E_BLOCKED;
    else if (s >= 500 && s < 600) code = MODNET_E_UNAVAILABLE;
    else code = MODNET_E_HTTP;
    if (cause[0]) return mn_fail(r, code, "%s: HTTP %d: %s", what, s, cause);
    return mn_fail(r, code, "%s: HTTP %d: %s", what, s, modnet_code_text(code));
}

static void mn_begin(modnet_result *r, double *t0)
{
    memset(r, 0, sizeof *r);
    *t0 = mn_now_ms();
}

static int mn_end(modnet_result *r, double t0, int code)
{
    r->ms = mn_now_ms() - t0;
    r->code = code;
    if (code == MODNET_OK) r->message[0] = 0;
    else if (!r->message[0]) strncpy_s(r->message, sizeof r->message, modnet_code_text(code), _TRUNCATE);
    return code;
}

/* ------------------------------------------------------------ text ------ */

typedef struct mn_buf { char *p; size_t n, cap; int bad; } mn_buf;

static void mn_put(mn_buf *b, const char *s, size_t n)
{
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 256;
        char *g;
        while (want < b->n + n + 1) want *= 2;
        g = (char *)realloc(b->p, want);
        if (!g) { b->bad = 1; return; }
        b->p = g;
        b->cap = want;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void mn_puts(mn_buf *b, const char *s) { mn_put(b, s, strlen(s)); }

/* A JSON string with '"', '\\' and control characters escaped. */
static void mn_put_json_string(mn_buf *b, const char *s)
{
    mn_put(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') mn_put(b, "\\\"", 2);
        else if (c == '\\') mn_put(b, "\\\\", 2);
        else if (c < 0x20) {
            char esc[8];
            _snprintf_s(esc, sizeof esc, _TRUNCATE, "\\u%04x", c);
            mn_puts(b, esc);
        } else mn_put(b, (const char *)&c, 1);
    }
    mn_put(b, "\"", 1);
}

/* ------------------------------------------------------------ routes ---- */

typedef struct mn_hashset { char (*v)[MODNET_HASH_HEX + 1]; int n, cap; } mn_hashset;

static int mn_set_has(const mn_hashset *s, const char *h)
{
    int i;
    for (i = 0; i < s->n; i++) if (memcmp(s->v[i], h, MODNET_HASH_HEX) == 0) return 1;
    return 0;
}

static int mn_set_add(mn_hashset *s, const char *h)
{
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        char (*g)[MODNET_HASH_HEX + 1] = (char (*)[MODNET_HASH_HEX + 1])realloc(s->v, sizeof *s->v * (size_t)cap);
        if (!g) return 0;
        s->v = g;
        s->cap = cap;
    }
    memcpy(s->v[s->n], h, MODNET_HASH_HEX);
    s->v[s->n][MODNET_HASH_HEX] = 0;
    s->n++;
    return 1;
}

int modnet_missing(const char *const *hashes, int n, char (**missing)[MODNET_HASH_HEX + 1], int *missing_count,
                   const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    mn_hashset all = {0}, out = {0};
    double t0;
    int i, start, code = MODNET_OK;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (missing) *missing = NULL;
    if (missing_count) *missing_count = 0;
    if (!missing || !missing_count || n < 0 || (n && !hashes)) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    for (i = 0; i < n; i++) {
        if (!modnet_valid_hash(hashes[i])) { free(all.v); return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "bad blob hash")); }
        if (!mn_set_has(&all, hashes[i]) && !mn_set_add(&all, hashes[i])) { free(all.v); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    }
    for (start = 0; start < all.n && code == MODNET_OK; start += MODNET_MAX_QUERY) {
        int end = start + MODNET_MAX_QUERY < all.n ? start + MODNET_MAX_QUERY : all.n;
        mn_buf body = {0};
        mn_req q;
        unsigned char *resp = NULL;
        size_t rlen = 0;
        mn_puts(&body, "{\"blobs\":[");
        for (i = start; i < end; i++) {
            if (i > start) mn_puts(&body, ",");
            mn_puts(&body, "\"");
            mn_put(&body, all.v[i], MODNET_HASH_HEX);
            mn_puts(&body, "\"");
        }
        mn_puts(&body, "]}");
        if (body.bad) { free(body.p); code = mn_fail(r, MODNET_E_NOMEM, NULL); break; }
        memset(&q, 0, sizeof q);
        q.verb = L"POST";
        q.route = "/mods/missing";
        q.content_type = "application/json";
        q.body = (const unsigned char *)body.p;
        q.body_len = body.n;
        q.max_response = MN_MISSING_BODY;
        code = mn_exchange(&q, &resp, &rlen, io, r);
        free(body.p);
        if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "missing");
        else if (code == MODNET_OK) {
            mpj *root = mpj_parse((const char *)resp, rlen);
            const mpj *arr = root && root->type == MPJ_OBJ ? mpj_get(root, "missing") : NULL;
            if (!arr || arr->type != MPJ_ARR) code = mn_fail(r, MODNET_E_PROTOCOL, "missing: no list in the answer");
            for (i = 0; code == MODNET_OK && i < arr->n; i++) {
                const mpj *h = arr->items[i];
                /* Only hashes this request asked about may come back. */
                if (h->type != MPJ_STR || !modnet_valid_hash(h->text)) { code = mn_fail(r, MODNET_E_PROTOCOL, "missing: bad hash in the answer"); break; }
                {
                    int k, asked = 0;
                    for (k = start; k < end && !asked; k++) asked = memcmp(all.v[k], h->text, MODNET_HASH_HEX) == 0;
                    if (!asked) { code = mn_fail(r, MODNET_E_PROTOCOL, "missing: the answer names a hash not asked about"); break; }
                }
                if (!mn_set_has(&out, h->text) && !mn_set_add(&out, h->text)) code = mn_fail(r, MODNET_E_NOMEM, NULL);
            }
            mpj_free(root);
        }
        free(resp);
    }
    free(all.v);
    if (code != MODNET_OK) { free(out.v); return mn_end(r, t0, code); }
    *missing = out.v;
    *missing_count = out.n;
    return mn_end(r, t0, MODNET_OK);
}

int modnet_put_blob(const char *sha, const void *bytes, size_t len, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[96], actual[SH_MB_HASH_HEX + 1];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(sha) || (len && !bytes)) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    if (len > MODNET_MAX_BLOB) return mn_end(r, t0, mn_fail(r, MODNET_E_TOO_LARGE, "blob %.16s is over 64 MiB", sha));
    sh_mb_sha256_hex(bytes ? bytes : "", len, actual);
    if (strcmp(actual, sha) != 0) return mn_end(r, t0, mn_fail(r, MODNET_E_HASH, "blob %.16s: the bytes do not hash to its name", sha));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/blob/%s", sha);
    memset(&q, 0, sizeof q);
    q.verb = L"PUT";
    q.route = route;
    q.content_type = "application/octet-stream";
    q.body = (const unsigned char *)bytes;
    q.body_len = len;
    q.max_response = MN_SMALL_BODY;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    if (code == MODNET_OK) {
        if (r->status == 201) r->created = 1;
        else if (r->status != 200) code = mn_status_fail(r, resp, rlen, "blob");
    }
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_get_blob(const char *sha, unsigned long long expected_size, unsigned char **bytes, size_t *len,
                    const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[96], actual[SH_MB_HASH_HEX + 1];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (!modnet_valid_hash(sha) || !bytes || !len || expected_size > MODNET_MAX_BLOB)
        return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/blob/%s", sha);
    memset(&q, 0, sizeof q);
    q.verb = L"GET";
    q.route = route;
    q.max_response = expected_size ? expected_size : MODNET_MAX_BLOB;
    q.expect_total = expected_size;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "blob");
    else if (code == MODNET_OK) {
        if (expected_size && rlen != expected_size)
            code = mn_fail(r, MODNET_E_HASH, "blob %.16s: %zu bytes arrived, %llu expected", sha, rlen, expected_size);
        else {
            sh_mb_sha256_hex(resp, rlen, actual);
            if (strcmp(actual, sha) != 0) code = mn_fail(r, MODNET_E_HASH, "blob %.16s: the bytes do not hash to its name", sha);
        }
        if (code == MODNET_OK) { *bytes = resp; *len = rlen; resp = NULL; }
    }
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_put_bundle(const char *hash, const char *canonical, size_t len, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[96], actual[SH_MB_HASH_HEX + 1];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(hash) || !canonical || !len) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    if (len > MODNET_MAX_BUNDLE) return mn_end(r, t0, mn_fail(r, MODNET_E_TOO_LARGE, "bundle manifest over 1 MiB"));
    sh_mb_sha256_hex(canonical, len, actual);
    if (strcmp(actual, hash) != 0) return mn_end(r, t0, mn_fail(r, MODNET_E_HASH, "bundle %.16s: the manifest text does not hash to its name", hash));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/bundle/%s", hash);
    memset(&q, 0, sizeof q);
    q.verb = L"PUT";
    q.route = route;
    q.content_type = "application/json";
    q.body = (const unsigned char *)canonical;
    q.body_len = len;
    q.max_response = MN_SMALL_BODY;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    if (code == MODNET_OK) {
        if (r->status == 201) r->created = 1;
        else if (r->status != 200) code = mn_status_fail(r, resp, rlen, "bundle");
    }
    free(resp);
    return mn_end(r, t0, code);
}

/* Remove `"bundle":"<hash>"` and one adjacent comma from text (in place). */
static int mn_strip_bundle_member(char *text, size_t *len, const char *hash)
{
    char needle[96];
    char *at;
    size_t nl, start, end;
    _snprintf_s(needle, sizeof needle, _TRUNCATE, "\"bundle\":\"%s\"", hash);
    nl = strlen(needle);
    at = strstr(text, needle);
    if (!at) return 0;
    start = (size_t)(at - text);
    end = start + nl;
    if (start > 0 && text[start - 1] == ',') start--;
    else if (text[end] == ',') end++;
    memmove(text + start, text + end, *len - end + 1);
    *len -= end - start;
    return 1;
}

int modnet_get_bundle(const char *hash, char **text, size_t *len, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[96], actual[SH_MB_HASH_HEX + 1];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (text) *text = NULL;
    if (len) *len = 0;
    if (!modnet_valid_hash(hash) || !text || !len) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/bundle/%s", hash);
    memset(&q, 0, sizeof q);
    q.verb = L"GET";
    q.route = route;
    q.max_response = MODNET_MAX_BUNDLE;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "bundle");
    else if (code == MODNET_OK) {
        sh_mb_sha256_hex(resp, rlen, actual);
        if (strcmp(actual, hash) != 0) {
            /* The store also keeps a body that is the canonical text with
             * "bundle" included; without that member it must hash to its name. */
            if (!mn_strip_bundle_member((char *)resp, &rlen, hash)) code = mn_fail(r, MODNET_E_HASH, "bundle %.16s: the manifest does not hash to its name", hash);
            else {
                sh_mb_sha256_hex(resp, rlen, actual);
                if (strcmp(actual, hash) != 0) code = mn_fail(r, MODNET_E_HASH, "bundle %.16s: the manifest does not hash to its name", hash);
            }
        }
        if (code == MODNET_OK) { *text = (char *)resp; *len = rlen; resp = NULL; }
    }
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_claim(const char *ref, const char *const *bundles, int n, const char *secret,
                 char claim_out[MODNET_CLAIM_HEX + 1], char secret_out[MODNET_HASH_HEX + 1],
                 const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    mn_hashset set = {0};
    mn_buf body = {0};
    unsigned char *resp = NULL;
    size_t rlen = 0;
    mn_req q;
    double t0;
    int code, i;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (claim_out) claim_out[0] = 0;
    if (secret_out) secret_out[0] = 0;
    if (!claim_out || !secret_out || !modnet_valid_ref(ref) || n <= 0 || !bundles || (secret && !modnet_valid_hash(secret)))
        return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    for (i = 0; i < n; i++) {
        if (!modnet_valid_hash(bundles[i])) { free(set.v); return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "bad bundle hash")); }
        if (!mn_set_has(&set, bundles[i]) && !mn_set_add(&set, bundles[i])) { free(set.v); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    }
    if (set.n > MODNET_MAX_CLAIM) { free(set.v); return mn_end(r, t0, mn_fail(r, MODNET_E_TOO_LARGE, "more than %d bundles in one claim", MODNET_MAX_CLAIM)); }
    mn_puts(&body, "{\"bundles\":[");
    for (i = 0; i < set.n; i++) {
        if (i) mn_puts(&body, ",");
        mn_puts(&body, "\"");
        mn_put(&body, set.v[i], MODNET_HASH_HEX);
        mn_puts(&body, "\"");
    }
    mn_puts(&body, "],\"ref\":");
    mn_put_json_string(&body, ref);
    if (secret) { mn_puts(&body, ",\"secret\":\""); mn_puts(&body, secret); mn_puts(&body, "\""); }
    mn_puts(&body, "}");
    free(set.v);
    if (body.bad) { free(body.p); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    memset(&q, 0, sizeof q);
    q.verb = L"POST";
    q.route = "/mods/claim";
    q.content_type = "application/json";
    q.body = (const unsigned char *)body.p;
    q.body_len = body.n;
    q.max_response = MN_SMALL_BODY;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    SecureZeroMemory(body.p, body.n);
    free(body.p);
    if (code == MODNET_OK && r->status != 200 && r->status != 201) code = mn_status_fail(r, resp, rlen, "claim");
    else if (code == MODNET_OK) {
        mpj *root = mpj_parse((const char *)resp, rlen);
        const mpj *c = root && root->type == MPJ_OBJ ? mpj_get(root, "claim") : NULL;
        const mpj *s = root && root->type == MPJ_OBJ ? mpj_get(root, "secret") : NULL;
        if (!c || c->type != MPJ_STR || !mn_valid_hex(c->text, MODNET_CLAIM_HEX) ||
            !s || s->type != MPJ_STR || !modnet_valid_hash(s->text) || (secret && strcmp(s->text, secret) != 0))
            code = mn_fail(r, MODNET_E_PROTOCOL, "claim: the answer is not a claim id and secret");
        else {
            strcpy_s(claim_out, MODNET_CLAIM_HEX + 1, c->text);
            strcpy_s(secret_out, MODNET_HASH_HEX + 1, s->text);
            r->created = r->status == 201;
        }
        mpj_free(root);
    }
    if (resp) { SecureZeroMemory(resp, rlen); free(resp); }
    return mn_end(r, t0, code);
}

int modnet_release(const char *claim, const char *secret, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char body[160];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!mn_valid_hex(claim, MODNET_CLAIM_HEX) || !modnet_valid_hash(secret)) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"claim\":\"%s\",\"secret\":\"%s\"}", claim, secret);
    memset(&q, 0, sizeof q);
    q.verb = L"POST";
    q.route = "/mods/release";
    q.content_type = "application/json";
    q.body = (const unsigned char *)body;
    q.body_len = strlen(body);
    q.max_response = MN_SMALL_BODY;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    SecureZeroMemory(body, sizeof body);
    if (code == MODNET_OK && r->status != 204 && r->status != 200) code = mn_status_fail(r, resp, rlen, "release");
    free(resp);
    return mn_end(r, t0, code);
}

/* ------------------------------------------------------------ jobs ------ */

static SRWLOCK g_job_lock = SRWLOCK_INIT;
static int     g_job_running;
static volatile long g_job_cancel;
static HANDLE  g_job_idle;            /* manual-reset, signalled while idle */
static char    g_job_name[64];
static modnet_job_fn      g_job_run;
static modnet_job_done_fn g_job_done;
static modnet_progress_fn g_job_progress;
static void   *g_job_progress_ctx, *g_job_ctx;

static DWORD WINAPI mn_job_thread(LPVOID unused)
{
    modnet_io io;
    int result;
    (void)unused;
    io.cancel = &g_job_cancel;
    io.progress = g_job_progress;
    io.context = g_job_progress_ctx;
    result = g_job_run(g_job_ctx, &io);
    if (g_job_done) g_job_done(g_job_ctx, result);
    AcquireSRWLockExclusive(&g_job_lock);
    g_job_running = 0;
    g_job_name[0] = 0;
    SetEvent(g_job_idle);
    ReleaseSRWLockExclusive(&g_job_lock);
    return 0;
}

int modnet_job_start(const char *name, modnet_job_fn run, modnet_job_done_fn done,
                     modnet_progress_fn progress, void *progress_context, void *context)
{
    HANDLE t;
    if (!run) return -1;
    AcquireSRWLockExclusive(&g_job_lock);
    if (g_job_running) { ReleaseSRWLockExclusive(&g_job_lock); return 0; }
    if (!g_job_idle) g_job_idle = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!g_job_idle) { ReleaseSRWLockExclusive(&g_job_lock); return -1; }
    g_job_running = 1;
    InterlockedExchange(&g_job_cancel, 0);
    strncpy_s(g_job_name, sizeof g_job_name, name ? name : "job", _TRUNCATE);
    g_job_run = run;
    g_job_done = done;
    g_job_progress = progress;
    g_job_progress_ctx = progress_context;
    g_job_ctx = context;
    ResetEvent(g_job_idle);
    t = CreateThread(NULL, 0, mn_job_thread, NULL, 0, NULL);
    if (!t) {
        g_job_running = 0;
        g_job_name[0] = 0;
        SetEvent(g_job_idle);
        ReleaseSRWLockExclusive(&g_job_lock);
        return -1;
    }
    CloseHandle(t);
    ReleaseSRWLockExclusive(&g_job_lock);
    return 1;
}

int modnet_job_cancel(void)
{
    int running;
    AcquireSRWLockShared(&g_job_lock);
    running = g_job_running;
    if (running) InterlockedExchange(&g_job_cancel, 1);
    ReleaseSRWLockShared(&g_job_lock);
    return running;
}

int modnet_job_busy(char *name, size_t cap)
{
    int running;
    AcquireSRWLockShared(&g_job_lock);
    running = g_job_running;
    if (name && cap) strncpy_s(name, cap, running ? g_job_name : "", _TRUNCATE);
    ReleaseSRWLockShared(&g_job_lock);
    return running;
}

int modnet_job_wait(unsigned timeout_ms)
{
    HANDLE ev;
    AcquireSRWLockShared(&g_job_lock);
    ev = g_job_idle;
    ReleaseSRWLockShared(&g_job_lock);
    if (!ev) return 1;
    return WaitForSingleObject(ev, timeout_ms) == WAIT_OBJECT_0;
}
