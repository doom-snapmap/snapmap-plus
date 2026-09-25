#include "audio_bus_native.h"
#include "audio_native_image.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Native bus layout (see the header). */
#define AB_SIZE 0x168
#define AB_ID 0x10
#define AB_CONSUMER 0x18
#define AB_MASKS 0x20
#define AB_GROUPS 0x30
#define AB_EFFECTS 0x50
#define AB_PARENT 0x68
#define AB_PROPERTIES 0x70
#define AB_LIMIT 0x78
#define AB_FLAGS_A 0x7a
#define AB_FLAGS_B 0x7b
#define AB_MIXER 0x98
#define AB_RECOVERY 0xb0
#define AB_CHANNELS 0xb4
#define AB_FLOOR 0xb8
#define AB_DUCK_HEAD 0xc0
#define AB_DUCK_TAIL 0xc8
#define AB_DUCK_FREE 0xd0
#define AB_DUCK_BLOCKS 0xd8
#define AB_DUCK_COUNT 0xe0
#define AB_DUCK_BASE 0xe8
#define AB_FLAGS_C 0x160
#define AB_TABLE 0x638          /* bus objects occupy the second index table */
#define AB_PROPERTY_COUNT 59    /* entries of the native property-to-curve table */
/* State group objects: owner +0x18, transition bundle +0x20, mappings +0x28
 * (0x18-byte entries: value +0, object +8, object ID +0x10), ID +0x38, sync
 * +0x40, next on the bus +0x10. */
#define AG_NEXT 0x10
#define AG_TRANSITIONS 0x20
#define AG_MAPPINGS 0x28
#define AG_ID 0x38
#define AG_SYNC 0x40

enum { AB_IDENTITIES = 1, AB_CONFIGURED, AB_ACCEPTED };
enum { AB_OP_CREATE = 1, AB_OP_ADOPT, AB_OP_PARENT, AB_OP_BYTE, AB_OP_HALF, AB_OP_WORD, AB_OP_PROPERTIES,
    AB_OP_KILL, AB_OP_VIRTUAL, AB_OP_CHANNELS, AB_OP_BACKGROUND, AB_OP_EFFECT, AB_OP_BYPASS, AB_OP_MIXER,
    AB_OP_DUCK, AB_OP_GROUP_ADD, AB_OP_GROUP_REMOVE, AB_OP_SYNC, AB_OP_MAP };
typedef struct ab_mapping { uint32_t value, object; } ab_mapping;
struct sh_audio_bus_op {
    int kind;
    unsigned char *bus;
    void *object, *previous;   /* parent, property block or state group */
    size_t offset;
    uint32_t key, mask, before, after, values[5], after_values[5];
    int existed, present;      /* duck/mapping state before and after */
    ab_mapping *mappings;      /* a removed group's mappings */
    size_t mapping_count;
};
typedef struct sh_audio_bus_op ab_op;
struct sh_audio_bus_target {
    uint32_t id;
    unsigned kind;
    int native;
    unsigned char *bus;
    size_t first, count;
};
typedef struct sh_audio_bus_target ab_target;

static int ab_fail(char *error, size_t capacity, const char *message)
{ if (error && capacity && !*error) snprintf(error, capacity, "%s", message); return 0; }
static int ab_failf(char *error, size_t capacity, const char *format, uint32_t value)
{ if (error && capacity && !*error) snprintf(error, capacity, format, (unsigned)value); return 0; }
static uint32_t ab_u32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void **ab_vt(const void *object) { return *(void ***)object; }
static void ab_addref(void *o) { ((int (*)(void *))ab_vt(o)[1])(o); }
static void ab_release(void *o) { ((int (*)(void *))ab_vt(o)[2])(o); }
static int ab_attach(void *parent, void *child) { return ((int (*)(void *, void *))ab_vt(parent)[0x48 / 8])(parent, child); }
static void ab_detach(void *parent, void *child) { ((void (*)(void *, void *))ab_vt(parent)[0x60 / 8])(parent, child); }
static void ab_curve_changed(void *bus, uint32_t parameter) { ((void (*)(void *, uint32_t))ab_vt(bus)[0xe8 / 8])(bus, parameter); }
static void ab_states_changed(void *owner) { ((void (*)(void *))ab_vt(owner)[0x1b0 / 8])(owner); }
static int ab_set_mixer(void *bus, uint32_t id, char shared) { return ((int (*)(void *, uint32_t, char, int))ab_vt(bus)[0x1c8 / 8])(bus, id, shared, 0); }
/* The same notification a curve value change sends to active instances. */
static void ab_value_changed(unsigned char *bus, uint32_t parameter)
{ void *consumer = bus + AB_CONSUMER; ((void (*)(void *, char, uint32_t))ab_vt(consumer)[3])(consumer, 1, parameter); }
static void *ab_ptr(const unsigned char *at) { void *p; memcpy(&p, at, sizeof(p)); return p; }
static void ab_set_ptr(unsigned char *at, const void *p) { memcpy(at, &p, sizeof(p)); }

static int ab_push(sh_audio_bus_change *c, const ab_op *op)
{
    if (c->op_count == c->op_capacity) {
        size_t next = c->op_capacity ? c->op_capacity * 2 : 32;
        if (next > SIZE_MAX / sizeof(*c->ops)) return 0;
        ab_op *grown = realloc(c->ops, next * sizeof(*grown));
        if (!grown) return 0;
        c->ops = grown; c->op_capacity = next;
    }
    c->ops[c->op_count++] = *op; return 1;
}
static const sh_audio_bus_setting *ab_row(const sh_audio_bus_change *c, const ab_target *t,
    sh_audio_bus_field field, uint32_t key0, uint32_t key1)
{
    for (size_t i = t->first; i < t->first + t->count; i++) {
        const sh_audio_bus_setting *s = c->desired->settings + i;
        if (s->field.field == field && s->field.key[0] == key0 && s->field.key[1] == key1) return s;
    }
    return NULL;
}
static const sh_audio_bus_setting *ab_live_row(const sh_audio_bus_change *c, const ab_target *t,
    sh_audio_bus_field field, uint32_t key0, uint32_t key1)
{ const sh_audio_bus_setting *s = ab_row(c, t, field, key0, key1); return s && !s->removed ? s : NULL; }
static int ab_owned(const sh_audio_bus_native *n, uint32_t id)
{ for (size_t i = 0; i < n->owned_count; i++) if (n->owned[i].id == id) return 1; return 0; }
static int ab_touched(const sh_audio_bus_native *n, uint32_t id)
{ for (size_t i = 0; i < n->touched_count; i++) if (n->touched[i] == id) return 1; return 0; }
static unsigned ab_kind(const sh_audio_bus_policy *p, uint32_t id)
{ for (size_t i = 0; p && i < p->bus_count; i++) if (p->buses[i].bus == id) return p->buses[i].kind; return 0; }
static int ab_alive(const sh_audio_bus_change *c)
{
    const sh_audio_bus_native *n = c->owner;
    return c->manager && *n->manager_slot == c->manager && *n->index_slot == c->index &&
        *n->subscriptions->parameters->pool == c->pool;
}
static void *ab_lookup(sh_audio_bus_change *c, uint32_t id) { return c->owner->lookup(c->index + AB_TABLE, id); }

/* ---- identities ---- */
static int ab_plan(sh_audio_bus_change *c, const sh_audio_bus_policy *baseline)
{
    const sh_audio_bus_policy *d = c->desired;
    if (!(c->targets = calloc(d->bus_count ? d->bus_count : 1, sizeof(*c->targets)))) return 0;
    for (size_t b = 0; b < d->bus_count; b++) {
        ab_target t = {d->buses[b].bus, d->buses[b].kind, ab_kind(baseline, d->buses[b].bus) != 0, NULL, 0, 0};
        size_t i = 0;
        while (i < d->count && d->settings[i].bus < t.id) i++;
        t.first = i;
        int edited = 0;
        while (i < d->count && d->settings[i].bus == t.id) edited |= d->settings[i++].edited;
        t.count = i - t.first;
        if (t.native && (c->new_only || (!edited && !ab_touched(c->owner, t.id)))) continue;
        c->targets[c->target_count++] = t;
    }
    return 1;
}
static void *ab_create(sh_audio_bus_change *c, const ab_target *t)
{
    sh_audio_bus_native *n = c->owner;
    if (t->kind == 20) return n->create_aux(t->id);
    void *memory = n->subscriptions->parameters->allocate(c->pool, AB_SIZE);
    if (!memory) return NULL;
    void *bus = n->construct(memory, t->id);
    if (!bus) { n->subscriptions->parameters->release(c->pool, memory); return NULL; }
    if (n->initialize(bus) != 1) { ab_release(bus); return NULL; }
    return bus;
}
static int ab_identity(sh_audio_bus_change *c, ab_target *t, char *error, size_t capacity)
{
    sh_audio_bus_native *n = c->owner; ab_op op = {0};
    void *live = ab_lookup(c, t->id);
    const void *vtable = t->kind == 20 ? n->aux_vtable : n->normal_vtable;
    if (live) {
        if (*(void **)live != vtable) { ab_release(live); return ab_failf(error, capacity, "live audio bus %u has a different kind", t->id); }
        t->bus = live;
        /* Native buses live with their banks; owned ones hold our reference. */
        if (t->native || ab_owned(n, t->id)) { ab_release(live); return 1; }
        op.kind = AB_OP_ADOPT; op.bus = live;
        if (!ab_push(c, &op)) { ab_release(live); return ab_fail(error, capacity, "cannot record audio bus ownership"); }
        return 1;
    }
    if (t->native) return ab_failf(error, capacity, "native audio bus %u is not loaded", t->id);
    if (!(t->bus = ab_create(c, t))) return ab_failf(error, capacity, "cannot construct audio bus %u", t->id);
    op.kind = AB_OP_CREATE; op.bus = t->bus;
    if (!ab_push(c, &op)) { ab_release(t->bus); t->bus = NULL; return ab_fail(error, capacity, "cannot record audio bus ownership"); }
    return 1;
}
static uint32_t ab_parent_id(const unsigned char *bus)
{ const unsigned char *parent = ab_ptr(bus + AB_PARENT); return parent ? ab_u32(parent + AB_ID) : 0; }
static int ab_is_ancestor(const unsigned char *bus, const unsigned char *of)
{
    size_t steps = 0;
    for (const unsigned char *at = of; at && steps < 4096; at = ab_ptr(at + AB_PARENT), steps++) if (at == bus) return 1;
    return 0;
}
/* 1 attached (or unchanged), 0 deferred (would close a cycle now), -1 failed. */
static int ab_parent(sh_audio_bus_change *c, ab_target *t, char *error, size_t capacity)
{
    const sh_audio_bus_setting *row = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_PARENT, 0, 0);
    if (!row || row->field.length != 4) { ab_failf(error, capacity, "audio bus %u has no parent", t->id); return -1; }
    uint32_t want = ab_u32(row->body);
    unsigned char *old = ab_ptr(t->bus + AB_PARENT);
    /* The engine has two root slots, both held by native roots. */
    if (!want) {
        if (!old && (*c->owner->roots[0] == t->bus || *c->owner->roots[1] == t->bus)) return 1;
        ab_failf(error, capacity, "audio bus %u cannot become a root bus", t->id); return -1;
    }
    if (want == ab_parent_id(t->bus)) return 1;
    unsigned char *parent = ab_lookup(c, want);
    if (!parent) { ab_failf(error, capacity, "parent audio bus %u is not loaded", want); return -1; }
    if (ab_is_ancestor(t->bus, parent)) { ab_release(parent); return 0; }
    if (old) ab_detach(old, t->bus);
    ab_addref(t->bus);
    if (ab_attach(parent, t->bus) != 1) {
        if (old) { ab_addref(t->bus); (void)ab_attach(old, t->bus); }
        ab_release(parent); ab_failf(error, capacity, "cannot attach audio bus %u to its parent", t->id); return -1;
    }
    ab_op op = {0}; op.kind = AB_OP_PARENT; op.bus = t->bus; op.object = parent; op.previous = old;
    ab_release(parent);
    if (!ab_push(c, &op)) { c->uncertain = 1; ab_fail(error, capacity, "cannot record audio bus parent"); return -1; }
    return 1;
}
static int ab_parents(sh_audio_bus_change *c, char *error, size_t capacity)
{
    unsigned char *done = calloc(c->target_count ? c->target_count : 1, 1);
    if (!done) return ab_fail(error, capacity, "cannot plan audio bus parents");
    for (size_t remaining = c->target_count; remaining;) {
        size_t progress = 0;
        for (size_t i = 0; i < c->target_count; i++) if (!done[i]) {
            int r = ab_parent(c, c->targets + i, error, capacity);
            if (r < 0) { free(done); return 0; }
            if (r) { done[i] = 1; progress++; remaining--; }
        }
        if (!progress) { free(done); return ab_fail(error, capacity, "audio bus parents cannot be attached without a cycle"); }
    }
    free(done); return 1;
}

/* ---- configuration ---- */
static int ab_byte(sh_audio_bus_change *c, unsigned char *bus, size_t offset, unsigned char bit, int want)
{
    int have = (bus[offset] & bit) != 0;
    if (have == !!want) return 1;
    ab_op op = {0}; op.kind = AB_OP_BYTE; op.bus = bus; op.offset = offset; op.mask = bit; op.before = bus[offset];
    op.after = want ? bit : 0;
    if (!ab_push(c, &op)) return 0;
    bus[offset] = (unsigned char)((bus[offset] & ~bit) | (want ? bit : 0)); return 1;
}
static int ab_word(sh_audio_bus_change *c, unsigned char *bus, size_t offset, uint32_t want)
{
    uint32_t have = ab_u32(bus + offset);
    if (have == want) return 1;
    ab_op op = {0}; op.kind = AB_OP_WORD; op.bus = bus; op.offset = offset; op.before = have; op.after = want;
    if (!ab_push(c, &op)) return 0;
    memcpy(bus + offset, &want, 4); return 1;
}
static uint32_t ab_channels_stored(uint32_t value)
{
    if ((value & 0xf00) != 0x100) return value;
    uint32_t mask = value >> 12 & 0x3ff3f, bits = 0;
    for (uint32_t m = mask; m; m &= m - 1) bits++;
    return (bits & 0xff) | mask << 12 | 0x100;
}
static int ab_settings(sh_audio_bus_change *c, ab_target *t, int created, char *error, size_t capacity)
{
    sh_audio_bus_native *n = c->owner; unsigned char *bus = t->bus;
    static const struct { uint32_t index, mask; size_t offset; unsigned char bit; } direct[] = {
        {0, 1, AB_FLAGS_B, 0x08}, {0, 2, AB_FLAGS_B, 0x40}, {1, 4, AB_FLAGS_A, 0x08},
        {4, 1, AB_FLAGS_B, 0x10}, {4, 2, AB_FLAGS_C, 0x08}};
    for (size_t i = 0; i < sizeof(direct) / sizeof(*direct); i++) {
        const sh_audio_bus_setting *s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_SETTING, direct[i].index, direct[i].mask);
        if (s && !ab_byte(c, bus, direct[i].offset, direct[i].bit, s->body[0] & direct[i].mask)) goto memory;
    }
    /* Instance-limit behavior and background music have native setters that
     * update active instances or a global registration. */
    for (uint32_t bit = 1; bit <= 8; bit <<= 1) {
        if (bit == 4) continue;
        const sh_audio_bus_setting *s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_SETTING, 1, bit);
        if (!s) continue;
        size_t offset = bit == 8 ? AB_FLAGS_C : AB_FLAGS_A;
        unsigned char native_bit = bit == 1 ? 0x01 : bit == 2 ? 0x02 : 0x80;
        int want = (s->body[0] & bit) != 0, have = (bus[offset] & native_bit) != 0;
        if (want == have) continue;
        ab_op op = {0}; op.bus = bus; op.before = (uint32_t)have; op.after = (uint32_t)want;
        op.kind = bit == 1 ? AB_OP_KILL : bit == 2 ? AB_OP_VIRTUAL : AB_OP_BACKGROUND;
        if (!ab_push(c, &op)) goto memory;
        if (bit == 1) n->set_limit_kill(bus, (unsigned char)want);
        else if (bit == 2) n->set_limit_virtual(bus, (unsigned char)want);
        else if (want) n->add_background(bus);
        else n->remove_background(bus);
        if (((bus[offset] & native_bit) != 0) != want) return ab_failf(error, capacity, "audio bus %u refused a limit setting", t->id);
    }
    const sh_audio_bus_setting *s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_SETTING, 2, 0x3ff);
    if (s && s->field.length == 2) {
        uint16_t want = (uint16_t)((s->body[0] | s->body[1] << 8) & 0x3ff), have;
        memcpy(&have, bus + AB_LIMIT, 2);
        if ((have & 0x3ff) != want) {
            ab_op op = {0}; op.kind = AB_OP_HALF; op.bus = bus; op.offset = AB_LIMIT; op.mask = 0x3ff; op.before = have;
            op.after = want;
            if (!ab_push(c, &op)) goto memory;
            have = (uint16_t)((have & ~0x3ff) | want); memcpy(bus + AB_LIMIT, &have, 2);
        }
    }
    s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_SETTING, 3, 0);
    if (s && s->field.length == 4) {
        uint32_t want = ab_u32(s->body), have = ab_u32(bus + AB_CHANNELS);
        if (ab_channels_stored(want) != have) {
            ab_op op = {0}; op.kind = AB_OP_CHANNELS; op.bus = bus; op.before = have; op.after = ab_channels_stored(want);
            if (!ab_push(c, &op)) goto memory;
            n->set_channels(bus, want);
        }
    }
    /* The cooked configuration marks every parsed bus. */
    if (created && !ab_byte(c, bus, AB_FLAGS_B, 0x01, 1)) goto memory;
    s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_FLAGS, 0, 1);
    if (s && !ab_byte(c, bus, AB_FLAGS_A, 0x10, s->body[0] & 1)) goto memory;
    s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_DUCK_SETTING, 0, 0);
    if (s && s->field.length == 4) {
        uint32_t samples = *n->sample_rate / 1000 * ab_u32(s->body);
        if (samples <= *n->duck_floor) samples = 0;
        if (!ab_word(c, bus, AB_RECOVERY, samples)) goto memory;
    }
    s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_DUCK_SETTING, 1, 0);
    if (s && s->field.length == 4 && !ab_word(c, bus, AB_FLOOR, ab_u32(s->body))) goto memory;
    return 1;
memory:
    return ab_fail(error, capacity, "cannot record audio bus settings");
}
static int ab_properties(sh_audio_bus_change *c, ab_target *t, char *error, size_t capacity)
{
    sh_audio_bus_native *n = c->owner; unsigned char *bus = t->bus, *live = ab_ptr(bus + AB_PROPERTIES);
    unsigned live_count = live ? live[0] : 0, count = 0;
    unsigned char ids[256]; uint32_t values[256];
    const unsigned char *live_values = live ? live + ((live_count + 4) & ~3u) : NULL;
    for (unsigned i = 0; i < live_count; i++) {
        unsigned char id = live[1 + i]; int seen = 0;
        for (unsigned j = 0; j < count && !seen; j++) seen = ids[j] == id;
        for (unsigned j = 0; j < i && !seen; j++) seen = live[1 + j] == id;
        if (seen) continue;
        const sh_audio_bus_setting *s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_PROPERTY, id, 0);
        if (!s || s->field.length != 4) continue;   /* removed, or left by a departed edit */
        ids[count] = id; values[count++] = ab_u32(s->body);
    }
    for (size_t i = t->first; i < t->first + t->count; i++) {
        const sh_audio_bus_setting *s = c->desired->settings + i; int seen = 0;
        if (s->field.field != SH_AUDIO_BUS_FIELD_PROPERTY || s->removed || s->field.length != 4 || s->field.key[0] > 255) continue;
        for (unsigned j = 0; j < count && !seen; j++) seen = ids[j] == s->field.key[0];
        if (!seen) { if (count == 255) return ab_fail(error, capacity, "audio bus has too many properties"); ids[count] = (unsigned char)s->field.key[0]; values[count++] = ab_u32(s->body); }
    }
    int changed = count != live_count;
    for (unsigned i = 0; i < count && !changed; i++) changed = ids[i] != live[1 + i] || values[i] != ab_u32(live_values + 4 * i);
    if (!changed) return 1;
    unsigned char *block = NULL;
    if (count) {
        size_t header = (count + 4) & ~3u;
        if (!(block = n->subscriptions->parameters->allocate(c->pool, header + (size_t)count * 4)))
            return ab_fail(error, capacity, "cannot allocate audio bus properties");
        memset(block, 0, header); block[0] = (unsigned char)count; memcpy(block + 1, ids, count);
        memcpy(block + header, values, (size_t)count * 4);
    }
    ab_op op = {0}; op.kind = AB_OP_PROPERTIES; op.bus = bus; op.object = block; op.previous = live;
    if (!ab_push(c, &op)) {
        if (block) n->subscriptions->parameters->release(c->pool, block);
        return ab_fail(error, capacity, "cannot record audio bus properties");
    }
    ab_set_ptr(bus + AB_PROPERTIES, block);
    return 1;
}
/* Instances read changed properties through the same curve refresh. */
static void ab_properties_changed(const sh_audio_bus_native *n, unsigned char *bus, const unsigned char *a, const unsigned char *b)
{
    for (unsigned id = 0; id < AB_PROPERTY_COUNT; id++) {
        int in_a = 0, in_b = 0; uint32_t va = 0, vb = 0;
        for (unsigned i = 0; a && i < a[0] && !in_a; i++) if (a[1 + i] == id) { in_a = 1; va = ab_u32(a + ((a[0] + 4) & ~3u) + 4 * i); }
        for (unsigned i = 0; b && i < b[0] && !in_b; i++) if (b[1 + i] == id) { in_b = 1; vb = ab_u32(b + ((b[0] + 4) & ~3u) + 4 * i); }
        int32_t parameter = n->property_parameters[id];
        if ((in_a != in_b || va != vb) && parameter >= 0 && parameter < 64) ab_value_changed(bus, (uint32_t)parameter);
    }
}
static unsigned char *ab_effects_table(unsigned char *bus) { return ab_ptr(bus + AB_EFFECTS); }
static int ab_effects(sh_audio_bus_change *c, ab_target *t, char *error, size_t capacity)
{
    sh_audio_bus_native *n = c->owner; unsigned char *bus = t->bus;
    for (uint32_t slot = 0; slot < 4; slot++) {
        const sh_audio_bus_setting *s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_EFFECT, slot, 0);
        uint32_t want = s && s->field.length == 5 ? ab_u32(s->body) : 0; unsigned char shared = s && s->field.length == 5 ? s->body[4] != 0 : 0;
        const unsigned char *table = ab_effects_table(bus);
        uint32_t have = table ? ab_u32(table + 4 + slot * 8) : 0; unsigned char have_shared = table ? table[9 + slot * 8] : 0;
        if (want == have && (!want || shared == have_shared)) continue;
        ab_op op = {0}; op.kind = AB_OP_EFFECT; op.bus = bus; op.key = slot; op.before = have; op.values[0] = have_shared;
        op.after = want; op.after_values[0] = shared;
        if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus effects");
        if (n->set_effect(bus, slot, want, (char)shared, 0) != 1 || !(table = ab_effects_table(bus)) ||
            ab_u32(table + 4 + slot * 8) != want || (want && table[9 + slot * 8] != shared))
            return ab_failf(error, capacity, "audio bus %u refused an effect slot", t->id);
    }
    uint32_t want = 0, mask = 0;
    for (uint32_t bit = 1; bit <= 0x80; bit <<= 1) {
        const sh_audio_bus_setting *s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_BYPASS, 0, bit);
        if (s) { mask |= bit; want |= s->body[0] & bit; }
    }
    const unsigned char *table = ab_effects_table(bus);
    uint32_t have = table ? table[0x24] : 0, differ = (have ^ want) & mask;
    if (differ) {
        ab_op op = {0}; op.kind = AB_OP_BYPASS; op.bus = bus; op.mask = differ; op.before = have & differ; op.after = want & differ;
        if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus bypass");
        if (n->set_bypass(bus, want & differ, differ) != 1) return ab_failf(error, capacity, "audio bus %u refused a bypass", t->id);
    }
    const sh_audio_bus_setting *s = ab_live_row(c, t, SH_AUDIO_BUS_FIELD_MIXER, 0, 0);
    uint32_t mixer = s && s->field.length == 5 ? ab_u32(s->body) : 0; unsigned char shared = s && s->field.length == 5 ? s->body[4] != 0 : 1;
    const unsigned char *live = ab_ptr(bus + AB_MIXER);
    uint32_t current = live ? ab_u32(live + 4) : 0; unsigned char current_shared = live ? live[9] : 1;
    if (mixer != current || (mixer && shared != current_shared)) {
        ab_op op = {0}; op.kind = AB_OP_MIXER; op.bus = bus; op.before = current; op.values[0] = current_shared;
        op.after = mixer; op.after_values[0] = shared;
        if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus mixer");
        live = ab_ptr(bus + AB_MIXER);
        if (ab_set_mixer(bus, mixer, (char)shared) != 1 || (live = ab_ptr(bus + AB_MIXER), (live ? ab_u32(live + 4) : 0) != mixer))
            return ab_failf(error, capacity, "audio bus %u refused a mixer", t->id);
    }
    return 1;
}
static unsigned char *ab_duck(unsigned char *bus, uint32_t target, unsigned char **previous)
{
    unsigned char *prior = NULL; size_t steps = 0;
    for (unsigned char *at = ab_ptr(bus + AB_DUCK_HEAD); at && steps < 100000; prior = at, at = ab_ptr(at), steps++)
        if (ab_u32(at + 8) == target) { if (previous) *previous = prior; return at; }
    return NULL;
}
/* List removal with the native disposal rule: entries from the preallocated
 * block return to its free list, others to the pool. */
static void ab_duck_remove(sh_audio_bus_change *c, unsigned char *bus, uint32_t target)
{
    unsigned char *prior = NULL, *entry = ab_duck(bus, target, &prior);
    if (!entry) return;
    unsigned char *next = ab_ptr(entry);
    if (prior) ab_set_ptr(prior, next); else ab_set_ptr(bus + AB_DUCK_HEAD, next);
    if (ab_ptr(bus + AB_DUCK_TAIL) == entry) ab_set_ptr(bus + AB_DUCK_TAIL, prior);
    unsigned char *base = ab_ptr(bus + AB_DUCK_BASE); uint32_t blocks = ab_u32(bus + AB_DUCK_BLOCKS), count = ab_u32(bus + AB_DUCK_COUNT);
    if (base && entry >= base && entry < base + (size_t)blocks * 0x20) {
        ab_set_ptr(entry, ab_ptr(bus + AB_DUCK_FREE)); ab_set_ptr(bus + AB_DUCK_FREE, entry);
    } else c->owner->subscriptions->parameters->release(c->pool, entry);
    count--; memcpy(bus + AB_DUCK_COUNT, &count, 4);
}
static int ab_ducks(sh_audio_bus_change *c, ab_target *t, char *error, size_t capacity)
{
    sh_audio_bus_native *n = c->owner; unsigned char *bus = t->bus;
    for (size_t i = t->first; i < t->first + t->count; i++) {
        const sh_audio_bus_setting *s = c->desired->settings + i;
        if (s->field.field != SH_AUDIO_BUS_FIELD_DUCK || s->removed || s->field.length != 14) continue;
        uint32_t want[5] = {ab_u32(s->body), ab_u32(s->body + 4), ab_u32(s->body + 8), s->body[12], s->body[13]};
        unsigned char *entry = ab_duck(bus, s->field.key[0], NULL);
        uint32_t have[5] = {0};
        if (entry) for (int k = 0; k < 5; k++) have[k] = ab_u32(entry + 0xc + 4 * k);
        if (entry && !memcmp(have, want, sizeof(want))) continue;
        ab_op op = {0}; op.kind = AB_OP_DUCK; op.bus = bus; op.key = s->field.key[0]; op.existed = entry != NULL; op.present = 1;
        memcpy(op.values, have, sizeof(have)); memcpy(op.after_values, want, sizeof(want));
        if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus ducking");
        if (n->set_duck(bus, s->field.key[0], want[0], want[1], want[2], want[3], (int)want[4]) != 1 ||
            !ab_duck(bus, s->field.key[0], NULL)) return ab_failf(error, capacity, "audio bus %u refused a ducking target", t->id);
    }
    for (int pass = 0; pass < 100000; pass++) {
        unsigned char *stale = NULL; size_t steps = 0;
        for (unsigned char *at = ab_ptr(bus + AB_DUCK_HEAD); at && !stale && steps < 100000; at = ab_ptr(at), steps++)
            if (!ab_live_row(c, t, SH_AUDIO_BUS_FIELD_DUCK, ab_u32(at + 8), 0)) stale = at;
        if (!stale) break;
        ab_op op = {0}; op.kind = AB_OP_DUCK; op.bus = bus; op.key = ab_u32(stale + 8); op.existed = 1; op.present = 0;
        for (int k = 0; k < 5; k++) op.values[k] = ab_u32(stale + 0xc + 4 * k);
        if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus ducking");
        ab_duck_remove(c, bus, op.key);
    }
    return 1;
}
static unsigned char *ab_group(unsigned char *bus, uint32_t id)
{
    size_t steps = 0;
    for (unsigned char *g = ab_ptr(bus + AB_GROUPS); g && steps < 100000; g = ab_ptr(g + AG_NEXT), steps++)
        if (ab_u32(g + AG_ID) == id) return g;
    return NULL;
}
static unsigned char *ab_mapping_entry(unsigned char *group, uint32_t value)
{
    unsigned char *items = ab_ptr(group + AG_MAPPINGS); uint32_t count = ab_u32(group + AG_MAPPINGS + 8);
    for (uint32_t i = 0; items && i < count; i++) if (ab_u32(items + 0x18 * i) == value) return items + 0x18 * i;
    return NULL;
}
/* The native per-object group disposal: active transition entries leave their
 * manager, mappings release their state objects, the group leaves the state
 * manager and the bus, then its storage returns to the pool. */
static int ab_group_remove(sh_audio_bus_change *c, unsigned char *bus, unsigned char *group)
{
    sh_audio_bus_native *n = c->owner;
    unsigned char *transitions = ab_ptr(group + AG_TRANSITIONS);
    if (transitions) {
        unsigned count = transitions[0]; unsigned char *entry = transitions + ((count + 4) & ~3u) + 8;
        for (unsigned i = 0; i < count; i++, entry += 0x10) if (ab_ptr(entry)) {
            n->transition_unregister(*n->transition_slot, ab_ptr(entry), group); ab_set_ptr(entry, NULL);
            ((void (*)(void *, int))ab_vt(ab_ptr(group + 0x18))[0x140 / 8])(ab_ptr(group + 0x18), 3);
        }
    }
    while (ab_u32(group + AG_MAPPINGS + 8)) n->state_unbind(group, ab_u32(ab_ptr(group + AG_MAPPINGS)));
    if (n->state_unregister(*n->state_slot, ab_u32(group + AG_ID), group) != 1) return 0;
    unsigned char **link = (unsigned char **)(bus + AB_GROUPS);
    while (*link && *link != group) link = (unsigned char **)(*link + AG_NEXT);
    if (*link) *link = ab_ptr(group + AG_NEXT);
    void *items = ab_ptr(group + AG_MAPPINGS);
    if (items) n->subscriptions->parameters->release(c->pool, items);
    if (transitions) n->subscriptions->parameters->release(c->pool, transitions);
    n->subscriptions->parameters->release(c->pool, group);
    return 1;
}
static int ab_states(sh_audio_bus_change *c, ab_target *t, char *error, size_t capacity)
{
    sh_audio_bus_native *n = c->owner; unsigned char *bus = t->bus; int changed = 0;
    for (size_t i = t->first; i < t->first + t->count; i++) {
        const sh_audio_bus_setting *s = c->desired->settings + i;
        if (s->field.field != SH_AUDIO_BUS_FIELD_STATE_GROUP || s->removed || s->field.length != 1) continue;
        unsigned char *group = ab_group(bus, s->field.key[0]);
        if (!group) {
            if (!(group = n->state_group(bus, s->field.key[0], 1))) return ab_failf(error, capacity, "state group %u cannot attach to the audio bus", s->field.key[0]);
            ab_op op = {0}; op.kind = AB_OP_GROUP_ADD; op.bus = bus; op.object = group; op.key = s->field.key[0];
            if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus states");
            changed = 1;
        }
        if (group[AG_SYNC] != s->body[0]) {
            ab_op op = {0}; op.kind = AB_OP_SYNC; op.bus = bus; op.object = group; op.key = s->field.key[0];
            op.before = group[AG_SYNC]; op.after = s->body[0];
            if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus states");
            group[AG_SYNC] = s->body[0];
        }
    }
    for (size_t i = t->first; i < t->first + t->count; i++) {
        const sh_audio_bus_setting *s = c->desired->settings + i;
        if (s->field.field != SH_AUDIO_BUS_FIELD_STATE || s->removed || s->field.length != 4) continue;
        unsigned char *group = ab_group(bus, s->field.key[0]), *entry;
        if (!group) return ab_failf(error, capacity, "audio bus state mapping has no group %u", s->field.key[0]);
        entry = ab_mapping_entry(group, s->field.key[1]);
        uint32_t have = entry ? ab_u32(entry + 0x10) : 0, want = ab_u32(s->body);
        if (entry && have == want) continue;
        ab_op op = {0}; op.kind = AB_OP_MAP; op.bus = bus; op.object = group; op.key = s->field.key[1];
        op.before = have; op.existed = entry != NULL; op.after = want; op.present = 1;
        if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus states");
        if (n->state_bind(group, want, s->field.key[1], 1) != 1)
            return ab_failf(error, capacity, "audio bus state object %u is not loaded", want);
    }
    /* Mappings and groups the policy no longer contains leave natively. */
    for (unsigned char *group = ab_ptr(bus + AB_GROUPS), *next; group; group = next) {
        next = ab_ptr(group + AG_NEXT);
        uint32_t id = ab_u32(group + AG_ID);
        if (!ab_live_row(c, t, SH_AUDIO_BUS_FIELD_STATE_GROUP, id, 0)) {
            ab_op op = {0}; op.kind = AB_OP_GROUP_REMOVE; op.bus = bus; op.key = id; op.before = group[AG_SYNC];
            uint32_t count = ab_u32(group + AG_MAPPINGS + 8); unsigned char *items = ab_ptr(group + AG_MAPPINGS);
            if (count && !(op.mappings = calloc(count, sizeof(*op.mappings)))) return ab_fail(error, capacity, "cannot record audio bus states");
            for (uint32_t m = 0; m < count; m++) op.mappings[op.mapping_count++] = (ab_mapping){ab_u32(items + 0x18 * m), ab_u32(items + 0x18 * m + 0x10)};
            if (!ab_push(c, &op)) { free(op.mappings); return ab_fail(error, capacity, "cannot record audio bus states"); }
            if (!ab_group_remove(c, bus, group)) return ab_failf(error, capacity, "state group %u cannot leave the audio bus", id);
            changed = 1; continue;
        }
        for (uint32_t m = 0; m < ab_u32(group + AG_MAPPINGS + 8);) {
            unsigned char *entry = (unsigned char *)ab_ptr(group + AG_MAPPINGS) + 0x18 * m;
            uint32_t value = ab_u32(entry);
            if (ab_live_row(c, t, SH_AUDIO_BUS_FIELD_STATE, id, value)) { m++; continue; }
            ab_op op = {0}; op.kind = AB_OP_MAP; op.bus = bus; op.object = group; op.key = value;
            op.before = ab_u32(entry + 0x10); op.existed = 1; op.present = 0;
            if (!ab_push(c, &op)) return ab_fail(error, capacity, "cannot record audio bus states");
            n->state_unbind(group, value); changed = 1;
        }
    }
    if (changed) ab_states_changed(bus);
    return 1;
}
/* Curves of every target bus form one subscription change; parameters whose
 * native subscription the policy no longer has are removed. */
static int ab_curves_plan(sh_audio_bus_change *c, char *error, size_t capacity)
{
    sh_audio_bus_native *n = c->owner; size_t modes = 0, curves = 0, live = 0;
    uint32_t parameters[256];
    for (size_t i = 0; i < c->target_count; i++) {
        const ab_target *t = c->targets + i;
        for (size_t r = t->first; r < t->first + t->count; r++) {
            const sh_audio_bus_setting *s = c->desired->settings + r;
            modes += s->field.field == SH_AUDIO_BUS_FIELD_RTPC_MODE;
            curves += s->field.field == SH_AUDIO_BUS_FIELD_RTPC_CURVE;
        }
        size_t found = sh_audio_subscription_parameters(n->subscriptions, t->bus + AB_CONSUMER, parameters, 256);
        live += found < 256 ? found : 256;
    }
    size_t total = modes + live;
    c->requests = calloc(total ? total : 1, sizeof(*c->requests));
    c->request_buses = calloc(total ? total : 1, sizeof(*c->request_buses));
    c->curves = calloc(curves ? curves : 1, sizeof(*c->curves));
    if (!c->requests || !c->request_buses || !c->curves) return ab_fail(error, capacity, "cannot plan audio bus curves");
    for (size_t i = 0; i < c->target_count; i++) {
        const ab_target *t = c->targets + i; void *consumer = t->bus + AB_CONSUMER;
        size_t first_request = c->request_count;
        for (size_t r = t->first; r < t->first + t->count; r++) {
            const sh_audio_bus_setting *mode = c->desired->settings + r;
            if (mode->field.field != SH_AUDIO_BUS_FIELD_RTPC_MODE || mode->removed || mode->field.length != 1) continue;
            sh_audio_subscription_request *q = c->requests + c->request_count;
            q->consumer = consumer; q->parameter = mode->field.key[0]; q->category = 2; q->mode = mode->body[0];
            q->curves = c->curves + c->curve_count;
            for (size_t k = t->first; k < t->first + t->count; k++) {
                const sh_audio_bus_setting *s = c->desired->settings + k;
                if (s->field.field != SH_AUDIO_BUS_FIELD_RTPC_CURVE || s->removed || s->field.key[0] != q->parameter) continue;
                if (s->field.length < 14) return ab_fail(error, capacity, "audio bus curve is malformed");
                uint32_t count = (uint32_t)(s->body[12] | s->body[13] << 8), curve;
                if (!count || s->field.length != 14 + (size_t)count * 12) return ab_fail(error, capacity, "audio bus curve is malformed");
                memcpy(&curve, s->body + 7, 4);
                c->curves[c->curve_count++] = (sh_audio_subscription_curve){(int32_t)curve, ab_u32(s->body), s->body[4],
                    s->body + 14, count, s->body[11]};
                q->count++;
            }
            if (!q->count) continue;
            c->request_buses[c->request_count++] = t->bus;
        }
        size_t found = sh_audio_subscription_parameters(n->subscriptions, consumer, parameters, 256);
        for (size_t p = 0; p < found && p < 256; p++) {
            int wanted = 0;
            for (size_t q = first_request; q < c->request_count && !wanted; q++) wanted = c->requests[q].parameter == parameters[p];
            if (wanted) continue;
            sh_audio_subscription_request *q = c->requests + c->request_count;
            q->consumer = consumer; q->parameter = parameters[p]; q->category = 2;
            c->request_buses[c->request_count++] = t->bus;
        }
    }
    /* Unchanged subscriptions keep their records. */
    size_t kept = 0;
    for (size_t i = 0; i < c->request_count; i++) {
        if (sh_audio_subscription_matches(n->subscriptions, c->requests + i)) continue;
        c->requests[kept] = c->requests[i]; c->request_buses[kept++] = c->request_buses[i];
    }
    c->request_count = kept;
    /* Registration uses the selector the native registration derives from
     * the consumer's active instances; its mask object must exist first. */
    for (size_t i = 0; i < c->request_count; i++) {
        void *consumer = c->requests[i].consumer;
        if (c->requests[i].count && !n->rtpc_ready(consumer)) return ab_fail(error, capacity, "cannot prepare audio bus curve state");
        const unsigned char *masks = ab_ptr((unsigned char *)consumer + 8);
        c->requests[i].selector = masks && ab_u32(masks + 0x18) != 0;
    }
    return 1;
}
static int ab_curves_publish(sh_audio_bus_change *c, char *error, size_t capacity)
{
    sh_audio_bus_native *n = c->owner;
    if (!c->request_count) return 1;
    if (sh_audio_subscription_prepare(n->subscriptions, c->requests, c->request_count, &c->subscriptions, error, capacity) != 1) return 0;
    int published = sh_audio_subscription_publish(&c->subscriptions, error, capacity);
    if (published < 0) { c->uncertain = 1; return -1; }
    if (!published) return 0;
    sh_audio_subscription_refresh(&c->subscriptions);
    for (size_t i = 0; i < c->request_count; i++) {
        if (c->requests[i].count && n->rtpc_mark(c->requests[i].consumer, (int)c->requests[i].parameter) != 1) {
            /* The subscription is live; only the fast-path mask failed. */
            ab_failf(error, capacity, "audio bus curve parameter %u was not marked", c->requests[i].parameter);
            c->uncertain = 1; return -1;
        }
        ab_curve_changed(c->request_buses[i], c->requests[i].parameter);
    }
    return 1;
}

/* ---- reversal ---- */
static int ab_undo(sh_audio_bus_change *c, ab_op *op)
{
    sh_audio_bus_native *n = c->owner; unsigned char *bus = op->bus;
    switch (op->kind) {
    case AB_OP_CREATE: {
        unsigned char *parent = ab_ptr(bus + AB_PARENT);
        if (parent) ab_detach(parent, bus);
        ab_release(bus); return 1;
    }
    case AB_OP_ADOPT: ab_release(bus); return 1;
    case AB_OP_PARENT:
        if (ab_ptr(bus + AB_PARENT) == op->object) ab_detach(op->object, bus);
        if (op->previous) { ab_addref(bus); if (ab_attach(op->previous, bus) != 1) return 0; }
        return 1;
    case AB_OP_BYTE: bus[op->offset] = (unsigned char)((bus[op->offset] & ~op->mask) | (op->before & op->mask)); return 1;
    case AB_OP_HALF: { uint16_t v; memcpy(&v, bus + op->offset, 2); v = (uint16_t)((v & ~op->mask) | (op->before & op->mask)); memcpy(bus + op->offset, &v, 2); return 1; }
    case AB_OP_WORD: case AB_OP_CHANNELS: memcpy(bus + (op->kind == AB_OP_WORD ? op->offset : AB_CHANNELS), &op->before, 4); return 1;
    case AB_OP_PROPERTIES: {
        ab_set_ptr(bus + AB_PROPERTIES, op->previous);
        ab_properties_changed(n, bus, op->object, op->previous);
        if (op->object) n->subscriptions->parameters->release(c->pool, op->object);
        op->object = NULL; return 1;
    }
    case AB_OP_KILL: n->set_limit_kill(bus, (unsigned char)op->before); return 1;
    case AB_OP_VIRTUAL: n->set_limit_virtual(bus, (unsigned char)op->before); return 1;
    case AB_OP_BACKGROUND: if (op->before) n->add_background(bus); else n->remove_background(bus); return 1;
    case AB_OP_EFFECT: return n->set_effect(bus, op->key, op->before, (char)op->values[0], 0) == 1;
    case AB_OP_BYPASS: return n->set_bypass(bus, op->before, op->mask) == 1;
    case AB_OP_MIXER: return ab_set_mixer(bus, op->before, (char)op->values[0]) == 1;
    case AB_OP_DUCK:
        if (!op->existed) { ab_duck_remove(c, bus, op->key); return 1; }
        return n->set_duck(bus, op->key, op->values[0], op->values[1], op->values[2], op->values[3], (int)op->values[4]) == 1;
    case AB_OP_GROUP_ADD: {
        unsigned char *group = ab_group(bus, op->key);
        int ok = !group || ab_group_remove(c, bus, group);
        ab_states_changed(bus); return ok;
    }
    case AB_OP_GROUP_REMOVE: {
        unsigned char *group = n->state_group(bus, op->key, 1); int ok = group != NULL;
        if (group) group[AG_SYNC] = (unsigned char)op->before;
        for (size_t m = 0; group && m < op->mapping_count; m++)
            ok &= n->state_bind(group, op->mappings[m].object, op->mappings[m].value, 1) == 1;
        return ok;
    }
    case AB_OP_SYNC: ((unsigned char *)op->object)[AG_SYNC] = (unsigned char)op->before; return 1;
    case AB_OP_MAP:
        if (op->existed) return n->state_bind(op->object, op->before, op->key, 1) == 1;
        n->state_unbind(op->object, op->key); ab_states_changed(bus); return 1;
    }
    return 0;
}
static int ab_revert(sh_audio_bus_change *c, size_t through)
{
    int ok = 1;
    for (size_t i = through; i > 0; i--) ok &= ab_undo(c, c->ops + i - 1);
    return ok;
}
static void ab_host_free(sh_audio_bus_change *c)
{
    for (size_t i = 0; i < c->op_count; i++) free(c->ops[i].mappings);
    free(c->ops); free(c->targets); free(c->requests); free(c->curves); free(c->request_buses);
    free(c->owned); free(c->touched);
}
static void ab_end(sh_audio_bus_change *c)
{
    if (c->subscriptions.owner) sh_audio_subscription_release(&c->subscriptions);
    ab_host_free(c);
    if (c->owner && c->owner->pending == c) c->owner->pending = NULL;
    memset(c, 0, sizeof(*c));
}
static int ab_owned_(const sh_audio_bus_change *c) { return c && c->owner && c->owner->pending == c && c->thread == GetCurrentThreadId(); }

/* ---- public ---- */
int sh_audio_bus_native_identities(sh_audio_bus_native *native, const sh_audio_bus_policy *effective,
    const sh_audio_bus_policy *baseline, int new_only, sh_audio_bus_change *change, char *error, size_t capacity)
{
    volatile int held = 0, result = 0;
    if (error && capacity) *error = 0;
    if (!native || native->pending || !change || change->owner || !effective)
        return ab_fail(error, capacity, "audio bus adapter is unavailable or has a pending transition");
    memset(change, 0, sizeof(*change)); change->owner = native; change->desired = effective; change->new_only = new_only;
    if (!ab_plan(change, baseline)) { ab_host_free(change); memset(change, 0, sizeof(*change)); return ab_fail(error, capacity, "cannot plan native audio buses"); }
    if (!change->target_count && (new_only || !native->owned_count)) { ab_host_free(change); memset(change, 0, sizeof(*change)); return 1; }
    if (!native->subscriptions || !native->render_lock || !native->index_slot || !native->lookup || !native->create_aux) {
        ab_host_free(change); memset(change, 0, sizeof(*change));
        return ab_fail(error, capacity, "native audio bus publication is unavailable");
    }
    __try {
        __try {
            EnterCriticalSection(native->render_lock); held = 1;
            change->manager = *native->manager_slot; change->index = *native->index_slot;
            change->pool = *native->subscriptions->parameters->pool; change->thread = GetCurrentThreadId();
            if (!change->manager || !change->index) ab_fail(error, capacity, "native audio bus manager is unavailable");
            else {
                native->pending = change; result = 1;
                for (size_t i = 0; result && i < change->target_count; i++) result = ab_identity(change, change->targets + i, error, capacity);
                if (result) result = ab_parents(change, error, capacity);
                if (result) change->state = AB_IDENTITIES;
                else if (!change->uncertain && ab_revert(change, change->op_count)) change->op_count = 0;
                else change->uncertain = 1;
            }
        } __finally { if (held) LeaveCriticalSection(native->render_lock); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { change->uncertain = 1; result = 0; }
    if (result) return 1;
    if (change->uncertain && native->pending == change) return -1;
    ab_end(change); return 0;
}
int sh_audio_bus_native_apply(sh_audio_bus_change *change, char *error, size_t capacity)
{
    volatile int held = 0, result = 0;
    if (error && capacity) *error = 0;
    if (!change || !change->owner) return 1;   /* nothing represented */
    if (!ab_owned_(change) || change->state != AB_IDENTITIES || change->uncertain)
        return ab_fail(error, capacity, "audio bus identities are not prepared");
    sh_audio_bus_native *native = change->owner; size_t identities = change->op_count;
    __try {
        __try {
            EnterCriticalSection(native->render_lock); held = 1;
            if (!ab_alive(change)) ab_fail(error, capacity, "native audio bus manager changed");
            else {
                result = 1;
                for (size_t i = 0; result == 1 && i < change->target_count; i++) {
                    ab_target *t = change->targets + i; int created = 0;
                    for (size_t o = 0; o < identities && !created; o++) created = change->ops[o].kind == AB_OP_CREATE && change->ops[o].bus == t->bus;
                    result = ab_properties(change, t, error, capacity) && ab_settings(change, t, created, error, capacity) &&
                        ab_ducks(change, t, error, capacity) && ab_effects(change, t, error, capacity) && ab_states(change, t, error, capacity);
                }
                size_t configured = change->op_count;
                for (size_t i = 0; result == 1 && i < configured; i++) if (change->ops[i].kind == AB_OP_PROPERTIES)
                    ab_properties_changed(native, change->ops[i].bus, change->ops[i].previous, change->ops[i].object);
                if (result == 1 && !ab_curves_plan(change, error, capacity)) result = 0;
                if (result == 1) result = ab_curves_publish(change, error, capacity);
                if (result == 1) change->state = AB_CONFIGURED;
                else if (result == 0 && !change->uncertain) {
                    /* Configuration reverts to the prepared identities. */
                    if (!ab_revert(change, change->op_count) ) change->uncertain = 1;
                    else change->op_count = 0;
                    if (change->subscriptions.owner) sh_audio_subscription_release(&change->subscriptions);
                }
            }
        } __finally { if (held) LeaveCriticalSection(native->render_lock); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { change->uncertain = 1; result = -1; }
    if (result == 1) return 1;
    if (change->uncertain) return -1;
    ab_end(change); return 0;
}
static int ab_matches(sh_audio_bus_change *c)
{
    sh_audio_bus_native *n = c->owner;
    if (!ab_alive(c)) return 0;
    for (size_t i = 0; i < c->op_count; i++) {
        const ab_op *op = c->ops + i; unsigned char *bus = op->bus; const unsigned char *table;
        switch (op->kind) {
        case AB_OP_CREATE: case AB_OP_ADOPT: {
            void *live = ab_lookup(c, ab_u32(bus + AB_ID));
            if (live) ab_release(live);
            if (live != bus) return 0;
            break;
        }
        case AB_OP_PARENT: if (ab_ptr(bus + AB_PARENT) != op->object) return 0; break;
        case AB_OP_BYTE: if ((bus[op->offset] & op->mask) != op->after) return 0; break;
        case AB_OP_WORD: if (ab_u32(bus + op->offset) != op->after) return 0; break;
        case AB_OP_CHANNELS: if (ab_u32(bus + AB_CHANNELS) != op->after) return 0; break;
        case AB_OP_HALF: { uint16_t v; memcpy(&v, bus + op->offset, 2); if ((v & op->mask) != op->after) return 0; break; }
        case AB_OP_PROPERTIES: if (ab_ptr(bus + AB_PROPERTIES) != op->object) return 0; break;
        case AB_OP_KILL: if ((bus[AB_FLAGS_A] & 1) != op->after) return 0; break;
        case AB_OP_VIRTUAL: if (((bus[AB_FLAGS_A] >> 1) & 1) != op->after) return 0; break;
        case AB_OP_BACKGROUND: if (((bus[AB_FLAGS_C] >> 7) & 1) != op->after) return 0; break;
        case AB_OP_EFFECT:
            table = ab_effects_table(bus);
            if ((table ? ab_u32(table + 4 + op->key * 8) : 0) != op->after) return 0;
            break;
        case AB_OP_BYPASS: table = ab_effects_table(bus); if (((table ? table[0x24] : 0) & op->mask) != op->after) return 0; break;
        case AB_OP_MIXER: { const unsigned char *m = ab_ptr(bus + AB_MIXER); if ((m ? ab_u32(m + 4) : 0) != op->after) return 0; break; }
        case AB_OP_DUCK: if ((ab_duck(bus, op->key, NULL) != NULL) != op->present) return 0; break;
        case AB_OP_GROUP_ADD: if (!ab_group(bus, op->key)) return 0; break;
        case AB_OP_GROUP_REMOVE: if (ab_group(bus, op->key)) return 0; break;
        case AB_OP_SYNC: if (((unsigned char *)op->object)[AG_SYNC] != op->after) return 0; break;
        case AB_OP_MAP: {
            unsigned char *entry = ab_mapping_entry(op->object, op->key);
            if ((entry != NULL) != op->present || (entry && ab_u32(entry + 0x10) != op->after)) return 0;
            break;
        }
        }
    }
    (void)n;
    return !c->subscriptions.owner || sh_audio_subscription_ready(&c->subscriptions);
}
int sh_audio_bus_native_ready(sh_audio_bus_change *change, char *error, size_t capacity)
{
    volatile int held = 0; int result = 0;
    if (error && capacity) *error = 0;
    if (!change || !change->owner) return 1;
    if (ab_owned_(change) && (change->state == AB_IDENTITIES || change->state == AB_CONFIGURED) && !change->uncertain) {
        CRITICAL_SECTION *lock = change->owner->render_lock;
        __try {
            __try { EnterCriticalSection(lock); held = 1; result = ab_matches(change); }
            __finally { if (held) LeaveCriticalSection(lock); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { result = 0; }
    }
    return result ? 1 : ab_fail(error, capacity, "audio buses no longer belong to the pending transition");
}
int sh_audio_bus_native_restore(sh_audio_bus_change *change, char *error, size_t capacity)
{
    volatile int held = 0; int result = 0;
    if (error && capacity) *error = 0;
    if (!change || !change->owner) return 1;
    if (ab_owned_(change) && change->state != AB_ACCEPTED && !change->uncertain) {
        CRITICAL_SECTION *lock = change->owner->render_lock;
        __try {
            __try {
                EnterCriticalSection(lock); held = 1;
                if (ab_alive(change)) {
                    int ok = 1;
                    if (change->subscriptions.owner) {
                        ok = sh_audio_subscription_restore(&change->subscriptions, error, capacity);
                        if (ok) {
                            sh_audio_subscription_refresh(&change->subscriptions);
                            for (size_t i = 0; i < change->request_count; i++)
                                ab_curve_changed(change->request_buses[i], change->requests[i].parameter);
                        } else if (change->subscriptions.uncertain) change->uncertain = 1;
                    }
                    if (ok && ab_revert(change, change->op_count)) { change->op_count = 0; ab_end(change); result = 1; }
                    else change->uncertain = 1;
                }
            } __finally { if (held) LeaveCriticalSection(lock); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { if (change->owner) change->uncertain = 1; result = 0; }
    }
    return result ? 1 : ab_fail(error, capacity, "audio bus recovery retains native ownership");
}
void sh_audio_bus_native_accept(sh_audio_bus_change *change)
{
    if (!change || !change->owner || !ab_owned_(change) || change->state != AB_CONFIGURED || change->uncertain) return;
    sh_audio_bus_native *n = change->owner; size_t owned = 0, touched = 0;
    sh_audio_bus_owned *next_owned = calloc(change->target_count + n->owned_count + 1, sizeof(*next_owned));
    uint32_t *next_touched = calloc(change->target_count + n->touched_count + 1, sizeof(*next_touched));
    if (!next_owned || !next_touched) { free(next_owned); free(next_touched); change->uncertain = 1; return; }
    for (size_t i = 0; i < change->target_count; i++) {
        const ab_target *t = change->targets + i;
        if (!t->native) { next_owned[owned++] = (sh_audio_bus_owned){t->id, t->bus}; continue; }
        int edited = 0;
        for (size_t r = t->first; r < t->first + t->count && !edited; r++) edited = change->desired->settings[r].edited;
        if (edited) next_touched[touched++] = t->id;
    }
    /* Additive admission keeps every earlier ownership. */
    for (size_t i = 0; change->new_only && i < n->touched_count; i++) next_touched[touched++] = n->touched[i];
    for (size_t i = 0; change->new_only && i < n->owned_count; i++) {
        int kept = 0;
        for (size_t j = 0; j < owned && !kept; j++) kept = next_owned[j].id == n->owned[i].id;
        if (!kept) next_owned[owned++] = n->owned[i];
    }
    /* Previously owned buses the policy dropped are released after commit. */
    change->owned = n->owned; change->owned_count = n->owned_count;
    n->owned = next_owned; n->owned_count = owned; n->touched = next_touched; n->touched_count = touched;
    free(change->touched); change->touched = NULL;
    change->state = AB_ACCEPTED;
}
int sh_audio_bus_native_release(sh_audio_bus_change *change, char *error, size_t capacity)
{
    volatile int held = 0; int result = 0;
    if (error && capacity) *error = 0;
    if (!change || !change->owner) return 1;
    if (ab_owned_(change) && change->state == AB_ACCEPTED && !change->uncertain) {
        sh_audio_bus_native *n = change->owner; CRITICAL_SECTION *lock = n->render_lock;
        __try {
            __try {
                EnterCriticalSection(lock); held = 1;
                for (size_t i = 0; i < change->op_count; i++)
                    if (change->ops[i].kind == AB_OP_PROPERTIES && change->ops[i].previous)
                        n->subscriptions->parameters->release(change->pool, change->ops[i].previous);
                for (size_t i = 0; i < change->owned_count; i++) {
                    int kept = 0;
                    for (size_t j = 0; j < n->owned_count && !kept; j++) kept = n->owned[j].id == change->owned[i].id;
                    if (!kept) ab_release(change->owned[i].bus);
                }
                ab_end(change); result = 1;
            } __finally { if (held) LeaveCriticalSection(lock); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { if (change->owner) change->uncertain = 1; result = 0; }
    }
    return result ? 1 : ab_fail(error, capacity, "audio bus retirement retains native ownership");
}
int sh_audio_bus_native_owns(const sh_audio_bus_native *native)
{ return native && (native->owned_count || native->touched_count); }

/* ---- binding ---- */
/* Vtable entries are absolute. A loader-relocated image already points into
 * itself; an unrelocated mapping (offline checks) still uses the preferred
 * base, which is translated back into the mapping. */
static const unsigned char *ab_image_pointer(const unsigned char *image, const void *value)
{
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
    uintptr_t v = (uintptr_t)value, size = nt->OptionalHeader.SizeOfImage, preferred = (uintptr_t)nt->OptionalHeader.ImageBase;
    if (v >= (uintptr_t)image && v - (uintptr_t)image < size) return value;
    if (v >= preferred && v - preferred < size) return image + (v - preferred);
    return NULL;
}
int sh_audio_bus_native_bind(sh_audio_bus_native *native, const unsigned char *image,
    const sig_result *results, size_t count, sh_audio_subscription_native *subscriptions,
    const sh_audio_limits_native *limits, const sh_audio_registry *registry, const sh_audio_state_native *state)
{
    const unsigned char *aux = NULL; sh_audio_bus_native bound = {0};
    if (!native || native->pending || !subscriptions || !subscriptions->parameters || !limits || !registry || !state ||
        !limits->render_lock || !registry->index_slot || !state->state_slot || (count && !results)) return 0;
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, "AudioAuxBusCreate")) {
        if (aux || results[i].status != SIG_OK) return 0;
        aux = (const unsigned char *)results[i].addr;
    }
    const DWORD code = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE, data = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE,
        constant = IMAGE_SCN_MEM_READ;
    if (!ani_section(image, aux, 0x66, code, IMAGE_SCN_MEM_WRITE)) return 0;
    __try {
        const unsigned char call[] = {0xe8}, jump[] = {0xe9}, lea_rax[] = {0x48, 0x8d, 0x05}, lea_r15[] = {0x4c, 0x8d, 0x3d},
            rcx[] = {0x48, 0x8b, 0x0d}, ecx[] = {0x8b, 0x0d}, rax_load[] = {0x48, 0x8b, 0x05}, rbx_store[] = {0x48, 0x89, 0x1d},
            cmp_q[] = {0x48, 0x83, 0x3d}, mul[] = {0xf7, 0x25}, movzx[] = {0x0f, 0xb7, 0x05};
        /* Auxiliary constructor: pool allocation, shared construction, its
         * vtables and initialization/registration. */
        if (ani_rip(aux + 0x11, ecx, 2) != (void *)subscriptions->parameters->pool ||
            ani_rip(aux + 0x17, call, 1) != (void *)subscriptions->parameters->allocate) return 0;
        const unsigned char *common = ani_rip(aux + 0x29, call, 1), *init = ani_rip(aux + 0x46, call, 1);
        bound.aux_vtable = ani_rip(aux + 0x2e, lea_rax, 3); bound.consumer_vtable = ani_rip(aux + 0x3b, lea_rax, 3);
        if (!ani_section(image, common, 0x30, code, IMAGE_SCN_MEM_WRITE)) return 0;
        bound.normal_vtable = ani_rip(common + 0x10, lea_rax, 3);
        if (ani_rip(common + 0x28, lea_rax, 3) != bound.consumer_vtable) return 0;
        const void *tables[] = {bound.normal_vtable, bound.aux_vtable, bound.consumer_vtable};
        for (size_t i = 0; i < 3; i++) if (!ani_section(image, tables[i], i < 2 ? 0x2b8 : 0x20, constant, IMAGE_SCN_MEM_EXECUTE)) return 0;
        const void *const *normal = bound.normal_vtable, *const *auxiliary = bound.aux_vtable;
        const unsigned char *parser = ab_image_pointer(image, normal[0x2b0 / 8]), *settings = ab_image_pointer(image, normal[0x268 / 8]),
            *effects = ab_image_pointer(image, normal[0x270 / 8]), *wrapper = ab_image_pointer(image, normal[0]);
        if (ab_image_pointer(image, auxiliary[0x2b0 / 8]) != parser || ab_image_pointer(image, auxiliary[0x268 / 8]) != settings ||
            ab_image_pointer(image, auxiliary[0x270 / 8]) != effects || auxiliary[0x48 / 8] != normal[0x48 / 8] ||
            auxiliary[0x60 / 8] != normal[0x60 / 8]) return 0;
        if (!ani_section(image, parser, 0x214, code, IMAGE_SCN_MEM_WRITE) || !ani_section(image, settings, 0xfc, code, IMAGE_SCN_MEM_WRITE) ||
            !ani_section(image, effects, 0xb7, code, IMAGE_SCN_MEM_WRITE) || !ani_section(image, wrapper, 0x14, code, IMAGE_SCN_MEM_WRITE)) return 0;
        /* Parser: index, lookup, root slots, duck recovery globals, readers. */
        if (ani_rip(parser + 0x2d, rcx, 3) != (void *)registry->index_slot) return 0;
        const unsigned char *lookup = ani_rip(parser + 0x3c, call, 1);
        if (!ani_section(image, lookup, 0x11, code, IMAGE_SCN_MEM_WRITE)) return 0;
        bound.lookup = (void *(*)(void *, uint32_t))ani_rip(lookup + 0xc, jump, 1);
        bound.roots[0] = ani_rip(parser + 0x7f, rax_load, 3);
        if (bound.roots[0] != ani_rip(parser + 0x8b, rbx_store, 3)) return 0;
        /* The second slot's compare carries an immediate after its offset. */
        bound.roots[1] = ani_rip(parser + 0xaa, rbx_store, 3);
        {
            int32_t relative; memcpy(&relative, parser + 0x9e, 4);
            if (memcmp(parser + 0x9b, cmp_q, 3) || parser[0xa2] || (void *)(parser + 0xa3 + relative) != (void *)bound.roots[1]) return 0;
        }
        bound.sample_rate = ani_rip(parser + 0xe7, mul, 2);
        bound.duck_floor = ani_rip(parser + 0xf4, movzx, 3);
        const unsigned char *duck = ani_rip(parser + 0x194, call, 1), *curves = ani_rip(parser + 0x1fb, call, 1),
            *states = ani_rip(parser + 0x20f, call, 1);
        bound.set_duck = (int (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int))duck;
        if (!ani_section(image, duck, 0x7e, code, IMAGE_SCN_MEM_WRITE) || !ani_section(image, curves, 0xbb, code, IMAGE_SCN_MEM_WRITE) ||
            !ani_section(image, states, 0xb1, code, IMAGE_SCN_MEM_WRITE)) return 0;
        bound.property_parameters = ani_rip(duck + 0x68, lea_rax, 3);
        if (!ani_section(image, bound.property_parameters, AB_PROPERTY_COUNT * 4, constant, IMAGE_SCN_MEM_EXECUTE) ||
            ani_rip(duck + 0x5b, call, 1) != lookup) return 0;
        /* Curve registration: mask object and parameter mark around the core. */
        const unsigned char *registration = ani_rip(curves + 0xb6, call, 1);
        if (!ani_section(image, registration, 0xf6, code, IMAGE_SCN_MEM_WRITE) ||
            ani_rip(registration + 0x31, rcx, 3) != (void *)subscriptions->parameters->parameter_slot) return 0;
        bound.rtpc_ready = (char (*)(void *))ani_rip(registration + 0x24, call, 1);
        bound.rtpc_mark = (int (*)(void *, int))ani_rip(registration + 0xf1, call, 1);
        if (ani_rip(duck + 0x79, call, 1) != (void *)bound.rtpc_mark) return 0;
        /* State reader, then the destructor chain's state removal helpers. */
        bound.state_group = (void *(*)(void *, uint32_t, char))ani_rip(states + 0x52, call, 1);
        bound.state_bind = (int (*)(void *, uint32_t, uint32_t, char))ani_rip(states + 0xac, call, 1);
        if (!ani_section(image, bound.state_group, 0x9e, code, IMAGE_SCN_MEM_WRITE) ||
            ani_rip((const unsigned char *)bound.state_group + 0x92, rcx, 3) != (void *)state->state_slot ||
            ani_rip((const unsigned char *)bound.state_group + 0x42, call, 1) != (void *)subscriptions->parameters->allocate) return 0;
        const unsigned char *body = ani_rip(wrapper + 0xf, call, 1);
        if (!ani_section(image, body, 0x466, code, IMAGE_SCN_MEM_WRITE)) return 0;
        bound.remove_background = (void (*)(void *))ani_rip(body + 0x27, call, 1);
        const unsigned char *base = ani_rip(body + 0x461, jump, 1);
        if (!ani_section(image, base, 0x7f, code, IMAGE_SCN_MEM_WRITE)) return 0;
        const unsigned char *transitions = ani_rip(base + 0x70, call, 1), *groups = ani_rip(base + 0x7a, call, 1);
        if (!ani_section(image, transitions, 0x99, code, IMAGE_SCN_MEM_WRITE) || !ani_section(image, groups, 0x6e, code, IMAGE_SCN_MEM_WRITE)) return 0;
        bound.transition_slot = ani_rip(transitions + 0x8a, rcx, 3);
        bound.transition_unregister = (int (*)(void *, void *, void *))ani_rip(transitions + 0x94, call, 1);
        if (ani_rip(groups + 0x4b, rcx, 3) != (void *)state->state_slot) return 0;
        bound.state_unregister = (int (*)(void *, uint32_t, void *))ani_rip(groups + 0x52, call, 1);
        bound.state_unbind = (void *(*)(void *, uint32_t))ani_rip(groups + 0x69, call, 1);
        (void)lea_r15;
        /* Cooked readers' setters. */
        bound.set_limit_kill = (void (*)(void *, unsigned char))ani_rip(settings + 0x5f, call, 1);
        bound.set_limit_virtual = (void (*)(void *, unsigned char))ani_rip(settings + 0x6f, call, 1);
        bound.set_channels = (void (*)(void *, uint32_t))ani_rip(settings + 0xb3, call, 1);
        bound.add_background = (void (*)(void *))ani_rip(settings + 0xf7, call, 1);
        bound.set_effect = (int (*)(void *, uint32_t, uint32_t, char, int))ani_rip(effects + 0x96, call, 1);
        bound.set_bypass = (int (*)(void *, uint32_t, uint32_t))ani_rip(effects + 0xb2, call, 1);
        bound.construct = (void *(*)(void *, uint32_t))common; bound.initialize = (int (*)(void *))init;
        bound.create_aux = (void *(*)(uint32_t))aux;
        const void *targets[] = {(void *)bound.lookup, (void *)bound.set_duck, (void *)bound.rtpc_ready, (void *)bound.rtpc_mark,
            (void *)bound.state_group, (void *)bound.state_bind, (void *)bound.remove_background, (void *)bound.transition_unregister,
            (void *)bound.state_unregister, (void *)bound.state_unbind, (void *)bound.set_limit_kill, (void *)bound.set_limit_virtual,
            (void *)bound.set_channels, (void *)bound.add_background, (void *)bound.set_effect, (void *)bound.set_bypass,
            (void *)bound.construct, (void *)bound.initialize};
        for (size_t i = 0; i < sizeof(targets) / sizeof(*targets); i++)
            if (!ani_section(image, targets[i], 1, code, IMAGE_SCN_MEM_WRITE)) return 0;
        const void *slots[] = {bound.roots[0], bound.roots[1], bound.transition_slot};
        for (size_t i = 0; i < 3; i++) if (((uintptr_t)slots[i] & 7) || !ani_section(image, slots[i], 8, data, IMAGE_SCN_MEM_EXECUTE)) return 0;
        if (!ani_section(image, bound.sample_rate, 4, data, IMAGE_SCN_MEM_EXECUTE) ||
            !ani_section(image, bound.duck_floor, 2, data, IMAGE_SCN_MEM_EXECUTE)) return 0;
        bound.subscriptions = subscriptions; bound.render_lock = limits->render_lock; bound.manager_slot = limits->manager_slot;
        bound.index_slot = (unsigned char **)registry->index_slot; bound.state_slot = (void **)state->state_slot;
        bound.owned = native->owned; bound.owned_count = native->owned_count;
        bound.touched = native->touched; bound.touched_count = native->touched_count;
        *native = bound; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
