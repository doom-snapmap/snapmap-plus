/* sdk_test.c -- the Graphics SDK's effects, particles, environments and values that change (sdk.h, modeffect.h,
 * modenv.h, sdkvalues.h): every effect item compiled and read back as the game's fx decl, the derived particle decl
 * and material, environments, values over time, the served emitter, each mistake's sentence and determinism. No game.
 *   sdk_test.exe */
#define WIN32_LEAN_AND_MEAN
#include "heap_out.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk.h"
#include "sdkparse.h"
#include "sdkvalues.h"
#include "sdkwords.h"
#include "modeffect.h"
#include "modenv.h"
#include "modprog.h"
#include "modbundle.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *message) { (void)message; }

/* registration */

/* The decls the compilers register, captured (the default registry is used where a test says so). */
typedef struct reg { char type[32]; char name[80]; char *text; } reg;
static reg g_regs[256];
static int g_nregs;

static const char *capture(const char *type, const char *text)
{
    const char *name = sh_modprog_register_decl(type, text);
    if (name && g_nregs < 256) {
        snprintf(g_regs[g_nregs].type, sizeof g_regs[g_nregs].type, "%s", type);
        snprintf(g_regs[g_nregs].name, sizeof g_regs[g_nregs].name, "%s", name);
        g_regs[g_nregs].text = _strdup(text);
        g_nregs++;
    }
    return name;
}

static const char *registered(const char *type, const char *name)
{
    int i;
    for (i = 0; i < g_nregs; i++) if (!strcmp(g_regs[i].type, type) && !strcmp(g_regs[i].name, name)) return g_regs[i].text;
    return NULL;
}

static int count_type(const char *type)
{
    int i, n = 0;
    for (i = 0; i < g_nregs; i++) if (!strcmp(g_regs[i].type, type)) n++;
    return n;
}

/* helpers */

typedef struct res { int r; char *out; size_t n; char why[400]; char *notes, *needs; } res;

static res compile(const char *path, const char *text)
{
    res x;
    memset(&x, 0, sizeof x);
    x.r = sh_sdk_compile(path, text, strlen(text), &x.out, &x.n, x.why, sizeof x.why, &x.notes, &x.needs);
    return x;
}
static void release(res *x) { free(x->out); free(x->notes); free(x->needs); memset(x, 0, sizeof *x); }

static int has(const char *text, const char *s) { return text && strstr(text, s) != NULL; }

/* The value of `key = value;` at the n-th occurrence, unquoted, as a static string; "" when absent. */
static const char *kv(const char *text, const char *key, int nth)
{
    static char v[256];
    char pat[80];
    const char *p = text;
    snprintf(pat, sizeof pat, "\t%s = ", key);
    v[0] = 0;
    while (p && (p = strstr(p, pat)) != NULL) {
        if (nth-- == 0) {
            const char *s = p + strlen(pat), *e = strchr(s, ';');
            size_t n;
            if (!e) return v;
            if (*s == '"') { s++; if (e > s && e[-1] == '"') e--; }
            n = (size_t)(e - s) < sizeof v - 1 ? (size_t)(e - s) : sizeof v - 1;
            memcpy(v, s, n);
            v[n] = 0;
            return v;
        }
        p += strlen(pat);
    }
    return v;
}

static int count(const char *text, const char *s)
{
    int n = 0;
    const char *p = text;
    while (p && (p = strstr(p, s)) != NULL) { n++; p += strlen(s); }
    return n;
}

#define FX "generated/decls/fx/mods/abcdefgh/blast.decl"
#define ENV "generated/decls/env/mods/abcdefgh/hell_red.decl"
#define MAT "generated/decls/material/mods/abcdefgh/rune.decl"

/* the tests */

static void test_parse(void)
{
    sdk_doc d;
    char why[300];
    const char *t = "{\n\ttype\teffect\n\tlight {\n\t\tcolour { 1, 0.5, 0.2 }\n\t\tbrightness pulse { rate 2, low 1, high 5 }\n\t}\n}\n";
    CHECK(sdk_parse("a/b/x.decl", t, strlen(t), &d, why, sizeof why) == 1);
    CHECK(d.root->count == 2);
    CHECK(sdk_is(&d.root->items[0].atoms[0], "type") && sdk_is(&d.root->items[0].atoms[1], "effect"));
    CHECK(d.root->items[1].count == 2 && d.root->items[1].atoms[1].kind == SDK_A_GROUP);
    {
        const sdk_group *g = d.root->items[1].atoms[1].group;
        double v[4];
        CHECK(g->count == 2);
        CHECK(sdk_numbers(&g->items[0].atoms[1], v, 4) == 3 && v[1] == 0.5);
        CHECK(g->items[1].count == 3 && g->items[1].line == 5);
    }
    CHECK(!strcmp(d.file, "x.decl"));
    sdk_doc_free(&d);
    {
        const char *u = "{\n\ttype effect\n\tlight {\n}\n";
        CHECK(sdk_parse("x.decl", u, strlen(u), &d, why, sizeof why) == 0);
        CHECK(has(why, "never closed"));
    }
    {
        const char *u = "{ name native(\"lights/gaus\") }";
        CHECK(sdk_parse("x.decl", u, strlen(u), &d, why, sizeof why) == 1);
        CHECK(d.root->items[0].atoms[1].kind == SDK_A_STRING && d.root->items[0].atoms[1].game);
        sdk_doc_free(&d);
    }
    {
        const char *u = "{ name native:effects/blast }";
        CHECK(sdk_parse("x.decl", u, strlen(u), &d, why, sizeof why) == 1);
        CHECK(d.root->items[0].atoms[1].kind == SDK_A_STRING && d.root->items[0].atoms[1].game &&
              d.root->items[0].atoms[1].n == 13 && !strncmp(d.root->items[0].atoms[1].p, "effects/blast", 13));
        sdk_doc_free(&d);
    }
    {
        char kind[16];
        const char *u = "// a comment\n{\n\t// another\n\ttype\tenvironment\n}";
        CHECK(sdk_first_type(u, strlen(u), kind, sizeof kind) && !strcmp(kind, "environment"));
        CHECK(!sdk_first_type("{ look particle }", 17, kind, sizeof kind));
    }
    CHECK(!strcmp(SDK_W(IT_LIGHT), "light"));
}

static const char *EXAMPLE =
    "{\n"
    "\ttype\t\teffect\n"
    "\tlasts\t\t2\n"
    "\tlight {\n"
    "\t\tcolour\t\t{ 1, 0.45, 0.1 }\n"
    "\t\tbrightness\t5\n"
    "\t\treach\t\t300\n"
    "\t\tflicker\t\tfire\n"
    "\t\tfadeout\t\t0.5\n"
    "\t}\n"
    "\tdecal {\n"
    "\t\tmaterial\tnative(\"textures/decals/blood_splatter/blood_splatter_01\")\n"
    "\t\tsize\t\t64\n"
    "\t\tlasts\t\t10\n"
    "\t\tfadeout\t\t2\n"
    "\t}\n"
    "\tshake {\n"
    "\t\tstrength\t0.4\n"
    "\t\tlasts\t\t0.5\n"
    "\t\treach\t\t800\n"
    "\t}\n"
    "\tflash {\n"
    "\t\tbrightness\t0.6\n"
    "\t\tlasts\t\t0.2\n"
    "\t\treach\t\t600\n"
    "\t}\n"
    "\tsound {\n"
    "\t\tsound\t\tnative(\"effects/explosions/grenade_explosion_default\")\n"
    "\t}\n"
    "}\n";

static void test_effect_items(void)
{
    res x = compile(FX, EXAMPLE);
    CHECK(x.r == SH_LOOK_CHANGED);
    CHECK(x.out && !strncmp(x.out, SH_EFFECT_MARK "\n{\n\tedit = {\n\t\teditEvents = {\n\t\t\tnum = 5;", strlen(SH_EFFECT_MARK) + 40));
    CHECK(!strcmp(kv(x.out, "type", 0), "FX_LIGHT"));
    CHECK(!strcmp(kv(x.out, "type", 1), "FX_DECAL"));
    CHECK(!strcmp(kv(x.out, "type", 2), "FX_SCREEN_SHAKE"));
    CHECK(!strcmp(kv(x.out, "type", 3), "FX_SCREEN_FLASH"));
    CHECK(!strcmp(kv(x.out, "type", 4), "FX_SOUND"));
    /* the light: colour every component written, the fire light material, the effect's lasts */
    CHECK(!strcmp(kv(x.out, "lightMtr", 0), "lights/fire_light_01"));
    CHECK(!strcmp(kv(x.out, "intensity", 0), "5"));
    CHECK(!strcmp(kv(x.out, "duration", 0), "2"));       /* the effect's lasts, for an item that names none */
    CHECK(!strcmp(kv(x.out, "fadeOutTime", 0), "0.5"));
    CHECK(has(x.out, "\t\t\t\tcolor = {\n\t\t\t\t\tx = 1;\n\t\t\t\t\ty = 0.45;\n\t\t\t\t\tz = 0.1;\n\t\t\t\t\tw = 1;\n"));
    CHECK(has(x.out, "radius = {\n\t\t\t\t\t\tx = 300;\n\t\t\t\t\t\ty = 300;\n\t\t\t\t\t\tz = 300;"));
    /* the decal */
    CHECK(!strcmp(kv(x.out, "decalMtr", 0), "textures/decals/blood_splatter/blood_splatter_01"));
    CHECK(!strcmp(kv(x.out, "size", 0), "64"));
    CHECK(!has(x.out, "depth ="));                       /* left out: the engine's own */
    /* shake and flash by distance */
    CHECK(!strcmp(kv(x.out, "magnitude", 0), "0.4"));
    CHECK(!strcmp(kv(x.out, "distance", 0), "800"));
    CHECK(!strcmp(kv(x.out, "exposure", 0), "0.6"));
    CHECK(!strcmp(kv(x.out, "distance", 1), "600"));
    CHECK(!strcmp(kv(x.out, "sound", 0), "effects/explosions/grenade_explosion_default"));
    CHECK(count(x.out, "item[0] = \"FX_NONE\";") == 10);
    /* the emitter is a need */
    CHECK(x.needs && !strcmp(x.needs, "entityDef snapmapplus/effect/mods/abcdefgh/blast\n"));
    /* the same text compiles to the same bytes */
    {
        res y = compile(FX, EXAMPLE);
        CHECK(y.r == SH_LOOK_CHANGED && y.n == x.n && !memcmp(x.out, y.out, x.n));
        release(&y);
    }
    release(&x);
}

static void test_effect_more_items(void)
{
    const char *t =
        "{\n type effect\n"
        " screen { material native(\"textures/effects/misc/screen_player_damage_02\")\n colour { 1, 0, 0 }\n tiles { 0.5, 0.5 }\n mirror yes }\n"
        " blur { style soft\n strength 0.02 }\n"
        " blur { strength 200 }\n"
        " focus { near 10\n focus 200\n far 2000 }\n"
        " flare { use native(\"flares/explosionflare\")\n size 0.75 }\n"
        " trail { use native(\"plasma_freeze\") }\n"
        " model { model native(\"models/effects/muzzleflash/plasma_round.lwo\")\n size 2 }\n"
        " glow { colour { 0, 0.6, 1 }\n strength 0.5\n fadein 0.2\n fadeout 0.5 }\n"
        " wind { strength 3\n angle 90 }\n"
        " environment { use native(\"grenade_hit\") }\n"
        " light { at fx_chest\n when spawn_start\n offset { 0, 0, 32 }\n starts { 0.1, 0.3 }\n repeats yes }\n"
        "}\n";
    res x = compile(FX, t);
    CHECK(x.r == SH_LOOK_CHANGED);
    CHECK(has(x.out, "\"FX_SCREEN_OVERLAY\"") && has(x.out, "mirrorTiles = true;"));
    CHECK(has(x.out, "tint = {\n") && has(x.out, "r = 1;"));
    CHECK(has(x.out, "\"FX_BLUR_GAUSSIAN\"") && has(x.out, "blurGaussianParms = {"));
    CHECK(has(x.out, "\"FX_BLUR_RADIAL\"") && has(x.out, "blurRadial2Parms = {"));
    CHECK(!strcmp(kv(x.out, "focusDepth", 0), "200"));
    CHECK(!strcmp(kv(x.out, "declFlare", 0), "flares/explosionflare") && has(x.out, "isAutosprited = true;"));
    CHECK(!strcmp(kv(x.out, "declRibbon", 0), "plasma_freeze"));
    CHECK(!strcmp(kv(x.out, "modelName", 0), "models/effects/muzzleflash/plasma_round.lwo"));
    CHECK(!strcmp(kv(x.out, "declRenderParm", 0), "bloomcolor"));
    CHECK(has(x.out, "y = 0.3;"));                             /* the glow's colour times its strength */
    CHECK(has(x.out, "\"FX_WIND\""));
    CHECK(!strcmp(kv(x.out, "declEnv", 0), "grenade_hit") && has(x.out, "\"FX_ENV_OVERRIDE\""));
    CHECK(has(x.out, "item[0] = \"FX_SPAWN_START\";"));
    CHECK(has(x.out, "item[0] = \"fx_chest\";"));
    CHECK(has(x.out, "looping = true;"));
    CHECK(has(x.out, "delay = {\n\t\t\t\t\tx = 0.1;\n\t\t\t\t\ty = 0.3;"));
    release(&x);
}

static void test_effect_environment_item(void)
{
    int i;
    char t[4096];
    /* 18 values: bloom alone writes six; split over two items of 16 and 2 */
    snprintf(t, sizeof t, "{\n type effect\n environment {\n fog { 0.5, 0, 0 }\n fogdensity 0.05\n bloom 2\n"
             " saturation 0.3\n tint { 1, 0.8, 0.8 }\n brightness 1.2\n vignette 0.8\n vignettecolour { 0.3, 0, 0 }\n"
             " grain 0.05\n fringe 0.2\n fogstart 10\n fogend 3000\n exposure { 2, 8 }\n lasts 3\n fadein 0.5 }\n}\n");
    {
        res x = compile(FX, t);
        CHECK(x.r == SH_LOOK_CHANGED);
        CHECK(count(x.out, "\"FX_ENV_CHANGE\"") == 2);
        CHECK(has(x.out, "num = 16;") && has(x.out, "num = 2;"));
        CHECK(has(x.out, "parm = \"fogcolor\";"));
        CHECK(has(x.out, "parm = \"bloomweight0\";") && has(x.out, "parm = \"bloomweight5\";"));
        CHECK(has(x.out, "parm = \"automaxexposure\";") == 0);
        CHECK(has(x.out, "parm = \"autoexposuremax\";"));
        /* the fog colour is sRGB-decoded as the env decl's own parse decodes it */
        for (i = 0; i < 1; i++) CHECK(has(x.out, "x = 0.21404114;"));
        CHECK(count(x.out, "fadeInTime = 0.5;") == 2 && count(x.out, "duration = 3;") == 2);
        release(&x);
    }
}

static void test_effect_light_values(void)
{
    const char *t = "{\n type effect\n light { brightness pulse { rate 2, low 1, high 4 }\n flicker none }\n}\n";
    res x;
    g_nregs = 0;
    sdk_set_register(capture);
    x = compile(FX, t);
    CHECK(x.r == SH_LOOK_CHANGED);
    CHECK(!strcmp(kv(x.out, "intensity", 0), "4"));
    CHECK(kv(x.out, "intensityTable", 0)[0] && !strncmp(kv(x.out, "intensityTable", 0), "snapmapplus_sdk_", 16));
    CHECK(registered("table", kv(x.out, "intensityTable", 0)) != NULL);
    CHECK(!strcmp(kv(x.out, "lightMtr", 0), "lights/gaus"));
    release(&x);
    sdk_set_register(NULL);
}

static void test_particles(void)
{
    const char *t =
        "{\n type effect\n particles {\n"
        "  image mods/abcdefgh/fx/ember.tga\n count 300\n life { 0.6, 1.4 }\n from sphere 24\n direction up\n spread 25\n"
        "  speed { 80, 160 }\n fall 400\n size curve { 0 4, 0.3 10, 1 0 }\n colour fade { from { 1, 0.8, 0.3 }, to { 0.6, 0.05, 0 } }\n"
        "  fadein 0.1\n fadeout 0.4\n repeats yes\n frames { 4, 4 }\n framerate 12\n faces motion\n }\n}\n";
    res x;
    const char *prt, *mtr;
    char name[96];
    g_nregs = 0;
    sdk_set_register(capture);
    x = compile(FX, t);
    CHECK(x.r == SH_LOOK_CHANGED);
    snprintf(name, sizeof name, "%s", kv(x.out, "declPrt", 0));
    CHECK(!strncmp(name, "snapmapplus/sdk/", 16));
    prt = registered("particle", name);
    CHECK(prt != NULL);
    if (prt) {
        CHECK(has(prt, "stages = {\n\t\t\tnum = 2;"));
        CHECK(count(prt, "totalParticles = 150;") == 2);
        CHECK(count(prt, "diversity = 1031;") == 1);
        CHECK(!strcmp(kv(prt, "cycles", 0), "0"));
        CHECK(!strcmp(kv(prt, "type", 0), "PDIST_SPHERE"));
        CHECK(!strcmp(kv(prt, "parms[0]", 0), "25"));
        CHECK(count(prt, "val0 = 120;") >= 3);                  /* the mean speed on all three axes */
        CHECK(has(prt, "repeat = true;"));                     /* a speed per particle: Snapmap+'s simulated motion */
        CHECK(has(prt, "boundsExpansion = {"));
        CHECK(has(prt, "z = -400;"));                          /* gravity in the simulated acceleration */
        CHECK(has(prt, "\"PARTICLE_CALC_CURVE_GENERIC\""));       /* the size curve */
        CHECK(has(prt, "\"PARTICLE_CALC_PARAMETRIC_EVAL\""));     /* the colour fade */
        CHECK(!strcmp(kv(prt, "fadeOutFraction", 0), "0.4"));
        CHECK(!strcmp(kv(prt, "numColumns", 0), "4"));
        CHECK(!strcmp(kv(prt, "type", 1), "PANIM_TYPE_CYLE_RATE") || has(prt, "PANIM_TYPE_CYLE_RATE"));
        CHECK(has(prt, "\"POR_AIMED\"") && has(prt, "segmentLength = 10;"));
        CHECK(has(prt, "tableDecls = {"));
        CHECK(count_type("table") >= 1);
        mtr = registered("material", kv(prt, "material", 0));
        CHECK(mtr != NULL);
        CHECK(mtr && has(mtr, "mods/abcdefgh/fx/ember.tga"));
        CHECK(mtr && !has(mtr, "look\t"));                      /* the look compiled */
    }
    CHECK(x.notes && has(x.notes, "300 billboards drawn on the CPU in 2 stages"));
    release(&x);
    sdk_set_register(NULL);
}

static void test_particles_plain(void)
{
    const char *t = "{\n type effect\n particles { material native(\"textures/effects/particles/sparks/spark_single\")\n"
                    " count 5\n fall 100\n bounce 0.4 }\n}\n";
    res x;
    const char *prt;
    g_nregs = 0;
    sdk_set_register(capture);
    x = compile(FX, t);
    CHECK(x.r == SH_LOOK_CHANGED);
    prt = registered("particle", kv(x.out, "declPrt", 0));
    CHECK(prt != NULL);
    CHECK(prt && !strcmp(kv(prt, "material", 0), "textures/effects/particles/sparks/spark_single"));
    CHECK(prt && has(prt, "elasticity = 0.4;"));
    CHECK(prt && has(prt, "fadeColor = {\n\t\t\t\t\t\tx = 2;"));
    CHECK(count_type("material") == 0);
    release(&x);
    sdk_set_register(NULL);
}

static void test_particle_bounds_and_counts(void)
{
    const int totals[] = {170, 171, 341, 20000};
    int i;
    sdk_set_register(capture);
    for (i = 0; i < 4; i++) {
        char source[180];
        const char *particle;
        int seen = 0, sum = 0, n, capped = totals[i] > 16383 ? 16383 : totals[i];
        res x;
        snprintf(source, sizeof source,
                 "{\ntype effect\nparticles { material native(\"test\")\n count %d\n from box {0, 64, 0}\n}\n}", totals[i]);
        g_nregs = 0;
        x = compile(FX, source);
        CHECK(x.r == SH_LOOK_CHANGED);
        particle = registered("particle", kv(x.out, "declPrt", 0));
        CHECK(particle != NULL);
        if (particle) {
            CHECK(has(particle, "x = -76;"));
            while ((n = atoi(kv(particle, "totalParticles", seen))) > 0) {
                CHECK(n <= 170);
                sum += n;
                seen++;
            }
            CHECK(sum == capped);
            CHECK(seen == (capped + 169) / 170);
            CHECK(count(particle, "diversity = ") == seen - 1);
            CHECK(count(particle, "\n\t\t\t\tbounds = {") == seen);
            CHECK(count(particle, "\n\t\tbounds = {") == 1);
            CHECK(count(particle, "x = -76;") == seen + 1);
            CHECK(count(particle, "z = 76;") == seen + 1);
        }
        if (totals[i] > capped) CHECK(x.notes && has(x.notes, "16383 billboards"));
        release(&x);
    }
    sdk_set_register(NULL);
}

static void test_effect_mistakes(void)
{
    static const struct { const char *text, *sentence; } M[] = {
        { "{\n type effect\n lazer { }\n}\n", "blast.decl line 3: 'lazer' is not an item of an effect; write light, decal, shake, flash, screen, blur, focus, flare, trail, model, sound, glow, environment, wind or particles." },
        { "{\n type effect\n lihgt { }\n}\n", "blast.decl line 3: 'lihgt' is not an item of an effect; the nearest is 'light'." },
        { "{\n type effect\n light { brigthness 4 }\n}\n", "blast.decl line 3: 'brigthness' is not a field of a light; the nearest is 'brightness'." },
        { "{\n type effect\n shake {\n strength { 1, 2 }\n }\n}\n", "blast.decl line 4: 'strength' needs one number, as in 'strength 2'." },
        { "{\n type effect\n decal { size 10 }\n}\n", "blast.decl line 3: a decal needs its 'material', as in 'material native(\"textures/decals/blood_splatter/blood_splatter_01\")'." },
        { "{\n type effect\n light { flicker strobe }\n}\n", "blast.decl line 3: 'flicker' is none, flicker or fire; to change the brightness yourself, write 'brightness pulse { ... }'." },
        { "{\n type effect\n particles { image x.tga\n from cube 4 }\n}\n", "blast.decl line 4: 'from' is point, sphere r, ring r, disc r or box { x, y, z }, with 'surface' before it for the surface only." },
        { "{\n type effect\n light { brightness pulse { rate 2, low 1 } }\n}\n", "blast.decl line 3: 'pulse' of 'brightness' needs both 'low' and 'high', as in 'pulse { rate 2, low 0.2, high 1 }'." },
        { "{\n type effect\n}\n", "blast.decl line 1: an effect needs at least one item (light, decal, shake, ...)." },
        { "{\n type effect\n light 4\n}\n", "blast.decl line 3: 'light' takes its fields in braces, as in 'light { ... }'." },
        { "{\n type effect\n light { repeats maybe }\n}\n", "blast.decl line 3: 'repeats' is yes or no." },
        { "{\n type effect\n environment { use native(\"x\")\n fog { 1, 0, 0 } }\n}\n", "blast.decl line 3: an environment item uses one environment or names its values, not both." },
        { "{\n type effect\n environment { fgo { 1, 0, 0 } }\n}\n", "blast.decl line 3: 'fgo' is not a field of an environment item; the nearest is 'fog'." },
    };
    size_t i;
    for (i = 0; i < sizeof M / sizeof M[0]; i++) {
        res x = compile(FX, M[i].text);
        CHECK(x.r == SH_LOOK_MISTAKE);
        if (strcmp(x.why, M[i].sentence)) fprintf(stderr, "  mistake %u: got \"%s\"\n", (unsigned)i, x.why);
        CHECK(!strcmp(x.why, M[i].sentence));
        CHECK(x.out == NULL);
        release(&x);
    }
}

static void test_not_sdk(void)
{
    const char *fx = "{\n\tedit = {\n\t\teditEvents = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n";
    const char *mat = "{\nstageprogram\tbasicadd\ndiffusemap\t\"x.tga\"\n}\n";
    res x = compile(FX, fx);
    CHECK(x.r == SH_LOOK_SAME && !x.out);
    release(&x);
    x = compile(MAT, mat);
    CHECK(x.r == SH_LOOK_SAME && !x.out);
    release(&x);
    x = compile("generated/decls/entitydef/mods/abcdefgh/x.decl", "{ type effect light { } }");
    CHECK(x.r == SH_LOOK_SAME);
    release(&x);
}

static void test_environment(void)
{
    const char *t = "{\n\ttype\tenvironment\n\tfog\t{ 0.35, 0.05, 0.03 }\n\tfogdensity\t0.03\n\tbloom\t1.5\n"
                    "\texposure\t{ 2, 10 }\n\tvignette\tpulse { rate 1, low 0.4, high 0.8 }\n\tblend\t1.5\n}\n";
    res x;
    const char *fx;
    char name[96];
    g_nregs = 0;
    sdk_set_register(capture);
    x = compile(ENV, t);
    CHECK(x.r == SH_LOOK_CHANGED);
    CHECK(x.out && !strncmp(x.out, SH_ENV_MARK " snapmapplus/sdk/", strlen(SH_ENV_MARK) + 17));
    CHECK(x.out && has(x.out, "\n{\n\tinherit {\n\t\tdefault\n\t}\n\trenderParms {\n"));
    CHECK(has(x.out, "\tfogColor\t{ 0.35, 0.05, 0.03 }\n"));
    CHECK(has(x.out, "\tbloomWeight0\t0.3\n") && has(x.out, "\tbloomWeight5\t1.5\n"));
    CHECK(has(x.out, "\tautoExposureMin\t2\n") && has(x.out, "\tautoExposureMax\t10\n"));
    CHECK(has(x.out, "\tenvBlendTime\t1.5\n"));
    CHECK(has(x.out, "sintable[temp2]") && has(x.out, "\tvignetteScale.x\ttemp5 + temp6\n"));
    /* the fx set_environment plays, and its emitter, are needs */
    CHECK(x.needs && !strncmp(x.needs, "fx snapmapplus/sdk/", 19) && has(x.needs, "\nentityDef snapmapplus/effect/snapmapplus/sdk/"));
    sscanf(x.needs, "fx %95s", name);
    fx = registered("fx", name);
    CHECK(fx && has(fx, "\"FX_ENV_CHANGE\"") && has(fx, "looping = true;") && has(fx, "fadeOutTime = 1.5;"));
    CHECK(fx && has(fx, "parm = \"vignettescale\";") && has(fx, "x = 0.6;"));   /* a changing value held at its middle */
    CHECK(x.notes && has(x.notes, "at its middle"));
    {
        char *m = sh_env_moment_fx(ENV, t, strlen(t));
        CHECK(m && !strcmp(m, name));
        free(m);
    }
    release(&x);
    {
        res y = compile(ENV, "{\n\ttype\tenvironment\n\tfgo\t{ 1, 0, 0 }\n}\n");
        CHECK(y.r == SH_LOOK_MISTAKE && !strcmp(y.why, "hell_red.decl line 3: 'fgo' is not a field of an environment; the nearest is 'fog'."));
        release(&y);
    }
    sdk_set_register(NULL);
}

static void test_material_values(void)
{
    const char *t = "{\nstageprogram\tbasicemissive\ntemp3\ttime * temp2\nbloomcolor\tpulse { rate 2, low { 0, 0, 0 }, high { 1, 0.5, 0 } }\n"
                    "color\tcurve { 0 1, 0.5 0, 1 1, loop yes }\n}\n";
    res x;
    g_nregs = 0;
    sdk_set_register(capture);
    x = compile(MAT, t);
    CHECK(x.r == SH_LOOK_CHANGED);
    /* registers start after the material's own temp3 */
    CHECK(has(x.out, "\ttemp4\t2\n\ttemp5\ttime * temp4\n\ttemp6\tsintable[temp5]\n"));
    CHECK(has(x.out, "\tbloomcolor.x\ttemp8 + temp9\n") && has(x.out, "\tbloomcolor.z\t"));
    /* bloomcolor is an sRGB parm: 0.5 decodes to 0.21404114, so its middle is 0.10702057 */
    CHECK(has(x.out, "0.10702057"));
    CHECK(has(x.out, "snapmapplus_sdk_") && count_type("table") == 1);
    CHECK(!has(x.out, "pulse"));
    CHECK(has(x.out, "stageprogram\tbasicemissive\n"));
    release(&x);
    {
        res y = compile(MAT, "{\nbloomcolor\tfade { from 0, to 1, over 2 }\n}\n");
        CHECK(y.r == SH_LOOK_MISTAKE);
        CHECK(has(y.why, "rune.decl line 2: a material has no start"));
        release(&y);
    }
    sdk_set_register(NULL);
}

static void test_wound(void)
{
    const char *t = "{\n\ttype\twound\n\tkind\tbig\n\tparts\t{ chest, left_upper_arm }\n\topens\t200\n\tinonehit\t50\n"
                    "\tshows\twound_chest\n\tcuts\t{ { 0, 4, 60, 12 }, { 2, 0, 70, 8 } }\n\tblood\tboth\n\tat\tgorefx_chest\n"
                    "\tthrows\tmods/abcdefgh/gibs/chest_chunk\n\twhilealive\tyes\n}\n";
    res x = compile("generated/decls/gorewounds/mods/abcdefgh/chest.decl", t);
    CHECK(x.r == SH_LOOK_CHANGED);
    CHECK(x.out && !strncmp(x.out, "{\n\tinherit = \"wounds/default\";", 30));
    CHECK(!strcmp(kv(x.out, "woundType", 0), "GORETYPE_BIG_WOUND"));
    CHECK(!strcmp(kv(x.out, "woundFlags", 0), "WOUND_FLAGS_ALLOW_WHEN_ALIVE"));
    CHECK(!strcmp(kv(x.out, "item[0]", 0), "wound_chest"));
    CHECK(has(x.out, "clippingSpheres = {\n\t\t\t\tnum = 2;"));
    CHECK(!strcmp(kv(x.out, "sprayFX", 0), "FX_GORE_SPRAY_1"));
    CHECK(has(x.out, "decalMat = \"textures/decals/blood_splatter/blood_splatter_small_01\";"));
    CHECK(has(x.out, "decalMat = \"textures/decals/blood_splatter/blood_pool_01\";"));
    CHECK(!strcmp(kv(x.out, "entityDef", 0), "mods/abcdefgh/gibs/chest_chunk"));
    CHECK(has(x.out, "damageGroups = {\n\t\t\t\tnum = 2;\n\t\t\t\titem[0] = \"chest\";\n\t\t\t\titem[1] = \"left_upper_arm\";"));
    CHECK(!strcmp(kv(x.out, "totalDamageRequiredToGore", 0), "200") && !strcmp(kv(x.out, "frameDamageRequiredToGore", 0), "50"));
    release(&x);
    x = compile("generated/decls/gorewounds/mods/abcdefgh/chest.decl", "{\n\ttype\twound\n\tparts\tchest\n\tblod\tspray\n}\n");
    CHECK(x.r == SH_LOOK_MISTAKE && !strcmp(x.why, "chest.decl line 4: 'blod' is not a field of a wound; the nearest is 'blood'."));
    release(&x);
    x = compile("generated/decls/gorewounds/mods/abcdefgh/chest.decl", "{\n\ttype\twound\n\tparts\tchest\n}\n");
    CHECK(x.r == SH_LOOK_MISTAKE && has(x.why, "needs the joint its blood comes from"));
    release(&x);
}

static void test_emitter_served(void)
{
    unsigned char *b = NULL;
    size_t n = 0;
    char *em = sh_modprog_effect_emitter("mods/abcdefgh/blast");
    CHECK(em && !strcmp(em, "snapmapplus/effect/mods/abcdefgh/blast"));
    free(em);
    CHECK(sh_modprog_owns("generated/decls/entitydef/snapmapplus/effect/mods/abcdefgh/blast.decl"));
    CHECK(sh_modprog_read("generated/decls/entitydef/snapmapplus/effect/mods/abcdefgh/blast.decl", &b, &n) == SH_MB_STOCK_OK);
    CHECK(b && n && !memcmp(b, "{\n\tinherit = \"func/fx\";\n\tedit = {\n\t\tfxEffect = \"mods/abcdefgh/blast\";\n\t}\n}\n", n));
    if (b) HeapFree(GetProcessHeap(), 0, b);
    {
        const char *name = sh_modprog_register_decl("Table", "{ { 0, 1 } }\n");
        char path[160];
        CHECK(name && !strncmp(name, "snapmapplus_sdk_", 16) && strlen(name) == 32);
        snprintf(path, sizeof path, "generated/decls/table/%s.decl", name);
        CHECK(sh_modprog_owns(path));
        b = NULL;
        CHECK(sh_modprog_read(path, &b, &n) == SH_MB_STOCK_OK && n == 13 && !memcmp(b, "{ { 0, 1 } }\n", 13));
        if (b) HeapFree(GetProcessHeap(), 0, b);
        CHECK(!sh_modprog_owns("generated/decls/table/snapmapplus_sdk_0000000000000000.decl"));
    }
    {
        const char *a = sh_modprog_register_decl("particle", "{ x }"), *c = sh_modprog_register_decl("particle", "{ x }");
        CHECK(a && c && a == c && !strncmp(a, "snapmapplus/sdk/", 16));
    }
}

int main(void)
{
    test_parse();
    test_effect_items();
    test_effect_more_items();
    test_effect_environment_item();
    test_effect_light_values();
    test_particles();
    test_particles_plain();
    test_particle_bounds_and_counts();
    test_effect_mistakes();
    test_not_sdk();
    test_environment();
    test_material_values();
    test_wound();
    test_emitter_served();
    printf("sdk_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
