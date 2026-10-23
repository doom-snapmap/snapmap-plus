#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include "package_role.h"
#include "hook.h"
#include "backend_log.h"
#include "palette_refresh.h"

#pragma intrinsic(_ReturnAddress)

/* Native palette contract (identical in both renderers): body rows at
 * palette+0x10 with their count at +0x18, stride 0x6f0, all of which the
 * by-declaration lookup scans (its prologue, below); the by-name end pointer
 * at +0x28 may bound only a prefix of them. Row decl at +0, key holder at
 * +0x150 with its name at +0x58; no holder compares as "". */
static int role_native_view(void *context, void *palette, const unsigned char **entries, size_t *count, size_t *stride)
{
    const unsigned char *begin = NULL, *end = NULL; int rows = 0; (void)context;
    __try {
        begin = *(const unsigned char *const *)((const unsigned char *)palette + 0x10);
        rows = *(const int *)((const unsigned char *)palette + 0x18);
        end = *(const unsigned char *const *)((const unsigned char *)palette + 0x28);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!begin || rows < 0 || end < begin || (size_t)(end - begin) % 0x6f0 ||
        (size_t)(end - begin) / 0x6f0 > (size_t)rows) return 0;
    *entries = begin; *count = (size_t)rows; *stride = 0x6f0; return 1;
}
static const char *role_native_key(void *context, const void *entry)
{
    const char *key = NULL; (void)context;
    __try {
        const unsigned char *holder = *(const unsigned char *const *)((const unsigned char *)entry + 0x150);
        key = holder ? *(const char *const *)(holder + 0x58) : "";
    } __except(EXCEPTION_EXECUTE_HANDLER) { key = NULL; }
    return key;
}
static SRWLOCK g_lock = SRWLOCK_INIT;
static sh_package_role_api g_api = {role_native_view, role_native_key, NULL};
static sh_package_role_sites g_sites;
static const void *(*g_by_decl_original)(void *palette, const void *decl);
static uintptr_t (*g_scan_original)(const void *map, const void *decl);

void sh_package_role_bind(const sh_package_role_api *api)
{
    static const sh_package_role_api native = {role_native_view, role_native_key, NULL};
    AcquireSRWLockExclusive(&g_lock); g_api = api ? *api : native; ReleaseSRWLockExclusive(&g_lock);
}
static sh_package_role_api role_api(void)
{ AcquireSRWLockShared(&g_lock); sh_package_role_api api = g_api; ReleaseSRWLockShared(&g_lock); return api; }

/* The key of the first row holding decl, as the native by-declaration lookup
 * finds it; NULL when absent, unreadable or empty. A missing holder reads as ""
 * natively (half-built Play bodies have them), so a keyless row never shares a
 * role and keeps pointer identity. */
static const char *role_key(const sh_package_role_api *api, void *palette, const void *decl, const unsigned char **row)
{
    const unsigned char *entries = NULL; size_t count = 0, stride = 0;
    if (!palette || !decl || !api->view || !api->key ||
        !api->view(api->context, palette, &entries, &count, &stride) || !entries || stride < sizeof(void *)) return NULL;
    __try {
        for (size_t i = 0; i < count; i++)
            if (*(const void *const *)(entries + i*stride) == decl) {
                const char *key = api->key(api->context, entries + i*stride);
                if (!key || !*key) return NULL;
                if (row) *row = entries + i*stride;
                return key;
            }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return NULL;
}
int sh_package_role_equal(void *palette, const void *decl, const void *slot)
{
    sh_package_role_api api = role_api();
    const char *a, *b;
    if (decl == slot) return 1;
    if (!(a = role_key(&api, palette, decl, NULL)) || !(b = role_key(&api, palette, slot, NULL))) return 0;
    __try { return !strcmp(a, b); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* The palette is the editor singleton's, read at this call: never a pointer
 * kept from another lookup, so a temporary or retired palette cannot decide
 * presence. Each palette row sharing the slot's key is matched against the
 * map's entity rows by pointer; there is no bound on how many rows share it. */
int sh_package_role_contains(const void *map, const void *decl)
{
    sh_package_role_api api = role_api();
    void *palette = sh_palette_refresh_editor_palette();
    const unsigned char *entries = NULL; size_t count = 0, stride = 0;
    const char *slot;
    if (!map || !decl || !(slot = role_key(&api, palette, decl, NULL)) ||
        !api.view(api.context, palette, &entries, &count, &stride)) return 0;
    __try {
        const void *const *it = *(const void *const *const *)((const unsigned char *)map + 0xc0);
        int n = *(const int *)((const unsigned char *)map + 0xc8);
        if (!it || n <= 0) return 0;
        for (size_t i = 0; i < count; i++) {
            const unsigned char *entry = entries + i*stride; const void *row = *(const void *const *)entry;
            const char *key;
            if (row == decl || !(key = api.key(api.context, entry)) || strcmp(key, slot)) continue;
            for (int k = 0; k < n; k++)
                if (it[k] && *(const void *const *)((const unsigned char *)it[k] + 8) == row) return 1;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
const void *sh_package_role_canonical(void *palette, const void *decl, const void *found, const void *slot)
{
    sh_package_role_api api = role_api();
    const unsigned char *own = NULL;
    if (!slot || decl == slot || !sh_package_role_equal(palette, decl, slot)) return found;
    return role_key(&api, palette, slot, &own) && own ? own : found;
}

/* After `call by_decl` (E8 rel32): mov rsi|rbx,rax; cmp dword [rip+flag],0;
 * jne +5; call init; xor edx,edx; lea rcx,[rip+holder]; call getter;
 * mov rcx,[rax+148h]; cmp [rsi|rbx],rcx. */
static int role_site(const uint8_t *c, uintptr_t by_decl, uintptr_t *flag, uintptr_t *holder, uintptr_t *getter)
{
    static const uint8_t tail[] = {0x48,0x8b,0x88,0x48,0x01,0x00,0x00,0x48,0x39};
    uintptr_t at = (uintptr_t)c;
    if (c[0] != 0xe8 || at + 5 + *(const int32_t *)(c + 1) != by_decl) return 0;
    if (c[5] != 0x48 || c[6] != 0x8b || (c[7] != 0xf0 && c[7] != 0xd8)) return 0;
    if (c[8] != 0x83 || c[9] != 0x3d || c[14] != 0x00 || c[15] != 0x75 || c[16] != 0x05 || c[17] != 0xe8) return 0;
    if (c[22] != 0x33 || c[23] != 0xd2 || c[24] != 0x48 || c[25] != 0x8d || c[26] != 0x0d || c[31] != 0xe8) return 0;
    if (memcmp(c + 36, tail, sizeof(tail)) || c[45] != (c[7] == 0xf0 ? 0x0e : 0x0b)) return 0;
    *flag = at + 15 + *(const int32_t *)(c + 10);
    *holder = at + 31 + *(const int32_t *)(c + 27);
    *getter = at + 36 + *(const int32_t *)(c + 32);
    return 1;
}
int sh_package_role_find_sites(const uint8_t *base, uintptr_t by_decl, sh_package_role_sites *out)
{
    memset(out, 0, sizeof(*out));
    if (!base || !by_decl) return 0;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    size_t found = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t *p = base + sec[s].VirtualAddress;
        size_t size = sec[s].Misc.VirtualSize;
        for (size_t i = 0; i + 46 <= size; i++) {
            uintptr_t flag, holder, getter;
            if (p[i] != 0xe8 || !role_site(p + i, by_decl, &flag, &holder, &getter)) continue;
            if (found == SH_PACKAGE_ROLE_SITES || (found && (flag != out->flag || holder != out->holder || getter != out->getter)))
                { out->count = found + 1; return 0; }
            out->flag = flag; out->holder = holder; out->getter = getter;
            out->returns[found++] = (uintptr_t)(p + i + 5);
        }
    }
    out->count = found;
    return found == SH_PACKAGE_ROLE_SITES;
}

static const void *role_by_decl_at(void *palette, const void *decl, uintptr_t ret,
    const void *(*original)(void *, const void *))
{
    const void *found = original(palette, decl), *slot = NULL;
    size_t i;
    for (i = 0; i < g_sites.count && g_sites.returns[i] != ret; i++) {}
    if (i == g_sites.count || !found) return found;
    __try {
        if (*(const volatile int32_t *)g_sites.flag == 0) return found; /* bank not yet built: native path */
        const unsigned char *bank = ((const unsigned char *(*)(void *, uint64_t))g_sites.getter)((void *)g_sites.holder, 0);
        slot = bank ? *(const void *const *)(bank + 0x148) : NULL;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return found; }
    return sh_package_role_canonical(palette, decl, found, slot);
}
static const void *role_by_decl(void *palette, const void *decl)
{ return role_by_decl_at(palette, decl, (uintptr_t)_ReturnAddress(), g_by_decl_original); }
static uintptr_t role_scan(const void *map, const void *decl)
{
    uintptr_t found = g_scan_original(map, decl);
    if (found & 0xff) return found;
    return sh_package_role_contains(map, decl) ? (found & ~(uintptr_t)0xff) | 1 : found;
}

static uintptr_t role_target(const sig_result *results, size_t count, const uint8_t *base, const char *name,
    const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < count; i++)
        if (results[i].name && !strcmp(results[i].name, name)) {
            if (results[i].status != SIG_OK || results[i].addr != (uintptr_t)base + results[i].rva) return 0;
            __try { if (memcmp((const void *)results[i].addr, bytes, size)) return 0; }
            __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
            return results[i].addr;
        }
    return 0;
}
static int role_hook(uintptr_t target, void *detour, size_t stolen, void **original)
{
    *original = hook_prepare((void *)target, detour, stolen);
    if (!*original) return 0;
    if (hook_commit(*original) == B2_PATCH_OK) return 1;
    if (hook_unpatch(*original)) *original = NULL;
    return 0;
}
int sh_package_role_install(const sig_result *results, size_t count, const uint8_t *base)
{
    /* Whole position-independent prologues (14 and 15 stolen bytes). */
    static const uint8_t scan[] = {0x48,0x8b,0x81,0xc0,0x00,0x00,0x00,0x48,0x63,0x89,0xc8,0x00,0x00,0x00};
    static const uint8_t by_decl[] = {0x48,0x8b,0x41,0x10,0x48,0x63,0x49,0x18,0x4c,0x69,0xc1,0xf0,0x06,0x00,0x00};
    uintptr_t scan_at = role_target(results, count, base, "SnapMapHasEntityDecl", scan, sizeof(scan));
    uintptr_t decl_at = role_target(results, count, base, "SnapPaletteFindDecl", by_decl, sizeof(by_decl));
    sh_package_role_sites sites = {0};
    int installed = 0; char line[256];
    if (scan_at && !g_scan_original && role_hook(scan_at, role_scan, sizeof(scan), (void **)&g_scan_original)) installed++;
    else if (!g_scan_original) backend_log("MPKG: role identity scan unavailable; player start keeps pointer identity");
    if (decl_at && !g_by_decl_original && sh_package_role_find_sites(base, decl_at, &sites)) {
        g_sites = sites;
        if (role_hook(decl_at, role_by_decl, sizeof(by_decl), (void **)&g_by_decl_original)) installed++;
        else memset(&g_sites, 0, sizeof(g_sites));
    }
    if (!g_by_decl_original) {
        snprintf(line, sizeof(line), "MPKG: role default marker unavailable (by_decl=%d sites=%zu); noWeapon keeps pointer identity",
            decl_at != 0, sites.count);
        backend_log(line);
    }
    snprintf(line, sizeof(line), "MPKG: role identity installed scan=%d marker=%d sites=%zu",
        g_scan_original != NULL, g_by_decl_original != NULL, g_sites.count);
    backend_log(line);
    return installed;
}
#ifdef SH_PACKAGE_ROLE_TESTING
void sh_package_role_test_sites(const sh_package_role_sites *sites, const void *(*by_decl)(void *, const void *))
{ if (sites) g_sites = *sites; else memset(&g_sites, 0, sizeof(g_sites)); g_by_decl_original = by_decl; }
const void *sh_package_role_test_by_decl(void *palette, const void *decl, uintptr_t return_address)
{ return role_by_decl_at(palette, decl, return_address, g_by_decl_original); }
#endif
