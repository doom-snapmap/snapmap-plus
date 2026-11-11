/* decltree_test.c -- the structural decl model behind derived edits
 * (decl_tree.c, DESIGN-additive 2.4): the grammar and its token-preserving
 * round trip, each kind of member a diff records, the apply of several
 * sources (identical edits merge, additions union, a named entry two mods add
 * merges its rows, a clash served by row order or by the map's choice), and,
 * when the game is installed, the game's own texts of the files mods edit and
 * the real edited copies found in the mods folders given.
 *
 *   decltree_test.exe [doom=<install root>] [mods=<folder>]...
 *
 * Without an install the real cases print one line and are skipped. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decl_tree.h"
#include "modbundle.h"
#include "modstock.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

void backend_log(const char *message) { (void)message; }

/* ------------------------------------------------------------------ helpers */

/* The token sequence of a text, comments and blanks dropped, list indices as
 * written: two texts are the same to the engine's lexer when they agree. */
static int next_tok(const char *t, size_t n, size_t *i, size_t *s, size_t *l)
{
    size_t p = *i;
    for (;;) {
        while (p < n && strchr(" \t\r\n\f", t[p])) p++;
        if (p + 1 < n && t[p] == '/' && t[p + 1] == '/') { while (p < n && t[p] != '\n') p++; continue; }
        if (p + 1 < n && t[p] == '/' && t[p + 1] == '*') { p += 2; while (p + 1 < n && !(t[p] == '*' && t[p + 1] == '/')) p++; p += 2; continue; }
        break;
    }
    if (p >= n) { *i = p; return 0; }
    *s = p;
    if (t[p] == '"') { p++; while (p < n && t[p] != '"') p++; p++; }
    else if (strchr("{}=;", t[p])) p++;
    else if (t[p] == '(') { int d = 0; for (; p < n; p++) { if (t[p] == '(') d++; else if (t[p] == ')' && --d == 0) { p++; break; } } }
    else while (p < n && !strchr(" \t\r\n\f{}=;\"()", t[p])) p++;
    *l = p - *s;
    *i = p;
    return 1;
}

static int same_tokens(const char *a, size_t an, const char *b, size_t bn)
{
    size_t i = 0, j = 0, s1, l1, s2, l2;
    for (;;) {
        int x = next_tok(a, an, &i, &s1, &l1), y = next_tok(b, bn, &j, &s2, &l2);
        if (!x && !y) return 1;
        if (x != y || l1 != l2 || memcmp(a + s1, b + s2, l1)) return 0;
    }
}

/* Parse, write, parse again: semantically equal and lexed the same. */
static int round_trips(const char *text)
{
    size_t n = strlen(text), out_n = 0;
    sh_dt *t = sh_dt_parse(text, n, NULL, 0);
    char *out = t ? sh_dt_text(t, &out_n) : NULL;
    int ok = out && sh_dt_texts_equal(text, n, out, out_n) && same_tokens(text, n, out, out_n);
    if (!ok && out) fprintf(stderr, "  round trip of:\n%s\n  gave:\n%s\n", text, out);
    free(out);
    sh_dt_free(t);
    return ok;
}

static char *wrap(const char *element)
{
    size_t n = strlen(element);
    char *a = (char *)malloc(n + 3);
    if (!a) return NULL;
    a[0] = '[';
    memcpy(a + 1, element, n);
    a[n + 1] = ']';
    a[n + 2] = 0;
    return a;
}

static char g_lines[65536];
static void line_cb(void *c, const char *t)
{
    (void)c;
    strcat_s(g_lines, sizeof g_lines, t);
    strcat_s(g_lines, sizeof g_lines, "\n");
}

/* The stock text with the edits of copies[0..n-1] (in that row order), written out. */
static char *apply_copies(const char *request, const char *stock, const char *const *copies, int n, const sh_dt_choice *ch,
                          int nch, sh_dt_report *rep_out)
{
    /* As many copies as find_copies collects (16): the installed mods and
     * SNAPMAP_EDITS_ROOT each give a body per campaign pack. */
    sh_dt_source src[16];
    char *arr[16], who[16][16], *text = NULL;
    sh_dt_report rep;
    sh_dt *t;
    int i;
    memset(src, 0, sizeof src);
    if (n > 16) n = 16;
    for (i = 0; i < n; ++i) {
        char cause[300], *one = sh_dt_diff(request, stock, strlen(stock), copies[i], strlen(copies[i]), NULL, cause, sizeof cause);
        if (!one) fprintf(stderr, "  diff %d: %s\n", i, cause);
        arr[i] = one ? wrap(one) : NULL;
        free(one);
        _snprintf_s(who[i], sizeof who[i], _TRUNCATE, "k.%c", 'a' + i);
        src[i].key = who[i];
        src[i].who = who[i];
        src[i].edits = arr[i];
        src[i].edits_len = arr[i] ? strlen(arr[i]) : 0;
    }
    memset(&rep, 0, sizeof rep);
    rep.line = line_cb;
    g_lines[0] = 0;
    t = sh_dt_apply(request, stock, strlen(stock), src, n, ch, nch, &rep);
    if (t) text = sh_dt_text(t, NULL);
    sh_dt_free(t);
    for (i = 0; i < n; ++i) free(arr[i]);
    if (rep_out) *rep_out = rep;
    return text;
}

/* ------------------------------------------------------------------ the grammar */

static void test_grammar(void)
{
    char cause[300];
    printf("-- the grammar: every statement form survives a round trip, token for token\n");
    CHECK(round_trips("{\r\n\tinherit = \"player/mp\";\r\n\teditorVars {\r\n\t\tplaceable = false;\r\n\t}\r\n\tedit = {\r\n"
                      "\t\tcolor = ( 1, 1, 1, 1 );\r\n\t\tsize = 0 0 0;\r\n\t}\r\n}"));
    CHECK(round_trips("{\n\tinit {\n\t\tmesh \"md6/x.md6mesh\"\n\t}\n\tevents {\n\t\tanim \"md6/a.md6anim\" {\n"
                      "\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n\t\t\t}\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 2\n\t\t\t}\n\t\t}\n\t}\n"
                      "\taliases {\n\t\talias { name \"m/x\" anim \"md6/x.md6anim\" }\n\t}\n}\n"));
    /* md6Def bare members on their own lines, multi-word values, a named statement with '=' */
    CHECK(round_trips("{\n\tgroup \"legs\" {\n\t\tLeftFoot\n\t\tLeftToeBase\n\t}\n\ttags far scramble p/far\n"
                      "\tpainGroup \"head\" = damageGroup \"head\"\n\tmodel box( ( -5, -6.5, -5 ), ( 5, 6.5, 5 ) )\n}\n"));
    CHECK(round_trips("{ { 0, 1 } }"));                         /* a table's anonymous block */
    CHECK(round_trips("{ \"MD6_WEIGHTGROUP_ALL\" }"));          /* a bare member before the brace */
    CHECK(round_trips("{\n\t// a comment\n\tnum = 2; /* another */\n\titem[0] = \"a\";\n\titem[1] = \"b\";\n}\n"));
    /* Refused, with the line. */
    CHECK(!sh_dt_parse("{\r\n\t= 3;\r\n}", 11, cause, sizeof cause) && strstr(cause, "line 2: expected a key, found '='"));
    CHECK(!sh_dt_parse("{ edit = { ", 11, cause, sizeof cause) && strstr(cause, "a block is not closed"));
    CHECK(!sh_dt_parse("edit = { }", 10, cause, sizeof cause) && strstr(cause, "is not one { ... } decl body"));
    CHECK(!sh_dt_parse("{ } x", 5, cause, sizeof cause) && strstr(cause, "text after the decl's closing brace"));
    /* Semantic equality: fields in any order, list items in order. */
    CHECK(sh_dt_texts_equal("{ a = 1; b { c = 2; } }", 23, "{ b { c = 2; } a = 1 }", 22));
    CHECK(!sh_dt_texts_equal("{ l = { num = 2; item[0] = \"x\"; item[1] = \"y\"; } }", 50,
                             "{ l = { num = 2; item[0] = \"y\"; item[1] = \"x\"; } }", 50));
    CHECK(sh_dt_texts_equal("{ size = 0 0 1; }", 17, "{ size = 0  0\t1; }", 18));
}

/* ------------------------------------------------------------------ the diff */

static void test_diff(void)
{
    static const char STOCK[] =
        "{\r\n\tinherit = \"player/mp\";\r\n\teditorVars {\r\n\t\tplaceable = false;\r\n\t}\r\n\tedit = {\r\n"
        "\t\tmasterWeaponList = {\r\n\t\t\tnum = 2;\r\n\t\t\titem[0] = \"weapon/a\";\r\n\t\t\titem[1] = \"weapon/b\";\r\n\t\t}\r\n"
        "\t\tsyncGroups = {\r\n\t\t\tnum = 3;\r\n\t\t}\r\n\t\tlayers = {\r\n\t\t\tlayer = \"common\";\r\n\t\t\tlayer = \"imp\";\r\n\t\t}\r\n"
        "\t\tsubGraphs = {\r\n\t\t\tsubGraph = {\r\n\t\t\t\tobject = {\r\n\t\t\t\t\tname = \"root\";\r\n\t\t\t\t\tx = 1;\r\n\t\t\t\t}\r\n"
        "\t\t\t}\r\n\t\t\tsubGraph = {\r\n\t\t\t\tobject = {\r\n\t\t\t\t\tname = \"wounded\";\r\n\t\t\t\t}\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}";
    static const char COPY[] =
        "{\r\n\tinherit = \"player/mp\";\r\n\tedit = {\r\n\t\thands = \"player/fp_hands.md6\";\r\n"
        "\t\tmasterWeaponList = {\r\n\t\t\tnum = 3;\r\n\t\t\titem[0] = \"weapon/a\";\r\n\t\t\titem[1] = \"weapon/b\";\r\n"
        "\t\t\titem[2] = \"weapon/c\";\r\n\t\t}\r\n"
        "\t\tsyncGroups = {\r\n\t\t\tnum = 3;\r\n\t\t}\r\n\t\tlayers = {\r\n\t\t\tlayer = \"common\";\r\n\t\t\tlayer = \"imp\";\r\n"
        "\t\t\tlayer = \"spider\";\r\n\t\t}\r\n"
        "\t\tsubGraphs = {\r\n\t\t\tsubGraph = {\r\n\t\t\t\tobject = {\r\n\t\t\t\t\tname = \"root\";\r\n\t\t\t\t\tx = 2;\r\n\t\t\t\t}\r\n"
        "\t\t\t}\r\n\t\t\tsubGraph = {\r\n\t\t\t\tobject = {\r\n\t\t\t\t\tname = \"root_old\";\r\n\t\t\t\t}\r\n\t\t\t}\r\n"
        "\t\t\tsubGraph = {\r\n\t\t\t\tobject = {\r\n\t\t\t\t\tname = \"wounded\";\r\n\t\t\t\t}\r\n\t\t\t}\r\n\t\t}\r\n"
        "\t\tdamageGroups = {\r\n\t\t\titem[0] = {\r\n\t\t\t\tdamageScale = 1.5;\r\n\t\t\t}\r\n\t\t\titem[4] = {\r\n"
        "\t\t\t\tdamageScale = 2;\r\n\t\t\t}\r\n\t\t}\r\n\t}\r\n}";
    sh_dt_counts c;
    char cause[300], *e, *text;
    const char *copies[1];
    printf("-- the diff: fields set and removed, list entries, keyed entries, bag entries\n");
    e = sh_dt_diff("generated/decls/entitydef/player/coop.decl", STOCK, strlen(STOCK), COPY, strlen(COPY), &c, cause, sizeof cause);
    CHECK(e != NULL);
    if (!e) { fprintf(stderr, "  %s\n", cause); return; }
    CHECK(c.set == 2 && c.remove == 1 && c.list == 1 && c.keyed == 2 && c.bag == 1 && c.drop == 0);
    if (c.set != 2 || c.keyed != 2) fprintf(stderr, "  %s\n", e);
    CHECK(strstr(e, "{\"op\":\"remove\",\"path\":[\"editorVars\"]}") &&
          strstr(e, "{\"op\":\"set\",\"path\":[\"edit\",\"hands\"],\"entry\":\"hands = \\\"player/fp_hands.md6\\\";\"}") &&
          strstr(e, "{\"op\":\"list\",\"path\":[\"edit\",\"masterWeaponList\"],\"entry\":\"\\\"weapon/c\\\"\"}") &&
          strstr(e, "{\"op\":\"bag\",\"path\":[\"edit\",\"layers\"],\"entry\":\"layer = \\\"spider\\\";\"}") &&
          strstr(e, "\"op\":\"keyed\",\"path\":[\"edit\",\"subGraphs\"],\"word\":\"subGraph\",\"name\":\"root\"") &&
          strstr(e, "\"name\":\"root_old\"") && !strstr(e, "wounded") &&
          strstr(e, "\"op\":\"set\",\"path\":[\"edit\",\"damageGroups\"]") && !strstr(e, "syncGroups"));
    free(e);
    /* Applied to the game's text: the copy, semantically. */
    copies[0] = COPY;
    text = apply_copies("generated/decls/entitydef/player/coop.decl", STOCK, copies, 1, NULL, 0, NULL);
    CHECK(text && sh_dt_texts_equal(text, strlen(text), COPY, strlen(COPY)));
    CHECK(text && strstr(text, "\t\t\tnum = 3;\r\n\t\t\titem[0] = \"weapon/a\";") && strstr(text, "item[4] = {"));
    free(text);
    /* A list the copy reorders is written whole; a copy equal to the game's has no edits. */
    e = sh_dt_diff("x", "{ l = { num = 2; item[0] = \"a\"; item[1] = \"b\"; } }", 50,
                   "{ l = { num = 2; item[0] = \"b\"; item[1] = \"a\"; } }", 50, &c, cause, sizeof cause);
    CHECK(e && c.set == 1 && c.list == 0 && strstr(e, "\"op\":\"set\",\"path\":[\"l\"]"));
    free(e);
    e = sh_dt_diff("x", STOCK, strlen(STOCK), STOCK, strlen(STOCK), &c, cause, sizeof cause);
    CHECK(e && sh_dt_counts_total(&c) == 0 && strstr(e, "\"ops\":[]"));
    free(e);
    /* A copy outside the grammar is refused with the line; the game's text likewise. */
    CHECK(!sh_dt_diff("x", STOCK, strlen(STOCK), "{\n\t= 1;\n}", 9, &c, cause, sizeof cause) && strstr(cause, "line 2:"));
    CHECK(!sh_dt_diff("x", "{ ", 2, STOCK, strlen(STOCK), &c, cause, sizeof cause) && strstr(cause, "the game's text does not parse"));
}

/* ------------------------------------------------------------------ the apply */

static void test_apply(void)
{
    static const char BODY[] =
        "{\n\tinit {\n\t\tmesh \"m\"\n\t}\n\tevents {\n\t\tanim \"md6/a.md6anim\" {\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n"
        "\t\t\t}\n\t\t}\n\t}\n\taliases {\n\t}\n}\n";
    static const char B1[] =
        "{\n\tinit {\n\t\tmesh \"m\"\n\t}\n\tevents {\n\t\tanim \"md6/a.md6anim\" {\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n"
        "\t\t\t}\n\t\t\tevent \"ae_one\" {\n\t\t\t\tframe 5\n\t\t\t}\n\t\t}\n\t\tanim \"md6/new.md6anim\" {\n\t\t\tevent \"ae_p\" {\n"
        "\t\t\t\tframe 1\n\t\t\t}\n\t\t}\n\t}\n\taliases {\n\t\talias { name \"k/x\" anim \"md6/x.md6anim\" }\n\t}\n}\n";
    static const char B2[] =
        "{\n\tinit {\n\t\tmesh \"m\"\n\t}\n\tevents {\n\t\tanim \"md6/a.md6anim\" {\n\t\t\tevent \"ae_x\" {\n\t\t\t\tframe 1\n"
        "\t\t\t}\n\t\t}\n\t\tanim \"md6/new.md6anim\" {\n\t\t\tevent \"ae_q\" {\n\t\t\t\tframe 2\n\t\t\t}\n\t\t}\n\t}\n"
        "\taliases {\n\t\talias { name \"k/x\" anim \"md6/x.md6anim\" }\n\t}\n}\n";
    static const char P[] = "{\n\tedit = {\n\t\thands = \"stock\";\n\t\tbody = \"stock\";\n\t}\n}\n";
    static const char P1[] = "{\n\tedit = {\n\t\thands = \"k.a/hands\";\n\t\tbody = \"stock\";\n\t}\n}\n";
    static const char P2[] = "{\n\tedit = {\n\t\thands = \"k.b/hands\";\n\t\tbody = \"k.b/body\";\n\t}\n}\n";
    static const char P3[] = "{\n\tedit = {\n\t\thands = \"k.a/hands\";\n\t\tbody = \"stock\";\n\t}\n}\n";
    const char *copies[3];
    char *t1, *t2;
    sh_dt_report rep;
    sh_dt_choice ch;
    printf("-- the apply: identical edits merge, additions union, a clash by row order or by choice\n");
    /* Two bodies: a clip both add merges its rows; the stock clip one edits keeps the game's row;
     * the alias both add is one; either order gives the same text. */
    copies[0] = B1; copies[1] = B2;
    t1 = apply_copies("generated/decls/md6def/player/mp/tp_body_mp.md6.decl", BODY, copies, 2, NULL, 0, &rep);
    copies[0] = B2; copies[1] = B1;
    t2 = apply_copies("generated/decls/md6def/player/mp/tp_body_mp.md6.decl", BODY, copies, 2, NULL, 0, NULL);
    CHECK(t1 && t2 && sh_dt_texts_equal(t1, strlen(t1), t2, strlen(t2)));
    CHECK(t1 && strstr(t1, "ae_p") && strstr(t1, "ae_q") && strstr(t1, "ae_one") && strstr(t1, "ae_x"));
    CHECK(t1 && strstr(strstr(t1, "alias"), "alias") == strstr(t1, "alias") && !strstr(strstr(t1, "alias {") + 1, "alias {"));
    CHECK(rep.clashes == 0 && rep.merged >= 1);
    free(t1);
    free(t2);
    /* A field two sources set to different values: row order, one line naming the slot; the choice. */
    copies[0] = P1; copies[1] = P2; copies[2] = P3;
    t1 = apply_copies("generated/decls/entitydef/player/coop.decl", P, copies, 3, NULL, 0, &rep);
    CHECK(t1 && strstr(t1, "hands = \"k.a/hands\";") && strstr(t1, "body = \"k.b/body\";") && rep.clashes == 1);
    CHECK(strstr(g_lines, "entitydef/player/coop/edit/hands: set by k.a, k.b, k.c; serving k.a (row order; choose with "
                          "sh_mods choose \"entitydef/player/coop/edit/hands\" \"<mod name>\")") != NULL);
    if (!strstr(g_lines, "serving k.a (row order")) fprintf(stderr, "%s", g_lines);
    free(t1);
    ch.slot = "entitydef/player/coop/edit/hands";
    ch.key = "k.b";
    t1 = apply_copies("generated/decls/entitydef/player/coop.decl", P, copies, 3, &ch, 1, &rep);
    CHECK(t1 && strstr(t1, "hands = \"k.b/hands\";") && strstr(g_lines, "serving k.b (the map's choice)"));
    free(t1);
    /* A choice naming a key that does not set it: row order, said so. */
    ch.key = "k.z";
    t1 = apply_copies("generated/decls/entitydef/player/coop.decl", P, copies, 3, &ch, 1, NULL);
    CHECK(t1 && strstr(t1, "hands = \"k.a/hands\";") && strstr(g_lines, "the map chose k.z, which does not set it"));
    free(t1);
    /* Slots of one source's edits, as the Mods tab reads them. */
    {
        char cause[200], *one = sh_dt_diff("generated/decls/entitydef/player/coop.decl", P, strlen(P), P2, strlen(P2), NULL, cause,
                                          sizeof cause), *arr = one ? wrap(one) : NULL;
        CHECK(arr && sh_dt_edits_slots("generated/decls/entitydef/player/coop.decl", arr, strlen(arr), NULL, NULL) == 2);
        CHECK(arr && sh_dt_edits_files(arr, strlen(arr), NULL, NULL) == 1);
        free(one);
        free(arr);
    }
}

/* ------------------------------------------------------------------ the game's texts, the real copies */

static char *stock_text(const char *request, size_t *len)
{
    char sha[SH_MB_HASH_HEX + 1], cause[300];
    unsigned long long size = 0;
    unsigned char *bytes = NULL;
    char *t;
    *len = 0;
    if (sh_mb_stock_identify(request, sha, &size) != SH_MB_STOCK_OK ||
        sh_mb_stock_read(request, sha, &bytes, len, request, cause, sizeof cause) != SH_MB_STOCK_OK) return NULL;
    t = (char *)malloc(*len + 1);
    if (t) { memcpy(t, bytes, *len); t[*len] = 0; }
    HeapFree(GetProcessHeap(), 0, bytes);
    return t;
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = NULL;
    char *b;
    long n;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; *len = (size_t)n; }
    return b;
}

/* Every mod folder below `root` (two levels) carrying assets/<request>. */
typedef struct found { char path[16][MAX_PATH]; char folder[16][MAX_PATH]; int n; } found;
static void find_copies(const char *root, const char *request, int depth, found *out)
{
    char pattern[MAX_PATH], path[MAX_PATH], *p;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\%s", root, request);
    for (p = path; *p; ++p) if (*p == '/') *p = '\\';
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && out->n < 16) {
        strcpy_s(out->path[out->n], MAX_PATH, path);
        strcpy_s(out->folder[out->n], MAX_PATH, root);
        out->n++;
        return;
    }
    if (depth <= 0) return;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || !_stricmp(fd.cFileName, "assets")) continue;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", root, fd.cFileName);
        find_copies(path, request, depth - 1, out);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

#define R_CONDUCTOR "generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl"
#define R_BODY      "generated/decls/md6def/player/mp/tp_body_mp.md6.decl"
#define R_COOP      "generated/decls/entitydef/player/coop.decl"
#define R_SPIDER    "generated/decls/aifsmmanager/states/spidermastermind.decl"
#define R_PISTOL    "generated/decls/weapon/weapon/zion/player/sp/pistol.decl"
#define R_HANDS     "generated/decls/animweb/player/fp_hands_mp.decl"

static int clips_of(const char *text, int *aliases)
{
    sh_dt *t = sh_dt_parse(text, strlen(text), NULL, 0);
    sh_dt_node *ev = t ? sh_dt_at(sh_dt_root(t), "events") : NULL, *al = t ? sh_dt_at(sh_dt_root(t), "aliases") : NULL;
    int i, c = 0;
    for (i = 0; ev && i < ev->nkids; ++i) c += ev->kids[i]->block && ev->kids[i]->name && ev->kids[i]->key_n == 4 && !strncmp(ev->kids[i]->key, "anim", 4);
    *aliases = al ? al->nkids : 0;
    sh_dt_free(t);
    return c;
}

static void test_real(const char *const *mods, int nmods)
{
    static const char *const requests[] = { R_CONDUCTOR, R_BODY, R_COOP, R_SPIDER, R_PISTOL, R_HANDS };
    int r, m, k, copies_seen = 0;
    printf("-- the game's texts of the files mods edit, and the real edited copies\n");
    for (r = 0; r < (int)(sizeof requests / sizeof requests[0]); ++r) {
        size_t sl = 0;
        char *stock = stock_text(requests[r], &sl);
        found f;
        CHECK(stock != NULL);
        if (!stock) { fprintf(stderr, "  %s: not readable from the install\n", requests[r]); continue; }
        CHECK(round_trips(stock));
        printf("   %s: %zu bytes, parses and round-trips token for token\n", requests[r], sl);
        memset(&f, 0, sizeof f);
        for (m = 0; m < nmods; ++m) find_copies(mods[m], requests[r], 2, &f);
        for (k = 0; k < f.n; ++k) {
            size_t cl = 0;
            char *copy = read_all(f.path[k], &cl), cause[300], *e, *applied;
            const char *one[1];
            sh_dt_counts c;
            if (!copy) continue;
            if (cl == sl && !memcmp(copy, stock, sl)) { free(copy); continue; }   /* a surface, not an edit */
            e = sh_dt_diff(requests[r], stock, sl, copy, cl, &c, cause, sizeof cause);
            CHECK(e != NULL);
            if (!e) { fprintf(stderr, "  %s: %s\n", f.path[k], cause); free(copy); continue; }
            copies_seen++;
            printf("   copy in %s: %d set, %d removed, %d list, %d keyed, %d bag, %d dropped\n", f.folder[k], c.set, c.remove, c.list,
                   c.keyed, c.bag, c.drop);
            if (!strcmp(requests[r], R_COOP))   /* bare-hands: field sets, removed blocks, the added damageGroups block */
                CHECK(c.set >= 4 && c.remove >= 6 && c.list == 0 && c.bag == 0 && strstr(e, "\"path\":[\"edit\",\"damageGroups\"]") &&
                      strstr(e, "\"op\":\"remove\",\"path\":[\"edit\",\"masterWeaponList\"]"));
            if (!strcmp(requests[r], R_BODY))   /* a campaign pack's body: only additions */
                CHECK(c.set == 0 && c.remove == 0 && c.drop == 0 && c.bag > 0);
            if (!strcmp(requests[r], R_CONDUCTOR))   /* a boss pack's conductor: items added */
                CHECK(c.list > 0 && c.set == 0 && c.remove == 0 && c.keyed == 0 && c.bag == 0 && c.drop == 0);
            if (!strcmp(requests[r], R_SPIDER))   /* the root graph takes the stock name, the stock graph renamed */
                CHECK(c.keyed == 2 && sh_dt_counts_total(&c) == 2 && strstr(e, "\"name\":\"spiderMastermind\"") &&
                      strstr(e, "\"name\":\"spiderMastermind_attacks\""));
            one[0] = copy;
            applied = apply_copies(requests[r], stock, one, 1, NULL, 0, NULL);
            CHECK(applied && sh_dt_texts_equal(applied, strlen(applied), copy, cl));
            free(applied);
            free(e);
            free(copy);
        }
        if (!strcmp(requests[r], R_BODY) && f.n > 1) {
            /* Every pack's body merged per clip, in both orders: one text. */
            const char *texts[16], *rev[16];
            size_t lens[16];
            char *a, *b;
            int n = 0, i, aliases = 0, clips;
            for (k = 0; k < f.n; ++k) { char *t = read_all(f.path[k], &lens[n]); if (t) texts[n++] = t; }
            for (i = 0; i < n; ++i) rev[i] = texts[n - 1 - i];
            a = apply_copies(R_BODY, stock, texts, n, NULL, 0, NULL);
            b = apply_copies(R_BODY, stock, rev, n, NULL, 0, NULL);
            CHECK(a && b && sh_dt_texts_equal(a, strlen(a), b, strlen(b)));
            clips = a ? clips_of(a, &aliases) : 0;
            printf("   %d bodies merged in both orders: %d clips, %d alias%s, %zu bytes\n", n, clips, aliases,
                   aliases == 1 ? "" : "es", a ? strlen(a) : 0);
            for (i = 0; i < n; ++i) free((void *)texts[i]);
            free(a);
            free(b);
        }
        free(stock);
    }
    printf("   %d real edited copies diffed and applied\n", copies_seen);
}

int main(int argc, char **argv)
{
    const char *doom = NULL, *mods[8];
    int nmods = 0, i;
    for (i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "doom=", 5) && argv[i][5]) doom = argv[i] + 5;
        else if (!strncmp(argv[i], "mods=", 5) && argv[i][5] && nmods < 8) mods[nmods++] = argv[i] + 5;
    }
    {
        char env[MAX_PATH];
        static char extra[MAX_PATH];
        if (GetEnvironmentVariableA("SNAPMAP_EDITS_ROOT", env, sizeof env) && nmods < 8) {
            strcpy_s(extra, sizeof extra, env);
            mods[nmods++] = extra;
        }
    }
    printf("decltree_test\n");
    test_grammar();
    test_diff();
    test_apply();
    if (doom) {
        sh_modstock_set_install(doom);
        sh_modstock_register();
        test_real(mods, nmods);
    } else printf("-- the game's texts: no install given (doom=<root>); skipped\n");
    printf("%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
