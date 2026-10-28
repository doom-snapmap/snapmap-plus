/* A synthetic module for bakes: one flat floor slab spanning +/-2000 at z = 0,
 * findable through a three-node BSP. The same fixture aas_augment_test builds. */
#ifndef SH_TEST_AAS_FIXTURE_H
#define SH_TEST_AAS_FIXTURE_H
static const unsigned REC[22] = {
    16, 12, 12, 4, 40, 44, 16, 12, 4, 16, 1, 132, 128, 128, 128, 56, 4, 4, 60, 24, 24, 12
};

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v;
}
static void putf(unsigned char *p, float f)
{
    unsigned u; memcpy(&u, &f, 4); put32(p, u);
}

#define SET_WORDS 208
#define SETW(n)   (30 + SET_WORDS + (n) * 4)

/* A floor slab spanning +/-2000 at z = 0, in a BSP band -9.6 < z < 200 so a
 * point standing on the slab resolves to it. Three nodes: 0 is the engine's
 * dummy, 1 is the root, 2 is the ceiling test. */
static unsigned char *fixture_module(size_t *out_len)
{
    unsigned counts[22];
    size_t total = 394, off;
    unsigned i;
    unsigned char *p;
    size_t o[22];

    memset(counts, 0, sizeof counts);
    counts[SH_AAS_L_PLANES] = 2;
    counts[SH_AAS_L_VERTICES] = 4;
    counts[SH_AAS_L_EDGES] = 4;
    counts[SH_AAS_L_EDGEINDEX] = 4;
    counts[SH_AAS_L_AREAS] = 2;
    counts[SH_AAS_L_NODES] = 3;
    counts[SH_AAS_L_CLUSTERS] = 1;
    counts[SH_AAS_L_OBSTACLEPVS] = 2;
    counts[SH_AAS_L_TREES] = 1;
    counts[SH_AAS_L_AREABOUNDS] = 2;

    for (i = 0; i < 22; i++) total += 4 + (size_t)counts[i] * REC[i];
    p = (unsigned char *)calloc(1, total);
    if (!p) return NULL;

    memcpy(p, "2SAA", 4);
    p[4] = 3;
    p[5] = 29;

    /* settings: agent box 48x48x80 (radius 24), maxStepHeight 18,
     * maxFallHeight 0, minFloorCos 0.7 -- the monster-class shape. */
    putf(p + SETW(0), -24.0f); putf(p + SETW(1), -24.0f); putf(p + SETW(2), 0.0f);
    putf(p + SETW(3),  24.0f); putf(p + SETW(4),  24.0f); putf(p + SETW(5), 80.0f);
    putf(p + SETW(12), 18.0f);          /* maxStepHeight */
    putf(p + SETW(15), 0.0f);           /* maxFallHeight */
    /* minFloorCos = 0.7 admits slopes up to about 45.57 degrees.
     * Missing or invalid settings must refuse augmentation. */
    putf(p + SETW(16), 0.7f);
    put32(p + SETW(36), 100);           /* tt_startWalkOffLedge */

    off = 394;
    for (i = 0; i < 22; i++) {
        put32(p + off, counts[i]);
        off += 4;
        o[i] = off;
        off += (size_t)counts[i] * REC[i];
    }

    /* plane 0: (0 0 1) d=9.6  -> front is z > -9.6
     * plane 1: (0 0 -1) d=200 -> front is z < 200 */
    putf(p + o[SH_AAS_L_PLANES] + 8, 1.0f);
    putf(p + o[SH_AAS_L_PLANES] + 12, 9.6f);
    putf(p + o[SH_AAS_L_PLANES] + 16 + 8, -1.0f);
    putf(p + o[SH_AAS_L_PLANES] + 16 + 12, 200.0f);

    for (i = 0; i < 4; i++) {
        unsigned char *e = p + o[SH_AAS_L_EDGES] + i * 12;
        put32(e, i);
        put32(e + 4, (i + 1) % 4);
        put32(p + o[SH_AAS_L_EDGEINDEX] + i * 4, i);
    }

    /* area 0 is the dummy; area 1 is the floor. */
    put32(p + o[SH_AAS_L_AREAS] + 0x14, 0xFFFFFFFFu);
    put32(p + o[SH_AAS_L_AREAS] + 0x18, 0xFFFFFFFFu);
    {
        unsigned char *a1 = p + o[SH_AAS_L_AREAS] + 44;
        put32(a1 + 0x00, 1);            /* flags: floor */
        put16(a1 + 0x06, 4);            /* numEdges */
        put32(a1 + 0x08, 0);            /* firstEdgeIndex */
        put16(a1 + 0x0C, 0);            /* cluster 0 */
        put32(a1 + 0x10, 0);            /* firstObstaclePVS */
        put32(a1 + 0x14, 0xFFFFFFFFu);
        put32(a1 + 0x18, 0xFFFFFFFFu);
    }
    /* areaBounds: area 1 is flat at z = 0, +/-2000 in x and y. */
    {
        unsigned char *b = p + o[SH_AAS_L_AREABOUNDS] + 12;
        put16(b + 0, (unsigned)(short)-2000);
        put16(b + 2, (unsigned)(short)-2000);
        put16(b + 4, 0);
        put16(b + 6, 2000);
        put16(b + 8, 2000);
        put16(b + 10, 0);
    }
    /* the BSP: root is node 1. */
    {
        unsigned char *n = p + o[SH_AAS_L_NODES];
        put32(n + 16 + 0, 0);                       /* node 1 uses plane 0 */
        put32(n + 16 + 8, 2);                       /* front -> node 2 */
        put32(n + 16 + 12, 0);                      /* back  -> void */
        put32(n + 32 + 0, 1);                       /* node 2 uses plane 1 */
        put32(n + 32 + 8, 0xFFFFFFFFu);             /* front -> area 1 */
        put32(n + 32 + 12, 0);                      /* back  -> void */
    }
    /* Tree records contain three floats followed by three ints. The root is at
     * +12 and the area count at +20; independent offsets catch layout drift. */
    putf(p + o[SH_AAS_L_TREES] + 8, 1.0f);          /* ( 0 0 1 ) */
    put32(p + o[SH_AAS_L_TREES] + 12, 1);           /* a = root node index */
    put32(p + o[SH_AAS_L_TREES] + 16, 1);           /* b */
    put32(p + o[SH_AAS_L_TREES] + 20, 2);           /* c = area count */

    *out_len = total;
    return p;
}
#endif
