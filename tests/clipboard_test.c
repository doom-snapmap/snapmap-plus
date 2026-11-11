/* clipboard_test.c -- text cut to fit is cut at a character boundary, never inside one. */
#include <stdio.h>
#include <string.h>

#include "../src/backend/clipboard.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

int main(void)
{
    /* "a" + U+00E9 (2 bytes) + U+20AC (3 bytes) + "b" */
    static const char text[] = "a\xC3\xA9\xE2\x82\xAC" "b";
    const int len = (int)sizeof text - 1;
    CHECK(len == 7);
    CHECK(sh_clipboard_utf8_fit(text, len, 10) == 7);
    CHECK(sh_clipboard_utf8_fit(text, len, 7) == 7);
    CHECK(sh_clipboard_utf8_fit(text, len, 6) == 6);   /* "a" e-acute euro */
    CHECK(sh_clipboard_utf8_fit(text, len, 5) == 3);   /* inside the euro sign: back to its start */
    CHECK(sh_clipboard_utf8_fit(text, len, 4) == 3);
    CHECK(sh_clipboard_utf8_fit(text, len, 2) == 1);   /* inside e-acute */
    CHECK(sh_clipboard_utf8_fit(text, len, 1) == 1);
    CHECK(sh_clipboard_utf8_fit(text, len, 0) == 0);
    if (failures) { fprintf(stderr, "%d clipboard check(s) failed\n", failures); return 1; }
    puts("clipboard tests passed");
    return 0;
}
