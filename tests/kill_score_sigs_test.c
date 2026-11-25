/* kill_score_sigs_test.c -- checks the Player Score switch kill_score detours (kill_score_sigs.c, kill_score.c) on each
 * build 13954591 executable given on the command line: the signatures resolve uniquely at their recorded RVAs, both
 * detour windows are whole instructions (the switch's one relative operand is a short branch landing past the window),
 * and the switch's 18 stock spawn-type tests, its "Unsupported AI Type" end and the snapAIScore reads (rules list
 * +0x7930, count +0x7938, decl aiType +0x70) are intact. Run via run-tests.ps1 with -Doom / -DoomAlt. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "kill_score.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#include "image_map.h"

/* The function's body as the compiler laid it out: per test, the field offset of its mov eax,[rcx+disp32]. */
static int field_reads(const uint8_t *fn, size_t span)
{
    size_t at = 0;
    int reads = 0;
    while (at + 6 <= span) {
        if (fn[at] == 0x8B && fn[at + 1] == 0x81) {
            uint32_t disp;
            memcpy(&disp, fn + at + 2, 4);
            if (disp >= 0x78BC && disp <= 0x7900 && !((disp - 0x78BC) & 3)) reads++;
            at += 6;
        } else at++;
    }
    return reads;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint8_t *base = test_image_read(path, NULL, NULL);
    const sh_build_identity *identity;
    const sig_entry *s = &SH_KILL_SCORE_SIGNATURES[0];
    sig_result r, scan;
    uint32_t w = 0;
    int relative = 0;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; free(base); return; }
    printf("image %s: %s\n", path, identity->label);
    CHECK(SH_KILL_SCORE_SIGNATURES[SH_KS_LOOKUP].name != NULL && SH_KILL_SCORE_SIGNATURES[SH_KS_LOOKUP + 1].name == NULL);
    CHECK(sig_resolve_one(base, s, &r) == SIG_OK && r.rva == (identity->vulkan ? s->rva_vk : s->rva_gl));
    CHECK(sig_scan_one(base, s, &scan) == SIG_OK && scan.rva == r.rva);
    if (r.status == SIG_OK) {
        const uint8_t *fn = base + r.rva;
        while (w < SH_KS_STOLEN) {
            unsigned flags = 0;
            size_t len = sh_x64_insn_len(fn + w, 32, &flags);
            if (!len) break;
            CHECK(!(flags & (SH_X64_RIP_RELATIVE | SH_X64_LEAVES)));
            if (flags & SH_X64_BRANCH_REL) {
                CHECK(w == SH_KS_JCC_OFFSET && len == 2 && (fn[w] & 0xF0) == 0x70);
                CHECK(SH_KS_JCC_OFFSET + 2 + (int8_t)fn[w + 1] >= SH_KS_STOLEN);
                relative++;
            }
            w += (uint32_t)len;
        }
        CHECK(w == SH_KS_STOLEN && relative == 1);
        /* the 18 Player Score fields, one per stock spawn-type bit */
        CHECK(field_reads(fn, 0x140) == 18);
        printf("  %-24s rva=0x%X window=%u (short branch at +%u)\n", s->name, r.rva, (unsigned)SH_KS_STOLEN,
               (unsigned)SH_KS_JCC_OFFSET);
    }
    {
        const sig_entry *l = &SH_KILL_SCORE_SIGNATURES[SH_KS_LOOKUP];
        sig_result lr, ls;
        uint32_t lw = 0;
        CHECK(sig_resolve_one(base, l, &lr) == SIG_OK && lr.rva == (identity->vulkan ? l->rva_vk : l->rva_gl));
        CHECK(sig_scan_one(base, l, &ls) == SIG_OK && ls.rva == lr.rva);
        if (lr.status == SIG_OK) {
            const uint8_t *fn = base + lr.rva;
            uint32_t list, count;
            while (lw < SH_KS_LOOKUP_STOLEN) {
                unsigned flags = 0;
                size_t len = sh_x64_insn_len(fn + lw, 32, &flags);
                if (!len) break;
                CHECK(!(flags & (SH_X64_RIP_RELATIVE | SH_X64_LEAVES | SH_X64_BRANCH_REL)));
                lw += (uint32_t)len;
            }
            CHECK(lw == SH_KS_LOOKUP_STOLEN);
            memcpy(&count, fn + 0x12, 4);    /* movsxd rax, dword [rcx+0x7938] */
            memcpy(&list, fn + 0x26, 4);     /* mov rbx, [rcx+0x7930] */
            CHECK(count == 0x7938 && list == 0x7930 && fn[0x3B] == 0x70);   /* test [rcx+0x70], edx */
            printf("  %-24s rva=0x%X window=%u\n", l->name, lr.rva, (unsigned)SH_KS_LOOKUP_STOLEN);
        }
    }
    free(base);
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && strcmp(argv[1], "--one") == 0) {
        check_image(argv[2]);
        return g_fail;
    }
    if (argc < 2) { printf("kill_score_sigs_test: no images given; skipped\n"); return 0; }
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "kill_score_sigs_test: FAIL\n" : "kill_score_sigs_test: PASS\n");
    return g_fail;
}
