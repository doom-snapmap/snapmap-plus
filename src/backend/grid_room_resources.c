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

static void put16(unsigned char *p, int v)
{
    p[0] = (unsigned char)((unsigned)v>>8); p[1] = (unsigned char)v;
}

/* An axis-aligned plane moves with its axis. An oblique one would need
 * splitting where the warp bends, so it is refused. */
static int axis_plane(const sh_grid_warp *w, const unsigned char *s, unsigned char *d)
{
    float normal[3], distance = read_float(s+12);
    unsigned a, axes = 0, last = 0;
    if (!isfinite(distance)) return 0;
    for (a = 0; a < 3; ++a) {
        normal[a] = read_float(s+a*4);
        if (!isfinite(normal[a])) return 0;
        if (fabsf(normal[a]) > .00001f) { ++axes; last = a; }
    }
    return axes == 1 && write_float(d+12, -normal[last] *
        sh_grid_coordinate(w,last,-distance/normal[last]));
}

/* idSoundBSPFile (VK 0x14184adc0 / GL 0x14183db60), big-endian: version
 * 'SND'+9..11; planes (16 bytes), vertices (12), polygon vertex indices (4),
 * polygons (16: plane, count, first index, areas, int16 centre), portal lists
 * (4), areas (8), nodes (8), two ints and the version again. Placement moves
 * only planes and vertices (VK 0x14184b6e0 / GL 0x14183e710), then sets each
 * polygon's centre to the truncated mean of its vertices (VK 0x141848ea0 /
 * GL 0x14183bbb0). A monotone per-axis warp keeps every point's side of an
 * axis-aligned plane, so the same topology is exact for the moved room. */
int sh_grid_sbsp(const sh_grid_warp *w, const unsigned char *s, size_t n, unsigned char *d)
{
    static const unsigned strides[3] = {4,8,8};
    static const uint32_t limits[3] = {4000000,4096,4000000};
    size_t at = 4, verts_at, index_at, poly_at;
    uint32_t version, planes, verts, indices, polys, count, i, k;
    unsigned t;
    if (!w || !s || !d || n < 48 || n > 64u*1024u*1024u || !disjoint(s,d,n)) return 0;
    version = be32(s);
    if (version - 0x534e4409u > 2u) return 0;
    memcpy(d,s,n);
    planes = be32(s+at); at += 4;
    if (planes > 1000000 || !block(n,at,planes,16)) return 0;
    for (i = 0; i < planes; ++i) if (!axis_plane(w,s+at+(size_t)i*16,d+at+(size_t)i*16)) return 0;
    at += (size_t)planes*16;
    if (!block(n,at,1,4)) return 0;
    verts = be32(s+at); at += 4; verts_at = at;
    if (verts > 1000000 || !block(n,at,verts,12)) return 0;
    for (i = 0; i < verts; ++i) if (!vector(w,s+at+(size_t)i*12,d+at+(size_t)i*12)) return 0;
    at += (size_t)verts*12;
    if (!block(n,at,1,4)) return 0;
    indices = be32(s+at); at += 4; index_at = at;
    if (indices > 4000000 || !block(n,at,indices,4)) return 0;
    for (i = 0; i < indices; ++i) if (be32(s+at+(size_t)i*4) >= verts) return 0;
    at += (size_t)indices*4;
    if (!block(n,at,1,4)) return 0;
    polys = be32(s+at); at += 4; poly_at = at;
    if (polys > 1000000 || !block(n,at,polys,16)) return 0;
    /* As the engine does, the first, empty polygon keeps its centre. */
    for (i = 1; i < polys; ++i) {
        const unsigned char *p = s+poly_at+(size_t)i*16;
        unsigned plane = be16(p), vcount = be16(p+2), first = be16(p+4), a;
        float sum[3] = {0,0,0};
        if (plane >= planes || first > indices || vcount > indices-first) return 0;
        if (!vcount) continue;
        for (k = 0; k < vcount; ++k) {
            size_t v = (size_t)be32(s+index_at+(size_t)(first+k)*4)*12;
            for (a = 0; a < 3; ++a) sum[a] += read_float(d+verts_at+v+a*4);
        }
        for (a = 0; a < 3; ++a) {
            float mean = sum[a]*(1.0f/(float)vcount);
            if (!(mean > -32769.f && mean < 32768.f)) return 0;
            put16(d+poly_at+(size_t)i*16+10+a*2,(int)mean);
        }
    }
    at += (size_t)polys*16;
    /* Portal lists, areas (at most the engine's MAX_ROUTE_AREAS) and nodes. */
    for (t = 0; t < 3; ++t) {
        if (!block(n,at,1,4)) return 0;
        count = be32(s+at); at += 4;
        if (count > limits[t] || !block(n,at,count,strides[t])) return 0;
        at += (size_t)count*strides[t];
    }
    return block(n,at,3,4) && be32(s+at+8) == version && at+12 == n;
}

/* Ambient lighting (idAmbientLighting, VK 0x140d697f0 / GL 0x140d69070):
 * big-endian magic 0x21155702, node and sample counts, nodes of eight
 * children (bit 30: a leaf's sample), then samples of nine RGB half-float SH
 * coefficients and an int16 module-space position. A SnapMap map reads only
 * a placed module's samples: it moves each by the placement and builds one
 * octree for the whole map from all of them (VK 0x1404ec690 / GL
 * 0x1404ec060; VK 0x140d67540 / GL 0x140d66dc0). So every authored sample
 * moves with the room and keeps its coefficients; the module's own nodes stay
 * valid, and a map does not use them. */
int sh_grid_ambient(const sh_grid_warp *w, const unsigned char *s, size_t n, unsigned char *d)
{
    uint32_t nodes, samples, i;
    size_t at;
    if (!w || !s || !d || n < 12 || n > 64u*1024u*1024u || !disjoint(s,d,n) ||
        be32(s) != 0x21155702u) return 0;
    nodes = be32(s+4); samples = be32(s+8);
    if (nodes > 1000000 || samples > 1000000 || !block(n,12,nodes,32) ||
        n-12-(size_t)nodes*32 != (size_t)samples*60) return 0;
    for (i = 0; i < nodes*8u; ++i) {
        uint32_t e = be32(s+12+(size_t)i*4);
        if ((e>>31) || (((e>>30)&1) ? (e&0x3fffffffu) >= samples : e >= nodes)) return 0;
    }
    memcpy(d,s,n);
    at = 12+(size_t)nodes*32+54;
    for (i = 0; i < samples; ++i, at += 60) {
        unsigned a;
        for (a = 0; a < 3; ++a) {
            float v = floorf(sh_grid_coordinate(w,a,(float)signed16(s+at+a*2))+.5f);
            if (!(v >= -32768.f && v <= 32767.f)) return 0;
            put16(d+at+a*2,(int)v);
        }
    }
    return 1;
}
