/* Native adapter lifetimes with exact authored bundle fixtures. */
#define main occurrence_fixture_main
#include "package_occurrence_test.c"
#undef main
#define SH_PACKAGE_EDITOR_TESTING
#include "../src/backend/package_editor.c"

static int prepared, committed, removed, fail_prepare, fail_commit;
void backend_log(const char *text) { (void)text; }
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)detour; CHECK(stolen >= 14 && stolen <= 48); return ++prepared == fail_prepare ? NULL : target; }
void *hook_prepare_rip_lea(void *target, void *detour, size_t stolen, size_t lea)
{ CHECK(stolen == 17 && lea == 10); return hook_prepare(target, detour, stolen); }
sh_patch_status hook_commit(void *trampoline)
{ CHECK(trampoline); return ++committed == fail_commit ? B2_PATCH_FAIL_PROTECT : B2_PATCH_OK; }
int hook_unpatch(void *trampoline) { CHECK(trampoline); removed++; return 1; }

static void *copy_body(void *destination, void *source) { (void)source; return destination; }
static void destroy(void *object) { (void)object; }
static void clone_body(void *body, void *flat, unsigned char pinned)
{ (void)body; (void)flat; (void)pinned; }
static void from_flat(void *palette, void *variables, void *flat, void *body)
{ (void)palette; (void)variables; (void)flat; (void)body; }
static int inspect_calls;
static int inspect_empty(void *context, unsigned kind, void *flat,
    const sh_package_occurrence_snapshot *previous, sh_package_occurrence_snapshot **next,
    char *error, size_t capacity)
{
    (void)context; (void)kind; (void)flat; (void)previous; inspect_calls++;
    *next = sh_package_occurrence_prepare_document(NULL, NULL, 0, error, capacity);
    return *next != NULL;
}
static void native_transfers(void)
{
    unsigned char map[64] = {0}, flat[2 * 0x1b0] = {0}, copied_flat[0x1b0] = {0};
    unsigned char a[16], b[16], clipboard[16];
    char error[512]; sh_package_provenance uses = {0};
    g_tracker = sh_package_occurrence_tracker_create(); CHECK(g_tracker);
    g_copy_ctor = g_copy_assign = g_move_assign = g_flat_assign = copy_body;
    g_body_dtor = g_flat_dtor = destroy; g_clone = clone_body; g_from = from_flat;
    sh_package_editor_bind(inspect_empty, NULL); g_ready = 1;
    *(void **)(map + 0x20) = flat; *(int *)(map + 0x28) = 2;
    make_document(&uses);
    sh_package_occurrence_snapshot *document = sh_package_occurrence_prepare_document(&uses, sources, 4, error, sizeof(error));
    expect(document, 4, 3, 3);
    CHECK(!sh_package_occurrence_prepare_document(&uses, sources, 1, error, sizeof(error)));
    CHECK(sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, document, error, sizeof(error)));
    pe_from(NULL, NULL, flat, a); pe_from(NULL, NULL, flat + 0x1b0, b);
    CHECK(!inspect_calls);
    CHECK(pe_copy_ctor(clipboard, a) == clipboard);
    pe_body_dtor(a); pe_flat_dtor(flat); pe_flat_dtor(flat + 0x1b0);
    sh_package_editor_clear_world();
    pe_clone(clipboard, copied_flat, 0);
    sh_package_occurrence_snapshot *exported = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_ENTITY, copied_flat, error, sizeof(error));
    expect(exported, 2, 1, 1); expect_tree(exported, 0, 0);
    sh_package_occurrence_release(exported);
    pe_copy_assign(clipboard, b); pe_clone(clipboard, copied_flat, 0);
    exported = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_ENTITY, copied_flat, error, sizeof(error));
    expect(exported, 1, 1, 1); expect_tree(exported, 0, 1); sh_package_occurrence_release(exported);
    /* List reallocation copies flat records, then destroys old storage. */
    pe_flat_assign(flat, copied_flat); pe_flat_dtor(copied_flat);
    pe_flat_assign(copied_flat, flat); pe_flat_assign(copied_flat, copied_flat); pe_flat_dtor(flat);
    exported = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_ENTITY, copied_flat, error, sizeof(error));
    expect(exported, 1, 1, 1); expect_tree(exported, 0, 1); sh_package_occurrence_release(exported);
    CHECK(pe_move_assign(a, clipboard) == a);
    exported = sh_package_occurrence_get(g_tracker, pe_key(PE_BODY, clipboard)); CHECK(!exported);
    pe_clone(a, copied_flat, 0); pe_body_dtor(a); pe_body_dtor(b);
    /* Flat data owns the archive even after both native source bodies die. */
    exported = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_ENTITY, copied_flat, error, sizeof(error));
    expect(exported, 1, 1, 1); expect_tree(exported, 0, 1); sh_package_occurrence_release(exported);
    pe_flat_dtor(copied_flat); pe_from(NULL, NULL, copied_flat, a); CHECK(inspect_calls == 1);
    pe_clone(a, copied_flat, 0);
    exported = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_ENTITY, copied_flat, error, sizeof(error));
    expect(exported, 0, 0, 0); sh_package_occurrence_release(exported);
    pe_log_failure("test allocation failure"); CHECK(!sh_package_editor_healthy(error, sizeof(error)));
    CHECK(!sh_package_editor_snapshot(SH_PACKAGE_EDITOR_ENTITY, copied_flat, error, sizeof(error)));
    sh_package_occurrence_release(document); sh_package_provenance_free(&uses);
    CHECK(sh_package_editor_uninstall()); CHECK(sh_package_editor_healthy(error, sizeof(error)));
}
static void atomic_hook_install(void)
{
    enum { count = sizeof(g_hooks) / sizeof(*g_hooks) };
    unsigned char image[count * 4096] = {0}; sig_result results[count];
    const unsigned char entry[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20};
    for (size_t i = 0; i < count; i++) {
        size_t offset = i * 4096 + g_hooks[i].anchor;
        memcpy(image + i * 4096, entry, sizeof(entry));
        results[i] = (sig_result){g_hooks[i].name, SIG_OK, (uintptr_t)image + offset, (uint32_t)offset};
    }
    prepared = committed = removed = 0; fail_prepare = 3;
    CHECK(!sh_package_editor_install(results, count, image)); CHECK(committed == 0 && removed == 2 && !g_tracker);
    prepared = committed = removed = 0; fail_prepare = 0; fail_commit = 4;
    CHECK(!sh_package_editor_install(results, count, image)); CHECK(committed == 4 && removed == count && !g_tracker);
    prepared = committed = removed = 0; fail_commit = 0;
    CHECK(sh_package_editor_install(results, count, image)); CHECK(prepared == count && committed == count);
    CHECK(sh_package_editor_uninstall()); CHECK(removed == count);
    results[2].status = SIG_OK_HOOKED; prepared = 0;
    CHECK(!sh_package_editor_install(results, count, image)); CHECK(prepared == 0);
}
/* Exact editor rows over equal-entityDef ties; only engine code is faked. The
 * engine lookup returns the first equal key in the order the last re-sort left. */
typedef struct row_entry { const void *decl; const char *key; unsigned char layout[0x40]; } row_entry;
enum { ROW_DECLS = 96, ROW_UNKNOWN = 95 };
static row_entry rows_palette[ROW_DECLS]; static size_t rows_count;
static char row_decls[ROW_DECLS];
static const void *rows_resolved; static const char *rows_inherit; static int rows_steering = 1;
static const char start_key[] = "player/coop/player_start";
static int rows_view(void *context, void *palette, const unsigned char **entries, size_t *count, size_t *stride)
{ (void)context; (void)palette; *entries = (const unsigned char *)rows_palette; *count = rows_count; *stride = sizeof(row_entry); return 1; }
static const char *rows_key(void *context, const void *entry) { (void)context; return ((const row_entry *)entry)->key; }
static int rows_first_class; /* row_decls[0]: 0 native original, 2 a changed row in its native slot */
static int rows_classify(void *context, const void *decl, char *name, size_t capacity)
{
    ptrdiff_t i = (const char *)decl - row_decls; (void)context;
    if (i < 0 || i >= ROW_UNKNOWN) return -1;
    if (!i && !rows_first_class) return 0;
    if (name) snprintf(name, capacity, "pkg/start_%d", (int)i);
    return i ? 1 : rows_first_class;
}
static const void *rows_resolve(void *context, const char *name) { (void)context; (void)name; return rows_resolved; }
/* A published row's exact publication identity, distinct from its portable name. */
static int rows_exact(void *context, const void *decl, char *name, size_t capacity)
{
    char portable[64];
    if (rows_classify(context, decl, portable, sizeof(portable)) <= 0) return 0;
    snprintf(name, capacity, "exact/%s", portable); return 1;
}
static const void *rows_owner; static size_t rows_supplier_a, rows_supplier_b;
static int rows_supplied(void *context, const void *owner, const void *decl)
{ (void)context; return owner == rows_owner && (decl == row_decls + rows_supplier_a || decl == row_decls + rows_supplier_b); }
static const char *row_name(unsigned kind, const void *object)
{ size_t i = pe_row_find(kind, object); return i == SIZE_MAX ? NULL : g_row_items[i].name; }
static void rows_fill(size_t ties, int variants_first)
{
    memset(rows_palette, 0, sizeof(rows_palette)); rows_count = 0;
    rows_palette[rows_count++] = (row_entry){row_decls + ROW_UNKNOWN - 1, "a/other"};
    if (!variants_first) rows_palette[rows_count++] = (row_entry){row_decls, start_key};
    for (size_t i = 1; i < ties; i++) rows_palette[rows_count++] = (row_entry){row_decls + i, start_key};
    if (variants_first) rows_palette[rows_count++] = (row_entry){row_decls, start_key};
}
static void from_steered(void *palette, void *variables, void *flat, void *body)
{
    const row_entry *engine = NULL; (void)variables; (void)flat;
    for (size_t i = 0; i < rows_count && !engine; i++)
        if (rows_palette[i].key && !strcmp(rows_palette[i].key, rows_inherit)) engine = rows_palette + i;
    if (rows_steering) engine = sh_package_editor_row_steer(palette, rows_inherit, engine, NULL);
    *(const void **)body = engine ? engine->decl : NULL;
}
static const void *rows_build(void *flat, size_t ties, int variants_first)
{
    static uintptr_t body[2];
    rows_fill(ties, variants_first); pe_from(rows_palette, NULL, flat, body);
    const void *built = (const void *)body[0]; pe_body_dtor(body); return built;
}
static void rows_place(void *flat, const void *decl, void *source_body)
{
    uintptr_t body[2] = {(uintptr_t)decl};
    if (source_body) pe_copy_assign(body, source_body);
    body[0] = (uintptr_t)decl; pe_clone(body, flat, 0); pe_body_dtor(body);
}
static void exact_rows(void)
{
    const sh_package_editor_rows_api api = {rows_view, rows_key, rows_classify, rows_resolve, rows_supplied, NULL, NULL, rows_exact};
    static char flats[10][16]; static uintptr_t held[2], moved[2], built[2];
    static unsigned char map[64], owned[2 * 0x1b0];
    char error[512], expected[64]; sh_package_provenance uses = {0};
    if (!g_tracker) g_tracker = sh_package_occurrence_tracker_create();
    CHECK(g_tracker); sh_package_editor_bind(NULL, NULL); sh_package_editor_bind_rows(&api);
    /* An entity occurrence of an attached map is the owner of owned objects. */
    make_document(&uses); *(void **)(map + 0x20) = owned; *(int *)(map + 0x28) = 2;
    sh_package_occurrence_snapshot *document = sh_package_occurrence_prepare_document(&uses, sources, 4, error, sizeof(error)), *owner, *got;
    CHECK(document && sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, document, error, sizeof(error)));
    owner = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, owned)); CHECK(owner); rows_owner = owner;
    g_copy_assign = g_move_assign = copy_body; g_body_dtor = g_flat_dtor = destroy; g_clone = clone_body; g_from = from_steered;
    rows_inherit = start_key;
    for (int order = 0; order < 2; order++) for (size_t ties = 2; ties <= 72; ties += 70) {
        const void *native = row_decls, *variant = row_decls + ties - 1;
        rows_steering = 1;
        CHECK(rows_build(flats[0], ties, order) == native); /* unrecorded tie: the native row */
        rows_place(flats[1], variant, NULL);                /* a variant tile placed, then copied */
        CHECK(!sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, flats[1]))); /* never provenance */
        CHECK(rows_build(flats[1], ties, order) == variant && rows_build(flats[1], ties, !order) == variant);
        rows_fill(ties, !order); pe_from(rows_palette, NULL, flats[1], held); /* paste keeps it */
        CHECK(held[0] == (uintptr_t)variant); pe_clone(held, flats[2], 0);
        rows_place(flats[3], row_decls + ROW_UNKNOWN, held); /* unknown capture retains the choice */
        CHECK(rows_build(flats[3], ties, order) == variant);
        rows_place(flats[4], native, held);                  /* positive native lineage clears it */
        CHECK(rows_build(flats[4], ties, !order) == native);
        pe_move_assign(moved, held);                         /* exact ownership transfer */
        CHECK(pe_row_find(PE_BODY, held) == SIZE_MAX && pe_row_find(PE_BODY, moved) != SIZE_MAX);
        pe_body_dtor(moved); CHECK(pe_row_find(PE_BODY, moved) == SIZE_MAX);
        sh_package_editor_rows_retire();                     /* retired generation: names re-resolve */
        rows_resolved = variant; CHECK(rows_build(flats[2], ties, order) == variant);
        /* Unresolved after a retired generation: the logged fallback is never
         * captured; choice and ownership survive clone, and a later generation
         * that resolves the name restores the authored row. */
        sh_package_editor_rows_retire(); rows_resolved = NULL;
        CHECK(sh_package_occurrence_set(g_tracker, pe_key(PE_FLAT, flats[2]), owner));
        rows_fill(ties, !order); pe_from(rows_palette, NULL, flats[2], built);
        CHECK(built[0] == (uintptr_t)native && pe_row_pending(PE_BODY, built));
        pe_clone(built, flats[6], 0); pe_body_dtor(built);
        snprintf(expected, sizeof(expected), "pkg/start_%d", (int)(ties - 1));
        CHECK(row_name(PE_FLAT, flats[6]) && !strcmp(row_name(PE_FLAT, flats[6]), expected));
        got = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, flats[6])); CHECK(got == owner); sh_package_occurrence_release(got);
        /* Owned: its name alone, even resolved uniquely, is not the choice; the
         * tie its owner's selected supplier supplies is. */
        sh_package_editor_rows_retire(); rows_resolved = variant;
        CHECK(rows_build(flats[6], ties, order) == native && rows_build(flats[6], ties, !order) == native);
        CHECK(row_name(PE_FLAT, flats[6]) && !strcmp(row_name(PE_FLAT, flats[6]), expected));
        rows_supplier_a = ties - 1; rows_resolved = NULL;
        CHECK(rows_build(flats[6], ties, order) == variant && rows_build(flats[6], ties, !order) == variant);
        rows_supplier_a = 0;
        /* Owned without a record (older map): the supplying context's row in
         * either order; never the native default. */
        rows_supplier_a = ties - 1;
        CHECK(sh_package_occurrence_set(g_tracker, pe_key(PE_FLAT, flats[7]), owner));
        CHECK(rows_build(flats[7], ties, order) == variant && rows_build(flats[7], ties, !order) == variant);
        rows_supplier_a = 0; rows_fill(ties, order); pe_from(rows_palette, NULL, flats[7], built);
        CHECK(pe_row_pending(PE_BODY, built)); pe_clone(built, flats[8], 0); pe_body_dtor(built);
        CHECK(pe_row_pending(PE_FLAT, flats[8]) && row_name(PE_FLAT, flats[8]) && !*row_name(PE_FLAT, flats[8]));
        if (ties > 2) { /* several suppliers: unresolved */
            rows_supplier_a = 1; rows_supplier_b = 2; rows_fill(ties, !order); pe_from(rows_palette, NULL, flats[7], built);
            CHECK(pe_row_pending(PE_BODY, built)); pe_body_dtor(built); rows_supplier_b = 0;
        }
        rows_supplier_a = ties - 1; CHECK(rows_build(flats[8], ties, !order) == variant); rows_supplier_a = 0;
        if (ties == 2) { /* owned, no record, zero or one candidate: never authoritative */
            rows_inherit = "missing/row"; rows_fill(1, order); pe_from(rows_palette, NULL, flats[7], built);
            CHECK(!built[0] && pe_row_pending(PE_BODY, built)); pe_body_dtor(built); rows_inherit = start_key;
            rows_fill(1, order); pe_from(rows_palette, NULL, flats[7], built); /* sole native */
            CHECK(built[0] == (uintptr_t)native && pe_row_pending(PE_BODY, built)); pe_clone(built, flats[8], 0); pe_body_dtor(built);
            CHECK(pe_row_pending(PE_FLAT, flats[8]) && row_name(PE_FLAT, flats[8]) && !*row_name(PE_FLAT, flats[8]));
            rows_fill(1, order); pe_from(rows_palette, NULL, flats[0], built); /* control: unowned sole native */
            CHECK(built[0] == (uintptr_t)native && !pe_row_pending(PE_BODY, built) && pe_row_find(PE_BODY, built) == SIZE_MAX);
            pe_body_dtor(built);
            rows_fill(1, order); rows_palette[rows_count-1].decl = row_decls + 5; /* sole non-supplier */
            pe_from(rows_palette, NULL, flats[7], built);
            CHECK(built[0] == (uintptr_t)(row_decls + 5) && pe_row_pending(PE_BODY, built)); pe_body_dtor(built);
            rows_supplier_a = 5; rows_fill(1, order); rows_palette[rows_count-1].decl = row_decls + 5; /* sole supplier */
            pe_from(rows_palette, NULL, flats[7], built);
            CHECK(built[0] == (uintptr_t)(row_decls + 5) && !pe_row_pending(PE_BODY, built));
            CHECK(row_name(PE_BODY, built) && !strcmp(row_name(PE_BODY, built), "pkg/start_5"));
            pe_clone(built, flats[8], 0); pe_body_dtor(built); rows_supplier_a = 0;
            CHECK(row_name(PE_FLAT, flats[8]) && !strcmp(row_name(PE_FLAT, flats[8]), "pkg/start_5") && !pe_row_pending(PE_FLAT, flats[8]));
            /* Clipboard/undo records keep their identity across retirement; the
             * retired pointer is never compared, the name re-resolves. */
            sh_package_editor_rows_retire();
            { pe_want want = pe_row_wanted(flats[8], owner);
              CHECK(want.recorded && want.decl == rows_resolve(NULL, "pkg/start_5")); }
            CHECK(row_name(PE_FLAT, flats[8]) && !strcmp(row_name(PE_FLAT, flats[8]), "pkg/start_5"));
            /* Supplier lineage: exactly a context the owner's own uses selected. */
            {
                const sh_package_provenance *own = sh_package_occurrence_provenance(owner);
                const sh_package_provenance_context *mine;
                sh_package_provenance row = {0}, variant = {0};
                unsigned char other[1][32], pair[2][32]; size_t at[2];
                CHECK(own && own->use_count && own->uses[0].context_count);
                mine = own->contexts + own->uses[0].contexts[0];
                memset(other, 0x5e, sizeof(other)); memcpy(pair[0], mine->trees[0], 32); memset(pair[1], 0x5e, 32);
                at[0] = sh_package_provenance_context_add(&row, mine->mode,
                    (const unsigned char (*)[32])mine->trees, mine->tree_count, mine->component);
                at[1] = sh_package_provenance_context_add(&row, SH_PACKAGE_CONTEXT_ISOLATED,
                    (const unsigned char (*)[32])other, 1, "pkg/other");
                CHECK(sh_package_provenance_bind(&row, "snapeditorentitydef", "start.decl", "pkg/start_5", at + 1, 1));
                CHECK(!pe_owner_supplies(owner, &row));                         /* another context only */
                CHECK(sh_package_provenance_bind(&row, "snapeditorentitydef", "start.decl", "pkg/start_5", at, 1));
                CHECK(pe_owner_supplies(owner, &row) == 1 && !pe_owner_supplies(NULL, &row)); /* shared */
                /* The same tree in another dependency context is another variant. */
                at[0] = sh_package_provenance_context_add(&variant, SH_PACKAGE_CONTEXT_COMPOSED,
                    (const unsigned char (*)[32])pair, 2, NULL);
                CHECK(sh_package_provenance_bind(&variant, "snapeditorentitydef", "start.decl", "pkg/start_6", at, 1));
                CHECK(!pe_owner_supplies(owner, &variant));
                sh_package_provenance_free(&row); sh_package_provenance_free(&variant);
            }
        }
        rows_fill(ties, 0); rows_palette[rows_count-1].key = NULL; /* unreadable holder: engine result */
        if (ties == 2) { static uintptr_t body[2]; rows_place(flats[5], variant, NULL);
            pe_from(rows_palette, NULL, flats[5], body); CHECK(body[0] == (uintptr_t)native); pe_body_dtor(body); }
        rows_steering = 0;                                   /* negative control: order decides */
        CHECK(rows_build(flats[0], ties, 1) != native && rows_build(flats[1], ties, 0) != variant);
        for (int i = 0; i < 9; i++) pe_flat_dtor(flats[i]);
        CHECK(!g_row_count);
    }
    rows_owner = NULL; sh_package_occurrence_release(owner); sh_package_occurrence_release(document);
    pe_flat_dtor(owned); pe_flat_dtor(owned + 0x1b0); sh_package_editor_clear_world(); sh_package_provenance_free(&uses);
    sh_package_editor_bind_rows(NULL); CHECK(sh_package_editor_uninstall()); /* frees the tracker and originals */
}
/* B2: an object without a record whose native default left the pickers,
 * because an equal exposure stands for it (package_runtime.c), is built from
 * the tie that exposure keeps (stands_for), and nothing is captured: unowned,
 * or owned by a map occurrence that supplies no tied row. Any other changed
 * tie is a different exposure, never a stand-in, and a recorded or supplied
 * row is never replaced. */
static size_t rows_kept = SIZE_MAX; static int rows_stands_unknown;
static int rows_stands_for(void *context, const void *decl, const void *candidate)
{
    (void)context;
    if (rows_stands_unknown && candidate == row_decls + 1) return -1; /* one tie's name unreadable */
    return decl == row_decls && rows_kept != SIZE_MAX && candidate == row_decls + rows_kept;
}
static void exposure_rows(void)
{
    const sh_package_editor_rows_api plain = {rows_view, rows_key, rows_classify, rows_resolve, rows_supplied, NULL, NULL, rows_exact};
    sh_package_editor_rows_api api = plain;
    static char flats[10][16]; static uintptr_t built[2];
    static unsigned char map[64], owned[2 * 0x1b0];
    char error[512]; sh_package_provenance uses = {0};
    api.stands_for = rows_stands_for;
    if (!g_tracker) g_tracker = sh_package_occurrence_tracker_create();
    CHECK(g_tracker); sh_package_editor_bind(NULL, NULL); sh_package_editor_bind_rows(&plain);
    make_document(&uses); *(void **)(map + 0x20) = owned; *(int *)(map + 0x28) = 2;
    sh_package_occurrence_snapshot *document = sh_package_occurrence_prepare_document(&uses, sources, 4, error, sizeof(error)), *owner;
    CHECK(document && sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, document, error, sizeof(error)));
    owner = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, owned)); CHECK(owner); rows_owner = owner;
    g_copy_assign = g_move_assign = copy_body; g_body_dtor = g_flat_dtor = destroy; g_clone = clone_body; g_from = from_steered;
    rows_inherit = start_key; rows_steering = 1; rows_first_class = 0; rows_resolved = NULL;
    rows_supplier_a = rows_supplier_b = 50; /* the owner supplies no tied row */
    rows_kept = 2;
    CHECK(rows_build(flats[0], 4, 0) == row_decls);                 /* no selection bound: the native default */
    sh_package_editor_bind_rows(&api); rows_kept = SIZE_MAX;
    CHECK(rows_build(flats[1], 4, 0) == row_decls);                 /* the native row stays listed */
    rows_kept = 50;                                                 /* its stand-in is no tie: its own native default */
    rows_fill(4, 0); pe_from(rows_palette, NULL, flats[5], built);
    CHECK(built[0] == (uintptr_t)row_decls && !pe_row_pending(PE_BODY, built)); pe_body_dtor(built);
    rows_kept = 2;                                                  /* ties 1 and 3 are other changed rows */
    for (int order = 0; order < 2; order++) {
        rows_fill(4, order); pe_from(rows_palette, NULL, flats[2 + order], built);
        CHECK(built[0] == (uintptr_t)(row_decls + 2) && pe_row_pending(PE_BODY, built)); /* the kept row, uncaptured */
        pe_body_dtor(built);
    }
    rows_kept = SIZE_MAX; rows_stands_unknown = 1;
    CHECK(rows_build(flats[4], 4, 0) == row_decls);                 /* an unreadable tie is no stand-in */
    rows_kept = 2; rows_stands_unknown = 0;
    rows_place(flats[7], row_decls + 3, NULL);                      /* a recorded row stays itself */
    CHECK(rows_build(flats[7], 4, 0) == row_decls + 3 && rows_build(flats[7], 4, 1) == row_decls + 3);
    /* Owned by the attached map, which supplies no tied row: the kept row
     * too, its choice unresolved; a row the owner supplies stays its own;
     * without a selection, the native original. */
    CHECK(sh_package_occurrence_set(g_tracker, pe_key(PE_FLAT, flats[6]), owner));
    for (int order = 0; order < 2; order++) {
        rows_fill(4, order); pe_from(rows_palette, NULL, flats[6], built);
        CHECK(built[0] == (uintptr_t)(row_decls + 2) && pe_row_pending(PE_BODY, built)); pe_body_dtor(built);
    }
    rows_supplier_a = 3;
    CHECK(sh_package_occurrence_set(g_tracker, pe_key(PE_FLAT, flats[8]), owner));
    for (int order = 0; order < 2; order++) {
        rows_fill(4, order); pe_from(rows_palette, NULL, flats[8], built);
        CHECK(built[0] == (uintptr_t)(row_decls + 3) && !pe_row_pending(PE_BODY, built)); pe_body_dtor(built);
    }
    rows_supplier_a = 50;
    sh_package_editor_bind_rows(&plain);
    rows_fill(4, 0); pe_from(rows_palette, NULL, flats[6], built);
    CHECK(built[0] == (uintptr_t)row_decls && pe_row_pending(PE_BODY, built)); pe_body_dtor(built);
    rows_kept = SIZE_MAX;
    for (int i = 0; i < 10; i++) pe_flat_dtor(flats[i]);
    CHECK(!g_row_count);
    rows_supplier_a = rows_supplier_b = 0; rows_owner = NULL; sh_package_occurrence_release(owner); sh_package_occurrence_release(document);
    pe_flat_dtor(owned); pe_flat_dtor(owned + 0x1b0); sh_package_editor_clear_world(); sh_package_provenance_free(&uses);
    sh_package_editor_bind_rows(NULL); CHECK(sh_package_editor_uninstall()); /* frees the tracker */
}
/* No tied row is a native original: a changed public row occupies the native
 * slot. Native-only data builds that slot in either palette order and never
 * records it; an added variant wins only by its own recorded or supplied
 * choice, and an unresolved record never substitutes another variant. */
static void native_slot_rows(void)
{
    const sh_package_editor_rows_api api = {rows_view, rows_key, rows_classify, rows_resolve, rows_supplied, NULL};
    static char flats[5][16]; static uintptr_t built[2];
    static unsigned char map[64], owned[2 * 0x1b0];
    char error[512]; sh_package_provenance uses = {0};
    const void *slot = row_decls, *variant = row_decls + 1;
    if (!g_tracker) g_tracker = sh_package_occurrence_tracker_create();
    CHECK(g_tracker); sh_package_editor_bind(NULL, NULL); sh_package_editor_bind_rows(&api);
    make_document(&uses); *(void **)(map + 0x20) = owned; *(int *)(map + 0x28) = 2;
    sh_package_occurrence_snapshot *document = sh_package_occurrence_prepare_document(&uses, sources, 4, error, sizeof(error)), *owner;
    CHECK(document && sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, document, error, sizeof(error)));
    owner = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, owned)); CHECK(owner); rows_owner = owner;
    g_copy_assign = g_move_assign = copy_body; g_body_dtor = g_flat_dtor = destroy; g_clone = clone_body; g_from = from_steered;
    rows_inherit = start_key; rows_steering = 1; rows_first_class = 2; rows_resolved = NULL;
    rows_supplier_a = rows_supplier_b = 50; /* the owner supplies no tied row */
    for (int order = 0; order < 2; order++) {
        rows_fill(2, order); pe_from(rows_palette, NULL, flats[0], built); /* unowned native-only entity */
        CHECK(built[0] == (uintptr_t)slot && pe_row_pending(PE_BODY, built)); pe_clone(built, flats[0], 0); pe_body_dtor(built);
        CHECK(!row_name(PE_FLAT, flats[0]) || !*row_name(PE_FLAT, flats[0]));
        CHECK(rows_build(flats[0], 2, !order) == slot && rows_build(flats[0], 2, order) == slot);
        CHECK(sh_package_occurrence_set(g_tracker, pe_key(PE_FLAT, flats[1]), owner)); /* attached, no supplier */
        rows_fill(2, order); pe_from(rows_palette, NULL, flats[1], built);
        CHECK(built[0] == (uintptr_t)slot && pe_row_pending(PE_BODY, built)); pe_body_dtor(built);
        rows_supplier_a = 1; CHECK(rows_build(flats[1], 2, order) == variant && rows_build(flats[1], 2, !order) == variant);
        rows_supplier_a = 50;
        rows_place(flats[2], variant, NULL);              /* placed variant: in-session exact choice */
        CHECK(rows_build(flats[2], 2, order) == variant && rows_build(flats[2], 2, !order) == variant);
        rows_place(flats[3], row_decls + 2, NULL);        /* recorded variant absent, unresolved */
        sh_package_editor_rows_retire(); rows_fill(2, !order); pe_from(rows_palette, NULL, flats[3], built);
        CHECK(built[0] == (uintptr_t)slot && pe_row_pending(PE_BODY, built)); pe_clone(built, flats[4], 0); pe_body_dtor(built);
        CHECK(row_name(PE_FLAT, flats[4]) && !strcmp(row_name(PE_FLAT, flats[4]), "pkg/start_2"));
        for (int i = 0; i < 5; i++) pe_flat_dtor(flats[i]);
        CHECK(!g_row_count);
    }
    rows_first_class = 0; rows_supplier_a = rows_supplier_b = 0; rows_owner = NULL; sh_package_occurrence_release(owner); sh_package_occurrence_release(document);
    pe_flat_dtor(owned); pe_flat_dtor(owned + 0x1b0); sh_package_editor_clear_world(); sh_package_provenance_free(&uses);
    sh_package_editor_bind_rows(NULL); CHECK(sh_package_editor_uninstall());
}
/* Exact rows through documents: export from flat records, restore before
 * construction, copy/prefab, rebuild carry and wrong-variant-only palettes.
 * Names are the production format: 64 lowercase hex digits. */
static int hex_published[ROW_DECLS];
static int hex_classify(void *context, const void *decl, char *name, size_t capacity)
{
    ptrdiff_t i = (const char *)decl - row_decls; (void)context;
    if (i < 0 || i >= ROW_UNKNOWN) return -1;
    if (!i) return 0;
    if (name) snprintf(name, capacity, "%064x", (unsigned)i);
    return 1;
}
static const void *hex_resolve(void *context, const char *name)
{
    unsigned i = 0; (void)context;
    if (!sh_package_provenance_row_valid(name) || sscanf(name + 56, "%8x", &i) != 1 || i >= ROW_UNKNOWN) return NULL;
    return hex_published[i] ? row_decls + i : NULL;
}
/* One authored row's portable identity on two dependency variants (rows 2
 * and 3): resolution names neither, so the ties carrying it decide. */
static int shared_classify(void *context, const void *decl, char *name, size_t capacity)
{
    ptrdiff_t i = (const char *)decl - row_decls;
    int result = hex_classify(context, decl, name, capacity);
    if (result > 0 && name && i == 3) snprintf(name, capacity, "%064x", 2u);
    return result;
}
static const void *shared_resolve(void *context, const char *name) { (void)context; (void)name; return NULL; }
static int row_is(const sh_package_occurrence_snapshot *snapshot, const char *owner, const char *expected)
{
    const char *row = NULL;
    int state = sh_package_provenance_row_get(sh_package_occurrence_provenance(snapshot), owner, &row);
    return expected ? state == 1 && !strcmp(row, expected) : state == 0;
}
static int record_is(const void *object, const char *expected)
{ const char *name = row_name(PE_FLAT, object); return expected ? name && !strcmp(name, expected) : !name; }
static sh_package_occurrence_snapshot *inspect_result;
static int inspect_rebuilt(void *context, unsigned kind, void *flat,
    const sh_package_occurrence_snapshot *previous, sh_package_occurrence_snapshot **next,
    char *error, size_t capacity)
{
    (void)context; (void)kind; (void)flat; (void)previous; (void)error; (void)capacity;
    *next = sh_package_occurrence_retain(inspect_result); return *next != NULL;
}
static unsigned char *map_bytes(const unsigned char *map, size_t *length)
{
    char error[512]; unsigned char *bytes = NULL;
    sh_package_occurrence_snapshot *value = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_MAP, map, error, sizeof(error));
    if (value) bytes = sh_package_provenance_encode(sh_package_occurrence_provenance(value), length, error, sizeof(error));
    sh_package_occurrence_release(value); return bytes;
}
static void document_rows(void)
{
    const sh_package_editor_rows_api api = {rows_view, rows_key, hex_classify, hex_resolve, rows_supplied, NULL};
    static unsigned char objects[3 * 0x1b0], map[64], copied[0x1b0], placed[0x1b0], prefab[64], staged[3 * 0x1b0];
    static unsigned char reopened_objects[3 * 0x1b0], reopened_map[64];
    static uintptr_t body[2];
    char error[512], two[65], three[65];
    sh_package_provenance uses = {0}, rebuilt_uses = {0}, decoded = {0};
    sh_package_occurrence_snapshot *document, *exported, *rebuilt, *empty, *again, *reopened;
    unsigned char *encoded; size_t length;
    snprintf(two, sizeof(two), "%064x", 2u); snprintf(three, sizeof(three), "%064x", 3u);
    g_tracker = sh_package_occurrence_tracker_create(); CHECK(g_tracker);
    sh_package_editor_bind(NULL, NULL); sh_package_editor_bind_rows(&api);
    g_copy_assign = g_move_assign = g_flat_assign = copy_body; g_body_dtor = g_flat_dtor = destroy;
    g_clone = clone_body; g_from = from_steered; rows_inherit = start_key; rows_steering = 1; rows_owner = NULL;
    *(void **)(map + 0x20) = objects; *(int *)(map + 0x28) = 3;
    make_document(&uses);
    document = sh_package_occurrence_prepare_document(&uses, sources, 4, error, sizeof(error)); CHECK(document);
    {   /* One choice per object, on every OBJECT use of its owner; never ownership. */
        const char *rows[3] = {two, NULL, three}, *bad[3] = {"0011", NULL, NULL};
        sh_package_occurrence_retain(document);
        CHECK(!sh_package_occurrence_document_rows(document, rows, 3)); /* shared: immutable */
        sh_package_occurrence_release(document);
        CHECK(!sh_package_occurrence_document_rows(document, bad, 3) && row_is(document, "entity/0", NULL));
        CHECK(sh_package_occurrence_document_rows(document, rows, 3));
    }
    CHECK(row_is(document, "entity/0", two) && row_is(document, "entity/1", NULL) && row_is(document, "entity/2", NULL));
    /* Attach restores flat records before any construction. */
    CHECK(sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, document, error, sizeof(error)));
    CHECK(record_is(objects, two) && record_is(objects + 0x1b0, NULL) && record_is(objects + 0x360, three));
    /* Construction honors the restored row by identity, whatever the tie order,
     * under its owner's selected supplier; the name alone, even resolved
     * uniquely, is not the choice. */
    hex_published[2] = hex_published[3] = 1;
    {
        sh_package_occurrence_snapshot *owner = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, objects));
        rows_fill(4, 0); pe_from(rows_palette, NULL, objects, body);
        CHECK(body[0] == (uintptr_t)row_decls && pe_row_pending(PE_BODY, body)); pe_body_dtor(body);
        rows_owner = owner; rows_supplier_a = 2;
        for (int order = 0; order < 2; order++) {
            rows_fill(4, order); pe_from(rows_palette, NULL, objects, body);
            CHECK(body[0] == (uintptr_t)(row_decls + 2) && !pe_row_pending(PE_BODY, body));
            CHECK(row_name(PE_BODY, body) && !strcmp(row_name(PE_BODY, body), two)); pe_body_dtor(body);
        }
        rows_owner = NULL; rows_supplier_a = 0; sh_package_occurrence_release(owner);
    }
    {   /* Variants sharing the portable identity: the tie key is the object's own
         * entityDef; among ties carrying it, only its owner's selected supplier. */
        sh_package_editor_rows_api shared = api;
        sh_package_occurrence_snapshot *owner = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, objects));
        shared.classify = shared_classify; shared.resolve = shared_resolve;
        sh_package_editor_bind_rows(&shared);
        rows_owner = owner; rows_supplier_a = rows_supplier_b = 3;
        rows_fill(4, 0); pe_from(rows_palette, NULL, objects, body);
        CHECK(owner && body[0] == (uintptr_t)(row_decls + 3) && !pe_row_pending(PE_BODY, body)); pe_body_dtor(body);
        rows_supplier_a = rows_supplier_b = 50; /* neither variant is the owner's */
        rows_fill(4, 0); pe_from(rows_palette, NULL, objects, body);
        CHECK(body[0] == (uintptr_t)row_decls && pe_row_pending(PE_BODY, body));
        CHECK(row_name(PE_BODY, body) && !strcmp(row_name(PE_BODY, body), two)); pe_body_dtor(body);
        rows_fill(4, 0); rows_palette[rows_count-1].key = "a/variant"; /* row 3 links another entityDef */
        pe_from(rows_palette, NULL, objects, body); /* the one tie left, unconfirmed: never taken */
        CHECK(body[0] == (uintptr_t)row_decls && pe_row_pending(PE_BODY, body)); pe_body_dtor(body);
        rows_supplier_a = 2; rows_fill(4, 0); rows_palette[rows_count-1].key = "a/variant";
        pe_from(rows_palette, NULL, objects, body);
        CHECK(body[0] == (uintptr_t)(row_decls + 2) && !pe_row_pending(PE_BODY, body)); pe_body_dtor(body);
        rows_owner = NULL; rows_supplier_a = rows_supplier_b = 0;
        sh_package_occurrence_release(owner); sh_package_editor_bind_rows(&api);
    }
    /* Wrong-variant-only availability: the old identity stays recorded, unresolved. */
    hex_published[2] = 0;
    rows_fill(4, 0); pe_from(rows_palette, NULL, objects, body);
    CHECK(body[0] == (uintptr_t)row_decls && pe_row_pending(PE_BODY, body)); /* logged native fallback */
    CHECK(row_name(PE_BODY, body) && !strcmp(row_name(PE_BODY, body), two));
    pe_clone(body, placed, 0); pe_body_dtor(body);   /* placed back: carried, never re-captured */
    CHECK(record_is(placed, two) && pe_row_pending(PE_FLAT, placed));
    again = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_ENTITY, placed, error, sizeof(error));
    CHECK(again && row_is(again, "entity/0", two)); sh_package_occurrence_release(again);
    /* Copy/undo: flat assignment carries record and ownership together. */
    pe_flat_assign(copied, objects);
    again = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_ENTITY, copied, error, sizeof(error));
    CHECK(again && row_is(again, "entity/0", two)); sh_package_occurrence_release(again);
    /* Export: a named record is a choice; an empty pending record states none. */
    pe_row_pend(PE_FLAT, objects + 0x1b0);
    exported = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_MAP, map, error, sizeof(error));
    CHECK(exported && row_is(exported, "entity/0", two) && row_is(exported, "entity/1", NULL));
    CHECK(row_is(exported, "entity/2", NULL) && sh_package_occurrence_document_row(exported, 2) &&
        !strcmp(sh_package_occurrence_document_row(exported, 2), three) && !sh_package_occurrence_document_row(exported, 1));
    encoded = sh_package_provenance_encode(sh_package_occurrence_provenance(exported), &length, error, sizeof(error));
    CHECK(encoded && !memcmp(encoded, "SMPBIND\2", 8) && sh_package_provenance_decode(encoded, length, &decoded, error, sizeof(error)));
    free(encoded);
    /* A rebuilt capture that newly owns entity/2 regains the table's row; a
     * different recorded row is never replaced. */
    make_document(&rebuilt_uses);
    {
        size_t b = binding(&rebuilt_uses, "smp/shared-boss"), context = rebuilt_uses.bindings[b].contexts[0];
        sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/2", "/m7:inherit"};
        CHECK(sh_package_provenance_use_set(&rebuilt_uses, &site, b, &context, 1));
    }
    rebuilt = sh_package_occurrence_prepare_document(&rebuilt_uses, sources, 4, error, sizeof(error));
    CHECK(rebuilt && sh_package_occurrence_carry_document_rows(rebuilt, exported));
    CHECK(row_is(rebuilt, "entity/0", two) && row_is(rebuilt, "entity/1", NULL) && row_is(rebuilt, "entity/2", three));
    sh_package_occurrence_release(rebuilt);
    rebuilt = sh_package_occurrence_prepare_document(&rebuilt_uses, sources, 4, error, sizeof(error));
    { const char *rows[3] = {three, NULL, NULL}; CHECK(rebuilt && sh_package_occurrence_document_rows(rebuilt, rows, 3)); }
    CHECK(!sh_package_occurrence_carry_document_rows(rebuilt, exported) && !sh_package_occurrence_carry_rows(rebuilt, &decoded));
    CHECK(row_is(rebuilt, "entity/0", three) && row_is(rebuilt, "entity/2", NULL));
    sh_package_occurrence_release(rebuilt);
    /* A successful inspection whose rebuilt document states no rows keeps
     * every choice, including the empty pending record. */
    empty = sh_package_occurrence_prepare_document(&uses, sources, 4, error, sizeof(error)); CHECK(empty);
    inspect_result = empty; sh_package_editor_bind(inspect_rebuilt, NULL);
    CHECK(sh_package_editor_inspect(SH_PACKAGE_EDITOR_MAP, map, error, sizeof(error)));
    sh_package_editor_bind(NULL, NULL); inspect_result = NULL;
    CHECK(record_is(objects, two) && pe_row_pending(PE_FLAT, objects + 0x1b0) && record_is(objects + 0x360, three));
    {   /* Attach is atomic: a row copy failing for the first or a later object
         * leaves ownership, records and the world exactly as they were. */
        const char *rows[3] = {three, NULL, two};
        sh_package_occurrence_snapshot *swapped, *world, *current;
        unsigned char *before, *after; size_t before_length = 0, after_length = 0;
        swapped = sh_package_occurrence_prepare_document(&rebuilt_uses, sources, 4, error, sizeof(error));
        CHECK(swapped && sh_package_occurrence_document_rows(swapped, rows, 3));
        world = sh_package_editor_world(); before = map_bytes(map, &before_length); CHECK(world && before);
        for (long fail = 0; fail < 2; fail++) {
            pe_alloc_fail_after = fail;
            CHECK(!sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, swapped, error, sizeof(error)));
            pe_alloc_fail_after = -1;
            current = sh_package_editor_world(); CHECK(current == world); sh_package_occurrence_release(current);
            CHECK(record_is(objects, two) && pe_row_pending(PE_FLAT, objects + 0x1b0) && record_is(objects + 0x360, three));
            after = map_bytes(map, &after_length);
            CHECK(after && after_length == before_length && !memcmp(after, before, before_length)); free(after);
        }
        /* A fresh attach is authoritative, so a reused container keeps no
         * stale record: the swapped rows replace, the pending record goes. */
        CHECK(sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, swapped, error, sizeof(error)));
        CHECK(record_is(objects, three) && record_is(objects + 0x1b0, NULL) && record_is(objects + 0x360, two));
        current = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_MAP, map, error, sizeof(error));
        CHECK(current && row_is(current, "entity/0", three) && row_is(current, "entity/2", two));
        sh_package_occurrence_release(current);
        after = map_bytes(map, &after_length);
        CHECK(after && (after_length != before_length || memcmp(after, before, before_length))); free(after);
        CHECK(sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, empty, error, sizeof(error)));
        CHECK(record_is(objects, NULL) && record_is(objects + 0x1b0, NULL) && record_is(objects + 0x360, NULL));
        free(before); sh_package_occurrence_release(world); sh_package_occurrence_release(swapped);
    }
    sh_package_occurrence_release(empty);
    /* Prefab staging: attach restores, snapshot exports. */
    *(void **)(prefab + 0x30) = staged; *(int *)(prefab + 0x38) = 3;
    CHECK(sh_package_editor_attach(SH_PACKAGE_EDITOR_PREFAB, prefab, exported, error, sizeof(error)));
    CHECK(record_is(staged, two) && record_is(staged + 0x1b0, NULL) && record_is(staged + 0x360, three));
    again = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_PREFAB, prefab, error, sizeof(error));
    CHECK(again && row_is(again, "entity/0", two)); sh_package_occurrence_release(again);
    /* Encoded, decoded, attached to a fresh map and constructed. The file kept
     * rows only on owned uses, so the unowned object restores nothing. */
    hex_published[2] = 1;
    *(void **)(reopened_map + 0x20) = reopened_objects; *(int *)(reopened_map + 0x28) = 3;
    reopened = sh_package_occurrence_prepare_document(&decoded, sources, 4, error, sizeof(error));
    CHECK(reopened && sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, reopened_map, reopened, error, sizeof(error)));
    CHECK(record_is(reopened_objects, two) && record_is(reopened_objects + 0x360, NULL));
    {
        sh_package_occurrence_snapshot *owner = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, reopened_objects));
        rows_fill(4, 1); pe_from(rows_palette, NULL, reopened_objects, body);
        CHECK(body[0] == (uintptr_t)row_decls && pe_row_pending(PE_BODY, body)); pe_body_dtor(body);
        rows_owner = owner; rows_supplier_a = 2;
        rows_fill(4, 1); pe_from(rows_palette, NULL, reopened_objects, body);
        CHECK(body[0] == (uintptr_t)(row_decls + 2) && !pe_row_pending(PE_BODY, body)); pe_body_dtor(body);
        rows_owner = NULL; rows_supplier_a = 0; sh_package_occurrence_release(owner);
    }
    sh_package_occurrence_release(reopened);
    /* A malformed record refuses export instead of dropping a choice. */
    {
        pe_row bad = {PE_FLAT, (uintptr_t)(objects + 0x1b0), NULL, 0, 0, "pkg/not-a-row"}; int stored;
        AcquireSRWLockExclusive(&g_row_lock); stored = pe_row_store(PE_FLAT, objects + 0x1b0, &bad); ReleaseSRWLockExclusive(&g_row_lock);
        CHECK(stored && !sh_package_editor_snapshot(SH_PACKAGE_EDITOR_MAP, map, error, sizeof(error)));
    }
    for (size_t i = 0; i < 3; i++) {
        pe_flat_dtor(objects + i * 0x1b0); pe_flat_dtor(staged + i * 0x1b0); pe_flat_dtor(reopened_objects + i * 0x1b0);
    }
    pe_flat_dtor(copied); pe_flat_dtor(placed); CHECK(!g_row_count);
    sh_package_occurrence_release(exported); sh_package_occurrence_release(document); sh_package_editor_clear_world();
    sh_package_provenance_free(&uses); sh_package_provenance_free(&rebuilt_uses); sh_package_provenance_free(&decoded);
    sh_package_editor_bind_rows(NULL); CHECK(sh_package_editor_uninstall());
}
int main(void)
{
    char temporary[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "ped", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    make_sources(); native_transfers(); exact_rows(); exposure_rows(); native_slot_rows(); document_rows(); atomic_hook_install();
    release_source_rows(); cleanup();
    printf("package_editor_test: %d failures\n", failures); return failures ? 1 : 0;
}
