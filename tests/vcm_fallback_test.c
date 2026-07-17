/* vcm_fallback_test.c -- the class/inherit dropdown corpus-snapshot fallback (vcm_fallback.c).
 * This is the list the dropdowns serve when the live engine type registry is unreachable on the
 * running build; it regressed once to an empty dropdown ("no matches") because nothing exercised
 * it off the game. Self-contained: no game, no built DLL. */
#include <stdio.h>
#include <string.h>

#include "vcm_fallback.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("[ok]   %s\n", msg); } \
    else      { printf("[FAIL] %s\n", msg); g_fail++; } \
} while (0)

/* walk a packed (double-NUL-terminated) string list: count entries + find `needle`. */
static int packed_count(const char *buf, int cap, const char *needle, int *found)
{
    int n = 0; const char *p = buf; const char *end = buf + cap;
    if (found) *found = 0;
    while (p < end && *p) {
        if (found && strcmp(p, needle) == 0) *found = 1;
        p += strlen(p) + 1;
        n++;
    }
    return n;
}

int main(void)
{
    static char buf[262144];

    /* ---- the inherit dropdown fallback ---- */
    int count = -1;
    int r = sh_vcm_fallback_inherits(buf, (int)sizeof buf, &count);
    CHECK(r == 1, "inherits: returns 1");
    CHECK(count >= 200, "inherits: a real list (>=200 snapshot entries)");
    int found = 0;
    int walked = packed_count(buf, (int)sizeof buf, "ai/default", &found);
    CHECK(walked == count, "inherits: packed-entry walk matches the reported count");
    CHECK(found, "inherits: contains ai/default");

    /* ---- the class dropdown fallback (inherit -> Y -> classes) ---- */
    int written = 0, names = 0;
    buf[0] = '\0';
    r = sh_vcm_fallback_valid_classes("ai/default", buf, (int)sizeof buf, &written, &names);
    buf[written] = '\0';
    CHECK(r == 1, "classes(ai/default): inherit is in the snapshot");
    CHECK(names > 0, "classes(ai/default): >=1 valid class");
    packed_count(buf, (int)sizeof buf, "idAI2", &found);
    CHECK(found, "classes(ai/default): contains idAI2 (its base class)");

    /* unknown inherit -> a clean miss, nothing packed */
    written = 0; names = 0;
    buf[0] = '\0';
    r = sh_vcm_fallback_valid_classes("not/a/real/inherit", buf, (int)sizeof buf, &written, &names);
    CHECK(r == 0 && names == 0 && written == 0, "classes(unknown): clean 0, nothing packed");

    /* ---- tiny-cap safety: truncation never overruns, list stays walkable ---- */
    char tiny[48];
    memset(tiny, 0x7f, sizeof tiny);
    count = -1;
    r = sh_vcm_fallback_inherits(tiny, (int)sizeof tiny, &count);
    CHECK(r == 1 && count >= 1, "inherits(tiny cap): still packs what fits");
    walked = packed_count(tiny, (int)sizeof tiny, "", NULL);
    CHECK(walked == count, "inherits(tiny cap): truncated list is cleanly terminated");

    if (g_fail) { printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    printf("\nall vcm_fallback checks passed\n");
    return 0;
}
