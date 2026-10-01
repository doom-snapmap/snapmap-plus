/* fsm_web.c -- per-configuration AI state-machine webs (see fsm_web.h). */
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fsm_web.h"
#include "backend_log.h"
#include "patch.h"

/* Native layout the builder reads on both renderers (instruction-identical
 * builders): the resource list keeps its entry array, count and capacity at
 * +0x20/+0x28/+0x2C; a declaration keeps its name at +8 and its graph array and
 * count at +0x68/+0x70; a graph keeps the layer mask the builder tests at +0x38. */
#define FW_LIST_ARRAY 0x20
#define FW_LIST_COUNT 0x28
#define FW_LIST_CAPACITY 0x2c
#define FW_DECL_NAME 0x08
#define FW_DECL_GRAPHS 0x68
#define FW_DECL_GRAPH_COUNT 0x70
#define FW_GRAPH_MASK 0x38
#define FW_DECL_CONFIGS 0xb8          /* layer configurations: 16-byte {interned name, mask} */
#define FW_DECL_CONFIG_COUNT 0xc0
#define FW_BEHAVIOR_CONFIG 0x558      /* the builder receives behavior+0x558 (both renderers' caller) */
#define FW_BUILDER_BYTES 0xBD0u
#define FW_REPLAY_CALL 0x74           /* the builder calls its cache replay here (both renderers) */
#define FW_REPLAY_BYTES 0x100u        /* the replay's decl-0 lookup lies in its first bytes */

typedef void *(*fw_get_fn)(void *list, int index);
typedef uintptr_t (*fw_builder_fn)(void *manager, void *config);

typedef struct fw_names { char **items; size_t count; } fw_names;
typedef struct fw_bind { char *config; fw_names behaviors, from, to, extra; } fw_bind;

/* Published bindings, shared by the states resolved from them. */
typedef struct fw_spec {
    volatile LONG references;
    fw_bind *bindings;
    size_t count;
} fw_spec;

/* One immutable resolution of a spec against one generation: the list's array
 * and count and the number of issued names (the issued set only grows). Each
 * build pins the state it starts with until it returns. */
typedef struct fw_state {
    volatile LONG references;
    fw_spec *spec;
    void *list, *array;
    int count;
    size_t issued;
    void **native;             /* view for every build nothing binds */
    void ***bound;             /* views parallel to spec->bindings */
    void **configs;            /* interned private configuration per binding */
} fw_state;

/* Invocation-local, on the wrapper's stack; t_current links the chain, so
 * nesting has no fixed depth and a frame never outlives its build. */
typedef struct fw_frame {
    struct fw_frame *previous;
    fw_state *state;
    void *const *view;
    int refused;
} fw_frame;

static SRWLOCK g_lock = SRWLOCK_INIT;
static fw_names g_issued;          /* every issued product declaration; only grows */
static fw_spec *g_spec;            /* current bindings; set from the first issue on */
static fw_state *g_state;          /* resolution of g_spec builds start from */
static volatile LONG g_live;       /* allocated specs and states */
static fw_get_fn g_get;
static fw_builder_fn g_builder;
static void *g_list;
static int g_installed;
static void *g_relay;
static sh_patch_handle g_patches[4];
static volatile LONG g_warned_refused;
static __declspec(thread) fw_frame *t_current;

static void fw_logf(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, format, args);
    va_end(args);
    backend_log(line);
}

static void fw_names_free(fw_names *names)
{
    for (size_t i = 0; i < names->count; i++) free(names->items[i]);
    free(names->items); names->items = NULL; names->count = 0;
}

static int fw_names_has(const fw_names *names, const char *name)
{
    for (size_t i = 0; name && i < names->count; i++) if (!strcmp(names->items[i], name)) return 1;
    return 0;
}

static int fw_names_add(fw_names *names, const char *name)
{
    char **grown;
    if (!name || !*name) return 0;
    if (fw_names_has(names, name)) return 1;
    grown = realloc(names->items, (names->count + 1) * sizeof(*grown));
    if (!grown) return 0;
    names->items = grown;
    if (!(names->items[names->count] = _strdup(name))) return 0;
    names->count++;
    return 1;
}

static int fw_names_copy(fw_names *out, const char *const *names, size_t count)
{
    if (count && !names) return 0;
    if (count && !(out->items = calloc(count, sizeof(*out->items)))) return 0;
    for (size_t i = 0; i < count; i++) {
        if (!names[i] || !*names[i] || !(out->items[i] = _strdup(names[i]))) return 0;
        out->count = i + 1;
    }
    return 1;
}

static void fw_spec_release(fw_spec *spec)
{
    if (!spec || InterlockedDecrement(&spec->references)) return;
    for (size_t i = 0; i < spec->count; i++) {
        free(spec->bindings[i].config); fw_names_free(&spec->bindings[i].behaviors);
        fw_names_free(&spec->bindings[i].from); fw_names_free(&spec->bindings[i].to);
        fw_names_free(&spec->bindings[i].extra);
    }
    free(spec->bindings); free(spec);
    InterlockedDecrement(&g_live);
}

static fw_spec *fw_spec_empty(void)
{
    fw_spec *spec = calloc(1, sizeof(*spec));
    if (spec) { spec->references = 1; InterlockedIncrement(&g_live); }
    return spec;
}

static void fw_state_release(fw_state *state)
{
    if (!state || InterlockedDecrement(&state->references)) return;
    if (state->bound) for (size_t i = 0; i < state->spec->count; i++) free(state->bound[i]);
    free(state->bound); free(state->native); free(state->configs);
    fw_spec_release(state->spec); free(state);
    InterlockedDecrement(&g_live);
}

/* Issued names and their count, copied under the lock for one resolution. */
static int fw_issued_copy(fw_names *out, size_t *count)
{
    int ok = 1;
    AcquireSRWLockShared(&g_lock);
    *count = g_issued.count;
    for (size_t i = 0; ok && i < g_issued.count; i++) ok = fw_names_add(out, g_issued.items[i]);
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

static int fw_issue_locked(const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; i++) if (!names || !fw_names_add(&g_issued, names[i])) return 0;
    return 1;
}

int sh_fsm_web_issue(const char *const *names, size_t count)
{
    int ok;
    fw_spec *empty = NULL;
    if (!count) return 1;
    AcquireSRWLockShared(&g_lock);
    ok = g_spec != NULL;
    ReleaseSRWLockShared(&g_lock);
    if (!ok && !(empty = fw_spec_empty())) return 0;
    AcquireSRWLockExclusive(&g_lock);
    ok = fw_issue_locked(names, count);
    if (ok && !g_spec) { g_spec = empty; empty = NULL; }
    ReleaseSRWLockExclusive(&g_lock);
    fw_spec_release(empty);
    return ok;
}

static const char *fw_name(void *decl)
{
    const char *name = NULL;
    if (!decl) return NULL;
    __try { name = *(const char *const *)((const unsigned char *)decl + FW_DECL_NAME); }
    __except (EXCEPTION_EXECUTE_HANDLER) { name = NULL; }
    return name;
}

/* The builder keeps a graph only when its mask meets the configuration's (test
 * [graph+0x38] then skip: Vulkan 0x108C256, OpenGL 0x107DDC6); root, state and
 * transition handling all sit behind that test. A declaration with at least one
 * graph and every graph mask zero therefore adds nothing to any web, whatever
 * the configuration names, and its graphs pass the earlier typeinfo check only
 * if they are genuine graphs. Anything else is refused as the placeholder. */
static int fw_placeholder_ok(void *decl)
{
    int ok = 0;
    if (!decl) return 0;
    __try {
        const unsigned char *d = (const unsigned char *)decl;
        void *const *graphs = *(void *const *const *)(d + FW_DECL_GRAPHS);
        int count = *(const int *)(d + FW_DECL_GRAPH_COUNT);
        ok = count > 0 && graphs;
        for (int i = 0; ok && i < count; i++)
            ok = graphs[i] && *(const uint32_t *)((const unsigned char *)graphs[i] + FW_GRAPH_MASK) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

/* The list's own count bounds every view; it must lie within the list's
 * capacity. No other limit applies. */
static int fw_generation(void *list, void **array, int *count)
{
    int ok = 0, capacity = 0;
    *array = NULL; *count = 0;
    if (!list) return 0;
    __try {
        const unsigned char *l = (const unsigned char *)list;
        *array = *(void *const *)(l + FW_LIST_ARRAY);
        *count = *(const int *)(l + FW_LIST_COUNT);
        capacity = *(const int *)(l + FW_LIST_CAPACITY);
        ok = *count >= 0 && *count <= capacity && (*array || !*count);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

/* The interned value of one configuration in a declaration's parsed table
 * (the table the builder's lookup scans, Vulkan 0x17BD1E0 / OpenGL 0x17AFCF0). */
static void *fw_config_atom(void *decl, const char *config)
{
    void *atom = NULL;
    if (!decl || !config) return NULL;
    __try {
        const unsigned char *entries = *(const unsigned char *const *)((const unsigned char *)decl + FW_DECL_CONFIGS);
        int count = *(const int *)((const unsigned char *)decl + FW_DECL_CONFIG_COUNT);
        for (int i = 0; entries && i < count && !atom; i++) {
            const char *name = *(const char *const *)(entries + (size_t)i * 16);
            if (name && !strcmp(name, config)) atom = (void *)name;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { atom = NULL; }
    return atom;
}

static int fw_index(const char *const *names, int count, const char *name)
{
    for (int i = 0; name && i < count; i++) if (names[i] && !strcmp(names[i], name)) return i;
    return -1;
}

/* Resolve every view of spec for the current generation. Only a complete,
 * validated resolution is returned; anything else reports why and changes
 * nothing. Runs at publication, or once per generation change. */
static fw_state *fw_resolve(fw_spec *spec, void *list, char *why, size_t capacity)
{
    fw_state *state = calloc(1, sizeof(*state));
    fw_names issued = {0};
    const char **names = NULL;
    void **decls = NULL, *placeholder = NULL;
    int count = 0, hidden = 0;
    size_t slots;
    if (!state) { snprintf(why, capacity, "allocation failed"); return NULL; }
    InterlockedIncrement(&g_live);
    InterlockedIncrement(&spec->references);
    state->references = 1; state->spec = spec; state->list = list;
    /* The issued set is read before the list: a declaration is issued before it
     * is registered, so every issued entry this list shows is excluded here. */
    if (!fw_issued_copy(&issued, &state->issued)) { snprintf(why, capacity, "allocation failed"); goto failed; }
    if (!fw_generation(list, &state->array, &state->count)) {
        snprintf(why, capacity, "the state-machine list count is outside its capacity"); goto failed;
    }
    count = state->count; slots = count ? (size_t)count : 1;
    decls = calloc(slots, sizeof(*decls));
    names = calloc(slots, sizeof(*names));
    state->native = calloc(slots, sizeof(*state->native));
    state->bound = spec->count ? calloc(spec->count, sizeof(*state->bound)) : NULL;
    state->configs = spec->count ? calloc(spec->count, sizeof(*state->configs)) : NULL;
    if (!decls || !names || !state->native || (spec->count && (!state->bound || !state->configs))) {
        snprintf(why, capacity, "allocation failed"); goto failed;
    }
    for (int i = 0; i < count; i++) {
        decls[i] = g_get(list, i);
        names[i] = fw_name(decls[i]);
        hidden |= names[i] && fw_names_has(&issued, names[i]);
    }
    if (hidden || spec->count) {
        int at = fw_index(names, count, SH_FSM_WEB_PLACEHOLDER);
        placeholder = at >= 0 ? decls[at] : NULL;
        if (!fw_placeholder_ok(placeholder)) {
            snprintf(why, capacity, "placeholder %s is not a registered non-selecting declaration",
                     SH_FSM_WEB_PLACEHOLDER);
            goto failed;
        }
    }
    for (int i = 0; i < count; i++)
        state->native[i] = names[i] && fw_names_has(&issued, names[i]) ? placeholder : decls[i];
    for (size_t b = 0; b < spec->count; b++) {
        const fw_bind *bind = &spec->bindings[b];
        void **view = calloc(slots, sizeof(*view));
        if (!view) { snprintf(why, capacity, "allocation failed"); goto failed; }
        state->bound[b] = view;
        for (size_t k = 0; k < bind->from.count; k++)
            if (fw_index(names, count, bind->from.items[k]) < 0 || fw_index(names, count, bind->to.items[k]) < 0) {
                snprintf(why, capacity, "%s: %s or its replacement %s is not registered",
                         bind->config, bind->from.items[k], bind->to.items[k]);
                goto failed;
            }
        {
            size_t k;
            for (k = 0; k < bind->from.count && strcmp(bind->from.items[k], SH_FSM_WEB_FIRST); k++) {}
            state->configs[b] = k < bind->from.count ?
                fw_config_atom(decls[fw_index(names, count, bind->to.items[k])], bind->config) : NULL;
            if (!state->configs[b]) {
                snprintf(why, capacity, "%s: its replacement of %s does not carry that configuration",
                         bind->config, SH_FSM_WEB_FIRST);
                goto failed;
            }
        }
        for (size_t k = 0; k < bind->extra.count; k++)
            if (fw_index(names, count, bind->extra.items[k]) < 0) {
                snprintf(why, capacity, "%s: addition %s is not registered", bind->config, bind->extra.items[k]);
                goto failed;
            }
        for (int i = 0; i < count; i++) {
            size_t k;
            for (k = 0; k < bind->from.count && (!names[i] || strcmp(bind->from.items[k], names[i])); k++) {}
            if (k < bind->from.count) view[i] = decls[fw_index(names, count, bind->to.items[k])];
            else if (names[i] && fw_names_has(&bind->extra, names[i])) view[i] = decls[i];
            else view[i] = state->native[i];
        }
    }
    free(decls); free((void *)names); fw_names_free(&issued);
    return state;
failed:
    free(decls); free((void *)names); fw_names_free(&issued);
    fw_state_release(state);
    return NULL;
}

static fw_spec *fw_spec_build(const sh_fsm_web_table *table)
{
    fw_spec *spec = fw_spec_empty();
    int ok = spec != NULL;
    if (!spec) return NULL;
    if (table && table->count) {
        if (!table->bindings || !(spec->bindings = calloc(table->count, sizeof(*spec->bindings)))) ok = 0;
        else spec->count = table->count;
    }
    for (size_t i = 0; ok && i < spec->count; i++) {
        const sh_fsm_web_binding *in = &table->bindings[i];
        fw_bind *b = &spec->bindings[i];
        if (!in->config || !*in->config || !(b->config = _strdup(in->config)) || !in->behavior_count ||
            !fw_names_copy(&b->behaviors, in->behaviors, in->behavior_count) ||
            !fw_names_copy(&b->from, in->from, in->count) || !fw_names_copy(&b->to, in->to, in->count) ||
            !fw_names_copy(&b->extra, in->extra, in->extra_count)) ok = 0;
        for (size_t j = 0; ok && j < i; j++) {
            if (!strcmp(spec->bindings[j].config, b->config)) ok = 0;   /* one web per configuration */
            for (size_t k = 0; ok && k < b->behaviors.count; k++)
                if (fw_names_has(&spec->bindings[j].behaviors, b->behaviors.items[k])) ok = 0;   /* one web per behavior */
        }
    }
    if (!ok) { fw_spec_release(spec); return NULL; }
    return spec;
}

int sh_fsm_web_publish(const sh_fsm_web_table *table)
{
    fw_spec *spec, *old_spec;
    fw_state *state = NULL, *old_state;
    char why[256] = "";
    int issued = 1;
    /* Issue first: a declaration named here stays excluded even if this
     * publication is refused below. */
    if (table) {
        issued = sh_fsm_web_issue(table->owned, table->owned_count);
        for (size_t i = 0; issued && table->bindings && i < table->count; i++)
            issued = sh_fsm_web_issue(table->bindings[i].to, table->bindings[i].count) &&
                     sh_fsm_web_issue(table->bindings[i].extra, table->bindings[i].extra_count);
    }
    spec = issued ? fw_spec_build(table) : NULL;
    if (!spec) {
        backend_log("fsm-web publication refused: invalid or unallocatable table; previous bindings stay");
        return 0;
    }
    if (g_get && g_list) {
        state = fw_resolve(spec, g_list, why, sizeof(why));
        if (!state) {
            fw_logf("fsm-web publication refused: %s; previous bindings stay", why);
            fw_spec_release(spec);
            return 0;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    old_state = g_state; old_spec = g_spec;
    g_state = state; g_spec = spec;
    ReleaseSRWLockExclusive(&g_lock);
    fw_state_release(old_state);
    fw_spec_release(old_spec);
    return 1;
}

/* A build pins the current state. When the generation changed since that
 * state was resolved (or none was resolved yet), the current bindings are
 * resolved for the new generation first and become current. A generation that
 * cannot be resolved coherently refuses the build: the engine then reports the
 * AI as uninitialized instead of running a mixed web. Nothing issued: native. */
static fw_state *fw_pin(int *refused)
{
    fw_state *state, *fresh;
    fw_spec *spec;
    void *array; int count, replaced = 0;
    size_t issued;
    char why[256] = "";
    *refused = 0;
    AcquireSRWLockShared(&g_lock);
    state = g_state; spec = g_spec; issued = g_issued.count;
    if (state) InterlockedIncrement(&state->references);
    if (spec) InterlockedIncrement(&spec->references);
    ReleaseSRWLockShared(&g_lock);
    if (!spec) { fw_state_release(state); return NULL; }
    if (state && state->spec == spec && state->issued == issued &&
        fw_generation(state->list, &array, &count) && array == state->array && count == state->count) {
        fw_spec_release(spec);
        return state;
    }
    fresh = g_list ? fw_resolve(spec, g_list, why, sizeof(why)) : NULL;
    fw_spec_release(spec);
    if (!fresh) {
        if (!InterlockedExchange(&g_warned_refused, 1))
            fw_logf("fsm-web: state-machine webs could not be resolved for the current list (%s); "
                    "AI state-machine builds are refused until it resolves", g_list ? why : "no list");
        fw_state_release(state);
        *refused = 1;
        return NULL;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_state == state && g_spec == fresh->spec) {
        g_state = fresh; InterlockedIncrement(&fresh->references); replaced = 1;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (replaced) fw_state_release(state);   /* the reference g_state held */
    fw_state_release(state);                  /* this build's pin */
    InterlockedExchange(&g_warned_refused, 0);
    return fresh;
}

static void *fw_decl_at(void *list, int index)
{
    fw_frame *frame = t_current;
    if (!frame) return g_get(list, index);
    if (frame->refused) return NULL;
    if (!frame->state) return g_get(list, index);
    if (list != frame->state->list || index < 0 || index >= frame->state->count) return NULL;
    return frame->view[index];
}

static uintptr_t fw_build(void *manager, void *config)
{
    fw_frame frame = {t_current, NULL, NULL, 0};
    uintptr_t result = 0;
    void *private_config = NULL, *argument = config;
    frame.state = fw_pin(&frame.refused);
    /* The native builder records the configuration in its cache before it
     * reads the list (ai_fsm_allowCache), so a build that could only fail is
     * not started: a failed build would leave that configuration cached empty
     * for the rest of the game session. The caller reports the AI uninitialized. */
    if (frame.refused) return 0;
    if (frame.state) {
        const fw_spec *spec = frame.state->spec;
        const char *behavior = config ? fw_name((unsigned char *)config - FW_BEHAVIOR_CONFIG) : NULL;
        frame.view = frame.state->native;
        for (size_t i = 0; behavior && i < spec->count; i++)
            if (fw_names_has(&spec->bindings[i].behaviors, behavior)) {
                frame.view = frame.state->bound[i];
                private_config = frame.state->configs[i];
                argument = &private_config;   /* the factory keeps the interned value, not this pointer */
                break;
            }
    }
    t_current = &frame;
    __try { result = g_builder(manager, argument); }
    __finally {
        t_current = frame.previous;
        fw_state_release(frame.state);
    }
    return result;
}

int sh_fsm_web_installed(void) { return g_installed; }

/* Two leaf tail jumps near the call sites: mov rax,imm64; jmp rax. */
static void *fw_relay(uintptr_t anchor)
{
    SYSTEM_INFO info;
    uintptr_t step, center;
    void *targets[2] = {(void *)fw_build, (void *)fw_decl_at};
    GetSystemInfo(&info);
    step = info.dwAllocationGranularity; center = anchor & ~(step - 1);
    for (uintptr_t delta = step; delta < 0x7fff0000u; delta += step) {
        uintptr_t candidates[2] = {center >= delta ? center - delta : 0, center + delta};
        for (int i = 0; i < 2; i++) {
            MEMORY_BASIC_INFORMATION mbi;
            unsigned char *relay;
            DWORD old;
            if (candidates[i] < (uintptr_t)info.lpMinimumApplicationAddress ||
                candidates[i] > (uintptr_t)info.lpMaximumApplicationAddress ||
                !VirtualQuery((void *)candidates[i], &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
            relay = (unsigned char *)VirtualAlloc((void *)candidates[i], 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!relay) continue;
            for (int k = 0; k < 2; k++) {
                relay[k * 16] = 0x48; relay[k * 16 + 1] = 0xb8;
                memcpy(relay + k * 16 + 2, &targets[k], sizeof(targets[k]));
                relay[k * 16 + 10] = 0xff; relay[k * 16 + 11] = 0xe0;
            }
            if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old)) { VirtualFree(relay, 0, MEM_RELEASE); return NULL; }
            FlushInstructionCache(GetCurrentProcess(), relay, 32);
            return relay;
        }
    }
    return NULL;
}

static const sig_result *fw_signature(const sig_result *results, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (results[i].name && !strcmp(results[i].name, name) && results[i].status == SIG_OK && results[i].addr)
            return &results[i];
    return NULL;
}

static uintptr_t fw_call_target(uintptr_t call)
{
    int32_t displacement;
    if (*(const unsigned char *)call != 0xe8) return 0;
    memcpy(&displacement, (const void *)(call + 1), sizeof(displacement));
    return call + 5 + (intptr_t)displacement;
}

static uintptr_t fw_lea_target(uintptr_t lea)
{
    const unsigned char *p = (const unsigned char *)lea;
    int32_t displacement;
    if (p[0] != 0x48 || p[1] != 0x8d || p[2] != 0x0d) return 0;
    memcpy(&displacement, p + 3, sizeof(displacement));
    return lea + 7 + (intptr_t)displacement;
}

static int fw_redirect(uintptr_t call, const char *name, void *stub, sh_patch_handle *handle)
{
    unsigned char expected[5], replacement[5] = {0xe8};
    intptr_t distance = (intptr_t)stub - (intptr_t)(call + 5);
    int32_t displacement;
    sig_result site = {name, SIG_OK, call, 0};
    if (distance < INT32_MIN || distance > INT32_MAX) return 0;
    memcpy(expected, (const void *)call, sizeof(expected));
    displacement = (int32_t)distance;
    memcpy(replacement + 1, &displacement, sizeof(displacement));
    /* The displacements are not 4-byte aligned, so this is a verified copy,
     * made at attach before any game or AI build exists. */
    return code_patch_sig(&site, expected, replacement, sizeof(replacement), handle) == B2_PATCH_OK;
}

static int fw_refuse(const char *why)
{
    fw_logf("fsm-web REFUSED: %s; package state-machine variants stay unbound", why);
    return 0;
}
int sh_fsm_web_install(const uint8_t *module, const sig_result *results, size_t count)
{
    const sig_result *builder = fw_signature(results, count, "FsmWebBuilder");
    const sig_result *call = fw_signature(results, count, "FsmWebBuildCall");
    const sig_result *first = fw_signature(results, count, "FsmWebDeclFirstCall");
    const sig_result *at = fw_signature(results, count, "FsmWebDeclAtCall");
    const sig_result *gate = fw_signature(results, count, "FsmWebCacheFlag");
    const sig_result *replay_site = fw_signature(results, count, "FsmWebReplayFirstCall");
    uintptr_t build_call, first_call, at_call, replay_call, replay, get, list, flag;
    int32_t displacement;
    const char *why = NULL;
    (void)module;
    if (g_installed) return 1;
    if (!builder || !call || !first || !at || !gate || !replay_site) {
        why = "a builder, lookup or cache signature did not resolve uniquely"; goto refused;
    }
    build_call = call->addr + 21; first_call = first->addr + 16; at_call = at->addr + 15; replay_call = replay_site->addr + 15;
    get = fw_call_target(first_call);
    list = fw_lea_target(first->addr + 9);
    replay = fw_call_target(builder->addr + FW_REPLAY_CALL);
    if (fw_call_target(build_call) != builder->addr) { why = "the call site does not call the builder"; goto refused; }
    if (!get || fw_call_target(at_call) != get || fw_call_target(replay_call) != get) {
        why = "the builder's list lookups call different functions"; goto refused;
    }
    if (!list || fw_lea_target(at->addr + 8) != list || fw_lea_target(replay_site->addr + 8) != list) {
        why = "the builder's list lookups name different lists"; goto refused;
    }
    if (first_call < builder->addr || first_call >= builder->addr + FW_BUILDER_BYTES ||
        at_call < builder->addr || at_call >= builder->addr + FW_BUILDER_BYTES ||
        gate->addr < builder->addr || gate->addr >= builder->addr + FW_BUILDER_BYTES) {
        why = "a lookup or the cache gate lies outside the builder"; goto refused;
    }
    /* With ai_fsm_allowCache set, the builder replays the web it cached for a
     * configuration (Vulkan 0x108B9F0, OpenGL 0x107D560; keyed by the interned
     * configuration value, reset with the game object and by
     * ai_reloadBehaviorFSMs). The replay reads list entry 0 again for the mask,
     * so that lookup is the view's too; a private configuration keys its own
     * cache entry, and native caching is otherwise left as it is. */
    if (!replay || replay_call < replay || replay_call >= replay + FW_REPLAY_BYTES) {
        why = "the cache replay's lookup is not in the builder's replay"; goto refused;
    }
    if (((const unsigned char *)gate->addr)[4] != 0x83 || ((const unsigned char *)gate->addr)[5] != 0x3d) {
        why = "the cache gate is not the builder's comparison"; goto refused;
    }
    memcpy(&displacement, (const void *)(gate->addr + 6), sizeof(displacement));
    flag = gate->addr + 4 + 7 + (intptr_t)displacement;
    g_get = (fw_get_fn)get; g_builder = (fw_builder_fn)builder->addr; g_list = (void *)list;
    g_relay = fw_relay(builder->addr);
    if (!g_relay) { why = "no relay could be placed near the builder"; goto refused; }
    /* Inner lookups first: without a build frame they pass straight through. */
    if (!fw_redirect(first_call, "FsmWebDeclFirstCall", (unsigned char *)g_relay + 16, &g_patches[0]) ||
        !fw_redirect(at_call, "FsmWebDeclAtCall", (unsigned char *)g_relay + 16, &g_patches[1]) ||
        !fw_redirect(replay_call, "FsmWebReplayFirstCall", (unsigned char *)g_relay + 16, &g_patches[2]) ||
        !fw_redirect(build_call, "FsmWebBuildCall", g_relay, &g_patches[3])) goto rollback;
    g_installed = 1;
    fw_logf("fsm-web installed: state-machine builds resolve per configuration (builder %p, list %p, "
            "cache replay %p; ai_fsm_allowCache %d)", (void *)builder->addr, (void *)list, (void *)replay,
            *(const volatile int32_t *)flag);
    return 1;
rollback:
    for (int i = 3; i >= 0; i--)
        if (g_patches[i].live && code_unpatch(&g_patches[i]) != B2_PATCH_OK) {
            backend_log("fsm-web rollback incomplete; relay retained for the outstanding patch");
            return 0;
        }
    VirtualFree(g_relay, 0, MEM_RELEASE); g_relay = NULL;
    why = "a call site could not be redirected";
refused:
    g_get = NULL; g_builder = NULL; g_list = NULL;
    return fw_refuse(why);
}

#ifdef SH_FSM_WEB_TESTING
void sh_fsm_web_test_bind(sh_fsm_web_test_get get, sh_fsm_web_test_builder builder, void *list)
{ g_get = (fw_get_fn)get; g_builder = (fw_builder_fn)builder; g_list = list; }
uintptr_t sh_fsm_web_test_build(void *manager, void *config) { return fw_build(manager, config); }
void *sh_fsm_web_test_decl_at(void *list, int index) { return fw_decl_at(list, index); }
int sh_fsm_web_test_depth(void) { int n = 0; for (fw_frame *f = t_current; f; f = f->previous) n++; return n; }
long sh_fsm_web_test_live(void) { return g_live; }
void *sh_fsm_web_test_relay(int resolver) { return g_relay ? (unsigned char *)g_relay + (resolver ? 16 : 0) : NULL; }
int sh_fsm_web_test_uninstall(void)
{
    for (int i = 3; i >= 0; i--)
        if (g_patches[i].live && code_unpatch(&g_patches[i]) != B2_PATCH_OK) return 0;
    if (g_relay) VirtualFree(g_relay, 0, MEM_RELEASE);
    g_relay = NULL; g_installed = 0; g_get = NULL; g_builder = NULL; g_list = NULL;
    g_warned_refused = 0;
    return 1;
}
void sh_fsm_web_test_reset(void)
{
    fw_state *state;
    fw_spec *spec;
    AcquireSRWLockExclusive(&g_lock);
    state = g_state; spec = g_spec; g_state = NULL; g_spec = NULL;
    fw_names_free(&g_issued);
    ReleaseSRWLockExclusive(&g_lock);
    fw_state_release(state);
    fw_spec_release(spec);
    g_warned_refused = 0;
}
#endif
