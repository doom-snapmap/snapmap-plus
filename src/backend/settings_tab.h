/* The Snapmap+ tab of the native Settings / Properties panel (editor state 5): a third category where features
 * register groups of rows, each with a map-owned draft that native Apply commits and Cancel / Revert discards. If the
 * category cannot be installed the rows go at the end of category 0. */
#ifndef SH_SETTINGS_TAB_H
#define SH_SETTINGS_TAB_H
#include <stddef.h>
#include "signatures.h"

#define SH_SETTINGS_TAB_TITLE     "SNAPMAP+ MOD SETTINGS"


typedef enum sh_settings_tab_mode {
    SH_SETTINGS_TAB_OFF = 0,     /* panel hooks unavailable */
    SH_SETTINGS_TAB_GROUP_END,   /* groups appended to category 0 */
    SH_SETTINGS_TAB_CATEGORY     /* groups in their own third category */
} sh_settings_tab_mode;

typedef struct sh_settings_rows sh_settings_rows;   /* valid only during populate */

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

/* Groups own their drafts and base values for the lifetime of the native toggle rows. */
int sh_settings_tab_add_bool(sh_settings_rows *rows, const sh_settings_bool *row,
                             int *draft, int *base);

/* Register before the panel is first opened; groups keep registration order.
 * The descriptor is copied. Returns 0 when full or invalid. */
int sh_settings_tab_register_group(const sh_settings_group *group);
/* Install byte checks over a Populate call site: 1 when it is an E8 whose target holds the 16-byte dirty prefix
 * (target returned), and 1 when it is an E8 to the given SettingsPanelClear. */
int sh_settings_tab_check_dirty(uintptr_t call, uintptr_t *target);
int sh_settings_tab_check_clear(uintptr_t call, uintptr_t clear);
int sh_settings_tab_install(const sig_result *results, size_t count);

/* Pure helpers shared with the tests. */
int sh_settings_tab_wrap(int category, int delta, int count);
#endif
