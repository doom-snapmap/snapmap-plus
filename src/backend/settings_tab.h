/* The Snapmap+ tab of the native Settings / Properties panel (editor state 5): a third category where features
 * register groups of rows, each with a map-owned draft that native Apply commits and Cancel / Revert discards. If the
 * category cannot be installed the rows go at the end of category 0 (sh_settings_tab_current_mode()). */
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
    /* Nonzero: a step past one limit lands on the other and adjust sees the jump; zero stops at the limit. */
    int wrap;
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

/* Engine-thread rows. draft and base belong to the group and stay valid while the panel is open; commit copies
 * draft to base and Cancel / Revert copies base to draft. adjust (optional) may rewrite *draft from the widget value. */
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

/* Install byte checks over a Populate call site: 1 when it is an E8 whose target holds the 16-byte dirty prefix
 * (target returned), and 1 when it is an E8 to the given SettingsPanelClear. */
int sh_settings_tab_check_dirty(uintptr_t call, uintptr_t *target);
int sh_settings_tab_check_clear(uintptr_t call, uintptr_t clear);
int sh_settings_tab_install(const sig_result *results, size_t count);
sh_settings_tab_mode sh_settings_tab_current_mode(void);

/* Pure helpers shared with the tests. */
int sh_settings_tab_wrap(int category, int delta, int count);
/* For a wrap-flagged float row: returns the limit the step was heading toward when it jumped across [min, max],
 * else value. */
float sh_settings_tab_unwrap(float previous, float value, float min, float max,
                             float small_step, float large_step);
#endif
