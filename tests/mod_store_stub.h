/* mod_store_stub.h -- a minimal HTTP server on 127.0.0.1 for the store client test (modnet_test).
 *
 * A Winsock HTTP/1.1 server on a thread, one request per connection (Connection: close). It answers every route with a
 * 404 JSON error, which is enough to observe the client's request rules: the last GET path, the request count, and one
 * injected failure (a status for the next request whose "METHOD path" starts with a prefix).
 *
 * Requires ws2_32.lib. */
#ifndef SNAPMAP_PLUS_MOD_STORE_STUB_H
#define SNAPMAP_PLUS_MOD_STORE_STUB_H

#include "heap_out.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

typedef struct stub_state {
    SOCKET listener;
    HANDLE thread;
    int port;
    volatile LONG stop;
    CRITICAL_SECTION cs;
    int requests;
    char last_method[8], last_path[200];
    char last_get[200];           /* the path of the last GET or HEAD */
    char fail_prefix[48];         /* the next request whose "METHOD path" starts with this ... */
    int fail_status;              /* ... gets this status and a JSON error */
} stub_state;

static stub_state g_stub;

static void stub_send_ex(SOCKET s, int status, const char *ctype, const void *body, size_t len, int head_only)
{
    char head[512];
    const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 429 ? "Too Many Requests"
                       : status == 451 ? "Unavailable For Legal Reasons" : status >= 500 ? "Service Unavailable" : "Error";
    int n = _snprintf_s(head, sizeof head, _TRUNCATE,
                        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                        status, reason, ctype, (unsigned)len);
    send(s, head, n, 0);
    if (!head_only && len) send(s, (const char *)body, (int)len, 0);
}

static void stub_json(SOCKET s, int status, const char *json)
{
    stub_send_ex(s, status, "application/json", json, strlen(json), 0);
}

static void stub_route(SOCKET s, const char *method, const char *path)
{
    char out[256], probe[48];
    _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s %s", method, path);
    if (g_stub.fail_prefix[0] && strncmp(probe, g_stub.fail_prefix, strlen(g_stub.fail_prefix)) == 0) {
        int st = g_stub.fail_status;
        g_stub.fail_prefix[0] = 0;
        _snprintf_s(out, sizeof out, _TRUNCATE, "{\"error\":\"injected failure %d\"}", st);
        stub_send_ex(s, st, "application/json", out, strlen(out), !strcmp(method, "HEAD"));
        return;
    }
    stub_json(s, 404, "{\"error\":\"not found\"}");
}

static void stub_serve_one(SOCKET s)
{
    char *head = (char *)malloc(16384);
    size_t got = 0;
    char *end = NULL, method[8] = "", path[200] = "";
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
    EnterCriticalSection(&g_stub.cs);
    g_stub.requests++;
    if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) strncpy_s(g_stub.last_get, sizeof g_stub.last_get, path, _TRUNCATE);
    strcpy_s(g_stub.last_method, sizeof g_stub.last_method, method);
    strncpy_s(g_stub.last_path, sizeof g_stub.last_path, path, _TRUNCATE);
    stub_route(s, method, path);
    LeaveCriticalSection(&g_stub.cs);
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
    InterlockedExchange(&g_stub.stop, 1);
    closesocket(g_stub.listener);
    if (g_stub.thread) { WaitForSingleObject(g_stub.thread, 5000); CloseHandle(g_stub.thread); }
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

#endif /* SNAPMAP_PLUS_MOD_STORE_STUB_H */
