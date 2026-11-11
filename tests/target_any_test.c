/* target_any_test.c -- hiding again puts back exactly the visibility the reveal changed. */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "../src/backend/target_any.c"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

void backend_log(const char *msg) { (void)msg; }
static char printed[256];
void sh_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(printed, sizeof printed, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static unsigned char decls[4][0x400];
static void *decl_ptrs[4];
static unsigned char registry[0x30];
static int registry_ok = 1;
static void *fake_get_decls(const char *type)
{
    (void)type;
    return registry_ok ? registry : NULL;
}

int main(void)
{
    int i;
    for (i = 0; i < 4; i++) decl_ptrs[i] = decls[i];
    decls[0][DECL_FLAGS_OFF] = 0xC0 | 0x05;   /* visible already: a palette entry */
    decls[1][DECL_FLAGS_OFF] = 0x00 | 0x10;   /* hidden */
    decls[2][DECL_FLAGS_OFF] = 0x40 | 0x20;   /* half set */
    decls[3][DECL_FLAGS_OFF] = 0xC0;          /* visible */
    *(void **)(registry + LIST_ARRAY_OFF) = decl_ptrs;
    *(unsigned *)(registry + LIST_COUNT_OFF) = 4;

    h_target_any(NULL);
    CHECK(strstr(printed, "not available") != NULL && !sh_target_any_is_shown());

    sh_target_any_install((void *)fake_get_decls);
    h_target_any(NULL);
    CHECK(sh_target_any_is_shown());
    CHECK(strstr(printed, "shown (run again to hide them)") != NULL);
    for (i = 0; i < 4; i++) CHECK((decls[i][DECL_FLAGS_OFF] & VIS_BITS) == VIS_BITS);
    CHECK(g_revealed_count == 2);

    /* A decl the game shows itself while revealed keeps its other bits. */
    decls[1][DECL_FLAGS_OFF] |= 0x01;
    h_target_any(NULL);
    CHECK(!sh_target_any_is_shown());
    CHECK(strstr(printed, "hidden again") != NULL);
    CHECK(decls[0][DECL_FLAGS_OFF] == (0xC0 | 0x05));
    CHECK(decls[1][DECL_FLAGS_OFF] == (0x10 | 0x01));
    CHECK(decls[2][DECL_FLAGS_OFF] == (0x40 | 0x20));
    CHECK(decls[3][DECL_FLAGS_OFF] == 0xC0);

    /* An unreadable registry changes nothing and stays hidden. */
    registry_ok = 0;
    h_target_any(NULL);
    CHECK(!sh_target_any_is_shown() && strstr(printed, "not available") != NULL);

    if (failures) { fprintf(stderr, "%d target_any check(s) failed\n", failures); return 1; }
    puts("target_any tests passed");
    return 0;
}
