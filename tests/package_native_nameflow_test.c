#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/package_native_nameflow.h"
#include "../src/backend/config_json.h"

typedef struct fixture {
    sh_decl_dependency_schema schema;
    char keys[1024];
    size_t values, projected, old_values;
    int unavailable, shadow, wrong_type, refuse, refuse_old;
} fixture;

static int derives(void *context, const char *name, const char *base)
{
    fixture *f = context;
    if (f->unavailable) return -1;
    return !strcmp(name, base) || (!strcmp(name, "TriggerChild") && !strcmp(base, "idTrigger"));
}
static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "idStr") || !strcmp(type.name, "int")) shape->kind = SH_DECL_VALUE_IGNORE;
    else if (!strcmp(type.name, "TriggerArray")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element = (sh_decl_value_type){"idTrigger", ""};
        shape->count_known = 1; shape->count = 3; shape->item_key = "item";
    } else shape->kind = SH_DECL_VALUE_OBJECT;
    return 1;
}
static int field(void *context, sh_decl_value_type owner, const char *key, sh_decl_value_type *type)
{
    fixture *f = context;
    if (!strcmp(key, "call")) {
        *type = (sh_decl_value_type){f->wrong_type ? "int" : "idStr", ""};
        return 1;
    }
    if (!strcmp(owner.name, "Outer")) {
        if (!strcmp(key, "nested")) *type = (sh_decl_value_type){"idTarget_Count", ""};
        else if (!strcmp(key, "triggers")) *type = (sh_decl_value_type){"TriggerArray", ""};
        else return 0;
        return 1;
    }
    return 0;
}
static int storage(void *context, sh_decl_value_type owner, const char *key, sh_decl_member_storage *out)
{
    fixture *f = context;
    if (strcmp(key, "call")) return 0;
    out->offset = !strcmp(owner.name, "idTarget_Count") ? 0xc90 : 0xca0;
    if (f->shadow) out->offset += 0x30;
    out->extent = 0x30;
    return 1;
}
static int reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{ (void)context; (void)path; (void)type; (void)name; (void)length; assert(0); return 0; }

static int old_value(void *context, const char *path, sh_decl_value_type type, const sh_decl_node *node)
{
    fixture *f = context;
    (void)path; (void)type; assert(node);
    f->old_values++;
    return !f->refuse_old;
}
static int value_site(void *context, const char *path, sh_decl_value_type type, const sh_decl_reference_site *site)
{
    fixture *f = context;
    const char *value = NULL, *key = NULL;
    size_t length = 0, key_length = 0;
    char *decoded = NULL;
    sh_native_nameflow_kind kind;
    (void)path;
    assert(site && site->owner_type && site->member && !strcmp(site->member, "call"));
    f->values++;
    kind = sh_native_nameflow_member(&f->schema, f, derives, type, site);
    assert(kind == SH_NATIVE_NAMEFLOW_SCRIPT_CALL);
    if (site->node) {
        assert(!site->json && !site->json_length);
        assert(sh_decl_tree_literal(site->node, &value, &length));
    } else {
        assert(site->json && site->json_length);
        decoded = malloc(site->json_length + 1); assert(decoded);
        assert(sh_native_json_decode_string(site->json, site->json_length, decoded, site->json_length + 1, &length));
        value = decoded;
    }
    if (sh_native_nameflow_key(kind, value, length, &key, &key_length) == 1) {
        size_t used = strlen(f->keys);
        int n = snprintf(f->keys + used, sizeof(f->keys) - used, "%.*s|", (int)key_length, key);
        assert(n >= 0 && (size_t)n < sizeof(f->keys) - used);
        f->projected++;
    }
    free(decoded);
    return !f->refuse;
}
static void setup(fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->schema.context = f;
    f->schema.describe = describe; f->schema.field = field; f->schema.reference = reference;
    f->schema.storage = storage; f->schema.value = old_value; f->schema.value_site = value_site;
}
static void projections(void)
{
    static const struct { const char *value, *expected; int result; } cases[] = {
        {"", NULL, 0}, {"localFunction", NULL, 0}, {"P_17.run", "P_17", 1},
        {"P_17.run.more", "P_17", 1}, {"$self.run", "$self", 1},
        {"P_17.", "P_17", 1}, {".run", "", 1}, {"P_17/run", NULL, 0},
        {"P_17::run", NULL, 0}, {"P_17\\run", NULL, 0}, {"P.17.run", "P", 1}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        const char *key = NULL; size_t length = 0;
        int result = sh_native_nameflow_key(SH_NATIVE_NAMEFLOW_SCRIPT_CALL, cases[i].value,
            strlen(cases[i].value), &key, &length);
        assert(result == cases[i].result);
        if (result) assert(key == cases[i].value && length == strlen(cases[i].expected) &&
            !memcmp(key, cases[i].expected, length));
        else assert(!key && !length);
    }
    {
        const char embedded[] = {'P', '_', '1', 0, '.', 'f'};
        const char *key; size_t length;
        assert(sh_native_nameflow_key(SH_NATIVE_NAMEFLOW_SCRIPT_CALL, embedded, sizeof(embedded), &key, &length) == -1);
        assert(sh_native_nameflow_key(SH_NATIVE_NAMEFLOW_UNAVAILABLE, "P_1.f", 5, &key, &length) == -1);
        assert(sh_native_nameflow_key(SH_NATIVE_NAMEFLOW_NONE, "P_1.f", 5, &key, &length) == 0);
    }
}
static void qualification(void)
{
    fixture f; setup(&f);
    sh_decl_reference_site site = {0};
    sh_decl_value_type string = {"idStr", ""};
    site.owner_type = "idTrigger"; site.member = "call";
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_SCRIPT_CALL);
    site.owner_type = "TriggerChild";
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_SCRIPT_CALL);
    site.owner_type = "idTarget_Count";
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_SCRIPT_CALL);
    site.owner_type = "Unrelated";
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_NONE);
    site.owner_type = "idTrigger"; site.member = "displayName";
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_NONE);
    site.member = "call"; f.shadow = 1;
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_UNAVAILABLE);
    f.shadow = 0; f.wrong_type = 1;
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_UNAVAILABLE);
    f.wrong_type = 0; f.unavailable = 1;
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_UNAVAILABLE);
    f.unavailable = 0; f.schema.storage = NULL;
    assert(sh_native_nameflow_member(&f.schema, &f, derives, string, &site) == SH_NATIVE_NAMEFLOW_UNAVAILABLE);
}
static void typed_sources(void)
{
    const char *text = "{ call = \"P_17.first\"; call = \"P_17.second\"; call = \"localFunction\"; }";
    char original[512]; strcpy(original, text);
    sh_decl_node *tree = sh_decl_tree_parse_ordered((sh_decl_source){text, strlen(text)}, NULL, 0);
    sh_decl_dependency_result result;
    fixture f; setup(&f);
    assert(tree && sh_decl_state_dependencies(tree, (sh_decl_value_type){"idTrigger", ""}, &f.schema, &result));
    assert(f.values == 3 && f.old_values == 3 && f.projected == 2 && !strcmp(f.keys, "P_17|P_17|"));
    assert(!strcmp(text, original));
    setup(&f); f.refuse_old = 1;
    assert(!sh_decl_state_dependencies(tree, (sh_decl_value_type){"idTrigger", ""}, &f.schema, &result));
    assert(result.aborted && f.old_values == 1 && !f.values);
    sh_decl_tree_free(tree);
    {
        const char *json = "{\"nested\":{\"call\":\"P_17\\u002erun\"},"
            "\"triggers\":[{\"call\":\"$self.run\"},{\"call\":\"localFunction\"}]}";
        setup(&f);
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Outer", ""}, &f.schema, &result));
        assert(f.values == 3 && !f.old_values && f.projected == 2 && !strcmp(f.keys, "P_17|$self|"));
        setup(&f); f.refuse = 1;
        assert(!sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Outer", ""}, &f.schema, &result));
        assert(result.aborted && f.values == 1);
    }
    {
        const char *json = "{\"triggers\":{\"item[0]\":{\"call\":\"P_17.run\"}}}";
        setup(&f);
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Outer", ""}, &f.schema, &result));
        assert(f.values == 1 && f.projected == 1 && !strcmp(f.keys, "P_17|"));
    }
}
static int record_drift;
static int record_describe(void *c,sh_decl_value_type type,sh_decl_value_shape *shape)
{
    (void)c;memset(shape,0,sizeof *shape);
    if(!strcmp(type.name,"idStr") || !strcmp(type.name,"idAtomicString") || !strcmp(type.name,"narrativeActionType_t") || !strcmp(type.name,"narrativeConditionType_t"))shape->kind=SH_DECL_VALUE_IGNORE;
    else if(!strcmp(type.name,"Actions") || !strcmp(type.name,"Conditions") || !strcmp(type.name,"Modifiers")){
        shape->kind=SH_DECL_VALUE_COLLECTION;shape->count_key="num";shape->item_key="item";
        shape->element=(sh_decl_value_type){!strcmp(type.name,"Actions")?"narrativeAction_t":!strcmp(type.name,"Conditions")?"idNarrativeLinkCondition":"splineMoverModifier_t",""};
    }else shape->kind=SH_DECL_VALUE_OBJECT;return 1;
}
static int record_field(void *c,sh_decl_value_type owner,const char *key,sh_decl_value_type *type)
{
    (void)c;const char *name=NULL;
    if(!strcmp(owner.name,"nameflowRoot"))name=!strcmp(key,"actions")?"Actions":!strcmp(key,"conditions")?"Conditions":!strcmp(key,"modifiers")?"Modifiers":NULL;
    else if(!strcmp(owner.name,"narrativeAction_t"))name=!strcmp(key,"type")?"narrativeActionType_t":!strcmp(key,"scriptParms")?"narrativeAction_t::narrativeScriptParms_t":NULL;
    else if(!strcmp(owner.name,"idNarrativeLinkCondition"))name=!strcmp(key,"type")?"narrativeConditionType_t":!strcmp(key,"scriptParms")?"idNarrativeLinkCondition::narrativeScriptParms_t":NULL;
    else if(!strcmp(owner.name,"splineMoverModifier_t") && !strcmp(key,"Script"))name="splineMoverModifier_t::splineMoverModifierScript_t";
    else if(!strcmp(key,"scriptFunction"))name=!strcmp(owner.name,"splineMoverModifier_t::splineMoverModifierScript_t")?"idAtomicString":"idStr";
    if(!name)return 0;*type=(sh_decl_value_type){name,""};return 1;
}
static int record_storage(void *c,sh_decl_value_type owner,const char *key,sh_decl_member_storage *store)
{
    (void)c;memset(store,0,sizeof *store);
    if(!strcmp(key,"scriptFunction")){store->extent=!strcmp(owner.name,"splineMoverModifier_t::splineMoverModifierScript_t")?8:0x30;store->offset=record_drift?8:0;return 1;}
    if(!strcmp(key,"type")){store->extent=4;store->offset=!strcmp(owner.name,"idNarrativeLinkCondition")?4:0;return 1;}
    if(!strcmp(key,"scriptParms")){store->extent=0x30;store->offset=!strcmp(owner.name,"narrativeAction_t")?0x80:0x10;return 1;}
    if(!strcmp(key,"Script")){store->extent=0x10;store->offset=0xc0;return 1;}return 0;
}
static int record_enum(void *c,sh_decl_value_type type,const sh_decl_node *token,int32_t *value)
{
    (void)c;(void)type;const char *literal;size_t length;if(!sh_decl_tree_literal(token,&literal,&length))return 0;
    char text[32];if(length>=sizeof text)return 0;memcpy(text,literal,length);text[length]=0;char *end;long number=strtol(text,&end,10);
    if(end==text || *end)return 0;*value=(int32_t)number;return 1;
}
static void record_case(const char *text,const char *path,const char *owner,const char *string,sh_native_nameflow_kind expected,const char *expected_value)
{
    sh_decl_node *tree=sh_decl_tree_parse_ordered((sh_decl_source){text,strlen(text)},NULL,0);assert(tree);
    sh_decl_dependency_schema schema={0};schema.describe=record_describe;schema.field=record_field;schema.storage=record_storage;
    sh_native_nameflow_scope scope={tree,{"nameflowRoot",""},NULL,record_enum};
    sh_decl_reference_site site={0};site.owner_type=owner;site.member="scriptFunction";const char *value;size_t length;
    sh_native_nameflow_kind got=sh_native_nameflow_scoped_member(&schema,NULL,NULL,(sh_decl_value_type){string,""},&site,path,&scope,&value,&length);
    assert(got==expected);if(expected_value)assert(length==strlen(expected_value) && !memcmp(value,expected_value,length));
    sh_decl_tree_free(tree);
}
static void scoped_records(void)
{
    const char *path="edit.actions.item[0].scriptParms.scriptFunction",*owner="narrativeAction_t::narrativeScriptParms_t";
    record_case("{ actions={ num=1; item[0]={type=10; scriptParms={scriptFunction=\"P_17.function\";};};};}",path,owner,"idStr",SH_NATIVE_NAMEFLOW_SCRIPT_CALL,"P_17.function");
    record_case("{ actions={ num=1; item[0]={type=1; scriptParms={scriptFunction=\"P_17.function\";};};};}",path,owner,"idStr",SH_NATIVE_NAMEFLOW_INACTIVE,NULL);
    record_case("{ actions={ num=1; item[0]={type=10; type=1; scriptParms={scriptFunction=\"P_17.function\";};};};}",path,owner,"idStr",SH_NATIVE_NAMEFLOW_INACTIVE,NULL);
    record_case("{ actions={ num=1; item[0]={type=1; type=10; scriptParms={scriptFunction=\"P_17.function\";};};};}",path,owner,"idStr",SH_NATIVE_NAMEFLOW_SCRIPT_CALL,"P_17.function");
    record_case("{ actions={ num=0; item[0]={type=10; scriptParms={scriptFunction=\"P_17.function\";};};};}",path,owner,"idStr",SH_NATIVE_NAMEFLOW_INACTIVE,NULL);
    record_case("{ actions={ num=1; item[0]={scriptParms={scriptFunction=\"P_17.function\";};};};}",path,owner,"idStr",SH_NATIVE_NAMEFLOW_UNAVAILABLE,NULL);
    record_case("{ actions={ num=2; item[0]={type=1; scriptParms={scriptFunction=\"P_17.function\";};}; item[1]={type=10; scriptParms={scriptFunction=\"localFunction\";};};};}",path,owner,"idStr",SH_NATIVE_NAMEFLOW_INACTIVE,NULL);
    record_case("{ actions={ num=2; item[0]={type=1; scriptParms={scriptFunction=\"P_17.function\";};}; item[1]={type=10; scriptParms={scriptFunction=\"localFunction\";};};};}","edit.actions.item[1].scriptParms.scriptFunction",owner,"idStr",SH_NATIVE_NAMEFLOW_INACTIVE,"localFunction");
    record_case("{ conditions={num=1; item[0]={type=14; scriptParms={scriptFunction=\"P_17.function\";};};};}","edit.conditions.item[0].scriptParms.scriptFunction","idNarrativeLinkCondition::narrativeScriptParms_t","idStr",SH_NATIVE_NAMEFLOW_SCRIPT_CALL,"P_17.function");
    record_case("{ modifiers={num=1; item[0]={Script={scriptFunction=\"P_17.function\";};};};}","edit.modifiers.item[0].Script.scriptFunction","splineMoverModifier_t::splineMoverModifierScript_t","idAtomicString",SH_NATIVE_NAMEFLOW_SCRIPT_CALL,"P_17.function");
    record_drift=1;record_case("{ actions={ num=1; item[0]={type=10; scriptParms={scriptFunction=\"P_17.function\";};};};}",path,owner,"idStr",SH_NATIVE_NAMEFLOW_UNAVAILABLE,NULL);record_drift=0;
}
int main(void)
{
    projections(); qualification(); typed_sources();scoped_records();
    puts("package_native_nameflow_test: PASS");
    return 0;
}
