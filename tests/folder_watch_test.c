/* folder_watch_test -- the prefabs folder watcher's debounce (src/ui/webview/folder_watch.h) on a
 * fake clock: a burst of changes is one refresh after the quiet time, and a change during the
 * wait pushes the refresh out again. */
#include <stdio.h>
#include "folder_watch.h"

static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

int main(void)
{
    sh_fw_debounce p;

    printf("-- prefabs: changes within 300 ms coalesce into one refresh\n");
    sh_fw_init(&p, SH_FW_PREFABS_QUIET_MS);
    CHECK(!sh_fw_due(&p, 0) && !sh_fw_due(&p, 100000));            /* nothing changed */
    sh_fw_note(&p, 1000);
    sh_fw_note(&p, 1100);
    sh_fw_note(&p, 1250);
    CHECK(!sh_fw_due(&p, 1300) && !sh_fw_due(&p, 1549));            /* 300 ms after the last */
    CHECK(sh_fw_due(&p, 1550));
    CHECK(sh_fw_done(&p) == 3 && !sh_fw_due(&p, 5000));             /* once */
    sh_fw_note(&p, 6000);
    CHECK(sh_fw_due(&p, 6300) && sh_fw_done(&p) == 1);

    printf("-- a change during the wait pushes the refresh out again\n");
    sh_fw_note(&p, 10000);
    sh_fw_note(&p, 10250);
    CHECK(!sh_fw_due(&p, 10300) && !sh_fw_due(&p, 10549) && sh_fw_due(&p, 10550));
    CHECK(sh_fw_done(&p) == 2 && !sh_fw_due(&p, 20000));

    printf("folder_watch_test: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
