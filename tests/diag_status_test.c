/* diag_status_test.c -- the diagnostic build's status export: its JSON, the
 * needed-length answer, and that xinput1_4_diag.def is xinput1_4.def plus the
 * one export. Built with /DSH_DIAG. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/diag_status.h"
#include "../src/backend/feature_health.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

void backend_log(const char *msg) { (void)msg; }
const char *sh_product_version(void) { return "v1.0.0"; }
char *sh_rawmap_diag_status(void)
{
    const char *s = "{\"opens\":1,\"generation\":3,\"pending\":0,\"faulted\":0,\"last\":{\"seq\":2,"
                    "\"path\":\"C:\\\\maps\\\\a.json\",\"state\":\"opened\",\"rc\":0,\"reason\":\"\","
                    "\"bytes\":10,\"mods\":0}}";
    return _strdup(s);
}

/* The non-comment lines of a .def file, joined by newlines. */
static char *def_body(const char *path)
{
    FILE *f = NULL;
    char line[512];
    size_t cap = 1 << 16, len = 0;
    char *out = (char *)calloc(cap, 1);
    if (!out || fopen_s(&f, path, "r") != 0 || !f) { free(out); return NULL; }
    while (fgets(line, sizeof line, f)) {
        size_t n = strcspn(line, "\r\n");
        line[n] = '\0';
        if (line[0] == ';' || !line[0]) continue;
        if (len + n + 2 < cap) { memcpy(out + len, line, n); len += n; out[len++] = '\n'; out[len] = '\0'; }
    }
    fclose(f);
    return out;
}

int main(int argc, char **argv)
{
    char out[1024], tiny[16];
    int n, need;
    const char *want_head = "{\"protocol\":1,\"version\":\"v1.0.0\",\"iface_vtbl\":";

    sh_feature_off("Rawmaps", "test");
    sh_feature_off("the \"quoted\" tile", "test");
    n = sh_diag_status(out, (int)sizeof out);
    CHECK(n > 0 && (size_t)n == strlen(out));
    CHECK(strncmp(out, want_head, strlen(want_head)) == 0);
    CHECK(strstr(out, "\"features_off\":[\"Rawmaps\",\"the \\\"quoted\\\" tile\"],\"rawmap\":{\"opens\":1,") != NULL);
    CHECK(out[n - 1] == '}' && out[n - 2] == '}');

    /* Too small: the needed length, and nothing written. */
    tiny[0] = 'x';
    need = sh_diag_status(tiny, (int)sizeof tiny);
    CHECK(need == n && tiny[0] == '\0');
    CHECK(sh_diag_status(NULL, 0) == n);

    /* A script note is counted; nothing listens until a listener says so. */
    CHECK(sh_diag_note_listening == 0 && sh_diag_note_count == 0);
    sh_diag_note("salt", "brain T a b", 11);
    sh_diag_note("salt", "", 0);
    CHECK(sh_diag_note_count == 1);

    /* The diagnostic export source is the release one plus ordinals 300 to 302. */
    if (argc > 1) {
        char rel[MAX_PATH], diag[MAX_PATH];
        char *a, *b;
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s\\xinput1_4.def", argv[1]);
        _snprintf_s(diag, sizeof diag, _TRUNCATE, "%s\\xinput1_4_diag.def", argv[1]);
        a = def_body(rel); b = def_body(diag);
        CHECK(a && b);
        if (a && b) {
            size_t la = strlen(a);
            CHECK(strncmp(a, b, la) == 0);
            CHECK(strcmp(b + la, "    sh_diag_status                  @300\n"
                                 "    sh_diag_note                    @301\n"
                                 "    sh_diag_note_listening          @302 DATA\n") == 0);
        }
        free(a); free(b);
    }
    if (failures) { fprintf(stderr, "%d diag status check(s) failed\n", failures); return 1; }
    puts("diag status tests passed");
    return 0;
}
