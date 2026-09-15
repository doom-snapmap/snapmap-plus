#include "decl_graph_dependencies.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct gd_context {
    const sh_decl_dependency_schema *types;
    const char *prefix;
    int root_object, failed;
} gd_context;

static int gd_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    gd_context *c = context;
    /* Only the first value in this walk is the root's edit.object. The graph
     * reader explicitly bypasses its own custom dispatcher there. A nested
     * field of the same class still goes through its registered native reader. */
    if (c->root_object) { c->root_object = 0; shape->kind = SH_DECL_VALUE_OBJECT; return 1; }
    return c->types->describe(c->types->context, type, shape);
}
static int gd_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    gd_context *c = context;
    return c->types->field(c->types->context, type, key, out);
}
static int gd_dynamic_type(void *context, sh_decl_value_type base, const char *name,
    size_t length, sh_decl_value_type *selected)
{
    gd_context *c = context;
    return c->types->dynamic_type ? c->types->dynamic_type(c->types->context, base, name, length, selected) : 0;
}
static char *gd_path(gd_context *c, const char *path)
{
    size_t a = strlen(c->prefix), b;
    char *out;
    /* The shared state walker starts at edit; replace only that root segment. */
    if (!strncmp(path, "edit", 4) && (!path[4] || path[4] == '.')) path += 4;
    b = strlen(path);
    if (a == SIZE_MAX || b > SIZE_MAX - a - 1 || !(out = (char *)malloc(a + b + 1))) {
        c->failed = 1; return NULL;
    }
    memcpy(out, c->prefix, a); memcpy(out + a, path, b + 1); return out;
}
static int gd_reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{
    gd_context *c = context;
    char *full = gd_path(c, path);
    int ok = full && c->types->reference(c->types->context, full, type, name, length);
    free(full); return ok;
}
static void gd_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{
    gd_context *c = context;
    if (c->types->gap) {
        char *full = gd_path(c, path);
        if (full) c->types->gap(c->types->context, full, type, reason);
        free(full);
    }
}
static int gd_add(sh_decl_dependency_result *to, const sh_decl_dependency_result *from)
{
    if (from->visited > SIZE_MAX - to->visited || from->references > SIZE_MAX - to->references ||
        from->gaps > SIZE_MAX - to->gaps) { to->aborted = 1; return 0; }
    to->visited += from->visited; to->references += from->references; to->gaps += from->gaps;
    to->aborted |= from->aborted;
    return !to->aborted;
}
static int gd_state(const sh_decl_node *state, sh_decl_value_type type, const char *prefix,
    int root_object, const sh_decl_dependency_schema *types, sh_decl_dependency_result *result)
{
    gd_context context = {types, prefix, root_object, 0};
    sh_decl_dependency_schema adapter = {&context, gd_describe, gd_field, gd_reference, gd_gap, gd_dynamic_type};
    sh_decl_dependency_result walked = {0};
    (void)sh_decl_state_dependencies(state, type, &adapter, &walked);
    if (context.failed) walked.aborted = 1;
    return gd_add(result, &walked);
}

int sh_decl_graph_dependencies(const sh_decl_graph *graph, const char *declaration_type,
    sh_decl_native_schema *schema, sh_decl_dependency_result *result)
{
    static const char *const bases[] = {"idTypeInfoSubGraph", "idTypeInfoGraphNode", "idTypeInfoGraphLink"};
    const sh_decl_dependency_schema *types = sh_decl_native_schema_view(schema);
    const sh_decl_node *inherit, *state;
    sh_decl_value_type root = {0};
    size_t i;
    if (!result) return 0;
    memset(result, 0, sizeof(*result));
    if (!graph || !schema || !declaration_type ||
        sh_decl_native_schema_decl_type(schema, declaration_type, &root) != 1 ||
        sh_decl_native_schema_graph_type(schema, root) != 1) {
        result->aborted = 1; return 0;
    }
    inherit = sh_decl_tree_member(sh_decl_graph_syntax(graph), "inherit");
    if (inherit) {
        const char *name;
        size_t length;
        sh_decl_value_type reference_type = {root.name, "*"};
        if (!sh_decl_tree_literal(inherit, &name, &length)) { result->aborted = 1; return 0; }
        result->visited++;
        if (length) {
            if (sh_decl_native_schema_reader(schema, reference_type) != SH_DECL_READER_DECL) {
                result->gaps++;
                if (types->gap) types->gap(types->context, "inherit", reference_type, "graph inheritance reader is unavailable");
            } else if (!types->reference(types->context, "inherit", reference_type, name, length)) {
                result->aborted = 1; return 0;
            } else result->references++;
        }
    }
    state = sh_decl_graph_state(graph);
    if (state && !gd_state(state, root, "edit.object", 1, types, result)) return 0;
    for (i = 0; i < sh_decl_graph_count(graph); i++) {
        const sh_decl_graph_record *record = sh_decl_graph_at(graph, i);
        char prefix[80], *name;
        sh_decl_value_type type;
        int ok;
        snprintf(prefix, sizeof(prefix), "edit.records[%zu].object", i);
        if (record->class_name.length == SIZE_MAX ||
            !(name = (char *)malloc(record->class_name.length + 1))) { result->aborted = 1; return 0; }
        memcpy(name, record->class_name.text, record->class_name.length); name[record->class_name.length] = 0;
        type = (sh_decl_value_type){name, ""};
        if (sh_decl_native_schema_class_derives(schema, name, bases[record->kind]) != 1) {
            result->gaps++;
            if (types->gap) types->gap(types->context, prefix, type, "graph object class is unavailable or incompatible");
            free(name); continue;
        }
        ok = gd_state(record->state, type, prefix, 0, types, result);
        free(name);
        if (!ok) return 0;
    }
    return !result->gaps && !result->aborted;
}
