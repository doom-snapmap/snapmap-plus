/* modaction.c -- "smp:" commands carried by String Variable Set nodes.
 * See modaction.h for the contract; the engine half is below the pure half. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modaction.h"

static void smp_err(char *err, size_t cap, const char *msg, const char *detail)
{
    if (!err || cap == 0) return;
    if (detail) snprintf(err, cap, "%s '%s'", msg, detail);
    else snprintf(err, cap, "%s", msg);
}

int smp_is_command(const char *s)
{
    return s != NULL && strncmp(s, SMP_PREFIX, sizeof SMP_PREFIX - 1) == 0;
}

int smp_parse(const char *literal, smp_command *out, char *err, size_t err_cap)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!smp_is_command(literal)) { smp_err(err, err_cap, "not an smp: command", NULL); return 0; }
    if (strlen(literal) > SMP_MAX_LITERAL) { smp_err(err, err_cap, "command too long", NULL); return 0; }
    const char *p = literal + sizeof SMP_PREFIX - 1;
    int n = 0;   /* tokens including the verb */
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (n > SMP_MAX_ARGS + 1) { smp_err(err, err_cap, "too many arguments", NULL); return 0; }
        char tok[SMP_MAX_TOKEN];
        size_t len = 0;
        int quoted = (*p == '"');
        if (quoted) p++;
        while (*p && (quoted ? *p != '"' : (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'))) {
            if (len + 1 >= sizeof tok) { smp_err(err, err_cap, "token too long", NULL); return 0; }
            tok[len++] = *p++;
        }
        if (quoted) {
            if (*p != '"') { smp_err(err, err_cap, "unterminated quote", NULL); return 0; }
            p++;
        }
        tok[len] = 0;
        if (n == 0) {
            if (len == 0 || len >= sizeof out->verb) { smp_err(err, err_cap, "bad verb", tok); return 0; }
            memcpy(out->verb, tok, len + 1);
        } else {
            if (n - 1 >= SMP_MAX_ARGS + 1) { smp_err(err, err_cap, "too many arguments", NULL); return 0; }
            memcpy(out->argv[n - 1], tok, len + 1);
            out->argc = n;
        }
        n++;
    }
    if (n == 0) { smp_err(err, err_cap, "empty command", NULL); return 0; }
    return 1;
}

static int smp_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == 0 && *b == 0;
}

int smp_parse_bool(const char *s, int *v)
{
    if (!s || !v) return 0;
    if (!strcmp(s, "1") || smp_ieq(s, "true") || smp_ieq(s, "on") || smp_ieq(s, "yes")) { *v = 1; return 1; }
    if (!strcmp(s, "0") || smp_ieq(s, "false") || smp_ieq(s, "off") || smp_ieq(s, "no")) { *v = 0; return 1; }
    return 0;
}

static int smp_parse_int(const char *s, int *v)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    errno = 0;
    long x = strtol(s, &end, 0);
    if (errno || *end || x < INT_MIN || x > INT_MAX) return 0;
    *v = (int)x;
    return 1;
}

static int smp_parse_float(const char *s, float *v)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    errno = 0;
    double x = strtod(s, &end);
    if (errno || *end || x != x) return 0;
    *v = (float)x;
    return 1;
}

int smp_type_args(const char *format, const char values[][SMP_MAX_TOKEN], int count,
                  smp_arg *out, char *err, size_t err_cap)
{
    if (!format) format = "";
    int want = (int)strlen(format);
    if (want > SMP_MAX_ARGS) { smp_err(err, err_cap, "event takes more arguments than supported", NULL); return -1; }
    if (count != want) {
        char m[64];
        snprintf(m, sizeof m, "event takes %d argument(s), got %d; format", want, count);
        smp_err(err, err_cap, m, format);
        return -1;
    }
    for (int i = 0; i < want; i++) {
        smp_arg *a = &out[i];
        memset(a, 0, sizeof *a);
        const char *v = values[i];
        switch (format[i]) {
        case 'i':
            a->type = 'i';
            if (!smp_parse_int(v, &a->i)) { smp_err(err, err_cap, "not an int:", v); return -1; }
            break;
        case 'b':
            a->type = 'i';   /* the engine's argument copy drops a 'b' value */
            if (!smp_parse_bool(v, &a->i)) { smp_err(err, err_cap, "not a bool:", v); return -1; }
            break;
        case 'f':
            a->type = 'f';
            if (!smp_parse_float(v, &a->f)) { smp_err(err, err_cap, "not a float:", v); return -1; }
            break;
        case 's':
            a->type = 's';
            a->text = v;
            break;
        case 'e':
            a->type = 'e';
            if (!*v) { smp_err(err, err_cap, "empty entity name", NULL); return -1; }
            a->text = v;
            break;
        default: {
            char t[2] = { format[i], 0 };
            smp_err(err, err_cap, "unsupported argument type", t);
            return -1;
        }
        }
    }
    return want;
}

#ifndef SH_MODACTION_PURE
/* ------------------------------------------------------------------------- */
/* Engine half: build 13954591 only (SH_RVA yields 0 on any other image).     */
#include <windows.h>
#include "backend_log.h"
#include "engine_build.h"
#include "entity.h"
#include "hook.h"
#include "typeinfo.h"

/* String Variable Set DoAction(this, idEntity *activator), the StringSetDoAction
 * signature. Its only reference is its vtable slot (VK 0x24BDA20 / GL
 * 0x24D7770). The 20-byte prologue is whole and position-independent on both
 * renderers: mov rax,rsp; push rdi; push r14; push r15; sub rsp,0x60;
 * mov [rax-0x58],-2. */
#define MA_STRSET_STOLEN        20u
static const uint8_t k_strset_prologue[MA_STRSET_STOLEN] = {
    0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
    0xEC, 0x60, 0x48, 0xC7, 0x40, 0xA8, 0xFE, 0xFF, 0xFF, 0xFF };

/* The action's value parameter at this+0xCA8: int kind, int index, then an
 * idStr literal (0x30 bytes, length +0x08, data pointer +0x10). The value is
 * a literal when the index equals the engine's "no variable" constant
 * (VK 0x236E9D0 / GL 0x2388720, -1 on both). */
#define MA_VALUE_OFF            0xCA8u
#define MA_VALUE_INDEX_OFF      (MA_VALUE_OFF + 4u)
#define MA_VALUE_LITERAL_OFF    (MA_VALUE_OFF + 8u)
#define MA_IDSTR_LEN_OFF        0x08u
#define MA_IDSTR_DATA_OFF       0x10u
#define MA_NO_VARIABLE_RVA      SH_RVA(0x236E9D0u, 0x2388720u)

/* Event system (lane P22): ProcessEventArgs(obj, ret[0x18], eventDef, n,
 * idEventArg *a0, ...) copies up to eight 0x18-byte arguments and calls
 * idEvent::Execute; the return slot is cleared with the argument clear. */
#define MA_PROCESS_EVENT_ARGS_RVA SH_RVA(0xB47350u, 0xB46940u)
#define MA_EVENTARG_CLEAR_RVA     SH_RVA(0xB44640u, 0xB43A50u)
#define MA_EVENTARG_ENTITY_RVA    SH_RVA(0xB44300u, 0xB438F0u)   /* (arg, idEntity*) -> 'e' handle */
#define MA_EVMGR_VSLOT            0x90u   /* decl manager -> idEventDefInterfaceLocal */
#define MA_EV_COMMAND_VSLOT       0x08u   /* GetEventCommand(index) -> record */
#define MA_EV_FIND_VSLOT          0x10u   /* FindByName(name) -> record */
#define MA_EV_COUNT_VSLOT         0x28u
#define MA_REC_FORMAT_OFF         0x08u
#define MA_REC_TYPES_OFF          0x10u
#define MA_REC_NAMES_OFF          0x18u
#define MA_REC_NUMARGS_OFF        0x2Cu
#define MA_REC_EVENTNUM_OFF       0x34u

/* Activator to player: an idPlayer cast, else the class whose vtable +0x558
 * returns its owning player (the Give Item action's own fallback). */
#define MA_CAST_PLAYER_RVA      SH_RVA(0x14AF840u, 0x14AF8D0u)
#define MA_CAST_OWNED_RVA       SH_RVA(0x142A230u, 0x142A190u)
#define MA_OWNER_PLAYER_VSLOT   0x558u
#define MA_ENTITY_NAME_OFF      0x388u   /* char* name, as idEvent::Execute prints it */

/* gameMgr fields: isServer byte +0x2BB3A4 (read by IsServer on both images)
 * and the SnapMap logic manager at +0x298128 -> +0x48, which every stock
 * action requires before doing anything. */
#define MA_GM_ISSERVER_OFF      0x2BB3A4u
#define MA_GM_LOGIC_OFF         0x298128u
#define MA_LOGIC_MANAGER_OFF    0x48u

/* idPlayer: the Doom-hands switch (written only by idPlayer::Spawn, VK
 * 0xDE64E0 / GL 0xDE5970: local player, SnapMap game type, the map's
 * useSPModels and fewer than two players) and the hands/body stale flags. */
#define MA_PLAYER_DOOMHANDS_OFF 0x45EB5u
#define MA_PLAYER_HANDS_STALE   0xB512u
#define MA_PLAYER_BODY_STALE    0xB513u

typedef struct { char type; char pad[7]; uint64_t value; uint64_t extra; } ma_event_arg;
typedef void (*ma_doaction_fn)(void *self, void *activator);
typedef void *(*ma_vfn0)(void *self);
typedef void *(*ma_vfn_name)(void *self, const char *name);
typedef void *(*ma_vfn_index)(void *self, int index);
typedef int (*ma_vfn_count)(void *self);
typedef void *(*ma_process_args_fn)(void *obj, void *ret, void *evdef, int n,
                                    ma_event_arg *a0, ma_event_arg *a1, ma_event_arg *a2, ma_event_arg *a3,
                                    ma_event_arg *a4, ma_event_arg *a5, ma_event_arg *a6, ma_event_arg *a7);
typedef void (*ma_clear_fn)(void *arg);
typedef void *(*ma_arg_entity_fn)(void *arg, void *entity);
typedef void *(*ma_cast_fn)(void *obj);

static ma_doaction_fn            g_orig_doaction = NULL;
static volatile LONG             g_installed = 0;
static volatile LONG             g_enabled = 1;
static sh_modaction_map_gate_fn  g_map_gate = NULL;
static uint8_t                  *g_base = NULL;

void sh_modaction_set_map_gate(sh_modaction_map_gate_fn fn) { g_map_gate = fn; }

static void *ma_fn(uint32_t rva) { return (g_base && rva) ? (void *)(g_base + rva) : NULL; }

static void *ma_vfn(void *self, size_t slot)
{
    __try {
        if (!self) return NULL;
        const uint8_t *vt = *(const uint8_t * const *)self;
        return vt ? *(void * const *)(vt + slot) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static void *ma_evmgr(void)
{
    void *declmgr = sh_typeinfo_get_declmgr();
    ma_vfn0 acc = (ma_vfn0)ma_vfn(declmgr, MA_EVMGR_VSLOT);
    if (!acc) return NULL;
    __try { return acc(declmgr); } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

static void *ma_find_event(const char *name)
{
    void *evmgr = ma_evmgr();
    ma_vfn_name find = (ma_vfn_name)ma_vfn(evmgr, MA_EV_FIND_VSLOT);
    if (!find) return NULL;
    __try { return find(evmgr, name); } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* Copy a NUL-terminated engine string; returns 0 when unreadable. */
static int ma_read_cstr(const char *src, char *dst, size_t cap)
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

static int ma_read_ptr(const void *at, void *out, size_t n)
{
    return sh_safe_read((const uint8_t *)at, (uint8_t *)out, n);
}

typedef struct { void *rec; char format[16]; int numargs; int eventnum; } ma_event;

static int ma_event_info(const char *name, ma_event *ev)
{
    memset(ev, 0, sizeof *ev);
    ev->rec = ma_find_event(name);
    if (!ev->rec) return 0;
    const char *fmt = NULL;
    ma_read_ptr((uint8_t *)ev->rec + MA_REC_FORMAT_OFF, &fmt, sizeof fmt);
    ma_read_ptr((uint8_t *)ev->rec + MA_REC_NUMARGS_OFF, &ev->numargs, sizeof ev->numargs);
    ma_read_ptr((uint8_t *)ev->rec + MA_REC_EVENTNUM_OFF, &ev->eventnum, sizeof ev->eventnum);
    ma_read_cstr(fmt, ev->format, sizeof ev->format);
    return 1;
}

static const char *ma_entity_name(void *ent, char *buf, size_t cap)
{
    const char *p = NULL;
    if (!ent || !ma_read_ptr((uint8_t *)ent + MA_ENTITY_NAME_OFF, &p, sizeof p) || !ma_read_cstr(p, buf, cap) || !buf[0])
        snprintf(buf, cap, "%p", ent);
    return buf;
}

/* The player the activator stands for, or NULL. */
static void *ma_player_of(void *activator)
{
    ma_cast_fn cast_player = (ma_cast_fn)ma_fn(MA_CAST_PLAYER_RVA);
    ma_cast_fn cast_owned = (ma_cast_fn)ma_fn(MA_CAST_OWNED_RVA);
    if (!activator || !cast_player || !cast_owned) return NULL;
    __try {
        void *p = cast_player(activator);
        if (p) return p;
        void *o = cast_owned(activator);
        if (!o) return NULL;
        ma_vfn0 owner = (ma_vfn0)ma_vfn(o, MA_OWNER_PLAYER_VSLOT);
        return owner ? owner(o) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* Host-side SnapMap session with a logic manager. */
static int ma_session_ok(const char **why)
{
    uint8_t *gm = (uint8_t *)sh_entity_gamemgr();
    uint8_t is_server = 0;
    void *logic = NULL, *mgr = NULL;
    if (!gm) { *why = "no game"; return 0; }
    if (!ma_read_ptr(gm + MA_GM_ISSERVER_OFF, &is_server, 1) || !is_server) { *why = "not the host"; return 0; }
    if (!ma_read_ptr(gm + MA_GM_LOGIC_OFF, &logic, sizeof logic) || !logic ||
        !ma_read_ptr((uint8_t *)logic + MA_LOGIC_MANAGER_OFF, &mgr, sizeof mgr) || !mgr) {
        *why = "no SnapMap logic manager"; return 0;
    }
    return 1;
}

static void ma_mark_models_stale(uint8_t *player)
{
    __try {
        player[MA_PLAYER_HANDS_STALE] = 1;
        player[MA_PLAYER_BODY_STALE] = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Fire `cmd` (verb "event") on player. Returns 1 when the engine ran it. */
static int ma_run_event(const smp_command *cmd, void *player, const char *who, char *err, size_t cap)
{
    if (cmd->argc < 1) { smp_err(err, cap, "usage: smp:event <eventName> [args...]", NULL); return 0; }
    ma_event ev;
    if (!ma_event_info(cmd->argv[0], &ev)) { smp_err(err, cap, "no event named", cmd->argv[0]); return 0; }
    if ((int)strlen(ev.format) != ev.numargs) { smp_err(err, cap, "unreadable event format for", cmd->argv[0]); return 0; }
    smp_arg typed[SMP_MAX_ARGS];
    int n = smp_type_args(ev.format, (const char (*)[SMP_MAX_TOKEN])&cmd->argv[1], cmd->argc - 1, typed, err, cap);
    if (n < 0) return 0;
    ma_process_args_fn process = (ma_process_args_fn)ma_fn(MA_PROCESS_EVENT_ARGS_RVA);
    ma_clear_fn clear = (ma_clear_fn)ma_fn(MA_EVENTARG_CLEAR_RVA);
    ma_arg_entity_fn from_entity = (ma_arg_entity_fn)ma_fn(MA_EVENTARG_ENTITY_RVA);
    if (!process || !clear || !from_entity) { smp_err(err, cap, "unsupported executable", NULL); return 0; }
    ma_event_arg a[SMP_MAX_ARGS];
    memset(a, 0, sizeof a);
    for (int i = 0; i < n; i++) {
        switch (typed[i].type) {
        case 'i': a[i].type = 'i'; a[i].value = (uint64_t)(uint32_t)typed[i].i; break;
        case 'f': { uint32_t bits; memcpy(&bits, &typed[i].f, 4); a[i].type = 'f'; a[i].value = bits; break; }
        case 's': a[i].type = 's'; a[i].value = (uint64_t)(uintptr_t)typed[i].text; break;
        case 'e': {
            void *ent = sh_entity_find(typed[i].text);
            if (!ent) { smp_err(err, cap, "no entity named", typed[i].text); return 0; }
            __try { from_entity(&a[i], ent); } __except (EXCEPTION_EXECUTE_HANDLER) { smp_err(err, cap, "entity argument faulted", NULL); return 0; }
            break;
        }
        }
    }
    ma_event_arg *p[SMP_MAX_ARGS];
    for (int i = 0; i < SMP_MAX_ARGS; i++) p[i] = &a[i];
    uint8_t ret[0x18] = {0};
    int ok = 0;
    __try {
        process(player, ret, ev.rec, n, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        clear(ret);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) { smp_err(err, cap, "event faulted", cmd->argv[0]); return 0; }
    char line[SMP_MAX_TOKEN * 3];
    size_t off = (size_t)snprintf(line, sizeof line, "%s(", cmd->argv[0]);
    for (int i = 1; i < cmd->argc && off < sizeof line; i++)
        off += (size_t)snprintf(line + off, sizeof line - off, "%s%s", i > 1 ? ", " : "", cmd->argv[i]);
    if (off < sizeof line) snprintf(line + off, sizeof line - off, ")");
    sh_printf("SMP: event %s (number %d, format '%s') on %s: ok\n", line, ev.eventnum, ev.format, who);
    return 1;
}

static int ma_run_doomhands(const smp_command *cmd, void *player, const char *who, char *err, size_t cap)
{
    int on = 0;
    if (cmd->argc != 1 || !smp_parse_bool(cmd->argv[0], &on)) { smp_err(err, cap, "usage: smp:doomhands <0|1>", NULL); return 0; }
    uint8_t before = 0;
    uint8_t *pl = (uint8_t *)player;
    if (!ma_read_ptr(pl + MA_PLAYER_DOOMHANDS_OFF, &before, 1)) { smp_err(err, cap, "player unreadable", NULL); return 0; }
    __try { pl[MA_PLAYER_DOOMHANDS_OFF] = (uint8_t)on; } __except (EXCEPTION_EXECUTE_HANDLER) { smp_err(err, cap, "player unwritable", NULL); return 0; }
    ma_mark_models_stale(pl);
    sh_printf("SMP: doomhands %d on %s (was %u): hands and body rebuild on the next think\n", on, who, before);
    return 1;
}

/* Run one literal on the player the activator stands for. Logs one line. */
static void ma_run_literal(const char *literal, void *activator)
{
    smp_command cmd;
    char err[192] = "";
    char who[96];
    if (!smp_parse(literal, &cmd, err, sizeof err)) { sh_printf("SMP: \"%s\": %s\n", literal, err); return; }
    void *player = ma_player_of(activator);
    if (!player) {
        char an[64];
        sh_printf("SMP: \"%s\": the activator %s is not a player; nothing done\n", literal, ma_entity_name(activator, an, sizeof an));
        return;
    }
    ma_entity_name(player, who, sizeof who);
    int ok;
    if (!strcmp(cmd.verb, "event")) ok = ma_run_event(&cmd, player, who, err, sizeof err);
    else if (!strcmp(cmd.verb, "doomhands")) ok = ma_run_doomhands(&cmd, player, who, err, sizeof err);
    else { smp_err(err, sizeof err, "unknown command", cmd.verb); ok = 0; }
    if (!ok) sh_printf("SMP: \"%s\": %s; nothing done\n", literal, err);
}

/* The literal of a String Set action, or 0 when its value is a variable. */
static int ma_action_literal(const uint8_t *self, char *out, size_t cap)
{
    int index = 0, none = 0, len = 0;
    const char *data = NULL;
    const uint8_t *sentinel = (const uint8_t *)ma_fn(MA_NO_VARIABLE_RVA);
    if (!sentinel || !ma_read_ptr(sentinel, &none, sizeof none)) return 0;
    if (!ma_read_ptr(self + MA_VALUE_INDEX_OFF, &index, sizeof index) || index != none) return 0;
    if (!ma_read_ptr(self + MA_VALUE_LITERAL_OFF + MA_IDSTR_LEN_OFF, &len, sizeof len) ||
        !ma_read_ptr(self + MA_VALUE_LITERAL_OFF + MA_IDSTR_DATA_OFF, &data, sizeof data)) return 0;
    if (len < 0 || (size_t)len >= cap || !data) return 0;
    return ma_read_cstr(data, out, cap) && (int)strlen(out) == len;
}

static void ma_strset_doaction(void *self, void *activator)
{
    __try {
        char literal[SMP_MAX_LITERAL + 1];
        if (InterlockedCompareExchange(&g_enabled, 0, 0) &&
            ma_action_literal((const uint8_t *)self, literal, sizeof literal) && smp_is_command(literal)) {
            const char *why = NULL;
            if (!ma_session_ok(&why)) sh_printf("SMP: \"%s\": %s; nothing done\n", literal, why);
            else if (g_map_gate && !g_map_gate()) sh_printf("SMP: \"%s\": the open map does not use Snapmap+ behaviour; nothing done\n", literal);
            else ma_run_literal(literal, activator);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sh_printf("SMP: a String Set command faulted; the stock write continues\n");
    }
    g_orig_doaction(self, activator);
}

int sh_modaction_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_installed, 0, 0)) return 1;
    g_base = (uint8_t *)module_base;
    uint8_t *target = results ? (uint8_t *)sig_addr_by_name(results, count, "StringSetDoAction") : NULL;
    if (!target || !ma_fn(MA_PROCESS_EVENT_ARGS_RVA)) {
        backend_log("SMP: String Set commands are not available on this executable");
        return 0;
    }
    uint8_t cur[MA_STRSET_STOLEN];
    if (!sh_safe_read(target, cur, sizeof cur) || memcmp(cur, k_strset_prologue, sizeof cur) != 0) {
        backend_log("SMP: String Set DoAction prologue differs; smp: commands are off");
        return 0;
    }
    void *tramp = hook_prepare(target, (void *)ma_strset_doaction, MA_STRSET_STOLEN);
    if (!tramp) { backend_log("SMP: String Set hook could not be prepared; smp: commands are off"); return 0; }
    g_orig_doaction = (ma_doaction_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_orig_doaction = NULL;
        backend_log("SMP: String Set hook could not be committed; smp: commands are off");
        return 0;
    }
    InterlockedExchange(&g_installed, 1);
    backend_log("SMP: String Set commands installed (smp:event, smp:doomhands)");
    return 1;
}

/* Console. */
void h_sh_smp(idCmdArgs *a)
{
    const char *v = cmd_argv(a, 1);
    if (!v) { sh_printf("usage: sh_smp \"smp:<command> ...\" | on | off | status\n"); return; }
    if (!strcmp(v, "on") || !strcmp(v, "off")) InterlockedExchange(&g_enabled, !strcmp(v, "on"));
    if (!strcmp(v, "on") || !strcmp(v, "off") || !strcmp(v, "status")) {
        sh_printf("SMP: String Set commands %s, hook %s\n", InterlockedCompareExchange(&g_enabled, 0, 0) ? "on" : "off",
                  InterlockedCompareExchange(&g_installed, 0, 0) ? "installed" : "not installed");
        return;
    }
    /* The console splits on spaces; rejoin the arguments into one literal. */
    char literal[SMP_MAX_LITERAL + 1];
    size_t off = 0;
    literal[0] = 0;
    for (int i = 1; i < cmd_argc(a) && off < sizeof literal; i++) {
        const char *t = cmd_argv(a, i);
        if (!t) break;
        off += (size_t)snprintf(literal + off, sizeof literal - off, "%s%s", i > 1 ? " " : "", t);
    }
    const char *why = NULL;
    if (!ma_session_ok(&why)) { sh_printf("SMP: \"%s\": %s; nothing done\n", literal, why); return; }
    void *player = sh_entity_find("player1");
    if (!player) { sh_printf("SMP: no player1 (start a playtest first)\n"); return; }
    ma_run_literal(literal, player);
}

static void ma_print_event(const char *name)
{
    ma_event ev;
    if (!ma_event_info(name, &ev)) { sh_printf("sh_event: no event named '%s'\n", name); return; }
    const char *types = NULL, *names = NULL;
    char t[256] = "", n[256] = "";
    ma_read_ptr((uint8_t *)ev.rec + MA_REC_TYPES_OFF, &types, sizeof types);
    ma_read_ptr((uint8_t *)ev.rec + MA_REC_NAMES_OFF, &names, sizeof names);
    ma_read_cstr(types, t, sizeof t);
    ma_read_cstr(names, n, sizeof n);
    sh_printf("sh_event: '%s' number %d args %d format '%s' types '%s' names '%s'\n",
              name, ev.eventnum, ev.numargs, ev.format, t, n);
}

static int ma_registry_count(void *evmgr)
{
    ma_vfn_count cnt = (ma_vfn_count)ma_vfn(evmgr, MA_EV_COUNT_VSLOT);
    if (!cnt) return 0;
    __try { return cnt(evmgr); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void *ma_registry_at(void *evmgr, int i)
{
    ma_vfn_index at = (ma_vfn_index)ma_vfn(evmgr, MA_EV_COMMAND_VSLOT);
    if (!at) return NULL;
    __try { return at(evmgr, i); } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

int sh_modaction_registry_text(char *out, int cap)
{
    void *evmgr = ma_evmgr();
    int count = ma_registry_count(evmgr);
    if (!evmgr || count <= 0 || count > 0x1000) { if (out && cap > 0) out[0] = 0; return 0; }
    int need = 0;
    for (int i = 0; i < count; i++) {
        uint8_t *rec = (uint8_t *)ma_registry_at(evmgr, i);
        const char *pn = NULL, *pf = NULL, *pt = NULL, *pa = NULL;
        char nm[128], f[16], t[256], an[256];
        if (!rec || !ma_read_ptr(rec, &pn, sizeof pn) || !ma_read_cstr(pn, nm, sizeof nm) || !nm[0]) continue;
        ma_read_ptr(rec + MA_REC_FORMAT_OFF, &pf, sizeof pf);
        ma_read_ptr(rec + MA_REC_TYPES_OFF, &pt, sizeof pt);
        ma_read_ptr(rec + MA_REC_NAMES_OFF, &pa, sizeof pa);
        ma_read_cstr(pf, f, sizeof f);
        ma_read_cstr(pt, t, sizeof t);
        ma_read_cstr(pa, an, sizeof an);
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
    if (v && !strcmp(v, "info") && cmd_argv(a, 2)) { ma_print_event(cmd_argv(a, 2)); return; }
    if (v && !strcmp(v, "list")) {
        const char *filter = cmd_argv(a, 2);
        int need = sh_modaction_registry_text(NULL, 0);
        char *buf = need > 0 ? (char *)malloc((size_t)need + 1) : NULL;
        if (!buf) { sh_printf("sh_event: the event registry is not reachable\n"); return; }
        sh_modaction_registry_text(buf, need + 1);
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
#endif
