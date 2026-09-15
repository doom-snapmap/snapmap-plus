#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decl_native_lex.h"
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
static sh_decl_tokens read(const char *source)
{
    sh_decl_tokens out = {0}; char error[256];
    int ok = sh_decl_native_lex((sh_decl_source){source, strlen(source)}, &out, error, sizeof(error));
    if (!ok) fprintf(stderr, "%s\n", error);
    CHECK(ok); return out;
}
static void refused(const char *source)
{
    sh_decl_tokens out = {0}; char error[256];
    CHECK(!sh_decl_native_lex((sh_decl_source){source, strlen(source)}, &out, error, sizeof(error)));
    CHECK(!out.items && !out.count && !out.source.text && error[0]);
}
int main(void)
{
    sh_decl_tokens t = read("{ parm.xy = -.5e+2; image models/boss/body.tga // comment\n value {1,2,3} }");
    CHECK(t.count == 17); CHECK(t.items[0].close == t.count - 1);
    CHECK(sh_decl_token_is(&t, 1, "parm.xy")); CHECK(sh_decl_token_is(&t, 3, "-"));
    CHECK(sh_decl_token_is(&t, 4, ".5e+2") && t.items[4].kind == SH_DECL_TOKEN_NUMBER);
    CHECK(sh_decl_token_is(&t, 7, "models/boss/body.tga"));
    CHECK(t.items[8].line == 2 && t.items[9].close == 15);
    sh_decl_tokens_free(&t);
    t = read("{ value \"path\\\" next \"a\\n\" \"b\" }");
    CHECK(t.count == 7); CHECK(sh_decl_token_is(&t, 2, "\"path\\\""));
    CHECK(sh_decl_token_is(&t, 4, "\"a\\n\"")); CHECK(sh_decl_token_is(&t, 5, "\"b\""));
    sh_decl_tokens_free(&t);
    t = read("a/b a / b _path$part .xy 12 - 3 (4 + 2) 'long literal'");
    CHECK(sh_decl_token_is(&t, 0, "a/b")); CHECK(sh_decl_token_is(&t, 2, "/"));
    CHECK(sh_decl_token_is(&t, 4, "_path$part")); CHECK(t.items[t.count - 1].kind == SH_DECL_TOKEN_LITERAL);
    sh_decl_tokens_free(&t);
    t = read("/*first\nsecond*/ a\r\n b\n");
    CHECK(t.items[0].line == 2 && t.items[1].line == 3); sh_decl_tokens_free(&t);
    refused("{]"); refused("{"); refused("/* unfinished"); refused("/* /* nested */");
    refused("\"unfinished"); refused("\"line\nbreak\""); refused("#include \"file\""); refused("$eval(1)");
    refused("<%text%>"); refused("\"a\" \\\\ \"b\""); refused("0xff"); refused("0b10");
    refused("012"); refused("1.0f"); refused("1e+"); refused("1.2.3"); refused("1.#INF");
    {
        size_t depth = 8192; char *deep = malloc(depth * 2 + 1);
        CHECK(deep);
        if (deep) {
            memset(deep, '(', depth); memset(deep + depth, ')', depth); deep[depth * 2] = 0;
            t = read(deep); CHECK(t.count == depth * 2 && t.items[0].close == depth * 2 - 1);
            sh_decl_tokens_free(&t); free(deep);
        }
    }
    {
        char error[256];
        CHECK(!sh_decl_native_lex((sh_decl_source){"a\0b", 3}, &t, error, sizeof(error)));
        CHECK(!t.items && !t.count);
        CHECK(!sh_decl_native_lex((sh_decl_source){NULL, 0}, &t, NULL, 0));
    }
    printf("native declaration lexer: %s\n", failures ? "FAIL" : "PASS"); return failures ? 1 : 0;
}
