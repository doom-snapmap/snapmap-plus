#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_variants.h"
#include "package_sources.h"
#include "config_json.h"
#include "decl_tree.h"

static const char pv_entity_domain[] = "snapmap-plus reader-bound entity parent\n";
static const char pv_row_domain[] = "snapmap-plus reader-bound editor row\n";

void sh_package_variant_free(sh_package_variant *variant)
{
    if (!variant) return;
    for (size_t i = 0; i < variant->count; i++) { free(variant->members[i].key); free(variant->members[i].value); }
    free(variant->members); free(variant->parent); memset(variant, 0, sizeof(*variant));
}

/* A declaration name or member key stated inside a quoted literal. */
static int pv_plain(const char *text, int key)
{
    if (!text || !*text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (key ? !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') :
            (*p < 0x21 || *p > 0x7e || *p == '"' || *p == '\\')) return 0;
    }
    return 1;
}
static int pv_member_order(const void *a, const void *b)
{ return strcmp(((const sh_package_variant_member *)a)->key, ((const sh_package_variant_member *)b)->key); }

unsigned char *sh_package_variant_text(const char *parent, const sh_package_variant_member *members,
    size_t count, size_t *length)
{
    sh_package_variant_member *sorted = NULL;
    char *out = NULL;
    size_t size = 64, used = 0;
    *length = 0;
    if (!pv_plain(parent, 0) || !count || !members || count > 4096) return NULL;
    if (!(sorted = malloc(count * sizeof(*sorted)))) return NULL;
    memcpy(sorted, members, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), pv_member_order);
    size += strlen(parent);
    for (size_t i = 0; i < count; i++) {
        if (!pv_plain(sorted[i].key, 1) || !pv_plain(sorted[i].value, 0) ||
            (i && !strcmp(sorted[i - 1].key, sorted[i].key))) { free(sorted); return NULL; }
        size += strlen(sorted[i].key) + strlen(sorted[i].value) + 16;
    }
    if (!(out = malloc(size))) { free(sorted); return NULL; }
    used = (size_t)snprintf(out, size, "{\n\tinherit = \"%s\";\n\tedit = {\n", parent);
    for (size_t i = 0; i < count; i++)
        used += (size_t)snprintf(out + used, size - used, "\t\t%s = \"%s\";\n", sorted[i].key, sorted[i].value);
    used += (size_t)snprintf(out + used, size - used, "\t}\n}\n");
    free(sorted);
    *length = used; return (unsigned char *)out;
}
unsigned char *sh_package_variant_row_text(const char *row, const char *entity_def, size_t *length)
{
    sh_package_variant_member member = {"entityDef", (char *)entity_def};
    return sh_package_variant_text(row, &member, 1, length);
}
int sh_package_variant_name(int row, const unsigned char *text, size_t length, char name[69])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    const char *domain = row ? pv_row_domain : pv_entity_domain;
    int ok = 0;
    name[0] = 0;
    if (!text || length > 0x7fffffff) return 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0 &&
        BCryptHashData(hash, (PUCHAR)domain, (ULONG)strlen(domain), 0) >= 0 &&
        BCryptHashData(hash, (PUCHAR)text, (ULONG)length, 0) >= 0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
        memcpy(name, "smp/", 4);
        for (size_t i = 0; i < 32; i++) snprintf(name + 4 + i * 2, 3, "%02x", digest[i]);
        ok = 1;
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

static const sh_compiled_resource *pv_resource(const sh_package_compilation *compiled, const char *type, const char *name)
{
    char path[512], *key;
    const sh_compiled_resource *found;
    if (!compiled || !type || !name || snprintf(path, sizeof(path), "generated/decls/%s/%s.decl", type, name) >= (int)sizeof(path))
        return NULL;
    key = compiled->canonical_path ? compiled->canonical_path(path) : sh_package_engine_path(path);
    if (!key) return NULL;
    found = sh_package_compilation_find(compiled, key);
    free(key);
    return found;
}
int sh_package_variant_served(const sh_package_compilation *compiled, const char *type, const char *name)
{ return pv_resource(compiled, type, name) != NULL; }

static char *pv_literal(const sh_decl_node *node)
{
    const char *text; size_t length; char *out;
    if (!node || node->compound || !sh_decl_tree_literal(node, &text, &length) || !length || !(out = malloc(length + 1)))
        return NULL;
    memcpy(out, text, length); out[length] = 0;
    return out;
}
int sh_package_variant_find(const sh_package_compilation *compiled, const char *name, sh_package_variant *out)
{
    char error[256], derived[69];
    unsigned char *body = NULL;
    size_t length = 0;
    sh_decl_node *tree = NULL;
    sh_package_variant found = {0};
    int result = 0;
    memset(out, 0, sizeof(*out));
    if (!name || strncmp(name, "smp/", 4) || strlen(name) != 68) return 0;
    const sh_compiled_resource *resource = pv_resource(compiled, "entitydef", name);
    /* Only a generated declaration (no authored source) can be a variant. */
    if (!resource || resource->source_count || !resource->body) return 0;
    if (!(body = sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, error, sizeof(error)))) return -1;
    if (!sh_package_variant_name(0, body, length, derived)) { result = -1; goto done; }
    if (strcmp(derived, name)) goto done;
    if (!(tree = sh_decl_tree_parse_ordered((sh_decl_source){(const char *)body, length}, error, sizeof(error)))) {
        result = -1; goto done;
    }
    const sh_decl_node *inherit = tree->children, *edit = inherit ? inherit->next : NULL;
    if (!inherit || !inherit->key || strcmp(inherit->key, "inherit") || !edit || !edit->key ||
        strcmp(edit->key, "edit") || !edit->compound || edit->next) { result = -1; goto done; }
    if (!(found.parent = pv_literal(inherit))) { result = -1; goto done; }
    for (const sh_decl_node *m = edit->children; m; m = m->next) found.count++;
    if (!found.count || !(found.members = calloc(found.count, sizeof(*found.members)))) { result = -1; goto done; }
    size_t i = 0;
    for (const sh_decl_node *m = edit->children; m; m = m->next, i++)
        if (!m->key || !(found.members[i].key = _strdup(m->key)) || !(found.members[i].value = pv_literal(m))) {
            result = -1; goto done;
        }
    *out = found; memset(&found, 0, sizeof(found)); result = 1;
done:
    sh_package_variant_free(&found); sh_decl_tree_free(tree); free(body);
    return result;
}

/* One pending byte replacement of the document. */
typedef struct pv_edit { size_t at, removed; char *text; } pv_edit;
typedef struct pv_fold {
    const sh_package_compilation *compiled;
    const char *json;
    int materialize, failed;
    /* retarget mode */
    const char *const *expected, *const *targets;
    size_t carrier, count;
    pv_edit *edits;
    size_t edit_count, edit_capacity;
    char **sites;
    size_t site_count;
} pv_fold;

static const sh_json_field_span *pv_field(const sh_json_field_span *fields, size_t count, const char *key)
{
    size_t n = strlen(key);
    for (size_t i = 0; i < count; i++) if (fields[i].key_length == n && !memcmp(fields[i].key, key, n)) return fields + i;
    return NULL;
}
static int pv_text(const sh_json_field_span *field, char *out, size_t capacity)
{
    size_t length;
    return field && field->kind == SH_JSON_STRING &&
        sh_native_json_decode_string(field->value, field->value_length, out, capacity, &length) && strlen(out) == length;
}
static int pv_gameplay(void *context, const char *key, size_t length, unsigned depth)
{
    (void)context; (void)depth;
    return length != sizeof("editorVars") - 1 || memcmp(key, "editorVars", length) != 0;
}
static int pv_edit_span(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    if (!depth) {
        const sh_json_field_span *edit = pv_field(fields, count, "edit");
        if (edit) *(sh_json_field_span *)context = *edit;
    }
    return 1;
}
typedef struct pv_keys { const char *key; int found; } pv_keys;
static int pv_has_key(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    pv_keys *keys = context;
    if (!depth && pv_field(fields, count, keys->key)) keys->found = 1;
    return 1;
}
static int pv_add(pv_fold *fold, size_t at, size_t removed, char *text)
{
    if (!text) { fold->failed = 1; return 0; }
    if (fold->edit_count == fold->edit_capacity) {
        size_t next = fold->edit_capacity ? fold->edit_capacity * 2 : 16;
        pv_edit *grown = next < SIZE_MAX / sizeof(*grown) ? realloc(fold->edits, next * sizeof(*grown)) : NULL;
        if (!grown) { free(text); fold->failed = 1; return 0; }
        fold->edits = grown; fold->edit_capacity = next;
    }
    fold->edits[fold->edit_count++] = (pv_edit){at, removed, text};
    return 1;
}
static char *pv_quoted(const char *name)
{
    size_t n = strlen(name);
    char *out = malloc(n + 3);
    if (out) { out[0] = '"'; memcpy(out + 1, name, n); out[n + 1] = '"'; out[n + 2] = 0; }
    return out;
}
/* The entity carriers sh_package_map_state_sites visits, in its order. */
static int pv_carrier(void *context, const sh_json_field_span *fields, size_t count, unsigned depth)
{
    pv_fold *fold = context;
    char target_name[256], inherit[4096] = "";
    const sh_json_field_span *target = pv_field(fields, count, "targetType");
    const sh_json_field_span *state = pv_field(fields, count, "state"), *field;
    sh_json_field_span edit = {0};
    (void)depth;
    if (!state || state->kind == SH_JSON_NULL) return 1;
    if (!target) target = pv_field(fields, count, "~type");
    if (!target) return 1;
    if (!pv_text(target, target_name, sizeof(target_name))) return 0;
    if (strcmp(target_name, "idDeclEntityDef")) return 1;
    field = pv_field(fields, count, "inherit");
    if (field && field->kind != SH_JSON_NULL && !pv_text(field, inherit, sizeof(inherit))) return 0;
    if (state->kind != SH_JSON_OBJECT || !sh_native_json_visit_objects_filtered(state->value,
        state->value_length, 128, pv_edit_span, NULL, &edit)) return 0;
    if (!edit.value || edit.kind == SH_JSON_NULL) return 1;
    size_t index = fold->carrier++;
    if (!field || field->kind != SH_JSON_STRING || field->value_length < 2 || field->value[0] != '"') {
        if (fold->targets && index < fold->count && fold->targets[index]) { fold->failed = 1; return 0; }
        return 1;
    }
    size_t at = (size_t)(field->value - fold->json);
    if (fold->targets) {
        if (index >= fold->count || !fold->targets[index]) return 1;
        if (!fold->expected || !fold->expected[index] || strcmp(fold->expected[index], inherit)) { fold->failed = 1; return 0; }
        return pv_add(fold, at, field->value_length, pv_quoted(fold->targets[index]));
    }
    sh_package_variant variant = {0};
    int found = sh_package_variant_find(fold->compiled, inherit, &variant);
    if (found < 0) { fold->failed = 1; return 0; }
    if (!found) return 1;
    int ok = pv_add(fold, at, field->value_length, pv_quoted(variant.parent));
    if (ok && fold->materialize && edit.kind == SH_JSON_OBJECT && edit.value_length >= 2) {
        size_t first = 1;
        while (first < edit.value_length && (edit.value[first] == ' ' || edit.value[first] == '\t' ||
            edit.value[first] == '\r' || edit.value[first] == '\n')) first++;
        int empty = first < edit.value_length && edit.value[first] == '}';
        size_t inserted = 0;
        for (size_t i = 0; ok && i < variant.count; i++) {
            pv_keys keys = {variant.members[i].key, 0};
            /* The copy's own members are read at depth 0; its nested objects and
             * arrays (spawnPosition, spawnOrientation) are scanned, not refused. */
            if (!sh_native_json_visit_objects_filtered(edit.value, edit.value_length, 128, pv_has_key, NULL, &keys)) ok = 0;
            else if (!keys.found) {
                size_t size = strlen(variant.members[i].key) + strlen(variant.members[i].value) + 8;
                char *piece = malloc(size);
                if (piece) snprintf(piece, size, "\"%s\":\"%s\"%s", variant.members[i].key, variant.members[i].value,
                    empty && !inserted ? "" : ",");
                ok = pv_add(fold, (size_t)(edit.value - fold->json) + 1, 0, piece);
                inserted++;
                size_t n = strlen(variant.members[i].key) + 24;
                char *site = ok ? malloc(n) : NULL, **grown = site ? realloc(fold->sites, (fold->site_count + 1) * sizeof(*grown)) : NULL;
                if (!grown) { free(site); ok = 0; }
                else { snprintf(site, n, "%zu\n%s", index, variant.members[i].key); fold->sites = grown; fold->sites[fold->site_count++] = site; }
            }
        }
        if (!ok) fold->failed = 1;
    }
    sh_package_variant_free(&variant);
    return ok;
}
static int pv_edit_order(const void *a, const void *b)
{
    const pv_edit *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}
static char *pv_apply(pv_fold *fold, const char *json, size_t length, size_t *out_length)
{
    size_t size = length, used = 0, from = 0;
    char *out;
    /* Insertions at one point keep their visiting order (stable by index). */
    for (size_t i = 0; i < fold->edit_count; i++) size += strlen(fold->edits[i].text);
    if (!(out = malloc(size + 1))) return NULL;
    for (size_t i = 1; i < fold->edit_count; i++) {
        pv_edit edit = fold->edits[i]; size_t j = i;
        while (j && pv_edit_order(fold->edits + j - 1, &edit) > 0) { fold->edits[j] = fold->edits[j - 1]; j--; }
        fold->edits[j] = edit;
    }
    for (size_t i = 0; i < fold->edit_count; i++) {
        const pv_edit *edit = fold->edits + i;
        if (edit->at < from || edit->at + edit->removed > length) { free(out); return NULL; }
        memcpy(out + used, json + from, edit->at - from); used += edit->at - from;
        size_t n = strlen(edit->text);
        memcpy(out + used, edit->text, n); used += n;
        from = edit->at + edit->removed;
    }
    memcpy(out + used, json + from, length - from); used += length - from;
    out[used] = 0; *out_length = used;
    return out;
}
void sh_package_variant_sites_free(char **sites, size_t count)
{
    for (size_t i = 0; i < count; i++) free(sites[i]);
    free(sites);
}
static void pv_fold_free(pv_fold *fold)
{
    for (size_t i = 0; i < fold->edit_count; i++) free(fold->edits[i].text);
    free(fold->edits); sh_package_variant_sites_free(fold->sites, fold->site_count);
}
char *sh_package_variant_fold_sites(const sh_package_compilation *compiled, const char *json, size_t length,
    int materialize, size_t *out_length, int *changed, char ***sites, size_t *site_count)
{
    pv_fold fold = {compiled, json, materialize};
    char *out = NULL;
    *out_length = 0; *changed = 0;
    if (sites) { *sites = NULL; *site_count = 0; }
    if (!json) return NULL;
    if (sh_native_json_visit_objects_filtered(json, length, 128, pv_carrier, pv_gameplay, &fold) && !fold.failed)
        out = pv_apply(&fold, json, length, out_length);
    if (out) *changed = fold.edit_count != 0;
    if (out && sites) { *sites = fold.sites; *site_count = fold.site_count; fold.sites = NULL; fold.site_count = 0; }
    pv_fold_free(&fold);
    return out;
}
char *sh_package_variant_fold(const sh_package_compilation *compiled, const char *json, size_t length,
    int materialize, size_t *out_length, int *changed)
{ return sh_package_variant_fold_sites(compiled, json, length, materialize, out_length, changed, NULL, NULL); }
char *sh_package_variant_retarget(const char *json, size_t length, const char *const *expected,
    const char *const *targets, size_t count, size_t *out_length)
{
    pv_fold fold = {NULL, json, 0};
    char *out = NULL;
    *out_length = 0;
    if (!json || (count && !targets)) return NULL;
    fold.expected = expected; fold.targets = targets; fold.count = count;
    if (sh_native_json_visit_objects_filtered(json, length, 128, pv_carrier, pv_gameplay, &fold) && !fold.failed &&
        fold.carrier == count)
        out = pv_apply(&fold, json, length, out_length);
    pv_fold_free(&fold);
    return out;
}
