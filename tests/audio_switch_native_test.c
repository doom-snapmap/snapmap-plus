#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/audio_subscription_native.c"
#include "../src/backend/audio_switch_native.c"

/* A fake native engine with the verified layouts: parameter nodes and their
 * sorted reverse lists, the 193-bucket subscription table at +0x610, modulator
 * consumer entries, switch nodes and the native helpers' observable rules. */
typedef struct param_fixture {
    sh_audio_parameter_table table;
    unsigned char *records[193];
    uint32_t total, padding;
    unsigned char rest[0x100];
} param_fixture;
typedef char param_layout[(offsetof(param_fixture, records) == 0x610 && offsetof(param_fixture, total) == 0xc18) ? 1 : -1];
typedef struct switch_fixture { sh_audio_switch_table table; unsigned char rest[0x700]; } switch_fixture;
typedef struct f_modulator {
    void *vtable, *next;
    uint32_t id, padding;
    unsigned char middle[0x44];
    sh_audio_parameter_vector list;
    unsigned char flag, padding2[7];
} f_modulator;
typedef char modulator_layout[(offsetof(f_modulator, id) == 0x10 && offsetof(f_modulator, list) == 0x60 &&
    offsetof(f_modulator, flag) == 0x70) ? 1 : -1];
typedef struct f_entry { void **items; uint32_t count, capacity; void *consumer; struct f_entry *next; } f_entry;

static param_fixture params;
static switch_fixture switches;
static f_entry *modulator_buckets[193];
static f_modulator modulators[4];
static size_t modulator_count;
static sh_audio_parameter_table *param_slot;
static void *switch_slot, *modulator_slot, *manager;
static CRITICAL_SECTION lock;
static int failures, pool = 11, calls, fail_at, allocations, growths, refreshes, notices;
static uint32_t notice_group, notice_value;
static float input;
static void *tracked[512];
static sh_audio_parameter_native parameters;
static sh_audio_subscription_native subscriptions;
static sh_audio_switch_native native;
static sh_audio_limits_native limits;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static int locked(void) { return lock.OwningThread == (HANDLE)(uintptr_t)GetCurrentThreadId(); }
static void *allocate(int p, size_t bytes)
{
    CHECK(p == pool && locked()); calls++;
    if (calls == fail_at) return NULL;
    void *value = calloc(1, bytes ? bytes : 1); CHECK(value); if (!value) return NULL;
    for (unsigned i = 0; i < 512; i++) if (!tracked[i]) { tracked[i] = value; allocations++; return value; }
    CHECK(0); free(value); return NULL;
}
static void release(int p, void *value)
{
    CHECK(p == pool && locked());
    for (unsigned i = 0; i < 512; i++) if (tracked[i] == value) { tracked[i] = NULL; allocations--; free(value); return; }
    CHECK(0);
}
static sh_audio_parameter_node *p_find(sh_audio_parameter_table *table, uint32_t id)
{
    for (sh_audio_parameter_node *node = table->buckets[id % 193]; node; node = node->next) if (node->id == id) return node;
    return NULL;
}
static sh_audio_parameter_node *p_ensure(sh_audio_parameter_table *table, uint32_t id)
{
    sh_audio_parameter_node *node = p_find(table, id);
    if (node) return node;
    if (!(node = allocate(pool, sizeof(*node)))) return NULL;
    node->id = id; node->next = table->buckets[id % 193]; table->buckets[id % 193] = node; table->count++;
    return node;
}
static void p_destroy(sh_audio_parameter_node *node)
{
    for (unsigned i = 0; i < 2; i++) if (node->subscribers[i].items) release(pool, node->subscribers[i].items);
    if (node->values.items) release(pool, node->values.items);
    memset(node->subscribers, 0, sizeof(node->subscribers)); memset(&node->values, 0, sizeof(node->values));
}
static int listed(const sh_audio_parameter_vector *list, const void *item)
{ for (uint32_t i = 0; i < list->count; i++) if (((void **)list->items)[i] == item) return 1; return 0; }
static int grow(sh_audio_parameter_vector *list, uint32_t capacity)
{
    void *items = allocate(pool, (size_t)capacity * sizeof(void *));
    if (!items) return 0;
    if (list->count) memcpy(items, list->items, (size_t)list->count * sizeof(void *));
    if (list->items) release(pool, list->items);
    list->items = items; list->capacity = capacity; return 1;
}
static int insert_sorted(sh_audio_parameter_vector *list, void *item)
{
    if (list->count == list->capacity && !grow(list, list->capacity * 2 + 1)) return 0;
    uint32_t at = 0; void **items = list->items;
    while (at < list->count && (uintptr_t)items[at] < (uintptr_t)item) at++;
    memmove(items + at + 1, items + at, (size_t)(list->count - at) * sizeof(void *));
    items[at] = item; list->count++; return 1;
}
static void remove_item(sh_audio_parameter_vector *list, const void *item)
{
    void **items = list->items;
    for (uint32_t i = 0; i < list->count; i++) if (items[i] == item) {
        memmove(items + i, items + i + 1, (size_t)(list->count - i - 1) * sizeof(void *)); list->count--; return;
    }
}
static const as_curve *curves_of(const void *record, uint32_t *count)
{
    const sh_audio_parameter_vector *curves = (const sh_audio_parameter_vector *)((const unsigned char *)record + 0x50);
    *count = curves->count; return curves->items;
}
/* Native reverse registration grows both lists to hold every subscriber plus
 * one before inserting; the adapter must have made that unnecessary. */
static int f_reverse_add(sh_audio_parameter_node *node, void *record, char selector)
{
    CHECK(locked());
    if (listed(node->subscribers + 1, record) || listed(node->subscribers, record)) return 1;
    uint32_t need = node->subscribers[0].count + node->subscribers[1].count + 1;
    for (unsigned i = 0; i < 2; i++) if (node->subscribers[i].capacity < need) {
        growths++; if (!grow(node->subscribers + i, need * 2)) return 0x34;
    }
    return insert_sorted(node->subscribers + (selector ? 0 : 1), record) ? 1 : 0x34;
}
static char f_reverse_remove(sh_audio_parameter_node *node, void *record)
{
    uint32_t count; const as_curve *curves = curves_of(record, &count);
    for (uint32_t i = 0; i < count; i++) if (curves[i].controller == node->id) return 0;
    remove_item(node->subscribers, record); remove_item(node->subscribers + 1, record); return 1;
}
static f_modulator *modulator(uint32_t id)
{ for (size_t i = 0; i < modulator_count; i++) if (modulators[i].id == id) return modulators + i; return NULL; }
static f_entry **entry_link(void *consumer)
{
    f_entry **link = modulator_buckets + (uint32_t)(uintptr_t)consumer % 193u;
    while (*link && (*link)->consumer != consumer) link = &(*link)->next;
    return link;
}
static int f_modulator_add(void *m, uint32_t id, void *record)
{
    CHECK(locked() && m == modulator_buckets);
    f_modulator *object = modulator(id); void *consumer; memcpy(&consumer, record, sizeof(consumer));
    if (!object) return 2;
    f_entry **link = entry_link(consumer);
    if (!*link) {
        f_entry *entry = allocate(pool, sizeof(*entry)); if (!entry) return 2;
        entry->consumer = consumer; entry->next = modulator_buckets[(uint32_t)(uintptr_t)consumer % 193u];
        modulator_buckets[(uint32_t)(uintptr_t)consumer % 193u] = entry; link = entry_link(consumer);
    }
    f_entry *entry = *link; sh_audio_parameter_vector items = {entry->items, entry->count, entry->capacity};
    if (!listed(&items, object)) {
        if (items.count == items.capacity && !grow(&items, items.capacity + 1)) return 2;
        ((void **)items.items)[items.count++] = object;
        entry->items = items.items; entry->count = items.count; entry->capacity = items.capacity;
    }
    if (listed(&object->list, record)) return 1;
    return insert_sorted(&object->list, record) ? 1 : 0x34;
}
static void f_modulator_remove(void *m, void *record, uint32_t controller)
{
    CHECK(locked() && m == modulator_buckets);
    void *consumer; memcpy(&consumer, record, sizeof(consumer));
    f_entry **link = entry_link(consumer), *entry = *link;
    if (!entry) return;
    for (uint32_t i = 0; i < entry->count;) {
        f_modulator *object = entry->items[i]; uint32_t count; const as_curve *curves = curves_of(record, &count); int uses = 0;
        if (controller && object->id != controller) { i++; continue; }
        for (uint32_t j = 0; j < count; j++) uses |= curves[j].controller == object->id;
        if (uses) { i++; continue; }
        remove_item(&object->list, record);
        int other = 0;
        for (uint32_t j = 0; j < object->list.count; j++) {
            void *owner; memcpy(&owner, ((void **)object->list.items)[j], sizeof(owner)); other |= owner == consumer;
        }
        if (other) { i++; continue; }
        memmove(entry->items + i, entry->items + i + 1, (size_t)(entry->count - i - 1) * sizeof(void *)); entry->count--;
    }
    if (!entry->count) {
        if (entry->items) release(pool, entry->items);
        *link = entry->next; release(pool, entry);
    }
}
/* Category 6 evaluates the step curve at the controller input and notifies
 * the switch node's consumers of values[index]. */
static void f_refresh(sh_audio_parameter_table *table, void *record, char flag)
{
    CHECK(locked() && table == param_slot && flag == 1); refreshes++;
    uint32_t category, count; memcpy(&category, (unsigned char *)record + 0x48, 4);
    const as_curve *curves = curves_of(record, &count);
    if (category != 6 || !count) return;
    sh_audio_switch_node *node; memcpy(&node, record, sizeof(node));
    const unsigned char *points = curves[0].descriptor.points; float y; memcpy(&y, points + 4, 4);
    for (uint32_t i = 0; i < curves[0].descriptor.count; i++) {
        float x; memcpy(&x, points + i * 12, 4);
        if (x <= input) memcpy(&y, points + i * 12 + 4, 4);
    }
    uint32_t index = (uint32_t)y;
    if (index < node->values.count) { notices++; notice_group = node->id; notice_value = ((uint32_t *)node->values.items)[index]; }
}
static int f_set_curve(sh_audio_native_curve *curve, const void *points, uint32_t count, uint32_t scaling)
{
    if (curve->points) { release(pool, curve->points); curve->points = NULL; }
    curve->count = curve->scaling = 0;
    if (!points || !count) return 0x1f;
    if (!(curve->points = allocate(pool, (size_t)count * 12))) return 0x34;
    memcpy(curve->points, points, (size_t)count * 12); curve->count = count; curve->scaling = scaling; return 1;
}
static void *f_switch_ensure(void *t, uint32_t id)
{
    sh_audio_switch_table *table = t;
    for (sh_audio_switch_node *node = table->buckets[id % 193]; node; node = node->next) if (node->id == id) return node;
    sh_audio_switch_node *node = allocate(pool, sizeof(*node));
    if (!node) return NULL;
    node->id = id; node->vtable = &native; node->next = table->buckets[id % 193]; table->buckets[id % 193] = node; table->count++;
    return node;
}

/* A mapping as the native STMG reader would have constructed it. */
static void native_map(uint32_t group, uint32_t controller, uint32_t type, uint32_t n, const float *x,
    const uint32_t *values, int bad_output)
{
    EnterCriticalSection(&lock);
    sh_audio_switch_node *node = f_switch_ensure(&switches.table, group);
    node->values = (sh_audio_parameter_vector){allocate(pool, n * 4), n, n};
    memcpy(node->values.items, values, n * 4); node->controller = controller; node->type = type;
    unsigned char *record = allocate(pool, 0x60), *points = malloc(n * 12);
    memcpy(record, &node, sizeof(node)); uint32_t parameter = 0x40, category = 6; memcpy(record + 8, &parameter, 4);
    as_default_key(record); memcpy(record + 0x48, &category, 4);
    as_curve *curve = allocate(pool, sizeof(as_curve));
    *(sh_audio_parameter_vector *)(record + 0x50) = (sh_audio_parameter_vector){curve, 1, 1};
    curve->controller = controller;
    for (uint32_t i = 0; i < n; i++) {
        float y = (float)i + (bad_output ? 0.5f : 0.0f); uint32_t shape = 4;
        memcpy(points + i * 12, x + i, 4); memcpy(points + i * 12 + 4, &y, 4); memcpy(points + i * 12 + 8, &shape, 4);
    }
    CHECK(f_set_curve(&curve->descriptor, points, n, 0) == 1); free(points);
    size_t bucket = as_bucket(node, parameter);
    memcpy(record + 0x10, &params.records[bucket], sizeof(void *)); params.records[bucket] = record; params.total++;
    if (type == 2) CHECK(f_modulator_add(modulator_buckets, controller, record) == 1);
    else CHECK(f_reverse_add(p_ensure(&params.table, controller), record, 1) == 1);
    LeaveCriticalSection(&lock);
}
static unsigned char *record_bytes(uint32_t group, uint32_t controller, unsigned char type, uint32_t n,
    const float *x, const uint32_t *values, size_t *length)
{
    *length = 13 + (size_t)n * 12; unsigned char *record = calloc(1, *length);
    memcpy(record, &group, 4); memcpy(record + 4, &controller, 4); record[8] = type; memcpy(record + 9, &n, 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t shape = 4; memcpy(record + 13 + i * 12, x + i, 4); memcpy(record + 17 + i * 12, values + i, 4);
        memcpy(record + 21 + i * 12, &shape, 4);
    }
    return record;
}
static sh_audio_shared_setting setting(unsigned char *body, size_t length, int edited)
{
    sh_audio_shared_setting s; memset(&s, 0, sizeof(s));
    s.record.kind = SH_AUDIO_SHARED_SWITCH; memcpy(&s.record.key[0], body, 4); s.record.length = length;
    s.body = body; s.edited = edited; return s;
}
static sh_audio_switch_node *live_switch(uint32_t group)
{ for (sh_audio_switch_node *n = switches.table.buckets[group % 193]; n; n = n->next) if (n->id == group) return n; return NULL; }
static unsigned char *live_record(const void *consumer)
{
    for (unsigned char *r = params.records[as_bucket(consumer, 0x40)]; r; r = *(unsigned char **)(r + 0x10))
        if (as_consumer(r) == consumer && as_parameter(r) == 0x40) return r;
    return NULL;
}

/* Observable native state; list capacities may grow (native-owned) but
 * membership, identities and mappings must match exactly. */
typedef struct print { unsigned char bytes[16384]; size_t length; } print;
static void put(print *p, const void *data, size_t n)
{ CHECK(p->length + n <= sizeof(p->bytes)); if (p->length + n <= sizeof(p->bytes)) { memcpy(p->bytes + p->length, data, n); p->length += n; } }
static void fingerprint(print *p)
{
    p->length = 0; put(p, &switches.table.count, 4);
    for (unsigned i = 0; i < 193; i++) for (sh_audio_switch_node *node = switches.table.buckets[i]; node; node = node->next) {
        put(p, &node, sizeof(node)); put(p, &node->values, sizeof(node->values)); put(p, &node->controller, 8);
        put(p, &node->consumers.count, 4);
        if (node->values.count) put(p, node->values.items, node->values.count * 4);
    }
    put(p, &params.table.count, 4); put(p, &params.total, 4);
    for (unsigned i = 0; i < 193; i++) for (sh_audio_parameter_node *node = params.table.buckets[i]; node; node = node->next) {
        put(p, &node, sizeof(node));
        for (unsigned l = 0; l < 2; l++) {
            put(p, &node->subscribers[l].count, 4);
            if (node->subscribers[l].count) put(p, node->subscribers[l].items, node->subscribers[l].count * sizeof(void *));
        }
    }
    for (unsigned i = 0; i < 193; i++) for (unsigned char *r = params.records[i]; r; r = *(unsigned char **)(r + 0x10)) {
        uint32_t count; const as_curve *curves = curves_of(r, &count);
        put(p, &r, sizeof(r)); put(p, r + 0x48, 8); put(p, &count, 4);
        for (uint32_t j = 0; j < count; j++) {
            put(p, curves + j, 8);
            if (curves[j].descriptor.count) put(p, curves[j].descriptor.points, curves[j].descriptor.count * 12);
        }
    }
    for (unsigned i = 0; i < 193; i++) for (f_entry *e = modulator_buckets[i]; e; e = e->next) {
        put(p, &e->consumer, sizeof(void *)); put(p, &e->count, 4);
        if (e->count) put(p, e->items, e->count * sizeof(void *));
    }
    for (size_t i = 0; i < modulator_count; i++) {
        put(p, &modulators[i].list.count, 4);
        if (modulators[i].list.count) put(p, modulators[i].list.items, modulators[i].list.count * sizeof(void *));
    }
}
static int same(const print *a, const print *b) { return a->length == b->length && !memcmp(a->bytes, b->bytes, a->length); }

static void teardown(void)
{
    CHECK(!native.pending);
    EnterCriticalSection(&lock);
    for (unsigned i = 0; i < 193; i++) for (unsigned char *r = params.records[i]; r;) {
        unsigned char *next = *(unsigned char **)(r + 0x10); uint32_t count; const as_curve *curves = curves_of(r, &count);
        for (uint32_t j = 0; j < count; j++) if (curves[j].descriptor.points) release(pool, curves[j].descriptor.points);
        if (curves) release(pool, (void *)curves);
        release(pool, r); r = next;
    }
    for (unsigned i = 0; i < 193; i++) for (sh_audio_parameter_node *node = params.table.buckets[i]; node;) {
        sh_audio_parameter_node *next = node->next; p_destroy(node); release(pool, node); node = next;
    }
    for (unsigned i = 0; i < 193; i++) for (sh_audio_switch_node *node = switches.table.buckets[i]; node;) {
        sh_audio_switch_node *next = node->next;
        if (node->values.items) release(pool, node->values.items);
        release(pool, node); node = next;
    }
    for (unsigned i = 0; i < 193; i++) for (f_entry *e = modulator_buckets[i]; e;) {
        f_entry *next = e->next; if (e->items) release(pool, e->items); release(pool, e); e = next;
    }
    for (size_t i = 0; i < modulator_count; i++) if (modulators[i].list.items) release(pool, modulators[i].list.items);
    LeaveCriticalSection(&lock);
    CHECK(!allocations);
    memset(&params, 0, sizeof(params)); memset(&switches, 0, sizeof(switches));
    memset(modulator_buckets, 0, sizeof(modulator_buckets)); memset(modulators, 0, sizeof(modulators)); modulator_count = 0;
    aw_originals_free(native.originals, native.original_count); native.originals = NULL; native.original_count = 0;
}
static void setup(void)
{
    param_slot = &params.table; switch_slot = &switches.table; modulator_slot = modulator_buckets; manager = &params;
    calls = fail_at = growths = refreshes = notices = 0; input = 0;
    parameters = (sh_audio_parameter_native){p_ensure, p_destroy, allocate, release, &pool, &param_slot, &manager, &lock, NULL};
    subscriptions = (sh_audio_subscription_native){&parameters, &modulator_slot, f_reverse_add, f_reverse_remove,
        f_modulator_add, f_modulator_remove, f_refresh, f_set_curve, &switch_slot, f_switch_ensure};
    limits.render_lock = &lock; limits.manager_slot = &manager;
    CHECK(sh_audio_switch_native_bind(&native, &subscriptions, &limits) && !native.original_count);
}
static int publish(const sh_audio_shared_policy *policy, char *error)
{
    sh_audio_switch_change change = {0};
    int result = sh_audio_switch_native_apply(&native, policy, &change, error, 256);
    if (result != 1) return result;
    if (!change.owner) return 1;
    CHECK(sh_audio_switch_native_ready(&change, error, 256));
    sh_audio_switch_native_accept(&change);
    CHECK(sh_audio_switch_native_release(&change, error, 256) && !native.pending && !change.owner);
    return 1;
}

static const float x3[] = {0, 10, 20}, x2[] = {0, 50};
static const uint32_t v3[] = {1001, 1002, 1003}, v2[] = {2001, 2002};

static void test_create_edit_and_return(void)
{
    char error[256]; size_t length3, length2;
    setup();
    unsigned char *mapped = record_bytes(500, 30, 0, 3, x3, v3, &length3);
    unsigned char *moved = record_bytes(500, 31, 0, 2, x2, v2, &length2);
    sh_audio_shared_setting edit = setting(mapped, length3, 1);
    sh_audio_shared_policy policy = {&edit, 1};
    /* 1. A new group: constructed, mapped, subscribed and notified once. */
    input = 12;
    CHECK(publish(&policy, error) == 1);
    sh_audio_switch_node *node = live_switch(500);
    CHECK(node && node->controller == 30 && node->type == 0 && node->values.count == 3 &&
        !memcmp(node->values.items, v3, 12));
    unsigned char *record = live_record(node);
    uint32_t count, category; const as_curve *curves = record ? curves_of(record, &count) : NULL;
    CHECK(record && count == 1 && curves[0].curve == 0 && curves[0].controller == 30 && curves[0].descriptor.count == 3);
    memcpy(&category, record + 0x48, 4); CHECK(category == 6 && record[0x4c] == 0);
    float y; memcpy(&y, (unsigned char *)curves[0].descriptor.points + 12 + 4, 4); CHECK(y == 1.0f);
    sh_audio_parameter_node *controller = p_find(&params.table, 30);
    CHECK(controller && listed(controller->subscribers, record) && !growths);
    CHECK(notices == 1 && notice_group == 500 && notice_value == 1002);
    CHECK(native.original_count == 1 && native.originals[0].group == 500 && !native.originals[0].record);
    /* 2. The same policy again is a no-op: no allocation, no notification. */
    int before_calls = calls, before_refreshes = refreshes; print a, b; fingerprint(&a);
    CHECK(publish(&policy, error) == 1);
    fingerprint(&b); CHECK(same(&a, &b) && calls == before_calls && refreshes == before_refreshes);
    /* 3. A changed edit keeps node identity, consumers and per-object values. */
    node->consumers.count = 1; node->has_value = 1;
    edit = setting(moved, length2, 1); input = 60;
    CHECK(publish(&policy, error) == 1);
    CHECK(live_switch(500) == node && node->controller == 31 && node->values.count == 2 && !memcmp(node->values.items, v2, 8));
    CHECK(node->consumers.count == 1 && node->has_value == 1);
    CHECK(!listed(controller->subscribers, record) && !controller->subscribers[0].count);
    record = live_record(node); sh_audio_parameter_node *other = p_find(&params.table, 31);
    CHECK(record && other && listed(other->subscribers, record) && notice_value == 2002);
    CHECK(native.original_count == 1 && !native.originals[0].record);
    /* 4. No longer edited: the group returns to its original (unmapped). */
    policy.count = 0;
    CHECK(publish(&policy, error) == 1);
    CHECK(live_switch(500) == node && !node->controller && !node->values.count && !node->values.items && !live_record(node));
    CHECK(!other->subscribers[0].count && !native.original_count);
    /* 5. Unedited baseline groups nobody edited stay native, even if absent. */
    sh_audio_shared_setting baseline = setting(mapped, length3, 0);
    unsigned char *unmapped = record_bytes(600, 30, 0, 3, x3, v3, &length3);
    sh_audio_shared_setting untouched = setting(unmapped, length3, 0);
    sh_audio_shared_setting rows[2] = {baseline, untouched}; policy = (sh_audio_shared_policy){rows, 2};
    before_calls = calls; fingerprint(&a);
    CHECK(publish(&policy, error) == 1);
    fingerprint(&b); CHECK(same(&a, &b) && calls == before_calls && !live_switch(600));
    teardown(); free(mapped); free(moved); free(unmapped);
}

static void test_native_original_roundtrip(void)
{
    char error[256]; size_t length, edited_length;
    setup();
    native_map(700, 40, 0, 3, x3, v3, 0);
    sh_audio_switch_node *node = live_switch(700); unsigned char *original_record = live_record(node);
    unsigned char *expected = record_bytes(700, 40, 0, 3, x3, v3, &length);
    unsigned char *edited = record_bytes(700, 41, 0, 2, x2, v2, &edited_length);
    sh_audio_shared_setting edit = setting(edited, edited_length, 1); sh_audio_shared_policy policy = {&edit, 1};
    CHECK(publish(&policy, error) == 1);
    CHECK(native.original_count == 1 && native.originals[0].length == length && !memcmp(native.originals[0].record, expected, length));
    CHECK(live_switch(700) == node && node->controller == 41 && live_record(node) != original_record);
    /* The map leaves: the reconstructed original is published again. */
    policy.count = 0; input = 25;
    CHECK(publish(&policy, error) == 1);
    CHECK(node->controller == 40 && node->values.count == 3 && !memcmp(node->values.items, v3, 12) && !native.original_count);
    uint32_t count; const as_curve *curves = curves_of(live_record(node), &count);
    CHECK(count == 1 && curves[0].controller == 40 && notice_value == 1003);
    /* A native mapping whose outputs are not value indexes is refused. */
    native_map(710, 42, 0, 3, x3, v3, 1);
    unsigned char *bad = record_bytes(710, 43, 0, 2, x2, v2, &edited_length);
    edit = setting(bad, edited_length, 1); policy.count = 1;
    print a, b; fingerprint(&a); int held = allocations;
    CHECK(publish(&policy, error) == 0 && strstr(error, "unrecognized") && !native.pending);
    fingerprint(&b); CHECK(same(&a, &b) && allocations == held);
    teardown(); free(expected); free(edited); free(bad);
}

static void test_modulators(void)
{
    char error[256]; size_t length;
    setup();
    modulators[0].id = 900; modulator_count = 1;
    unsigned char *driven = record_bytes(800, 900, 2, 3, x3, v3, &length);
    sh_audio_shared_setting edit = setting(driven, length, 1); sh_audio_shared_policy policy = {&edit, 1};
    CHECK(publish(&policy, error) == 1);
    sh_audio_switch_node *node = live_switch(800); unsigned char *record = live_record(node);
    f_entry *entry = *entry_link(node);
    CHECK(node && record && entry && entry->count == 1 && entry->items[0] == &modulators[0] && listed(&modulators[0].list, record));
    CHECK(!p_find(&params.table, 900));
    /* A modulator the engine has not constructed fails registration: every
     * change reverts, including the node constructed for it. */
    size_t missing_length; unsigned char *missing = record_bytes(810, 901, 2, 2, x2, v2, &missing_length);
    sh_audio_shared_setting rows[2] = {edit, setting(missing, missing_length, 1)}; policy = (sh_audio_shared_policy){rows, 2};
    print a, b; fingerprint(&a); int held = allocations;
    CHECK(publish(&policy, error) == 0 && !native.pending && !live_switch(810));
    fingerprint(&b); CHECK(same(&a, &b) && allocations == held && native.original_count == 1);
    /* Returning to the original unregisters the modulator entry entirely. */
    policy.count = 0;
    CHECK(publish(&policy, error) == 1 && !*entry_link(node) && !modulators[0].list.count && !live_record(node));
    teardown(); free(driven); free(missing);
}

/* Every allocation failure leaves the engine exactly as it was. */
static void test_allocation_failures(void)
{
    char error[256]; size_t length, moved_length; int completed = 0;
    unsigned char *fresh = record_bytes(500, 30, 0, 3, x3, v3, &length);
    unsigned char *moved = record_bytes(700, 41, 0, 2, x2, v2, &moved_length);
    for (int at = 1; at < 64 && !completed; at++) {
        setup();
        native_map(700, 40, 0, 3, x3, v3, 0);
        sh_audio_parameter_node *controller = p_find(&params.table, 40);
        EnterCriticalSection(&lock); CHECK(grow(controller->subscribers, 1) && grow(controller->subscribers + 1, 1));
        LeaveCriticalSection(&lock);
        sh_audio_shared_setting rows[2] = {setting(fresh, length, 1), setting(moved, moved_length, 1)};
        sh_audio_shared_policy policy = {rows, 2};
        print a, b; fingerprint(&a); int held = allocations;
        calls = 0; fail_at = at; growths = 0;
        int result = publish(&policy, error);
        fail_at = 0;
        if (result == 1) { completed = at; CHECK(!growths && live_switch(500) && live_switch(700)->controller == 41); }
        else {
            CHECK(result == 0 && !native.pending);
            fingerprint(&b); CHECK(same(&a, &b) && allocations == held && !native.original_count);
        }
        teardown();
    }
    CHECK(completed > 8);
    printf("switch publication: %d allocation failure points reverted\n", completed - 1);
    free(fresh); free(moved);
}

/* A later leg's failure restores mappings, subscriptions and notifications;
 * a constructed node something attached to stays with the engine. */
static void test_restore(void)
{
    char error[256]; size_t length, moved_length;
    setup();
    native_map(700, 40, 0, 3, x3, v3, 0);
    unsigned char *fresh = record_bytes(500, 30, 0, 3, x3, v3, &length);
    unsigned char *moved = record_bytes(700, 41, 0, 2, x2, v2, &moved_length);
    sh_audio_shared_setting rows[2] = {setting(fresh, length, 1), setting(moved, moved_length, 1)};
    sh_audio_shared_policy policy = {rows, 2};
    print a, b; fingerprint(&a); int held = allocations;
    sh_audio_switch_change change = {0}; input = 25;
    CHECK(sh_audio_switch_native_apply(&native, &policy, &change, error, sizeof(error)) == 1 && native.pending == &change);
    CHECK(sh_audio_switch_native_ready(&change, error, sizeof(error)) && notice_value == 2001);
    /* Other subscribers join the previous controller while the receipt waits. */
    sh_audio_parameter_node *controller = p_find(&params.table, 40);
    EnterCriticalSection(&lock);
    for (uintptr_t i = 1; i <= 6; i++) CHECK(f_reverse_add(controller, (void *)(i * 16), 0) == 1);
    CHECK(controller->subscribers[1].count == controller->subscribers[1].capacity);
    LeaveCriticalSection(&lock);
    growths = 0;
    CHECK(sh_audio_switch_native_restore(&change, error, sizeof(error)) && !native.pending && !change.owner && !growths);
    CHECK(notice_value == 1003 && !live_switch(500) && !native.original_count);
    EnterCriticalSection(&lock);
    for (uintptr_t i = 1; i <= 6; i++) remove_item(controller->subscribers + 1, (void *)(i * 16));
    LeaveCriticalSection(&lock);
    fingerprint(&b); CHECK(same(&a, &b) && allocations == held);
    /* A consumer attached to the constructed node keeps it after restore. */
    CHECK(sh_audio_switch_native_apply(&native, &policy, &change, error, sizeof(error)) == 1);
    sh_audio_switch_node *created = live_switch(500); created->consumers.count = 1;
    CHECK(sh_audio_switch_native_restore(&change, error, sizeof(error)) && !native.pending);
    CHECK(live_switch(500) == created && !created->controller && !created->values.count && !live_record(created));
    created->consumers.count = 0;
    /* A changed mapping cannot be restored over: the receipt is retained. */
    CHECK(sh_audio_switch_native_apply(&native, &policy, &change, error, sizeof(error)) == 1);
    sh_audio_switch_node *mapped = live_switch(700); uint32_t saved = mapped->controller; mapped->controller = 99;
    CHECK(!sh_audio_switch_native_ready(&change, error, sizeof(error)));
    CHECK(!sh_audio_switch_native_restore(&change, error, sizeof(error)) && native.pending == &change);
    mapped->controller = saved;
    CHECK(sh_audio_switch_native_restore(&change, error, sizeof(error)) && !native.pending);
    /* Nothing represented: no receipt and no lock. */
    sh_audio_shared_policy empty = {NULL, 0}; calls = 0;
    CHECK(sh_audio_switch_native_apply(&native, &empty, &change, error, sizeof(error)) == 1 && !change.owner && !calls);
    teardown(); free(fresh); free(moved);
}

int main(void)
{
    InitializeCriticalSection(&lock);
    test_create_edit_and_return();
    test_native_original_roundtrip();
    test_modulators();
    test_allocation_failures();
    test_restore();
    DeleteCriticalSection(&lock);
    if (failures) { fprintf(stderr, "%d audio switch checks failed\n", failures); return 1; }
    puts("audio switch native checks passed");
    return 0;
}
