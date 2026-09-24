/* weapon_hud.c -- bounded declarative HUD policies and one scoped call-site hook. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "weapon_hud.h"
#include "package_hud.h"
#include "packages.h"
#include "package_runtime.h"
#include "config_json.h"
#include "backend_log.h"
#include "patch.h"

typedef struct hud_rule { char *name; int weapon_mode; } hud_rule;
typedef struct hud_table { size_t count; hud_rule *rules; } hud_table;
static hud_table g_rules;
static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_enabled;
static void *g_relay;
static sh_patch_handle g_patch;
static int g_hook_ready;
typedef unsigned char (*hud_mode_fn)(void *game);
static hud_mode_fn g_original;
static volatile LONG g_reported;

static void hud_free(hud_table *table)
{
    for (size_t i = 0; i < table->count; i++) free(table->rules[i].name);
    free(table->rules); memset(table, 0, sizeof(*table));
}

static int hud_parse(hud_table *table, const char *json, size_t length)
{
    sh_json_object source = {0}, canonical = {0}, weapons = {0};
    int ok = 0;
    if (!sh_json_parse_object(json, length, 5, &source) ||
        !sh_package_hud_merge_provisional(&canonical, &source, NULL, 0, NULL)) goto done;
    const char *raw = sh_json_object_get(&canonical, "weapons");
    if (!raw) { ok = 1; goto done; }
    if (!sh_json_parse_object(raw, strlen(raw), 3, &weapons) ||
        weapons.count > SIZE_MAX / sizeof(*table->rules)) goto done;
    table->rules = calloc(weapons.count ? weapons.count : 1, sizeof(*table->rules));
    if (!table->rules) goto done;
    for (size_t i = 0; i < weapons.count; i++) {
        hud_rule *rule = table->rules + table->count++;
        rule->name = _strdup(weapons.members[i].key);
        if (!rule->name || !sh_package_hud_mode(weapons.members[i].value_json, &rule->weapon_mode)) goto done;
    }
    ok = 1;
done:
    sh_json_object_free(&source); sh_json_object_free(&canonical); sh_json_object_free(&weapons);
    if (!ok) hud_free(table);
    return ok;
}

int sh_weapon_hud_reload(const char *data_root)
{
    hud_table next = {0}, previous = {0};
    size_t length = 0;
    char *json = NULL, message[512];
    int ok = 0;
    (void)data_root;
    if (!g_enabled) return 1;
    json = sh_package_runtime_active_policy("hud", &length);
    if (json) ok = hud_parse(&next, json, length);
    AcquireSRWLockExclusive(&g_lock);
    if (ok) { previous = g_rules; g_rules = next; memset(&next, 0, sizeof(next)); }
    InterlockedExchange(&g_reported, 0);
    snprintf(message, sizeof(message), "weapon-hud %s: %zu rules", ok ? "compiled" : "retained previous policy", g_rules.count);
    ReleaseSRWLockExclusive(&g_lock);
    backend_log(message); free(json); hud_free(&previous); hud_free(&next); return ok;
}

unsigned char sh_weapon_hud_select(const char *weapon, unsigned char engine_mode)
{
    unsigned char result = engine_mode;
    if (!weapon) return result;
    AcquireSRWLockShared(&g_lock);
    __try {
    for (size_t i = 0; i < g_rules.count; i++) {
        if (!strcmp(weapon, g_rules.rules[i].name)) {
            if (g_rules.rules[i].weapon_mode) result = 0;
            break;
        }
    }
    } __finally { ReleaseSRWLockShared(&g_lock); }
    return result;
}
/* The call-site relay passes the widget in RDX; RCX is the original game
 * argument. No widget writes, global predicate detour or TLS context is needed. */
static unsigned char hud_mode(void *game, const unsigned char *widget)
{
    unsigned char original = g_original(game), result = original;

    __try {
        const unsigned char *decl = *(const unsigned char *const *)(widget + 0x1f8);
        const char *source = decl ? *(const char *const *)(decl + 8) : NULL;
        if (source) result = sh_weapon_hud_select(source, original);
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = original; }
    if (result != original && InterlockedCompareExchange(&g_reported, 1, 0) == 0)
        backend_log("weapon-hud active: package weapon-capacity display selected");
    return result;
}

/* Allocate only our own small relay near the signed call. RW while constructing,
 * RX before publication. The relay is a leaf tail jump with no stack changes. */
static void *hud_near_relay(uintptr_t call, void *handler)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    uintptr_t step = info.dwAllocationGranularity;
    uintptr_t center = call & ~(step - 1);
    for (uintptr_t delta = step; delta < 0x7fff0000u; delta += step) {
        uintptr_t candidates[2] = {center >= delta ? center - delta : 0, center + delta};
        for (int i = 0; i < 2; i++) {
            uintptr_t address = candidates[i];
            MEMORY_BASIC_INFORMATION mbi;
            if (address < (uintptr_t)info.lpMinimumApplicationAddress ||
                address > (uintptr_t)info.lpMaximumApplicationAddress ||
                !VirtualQuery((void *)address, &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
            unsigned char *relay = (unsigned char *)VirtualAlloc((void *)address, 4096,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!relay) continue;
            /* mov rdx,rbx; mov rax,handler; jmp rax */
            const unsigned char prefix[] = {0x48,0x89,0xda,0x48,0xb8};
            DWORD old;
            memcpy(relay, prefix, sizeof prefix);
            memcpy(relay + 5, &handler, sizeof handler);
            relay[13] = 0xff; relay[14] = 0xe0;
            if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old)) {
                VirtualFree(relay, 0, MEM_RELEASE); return NULL;
            }
            FlushInstructionCache(GetCurrentProcess(), relay, 15);
            return relay;
        }
    }
    return NULL;
}

int sh_weapon_hud_install(const char *root, const uint8_t *module,
                          const sig_result *results, size_t count, int enabled)
{
    const sig_result *site = NULL, *predicate = NULL;
    unsigned char expected[5], replacement[5] = {0xe8};
    int32_t displacement;
    intptr_t distance;
    (void)module;
    if (g_patch.live) {
        if (g_hook_ready) return 1;
        if (code_unpatch(&g_patch) != B2_PATCH_OK) return 0;
        VirtualFree(g_relay, 0, MEM_RELEASE); g_relay = NULL; g_original = NULL;
    }
    g_hook_ready = 0;
    g_enabled = enabled;
    if (!enabled) { backend_log("weapon-hud disabled with user overrides"); return 1; }
    sh_weapon_hud_reload(root);
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(results[i].name, "WeaponHudModeCall")) site = &results[i];
        if (!strcmp(results[i].name, "WeaponHudGameMode")) predicate = &results[i];
    }
    if (!site || !predicate || site->status != SIG_OK || predicate->status != SIG_OK) goto refused;
    memcpy(expected, (void *)site->addr, sizeof expected);
    memcpy(&displacement, expected + 1, sizeof displacement);
    if (expected[0] != 0xe8 || site->addr + 5 + displacement != predicate->addr) goto refused;
    g_relay = hud_near_relay(site->addr, (void *)hud_mode);
    if (!g_relay) goto refused;
    distance = (intptr_t)g_relay - (intptr_t)(site->addr + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) goto free_relay;
    displacement = (int32_t)distance;
    memcpy(replacement + 1, &displacement, sizeof displacement);
    g_original = (hud_mode_fn)predicate->addr;
    if (code_patch_call_sig(site, expected, replacement, &g_patch) != B2_PATCH_OK) goto free_relay;
    g_hook_ready = 1;
    backend_log("weapon-hud installed: scoped ammo-display call, engine fallback for unlisted weapons");
    return 1;
free_relay:
    if (g_patch.live) {
        backend_log("weapon-hud rollback incomplete; relay retained for the outstanding patch");
        return 0;
    }
    VirtualFree(g_relay, 0, MEM_RELEASE); g_relay = NULL; g_original = NULL;
refused:
    backend_log("weapon-hud REFUSED: verified HUD call/predicate or relay unavailable; no hook installed");
    return 0;
}

#ifdef SH_WEAPON_HUD_TESTING
void sh_weapon_hud_test_reset(void) { hud_free(&g_rules); g_enabled = 1; }
size_t sh_weapon_hud_test_count(void) { return g_rules.count; }
int sh_weapon_hud_test_parse(const char *json, size_t length, const char *owner)
{
    sh_json_object rows = {0}, combined = {0}, incoming = {0};
    hud_table next = {0}; char *raw = NULL; int ok = 0;
    (void)owner;
    for (size_t i = 0; i < g_rules.count; i++)
        if (!sh_json_object_set(&rows, g_rules.rules[i].name, g_rules.rules[i].weapon_mode ?
            "{\"ammo_display\":\"weapon\"}" : "{\"ammo_display\":\"engine\"}", 2)) goto done;
    raw = sh_json_serialize_object(&rows, 0, NULL);
    if (!raw || !sh_json_object_set(&combined, "weapons", raw, 3) ||
        !sh_json_parse_object(json, length, 5, &incoming) ||
        !sh_package_hud_merge_provisional(&combined, &incoming, NULL, 0, NULL)) goto done;
    free(raw); raw = sh_json_serialize_object(&combined, 0, &length);
    if (!raw || !hud_parse(&next, raw, length)) goto done;
    hud_free(&g_rules); g_rules = next; memset(&next, 0, sizeof(next)); ok = 1;
done:
    free(raw); hud_free(&next); sh_json_object_free(&rows);
    sh_json_object_free(&combined); sh_json_object_free(&incoming); return ok;
}

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

int sh_weapon_hud_test_relay(const char *root)
{
    /* An owned synthetic caller uses precisely the normal Windows x64 ABI.
     * No game image or process is opened by this test. */
    unsigned char *code = (unsigned char *)VirtualAlloc(NULL, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char widget[0x208] = {0}, decl[16] = {0}, game = 1;
    const char *name = "weapon/test/unlimited";
    const unsigned char caller[] = {0x90,0x90,0x90,0x53,0x48,0x83,0xec,0x20,0x48,0x89,0xd3,
        0xe8,0x30,0,0,0,0x48,0x83,0xc4,0x20,0x5b,0xc3};
    const unsigned char predicate[] = {0x8a,0x01,0xc3};
    sig_result results[2] = {{"WeaponHudModeCall", SIG_OK, 0, 0},
                             {"WeaponHudGameMode", SIG_OK, 0, 0}};
    DWORD old;
    int ok = 0;
    hud_test_runner run = {0};
    HANDLE thread = NULL;
    if (!code) return 0;
    memcpy(code, caller, sizeof caller); memcpy(code + 64, predicate, sizeof predicate);
    memcpy(decl + 8, &name, sizeof name);
    void *d = decl; memcpy(widget + 0x1f8, &d, sizeof d);
    VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    unsigned char (*call)(void *, void *) = (unsigned char (*)(void *, void *))code;
    if (call(&game, widget) != 1) goto done;
    run.call = call;
    run.ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!run.ready) goto done;
    thread = CreateThread(NULL, 0, hud_test_run, &run, 0, NULL);
    if (!thread || WaitForSingleObject(run.ready, 2000) != WAIT_OBJECT_0) goto done;
    results[0].addr = (uintptr_t)code + 11;
    results[1].addr = (uintptr_t)code + 64;
    {
        sig_result unaligned = results[0];
        sh_patch_handle rejected;
        unaligned.addr++;
        if (code_patch_call_sig(&unaligned, caller + 11, caller + 11, &rejected) !=
            B2_PATCH_REFUSED_BADARG || memcmp(code, caller, sizeof caller)) goto done;
    }
    /* A mismatched decoded target must leave the synthetic caller untouched. */
    results[1].addr++;
    if (sh_weapon_hud_install(root, code, results, 2, 1) ||
        memcmp(code, caller, sizeof caller)) goto done;
    results[1].addr--;
#ifdef SH_PATCH_TESTING
    sh_patch_test_faults(2 | 8, 0, 0);
    if (sh_weapon_hud_install(root, code, results, 2, 1) ||
        !g_patch.live || !g_relay || !g_original || g_hook_ready) goto done;
    sh_patch_test_faults(0, 0, 0);
#endif
    if (!sh_weapon_hud_install(root, code, results, 2, 1)) goto done;
    {
        LONG initial = InterlockedCompareExchange(&run.iterations, 0, 0);
        ULONGLONG deadline = GetTickCount64() + 2000;
        while (InterlockedCompareExchange(&run.iterations, 0, 0) - initial < 100 &&
               GetTickCount64() < deadline) Sleep(1);
        if (InterlockedCompareExchange(&run.iterations, 0, 0) - initial < 100) goto done;
    }
    if (call(&game, widget) != 0 || game != 1) goto done;
    name = "weapon/test/other"; memcpy(decl + 8, &name, sizeof name);
    if (call(&game, widget) != 1) goto done;
    game = 0;
    if (call(&game, widget) != 0) goto done;
    if (call(&game, NULL) != 0) goto done;
    ok = 1;
done:
#ifdef SH_PATCH_TESTING
    sh_patch_test_faults(0, 0, 0);
#endif
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
