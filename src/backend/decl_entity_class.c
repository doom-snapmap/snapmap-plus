#include "decl_entity_class.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ec_frame { char *class_name, *inherit; } ec_frame;

static char *ec_copy(const char *text, size_t length)
{
    char *out;
    if (length == SIZE_MAX || !(out = (char *)malloc(length + 1))) return NULL;
    memcpy(out, text, length); out[length] = 0; return out;
}

static int ec_header(const sh_decl_node *tree, const char *key, char **value,
    char *error, size_t capacity)
{
    const sh_decl_node *node = sh_decl_tree_member(tree, key);
    const char *text = "";
    size_t length = 0;
    if (node && (!node->assignment || node->reset || !sh_decl_tree_literal(node, &text, &length))) {
        snprintf(error, capacity, "%s: unsupported entity header value", key); return 0;
    }
    *value = ec_copy(text, length); return *value != NULL;
}

/* Match the verified native resource-name normalization, independently of
 * case-sensitive class names. Literal escapes remain the lexer adapter's job. */
static int ec_normalize(char *name)
{
    size_t i;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 128) return 0;
        name[i] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : (char)c;
    }
    if (name[0] == '/' && name[1] != '/') memmove(name, name + 1, strlen(name));
    return 1;
}

char *sh_decl_entity_tree_class(const sh_decl_node *definition_tree,
    const sh_decl_entity_class_source *source, char *error, size_t capacity)
{
    ec_frame *frames = NULL;
    size_t count = 0, allocated = 0, i;
    char *owned_text = NULL, *result = NULL, fallback[512];
    const char *effective = NULL;
    sh_decl_source definition = {0};
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!definition_tree || !definition_tree->compound || definition_tree->key) {
        snprintf(error, capacity, "entity definition tree is invalid"); return NULL;
    }
    if (!source || !source->derives) { snprintf(error, capacity, "entity class metadata is unavailable"); return NULL; }
    for (;;) {
        const sh_decl_node *tree;
        sh_decl_node *owned_tree = NULL;
        ec_frame *frame;
        int read;
        if (count == allocated) {
            size_t next = allocated ? allocated * 2 : 8;
            ec_frame *grown;
            if (next < allocated || next > SIZE_MAX / sizeof(*frames) ||
                !(grown = (ec_frame *)realloc(frames, next * sizeof(*frames)))) goto done;
            frames = grown; allocated = next;
        }
        frame = &frames[count++]; memset(frame, 0, sizeof(*frame));
        tree = count == 1 ? definition_tree : (owned_tree = sh_decl_tree_parse(definition, error, capacity));
        if (!tree) goto done;
        read = ec_header(tree, "class", &frame->class_name, error, capacity) &&
            ec_header(tree, "inherit", &frame->inherit, error, capacity);
        sh_decl_tree_free(owned_tree);
        if (!read) goto done;
        if (!ec_normalize(frame->inherit)) {
            snprintf(error, capacity, "inherit: unsupported native resource name"); goto done;
        }
        if (*frame->class_name && source->derives(source->context, frame->class_name, frame->class_name) != 1) {
            snprintf(error, capacity, "class '%s': native class metadata is unavailable", frame->class_name); goto done;
        }
        if (!*frame->inherit) break;
        for (i = 0; i + 1 < count; i++) if (!strcmp(frames[i].inherit, frame->inherit)) {
            snprintf(error, capacity, "inherit '%s': cyclic entity inheritance", frame->inherit); goto done;
        }
        free(owned_text); owned_text = NULL;
        definition = (sh_decl_source){0};
        read = source->read ? source->read(source->context, frame->inherit, &definition) : -1;
        owned_text = (char *)definition.text;
        if (read != 1 || !definition.text) {
            snprintf(error, capacity, "inherit '%s': %s", frame->inherit,
                read == 0 ? "parent declaration is absent" : "parent source is unavailable or ambiguous"); goto done;
        }
    }
    for (i = count; i > 0; i--) {
        ec_frame *frame = &frames[i - 1];
        if (*frame->class_name) {
            if (effective) {
                int derives = source->derives(source->context, frame->class_name, effective);
                if (derives != 1) {
                    snprintf(error, capacity, "class '%s': %s '%s'", frame->class_name,
                        derives == 0 ? "does not derive from inherited class" : "cannot verify ancestry against", effective);
                    goto done;
                }
            }
            effective = frame->class_name;
        }
        if (!effective) { snprintf(error, capacity, "entityDef has no class or inherited class"); goto done; }
    }
    result = ec_copy(effective, strlen(effective));
done:
    free(owned_text);
    for (i = 0; i < count; i++) { free(frames[i].class_name); free(frames[i].inherit); }
    free(frames);
    if (!result && !error[0]) snprintf(error, capacity, "entity class resolution allocation failed");
    return result;
}

char *sh_decl_entity_class(sh_decl_source definition,
    const sh_decl_entity_class_source *source, char *error, size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse(definition, error, capacity);
    char *result;
    if (!tree) return NULL;
    result = sh_decl_entity_tree_class(tree, source, error, capacity);
    sh_decl_tree_free(tree); return result;
}

typedef struct ec_merge_frame { sh_decl_node *target, *source; } ec_merge_frame;

/* Consume one source tree into the owned result. The native expanded-text
 * builder updates an existing named node and recursively overlays its children;
 * it retains the latest reset marker rather than clearing those children. */
static int ec_overlay(sh_decl_node *target, sh_decl_node *source)
{
    ec_merge_frame *stack = NULL;
    size_t count = 0, capacity = 0;
    int ok = 0;
    ec_merge_frame current = {target, source};
    for (;;) {
        while (current.source->children) {
            sh_decl_node *incoming = current.source->children, **slot = &current.target->children;
            current.source->children = incoming->next; incoming->next = NULL;
            while (*slot && strcmp((*slot)->key, incoming->key)) {
                /* The assignment parser does not certify differently cased
                 * spellings of the same reflected field. */
                if (!_stricmp((*slot)->key, incoming->key)) {
                    sh_decl_tree_free(incoming); goto done;
                }
                slot = &(*slot)->next;
            }
            if (!*slot) { *slot = incoming; continue; }
            if ((*slot)->compound != incoming->compound) {
                sh_decl_tree_free(incoming); goto done;
            }
            (*slot)->assignment = incoming->assignment; (*slot)->reset = incoming->reset;
            if (incoming->compound) {
                if (count == capacity) {
                    size_t next = capacity ? capacity * 2 : 16;
                    ec_merge_frame *grown;
                    if (next < capacity || next > SIZE_MAX / sizeof(*stack) ||
                        !(grown = realloc(stack, next * sizeof(*stack)))) {
                        sh_decl_tree_free(incoming); goto done;
                    }
                    stack = grown; capacity = next;
                }
                stack[count++] = (ec_merge_frame){*slot, incoming};
            } else {
                free((*slot)->value); (*slot)->value = incoming->value; incoming->value = NULL;
                sh_decl_tree_free(incoming);
            }
        }
        sh_decl_tree_free(current.source); current.source = NULL;
        if (!count) { ok = 1; break; }
        current = stack[--count];
    }
done:
    sh_decl_tree_free(current.source);
    while (count) sh_decl_tree_free(stack[--count].source);
    free(stack); return ok;
}

static int ec_number(const char *text, size_t length, size_t *value)
{
    size_t result = 0;
    if (!length) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned digit = (unsigned char)text[i] - '0';
        if (digit > 9 || result > (INT32_MAX - digit) / 10u) return 0;
        result = result * 10 + digit;
    }
    *value = result; return 1;
}

static int ec_prune(sh_decl_node *root)
{
    sh_decl_node **stack = NULL, *current = root;
    size_t count = 0, capacity = 0;
    int ok = 0;
    for (;;) {
        const sh_decl_node *number = sh_decl_tree_member(current, "num");
        size_t limit = 0;
        if (number && (number->compound || !number->value ||
            !ec_number(number->value, strlen(number->value), &limit))) goto done;
        for (sh_decl_node **slot = &current->children; *slot;) {
            sh_decl_node *child = *slot;
            size_t index, length = strlen(child->key);
            if (number && length > 6 && !strncmp(child->key, "item[", 5) && child->key[length - 1] == ']') {
                if (!ec_number(child->key + 5, length - 6, &index)) goto done;
                if (index >= limit) {
                    *slot = child->next; child->next = NULL; sh_decl_tree_free(child); continue;
                }
            }
            if (child->compound) {
                if (count == capacity) {
                    size_t next = capacity ? capacity * 2 : 16;
                    sh_decl_node **grown;
                    if (next < capacity || next > SIZE_MAX / sizeof(*stack) ||
                        !(grown = realloc(stack, next * sizeof(*stack)))) goto done;
                    stack = grown; capacity = next;
                }
                stack[count++] = child;
            }
            slot = &child->next;
        }
        if (!count) { ok = 1; break; }
        current = stack[--count];
    }
done:
    free(stack); return ok;
}

sh_decl_node *sh_decl_entity_expanded_state(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, char *error, size_t capacity)
{
    typedef struct layer { sh_decl_node *tree; char *inherit; } layer;
    layer *layers = NULL;
    size_t count = 0, allocated = 0, length = 0;
    sh_decl_node *tree = NULL, *result = NULL;
    const sh_decl_node *expand = sh_decl_tree_member(definition, "expandInheritance");
    char *text = NULL, fallback[512];
    int ok = 0;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!definition || !source) goto done;
    if (expand && (!expand->value || (strcmp(expand->value, "true") && strcmp(expand->value, "1")))) {
        snprintf(error, capacity, "expandInheritance: unexpanded or unsupported state"); goto done;
    }
    text = sh_decl_tree_write(definition, &length);
    if (!text || !(tree = sh_decl_tree_parse((sh_decl_source){text, length}, error, capacity))) goto done;
    free(text); text = NULL;
    for (;;) {
        char *parent = NULL;
        sh_decl_source bytes = {0};
        int read;
        if (count == allocated) {
            size_t next = allocated ? allocated * 2 : 8;
            layer *grown;
            if (next < allocated || next > SIZE_MAX / sizeof(*layers) ||
                !(grown = realloc(layers, next * sizeof(*layers)))) goto done;
            layers = grown; allocated = next;
        }
        if (!ec_header(tree, "inherit", &parent, error, capacity)) goto done;
        layers[count++] = (layer){tree, parent}; tree = NULL;
        if (!ec_normalize(parent)) { snprintf(error, capacity, "inherit: unsupported name"); goto done; }
        if (!*parent) break;
        for (size_t i = 0; i + 1 < count; i++) if (!strcmp(layers[i].inherit, parent)) {
            snprintf(error, capacity, "inherit '%s': cyclic entity inheritance", parent); goto done;
        }
        read = source->read ? source->read(source->context, parent, &bytes) : -1;
        text = (char *)bytes.text;
        if (read != 1 || !text) { snprintf(error, capacity, "inherit '%s': parent source unavailable", parent); goto done; }
        tree = sh_decl_tree_parse(bytes, error, capacity); free(text); text = NULL;
        if (!tree) goto done;
    }
    result = calloc(1, sizeof(*result));
    if (!result) goto done;
    result->compound = 1;
    for (size_t i = count; i > 0; i--) {
        sh_decl_node *layer_root = layers[i - 1].tree;
        sh_decl_node **slot = &layer_root->children;
        /* Header/editor metadata never becomes runtime state. */
        while (*slot) {
            sh_decl_node *node = *slot;
            if (strcmp(node->key, "edit")) {
                *slot = node->next; node->next = NULL; sh_decl_tree_free(node);
            } else {
                if (!node->compound) { snprintf(error, capacity, "edit: unsupported state shape"); goto done; }
                slot = &node->next;
            }
        }
        layers[i - 1].tree = NULL;
        if (!ec_overlay(result, layer_root) || !ec_prune(result)) {
            snprintf(error, capacity, "inherited state has unsupported field shapes, counts, or allocation failure"); goto done;
        }
    }
    ok = 1;
done:
    free(text); sh_decl_tree_free(tree);
    for (size_t i = 0; i < count; i++) { sh_decl_tree_free(layers[i].tree); free(layers[i].inherit); }
    free(layers);
    if (!ok) {
        sh_decl_tree_free(result); result = NULL;
        if (!error[0]) snprintf(error, capacity, "inherited state allocation failed");
    }
    return result;
}
