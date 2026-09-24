#include "decl_polymorphic.h"
#include <string.h>

int sh_decl_polymorphic_read(const sh_decl_node *node, sh_decl_value_type base_type,
    const sh_decl_dependency_schema *schema, sh_decl_polymorphic_state *state,
    const char **reason)
{
    const sh_decl_node *class_node = node, *object = NULL;
    const char *name;
    size_t length;
    if (state) memset(state, 0, sizeof(*state));
    if (reason) *reason = "polymorphic value requires an explicit class and optional object state";
    if (!node || !state || node->reset || !schema || !schema->dynamic_type || !base_type.name) return 0;
    if (node->compound) {
        class_node = node->children;
        if (!class_node || strcmp(class_node->key, "className") || !class_node->assignment) return 0;
        object = class_node->next;
        if (object && (strcmp(object->key, "object") || object->next || !object->assignment ||
            !object->compound || object->reset)) return 0;
    }
    if (!sh_decl_tree_literal(class_node, &name, &length)) return 0;
    if (!length || (length == 4 && !memcmp(name, "NULL", 4))) {
        if (reason) *reason = "polymorphic null or empty value depends on native destination state";
        return 0;
    }
    if (schema->dynamic_type(schema->context, base_type, name, length, &state->type) != 1 ||
        !state->type.name || !*state->type.name || (state->type.ops && *state->type.ops)) {
        memset(state, 0, sizeof(*state));
        if (reason) *reason = "selected object class is unavailable or incompatible with its native base";
        return -1;
    }
    state->object = object;
    return 1;
}

int sh_decl_script_read(const sh_decl_node *node, sh_decl_value_type base_type,
    const sh_decl_dependency_schema *schema, sh_decl_value_type *selected,
    sh_decl_node *fields, const char **reason)
{
    const sh_decl_node *selector = node;
    const char *name;
    size_t length;
    if (selected) memset(selected, 0, sizeof(*selected));
    if (fields) memset(fields, 0, sizeof(*fields));
    if (reason) *reason = "script object requires its class selector before field values";
    if (!node || node->reset || !selected || !fields || !schema || !schema->dynamic_type) return 0;
    if (node->compound) {
        selector = node->children;
        if (!selector) return 1;
        if (strcmp(selector->key, "object") || !selector->assignment) return 0;
    }
    if (!sh_decl_tree_literal(selector, &name, &length)) return 0;
    if (length == 4 && !memcmp(name, "NULL", 4) &&
        (node->compound || selector->value[0] != '"')) return 1;
    if (!length || schema->dynamic_type(schema->context, base_type, name, length, selected) != 1 ||
        !selected->name || !*selected->name) {
        if (reason) *reason = "script class metadata is unavailable";
        return 0;
    }
    fields->compound = 1;
    if (node->compound) fields->children = selector->next;
    return 1;
}
