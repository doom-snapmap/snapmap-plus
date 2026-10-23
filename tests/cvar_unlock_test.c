/* Cvar gate rewrite: synthetic sites prove exact matching, list-before-hash
 * order, idempotence, absent (retryable) versus ambiguous (refused) sites
 * without writes, and rollback after a failed write. With an unpacked
 * executable argument, the eleven sites must rewrite in its .text, one
 * immediate byte each. Run via run-tests.ps1. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cvar_unlock.h"
#include "patch.h"

void backend_log(const char *line) { (void)line; }
const uint8_t *sh_host_image_base(void) { return NULL; }

static int failures;
#define CHECK(test) do { if (!(test)) { printf("FAIL line %d: %s\n", __LINE__, #test); failures++; } } while (0)

/* The engine sequences; 0x100 marks a relative operand. */
typedef struct site { int bytes[28]; int len, imm, full; } site;
static const site SITES[SH_CVAR_UNLOCK_GATE_SITES] = {
    {{0xFF,0x50,0x10,0xB9,0x08,0,0,0,0x41,0xB9,0x20,0,0,0,0x85,0xC0,0x44,0x0F,0x44,0xC9}, 20, 10, 0x08},
    {{0xFF,0x50,0x10,0xB9,0x08,0,0,0,0x41,0xB8,0x20,0,0,0,0x85,0xC0,0x44,0x0F,0x44,0xC1,0x49,0x03}, 22, 10, 0x08},
    {{0xFF,0x50,0x10,0xB9,0x08,0,0,0,0xBE,0x20,0,0,0,0x85,0xC0,0x0F,0x44,0xF1,0x48,0x8B}, 20, 9, 0x08},
    {{0xFF,0x50,0x10,0xBA,0x38,0,0,0,0x41,0xBE,0x60,0,0,0,0x85,0xC0,0x44,0x0F,0x44,0xF2}, 20, 10, 0x38},
    {{0xFF,0x50,0x10,0xB9,0x38,0,0,0,0xBE,0x60,0,0,0,0x85,0xC0,0x0F,0x44,0xF1,0x48,0x8B}, 20, 9, 0x38},
    {{0x20,0xBA,0x08,0,0,0,0x85,0xC0,0x41,0xBE,0x20,0,0,0,0x48,0x63,0x46,0x1C,0x44,0x0F}, 20, 10, 0x08},
    {{0xFF,0x50,0x10,0xB9,0x08,0,0,0,0x41,0xB8,0x20,0,0,0,0x85,0xC0,0x44,0x0F,0x44,0xC1,0x49,0x8B,0x04,0x38,0x48,0x8B,0x04,0xD8}, 28, 10, 0x08},
    {{0xFF,0x51,0x10,0xB9,0x08,0,0,0,0x41,0xBE,0x20,0,0,0,0x85,0xC0,0x44,0x0F,0x44,0xF1}, 20, 10, 0x08},
    {{0x8B,0xFA,0x48,0x8B,0x01,0xFF,0x50,0x10,0xBE,0x20,0,0,0,0xB9,0x08,0,0,0,0x85,0xC0}, 20, 9, 0x08},
    {{0x00,0x48,0x8B,0x0D,0x100,0x100,0x100,0x100,0x48,0x8B,0x01,0xFF,0x50,0x10,0xBE,0x20,0,0,0,0xB9,0x08,0,0,0,0x85,0xC0}, 26, 15, 0x08},
    {{0xFF,0x50,0x10,0xB9,0x08,0,0,0,0x41,0xB8,0x20,0,0,0,0x85,0xC0,0x44,0x0F,0x44,0xC1,0x49,0x8B,0x04,0x38,0x48,0x8B,0x1C,0xD8}, 28, 10, 0x08},
};

#define TEXT_SIZE 4096
#define SITE_AT(i) (64 + (i) * 96)
static uint8_t *text;
static uint8_t *writes[64];
static int write_count;

static void observe(void *target) { if (write_count < 64) writes[write_count] = target; write_count++; }

static void place(uint8_t *at, int index, int relative)
{
    for (int k = 0; k < SITES[index].len; k++)
        at[k] = SITES[index].bytes[k] == 0x100 ? (uint8_t)(relative + k) : (uint8_t)SITES[index].bytes[k];
}

/* Lay out every site at its slot over int3 filler, then make the span code-like. */
static void layout(void)
{
    DWORD old;
    VirtualProtect(text, TEXT_SIZE, PAGE_READWRITE, &old);
    memset(text, 0xCC, TEXT_SIZE);
    for (int i = 0; i < SH_CVAR_UNLOCK_GATE_SITES; i++)
        place(text + SITE_AT(i), i, 0x31 + i);
    VirtualProtect(text, TEXT_SIZE, PAGE_EXECUTE_READ, &old);
}

static void edit(size_t at, uint8_t value)
{
    DWORD old;
    VirtualProtect(text, TEXT_SIZE, PAGE_READWRITE, &old);
    text[at] = value;
    VirtualProtect(text, TEXT_SIZE, PAGE_EXECUTE_READ, &old);
}

/* Add a second copy of a site to the laid-out span. */
static void copy_site(size_t at, int index)
{
    DWORD old;
    VirtualProtect(text, TEXT_SIZE, PAGE_READWRITE, &old);
    place(text + at, index, 0);
    VirtualProtect(text, TEXT_SIZE, PAGE_EXECUTE_READ, &old);
}

static int site_of(const uint8_t *target)
{
    for (int i = 0; i < SH_CVAR_UNLOCK_GATE_SITES; i++)
        if (target == text + SITE_AT(i) + SITES[i].imm) return i;
    return -1;
}

static int is_hash(int index) { return index == 3 || index == 4; }

static void synthetic(void)
{
    uint8_t before[TEXT_SIZE];
    unsigned unproven = 0;
    text = VirtualAlloc(NULL, TEXT_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!text) { failures++; return; }
    sh_patch_test_observe_write(observe);

    /* Every developer immediate becomes its full value, list selections first. */
    layout();
    memcpy(before, text, TEXT_SIZE);
    write_count = 0;
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, &unproven) == SH_CVAR_UNLOCK_GATE_SITES);
    CHECK(unproven == 0);
    CHECK(write_count == SH_CVAR_UNLOCK_GATE_SITES);
    for (int w = 0; w < write_count && w < 64; w++) {
        int index = site_of(writes[w]);
        CHECK(index >= 0);
        CHECK(is_hash(index) == (w >= SH_CVAR_UNLOCK_GATE_SITES - 2));
    }
    for (size_t at = 0; at < TEXT_SIZE; at++) {
        int index = site_of(text + at);
        CHECK(index < 0 ? text[at] == before[at] : text[at] == SITES[index].full);
    }

    /* A rewritten image is recognized and left alone. */
    write_count = 0;
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, NULL) == SH_CVAR_UNLOCK_GATE_SITES);
    CHECK(write_count == 0);

    /* A second copy of any site refuses the whole rewrite without a write. */
    layout();
    copy_site(TEXT_SIZE - 128, 0);
    memcpy(before, text, TEXT_SIZE);
    write_count = 0;
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, &unproven) == -1);
    CHECK(unproven == 1u);
    CHECK(write_count == 0 && !memcmp(before, text, TEXT_SIZE));

    /* So does one developer copy beside an already rewritten one. */
    layout();
    edit(SITE_AT(4) + SITES[4].imm, SITES[4].full);
    copy_site(TEXT_SIZE - 128, 4);
    write_count = 0;
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, &unproven) == -1);
    CHECK(unproven == 1u << 4);
    CHECK(write_count == 0);

    /* Or a rewritten copy beside two developer copies. */
    layout();
    edit(SITE_AT(2) + SITES[2].imm, SITES[2].full);
    copy_site(TEXT_SIZE - 128, 2);
    copy_site(TEXT_SIZE - 64, 2);
    write_count = 0;
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, &unproven) == -1);
    CHECK(unproven == 1u << 2);
    CHECK(write_count == 0);

    /* An unexpected immediate or a missing site is absent, which the worker
     * may retry while .text is still being decrypted. */
    layout();
    edit(SITE_AT(5) + SITES[5].imm, 0x21);
    write_count = 0;
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, &unproven) == 0);
    CHECK(unproven == 1u << 5);
    CHECK(write_count == 0);
    layout();
    edit(SITE_AT(9), 0x90);
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, &unproven) == 0);
    CHECK(unproven == 1u << 9);

    /* Ambiguity outranks absence: that build is never retried. */
    copy_site(TEXT_SIZE - 128, 3);
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, &unproven) == -1);
    CHECK(unproven == ((1u << 9) | (1u << 3)));

    /* A failed write undoes every earlier rewrite, hash selections first. */
    layout();
    memcpy(before, text, TEXT_SIZE);
    write_count = 0;
    sh_patch_test_faults(0, UINT64_C(1) << (SH_CVAR_UNLOCK_GATE_SITES - 1), 0);
    CHECK(sh_cvar_unlock_rewrite_gates(text, TEXT_SIZE, NULL) == -1);
    sh_patch_test_faults(0, 0, 0);
    CHECK(!memcmp(before, text, TEXT_SIZE));
    CHECK(write_count > SH_CVAR_UNLOCK_GATE_SITES && write_count <= 64);
    int last_hash = -1, first_list = -1;
    for (int w = SH_CVAR_UNLOCK_GATE_SITES; w < write_count && w < 64; w++) {
        int index = site_of(writes[w]);
        CHECK(index >= 0);
        if (index >= 0 && is_hash(index)) last_hash = w;
        else if (index >= 0 && first_list < 0) first_list = w;
    }
    CHECK(last_hash >= 0 && first_list > last_hash);
    sh_patch_test_observe_write(NULL);
    VirtualFree(text, 0, MEM_RELEASE);
}

/* Copy the unpacked image's .text into writable memory and rewrite it there. */
static void image(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *file = NULL, *copy = NULL, *before = NULL;
    if (!f) { printf("FAIL open %s\n", path); failures++; return; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(size);
    if (!file || fread(file, 1, size, f) != (size_t)size) { fclose(f); free(file); failures++; return; }
    fclose(f);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".text", 6)) continue;
        size_t length = sec[i].SizeOfRawData;
        copy = VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        before = (uint8_t *)malloc(length);
        if (!copy || !before) break;
        memcpy(copy, file + sec[i].PointerToRawData, length);
        memcpy(before, copy, length);
        CHECK(sh_cvar_unlock_rewrite_gates(copy, length, NULL) == SH_CVAR_UNLOCK_GATE_SITES);
        size_t changed = 0;
        for (size_t at = 0; at < length; at++) {
            if (copy[at] == before[at]) continue;
            changed++;
            CHECK((before[at] == 0x20 && copy[at] == 0x08) || (before[at] == 0x60 && copy[at] == 0x38));
        }
        CHECK(changed == SH_CVAR_UNLOCK_GATE_SITES);
        CHECK(sh_cvar_unlock_rewrite_gates(copy, length, NULL) == SH_CVAR_UNLOCK_GATE_SITES);
        printf("%s: %zu gate selections rewritten\n", path, changed);
        break;
    }
    CHECK(copy != NULL);
    if (copy) VirtualFree(copy, 0, MEM_RELEASE);
    free(before);
    free(file);
}

int main(int argc, char **argv)
{
    if (argc > 1) image(argv[1]);
    else synthetic();
    if (failures) { printf("cvar_unlock_test: %d FAILED\n", failures); return 1; }
    printf("cvar_unlock_test: PASS\n");
    return 0;
}
