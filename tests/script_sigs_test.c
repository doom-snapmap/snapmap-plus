/* script_sigs_test.c -- the engine sites the mod-script hooks depend on
 * (src/backend/script/script_sigs.c, script_hooks.c), checked on each build
 * 13954591 executable given on the command line:
 *   - every private signature resolves to one match at its recorded RVA;
 *   - the OnAnimNotify dispatcher's bytes match two functions, and the
 *     callback-name check picks the recorded one;
 *   - each detour window is whole instructions with no RIP-relative operand
 *     or relative branch, and the tick's window is the bytes the installer
 *     compares;
 *   - the tick's only direct caller is idGameLocal::RunFrame, which is the
 *     idGameLocal vtable's slot +0x08;
 *   - idGameLocal vtable +0x7F0 is the spawn-id lookup and +0x7F8 the lookup
 *     followed by the idEntity cast (the handle scheme's resolver);
 *   - the main_thread_id and load_screen globals resolve.
 * Run via run-tests.ps1 with -Doom / -DoomAlt. Each image is checked in its own
 * process because glb_resolve caches the first image's globals. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "engine_globals.h"
#include "x64_len.h"
#include "script/script_hooks.h"

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

typedef struct site { int sig; size_t stolen; } site;

/* Direct E8 calls to callee_rva, and how many lie in [from, to). */
static int count_calls(const uint8_t *base, uint32_t size, uint32_t callee_rva, uint32_t from, uint32_t to, int *inside)
{
    int n = 0;
    *inside = 0;
    for (uint32_t i = 0; i + 5 <= size; i++) {
        int32_t rel;
        if (base[i] != 0xE8) continue;
        memcpy(&rel, base + i + 1, 4);
        if ((uint32_t)(i + 5 + rel) != callee_rva) continue;
        n++;
        if (i >= from && i < to) (*inside)++;
    }
    return n;
}

static uint64_t qword_at(const uint8_t *base, uint32_t rva)
{
    uint64_t v;
    memcpy(&v, base + rva, 8);
    return v;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, rva[SH_SCRIPT_SIG_COUNT];
    uint8_t *base = map_pe_by_rva(path, &size);
    const sh_build_identity *identity;
    IMAGE_NT_HEADERS *nt;
    uint64_t image_base;
    int vk, i;
    static const site windows[] = {
        { SH_SCRIPT_SIG_TICK, SH_SCRIPT_TICK_STOLEN },
        { SH_SCRIPT_SIG_ON_DAMAGE, SH_SCRIPT_ON_DAMAGE_STOLEN },
        { SH_SCRIPT_SIG_KILLED, SH_SCRIPT_KILLED_STOLEN },
        { SH_SCRIPT_SIG_RELAY_DOACTION, SH_SCRIPT_RELAY_STOLEN },
    };

    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    vk = identity->vulkan;
    printf("image %s: %s\n", path, identity->label);
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    image_base = nt->OptionalHeader.ImageBase;

    /* Every entry of the table, in enum order, unique at its recorded RVA. */
    for (i = 0; SH_SCRIPT_SIGNATURES[i].name; i++) {}
    CHECK(i == SH_SCRIPT_SIG_COUNT);
    for (i = 0; i < SH_SCRIPT_SIG_COUNT; i++) {
        const sig_entry *e = &SH_SCRIPT_SIGNATURES[i];
        sig_result r;
        rva[i] = 0;
        if (sig_resolve_one(base, e, &r) != SIG_OK || !r.addr) {
            printf("FAIL %s did not resolve to a unique match\n", e->name);
            g_fail = 1;
            continue;
        }
        CHECK(r.rva == (vk ? e->rva_vk : e->rva_gl));
        rva[i] = r.rva;
        printf("  %-22s rva=0x%X\n", e->name, r.rva);
    }

    /* OnAnimNotify: the plain pattern is ambiguous (OnMoverNotify shares it);
     * the callback name settles it. */
    {
        const sh_script_named_sig *s = &SH_SCRIPT_ANIM_NOTIFY_SIGNATURE;
        sh_script_named_sig mover = *s;
        sig_result r;
        CHECK(sig_resolve_one(base, &s->sig, &r) == SIG_AMBIGUOUS);
        CHECK(sh_script_resolve_named(base, s, &r) == SIG_OK);
        CHECK(r.rva == (vk ? s->sig.rva_vk : s->sig.rva_gl));
        printf("  %-22s rva=0x%X (by its callback name)\n", s->sig.name, r.rva);
        if (r.status == SIG_OK) {
            unsigned flags = 0;
            CHECK(sh_x64_stolen_boundary(base + r.rva, 64, 14, 48, &flags) == SH_SCRIPT_ON_ANIM_STOLEN);
            CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
        }
        mover.callback = "OnMoverNotify";
        CHECK(sh_script_resolve_named(base, &mover, &r) == SIG_OK);
        CHECK(r.rva == (vk ? 0x828990u : 0x827B50u));
        mover.callback = "NoSuchCallback";
        CHECK(sh_script_resolve_named(base, &mover, &r) == SIG_NOT_FOUND);
    }

    /* Detour windows: whole, position-independent instructions. */
    for (i = 0; i < (int)(sizeof windows / sizeof windows[0]); i++) {
        unsigned flags = 0;
        uint32_t at = rva[windows[i].sig];
        size_t b;
        if (!at) continue;
        b = sh_x64_stolen_boundary(base + at, 64, 14, 48, &flags);
        if (b != windows[i].stolen)
            printf("  %s: first boundary at or past 14 is %u, window %u\n", SH_SCRIPT_SIGNATURES[windows[i].sig].name,
                   (unsigned)b, (unsigned)windows[i].stolen);
        CHECK(b == windows[i].stolen);
        CHECK((flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)) == 0);
    }
    if (rva[SH_SCRIPT_SIG_TICK])
        CHECK(memcmp(base + rva[SH_SCRIPT_SIG_TICK], k_script_tick_prologue, SH_SCRIPT_TICK_STOLEN) == 0);

    /* The tick's only direct caller is RunFrame, the game vtable's slot +0x08;
     * the handle resolver's slots. */
    {
        uint32_t runframe = vk ? 0x3AE730u : 0x3ADE50u, runframe_size = 13865;
        uint32_t game_vt = vk ? 0x201E3D8u : 0x20381A8u;
        uint32_t lookup_cast = vk ? 0x3912A0u : 0x390B70u;
        int inside = 0, calls;
        if (rva[SH_SCRIPT_SIG_TICK]) {
            calls = count_calls(base, size, rva[SH_SCRIPT_SIG_TICK], runframe, runframe + runframe_size, &inside);
            printf("  tick direct calls: %d (%d inside RunFrame)\n", calls, inside);
            CHECK(calls == 1 && inside == 1);
        }
        CHECK(qword_at(base, game_vt + 0x08) == image_base + runframe);
        if (rva[SH_SCRIPT_SIG_SPAWN_ID_LOOKUP])
            CHECK(qword_at(base, game_vt + SH_SCRIPT_GAME_LOOKUP_SLOT) == image_base + rva[SH_SCRIPT_SIG_SPAWN_ID_LOOKUP]);
        CHECK(qword_at(base, game_vt + SH_SCRIPT_GAME_LOOKUP_SLOT + 8) == image_base + lookup_cast);
        /* The wrapper: sub rsp,28; mov rax,[rcx]; call [rax+0x7F0]; ...; jmp the cast. */
        CHECK(!memcmp(base + lookup_cast, "\x48\x83\xEC\x28\x48\x8B\x01\xFF\x90\xF0\x07\x00\x00", 13));
        /* GetSpawnId (VK 0x140394100, GL 0x140393940): compares idEntity +0x08 with
         * the world's number 0x3FFE and loads idGameLocal +0x653F8 + 4 * number. */
        {
            uint32_t getid = vk ? 0x394100u : 0x393940u, k;
            int table = 0, world = 0;
            for (k = getid; k < getid + 0x40; k++) {
                if (!memcmp(base + k, "\xF8\x53\x06\x00", 4)) table = 1;
                if (!memcmp(base + k, "\xFE\x3F\x00\x00", 4)) world = 1;
            }
            CHECK(table && world);
        }
    }

    CHECK(glb_resolve(base, "main_thread_id", NULL) != 0);
    CHECK(glb_resolve(base, "load_screen", NULL) != 0);
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("script_sigs_test: no images given; skipped\n"); return 0; }
    for (int i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "script_sigs_test: FAIL\n" : "script_sigs_test: PASS\n");
    return g_fail;
}
