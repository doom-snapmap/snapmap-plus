/* modlobby -- a co-op lobby for a map whose content comes from the mod store (modlobby.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modlobby.h"

/* ------------------------------------------------------------ the state codes */

unsigned sh_modlobby_progress_code(unsigned long long done, unsigned long long total)
{
    /* In steps of 5 %: one message per step at most. */
    unsigned long long pct = total ? done * 100 / total : 0;
    if (pct > 100) pct = 100;
    return SH_MODLOBBY_CODE_PROGRESS + (unsigned)(pct - pct % 5);
}

int sh_modlobby_decode(unsigned code, int *state, int *percent)
{
    int s, p = 0;
    if (code >= SH_MODLOBBY_CODE_PROGRESS && code <= SH_MODLOBBY_CODE_PROGRESS + 100) {
        s = SH_MODLOBBY_DOWNLOADING;
        p = (int)(code - SH_MODLOBBY_CODE_PROGRESS);
    } else if (code == SH_MODLOBBY_CODE_HOLDS) {
        s = SH_MODLOBBY_HOLDS;
        p = 100;
    } else if (code == SH_MODLOBBY_CODE_FAILED) {
        s = SH_MODLOBBY_FAILED;
    } else {
        return 0;
    }
    if (state) *state = s;
    if (percent) *percent = p;
    return 1;
}

/* ------------------------------------------------------------ the host's table */

#define LB_MEMBERS 16   /* a SnapMap lobby holds four players; the table outlives a few who came and went */

static struct { uint64_t user; int state, percent; } g_table[LB_MEMBERS];
static size_t g_table_count;
static SRWLOCK g_table_lock = SRWLOCK_INIT;   /* noted on the network's thread of the main loop, read by the tick */

void sh_modlobby_note(uint64_t user, unsigned code)
{
    int state, percent;
    size_t i;
    if (!sh_modlobby_decode(code, &state, &percent)) return;
    AcquireSRWLockExclusive(&g_table_lock);
    for (i = 0; i < g_table_count && g_table[i].user != user; i++) {}
    if (i == g_table_count) {
        if (g_table_count < LB_MEMBERS) g_table_count++;
        else { memmove(g_table, g_table + 1, sizeof g_table[0] * (LB_MEMBERS - 1)); i = LB_MEMBERS - 1; }
    }
    g_table[i].user = user;
    g_table[i].state = state;
    g_table[i].percent = percent;
    ReleaseSRWLockExclusive(&g_table_lock);
}

void sh_modlobby_forget(void)
{
    AcquireSRWLockExclusive(&g_table_lock);
    g_table_count = 0;
    ReleaseSRWLockExclusive(&g_table_lock);
}

int sh_modlobby_state_of(uint64_t user, int *percent)
{
    int state = SH_MODLOBBY_UNKNOWN;
    if (percent) *percent = 0;
    AcquireSRWLockShared(&g_table_lock);
    for (size_t i = 0; i < g_table_count; i++)
        if (g_table[i].user == user) { state = g_table[i].state; if (percent) *percent = g_table[i].percent; }
    ReleaseSRWLockShared(&g_table_lock);
    return state;
}

/* ------------------------------------------------------------ the host's gate */

/* One pending member as the sentences say it: "Sam (42%)", "Sam (could not get it)", or "Sam". */
static void lb_member_text(const sh_modlobby_member *m, int with_parens, char *out, size_t cap)
{
    int percent, state = sh_modlobby_state_of(m->user, &percent);
    const char *name = m->name && m->name[0] ? m->name : "a player";
    if (state == SH_MODLOBBY_DOWNLOADING)
        _snprintf_s(out, cap, _TRUNCATE, with_parens ? "%s (%d%%)" : "%s %d%%", name, percent);
    else if (state == SH_MODLOBBY_FAILED)
        _snprintf_s(out, cap, _TRUNCATE, with_parens ? "%s (could not get it)" : "%s could not get it", name);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%s", name);
}

/* The pending members joined as "A, B and C", fitting `cap` (those that do not fit: "and N more"). The number of
 * pending members is returned. */
static size_t lb_pending_list(const sh_modlobby_member *members, size_t count, int with_parens, const char *join_last,
                              char *out, size_t cap)
{
    size_t pending = 0, listed = 0, i;
    out[0] = 0;
    for (i = 0; i < count; i++) if (members[i].pending) pending++;
    for (i = 0; i < count; i++) {
        char one[96], more[32] = "";
        size_t sep, rest;
        if (!members[i].pending) continue;
        lb_member_text(&members[i], with_parens, one, sizeof one);
        rest = pending - listed - 1;
        if (rest) _snprintf_s(more, sizeof more, _TRUNCATE, " and %u more", (unsigned)rest);
        sep = listed ? (rest ? 2 : strlen(join_last)) : 0;
        if (strlen(out) + sep + strlen(one) + strlen(more) >= cap) break;
        if (listed) strcat_s(out, cap, rest ? ", " : join_last);
        strcat_s(out, cap, one);
        listed++;
    }
    if (listed < pending) {
        char more[32];
        _snprintf_s(more, sizeof more, _TRUNCATE, listed ? " and %u more" : "%u players", (unsigned)(pending - listed));
        if (strlen(out) + strlen(more) < cap) strcat_s(out, cap, more);
    }
    return pending;
}

int sh_modlobby_gate(const sh_modlobby_member *members, size_t count, char *out, size_t cap)
{
    static const char head[] = "The match starts when every player has this map's mod content. Still getting it: ";
    char list[256];
    if (out && cap) out[0] = 0;
    if (!members || !lb_pending_list(members, count, 1, " and ", list, sizeof list - sizeof head)) return 1;
    if (out && cap) _snprintf_s(out, cap, _TRUNCATE, "%s%s.", head, list);
    return 0;
}

int sh_modlobby_waiting_line(const sh_modlobby_member *members, size_t count, char *out, size_t cap)
{
    static const char head[] = "Waiting for this map's mod content: ";
    char list[256];
    if (out && cap) out[0] = 0;
    if (!members || !lb_pending_list(members, count, 0, ", ", list, sizeof list - sizeof head)) return 0;
    if (out && cap) _snprintf_s(out, cap, _TRUNCATE, "%s%s.", head, list);
    return 1;
}

/* ================================================================== engine
 * Build 13954591, both renderers: the sites are located by signature and their
 * offsets are equal on both (read in Ghidra on each, R-CO1). Every engine read
 * and call is SEH-guarded; a failed bind installs nothing. */
#ifndef SH_MODLOBBY_PURE
#include <stdarg.h>
#include "hook.h"
#include "backend_log.h"
#include "engine_globals.h"
#include "modpublished.h"

/* idLobby (the session's game lobby, session vtable +0x210). */
#define LB_TYPE          0x1E80   /* 1: a game lobby */
#define LB_USERS         0x4E20   /* idLobbyUser *[] */
#define LB_USER_COUNT    0x4E28
#define LB_PEERS         0xB728   /* peer records, stride LB_PEER_SIZE */
#define LB_PEER_COUNT    0xB730
#define LB_PEER_SIZE     0x4B8
#define LB_PEER_STATE    0x0      /* 2: connected */
#define LB_PEER_VERIFY   0x4      /* the map check: 0 none or answered, 1 asked, 2 sent its start */
#define LB_PARAMETERS    0x2B8    /* vtable: the launch parameters */
#define LB_PARAM_ITEM    0x550    /* parameters: the published item id (idStr) */
#define LB_PARAM_REV     0x600    /* parameters: its revision (idStr) */
#define LB_START_BYTE    0xF58A   /* the acting game lobby: a peer's start was asked (reliable 3 or 5) */
/* idLobbyUser. */
#define LU_PEER          0x10     /* its peer index in the host's lobby */
#define LU_ID            0x14     /* lobbyUserID_t: int, then a byte */
#define LU_NAME          0x681    /* the name the lobby shows */
#define SESSION_GAME_LOBBY 0x210  /* session vtable: the game lobby */
#define CONTROLLER_IDLE  0        /* idLobbyController's first state, the one its pump drops a countdown to */
#define SHELL_MANAGER    0x18     /* shell: the menu manager */

typedef void (*lb_screen_fn)(void *screen);
typedef void (*lb_controller_fn)(int *controller);
typedef void (*lb_ready_fn)(void *lobby, uint64_t user, int command);
typedef unsigned long long (*lb_peer_fn)(void *session);
typedef unsigned long long (*lb_send_fn)(void *screen, unsigned command);
typedef void *(*lb_lobby_of_fn)(void *session);

static struct {
    const uint8_t *site_try, *site_controller, *site_ready, *site_peer, *send;
    void *orig_try, *orig_controller, *orig_ready, *orig_peer;
    void **session_slot, **shell_slot;
    lb_lobby_of_fn acting_lobby;
    volatile LONG ready;
    char line[256];
    int line_shown;
    ULONGLONG line_at;
    char item[128], rev[128];       /* the lobby map the host's table belongs to */
} g_lb;

static void lb_logf(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void *lb_game_lobby(void)
{
    __try {
        void *session = g_lb.session_slot ? *g_lb.session_slot : NULL;
        return session ? ((void *(*)(void *))(*(void ***)session)[SESSION_GAME_LOBBY / sizeof(void *)])(session) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* A native idStr's text into `out` (length at +8, data at +0x10). */
static int lb_idstr(const uint8_t *s, char *out, size_t cap)
{
    out[0] = 0;
    __try {
        int length = *(const int *)(s + 8);
        const char *data = *(const char *const *)(s + 0x10);
        if (length < 0 || (size_t)length >= cap || (length && !data)) return 0;
        memcpy(out, data, (size_t)length);
        out[length] = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 0; }
}

/* This machine hosts a game lobby whose map a gate saw with content rows: its item and revision. */
static int lb_hosting_modded(void *lobby, char *item, size_t icap, char *rev, size_t rcap)
{
    const uint8_t *parameters = NULL;
    int host = 0, type = -1;
    if (!lobby) return 0;
    __try {
        host = ((unsigned char (*)(void *))(*(void ***)lobby)[1])(lobby) != 0;
        type = *(const int *)((const uint8_t *)lobby + LB_TYPE);
        if (host && type == 1)
            parameters = ((const uint8_t *(*)(void *))(*(void ***)lobby)[LB_PARAMETERS / sizeof(void *)])(lobby);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!parameters || !lb_idstr(parameters + LB_PARAM_ITEM, item, icap) || !lb_idstr(parameters + LB_PARAM_REV, rev, rcap))
        return 0;
    return sh_modpub_lobby_map_modded(item, rev);
}

/* The host lobby's members: each connected peer, pending while it has not answered the map check, named by its user. */
static size_t lb_members(void *lobby, sh_modlobby_member *out, char (*names)[64], size_t cap)
{
    size_t n = 0;
    __try {
        const uint8_t *lb = (const uint8_t *)lobby;
        const uint8_t *peers = *(const uint8_t *const *)(lb + LB_PEERS);
        int peer_count = *(const int *)(lb + LB_PEER_COUNT);
        void *const *users = *(void *const *const *)(lb + LB_USERS);
        int user_count = *(const int *)(lb + LB_USER_COUNT);
        for (int p = 0; peers && p < peer_count && n < cap; p++) {
            const uint8_t *peer = peers + (size_t)p * LB_PEER_SIZE;
            if (*(const int *)(peer + LB_PEER_STATE) != 2) continue;
            memset(&out[n], 0, sizeof out[n]);
            names[n][0] = 0;
            out[n].pending = *(const int *)(peer + LB_PEER_VERIFY) == 1;
            for (int u = 0; users && u < user_count; u++) {
                const uint8_t *user = (const uint8_t *)users[u];
                if (!user || *(const int *)(user + LU_PEER) != p) continue;
                out[n].user = (uint64_t)*(const uint32_t *)(user + LU_ID) | ((uint64_t)user[LU_ID + 4] << 32);
                strncpy_s(names[n], 64, (const char *)(user + LU_NAME), _TRUNCATE);
                break;
            }
            out[n].name = names[n];
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}

/* The host's decision now: 1 when the start may run (not a modded map's lobby we host, or everyone answered). */
static int lb_host_may_start(const char *where)
{
    sh_modlobby_member members[8];
    char names[8][64], item[128], rev[128], sentence[256];
    void *lobby = lb_game_lobby();
    size_t n;
    if (!lb_hosting_modded(lobby, item, sizeof item, rev, sizeof rev)) return 1;
    n = lb_members(lobby, members, names, 8);
    if (sh_modlobby_gate(members, n, sentence, sizeof sentence)) return 1;
    lb_logf("MODS: lobby: the %s of '%s' waits: a player has not got the map's mod content yet", where, item);
    sh_modpublished_notice(sentence);
    return 0;
}

/* idMenuScreen_Shell_SnapmapLobby::TryToStartMatch (the lobby's Start, and the Update that starts after a kick). */
static void lb_try_detour(void *screen)
{
    int pass = 1;
    if (InterlockedCompareExchange(&g_lb.ready, 0, 0)) {
        __try { pass = lb_host_may_start("start"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: lobby: the start gate faulted; the start runs"); pass = 1; }
    }
    if (pass) ((lb_screen_fn)g_lb.orig_try)(screen);
}

/* The lobby controller's start step, at the countdown's end: a player who joined during the countdown has not got the
 * content, so the countdown goes back to idle, as the controller's own pump drops it when players leave. */
static void lb_controller_detour(int *controller)
{
    int pass = 1;
    if (InterlockedCompareExchange(&g_lb.ready, 0, 0)) {
        __try { pass = lb_host_may_start("countdown"); if (!pass && controller) *controller = CONTROLLER_IDLE; }
        __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: lobby: the countdown gate faulted; the start runs"); pass = 1; }
    }
    if (pass) ((lb_controller_fn)g_lb.orig_controller)(controller);
}

/* idLobby::SetLobbyUserReady on the host: a product joiner's state code. The engine applies the bytes it knows and
 * ignores ours; it runs unchanged for every byte. */
static void lb_ready_detour(void *lobby, uint64_t user, int command)
{
    if (InterlockedCompareExchange(&g_lb.ready, 0, 0) && command > 2 && sh_modlobby_decode((unsigned)command, NULL, NULL)) {
        sh_modlobby_note(user, (unsigned)command);
        lb_logf("MODS: lobby: member %08x reports content code 0x%02x", (unsigned)user, (unsigned)command);
    }
    ((lb_ready_fn)g_lb.orig_ready)(lobby, user, command);
}

/* idSessionLocal::State_Game_Lobby_Peer: a joiner whose content is still downloading must not load. */
static unsigned long long lb_peer_detour(void *session)
{
    if (InterlockedCompareExchange(&g_lb.ready, 0, 0)) {
        __try {
            uint8_t *lobby = g_lb.acting_lobby ? (uint8_t *)g_lb.acting_lobby(session) : NULL;
            if (lobby && lobby[LB_START_BYTE] && sh_modpub_joiner_start_held()) lobby[LB_START_BYTE] = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: lobby: the joiner's start guard faulted"); }
    }
    return ((lb_peer_fn)g_lb.orig_peer)(session);
}

void sh_modlobby_send(unsigned code)
{
    /* The screen's own sender reads only the screen's menu manager (+0x18, else +0x20's): the shell's. */
    void *fake[5] = { 0 };
    if (!InterlockedCompareExchange(&g_lb.ready, 0, 0) || !g_lb.send || code <= 2) return;
    __try {
        void *shell = g_lb.shell_slot ? *g_lb.shell_slot : NULL;
        fake[3] = shell ? *(void **)((uint8_t *)shell + SHELL_MANAGER) : NULL;
        if (!fake[3]) return;
        if (((lb_send_fn)g_lb.send)(fake, code))
            lb_logf("MODS: lobby: told the host content code 0x%02x", code);
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("MODS: lobby: sending the content state faulted"); }
}

void sh_modlobby_poll(void)
{
    sh_modlobby_member members[8];
    char names[8][64], item[128], rev[128], line[256];
    void *lobby;
    size_t n;
    ULONGLONG now = GetTickCount64();
    if (!InterlockedCompareExchange(&g_lb.ready, 0, 0) || now - g_lb.line_at < 250) return;
    g_lb.line_at = now;
    lobby = lb_game_lobby();
    if (!lb_hosting_modded(lobby, item, sizeof item, rev, sizeof rev)) {
        if (g_lb.item[0]) { sh_modlobby_forget(); g_lb.item[0] = g_lb.rev[0] = 0; }
        if (g_lb.line_shown) { sh_modpublished_lobby_line(NULL); g_lb.line_shown = 0; g_lb.line[0] = 0; }
        return;
    }
    if (strcmp(item, g_lb.item) || strcmp(rev, g_lb.rev)) {
        /* Another map: what members said was about the last one. */
        sh_modlobby_forget();
        strcpy_s(g_lb.item, sizeof g_lb.item, item);
        strcpy_s(g_lb.rev, sizeof g_lb.rev, rev);
    }
    n = lb_members(lobby, members, names, 8);
    if (sh_modlobby_waiting_line(members, n, line, sizeof line)) {
        if (!g_lb.line_shown || strcmp(line, g_lb.line)) {
            sh_modpublished_lobby_line(line);
            strcpy_s(g_lb.line, sizeof g_lb.line, line);
            g_lb.line_shown = 1;
        }
    } else if (g_lb.line_shown) {
        sh_modpublished_lobby_line(NULL);
        g_lb.line_shown = 0;
        g_lb.line[0] = 0;
    }
}

/* ------------------------------------------------------------ install */

static const uint8_t *lb_site(const sig_result *results, size_t count, const uint8_t *base, const char *name)
{
    const uint8_t *site = NULL;
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name)) {
        if (site || results[i].status != SIG_OK || !results[i].addr || results[i].addr != (uintptr_t)base + results[i].rva)
            return NULL;
        site = (const uint8_t *)results[i].addr;
    }
    return site;
}

static void *lb_rip(const uint8_t *p, const char *opcode, size_t n)
{
    int32_t d;
    if (memcmp(p, opcode, n)) return NULL;
    memcpy(&d, p + n, 4);
    return (void *)(p + n + 4 + d);
}

static void *lb_call(const uint8_t *p)
{
    int32_t d;
    if (p[0] != 0xE8) return NULL;
    memcpy(&d, p + 1, 4);
    return (void *)(p + 5 + d);
}

static int lb_has(const uint8_t *from, size_t span, const char *bytes, size_t n)
{
    for (size_t i = 0; i + n <= span; i++) if (!memcmp(from + i, bytes, n)) return 1;
    return 0;
}

/* Every offset the detours and the sender rely on, checked in the bytes of both renderers' functions (R-CO1). */
static int lb_bind(const sig_result *results, size_t count, const uint8_t *base)
{
    const uint8_t *confirm;
    void **session_from_send;
    g_lb.site_try = lb_site(results, count, base, "SnapmapLobbyTryToStartMatch");
    g_lb.site_controller = lb_site(results, count, base, "LobbyControllerStartMatch");
    g_lb.site_ready = lb_site(results, count, base, "LobbySetUserReady");
    g_lb.site_peer = lb_site(results, count, base, "SessionStateGameLobbyPeer");
    g_lb.send = lb_site(results, count, base, "SnapmapLobbySendReadyCommand");
    confirm = lb_site(results, count, base, "LobbyPeerConfirmsMap");
    if (!g_lb.site_try || !g_lb.site_controller || !g_lb.site_ready || !g_lb.site_peer || !g_lb.send || !confirm) {
        backend_log("MODS: lobby: a co-op lobby site did not resolve cleanly");
        return 0;
    }
    __try {
        /* The sender: the session's game lobby (vtable +0x210) at +0x76, the 0xB6 ready command at +0x179, the host's
         * own apply on the controller (shell manager +0x918) at +0x146. */
        session_from_send = (void **)lb_rip(g_lb.send + 0x54, "\x48\x8b\x0d", 3);
        if (!session_from_send || memcmp(g_lb.send + 0x76, "\xff\x90\x10\x02\x00\x00", 6) ||
            memcmp(g_lb.send + 0x179, "\xba\xb6\x00\x00\x00", 5) ||
            memcmp(g_lb.send + 0x146, "\x48\x8d\x8f\x18\x09\x00\x00", 7)) return 0;
        /* The controller's start step reads the same session, and its game lobby's parameters (+0x2B8). */
        if (lb_rip(g_lb.site_controller + 0x10, "\x48\x8b\x0d", 3) != (void *)session_from_send ||
            memcmp(g_lb.site_controller + 0xF6, "\xff\x90\x10\x02\x00\x00", 6) ||
            !lb_has(g_lb.site_controller + 0xF6, 0x30, "\xff\x90\xb8\x02\x00\x00", 6)) return 0;
        /* The joiner's peer state: the acting lobby's call at +0x57, then its start byte. */
        if (!(g_lb.acting_lobby = (lb_lobby_of_fn)lb_call(g_lb.site_peer + 0x57)) ||
            memcmp(g_lb.site_peer + 0x5C, "\x80\xb8\x8a\xf5\x00\x00\x00", 7)) return 0;
        /* The peer records and the users list, as the host's map confirmation and SetLobbyUserReady read them. */
        if (memcmp(confirm + 0x15, "\x48\x69\xdf\xb8\x04\x00\x00\x48\x03\x99\x28\xb7\x00\x00\x83\x3b\x02", 17) ||
            memcmp(confirm + 0x28, "\x83\x7b\x04\x01", 4) ||
            !lb_has(g_lb.site_ready, 0x40, "\x48\x8b\x83\x20\x4e\x00\x00", 7)) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_lb.session_slot = session_from_send;
    g_lb.shell_slot = (void **)glb_resolve(base, "shell_ptr_slot", NULL);
    return g_lb.shell_slot != NULL;
}

int sh_modlobby_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    if (InterlockedCompareExchange(&g_lb.ready, 0, 0)) return 1;
    if (!results || !module_base || !lb_bind(results, count, module_base)) {
        backend_log("MODS: lobby: the co-op lobby sites did not verify; lobbies run as the engine runs them");
        return 0;
    }
    /* Every window is whole, position-independent instructions (detour_windows_test). */
    g_lb.orig_try = hook_prepare((void *)g_lb.site_try, (void *)lb_try_detour, 16);
    g_lb.orig_controller = hook_prepare((void *)g_lb.site_controller, (void *)lb_controller_detour, 16);
    g_lb.orig_ready = hook_prepare((void *)g_lb.site_ready, (void *)lb_ready_detour, 18);
    g_lb.orig_peer = hook_prepare((void *)g_lb.site_peer, (void *)lb_peer_detour, 16);
    if (!g_lb.orig_try || !g_lb.orig_controller || !g_lb.orig_ready || !g_lb.orig_peer) goto failed;
    if (hook_commit(g_lb.orig_try) != B2_PATCH_OK || hook_commit(g_lb.orig_controller) != B2_PATCH_OK ||
        hook_commit(g_lb.orig_ready) != B2_PATCH_OK || hook_commit(g_lb.orig_peer) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_lb.ready, 1);
    backend_log("MODS: lobby: co-op lobbies for modded maps installed (the host's start waits for every player's map "
                "content; a joiner downloads in the lobby)");
    return 1;
failed:
    {
        void *all[4] = { g_lb.orig_peer, g_lb.orig_ready, g_lb.orig_controller, g_lb.orig_try };
        for (size_t i = 0; i < 4; i++) if (all[i]) (void)hook_unpatch(all[i]);
    }
    memset(&g_lb, 0, sizeof g_lb);
    backend_log("MODS: lobby: the co-op lobby hooks could not be installed; lobbies run as the engine runs them");
    return 0;
}
#endif

#ifdef SH_MODLOBBY_TESTING
void sh_modlobby_test_reset(void) { sh_modlobby_forget(); }
#endif
