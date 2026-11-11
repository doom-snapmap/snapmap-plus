/* script_compile.c -- see script_compile.h. */
#include "script_compile.h"
#include "script_runtime.h"
#include "script_reason.h"
#include "../modbundle.h"
#include "../../third_party/lua/lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* The builder's limits are the runtime's. */
typedef char sh_script_compile_file_cap[SH_MB_SCRIPT_FILE_MAX == SH_SCRIPT_FILE_MAX ? 1 : -1];
typedef char sh_script_compile_mod_cap[SH_MB_SCRIPT_MOD_MAX == SH_SCRIPT_MOD_TEXT_MAX ? 1 : -1];
typedef char sh_script_compile_files_cap[SH_MB_SCRIPT_FILES_MAX == SH_SCRIPT_MOD_FILES_MAX ? 1 : -1];

static void put(char *out, size_t cap, const char *fmt, ...);

int sh_script_api_declared(const char *t, size_t n, long *version, int *line)
{
    size_t i = 0;
    int ln = 1;
    if (n >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) i = 3;
    for (;;) {
        while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n' || t[i] == ';')) { if (t[i] == '\n') ln++; i++; }
        if (i + 1 < n && t[i] == '-' && t[i + 1] == '-') {
            size_t eq = 0, j = i + 2;
            if (j < n && t[j] == '[') {
                size_t k = j + 1;
                while (k < n && t[k] == '=') { eq++; k++; }
                if (k < n && t[k] == '[') {
                    /* long comment: find ]=*] with the same level */
                    for (k = k + 1; k < n; k++) {
                        if (t[k] == '\n') ln++;
                        if (t[k] == ']') {
                            size_t e = 0, q = k + 1;
                            while (q < n && t[q] == '=') { e++; q++; }
                            if (e == eq && q < n && t[q] == ']') { i = q + 1; break; }
                        }
                    }
                    if (k >= n) return 0;
                    continue;
                }
            }
            while (i < n && t[i] != '\n') i++;
            continue;
        }
        break;
    }
    *line = ln;
    if (i + 3 > n || strncmp(t + i, "api", 3) != 0) return 0;
    i += 3;
    if (i < n && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '_')) return 0;
    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    if (i >= n || t[i] != '=' || (i + 1 < n && t[i + 1] == '=')) return 0;
    i++;
    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    if (i >= n || t[i] < '0' || t[i] > '9') return 0;
    {
        long v = 0;
        while (i < n && t[i] >= '0' && t[i] <= '9') { if (v < 1000000) v = v * 10 + (t[i] - '0'); i++; }
        if (i < n && (t[i] == '.' || t[i] == 'x' || t[i] == 'X' || t[i] == 'e' || t[i] == 'E' ||
                      (t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || t[i] == '_')) return 0;
        *version = v;
    }
    return 1;
}

typedef struct { size_t used; } compile_mem;

static void *compile_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    compile_mem *m = (compile_mem *)ud;
    size_t old = ptr ? osize : 0;
    void *p;
    if (nsize == 0) {
        if (ptr) { m->used -= old; free(ptr); }
        return NULL;
    }
    if (nsize > old && m->used + (nsize - old) > SH_SCRIPT_COMPILE_MEM) return NULL;
    p = realloc(ptr, nsize);
    if (!p) return nsize <= old ? ptr : NULL;
    m->used = m->used - old + nsize;
    return p;
}

static void put(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!out || !cap) return;
    va_start(ap, fmt);
    _vsnprintf_s(out, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
    out[cap - 1] = 0;
}

int sh_script_compile_check(const char *file, int entry, const char *text, size_t len, int *line,
                            char *what, size_t what_cap, char *todo, size_t todo_cap)
{
    compile_mem mem = { 0 };
    lua_State *L;
    char chunk[200];
    const char *t = text;
    size_t n = len;
    int status;
    *line = 0;
    put(what, what_cap, "%s", "");
    put(todo, todo_cap, "%s", "");
    if (len >= 4 && memcmp(text, "\x1bLua", 4) == 0) {
        put(what, what_cap, "the file is compiled Lua, not script text");
        put(todo, todo_cap, "Put the .lua source text in the file");
        return 0;
    }
    if (len > SH_SCRIPT_FILE_MAX) {
        put(what, what_cap, "the file is larger than 256 KB");
        put(todo, todo_cap, "Split it into smaller files");
        return 0;
    }
    if (entry) {
        long version = 0;
        int ln = 1;
        if (!sh_script_api_declared(text, len, &version, &ln)) {
            *line = ln;
            put(what, what_cap, "the file does not start with \"api = 1\"");
            put(todo, todo_cap, "Add \"api = 1\" as its first line");
            return 0;
        }
        if (version > SH_SCRIPT_API_VERSION) {
            *line = ln;
            put(what, what_cap, "the file asks for scripting version %ld, but this Snapmap+ has version %d", version,
                SH_SCRIPT_API_VERSION);
            put(todo, todo_cap, "Update Snapmap+ to run it");
            return 0;
        }
        if (version < 1) {
            *line = ln;
            put(what, what_cap, "the file asks for scripting version %ld, which does not exist", version);
            put(todo, todo_cap, "Write \"api = 1\"");
            return 0;
        }
    }
    if (n >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) { t += 3; n -= 3; }
    L = lua_newstate(compile_alloc, &mem);
    if (!L) {
        put(what, what_cap, "there was not enough memory to check the file");
        put(todo, todo_cap, "Close other programs and press Refresh again");
        return 0;
    }
    put(chunk, sizeof chunk, "=%s", file && file[0] ? file : "scripts\\script.lua");
    status = luaL_loadbufferx(L, t, n, chunk, "t");
    if (status != LUA_OK) {
        char where[160];
        int ln = 0;
        const char *msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
        const char *rest;
        if (status == LUA_ERRMEM || !msg) {
            put(what, what_cap, "the file is too large or too deeply nested to check");
            put(todo, todo_cap, "Split it into smaller files");
        } else {
            rest = sh_script_split_where(msg, where, sizeof where, &ln);
            sh_script_reason(rest, what, what_cap, todo, todo_cap);
            *line = ln;
        }
    }
    lua_close(L);
    return status == LUA_OK;
}
