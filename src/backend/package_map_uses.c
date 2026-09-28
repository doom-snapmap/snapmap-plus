#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_map_uses.h"
#include "package_keys.h"

struct sh_package_map_uses {
    sh_package_provenance saved, selected;
    const sh_package_provenance *current;
    const sh_package_bundle_binding *bundles;
    size_t bundle_count;
    sh_package_map_use_required required;
    void *required_context;
    sh_package_map_use_retained retained;
    void *retained_context;
    sh_package_references references;
    char failure[1024];
    int finished;
};

static int mu_fail(sh_package_map_uses *selection, const char *reason)
{
    if (selection && !selection->failure[0])
        snprintf(selection->failure, sizeof(selection->failure), "%s", reason);
    return -1;
}
static void mu_error(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); }
static int mu_context_order(const sh_package_provenance_context *a,
    const sh_package_provenance_context *b)
{
    int order;
    if (a->mode != b->mode) return a->mode < b->mode ? -1 : 1;
    if (a->tree_count != b->tree_count) return a->tree_count < b->tree_count ? -1 : 1;
    order = memcmp(a->trees, b->trees, a->tree_count * 32);
    return order ? order : strcmp(a->component ? a->component : "", b->component ? b->component : "");
}
static size_t mu_binding(const sh_package_provenance *value, const char *family, const char *name)
{
    for (size_t i = 0; i < value->binding_count; i++)
        if (!_stricmp(value->bindings[i].family, family) && !_stricmp(value->bindings[i].name, name)) return i;
    return SIZE_MAX;
}
typedef struct mu_local {
    const sh_package_provenance *library;
    int (*native)(void *, const char *, const char *);
    void *context;
} mu_local;
static int mu_local_resolve(void *context, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement)
{
    mu_local *local = context;
    const sh_package_provenance *library = local->library;
    size_t chosen = SIZE_MAX, chosen_context = SIZE_MAX;
    (void)site; *replacement = NULL;
    if (!strcmp(family, SH_PACKAGE_KEY_FAMILY) || mu_binding(library, family, name) != SIZE_MAX) return 0;
    for (size_t i = 0; i < library->binding_count; i++) {
        const sh_package_provenance_binding *binding = library->bindings + i;
        if (_stricmp(binding->family, family) || _stricmp(binding->original, name)) continue;
        for (size_t c = 0; c < binding->context_count; c++) {
            size_t index = binding->contexts[c];
            if (index >= library->context_count) return -1;
            int order = chosen == SIZE_MAX ? -1 : mu_context_order(library->contexts + index,
                library->contexts + chosen_context);
            if (order < 0 || (!order && strcmp(binding->name, library->bindings[chosen].name) < 0)) {
                chosen = i; chosen_context = index;
            }
        }
    }
    if (chosen == SIZE_MAX) return 0;
    int native = local->native(local->context, family, name);
    if (native) return native < 0 ? -1 : 0;
    *replacement = library->bindings[chosen].name; return 1;
}
char *sh_package_map_uses_bind_local(sh_package_source_reader *reader,
    const char *json, size_t length, const sh_package_provenance *library,
    int (*native)(void *, const char *, const char *), void *context,
    size_t *output_length, char *error, size_t capacity)
{
    mu_local local = {library, native, context};
    if (output_length) *output_length = 0;
    if (!reader || !library || library->semantics != SH_PACKAGE_PROVENANCE_CATALOG || !native) {
        mu_error(error, capacity, "local legacy binding requires an installed source catalog"); return NULL;
    }
    return sh_package_source_bind_map_sites_observed(reader, json, length,
        mu_local_resolve, mu_local_resolve, &local, output_length, NULL, error, capacity);
}
static int mu_same_site(const sh_package_provenance_use *use, const sh_package_provenance_site *site)
{
    return site && use->kind == site->kind && !strcmp(use->owner, site->owner) &&
        !strcmp(use->reference, site->reference);
}
static int mu_has_sources(sh_package_map_uses *selection, const sh_package_provenance_context *context)
{
    for (size_t t = 0; t < context->tree_count; t++) {
        size_t i;
        for (i = 0; i < selection->bundle_count; i++) {
            const unsigned char *tree = sh_package_bundle_fingerprint(selection->bundles[i].original);
            if (tree && !memcmp(context->trees[t], tree, 32)) break;
        }
        if (i == selection->bundle_count)
            return mu_fail(selection, "a selected map source has no exact retained original delivery");
    }
    return 1;
}

/* Old carriers could name the derived migration tree. Only historical uses
 * admit that compatibility route, and all exact original rows participate. */
static size_t mu_original_context(sh_package_map_uses *selection,
    const sh_package_provenance_context *context, int historical, sh_package_provenance *out)
{
    unsigned char (*trees)[32] = NULL;
    size_t count = 0, result = SIZE_MAX;
    for (size_t t = 0; t < context->tree_count; t++) {
        int exact = 0, found = 0;
        for (size_t i = 0; i < selection->bundle_count; i++) {
            const unsigned char *tree = sh_package_bundle_fingerprint(selection->bundles[i].original);
            if (tree && !memcmp(tree, context->trees[t], 32)) { exact = 1; break; }
        }
        for (size_t i = 0; i < selection->bundle_count; i++) {
            const unsigned char *tree = sh_package_bundle_fingerprint(selection->bundles[i].original);
            if (!tree || (exact ? memcmp(tree, context->trees[t], 32) :
                !historical || memcmp(selection->bundles[i].compiler_tree, context->trees[t], 32))) continue;
            size_t previous;
            for (previous = 0; previous < count; previous++) if (!memcmp(trees[previous], tree, 32)) break;
            found = 1;
            if (previous < count) continue;
            unsigned char (*grown)[32];
            if (count >= SIZE_MAX / 32 - 1 || !(grown = realloc(trees, (count + 1) * 32))) goto done;
            trees = grown; memcpy(trees[count++], tree, 32);
        }
        if (!found) goto done;
    }
    /* Isolated historical expansion is handled as separate contexts below. */
    result = sh_package_provenance_context_add(out, context->mode,
        (const unsigned char (*)[32])trees, count, context->component);
done:
    free(trees); return result;
}

static int mu_index_add(size_t **indices, size_t *count, size_t index)
{
    size_t *grown;
    if (index == SIZE_MAX) return 0;
    for (size_t i = 0; i < *count; i++) if ((*indices)[i] == index) return 1;
    if (*count >= SIZE_MAX / sizeof(*grown) - 1 || !(grown = realloc(*indices, (*count + 1) * sizeof(*grown)))) return 0;
    *indices = grown; (*indices)[(*count)++] = index; return 1;
}
static int mu_original_contexts(sh_package_map_uses *selection,
    const sh_package_provenance_context *context, int historical, sh_package_provenance *out,
    size_t **indices, size_t *count)
{
    size_t index = mu_original_context(selection, context, historical, out);
    if (index != SIZE_MAX) return mu_index_add(indices, count, index);
    if (!historical || context->mode != SH_PACKAGE_CONTEXT_ISOLATED || context->tree_count != 1) return 0;
    /* Original identity always wins. Do not reinterpret it as a migration key
     * just because some unrelated normalized tree has the same bytes. */
    for (size_t i = 0; i < selection->bundle_count; i++) {
        const unsigned char *tree = sh_package_bundle_fingerprint(selection->bundles[i].original);
        if (tree && !memcmp(tree, context->trees[0], 32)) return 0;
    }
    int found = 0;
    for (size_t i = 0; i < selection->bundle_count; i++) {
        const unsigned char *tree = sh_package_bundle_fingerprint(selection->bundles[i].original);
        if (!tree || memcmp(selection->bundles[i].compiler_tree, context->trees[0], 32)) continue;
        index = sh_package_provenance_context_add(out, SH_PACKAGE_CONTEXT_ISOLATED,
            (const unsigned char (*)[32])tree, 1, context->component);
        if (!mu_index_add(indices, count, index)) return 0;
        found = 1;
    }
    return found;
}

static int mu_historical_resolve(sh_package_map_uses *selection, size_t use, const char **resolved)
{
    const sh_package_provenance_use *entry = selection->saved.uses + use;
    const sh_package_provenance_binding *source = selection->saved.bindings + entry->binding;
    const char *target = NULL;
    sh_package_provenance contexts = {0};
    int ok = -1;
    for (size_t c = 0; c < entry->context_count; c++) {
        const sh_package_provenance_context *required = selection->saved.contexts + entry->contexts[c];
        size_t *required_indices = NULL, required_count = 0;
        if (!mu_original_contexts(selection, required, 1, &contexts, &required_indices, &required_count)) {
            free(required_indices); goto done;
        }
        int all = 1;
        for (size_t r = 0; r < required_count && all; r++) {
            int found = 0;
            for (size_t b = 0; b < selection->current->binding_count; b++) {
                const sh_package_provenance_binding *binding = selection->current->bindings + b;
                if (strcmp(source->family, binding->family) || strcmp(source->original, binding->original)) continue;
                for (size_t k = 0; k < binding->context_count; k++) {
                    if (binding->contexts[k] >= selection->current->context_count) { all = 0; break; }
                    const sh_package_provenance_context *candidate = selection->current->contexts + binding->contexts[k];
                    size_t *candidate_indices = NULL, candidate_count = 0;
                    int same = 0;
                    if (mu_original_contexts(selection, candidate, 1, &contexts, &candidate_indices, &candidate_count))
                        for (size_t n = 0; n < candidate_count; n++) if (candidate_indices[n] == required_indices[r]) same = 1;
                    free(candidate_indices);
                    if (!same) continue;
                    if (target && strcmp(target, binding->name)) { all = 0; break; }
                    target = binding->name; found = 1;
                }
                if (!all) break;
            }
            if (!found) all = 0;
        }
        free(required_indices); if (!all) goto done;
    }
    if (target) { *resolved = target; ok = 1; }
done:
    sh_package_provenance_free(&contexts); return ok;
}

sh_package_map_uses *sh_package_map_uses_open(const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_bundle_binding *bundles,
    size_t bundle_count, sh_package_map_use_required required, void *required_context,
    char *error, size_t capacity)
{
    sh_package_map_uses *out = NULL;
    if (error && capacity) *error = 0;
    if (!current || !required || (bundle_count && !bundles) || (saved &&
        saved->semantics == SH_PACKAGE_PROVENANCE_CATALOG && (saved->binding_count || saved->use_count))) {
        mu_error(error, capacity, "map use selection requires an explicit saved document and current catalog"); return NULL;
    }
    out = calloc(1, sizeof(*out));
    if (!out) { mu_error(error, capacity, "cannot allocate map occurrence selection"); return NULL; }
    out->current = current; out->bundles = bundles; out->bundle_count = bundle_count;
    out->required = required; out->required_context = required_context;
    out->selected.semantics = out->saved.semantics = SH_PACKAGE_PROVENANCE_USES;
    if (saved && saved->semantics != SH_PACKAGE_PROVENANCE_CATALOG &&
        !sh_package_provenance_select_uses(saved, NULL, NULL, &out->saved)) {
        mu_error(error, capacity, "saved map occurrences are invalid"); sh_package_map_uses_close(out); return NULL;
    }
    return out;
}
void sh_package_map_uses_close(sh_package_map_uses *selection)
{
    if (!selection) return;
    sh_package_provenance_free(&selection->saved); sh_package_provenance_free(&selection->selected);
    sh_package_references_free(&selection->references); free(selection);
}

/* Match saved spelling or its already-rebound current spelling. A changed
 * value is fresh selection; an unchanged value with a missing source fails. */
static int mu_saved_match(sh_package_map_uses *selection, size_t use,
    const char *family, const char *name, const char **resolved)
{
    const sh_package_provenance_use *entry = selection->saved.uses + use;
    const sh_package_provenance_binding *binding = selection->saved.bindings + entry->binding;
    if (_stricmp(binding->family, family)) return 0;
    int same = !_stricmp(binding->name, name);
    /* A key names no native object a capture could retain: the serving
     * catalog alone rebinds it, as it does in a portable document. */
    if (same && selection->retained && strcmp(binding->family, SH_PACKAGE_KEY_FAMILY)) {
        int kept = selection->retained(selection->retained_context, &selection->saved, use, resolved);
        if (kept < 0) return mu_fail(selection, "retained native map use could not be verified");
        if (kept) return *resolved && !_stricmp(*resolved, binding->name) ? 1 :
            mu_fail(selection, "retained native map use changed its saved identity");
    }
    int found = entry->kind == SH_PACKAGE_USE_HISTORICAL ? mu_historical_resolve(selection, use, resolved) :
        sh_package_provenance_resolve_use(&selection->saved, use, selection->current, resolved);
    if (found != 1 && same) {
        char detail[768], reason[1024];
        sh_package_provenance_describe_use(&selection->saved, use, selection->current, detail, sizeof(detail));
        snprintf(reason, sizeof(reason), "an unchanged map use cannot resolve its saved source context (%s)", detail);
        return mu_fail(selection, reason);
    }
    if (found != 1) return 0;
    return same || !_stricmp(*resolved, name);
}
static int mu_append(sh_package_map_uses *selection, const sh_package_provenance *source,
    size_t binding_index, const size_t *contexts, size_t count,
    const sh_package_provenance_site *site, const char *name, int historical)
{
    const sh_package_provenance_binding *binding = source->bindings + binding_index;
    size_t *mapped = NULL, mapped_count = 0, target;
    int ok = -1;
    if (!count)
        return mu_fail(selection, "cannot allocate selected map contexts");
    for (size_t i = 0; i < count; i++) {
        if (contexts[i] >= source->context_count) goto done;
        const sh_package_provenance_context *context = source->contexts + contexts[i];
        if (!historical && mu_has_sources(selection, context) < 0) goto done;
        if (!mu_original_contexts(selection, context, historical, &selection->selected, &mapped, &mapped_count)) goto done;
    }
    if (!sh_package_provenance_bind(&selection->selected, binding->family, binding->original, name, mapped, mapped_count)) goto done;
    target = mu_binding(&selection->selected, binding->family, name);
    for (size_t i = 0; i < selection->selected.use_count; i++) {
        const sh_package_provenance_use *previous = selection->selected.uses + i;
        if (!mu_same_site(previous, site)) continue;
        if (previous->binding != target || previous->context_count != mapped_count) goto done;
        for (size_t c = 0; c < mapped_count; c++) {
            size_t p;
            for (p = 0; p < previous->context_count; p++) if (previous->contexts[p] == mapped[c]) break;
            if (p == previous->context_count) goto done;
        }
    }
    if (target == SIZE_MAX || !sh_package_provenance_use_set(&selection->selected, site, target, mapped, mapped_count)) goto done;
    ok = 1;
done:
    free(mapped); return ok < 0 ? mu_fail(selection, "cannot retain the selected typed map use") : ok;
}

/* A key bit's site is "<value site>#<bit>". Once rebound, its occurrence is
 * recorded under the bit the value now holds, so a later edit of that value
 * never meets the stale spelling of another package's key. */
static int mu_key_record_site(const sh_package_provenance_site *site, const char *resolved,
    sh_package_provenance_site *out, char **owned)
{
    const char *hash = site && site->reference ? strrchr(site->reference, '#') : NULL;
    size_t prefix, length;
    *owned = NULL;
    if (!hash || !resolved) return 0;
    prefix = (size_t)(hash - site->reference) + 1; length = strlen(resolved);
    if (!(*owned = malloc(prefix + length + 1))) return 0;
    memcpy(*owned, site->reference, prefix); memcpy(*owned + prefix, resolved, length + 1);
    *out = (sh_package_provenance_site){site->kind, site->owner, *owned};
    return 1;
}
static int mu_choose(sh_package_map_uses *selection, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement, int observe)
{
    const char *resolved = NULL;
    size_t saved_use = SIZE_MAX, current_binding, chosen = SIZE_MAX;
    int key;
    if (replacement) *replacement = NULL;
    if (!selection || selection->failure[0] || selection->finished || !family || !name || !replacement)
        return mu_fail(selection, "invalid map occurrence callback");
    if (site && (!site->owner || !site->reference)) return mu_fail(selection, "map occurrence site is incomplete");
    if (observe && (!site || (site->kind != SH_PACKAGE_USE_OBJECT && site->kind != SH_PACKAGE_USE_WORLD)))
        return mu_fail(selection, "a typed map value has no portable occurrence site");
    /* A registration key bit is no resource: it names the package whose AI it
     * registers, and binds only while some current package registers it. */
    key = !strcmp(family, SH_PACKAGE_KEY_FAMILY);

    /* An exact occurrence record, even an edited one, takes precedence over
     * historical name-level records retained for different copied objects. */
    int exact = 0;
    for (size_t i = 0; i < selection->saved.use_count; i++) {
        const sh_package_provenance_use *use = selection->saved.uses + i;
        if (use->kind == SH_PACKAGE_USE_HISTORICAL || !mu_same_site(use, site)) continue;
        exact = 1;
        int match = mu_saved_match(selection, i, family, name, &resolved);
        if (match < 0) return -1;
        if (match) saved_use = i;
        break;
    }
    if (!exact) for (size_t i = 0; i < selection->saved.use_count; i++) {
        const sh_package_provenance_use *use = selection->saved.uses + i;
        if (use->kind != SH_PACKAGE_USE_HISTORICAL) continue;
        const char *candidate = NULL;
        int match = mu_saved_match(selection, i, family, name, &candidate);
        if (match < 0) return -1;
        if (!match) continue;
        if (saved_use != SIZE_MAX) {
            /* Separate historical contributor selections cannot be guessed
             * from a name alone. The caller must filter its exact snapshot. */
            return mu_fail(selection, "historical map ownership needs the caller's exact selected contributor context");
        }
        saved_use = i; resolved = candidate;
    }
    if (saved_use != SIZE_MAX) {
        const sh_package_provenance_use *use = selection->saved.uses + saved_use;
        sh_package_provenance_site moved; char *owned = NULL;
        const sh_package_provenance_site *record = site;
        if (observe && key && resolved && strcmp(resolved, name)) {
            if (!mu_key_record_site(site, resolved, &moved, &owned)) return mu_fail(selection, "cannot record a rebound map key");
            record = &moved;
        }
        /* A saved or historical row can now resolve to the native identity it
         * names (an authored exact copy after dedupe, or an edit SnapMap never
         * runs). SnapMap then serves that value itself: unless the current
         * provider still supplies that identity, the site is vanilla and is
         * neither a use nor a delivery requirement. */
        int vanilla = 0;
        if (observe && !key && resolved &&
            !_stricmp(resolved, selection->saved.bindings[use->binding].original)) {
            int needed = selection->required(selection->required_context, family, resolved,
                selection->failure, sizeof(selection->failure));
            if (needed < 0) return mu_fail(selection, "saved map reference gameplay classification is unavailable");
            vanilla = !needed;
        }
        int appended = observe && !vanilla ? mu_append(selection, &selection->saved, use->binding,
            use->contexts, use->context_count, record, resolved, use->kind == SH_PACKAGE_USE_HISTORICAL) : 1;
        free(owned);
        if (appended < 0) return -1;
    } else {
        /* Names are current compiled bindings, not authored aliases or IDs. */
        current_binding = mu_binding(selection->current, family, name);
        int required = !observe ? 1 : key ? current_binding != SIZE_MAX :
            selection->required(selection->required_context, family, name, selection->failure, sizeof(selection->failure));
        if (required < 0) return mu_fail(selection, "new map reference gameplay classification is unavailable");
        if (observe && required && current_binding == SIZE_MAX)
            return mu_fail(selection, "a new gameplay reference has no current source catalog binding");
        if (current_binding != SIZE_MAX) {
            const sh_package_provenance_binding *binding = selection->current->bindings + current_binding;
            for (size_t i = 0; i < binding->context_count; i++) {
                size_t context = binding->contexts[i];
                if (context >= selection->current->context_count)
                    return mu_fail(selection, "current map binding catalog has an invalid context");
                if (chosen == SIZE_MAX || mu_context_order(selection->current->contexts + context,
                    selection->current->contexts + chosen) < 0) chosen = context;
            }
            if (chosen == SIZE_MAX) return mu_fail(selection, "current map binding has no supplying source context");
            resolved = binding->name;
            if (observe && required && mu_append(selection, selection->current, current_binding, &chosen, 1, site, resolved, 0) < 0) return -1;
        }
    }
    if (observe && !key && !sh_package_references_add(&selection->references, family, resolved ? resolved : name))
        return mu_fail(selection, "cannot retain typed map references");
    if (!resolved || !strcmp(resolved, name)) return 0;
    *replacement = resolved; return 1;
}
int sh_package_map_uses_saved_keys(void *selection, const sh_package_provenance_site *site,
    uint32_t *keys, size_t capacity)
{
    sh_package_map_uses *s = selection;
    size_t count = 0, prefix;
    if (!s || !site || !site->owner || !site->reference) return -1;
    prefix = strlen(site->reference);
    for (size_t i = 0; i < s->saved.use_count; i++) {
        const sh_package_provenance_use *use = s->saved.uses + i;
        char *end = NULL;
        unsigned long key;
        if (use->kind != site->kind || !use->owner || !use->reference || strcmp(use->owner, site->owner) ||
            use->binding >= s->saved.binding_count ||
            strcmp(s->saved.bindings[use->binding].family, SH_PACKAGE_KEY_FAMILY) ||
            strncmp(use->reference, site->reference, prefix) || use->reference[prefix] != '#') continue;
        key = strtoul(use->reference + prefix + 1, &end, 10);
        if (!end || *end || !key || key > 0xfffffffful) continue;
        if (count < capacity) keys[count] = (uint32_t)key;
        if (count == INT_MAX) return -1;
        count++;
    }
    return (int)count;
}
int sh_package_map_uses_lookup(void *selection, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement)
{ return mu_choose(selection, site, family, name, replacement, 0); }
int sh_package_map_uses_observe(void *selection, const sh_package_provenance_site *site,
    const char *family, const char *name, const char **replacement)
{ return mu_choose(selection, site, family, name, replacement, 1); }

int sh_package_map_uses_finish(sh_package_map_uses *selection,
    sh_package_provenance *uses, sh_package_references *references,
    char *error, size_t capacity)
{
    sh_package_provenance selected = {0};
    if (error && capacity) *error = 0;
    if (!selection || !uses || !references || selection->finished || selection->failure[0]) {
        mu_error(error, capacity, selection && selection->failure[0] ? selection->failure : "map occurrence selection is unavailable");
        return 0;
    }
    if (!sh_package_provenance_select_uses(&selection->selected, NULL, NULL, &selected)) {
        mu_fail(selection, "cannot finalize selected map uses"); mu_error(error, capacity, selection->failure); return 0;
    }
    sh_package_provenance_free(uses); sh_package_references_free(references);
    *uses = selected; *references = selection->references;
    memset(&selection->references, 0, sizeof(selection->references)); selection->finished = 1; return 1;
}
char *sh_package_map_uses_select_retained(sh_package_source_reader *reader,
    const char *json, size_t length, const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_bundle_binding *bundles,
    size_t bundle_count, sh_package_map_use_required required, void *required_context,
    sh_package_map_use_retained retained, void *retained_context,
    size_t *output_length, sh_package_provenance *uses,
    sh_package_references *references, sh_package_source_binding *inspection,
    char *error, size_t capacity)
{
    sh_package_map_uses *selection;
    sh_package_source_binding walked = {0};
    char *out; size_t n = 0;
    if (output_length) *output_length = 0;
    if (inspection) memset(inspection, 0, sizeof(*inspection));
    if (!uses || !references) { mu_error(error, capacity, "map use outputs are unavailable"); return NULL; }
    selection = sh_package_map_uses_open(saved, current, bundles, bundle_count, required, required_context, error, capacity);
    if (!selection) return NULL;
    selection->retained = retained; selection->retained_context = retained_context;
    out = sh_package_source_bind_map_sites_observed(reader, json, length,
        sh_package_map_uses_lookup, sh_package_map_uses_observe, selection, &n, &walked, error, capacity);
    selection->references.incomplete = walked.inspection.gaps != 0;
    if (inspection) *inspection = walked;
    if (!out || !sh_package_map_uses_finish(selection, uses, references, error, capacity)) {
        if (selection->failure[0]) mu_error(error, capacity, selection->failure);
        free(out); out = NULL; if (inspection) inspection->changed = 0;
    }
    if (out && output_length) *output_length = n;
    sh_package_map_uses_close(selection); return out;
}

char *sh_package_map_uses_select(sh_package_source_reader *reader,
    const char *json, size_t length, const sh_package_provenance *saved,
    const sh_package_provenance *current, const sh_package_bundle_binding *bundles,
    size_t bundle_count, sh_package_map_use_required required, void *required_context,
    size_t *output_length, sh_package_provenance *uses,
    sh_package_references *references, sh_package_source_binding *inspection,
    char *error, size_t capacity)
{
    return sh_package_map_uses_select_retained(reader, json, length, saved, current,
        bundles, bundle_count, required, required_context, NULL, NULL,
        output_length, uses, references, inspection, error, capacity);
}
