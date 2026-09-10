/* Tests AAS2 3.29 parsing, appending and byte-preserving serialization.
 * Synthetic fixtures use nonzero header, settings and record bytes to expose
 * accidental rewriting. They also satisfy navmesh.c's structural gate. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aas_edit.h"
#include "navmesh.h"

static int g_failed;

#define CHECK(expr) do {                                                         \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ==================================================================== */
/* a synthetic AAS2 3.29 file                                            */
/* ==================================================================== */

/* Keep format sizes independent of aas_edit.c so drift cannot make the
 * implementation and its fixture agree on the same mistake. */
static const unsigned AAS_REC[SH_AAS_L__COUNT] = {
    16, 12, 12, 4, 40, 44, 16, 12, 4, 16, 1, 132, 128, 128, 128, 56, 4, 4, 60, 24, 24, 12
};

/* Assert the shared 22-lump layout and preamble size at compile time. */
typedef char aas_lumps_agree[(SH_AAS_L__COUNT == SH_AAS_LUMPS) ? 1 : -1];
typedef char aas_preamble_agrees[
    (SH_AAS_PREAMBLE_BYTES == SH_AAS_HEADER_BYTES + SH_AAS_SETTINGS_BYTES) ? 1 : -1];

/* The settings words start past the u32 type and the three length-prefixed
 * 64-byte strings; maxStepHeight is word 12 and the agent box is words 0..5. */
#define SET_WORDS_OFF   208u
#define SET_W_MAXSTEP   12u
#define SET_STRING_SIZE 64u

typedef struct aas_synth {
    unsigned char *p;
    size_t         len;
    size_t         off[SH_AAS_L__COUNT];    /* first record of each lump */
    unsigned       count[SH_AAS_L__COUNT];
} aas_synth;

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
    unsigned bits;
    memcpy(&bits, &f, sizeof bits);
    put32(p, bits);
}

/* Synthetic valid file: dummy and floor areas, four vertices and edges, one
 * reachability, cluster and cover record, and a three-node BSP. Dependency-name
 * padding grows it without new geometry; nonzero metadata tests preservation. */
static int synth_aas(aas_synth *b, unsigned pad)
{
    unsigned counts[SH_AAS_L__COUNT];
    size_t total = SH_AAS_PREAMBLE_BYTES, off;
    unsigned i;

    memset(b, 0, sizeof *b);
    memset(counts, 0, sizeof counts);
    counts[SH_AAS_L_PLANES] = 1;
    counts[SH_AAS_L_VERTICES] = 4;
    counts[SH_AAS_L_EDGES] = 4;
    counts[SH_AAS_L_EDGEINDEX] = 4;
    counts[SH_AAS_L_REACHABILITIES] = 1;
    counts[SH_AAS_L_AREAS] = 2;
    counts[SH_AAS_L_NODES] = 3;
    counts[SH_AAS_L_PORTALS] = 1;
    counts[SH_AAS_L_CLUSTERS] = 1;
    counts[SH_AAS_L_OBSTACLEPVS] = 2;
    counts[SH_AAS_L_DEPENDENCYNAMES] = pad;
    counts[SH_AAS_L_COVER] = 1;
    counts[SH_AAS_L_AREACOVERINDEX] = 1;
    counts[SH_AAS_L_TREES] = 1;
    counts[SH_AAS_L_AREABOUNDS] = 2;

    for (i = 0; i < SH_AAS_L__COUNT; i++) total += 4 + (size_t)counts[i] * AAS_REC[i];
    b->p = (unsigned char *)calloc(1, total);
    if (!b->p) return 0;
    b->len = total;

    /* header: magic, version, then crc, mapVersion and the four firstFake
     * indices, all non-zero so a writer that zeroed them would be visible */
    memcpy(b->p, "2SAA", 4);
    b->p[4] = SH_AAS_MAJOR;
    b->p[5] = SH_AAS_MINOR;
    put32(b->p + 6, 0xDEADBEEFu);       /* crc */
    put32(b->p + 10, 0x00010203u);      /* map version */
    put32(b->p + 14, 3);                /* firstFakeVertex */
    put32(b->p + 18, 3);                /* firstFakeEdge */
    put32(b->p + 22, 3);                /* firstFakeEdgeIndex */
    put32(b->p + 26, 1);                /* firstFakeArea */

    /* settings: type, three length-prefixed fixed strings, then the 39 words */
    {
        unsigned char *s = b->p + SH_AAS_HEADER_BYTES;
        unsigned so = 0;
        static const char *names[3] = { "monster128", "monster", "" };
        put32(s + so, 1);               /* type: monster */
        so += 4;
        for (i = 0; i < 3; i++) {
            put32(s + so, SET_STRING_SIZE);
            so += 4;
            memcpy(s + so, names[i], strlen(names[i]));
            so += SET_STRING_SIZE;
        }
        CHECK(so == SET_WORDS_OFF);
        putf(s + so +  0 * 4, -24.0f);  /* agent box mins */
        putf(s + so +  1 * 4, -24.0f);
        putf(s + so +  2 * 4, 0.0f);
        putf(s + so +  3 * 4, 24.0f);   /* agent box maxs */
        putf(s + so +  4 * 4, 24.0f);
        putf(s + so +  5 * 4, 74.0f);
        put32(s + so +  6 * 4, 1);      /* primitiveModeBrush */
        put32(s + so +  7 * 4, 1);      /* primitiveModeModel */
        putf(s + so +  8 * 4, 0.0f);    /* gravity, normalised */
        putf(s + so +  9 * 4, 0.0f);
        putf(s + so + 10 * 4, -1.0f);
        putf(s + so + 11 * 4, 1066.66f);
        putf(s + so + SET_W_MAXSTEP * 4, 18.0f);
        for (i = SET_W_MAXSTEP + 1; i < 39; i++)
            putf(s + so + i * 4, (float)i + 0.5f);
    }

    off = SH_AAS_PREAMBLE_BYTES;
    for (i = 0; i < SH_AAS_L__COUNT; i++) {
        put32(b->p + off, counts[i]);
        off += 4;
        b->off[i] = off;
        b->count[i] = counts[i];
        off += (size_t)counts[i] * AAS_REC[i];
    }
    CHECK(off == b->len);

    /* the one plane: a normal and a distance, so the lump is not all zeroes */
    putf(b->p + b->off[SH_AAS_L_PLANES] + 8, 1.0f);
    putf(b->p + b->off[SH_AAS_L_PLANES] + 12, 64.0f);

    /* vertices at the corners of a 128-unit square */
    for (i = 0; i < 4; i++) {
        unsigned char *v = b->p + b->off[SH_AAS_L_VERTICES] + i * 12;
        putf(v + 0, (i == 1 || i == 2) ? 128.0f : 0.0f);
        putf(v + 4, (i >= 2) ? 128.0f : 0.0f);
        putf(v + 8, 32.0f);
    }
    /* edges reference vertices 0..3 */
    for (i = 0; i < 4; i++) {
        unsigned char *e = b->p + b->off[SH_AAS_L_EDGES] + i * 12;
        put32(e, i);
        put32(e + 4, (i + 1) % 4);
    }
    /* edgeIndex references edges 0..3 (the sign is a direction, not an index) */
    for (i = 0; i < 4; i++) put32(b->p + b->off[SH_AAS_L_EDGEINDEX] + i * 4, i);
    /* areaCoverIndex references cover 0 */
    put32(b->p + b->off[SH_AAS_L_AREACOVERINDEX], 0);

    /* area 0: the dummy. Owns nothing, heads no reachability list. */
    {
        unsigned char *a0 = b->p + b->off[SH_AAS_L_AREAS];
        put32(a0 + 0x14, 0xFFFFFFFFu);
        put32(a0 + 0x18, 0xFFFFFFFFu);
    }
    /* area 1: owns every edge and the one cover entry, and heads both lists. */
    {
        unsigned char *a1 = b->p + b->off[SH_AAS_L_AREAS] + 44;
        put16(a1 + 0x06, 4);        /* numEdges */
        put32(a1 + 0x08, 0);        /* firstEdgeIndex */
        put16(a1 + 0x0C, 0);        /* cluster */
        put32(a1 + 0x10, 0);        /* firstObstaclePVS */
        put32(a1 + 0x14, 0);        /* firstReachabilityFrom -> reach 0 */
        put32(a1 + 0x18, 0);        /* firstReachabilityTo   -> reach 0 */
        put16(a1 + 0x20, 0);        /* firstAreaCoverIndex */
        put16(a1 + 0x22, 1);        /* numAreaCover */
    }
    /* the one reachability: area 1 to itself, ending both lists */
    {
        unsigned char *r = b->p + b->off[SH_AAS_L_REACHABILITIES];
        put16(r + 0x06, 1);
        put16(r + 0x08, 1);
        put32(r + 0x20, 0xFFFFFFFFu);
        put32(r + 0x24, 0xFFFFFFFFu);
    }
    /* the BSP: one root over two leaves, both naming area 1 */
    {
        unsigned char *n = b->p + b->off[SH_AAS_L_NODES];
        put32(n + 8, 1); put32(n + 12, 2);
        put32(n + 16 + 8, 0xFFFFFFFFu);
        put32(n + 32 + 8, 0xFFFFFFFFu);
    }
    /* areaBounds: one per area, the second a real box */
    {
        unsigned char *ab = b->p + b->off[SH_AAS_L_AREABOUNDS] + 12;
        put16(ab + 0, 0);    put16(ab + 2, 0);    put16(ab + 4, 32);
        put16(ab + 6, 128);  put16(ab + 8, 128);  put16(ab + 10, 106);
    }
    /* dependencyName padding, distinguishable per record */
    for (i = 0; i < pad; i++) {
        unsigned char *d = b->p + b->off[SH_AAS_L_DEPENDENCYNAMES] + (size_t)i * 128;
        _snprintf_s((char *)d, 128, _TRUNCATE, "dependency_%u", i);
    }
    return 1;
}

static void free_synth(aas_synth *b) { free(b->p); b->p = NULL; }

/* ==================================================================== */
/* the format table                                                      */
/* ==================================================================== */

static void test_record_sizes(void)
{
    int i;
    for (i = 0; i < SH_AAS_L__COUNT; i++)
        CHECK(sh_aas_record_size(i) == AAS_REC[i]);

    /* a lump ordinal nobody defines has no record size, rather than reading
     * off the end of the table */
    CHECK(sh_aas_record_size(-1) == 0);
    CHECK(sh_aas_record_size(SH_AAS_L__COUNT) == 0);
    CHECK(sh_aas_record_size(9999) == 0);
}

/* ==================================================================== */
/* the big-endian edge                                                   */
/* ==================================================================== */

/* The store is big-endian and the host is not, so these helpers are the only
 * thing standing between an author's coordinate and a byte-swapped navmesh. */
static void test_field_helpers(void)
{
    unsigned char rec[32];

    memset(rec, 0xCC, sizeof rec);
    sh_aas_put_u32(rec, 4, 0x01020304u);
    CHECK(rec[4] == 0x01 && rec[5] == 0x02 && rec[6] == 0x03 && rec[7] == 0x04);
    CHECK(sh_aas_get_u32(rec, 4) == 0x01020304u);
    CHECK(rec[3] == 0xCC && rec[8] == 0xCC);        /* nothing either side moved */

    sh_aas_put_u16(rec, 12, 0xBEEFu);
    CHECK(rec[12] == 0xBE && rec[13] == 0xEF);
    CHECK(sh_aas_get_u16(rec, 12) == 0xBEEFu);

    sh_aas_put_i32(rec, 16, -2);
    CHECK(rec[16] == 0xFF && rec[17] == 0xFF && rec[18] == 0xFF && rec[19] == 0xFE);
    CHECK(sh_aas_get_i32(rec, 16) == -2);
    CHECK(sh_aas_get_u32(rec, 16) == 0xFFFFFFFEu);

    sh_aas_put_i16(rec, 20, -3);
    CHECK(sh_aas_get_i16(rec, 20) == -3);           /* sign-extended, not 65533 */
    CHECK(sh_aas_get_u16(rec, 20) == 0xFFFDu);

    /* floats travel big-endian too, which is the easy one to get wrong */
    sh_aas_put_f32(rec, 24, 1.0f);
    CHECK(rec[24] == 0x3F && rec[25] == 0x80 && rec[26] == 0x00 && rec[27] == 0x00);
    CHECK(sh_aas_get_f32(rec, 24) == 1.0f);
    sh_aas_put_f32(rec, 24, -18.5f);
    CHECK(sh_aas_get_f32(rec, 24) == -18.5f);
}

/* ==================================================================== */
/* parse, and the byte-for-byte round trip                               */
/* ==================================================================== */

static void roundtrip_one(unsigned pad)
{
    aas_synth b;
    char err[256];
    sh_aas *a;
    unsigned char *out;
    size_t out_len = 0;
    int i;

    if (!synth_aas(&b, pad)) { CHECK(0); return; }

    err[0] = 'x';
    a = sh_aas_parse(b.p, b.len, err, sizeof err);
    CHECK(a != NULL);
    CHECK(err[0] == '\0');                          /* success clears the reason */
    if (!a) { free_synth(&b); return; }

    for (i = 0; i < SH_AAS_L__COUNT; i++)
        CHECK(sh_aas_count(a, i) == b.count[i]);

    out = sh_aas_write(a, &out_len);
    CHECK(out != NULL);
    if (out) {
        CHECK(out_len == b.len);
        CHECK(out_len == b.len && memcmp(out, b.p, b.len) == 0);
        HeapFree(GetProcessHeap(), 0, out);
    }
    sh_aas_free(a);
    free_synth(&b);
}

static void test_roundtrip_is_byte_identical(void)
{
    /* an empty tail lump, a small one, and one crossing the growth minimum */
    roundtrip_one(0);
    roundtrip_one(1);
    roundtrip_one(17);
}

/* Preserve record bytes, including header and settings fields the model does not decode. */
static void test_record_access(void)
{
    aas_synth b;
    sh_aas *a;
    const unsigned char *v;

    if (!synth_aas(&b, 2)) { CHECK(0); return; }
    a = sh_aas_parse(b.p, b.len, NULL, 0);          /* err is optional */
    CHECK(a != NULL);
    if (!a) { free_synth(&b); return; }

    v = sh_aas_rec_const(a, SH_AAS_L_VERTICES, 2);
    CHECK(v != NULL);
    if (v) {
        CHECK(sh_aas_get_f32(v, 0) == 128.0f);
        CHECK(sh_aas_get_f32(v, 4) == 128.0f);
        CHECK(sh_aas_get_f32(v, 8) == 32.0f);
    }
    CHECK(sh_aas_get_u32(sh_aas_rec(a, SH_AAS_L_EDGES, 3), 0) == 3);
    CHECK(sh_aas_get_u16(sh_aas_rec(a, SH_AAS_L_AREAS, 1), 0x06) == 4);
    CHECK(sh_aas_get_i32(sh_aas_rec(a, SH_AAS_L_AREAS, 0), 0x14) == -1);
    CHECK(strcmp((const char *)sh_aas_rec(a, SH_AAS_L_DEPENDENCYNAMES, 1),
                 "dependency_1") == 0);

    /* out of range in every direction, and an empty lump has no record 0 */
    CHECK(sh_aas_rec(a, SH_AAS_L_VERTICES, 4) == NULL);
    CHECK(sh_aas_rec(a, SH_AAS_L_HINTNODES, 0) == NULL);
    CHECK(sh_aas_rec(a, -1, 0) == NULL);
    CHECK(sh_aas_rec(a, SH_AAS_L__COUNT, 0) == NULL);
    CHECK(sh_aas_rec(NULL, SH_AAS_L_VERTICES, 0) == NULL);
    CHECK(sh_aas_count(NULL, SH_AAS_L_VERTICES) == 0);
    CHECK(sh_aas_count(a, SH_AAS_L__COUNT) == 0);

    sh_aas_free(a);
    free_synth(&b);
}

/* Every refusal names its reason. A payload this module will not edit is one
 * navmesh.c would not serve either: the structural rules are the same rules. */
static void test_parse_refusals(void)
{
    aas_synth b;
    char err[256];
    sh_aas *a;

    CHECK(sh_aas_parse(NULL, 4096, err, sizeof err) == NULL);
    CHECK(err[0] != '\0');

    /* too small to hold a preamble and 22 counts */
    {
        unsigned char tiny[64];
        memset(tiny, 0, sizeof tiny);
        memcpy(tiny, "2SAA", 4);
        CHECK(sh_aas_parse(tiny, sizeof tiny, err, sizeof err) == NULL);
        CHECK(strstr(err, "too small") != NULL);
    }

    /* over the one-payload budget, refused before anything is read */
    {
        size_t big = (size_t)SH_SMNAV_MAX_PAYLOAD + 1;
        unsigned char *p = (unsigned char *)calloc(1, big);
        if (p) {
            memcpy(p, "2SAA", 4);
            p[4] = SH_AAS_MAJOR;
            p[5] = SH_AAS_MINOR;
            CHECK(sh_aas_parse(p, big, err, sizeof err) == NULL);
            CHECK(strstr(err, "budget") != NULL);
            free(p);
        }
    }

    if (!synth_aas(&b, 0)) { CHECK(0); return; }

    b.p[1] = 'X';
    CHECK(sh_aas_parse(b.p, b.len, err, sizeof err) == NULL);
    CHECK(strstr(err, "magic") != NULL);
    b.p[1] = 'S';

    b.p[5] = 22;
    CHECK(sh_aas_parse(b.p, b.len, err, sizeof err) == NULL);
    CHECK(strstr(err, "version") != NULL);
    b.p[5] = SH_AAS_MINOR;

    /* a count past the per-lump cap, before it is ever multiplied out */
    put32(b.p + b.off[SH_AAS_L_PLANES] - 4, SH_AAS_MAX_RECORDS + 1);
    CHECK(sh_aas_parse(b.p, b.len, err, sizeof err) == NULL);
    CHECK(strstr(err, "planes") != NULL);
    put32(b.p + b.off[SH_AAS_L_PLANES] - 4, b.count[SH_AAS_L_PLANES]);

    /* a count that fits the cap but not the payload */
    put32(b.p + b.off[SH_AAS_L_EDGES] - 4, 100000);
    CHECK(sh_aas_parse(b.p, b.len, err, sizeof err) == NULL);
    CHECK(strstr(err, "overrun") != NULL);
    put32(b.p + b.off[SH_AAS_L_EDGES] - 4, b.count[SH_AAS_L_EDGES]);

    /* one byte short of what the last lump claims */
    CHECK(sh_aas_parse(b.p, b.len - 1, err, sizeof err) == NULL);
    CHECK(strstr(err, "areaBounds") != NULL && strstr(err, "overrun") != NULL);

    /* Dropping the final areaBounds record preserves the file end but violates
     * the required one-record-per-area count. */
    put32(b.p + b.off[SH_AAS_L_AREABOUNDS] - 4, 1);
    CHECK(sh_aas_parse(b.p, b.len - 12, err, sizeof err) == NULL);
    CHECK(strstr(err, "areaBounds") != NULL && strstr(err, "2 areas") != NULL);
    put32(b.p + b.off[SH_AAS_L_AREABOUNDS] - 4, b.count[SH_AAS_L_AREABOUNDS]);

    /* the good payload still parses after every one of those was undone */
    a = sh_aas_parse(b.p, b.len, err, sizeof err);
    CHECK(a != NULL);
    sh_aas_free(a);

    /* one byte more than the lumps account for */
    {
        unsigned char *longer = (unsigned char *)malloc(b.len + 1);
        if (longer) {
            memcpy(longer, b.p, b.len);
            longer[b.len] = 0;
            CHECK(sh_aas_parse(longer, b.len + 1, err, sizeof err) == NULL);
            CHECK(strstr(err, "trailing") != NULL);
            free(longer);
        }
    }
    free_synth(&b);
}

/* ==================================================================== */
/* appending                                                             */
/* ==================================================================== */

/* Growth is the part with a memory bug in it if there is one: the array
 * reallocates under records the caller already wrote. */
static void test_append_grows_and_preserves(void)
{
    aas_synth b;
    sh_aas *a;
    unsigned first = 0xFFFFFFFFu, i;

    if (!synth_aas(&b, 0)) { CHECK(0); return; }
    a = sh_aas_parse(b.p, b.len, NULL, 0);
    CHECK(a != NULL);
    if (!a) { free_synth(&b); return; }

    /* one past the parsed count: the first growth, to the 16-record minimum */
    CHECK(sh_aas_append(a, SH_AAS_L_VERTICES, 1, &first) == 1);
    CHECK(first == 4);
    CHECK(sh_aas_count(a, SH_AAS_L_VERTICES) == 5);
    /* appended records arrive zeroed, not holding whatever the heap had */
    for (i = 0; i < 12; i++)
        CHECK(sh_aas_rec(a, SH_AAS_L_VERTICES, 4)[i] == 0);
    sh_aas_put_f32(sh_aas_rec(a, SH_AAS_L_VERTICES, 4), 0, 640.0f);

    /* enough more to force a doubling, twice */
    CHECK(sh_aas_append(a, SH_AAS_L_VERTICES, 60, &first) == 1);
    CHECK(first == 5);
    CHECK(sh_aas_count(a, SH_AAS_L_VERTICES) == 65);
    /* the record written before the reallocation still reads back */
    CHECK(sh_aas_get_f32(sh_aas_rec(a, SH_AAS_L_VERTICES, 4), 0) == 640.0f);
    /* and so does one the file brought */
    CHECK(sh_aas_get_f32(sh_aas_rec_const(a, SH_AAS_L_VERTICES, 2), 4) == 128.0f);
    CHECK(sh_aas_rec(a, SH_AAS_L_VERTICES, 65) == NULL);

    /* appending nothing is not an error, and reports where the end is */
    CHECK(sh_aas_append(a, SH_AAS_L_VERTICES, 0, &first) == 1);
    CHECK(first == 65);
    CHECK(sh_aas_count(a, SH_AAS_L_VERTICES) == 65);

    /* a lump that started empty grows from nothing */
    CHECK(sh_aas_append(a, SH_AAS_L_HINTNODES, 3, &first) == 1);
    CHECK(first == 0);
    CHECK(sh_aas_count(a, SH_AAS_L_HINTNODES) == 3);
    CHECK(sh_aas_rec(a, SH_AAS_L_HINTNODES, 0) != NULL);

    /* out_first is optional */
    CHECK(sh_aas_append(a, SH_AAS_L_HINTNODES, 1, NULL) == 1);
    CHECK(sh_aas_count(a, SH_AAS_L_HINTNODES) == 4);

    CHECK(sh_aas_append(NULL, SH_AAS_L_VERTICES, 1, &first) == 0);
    CHECK(sh_aas_append(a, -1, 1, &first) == 0);
    CHECK(sh_aas_append(a, SH_AAS_L__COUNT, 1, &first) == 0);

    sh_aas_free(a);
    free_synth(&b);
}

/* What is appended is what comes back out of the written file: the whole point
 * of the module is that an augmented payload re-parses. */
static void test_append_survives_a_round_trip(void)
{
    aas_synth b;
    sh_aas *a, *again;
    unsigned char *out;
    size_t out_len = 0;
    unsigned first = 0, area = 0, bounds = 0;

    if (!synth_aas(&b, 1)) { CHECK(0); return; }
    a = sh_aas_parse(b.p, b.len, NULL, 0);
    CHECK(a != NULL);
    if (!a) { free_synth(&b); return; }

    CHECK(sh_aas_append(a, SH_AAS_L_VERTICES, 2, &first) == 1);
    sh_aas_put_f32(sh_aas_rec(a, SH_AAS_L_VERTICES, first), 0, -1.5f);
    sh_aas_put_f32(sh_aas_rec(a, SH_AAS_L_VERTICES, first), 4, 2.25f);
    sh_aas_put_f32(sh_aas_rec(a, SH_AAS_L_VERTICES, first + 1), 8, 96.0f);

    /* one area needs its areaBounds row, or the result would not re-parse */
    CHECK(sh_aas_append(a, SH_AAS_L_AREAS, 1, &area) == 1);
    CHECK(sh_aas_append(a, SH_AAS_L_AREABOUNDS, 1, &bounds) == 1);
    CHECK(area == 2 && bounds == 2);
    sh_aas_put_i32(sh_aas_rec(a, SH_AAS_L_AREAS, area), 0x14, -1);
    sh_aas_put_i32(sh_aas_rec(a, SH_AAS_L_AREAS, area), 0x18, -1);
    sh_aas_put_u16(sh_aas_rec(a, SH_AAS_L_AREABOUNDS, bounds), 10, 106);

    out = sh_aas_write(a, &out_len);
    CHECK(out != NULL);
    if (out) {
        /* two vertices, one area and one areaBounds row longer than the input */
        CHECK(out_len == b.len + 2 * 12 + 44 + 12);
        again = sh_aas_parse(out, out_len, NULL, 0);
        CHECK(again != NULL);
        if (again) {
            CHECK(sh_aas_count(again, SH_AAS_L_VERTICES) == 6);
            CHECK(sh_aas_count(again, SH_AAS_L_AREAS) == 3);
            CHECK(sh_aas_count(again, SH_AAS_L_AREABOUNDS) == 3);
            CHECK(sh_aas_get_f32(sh_aas_rec_const(again, SH_AAS_L_VERTICES, 4), 0) == -1.5f);
            CHECK(sh_aas_get_f32(sh_aas_rec_const(again, SH_AAS_L_VERTICES, 4), 4) == 2.25f);
            CHECK(sh_aas_get_f32(sh_aas_rec_const(again, SH_AAS_L_VERTICES, 5), 8) == 96.0f);
            CHECK(sh_aas_get_i32(sh_aas_rec_const(again, SH_AAS_L_AREAS, 2), 0x14) == -1);
            CHECK(sh_aas_get_u16(sh_aas_rec_const(again, SH_AAS_L_AREABOUNDS, 2), 10) == 106);
            /* everything the file brought is still exactly where it was */
            CHECK(memcmp(out, b.p, SH_AAS_PREAMBLE_BYTES) == 0);
            CHECK(sh_aas_get_f32(sh_aas_rec_const(again, SH_AAS_L_VERTICES, 2), 0) == 128.0f);
            CHECK(strcmp((const char *)sh_aas_rec_const(again, SH_AAS_L_DEPENDENCYNAMES, 0),
                         "dependency_0") == 0);
            sh_aas_free(again);
        }
        HeapFree(GetProcessHeap(), 0, out);
    }
    sh_aas_free(a);
    free_synth(&b);
}

/* Reject area counts that exceed the format's u16 index limit before allocating. */
static void test_append_caps(void)
{
    aas_synth b;
    sh_aas *a;
    unsigned first = 0;

    if (!synth_aas(&b, 0)) { CHECK(0); return; }
    a = sh_aas_parse(b.p, b.len, NULL, 0);
    CHECK(a != NULL);
    if (!a) { free_synth(&b); return; }

    CHECK(sh_aas_append(a, SH_AAS_L_AREAS, SH_AAS_MAX_AREAS - 1, &first) == 0);
    CHECK(sh_aas_count(a, SH_AAS_L_AREAS) == 2);

    CHECK(sh_aas_append(a, SH_AAS_L_EDGEINDEX, SH_AAS_MAX_RECORDS, &first) == 0);
    CHECK(sh_aas_count(a, SH_AAS_L_EDGEINDEX) == 4);

    /* a count that would wrap a 32-bit sum is refused, not wrapped */
    CHECK(sh_aas_append(a, SH_AAS_L_EDGEINDEX, 0xFFFFFFFFu, &first) == 0);
    CHECK(sh_aas_count(a, SH_AAS_L_EDGEINDEX) == 4);

    /* and the model is still usable afterwards */
    CHECK(sh_aas_append(a, SH_AAS_L_EDGEINDEX, 1, &first) == 1);
    CHECK(first == 4);

    sh_aas_free(a);
    free_synth(&b);
}

/* The writer refuses rather than handing the shard packer a payload the engine
 * would never be given. */
static void test_write_refuses_over_budget(void)
{
    aas_synth b;
    sh_aas *a;
    unsigned char *out;
    size_t out_len = 12345;
    unsigned n;

    if (!synth_aas(&b, 0)) { CHECK(0); return; }
    a = sh_aas_parse(b.p, b.len, NULL, 0);
    CHECK(a != NULL);
    if (!a) { free_synth(&b); return; }

    n = (unsigned)((SH_SMNAV_MAX_PAYLOAD - b.len) / 132) + 1;
    CHECK(sh_aas_append(a, SH_AAS_L_REACHNAMES, n, NULL) == 1);
    CHECK(sh_aas_count(a, SH_AAS_L_REACHNAMES) == n);

    out = sh_aas_write(a, &out_len);
    CHECK(out == NULL);
    CHECK(out_len == 0);                            /* and the length is cleared */
    if (out) HeapFree(GetProcessHeap(), 0, out);

    CHECK(sh_aas_write(NULL, &out_len) == NULL);
    sh_aas_free(a);
    free_synth(&b);
}

/* ==================================================================== */
/* the settings block                                                    */
/* ==================================================================== */

/* Agent radius and height must come from the correct settings words. */
static void test_settings_accessors(void)
{
    aas_synth b;
    sh_aas *a;
    unsigned char *out;
    size_t out_len = 0;
    float mins[3], maxs[3];

    if (!synth_aas(&b, 0)) { CHECK(0); return; }
    a = sh_aas_parse(b.p, b.len, NULL, 0);
    CHECK(a != NULL);
    if (!a) { free_synth(&b); return; }

    sh_aas_agent_bounds(a, mins, maxs);
    CHECK(mins[0] == -24.0f && mins[1] == -24.0f && mins[2] == 0.0f);
    CHECK(maxs[0] == 24.0f && maxs[1] == 24.0f && maxs[2] == 74.0f);
    CHECK(sh_aas_max_step_height(a) == 18.0f);

    /* the raw accessor sees the same words at the same offsets */
    CHECK(sh_aas_setting_f32(a, SET_WORDS_OFF + SET_W_MAXSTEP * 4) == 18.0f);
    CHECK(sh_aas_setting_f32(a, SET_WORDS_OFF + 5 * 4) == 74.0f);

    /* a write lands in the block and comes back out of the file */
    sh_aas_set_setting_f32(a, SET_WORDS_OFF + SET_W_MAXSTEP * 4, 24.0f);
    CHECK(sh_aas_max_step_height(a) == 24.0f);
    out = sh_aas_write(a, &out_len);
    CHECK(out != NULL);
    if (out) {
        sh_aas *again = sh_aas_parse(out, out_len, NULL, 0);
        CHECK(again != NULL);
        if (again) {
            CHECK(sh_aas_max_step_height(again) == 24.0f);
            sh_aas_agent_bounds(again, mins, maxs);
            CHECK(mins[0] == -24.0f && maxs[2] == 74.0f);
            sh_aas_free(again);
        }
        /* exactly four bytes of the file changed, and they are in the block */
        CHECK(out_len == b.len);
        CHECK(memcmp(out, b.p, SH_AAS_HEADER_BYTES + SET_WORDS_OFF + SET_W_MAXSTEP * 4) == 0);
        CHECK(memcmp(out + SH_AAS_HEADER_BYTES + SET_WORDS_OFF + (SET_W_MAXSTEP + 1) * 4,
                     b.p + SH_AAS_HEADER_BYTES + SET_WORDS_OFF + (SET_W_MAXSTEP + 1) * 4,
                     b.len - (SH_AAS_HEADER_BYTES + SET_WORDS_OFF + (SET_W_MAXSTEP + 1) * 4)) == 0);
        HeapFree(GetProcessHeap(), 0, out);
    }

    /* an offset off the end of the block reads zero and writes nothing */
    CHECK(sh_aas_setting_f32(a, SH_AAS_SETTINGS_BYTES) == 0.0f);
    CHECK(sh_aas_setting_f32(a, SH_AAS_SETTINGS_BYTES - 3) == 0.0f);
    CHECK(sh_aas_setting_f32(a, 0xFFFFFFFFu) == 0.0f);
    CHECK(sh_aas_setting_f32(NULL, 0) == 0.0f);
    sh_aas_set_setting_f32(a, 0xFFFFFFFFu, 1.0f);
    sh_aas_set_setting_f32(NULL, 0, 1.0f);
    CHECK(sh_aas_max_step_height(a) == 24.0f);      /* nothing was corrupted */

    /* a model that is not there answers a defined zero, not a fault */
    sh_aas_agent_bounds(NULL, mins, maxs);
    CHECK(mins[0] == 0.0f && maxs[2] == 0.0f);
    CHECK(sh_aas_max_step_height(NULL) == 0.0f);

    sh_aas_free(a);
    sh_aas_free(NULL);
    free_synth(&b);
}

static void test_truncate_preserves_prefix_and_clears_regrowth(void)
{
    aas_synth b;
    sh_aas *a;
    unsigned char *out;
    size_t len;
    unsigned first;
    if (!synth_aas(&b, 2)) { CHECK(0); return; }
    a = sh_aas_parse(b.p, b.len, NULL, 0);
    CHECK(a != NULL);
    if (!a) { free_synth(&b); return; }
    CHECK(sh_aas_append(a, SH_AAS_L_DEPENDENCYNAMES, 3, &first));
    memset(sh_aas_rec(a, SH_AAS_L_DEPENDENCYNAMES, first), 'x',
           sh_aas_record_size(SH_AAS_L_DEPENDENCYNAMES));
    CHECK(!sh_aas_truncate(a, SH_AAS_L_DEPENDENCYNAMES, first + 4));
    CHECK(sh_aas_truncate(a, SH_AAS_L_DEPENDENCYNAMES, first));
    CHECK(sh_aas_rec(a, SH_AAS_L_DEPENDENCYNAMES, first) == NULL);
    out = sh_aas_write(a, &len);
    CHECK(out && len == b.len && !memcmp(out, b.p, len));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    CHECK(sh_aas_append(a, SH_AAS_L_DEPENDENCYNAMES, 1, &first));
    CHECK(sh_aas_rec(a, SH_AAS_L_DEPENDENCYNAMES, first)[0] == 0);
    CHECK(!sh_aas_truncate(NULL, SH_AAS_L_DEPENDENCYNAMES, 0));
    CHECK(!sh_aas_truncate(a, SH_AAS_L__COUNT, 0));
    sh_aas_free(a);
    free_synth(&b);
}

int main(void)
{
    test_record_sizes();
    test_field_helpers();
    test_roundtrip_is_byte_identical();
    test_record_access();
    test_parse_refusals();
    test_append_grows_and_preserves();
    test_append_survives_a_round_trip();
    test_append_caps();
    test_write_refuses_over_budget();
    test_settings_accessors();
    test_truncate_preserves_prefix_and_clears_regrowth();

    if (g_failed) {
        fprintf(stderr, "%d check(s) FAILED\n", g_failed);
        return 1;
    }
    printf("aas_edit_test: all checks passed\n");
    return 0;
}
