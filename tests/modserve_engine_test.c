/* modserve_engine_test.c -- checks modserve_engine.c against a faked engine table (decl registry, DeclFind,
 * generic load, heap stack and resources are plain memory), so resource levels and the kept set need no game. */
#include "../src/backend/modserve.h"

static int fe_marks, fe_named_passes, fe_shared_passes, fe_attrib_passes;
static int fe_watermark_take(const void *head, sh_modserve_watermark *w)
{
    fe_marks++;
    return sh_modserve_watermark_take(head, w);
}
static int fe_holds_raise(const void *head, sh_modserve_holds *out)
{
    fe_named_passes++;
    return sh_modserve_holds_raise(head, out);
}
static int fe_attrib_close(const void *head, int forget, sh_modserve_attrib *out)
{
    fe_attrib_passes++;
    return sh_modserve_attrib_close(head, forget, out);
}
static int fe_maintain_models(const void *head, sh_modserve_held_model_fn fn, void *context,
                              sh_modserve_holds *out, int *named)
{
    int n = sh_modserve_maintain_models(head, fn, context, out, named);
    if (*named) ++fe_shared_passes;
    return n;
}
#define sh_modserve_watermark_take fe_watermark_take
#define sh_modserve_holds_raise fe_holds_raise
#define sh_modserve_maintain_models fe_maintain_models
#define sh_modserve_attrib_close fe_attrib_close
#include "../src/backend/modserve_engine.c"
#undef sh_modserve_watermark_take
#undef sh_modserve_holds_raise
#undef sh_modserve_maintain_models
#undef sh_modserve_attrib_close
#include "mod_test_util.h"

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

void sh_script_game_cvar_recover_poll(void) {}
static int g_smodel_begins, g_smodel_reloads;
void sh_static_model_reload_begin(void) { g_smodel_begins++; }
int sh_static_model_reloaded(const void *backing) { (void)backing; g_smodel_reloads++; return 1; }
void sh_direct_iter_reset(void) {}
void sh_modhud_scene_play_begin(void) {}
void sh_modscene_place_play_begin(void) {}
int sh_modstock_exists(const char *request) { (void)request; return 0; }
unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *out_len) { (void)name; if (out_len) *out_len = 0; return NULL; }
static const char *fe_pending_default;
int sh_overrides_repair_defaults(int (*visit)(void *, const char *), void *context, int *reads)
{
    if (!fe_pending_default) return 0;
    if (visit(context, fe_pending_default)) {
        fe_pending_default = NULL;
        (*reads)++;
    }
    return 1;
}
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
static unsigned char fe_palette[32], fe_palette_rows[2 * PAL_STRIDE];
static unsigned char fe_new_sedef[SEDEF_NAME_OFF + sizeof(void *)];
static unsigned char fe_record[0x58 + sizeof(void *)];
static int fe_has_palette, fe_rebuilds;

int sh_timeline_actor_filter_ready(void) { return fe_has_palette; }
int sh_timeline_tile_rebuild_palette(int *before, int *after)
{
    if (!fe_has_palette) return 0;
    *before = *(int *)(fe_palette + PAL_COUNT_OFF);
    *(void **)(fe_palette_rows + PAL_STRIDE) = fe_new_sedef;
    *(void **)(fe_palette_rows + PAL_STRIDE + 0x150) = fe_record;
    *after = *(int *)(fe_palette + PAL_COUNT_OFF) = 2;
    fe_rebuilds++;
    return SH_TL_REBUILD_OK;
}
void *sh_timeline_tile_palette(void) { return fe_has_palette ? fe_palette : NULL; }
void sh_timeline_tile_sorts(int *sorts, int *unscoped, int *guarded) { (void)sorts; (void)unscoped; (void)guarded; }
const char *sh_apply_engine_block_heap_name(const void *p) { (void)p; return ""; }
void sh_apply_engine_get_run_on_main(sh_run_on_main_fn *run_on_main) { if (run_on_main) *run_on_main = NULL; }
void sh_modvt_before_pass(const char *phase) { (void)phase; }
void sh_modvt_print_status(void) {}
void sh_modsound_before_pass(const char *phase) { (void)phase; }
static unsigned audio_scope_checks;
int sh_modsound_apply_rows(void) { audio_scope_checks++; return 0; }
void sh_modsound_print_status(void) {}
int sh_modsound_stock_before_map(const char *const *hashes, int n) { (void)hashes; (void)n; return 0; }
void sh_strids_request_refresh(void) {}
int sh_strids_scope_check(void) { return 0; }
int sh_strids_refresh_pending(void) { return 0; }
void sh_strids_set_process_heap(const struct sh_process_heap_api *api) { (void)api; }
void sh_strids_set_language_reader(char *(*read_language)(void)) { (void)read_language; }
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
void sh_clip_events_play_begin(void) {}
void sh_clip_events_map_end(void) {}
void sh_attach_tags_play_begin(void) {}
void sh_attach_tags_map_end(void) {}
void sh_sync_clock_play_begin(void) {}
void sh_sync_clock_map_end(void) {}

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
static fe_res *fe_entries[FE_MAX];
static int g_nres;
static const char *fe_demand_name;
static unsigned char g_manager[64];        /* one manager answers every type */
static void *g_vtable[16];
static void *g_registry_obj[1];
static unsigned char g_anchor_bytes[0x40];
static uint32_t g_main_id;
static unsigned char g_memlocal[0x100];    /* idMemLocal: heap ids at +0x44, depth at +0xc4 */

static void *fe_type(void *registry, const char *type) { (void)registry; (void)type; return g_manager; }
static void *fe_source(void *manager, const char *name) { (void)name; return manager; }
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
    fe_entries[g_nres - 1] = r;
    *(fe_res ***)(g_manager + ME_LIST_ENTRIES) = fe_entries;
    *(int *)(g_manager + ME_LIST_COUNT) = g_nres;
    return r;
}

static void *fe_pure_decl(void *manager, const char *name)
{
    return manager == g_manager ? fe_find(name) : NULL;
}

static void *fe_decl(void *manager, const char *name, unsigned char create)
{
    fe_res *r = fe_find(name);
    (void)manager;
    if (!r && fe_demand_name && !strcmp(name, fe_demand_name)) {
        r = fe_add(name, 1, 2);
        g_generic_load(r);
    }
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
    memset(g_manager, 0, sizeof g_manager);
    fe_demand_name = NULL;
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
    g_pure_decl = fe_pure_decl;
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

/* A map pass's step (persist 0) returns to the engine's level and joins no kept set, nor does what its load
 * loads; a kept step's decl and its load's children are kept. */
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
    fe_res *m, *second;
    void *head = NULL;
    me_held_load r;
    me_closure outer;
    fe_reset();
    g_res_head = &head;
    fe_marks = 0;
    m = fe_add("models/mods/abcdefgh/rock.lwo", 1, 4);
    second = fe_add("models/mods/abcdefgh/second.lwo", 1, 4);
    memset(&r, 0, sizeof r);
    fe_heap_push(g_memlocal, 0);
    CHECK(me_held_one(&r, "missing", NULL) == 1 && r.skipped == 1 && !fe_marks);
    CHECK(me_held_one(&r, m->name, m) == 1);
    CHECK(m->loads == 1 && m->lifetime == 4 && sh_modserve_kept_has(m) && r.reloaded == 1 && !r.failed);
    CHECK(me_held_one(&r, second->name, second) == 1 && second->loads == 1 && second->lifetime == 4);
    CHECK(fe_marks == 1 && r.keeper.counted && g_mark_depth == 1);
    me_close_watermark(&r.keeper, "test", "held models");
    memset(&r, 0, sizeof r);
    me_closure_begin(&outer);
    CHECK(me_held_one(&r, m->name, m) == 1 && !r.keeper.counted && g_mark_depth == 1 && fe_marks == 2);
    me_close_watermark(&outer, "test", "enclosing pass");
    fe_heap_pop(g_memlocal);
    CHECK(g_mark_depth == 0);
    g_res_head = NULL;
}

static void test_placement_lifetimes(void)
{
    struct fe_list {
        void *unused;
        const char *type, *cls;
        struct fe_list *next;
        fe_res **entries;
        int count;
    };
    char root[MAX_PATH], mod[MAX_PATH], *why = NULL;
    sh_mb_bundle bundle;
    const char *hash;
    fe_res *row, *entity, *anchor;
    fe_res *row_entries[1], *other_entries[4];
    struct fe_list entities = { NULL, "entityDef", "idDeclEntityDef", NULL, other_entries, 2 };
    struct fe_list rows = { NULL, "snapEditorEntityDef", "idDeclSnapEditorEntityDef", &entities, row_entries, 1 };
    struct fe_list *head = &rows;
    fe_reset();
    CHECK(mt_temp_dir("keepers", root, sizeof root));
    snprintf(mod, sizeof mod, "%s\\pack", root);
    mt_write(mod, "mod.json", "{\"name\":\"Test\",\"snapmap-plus\":{\"format\":2,\"salt\":\"abcdefgh\"}}");
    mt_write(mod, "entities\\placed.decl", "{ edit = { } }");
    mt_write(mod, "decls\\snapeditorentitydef\\placed.decl", "{ edit = { entityDef = \"entities/placed\"; } }");
    CHECK(sh_mb_build(mod, NULL, NULL, &bundle));
    CHECK(sh_mb_write(&bundle, mod, &why));
    CHECK(sh_modserve_index_build(root) == 1);
    hash = bundle.hash;
    sh_modserve_scope_enable(1);
    sh_modserve_scope_set(&hash, 1, 1);
    g_res_head = &head;
    row_entries[0] = row = fe_add("mods/abcdefgh/placed", 1, 0);
    other_entries[0] = entity = fe_add("mods/abcdefgh/placed", 1, 0);
    other_entries[1] = anchor = fe_add("anchor", 4, 0);

    fe_marks = fe_named_passes = fe_shared_passes = fe_attrib_passes = 0;
    me_keep_resources("test", 0);
    CHECK(fe_marks == 0 && fe_shared_passes == 1 && fe_named_passes == 0 && fe_attrib_passes == 1);
    CHECK(row->lifetime == 1 && entity->lifetime == 1);

    /* An attributed child becomes a new root for a named dependency. */
    sh_modserve_kept_add(anchor);
    CHECK(sh_modserve_attrib_note(anchor, row) == 1);
    fe_named_passes = fe_shared_passes = fe_attrib_passes = 0;
    me_keep_resources("test", 0);
    CHECK(fe_marks == 0 && fe_shared_passes == 1 && fe_named_passes == 1 && fe_attrib_passes == 2);
    CHECK(row->lifetime == 4 && entity->lifetime == 4);

    other_entries[2] = fe_add("map_parent", 1, 0);
    other_entries[3] = fe_add("map_child", 1, 0);
    entities.count = 4;
    CHECK(!sh_modserve_attrib_note(other_entries[2], other_entries[3]));
    me_keep_resources("play", 1);
    sh_modserve_kept_add(other_entries[2]);
    me_keep_resources("after purge", 0);
    CHECK(other_entries[3]->lifetime == 1);
    g_res_head = NULL;
    sh_modserve_scope_set(NULL, 0, 0);
    sh_modserve_scope_enable(0);
    sh_mb_free(&bundle);
    free(why);
    mt_remove_tree(root);
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

static void test_refresh_looks_up_only_held_decls(void)
{
    const char *request = "generated/decls/entitydef/props/test.decl";
    for (int native = 0; native < 2; ++native) {
        me_refresh c = {0};
        sh_modserve_refresh_item change = {"entityDef", "props/test", request, "pack", SH_MODSERVE_RF_DECL, 0, 1, 1};
        fe_reset();
        if (!native) g_pure_decl = NULL;
        fe_demand_name = change.name;
        fe_heap_push(g_memlocal, 0);
        CHECK(me_reparse_one(&c.decls, change.type, change.name, request, "pack") == 0);
        CHECK(me_refresh_visit(&c, &change) == 1 && c.not_held == 1 && g_nres == 0);
        fe_res *obj = g_decl(g_manager, change.name, 0);
        CHECK(obj && g_nres == 1 && obj->loads == 1);
        obj->state = 2;
        CHECK(me_reparse_one(&c.decls, change.type, change.name, request, "pack") == 0);
        CHECK(c.decls.pending == 1 && obj->loads == 1 && obj->state == 2);
        obj->state = 0;
        CHECK(me_refresh_visit(&c, &change) == 1 && obj->loads == 2);
        change.gone = 1;
        CHECK(me_refresh_visit(&c, &change) == 1 && obj->loads == 3 && obj->lifetime == 1);
        fe_heap_pop(g_memlocal);
    }
}

static void test_palette_repairs_early_default_without_mods(void)
{
    fe_res *unknown;
    fe_reset();
    CHECK(!sh_modserve_active());
    unknown = fe_add("unknown/unknown", 4, 0);
    fe_pending_default = "generated/decls/snapeditorentitydef/unknown/unknown.decl";
    sh_modserve_palette_before_build();
    CHECK(unknown->loads == 1 && unknown->lifetime == 4 && !fe_pending_default);
    sh_modserve_palette_before_build();
    CHECK(unknown->loads == 1);
}

/* A refresh item of type "model" (a .bmodel) is a static model; a walk holding one begins a static-model reload
 * pass, and one without does not. */
static void test_static_model_kind(void)
{
    me_raw r;
    me_raw_name n[2];
    memset(&r, 0, sizeof r);
    memset(n, 0, sizeof n);
    CHECK(me_raw_kind("model") == ME_RAW_SMODEL && me_raw_kind("baseModel") == ME_RAW_MODEL &&
          me_raw_kind("image") == ME_RAW_IMAGE && me_raw_kind("anim") == ME_RAW_ANIM);
    r.names = n;
    r.cap = 2;
    (void)me_raw_collect(&r, "baseModel", "md6/x.md6mesh", "generated/basemodel/x.bmd6model", "b");
    g_smodel_begins = 0;
    me_raw_smodel_begin(&r);
    CHECK(g_smodel_begins == 0);
    (void)me_raw_collect(&r, "model", "cooked/model/models/x.bmodel", "cooked/model/models/x.bmodel", "b");
    me_raw_smodel_begin(&r);
    CHECK(g_smodel_begins == 1 && r.names[1].kind == ME_RAW_SMODEL);
}

/* The list of models, images and anims a pass reloads has no fixed cap. */
static void test_raw_names_grow(void)
{
    me_raw r;
    int i;
    memset(&r, 0, sizeof r);
    for (i = 0; i < 5000; i++) (void)me_raw_collect(&r, "image", "x", "generated/image/x.bimage", "b");
    CHECK(r.n == 5000 && r.cap >= 5000 && r.dropped == 0 && r.names && r.names[4999].kind == ME_RAW_IMAGE);
    free(r.names);
}

static void test_prefab_adds_a_row_to_the_open_palette(void)
{
    char root[MAX_PATH], mod[MAX_PATH], *why = NULL;
    sh_mb_bundle bundle;
    const char *hash;
    fe_reset();
    CHECK(mt_temp_dir("prefabrow", root, sizeof root));
    g_source = fe_source;
    snprintf(mod, sizeof mod, "%s\\pack", root);
    mt_write(mod, "mod.json", "{\"name\":\"Test\",\"snapmap-plus\":{\"format\":2,\"salt\":\"abcdefgh\"}}");
    mt_write(mod, "entities\\placed.decl", "{ edit = { } }");
    mt_write(mod, "decls\\snapeditorentitydef\\placed.decl", "{ edit = { entityDef = \"entities/placed\"; } }");
    CHECK(sh_mb_build(mod, NULL, NULL, &bundle));
    CHECK(bundle.file_count == 2);
    CHECK(sh_mb_write(&bundle, mod, &why));
    CHECK(sh_modserve_index_build(root) == 1);
    sh_modserve_scope_enable(1);
    hash = bundle.hash;
    sh_modserve_scope_set(&hash, 1, 1);
    *(void **)(fe_palette + PAL_ENTRIES_OFF) = fe_palette_rows;
    *(int *)(fe_palette + PAL_COUNT_OFF) = 1;
    *(const char **)(fe_new_sedef + SEDEF_NAME_OFF) = "mods/abcdefgh/placed";
    fe_has_palette = 1;
    CHECK(!pal_has(NULL, "mods/abcdefgh/placed"));
    CHECK(sh_modserve_scope_activate("prefab") >= 0);
    CHECK(fe_rebuilds == 1 && pal_has(NULL, "mods/abcdefgh/placed"));
    CHECK(sh_modserve_scope_activate("prefab") >= 0 && fe_rebuilds == 1);
    sh_modserve_scope_set(NULL, 0, 1);
    *(int *)(fe_palette + PAL_COUNT_OFF) = 1;
    const char *inherit = "mods/abcdefgh/placedEntity", *missing = NULL;
    *(const char **)(fe_record + 0x58) = inherit;
    CHECK(sh_modserve_prefab_rows(&inherit, 1, &missing) && !missing && fe_rebuilds == 2);
    CHECK(sh_modserve_prefab_rows(&inherit, 1, &missing) && fe_rebuilds == 2);
    inherit = "mods/abcdefgh/absent";
    CHECK(!sh_modserve_prefab_rows(&inherit, 1, &missing) && missing == inherit);

    CHECK(!strstr(g_log, "FAULTED") && !strstr(g_log, "NOT READY"));
    fe_has_palette = 0;
    sh_modserve_scope_set(NULL, 0, 0);
    sh_modserve_scope_enable(0);
    sh_mb_free(&bundle);
    free(why);
    mt_remove_tree(root);
}

int main(void)
{
    fe_reset();
    (void)sh_modserve_scope_apply("map exit");
    CHECK(audio_scope_checks == 1);
    test_raw_names_grow();
    test_op_load_levels();
    test_reparse_drops_type_info();
    test_refresh_looks_up_only_held_decls();
    test_palette_repairs_early_default_without_mods();
    test_keep_ref_map_heap();
    test_held_model_kept();
    test_placement_lifetimes();
    test_static_model_kind();
    test_prefab_adds_a_row_to_the_open_palette();
    if (g_failed) {
        fprintf(stderr, "modserve_engine_test: %d check(s) failed\n--- log ---\n%s", g_failed, g_log);
        return 1;
    }
    printf("modserve_engine_test: all checks passed\n");
    return 0;
}
