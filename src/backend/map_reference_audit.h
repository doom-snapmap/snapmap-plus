/* What the product can establish, from a map's text alone, about the tables
 * the engine keys by entity uniqueId: references.entityEntRefs and
 * references.entityVarRefs (prefix sums over uniqueId, each entity owning
 * keyValues[uid+1] - keyValues[uid] rows), instanceEntities (prefix sums over
 * instances, values naming uniqueIds) and targets.connections (runs whose
 * source is stored as -(uid + 1)).
 *
 * The engine's load validator requires every entity's row count to equal the
 * count its inherited declaration expects. That expectation is not in the
 * text, but it is one value per inherit, so entities sharing an inherit must
 * own equal row counts; a disagreement is reported as a problem. Rows at a
 * uniqueId no entity holds are loaded and ignored, but an entity the editor
 * later mints at that uniqueId inherits them, so they are reported as hazards.
 *
 * Diagnostics only: nothing here changes or refuses a map. */
#ifndef SNAPMAP_PLUS_MAP_REFERENCE_AUDIT_H
#define SNAPMAP_PLUS_MAP_REFERENCE_AUDIT_H

#include <stddef.h>

typedef struct sh_map_reference_audit {
    int readable;          /* 0: the text or its tables did not read */
    unsigned problems;     /* inconsistencies the engine's load may refuse */
    unsigned hazards;      /* rows held by no entity */
    char problem_text[768];
    char hazard_text[384];
} sh_map_reference_audit;

/* Audit `json` (length bytes, not necessarily NUL-terminated). Returns 1 when
 * the tables read (out->readable), 0 otherwise; out is always filled. */
int sh_map_reference_audit_run(const char *json, size_t length, sh_map_reference_audit *out);

#endif
