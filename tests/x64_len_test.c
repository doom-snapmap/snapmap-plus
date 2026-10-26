/* x64 prologue length decoder checks.
 * Without arguments: hand-assembled encodings with known lengths and flags.
 * With a build 13954591 executable: resolve the four cs_dontuse signatures and
 * require the decoded stolen boundary, and the RIP-relative flag, that an
 * independent disassembler reports for each prologue. Run via run-tests.ps1. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x64_len.h"
#include "signatures.h"
#include "host_image.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

typedef struct { const char *what; const char *hex; size_t len; unsigned flags; } insn_case;

static size_t parse_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (*hex && n < cap) {
        unsigned v;
        while (*hex == ' ') hex++;
        if (!*hex || sscanf(hex, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return n;
}

static const insn_case CASES[] = {
    { "mov rax,rsp",                 "48 8B C4",                      3,  0 },
    { "sub rsp,imm32",               "48 81 EC E8 00 00 00",          7,  0 },
    { "movups xmm0,[rcx]",           "0F 10 01",                      3,  0 },
    { "movups xmm0,[rcx+0x10]",      "0F 10 41 10",                   4,  0 },
    { "lea r11,[rax]",               "4C 8D 18",                      3,  0 },
    { "movss xmm0,[rcx]",            "F3 0F 10 01",                   4,  0 },
    { "mov r10d,imm32",              "41 BA FF 00 00 00",             6,  0 },
    { "movss xmm1,[rip+disp]",       "F3 0F 10 0D EA DD D6 01",       8,  SH_X64_RIP_RELATIVE },
    { "mov [rsp+0x10],rbx",          "48 89 5C 24 10",                5,  0 },
    { "push rsi",                    "56",                            1,  0 },
    { "sub rsp,0x40",                "48 83 EC 40",                   4,  0 },
    { "mov eax,[rcx+0x20c]",         "8B 81 0C 02 00 00",             6,  0 },
    { "movzx esi,r8b",               "41 0F B6 F0",                   4,  0 },
    { "mov qword [rsp+0x20],-2",     "48 C7 44 24 20 FE FF FF FF",    9,  0 },
    { "lea rdx,[rip+disp]",          "48 8D 15 00 10 00 00",          7,  SH_X64_RIP_RELATIVE },
    { "mov rax,imm64",               "48 B8 01 02 03 04 05 06 07 08", 10, 0 },
    { "call rel32",                  "E8 00 00 00 00",                5,  SH_X64_BRANCH_REL },
    { "jmp rel8",                    "EB 05",                         2,  SH_X64_BRANCH_REL | SH_X64_LEAVES },
    { "jne rel32",                   "0F 85 10 00 00 00",             6,  SH_X64_BRANCH_REL },
    { "ret",                         "C3",                            1,  SH_X64_LEAVES },
    { "test cl,1",                   "F6 C1 01",                      3,  0 },
    { "test ecx,imm32",              "F7 C1 00 01 00 00",             6,  0 },
    { "mov word [rcx],imm16",        "66 C7 01 34 12",                5,  0 },
    { "shufps xmm0,xmm1,0",          "0F C6 C1 00",                   4,  0 },
    { "pshufb xmm0,xmm1",            "66 0F 38 00 C1",                5,  0 },
    { "roundss xmm0,xmm1,1",         "66 0F 3A 0A C1 01",             6,  0 },
    { "mov eax,[disp32] (SIB)",      "8B 04 25 00 10 00 00",          7,  0 },
    { "mov rax,gs:[0x30]",           "65 48 8B 04 25 30 00 00 00",    9,  0 },
    { "vmovaps (VEX, refused)",      "C5 F8 28 C1",                   0,  0 },
};

static void check_cases(void)
{
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint8_t code[16];
        unsigned flags = 0xFFu;
        size_t n = parse_hex(CASES[i].hex, code, sizeof code);
        size_t len = sh_x64_insn_len(code, n, &flags);
        if (len != CASES[i].len || (len && flags != CASES[i].flags)) {
            printf("FAIL %s: length %u flags 0x%x, expected %u 0x%x\n", CASES[i].what,
                   (unsigned)len, flags, (unsigned)CASES[i].len, CASES[i].flags);
            g_fail = 1;
        }
        if (n > 1) CHECK(sh_x64_insn_len(code, n - 1, &flags) == 0 || CASES[i].len == 0);
    }
    {
        /* A ret before the jump would fit means the function is too short. */
        uint8_t tiny[] = { 0x33, 0xC0, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                           0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
        CHECK(sh_x64_stolen_boundary(tiny, sizeof tiny, 14, 32, NULL) == 0);
    }
    {
        /* The boundary may not exceed max_len. */
        uint8_t movs[] = { 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8, 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8 };
        CHECK(sh_x64_stolen_boundary(movs, sizeof movs, 14, 19, NULL) == 0);
        CHECK(sh_x64_stolen_boundary(movs, sizeof movs, 14, 20, NULL) == 20);
    }
}

static uint8_t *map_pe_by_rva(const char *path)
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

/* Boundaries and RIP-relative flags from capstone on both 13954591 images. */
static const struct { const char *name; size_t boundary; int rip; } ALGO[] = {
    { "AlgoMatMul",    16, 0 },
    { "AlgoInverse",   17, 0 },
    { "AlgoPackRGBA",  18, 1 },
    { "AlgoCurveEval", 16, 0 },
};

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    uint8_t *base = map_pe_by_rva(path);
    const sh_build_identity *identity;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    printf("image %s: %s\n", path, identity->label);
    for (size_t a = 0; a < sizeof ALGO / sizeof ALGO[0]; a++) {
        const sig_entry *entry = NULL;
        sig_result r;
        unsigned flags = 0;
        size_t boundary;
        for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++)
            if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, ALGO[a].name)) entry = &BACKEND_ENGINE_SIGNATURES[i];
        if (!entry) { printf("FAIL %s missing from the signature DB\n", ALGO[a].name); g_fail = 1; continue; }
        if (sig_resolve_one(base, entry, &r) != SIG_OK || !r.addr) {
            printf("FAIL %s did not resolve\n", ALGO[a].name); g_fail = 1; continue;
        }
        boundary = sh_x64_stolen_boundary((const uint8_t *)r.addr, 32, 14, 32, &flags);
        printf("%-14s rva=0x%llx boundary=%u flags=0x%x\n", ALGO[a].name,
               (unsigned long long)(r.addr - (uintptr_t)base), (unsigned)boundary, flags);
        CHECK(boundary == ALGO[a].boundary);
        CHECK(((flags & SH_X64_RIP_RELATIVE) != 0) == ALGO[a].rip);
        CHECK((flags & SH_X64_BRANCH_REL) == 0);
    }
}

int main(int argc, char **argv)
{
    check_cases();
    for (int i = 1; i < argc; i++) check_image(argv[i]);
    printf(g_fail ? "x64_len_test: FAIL\n" : "x64_len_test: PASS\n");
    return g_fail;
}
