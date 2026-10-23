/* Boot milestones from process creation to the engine's menu, and where the
 * first package registration is placed (R-400).
 *
 * The engine draws its boot UI (Boot_Sequence.bik and the percentage counter
 * video) on its video/render thread while idCommonLocal::Init runs on the main
 * thread; the counter follows the "common" resource block load and then waits,
 * at its last value, for the rest of Init. The whole-registry promotion that
 * the decl server detours is inside that wait, so every millisecond the
 * product spends there is a millisecond the counter sits at 99 percent.
 * "MPKG: boot phases" names each stage so a boot can be compared with a
 * vanilla boot of the same machine.
 */
#ifndef SH_BOOT_PHASES_H
#define SH_BOOT_PHASES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sh_boot_mark {
    SH_BOOT_ATTACH = 0,          /* DllMain attach (the backend is loaded) */
    SH_BOOT_SIGNATURES,          /* engine code decrypted and signatures resolved */
    SH_BOOT_HOOKS,               /* startup hooks installed */
    SH_BOOT_PROMOTION,           /* the engine reached the whole-registry promotion (end of decl loading) */
    SH_BOOT_LIBRARY,             /* the library is prepared (warm record or compile) */
    SH_BOOT_REGISTERED,          /* the boot registration returned */
    SH_BOOT_PROMOTION_RETURNED,  /* the engine's promotion returned; Init continues to the shell map */
    SH_BOOT_MENU,                /* the shell's first dialog surface: the menu is up */
    SH_BOOT_FIRST_BOUNDARY,      /* a deferred first registration completed at its boundary */
    SH_BOOT_MARK_COUNT
} sh_boot_mark;

/* Where the first registration of the library's rows runs. */
typedef enum sh_boot_placement {
    SH_BOOT_PLACE_PROMOTION = 0,      /* inside the engine's boot promotion: static lifetime, before the menu */
    SH_BOOT_PLACE_FIRST_BOUNDARY,     /* after the menu, at the first settled SnapMap browser (runtime pass) */
    SH_BOOT_PLACE_NONE                /* user packages are off for this launch: nothing registers */
} sh_boot_placement;

/* Decide the placement. register_after_menu is the launch policy
 * (packages.register_after_menu); packages_enabled is the launch's user
 * package switch. Pure; the same inputs always place alike. */
sh_boot_placement sh_boot_placement_decide(int packages_enabled, int register_after_menu);
const char *sh_boot_placement_name(sh_boot_placement placement);

/* Record a milestone once (later calls keep the first time). Any thread. */
void sh_boot_phases_mark(sh_boot_mark mark);
/* The placement this boot took and, after the library is prepared, how
 * (e.g. "warm record", "compile", "failed"). Copies the text. */
void sh_boot_phases_set_placement(sh_boot_placement placement);
sh_boot_placement sh_boot_phases_placement(void);
void sh_boot_phases_set_library(const char *route);
/* The boot worker's own summary (catalog, record prefetch), one clause. */
void sh_boot_phases_set_worker(const char *summary);
/* Log "MPKG: boot phases" once, at the menu. A deferred placement logs a
 * second line when its first registration completes. */
void sh_boot_phases_report_menu(void);
void sh_boot_phases_report_first_boundary(unsigned long long registration_ms);

/* The line for given milestone times (milliseconds after process creation, 0
 * when not reached) without logging it. Returns the length written. */
size_t sh_boot_phases_format(char *out, size_t capacity, const uint64_t at[SH_BOOT_MARK_COUNT],
    sh_boot_placement placement, const char *library, const char *worker);

#ifdef SH_BOOT_PHASES_TESTING
void sh_boot_phases_test_reset(void);
void sh_boot_phases_test_set(sh_boot_mark mark, uint64_t at_ms);
const char *sh_boot_phases_test_last_line(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
