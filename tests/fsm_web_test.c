/* fsm_web_test.c -- per-configuration state-machine webs: behavior-keyed views,
 * private configurations, activation, exclusion retention, generations, build
 * scoping and install. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsm_web.h"

void backend_log(const char *s) { (void)s; }

static int g_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

/* Synthetic engine objects with the native field offsets the hook reads. */
#define MAX_DECLS 16
typedef struct fake_decl { unsigned char bytes[0xd0]; } fake_decl;
typedef struct fake_graph { unsigned char bytes[0x40]; } fake_graph;
static fake_decl g_decls[MAX_DECLS];
static fake_graph g_graphs[MAX_DECLS];
static void *g_graph_ptrs[MAX_DECLS];
static unsigned char g_list[0x40];
static int g_count;
static uintptr_t g_generation = 0x1000;

/* The parsed configuration table of the declaration that replaces the first
 * one: 16-byte {interned name, mask} entries. */
typedef struct fake_config { const char *name; uint32_t mask, pad; } fake_config;
static const char CFG_A[] = "smp_cfg_a", CFG_B[] = "smp_cfg_b";
static fake_config g_configs[3] = {{"common", 1, 0}, {CFG_A, 5, 0}, {CFG_B, 5, 0}};

static void decl_set(int index, const char *name, uint32_t mask, int graphs)
{
    unsigned char *d = g_decls[index].bytes;
    void **array = &g_graph_ptrs[index];
    fake_config *configs = g_configs;
    int config_count = 3;
    memset(d, 0, sizeof(g_decls[index].bytes));
    memset(g_graphs[index].bytes, 0, sizeof(g_graphs[index].bytes));
    memcpy(d + 0x08, &name, sizeof(name));
    memcpy(g_graphs[index].bytes + 0x38, &mask, sizeof(mask));
    g_graph_ptrs[index] = &g_graphs[index];
    memcpy(d + 0x68, &array, sizeof(array));
    memcpy(d + 0x70, &graphs, sizeof(graphs));
    if (!strcmp(name, "smp/alert_ab")) {
        memcpy(d + 0xb8, &configs, sizeof(configs));
        memcpy(d + 0xc0, &config_count, sizeof(config_count));
    }
}

/* Each call is a new list generation: a new array identity and count. */
static void list_generation(int count)
{
    int capacity = MAX_DECLS;
    g_count = count; g_generation += 0x100;
    memset(g_list, 0, sizeof(g_list));
    memcpy(g_list + 0x20, &g_generation, sizeof(g_generation));
    memcpy(g_list + 0x28, &count, sizeof(count));
    memcpy(g_list + 0x2c, &capacity, sizeof(capacity));
}

static void list_set(const char *const *names, int count)
{
    for (int i = 0; i < count; i++)
        decl_set(i, names[i], strcmp(names[i], SH_FSM_WEB_PLACEHOLDER) ? 1u << (i % 30) : 0u, 1);
    list_generation(count);
}

static void *fake_get(void *list, int index)
{
    if (list != g_list || index < 0 || index >= g_count) return NULL;
    return &g_decls[index];
}

static const char *decl_name(void *decl)
{
    const char *name = NULL;
    if (decl) memcpy(&name, (unsigned char *)decl + 0x08, sizeof(name));
    return name;
}

/* Synthetic behavior declarations: name at +8, layer configuration at +0x558
 * (the builder receives behavior+0x558, as the native caller passes it). */
typedef struct fake_behavior { unsigned char bytes[0x600]; const char *name, *config; } fake_behavior;
static fake_behavior g_behaviors[8];
static void *owner(const char *name)
{
    const char *config = !strcmp(name, "behaviors/imp") ? "imp" : "cyberdemon_hell";
    int i;
    for (i = 0; i < 8 && g_behaviors[i].name && strcmp(g_behaviors[i].name, name); i++) {}
    if (i == 8) return NULL;
    fake_behavior *b = &g_behaviors[i];
    b->name = name; b->config = config;
    memcpy(b->bytes + 0x08, &b->name, sizeof(b->name));
    memcpy(b->bytes + 0x558, &b->config, sizeof(b->config));
    return b->bytes + 0x558;
}

/* The fake builder records its configuration argument and what the list
 * resolver returns in native order (entry 0 first, then every index); it can
 * nest, raise or republish. */
static const char *g_seen[MAX_DECLS + 1];
static int g_seen_count;
static void *g_arg_seen;
static const char *g_config_seen;
static enum { PLAIN, NEST_NATIVE, RAISE, REPUBLISH, DEEP } g_mode;
static int g_deep, g_deep_max, g_deep_ok;
static const sh_fsm_web_table *g_swap;

static void record(void)
{
    g_seen_count = 0;
    g_seen[g_seen_count++] = decl_name(sh_fsm_web_test_decl_at(g_list, 0));
    for (int i = 0; i < g_count; i++) g_seen[g_seen_count++] = decl_name(sh_fsm_web_test_decl_at(g_list, i));
}

static uintptr_t fake_builder(void *manager, void *config)
{
    const char *head, *received = config ? *(const char *const *)config : NULL;
    (void)manager;
    if (g_mode == DEEP) {
        /* Alternate bound and native builds at every level; each level must see
         * its own view before and after its nested build. */
        int level = g_deep++;
        const char *before = decl_name(sh_fsm_web_test_decl_at(g_list, 2));
        if (level < g_deep_max)
            sh_fsm_web_test_build(NULL, owner((level % 2) ? "behaviors/a" : "behaviors/native"));
        const char *after = decl_name(sh_fsm_web_test_decl_at(g_list, 2));
        const char *expect = received == CFG_A ? "smp/variant_a" : "states/cyberdemon_hell";
        if (!before || !after || strcmp(before, expect) || strcmp(after, expect)) g_deep_ok = 0;
        return 7;
    }
    head = decl_name(sh_fsm_web_test_decl_at(g_list, 0));
    if (g_mode == NEST_NATIVE) {
        g_mode = PLAIN;
        sh_fsm_web_test_build(NULL, owner("behaviors/native"));
        CHECK(!strcmp(g_seen[3], "states/cyberdemon_hell") && !strcmp(g_config_seen, "cyberdemon_hell"));
        g_mode = NEST_NATIVE;
    }
    if (g_mode == REPUBLISH) CHECK(sh_fsm_web_publish(g_swap));
    if (g_mode == RAISE) RaiseException(0xE0FF0001u, 0, 0, NULL);
    record();
    g_arg_seen = config; g_config_seen = received;
    CHECK(g_seen[0] == head || (head && g_seen[0] && !strcmp(head, g_seen[0])));
    return 7;
}

static void *g_arg_sent;
static int build(const char *behavior)
{
    g_seen_count = 0; g_arg_seen = NULL; g_config_seen = NULL;
    g_arg_sent = behavior ? owner(behavior) : NULL;
    return (int)sh_fsm_web_test_build(NULL, g_arg_sent);
}

/* A native build receives its own argument untouched. */
static int native_config(const char *config)
{
    return g_arg_seen == g_arg_sent && (config ? g_config_seen && !strcmp(g_config_seen, config) : !g_config_seen);
}

/* A bound build receives the interned private configuration itself, through a
 * pointer that is not the behavior's. */
static int private_config(const char *interned)
{
    return g_config_seen == interned && g_arg_seen != g_arg_sent;
}

static int seen_is(const char *const *expected, int count)
{
    if (g_seen_count != count) { printf("  seen %d entries, expected %d\n", g_seen_count, count); return 0; }
    for (int i = 0; i < count; i++) {
        const char *a = g_seen[i], *b = expected[i];
        if ((a == NULL) != (b == NULL) || (a && strcmp(a, b))) {
            printf("  entry %d: %s, expected %s\n", i, a ? a : "(null)", b ? b : "(null)");
            return 0;
        }
    }
    return 1;
}

static const char *const LIST[] = {"states/alertcycle", "states/behaviors", "states/cyberdemon_hell", "states/impcombat",
    SH_FSM_WEB_PLACEHOLDER, "smp/variant_a", "smp/alert_ab", "smp/variant_b", "smp/extra_a"};
static const char *const OWNED[] = {SH_FSM_WEB_PLACEHOLDER, "smp/variant_a", "smp/alert_ab", "smp/variant_b", "smp/extra_a"};
static const char *const FROM[] = {"states/alertcycle", "states/cyberdemon_hell"};
static const char *const TO_A[] = {"smp/alert_ab", "smp/variant_a"};
static const char *const TO_B[] = {"smp/alert_ab", "smp/variant_b"};
static const char *const EXTRA_A[] = {"smp/extra_a"};
static const char *const OWNERS_A[] = {"behaviors/a"};
static const char *const OWNERS_B[] = {"behaviors/b"};

static const char *const NATIVE[] = {"states/alertcycle", "states/alertcycle", "states/behaviors", "states/cyberdemon_hell",
    "states/impcombat", SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER};
static const char *const BOUND_A[] = {"smp/alert_ab", "smp/alert_ab", "states/behaviors", "smp/variant_a",
    "states/impcombat", SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, "smp/extra_a"};
static const char *const BOUND_B[] = {"smp/alert_ab", "smp/alert_ab", "states/behaviors", "smp/variant_b",
    "states/impcombat", SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER};

static sh_fsm_web_binding g_ab[2];
static sh_fsm_web_table table_ab(int reversed)
{
    sh_fsm_web_binding a = {CFG_A, OWNERS_A, 1, FROM, TO_A, 2, EXTRA_A, 1};
    sh_fsm_web_binding b = {CFG_B, OWNERS_B, 1, FROM, TO_B, 2, NULL, 0};
    g_ab[0] = reversed ? b : a; g_ab[1] = reversed ? a : b;
    return (sh_fsm_web_table){g_ab, 2, OWNED, 5};
}

static void fresh(void)
{
    sh_fsm_web_test_reset();
    CHECK(sh_fsm_web_test_live() == 0);
    g_mode = PLAIN;
    list_set(LIST, 9);
}

static void test_passthrough(void)
{
    const char *const raw[] = {"states/alertcycle", "states/alertcycle", "states/behaviors", "states/cyberdemon_hell",
        "states/impcombat", SH_FSM_WEB_PLACEHOLDER, "smp/variant_a", "smp/alert_ab", "smp/variant_b", "smp/extra_a"};
    fresh();
    /* Before any product declaration is issued the list and argument are the engine's. */
    CHECK(build("behaviors/a") == 7 && seen_is(raw, 10) && native_config("cyberdemon_hell"));
    CHECK(sh_fsm_web_publish(NULL) && build("behaviors/a") == 7 && seen_is(raw, 10) && native_config("cyberdemon_hell"));
    CHECK(sh_fsm_web_test_depth() == 0);
}

static void test_views(int reversed)
{
    sh_fsm_web_table t = table_ab(reversed);
    fresh();
    CHECK(sh_fsm_web_publish(&t));
    /* Every other behavior keeps its argument and the stock list. */
    CHECK(build("behaviors/native") == 7 && seen_is(NATIVE, 10) && native_config("cyberdemon_hell"));
    CHECK(build("behaviors/imp") == 7 && seen_is(NATIVE, 10) && native_config("imp"));
    CHECK(build(NULL) == 7 && seen_is(NATIVE, 10) && native_config(NULL));
    /* Bound behaviors get their own web and the interned private configuration
     * their first declaration carries. */
    CHECK(build("behaviors/a") == 7 && seen_is(BOUND_A, 10) && private_config(CFG_A));
    CHECK(build("behaviors/b") == 7 && seen_is(BOUND_B, 10) && private_config(CFG_B));
}

static void test_activation_refusals(void)
{
    sh_fsm_web_table t = table_ab(0);
    const char *const to_missing[] = {"smp/alert_ab", "smp/not_registered"};
    sh_fsm_web_binding missing = {CFG_A, OWNERS_A, 1, FROM, to_missing, 2, NULL, 0};
    sh_fsm_web_table missing_table = {&missing, 1, OWNED, 5};
    const char *const extra_missing[] = {"smp/not_registered"};
    sh_fsm_web_binding no_extra = {CFG_A, OWNERS_A, 1, FROM, TO_A, 2, extra_missing, 1};
    sh_fsm_web_table no_extra_table = {&no_extra, 1, OWNED, 5};
    sh_fsm_web_binding uncarried = {"smp_cfg_z", OWNERS_A, 1, FROM, TO_A, 2, NULL, 0};
    sh_fsm_web_table uncarried_table = {&uncarried, 1, OWNED, 5};
    const char *const one[] = {"states/cyberdemon_hell"}, *const one_to[] = {"smp/variant_a"};
    sh_fsm_web_binding no_first = {CFG_A, OWNERS_A, 1, one, one_to, 1, NULL, 0};
    sh_fsm_web_table no_first_table = {&no_first, 1, OWNED, 5};
    sh_fsm_web_binding twice[2] = {{CFG_A, OWNERS_A, 1, FROM, TO_A, 2, NULL, 0}, {CFG_A, OWNERS_B, 1, FROM, TO_B, 2, NULL, 0}};
    sh_fsm_web_table duplicate = {twice, 2, OWNED, 5};
    sh_fsm_web_binding shared[2] = {{CFG_A, OWNERS_A, 1, FROM, TO_A, 2, NULL, 0}, {CFG_B, OWNERS_A, 1, FROM, TO_B, 2, NULL, 0}};
    sh_fsm_web_table shared_table = {shared, 2, OWNED, 5};
    sh_fsm_web_binding nobody = {CFG_A, NULL, 0, FROM, TO_A, 2, NULL, 0};
    sh_fsm_web_table nobody_table = {&nobody, 1, OWNED, 5};
    fresh();
    /* Product declarations without a registered placeholder can never be excluded. */
    decl_set(4, "smp/not_the_placeholder", 0, 1);
    list_generation(9);
    CHECK(!sh_fsm_web_publish(&t));
    /* Refused, and already issued: no build may see them unfiltered. */
    CHECK(build("behaviors/native") == 7 && g_seen_count == 10 && !g_seen[0] && !g_seen[6]);
    /* A placeholder whose graph carries any layer bit could meet some configuration. */
    decl_set(4, SH_FSM_WEB_PLACEHOLDER, 0x20000000u, 1);
    CHECK(!sh_fsm_web_publish(&t));
    /* So could nothing that is not a genuine graph declaration. */
    decl_set(4, SH_FSM_WEB_PLACEHOLDER, 0, 0);
    CHECK(!sh_fsm_web_publish(&t));
    decl_set(4, SH_FSM_WEB_PLACEHOLDER, 0, 1);
    CHECK(sh_fsm_web_publish(&t));
    long live = sh_fsm_web_test_live();
    /* Each refusal leaves the previous coherent bindings active and unchanged. */
    CHECK(!sh_fsm_web_publish(&missing_table));
    CHECK(!sh_fsm_web_publish(&no_extra_table));
    CHECK(!sh_fsm_web_publish(&uncarried_table));   /* the first replacement lacks the configuration */
    CHECK(!sh_fsm_web_publish(&no_first_table));    /* nothing replaces the first declaration */
    CHECK(!sh_fsm_web_publish(&duplicate));         /* one web per configuration */
    CHECK(!sh_fsm_web_publish(&shared_table));      /* one web per behavior */
    CHECK(!sh_fsm_web_publish(&nobody_table));      /* a web some behavior builds */
    CHECK(sh_fsm_web_test_live() == live);
    CHECK(build("behaviors/a") == 7 && seen_is(BOUND_A, 10) && private_config(CFG_A));
    CHECK(build("behaviors/b") == 7 && seen_is(BOUND_B, 10) && private_config(CFG_B));
}

static void test_release_to_native(void)
{
    sh_fsm_web_table t = table_ab(0);
    const char *const only_a[] = {"smp/variant_a", "smp/alert_ab", "smp/extra_a"};
    sh_fsm_web_table variant_only = {g_ab, 1, only_a, 3};
    fresh();
    CHECK(sh_fsm_web_publish(&t));
    /* Release: the bindings go, but the engine still lists the retired private
     * declarations, and every build keeps them out with its own configuration. */
    CHECK(sh_fsm_web_publish(NULL));
    CHECK(build("behaviors/a") == 7 && seen_is(NATIVE, 10) && native_config("cyberdemon_hell"));
    CHECK(build("behaviors/native") == 7 && seen_is(NATIVE, 10) && native_config("cyberdemon_hell"));
    /* A later publication naming fewer declarations still excludes the rest. */
    CHECK(sh_fsm_web_publish(&variant_only));
    CHECK(build("behaviors/b") == 7 && seen_is(NATIVE, 10) && native_config("cyberdemon_hell"));
    CHECK(build("behaviors/a") == 7 && seen_is(BOUND_A, 10) && private_config(CFG_A));
    /* Retirement cannot drop the placeholder while hidden entries remain: a
     * release that cannot resolve is refused and the old bindings stay. */
    decl_set(4, SH_FSM_WEB_PLACEHOLDER, 1, 1);
    list_generation(9);
    CHECK(!sh_fsm_web_publish(NULL));
    decl_set(4, SH_FSM_WEB_PLACEHOLDER, 0, 1);
    list_generation(9);
    CHECK(build("behaviors/a") == 7 && seen_is(BOUND_A, 10) && private_config(CFG_A));
}

static void test_failed_candidate(void)
{
    const char *const l0[] = {"states/alertcycle", "states/behaviors", "states/cyberdemon_hell", "states/impcombat",
        SH_FSM_WEB_PLACEHOLDER, "smp/variant_a", "smp/alert_ab", "smp/extra_a"};
    const char *const l1[] = {"states/alertcycle", "states/behaviors", "states/cyberdemon_hell", "states/impcombat",
        SH_FSM_WEB_PLACEHOLDER, "smp/variant_a", "smp/alert_ab", "smp/extra_a", "smp/variant_b"};
    const char *const l2[] = {"states/alertcycle", "states/behaviors", "states/cyberdemon_hell", "states/impcombat",
        SH_FSM_WEB_PLACEHOLDER, "smp/variant_a", "smp/alert_ab", "smp/extra_a", "smp/variant_b", "smp/variant_c"};
    const char *const owned_a[] = {"smp/variant_a", "smp/alert_ab", "smp/extra_a"};
    sh_fsm_web_binding a = {CFG_A, OWNERS_A, 1, FROM, TO_A, 2, EXTRA_A, 1};
    sh_fsm_web_table table_a = {&a, 1, owned_a, 3};
    const char *const missing_to[] = {"smp/alert_ab", "smp/missing"};
    const char *const b_names[] = {"smp/variant_b"};
    const char *const c_names[] = {"smp/variant_c"};
    sh_fsm_web_binding with_b[2] = {{CFG_A, OWNERS_A, 1, FROM, TO_A, 2, EXTRA_A, 1},
                                    {CFG_B, OWNERS_B, 1, FROM, missing_to, 2, NULL, 0}};
    sh_fsm_web_table table_b = {with_b, 2, b_names, 1};
    sh_fsm_web_table table_c = {with_b, 2, c_names, 1};
    const char *const native1[] = {"states/alertcycle", "states/alertcycle", "states/behaviors", "states/cyberdemon_hell",
        "states/impcombat", SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER,
        SH_FSM_WEB_PLACEHOLDER};
    const char *const a1[] = {"smp/alert_ab", "smp/alert_ab", "states/behaviors", "smp/variant_a",
        "states/impcombat", SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, "smp/extra_a",
        SH_FSM_WEB_PLACEHOLDER};
    fresh();
    list_set(l0, 8);
    CHECK(sh_fsm_web_publish(&table_a));
    /* B is issued, then registered (the list grows), then its publication is
     * refused for a different missing replacement. A stays bound; B is kept
     * out of the native web and A's web on the next generation, and B's
     * behavior builds natively. */
    CHECK(sh_fsm_web_issue(b_names, 1));
    list_set(l1, 9);
    CHECK(!sh_fsm_web_publish(&table_b));
    CHECK(build("behaviors/native") == 7 && seen_is(native1, 10) && native_config("cyberdemon_hell"));
    CHECK(build("behaviors/a") == 7 && seen_is(a1, 10) && private_config(CFG_A));
    CHECK(build("behaviors/b") == 7 && seen_is(native1, 10) && native_config("cyberdemon_hell"));
    /* C is registered and named only by a refused publication: naming it issues
     * it, so the next build keeps it out as well. */
    list_set(l2, 10);
    CHECK(!sh_fsm_web_publish(&table_c));
    CHECK(build("behaviors/native") == 7 && g_seen_count == 11 && !strcmp(g_seen[10], SH_FSM_WEB_PLACEHOLDER));
    CHECK(build("behaviors/a") == 7 && !strcmp(g_seen[3], "smp/variant_a") && !strcmp(g_seen[10], SH_FSM_WEB_PLACEHOLDER));
    /* Clearing every binding keeps the placeholder in use while issued entries
     * remain listed, through a later reload of the same resources. */
    CHECK(sh_fsm_web_publish(NULL));
    list_set(l2, 10);
    CHECK(build("behaviors/a") == 7 && g_seen_count == 11 && !strcmp(g_seen[3], "states/cyberdemon_hell"));
    CHECK(native_config("cyberdemon_hell"));
    for (int i = 5; i < 11; i++) CHECK(!strcmp(g_seen[i], SH_FSM_WEB_PLACEHOLDER));
}

static void test_generations(void)
{
    sh_fsm_web_table t = table_ab(0);
    const char *const grown[] = {"states/alertcycle", "states/behaviors", "states/cyberdemon_hell", "states/impcombat",
        SH_FSM_WEB_PLACEHOLDER, "smp/variant_a", "smp/alert_ab", "smp/variant_b", "smp/extra_a", "states/zombiecombat", "smp/variant_a2"};
    const char *const refused[] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
    fresh();
    CHECK(sh_fsm_web_publish(&t));
    long live = sh_fsm_web_test_live();
    /* Two more declarations registered after publication: a stock one stays
     * visible, and so does a product one nothing has issued. The next build
     * resolves the new generation. */
    list_set(grown, 11);
    CHECK(build("behaviors/a") == 7 && private_config(CFG_A));
    CHECK(g_seen_count == 12 && !strcmp(g_seen[3], "smp/variant_a") && !strcmp(g_seen[10], "states/zombiecombat"));
    CHECK(!strcmp(g_seen[11], "smp/variant_a2"));
    CHECK(sh_fsm_web_test_live() == live);   /* the old resolution was replaced, not leaked */
    /* A generation that cannot be resolved (the placeholder is gone while
     * hidden entries remain) gives builds no graphs at all. */
    decl_set(4, "smp/gone", 0, 1);
    list_generation(11);
    CHECK(build("behaviors/native") == 7 && seen_is(refused, 12));
    CHECK(build("behaviors/a") == 7 && seen_is(refused, 12));
    /* Once the list resolves again, builds recover. */
    decl_set(4, SH_FSM_WEB_PLACEHOLDER, 0, 1);
    list_generation(11);
    CHECK(build("behaviors/b") == 7 && !strcmp(g_seen[3], "smp/variant_b") && !strcmp(g_seen[11], "smp/variant_a2"));
    CHECK(private_config(CFG_B) && sh_fsm_web_test_live() == live);
}

static void test_scoping(void)
{
    sh_fsm_web_table t = table_ab(0);
    const char *const to_swap[] = {"smp/alert_ab", "smp/variant_b"};
    sh_fsm_web_binding swapped = {CFG_A, OWNERS_A, 1, FROM, to_swap, 2, NULL, 0};
    sh_fsm_web_table swap = {&swapped, 1, OWNED, 5};
    const char *const now[] = {"smp/alert_ab", "smp/alert_ab", "states/behaviors", "smp/variant_b",
        "states/impcombat", SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER, SH_FSM_WEB_PLACEHOLDER};
    fresh();
    CHECK(sh_fsm_web_publish(&t));
    long live = sh_fsm_web_test_live();
    /* A native build nested inside owner A's build; A's view and configuration
     * continue after it. */
    g_mode = NEST_NATIVE;
    CHECK(build("behaviors/a") == 7 && seen_is(BOUND_A, 10) && g_config_seen == CFG_A);
    g_mode = PLAIN;
    CHECK(sh_fsm_web_test_depth() == 0 && sh_fsm_web_test_live() == live);
    /* Nesting has no fixed depth: 20 alternating levels keep their own views. */
    g_mode = DEEP; g_deep = 0; g_deep_max = 20; g_deep_ok = 1;
    CHECK(build("behaviors/a") == 7);
    g_mode = PLAIN;
    CHECK(g_deep_ok && g_deep == 21 && sh_fsm_web_test_depth() == 0 && sh_fsm_web_test_live() == live);
    /* An exception unwinding through the builder restores the chain and the pin. */
    g_mode = RAISE;
    __try { build("behaviors/a"); CHECK(0); }
    __except (GetExceptionCode() == 0xE0FF0001u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
    g_mode = PLAIN;
    CHECK(sh_fsm_web_test_depth() == 0 && sh_fsm_web_test_live() == live);
    CHECK(build("behaviors/b") == 7 && seen_is(BOUND_B, 10) && private_config(CFG_B));
    /* A table published mid-build: the build finishes on its pinned views,
     * which stay allocated until it returns; the next build uses the new table. */
    g_swap = &swap; g_mode = REPUBLISH;
    CHECK(build("behaviors/a") == 7 && seen_is(BOUND_A, 10) && private_config(CFG_A));
    g_mode = PLAIN;
    CHECK(sh_fsm_web_test_live() == live);
    CHECK(build("behaviors/a") == 7 && seen_is(now, 10) && private_config(CFG_A));
    CHECK(build("behaviors/b") == 7 && seen_is(NATIVE, 10) && native_config("cyberdemon_hell"));
}

/* Synthetic call sites carrying the signed byte forms; never executed. The
 * installed relays are executed with the native convention. */
static uintptr_t relay_builder(void *manager, void *config) { (void)manager; return config ? 11 : 12; }

static void put(unsigned char *at, const char *hex)
{
    for (const char *p = hex; *p; p += (p[2] ? 3 : 2)) {
        char byte[3] = {p[0], p[1], 0};
        *at++ = (unsigned char)strtoul(byte, NULL, 16);
    }
}
static void put_rel(unsigned char *at, uintptr_t base, uintptr_t target)
{
    int32_t d = (int32_t)((intptr_t)target - (intptr_t)base);
    memcpy(at, &d, sizeof(d));
}

static void test_install(void)
{
    sig_result results[5] = {
        {"FsmWebBuilder", SIG_OK, 0, 0}, {"FsmWebBuildCall", SIG_OK, 0, 0},
        {"FsmWebDeclFirstCall", SIG_OK, 0, 0}, {"FsmWebDeclAtCall", SIG_OK, 0, 0}, {"FsmWebCacheFlag", SIG_OK, 0, 0}};
    unsigned char *code = VirtualAlloc(NULL, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    unsigned char *builder, *caller, *first, *at, *gate, *get, *list, *flag;
    unsigned char before[0x3000];
    CHECK(code != NULL);
    if (!code) return;
    fresh();
    memset(code, 0xcc, 0x3000);
    builder = code + 0x1000; caller = code + 0x100; get = code + 0x2800;
    list = code + 0x2900; flag = code + 0x2a00;
    gate = builder + 0x4b; first = builder + 0x7e; at = builder + 0x1fa;
    put(builder, "48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57");
    put(caller, "48 8B 93 20 C1 00 00 48 81 C2 58 05 00 00 48 8D 8B B8 23 01 00 E8");
    put_rel(caller + 22, (uintptr_t)caller + 26, (uintptr_t)builder); put(caller + 26, "84 C0 75 2D");
    put(first, "48 8B 03 49 89 45 00 33 D2 48 8D 0D"); put_rel(first + 12, (uintptr_t)first + 16, (uintptr_t)list);
    put(first + 16, "E8"); put_rel(first + 17, (uintptr_t)first + 21, (uintptr_t)get); put(first + 21, "4C 8B F0");
    put(at, "66 0F 1F 44 00 00 8B D6 48 8D 0D"); put_rel(at + 11, (uintptr_t)at + 15, (uintptr_t)list);
    put(at + 15, "E8"); put_rel(at + 16, (uintptr_t)at + 20, (uintptr_t)get); put(at + 20, "4C 8B F8");
    put(gate, "48 89 45 A0 83 3D"); put_rel(gate + 6, (uintptr_t)gate + 11, (uintptr_t)flag); put(gate + 10, "00 40 0F 95 C7");
    memset(flag, 0, 8);
    results[0].addr = (uintptr_t)builder; results[1].addr = (uintptr_t)caller;
    results[2].addr = (uintptr_t)first; results[3].addr = (uintptr_t)at; results[4].addr = (uintptr_t)gate;
    /* Refusals leave every byte unchanged. */
    flag[0] = 1; memcpy(before, code, sizeof(before));
    CHECK(!sh_fsm_web_install(NULL, results, 5) && !memcmp(before, code, sizeof(before)));
    flag[0] = 0; memcpy(before, code, sizeof(before));
    results[3].addr++;
    CHECK(!sh_fsm_web_install(NULL, results, 5) && !memcmp(before, code, sizeof(before)));
    results[3].addr--;
    put_rel(first + 17, (uintptr_t)first + 21, (uintptr_t)get + 16); memcpy(before, code, sizeof(before));
    CHECK(!sh_fsm_web_install(NULL, results, 5) && !memcmp(before, code, sizeof(before)));
    put_rel(first + 17, (uintptr_t)first + 21, (uintptr_t)get); memcpy(before, code, sizeof(before));
    results[1].status = SIG_AMBIGUOUS;
    CHECK(!sh_fsm_web_install(NULL, results, 5) && !memcmp(before, code, sizeof(before)));
    results[1].status = SIG_OK;
    /* Positive: only the three call operands change, each to a relay. */
    CHECK(sh_fsm_web_install(NULL, results, 5) && sh_fsm_web_installed());
    {
        size_t changed = 0;
        int32_t d;
        unsigned char *relay_build = sh_fsm_web_test_relay(0), *relay_resolve = sh_fsm_web_test_relay(1);
        for (size_t i = 0; i < sizeof(before); i++) changed += before[i] != code[i];
        CHECK(changed >= 3 && changed <= 12);
        memcpy(&d, caller + 22, 4); CHECK(caller + 26 + d == relay_build);
        memcpy(&d, first + 17, 4); CHECK(first + 21 + d == relay_resolve);
        memcpy(&d, at + 16, 4); CHECK(at + 20 + d == relay_resolve);
        /* Execute the relays: the build wrapper and the list resolver keep the
         * native argument registers and return value. */
        sh_fsm_web_test_bind(fake_get, relay_builder, g_list);
        list_set(LIST, 9);
        uintptr_t (*run)(void *, void *) = (uintptr_t (*)(void *, void *))relay_build;
        void *(*resolve)(void *, int) = (void *(*)(void *, int))relay_resolve;
        CHECK(run(NULL, owner("behaviors/imp")) == 11 && run(NULL, NULL) == 12);
        CHECK(decl_name(resolve(g_list, 2)) && !strcmp(decl_name(resolve(g_list, 2)), "states/cyberdemon_hell"));
        CHECK(sh_fsm_web_test_depth() == 0);
    }
    CHECK(sh_fsm_web_test_uninstall() && !memcmp(before, code, sizeof(before)) && !sh_fsm_web_installed());
    VirtualFree(code, 0, MEM_RELEASE);
    sh_fsm_web_test_bind(fake_get, fake_builder, g_list);
}

int main(void)
{
    sh_fsm_web_test_bind(fake_get, fake_builder, g_list);
    test_passthrough();
    test_views(0);
    test_views(1);
    test_activation_refusals();
    test_release_to_native();
    test_failed_candidate();
    test_generations();
    test_scoping();
    test_install();
    sh_fsm_web_test_reset();
    CHECK(sh_fsm_web_test_live() == 0);
    if (g_failures) { printf("fsm_web_test: %d failure(s)\n", g_failures); return 1; }
    printf("fsm_web_test: all checks passed\n");
    return 0;
}
