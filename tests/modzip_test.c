/* modzip_test -- the ZIP reader against hostile archives.
 *
 * A valid archive written by the product's writer is read back; then every
 * truncation of it, every single-byte change of its directory and local
 * records, and a set of built hostile archives (an unknown or folder method,
 * an encrypted entry, ZIP64 size markers, a local record or data past the
 * directory, a stored entry whose sizes differ, a NUL in a name, two names
 * that differ only in case, a directory larger than its entries, a huge
 * declared size) are opened and every entry they admit is read. A ZIP64
 * archive as another tool writes it reads back and gets the same truncation,
 * byte and random damage; the writer past 65,535 entries (and, with
 * SH_ZIP_LARGE=1, past 4 GiB) writes ZIP64 records the reader reads. Nothing may
 * crash or hang; every refusal is one sentence, and a refusal of the
 * archive's structure names the next step. */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modzip.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* modbundle.c's path helper, which the reader uses to open a UTF-8 path. */
wchar_t *sh_mb_wpath(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}

static char g_path[MAX_PATH];

static int spit(const unsigned char *data, size_t len)
{
    FILE *f = NULL;
    if (fopen_s(&f, g_path, "wb") || !f) return 0;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = NULL;
    unsigned char *b;
    long n;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* Open the archive in g_path and read every entry it admits. Returns 1 when
 * it opened; the cause of a refusal is checked to be one plain sentence. */
static int exercise(char *cause, size_t cap)
{
    sh_zip_reader r;
    size_t i;
    int opened;
    cause[0] = 0;
    opened = HOC(cause, cap, sh_zip_reader_open(&r, g_path, HO_P(0)));
    if (!opened) {
        CHECK(cause[0] && strlen(cause) < cap - 1 && !strchr(cause, '\n'));
        return 0;
    }
    for (i = 0; i < r.n; i++) {
        size_t len = 0;
        char why[512];
        unsigned char *b = HOT(unsigned char *, why, sh_zip_reader_read(&r, (long)i, 1u << 20, &len, HO_P(0)));
        if (b) free(b);
        else CHECK(why[0]);
        CHECK(sh_zip_reader_find(&r, r.e[i].name) == (long)i);
    }
    sh_zip_reader_close(&r);
    return 1;
}

/* Offsets in the writer's archive of two stored entries, "a.txt" (5 bytes)
 * and "dir/b.txt" (3 bytes): local records at 0 and 35, directory after. */
static unsigned char *g_good;
static size_t g_good_len, g_cd_off;

static void write_good(void)
{
    sh_zip_writer w;
    wchar_t wp[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, g_path, -1, wp, MAX_PATH);
    memset(&w, 0, sizeof w);
    CHECK(sh_zip_writer_open(&w, wp));
    CHECK(sh_zip_writer_add(&w, "a.txt", "hello", 5));
    CHECK(sh_zip_writer_add(&w, "dir/b.txt", "abc", 3));
    CHECK(sh_zip_writer_finish(&w));
    sh_zip_writer_free(&w);
    g_good = slurp(g_path, &g_good_len);
    CHECK(g_good && g_good_len > 22);
    g_cd_off = g_good ? (size_t)(g_good[g_good_len - 6] | (g_good[g_good_len - 5] << 8) |
                                 (g_good[g_good_len - 4] << 16) | ((size_t)g_good[g_good_len - 3] << 24)) : 0;
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* A copy of the good archive with `fn` applied to it, written and opened. */
typedef void (*mutate_fn)(unsigned char *z, size_t len);
static int open_mutated(mutate_fn fn, char *cause, size_t cap)
{
    unsigned char *z = (unsigned char *)malloc(g_good_len);
    int ok;
    memcpy(z, g_good, g_good_len);
    fn(z, g_good_len);
    CHECK(spit(z, g_good_len));
    ok = exercise(cause, cap);
    free(z);
    return ok;
}

static void m_method_folder(unsigned char *z, size_t n) { (void)n; put16(z + g_cd_off + 10, 0xFFFF); }
static void m_method_odd(unsigned char *z, size_t n) { (void)n; put16(z + g_cd_off + 10, 12); }
static void m_encrypted(unsigned char *z, size_t n) { (void)n; put16(z + g_cd_off + 8, 1); }
static void m_zip64_size(unsigned char *z, size_t n) { (void)n; put32(z + g_cd_off + 20, 0xFFFFFFFFu); put32(z + g_cd_off + 24, 0xFFFFFFFFu); }
static void m_sizes_differ(unsigned char *z, size_t n) { (void)n; put32(z + g_cd_off + 24, 4); }
static void m_lho_past(unsigned char *z, size_t n) { put32(z + g_cd_off + 42, (unsigned)n); }
static void m_lho_into_cd(unsigned char *z, size_t n) { (void)n; put32(z + g_cd_off + 42, (unsigned)g_cd_off - 10); }
static void m_csize_huge(unsigned char *z, size_t n) { (void)n; put16(z + g_cd_off + 10, 8); put32(z + g_cd_off + 20, 0x7FFFFFF0u); }
static void m_usize_huge(unsigned char *z, size_t n) { (void)n; put16(z + g_cd_off + 10, 8); put32(z + g_cd_off + 24, 0x7FFFFFF0u); }
static void m_deflate_junk(unsigned char *z, size_t n) { (void)n; put16(z + g_cd_off + 10, 8); }
static void m_nul_name(unsigned char *z, size_t n) { (void)n; z[g_cd_off + 46 + 1] = 0; }
static void m_entries_many(unsigned char *z, size_t n) { put16(z + n - 22 + 8, 4000); put16(z + n - 22 + 10, 4000); }
static void m_local_sig(unsigned char *z, size_t n) { (void)n; z[0] = 'X'; }

static void hostile(void)
{
    char cause[512];
    printf("-- hostile archives are refused, each in one sentence with the next step\n");
    CHECK(!open_mutated(m_method_folder, cause, sizeof cause) && strstr(cause, "cannot read") && strstr(cause, "export it again"));
    CHECK(!open_mutated(m_method_odd, cause, sizeof cause) && strstr(cause, "packed in a way"));
    CHECK(!open_mutated(m_encrypted, cause, sizeof cause) && strstr(cause, "encrypted"));
    CHECK(!open_mutated(m_zip64_size, cause, sizeof cause) && strstr(cause, "export it again"));
    CHECK(!open_mutated(m_sizes_differ, cause, sizeof cause) && strstr(cause, "file list is damaged"));
    CHECK(!open_mutated(m_lho_past, cause, sizeof cause) && strstr(cause, "file list is damaged"));
    CHECK(!open_mutated(m_lho_into_cd, cause, sizeof cause) && strstr(cause, "file list is damaged"));
    CHECK(!open_mutated(m_csize_huge, cause, sizeof cause) && strstr(cause, "file list is damaged"));
    CHECK(!open_mutated(m_entries_many, cause, sizeof cause) && strstr(cause, "file list is damaged"));
    CHECK(!open_mutated(m_nul_name, cause, sizeof cause) && strstr(cause, "unsafe name"));
    /* Admitted at open, refused at the read: a declared size over the bound,
     * deflate that is not, a local record that is not one. */
    CHECK(open_mutated(m_usize_huge, cause, sizeof cause));
    CHECK(open_mutated(m_deflate_junk, cause, sizeof cause));
    CHECK(open_mutated(m_local_sig, cause, sizeof cause));
}

/* Two names one Windows folder cannot tell apart are one file. */
static void case_twins(void)
{
    sh_zip_writer w;
    wchar_t wp[MAX_PATH];
    char cause[512];
    printf("-- two names that differ only in case refuse the archive\n");
    MultiByteToWideChar(CP_UTF8, 0, g_path, -1, wp, MAX_PATH);
    memset(&w, 0, sizeof w);
    CHECK(sh_zip_writer_open(&w, wp));
    CHECK(sh_zip_writer_add(&w, "assets/Mod/a.decl", "x", 1));
    CHECK(sh_zip_writer_add(&w, "assets/mod/A.decl", "y", 1));
    CHECK(sh_zip_writer_finish(&w));
    sh_zip_writer_free(&w);
    CHECK(!exercise(cause, sizeof cause) && strstr(cause, "two files with one name"));
}

static void fuzz(void)
{
    char cause[512];
    size_t i, k;
    unsigned seed = 12345;
    unsigned char *z = (unsigned char *)malloc(g_good_len);
    printf("-- every truncation and every one-byte change of the records\n");
    /* Truncated anywhere. */
    for (i = 0; i < g_good_len; i++) {
        CHECK(spit(g_good, i));
        (void)exercise(cause, sizeof cause);
    }
    /* Each byte of the directory, the end record and both local records,
     * set to a spread of values. */
    for (i = 0; i < g_good_len; i++) {
        static const unsigned char vals[] = {0x00, 0x01, 0x08, 0x7F, 0x80, 0xFE, 0xFF};
        for (k = 0; k < sizeof vals; k++) {
            memcpy(z, g_good, g_good_len);
            if (z[i] == vals[k]) continue;
            z[i] = vals[k];
            CHECK(spit(z, g_good_len));
            (void)exercise(cause, sizeof cause);
        }
    }
    /* Random multi-byte damage. */
    for (i = 0; i < 4000; i++) {
        memcpy(z, g_good, g_good_len);
        for (k = 0; k < 6; k++) {
            seed = seed * 1103515245u + 12345u;
            z[(seed >> 8) % g_good_len] = (unsigned char)(seed >> 20);
        }
        CHECK(spit(z, g_good_len));
        (void)exercise(cause, sizeof cause);
    }
    free(z);
}

/* ---------------------------------------------------------------- ZIP64 */

static unsigned crc32_of(const unsigned char *p, size_t n)
{
    unsigned c = 0xFFFFFFFFu, k;
    size_t i;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

static void put64(unsigned char *p, unsigned long long v) { put32(p, (unsigned)v); put32(p + 4, (unsigned)(v >> 32)); }

/* The two entries of the good archive written as another tool writes ZIP64:
 * "a.txt" with its sizes as markers and the values in its local and central
 * ZIP64 extras, "dir/b.txt" with its local record offset as a marker, and the
 * ZIP64 end record and locator ahead of an end record of markers. */
static unsigned char *g_z64;
static size_t g_z64_len;

static void build_zip64(void)
{
    unsigned char *z = (unsigned char *)calloc(1, 1024);
    size_t at = 0, lho_a, lho_b, cd, cd_size, end64;
    unsigned crc_a = crc32_of((const unsigned char *)"hello", 5), crc_b = crc32_of((const unsigned char *)"abc", 3);
    /* a.txt */
    lho_a = at;
    put32(z + at, 0x04034b50u); put16(z + at + 4, 45); put16(z + at + 12, 0x21); put32(z + at + 14, crc_a);
    put32(z + at + 18, 0xFFFFFFFFu); put32(z + at + 22, 0xFFFFFFFFu); put16(z + at + 26, 5); put16(z + at + 28, 20);
    at += 30;
    memcpy(z + at, "a.txt", 5); at += 5;
    put16(z + at, 1); put16(z + at + 2, 16); put64(z + at + 4, 5); put64(z + at + 12, 5); at += 20;
    memcpy(z + at, "hello", 5); at += 5;
    /* dir/b.txt */
    lho_b = at;
    put32(z + at, 0x04034b50u); put16(z + at + 4, 20); put16(z + at + 12, 0x21); put32(z + at + 14, crc_b);
    put32(z + at + 18, 3); put32(z + at + 22, 3); put16(z + at + 26, 9);
    at += 30;
    memcpy(z + at, "dir/b.txt", 9); at += 9;
    memcpy(z + at, "abc", 3); at += 3;
    /* the directory */
    cd = at;
    put32(z + at, 0x02014b50u); put16(z + at + 4, 45); put16(z + at + 6, 45); put16(z + at + 14, 0x21);
    put32(z + at + 16, crc_a); put32(z + at + 20, 0xFFFFFFFFu); put32(z + at + 24, 0xFFFFFFFFu); put16(z + at + 28, 5);
    put16(z + at + 30, 28); put32(z + at + 42, 0xFFFFFFFFu);
    at += 46;
    memcpy(z + at, "a.txt", 5); at += 5;
    put16(z + at, 1); put16(z + at + 2, 24); put64(z + at + 4, 5); put64(z + at + 12, 5); put64(z + at + 20, lho_a); at += 28;
    put32(z + at, 0x02014b50u); put16(z + at + 4, 45); put16(z + at + 6, 45); put16(z + at + 14, 0x21);
    put32(z + at + 16, crc_b); put32(z + at + 20, 3); put32(z + at + 24, 3); put16(z + at + 28, 9);
    put16(z + at + 30, 12); put32(z + at + 42, 0xFFFFFFFFu);
    at += 46;
    memcpy(z + at, "dir/b.txt", 9); at += 9;
    put16(z + at, 1); put16(z + at + 2, 8); put64(z + at + 4, lho_b); at += 12;
    cd_size = at - cd;
    /* the ZIP64 end record and locator */
    end64 = at;
    put32(z + at, 0x06064b50u); put64(z + at + 4, 44); put16(z + at + 12, 45); put16(z + at + 14, 45);
    put64(z + at + 24, 2); put64(z + at + 32, 2); put64(z + at + 40, cd_size); put64(z + at + 48, cd); at += 56;
    put32(z + at, 0x07064b50u); put64(z + at + 8, end64); put32(z + at + 16, 1); at += 20;
    /* the end record, every field a marker */
    put32(z + at, 0x06054b50u); put16(z + at + 8, 0xFFFF); put16(z + at + 10, 0xFFFF);
    put32(z + at + 12, 0xFFFFFFFFu); put32(z + at + 16, 0xFFFFFFFFu); at += 22;
    g_z64 = z;
    g_z64_len = at;
}

static void zip64_reads(void)
{
    char cause[512];
    sh_zip_reader r;
    size_t n = 0, i, k;
    unsigned seed = 777;
    unsigned char *b, *z;
    printf("-- a ZIP64 archive another tool wrote reads back; its damage is refused like any other\n");
    build_zip64();
    CHECK(spit(g_z64, g_z64_len));
    CHECK(HO(cause, sh_zip_reader_open(&r, g_path, HO_P(0))) && r.n == 2);
    if (r.n == 2) {
        b = HOT(unsigned char *, cause, sh_zip_reader_read(&r, sh_zip_reader_find(&r, "a.txt"), 16, &n, HO_P(0)));
        CHECK(b && n == 5 && !memcmp(b, "hello", 5));
        free(b);
        b = HOT(unsigned char *, cause, sh_zip_reader_read(&r, sh_zip_reader_find(&r, "dir/b.txt"), 16, &n, HO_P(0)));
        CHECK(b && n == 3 && !memcmp(b, "abc", 3));
        free(b);
    }
    sh_zip_reader_close(&r);
    /* Every truncation, every one-byte change, random damage: no crash, and
     * every refusal is one sentence. */
    z = (unsigned char *)malloc(g_z64_len);
    for (i = 0; i < g_z64_len; i++) { CHECK(spit(g_z64, i)); (void)exercise(cause, sizeof cause); }
    for (i = 0; i < g_z64_len; i++) {
        static const unsigned char vals[] = {0x00, 0x01, 0x08, 0x10, 0x7F, 0x80, 0xFE, 0xFF};
        for (k = 0; k < sizeof vals; k++) {
            memcpy(z, g_z64, g_z64_len);
            if (z[i] == vals[k]) continue;
            z[i] = vals[k];
            CHECK(spit(z, g_z64_len));
            (void)exercise(cause, sizeof cause);
        }
    }
    for (i = 0; i < 4000; i++) {
        memcpy(z, g_z64, g_z64_len);
        for (k = 0; k < 6; k++) {
            seed = seed * 1103515245u + 12345u;
            z[(seed >> 8) % g_z64_len] = (unsigned char)(seed >> 20);
        }
        CHECK(spit(z, g_z64_len));
        (void)exercise(cause, sizeof cause);
    }
    /* Particular damage: a marker without its extra value, an extra holding
     * more than the markers ask, a locator pointing elsewhere. */
    {
        size_t cd = 0;
        for (i = 0; i + 4 <= g_z64_len; i++)
            if (g_z64[i] == 0x50 && g_z64[i + 1] == 0x4b && g_z64[i + 2] == 1 && g_z64[i + 3] == 2) { cd = i; break; }
        memcpy(z, g_z64, g_z64_len);
        put16(z + cd + 46 + 5 + 2, 16);   /* a.txt's central extra now leaves the offset out */
        CHECK(spit(z, g_z64_len) && !exercise(cause, sizeof cause) && strstr(cause, "file list is damaged"));
        memcpy(z, g_z64, g_z64_len);
        put32(z + cd + 20, 5);            /* a.txt's compressed size is no marker, the extra still holds it */
        CHECK(spit(z, g_z64_len) && !exercise(cause, sizeof cause) && strstr(cause, "file list is damaged"));
        memcpy(z, g_z64, g_z64_len);
        put64(z + g_z64_len - 22 - 20 + 8, 3);   /* the locator points into the data */
        CHECK(spit(z, g_z64_len) && !exercise(cause, sizeof cause) && strstr(cause, "file list is damaged"));
        memcpy(z, g_z64, g_z64_len);
        put32(z + g_z64_len - 22 - 20 + 16, 2);  /* two disks */
        CHECK(spit(z, g_z64_len) && !exercise(cause, sizeof cause) && strstr(cause, "split across several files"));
    }
    free(z);
    free(g_z64);
}

/* The writer past 65,535 entries: ZIP64 end records, read back whole. */
static void zip64_many(void)
{
    sh_zip_writer w;
    sh_zip_reader r;
    wchar_t wp[MAX_PATH];
    char cause[512], name[32];
    unsigned char *z, *b;
    size_t len = 0, n = 0;
    unsigned i;
    printf("-- 70,000 entries: the writer ends the archive with ZIP64 records, the reader reads every entry\n");
    MultiByteToWideChar(CP_UTF8, 0, g_path, -1, wp, MAX_PATH);
    CHECK(sh_zip_writer_open(&w, wp));
    for (i = 0; i < 70000; i++) {
        _snprintf_s(name, sizeof name, _TRUNCATE, "f/%05u.txt", i);
        if (!sh_zip_writer_add(&w, name, name, strlen(name))) break;
    }
    CHECK(i == 70000 && sh_zip_writer_finish(&w));
    sh_zip_writer_free(&w);
    z = slurp(g_path, &len);
    CHECK(z && len > 98);
    if (z) {
        CHECK(z[len - 22 + 8] == 0xFF && z[len - 22 + 9] == 0xFF);             /* the end record's count: a marker */
        CHECK(z[len - 42] == 0x50 && z[len - 41] == 0x4b && z[len - 40] == 6 && z[len - 39] == 7);   /* the locator */
        free(z);
    }
    CHECK(HO(cause, sh_zip_reader_open(&r, g_path, HO_P(0))) && r.n == 70000);
    b = HOT(unsigned char *, cause, sh_zip_reader_read(&r, sh_zip_reader_find(&r, "f/69999.txt"), 64, &n, HO_P(0)));
    CHECK(b && n == 11 && !memcmp(b, "f/69999.txt", 11));
    free(b);
    sh_zip_reader_close(&r);
    /* A small archive stays the ordinary ZIP it always was. */
    CHECK(sh_zip_writer_open(&w, wp) && sh_zip_writer_add(&w, "a.txt", "hello", 5) && sh_zip_writer_finish(&w));
    sh_zip_writer_free(&w);
    z = slurp(g_path, &len);
    CHECK(z && len == 22 + 46 + 5 + 30 + 5 + 5);
    free(z);
}

/* Past 4 GiB of data (set SH_ZIP_LARGE=1; writes about 4.3 GB to the temp
 * folder): the offsets past 4 GiB go in the ZIP64 extras and end record. */
static void zip64_large(void)
{
    sh_zip_writer w;
    sh_zip_reader r;
    wchar_t wp[MAX_PATH];
    char cause[512], name[32], flag[8];
    unsigned char *block, *b;
    size_t n = 0, size = 64u << 20;
    unsigned i, count = 66;
    if (!GetEnvironmentVariableA("SH_ZIP_LARGE", flag, sizeof flag) || strcmp(flag, "1")) {
        printf("  [skip] a ZIP past 4 GiB (set SH_ZIP_LARGE=1)\n");
        return;
    }
    printf("-- past 4 GiB: offsets in ZIP64 extras, read back\n");
    block = (unsigned char *)malloc(size);
    for (i = 0; i < size; i++) block[i] = (unsigned char)(i * 7);
    MultiByteToWideChar(CP_UTF8, 0, g_path, -1, wp, MAX_PATH);
    CHECK(sh_zip_writer_open(&w, wp));
    for (i = 0; i < count; i++) {
        _snprintf_s(name, sizeof name, _TRUNCATE, "big/%02u.bin", i);
        block[0] = (unsigned char)i;
        if (!sh_zip_writer_add(&w, name, block, size)) break;
    }
    CHECK(i == count && sh_zip_writer_finish(&w) && w.off > (4ull << 30));
    sh_zip_writer_free(&w);
    CHECK(HO(cause, sh_zip_reader_open(&r, g_path, HO_P(0))) && r.n == count);
    b = HOT(unsigned char *, cause, sh_zip_reader_read(&r, sh_zip_reader_find(&r, "big/65.bin"), size, &n, HO_P(0)));
    block[0] = 65;
    CHECK(b && n == size && !memcmp(b, block, size) && r.e[sh_zip_reader_find(&r, "big/65.bin")].lho > (4ull << 30));
    free(b);
    sh_zip_reader_close(&r);
    free(block);
}

int main(void)
{
    char tmp[MAX_PATH], cause[512];
    GetTempPathA(sizeof tmp, tmp);
    _snprintf_s(g_path, sizeof g_path, _TRUNCATE, "%smodzip_test_%lu.zip", tmp, GetCurrentProcessId());
    printf("-- the writer's archive reads back\n");
    write_good();
    CHECK(spit(g_good, g_good_len) && exercise(cause, sizeof cause));
    {
        sh_zip_reader r;
        size_t n = 0;
        unsigned char *b;
        CHECK(HO(cause, sh_zip_reader_open(&r, g_path, HO_P(0))) && r.n == 2);
        b = HOT(unsigned char *, cause, sh_zip_reader_read(&r, sh_zip_reader_find(&r, "dir/b.txt"), 16, &n, HO_P(0)));
        CHECK(b && n == 3 && !memcmp(b, "abc", 3));
        free(b);
        CHECK(sh_zip_reader_find(&r, "DIR/B.TXT") < 0);   /* the exact name */
        sh_zip_reader_close(&r);
    }
    hostile();
    case_twins();
    fuzz();
    zip64_reads();
    zip64_many();
    zip64_large();
    DeleteFileA(g_path);
    free(g_good);
    printf("modzip_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
