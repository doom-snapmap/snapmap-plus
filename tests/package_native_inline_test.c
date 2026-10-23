#include "../src/backend/package_native_inline.h"
#include "../src/backend/config_json.h"
#include "../src/backend/decl_native_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

typedef struct fixture_decl { const char *name, *text; int served; } fixture_decl;
static const fixture_decl decls[] = {
    {"snapmaps/props/base", "{ class = \"idProp\"; edit = { health = 10; speed = 1; model = \"md6/base\";"
        " list = { num = 3; item[0] = 1; item[1] = 2; item[2] = 3; } } }", 0},
    {"pkg/prop", "{ inherit = \"snapmaps/props/base\"; class = \"idProp\"; editorVars { placeable = true; }"
        " edit = { health = 50; speed = 0.35; model = \"md6/custom\"; } }", 1},
    {"pkg/prop2", "{ inherit = \"pkg/prop\"; edit = { speed = 2.5; } }", 1},
    {"pkg/shrink", "{ inherit = \"snapmaps/props/base\"; edit = { list = { num = 1; } } }", 1},
    {"pkg/reset", "{ inherit = \"snapmaps/props/base\"; edit = { list = ! { num = 1; item[0] = 9; } } }", 1},
    {"pkg/apostrophe", "{ inherit = \"snapmaps/props/base\"; edit = { model = \"it's\"; } }", 1},
    {"pkg/lookedup", "{ inherit = \"snapmaps/props/base\"; edit = { health = 7; } }", 1},
    {"pkg/intdecimal", "{ inherit = \"snapmaps/props/base\"; edit = { health = 2.5; } }", 1},
    {"pkg/unknown", "{ inherit = \"snapmaps/props/base\"; edit = { mystery = 3; } }", 1},
    {"pkg/stock_changed", "{ inherit = \"snapmaps/props/base\"; edit = { health = 11; } }", 1},
    /* Package replacements of the stock base under private names. */
    {"smp/replaced", "{ class = \"idProp\"; edit = { health = 12; speed = 1; model = \"md6/base\";"
        " list = { num = 3; item[0] = 1; item[1] = 2; item[2] = 3; } } }", 1},
    {"smp/dropped", "{ class = \"idProp\"; edit = { health = 12; } }", 1},
    /* A native base with no SnapMap editor entry. */
    {"snapmaps/props/hidden", "{ class = \"idProp\"; edit = { health = 10; } }", 0},
    {"pkg/hidden", "{ inherit = \"snapmaps/props/hidden\"; edit = { health = 5; } }", 1},
    /* Package chains with no native ancestor: a served root and its child. */
    {"pkg/root", "{ class = \"idProp\"; edit = { health = 4; model = \"md6/root\"; } }", 1},
    {"pkg/rootchild", "{ inherit = \"pkg/root\"; edit = { speed = 3; } }", 1},
    {"pkg/rootkept", "{ class = \"idProp\"; edit = { health = 6; } }", 1},
    /* A served child selecting another reflected class than its stock base. */
    {"pkg/variant", "{ inherit = \"snapmaps/props/base\"; class = \"idPropVariant\"; edit = { health = 7; } }", 1},
    /* Nested reflected values can fold into the entity carrier. Declaration
     * pointers within them remain names, never substitute resource bodies. */
    {"pkg/nested", "{ inherit = \"snapmaps/props/base\"; edit = { nested = { damage = \"damage/package\"; gain = 2.5; }"
        " nestedList = { num = 2; item[0] = { damage = \"damage/other\"; gain = 0.5; }"
        " item[1] = { damage = NULL; gain = 1.5; } } } }", 1},
    {"pkg/nested_reset", "{ inherit = \"snapmaps/props/base\"; edit = { nested = ! { gain = 1; } } }", 1},
    {"pkg/nested_unknown", "{ inherit = \"snapmaps/props/base\"; edit = { nested = { unknown = 1; } } }", 1},
    {"pkg/exponent", "{ inherit = \"snapmaps/props/base\"; edit = { speed = 1e5; } }", 1},
    {"pkg/exponent_loss", "{ inherit = \"snapmaps/props/base\"; edit = { speed = 1e-11; } }", 1},
    {"pkg/exponent_empty", "{ inherit = \"snapmaps/props/base\"; edit = { speed = 1e+; } }", 1},
    {"snapmaps/volume/blocking", "{ inherit = \"snapmaps/props/base\"; edit = { health = 10; } }", 1},
    {"pkg/blocking", "{ inherit = \"snapmaps/volume/blocking\"; edit = { health = 5; } }", 1},
    {"snapmaps/event/survive", "{ inherit = \"snapmaps/props/base\"; edit = { health = 6; } }", 1},
    {"snapmaps/event/survive_deprecated", "{ inherit = \"snapmaps/props/base\"; edit = { health = 5; } }", 1},
    {"snapmaps/damageable_props/default", "{ inherit = \"snapmaps/props/base\"; edit = { health = 6; } }", 1},
    /* A definition's own pool settings and a class-selecting private chain. */
    {"pkg/pooled", "{ inherit = \"snapmaps/props/base\"; poolCount = 4; poolGranularity = 3; edit = { health = 9; } }", 1},
    {"pkg/pooledchild", "{ inherit = \"pkg/pooled\"; poolCount = 2; edit = { speed = 4; } }", 1},
    {"pkg/variantchild", "{ inherit = \"pkg/variant\"; edit = { speed = 5; } }", 1},
    /* Own expansion modes (M97): a placed copy's merge reads every layer's own
     * state whatever the layer's flag; a non-boolean flag fails Parse. */
    {"pkg/xbase", "{ inherit = \"snapmaps/props/base\"; edit = { health = 21; } }", 1},
    {"pkg/xunexp", "{ inherit = \"pkg/xbase\"; expandInheritance = false; poolCount = 4; edit = { speed = 22; } }", 1},
    {"pkg/xzero", "{ inherit = \"pkg/xbase\"; expandInheritance = 0; edit = { speed = 25; } }", 1},
    {"pkg/xchild", "{ inherit = \"pkg/xunexp\"; edit = { speed = 23; } }", 1},
    {"pkg/xbad", "{ inherit = \"pkg/xbase\"; expandInheritance = False; edit = { speed = 24; } }", 1},
    {"pkg/xquoted", "{ inherit = \"pkg/xbase\"; expandInheritance = \"false\"; edit = { speed = 24; } }", 1},
    /* Registered readers (idStr, declaration pointers) under a pooled base. */
    {"pkg/readers2", "{ inherit = \"pkg/prop\"; edit = { nested = { damage = \"damage/x\"; } } }", 1},
    {"pkg/ordbase", "{ inherit = \"snapmaps/props/base\"; edit = { nested = { gain = 1; } speed = 2; } }", 1},
    {"pkg/ordchild", "{ inherit = \"pkg/ordbase\"; edit = { model = \"md6/x\"; nested = { damage = \"damage/y\"; } } }", 1},
    {"pkg/labelbase", "{ inherit = \"snapmaps/props/base\"; edit = { label = \"a\"; } }", 1},
    {"pkg/labelchild", "{ inherit = \"pkg/labelbase\"; edit = { label = \"b\"; } }", 1},
    {"pkg/labelbare", "{ inherit = \"pkg/labelbase\"; edit = { label = 5; } }", 1},
    {"pkg/labelreader", "{ inherit = \"pkg/labelbase\"; edit = { nested = { damage = \"damage/z\"; } } }", 1},
    {"pkg/ordkept", "{ inherit = \"pkg/ordbase\"; edit = { nested = { damage = \"damage/y\"; } model = \"md6/x\"; } }", 1},
    /* FIELD (tint), number-ASSIGN (delay) and unmodelled (script) readers. */
    {"pkg/tintbase", "{ inherit = \"snapmaps/props/base\"; edit = { tint = { r = 1; } delay = 1; } }", 1},
    {"pkg/tintglobal", "{ inherit = \"pkg/tintbase\"; edit = { delay = 2.5; nested = { damage = \"damage/t\"; } } }", 1},
    {"pkg/tinttwice", "{ inherit = \"pkg/tintbase\"; edit = { tint = { g = 1; } } }", 1},
    {"pkg/delayblock", "{ inherit = \"snapmaps/props/base\"; edit = { delay = { value = 3; } } }", 1},
    {"pkg/delayneg", "{ inherit = \"pkg/tintbase\"; edit = { delay = -2; } }", 1},
    {"pkg/delayquoted", "{ inherit = \"pkg/tintbase\"; edit = { delay = \"x\"; } }", 1},
    {"pkg/opaque", "{ inherit = \"snapmaps/props/base\"; edit = { opaque = \"o/x\"; } }", 1},
    /* Production-schema layers (M78): reader-less pointer, polymorphic object
     * and bounded value, spelled as an integer so the carrier accepts it. */
    {"pkg/pr_null", "{ inherit = \"snapmaps/props/base\"; edit = { owner = NULL; } }", 1},
    {"pkg/pr_skip", "{ inherit = \"snapmaps/props/base\"; edit = { owner = \"y\"; } }", 1},
    {"pkg/pr_shape", "{ inherit = \"snapmaps/props/base\"; edit = { shape = { className = \"Derived\"; object = { gain = 2; } } } }", 1},
    {"pkg/pr_shape_bounded", "{ inherit = \"snapmaps/props/base\"; edit = { shape = { className = \"Derived\"; object = { falloff = 1; } } } }", 1},
    {"pkg/pr_bounded", "{ inherit = \"snapmaps/props/base\"; edit = { falloff = 1; } }", 1},
    {"pkg/pr_bounded_child", "{ inherit = \"pkg/pr_bounded\"; edit = { falloff = 0; } }", 1},
    /* Native reads accept a repeated member (M79): ordered writes (M80). */
    {"pkg/pr_repeat", "{ inherit = \"snapmaps/props/base\"; edit = { falloff = 1; falloff = 2; } }", 1},
    {"pkg/pr_voice2", "{ inherit = \"snapmaps/props/base\"; edit = { voice = \"vo/a\"; voice = \"vo/b\"; } }", 1},
    {"pkg/pr_nullskip", "{ inherit = \"snapmaps/props/base\"; edit = { owner = NULL; owner = \"y\"; } }", 1},
    {"pkg/pr_skipnull", "{ inherit = \"snapmaps/props/base\"; edit = { owner = \"y\"; owner = NULL; } }", 1},
    {"pkg/rr_root", "{ class = \"idProp\"; edit = { health = 1; health = 2; } }", 1},
    {"pkg/rr_child", "{ inherit = \"pkg/rr_root\"; edit = { health = 3; } }", 1},
    {"pkg/rr_num", "{ class = \"idProp\"; edit = { numLives = 1; numLives = 2; } }", 1},
    {"pkg/rr_Num", "{ class = \"idProp\"; edit = { NumLives = 1; NumLives = 2; } }", 1},
    {"pkg/rr_item", "{ class = \"idProp\"; edit = { itemCount = 1; itemCount = 2; } }", 1},
    /* A native definition with no state: its first served layer is appended. */
    {"snapmaps/props/empty", "{ class = \"idProp\"; }", 0},
    {"pkg/rs_root", "{ inherit = \"snapmaps/props/empty\"; edit = { health = 1; health = 2; } }", 1},
    {"pkg/rs_child", "{ inherit = \"pkg/rs_root\"; edit = { health = 3; } }", 1},
    {"pkg/rr_global", "{ class = \"idProp\"; edit = { model = \"md6/a\"; model = \"md6/b\"; } }", 1},
    {"pkg/rr_globalchild", "{ inherit = \"pkg/rr_global\"; edit = { speed = 2; } }", 1},
    {"pkg/nb", "{ inherit = \"snapmaps/props/base\"; edit = { nested = { gain = 1; gain = 2; } nested = { damage = \"damage/a\"; } } }", 1},
    {"pkg/rr_decl", "{ inherit = \"snapmaps/props/base\"; inherit = \"snapmaps/props/base\"; edit = { health = 1; } }", 1},
    /* Positional headers and first-statement state (M85/M86). */
    {"pkg/hd_other", "{ inherit = \"snapmaps/props/base\"; inherit = \"ai/unrelated\"; edit = { health = 1; } }", 1},
    {"pkg/hd_twice", "{ inherit = \"snapmaps/props/base\"; edit = { health = 1; } edit = { speed = 2; } }", 1},
    {"pkg/hd_inert", "{ inherit = \"snapmaps/props/base\"; data = { health = 7; } edit = { speed = 2; } }", 1},
    {"pkg/rs_hd_first", "{ inherit = \"snapmaps/props/empty\"; edit = { health = 1; } edit = { speed = 2; } }", 1},
    {"pkg/rs_hd_named", "{ inherit = \"snapmaps/props/empty\"; data = { health = 1; } }", 1},
    {"pkg/rr_hd_late", "{ class = \"idProp\"; inherit = \"snapmaps/props/base\"; edit = { health = 1; } }", 1},
    /* A differently named first state block (M87): later layers update it by
     * its own name; an edit under it is inert, as is one over a native base
     * whose state has another name. */
    {"pkg/rs_dn_child", "{ inherit = \"pkg/rs_hd_named\"; data = { health = 2; } edit = { speed = 5; } }", 1},
    {"snapmaps/props/dbase", "{ class = \"idProp\"; data = { health = 3; } }", 0},
    {"pkg/db_update", "{ inherit = \"snapmaps/props/dbase\"; data = { health = 4; } }", 1},
    {"pkg/db_inert", "{ inherit = \"snapmaps/props/dbase\"; edit = { speed = 1; } }", 1},
    /* Pooled layers read their own first statement (M87). */
    {"pkg/pm_multi", "{ inherit = \"snapmaps/props/base\"; edit = { health = 1; } data = { armor = 2; } }", 1},
    {"pkg/pm_other", "{ inherit = \"snapmaps/props/base\"; data = { armor = 2; } edit = { health = 1; } }", 1},
    /* Pooled storage order (R-213, production schema). */
    {"pkg/po_order", "{ inherit = \"snapmaps/props/base\"; edit = { health = 1; armor = 2; } }", 1},
    {"pkg/po_alias", "{ inherit = \"snapmaps/props/base\"; edit = { health = 1; hp = 2; } }", 1},
    {"pkg/po_tint", "{ inherit = \"snapmaps/props/base\"; edit = { armor = 2; tint = { r = 1; } } }", 1},
    {"pkg/po_tint2", "{ inherit = \"snapmaps/props/base\"; edit = { tint = { r = 1; } armor = 2; } }", 1},
    {"pkg/po_inner", "{ inherit = \"snapmaps/props/base\"; edit = { inner = { b = 1; } armor2 = 2; } }", 1},
    {"pkg/po_inner2", "{ inherit = \"snapmaps/props/base\"; edit = { inner = { a = 1; } armor2 = 2; } }", 1},
    {"pkg/po_shape", "{ inherit = \"snapmaps/props/base\"; edit = { shape = { className = \"Derived\"; object = { gain = 2; } }"
        " health = 1; } }", 1},
    {"pkg/po_flag", "{ inherit = \"snapmaps/props/base\"; edit = { health = 2; flag = 1; } }", 1},
    {"pkg/po_flag2", "{ inherit = \"snapmaps/props/base\"; edit = { flag = 1; health = 2; } }", 1},
    {"pkg/po_flagchild", "{ inherit = \"pkg/po_flag2\"; edit = { flag = 0; } }", 1},
    /* Array footprints, selected-object storage and verified setter stores (M84). */
    {"pkg/pa_over", "{ inherit = \"snapmaps/props/base\"; edit = { over = 1; vals = { vals[0] = 1; } } }", 1},
    {"pkg/pa_after", "{ inherit = \"snapmaps/props/base\"; edit = { after = 1; vals = { vals[0] = 1; } } }", 1},
    {"pkg/ps_alias", "{ inherit = \"snapmaps/props/base\"; edit = { alias = 1; shape = { className = \"Derived\";"
        " object = { gain = 2; } } } }", 1},
    {"pkg/ps_health", "{ inherit = \"snapmaps/props/base\"; edit = { health = 1; shape = { className = \"Derived\";"
        " object = { gain = 2; } } } }", 1},
    {"pkg/pb_bits", "{ inherit = \"snapmaps/props/base\"; edit = { health = 1; bits = 1; } }", 1},
    {"pkg/pb_word", "{ inherit = \"snapmaps/props/base\"; edit = { word = 1; bits = 1; } }", 1},
    {"pkg/pb_rogue", "{ inherit = \"snapmaps/props/base\"; edit = { health = 1; rogue = 1; } }", 1},
    {"pkg/pl_header", "{ inherit = \"snapmaps/props/base\"; edit = { inHeader = 1; nums = { num = 1; item[0] = 1; } } }", 1},
    {"pkg/pl_after", "{ inherit = \"snapmaps/props/base\"; edit = { afterList = 1; nums = { num = 8; item[0] = 1;"
        " item[7] = 1; } } }", 1},
    {"pkg/pb_true", "{ inherit = \"snapmaps/props/base\"; edit = { bits = true; health = 2; } }", 1},
    {"pkg/pb_false", "{ inherit = \"pkg/pb_true\"; edit = { bits = false; } }", 1},
    {"pkg/pb_one", "{ inherit = \"pkg/pb_true\"; edit = { bits = 1; } }", 1},
    {"pkg/pi_base", "{ inherit = \"snapmaps/props/base\"; edit = { count3 = 2; } }", 1},
    {"pkg/pi_child", "{ inherit = \"pkg/pi_base\"; edit = { count3 = -1; } }", 1},
    {"pkg/pi_float", "{ inherit = \"pkg/pi_base\"; edit = { count3 = 1.5; } }", 1},
    {"pkg/pi_repeat", "{ inherit = \"snapmaps/props/base\"; edit = { count3 = 2; health = 1; count3 = 5; } }", 1},
    {"pkg/pa_two", "{ inherit = \"snapmaps/props/base\"; edit = { vals = { vals[0] = 1; vals[2] = 1; } } }", 1},
    {"pkg/pa_item3", "{ inherit = \"snapmaps/props/base\"; edit = { over = 1; vals = { vals[3] = 1; } } }", 1},
    /* Served definitions a served block-grammar declaration names (served_checks). */
    {"pkg/noted", "{ inherit = \"snapmaps/props/base\"; edit = { target = \"pkg/prop\"; } }", 1},
    {"pkg/relay-def", "{ inherit = \"snapmaps/props/base\"; edit = { target = \"pkg/prop\"; } }", 1},
    {"pkg/broken_def", "{ inherit \"snapmaps/props/base\" }", 1},
};

/* The effective world definition chain and aiPoolNumbers declarations
 * (R-203). The default world is the stock, unserved worldspawn. */
static const char *world_text = "{ class = \"idWorldspawn\"; edit = { globalAIsettings = \"default\"; } }";
static int world_served;
static const char *world_private; /* pkg/world_private, when a served world inherits it */
static const char *pool_text;     /* aipoolnumbers/pool/a */
static const char *pool_b_text, *pool_c_text; /* aipoolnumbers/pool/b, pool/c */
static int fixture_read(void *context, const char *name, sh_decl_source *text, int *served, char **replaces)
{
    (void)context;
    if (!strcmp(name, "worldspawn") || (world_private && !strcmp(name, "pkg/world_private"))) {
        const char *body = !strcmp(name, "worldspawn") ? world_text : world_private;
        char *copy = body ? _strdup(body) : NULL;
        if (!body) return 0;
        if (!copy) return -1;
        *text = (sh_decl_source){copy, strlen(copy)}; *served = !strcmp(name, "worldspawn") ? world_served : 1;
        return 1;
    }
    for (size_t i = 0; i < sizeof(decls) / sizeof(decls[0]); i++) if (!strcmp(decls[i].name, name)) {
        char *copy = _strdup(decls[i].text);
        if (!copy) return -1;
        *text = (sh_decl_source){copy, strlen(copy)}; *served = decls[i].served;
        if (replaces && !strncmp(name, "smp/", 4) && !(*replaces = _strdup("snapmaps/props/base"))) { free(copy); return -1; }
        return 1;
    }
    return 0;
}
/* R-295 h2j: the declarations the product serves (the converter's served
 * index, ni_served_collect), listed per case. The non-entityDef texts are
 * block grammar the generic grammar does not read; the entityDefs are in
 * decls. */
static const struct { const char *family, *name, *text; } served_fixture[] = {
    {"animweb", "web/dormant", "{ subweb \"fist\" { anim \"anim/fist_idle\"; } }"},
    {"animweb", "web/live", "{ subweb \"fist\" { anim \"pkg/prop\"; } }"},
    {"animweb", "web/relay", "{ subweb \"fist\" { spawn pkg/noted } }"},
    {"animweb", "web/joined", "{ subweb \"fist\" { spawn pkg/relay-def } }"},
    {"md6def", "md6/unlexable", "{ joint \"origin\" { rate 0x10 } }"},
    {"entitydef", "pkg/noted", NULL},
    {"entitydef", "pkg/relay-def", NULL},
    {"entitydef", "pkg/broken_def", NULL},
};
static int served_listed[8], served_listed_count;
static int fixture_served_decl(void *context, size_t index, const char **family, const char **name)
{
    (void)context;
    if (index >= (size_t)served_listed_count) return 0;
    *family = served_fixture[served_listed[index]].family; *name = served_fixture[served_listed[index]].name;
    return 1;
}
static int fixture_read_decl(void *context, const char *type, const char *name, sh_decl_source *text, int *served)
{
    (void)context;
    for (size_t i = 0; i < sizeof(served_fixture) / sizeof(*served_fixture); i++)
        if (served_fixture[i].text && !strcmp(type, served_fixture[i].family) && !strcmp(name, served_fixture[i].name)) {
            char *copy = _strdup(served_fixture[i].text);
            if (!copy) return -1;
            *text = (sh_decl_source){copy, strlen(copy)}; *served = 1;
            return 1;
        }
    const char *body = !strcmp(name, "pool/a") ? pool_text : !strcmp(name, "pool/b") ? pool_b_text :
        !strcmp(name, "pool/c") ? pool_c_text : NULL;
    if (strcmp(type, "aipoolnumbers") || !body) return 0;
    char *copy = _strdup(body);
    if (!copy) return -1;
    *text = (sh_decl_source){copy, strlen(copy)}; *served = 1;
    return 1;
}
static int fixture_derives(void *context, const char *class_name, const char *base_name)
{
    (void)context;
    if (!strcmp(base_name, "idProp")) return !strcmp(class_name, "idProp") || !strcmp(class_name, "idPropVariant");
    return !strcmp(class_name, "idPropVariant") && !strcmp(base_name, "idPropVariant");
}
/* A world site no consumer is traced for also names pkg/lookedup. */
static int fixture_looked_up(void *context, size_t entity, const char *name, sh_native_inline_use *uses, size_t capacity)
{
    (void)context; (void)entity;
    if (strcmp(name, "pkg/lookedup")) return 0;
    if (capacity) uses[0] = (sh_native_inline_use){"world", "/k:filter/k:entityDef"};
    return 1;
}
static const char *fixture_keep(void *context, size_t entity, const char *name, sh_decl_carrier_limit *limit)
{
    (void)context; (void)entity; *limit = SH_DECL_CARRIER_ENGINE;
    return !strcmp(name, "pkg/stock_changed") ? "its package also changes a stock definition globally" : NULL;
}
static const char *fixture_placement(void *context, const char *from, const char *base, sh_decl_carrier_limit *limit)
{
    (void)context; *limit = SH_DECL_CARRIER_ENGINE;
    /* A placeable package root: the empty base (a class-only definition) of
     * pkg/rootchild is admitted, pkg/rootkept's is not. */
    if (!*base && (!strcmp(from, "pkg/rootchild") || !strncmp(from, "pkg/rr_", 7))) return NULL;
    if (!strcmp(base, "snapmaps/props/empty") && !strncmp(from, "pkg/rs_", 7)) return NULL;
    if (!strcmp(base, "snapmaps/props/dbase") && !strncmp(from, "pkg/db_", 7)) return NULL;
    return strcmp(base, "snapmaps/props/base") ? "no SnapMap editor entry" : NULL;
}

/* Reflected schema includes nested objects and a collection containing native
 * declaration pointers. These links carry names, not declaration bodies (R136). */
static int schema_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "idProp") || !strcmp(type.name, "idPropVariant") || !strcmp(type.name, "idNested")) {
        shape->kind = SH_DECL_VALUE_OBJECT; return 1;
    }
    if (!strcmp(type.name, "idDeclDamage") && !strcmp(type.ops, "*")) {
        shape->kind = SH_DECL_VALUE_REFERENCE; shape->effects = SH_DECL_EFFECTS_GLOBAL; return 1;
    }
    /* A reader with unmodelled effects, one confined to its field, and one
     * that assigns a number completely and otherwise reads a partial block. */
    if (!strcmp(type.name, "idOpaque")) { shape->kind = SH_DECL_VALUE_IGNORE; return 1; }
    if (!strcmp(type.name, "idColor")) { shape->kind = SH_DECL_VALUE_IGNORE; shape->effects = SH_DECL_EFFECTS_FIELD; return 1; }
    if (!strcmp(type.name, "idTypesafeTime<float>")) {
        shape->kind = SH_DECL_VALUE_IGNORE; shape->assigns = SH_DECL_ASSIGN_NUMBER;
        shape->effects = SH_DECL_EFFECTS_FIELD; return 1;
    }
    if (!strcmp(type.name, "idList<idNested>")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"idNested", ""};
        shape->item_key = "item"; shape->count_key = "num"; return 1;
    }
    if (!strcmp(type.name, "idList<int>")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"int", ""};
        shape->item_key = "item"; shape->count_key = "num"; return 1;
    }
    if (!strcmp(type.name, "int") || !strcmp(type.name, "float") || !strcmp(type.name, "idStr") || !strcmp(type.name, "bool")) {
        shape->kind = SH_DECL_VALUE_IGNORE; shape->plain = strcmp(type.name, "idStr") != 0;
        if (!shape->plain) { shape->assigns = SH_DECL_ASSIGN_QUOTED; shape->effects = SH_DECL_EFFECTS_FIELD; }
        return 1;
    }
    return 0;
}
static int schema_field(void *context, sh_decl_value_type object, const char *key, sh_decl_value_type *field)
{
    (void)context;
    if (!strcmp(object.name, "idNested")) {
        if (!strcmp(key, "damage")) { *field = (sh_decl_value_type){"idDeclDamage", "*"}; return 1; }
        if (!strcmp(key, "gain")) { *field = (sh_decl_value_type){"float", ""}; return 1; }
        return -1;
    }
    if (strcmp(object.name, "idProp") && strcmp(object.name, "idPropVariant")) return -1;
    /* A bitfield and an obsolete name: the native schema skips both here. */
    if (!strcmp(key, "flag") || !strcmp(key, "obsolete")) return 0;
    if (!strcmp(key, "isVisible") || !strcmp(key, "isNetworkStatic")) { *field = (sh_decl_value_type){"bool", ""}; return 1; }
    if (!strcmp(key, "health") || !strcmp(key, "numLives") || !strcmp(key, "NumLives") || !strcmp(key, "itemCount")) {
        *field = (sh_decl_value_type){"int", ""}; return 1;
    }
    if (!strcmp(key, "speed")) { *field = (sh_decl_value_type){"float", ""}; return 1; }
    if (!strcmp(key, "model")) { *field = (sh_decl_value_type){"idDeclDamage", "*"}; return 1; }
    if (!strcmp(key, "label")) { *field = (sh_decl_value_type){"idStr", ""}; return 1; }
    if (!strcmp(key, "tint")) { *field = (sh_decl_value_type){"idColor", ""}; return 1; }
    if (!strcmp(key, "delay")) { *field = (sh_decl_value_type){"idTypesafeTime<float>", ""}; return 1; }
    if (!strcmp(key, "opaque")) { *field = (sh_decl_value_type){"idOpaque", ""}; return 1; }
    if (!strcmp(key, "list")) { *field = (sh_decl_value_type){"idList<int>", ""}; return 1; }
    if (!strcmp(key, "nested")) { *field = (sh_decl_value_type){"idNested", ""}; return 1; }
    if (!strcmp(key, "nestedList")) { *field = (sh_decl_value_type){"idList<idNested>", ""}; return 1; }
    return -1;
}
static int schema_reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{ (void)context; (void)path; (void)type; (void)name; (void)length; return 1; }
/* Inline member storage: offset and stored bytes. flag is written through
 * its bitfield setter; obsolete is not a member; others are unknown. */
static int schema_storage(void *context, sh_decl_value_type object, const char *key, sh_decl_member_storage *storage)
{
    static const struct { const char *type, *key; long long offset, extent; } rows[] = {
        {"idProp", "health", 0, 4}, {"idProp", "speed", 4, 4}, {"idProp", "model", 8, 8}, {"idProp", "label", 16, 48},
        {"idProp", "tint", 64, 16}, {"idProp", "delay", 80, 4}, {"idProp", "opaque", 84, 4}, {"idProp", "list", 88, 24},
        {"idProp", "nested", 112, 16}, {"idProp", "nestedList", 128, 24}, {"idProp", "isVisible", 152, 1},
        {"idProp", "isNetworkStatic", 153, 1}, {"idProp", "numLives", 156, 4}, {"idProp", "NumLives", 160, 4},
        {"idProp", "itemCount", 164, 4}, {"idNested", "damage", 0, 8}, {"idNested", "gain", 8, 4},
    };
    (void)context; storage->offset = storage->extent = 0;
    if (!strcmp(key, "flag")) return 2;
    if (!strcmp(key, "obsolete")) return 0;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        if (!strcmp(rows[i].key, key) && (!strcmp(rows[i].type, object.name) ||
            (!strcmp(rows[i].type, "idProp") && !strcmp(object.name, "idPropVariant")))) {
            storage->offset = rows[i].offset; storage->extent = rows[i].extent; return 1;
        }
    return -1;
}
/* One edit block through the production reduction: removed occurrences, or
 * a refusal naming reason. */
static int reduce_case(const sh_decl_dependency_schema *schema, const char *text, size_t want, const char *reason)
{
    char error[160] = "", why[320] = "";
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){text, strlen(text)}, error, sizeof(error));
    sh_decl_node *edit = tree ? (sh_decl_node *)sh_decl_tree_member(tree, "edit") : NULL;
    size_t removed = 0;
    int result = edit ? sh_native_inline_reduce(schema, edit, "idProp", &removed, why, sizeof(why)) : -1;
    int ok = reason ? result == 0 && strstr(why, reason) != NULL : result == 1 && removed == want;
    if (!ok) fprintf(stderr, "reduce case %s: %d %s %s\n", text, result, why, error);
    sh_decl_tree_free(tree);
    return ok;
}

#define ENTITY_BARE(inherit, state) \
    "{\"entityDef\":{\"inherit\":\"" inherit "\",\"name\":\"\",\"state\":" state \
    ",\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\"},\"uniqueId\":1}"
#define ENTITY_NO_CLASS(inherit, state, version) \
    "{\"entityDef\":{\"inherit\":\"" inherit "\",\"name\":\"\",\"state\":" state \
    ",\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":" version "},\"uniqueId\":1}"
#define ENTITY(inherit, state, version) \
    "{\"entityDef\":{\"className\":\"idProp\",\"inherit\":\"" inherit "\",\"name\":\"\",\"state\":" state \
    ",\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":" version "},\"uniqueId\":1}"

static const sh_native_inline_site *site_for(const sh_native_inline_report *report, size_t entity)
{
    for (size_t i = 0; i < report->count; i++) if (report->sites[i].entity == entity) return report->sites + i;
    return NULL;
}

#ifdef SH_NATIVE_INLINE_TESTING
static char *version_path(const char *json, const char *path)
{
    char *value = _strdup(json), *keys = _strdup(path), *next = NULL;
    if (!value || !keys) { free(value); free(keys); return NULL; }
    for (char *key = strtok_s(keys, ".", &next); key; key = strtok_s(NULL, ".", &next)) {
        sh_json_object object = {0};
        if (!sh_native_json_parse_object(value, strlen(value), 64, &object)) { free(value); value = NULL; break; }
        const char *member = sh_json_object_get(&object, key);
        char *copy = member ? _strdup(member) : NULL;
        sh_json_object_free(&object); free(value); value = copy;
        if (!value) break;
    }
    free(keys); return value;
}
static void version_case(const char *inherit, int version, const char *edit,
    const char *path, const char *expected, int expected_result)
{
    char json[4096], version_json[32];
    snprintf(json, sizeof(json), "{\"inherit\":\"%s\",\"className\":\"old\",\"state\":{\"edit\":%s},"
        "\"~version\":%d,\"ordinary\":{\"bytes\":[\"x\",  1.0]}}", inherit, edit, version);
    snprintf(version_json, sizeof(version_json), "%d", version);
    char original[4096]; strcpy(original, json);
    char *copy = NULL;
    int result = sh_native_inline_test_version_copy(json, 26, &copy);
    if (result != expected_result) fprintf(stderr, "version %d %s: result %d expected %d\n", version, inherit, result, expected_result);
    CHECK(result == expected_result); CHECK(!strcmp(json, original));
    if (result == 1 && copy) {
        char *value = version_path(copy, path);
        if ((expected && (!value || strcmp(value, expected))) || (!expected && value))
            fprintf(stderr, "version %d %s %s: %s expected %s\n", version, inherit, path, value ? value : "<absent>", expected ? expected : "<absent>");
        CHECK(expected ? value && !strcmp(value, expected) : !value); free(value);
        value = version_path(copy, "ordinary"); CHECK(value && !strcmp(value, "{\"bytes\":[\"x\",  1.0]}")); free(value);
        value = version_path(copy, "~version"); CHECK(value && !strcmp(value, version_json)); free(value);
    } else CHECK(copy == NULL);
    free(copy);
}
static void version_policy_checks(void)
{
    /* Both sides of every actual native threshold; unrelated values stay raw. */
    version_case("prop/moveable/bucket", 14, "{}", "inherit", "\"snapmaps/moveable_props/bucket\"", 1);
    version_case("prop/moveable/bucket", 15, "{}", "inherit", "\"prop/moveable/bucket\"", 1);
    version_case("snapmaps/moveable_props/battery", 14, "{}", "className", "\"idProp\"", 1);
    version_case("snapmaps/moveable_props/battery", 15, "{}", "className", "\"old\"", 1);
    version_case("snapmaps/event/survive", 15, "{\"eventDurationMS\":125}", "state.edit.eventDuration", "{\"milliseconds\":125}", 1);
    version_case("snapmaps/event/survive", 16, "{\"eventDurationMS\":125}", "state.edit.eventDurationMS", "125", 1);
    /* These delimiters had whitespace before ';' in retained source. An
     * extraction that merges neighboring calls misses this distinct selector. */
    version_case("snapmaps/capture_point/demon_signal", 15, "{\"animationTimeMS\":4}", "state.edit.animationTime", "{\"milliseconds\":4}", 1);
    version_case("snapmaps/damageable_props/default", 15, "{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":6}", "state.edit.autoRespawnTime", "{\"milliseconds\":6}", 1);
    version_case("snapmaps/logic/repeater", 15, "{\"delayBetweenRepeats\":9}", "state.edit.delayBetweenRepeats", "{\"milliseconds\":9}", 1);
    version_case("snapmaps/action/player/give_droppable", 16, "{}", "state.edit.droppableDecl", "\"power_core/power_core_normal\"", 1);
    version_case("snapmaps/action/player/take_droppable", 16, "{\"droppableDecl\":\"flag/flag_blue\"}", "state.edit.droppableDecl", NULL, 1);
    version_case("snapmaps/action/player/take_droppable", 17, "{\"droppableDecl\":\"flag/flag_blue\"}", "state.edit.droppableDecl", "\"flag/flag_blue\"", 1);
    version_case("snapmaps/settings/hud_settings", 17, "{\"topLeft\":{\"type\":\"SNAP_HUD_TOP_LEADERBOARD\",\"other\":1}}",
        "state.edit.topLeft.type", "\"SNAP_HUD_GAME_PAR_TIME\"", 1);
    version_case("snapmaps/settings/hud_settings", 18, "{\"bottom\":{\"type\":\"SNAP_HUD_TOP_LEADERBOARD\"}}",
        "state.edit.bottom.type", "\"SNAP_HUD_TOP_LEADERBOARD\"", 1);
    version_case("snapmaps/event/survive", 18, "{}", "inherit", "\"snapmaps/event/survive_deprecated\"", 1);
    version_case("snapmaps/event/survive", 19, "{}", "inherit", "\"snapmaps/event/survive\"", 1);
    version_case("snapmaps/spawners/spawn_weapon", 19, "{\"spawnOnStart\":true}", "state.edit.spawnOnStart", NULL, 1);
    version_case("snapmaps/spawners/spawn_weapon", 20, "{\"spawnOnStart\":true}", "state.edit.spawnOnStart", "true", 1);
    version_case("snapmaps/visblockers/industrial/cap_01", 20, "{}", "className", "\"idSnapMapCapEntity\"", 1);
    version_case("snapmaps/visblockers/industrial/cap_01", 21, "{}", "className", "\"old\"", 1);
    version_case("snapmaps/spawners/spawn_backpack", 21, "{\"colorParam\":{\"constantValue\":{\"r\":1,\"g\":2,\"b\":3,\"a\":0,\"other\": 4.0}}}",
        "state.edit.colorParam.constantValue", "{\"r\":0,\"g\":0,\"b\":0,\"a\":0,\"other\":4.0}", 1);
    version_case("snapmaps/spawners/spawn_backpack", 22, "{\"colorParam\":{\"constantValue\":{\"a\":0,\"r\":1}}}", "state.edit.colorParam.constantValue.r", "1", 1);
    version_case("snapmaps/spawners/spawn_backpack", 21, "{\"colorParam\":{\"constantValue\":{\"a\":-1}}}", "state.edit.colorParam.constantValue.b", "0", 1);
    version_case("snapmaps/spawners/spawn_backpack", 21, "{\"colorParam\":{\"constantValue\":{\"a\":0.5,\"r\":7}}}", "state.edit.colorParam.constantValue.r", "7", 1);
    version_case("snapmaps/spawners/spawn_backpack", 21, "{\"colorParam\":{\"constantValue\":{\"r\":7}}}", "state.edit.colorParam.constantValue.a", NULL, 1);
    version_case("snapmaps/message/codex_message", 23, "{}", "state.edit.image", "\"textures/guis/codex_images/uac_personnel/vega\"", 1);
    version_case("snapmaps/message/codex_message", 24, "{}", "state.edit.image", NULL, 1);
    version_case("snapmaps/message/codex_message", 23, "{\"image\":\"textures/guis/snapmaps/codex_images/vega\"}", "state.edit.image", NULL, 1);
    version_case("snapmaps/volume/blocking", 24, "{\"isVisible\":false,\"isNetworkStatic\":null}", "state.edit.isVisible", NULL, 1);
    version_case("snapmaps/volume/blocking", 24, "{}", "state.edit.isVisible", "false", 1);
    version_case("snapmaps/volume/blocking", 25, "{\"isVisible\":true,\"isNetworkStatic\":false}", "state.edit.isVisible", "true", 1);
    version_case("snapmaps/volume/blocking", 25, "{\"isVisible\":true,\"isNetworkStatic\":false}", "state.edit.isNetworkStatic", NULL, 1);
    version_case("snapmaps/volume/blocking", 26, "{\"isNetworkStatic\":false}", "state.edit.isNetworkStatic", "false", 1);
    version_case("Snapmaps/volume/blocking", 0, "{\"isVisible\":true}", "state.edit.isVisible", "true", 1);
    version_case("snapmaps\\\\volume\\\\blocking", 0, "{\"isVisible\":true}", "state.edit.isVisible", "true", 1);
    version_case("pkg/blocking", 0, "{\"isVisible\":true}", "state.edit.isVisible", "true", 1);
    /* Native integer and real checks differ. Conversion cannot approximate
     * error-domain integers or coerce strings/containers into milliseconds. */
    const char *numbers[] = {"-2.75", "true", "null", "1e3", "2147483647.0", "-2147483648.0"};
    const char *wrapped[] = {"{\"milliseconds\":-2}", "{\"milliseconds\":1}", "{\"milliseconds\":0}",
        "{\"milliseconds\":1000}", "{\"milliseconds\":2147483647}", "{\"milliseconds\":-2147483648}"};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(*numbers); i++) {
        char edit[128]; snprintf(edit, sizeof(edit), "{\"eventDurationMS\":%s,\"eventDuration\":{\"milliseconds\":9}}", numbers[i]);
        version_case("snapmaps/event/survive", 15, edit, "state.edit.eventDuration", wrapped[i], 1);
    }
    const char *unsupported[] = {"2147483647", "-2147483648", "\"100\"", "[]", "{}", "1e400"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(*unsupported); i++) {
        char edit[128]; snprintf(edit, sizeof(edit), "{\"eventDurationMS\":%s}", unsupported[i]);
        version_case("snapmaps/event/survive", 15, edit, "state.edit", NULL, 0);
    }
    version_case("snapmaps/damageable_props/default", 15, "{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":\"bad\"}", "state.edit", NULL, 0);
    version_case("pkg/prop", 27, "{}", "state.edit", NULL, 0);
    /* Failure after an earlier mutation still returns no candidate. Retrying
     * from the exact original starts clean and cannot strand partial output. */
    const char *allocation = "{\"inherit\":\"snapmaps/damageable_props/default\",\"state\":{\"edit\":{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":6}},\"~version\":15}";
    long allocation_failures = 0; int allocation_success = 0;
    for (long fail = 0; fail < 512; fail++) {
        char *copy = NULL;
        sh_native_inline_test_version_alloc_fail_after(fail);
        int result = sh_native_inline_test_version_copy(allocation, 26, &copy);
        CHECK(result == -1 || result == 1);
        if (result == -1) { CHECK(copy == NULL); allocation_failures++; }
        else {
            char *value = version_path(copy, "state.edit.autoRespawnTime");
            CHECK(value && !strcmp(value, "{\"milliseconds\":6}")); free(value); allocation_success = 1;
        }
        free(copy);
        if (allocation_success) break;
    }
    CHECK(allocation_failures > 10 && allocation_success);
    sh_native_inline_test_version_alloc_fail_after(-1);
    char *copy = NULL; CHECK(sh_native_inline_test_version_copy(allocation, 26, &copy) == 1); free(copy);
    const char *native_bytes = "{\"inherit\":\"pkg/prop\",\"state\":{\"edit\":{}},\"~version\":0,"
        "\"ordinary\\u0000tail\":\"\xff\",\"array\":[ 1.0,  2 ]}";
    copy = NULL; CHECK(sh_native_inline_test_version_copy(native_bytes, 26, &copy) == 1);
    CHECK(copy && strstr(copy, "\"ordinary\\u0000tail\":\"\xff\"") && strstr(copy, "[ 1.0,  2 ]")); free(copy);
    copy = NULL;
    CHECK(sh_native_inline_test_version_copy("{\"inherit\":\"snapmaps/volume/blocking\\u0000tail\",\"state\":{\"edit\":{}},\"~version\":24}", 26, &copy) == 0);
    CHECK(copy == NULL); free(copy);
}
static void version_integration_checks(const sh_native_inline_source *source)
{
    const char *json = "{\"entities\":["
        ENTITY("snapmaps/event/survive", "{\"edit\":{\"health\":99}}", "18") ","
        ENTITY("snapmaps/volume/blocking", "{\"edit\":{\"isVisible\":true,\"isNetworkStatic\":null}}", "24") ","
        ENTITY("pkg/blocking", "{\"edit\":{\"isVisible\":true,\"isNetworkStatic\":true}}", "0") ","
        ENTITY("snapmaps/damageable_props/default", "{\"edit\":{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":\"bad\"}}", "15") ","
        ENTITY("pkg/prop", "{\"edit\":{}}", "30") "],\"outside\": [  1.0 ]}";
    sh_native_inline_report report = {0}; size_t length = 0; char error[512];
    char *out = sh_native_inline_document(json, strlen(json), source, &length, &report, error, sizeof(error));
    CHECK(out != NULL);
    if (!out) return;
    const sh_native_inline_site *site = site_for(&report, 0);
    CHECK(site && site->converted && !strcmp(site->from, "snapmaps/event/survive_deprecated"));
    site = site_for(&report, 1); CHECK(site && site->converted);
    site = site_for(&report, 2); CHECK(site && site->converted);
    /* Original private inherit does not select its ancestor's old-version
     * boolean removal. Neither field may be upgraded a second time later. */
    CHECK(strstr(out, "\"isVisible\":true") != NULL && strstr(out, "\"isNetworkStatic\":true") != NULL);
    site = site_for(&report, 3); CHECK(site && !site->converted && !strcmp(site->from, "snapmaps/damageable_props/default"));
    CHECK(strstr(out, ENTITY("snapmaps/damageable_props/default", "{\"edit\":{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":\"bad\"}}", "15")) != NULL);
    site = site_for(&report, 4); CHECK(site && !site->converted);
    CHECK(strstr(out, ENTITY("pkg/prop", "{\"edit\":{}}", "30")) != NULL);
    CHECK(strstr(out, "],\"outside\": [  1.0 ]}") != NULL);
    char *again = NULL; sh_native_inline_report second = {0}; size_t again_length = 0;
    again = sh_native_inline_document(out, length, source, &again_length, &second, error, sizeof(error));
    CHECK(again && again_length == length && !memcmp(again, out, length));
    free(again); sh_native_inline_report_free(&second); free(out); sh_native_inline_report_free(&report);
    sh_native_inline_test_version_alloc_fail_after(0);
    out = sh_native_inline_document(json, strlen(json), source, &length, &report, error, sizeof(error));
    CHECK(out && length == strlen(json) && !memcmp(out, json, length) && !error[0]);
    for (size_t i = 0; i < report.count; i++) CHECK(!report.sites[i].converted);
    free(out); sh_native_inline_report_free(&report);
    sh_native_inline_test_version_alloc_fail_after(-1);
}
#endif

/* Plumbing double for the native evaluator: it returns a configured status
 * per entity and records which sites asked. It proves classification, fallback
 * and acceptance authority only, never native equivalence. */
static sh_native_eval_status fake_by_entity[64];
static unsigned long long fake_asked;
/* R-212: the production reduction callback, applied inside the evaluator to
 * prepared texts of the first stateful layer's shape (-2 not asked). The
 * engine prepares a statement list with no enclosing braces (VK 0x1417ae9d0
 * prints the merged tree's top-level nodes through VK 0x141a670d0, R-295). */
static int fake_reduced_equal = -2, fake_reduced_refused = -2;
static char fake_braced_why[200];
static void fake_reduction(const sh_native_eval_reduction *reduction)
{
    static const char *const texts[] = {"edit = {\n\thealth = 3;\n\thealth = 2;\n}\n", "edit = {\n\thealth = 2;\n}\n",
        "edit = { health = 3; model = \"md6/a\"; health = 2; }", "edit = { num = 1; num = 2; }",
        "edit = { health = 3; health = 2; }", "edit = { health = 3; }", "{ edit = { health = 2; } }"};
    char *out[7] = {0}, why[200];
    size_t length[7] = {0}, removed = 0;
    int r[7];
    for (int i = 0; i < 7; i++)
        r[i] = reduction->reduce(reduction->context, texts[i], strlen(texts[i]), out + i, length + i, &removed,
            i == 6 ? fake_braced_why : why, i == 6 ? sizeof(fake_braced_why) : sizeof(why));
    fake_reduced_equal = r[0] == 1 && r[1] == 1 && length[0] == length[1] && !memcmp(out[0], out[1], length[0]);
    /* A storage read between the occurrences and a counted name stay outside
     * the proof; a reduced text never equals one keeping the hidden value. A
     * braced declaration is not the shape the engine prepares. */
    fake_reduced_refused = r[2] == 0 && r[3] == 0 && r[4] == 1 && r[5] == 1 &&
        (length[4] != length[5] || memcmp(out[4], out[5], length[4])) && r[6] == 0 &&
        strstr(fake_braced_why, "does not parse as statements") && strstr(fake_braced_why, "it begins '{ edit");
    for (int i = 0; i < 7; i++) free(out[i]);
}
/* The class each evaluated tree states, "" for none. */
static char fake_target_class[64][48], fake_candidate_class[64][48];
static void fake_class(const sh_decl_node *tree, char *out, size_t capacity)
{
    const sh_decl_node *member = tree ? sh_decl_tree_member(tree, "class") : NULL;
    const char *text; size_t length;
    out[0] = 0;
    if (member && sh_decl_tree_literal(member, &text, &length) && length < capacity) { memcpy(out, text, length); out[length] = 0; }
}
static char fake_from[64], fake_base[96];
static sh_native_eval_status fake_evaluate(void *context, size_t entity, const char *from, const char *base,
    const sh_decl_node *target, const sh_decl_node *candidate, const sh_native_eval_reduction *reduction,
    char *detail, size_t capacity)
{
    (void)context;
    if (reduction) fake_reduction(reduction);
    if (!target || !sh_decl_tree_member(target, "inherit")) return SH_NATIVE_EVAL_FAILED;
    if (entity >= 64) return SH_NATIVE_EVAL_FAILED;
    fake_asked |= 1ull << entity;
    fake_class(target, fake_target_class[entity], sizeof(fake_target_class[entity]));
    fake_class(candidate, fake_candidate_class[entity], sizeof(fake_candidate_class[entity]));
    if (entity == 2) {
        snprintf(fake_from, sizeof(fake_from), "%s", from ? from : "");
        snprintf(fake_base, sizeof(fake_base), "%s", base ? base : "");
    }
    snprintf(detail, capacity, "%s", candidate ? "candidate supplied" : "no candidate");
    return fake_by_entity[entity];
}
static void fake_reset(sh_native_eval_status all)
{
    for (size_t i = 0; i < 64; i++) fake_by_entity[i] = all;
    fake_asked = 0; fake_from[0] = fake_base[0] = 0;
}

/* A conductor row both constructs its declaration and compares that exact
 * identity. Repeated rows are separate ordered choices, not a set. */
typedef struct roster_fixture {
    const char *names[3];
    char paths[3][128];
    size_t count;
    int unavailable;
    const char *holder;
    int bad_storage;
    const char *name_parent;
} roster_fixture;
static int roster_read(void *context, const char *name, sh_decl_source *out, int *served, char **replaces)
{
    roster_fixture *f = context;
    const char *text = !strcmp(name, "roster/p") || !strcmp(name, "roster/q") ?
        "{ inherit = \"roster/base\"; }" : !strcmp(name, "roster/base") ?
        "{ class = \"idProp\"; edit = { health = 1; } }" : !strcmp(name, "roster/conductor") ?
        "{ class = \"idAIConductor\"; edit = { } }" : !strcmp(name, "roster/trigger") ?
        "{ class = \"idTrigger\"; edit = { } }" : !strcmp(name, "roster/holder") ? f->holder :
        !strcmp(name,"roster/narrative")?"{ class = \"idNarrativeDirector\"; edit = { } }":
        !strcmp(name,"roster/narrative_parent")?f->name_parent:
        !strcmp(name, "ai/conductor/coop/snapmap_default") || !strcmp(name, "mp/modes/coop/game_challenge") ?
        "{ class = \"idProp\"; edit = { } }" : NULL;
    if (!text) return fixture_read(NULL, name, out, served, replaces);
    char *copy = _strdup(text); if (!copy) return -1;
    *out = (sh_decl_source){copy, strlen(copy)};
    *served = !strcmp(name, "roster/p") || !strcmp(name, "roster/q") || !strcmp(name, "roster/holder");
    return 1;
}
static int roster_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    if(!strcmp(type.name,"idNarrativeDirector") || !strcmp(type.name,"narrativeAction_t") || !strcmp(type.name,"idNarrativeLinkCondition") ||
       !strcmp(type.name,"splineMoverModifier_t") || strstr(type.name,"::narrativeScriptParms_t") || strstr(type.name,"::splineMoverModifierScript_t")){
        shape->kind=SH_DECL_VALUE_OBJECT;return 1;}
    if(!strcmp(type.name,"NameActions") || !strcmp(type.name,"NameConditions") || !strcmp(type.name,"NameModifiers")){
        shape->kind=SH_DECL_VALUE_COLLECTION;shape->item_key="item";shape->count_key="num";
        shape->element=(sh_decl_value_type){!strcmp(type.name,"NameActions")?"narrativeAction_t":!strcmp(type.name,"NameConditions")?"idNarrativeLinkCondition":"splineMoverModifier_t",""};return 1;}
    if(!strcmp(type.name,"idAtomicString") || !strcmp(type.name,"narrativeActionType_t") || !strcmp(type.name,"narrativeConditionType_t")){
        shape->kind=SH_DECL_VALUE_IGNORE;return 1;}
    if (!strcmp(type.name, "idAIConductor") || !strcmp(type.name, "RosterRow") || !strcmp(type.name, "idTrigger")) {
        shape->kind = SH_DECL_VALUE_OBJECT; return 1;
    }
    if (!strcmp(type.name, "RosterList")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"RosterRow", ""};
        shape->item_key = "item"; shape->count_key = "num"; return 1;
    }
    if (!strcmp(type.name, "idDeclEntityDef") && !strcmp(type.ops, "*")) {
        shape->kind = SH_DECL_VALUE_REFERENCE; shape->effects = SH_DECL_EFFECTS_GLOBAL; return 1;
    }
    return schema_describe(context, type, shape);
}
static int roster_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *field)
{
    const char *nf=NULL;
    if(!strcmp(type.name,"idNarrativeDirector"))nf=!strcmp(key,"actions")?"NameActions":!strcmp(key,"conditions")?"NameConditions":!strcmp(key,"modifiers")?"NameModifiers":NULL;
    else if(!strcmp(type.name,"narrativeAction_t"))nf=!strcmp(key,"type")?"narrativeActionType_t":!strcmp(key,"scriptParms")?"narrativeAction_t::narrativeScriptParms_t":NULL;
    else if(!strcmp(type.name,"idNarrativeLinkCondition"))nf=!strcmp(key,"type")?"narrativeConditionType_t":!strcmp(key,"scriptParms")?"idNarrativeLinkCondition::narrativeScriptParms_t":NULL;
    else if(!strcmp(type.name,"splineMoverModifier_t") && !strcmp(key,"Script"))nf="splineMoverModifier_t::splineMoverModifierScript_t";
    else if((strstr(type.name,"::narrativeScriptParms_t") || strstr(type.name,"::splineMoverModifierScript_t")) && !strcmp(key,"scriptFunction"))
        nf=strstr(type.name,"splineMover")?"idAtomicString":"idStr";
    if(nf){*field=(sh_decl_value_type){nf,""};return 1;}
    if (!strcmp(type.name, "idAIConductor") && !strcmp(key, "aiTypeList")) {
        *field = (sh_decl_value_type){"RosterList", ""}; return 1;
    }
    if (!strcmp(type.name, "RosterRow")) {
        if (!strcmp(key, "entityDef")) *field = (sh_decl_value_type){"idDeclEntityDef", "*"};
        else if (!strcmp(key, "aiType")) *field = (sh_decl_value_type){"int", ""};
        else return -1;
        return 1;
    }
    if (!strcmp(type.name, "idTrigger") && (!strcmp(key, "call") || !strcmp(key, "note"))) {
        *field = (sh_decl_value_type){"idStr", ""}; return 1;
    }
    return schema_field(context, type, key, field);
}
static int roster_derives(void *context, const char *name, const char *base)
{ return !strcmp(name, base) || fixture_derives(context, name, base); }
static int roster_storage(void *context, sh_decl_value_type type, const char *key, sh_decl_member_storage *storage)
{
    roster_fixture *f = context;
    if(!strcmp(key,"scriptParms") && (!strcmp(type.name,"narrativeAction_t") || !strcmp(type.name,"idNarrativeLinkCondition"))){
        *storage=(sh_decl_member_storage){!strcmp(type.name,"narrativeAction_t")?0x80:0x10,0x30};return 1;}
    if(!strcmp(type.name,"splineMoverModifier_t") && !strcmp(key,"Script")){*storage=(sh_decl_member_storage){0xc0,0x10};return 1;}
    if(!strcmp(key,"scriptFunction") && (strstr(type.name,"::narrativeScriptParms_t") || strstr(type.name,"::splineMoverModifierScript_t"))){
        *storage=(sh_decl_member_storage){f->bad_storage?8:0,strstr(type.name,"splineMover")?8:0x30};return 1;}
    if(!strcmp(key,"type") && (!strcmp(type.name,"narrativeAction_t") || !strcmp(type.name,"idNarrativeLinkCondition"))){
        *storage=(sh_decl_member_storage){!strcmp(type.name,"narrativeAction_t")?0:4,4};return 1;}
    if (!strcmp(type.name, "idTrigger") && !strcmp(key, "call")) {
        memset(storage, 0, sizeof(*storage)); storage->offset = f->bad_storage ? 0 : 0xca0;
        storage->extent = 0x30; return 1;
    }
    return schema_storage(context, type, key, storage);
}
static int roster_resolve(void *context, const char *type, const char *name, const char **family)
{
    (void)context; (void)name;
    if (strcmp(type, "idDeclEntityDef")) return 0;
    *family = "entitydef"; return 1;
}
static int roster_typed(void *context, size_t index, const char **family, const char **name)
{
    roster_fixture *f = context; if (index >= f->count) return f->unavailable ? -1 : 0;
    *family = "entitydef"; *name = f->names[index]; return 1;
}
static int roster_bound(void *context, size_t index, sh_native_inline_use *use, const char **name,
    const char **kept, sh_decl_carrier_limit *limit)
{
    roster_fixture *f = context; (void)limit; *kept = NULL;
    if (index >= f->count) return 0;
    *use = (sh_native_inline_use){"entity/0", f->paths[index]}; *name = f->names[index]; return 1;
}
static int roster_uses(void *context, size_t entity, const char *name, sh_native_inline_use *uses, size_t capacity)
{
    roster_fixture *f = context; (void)entity; int count = 0;
    for (size_t i = 0; i < f->count; i++) if (!strcmp(name, f->names[i])) {
        if ((size_t)count < capacity) uses[count] = (sh_native_inline_use){"entity/0", f->paths[i]};
        count++;
    }
    return count;
}
typedef struct hint_fixture {
    roster_fixture roster;
    const char *module;
    int unavailable_bank, unavailable_row, reads, lookups;
    int category;
    const char *selected_parent;
    int metadata_mode;
} hint_fixture;
static int hint_read(void *context, const char *name, sh_decl_source *out, int *served, char **replaces)
{
    hint_fixture *f = context;
    if (!strncmp(name, "hint/", 5)) {
        const char *value = "{ class = \"idProp\"; edit = { } }";
        char *text = _strdup(value); if (!text) return -1;
        *out = (sh_decl_source){text, strlen(text)}; *served = 0; return 1;
    }
    return roster_read(&f->roster, name, out, served, replaces);
}
static int hint_decl(void *context, const char *family, const char *name, sh_decl_source *out, int *served)
{
    hint_fixture *f = context; (void)name;
    const char *body = f->metadata_mode == 1 ? "{ edit = { displayNameTag = \"cosmetic only\"; categoryNameTag = \"other\"; } }" :
        f->metadata_mode == 2 && !strcmp(family, "snapeditorentitybank") ?
            "{ edit = { hintWeapon = \"row/changed\"; } }" :
        f->metadata_mode == 3 && !strcmp(family, "snapeditorentitydef") ?
            "{ edit = { moduleAutomation = 99; } }" : "{ edit = { } }";
    char *text = _strdup(body); if (!text) return -1;
    *out = (sh_decl_source){text, strlen(text)}; *served = 0; return 1;
}
static int hint_asset(void *context, const char *path, sh_decl_source *out, int *served)
{
    hint_fixture *f = context; f->reads++;
    if (strcmp(path, "maps/modules/actual.decl")) return 0;
    if (!f->module) return -1;
    char *text = _strdup(f->module); if (!text) return -1;
    *out = (sh_decl_source){text, strlen(text)}; *served = 0; return 1;
}
static int hint_bank(void *context, const sh_native_inline_hint_bank **out)
{
    static sh_native_inline_hint_bank bank = {"actually_loaded_bank", {
        {"row/large", "hint/large", 0}, {"row/medium", "hint/medium", 1},
        {"row/small", "hint/small", 2}, {"row/powerup", "hint/powerup", 3},
        {"row/weapon", "hint/weapon", 3}}};
    hint_fixture *f = context; *out = f->unavailable_bank ? NULL : &bank;
    return f->unavailable_bank ? -1 : 1;
}
static int hint_row(void *context, const char *key, const sh_native_inline_hint_selector **out)
{
    static sh_native_inline_hint_selector row;
    hint_fixture *f = context; f->lookups++; *out = NULL;
    if (f->unavailable_row) return -1;
    if (!*key || !strcmp(key, "missing")) return 0;
    row = (sh_native_inline_hint_selector){"row/actual", key, f->category};
    *out = &row; return 1;
}
static void hint_module_text(char *out, size_t capacity, const char *parent, const char *first, const char *second)
{
    snprintf(out, capacity, "{ edit = { module = { entities = { num = 1; item[0] = { text = \"{ inherit = \\\"%s\\\"; edit = { spawnableEntities = { num = %d; item[0] = \\\"%s\\\"; %s%s%s } } }\"; } } } } }",
        parent, second ? 2 : 1, first, second ? "item[1] = \\\"" : "", second ? second : "", second ? "\\\";" : "");
}
static char *hint_text_token(const char *module)
{
    const char *first = strstr(module, "text = "), *end;
    if (!first) return NULL;
    first += 7; end = first + 1;
    while (*end && *end != '"') { if (*end++ == '\\' && *end) end++; }
    if (!*end) return NULL;
    size_t length = (size_t)(++end - first); char *out = malloc(length + 1);
    if (out) { memcpy(out, first, length); out[length] = 0; } return out;
}
static void hint_two_records(char *out, size_t capacity, const char *first, const char *second)
{
    char *a = hint_text_token(first), *b = hint_text_token(second);
    CHECK(a && b);
    snprintf(out, capacity, "{ edit = { module = { entities = { num = 2; item[0] = { text = %s; } item[1] = { text = %s; } } } } }",
        a ? a : "\"\"", b ? b : "\"\""); free(a); free(b);
}
static void hint_checks(void)
{
    const char *json = "{\"entities\":[],\"instances\":[{\"moduleName\":\"maps/modules/actual.decl\"}]}";
    char left[1400], right[1400], reason[1000];
    hint_fixture a = {0}, b = {0}; a.category = b.category = 0;
    sh_decl_dependency_schema schema = {&a.roster, roster_describe, roster_field, NULL, NULL, NULL, NULL, NULL,
        roster_storage};
    sh_native_inline_source source = {&a, hint_read, roster_derives, &schema};
    source.read_decl = hint_decl; source.resolve_decl = roster_resolve;
    source.read_asset = hint_asset; source.hint_bank = hint_bank; source.hint_row = hint_row;
    sh_native_inline_source derived = source; derived.context = &b;
    sh_native_inline_substitution substitutions[2] = {{"roster/p", "roster/base", 0}, {"roster/q", "roster/base", 0}};
    sh_native_inline_report plan = {0}; plan.substitutions = substitutions; plan.substitution_count = 1;
    hint_module_text(left, sizeof(left), "hint/large", "roster/p", "roster/p");
    hint_module_text(right, sizeof(right), "hint/large", "roster/base", "roster/base");
    a.module = left; b.module = right;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 1); /* duplicate source identity and multiplicity preserved */
    CHECK(a.reads && a.lookups && b.reads && b.lookups);
    b.metadata_mode = 1;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 1); /* unused bank/row presentation never changes hint meaning */
    b.metadata_mode = 2;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0); /* an actually selected bank input does change meaning */
    b.metadata_mode = 3;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0);
    b.metadata_mode = 0;

    hint_module_text(left, sizeof(left), "hint/large", "roster/p", "roster/base");
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0); /* exact P/S comparison domain may not collapse */
    hint_module_text(left, sizeof(left), "hint/large", "roster/p", "roster/q"); plan.substitution_count = 2;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0);
    plan.substitution_count = 1;
    hint_module_text(left, sizeof(left), "hint/large", "roster/p", "roster/p");
    hint_module_text(right, sizeof(right), "hint/large", "roster/base", NULL);
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0); /* no duplicate erasure */
    b.module = left;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0); /* module text is not a fictitious editable map site */
    a.category = b.category = 2;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 1); /* source category 2 is not selector 2's bucket 3 */
    a.category = b.category = 0; a.unavailable_row = 1;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0 && strstr(reason, "incomplete"));
    a.unavailable_row = 0; a.unavailable_bank = 1;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0 && strstr(reason, "incomplete"));
    a.unavailable_bank = 0;
    char one[1400], two[1400], many_left[3200], many_right[3200];
    hint_module_text(one, sizeof(one), "hint/large", "roster/p", NULL);
    hint_module_text(two, sizeof(two), "hint/large", "roster/base", NULL);
    hint_two_records(many_left, sizeof(many_left), one, two);
    hint_two_records(many_right, sizeof(many_right), two, two);
    a.module = many_left; b.module = many_right;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 0 && strstr(reason, "uniqueness batch"));
    hint_two_records(many_left, sizeof(many_left), one, one);
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 1); /* same-source duplicates across separate records */
    hint_module_text(two, sizeof(two), "hint/large", "roster/q", NULL);
    snprintf(many_left, sizeof(many_left), "{ %.*s %.*s }", (int)strlen(one) - 2, one + 1,
        (int)strlen(two) - 2, two + 1);
    a.module = many_left; b.module = two;
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 1); /* repeated list/item/text assignments use the last write */
    snprintf(many_left, sizeof(many_left), "{ %.*s edit = { module = { entities = { num = 0; } entities = { num = 1; } } } }",
        (int)strlen(one) - 2, one + 1);
    b.module = "{ edit = { module = { entities = { num = 1; item[0] = { text = \"\"; } } } } }";
    CHECK(sh_native_inline_observations(&source, json, strlen(json), &plan, &derived, json, strlen(json), reason,
        sizeof(reason)) == 1); /* shrink then grow never resurrects the old text */
}
static void roster_checks(void)
{
    static const struct { const char *names[3]; size_t count, converted; int unavailable; } cases[] = {
        {{"roster/p"}, 1, 1},
        {{"roster/p", "roster/p"}, 2, 2},
        {{"roster/p", "roster/q"}, 2, 1},
        {{"roster/p", "roster/p", "roster/q"}, 3, 2},
        {{"roster/p", "roster/base"}, 2, 0},
        {{"roster/p"}, 1, 0, 1}
    };
    sh_decl_dependency_schema schema = {0};
    schema.describe = roster_describe; schema.field = roster_field; schema.reference = schema_reference;
    for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        roster_fixture f = {0}; f.count = cases[c].count; f.unavailable = cases[c].unavailable;
        char json[2048], error[512] = ""; size_t used = (size_t)snprintf(json, sizeof(json),
            "{\"entities\":[{\"entityDef\":{\"className\":\"idAIConductor\",\"inherit\":\"roster/conductor\","
            "\"state\":{\"edit\":{\"aiTypeList\":{\"num\":%zu", f.count);
        for (size_t i = 0; i < f.count; i++) {
            f.names[i] = cases[c].names[i];
            snprintf(f.paths[i], sizeof(f.paths[i]), "/k:entityDef/k:state/k:edit/k:aiTypeList/k:item%%5b%zu%%5d/k:entityDef", i);
            used += (size_t)snprintf(json + used, sizeof(json) - used,
                ",\"item[%zu]\":{\"aiType\":%zu,\"entityDef\":\"%s\"}", i, i + 3, f.names[i]);
        }
        snprintf(json + used, sizeof(json) - used, "}}},\"~version\":26}}],\"note\": [  1.0 ]}");
        sh_native_inline_source source = {0}; source.context = &f; source.read = roster_read;
        source.derives = roster_derives; source.schema = &schema; source.version = 26; source.evaluate = fake_evaluate;
        source.typed_name = roster_typed; source.bound_use = roster_bound; source.looked_up = roster_uses;
        source.resolve_decl = roster_resolve;
        sh_native_inline_report report = {0}; size_t length = 0, converted = 0;
        fake_reset(SH_NATIVE_EVAL_EQUAL);
        char *out = sh_native_inline_document(json, strlen(json), &source, &length, &report, error, sizeof(error));
        for (size_t i = 0; i < report.count; i++) converted += report.sites[i].converted != 0;
        if (!out || converted != cases[c].converted) {
            fprintf(stderr, "roster case %zu: %s converted %zu/%zu\n", c, error, converted, cases[c].converted);
            for (size_t i = 0; i < report.count; i++) fprintf(stderr, "%s: %s\n", report.sites[i].from, report.sites[i].reason);
        }
        CHECK(out && converted == cases[c].converted);
        CHECK(report.count == f.count - (c == 4 ? 1 : 0));
        CHECK(out && strstr(out, "\"note\": [  1.0 ]"));
        for (size_t i = 0; out && i < f.count; i++) {
            char row[160]; const char *expected = f.names[i];
            for (size_t s = 0; s < report.count; s++) if (report.sites[s].converted && !strcmp(report.sites[s].reference, f.paths[i]))
                expected = "roster/base";
            snprintf(row, sizeof(row), "\"item[%zu]\":{\"aiType\":%zu,\"entityDef\":\"%s\"}", i, i + 3, expected);
            CHECK(strstr(out, row));
        }
        if (c == 2 || c == 3) {
            size_t conflicts = 0;
            for (size_t s = 0; s < report.count; s++) conflicts += report.sites[s].conflict != 0;
            CHECK(conflicts == 1);
        }
        free(out); sh_native_inline_report_free(&report);
    }
}
static int roster_served(void *context, size_t index, const char **family, const char **name)
{
    roster_fixture *f = context; if (index || !f->holder) return 0;
    *family = "entitydef"; *name = "roster/holder"; return 1;
}
static const char *roster_keep(void *context, size_t entity, const char *name, sh_decl_carrier_limit *limit)
{
    (void)context; (void)entity; *limit = SH_DECL_CARRIER_GAP;
    return !strcmp(name, "roster/holder") ? "the served-holder control keeps its source" : NULL;
}
static void roster_script_checks(void)
{
    static const struct { const char *member, *value; int converted, bad_storage; const char *reason; } cases[] = {
        {"call", "roster/p_17.run", 0, 0, "CallScriptFunction entity lookup key"},
        {"call", "roster/base_17.run.more", 0, 0, "CallScriptFunction entity lookup key"},
        {"call", "roster/p_17", 1},
        {"call", "unrelated.run", 1},
        {"note", "roster/p_17", 0, 0, "reader is not enumerated"},
        {"call", "roster/p_17.run", 0, 1, "native reader/storage is not established"}
    };
    for (int served = 0; served < 2; served++) for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
        roster_fixture f = {0}; f.names[0] = "roster/p"; f.count = 1; f.bad_storage = cases[c].bad_storage;
        strcpy(f.paths[0], "/k:entityDef/k:state/k:edit/k:aiTypeList/k:item%5b0%5d/k:entityDef");
        char holder[1024], json[4096], error[512] = "";
        snprintf(holder, sizeof(holder), "{ inherit = \"roster/trigger\"; edit = { %s = \"%s\"; } }",
            cases[c].member, cases[c].value); f.holder = served ? holder : NULL;
        char edit[1024]; snprintf(edit, sizeof(edit), "\"%s\":\"%s\"", cases[c].member, cases[c].value);
        snprintf(json, sizeof(json), "{\"entities\":[{\"entityDef\":{\"className\":\"idAIConductor\","
            "\"inherit\":\"roster/conductor\",\"state\":{\"edit\":{\"aiTypeList\":{\"num\":1,"
            "\"item[0]\":{\"entityDef\":\"roster/p\",\"aiType\":3}}}},\"~version\":26}},"
            "{\"entityDef\":{\"className\":\"idTrigger\",\"inherit\":\"%s\",\"state\":{\"edit\":{%s}},\"~version\":26}}]}",
            served ? "roster/holder" : "roster/trigger", served ? "" : edit);
        sh_decl_dependency_schema schema = {0}; schema.context = &f; schema.describe = roster_describe;
        schema.field = roster_field; schema.reference = schema_reference; schema.storage = roster_storage;
        sh_native_inline_source source = {0}; source.context = &f; source.read = roster_read; source.schema = &schema;
        source.derives = roster_derives; source.version = 26; source.evaluate = fake_evaluate; source.keep = roster_keep;
        source.typed_name = roster_typed; source.bound_use = roster_bound; source.looked_up = roster_uses;
        source.resolve_decl = roster_resolve; source.served_decl = roster_served;
        sh_native_inline_report report = {0}; size_t length = 0; fake_reset(SH_NATIVE_EVAL_EQUAL);
        char *out = sh_native_inline_document(json, strlen(json), &source, &length, &report, error, sizeof(error));
        const sh_native_inline_site *choice = NULL;
        for (size_t s = 0; s < report.count; s++) if (report.sites[s].owner && !strcmp(report.sites[s].from, "roster/p"))
            choice = report.sites + s;
        int ok = out && choice && choice->converted == cases[c].converted &&
            (!cases[c].reason || strstr(choice->reason, cases[c].reason));
        if (!ok) fprintf(stderr, "script case %zu served %d: %s\n", c, served, choice ? choice->reason : error);
        CHECK(ok);
        if (!served) CHECK(out && strstr(out, edit));
        if (served) CHECK(!strcmp(f.holder, holder));
        free(out); sh_native_inline_report_free(&report);
    }
}
static void roster_scoped_script_checks(void)
{
    static const struct {const char *record,*value,*tag;int converted,bad_storage,inherited;} cases[]={
        {"actions","roster/p_17.function","10",0},
        {"actions","roster/base_17.function","10",0},
        {"actions","roster/p_17.function","1",1},
        {"actions","unrelated.function","10",1},
        {"actions","roster/p_17","10",1},
        {"actions","roster/p_17.function",NULL,0},
        {"actions","unrelated.function",NULL,1},
        {"actions","roster/p_17.function","10",0,1},
        {"conditions","roster/p_17.function","14",0},
        {"conditions","roster/p_17.function","1",1},
        {"modifiers","roster/p_17.function",NULL,0},
        {"actions","roster/p_17.function",NULL,0,0,1}
    };
    for(int served=0;served<2;served++)for(size_t c=0;c<sizeof cases/sizeof *cases;c++){
        roster_fixture f={0};f.names[0]="roster/p";f.count=1;f.bad_storage=cases[c].bad_storage;
        strcpy(f.paths[0],"/k:entityDef/k:state/k:edit/k:aiTypeList/k:item%5b0%5d/k:entityDef");
        char holder[2048],edit[2048],json[8192],error[512]={0},tag[80]="",text_tag[80]="";
        if(cases[c].tag){snprintf(tag,sizeof tag,"\"type\":%s,",cases[c].tag);snprintf(text_tag,sizeof text_tag,"type=%s;",cases[c].tag);}
        const char *block=!strcmp(cases[c].record,"modifiers")?"Script":"scriptParms";
        snprintf(edit,sizeof edit,"\"%s\":{\"num\":1,\"item[0]\":{%s\"%s\":{\"scriptFunction\":\"%s\"}}}",cases[c].record,tag,block,cases[c].value);
        f.name_parent="{inherit=\"roster/narrative\";edit={actions={num=1;item[0]={type=10;};};};}";
        const char *base=cases[c].inherited?"roster/narrative_parent":"roster/narrative";
        snprintf(holder,sizeof holder,"{inherit=\"%s\";edit={%s={num=1;item[0]={%s %s={scriptFunction=\"%s\";};};};};}",base,cases[c].record,text_tag,block,cases[c].value);
        f.holder=served?holder:NULL;
        snprintf(json,sizeof json,"{\"entities\":[{\"entityDef\":{\"className\":\"idAIConductor\",\"inherit\":\"roster/conductor\",\"state\":{\"edit\":{\"aiTypeList\":{\"num\":1,\"item[0]\":{\"entityDef\":\"roster/p\",\"aiType\":3}}}},\"~version\":26}},{\"entityDef\":{\"className\":\"idNarrativeDirector\",\"inherit\":\"%s\",\"state\":{\"edit\":{%s}},\"~version\":26}}]}",served?"roster/holder":base,served?"":edit);
        sh_decl_dependency_schema schema={0};schema.context=&f;schema.describe=roster_describe;schema.field=roster_field;schema.reference=schema_reference;schema.storage=roster_storage;
        sh_native_inline_source source={0};source.context=&f;source.read=roster_read;source.schema=&schema;source.derives=roster_derives;source.version=26;
        source.evaluate=fake_evaluate;source.keep=roster_keep;source.typed_name=roster_typed;source.bound_use=roster_bound;source.looked_up=roster_uses;source.resolve_decl=roster_resolve;source.served_decl=roster_served;
        sh_native_inline_report report={0};size_t length=0;fake_reset(SH_NATIVE_EVAL_EQUAL);
        char *out=sh_native_inline_document(json,strlen(json),&source,&length,&report,error,sizeof error);
        const sh_native_inline_site *choice=NULL;for(size_t s=0;s<report.count;s++)if(report.sites[s].owner && !strcmp(report.sites[s].from,"roster/p"))choice=report.sites+s;
        int ok=out && choice && choice->converted==cases[c].converted;
        if(!ok)fprintf(stderr,"scoped script case%zu served%d: %s\n",c,served,choice?choice->reason:error);
        CHECK(ok);if(out && !served)CHECK(strstr(out,edit));if(served)CHECK(!strcmp(f.holder,holder));
        free(out);sh_native_inline_report_free(&report);
    }
}

/* One served use per run: whether it converts, and part of its reason. */
static void pool_case(sh_native_inline_source *source, const char *line, const char *document, int converts,
    const char *reason, sh_decl_carrier_limit limit)
{
    sh_native_inline_report report = {0};
    size_t length = 0; char error[256];
    char *out = sh_native_inline_document(document, strlen(document), source, &length, &report, error, sizeof(error));
    const sh_native_inline_site *site = out ? site_for(&report, 0) : NULL;
    int ok = site && site->converted == converts && (!reason || strstr(site->reason, reason)) &&
        (converts || site->limit == limit);
    if (!ok) fprintf(stderr, "pool case %s: %s\n", line, site ? site->reason : (out ? "no site" : error));
    CHECK(ok);
    free(out); sh_native_inline_report_free(&report);
}
/* R-295 h2j: a served declaration the generic grammar does not read is
 * indexed by its words (gate-14's served animweb player/fp_hands_mp refused
 * every conversion as "does not parse"). */
static void served_checks(sh_native_inline_source *source)
{
    int (*listed)(void *, size_t, const char **, const char **) = source->served_decl;
    source->served_decl = fixture_served_decl;
    fake_reset(SH_NATIVE_EVAL_EQUAL);
    for (size_t i = 0; i < 5; i++) {
        char why[160];
        sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){served_fixture[i].text,
            strlen(served_fixture[i].text)}, why, sizeof(why));
        CHECK(!tree);
        sh_decl_tree_free(tree);
    }
#define SERVED_DOC(note) "{\"entities\":[" ENTITY("pkg/prop", "{\"edit\":{}}", "26") "],\"note\":\"" note "\"}"
    /* Dormant, it selects nothing: the placed copy converts. */
    served_listed[0] = 0; served_listed_count = 1;
    pool_case(source, "served words dormant", SERVED_DOC("-"), 1, NULL, SH_DECL_CARRIER_EXACT);
    /* Spelled by a map value, its words name the placed copy's definition: a
     * use whose reader is not traced keeps the package. Dormant, the same
     * text selects nothing. */
    served_listed[0] = 1;
    pool_case(source, "served words live", SERVED_DOC("web/live"), 0, "served animweb declaration 'web/live', which play "
        "instantiates (spelled by a map value), names 'pkg/prop' in text the generic grammar does not read (indexed by its "
        "words)", SH_DECL_CARRIER_GAP);
    pool_case(source, "served words live dormant", SERVED_DOC("-"), 1, NULL, SH_DECL_CARRIER_EXACT);
    /* Its words make a served definition they name live, and that
     * definition's own untraced use keeps the package. */
    served_listed[0] = 2; served_listed[1] = 5; served_listed_count = 2;
    pool_case(source, "served words relay", SERVED_DOC("web/relay"), 0, "(named by served 'web/relay'), names 'pkg/prop' at "
        "/k:edit/k:target", SH_DECL_CARRIER_GAP);
    pool_case(source, "served words relay dormant", SERVED_DOC("-"), 1, NULL, SH_DECL_CARRIER_EXACT);
    /* Tokens written with no separation are one word as well. */
    served_listed[0] = 3; served_listed[1] = 6;
    pool_case(source, "served words joined", SERVED_DOC("web/joined"), 0, "(named by served 'web/joined'), names 'pkg/prop' "
        "at /k:edit/k:target", SH_DECL_CARRIER_GAP);
    /* Text the declaration lexer refuses still refuses every conversion. */
    served_listed[0] = 4; served_listed_count = 1;
    pool_case(source, "served unlexable", SERVED_DOC("-"), 0, "served declaration 'md6/unlexable' does not parse, and the "
        "declaration lexer does not read it: ", SH_DECL_CARRIER_GAP);
    /* An entityDef's walks read typed fields: one the generic grammar does
     * not read still refuses. */
    served_listed[0] = 7;
    pool_case(source, "served entitydef unparsed", SERVED_DOC("-"), 0, "served declaration 'pkg/broken_def' does not parse",
        SH_DECL_CARRIER_GAP);
#undef SERVED_DOC
    served_listed_count = 0;
    source->served_decl = listed;
}
/* One use whose refusal leaves the document byte-identical, or whose
 * conversion emits want and not unwanted. */
static void ordered_case(sh_native_inline_source *source, const char *line, const char *document, int converts,
    const char *reason, sh_decl_carrier_limit limit, const char *want, const char *unwanted)
{
    sh_native_inline_report report = {0};
    size_t length = 0; char error[256];
    char *out = sh_native_inline_document(document, strlen(document), source, &length, &report, error, sizeof(error));
    const sh_native_inline_site *site = out ? site_for(&report, 0) : NULL;
    int ok = site && site->converted == converts && (!reason || strstr(site->reason, reason)) &&
        (converts ? (!want || strstr(out, want)) && (!unwanted || !strstr(out, unwanted)) :
            site->limit == limit && !strcmp(out, document));
    if (!ok) fprintf(stderr, "ordered case %s: %s | %s\n", line, site ? site->reason : error, out ? out : "");
    CHECK(ok);
    free(out); sh_native_inline_report_free(&report);
}
/* Ordered native writes through the converter (M80). */
static void ordered_checks(sh_native_inline_source *source)
{
    fake_reset(SH_NATIVE_EVAL_EQUAL);
    /* Without a native ancestor the first stateful layer keeps health = 1; 2;
     * the child updates the first: health = 3; 2. The carrier reads members by
     * key, but plain complete assignments with nothing between them reduce to
     * the last (R-212): the hidden first-match override drops and the
     * evaluator receives the same proof for both prepared texts. */
    fake_reduced_equal = fake_reduced_refused = -2;
    ordered_case(source, "root first match", "{\"entities\":[" ENTITY("pkg/rr_child", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"health\":2", "\"health\":3");
    CHECK(fake_reduced_equal == 1 && fake_reduced_refused == 1);
    /* The stateless root itself reduces the same way. */
    ordered_case(source, "root repeats", "{\"entities\":[" ENTITY("pkg/rr_root", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"health\":2", "\"health\":1");
    /* An instance value that a later repeated occurrence hides is kept with
     * its package, never silently dropped. */
    ordered_case(source, "root hidden instance", "{\"entities\":[" ENTITY("pkg/rr_root", "{\"edit\":{\"health\":5}}", "26") "]}",
        0, "holds the instance's own value", SH_DECL_CARRIER_GAP, NULL, NULL);
    /* Without the evaluator's EQUAL the reduced layer never converts. */
    fake_reset(SH_NATIVE_EVAL_UNSUPPORTED);
    ordered_case(source, "root reduction unsupported", "{\"entities\":[" ENTITY("pkg/rr_child", "{\"edit\":{}}", "26") "]}", 0,
        "native evaluation does not cover a value", SH_DECL_CARRIER_GAP, NULL, NULL);
    fake_reset(SH_NATIVE_EVAL_EQUAL);
    /* Distinct resources on repeated registered reads: both are read. The
     * carrier's key lookup alone proves no native limit, so this stays a
     * product gap (M81 corrects M80's ENGINE). */
    ordered_case(source, "root global", "{\"entities\":[" ENTITY("pkg/rr_globalchild", "{\"edit\":{}}", "26") "]}", 0,
        "is not established", SH_DECL_CARRIER_GAP, NULL, NULL);
    /* Over a stateful native base later layers update the first match: a new
     * block collapses its own repeats and a repeated block merges into it. */
    ordered_case(source, "new block", "{\"entities\":[" ENTITY("pkg/nb", "{\"edit\":{}}", "26") "]}", 1, NULL,
        SH_DECL_CARRIER_EXACT, "\"gain\":2", "\"gain\":1");
    /* Headers are positional (M86): a repeated inherit is inert update-mode
     * state text, whatever it names; every edit statement of a later layer
     * updates the inherited state in order, other names are inert. */
    ordered_case(source, "repeated header", "{\"entities\":[" ENTITY("pkg/rr_decl", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"health\":1", NULL);
    ordered_case(source, "inert other inherit", "{\"entities\":[" ENTITY("pkg/hd_other", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"snapmaps/props/base\"", "ai/unrelated");
    ordered_case(source, "update edits", "{\"entities\":[" ENTITY("pkg/hd_twice", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"speed\":2", NULL);
    ordered_case(source, "update edits both", "{\"entities\":[" ENTITY("pkg/hd_twice", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"health\":1", NULL);
    ordered_case(source, "inert other block", "{\"entities\":[" ENTITY("pkg/hd_inert", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"speed\":2", "\"health\":7");
    /* The first stateful layer contributes only its first statement. */
    ordered_case(source, "first stateful edits", "{\"entities\":[" ENTITY("pkg/rs_hd_first", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"health\":1", "\"speed\"");
    /* Its name is not read (M87; failing before: coverage): over a stateless
     * base the inline layer states it as edit. */
    ordered_case(source, "first statement named", "{\"entities\":[" ENTITY("pkg/rs_hd_named", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"health\":1", NULL);
    /* A later layer updates it by that name, and its edit is inert. */
    ordered_case(source, "named update", "{\"entities\":[" ENTITY("pkg/rs_dn_child", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"health\":2", "\"speed\"");
    /* The instance's own edit is inert under that name too: one inline layer
     * would make its values effective, so the package stays. */
    ordered_case(source, "named inert instance", "{\"entities\":[" ENTITY("pkg/rs_dn_child", "{\"edit\":{\"speed\":3}}", "26") "]}",
        0, "this instance's values are inert", SH_DECL_CARRIER_GAP, NULL, NULL);
    /* Over a native base whose state has another name an update cannot ride
     * in the instance's edit; an inert edit changes nothing and converts with
     * the instance's values kept. */
    ordered_case(source, "native named update", "{\"entities\":[" ENTITY("pkg/db_update", "{\"edit\":{}}", "26") "]}", 0,
        "statement is named 'data'", SH_DECL_CARRIER_GAP, NULL, NULL);
    ordered_case(source, "native named inert", "{\"entities\":[" ENTITY("pkg/db_inert", "{\"edit\":{\"speed\":7}}", "26") "]}",
        1, NULL, SH_DECL_CARRIER_EXACT, "\"speed\":7", "\"speed\":1");
    /* An inherit after class is state text: no parent, a scalar first state. */
    ordered_case(source, "late inherit", "{\"entities\":[" ENTITY("pkg/rr_hd_late", "{\"edit\":{}}", "26") "]}", 0,
        "state statement is not a block", SH_DECL_CARRIER_GAP, NULL, NULL);
    /* The post-pass counts the first num-prefixed member (case-sensitive), so
     * an appended layer's count is its first occurrence while the reader keeps
     * the last; a plain field named so is never reduced. NumLives is not
     * counted and reduces. It prunes only item-prefixed names ending in ']'
     * (VK 141a65fa0 / GL 141a58470): a plain itemCount reduces, item[1] does
     * not. */
    ordered_case(source, "root counted", "{\"entities\":[" ENTITY("pkg/rr_num", "{\"edit\":{}}", "26") "]}", 0,
        "counts num-prefixed members", SH_DECL_CARRIER_GAP, NULL, NULL);
    ordered_case(source, "root uncounted", "{\"entities\":[" ENTITY("pkg/rr_Num", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"NumLives\":2", "\"NumLives\":1");
    ordered_case(source, "root item count", "{\"entities\":[" ENTITY("pkg/rr_item", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, "\"itemCount\":2", "\"itemCount\":1");
    CHECK(reduce_case(source->schema, "{ edit = { itemCount = 1; itemCount = 2; } }", 1, NULL));
    CHECK(reduce_case(source->schema, "{ edit = { item[1] = 1; item[1] = 2; } }", 0, "prunes indexed items"));
    CHECK(reduce_case(source->schema, "{ edit = { numLives = 1; numLives = 2; } }", 0, "counts num-prefixed"));
    /* A bitfield token goes to its setter (VK 141a20600 / GL 141a12840),
     * whose storage and effect the metadata does not describe; a name the
     * type lacks is skipped. */
    CHECK(reduce_case(source->schema, "{ edit = { flag = 1; flag = 0; } }", 0, "neither a complete assignment"));
    CHECK(reduce_case(source->schema, "{ edit = { health = 1; flag = 1; health = 2; } }", 0, "not proved to leave"));
    CHECK(reduce_case(source->schema, "{ edit = { obsolete = 1; obsolete = 2; } }", 1, NULL));
    /* A pool based on a stateless native definition reads rs_root (1; 2) and
     * then rs_child (3): 3. The prepared fold updates the hidden first
     * occurrence (3; 2): 2. One inline layer cannot give both. */
    {
        const char *world = world_text, *private_world = world_private, *pool = pool_text; int served = world_served;
        int (*read_decl)(void *, const char *, const char *, sh_decl_source *, int *) = source->read_decl;
        world_text = "{ inherit = \"pkg/world_private\"; edit = { } }"; world_served = 1;
        world_private = "{ class = \"idWorldspawn\"; edit = { aiSpawnPoolDecl = \"pool/a\"; } }";
        pool_text = "{ edit = { enabled = true; spawnPoolEntries = { num = 2; item[0] = { entityDef = \"snapmaps/props/empty\"; "
            "poolSize = 2; } item[1] = { entityDef = \"ai/unrelated\"; poolSize = 1; } } } }";
        source->read_decl = fixture_read_decl;
        ordered_case(source, "pooled stateless later write", "{\"entities\":[" ENTITY("pkg/rs_child", "{\"edit\":{}}", "26") "]}",
            0, "is written by a later one", SH_DECL_CARRIER_GAP, NULL, NULL);
        /* Without a later write the pool's reads agree with the reduced fold. */
        ordered_case(source, "pooled stateless repeats", "{\"entities\":[" ENTITY("pkg/rs_root", "{\"edit\":{}}", "26") "]}",
            1, NULL, SH_DECL_CARRIER_EXACT, "\"health\":2", "\"health\":1");
        world_text = world; world_private = private_world; pool_text = pool; world_served = served; source->read_decl = read_decl;
    }
}
#define POOL_ENTRIES(a, b) "{ edit = { enabled = true; spawnPoolEntries = { num = 2; item[0] = { entityDef = \"" a \
    "\"; poolSize = 2; } item[1] = { entityDef = \"" b "\"; poolSize = 1; } } } }"
/* The world's AI spawn pool (R-203): default, unread, map/package/private
 * configuration, enabled/disabled/unaffected uses, base order, suppressed
 * names and per-layer folding. */
static void pool_checks(sh_native_inline_source *source)
{
    const char *stock = world_text;
    const char *prop = "{\"entities\":[" ENTITY("pkg/prop", "{\"edit\":{\"health\":99}}", "26") "]}";
    const char *prop2 = "{\"entities\":[" ENTITY("pkg/prop2", "{\"edit\":{}}", "26") "]}";
    const char *shrink = "{\"entities\":[" ENTITY("pkg/shrink", "{\"edit\":{\"list\":{\"num\":3}}}", "26") "]}";
    const char *nested = "{\"entities\":[" ENTITY("pkg/nested", "{\"edit\":{\"nested\":{\"gain\":4}}}", "26") "]}";
    const char *reset = "{\"entities\":[" ENTITY("pkg/nested_reset", "{\"edit\":{\"nested\":{\"gain\":4}}}", "26") "]}";
    const char *configured = "{ inherit = \"pkg/world_private\"; edit = { } }";
    fake_reset(SH_NATIVE_EVAL_EQUAL);
    /* Default world: the stock definition names no pool, so the use converts. */
    pool_case(source, "stock", prop, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* An unread world proves nothing (failing before: it converted). */
    world_text = NULL;
    pool_case(source, "absent world", prop, 0, "world definition 'worldspawn' is unavailable", SH_DECL_CARRIER_GAP);
    /* A served world whose private parent names a pool. */
    world_text = configured; world_served = 1;
    world_private = "{ class = \"idWorldspawn\"; edit = { aiSpawnPoolDecl = \"pool/a\"; } }";
    pool_text = NULL;
    pool_case(source, "absent pool", prop, 0, "AI spawn pool 'pool/a', which is absent", SH_DECL_CARRIER_GAP);
    source->read_decl = NULL;
    pool_text = POOL_ENTRIES("snapmaps/props/base", "ai/unrelated");
    pool_case(source, "no typed read", prop, 0, "which cannot be read here", SH_DECL_CARRIER_GAP);
    source->read_decl = fixture_read_decl;
    /* A nearer layer clearing the name wins the merge. */
    world_text = "{ inherit = \"pkg/world_private\"; edit = { aiSpawnPoolDecl = \"\"; } }";
    pool_case(source, "cleared", prop, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* The world chain is positional (M86): a repeated inherit is inert state
     * text and each later edit updates the state in order (failing before:
     * the repeat left the pool unresolved). */
    world_text = "{ inherit = \"pkg/world_private\"; inherit = \"pkg/missing\"; edit = { } edit = { aiSpawnPoolDecl = \"\"; } }";
    pool_case(source, "positional world cleared", prop, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* The root-most stateful layer contributes its first statement only: its
     * later edit clearing the name is inert, so pool/a stays configured. */
    world_text = configured;
    world_private = "{ class = \"idWorldspawn\"; edit = { aiSpawnPoolDecl = \"pool/a\"; } edit = { aiSpawnPoolDecl = \"\"; } }";
    pool_text = NULL;
    pool_case(source, "positional world inert", prop, 0, "AI spawn pool 'pool/a', which is absent", SH_DECL_CARRIER_GAP);
    world_private = "{ class = \"idWorldspawn\"; edit = { aiSpawnPoolDecl = \"pool/a\"; } }";
    pool_text = POOL_ENTRIES("snapmaps/props/base", "ai/unrelated");
    world_text = configured;
    /* Same native base for both forms: scalar and object overlays fold. */
    pool_case(source, "native base", prop, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_case(source, "native base nested", nested, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* A separate layer read applies only its first statement (M87; failing
     * before: any later statement was coverage). */
    const char *multi = "{\"entities\":[" ENTITY("pkg/pm_multi", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "layer later statement", multi, 1, NULL, SH_DECL_CARRIER_EXACT);
    const char *other = "{\"entities\":[" ENTITY("pkg/pm_other", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "layer first other", other, 0, "applies its first state statement 'data'", SH_DECL_CARRIER_GAP);
    /* One list written by P and by the instance: per-layer reads can differ
     * (failing before: the evaluator's EQUAL converted it). */
    pool_case(source, "two list writers", shrink, 0, "'edit/list' has more than one folded writer", SH_DECL_CARRIER_GAP);
    /* A package reset the instance overrides survives the carrier, but has no
     * effect in a per-layer read (failing before: it converted). */
    pool_case(source, "reset", reset, 0, "a reset in 'pkg/nested_reset' has no effect", SH_DECL_CARRIER_GAP);
    world_text = stock; world_served = 0;
    pool_case(source, "reset unpooled", reset, 1, NULL, SH_DECL_CARRIER_EXACT);
    world_text = configured; world_served = 1;
    /* Disabled, the same two-writer use reaches the evaluator and converts. */
    pool_text = "{ edit = { enabled = false; spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_case(source, "disabled", shrink, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* An unrelated entry selects no base for either form. */
    pool_text = POOL_ENTRIES("ai/unrelated", "ai/other");
    pool_case(source, "unaffected", shrink, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* The first matching entry decides: a package layer as the base cannot
     * be derived from natively; a native ancestor listed first is shared. */
    pool_text = POOL_ENTRIES("pkg/prop", "snapmaps/props/base");
    pool_case(source, "package base", prop2, 0, "selects 'pkg/prop' of this package chain", SH_DECL_CARRIER_ENGINE);
    pool_text = POOL_ENTRIES("snapmaps/props/base", "pkg/prop");
    pool_case(source, "native first", prop2, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* Suppressed names: this copy's generated name (uniqueId 1) before or
     * after conversion is refused; another copy's and an ordinary name are not. */
    pool_text = "{ edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } }"
        " suppressedEntityDefNames = { num = 2; item[0] = \"ai/other\"; item[1] = \"3_mod/pkg/prop_1\"; } } }";
    pool_case(source, "generated suppressed", prop, 0, "'3_mod/pkg/prop_1', which can be this placed copy's generated",
        SH_DECL_CARRIER_GAP);
    pool_text = "{ edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } }"
        " suppressedEntityDefNames = { num = 1; item[0] = \"0_mod/snapmaps/props/base_1\"; } } }";
    pool_case(source, "generated converted suppressed", prop, 0, "'0_mod/snapmaps/props/base_1'", SH_DECL_CARRIER_GAP);
    pool_text = "{ edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } }"
        " suppressedEntityDefNames = { num = 1; item[0] = \"3_mod/pkg/prop_7\"; } } }";
    pool_case(source, "other copy suppressed", prop, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_text = POOL_ENTRIES("0_mod/pkg/prop_1", "snapmaps/props/base");
    pool_case(source, "generated entry", prop, 0, "'0_mod/pkg/prop_1'", SH_DECL_CARRIER_GAP);
    /* Module identity (play map builder VK 1404ee8e0): instanceEntities groups
     * uniqueIds by module instance, then the GlobalModule; the index part
     * then decides, and the GlobalModule name too. */
#define SUPPRESS(n) "{ edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } }" \
    " suppressedEntityDefNames = { num = 1; item[0] = \"" n "\"; } } }"
#define PLACED(instances, keys, values) "{\"entities\":[" ENTITY("pkg/prop", "{\"edit\":{\"health\":99}}", "26") "]," \
    "\"instances\":[" instances "],\"instanceEntities\":{\"keyValues\":[" keys "],\"values\":[" values "]}}"
    const char *in_module = PLACED("{\"moduleName\":\"maps/modules/a.decl\"},{\"moduleName\":\"maps/modules/b.decl\"}", "0,0,1,1", "1");
    const char *in_global = PLACED("{\"moduleName\":\"maps/modules/a.decl\"}", "0,0,1", "1");
    const char *twice = PLACED("{\"moduleName\":\"maps/modules/a.decl\"}", "0,1,2", "1,1");
    pool_text = SUPPRESS("0_mod/pkg/prop_1");
    pool_case(source, "other module index", in_module, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_case(source, "any module unplaced", prop, 0, "'0_mod/pkg/prop_1'", SH_DECL_CARRIER_GAP);
    pool_case(source, "ambiguous placement", twice, 0, "'0_mod/pkg/prop_1'", SH_DECL_CARRIER_GAP);
    pool_text = SUPPRESS("1_b/pkg/prop_1");
    pool_case(source, "own module index", in_module, 0, "'1_b/pkg/prop_1'", SH_DECL_CARRIER_GAP);
    pool_text = SUPPRESS("1_b/snapmaps/props/base_1");
    pool_case(source, "own module converted", in_module, 0, "'1_b/snapmaps/props/base_1'", SH_DECL_CARRIER_GAP);
    /* The loaded module name (LoadSnapModule VK 1405b7e80): no extension or
     * leading maps/modules/, then the file base after '/' or '\'. */
    pool_text = SUPPRESS("1_a/pkg/prop_1");
    pool_case(source, "other module name", in_module, 1, NULL, SH_DECL_CARRIER_EXACT);
    const char *stripped = PLACED("{\"moduleName\":\"maps/modules/maps/modules/d\\\\s/c.v2.decl\"}", "0,1,1", "1");
    pool_text = SUPPRESS("0_c.v2/pkg/prop_1");
    pool_case(source, "module name base", stripped, 0, "'0_c.v2/pkg/prop_1'", SH_DECL_CARRIER_GAP);
    pool_text = SUPPRESS("0_s/pkg/prop_1");
    pool_case(source, "module name not dir", stripped, 1, NULL, SH_DECL_CARRIER_EXACT);
    const char *unnamed = PLACED("{\"moduleName\":7}", "0,1,1", "1");
    pool_case(source, "module name unknown", unnamed, 0, "'0_s/pkg/prop_1'", SH_DECL_CARRIER_GAP);
    pool_text = SUPPRESS("1_GlobalModule/pkg/prop_1");
    pool_case(source, "global module", in_global, 0, "'1_GlobalModule/pkg/prop_1'", SH_DECL_CARRIER_GAP);
    pool_text = SUPPRESS("1_other/pkg/prop_1");
    pool_case(source, "global module name", in_global, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_text = SUPPRESS("0_GlobalModule/pkg/prop_1");
    pool_case(source, "global module index", in_global, 1, NULL, SH_DECL_CARRIER_EXACT);
#undef PLACED
#undef SUPPRESS
    pool_text = "{ edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } }"
        " suppressedEntityDefNames = { num = 1; item[0] = \"ai/other\"; } } }";
    pool_case(source, "ordinary suppressed", prop, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* aiPoolNumbers Parse (R-204/R-205): constructor default enabled, a
     * found parent's whole object, own num prescanned before items, items
     * outside the count skipped, the edit/data body keyword. The two-writer
     * use refuses only where the pool selects the native base. */
    pool_text = "{ edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_case(source, "default enabled", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    pool_b_text = POOL_ENTRIES("ai/unrelated", "snapmaps/props/base");
    pool_text = "{ inherit = \"pool/b\"; edit = { } }";
    pool_case(source, "inherited pool", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    pool_text = "{ inherit = \"pool/b\"; edit = { spawnPoolEntries = { num = 1; } } }";
    pool_case(source, "inherited truncated", shrink, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_b_text = "{ edit = { enabled = false; spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_text = "{ inherit = \"pool/b\"; edit = { } }";
    pool_case(source, "inherited disabled", shrink, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_text = "{ inherit = \"pool/b\"; edit = { enabled = true; } }";
    pool_case(source, "child enables", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    pool_b_text = POOL_ENTRIES("ai/x", "ai/y");
    pool_text = "{ inherit = \"pool/b\"; edit = { spawnPoolEntries = { item[1] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_case(source, "item overlay", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    /* Shrink then regrow clears the dropped slot instead of restoring it. */
    pool_c_text = POOL_ENTRIES("ai/x", "snapmaps/props/base");
    pool_b_text = "{ inherit = \"pool/c\"; edit = { spawnPoolEntries = { num = 1; } } }";
    pool_text = "{ inherit = \"pool/b\"; edit = { spawnPoolEntries = { num = 2; } } }";
    pool_case(source, "shrink regrow", shrink, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_text = "{ inherit = \"pool/b\"; edit = { spawnPoolEntries = { num = 2; item[1] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_case(source, "regrow written", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    pool_text = "{ inherit = \"pool/missing\"; edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_case(source, "absent parent", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    pool_text = "{ edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"ai/x\"; } item[1] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_case(source, "item beyond count", shrink, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_text = "{ edit = { spawnPoolEntries = { item[0] = { entityDef = \"snapmaps/props/base\"; } num = 1; } } }";
    pool_case(source, "num prescanned", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    pool_text = "{ data = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_case(source, "data body", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    pool_text = "{ other = 1; edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } } } }";
    pool_case(source, "unread body", shrink, 1, NULL, SH_DECL_CARRIER_EXACT);
    pool_text = "{ edit = { spawnPoolEntries = { num = 1; item[0] = { entityDef = \"snapmaps/props/base\"; } } } junk = 2; }";
    pool_case(source, "trailing member", shrink, 0, "has more than one folded writer", SH_DECL_CARRIER_GAP);
    /* Rejected input and unsupported valid forms stay explicit gaps. */
    pool_text = "{ edit = { spawnPoolEntries = ! { num = 1; item[0] = { entityDef = \"pkg/prop\"; } } } }";
    pool_case(source, "reset pool", prop, 0, "which the native list reader rejects", SH_DECL_CARRIER_GAP);
    pool_text = "{ EDIT = { } }";
    pool_case(source, "keyword case", prop, 0, "member 'EDIT' is not modelled", SH_DECL_CARRIER_GAP);
    pool_text = "{ edit = { spawnPoolEntries = { num = many; } } }";
    pool_case(source, "count form", prop, 0, "num is not a plain count", SH_DECL_CARRIER_GAP);
    /* Registered readers: one layer's reads in their order fold; two
     * layers' reads, or a fold that reorders them, do not. */
    pool_text = POOL_ENTRIES("snapmaps/props/base", "ai/other");
    const char *readers2 = "{\"entities\":[" ENTITY("pkg/readers2", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "two reader layers", readers2, 0, "registered readers in 'pkg/readers2' and 'pkg/prop'", SH_DECL_CARRIER_GAP);
    const char *ordchild = "{\"entities\":[" ENTITY("pkg/ordchild", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "reordered reads", ordchild, 0, "the fold reorders the registered read of 'edit/model'", SH_DECL_CARRIER_GAP);
    const char *ordkept = "{\"entities\":[" ENTITY("pkg/ordkept", "{\"edit\":{\"speed\":3}}", "26") "]}";
    pool_case(source, "ordered reads", ordkept, 1, NULL, SH_DECL_CARRIER_EXACT);
    /* expandInheritance selects only the definition's own prepared text: the
     * carrier rebuild never writes it and the carrier's root-first merge reads
     * each ancestor's own slice, so a placed copy of an unexpanded definition
     * (own false or 0, or false on an ancestor) carries the whole chain. A
     * conversion that dropped the parent's health would change the copy. */
    ordered_case(source, "own unexpanded", "{\"entities\":[" ENTITY("pkg/xunexp", "{\"edit\":{}}", "26") "]}", 1, NULL,
        SH_DECL_CARRIER_EXACT, ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":21,\"speed\":22}}", "26"), "poolCount");
    ordered_case(source, "own zero", "{\"entities\":[" ENTITY("pkg/xzero", "{\"edit\":{}}", "26") "]}", 1, NULL,
        SH_DECL_CARRIER_EXACT, ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":21,\"speed\":25}}", "26"), NULL);
    ordered_case(source, "ancestor unexpanded", "{\"entities\":[" ENTITY("pkg/xchild", "{\"edit\":{}}", "26") "]}", 1,
        NULL, SH_DECL_CARRIER_EXACT, ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":21,\"speed\":23}}", "26"), NULL);
    ordered_case(source, "cased flag", "{\"entities\":[" ENTITY("pkg/xbad", "{\"edit\":{}}", "26") "]}", 0,
        "'expandInheritance'", SH_DECL_CARRIER_GAP, NULL, NULL);
    ordered_case(source, "quoted flag", "{\"entities\":[" ENTITY("pkg/xquoted", "{\"edit\":{}}", "26") "]}", 0,
        "'expandInheritance'", SH_DECL_CARRIER_GAP, NULL, NULL);
    /* idStr's reader assigns a quoted token: repeated writes fold and do not
     * count as a reader layer; another form stays an unmodelled reader. */
    const char *labels = "{\"entities\":[" ENTITY("pkg/labelchild", "{\"edit\":{\"label\":\"c\"}}", "26") "]}";
    pool_case(source, "assigned strings", labels, 1, NULL, SH_DECL_CARRIER_EXACT);
    const char *bare = "{\"entities\":[" ENTITY("pkg/labelbare", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "unquoted string", bare, 0, "'edit/label' is read more than once", SH_DECL_CARRIER_GAP);
    const char *assign_reader = "{\"entities\":[" ENTITY("pkg/labelreader", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "assign beside reader", assign_reader, 1, NULL, SH_DECL_CARRIER_EXACT);
    const char *instance_reader = "{\"entities\":[" ENTITY("pkg/prop", "{\"edit\":{\"model\":\"md6/i\"}}", "26") "]}";
    pool_case(source, "instance reader", instance_reader, 0, "registered readers in 'pkg/prop' and 'the instance'", SH_DECL_CARRIER_GAP);
    /* Effect classes (M77): a FIELD read in one layer beside a GLOBAL read in
     * another folds, as do repeated complete number assignments; a second
     * FIELD writer, a block or exponent beside a number, and any reader whose
     * effects are not modelled, even alone, are refused. */
    const char *tint_global = "{\"entities\":[" ENTITY("pkg/tintglobal", "{\"edit\":{\"delay\":4}}", "26") "]}";
    pool_case(source, "field beside global", tint_global, 1, NULL, SH_DECL_CARRIER_EXACT);
    const char *tint_twice = "{\"entities\":[" ENTITY("pkg/tinttwice", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "field twice", tint_twice, 0, "'edit/tint' is read more than once", SH_DECL_CARRIER_GAP);
    const char *delay_block = "{\"entities\":[" ENTITY("pkg/delayblock", "{\"edit\":{\"delay\":{\"value\":4}}}", "26") "]}";
    pool_case(source, "number blocks", delay_block, 0, "'edit/delay' is read more than once", SH_DECL_CARRIER_GAP);
    const char *delay_neg = "{\"entities\":[" ENTITY("pkg/delayneg", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "negative number", delay_neg, 1, NULL, SH_DECL_CARRIER_EXACT);
    const char *delay_quoted = "{\"entities\":[" ENTITY("pkg/delayquoted", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "quoted number", delay_quoted, 0, "'edit/delay' is read more than once", SH_DECL_CARRIER_GAP);
    const char *opaque = "{\"entities\":[" ENTITY("pkg/opaque", "{\"edit\":{}}", "26") "]}";
    pool_case(source, "unmodelled reader", opaque, 0, "the registered reader of 'edit/opaque' has effects not modelled",
        SH_DECL_CARRIER_GAP);
    world_text = stock; world_served = 0; world_private = NULL; pool_text = pool_b_text = pool_c_text = NULL;
}

/* ---- production schema (M78): the native reflection adapter selects each
 * reader by its registered address, and the converter walks those shapes. ---- */
typedef struct pr_region { void *data; size_t size; } pr_region;
static pr_region pr_regions[256];
static size_t pr_count;
static unsigned char *pr_records, *pr_objects, *pr_pointers;
static void *pr_alloc(size_t size)
{
    void *memory = calloc(1, size);
    if (!memory || pr_count >= sizeof(pr_regions) / sizeof(pr_regions[0])) abort();
    pr_regions[pr_count++] = (pr_region){memory, size}; return memory;
}
static void pr_ptr(void *memory, size_t offset, const void *value)
{ uintptr_t address = (uintptr_t)value; memcpy((char *)memory + offset, &address, sizeof(address)); }
static void pr_num(void *memory, size_t offset, int32_t value) { memcpy((char *)memory + offset, &value, sizeof(value)); }
static const char *pr_text(const char *value)
{ size_t size = strlen(value) + 1; char *copy = (char *)pr_alloc(size); memcpy(copy, value, size); return copy; }
static int pr_read(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    for (size_t i = 0; i < pr_count; i++) {
        uintptr_t start = (uintptr_t)pr_regions[i].data;
        if (address >= start && address - start <= pr_regions[i].size &&
            length <= pr_regions[i].size - (size_t)(address - start)) { memcpy(out, (const void *)address, length); return 1; }
    }
    return 0;
}
static unsigned char *pr_type(size_t index, const char *name, const char *super, uintptr_t object, uintptr_t pointer)
{
    unsigned char *record = pr_records + index * 0x38;
    pr_ptr(record, 0, pr_text(name)); pr_ptr(record, 8, pr_text(super));
    pr_ptr(pr_objects, index * 16 + 8, (const void *)object); pr_ptr(pr_pointers, index * 16 + 8, (const void *)pointer);
    return record;
}
static unsigned char *pr_fields(unsigned char *record, size_t offset, size_t rows)
{ unsigned char *fields = (unsigned char *)pr_alloc((rows + 1) * 0x48); pr_ptr(record, offset, fields); return fields; }
static void pr_field(unsigned char *fields, size_t index, const char *name, const char *type, const char *ops, int32_t size)
{
    unsigned char *row = fields + index * 0x48;
    pr_ptr(row, 0, pr_text(type)); pr_ptr(row, 8, pr_text(ops)); pr_ptr(row, 0x10, pr_text(name)); pr_num(row, 0x1c, size);
}
/* The variable's offset (+0x18) and a type's stored bytes (record +0x10). */
static void pr_at(unsigned char *fields, size_t index, int32_t offset) { pr_num(fields + index * 0x48, 0x18, offset); }
static int pr_reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{ (void)context; (void)path; (void)type; (void)name; (void)length; return 1; }
static void pr_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{ (void)context; (void)path; (void)type; (void)reason; }
#define PR_USE(def, edit) "{\"entities\":[" ENTITY(def, edit, "26") "]}"
static void production_checks(sh_native_inline_source *source)
{
    const size_t count = 13;
    unsigned char *reflect = (unsigned char *)pr_alloc(0xc0), *container = (unsigned char *)pr_alloc(0x30), *fields;
    const sh_decl_dependency_schema *fake = source->schema;
    const char *world = world_text, *private_world = world_private, *pool = pool_text;
    int served = world_served;
    char error[256] = "";
    pr_records = (unsigned char *)pr_alloc((count + 1) * 0x38);
    pr_objects = (unsigned char *)pr_alloc((count + 1) * 16); pr_pointers = (unsigned char *)pr_alloc((count + 1) * 16);
    pr_ptr(reflect, 0, container); pr_ptr(container, 0x20, pr_records);
    pr_ptr(reflect, 0x88, pr_objects); pr_num(reflect, 0x90, (int32_t)count + 1);
    pr_ptr(reflect, 0xa0, pr_pointers); pr_num(reflect, 0xa8, (int32_t)count + 1);
    pr_type(0, "idDecl", "", 0, 0x10);
    pr_type(1, "idDeclEntityDef", "idDecl", 0, 0x10);
    fields = pr_fields(pr_type(2, "idProp", "", 0, 0), 0x20, 25);
    pr_field(fields, 0, "owner", "Base", "*", 8);
    pr_field(fields, 1, "shape", "idTypeInfoObjectPtr < Base >", "", 16);
    pr_field(fields, 2, "falloff", "idBoundedFloat < falloff_t >", "", 16);
    pr_field(fields, 3, "voice", "idVoiceTrack", "*", 8);
    pr_field(fields, 4, "tint", "idColor", "", 16);
    pr_field(fields, 5, "health", "int", "", 4);
    /* hp shares health's bytes; armor2 shares inner's b (80 + 4). idColor
     * states no stored size, and flag is a bitfield (negative +0x1c). */
    pr_field(fields, 6, "hp", "int", "", 4);
    pr_field(fields, 7, "armor", "int", "", 4);
    pr_field(fields, 8, "flag", "bool", "", -1);
    pr_field(fields, 9, "inner", "Inner", "", 8);
    pr_field(fields, 10, "armor2", "int", "", 4);
    {
        static const int32_t offsets[] = {0, 8, 24, 40, 48, 64, 64, 68, 0, 80, 84};
        for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) pr_at(fields, i, offsets[i]);
    }
    {
        /* M84: arrays store count * size(T); bits and rogue are bitfields
         * whose setters (variable +0x38) clear and set byte 120: bits's OR
         * stays inside its cleared bit 1, rogue's reaches kept bits. */
        static const unsigned char bits_code[] = {0x80, 0x61, 0x78, 0xfd, 0xf7, 0xda, 0x1a, 0xc0, 0x24, 0x02,
            0x08, 0x41, 0x78, 0xc3};
        static const unsigned char rogue_code[] = {0x80, 0x61, 0x78, 0xfb, 0xf7, 0xda, 0x1a, 0xc0, 0x08, 0x41, 0x78,
            0xc3};
        unsigned char *bits = (unsigned char *)pr_alloc(32), *rogue = (unsigned char *)pr_alloc(32);
        memcpy(bits, bits_code, sizeof(bits_code)); memcpy(rogue, rogue_code, sizeof(rogue_code));
        pr_field(fields, 11, "vals", "int", "[4]", 16); pr_at(fields, 11, 96);
        pr_field(fields, 12, "over", "int", "", 4); pr_at(fields, 12, 108);
        pr_field(fields, 13, "after", "int", "", 4); pr_at(fields, 13, 112);
        pr_field(fields, 14, "alias", "int", "", 4); pr_at(fields, 14, 8);
        pr_field(fields, 15, "bits", "bool", "", -1); pr_ptr(fields + 15 * 0x48, 0x38, bits);
        pr_field(fields, 16, "word", "int", "", 4); pr_at(fields, 16, 120);
        pr_field(fields, 17, "rogue", "bool", "", -1); pr_ptr(fields + 17 * 0x48, 0x38, rogue);
        pr_field(fields, 18, "grid", "int", "[2][3]", 24); pr_at(fields, 18, 128);
        pr_field(fields, 19, "ptrs", "Base", "*[4]", 32); pr_at(fields, 19, 160);
        pr_field(fields, 20, "odd", "int", "[x]", 4); pr_at(fields, 20, 192);
        /* nums is an idList: a 24-byte header at 200 whose elements the list
         * reader keeps on the heap; inHeader (216) is inside the header. */
        pr_field(fields, 21, "nums", "idList < int >", "", 24); pr_at(fields, 21, 200);
        pr_field(fields, 22, "inHeader", "int", "", 4); pr_at(fields, 22, 216);
        pr_field(fields, 23, "afterList", "int", "", 4); pr_at(fields, 23, 224);
        {
            /* count3: an int bitfield whose setter stores bits 3..5 of the
             * dword at 124 (and, and edx 7, shl 3, or, ret). */
            static const unsigned char count_code[] = {0x83, 0x61, 0x7c, 0xc7, 0x83, 0xe2, 0x07, 0xc1, 0xe2, 0x03,
                0x09, 0x51, 0x7c, 0xc3};
            unsigned char *count_setter = (unsigned char *)pr_alloc(32);
            memcpy(count_setter, count_code, sizeof(count_code));
            pr_field(fields, 24, "count3", "int", "", -3); pr_ptr(fields + 24 * 0x48, 0x38, count_setter);
        }
        {
            unsigned char *list = pr_type(12, "idList < int >", "", 0xd0, 0), *element = (unsigned char *)pr_alloc(16);
            pr_ptr(element, 0, pr_text("int")); pr_ptr(element, 8, pr_text(""));
            pr_ptr(list, 0x18, element); pr_num(list, 0x10, 24);
        }
    }
    fields = pr_fields(pr_type(11, "Inner", "", 0, 0), 0x20, 2);
    pr_field(fields, 0, "a", "int", "", 4); pr_field(fields, 1, "b", "int", "", 4); pr_at(fields, 1, 4);
    pr_num(pr_records + 11 * 0x38, 0x10, 8);
    /* A selected object is separate storage: its gain at 56 would meet health
     * (64) only if projected through shape's offset 8. */
    fields = pr_fields(pr_type(3, "Base", "", 0, 0), 0x20, 1);
    pr_field(fields, 0, "gain", "int", "", 4); pr_at(fields, 0, 56);
    pr_field(pr_fields(pr_type(4, "Derived", "Base", 0, 0), 0x20, 1), 0, "falloff", "idBoundedFloat < falloff_t >", "", 16);
    pr_field(pr_fields(pr_type(5, "idTypeInfoObjectPtr < Base >", "", 0xa0, 0), 0x18, 1), 0, "element", "Base", "", 16);
    pr_type(6, "idBoundedFloatBase", "", 0x141, 0);
    pr_type(7, "idBoundedFloat < falloff_t >", "", 0x141, 0);
    pr_type(8, "idVoiceTrack", "", 0, 0x150);
    pr_type(9, "idColor", "", 0xc0, 0);
    pr_type(10, "int", "", 0, 0);
    pr_num(pr_records + 10 * 0x38, 0x10, 4);
    pr_num(pr_records + 5 * 0x38, 0x10, 16); pr_num(pr_records + 7 * 0x38, 0x10, 16);
    sh_decl_native_source native_source = {NULL, pr_read, (uintptr_t)reflect, NULL};
    sh_decl_dependency_observer observer = {NULL, pr_reference, pr_gap};
    sh_decl_native_schema *native = sh_decl_native_schema_open(native_source, observer, error, sizeof(error));
    if (!native) fprintf(stderr, "production schema: %s\n", error);
    CHECK(native != NULL);
    if (native) {
        source->schema = sh_decl_native_schema_view(native);
        fake_reset(SH_NATIVE_EVAL_EQUAL);
        world_text = "{ inherit = \"pkg/world_private\"; edit = { } }"; world_served = 1;
        world_private = "{ class = \"idWorldspawn\"; edit = { aiSpawnPoolDecl = \"pool/a\"; } }";
        pool_text = POOL_ENTRIES("snapmaps/props/base", "ai/unrelated");
        /* A selected class with modelled members converts, and so do nested
         * and direct bounded members: their setters clamp into the field (M79).
         * A layer repeating a member is coverage, never a parse failure. */
        pool_case(source, "prod selected", PR_USE("pkg/pr_shape", "{\"edit\":{}}"), 1, NULL, SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod nested bounded", PR_USE("pkg/pr_shape_bounded", "{\"edit\":{}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod bounded", PR_USE("pkg/pr_bounded", "{\"edit\":{}}"), 1, NULL, SH_DECL_CARRIER_EXACT);
        /* Under the chosen pool both package layers are read separately; their
         * two bounded writes fold only because the setter assigns totally. */
        pool_case(source, "prod bounded layers", PR_USE("pkg/pr_bounded_child", "{\"edit\":{}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* A package write and an instance write also convert. */
        pool_case(source, "prod bounded twice", PR_USE("pkg/pr_bounded", "{\"edit\":{\"falloff\":2}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* One layer's repeats are its own writers, in occurrence order: total
         * assignments fold to the last; a registered reader's earlier read and
         * an earlier NULL before a skipped form cannot. */
        ordered_case(source, "prod repeat", PR_USE("pkg/pr_repeat", "{\"edit\":{}}"), 1, NULL, SH_DECL_CARRIER_EXACT,
            "\"falloff\":2", "\"falloff\":1");
        ordered_case(source, "prod repeated voice", PR_USE("pkg/pr_voice2", "{\"edit\":{}}"), 0,
            "read more than once by a registered reader", SH_DECL_CARRIER_GAP, NULL, NULL);
        ordered_case(source, "prod null then skip in one layer", PR_USE("pkg/pr_nullskip", "{\"edit\":{}}"), 0,
            "keeping an earlier NULL", SH_DECL_CARRIER_GAP, NULL, NULL);
        ordered_case(source, "prod skip then null in one layer", PR_USE("pkg/pr_skipnull", "{\"edit\":{}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT, "\"owner\":null", NULL);
        /* The package's call constructs the selected class and the instance's
         * finds the voice track, so their global effects regroup. */
        pool_case(source, "prod two readers", PR_USE("pkg/pr_shape", "{\"edit\":{\"voice\":\"vo/x\"}}"), 0,
            "read in separate calls", SH_DECL_CARRIER_GAP);
        /* A skipped value keeps the package's NULL, which the fold drops. */
        pool_case(source, "prod null then skip", PR_USE("pkg/pr_null", "{\"edit\":{\"owner\":\"x\"}}"), 0,
            "keeping an earlier NULL", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod skip then null", PR_USE("pkg/pr_skip", "{\"edit\":{\"owner\":null}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* Storage order (R-213). The instance's health is read after the
         * layer's armor, while the fold keeps health first: the type reader's
         * stored bytes (int 4 at 64 and 68) prove the writes commute. */
        pool_case(source, "prod disjoint reorder", PR_USE("pkg/po_order", "{\"edit\":{\"health\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* hp and health share bytes: the pool ends with health = 3, the fold
         * with hp = 2. In the same order (hp last) the fold agrees. */
        pool_case(source, "prod shared reorder", PR_USE("pkg/po_alias", "{\"edit\":{\"health\":3}}"), 0,
            "changes the order of the writes to 'edit/health' and 'edit/hp'", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod shared same order", PR_USE("pkg/po_alias", "{\"edit\":{\"hp\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* A partial read with no stored size: armor's writes on both sides of
         * it stay unfinished; all of them after it keep the order. */
        pool_case(source, "prod unknown partial", PR_USE("pkg/po_tint", "{\"edit\":{\"armor\":5}}"), 0,
            "'edit/armor' and 'edit/tint', whose storage is not proved disjoint", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod unknown same side", PR_USE("pkg/po_tint2", "{\"edit\":{\"armor\":5}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* Nested inline offsets add up: inner's b (84) meets armor2, a (80)
         * does not. A selected object's members are not projected. */
        pool_case(source, "prod nested shared", PR_USE("pkg/po_inner", "{\"edit\":{\"inner\":{\"b\":3}}}"), 0,
            "'edit/armor2' and 'edit/inner/b'", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod nested disjoint", PR_USE("pkg/po_inner2", "{\"edit\":{\"inner\":{\"a\":3}}}"), 1,
            NULL, SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod indirect object", PR_USE("pkg/po_shape", "{\"edit\":{\"health\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* A bitfield setter is a write of undescribed storage: kept in order it
         * folds; crossed by health, or called by two layers, it does not. */
        pool_case(source, "prod setter same side", PR_USE("pkg/po_flag2", "{\"edit\":{\"health\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod setter crossed", PR_USE("pkg/po_flag", "{\"edit\":{\"health\":3}}"), 0,
            "'edit/flag' and 'edit/health'", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod setter twice", PR_USE("pkg/po_flagchild", "{\"edit\":{}}"), 0,
            "'edit/flag' is written more than once through its bitfield setter", SH_DECL_CARRIER_GAP);
        {
            const sh_decl_dependency_schema *view = source->schema;
            sh_decl_member_storage at;
            const sh_decl_value_type prop = {"idProp", ""};
            CHECK(view->storage(view->context, prop, "vals", &at) == 1 && at.offset == 96 && at.extent == 16);
            CHECK(view->storage(view->context, prop, "grid", &at) == 1 && at.offset == 128 && at.extent == 24);
            CHECK(view->storage(view->context, prop, "ptrs", &at) == 1 && at.offset == 160 && at.extent == 32);
            CHECK(view->storage(view->context, prop, "odd", &at) == -1);
            CHECK(view->storage(view->context, prop, "bits", &at) == 2 && at.offset == 120 && at.extent == 1 &&
                at.bit_first == 1 && at.bit_count == 1);
            CHECK(view->storage(view->context, prop, "rogue", &at) == 2 && at.extent == 0);
            CHECK(view->storage(view->context, prop, "flag", &at) == 2 && at.extent == 0 && !at.assigns);
            CHECK(view->storage(view->context, prop, "bits", &at) == 2 && at.assigns == SH_DECL_ASSIGN_BOOLEAN);
            CHECK(view->storage(view->context, prop, "count3", &at) == 2 && at.offset == 124 && at.extent == 4 &&
                at.bit_first == 3 && at.bit_count == 3 && at.assigns == SH_DECL_ASSIGN_INTEGER);
        }
        /* M84. A fixed array stores count * size(T) from its offset (vals at
         * 96..112) and each item at index * size(T): over (108) is vals[3]'s
         * storage, not vals[0]'s, and after (112) is outside the array. */
        pool_case(source, "prod array item crossed", PR_USE("pkg/pa_over", "{\"edit\":{\"over\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod array disjoint", PR_USE("pkg/pa_after", "{\"edit\":{\"after\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* A selected object stores only its own member (8..24); the selected
         * object is separate storage, never projected inline. Crossed, alias
         * (8) meets it although gain projected to 64 would not, and health
         * (64) commutes although that projection would meet it. */
        pool_case(source, "prod selected alias", PR_USE("pkg/ps_alias", "{\"edit\":{\"alias\":3}}"), 0,
            "'edit/alias' and 'edit/shape'", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod selected crossed", PR_USE("pkg/ps_health", "{\"edit\":{\"health\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* A verified setter stores bit 1 of byte 120: crossed health commutes,
         * word (120..124) does not; rogue's store stays unknown. */
        pool_case(source, "prod setter verified", PR_USE("pkg/pb_bits", "{\"edit\":{\"health\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod setter word", PR_USE("pkg/pb_word", "{\"edit\":{\"word\":3}}"), 0,
            "'edit/bits' and 'edit/word'", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod setter rogue", PR_USE("pkg/pb_rogue", "{\"edit\":{\"health\":3}}"), 0,
            "'edit/health' and 'edit/rogue'", SH_DECL_CARRIER_GAP);
        /* A verified setter assigns its bits completely from a proved token
         * (bool: true or false; int: an integer, optionally negative), so
         * two layers may write it; another token stays a described accessor
         * write, and a single layer's repeats reduce to the last. */
        pool_case(source, "prod setter assigns twice", PR_USE("pkg/pb_false", "{\"edit\":{}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod setter unproved token", PR_USE("pkg/pb_one", "{\"edit\":{}}"), 0,
            "'edit/bits' is written more than once through its bitfield setter", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod int setter twice", PR_USE("pkg/pi_child", "{\"edit\":{}}"), 1, NULL, SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod int setter float", PR_USE("pkg/pi_float", "{\"edit\":{}}"), 0,
            "float spelling of a field whose native reader is unknown", SH_DECL_CARRIER_GAP);
        ordered_case(source, "prod int setter repeat", PR_USE("pkg/pi_repeat", "{\"edit\":{}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT, "\"count3\":5", "\"count3\":2");
        /* A list stores only its header inline (200..224): crossed, inHeader
         * meets it, while afterList commutes although eight elements projected
         * from the header (200..232) would meet it. */
        pool_case(source, "prod list header", PR_USE("pkg/pl_header", "{\"edit\":{\"inHeader\":3}}"), 0,
            "'edit/inHeader' and 'edit/nums'", SH_DECL_CARRIER_GAP);
        pool_case(source, "prod list after", PR_USE("pkg/pl_after", "{\"edit\":{\"afterList\":3}}"), 1, NULL,
            SH_DECL_CARRIER_EXACT);
        /* Fixed-array items are separate writes merged by key: the instance
         * may write other or the same items; over (108) meets only vals[3]. */
        pool_case(source, "prod array items", PR_USE("pkg/pa_two", "{\"edit\":{\"vals\":{\"vals[1]\":5,\"vals[2]\":6}}}"),
            1, NULL, SH_DECL_CARRIER_EXACT);
        pool_case(source, "prod array item overlap", PR_USE("pkg/pa_item3", "{\"edit\":{\"over\":3}}"), 0,
            "'edit/over' and 'edit/vals/vals[3]'", SH_DECL_CARRIER_GAP);
        sh_decl_native_schema_close(native);
    }
    source->schema = fake; world_text = world; world_private = private_world; pool_text = pool; world_served = served;
    while (pr_count) free(pr_regions[--pr_count].data);
}

typedef struct drop_fixture {
    const char *native, *parent, *own;
    int wrong_family, unresolved;
} drop_fixture;
static int drop_read(void *context, const char *family, const char *name, sh_decl_source *text, int *served)
{
    drop_fixture *f = context;
    const char *body = !strcmp(name, "stock/drop") ? f->native : !strcmp(name, "package/parent") ? f->parent :
        !strcmp(name, "package/drop") ? f->own : NULL;
    if (strcmp(family, "lootdrop") || !body) return 0;
    char *copy = _strdup(body);
    if (!copy) return -1;
    *text = (sh_decl_source){copy, strlen(copy)}; *served = strcmp(name, "stock/drop") != 0; return 1;
}
static int drop_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "idDeclLootDrop") || !strcmp(type.name, "nested")) shape->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "int")) { shape->kind = SH_DECL_VALUE_IGNORE; shape->plain = 1; }
    else if (!strcmp(type.name, "string")) {
        shape->kind = SH_DECL_VALUE_IGNORE; shape->assigns = SH_DECL_ASSIGN_QUOTED; shape->effects = SH_DECL_EFFECTS_FIELD;
    } else if (!strcmp(type.name, "idEntityDefRef")) {
        shape->kind = SH_DECL_VALUE_REFERENCE; shape->element = (sh_decl_value_type){"idDeclEntityDef", SH_DECL_BY_NAME_DECL_OPS};
        shape->assigns = SH_DECL_ASSIGN_QUOTED; shape->effects = SH_DECL_EFFECTS_FIELD;
    } else if (!strcmp(type.name, "idDeclEntityDef") && !strcmp(type.ops, "*")) shape->kind = SH_DECL_VALUE_REFERENCE;
    else if (!strcmp(type.name, "event")) shape->kind = SH_DECL_VALUE_EVENT_CALL;
    else if (!strcmp(type.name, "list")) shape->kind = SH_DECL_VALUE_COLLECTION;
    else return 0;
    return 1;
}
static int drop_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *field)
{
    (void)context; (void)type;
    if (!strcmp(key, "entityDef")) *field = (sh_decl_value_type){"idDeclEntityDef", "*"};
    else if (!strcmp(key, "nested")) *field = (sh_decl_value_type){"nested", ""};
    else if (!strcmp(key, "label")) *field = (sh_decl_value_type){"string", ""};
    else if (!strcmp(key, "byName")) *field = (sh_decl_value_type){"idEntityDefRef", ""};
    else if (!strcmp(key, "amount")) *field = (sh_decl_value_type){"int", ""};
    else if (!strcmp(key, "eventCall")) *field = (sh_decl_value_type){"event", ""};
    else if (!strcmp(key, "items")) *field = (sh_decl_value_type){"list", ""};
    else return -1;
    return 1;
}
static int drop_resolve(void *context, const char *type, const char *name, const char **family)
{
    drop_fixture *f = context; (void)type; (void)name;
    if (f->unresolved) return -1;
    *family = f->wrong_family ? "damage" : "entitydef"; return 1;
}
static void drop_checks(void)
{
    static const char baseline[] = "{ edit = { entityDef = \"stock/item\"; amount = 4; label = \"stock/item\"; "
        "byName = \"stock/item\"; nested = { entityDef = \"stock/item\"; label = \"stock/item\"; amount = 4; } } }";
    drop_fixture f = {baseline};
    sh_decl_dependency_schema schema = {&f, drop_describe, drop_field};
    sh_native_inline_source source = {0}; source.context = &f; source.schema = &schema;
    source.read_decl = drop_read; source.resolve_decl = drop_resolve;
    sh_native_inline_substitution substitution = {"package/item", "stock/item", 0};
    static const struct { const char *label, *parent, *own, *reason; int equal; } cases[] = {
        {"pointer", NULL, "{ inherit = \"stock/drop\"; edit = { entityDef = \"package/item\"; } }", NULL, 1},
        {"nested", NULL, "{ inherit = \"stock/drop\"; edit = { nested = { entityDef = \"package/item\"; amount = 4; } } }", NULL, 1},
        {"nested partial", NULL, "{ inherit = \"stock/drop\"; edit = { nested = { amount = 4; } } }", NULL, 1},
        {"repeated idempotent", NULL, "{ inherit = \"stock/drop\"; edit = { amount = 4; amount = 4; } }", NULL, 1},
        {"served parent", "{ inherit = \"stock/drop\"; edit = { nested = { entityDef = \"package/item\"; } } }",
            "{ inherit = \"package/parent\"; edit = { amount = 4; entityDef = \"package/item\"; } }", NULL, 1},
        {"served empty child", "{ inherit = \"stock/drop\"; edit = { entityDef = \"package/item\"; } }",
            "{ inherit = \"package/parent\"; }", NULL, 1},
        {"changed nested", NULL, "{ inherit = \"stock/drop\"; edit = { nested = { amount = 5; } } }", "edit.nested.amount", 0},
        {"string identity", NULL, "{ inherit = \"stock/drop\"; edit = { entityDef = \"package/item\"; label = \"package/item\"; } }", "edit.label", 0},
        {"nested string identity", NULL, "{ inherit = \"stock/drop\"; edit = { nested = { label = \"package/item\"; } } }", "edit.nested.label", 0},
        {"by-name identity", NULL, "{ inherit = \"stock/drop\"; edit = { byName = \"package/item\"; } }", "edit.byName", 0},
        {"event identity", NULL, "{ inherit = \"stock/drop\"; edit = { eventCall = { eventDef = \"spawn\"; } } }", "edit.eventCall", 0},
        {"list coverage", NULL, "{ inherit = \"stock/drop\"; edit = { items = { num = 1; item[0] = 1; } } }", "edit.items", 0},
        {"unknown coverage", NULL, "{ inherit = \"stock/drop\"; edit = { custom = 1; } }", "edit.custom", 0},
        {"reset coverage", NULL, "{ inherit = \"stock/drop\"; edit = { nested = ! { amount = 4; } } }", "edit.nested", 0},
        {"repeated changed", NULL, "{ inherit = \"stock/drop\"; edit = { amount = 5; amount = 4; } }", "edit.amount", 0},
        {"changed parent", "{ inherit = \"stock/drop\"; edit = { amount = 5; } }",
            "{ inherit = \"package/parent\"; edit = { amount = 4; } }", "edit.amount", 0},
        {"cycle", "{ inherit = \"package/drop\"; }", "{ inherit = \"package/parent\"; }", "inherits itself", 0},
        {"missing ancestor", NULL, "{ inherit = \"package/absent\"; }", "no source provides", 0},
        {"null versus name", NULL, "{ inherit = \"stock/drop\"; edit = { entityDef = NULL; } }", "edit.entityDef", 0},
    };
    char native[512], reason[1024];
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        f.parent = cases[i].parent; f.own = cases[i].own; *reason = 0;
        int result = sh_native_inline_test_lootdrop(&source, "package/drop", &substitution, 1,
            native, sizeof(native), reason, sizeof(reason));
        if (result != cases[i].equal || (cases[i].reason && !strstr(reason, cases[i].reason)))
            fprintf(stderr, "drop %s: result %d (%s) native %s\n", cases[i].label, result, reason, native);
        CHECK(result == cases[i].equal && (!cases[i].equal || !strcmp(native, "stock/drop")) &&
            (!cases[i].reason || strstr(reason, cases[i].reason)));
    }
    f.parent = NULL; f.own = cases[0].own;
    f.wrong_family = 1;
    CHECK(sh_native_inline_test_lootdrop(&source, "package/drop", &substitution, 1, native, sizeof(native), reason, sizeof(reason)) == 0);
    f.wrong_family = 0; f.unresolved = 1;
    CHECK(sh_native_inline_test_lootdrop(&source, "package/drop", &substitution, 1, native, sizeof(native), reason, sizeof(reason)) == 0);
    f.unresolved = 0;
    CHECK(sh_native_inline_test_lootdrop(&source, "package/drop", NULL, 0, native, sizeof(native), reason, sizeof(reason)) == 0);
    f.native = "{ edit = { entityDef = NULL; } }";
    f.own = "{ inherit = \"stock/drop\"; edit = { entityDef = \"NULL\"; } }";
    CHECK(sh_native_inline_test_lootdrop(&source, "package/drop", NULL, 0, native, sizeof(native), reason, sizeof(reason)) == 0);
    f.own = "{ inherit = \"stock/drop\"; edit = { entityDef = NULL; } }";
    CHECK(sh_native_inline_test_lootdrop(&source, "package/drop", NULL, 0, native, sizeof(native), reason, sizeof(reason)) == 1);
}

/* A large carrier-free map (R-388): 4096 placed stock entities, each naming
 * its own declaration, under a product that serves a loot drop. With one
 * served placed copy the census is built over the whole map, and each push
 * compares only nodes of its own family and name (it compared every node
 * before, which stalled the save of a 4091-entity map for minutes). With no
 * served copy and no typed use nothing can convert, so the census is not
 * built at all and the document comes back exact with no site. */
#define SCALE_ENTITIES 4096
static int scale_read(void *context, const char *name, sh_decl_source *out, int *served, char **replaces)
{
    (void)context; (void)replaces;
    const char *text = !strcmp(name, "scale/served") ? "{ inherit = \"scale/stock/0\"; }" :
        !strncmp(name, "scale/", 6) || !strcmp(name, "ai/conductor/coop/snapmap_default") ||
        !strcmp(name, "mp/modes/coop/game_challenge") ? "{ class = \"idProp\"; edit = { } }" : NULL;
    if (!text) return 0;
    char *copy = _strdup(text); if (!copy) return -1;
    *out = (sh_decl_source){copy, strlen(copy)}; *served = !strcmp(name, "scale/served"); return 1;
}
static int scale_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "idProp")) { shape->kind = SH_DECL_VALUE_OBJECT; return 1; }
    if (!strcmp(type.name, "idDeclEntityDef") && !strcmp(type.ops, "*")) { shape->kind = SH_DECL_VALUE_REFERENCE; return 1; }
    return 0;
}
static int scale_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *field)
{
    (void)context;
    if (strcmp(type.name, "idProp") || strcmp(key, "item")) return -1;
    *field = (sh_decl_value_type){"idDeclEntityDef", "*"}; return 1;
}
static int scale_derives(void *context, const char *name, const char *base) { (void)context; return !strcmp(name, base); }
static int scale_resolve(void *context, const char *type, const char *name, const char **family)
{ (void)context; (void)name; if (strcmp(type, "idDeclEntityDef")) return 0; *family = "entitydef"; return 1; }
static int scale_served_decl(void *context, size_t index, const char **family, const char **name)
{ (void)context; if (index) return 0; *family = "lootdrop"; *name = "scale/drop"; return 1; }
static const char *scale_keep(void *context, size_t entity, const char *name, sh_decl_carrier_limit *limit)
{ (void)context; (void)entity; (void)name; *limit = SH_DECL_CARRIER_GAP; return "the scale control keeps its package"; }
static void census_scale_checks(void)
{
    sh_decl_dependency_schema schema = {0};
    schema.describe = scale_describe; schema.field = scale_field; schema.reference = schema_reference;
    size_t capacity = SCALE_ENTITIES * 256 + 64, used = 0;
    char *json = malloc(capacity), error[512] = "";
    CHECK(json != NULL);
    if (!json) return;
    for (int served = 1; served >= 0; served--) {
        used = (size_t)snprintf(json, capacity, "{\"entities\":[");
        for (size_t i = 0; i < SCALE_ENTITIES; i++) {
            char inherit[32];
            if (served && i == 0) snprintf(inherit, sizeof(inherit), "scale/served");
            else snprintf(inherit, sizeof(inherit), "scale/stock/%zu", i);
            used += (size_t)snprintf(json + used, capacity - used, "%s{\"entityDef\":{\"className\":\"idProp\",\"inherit\":\"%s\","
                "\"state\":{\"edit\":{\"item\":\"scale/def/%zu\"}},\"~version\":26},\"uniqueId\":%zu}", i ? "," : "", inherit, i,
                i + 1);
        }
        snprintf(json + used, capacity - used, "],\"version\":111}");
        sh_native_inline_source source = {0}; source.read = scale_read; source.derives = scale_derives;
        source.schema = &schema; source.version = 26; source.evaluate = fake_evaluate; source.resolve_decl = scale_resolve;
        source.served_decl = scale_served_decl; source.keep = scale_keep;
        sh_native_inline_report report = {0}; size_t length = 0, builds = 0, pushes = 0, compared = 0;
        sh_native_inline_test_census_counts(NULL, NULL, NULL, 1);
        char *out = sh_native_inline_document(json, strlen(json), &source, &length, &report, error, sizeof(error));
        sh_native_inline_test_census_counts(&builds, &pushes, &compared, 1);
        if (!out) fprintf(stderr, "scale %s: %s\n", served ? "served" : "stock", error);
        CHECK(out && length == strlen(json) && !memcmp(out, json, length));
        if (served) {
            /* The census reached every placed entity's declaration, and the
             * pushes compared a bounded number of nodes each. */
            if (builds < 1 || pushes < SCALE_ENTITIES || compared > 4 * pushes)
                fprintf(stderr, "scale census: %zu builds, %zu pushes, %zu compared\n", builds, pushes, compared);
            CHECK(builds >= 1 && pushes >= SCALE_ENTITIES && compared <= 4 * pushes);
            CHECK(report.count == 1 && !report.sites[0].converted && report.sites[0].entity == 0);
        } else {
            if (builds || pushes) fprintf(stderr, "scale stock: %zu builds, %zu pushes\n", builds, pushes);
            CHECK(builds == 0 && pushes == 0 && report.count == 0);
        }
        free(out); sh_native_inline_report_free(&report);
    }
    free(json);
}

int main(void)
{
    sh_decl_dependency_schema schema = {NULL, schema_describe, schema_field, schema_reference, NULL, NULL, NULL, NULL,
        schema_storage};
    sh_native_inline_source source = {NULL, fixture_read, fixture_derives, &schema, fixture_looked_up, fixture_keep, 26,
        fixture_placement, fake_evaluate, fixture_read_decl};
    /* Plumbing verdicts: EQUAL unless a case below says otherwise. */
    fake_reset(SH_NATIVE_EVAL_EQUAL);
#ifdef SH_NATIVE_INLINE_TESTING
    version_policy_checks();
    version_integration_checks(&source);
    /* The shrink/regrow site's native evaluation is unavailable in the main run. */
    fake_by_entity[2] = SH_NATIVE_EVAL_UNAVAILABLE;
#endif
    const char *document = "{\"entities\":["
        ENTITY("pkg/prop", "{\"edit\":{\"health\":99,\"spawnPosition\":{\"x\":1.5}}}", "26") ","
        ENTITY("pkg/prop2", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/shrink", "{\"edit\":{\"list\":{\"num\":3}}}", "26") ","
        ENTITY("pkg/reset", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/apostrophe", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/lookedup", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/intdecimal", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/unknown", "{\"edit\":{}}", "26") ","
        ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":3}}", "26") ","
        ENTITY("pkg/prop", "{\"edit\":{}}", "25") ","
        ENTITY("pkg/prop", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/stock_changed", "{\"edit\":{}}", "26") ","
        ENTITY("smp/replaced", "{\"edit\":{}}", "26") ","
        ENTITY("smp/dropped", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/hidden", "{\"edit\":{}}", "26") ","
        ENTITY_NO_CLASS("pkg/prop", "{\"edit\":{\"health\":42}}", "26") ","
        ENTITY_BARE("pkg/prop", "{\"edit\":{}}") ","
        ENTITY("pkg/rootchild", "{\"edit\":{\"health\":8}}", "26") ","
        ENTITY("pkg/rootkept", "{\"edit\":{}}", "26") ","
        ENTITY_NO_CLASS("pkg/rootchild", "{\"edit\":{\"health\":8}}", "26") ","
        ENTITY_NO_CLASS("pkg/variant", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/variant", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/nested", "{\"edit\":{\"nested\":{\"gain\":4}}}", "26") ","
        ENTITY("pkg/nested_unknown", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/exponent", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/exponent_loss", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/exponent_empty", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/pooled", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/pooledchild", "{\"edit\":{}}", "26") ","
        ENTITY_NO_CLASS("pkg/variantchild", "{\"edit\":{}}", "26")
        "],\"version\":7,\"note\":\"ordinary \\\"string\\\" bytes stay exact\"}";
    sh_native_inline_report report = {0};
    size_t length = 0; char error[512];
    char *out = sh_native_inline_document(document, strlen(document), &source, &length, &report, error, sizeof(error));
    if (!out) fprintf(stderr, "document: %s\n", error);
    CHECK(out != NULL);
    if (!out) return 1;

    /* The first use converts: inherit the native ancestor, carry P's edits and
     * keep the instance's own values (health 99, spawnPosition) verbatim. */
    const char *first = "{\"entityDef\":{\"className\":\"idProp\",\"inherit\":\"snapmaps/props/base\",\"name\":\"\","
        "\"state\":{\"edit\":{\"health\":99,\"speed\":0.349999994,\"model\":\"md6/custom\",\"spawnPosition\":{\"x\":1.5}}},"
        "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":26},\"uniqueId\":1}";
    if (!strstr(out, first)) fprintf(stderr, "converted document:\n%s\n", out);
    CHECK(strstr(out, first) != NULL);
    const sh_native_inline_site *site = site_for(&report, 0);
    CHECK(site && site->converted && !strcmp(site->from, "pkg/prop") && !strcmp(site->to, "snapmaps/props/base"));
    /* A two-level chain folds both layers; an equal second use converts too. */
    CHECK(strstr(out, "\"state\":{\"edit\":{\"health\":50,\"speed\":2.5,\"model\":\"md6/custom\"}}") != NULL);
    site = site_for(&report, 1); CHECK(site && site->converted);
    site = site_for(&report, 10); CHECK(site && site->converted);
    /* P shrinks the inherited list and the instance regrows it. The source model
     * cannot decide the regrown slots and no native evaluator is bound, so this
     * use keeps its package as a product gap, not a native limit. */
    site = site_for(&report, 2);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP && strstr(site->reason, "is unavailable"));
    CHECK(strstr(out, ENTITY("pkg/shrink", "{\"edit\":{\"list\":{\"num\":3}}}", "26")) != NULL);
    /* No carrier spelling for a reset block or an apostrophe: native limits. */
    site = site_for(&report, 3); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE);
    site = site_for(&report, 4); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE);
    /* A site whose definition another map site names keeps it while that
     * consumer is untraced: undecided support, not a native limit. */
    site = site_for(&report, 5);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP && strstr(site->reason, "world /k:filter/k:entityDef") &&
        strstr(site->reason, "not traced"));
    /* A decimal for an int field and an unverified field are product gaps. */
    site = site_for(&report, 6); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP);
    site = site_for(&report, 7); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP &&
        strstr(site->reason, "edit.mystery"));
    /* Native definitions are not sites. Older private identities normalize
     * without selecting their stock ancestor's native upgrade operations. */
    CHECK(!site_for(&report, 8));
    CHECK(strstr(out, ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":3}}", "26")) != NULL);
    site = site_for(&report, 9); CHECK(site && site->converted);
    /* A caller-kept global requirement stays on the package route. */
    site = site_for(&report, 11);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE && strstr(site->reason, "globally"));
    /* A package replacement of a stock identity becomes that stock identity
     * plus one inline layer when exact; one that omits stock values keeps its
     * package, a product gap rather than a native limit. */
    site = site_for(&report, 12);
    CHECK(site && site->converted && !strcmp(site->from, "smp/replaced") && !strcmp(site->to, "snapmaps/props/base"));
    site = site_for(&report, 13);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP && strstr(site->reason, "omitted stock values"));
    /* SnapMap's validator admits only palette definitions: a base without an
     * editor entry would make the converted map unloadable. */
    site = site_for(&report, 14);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE && strstr(site->reason, "editor entry"));
    /* D7: the native value reader needs only state. Without className the class
     * inherits along the chain, so the use converts and keeps no className;
     * without ~version the native policy starts at 0, selecting the original
     * private identity. An accepted conversion then explicitly emits 26. */
    site = site_for(&report, 15);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "{\"entityDef\":{\"inherit\":\"snapmaps/props/base\",\"name\":\"\",\"state\":{\"edit\":"
        "{\"health\":42,\"speed\":0.349999994,\"model\":\"md6/custom\"}}") != NULL);
    site = site_for(&report, 16);
    CHECK(site && site->converted);
    CHECK(strstr(out, ENTITY_NO_CLASS("snapmaps/props/base", "{\"edit\":{\"health\":50,\"speed\":0.349999994,\"model\":\"md6/custom\"}}", "26")) != NULL);
    /* D7: a chain with no native ancestor folds over an empty inherit, which
     * the native reader accepts without a lookup; SnapMap placement then
     * judges that base like any other (admitted here for pkg/rootchild, not
     * for pkg/rootkept). */
    site = site_for(&report, 17);
    CHECK(site && site->converted && !strcmp(site->to, ""));
    CHECK(strstr(out, "\"inherit\":\"\",\"name\":\"\",\"state\":{\"edit\":{\"health\":8,\"model\":\"md6/root\",\"speed\":3}}")
        != NULL);
    site = site_for(&report, 18);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE && strstr(site->reason, "editor entry"));
    /* D7 class identity: without className, the converted value states the
     * class its original chain gave whenever the new base would give another:
     * the empty base gives none (a package root's class), and a served child
     * selecting another reflected class than its stock base keeps it. A value
     * stating className keeps its own. */
    site = site_for(&report, 19);
    CHECK(site && site->converted && !strcmp(site->to, ""));
    CHECK(strstr(out, "{\"entityDef\":{\"className\":\"idProp\",\"inherit\":\"\",\"name\":\"\",\"state\":{\"edit\":"
        "{\"health\":8,\"model\":\"md6/root\",\"speed\":3}}") != NULL);
    site = site_for(&report, 20);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "{\"entityDef\":{\"className\":\"idPropVariant\",\"inherit\":\"snapmaps/props/base\"") != NULL);
    site = site_for(&report, 21);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    /* The value without className whose base gives the same class (site 15)
     * states none. */
    CHECK(strstr(out, "{\"entityDef\":{\"inherit\":\"snapmaps/props/base\",\"name\":\"\",\"state\":{\"edit\":"
        "{\"health\":42,") != NULL);
    /* M70 carrier: the entity carrier stores className and its reader's
     * rebuild writes class only where it differs from the inherited one, so
     * each value is evaluated with the class it loads with. Site 20 (no
     * className) keeps its chain's class and its converted value carries it;
     * site 21's own className overrides its chain's class and needs no class
     * line over the base; a private two-level chain (site 29) carries its
     * selected class; equal classes (sites 0 and 1) add nothing. */
    CHECK(!strcmp(fake_target_class[20], "") && !strcmp(fake_candidate_class[20], "idPropVariant"));
    CHECK(!strcmp(fake_target_class[21], "idProp") && !strcmp(fake_candidate_class[21], ""));
    CHECK(!strcmp(fake_target_class[0], "") && !strcmp(fake_candidate_class[0], ""));
    CHECK(!strcmp(fake_target_class[1], "") && !strcmp(fake_candidate_class[1], ""));
    site = site_for(&report, 29);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(!strcmp(fake_target_class[29], "") && !strcmp(fake_candidate_class[29], "idPropVariant"));
    CHECK(strstr(out, "{\"entityDef\":{\"className\":\"idPropVariant\",\"inherit\":\"snapmaps/props/base\",\"name\":\"\","
        "\"state\":{\"edit\":{\"health\":7,\"speed\":5}}") != NULL);
    /* A definition's own pool settings never reach an inheriting map entity
     * (Parse does not inherit them; the carrier does not store them): direct
     * and two-level private chains convert, and no pool setting is carried. */
    site = site_for(&report, 27);
    CHECK(site && site->converted && !strcmp(site->from, "pkg/pooled") && !strcmp(site->to, "snapmaps/props/base"));
    site = site_for(&report, 28);
    CHECK(site && site->converted && !strcmp(site->from, "pkg/pooledchild"));
    CHECK(strstr(out, ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":9,\"speed\":4}}", "26")) != NULL);
    CHECK(strstr(out, "poolCount") == NULL && strstr(out, "poolGranularity") == NULL);
    /* The instance's nested override wins, the sibling reference survives,
     * and references in nested rows remain names or NULL. A referenced
     * declaration is deliberately unavailable through fixture_read: conversion
     * must not invent an inline resource body for the native pointer carrier. */
    site = site_for(&report, 22);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "\"nested\":{\"damage\":\"damage/package\",\"gain\":4}") != NULL);
    CHECK(strstr(out, "\"nestedList\":{\"num\":2,\"item[0]\":{\"damage\":\"damage/other\",\"gain\":0.5},"
        "\"item[1]\":{\"damage\":null,\"gain\":1.5}}") != NULL);
    /* Unknown nested fields stay on the original package route; recognizing
     * the enclosing object does not establish its children's native readers. */
    site = site_for(&report, 23);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP && strstr(site->reason, "edit.nested.unknown"));
    CHECK(strstr(out, ENTITY("pkg/nested_unknown", "{\"edit\":{}}", "26")) != NULL);
    /* Safe exponent spelling uses native state; a value the carrier would
     * round away keeps the original package rather than changing behavior. */
    site = site_for(&report, 24);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "\"state\":{\"edit\":{\"speed\":100000.0}}") != NULL);
    site = site_for(&report, 25);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE && strstr(site->reason, "edit.speed"));
    CHECK(strstr(out, ENTITY("pkg/exponent_loss", "{\"edit\":{}}", "26")) != NULL);
    site = site_for(&report, 26);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "\"state\":{\"edit\":{\"speed\":1.0}}") != NULL);
    CHECK(report.count == 29);
    /* Bytes outside converted entity values are exact. */
    CHECK(strstr(out, "],\"version\":7,\"note\":\"ordinary \\\"string\\\" bytes stay exact\"}") != NULL);
    CHECK(!strncmp(out, "{\"entities\":[{\"entityDef\":", 26));
    if (failures)
        for (size_t i = 0; i < report.count; i++)
            fprintf(stderr, "site %zu %s: %s limit %d %s\n", report.sites[i].entity, report.sites[i].from,
                report.sites[i].converted ? "converted" : "kept", (int)report.sites[i].limit, report.sites[i].reason);
    free(out); sh_native_inline_report_free(&report);

    /* Each non-EQUAL verdict keeps site 2's package as a GAP with its own
     * reason; EQUAL lets the native verdict accept a candidate the source
     * model doubts. No verdict is ever an ENGINE limit. */
    {
        static const struct { sh_native_eval_status status; const char *reason; } cases[] = {
            {SH_NATIVE_EVAL_UNEQUAL, "found a different state"}, {SH_NATIVE_EVAL_UNSUPPORTED, "does not cover a value"},
            {SH_NATIVE_EVAL_FAILED, "evaluation failed"}, {SH_NATIVE_EVAL_UNAVAILABLE, "is unavailable"},
            {SH_NATIVE_EVAL_EQUAL, NULL}};
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            sh_native_inline_report evaluated = {0};
            fake_reset(SH_NATIVE_EVAL_EQUAL); fake_by_entity[2] = cases[c].status;
            char *again = sh_native_inline_document(document, strlen(document), &source, &length, &evaluated, error, sizeof(error));
            CHECK(again != NULL);
            const sh_native_inline_site *shrink = site_for(&evaluated, 2);
            CHECK((fake_asked & (1ull << 2)) && !strcmp(fake_from, "pkg/shrink") && fake_base[0]);
            CHECK(shrink && shrink->limit != SH_DECL_CARRIER_ENGINE);
            if (cases[c].reason)
                CHECK(shrink && !shrink->converted && shrink->limit == SH_DECL_CARRIER_GAP &&
                    strstr(shrink->reason, "differs") && strstr(shrink->reason, cases[c].reason) &&
                    strstr(shrink->reason, "candidate supplied"));
            else
                CHECK(shrink && !strstr(shrink->reason, "native evaluation"));
            site = site_for(&evaluated, 0); CHECK(site && site->converted);
            free(again); sh_native_inline_report_free(&evaluated);
        }
    }
    /* Without a native evaluator nothing converts, even where the source model
     * agrees: every site keeps its complete package and the bytes are exact. */
    {
        sh_native_inline_report kept = {0};
        source.evaluate = NULL;
        char *unchanged = sh_native_inline_document(document, strlen(document), &source, &length, &kept, error, sizeof(error));
        CHECK(unchanged != NULL && !strcmp(unchanged, document));
        size_t agreed = 0;
        for (size_t i = 0; i < kept.count; i++) {
            CHECK(!kept.sites[i].converted && kept.sites[i].limit != SH_DECL_CARRIER_EXACT);
            if (strstr(kept.sites[i].reason, "the source model agrees (native evaluation is unavailable)")) agreed++;
        }
        CHECK(agreed >= 2);
        free(unchanged); sh_native_inline_report_free(&kept);
        source.evaluate = fake_evaluate;
    }
    pool_checks(&source);
    ordered_checks(&source);
    served_checks(&source);
    production_checks(&source);
    drop_checks();
    roster_checks();
    roster_script_checks();
    roster_scoped_script_checks();
    hint_checks();
    census_scale_checks();
    if (failures) { fprintf(stderr, "%d native inline check(s) failed\n", failures); return 1; }
    puts("native inline representation checks passed");
    return 0;
}
