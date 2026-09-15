#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_compose.h"
#include "../src/backend/decl_entity_class.h"

typedef struct fixture {
    sh_decl_composition_role role;
    size_t index, calls[3], owned_count;
    char *owned[16];
    int changed_parent, deny_result, result_rewritten;
} fixture;

static sh_decl_source text(const char *value)
{ return (sh_decl_source){value, strlen(value)}; }
static int known(const char *name)
{ return !strcmp(name, "Base") || !strcmp(name, "Derived") || !strcmp(name, "Narrow") || !strcmp(name, "Rewritten"); }
static int derives(void *context, const char *child, const char *parent)
{
    (void)context;
    if (!known(child) || !known(parent)) return -1;
    return !strcmp(child, parent) || !strcmp(parent, "Base");
}
static int parent_read(void *context, const char *name, sh_decl_source *out)
{
    fixture *f = (fixture *)context;
    const char *source;
    if (strcmp(name, "parent")) return 0;
    source = f->result_rewritten && f->role == SH_DECL_COMPOSITION_RESULT ?
        "{ class = \"Rewritten\"; }" : f->changed_parent && (f->role == SH_DECL_COMPOSITION_RESULT ||
        (f->role == SH_DECL_COMPOSITION_CONTRIBUTION && !f->index)) ?
        "{ class = \"Derived\"; }" : "{ class = \"Base\"; }";
    out->text = _strdup(source); out->length = strlen(source); assert(out->text); return 1;
}
static int resolve(void *context, const sh_decl_node *definition,
    sh_decl_composition_role role, size_t index, sh_decl_value_type *type,
    char *error, size_t capacity)
{
    fixture *f = (fixture *)context;
    sh_decl_entity_class_source source = {f, parent_read, derives};
    char *class_name;
    f->role = role; f->index = index; f->calls[role]++;
    if (role == SH_DECL_COMPOSITION_RESULT) {
        assert(!sh_decl_tree_member(definition, "edit"));
        if (f->deny_result) { snprintf(error, capacity, "effective parent view is unavailable"); return -1; }
    }
    class_name = sh_decl_entity_tree_class(definition, &source, error, capacity);
    if (!class_name) return -1;
    assert(f->owned_count < 16); f->owned[f->owned_count++] = class_name;
    *type = (sh_decl_value_type){class_name, ""}; return 1;
}
static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (type.ops && (!strcmp(type.ops, "[4]") || !strcmp(type.ops, "[2]"))) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element = (sh_decl_value_type){type.name, ""};
        shape->count = type.ops[1] == '4' ? 4 : 2; shape->count_known = 1;
    } else if (known(type.name) || !strcmp(type.name, "Element") || !strcmp(type.name, "AddedElement"))
        shape->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "int") || !strcmp(type.name, "float")) shape->kind = SH_DECL_VALUE_IGNORE;
    return 1;
}
static int field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    (void)context;
    if (!strcmp(key, "slots")) *out = (sh_decl_value_type){"Element", !strcmp(type.name, "Narrow") ? "[2]" : "[4]"};
    else if (!strcmp(key, "node")) *out = (sh_decl_value_type){!strcmp(type.name, "Derived") ? "AddedElement" : "Element", ""};
    else if (!strcmp(key, "custom")) *out = (sh_decl_value_type){!strcmp(type.name, "Rewritten") ? "Element" : "CustomReader", ""};
    else if (!strcmp(key, "value")) *out = (sh_decl_value_type){!strcmp(type.name, "Rewritten") ? "float" : "int", ""};
    else if (!strcmp(key, "shared") || !strcmp(key, "x") || !strcmp(key, "y") ||
        (!strcmp(type.name, "AddedElement") && !strcmp(key, "extra"))) *out = (sh_decl_value_type){"int", ""};
    else return -1;
    return 1;
}
static char *compose(fixture *f, const char *original, const char *a, const char *b,
    char *error, sh_decl_conflict *conflict)
{
    sh_decl_dependency_schema types = {NULL, describe, field, NULL, NULL};
    sh_decl_composition_schema schema = {&types, {0}, f, resolve};
    sh_decl_source sources[] = {text(a), text(b)};
    size_t length, i;
    char *result;
    memset(f->calls, 0, sizeof(f->calls));
    result = sh_decl_compose_resource("entitydef", text(original), sources, 2, NULL, 0, &schema,
        &length, error, 512, conflict);
    for (i = 0; i < f->owned_count; i++) free(f->owned[i]);
    f->owned_count = 0;
    assert((result != NULL) == (length != 0));
    return result;
}
int main(void)
{
    fixture f = {0};
    char error[512], *result, *reverse;
    sh_decl_conflict conflict;
    const char *base = "{ inherit = \"parent\"; edit = { shared = 0; node = { x = 0; } } }";
    const char *a = "{ inherit = \"parent\"; class = \"Derived\"; edit = { shared = 0; node = { x = 0; extra = 4; } } }";
    const char *b = "{ inherit = \"parent\"; edit = { shared = 2; node = { x = 3; } } }";
    result = compose(&f, base, a, b, error, &conflict);
    assert(result && strstr(result, "shared = 2;") && strstr(result, "x = 3;") && strstr(result, "extra = 4;"));
    assert(f.calls[0] == 1 && f.calls[1] == 2 && f.calls[2] == 1);
    reverse = compose(&f, base, b, a, error, &conflict);
    assert(reverse && !strcmp(result, reverse)); free(result); free(reverse);
    /* Original, each author's parent view and the effective parent can differ. */
    f.changed_parent = 1;
    result = compose(&f, base,
        "{ inherit = \"parent\"; edit = { shared = 0; node = { x = 0; extra = 4; } } }", b, error, &conflict);
    assert(result && strstr(result, "extra = 4;") && strstr(result, "x = 3;")); free(result);
    f.changed_parent = 0;
    /* Sparse positions retain identity when a class changes the array extent. */
    base = "{ class = \"Base\"; edit = { slots = { slots[0] = { x = 1; } slots[3] = { y = 3; } } } }";
    a = "{ class = \"Narrow\"; edit = { slots = { slots[0] = { x = 1; } } } }";
    b = "{ class = \"Base\"; edit = { slots = { slots[0] = { x = 2; } slots[3] = { y = 3; } } } }";
    result = compose(&f, base, a, b, error, &conflict);
    assert(result && strstr(result, "x = 2;") && !strstr(result, "slots[3]")); free(result);
    result = compose(&f, base, a,
        "{ class = \"Base\"; edit = { slots = { slots[0] = { x = 1; } slots[3] = { y = 4; } } } }", error, &conflict);
    assert(!result && strstr(error, "slots[3]") && conflict.first == 0 && conflict.second == 1);
    result = compose(&f, base,
        "{ class = \"Narrow\"; edit = { slots = { slots[3] = { y = 3; } } } }", b, error, &conflict);
    assert(!result && strstr(error, "out-of-range") && conflict.first == 0);
    /* Equal text is a replacement when the native reader type changes. */
    base = "{ class = \"Base\"; edit = { value = 1; shared = 0; } }";
    a = "{ class = \"Rewritten\"; edit = { value = 1; shared = 0; } }";
    b = "{ class = \"Base\"; edit = { value = 1; shared = 2; } }";
    result = compose(&f, base, a, b, error, &conflict);
    assert(result && strstr(result, "value = 1;") && strstr(result, "shared = 2;")); free(result);
    result = compose(&f, base, a,
        "{ class = \"Base\"; edit = { value = 2; shared = 0; } }", error, &conflict);
    assert(!result && strstr(error, "edit.value") && strstr(error, "reader type") && conflict.first == 1);
    /* A previous custom-reader block is not the baseline of a new object. */
    base = "{ class = \"Base\"; edit = { custom = { x = 1; } } }";
    result = compose(&f, base,
        "{ class = \"Rewritten\"; edit = { custom = { x = 2; } } }",
        "{ class = \"Rewritten\"; edit = { custom = { y = 3; } } }", error, &conflict);
    assert(result && strstr(result, "x = 2;") && strstr(result, "y = 3;")); free(result);
    result = compose(&f, base,
        "{ class = \"Rewritten\"; edit = { custom = { y = 3; } } }", base, error, &conflict);
    assert(result && !strstr(result, "x = 1;") && strstr(result, "y = 3;")); free(result);
    result = compose(&f, base,
        "{ class = \"Rewritten\"; edit = { custom = { y = 3; } } }",
        "{ class = \"Base\"; edit = { custom = { x = 2; } } }", error, &conflict);
    assert(!result && strstr(error, "edit.custom") && conflict.first == 1);
    result = compose(&f, "{}", "{ class = \"Derived\"; edit = { shared = 1; } }",
        "{ class = \"Derived\"; edit = { value = 2; } }", error, &conflict);
    assert(result && f.calls[0] == 0 && f.calls[2] == 1); free(result);
    result = compose(&f, "{}", "{ class = \"Missing\"; edit = {} }", "{ class = \"Derived\"; edit = {} }", error, &conflict);
    assert(!result && conflict.first == 0 && strstr(error, "metadata"));
    /* A different package can change the effective parent even when every
     * contribution to this child remains byte-for-byte unchanged. */
    f.result_rewritten = 1;
    base = "{ inherit = \"parent\"; edit = { value = 1; shared = 0; } }";
    result = compose(&f, base, base, base, error, &conflict);
    assert(!result && strstr(error, "edit.value") && strstr(error, "reader type"));
    result = compose(&f, base, base,
        "{ inherit = \"parent\"; edit = { value = 1; shared = 2; } }", error, &conflict);
    assert(!result && strstr(error, "edit.value") && strstr(error, "reader type"));
    f.result_rewritten = 0;
    f.deny_result = 1;
    result = compose(&f, base, base, base, error, &conflict);
    assert(!result && strstr(error, "effective parent view"));
    puts("decl_composition_classes_test: PASS"); return 0;
}
