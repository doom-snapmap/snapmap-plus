/* modhud.c -- see modhud.h. The weapon ammo-display policy and its call-site
 * relay, fed from the builds the loaded map serves. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/text.h"
#include "modhud.h"
#include "config_json.h"
#include "modmanifest.h"
#ifndef SH_MODHUD_PURE
#include "backend_log.h"
#include "patch.h"
#endif

#define HUD_JSON_DEPTH   24

typedef struct hud_rule {
    char *name;              /* the weapon decl, malloc'd, any length */
    char *owner;             /* the mod's name, malloc'd, any length */
    int weapon_mode;
    int conflicted;          /* two mods ask different displays: the game's stays */
} hud_rule;
/* As many rules as the mods in use give. */
typedef struct hud_table { size_t count, cap; hud_rule *rules; } hud_table;

static hud_table g_live, g_pending;
static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_stale;
static int (*g_reload)(void);   /* the table from the serving index (engine), or a test's */

#ifdef SH_MODHUD_PURE
#define hud_log(...) ((void)0)
#else
static void hud_log(const char *fmt, ...)
{
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    backend_log(line ? line : "weapon-hud: (out of memory)");
    free(line);
}
#endif

/* ------------------------------------------------------------------ policy */

static int hud_name_valid(const char *s)
{
    const char *part = s;
    size_t n = strlen(s);
    if (!n || s[0] == '/' || s[n - 1] == '/') return 0;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!c || c == '/') {
            size_t len = (size_t)(s + i - part);
            if (!len || (len == 1 && part[0] == '.') ||
                (len == 2 && part[0] == '.' && part[1] == '.')) return 0;
            part = s + i + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int hud_string(const char *raw, char *out, size_t cap)
{
    size_t length = 0;
    return raw && sh_json_decode_string(raw, strlen(raw), out, cap, &length) && strlen(out) == length;
}

static void hud_table_free(hud_table *t)
{
    size_t i;
    for (i = 0; i < t->count; i++) { free(t->rules[i].name); free(t->rules[i].owner); }
    free(t->rules);
    memset(t, 0, sizeof *t);
}

void sh_modhud_begin(void) { hud_table_free(&g_pending); }

/* One weapon's member: 1 "weapon", 0 "engine", -1 when it does not read. */
static int hud_member_mode(const sh_json_member *m)
{
    sh_json_object settings = {0};
    char mode[24];
    int valid;
    if (strlen(m->key) != m->key_length || !hud_name_valid(m->key) ||
        !sh_json_parse_object(m->value_json, strlen(m->value_json), 2, &settings)) return -1;
    valid = settings.count == 1 && hud_string(sh_json_object_get(&settings, "ammo_display"), mode, sizeof mode);
    sh_json_object_free(&settings);
    if (!valid || (strcmp(mode, "weapon") && strcmp(mode, "engine"))) return -1;
    return strcmp(mode, "weapon") == 0;
}

static void hud_add(const char *weapon, int weapon_mode, const char *owner)
{
    size_t j;
    for (j = 0; j < g_pending.count; j++) {
        hud_rule *r = &g_pending.rules[j];
        if (strcmp(r->name, weapon)) continue;
        if (r->weapon_mode != weapon_mode && !r->conflicted) {
            r->conflicted = 1;
            hud_log("weapon-hud: %s and %s ask different ammo displays for %s; the game's display is kept for that "
                    "weapon", r->owner, owner, weapon);
        }
        return;
    }
    if (g_pending.count == g_pending.cap) {
        size_t cap = g_pending.cap ? g_pending.cap * 2 : 32;
        hud_rule *grown = (hud_rule *)realloc(g_pending.rules, cap * sizeof *grown);
        if (!grown) { hud_log("weapon-hud: out of memory; the rule %s gives %s is not used", owner, weapon); return; }
        g_pending.rules = grown;
        g_pending.cap = cap;
    }
    {
        hud_rule *r = &g_pending.rules[g_pending.count];
        r->name = _strdup(weapon);
        r->owner = _strdup(owner);
        if (!r->name || !r->owner) {
            free(r->name);
            free(r->owner);
            hud_log("weapon-hud: out of memory; the rule %s gives %s is not used", owner, weapon);
            return;
        }
        g_pending.count++;
        r->weapon_mode = weapon_mode;
        r->conflicted = 0;
    }
}

int sh_modhud_parse(const char *json, size_t length, const char *owner)
{
    sh_json_object root = {0}, weapons = {0};
    const char *raw;
    size_t i;
    int ok = 0;
    if (!owner) owner = "a mod";
    if (!json || !sh_json_parse_object(json, length, 5, &root)) goto done;
    if (!root.count) { ok = 1; goto done; }
    /* Exactly {"weapons": {...}}. */
    if (root.count != 1) goto done;
    raw = sh_json_object_get(&root, "weapons");
    if (!raw || !sh_json_parse_object(raw, strlen(raw), 3, &weapons)) goto done;
    /* Every member reads before any is added: a mod's table is used whole or not at all. */
    for (i = 0; i < weapons.count; i++) if (hud_member_mode(&weapons.members[i]) < 0) goto done;
    for (i = 0; i < weapons.count; i++) hud_add(weapons.members[i].key, hud_member_mode(&weapons.members[i]), owner);
    ok = 1;
done:
    sh_json_object_free(&weapons);
    sh_json_object_free(&root);
    return ok;
}

static size_t hud_count_of(const hud_table *t)
{
    size_t i, n = 0;
    for (i = 0; i < t->count; i++) if (t->rules[i].weapon_mode && !t->rules[i].conflicted) n++;
    return n;
}

void sh_modhud_publish(void)
{
    hud_table old;
    AcquireSRWLockExclusive(&g_lock);
    old = g_live;
    g_live = g_pending;   /* the pending table moves to live; a new one starts empty */
    memset(&g_pending, 0, sizeof g_pending);
    ReleaseSRWLockExclusive(&g_lock);
    hud_table_free(&old);
}

size_t sh_modhud_count(void)
{
    size_t n;
    AcquireSRWLockShared(&g_lock);
    n = hud_count_of(&g_live);
    ReleaseSRWLockShared(&g_lock);
    return n;
}

void sh_modhud_mark_stale(void) { InterlockedExchange(&g_stale, 1); }

unsigned char sh_modhud_select(const char *weapon, unsigned char engine_mode)
{
    unsigned char result = engine_mode;
    /* The HUD asks on the game thread: after an index change or a Play entry
     * the table is read again here, from the builds the open map serves. */
    if (InterlockedExchange(&g_stale, 0) && g_reload) (void)g_reload();
    if (!weapon) return result;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_live.count; i++) {
        if (!strcmp(weapon, g_live.rules[i].name)) {
            if (g_live.rules[i].weapon_mode && !g_live.rules[i].conflicted) result = 0;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return result;
}

/* ------------------------------------------------------------------ the builds */

typedef struct hud_load {
    int (*serves_now)(const char *);
    sh_modhud_label_fn label;
    char (*seen)[SH_MODMANIFEST_SALT_CAP];
    size_t nseen, cap;
    int failed;
} hud_load;

/* 1 when a build of `salt` was read already (or the list cannot grow). */
static int hud_seen(hud_load *c, const char *salt)
{
    size_t i;
    for (i = 0; i < c->nseen; i++) if (!strcmp(c->seen[i], salt)) return 1;
    if (c->nseen == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        void *grown = realloc(c->seen, cap * sizeof *c->seen);
        if (!grown) { c->failed = 1; return 1; }
        c->seen = (char (*)[SH_MODMANIFEST_SALT_CAP])grown;
        c->cap = cap;
    }
    strncpy_s(c->seen[c->nseen++], sizeof c->seen[0], salt, _TRUNCATE);
    return 0;
}

static void hud_visit(void *context, const char *salt, const char *hash, int place, const char *extra)
{
    hud_load *c = (hud_load *)context;
    sh_json_object root = {0};
    const char *raw;
    char *owner;
    (void)place;
    if (!salt || !hash || !extra || c->failed) return;
    /* One build per salt: the one the open map serves. */
    if (c->serves_now && !c->serves_now(hash)) return;
    if (hud_seen(c, salt)) return;
    if (!sh_json_parse_object(extra, strlen(extra), HUD_JSON_DEPTH, &root)) return;
    raw = sh_json_object_get(&root, "hud");
    if (raw) {
        owner = c->label ? c->label(salt, hash) : NULL;
        if (!sh_modhud_parse(raw, strlen(raw), owner ? owner : salt))
            hud_log("weapon-hud: the hud entry of %s is not a valid weapon ammo-display table; its rules are not used",
                    owner ? owner : salt);
        free(owner);
    }
    sh_json_object_free(&root);
}

int sh_modhud_load(sh_modhud_each_fn each, int (*serves_now)(const char *bundle_hash), sh_modhud_label_fn label)
{
    hud_load c;
    int visited;
    memset(&c, 0, sizeof c);
    c.serves_now = serves_now;
    c.label = label;
    sh_modhud_begin();
    visited = each ? each(hud_visit, &c) : -1;
    free(c.seen);
    if (visited < 0 || c.failed) sh_modhud_begin();
    sh_modhud_publish();
    if (visited < 0 || c.failed) {
        hud_log("weapon-hud: the mods' rules could not be read; the game's ammo displays are used");
        return -1;
    }
    hud_log("weapon-hud: %zu rule%s from the mods the open map uses", sh_modhud_count(),
            sh_modhud_count() == 1 ? "" : "s");
    return (int)sh_modhud_count();
}

/* ------------------------------------------------------------------ the call */
#ifndef SH_MODHUD_PURE

typedef unsigned char (*hud_mode_fn)(void *game);
static hud_mode_fn g_original;
static sh_patch_handle g_patch;
static void *g_relay;
static int g_hook_ready;
static volatile LONG g_reported;

/* The relay passes the widget in RDX; RCX is the original game argument. */
static unsigned char hud_mode(void *game, const unsigned char *widget)
{
    unsigned char original = g_original(game), result = original;
    char small[256], *name = NULL;
    size_t n = 0;
    /* The weapon's decl name, copied out of the engine's memory first (the
     * select takes a lock), whole whatever its length. */
    __try {
        const unsigned char *decl = widget ? *(const unsigned char *const *)(widget + 0x1f8) : NULL;
        const char *source = decl ? *(const char *const *)(decl + 8) : NULL;
        if (source) {
            n = strlen(source);
            name = n < sizeof small ? small : (char *)malloc(n + 1);
            if (name) memcpy(name, source, n + 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (name && name != small) free(name); name = NULL; }
    if (name) {
        result = sh_modhud_select(name, original);
        if (name != small) free(name);
    }
    if (result != original && InterlockedCompareExchange(&g_reported, 1, 0) == 0)
        hud_log("weapon-hud active: a mod's weapon-capacity display is in use");
    return result;
}

/* Our own small relay within rel32 reach of the call: RW while written, RX
 * before publication. mov rdx,rbx; mov rax,handler; jmp rax. */
static void *hud_near_relay(uintptr_t call, void *handler)
{
    SYSTEM_INFO info;
    uintptr_t step, center;
    GetSystemInfo(&info);
    step = info.dwAllocationGranularity;
    center = call & ~(step - 1);
    for (uintptr_t delta = step; delta < 0x7fff0000u; delta += step) {
        uintptr_t candidates[2] = { center >= delta ? center - delta : 0, center + delta };
        for (int i = 0; i < 2; i++) {
            uintptr_t address = candidates[i];
            MEMORY_BASIC_INFORMATION mbi;
            unsigned char *relay;
            static const unsigned char prefix[] = { 0x48, 0x89, 0xda, 0x48, 0xb8 };
            DWORD old;
            if (address < (uintptr_t)info.lpMinimumApplicationAddress ||
                address > (uintptr_t)info.lpMaximumApplicationAddress ||
                !VirtualQuery((void *)address, &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
            relay = (unsigned char *)VirtualAlloc((void *)address, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!relay) continue;
            memcpy(relay, prefix, sizeof prefix);
            memcpy(relay + 5, &handler, sizeof handler);
            relay[13] = 0xff; relay[14] = 0xe0;
            if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old)) {
                VirtualFree(relay, 0, MEM_RELEASE);
                return NULL;
            }
            FlushInstructionCache(GetCurrentProcess(), relay, 15);
            return relay;
        }
    }
    return NULL;
}

static int hud_route(const sig_result *results, size_t count)
{
    const sig_result *site = NULL, *predicate = NULL;
    unsigned char expected[5], replacement[5] = { 0xe8 };
    int32_t displacement;
    intptr_t distance;
    if (g_patch.live) return g_hook_ready;
    g_hook_ready = 0;
    for (size_t i = 0; i < count; i++) {
        if (!results[i].name) continue;
        if (!strcmp(results[i].name, "WeaponHudModeCall")) site = &results[i];
        if (!strcmp(results[i].name, "WeaponHudGameMode")) predicate = &results[i];
    }
    if (!site || !predicate || site->status != SIG_OK || predicate->status != SIG_OK) goto refused;
    memcpy(expected, (void *)site->addr, sizeof expected);
    memcpy(&displacement, expected + 1, sizeof displacement);
    if (expected[0] != 0xe8 || site->addr + 5 + (intptr_t)displacement != predicate->addr) goto refused;
    g_relay = hud_near_relay(site->addr, (void *)hud_mode);
    if (!g_relay) goto refused;
    distance = (intptr_t)g_relay - (intptr_t)(site->addr + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) goto free_relay;
    displacement = (int32_t)distance;
    memcpy(replacement + 1, &displacement, sizeof displacement);
    g_original = (hud_mode_fn)predicate->addr;
    if (code_patch_call_sig(site, expected, replacement, &g_patch) != B2_PATCH_OK) goto free_relay;
    g_hook_ready = 1;
    hud_log("weapon-hud installed: scoped ammo-display call, engine fallback for unlisted weapons");
    return 1;
free_relay:
    if (g_patch.live) {
        hud_log("weapon-hud rollback incomplete; relay retained for the outstanding patch");
        return 0;
    }
    VirtualFree(g_relay, 0, MEM_RELEASE);
    g_relay = NULL;
    g_original = NULL;
refused:
    hud_log("weapon-hud REFUSED: verified HUD call/predicate or relay unavailable; no hook installed");
    return 0;
}

#ifndef SH_MODHUD_TESTING
/* The build the open map serves for its salt: its names serve now. */
static int hud_serves_now(const char *hash)
{
    sh_modserve_scope_info info;
    return sh_modserve_bundle_scope(hash, &info) && info.names_live;
}

/* A mod by its name, else its folder, for the log (malloc'd). */
static char *hud_label(const char *salt, const char *hash)
{
    sh_modmanifest_row row;
    sh_modserve_bundle_ref ref;
    char *out;
    memset(&row, 0, sizeof row);
    memset(&ref, 0, sizeof ref);
    /* A salt and a build hash have one width each. */
    if (strlen(salt) < sizeof row.salt && strlen(hash) < sizeof row.build) {
        strcpy_s(row.salt, sizeof row.salt, salt);
        strcpy_s(row.build, sizeof row.build, hash);
        (void)sh_modserve_row_build(&row, &ref);
    }
    out = _strdup(ref.name && ref.name[0] ? ref.name : ref.folder && ref.folder[0] ? ref.folder : salt);
    sh_modserve_bundle_ref_free(&ref);
    return out;
}

static int hud_reload_index(void) { return sh_modhud_load(sh_modserve_each_bundle_extra, hud_serves_now, hud_label); }

int sh_modhud_install(const sig_result *results, size_t count)
{
    g_reload = hud_reload_index;
    sh_modserve_set_index_listener(sh_modhud_mark_stale);
    sh_modhud_mark_stale();
    return hud_route(results, count);
}
#else
void sh_modhud_test_reload_with(int (*load)(void)) { g_reload = load; }

/* ------------------------------------------------------------------ test */
typedef struct hud_test_runner {
    unsigned char (*call)(void *, void *);
    volatile LONG stop, failed, iterations;
    HANDLE ready;
} hud_test_runner;

static DWORD WINAPI hud_test_run(LPVOID parameter)
{
    hud_test_runner *run = (hud_test_runner *)parameter;
    unsigned char game = 1, widget[0x208] = {0};
    if (run->call(&game, widget) != 1) InterlockedIncrement(&run->failed);
    InterlockedIncrement(&run->iterations);
    SetEvent(run->ready);
    while (!InterlockedCompareExchange(&run->stop, 0, 0)) {
        if (run->call(&game, widget) != 1) InterlockedIncrement(&run->failed);
        InterlockedIncrement(&run->iterations);
    }
    return 0;
}

/* An owned synthetic caller: push rbx; sub rsp,0x20; mov rbx,rdx; call
 * predicate; add rsp,0x20; pop rbx; ret, and predicate: mov al,[rcx]; ret.
 * The call's rel32 starts at +12, 4-byte aligned. No game image is used. */
int sh_modhud_test_relay(void)
{
    unsigned char *code = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char widget[0x208] = {0}, decl[16] = {0}, game = 1;
    const char *name = "weapon/test/unlimited";
    static const unsigned char caller[] = { 0x90, 0x90, 0x90, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xd3,
                                            0xe8, 0x30, 0, 0, 0, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3 };
    static const unsigned char predicate[] = { 0x8a, 0x01, 0xc3 };
    sig_result results[2] = { { "WeaponHudModeCall", SIG_OK, 0, 0 }, { "WeaponHudGameMode", SIG_OK, 0, 0 } };
    DWORD old;
    int ok = 0;
    hud_test_runner run = {0};
    HANDLE thread = NULL;
    unsigned char (*call)(void *, void *);
    void *d = decl;
    if (!code) return 0;
    memcpy(code, caller, sizeof caller);
    memcpy(code + 64, predicate, sizeof predicate);
    memcpy(decl + 8, &name, sizeof name);
    memcpy(widget + 0x1f8, &d, sizeof d);
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    call = (unsigned char (*)(void *, void *))code;
    if (call(&game, widget) != 1) goto done;
    run.call = call;
    run.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!run.ready) goto done;
    thread = CreateThread(NULL, 0, hud_test_run, &run, 0, NULL);
    if (!thread || WaitForSingleObject(run.ready, 2000) != WAIT_OBJECT_0) goto done;
    results[0].addr = (uintptr_t)code + 11;
    results[1].addr = (uintptr_t)code + 64;
    /* A decoded call target that is not the predicate leaves the caller untouched. */
    results[1].addr++;
    if (hud_route(results, 2) || memcmp(code, caller, sizeof caller)) goto done;
    results[1].addr--;
    if (!hud_route(results, 2)) goto done;
    {
        LONG initial = InterlockedCompareExchange(&run.iterations, 0, 0);
        ULONGLONG deadline = GetTickCount64() + 2000;
        while (InterlockedCompareExchange(&run.iterations, 0, 0) - initial < 100 && GetTickCount64() < deadline) Sleep(1);
        if (InterlockedCompareExchange(&run.iterations, 0, 0) - initial < 100) goto done;
    }
    /* The rule table the caller set up names weapon/test/unlimited. */
    if (call(&game, widget) != 0 || game != 1) goto done;
    name = "weapon/test/other";
    memcpy(decl + 8, &name, sizeof name);
    if (call(&game, widget) != 1) goto done;
    game = 0;
    if (call(&game, widget) != 0) goto done;
    if (call(&game, NULL) != 0) goto done;
    ok = 1;
done:
    if (g_patch.live && code_unpatch(&g_patch) != B2_PATCH_OK) ok = 0;
    InterlockedExchange(&run.stop, 1);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        if (run.failed || !run.iterations) ok = 0;
    }
    if (run.ready) CloseHandle(run.ready);
    if (!g_patch.live) {
        if (g_relay) VirtualFree(g_relay, 0, MEM_RELEASE);
        g_relay = NULL; g_original = NULL; g_hook_ready = 0;
    }
    if (memcmp(code, caller, sizeof caller)) ok = 0;
    VirtualFree(code, 0, MEM_RELEASE);
    return ok;
}
#endif
#endif
