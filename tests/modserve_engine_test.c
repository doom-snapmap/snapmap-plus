/* modserve_engine_test.c -- the engine side of mod serving (modserve_engine.c)
 * against a faked engine table: the decl registry, DeclFind, the generic load
 * with its nesting detour, the heap stack and the resource objects are plain
 * memory the test owns, so the side's decisions about resource levels and the
 * kept set are checked without the game. The index (modserve.c) is real and
 * empty: these checks drive the engine operations directly. */
#include "../src/backend/modserve_engine.c"

static int g_failed;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

/* ------------------------------------------------------------------ log */

static char g_log[65536];
static size_t g_log_len;
void backend_log(const char *message)
{
    size_t n = message ? strlen(message) : 0;
    if (g_log_len + n + 2 < sizeof g_log) {
        memcpy(g_log + g_log_len, message, n);
        g_log_len += n;
        g_log[g_log_len++] = '\n';
        g_log[g_log_len] = 0;
    }
}
static void log_clear(void) { g_log_len = 0; g_log[0] = 0; }
static int log_has(const char *needle) { return strstr(g_log, needle) != NULL; }

/* ------------------------------------------------------------------ link stand-ins */

void sh_modhud_scene_play_begin(void) {}
void sh_modscene_place_play_begin(void) {}
int sh_modstock_exists(const char *request) { (void)request; return 0; }
unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *out_len) { (void)name; if (out_len) *out_len = 0; return NULL; }
void *sh_prepare_detour_sig(const sig_result *sig, void *detour, size_t stolen) { (void)sig; (void)detour; (void)stolen; return NULL; }
sh_patch_status sh_commit_detour(void *tramp) { (void)tramp; return (sh_patch_status)0; }
int sh_uninstall_detour(void *tramp) { (void)tramp; return 0; }
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{ (void)module_base; (void)name; (void)out_status; return 0; }
const char *cmd_argv(idCmdArgs *a, int n) { (void)a; (void)n; return ""; }
static char g_console[16384];
void sh_printf(const char *fmt, ...)
{
    size_t at = strlen(g_console);
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g_console + at, sizeof g_console - at, _TRUNCATE, fmt, ap);
    va_end(ap);
}
void *sh_resolve_cmdsys(const sig_result *results, size_t n, const uint8_t *module_base)
{ (void)results; (void)n; (void)module_base; return NULL; }
int sh_timeline_tile_rebuild_palette(int *before, int *after) { (void)before; (void)after; return 0; }
void *sh_timeline_tile_palette(void) { return NULL; }
void sh_timeline_tile_sorts(int *sorts, int *unscoped, int *guarded) { (void)sorts; (void)unscoped; (void)guarded; }
const char *sh_apply_engine_block_heap_name(const void *p) { (void)p; return ""; }
void sh_apply_engine_get_run_on_main(sh_run_on_main_fn *run_on_main) { if (run_on_main) *run_on_main = NULL; }
void sh_modvt_before_pass(const char *phase) { (void)phase; }
void sh_modvt_print_status(void) {}
void sh_modsound_before_pass(const char *phase) { (void)phase; }
void sh_modsound_print_status(void) {}
int sh_modsound_stock_before_map(const char *const *hashes, int n) { (void)hashes; (void)n; return 0; }
void sh_strids_request_refresh(void) {}
int sh_strids_scope_check(void) { return 0; }
int sh_strids_refresh_pending(void) { return 0; }
void sh_strids_set_process_heap(const struct sh_process_heap_api *api) { (void)api; }
int sh_editor_delete_native_pending(void) { return 0; }
int sh_modtypeinfo_install(const struct sig_result *results, size_t count, const uint8_t *module_base)
{ (void)results; (void)count; (void)module_base; return 0; }
void sh_modtypeinfo_after_palette(void *palette) { (void)palette; }
static int g_typeinfo_dropped;
void sh_modtypeinfo_decl_changed(const char *type, const char *name) { (void)type; (void)name; g_typeinfo_dropped++; }
int sh_companions_ensure(const char *phase) { (void)phase; return 0; }
void sh_companions_refresh(void) {}
void sh_script_hooks_gate(int gate) { (void)gate; }
int sh_script_hooks_open(void) { return 0; }

/* ------------------------------------------------------------------ the faked engine */

/* An idResource: name at +0x08, owning list at +0x10, lifetime at +0x28,
 * state at +0x2c. */
typedef struct fe_res {
    unsigned char pad0[8];
    const char *name;
    void *list;
    unsigned char pad1[0x10];
    unsigned lifetime;
    unsigned char state;
    unsigned char pad2[3];
    struct fe_res *child;         /* the test's: what this resource's load loads */
    int loads;                    /* the test's: generic loads run */
    unsigned char pad3[0x1c8];
} fe_res;

#define FE_MAX 16
static fe_res g_res[FE_MAX];
static int g_nres;
static unsigned char g_manager[64];        /* one manager answers every type */
static void *g_vtable[16];
static void *g_registry_obj[1];
static unsigned char g_anchor_bytes[0x40];
static uint32_t g_main_id;
static unsigned char g_memlocal[0x100];    /* idMemLocal: heap ids at +0x44, depth at +0xc4 */

static void *fe_type(void *registry, const char *type) { (void)registry; (void)type; return g_manager; }
static unsigned char fe_register(void *registry, const void *path, void *x) { (void)registry; (void)path; (void)x; return 1; }

static fe_res *fe_find(const char *name)
{
    int i;
    for (i = 0; i < g_nres; ++i) if (!strcmp(g_res[i].name, name)) return &g_res[i];
    return NULL;
}

static fe_res *fe_add(const char *name, unsigned lifetime, unsigned char state)
{
    fe_res *r = &g_res[g_nres++];
    memset(r, 0, sizeof *r);
    r->name = name;
    r->list = g_manager;
    r->lifetime = lifetime;
    r->state = state;
    return r;
}

static void *fe_decl(void *manager, const char *name, unsigned char create)
{
    fe_res *r = fe_find(name);
    (void)manager;
    if (!r && create) r = fe_add(name, 1, 2);
    return r;
}

/* The heap stack, as idMemLocal keeps it. */
static void *fe_heap_get(void) { return g_memlocal; }
static void fe_heap_push(void *self, int id)
{
    int depth = *(int *)((unsigned char *)self + 0xc4);
    *(int *)((unsigned char *)self + 0x44 + depth * 4) = id;
    *(int *)((unsigned char *)self + 0xc4) = depth + 1;
}
static void fe_heap_pop(void *self) { (*(int *)((unsigned char *)self + 0xc4))--; }

/* The load body (the trampoline): the engine's loader, which loads what the
 * resource uses through the detoured entry, as ResourceMaterialize does. */
static void fe_load_body(void *res)
{
    fe_res *r = (fe_res *)res;
    r->loads++;
    r->state = 0;
    if (r->child) g_generic_load(r->child);
}

/* The detoured entry: every load goes through the nesting hook. */
static void fe_load_entry(void *res) { me_load_hook(res); }

static void fe_reset(void)
{
    int32_t disp;
    memset(g_res, 0, sizeof g_res);
    g_nres = 0;
    memset(g_memlocal, 0, sizeof g_memlocal);
    sh_modserve_attrib_reset();
    (void)sh_modserve_left_to_map(NULL, 0);
    g_vtable[7] = (void *)fe_register;
    g_vtable[11] = (void *)fe_type;
    g_registry_obj[0] = g_vtable;
    /* DeclRegistryAnchor: MOV RCX,[rip+disp] at +0x10 naming the registry pointer. */
    memset(g_anchor_bytes, 0, sizeof g_anchor_bytes);
    memcpy(g_anchor_bytes + 0x10, "\x48\x8b\x0d", 3);
    {
        static void *registry_slot;
        registry_slot = g_registry_obj;
        disp = (int32_t)((intptr_t)&registry_slot - (intptr_t)(g_anchor_bytes + 0x17));
    }
    memcpy(g_anchor_bytes + 0x13, &disp, 4);
    g_anchor = g_anchor_bytes;
    g_register = (register_fn)fe_register;
    g_type = (type_fn)fe_type;
    g_decl = (find_decl_fn)fe_decl;
    g_generic_load = (generic_load_fn)fe_load_entry;
    g_load_orig = (generic_load_fn)fe_load_body;
    g_main_id = GetCurrentThreadId();
    g_main_thread_at = (const uint8_t *)&g_main_id;
    g_heap.get = fe_heap_get;
    g_heap.push = fe_heap_push;
    g_heap.pop = fe_heap_pop;
    g_last_error = NULL;
    g_bound = 1;
    log_clear();
}

/* ------------------------------------------------------------------ the keepability rule */

/* A map pass's step (persist 0) returns to the engine's level, and neither it
 * nor what its load loads joins the kept set (a mod without editor rows is
 * not made permanent at the next Play entry). A kept step's decl and its
 * load's children are kept. */
static void test_op_load_levels(void)
{
    fe_res *def, *model, *row, *rowmodel;
    unsigned state, lifetime;
    sh_modserve_attrib a;
    fe_reset();
    def = fe_add("mods/abcdefgh/imp_blue", 1, 2);
    model = fe_add("md6/mods/abcdefgh/imp_blue.md6", 1, 2);
    def->child = model;
    fe_heap_push(g_memlocal, 0);
    CHECK(op_load(NULL, "entityDef", def->name, 0, &state, &lifetime) == 1);
    CHECK(def->loads == 1 && model->loads == 1);
    CHECK(def->lifetime == 1 && lifetime == 1 && model->lifetime == 1);
    CHECK(!sh_modserve_kept_has(def) && !sh_modserve_kept_has(model));
    /* The next close (the Play entry's) raises nothing of it. */
    CHECK(sh_modserve_attrib_close(NULL, 0, &a) == 0 || a.raised == 0);
    CHECK(def->lifetime == 1 && model->lifetime == 1);

    row = fe_add("mods/abcdefgh/imp_blue_row", 1, 2);
    rowmodel = fe_add("md6/mods/abcdefgh/imp_blue_row.md6", 1, 2);
    row->child = rowmodel;
    CHECK(op_load(NULL, "entityDef", row->name, 1, &state, &lifetime) == 1);
    CHECK(row->lifetime == 4 && lifetime == 4 && sh_modserve_kept_has(row));
    CHECK(rowmodel->lifetime == 4 && sh_modserve_kept_has(rowmodel));
    fe_heap_pop(g_memlocal);
}

/* An editor row's animWeb parsed by the in-editor reload after a Play exit
 * (a map heap selected) stays at level 1 when the keeper reaches it, with one
 * line; one parsed in process heap 0 is kept. */
static void test_keep_ref_map_heap(void)
{
    fe_res *web, *mat;
    me_keep k;
    int depth, kept;
    fe_reset();
    web = fe_add("mods/abcdefgh/x_editor", 1, 0);
    mat = fe_add("mods/abcdefgh/x_material", 1, 0);
    depth = sh_modserve_attrib_enter(web, 3, &kept);   /* its load ran in a map heap */
    sh_modserve_attrib_leave(depth);
    depth = sh_modserve_attrib_enter(mat, 0, &kept);   /* this one in process heap 0 */
    sh_modserve_attrib_leave(depth);
    memset(&k, 0, sizeof k);
    me_keep_ref(&k, "animWeb", web->name, "row_x");
    CHECK(web->lifetime == 1 && !sh_modserve_kept_has(web) && k.left == 1 && k.kept == 0);
    CHECK(log_has("animWeb 'mods/abcdefgh/x_editor' left at level 1") && log_has("map's heap"));
    me_keep_ref(&k, "material", mat->name, "row_x");
    CHECK(mat->lifetime == 4 && sh_modserve_kept_has(mat) && k.kept == 1);
    /* Loaded again in process heap 0, it is kept at the next keep. */
    depth = sh_modserve_attrib_enter(web, 0, &kept);
    sh_modserve_attrib_leave(depth);
    me_keep_ref(&k, "animWeb", web->name, "row_x");
    CHECK(web->lifetime == 4 && k.kept == 2);
}

/* A held render model loaded in process heap 0 is kept through the rule. */
static void test_held_model_kept(void)
{
    fe_res *m;
    me_held_load r;
    fe_reset();
    m = fe_add("models/mods/abcdefgh/rock.lwo", 1, 4);
    memset(&r, 0, sizeof r);
    fe_heap_push(g_memlocal, 0);
    CHECK(me_held_one(&r, m->name, m) == 1);
    fe_heap_pop(g_memlocal);
    CHECK(m->loads == 1 && m->lifetime == 4 && sh_modserve_kept_has(m) && r.reloaded == 1 && !r.failed);
}

/* Every re-parse of a decl drops its default type-info tree, whichever path
 * asks for it (a refresh, a map that serves its own copy of the salt, the
 * early shadows), so the tree follows the bytes the open map serves. */
static void test_reparse_drops_type_info(void)
{
    fe_res *def;
    me_reparse r;
    fe_reset();
    def = fe_add("mods/abcdefgh/imp_blue", 1, 0);
    memset(&r, 0, sizeof r);
    g_typeinfo_dropped = 0;
    fe_heap_push(g_memlocal, 0);
    CHECK(me_reparse_one(&r, "entityDef", def->name, "generated/decls/entitydef/mods/abcdefgh/imp_blue.decl",
                         "abcdefgh") == 1);
    fe_heap_pop(g_memlocal);
    CHECK(r.reparsed == 1 && def->loads == 1 && g_typeinfo_dropped == 1);
}

int main(void)
{
    test_op_load_levels();
    test_reparse_drops_type_info();
    test_keep_ref_map_heap();
    test_held_model_kept();
    if (g_failed) {
        fprintf(stderr, "modserve_engine_test: %d check(s) failed\n--- log ---\n%s", g_failed, g_log);
        return 1;
    }
    printf("modserve_engine_test: all checks passed\n");
    return 0;
}
