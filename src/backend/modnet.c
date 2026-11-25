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

/* The release tag the build stamps (build.ps1 -Version), "dev" for a local build; the store counts clients by it and
 * refuses retired ones. */
#ifndef SNAPMAP_PLUS_VERSION
#define SNAPMAP_PLUS_VERSION "dev"
#endif
#define MN_WIDEN2(x)    L##x
#define MN_WIDEN(x)     MN_WIDEN2(x)
#define MN_AGENT        L"snapmap-plus/" MN_WIDEN(SNAPMAP_PLUS_VERSION)
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
    case MODNET_E_RETIRED:     return "this version of Snapmap+ is retired by the mod store";
    case MODNET_E_KEY:         return "the publishing key could not be used";
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
    /* The setting is read on every request, so a changed "mods.store" takes effect at the next request: a session
     * opened for another base URL is dropped (after the requests in flight finish) and reopened. */
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

static SRWLOCK g_min_lock = SRWLOCK_INIT;
static char g_min_seen[64];
static volatile LONG g_min_told;

/* Each dot-separated number of `a` against `b`'s ("1.2.10" > "1.2.9"); a part that is not a number compares as text.
 * "dev" is never older than anything: a local build is the newest. */
int modnet_version_cmp(const char *a, const char *b)
{
    if (!a || !b) return 0;
    if (!_stricmp(a, "dev")) return _stricmp(b, "dev") ? 1 : 0;
    if (!_stricmp(b, "dev")) return -1;
    while (*a == 'v' || *a == 'V') a++;
    while (*b == 'v' || *b == 'V') b++;
    while (*a || *b) {
        char *ea, *eb;
        unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
        if (ea != a && eb != b) { if (x != y) return x < y ? -1 : 1; a = ea; b = eb; }
        else { int c = (unsigned char)*a - (unsigned char)*b; if (c) return c < 0 ? -1 : 1; if (*a) a++; if (*b) b++; continue; }
        if (*a == '.' || *a == '-') a++;
        if (*b == '.' || *b == '-') b++;
    }
    return 0;
}

static void mn_minimum_seen(const wchar_t *value)
{
    char v[64];
    int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, v, (int)sizeof v, NULL, NULL);
    if (n <= 1) return;
    if (modnet_version_cmp(SNAPMAP_PLUS_VERSION, v) >= 0) return;
    AcquireSRWLockExclusive(&g_min_lock);
    strcpy_s(g_min_seen, sizeof g_min_seen, v);
    ReleaseSRWLockExclusive(&g_min_lock);
}

int modnet_newer_needed(void)
{
    int seen;
    AcquireSRWLockShared(&g_min_lock);
    seen = g_min_seen[0] != 0;
    ReleaseSRWLockShared(&g_min_lock);
    if (!seen) return 0;
    return InterlockedCompareExchange(&g_min_told, 1, 0) == 0;
}

const char *modnet_version(void) { return SNAPMAP_PLUS_VERSION; }

static int mn_winhttp_error(modnet_result *r, const char *step)
{
    DWORD e = GetLastError();
    if (e == ERROR_WINHTTP_TIMEOUT) return mn_fail(r, MODNET_E_TIMEOUT, "%s timed out", step);
    if (e == ERROR_WINHTTP_OPERATION_CANCELLED) return mn_fail(r, MODNET_E_CANCELLED, NULL);
    if (e == ERROR_WINHTTP_SECURE_FAILURE) return mn_fail(r, MODNET_E_NET, "%s: TLS failure", step);
    if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
        /* No name resolves: this computer is not connected, as against a store that does not answer. */
        r->offline = 1;
        return mn_fail(r, MODNET_E_NET, "%s: the store host could not be resolved", step);
    }
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
        headers = sh_wtextf(L"Accept: application/json, application/octet-stream\r\n%s", q->headers ? q->headers : L"");
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
        /* A busy store says when to ask again (seconds); the client waits it out. */
        wchar_t ra[32];
        DWORD n = sizeof ra;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, ra, &n,
                                WINHTTP_NO_HEADER_INDEX)) {
            unsigned long secs = wcstoul(ra, NULL, 10);
            r->retry_after_ms = secs > 86400ul ? 86400000ull : (unsigned long long)secs * 1000ull;
        }
    }
    {
        /* The oldest product the store still serves; remembered for the once-per-start notice. */
        wchar_t mv[64];
        DWORD n = sizeof mv;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"X-Snapmap-Plus-Minimum", mv, &n, WINHTTP_NO_HEADER_INDEX))
            mn_minimum_seen(mv);
    }
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
    else if (s == 410) code = MODNET_E_RETIRED;
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

/* The signed headers of a write, from the signer, for a body of SHA-256 `body_sha`.
 * NULL with the failure set in r when there is no signer or it fails. */
static wchar_t *mn_sign(const modnet_signer *signer, const char *method, const char *route, const char *body_sha,
                        modnet_result *r)
{
    wchar_t *headers = NULL;
    if (!signer || !signer->sign) { mn_fail(r, MODNET_E_ARG, "no publishing key signs this request"); return NULL; }
    if (!signer->sign(signer->context, method, route, body_sha, &headers) || !headers) {
        free(headers);
        mn_fail(r, MODNET_E_ARG, "the publishing key could not sign the request");
        return NULL;
    }
    return headers;
}

/* A small signed JSON POST: the answer in *resp (malloc'd). */
static int mn_signed_post(const char *route, const char *body, size_t len, const modnet_signer *signer, DWORD receive_ms,
                          unsigned char **resp, size_t *rlen, const modnet_io *io, modnet_result *r)
{
    char sha[MODNET_HASH_HEX + 1];
    wchar_t *headers;
    mn_req q;
    int code;
    sh_mb_sha256_hex(body, len, sha);
    if (!(headers = mn_sign(signer, "POST", route, sha, r))) return r->code ? r->code : MODNET_E_ARG;
    memset(&q, 0, sizeof q);
    q.verb = L"POST";
    q.route = route;
    q.content_type = "application/json";
    q.body = (const unsigned char *)body;
    q.body_len = len;
    q.headers = headers;
    q.receive_ms = receive_ms;
    q.max_response = MN_SMALL_BODY * 16;
    code = mn_exchange(&q, resp, rlen, io, r);
    free(headers);
    return code;
}

/* A JSON array of 64-hex strings from member `key` of the answer: *out (malloc'd, *n of them). */
static int mn_hash_list(const mpj *root, const char *key, char (**out)[MODNET_HASH_HEX + 1], size_t *n)
{
    const mpj *a = root && root->type == MPJ_OBJ ? mpj_get(root, key) : NULL;
    int i;
    *out = NULL;
    *n = 0;
    if (!a) return 1;
    if (a->type != MPJ_ARR) return 0;
    if (a->n && !(*out = calloc((size_t)a->n, sizeof **out))) return 0;
    for (i = 0; i < a->n; i++) {
        if (!a->items[i] || a->items[i]->type != MPJ_STR || !modnet_valid_hash(a->items[i]->text)) { free(*out); *out = NULL; *n = 0; return 0; }
        strcpy_s((*out)[*n], MODNET_HASH_HEX + 1, a->items[i]->text);
        (*n)++;
    }
    return 1;
}

int modnet_unit_status(const char *const *hashes, size_t n, int *states, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    double t0;
    size_t at = 0;
    int code = MODNET_OK;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if ((n && (!hashes || !states))) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    for (size_t i = 0; i < n; i++) {
        if (!modnet_valid_hash(hashes[i])) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "bad unit hash"));
        states[i] = MODNET_UNIT_MISSING;
    }
    while (at < n && code == MODNET_OK) {
        size_t batch = n - at > MODNET_UNITS_MAX ? MODNET_UNITS_MAX : n - at, i;
        mn_buf body = {0};
        unsigned char *resp = NULL;
        size_t rlen = 0;
        mn_req q;
        mn_puts(&body, "{\"units\":[");
        for (i = 0; i < batch; i++) {
            if (i) mn_puts(&body, ",");
            mn_puts(&body, "\"");
            mn_put(&body, hashes[at + i], MODNET_HASH_HEX);
            mn_puts(&body, "\"");
        }
        mn_puts(&body, "]}");
        if (body.bad) { free(body.p); code = mn_fail(r, MODNET_E_NOMEM, NULL); break; }
        memset(&q, 0, sizeof q);
        q.verb = L"POST";
        q.route = "/mods/units";
        q.content_type = "application/json";
        q.body = (const unsigned char *)body.p;
        q.body_len = body.n;
        q.max_response = MN_SMALL_BODY * 64;
        code = mn_exchange(&q, &resp, &rlen, io, r);
        free(body.p);
        if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "units");
        else if (code == MODNET_OK) {
            mpj *root = mpj_parse((const char *)resp, rlen);
            if (!root || root->type != MPJ_OBJ) code = mn_fail(r, MODNET_E_PROTOCOL, "units: the answer is not an object");
            for (i = 0; code == MODNET_OK && i < batch; i++) {
                const mpj *s = mpj_get(root, hashes[at + i]);
                if (!s || s->type != MPJ_STR) { code = mn_fail(r, MODNET_E_PROTOCOL, "units: the answer leaves out a unit"); break; }
                states[at + i] = mpj_str_is(s, "held") ? MODNET_UNIT_HELD : mpj_str_is(s, "removed") ? MODNET_UNIT_REMOVED
                                                                                                    : MODNET_UNIT_MISSING;
            }
            mpj_free(root);
        }
        free(resp);
        at += batch;
    }
    return mn_end(r, t0, code);
}

/* GET `route` into `out_path`, resuming from `have` bytes the file already holds (Range); a 200 answer to a range
 * means the store sent the whole object, which is written from the start. */
static int mn_get_resumed(const char *route, const char *out_path, unsigned long long have, const modnet_io *io,
                          modnet_result *r)
{
    unsigned char *resp = NULL;
    size_t rlen = 0;
    wchar_t *wpath, *range = NULL;
    HANDLE h;
    mn_req q;
    LARGE_INTEGER at;
    int code, tries;
    if (!(wpath = sh_mb_wpath(out_path))) return mn_fail(r, MODNET_E_NOMEM, NULL);
    for (tries = 0; tries < 2; tries++) {
        h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, have ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { free(wpath); return mn_fail(r, MODNET_E_ARG, "the download cannot be written (error %lu)", GetLastError()); }
        at.QuadPart = (LONGLONG)have;
        if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
            CloseHandle(h);
            free(wpath);
            return mn_fail(r, MODNET_E_ARG, "the download cannot be written (error %lu)", GetLastError());
        }
        free(range);
        range = have ? sh_wtextf(L"Range: bytes=%llu-\r\n", have) : NULL;
        memset(&q, 0, sizeof q);
        q.verb = L"GET";
        q.route = route;
        q.max_response = ~0ull;
        q.sink = h;
        q.headers = range;
        code = mn_exchange(&q, &resp, &rlen, io, r);
        if (code == MODNET_OK && !FlushFileBuffers(h)) code = mn_fail(r, MODNET_E_ARG, "the download cannot be written (error %lu)", GetLastError());
        CloseHandle(h);
        if (code == MODNET_OK && have && r->status == 200) {
            /* The whole object came: write it again from the start. */
            free(resp);
            resp = NULL;
            have = 0;
            continue;
        }
        /* A complete partial may receive 416. Its caller still verifies the file. */
        if (code == MODNET_OK && r->status != 200 && r->status != 206 && !(r->status == 416 && have))
            code = mn_status_fail(r, resp, rlen, "download");
        break;
    }
    free(range);
    free(resp);
    free(wpath);
    return code;
}

int modnet_get_blob(const char *sha, const char *out_path, unsigned long long have, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[96];
    double t0;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(sha) || !out_path || !out_path[0]) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/blob/%s", sha);
    return mn_end(r, t0, mn_get_resumed(route, out_path, have, io, r));
}

int modnet_get_unit_archive(const char *hash, const unsigned char *mask, size_t mask_len, const char *out_path,
                            unsigned long long have, const modnet_io *io, modnet_result *r)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    modnet_result local;
    char *route;
    size_t i, at;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(hash) || !out_path || !out_path[0] || (mask_len && !mask)) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    /* The route, and the mask as base64url without padding: 4 characters for each 3 bytes. */
    if (!(route = (char *)malloc(64 + 32 + (mask_len + 2) / 3 * 4 + 1))) return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL));
    at = (size_t)sprintf_s(route, 64 + 32, "/mods/unit/%s/archive%s", hash, mask_len ? "?want=" : "");
    for (i = 0; i < mask_len; i += 3) {
        unsigned v = (unsigned)mask[i] << 16 | (i + 1 < mask_len ? (unsigned)mask[i + 1] << 8 : 0) | (i + 2 < mask_len ? mask[i + 2] : 0);
        route[at++] = b64[(v >> 18) & 63];
        route[at++] = b64[(v >> 12) & 63];
        if (i + 1 < mask_len) route[at++] = b64[(v >> 6) & 63];
        if (i + 2 < mask_len) route[at++] = b64[v & 63];
    }
    route[at] = 0;
    code = mn_get_resumed(route, out_path, have, io, r);
    free(route);
    return mn_end(r, t0, code);
}

int modnet_begin_unit(const char *hash, const char *manifest, size_t len, const modnet_signer *signer, int *held,
                      char (**missing)[MODNET_HASH_HEX + 1], size_t *nmissing, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[112];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    wchar_t *headers;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (held) *held = 0;
    if (missing) *missing = NULL;
    if (nmissing) *nmissing = 0;
    if (!modnet_valid_hash(hash) || !manifest || !len || !held || !missing || !nmissing) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/unit/%s/begin", hash);
    /* The body is the manifest, so its SHA-256 is the unit's hash. */
    if (!(headers = mn_sign(signer, "POST", route, hash, r))) return mn_end(r, t0, r->code ? r->code : MODNET_E_ARG);
    memset(&q, 0, sizeof q);
    q.verb = L"POST";
    q.route = route;
    q.content_type = "application/json";
    q.body = (const unsigned char *)manifest;
    q.body_len = len;
    q.headers = headers;
    q.max_response = MN_SMALL_BODY * 64;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    free(headers);
    if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "begin");
    else if (code == MODNET_OK) {
        mpj *root = mpj_parse((const char *)resp, rlen);
        const mpj *h = root && root->type == MPJ_OBJ ? mpj_get(root, "held") : NULL;
        if (h && h->type == MPJ_BOOL && h->b) *held = 1;
        else if (!root || !mn_hash_list(root, "missing", missing, nmissing) || !*nmissing)
            code = mn_fail(r, MODNET_E_PROTOCOL, "begin: the answer says neither held nor missing");
        mpj_free(root);
    }
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_put_unit_archive(const char *hash, const char *path, unsigned long long size, const modnet_signer *signer,
                            int *held, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[112], sha[MODNET_HASH_HEX + 1];
    unsigned long long got = 0;
    unsigned char *resp = NULL;
    size_t rlen = 0;
    wchar_t *headers, *wpath;
    HANDLE h;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (held) *held = 0;
    if (!modnet_valid_hash(hash) || !path || !size || size > MODNET_ONE_REQUEST_MAX) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    if (!sh_mb_sha256_file(path, &got, sha) || got != size) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "the unit archive cannot be read"));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/unit/%s/archive", hash);
    if (!(headers = mn_sign(signer, "PUT", route, sha, r))) return mn_end(r, t0, r->code ? r->code : MODNET_E_ARG);
    wpath = sh_mb_wpath(path);
    h = wpath ? CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL)
              : INVALID_HANDLE_VALUE;
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) { free(headers); return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "the unit archive cannot be opened")); }
    memset(&q, 0, sizeof q);
    q.verb = L"PUT";
    q.route = route;
    q.content_type = MODNET_ARCHIVE_TYPE;
    q.body_file = h;
    q.body_len = (size_t)size;
    q.headers = headers;
    q.receive_ms = MN_COMPLETE_MS;   /* the store reads the archive back to check it before it answers */
    q.max_response = MN_SMALL_BODY * 64;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    CloseHandle(h);
    free(headers);
    if (code == MODNET_OK && r->status != 200 && r->status != 201) code = mn_status_fail(r, resp, rlen, "archive");
    else if (code == MODNET_OK) {
        mpj *root = mpj_parse((const char *)resp, rlen);
        const mpj *hv = root && root->type == MPJ_OBJ ? mpj_get(root, "held") : NULL;
        if (held) *held = hv && hv->type == MPJ_BOOL && hv->b;
        r->created = r->status == 201;
        mpj_free(root);
    }
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_put_blob(const char *sha, const char *path, unsigned long long size, const modnet_signer *signer,
                    const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[128], body[64];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    unsigned long long part_size = 0, parts = 0;
    unsigned char *have = NULL;
    wchar_t *wpath;
    HANDLE h;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(sha) || !path || !size) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(body, sizeof body, _TRUNCATE, "{\"size\":%llu}", size);
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/blob/%s/parts", sha);
    code = mn_signed_post(route, body, strlen(body), signer, 0, &resp, &rlen, io, r);
    if (code == MODNET_OK && r->status != 200 && r->status != 201) code = mn_status_fail(r, resp, rlen, "parts");
    else if (code == MODNET_OK) {
        mpj *root = mpj_parse((const char *)resp, rlen);
        const mpj *ps = root ? mpj_get(root, "partSize") : NULL, *pn = root ? mpj_get(root, "parts") : NULL,
                  *hv = root ? mpj_get(root, "have") : NULL;
        if (!ps || !mpj_is_num(ps) || !pn || !mpj_is_num(pn) || mpj_int(ps) <= 0 || mpj_int(pn) <= 0 ||
            (unsigned long long)mpj_int(pn) != (size + (unsigned long long)mpj_int(ps) - 1) / (unsigned long long)mpj_int(ps))
            code = mn_fail(r, MODNET_E_PROTOCOL, "parts: the answer is not an upload session");
        else {
            part_size = (unsigned long long)mpj_int(ps);
            parts = (unsigned long long)mpj_int(pn);
            have = calloc((size_t)parts + 1, 1);
            if (!have) code = mn_fail(r, MODNET_E_NOMEM, NULL);
            for (int i = 0; have && hv && hv->type == MPJ_ARR && i < hv->n; i++) {
                long long n = mpj_int(hv->items[i]);
                if (n >= 1 && (unsigned long long)n <= parts) have[n] = 1;
            }
        }
        mpj_free(root);
    }
    free(resp);
    resp = NULL;
    if (code != MODNET_OK) { free(have); return mn_end(r, t0, code); }
    wpath = sh_mb_wpath(path);
    h = wpath ? CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL)
              : INVALID_HANDLE_VALUE;
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) { free(have); return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "the file cannot be opened")); }
    for (unsigned long long n = 1; code == MODNET_OK && n <= parts; n++) {
        unsigned long long off = (n - 1) * part_size, len = n < parts ? part_size : size - off;
        wchar_t *extra;
        mn_req q;
        if (have[n]) continue;
        _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/blob/%s/parts/%llu", sha, n);
        extra = sh_wtextf(L"X-Blob-Size: %llu\r\n", size);
        if (!extra) { code = mn_fail(r, MODNET_E_NOMEM, NULL); break; }
        memset(&q, 0, sizeof q);
        q.verb = L"PUT";
        q.route = route;
        q.content_type = "application/octet-stream";
        q.body_file = h;
        q.body_offset = off;
        q.body_len = (size_t)len;
        q.headers = extra;
        q.max_response = MN_SMALL_BODY;
        code = mn_exchange(&q, &resp, &rlen, io, r);
        free(extra);
        if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "part");
        else if (code == MODNET_OK) r->parts++;
        free(resp);
        resp = NULL;
    }
    CloseHandle(h);
    free(have);
    if (code != MODNET_OK) return mn_end(r, t0, code);
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/blob/%s/complete", sha);
    code = mn_signed_post(route, body, strlen(body), signer, MN_COMPLETE_MS, &resp, &rlen, io, r);
    if (code == MODNET_OK && r->status != 200 && r->status != 201) code = mn_status_fail(r, resp, rlen, "complete");
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_complete_unit(const char *hash, const modnet_signer *signer, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[112];
    unsigned char *resp = NULL;
    size_t rlen = 0;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(hash)) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/unit/%s/complete", hash);
    code = mn_signed_post(route, "", 0, signer, 0, &resp, &rlen, io, r);
    if (code == MODNET_OK && r->status != 201 && r->status != 200) code = mn_status_fail(r, resp, rlen, "complete");
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_map_claim(const char *ref, const char *const *units, size_t n, const char *secret, long long rev,
                     const modnet_signer *signer, long long *rev_out, char (**removed)[MODNET_HASH_HEX + 1],
                     size_t *nremoved, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    mn_hashset set = {0};
    mn_buf body = {0};
    unsigned char *resp = NULL;
    size_t rlen = 0, i;
    double t0;
    int code;
    char num[32];
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (rev_out) *rev_out = -1;
    if (removed) *removed = NULL;
    if (nremoved) *nremoved = 0;
    if (!modnet_valid_ref(ref) || (n && !units) || !modnet_valid_hash(secret) || rev < 1 || !rev_out || !removed || !nremoved)
        return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    for (i = 0; i < n; i++) {
        if (!modnet_valid_hash(units[i])) { free(set.v); return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, "bad unit hash")); }
        if (!mn_set_has(&set, units[i]) && !mn_set_add(&set, units[i])) { free(set.v); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    }
    mn_puts(&body, "{\"ref\":");
    mn_put_json_string(&body, ref);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"rev\":%lld", rev);
    mn_puts(&body, num);
    mn_puts(&body, ",\"secret\":\"");
    mn_puts(&body, secret);
    mn_puts(&body, "\",\"units\":[");
    for (i = 0; i < (size_t)set.n; i++) {
        if (i) mn_puts(&body, ",");
        mn_puts(&body, "\"");
        mn_put(&body, set.v[i], MODNET_HASH_HEX);
        mn_puts(&body, "\"");
    }
    mn_puts(&body, "]}");
    free(set.v);
    if (body.bad) { free(body.p); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    code = mn_signed_post("/mods/map/claim", body.p, body.n, signer, 0, &resp, &rlen, io, r);
    SecureZeroMemory(body.p, body.n);
    free(body.p);
    if (code == MODNET_OK) {
        mpj *root = mpj_parse((const char *)resp, rlen);
        const mpj *rv = root && root->type == MPJ_OBJ ? mpj_get(root, "rev") : NULL;
        if (r->status == 200 || r->status == 201 || r->status == 451) {
            /* 451: the claim holds every unit but the removed ones. */
            if (!rv || !mpj_is_num(rv) || (r->status == 451 && !mn_hash_list(root, "removed", removed, nremoved)))
                code = r->status == 451 ? mn_status_fail(r, resp, rlen, "claim") : mn_fail(r, MODNET_E_PROTOCOL, "claim: the answer has no rev");
            else { *rev_out = mpj_int(rv); r->created = r->status == 201; }
        } else {
            if (r->status == 409 && rv && mpj_is_num(rv)) *rev_out = mpj_int(rv);
            code = mn_status_fail(r, resp, rlen, "claim");
        }
        mpj_free(root);
    }
    if (resp) { SecureZeroMemory(resp, rlen); free(resp); }
    return mn_end(r, t0, code);
}

int modnet_map_release(const char *ref, const char *secret, long long rev, const modnet_signer *signer, long long *rev_out,
                       const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    mn_buf body = {0};
    unsigned char *resp = NULL;
    size_t rlen = 0;
    double t0;
    int code;
    char num[32];
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (rev_out) *rev_out = -1;
    if (!modnet_valid_ref(ref) || !modnet_valid_hash(secret) || rev < 1) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    mn_puts(&body, "{\"ref\":");
    mn_put_json_string(&body, ref);
    _snprintf_s(num, sizeof num, _TRUNCATE, ",\"rev\":%lld", rev);
    mn_puts(&body, num);
    mn_puts(&body, ",\"secret\":\"");
    mn_puts(&body, secret);
    mn_puts(&body, "\"}");
    if (body.bad) { free(body.p); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    code = mn_signed_post("/mods/map/release", body.p, body.n, signer, 0, &resp, &rlen, io, r);
    SecureZeroMemory(body.p, body.n);
    free(body.p);
    if (code == MODNET_OK && r->status != 204) {
        mpj *root = mpj_parse((const char *)resp, rlen);
        const mpj *rv = root && root->type == MPJ_OBJ ? mpj_get(root, "rev") : NULL;
        if (r->status == 409 && rv && mpj_is_num(rv) && rev_out) *rev_out = mpj_int(rv);
        mpj_free(root);
        code = mn_status_fail(r, resp, rlen, "release");
    }
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_unit_asked(const char *hash, const char *path, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    char route[112];
    mn_buf body = {0};
    unsigned char *resp = NULL;
    size_t rlen = 0;
    mn_req q;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (!modnet_valid_hash(hash) || !path || !path[0]) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    _snprintf_s(route, sizeof route, _TRUNCATE, "/mods/unit/%s/asked", hash);
    mn_puts(&body, "{\"path\":");
    mn_put_json_string(&body, path);
    mn_puts(&body, "}");
    if (body.bad) { free(body.p); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    memset(&q, 0, sizeof q);
    q.verb = L"POST";
    q.route = route;
    q.content_type = "application/json";
    q.body = (const unsigned char *)body.p;
    q.body_len = body.n;
    q.max_response = MN_SMALL_BODY;
    code = mn_exchange(&q, &resp, &rlen, io, r);
    free(body.p);
    if (code == MODNET_OK && r->status != 204) code = mn_status_fail(r, resp, rlen, "asked");
    free(resp);
    return mn_end(r, t0, code);
}

int modnet_asked_units(const char *const *refs, const char *const *secrets, size_t n, const modnet_signer *signer,
                       char **json, const modnet_io *io, modnet_result *r)
{
    modnet_result local;
    mn_buf body = {0};
    unsigned char *resp = NULL;
    size_t rlen = 0, i;
    double t0;
    int code;
    if (!r) r = &local;
    mn_begin(r, &t0);
    if (json) *json = NULL;
    if (!json || (n && (!refs || !secrets))) return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL));
    mn_puts(&body, "{\"claims\":[");
    for (i = 0; i < n; i++) {
        if (!modnet_valid_ref(refs[i]) || !modnet_valid_hash(secrets[i])) { free(body.p); return mn_end(r, t0, mn_fail(r, MODNET_E_ARG, NULL)); }
        if (i) mn_puts(&body, ",");
        mn_puts(&body, "{\"ref\":");
        mn_put_json_string(&body, refs[i]);
        mn_puts(&body, ",\"secret\":\"");
        mn_puts(&body, secrets[i]);
        mn_puts(&body, "\"}");
    }
    mn_puts(&body, "]}");
    if (body.bad) { free(body.p); return mn_end(r, t0, mn_fail(r, MODNET_E_NOMEM, NULL)); }
    code = mn_signed_post("/mods/asked", body.p, body.n, signer, 0, &resp, &rlen, io, r);
    SecureZeroMemory(body.p, body.n);
    free(body.p);
    if (code == MODNET_OK && r->status != 200) code = mn_status_fail(r, resp, rlen, "asked");
    if (code == MODNET_OK) { *json = (char *)resp; resp = NULL; }
    free(resp);
    return mn_end(r, t0, code);
}

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
