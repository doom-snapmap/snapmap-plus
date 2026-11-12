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
#include "text.h"
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
#define MN_RESOLVE_MS   15000
#define MN_CONNECT_MS   15000
#define MN_SEND_MS      30000
#define MN_RECEIVE_MS   30000
/* Joining an archive's parts: the store reads the whole archive back once to
 * check it before it answers, so the answer waits that long. */
#define MN_COMPLETE_MS  (15u * 60u * 1000u)
#define MN_PART_TRIES   4       /* starts of one upload in parts, each resuming the last */

/* ------------------------------------------------------------ session ---- */

static SRWLOCK   g_lock = SRWLOCK_INIT;
static HINTERNET g_session;
static char     *g_override;         /* malloc'd; NULL: the setting */
static wchar_t  *g_host;             /* malloc'd, while a session is open */
static char     *g_session_url;      /* the base URL the pooled session was opened with (malloc'd) */
static INTERNET_PORT g_port;
static int       g_secure;
static wchar_t  *g_prefix;           /* base path without a trailing '/' (malloc'd) */

static double mn_now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

/* The message of the last call on this thread: the whole sentence, kept
 * until the thread's next call replaces it. */
static __declspec(thread) char *t_mn_message;

static void mn_message(modnet_result *r, char *text)
{
    if (!text) { r->message = modnet_code_text(r->code); return; }
    free(t_mn_message);
    t_mn_message = text;
    r->message = t_mn_message;
}

static int mn_fail(modnet_result *r, int code, const char *fmt, ...)
{
    if (r) {
        r->code = code;
        if (fmt) {
            va_list ap;
            va_start(ap, fmt);
            mn_message(r, sh_vtextf(fmt, ap));
            va_end(ap);
        } else {
            r->message = modnet_code_text(code);
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
    case MODNET_E_GAME:        return "it needs game content this game does not hold";
    default:                   return "error";
    }
}

int modnet_unreachable(int code)
{
    switch (code) {
    case MODNET_E_CONFIG: case MODNET_E_NET: case MODNET_E_TIMEOUT: case MODNET_E_HTTP: case MODNET_E_PROTOCOL:
    case MODNET_E_RATE: case MODNET_E_UNAVAILABLE:
        return 1;
    default:
        return 0;
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
    int units, ok = 1;
    const unsigned char *p;
    wchar_t *w;
    if (!ref || !ref[0]) return 0;
    for (p = (const unsigned char *)ref; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return 0;
    units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ref, -1, NULL, 0);
    if (units <= 1 || !(w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)units))) return 0;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ref, -1, w, units) <= 0) ok = 0;
    for (units = 0; ok && w[units]; units++)
        if (w[units] >= 0x80 && w[units] <= 0x9f) ok = 0;   /* C1 controls, as the store refuses */
    free(w);
    return ok;
}

static int mn_is_loopback(const wchar_t *host)
{
    return _wcsicmp(host, L"127.0.0.1") == 0 || _wcsicmp(host, L"localhost") == 0 ||
           _wcsicmp(host, L"[::1]") == 0 || _wcsicmp(host, L"::1") == 0;
}

/* The base URL in effect, given the override (read under the lock). */
static char *mn_url_from(const char *override)
{
    char *url;
    if (override) return _strdup(override);
    url = sh_config_get_text("mods.store");
    if (url && url[0]) return url;
    free(url);
    return _strdup(MODNET_DEFAULT_STORE);
}

char *modnet_base_url(void)
{
    char *url;
    AcquireSRWLockShared(&g_lock);
    url = g_override ? _strdup(g_override) : NULL;
    ReleaseSRWLockShared(&g_lock);
    return url ? url : mn_url_from(NULL);
}

/* Parse a base URL into host, port, scheme and path prefix (each malloc'd).
 * The address is any length; WinHTTP points into it, so nothing is cut. */
static int mn_parse_url(const char *url, wchar_t **host, INTERNET_PORT *port, int *secure, wchar_t **prefix,
                        char *why, size_t why_cap)
{
    wchar_t *wurl = NULL, *whost = NULL, *wpath = NULL;
    URL_COMPONENTS uc;
    size_t n;
    int units, ok = 0;
    *host = *prefix = NULL;
    units = url && url[0] ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, NULL, 0) : 0;
    if (units <= 1 || !(wurl = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)units)) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url, -1, wurl, units)) {
        strncpy_s(why, why_cap, "the mod store address is empty or not text", _TRUNCATE);
        goto done;
    }
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = (DWORD)-1;   /* point into wurl: no buffer to fit */
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !uc.lpszHostName || !uc.dwHostNameLength) {
        strncpy_s(why, why_cap, "the mod store address is not a URL", _TRUNCATE);
        goto done;
    }
    whost = (wchar_t *)malloc(sizeof(wchar_t) * ((size_t)uc.dwHostNameLength + 1));
    wpath = (wchar_t *)malloc(sizeof(wchar_t) * ((size_t)uc.dwUrlPathLength + 1));
    if (!whost || !wpath) { strncpy_s(why, why_cap, "out of memory", _TRUNCATE); goto done; }
    wmemcpy(whost, uc.lpszHostName, uc.dwHostNameLength);
    whost[uc.dwHostNameLength] = 0;
    if (uc.dwUrlPathLength) wmemcpy(wpath, uc.lpszUrlPath, uc.dwUrlPathLength);
    wpath[uc.dwUrlPathLength] = 0;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) *secure = 1;
    else if (uc.nScheme == INTERNET_SCHEME_HTTP && mn_is_loopback(whost)) *secure = 0;
    else {
        strncpy_s(why, why_cap, "the mod store address must use https (http only for a loopback test host)", _TRUNCATE);
        goto done;
    }
    if (uc.dwExtraInfoLength || wcschr(wpath, L'?') || wcschr(wpath, L'#')) {
        strncpy_s(why, why_cap, "the mod store address must not carry a query", _TRUNCATE);
        goto done;
    }
    n = wcslen(wpath);
    while (n && wpath[n - 1] == L'/') wpath[--n] = 0;
    *host = whost;
    *prefix = wpath;
    whost = wpath = NULL;
    *port = uc.nPort;
    ok = 1;
done:
    free(wurl);
    free(whost);
    free(wpath);
    return ok;
}

static void mn_close_session_locked(void)
{
    if (g_session) WinHttpCloseHandle(g_session);
    g_session = NULL;
    free(g_host);
    free(g_prefix);
    free(g_session_url);
    g_host = g_prefix = NULL;
    g_session_url = NULL;
}

int modnet_set_base_url(const char *url)
{
    wchar_t *host = NULL, *prefix = NULL;
    INTERNET_PORT port = 0;
    int secure = 0;
    char why[160], *copy = NULL;
    if (url && url[0]) {
        if (!mn_parse_url(url, &host, &port, &secure, &prefix, why, sizeof why)) return 0;
        free(host);
        free(prefix);
        if (!(copy = _strdup(url))) return 0;
    }
    AcquireSRWLockExclusive(&g_lock);   /* waits for requests in flight */
    mn_close_session_locked();
    free(g_override);
    g_override = copy;
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
    char *url;
    DWORD protocols;
    if (g_session) return 1;
    /* modnet_base_url takes the shared lock; read the override directly. */
    if (!(url = mn_url_from(g_override))) { strncpy_s(why, why_cap, "out of memory", _TRUNCATE); return 0; }
    if (!mn_parse_url(url, &g_host, &g_port, &g_secure, &g_prefix, why, why_cap)) { free(url); return 0; }
    g_session_url = url;
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
        mn_close_session_locked();
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
    /* The setting is read on every request so a changed "mods.store" takes
     * effect at the next request, not at the next launch: a session opened
     * for another base URL is dropped (after the requests in flight finish)
     * and reopened for the current one. */
    char *wanted = modnet_base_url();
    for (;;) {
        AcquireSRWLockShared(&g_lock);
        if (g_session && (!wanted || strcmp(g_session_url, wanted) == 0)) { free(wanted); return 1; }
        ReleaseSRWLockShared(&g_lock);
        AcquireSRWLockExclusive(&g_lock);
        if (g_session && wanted && strcmp(g_session_url, wanted) != 0) mn_close_session_locked();
        if (!mn_open_session_locked(why, why_cap)) { ReleaseSRWLockExclusive(&g_lock); free(wanted); return 0; }
        ReleaseSRWLockExclusive(&g_lock);
    }
}

/* ------------------------------------------------------------ exchange --- */

typedef struct mn_req {
    const wchar_t *verb;
    const char *route;             /* "/mods/..." */
    const char *content_type;      /* NULL: no body */
    const unsigned char *body;     /* the body in memory, or ... */
    HANDLE body_file;              /* ... read from this file (body_len bytes from body_offset) */
    unsigned long long body_offset;
    size_t body_len;
    const wchar_t *headers;        /* extra request headers, each ending in CRLF, or NULL */
    DWORD receive_ms;              /* 0: MN_RECEIVE_MS */
    unsigned *scripts;             /* the answer's X-Mod-Scripts, when it has one */
    unsigned long long max_response;
    unsigned long long expect_total;   /* download progress total when no Content-Length */
    HANDLE sink;                   /* a 2xx body is written here, not returned */
    int head;                      /* HEAD: no body is read */
    unsigned long long *length;    /* the response's declared Content-Length, when it has one */
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
    wchar_t *path = NULL, *wroute = NULL, *headers = NULL, *ct = NULL;
    int units;
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
    units = MultiByteToWideChar(CP_UTF8, 0, q->route, -1, NULL, 0);
    if (units <= 0 || !(wroute = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)units)) ||
        !MultiByteToWideChar(CP_UTF8, 0, q->route, -1, wroute, units)) {
        free(wroute);
        return mn_fail(r, MODNET_E_ARG, "bad route");
    }
    why[0] = 0;
    if (!mn_acquire(why, sizeof why)) {
        free(wroute);
        return mn_fail(r, MODNET_E_CONFIG, "%s", why[0] ? why : "no mod store address");
    }
    path = sh_wtextf(L"%s%s", g_prefix, wroute);
    if (!path) { code = mn_fail(r, MODNET_E_ARG, "out of memory"); goto done; }
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
    if (q->receive_ms) WinHttpSetTimeouts(req, MN_RESOLVE_MS, MN_CONNECT_MS, MN_SEND_MS, (int)q->receive_ms);
    if (q->content_type) {
        units = MultiByteToWideChar(CP_UTF8, 0, q->content_type, -1, NULL, 0);
        if (units > 0 && (ct = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)units)))
            MultiByteToWideChar(CP_UTF8, 0, q->content_type, -1, ct, units);
        /* Content-Length is written by WinHTTP from the total length below,
         * zero included, for every request that has a body. */
        if (ct)
            headers = sh_wtextf(L"Content-Type: %s\r\nAccept: application/json, application/octet-stream\r\n%s", ct,
                                q->headers ? q->headers : L"");
    } else {
        headers = sh_wtextf(L"Accept: application/json, application/octet-stream\r\n");
    }
    if (!headers) { code = mn_fail(r, MODNET_E_ARG, "out of memory"); goto done; }
    r->requests++;
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0,
                            q->content_type ? (DWORD)q->body_len : 0, 0)) {
        code = mn_winhttp_error(r, "send");
        goto done;
    }
    if (q->content_type && q->body_len) {
        size_t off = 0;
        unsigned char *chunkbuf = NULL;
        if (q->body_file) {
            LARGE_INTEGER from;
            from.QuadPart = (LONGLONG)q->body_offset;
            chunkbuf = (unsigned char *)malloc(MN_CHUNK);
            if (!chunkbuf) { code = mn_fail(r, MODNET_E_NOMEM, NULL); goto done; }
            if (!SetFilePointerEx(q->body_file, from, NULL, FILE_BEGIN)) {
                free(chunkbuf);
                code = mn_fail(r, MODNET_E_ARG, "the archive file cannot be read (error %lu)", GetLastError());
                goto done;
            }
        }
        while (off < q->body_len) {
            DWORD chunk = (DWORD)((q->body_len - off) > MN_CHUNK ? MN_CHUNK : (q->body_len - off)), wrote = 0;
            const unsigned char *from = q->body + off;
            if (mn_cancelled(io)) { free(chunkbuf); code = mn_fail(r, MODNET_E_CANCELLED, NULL); goto done; }
            if (chunkbuf) {
                DWORD rd = 0;
                if (!ReadFile(q->body_file, chunkbuf, chunk, &rd, NULL) || rd != chunk) {
                    free(chunkbuf);
                    code = mn_fail(r, MODNET_E_ARG, "the archive file changed while it was sent");
                    goto done;
                }
                from = chunkbuf;
            }
            if (!WinHttpWriteData(req, from, chunk, &wrote) || wrote != chunk) {
                free(chunkbuf);
                code = mn_winhttp_error(r, "upload");
                goto done;
            }
            off += wrote;
            r->sent += wrote;
            /* Progress follows an archive's bytes, never a small JSON body. */
            if (io && io->progress && q->body_file) io->progress(io->context, off, q->body_len);
        }
        free(chunkbuf);
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
    if (q->length && have_declared) *q->length = declared;
    if (q->scripts) {
        wchar_t sv[32];
        DWORD n = sizeof sv;
        *q->scripts = 0;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"X-Mod-Scripts", sv, &n, WINHTTP_NO_HEADER_INDEX))
            *q->scripts = (unsigned)wcstoul(sv, NULL, 10);
    }
    if (q->head) {
        /* A HEAD answer has no body, whatever its Content-Length says. */
        buf = (unsigned char *)malloc(1);
        if (!buf) { code = mn_fail(r, MODNET_E_NOMEM, NULL); goto done; }
        buf[0] = 0;
        *resp = buf;
        buf = NULL;
        goto done;
    }
    /* A refusal's JSON error is read whole: its sentence may name a path of
     * any length. */
    cap_bytes = status >= 300 ? (unsigned long long)-1 : q->max_response;
    if (status < 300 && have_declared && declared > cap_bytes) {
        code = mn_fail(r, MODNET_E_TOO_LARGE, "the response is larger than %llu bytes", cap_bytes);
        goto done;
    }
    if (q->sink && status < 300) cap = MN_CHUNK + 1;   /* one chunk at a time, written out */
    else cap = (size_t)(have_declared && declared < cap_bytes ? declared : (cap_bytes < 65536 ? cap_bytes : 65536)) + 1;
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
        if (q->sink && status < 300) {
            /* Streamed to the file, one chunk at a time. */
            DWORD left = avail;
            while (left) {
                DWORD want = left > MN_CHUNK ? MN_CHUNK : left, put = 0;
                if (!WinHttpReadData(req, buf, want, &rd)) { code = mn_winhttp_error(r, "download"); goto done; }
                if (rd == 0) break;
                if (!WriteFile(q->sink, buf, rd, &put, NULL) || put != rd) {
                    code = mn_fail(r, MODNET_E_ARG, "the download cannot be written (error %lu)", GetLastError());
                    goto done;
                }
                got += rd;
                r->received += rd;
                left -= rd < left ? rd : left;
                if (io && io->progress) io->progress(io->context, got, have_declared ? declared : q->expect_total);
            }
            if (left) break;
            continue;
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
    if (q->sink && status < 300) got = 0;   /* the body went to the file */
    buf[got] = 0;
    *resp = buf;
    *resp_len = got;
    buf = NULL;
done:
    free(buf);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    ReleaseSRWLockShared(&g_lock);
    free(path);
    free(wroute);
    free(headers);
    free(ct);
    if (code == MODNET_OK) r->code = MODNET_OK;
    return code;
}

/* The store's {"error": "..."} text, whole, printable ASCII only (malloc'd),
 * or NULL. */
static char *mn_error_text(const unsigned char *body, size_t len)
{
    mpj *root;
    const mpj *e;
    size_t i, w = 0;
    char *out = NULL;
    if (!body || !len) return NULL;
    root = mpj_parse((const char *)body, len);
    if (!root) return NULL;
    e = root->type == MPJ_OBJ ? mpj_get(root, "error") : NULL;
    if (e && e->type == MPJ_STR && e->text && (out = (char *)malloc(strlen(e->text) + 1)) != NULL) {
        for (i = 0; e->text[i]; i++) {
            unsigned char c = (unsigned char)e->text[i];
            if (c >= 0x20 && c < 0x7f) out[w++] = (char)c;
        }
        out[w] = 0;
        if (!w) { free(out); out = NULL; }
    }
    mpj_free(root);
    return out;
}

/* Map a refusal status onto a code with the store's cause. */
static int mn_status_fail(modnet_result *r, const unsigned char *body, size_t len, const char *what)
{
    char *cause;
    int code;
    int s = r->status;
    cause = mn_error_text(body, len);
    if (s == 400 || s == 411 || s == 415 || s == 422) code = MODNET_E_REFUSED;
    else if (s == 403) code = MODNET_E_FORBIDDEN;
    else if (s == 404) code = MODNET_E_NOT_FOUND;
    else if (s == 409) code = MODNET_E_CONFLICT;
    else if (s == 413) code = MODNET_E_TOO_LARGE;
    else if (s == 429) code = MODNET_E_RATE;
    else if (s == 451) code = MODNET_E_BLOCKED;
    else if (s >= 500 && s < 600) code = MODNET_E_UNAVAILABLE;
    else code = MODNET_E_HTTP;
    /* The store's own sentence is the message, alone; the status stays in
     * r->status for the caller's log. */
    (void)what;
    r->code = code;
    if (cause) mn_message(r, cause);
    else r->message = modnet_code_text(code);
    return code;
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
    if (code == MODNET_OK) r->message = "";
    else if (!r->message || !r->message[0]) r->message = modnet_code_text(code);
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

int modnet_head_build(const char *hash, unsigned long long *size, unsigned *scripts, const modnet_io *io,
                      modnet_result *r)
{
    modnet_result local;
    char route[96];
    unsigned char *resp = NULL;
    unsigned long long length = 0;
    size_t rlen = 0;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (size) *size = 0;
    if (scripts) *scripts = 0;
    if (!modnet_valid_hash(hash)) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/build/%s", hash);
    memset(&q, 0, sizeof q);
    q.verb = L"HEAD";
    q.route = route;
    q.head = 1;
    q.length = &length;
    q.scripts = scripts;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, NULL, 0, "build");
    else if (code == MODNET_OK && size) *size = length;
    free(resp);
    return mn_end(r, t0, code);
}

/* Progress of one request of an upload in parts, as progress of the whole
 * archive: `base` bytes are already at the store. */
typedef struct mn_part_progress { const modnet_io *outer; unsigned long long base, total; } mn_part_progress;

static void mn_part_progress_cb(void *ctx, unsigned long long done, unsigned long long total)
{
    mn_part_progress *p = (mn_part_progress *)ctx;
    (void)total;
    p->outer->progress(p->outer->context, p->base + done, p->total);
}

/* One small JSON request of an upload in parts ({"size":N} to `route`). */
static int mn_parts_post(const char *route, unsigned long long size, DWORD receive_ms, unsigned char **resp, size_t *rlen,
                         const modnet_io *io, modnet_result *r)
{
    char body[64];
    mn_req q;
    modnet_io quiet;   /* the cancel flag only: progress follows the archive's bytes */
    quiet.cancel = io ? io->cancel : NULL;
    quiet.progress = NULL;
    quiet.context = NULL;
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"size\":%llu}", size);
    memset(&q, 0, sizeof q);
    q.verb = L"POST";
    q.route = route;
    q.content_type = "application/json";
    q.body = (const unsigned char *)body;
    q.body_len = strlen(body);
    q.max_response = MN_SMALL_BODY;
    q.receive_ms = receive_ms;
    return mn_exchange(&q, resp, rlen, &quiet, r);
}

/* The store's account of an upload in parts: {"partSize","parts","have":[...]},
 * or {"stored":true} when it holds the build already. `have` (malloc'd, one
 * flag per part) is set when it is not stored. */
static int mn_parts_state(const unsigned char *resp, size_t rlen, unsigned long long size, unsigned long long *part_size,
                          unsigned *parts, unsigned char **have, int *stored)
{
    mpj *root = mpj_parse((const char *)resp, rlen);
    const mpj *ps, *pn, *hv, *st;
    int ok = 0, i;
    *have = NULL;
    *stored = 0;
    if (!root || root->type != MPJ_OBJ) goto done;
    st = mpj_get(root, "stored");
    if (st && st->type == MPJ_BOOL && st->b) { *stored = 1; ok = 1; goto done; }
    ps = mpj_get(root, "partSize");
    pn = mpj_get(root, "parts");
    hv = mpj_get(root, "have");
    if (!ps || !mpj_is_num(ps) || mpj_int(ps) < 1 || !pn || !mpj_is_num(pn) || mpj_int(pn) < 1 || !hv || hv->type != MPJ_ARR)
        goto done;
    *part_size = (unsigned long long)mpj_int(ps);
    *parts = (unsigned)mpj_int(pn);
    if ((size + *part_size - 1) / *part_size != *parts) goto done;
    *have = (unsigned char *)calloc(*parts, 1);
    if (!*have) goto done;
    for (i = 0; i < hv->n; i++) {
        long long n = mpj_int(hv->items[i]);
        if (n >= 1 && (unsigned long long)n <= *parts) (*have)[n - 1] = 1;
    }
    ok = 1;
done:
    mpj_free(root);
    return ok;
}

/* An archive larger than one request, in parts: start (or resume), send the
 * parts the store does not hold, complete. A failure a retry could fix
 * starts again, and the store's answer then names the parts already held. */
static int mn_put_parts(const char *hash, HANDLE h, unsigned long long size, const modnet_io *io, modnet_result *r)
{
    char start[128], complete[128];
    int tries, code = MODNET_E_PROTOCOL;
    _snprintf_s(start, sizeof start, _TRUNCATE, "/mods/build/%s/parts", hash);
    _snprintf_s(complete, sizeof complete, _TRUNCATE, "/mods/build/%s/complete", hash);
    for (tries = 0; tries < MN_PART_TRIES; tries++) {
        unsigned char *resp = NULL, *have = NULL;
        size_t rlen = 0;
        unsigned long long part_size = 0, held = 0;
        unsigned parts = 0, n;
        int stored = 0;
        code = mn_parts_post(start, size, 0, &resp, &rlen, io, r);
        if (code == MODNET_OK && r->status != 200 && r->status != 201) code = mn_status_fail(r, resp, rlen, "parts");
        else if (code == MODNET_OK && !mn_parts_state(resp, rlen, size, &part_size, &parts, &have, &stored))
            code = mn_fail(r, MODNET_E_PROTOCOL, "the store's answer to an upload in parts is malformed");
        free(resp);
        if (code != MODNET_OK) { free(have); if (code == MODNET_E_CANCELLED || !modnet_transient(code)) return code; continue; }
        if (stored) { r->created = 0; return MODNET_OK; }
        for (n = 0; n < parts; n++) if (have[n]) held += n + 1 < parts ? part_size : size - part_size * n;
        for (n = 0; n < parts && code == MODNET_OK; n++) {
            char route[160];
            wchar_t extra[64];
            mn_part_progress pp;
            modnet_io sub;
            mn_req q;
            unsigned long long at = part_size * n, len = n + 1 < parts ? part_size : size - at;
            if (have[n]) continue;
            _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/build/%s/parts/%u", hash, n + 1);
            _snwprintf_s(extra, 64, _TRUNCATE, L"X-Build-Size: %llu\r\n", size);
            pp.outer = io;
            pp.base = held;
            pp.total = size;
            sub.cancel = io ? io->cancel : NULL;
            sub.progress = io && io->progress ? mn_part_progress_cb : NULL;
            sub.context = &pp;
            memset(&q, 0, sizeof q);
            q.verb = L"PUT";
            q.route = route;
            q.content_type = "application/octet-stream";
            q.headers = extra;
            q.body_file = h;
            q.body_offset = at;
            q.body_len = (size_t)len;
            q.max_response = MN_SMALL_BODY;
            resp = NULL;
            code = mn_exchange(&q, &resp, &rlen, &sub, r);
            if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "part");
            free(resp);
            if (code == MODNET_OK) { held += len; r->parts++; }
        }
        free(have);
        if (code == MODNET_OK) {
            double t0 = mn_now_ms();
            resp = NULL;
            code = mn_parts_post(complete, size, MN_COMPLETE_MS, &resp, &rlen, io, r);
            r->check_ms += mn_now_ms() - t0;
            if (code == MODNET_OK) {
                if (r->status == 201) r->created = 1;
                else if (r->status != 200) code = mn_status_fail(r, resp, rlen, "complete");
            }
            free(resp);
            if (code == MODNET_OK) return MODNET_OK;
        }
        /* The upload was abandoned or its parts did not join (404, 409): the
         * next start says what the store holds. */
        if (code == MODNET_E_CANCELLED || (!modnet_transient(code) && code != MODNET_E_NOT_FOUND)) return code;
    }
    return code;
}

int modnet_put_build(const char *hash, const char *path, unsigned long long size, const modnet_io *io,
                     modnet_result *r)
{
    modnet_result local;
    char route[96];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    wchar_t *wpath;
    HANDLE h;
    LARGE_INTEGER actual;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(hash) || !path || !path[0] || !size) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    wpath = sh_mb_wpath(path);
    if (!wpath) return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL));
    h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE)
        return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "the build archive cannot be opened (error %lu)", GetLastError()));
    if (!GetFileSizeEx(h, &actual) || (unsigned long long)actual.QuadPart != size) {
        CloseHandle(h);
        return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "the build archive is not the size given"));
    }
    if (size > MODNET_PART_SIZE) {
        code = mn_put_parts(hash, h, size, io, r);
        CloseHandle(h);
        return mn_end(r, t0, code);
    }
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/build/%s", hash);
    memset(&q, 0, sizeof q);
    q.verb = L"PUT";
    q.route = route;
    q.content_type = MODNET_ARCHIVE_TYPE;
    q.body_file = h;
    q.body_len = (size_t)size;
    q.max_response = MN_SMALL_BODY;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    CloseHandle(h);
    if (code == MODNET_OK) {
        if (r->status == 201) r->created = 1;
        else if (r->status != 200) code = mn_status_fail(r, resp, rlen, "build");
    }
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_get_build(const char *hash, const char *out_path, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[96];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    wchar_t *wpath;
    HANDLE h;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(hash) || !out_path || !out_path[0]) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    wpath = sh_mb_wpath(out_path);
    if (!wpath) return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL));
    SetFileAttributesW(wpath, FILE_ATTRIBUTE_NORMAL);
    h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                    NULL);
    if (h == INVALID_HANDLE_VALUE) {
        code = mn_fail(r, MODNET_E_ARG, "the download cannot be written (error %lu)", GetLastError());
        free(wpath);
        return mn_end(r, t0, code);
    }
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/build/%s", hash);
    memset(&q, 0, sizeof q);
    q.verb = L"GET";
    q.route = route;
    q.max_response = ~0ull;   /* any size: it streams to the file */
    q.sink = h;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "build");
    else if (code == MODNET_OK && r->received == 0)
        code = mn_fail(r, MODNET_E_PROTOCOL, "build %.16s: the store sent nothing", hash);
    if (code == MODNET_OK && !FlushFileBuffers(h))
        code = mn_fail(r, MODNET_E_ARG, "the download cannot be written (error %lu)", GetLastError());
    CloseHandle(h);
    if (code != MODNET_OK) DeleteFileW(wpath);
    free(wpath);
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_claim(const char *ref, const char *const *builds, int n, const char *secret,
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
    if (!claim_out || !secret_out || !modnet_valid_ref(ref) || n <= 0 || !builds || (secret && !modnet_valid_hash(secret)))
        return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    for (i = 0; i < n; i++) {
        if (!modnet_valid_hash(builds[i])) { free(set.v); return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "bad build hash")); }
        if (!mn_set_has(&set, builds[i]) && !mn_set_add(&set, builds[i])) { free(set.v); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    }
    mn_puts(&body, "{\"builds\":[");
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

int modnet_report_asked(const char *hash, const char *request, const char *by, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    mn_buf body = {0};
    unsigned char *resp = NULL;
    size_t rlen = 0;
    char *route;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(hash) || !request || !request[0]) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    mn_puts(&body, "{\"by\":");
    mn_put_json_string(&body, by ? by : "");
    mn_puts(&body, ",\"request\":");
    mn_put_json_string(&body, request);
    mn_puts(&body, "}");
    route = sh_textf("/mods/build/%s/asked", hash);
    if (body.bad || !route) { free(body.p); free(route); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    memset(&q, 0, sizeof q);
    q.verb = L"POST";
    q.route = route;
    q.content_type = "application/json";
    q.body = (const unsigned char *)body.p;
    q.body_len = body.n;
    q.max_response = MN_SMALL_BODY;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    if (code == MODNET_OK && r->status != 204 && r->status != 200) code = mn_status_fail(r, resp, rlen, "asked");
    free(resp);
    free(body.p);
    free(route);
    return mn_end(r, t0, code);
}

int modnet_list_asked(const char *claim, const char *secret, char **json, const modnet_io *io, modnet_result *r)
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
    if (json) *json = NULL;
    if (!json || !mn_valid_hex(claim, MODNET_CLAIM_HEX) || !modnet_valid_hash(secret))
        return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"claim\":\"%s\",\"secret\":\"%s\"}", claim, secret);
    memset(&q, 0, sizeof q);
    q.verb = L"POST";
    q.route = "/mods/asked";
    q.content_type = "application/json";
    q.body = (const unsigned char *)body;
    q.body_len = strlen(body);
    q.max_response = ~0ull;   /* as many as players asked for */
    code = mn_exchange(&q, &resp, &rlen, io, r);
    SecureZeroMemory(body, sizeof body);
    if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "asked");
    else if (code == MODNET_OK) {
        char *text = (char *)malloc(rlen + 1);
        if (!text) code = mn_fail(r, MODNET_E_NOMEM, NULL);
        else {
            if (rlen) memcpy(text, resp, rlen);
            text[rlen] = 0;
            *json = text;
        }
    }
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
