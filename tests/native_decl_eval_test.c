/* native_decl_eval prepared-state comparison over a synthetic engine.
 *
 * The fake parser stores "class|editor variables|state" from the definition
 * text in the recovered idDeclEntityDef layout: className +0x60, editor
 * variables +0x68 (editorName idStr length +0x08 and data +0x10, color +0x60,
 * flags +0x90 within them), prepared idStr +0x130 (length +0x138, data
 * +0x140). Its own state slice (+0x148) and parent name (+0x158) are fake
 * fields the evaluator never reads. The parser-state owner exposes the
 * recovered slot +0xb0/+0xb8 bodies, executable, and its +0x2c34 field. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/native_decl_eval.h"
#include "../src/backend/snap_type_cache.h"
#include "../src/backend/engine_globals.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static char last_log[256];
void backend_log(const char *message) { _snprintf_s(last_log, sizeof(last_log), _TRUNCATE, "%s", message); }
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{ (void)module_base; (void)name; (void)out_status; return 0; }
int sh_snap_type_cache_entity_route(sh_snap_entity_route *out) { (void)out; return 0; }

static const uint8_t set_state_bytes[] = { 0x89, 0x91, 0x34, 0x2c, 0x00, 0x00, 0xc3 };
static const uint8_t get_state_bytes[] = { 0x8b, 0x81, 0x34, 0x2c, 0x00, 0x00, 0xc3 };
static uint8_t *set_state_body, *get_state_body;
static void *owner_table[0x20];
static uint8_t *owner;
static void *owner_slot;
static int ctors, dtors, parses, game_dummy;
static void *game_access = &game_dummy;
static volatile uint32_t main_thread;
static char text_copy[512];
static int leave_busy;

static void fake_dtor(void *decl, unsigned int flags)
{
    uint8_t *d = decl;
    (void)flags;
    free(*(void **)(d + 0x140)); free(*(void **)(d + 0x60)); free(*(void **)(d + 0x148)); free(*(void **)(d + 0x158));
    free(*(void **)(d + 0x78));
    dtors++;
}
static void *decl_table[1] = { (void *)fake_dtor };

static void fake_ctor(void *decl)
{
    memset(decl, 0, 0x168);
    *(void **)decl = decl_table;
    *((uint8_t *)decl + 0x128) = 1;
    ctors++;
}

static void fake_set_text(void *decl, const char *text, int length)
{
    (void)decl;
    _snprintf_s(text_copy, sizeof(text_copy), _TRUNCATE, "%.*s", length, text);
}

/* The engine's entityDef list and lookup: served stock definitions. Looking
 * up stock_base while nested_probe is set loads it "on demand", which parses
 * an engine definition whose parent is nested_probe. */
static int entitydef_list, other_list;
static uint8_t engine_stock[0x200];
static int engine_lookups, private_answers_off_thread, other_list_probe, nested_private_answers, nested_fired;
static const char *nested_probe;
static char last_engine_name[64];
static void *(*parse_lookup)(void *list, const char *name, char make_default) = sh_native_decl_eval_test_lookup;
static void *(*merge_lookup)(void *list, const char *name, char make_default) = sh_native_decl_eval_test_lookup;
static int private_decl(const void *found) { return found && found != engine_stock; }
static void *engine_lookup(void *list, const char *name, char make_default)
{
    (void)make_default;
    engine_lookups++;
    _snprintf_s(last_engine_name, sizeof(last_engine_name), _TRUNCATE, "%s", name);
    if (nested_probe && list == &entitydef_list && !strcmp(name, "stock_base")) {
        nested_fired++;
        if (private_decl(parse_lookup(&entitydef_list, nested_probe, 0))) nested_private_answers++;
        if (private_decl(merge_lookup(&entitydef_list, nested_probe, 0))) nested_private_answers++;
    }
    return list == &entitydef_list && !strcmp(name, "stock_base") ? engine_stock : NULL;
}
static DWORD WINAPI off_thread_lookup(LPVOID name)
{
    if (private_decl(parse_lookup(&entitydef_list, name, 0))) private_answers_off_thread++;
    return 0;
}

static void parse_done(void) { if (!leave_busy) *(int32_t *)(owner + 0x2c34) = 15; }

/* "[^parent|]class|editor|state" ("-" inherits the field, the editor
 * variables as a whole as Parse copies them; otherwise editor is
 * "flags[@editorName][*]", '*' setting color red to 0.5); "FAIL" refuses,
 * "SOURCE" acquires a source file. Like the engine, Parse looks up the parent
 * (class and editor variables) and the inheritance merge looks up every
 * ancestor and folds their own slices root-first. */
static unsigned int fake_parse(void *decl, unsigned char read_source, void *error_text)
{
    uint8_t *d = decl, *parent = NULL, *chain[40];
    char *parent_name = NULL, *body = text_copy, *bar1, *bar2, *state;
    const char *name;
    size_t length, depth = 0;
    (void)read_source; (void)error_text;
    parses++;
    *(int32_t *)(owner + 0x2c34) = 0;
    if (!strcmp(text_copy, "FAIL")) { parse_done(); return 1; }
    if (!strcmp(text_copy, "SOURCE")) { *(void **)(d + 0x50) = d; parse_done(); return 0; }
    if (body[0] == '^') {
        parent_name = body + 1;
        if (!(body = strchr(body, '|'))) { parse_done(); return 1; }
        *body++ = 0;
        parent = parse_lookup(&entitydef_list, parent_name, 0);
        if (other_list_probe && private_decl(parse_lookup(&other_list, parent_name, 0))) private_answers_off_thread++;
        if (other_list_probe) {
            HANDLE thread = CreateThread(NULL, 0, off_thread_lookup, parent_name, 0, NULL);
            if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
        }
    }
    bar1 = strchr(body, '|'); bar2 = bar1 ? strchr(bar1 + 1, '|') : NULL;
    if (!bar2) { parse_done(); return 1; }
    *bar1 = 0; *bar2 = 0;
    /* "#count,granularity,expand" sets the Parse-set declaration fields. */
    {
        char *pool = strchr(bar1 + 1, '#');
        int expand = 1;
        if (pool) {
            *pool++ = 0;
            sscanf_s(pool, "%d,%d,%d", (int *)(d + 0x10c), (int *)(d + 0x110), &expand);
            d[0x128] = (uint8_t)expand;
        }
    }
    if (!strcmp(body, "-") && !parent) { parse_done(); return 1; } /* No class specified */
    *(char **)(d + 0x60) = _strdup(!strcmp(body, "-") ? *(char **)(parent + 0x60) : body);
    if (!strcmp(bar1 + 1, "-")) {
        if (parent) {
            memcpy(d + 0x68, parent + 0x68, 0x98);
            if (*(char **)(parent + 0x78)) *(char **)(d + 0x78) = _strdup(*(char **)(parent + 0x78));
        }
    } else {
        char *name = strchr(bar1 + 1, '@'), *red = strchr(bar1 + 1, '*');
        d[0x68 + 0x90] = (uint8_t)atoi(bar1 + 1);
        if (red) { *red = 0; *(float *)(d + 0x68 + 0x60) = 0.5f; }
        if (name) { *(char **)(d + 0x78) = _strdup(name + 1); *(int32_t *)(d + 0x70) = (int32_t)strlen(name + 1); }
    }
    *(char **)(d + 0x148) = _strdup(bar2 + 1);
    *(char **)(d + 0x158) = parent_name ? _strdup(parent_name) : NULL;
    /* The merge: every ancestor by name, then own slices root-first. */
    length = strlen(bar2 + 1) + 1;
    for (name = parent_name; name && depth < 40; name = *(const char **)(chain[depth++] + 0x158)) {
        if (!(chain[depth] = merge_lookup(&entitydef_list, name, 0))) { length += strlen(name) + 10; break; }
        length += strlen(*(const char **)(chain[depth] + 0x148)) + 1;
    }
    state = calloc(1, length);
    if (name && depth < 40 && !chain[depth]) _snprintf_s(state, length, _TRUNCATE, "UNKNOWN(%s)+", name);
    while (depth-- > 0) {
        strcat_s(state, length, *(const char **)(chain[depth] + 0x148)); strcat_s(state, length, "+");
    }
    strcat_s(state, length, bar2 + 1);
    *(int32_t *)(d + 0x138) = (int32_t)strlen(state);
    *(char **)(d + 0x140) = state;
    *(void **)(d + 0x160) = parent;
    parse_done();
    return 0;
}

static void noop_idstr(void *text) { (void)text; }

static int redirected = 1;
static void bind_engine(void)
{
    sh_native_decl_eval_native n;
    memset(&n, 0, sizeof(n));
    n.game_access = &game_access;
    n.decl_ctor = fake_ctor; n.decl_set_text = fake_set_text; n.decl_parse = fake_parse;
    n.idstr_ctor = noop_idstr; n.idstr_dtor = noop_idstr;
    n.main_thread = &main_thread;
    n.parse_state_owner = &owner_slot;
    n.lookup = engine_lookup; n.entitydef_list = &entitydef_list; n.private_chains = redirected;
    sh_native_decl_eval_test_bind(&n);
}

/* R-212 evaluator authority: a reduction maps "<state>~<n>" to "<state>"
 * and refuses "refuse"; it applies only to the prepared state texts. */
static int fake_reduce(void *context, const char *state, size_t length, char **reduced, size_t *reduced_length,
    size_t *removed, char *reason, size_t capacity)
{
    const char *mark = memchr(state, '~', length);
    (void)context;
    if (length == 6 && !memcmp(state, "refuse", 6)) { snprintf(reason, capacity, "outside the proof"); return 0; }
    if (mark) { length = (size_t)(mark - state); (*removed)++; }
    if (!(*reduced = malloc(length + 1))) return -1;
    memcpy(*reduced, state, length); (*reduced)[length] = 0; *reduced_length = length;
    return 1;
}
static sh_native_eval_status reduced_cmp(const char *target, const char *candidate, char *detail)
{
    sh_native_eval_reduction reduction = {NULL, fake_reduce};
    detail[0] = 0;
    return sh_native_decl_eval_compare_reduced(NULL, 0, target, candidate, &reduction, detail, 256);
}

static sh_native_eval_status cmp(const char *target, const char *candidate, char *detail)
{
    detail[0] = 0;
    return sh_native_decl_eval_compare(NULL, 0, target, candidate, detail, 256);
}

static sh_native_eval_status chain(const sh_native_eval_layer *layers, size_t count, const char *target,
    const char *candidate, char *detail)
{
    detail[0] = 0;
    return sh_native_decl_eval_compare(layers, count, target, candidate, detail, 256);
}

/* Private chains, nearest first; the engine serves only stock_base (class
 * idAI2, flags 0, own and prepared "S"). */
static void private_chains(void)
{
    char detail[256];
    sh_native_eval_layer two[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "^stock_base|-|-|B" } };
    sh_native_eval_layer classed[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "^stock_base|idProp2|-|B" } };
    sh_native_eval_layer flagged[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "^stock_base|-|4|B" } };
    sh_native_eval_layer made[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "^stock_base|-|32|B" } };
    sh_native_eval_layer clipped[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "^stock_base|-|64|B" } };
    sh_native_eval_layer named[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "^stock_base|-|0@pkg/odd*|B" } };
    sh_native_eval_layer refused[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "FAIL" } };
    sh_native_eval_layer empty[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "" } };
    sh_native_eval_layer altered[1] = { { "stock_base", "idProp2|7|ALT" } };
    sh_native_eval_layer deep[33];
    int before = ctors;

    /* Root-first: the merge folds stock, base, child, then the target. */
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(ctors - before == 4 && ctors == dtors);
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^stock_base|-|-|C+B+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    /* Class and editor variables come through the private parent's Parse. */
    CHECK(chain(classed, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(strstr(detail, "idProp2") != NULL);
    CHECK(chain(classed, 2, "^pkg_child|-|-|T", "^stock_base|idProp2|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    /* An inherited showFixedBox (4) is editor presentation, which no gameplay
     * route reads (R-201): equal, with the member named. */
    CHECK(chain(flagged, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "editor presentation") && strstr(detail, "(showFixedBox)"));
    CHECK(chain(flagged, 2, "^pkg_child|-|-|T", "^stock_base|-|4|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "identical prepared state") != NULL);
    /* An inherited instantiable (32) is runtime state (entity creation reads
     * it); noClip (64), editorName and color are presentation (R-201). */
    CHECK(chain(made, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(strstr(detail, "instantiable") != NULL);
    CHECK(chain(made, 2, "^pkg_child|-|-|T", "^stock_base|-|32|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(chain(clipped, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "editor presentation") && strstr(detail, "(noClip)"));
    CHECK(chain(named, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "editor presentation") && strstr(detail, "(editorName, color)"));
    CHECK(chain(named, 2, "^pkg_child|-|-|T", "^stock_base|-|0@pkg/odd*|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "identical prepared state") != NULL);
    CHECK(chain(named, 2, "^pkg_child|-|-|T", "^stock_base|-|32@pkg/odd*|B+C+T", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(strstr(detail, "instantiable") != NULL);
    CHECK(chain(named, 2, "^pkg_child|-|-|T", "^stock_base|-|0@pkg/odd*|B+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    /* Parse-set pool and inheritance fields of a privately inherited target
     * are compared with the candidate's; untraced consumers keep a difference
     * UNSUPPORTED (R-198). */
    CHECK(chain(two, 2, "^pkg_child|-|-#4,2,1|T", "^stock_base|-|-#0,2,1|B+C+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(strstr(detail, "(poolCount)") != NULL);
    CHECK(chain(two, 2, "^pkg_child|-|-#4,2,1|T", "^stock_base|-|-#4,2,1|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(chain(two, 2, "^pkg_child|-|-#0,8,0|T", "^stock_base|-|-#0,2,1|B+C+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(strstr(detail, "(poolGranularity, expandInheritance)") != NULL);
    /* A pool difference is not excused by presentation (R-201 keeps pool
     * fields UNSUPPORTED). */
    CHECK(chain(named, 2, "^pkg_child|-|-#4,2,1|T", "^stock_base|-|-#0,2,1|B+C+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(strstr(detail, "(poolCount)") && !strstr(detail, "editorName"));
    /* A runtime difference still dominates. */
    CHECK(chain(made, 2, "^pkg_child|-|-#4,2,1|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_UNEQUAL);

    /* An altered stock name answers the target's lookups with the authored
     * copy; the candidate still reaches the engine's stock definition. */
    CHECK(chain(altered, 1, "^stock_base|-|-|T", "idProp2|7|ALT+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(chain(altered, 1, "^stock_base|-|-|T", "^stock_base|-|-|T", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(strstr(detail, "idProp2") != NULL && strstr(detail, "idAI2") != NULL);

    /* Private names answer only this evaluation's lookups, on its thread and
     * list: never the candidate, another thread, another list or afterwards.
     * The candidate's Parse asks the engine for pkg_base and is refused. */
    engine_lookups = 0;
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^pkg_base|-|-|C+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(!strcmp(last_engine_name, "pkg_base") && engine_lookups == 5 && strstr(detail, "refused") != NULL);
    private_answers_off_thread = 0; other_list_probe = 1;
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    other_list_probe = 0;
    CHECK(private_answers_off_thread == 0);
    CHECK(parse_lookup(&entitydef_list, "pkg_child", 0) == NULL);
    CHECK(parse_lookup(&entitydef_list, "stock_base", 0) == engine_stock);
    /* A definition the engine loads inside a passed-through lookup sees only
     * the engine's names, even one a private layer also carries. */
    nested_private_answers = nested_fired = 0; nested_probe = "pkg_child";
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    nested_probe = NULL;
    CHECK(nested_fired == 6 && nested_private_answers == 0);

    /* A refused or empty layer names itself; every built definition is destroyed. */
    CHECK(chain(refused, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(strstr(detail, "pkg_base") != NULL && ctors == dtors);
    parses = 0;
    CHECK(chain(empty, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(strstr(detail, "pkg_base") != NULL && parses == 0 && ctors == dtors);
    /* Without its private parent a class-less layer is refused, as Parse does. */
    CHECK(chain(two, 1, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(strstr(detail, "pkg_child") != NULL && ctors == dtors);
    for (size_t i = 0; i < 33; i++) deep[i] = two[1];
    parses = 0;
    CHECK(chain(deep, 33, "a|0|x", "a|0|x", detail) == SH_NATIVE_EVAL_UNSUPPORTED && parses == 0);
    /* A layer leaving the parser busy is a fault: the engine's setter puts the
     * idle state back, evaluation is disabled and every definition released. */
    leave_busy = 1;
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_FAILED);
    leave_busy = 0;
    CHECK(*(int32_t *)(owner + 0x2c34) == 15);
    CHECK(!sh_native_decl_eval_available() && ctors == dtors);
    CHECK(parse_lookup(&entitydef_list, "pkg_base", 0) == NULL);
    bind_engine();
}

/* Call sites shaped like the engine's (sub rsp,28h; mov rdx,rcx; xor r8d,r8d;
 * lea rcx,[rip+list]; call lookup; add rsp,28h; ret) in executable memory
 * within rel32 reach of this program. The call is at +17, its operand at +18. */
static uint8_t *near_code(void)
{
    uintptr_t self = (uintptr_t)GetModuleHandleA(NULL);
    for (uintptr_t step = 0x1000000; step < 0x60000000; step += 0x1000000) {
        void *at = VirtualAlloc((void *)(self - step), 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!at) at = VirtualAlloc((void *)(self + step), 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (at) return at;
    }
    return NULL;
}
static uint8_t *emit_site(uint8_t *at, void *list, void *lookup)
{
    static const uint8_t head[] = { 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0xca, 0x45, 0x33, 0xc0, 0x48, 0x8d, 0x0d };
    int32_t rel;
    memcpy(at, head, sizeof(head));
    rel = (int32_t)((uint8_t *)list - (at + 17)); memcpy(at + 13, &rel, 4);
    at[17] = 0xe8; rel = (int32_t)((uint8_t *)lookup - (at + 22)); memcpy(at + 18, &rel, 4);
    memcpy(at + 22, "\x48\x83\xc4\x28\xc3", 5);
    return at;
}
static const uint8_t *call_target(const uint8_t *site)
{
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    return site + 5 + rel;
}

static void *(*parse_site)(const char *name), *(*merge_site)(const char *name);
static void *site_parse(void *list, const char *name, char make_default)
{ return list == &entitydef_list ? parse_site(name) : sh_native_decl_eval_test_lookup(list, name, make_default); }
static void *site_merge(void *list, const char *name, char make_default)
{ return list == &entitydef_list ? merge_site(name) : sh_native_decl_eval_test_lookup(list, name, make_default); }

static void redirect_install(void)
{
    char detail[256];
    sh_native_eval_layer two[2] = { { "pkg_child", "^pkg_base|-|-|C" }, { "pkg_base", "^stock_base|-|-|B" } };
    uint8_t *page = near_code(), *parse_fn, *merge_fn, *straddle_fn, original[5];
    sig_result r[2];
    void *(*parse_call)(const char *), *(*merge_call)(const char *);
    CHECK(page != NULL);
    if (!page) return;
    /* Parse's operand is not 4-byte aligned but lies in one aligned qword, as
     * on both images; the third site's operand straddles two qwords. */
    parse_fn = emit_site(page + 0x107, &entitydef_list, (void *)engine_lookup);
    merge_fn = emit_site(page + 0x207, &entitydef_list, (void *)engine_lookup);
    straddle_fn = emit_site(page + 0x303, &entitydef_list, (void *)engine_lookup);
    parse_call = (void *(*)(const char *))parse_fn; merge_call = (void *(*)(const char *))merge_fn;
    memcpy(original, parse_fn + 17, 5);
    memset(r, 0, sizeof(r));
    r[0].name = "DeclEntityDefParseParentCall"; r[0].status = SIG_OK;
    r[0].addr = (uintptr_t)parse_fn + 17; r[0].rva = 0x107 + 17;
    r[1].name = "DeclEntityDefMergeParentCall"; r[1].status = SIG_OK;
    r[1].addr = (uintptr_t)straddle_fn + 17; r[1].rva = 0x303 + 17;
    engine_lookups = 0;
    CHECK(parse_call("stock_base") == engine_stock && engine_lookups == 1);

    /* The merge site cannot be redirected: Parse's redirect is undone. */
    CHECK(sh_native_decl_eval_test_install_chains(r, 2, page, 1) == 0);
    CHECK(!memcmp(parse_fn + 17, original, 5) && strstr(last_log, "was restored") != NULL);
    CHECK(call_target(straddle_fn + 17) == (const uint8_t *)engine_lookup);
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_UNAVAILABLE);
    /* Differing lists are refused before any write. */
    r[1].addr = (uintptr_t)emit_site(page + 0x407, &other_list, (void *)engine_lookup) + 17; r[1].rva = 0x407 + 17;
    CHECK(sh_native_decl_eval_test_install_chains(r, 2, page, 1) == 0 && !memcmp(parse_fn + 17, original, 5));

    /* Both redirect through one relay to the evaluator's lookup. */
    r[1].addr = (uintptr_t)merge_fn + 17; r[1].rva = 0x207 + 17;
    CHECK(sh_native_decl_eval_test_install_chains(r, 2, page, 1) == 1);
    CHECK(call_target(parse_fn + 17) == call_target(merge_fn + 17) &&
        call_target(parse_fn + 17) != (const uint8_t *)engine_lookup);
    CHECK(parse_fn[17] == 0xe8 && merge_fn[17] == 0xe8 && !memcmp(parse_fn, "\x48\x83\xec\x28", 4));
    CHECK(!memcmp(parse_fn + 22, "\x48\x83\xc4\x28\xc3", 5));
    /* Outside an evaluation every name passes through. */
    engine_lookups = 0;
    CHECK(parse_call("stock_base") == engine_stock && merge_call("pkg_base") == NULL && engine_lookups == 2);

    /* The private chain cases again, through the redirected machine code. */
    parse_site = parse_call; merge_site = merge_call;
    parse_lookup = site_parse; merge_lookup = site_merge;
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "identical prepared state") != NULL);
    private_answers_off_thread = 0; other_list_probe = 1;
    nested_private_answers = nested_fired = 0; nested_probe = "pkg_base";
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^stock_base|-|-|B+C+T", detail) == SH_NATIVE_EVAL_EQUAL);
    other_list_probe = 0; nested_probe = NULL;
    CHECK(private_answers_off_thread == 0 && nested_fired == 8 && nested_private_answers == 0); /* 2 off-thread */
    CHECK(chain(two, 2, "^pkg_child|-|-|T", "^pkg_base|-|-|C+T", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(parse_call("pkg_child") == NULL && ctors == dtors);
    /* A later install is refused: it would decode the relay as the lookup. */
    {
        const uint8_t *relay = call_target(parse_fn + 17);
        CHECK(sh_native_decl_eval_test_install_chains(r, 2, page, 0) == 1);
        CHECK(call_target(parse_fn + 17) == relay && parse_call("stock_base") == engine_stock);
    }
    parse_lookup = merge_lookup = sh_native_decl_eval_test_lookup;
}

int main(void)
{
    char detail[256];
    sh_native_eval_layer layer = { "child", "^stock_base|-|-|C" };
    uint8_t *code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    owner = calloc(1, 0x3000);
    if (!code || !owner) { printf("FAIL native_decl_eval_test (setup)\n"); return 1; }
    set_state_body = code; get_state_body = code + 0x40;
    memcpy(set_state_body, set_state_bytes, sizeof(set_state_bytes));
    memcpy(get_state_body, get_state_bytes, sizeof(get_state_bytes));
    owner_table[0xb0 / 8] = set_state_body;
    owner_table[0xb8 / 8] = get_state_body;
    *(void **)owner = owner_table;
    *(int32_t *)(owner + 0x2c34) = 15;
    owner_slot = owner;
    main_thread = GetCurrentThreadId();
    *(char **)(engine_stock + 0x60) = "idAI2";
    *(int32_t *)(engine_stock + 0x138) = 1;
    *(char **)(engine_stock + 0x140) = "S";
    *(char **)(engine_stock + 0x148) = "S";
    bind_engine();

    CHECK(cmp("idProp|0|edit = { a = 1; }", "idProp|0|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "identical prepared state") != NULL);
    CHECK(cmp("idProp|0|edit = { a = 1; }", "idProp|0|edit = { a = 2; }", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(cmp("idProp|0|edit = { a = 1; }", "idLight|0|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(strstr(detail, "different classes") != NULL);
    /* A proved reduction equates prepared texts only through the same
     * canonical result for both; class and instantiable checks still decide
     * first, and a refused or different result stays UNSUPPORTED. */
    CHECK(reduced_cmp("idProp|0|S~1", "idProp|0|S", detail) == SH_NATIVE_EVAL_EQUAL && strstr(detail, "proved reduction of 1"));
    CHECK(cmp("idProp|0|S~1", "idProp|0|S", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(reduced_cmp("idProp|0|S~1", "idLight|0|S", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(reduced_cmp("idProp|33|S~1", "idProp|1|S", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(reduced_cmp("idProp|0|S~1", "idProp|0|T", detail) == SH_NATIVE_EVAL_UNSUPPORTED && strstr(detail, "outside the proved"));
    CHECK(reduced_cmp("idProp|0|refuse", "idProp|0|S", detail) == SH_NATIVE_EVAL_UNSUPPORTED && strstr(detail, "outside the proof"));
    /* instantiable (32) is runtime state; the other members are editor
     * presentation: equal with the members named (R-201). */
    CHECK(cmp("idProp|1|edit = { a = 1; }", "idProp|0|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "editor presentation") && strstr(detail, "(rotatable)"));
    CHECK(cmp("idProp|1|edit = { a = 1; }", "idProp|0|edit = { a = 2; }", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(cmp("idProp|33|edit = { a = 1; }", "idProp|1|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(strstr(detail, "instantiable") != NULL);
    CHECK(cmp("idProp|64|edit = { a = 1; }", "idProp|0|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "(noClip)") != NULL);
    CHECK(cmp("idProp|0@ai/odd|edit = { a = 1; }", "idProp|0|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "(editorName)") != NULL);
    CHECK(cmp("idProp|0@ai/odd|edit = { a = 1; }", "idProp|0@ai/odd|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_EQUAL);
    /* Directly authored pool fields are compared too (R-198). */
    CHECK(cmp("idProp|0#2,2,1|edit = { a = 1; }", "idProp|0#0,2,1|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(strstr(detail, "(poolCount)") != NULL);
    CHECK(cmp("idProp|0#2,2,1|edit = { a = 1; }", "idProp|0#2,2,1|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "identical prepared state") != NULL);
    CHECK(cmp("idProp|78@ai/odd*|edit = { a = 1; }", "idProp|0|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_EQUAL);
    CHECK(strstr(detail, "(editorName, color, showOrientation, showFixedBox, showInRenderMode, noClip)") != NULL);
    CHECK(cmp("idProp|78@ai/odd*#2,2,1|edit = { a = 1; }", "idProp|0#0,2,1|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(strstr(detail, "(poolCount)") != NULL);
    CHECK(cmp("idProp|78@ai/odd*|edit = { a = 1; }", "idProp|0|edit = { a = 2; }", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(cmp("idProp|32@ai/odd|edit = { a = 1; }", "idProp|0|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(strstr(detail, "instantiable") != NULL);
    CHECK(cmp("idProp|33|edit = { a = 1; }", "idLight|1|edit = { a = 1; }", detail) == SH_NATIVE_EVAL_UNEQUAL);
    CHECK(strstr(detail, "different classes") != NULL);
    CHECK(cmp("FAIL", "idProp|0|x", detail) == SH_NATIVE_EVAL_UNSUPPORTED);
    CHECK(ctors == dtors && sh_native_decl_eval_available());
    parses = 0;
    CHECK(cmp("", "idProp|0|x", detail) == SH_NATIVE_EVAL_UNSUPPORTED && parses == 0);

    /* Without both parent lookups redirected, private layers are not parsed. */
    redirected = 0; bind_engine();
    parses = 0;
    CHECK(sh_native_decl_eval_compare(&layer, 1, "a|0|x", "a|0|x", detail, sizeof(detail)) == SH_NATIVE_EVAL_UNAVAILABLE);
    CHECK(parses == 0 && strstr(detail, "not redirected") != NULL);
    redirected = 1; bind_engine();
    private_chains();

    /* A parser that is not idle, or an owner whose slots differ, refuses before parsing. */
    parses = 0;
    *(int32_t *)(owner + 0x2c34) = 0;
    CHECK(cmp("a|0|x", "a|0|x", detail) == SH_NATIVE_EVAL_UNAVAILABLE && parses == 0);
    *(int32_t *)(owner + 0x2c34) = 15;
    set_state_body[6] = 0x90;
    CHECK(cmp("a|0|x", "a|0|x", detail) == SH_NATIVE_EVAL_UNAVAILABLE && parses == 0);
    set_state_body[6] = 0xc3;

    /* Off the main thread nothing runs. */
    main_thread = GetCurrentThreadId() + 1;
    CHECK(cmp("a|0|x", "a|0|x", detail) == SH_NATIVE_EVAL_UNAVAILABLE && parses == 0);
    main_thread = GetCurrentThreadId();
    CHECK(ctors == dtors);

    /* A parser that leaves its state changed is a fault and disables
     * evaluation, after the engine's setter restored the idle state. */
    leave_busy = 1;
    CHECK(cmp("a|0|x", "a|0|x", detail) == SH_NATIVE_EVAL_FAILED);
    leave_busy = 0;
    CHECK(*(int32_t *)(owner + 0x2c34) == 15 && ctors == dtors);
    CHECK(cmp("a|0|x", "a|0|x", detail) == SH_NATIVE_EVAL_UNAVAILABLE);
    bind_engine();
    /* So is a refusal that leaves it changed. */
    leave_busy = 1;
    CHECK(cmp("FAIL", "a|0|x", detail) == SH_NATIVE_EVAL_FAILED);
    leave_busy = 0;
    CHECK(*(int32_t *)(owner + 0x2c34) == 15 && !sh_native_decl_eval_available() && ctors == dtors);
    bind_engine();
    CHECK(cmp("SOURCE", "a|0|x", detail) == SH_NATIVE_EVAL_FAILED);
    CHECK(!sh_native_decl_eval_available());
    bind_engine();
    CHECK(cmp("a|0|x", "a|0|x", detail) == SH_NATIVE_EVAL_EQUAL);

    redirect_install();

    free(owner);
    printf("%s native_decl_eval_test (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
