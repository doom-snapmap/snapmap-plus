/* modprog_bank_test.c -- a mod's material decl in the bank's names (modprog_bank.c): which program a material draws
 * with, its readable keys under their bank names, the keys it does not write added with that program's defaults, and
 * every text with nothing to do served byte for byte; idempotent. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modprog.h"
#include "modprog_bank.h"

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

/* Lines of `t` whose first word is `key` (any case). */
static int lines_keyed(const char *t, const char *key)
{
    int n = 0;
    size_t k = strlen(key);
    const char *p = t;
    while (*p) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (!_strnicmp(q, key, k) && (q[k] == '\t' || q[k] == ' ')) n++;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return n;
}

/* The transform of `in`, checked to be idempotent; NULL when it left the text as it was. */
static char *bank(const char *in)
{
    char *out = NULL, *again = NULL;
    size_t n = 0, m = 0;
    int r = sh_modprog_bank(in, strlen(in), &out, &n);
    CHECK(r == SH_MODPROG_BANK_SAME || r == SH_MODPROG_BANK_CHANGED);
    if (r != SH_MODPROG_BANK_CHANGED) { CHECK(out == NULL); return NULL; }
    CHECK(out && n == strlen(out));
    CHECK(sh_modprog_bank(out, n, &again, &m) == SH_MODPROG_BANK_SAME && again == NULL);
    return out;
}

static int program(const char *t) { return sh_modprog_bank_program(t, strlen(t)); }

int main(void)
{
    unsigned k;
    char *t;
    printf("modprog_bank_test\n");

    printf("-- a material that names neither program is left as it is\n");
    CHECK(bank("{\r\nstageprogram\ttranssortblend\r\ntransatlasmap\t\"k/beam.tga\"\r\n}\r\n") == NULL);
    CHECK(bank("") == NULL);
    /* the blend's readable keys on another program are not the blend's */
    CHECK(bank("{\n\tambientprogram\tskinvmtr\n\tlayerstilingfactor\t{ 2, 2, 2, 2 }\n}\n") == NULL);
    CHECK(program("{\n\tambientprogram\tskinvmtr\n}\n") == SH_MODPROG_BANK_NONE);

    printf("-- a multilayer material: readable keys renamed, every other bank key added with the blend's default\n");
    t = bank("{\r\n\tambientprogram\t\"snapmapplus/multilayer/skinvmtr\"\r\n\tlayerstilingfactor\t{ 2, 2, 2, 2 }\r\n"
             "\tMultiBlendSoftness\t{ 0.5, 0.5, 0.5, 0.5 }\r\n\tlayerblendmask\tk/mask.tga\r\n}\r\n");
    CHECK(t != NULL);
    if (t) {
        CHECK(strstr(t, "\tsnapmapplusvec0\t{ 2, 2, 2, 2 }\r\n") != NULL);
        CHECK(strstr(t, "\tsnapmapplusvec3\t{ 0.5, 0.5, 0.5, 0.5 }\r\n") != NULL);
        CHECK(strstr(t, "\tlayerblendmask\tk/mask.tga\r\n") != NULL);              /* a texture keeps its name */
        for (k = 0; k < SH_MODPROG_ML_PARM_COUNT; k++) {
            CHECK(lines_keyed(t, SH_MODPROG_ML_PARMS[k]) == 1);
            CHECK(lines_keyed(t, SH_MODPROG_ML_NAMES[k]) == 0);
        }
        CHECK(strstr(t, "snapmapplusvec1\t{ 1.0, 1.0, 1.0, 1.0 }\r\nsnapmapplusvec2\t") != NULL);
        CHECK(strstr(t, "snapmapplusvec27\t{ 1.0, 0.07, 0.0, 0.0 }\r\n}\r\n") != NULL);   /* before the brace */
        CHECK(!strstr(t, "snapmapplusvec28"));
        CHECK(!strstr(t, "\n\n") && !strstr(t, "\r\r"));
        free(t);
    }
    /* the program unquoted, in another case */
    t = bank("{\n  ambientProgram SNAPMAPPLUS/MULTILAYER/skinvmtrwounds\n  multilayerEmberHot { 1, 0, 0, 0 }\n}");
    CHECK(t != NULL);
    if (t) {
        CHECK(strstr(t, "\n  snapmapplusvec27 { 1, 0, 0, 0 }\n") != NULL);
        CHECK(strstr(t, "\nsnapmapplusvec0\t{ 1.0, 1.0, 1.0, 1.0 }\n") != NULL);
        CHECK(strstr(t, "snapmapplusvec26\t{ 1.0, 4.0, 0.7, 0.4 }\n}") != NULL && t[strlen(t) - 1] == '}');
        CHECK(lines_keyed(t, "snapmapplusvec26") == 1);
        free(t);
    }
    CHECK(program("{\n ambientprogram \"Snapmapplus/Multilayer/skinvmtr\"\n}") == SH_MODPROG_BANK_MULTILAYER);

    printf("-- a multilayer material already in the bank's names is served byte for byte\n");
    {
        char full[4096];
        size_t n = 0;
        n += (size_t)sprintf(full + n, "{\n\tambientprogram\tsnapmapplus/multilayer/skinvmtr\n");
        for (k = 0; k < SH_MODPROG_BANK_COUNT; k++) n += (size_t)sprintf(full + n, "\tsnapmapplusvec%u\t{ 3, 3, 3, 3 }\n", k);
        sprintf(full + n, "}\n");
        CHECK(bank(full) == NULL);
    }

    printf("-- a particle material: readable keys renamed, the others of its four added with the shading's defaults\n");
    t = bank("{\r\nstageprogram\t\"snapmapplus/eternal/uberparticles\"\r\ntransmap\tk/smoke.tga\r\n"
             "eternalparticle\t{ 1.000000, 2.000000, 0.000000, 0.000000 }\r\n"
             "layerstilingfactor\t{ 9, 9, 9, 9 }\r\n}\r\n");
    CHECK(t != NULL);
    if (t) {
        CHECK(strstr(t, "\r\nsnapmapplusvec0\t{ 1.000000, 2.000000, 0.000000, 0.000000 }\r\n") != NULL);
        CHECK(strstr(t, "\r\nlayerstilingfactor\t{ 9, 9, 9, 9 }\r\n") != NULL);   /* not the particle program's */
        CHECK(strstr(t, "snapmapplusvec1\t{ 0.0, 0.0, 0.0, 0.0 }\r\nsnapmapplusvec2\t{ 1.0, 1.0, 1.0, 1.0 }\r\n"
                        "snapmapplusvec3\t{ 1.0, 1.0, 1.0, 1.0 }\r\n}\r\n") != NULL);
        CHECK(!strstr(t, "snapmapplusvec4") && !strstr(t, "eternalparticle\t"));
        free(t);
    }
    /* a bank key alone makes it the particle program's */
    t = bank("{\nstageprogram\tsnapmapplus/eternal/uberparticles\nsnapmapplusvec2\t{ 1, 0, 0, 1 }\n}\n");
    CHECK(t && lines_keyed(t, "snapmapplusvec0") == 1 && lines_keyed(t, "snapmapplusvec2") == 1 &&
          strstr(t, "snapmapplusvec2\t{ 1, 0, 0, 1 }\n") != NULL);
    free(t);
    /* one that writes none of the four reads the bank parms' decl defaults (the particle program's own) */
    CHECK(bank("{\nstageprogram\t\"snapmapplus/eternal/uberparticles\"\ntransmap\tk/smoke.tga\n}\n") == NULL);

    printf("-- only keys of the decl's own block, outside comments\n");
    t = bank("{\n// eternalparticle { 0, 0, 0, 0 }\n/* eternalparticlecolor\neternalparticlecolor2 */\n"
             "stage {\n\teternalparticlefade\t{ 1, 1, 1, 1 }\n}\neternalparticlecolorx\t{ 1, 1, 1, 1 }\n"
             "EternalParticleColor\t{ 0.5, 0.5, 0.5, 1 }\n}\n");
    CHECK(t != NULL);
    if (t) {
        CHECK(strstr(t, "// eternalparticle { 0, 0, 0, 0 }\n/* eternalparticlecolor\neternalparticlecolor2 */\n") != NULL);
        CHECK(strstr(t, "\teternalparticlefade\t{ 1, 1, 1, 1 }\n") != NULL);
        CHECK(strstr(t, "\neternalparticlecolorx\t{ 1, 1, 1, 1 }\n") != NULL);
        CHECK(strstr(t, "\nsnapmapplusvec2\t{ 0.5, 0.5, 0.5, 1 }\n") != NULL);
        CHECK(lines_keyed(t, "snapmapplusvec0") == 1 && lines_keyed(t, "snapmapplusvec1") == 1 &&
              lines_keyed(t, "snapmapplusvec3") == 1);
        free(t);
    }

    printf("-- a closing brace that is not alone on its line\n");
    t = bank("{\neternalparticle\t{ 1, 2, 3, 4 } }");
    CHECK(t && !strcmp(t, "{\nsnapmapplusvec0\t{ 1, 2, 3, 4 } \nsnapmapplusvec1\t{ 0.0, 0.0, 0.0, 0.0 }\n"
                          "snapmapplusvec2\t{ 1.0, 1.0, 1.0, 1.0 }\nsnapmapplusvec3\t{ 1.0, 1.0, 1.0, 1.0 }\n}"));
    free(t);

    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
