#include "../src/backend/audio_effective_bus.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hand-built bound policies: one row per (bus, field, key) as the compiler
 * emits them, with the edited flag the compiler derives from the baseline. */
typedef struct built { sh_audio_bus_setting rows[32]; unsigned char bodies[32][8]; size_t count; sh_audio_bus_type buses[8]; size_t bus_count; } built;
static void bus(built *b, uint32_t id, unsigned kind) { b->buses[b->bus_count++] = (sh_audio_bus_type){id, kind}; }
static void row(built *b, uint32_t id, sh_audio_bus_field field, uint32_t k0, uint32_t k1, int removed, int edited, uint32_t value)
{
    sh_audio_bus_setting *s = b->rows + b->count; memset(s, 0, sizeof(*s));
    s->bus = id; s->field.field = field; s->field.key[0] = k0; s->field.key[1] = k1; s->removed = removed; s->edited = edited;
    if (!removed) { memcpy(b->bodies[b->count], &value, 4); s->body = b->bodies[b->count]; s->field.length = 4; }
    b->count++;
}
static sh_audio_bus_policy view(built *b) { return (sh_audio_bus_policy){b->rows, b->count, b->buses, b->bus_count}; }
static const sh_audio_bus_setting *find(const sh_audio_bus_policy *p, uint32_t id, sh_audio_bus_field field, uint32_t k0, uint32_t k1)
{
    for (size_t i = 0; i < p->count; i++) {
        const sh_audio_bus_setting *s = p->settings + i;
        if (s->bus == id && s->field.field == field && s->field.key[0] == k0 && s->field.key[1] == k1) return s;
    }
    return NULL;
}
static uint32_t value(const sh_audio_bus_setting *s) { uint32_t v; assert(s && !s->removed && s->field.length == 4); memcpy(&v, s->body, 4); return v; }
static void sorted(const sh_audio_bus_policy *p)
{
    for (size_t i = 1; i < p->count; i++) {
        const sh_audio_bus_setting *a = p->settings + i - 1, *b = p->settings + i;
        int order = a->bus != b->bus ? (a->bus < b->bus ? -1 : 1) : a->field.field != b->field.field ? (a->field.field < b->field.field ? -1 : 1) :
            a->field.key[0] != b->field.key[0] ? (a->field.key[0] < b->field.key[0] ? -1 : 1) :
            a->field.key[1] != b->field.key[1] ? (a->field.key[1] < b->field.key[1] ? -1 : 1) : 0;
        assert(order < 0);
    }
    for (size_t i = 1; i < p->bus_count; i++) assert(p->buses[i - 1].bus < p->buses[i].bus);
}
/* Native tree: 1 root, 2 and 3 children of 1. Bus 2 has a property, an RTPC
 * mode/curve on parameter 5 and a state group 7 with one mapping. */
static void native(built *b)
{
    memset(b, 0, sizeof(*b)); bus(b, 1, 8); bus(b, 2, 8); bus(b, 3, 20);
    row(b, 1, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 0, 0);
    row(b, 2, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 0, 1);
    row(b, 2, SH_AUDIO_BUS_FIELD_PROPERTY, 4, 0, 0, 0, 100);
    row(b, 2, SH_AUDIO_BUS_FIELD_RTPC_MODE, 5, 0, 0, 0, 1);
    row(b, 2, SH_AUDIO_BUS_FIELD_RTPC_CURVE, 5, 1, 0, 0, 51);
    row(b, 2, SH_AUDIO_BUS_FIELD_STATE_GROUP, 7, 0, 0, 0, 0);
    row(b, 2, SH_AUDIO_BUS_FIELD_STATE, 7, 70, 0, 0, 700);
    row(b, 3, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 0, 1);
}
int main(void)
{
    built base, map, library; sh_audio_bus_policy e = {0}; char error[256];
    native(&base); sh_audio_bus_policy baseline = view(&base);
    /* Nothing active: exactly the native configuration, no edits. */
    assert(sh_audio_effective_bus(&baseline, NULL, 0, &e, error, sizeof(error)));
    sorted(&e); assert(e.count == base.count && !sh_audio_effective_bus_edited(&e) && e.bus_count == 3);
    sh_audio_bus_policy_free(&e);
    /* Map and library edit the same property; each adds an independent key.
     * The map wins the overlap, the library's own key survives. */
    native(&map); native(&library);
    map.rows[2].edited = 1; memcpy(map.bodies[2], &(uint32_t){200}, 4);
    library.rows[2].edited = 1; memcpy(library.bodies[2], &(uint32_t){300}, 4);
    row(&library, 2, SH_AUDIO_BUS_FIELD_PROPERTY, 6, 0, 0, 1, 60);
    /* The map removes the only curve: its mode leaves with it. */
    map.rows[4].removed = map.rows[4].edited = 1; map.rows[4].body = NULL; map.rows[4].field.length = 0;
    sh_audio_bus_policy mv = view(&map), lv = view(&library);
    const sh_audio_bus_policy *views[2] = {&mv, &lv};
    assert(sh_audio_effective_bus(&baseline, views, 2, &e, error, sizeof(error)));
    sorted(&e);
    const sh_audio_bus_setting *s = find(&e, 2, SH_AUDIO_BUS_FIELD_PROPERTY, 4, 0);
    assert(value(s) == 200 && s->edited && s->recipe == 1);
    s = find(&e, 2, SH_AUDIO_BUS_FIELD_PROPERTY, 6, 0); assert(value(s) == 60 && s->edited && s->recipe == 2);
    assert(find(&e, 2, SH_AUDIO_BUS_FIELD_RTPC_CURVE, 5, 1)->removed && find(&e, 2, SH_AUDIO_BUS_FIELD_RTPC_CURVE, 5, 1)->edited);
    s = find(&e, 2, SH_AUDIO_BUS_FIELD_RTPC_MODE, 5, 0); assert(s->removed && s->edited);
    assert(!find(&e, 1, SH_AUDIO_BUS_FIELD_PARENT, 0, 0)->edited && value(find(&e, 2, SH_AUDIO_BUS_FIELD_STATE, 7, 70)) == 700);
    sh_audio_bus_policy_free(&e);
    /* A library curve on the same parameter keeps the native mode alive. */
    row(&library, 2, SH_AUDIO_BUS_FIELD_RTPC_CURVE, 5, 2, 0, 1, 52); lv = view(&library);
    assert(sh_audio_effective_bus(&baseline, views, 2, &e, error, sizeof(error)));
    s = find(&e, 2, SH_AUDIO_BUS_FIELD_RTPC_MODE, 5, 0); assert(!s->removed && !s->edited && value(s) == 1);
    assert(value(find(&e, 2, SH_AUDIO_BUS_FIELD_RTPC_CURVE, 5, 2)) == 52);
    sh_audio_bus_policy_free(&e);
    /* Crossed parent edits: the lower-precedence one falls back to native. */
    native(&map); native(&library);
    map.rows[7].edited = 1; memcpy(map.bodies[7], &(uint32_t){2}, 4);     /* 3 under 2 */
    library.rows[1].edited = 1; memcpy(library.bodies[1], &(uint32_t){3}, 4); /* 2 under 3 */
    mv = view(&map); lv = view(&library);
    assert(sh_audio_effective_bus(&baseline, views, 2, &e, error, sizeof(error)));
    assert(value(find(&e, 3, SH_AUDIO_BUS_FIELD_PARENT, 0, 0)) == 2 && value(find(&e, 2, SH_AUDIO_BUS_FIELD_PARENT, 0, 0)) == 1);
    assert(!find(&e, 2, SH_AUDIO_BUS_FIELD_PARENT, 0, 0)->edited);
    sh_audio_bus_policy_free(&e);
    /* New buses: the first definer chooses the kind; a conflicting definer
     * does not contribute. New buses with only cyclic parents fail. */
    native(&map); native(&library);
    bus(&map, 50, 20); row(&map, 50, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 1, 1);
    bus(&library, 50, 8); row(&library, 50, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 1, 1);
    row(&library, 50, SH_AUDIO_BUS_FIELD_PROPERTY, 4, 0, 0, 1, 9);
    mv = view(&map); lv = view(&library);
    assert(sh_audio_effective_bus(&baseline, views, 2, &e, error, sizeof(error)));
    assert(sh_audio_effective_bus_kind(&e, 50) == 20 && !find(&e, 50, SH_AUDIO_BUS_FIELD_PROPERTY, 4, 0));
    s = find(&e, 50, SH_AUDIO_BUS_FIELD_PARENT, 0, 0); assert(value(s) == 1 && s->edited && s->recipe == 1);
    assert(e.bus_count == 4); sorted(&e); sh_audio_bus_policy_free(&e);
    native(&map); bus(&map, 60, 8); bus(&map, 61, 8);
    row(&map, 60, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 1, 61); row(&map, 61, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 1, 60);
    mv = view(&map);
    assert(!sh_audio_effective_bus(&baseline, views, 1, &e, error, sizeof(error)) && strstr(error, "acyclic") && !e.count);
    /* A state mapping keeps its group even when another program deletes it. */
    native(&map); native(&library);
    row(&map, 2, SH_AUDIO_BUS_FIELD_STATE, 7, 71, 0, 1, 701);
    library.rows[5].removed = library.rows[5].edited = 1; library.rows[5].body = NULL; library.rows[5].field.length = 0;
    library.rows[6].removed = library.rows[6].edited = 1; library.rows[6].body = NULL; library.rows[6].field.length = 0;
    mv = view(&map); lv = view(&library);
    assert(sh_audio_effective_bus(&baseline, views, 2, &e, error, sizeof(error)));
    assert(!find(&e, 2, SH_AUDIO_BUS_FIELD_STATE_GROUP, 7, 0)->removed && value(find(&e, 2, SH_AUDIO_BUS_FIELD_STATE, 7, 71)) == 701);
    assert(find(&e, 2, SH_AUDIO_BUS_FIELD_STATE, 7, 70)->removed);
    sh_audio_bus_policy_free(&e);
    /* A removal of a key the native configuration lacks is no setting. */
    native(&map); row(&map, 2, SH_AUDIO_BUS_FIELD_PROPERTY, 9, 0, 1, 1, 0); mv = view(&map);
    assert(sh_audio_effective_bus(&baseline, views, 1, &e, error, sizeof(error)) && !find(&e, 2, SH_AUDIO_BUS_FIELD_PROPERTY, 9, 0));
    assert(!sh_audio_effective_bus_edited(&e)); sh_audio_bus_policy_free(&e);
    const sh_audio_bus_policy *missing[1] = {NULL};
    assert(!sh_audio_effective_bus(&baseline, missing, 1, &e, error, sizeof(error)));
    puts("effective audio bus checks passed"); return 0;
}
