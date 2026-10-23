/* raw_deflate_test.c -- the table-driven Huffman path decodes exactly what the
 * bitwise reference decodes: same bytes, same results, same refusals, and no
 * read past the source slice. Optional argv[1]: a DOOM base directory whose
 * compressed resource rows are decoded both ways. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/raw_deflate.h"
#include "../src/backend/raw_deflate_encode.h"
#include "../src/backend/resource_catalog.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned g_seed = 12345u;
static unsigned rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }

/* One decode in each mode through each entry point; results and the whole
 * destination buffer must agree. Returns the exact-length result. */
static size_t same_both_ways(const unsigned char *src, size_t len, size_t out_len, const char *what)
{
    size_t caps[4] = {out_len, out_len + 1, out_len ? out_len - 1 : 0, out_len + 64};
    size_t exact_result = 0;
    for (int c = 0; c < 4; c++) {
        size_t cap = caps[c];
        unsigned char *a = calloc(cap + 1, 1), *b = calloc(cap + 1, 1);
        if (!a || !b) { CHECK(0, "allocation"); free(a); free(b); return 0; }
        sh_inflate_test_bitwise(1);
        size_t ra = c ? sh_inflate_raw_upto(src, len, a, cap) : sh_inflate_raw(src, len, a, cap);
        int va = c ? 0 : sh_inflate_raw_exact(src, len, a, cap);
        sh_inflate_test_bitwise(0);
        size_t rb = c ? sh_inflate_raw_upto(src, len, b, cap) : sh_inflate_raw(src, len, b, cap);
        int vb = c ? 0 : sh_inflate_raw_exact(src, len, b, cap);
        CHECK(ra == rb && va == vb, "%s cap %zu: bitwise %zu/%d, table %zu/%d", what, cap, ra, va, rb, vb);
        CHECK(!memcmp(a, b, cap + 1), "%s cap %zu: destination bytes differ", what, cap);
        if (!c) exact_result = rb;
        free(a); free(b);
    }
    return exact_result;
}

/* The source ends exactly at a no-access page: any read past len faults. */
static unsigned char *g_guard_base;
static size_t g_guard_size;
static const unsigned char *at_guard(const unsigned char *src, size_t len)
{
    SYSTEM_INFO info; GetSystemInfo(&info);
    size_t page = info.dwPageSize, data = (len + page - 1) / page * page;
    if (g_guard_base) VirtualFree(g_guard_base, 0, MEM_RELEASE);
    g_guard_size = data + page;
    g_guard_base = VirtualAlloc(NULL, g_guard_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_guard_base) return NULL;
    DWORD old; VirtualProtect(g_guard_base + data, page, PAGE_NOACCESS, &old);
    unsigned char *start = g_guard_base + data - len;
    memcpy(start, src, len);
    return start;
}

static void stream_cases(const unsigned char *input, size_t length, const char *what)
{
    size_t z = 0;
    unsigned char *stream = sh_deflate_raw(input, length, &z);
    CHECK(stream && z, "%s: encode", what);
    if (!stream) return;
    size_t got = same_both_ways(stream, z, length, what);
    unsigned char *check = malloc(length + 1);
    CHECK(check && sh_inflate_raw(stream, z, check, length) == length && (!length || !memcmp(check, input, length)),
        "%s: table path does not reproduce the input (%zu of %zu)", what, got, length);
    free(check);
    /* Guarded: valid and every truncation never read beyond the slice. */
    for (size_t cut = 0; cut <= z; cut += cut < 64 || z - cut < 64 ? 1 : (z / 97 + 1)) {
        const unsigned char *guarded = at_guard(stream, cut);
        if (!guarded) { CHECK(0, "guard allocation"); break; }
        char label[128]; snprintf(label, sizeof(label), "%s truncated to %zu", what, cut);
        same_both_ways(guarded, cut, length, label);
    }
    /* Bit flips, byte replacements and trailing bytes refuse or decode alike. */
    unsigned char *mutated = malloc(z + 2);
    for (int m = 0; mutated && m < 300; m++) {
        memcpy(mutated, stream, z);
        size_t at = rnd() % z, mlen = z;
        if (m % 3 == 0) mutated[at] ^= (unsigned char)(1u << (rnd() % 8));
        else if (m % 3 == 1) mutated[at] = (unsigned char)rnd();
        else { mutated[z] = (unsigned char)rnd(); mlen = z + 1; }
        char label[128]; snprintf(label, sizeof(label), "%s mutation %d at %zu", what, m, at);
        const unsigned char *guarded = at_guard(mutated, mlen);
        if (guarded) same_both_ways(guarded, mlen, length, label);
    }
    free(mutated);
    free(stream);
}

/* Stored, Doom's Z_SYNC_FLUSH form and fixed-Huffman blocks built by hand. */
static void hand_built(void)
{
    static const unsigned char stored[] = {0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c'};
    static const unsigned char sync[] = {0x00, 0x03, 0x00, 0xfc, 0xff, 'x', 'y', 'z', 0x00, 0x00, 0x00, 0xff, 0xff};
    /* Fixed block: literals 'a','b', length 3 distance 2 ("aba"), end of block. */
    static const unsigned char fixed[] = {0x4b, 0x4c, 0x02, 0x42, 0x00};
    static const unsigned char padded[] = {0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c', 0x00};
    unsigned char out[16];
    CHECK(same_both_ways(stored, sizeof(stored), 3, "stored") == 3, "stored block");
    CHECK(same_both_ways(sync, sizeof(sync), 3, "sync flush") == 3, "sync flush fragment");
    size_t f = same_both_ways(fixed, sizeof(fixed), 5, "fixed");
    CHECK(f == 5 && sh_inflate_raw(fixed, sizeof(fixed), out, 5) == 5 && !memcmp(out, "ababa", 5),
        "fixed block back-reference (%zu)", f);
    CHECK(!same_both_ways(padded, sizeof(padded), 3, "trailing byte"), "trailing byte refused");
}

static void real_rows(const char *base)
{
    char error[512] = "";
    sh_resource_catalog *catalog = sh_resource_catalog_open(base, error, sizeof(error));
    CHECK(catalog != NULL, "catalog open: %s", error);
    if (!catalog) return;
    size_t n = sh_resource_catalog_count(catalog), rows = 0;
    unsigned long long bytes = 0;
    g_seed = 7u;
    for (size_t k = 0; k < 400000 && rows < 1500; k++) {
        size_t i = ((size_t)rnd() * 4099u + rnd()) % n;
        const sh_resource_catalog_entry *row = sh_resource_catalog_at(catalog, i);
        if (!row->size || row->size == row->stored_size || row->size > (16u << 20)) continue;
        unsigned char *a = NULL, *b = NULL; size_t la = 0, lb = 0;
        sh_inflate_test_bitwise(1);
        int ra = sh_resource_catalog_read_entry(catalog, i, &a, &la);
        sh_inflate_test_bitwise(0);
        int rb = sh_resource_catalog_read_entry(catalog, i, &b, &lb);
        CHECK(ra == rb && la == lb && (!ra || !memcmp(a, b, la)), "row %zu %s differs", i, row->path);
        free(a); free(b);
        rows++; bytes += row->size;
    }
    printf("  real rows: %zu compressed rows, %.1f MB decoded both ways\n", rows, bytes / 1048576.0);
    sh_resource_catalog_close(catalog);
}

int main(int argc, char **argv)
{
    hand_built();
    size_t sizes[] = {0, 1, 2, 3, 17, 258, 259, 1000, 32768, 32769, 70000, 300000};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t n = sizes[s];
        unsigned char *data = malloc(n + 1);
        char what[64];
        for (int kind = 0; data && kind < 4; kind++) {
            for (size_t i = 0; i < n; i++) data[i] = kind == 0 ? 0 : kind == 1 ? (unsigned char)rnd() :
                kind == 2 ? (unsigned char)("generated/decls/entitydef/"[i % 26]) : (unsigned char)((i * 7 / 13) ^ (rnd() % 3));
            snprintf(what, sizeof(what), "size %zu kind %d", n, kind);
            stream_cases(data, n, what);
        }
        free(data);
    }
    if (argc > 1 && argv[1][0]) real_rows(argv[1]);
    if (g_guard_base) VirtualFree(g_guard_base, 0, MEM_RELEASE);
    printf("raw_deflate_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
