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

/* node is a positional header from sh_decl_entity_split. A repeated or
 * out-of-order copy is state text and never changes the class or parent. */
static int ec_header(const sh_decl_node *node, const char *key, char **value,
    char *error, size_t capacity)
{
    const char *text = "";
    size_t length = 0;
    if (node && (!node->assignment || node->reset || !sh_decl_tree_literal(node, &text, &length))) {
        snprintf(error, capacity, "%s: unsupported entity header value", key); return 0;
    }
    *value = ec_copy(text, length); return *value != NULL;
}

/* Match the verified native resource-name normalization, independently of
 * case-sensitive class names. Native declaration backslashes are path bytes. */
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
{ return sh_decl_entity_tree_class_classified(definition_tree, source, NULL, error, capacity); }

char *sh_decl_entity_tree_class_classified(const sh_decl_node *definition_tree,
    const sh_decl_entity_class_source *source, sh_decl_entity_merge_failure *failure, char *error, size_t capacity)
{ return sh_decl_entity_tree_class_detailed(definition_tree, source, failure, NULL, error, capacity); }

char *sh_decl_entity_tree_class_detailed(const sh_decl_node *definition_tree,
    const sh_decl_entity_class_source *source, sh_decl_entity_merge_failure *failure, char **unreflected_class,
    char *error, size_t capacity)
{
    sh_decl_entity_merge_failure kind = SH_DECL_ENTITY_MERGE_MALFORMED, ignored;
    ec_frame *frames = NULL;
    size_t count = 0, allocated = 0, i;
    char *owned_text = NULL, *result = NULL, fallback[512];
    const char *effective = NULL;
    char coverage[512] = "";
    int unreflected = 0;
    sh_decl_source definition = {0};
    if (unreflected_class) *unreflected_class = NULL;
    if (!failure) failure = &ignored;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!definition_tree || !definition_tree->compound || definition_tree->key) {
        *failure = kind; snprintf(error, capacity, "entity definition tree is invalid"); return NULL;
    }
    if (!source || !source->derives) {
        *failure = SH_DECL_ENTITY_MERGE_UNAVAILABLE; snprintf(error, capacity, "entity class metadata is unavailable"); return NULL;
    }
    for (;;) {
        const sh_decl_node *tree;
        sh_decl_node *owned_tree = NULL;
        ec_frame *frame;
        int read;
        kind = SH_DECL_ENTITY_MERGE_ALLOCATION;
        if (count == allocated) {
            size_t next = allocated ? allocated * 2 : 8;
            ec_frame *grown;
            if (next < allocated || next > SIZE_MAX / sizeof(*frames) ||
                !(grown = (ec_frame *)realloc(frames, next * sizeof(*frames)))) goto done;
            frames = grown; allocated = next;
        }
        frame = &frames[count++]; memset(frame, 0, sizeof(*frame));
        kind = SH_DECL_ENTITY_MERGE_MALFORMED;
        tree = count == 1 ? definition_tree : (owned_tree = sh_decl_tree_parse_ordered(definition, error, capacity));
        if (!tree) { if (strstr(error, "allocation")) kind = SH_DECL_ENTITY_MERGE_ALLOCATION; goto done; }
        sh_decl_entity_headers headers;
        sh_decl_entity_split(tree, &headers);
        read = ec_header(headers.class_name, "class", &frame->class_name, error, capacity) &&
            ec_header(headers.inherit, "inherit", &frame->inherit, error, capacity);
        sh_decl_tree_free(owned_tree);
        if (!read) goto done;
        if (!ec_normalize(frame->inherit)) {
            snprintf(error, capacity, "inherit: unsupported native resource name"); goto done;
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
            kind = read == 0 ? SH_DECL_ENTITY_MERGE_MISSING : SH_DECL_ENTITY_MERGE_UNAVAILABLE;
            snprintf(error, capacity, "inherit '%s': %s", frame->inherit,
                read == 0 ? "parent declaration is absent" : "parent source is unavailable or ambiguous"); goto done;
        }
    }
    for (i = count; i > 0; i--) {
        ec_frame *frame = &frames[i - 1];
        if (*frame->class_name) {
            /* entityDef Parse (VK 0x1417ad460 / GL 0x14179ff40) checks a layer's
             * own class only against its parent's class, through the reflection
             * database (VK 0x141a201d0 / GL 0x141a12410): equal spellings pass,
             * otherwise the reflected super chain must reach it; an unreflected
             * class fails that walk. Failure is a lexer Error ("Class %s does
             * not derive from %s"), which fails the declaration. A parentless
             * class is not checked by Parse: construction resolves it later
             * through the idClass registry (VK 0x1403dd9b0), whose
             * declaration-constructible classes (idManagedClass descendants)
             * are all reflected with the same ancestry in both verified images. */
            if (effective && strcmp(frame->class_name, effective)) {
                /* Nothing reflected derives from an unreflected class, so only
                 * an own class the source cannot read stays undecided there. */
                int derives = unreflected ? (source->known && source->known(source->context, frame->class_name) < 0 ? -1 : 0) :
                    source->derives(source->context, frame->class_name, effective);
                int known = derives < 0 && !unreflected && source->known ? source->known(source->context, frame->class_name) : -1;
                if (derives != 1) {
                    kind = derives == 0 || known == 0 ? SH_DECL_ENTITY_MERGE_MALFORMED : SH_DECL_ENTITY_MERGE_UNAVAILABLE;
                    snprintf(error, capacity, "class '%s': %s '%s'", frame->class_name, kind == SH_DECL_ENTITY_MERGE_MALFORMED ?
                        "does not derive from inherited class (entityDef Parse rejects the declaration)" :
                        "cannot verify ancestry against", effective);
                    goto done;
                }
            } else if (!effective && source->derives(source->context, frame->class_name, frame->class_name) != 1) {
                int known = source->known ? source->known(source->context, frame->class_name) : -1;
                if (known != 0) {
                    kind = SH_DECL_ENTITY_MERGE_UNAVAILABLE;
                    snprintf(error, capacity, "class '%s': native class metadata is unavailable", frame->class_name);
                    goto done;
                }
                /* Valid but uninterpretable: derived layers are still checked
                 * as Parse checks them before the chain reports coverage. */
                unreflected = 1;
                snprintf(coverage, sizeof(coverage), "class '%s': not a reflected native class; Parse accepts it, but "
                    "no declaration-constructible native class is unreflected in the verified images, and its "
                    "fields have no reflected schema (product coverage)", frame->class_name);
            }
            effective = frame->class_name;
        }
        if (!effective) { kind = SH_DECL_ENTITY_MERGE_MALFORMED; snprintf(error, capacity, "entityDef has no class or inherited class"); goto done; }
    }
    if (unreflected) {
        kind = SH_DECL_ENTITY_MERGE_COVERAGE; snprintf(error, capacity, "%s", coverage);
        if (unreflected_class && !(*unreflected_class = ec_copy(effective, strlen(effective)))) {
            kind = SH_DECL_ENTITY_MERGE_ALLOCATION; snprintf(error, capacity, "entity class resolution allocation failed");
        }
        goto done;
    }
    kind = SH_DECL_ENTITY_MERGE_ALLOCATION;
    result = ec_copy(effective, strlen(effective));
done:
    free(owned_text);
    for (i = 0; i < count; i++) { free(frames[i].class_name); free(frames[i].inherit); }
    free(frames);
    if (!result && !error[0]) { kind = SH_DECL_ENTITY_MERGE_ALLOCATION; snprintf(error, capacity, "entity class resolution allocation failed"); }
    if (!result) *failure = kind;
    return result;
}

char *sh_decl_entity_class(sh_decl_source definition,
    const sh_decl_entity_class_source *source, char *error, size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse_ordered(definition, error, capacity);
    char *result;
    if (!tree) return NULL;
    result = sh_decl_entity_tree_class(tree, source, error, capacity);
    sh_decl_tree_free(tree); return result;
}

int sh_decl_entity_merges(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, char **state_name, char *error, size_t capacity)
{ return sh_decl_entity_merges_classified(definition, source, state_name, NULL, error, capacity); }
int sh_decl_entity_merges_classified(const sh_decl_node *definition,
    const sh_decl_entity_class_source *source, char **state_name, sh_decl_entity_merge_failure *failure,
    char *error, size_t capacity)
{
    typedef struct seen { char *name; } seen;
    seen *names = NULL;
    size_t count = 0, allocated = 0;
    sh_decl_node *tree = NULL;
    char *parent = NULL, fallback[512];
    int result = -1, stateful = 0, edit_first = 0;
    sh_decl_entity_merge_failure kind = SH_DECL_ENTITY_MERGE_MALFORMED, ignored;
    const char *first_name = NULL;
    sh_decl_node *first_tree = NULL;
    if (!failure) failure = &ignored;
    *failure = SH_DECL_ENTITY_MERGE_DECIDED;
    if (state_name) *state_name = NULL;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!definition) { *failure = kind; snprintf(error, capacity, "entity definition tree is invalid"); return -1; }
    /* An unexpanded definition's prepared text is its own state text: nothing
     * merges, so its first state statement is the state (Parse reads only its
     * own flag; sh_decl_entity_expands). */
    int expands = sh_decl_entity_expands(definition);
    if (expands < 0) {
        *failure = expands == -2 ? SH_DECL_ENTITY_MERGE_COVERAGE : kind;
        snprintf(error, capacity, expands == -2 ? "expandInheritance: value needs the native preprocessor" :
            "expandInheritance: value is not a native boolean");
        return -1;
    }
    if (!expands) return 0;
    const sh_decl_node *layer = definition;
    for (;;) {
        sh_decl_entity_headers headers;
        sh_decl_source bytes = {0};
        sh_decl_entity_split(layer, &headers);
        /* Not the definition's own state: the accumulated tree's first node is
         * the root-most stateful ancestor's first state statement. */
        if (layer != definition && headers.state) {
            /* Keep the root-most stateful layer's tree for its first name. */
            stateful = 1;
            edit_first = headers.state->compound && headers.state->assignment;
            first_name = headers.state->key;
            sh_decl_tree_free(first_tree); first_tree = tree; tree = NULL;
        }
        free(parent); parent = NULL;
        if (!ec_header(headers.inherit, "inherit", &parent, error, capacity)) goto done;
        if (!ec_normalize(parent)) { snprintf(error, capacity, "inherit: unsupported native resource name"); goto done; }
        /* Without a positional parent this layer ends the chain; a definition
         * without one is the first stateful layer, known with no source. */
        if (!*parent) break;
        for (size_t i = 0; i < count; i++) if (!strcmp(names[i].name, parent)) {
            snprintf(error, capacity, "inherit '%s': cyclic entity inheritance", parent); goto done;
        }
        kind = SH_DECL_ENTITY_MERGE_ALLOCATION;
        if (count == allocated) {
            size_t next = allocated ? allocated * 2 : 8;
            seen *grown;
            if (next < allocated || next > SIZE_MAX / sizeof(*names) ||
                !(grown = (seen *)realloc(names, next * sizeof(*names)))) goto done;
            names = grown; allocated = next;
        }
        if (!(names[count].name = ec_copy(parent, strlen(parent)))) goto done;
        count++;
        kind = SH_DECL_ENTITY_MERGE_UNAVAILABLE;
        if (!source || !source->read) {
            snprintf(error, capacity, "inherit '%s': entity parent sources are unavailable", parent); goto done;
        }
        int read = source->read(source->context, parent, &bytes);
        if (read != 1 || !bytes.text) {
            if (read >= 0) kind = SH_DECL_ENTITY_MERGE_MISSING;
            snprintf(error, capacity, "inherit '%s': %s", parent,
                read == 0 ? "parent declaration is absent" : "parent source is unavailable or ambiguous"); goto done;
        }
        sh_decl_tree_free(tree);
        kind = SH_DECL_ENTITY_MERGE_MALFORMED;
        tree = sh_decl_tree_parse_ordered(bytes, error, capacity); free((char *)bytes.text);
        if (!tree) { if (strstr(error, "allocation")) kind = SH_DECL_ENTITY_MERGE_ALLOCATION; goto done; }
        layer = tree;
    }
    if (stateful && !edit_first) {
        kind = SH_DECL_ENTITY_MERGE_COVERAGE;
        snprintf(error, capacity, "inherited prepared state does not begin with a block assignment"); goto done;
    }
    kind = SH_DECL_ENTITY_MERGE_ALLOCATION;
    if (stateful && state_name && !(*state_name = ec_copy(first_name, strlen(first_name)))) goto done;
    result = stateful;
done:
    free(parent); sh_decl_tree_free(tree); sh_decl_tree_free(first_tree);
    for (size_t i = 0; i < count; i++) free(names[i].name);
    free(names);
    if (result < 0) *failure = kind;
    if (result < 0 && !error[0]) snprintf(error, capacity, "entity merge mode allocation failed");
    return result;
}

/* One later layer merges by sh_decl_tree_overlay (VK 0x141a65820 / GL
 * 0x141a57cf0 with the flag set). */
static int ec_overlay(sh_decl_node *target, sh_decl_node *source)
{ return sh_decl_tree_overlay(target, source); }

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

/* The count a block holds after this layer merges. The post-pass (VK
 * 0x141a65fa0 / GL 0x141a58470, retained in m62) reads the first child whose
 * name begins with "num" (case-sensitive prefix compare VK 0x1419fec20 / GL
 * 0x1419f0e80), as does the idList hook's prescan (VK 0x1409cd610, R-205); a
 * typed list block holds only num and item[N]. Appended (the tree had no
 * node), every occurrence is kept, so the block's first num counts. Merged,
 * each num updates the tree's first num in turn, so its last one counts. */
static const sh_decl_node *ec_layer_count(const sh_decl_node *target, const sh_decl_node *source, int append)
{
    const sh_decl_node *number = NULL;
    for (const sh_decl_node *child = source ? source->children : NULL; child; child = child->next)
        if (!strcmp(child->key, "num")) { number = child; if (append) break; }
    return number ? number : sh_decl_tree_member(target, "num");
}

/* Shrinking a list discards inherited rows, but an explicit write outside the
 * effective count is a native reader error. Check the incoming layer before
 * overlay/pruning so it cannot silently turn that invalid write into a no-op. */
static int ec_validate_layer(const sh_decl_node *target, const sh_decl_node *source, int append)
{
    typedef struct frame { const sh_decl_node *target, *source; } frame;
    frame *stack = NULL, current = {target, source};
    size_t count = 0, capacity = 0;
    int result = -1;
    for (;;) {
        const sh_decl_node *number = ec_layer_count(current.target, current.source, append);
        size_t limit = 0;
        if (number && !sh_decl_tree_list_count(number, &limit)) { result = -2; goto done; }
        for (const sh_decl_node *child = current.source->children; child; child = child->next) {
            size_t length = strlen(child->key), index;
            if (number && length > 6 && !strncmp(child->key, "item[", 5) && child->key[length - 1] == ']' &&
                (!ec_number(child->key + 5, length - 6, &index) || index >= limit)) {
                result = -2; goto done;
            }
            if (!child->compound) continue;
            if (count == capacity) {
                size_t next = capacity ? capacity * 2 : 16;
                frame *grown;
                if (next < capacity || next > SIZE_MAX / sizeof(*stack) ||
                    !(grown = realloc(stack, next * sizeof(*stack)))) goto done;
                stack = grown; capacity = next;
            }
            stack[count++] = (frame){sh_decl_tree_member(current.target, child->key), child};
        }
        if (!count) { result = 1; break; }
        current = stack[--count];
    }
done:
    free(stack); return result;
}

static int ec_prune(sh_decl_node *root)
{
    sh_decl_node **stack = NULL, *current = root;
    size_t count = 0, capacity = 0;
    int ok = 0;
    for (;;) {
        const sh_decl_node *number = sh_decl_tree_member(current, "num");
        size_t limit = 0;
        if (number && !sh_decl_tree_list_count(number, &limit)) goto done;
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
    char *text = NULL, fallback[512];
    int ok = 0, expands;
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    error[0] = 0;
    if (!definition || !source) goto done;
    /* Only the leaf's own positional flag selects expansion (Parse reads it
     * from its own text; the root-first merge VK 0x1417aec10 / GL 0x1417a16f0
     * never consults a parent's). Unexpanded, the prepared text is the own
     * state slice verbatim (VK 0x1417ae9d0 / GL 0x1417a14b0): no parent state,
     * merge or post-pass; Parse still inherits the class and editor variables. */
    if ((expands = sh_decl_entity_expands(definition)) < 0) {
        snprintf(error, capacity, expands == -2 ? "expandInheritance: value needs the native preprocessor" :
            "expandInheritance: value is not a native boolean");
        goto done;
    }
    /* Native reads accept repeated members (the member walker VK 0x141a209b0
     * reads every occurrence), so every layer keeps its occurrences in order. */
    text = sh_decl_tree_write(definition, &length);
    if (!text || !(tree = sh_decl_tree_parse_ordered((sh_decl_source){text, length}, error, capacity))) goto done;
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
        sh_decl_entity_headers headers;
        sh_decl_entity_split(tree, &headers);
        if (!ec_header(headers.inherit, "inherit", &parent, error, capacity)) goto done;
        layers[count++] = (layer){tree, parent}; tree = NULL;
        if (!ec_normalize(parent)) { snprintf(error, capacity, "inherit: unsupported name"); goto done; }
        if (!*parent || !expands) break;
        for (size_t i = 0; i + 1 < count; i++) if (!strcmp(layers[i].inherit, parent)) {
            snprintf(error, capacity, "inherit '%s': cyclic entity inheritance", parent); goto done;
        }
        read = source->read ? source->read(source->context, parent, &bytes) : -1;
        text = (char *)bytes.text;
        if (read != 1 || !text) { snprintf(error, capacity, "inherit '%s': parent source unavailable", parent); goto done; }
        tree = sh_decl_tree_parse_ordered(bytes, error, capacity); free(text); text = NULL;
        if (!tree) goto done;
    }
    result = calloc(1, sizeof(*result));
    if (!result) goto done;
    result->compound = 1;
    for (size_t i = count; i > 0; i--) {
        sh_decl_node *layer_root = layers[i - 1].tree;
        sh_decl_entity_headers headers;
        /* Consumed headers are a prefix and never state. Everything after them
         * is this layer's state text (the root-first merge reads the slice from
         * decl+0x100), so a repeated edit block or an unconsumed header merges
         * like any other statement. */
        sh_decl_entity_split(layer_root, &headers);
        while (layer_root->children && layer_root->children != headers.state) {
            sh_decl_node *node = layer_root->children;
            layer_root->children = node->next; node->next = NULL; sh_decl_tree_free(node);
        }
        int valid = ec_validate_layer(result, layer_root, !result->children);
        if (valid < 0) {
            snprintf(error, capacity, valid == -2 ? "inherited state has an invalid list count or explicit index" :
                "inherited state validation allocation failed"); goto done;
        }
        layers[i - 1].tree = NULL;
        /* Each layer merges with the flag set only once the tree has a node
         * (VK 0x141a65560 passes *tree != 0). Until then every occurrence,
         * nested ones included, is appended in order (VK 0x141a65410 / GL
         * 0x141a578e0), so the first stateful layer's repeats survive into the
         * prepared text. */
        if (!result->children) {
            result->children = layer_root->children; layer_root->children = NULL;
            sh_decl_tree_free(layer_root);
            if (expands && !ec_prune(result)) {
                snprintf(error, capacity, "inherited state has unsupported field shapes, counts, or allocation failure"); goto done;
            }
            continue;
        }
        if (!ec_overlay(result, layer_root) || !ec_prune(result)) {
            snprintf(error, capacity, "inherited state has unsupported field shapes, counts, or allocation failure"); goto done;
        }
    }
    /* SetEntityEditState (VK 0x1409d1ef0 / GL 0x1409d16b0) reads one
     * `type ops name =` header (VK 0x141a26810 / GL 0x141a18bd0, no name check)
     * and one class value, so only the prepared text's first statement is
     * state; later top-level statements are never read. Its name and type/ops
     * tokens select nothing (the instance's own type is read), so any block
     * assignment is state and is returned as edit. A later layer updates it
     * only under the same name (first match). A scalar first statement fails
     * the struct reader's opening brace, and a block without '=' is not the
     * header form; both stay outside this model. */
    if (result->children) {
        sh_decl_node *first = result->children;
        if (!first->compound || !first->assignment) {
            snprintf(error, capacity, "%s: first prepared state statement is not a block assignment", first->key); goto done;
        }
        sh_decl_tree_free(first->next); first->next = NULL;
        free(first->annotation); first->annotation = NULL;
        if (strcmp(first->key, "edit")) {
            char *renamed = ec_copy("edit", 4);
            if (!renamed) goto done;
            free(first->key); first->key = renamed;
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
