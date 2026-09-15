#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decl_material_compose.h"
static int failures, binding_mode;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
static int same(sh_decl_source name, const char *text)
{ return name.length == strlen(text) && !memcmp(name.text, text, name.length); }
static int resolve(void *context, sh_decl_composition_role role, size_t index,
    const char *family, sh_decl_source name, int *kind, sh_decl_source *binding)
{
    const char *schema;
    (void)context;
    *kind = -1; *binding = (sh_decl_source){0};
    if (strcmp(family, "renderparm")) return 1;
    if (same(name, "a") || same(name, "b") || same(name, "c") || same(name, "color") || same(name, "time")) {
        *kind = 0; schema = "vector:default=0";
    } else if (same(name, "tex")) { *kind = 1; schema = "image"; }
    else if (same(name, "label")) { *kind = 12; schema = "string"; }
    else return 0;
    if (same(name, "color") && binding_mode && (role == SH_DECL_COMPOSITION_RESULT ||
        (binding_mode == 2 && role == SH_DECL_COMPOSITION_CONTRIBUTION && index == 0))) schema = "vector:default=2";
    *binding = (sh_decl_source){schema, strlen(schema)}; return 1;
}
static char *compose(const char *base, const char *a, const char *b, int success)
{
    sh_decl_source sources[] = {{a, strlen(a)}, {b, strlen(b)}};
    sh_decl_material_composition_schema schema = {NULL, resolve};
    sh_decl_conflict conflict; char error[512]; size_t length = 999;
    char *result = sh_decl_material_compose((sh_decl_source){base, strlen(base)}, sources, 2,
        &schema, &length, error, sizeof(error), &conflict);
    if (success && !result) fprintf(stderr, "unexpected refusal: %s\n", error);
    CHECK((result != NULL) == success);
    if (result) {
        CHECK(length == strlen(result)); CHECK(!strstr(result, "schema"));
        CHECK(!strstr(result, "reads")); CHECK(!strstr(result, "item["));
    } else CHECK(error[0] && !length);
    return result;
}
int main(void)
{
    const char *base = "{ color {1,2,3,4} a 0 b time }";
    const char *a = "{ color {5,2,3,4} a 0 b time }";
    const char *b = "{ color {1,6,3,4} a 7 b time }";
    char *out = compose(base, a, b, 1), *reverse = compose(base, b, a, 1);
    if (out && reverse) {
        CHECK(!strcmp(out, reverse)); CHECK(strstr(out, "color { 5, 6, 3, 4 }"));
        CHECK(strstr(out, "a { 7, 7, 7, 7 }")); CHECK(strstr(out, "b time\n"));
    }
    free(out); free(reverse);
    out = compose("{}", "{ a 1 }", "{ b 2 }", 1);
    if (out) CHECK(strstr(out, "a ") && strstr(out, "b ")); free(out);
    out = compose(base, a, "{ color {9,2,3,4} a 0 b time }", 0); free(out);
    out = compose("{ a 1 b 2 }", "{ a 1 }", "{ a 1 b 3 }", 0); free(out);
    out = compose("{ a 1 b 2 }", "{ b 2 }", "{ a 1 b 3 }", 1);
    if (out) CHECK(!strstr(out, "a ") && strstr(out, "b { 3, 3, 3, 3 }")); free(out);
    out = compose("{ a time b a }", "{ a (time + 2) b a }", "{ a time b a.xy }", 1);
    if (out) CHECK(strstr(out, "a (time + 2)") && strstr(out, "b a.xy")); free(out);
    /* Neither author put a write to a before b's expression. An arbitrary
     * identity tie-break must not silently change b's default-value binding. */
    out = compose("{}", "{ a 1 }", "{ b a }", 1);
    reverse = compose("{}", "{ b a }", "{ a 1 }", 1);
    if (out && reverse) {
        const char *read = strstr(out, "b a\n"), *write = strstr(out, "a { 1, 1, 1, 1 }");
        CHECK(!strcmp(out, reverse)); CHECK(read && write && read < write);
    }
    free(out); free(reverse);
    /* Both new expressions read defaults: each must precede the other write,
     * which is impossible. Longer dependency cycles also remain conflicts. */
    out = compose("{}", "{ a b }", "{ b a }", 0); free(out);
    out = compose("{}", "{ a b }", "{ b c c a }", 0); free(out);
    /* A newly appended repeated writer must remain after a peer's default
     * read. Whole prefixes include every occurrence, including masked writes. */
    out = compose("{ a 1 b a }", "{ a 1 b a a 2 }", "{ a 1 b a.xy }", 1);
    if (out) {
        const char *read = strstr(out, "b a.xy\n"), *write = strstr(out, "a { 2, 2, 2, 2 }");
        CHECK(read && write && read < write);
    }
    free(out);
    out = compose("{ b a }", "{ a 1 b a }", "{ b a c 2 }", 1);
    if (out) {
        const char *left = strstr(out, "a { 1, 1, 1, 1 }"), *right = strstr(out, "b a");
        CHECK(left && right && left < right);
    }
    free(out);
    out = compose("{ a 1 a 2 }", "{ a 3 a 2 }", "{ a 1 a 4 }", 1);
    if (out) CHECK(strstr(out, "a { 3, 3, 3, 3 }") && strstr(out, "a { 4, 4, 4, 4 }")); free(out);
    out = compose("{ a 1 a 2 }", "{ a 1 a 2 a 3 }", "{ a 4 a 2 }", 1); free(out);
    out = compose("{ a 1 a 2 }", "{ a 9 a 1 a 2 }", "{ a 1 a 4 }", 0); free(out);
    out = compose("{ a 1 a 2 b a }", "{ a 1 a 2 a 3 b a }", "{ a 1 a 2 b a.xy }", 0); free(out);
    out = compose("{ color.x 1 a 0 }", "{ color.x 2 a 0 }", "{ color.x 1 a 3 }", 1);
    if (out) CHECK(strstr(out, "color.x 2\n") && strstr(out, "a { 3, 3, 3, 3 }")); free(out);
    out = compose("{\n label \"path\\\"\n tex base/image\n a 0\n}",
        "{\n label \"new\\\"\n tex base/image\n a 0\n}",
        "{\n label \"path\\\"\n tex clamp mod/image\n a 1\n}", 1);
    if (out) CHECK(strstr(out, "label \"new\\\"\n") && strstr(out, "tex clamp mod/image\n")); free(out);
    binding_mode = 1;
    out = compose(base, a, b, 0); free(out);
    binding_mode = 2;
    out = compose(base, a, b, 0); free(out);
    out = compose(base, a, "{ color {1,2,3,4} a 7 b time }", 1); free(out);
    binding_mode = 0;
    out = compose("{ a 0 b 0 }", "{ a dot3(time, {1,2,3}) b 0 }", "{ a 0 b 2 }", 1);
    if (out) CHECK(strstr(out, "dot3(time, {1,2,3})")); free(out);
    printf("material composition: %s\n", failures ? "FAIL" : "PASS"); return failures ? 1 : 0;
}
