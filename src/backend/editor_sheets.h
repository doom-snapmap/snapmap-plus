#ifndef SH_EDITOR_SHEETS_H
#define SH_EDITOR_SHEETS_H
#include "decl_tree.h"
#include <stdint.h>

typedef struct sh_editor_sheet_source {
    const char *salt;
    const char *json;  /* sheet definitions for this tile */
    const char *settings; /* this mod's setting declarations */
} sh_editor_sheet_source;

/* Sources run in precedence order. Returns 1 changed, 0 unchanged, -1 on error. */
int sh_editor_sheets_apply(sh_dt *tree, const sh_editor_sheet_source *sources, size_t count);
int sh_editor_sheets_inherit(sh_dt *tree, const sh_dt *parent);
/* Snapshots the local preferences as JSON when a conditional sheet is evaluated. */
void sh_editor_sheets_set_preferences(char *(*read)(void));
uint64_t sh_editor_sheets_preferences_key(const sh_editor_sheet_source *sources, size_t count);
/* Exact page and binding IDs from the effective native tile before applying any personal layout. */
char *sh_editor_sheet_ids(sh_dt *tree);
#endif
