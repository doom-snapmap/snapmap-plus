#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/audio_subscription_native.c"
#include "../src/backend/audio_bus_native.c"
#include "../src/backend/audio_effective_bus.h"

/* A fake audio engine following the native bus rules the adapter relies on:
 * reference-counted buses in an ID index, parent attach/detach through their
 * vtables, lazily allocated effect/mixer/duck/state storage, the pool, and
 * the parameter-subscription table. Every allocation is tracked. */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
static int failures, pool = 5, calls, fail_at, allocations, init_fail, value_notices, curve_notices, state_notices, effect_notices;
static uint32_t last_value_parameter, last_curve_parameter;
static void *tracked[2048];
static CRITICAL_SECTION lock;
static int locked(void) { return lock.OwningThread == (HANDLE)(uintptr_t)GetCurrentThreadId(); }
static void *allocate(int p, size_t bytes)
{
    CHECK(p == pool && locked()); calls++;
    if (calls == fail_at) return NULL;
    void *value = calloc(1, bytes ? bytes : 1); CHECK(value); if (!value) return NULL;
    for (unsigned i = 0; i < 2048; i++) if (!tracked[i]) { tracked[i] = value; allocations++; return value; }
    CHECK(0); free(value); return NULL;
}
static void release(int p, void *value)
{
    CHECK(p == pool && locked());
    for (unsigned i = 0; i < 2048; i++) if (tracked[i] == value) { tracked[i] = NULL; allocations--; free(value); return; }
    CHECK(0);
}
/* ---- parameter subscriptions (as in the switch test) ---- */
typedef struct param_fixture {
    sh_audio_parameter_table table;
    unsigned char *records[193];
    uint32_t total, padding;
    unsigned char rest[0x100];
} param_fixture;
static param_fixture params;
static sh_audio_parameter_table *param_slot;
static void *modulator_slot = &params, *switch_slot, *manager;
static sh_audio_parameter_node *p_find(sh_audio_parameter_table *t, uint32_t id)
{ for (sh_audio_parameter_node *n = t->buckets[id % 193]; n; n = n->next) if (n->id == id) return n; return NULL; }
static sh_audio_parameter_node *p_ensure(sh_audio_parameter_table *t, uint32_t id)
{
    sh_audio_parameter_node *n = p_find(t, id);
    if (n) return n;
    if (!(n = allocate(pool, sizeof(*n)))) return NULL;
    n->id = id; n->next = t->buckets[id % 193]; t->buckets[id % 193] = n; t->count++; return n;
}
static void p_destroy(sh_audio_parameter_node *n)
{
    for (unsigned i = 0; i < 2; i++) if (n->subscribers[i].items) release(pool, n->subscribers[i].items);
    memset(n->subscribers, 0, sizeof(n->subscribers));
}
static int listed(const sh_audio_parameter_vector *l, const void *item)
{ for (uint32_t i = 0; i < l->count; i++) if (((void **)l->items)[i] == item) return 1; return 0; }
static void remove_item(sh_audio_parameter_vector *l, const void *item)
{
    void **items = l->items;
    for (uint32_t i = 0; i < l->count; i++) if (items[i] == item) { memmove(items + i, items + i + 1, (l->count - i - 1) * sizeof(void *)); l->count--; return; }
}
static int f_reverse_add(sh_audio_parameter_node *n, void *record, char selector)
{
    CHECK(locked());
    if (listed(n->subscribers, record) || listed(n->subscribers + 1, record)) return 1;
    sh_audio_parameter_vector *l = n->subscribers + (selector ? 0 : 1);
    CHECK(n->subscribers[0].capacity >= n->subscribers[0].count + n->subscribers[1].count + 1);
    if (l->count >= l->capacity) return 0x34;
    ((void **)l->items)[l->count++] = record; return 1;
}
static char f_reverse_remove(sh_audio_parameter_node *n, void *record)
{
    const sh_audio_parameter_vector *curves = (const sh_audio_parameter_vector *)((unsigned char *)record + 0x50);
    for (uint32_t i = 0; i < curves->count; i++) if (((const as_curve *)curves->items)[i].controller == n->id) return 0;
    remove_item(n->subscribers, record); remove_item(n->subscribers + 1, record); return 1;
}
static int f_modulator_add(void *m, uint32_t id, void *r) { (void)m; (void)id; (void)r; return 2; }
static void f_modulator_remove(void *m, void *r, uint32_t id) { (void)m; (void)r; (void)id; }
static int refreshes;
static void f_refresh(sh_audio_parameter_table *t, void *r, char flag) { (void)t; (void)r; CHECK(flag == 1 && locked()); refreshes++; }
static int f_set_curve(sh_audio_native_curve *c, const void *points, uint32_t count, uint32_t scaling)
{
    if (c->points) { release(pool, c->points); c->points = NULL; }
    c->count = c->scaling = 0;
    if (!points || !count) return 0x1f;
    if (!(c->points = allocate(pool, (size_t)count * 12))) return 0x34;
    memcpy(c->points, points, (size_t)count * 12); c->count = count; c->scaling = scaling; return 1;
}
static void *f_switch_ensure(void *t, uint32_t id) { (void)t; (void)id; return NULL; }
/* ---- buses ---- */
static unsigned char index_block[0x2000], *index_value = index_block;
static unsigned char *objects[64];
static size_t object_count;
static void *normal_vtable[0x2b8 / 8], *aux_vtable[0x2b8 / 8], *consumer_vtable[8], *state_object_vtable[4];
static unsigned char *root_slots[2];
static uint32_t sample_rate = 48000;
static uint16_t duck_floor = 16;
static int32_t property_parameters[AB_PROPERTY_COUNT];
static uint32_t refs(const unsigned char *o) { return ab_u32(o + 0x14); }
static void set_refs(unsigned char *o, uint32_t v) { memcpy(o + 0x14, &v, 4); }
static unsigned char *registered(uint32_t id)
{ for (size_t i = 0; i < object_count; i++) if (ab_u32(objects[i] + AB_ID) == id) return objects[i]; return NULL; }
static int f_addref(void *o) { CHECK(locked()); set_refs(o, refs(o) + 1); return (int)refs(o); }
static void f_detach(void *parent, void *child);
static void destroy_groups(unsigned char *bus);
static void drop_records(unsigned char *bus);
static int f_release(void *o)
{
    unsigned char *bus = o; CHECK(locked() && refs(bus) > 0);
    set_refs(bus, refs(bus) - 1);
    if (refs(bus)) return (int)refs(bus);
    for (size_t i = 0; i < object_count; i++) if (objects[i] == bus) { objects[i] = objects[--object_count]; break; }
    unsigned char *parent = ab_ptr(bus + AB_PARENT);
    if (parent) f_detach(parent, bus);
    drop_records(bus); destroy_groups(bus);
    for (unsigned char *d = ab_ptr(bus + AB_DUCK_HEAD), *next; d; d = next) { next = ab_ptr(d); release(pool, d); }
    for (unsigned char *d = ab_ptr(bus + AB_DUCK_FREE), *next; d; d = next) { next = ab_ptr(d); release(pool, d); }
    void *owned[] = {ab_ptr(bus + AB_PROPERTIES), ab_ptr(bus + AB_EFFECTS), ab_ptr(bus + AB_MIXER), ab_ptr(bus + AB_MASKS)};
    for (size_t i = 0; i < 4; i++) if (owned[i]) release(pool, owned[i]);
    release(pool, bus); return 0;
}
static void f_set_parent(void *child, void *parent) { ab_set_ptr((unsigned char *)child + AB_PARENT, parent); }
static int f_attach(void *parent, void *child)
{
    CHECK(locked());
    int result = 1;
    if (ab_ptr((unsigned char *)child + AB_PARENT)) result = 0x15;
    else if (child == parent) result = 0x17;
    else { f_set_parent(child, parent); f_addref(parent); }
    f_release(child); return result;
}
static void f_detach(void *parent, void *child)
{
    if (ab_ptr((unsigned char *)child + AB_PARENT) != parent) return;
    f_set_parent(child, NULL); f_release(parent);
}
static void f_curve_changed(void *bus, uint32_t parameter) { (void)bus; CHECK(locked()); curve_notices++; last_curve_parameter = parameter; }
static void f_states_changed(void *bus) { (void)bus; CHECK(locked()); state_notices++; }
static void f_value_changed(void *consumer, char flag, uint32_t parameter) { (void)consumer; CHECK(flag == 1 && locked()); value_notices++; last_value_parameter = parameter; }
static int f_mixer(void *b, uint32_t id, char shared, int priority)
{
    unsigned char *bus = b, *m = ab_ptr(bus + AB_MIXER);
    if (id && !m) {
        if (!(m = allocate(pool, 0xc))) return 2;
        m[9] = 1; ab_set_ptr(bus + AB_MIXER, m);
    }
    if (m) {
        int stored; memcpy(&stored, m, 4);
        if (priority < stored) return 1;
        memcpy(m, &priority, 4);
        if (m[9] != (unsigned char)shared || ab_u32(m + 4) != id) { m[9] = (unsigned char)shared; memcpy(m + 4, &id, 4); effect_notices++; }
        if (!id) { release(pool, m); ab_set_ptr(bus + AB_MIXER, NULL); }
    }
    return 1;
}
static void *f_lookup(void *table, uint32_t id)
{
    CHECK(table == index_block + AB_TABLE && locked());
    unsigned char *o = registered(id);
    if (o) set_refs(o, refs(o) + 1);
    return o;
}
static void *f_construct(void *memory, uint32_t id)
{
    unsigned char *bus = memory;
    memset(bus, 0, AB_SIZE); ab_set_ptr(bus, normal_vtable); ab_set_ptr(bus + AB_CONSUMER, consumer_vtable);
    memcpy(bus + AB_ID, &id, 4); set_refs(bus, 1);
    bus[AB_FLAGS_A] = 0x20; bus[AB_FLAGS_B] = 0x01; bus[AB_FLAGS_C] = 0x30;
    uint32_t floor_volume = 0xc2c0999a, limit = 100; memcpy(bus + AB_FLOOR, &floor_volume, 4); memcpy(bus + 0xdc, &limit, 4);
    return bus;
}
static int f_initialize(void *bus)
{
    if (init_fail) return 2;
    CHECK(object_count < 64); objects[object_count++] = bus; return 1;
}
static void *f_create_aux(uint32_t id)
{
    unsigned char *bus = allocate(pool, AB_SIZE);
    if (!bus) return NULL;
    f_construct(bus, id); ab_set_ptr(bus, aux_vtable);
    if (f_initialize(bus) != 1) { f_release(bus); return NULL; }
    return bus;
}
static unsigned char *effects_table(unsigned char *bus)
{
    unsigned char *t = ab_ptr(bus + AB_EFFECTS);
    if (!t && (t = allocate(pool, 0x28))) ab_set_ptr(bus + AB_EFFECTS, t);
    return t;
}
static int f_set_effect(void *b, uint32_t slot, uint32_t id, char shared, int priority)
{
    unsigned char *bus = b, *t;
    if (slot >= 4) return 0x1f;
    if (!(t = effects_table(bus))) return 0x34;
    int stored; memcpy(&stored, t, 4);
    if (stored <= priority) {
        memcpy(t, &priority, 4);
        if (t[9 + slot * 8] != (unsigned char)shared || ab_u32(t + 4 + slot * 8) != id) {
            t[9 + slot * 8] = (unsigned char)shared; memcpy(t + 4 + slot * 8, &id, 4); effect_notices++;
        }
    }
    return 1;
}
static int f_set_bypass(void *b, uint32_t bits, uint32_t mask)
{
    unsigned char *bus = b, *t = ab_ptr(bus + AB_EFFECTS);
    if (!t) { if (!bits) return 1; if (!(t = effects_table(bus))) return 0x34; }
    t[0x24] = (unsigned char)((~mask & t[0x24]) | (bits & mask)); effect_notices++; return 1;
}
static void f_kill(void *b, unsigned char v) { unsigned char *bus = b; bus[AB_FLAGS_A] = (unsigned char)((bus[AB_FLAGS_A] & ~1) | (v & 1)); }
static void f_virtual(void *b, unsigned char v) { unsigned char *bus = b; bus[AB_FLAGS_A] = (unsigned char)((bus[AB_FLAGS_A] & ~2) | (v & 1) << 1); }
static void f_channels(void *b, uint32_t v) { uint32_t stored = ab_channels_stored(v); memcpy((unsigned char *)b + AB_CHANNELS, &stored, 4); }
static void f_add_background(void *b) { ((unsigned char *)b)[AB_FLAGS_C] |= 0x80; }
static void f_remove_background(void *b) { ((unsigned char *)b)[AB_FLAGS_C] &= 0x7f; }
static int f_set_duck(void *b, uint32_t target, uint32_t volume, uint32_t fade_out, uint32_t fade_in, uint32_t curve, int property)
{
    unsigned char *bus = b, *entry = ab_duck(bus, target, NULL);
    if (!entry) {
        unsigned char *free_entry = ab_ptr(bus + AB_DUCK_FREE);
        if (!free_entry) {
            if (ab_u32(bus + AB_DUCK_COUNT) >= ab_u32(bus + 0xdc) || !(free_entry = allocate(pool, 0x20))) return 2;
        } else ab_set_ptr(bus + AB_DUCK_FREE, ab_ptr(free_entry));
        unsigned char *tail = ab_ptr(bus + AB_DUCK_TAIL);
        if (tail) ab_set_ptr(tail, free_entry); else ab_set_ptr(bus + AB_DUCK_HEAD, free_entry);
        ab_set_ptr(bus + AB_DUCK_TAIL, free_entry); ab_set_ptr(free_entry, NULL);
        uint32_t count = ab_u32(bus + AB_DUCK_COUNT) + 1; memcpy(bus + AB_DUCK_COUNT, &count, 4);
        memcpy(free_entry + 8, &target, 4); entry = free_entry;
    }
    uint32_t values[5] = {volume, fade_out, fade_in, curve, (uint32_t)property};
    memcpy(entry + 0xc, values, sizeof(values)); return 1;
}
static char f_rtpc_ready(void *consumer)
{
    unsigned char *c = consumer;
    if (ab_ptr(c + 8)) return 1;
    unsigned char *masks = allocate(pool, 0x20);
    if (!masks) return 0;
    memset(masks + 8, 0xff, 8); ab_set_ptr(c + 8, masks); return 1;
}
static int f_rtpc_mark(void *consumer, int parameter)
{
    unsigned char *masks = ab_ptr((unsigned char *)consumer + 8);
    if (!masks) return 2;
    uint64_t bits; memcpy(&bits, masks, 8); bits |= 1ull << (parameter & 63); memcpy(masks, &bits, 8); return 1;
}
/* State manager: known groups 7 and 8, each listing its per-object groups. */
typedef struct state_entry { uint32_t id, padding; unsigned char *list; } state_entry;
static state_entry state_groups[2] = {{7, 0, NULL}, {8, 0, NULL}};
static struct { state_entry *items; uint32_t count, capacity; } state_manager = {state_groups, 2, 2};
static void *state_slot = &state_manager, *transition_slot = &state_manager;
typedef struct state_object { void **vtable; uint32_t id, refs; unsigned char pad[0x10]; void *owner; } state_object;
static state_object state_objects[4];
static state_object *state_find(uint32_t id) { for (int i = 0; i < 4; i++) if (state_objects[i].id == id) return state_objects + i; return NULL; }
static state_entry *manager_entry(uint32_t id) { for (uint32_t i = 0; i < 2; i++) if (state_groups[i].id == id) return state_groups + i; return NULL; }
static void *f_state_group(void *b, uint32_t id, char notify)
{
    unsigned char *bus = b, *g = ab_group(bus, id);
    if (g) return g;
    state_entry *entry = manager_entry(id);
    if (!(g = allocate(pool, 0x48))) return NULL;
    if (!entry) { release(pool, g); return NULL; }
    ab_set_ptr(g + 0x18, bus); memcpy(g + AG_ID, &id, 4);
    ab_set_ptr(g + 8, entry->list); entry->list = g;
    ab_set_ptr(g + AG_NEXT, ab_ptr(bus + AB_GROUPS)); ab_set_ptr(bus + AB_GROUPS, g);
    if (notify) f_states_changed(bus);
    return g;
}
static void *f_state_unbind(void *group, uint32_t value)
{
    unsigned char *g = group, *items = ab_ptr(g + AG_MAPPINGS); uint32_t count = ab_u32(g + AG_MAPPINGS + 8);
    for (uint32_t i = 0; i < count; i++) if (ab_u32(items + 0x18 * i) == value) {
        state_object *o = ab_ptr(items + 0x18 * i + 8); o->owner = NULL; o->refs--;
        memmove(items + 0x18 * i, items + 0x18 * (i + 1), 0x18 * (count - i - 1)); count--; memcpy(g + AG_MAPPINGS + 8, &count, 4);
        return items;
    }
    return NULL;
}
static int f_state_bind(void *group, uint32_t object, uint32_t value, char notify)
{
    unsigned char *g = group; state_object *o = state_find(object);
    if (!o) return 0x10;
    unsigned char *entry = ab_mapping_entry(g, value);
    if (entry && ab_ptr(entry + 8) == o) return 1;
    if (entry) f_state_unbind(g, value);
    uint32_t count = ab_u32(g + AG_MAPPINGS + 8), capacity = ab_u32(g + AG_MAPPINGS + 12);
    unsigned char *items = ab_ptr(g + AG_MAPPINGS);
    if (count == capacity) {
        unsigned char *grown = allocate(pool, 0x18 * (capacity + 2));
        if (!grown) return 0x34;
        if (count) memcpy(grown, items, 0x18 * count);
        if (items) release(pool, items);
        items = grown; ab_set_ptr(g + AG_MAPPINGS, items); capacity += 2; memcpy(g + AG_MAPPINGS + 12, &capacity, 4);
    }
    unsigned char *at = items + 0x18 * count;
    memset(at, 0, 0x18); memcpy(at, &value, 4); ab_set_ptr(at + 8, o); memcpy(at + 0x10, &object, 4);
    count++; memcpy(g + AG_MAPPINGS + 8, &count, 4); o->refs++; o->owner = ab_ptr(g + 0x18);
    if (notify) f_states_changed(ab_ptr(g + 0x18));
    return 1;
}
static int f_state_unregister(void *m, uint32_t id, void *group)
{
    CHECK(m == &state_manager);
    state_entry *entry = manager_entry(id);
    if (!entry) return 0x14;
    unsigned char **link = &entry->list;
    while (*link && *link != group) link = (unsigned char **)(*link + 8);
    if (*link) *link = ab_ptr((unsigned char *)group + 8);
    return 1;
}
static int f_transition_unregister(void *m, void *e, void *g) { (void)m; (void)e; (void)g; CHECK(0); return 0x1d; }
static void destroy_groups(unsigned char *bus)
{
    for (unsigned char *g = ab_ptr(bus + AB_GROUPS), *next; g; g = next) {
        next = ab_ptr(g + AG_NEXT);
        while (ab_u32(g + AG_MAPPINGS + 8)) f_state_unbind(g, ab_u32(ab_ptr(g + AG_MAPPINGS)));
        f_state_unregister(&state_manager, ab_u32(g + AG_ID), g);
        if (ab_ptr(g + AG_MAPPINGS)) release(pool, ab_ptr(g + AG_MAPPINGS));
        release(pool, g);
    }
    ab_set_ptr(bus + AB_GROUPS, NULL);
}
static void drop_records(unsigned char *bus)
{
    for (unsigned i = 0; i < 193; i++) for (unsigned char **link = &params.records[i]; *link;) {
        unsigned char *r = *link;
        if (as_consumer(r) != bus + AB_CONSUMER) { link = (unsigned char **)(r + 0x10); continue; }
        *link = ab_ptr(r + 0x10); params.total--;
        const sh_audio_parameter_vector *curves = (const sh_audio_parameter_vector *)(r + 0x50);
        for (uint32_t k = 0; k < curves->count; k++) {
            const as_curve *curve = (const as_curve *)curves->items + k;
            sh_audio_parameter_node *n = p_find(&params.table, curve->controller);
            if (n) { remove_item(n->subscribers, r); remove_item(n->subscribers + 1, r); }
            if (curve->descriptor.points) release(pool, curve->descriptor.points);
        }
        if (curves->items) release(pool, curves->items);
        release(pool, r);
    }
}

static sh_audio_parameter_native parameters;
static sh_audio_subscription_native subscriptions;
static sh_audio_bus_native native;
static void bind_fake(void)
{
    param_slot = &params.table; manager = &params;
    parameters = (sh_audio_parameter_native){p_ensure, p_destroy, allocate, release, &pool, &param_slot, &manager, &lock, NULL};
    subscriptions = (sh_audio_subscription_native){&parameters, &modulator_slot, f_reverse_add, f_reverse_remove,
        f_modulator_add, f_modulator_remove, f_refresh, f_set_curve, &switch_slot, f_switch_ensure};
    void **tables[2] = {normal_vtable, aux_vtable};
    for (int i = 0; i < 2; i++) {
        tables[i][1] = (void *)f_addref; tables[i][2] = (void *)f_release; tables[i][0x38 / 8] = (void *)f_set_parent;
        tables[i][0x48 / 8] = (void *)f_attach; tables[i][0x60 / 8] = (void *)f_detach; tables[i][0xe8 / 8] = (void *)f_curve_changed;
        tables[i][0x1b0 / 8] = (void *)f_states_changed; tables[i][0x1c8 / 8] = (void *)f_mixer;
    }
    consumer_vtable[3] = (void *)f_value_changed;
    for (int i = 0; i < AB_PROPERTY_COUNT; i++) property_parameters[i] = i < 6 ? i : 64;
    sh_audio_bus_owned *owned = native.owned; size_t owned_count = native.owned_count;
    uint32_t *touched = native.touched; size_t touched_count = native.touched_count;
    native = (sh_audio_bus_native){&subscriptions, &lock, &manager, &index_value, &state_slot, &transition_slot,
        f_lookup, f_construct, f_initialize, f_create_aux, normal_vtable, aux_vtable, consumer_vtable,
        {(void **)&root_slots[0], (void **)&root_slots[1]}, &sample_rate, &duck_floor, property_parameters,
        f_set_effect, f_set_bypass, f_kill, f_virtual, f_channels, f_add_background, f_remove_background, f_set_duck,
        f_rtpc_ready, f_rtpc_mark, f_state_group, f_state_bind, f_state_unbind, f_state_unregister, f_transition_unregister,
        NULL, owned, owned_count, touched, touched_count};
}
/* Native tree built as the bank parser would: master 1 (root), bus 2 under 1
 * with a property, effect, state mapping and a duck on 3; aux bus 3 under 1. */
static unsigned char *native_bus(uint32_t id, int aux, unsigned char *parent)
{
    unsigned char *bus = aux ? f_create_aux(id) : f_construct(allocate(pool, AB_SIZE), id);
    if (!aux) f_initialize(bus);
    if (parent) { f_addref(bus); CHECK(f_attach(parent, bus) == 1); }
    return bus;
}
static void native_tree(void)
{
    EnterCriticalSection(&lock);
    for (int i = 0; i < 4; i++) state_objects[i] = (state_object){state_object_vtable, (uint32_t[]){700, 701, 702, 703}[i], 1, {0}, NULL};
    unsigned char *master = native_bus(1, 0, NULL); root_slots[0] = master;
    unsigned char *two = native_bus(2, 0, master), *three = native_bus(3, 1, master);
    unsigned char *block = allocate(pool, 8); block[0] = 1; block[1] = 0; float volume = -3.0f; memcpy(block + 4, &volume, 4);
    ab_set_ptr(two + AB_PROPERTIES, block);
    CHECK(f_set_effect(two, 0, 900, 1, 0) == 1);
    unsigned char *group = f_state_group(two, 7, 0); CHECK(group && f_state_bind(group, 700, 70, 0) == 1);
    CHECK(f_set_duck(two, 3, 0xc0000000u, 100, 200, 4, 0) == 1);
    LeaveCriticalSection(&lock);
    value_notices = curve_notices = state_notices = effect_notices = refreshes = 0; calls = 0;
}
static void teardown(void)
{
    CHECK(!native.pending);
    EnterCriticalSection(&lock);
    for (size_t i = 0; i < native.owned_count; i++) f_release(native.owned[i].bus);
    free(native.owned); free(native.touched); native.owned = NULL; native.touched = NULL; native.owned_count = native.touched_count = 0;
    /* Children release their parents: take the leaves first. */
    for (int round = 0; round < 64 && object_count; round++)
        for (size_t i = 0; i < object_count; i++) {
            int parent_of_other = 0;
            for (size_t j = 0; j < object_count; j++) parent_of_other |= ab_ptr(objects[j] + AB_PARENT) == objects[i];
            if (!parent_of_other) { unsigned char *o = objects[i]; set_refs(o, 1); f_release(o); break; }
        }
    for (unsigned i = 0; i < 193; i++) for (sh_audio_parameter_node *n = params.table.buckets[i], *next; n; n = next) {
        next = n->next; p_destroy(n); release(pool, n);
    }
    LeaveCriticalSection(&lock);
    CHECK(!object_count && !allocations && !params.total);
    memset(&params, 0, sizeof(params)); root_slots[0] = root_slots[1] = NULL;
    for (int i = 0; i < 2; i++) state_groups[i].list = NULL;
}

/* ---- policies ---- */
typedef struct built { sh_audio_bus_setting rows[64]; unsigned char bodies[64][48]; size_t count; sh_audio_bus_type buses[8]; size_t bus_count; } built;
static void kind(built *b, uint32_t id, unsigned k) { b->buses[b->bus_count++] = (sh_audio_bus_type){id, k}; }
static sh_audio_bus_setting *row(built *b, uint32_t bus, sh_audio_bus_field field, uint32_t k0, uint32_t k1, int edited,
    const void *body, size_t length)
{
    sh_audio_bus_setting *s = b->rows + b->count; memset(s, 0, sizeof(*s));
    s->bus = bus; s->field.field = field; s->field.key[0] = k0; s->field.key[1] = k1; s->edited = edited;
    if (body) { memcpy(b->bodies[b->count], body, length); s->body = b->bodies[b->count]; s->field.length = length; }
    else s->removed = 1;
    b->count++; return s;
}
static void word_row(built *b, uint32_t bus, sh_audio_bus_field field, uint32_t k0, uint32_t k1, int edited, uint32_t value)
{ row(b, bus, field, k0, k1, edited, &value, 4); }
static void byte_row(built *b, uint32_t bus, sh_audio_bus_field field, uint32_t k0, uint32_t k1, int edited, unsigned char value)
{ row(b, bus, field, k0, k1, edited, &value, 1); }
static void effect_row(built *b, uint32_t bus, sh_audio_bus_field field, uint32_t slot, int edited, uint32_t id, unsigned char shared)
{ unsigned char body[5]; memcpy(body, &id, 4); body[4] = shared; row(b, bus, field, slot, 0, edited, body, 5); }
static void duck_row(built *b, uint32_t bus, uint32_t target, int edited, uint32_t volume, uint32_t out, uint32_t in, unsigned char curve)
{ unsigned char body[14] = {0}; memcpy(body, &volume, 4); memcpy(body + 4, &out, 4); memcpy(body + 8, &in, 4); body[12] = curve; row(b, bus, SH_AUDIO_BUS_FIELD_DUCK, target, 0, edited, body, 14); }
static void settings_rows(built *b, uint32_t bus, int edited)
{
    for (uint32_t bit = 1; bit <= 2; bit <<= 1) byte_row(b, bus, SH_AUDIO_BUS_FIELD_SETTING, 0, bit, edited, 0);
    for (uint32_t bit = 1; bit <= 8; bit <<= 1) byte_row(b, bus, SH_AUDIO_BUS_FIELD_SETTING, 1, bit, edited, 0);
    uint16_t limit = 0; row(b, bus, SH_AUDIO_BUS_FIELD_SETTING, 2, 0x3ff, edited, &limit, 2);
    word_row(b, bus, SH_AUDIO_BUS_FIELD_SETTING, 3, 0, edited, 0);
    for (uint32_t bit = 1; bit <= 2; bit <<= 1) byte_row(b, bus, SH_AUDIO_BUS_FIELD_SETTING, 4, bit, edited, 0);
}
/* The native configuration of the tree above, in policy form. */
static void baseline_rows(built *b)
{
    memset(b, 0, sizeof(*b)); kind(b, 1, 8); kind(b, 2, 8); kind(b, 3, 20);
    float volume = -3.0f, floor_volume = -96.3f; uint32_t fv; memcpy(&fv, &floor_volume, 4);
    uint32_t volume_bits; memcpy(&volume_bits, &volume, 4);
    word_row(b, 1, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 0);
    word_row(b, 2, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 1);
    word_row(b, 2, SH_AUDIO_BUS_FIELD_PROPERTY, 0, 0, 0, volume_bits);
    word_row(b, 2, SH_AUDIO_BUS_FIELD_DUCK_SETTING, 1, 0, 0, fv);
    duck_row(b, 2, 3, 0, 0xc0000000u, 100, 200, 4);
    effect_row(b, 2, SH_AUDIO_BUS_FIELD_EFFECT, 0, 0, 900, 1);
    byte_row(b, 2, SH_AUDIO_BUS_FIELD_STATE_GROUP, 7, 0, 0, 0);
    word_row(b, 2, SH_AUDIO_BUS_FIELD_STATE, 7, 70, 0, 700);
    word_row(b, 3, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 0, 1);
}
static sh_audio_bus_policy view(built *b) { return (sh_audio_bus_policy){b->rows, b->count, b->buses, b->bus_count}; }
/* New normal bus 50 under 1 with every configuration kind, and aux bus 51 under 50. */
static void new_buses(built *b)
{
    kind(b, 50, 8); kind(b, 51, 20);
    word_row(b, 50, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 1, 1);
    word_row(b, 50, SH_AUDIO_BUS_FIELD_PROPERTY, 0, 0, 1, 0x3f000000u);
    word_row(b, 50, SH_AUDIO_BUS_FIELD_PROPERTY, 4, 0, 1, 0x40000000u);
    settings_rows(b, 50, 1);
    b->rows[b->count - 8].body[0] = 1;   /* setting 1 bit 1: instance-limit kill */
    word_row(b, 50, SH_AUDIO_BUS_FIELD_DUCK_SETTING, 0, 0, 1, 500);
    word_row(b, 50, SH_AUDIO_BUS_FIELD_DUCK_SETTING, 1, 0, 1, 0xc1a00000u);
    duck_row(b, 50, 3, 1, 0xc0400000u, 300, 400, 1);
    byte_row(b, 50, SH_AUDIO_BUS_FIELD_BYPASS, 0, 2, 1, 2);
    effect_row(b, 50, SH_AUDIO_BUS_FIELD_EFFECT, 1, 1, 901, 0);
    effect_row(b, 50, SH_AUDIO_BUS_FIELD_MIXER, 0, 1, 950, 1);
    byte_row(b, 50, SH_AUDIO_BUS_FIELD_FLAGS, 0, 1, 1, 1);
    byte_row(b, 50, SH_AUDIO_BUS_FIELD_RTPC_MODE, 0x18, 0, 1, 1);
    unsigned char curve[14 + 24] = {0}; uint32_t controller = 30, id = 5; float x1 = 100, y1 = 1;
    memcpy(curve, &controller, 4); curve[6] = 0x18; memcpy(curve + 7, &id, 4); curve[12] = 2;
    memcpy(curve + 26, &x1, 4); memcpy(curve + 30, &y1, 4);
    row(b, 50, SH_AUDIO_BUS_FIELD_RTPC_CURVE, 0x18, 5, 1, curve, sizeof(curve));
    byte_row(b, 50, SH_AUDIO_BUS_FIELD_STATE_GROUP, 8, 0, 1, 1);
    word_row(b, 50, SH_AUDIO_BUS_FIELD_STATE, 8, 80, 1, 701);
    word_row(b, 51, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 1, 50);
}
static int compose(built *base, built *program, sh_audio_bus_policy *out)
{
    char error[256]; sh_audio_bus_policy b = view(base), p = view(program); const sh_audio_bus_policy *views[1] = {&p};
    int ok = sh_audio_effective_bus(&b, program ? views : NULL, program ? 1 : 0, out, error, sizeof(error));
    if (!ok) fprintf(stderr, "compose: %s\n", error);
    return ok;
}
static int publish(const sh_audio_bus_policy *effective, built *base, char *error)
{
    sh_audio_bus_change change = {0}; sh_audio_bus_policy b = view(base);
    int result = sh_audio_bus_native_identities(&native, effective, &b, 0, &change, error, 256);
    if (result != 1) return result;
    if ((result = sh_audio_bus_native_apply(&change, error, 256)) != 1) return result;
    if (!change.owner) return 1;
    CHECK(sh_audio_bus_native_ready(&change, error, 256));
    sh_audio_bus_native_accept(&change);
    CHECK(sh_audio_bus_native_release(&change, error, 256) && !native.pending && !change.owner);
    return 1;
}
/* Observable configuration: identities, parents and every owned field. */
typedef struct print { unsigned char bytes[32768]; size_t length; } print;
static void put(print *p, const void *data, size_t n)
{ CHECK(p->length + n <= sizeof(p->bytes)); if (p->length + n <= sizeof(p->bytes)) { memcpy(p->bytes + p->length, data, n); p->length += n; } }
static void fingerprint(print *p)
{
    p->length = 0; put(p, &object_count, sizeof(object_count));
    for (uint32_t id = 0; id < 64; id++) {
        unsigned char *bus = registered(id);
        if (!bus) continue;
        put(p, &id, 4); put(p, &bus, sizeof(bus)); put(p, bus + 0x14, 4); put(p, bus + AB_PARENT, 8);
        put(p, bus + AB_LIMIT, 4); put(p, bus + AB_FLAGS_C, 1); put(p, bus + AB_RECOVERY, 12);
        unsigned char *block = ab_ptr(bus + AB_PROPERTIES);
        if (block) put(p, block, ((block[0] + 4) & ~3u) + block[0] * 4u);
        unsigned char *t = ab_ptr(bus + AB_EFFECTS); if (t) put(p, t + 4, 0x21);
        unsigned char *m = ab_ptr(bus + AB_MIXER); if (m) put(p, m + 4, 6);
        for (unsigned char *d = ab_ptr(bus + AB_DUCK_HEAD); d; d = ab_ptr(d)) put(p, d + 8, 24);
        put(p, bus + AB_DUCK_COUNT, 4);
        for (unsigned char *g = ab_ptr(bus + AB_GROUPS); g; g = ab_ptr(g + AG_NEXT)) {
            put(p, g + AG_ID, 4); put(p, g + AG_SYNC, 1);
            for (uint32_t i = 0; i < ab_u32(g + AG_MAPPINGS + 8); i++) { unsigned char *e = (unsigned char *)ab_ptr(g + AG_MAPPINGS) + 0x18 * i; put(p, e, 4); put(p, e + 0x10, 4); }
        }
    }
    put(p, &params.total, 4);
    for (int i = 0; i < 4; i++) put(p, &state_objects[i].refs, 4);
}
static int same(const print *a, const print *b) { return a->length == b->length && !memcmp(a->bytes, b->bytes, a->length); }

static void test_new_buses(void)
{
    char error[256] = {0}; built base, program; sh_audio_bus_policy e;
    bind_fake(); native_tree(); baseline_rows(&base); baseline_rows(&program); new_buses(&program);
    CHECK(compose(&base, &program, &e));
    CHECK(publish(&e, &base, error) == 1);
    unsigned char *fifty = registered(50), *fifty_one = registered(51);
    CHECK(fifty && fifty_one && ab_ptr(fifty + AB_PARENT) == registered(1) && ab_ptr(fifty_one + AB_PARENT) == fifty);
    CHECK(*(void **)fifty == normal_vtable && *(void **)fifty_one == aux_vtable);
    unsigned char *block = ab_ptr(fifty + AB_PROPERTIES);
    CHECK(block && block[0] == 2 && ab_u32(block + 4) == 0x3f000000u && ab_u32(block + 8) == 0x40000000u);
    unsigned char *t = ab_ptr(fifty + AB_EFFECTS); CHECK(t && ab_u32(t + 12) == 901 && t[17] == 0 && t[0x24] == 2);
    unsigned char *m = ab_ptr(fifty + AB_MIXER); CHECK(m && ab_u32(m + 4) == 950);
    CHECK((fifty[AB_FLAGS_A] & 0x11) == 0x11 && ab_u32(fifty + AB_RECOVERY) == 48 * 500 && ab_u32(fifty + AB_FLOOR) == 0xc1a00000u);
    CHECK(ab_duck(fifty, 3, NULL) && ab_u32(fifty + AB_DUCK_COUNT) == 1);
    unsigned char *g = ab_group(fifty, 8); CHECK(g && g[AG_SYNC] == 1 && ab_mapping_entry(g, 80) && ab_u32(ab_mapping_entry(g, 80) + 0x10) == 701);
    CHECK(params.total == 1 && sh_audio_subscription_live(&subscriptions, fifty + AB_CONSUMER, 0x18, NULL) == 1);
    CHECK(curve_notices == 1 && last_curve_parameter == 0x18 && value_notices >= 2);
    CHECK(native.owned_count == 2 && !native.touched_count && refs(fifty) == 2 && refs(fifty_one) == 1);
    /* Publishing the same policy again is a no-op. */
    print a, b; fingerprint(&a); int before = calls, notices = value_notices + curve_notices + effect_notices + state_notices;
    CHECK(publish(&e, &base, error) == 1);
    fingerprint(&b); CHECK(same(&a, &b) && calls == before && notices == value_notices + curve_notices + effect_notices + state_notices);
    /* The new buses leave the policy: released after commit, destroyed natively. */
    sh_audio_bus_policy_free(&e); CHECK(compose(&base, NULL, &e));
    CHECK(publish(&e, &base, error) == 1 && !native.owned_count && !registered(50) && !registered(51));
    sh_audio_bus_policy_free(&e); teardown();
}
/* Edits of a native bus, their recovery and the return to baseline. */
static void test_native_edits(void)
{
    char error[256] = {0}; built base, program; sh_audio_bus_policy e; print original, now;
    bind_fake(); native_tree(); baseline_rows(&base); fingerprint(&original);
    baseline_rows(&program);
    for (size_t i = 0; i < program.count; i++) {
        sh_audio_bus_setting *s = program.rows + i;
        if (s->bus != 2) continue;
        if (s->field.field == SH_AUDIO_BUS_FIELD_PROPERTY) { uint32_t v = 0x3f800000u; memcpy(s->body, &v, 4); s->edited = 1; }
        if (s->field.field == SH_AUDIO_BUS_FIELD_EFFECT) { uint32_t v = 902; memcpy(s->body, &v, 4); s->edited = 1; }
        if (s->field.field == SH_AUDIO_BUS_FIELD_STATE) { uint32_t v = 701; memcpy(s->body, &v, 4); s->edited = 1; }
        if (s->field.field == SH_AUDIO_BUS_FIELD_DUCK) { s->removed = s->edited = 1; s->body = NULL; s->field.length = 0; }
    }
    byte_row(&program, 2, SH_AUDIO_BUS_FIELD_STATE_GROUP, 8, 0, 1, 2);
    word_row(&program, 2, SH_AUDIO_BUS_FIELD_STATE, 8, 81, 1, 702);
    effect_row(&program, 2, SH_AUDIO_BUS_FIELD_MIXER, 0, 1, 951, 1);
    word_row(&program, 3, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 1, 2); /* aux 3 moves under 2 */
    CHECK(compose(&base, &program, &e));
    /* A later leg's failure restores everything exactly. */
    sh_audio_bus_change change = {0}; sh_audio_bus_policy b = view(&base);
    CHECK(sh_audio_bus_native_identities(&native, &e, &b, 0, &change, error, sizeof(error)) == 1);
    CHECK(ab_ptr(registered(3) + AB_PARENT) == registered(2));
    CHECK(sh_audio_bus_native_apply(&change, error, sizeof(error)) == 1 && sh_audio_bus_native_ready(&change, error, sizeof(error)));
    unsigned char *two = registered(2);
    CHECK(ab_u32((unsigned char *)ab_ptr(two + AB_PROPERTIES) + 4) == 0x3f800000u && last_value_parameter == 0 && value_notices >= 1);
    CHECK(ab_u32((unsigned char *)ab_ptr(two + AB_EFFECTS) + 4) == 902 && !ab_duck(two, 3, NULL));
    CHECK(ab_u32(ab_mapping_entry(ab_group(two, 7), 70) + 0x10) == 701 && ab_mapping_entry(ab_group(two, 8), 81));
    CHECK(ab_ptr(two + AB_MIXER) && ab_u32((unsigned char *)ab_ptr(two + AB_MIXER) + 4) == 951);
    CHECK(sh_audio_bus_native_restore(&change, error, sizeof(error)) && !native.pending && !change.owner);
    fingerprint(&now); CHECK(same(&original, &now) && !ab_group(two, 8) && !native.touched_count);
    /* Accepted edits mark the bus touched; an empty policy restores it. */
    CHECK(publish(&e, &base, error) == 1 && native.touched_count == 2);
    sh_audio_bus_policy_free(&e); CHECK(compose(&base, NULL, &e));
    CHECK(publish(&e, &base, error) == 1 && !native.touched_count);
    fingerprint(&now); CHECK(same(&original, &now));
    sh_audio_bus_policy_free(&e); teardown();
}
/* Every allocation failure during both phases leaves the engine unchanged. */
static void test_allocation_failures(void)
{
    char error[256]; int completed = 0;
    for (int at = 1; at < 200 && !completed; at++) {
        built base, program; sh_audio_bus_policy e; print a, b;
        bind_fake(); native_tree(); baseline_rows(&base); baseline_rows(&program); new_buses(&program);
        for (size_t i = 0; i < program.count; i++) if (program.rows[i].bus == 2 && program.rows[i].field.field == SH_AUDIO_BUS_FIELD_PROPERTY) {
            uint32_t v = 0x3f800000u; memcpy(program.rows[i].body, &v, 4); program.rows[i].edited = 1;
        }
        CHECK(compose(&base, &program, &e));
        fingerprint(&a); int held = allocations;
        calls = 0; fail_at = at; error[0] = 0;
        int result = publish(&e, &base, error);
        fail_at = 0;
        if (result == 1) completed = at;
        else { CHECK(result == 0 && !native.pending); fingerprint(&b); CHECK(same(&a, &b) && allocations == held && !native.owned_count); }
        sh_audio_bus_policy_free(&e); teardown();
    }
    CHECK(completed > 10);
    printf("bus publication: %d allocation failure points reverted\n", completed - 1);
}
static void test_refusals(void)
{
    char error[256] = {0}; built base, program; sh_audio_bus_policy e; print a, b;
    bind_fake(); native_tree(); baseline_rows(&base); fingerprint(&a);
    /* A new root bus has no native root slot to occupy. */
    baseline_rows(&program); kind(&program, 60, 8); word_row(&program, 60, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 1, 0);
    CHECK(compose(&base, &program, &e) && publish(&e, &base, error) == 0 && strstr(error, "root"));
    fingerprint(&b); CHECK(same(&a, &b)); sh_audio_bus_policy_free(&e);
    /* A missing parent is not invented. */
    error[0] = 0; baseline_rows(&program); kind(&program, 61, 8); word_row(&program, 61, SH_AUDIO_BUS_FIELD_PARENT, 0, 0, 1, 99);
    CHECK(compose(&base, &program, &e) && publish(&e, &base, error) == 0 && strstr(error, "not loaded"));
    fingerprint(&b); CHECK(same(&a, &b)); sh_audio_bus_policy_free(&e);
    /* A live bus of the other kind is never converted. */
    error[0] = 0; baseline_rows(&program); program.buses[2].kind = 8;
    word_row(&program, 3, SH_AUDIO_BUS_FIELD_PROPERTY, 0, 0, 1, 0x3f800000u);
    built mixed = base; mixed.buses[2].kind = 8;
    CHECK(compose(&mixed, &program, &e) && publish(&e, &mixed, error) == 0 && strstr(error, "kind"));
    fingerprint(&b); CHECK(same(&a, &b)); sh_audio_bus_policy_free(&e);
    /* A failed native construction reports and changes nothing. */
    error[0] = 0; baseline_rows(&program); new_buses(&program); init_fail = 1;
    CHECK(compose(&base, &program, &e) && publish(&e, &base, error) == 0 && strstr(error, "construct"));
    init_fail = 0; fingerprint(&b); CHECK(same(&a, &b)); sh_audio_bus_policy_free(&e);
    teardown();
}
int main(void)
{
    InitializeCriticalSection(&lock);
    test_new_buses();
    test_native_edits();
    test_allocation_failures();
    test_refusals();
    DeleteCriticalSection(&lock);
    if (failures) { fprintf(stderr, "%d audio bus checks failed\n", failures); return 1; }
    puts("audio bus native checks passed");
    return 0;
}
