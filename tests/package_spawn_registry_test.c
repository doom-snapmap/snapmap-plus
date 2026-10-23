/* Selection membership and first-match behavior must survive key lowering. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_spawn_registry.h"

int sh_decl_native_schema_enum_constants(sh_decl_native_schema *schema, const char *name,
    int (*visit)(void *, const char *, uint32_t), void *context)
{ (void)schema; (void)name; (void)visit; (void)context; return 0; }

static const sh_package_key_space space = {
    0x4007ffffu, SH_PACKAGE_KEY_UNNAMED,
    {{"IMP", 4}, {"GROUP", 131072}, {"ALL", 0xffffffffu}}, 3
};
static sh_package_spawn_registry read_registry(const char *text)
{
    sh_package_spawn_registry out = {0}; char error[256] = "";
    int result = sh_package_spawn_registry_read((sh_decl_source){text, strlen(text)}, &space,
        &out, error, sizeof(error));
    if (result != 1) fprintf(stderr, "%s\n", error);
    assert(result == 1); return out;
}
static size_t exact_modifier(const sh_package_spawn_registry *registry, uint32_t type)
{
    for (size_t i = 0; i < registry->modifier_count; i++)
        if (registry->modifiers[i].mask == type) return i;
    return SIZE_MAX;
}
typedef struct test_codes { sh_package_key_groups groups; sh_package_key_set taken; } test_codes;
static int group_code(void *context, const sh_package_key_set *members, uint32_t *code)
{
    test_codes *codes = context;
    *code = sh_package_key_groups_code(&codes->groups, NULL, &codes->taken, members);
    return *code ? 1 : 0;
}
static int exhausted_code(void *context, const sh_package_key_set *members, uint32_t *code)
{ (void)context; (void)members; *code = 123; return 0; }
static void test_native_membership(void)
{
    /* AI A's second row can cause its spawn, but only its first row controls
     * modifiers/filters. A historical high bit is an ordinary authored bit;
     * lowering must not interpret it as a compiled group marker. */
    sh_package_spawn_registry r = read_registry("{ edit = { aiTypeList = { num = 4; "
        "item[2] = { aiType = 16; entityDef = \"a\"; } "
        "item[0] = { aiType = \"IMP GROUP\"; entityDef = \"a\"; } "
        "item[3] = { aiType = 268435456; entityDef = \"c\"; } "
        "item[1] = { aiType = 8; entityDef = \"b\"; weight = 7; } } "
        "globalAIMods = { num = 3; item[0] = { aiType = 16; healthMod = 99; } "
        "item[1] = { aiType = \"GROUP\"; healthMod = 0.1; damageMod = 0.2; } "
        "item[2] = { aiType = \"ALL\"; healthMod = 0.5; } } } }");
    assert(r.count == 4 && r.modifier_count == 3 && !strcmp(r.rows[0].entity, "a"));
    assert(strstr(r.rows[1].body, "weight = 7") && strstr(r.modifiers[1].body, "damageMod = 0.2"));
    assert(sh_package_spawn_registry_type(&r, 2) == 0);
    assert(sh_package_spawn_registry_modifier(&r, 0) == 1 && sh_package_spawn_registry_modifier(&r, 2) == 1);
    assert(sh_package_spawn_registry_modifier(&r, 1) == 2 && sh_package_spawn_registry_modifier(&r, 3) == 2);
    uint32_t keys[4] = {524288, 1048576, 2097152, SH_PACKAGE_KEY_MARKER | 524288};
    test_codes codes = {0}; sh_package_key_encoder encoder = {group_code, &codes};
    for (size_t i = 0; i < 4; i++) assert(sh_package_key_set_add(&codes.taken, keys[i]));
    const uint32_t authored_bits[] = {4, 8, 16, 131072, 268435456};
    for (uint32_t bits = 0; bits < 32; bits++) {
        uint32_t mask = 0;
        for (size_t i = 0; i < 5; i++) if (bits >> i & 1) mask |= authored_bits[i];
        for (int role = SH_PACKAGE_SPAWN_REQUEST; role <= SH_PACKAGE_SPAWN_CLASSIFIER; role++) {
            sh_package_key_set got = {{0}};
            assert(sh_package_spawn_registry_keys(&r, keys, mask, role, &got));
            for (size_t row = 0; row < 4; row++) {
                int expected = !!(r.rows[row].mask & mask);
                if (role == SH_PACKAGE_SPAWN_CLASSIFIER && row == 2) expected = 0;
                assert(sh_package_key_set_has(&got, keys[row]) == expected);
            }
            uint32_t code = 0;
            assert(sh_package_spawn_registry_code(&r, keys, mask, role, &encoder, &code) == 1);
            /* Assert actual dispatch membership, not merely the emitted set. */
            for (size_t row = 0; row < 4; row++) {
                int expected = !!(r.rows[row].mask & mask);
                if (role == SH_PACKAGE_SPAWN_CLASSIFIER && row == 2) expected = 0;
                assert(sh_package_key_request_matches(&codes.groups, code, keys[row]) == expected);
            }
            assert(!sh_package_key_request_matches(&codes.groups, code, 4194304));
        }
    }
    sh_package_key_set keys_all = {{0}};
    assert(sh_package_spawn_registry_keys(&r, keys, UINT32_MAX, SH_PACKAGE_SPAWN_REQUEST, &keys_all));
    assert(sh_package_key_set_count(&keys_all) == 4);
    sh_package_key_groups groups = {0};
    uint32_t code = sh_package_key_groups_code(&groups, NULL, &keys_all, &keys_all);
    assert(code && sh_package_key_private_valid(code));
    for (size_t i = 0; i < 4; i++) assert(sh_package_key_request_matches(&groups, code, keys[i]));
    assert(!sh_package_key_request_matches(&groups, code, 4194304));
    sh_package_key_encoder exhausted = {exhausted_code, NULL};
    code = 99;
    assert(sh_package_spawn_registry_code(&r, keys, UINT32_MAX, SH_PACKAGE_SPAWN_REQUEST, &exhausted, &code) == 0);
    assert(code == 0); /* Exhaustion must never fall back to a peer's row. */
    sh_package_spawn_registry_free(&r);
}
static void test_empty_and_rejected(void)
{
    sh_package_spawn_registry r = read_registry("{ edit = { aiTypeList = { num = 0; } } }");
    sh_package_key_set got;
    assert(sh_package_spawn_registry_keys(&r, NULL, UINT32_MAX, SH_PACKAGE_SPAWN_REQUEST, &got));
    assert(!sh_package_key_set_count(&got)); sh_package_spawn_registry_free(&r);
    r = read_registry("{ edit = { aiTypeList = { num = -1; } globalAIMods = { num = -2147483648; } } }");
    assert(!r.count && !r.modifier_count); sh_package_spawn_registry_free(&r);
    const char *bad[] = {
        "{ edit = { aiTypeList = { num = 1; item[1] = { aiType = 4; entityDef = \"x\"; } } } }",
        "{ edit = { aiTypeList = { num = 1; item[0] = 2; } } }",
        "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = {}; } } } }",
        "{ edit = { aiTypeList = { num = 18446744073709551616; } } }"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        char error[256] = "";
        assert(sh_package_spawn_registry_read((sh_decl_source){bad[i], strlen(bad[i])}, &space,
            &r, error, sizeof(error)) == -1);
        assert(!r.rows && !r.modifiers && !r.count && !r.modifier_count && *error);
    }
    const char *plain = "{ edit = { health = 10; } }";
    assert(sh_package_spawn_registry_read((sh_decl_source){plain, strlen(plain)}, &space, &r, NULL, 0) == 0);
    r = read_registry("{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 4; entityDef = \"x\"; } } } }");
    uint32_t invalid = 4;
    memset(&got, 0xff, sizeof(got));
    assert(!sh_package_spawn_registry_keys(&r, &invalid, UINT32_MAX, SH_PACKAGE_SPAWN_REQUEST, &got));
    assert(!sh_package_key_set_count(&got));
    assert(sh_package_spawn_registry_modifier(&r, 0) == SIZE_MAX && sh_package_spawn_registry_type(&r, 1) == SIZE_MAX);
    sh_package_spawn_registry_free(&r);
}
static void test_native_defaults(void)
{
    const char *source = "{ edit = { aiTypeList = { num = 3; item[1] = { entityDef = \"idle\"; } "
        "item[2] = { aiType = 4; entityDef = \"active\"; } } globalAIMods = { num = 2; "
        "item[1] = { aiType = 4; damageMod = 2; } } } }";
    sh_package_spawn_registry rows = read_registry(source);
    assert(rows.count == 3 && rows.modifier_count == 2);
    assert(rows.rows[0].mask == 0 && !*rows.rows[0].entity &&
        sh_package_spawn_registry_type(&rows, 0) == SIZE_MAX);
    assert(rows.rows[1].mask == 0 && !strcmp(rows.rows[1].entity, "idle"));
    assert(rows.modifiers[0].mask == 0 && strstr(rows.modifiers[0].body, "healthMod = 1") &&
        strstr(rows.modifiers[0].body, "damageMod = 1"));
    assert(sh_package_spawn_registry_modifier(&rows, 2) == 1 &&
        strstr(rows.modifiers[1].body, "healthMod = 1") && strstr(rows.modifiers[1].body, "damageMod = 2"));
    size_t length = 0; char error[256] = "";
    char *prepared = NULL;
    assert(sh_package_spawn_registry_prepare((sh_decl_source){source, strlen(source)}, &prepared,
        &length, error, sizeof(error)) == 1 && prepared && length == strlen(prepared));
    sh_package_spawn_registry complete = read_registry(prepared);
    assert(sh_package_spawn_registry_equivalent(&rows, &complete));
    sh_decl_source tables[] = {{source, strlen(source)}};
    size_t composed_length = 0;
    char *composed = sh_package_spawn_registry_compose((sh_decl_source){prepared, length}, tables, 1,
        NULL, &composed_length, error, sizeof(error));
    assert(composed);
    sh_package_spawn_registry published = read_registry(composed);
    assert(sh_package_spawn_registry_equivalent(&rows, &published));
    free(prepared); free(composed); sh_package_spawn_registry_free(&rows);
    sh_package_spawn_registry_free(&complete); sh_package_spawn_registry_free(&published);
}
static void test_publication(void)
{
    const char *stock = "{ class = \"Conductor\"; edit = { other = 17; aiTypeList = { num = 1; "
        "item[0] = { aiType = 4; entityDef = \"stock\"; } } } }";
    const char *private = "{ edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = 524288; entityDef = \"private\"; } "
        "item[1] = { aiType = 524288; entityDef = \"private\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 524288; healthMod = 2; } } } }";
    sh_decl_source tables[] = {{private, strlen(private)}, {private, strlen(private)}};
    size_t length = 0; char error[256] = "";
    char *published = sh_package_spawn_registry_compose((sh_decl_source){stock, strlen(stock)},
        tables, 2, NULL, &length, error, sizeof(error));
    assert(published && length == strlen(published) && strstr(published, "other = 17"));
    sh_package_spawn_registry registry = read_registry(published);
    assert(registry.count == 3 && registry.modifier_count == 1);
    assert(!strcmp(registry.rows[0].entity, "private") && !strcmp(registry.rows[1].entity, "private"));
    assert(!strcmp(registry.rows[2].entity, "stock"));
    sh_package_spawn_registry_free(&registry);
    char *again = sh_package_spawn_registry_compose((sh_decl_source){published, length},
        tables, 2, NULL, &length, error, sizeof(error));
    assert(again && !strcmp(published, again)); free(again); free(published);
    const char *bad = "{ edit = { aiTypeList = { num = 1; item[1] = {} } } }";
    tables[0] = (sh_decl_source){bad, strlen(bad)};
    assert(!sh_package_spawn_registry_compose((sh_decl_source){stock, strlen(stock)},
        tables, 1, NULL, &length, error, sizeof(error)) && !length && *error);
}
static void test_classification_identity(void)
{
    const char *a = "{ edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = 4; entityDef = \"a\"; } item[1] = { aiType = 8; entityDef = \"b\"; } } "
        "globalAIMods = { num = 2; item[0] = { aiType = 4; healthMod = 2; } "
        "item[1] = { aiType = 8; healthMod = 3; } } } }";
    const char *reordered = "{ edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = 8; entityDef = \"b\"; } item[1] = { aiType = 4; entityDef = \"a\"; } } "
        "globalAIMods = { num = 2; item[0] = { aiType = 8; healthMod = 3; } "
        "item[1] = { aiType = 4; healthMod = 2; } } } }";
    const char *different_b = "{ edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = 16; entityDef = \"b\"; } item[1] = { aiType = 4; entityDef = \"a\"; } } "
        "globalAIMods = { num = 2; item[0] = { aiType = 16; healthMod = 99; } "
        "item[1] = { aiType = 4; healthMod = 2; } } } }";
    sh_package_spawn_registry first = read_registry(a), equal = read_registry(reordered), changed = read_registry(different_b);
    assert(sh_package_spawn_registry_equivalent(&first, &equal));
    assert(!sh_package_spawn_registry_equivalent(&first, &changed));
    char *projected[3]; size_t length; char error[256] = "";
    const char *sources[] = {a, reordered, different_b};
    for (size_t i = 0; i < 3; i++) {
        projected[i] = sh_package_spawn_registry_classification((sh_decl_source){sources[i], strlen(sources[i])},
            &space, "a", &length, error, sizeof(error));
        assert(projected[i] && length == strlen(projected[i]));
    }
    assert(!strcmp(projected[0], projected[1]) && !strcmp(projected[0], projected[2]));
    for (size_t i = 0; i < 3; i++) free(projected[i]);
    const char *repeat = "{ edit = { aiTypeList = { num = 2; "
        "item[0] = { aiType = 4; entityDef = \"a\"; } item[1] = { aiType = 8; entityDef = \"a\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 8; healthMod = 99; } } } }";
    char *only_first = sh_package_spawn_registry_classification((sh_decl_source){repeat, strlen(repeat)},
        &space, "a", &length, error, sizeof(error));
    assert(only_first);
    sh_package_spawn_registry projected_first = read_registry(only_first);
    assert(projected_first.count == 1 && projected_first.rows[0].mask == 4 && projected_first.modifier_count == 0);
    free(only_first); sh_package_spawn_registry_free(&projected_first);
    sh_package_spawn_registry_free(&first); sh_package_spawn_registry_free(&equal); sh_package_spawn_registry_free(&changed);
}
static void test_composed_first_modifiers(void)
{
    const char *base = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 4; entityDef = \"stock\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 4; healthMod = 99; } } } }";
    for (int grouped = 0; grouped < 2; grouped++) for (int reversed = 0; reversed < 2; reversed++) {
        sh_package_key_groups groups = {0}; sh_package_key_set members = {{0}};
        uint32_t first_key = grouped ? sh_package_key_private(9) : 524288u;
        uint32_t second_key = grouped ? sh_package_key_private(10) : 1048576u;
        uint32_t both = first_key | second_key;
        if (grouped) {
            both = sh_package_key_private(20);
            assert(sh_package_key_set_add(&members, first_key) && sh_package_key_set_add(&members, second_key));
            assert(sh_package_key_groups_define(&groups, both, &members));
        }
        char a[512], b[512];
        snprintf(a, sizeof(a), "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = %u; entityDef = \"a\"; } } "
            "globalAIMods = { num = 1; item[0] = { aiType = %u; healthMod = 2; } } } }", first_key | 4u, first_key);
        snprintf(b, sizeof(b), "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = %u; entityDef = \"b\"; } } "
            "globalAIMods = { num = 1; item[0] = { aiType = %u; healthMod = 3; } } } }", both | 8u, second_key);
        sh_decl_source tables[] = {{reversed ? b : a, strlen(reversed ? b : a)}, {reversed ? a : b, strlen(reversed ? a : b)}};
        char error[256] = ""; size_t length = 0;
        char *text = sh_package_spawn_registry_compose((sh_decl_source){base, strlen(base)}, tables, 2,
            &groups, &length, error, sizeof(error));
        assert(text);
        sh_package_spawn_registry out = read_registry(text);
        for (size_t i = 0; i < out.count; i++) {
            size_t first;
            for (first = 0; first < out.modifier_count; first++)
                if ((out.rows[i].mask & SH_PACKAGE_KEY_PRIVATE) ? out.rows[i].mask == out.modifiers[first].mask :
                    sh_package_key_modifier_matches(&groups, out.rows[i].mask, out.modifiers[first].mask)) break;
            const char *expected = !strcmp(out.rows[i].entity, "a") ? "healthMod = 2" :
                !strcmp(out.rows[i].entity, "b") ? "healthMod = 3" : "healthMod = 99";
            if (first == out.modifier_count || !strstr(out.modifiers[first].body, expected))
                fprintf(stderr, "composed first modifier for %s: expected %s\n", out.rows[i].entity, expected);
            assert(first < out.modifier_count && strstr(out.modifiers[first].body, expected));
        }
        assert(out.count == 3);
        size_t again_length = 0;
        char *again = sh_package_spawn_registry_compose((sh_decl_source){text, length}, tables, 2,
            &groups, &again_length, error, sizeof(error));
        assert(again && length == again_length && !strcmp(text, again));
        free(again); free(text); sh_package_spawn_registry_free(&out);
    }
}
/* Exact retained D8 input: one classifier and its registry wrote the same
 * native 1/1 policy in opposite field orders. This is a two-row cycle in the
 * old string-based ordering graph, independent of the group dictionary. */
static void test_retained_field_order_cycle(void)
{
    const char *a = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = \"310378496\"; entityDef = \"actor\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = \"310378496\"; healthMod = 1; damageMod = 1; } } } }";
    const char *b = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = \"310378496\"; entityDef = \"actor\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = \"310378496\"; damageMod = 1; healthMod = 1; } } } }";
    for (int reversed = 0; reversed < 2; reversed++) {
        sh_decl_source tables[] = {{reversed ? b : a, strlen(a)}, {reversed ? a : b, strlen(b)}};
        const char *base = "{ edit = { aiTypeList = { num = 0; } globalAIMods = { num = 0; } } }";
        char error[256] = ""; size_t length = 0;
        char *text = sh_package_spawn_registry_compose((sh_decl_source){base, strlen(base)}, tables, 2,
            NULL, &length, error, sizeof(error));
        if (!text) fprintf(stderr, "retained field-order cycle: %s\n", error);
        assert(text);
        sh_package_spawn_registry out = read_registry(text);
        size_t modifier = exact_modifier(&out, 310378496u);
        assert(out.count == 1 && modifier != SIZE_MAX && out.modifier_count == 1);
        assert(strstr(out.modifiers[modifier].body, "healthMod = 1") && strstr(out.modifiers[modifier].body, "damageMod = 1"));
        free(text); sh_package_spawn_registry_free(&out);
    }
}
/* Three pairwise-overlapping private classification sets select different
 * broad policies. No ordering of intersection predicates can preserve them:
 * every policy meets every type. Exact classification dispatch can. */
static void test_different_policy_cycle(void)
{
    uint32_t keys[] = {524288u, 1048576u, 2097152u};
    uint32_t types[] = {keys[0] | keys[1] | 4u, keys[1] | keys[2] | 8u, keys[2] | keys[0] | 16u};
    uint32_t broad = keys[0] | keys[1] | keys[2];
    const char *base = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 4; entityDef = \"stock\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 4; healthMod = 9; damageMod = 1; } } } }";
    char source[3][768];
    for (size_t i = 0; i < 3; i++) snprintf(source[i], sizeof(source[i]),
        "{ edit = { aiTypeList = { num = 2; item[0] = { aiType = \"%u\"; entityDef = \"actor%zu\"; } "
        "item[1] = { aiType = \"%u\"; entityDef = \"actor%zu\"; weight = 7; } } globalAIMods = { num = 1; "
        "item[0] = { aiType = \"%u\"; healthMod = %zu; damageMod = 1; } } } }",
        types[i], i, types[i], i, broad, i + 2);
    for (int reversed = 0; reversed < 2; reversed++) {
        sh_decl_source tables[3];
        for (size_t i = 0; i < 3; i++) { const char *s = source[reversed ? 2 - i : i]; tables[i] = (sh_decl_source){s, strlen(s)}; }
        char error[256] = ""; size_t length = 0;
        char *text = sh_package_spawn_registry_compose((sh_decl_source){base, strlen(base)}, tables, 3,
            NULL, &length, error, sizeof(error));
        if (!text) fprintf(stderr, "different policy cycle: %s\n", error);
        assert(text);
        sh_package_spawn_registry out = read_registry(text);
        assert(out.count == 7);
        for (size_t i = 0; i < 3; i++) {
            size_t modifier = exact_modifier(&out, types[i]); char expected[32];
            snprintf(expected, sizeof(expected), "healthMod = %zu", i + 2);
            assert(modifier != SIZE_MAX && strstr(out.modifiers[modifier].body, expected));
            assert(out.modifiers[modifier].mask == types[i]);
        }
        size_t again_length = 0;
        char *again = sh_package_spawn_registry_compose((sh_decl_source){text, length}, tables, 3,
            NULL, &again_length, error, sizeof(error));
        assert(again && length == again_length && !strcmp(text, again));
        free(again); free(text); sh_package_spawn_registry_free(&out);
    }
}
static void test_classifier_policy_projection(void)
{
    const char *base = "{ edit = { aiTypeList = { num = 0; } globalAIMods = { num = 0; } } }";
    const char *named = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 524292; entityDef = \"named\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 4; healthMod = 2; damageMod = 1; } } } }";
    const char *all = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 1048584; entityDef = \"all\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = -1; healthMod = 3; damageMod = 1; } } } }";
    const char *historical = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 2097156; entityDef = \"old\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 4; healthMod = 5; damageMod = 1; } } } }";
    const char *broad = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 524292; entityDef = \"named\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 524292; healthMod = 99; damageMod = 1; } } } }";
    sh_decl_source tables[] = {{broad, strlen(broad)}, {historical, strlen(historical)},
        {all, strlen(all)}, {named, strlen(named)}};
    unsigned char flags[] = {0, 1, 1, 1};
    sh_package_key_groups groups = {0}; groups.historical = 2097152u;
    char error[256] = ""; size_t length = 0;
    char *text = sh_package_spawn_registry_compose_classified((sh_decl_source){base, strlen(base)}, tables,
        flags, 4, &groups, &length, error, sizeof(error));
    assert(text);
    sh_package_spawn_registry out = read_registry(text);
    assert(out.modifiers[0].mask == 524292 && strstr(out.modifiers[0].body, "healthMod = 2"));
    assert(out.modifiers[1].mask == 1048584 && strstr(out.modifiers[1].body, "healthMod = 3"));
    assert(exact_modifier(&out, 2097156) == SIZE_MAX);
    size_t broad_at = SIZE_MAX;
    for (size_t i = 2; i < out.modifier_count; i++)
        if (out.modifiers[i].mask == 524292 && strstr(out.modifiers[i].body, "healthMod = 99")) broad_at = i;
    assert(broad_at != SIZE_MAX);
    free(text); sh_package_spawn_registry_free(&out);

    const char *changed = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 524292; entityDef = \"other\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 4; healthMod = 3; damageMod = 1; } } } }";
    tables[0] = (sh_decl_source){changed, strlen(changed)}; flags[0] = 1;
    assert(!sh_package_spawn_registry_compose_classified((sh_decl_source){base, strlen(base)}, tables,
        flags, 4, &groups, &length, error, sizeof(error)));
    assert(strstr(error, "classification 524292 has different modifier policies"));
    const char *absent = "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = 524292; entityDef = \"absent\"; } } "
        "globalAIMods = { num = 0; } } }";
    tables[0] = (sh_decl_source){absent, strlen(absent)};
    assert(!sh_package_spawn_registry_compose_classified((sh_decl_source){base, strlen(base)}, tables,
        flags, 4, &groups, &length, error, sizeof(error)));
    assert(strstr(error, "different modifier policies"));
}
static void test_high_bit_projection(void)
{
    uint32_t type = 0x80080004u;
    char source[512];
    snprintf(source, sizeof(source), "{ edit = { aiTypeList = { num = 1; item[0] = { aiType = %d; entityDef = \"high\"; } } "
        "globalAIMods = { num = 1; item[0] = { aiType = 524288; healthMod = 2; damageMod = 1; } } } }", (int)(int32_t)type);
    sh_decl_source table = {source, strlen(source)}; char error[256] = ""; size_t length = 0;
    char *text = sh_package_spawn_registry_compose(table, &table, 1, NULL, &length, error, sizeof(error));
    assert(text);
    sh_package_spawn_registry out = read_registry(text);
    assert(out.modifiers[0].mask == type && strstr(out.modifiers[0].body, "aiType = \"-2146959356\""));
    free(text); sh_package_spawn_registry_free(&out);
}
int main(void)
{
    test_native_membership(); test_empty_and_rejected(); test_publication(); test_classification_identity();
    test_retained_field_order_cycle(); test_different_policy_cycle();
    test_composed_first_modifiers();
    test_classifier_policy_projection(); test_high_bit_projection();
    test_native_defaults();
    puts("package spawn registry tests passed"); return 0;
}
