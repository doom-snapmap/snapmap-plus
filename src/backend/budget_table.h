/* budget_table.h -- the editor budget table for the Prefabs tab's Limits section. The editor sums one 12-int
 * block (snapLimitAdjustments_t: editorModels, entities, renderModels, lights, network, thinks, worldAreas,
 * decals, renderKB, collisionKB, generalKB, numAI) per counted thing; this module reads every palette row and
 * resident module wrapper once per palette build on the game thread and keeps the result as JSON for the frontend.
 * Engine offsets (build 13954591, same on both renderers) are the named constants in budget_table.c; every engine
 * read is SEH-guarded, and a fault leaves an `error` table. */
#ifndef SH_BUDGET_TABLE_H
#define SH_BUDGET_TABLE_H

#include <stddef.h>

#define SH_BT_FIELDS        12
#define SH_BT_ENTITIES      1
#define SH_BT_RENDER_MODELS 2
#define SH_BT_NETWORK       4
#define SH_BT_GENERAL_KB    10
#define SH_BT_BASE_GENERAL  50     /* every engine-made block starts here */
#define SH_BT_AUTO_GAME     2
#define SH_BT_AUTO_VIEW     3

/* Growable text buffer; oom latches on the first failed growth. */
typedef struct sh_bt_buf { char *p; size_t len, cap; int oom; } sh_bt_buf;
void sh_bt_buf_free(sh_bt_buf *b);
void sh_bt_puts(sh_bt_buf *b, const char *s);
void sh_bt_put_str(sh_bt_buf *b, const char *s);       /* JSON string, quoted and escaped */
void sh_bt_put_int(sh_bt_buf *b, long long v);
void sh_bt_put_block(sh_bt_buf *b, const int block[SH_BT_FIELDS]);

/* Block arithmetic, as the engine does it. */
void sh_bt_base(int block[SH_BT_FIELDS]);              /* zero, generalKB 50 */
void sh_bt_add(int block[SH_BT_FIELDS], const int add[SH_BT_FIELDS]);
/* A module's own block: renderModels floors + 3 walls + 2 (portals + 2) and the card's three KB fields; no 50 KB
 * base. */
void sh_bt_module_own(int floors, int walls, int portals, int render_kb, int collision_kb,
                      int general_kb, int out[SH_BT_FIELDS]);
/* One palette miss in a template block: 1 entity, 1 render model, 1 network. */
void sh_bt_add_miss(int block[SH_BT_FIELDS]);
/* The layer filter of a module about to be placed: 1 | (decl mask & sound) | the autoView mask. */
unsigned sh_bt_filter_mask(unsigned decl_mask, unsigned sound_mask, unsigned auto_view_mask);
/* "maps/modules/x/y.decl" -> "y". */
void sh_bt_leaf(const char *decl_name, char *out, size_t cap);

/* The table under construction. */
typedef struct sh_bt_table {
    sh_bt_buf sedefs, inherits, modules, aliases;
    int nsedef, ninherit, nmodule, nalias;
    char **seen;                /* open-addressing set of inherits and leaves already keyed */
    size_t seen_cap;
} sh_bt_table;

void sh_bt_table_init(sh_bt_table *t);
void sh_bt_table_free(sh_bt_table *t);
/* Adds one palette row. The first row of an inherit owns it, as map load and paste bind an entity to the first
 * matching row. Returns 1 when this row owns its inherit. */
int sh_bt_table_add_sedef(sh_bt_table *t, const char *name, const char *inherit,
                          const int limits[SH_BT_FIELDS], int automation, int network_static);
/* A module's template entities grouped by layer mask and kind, so a caller can apply a prefab's saved instance
 * layer mask. */
#define SH_BT_PART_GAME 0
#define SH_BT_PART_VIEW 1
#define SH_BT_PART_MISS 2
#define SH_BT_MAX_PARTS 64
typedef struct sh_bt_part { unsigned mask; int kind; int block[SH_BT_FIELDS]; } sh_bt_part;
typedef struct sh_bt_parts { int count, overflow; sh_bt_part part[SH_BT_MAX_PARTS]; } sh_bt_parts;
/* Adds one template's block to the (mask, kind) group; past SH_BT_MAX_PARTS groups, overflow latches and the
 * groups are not emitted. */
void sh_bt_parts_add(sh_bt_parts *ps, unsigned mask, int kind, const int block[SH_BT_FIELDS]);
/* Adds one resident module: own block, template block (GAME and misses, from the 50 KB base) and VIEW block,
 * filtered with `mask`, plus the unfiltered groups (parts may be NULL). */
void sh_bt_table_add_module(sh_bt_table *t, const char *decl_name, const int own[SH_BT_FIELDS],
                            const int templ[SH_BT_FIELDS], const int view[SH_BT_FIELDS], unsigned mask,
                            const sh_bt_parts *parts);
typedef struct sh_bt_head {
    unsigned generation;
    const int *limits, *usage;   /* 12 each */
    int reached;
    unsigned auto_view_mask, sound_mask;
    const char *layers_source;   /* "decl" or "fallback" */
} sh_bt_head;
/* Returns the whole table as one malloc-owned JSON object, or NULL when out of memory. */
char *sh_bt_table_finish(sh_bt_table *t, const sh_bt_head *head);
/* Writes {"limits":[..],"usage":[..],"reached":n,"gen":g}. Returns the length, or -1 when cap is too small. */
int sh_bt_usage_json(char *out, int cap, const int limits[SH_BT_FIELDS], const int usage[SH_BT_FIELDS],
                     int reached, unsigned generation);
/* Returns malloc-owned {"error":"..."} for a table that could not be read. */
char *sh_bt_error_json(const char *message, unsigned generation);

#ifndef SH_BUDGET_TABLE_PURE
#include <stdint.h>
#include "signatures.h"
#include "snapmap_plus_iface.h"

/* Binds the decl lookup used for the snapLayers masks. Returns 1 when available; without it the filter is
 * 0xffffffff. */
int sh_budget_table_install(const sig_result *results, size_t count);

/* Tells the table the entity palette changed; it is rebuilt on the next frame. */
void sh_budget_table_palette_rebuilt(void);

/* Called by the EditorFrame detour after the engine's frame, on the game thread. */
void sh_budget_table_tick(void *editor);

/* Finds the palette row that owns an entityDef inherit as of the last palette build, and the model it draws
 * (renderInfo.renderModelName and .size). Any thread. Returns SH_BT_ROW_* bits; 0 when no row owns it or no table
 * has been built. */
#define SH_BT_ROW_FOUND 0x1
#define SH_BT_ROW_MODEL 0x2
#define SH_BT_ROW_SIZE  0x4
int sh_budget_table_row_model(const char *inherit, char *sedef, size_t sedef_cap,
                              char *model, size_t model_cap, float size[3]);

/* The +0x370 slot body (see snapmap_plus_iface.h). */
int sh_budget_table_slot(struct sh_iface *self, sh_budget_call *call);
#endif

#endif
