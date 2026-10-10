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

/* The player's hands web: whole-copy contributions aligned, then composed. */
static char *hands(const char *name, const char *baseline, const char **texts, size_t count,
    sh_decl_animweb_move **moves, size_t *move_count)
{
    sh_decl_source sources[4];
    char *owned[4], error[512] = "", *out = NULL;
    size_t length = 0;
    for (size_t i = 0; i < count; i++) sources[i] = src(texts[i]);
    if (!sh_decl_animweb_prepare(src(baseline), sources, count, owned, moves, move_count, error, sizeof(error))) {
        report(name, error[0] ? error : "prepare refused"); return NULL;
    }
    out = sh_decl_block_compose("animweb", src(baseline), sources, count, &length, error, sizeof(error), NULL);
    for (size_t i = 0; i < count; i++) free(owned[i]);
    if (!out) report(name, error[0] ? error : "compose refused");
    return out;
}
static void holds(const char *name, const char *out, const char *needle, int present)
{
    if (!out || (strstr(out, needle) != NULL) != present) {
        char detail[900];
        snprintf(detail, sizeof(detail), "%s '%s' in:\n%s", present ? "missing" : "unexpected", needle, out ? out : "(none)");
        report(name, detail);
    }
}
static size_t occurrences(const char *out, const char *needle)
{
    size_t count = 0;
    for (const char *at = out; at && (at = strstr(at, needle)); at += strlen(needle)) count++;
    return count;
}

/* Edges in subweb `from`'s block of out that enter subweb `to`. */
static size_t routes(const char *out, const char *from, const char *to)
{
    char head[160], edge[160];
    snprintf(head, sizeof(head), "subWeb \"%s\" {", from);
    snprintf(edge, sizeof(edge), "toSubWeb \"%s\"", to);
    const char *at = out ? strstr(out, head) : NULL, *end;
    if (!at) return SIZE_MAX;
    int depth = 0;
    for (end = strchr(at, '{'); *end; end++) {
        if (*end == '{') depth++;
        else if (*end == '}' && --depth == 0) break;
    }
    size_t count = 0;
    for (const char *p = at; (p = strstr(p, edge)) && p < end; p += strlen(edge)) count++;
    return count;
}

#define AW_STOCK \
    "{ props { editPos ( 1 2 3 ) modelInfos { \"hands\" \"\" } }\n" \
    "states { state \"up\" { } state \"down\" { } }\n" \
    "subWebs {\n" \
    "subWeb \"rifle\" { node \"down\" { edges {\n" \
    "edge { toState \"up\" toSubWeb \"pistol\" blendParms { duration 3 } } } }\n" \
    "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"rifle_up\" } } } } } }\n" \
    "subWeb \"pistol\" { node \"down\" { edges {\n" \
    "edge { toState \"up\" toSubWeb \"rifle\" blendParms { duration 3 } } } }\n" \
    "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"pistol_up\" } } } } } } } }"

/* A weapon package's whole copy: the stock web, its own model and subweb,
 * and glue edges from the stock subwebs into it. */
#define AW_WEAPON(view, model, sub, anim) \
    "{ props { editPos " view " modelInfos { \"hands\" \"\" \"" model "\" \"\" } }\n" \
    "states { state \"up\" { } state \"down\" { } }\n" \
    "subWebs {\n" \
    "subWeb \"rifle\" { node \"down\" { edges {\n" \
    "edge { toState \"up\" toSubWeb \"pistol\" blendParms { duration 3 } }\n" \
    "edge { toState \"up\" toSubWeb \"" sub "\" blendParms { duration 3 } } } }\n" \
    "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"rifle_up\" } } } } } }\n" \
    "subWeb \"pistol\" { node \"down\" { edges {\n" \
    "edge { toState \"up\" toSubWeb \"rifle\" blendParms { duration 3 } }\n" \
    "edge { toState \"up\" toSubWeb \"" sub "\" blendParms { duration 3 } } } }\n" \
    "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"pistol_up\" } } } } } }\n" \
    "subWeb \"" sub "\" { node \"down\" { edges {\n" \
    "edge { toState \"up\" toSubWeb \"rifle\" blendParms { duration 3 } } } }\n" \
    "node \"up\" { blendTrees { tree { modelIndex 1 anims { alias { name \"" anim "\" } } } } } } } }"

static void test_hands_web(void)
{
    sh_decl_animweb_move *moves = NULL; size_t move_count = 0;
    /* Disjoint weapons: both subwebs, both glue edges in every stock node,
     * one model list with each weapon's index following its model, and the
     * authors' editor views never collide. */
    {
        const char *texts[] = {AW_WEAPON("( 9 9 9 )", "sp", "sp_pistol", "sp_up"),
            AW_WEAPON("( 7 7 7 )", "plasma", "plasma", "plasma_up")};
        char *out = hands("hands disjoint", AW_STOCK, texts, 2, &moves, &move_count);
        holds("hands disjoint", out, "subWeb \"sp_pistol\"", 1);
        holds("hands disjoint", out, "subWeb \"plasma\"", 1);
        holds("hands disjoint", out, "\"sp\" \"\"", 1);
        holds("hands disjoint", out, "\"plasma\" \"\"", 1);
        holds("hands disjoint", out, "editPos ( 1 2 3 )", 1);
        /* Each package wrote its weapon's switches among its own copy's
         * subwebs; the two new weapons also switch to each other, while the
         * restriction both wrote (neither leaves for the stock pistol) stays. */
        if (out && occurrences(out, "toSubWeb \"sp_pistol\"") != 3) report("hands disjoint", "sp_pistol glue edges");
        if (out && occurrences(out, "toSubWeb \"plasma\"") != 3) report("hands disjoint", "plasma glue edges");
        if (routes(out, "sp_pistol", "plasma") != 1 || routes(out, "plasma", "sp_pistol") != 1)
            report("hands disjoint", "the new weapons need one switch to each other");
        if (routes(out, "sp_pistol", "pistol") || routes(out, "plasma", "pistol"))
            report("hands disjoint", "a restriction both authors wrote must stay");
        /* plasma's model is third in the union; its node follows it. */
        const char *plasma = out ? strstr(out, "subWeb \"plasma\"") : NULL;
        if (!plasma || !strstr(plasma, "modelIndex 2") || strstr(out, "subWeb \"sp_pistol\"") > plasma) {
            const char *sp = out ? strstr(out, "subWeb \"sp_pistol\"") : NULL;
            if (!plasma || !strstr(plasma, "modelIndex 2") || (sp && strstr(sp, "modelIndex 1") == NULL))
                report("hands disjoint", "model indices do not follow their models");
        }
        if (move_count) report("hands disjoint", "disjoint weapons must not move");
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
    }
    /* Exact-equal weapons compose once. */
    {
        const char *texts[] = {AW_WEAPON("( 9 9 9 )", "sp", "sp_pistol", "sp_up"),
            AW_WEAPON("( 5 5 5 )", "sp", "sp_pistol", "sp_up")};
        char *out = hands("hands equal", AW_STOCK, texts, 2, &moves, &move_count);
        if (out && occurrences(out, "subWeb \"sp_pistol") != 1) report("hands equal", "equal subwebs must compose once");
        if (move_count) report("hands equal", "equal weapons must not move");
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
    }
    /* Same subweb name, different behavior: the later weapon gets its own
     * copy, entered from every stock subweb, and neither inherits the other. */
    {
        const char *texts[] = {AW_WEAPON("( 9 9 9 )", "sp", "sp_pistol", "sp_up"),
            AW_WEAPON("( 9 9 9 )", "sp2", "sp_pistol", "sp2_up")};
        char *out = hands("hands variants", AW_STOCK, texts, 2, &moves, &move_count);
        if (move_count != 1 || !moves || moves[0].source != 1 || strcmp(moves[0].from, "sp_pistol") ||
            strncmp(moves[0].to, "sp_pistol_", 10)) report("hands variants", "the later weapon's subweb must move");
        else {
            char block[128], edge[128];
            snprintf(block, sizeof(block), "subWeb \"%s\"", moves[0].to);
            snprintf(edge, sizeof(edge), "toSubWeb \"%s\"", moves[0].to);
            holds("hands variants", out, block, 1);
            if (out && occurrences(out, edge) != 3) report("hands variants", "the moved copy needs its entering twins");
            if (routes(out, "sp_pistol", moves[0].to) != 1 || routes(out, moves[0].to, "sp_pistol") != 1)
                report("hands variants", "owner copies need one switch to each other");
            const char *mine = out ? strstr(out, block) : NULL, *first = out ? strstr(out, "subWeb \"sp_pistol\"") : NULL;
            if (!mine || !first || !strstr(mine, "sp2_up") || strstr(first, "sp_up") == NULL ||
                (strstr(first, "sp2_up") && strstr(first, "sp2_up") < mine && first < mine))
                report("hands variants", "each copy must keep its own animation");
        }
        if (out && occurrences(out, "toSubWeb \"sp_pistol\"") != 3) report("hands variants", "original glue edges");
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
    }
    /* Two later weapons with one equal version share one copy of it. */
    {
        const char *texts[] = {AW_WEAPON("( 9 9 9 )", "sp", "sp_pistol", "sp_up"),
            AW_WEAPON("( 9 9 9 )", "sp2", "sp_pistol", "sp2_up"),
            AW_WEAPON("( 4 4 4 )", "sp2", "sp_pistol", "sp2_up")};
        char *out = hands("hands shared copy", AW_STOCK, texts, 3, &moves, &move_count);
        if (move_count != 2 || !moves || strcmp(moves[0].to, moves[1].to)) report("hands shared copy", "equal versions must share a name");
        if (out && occurrences(out, "subWeb \"sp_pistol") != 2) report("hands shared copy", "one original and one shared copy");
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
    }
    /* A weapon that changes a stock subweb's behavior owns its copy; the
     * stock subweb stays stock but keeps the weapon's glue into its subweb. */
    {
        const char *texts[] = {
            "{ props { editPos ( 1 2 3 ) modelInfos { \"hands\" \"\" \"sp\" \"\" } }\n"
            "states { state \"up\" { } state \"down\" { } }\n"
            "subWebs {\n"
            "subWeb \"rifle\" { node \"down\" { edges {\n"
            "edge { toState \"up\" toSubWeb \"pistol\" blendParms { duration 3 } }\n"
            "edge { toState \"up\" toSubWeb \"sp_pistol\" blendParms { duration 3 } } } }\n"
            "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"rifle_fast\" } } } } } }\n"
            "subWeb \"sp_pistol\" { node \"down\" { edges {\n"
            "edge { toState \"up\" toSubWeb \"rifle\" blendParms { duration 3 } } } }\n"
            "node \"up\" { blendTrees { tree { modelIndex 1 anims { alias { name \"sp_up\" } } } } } } } }"};
        char *out = hands("hands stock change", AW_STOCK, texts, 1, &moves, &move_count);
        if (move_count != 1 || !moves || strcmp(moves[0].from, "rifle")) report("hands stock change", "the changed stock subweb must move");
        holds("hands stock change", out, "rifle_up", 1);
        holds("hands stock change", out, "rifle_fast", 1);
        /* The stock pistol subweb, deleted by this copy, stays for the others. */
        holds("hands stock change", out, "subWeb \"pistol\"", 1);
        holds("hands stock change", out, "pistol_up", 1);
        /* Stock rifle and the weapon's copy both enter sp_pistol. */
        if (out && occurrences(out, "toSubWeb \"sp_pistol\"") != 2) report("hands stock change", "stock rifle keeps the weapon's glue");
        /* Stock subwebs keep their order: rifle stays first, then pistol. */
        const char *first = out ? strstr(out, "subWeb \"") : NULL, *pistol = out ? strstr(out, "subWeb \"pistol\"") : NULL;
        if (!first || strncmp(first, "subWeb \"rifle\" {", 16) || !pistol || strstr(out, "subWeb \"rifle\" {") > pistol)
            report("hands stock change", "stock subwebs must keep their order");
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
    }
    /* M47's three owners of one weapon subweb: the kept one and two copies
     * each switch to the other two, in every discovery order; exact copies
     * still share one. */
    {
        const char *a = AW_WEAPON("( 9 9 9 )", "sp", "sp_pistol", "sp_up"),
            *b = AW_WEAPON("( 9 9 9 )", "sp2", "sp_pistol", "sp2_up"),
            *c = AW_WEAPON("( 9 9 9 )", "sp3", "sp_pistol", "sp3_up");
        const char *orders[][4] = {{a, b, c, b}, {c, a, b, a}, {b, c, a, c}};
        for (size_t o = 0; o < 3; o++) {
            char *out = hands("hands owner switches", AW_STOCK, orders[o], 4, &moves, &move_count);
            const char *names[3] = {"sp_pistol", NULL, NULL};
            size_t named = 1;
            for (size_t m = 0; m < move_count && out; m++) {
                int known = 0;
                for (size_t k = 1; k < named; k++) known |= !strcmp(names[k], moves[m].to);
                if (!known && named < 3) names[named++] = moves[m].to;
            }
            if (!out || move_count != 3 || named != 3) report("hands owner switches", "two copies, the duplicate sharing one");
            else for (size_t i = 0; i < 3; i++) {
                if (routes(out, names[i], names[i]) != 0) report("hands owner switches", "a copy must not enter itself");
                if (routes(out, names[i], "pistol") != 0) report("hands owner switches", "a written restriction must stay");
                if (routes(out, "rifle", names[i]) != 1 || routes(out, "pistol", names[i]) != 1)
                    report("hands owner switches", "every stock weapon enters every copy once");
                for (size_t j = 0; j < 3; j++)
                    if (i != j && routes(out, names[i], names[j]) != 1) {
                        char detail[256];
                        snprintf(detail, sizeof(detail), "order %zu: no single switch from %s to %s", o, names[i], names[j]);
                        report("hands owner switches", detail);
                    }
            }
            sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
        }
    }
    /* A weapon changing a universal stock weapon: the restored stock subweb
     * and the owner's copy switch to each other. */
    {
        #define AW_THREE(rifle_anim) \
            "{ props { editPos ( 1 2 3 ) modelInfos { \"hands\" \"\" } }\n" \
            "subWebs {\n" \
            "subWeb \"rifle\" { node \"down\" { edges {\n" \
            "edge { toState \"up\" toSubWeb \"pistol\" } edge { toState \"up\" toSubWeb \"shotgun\" } } }\n" \
            "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"" rifle_anim "\" } } } } } }\n" \
            "subWeb \"pistol\" { node \"down\" { edges {\n" \
            "edge { toState \"up\" toSubWeb \"rifle\" } edge { toState \"up\" toSubWeb \"shotgun\" } } }\n" \
            "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"pistol_up\" } } } } } }\n" \
            "subWeb \"shotgun\" { node \"down\" { edges {\n" \
            "edge { toState \"up\" toSubWeb \"rifle\" } edge { toState \"up\" toSubWeb \"pistol\" } } }\n" \
            "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"shotgun_up\" } } } } } } } }"
        const char *texts[] = {AW_THREE("rifle_fast")};
        char *out = hands("hands stock copy switches", AW_THREE("rifle_up"), texts, 1, &moves, &move_count);
        if (move_count != 1 || !moves || strcmp(moves[0].from, "rifle")) report("hands stock copy switches", "the changed rifle must move");
        else {
            if (routes(out, "rifle", moves[0].to) != 1 || routes(out, moves[0].to, "rifle") != 1)
                report("hands stock copy switches", "stock and copy need one switch to each other");
            if (routes(out, "pistol", moves[0].to) != 1 || routes(out, "shotgun", moves[0].to) != 1)
                report("hands stock copy switches", "every stock weapon enters the copy");
        }
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
        #undef AW_THREE
    }
    /* Three universal stock weapons; a package's weapon "sp" and its entering
     * edges come in through the extra arguments. */
    #define AW_TRIO(pistol_anim, into_rifle, into_pistol, into_shotgun, more) \
        "{ props { editPos ( 1 2 3 ) modelInfos { \"hands\" \"\" } }\n" \
        "subWebs {\n" \
        "subWeb \"rifle\" { node \"down\" { edges {\n" \
        "edge { toState \"up\" toSubWeb \"pistol\" } edge { toState \"up\" toSubWeb \"shotgun\" } " into_rifle " } }\n" \
        "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"rifle_up\" } } } } } }\n" \
        "subWeb \"pistol\" { node \"down\" { edges {\n" \
        "edge { toState \"up\" toSubWeb \"rifle\" } edge { toState \"up\" toSubWeb \"shotgun\" } " into_pistol " } }\n" \
        "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"" pistol_anim "\" } } } } } }\n" \
        "subWeb \"shotgun\" { node \"down\" { edges {\n" \
        "edge { toState \"up\" toSubWeb \"rifle\" } edge { toState \"up\" toSubWeb \"pistol\" } " into_shotgun " } }\n" \
        "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"shotgun_up\" } } } } } }\n" \
        more "} }"
    #define AW_SP(anim, exits) \
        "subWeb \"sp\" { node \"down\" { edges { " exits " } }\n" \
        "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"" anim "\" } } } } } }\n"
    #define AW_INTO_SP(parms) "edge { toState \"up\" toSubWeb \"sp\" " parms " }"
    /* A source's omission survives another package's copy of the omitted
     * weapon: "sp" leaves for rifle and shotgun but never the pistol, and no
     * contribution writes "sp" with the changed pistol's copy, so universal
     * rifle and shotgun do not authorize a route from "sp" to it. */
    {
        const char *texts[] = {
            AW_TRIO("pistol_up", AW_INTO_SP(""), AW_INTO_SP(""), AW_INTO_SP(""),
                AW_SP("sp_up", "edge { toState \"up\" toSubWeb \"rifle\" } edge { toState \"up\" toSubWeb \"shotgun\" }")),
            AW_TRIO("pistol_fast", "", "", "", "")};
        char *out = hands("hands omitted destination copy", AW_TRIO("pistol_up", "", "", "", ""), texts, 2, &moves, &move_count);
        if (move_count != 1 || !moves || moves[0].source != 1 || strcmp(moves[0].from, "pistol"))
            report("hands omitted destination copy", "the changed pistol must move");
        else {
            if (routes(out, "sp", moves[0].to) != 0) report("hands omitted destination copy", "an omitted destination's copy gained a route");
            if (routes(out, "sp", "pistol") != 0) report("hands omitted destination copy", "the written omission must stay");
            if (routes(out, "rifle", moves[0].to) != 1) report("hands omitted destination copy", "stock rifle enters the copy");
        }
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
    }
    /* Sources that leave exactly as the other owner's "sp" does enter "sp"
     * with different blends (3, 3, 9): no edge is chosen for its author. */
    {
        #define AW_PAYLOAD_OWNER(anim) \
            AW_TRIO("pistol_up", AW_INTO_SP("blendParms { duration 3 }"), AW_INTO_SP("blendParms { duration 3 }"), \
                AW_INTO_SP("blendParms { duration 9 }"), AW_SP(anim, "edge { toState \"up\" toSubWeb \"rifle\" } " \
                "edge { toState \"up\" toSubWeb \"pistol\" } edge { toState \"up\" toSubWeb \"shotgun\" }"))
        const char *texts[] = {AW_PAYLOAD_OWNER("sp_up"), AW_PAYLOAD_OWNER("sp2_up")};
        char *out = hands("hands differing payload", AW_TRIO("pistol_up", "", "", "", ""), texts, 2, &moves, &move_count);
        if (!out || move_count != 1 || !moves || strcmp(moves[0].from, "sp")) report("hands differing payload", "the second owner's copy");
        else {
            if (routes(out, "sp", moves[0].to) != 0 || routes(out, moves[0].to, "sp") != 0)
                report("hands differing payload", "unequal entering edges must not yield a route");
            if (routes(out, "shotgun", moves[0].to) != 1) report("hands differing payload", "stock shotgun enters the copy");
        }
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
        #undef AW_PAYLOAD_OWNER
    }
    #define AW_EXITS "edge { toState \"up\" toSubWeb \"rifle\" } edge { toState \"up\" toSubWeb \"pistol\" } " \
        "edge { toState \"up\" toSubWeb \"shotgun\" }"
    /* A new weapon leaving with `exits` and entering "sp" with `into`. */
    #define AW_PLASMA(exits, into) "subWeb \"plasma\" { node \"down\" { edges { " exits " " into " } }\n" \
        "node \"up\" { blendTrees { tree { modelIndex 0 anims { alias { name \"plasma_up\" } } } } } }\n"
    /* A mirror's omission vetoes:the second owner's shotgun leaves exactly
     * as either "sp" does but never enters its own "sp", so the first owner's
     * "sp" gains no route into that copy; the first web's sources all enter
     * its "sp", so the route back is derived. */
    {
        const char *texts[] = {
            AW_TRIO("pistol_up", AW_INTO_SP(""), AW_INTO_SP(""), AW_INTO_SP(""), AW_SP("sp_up", AW_EXITS)),
            AW_TRIO("pistol_up", AW_INTO_SP(""), AW_INTO_SP(""), "", AW_SP("sp2_up", AW_EXITS))};
        char *out = hands("hands mirror omission", AW_TRIO("pistol_up", "", "", "", ""), texts, 2, &moves, &move_count);
        if (!out || move_count != 1 || !moves || strcmp(moves[0].from, "sp")) report("hands mirror omission", "the second owner's copy");
        else {
            if (routes(out, "sp", moves[0].to) != 0) report("hands mirror omission", "a mirror's omission must veto the route");
            if (routes(out, moves[0].to, "sp") != 1) report("hands mirror omission", "the unanimous route back must stay");
        }
        sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
    }
    /* Repeated edges into one subweb are a family of weighted alternatives:
     * an agreed family is copied whole and in order, a mirror whose family
     * differs only in a later alternative derives nothing, and a source that
     * leaves with another family (one more alternative) is no mirror. */
    {
        #define AW_PAIR(second) AW_INTO_SP("weightScale 1") " " AW_INTO_SP(second)
        #define AW_TWICE(to) "edge { toState \"up\" toSubWeb \"" to "\" } edge { toState \"up\" toSubWeb \"" to "\" weightScale 2 } "
        static const char *cases[][2] = {
            {AW_TRIO("pistol_up", AW_PAIR("weightScale 3"), AW_PAIR("weightScale 3"), AW_PAIR("weightScale 3"), AW_SP("sp_up", AW_EXITS)),
             AW_TRIO("pistol_up", AW_PAIR("weightScale 3"), AW_PAIR("weightScale 3"), AW_PAIR("weightScale 3"), AW_SP("sp2_up", AW_EXITS))},
            {AW_TRIO("pistol_up", AW_PAIR("weightScale 3"), AW_PAIR("weightScale 3"), AW_PAIR("weightScale 3"), AW_SP("sp_up", AW_EXITS)),
             AW_TRIO("pistol_up", AW_PAIR("weightScale 3"), AW_PAIR("weightScale 3"), AW_PAIR("weightScale 3"),
                AW_SP("sp2_up", AW_EXITS) AW_PLASMA(AW_EXITS, AW_PAIR("weightScale 5")))},
            {AW_TRIO("pistol_up", AW_INTO_SP(""), AW_INTO_SP(""), AW_INTO_SP(""), AW_SP("sp_up", AW_EXITS)),
             AW_TRIO("pistol_up", AW_INTO_SP(""), AW_INTO_SP(""), AW_INTO_SP(""), AW_SP("sp2_up", AW_EXITS)
                AW_PLASMA(AW_TWICE("rifle") AW_TWICE("pistol") AW_TWICE("shotgun"), AW_INTO_SP("weightScale 7")))}};
        static const size_t forward[] = {2, 0, 1}, back[] = {2, 2, 1};
        static const char *names[] = {"hands edge family", "hands edge family later alternative", "hands exit family"};
        for (size_t c = 0; c < 3; c++) {
            char *out = hands(names[c], AW_TRIO("pistol_up", "", "", "", ""), cases[c], 2, &moves, &move_count);
            if (!out || move_count != 1 || !moves || strcmp(moves[0].from, "sp")) report(names[c], "the second owner's copy");
            else {
                if (routes(out, "sp", moves[0].to) != forward[c]) report(names[c], "route into the copy");
                if (routes(out, moves[0].to, "sp") != back[c]) report(names[c], "route back from the copy");
                const char *sp = strstr(out, "subWeb \"sp\" {"), *one = sp ? strstr(sp, "weightScale 1") : NULL,
                    *three = sp ? strstr(sp, "weightScale 3") : NULL;
                if (c == 0 && (!one || !three || one > three)) report(names[c], "the copied family must keep its order");
            }
            sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
        }
        #undef AW_TWICE
        #undef AW_PAIR
    }
    /* Every writer of a name speaks for it, in any discovery order: two
     * packages write one "sp", and in one of them a "plasma" leaving exactly
     * as the third owner's copy does enters "sp" with another blend, so the
     * copy gains no route to "sp"; with an equal blend it gains one. */
    {
        #define AW_OWNER(anim, more) AW_TRIO("pistol_up", AW_INTO_SP("blendParms { duration 3 }"), \
            AW_INTO_SP("blendParms { duration 3 }"), AW_INTO_SP("blendParms { duration 3 }"), AW_SP(anim, AW_EXITS) more)
        static const char *x = AW_OWNER("sp_up", ""), *y = AW_OWNER("sp2_up", ""),
            *plasma[] = {AW_OWNER("sp_up", AW_PLASMA(AW_EXITS, AW_INTO_SP("blendParms { duration 9 }"))),
                AW_OWNER("sp_up", AW_PLASMA(AW_EXITS, AW_INTO_SP("blendParms { duration 3 }")))};
        for (size_t equal = 0; equal < 2; equal++)
            for (size_t o = 0; o < 2; o++) {
                const char *texts[] = {o ? plasma[equal] : x, o ? x : plasma[equal], y};
                char detail[160];
                char *out = hands("hands writer authority", AW_TRIO("pistol_up", "", "", "", ""), texts, 3, &moves, &move_count);
                if (!out || move_count != 1 || !moves || moves[0].source != 2) report("hands writer authority", "the third owner's copy");
                else {
                    snprintf(detail, sizeof(detail), "%s blend, order %zu: route back to the shared name", equal ? "equal" : "differing", o);
                    if (routes(out, moves[0].to, "sp") != equal) report("hands writer authority", detail);
                    snprintf(detail, sizeof(detail), "%s blend, order %zu: both writers' route into the copy", equal ? "equal" : "differing", o);
                    if (routes(out, "sp", moves[0].to) != 1) report("hands writer authority", detail);
                }
                sh_decl_animweb_moves_free(moves, move_count); moves = NULL; move_count = 0; free(out);
            }
        #undef AW_OWNER
    }
    #undef AW_PLASMA
    #undef AW_EXITS
    #undef AW_INTO_SP
    #undef AW_SP
    #undef AW_TRIO
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
        /* A node deep inside a sub-web composes at its own field granularity:
         * two contributions tune different blend fields of one transition. */
        static const char *expect[] = {"duration 7", "destDuration 12", "toState \"locked\""};
        pass("animweb nested edge parms", "animweb",
            "{ subWebs { subWeb \"w\" { node \"locked\" { edges { edge {\n"
            "toState \"locked\" blendParms { destDuration 4 duration 0 } } } } } } }",
            "{ subWebs { subWeb \"w\" { node \"locked\" { edges { edge {\n"
            "toState \"locked\" blendParms { destDuration 4 duration 7 } } } } } } }",
            "{ subWebs { subWeb \"w\" { node \"locked\" { edges { edge {\n"
            "toState \"locked\" blendParms { destDuration 12 duration 0 } } } } } } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    {
        /* An edge is the transition to its destination: edges two
         * contributions insert into one node both survive, and an insertion
         * before an edge another contribution tunes renumbers nothing. */
        static const char *expect[] = {"toSubWeb \"sp\"", "toSubWeb \"plasma\"", "duration 9"};
        pass("animweb inserted edges", "animweb",
            "{ subWebs { subWeb \"w\" { node \"down\" { edges {\n"
            "edge { toState \"up\" toSubWeb \"rifle\" blendParms { duration 3 } } } } } } }",
            "{ subWebs { subWeb \"w\" { node \"down\" { edges {\n"
            "edge { toState \"up\" toSubWeb \"sp\" blendParms { duration 3 } }\n"
            "edge { toState \"up\" toSubWeb \"rifle\" blendParms { duration 3 } } } } } } }",
            "{ subWebs { subWeb \"w\" { node \"down\" { edges {\n"
            "edge { toState \"up\" toSubWeb \"rifle\" blendParms { duration 9 } }\n"
            "edge { toState \"up\" toSubWeb \"plasma\" blendParms { duration 3 } } } } } } }",
            expect, sizeof(expect) / sizeof(expect[0]));
    }
    /* Retargeting a transition removes it; tuning the old one then conflicts. */
    refuse("animweb retargeted edge", "animweb",
        "{ subWebs { subWeb \"w\" { node \"n\" { edges { edge { toState \"a\" blendParms { duration 0 } } } } } } }",
        "{ subWebs { subWeb \"w\" { node \"n\" { edges { edge { toState \"b\" blendParms { duration 0 } } } } } } }",
        "{ subWebs { subWeb \"w\" { node \"n\" { edges { edge { toState \"a\" blendParms { duration 4 } } } } } } }",
        "incompatible");
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

    test_hands_web();

    /* ------------------------------------------------------------ families */
    if (!sh_decl_block_family("breakable")) report("family table", "breakable is not registered");
    if (sh_decl_block_family("md6def")) report("family table", "md6def must use its own adapter");
    if (sh_decl_block_family("renderprog")) report("family table", "renderprog is program text");

    printf("block composition: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
