/* module_prefab_map.h -- cut Blueprint modules out of a saved map and merge them into another: pure functions
 * over idSnapMap JSON, no engine dependency. A module prefab file is an idSnapMap holding only the chosen
 * instances (origins relative to the first, the anchor), their entities (uniqueIds unchanged), portal slots
 * and wires with both ends inside the set; entity references that leave the set become -1 and variable
 * references (type << 24 | index, 0xFFFFFF unbound) are remapped to the carried variables. Entity positions
 * are module-local. Variable type 9 (persistentInteger) is fixed: never carried or remapped. */
#ifndef SH_MODULE_PREFAB_MAP_H
#define SH_MODULE_PREFAB_MAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MP_MAX_ENVS      16
#define SH_MP_NAME_CAP      260
#define SH_MP_MAX_MODULES   1024

typedef struct sh_mp_module {
    char  name[SH_MP_NAME_CAP];   /* moduleName (decl path) */
    float origin[3];              /* relative to the anchor */
    int   rotation;               /* orientation, 0..7 */
} sh_mp_module;

typedef struct sh_mp_summary {
    int module_count;
    int entity_count;
    int door_count;               /* portal slots (doorways, capped or open) */
    int variable_count;           /* carried variables, persistent integers excluded */
    int dropped_wires;            /* wires with one end outside the set */
    int dropped_refs;             /* entity references set to -1 at the boundary */
    int has_nav_boxes;            /* a Blocking Box ticked for AI Navigation (flags.noFlood) */
    int env_count;
    char envs[SH_MP_MAX_ENVS][SH_MP_NAME_CAP];
    sh_mp_module *modules;        /* module_count entries, malloc-owned */
} sh_mp_summary;

void sh_mp_summary_free(sh_mp_summary *s);

/* Cut the instances sel[0..nsel) (sel[0] is the anchor) out of map: 1 with *out (malloc-owned,
 * NUL-terminated) and *sum, or 0 with a reason in err. */
int sh_mp_cut(const char *map, size_t len, const int *sel, int nsel,
              char **out, size_t *out_len, sh_mp_summary *sum, char *err, int errcap);

/* Merge the prefab frag into map with the anchor at `at` (world): new entities get fresh uniqueIds, variables
 * are appended and remapped. Returns 1 with *out (*first_instance, *added set), or 0 with a reason in err. */
int sh_mp_merge(const char *map, size_t len, const char *frag, size_t flen, const float at[3],
                char **out, size_t *out_len, int *first_instance, int *added, char *err, int errcap);

/* Re-derive a summary from a prefab file. 1 ok. Dropped counts are 0 (they are
 * only known at cut time). */
int sh_mp_inspect(const char *frag, size_t flen, sh_mp_summary *sum, char *err, int errcap);

/* 1 when text's top-level "~type" is "idSnapMap" (a module prefab), 0 when it
 * is anything else, including "idSnapEntityPrefab". Scans only the top level. */
int sh_mp_is_module_file(const char *text, size_t len);

/* The module guard: a prefab is placed only when every module it names is in the editor's module palette,
 * a Grid Room size, or served by an installed mod. For any other name the map validator drops the instance
 * and keeps its entities, and the record builder raises "Couldn't find module in modulePalette". */

/* known(ctx, name): 1 the receiver can build the module, 0 it cannot, -1 it
 * cannot tell (no editor palette yet). */
typedef int (*sh_mp_module_known_fn)(void *ctx, const char *name);

#define SH_MP_CHECK_NAMES 8
typedef struct sh_mp_module_check {
    int checked;            /* distinct names the callback answered (0 or 1) */
    int unknown;            /* distinct names it could not tell (-1) */
    int missing_count;      /* distinct names it cannot build, all of them */
    int listed;             /* names kept in missing[], at most SH_MP_CHECK_NAMES */
    char missing[SH_MP_CHECK_NAMES][SH_MP_NAME_CAP];
} sh_mp_module_check;

/* Ask known() once for each distinct moduleName in s (ASCII case-insensitive).
 * An empty name is missing. Returns missing_count, or -1 when no name could be
 * answered (nothing is known about the palette: the caller must not refuse). */
int sh_mp_check_modules(const sh_mp_summary *s, sh_mp_module_known_fn known, void *ctx,
                        sh_mp_module_check *out);

/* "maps/modules/ind_dlc/ind_room.decl" -> "ind_dlc/ind_room": the name a toast shows. */
void sh_mp_module_short_name(const char *name, char *out, size_t cap);

/* The refusal texts for a check with missing_count > 0; each returns the length written (truncated to cap).
 * The console line names every listed module in full, the toast by short name, the status is the Prefabs
 * tab line shown before Load. */
int sh_mp_missing_console_line(const sh_mp_module_check *c, char *out, size_t cap);
int sh_mp_missing_toast(const sh_mp_module_check *c, char *out, size_t cap);
int sh_mp_missing_status(const sh_mp_module_check *c, char *out, size_t cap);

/* The Prefabs tab's check result: {"checked":true,"missing":["<name>",...],"more":N},
 * or {"checked":false} when check is NULL or nothing could be answered. Returns
 * malloc-owned text or NULL. */
char *sh_mp_check_json(const sh_mp_module_check *c);

/* The summary view the save composes into the prefab's snapmapPlus block (prefab_mods.h); desc and tags are
 * written empty. bounds (may be NULL) is one anchor-relative box per module (min xyz, max xyz), labels (may
 * be NULL) the display names. Returns malloc-owned text or NULL. */
char *sh_mp_summary_view_json(const sh_mp_summary *s, const float (*bounds)[6], const char *const *labels);

/* ModuleRedoAction (Module Mode redo handler) calls Redo(editor) with an E8 rel32 at this offset. */
#define SH_MP_ACTION_REDO_CALL_OFF 0x13

/* Follow a rel32 CALL at `at`: its target, NULL unless the byte there is E8 (or `at` is unreadable). */
void *sh_mp_call_target(const unsigned char *at);

/* The Redo the install binds: the call target at ModuleRedoAction + SH_MP_ACTION_REDO_CALL_OFF, NULL when
 * that is not an E8 call. */
void *sh_mp_redo_target(const unsigned char *redo_action);

#ifdef __cplusplus
}
#endif

#endif
