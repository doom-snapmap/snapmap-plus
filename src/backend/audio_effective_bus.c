#include "audio_effective_bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct eb_row { const sh_audio_bus_setting *setting; size_t source; } eb_row;
/* Rows of one key sorted by source; candidates are the edits in precedence
 * order followed by the native row. pick selects the current candidate. */
typedef struct eb_group { size_t first, end, pick; int native, forced_removed; } eb_group;

static int eb_fail(char *error, size_t capacity, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); return 0; }
static uint32_t eb_u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int eb_key(const sh_audio_bus_setting *a, const sh_audio_bus_setting *b)
{
    if (a->bus != b->bus) return a->bus < b->bus ? -1 : 1;
    if (a->field.field != b->field.field) return a->field.field < b->field.field ? -1 : 1;
    for (size_t i = 0; i < 2; i++) if (a->field.key[i] != b->field.key[i]) return a->field.key[i] < b->field.key[i] ? -1 : 1;
    return 0;
}
static int eb_order(const void *left, const void *right)
{
    const eb_row *a = left, *b = right; int key = eb_key(a->setting, b->setting);
    return key ? key : a->source < b->source ? -1 : a->source > b->source;
}
unsigned sh_audio_effective_bus_kind(const sh_audio_bus_policy *policy, uint32_t bus)
{
    for (size_t i = 0; policy && i < policy->bus_count; i++) if (policy->buses[i].bus == bus) return policy->buses[i].kind;
    return 0;
}
int sh_audio_effective_bus_edited(const sh_audio_bus_policy *policy)
{
    for (size_t i = 0; policy && i < policy->count; i++) if (policy->settings[i].edited) return 1;
    return 0;
}
int sh_audio_effective_bus_copy(const sh_audio_bus_policy *policy, sh_audio_bus_policy *out)
{
    sh_audio_bus_policy copy = {0};
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!policy) return 1;
    copy.settings = calloc(policy->count ? policy->count : 1, sizeof(*copy.settings));
    copy.buses = calloc(policy->bus_count ? policy->bus_count : 1, sizeof(*copy.buses));
    if (!copy.settings || !copy.buses) { sh_audio_bus_policy_free(&copy); return 0; }
    if (policy->bus_count) memcpy(copy.buses, policy->buses, policy->bus_count * sizeof(*copy.buses));
    copy.bus_count = policy->bus_count;
    for (size_t i = 0; i < policy->count; i++) {
        sh_audio_bus_setting *s = copy.settings + copy.count++;
        *s = policy->settings[i]; s->body = NULL; s->contributors = NULL; s->contributor_count = 0;
        if (policy->settings[i].body && s->field.length) {
            if (!(s->body = malloc(s->field.length))) { sh_audio_bus_policy_free(&copy); return 0; }
            memcpy(s->body, policy->settings[i].body, s->field.length);
        }
    }
    *out = copy; return 1;
}
static size_t eb_candidates(const eb_group *g) { return g->end - g->first; }
static const eb_row *eb_candidate(const eb_row *rows, const eb_group *g, size_t k)
{
    size_t edits = g->end - g->first - (g->native != 0);
    if (k < edits) return rows + g->first + (g->native != 0) + k;
    return g->native ? rows + g->first : NULL;
}
static const eb_row *eb_chosen(const eb_row *rows, const eb_group *g) { return eb_candidate(rows, g, g->pick); }
static size_t eb_find(const eb_row *rows, const eb_group *groups, size_t count, uint32_t bus,
    sh_audio_bus_field field, uint32_t key0, uint32_t key1, int match_key1)
{
    for (size_t i = 0; i < count; i++) {
        const sh_audio_bus_setting *s = rows[groups[i].first].setting;
        if (s->bus == bus && s->field.field == field && s->field.key[0] == key0 && (!match_key1 || s->field.key[1] == key1))
            return i;
    }
    return SIZE_MAX;
}
static uint32_t eb_parent(const eb_row *rows, const eb_group *g)
{
    const sh_audio_bus_setting *s = eb_chosen(rows, g)->setting;
    return s->removed || s->field.length != 4 ? 0 : eb_u32(s->body);
}
/* Advance the lowest-precedence edit on any cycle until the forest is rooted. */
static int eb_parents(const eb_row *rows, eb_group *groups, size_t count, char *error, size_t capacity)
{
    size_t parents = 0, limit = 1;
    for (size_t i = 0; i < count; i++)
        if (rows[groups[i].first].setting->field.field == SH_AUDIO_BUS_FIELD_PARENT) { parents++; limit += eb_candidates(groups + i); }
    for (size_t round = 0; round < limit; round++) {
        size_t cycle = SIZE_MAX;
        for (size_t start = 0; start < count && cycle == SIZE_MAX; start++) {
            if (rows[groups[start].first].setting->field.field != SH_AUDIO_BUS_FIELD_PARENT) continue;
            size_t at = start;
            for (size_t steps = 0; steps <= parents; steps++) {
                uint32_t parent = eb_parent(rows, groups + at);
                size_t next = parent ? eb_find(rows, groups, count, parent, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0) : SIZE_MAX;
                if (next == SIZE_MAX) break;
                if (next == start) { cycle = start; break; }
                at = next;
            }
        }
        if (cycle == SIZE_MAX) return 1;
        size_t chosen = SIZE_MAX, at = cycle;
        do {
            const eb_row *row = eb_chosen(rows, groups + at);
            if (row->source && (chosen == SIZE_MAX || row->source > eb_chosen(rows, groups + chosen)->source)) chosen = at;
            at = eb_find(rows, groups, count, eb_parent(rows, groups + at), SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0);
        } while (at != cycle && at != SIZE_MAX);
        if (chosen == SIZE_MAX || ++groups[chosen].pick >= eb_candidates(groups + chosen))
            return eb_fail(error, capacity, "active audio programs leave a bus with no acyclic parent");
    }
    return eb_fail(error, capacity, "active audio programs leave a bus with no acyclic parent");
}
/* Pick the highest-precedence live candidate of a required container key. */
static int eb_require(const eb_row *rows, eb_group *g)
{
    if (!eb_chosen(rows, g)->setting->removed) return 1;
    for (size_t k = 0; k < eb_candidates(g); k++)
        if (!eb_candidate(rows, g, k)->setting->removed) { g->pick = k; return 1; }
    return 0;
}
static int eb_containers(const eb_row *rows, eb_group *groups, size_t count, char *error, size_t capacity)
{
    for (size_t i = 0; i < count; i++) {
        const sh_audio_bus_setting *s = rows[groups[i].first].setting;
        if (s->field.field == SH_AUDIO_BUS_FIELD_RTPC_MODE) {
            int curves = 0;
            for (size_t j = 0; j < count && !curves; j++) {
                const sh_audio_bus_setting *c = rows[groups[j].first].setting;
                curves = c->bus == s->bus && c->field.field == SH_AUDIO_BUS_FIELD_RTPC_CURVE &&
                    c->field.key[0] == s->field.key[0] && !eb_chosen(rows, groups + j)->setting->removed;
            }
            /* The engine destroys a subscription with its final curve. */
            if (!curves) groups[i].forced_removed = 1;
            else if (!eb_require(rows, groups + i)) return eb_fail(error, capacity, "an active bus curve has no subscription mode");
        }
        if ((s->field.field == SH_AUDIO_BUS_FIELD_RTPC_CURVE || s->field.field == SH_AUDIO_BUS_FIELD_STATE) &&
            !eb_chosen(rows, groups + i)->setting->removed) {
            sh_audio_bus_field container = s->field.field == SH_AUDIO_BUS_FIELD_STATE ? SH_AUDIO_BUS_FIELD_STATE_GROUP :
                SH_AUDIO_BUS_FIELD_RTPC_MODE;
            size_t g = eb_find(rows, groups, count, s->bus, container, s->field.key[0], 0, 0);
            if (g == SIZE_MAX || !eb_require(rows, groups + g))
                return eb_fail(error, capacity, container == SH_AUDIO_BUS_FIELD_RTPC_MODE ?
                    "an active bus curve has no subscription mode" : "an active bus state mapping has no state group");
        }
    }
    return 1;
}
static int eb_bus_order(const void *left, const void *right)
{ uint32_t a = ((const sh_audio_bus_type *)left)->bus, b = ((const sh_audio_bus_type *)right)->bus; return a < b ? -1 : a > b; }

int sh_audio_effective_bus(const sh_audio_bus_policy *baseline, const sh_audio_bus_policy *const *programs,
    size_t count, sh_audio_bus_policy *out, char *error, size_t capacity)
{
    eb_row *rows = NULL; eb_group *groups = NULL; size_t used = 0, total = 0, group_count = 0;
    sh_audio_bus_policy policy = {0}; int ok = 0;
    if (error && capacity) *error = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if ((count && !programs) || (baseline && baseline->count && !baseline->settings))
        return eb_fail(error, capacity, "shared audio bus inputs are missing");
    total = baseline ? baseline->count : 0;
    for (size_t p = 0; p < count; p++) {
        if (!programs[p] || (programs[p]->count && !programs[p]->settings) || programs[p]->count > SIZE_MAX - total)
            return eb_fail(error, capacity, "shared audio bus inputs are missing");
        total += programs[p]->count;
    }
    if (total >= SIZE_MAX / sizeof(*rows)) goto memory;
    if (!(rows = malloc((total ? total : 1) * sizeof(*rows)))) goto memory;
    for (size_t i = 0; baseline && i < baseline->count; i++) rows[used++] = (eb_row){baseline->settings + i, 0};
    for (size_t p = 0; p < count; p++)
        for (size_t i = 0; i < programs[p]->count; i++) {
            const sh_audio_bus_setting *s = programs[p]->settings + i;
            if (!s->edited) continue;
            /* Native buses keep their kind; a new bus takes its first definer's. */
            unsigned kind = sh_audio_effective_bus_kind(baseline, s->bus);
            for (size_t q = 0; !kind && q < count; q++) kind = sh_audio_effective_bus_kind(programs[q], s->bus);
            if (sh_audio_effective_bus_kind(programs[p], s->bus) != kind) continue;
            rows[used++] = (eb_row){s, p + 1};
        }
    if (used > 1) qsort(rows, used, sizeof(*rows), eb_order);
    if (!(groups = calloc(used ? used : 1, sizeof(*groups)))) goto memory;
    for (size_t first = 0; first < used;) {
        size_t end = first + 1;
        while (end < used && !eb_key(rows[first].setting, rows[end].setting)) end++;
        groups[group_count++] = (eb_group){first, end, 0, rows[first].source == 0, 0};
        first = end;
    }
    if (!eb_parents(rows, groups, group_count, error, capacity) ||
        !eb_containers(rows, groups, group_count, error, capacity)) goto done;
    if (group_count > SIZE_MAX / sizeof(*policy.settings)) goto memory;
    policy.settings = calloc(group_count ? group_count : 1, sizeof(*policy.settings));
    policy.buses = calloc((baseline ? baseline->bus_count : 0) + group_count + 1, sizeof(*policy.buses));
    if (!policy.settings || !policy.buses) goto memory;
    for (size_t g = 0; g < group_count; g++) {
        const eb_group *group = groups + g; const eb_row *winner = eb_chosen(rows, group);
        const sh_audio_bus_setting *base = group->native ? rows[group->first].setting : NULL, *w = winner->setting;
        int removed = w->removed || group->forced_removed;
        if (removed && (!base || base->removed)) continue;
        sh_audio_bus_setting *s = policy.settings + policy.count++;
        s->bus = w->bus; s->field = w->field; s->field.offset = 0; s->removed = removed;
        s->recipe = winner->source; s->value = SIZE_MAX;
        if (removed) s->field.length = 0;
        s->edited = !base || removed != base->removed ||
            (!removed && (w->field.length != base->field.length || memcmp(w->body, base->body, w->field.length)));
        if (!removed) {
            if (!(s->body = malloc(w->field.length ? w->field.length : 1))) goto memory;
            memcpy(s->body, w->body, w->field.length);
        }
    }
    for (size_t i = 0; baseline && i < baseline->bus_count; i++) policy.buses[policy.bus_count++] = baseline->buses[i];
    for (size_t i = 0; i < policy.count; i++) {
        uint32_t bus = policy.settings[i].bus;
        if (sh_audio_effective_bus_kind(&policy, bus)) continue;
        unsigned kind = 0;
        for (size_t q = 0; !kind && q < count; q++) kind = sh_audio_effective_bus_kind(programs[q], bus);
        policy.buses[policy.bus_count++] = (sh_audio_bus_type){bus, kind};
    }
    if (policy.bus_count > 1) qsort(policy.buses, policy.bus_count, sizeof(*policy.buses), eb_bus_order);
    *out = policy; memset(&policy, 0, sizeof(policy)); ok = 1; goto done;
memory: eb_fail(error, capacity, "cannot allocate the effective audio bus configuration");
done:
    free(rows); free(groups); sh_audio_bus_policy_free(&policy); return ok;
}
