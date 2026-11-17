/* parmslots_test.c -- where a render parm asking for a parm state slot goes (parmslots_rule.c): the engine's
 * ranges, the engine placing every parm it can, the product's vector parms keeping their slots whatever asks first,
 * the shared slot once the vector range is at its end, a full range of another kind. The table here is the game's
 * as measured (973 vector parms that are not the product's) with the product's 32. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parmslots.h"

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

typedef struct holder { const char *name; int kind; } holder;
static holder g_table[SH_PARMSLOTS_COUNT];
static char g_names[SH_PARMSLOTS_COUNT + 64][16];
static const char *g_reserved[32];
static int g_spill;
static const char TEX[] = "tex";

static int read_slot(void *context, int slot, const char **name, int *kind)
{
    (void)context;
    if (!g_table[slot].name) return 0;
    *name = g_table[slot].name;
    *kind = g_table[slot].kind;
    return 1;
}

static void reset(int stock_vectors)
{
    int i;
    memset(g_table, 0, sizeof g_table);
    g_spill = -1;
    for (i = 0; i < stock_vectors; i++) {
        sprintf(g_names[i], "stock%d", i);
        g_table[i].name = g_names[i];
        g_table[i].kind = 0;
    }
}

/* What the detour does with a decision: the engine's own rule when it is the engine's, else the shared slot. */
static int ask(int kind, const char *name)
{
    int becomes = 0, lo, hi, s;
    int slot = sh_parmslots_decide(kind, name, read_slot, NULL, g_reserved, 32, g_spill, &becomes);
    if (slot != SH_PARMSLOTS_ENGINE) return slot;
    sh_parmslots_range(kind, &lo, &hi);
    for (s = lo; s < hi; s++)
        if (g_table[s].name == name) return s;
    for (s = lo; s < hi; s++)
        if (!g_table[s].name) {
            g_table[s].name = name;
            g_table[s].kind = kind;
            if (becomes) g_spill = s;
            return s;
        }
    return -2;                                   /* the engine's refusal: must never be reached */
}

static int distinct_own(int *slots, int n)
{
    int i, j;
    for (i = 0; i < n; i++) {
        if (slots[i] < 0 || slots[i] >= 0x400 || slots[i] == g_spill) return 0;
        for (j = 0; j < i; j++) if (slots[j] == slots[i]) return 0;
    }
    return 1;
}

int main(void)
{
    static char reserved_names[32][16], foreign[64][16];
    int lo, hi, i, own[32], f[64];
    printf("parmslots_test\n");
    for (i = 0; i < 32; i++) { sprintf(reserved_names[i], "smp%d", i); g_reserved[i] = reserved_names[i]; }
    for (i = 0; i < 64; i++) sprintf(foreign[i], "mod%d", i);

    printf("-- the ranges are AllocParmState's\n");
    CHECK(sh_parmslots_range(0, &lo, &hi) && lo == 0 && hi == 0x400);
    CHECK(sh_parmslots_range(1, &lo, &hi) && lo == 0x400 && hi == 0x600);
    CHECK(sh_parmslots_range(9, &lo, &hi) && lo == 0x400 && hi == 0x600);
    CHECK(sh_parmslots_range(10, &lo, &hi) && lo == 0x600 && hi == 0x680);
    CHECK(sh_parmslots_range(11, &lo, &hi) && lo == 0x680 && hi == 0x800);
    CHECK(sh_parmslots_range(12, &lo, &hi) && lo == 0x800 && hi == 0x880);
    CHECK(sh_parmslots_range(14, &lo, &hi) && lo == 0x800 && hi == 0x880);
    CHECK(sh_parmslots_range(13, &lo, &hi) && lo == 0x880 && hi == 0x8C0);
    CHECK(sh_parmslots_range(15, &lo, &hi) && lo == 0x8C0 && hi == 0x900);
    CHECK(sh_parmslots_range(18, &lo, &hi) && lo == 0x900 && hi == 0x93F);
    CHECK(!sh_parmslots_range(19, &lo, &hi) && !sh_parmslots_range(-1, &lo, &hi));

    printf("-- with room, every parm is the engine's, and a name that holds a slot keeps it\n");
    reset(100);
    CHECK(sh_parmslots_decide(0, foreign[0], read_slot, NULL, g_reserved, 32, -1, NULL) == SH_PARMSLOTS_ENGINE);
    CHECK(ask(0, foreign[0]) == 100);
    CHECK(ask(0, foreign[0]) == 100);
    CHECK(ask(3, "atexture") == 0x400);
    CHECK(sh_parmslots_decide(99, "unknownkind", read_slot, NULL, g_reserved, 32, -1, NULL) == SH_PARMSLOTS_ENGINE);

    printf("-- today's game: the product's 32 first, then a mod's parms: eighteen slots of their own, then one shared\n");
    reset(973);
    for (i = 0; i < 32; i++) own[i] = ask(0, g_reserved[i]);
    CHECK(distinct_own(own, 32));
    for (i = 0; i < 30; i++) f[i] = ask(0, foreign[i]);
    CHECK(f[0] == 1005 && f[1] == 1006 && f[17] == 1022 && f[18] == 1023);
    CHECK(g_spill == 1023);
    for (i = 18; i < 30; i++) CHECK(f[i] == 1023);
    CHECK(ask(0, foreign[18]) == 1023 && ask(0, foreign[29]) == 1023);     /* asked again: the same slots */
    CHECK(ask(0, foreign[0]) == 1005);

    printf("-- a mod's parms first: the product's 32 still get a slot each\n");
    reset(973);
    for (i = 0; i < 30; i++) f[i] = ask(0, foreign[i]);
    CHECK(f[0] == 973 && f[1] == 974 && f[18] == 991 && g_spill == 991);
    for (i = 18; i < 30; i++) CHECK(f[i] == 991);
    for (i = 0; i < 32; i++) own[i] = ask(0, g_reserved[i]);
    CHECK(distinct_own(own, 32));
    for (i = 0; i < 32; i++) CHECK(ask(0, g_reserved[i]) == own[i]);

    printf("-- interleaved: a mod's parm between each of the product's\n");
    reset(973);
    for (i = 0; i < 32; i++) { own[i] = ask(0, g_reserved[i]); f[i] = ask(0, foreign[i]); }
    CHECK(distinct_own(own, 32));
    for (i = 0; i < 32; i++) CHECK(f[i] >= 0 && f[i] < 0x400);
    CHECK(g_spill >= 0 && f[31] == g_spill);

    printf("-- no room at all: nothing is refused\n");
    reset(1024);
    CHECK(ask(0, foreign[0]) == 0x3FF);                    /* the range's last slot, with no shared slot made */
    CHECK(ask(0, g_reserved[0]) == 0x3FF);
    reset(1023);                                           /* one left and 32 reserved still missing */
    CHECK(ask(0, foreign[0]) == 1023 && g_spill == 1023);
    CHECK(ask(0, foreign[1]) == 1023);
    CHECK(ask(0, g_reserved[0]) == 1023);                  /* the product's own shares it too */

    printf("-- a full range of another kind: the last parm of the same kind\n");
    reset(0);
    for (i = 0x400; i < 0x600; i++) { g_table[i].name = TEX; g_table[i].kind = (i == 0x4F0) ? 7 : 3; }
    CHECK(ask(3, "another2d") == 0x5FF);
    CHECK(ask(7, "anothercube") == 0x4F0);
    CHECK(ask(5, "nokindlikeit") == 0x5FF);
    CHECK(ask(3, TEX) == 0x400);                           /* the same name pointer: the engine's reuse */

    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
