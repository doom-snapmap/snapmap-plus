/* event_registry.c -- the `sh_event` console command: the live engine event
 * registry, read through the decl manager's event interface.
 *
 *   sh_event info <name>       an event's number, format and argument types
 *   sh_event list [filter]     the live event registry, one event per line
 *
 * Reads only; build 13954591 layout. commands.c registers h_sh_event. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "commands.h"
#include "typeinfo.h"

#define EV_EVMGR_VSLOT          0x90u   /* decl manager -> idEventDefInterfaceLocal */
#define EV_COMMAND_VSLOT        0x08u   /* GetEventCommand(index) -> record */
#define EV_FIND_VSLOT           0x10u   /* FindByName(name) -> record */
#define EV_COUNT_VSLOT          0x28u
#define EV_REC_FORMAT_OFF       0x08u
#define EV_REC_TYPES_OFF        0x10u
#define EV_REC_NAMES_OFF        0x18u
#define EV_REC_NUMARGS_OFF      0x2Cu
#define EV_REC_EVENTNUM_OFF     0x34u

typedef void *(*ev_vfn0)(void *self);
typedef void *(*ev_vfn_name)(void *self, const char *name);
typedef void *(*ev_vfn_index)(void *self, int index);
typedef int (*ev_vfn_count)(void *self);

/* An access violation in a registry call stops that call with no result; any
 * other exception (an idException is a C++ throw) is named in one line and
 * goes on to the engine's own handler, never swallowed. */
static int ev_filter(DWORD code)
{
    if (code == EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_EXECUTE_HANDLER;
    sh_printf("sh_event: the game raised an error reading the event registry (exception 0x%08lx); the game handles it\n",
              (unsigned long)code);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void *ev_vfn(void *self, size_t slot)
{
    __try {
        if (!self) return NULL;
        const uint8_t *vt = *(const uint8_t * const *)self;
        return vt ? *(void * const *)(vt + slot) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static void *ev_manager(void)
{
    void *declmgr = sh_typeinfo_get_declmgr();
    ev_vfn0 acc = (ev_vfn0)ev_vfn(declmgr, EV_EVMGR_VSLOT);
    if (!acc) return NULL;
    __try { return acc(declmgr); } __except (ev_filter(GetExceptionCode())) { return NULL; }
}

static void *ev_find(const char *name)
{
    void *evmgr = ev_manager();
    ev_vfn_name find = (ev_vfn_name)ev_vfn(evmgr, EV_FIND_VSLOT);
    if (!find) return NULL;
    __try { return find(evmgr, name); } __except (ev_filter(GetExceptionCode())) { return NULL; }
}

/* Copy a NUL-terminated engine string; returns 0 when unreadable. */
static int ev_read_cstr(const char *src, char *dst, size_t cap)
{
    if (!dst || cap == 0) return 0;
    dst[0] = 0;
    if (!src) return 0;
    __try {
        size_t i = 0;
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
        dst[i] = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { dst[0] = 0; return 0; }
}

static int ev_read_ptr(const void *at, void *out, size_t n)
{
    return sh_safe_read((const uint8_t *)at, (uint8_t *)out, n);
}

static void ev_print(const char *name)
{
    uint8_t *rec = (uint8_t *)ev_find(name);
    if (!rec) { sh_printf("sh_event: no event named '%s'\n", name); return; }
    const char *fmt = NULL, *types = NULL, *names = NULL;
    int numargs = 0, eventnum = 0;
    char f[16] = "", t[256] = "", n[256] = "";
    ev_read_ptr(rec + EV_REC_FORMAT_OFF, &fmt, sizeof fmt);
    ev_read_ptr(rec + EV_REC_NUMARGS_OFF, &numargs, sizeof numargs);
    ev_read_ptr(rec + EV_REC_EVENTNUM_OFF, &eventnum, sizeof eventnum);
    ev_read_ptr(rec + EV_REC_TYPES_OFF, &types, sizeof types);
    ev_read_ptr(rec + EV_REC_NAMES_OFF, &names, sizeof names);
    ev_read_cstr(fmt, f, sizeof f);
    ev_read_cstr(types, t, sizeof t);
    ev_read_cstr(names, n, sizeof n);
    sh_printf("sh_event: '%s' number %d args %d format '%s' types '%s' names '%s'\n",
              name, eventnum, numargs, f, t, n);
}

static int ev_count(void *evmgr)
{
    ev_vfn_count cnt = (ev_vfn_count)ev_vfn(evmgr, EV_COUNT_VSLOT);
    if (!cnt) return 0;
    __try { return cnt(evmgr); } __except (ev_filter(GetExceptionCode())) { return 0; }
}

static void *ev_at(void *evmgr, int i)
{
    ev_vfn_index at = (ev_vfn_index)ev_vfn(evmgr, EV_COMMAND_VSLOT);
    if (!at) return NULL;
    __try { return at(evmgr, i); } __except (ev_filter(GetExceptionCode())) { return NULL; }
}

/* The live registry as UTF-8 lines "name\tformat\ttypes\tnames\n". Returns
 * the bytes needed excluding the NUL, writing at most cap bytes (always NUL
 * terminated when cap > 0); 0 when the registry is not reachable. */
static int ev_registry_text(char *out, int cap)
{
    void *evmgr = ev_manager();
    int count = ev_count(evmgr);
    if (!evmgr || count <= 0 || count > 0x1000) { if (out && cap > 0) out[0] = 0; return 0; }
    int need = 0;
    for (int i = 0; i < count; i++) {
        uint8_t *rec = (uint8_t *)ev_at(evmgr, i);
        const char *pn = NULL, *pf = NULL, *pt = NULL, *pa = NULL;
        char nm[128], f[16], t[256], an[256];
        if (!rec || !ev_read_ptr(rec, &pn, sizeof pn) || !ev_read_cstr(pn, nm, sizeof nm) || !nm[0]) continue;
        ev_read_ptr(rec + EV_REC_FORMAT_OFF, &pf, sizeof pf);
        ev_read_ptr(rec + EV_REC_TYPES_OFF, &pt, sizeof pt);
        ev_read_ptr(rec + EV_REC_NAMES_OFF, &pa, sizeof pa);
        ev_read_cstr(pf, f, sizeof f);
        ev_read_cstr(pt, t, sizeof t);
        ev_read_cstr(pa, an, sizeof an);
        char line[700];
        int len = snprintf(line, sizeof line, "%s\t%s\t%s\t%s\n", nm, f, t, an);
        if (len < 0 || len >= (int)sizeof line) continue;
        if (out && need + len < cap) memcpy(out + need, line, (size_t)len);
        need += len;
    }
    if (out && cap > 0) out[need < cap ? need : cap - 1] = 0;
    return need;
}

void h_sh_event(idCmdArgs *a)
{
    const char *v = cmd_argv(a, 1);
    if (v && !strcmp(v, "info") && cmd_argv(a, 2)) { ev_print(cmd_argv(a, 2)); return; }
    if (v && !strcmp(v, "list")) {
        const char *filter = cmd_argv(a, 2);
        int need = ev_registry_text(NULL, 0);
        char *buf = need > 0 ? (char *)malloc((size_t)need + 1) : NULL;
        if (!buf) { sh_printf("sh_event: the event registry is not reachable\n"); return; }
        ev_registry_text(buf, need + 1);
        int shown = 0, total = 0;
        for (char *line = buf; *line;) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            total++;
            if (!filter || strstr(line, filter)) { sh_printf("  %s\n", line); shown++; }
            if (!nl) break;
            line = nl + 1;
        }
        sh_printf("sh_event: %d of %d events\n", shown, total);
        free(buf);
        return;
    }
    sh_printf("usage: sh_event info <name> | list [filter]\n");
}
