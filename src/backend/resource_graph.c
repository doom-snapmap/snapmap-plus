#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "execution_context.h"
#include "resource_graph.h"
#include "native_filename.h"

typedef struct rg_phase {
    size_t *children, count;
    int complete, active, raced;
    int expanded_inheritance;
} rg_phase;
typedef struct rg_producer {
    char *name;
    size_t *children, count;
    struct rg_producer *next;
} rg_producer;
typedef struct rg_node {
    char *type, *name;
    uint64_t hash;
    size_t generation;
    rg_phase phases[2];
    rg_producer *producers;
    unsigned history; /* SH_RESOURCE_GRAPH_HISTORY_*: never cleared by a reparse. */
} rg_node;

static SRWLOCK g_lock = SRWLOCK_INIT;
static rg_node *g_nodes;
static size_t g_count, g_capacity, *g_hash, g_hash_capacity;
static int g_failed;
static volatile LONG g_size_history;
typedef struct rg_scope {
    uint64_t serial;
    size_t node, generation, *children, count, capacity;
    unsigned phase;
    int failed, requests, expanded_inheritance;
    struct rg_scope *parent;
} rg_scope;
typedef struct rg_context {
    uint64_t id;
    rg_scope *top;
    struct rg_context *next;
} rg_context;
static rg_context *g_contexts;
static volatile LONG64 g_scope_serial;
static void rg_retire(uint64_t context);
static sh_execution_owner g_execution = {rg_retire};
static rg_context *rg_context_find(uint64_t id)
{
    for (rg_context *c = g_contexts; c; c = c->next) if (c->id == id) return c;
    return NULL;
}

static void rg_producer_free(rg_producer *producer)
{
    while (producer) {
        rg_producer *next = producer->next;
        free(producer->name); free(producer->children); free(producer); producer = next;
    }
}

static char rg_fold(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c == '\\' ? '/' : c;
}
static uint64_t rg_hash_key(const char *type, const char *name)
{
    uint64_t hash = 14695981039346656037ULL;
    unsigned i;
    if (!*type) name = sh_native_virtual_path(name);
    for (i = 0; i < 2; i++) {
        const char *s = i ? name : type;
        do { hash = (hash ^ (unsigned char)rg_fold(*s)) * 1099511628211ULL; } while (*s++);
    }
    return hash;
}
static int rg_equal(const char *a, const char *b)
{
    do { if (rg_fold(*a) != rg_fold(*b)) return 0; } while (*a++ && *b++);
    return 1;
}
static char *rg_copy(const char *s)
{
    size_t i, length = strlen(s);
    char *out = (char *)malloc(length + 1);
    if (out) for (i = 0; i <= length; i++) out[i] = rg_fold(s[i]);
    return out;
}
static int rg_rehash(size_t capacity)
{
    size_t *table = (size_t *)calloc(capacity, sizeof(*table)), i;
    if (!table) return 0;
    for (i = 0; i < g_count; i++) {
        size_t slot = (size_t)g_nodes[i].hash & (capacity - 1);
        while (table[slot]) slot = (slot + 1) & (capacity - 1);
        table[slot] = i + 1;
    }
    free(g_hash); g_hash = table; g_hash_capacity = capacity; return 1;
}
static size_t rg_find(const char *type, const char *name, uint64_t hash)
{
    size_t slot;
    if (!*type) name = sh_native_virtual_path(name);
    if (!g_hash_capacity) return SIZE_MAX;
    slot = (size_t)hash & (g_hash_capacity - 1);
    while (g_hash[slot]) {
        size_t i = g_hash[slot] - 1;
        if (g_nodes[i].hash == hash && rg_equal(type, g_nodes[i].type) && rg_equal(name, g_nodes[i].name)) return i;
        slot = (slot + 1) & (g_hash_capacity - 1);
    }
    return SIZE_MAX;
}
static size_t rg_intern(const char *type, const char *name)
{
    uint64_t hash;
    size_t index, slot;
    rg_node node = {0};
    if (!type || !name) return SIZE_MAX;
    if (!*type) name = sh_native_virtual_path(name);
    if (!*name) return SIZE_MAX;
    hash = rg_hash_key(type, name);
    index = rg_find(type, name, hash);
    if (index != SIZE_MAX) return index;
    if (g_count == g_capacity) {
        size_t capacity = g_capacity ? g_capacity * 2 : 1024;
        rg_node *nodes;
        if (capacity < g_capacity || capacity > SIZE_MAX / sizeof(*nodes)) goto bad;
        nodes = (rg_node *)realloc(g_nodes, capacity * sizeof(*nodes));
        if (!nodes) goto bad;
        g_nodes = nodes; g_capacity = capacity;
    }
    if (!g_hash_capacity || g_count >= g_hash_capacity / 2) {
        size_t capacity = g_hash_capacity ? g_hash_capacity * 2 : 2048;
        if (capacity < g_hash_capacity || !rg_rehash(capacity)) goto bad;
    }
    node.type = rg_copy(type); node.name = rg_copy(name); node.hash = hash;
    node.phases[0].complete = !*type; /* Provider requests are concrete leaves. */
    if (!node.type || !node.name) { free(node.type); free(node.name); goto bad; }
    index = g_count++; g_nodes[index] = node;
    slot = (size_t)hash & (g_hash_capacity - 1);
    while (g_hash[slot]) slot = (slot + 1) & (g_hash_capacity - 1);
    g_hash[slot] = index + 1; return index;
bad:
    g_failed = 1; return SIZE_MAX;
}
static void rg_append(rg_scope *frame, size_t child)
{
    size_t i;
    if (!frame || frame->node == SIZE_MAX) return;
    if (child == SIZE_MAX) { frame->failed = 1; return; }
    if (child == frame->node) return;
    for (i = 0; i < frame->count; i++) if (frame->children[i] == child) return;
    if (frame->count == frame->capacity) {
        size_t capacity = frame->capacity ? frame->capacity * 2 : 16;
        size_t *children;
        if (capacity < frame->capacity || capacity > SIZE_MAX / sizeof(*children)) { frame->failed = 1; return; }
        children = (size_t *)realloc(frame->children, capacity * sizeof(*children));
        if (!children) { frame->failed = 1; return; }
        frame->children = children; frame->capacity = capacity;
    }
    frame->children[frame->count++] = child;
}
/* All scope and graph operations below hold g_lock. Execution-context
 * retirement is drained before taking that lock and never reads a token's
 * caller stack or enters native code. */
static void rg_scope_finish(rg_scope *scope, int parsed, int publish)
{
    if (scope->node != SIZE_MAX && scope->node < g_count) {
        rg_node *node = &g_nodes[scope->node];
        rg_phase *record = &node->phases[scope->phase];
        if (record->active > 0) record->active--;
        else { scope->failed = 1; record->complete = 0; }
        if (scope->generation == node->generation && publish) {
            free(record->children); record->children = scope->children; record->count = scope->count;
            record->complete = parsed && !scope->failed && !record->active && !record->raced;
            record->expanded_inheritance = scope->expanded_inheritance;
            scope->children = NULL;
        }
    }
    free(scope->children); free(scope);
}
static void rg_retire(uint64_t id)
{
    AcquireSRWLockExclusive(&g_lock);
    rg_context **at = &g_contexts;
    while (*at && (*at)->id != id) at = &(*at)->next;
    if (*at) {
        rg_context *c = *at; *at = c->next;
        while (c->top) {
            rg_scope *scope = c->top; c->top = scope->parent;
            scope->failed = 1; rg_scope_finish(scope, 0, 1);
        }
        free(c);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
static void rg_invalid(const sh_resource_graph_frame *frame, rg_context *current)
{
    if (current && current->top) current->top->failed = 1;
    if (!frame || !frame->serial) return;
    rg_context *owner = rg_context_find(frame->context);
    for (rg_scope *s = owner ? owner->top : NULL; s; s = s->parent)
        if (s->serial == frame->serial) { s->failed = 1; break; }
    if (frame->node < g_count && frame->phase < 2 &&
        g_nodes[frame->node].generation == frame->generation)
        g_nodes[frame->node].phases[frame->phase].complete = 0;
}
static int rg_begin(sh_resource_graph_frame *frame, const char *type, const char *name,
    unsigned phase, int only_missing, int pause, int requests, int expanded)
{
    if (!frame) return 0;
    memset(frame, 0, sizeof(*frame)); frame->node = SIZE_MAX;
    uint64_t id = sh_execution_context(&g_execution, 1);
    rg_scope *scope = calloc(1, sizeof(*scope));
    if (scope) { scope->serial = sh_execution_next(&g_scope_serial); scope->node = SIZE_MAX; }
    AcquireSRWLockExclusive(&g_lock);
    if (!id || !scope || !scope->serial) { g_failed = 1; goto bad; }
    rg_context *c = rg_context_find(id);
    if (!c) {
        c = calloc(1, sizeof(*c));
        if (!c) { g_failed = 1; goto bad; }
        c->id = id; c->next = g_contexts; g_contexts = c;
    }
    scope->phase = phase; scope->requests = requests; scope->expanded_inheritance = expanded;
    if (!pause) {
        scope->node = rg_intern(type, name);
        if (scope->node != SIZE_MAX) {
            rg_node *node = &g_nodes[scope->node];
            rg_phase *record = &node->phases[phase];
            if (only_missing && (record->complete || record->active || node->phases[0].active)) goto bad;
            if (record->active == INT_MAX || (!phase && node->generation == SIZE_MAX)) {
                g_failed = 1; scope->node = SIZE_MAX;
            } else {
                if (!phase) {
                    node->generation++;
                    rg_producer_free(node->producers); node->producers = NULL;
                    for (unsigned i = 0; i < 2; i++) {
                        free(node->phases[i].children); node->phases[i].children = NULL;
                        node->phases[i].count = 0; node->phases[i].complete = 0;
                    }
                }
                scope->generation = node->generation;
                record->raced = record->active != 0; record->active++; record->complete = 0;
            }
        }
        rg_append(c->top, scope->node);
        scope->failed = scope->node == SIZE_MAX;
    }
    scope->parent = c->top; c->top = scope;
    frame->context = id; frame->serial = scope->serial; frame->node = scope->node;
    frame->generation = scope->generation; frame->phase = scope->phase;
    ReleaseSRWLockExclusive(&g_lock); return 1;
bad:
    ReleaseSRWLockExclusive(&g_lock); free(scope); return 0;
}
void sh_resource_graph_begin(sh_resource_graph_frame *frame, const char *type, const char *name)
{ rg_begin(frame,type,name,0,0,0,0,0); }
void sh_resource_graph_begin_state(sh_resource_graph_frame *frame, const char *type, const char *name,
    int expanded_inheritance)
{ rg_begin(frame,type,name,1,0,0,0,expanded_inheritance != 0); }
int sh_resource_graph_begin_missing_state(sh_resource_graph_frame *frame, const char *type,
    const char *name, int expanded_inheritance)
{ return rg_begin(frame,type,name,1,1,0,0,expanded_inheritance != 0); }
void sh_resource_graph_pause(sh_resource_graph_frame *frame)
{ rg_begin(frame,NULL,NULL,0,0,1,0,0); }
static void rg_end(sh_resource_graph_frame *frame, int parsed, int publish)
{
    uint64_t id = sh_execution_context(NULL,0);
    AcquireSRWLockExclusive(&g_lock);
    rg_context *c = rg_context_find(id);
    if (!frame || !frame->serial || frame->context != id || !c) { rg_invalid(frame,c); goto done; }
    rg_scope *scope = c->top;
    while (scope && scope->serial != frame->serial) scope = scope->parent;
    if (!scope) { rg_invalid(frame,c); goto done; }
    int ordered = c->top == scope;
    while (c->top != scope) {
        rg_scope *lost = c->top; c->top = lost->parent;
        lost->failed = 1; rg_scope_finish(lost,0,1);
    }
    c->top = scope->parent;
    if (!ordered) scope->failed = 1;
    rg_scope_finish(scope,parsed && ordered,publish);
done:
    ReleaseSRWLockExclusive(&g_lock);
}
void sh_resource_graph_end(sh_resource_graph_frame *frame, int parsed) { rg_end(frame,parsed,1); }
void sh_resource_graph_discard(sh_resource_graph_frame *frame) { rg_end(frame,0,0); }
static void rg_reference(const char *type, const char *name, int request)
{
    if (!name || !*name) return;
    uint64_t id = sh_execution_context(NULL,0);
    AcquireSRWLockExclusive(&g_lock);
    rg_context *c = rg_context_find(id); rg_scope *scope = c ? c->top : NULL;
    if (scope && scope->node != SIZE_MAX && (!request || scope->requests))
        rg_append(scope,rg_intern(type,name));
    ReleaseSRWLockExclusive(&g_lock);
}
void sh_resource_graph_reference(const char *type, const char *name) { rg_reference(type,name,0); }
void sh_resource_graph_incomplete(void)
{
    uint64_t id = sh_execution_context(NULL,0);
    AcquireSRWLockExclusive(&g_lock);
    rg_context *c = rg_context_find(id);
    if (c && c->top && c->top->node != SIZE_MAX) c->top->failed = 1;
    ReleaseSRWLockExclusive(&g_lock);
}
int sh_resource_graph_frame_current(const sh_resource_graph_frame *frame)
{
    uint64_t id = sh_execution_context(NULL,0); int current = 0;
    AcquireSRWLockShared(&g_lock);
    rg_context *c = rg_context_find(id);
    current = frame && frame->serial && frame->context == id && c && c->top && c->top->serial == frame->serial;
    ReleaseSRWLockShared(&g_lock); return current;
}
void sh_resource_graph_expanded_inheritance(sh_resource_graph_frame *frame, int expanded)
{
    uint64_t id = sh_execution_context(NULL,0);
    AcquireSRWLockExclusive(&g_lock);
    rg_context *c = rg_context_find(id);
    if (frame && frame->serial && frame->context == id && c && c->top && c->top->serial == frame->serial)
        c->top->expanded_inheritance = expanded != 0;
    else rg_invalid(frame,c);
    ReleaseSRWLockExclusive(&g_lock);
}
void sh_resource_graph_begin_requests(sh_resource_graph_frame *frame, const char *type, const char *name)
{ rg_begin(frame,type,name,0,0,0,1,0); }
void sh_resource_graph_begin_state_requests(sh_resource_graph_frame *frame, const char *type,
    const char *name, int expanded_inheritance)
{ rg_begin(frame,type,name,1,0,0,1,expanded_inheritance != 0); }
int sh_resource_graph_recording_requests(void)
{
    uint64_t id = sh_execution_context(NULL,0); int recording = 0;
    AcquireSRWLockShared(&g_lock);
    rg_context *c = rg_context_find(id);
    recording = c && c->top && c->top->node != SIZE_MAX && c->top->requests;
    ReleaseSRWLockShared(&g_lock); return recording;
}
void sh_resource_graph_request(const char *type, const char *name) { rg_reference(type,name,1); }
void sh_resource_graph_file(const char *path) { sh_resource_graph_reference("", path); }
int sh_resource_graph_producer_inputs(const char *producer, const char *path,
    const char *const *inputs, size_t count)
{
    rg_producer *pending = NULL, **slot, *old;
    size_t node, i, filled = 0;
    int ok = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (!producer || !*producer || !path || !*path || (count && !inputs) ||
        count > SIZE_MAX / sizeof(size_t)) goto done;
    node = rg_intern("", path);
    if (node == SIZE_MAX) goto done;
    if (count) {
        pending = (rg_producer *)calloc(1, sizeof(*pending));
        if (!pending) goto done;
        pending->name = rg_copy(producer);
        pending->children = (size_t *)malloc(count * sizeof(*pending->children));
        if (!pending->name || !pending->children) goto done;
        for (i = 0; i < count; i++) {
            size_t child = rg_intern("", inputs[i]), j;
            if (child == SIZE_MAX) goto done;
            if (child == node) continue;
            for (j = 0; j < filled; j++) if (pending->children[j] == child) break;
            if (j == filled) pending->children[filled++] = child;
        }
        pending->count = filled;
    }
    /* Interning inputs can relocate g_nodes, so obtain the slot only now. */
    slot = &g_nodes[node].producers;
    while (*slot && !rg_equal((*slot)->name, producer)) slot = &(*slot)->next;
    old = *slot;
    if (pending && filled) { pending->next = old ? old->next : NULL; *slot = pending; pending = NULL; }
    else *slot = old ? old->next : NULL;
    if (old) { old->next = NULL; rg_producer_free(old); }
    ok = 1;
done:
    if (!ok) g_failed = 1;
    ReleaseSRWLockExclusive(&g_lock);
    rg_producer_free(pending); return ok;
}
void sh_resource_graph_history_mark(const char *type, const char *name, unsigned bits)
{
    size_t node;
    if (!bits) return;
    if (!type || !name || !*name) { sh_resource_graph_size_history_invalidate(); return; }
    AcquireSRWLockExclusive(&g_lock);
    node = rg_intern(type, name);
    if (node != SIZE_MAX) g_nodes[node].history |= bits;
    ReleaseSRWLockExclusive(&g_lock);
    if (node == SIZE_MAX) sh_resource_graph_size_history_invalidate();
}
unsigned sh_resource_graph_history(const char *type, const char *name)
{
    unsigned bits = 0;
    size_t node;
    if (!type || !name || !*name) return 0;
    AcquireSRWLockShared(&g_lock);
    node = rg_find(type, name, rg_hash_key(type, name));
    if (node != SIZE_MAX) bits = g_nodes[node].history;
    ReleaseSRWLockShared(&g_lock);
    return bits;
}
/* 0 not begun, 1 begun, -1 invalid (sticky). */
void sh_resource_graph_size_history_begin(void) { InterlockedCompareExchange(&g_size_history, 1, 0); }
void sh_resource_graph_size_history_invalidate(void) { InterlockedExchange(&g_size_history, -1); }
int sh_resource_graph_size_history_ready(void) { return InterlockedCompareExchange(&g_size_history, 0, 0) == 1; }
int sh_resource_graph_recording(void)
{
    uint64_t id = sh_execution_context(NULL,0); int recording = 0;
    AcquireSRWLockShared(&g_lock);
    rg_context *c = rg_context_find(id); recording = c && c->top && c->top->node != SIZE_MAX;
    ReleaseSRWLockShared(&g_lock); return recording;
}
int sh_resource_graph_source_active(const char *type, const char *name)
{
    int active = 0; uint64_t id = sh_execution_context(NULL,0);
    if (!type || !name) return 0;
    AcquireSRWLockShared(&g_lock);
    rg_context *c = rg_context_find(id); rg_scope *scope = c ? c->top : NULL;
    if (scope && !scope->phase && scope->node < g_count) {
        const rg_node *node = &g_nodes[scope->node];
        active = scope->generation == node->generation && rg_equal(type,node->type) && rg_equal(name,node->name);
    }
    ReleaseSRWLockShared(&g_lock); return active;
}

static int rg_complete(const rg_node *node)
{
    return node->phases[0].complete && !node->phases[0].active &&
        (strcmp(node->type, "entitydef") ||
         (node->phases[1].complete && !node->phases[1].active));
}

sh_resource_graph_status sh_resource_graph_walk_status(const char *type, const char *name,
    sh_resource_graph_visitor visitor, void *context)
{
    typedef struct rg_visit { size_t node; unsigned char mode; } rg_visit;
    size_t root, position = 0, count = 0;
    rg_visit *queue = NULL;
    unsigned char *visited = NULL;
    sh_resource_graph_status result = SH_RESOURCE_GRAPH_ERROR;
    int follow, covered = 1;
    unsigned char mode;
    if (!type || !name || !visitor) return SH_RESOURCE_GRAPH_ERROR;
    AcquireSRWLockShared(&g_lock);
    root = rg_find(type, name, rg_hash_key(type, name));
    if (g_failed) goto done;
    if (root == SIZE_MAX) { result = SH_RESOURCE_GRAPH_INCOMPLETE; goto done; }
    if (g_count > SIZE_MAX / 2 / sizeof(*queue)) goto done;
    queue = (rg_visit *)malloc(2 * g_count * sizeof(*queue));
    visited = (unsigned char *)calloc(g_count, 1);
    if (!queue || !visited) goto done;
    follow = visitor(context, NULL, NULL, g_nodes[root].type, g_nodes[root].name);
    if (follow < 0) goto done;
    if (!follow) { result = SH_RESOURCE_GRAPH_COMPLETE; goto done; }
    mode = !strcmp(g_nodes[root].type, "entitydef") ? 2 : 1;
    queue[count].node = root; queue[count++].mode = mode; visited[root] = mode;
    while (position < count) {
        size_t parent_index = queue[position].node;
        const rg_node *parent = &g_nodes[parent_index];
        size_t i;
        unsigned phase, phases;
        int source_only_inheritance;
        mode = queue[position++].mode;
        if (mode == 1 && (visited[parent_index] & 2)) continue; /* A full visit is queued. */
        phases = mode == 2 ? 2 : 1;
        source_only_inheritance = mode == 1 ||
            (parent->phases[1].complete && parent->phases[1].expanded_inheritance);
        for (phase = 0; phase < phases; phase++) {
            if (!parent->phases[phase].complete || parent->phases[phase].active) covered = 0;
            for (i = 0; i < parent->phases[phase].count; i++) {
                size_t child = parent->phases[phase].children[i];
                const rg_node *node = &g_nodes[child];
                unsigned char required = !strcmp(node->type, "entitydef") ? 2 : 1;
                if (required == 2 && !phase && source_only_inheritance &&
                    !strcmp(parent->type, "entitydef")) required = 1;
                follow = visitor(context, parent->type, parent->name, node->type, node->name);
                if (follow < 0) goto done;
                if (follow && !(visited[child] & required) && !(required == 1 && (visited[child] & 2))) {
                    visited[child] |= required;
                    queue[count].node = child; queue[count++].mode = required;
                }
            }
        }
        {
            const rg_producer *producer;
            for (producer = parent->producers; producer; producer = producer->next)
                for (i = 0; i < producer->count; i++) {
                    size_t child = producer->children[i];
                    const rg_node *node = &g_nodes[child];
                    follow = visitor(context, parent->type, parent->name, node->type, node->name);
                    if (follow < 0) goto done;
                    if (follow && !visited[child]) {
                        visited[child] = 1;
                        queue[count].node = child; queue[count++].mode = 1;
                    }
                }
        }
    }
    result = covered ? SH_RESOURCE_GRAPH_COMPLETE : SH_RESOURCE_GRAPH_INCOMPLETE;
done:
    free(queue); free(visited); ReleaseSRWLockShared(&g_lock); return result;
}
int sh_resource_graph_walk(const char *type, const char *name, sh_resource_graph_visitor visitor, void *context)
{
    return sh_resource_graph_walk_status(type, name, visitor, context) == SH_RESOURCE_GRAPH_COMPLETE;
}
void sh_resource_graph_impact_free(sh_resource_graph_impact *impact)
{
    size_t i;
    if (!impact) return;
    for (i = 0; i < impact->count; i++) {
        free(impact->items[i].type); free(impact->items[i].name);
    }
    free(impact->items); memset(impact, 0, sizeof(*impact));
}
static int rg_impact_compare(const void *a, const void *b)
{
    const sh_resource_graph_identity *left = (const sh_resource_graph_identity *)a;
    const sh_resource_graph_identity *right = (const sh_resource_graph_identity *)b;
    int cmp = strcmp(left->type, right->type);
    return cmp ? cmp : strcmp(left->name, right->name);
}
sh_resource_graph_status sh_resource_graph_consumers(const sh_resource_graph_identity *seeds,
    size_t count, sh_resource_graph_impact *out)
{ return sh_resource_graph_consumers_bounded(seeds, count, NULL, NULL, out); }
sh_resource_graph_status sh_resource_graph_consumers_bounded(const sh_resource_graph_identity *seeds,
    size_t count, sh_resource_graph_barrier barrier, void *context, sh_resource_graph_impact *out)
{
    typedef struct rg_reverse_edge { size_t parent, next; } rg_reverse_edge;
    rg_reverse_edge *edges = NULL;
    size_t *heads = NULL, *queue = NULL;
    unsigned char *visited = NULL;
    size_t i, total = 0, written = 0, queued = 0, position = 0;
    int covered = 1;
    sh_resource_graph_status result = SH_RESOURCE_GRAPH_ERROR;
    if (!out) return result;
    memset(out, 0, sizeof(*out));
    if (count && !seeds) return result;
    for (i = 0; i < count; i++)
        if (!seeds[i].type || !seeds[i].name || !*seeds[i].name) return result;
    if (!count) return SH_RESOURCE_GRAPH_COMPLETE;
    AcquireSRWLockShared(&g_lock);
    if (g_failed) goto done;
    if (!g_count) { result = SH_RESOURCE_GRAPH_INCOMPLETE; goto done; }
    if (g_count > SIZE_MAX / sizeof(*queue)) goto done;
    /* Build reverse adjacency once. A repeated whole-graph search per input
     * or per dependency depth would make large installed inventories costly. */
    for (i = 0; i < g_count; i++) {
        const rg_node *node = &g_nodes[i];
        const rg_producer *producer;
        unsigned phase;
        if (!rg_complete(node) || node->phases[1].active) covered = 0;
        for (phase = 0; phase < 2; phase++) {
            if (node->phases[phase].count > SIZE_MAX - total) goto done;
            total += node->phases[phase].count;
        }
        for (producer = node->producers; producer; producer = producer->next) {
            if (producer->count > SIZE_MAX - total) goto done;
            total += producer->count;
        }
    }
    if (total > SIZE_MAX / sizeof(*edges)) goto done;
    heads = (size_t *)malloc(g_count * sizeof(*heads));
    queue = (size_t *)malloc(g_count * sizeof(*queue));
    visited = (unsigned char *)calloc(g_count, 1);
    if (total) edges = (rg_reverse_edge *)malloc(total * sizeof(*edges));
    if (!heads || !queue || !visited || (total && !edges)) goto done;
    for (i = 0; i < g_count; i++) heads[i] = SIZE_MAX;
    for (i = 0; i < g_count; i++) {
        const rg_node *node = &g_nodes[i];
        const rg_producer *producer = node->producers;
        unsigned phase = 0;
        /* Phase sets and independent producer sets obey the same edge rule. */
        while (phase < 2 || producer) {
            const size_t *children;
            size_t j, length;
            if (phase < 2) {
                children = node->phases[phase].children;
                length = node->phases[phase++].count;
            } else {
                children = producer->children; length = producer->count;
                producer = producer->next;
            }
            for (j = 0; j < length; j++) {
                size_t child = children[j];
                if (child >= g_count || written >= total) goto done;
                edges[written] = (rg_reverse_edge){i, heads[child]};
                heads[child] = written++;
            }
        }
    }
    for (i = 0; i < count; i++) {
        size_t node = rg_find(seeds[i].type, seeds[i].name, rg_hash_key(seeds[i].type, seeds[i].name));
        if (node == SIZE_MAX) { covered = 0; continue; }
        if (!visited[node]) { visited[node] = 1; queue[queued++] = node; }
    }
    while (position < queued) {
        size_t edge;
        for (edge = heads[queue[position++]]; edge != SIZE_MAX; edge = edges[edge].next) {
            size_t parent = edges[edge].parent;
            if (visited[parent]) continue;
            visited[parent] = 1;
            if (!barrier || !barrier(context, g_nodes[parent].type, g_nodes[parent].name))
                queue[queued++] = parent;
        }
    }
    if (queued > SIZE_MAX / sizeof(*out->items)) goto done;
    if (queued) {
        out->items = (sh_resource_graph_identity *)calloc(queued, sizeof(*out->items));
        if (!out->items) goto done;
        for (i = 0; i < queued; i++) {
            const rg_node *node = &g_nodes[queue[i]];
            sh_resource_graph_identity *item = &out->items[out->count++];
            item->type = rg_copy(node->type); item->name = rg_copy(node->name);
            if (!item->type || !item->name) goto done;
        }
        qsort(out->items, out->count, sizeof(*out->items), rg_impact_compare);
    }
    result = covered ? SH_RESOURCE_GRAPH_COMPLETE : SH_RESOURCE_GRAPH_INCOMPLETE;
done:
    free(edges); free(heads); free(queue); free(visited);
    ReleaseSRWLockShared(&g_lock);
    if (result == SH_RESOURCE_GRAPH_ERROR) sh_resource_graph_impact_free(out);
    return result;
}
sh_resource_graph_status sh_resource_graph_affected(const char *const *paths,
    size_t count, sh_resource_graph_impact *out)
{
    sh_resource_graph_identity *seeds = NULL;
    sh_resource_graph_status result;
    if (!out) return SH_RESOURCE_GRAPH_ERROR;
    memset(out, 0, sizeof(*out));
    if ((count && !paths) || count > SIZE_MAX / sizeof(*seeds)) return SH_RESOURCE_GRAPH_ERROR;
    if (count) {
        seeds = (sh_resource_graph_identity *)malloc(count * sizeof(*seeds));
        if (!seeds) return SH_RESOURCE_GRAPH_ERROR;
        for (size_t i = 0; i < count; i++) {
            seeds[i].type = ""; seeds[i].name = (char *)paths[i];
        }
    }
    result = sh_resource_graph_consumers(seeds, count, out);
    free(seeds); return result;
}

/* The bounded closure of sh_resource_graph_consumers_bounded, with coverage judged on that closure
 * alone: COMPLETE when every seed has a node and every node the walk visits (consumers it returns
 * and barrier holders it stops at) has every recorded phase complete. Otherwise INCOMPLETE, naming
 * the first unobserved seed or incompletely observed consumer in first. */
sh_resource_graph_status sh_resource_graph_consumers_observed(const sh_resource_graph_identity *seeds,
    size_t count, sh_resource_graph_barrier barrier, void *context, sh_resource_graph_impact *out,
    char *first, size_t capacity)
{
    sh_resource_graph_status result = SH_RESOURCE_GRAPH_ERROR;
    size_t *queue = NULL;
    unsigned char *visited = NULL;
    size_t queued = 0, position = 0;
    int covered = 1;
    if (first && capacity) *first = 0;
    if (!out) return result;
    memset(out, 0, sizeof(*out));
    if (count && !seeds) return result;
    for (size_t i = 0; i < count; i++) if (!seeds[i].type || !seeds[i].name || !*seeds[i].name) return result;
    if (!count) return SH_RESOURCE_GRAPH_COMPLETE;
    AcquireSRWLockShared(&g_lock);
    if (g_failed || g_count > SIZE_MAX / sizeof(*queue)) goto done;
    if (g_count && (!(queue = (size_t *)malloc(g_count * sizeof(*queue))) ||
        !(visited = (unsigned char *)calloc(g_count, 1)))) goto done;
    for (size_t i = 0; i < count; i++) {
        size_t node = rg_find(seeds[i].type, seeds[i].name, rg_hash_key(seeds[i].type, seeds[i].name));
        if (node == SIZE_MAX) {
            if (covered && first && capacity) snprintf(first, capacity, "%s:%s (no recorded node)", seeds[i].type, seeds[i].name);
            covered = 0; continue;
        }
        if (!visited[node]) { visited[node] = 1; queue[queued++] = node; }
    }
    /* A reverse scan per dequeued node keeps this independent of any whole-graph coverage. */
    while (position < queued) {
        size_t target = queue[position++];
        for (size_t parent = 0; parent < g_count; parent++) {
            const rg_node *node = &g_nodes[parent];
            const rg_producer *producer = node->producers;
            int names = 0;
            for (unsigned phase = 0; phase < 2 && !names; phase++)
                for (size_t j = 0; j < node->phases[phase].count && !names; j++)
                    names = node->phases[phase].children[j] == target;
            for (; producer && !names; producer = producer->next)
                for (size_t j = 0; j < producer->count && !names; j++) names = producer->children[j] == target;
            if (!names || visited[parent]) continue;
            visited[parent] = 1;
            if (!rg_complete(node) || node->phases[1].active) {
                if (covered && first && capacity) snprintf(first, capacity, "%s:%s", node->type, node->name);
                covered = 0;
            }
            if (barrier && barrier(context, node->type, node->name)) continue;
            queue[queued++] = parent;
        }
    }
    if (queued && !(out->items = (sh_resource_graph_identity *)calloc(queued, sizeof(*out->items)))) goto done;
    for (size_t i = 0; i < queued; i++) {
        const rg_node *node = &g_nodes[queue[i]];
        sh_resource_graph_identity *item = &out->items[out->count++];
        item->type = rg_copy(node->type); item->name = rg_copy(node->name);
        if (!item->type || !item->name) goto done;
    }
    if (out->count > 1) qsort(out->items, out->count, sizeof(*out->items), rg_impact_compare);
    result = covered ? SH_RESOURCE_GRAPH_COMPLETE : SH_RESOURCE_GRAPH_INCOMPLETE;
done:
    free(queue); free(visited);
    ReleaseSRWLockShared(&g_lock);
    if (result == SH_RESOURCE_GRAPH_ERROR) sh_resource_graph_impact_free(out);
    return result;
}
void sh_resource_graph_counts(size_t *nodes, size_t *complete, size_t *edges)
{
    size_t i, parsed = 0, links = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_count; i++) {
        const rg_producer *producer;
        parsed += rg_complete(&g_nodes[i]) != 0;
        links += g_nodes[i].phases[0].count + g_nodes[i].phases[1].count;
        for (producer = g_nodes[i].producers; producer; producer = producer->next) links += producer->count;
    }
    if (nodes) *nodes = g_count;
    if (complete) *complete = parsed;
    if (edges) *edges = links;
    ReleaseSRWLockShared(&g_lock);
}
#ifdef SH_RESOURCE_GRAPH_TESTING
void sh_resource_graph_test_reset(void)
{
    size_t i;
    AcquireSRWLockExclusive(&g_lock);
    while (g_contexts) {
        rg_context *c = g_contexts; g_contexts = c->next;
        while (c->top) { rg_scope *scope = c->top; c->top = scope->parent;
            free(scope->children); free(scope); }
        free(c);
    }
    for (i = 0; i < g_count; i++) {
        free(g_nodes[i].type); free(g_nodes[i].name);
        free(g_nodes[i].phases[0].children); free(g_nodes[i].phases[1].children);
        rg_producer_free(g_nodes[i].producers);
    }
    free(g_nodes); free(g_hash); g_nodes = NULL; g_hash = NULL;
    g_count = g_capacity = g_hash_capacity = 0; g_failed = 0;
    InterlockedExchange(&g_size_history, 0);
    ReleaseSRWLockExclusive(&g_lock);
}
#endif
