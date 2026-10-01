/* package_fsm.c -- compiler side of per-configuration state-machine webs (see package_fsm.h). */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_fsm.h"
#include "decl_graph.h"

#pragma comment(lib, "bcrypt.lib")

void sh_package_fsm_names_free(char **names, size_t count)
{
    if (names) for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

static int pf_fail(char *error, size_t capacity, const char *reason)
{
    if (error && capacity) snprintf(error, capacity, "%s", reason);
    return 0;
}

static const sh_decl_node *pf_configs(const sh_decl_node *root)
{
    const sh_decl_node *edit = sh_decl_tree_member(root, "edit");
    const sh_decl_node *configs = edit ? sh_decl_tree_member(edit, "layersConfigs") : NULL;
    return configs && configs->compound && !configs->reset ? configs : NULL;
}

static const sh_decl_node *pf_child(const sh_decl_node *parent, const char *key)
{
    for (const sh_decl_node *child = parent ? parent->children : NULL; child; child = child->next)
        if (child->key && !strcmp(child->key, key)) return child;
    return NULL;
}

int sh_package_fsm_config_layers(sh_decl_source first, const char *config,
    char ***layers, size_t *count, char *error, size_t capacity)
{
    sh_decl_node *root = sh_decl_tree_parse_ordered(first, error, capacity);
    const sh_decl_node *configs, *entry;
    size_t n = 0;
    char **out = NULL;
    *layers = NULL; *count = 0;
    if (!root) return -1;
    configs = pf_configs(root);
    if (!configs) { sh_decl_tree_free(root); pf_fail(error, capacity, "no layer configurations"); return -1; }
    entry = pf_child(configs, config);
    if (!entry) { sh_decl_tree_free(root); return 0; }
    if (!entry->compound || entry->reset) { sh_decl_tree_free(root); pf_fail(error, capacity, "malformed configuration"); return -1; }
    for (const sh_decl_node *layer = entry->children; layer; layer = layer->next) {
        const char *text; size_t length; char **grown;
        if (!layer->key || strcmp(layer->key, "layer") || !sh_decl_tree_literal(layer, &text, &length)) {
            sh_package_fsm_names_free(out, n); sh_decl_tree_free(root);
            pf_fail(error, capacity, "malformed configuration layer"); return -1;
        }
        grown = realloc(out, (n + 1) * sizeof(*grown));
        if (!grown || !(grown[n] = malloc(length + 1))) {
            sh_package_fsm_names_free(grown ? grown : out, n); sh_decl_tree_free(root);
            pf_fail(error, capacity, "allocation failed"); return -1;
        }
        memcpy(grown[n], text, length); grown[n][length] = 0;
        out = grown; n++;
    }
    sh_decl_tree_free(root);
    *layers = out; *count = n;
    return 1;
}

int sh_package_fsm_config_mask(sh_decl_source first, const char *config, uint32_t *mask,
    char *error, size_t capacity)
{
    sh_decl_node *root = sh_decl_tree_parse_ordered(first, error, capacity);
    const sh_decl_node *edit, *layers, *configs, *entry;
    const char *names[32];
    size_t lengths[32], count = 0;
    int result = -1;
    *mask = 0;
    if (!root) return -1;
    edit = sh_decl_tree_member(root, "edit");
    layers = edit ? sh_decl_tree_member(edit, "layers") : NULL;
    configs = pf_configs(root);
    if (!layers || !layers->compound || layers->reset || !configs) {
        pf_fail(error, capacity, "no layers or layer configurations"); goto done;
    }
    /* Bit i is the i-th listed layer (the native graph reader's order). */
    for (const sh_decl_node *layer = layers->children; layer; layer = layer->next) {
        if (count == 32 || !layer->key || strcmp(layer->key, "layer") ||
            !sh_decl_tree_literal(layer, &names[count], &lengths[count])) {
            pf_fail(error, capacity, "malformed layer list"); goto done;
        }
        count++;
    }
    entry = pf_child(configs, config);
    if (!entry) { result = 0; goto done; }
    if (!entry->compound || entry->reset) { pf_fail(error, capacity, "malformed configuration"); goto done; }
    for (const sh_decl_node *layer = entry->children; layer; layer = layer->next) {
        const char *text; size_t length, i;
        if (!layer->key || strcmp(layer->key, "layer") || !sh_decl_tree_literal(layer, &text, &length)) {
            pf_fail(error, capacity, "malformed configuration layer"); goto done;
        }
        for (i = 0; i < count && (lengths[i] != length || memcmp(names[i], text, length)); i++) {}
        if (i == count) { pf_fail(error, capacity, "configuration names an unlisted layer"); goto done; }
        *mask |= UINT32_C(1) << i;
    }
    result = 1;
done:
    sh_decl_tree_free(root);
    if (result != 1) *mask = 0;
    return result;
}

int sh_package_fsm_graph_mask(sh_decl_source declaration, uint32_t *mask, char *error, size_t capacity)
{
    sh_decl_graph *graph = sh_decl_graph_open(declaration, error, capacity);
    size_t graphs = 0;
    *mask = 0;
    if (!graph) return -1;
    for (size_t i = 0; i < sh_decl_graph_count(graph); i++) {
        const sh_decl_graph_record *record = sh_decl_graph_at(graph, i);
        if (record->kind == SH_DECL_GRAPH_SUBGRAPH) { *mask |= record->layers; graphs++; }
    }
    sh_decl_graph_close(graph);
    return graphs ? 1 : 0;
}

static int pf_identifier(const char *name)
{
    if (!name || !((*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') || *name == '_')) return 0;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
    return 1;
}

static sh_decl_node *pf_clone(const sh_decl_node *source, const char *key)
{
    sh_decl_node view = *source, *out;
    size_t length;
    char *text;
    view.key = NULL; view.next = NULL; view.annotation = NULL;
    text = sh_decl_tree_write(&view, &length);
    out = text ? sh_decl_tree_parse_ordered((sh_decl_source){text, length}, NULL, 0) : NULL;
    free(text);
    if (!out) return NULL;
    free(out->key);
    if (!(out->key = _strdup(key))) { sh_decl_tree_free(out); return NULL; }
    out->assignment = source->assignment;
    return out;
}

char *sh_package_fsm_first(sh_decl_source first, const sh_package_fsm_config *configs,
    size_t count, size_t *length, char *error, size_t capacity)
{
    sh_decl_node *root = sh_decl_tree_parse_ordered(first, error, capacity), *list, **tail;
    char *out = NULL;
    *length = 0;
    if (!root) return NULL;
    list = (sh_decl_node *)pf_configs(root);
    if (!list) { pf_fail(error, capacity, "no layer configurations"); goto done; }
    for (size_t i = 0; i < count; i++) {
        const sh_decl_node *base = pf_child(list, configs[i].base ? configs[i].base : "");
        sh_decl_node *copy;
        if (!pf_identifier(configs[i].name)) { pf_fail(error, capacity, "private configuration is not a plain name"); goto done; }
        if (pf_child(list, configs[i].name)) { pf_fail(error, capacity, "private configuration already exists"); goto done; }
        if (!base || !base->compound || base->reset) { pf_fail(error, capacity, "base configuration is missing"); goto done; }
        if (!(copy = pf_clone(base, configs[i].name))) { pf_fail(error, capacity, "configuration copy failed"); goto done; }
        for (tail = &list->children; *tail; tail = &(*tail)->next) {}
        *tail = copy;
    }
    out = sh_decl_tree_write(root, length);
    if (!out) pf_fail(error, capacity, "allocation failed");
done:
    sh_decl_tree_free(root);
    return out;
}

char *sh_package_fsm_placeholder(sh_decl_source stock, size_t *length, char *error, size_t capacity)
{
    sh_decl_node *root = sh_decl_tree_parse_ordered(stock, error, capacity);
    const sh_decl_node *edit, *graphs;
    sh_decl_graph *check = NULL;
    size_t found = 0;
    char *out = NULL;
    *length = 0;
    if (!root) return NULL;
    edit = sh_decl_tree_member(root, "edit");
    graphs = edit ? sh_decl_tree_member(edit, "subGraphs") : NULL;
    for (sh_decl_node *graph = graphs ? graphs->children : NULL; graph; graph = graph->next) {
        sh_decl_node *layers;
        if (!graph->key || strcmp(graph->key, "subGraph")) continue;
        found++;
        layers = (sh_decl_node *)sh_decl_tree_member(graph, "layers");
        if (layers) { sh_decl_tree_free(layers->children); layers->children = NULL; }
    }
    if (!found) { pf_fail(error, capacity, "source has no graphs"); goto done; }
    out = sh_decl_tree_write(root, length);
    if (!out) { pf_fail(error, capacity, "allocation failed"); goto done; }
    /* The placeholder must be a genuine graph declaration whose graphs select nothing. */
    check = sh_decl_graph_open((sh_decl_source){out, *length}, error, capacity);
    for (size_t i = 0; check && i < sh_decl_graph_count(check); i++) {
        const sh_decl_graph_record *record = sh_decl_graph_at(check, i);
        if (record->kind == SH_DECL_GRAPH_SUBGRAPH && record->layers) { sh_decl_graph_close(check); check = NULL; }
    }
    if (!check) { free(out); out = NULL; *length = 0; pf_fail(error, capacity, "placeholder is not a non-selecting graph"); }
done:
    sh_decl_graph_close(check);
    sh_decl_tree_free(root);
    return out;
}

static int pf_order(const void *a, const void *b)
{ return strcmp(**(const char *const *const *)a, **(const char *const *const *)b); }
static int pf_order_names(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int pf_hash(BCRYPT_HASH_HANDLE hash, const char *text)
{ return BCryptHashData(hash, (PUCHAR)text, (ULONG)strlen(text) + 1, 0) >= 0; }

int sh_package_fsm_config_name(const char *base, const char *const *from, const char *const *to,
    size_t count, const char *const *extra, size_t extra_count, char out[40])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    const char *const **pairs = NULL, **extras = NULL;
    unsigned char digest[32];
    int ok = 0;
    out[0] = 0;
    if (!base || (count && (!from || !to)) || (extra_count && !extra)) return 0;
    pairs = count ? calloc(count, sizeof(*pairs)) : NULL;
    extras = extra_count ? calloc(extra_count, sizeof(*extras)) : NULL;
    if ((count && !pairs) || (extra_count && !extras)) goto done;
    /* Sort replacement pairs by their replaced name, additions by name. */
    for (size_t i = 0; i < count; i++) { if (!from[i] || !to[i]) goto done; pairs[i] = &from[i]; }
    for (size_t i = 0; i < extra_count; i++) extras[i] = extra[i];
    if (count) qsort(pairs, count, sizeof(*pairs), pf_order);
    for (size_t i = 0; i < extra_count; i++) if (!extras[i]) goto done;
    if (extra_count) qsort(extras, extra_count, sizeof(*extras), pf_order_names);
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    ok = pf_hash(hash, "fsmweb-config") && pf_hash(hash, base);
    for (size_t i = 0; ok && i < count; i++) {
        size_t k = (size_t)(pairs[i] - from);
        ok = from[k] && to[k] && pf_hash(hash, from[k]) && pf_hash(hash, to[k]);
    }
    ok = ok && pf_hash(hash, "+");
    for (size_t i = 0; ok && i < extra_count; i++) ok = extras[i] && pf_hash(hash, extras[i]);
    if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
        static const char hex[] = "0123456789abcdef";
        memcpy(out, "smp_", 4);
        for (int i = 0; i < 16; i++) { out[4 + 2 * i] = hex[digest[i] >> 4]; out[5 + 2 * i] = hex[digest[i] & 15]; }
        out[36] = 0;
    } else ok = 0;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    free((void *)pairs); free((void *)extras);
    return ok;
}

/* ---- Selection ------------------------------------------------------------ */

int sh_package_fsm_behavior_config(sh_decl_source behavior, const sh_decl_entity_class_source *parents,
    char **config, char *error, size_t capacity)
{
    sh_decl_node *tree = sh_decl_tree_parse(behavior, error, capacity), *state = NULL;
    const sh_decl_node *edit;
    const char *value = NULL;
    size_t length = 0;
    int found;
    *config = NULL;
    if (!tree) return -1;
    state = sh_decl_entity_expanded_state(tree, parents, error, capacity);
    sh_decl_tree_free(tree);
    if (!state) return -1;
    edit = sh_decl_tree_member(state, "edit");
    found = edit && sh_decl_tree_literal(sh_decl_tree_member(edit, "aiFSMManagerDeclLayersConfig"), &value, &length) &&
        length;
    if (found && (*config = malloc(length + 1))) { memcpy(*config, value, length); (*config)[length] = 0; }
    sh_decl_tree_free(state);
    if (found && !*config) { pf_fail(error, capacity, "allocation failed"); return -1; }
    return found;
}

int sh_package_fsm_select(const char *config, const sh_package_fsm_layers *layers,
    const char *const *names, size_t count, int (*visit)(void *context, const char *name), void *context)
{
    uint32_t selected = 0, mask = 0;
    int status = layers->exists(layers->context, SH_PACKAGE_FSM_FIRST);
    if (status <= 0) return !status;
    /* The configuration source, whatever its own graphs contribute. */
    if (!visit(context, SH_PACKAGE_FSM_FIRST)) return 0;
    status = layers->mask(layers->context, SH_PACKAGE_FSM_FIRST, config, &selected);
    if (status <= 0) return !status;
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(names[i], SH_PACKAGE_FSM_FIRST)) continue;
        status = layers->mask(layers->context, names[i], NULL, &mask);
        if (status < 0) return 0;
        if (status && (mask & selected) && !visit(context, names[i])) return 0;
    }
    return 1;
}

/* ---- Webs ------------------------------------------------------------------ */

static void pf_strings_free(char **items, size_t count)
{
    for (size_t i = 0; items && i < count; i++) free(items[i]);
    free(items);
}
/* Append a copy unless present (sorted later). 1, or 0 on allocation failure. */
static int pf_strings_add(char ***items, size_t *count, const char *value)
{
    char **grown, *copy;
    for (size_t i = 0; i < *count; i++) if (!strcmp((*items)[i], value)) return 1;
    if (!(copy = _strdup(value))) return 0;
    if (!(grown = realloc(*items, (*count + 1) * sizeof(*grown)))) { free(copy); return 0; }
    *items = grown; grown[(*count)++] = copy; return 1;
}
static void pf_strings_sort(char **items, size_t count)
{ if (count > 1) qsort(items, count, sizeof(*items), pf_order_names); }
static void pf_web_free(sh_package_fsm_web *web)
{
    free(web->config);
    pf_strings_free(web->behaviors, web->behavior_count);
    pf_strings_free(web->from, web->count); pf_strings_free(web->to, web->count);
    pf_strings_free(web->extra, web->extra_count);
    memset(web, 0, sizeof(*web));
}
void sh_package_fsm_webs_free(sh_package_fsm_webs *webs)
{
    if (!webs) return;
    for (size_t i = 0; i < webs->count; i++) pf_web_free(webs->webs + i);
    free(webs->webs);
    pf_strings_free(webs->owned, webs->owned_count);
    for (size_t i = 0; i < webs->generated_count; i++) { free(webs->generated[i].name); free(webs->generated[i].text); }
    free(webs->generated); free(webs);
}
static sh_package_fsm_web *pf_web_find(sh_package_fsm_webs *webs, const char *config)
{
    for (size_t i = 0; i < webs->count; i++) if (!strcmp(webs->webs[i].config, config)) return webs->webs + i;
    return NULL;
}
static const sh_package_fsm_web *pf_web_binding(const sh_package_fsm_webs *webs, const char *behavior)
{
    for (size_t i = 0; i < webs->count; i++)
        for (size_t k = 0; k < webs->webs[i].behavior_count; k++)
            if (!strcmp(webs->webs[i].behaviors[k], behavior)) return webs->webs + i;
    return NULL;
}
static sh_package_fsm_web *pf_web_add(sh_package_fsm_webs *webs)
{
    sh_package_fsm_web *grown = realloc(webs->webs, (webs->count + 1) * sizeof(*grown));
    if (!grown) return NULL;
    webs->webs = grown; memset(grown + webs->count, 0, sizeof(*grown));
    return grown + webs->count++;
}
/* Parallel replacement lists grow together; from and to stay equal in count. */
static int pf_web_replace(sh_package_fsm_web *web, const char *from, const char *to)
{
    char **froms = realloc(web->from, (web->count + 1) * sizeof(*froms));
    if (froms) web->from = froms;
    char **tos = froms ? realloc(web->to, (web->count + 1) * sizeof(*tos)) : NULL;
    if (tos) web->to = tos;
    char *a = tos ? _strdup(from) : NULL, *b = a ? _strdup(to) : NULL;
    if (!b) { free(a); return 0; }
    web->from[web->count] = a; web->to[web->count++] = b; return 1;
}
static int pf_web_copy(sh_package_fsm_web *out, const sh_package_fsm_web *in)
{
    if (!(out->config = _strdup(in->config))) return 0;
    for (size_t i = 0; i < in->count; i++) if (!pf_web_replace(out, in->from[i], in->to[i])) return 0;
    for (size_t i = 0; i < in->extra_count; i++) if (!pf_strings_add(&out->extra, &out->extra_count, in->extra[i])) return 0;
    return 1;
}
/* Takes text. 1 added or equal, 0 allocation failure, -1 different bytes. */
static int pf_generated_add(sh_package_fsm_webs *webs, const char *name, char *text, size_t length)
{
    for (size_t i = 0; i < webs->generated_count; i++) if (!strcmp(webs->generated[i].name, name)) {
        int same = webs->generated[i].length == length && !memcmp(webs->generated[i].text, text, length);
        free(text); return same ? 1 : -1;
    }
    sh_package_fsm_declaration *grown = realloc(webs->generated, (webs->generated_count + 1) * sizeof(*grown));
    char *copy = _strdup(name);
    if (grown) webs->generated = grown;
    if (!grown || !copy) { free(copy); free(text); return 0; }
    webs->generated[webs->generated_count++] = (sh_package_fsm_declaration){copy, text, length};
    return 1;
}
static int pf_web_order(const void *a, const void *b)
{ return strcmp(((const sh_package_fsm_web *)a)->config, ((const sh_package_fsm_web *)b)->config); }
static int pf_generated_order(const void *a, const void *b)
{ return strcmp(((const sh_package_fsm_declaration *)a)->name, ((const sh_package_fsm_declaration *)b)->name); }
static void pf_webs_sort(sh_package_fsm_webs *webs)
{
    for (size_t i = 0; i < webs->count; i++) pf_strings_sort(webs->webs[i].behaviors, webs->webs[i].behavior_count);
    if (webs->count > 1) qsort(webs->webs, webs->count, sizeof(*webs->webs), pf_web_order);
    pf_strings_sort(webs->owned, webs->owned_count);
    if (webs->generated_count > 1)
        qsort(webs->generated, webs->generated_count, sizeof(*webs->generated), pf_generated_order);
}

sh_package_fsm_webs *sh_package_fsm_webs_build(const sh_package_fsm_bound *bound, size_t count,
    const char *const *owned, size_t owned_count, char *error, size_t capacity)
{
    sh_package_fsm_webs *webs = calloc(1, sizeof(*webs));
    char config[40], first[80];
    if (!webs) { pf_fail(error, capacity, "allocation failed"); return NULL; }
    for (size_t i = 0; i < count; i++) {
        const sh_package_fsm_bound *b = bound + i;
        sh_package_fsm_web *web;
        uint32_t mask = 0;
        char reason[256] = "";
        if (!b->behavior || !b->config || !b->first.text || (b->count && (!b->from || !b->to)) ||
            (b->extra_count && !b->extra)) { pf_fail(error, capacity, "incomplete state-machine binding"); goto failed; }
        if (sh_package_fsm_config_mask(b->first, b->config, &mask, reason, sizeof(reason)) != 1) {
            if (error && capacity) snprintf(error, capacity, "behavior %s names layer configuration %s, which its %s does not define%s%s",
                b->behavior, b->config, SH_PACKAGE_FSM_FIRST, *reason ? ": " : "", reason);
            goto failed;
        }
        if (!sh_package_fsm_config_name(b->config, b->from, b->to, b->count, b->extra, b->extra_count, config)) {
            pf_fail(error, capacity, "cannot name a private layer configuration"); goto failed;
        }
        web = pf_web_find(webs, config);
        if (pf_web_binding(webs, b->behavior) && pf_web_binding(webs, b->behavior) != web) {
            if (error && capacity) snprintf(error, capacity, "behavior %s builds two state-machine webs", b->behavior);
            goto failed;
        }
        if (!web) {
            sh_package_fsm_config private_config = {config, b->config};
            size_t length = 0;
            char *text;
            if (!(web = pf_web_add(webs)) || !(web->config = _strdup(config))) { pf_fail(error, capacity, "allocation failed"); goto failed; }
            snprintf(first, sizeof(first), "%s%s", SH_PACKAGE_FSM_FIRST_PREFIX, config);
            /* The first declaration's replacement is the generated one; any
             * change to it is in the generated text and in the name above. */
            if (!pf_web_replace(web, SH_PACKAGE_FSM_FIRST, first)) { pf_fail(error, capacity, "allocation failed"); goto failed; }
            for (size_t k = 0; k < b->count; k++) {
                if (!strcmp(b->from[k], SH_PACKAGE_FSM_FIRST)) continue;
                for (size_t j = 0; j < web->count; j++)
                    if (!strcmp(web->from[j], b->from[k])) { pf_fail(error, capacity, "a state-machine declaration is replaced twice"); goto failed; }
                if (!pf_web_replace(web, b->from[k], b->to[k])) { pf_fail(error, capacity, "allocation failed"); goto failed; }
            }
            for (size_t k = 0; k < b->extra_count; k++)
                if (!pf_strings_add(&web->extra, &web->extra_count, b->extra[k])) { pf_fail(error, capacity, "allocation failed"); goto failed; }
            text = sh_package_fsm_first(b->first, &private_config, 1, &length, reason, sizeof(reason));
            if (!text) {
                if (error && capacity) snprintf(error, capacity, "cannot generate %s for %s: %s", first, b->behavior, reason);
                goto failed;
            }
            if (pf_generated_add(webs, first, text, length) != 1) { pf_fail(error, capacity, "conflicting generated state-machine declaration"); goto failed; }
            if (!pf_strings_add(&webs->owned, &webs->owned_count, first)) { pf_fail(error, capacity, "allocation failed"); goto failed; }
        }
        if (!pf_strings_add(&web->behaviors, &web->behavior_count, b->behavior)) { pf_fail(error, capacity, "allocation failed"); goto failed; }
    }
    for (size_t i = 0; i < owned_count; i++)
        if (!owned[i] || !pf_strings_add(&webs->owned, &webs->owned_count, owned[i])) { pf_fail(error, capacity, "allocation failed"); goto failed; }
    if (webs->owned_count && !pf_strings_add(&webs->owned, &webs->owned_count, SH_PACKAGE_FSM_PLACEHOLDER)) {
        pf_fail(error, capacity, "allocation failed"); goto failed;
    }
    pf_webs_sort(webs);
    return webs;
failed:
    sh_package_fsm_webs_free(webs);
    return NULL;
}

sh_package_fsm_webs *sh_package_fsm_webs_filter(const sh_package_fsm_webs *webs,
    int (*keep)(void *context, const char *behavior), void *context, char *error, size_t capacity)
{
    sh_package_fsm_webs *out = sh_package_fsm_webs_join(NULL, webs, error, capacity);
    if (!out) return NULL;
    for (size_t i = 0; i < out->count;) {
        sh_package_fsm_web *web = out->webs + i;
        size_t kept = 0;
        for (size_t k = 0; k < web->behavior_count; k++) {
            if (keep(context, web->behaviors[k])) web->behaviors[kept++] = web->behaviors[k];
            else free(web->behaviors[k]);
        }
        web->behavior_count = kept;
        if (kept) { i++; continue; }
        pf_web_free(web);
        memmove(web, web + 1, (out->count - i - 1) * sizeof(*web));
        out->count--;
    }
    return out;
}

sh_package_fsm_webs *sh_package_fsm_webs_join(const sh_package_fsm_webs *map, const sh_package_fsm_webs *local,
    char *error, size_t capacity)
{
    sh_package_fsm_webs *webs = calloc(1, sizeof(*webs));
    const sh_package_fsm_webs *sides[2] = {map, local};
    if (!webs) { pf_fail(error, capacity, "allocation failed"); return NULL; }
    for (int side = 0; side < 2; side++) {
        const sh_package_fsm_webs *in = sides[side];
        for (size_t i = 0; in && i < in->count; i++) {
            const sh_package_fsm_web *web = in->webs + i;
            sh_package_fsm_web *out = pf_web_find(webs, web->config);
            for (size_t k = 0; k < web->behavior_count; k++) {
                /* A behavior the map already binds keeps the map's web. */
                const sh_package_fsm_web *bound = pf_web_binding(webs, web->behaviors[k]);
                if (bound && strcmp(bound->config, web->config)) continue;
                if (!out && (!(out = pf_web_add(webs)) || !pf_web_copy(out, web))) { pf_fail(error, capacity, "allocation failed"); goto failed; }
                if (!pf_strings_add(&out->behaviors, &out->behavior_count, web->behaviors[k])) { pf_fail(error, capacity, "allocation failed"); goto failed; }
            }
        }
        for (size_t i = 0; in && i < in->owned_count; i++)
            if (!pf_strings_add(&webs->owned, &webs->owned_count, in->owned[i])) { pf_fail(error, capacity, "allocation failed"); goto failed; }
        for (size_t i = 0; in && i < in->generated_count; i++) {
            char *text = malloc(in->generated[i].length + 1);
            if (!text) { pf_fail(error, capacity, "allocation failed"); goto failed; }
            memcpy(text, in->generated[i].text, in->generated[i].length); text[in->generated[i].length] = 0;
            int added = pf_generated_add(webs, in->generated[i].name, text, in->generated[i].length);
            if (added != 1) { pf_fail(error, capacity, added ? "conflicting generated state-machine declaration" : "allocation failed"); goto failed; }
        }
    }
    pf_webs_sort(webs);
    return webs;
failed:
    sh_package_fsm_webs_free(webs);
    return NULL;
}
