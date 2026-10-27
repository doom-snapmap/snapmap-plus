/* The Snapmap+ tab of the native Settings / Properties panel (editor state 5).
 *
 * Native Populate(panel, category) knows two categories (0 and 1); LB/RB and
 * the diamond notches select them modulo 2. This module owns the panel's hooks
 * (Enter, Exit, Populate, Apply, Reset, the dirty query and the property-change
 * slot), adds a third category for Snapmap+ mod settings and lets features
 * register groups of rows into it. Each group keeps a map-owned draft that
 * native Apply commits and native Cancel / Revert discards.
 *
 * The tab shows one header title (SH_SETTINGS_TAB_TITLE) and then the rows of
 * every active group; the panel has no label-only row, so groups are not
 * titled on screen and row labels must name what they set.
 *
 * When the third category cannot be installed, the same rows appear at the
 * end of category 0. sh_settings_tab_current_mode() reports which. */
#ifndef SH_SETTINGS_TAB_H
#define SH_SETTINGS_TAB_H
#include <stddef.h>
#include "signatures.h"

#define SH_SETTINGS_TAB_TITLE     "SNAPMAP+ MOD SETTINGS"
#define SH_SETTINGS_TAB_MAX_ROWS  32
#define SH_SETTINGS_TAB_MAX_GROUPS 8

typedef enum sh_settings_tab_mode {
    SH_SETTINGS_TAB_OFF = 0,     /* panel hooks unavailable */
    SH_SETTINGS_TAB_GROUP_END,   /* groups appended to category 0 */
    SH_SETTINGS_TAB_GROUP_TOP,   /* unused: rows ahead of the native title overlap it */
    SH_SETTINGS_TAB_CATEGORY     /* groups in their own third category */
} sh_settings_tab_mode;

typedef struct sh_settings_rows sh_settings_rows;   /* valid only during populate */

typedef struct sh_settings_float {
    const char *name;        /* native row name (shown as the row text) */
    const char *label;       /* "#str_..." label id */
    const char *help;        /* "#str_..." help id */
    float min, max, small_step, large_step;
} sh_settings_float;

typedef struct sh_settings_bool {
    const char *name;        /* native row name (shown as the row text) */
    const char *label;       /* "#str_..." label id */
    const char *help;        /* "#str_..." help id */
} sh_settings_bool;

/* Populate callback: add this group's rows in display order. */
typedef void (*sh_settings_populate_fn)(sh_settings_rows *rows, void *user);

typedef struct sh_settings_group {
    const char *title;       /* group name for logs; not drawn (optional) */
    /* Settings opened for this editable map: load the draft and base values.
     * Return 0 to hide the group for this session. Optional. */
    int  (*begin)(void *user, void *editor, void *map);
    sh_settings_populate_fn populate;
    /* Native Apply with a changed draft: store it in the map. Return 1 on
     * success; the tab then copies each draft row to its base. Optional. */
    int  (*commit)(void *user, void *map);
    /* Settings closed. Optional. */
    void (*end)(void *user);
    void *user;
} sh_settings_group;

/* Engine-thread rows. draft and base belong to the group and must stay valid
 * while the panel is open; after a successful commit the tab copies draft to
 * base, and Cancel / Revert copies base to draft. adjust (optional) receives the native widget value
 * and may rewrite *draft (clamping, snapping, dependent fields). */
int sh_settings_tab_add_float(sh_settings_rows *rows, const sh_settings_float *row,
                              float *draft, float *base,
                              void (*adjust)(void *user, float *draft, float value));

/* A native on/off row (the toggle rows of the editor settings). draft and
 * base hold 0 or 1 and follow the same draft, commit and revert rules as the
 * float rows. Returns 0 when the toggle row creator is unavailable. */
int sh_settings_tab_add_bool(sh_settings_rows *rows, const sh_settings_bool *row,
                             int *draft, int *base);

/* Register before the panel is first opened; groups keep registration order.
 * The descriptor is copied. Returns 0 when full or invalid. */
int sh_settings_tab_register_group(const sh_settings_group *group);
/* Shorthand for a group with only a populate callback: rows are drafted and
 * shown but nothing is committed unless the rows' owner does so itself. */
int sh_settings_tab_register(const char *title, sh_settings_populate_fn populate);

int sh_settings_tab_install(const sig_result *results, size_t count);
sh_settings_tab_mode sh_settings_tab_current_mode(void);

/* Pure helpers shared with the tests. */
int sh_settings_tab_wrap(int category, int delta, int count);
/* A native float row created with its wrap flag set wraps past its limits.
 * Given the value shown before a step and the widget's new value, returns the
 * limit the user was stepping toward when the step jumped from one end of
 * [min, max] to the other, else value unchanged. The tab now creates its rows
 * with the wrap flag clear (the native step clamps), so this is only a
 * documented helper for rows created by other code. */
float sh_settings_tab_unwrap(float previous, float value, float min, float max,
                             float small_step, float large_step);
#endif
