/* Mutable AAS2 3.29 payloads: parse, append records, and serialize.
 *
 * The file has a 30-byte header, 364-byte settings block, and 22 count-
 * prefixed lumps of fixed-size big-endian records. Each lump owns its bytes
 * so appends do not move other lumps. Untouched records are copied verbatim
 * for byte-identical round trips.
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

/* Parse the header, lump bounds and count relationships. Returns NULL and
 * fills err on failure. Cross-lump indices and BSP integrity require
 * sh_navmesh_validate_aas after editing.
 */
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

/* Remove a tail after compaction. Does not repair cross-lump references. */
int sh_aas_truncate(sh_aas *a, int lump, unsigned count);

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

/* Settings occupy 364 bytes at SH_AAS_HEADER_BYTES. Offsets below are
 * relative to that block. Agent bounds determine clearance; maxStepHeight
 * limits walk links.
 */
float sh_aas_setting_f32(const sh_aas *a, unsigned off);
void  sh_aas_set_setting_f32(sh_aas *a, unsigned off, float v);

/* The nav class's agent box, decoded from the settings block. */
void sh_aas_agent_bounds(const sh_aas *a, float out_mins[3], float out_maxs[3]);
float sh_aas_max_step_height(const sh_aas *a);

#endif /* SNAPMAP_PLUS_AAS_EDIT_H */
