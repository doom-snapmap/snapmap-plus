/* Executable identity and the raw-RVA selection it gates.
 *
 *   build_identity_test                 offline checks only
 *   build_identity_test <image> ...     also hash each image; each must be supported
 *
 * Only the two build 13954591 executables are recognized; the 2018 executables
 * and anything else are not. A recorded Vulkan/OpenGL RVA pair is applied only
 * to an image bound to a supported identity, and only for its renderer. */
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_image.h"
#include "engine_build.h"
#include "signatures.h"

static void digest_of(const char *hex, unsigned char digest[32])
{
    for (int b = 0; b < 32; b++) {
        unsigned v = 0;
        sscanf_s(hex + 2 * b, "%2x", &v);
        digest[b] = (unsigned char)v;
    }
}

static void check_identities(void)
{
    unsigned char d[32];
    const sh_build_identity *vk, *gl;
    size_t count = 0;
    digest_of("3d8b4393649f80bdae85c19ae2c985c45ec9ac54f74ab04a18816e30ead63c21", d);
    vk = sh_host_identity_for_digest(d);
    digest_of("1bddecaa4e86850412f96e3117978bce1ec7bb701ae1b5a33120c21e4421e51e", d);
    gl = sh_host_identity_for_digest(d);
    assert(vk && vk->vulkan == 1 && gl && gl->vulkan == 0);
    while (SH_KNOWN_BUILDS[count].label) count++;
    assert(count == 2);
    /* The 2018 executables (shipped and unpacked) are not supported. */
    digest_of("139763e94f1a75b5310179f9eeeb8a949a1f53c49acbc722fcfc5dfe7bb6d323", d);
    assert(!sh_host_identity_for_digest(d));
    digest_of("d46ca967824da6ffc40f1ce0b6d7f4afa868fe12b795124d3e31a1bdb646a8e5", d);
    assert(!sh_host_identity_for_digest(d));
    digest_of("5ad2548ceab3dfa27f271e381222ca4a84dcac7cb83cce9446444c30c8309367", d);
    assert(!sh_host_identity_for_digest(d));
    digest_of("ffd0ba84ad5f296603927a576ac5ce6c683a3a6d8fafc26b50b28db2082abc03", d);
    assert(!sh_host_identity_for_digest(d));
    assert(!sh_host_identity_for_digest(NULL));
    puts("OK  only the two build 13954591 executables are recognized");
    {
        /* The one game-identity string every report carries. */
        char game[SH_GAME_IDENTITY_MAX];
        assert(sh_build_game_identity(vk, game, sizeof game) == 1);
        assert(!strcmp(game, "DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)"));
        assert(sh_build_game_identity(gl, game, sizeof game) == 1);
        assert(!strcmp(game, "DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, OpenGL, 1bddecaa)"));
        assert(strlen(game) < SH_GAME_IDENTITY_MAX);
        assert(sh_build_game_identity(NULL, game, sizeof game) == 0 && game[0] == '\0');
        assert(sh_build_game_identity(vk, NULL, 0) == 0);
        puts("OK  game identity strings");
    }
}

static void check_constants(void)
{
    assert(SH_IDCVAR_VALUE_INT == 0x30 && SH_IDCVAR_NAME == 0x50 && SH_IDCVAR_FLAGS == 0x68);
    assert(SH_IDFILE_SLOTS == 34 && SH_IDFILE_READ_STRING_SLOT == 0xF8 &&
           SH_IDFILE_COMPARE_SLOT == 0x100 && SH_IDFILE_WRITE_STRING_SLOT == 0x108);
    assert(SH_SESSION_DEVMODE_FIELD == 0x349BB && SH_VALIDATOR_REFLECT_SLOT == 0x258);
    assert(SH_GAME_SYSTEM_SLOT_RVA_VK == 0x2F48600u && SH_GAME_SYSTEM_SLOT_RVA_GL == 0x2F905D8u);
    /* Established by the build 13954591 address table. */
    assert(SH_CVARSYS_LAYOUT_ESTABLISHED && SH_IDCVAR_NAME_FOR_LOOKUP == 0x50);
    assert(SH_PROVIDER_OPEN_SLOT == 0x148);
    puts("OK  build 13954591 constants");
}

static void check_selection(void)
{
    static uint8_t image[0x2000];
    const sh_build_identity *vk = &SH_KNOWN_BUILDS[0], *gl = &SH_KNOWN_BUILDS[1];
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    /* The pattern is detoured at 0x1100 (prologue replaced by jmp rel32), tail intact. */
    sig_entry hooked = { "Hooked", "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 8B 91 18 36 02 00", 0x1100, 0 };
    sig_result r;
    assert(vk->vulkan && !gl->vulkan);

    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    nt = (IMAGE_NT_HEADERS64 *)(image + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = sizeof image;
    sec = IMAGE_FIRST_SECTION(nt);
    sec->VirtualAddress = 0x1000; sec->Misc.VirtualSize = 0x1000;
    sec->Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    memcpy(image + 0x1100, "\xE9\x11\x22\x33\x44\x57\x48\x83\xEC\x20\x8B\xDA\x48\x8B\xF9\x8B\x91\x18\x36\x02\x00", 21);

    /* An unbound image is unsupported: no RVA is ever applied. */
    sh_host_test_bind_identity(NULL, NULL);
    assert(!sh_host_image_is_supported(image) && sh_host_image_rva(image, 1, 2) == 0);
    assert(sh_engine_game_system_slot(image) == 0);
    assert(sig_resolve_one(image, &hooked, &r) == SIG_NOT_FOUND);

    /* Vulkan identity: the Vulkan RVA recovers the detour. */
    sh_host_test_bind_identity(image, vk);
    assert(sh_host_image_is_supported(image) && sh_host_image_rva(image, 1, 2) == 1);
    assert(sh_engine_game_system_slot(image) == (uintptr_t)image + 0x2F48600u);
    assert(sig_resolve_one(image, &hooked, &r) == SIG_OK_HOOKED && r.rva == 0x1100);

    /* OpenGL identity: its own (here absent) RVA applies, never the Vulkan one. */
    sh_host_test_bind_identity(image, gl);
    assert(sh_host_image_rva(image, 1, 2) == 2);
    assert(sh_engine_game_system_slot(image) == (uintptr_t)image + 0x2F905D8u);
    assert(sig_resolve_one(image, &hooked, &r) == SIG_NOT_FOUND);
    sh_host_test_bind_identity(NULL, NULL);
    puts("OK  recorded RVAs apply only to a supported image, per renderer");
}

static int check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    static uint8_t probe[16];
    const sh_build_identity *id;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    id = sh_host_test_bind_image(probe, wide);
    if (!id || sh_host_image_identity(probe) != id) {
        printf("BAD %s is not a supported executable (build 13954591)\n", path);
        sh_host_test_bind_image(NULL, NULL);
        return 1;
    }
    sh_host_test_bind_image(NULL, NULL);
    printf("OK  %s -> %s\n", path, id->label);
    return 0;
}

int main(int argc, char **argv)
{
    int bad = 0;
    check_identities();
    check_constants();
    check_selection();
    for (int i = 1; i < argc; i++) bad += check_image(argv[i]);
    printf("build_identity_test %s\n", bad ? "FAILED" : "OK");
    return bad ? 1 : 0;
}
