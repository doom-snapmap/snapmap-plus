/* Portable inspection of the native virtual texture page-source family. */
#ifndef SH_PACKAGE_VIRTUAL_PAGES_H
#define SH_PACKAGE_VIRTUAL_PAGES_H
#include <stddef.h>
#include <stdint.h>

enum sh_virtual_page_kind {
    SH_VIRTUAL_PAGE_NONE, SH_VIRTUAL_PAGE_TABLE, SH_VIRTUAL_PAGE_MEGA2,
    SH_VIRTUAL_PAGE_PAGES
};
typedef struct sh_virtual_page_info {
    uint64_t length, table_offset, index_offset;
    uint32_t page_count, index_count, levels;
    unsigned char header[0x170];
    int empty;
} sh_virtual_page_info;
/* Read exactly count bytes at offset from one immutable captured file.
 * Return 1, or 0 on failure. No cursor is shared with other consumers. */
typedef int (*sh_virtual_page_read)(void *context, uint64_t offset,
    void *bytes, size_t count);

/* Classify only canonical native companion spellings under virtualtextures/.
 * mega2 indices are exactly sq1..sq16 or q1..q4, with no leading zeroes.
 * A positive result establishes filename grammar, never valid content.
 * source is an optional malloc-owned stem, without the native suffix.
 * slot is 0..15 for sq, 16..19 for q; zero for .pages/.vmtr. */
int sh_package_virtual_page_path(const char *path, char **source, unsigned *slot);
/* Native mega2 rule: strip the input source's extension, then append the
 * requested sq/q suffix. slot20 is the corresponding .pages source.
 * The source must already be canonical and rooted at virtualtextures/. */
char *sh_package_virtual_page_companion(const char *source, unsigned slot);

/* Validate native read/allocation ranges before any renderer call. Empty
 * mega2 members are native omissions. Nonempty mega2 validates the 0x170
 * header, all 12 level ranges, every index and every payload extent using
 * bounded streaming reads. No arbitrary byte/count cap is imposed. pages
 * validates its 0x50 header; its separate native index/stream preparation
 * remains an admission obligation. This does not certify GPU decoding. */
int sh_package_virtual_page_inspect(int kind, uint64_t length,
    sh_virtual_page_read read, void *context, sh_virtual_page_info *out,
    char *error, size_t capacity);
/* Positive equality, including bytes after native tables. Digest equality is
 * only a caller's filter. Equal lengths never replace exact comparison. */
int sh_package_virtual_page_equal(uint64_t a_length, sh_virtual_page_read a,
    void *a_context, uint64_t b_length, sh_virtual_page_read b, void *b_context);
#endif
