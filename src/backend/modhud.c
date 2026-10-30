/* modhud.c -- see modhud.h. The beta.19 weapon_hud policy and call-site relay,
 * fed from admitted bundles instead of compiled packages. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modhud.h"
#include "config_json.h"
#ifndef SH_MODHUD_PURE
#include "backend_log.h"
#include "patch.h"
#ifndef SH_MODHUD_TESTING
#include "modserve.h"
#endif
#endif

#define HUD_JSON_DEPTH   24
#define HUD_BUNDLE_CAP   (1u << 20)        /* bundle.json, as modserve caps it */
#define HUD_OWNER_CAP    80

typedef struct hud_rule {
    char name[SH_MODHUD_NAME_CAP];
    char owner[HUD_OWNER_CAP];
    int weapon_mode;
} hud_rule;
typedef struct hud_table { size_t count; hud_rule rules[SH_MODHUD_MAX_RULES]; int refused; } hud_table;

static hud_table g_live, g_pending;
static SRWLOCK g_lock = SRWLOCK_INIT;

#ifdef SH_MODHUD_PURE
#define hud_log(...) ((void)0)
#else
static void hud_log(const char *fmt, ...)
{
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}
#endif

/* ------------------------------------------------------------------ policy */

static int hud_name_valid(const char *s)
{
    const char *part = s;
    size_t n = strlen(s);
    if (!n || n >= SH_MODHUD_NAME_CAP || s[0] == '/' || s[n - 1] == '/') return 0;
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

void sh_modhud_begin(void) { memset(&g_pending, 0, sizeof g_pending); }

int sh_modhud_parse(const char *json, size_t length, const char *owner)
{
    sh_json_object root = {0}, weapons = {0};
    const char *raw;
    char schema[64];
    int ok = 0;
    size_t keys = 1;
    if (g_pending.refused) return 0;
    if (!json || length > SH_MODHUD_FILE_CAP || !sh_json_parse_object(json, length, 5, &root)) goto done;
    if (!root.count) { ok = 1; goto done; }
    /* The beta.13 file form also named its schema. */
    raw = sh_json_object_get(&root, "schema");
    if (raw) {
        if (!hud_string(raw, schema, sizeof schema) || strcmp(schema, "snapmap-plus.weapon-hud.v1")) goto done;
        keys = 2;
    }
    if (root.count != keys) goto done;
    raw = sh_json_object_get(&root, "weapons");
    if (!raw || !sh_json_parse_object(raw, strlen(raw), 3, &weapons)) goto done;
    for (size_t i = 0; i < weapons.count; i++) {
        sh_json_member *m = &weapons.members[i];
        sh_json_object settings = {0};
        char mode[24];
        int weapon_mode, valid;
        size_t j;
        if (strlen(m->key) != m->key_length || !hud_name_valid(m->key) ||
            !sh_json_parse_object(m->value_json, strlen(m->value_json), 2, &settings)) goto done;
        valid = settings.count == 1 && hud_string(sh_json_object_get(&settings, "ammo_display"), mode, sizeof mode);
        sh_json_object_free(&settings);
        if (!valid || (strcmp(mode, "weapon") && strcmp(mode, "engine"))) goto done;
        weapon_mode = strcmp(mode, "weapon") == 0;
        for (j = 0; j < g_pending.count; j++) {
            if (strcmp(g_pending.rules[j].name, m->key)) continue;
            if (g_pending.rules[j].weapon_mode != weapon_mode) {
                hud_log("weapon-hud REFUSED: conflicting ammo_display for %s in bundles %s and %s",
                        m->key, g_pending.rules[j].owner, owner ? owner : "?");
                goto done;
            }
            break;
        }
        if (j < g_pending.count) continue;
        if (g_pending.count == SH_MODHUD_MAX_RULES) goto done;
        {
            hud_rule *r = &g_pending.rules[g_pending.count++];
            strcpy_s(r->name, sizeof r->name, m->key);
            strncpy_s(r->owner, sizeof r->owner, owner ? owner : "?", _TRUNCATE);
            r->weapon_mode = weapon_mode;
        }
    }
    ok = 1;
done:
    sh_json_object_free(&weapons);
    sh_json_object_free(&root);
    if (!ok) g_pending.refused = 1;
    return ok;
}

void sh_modhud_publish(int ok)
{
    AcquireSRWLockExclusive(&g_lock);
    if (ok && !g_pending.refused) g_live = g_pending;
    else memset(&g_live, 0, sizeof g_live);
    ReleaseSRWLockExclusive(&g_lock);
}

size_t sh_modhud_count(void)
{
    size_t n;
    AcquireSRWLockShared(&g_lock);
    n = g_live.count;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

unsigned char sh_modhud_select(const char *weapon, unsigned char engine_mode)
{
    unsigned char result = engine_mode;
    if (!weapon) return result;
    AcquireSRWLockShared(&g_lock);
    for (size_t i = 0; i < g_live.count; i++) {
        if (!strcmp(weapon, g_live.rules[i].name)) {
            if (g_live.rules[i].weapon_mode) result = 0;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return result;
}

/* ------------------------------------------------------------------ bundles */

static char *hud_read(const char *path, size_t cap, size_t *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    char *buf = NULL;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && (unsigned long long)size.QuadPart <= cap &&
        (buf = (char *)malloc((size_t)size.QuadPart + 1)) != NULL &&
        ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart) {
        buf[got] = 0;
        *len = got;
    } else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(h);
    return buf;
}

/* One bundle folder: 1 parsed (or no hud), 0 refused. Unadmitted bundles and
 * unreadable manifests are skipped (the index logs those). */
static int hud_bundle(const char *dir, int (*admitted)(const char *))
{
    char path[MAX_PATH], hash[80], id[HUD_OWNER_CAP];
    size_t len = 0;
    char *text;
    sh_json_object root = {0}, extra = {0};
    const char *raw;
    int ok = 1;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.json", dir);
    text = hud_read(path, HUD_BUNDLE_CAP, &len);
    if (!text) return 1;
    if (!sh_json_parse_object(text, len, HUD_JSON_DEPTH, &root)) goto done;
    if (!hud_string(sh_json_object_get(&root, "bundle"), hash, sizeof hash) || !admitted || !admitted(hash)) goto done;
    raw = sh_json_object_get(&root, "extra");
    if (!raw || *raw != '{' || !sh_json_parse_object(raw, strlen(raw), HUD_JSON_DEPTH, &extra)) goto done;
    raw = sh_json_object_get(&extra, "hud");
    if (!raw) goto done;
    if (!hud_string(sh_json_object_get(&root, "id"), id, sizeof id)) strcpy_s(id, sizeof id, "?");
    ok = sh_modhud_parse(raw, strlen(raw), id);
    if (!ok) hud_log("weapon-hud REFUSED: the hud object of %s is not a valid weapon ammo-display table", id);
done:
    sh_json_object_free(&extra);
    sh_json_object_free(&root);
    free(text);
    return ok;
}

static int hud_scan(const char *folder, int (*admitted)(const char *))
{
    char pattern[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int ok = 1;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", folder);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            fd.cFileName[0] == '.' || !strchr(fd.cFileName, '@')) continue;
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", folder, fd.cFileName);
        if (!hud_bundle(sub, admitted)) ok = 0;
    } while (ok && FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

int sh_modhud_load_tree(const char *mods_root, int (*admitted)(const char *bundle_hash))
{
    char legacy[MAX_PATH];
    int ok;
    sh_modhud_begin();
    ok = mods_root && hud_scan(mods_root, admitted);
    if (ok) {
        _snprintf_s(legacy, sizeof legacy, _TRUNCATE, "%s\\legacy", mods_root);
        ok = hud_scan(legacy, admitted);
    }
    sh_modhud_publish(ok);
    hud_log("weapon-hud %s: %zu rules", ok ? "compiled" : "REFUSED", sh_modhud_count());
    return ok ? (int)sh_modhud_count() : -1;
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
    char name[SH_MODHUD_NAME_CAP];
    __try {
        const unsigned char *decl = widget ? *(const unsigned char *const *)(widget + 0x1f8) : NULL;
        const char *source = decl ? *(const char *const *)(decl + 8) : NULL;
        if (source) {
            size_t i;
            for (i = 0; i < sizeof name; i++) { name[i] = source[i]; if (!name[i]) break; }
            if (i < sizeof name) result = sh_modhud_select(name, original);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = original; }
    if (result != original && InterlockedCompareExchange(&g_reported, 1, 0) == 0)
        hud_log("weapon-hud active: package weapon-capacity display selected");
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
static int hud_admitted(const char *hash) { return sh_modserve_bundle_state(hash, NULL, 0) == 1; }

static int hud_root(char *out, size_t cap)
{
    char base[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus\\mods", base);
    return 1;
}

int sh_modhud_reload(void)
{
    char root[MAX_PATH];
    if (!hud_root(root, sizeof root)) { sh_modhud_publish(0); return -1; }
    return sh_modhud_load_tree(root, hud_admitted);
}

int sh_modhud_install(const sig_result *results, size_t count)
{
    (void)sh_modhud_reload();
    return hud_route(results, count);
}
#else
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
