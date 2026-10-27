/* Native idFile / idFileSystem slots against the supported executables.
 * The backend calls native streams through SH_IDFILE_SLOT_* (overrides.c) and
 * the filesystem through the slots map_source.c decodes from
 * SnapMapReadSizeLimit. This test checks both against the engine's own code on
 * each image:
 *   - idFileSystemLocal (found through RTTI) slot +0xF8 is ReadFile; it opens
 *     through SH_PROVIDER_OPEN_SLOT and calls the returned stream's GetLength
 *     and Read at SH_IDFILE_SLOT_LENGTH and SH_IDFILE_SLOT_READ;
 *   - SnapMapReadSizeLimit's entry calls GetFileLength +0x180 and ReadFile
 *     +0xF8, where map_source.c reads them;
 *   - PublishedMapCacheReady makes the five virtual calls map_published.c
 *     binds from it (session -> lobby +0x348, lobby -> parameters +0x2B8,
 *     progress +0x30, map cache +0x38, cache set +0x190) and compares the
 *     launch parameters' item id +0x550 and revision +0x600;
 *   - FinalizeMapChange sets idCommonLocal's loading byte at
 *     SH_COMMON_LOADING_FLAG (decl_server.c and map_transition.c read it).
 * Run via run-tests.ps1 with -Doom / -DoomAlt. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x64_len.h"
#include "signatures.h"
#include "host_image.h"
#include "overrides.h"
#include "engine_build.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static uint8_t *map_pe_by_rva(const char *path, size_t *size, uint64_t *image_base)
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
    img = (uint8_t *)calloc(1, nt->OptionalHeader.SizeOfImage + 64);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz)
            memcpy(img + va, file + po, rs);
    }
    *size = nt->OptionalHeader.SizeOfImage;
    *image_base = nt->OptionalHeader.ImageBase;
    free(file);
    return img;
}

/* Primary vtable of an MSVC class: type descriptor name -> complete object
 * locator (signature 1, offset 0, self RVA) -> the qword that points at it. */
static uint32_t rtti_vtable(const uint8_t *img, size_t size, uint64_t image_base, const char *mangled)
{
    size_t n = strlen(mangled) + 1;
    for (size_t at = 0x10; at + n < size; at++) {
        uint32_t td;
        if (img[at] != '.' || memcmp(img + at, mangled, n)) continue;
        td = (uint32_t)(at - 0x10);
        for (size_t c = 12; c + 24 <= size; c += 4) {
            const uint32_t *col = (const uint32_t *)(img + c - 12);
            uint32_t col_rva = (uint32_t)(c - 12);
            uint64_t want;
            if (*(const uint32_t *)(img + c) != td) continue;
            if (col[0] != 1 || col[1] != 0 || col[5] != col_rva) continue;
            want = image_base + col_rva;
            for (size_t v = 0; v + 16 <= size; v += 8)
                if (*(const uint64_t *)(img + v) == want) return (uint32_t)(v + 8);
        }
    }
    return 0;
}

/* Collect the displacements of `call qword ptr [reg+disp]` (FF /2, memory form)
 * in a function body, decoding instruction by instruction until a ret+int3. */
static size_t call_slots(const uint8_t *code, size_t limit, uint32_t *out, size_t cap)
{
    size_t at = 0, count = 0;
    while (at < limit && count < cap) {
        unsigned flags = 0;
        size_t len = sh_x64_insn_len(code + at, limit - at, &flags), p = at;
        if (!len) break;
        if (code[p] >= 0x40 && code[p] <= 0x4f) p++;
        if (code[p] == 0xff && ((code[p + 1] >> 3) & 7) == 2) {
            uint8_t mod = code[p + 1] >> 6, rm = code[p + 1] & 7;
            size_t d = p + 2 + (rm == 4 ? 1 : 0);
            if (mod == 1) out[count++] = code[d];
            else if (mod == 2) out[count++] = *(const uint32_t *)(code + d);
        }
        if (code[at] == 0xc3 && code[at + 1] == 0xcc) break;
        at += len;
    }
    return count;
}

static int has(const uint32_t *v, size_t n, uint32_t x)
{
    for (size_t i = 0; i < n; i++) if (v[i] == x) return 1;
    return 0;
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    size_t size = 0, n;
    uint64_t image_base = 0;
    uint8_t *img = map_pe_by_rva(path, &size, &image_base);
    const sh_build_identity *identity;
    sig_result results[SIG_RESULTS_MAX];
    uint32_t vt, readfile, slots[64];
    const uint8_t *entry;
    int32_t disp;
    if (!img) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(img, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    printf("image %s: %s\n", path, identity->label);

    vt = rtti_vtable(img, size, image_base, ".?AVidFileSystemLocal@@");
    CHECK(vt != 0);
    if (vt) {
        readfile = (uint32_t)(*(const uint64_t *)(img + vt + 0xf8) - image_base);
        n = call_slots(img + readfile, 0x400, slots, 64);
        printf("idFileSystemLocal vtable 0x%x, ReadFile (+0xF8) 0x%x, %u indirect calls\n",
               vt, readfile, (unsigned)n);
        CHECK(has(slots, n, SH_PROVIDER_OPEN_SLOT));                 /* opens through +0x148 */
        CHECK(has(slots, n, SH_IDFILE_SLOT_LENGTH * 8u));            /* stream GetLength +0x68 */
        CHECK(has(slots, n, SH_IDFILE_SLOT_READ * 8u));              /* stream Read +0x30 */
        CHECK(!has(slots, n, 0x58));                                 /* not the 2018 GetLength */
        CHECK(SH_OVERRIDES_STREAM_SLOTS == SH_IDFILE_SLOTS);
    }

    (void)sig_resolve_all(img, results, SIG_RESULTS_MAX);
    {
        uintptr_t reader = sig_addr_by_name(results, sig_db_count(), "SnapMapReadSizeLimit");
        CHECK(reader != 0);
        if (reader) {
            entry = (const uint8_t *)reader - 0x43;
            CHECK(entry[0x3d] == 0xff && entry[0x3e] == 0x90);
            memcpy(&disp, entry + 0x3f, 4);
            printf("SnapMapReadSizeLimit: GetFileLength +0x%x", (unsigned)disp);
            CHECK(disp == 0x180);
            CHECK(entry[0x8d] == 0xff && entry[0x8e] == 0x90);
            memcpy(&disp, entry + 0x8f, 4);
            printf(", ReadFile +0x%x\n", (unsigned)disp);
            CHECK(disp == 0xf8);
        }
    }
    {
        uintptr_t ready = sig_addr_by_name(results, sig_db_count(), "PublishedMapCacheReady");
        static const struct { size_t at; const char *bytes; size_t n; } calls[] = {
            { 0x49, "\xff\x50\x30", 3 },                  /* progress +0x30 */
            { 0x56, "\xff\x90\x48\x03\x00\x00", 6 },      /* session -> lobby +0x348 (2018 +0x338) */
            { 0x62, "\xff\x92\xb8\x02\x00\x00", 6 },      /* lobby -> parameters +0x2B8 (2018 +0x2B0) */
            { 0xaa, "\x41\xff\x51\x38", 4 },              /* system -> cache +0x38 */
            { 0xba, "\x41\xff\x91\x90\x01\x00\x00", 7 },  /* cache set +0x190 */
        };
        CHECK(ready != 0);
        if (ready) {
            const uint8_t *r = (const uint8_t *)ready;
            for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++)
                CHECK(!memcmp(r + calls[i].at, calls[i].bytes, calls[i].n));
            /* The parameters are copied to rsp+0x30; item id and revision are
             * compared at rsp+0x580 and rsp+0x630, i.e. +0x550 and +0x600. */
            CHECK(!memcmp(r + 0x79, "\x48\x8d\x8c\x24\x80\x05\x00\x00", 8));
            CHECK(!memcmp(r + 0x8e, "\x48\x8d\x8c\x24\x30\x06\x00\x00", 8));
            printf("PublishedMapCacheReady: lobby +0x348, parameters +0x2b8, item +0x550, revision +0x600\n");
        }
    }
    {
        uintptr_t fin = sig_addr_by_name(results, sig_db_count(), "FinalizeMapChange");
        uint8_t want[7] = { 0xc6, 0x86, 0, 0, 0, 0, 0x01 };
        int found = 0;
        uint32_t off = SH_COMMON_LOADING_FLAG;
        memcpy(want + 2, &off, 4);
        CHECK(fin != 0);
        for (size_t i = 0; fin && i < 0x400 && !found; i++)
            found = !memcmp((const uint8_t *)fin + i, want, sizeof want);
        CHECK(found);
        if (found) printf("FinalizeMapChange sets common+0x%x\n", (unsigned)off);
    }
    free(img);
}

int main(int argc, char **argv)
{
    CHECK(SH_IDFILE_SLOT_READ == 6 && SH_IDFILE_SLOT_LENGTH == 13 && SH_IDFILE_SLOT_CLOSE == 0);
    for (int i = 1; i < argc; i++) check_image(argv[i]);
    if (argc < 2) printf("native_slots_test: no images given (needs -Doom/-DoomAlt)\n");
    printf(g_fail ? "native_slots_test: FAIL\n" : "native_slots_test: PASS\n");
    return g_fail;
}
