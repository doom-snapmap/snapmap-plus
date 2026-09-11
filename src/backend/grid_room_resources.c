/* Transform admitted installed geometry without embedding game asset bytes. */
#include "grid_room.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static unsigned be16(const unsigned char *p) { return (unsigned)p[0]<<8 | p[1]; }
static int signed16(const unsigned char *p)
{
    unsigned v = be16(p);
    return v >= 32768 ? (int)v - 65536 : (int)v;
}
static float read_float(const unsigned char *p)
{
    uint32_t bits = be32(p); float value;
    memcpy(&value, &bits, 4); return value;
}
static int write_float(unsigned char *p, float value)
{
    uint32_t bits;
    if (!isfinite(value)) return 0;
    memcpy(&bits, &value, 4);
    p[0] = (unsigned char)(bits>>24); p[1] = (unsigned char)(bits>>16);
    p[2] = (unsigned char)(bits>>8); p[3] = (unsigned char)bits;
    return 1;
}
static int block(size_t size, size_t offset, size_t count, size_t stride)
{
    return offset <= size && stride && count <= (size-offset)/stride;
}
static int disjoint(const void *a, const void *b, size_t length)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<y ? y-x>=length : x-y>=length;
}
static int vector(const sh_grid_warp *w, const unsigned char *s, unsigned char *d)
{
    unsigned a;
    for (a = 0; a < 3; ++a) {
        float v = read_float(s + a*4);
        if (!isfinite(v) || !write_float(d+a*4, sh_grid_coordinate(w,a,v))) return 0;
    }
    return 1;
}
static int bounds(const sh_grid_warp *w, const unsigned char *s, unsigned char *d)
{
    unsigned a;
    for (a = 0; a < 3; ++a) if (read_float(s+a*4) > read_float(s+12+a*4)) return 0;
    return vector(w,s,d) && vector(w,s+12,d+12);
}
static int short_bounds(const sh_grid_warp *w, const unsigned char *s, unsigned char *d)
{
    unsigned a;
    for (a = 0; a < 6; ++a) {
        float v = sh_grid_coordinate(w,a%3,(float)signed16(s+a*2));
        int n;
        if (!isfinite(v)) return 0;
        v = a < 3 ? floorf(v) : ceilf(v);
        if (v < -32768 || v > 32767) return 0;
        n = (int)v;
        d[a*2] = (unsigned char)((unsigned)n>>8); d[a*2+1] = (unsigned char)n;
    }
    return 1;
}
static int string_skip(const unsigned char *s, size_t n, size_t *at)
{
    uint32_t length;
    if (!block(n,*at,1,4)) return 0;
    length = (uint32_t)s[*at] | (uint32_t)s[*at+1]<<8 |
             (uint32_t)s[*at+2]<<16 | (uint32_t)s[*at+3]<<24;
    *at += 4;
    if (length > 10000 || !block(n,*at,length,1)) return 0;
    *at += length;
    return 1;
}

int sh_grid_bmodel(const sh_grid_warp *w, const unsigned char *s, size_t n, unsigned char *d)
{
    size_t at = 12, vstart;
    uint32_t magic, surfaces, i, j, patches, vertices, indices, materials;
    if (!w || !s || !d || n < 16 || n > 64u*1024u*1024u || !disjoint(s,d,n)) return 0;
    magic = be32(s); surfaces = be32(s+8);
    if ((magic != 0x1a4c4d42 && magic != 0x1b4c4d42) || surfaces > 10000) return 0;
    memcpy(d,s,n);
    for (i = 0; i < surfaces; ++i) {
        if (!string_skip(s,n,&at)) return 0;
        if (magic == 0x1b4c4d42) at += 4;
        if (!block(n,at,1,12)) return 0;
        patches = be32(s+at+8); at += 12;
        if (patches > 10000) return 0;
        for (j = 0; j < patches; ++j) if (!string_skip(s,n,&at)) return 0;
        if (!block(n,at,1,52)) return 0;
        vertices = be32(s+at); indices = be32(s+at+4);
        if (be32(s+at+8) != 0x1801f || vertices > 65536 || indices > 1000000 || indices%3) return 0;
        at += 52; vstart = at;
        if (!block(n,at,vertices,48)) return 0;
        for (j = 0; j < vertices; ++j) if (!vector(w,s+at+j*48,d+at+j*48)) return 0;
        at += (size_t)vertices*48;
        if (!block(n,at,indices,2)) return 0;
        for (j = 0; j < indices; ++j) if (be16(s+at+j*2) >= vertices) return 0;
        at += (size_t)indices*2;
        if (!block(n,at,1,32) || !bounds(w,s+at,d+at) || be32(s+at+28) != magic) return 0;
        /* Recompute declared bounds from transformed vertices: a piecewise
         * mapping may preserve empty margins, but must never exclude a vertex. */
        if (vertices) {
            unsigned a;
            for (a = 0; a < 3; ++a) {
                float lo = read_float(d+vstart+a*4), hi = lo;
                for (j = 1; j < vertices; ++j) {
                    float v = read_float(d+vstart+(size_t)j*48+a*4);
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                if (lo < read_float(d+at+a*4)) write_float(d+at+a*4,lo);
                if (hi > read_float(d+at+12+a*4)) write_float(d+at+12+a*4,hi);
            }
        }
        at += 32;
    }
    if (!block(n,at,1,4)) return 0;
    materials = be32(s+at); at += 4;
    if (materials > 10000) return 0;
    for (i = 0; i < materials; ++i) {
        if (!string_skip(s,n,&at) || !block(n,at,1,12)) return 0;
        at += 12;
    }
    return at == n;
}

static int split(const sh_grid_warp *w, const unsigned char *s, unsigned char *d)
{
    int axis = signed16(s);
    if (axis == -1) return 1;
    if (axis < 0 || axis > 2 || !isfinite(read_float(s+4))) return 0;
    return write_float(d+4, sh_grid_coordinate(w,(unsigned)axis,read_float(s+4)));
}

static int plane(const sh_grid_warp *w, const unsigned char *s, unsigned char *d,
                   const unsigned char *brush)
{
    float normal[3], distance = read_float(s+12), magnitude = 0, nn[3];
    unsigned a, axes = 0, last = 0;
    if (!isfinite(distance)) return 0;
    for (a = 0; a < 3; ++a) {
        normal[a] = read_float(s+a*4);
        if (!isfinite(normal[a])) return 0;
        if (fabsf(normal[a]) > .00001f) { ++axes; last = a; }
    }
    if (!axes) return 0;
    if (axes == 1) return write_float(d+12, -normal[last] *
        sh_grid_coordinate(w,last,-distance/normal[last]));
    for (a = 0; a < 3; ++a) {
        float lo = (float)signed16(brush+a*2), hi = (float)signed16(brush+6+a*2);
        float scale, shift;
        if (hi < lo) return 0;
        sh_grid_affine(w,a,(lo+hi)*.5f,&scale,&shift);
        /* An oblique brush spanning a knot requires splitting its planes.
         * Refuse that topology instead of silently producing bad collision. */
        if (fabsf(normal[a]) > .00001f &&
            (fabsf(sh_grid_coordinate(w,a,lo)-(scale*lo+shift)) > .01f ||
             fabsf(sh_grid_coordinate(w,a,hi)-(scale*hi+shift)) > .01f)) return 0;
        nn[a] = normal[a]/scale; distance -= nn[a]*shift;
        magnitude += nn[a]*nn[a];
    }
    magnitude = sqrtf(magnitude);
    if (!(magnitude > 0)) return 0;
    for (a = 0; a < 3; ++a) if (!write_float(d+a*4,nn[a]/magnitude)) return 0;
    return write_float(d+12,distance/magnitude);
}

int sh_grid_bcm(const sh_grid_warp *w, const unsigned char *s, size_t n, unsigned char *d)
{
    const unsigned strides[9] = {16,2,16,16,2,4,16,16,16};
    size_t at = 61;
    uint32_t nodes, areas, i, j, t;
    if (!w || !s || !d || n < 69 || n > 64u*1024u*1024u || !disjoint(s,d,n) || memcmp(s,"BCM8",4) || s[56]) return 0;
    nodes = be32(s+57);
    /* Installed Grid Room collisions have no outer BSP. Its node layout has
     * not been established; it must not be inferred from the area's tree. */
    if (nodes) return 0;
    if (!block(n,at,nodes,16)) return 0;
    memcpy(d,s,n);
    if (!bounds(w,s+25,d+25)) return 0;
    for (i = 0; i < nodes; ++i) if (!split(w,s+at+i*16,d+at+i*16)) return 0;
    at += (size_t)nodes*16;
    if (!block(n,at,1,4)) return 0;
    areas = be32(s+at); at += 4;
    if (areas > 65536) return 0;
    for (i = 0; i < areas; ++i) {
        uint32_t size, counts[9], offsets[9];
        const unsigned char *b; unsigned char *out;
        if (!block(n,at,1,32)) return 0;
        size = be32(s+at);
        if (!bounds(w,s+at+8,d+at+8)) return 0;
        at += 32;
        if (size == 32) continue;
        if (size < 112 || !block(n,at,size,1)) return 0;
        b = s+at; out = d+at;
        if (!bounds(w,b+8,out+8)) return 0;
        for (t = 0; t < 9; ++t) {
            counts[t] = be32(b+36+t*8); offsets[t] = be32(b+40+t*8);
            if ((counts[t] && offsets[t] < 112) || !block(size,offsets[t],counts[t],strides[t])) return 0;
        }
        for (j = 0; j < counts[0]; ++j)
            if (!split(w,b+offsets[0]+j*16,out+offsets[0]+j*16)) return 0;
        for (j = 0; j < counts[6]; ++j)
            if (!vector(w,b+offsets[6]+j*16,out+offsets[6]+j*16)) return 0;
        for (t = 3; t <= 7; t += 4)
            for (j = 0; j < counts[t]; ++j)
                if (!short_bounds(w,b+offsets[t]+j*16,out+offsets[t]+j*16)) return 0;
        for (j = 0; j < counts[7]; ++j) {
            const unsigned char *brush = b+offsets[7]+j*16;
            unsigned count = brush[13], first = be16(brush+14), k;
            if (first > counts[8] || count > counts[8]-first) return 0;
            for (k = first; k < first+count; ++k)
                if (!plane(w,b+offsets[8]+k*16,out+offsets[8]+k*16,brush)) return 0;
        }
        at += size;
    }
    return block(n,at,1,4) && !memcmp(s+at,"BCM8",4) && at+4 == n;
}
