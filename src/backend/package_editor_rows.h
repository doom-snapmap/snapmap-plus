/* SnapMap editor rows and native placement (R137).
 *
 * SnapMap's map validator admits an entity only when its inherit names the
 * entityDef that a palette row resolves to. The native builder derives those
 * rows from live snapEditorEntityDef declarations, resolving entityDef through
 * each declaration's parents. A native map entity therefore needs a row among
 * SnapMap's shipped editor declarations; a row a package adds for its own
 * definition carries editor presentation and inputs/outputs of its own. */
#ifndef SH_PACKAGE_EDITOR_ROWS_H
#define SH_PACKAGE_EDITOR_ROWS_H

#include <stddef.h>
#include "decl_native_carrier.h"

typedef struct sh_editor_row {
    char *name, *parent, *entity_def;
    int plain; /* only an inherit and edit = { entityDef } */
    /* Its own toybox presentation keys (displayNameTag, categoryNameTag,
     * metaCategoryNameTag), NULL when not stated; see sh_editor_rows_effective.
     * A stated empty literal is "" here and in parent/entity_def: the row's
     * own value, never a fall-through to its parent. */
    char *label, *category, *meta;
} sh_editor_row;
typedef struct sh_editor_rows {
    sh_editor_row *items;
    size_t count, unreadable;
    const char **placeable; /* sorted resolved entityDef names */
    size_t placeable_count;
} sh_editor_rows;

/* Parse one declaration. 1 added, 0 unparsable (counted unreadable), -1 on
 * allocation failure. Call finish once every row is added. */
int sh_editor_rows_add_text(sh_editor_rows *rows, const char *name, const char *text, size_t length);
/* Sort and resolve; parents absent from rows resolve in fallback (may be NULL). */
int sh_editor_rows_finish(sh_editor_rows *rows, const sh_editor_rows *fallback);
void sh_editor_rows_free(sh_editor_rows *rows);
/* A row's value of one of its fields through its parents (absent parents
 * resolve in fallback, which may be NULL), or NULL. The nearest row stating
 * the field decides, even when it states "". */
enum { SH_EDITOR_ROW_ENTITY_DEF, SH_EDITOR_ROW_LABEL, SH_EDITOR_ROW_CATEGORY, SH_EDITOR_ROW_META };
const char *sh_editor_rows_effective(const sh_editor_rows *rows, const sh_editor_rows *fallback,
    const sh_editor_row *row, int field);
const sh_editor_row *sh_editor_rows_find(const sh_editor_rows *rows, const char *name);

/* NULL when base is placed by a shipped row and every package row placing
 * from (or its authored spelling, may be NULL) only copies a shipped row that
 * places base. Otherwise the reason, written to reason, with *limit: ENGINE
 * when no shipped row places base (the validator would reject the entity),
 * GAP when shipped rows were unreadable or a package row adds more. */
const char *sh_editor_rows_placement(const sh_editor_rows *shipped, const sh_editor_rows *package,
    const char *from, const char *authored, const char *base, sh_decl_carrier_limit *limit,
    char *reason, size_t capacity);

#endif
