/* script_combat_sigs_test.c -- the engine sites of the mod-script combat
 * calls and of the damage, pain and projectile hooks
 * (src/backend/script/script_combat_sigs.c, script_api_combat.c,
 * script_hooks.c), checked on each build 13954591 executable given on the
 * command line:
 *   - every private signature resolves to one match at its recorded RVA, and
 *     each detour window is whole, position-independent instructions;
 *   - the call graph the hooks rely on: idAI2::Damage calls Damage_Apply, which
 *     calls the OnDamage dispatcher; Event_LaunchProjectileAtTarget calls
 *     idProjectile::Launch; the ae_radiusDamage handler calls RadiusDamage;
 *     SetPainReaction writes ai +0x5060;
 *   - idProjectile's vtable (by its RTTI name) holds Explode at +0xAA0;
 *   - the painType_t, soundChannel_t and fxCondition_t tables are found by
 *     sh_script_combat_enum_find with the names and values the calls use.
 * Run via run-tests.ps1 with -Doom / -DoomAlt; each image in its own process. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "script/script_api_combat.h"
#include "script/script_fsm.h"

extern const sig_entry SH_SCRIPT_FSM_SIGNATURE;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static uint8_t *map_pe_by_rva(const char *path, uint32_t *image_size)
{
    FILE *f = fopen(path, "rb");
    long fsz;
    uint8_t *file, *img;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int i;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    *image_size = nt->OptionalHeader.SizeOfImage;
    img = (uint8_t *)calloc(1, nt->OptionalHeader.SizeOfImage);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz) memcpy(img + va, file + po, rs);
    }
    free(file);
    return img;
}

static uint64_t qword_at(const uint8_t *base, uint32_t rva)
{
    uint64_t v;
    memcpy(&v, base + rva, 8);
    return v;
}

/* RVA of the E8 call to `callee` inside [from, from + len), or 0. */
static uint32_t call_in(const uint8_t *base, uint32_t from, uint32_t len, uint32_t callee)
{
    uint32_t i;
    for (i = from; i + 5 <= from + len; i++) {
        int32_t rel;
        if (base[i] != 0xE8) continue;
        memcpy(&rel, base + i + 1, 4);
        if ((uint32_t)(i + 5 + rel) == callee) return i;
    }
    return 0;
}

/* The vtable whose RTTI type name is `name`, or 0. */
static uint32_t vtable_of(const uint8_t *base, uint32_t size, uint64_t image_base, const char *name)
{
    size_t n = strlen(name);
    uint32_t td = 0, col = 0, i;
    for (i = 0x10; i + n + 1 < size; i++)
        if (!memcmp(base + i, name, n + 1) && base[i - 1] == 0) { td = i - 0x10; break; }
    if (!td) return 0;
    for (i = 0; i + 0x18 < size; i += 4) {
        uint32_t sig, off, t;
        memcpy(&sig, base + i, 4);
        memcpy(&off, base + i + 4, 4);
        memcpy(&t, base + i + 12, 4);
        if (sig == 1 && off == 0 && t == td) { col = i; break; }
    }
    if (!col) return 0;
    for (i = 0; i + 16 < size; i += 8)
        if (qword_at(base, i) == image_base + col) return i + 8;
    return 0;
}

static const char *name_at(const uint8_t *base, uint64_t image_base, uint32_t row)
{
    return (const char *)base + (qword_at(base, row) - image_base);
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, rva[SH_COMBAT_SIG_COUNT];
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    IMAGE_NT_HEADERS *nt;
    uint64_t image_base;
    int vk, i, n;
    static const size_t windows[] = { SH_COMBAT_DAMAGE_APPLY_STOLEN, SH_COMBAT_SET_PAIN_STOLEN, SH_COMBAT_LAUNCH_STOLEN };
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    vk = identity->vulkan;
    printf("image %s: %s\n", path, identity->label);
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    image_base = nt->OptionalHeader.ImageBase;

    for (n = 0; SH_COMBAT_SIGNATURES[n].name; n++) {
        const sig_entry *e = &SH_COMBAT_SIGNATURES[n];
        sig_result r;
        rva[n] = 0;
        if (sig_resolve_one(base, e, &r) != SIG_OK || !r.addr) {
            printf("FAIL %s did not resolve to a unique match\n", e->name);
            g_fail = 1;
            continue;
        }
        CHECK(r.rva == (vk ? e->rva_vk : e->rva_gl));
        rva[n] = r.rva;
        printf("  %-22s rva=0x%X\n", e->name, r.rva);
    }
    CHECK(n == SH_COMBAT_SIG_COUNT);

    /* detour windows */
    for (i = 0; i < 3; i++) {
        unsigned flags = 0;
        if (!rva[i]) continue;
        CHECK(sh_x64_stolen_boundary(base + rva[i], 64, 14, 48, &flags) == windows[i]);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
    }

    /* the type lookup the state-machine class bridge detours */
    {
        sig_result r;
        unsigned flags = 0;
        if (sig_resolve_one(base, &SH_SCRIPT_FSM_SIGNATURE, &r) != SIG_OK || !r.addr) {
            printf("FAIL %s did not resolve to a unique match\n", SH_SCRIPT_FSM_SIGNATURE.name);
            g_fail = 1;
        } else {
            CHECK(r.rva == (vk ? SH_SCRIPT_FSM_SIGNATURE.rva_vk : SH_SCRIPT_FSM_SIGNATURE.rva_gl));
            CHECK(sh_x64_stolen_boundary(base + r.rva, 64, 14, 48, &flags) == SH_FSM_LOOKUP_STOLEN);
            CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
            printf("  %-22s rva=0x%X\n", SH_SCRIPT_FSM_SIGNATURE.name, r.rva);
        }
    }

    /* the call graph */
    {
        uint32_t damage = vk ? 0x3F8C40u : 0x3F8120u, on_damage = vk ? 0x8283A0u : 0x827560u;
        uint32_t launch_at = vk ? 0x76E780u : 0x76DFD0u, radius_handler = vk ? 0x3EBF60u : 0x3EB430u;
        if (rva[SH_COMBAT_SIG_DAMAGE_APPLY]) {
            CHECK(call_in(base, damage, 1180, rva[SH_COMBAT_SIG_DAMAGE_APPLY]) != 0);
            CHECK(call_in(base, rva[SH_COMBAT_SIG_DAMAGE_APPLY], 0x1600, on_damage) != 0);
        }
        if (rva[SH_COMBAT_SIG_PROJECTILE_LAUNCH])
            CHECK(call_in(base, launch_at, 0x400, rva[SH_COMBAT_SIG_PROJECTILE_LAUNCH]) == launch_at + 0x32C);
        if (rva[SH_COMBAT_SIG_RADIUS_DAMAGE])
            CHECK(call_in(base, radius_handler, 0x200, rva[SH_COMBAT_SIG_RADIUS_DAMAGE]) != 0);
        /* idEntity StartFX / StopFX: each loads the entity's FX manager (lea rcx,[rbx+0x30]) and calls its start
         * (stop) by condition; StopFX passes "not immediate" (mov byte [rsp+0x20],0) */
        if (rva[SH_COMBAT_SIG_ENT_START_FX]) {
            uint32_t mgr_start = vk ? 0x16EED40u : 0x16EF6C0u;
            CHECK(!memcmp(base + rva[SH_COMBAT_SIG_ENT_START_FX] + 0x3F, "\x48\x8D\x4B\x30", 4));
            CHECK(call_in(base, rva[SH_COMBAT_SIG_ENT_START_FX], 0x68, mgr_start) != 0);
        }
        if (rva[SH_COMBAT_SIG_ENT_STOP_FX]) {
            uint32_t mgr_stop = vk ? 0x16EFE60u : 0x16F07E0u;
            CHECK(!memcmp(base + rva[SH_COMBAT_SIG_ENT_STOP_FX] + 0x32, "\x48\x8D\x4B\x30\xC6\x44\x24\x20\x00", 9));
            CHECK(call_in(base, rva[SH_COMBAT_SIG_ENT_STOP_FX], 0x58, mgr_stop) != 0);
        }
        /* SetPainReaction: mov [rdi+0x5060], esi */
        if (rva[SH_COMBAT_SIG_SET_PAIN])
            CHECK(!memcmp(base + rva[SH_COMBAT_SIG_SET_PAIN] + 0x126, "\x89\xB7\x60\x50\x00\x00", 6));
    }

    /* idProjectile::Explode at vtable +0xAA0 */
    {
        uint32_t vt = vtable_of(base, size, image_base, ".?AVidProjectile@@");
        uint32_t explode = vk ? 0xF38240u : 0xF37700u;
        CHECK(vt == (vk ? 0x22DFFB0u : 0x22F9D00u));
        if (vt) CHECK(qword_at(base, vt + SH_COMBAT_PROJ_EXPLODE) == image_base + explode);
    }

    /* enum tables */
    {
        static const char *const pain[] = {
            "PAIN_NONE", "PAIN_TWITCH", "PAIN_TWITCH_HEAVY", "PAIN_FALTER_LIGHT", "PAIN_INTERRUPT", "PAIN_STUN_LOOP",
            "PAIN_FALTER", "PAIN_PUSHBACK", "PAIN_STUN", "PAIN_STAGGER", "PAIN_STAGGER_VULNERABLE", "PAIN_KNOCKDOWN",
            "PAIN_DEATH", "PAIN_FULL_GIB", "PAIN_MAX"
        };
        static const char *const channels[] = {
            "SND_CHANNEL_ANY", "SND_CHANNEL_VOICE", "SND_CHANNEL_VOICE2", "SND_CHANNEL_BODY", "SND_CHANNEL_BODY2",
            "SND_CHANNEL_BODY3", "SND_CHANNEL_BODY_LOOP", "SND_CHANNEL_WEAPON", "SND_CHANNEL_WEAPON_ALERT",
            "SND_CHANNEL_WEAPON_AMBIENT", "SND_CHANNEL_WEAPON_AMBIENT2", "SND_CHANNEL_WEAPON_LOOP",
            "SND_CHANNEL_WEAPON_CHARGE_LOOP", "SND_CHANNEL_SPECIAL_FIRE_LOOP", "SND_CHANNEL_ITEM", "SND_CHANNEL_ITEM2"
        };
        int count = 0, k, found_cinematic = 0;
        uint32_t t = sh_script_combat_enum_find(base, 0, "PAIN_NONE", "PAIN_TWITCH", "PAIN_", &count);
        CHECK(t == (vk ? 0x34D4C40u : 0x351CAE0u));
        CHECK(count == 15);
        for (k = 0; t && k < 15; k++) {
            char v[8];
            CHECK(!strcmp(name_at(base, image_base, t + k * 0x18), pain[k]));
            sprintf_s(v, sizeof v, "%d", k);
            CHECK(!strcmp(name_at(base, image_base, t + k * 0x18 + 8), v));
            if (k < SH_COMBAT_PAIN_COUNT) CHECK(!strcmp(sh_script_combat_pain_name(k), pain[k]));
        }
        t = sh_script_combat_enum_find(base, 0, "SND_CHANNEL_ANY", "SND_CHANNEL_VOICE", "SND_CHANNEL_", &count);
        CHECK(t != 0 && count >= 16);
        for (k = 0; t && k < 16; k++) {
            char v[8];
            CHECK(!strcmp(name_at(base, image_base, t + k * 0x18), channels[k]));
            sprintf_s(v, sizeof v, "%d", k);
            CHECK(!strcmp(name_at(base, image_base, t + k * 0x18 + 8), v));
            CHECK(sh_script_combat_channel(channels[k] + 12) == k);   /* the name after SND_CHANNEL_ */
        }
        t = sh_script_combat_enum_find(base, 0, "FX_NONE", "FX_EXPLOSIVE_ARMED", "FX_", &count);
        CHECK(t != 0 && count > 500);
        for (k = 0; t && k < count; k++) {
            if (k && !strcmp(name_at(base, image_base, t + k * 0x18 + 8), "0")) break;
            if (!strcmp(name_at(base, image_base, t + k * 0x18), "FX_CINEMATIC_8")) found_cinematic = 1;
        }
        printf("  fxCondition_t: %d conditions\n", k);
        CHECK(k == 577 && found_cinematic);
    }
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("script_combat_sigs_test: no images given; skipped\n"); return 0; }
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "script_combat_sigs_test: FAIL\n" : "script_combat_sigs_test: PASS\n");
    return g_fail;
}
