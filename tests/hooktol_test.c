/* hooktol_test.c -- offline validation of the resolver's HOOK-TOLERANT known_rva fallback.
 *
 * Builds an in-memory RVA-mapped image of the unpacked DOOM (like sig_test.c), then SYNTHESIZES the
 * exact live condition that defeats the masked-byte scan: it overwrites a few functions' PROLOGUES with
 * an inline detour (E9 rel32 jmp) -- mimicking an external instrumentation tool's hooks on DeserializeFromJson /
 * AddCommand / MenuPump. With the prologue clobbered the masked scan can no longer find those fns by
 * pattern; the test asserts the resolver still resolves them via the known_rva fallback as SIG_OK_HOOKED
 * (with the right RVA), and that every UN-hooked fn still resolves cleanly as SIG_OK. NOT shipped.
 *
 *   cl /nologo /O2 /MT hooktol_test.c signatures.c /Fe:hooktol_test.exe
 *   hooktol_test.exe <DOOM_unpacked.exe>      # exit 0 iff hook-tolerance behaves
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "signatures.h"

static uint8_t *map_pe_by_rva(const char *path, size_t *image_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)file;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    uint32_t hdr_size   = nt->OptionalHeader.SizeOfHeaders;
    uint8_t *img = (uint8_t *)calloc(1, image_size);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, hdr_size);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= image_size && po + rs <= (uint32_t)fsz)
            memcpy(img + va, file + po, rs);
    }
    free(file);
    *image_sz = image_size;
    return img;
}

/* Overwrite the first `stolen` bytes at `rva` with an E9 rel32 jmp + 0xCC fill (a plausible detour). */
static void hook_prologue(uint8_t *img, uint32_t rva, int stolen)
{
    img[rva + 0] = 0xE9;
    img[rva + 1] = 0x11; img[rva + 2] = 0x22; img[rva + 3] = 0x33; img[rva + 4] = 0x44;  /* rel32 */
    for (int i = 5; i < stolen; i++) img[rva + i] = 0xCC;
}

static const sig_entry *find_sig(const char *name)
{
    for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++)
        if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name) == 0)
            return &BACKEND_ENGINE_SIGNATURES[i];
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: hooktol_test <DOOM_unpacked.exe>\n"); return 2; }
    size_t image_sz = 0;
    uint8_t *base = map_pe_by_rva(argv[1], &image_sz);
    if (!base) return 2;

    /* The three functions an external instrumentation tool would hook -- clobber their prologues here. Steal
     * widths span the realistic detour sizes (E9 rel32 = 5; a wider whole-instruction window = 8). Each
     * stays within the sig's fixed length so a matchable tail remains -- the resolver's premise is that a
     * hooked-but-present fn keeps >=HOOK_MIN_TAIL fixed bytes past the stolen window (true for these
     * sigs; MenuPump's 11-byte pattern keeps exactly 6 fixed bytes past a 5-byte steal). A detour that
     * over-clobbers a fn's ENTIRE fixed length cannot be tail-recovered -- a real but accepted limit for
     * ultra-short sigs (a documented limitation). */
    struct { const char *name; int stolen; } hooks[] = {
        { "DeserializeFromJson", 5 },   /* exactly the E9 rel32 width */
        { "AddCommand",          8 },   /* a wider whole-instruction steal (34-byte sig, long tail) */
        { "MenuPump",            5 },   /* short 11-byte sig: 5-byte steal leaves 6 fixed tail bytes */
    };
    int NH = (int)(sizeof hooks / sizeof hooks[0]);
    for (int i = 0; i < NH; i++) {
        const sig_entry *s = find_sig(hooks[i].name);
        if (!s) { printf("BAD: sig %s not in DB\n", hooks[i].name); free(base); return 1; }
        hook_prologue(base, s->known_rva, hooks[i].stolen);
    }

    size_t total = sig_db_count();
    sig_result results[64];
    size_t ok = sig_resolve_all(base, results, 64);

    int fail = 0, hooked = 0;
    for (size_t i = 0; i < total; i++) {
        const char *name = BACKEND_ENGINE_SIGNATURES[i].name;
        int should_be_hooked = 0;
        for (int h = 0; h < NH; h++) if (strcmp(name, hooks[h].name) == 0) should_be_hooked = 1;

        if (should_be_hooked) {
            int good = (results[i].status == SIG_OK_HOOKED &&
                        results[i].rva == BACKEND_ENGINE_SIGNATURES[i].known_rva);
            if (good) hooked++; else fail++;
            printf("%s %-20s status=%d rva=0x%-9x (expected SIG_OK_HOOKED@known)\n",
                   good ? "OK " : "BAD", name, (int)results[i].status, results[i].rva);
        } else {
            int good = (results[i].status == SIG_OK &&
                        results[i].rva == BACKEND_ENGINE_SIGNATURES[i].known_rva);
            if (!good) {
                fail++;
                printf("BAD %-20s status=%d rva=0x%-9x (expected clean SIG_OK@known)\n",
                       name, (int)results[i].status, results[i].rva);
            }
        }
    }
    printf("======================================================================\n");
    printf("resolved=%zu/%zu  hook-tolerant=%d/%d  failures=%d\n", ok, total, hooked, NH, fail);
    free(base);
    /* Pass iff all sigs resolved, all 3 hooked ones came back SIG_OK_HOOKED, and nothing else broke. */
    return (ok == total && hooked == NH && fail == 0) ? 0 : 1;
}
