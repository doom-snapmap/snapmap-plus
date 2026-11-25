/* clip_events_sigs_test.c -- the engine sites clip events detours (src/backend/clip_events_sigs.c, clip_events.c) on
 * each build 13954591 executable named on the command line: each event lookup resolves at its recorded RVA with its
 * first call reaching its own table query, and every query pattern is unique (the filtered pair's shared pattern
 * matches exactly those two, each refused at the other's RVA). After the first query the lookup loads the parent at
 * +0x98, queries it the same way and merges through one function shared by all three; each detour window is whole
 * instructions with no RIP-relative operand or relative branch. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "signatures.h"
#include "host_image.h"
#include "x64_len.h"
#include "clip_events.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#include "image_map.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t parse(const char *p, uint8_t *bytes, uint8_t *mask, size_t cap)
{
    size_t n = 0;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (n >= cap) return 0;
        if (*p == '?') { bytes[n] = 0; mask[n++] = 0; p += p[1] == '?' ? 2 : 1; continue; }
        bytes[n] = (uint8_t)(hexval(p[0]) * 16 + hexval(p[1]));
        mask[n++] = 1;
        p += 2;
    }
    return n;
}

/* Every RVA in the executable sections where the pattern matches (up to cap). */
static int scan(const uint8_t *base, const char *pattern, uint32_t *hits, int cap)
{
    uint8_t bytes[256], mask[256];
    size_t n = parse(pattern, bytes, mask, sizeof bytes), k;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int i, found = 0;
    if (!n) return -1;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, size = sec[i].Misc.VirtualSize;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < n) continue;
        for (k = 0; k + n <= size; k++) {
            const uint8_t *at = base + va + k;
            size_t j;
            if (at[0] != bytes[0]) continue;
            for (j = 0; j < n && (!mask[j] || at[j] == bytes[j]); j++) {}
            if (j < n) continue;
            if (found < cap) hits[found] = (uint32_t)(va + k);
            found++;
        }
    }
    return found;
}

static uint32_t call_target(const uint8_t *base, uint32_t at)
{
    int32_t rel;
    if (base[at] != 0xE8) return 0;
    memcpy(&rel, base + at + 1, 4);
    return (uint32_t)(at + 5 + rel);
}

/* The next E8 at or after `from` within `len` bytes, or 0. */
static uint32_t next_call(const uint8_t *base, uint32_t from, uint32_t len)
{
    uint32_t at = from;
    while (at < from + len) {
        unsigned f = 0;
        size_t n = sh_x64_insn_len(base + at, 16, &f);
        if (!n) return 0;
        if (base[at] == 0xE8) return at;
        at += (uint32_t)n;
    }
    return 0;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint32_t size = 0, merge = 0, hits[4];
    uint8_t *base = test_image_read32(path, &size);
    const sh_build_identity *identity;
    int vk, i, n;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    vk = identity->vulkan;
    printf("image %s: %s\n", path, identity->label);

    for (i = 0; i < SH_CE_SITE_COUNT; i++) {
        const sh_ce_site_sig *s = &SH_CE_SITES[i];
        uint32_t rva = vk ? s->fn.rva_vk : s->fn.rva_gl, qrva = vk ? s->query.rva_vk : s->query.rva_gl;
        uint32_t at, second, mcall, w = 0;
        sig_result r;
        unsigned flags = 0;
        CHECK(sh_ce_site_resolve(base, s, &r) == SIG_OK && r.rva == rva);
        CHECK(call_target(base, rva + (uint32_t)s->call_offset) == qrva);
        /* the query is the only one of its pattern */
        n = scan(base, s->query.pattern, hits, 4);
        CHECK(n == 1 && hits[0] == qrva);
        /* the lookup's pattern: the window lookup alone; the filtered pair, both of them */
        n = scan(base, s->fn.pattern, hits, 4);
        if (i == SH_CE_SITE_WINDOW) CHECK(n == 1 && hits[0] == rva);
        else {
            uint32_t a = vk ? SH_CE_SITES[SH_CE_SITE_TYPE].fn.rva_vk : SH_CE_SITES[SH_CE_SITE_TYPE].fn.rva_gl;
            uint32_t b = vk ? SH_CE_SITES[SH_CE_SITE_TYPE2].fn.rva_vk : SH_CE_SITES[SH_CE_SITE_TYPE2].fn.rva_gl;
            CHECK(n == 2 && ((hits[0] == a && hits[1] == b) || (hits[0] == b && hits[1] == a)));
        }
        /* the walk: parent at +0x98 right after the first query, the same query on it, then the merge */
        at = rva + (uint32_t)s->call_offset + 5;
        CHECK(base[at] == 0x48 && base[at + 1] == 0x8B && base[at + 3] == 0x98 && !memcmp(base + at + 4, "\0\0\0", 3));
        second = next_call(base, at, 0x80);
        CHECK(second && call_target(base, second) == qrva);
        mcall = second ? next_call(base, second + 5, 0x20) : 0;
        CHECK(mcall != 0);
        if (mcall) {
            if (!merge) merge = call_target(base, mcall);
            CHECK(call_target(base, mcall) == merge);
        }
        /* the window */
        while (w < s->stolen) {
            size_t len = sh_x64_insn_len(base + rva + w, 32, &flags);
            if (!len) break;
            CHECK(!(flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL | SH_X64_LEAVES)));
            w += (uint32_t)len;
        }
        CHECK(w == s->stolen);
        printf("  %-18s rva=0x%X query=0x%X window=%u merge=0x%X\n", s->fn.name, rva, qrva, (unsigned)s->stolen, merge);
    }
    /* each filtered lookup checked at the other's address is refused */
    {
        sh_ce_site_sig swapped = SH_CE_SITES[SH_CE_SITE_TYPE];
        sig_result r;
        swapped.fn.rva_vk = SH_CE_SITES[SH_CE_SITE_TYPE2].fn.rva_vk;
        swapped.fn.rva_gl = SH_CE_SITES[SH_CE_SITE_TYPE2].fn.rva_gl;
        CHECK(sh_ce_site_resolve(base, &swapped, &r) == SIG_CHANGED && !r.addr);
        swapped = SH_CE_SITES[SH_CE_SITE_TYPE2];
        swapped.fn.rva_vk = SH_CE_SITES[SH_CE_SITE_TYPE].fn.rva_vk;
        swapped.fn.rva_gl = SH_CE_SITES[SH_CE_SITE_TYPE].fn.rva_gl;
        CHECK(sh_ce_site_resolve(base, &swapped, &r) == SIG_CHANGED && !r.addr);
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
    if (argc < 2) { printf("clip_events_sigs_test: no images given; skipped\n"); return 0; }
    /* each image in its own process: the host binding is the first image's */
    for (i = 1; i < argc; i++) {
        intptr_t rc;
        fflush(stdout);
        rc = _spawnl(_P_WAIT, argv[0], argv[0], "--one", argv[i], NULL);
        if (rc != 0) { printf("FAIL %s (exit %d)\n", argv[i], (int)rc); g_fail = 1; }
    }
    printf(g_fail ? "clip_events_sigs_test: FAIL\n" : "clip_events_sigs_test: PASS\n");
    return g_fail;
}
