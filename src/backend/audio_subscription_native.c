#include "audio_subscription_native.h"
#include "audio_native_image.h"
#include <stdio.h>
#include <stdlib.h>

enum { AS_PREPARED = 1, AS_PUBLISHED, AS_RESTORED, AS_FAILED };
typedef struct as_curve { int32_t curve; uint32_t controller; sh_audio_native_curve descriptor; } as_curve;
typedef char as_curve_layout[sizeof(as_curve) == 24 ? 1 : -1];
typedef struct as_registration { sh_audio_parameter_node *node; char selector; } as_registration;
/* Modulator-manager entry for one consumer: modulator vector, key, next. */
typedef struct as_modulator_entry { void **items; uint32_t count, capacity; void *consumer; struct as_modulator_entry *next; } as_modulator_entry;
struct sh_audio_subscription_row {
    void *consumer;
    uint32_t parameter;
    char selector;
    unsigned char *prepared, *previous;
    const sh_audio_subscription_request *request;
    as_registration *registrations; /* previous record's parameter lists */
    size_t registration_count;
    uint32_t *modulators;           /* previous record's modulator IDs */
    size_t modulator_count;
    int linked, unlinked;
};
struct sh_audio_subscription_nodes {
    uint32_t id;
    sh_audio_parameter_node *node;
    int created;
    unsigned additions;
};
#define AS_RECORD 0x60
#define AS_KEY 0x18
#define AS_CATEGORY 0x48
#define AS_MODE 0x4c
#define AS_CURVES 0x50

static int as_fail(char *error, size_t capacity, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); return 0; }
static sh_audio_parameter_vector *as_curves(unsigned char *record) { return (sh_audio_parameter_vector *)(record + AS_CURVES); }
static void *as_consumer(const unsigned char *record) { void *value; memcpy(&value, record, sizeof(value)); return value; }
static uint32_t as_parameter(const unsigned char *record) { uint32_t value; memcpy(&value, record + 8, 4); return value; }
static unsigned char **as_next(unsigned char *record) { return (unsigned char **)(record + 0x10); }
static unsigned char **as_buckets(sh_audio_parameter_table *table) { return (unsigned char **)((unsigned char *)table + 0x610); }
static uint32_t *as_total(sh_audio_parameter_table *table) { return (uint32_t *)((unsigned char *)table + 0xc18); }
static size_t as_bucket(const void *consumer, uint32_t parameter)
{ return (size_t)((uint32_t)(uintptr_t)consumer + parameter) % 193u; }
static unsigned char **as_link(sh_audio_parameter_table *table, const void *consumer, uint32_t parameter)
{
    unsigned char **link = as_buckets(table) + as_bucket(consumer, parameter);
    while (*link && (as_consumer(*link) != consumer || as_parameter(*link) != parameter)) link = as_next(*link);
    return link;
}
static sh_audio_parameter_node *as_node(const sh_audio_parameter_table *table, uint32_t id)
{
    uint64_t steps = 0;
    for (sh_audio_parameter_node *node = table->buckets[id % 193]; node && steps <= table->count; node = node->next, steps++)
        if (node->id == id) return node;
    return NULL;
}
static int as_listed(const sh_audio_parameter_vector *list, const void *record)
{
    for (uint32_t i = 0; i < list->count; i++) if (((void **)list->items)[i] == record) return 1;
    return 0;
}
static void as_default_key(unsigned char *record)
{ memset(record + AS_KEY, 0, 48); record[AS_KEY + 24] = 0xff; record[AS_KEY + 32] = 0xff; }
static void as_free_record(sh_audio_subscription_native *native, int pool, unsigned char *record)
{
    if (!record) return;
    sh_audio_parameter_vector *curves = as_curves(record);
    for (uint32_t i = 0; curves->items && i < curves->count; i++) {
        as_curve *curve = (as_curve *)curves->items + i;
        if (curve->descriptor.points) native->parameters->release(pool, curve->descriptor.points);
    }
    if (curves->items) native->parameters->release(pool, curves->items);
    native->parameters->release(pool, record);
}
static void as_host_free(sh_audio_subscription_change *change)
{
    for (size_t i = 0; i < change->count; i++) { free(change->rows[i].registrations); free(change->rows[i].modulators); }
    free(change->rows); free(change->nodes);
    change->rows = NULL; change->nodes = NULL; change->count = change->node_count = 0;
}
static int as_node_add(sh_audio_subscription_change *change, uint32_t id)
{
    for (size_t i = 0; i < change->node_count; i++) if (change->nodes[i].id == id) { change->nodes[i].additions++; return 1; }
    sh_audio_subscription_nodes *grown = realloc(change->nodes, (change->node_count + 1) * sizeof(*grown));
    if (!grown) return 0;
    change->nodes = grown; memset(grown + change->node_count, 0, sizeof(*grown));
    grown[change->node_count].id = id; grown[change->node_count++].additions = 1; return 1;
}
/* Native registration grows both reverse lists to hold every subscriber of the
 * node plus one before inserting; growing them here first keeps registration
 * allocation-free. The new arrays belong to the node like native growth. */
static int as_reserve(sh_audio_subscription_change *change, sh_audio_parameter_node *node, uint32_t additions)
{
    sh_audio_subscription_native *native = change->owner;
    uint64_t need = (uint64_t)node->subscribers[0].count + node->subscribers[1].count + additions + 1;
    if (need > UINT32_MAX / sizeof(void *)) return 0;
    for (unsigned list = 0; list < 2; list++) {
        sh_audio_parameter_vector *live = node->subscribers + list;
        if (live->capacity >= need) continue;
        void *items = native->parameters->allocate(change->pool, (size_t)need * sizeof(void *));
        if (!items) return 0;
        if (live->count) memcpy(items, live->items, (size_t)live->count * sizeof(void *));
        void *old = live->items;
        live->items = items; live->capacity = (uint32_t)need;
        if (old) native->parameters->release(change->pool, old);
    }
    return 1;
}

int sh_audio_subscription_prepare(sh_audio_subscription_native *native,
    const sh_audio_subscription_request *requests, size_t count,
    sh_audio_subscription_change *change, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!native || !native->parameters || !native->parameters->allocate || !native->parameters->release ||
        !native->parameters->pool || !native->set_curve || !change || change->owner || (count && !requests))
        return as_fail(error, capacity, "audio subscription adapter is unavailable");
    memset(change, 0, sizeof(*change));
    change->owner = native; change->pool = *native->parameters->pool;
    if (count > SIZE_MAX / sizeof(*change->rows) || !(change->rows = calloc(count ? count : 1, sizeof(*change->rows)))) goto memory;
    change->count = count;
    for (size_t i = 0; i < count; i++) {
        const sh_audio_subscription_request *request = requests + i;
        sh_audio_subscription_row *row = change->rows + i;
        if (!request->consumer || (request->count && !request->curves) || request->count > UINT32_MAX / sizeof(as_curve)) goto invalid;
        for (size_t j = 0; j < i; j++)
            if (requests[j].consumer == request->consumer && requests[j].parameter == request->parameter) goto invalid;
        row->consumer = request->consumer; row->parameter = request->parameter;
        row->selector = request->selector; row->request = request;
        if (!request->count) continue;
        unsigned char *record = native->parameters->allocate(change->pool, AS_RECORD);
        if (!record) goto memory;
        memset(record, 0, AS_RECORD); row->prepared = record;
        memcpy(record, &request->consumer, sizeof(void *)); memcpy(record + 8, &request->parameter, 4);
        as_default_key(record); memcpy(record + AS_CATEGORY, &request->category, 4); memcpy(record + AS_MODE, &request->mode, 4);
        as_curve *curves = native->parameters->allocate(change->pool, request->count * sizeof(as_curve));
        if (!curves) goto memory;
        memset(curves, 0, request->count * sizeof(as_curve));
        *as_curves(record) = (sh_audio_parameter_vector){curves, (uint32_t)request->count, (uint32_t)request->count};
        for (size_t j = 0; j < request->count; j++) {
            const sh_audio_subscription_curve *want = request->curves + j;
            for (size_t k = 0; k < j; k++) if (request->curves[k].curve == want->curve) goto invalid;
            curves[j].curve = want->curve; curves[j].controller = want->controller;
            if (!want->controller || !want->count || !want->points) goto invalid;
            /* The native setter owns the copy and any scaling conversion. */
            if (native->set_curve(&curves[j].descriptor, want->points, want->count, want->scaling) != 1 ||
                !curves[j].descriptor.points || curves[j].descriptor.count != want->count) goto memory;
            if (want->type != 2 && !as_node_add(change, want->controller)) goto memory;
        }
    }
    change->state = AS_PREPARED; return 1;
invalid:
    as_fail(error, capacity, "audio subscription request is malformed"); goto failed;
memory:
    as_fail(error, capacity, "cannot prepare native audio subscriptions");
failed:
    for (size_t i = 0; i < change->count; i++) as_free_record(native, change->pool, change->rows[i].prepared);
    as_host_free(change); memset(change, 0, sizeof(*change)); return 0;
}

/* Native removal checks whether another curve of the record still uses the
 * controller; a record leaving entirely has no remaining curve, so its count is
 * hidden while the native removal helpers run. The vector itself is untouched. */
static void as_unregister(sh_audio_subscription_change *change, unsigned char *record)
{
    sh_audio_subscription_native *native = change->owner;
    sh_audio_parameter_vector *curves = as_curves(record), saved = *curves;
    curves->count = 0;
    for (uint32_t i = 0; i < saved.count; i++) {
        sh_audio_parameter_node *node = as_node(change->parameters, ((as_curve *)saved.items)[i].controller);
        if (node) native->reverse_remove(node, record);
    }
    if (change->modulators) native->modulator_remove(change->modulators, record, 0);
    *curves = saved;
}
/* Registration evidence of a live record, read before it leaves: parameter
 * nodes (with their list) and modulator objects naming it. */
static int as_capture(sh_audio_subscription_change *change, sh_audio_subscription_row *row, unsigned char *record)
{
    sh_audio_parameter_vector *curves = as_curves(record);
    row->registrations = calloc(curves->count ? curves->count : 1, sizeof(*row->registrations));
    if (!row->registrations) return 0;
    for (uint32_t i = 0; i < curves->count; i++) {
        sh_audio_parameter_node *node = as_node(change->parameters, ((as_curve *)curves->items)[i].controller);
        if (!node) continue;
        int first = as_listed(node->subscribers, record), second = as_listed(node->subscribers + 1, record);
        if (!first && !second) continue;
        int known = 0;
        for (size_t j = 0; j < row->registration_count; j++) known |= row->registrations[j].node == node;
        if (!known) row->registrations[row->registration_count++] = (as_registration){node, (char)(first != 0)};
    }
    if (!change->modulators) return 1;
    const as_modulator_entry *entry = ((as_modulator_entry **)change->modulators)[(uint32_t)(uintptr_t)as_consumer(record) % 193u];
    for (; entry; entry = entry->next) if (entry->consumer == as_consumer(record)) break;
    if (!entry) return 1;
    row->modulators = calloc(entry->count ? entry->count : 1, sizeof(*row->modulators));
    if (!row->modulators) return 0;
    for (uint32_t i = 0; i < entry->count; i++) {
        const unsigned char *modulator = entry->items[i];
        if (!modulator || !as_listed((const sh_audio_parameter_vector *)(modulator + 0x60), record)) continue;
        memcpy(row->modulators + row->modulator_count++, modulator + 0x10, 4);
    }
    return 1;
}
static int as_register(sh_audio_subscription_change *change, sh_audio_subscription_row *row)
{
    sh_audio_subscription_native *native = change->owner;
    const sh_audio_subscription_request *request = row->request;
    for (size_t i = 0; i < request->count; i++) {
        const sh_audio_subscription_curve *curve = request->curves + i;
        if (curve->type == 2) {
            if (!change->modulators || native->modulator_add(change->modulators, curve->controller, row->prepared) != 1) return 0;
            continue;
        }
        sh_audio_parameter_node *node = as_node(change->parameters, curve->controller);
        if (!node || native->reverse_add(node, row->prepared, row->selector) != 1) return 0;
    }
    return 1;
}
static int as_reregister(sh_audio_subscription_change *change, sh_audio_subscription_row *row)
{
    sh_audio_subscription_native *native = change->owner; int ok = 1;
    for (size_t i = 0; i < row->registration_count; i++)
        ok &= native->reverse_add(row->registrations[i].node, row->previous, row->registrations[i].selector) == 1;
    for (size_t i = 0; i < row->modulator_count; i++)
        ok &= change->modulators && native->modulator_add(change->modulators, row->modulators[i], row->previous) == 1;
    return ok;
}
static void as_link_record(sh_audio_subscription_change *change, unsigned char *record)
{
    unsigned char **bucket = as_buckets(change->parameters) + as_bucket(as_consumer(record), as_parameter(record));
    *as_next(record) = *bucket; *bucket = record; (*as_total(change->parameters))++;
}
static void as_unlink_record(sh_audio_subscription_change *change, unsigned char *record)
{
    unsigned char **link = as_link(change->parameters, as_consumer(record), as_parameter(record));
    if (*link == record) { *link = *as_next(record); *as_next(record) = NULL; (*as_total(change->parameters))--; }
}
/* Reverse one row. 1 restored, 0 a native re-registration failed. */
static int as_revert(sh_audio_subscription_change *change, sh_audio_subscription_row *row)
{
    int ok = 1;
    if (row->linked) {
        as_unregister(change, row->prepared); as_unlink_record(change, row->prepared); row->linked = 0;
    }
    if (row->unlinked) {
        as_link_record(change, row->previous); row->unlinked = 0;
        ok = as_reregister(change, row);
    }
    return ok;
}
static int as_revert_all(sh_audio_subscription_change *change, size_t through)
{
    int ok = 1;
    for (size_t i = through; i > 0; i--) ok &= as_revert(change, change->rows + i - 1);
    for (size_t i = 0; i < change->node_count; i++) {
        sh_audio_subscription_nodes *entry = change->nodes + i;
        if (entry->created && entry->node && !entry->node->subscribers[0].count &&
            !entry->node->subscribers[1].count && !entry->node->has_value && !entry->node->values.count && !entry->node->transitions) {
            sh_audio_parameter_node **link = change->parameters->buckets + entry->id % 193;
            while (*link && *link != entry->node) link = &(*link)->next;
            if (*link) {
                *link = entry->node->next; change->parameters->count--;
                change->owner->parameters->destroy(entry->node);
                change->owner->parameters->release(change->pool, entry->node);
                entry->node = NULL; entry->created = 0;
            }
        }
    }
    return ok;
}
int sh_audio_subscription_publish(sh_audio_subscription_change *change, char *error, size_t capacity)
{
    size_t row = 0;
    if (error && capacity) *error = 0;
    if (!change || !change->owner || change->state != AS_PREPARED) return as_fail(error, capacity, "audio subscription change is not prepared");
    sh_audio_subscription_native *native = change->owner;
    change->parameters = *native->parameters->parameter_slot;
    change->modulators = native->modulator_slot ? *native->modulator_slot : NULL;
    if (!change->parameters) return as_fail(error, capacity, "native audio parameter manager is unavailable");
    /* Parameter controllers exist and both reverse lists can take every
     * addition without the native registration allocating. */
    for (size_t i = 0; i < change->node_count; i++) {
        sh_audio_subscription_nodes *entry = change->nodes + i;
        entry->node = as_node(change->parameters, entry->id);
        if (!entry->node) {
            entry->node = native->parameters->ensure(change->parameters, entry->id);
            if (!entry->node) { as_fail(error, capacity, "cannot create native audio parameter controller"); goto failed; }
            entry->created = 1;
        }
        if (!as_reserve(change, entry->node, entry->additions)) {
            as_fail(error, capacity, "cannot reserve native audio subscription lists"); goto failed;
        }
    }
    for (; row < change->count; row++) {
        sh_audio_subscription_row *r = change->rows + row;
        unsigned char **link = as_link(change->parameters, r->consumer, r->parameter);
        if (*link) {
            r->previous = *link;
            if (!as_capture(change, r, r->previous)) { as_fail(error, capacity, "cannot record native audio subscription"); goto failed; }
            as_unregister(change, r->previous); as_unlink_record(change, r->previous); r->unlinked = 1;
        }
        if (r->prepared) {
            as_link_record(change, r->prepared); r->linked = 1;
            if (!as_register(change, r)) { row++; as_fail(error, capacity, "native audio subscription registration failed"); goto failed; }
        }
    }
    change->state = AS_PUBLISHED; return 1;
failed:
    if (!as_revert_all(change, row)) { change->uncertain = 1; change->state = AS_FAILED; return -1; }
    change->state = AS_FAILED; return 0;
}
int sh_audio_subscription_refresh(sh_audio_subscription_change *change)
{
    if (!change || !change->owner || change->uncertain || (change->state != AS_PUBLISHED && change->state != AS_RESTORED))
        return 0;
    for (size_t i = 0; i < change->count; i++) {
        unsigned char *record = change->state == AS_PUBLISHED ? change->rows[i].prepared : change->rows[i].previous;
        if (record) change->owner->refresh(change->parameters, record, 1);
    }
    return 1;
}
int sh_audio_subscription_ready(const sh_audio_subscription_change *change)
{
    if (!change || change->state != AS_PUBLISHED || change->uncertain) return 0;
    for (size_t i = 0; i < change->count; i++) {
        const sh_audio_subscription_row *row = change->rows + i;
        unsigned char **link = as_link(change->parameters, row->consumer, row->parameter);
        if (*link != row->prepared) return 0;
    }
    return 1;
}
int sh_audio_subscription_restore(sh_audio_subscription_change *change, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (!change || change->state != AS_PUBLISHED || change->uncertain) return as_fail(error, capacity, "audio subscription change cannot be restored");
    /* Other subscribers may have joined these nodes since publication. */
    for (size_t i = 0; i < change->count; i++) for (size_t j = 0; j < change->rows[i].registration_count; j++) {
        sh_audio_parameter_node *node = change->rows[i].registrations[j].node; uint32_t additions = 0;
        for (size_t k = 0; k < change->count; k++) for (size_t m = 0; m < change->rows[k].registration_count; m++)
            additions += change->rows[k].registrations[m].node == node;
        if (!as_reserve(change, node, additions))
            return as_fail(error, capacity, "cannot reserve native audio subscription lists for restore");
    }
    if (!as_revert_all(change, change->count)) {
        change->uncertain = 1; change->state = AS_FAILED;
        return as_fail(error, capacity, "native audio subscription restore could not re-register a record");
    }
    change->state = AS_RESTORED; return 1;
}
void sh_audio_subscription_release(sh_audio_subscription_change *change)
{
    if (!change || !change->owner) return;
    sh_audio_subscription_native *native = change->owner;
    int published = change->state == AS_PUBLISHED;
    for (size_t i = 0; i < change->count; i++) {
        sh_audio_subscription_row *row = change->rows + i;
        /* A record is freed only once it is neither linked nor registered. */
        if (published) as_free_record(native, change->pool, row->previous);
        else if (!row->linked) as_free_record(native, change->pool, row->prepared);
    }
    as_host_free(change); memset(change, 0, sizeof(*change));
}

static const unsigned char *as_live(const sh_audio_subscription_native *native, const void *consumer, uint32_t parameter)
{
    sh_audio_parameter_table *table = native && native->parameters && native->parameters->parameter_slot ?
        *native->parameters->parameter_slot : NULL;
    return table ? *as_link(table, consumer, parameter) : NULL;
}
int sh_audio_subscription_valid(const sh_audio_subscription_native *native)
{
    sh_audio_parameter_table *table = native && native->parameters && native->parameters->parameter_slot ?
        *native->parameters->parameter_slot : NULL;
    uint64_t total = 0;
    if (!table) return 0;
    for (size_t i = 0; i < 193; i++)
        for (unsigned char *record = as_buckets(table)[i]; record; record = *as_next(record))
            if (++total > *as_total(table) || as_bucket(as_consumer(record), as_parameter(record)) != i) return 0;
    return total == *as_total(table);
}
int sh_audio_subscription_matches(const sh_audio_subscription_native *native,
    const sh_audio_subscription_request *request)
{
    if (!request) return 0;
    const unsigned char *record = as_live(native, request->consumer, request->parameter);
    if (!request->count || !record) return !request->count && !record;
    uint32_t category, mode; memcpy(&category, record + AS_CATEGORY, 4); memcpy(&mode, record + AS_MODE, 4);
    const sh_audio_parameter_vector *curves = (const sh_audio_parameter_vector *)(record + AS_CURVES);
    if (category != request->category || mode != request->mode || curves->count != request->count || !curves->items) return 0;
    for (size_t i = 0; i < request->count; i++) {
        const sh_audio_subscription_curve *want = request->curves + i; int found = 0;
        for (uint32_t j = 0; j < curves->count && !found; j++) {
            const as_curve *have = (const as_curve *)curves->items + j;
            found = have->curve == want->curve && have->controller == want->controller &&
                have->descriptor.count == want->count && have->descriptor.scaling == want->scaling &&
                have->descriptor.points && want->points &&
                !memcmp(have->descriptor.points, want->points, (size_t)want->count * 12);
        }
        if (!found) return 0;
    }
    return 1;
}
size_t sh_audio_subscription_parameters(const sh_audio_subscription_native *native, const void *consumer,
    uint32_t *out, size_t capacity)
{
    sh_audio_parameter_table *table = native && native->parameters && native->parameters->parameter_slot ?
        *native->parameters->parameter_slot : NULL;
    size_t found = 0;
    if (!table) return 0;
    for (size_t i = 0; i < 193; i++)
        for (unsigned char *record = as_buckets(table)[i]; record; record = *as_next(record)) {
            if (as_consumer(record) != consumer) continue;
            if (out && found < capacity) out[found] = as_parameter(record);
            found++;
        }
    for (size_t i = 1; out && i < found && i < capacity; i++)
        for (size_t j = i; j > 0 && out[j - 1] > out[j]; j--) { uint32_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return found;
}
size_t sh_audio_subscription_live(const sh_audio_subscription_native *native, const void *consumer,
    uint32_t parameter, sh_audio_subscription_curve *first)
{
    const unsigned char *record = as_live(native, consumer, parameter);
    if (!record) return 0;
    const sh_audio_parameter_vector *curves = (const sh_audio_parameter_vector *)(record + AS_CURVES);
    if (first && curves->count && curves->items) {
        const as_curve *curve = curves->items;
        *first = (sh_audio_subscription_curve){curve->curve, curve->controller, 0,
            curve->descriptor.points, curve->descriptor.count, curve->descriptor.scaling};
    }
    return curves->items ? curves->count : 0;
}

int sh_audio_subscription_native_bind(sh_audio_subscription_native *native, const unsigned char *image,
    const sig_result *results, size_t count, sh_audio_parameter_native *parameters,
    const sh_audio_environment_native *environment)
{
    const unsigned char *reader = NULL; sh_audio_subscription_native bound = {0};
    if (!native || !parameters || !parameters->parameter_slot || !parameters->allocate || !environment ||
        !environment->set_curve || (count && !results)) return 0;
    memset(native, 0, sizeof(*native));
    for (size_t i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, "AudioStateSettingsRead")) {
        if (reader || results[i].status != SIG_OK) return 0;
        reader = (const unsigned char *)results[i].addr;
    }
    const DWORD code = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE, data = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    if (!ani_section(image, reader, 0x438, code, IMAGE_SCN_MEM_WRITE)) return 0;
    __try {
        const unsigned char rcx[] = {0x48, 0x8b, 0x0d}, call[] = {0xe8}, jump[] = {0xe9};
        /* STMG reader: switch manager and its setter. */
        bound.switch_slot = ani_rip(reader + 0x2d7, rcx, 3);
        const unsigned char *setter = ani_rip(reader + 0x2e3, call, 1);
        if (!bound.switch_slot || !ani_section(image, setter, 0x95, code, IMAGE_SCN_MEM_WRITE)) return 0;
        bound.switch_ensure = (void *(*)(void *, uint32_t))ani_rip(setter + 0x15, call, 1);
        if (ani_rip(setter + 0x2c, rcx, 3) != (void *)parameters->parameter_slot) return 0;
        const unsigned char *build = ani_rip(setter + 0x90, jump, 1);
        if (!ani_section(image, build, 0xf6, code, IMAGE_SCN_MEM_WRITE) ||
            ani_rip(build + 0xde, rcx, 3) != (void *)parameters->parameter_slot) return 0;
        const unsigned char *wrapper = ani_rip(build + 0xf1, call, 1);
        if (!ani_section(image, wrapper, 0x6d, code, IMAGE_SCN_MEM_WRITE)) return 0;
        const unsigned char *core = ani_rip(wrapper + 0x68, call, 1);
        if (!ani_section(image, core, 0x3f2, code, IMAGE_SCN_MEM_WRITE)) return 0;
        /* Subscription core call sites, identical on both renderers. */
        bound.modulator_slot = ani_rip(core + 0x198, rcx, 3);
        if (!bound.modulator_slot || bound.modulator_slot != ani_rip(core + 0x321, rcx, 3)) return 0;
        bound.reverse_remove = (char (*)(sh_audio_parameter_node *, void *))ani_rip(core + 0x18e, call, 1);
        bound.modulator_remove = (void (*)(void *, void *, uint32_t))ani_rip(core + 0x1a5, call, 1);
        bound.modulator_add = (int (*)(void *, uint32_t, void *))ani_rip(core + 0x32b, call, 1);
        bound.reverse_add = (int (*)(sh_audio_parameter_node *, void *, char))ani_rip(core + 0x34e, call, 1);
        bound.refresh = (void (*)(sh_audio_parameter_table *, void *, char))ani_rip(core + 0x364, call, 1);
        /* Cross-check helpers other adapters verified independently. */
        if ((void *)environment->set_curve != ani_rip(core + 0x20e, call, 1) ||
            (void *)parameters->allocate != ani_rip(core + 0x232, call, 1) ||
            (void *)parameters->ensure != ani_rip(core + 0x335, call, 1)) return 0;
        const void *targets[] = {bound.switch_ensure, (void *)bound.reverse_remove, (void *)bound.modulator_remove,
            (void *)bound.modulator_add, (void *)bound.reverse_add, (void *)bound.refresh};
        for (size_t i = 0; i < sizeof(targets) / sizeof(*targets); i++)
            if (!ani_section(image, targets[i], 1, code, IMAGE_SCN_MEM_WRITE)) return 0;
        const void *slots[] = {bound.switch_slot, bound.modulator_slot};
        for (size_t i = 0; i < 2; i++)
            if (((uintptr_t)slots[i] & 7) || !ani_section(image, slots[i], 8, data, IMAGE_SCN_MEM_EXECUTE)) return 0;
        if (bound.switch_slot == bound.modulator_slot || (void *)bound.switch_slot == (void *)parameters->parameter_slot ||
            (void *)bound.modulator_slot == (void *)parameters->parameter_slot) return 0;
        bound.parameters = parameters; bound.set_curve = environment->set_curve;
        *native = bound; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
