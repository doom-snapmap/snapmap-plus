/* weapon_hud.c -- bounded declarative HUD policies and one scoped call-site hook. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "weapon_hud.h"
#include "packages.h"
#include "config_json.h"
#include "backend_log.h"
#include "patch.h"

#define HUD_MAX_RULES 256
#define HUD_NAME_CAP 192
#define HUD_FILE_CAP (64u * 1024u)
#define HUD_TOTAL_CAP (256u * 1024u)

typedef struct hud_rule {
    char name[HUD_NAME_CAP];
    char owner[SH_PACKAGE_NAME_CAP];
    int weapon_mode;
} hud_rule;
typedef struct hud_table { size_t count; hud_rule rules[HUD_MAX_RULES]; } hud_table;
static hud_table g_rules;
static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_enabled;
static void *g_relay;
static sh_patch_handle g_patch;
typedef unsigned char (*hud_mode_fn)(void *game);
static hud_mode_fn g_original;
static volatile LONG g_reported;

static int hud_name_valid(const char *s)
{
    const char *part = s;
    size_t n = strlen(s);
    if (!n || n >= HUD_NAME_CAP || s[0] == '/' || s[n-1] == '/') return 0;
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
    return raw && sh_json_decode_string(raw, strlen(raw), out, cap, &length) &&
           strlen(out) == length;
}

static int hud_parse(hud_table *table, const char *json, size_t length, const char *owner)
{
    sh_json_object root = {0}, weapons = {0};
    char schema[64], message[512];
    const char *raw;
    int ok = 0;
    if (length > HUD_FILE_CAP || !sh_json_parse_object(json, length, 5, &root)) goto done;
    if (root.count != 2 ||
        !hud_string(sh_json_object_get(&root, "schema"), schema, sizeof schema) ||
        strcmp(schema, "snapmap-plus.weapon-hud.v1") != 0) goto done;
    raw = sh_json_object_get(&root, "weapons");
    if (!raw || !sh_json_parse_object(raw, strlen(raw), 3, &weapons)) goto done;
    for (size_t i = 0; i < weapons.count; i++) {
        sh_json_member *m = &weapons.members[i];
        sh_json_object settings = {0};
        char mode[24];
        int weapon_mode;
        size_t j;
        if (strlen(m->key) != m->key_length || !hud_name_valid(m->key) ||
            !sh_json_parse_object(m->value_json, strlen(m->value_json), 2, &settings)) goto done;
        int valid = settings.count == 1 &&
            hud_string(sh_json_object_get(&settings, "ammo_display"), mode, sizeof mode);
        sh_json_object_free(&settings);
        if (!valid || (strcmp(mode, "weapon") && strcmp(mode, "engine"))) goto done;
        weapon_mode = strcmp(mode, "weapon") == 0;
        for (j = 0; j < table->count; j++) {
            if (strcmp(table->rules[j].name, m->key)) continue;
            if (table->rules[j].weapon_mode != weapon_mode) {
                _snprintf_s(message, sizeof message, _TRUNCATE,
                    "weapon-hud REFUSED: conflicting ammo_display for %s in packages %s and %s",
                    m->key, table->rules[j].owner, owner);
                backend_log(message);
                goto done;
            }
            break;
        }
        if (j < table->count) continue;
        if (table->count == HUD_MAX_RULES) goto done;
        hud_rule *r = &table->rules[table->count++];
        strcpy_s(r->name, sizeof r->name, m->key);
        strncpy_s(r->owner, sizeof r->owner, owner, _TRUNCATE);
        r->weapon_mode = weapon_mode;
    }
    ok = 1;
done:
    sh_json_object_free(&weapons);
    sh_json_object_free(&root);
    return ok;
}

static char *hud_read(const char *path, size_t *length)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    DWORD got = 0;
    char *body = NULL;
    *length = 0;
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > HUD_FILE_CAP) goto done;
    body = (char *)malloc((size_t)size.QuadPart + 1);
    if (!body) goto done;
    if (!ReadFile(file, body, (DWORD)size.QuadPart, &got, NULL) || got != size.QuadPart) {
        free(body); body = NULL; goto done;
    }
    *length = got;
    body[got] = 0;
done:
    CloseHandle(file);
    return body;
}

int sh_weapon_hud_reload(const char *data_root)
{
    sh_package *packages = (sh_package *)calloc(SH_PACKAGES_MAX, sizeof(sh_package));
    hud_table *next = (hud_table *)calloc(1, sizeof(hud_table));
    size_t count = 0, total = 0;
    char path[MAX_PATH] = "", directory[MAX_PATH], message[512];
    int ok = 0;
    if (!g_enabled) { free(next); free(packages); return 1; }
    if (!next || !packages || !data_root ||
        !sh_packages_enumerate(data_root, packages, SH_PACKAGES_MAX, &count)) goto done;
    for (size_t i = 0; i < count; i++) {
        DWORD attr, error;
        size_t length = 0;
        char *body;
        if (!sh_package_subdir(&packages[i], "hud", directory, sizeof directory)) goto done;
        attr = GetFileAttributesA(directory);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            goto done;
        }
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) goto done;
        if (_snprintf_s(path, sizeof path, _TRUNCATE, "%s\\weapons.json", directory) < 0) goto done;
        attr = GetFileAttributesA(path);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
            goto done;
        }
        body = hud_read(path, &length);
        if (!body) goto done;
        int parsed = total <= HUD_TOTAL_CAP - length &&
            hud_parse(next, body, length, packages[i].name);
        free(body);
        if (!parsed) goto done;
        total += length;
    }
    ok = 1;
done:
    AcquireSRWLockExclusive(&g_lock);
    if (ok) g_rules = *next;
    else memset(&g_rules, 0, sizeof g_rules);
    InterlockedExchange(&g_reported, 0);
    if (ok) _snprintf_s(message, sizeof message, _TRUNCATE,
        "weapon-hud captured: %zu rule(s)", g_rules.count);
    else _snprintf_s(message, sizeof message, _TRUNCATE,
        "weapon-hud REFUSED: invalid, conflicting or unreadable policy near %s; engine display retained", path);
    ReleaseSRWLockExclusive(&g_lock);
    backend_log(message);
    free(next); free(packages);
    return ok;
}

unsigned char sh_weapon_hud_select(const char *weapon, unsigned char engine_mode)
{
    unsigned char result = engine_mode;
    if (!weapon) return result;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_rules.count; i++) {
        if (!strcmp(weapon, g_rules.rules[i].name)) {
            if (g_rules.rules[i].weapon_mode) result = 0;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return result;
}

/* The call-site relay passes the widget in RDX; RCX is the original game
 * argument. No widget writes, global predicate detour or TLS context is needed. */
static unsigned char hud_mode(void *game, const unsigned char *widget)
{
    unsigned char original = g_original(game), result = original;
    char name[HUD_NAME_CAP];
    __try {
        const unsigned char *decl = *(const unsigned char *const *)(widget + 0x1f8);
        const char *source = decl ? *(const char *const *)(decl + 8) : NULL;
        if (source) {
            size_t i;
            for (i = 0; i < sizeof name; i++) { name[i] = source[i]; if (!name[i]) break; }
            if (i < sizeof name) result = sh_weapon_hud_select(name, original);
        }
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
    if (g_patch.live) return 1;
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
    backend_log("weapon-hud installed: scoped ammo-display call, engine fallback for unlisted weapons");
    return 1;
free_relay:
    VirtualFree(g_relay, 0, MEM_RELEASE); g_relay = NULL; g_original = NULL;
refused:
    backend_log("weapon-hud REFUSED: verified HUD call/predicate or relay unavailable; no hook installed");
    return 0;
}

#ifdef SH_WEAPON_HUD_TESTING
void sh_weapon_hud_test_reset(void) { memset(&g_rules, 0, sizeof g_rules); g_enabled = 1; }
size_t sh_weapon_hud_test_count(void) { return g_rules.count; }
int sh_weapon_hud_test_parse(const char *json, size_t length, const char *owner)
{
    hud_table *next = (hud_table *)malloc(sizeof *next);
    if (!next) return 0;
    *next = g_rules;
    int ok = hud_parse(next, json, length, owner);
    if (ok) g_rules = *next;
    else memset(&g_rules, 0, sizeof g_rules);
    free(next);
    return ok;
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
    if (g_patch.live && code_unpatch(&g_patch) != B2_PATCH_OK) ok = 0;
    InterlockedExchange(&run.stop, 1);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        if (run.failed || !run.iterations) ok = 0;
    }
    if (run.ready) CloseHandle(run.ready);
    if (g_relay) VirtualFree(g_relay, 0, MEM_RELEASE);
    g_relay = NULL; g_original = NULL;
    if (memcmp(code, caller, sizeof caller)) ok = 0;
    VirtualFree(code, 0, MEM_RELEASE);
    return ok;
}
#endif
