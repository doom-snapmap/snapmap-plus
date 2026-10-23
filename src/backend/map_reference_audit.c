#include "map_reference_audit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_json.h"

enum { MRA_ENT_KEYS, MRA_ENT_VALUES, MRA_VAR_KEYS, MRA_VAR_VALUES, MRA_INST_KEYS, MRA_INST_VALUES,
       MRA_CONNECTIONS, MRA_LISTS };
static const char *const mra_paths[MRA_LISTS][3] = {
    {"references", "entityEntRefs", "keyValues"}, {"references", "entityEntRefs", "values"},
    {"references", "entityVarRefs", "keyValues"}, {"references", "entityVarRefs", "values"},
    {"instanceEntities", "keyValues", NULL}, {"instanceEntities", "values", NULL},
    {"targets", "connections", NULL}
};
static const char *const mra_names[MRA_LISTS] = {
    "references.entityEntRefs.keyValues", "references.entityEntRefs.values",
    "references.entityVarRefs.keyValues", "references.entityVarRefs.values",
    "instanceEntities.keyValues", "instanceEntities.values", "targets.connections"
};

typedef struct mra_list { long long *items; size_t count, capacity; int located, malformed; } mra_list;
typedef struct mra_entity { long long uid; int has_uid; char inherit[160]; } mra_entity;
typedef struct mra_scan {
    mra_list lists[MRA_LISTS];
    mra_entity *entities; size_t entity_count, entity_capacity;
    int *held; size_t held_count;   /* uniqueId -> entities[] index + 1, when dense enough */
    int failed;
} mra_scan;

static int mra_key_is(const sh_json_path *node, const char *key)
{ return node->key && node->key_length == strlen(key) && !memcmp(node->key, key, node->key_length); }

static int mra_number(const char *json, size_t length, long long *out)
{
    char digits[24], *end = NULL;
    if (!length || length >= sizeof digits) return 0;
    memcpy(digits, json, length); digits[length] = 0;
    *out = _strtoi64(digits, &end, 10);
    return end && !*end && (digits[0] == '-' ? length > 1 : 1);
}

static int mra_push(mra_list *list, size_t index, long long value)
{
    if (index != list->count) { list->malformed = 1; return 1; }
    if (list->count == list->capacity) {
        size_t grown = list->capacity ? list->capacity * 2 : 64;
        long long *more;
        if (grown > ((size_t)-1) / sizeof(*more) || !(more = realloc(list->items, grown * sizeof(*more)))) return 0;
        list->items = more; list->capacity = grown;
    }
    list->items[list->count++] = value;
    return 1;
}

static mra_entity *mra_entity_at(mra_scan *scan, size_t index)
{
    if (index >= scan->entity_count) {
        if (index >= scan->entity_capacity) {
            size_t grown = scan->entity_capacity ? scan->entity_capacity * 2 : 64;
            mra_entity *more;
            while (grown <= index) grown *= 2;
            if (grown > ((size_t)-1) / sizeof(*more) || !(more = realloc(scan->entities, grown * sizeof(*more)))) return NULL;
            scan->entities = more; scan->entity_capacity = grown;
        }
        memset(scan->entities + scan->entity_count, 0, (index + 1 - scan->entity_count) * sizeof(*scan->entities));
        scan->entity_count = index + 1;
    }
    return scan->entities + index;
}

static int mra_visit(void *context, const sh_json_path *path, const char *json, size_t length, sh_json_kind kind)
{
    mra_scan *scan = context;
    const sh_json_path *nodes[5];
    size_t depth = 0;
    for (const sh_json_path *p = path; p; p = p->parent) { if (depth == 5) return 1; nodes[depth++] = p; }
    for (size_t i = 0; i < depth / 2; i++) { const sh_json_path *t = nodes[i]; nodes[i] = nodes[depth - 1 - i]; nodes[depth - 1 - i] = t; }
    if (!depth || !nodes[0]->key) return 1;
    if (mra_key_is(nodes[0], "entities")) {
        mra_entity *entity;
        if (depth < 2 || nodes[1]->key) return 1;
        if (!(entity = mra_entity_at(scan, nodes[1]->index))) { scan->failed = 1; return 0; }
        if (depth == 3 && mra_key_is(nodes[2], "uniqueId") && kind == SH_JSON_NUMBER)
            entity->has_uid = mra_number(json, length, &entity->uid);
        else if (depth == 4 && mra_key_is(nodes[2], "entityDef") && mra_key_is(nodes[3], "inherit") &&
                 kind == SH_JSON_STRING) {
            size_t decoded = 0;
            if (!sh_native_json_decode_string(json, length, entity->inherit, sizeof(entity->inherit), &decoded))
                strcpy_s(entity->inherit, sizeof(entity->inherit), "?");
        }
        return 1;
    }
    for (int list = 0; list < MRA_LISTS; list++) {
        size_t n = 0, k;
        while (n < 3 && mra_paths[list][n]) n++;
        if (depth != n && depth != n + 1) continue;
        for (k = 0; k < n; k++) if (!mra_key_is(nodes[k], mra_paths[list][k])) break;
        if (k != n) continue;
        if (depth == n) { if (kind == SH_JSON_ARRAY) scan->lists[list].located = 1; return 1; }
        if (nodes[n]->key) return 1;
        {
            long long value = 0;
            if (kind != SH_JSON_NUMBER || !mra_number(json, length, &value)) { scan->lists[list].malformed = 1; return 1; }
            if (!mra_push(scan->lists + list, nodes[n]->index, value)) { scan->failed = 1; return 0; }
        }
        return 1;
    }
    return 1;
}

typedef struct mra_text { char *bytes; size_t capacity, length; unsigned count; } mra_text;
static void mra_say(mra_text *text, const char *format, ...)
{
    va_list args;
    int n;
    text->count++;
    if (!text->capacity || text->length + 1 >= text->capacity) return;
    if (text->count > 1) {
        n = _snprintf_s(text->bytes + text->length, text->capacity - text->length, _TRUNCATE, "; ");
        if (n < 0) { text->length = text->capacity - 1; return; }
        text->length += (size_t)n;
    }
    va_start(args, format);
    n = _vsnprintf_s(text->bytes + text->length, text->capacity - text->length, _TRUNCATE, format, args);
    va_end(args);
    text->length = n < 0 ? text->capacity - 1 : text->length + (size_t)n;
}

static long long mra_rows(const mra_list *keys, long long uid)
{
    if (uid < 0 || (size_t)uid + 1 >= keys->count) return 0;
    return keys->items[uid + 1] - keys->items[uid];
}

static int mra_entity_index(const mra_scan *scan, long long uid)
{
    if (scan->held) return uid >= 0 && (size_t)uid < scan->held_count ? scan->held[uid] - 1 : -1;
    for (size_t i = 0; i < scan->entity_count; i++)
        if (scan->entities[i].has_uid && scan->entities[i].uid == uid) return (int)i;
    return -1;
}

/* Prefix sums over `values`: start at zero, never fall, end at its count. */
static int mra_prefix_sums(const mra_scan *scan, int keys, int values, mra_text *problems)
{
    const mra_list *k = scan->lists + keys;
    size_t total = scan->lists[values].count;
    if (!k->count) { mra_say(problems, "%s is empty", mra_names[keys]); return 0; }
    if (k->items[0] != 0) { mra_say(problems, "%s starts at %lld, not 0", mra_names[keys], k->items[0]); return 0; }
    for (size_t i = 1; i < k->count; i++)
        if (k->items[i] < k->items[i - 1]) {
            mra_say(problems, "%s falls from %lld to %lld at slot %zu", mra_names[keys], k->items[i - 1], k->items[i], i);
            return 0;
        }
    if (k->items[k->count - 1] != (long long)total) {
        mra_say(problems, "%s ends at %lld but %s holds %zu", mra_names[keys], k->items[k->count - 1],
            mra_names[values], total);
        return 0;
    }
    return 1;
}

int sh_map_reference_audit_run(const char *json, size_t length, sh_map_reference_audit *out)
{
    mra_scan scan = {0};
    mra_text problems = {0}, hazards = {0};
    long long max_uid = -1;
    int sums_ok[2] = {0, 0}, ok = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    problems.bytes = out->problem_text; problems.capacity = sizeof(out->problem_text);
    hazards.bytes = out->hazard_text; hazards.capacity = sizeof(out->hazard_text);
    if (!json || !length || !sh_native_json_visit_values(json, length, 128, mra_visit, &scan) || scan.failed) goto done;
    for (int list = 0; list < MRA_LISTS; list++)
        if (!scan.lists[list].located || scan.lists[list].malformed) goto done;
    out->readable = 1; ok = 1;

    for (size_t i = 0; i < scan.entity_count; i++) {
        const mra_entity *entity = scan.entities + i;
        if (!entity->has_uid || entity->uid < 0) { mra_say(&problems, "entities[%zu] has no usable uniqueId", i); continue; }
        if (entity->uid > max_uid) max_uid = entity->uid;
        for (size_t j = 0; j < i; j++)
            if (scan.entities[j].has_uid && scan.entities[j].uid == entity->uid) {
                mra_say(&problems, "uniqueId %lld is held by entities[%zu] and entities[%zu]", entity->uid, j, i);
                break;
            }
    }

    if (max_uid >= 0 && max_uid < (1 << 20) && scan.entity_count < (1u << 30) &&
        (scan.held = calloc((size_t)max_uid + 1, sizeof(*scan.held)))) {
        scan.held_count = (size_t)max_uid + 1;
        for (size_t i = scan.entity_count; i-- > 0;)
            if (scan.entities[i].has_uid && scan.entities[i].uid >= 0) scan.held[scan.entities[i].uid] = (int)i + 1;
    }

    /* Reference rows: complete prefix sums covering every held uniqueId. */
    for (int m = 0; m < 2; m++) {
        int keys = m ? MRA_VAR_KEYS : MRA_ENT_KEYS, values = m ? MRA_VAR_VALUES : MRA_ENT_VALUES;
        const mra_list *k = scan.lists + keys;
        if (!mra_prefix_sums(&scan, keys, values, &problems)) continue;
        if (max_uid >= 0 && k->count < (size_t)max_uid + 2) {
            mra_say(&problems, "%s has %zu slots and does not cover uniqueId %lld", mra_names[keys], k->count, max_uid);
            continue;
        }
        sums_ok[m] = 1;
    }
    if (sums_ok[0] && sums_ok[1]) {
        const mra_list *ek = scan.lists + MRA_ENT_KEYS, *vk = scan.lists + MRA_VAR_KEYS;
        size_t slots = (ek->count > vk->count ? ek->count : vk->count) - 1;
        unsigned char *seen = calloc(scan.entity_count ? scan.entity_count : 1, 1);
        if (!seen) { ok = 0; out->readable = 0; goto done; }
        /* Rows held by no entity. */
        for (size_t uid = 0; uid < slots; uid++) {
            long long e = mra_rows(ek, (long long)uid), v = mra_rows(vk, (long long)uid);
            if ((e || v) && mra_entity_index(&scan, (long long)uid) < 0)
                mra_say(&hazards, "uniqueId %zu holds %lld entity and %lld variable reference row(s) but no entity",
                    uid, e, v);
        }
        /* Entities sharing an inherit share the engine's expected row counts. */
        for (size_t i = 0; i < scan.entity_count; i++) {
            const mra_entity *a = scan.entities + i;
            size_t members = 0, agree = 0, best = 0, best_index = i;
            if (seen[i] || !a->has_uid || !a->inherit[0]) continue;
            for (size_t j = i; j < scan.entity_count; j++) {
                const mra_entity *b = scan.entities + j;
                if (!b->has_uid || strcmp(a->inherit, b->inherit)) continue;
                seen[j] = 1; members++;
                agree = 0;
                for (size_t q = i; q < scan.entity_count; q++) {
                    const mra_entity *c = scan.entities + q;
                    if (c->has_uid && !strcmp(a->inherit, c->inherit) &&
                        mra_rows(ek, c->uid) == mra_rows(ek, b->uid) && mra_rows(vk, c->uid) == mra_rows(vk, b->uid)) agree++;
                }
                if (agree > best) { best = agree; best_index = j; }
            }
            if (members < 2 || best == members) continue;
            {
                const mra_entity *model = scan.entities + best_index;
                long long me = mra_rows(ek, model->uid), mv = mra_rows(vk, model->uid);
                for (size_t j = i; j < scan.entity_count; j++) {
                    const mra_entity *b = scan.entities + j;
                    long long be, bv;
                    if (!b->has_uid || strcmp(a->inherit, b->inherit)) continue;
                    be = mra_rows(ek, b->uid); bv = mra_rows(vk, b->uid);
                    if (be == me && bv == mv) continue;
                    if (best * 2 > members)
                        mra_say(&problems, "uniqueId %lld (%s) owns %lld entity and %lld variable reference row(s) "
                            "where %zu other entit%s with that inherit own %lld and %lld", b->uid, b->inherit, be, bv,
                            best, best == 1 ? "y" : "ies", me, mv);
                    else
                        mra_say(&problems, "uniqueIds %lld and %lld (%s) own different reference rows: %lld/%lld and %lld/%lld "
                            "entity/variable", model->uid, b->uid, b->inherit, me, mv, be, bv);
                }
            }
        }
        free(seen);
    }

    /* Instance attribution names held uniqueIds, each once. */
    if (mra_prefix_sums(&scan, MRA_INST_KEYS, MRA_INST_VALUES, &problems)) {
        const mra_list *values = scan.lists + MRA_INST_VALUES;
        for (size_t i = 0; i < values->count; i++) {
            if (mra_entity_index(&scan, values->items[i]) < 0)
                mra_say(&problems, "instanceEntities names uniqueId %lld, which no entity holds", values->items[i]);
            for (size_t j = 0; j < i; j++)
                if (values->items[j] == values->items[i]) {
                    mra_say(&problems, "instanceEntities names uniqueId %lld twice", values->items[i]); break;
                }
        }
    }

    /* Connections: runs opened by -(source + 1), targets stored raw. */
    {
        const mra_list *wires = scan.lists + MRA_CONNECTIONS;
        int open = 0;
        for (size_t i = 0; i < wires->count; i++) {
            long long v = wires->items[i], uid = v < 0 ? -v - 1 : v;
            if (v >= 0 && !open) { mra_say(&problems, "targets.connections starts a target %lld outside a source run", v); break; }
            open = 1;
            if (mra_entity_index(&scan, uid) < 0)
                mra_say(&problems, "targets.connections names %s uniqueId %lld, which no entity holds",
                    v < 0 ? "source" : "target", uid);
        }
    }

done:
    out->problems = ok ? problems.count : 0;
    out->hazards = ok ? hazards.count : 0;
    if (!ok) { out->problem_text[0] = 0; out->hazard_text[0] = 0; }
    for (int list = 0; list < MRA_LISTS; list++) free(scan.lists[list].items);
    free(scan.entities); free(scan.held);
    return ok;
}
