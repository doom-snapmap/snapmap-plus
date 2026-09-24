#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_virtual_pages.h"

typedef struct page_bytes { unsigned char *bytes; size_t length, calls; int fail; } page_bytes;
static int read_page(void *context, uint64_t offset, void *bytes, size_t count)
{
    page_bytes *p = context; p->calls++;
    if (p->fail || offset > p->length || count > p->length - offset) return 0;
    memcpy(bytes, p->bytes + (size_t)offset, count); return 1;
}
static void u32(unsigned char *p, uint32_t value)
{ for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (8 * i)); }
static void u64(unsigned char *p, uint64_t value)
{ u32(p, (uint32_t)value); u32(p + 4, (uint32_t)(value >> 32)); }
static void paths(void)
{
    char *source = NULL; unsigned slot; char path[100];
    for (unsigned i = 0; i < 21; i++) {
        char *p = sh_package_virtual_page_companion("virtualtextures/tree/source.pages", i);
        snprintf(path, sizeof(path), i < 16 ? "virtualtextures/tree/source_sq%u.mega2" :
            i < 20 ? "virtualtextures/tree/source_q%u.mega2" : "virtualtextures/tree/source.pages",
            i < 16 ? i + 1 : i - 15);
        assert(p && !strcmp(p, path));
        assert(sh_package_virtual_page_path(p, &source, &slot) ==
            (i < 20 ? SH_VIRTUAL_PAGE_MEGA2 : SH_VIRTUAL_PAGE_PAGES));
        assert(source && !strcmp(source, "virtualtextures/tree/source") && slot == (i < 20 ? i : 0));
        free(source); free(p);
    }
    assert(sh_package_virtual_page_path("virtualtextures/a_dlc5.vmtr", &source, &slot) == SH_VIRTUAL_PAGE_TABLE);
    assert(!strcmp(source, "virtualtextures/a_dlc5")); free(source);
    static const char *const invalid[] = {"a_sq1.mega2", "virtualtextures/a_sq0.mega2",
        "virtualtextures/a_sq17.mega2", "virtualtextures/a_q5.mega2", "virtualtextures/a_q01.mega2",
        "virtualtextures/a_sq1.mega2.extra", "VirtualTextures/a_sq1.mega2",
        "virtualtextures/../a_sq1.mega2", "virtualtextures/_sq1.mega2",
        "virtualtextures/.pages", "virtualtextures/a.unknown"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); i++)
        assert(sh_package_virtual_page_path(invalid[i], NULL, NULL) == SH_VIRTUAL_PAGE_NONE);
    assert(!sh_package_virtual_page_companion("virtualtextures/a", 21));
}
static page_bytes fixture(void)
{
    page_bytes p = {0}; p.length = 0x170 + 24 + 17;
    p.bytes = calloc(p.length, 1); assert(p.bytes);
    u32(p.bytes, 0xa63fbb21u); u32(p.bytes + 4, 2); u32(p.bytes + 0xc, 12);
    u64(p.bytes + 0x38, 0x170); u64(p.bytes + 0x40, 0x180);
    u32(p.bytes + 0x48, 1); u32(p.bytes + 0x4c, 2);
    u32(p.bytes + 0x58, 2); u32(p.bytes + 0x5c, 1); u32(p.bytes + 0x64, 2);
    u64(p.bytes + 0x170, 0x188); u64(p.bytes + 0x178, 17);
    u32(p.bytes + 0x180, 0); u32(p.bytes + 0x184, UINT32_MAX);
    memset(p.bytes + 0x188, 0x39, 17); return p;
}
static void inspect(void)
{
    page_bytes p = fixture(); sh_virtual_page_info info; char error[256];
    assert(sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, p.length, read_page, &p, &info, error, sizeof(error)));
    assert(info.page_count == 1 && info.index_count == 2 && info.levels == 12 && !info.empty && p.calls == 3);
    assert(sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, 0, read_page, &p, &info, error, sizeof(error)) && info.empty);
    assert(!sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, 0x16f, read_page, &p, &info, error, sizeof(error)));
    u32(p.bytes + 4, 3);
    assert(!sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, p.length, read_page, &p, &info, error, sizeof(error)));
    u32(p.bytes + 4, 2); u64(p.bytes + 0x40, UINT64_MAX - 3);
    assert(!sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, p.length, read_page, &p, &info, error, sizeof(error)));
    u64(p.bytes + 0x40, 0x180); u32(p.bytes + 0x180, 1);
    assert(!sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, p.length, read_page, &p, &info, error, sizeof(error)));
    u32(p.bytes + 0x180, 0); u64(p.bytes + 0x178, 18);
    assert(!sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, p.length, read_page, &p, &info, error, sizeof(error)));
    u64(p.bytes + 0x178, 17); u32(p.bytes + 0x60, 1);
    assert(!sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, p.length, read_page, &p, &info, error, sizeof(error)));
    u32(p.bytes + 0x60, 0); p.fail = 1;
    assert(!sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_MEGA2, p.length, read_page, &p, &info, error, sizeof(error)));
    p.fail = 0; u32(p.bytes, 0x77339906u); u32(p.bytes + 4, 3);
    assert(sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_PAGES, p.length, read_page, &p, &info, error, sizeof(error)));
    assert(!sh_package_virtual_page_inspect(SH_VIRTUAL_PAGE_PAGES, 0x4f, read_page, &p, &info, error, sizeof(error)));
    free(p.bytes);
}
static void exact_equal(void)
{
    page_bytes a = {0}, b = {0};
    a.length = b.length = 32773; a.bytes = calloc(a.length, 1); b.bytes = calloc(b.length, 1);
    assert(a.bytes && b.bytes);
    assert(sh_package_virtual_page_equal(a.length, read_page, &a, b.length, read_page, &b) == 1);
    /* Same external digest/length cannot hide different trailing bytes. */
    b.bytes[b.length - 1] = 1;
    assert(sh_package_virtual_page_equal(a.length, read_page, &a, b.length, read_page, &b) == 0);
    b.bytes[b.length - 1] = 0; b.fail = 1;
    assert(sh_package_virtual_page_equal(a.length, read_page, &a, b.length, read_page, &b) == -1);
    assert(sh_package_virtual_page_equal(a.length - 1, read_page, &a, b.length, read_page, &b) == 0);
    free(a.bytes); free(b.bytes);
}
int main(void)
{ paths(); inspect(); exact_equal(); puts("package_virtual_pages_test: PASS"); return 0; }
