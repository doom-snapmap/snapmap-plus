#include "package_editor_rows.h"
#include "decl_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 1 copied, 0 absent or empty, -1 allocation. */
static int er_literal(const sh_decl_node *node, char **out)
{
    const char *text; size_t length;
    *out = NULL;
    if (!node || node->compound || !sh_decl_tree_literal(node, &text, &length) || !length) return 0;
    if (!(*out = malloc(length + 1))) return -1;
    memcpy(*out, text, length); (*out)[length] = 0;
    return 1;
}

/* The last of parent's members named key: the native reader applies a
 * repeated field in order, so the last one is effective. */
static const sh_decl_node *er_last(const sh_decl_node *parent, const char *key)
{
    const sh_decl_node *found = NULL;
    for (const sh_decl_node *m = parent ? parent->children : NULL; m; m = m->next)
        if (m->key && !strcmp(m->key, key)) found = m;
    return found;
}

int sh_editor_rows_add_text(sh_editor_rows *rows, const char *name, const char *text, size_t length)
{
    char error[256];
    sh_editor_row row = {0};
    /* Shipped entries may repeat a field (fx/ambient_embers states fxInfo
     * twice), which the native reader applies in order; only text that does
     * not parse at all is unreadable. */
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){text, length}, error, sizeof(error));
    if (!tree) { rows->unreadable++; return 0; }
    const sh_decl_node *def = NULL;
    int edits = 0;
    for (const sh_decl_node *m = tree->children; m; m = m->next)
        if (m->key && !strcmp(m->key, "edit") && m->compound) {
            const sh_decl_node *stated = er_last(m, "entityDef");
            edits++;
            if (stated) def = stated;
        }
    int ok = (row.name = _strdup(name)) != NULL &&
        er_literal(er_last(tree, "inherit"), &row.parent) >= 0 &&
        er_literal(def, &row.entity_def) >= 0;
    int plain = ok && row.parent && row.entity_def && edits == 1;
    for (const sh_decl_node *m = tree->children; plain && m; m = m->next)
        plain = m->key && (!strcmp(m->key, "inherit") || !strcmp(m->key, "edit"));
    const sh_decl_node *edit = plain ? er_last(tree, "edit") : NULL;
    for (const sh_decl_node *m = edit ? edit->children : NULL; plain && m; m = m->next)
        plain = m->key && !strcmp(m->key, "entityDef");
    row.plain = plain;
    sh_decl_tree_free(tree);
    sh_editor_row *grown = ok ? realloc(rows->items, (rows->count + 1) * sizeof(*grown)) : NULL;
    if (!grown) { free(row.name); free(row.parent); free(row.entity_def); return -1; }
    rows->items = grown; rows->items[rows->count++] = row;
    return 1;
}

static int er_row_order(const void *a, const void *b)
{ return strcmp(((const sh_editor_row *)a)->name, ((const sh_editor_row *)b)->name); }
static int er_name_order(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }
static const sh_editor_row *er_find(const sh_editor_rows *rows, const char *name)
{
    sh_editor_row key = {(char *)name, NULL, NULL, 0};
    return rows && rows->count && name ? bsearch(&key, rows->items, rows->count, sizeof(key), er_row_order) : NULL;
}
/* The entityDef a row resolves to through its parents. */
static const char *er_resolve(const sh_editor_rows *rows, const sh_editor_rows *fallback, const sh_editor_row *row)
{
    for (int depth = 0; row && depth < 64; depth++) {
        if (row->entity_def) return row->entity_def;
        const sh_editor_row *parent = er_find(rows, row->parent);
        row = parent ? parent : er_find(fallback, row->parent);
    }
    return NULL;
}

int sh_editor_rows_finish(sh_editor_rows *rows, const sh_editor_rows *fallback)
{
    if (rows->count) qsort(rows->items, rows->count, sizeof(*rows->items), er_row_order);
    free(rows->placeable); rows->placeable_count = 0;
    rows->placeable = calloc(rows->count ? rows->count : 1, sizeof(*rows->placeable));
    if (!rows->placeable) return 0;
    for (size_t i = 0; i < rows->count; i++) {
        const char *resolved = er_resolve(rows, fallback, rows->items + i);
        if (resolved) rows->placeable[rows->placeable_count++] = resolved;
    }
    if (rows->placeable_count) qsort(rows->placeable, rows->placeable_count, sizeof(*rows->placeable), er_name_order);
    return 1;
}

void sh_editor_rows_free(sh_editor_rows *rows)
{
    if (!rows) return;
    for (size_t i = 0; i < rows->count; i++) {
        free(rows->items[i].name); free(rows->items[i].parent); free(rows->items[i].entity_def);
    }
    free(rows->items); free((void *)rows->placeable); memset(rows, 0, sizeof(*rows));
}

const char *sh_editor_rows_placement(const sh_editor_rows *shipped, const sh_editor_rows *package,
    const char *from, const char *authored, const char *base, sh_decl_carrier_limit *limit,
    char *reason, size_t capacity)
{
    *limit = SH_DECL_CARRIER_GAP;
    if (!bsearch(&base, shipped->placeable, shipped->placeable_count, sizeof(*shipped->placeable), er_name_order)) {
        if (shipped->unreadable) snprintf(reason, capacity,
            "'%s' has no readable SnapMap editor entry (%zu entries could not be read)", base, shipped->unreadable);
        else {
            *limit = SH_DECL_CARRIER_ENGINE;
            snprintf(reason, capacity,
                "'%s' has no SnapMap editor entry, and the native map validator rejects entities that inherit it", base);
        }
        return reason;
    }
    for (size_t i = 0; package && i < package->count; i++) {
        const sh_editor_row *row = package->items + i;
        const char *resolved = er_resolve(package, shipped, row);
        if (!resolved || (strcmp(resolved, from) && (!authored || strcmp(resolved, authored)))) continue;
        /* The converted entity takes the shipped row placing base: the package
         * row must be that row with only its placed definition changed. */
        const sh_editor_row *parent = row->plain && !er_find(package, row->parent) ? er_find(shipped, row->parent) : NULL;
        const char *placed = parent ? er_resolve(shipped, NULL, parent) : NULL;
        if (!placed || strcmp(placed, base)) {
            snprintf(reason, capacity, "its package's editor entry '%s' is not a plain copy of SnapMap's entry for '%s'",
                row->name, base);
            return reason;
        }
    }
    return NULL;
}
