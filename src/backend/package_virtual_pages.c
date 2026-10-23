#include "package_virtual_pages.h"
#include "package_sources.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t vp_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t vp_u64(const unsigned char *p)
{ return (uint64_t)vp_u32(p) | (uint64_t)vp_u32(p + 4) << 32; }
static int vp_fail(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }
static int vp_root(const char *path)
{
    if (!path || strncmp(path, "virtualtextures/", 16) || !path[16]) return 0;
    char *canonical = sh_package_engine_path(path);
    int ok = canonical && !strcmp(canonical, path);
    free(canonical); return ok;
}
static char *vp_prefix(const char *path, size_t length)
{
    char *out = malloc(length + 1);
    if (out) { memcpy(out, path, length); out[length] = 0; }
    return out;
}
int sh_package_virtual_page_path(const char *path, char **source, unsigned *slot)
{
    size_t length, stem; int kind = SH_VIRTUAL_PAGE_NONE; unsigned index = 0;
    if (source) *source = NULL;
    if (slot) *slot = 0;
    if (!vp_root(path)) return SH_VIRTUAL_PAGE_NONE;
    length = strlen(path); stem = length;
    if (length > 22 && !strcmp(path + length - 6, ".pages")) {
        kind = SH_VIRTUAL_PAGE_PAGES; stem -= 6;
    } else if (length > 21 && !strcmp(path + length - 5, ".vmtr")) {
        kind = SH_VIRTUAL_PAGE_TABLE; stem -= 5;
    } else if (length > 25 && !strcmp(path + length - 6, ".mega2")) {
        size_t end = length - 6, start = end;
        while (start && path[start - 1] >= '0' && path[start - 1] <= '9') start--;
        if (start == end || end - start > 2 || path[start] == '0') return 0;
        for (size_t i = start; i < end; i++) index = index * 10 + (unsigned)(path[i] - '0');
        if (start >= 3 && !memcmp(path + start - 3, "_sq", 3) && index <= 16) {
            stem = start - 3; index--;
        } else if (start >= 2 && !memcmp(path + start - 2, "_q", 2) && index <= 4) {
            stem = start - 2; index += 15;
        } else return 0;
        kind = SH_VIRTUAL_PAGE_MEGA2;
    }
    if (!kind || stem <= 16 || path[stem - 1] == '/') return 0;
    if (source && !(*source = vp_prefix(path, stem))) return -1;
    if (slot) *slot = index;
    return kind;
}
char *sh_package_virtual_page_companion(const char *source, unsigned slot)
{
    if (slot > 20 || !vp_root(source)) return NULL;
    size_t length = strlen(source), stem = length;
    for (size_t i = length; i > 16 && source[i - 1] != '/'; i--)
        if (source[i - 1] == '.') { stem = i - 1; break; }
    if (stem <= 16 || source[stem - 1] == '/' || stem > SIZE_MAX - 20) return NULL;
    char *out = malloc(stem + 20);
    if (!out) return NULL;
    memcpy(out, source, stem);
    if (slot < 16) snprintf(out + stem, 20, "_sq%u.mega2", slot + 1);
    else if (slot < 20) snprintf(out + stem, 20, "_q%u.mega2", slot - 15);
    else memcpy(out + stem, ".pages", 7);
    return out;
}
static int vp_range(uint64_t offset, uint64_t count, uint64_t stride, uint64_t length)
{ return offset <= length && count <= (length - offset) / stride; }
int sh_package_virtual_page_inspect(int kind, uint64_t length,
    sh_virtual_page_read read, void *context, sh_virtual_page_info *out,
    char *error, size_t capacity)
{
    sh_virtual_page_info info = {0}; unsigned char block[16384];
    if (error && capacity) *error = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!out || !read || length > INT64_MAX ||
        (kind != SH_VIRTUAL_PAGE_MEGA2 && kind != SH_VIRTUAL_PAGE_PAGES))
        return vp_fail(error, capacity, "invalid virtual page inspection input");
    info.length = length;
    if (!length && kind == SH_VIRTUAL_PAGE_MEGA2) { info.empty = 1; *out = info; return 1; }
    size_t header = kind == SH_VIRTUAL_PAGE_MEGA2 ? 0x170 : 0x50;
    if (length < header || !read(context, 0, info.header, header))
        return vp_fail(error, capacity, "virtual page source has a truncated header");
    uint32_t magic = vp_u32(info.header), version = vp_u32(info.header + 4);
    if (kind == SH_VIRTUAL_PAGE_PAGES) {
        if (magic != 0x77339906u || version != 3)
            return vp_fail(error, capacity, "virtual pages header has an unsupported magic or version");
        info.levels = (uint16_t)vp_u32(info.header + 0x14);
        info.page_count = vp_u32(info.header + 0x18);
        *out = info; return 1;
    }
    if (magic != 0xa63fbb21u || version != 2)
        return vp_fail(error, capacity, "mega2 header has an unsupported magic or version");
    info.levels = vp_u32(info.header + 0xc);
    info.table_offset = vp_u64(info.header + 0x38);
    info.index_offset = vp_u64(info.header + 0x40);
    info.page_count = vp_u32(info.header + 0x48);
    info.index_count = vp_u32(info.header + 0x4c);
    /* Native counts are signed 32-bit and native table offsets are signed
     * file positions. Validate their actual representation and file ranges. */
    if (info.page_count > INT32_MAX || info.index_count > INT32_MAX ||
        info.table_offset < header || info.index_offset < header ||
        !vp_range(info.table_offset, info.page_count, 16, length) ||
        !vp_range(info.index_offset, info.index_count, 4, length))
        return vp_fail(error, capacity, "mega2 native table range is outside the captured file");
    for (size_t level = 0; level < 12; level++) {
        const unsigned char *r = info.header + 0x50 + level * 24;
        uint32_t x = vp_u32(r), y = vp_u32(r + 4), width = vp_u32(r + 8), height = vp_u32(r + 12);
        uint32_t first = vp_u32(r + 16), count = vp_u32(r + 20);
        uint64_t cells = (uint64_t)width * height;
        if (x > INT32_MAX || y > INT32_MAX || width > INT32_MAX || height > INT32_MAX ||
            cells > INT32_MAX || first > info.index_count || cells > info.index_count - first ||
            count > info.index_count - first)
            return vp_fail(error, capacity, "mega2 level indexes exceed the captured index table");
    }
    for (uint64_t at = 0; at < info.index_count;) {
        size_t count = info.index_count - at < sizeof(block)/4 ? (size_t)(info.index_count - at) : sizeof(block)/4;
        if (!read(context, info.index_offset + at * 4, block, count * 4))
            return vp_fail(error, capacity, "cannot read captured mega2 index table");
        for (size_t i = 0; i < count; i++) {
            uint32_t page = vp_u32(block + i * 4);
            if (page != UINT32_MAX && page >= info.page_count)
                return vp_fail(error, capacity, "mega2 index refers outside its page table");
        }
        at += count;
    }
    for (uint64_t at = 0; at < info.page_count;) {
        size_t count = info.page_count - at < sizeof(block)/16 ? (size_t)(info.page_count - at) : sizeof(block)/16;
        if (!read(context, info.table_offset + at * 16, block, count * 16))
            return vp_fail(error, capacity, "cannot read captured mega2 page table");
        for (size_t i = 0; i < count; i++) {
            uint64_t offset = vp_u64(block + i * 16), bytes = vp_u64(block + i * 16 + 8);
            if (bytes > INT32_MAX || !vp_range(offset, bytes, 1, length))
                return vp_fail(error, capacity, "mega2 payload extent is outside the captured file");
        }
        at += count;
    }
    *out = info; return 1;
}
int sh_package_virtual_page_equal(uint64_t a_length, sh_virtual_page_read a,
    void *a_context, uint64_t b_length, sh_virtual_page_read b, void *b_context)
{
    unsigned char left[16384], right[16384];
    if (!a || !b) return -1;
    if (a_length != b_length) return 0;
    for (uint64_t at = 0; at < a_length;) {
        size_t count = a_length - at < sizeof(left) ? (size_t)(a_length - at) : sizeof(left);
        if (!a(a_context, at, left, count) || !b(b_context, at, right, count)) return -1;
        if (memcmp(left, right, count)) return 0;
        at += count;
    }
    return 1;
}
