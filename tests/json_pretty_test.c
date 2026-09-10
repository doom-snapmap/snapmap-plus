/* Tests JSON layout changes preserve token order and refuse unsupported
 * input so the caller can retain the original map bytes. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/json_pretty.h"

/* Measure, allocate and format; return NULL on refusal. Add a terminator
 * for assertions, although the production writer uses an explicit length. */
static char *pretty(const char *src)
{
    size_t len  = strlen(src);
    size_t need = json_pretty(src, len, NULL, 0);
    char  *out;
    if (need == 0) return NULL;
    out = (char *)malloc(need + 1);
    assert(out != NULL);
    assert(json_pretty(src, len, out, need) == need);   /* the measuring pass must agree with the writing one */
    out[need] = '\0';
    return out;
}

/* Everything that is not layout: the token stream, with whitespace outside strings removed. Two
 * documents with the same skeleton are the same JSON however they are spaced. */
static void skeleton(const char *s, char *out)
{
    int in_string = 0;
    size_t o = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (in_string) {
            out[o++] = c;
            if (c == '\\') { out[o++] = s[++i]; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; out[o++] = c; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out[o++] = c;
    }
    out[o] = '\0';
}

static void test_layout(void)
{
    char *p;

    p = pretty("{\"a\":1}");
    assert(strcmp(p, "{\n  \"a\": 1\n}\n") == 0);
    free(p);

    /* nesting indents by two per level, and a separator stays with the value it follows */
    p = pretty("{\"a\":{\"b\":2,\"c\":3}}");
    assert(strcmp(p, "{\n  \"a\": {\n    \"b\": 2,\n    \"c\": 3\n  }\n}\n") == 0);
    free(p);

    p = pretty("{\"a\":[1,2]}");
    assert(strcmp(p, "{\n  \"a\": [\n    1,\n    2\n  ]\n}\n") == 0);
    free(p);

    /* empty containers stay on one line -- map JSON is full of them and a lone bracket on its own line
     * reads as a mistake */
    p = pretty("{\"a\":{},\"b\":[],\"c\":1}");
    assert(strcmp(p, "{\n  \"a\": {},\n  \"b\": [],\n  \"c\": 1\n}\n") == 0);
    free(p);

    /* already-laid-out input is re-laid-out, not doubled up (a rawmap edited by hand and saved again) */
    p = pretty("{\n\t\"a\" : 1\n}\n");
    assert(strcmp(p, "{\n  \"a\": 1\n}\n") == 0);
    free(p);
}

static void test_document_is_unchanged(void)
{
    /* the shape of what the engine actually hands us: no spaces, deep nesting, escaped strings */
    static const char src[] =
        "{\"version\":5,\"name\":\"my map\",\"entities\":[{\"class\":\"idSnapEntity\","
        "\"state\":{\"edit\":{\"targets\":{\"item[0]\":\"a/b\",\"num\":1},\"flags\":[true,false,null],"
        "\"scale\":1.0,\"note\":\"quote \\\" brace } comma , colon : bracket ]\"}}}],\"meta\":{}}";
    char before[sizeof src], after[4096];
    char *p = pretty(src);
    assert(p != NULL);

    skeleton(src, before);
    skeleton(p, after);
    assert(strcmp(before, after) == 0);   /* same tokens, same order -- only the spacing moved */

    /* structural characters INSIDE a string are data, so the layout must not have broken on them */
    assert(strstr(p, "\"quote \\\" brace } comma , colon : bracket ]\"") != NULL);
    free(p);
}

static void test_refusals(void)
{
    /* Refusing means the shadow writes the engine's bytes unchanged. A hard-to-read rawmap still loads;
     * a mangled one does not, so every doubtful case has to land here. */
    assert(json_pretty("{\"a\":\"unterminated", 18, NULL, 0) == 0);
    assert(json_pretty("{\"a\":1", 6, NULL, 0) == 0);          /* truncated -- never closed */
    assert(json_pretty("{\"a\":1}}", 8, NULL, 0) == 0);        /* one close too many */
    assert(json_pretty("   \t\n ", 6, NULL, 0) == 0);          /* nothing but whitespace */
    assert(json_pretty("", 0, NULL, 0) == 0);
    assert(json_pretty(NULL, 10, NULL, 0) == 0);

    /* a string ending in a lone backslash must not read one past the buffer */
    assert(json_pretty("{\"a\":\"x\\", 8, NULL, 0) == 0);

    /* pathological nesting is refused rather than indented into a wall of spaces */
    {
        char deep[2 * (JSON_PRETTY_MAX_DEPTH + 4) + 1];
        int  n = JSON_PRETTY_MAX_DEPTH + 2, i;
        for (i = 0; i < n; i++) deep[i] = '[';
        for (i = 0; i < n; i++) deep[n + i] = ']';
        deep[2 * n] = '\0';
        assert(json_pretty(deep, (size_t)(2 * n), NULL, 0) == 0);
    }
}

static void test_measure_and_bounds(void)
{
    static const char src[] = "{\"a\":[1,{\"b\":\"c\"}]}";
    size_t need = json_pretty(src, strlen(src), NULL, 0);
    char   full[256];
    char   guarded[256];
    size_t i;

    assert(need > 0 && need < sizeof full);
    assert(json_pretty(src, strlen(src), full, sizeof full) == need);

    /* A short buffer must report the full requirement and write not one byte past its cap -- that
     * report is the only reason the caller's allocation is the right size. */
    for (i = 0; i <= need; i++) {
        memset(guarded, '#', sizeof guarded);
        assert(json_pretty(src, strlen(src), guarded, i) == need);
        assert(memcmp(guarded, full, i) == 0);
        assert(guarded[i] == '#');
    }
}

int main(void)
{
    test_layout();
    test_document_is_unchanged();
    test_refusals();
    test_measure_and_bounds();
    printf("json_pretty_test: OK\n");
    return 0;
}
