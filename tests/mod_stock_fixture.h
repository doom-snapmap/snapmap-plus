/* mod_stock_fixture.h -- a synthetic DOOM install for the stock-pointer tests:
 * <dir>\base\<container>.pindex with its .resources and .patch, in the
 * layout modstock.c reads (see there). Needs mod_test_util.h. */
#ifndef SNAPMAP_PLUS_MOD_STOCK_FIXTURE_H
#define SNAPMAP_PLUS_MOD_STOCK_FIXTURE_H

#include <stdlib.h>

static void put_bytes(const char *path, const void *p, size_t n)
{
    FILE *f = NULL;
    mt_mkdirs_for(path);
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (fopen_s(&f, path, "wb") == 0 && f) { if (n) fwrite(p, 1, n, f); fclose(f); }
}

static void put_rel(const char *dir, const char *rel, const void *p, size_t n)
{
    char path[MAX_PATH * 2], *q;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, rel);
    for (q = path; *q; q++) if (*q == '/') *q = '\\';
    put_bytes(path, p, n);
}

typedef struct tbuf { unsigned char *p; size_t n, cap; } tbuf;
static void tb_put(tbuf *b, const void *s, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = (unsigned char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void tb_be32(tbuf *b, unsigned v) { unsigned char c[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; tb_put(b, c, 4); }
static void tb_le32(tbuf *b, unsigned v) { unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) }; tb_put(b, c, 4); }
static void tb_str(tbuf *b, const char *s) { tb_le32(b, (unsigned)strlen(s)); tb_put(b, s, strlen(s)); }

typedef struct entry { const char *path; const unsigned char *bytes; size_t n; int patch; int deflate; } entry;

/* Write <dir>\base\<name>.pindex, .resources and .patch for these entries. A
 * deflate entry is stored as one final raw-DEFLATE stored block. */
static void write_container(const char *dir, const char *name, const entry *e, int count)
{
    tbuf res = {0}, pat = {0}, rows = {0}, pin = {0};
    char path[MAX_PATH * 2];
    int i;
    tb_put(&res, "RES0", 4);
    tb_put(&pat, "PAT0", 4);
    for (i = 0; i < count; i++) {
        tbuf *arch = e[i].patch ? &pat : &res;
        unsigned off = (unsigned)arch->n, zsize = (unsigned)e[i].n;
        unsigned char flags[5] = { 0, 0, 0, 0, (unsigned char)(e[i].patch ? 1 : 0) };
        if (e[i].deflate) {
            unsigned char hdr[5];
            hdr[0] = 1;
            hdr[1] = (unsigned char)e[i].n; hdr[2] = (unsigned char)(e[i].n >> 8);
            hdr[3] = (unsigned char)~hdr[1]; hdr[4] = (unsigned char)~hdr[2];
            tb_put(arch, hdr, 5);
            zsize += 5;
        }
        tb_put(arch, e[i].bytes, e[i].n);
        tb_le32(&rows, (unsigned)i);
        tb_str(&rows, "file");
        tb_str(&rows, e[i].path);
        tb_str(&rows, e[i].path);
        tb_be32(&rows, 0); tb_be32(&rows, off);
        tb_be32(&rows, (unsigned)e[i].n);
        tb_be32(&rows, zsize);
        tb_put(&rows, flags, 5);
    }
    {
        unsigned char head[0x20];
        memset(head, 0, sizeof head);
        head[0] = 5; memcpy(head + 1, "SER", 3);
        tb_put(&pin, head, sizeof head);
        tb_be32(&pin, (unsigned)count);
        tb_put(&pin, rows.p, rows.n);
        {
            unsigned rest = (unsigned)(pin.n - 0x20);
            pin.p[4] = (unsigned char)(rest >> 24); pin.p[5] = (unsigned char)(rest >> 16);
            pin.p[6] = (unsigned char)(rest >> 8); pin.p[7] = (unsigned char)rest;
        }
    }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\base\\%s.pindex", dir, name); put_bytes(path, pin.p, pin.n);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\base\\%s.resources", dir, name); put_bytes(path, res.p, res.n);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\base\\%s.patch", dir, name); put_bytes(path, pat.p, pat.n);
    free(res.p); free(pat.p); free(rows.p); free(pin.p);
}

#endif
