#include "package_spawn_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sh_package_spawn_registry_free(sh_package_spawn_registry *registry)
{
    if (!registry) return;
    for (size_t i = 0; i < registry->count; i++) {
        free(registry->rows[i].entity); free(registry->rows[i].body);
    }
    for (size_t i = 0; i < registry->modifier_count; i++) free(registry->modifiers[i].body);
    free(registry->rows); free(registry->modifiers); memset(registry, 0, sizeof(*registry));
}
static int sr_mask(const sh_decl_node *row, const sh_package_key_space *space, uint32_t *mask)
{
    const char *value; size_t length;
    return row && row->compound &&
        sh_decl_tree_literal(sh_decl_tree_member(row, "aiType"), &value, &length) &&
        sh_package_key_bits(space, value, length, mask);
}
static char *sr_body(const sh_decl_node *row)
{
    sh_decl_node view = {0}; size_t length;
    view.compound = 1; view.children = row->children;
    return sh_decl_tree_write(&view, &length);
}
static int sr_default(sh_decl_node *row, const char *name, const char *value)
{
    if (sh_decl_tree_member(row, name)) return 1;
    sh_decl_node *field = calloc(1, sizeof(*field));
    if (!field) return 0;
    field->key = _strdup(name); field->value = _strdup(value); field->assignment = 1;
    if (!field->key || !field->value) { sh_decl_tree_free(field); return 0; }
    field->next = row->children; row->children = field; return 1;
}
/* Native list allocation constructs every declared slot before explicit item
 * writes. Spawn rows start with no mask/definition; modifiers start at 1/1/0.
 * Inheritance must already be expanded, so an absent count is an empty list. */
static int sr_initialize(sh_decl_node *list, int modifiers)
{
    size_t count = 0;
    if (!list || !list->compound) return -2;
    if (!sr_default(list, "num", "0")) return -1;
    if (!sh_decl_tree_list_count(sh_decl_tree_member(list, "num"), &count)) return -2;
    for (sh_decl_node *row = list->children; row; row = row->next) {
        if (!strcmp(row->key, "num")) continue;
        if (strncmp(row->key, "item[", 5) || !row->compound) return -2;
        size_t index = 0, at = 5;
        if (row->key[at] < '0' || row->key[at] > '9') return -2;
        while (row->key[at] >= '0' && row->key[at] <= '9') {
            if (index > (SIZE_MAX - (size_t)(row->key[at] - '0')) / 10) return -2;
            index = index * 10 + (size_t)(row->key[at++] - '0');
        }
        if (row->key[at] != ']' || row->key[at + 1] || index >= count) return -2;
        char key[48]; snprintf(key, sizeof(key), "item[%zu]", index);
        if (strcmp(key, row->key)) return -2;
    }
    for (size_t i = 0; i < count; i++) {
        char key[48]; snprintf(key, sizeof(key), "item[%zu]", i);
        sh_decl_node *row = (sh_decl_node *)sh_decl_tree_member(list, key);
        if (!row) {
            row = calloc(1, sizeof(*row));
            if (row) row->key = _strdup(key);
            if (!row || !row->key) { sh_decl_tree_free(row); return -1; }
            row->assignment = row->compound = 1; row->next = list->children; list->children = row;
        }
        if (!sr_default(row, "aiType", "0") ||
            (modifiers ? !sr_default(row, "healthMod", "1") || !sr_default(row, "damageMod", "1") :
                !sr_default(row, "entityDef", "NULL"))) return -1;
    }
    return 1;
}
static int sr_initialize_tree(sh_decl_node *tree)
{
    const sh_decl_node *edit = tree ? sh_decl_tree_member(tree, "edit") : NULL;
    const char *fields[] = {"aiTypeList", "globalAIMods"};
    for (size_t i = 0; i < 2; i++) {
        sh_decl_node *list = (sh_decl_node *)sh_decl_tree_member(edit, fields[i]);
        int status = list ? sr_initialize(list, (int)i) : 1;
        if (status < 0) return status;
    }
    return tree != NULL;
}
int sh_package_spawn_registry_prepare(sh_decl_source source, char **output,
    size_t *length, char *error, size_t capacity)
{
    char fallback[512];
    if (!error || !capacity) { error = fallback; capacity = sizeof(fallback); }
    *error = 0;
    sh_decl_node *tree = NULL;
    int status = -1;
    if (output) *output = NULL;
    if (length) *length = 0;
    if (!output || !length) goto done;
    tree = sh_decl_tree_parse(source, error, capacity);
    if (!tree) { status = *error && !strstr(error, "allocation") ? -2 : -1; goto done; }
    status = sr_initialize_tree(tree);
    if (status > 0 && !(*output = sh_decl_tree_write(tree, length))) status = -1;
done:
    sh_decl_tree_free(tree);
    if (status < 0 && !*error) snprintf(error, capacity, status == -2 ?
        "native spawn list has an invalid count, row or index" : "spawn list preparation allocation failed");
    return status;
}
/* Numbered native lists have one count and exactly those item indices. Read
 * numeric index order, independent of how the source spells/reorders fields. */
static int sr_count(const sh_decl_node *list, size_t *count)
{
    size_t n = 0, members = 0;
    if (!list || !list->compound || !sh_decl_tree_list_count(sh_decl_tree_member(list, "num"), &n)) return 0;
    for (const sh_decl_node *row = list->children; row; row = row->next) members++;
    if (!members || n != members - 1) return 0;
    *count = n; return 1;
}
int sh_package_spawn_registry_read(sh_decl_source source, const sh_package_key_space *space,
    sh_package_spawn_registry *out, char *error, size_t capacity)
{
    sh_package_spawn_registry pending = {0};
    sh_decl_node *tree = NULL;
    int result = -1;
    if (!out || !space) goto done;
    tree = sh_decl_tree_parse(source, error, capacity);
    if (!tree || sr_initialize_tree(tree) <= 0) goto done;
    const sh_decl_node *edit = sh_decl_tree_member(tree, "edit");
    const sh_decl_node *list = edit ? sh_decl_tree_member(edit, "aiTypeList") : NULL;
    const sh_decl_node *mods = edit ? sh_decl_tree_member(edit, "globalAIMods") : NULL;
    if (!list) { result = 0; goto done; }
    size_t count = 0, modifier_count = 0;
    if (!sr_count(list, &count) || (mods && !sr_count(mods, &modifier_count))) goto done;
    if (count > SIZE_MAX / sizeof(*pending.rows) || modifier_count > SIZE_MAX / sizeof(*pending.modifiers)) goto done;
    pending.rows = calloc(count ? count : 1, sizeof(*pending.rows));
    pending.modifiers = calloc(modifier_count ? modifier_count : 1, sizeof(*pending.modifiers));
    if (!pending.rows || !pending.modifiers) goto done;
    pending.count = count; pending.modifier_count = modifier_count;
    for (size_t i = 0; i < count; i++) {
        char name[48]; const char *entity; size_t length;
        snprintf(name, sizeof(name), "item[%zu]", i);
        const sh_decl_node *row = sh_decl_tree_member(list, name);
        if (!sr_mask(row, space, &pending.rows[i].mask) ||
            !sh_decl_tree_literal(sh_decl_tree_member(row, "entityDef"), &entity, &length) ||
            length == SIZE_MAX) goto done;
        if (!strcmp(sh_decl_tree_member(row, "entityDef")->value, "NULL")) { entity = ""; length = 0; }
        pending.rows[i].entity = malloc(length + 1); pending.rows[i].body = sr_body(row);
        if (!pending.rows[i].entity || !pending.rows[i].body) goto done;
        memcpy(pending.rows[i].entity, entity, length); pending.rows[i].entity[length] = 0;
    }
    for (size_t i = 0; i < modifier_count; i++) {
        char name[48]; snprintf(name, sizeof(name), "item[%zu]", i);
        const sh_decl_node *row = sh_decl_tree_member(mods, name);
        if (!sr_mask(row, space, &pending.modifiers[i].mask) ||
            !(pending.modifiers[i].body = sr_body(row))) goto done;
    }
    *out = pending; memset(&pending, 0, sizeof(pending)); result = 1;
done:
    if (result < 0 && error && capacity && !*error)
        snprintf(error, capacity, "spawn registration list could not be captured completely");
    sh_decl_tree_free(tree); sh_package_spawn_registry_free(&pending); return result;
}
size_t sh_package_spawn_registry_type(const sh_package_spawn_registry *registry, size_t row)
{
    if (!registry || row >= registry->count || !registry->rows[row].entity || !*registry->rows[row].entity) return SIZE_MAX;
    for (size_t i = 0; i <= row; i++)
        if (!strcmp(registry->rows[i].entity, registry->rows[row].entity)) return i;
    return SIZE_MAX;
}

typedef struct sr_compose_row { char *body; size_t copies; } sr_compose_row;
char *sh_package_spawn_registry_classification(sh_decl_source source,
    const sh_package_key_space *space, const char *entity, size_t *length, char *error, size_t capacity)
{
    sh_package_spawn_registry registry = {0};
    sh_decl_node *tree = NULL, *selected = NULL;
    char *fragment = NULL, *out = NULL;
    if (length) *length = 0;
    if (!length || !entity || sh_package_spawn_registry_read(source, space, &registry, error, capacity) != 1) goto done;
    size_t first;
    for (first = 0; first < registry.count; first++) if (!strcmp(registry.rows[first].entity, entity)) break;
    if (first == registry.count) goto done;
    size_t modifier = sh_package_spawn_registry_modifier(&registry, first);
    const char *row = registry.rows[first].body;
    const char *mod = modifier == SIZE_MAX ? "" : registry.modifiers[modifier].body;
    size_t size = strlen(row) + strlen(mod) + 256;
    if (size < strlen(row) || !(fragment = malloc(size))) goto done;
    snprintf(fragment, size, "{ edit = { aiTypeList = { num = 1; item[0] = %s } "
        "globalAIMods = { num = %d; %s%s } } }", row, modifier != SIZE_MAX,
        modifier == SIZE_MAX ? "" : "item[0] = ", mod);
    tree = sh_decl_tree_parse(source, error, capacity);
    selected = sh_decl_tree_parse((sh_decl_source){fragment, strlen(fragment)}, error, capacity);
    sh_decl_node *edit = (sh_decl_node *)sh_decl_tree_member(tree, "edit");
    sh_decl_node *part = (sh_decl_node *)sh_decl_tree_member(selected, "edit");
    if (!edit || !part) goto done;
    for (sh_decl_node **slot = &edit->children; *slot;) {
        sh_decl_node *node = *slot;
        if (!strcmp(node->key, "aiTypeList") || !strcmp(node->key, "globalAIMods")) {
            *slot = node->next; node->next = NULL; sh_decl_tree_free(node);
        } else slot = &node->next;
    }
    sh_decl_node **tail = &edit->children;
    while (*tail) tail = &(*tail)->next;
    *tail = part->children; part->children = NULL;
    out = sh_decl_tree_write(tree, length);
done:
    free(fragment); sh_decl_tree_free(tree); sh_decl_tree_free(selected);
    sh_package_spawn_registry_free(&registry);
    if (!out && error && capacity && !*error) snprintf(error, capacity, "cannot capture AI classification");
    return out;
}
static void sr_rows_free(sr_compose_row *rows, size_t count)
{
    for (size_t i = 0; i < count; i++) free(rows[i].body);
    free(rows);
}
/* A complete source may intentionally repeat a row (native random-selection
 * weight). Peer publication shares that record's maximum multiplicity; it
 * does not erase repetitions inside one authored table or multiply them for
 * every clipboard retaining the same program. */
static int sr_compose_list(sh_decl_node *destination, const sh_decl_node *const *sources,
    size_t source_count, const char *name)
{
    sr_compose_row *rows = NULL;
    size_t count = 0, capacity = 0;
    sh_decl_node *list = calloc(1, sizeof(*list)), **tail;
    int ok = 0;
    if (!list || !(list->key = _strdup(name))) goto done;
    list->compound = list->assignment = 1; tail = &list->children;
    size_t emitted = 0;
    for (size_t source = 0; source < source_count; source++) {
        const sh_decl_node *edit = sh_decl_tree_member(sources[source], "edit");
        const sh_decl_node *input = edit ? sh_decl_tree_member(edit, name) : NULL;
        size_t row_count = 0;
        if (!input) continue;
        if (!sr_count(input, &row_count)) goto done;
        size_t *occurrences = calloc(row_count ? row_count : 1, sizeof(*occurrences));
        char **bodies = calloc(row_count ? row_count : 1, sizeof(*bodies));
        if (!occurrences || !bodies) { free(occurrences); free(bodies); goto done; }
        int complete = 1;
        for (size_t i = 0; complete && i < row_count; i++) {
            char key[48]; snprintf(key, sizeof(key), "item[%zu]", i);
            const sh_decl_node *row = sh_decl_tree_member(input, key);
            if (!row || !row->compound || !(bodies[i] = sr_body(row))) { complete = 0; break; }
            occurrences[i] = 1;
            for (size_t j = 0; j < i; j++) if (!strcmp(bodies[i], bodies[j])) occurrences[i]++;
            size_t at;
            for (at = 0; at < count; at++) if (!strcmp(rows[at].body, bodies[i])) break;
            if (at < count && rows[at].copies >= occurrences[i]) continue;
            sh_decl_node *copy = sh_decl_tree_parse((sh_decl_source){bodies[i], strlen(bodies[i])}, NULL, 0);
            if (!copy) { complete = 0; break; }
            snprintf(key, sizeof(key), "item[%zu]", emitted++);
            free(copy->key); copy->key = _strdup(key); copy->assignment = 1;
            if (!copy->key) { sh_decl_tree_free(copy); complete = 0; break; }
            *tail = copy; tail = &copy->next;
            if (at == count) {
                if (count == capacity) {
                    size_t next = capacity ? capacity * 2 : 16;
                    sr_compose_row *grown = next > capacity && next <= SIZE_MAX / sizeof(*grown) ?
                        realloc(rows, next * sizeof(*grown)) : NULL;
                    if (!grown) { complete = 0; break; }
                    rows = grown; capacity = next;
                }
                rows[count] = (sr_compose_row){_strdup(bodies[i]), 0};
                if (!rows[count].body) { complete = 0; break; }
                count++;
            }
            rows[at].copies = occurrences[i];
        }
        for (size_t i = 0; i < row_count; i++) free(bodies[i]);
        free(bodies); free(occurrences); if (!complete) goto done;
    }
    sh_decl_node *num = calloc(1, sizeof(*num));
    char number[48]; snprintf(number, sizeof(number), "%zu", emitted);
    if (!num || !(num->key = _strdup("num")) || !(num->value = _strdup(number))) {
        sh_decl_tree_free(num); goto done;
    }
    num->assignment = 1; num->next = list->children; list->children = num;
    sh_decl_node **slot = &destination->children;
    while (*slot && strcmp((*slot)->key, name)) slot = &(*slot)->next;
    sh_decl_node *old = *slot;
    list->next = old ? old->next : NULL; *slot = list; list = NULL;
    if (old) { old->next = NULL; sh_decl_tree_free(old); }
    ok = 1;
done:
    sh_decl_tree_free(list); sr_rows_free(rows, count); return ok;
}
static int sr_private(uint32_t mask)
{ return (mask & SH_PACKAGE_KEY_PRIVATE) && !(mask & SH_PACKAGE_KEY_COMPOSITE); }
static int sr_owned(uint32_t mask, const sh_package_key_groups *groups)
{
    uint32_t keys = mask & SH_PACKAGE_KEY_PRIVATE;
    return sr_private(mask) && !(groups && keys && !(keys & ~groups->historical));
}
/* These three independent native scalar fields occupy health/damage/type at
 * offsets 0/4/8. Only this proven row shape may ignore their statement order;
 * other fields, annotations and compound bodies retain their authored order.
 * Values remain native parser input, never host-parsed floats. */
static void sr_modifier_scalars(sh_decl_node *row)
{
    const char *names[] = {"healthMod", "damageMod", "aiType"};
    sh_decl_node *fields[3]; size_t count = 0;
    for (sh_decl_node *field = row->children; field; field = field->next) count++;
    if (count != 3 || row->reset || row->annotation) return;
    for (size_t i = 0; i < 3; i++) {
        fields[i] = (sh_decl_node *)sh_decl_tree_member(row, names[i]);
        if (!fields[i] || !fields[i]->assignment || fields[i]->compound || fields[i]->reset || fields[i]->annotation) return;
    }
    row->children = fields[0]; fields[0]->next = fields[1]; fields[1]->next = fields[2]; fields[2]->next = NULL;
}
static int sr_modifier_mask(sh_decl_node *row, uint32_t type)
{
    sh_decl_node *mask = (sh_decl_node *)sh_decl_tree_member(row, "aiType");
    char number[32];
    if (type & 0x80000000u) snprintf(number, sizeof(number), "\"%d\"", (int)(int32_t)type);
    else snprintf(number, sizeof(number), "\"%u\"", type);
    char *value = mask ? _strdup(number) : NULL;
    if (!value) return 0;
    free(mask->value); mask->value = value; return 1;
}
static int sr_modifier_normalize(sh_decl_node *row)
{
    const char *literal = NULL; size_t length = 0; uint32_t type;
    if (!sh_decl_tree_literal(sh_decl_tree_member(row, "aiType"), &literal, &length) || !length ||
        !(literal[0] == '-' || (literal[0] >= '0' && literal[0] <= '9')) ||
        !sr_mask(row, NULL, &type) || !sr_private(type)) return 1;
    if (!sr_modifier_mask(row, type)) return 0;
    sr_modifier_scalars(row); return 1;
}
typedef struct sr_modifier_policy { uint32_t type; sh_decl_node *row; char *body; int authoritative; } sr_modifier_policy;
static int sr_policy_order(const void *a, const void *b)
{
    uint32_t x = ((const sr_modifier_policy *)a)->type, y = ((const sr_modifier_policy *)b)->type;
    return x < y ? -1 : x > y;
}
/* Retained classifications select a policy in their own original program.
 * Publish one exact-type row for that selection before the broad native rows.
 * The immutable dispatch program uses equality for these private types, so
 * overlapping multi-key types do not demand an impossible shared row order.
 * Stock and frozen historical types keep their native intersection policy. */
static int sr_modifier_project(sh_decl_node *edit, const sh_decl_source *sources,
    const unsigned char *classifiers, size_t source_count,
    const sh_package_key_groups *groups, char *error, size_t capacity)
{
    sh_decl_node *list = (sh_decl_node *)sh_decl_tree_member(edit, "globalAIMods");
    sh_package_key_space numeric = {0};
    sr_modifier_policy *policies = NULL;
    size_t count = 0, policy_count = 0, emitted = 0;
    sh_decl_node *output = NULL, **tail = &output;
    int ok = 0;
    if (!list || !sr_count(list, &count)) goto done;
    for (int pass = 0; pass < (classifiers ? 2 : 1); pass++) for (size_t s = 0; s < source_count; s++) {
        int authoritative = !classifiers || classifiers[s];
        if (classifiers && (classifiers[s] != 0) != (pass == 0)) continue;
        sh_package_spawn_registry program = {0};
        if (sh_package_spawn_registry_read(sources[s], &numeric, &program, error, capacity) != 1) goto done;
        if (classifiers && authoritative && program.count != 1) {
            sh_package_spawn_registry_free(&program); goto done;
        }
        int valid = 1;
        for (size_t i = 0; valid && i < program.count; i++) {
            uint32_t type = program.rows[i].mask;
            if (!sr_owned(type, groups) || sh_package_spawn_registry_type(&program, i) != i) continue;
            size_t first;
            for (first = 0; first < program.modifier_count; first++)
                if ((authoritative || sr_owned(program.modifiers[first].mask, groups)) &&
                    sh_package_key_modifier_matches(groups, type, program.modifiers[first].mask)) break;
            int absent = first == program.modifier_count;
            if (absent && !authoritative) continue;
            const char *body = absent ? NULL : program.modifiers[first].body;
            sh_decl_node *row = body ? sh_decl_tree_parse((sh_decl_source){body, strlen(body)}, error, capacity) : NULL;
            char *canonical = NULL;
            if (row && sr_modifier_mask(row, type)) { sr_modifier_scalars(row); canonical = sr_body(row); }
            size_t peer;
            for (peer = 0; peer < policy_count; peer++) if (policies[peer].type == type) break;
            if (!absent && !canonical) valid = 0;
            else if (peer < policy_count && (authoritative || !policies[peer].authoritative)) {
                if ((!policies[peer].body != !canonical) ||
                    (canonical && strcmp(policies[peer].body, canonical))) {
                    if (error && capacity) snprintf(error, capacity,
                        "retained private classification %u has different modifier policies", type);
                    valid = 0;
                }
            } else if (peer == policy_count) {
                sr_modifier_policy *grown = policy_count < SIZE_MAX / sizeof(*grown) - 1 ?
                    realloc(policies, (policy_count + 1) * sizeof(*grown)) : NULL;
                if (!grown) valid = 0;
                else {
                    policies = grown; policies[policy_count++] = (sr_modifier_policy){type, row, canonical, authoritative};
                    row = NULL; canonical = NULL;
                }
            }
            sh_decl_tree_free(row); free(canonical);
        }
        sh_package_spawn_registry_free(&program);
        if (!valid) goto done;
    }
    if (!policy_count) return 1;
    if (count > SIZE_MAX - policy_count) goto done;
    qsort(policies, policy_count, sizeof(*policies), sr_policy_order);
    for (size_t i = 0; i < policy_count + count; i++) {
        sh_decl_node *row = NULL;
        if (i < policy_count) {
            row = policies[i].row; policies[i].row = NULL;
            if (!row) continue;
        }
        else {
            char key[48]; snprintf(key, sizeof(key), "item[%zu]", i - policy_count);
            const sh_decl_node *input = sh_decl_tree_member(list, key);
            char *body = input ? sr_body(input) : NULL;
            row = body ? sh_decl_tree_parse((sh_decl_source){body, strlen(body)}, error, capacity) : NULL;
            free(body);
            if (row) {
                if (!sr_modifier_normalize(row)) { sh_decl_tree_free(row); goto done; }
                body = sr_body(row);
                if (!body) { sh_decl_tree_free(row); goto done; }
                size_t peer;
                for (peer = 0; peer < policy_count; peer++)
                    if (policies[peer].body && !strcmp(policies[peer].body, body)) break;
                free(body);
                if (peer < policy_count) { sh_decl_tree_free(row); continue; }
            }
        }
        char key[48]; snprintf(key, sizeof(key), "item[%zu]", emitted++);
        if (!row) goto done;
        free(row->key); row->key = _strdup(key); row->assignment = 1;
        if (!row->key) { sh_decl_tree_free(row); goto done; }
        *tail = row; tail = &row->next;
    }
    sh_decl_node *num = (sh_decl_node *)sh_decl_tree_member(list, "num");
    char number[48]; snprintf(number, sizeof(number), "%zu", emitted);
    char *value = _strdup(number);
    if (!value) goto done;
    free(num->value); num->value = value;
    sh_decl_node *old = list->children;
    while (old) { sh_decl_node *next = old->next; old->next = NULL; if (old != num) sh_decl_tree_free(old); old = next; }
    list->children = num; num->next = output; output = NULL; ok = 1;
done:
    for (size_t i = 0; i < policy_count; i++) { sh_decl_tree_free(policies[i].row); free(policies[i].body); }
    free(policies); sh_decl_tree_free(output);
    if (!ok && error && capacity && !*error) snprintf(error, capacity, "cannot project retained modifier policies");
    return ok;
}
char *sh_package_spawn_registry_compose_classified(sh_decl_source base, const sh_decl_source *tables,
    const unsigned char *classifiers, size_t count, const sh_package_key_groups *groups,
    size_t *length, char *error, size_t capacity)
{
    sh_decl_node *tree = NULL, **parts = NULL;
    char *out = NULL;
    if (length) *length = 0;
    if (!length || (count && !tables) || count >= SIZE_MAX / sizeof(*parts) - 1) goto done;
    tree = sh_decl_tree_parse(base, error, capacity);
    sh_decl_node *edit = (sh_decl_node *)sh_decl_tree_member(tree, "edit");
    parts = calloc(count + 1, sizeof(*parts));
    if (!tree || !edit || !edit->compound || !parts) goto done;
    if (sr_initialize_tree(tree) <= 0) goto done;
    for (size_t i = 0; i < count; i++)
        if (!(parts[i] = sh_decl_tree_parse(tables[i], error, capacity)) || sr_initialize_tree(parts[i]) <= 0) goto done;
    parts[count] = tree;
    /* Normalize proven numeric masks before multiplicity accounting, so bare
     * and quoted spellings cannot grow a duplicate on every reprojection. */
    for (size_t i = 0; i <= count; i++) {
        sh_decl_node *part = (sh_decl_node *)sh_decl_tree_member(parts[i], "edit");
        sh_decl_node *list = (sh_decl_node *)sh_decl_tree_member(part, "globalAIMods");
        for (sh_decl_node *row = list ? list->children : NULL; row; row = row->next)
            if (row->compound && !sr_modifier_normalize(row)) goto done;
    }
    if (sr_compose_list(edit, (const sh_decl_node *const *)parts, count + 1, "aiTypeList") &&
        sr_compose_list(edit, (const sh_decl_node *const *)parts, count + 1, "globalAIMods") &&
        sr_modifier_project(edit, tables, classifiers, count, groups, error, capacity))
        out = sh_decl_tree_write(tree, length);
done:
    if (parts) for (size_t i = 0; i < count; i++) sh_decl_tree_free(parts[i]);
    free(parts); sh_decl_tree_free(tree);
    if (!out && error && capacity && !*error) snprintf(error, capacity, "cannot publish retained spawn registrations");
    return out;
}
char *sh_package_spawn_registry_compose(sh_decl_source base, const sh_decl_source *tables,
    size_t count, const sh_package_key_groups *groups, size_t *length, char *error, size_t capacity)
{
    return sh_package_spawn_registry_compose_classified(base, tables, NULL, count,
        groups, length, error, capacity);
}
size_t sh_package_spawn_registry_modifier(const sh_package_spawn_registry *registry, size_t row)
{
    size_t first = sh_package_spawn_registry_type(registry, row);
    if (first == SIZE_MAX) return SIZE_MAX;
    for (size_t i = 0; i < registry->modifier_count; i++)
        if (registry->rows[first].mask & registry->modifiers[i].mask) return i;
    return SIZE_MAX;
}
int sh_package_spawn_registry_equivalent(const sh_package_spawn_registry *a, const sh_package_spawn_registry *b)
{
    if (!a || !b || a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++) {
        size_t same_a = 0, same_b = 0, peer = SIZE_MAX;
        for (size_t j = 0; j < a->count; j++) {
            same_a += !strcmp(a->rows[i].body, a->rows[j].body);
            same_b += !strcmp(a->rows[i].body, b->rows[j].body);
            if (peer == SIZE_MAX && !strcmp(a->rows[i].entity, b->rows[j].entity)) peer = j;
        }
        if (same_a != same_b || peer == SIZE_MAX) return 0;
        if (!*a->rows[i].entity) continue;
        size_t first = sh_package_spawn_registry_type(a, i);
        if (a->rows[first].mask != b->rows[peer].mask) return 0;
        size_t am = sh_package_spawn_registry_modifier(a, i), bm = sh_package_spawn_registry_modifier(b, peer);
        if (am == SIZE_MAX || bm == SIZE_MAX) { if (am != bm) return 0; }
        else if (strcmp(a->modifiers[am].body, b->modifiers[bm].body)) return 0;
    }
    return 1;
}
int sh_package_spawn_registry_keys(const sh_package_spawn_registry *registry,
    const uint32_t *row_keys, uint32_t mask, int role, sh_package_key_set *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!registry || (registry->count && !row_keys) ||
        (role != SH_PACKAGE_SPAWN_REQUEST && role != SH_PACKAGE_SPAWN_CLASSIFIER)) return 0;
    sh_package_key_set keys = {{0}};
    for (size_t i = 0; i < registry->count; i++) {
        if (!sh_package_key_private_valid(row_keys[i])) return 0;
        if (role == SH_PACKAGE_SPAWN_CLASSIFIER && sh_package_spawn_registry_type(registry, i) != i) continue;
        if ((registry->rows[i].mask & mask) && !sh_package_key_set_add(&keys, row_keys[i])) {
            /* Equal row keys are permitted only after the compiler has proved
             * equal registrations; set insertion itself is idempotent. */
            if (!sh_package_key_set_has(&keys, row_keys[i])) return 0;
        }
    }
    *out = keys; return 1;
}
int sh_package_spawn_registry_code(const sh_package_spawn_registry *registry,
    const uint32_t *row_keys, uint32_t mask, int role,
    const sh_package_key_encoder *encoder, uint32_t *code)
{
    sh_package_key_set keys;
    uint32_t single = 0, bits = 0;
    size_t count = 0;
    int marked = 0;
    if (!code) return -1;
    *code = 0;
    if (!sh_package_spawn_registry_keys(registry, row_keys, mask, role, &keys)) return -1;
    for (size_t i = 0; i < SH_PACKAGE_KEY_CODES; i++) {
        uint32_t key = sh_package_key_code(i);
        if (!sh_package_key_set_has(&keys, key)) continue;
        count++; single = key; bits |= key; marked |= !!(key & SH_PACKAGE_KEY_MARKER);
    }
    if (count < 2) { *code = single; return 1; }
    if (!marked) { *code = bits; return 1; }
    if (!encoder || !encoder->group) return -1;
    uint32_t group = 0;
    int result = encoder->group(encoder->context, &keys, &group);
    if (result != 1) return result == 0 ? 0 : -1;
    if (!(group & SH_PACKAGE_KEY_MARKER) || !sh_package_key_private_valid(group)) return -1;
    *code = group; return 1;
}
