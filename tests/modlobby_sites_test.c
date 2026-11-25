/* modlobby_sites_test.c -- the co-op lobby's bind checks (sh_modlobby_check_sites) against each build 13954591
 * executable given on the command line: the six lobby signatures resolve at their recorded RVAs, every byte check
 * holds on the executable's own code (also with the four detours' stolen windows written over), and a changed byte at
 * the controller's lobby-parameters call is refused and named. Run via run-tests.ps1 with -Doom / -DoomAlt. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "modlobby.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static uint8_t *map_pe_by_rva(const char *path)
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

static const uint8_t *site(const uint8_t *base, const sig_result *results, size_t n, const char *name, int vulkan)
{
    const sig_result *r = sig_lookup(results, n, name);
    const sig_entry *e = NULL;
    size_t i;
    for (i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++)
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name)) { e = &BACKEND_ENGINE_SIGNATURES[i]; break; }
    CHECK(e != NULL && r != NULL);
    if (!e || !r) return NULL;
    CHECK(r->status == SIG_OK && r->rva == (vulkan ? e->rva_vk : e->rva_gl));
    if (r->status != SIG_OK) { printf("  %s: %s\n", name, sig_status_reason(r->status)); return NULL; }
    return base + r->rva;
}

/* What a detour leaves in its window: a jump, then the window's remaining bytes as the hook pads them. */
static void write_over(uint8_t *at, size_t stolen)
{
    memset(at, 0xCC, stolen);
    at[0] = 0xFF; at[1] = 0x25; memset(at + 2, 0, 4);   /* jmp [rip+0] */
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint8_t *base = map_pe_by_rva(path);
    const sh_build_identity *identity;
    sig_result *results;
    size_t n, cap = sig_db_count();
    sh_modlobby_sites s;
    void **session = NULL;
    const void *acting = NULL;
    const char *failed;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; free(base); return; }
    printf("image %s: %s\n", path, identity->label);
    results = (sig_result *)calloc(cap, sizeof *results);
    if (!results) { g_fail = 1; free(base); return; }
    n = sig_resolve_all(base, results, cap);
    s.try_start = site(base, results, n, "SnapmapLobbyTryToStartMatch", identity->vulkan);
    s.controller = site(base, results, n, "LobbyControllerStartMatch", identity->vulkan);
    s.ready = site(base, results, n, "LobbySetUserReady", identity->vulkan);
    s.peer = site(base, results, n, "SessionStateGameLobbyPeer", identity->vulkan);
    s.send = site(base, results, n, "SnapmapLobbySendReadyCommand", identity->vulkan);
    s.confirm = site(base, results, n, "LobbyPeerConfirmsMap", identity->vulkan);
    if (s.try_start && s.controller && s.ready && s.peer && s.send && s.confirm) {
        uint32_t text_end = ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
        failed = sh_modlobby_check_sites(&s, &session, &acting);
        printf("  the lobby's checks on the executable's code: %s\n", failed ? failed : "all hold");
        CHECK(failed == NULL);
        CHECK(session && (const uint8_t *)session > base && (const uint8_t *)session < base + text_end);
        CHECK(acting && (const uint8_t *)acting > base && (const uint8_t *)acting < base + text_end);
        /* The controller reads the session through the same global again before its lobby call (+0xEC). */
        {
            int32_t d;
            memcpy(&d, s.controller + 0xEF, 4);
            CHECK(!memcmp(s.controller + 0xEC, "\x48\x8b\x0d", 3) && (void *)(s.controller + 0xF3 + d) == (void *)session);
        }
        /* The four detours in place (modlobby.c sh_modlobby_install's windows: 16, 16, 18, 16 bytes). */
        write_over((uint8_t *)s.try_start, 16);
        write_over((uint8_t *)s.controller, 16);
        write_over((uint8_t *)s.ready, 18);
        write_over((uint8_t *)s.peer, 16);
        failed = sh_modlobby_check_sites(&s, NULL, NULL);
        printf("  with the four detours written: %s\n", failed ? failed : "all hold");
        CHECK(failed == NULL);
        /* The lobby-parameters call is pinned to its slot and its receiver: the game lobby the +0x210 call gave. */
        {
            uint8_t *slot = (uint8_t *)s.controller + 0x104, *receiver = (uint8_t *)s.controller + 0xFF;
            uint8_t keep = *slot, keep_receiver = *receiver;
            *slot = 0xB0;   /* +0x2B0, another slot */
            failed = sh_modlobby_check_sites(&s, NULL, NULL);
            CHECK(failed && strstr(failed, "controller +0xF6"));
            *slot = keep;
            *receiver = 0x49;   /* mov rdx, [r8]-style: not the lobby the +0x210 call returned */
            failed = sh_modlobby_check_sites(&s, NULL, NULL);
            CHECK(failed && strstr(failed, "controller +0xF6"));
            *receiver = keep_receiver;
            CHECK(sh_modlobby_check_sites(&s, NULL, NULL) == NULL);
        }
    }
    free(results);
    (void)sh_host_test_bind_image(NULL, NULL);
    free(base);
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("modlobby_sites_test: no images given; skipped\n"); return 0; }
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "modlobby_sites_test: FAIL\n" : "modlobby_sites_test: PASS\n");
    return g_fail;
}
