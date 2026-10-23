/* Native entity-state evaluation (R-191, M61). Conversions compare the
 * state text the engine's own parser prepares for two private definitions
 * (no temporary entity); sh_native_decl_eval_state keeps the temporary-entity
 * route for paired controls. Both builds share this code; the
 * private-definition and written-text routines are signature-verified in each
 * image, the game access and context routines come from the verified
 * type-cache build.
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
 * GL 0x1a17cf0) returns its NUL-terminated data, freed by reset or dtor.
 *
 * Private chains (M62): Parse (VK 0x17ad460 / GL 0x179ff40) and the
 * root-first inheritance merge (VK 0x17aec10 / GL 0x17a16f0) resolve a parent
 * only through lookup(entityDef list, name, 0) (VK 0x17b36f0 / GL 0x17a61b0).
 * Those two calls are redirected through a relay that answers the names of
 * the definitions being prepared, on the evaluating main thread only, and
 * otherwise calls the lookup unchanged. The engine's own Parse, per-layer
 * merge (with its per-stream num/item post-pass VK 0x1a65fa0), writer and
 * tree cleanup then run unmodified over the authored chain. */
#include <windows.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_decl_eval.h"
#include "snap_type_cache.h"
#include "process_heap_scope.h"
#include "engine_globals.h"
#include "prepare_worker.h"

void backend_log(const char *message);

#define NDE_GAME_GET 0x228
#define NDE_CREATE 0x448
#define NDE_WRITE 0x378
#define NDE_DESTROY 0x470
#define NDE_DECL_SIZE 0x168
#define NDE_DECL_ALLOC 0x200
#define NDE_DECL_SOURCE_FILE 0x50   /* set only by MakeDefault after a failed parse */
#define NDE_DECL_CLASS 0x60         /* className, idAtomicString (char *) */
#define NDE_DECL_EDITOR 0x68        /* idEntityDefEditorVars, 0x98 bytes */
#define NDE_DECL_POOL_COUNT 0x10c   /* poolCount, int */
#define NDE_DECL_POOL_GRANULARITY 0x110 /* poolGranularity, int */
#define NDE_DECL_EXPAND 0x128       /* expandInheritance; the constructor sets 1 */
#define NDE_DECL_PREPARED_LENGTH 0x138
#define NDE_DECL_PREPARED_TEXT 0x140
#define NDE_CONTEXT 0x1240
#define NDE_ARGS 0x40
#define NDE_TEXT_CAP (16u * 1024u * 1024u)
#define NDE_PARSE_ANCHOR 0x19a      /* DeclParseSource: mov rcx,[owner]; mov rax,[rcx]; xor edx,edx; call [rax+0xb0] */
#define NDE_PARSE_STATE 0x2c34      /* parser-state field written by owner slot +0xb0 */
#define NDE_PARSE_IDLE 15

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
    void **parse_state_owner;
    void *(*lookup)(void *list, const char *name, char make_default);
    void *entitydef_list;
    int private_chains;     /* both parent lookups are redirected */
} nde_native;

static nde_native g;
static sh_process_heap_api g_heap;
static volatile LONG g_ready, g_disabled;
static int g_busy;

/* Definitions answering their own names while a chain is prepared. They are
 * visible to the redirected lookups of the evaluating thread only. */
#define NDE_PRIVATE_MAX 32
typedef struct nde_private { const char *name; void *decl; } nde_private;
static nde_private g_private[NDE_PRIVATE_MAX];
static size_t g_private_count;
static volatile DWORD g_private_thread;
static int g_forwarding;    /* the evaluating thread is inside a lookup passed to the engine */

static void *nde_lookup(void *list, const char *name, char make_default)
{
    void *found;
    if (!g_private_thread || g_private_thread != GetCurrentThreadId()) return g.lookup(list, name, make_default);
    /* A definition the engine loads on demand inside a passed-through lookup
     * is the engine's own: its parent lookups see only the engine's names. */
    if (!g_forwarding && list == g.entitydef_list && name)
        for (size_t i = 0; i < g_private_count; i++)
            if (!strcmp(g_private[i].name, name)) return g_private[i].decl;
    g_forwarding++;
    found = g.lookup(list, name, make_default);
    g_forwarding--;
    return found;
}

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

/* The parser bracket (owner slot +0xb0 with 0, then 15) restores the state
 * only when it was idle. The slot bodies are checked because the owner's
 * vtable is installed at run time. */
static const uint8_t *nde_parse_owner(void)
{
    static const uint8_t set_state[] = { 0x89, 0x91, 0x34, 0x2c, 0x00, 0x00, 0xc3 };
    static const uint8_t get_state[] = { 0x8b, 0x81, 0x34, 0x2c, 0x00, 0x00 };
    const uint8_t *owner, *const *table;
    if (!g.parse_state_owner || !(owner = *(const uint8_t * const *)g.parse_state_owner)) return NULL;
    if (!(table = *(const uint8_t * const * const *)owner)) return NULL;
    if (memcmp(table[0xb0 / 8], set_state, sizeof(set_state)) || memcmp(table[0xb8 / 8], get_state, sizeof(get_state)))
        return NULL;
    return owner;
}

static int nde_parse_idle(void)
{
    const uint8_t *owner = nde_parse_owner();
    return owner && *(const int32_t *)(owner + NDE_PARSE_STATE) == NDE_PARSE_IDLE;
}

/* After a parse that started idle: put the prior state back through the
 * engine's own setter if an unrecovered path left it changed. 1 when it was
 * already idle. With text set, DeclParseSource closes its bracket before
 * calling Parse (VK 0x17abb93), so a refused definition leaves it idle. */
static int nde_parse_restore(void)
{
    const uint8_t *owner = nde_parse_owner();
    if (!owner) return 0;
    if (*(const int32_t *)(owner + NDE_PARSE_STATE) == NDE_PARSE_IDLE) return 1;
    ((void (*)(const void *, int))(*(void * const * const *)owner)[0xb0 / 8])(owner, NDE_PARSE_IDLE);
    return 0;
}

/* Construct and parse one private definition. 1 parsed with prepared state,
 * 0 refused (reason set), -1 fault. */
static const char NDE_NOT_IDLE[] = "the engine parser is not idle";

static int nde_parse(void *decl, const char *definition, const char **reason, int *decl_built)
{
    uint8_t error_text[0x40];
    unsigned int parsed;
    size_t length = strlen(definition);
    /* Without text DeclParseSource returns inside its bracket (and may make a
     * default); such input is never handed to it. */
    if (!length || length >= NDE_TEXT_CAP) { *reason = "the definition text is empty or too large"; return 0; }
    if (!nde_parse_idle()) { *reason = NDE_NOT_IDLE; return 0; }
    g.decl_ctor(decl); *decl_built = 1;
    g.decl_set_text(decl, definition, (int)length);
    g.idstr_ctor(error_text);
    parsed = g.decl_parse(decl, 0, error_text);
    g.idstr_dtor(error_text);
    if (!nde_parse_restore()) { *reason = "the engine parser did not return to idle (restored)"; return -1; }
    if (parsed) { *reason = "the engine parser refused the definition"; return 0; }
    if (*(const void * const *)((const uint8_t *)decl + NDE_DECL_SOURCE_FILE)) {
        *reason = "the parsed definition acquired a source file"; return -1;
    }
    if (*(const int32_t *)((const uint8_t *)decl + NDE_DECL_PREPARED_LENGTH) <= 0 ||
        !*(const void * const *)((const uint8_t *)decl + NDE_DECL_PREPARED_TEXT)) {
        *reason = "the definition has no prepared state"; return 0;
    }
    return 1;
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
    int parsed;
    *state = NULL;
    if (!access || !(game = ((nde_game_fn)(*(void ***)access)[NDE_GAME_GET / 8])(access))) {
        *reason = "no game"; return 0;
    }
    table = *(void ***)game;
    if ((parsed = nde_parse(decl, definition, reason, decl_built)) != 1) return parsed;
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

static int nde_state_engine(const char *definition, char **state, char *detail, size_t capacity);
typedef struct nde_state_call { const char *definition; char **state; char *detail; size_t capacity; } nde_state_call;
static intptr_t nde_state_thunk(void *p)
{ nde_state_call *c = p; return nde_state_engine(c->definition, c->state, c->detail, c->capacity); }
/* From an open's preparation worker the evaluation is the engine thread's (prepare_worker.h). */
int sh_native_decl_eval_state(const char *definition, char **state, char *detail, size_t capacity)
{
    nde_state_call call = {definition, state, detail, capacity};
    if (sh_prepare_on_worker()) return (int)sh_prepare_engine(nde_state_thunk, &call);
    return nde_state_engine(definition, state, detail, capacity);
}
static int nde_state_engine(const char *definition, char **state, char *detail, size_t capacity)
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

/* Prepared-state equality (see the header): two private definitions are
 * parsed, their reader input captured and the definitions destroyed. No
 * temporary entity is created. */
typedef struct nde_prepared {
    char *state, *class_name, *editor_name, *description, *material;
    size_t length;
    uint8_t editor_values[0x28];    /* color, size, originOffset: editor vars +0x60..+0x88 */
    uint8_t editor_flags;           /* the editor vars flag bits at +0x90 */
    int32_t pool_count, pool_granularity;
    uint8_t expand_inheritance;
} nde_prepared;

/* idEntityDefEditorVars members by native consumer (R-197). The editorVars
 * parser (VK 1417ad960) sets flag bits 0 rotatable, 1 showOrientation, 2
 * showFixedBox, 3 showInRenderMode, 4 placeable, 5 instantiable, 6 noClip
 * and 7 placeableInLayers. Outside the declaration's own Parse, parser,
 * constructor, copy (VK 1417acca0, writer only), assignment (VK 1417acec0,
 * Parse only) and writer, the members are reached only through the accessor
 * (VK 1417ad1e0 / GL 1419d8a50), which is never address-taken. Its entityDef
 * callers are entity creation (VK 14030cd80 / GL 14030c590), which refuses a
 * definition without instantiable, and a telemetry record (VK 1407081d0 /
 * GL 140706ee0) copying editorName into ids-zion death and glory-kill events.
 * A typed census of both images (entityDef lookups, entity definition fields,
 * the creation and spawn path) finds no other direct reader. Every member is
 * also described by typeinfo variable entries (VK 144914240..; bitfields
 * through getter/setter thunks such as VK 1413cc510/1413cb080). Only the
 * generic typeinfo walkers invoke those thunks (R-198: getter VK 141a22e90 /
 * GL 141a15250, setter VK 141a20600 / GL 141a12840), and a reflected
 * idDeclEntityDef value meets the registered entityDef value pair, which
 * carries the declaration's text rather than its members. Direct reads through
 * by-value holders (idSnapEntity) and generic containers were closed in
 * R-201: play builds the gameplay entity from a registered copy of the map
 * declaration's text (MapEntityForSnapEntity VK 1404f1350 / GL 1404f0c50),
 * and every remaining holder of a declaration pointer keeps a separately named
 * definition, an identity token, a name for serialization or an input of the
 * spawn route; none reads an editor variable. The editorName telemetry record
 * is serialized only into ids-zion event slots sent to the AWS Kinesis
 * logging streams (VK 14181fb00, 1418211b0 / GL 141812490, 141813b40). So
 * instantiable is runtime state and every other member is editor
 * presentation: a difference there alone is EQUAL, named in the detail.
 * Parse also sets poolCount, poolGranularity and expandInheritance, whose
 * consumers are not all traced; a difference there stays UNSUPPORTED.
 * The map cannot carry these members for a placed entity (M70): its entity
 * value writer (VK 1409d1990 / GL 1409d1150) stores name, inherit, className
 * and state only; the reader (VK 1409c6160 / GL 1409c59b0) rebuilds the text
 * (VK 1417ae560 / GL 1417a1040), adding editorVars (VK 1417aed50) only while
 * the load state (VK 146dde198) is 0, and reparses. A loaded entity therefore
 * takes its editor variables from its inherit by Parse (VK 1417ad460 copies
 * them with 1417acec0), and its pool and expandInheritance settings are its
 * own text's (Parse inherits none). A difference here is a reader question,
 * never one the converted map could carry. */
#define NDE_EDITOR_RUNTIME 0x20u        /* instantiable */

static char *nde_copy(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy) { if (length) memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}

/* idStr: length +0x08, data +0x10 (as the prepared text at +0x130). */
static char *nde_idstr_copy(const uint8_t *text)
{
    int32_t length = *(const int32_t *)(text + 0x08);
    const char *data = *(const char * const *)(text + 0x10);
    if (length < 0 || (length && !data) || (size_t)length >= NDE_TEXT_CAP) return NULL;
    return nde_copy(data ? data : "", (size_t)length);
}

static void nde_prepared_free(nde_prepared *p)
{
    free(p->state); free(p->class_name); free(p->editor_name); free(p->description); free(p->material);
    memset(p, 0, sizeof(*p));
}

static int nde_capture(const uint8_t *decl, nde_prepared *out, const char **reason)
{
    int32_t length = *(const int32_t *)(decl + NDE_DECL_PREPARED_LENGTH);
    const char *text = *(const char * const *)(decl + NDE_DECL_PREPARED_TEXT);
    const char *class_name = *(const char * const *)(decl + NDE_DECL_CLASS);
    const uint8_t *editor = decl + NDE_DECL_EDITOR;
    const char *material = *(const char * const *)(editor + 0x88);
    if ((size_t)length >= NDE_TEXT_CAP) { *reason = "prepared state too large"; return 0; }
    out->length = (size_t)length;
    out->state = nde_copy(text, out->length);
    out->class_name = nde_copy(class_name ? class_name : "", class_name ? strlen(class_name) : 0);
    out->material = nde_copy(material ? material : "", material ? strlen(material) : 0);
    out->editor_name = nde_idstr_copy(editor);
    out->description = nde_idstr_copy(editor + 0x30);
    memcpy(out->editor_values, editor + 0x60, sizeof(out->editor_values));
    out->editor_flags = editor[0x90];
    out->pool_count = *(const int32_t *)(decl + NDE_DECL_POOL_COUNT);
    out->pool_granularity = *(const int32_t *)(decl + NDE_DECL_POOL_GRANULARITY);
    out->expand_inheritance = decl[NDE_DECL_EXPAND];
    if (!out->state || !out->class_name || !out->material || !out->editor_name || !out->description) {
        *reason = "allocation failed"; return 0;
    }
    return 1;
}

static int nde_prepare(void *decl, const char *definition, nde_prepared *out, const char **reason, int *decl_built)
{
    int result;
    memset(decl, 0, NDE_DECL_ALLOC);
    result = nde_parse(decl, definition, reason, decl_built);
    if (result == 1) result = nde_capture((const uint8_t *)decl, out, reason);
    if (*decl_built) { *decl_built = 0; ((nde_decl_dtor_fn)(*(void ***)decl)[0])(decl, 0); }
    return result;
}

/* Names the differing members of one group, comma separated; empty when
 * they are identical: the Parse-set pool and inheritance fields, or (with
 * presentation set) the editor variables other than instantiable (see
 * NDE_EDITOR_RUNTIME). */
static void nde_member_diff(const nde_prepared *a, const nde_prepared *b, int presentation, char *out, size_t capacity)
{
    static const char *const bits[8] = { "rotatable", "showOrientation", "showFixedBox", "showInRenderMode",
        "placeable", NULL, "noClip", "placeableInLayers" };
    const char *names[20];
    size_t count = 0, used = 0;
    unsigned flags = (unsigned)(a->editor_flags ^ b->editor_flags) & ~NDE_EDITOR_RUNTIME;
    if (!presentation) {
        if (a->pool_count != b->pool_count) names[count++] = "poolCount";
        if (a->pool_granularity != b->pool_granularity) names[count++] = "poolGranularity";
        if (!a->expand_inheritance != !b->expand_inheritance) names[count++] = "expandInheritance";
    } else {
        if (strcmp(a->editor_name, b->editor_name)) names[count++] = "editorName";
        if (strcmp(a->description, b->description)) names[count++] = "description";
        if (memcmp(a->editor_values, b->editor_values, 0x10)) names[count++] = "color";
        if (memcmp(a->editor_values + 0x10, b->editor_values + 0x10, 0x0c)) names[count++] = "size";
        if (memcmp(a->editor_values + 0x1c, b->editor_values + 0x1c, 0x0c)) names[count++] = "originOffset";
        if (strcmp(a->material, b->material)) names[count++] = "material";
        for (int bit = 0; bit < 8; bit++) if (flags & (1u << bit) && bits[bit]) names[count++] = bits[bit];
    }
    out[0] = 0;
    for (size_t i = 0; i < count && used + 1 < capacity; i++) {
        int n = _snprintf_s(out + used, capacity - used, _TRUNCATE, "%s%s", i ? ", " : "", names[i]);
        if (n < 0) break;
        used += (size_t)n;
    }
}

/* Parse the chain's private definitions root-first, each visible by name to
 * the redirected lookups once parsed, so the engine's Parse inherits class
 * and editor variables from them and the merge folds their state slices in
 * order. layers are nearest first. Returns 1, 0 refused (reason set), -1. */
static const char *nde_refused_layer;

static int nde_prepare_chain(void **members, int *built, const sh_native_eval_layer *layers, size_t count,
    const char **reason)
{
    for (size_t i = count; i-- > 0;) {
        int result;
        memset(members[i], 0, NDE_DECL_ALLOC);
        if ((result = nde_parse(members[i], layers[i].text, reason, &built[i])) != 1) {
            nde_refused_layer = layers[i].name;
            return result;
        }
        g_private[g_private_count].name = layers[i].name;
        g_private[g_private_count++].decl = members[i];
    }
    return 1;
}

/* Destroy children before parents; a member still referenced by engine state
 * after an exception is left allocated. */
static void nde_release_chain(void **members, int *built, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (built[i]) { built[i] = 0; ((nde_decl_dtor_fn)(*(void ***)members[i])[0])(members[i], 0); }
}

/* R-212: the converter's proved reduction applied identically to both
 * prepared texts; the canonical results must then be byte-identical. 1
 * equal, 0 not (reason set), -1 failure. */
static int nde_reduced_equal(const nde_prepared *t, const nde_prepared *c, const sh_native_eval_reduction *reduction,
    size_t *removed, char *reason, size_t capacity)
{
    char *left = NULL, *right = NULL;
    size_t left_length = 0, right_length = 0;
    int result = reduction->reduce(reduction->context, t->state, t->length, &left, &left_length, removed, reason, capacity);
    if (result > 0)
        result = reduction->reduce(reduction->context, c->state, c->length, &right, &right_length, removed, reason, capacity);
    if (result > 0 && (left_length != right_length || memcmp(left, right, left_length))) {
        _snprintf_s(reason, capacity, _TRUNCATE, "the prepared states differ outside the proved reduction"); result = 0;
    }
    free(left); free(right);
    return result;
}

sh_native_eval_status sh_native_decl_eval_compare(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, char *detail, size_t capacity)
{ return sh_native_decl_eval_compare_reduced(layers, layer_count, target, candidate, NULL, detail, capacity); }

static sh_native_eval_status nde_compare_engine(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, const sh_native_eval_reduction *reduction, char *detail, size_t capacity);
typedef struct nde_compare_call {
    const sh_native_eval_layer *layers; size_t layer_count; const char *target, *candidate;
    const sh_native_eval_reduction *reduction; char *detail; size_t capacity;
} nde_compare_call;
static intptr_t nde_compare_thunk(void *p)
{
    nde_compare_call *c = p;
    return (intptr_t)nde_compare_engine(c->layers, c->layer_count, c->target, c->candidate, c->reduction, c->detail, c->capacity);
}
sh_native_eval_status sh_native_decl_eval_compare_reduced(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, const sh_native_eval_reduction *reduction, char *detail, size_t capacity)
{
    nde_compare_call call = {layers, layer_count, target, candidate, reduction, detail, capacity};
    if (sh_prepare_on_worker()) return (sh_native_eval_status)sh_prepare_engine(nde_compare_thunk, &call);
    return nde_compare_engine(layers, layer_count, target, candidate, reduction, detail, capacity);
}
static sh_native_eval_status nde_compare_engine(const sh_native_eval_layer *layers, size_t layer_count,
    const char *target, const char *candidate, const sh_native_eval_reduction *reduction, char *detail, size_t capacity)
{
    nde_prepared t, c;
    char reduced_reason[200] = "";
    size_t removed = 0;
    int reduced = 0, tried = 0;
    void *decl = NULL, *members[NDE_PRIVATE_MAX] = {0};
    int built[NDE_PRIVATE_MAX] = {0};
    const char *reason = "unavailable";
    sh_native_eval_status status = SH_NATIVE_EVAL_UNAVAILABLE;
    int result = 0, decl_built = 0, allocated = 1;
    unsigned long code = 0;
    sh_process_heap_scope scope;
    char line[256];
    memset(&t, 0, sizeof(t)); memset(&c, 0, sizeof(c));
    if (!target || !candidate || (layer_count && !layers)) { nde_detail(detail, capacity, "invalid request"); return SH_NATIVE_EVAL_FAILED; }
    for (size_t i = 0; i < layer_count; i++)
        if (!layers[i].name || !*layers[i].name || !layers[i].text) { nde_detail(detail, capacity, "invalid request"); return SH_NATIVE_EVAL_FAILED; }
    if (!sh_native_decl_eval_available()) { nde_detail(detail, capacity, "unavailable"); return SH_NATIVE_EVAL_UNAVAILABLE; }
    if (layer_count && !g.private_chains) {
        nde_detail(detail, capacity, "the engine's entityDef parent lookups are not redirected on this build");
        return SH_NATIVE_EVAL_UNAVAILABLE;
    }
    if (layer_count > NDE_PRIVATE_MAX) {
        nde_detail(detail, capacity, "the private definition chain is deeper than the evaluator prepares");
        return SH_NATIVE_EVAL_UNSUPPORTED;
    }
    if (!nde_on_main_thread()) { nde_detail(detail, capacity, "not on the engine's main thread"); return SH_NATIVE_EVAL_UNAVAILABLE; }
    if (g_busy) { nde_detail(detail, capacity, "re-entered"); return SH_NATIVE_EVAL_UNAVAILABLE; }
    decl = _aligned_malloc(NDE_DECL_ALLOC, 16);
    for (size_t i = 0; i < layer_count; i++) if (!(members[i] = _aligned_malloc(NDE_DECL_ALLOC, 16))) allocated = 0;
    if (!decl || !allocated) {
        _aligned_free(decl);
        for (size_t i = 0; i < layer_count; i++) _aligned_free(members[i]);
        nde_detail(detail, capacity, "allocation failed"); return SH_NATIVE_EVAL_FAILED;
    }
    if (!sh_process_heap_enter(&g_heap, &scope)) {
        _aligned_free(decl);
        for (size_t i = 0; i < layer_count; i++) _aligned_free(members[i]);
        nde_detail(detail, capacity, "process heap scope unavailable"); return SH_NATIVE_EVAL_UNAVAILABLE;
    }
    g_busy = 1;
    nde_refused_layer = NULL;
    g_private_count = 0;
    g_private_thread = GetCurrentThreadId();
    __try {
        result = nde_prepare_chain(members, built, layers, layer_count, &reason);
        if (result == 1) result = nde_prepare(decl, target, &t, &reason, &decl_built);
        /* The candidate inherits stock definitions only: the engine's own. */
        g_private_thread = 0;
        if (result == 1) result = nde_prepare(decl, candidate, &c, &reason, &decl_built);
        nde_release_chain(members, built, layer_count);
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        result = -1; reason = "engine exception";
        decl = NULL; /* engine state may still reference them */
        memset(members, 0, sizeof(members));
    }
    g_private_thread = 0;
    g_private_count = 0;
    g_forwarding = 0;
    g_busy = 0;
    if (!sh_process_heap_leave(&scope)) { result = -1; reason = "process heap scope was not restored"; }
    _aligned_free(decl);
    for (size_t i = 0; i < layer_count; i++) _aligned_free(members[i]);
    if (result == 0 && nde_refused_layer) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "private definition '%s': %s", nde_refused_layer, reason);
        nde_detail(detail, capacity, line);
        nde_prepared_free(&t); nde_prepared_free(&c);
        return reason == NDE_NOT_IDLE ? SH_NATIVE_EVAL_UNAVAILABLE : SH_NATIVE_EVAL_UNSUPPORTED;
    }
    if (result < 0) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s (code 0x%08lx)", reason, code);
        nde_disable(line);
        nde_detail(detail, capacity, reason); status = SH_NATIVE_EVAL_FAILED;
    } else if (!result) {
        nde_detail(detail, capacity, reason);
        status = reason == NDE_NOT_IDLE ? SH_NATIVE_EVAL_UNAVAILABLE : SH_NATIVE_EVAL_UNSUPPORTED;
    } else if (strcmp(t.class_name, c.class_name)) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "different classes (%s, %s)", t.class_name, c.class_name);
        nde_detail(detail, capacity, line); status = SH_NATIVE_EVAL_UNEQUAL;
    } else if ((t.editor_flags ^ c.editor_flags) & NDE_EDITOR_RUNTIME) {
        nde_detail(detail, capacity, "different editor variable instantiable, which entity creation reads");
        status = SH_NATIVE_EVAL_UNEQUAL;
    } else if ((t.length == c.length && !memcmp(t.state, c.state, t.length)) || (reduction && reduction->reduce && (tried = 1) != 0 &&
        (reduced = nde_reduced_equal(&t, &c, reduction, &removed, reduced_reason, sizeof(reduced_reason))) > 0)) {
        char differs[200], shown[200];
        nde_member_diff(&t, &c, 0, differs, sizeof(differs));
        nde_member_diff(&t, &c, 1, shown, sizeof(shown));
        if (*differs) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "different definition members (%s), whose readers are not "
                "all traced", differs);
            status = SH_NATIVE_EVAL_UNSUPPORTED;
        } else if (*shown) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "identical prepared state (%zu bytes); only editor presentation "
                "differs (%s), which no gameplay route reads", t.length, shown);
            status = SH_NATIVE_EVAL_EQUAL;
        } else {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "identical prepared state (%zu bytes)", t.length);
            status = SH_NATIVE_EVAL_EQUAL;
        }
        if (status == SH_NATIVE_EVAL_EQUAL && reduced > 0)
            _snprintf_s(line, sizeof(line), _TRUNCATE, "prepared states equal after the proved reduction of %zu repeated "
                "complete assignment(s)%s", removed, *shown ? "; only editor presentation differs" : "");
        nde_detail(detail, capacity, line);
    } else {
        if (tried) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "the prepared states differ; %s", reduced < 0 ?
                "the proved reduction failed" : reduced_reason);
            nde_detail(detail, capacity, line);
        } else nde_detail(detail, capacity, "the prepared states differ; typed equality is not established");
        status = reduced < 0 ? SH_NATIVE_EVAL_FAILED : SH_NATIVE_EVAL_UNSUPPORTED;
    }
    nde_prepared_free(&t); nde_prepared_free(&c);
    return status;
}

static uint8_t *nde_clean(const sig_result *r, size_t n, const char *name, const uint8_t *base)
{
    for (size_t i = 0; i < n; i++)
        if (r[i].name && !strcmp(r[i].name, name) && r[i].status == SIG_OK && r[i].addr &&
            r[i].addr == (uintptr_t)base + r[i].rva) return (uint8_t *)r[i].addr;
    return NULL;
}

/* The parser's state-owner pointer: the same instruction sequence at
 * DeclParseSource +0x19a on both images (VK owner slot 0x5557090). */
static void **nde_parse_state_owner(const uint8_t *parse)
{
    static const uint8_t tail[] = { 0x48, 0x8b, 0x01, 0x33, 0xd2, 0xff, 0x90, 0xb0, 0x00, 0x00, 0x00 };
    const uint8_t *at = parse + NDE_PARSE_ANCHOR;
    int32_t rel;
    if (at[0] != 0x48 || at[1] != 0x8b || at[2] != 0x0d || memcmp(at + 7, tail, sizeof(tail))) return NULL;
    memcpy(&rel, at + 3, sizeof(rel));
    return (void **)(at + 7 + rel);
}

/* A parent lookup call: xor r8d,r8d; lea rcx,[rip+list]; call lookup. */
static int nde_parent_call(const uint8_t *site, void **list, void **lookup)
{
    int32_t rel;
    if (!site || site[0] != 0xe8 || memcmp(site - 10, "\x45\x33\xc0\x48\x8d\x0d", 6)) return 0;
    memcpy(&rel, site - 4, sizeof(rel)); *list = (void *)(site + rel);
    memcpy(&rel, site + 1, sizeof(rel)); *lookup = (void *)(site + 5 + rel);
    return 1;
}

/* mov rax, nde_lookup; jmp rax, within rel32 reach of both call sites. */
static uint8_t *nde_relay(const uint8_t *a, const uint8_t *b)
{
    SYSTEM_INFO info;
    uintptr_t low = (uintptr_t)(a < b ? a : b), high = (uintptr_t)(a < b ? b : a), step;
    void *handler = (void *)nde_lookup;
    GetSystemInfo(&info);
    step = info.dwAllocationGranularity;
    for (uintptr_t distance = step; distance < 0x7ff00000u - (high - low); distance += step) {
        for (int side = 0; side < 2; side++) {
            uintptr_t address = side ? ((high + distance) & ~(step - 1)) : ((low - distance) & ~(step - 1));
            MEMORY_BASIC_INFORMATION mbi;
            uint8_t *relay;
            DWORD old;
            if (!side && distance > low) continue;
            if (!VirtualQuery((void *)address, &mbi, sizeof(mbi)) || mbi.State != MEM_FREE) continue;
            if (!(relay = VirtualAlloc((void *)address, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) continue;
            if ((intptr_t)(relay - a) > INT32_MAX - 64 || (intptr_t)(a - relay) > INT32_MAX - 64 ||
                (intptr_t)(relay - b) > INT32_MAX - 64 || (intptr_t)(b - relay) > INT32_MAX - 64) {
                VirtualFree(relay, 0, MEM_RELEASE); continue;
            }
            relay[0] = 0x48; relay[1] = 0xb8; memcpy(relay + 2, &handler, sizeof(handler));
            relay[10] = 0xff; relay[11] = 0xe0;
            if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old)) { VirtualFree(relay, 0, MEM_RELEASE); return NULL; }
            FlushInstructionCache(GetCurrentProcess(), relay, 12);
            return relay;
        }
    }
    return NULL;
}

/* Move a verified call from one target to another with one aligned 8-byte
 * compare-exchange of the qword holding its operand, so no thread fetches a
 * torn operand (the 4-byte form is code_patch_call_sig's). The new target
 * must be complete first; the relay is never released, because a thread may
 * have entered it before a redirect is undone. */
static int nde_redirect(uint8_t *site, const void *from, const void *to)
{
    uintptr_t operand = (uintptr_t)site + 1, word = operand & ~(uintptr_t)7;
    intptr_t distance = (intptr_t)to - (intptr_t)(site + 5);
    int32_t before, after;
    LONG64 old_word, new_word;
    DWORD protect, ignored;
    int ok = 0;
    memcpy(&before, site + 1, sizeof(before));
    if (operand + 4 > word + 8 || site[0] != 0xe8 || site + 5 + before != (const uint8_t *)from ||
        distance < INT32_MIN || distance > INT32_MAX) return 0;
    after = (int32_t)distance;
    if (!VirtualProtect((void *)word, 8, PAGE_EXECUTE_READWRITE, &protect)) return 0;
    __try {
        old_word = *(volatile LONG64 *)word;
        new_word = old_word;
        if (!memcmp((uint8_t *)&old_word + (operand - word), &before, 4)) {
            memcpy((uint8_t *)&new_word + (operand - word), &after, 4);
            ok = InterlockedCompareExchange64((volatile LONG64 *)word, new_word, old_word) == old_word;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    VirtualProtect((void *)word, 8, protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void *)word, 8);
    return ok;
}

/* Bind the parent lookups of Parse and the inheritance merge, once per
 * process (a second decode would read the relay as the lookup). Both must name
 * the same lookup and list; a relay is installed only for both. */
static volatile LONG g_chains_attempted;

static void nde_install_private_chains(const sig_result *r, size_t n, const uint8_t *base)
{
    uint8_t *parse_call = nde_clean(r, n, "DeclEntityDefParseParentCall", base);
    uint8_t *merge_call = nde_clean(r, n, "DeclEntityDefMergeParentCall", base);
    void *list_a, *list_b, *lookup_a, *lookup_b;
    uint8_t *relay;
    if (InterlockedExchange(&g_chains_attempted, 1)) return;
    if (!nde_parent_call(parse_call, &list_a, &lookup_a) || !nde_parent_call(merge_call, &list_b, &lookup_b) ||
        list_a != list_b || lookup_a != lookup_b) {
        backend_log("NATIVE-EVAL: entityDef parent lookups not verified; private definition chains stay unevaluated");
        return;
    }
    g.lookup = (void *(*)(void *, const char *, char))lookup_a;
    g.entitydef_list = list_a;
    if (!(relay = nde_relay(parse_call, merge_call))) {
        backend_log("NATIVE-EVAL: no relay within reach of the entityDef parent lookups; private definition chains stay unevaluated");
        return;
    }
    /* Both or neither: a Parse redirect without the merge's is undone. The
     * relay passes every lookup through outside an evaluation, so a redirect
     * that cannot be undone stays inert. */
    if (!nde_redirect(parse_call, lookup_a, relay)) {
        backend_log("NATIVE-EVAL: entityDef parent lookups could not be redirected; private definition chains stay unevaluated");
        return;
    }
    if (!nde_redirect(merge_call, lookup_a, relay)) {
        backend_log(nde_redirect(parse_call, relay, lookup_a) ?
            "NATIVE-EVAL: the inheritance merge's parent lookup could not be redirected; Parse's was restored; private definition chains stay unevaluated" :
            "NATIVE-EVAL: the inheritance merge's parent lookup could not be redirected; Parse's stays on the pass-through relay; private definition chains stay unevaluated");
        return;
    }
    g.private_chains = 1;
    backend_log("NATIVE-EVAL: private definition chains evaluate through the engine's Parse and inheritance merge");
}

int sh_native_decl_eval_install(const sig_result *r, size_t n, const uint8_t *base)
{
    sh_snap_entity_route route;
    uint8_t *ctor = nde_clean(r, n, "DeclEntityDefCtor", base), *text = nde_clean(r, n, "DeclSetText", base);
    uint8_t *parse = nde_clean(r, n, "DeclParseSource", base), *written = nde_clean(r, n, "ContextWrittenText", base);
    uint8_t *idstr_ctor = nde_clean(r, n, "DefaultIdStrCtor", base), *idstr_dtor = nde_clean(r, n, "IdStrDtor", base);
    uintptr_t main_thread = base ? glb_resolve(base, "main_thread_id", NULL) : 0;
    void **owner = parse ? nde_parse_state_owner(parse) : NULL;
    if (InterlockedCompareExchange(&g_ready, 0, 0)) return 1;
    if (!ctor || !text || !parse || !written || !idstr_ctor || !idstr_dtor || !main_thread || !owner || !sh_snap_type_cache_entity_route(&route) ||
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
    g.parse_state_owner = owner;
    nde_install_private_chains(r, n, base);
    InterlockedExchange(&g_ready, 1);
    backend_log("NATIVE-EVAL: native entity-state evaluation available");
    return 1;
}

#ifdef SH_NATIVE_DECL_EVAL_TESTING
/* A heap stack shaped like the native one: 32 IDs at +0x44, depth at +0xc4. */
static uint8_t nde_test_heap[0xc8];
static void *nde_test_heap_get(void) { return nde_test_heap; }
static void nde_test_heap_push(void *self, int heap)
{
    int *depth = (int *)((uint8_t *)self + 0xc4);
    ((int *)((uint8_t *)self + 0x44))[*depth] = heap; ++*depth;
}
static void nde_test_heap_pop(void *self) { --*(int *)((uint8_t *)self + 0xc4); }
/* The redirected call sites' target. */
void *sh_native_decl_eval_test_lookup(void *list, const char *name, char make_default)
{
    return nde_lookup(list, name, make_default);
}
int sh_native_decl_eval_test_install_chains(const sig_result *r, size_t n, const uint8_t *base, int first)
{
    if (first) { g.private_chains = 0; InterlockedExchange(&g_chains_attempted, 0); }
    nde_install_private_chains(r, n, base);
    return g.private_chains;
}
void sh_native_decl_eval_test_bind(const sh_native_decl_eval_native *n)
{
    memcpy(&g, n, sizeof(g));
    g_heap.get = nde_test_heap_get; g_heap.push = nde_test_heap_push; g_heap.pop = nde_test_heap_pop;
    InterlockedExchange(&g_disabled, 0);
    InterlockedExchange(&g_ready, n && n->game_access ? 1 : 0);
}
#endif
