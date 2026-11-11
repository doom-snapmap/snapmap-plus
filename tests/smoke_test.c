/* smoke_test.c -- the startup resolution report counts what resolved and names what did not. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/smoke.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

static char logged[16][512];
static int logs;
void backend_log(const char *msg)
{
    if (logs < 16) strncpy_s(logged[logs++], sizeof logged[0], msg, _TRUNCATE);
}

int main(void)
{
    size_t n = sig_db_count(), i;
    sig_result *r = (sig_result *)calloc(n, sizeof *r);
    static const uint8_t image[64];
    CHECK(r != NULL && n > 1);
    if (!r) return 1;
    for (i = 0; i < n; i++) {
        r[i].name = BACKEND_ENGINE_SIGNATURES[i].name;
        r[i].status = SIG_OK;
    }
    CHECK(sh_smoke_run(image, r, n, 5) == 1);
    CHECK(logs == 1 && strncmp(logged[0], "startup: resolved ", 18) == 0);
    CHECK(strstr(logged[0], "detour") == NULL);   /* a release build runs no scratch detour */

    logs = 0;
    r[1].status = SIG_NOT_FOUND;
    CHECK(sh_smoke_run(image, r, n, 5) == 0);
    CHECK(logs == 2);
    CHECK(strstr(logged[0], r[1].name) != NULL && strstr(logged[0], "NOT_FOUND") != NULL);
    CHECK(strstr(logged[1], "resolved only") != NULL && strstr(logged[1], r[1].name) != NULL);
    free(r);
    if (failures) { fprintf(stderr, "%d smoke check(s) failed\n", failures); return 1; }
    puts("smoke tests passed");
    return 0;
}
