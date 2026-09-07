/* aas_edit.h -- a MUTABLE AAS2 3.29 file: parse, append, re-serialize.
 *
 * WHY THIS EXISTS
 * ---------------
 * navmesh.c can already VALIDATE a payload -- walk the 22 count-prefixed lumps,
 * bounds-check the cross-lump indices, prove the BSP terminates. That is a
 * read-only gate over a const buffer, which is all serving substitute bytes ever
 * needed.
 *
 * Authoring needs the other half. To give an author's own geometry navigation we
 * must ADD to a shipped payload: vertices, edges, planes, one area per platform,
 * the BSP nodes that make that area findable, a cluster and obstaclePVS row, and
 * the reachabilities that link it. This module is the editable model that makes
 * that expressible, and the writer that turns it back into bytes the engine's
 * own loader accepts.
 *
 * THE SHAPE
 * ---------
 * A payload is a 30-byte header, a 364-byte settings block, then 22 lumps, each
 * a u32 count followed by that many FIXED-SIZE records. Everything multi-byte is
 * BIG-ENDIAN on disk. There is no offset table: lump N begins where lump N-1
 * ended, so appending to any lump shifts every later one -- which is exactly why
 * a mutable model has to own the whole file rather than patch it in place.
 *
 * Records stay big-endian in the backing store and are converted at the edge by
 * the typed accessors below. Only the lumps the augmenter actually reads or
 * writes get typed views; the rest are carried through verbatim. That keeps this
 * file small and makes a round-trip byte-identical by construction: an untouched
 * lump is copied, not re-encoded.
 *
 * NO GAME BYTES. Like every other test in this repo, the tests for this module
 * SYNTHESISE payloads (tests/navmesh_test.c `build_aas` is the precedent). Do not
 * add a real .aas_* file as a fixture -- see README.md.
 */
#ifndef SNAPMAP_PLUS_AAS_EDIT_H
#define SNAPMAP_PLUS_AAS_EDIT_H

#include <stddef.h>
#include <stdint.h>

/* Lump ordinals -- the on-disk order, which is also the write order. */
enum {
    SH_AAS_L_PLANES = 0,
    SH_AAS_L_VERTICES,
    SH_AAS_L_EDGES,
    SH_AAS_L_EDGEINDEX,
    SH_AAS_L_REACHABILITIES,
    SH_AAS_L_AREAS,
    SH_AAS_L_NODES,
    SH_AAS_L_PORTALS,
    SH_AAS_L_PORTALINDEX,
    SH_AAS_L_CLUSTERS,
    SH_AAS_L_OBSTACLEPVS,
    SH_AAS_L_REACHNAMES,
    SH_AAS_L_TRAVERSALANIMNAMES,
    SH_AAS_L_DEPENDENCYNAMES,
    SH_AAS_L_INTERACTIONENTITYNAMES,
    SH_AAS_L_COVER,
    SH_AAS_L_AREACOVERINDEX,
    SH_AAS_L_TOUCHINGCOVERINDEX,
    SH_AAS_L_TRAVERSALPOINTS,
    SH_AAS_L_HINTNODES,
    SH_AAS_L_TREES,
    SH_AAS_L_AREABOUNDS,
    SH_AAS_L__COUNT
};

typedef struct sh_aas sh_aas;

/* ---- lifetime ---------------------------------------------------------- */

/* Parse `len` bytes into an editable model. Returns NULL and fills `err` on any
 * structural problem -- this applies the SAME rules as sh_navmesh_validate_aas,
 * because a payload we cannot trust is one we must not edit either. */
sh_aas *sh_aas_parse(const unsigned char *bytes, size_t len, char *err, size_t err_cap);

void sh_aas_free(sh_aas *a);

/* Serialize. Returns a HeapAlloc(GetProcessHeap()) buffer the caller frees, or
 * NULL if the result would exceed SH_SMNAV_MAX_PAYLOAD. Re-serializing an
 * unmodified model reproduces its input byte for byte. */
unsigned char *sh_aas_write(const sh_aas *a, size_t *out_len);

/* ---- generic lump access ----------------------------------------------- */

unsigned sh_aas_count(const sh_aas *a, int lump);
unsigned sh_aas_record_size(int lump);

/* Pointer to record `i` of `lump`, big-endian, or NULL if out of range.
 * Invalidated by any append to the same lump. */
unsigned char       *sh_aas_rec(sh_aas *a, int lump, unsigned i);
const unsigned char *sh_aas_rec_const(const sh_aas *a, int lump, unsigned i);

/* Append `n` zeroed records; `*out_first` receives the index of the first.
 * Returns 0 on allocation failure or if the lump would exceed its cap. */
int sh_aas_append(sh_aas *a, int lump, unsigned n, unsigned *out_first);

/* ---- big-endian field helpers ------------------------------------------ */

uint32_t sh_aas_get_u32(const unsigned char *rec, unsigned off);
uint16_t sh_aas_get_u16(const unsigned char *rec, unsigned off);
int32_t  sh_aas_get_i32(const unsigned char *rec, unsigned off);
int16_t  sh_aas_get_i16(const unsigned char *rec, unsigned off);
float    sh_aas_get_f32(const unsigned char *rec, unsigned off);

void sh_aas_put_u32(unsigned char *rec, unsigned off, uint32_t v);
void sh_aas_put_u16(unsigned char *rec, unsigned off, uint16_t v);
void sh_aas_put_i32(unsigned char *rec, unsigned off, int32_t v);
void sh_aas_put_i16(unsigned char *rec, unsigned off, int16_t v);
void sh_aas_put_f32(unsigned char *rec, unsigned off, float v);

/* ---- the settings block ------------------------------------------------
 * 364 bytes at SH_AAS_HEADER_BYTES. The augmenter needs the agent bounding box
 * (to inset a platform by the agent radius and to reject one with too little
 * headroom) and maxStepHeight (the walk-link ceiling, 18 in every shipped
 * SnapMap module). Offsets are into the settings block, not the file. */
float sh_aas_setting_f32(const sh_aas *a, unsigned off);
void  sh_aas_set_setting_f32(sh_aas *a, unsigned off, float v);

/* The nav class's agent box, decoded from the settings block. */
void sh_aas_agent_bounds(const sh_aas *a, float out_mins[3], float out_maxs[3]);
float sh_aas_max_step_height(const sh_aas *a);

#endif /* SNAPMAP_PLUS_AAS_EDIT_H */
