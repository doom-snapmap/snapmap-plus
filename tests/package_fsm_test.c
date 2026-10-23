/* package_fsm_test.c -- layer configurations, the first declaration's private
 * configurations, the placeholder and private configuration names. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_fsm.h"
#include "decl_graph.h"

static int g_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static const char FIRST[] =
    "{\n\tedit = {\n\t\tlayers = {\n\t\t\tlayer = \"common\";\n\t\t\tlayer = \"imp_combat\";\n"
    "\t\t\tlayer = \"cyberdemon_hell\";\n\t\t\tlayer = \"disabled\";\n\t\t}\n"
    "\t\tlayersConfigs = {\n\t\t\tcommon = {\n\t\t\t\tlayer = \"common\";\n\t\t\t}\n"
    "\t\t\tcyberdemon_hell = {\n\t\t\t\tlayer = \"common\";\n\t\t\t\tlayer = \"cyberdemon_hell\";\n\t\t\t}\n"
    "\t\t\timp = {\n\t\t\t\tlayer = \"common\";\n\t\t\t\tlayer = \"imp_combat\";\n\t\t\t}\n\t\t}\n"
    "\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n\t\t\t\t\tclassName = \"idAIFSMGraphFSM\";\n"
    "\t\t\t\t\tobject = {\n\t\t\t\t\t\tname = \"FSM_alertCycle\";\n\t\t\t\t\t\tfsmType = \"idAlertCycleFSM\";\n"
    "\t\t\t\t\t\tisRoot = true;\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t\tlayers = {\n\t\t\t\t\tlayer = \"common\";\n\t\t\t\t}\n"
    "\t\t\t\tnodes = {\n\t\t\t\t\tnode = {\n\t\t\t\t\t\tobject = {\n\t\t\t\t\t\t\tclassName = \"idAIFSMGraphState\";\n"
    "\t\t\t\t\t\t\tobject = {\n\t\t\t\t\t\t\t\tname = \"alert_default\";\n\t\t\t\t\t\t\t\tisDefault = true;\n"
    "\t\t\t\t\t\t\t}\n\t\t\t\t\t\t}\n\t\t\t\t\t\tlayers = {\n\t\t\t\t\t\t\tlayer = \"common\";\n\t\t\t\t\t\t}\n"
    "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t\tlinks = {\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";

static const char GENERIC[] =
    "{\n\tedit = {\n\t\tlayers = {\n\t\t\tlayer = \"common\";\n\t\t\tlayer = \"imp_combat\";\n"
    "\t\t\tlayer = \"cyberdemon_hell\";\n\t\t\tlayer = \"disabled\";\n\t\t}\n"
    "\t\tlayersConfigs = {\n\t\t\tcommon = {\n\t\t\t\tlayer = \"common\";\n\t\t\t}\n\t\t}\n"
    "\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n\t\t\t\t\tclassName = \"idAIFSMGraphFSM\";\n"
    "\t\t\t\t\tobject = {\n\t\t\t\t\t\tname = \"FSM_generic\";\n\t\t\t\t\t\tfsmType = \"idAIFSM\";\n"
    "\t\t\t\t\t\tisRoot = false;\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t\tlayers = {\n\t\t\t\t\tlayer = \"common\";\n"
    "\t\t\t\t\tlayer = \"cyberdemon_hell\";\n\t\t\t\t}\n"
    "\t\t\t\tnodes = {\n\t\t\t\t\tnode = {\n\t\t\t\t\t\tobject = {\n\t\t\t\t\t\t\tclassName = \"idAIFSMGraphState\";\n"
    "\t\t\t\t\t\t\tobject = {\n\t\t\t\t\t\t\t\tname = \"generic_default\";\n\t\t\t\t\t\t\t\tisDefault = true;\n"
    "\t\t\t\t\t\t\t}\n\t\t\t\t\t\t}\n\t\t\t\t\t\tlayers = {\n\t\t\t\t\t\t\tlayer = \"common\";\n\t\t\t\t\t\t}\n"
    "\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t\tlinks = {\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";

static sh_decl_source src(const char *text) { return (sh_decl_source){text, strlen(text)}; }

static int layers_are(sh_decl_source first, const char *config, const char *const *expected, size_t count)
{
    char **layers = NULL, error[256] = "";
    size_t n = 0;
    int found = sh_package_fsm_config_layers(first, config, &layers, &n, error, sizeof(error));
    int ok = found == 1 && n == count;
    for (size_t i = 0; ok && i < n; i++) ok = !strcmp(layers[i], expected[i]);
    if (!ok) printf("  %s: found %d, %zu layer(s) %s\n", config, found, n, error);
    sh_package_fsm_names_free(layers, n);
    return ok;
}

static void test_layers(void)
{
    const char *const hell[] = {"common", "cyberdemon_hell"}, *const common[] = {"common"};
    char **layers = NULL, error[256];
    size_t n = 0;
    CHECK(layers_are(src(FIRST), "cyberdemon_hell", hell, 2));
    CHECK(layers_are(src(FIRST), "common", common, 1));
    CHECK(sh_package_fsm_config_layers(src(FIRST), "spider_mastermind", &layers, &n, error, sizeof(error)) == 0 && !n);
    CHECK(sh_package_fsm_config_layers(src("{ edit = { } }"), "common", &layers, &n, error, sizeof(error)) == -1);
    CHECK(sh_package_fsm_config_layers(src("{ edit = { layersConfigs = { common = { other = \"x\"; } } } }"),
        "common", &layers, &n, error, sizeof(error)) == -1);
}

static void test_masks(void)
{
    static const char UNLISTED[] =
        "{ edit = { layers = { layer = \"common\"; } layersConfigs = { odd = { layer = \"common\"; layer = \"other\"; } } } }";
    uint32_t mask = 0xffffffffu;
    char error[256];
    /* Bits follow the declaration's own layer order: common 0, imp_combat 1, cyberdemon_hell 2. */
    CHECK(sh_package_fsm_config_mask(src(FIRST), "cyberdemon_hell", &mask, error, sizeof(error)) == 1 && mask == 5);
    CHECK(sh_package_fsm_config_mask(src(FIRST), "imp", &mask, error, sizeof(error)) == 1 && mask == 3);
    CHECK(sh_package_fsm_config_mask(src(FIRST), "spider_mastermind", &mask, error, sizeof(error)) == 0 && !mask);
    CHECK(sh_package_fsm_config_mask(src(UNLISTED), "odd", &mask, error, sizeof(error)) == -1 && !mask);
    CHECK(sh_package_fsm_config_mask(src("{ edit = { } }"), "common", &mask, error, sizeof(error)) == -1);
    CHECK(sh_package_fsm_graph_mask(src(GENERIC), &mask, error, sizeof(error)) == 1 && mask == 5);
    CHECK(sh_package_fsm_graph_mask(src(FIRST), &mask, error, sizeof(error)) == 1 && mask == 1);
    CHECK(sh_package_fsm_graph_mask(src("{ edit = { layers = { layer = \"common\"; } subGraphs = { } } }"),
        &mask, error, sizeof(error)) == 0 && !mask);
    CHECK(sh_package_fsm_graph_mask(src("{ edit = { subGraphs = { subGraph = { layers = { layer = \"x\"; } } } } }"),
        &mask, error, sizeof(error)) == -1);
}

static void test_first(void)
{
    const char *const hell[] = {"common", "cyberdemon_hell"}, *const imp[] = {"common", "imp_combat"};
    sh_package_fsm_config configs[2] = {{"smp_0123456789abcdef0123456789abcdef", "cyberdemon_hell"},
                                        {"smp_ffffffffffffffffffffffffffffffff", "imp"}};
    char error[256] = "";
    size_t length = 0;
    char *out = sh_package_fsm_first(src(FIRST), configs, 2, &length, error, sizeof(error));
    CHECK(out && length == strlen(out));
    if (out) {
        sh_decl_graph *graph = sh_decl_graph_open((sh_decl_source){out, length}, error, sizeof(error));
        /* Private configurations list exactly their base layers; native ones are unchanged. */
        CHECK(layers_are((sh_decl_source){out, length}, configs[0].name, hell, 2));
        CHECK(layers_are((sh_decl_source){out, length}, configs[1].name, imp, 2));
        CHECK(layers_are((sh_decl_source){out, length}, "cyberdemon_hell", hell, 2));
        CHECK(layers_are((sh_decl_source){out, length}, "imp", imp, 2));
        /* Still one valid graph declaration with its root graph. */
        CHECK(graph && sh_decl_graph_count(graph) >= 2);
        sh_decl_graph_close(graph);
        free(out);
    }
    /* Refusals: missing base, existing name, not a plain name. */
    sh_package_fsm_config bad[3] = {{"smp_x", "spider_mastermind"}, {"imp", "common"}, {"smp/x", "common"}};
    for (int i = 0; i < 3; i++) {
        out = sh_package_fsm_first(src(FIRST), &bad[i], 1, &length, error, sizeof(error));
        CHECK(!out && !length && error[0]);
        free(out);
    }
}

static void test_placeholder(void)
{
    char error[256] = "";
    size_t length = 0;
    char *out = sh_package_fsm_placeholder(src(GENERIC), &length, error, sizeof(error));
    CHECK(out != NULL);
    if (out) {
        sh_decl_graph *graph = sh_decl_graph_open((sh_decl_source){out, length}, error, sizeof(error));
        size_t graphs = 0;
        CHECK(graph != NULL);
        for (size_t i = 0; graph && i < sh_decl_graph_count(graph); i++) {
            const sh_decl_graph_record *r = sh_decl_graph_at(graph, i);
            if (r->kind == SH_DECL_GRAPH_SUBGRAPH) { graphs++; CHECK(r->layers == 0); }
        }
        CHECK(graphs == 1);
        sh_decl_graph_close(graph);
        free(out);
    }
    CHECK(!sh_package_fsm_placeholder(src("{ edit = { subGraphs = { } } }"), &length, error, sizeof(error)));
}

static void test_names(void)
{
    const char *const from[] = {"states/cyberdemon_hell", "states/alertcycle"};
    const char *const to[] = {"smp/aaaa", "smp/first"};
    const char *const from_r[] = {"states/alertcycle", "states/cyberdemon_hell"};
    const char *const to_r[] = {"smp/first", "smp/aaaa"};
    const char *const to_other[] = {"smp/bbbb", "smp/first"};
    const char *const extra[] = {"smp/x2", "smp/x1"}, *const extra_r[] = {"smp/x1", "smp/x2"};
    char a[40], b[40], c[40], d[40], e[40];
    CHECK(sh_package_fsm_config_name("cyberdemon_hell", from, to, 2, extra, 2, a));
    CHECK(sh_package_fsm_config_name("cyberdemon_hell", from_r, to_r, 2, extra_r, 2, b));
    CHECK(sh_package_fsm_config_name("cyberdemon_hell", from, to_other, 2, extra, 2, c));
    CHECK(sh_package_fsm_config_name("cyberdemon", from, to, 2, extra, 2, d));
    CHECK(sh_package_fsm_config_name("cyberdemon_hell", from, to, 2, NULL, 0, e));
    CHECK(!strcmp(a, b));                        /* admission order does not matter */
    CHECK(strcmp(a, c) && strcmp(a, d) && strcmp(a, e));
    CHECK(strlen(a) == 36 && !strncmp(a, "smp_", 4));
    for (int i = 4; i < 36; i++) CHECK((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f'));
    CHECK(!sh_package_fsm_config_name(NULL, from, to, 2, NULL, 0, a));
}

/* ---- Selection ---------------------------------------------------------- */

/* A replacement of one substring (exactly one occurrence), malloc-owned. */
static char *replaced(const char *text, const char *from, const char *to)
{
    const char *at = strstr(text, from);
    if (!at || strstr(at + 1, from)) return NULL;
    size_t head = (size_t)(at - text), length = strlen(text) - strlen(from) + strlen(to);
    char *out = malloc(length + 1);
    if (!out) return NULL;
    memcpy(out, text, head); strcpy(out + head, to); strcat(out, at + strlen(from));
    return out;
}
/* One graph declaration with FIRST's layer list and one graph on the given layers. */
static char *graph(const char *layers)
{
    static const char head[] = "{\n\tedit = {\n\t\tlayers = {\n\t\t\tlayer = \"common\";\n\t\t\tlayer = \"imp_combat\";\n"
        "\t\t\tlayer = \"cyberdemon_hell\";\n\t\t\tlayer = \"disabled\";\n\t\t}\n"
        "\t\tsubGraphs = {\n\t\t\tsubGraph = {\n\t\t\t\tobject = {\n\t\t\t\t\tclassName = \"idAIFSMGraphFSM\";\n"
        "\t\t\t\t\tobject = {\n\t\t\t\t\t\tname = \"FSM_x\";\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t\tlayers = {\n";
    static const char tail[] = "\t\t\t\t}\n\t\t\t\tnodes = {\n\t\t\t\t}\n\t\t\t\tlinks = {\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
    char *out = malloc(sizeof(head) + strlen(layers) + sizeof(tail));
    if (out) { strcpy(out, head); strcat(out, layers); strcat(out, tail); }
    return out;
}
typedef struct named { const char *name; const char *text; } named;
typedef struct world { named items[8]; size_t count; const char *visited[8]; size_t visits; } world;
static const char *world_text(const world *w, const char *name)
{
    for (size_t i = 0; i < w->count; i++) if (!strcmp(w->items[i].name, name)) return w->items[i].text;
    return NULL;
}
static int world_exists(void *context, const char *declaration) { return world_text(context, declaration) != NULL; }
static int world_mask(void *context, const char *declaration, const char *config, uint32_t *mask)
{
    const char *text = world_text(context, declaration);
    char error[256];
    *mask = 0;
    if (!text) return 0;
    return config ? sh_package_fsm_config_mask(src(text), config, mask, error, sizeof(error)) :
        sh_package_fsm_graph_mask(src(text), mask, error, sizeof(error));
}
static int world_visit(void *context, const char *name)
{
    world *w = context;
    if (w->visits == 8) return 0;
    w->visited[w->visits++] = name; return 1;
}
static int visited_are(const world *w, const char *const *expected, size_t count)
{
    if (w->visits != count) { printf("  visited %zu, expected %zu\n", w->visits, count); return 0; }
    for (size_t i = 0; i < count; i++) if (strcmp(w->visited[i], expected[i])) {
        printf("  visit %zu: %s, expected %s\n", i, w->visited[i], expected[i]); return 0;
    }
    return 1;
}

static void test_select(void)
{
    /* A configuration without "common": its first declaration's own graphs
     * (on common) are not selected, but the configuration is still read there. */
    char *first = replaced(FIRST, "\t\t\timp = {", "\t\t\tsolo = {\n\t\t\t\tlayer = \"cyberdemon_hell\";\n\t\t\t}\n\t\t\timp = {");
    char *common = graph("\t\t\t\t\tlayer = \"common\";\n"), *hell = graph("\t\t\t\t\tlayer = \"cyberdemon_hell\";\n");
    char *imp = graph("\t\t\t\t\tlayer = \"imp_combat\";\n"), *broken = graph("\t\t\t\t\tlayer = \"nowhere\";\n");
    const char *names[] = {"states/behaviors", "states/cyberdemon_hell", "states/impcombat", SH_PACKAGE_FSM_FIRST};
    world w = {{{SH_PACKAGE_FSM_FIRST, first}, {"states/behaviors", common}, {"states/cyberdemon_hell", hell},
        {"states/impcombat", imp}}, 4};
    sh_package_fsm_layers layers = {&w, world_exists, world_mask};
    CHECK(first && common && hell && imp && broken);
    if (!first || !common || !hell || !imp || !broken) goto done;
    {
        const char *expected[] = {SH_PACKAGE_FSM_FIRST, "states/behaviors", "states/cyberdemon_hell"};
        w.visits = 0;
        CHECK(sh_package_fsm_select("cyberdemon_hell", &layers, names, 4, world_visit, &w) && visited_are(&w, expected, 3));
    }
    {
        const char *expected[] = {SH_PACKAGE_FSM_FIRST, "states/cyberdemon_hell"};
        w.visits = 0;
        CHECK(sh_package_fsm_select("solo", &layers, names, 4, world_visit, &w) && visited_are(&w, expected, 2));
    }
    {
        /* A configuration the first declaration lacks: still its source. */
        const char *expected[] = {SH_PACKAGE_FSM_FIRST};
        w.visits = 0;
        CHECK(sh_package_fsm_select("spider_mastermind", &layers, names, 4, world_visit, &w) && visited_are(&w, expected, 1));
    }
    /* No first declaration: no web, nothing selected. */
    w.items[0].name = "states/elsewhere"; w.visits = 0;
    CHECK(sh_package_fsm_select("cyberdemon_hell", &layers, names, 4, world_visit, &w) && w.visits == 0);
    w.items[0].name = SH_PACKAGE_FSM_FIRST;
    /* An unreadable candidate fails the selection. */
    w.items[3].text = broken; w.visits = 0;
    CHECK(!sh_package_fsm_select("imp", &layers, names, 4, world_visit, &w));
done:
    free(first); free(common); free(hell); free(imp); free(broken);
}

static int read_parent(void *context, const char *name, sh_decl_source *out)
{
    const char *text = world_text(context, name);
    if (!text) return 0;
    out->text = _strdup(text); out->length = strlen(text);
    return out->text ? 1 : -1;
}
static void test_behavior_config(void)
{
    world parents = {{{"behaviors/base", "{ edit = { aiFSMManagerDeclLayersConfig = \"cyberdemon_hell\"; } }"},
        {"behaviors/middle", "{ inherit = \"behaviors/base\"; edit = { monsterType = \"AI_MONSTER_X\"; } }"}}, 2};
    sh_decl_entity_class_source source = {&parents, read_parent, NULL};
    char *config = NULL, error[256];
    CHECK(sh_package_fsm_behavior_config(src("{ inherit = \"behaviors/middle\"; edit = { } }"), &source, &config,
        error, sizeof(error)) == 1 && config && !strcmp(config, "cyberdemon_hell"));
    free(config);
    CHECK(sh_package_fsm_behavior_config(src("{ inherit = \"behaviors/middle\"; edit = { aiFSMManagerDeclLayersConfig = \"solo\"; } }"),
        &source, &config, error, sizeof(error)) == 1 && config && !strcmp(config, "solo"));
    free(config);
    CHECK(sh_package_fsm_behavior_config(src("{ edit = { monsterType = \"AI_MONSTER_X\"; } }"), &source, &config,
        error, sizeof(error)) == 0 && !config);
    CHECK(sh_package_fsm_behavior_config(src("{ inherit = \"behaviors/missing\"; edit = { } }"), &source, &config,
        error, sizeof(error)) == -1 && !config);
}

/* ---- Webs ----------------------------------------------------------------- */

static const sh_package_fsm_web *web_of(const sh_package_fsm_webs *webs, const char *behavior)
{
    for (size_t i = 0; webs && i < webs->count; i++)
        for (size_t k = 0; k < webs->webs[i].behavior_count; k++)
            if (!strcmp(webs->webs[i].behaviors[k], behavior)) return webs->webs + i;
    return NULL;
}
static const sh_package_fsm_declaration *generated_of(const sh_package_fsm_webs *webs, const char *name)
{
    for (size_t i = 0; webs && i < webs->generated_count; i++)
        if (!strcmp(webs->generated[i].name, name)) return webs->generated + i;
    return NULL;
}
static int owns(const sh_package_fsm_webs *webs, const char *name)
{
    for (size_t i = 0; webs && i < webs->owned_count; i++) if (!strcmp(webs->owned[i], name)) return 1;
    return 0;
}
/* The mask a web's private configuration selects in its generated first declaration. */
static uint32_t private_mask(const sh_package_fsm_webs *webs, const sh_package_fsm_web *web)
{
    const sh_package_fsm_declaration *first = web ? generated_of(webs, web->to[0]) : NULL;
    uint32_t mask = 0; char error[256];
    if (!first || sh_package_fsm_config_mask((sh_decl_source){first->text, first->length}, web->config, &mask,
        error, sizeof(error)) != 1) return 0xffffffffu;
    return mask;
}

static void test_webs(void)
{
    char *native = replaced(FIRST, "\t\t\timp = {", "\t\t\tsolo = {\n\t\t\t\tlayer = \"cyberdemon_hell\";\n\t\t\t}\n\t\t\timp = {");
    /* A configuration-only edit: solo gains imp_combat; no graph declaration changes. */
    char *edited = native ? replaced(native, "\t\t\tsolo = {\n\t\t\t\tlayer = \"cyberdemon_hell\";",
        "\t\t\tsolo = {\n\t\t\t\tlayer = \"imp_combat\";\n\t\t\t\tlayer = \"cyberdemon_hell\";") : NULL;
    const char *first_from[] = {SH_PACKAGE_FSM_FIRST}, *first_to[] = {"smp/alert_x"}, *first_other[] = {"smp/alert_y"};
    const char *hell_from[] = {"states/cyberdemon_hell"}, *hell_to[] = {"smp/hell_x"};
    const char *added[] = {"smp/added_x"}, *owned[] = {"smp/hell_x", "smp/alert_x", "smp/added_x"};
    char error[512] = "";
    uint32_t chosen = 0;
    CHECK(native && edited);
    if (!native || !edited) goto done;
    CHECK(sh_package_fsm_config_mask(src(edited), "solo", &chosen, error, sizeof(error)) == 1 && chosen == 6);
    {
        /* The config-only edit binds its behaviors to a private web whose
         * first declaration carries the edited mask under a private
         * configuration (its own state-data factory), though no selected
         * graph file changed. A graph edit under the native configuration is
         * a different web; equal inputs share one. */
        sh_package_fsm_bound bound[4] = {
            {"smp/b_config", "solo", src(edited), first_from, first_to, 1, NULL, 0},
            {"smp/b_config2", "solo", src(edited), first_from, first_to, 1, NULL, 0},
            {"smp/b_graph", "solo", src(native), hell_from, hell_to, 1, added, 1},
            {"smp/b_other", "solo", src(edited), first_from, first_other, 1, NULL, 0}};
        sh_package_fsm_webs *webs = sh_package_fsm_webs_build(bound, 4, owned, 3, error, sizeof(error));
        CHECK(webs != NULL);
        if (webs) {
            const sh_package_fsm_web *config_web = web_of(webs, "smp/b_config"), *graph_web = web_of(webs, "smp/b_graph");
            const sh_package_fsm_web *other_web = web_of(webs, "smp/b_other");
            CHECK(webs->count == 3 && config_web && graph_web && other_web);
            CHECK(config_web == web_of(webs, "smp/b_config2") && config_web->behavior_count == 2);
            CHECK(config_web != graph_web && config_web != other_web && graph_web != other_web);
            CHECK(config_web && config_web->count == 1 && !strcmp(config_web->from[0], SH_PACKAGE_FSM_FIRST));
            CHECK(config_web && !strncmp(config_web->to[0], SH_PACKAGE_FSM_FIRST_PREFIX, strlen(SH_PACKAGE_FSM_FIRST_PREFIX)) &&
                !strcmp(config_web->to[0] + strlen(SH_PACKAGE_FSM_FIRST_PREFIX), config_web->config));
            CHECK(private_mask(webs, config_web) == 6 && private_mask(webs, other_web) == 6);
            CHECK(private_mask(webs, graph_web) == 4);
            CHECK(graph_web && graph_web->count == 2 && !strcmp(graph_web->from[1], "states/cyberdemon_hell") &&
                !strcmp(graph_web->to[1], "smp/hell_x") && graph_web->extra_count == 1 && !strcmp(graph_web->extra[0], "smp/added_x"));
            /* Native configurations are unchanged in every generated first declaration. */
            for (size_t i = 0; i < webs->generated_count; i++) {
                uint32_t mask = 0;
                CHECK(sh_package_fsm_config_mask((sh_decl_source){webs->generated[i].text, webs->generated[i].length},
                    "cyberdemon_hell", &mask, error, sizeof(error)) == 1 && mask == 5);
            }
            CHECK(webs->generated_count == 3);
            CHECK(owns(webs, "smp/hell_x") && owns(webs, "smp/added_x") && owns(webs, SH_PACKAGE_FSM_PLACEHOLDER));
            for (size_t i = 0; i < webs->count; i++) CHECK(owns(webs, webs->webs[i].to[0]));
            sh_package_fsm_webs_free(webs);
        }
    }
    {
        /* The configuration must exist where the engine reads it. */
        sh_package_fsm_bound missing = {"smp/b", "spider_mastermind", src(native), hell_from, hell_to, 1, NULL, 0};
        error[0] = 0;
        CHECK(!sh_package_fsm_webs_build(&missing, 1, NULL, 0, error, sizeof(error)) && strstr(error, "does not define"));
        /* Nothing binds or is owned: an empty table, no placeholder. */
        sh_package_fsm_webs *empty = sh_package_fsm_webs_build(NULL, 0, NULL, 0, error, sizeof(error));
        CHECK(empty && !empty->count && !empty->owned_count && !empty->generated_count);
        sh_package_fsm_webs_free(empty);
    }
    {
        /* Joining: a behavior the map binds keeps the map's web; equal webs
         * unite their behaviors; owned names and generated text unite. */
        sh_package_fsm_bound map_bound[1] = {{"smp/b1", "solo", src(native), hell_from, hell_to, 1, NULL, 0}};
        sh_package_fsm_bound local_bound[3] = {
            {"smp/b1", "solo", src(edited), first_from, first_to, 1, NULL, 0},
            {"smp/b2", "solo", src(edited), first_from, first_to, 1, NULL, 0},
            {"smp/b3", "solo", src(native), hell_from, hell_to, 1, NULL, 0}};
        sh_package_fsm_webs *map = sh_package_fsm_webs_build(map_bound, 1, NULL, 0, error, sizeof(error));
        sh_package_fsm_webs *local = sh_package_fsm_webs_build(local_bound, 3, owned, 3, error, sizeof(error));
        sh_package_fsm_webs *joined = map && local ? sh_package_fsm_webs_join(map, local, error, sizeof(error)) : NULL;
        CHECK(map && local && joined);
        if (joined) {
            const sh_package_fsm_web *b1 = web_of(joined, "smp/b1"), *b2 = web_of(joined, "smp/b2"), *b3 = web_of(joined, "smp/b3");
            CHECK(b1 && b2 && b3 && b1 == b3 && b1 != b2 && b1->behavior_count == 2 && b2->behavior_count == 1);
            CHECK(b1 && private_mask(joined, b1) == 4 && b2 && private_mask(joined, b2) == 6);
            CHECK(owns(joined, "smp/hell_x") && owns(joined, SH_PACKAGE_FSM_PLACEHOLDER) && joined->generated_count == 2);
        }
        sh_package_fsm_webs_free(joined);
        /* Generated text with one name and different bytes cannot meet. */
        if (map && map->generated_count) {
            map->generated[0].text[map->generated[0].length - 2] ^= 1;
            sh_package_fsm_webs *conflict = sh_package_fsm_webs_join(map, local, error, sizeof(error));
            CHECK(!conflict || !generated_of(local, map->generated[0].name));
            sh_package_fsm_webs_free(conflict);
        }
        sh_package_fsm_webs_free(map); sh_package_fsm_webs_free(local);
    }
done:
    free(native); free(edited);
}

int main(void)
{
    test_layers();
    test_masks();
    test_select();
    test_behavior_config();
    test_webs();
    test_first();
    test_placeholder();
    test_names();
    if (g_failures) { printf("package_fsm_test: %d failure(s)\n", g_failures); return 1; }
    printf("package_fsm_test: all checks passed\n");
    return 0;
}
