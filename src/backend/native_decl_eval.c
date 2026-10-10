/* Native entity-state evaluation through the engine's temporary-entity
 * route (R-191). Both builds share this code; the private-definition and
 * written-text routines are signature-verified in each image, the game
 * access and context routines come from the verified type-cache build.
 *
 * Definition (idDeclEntityDef, 0x168 bytes): constructor VK 0x17acbf0 /
 * GL 0x179f6d0 installs the vtable whose slot 0 is the scalar deleting
 * destructor (flag 0: members and base only). Source text (DeclSetText) is
 * copied; DeclParseSource (VK 0x17ab5e0 / GL 0x179e0c0) runs Parse, which
 * prepares the state text at +0x130 from the inheritance chain (VK
 * 0x17ae9d0 / GL 0x17a14b0), and returns nonzero on failure. Only the
 * reparse wrapper (VK 0x17ab550) then runs MakeDefault, which takes a global
 * file entry; calling the parser directly leaves a refused definition
 * private, to be destroyed.
 * Written text: context +0x1018 owns the writer; the getter (VK 0x1a25930 /
 * GL 0x1a17cf0) returns its NUL-terminated data, freed by reset or dtor. */
#include <windows.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_decl_eval.h"
#include "snap_type_cache.h"
#include "process_heap_scope.h"
#include "engine_globals.h"

void backend_log(const char *message);

#define NDE_GAME_GET 0x228
#define NDE_CREATE 0x448
#define NDE_WRITE 0x378
#define NDE_DESTROY 0x470
#define NDE_DECL_SIZE 0x168
#define NDE_DECL_ALLOC 0x200
#define NDE_DECL_SOURCE_FILE 0x50   /* set only by MakeDefault after a failed parse */
#define NDE_DECL_PREPARED_LENGTH 0x138
#define NDE_DECL_PREPARED_TEXT 0x140
#define NDE_CONTEXT 0x1240
#define NDE_ARGS 0x40
#define NDE_TEXT_CAP (16u * 1024u * 1024u)

typedef void *(*nde_game_fn)(void *access);
typedef void *(*nde_create_fn)(void *game, void *decl, void *args);
typedef void (*nde_write_fn)(void *game, void *entity, void *context, unsigned char full);
typedef void (*nde_destroy_fn)(void *game, void *entity);
typedef void (*nde_decl_dtor_fn)(void *decl, unsigned int flags);

typedef struct nde_native {
    void **game_access;
    void (*context_ctor)(void *context, int zero);
    void (*context_args)(void *args, int a, int b);
    void (*context_reset)(void *context);
    void (*context_dtor)(void *context);
    const char *(*context_text)(void *context);
    void (*decl_ctor)(void *decl);
    void (*decl_set_text)(void *decl, const char *text, int length);
    unsigned int (*decl_parse)(void *decl, unsigned char read_source, void *error_text);
    void (*idstr_ctor)(void *text);
    void (*idstr_dtor)(void *text);
    const volatile uint32_t *main_thread;
} nde_native;

static nde_native g;
static sh_process_heap_api g_heap;
static volatile LONG g_ready, g_disabled;
static int g_busy;

static void nde_detail(char *detail, size_t capacity, const char *text)
{
    if (detail && capacity) _snprintf_s(detail, capacity, _TRUNCATE, "%s", text);
}

int sh_native_decl_eval_available(void)
{
    return InterlockedCompareExchange(&g_ready, 0, 0) && !InterlockedCompareExchange(&g_disabled, 0, 0);
}

static int nde_on_main_thread(void)
{
    return g.main_thread && *g.main_thread && *g.main_thread == GetCurrentThreadId();
}

static void nde_disable(const char *why)
{
    char line[256];
    if (InterlockedExchange(&g_disabled, 1)) return;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "NATIVE-EVAL: disabled after %s; conversions keep packages", why);
    backend_log(line);
}

/* The faulting steps of one evaluation. Returns 1 with *state set, 0 not
 * evaluable, -1 fault. Engine objects are released on every path the engine
 * returns through; a raised exception leaves the private memory to the caller. */
static int nde_run(void *decl, void *context, void *args, const char *definition, char **state,
    const char **reason, int *decl_built)
{
    void *access = *g.game_access, *game, *entity;
    void **table;
    const char *text;
    size_t length;
    unsigned int parsed;
    uint8_t error_text[0x40];
    *state = NULL;
    if (!access || !(game = ((nde_game_fn)(*(void ***)access)[NDE_GAME_GET / 8])(access))) {
        *reason = "no game"; return 0;
    }
    table = *(void ***)game;
    g.decl_ctor(decl); *decl_built = 1;
    g.decl_set_text(decl, definition, (int)strlen(definition));
    g.idstr_ctor(error_text);
    parsed = g.decl_parse(decl, 0, error_text);
    g.idstr_dtor(error_text);
    if (parsed) { *reason = "the engine parser refused the definition"; return 0; }
    if (*(const void * const *)((const uint8_t *)decl + NDE_DECL_SOURCE_FILE)) {
        *reason = "the parsed definition acquired a source file"; return -1;
    }
    if (*(const int32_t *)((const uint8_t *)decl + NDE_DECL_PREPARED_LENGTH) <= 0 ||
        !*(const void * const *)((const uint8_t *)decl + NDE_DECL_PREPARED_TEXT)) {
        *reason = "the definition has no prepared state"; return 0;
    }
    g.context_args(args, 0, 0);
    entity = ((nde_create_fn)table[NDE_CREATE / 8])(game, decl, args);
    if (!entity) { *reason = "the class is not instantiable"; return 0; }
    g.context_ctor(context, 0);
    memcpy(context, args, 20);
    g.context_reset(context);
    ((nde_write_fn)table[NDE_WRITE / 8])(game, entity, context, 1);
    ((nde_destroy_fn)table[NDE_DESTROY / 8])(game, entity);
    text = g.context_text(context);
    length = text ? strlen(text) : 0;
    if (length && length < NDE_TEXT_CAP && (*state = malloc(length + 1)))
        memcpy(*state, text, length + 1);
    g.context_dtor(context);
    if (!*state) { *reason = length ? "written state too large" : "empty written state"; return 0; }
    return 1;
}

int sh_native_decl_eval_state(const char *definition, char **state, char *detail, size_t capacity)
{
    void *decl = NULL, *context = NULL, *args = NULL;
    const char *reason = "unavailable";
    int result = 0, decl_built = 0;
    unsigned long code = 0;
    sh_process_heap_scope scope;
    if (state) *state = NULL;
    if (!definition || !state) return 0;
    if (!sh_native_decl_eval_available()) { nde_detail(detail, capacity, "unavailable"); return 0; }
    if (!nde_on_main_thread()) { nde_detail(detail, capacity, "not on the engine's main thread"); return 0; }
    if (g_busy) { nde_detail(detail, capacity, "re-entered"); return 0; }
    decl = _aligned_malloc(NDE_DECL_ALLOC, 16);
    context = _aligned_malloc(NDE_CONTEXT, 16);
    args = _aligned_malloc(NDE_ARGS, 16);
    if (!decl || !context || !args) { reason = "allocation failed"; goto done; }
    memset(decl, 0, NDE_DECL_ALLOC); memset(context, 0, NDE_CONTEXT); memset(args, 0, NDE_ARGS);
    if (!sh_process_heap_enter(&g_heap, &scope)) { reason = "process heap scope unavailable"; goto done; }
    g_busy = 1;
    __try {
        result = nde_run(decl, context, args, definition, state, &reason, &decl_built);
        if (decl_built) { decl_built = 0; ((nde_decl_dtor_fn)(*(void ***)decl)[0])(decl, 0); }
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        free(*state); *state = NULL; result = -1; reason = "engine exception";
        decl = context = NULL; /* engine state may still reference them */
    }
    g_busy = 0;
    if (!sh_process_heap_leave(&scope)) { result = -1; reason = "process heap scope was not restored"; }
done:
    if (result < 0) {
        char why[160];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "%s (code 0x%08lx)", reason, code);
        nde_disable(why);
        free(*state); *state = NULL;
    }
    if (result != 1) nde_detail(detail, capacity, reason);
    _aligned_free(decl); _aligned_free(context); _aligned_free(args);
    return result;
}

/* Conversions do not run the engine route yet. Its envelope is open: the
 * parser's decl-manager +0xb0 bracket targets a runtime-constructed object
 * (VK 0x6218e70, no static constructor; its destructor 0x1802fb0 restores
 * base vtable 0x2710a28), the temporary entity's destruction is not shown to
 * withdraw the registrations SetEntityEditState publishes or the names
 * entity-reference readers intern, and a reported read error raises Error6.
 * Written-text equality is not yet an accepted predicate either: custom
 * writers and resource binding identity are unqualified. Paired controls use
 * sh_native_decl_eval_state directly. */
#define NDE_UNQUALIFIED "the engine route's parse, registration and error envelope is not yet qualified"

sh_native_eval_status sh_native_decl_eval_compare(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, char *detail, size_t capacity)
{
    if (!target || !candidate || (layer_count && !layers)) { nde_detail(detail, capacity, "invalid request"); return SH_NATIVE_EVAL_FAILED; }
    if (layer_count) {
        nde_detail(detail, capacity, "a private inheritance chain needs the entity initializer's parent contract");
        return SH_NATIVE_EVAL_UNAVAILABLE;
    }
    nde_detail(detail, capacity, NDE_UNQUALIFIED);
    return SH_NATIVE_EVAL_UNAVAILABLE;
}

static uint8_t *nde_clean(const sig_result *r, size_t n, const char *name, const uint8_t *base)
{
    for (size_t i = 0; i < n; i++)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK && r[i].addr &&
            r[i].addr == (uintptr_t)base + r[i].rva) return (uint8_t *)r[i].addr;
    return NULL;
}

int sh_native_decl_eval_install(const sig_result *r, size_t n, const uint8_t *base)
{
    sh_snap_entity_route route;
    uint8_t *ctor = nde_clean(r, n, "DeclEntityDefCtor", base), *text = nde_clean(r, n, "DeclSetText", base);
    uint8_t *parse = nde_clean(r, n, "DeclParseSource", base), *written = nde_clean(r, n, "ContextWrittenText", base);
    uint8_t *idstr_ctor = nde_clean(r, n, "DefaultIdStrCtor", base), *idstr_dtor = nde_clean(r, n, "IdStrDtor", base);
    uintptr_t main_thread = base ? glb_resolve(base, "main_thread_id", NULL) : 0;
    if (InterlockedCompareExchange(&g_ready, 0, 0)) return 1;
    if (!ctor || !text || !parse || !written || !idstr_ctor || !idstr_dtor || !main_thread || !sh_snap_type_cache_entity_route(&route) ||
        !sh_process_heap_bind(&g_heap, r, n, base)) {
        backend_log("NATIVE-EVAL: native entity-state routines not verified; conversions keep packages");
        return 0;
    }
    g.game_access = route.game_access;
    g.context_ctor = route.context_ctor; g.context_args = route.context_args;
    g.context_reset = route.context_reset; g.context_dtor = route.context_dtor;
    g.context_text = (const char *(*)(void *))written;
    g.decl_ctor = (void (*)(void *))ctor;
    g.decl_set_text = (void (*)(void *, const char *, int))text;
    g.decl_parse = (unsigned int (*)(void *, unsigned char, void *))parse;
    g.idstr_ctor = (void (*)(void *))idstr_ctor; g.idstr_dtor = (void (*)(void *))idstr_dtor;
    g.main_thread = (const volatile uint32_t *)main_thread;
    InterlockedExchange(&g_ready, 1);
    backend_log("NATIVE-EVAL: native entity-state evaluation available");
    return 1;
}

#ifdef SH_NATIVE_DECL_EVAL_TESTING
static void *nde_test_heap_get(void) { static int heap; return &heap; }
static void nde_test_heap_push(void *self, int heap) { (void)self; (void)heap; }
static void nde_test_heap_pop(void *self) { (void)self; }
void sh_native_decl_eval_test_bind(const sh_native_decl_eval_native *n)
{
    memcpy(&g, n, sizeof(g));
    g_heap.get = nde_test_heap_get; g_heap.push = nde_test_heap_push; g_heap.pop = nde_test_heap_pop;
    InterlockedExchange(&g_disabled, 0);
    InterlockedExchange(&g_ready, n && n->game_access ? 1 : 0);
}
#endif
