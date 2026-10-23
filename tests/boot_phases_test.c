/* R-400: the first registration's placement and the "MPKG: boot phases" line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/boot_phases.h"

static int failures;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++;                                                             \
    }                                                                           \
} while (0)

static unsigned log_lines;
void backend_log(const char *message) { (void)message; log_lines++; }

static int has(const char *line, const char *needle)
{
    if (!strstr(line, needle)) fprintf(stderr, "expected \"%s\" in:\n  %s\n", needle, line);
    return strstr(line, needle) != NULL;
}

/* User packages on, no after-menu marker: the rows register inside the engine's boot promotion, the earliest
 * point the decl server can take them (after the engine's own declarations, before its whole-registry promotion);
 * with the marker they wait for the first SnapMap browser; with packages off nothing registers. */
static void test_placement(void)
{
    CHECK(sh_boot_placement_decide(1, 0) == SH_BOOT_PLACE_PROMOTION);
    CHECK(sh_boot_placement_decide(1, 1) == SH_BOOT_PLACE_FIRST_BOUNDARY);
    CHECK(sh_boot_placement_decide(0, 0) == SH_BOOT_PLACE_NONE);
    CHECK(sh_boot_placement_decide(0, 1) == SH_BOOT_PLACE_NONE);
    CHECK(strstr(sh_boot_placement_name(SH_BOOT_PLACE_PROMOTION), "promotion") != NULL);
    CHECK(strstr(sh_boot_placement_name(SH_BOOT_PLACE_FIRST_BOUNDARY), "after the menu") != NULL);
}

/* A boot that registers in the promotion: every span is the difference of its two milestones, the line names the
 * library route and the time the promotion held the engine, and it is written once, at the menu. */
static void test_promotion_line(void)
{
    sh_boot_phases_test_reset();
    sh_boot_phases_set_placement(SH_BOOT_PLACE_PROMOTION);
    sh_boot_phases_test_set(SH_BOOT_ATTACH, 1200);
    sh_boot_phases_test_set(SH_BOOT_SIGNATURES, 13200);       /* +12.0 s */
    sh_boot_phases_test_set(SH_BOOT_HOOKS, 13700);            /* +0.5 s */
    sh_boot_phases_test_set(SH_BOOT_PROMOTION, 199700);       /* +186.0 s */
    sh_boot_phases_test_set(SH_BOOT_LIBRARY, 218700);         /* +19.0 s */
    sh_boot_phases_test_set(SH_BOOT_REGISTERED, 230700);      /* +12.0 s */
    sh_boot_phases_test_set(SH_BOOT_PROMOTION_RETURNED, 230900);
    sh_boot_phases_test_set(SH_BOOT_MENU, 233500);
    sh_boot_phases_set_library("warm record");
    sh_boot_phases_set_worker("catalog opened in 900 ms");
    unsigned before = log_lines;
    sh_boot_phases_report_menu();
    sh_boot_phases_report_menu();
    CHECK(log_lines == before + 1);
    const char *line = sh_boot_phases_test_last_line();
    CHECK(has(line, "MPKG: boot phases: attached 1.2 s after process start"));
    CHECK(has(line, "engine code ready 12.0 s later"));
    CHECK(has(line, "hooks 0.5 s"));
    CHECK(has(line, "engine init to the boot promotion 186.0 s"));
    CHECK(has(line, "registration inside the engine's boot promotion: library 19.0 s (warm record), registration 12.0 s, 31.2 s held in the promotion"));
    CHECK(has(line, "promotion return to menu 2.6 s; menu 233.5 s after process start"));
    CHECK(has(line, "boot worker: catalog opened in 900 ms"));
    /* Not a deferred boot: no second line. */
    sh_boot_phases_report_first_boundary(5000);
    CHECK(log_lines == before + 1);
}

/* A boot whose registration waits for the menu: the promotion holds the engine only for itself, and the first
 * SnapMap browser's pass is reported once with its duration and its distance from the menu. */
static void test_after_menu_line(void)
{
    sh_boot_phases_test_reset();
    sh_boot_phases_set_placement(SH_BOOT_PLACE_FIRST_BOUNDARY);
    sh_boot_phases_test_set(SH_BOOT_ATTACH, 1000);
    sh_boot_phases_test_set(SH_BOOT_SIGNATURES, 2000);
    sh_boot_phases_test_set(SH_BOOT_HOOKS, 2500);
    sh_boot_phases_test_set(SH_BOOT_PROMOTION, 150000);
    sh_boot_phases_test_set(SH_BOOT_PROMOTION_RETURNED, 150100);
    sh_boot_phases_test_set(SH_BOOT_MENU, 152600);
    unsigned before = log_lines;
    sh_boot_phases_report_menu();
    const char *line = sh_boot_phases_test_last_line();
    CHECK(has(line, "registration after the menu, at the first SnapMap browser: 0.1 s held in the promotion"));
    CHECK(has(line, "menu 152.6 s after process start"));
    CHECK(!strstr(line, "library"));
    sh_boot_phases_test_set(SH_BOOT_FIRST_BOUNDARY, 182600);
    sh_boot_phases_set_library("compiled");
    sh_boot_phases_report_first_boundary(41000);
    sh_boot_phases_report_first_boundary(41000);
    CHECK(log_lines == before + 2);
    line = sh_boot_phases_test_last_line();
    CHECK(has(line, "the deferred first registration completed at the first SnapMap browser 30.0 s after the menu"));
    CHECK(has(line, "(library compiled); the pass took 41000 ms"));
}

/* An unreached milestone prints as "-", never as a negative or wrapped span. */
static void test_missing_marks(void)
{
    uint64_t at[SH_BOOT_MARK_COUNT] = {0};
    char line[2048];
    at[SH_BOOT_ATTACH] = 900; at[SH_BOOT_PROMOTION] = 5000; at[SH_BOOT_HOOKS] = 6000; /* hooks after the promotion */
    size_t n = sh_boot_phases_format(line, sizeof(line), at, SH_BOOT_PLACE_PROMOTION, NULL, NULL);
    CHECK(n == strlen(line));
    CHECK(has(line, "engine code ready - later"));
    CHECK(has(line, "engine init to the boot promotion -"));
    CHECK(has(line, "library - (not prepared)"));
    CHECK(has(line, "menu - after process start"));
    CHECK(!strstr(line, "boot worker"));
    char small[40];
    n = sh_boot_phases_format(small, sizeof(small), at, SH_BOOT_PLACE_PROMOTION, NULL, NULL);
    CHECK(n < sizeof(small) && strlen(small) == n);
}

int main(void)
{
    test_placement();
    test_promotion_line();
    test_after_menu_line();
    test_missing_marks();
    if (failures) { fprintf(stderr, "boot_phases_test: %d failure(s)\n", failures); return 1; }
    printf("boot_phases_test: ok\n");
    return 0;
}
