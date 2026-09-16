/* Composition of the registered native block-grammar families. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/decl_block_compose.h"

static int failures;

static void report(const char *name, const char *detail)
{
    printf("FAIL %s: %s\n", name, detail);
    failures++;
}

static sh_decl_source src(const char *text)
{
    sh_decl_source s = {text, strlen(text)};
    return s;
}

/* Compose baseline plus contributions and require every expectation to appear
 * in the result, with no refusal. */
static void pass(const char *name, const char *family, const char *baseline,
    const char *a, const char *b, const char *const *expected, size_t count)
{
    sh_decl_source sources[2];
    char error[512] = "";
    size_t length = 0, used = 0;
    char *out;
    sources[used++] = src(a);
    if (b) sources[used++] = src(b);
    out = sh_decl_block_compose(family, src(baseline), sources, used, &length,
        error, sizeof(error), NULL);
    if (!out) { report(name, error[0] ? error : "refused"); return; }
    for (size_t i = 0; i < count; i++) {
        if (!strstr(out, expected[i])) {
            char detail[512];
            snprintf(detail, sizeof(detail), "missing '%s' in:\n%s", expected[i], out);
            report(name, detail);
            break;
        }
    }
    free(out);
}

/* Compose and require a refusal whose diagnostic mentions the given fragment. */
static void refuse(const char *name, const char *family, const char *baseline,
    const char *a, const char *b, const char *fragment)
{
    sh_decl_source sources[2];
    char error[512] = "";
    size_t length = 0, used = 0;
    char *out;
    sources[used++] = src(a);
    if (b) sources[used++] = src(b);
    out = sh_decl_block_compose(family, src(baseline), sources, used, &length,
        error, sizeof(error), NULL);
    if (out) {
        char detail[512];
        snprintf(detail, sizeof(detail), "composed instead of refusing:\n%s", out);
        report(name, detail);
        free(out);
        return;
    }
    if (fragment && !strstr(error, fragment)) {
        char detail[512];
        snprintf(detail, sizeof(detail), "expected '%s', got '%s'", fragment, error);
        report(name, detail);
    }
}

int main(void)
{
    /* ---------------------------------------------------------- breakable */
    {
        static const char *expect[] = {"bouncyness 0.9", "gravity 0 0 -750.0", "maxSimulationTime 40"};
        /* Two packages change different scalars that the author wrote on one
         * physical line. The native reader counts values, so both survive. */
        pass("breakable one line two fields", "breakable",
            "{ model models/a.lwo bouncyness 0.35 gravity 0 0 -750.0 maxSimulationTime 20 }",
            "{ model models/a.lwo bouncyness 0.9 gravity 0 0 -750.0 maxSimulationTime 20 }",
            "{ model models/a.lwo bouncyness 0.35 gravity 0 0 -750.0 maxSimulationTime 40 }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        static const char *expect[] = {"gravity 0 0 -100"};
        pass("breakable lone contribution", "breakable",
            "{ model models/a.lwo\ngravity 0 0 -750.0\n}",
            "{ model models/a.lwo\ngravity 0 0 -100\n}", NULL,
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        static const char *expect[] = {"bouncyness 0.5"};
        pass("breakable equal contributions", "breakable",
            "{ model models/a.lwo bouncyness 0.35 }",
            "{ model models/a.lwo bouncyness 0.5 }",
            "{ model models/a.lwo bouncyness 0.5 }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    refuse("breakable same field", "breakable",
        "{ model models/a.lwo bouncyness 0.35 }",
        "{ model models/a.lwo bouncyness 0.5 }",
        "{ model models/a.lwo bouncyness 0.9 }", "bouncyness");
    {
        /* A nested block reader composes at its own field granularity. */
        static const char *expect[] = {"radius 200", "impulse 900"};
        pass("breakable nested block fields", "breakable",
            "{ model models/a.lwo explosion { radius 100 impulse 500 delay 0 } }",
            "{ model models/a.lwo explosion { radius 200 impulse 500 delay 0 } }",
            "{ model models/a.lwo explosion { radius 100 impulse 900 delay 0 } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        /* A positional list keeps its entries by position. */
        static const char *expect[] = {"2 \"left door\"", "3 \"piece 3\""};
        pass("breakable positional list", "breakable",
            "{ model models/a.lwo pieceNames { 1 \"piece 1\" 2 \"piece 2\" 3 \"piece 3\" } }",
            "{ model models/a.lwo pieceNames { 1 \"piece 1\" 2 \"left door\" 3 \"piece 3\" } }",
            NULL, expect, sizeof(expect) / sizeof(expect[0]));
    }
    /* A word the reader does not accept where a record starts is refused. A
     * word that follows a key is one of that key's values, which is how a flag
     * list and an enum value reach the reader. */
    refuse("breakable unsupported key", "breakable",
        "{ model models/a.lwo }", "{ inventedKey 1 model models/a.lwo }", NULL,
        "unsupported key 'inventedKey'");
    {
        static const char *expect[] = {"clipMask solid, ikclip, corpse"};
        pass("breakable flag list value", "breakable",
            "{ model models/a.lwo clipMask solid, ikclip }",
            "{ model models/a.lwo clipMask solid, ikclip, corpse }", NULL,
            expect, sizeof(expect) / sizeof(expect[0]));
    }

    /* ---------------------------------------------------------------- env */
    {
        /* A render parm value may be a bracketed component list; changing one
         * parm must not disturb another written on the same line. */
        static const char *expect[] = {"fogScale 0.5", "fogColor { 1, 0.9, 0.8 }", "fogStart 600.0"};
        pass("env parms on one line", "env",
            "{ renderParms { fogColor { 1, 0.9, 0.8 } fogScale 0.00008 fogStart 600.0 } }",
            "{ renderParms { fogColor { 1, 0.9, 0.8 } fogScale 0.5 fogStart 600.0 } }",
            NULL, expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        static const char *expect[] = {"inherit", "default_snapmap", "allowOverride"};
        pass("env inherit and override", "env",
            "{ inherit { default } allowOverride = true; }",
            "{ inherit { default_snapmap } allowOverride = true; }",
            NULL, expect, sizeof(expect) / sizeof(expect[0]));
    }
    refuse("env same parm", "env",
        "{ renderParms { fogScale 0.1 } }",
        "{ renderParms { fogScale 0.2 } }",
        "{ renderParms { fogScale 0.3 } }", "fogScale");

    /* -------------------------------------------------------------- table */
    {
        static const char *expect[] = {"min 0", "max 2", "{0.1, 0.9}"};
        pass("table range and samples", "table",
            "{ clamp min 0 max 1 left 0 right 1 {0.1, 0.9} }",
            "{ clamp min 0 max 2 left 0 right 1 {0.1, 0.9} }",
            NULL, expect, sizeof(expect) / sizeof(expect[0]));
    }
    refuse("table same samples", "table",
        "{ min 0 max 1 left 0 right 1 {0.1, 0.9} }",
        "{ min 0 max 1 left 0 right 1 {0.2, 0.9} }",
        "{ min 0 max 1 left 0 right 1 {0.3, 0.9} }", "group");

    /* --------------------------------------------------------------- skins */
    {
        /* Channels are author-named; two packages adding different channels
         * both survive, and a pair list keeps its positions. */
        static const char *expect[] = {"echo {", "bloody {", "models/b_echo"};
        pass("skins independent channels", "skins",
            "{ echo { \"models/a\" \"models/a_echo\" } }",
            "{ echo { \"models/a\" \"models/a_echo\" \"models/b\" \"models/b_echo\" } }",
            "{ echo { \"models/a\" \"models/a_echo\" } bloody { \"models/a\" \"models/a_blood\" } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }

    /* --------------------------------------------- articulated figure */
    {
        /* A body origin names a joint, and "joint" is also a body keyword: the
         * value still belongs to origin, so both edits land. */
        static const char *expect[] = {"origin joint \"spine\"", "density 0.5", "joint \"hips\""};
        pass("af body origin names a joint", "articulatedfigure",
            "{ settings { model \"a.md6\" contents corpse clipMask solid, ikclip }\n"
            "body \"torso\" { joint \"hips\" origin joint \"neck\" density 0.2 } }",
            "{ settings { model \"a.md6\" contents corpse clipMask solid, ikclip }\n"
            "body \"torso\" { joint \"hips\" origin joint \"spine\" density 0.2 } }",
            "{ settings { model \"a.md6\" contents corpse clipMask solid, ikclip }\n"
            "body \"torso\" { joint \"hips\" origin joint \"neck\" density 0.5 } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        /* Two packages add different named bodies to one figure. */
        static const char *expect[] = {"body \"arm\"", "body \"leg\"", "body \"torso\""};
        pass("af independent bodies", "articulatedfigure",
            "{ settings { model \"a.md6\" }\nbody \"torso\" { density 0.2 } }",
            "{ settings { model \"a.md6\" }\nbody \"torso\" { density 0.2 } body \"arm\" { density 0.1 } }",
            "{ settings { model \"a.md6\" }\nbody \"torso\" { density 0.2 } body \"leg\" { density 0.3 } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        static const char *expect[] = {"lcpEpsilon 0.0005", "errorReduction 0.9"};
        pass("af solver constants", "articulatedfigure",
            "{ settings { model \"a.md6\" solverConstants { errorReduction 0.75 lcpEpsilon 0.0001 } } }",
            "{ settings { model \"a.md6\" solverConstants { errorReduction 0.9 lcpEpsilon 0.0001 } } }",
            "{ settings { model \"a.md6\" solverConstants { errorReduction 0.75 lcpEpsilon 0.0005 } } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    refuse("af same body field", "articulatedfigure",
        "{ settings { model \"a.md6\" }\nbody \"torso\" { density 0.2 } }",
        "{ settings { model \"a.md6\" }\nbody \"torso\" { density 0.4 } }",
        "{ settings { model \"a.md6\" }\nbody \"torso\" { density 0.6 } }", "density");

    /* ------------------------------------------------------------ animweb */
    {
        static const char *expect[] = {"state \"opened\"", "state \"locked\"", "gridSize 64"};
        pass("animweb independent states", "animweb",
            "{ props { gridSize 32 }\nlayers { }\nstates { state \"locked\" { } }\n"
            "scalars { }\nsubWebs { } }",
            "{ props { gridSize 64 }\nlayers { }\nstates { state \"locked\" { } }\n"
            "scalars { }\nsubWebs { } }",
            "{ props { gridSize 32 }\nlayers { }\n"
            "states { state \"locked\" { } state \"opened\" { } }\nscalars { }\nsubWebs { } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        /* A node deep inside a sub-web composes at its own field granularity. */
        static const char *expect[] = {"toState \"unlocked\"", "destDuration 12"};
        pass("animweb nested edge parms", "animweb",
            "{ subWebs { subWeb \"w\" { node \"locked\" { edges { edge {\n"
            "toState \"locked\" blendParms { destDuration 4 duration 0 } } } } } } }",
            "{ subWebs { subWeb \"w\" { node \"locked\" { edges { edge {\n"
            "toState \"unlocked\" blendParms { destDuration 4 duration 0 } } } } } } }",
            "{ subWebs { subWeb \"w\" { node \"locked\" { edges { edge {\n"
            "toState \"locked\" blendParms { destDuration 12 duration 0 } } } } } } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        /* wrap takes one enum value; the reader does not read to end of line. */
        static const char *expect[] = {"wrap WRAP_REPEAT", "rate 2"};
        pass("animweb alias enum value", "animweb",
            "{ subWebs { subWeb \"w\" { node \"n\" { blendTrees { tree { modelIndex 0\n"
            "anims { alias { name \"a\" wrap WRAP_CLAMP rate 1 } } } } } } } }",
            "{ subWebs { subWeb \"w\" { node \"n\" { blendTrees { tree { modelIndex 0\n"
            "anims { alias { name \"a\" wrap WRAP_REPEAT rate 1 } } } } } } } }",
            "{ subWebs { subWeb \"w\" { node \"n\" { blendTrees { tree { modelIndex 0\n"
            "anims { alias { name \"a\" wrap WRAP_CLAMP rate 2 } } } } } } } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }

    /* ------------------------------------------------------- renderparm */
    {
        static const char *expect[] = {"Vec { 1.0, 16.0, 1.0, 1.0 }"};
        pass("renderparm vector value", "renderparm",
            "{ Vec { 1.0, 8.0, 1.0, 1.0 } }",
            "{ Vec { 1.0, 16.0, 1.0, 1.0 } }", NULL,
            expect, sizeof(expect) / sizeof(expect[0]));
    }

    /* --------------------------------------------------------------- cloth */
    {
        static const char *expect[] = {"mass 2.0", "friction 1"};
        pass("cloth scalars", "cloth",
            "{ mass 1.0 gravity -10000.0 friction 1 type generic }",
            "{ mass 2.0 gravity -10000.0 friction 1 type generic }", NULL,
            expect, sizeof(expect) / sizeof(expect[0]));
    }

    /* ------------------------------------------- detail, foliage, visemes */
    {
        static const char *expect[] = {"colorVariance 0.5", "model models/b.lwo"};
        pass("detail scalars", "detail",
            "{ model models/a.lwo colorVariance 0.2 }",
            "{ model models/b.lwo colorVariance 0.2 }",
            "{ model models/a.lwo colorVariance 0.5 }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        static const char *expect[] = {"quadWidth 32", "material textures/foliage/grass"};
        pass("foliage scalars", "foliage",
            "{ quadWidth 16 quadHeight 16 material textures/foliage/weed autosprites 1 }",
            "{ quadWidth 32 quadHeight 16 material textures/foliage/weed autosprites 1 }",
            "{ quadWidth 16 quadHeight 16 material textures/foliage/grass autosprites 1 }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        static const char *expect[] = {"weightScale 2", "phonemeSet \"english\""};
        pass("visemeset root scalars", "visemeset",
            "{ phonemeSet \"english\" weightScale 1 durationScale 1 phonemes { } }",
            "{ phonemeSet \"english\" weightScale 2 durationScale 1 phonemes { } }",
            NULL, expect, sizeof(expect) / sizeof(expect[0]));
    }

    /* ------------------------------------------------------------ families */
    if (!sh_decl_block_family("breakable")) report("family table", "breakable is not registered");
    if (sh_decl_block_family("md6def")) report("family table", "md6def must use its own adapter");
    if (sh_decl_block_family("renderprog")) report("family table", "renderprog is program text");

    printf("block composition: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
