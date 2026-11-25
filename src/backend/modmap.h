/* The open map's entities, wires and timeline references, read from its serialized JSON (the editor's SerializeToJson
 * output) for the Mods tab (modui.c). Pure C over mp_json; no engine dependency. */
#ifndef SNAPMAP_PLUS_MODMAP_H
#define SNAPMAP_PLUS_MODMAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MM_TIMELINE_CLASS "idTarget_Timeline"

typedef struct sh_mm_entity {
    int    uid;
    char  *label;          /* displayName, decoded; "" when none */
    char  *inherit;        /* entityDef.inherit, decoded; "" when none */
    char  *class_name;     /* entityDef.className, decoded; "" when none */
    char **names;          /* the placed names (the save-time closure's rule), decoded, without duplicates */
    size_t name_count;
    int    timeline;       /* class_name is SH_MM_TIMELINE_CLASS */
} sh_mm_entity;

typedef struct sh_mm_map {
    sh_mm_entity *ents;
    size_t        ent_count;
    char        **modules;       /* instances[].moduleName, decoded */
    size_t        module_count;
    int          *wire_src, *wire_dst;
    size_t        wire_count;
    long long    *ref_kv, *ref_v;    /* entityEntRefs keyValues and values: row u is values[kv[u] .. kv[u + 1]) */
    size_t        ref_kv_n, ref_v_n;
} sh_mm_map;

enum { SH_MM_REF_WIRE = 1, SH_MM_REF_TIMELINE = 2 };

typedef struct sh_mm_ref {
    int kind;        /* SH_MM_REF_* */
    int from_uid;    /* the wire's source, or the Timeline */
    int to_uid;      /* the wire's target, or the first entity of the set the Timeline names */
} sh_mm_ref;

/* Parse a serialized map. 1 on success; 0 with the reason in err (out is
 * then empty). Missing targets or references read as none. */
int  sh_mm_parse(const char *json, size_t length, sh_mm_map *out, char *err, size_t err_cap);
void sh_mm_free(sh_mm_map *map);

/* The entity with uniqueId uid, or NULL. */
const sh_mm_entity *sh_mm_find(const sh_mm_map *map, int uid);

/* What points at the entities `set` (uids, count n) from outside it: one entry per wire with exactly one end in the
 * set, and one per Timeline outside it whose entityEntRefs row names one of them. *out is malloc'd (free); returns the
 * count, or (size_t)-1 without memory. */
size_t sh_mm_references(const sh_mm_map *map, const int *set, size_t n, sh_mm_ref **out);

/* Decode a JSON string body (the escaped text between the quotes) into a
 * malloc'd string; NULL without memory. \uXXXX becomes UTF-8. */
char *sh_mm_decode(const char *escaped);

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_MODMAP_H */
