/* script_ai_sigs_test.c -- the engine sites the AI calls of mod scripts
 * depend on (src/backend/script/script_ai_sigs.c, script_api_ai.c), checked
 * on each build 13954591 executable given on the command line:
 *   - every private signature resolves to one match at its recorded RVA;
 *   - IsRunningScript's detour window is whole, position-independent
 *     instructions and is the bytes the installer compares;
 *   - the path site's calls and load reach the path key, the path query and
 *     the query's first word at the recorded addresses;
 *   - idAI2's GetType (vtable slot 0) returns the typeinfo the demon check
 *     reads;
 *   - the move FSM status (Shared_MoveFailedAny) and the weapon trigger
 *     (Shared_TriggerPulled) are read at the offsets the calls use;
 *   - the calls the Tyrant's brain needs: the recoil play reads ai+0x24EB0;
 *     the next-pull writer's fire-decl call, slot table and clock slot; the
 *     aim focus call; the pain request's two loads (the stub's window), its
 *     atomic-string set and its later compare of each anim's +0x48 string
 *     with [rbp+0x88]; the threat row and path length detour windows; the
 *     animation and root-motion functions; the monster type names against
 *     the executable's own enum table;
 *   - the fire calls: the muzzle test reads the focus fields the calls read
 *     (+0xEE9B, +0xE020, +0x11AE4, +0x11AE8), ClearMove takes the animFSM,
 *     the fire control's think reads its two fire modes at +0x1C and +0x20,
 *     the idle action stores idleTurnType at animFSM +0xBF0 after reading
 *     the behaviour decl's turnParms (+0xC120, +0x528, +0x530), the fire gate
 *     asks requireMuzzleOnTarget at +0x27, and the fire mode and idle turn
 *     names against the executable's own enum tables;
 *   - wound damage counts: DamageActivatesWound's window, its unique
 *     signature, the registers its arguments land in, the threshold helper
 *     it calls with the wound and the max health (+0x214, +0x210), the one
 *     read of the lost health, and the frame damage queue's record layout.
 * Run via run-tests.ps1 with -Doom / -DoomAlt. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "script/script_api_ai.h"

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
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz)
            memcpy(img + va, file + po, rs);
    }
    free(file);
    return img;
}

static uint32_t rel_target(const uint8_t *base, uint32_t at, uint32_t len, uint32_t disp_off)
{
    int32_t d;
    memcpy(&d, base + at + disp_off, 4);
    return (uint32_t)(at + len + d);
}

static uint64_t qword_at(const uint8_t *base, uint32_t rva)
{
    uint64_t v;
    memcpy(&v, base + rva, 8);
    return v;
}

static int window_has(const uint8_t *base, uint32_t from, uint32_t len, const char *bytes, size_t n)
{
    uint32_t k;
    for (k = from; k + n <= from + len; k++) if (!memcmp(base + k, bytes, n)) return 1;
    return 0;
}

/* An enum table of the executable: {name pointer, value qword} entries 0x10 apart, the first
 * named names[0] with value 0, then names[j] with value j. 1 when found. */
static int enum_table_found(const uint8_t *base, uint32_t size, uint64_t image_base, const char *const *names, int n)
{
    uint32_t k;
    for (k = 0x1000; k + 0x10u * (uint32_t)n < size; k += 8) {
        uint64_t p0 = qword_at(base, k), v0 = qword_at(base, k + 8);
        int j, ok = 1;
        if (v0 != 0 || p0 < image_base || p0 - image_base >= size) continue;
        if (strcmp((const char *)base + (p0 - image_base), names[0])) continue;
        for (j = 0; j < n && ok; j++) {
            uint64_t pn = qword_at(base, k + 0x10u * (uint32_t)j), vn = qword_at(base, k + 0x10u * (uint32_t)j + 8);
            if (pn < image_base || pn - image_base >= size) { ok = 0; break; }
            if (strcmp((const char *)base + (pn - image_base), names[j]) || vn != (uint64_t)j) ok = 0;
        }
        if (ok) return 1;
    }
    return 0;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, rva[SH_AI_SIG_COUNT];
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    IMAGE_NT_HEADERS *nt;
    uint64_t image_base;
    int vk, i;

    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    vk = identity->vulkan;
    printf("image %s: %s\n", path, identity->label);
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    image_base = nt->OptionalHeader.ImageBase;

    for (i = 0; SH_AI_SIGNATURES[i].name; i++) {}
    CHECK(i == SH_AI_SIG_COUNT);
    for (i = 0; i < SH_AI_SIG_COUNT; i++) {
        const sig_entry *e = &SH_AI_SIGNATURES[i];
        sig_result r;
        rva[i] = 0;
        if (sig_resolve_one(base, e, &r) != SIG_OK || !r.addr) {
            printf("FAIL %s did not resolve to a unique match\n", e->name);
            g_fail = 1;
            continue;
        }
        CHECK(r.rva == (vk ? e->rva_vk : e->rva_gl));
        rva[i] = r.rva;
        printf("  %-20s rva=0x%X\n", e->name, r.rva);
    }

    /* The detour window. */
    if (rva[SH_AI_SIG_IS_RUNNING_SCRIPT]) {
        unsigned flags = 0;
        size_t b = sh_x64_stolen_boundary(base + rva[SH_AI_SIG_IS_RUNNING_SCRIPT], 64, 14, 48, &flags);
        CHECK(b == SH_AI_IRS_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + rva[SH_AI_SIG_IS_RUNNING_SCRIPT], k_ai_irs_prologue, SH_AI_IRS_STOLEN));
    }
    if (rva[SH_AI_SIG_MOVE_TRANSITION]) {
        unsigned flags = 0;
        size_t b = sh_x64_stolen_boundary(base + rva[SH_AI_SIG_MOVE_TRANSITION], 64, 14, 48, &flags);
        CHECK(b == SH_AI_MOVE_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + rva[SH_AI_SIG_MOVE_TRANSITION], k_ai_move_prologue, SH_AI_MOVE_STOLEN));
    }

    /* The path site's targets. */
    if (rva[SH_AI_SIG_PATH_SITE]) {
        uint32_t s = rva[SH_AI_SIG_PATH_SITE];
        uint32_t key = rel_target(base, s + SH_AI_PATH_SITE_KEY_CALL, 5, 1);
        uint32_t glob = rel_target(base, s + SH_AI_PATH_SITE_GLOBAL_LOAD, 7, 3);
        uint32_t query = rel_target(base, s + SH_AI_PATH_SITE_QUERY_CALL, 5, 1);
        CHECK(base[s + SH_AI_PATH_SITE_KEY_CALL] == 0xE8 && base[s + SH_AI_PATH_SITE_QUERY_CALL] == 0xE8);
        CHECK(!memcmp(base + s + SH_AI_PATH_SITE_GLOBAL_LOAD, "\x48\x8B\x0D", 3));
        printf("  path key 0x%X, path query 0x%X, query word 0x%X\n", key, query, glob);
        CHECK(key == (vk ? 0x3D54C0u : 0x3D49C0u));
        CHECK(query == (vk ? 0x515300u : 0x514030u));
        CHECK(glob == (vk ? 0x5B64F00u : 0x446A580u));
        /* the path key indexes the list at ai+0x2AA88 */
        CHECK(!memcmp(base + key, "\x40\x53\x48\x83\xEC\x20\x48\x8B\x81\x88\xAA\x02\x00", 13));
        /* the query reads the move FSM's owner origin (+0x808, slot +0x80) */
        CHECK(window_has(base, query, 0x200, "\xFF\x90\x80\x00\x00\x00", 6));
    }

    /* idAI2's GetType: lea rax,[rip+typeinfo]; ret. */
    {
        uint32_t vt = vk ? 0x2035B30u : 0x204F900u, get_type, ti;
        get_type = (uint32_t)(qword_at(base, vt) - image_base);
        CHECK(get_type == (vk ? 0x391970u : 0x391240u));
        CHECK(base[get_type] == 0x48 && base[get_type + 1] == 0x8D && base[get_type + 2] == 0x05 && base[get_type + 7] == 0xC3);
        ti = rel_target(base, get_type, 7, 3);
        printf("  idAI2 typeinfo 0x%X\n", ti);
        CHECK(ti == (vk ? 0x5CF7A90u : 0x45FD110u));
    }
    /* the off-limits getter (script_api_ai.h, SH_AI_VSLOT_OFF_LIMITS): idAI2's and idPlayer's vtables name the same
     * function, movzx eax, byte [rcx+0x9408]; ret */
    {
        uint32_t vts[2] = { vk ? 0x2035B30u : 0x204F900u, vk ? 0x22A1298u : 0x22BAFC8u }, k;
        for (k = 0; k < 2; k++) {
            uint32_t fn = (uint32_t)(qword_at(base, vts[k] + SH_AI_VSLOT_OFF_LIMITS) - image_base);
            CHECK(fn == (vk ? 0x427550u : 0x426600u));
            CHECK(sh_ai_off_limits_getter(base + fn));
        }
    }

    /* The brain's calls. */
    if (rva[SH_AI_SIG_RECOIL_PLAY])
        CHECK(!memcmp(base + rva[SH_AI_SIG_RECOIL_PLAY] + SH_AI_RECOIL_SITE_MUL, k_ai_recoil_mul, sizeof k_ai_recoil_mul));
    if (rva[SH_AI_SIG_NEXT_PULL]) {
        uint32_t s = rva[SH_AI_SIG_NEXT_PULL];
        uint32_t decl = rel_target(base, s + SH_AI_NEXT_PULL_DECL_CALL, 5, 1);
        uint32_t table = rel_target(base, s + SH_AI_NEXT_PULL_SLOT_TABLE, 7, 3);
        int32_t slots[3];
        CHECK(base[s + SH_AI_NEXT_PULL_DECL_CALL] == 0xE8);
        CHECK(!memcmp(base + s + SH_AI_NEXT_PULL_SLOT_TABLE, "\x48\x8D\x2D", 3));
        printf("  fire decl 0x%X, trigger slots 0x%X\n", decl, table);
        CHECK(decl == (vk ? 0xF137B0u : 0xF12C10u));
        CHECK(table == (vk ? 0x2038F88u : 0x2052D58u));
        memcpy(slots, base + table, sizeof slots);
        CHECK(slots[0] == -1 && slots[1] == 0 && slots[2] == 1);   /* -1: the holder record at +0x30, as the engine reads it */
        /* the fire decl reads the weapon's fire mode table: +0x1898 + mode * 400 */
        CHECK(window_has(base, decl, 0x30, "\x48\x69\xD0\x90\x01\x00\x00\x48\x8B\x84\x0A\x98\x18\x00\x00", 15));
    }
    if (rva[SH_AI_SIG_AIM_FOCUS_SITE]) {
        uint32_t f = rel_target(base, rva[SH_AI_SIG_AIM_FOCUS_SITE] + SH_AI_AIM_FOCUS_CALL, 5, 1);
        printf("  aim focus 0x%X\n", f);
        CHECK(f == (vk ? 0x41BBA0u : 0x41AD60u));
        /* the focus array at ai+0xDFE0, 0xEC0 apart */
        CHECK(window_has(base, f, 0x60, "\x48\x8D\x8E\xE0\xDF\x00\x00", 7) || window_has(base, f, 0x60, "\xE0\xDF\x00\x00", 4));
    }
    if (rva[SH_AI_SIG_PAIN_STRING_SITE]) {
        uint32_t s = rva[SH_AI_SIG_PAIN_STRING_SITE];
        uint32_t intern = rel_target(base, s + SH_AI_PAIN_INTERN_CALL, 5, 1);
        unsigned flags = 0;
        CHECK(!memcmp(base + s + SH_AI_PAIN_STUB_AT, k_ai_pain_stolen, SH_AI_PAIN_STUB_STOLEN));
        CHECK(sh_x64_stolen_boundary(base + s + SH_AI_PAIN_STUB_AT, 64, 14, 48, &flags) == SH_AI_PAIN_STUB_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        /* the atomic-string set writes [rbp+0x88]: lea rcx,[rbp+0x88] just before it */
        CHECK(!memcmp(base + s + SH_AI_PAIN_INTERN_CALL - 7, "\x48\x8D\x8D\x88\x00\x00\x00", 7));
        printf("  atomic string set 0x%X\n", intern);
        CHECK(intern == (vk ? 0x32B3D0u : 0x32B040u));
        /* later in the request: mov rax,[rbp+0x88]; cmp [r13+0x48],rax (each pain anim's string) */
        CHECK(window_has(base, s, 0x900, "\x48\x8B\x85\x88\x00\x00\x00\x49\x39\x45\x48", 11));
    }
    {
        static const int win[2] = { SH_AI_SIG_THREAT_ROW, SH_AI_SIG_PATH_LENGTH };
        static const size_t len[2] = { SH_AI_THREAT_STOLEN, SH_AI_PATH_LENGTH_STOLEN };
        /* the install compares these two windows with the product's own tables before it hooks */
        static const unsigned char *const want[2] = { k_threat_prologue, k_plen_prologue };
        int k;
        for (k = 0; k < 2; k++) {
            unsigned flags = 0;
            if (!rva[win[k]]) continue;
            CHECK(!memcmp(base + rva[win[k]], want[k], len[k]));
            CHECK(sh_x64_stolen_boundary(base + rva[win[k]], 64, 14, 48, &flags) == len[k]);
            CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        }
        /* the row check reads the row's reaction at +0x1C (mov eax,[r8+0x1C]) */
        if (rva[SH_AI_SIG_THREAT_ROW]) CHECK(window_has(base, rva[SH_AI_SIG_THREAT_ROW], 0x180, "\x41\x8B\x40\x1C", 4));
        /* the path length walks segments 0x1C apart from +0x90, count +0x70 */
        if (rva[SH_AI_SIG_PATH_LENGTH])
            CHECK(!memcmp(base + rva[SH_AI_SIG_PATH_LENGTH] + 4, "\x48\x63\x4A\x70", 4) &&
                  window_has(base, rva[SH_AI_SIG_PATH_LENGTH], 0x40, "\x48\x8B\x82\x90\x00\x00\x00", 7));
    }
    /* the monster type names: the executable's {name, value} table */
    {
        uint32_t k, found = 0;
        for (k = 0x1000; k + 0x10 * SH_AI_MONSTER_TYPES < size && !found; k += 8) {
            uint64_t p0 = qword_at(base, k), v0 = qword_at(base, k + 8);
            int j, ok = 1;
            if (v0 != 0 || p0 < image_base || p0 - image_base >= size) continue;
            if (strcmp((const char *)base + (p0 - image_base), "AI_MONSTER_NONE")) continue;
            for (j = 0; j < SH_AI_MONSTER_TYPES && ok; j++) {
                uint64_t pn = qword_at(base, k + 0x10u * (uint32_t)j), vn = qword_at(base, k + 0x10u * (uint32_t)j + 8);
                if (pn < image_base || pn - image_base >= size) { ok = 0; break; }
                if (strcmp((const char *)base + (pn - image_base), SH_AI_MONSTER_NAMES[j])) ok = 0;
                if (vn != (j ? (1ull << (j - 1)) : 0)) ok = 0;
            }
            if (ok) found = k;
        }
        printf("  aiMonsterType_t table 0x%X\n", found);
        CHECK(found != 0);
    }

    /* The fire calls. */
    if (rva[SH_AI_SIG_MUZZLE_TEST]) {
        uint32_t m = rva[SH_AI_SIG_MUZZLE_TEST];
        /* the sphere's radius (ai+0x11AE4) and its precision word (ai+0x11AE8), the weights 3.0, 0.5 and 1.5 */
        CHECK(window_has(base, m, 0x90, "\x83\xB9\xE8\x1A\x01\x00\x01", 7));
        CHECK(window_has(base, m, 0x90, "\xE4\x1A\x01\x00", 4));
        CHECK(window_has(base, m, 0x90, "\x80\xB9\x9B\xEE\x00\x00\x00", 7));
        CHECK(window_has(base, m, 0x90, "\x20\xE0\x00\x00", 4));
    }
    if (rva[SH_AI_SIG_CLEAR_MOVE]) {
        uint32_t c = rva[SH_AI_SIG_CLEAR_MOVE];
        /* ClearMove(fsm, code): the code in r12d, the fsm in r15, the demon is [fsm] */
        CHECK(window_has(base, c, 0x60, "\x44\x8B\xE2\x4C\x8B\xF9\x48\x8B\x09", 9));
    }
    if (rva[SH_AI_SIG_FIRE_THINK]) {
        uint32_t t = rva[SH_AI_SIG_FIRE_THINK];
        /* the state mode at +0x20, the animation event's mode at +0x1C; mode 5 (MAX) is skipped */
        CHECK(window_has(base, t, 0x60, "\x8B\x43\x20\x85\xC0\x74", 6));
        CHECK(window_has(base, t, 0x60, "\x8B\x4B\x1C\x85\xC9\x74", 6));
        CHECK(window_has(base, t, 0x60, "\x83\xF9\x05", 3));
    }
    if (rva[SH_AI_SIG_IDLE_ACTION]) {
        uint32_t a = rva[SH_AI_SIG_IDLE_ACTION];
        /* turnParms count (+0x530) and list (+0x528) of the behaviour decl at ai+0xC120, then the idle
         * turn type stored at animFSM +0xBF0 (animFSM at ai+0x23198) and the move flag 8 at +0xB6C */
        CHECK(window_has(base, a, 0x60, "\x48\x8B\x8F\x20\xC1\x00\x00\x3B\x91\x30\x05\x00\x00", 13));
        CHECK(window_has(base, a, 0x60, "\x48\x8B\x89\x28\x05\x00\x00", 7));
        CHECK(window_has(base, a, 0x100, "\x89\x81\xF0\x0B\x00\x00", 6));
        CHECK(window_has(base, a, 0x100, "\x48\x8B\x8F\x98\x31\x02\x00", 7));
        CHECK(window_has(base, a, 0x100, "\x83\x88\x6C\x0B\x00\x00\x08", 7));
    }
    if (rva[SH_AI_SIG_CHECK_PULL]) {
        /* the muzzle test is asked only when the fire control's byte +0x27 is set: cmp byte [r13+0x27], 0 */
        CHECK(window_has(base, rva[SH_AI_SIG_CHECK_PULL] + 0x25A, 5, "\x41\x80\x7D\x27\x00", 5));
    }
    if (rva[SH_AI_SIG_LASER_ADD]) {
        uint32_t a = rva[SH_AI_SIG_LASER_ADD];
        /* the list's count at component +0x108, the entry's name at +0x60 of a 0x80 entry; the ribbon comes from
         * the component's decl (+0x10) field +0x108, read where the product checks it */
        CHECK(!memcmp(base + a + SH_AI_LASER_ADD_DECL_AT, k_ai_laser_decl_read, sizeof k_ai_laser_decl_read));
        CHECK(!memcmp(base + a + SH_AI_LASER_ADD_RIBBON_AT, k_ai_laser_ribbon_read, sizeof k_ai_laser_ribbon_read));
        CHECK(base[a + SH_AI_LASER_ADD_RIBBON_AT + 7] == 0xE8);
        CHECK(window_has(base, a, 0x60, "\x44\x8B\x86\x08\x01\x00\x00", 7));
    }
    if (rva[SH_AI_SIG_LASER_STATE]) {
        /* cmp qword [rcx+0x2CD90], 0: the laser targeter component of an idAI2 */
        CHECK(window_has(base, rva[SH_AI_SIG_LASER_STATE], 0x20, "\x48\x83\xB9\x90\xCD\x02\x00\x00", 8));
    }
    if (rva[SH_AI_SIG_FOOT_TABLE]) {
        unsigned flags = 0;
        size_t b = sh_x64_stolen_boundary(base + rva[SH_AI_SIG_FOOT_TABLE], 64, 14, 48, &flags);
        CHECK(b == SH_AI_FOOT_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + rva[SH_AI_SIG_FOOT_TABLE], k_ai_foot_prologue, SH_AI_FOOT_STOLEN));
    }
    if (rva[SH_AI_SIG_CMD_SET_WAIT]) {
        uint32_t w = rva[SH_AI_SIG_CMD_SET_WAIT];
        /* the active handle at context +0x18, the status written at handle +0x18: 9, INTERRUPTED */
        CHECK(window_has(base, w, 0x20, "\x48\x8B\x79\x18", 4));
        CHECK(window_has(base, w, 0x70, "\xC7\x40\x18\x09\x00\x00\x00\x48\x89\x73\x18", 11));
    }
    if (rva[SH_AI_SIG_GORE_INIT_SITE]) {
        uint32_t s = rva[SH_AI_SIG_GORE_INIT_SITE];
        uint32_t f = rel_target(base, s + SH_AI_GORE_SITE_CALL, 5, 1);
        int32_t damage, comp;
        memcpy(&damage, base + s + SH_AI_GORE_SITE_DAMAGE + 3, 4);
        memcpy(&comp, base + s + SH_AI_GORE_SITE_COMP + 3, 4);
        printf("  gore init 0x%X, component ai +0x%X, entity damage ai +0x%X\n", f, comp, damage);
        if (vk) CHECK(f == 0x8FF0B0u);
        CHECK(comp == 0x2B3D8 && damage == 0x4E08);
        /* the 12 bytes before the site load the behaviour decl (ai +0xC120) whose monster type it tests */
        CHECK(!memcmp(base + s - 12, "\x49\x8B\x86\x20\xC1\x00\x00", 7));
        CHECK(!memcmp(base + s + SH_AI_GORE_SITE_CVAR, "\x83\x3D", 2));
        /* the init keeps the graph at component +0x630 and reads the owner's model at +0x3DB0 */
        CHECK(window_has(base, f, 0x100, "\x30\x06\x00\x00", 4));
        CHECK(window_has(base, f, 0x200, "\xB0\x3D\x00\x00", 4));
    }
    if (rva[SH_AI_SIG_BREAKABLE_GORE]) {
        uint32_t g = rva[SH_AI_SIG_BREAKABLE_GORE];
        unsigned flags = 0;
        size_t b = sh_x64_stolen_boundary(base + g, 64, 14, 48, &flags);
        CHECK(b == SH_AI_BRK_GORE_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + g, k_ai_brk_gore_prologue, SH_AI_BRK_GORE_STOLEN));
        /* the breakable's physics at +0xCA8 and its rigid body count at +0xCC8 (the list +0x18, count +8) */
        CHECK(window_has(base, g, 0x500, "\xA8\x0C\x00\x00", 4));
        CHECK(window_has(base, g, 0x900, "\xC8\x0C\x00\x00", 4));
        CHECK(SH_AI_BRK_PHYSICS + SH_AI_FXB_BODIES + 8 == 0xCC8u);
    }
    if (rva[SH_AI_SIG_BREAKABLE_BREAK]) {
        uint32_t k = rva[SH_AI_SIG_BREAKABLE_BREAK];
        /* cmp dword [r14+0xE6C], -1 (brokenStartTime unset); mov rcx,[r14+0x808] (the physics); the pieces' physics
         * at +0xCA8 passed to the placement */
        CHECK(!memcmp(base + k + 0x1B, "\x41\x83\xBE\x6C\x0E\x00\x00\xFF", 8));
        CHECK(!memcmp(base + k + 0x29, "\x49\x8B\x8E\x08\x08\x00\x00", 7));
        CHECK(SH_AI_BRK_BROKEN == 0xE6Cu && SH_AI_OFF_PHYSICS == 0x808u);
        CHECK(window_has(base, k, 0x100, "\xA8\x0C\x00\x00", 4));
    }
    if (rva[SH_AI_SIG_WEAPON_USE]) {
        /* the accessor's own bytes are the offsets the call uses */
        const uint8_t *s = base + rva[SH_AI_SIG_WEAPON_USE];
        uint32_t index, decl, list;
        memcpy(&index, s + 3, 4); memcpy(&decl, s + 14, 4); memcpy(&list, s + 29, 4);
        CHECK(index == SH_AI_OFF_WEAPON_USE_INDEX && decl == SH_AI_OFF_BEHAVIOR_DECL && list == SH_AI_BEHAVIOR_WEAPON_USES);
        CHECK(!memcmp(s + 18, "\x48\x8D\x0C\x80\x48\xC1\xE1\x04", 8));   /* index x 5 x 16 = 0x50 an entry */
        CHECK(!memcmp(s + 33, "\x48\x83\xC0\x08", 4));                     /* the entry's data at +8 */
    }
    if (rva[SH_AI_SIG_WEB_SCALAR_SET]) {
        /* the set-by-name (script_api_ai.h, SH_AI_WEB_*): the rows, their count and stride, the web's own memory, and
         * the store it calls (a bound row's member at +0x10 when the short at +10 is below 0, else memory[index]) */
        static const uint8_t store[] = {
            0x0F, 0xB7, 0x41, 0x0A, 0x66, 0x85, 0xC0, 0x79, 0x09, 0x48, 0x8B, 0x41, 0x10, 0xF3, 0x0F, 0x11, 0x10, 0xC3,
            0x48, 0x0F, 0xBF, 0xC8, 0x48, 0x8B, 0x02, 0xF3, 0x0F, 0x11, 0x14, 0x88, 0xC3 };
        static const uint8_t snippet[] = { 0x0F, 0x5B, 0xD2, 0x48, 0x8D, 0x88, 0xB8, 0x22, 0x02, 0x00, 0xE9 };
        uint32_t s = rva[SH_AI_SIG_WEB_SCALAR_SET], k, found = 0;
        CHECK(!memcmp(base + s + 4, "\x48\x63\x81\x88\x05\x00\x00", 7) && SH_AI_WEB_ROW_COUNT == 0x588u);
        CHECK(!memcmp(base + s + 0x12, "\x4C\x8B\x99\x80\x05\x00\x00", 7) && SH_AI_WEB_ROWS == 0x580u);
        CHECK(!memcmp(base + s + 0x30, "\x48\x83\xC0\x18", 4) && SH_AI_WEB_ROW_SIZE == 0x18u);
        CHECK(!memcmp(base + s + 0x40, "\x48\x8D\x91\x98\x05\x00\x00", 7) && SH_AI_WEB_SCALAR_MEMORY == 0x598u);
        CHECK(base[s + SH_AI_WEB_SCALAR_STORE] == 0xE8);
        CHECK(!memcmp(base + rel_target(base, s + SH_AI_WEB_SCALAR_STORE, 5, 1), store, sizeof store));
        /* the game's own snippet (idAISnippet_Shared_SetScalarToProjectileIndex) sets a variable on the animator at
         * ai +0x222B8 with a float made from an int: cvtdq2ps xmm2; lea rcx,[rax+0x222B8]; jmp set */
        for (k = 0x1000; k + sizeof snippet + 4 < size; k++) {
            if (memcmp(base + k, snippet, sizeof snippet)) continue;
            if (rel_target(base, k + sizeof snippet - 1, 5, 1) == s) found++;
        }
        CHECK(found == 1 && SH_AI_OFF_WEB_ANIMATOR == 0x222B8u);
    }
    if (rva[SH_AI_SIG_FSM_MOVE]) {
        /* the entrance move (script_ai_sigs.c AiFsmMove), which the dying park repeats into ac_default: it finds the
         * FSM by name (owner vtable +0x60), reads the FSM's states at +0x38 with their count at +0x40, loads
         * idTransitionCode's vtable (by its RTTI name), stores code 5 and takes the move through the FSM's vtable
         * +0xA8 (fsm, move at rsp+0x50, time at rsp+0x38, 1, 1); idAI2's entrance animation calls it */
        const uint8_t *m = base + rva[SH_AI_SIG_FSM_MOVE];
        uint32_t vt = rel_target(base, rva[SH_AI_SIG_FSM_MOVE] + SH_AI_FSM_MOVE_VT_LEA, 7, 3), col = 0, td = 0;
        uint64_t meta = 0;
        CHECK(!memcmp(m + 0x7E, "\x41\xFF\x50\x60", 4));                                  /* call [r8+0x60] */
        CHECK(!memcmp(m + 0xA8, "\x39\x70", 2) && m[0xAA] == SH_AI_FSM_STATE_COUNT);       /* cmp [rax+0x40], esi */
        CHECK(!memcmp(m + 0xB1, "\x48\x8B\x43", 3) && m[0xB4] == SH_AI_FSM_STATES);        /* mov rax, [rbx+0x38] */
        CHECK(!memcmp(m + SH_AI_FSM_MOVE_VT_LEA, "\x48\x8D\x0D", 3));
        CHECK(!memcmp(m + SH_AI_FSM_MOVE_CODE_STORE, "\xC7\x44\x24\x78", 4) &&
              m[SH_AI_FSM_MOVE_CODE_STORE + 4] == SH_AI_FSM_MOVE_IMMEDIATE &&
              0x78 - 0x50 == SH_AI_FSM_MOVE_CODE);                                       /* the record at rsp+0x50 */
        CHECK(!memcmp(m + 0x10A, "\x48\x89\x7C\x24\x60", 5) && 0x60 - 0x50 == SH_AI_FSM_MOVE_TARGET);
        CHECK(!memcmp(m + 0xF7, "\x48\x89\x4C\x24\x70", 5) && 0x70 - 0x50 == SH_AI_FSM_MOVE_CODE_VT);
        CHECK(!memcmp(m + SH_AI_FSM_MOVE_TAKE_CALL, "\xFF\x90\xA8\x00\x00\x00", 6) && SH_AI_FSM_TAKE_SLOT == 0xA8u);
        CHECK(!memcmp(m + SH_AI_FSM_MOVE_TAKE_CALL - 0x15, "\xC6\x44\x24\x20\x01\x41\xB1\x01\x4C\x8D\x44\x24\x38\x48\x8D\x54\x24\x50", 18));
        meta = qword_at(base, vt - 8);
        if (meta > image_base && meta - image_base + 16 < size) col = (uint32_t)(meta - image_base);
        if (col) memcpy(&td, base + col + 12, 4);
        CHECK(td && !strcmp((const char *)base + td + 0x10, ".?AVidTransitionCode@@"));
        { uint32_t ent = vk ? 0x3D4C80u : 0x3D4180u;   /* idAI2 entrance animation: the move at +0xAD */
          CHECK(base[ent + 0xAD] == 0xE8 && rel_target(base, ent + 0xAD, 5, 1) == rva[SH_AI_SIG_FSM_MOVE]); }
    }
    if (rva[SH_AI_SIG_UPDATE_RELATIONS] && rva[SH_AI_SIG_BEST_RELATION]) {
        /* the senses' relation table (script_api_ai.h, SH_AI_REL_*): UpdateRelations empties it (rows at +0x28,
         * count at rows + 8) and reads each perceived entity's faction at +0x4EC0; the engine's pick reads a row's
         * emotion at +8 and its entity at +0x10, 0x28 bytes a row, from EMOTION_MAX 14 down */
        const uint8_t *u = base + rva[SH_AI_SIG_UPDATE_RELATIONS], *b = base + rva[SH_AI_SIG_BEST_RELATION];
        uint32_t faction;
        CHECK(u[SH_AI_UPDREL_ROWS_DISP] == SH_AI_REL_ROWS && !memcmp(u + SH_AI_UPDREL_ROWS_DISP - 3, "\x48\x8D\x59", 3));
        CHECK(u[SH_AI_UPDREL_COUNT_DISP] == SH_AI_REL_COUNT - SH_AI_REL_ROWS &&
              !memcmp(u + SH_AI_UPDREL_COUNT_DISP - 2, "\xC7\x43", 2));
        memcpy(&faction, u + SH_AI_UPDREL_FACTION_IMM, 4);
        CHECK(faction == 0x4EC0u);
        CHECK(b[SH_AI_BEST_EMOTION_DISP] == SH_AI_REL_EMOTION && !memcmp(b + SH_AI_BEST_EMOTION_DISP - 3, "\x8B\x44\x0B", 3));
        CHECK(b[SH_AI_BEST_ENTITY_IMM] == SH_AI_REL_ENTITY && !memcmp(b + SH_AI_BEST_ENTITY_IMM - 3, "\x48\x83\xC1", 3));
        CHECK(b[SH_AI_BEST_STRIDE_IMM] == SH_AI_REL_SIZE && !memcmp(b + SH_AI_BEST_STRIDE_IMM - 3, "\x48\x83\xC3", 3));
        CHECK(b[SH_AI_BEST_START_DISP] == 14 && !memcmp(b + SH_AI_BEST_START_DISP - 3, "\x44\x8D\x7D", 3));
    }
    if (rva[SH_AI_SIG_FXPHYS_PLACE]) {
        /* mov rdx,[rsp+0x90]: the fifth argument the placement passes as 0 */
        CHECK(!memcmp(base + rva[SH_AI_SIG_FXPHYS_PLACE] + 0x1F, "\x48\x8B\x94\x24\x90\x00\x00\x00", 8));
    }
    if (rva[SH_AI_SIG_SHOW_WOUND]) {
        unsigned flags = 0;
        size_t b = sh_x64_stolen_boundary(base + rva[SH_AI_SIG_SHOW_WOUND], 64, 14, 48, &flags);
        CHECK(b == SH_AI_WOUND_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + rva[SH_AI_SIG_SHOW_WOUND], k_ai_wound_prologue, SH_AI_WOUND_STOLEN));
        /* the owner handle is the pair at component +0x608 (its pointer, +0x610, is read through it) */
        CHECK(window_has(base, rva[SH_AI_SIG_SHOW_WOUND], 0x120, "\x08\x06\x00\x00", 4));
    }
    if (rva[SH_AI_SIG_DAMAGE_WOUND]) {
        /* DamageActivatesWound (script_api_ai.h, SH_AI_DAW_*) */
        static const sig_entry gfd = { "GoreFrameDamage",
            "48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 8B 81 DC 02 00 00 4D 8B F0 0F 29 74 24 30 "
            "0F 28 F3 0F 29 7C 24 20 0F 28 F9 48 8B F9", 0x8FA7B0u, 0x8F97E0u };
        uint32_t s = rva[SH_AI_SIG_DAMAGE_WOUND], thr, k, reads = 0;
        unsigned flags = 0;
        sig_result u;
        size_t b = sh_x64_stolen_boundary(base + s, 64, 14, 48, &flags);
        CHECK(b == SH_AI_DAW_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + s, k_ai_daw_prologue, SH_AI_DAW_STOLEN));
        /* the signature is the only match in the image */
        CHECK(sig_scan_one(base, &SH_AI_SIGNATURES[SH_AI_SIG_DAMAGE_WOUND], &u) == SIG_OK && u.rva == s);
        /* the arguments kept: r12 the group decl, r15 the 5th (the group index), r14 the record, rsi the wound,
         * rbx the component; the index is refused above 31; the owner handle at component +0x608 */
        CHECK(!memcmp(base + s + 0x2D, "\x4D\x8B\xE1\x4C\x63\x7D\x6F\x4D\x8B\xF0\x48\x8B\xF2\x48\x8B\xD9", 16));
        CHECK(!memcmp(base + s + 0x94, "\x41\x83\xFF\x1F", 4));
        CHECK(window_has(base, s, 0x60, "\x48\x8D\xB9\x08\x06\x00\x00", 7));
        /* the record's gore behaviour at +0x40 (the queue stores it there) */
        CHECK(!memcmp(base + s + 0xC6, "\x49\x8B\x46\x40", 4));
        /* the frame gate: the wound's +0x218 against the component's group frame damage +0xA0C + 4 x group */
        CHECK(!memcmp(base + s + 0x1AC, "\xF3\x0F\x10\x86\x18\x02\x00\x00", 8));
        CHECK(!memcmp(base + s + 0x1BC, "\x42\x0F\x2F\x84\xBB\x0C\x0A\x00\x00", 9));
        /* the threshold: the helper gets the wound and the 7th argument (the max health, a float) */
        CHECK(!memcmp(base + s + SH_AI_DAW_MAXH_READ, "\xF3\x0F\x10\x4D\x7F\x48\x8B\xCE\xE8", 9));
        CHECK(SH_AI_DAW_MAXH_READ + 8 == SH_AI_DAW_THRESHOLD_CALL);
        thr = rel_target(base, s + SH_AI_DAW_THRESHOLD_CALL, 5, 1);
        printf("  wound threshold 0x%X\n", thr);
        CHECK(thr == (vk ? 0x6E8A80u : 0x6E7EE0u));
        /* relative (+0x214) below 0: total (+0x210); else relative x max health */
        CHECK(!memcmp(base + thr, "\xF3\x0F\x10\x91\x14\x02\x00\x00\x0F\x57\xC0\x0F\x2F\xC2\x76\x09"
                                  "\xF3\x0F\x10\x81\x10\x02\x00\x00\xC3\xF3\x0F\x59\xD1\x0F\x28\xC2", 32));
        CHECK(*(const uint32_t *)(base + thr + 4) == SH_AI_WOUND_RELATIVE_REQUIRED &&
              *(const uint32_t *)(base + thr + 20) == SH_AI_WOUND_TOTAL_REQUIRED);
        /* the 6th argument (the lost health) is an int, compared with the threshold and refused below it */
        CHECK(!memcmp(base + s + SH_AI_DAW_LOST_READ, "\x66\x0F\x6E\x4D\x77\x0F\x5B\xC9\x0F\x2F\xC1\x0F\x87", 13));
        /* ... and read nowhere else in the function (no other [rbp+0x77] operand in its 0x692 bytes) */
        for (k = 0; k + 1 < 0x692u; k++) if ((base[s + k] & 0xC7) == 0x45 && base[s + k + 1] == 0x77) reads++;
        CHECK(reads == 1);
        /* the record: the frame damage queue's 8-slot ring of 0x58-byte records, its +0 the hit's damage times the
         * gore behaviour's +0x9C and the two scales, the behaviour at +0x40 */
        if (sig_resolve_one(base, &gfd, &u) == SIG_OK && u.addr) {
            CHECK(!memcmp(base + u.rva + 0x49, "\x48\x6B\xF0\x58", 4) && SH_AI_GFD_SIZE == 0x58);
            CHECK(!memcmp(base + u.rva + 0x6F, "\xF3\x0F\x59\xB0\x9C\x00\x00\x00", 8));
            CHECK(!memcmp(base + u.rva + 0x96, "\xF3\x0F\x11\x36", 4) && SH_AI_GFD_DAMAGE == 0);
            CHECK(!memcmp(base + u.rva + 0xA1, "\x48\x89\x46\x40", 4));
        } else {
            printf("FAIL the frame damage queue did not resolve\n");
            g_fail = 1;
        }
    }
    if (rva[SH_AI_SIG_PAIN_START]) {
        uint32_t s = rva[SH_AI_SIG_PAIN_START];
        unsigned flags = 0;
        size_t b;
        uint32_t request = rel_target(base, s + SH_AI_PAIN_START_FSM_CALL, 5, 1);
        uint32_t code = rel_target(base, s + SH_AI_PAIN_START_CODE_CALL, 5, 1);
        printf("  pain start 0x%X: fsm request 0x%X, pain code 0x%X\n", s, request, code);
        /* the FSM request is the detoured function: its window */
        b = sh_x64_stolen_boundary(base + request, 64, 14, 48, &flags);
        CHECK(b == SH_AI_FSM_REQUEST_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + request, k_ai_fsm_request_prologue, SH_AI_FSM_REQUEST_STOLEN));
        /* it keeps the code it is asked with at fsm +0x6C08 while it looks for a link */
        CHECK(window_has(base, request, 0x90, "\x08\x6C\x00\x00", 4));
        CHECK(request == (vk ? 0x64A560u : 0x649870u) && code == (vk ? 0x5FBE70u : 0x5FB0A0u));
        /* the FSM is asked of the object at ai +0x123B8 through its vtable +0x78, with TRANSCODE_AC_IN_PAIN (232) */
        CHECK(!memcmp(base + s + SH_AI_PAIN_START_FSM_CALL - 21,
                      "\x48\x81\xC1\xB8\x23\x01\x00\x48\x8B\x01\xFF\x50\x78\xBA\xE8\x00\x00\x00\x48\x8B\xC8", 21));
        /* the second request is made with the code the pain code call returned (mov edx,eax ... call) */
        CHECK(rel_target(base, s + SH_AI_PAIN_START_CODE_CALL + 10, 5, 1) == request);
        /* the pain code reads the request at ai +0x8678 and answers 232, 233 or 237/238 */
        CHECK(window_has(base, code, 0x90, "\x8B\x91\x78\x86\x00\x00", 6));
        CHECK(window_has(base, code, 0x90, "\xB8\xE8\x00\x00\x00", 5));
        /* request_pain: the pain start's own calls, each at its call site (the same offsets on both renderers) */
        {
            static const struct { uint32_t at, vk, gl; } calls[] = {
                { SH_AI_PS_INTENSITY_CALL,   0x74A9B0u,  0x74A2C0u },
                { SH_AI_PS_DIR_CALL,         0x74A640u,  0x749F50u },
                { SH_AI_PS_GROUP_CALL,       0x74BF70u,  0x74B880u },
                { SH_AI_PS_CHECK_CALL,       0x3F5440u,  0x3F4920u },   /* CheckPainAnim */
                { SH_AI_PS_STR_CALL,         0x32B3D0u,  0x32B040u },
                { SH_AI_PS_HANDLE_CALL,      0x3B7330u,  0x3B6A50u },
                { SH_AI_PS_DAMAGE_COPY_CALL, 0x365FF0u,  0x365980u },
                { SH_AI_PS_ASSIGN_CALL,      0x3F48E0u,  0x3F3DC0u },
                { SH_AI_PS_BUSY_CALL,        0x13350D0u, 0x1335080u },
                { SH_AI_PS_BB_SET_CALL,      0x3CFA90u,  0x3CF400u },
                { SH_AI_PS_BB_CLEAR_CALL,    0x3DA310u,  0x3D9830u },
                { SH_AI_PS_DAMAGE_DTOR_CALL, 0x6E5480u,  0x6E48A0u },
            };
            /* the gates and fields, each the displacement its instruction at that offset reads */
            static const struct { uint32_t at, disp; } reads[] = {
                { 0x04Au, SH_AI_PS_OFF_REMOTE }, { 0x057u, SH_AI_PS_OFF_FORCED_A }, { 0x060u, SH_AI_PS_OFF_FORCED_B },
                { 0x092u, SH_AI_PS_OFF_DISABLED }, { 0x13Cu, SH_AI_PS_OFF_BEHAVIOR }, { 0x143u, SH_AI_PS_OFF_DIRS },
                { 0x177u, 0x5060u }, { 0x1EFu, 0xD4u }, { 0x21Cu, 0xD6u }, { 0x259u, SH_AI_PS_VSLOT_DEAD },
                { 0x287u, SH_AI_PS_OFF_ANIMFSM }, { 0x28Eu, 0x199Cu }, { 0x2BBu, SH_AI_PS_OFF_INJURED },
                { 0x2CAu, SH_AI_PS_OFF_INJURED_TABLE }, { 0x357u, SH_AI_PS_OFF_CAN_PAIN },
                { 0x48Fu, SH_AI_PS_OFF_PAIN_STRINGS }, { 0x4AEu, SH_AI_PS_OFF_PAIN_STRINGS + 8u },
                { 0x5D2u, SH_AI_PS_OFF_REQUEST }, { 0x5F8u, SH_AI_PS_OFF_BB_FLAG }, { 0x5FFu, SH_AI_PS_OFF_BLACKBOARD },
                { 0x610u, 0xD8u },
            };
            size_t k;
            for (k = 0; k < sizeof calls / sizeof calls[0]; k++) {
                CHECK(base[s + calls[k].at] == 0xE8);
                CHECK(rel_target(base, s + calls[k].at, 5, 1) == (vk ? calls[k].vk : calls[k].gl));
            }
            for (k = 0; k < sizeof reads / sizeof reads[0]; k++) {
                uint32_t d = reads[k].disp;
                CHECK(window_has(base, s + reads[k].at, 8, (const char *)&d, 4));
            }
            /* the blackboard key and the engine's empty string the start leas */
            CHECK(!memcmp(base + s + SH_AI_PS_KEY_LEA, "\x48\x8D\x15", 3) && !memcmp(base + s + SH_AI_PS_EMPTY_LEA, "\x48\x8D\x15", 3));
            CHECK(rel_target(base, s + SH_AI_PS_KEY_LEA, 7, 3) == (vk ? 0x5B65720u : 0x446ADA0u));
            CHECK(rel_target(base, s + SH_AI_PS_EMPTY_LEA, 7, 3) == (vk ? 0x1FE5308u : 0x1FFF108u));
            CHECK(base[rel_target(base, s + SH_AI_PS_EMPTY_LEA, 7, 3)] == 0);
            /* the request: built at rbp-0x20 and copied to ai +0x8678; its damage info at +0x68 (rbp+0x48), its
             * direction at +0x688 (rbp+0x668), the copy ends at +0x690 (the assign's last dword, 0x1A4 x 4) */
            CHECK(!memcmp(base + s + SH_AI_PS_ASSIGN_CALL - 4, "\x48\x8D\x55\xE0", 4));
            CHECK(!memcmp(base + s + SH_AI_PS_DAMAGE_COPY_CALL - 4, "\x48\x8D\x4D\x48", 4));
            CHECK(!memcmp(base + s + 0x57C, "\xF2\x0F\x11\x85\x68\x06\x00\x00", 8) &&
                  SH_AI_PS_REQ_DIR == 0x688u && SH_AI_PS_REQ_DAMAGE == 0x68u);
            {
                uint32_t assign = rel_target(base, s + SH_AI_PS_ASSIGN_CALL, 5, 1);
                /* its last copies: the direction at +0x688 and the request's last dword at +0x690 */
                CHECK(window_has(base, assign, 0xC0, "\x89\x83\x88\x06\x00\x00", 6) &&
                      window_has(base, assign, 0xC0, "\x89\x83\x90\x06\x00\x00", 6));
                CHECK(SH_AI_PS_REQUEST_SIZE == 0x694u);
            }
            /* CheckPainAnim holds the pain string site (SH_AI_SIG_PAIN_STRING_SITE) */
            if (rva[SH_AI_SIG_PAIN_STRING_SITE]) {
                uint32_t check = rel_target(base, s + SH_AI_PS_CHECK_CALL, 5, 1);
                CHECK(rva[SH_AI_SIG_PAIN_STRING_SITE] > check && rva[SH_AI_SIG_PAIN_STRING_SITE] < check + 0x2000u);
            }
        }
    }
    if (rva[SH_AI_SIG_MESH_HIDE_SITE]) {
        uint32_t s = rva[SH_AI_SIG_MESH_HIDE_SITE];
        uint32_t index = rel_target(base, s + SH_AI_MESH_SITE_INDEX, 5, 1);
        uint32_t hide = rel_target(base, s + SH_AI_MESH_SITE_HIDE, 5, 1);
        printf("  mesh index by name 0x%X, mesh hide 0x%X\n", index, hide);
        if (vk) CHECK(index == 0x15EDF50u && hide == 0x15EFEC0u);
        /* the hide clears a bit of the shown-mesh mask at model +0xEE8; the index walks the model's meshes */
        CHECK(window_has(base, hide, 0x30, "\xE8\x0E\x00\x00", 4));
        CHECK(window_has(base, index, 0x20, "\x90\x0E\x00\x00", 4));
    }
    if (rva[SH_AI_SIG_RIG_ENTRY] && rva[SH_AI_SIG_RIG_EVAL] && rva[SH_AI_SIG_RIG_SOLVE] && rva[SH_AI_SIG_RIG_SITE]) {
        uint32_t e = rva[SH_AI_SIG_RIG_ENTRY], ev = rva[SH_AI_SIG_RIG_EVAL], s = rva[SH_AI_SIG_RIG_SITE];
        uint32_t align = rel_target(base, e + 39, 5, 1);
        unsigned flags = 0;
        size_t b = sh_x64_stolen_boundary(base + e, 64, 14, 48, &flags);
        int32_t model, anim;
        CHECK(b == SH_AI_RIG_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + e, k_ai_rig_prologue, SH_AI_RIG_STOLEN));
        /* the entry runs the engine at runtime +0x8 after the aligner at +0x18 */
        CHECK(window_has(base, e, 0x30, "\x48\x8B\x49\x18", 4) && window_has(base, e, 0x40, "\x48\x8B\x4B\x08", 4));
        /* the aligner writes local translations of the pose at context +0x50: stride +0x28, base +0x10 */
        CHECK(window_has(base, align, 0x100, "\x48\x8B\x77\x50", 4));
        CHECK(window_has(base, align, 0x100, "\x0F\xAF\x46\x28", 4) && window_has(base, align, 0x100, "\x48\x03\x4E\x10", 4));
        /* the evaluate: legs from +0x70 on, the target (+0x84) read in model space, the pole (+0x88) as a local
         * rotation (stride +0x2C, base +0x18), the hip (+0x8C), the parent (+0x80); thigh, shin and foot written
         * through +0x90, +0x94 and +0x98; a leg is 0xCC bytes */
        CHECK(window_has(base, ev, 0x40, "\x48\x8D\x7A\x70", 4));
        CHECK(window_has(base, ev, 0xA0, "\x8B\x57\x14\x81\xE2\xFF\xFF\xFF\x00", 9));
        CHECK(window_has(base, ev, 0xE0, "\x8B\x47\x18\x25\xFF\xFF\xFF\x00", 8));
        CHECK(window_has(base, ev, 0xE0, "\x0F\xAF\x43\x2C", 4) && window_has(base, ev, 0xE0, "\x48\x03\x4B\x18", 4));
        CHECK(window_has(base, ev, 0x100, "\x8B\x47\x1C\x25\xFF\xFF\xFF\x00", 8));
        CHECK(window_has(base, ev, 0x130, "\x8B\x47\x10\x25\xFF\xFF\xFF\x00", 8));
        CHECK(window_has(base, ev, 0x720, "\x8B\x47\x20", 3) && window_has(base, ev, 0x720, "\x8B\x47\x24", 3) &&
              window_has(base, ev, 0x720, "\x8B\x47\x28", 3));
        CHECK(window_has(base, ev, 0x720, "\x48\x81\xC7\xCC\x00\x00\x00", 7));
        CHECK(base[ev + SH_AI_RIG_EVAL_SOLVE_CALL] == 0xE8);
        CHECK(rel_target(base, ev + SH_AI_RIG_EVAL_SOLVE_CALL, 5, 1) == rva[SH_AI_SIG_RIG_SOLVE]);
        /* the demon's model and the model's animation object, as the setup reads them */
        memcpy(&model, base + s + SH_AI_RIG_SITE_MODEL, 4);
        memcpy(&anim, base + s + SH_AI_RIG_SITE_ANIM, 4);
        printf("  rig: aligner 0x%X, model ai +0x%X, animation object model +0x%X\n", align, model, anim);
        CHECK(model == 0x3DB0 && anim == 0xE98);
    }
    if (rva[SH_AI_SIG_FK_REFERENCE] && rva[SH_AI_SIG_WEB_LEAF]) {
        uint32_t f = rva[SH_AI_SIG_FK_REFERENCE], w = rva[SH_AI_SIG_WEB_LEAF];
        unsigned flags = 0;
        size_t b = sh_x64_stolen_boundary(base + f, 64, 14, 48, &flags);
        CHECK(b == SH_AI_FKREF_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + f, k_ai_fkref_prologue, SH_AI_FKREF_STOLEN));
        flags = 0;
        b = sh_x64_stolen_boundary(base + w, 64, 14, 48, &flags);
        CHECK(b == SH_AI_LEAF_STOLEN);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        CHECK(!memcmp(base + w, k_ai_leaf_prologue, SH_AI_LEAF_STOLEN));
        /* the reference pose asks the web at ai +0x222B8 for its current leaf first (add rcx, 0x222B8; call) */
        CHECK(!memcmp(base + f + SH_AI_FKREF_LEAF_CALL - 7, "\x48\x81\xC1\xB8\x22\x02\x00", 7));
        CHECK(base[f + SH_AI_FKREF_LEAF_CALL] == 0xE8);
        CHECK(rel_target(base, f + SH_AI_FKREF_LEAF_CALL, 5, 1) == w);
        /* without one, the effector's poseReferenceAnim: index * 0x58 into the subgraph's effectors (+0x40), the
         * idStr's data at +0x30 (so its length at +0x28) */
        CHECK(window_has(base, f + SH_AI_FKREF_LEAF_CALL, 0x30, "\x49\x6B\xDC\x58", 4));
        CHECK(window_has(base, f + SH_AI_FKREF_LEAF_CALL, 0x30, "\x48\x8B\x48\x40", 4));
        CHECK(window_has(base, f + SH_AI_FKREF_LEAF_CALL, 0x40, "\x4C\x8B\x4C\x19\x30", 5));
        CHECK(SH_AI_FK_SUBGRAPH_EFFECTORS == 0x40 && SH_AI_FK_EFFECTOR_SIZE == 0x58 && SH_AI_FK_EFFECTOR_REF_LEN == 0x30 - 8);
    }
    if (rva[SH_AI_SIG_WEB_FORCE_STATE] && rva[SH_AI_SIG_WEB_UPDATE]) {
        uint32_t f = rva[SH_AI_SIG_WEB_FORCE_STATE], u = rva[SH_AI_SIG_WEB_UPDATE];
        /* ForceState sets interruptBlend YES at web +0x570 (interruptPath at +0x56C just before) */
        CHECK(!memcmp(base + f + SH_AI_FORCE_STATE_SET_YES, "\x41\xC7\x85\x70\x05\x00\x00\x01\x00\x00\x00", 11));
        CHECK(!memcmp(base + f + SH_AI_FORCE_STATE_SET_YES - 11, "\x41\xC7\x85\x6C\x05\x00\x00\x01\x00\x00\x00", 11));
        /* the update: the node's bit 25 (AW_CF_USE_MULTIPLE_INTERRUPT), then the mode at +0x570, 0 waits and 2
         * or more nests; a transition writes it back to zero */
        CHECK(!memcmp(base + u + SH_AI_UPDATE_FLAG_TEST, "\x8B\x40\x10\xC1\xE8\x19\xA8\x01", 8));
        CHECK(!memcmp(base + u + SH_AI_UPDATE_READ_MODE, "\x41\x8B\x84\x24\x70\x05\x00\x00\x85\xC0", 10));
        CHECK(!memcmp(base + u + SH_AI_UPDATE_READ_MODE + 0x10, "\x83\xF8\x02", 3));
        CHECK(!memcmp(base + u + SH_AI_UPDATE_RESET_MODE, "\x45\x89\xAC\x24\x70\x05\x00\x00", 8));
        CHECK(SH_AI_AW_INTERRUPT_BLEND == 0x570 && SH_AI_OFF_WEB_ANIMATOR == 0x222B8);
        CHECK(SH_AI_INTR_BLEND_NO == 0 && SH_AI_INTR_BLEND_YES == 1 && SH_AI_INTR_BLEND_MULTI == 2);
        printf("  interrupts: ForceState 0x%X, update 0x%X\n", f, u);
    }
    if (rva[SH_AI_SIG_MOVE_STAGE]) {
        /* the stage is the int at animFSM +0x2A9C: mov eax,[rdi+0x2A9C]; dec eax; cmp eax,0x12 */
        CHECK(window_has(base, rva[SH_AI_SIG_MOVE_STAGE], 0x50, "\x8B\x87\x9C\x2A\x00\x00\xFF\xC8\x83\xF8\x12", 11));
    }
    printf("  aiFireMode_t table %s, aiIdleTurn_t table %s\n",
           enum_table_found(base, size, image_base, SH_AI_FIREMODE_NAMES, SH_AI_FIREMODE_MAX + 1) ? "found" : "MISSING",
           enum_table_found(base, size, image_base, SH_AI_IDLETURN_NAMES, SH_AI_IDLETURN_MAX + 1) ? "found" : "MISSING");
    CHECK(enum_table_found(base, size, image_base, SH_AI_FIREMODE_NAMES, SH_AI_FIREMODE_MAX + 1));
    CHECK(enum_table_found(base, size, image_base, SH_AI_IDLETURN_NAMES, SH_AI_IDLETURN_MAX + 1));

    /* Offsets: Shared_MoveFailedAny reads *(ai+0x23198)+0xD20 against 5;
     * Shared_TriggerPulled reads ai+0x22278, weapon +0x8D4 and +0x8F8. */
    {
        uint32_t mfa = vk ? 0x5EF6F0u : 0x62CD00u, tp = vk ? 0x5F7BD0u : 0x5F6DF0u;
        CHECK(window_has(base, mfa, 0x90, "\x48\x8B\x86\x98\x31\x02\x00\x83\xB8\x20\x0D\x00\x00\x05", 14));
        CHECK(window_has(base, tp, 0x80, "\x49\x8B\x80\x78\x22\x02\x00", 7));
        CHECK(window_has(base, tp, 0x80, "\x48\x63\x81\xD4\x08\x00\x00\x39\xBC\x81\xF8\x08\x00\x00", 14));
    }
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("script_ai_sigs_test: no images given; skipped\n"); return 0; }
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "script_ai_sigs_test: FAIL\n" : "script_ai_sigs_test: PASS\n");
    return g_fail;
}
