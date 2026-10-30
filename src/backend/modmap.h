/* modmap.h -- the open map's entities, wires and timeline references, read
 * from its serialized JSON for the Mods tab (modui.c).
 *
 * The Mods tab removes a mod from the open map by deleting the entities that
 * use it. Which entities those are, and what else points at them, is read
 * from the same text a save writes (the editor's SerializeToJson output):
 *
 *   entities    each entity's uniqueId (the editor's entity id), displayName,
 *               className and its placed names: the inherit and every string
 *               inside an object keyed "edit", the rule the save-time
 *               manifest closure uses (modmanifest.c)
 *   instances   each placed module's moduleName
 *   targets     the wires: connections, a run-length list of
 *               -(source + 1) followed by that source's targets
 *   references  entityEntRefs, one row of entity references per uniqueId
 *               (an idIndexMultimap: row u is values[kv[u] .. kv[u + 1]))
 *
 * A reference to a set of entities is a wire with exactly one end in the set,
 * or a Timeline (className idTarget_Timeline) outside the set whose
 * entityEntRefs row names an entity in it. Other entity references are not
 * reported. Pure C over mp_json; no engine dependency. */
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
    char **names;          /* the placed names, decoded, without duplicates */
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
    long long    *ref_kv, *ref_v;    /* entityEntRefs keyValues and values */
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

/* What points at the entities `set` (uids, any order, count n) from outside
 * it: one entry per wire with exactly one end in the set, then one per
 * Timeline outside the set naming one or more of them. *out is malloc'd
 * (free); returns the count, or (size_t)-1 without memory. */
size_t sh_mm_references(const sh_mm_map *map, const int *set, size_t n, sh_mm_ref **out);

/* Decode a JSON string body (the escaped text between the quotes) into a
 * malloc'd string; NULL without memory. \uXXXX becomes UTF-8. */
char *sh_mm_decode(const char *escaped);

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_MODMAP_H */
