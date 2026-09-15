#include "resource_graph_prepared.h"
#include "resource_graph.h"
#include "decl_native_schema.h"
#include <stdlib.h>
#include <string.h>

struct sh_resource_graph_preparation {
    sh_decl_native_registry *registry;
    sh_decl_native_schema *schema;
    size_t unresolved;
    int (*visitor)(void *context, const char *type, const char *name);
    void *visitor_context;
};
static int rp_reference(void *context, const char *path, sh_decl_value_type type,
                        const char *name, size_t length)
{
    sh_resource_graph_preparation *p = context;
    sh_decl_reference reference = {0};
    char *copy;
    int status, ok = 1;
    (void)path;
    if (sh_decl_native_schema_reader(p->schema, type) != SH_DECL_READER_DECL) {
        p->unresolved++; return 1;
    }
    if (length == SIZE_MAX || !(copy = (char *)malloc(length + 1))) return 0;
    memcpy(copy, name, length); copy[length] = 0;
    status = sh_decl_native_registry_resolve(p->registry, type.name, copy, &reference);
    if (status > 0) {
        if (p->visitor) ok = p->visitor(p->visitor_context, reference.type, reference.name);
        else sh_resource_graph_reference(reference.type, reference.name);
    }
    else p->unresolved++;
    sh_decl_reference_clear(&reference); free(copy); return ok;
}
sh_resource_graph_preparation *sh_resource_graph_prepare_open(sh_decl_registry_source source,
    uintptr_t reflection)
{
    sh_resource_graph_preparation *p = (sh_resource_graph_preparation *)calloc(1, sizeof(*p));
    sh_decl_native_source metadata = {source.context, source.read, reflection};
    sh_decl_dependency_observer observer = {p, rp_reference, NULL};
    if (!p) return NULL;
    p->registry = sh_decl_native_registry_open(source, NULL, 0);
    p->schema = sh_decl_native_schema_open(metadata, observer, NULL, 0);
    if (!p->registry || !p->schema) { sh_resource_graph_prepare_close(p); return NULL; }
    return p;
}
int sh_resource_graph_prepare_entity(sh_resource_graph_preparation *p,
    const char *name, sh_decl_dependency_result *result)
{
    sh_decl_dependency_result walked = {0};
    sh_decl_reference reference = {0};
    sh_decl_prepared_state state = {0};
    sh_resource_graph_frame frame;
    sh_decl_node *tree = NULL;
    sh_decl_source source;
    sh_decl_value_type type;
    char *wrapped = NULL;
    int tracked = 0, complete = 0, inspected = 0;
    if (result) memset(result, 0, sizeof(*result));
    if (!p || !name || !*name) return 0;
    if (sh_decl_native_registry_resolve(p->registry, "idDeclEntityDef", name, &reference) != 1 ||
        !reference.resource) goto done;
    tracked = sh_resource_graph_begin_missing_state(&frame, reference.type, reference.name, 0);
    /* A complete or active state is left to its native owner. A skipped active
     * record does not imply the whole dependency graph is complete. */
    if (!tracked) goto done;
    if (!sh_decl_native_registry_entity_state(p->registry, &reference, &state) ||
        state.length > SIZE_MAX - 3 || !(wrapped = (char *)malloc(state.length + 3))) goto done;
    wrapped[0] = '{'; memcpy(wrapped + 1, state.text, state.length);
    wrapped[state.length + 1] = '}'; wrapped[state.length + 2] = 0;
    source.text = wrapped; source.length = state.length + 2;
    tree = sh_decl_tree_parse(source, NULL, 0);
    if (!tree) goto done;
    type.name = state.class_name; type.ops = "";
    frame.expanded_inheritance = state.expanded_inheritance;
    p->unresolved = 0;
    inspected = 1;
    complete = sh_decl_entity_dependencies(tree, type,
        sh_decl_native_schema_view(p->schema), &walked);
    walked.gaps += p->unresolved;
    complete = complete && !p->unresolved;
done:
    if (tracked) {
        if (inspected) sh_resource_graph_end(&frame, complete);
        else sh_resource_graph_discard(&frame);
    }
    sh_decl_tree_free(tree); free(wrapped); sh_decl_prepared_state_clear(&state);
    sh_decl_reference_clear(&reference);
    if (result) *result = walked;
    return complete;
}
void sh_resource_graph_prepare_close(sh_resource_graph_preparation *p)
{
    if (p) { sh_decl_native_schema_close(p->schema); sh_decl_native_registry_close(p->registry); free(p); }
}

int sh_resource_graph_prepare_inline(sh_resource_graph_preparation *p,
    const char *class_name, const char *inherit, const char *json, size_t length,
    int (*visitor)(void *context, const char *type, const char *name), void *context,
    sh_decl_dependency_result *result)
{
    sh_decl_dependency_result walked = {0};
    sh_decl_reference reference = {0};
    sh_decl_prepared_state state = {0};
    sh_decl_value_type type = {class_name, ""};
    sh_resource_graph_frame pause;
    int complete = 0;
    if (!p || !visitor) { walked.aborted = 1; goto done; }
    /* Registry source probes can trigger engine lookups. Neither those nor
     * the emitted references belong to an enclosing canonical asset parse. */
    sh_resource_graph_pause(&pause);
    if (!class_name || !*class_name) {
        if (!inherit || !*inherit ||
            sh_decl_native_registry_resolve(p->registry, "idDeclEntityDef", inherit, &reference) != 1 ||
            !sh_decl_native_registry_entity_state(p->registry, &reference, &state)) {
            walked.gaps++; goto resume;
        }
        type.name = state.class_name;
    }
    p->unresolved = 0; p->visitor = visitor; p->visitor_context = context;
    complete = sh_decl_json_state_dependencies(json, length, type,
        sh_decl_native_schema_view(p->schema), &walked);
    walked.gaps += p->unresolved; complete = complete && !p->unresolved;
    p->visitor = NULL; p->visitor_context = NULL;
resume:
    sh_resource_graph_end(&pause, 0);
done:
    sh_decl_reference_clear(&reference); sh_decl_prepared_state_clear(&state);
    if (result) *result = walked;
    return complete;
}
