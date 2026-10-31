/* folder_watch_test -- the folder watchers' debounce (src/ui/webview/folder_watch.h) on a fake
 * clock: a burst of changes is one refresh after the quiet time, the mods folder waits longer,
 * a deferred refresh is asked again later, and changes the product's own store writes cause
 * (the store lock held, or the grace after a hold ended) are dropped. */
#include <stdio.h>
#include "folder_watch.h"

static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

int main(void)
{
    sh_fw_debounce p, m;
    unsigned long long t;

    printf("-- prefabs: changes within 300 ms coalesce into one refresh\n");
    sh_fw_init(&p, SH_FW_PREFABS_QUIET_MS);
    CHECK(!sh_fw_due(&p, 0) && !sh_fw_due(&p, 100000));            /* nothing changed */
    CHECK(sh_fw_note(&p, 1000, 0) && sh_fw_note(&p, 1100, 0) && sh_fw_note(&p, 1250, 0));
    CHECK(!sh_fw_due(&p, 1300) && !sh_fw_due(&p, 1549));            /* 300 ms after the last */
    CHECK(sh_fw_due(&p, 1550));
    CHECK(sh_fw_done(&p) == 3 && !sh_fw_due(&p, 5000));               /* once */
    CHECK(sh_fw_note(&p, 6000, 0) && sh_fw_due(&p, 6300) && sh_fw_done(&p) == 1);

    printf("-- mods: 1500 ms of quiet; a deferred rescan is asked again later\n");
    sh_fw_init(&m, SH_FW_MODS_QUIET_MS);
    sh_fw_store(&m, 0, 0, 7);
    CHECK(sh_fw_note(&m, 10000, 0));
    CHECK(!sh_fw_due(&m, 11000) && !sh_fw_due(&m, 11499) && sh_fw_due(&m, 11500));
    sh_fw_retry(&m, 11500, SH_FW_RETRY_MS);                            /* Play runs: deferred */
    CHECK(!sh_fw_due(&m, 12000) && !sh_fw_due(&m, 13499) && sh_fw_due(&m, 13500));
    CHECK(sh_fw_done(&m) == 1 && !sh_fw_due(&m, 20000));
    /* A change during the wait pushes the rescan out again. */
    CHECK(sh_fw_note(&m, 30000, 0) && sh_fw_note(&m, 31400, 0) && !sh_fw_due(&m, 32000) && sh_fw_due(&m, 32900));
    (void)sh_fw_done(&m);

    printf("-- the product's own store writes do not trigger a rescan mid-write\n");
    /* An install holds the store lock: its changes are dropped, and so are the trailing ones
     * within the grace after the hold ends. */
    t = 40000;
    sh_fw_store(&m, t, 1, 7);
    CHECK(!sh_fw_note(&m, t + 10, 1) && !sh_fw_note(&m, t + 500, 0));
    sh_fw_store(&m, t + 900, 0, 8);                                    /* the hold ended */
    CHECK(!sh_fw_note(&m, t + 1200, 0));                               /* its trailing notification */
    CHECK(!sh_fw_due(&m, t + 10000) && m.dropped == 3);
    /* After the grace a change is the player's: re-armed. */
    sh_fw_store(&m, t + 2000, 0, 8);
    CHECK(sh_fw_note(&m, t + 2000, 0) && sh_fw_due(&m, t + 3500));
    (void)sh_fw_done(&m);
    /* The generation is stable afterwards: later changes are kept however long ago the hold was. */
    sh_fw_store(&m, t + 60000, 0, 8);
    CHECK(sh_fw_note(&m, t + 60000, 0));

    printf("folder_watch_test: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
