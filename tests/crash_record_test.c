/* crash_record_test.c -- pure-logic tests for the crash-record JSON formatter (no game, no engine). */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>
#include "../src/fault_shield/crash_record_format.h"

static DWORD WINAPI format_thread(LPVOID arg)
{
    const char *tag = (const char *)arg;
    crash_record r = {0};
    char actual[8192], expected[8192];
    r.kind = tag; r.stack = tag; r.engine_text = tag; r.module = tag;
    assert(crash_record_json(expected, sizeof expected, &r) > 0);
    for (int i = 0; i < 20000; i++) {
        assert(crash_record_json(actual, sizeof actual, &r) > 0);
        assert(strcmp(actual, expected) == 0);
    }
    return 0;
}

int main(void)
{
    char buf[8192];

    /* escaping: backslash, quote, newline, tab, control char */
    {
        char e[128];
        int n = crash_json_escape(e, sizeof e, "a\\b\"c\nd\te\x01" "f");   /* spliced: \x is greedy */
        assert(n > 0);
        assert(strcmp(e, "a\\\\b\\\"c\\nd\\te\\u0001f") == 0);
    }
    /* escaping: truncation is clean (NUL-terminated, never a torn escape) */
    {
        char e[6];
        crash_json_escape(e, sizeof e, "aaaaaaaaaa");
        assert(strlen(e) < sizeof e);
    }
    /* full record: every field lands, hex fields formatted, JSON stays balanced */
    {
        crash_record r;
        int n;
        memset(&r, 0, sizeof r);
        r.kind = "classB"; r.code = 0xC0000005ul; r.rip_rva = 0x5e0b12ull; r.fault_addr = 0x3f00000060ull;
        r.module = "DOOMx64vk.exe";
        r.stack = "DOOM+0x5e0b12\n    DOOM+0x5e6410";
        r.engine_text = "^1ERROR: \"quoted\" thing";
        r.dump = ""; r.version = "0.2.0-beta.3"; r.time = "2026-07-18 12:00:00";
        r.renderer = "opengl";
        n = crash_record_json(buf, sizeof buf, &r);
        assert(n > 0 && (int)strlen(buf) == n);
        assert(buf[0] == '{' && buf[n - 1] == '}');
        assert(strstr(buf, "\"kind\":\"classB\""));
        assert(strstr(buf, "\"code\":\"0xc0000005\""));
        assert(strstr(buf, "\"rip_rva\":\"0x5e0b12\""));
        assert(strstr(buf, "\"fault_addr\":\"0x3f00000060\""));
        assert(strstr(buf, "\"module\":\"DOOMx64vk.exe\""));
        assert(strstr(buf, "DOOM+0x5e0b12\\n"));                     /* newline escaped inside stack */
        assert(strstr(buf, "\\\"quoted\\\""));                       /* quotes escaped inside text */
        assert(strstr(buf, "\"version\":\"0.2.0-beta.3\""));
        /* Retain the crashing session's renderer even if the game later relaunches. */
        assert(strstr(buf, "\"renderer\":\"opengl\""));
        assert(strstr(buf, "\"time\":\"2026-07-18 12:00:00\""));
    }
    /* NULL fields degrade to empty strings, not crashes */
    {
        crash_record r;
        memset(&r, 0, sizeof r);
        r.kind = "fatal";
        assert(crash_record_json(buf, sizeof buf, &r) > 0);
        assert(strstr(buf, "\"kind\":\"fatal\""));
        assert(strstr(buf, "\"stack\":\"\""));
        assert(strstr(buf, "\"renderer\":\"\""));
    }
    /* Every insufficient capacity fails empty, including an exact byte fit
     * without room for NUL. Guard bytes detect writes outside the capacity. */
    {
        crash_record r = {0};
        char bounded[8192];
        r.engine_text = "quote\" control\x01 UTF-8 \xc3\xa9";
        int full = crash_record_json(buf, sizeof buf, &r);
        for (int cap = 1; cap <= full; cap++) {
            memset(bounded, '#', sizeof bounded);
            assert(crash_record_json(bounded, (size_t)cap, &r) == 0);
            assert(bounded[0] == '\0' && bounded[cap] == '#');
        }
        assert(crash_record_json(bounded, (size_t)full + 1, &r) == full);
        assert(strcmp(bounded, buf) == 0);
        r.engine_text = "\xf0\x80\x80\x80\xc3";
        assert(crash_record_json(buf, sizeof buf, &r) > 0);
        assert(strstr(buf, "\\ufffd\\ufffd\\ufffd\\ufffd\\ufffd"));
    }
    {
        HANDLE a = CreateThread(NULL, 0, format_thread, "alpha\\\"\n", 0, NULL);
        HANDLE b = CreateThread(NULL, 0, format_thread, "beta\t\x01", 0, NULL);
        assert(a && b);
        assert(WaitForSingleObject(a, 30000) == WAIT_OBJECT_0);
        assert(WaitForSingleObject(b, 30000) == WAIT_OBJECT_0);
        CloseHandle(a); CloseHandle(b);
    }
    printf("crash_record_test OK\n");
    return 0;
}
