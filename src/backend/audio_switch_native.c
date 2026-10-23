#include "audio_switch_native.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef char aw_layout[(sizeof(sh_audio_switch_node) == 0x58 && offsetof(sh_audio_switch_node, values) == 0x10 &&
    offsetof(sh_audio_switch_node, controller) == 0x20 && offsetof(sh_audio_switch_node, consumers) == 0x28 &&
    offsetof(sh_audio_switch_node, vtable) == 0x38 && offsetof(sh_audio_switch_node, has_value) == 0x44 &&
    offsetof(sh_audio_switch_node, entries) == 0x48 && sizeof(sh_audio_switch_table) == 0x610) ? 1 : -1];
enum { AW_APPLIED = 1, AW_ACCEPTED };
#define AW_PARAMETER 0x40u
#define AW_CATEGORY 6u

struct sh_audio_switch_row {
    uint32_t group, controller, type, count;
    const unsigned char *record; /* desired record; NULL returns the group to unmapped */
    size_t length;
    int edited, changed, created, linked, installed;
    sh_audio_switch_node *node;
    sh_audio_parameter_vector before, after;
    uint32_t before_controller, before_type;
    unsigned char *points; /* transformed curve handed to the native setter */
};

static int aw_fail(char *error, size_t capacity, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); return 0; }
/* Complete STMG switch record: group, controller, type byte, point count and
 * twelve-byte points {input, switch value, shape}. Empty curves are never
 * records because the native reader does not apply them. */
static int aw_record(const unsigned char *record, size_t length, sh_audio_switch_row *row)
{
    uint32_t count;
    if (!record || length < 13) return 0;
    memcpy(&row->group, record, 4); memcpy(&row->controller, record + 4, 4); row->type = record[8];
    memcpy(&count, record + 9, 4);
    if (!count || count > (UINT32_MAX - 13) / 12 || length != 13 + (size_t)count * 12 || !row->controller) return 0;
    row->count = count; row->record = record; row->length = length; return 1;
}
/* The native construction keeps each authored switch value and hands the
 * subscription the same point with its output replaced by the value index. */
static void aw_point(const sh_audio_switch_row *row, uint32_t i, uint32_t *value, unsigned char *out)
{
    const unsigned char *point = row->record + 13 + (size_t)i * 12; float index = (float)i;
    memcpy(value, point + 4, 4);
    if (out) { memcpy(out, point, 4); memcpy(out + 4, &index, 4); memcpy(out + 8, point + 8, 4); }
}
static sh_audio_switch_node *aw_find(const sh_audio_switch_table *table, uint32_t id)
{
    uint64_t steps = 0;
    for (sh_audio_switch_node *node = table->buckets[id % 193]; node && steps <= table->count; node = node->next, steps++)
        if (node->id == id) return node;
    return NULL;
}
static int aw_table_valid(const sh_audio_switch_table *table)
{
    uint64_t count = 0;
    for (unsigned i = 0; i < 193; i++)
        for (const sh_audio_switch_node *node = table->buckets[i]; node; node = node->next)
            if (++count > table->count || node->id % 193 != i) return 0;
    return count == table->count;
}
static const sh_audio_switch_original *aw_original(const sh_audio_switch_native *native, uint32_t group)
{
    for (size_t i = 0; i < native->original_count; i++) if (native->originals[i].group == group) return native->originals + i;
    return NULL;
}
static void aw_originals_free(sh_audio_switch_original *rows, size_t count)
{ for (size_t i = 0; rows && i < count; i++) free(rows[i].record); free(rows); }
static int aw_owned(const sh_audio_switch_change *c)
{ return c && c->owner && c->owner->pending == c && c->thread == GetCurrentThreadId(); }
static int aw_alive(const sh_audio_switch_change *c)
{
    const sh_audio_subscription_native *s = c->owner->subscriptions;
    return c->manager && *c->owner->manager_slot == c->manager && *s->switch_slot == (void *)c->table &&
        *s->parameters->pool == c->pool;
}
static int aw_idle(const sh_audio_switch_change *c, const sh_audio_switch_node *node)
{
    return !node->consumers.count && !node->has_value && !node->entries.count && !node->values.count &&
        !node->controller && !sh_audio_subscription_live(c->owner->subscriptions, node, AW_PARAMETER, NULL);
}
static void aw_host_free(sh_audio_switch_change *c)
{
    for (size_t i = 0; c->rows && i < c->count; i++) free(c->rows[i].points);
    aw_originals_free(c->originals, c->original_count);
    free(c->requests); free(c->curves); free(c->rows);
}

/* Edited groups, earlier edits the policy now leaves unedited, and earlier
 * edits it no longer represents (restored to their original). */
static int aw_plan(sh_audio_switch_change *c, const sh_audio_shared_policy *policy)
{
    const sh_audio_switch_native *native = c->owner; size_t n = 0;
    if (!policy || (policy->count && !policy->settings)) return 0;
    for (size_t i = 0; i < policy->count; i++) n += policy->settings[i].record.kind == SH_AUDIO_SHARED_SWITCH;
    if (n > SIZE_MAX / sizeof(*c->rows) - native->original_count - 1) return 0;
    if (!(c->rows = calloc(n + native->original_count + 1, sizeof(*c->rows)))) return 0;
    for (size_t i = 0; i < policy->count; i++) {
        const sh_audio_shared_setting *s = policy->settings + i; sh_audio_switch_row row = {0};
        if (s->record.kind != SH_AUDIO_SHARED_SWITCH) continue;
        if (s->record.offset || s->record.key[1] || s->record.key[2] || !aw_record(s->body, s->record.length, &row) ||
            row.group != s->record.key[0]) return 0;
        for (size_t j = 0; j < c->count; j++) if (c->rows[j].group == row.group) return 0;
        if (!s->edited && !aw_original(native, row.group)) continue;
        row.edited = s->edited; c->rows[c->count++] = row;
    }
    for (size_t i = 0; i < native->original_count; i++) {
        const sh_audio_switch_original *original = native->originals + i; int represented = 0;
        for (size_t j = 0; j < policy->count && !represented; j++)
            represented = policy->settings[j].record.kind == SH_AUDIO_SHARED_SWITCH &&
                policy->settings[j].record.key[0] == original->group;
        if (represented) continue;
        sh_audio_switch_row row = {0}; row.group = original->group;
        if (original->record && (!aw_record(original->record, original->length, &row) || row.group != original->group)) return 0;
        c->rows[c->count++] = row;
    }
    return 1;
}
/* The live mapping as the record that reproduces it. 1 captured, 0 not a
 * native switch construction, -1 memory. */
static int aw_capture(const sh_audio_switch_change *c, const sh_audio_switch_row *row, sh_audio_switch_original *out)
{
    const sh_audio_switch_node *node = row->node; sh_audio_subscription_curve first = {0};
    size_t curves = node ? sh_audio_subscription_live(c->owner->subscriptions, node, AW_PARAMETER, &first) : 0;
    memset(out, 0, sizeof(*out)); out->group = row->group;
    if (!node || (!node->controller && !node->values.count && !curves)) return 1;
    uint32_t n = node->values.count;
    if (curves != 1 || !n || n > (UINT32_MAX - 13) / 12 || !node->values.items || first.curve || first.scaling ||
        first.count != n || !first.points || first.controller != node->controller || !node->controller || node->type > 255) return 0;
    out->length = 13 + (size_t)n * 12;
    if (!(out->record = malloc(out->length))) return -1;
    memcpy(out->record, &row->group, 4); memcpy(out->record + 4, &node->controller, 4);
    out->record[8] = (unsigned char)node->type; memcpy(out->record + 9, &n, 4);
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *point = (const unsigned char *)first.points + (size_t)i * 12;
        unsigned char *to = out->record + 13 + (size_t)i * 12; float index;
        memcpy(&index, point + 4, 4);
        if (index != (float)i) { free(out->record); memset(out, 0, sizeof(*out)); return 0; }
        memcpy(to, point, 4); memcpy(to + 4, (const uint32_t *)node->values.items + i, 4); memcpy(to + 8, point + 8, 4);
    }
    return 1;
}
static int aw_unchanged(const sh_audio_switch_change *c, const sh_audio_switch_row *row,
    const sh_audio_subscription_request *request)
{
    const sh_audio_switch_node *node = row->node;
    if (!node) return !row->record;
    if (!row->record)
        return !node->controller && !node->values.count && sh_audio_subscription_matches(c->owner->subscriptions, request);
    if (node->controller != row->controller || node->type != row->type || node->values.count != row->count ||
        !node->values.items) return 0;
    for (uint32_t i = 0; i < row->count; i++) {
        uint32_t value; aw_point(row, i, &value, NULL);
        if (((const uint32_t *)node->values.items)[i] != value) return 0;
    }
    return sh_audio_subscription_matches(c->owner->subscriptions, request);
}
/* Called after a fully reverted subscription change (same lock hold) or its
 * restore: previous mappings return and constructed nodes leave while idle. */
static void aw_revert(sh_audio_switch_change *c)
{
    for (size_t i = c->count; i > 0; i--) {
        sh_audio_switch_row *row = c->rows + i - 1; sh_audio_switch_node *node = row->node;
        if (row->installed) {
            node->values = row->before; node->controller = row->before_controller; node->type = row->before_type;
            row->installed = 0;
        }
        if (row->linked && aw_idle(c, node)) {
            sh_audio_switch_node **link = c->table->buckets + row->group % 193;
            while (*link && *link != node) link = &(*link)->next;
            if (*link) { *link = node->next; node->next = NULL; c->table->count--; row->linked = 0; }
        }
        /* Something attached to a constructed node: the engine keeps it. */
        if (row->linked) row->created = 0;
    }
}
/* Frees the side that is no longer live and ends the receipt. */
static void aw_cleanup(sh_audio_switch_change *c)
{
    sh_audio_parameter_native *parameters = c->owner->subscriptions->parameters; int accepted = c->state == AW_ACCEPTED;
    for (size_t i = 0; i < c->count; i++) {
        sh_audio_switch_row *row = c->rows + i;
        if (accepted) {
            if (row->installed && row->before.items) parameters->release(c->pool, row->before.items);
        } else {
            if (!row->installed && row->after.items) parameters->release(c->pool, row->after.items);
            if (row->created && !row->linked && row->node) parameters->release(c->pool, row->node);
        }
    }
    if (c->subscriptions.owner) sh_audio_subscription_release(&c->subscriptions);
    aw_host_free(c);
    if (c->owner->pending == c) c->owner->pending = NULL;
    memset(c, 0, sizeof(*c));
}
static int aw_publish(sh_audio_switch_change *c, char *error, size_t capacity)
{
    sh_audio_switch_native *native = c->owner; sh_audio_subscription_native *subscriptions = native->subscriptions;
    size_t used = 0;
    c->requests = calloc(c->count, sizeof(*c->requests)); c->curves = calloc(c->count, sizeof(*c->curves));
    c->originals = calloc(c->count, sizeof(*c->originals));
    if (!c->requests || !c->curves || !c->originals) return aw_fail(error, capacity, "cannot plan native audio switch settings");
    for (size_t i = 0; i < c->count; i++) {
        sh_audio_switch_row *row = c->rows + i;
        sh_audio_subscription_request *request = c->requests + used; sh_audio_subscription_curve *curve = c->curves + used;
        row->node = aw_find(c->table, row->group);
        if (row->record) {
            if (!(row->points = malloc((size_t)row->count * 12))) return aw_fail(error, capacity, "cannot plan native audio switch settings");
            for (uint32_t j = 0; j < row->count; j++) { uint32_t value; aw_point(row, j, &value, row->points + (size_t)j * 12); }
            *curve = (sh_audio_subscription_curve){0, row->controller, row->type, row->points, row->count, 0};
        }
        *request = (sh_audio_subscription_request){row->node, AW_PARAMETER, AW_CATEGORY, 0, 1, curve, row->record ? 1u : 0u};
        row->changed = !aw_unchanged(c, row, request);
        if (row->edited) {
            const sh_audio_switch_original *old = aw_original(native, row->group);
            sh_audio_switch_original *entry = c->originals + c->original_count;
            if (old) {
                *entry = (sh_audio_switch_original){old->group, NULL, old->length};
                if (old->record && !(entry->record = malloc(old->length)))
                    return aw_fail(error, capacity, "cannot record native audio switch originals");
                if (old->record) memcpy(entry->record, old->record, old->length);
                c->original_count++;
            } else if (row->changed) {
                int captured = aw_capture(c, row, entry);
                if (captured < 0) return aw_fail(error, capacity, "cannot record native audio switch originals");
                if (!captured) {
                    if (error && capacity) snprintf(error, capacity, "native audio switch group %u has an unrecognized live mapping", row->group);
                    return 0;
                }
                c->original_count++;
            }
        }
        if (!row->changed) { free(row->points); row->points = NULL; continue; }
        if (!row->node) {
            sh_audio_switch_table local = {0};
            if (!(row->node = subscriptions->switch_ensure(&local, row->group)))
                return aw_fail(error, capacity, "cannot construct native audio switch group");
            row->created = 1;
            if (local.count != 1 || local.buckets[row->group % 193] != row->node || row->node->next ||
                row->node->id != row->group || !aw_idle(c, row->node)) {
                c->uncertain = 1; return aw_fail(error, capacity, "native audio switch construction differs from the verified layout");
            }
            request->consumer = row->node;
        }
        if (row->record) {
            uint32_t *values = subscriptions->parameters->allocate(c->pool, (size_t)row->count * 4);
            if (!values) return aw_fail(error, capacity, "cannot allocate native audio switch values");
            for (uint32_t j = 0; j < row->count; j++) aw_point(row, j, values + j, NULL);
            row->after = (sh_audio_parameter_vector){values, row->count, row->count};
        }
        used++;
    }
    if (used && sh_audio_subscription_prepare(subscriptions, c->requests, used, &c->subscriptions, error, capacity) != 1) return 0;
    if (!aw_alive(c)) return aw_fail(error, capacity, "native audio switch manager changed during preparation");
    /* Mappings first, so the refresh below reads the new values. */
    for (size_t i = 0; i < c->count; i++) {
        sh_audio_switch_row *row = c->rows + i; sh_audio_switch_node *node = row->node;
        if (!row->changed) continue;
        if (row->created) {
            node->next = c->table->buckets[row->group % 193]; c->table->buckets[row->group % 193] = node;
            c->table->count++; row->linked = 1;
        }
        row->before = node->values; row->before_controller = node->controller; row->before_type = node->type;
        node->values = row->after; node->controller = row->record ? row->controller : 0;
        if (row->record) node->type = row->type;
        row->installed = 1;
    }
    if (used) {
        int published = sh_audio_subscription_publish(&c->subscriptions, error, capacity);
        if (published < 0) { c->uncertain = 1; return -1; }
        if (!published) { aw_revert(c); return 0; }
        sh_audio_subscription_refresh(&c->subscriptions);
    }
    c->state = AW_APPLIED; return 1;
}

int sh_audio_switch_native_bind(sh_audio_switch_native *native, sh_audio_subscription_native *subscriptions,
    const sh_audio_limits_native *limits)
{
    if (!native || native->pending) return 0;
    aw_originals_free(native->originals, native->original_count);
    memset(native, 0, sizeof(*native));
    if (!subscriptions || !subscriptions->switch_slot || !subscriptions->switch_ensure || !subscriptions->parameters ||
        !limits || !limits->render_lock || !limits->manager_slot || subscriptions->parameters->render_lock != limits->render_lock)
        return 0;
    native->subscriptions = subscriptions; native->render_lock = limits->render_lock; native->manager_slot = limits->manager_slot;
    return 1;
}
int sh_audio_switch_native_apply(sh_audio_switch_native *native, const sh_audio_shared_policy *policy,
    sh_audio_switch_change *change, char *error, size_t capacity)
{
    volatile int held = 0, result = 0;
    if (error && capacity) *error = 0;
    if (!native || native->pending || !change || change->owner)
        return aw_fail(error, capacity, "audio switch adapter is unavailable or has a pending transition");
    memset(change, 0, sizeof(*change)); change->owner = native;
    if (!aw_plan(change, policy)) {
        aw_host_free(change); memset(change, 0, sizeof(*change));
        return aw_fail(error, capacity, "compiled audio switch settings are malformed");
    }
    /* Nothing edited now or before: the native groups stay untouched. */
    if (!change->count) { aw_host_free(change); memset(change, 0, sizeof(*change)); return 1; }
    sh_audio_subscription_native *subscriptions = native->subscriptions;
    if (!subscriptions || !native->render_lock || !native->manager_slot || !subscriptions->switch_slot ||
        !subscriptions->switch_ensure || !subscriptions->parameters || !subscriptions->parameters->pool ||
        !subscriptions->parameters->allocate || !subscriptions->parameters->release || !subscriptions->parameters->parameter_slot) {
        aw_host_free(change); memset(change, 0, sizeof(*change));
        return aw_fail(error, capacity, "native audio switch publication is unavailable");
    }
    __try {
        __try {
            EnterCriticalSection(native->render_lock); held = 1;
            change->table = (sh_audio_switch_table *)*subscriptions->switch_slot; change->manager = *native->manager_slot;
            change->pool = *subscriptions->parameters->pool; change->thread = GetCurrentThreadId();
            if (!change->table || !change->manager || !*subscriptions->parameters->parameter_slot)
                aw_fail(error, capacity, "native audio switch manager is unavailable");
            else if (!aw_table_valid(change->table) || !sh_audio_subscription_valid(subscriptions))
                aw_fail(error, capacity, "native audio switch tables are inconsistent");
            else { native->pending = change; result = aw_publish(change, error, capacity); }
            if (!result && change->uncertain) result = -1;
            if (!result) aw_cleanup(change);
        } __finally { if (held) LeaveCriticalSection(native->render_lock); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (native->pending == change) { change->uncertain = 1; result = -1; } else result = 0;
    }
    /* A fault before the receipt was published wrote nothing native. */
    if (!result && change->owner) { aw_host_free(change); memset(change, 0, sizeof(*change)); }
    if (result < 0 && error && capacity && !*error) aw_fail(error, capacity, "native audio switch publication failed");
    return result;
}
static int aw_matches(const sh_audio_switch_change *c)
{
    if (!aw_alive(c)) return 0;
    for (size_t i = 0; i < c->count; i++) {
        const sh_audio_switch_row *row = c->rows + i; const sh_audio_switch_node *node = row->node;
        if (!row->changed) continue;
        if (aw_find(c->table, row->group) != node || node->values.items != row->after.items ||
            node->values.count != row->after.count || node->controller != (row->record ? row->controller : 0) ||
            (row->record && node->type != row->type)) return 0;
    }
    return !c->subscriptions.owner || sh_audio_subscription_ready(&c->subscriptions);
}
int sh_audio_switch_native_ready(sh_audio_switch_change *change, char *error, size_t capacity)
{
    volatile int held = 0; int result = 0;
    if (error && capacity) *error = 0;
    if (aw_owned(change) && change->state == AW_APPLIED && !change->uncertain) {
        CRITICAL_SECTION *lock = change->owner->render_lock;
        __try {
            __try { EnterCriticalSection(lock); held = 1; result = aw_matches(change); }
            __finally { if (held) LeaveCriticalSection(lock); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { result = 0; }
    }
    return result ? 1 : aw_fail(error, capacity, "audio switch mappings no longer belong to the pending transition");
}
int sh_audio_switch_native_restore(sh_audio_switch_change *change, char *error, size_t capacity)
{
    volatile int held = 0; int result = 0;
    if (error && capacity) *error = 0;
    if (aw_owned(change) && change->state == AW_APPLIED && !change->uncertain) {
        CRITICAL_SECTION *lock = change->owner->render_lock;
        __try {
            __try {
                EnterCriticalSection(lock); held = 1;
                if (aw_matches(change)) {
                    if (change->subscriptions.owner && !sh_audio_subscription_restore(&change->subscriptions, error, capacity)) {
                        if (change->subscriptions.uncertain) change->uncertain = 1;
                    } else {
                        aw_revert(change);
                        if (change->subscriptions.owner) sh_audio_subscription_refresh(&change->subscriptions);
                        aw_cleanup(change); result = 1;
                    }
                }
            } __finally { if (held) LeaveCriticalSection(lock); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { if (change->owner) change->uncertain = 1; result = 0; }
    }
    if (result) return 1;
    return error && capacity && *error ? 0 : aw_fail(error, capacity, "audio switch recovery retains native ownership");
}
void sh_audio_switch_native_accept(sh_audio_switch_change *change)
{
    if (!aw_owned(change) || change->state != AW_APPLIED || change->uncertain) return;
    sh_audio_switch_native *native = change->owner;
    aw_originals_free(native->originals, native->original_count);
    native->originals = change->originals; native->original_count = change->original_count;
    change->originals = NULL; change->original_count = 0; change->state = AW_ACCEPTED;
}
int sh_audio_switch_native_release(sh_audio_switch_change *change, char *error, size_t capacity)
{
    volatile int held = 0; int result = 0;
    if (error && capacity) *error = 0;
    if (aw_owned(change) && change->state == AW_ACCEPTED && !change->uncertain) {
        CRITICAL_SECTION *lock = change->owner->render_lock;
        __try {
            __try { EnterCriticalSection(lock); held = 1; aw_cleanup(change); result = 1; }
            __finally { if (held) LeaveCriticalSection(lock); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { if (change->owner) change->uncertain = 1; result = 0; }
    }
    return result ? 1 : aw_fail(error, capacity, "audio switch retirement retains native ownership");
}
