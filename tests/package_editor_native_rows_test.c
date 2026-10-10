/* Provider to editor: decl_server's exact publication lineage, recorded from
 * a production compile/link/refresh of installed packages, bound to the
 * editor's steer through the production rows callbacks. Only the palette
 * view and the engine construction are faked. */
#define main decl_server_fixture_main
#include "decl_server_test.c"
#undef main
#define SH_PACKAGE_EDITOR_TESTING
#define SH_PACKAGE_EDITOR_NATIVE_ROWS
#include "../src/backend/package_editor.c"

void *hook_prepare_rip_lea(void *target, void *detour, size_t stolen, size_t lea)
{ (void)lea; return hook_prepare(target, detour, stolen); }

typedef struct nr_entry { const void *decl; const char *key; } nr_entry;
static nr_entry nr_palette[4]; static size_t nr_count;
static const char nr_key_name[] = "player/coop/player_start";
static int nr_view(void *context, void *palette, const unsigned char **entries, size_t *count, size_t *stride)
{ (void)context; (void)palette; *entries = (const unsigned char *)nr_palette; *count = nr_count; *stride = sizeof(nr_entry); return 1; }
static const char *nr_key(void *context, const void *entry) { (void)context; return ((const nr_entry *)entry)->key; }
static void nr_fill(const void *first, const void *second, const void *third)
{
    const void *decls[3] = {first, second, third};
    memset(nr_palette, 0, sizeof(nr_palette)); nr_count = 0;
    for (int i = 0; i < 3; i++) if (decls[i]) nr_palette[nr_count++] = (nr_entry){decls[i], nr_key_name};
}
/* The engine's by-name lookup: the first equal key, then the steer. */
static void nr_from(void *palette, void *variables, void *flat, void *body)
{
    const nr_entry *engine = NULL; (void)variables; (void)flat;
    for (size_t i = 0; i < nr_count && !engine; i++) if (!strcmp(nr_palette[i].key, nr_key_name)) engine = nr_palette + i;
    engine = sh_package_editor_row_steer(palette, nr_key_name, engine, NULL);
    *(const void **)body = engine ? engine->decl : NULL;
}
static void *nr_copy(void *destination, void *source) { (void)source; return destination; }
static void nr_destroy(void *object) { (void)object; }
static void nr_clone(void *body, void *flat, unsigned char pinned) { (void)body; (void)flat; (void)pinned; }
static const char *nr_record(unsigned kind, const void *object)
{ size_t i = pe_row_find(kind, object); return i == SIZE_MAX ? NULL : g_row_items[i].name; }
static const char *nr_exact(unsigned kind, const void *object)
{ size_t i = pe_row_find(kind, object); return i == SIZE_MAX ? NULL : g_row_items[i].exact; }
static int nr_lost(unsigned kind, const void *object)
{ size_t i = pe_row_find(kind, object); return i != SIZE_MAX && g_row_items[i].lost && g_row_items[i].pending && !*g_row_items[i].name && !g_row_items[i].exact; }
/* An injected failure faults ownership (saves refuse); clear it for the next control. */
static int nr_faulted(void)
{ int faulted = !sh_package_editor_healthy(NULL, 0); InterlockedExchange(&g_faulted, 0); g_failure[0] = 0; return faulted; }
static int nr_exact_none(void *context, const void *decl, char *name, size_t capacity)
{ (void)context; (void)decl; (void)name; (void)capacity; return 0; }
/* A new publication of count resources, in that order, then retirement. */
static int nr_publish(const sh_package_compilation *shaped, sh_compiled_resource *resources, size_t count)
{
    sh_package_compilation one = *shaped; int ok;
    one.resources = resources; one.resource_count = count;
    sh_decl_server_test_rows_reset(); sh_decl_server_test_rows_bindings(sh_package_runtime_bindings());
    sh_decl_server_test_rows_manager(&rl_manager, rl_source);
    ok = sh_decl_server_test_rows_record(&one, count);
    sh_package_editor_rows_retire();
    return ok;
}
/* A later publication into the same lineage: production never resets it. */
static int nr_republish(const sh_package_compilation *shaped, sh_compiled_resource *resources, size_t count)
{
    sh_package_compilation one = *shaped; int ok;
    one.resources = resources; one.resource_count = count;
    ok = sh_decl_server_test_rows_record(&one, count);
    sh_package_editor_rows_retire();
    return ok;
}
/* Production mode's native resource index: the stock row's own path only. */
static int nr_index = 1; static char nr_index_path[200];
static int nr_native_index(const char *path)
{
    snprintf(nr_index_path, sizeof(nr_index_path), "%s", path);
    return strcmp(path, "generated/decls/snapeditorentitydef/player/start.decl") ? 0 : nr_index;
}
/* What constructing flat built and recorded; the body is then placed back
 * into flat (the save path) and destroyed. */
static const void *nr_build(void *flat, char record[80], int *pending)
{
    uintptr_t body[2] = {0};
    const char *name;
    pe_from(nr_palette, NULL, flat, body);
    name = nr_record(PE_BODY, body); snprintf(record, 80, "%s", name ? name : "");
    *pending = pe_row_pending(PE_BODY, body);
    pe_clone(body, flat, 0); pe_body_dtor(body);
    return (const void *)body[0];
}

/* The fixture reflection plus one typed reference field on the editor row
 * itself (icon, an entityDef pointer that is not the row's entityDef key). */
static void nr_reflection(slot_fixture *f)
{
    unsigned char *container, *records, *editor, *fields;
    slot_reflection(f);
    container = *(unsigned char **)f->reflection; records = *(unsigned char **)(container + 0x20);
    editor = *(unsigned char **)(records + 4 * 0x38 + 0x20);
    fields = slot_alloc(f, 4 * 0x48);
    memcpy(fields, editor, 2 * 0x48);
    slot_pointer(fields + 2 * 0x48, 0, slot_string(f, "idDeclEntityDef"));
    slot_pointer(fields + 2 * 0x48, 8, slot_string(f, "*"));
    slot_pointer(fields + 2 * 0x48, 0x10, slot_string(f, "icon")); slot_number(fields + 2 * 0x48, 0x1c, 8);
    slot_pointer(records, 4 * 0x38 + 0x20, fields);
}
/* names: the three packages' folders and descriptor IDs, in authored order;
 * the second always carries the dependency variant. element: the variant
 * differs through the row's own icon reference, not through its entityDef. */
static void test_provider_editor_rows(int element, const char *const names[3], char portable[80])
{
    static int run;
    const sh_package_editor_rows_api api = {nr_view, nr_key, pe_native_classify, pe_native_resolve,
        pe_native_supplied, NULL, pe_native_named, pe_native_exact};
    static unsigned char objects[4 * 0x1b0], map[64];
    char root[MAX_PATH], temp[MAX_PATH], identity[2][80] = {{0}}, linked[2][80] = {{0}}, error[512], record[80];
    const sh_package_compilation *compiled = NULL;
    sh_compiled_resource rows[2];
    sh_package_compilation shaped = {0};
    sh_package_provenance supply[2] = {{0}}, uses = {0};
    sh_package_bundle_binding bundles[3] = {{{0}}};
    size_t bundle_count = 0;
    sh_package_occurrence_snapshot *document = NULL, *exported = NULL;
    rl_decl live[2] = {{0, NULL}, {0, NULL}}, native = {0, "player/start"};
    slot_fixture fixture;
    sh_decl_registry_source registry;
    size_t found = 0, shared = 0, variant = 1;
    int pending, before = g_failed;
    g_port_stock = g_port_variant = 0;
    GetTempPathA(sizeof(temp), temp);
    snprintf(root, sizeof(root), "%spe-native-rows-%lu-%d", temp, GetCurrentProcessId(), run++);
    CreateDirectoryA(root, NULL);
    for (int p = 0; p < 3; p++) { /* a and c equal; b changes only a dependency */
        char path[MAX_PATH], text[256];
        snprintf(path, sizeof(path), "overrides/%s/package.json", names[p]);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", names[p], names[p]);
        slot_file(root, path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/%s", names[p], slot_def);
        slot_file(root, path, "{\n\tclass = \"Entity\";\n\tedit = {\n\t\ttarget = \"player/coop/base\";\n\t\thealth = 150;\n\t}\n}\n");
        if (element) { /* a package-new referent, equal in a and c */
            snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/player/coop/extra.decl", names[p]);
            slot_file(root, path, p == 1 ? "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 7;\n\t}\n}\n" :
                "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 150;\n\t}\n}\n");
        } else if (p == 1) {
            snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/player/coop/base.decl", names[p]);
            slot_file(root, path, "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 7;\n\t}\n}\n");
        }
        snprintf(path, sizeof(path), "overrides/%s/assets/%s", names[p], slot_row);
        slot_file(root, path, element ? "{\n\tedit = {\n\t\tentityDef = \"player/coop/player_start\";\n"
            "\t\ticon = \"player/coop/extra\";\n"
            "\t\tplacementFlags = \"SNAP_ENTITY_PLACE_AIR_UNALIGNED\";\n\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n" :
            "{\n\tedit = {\n\t\tentityDef = \"player/coop/player_start\";\n"
            "\t\tplacementFlags = \"SNAP_ENTITY_PLACE_AIR_UNALIGNED\";\n\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n");
    }
    if (element) nr_reflection(&fixture); else slot_reflection(&fixture);
    registry = (sh_decl_registry_source){&fixture, slot_read, (uintptr_t)fixture.registry, NULL};
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(port_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)fixture.reflection);
    sh_package_runtime_test_installed_links(1);
    if (!sh_package_runtime_refresh(root)) {
        sh_package_runtime_error(error, sizeof(error)); fprintf(stderr, "provider rows library: %s\n", error); CHECK(0); goto done;
    }
    compiled = sh_package_runtime_acquire();
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->type && r->original_path && !_stricmp(r->type, "snapeditorentitydef") &&
            !strcmp(r->original_path, slot_row) && found++ < 2) rows[found - 1] = *r;
    }
    if (found != 2) for (size_t i = 0; compiled && i < compiled->resource_count; i++)
        if (compiled->resources[i].type && !_stricmp(compiled->resources[i].type, "snapeditorentitydef"))
            fprintf(stderr, "  row %s at %s\n", compiled->resources[i].name, compiled->resources[i].original_path);
    CHECK(found == 2); if (found != 2) goto done;
    shaped.sources = compiled->sources; shaped.resources = rows; shaped.resource_count = 2;
    live[0].name = rows[0].name; live[1].name = rows[1].name;
    rl_live[5] = &native; rl_live[6] = &live[0]; rl_live[7] = &live[1];
    sh_decl_server_test_rows_reset(); sh_decl_server_test_rows_bindings(sh_package_runtime_bindings());
    sh_decl_server_test_set_reconstruct(NULL, (sh_decl_server_test_find_decl_fn)rl_peek);
    sh_decl_server_test_rows_manager(&rl_manager, rl_source);
    CHECK(sh_decl_server_test_rows_record(&shaped, 2));
    for (int i = 0; i < 2; i++) {
        CHECK(pe_native_classify(NULL, &live[i], identity[i], sizeof(identity[i])) == 1);
        CHECK(sh_decl_server_row_exact(&live[i], linked[i], sizeof(linked[i])) && strcmp(linked[i], identity[i]));
        CHECK(sh_decl_server_row_supply(&live[i], &supply[i]) == 1 && supply[i].binding_count == 1);
    }
    CHECK(pe_native_classify(NULL, &native, NULL, 0) == 0);
    CHECK(!strcmp(identity[0], identity[1]) && strcmp(linked[0], linked[1]));
    if (supply[1].binding_count == 1 && supply[1].bindings[0].context_count == 2) { shared = 1; variant = 0; }
    CHECK(supply[shared].binding_count == 1 && supply[shared].bindings[0].context_count == 2);
    CHECK(supply[variant].binding_count == 1 && supply[variant].bindings[0].context_count == 1);
    if (g_failed != before) goto done;
    fprintf(stderr, "provider rows (%s; %s,%s,%s): shared %s linked %s / variant %s linked %s; identity %s\n",
        element ? "row icon field" : "entity target", names[0], names[1], names[2],
        rows[shared].name, linked[shared], rows[variant].name, linked[variant], identity[0]);
    snprintf(portable, 80, "%s", identity[0]);
    {   /* Owners: entity/0 selected a's context, entity/1 c's, entity/2 b's,
         * entity/3 c's. Records: 0 an earlier map's token of the shared row,
         * 1 and 2 the portable identity, 3 the variant's earlier token. */
        const sh_package_provenance_context *ctx[3] = {
            supply[shared].contexts + supply[shared].bindings[0].contexts[0],
            supply[variant].contexts + supply[variant].bindings[0].contexts[0],
            supply[shared].contexts + supply[shared].bindings[0].contexts[1]};
        static const char *const bound[3] = {"smp/a", "smp/variant", "smp/c"};
        size_t index[3]; const int owner_ctx[4] = {0, 2, 1, 2};
        const char *records[4] = {linked[shared], identity[0], identity[0], linked[variant]};
        for (int i = 0; i < 3; i++) {
            index[i] = sh_package_provenance_context_add(&uses, ctx[i]->mode, (const unsigned char (*)[32])ctx[i]->trees,
                ctx[i]->tree_count, ctx[i]->component ? ctx[i]->component : "");
            CHECK(index[i] != SIZE_MAX);
            CHECK(sh_package_provenance_bind(&uses, "entitydef", "player/coop/player_start", bound[i], index + i, 1));
        }
        for (int o = 0; o < 4; o++) {
            char owner[16]; sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, owner, "/m7:inherit"};
            snprintf(owner, sizeof(owner), "entity/%d", o);
            CHECK(sh_package_provenance_use_set(&uses, &site, (size_t)owner_ctx[o], index + owner_ctx[o], 1));
        }
        /* The exact authored deliveries the compiler retained for a, b and c. */
        for (size_t p = 0; p < compiled->sources->package_count && p < 3; p++, bundle_count++) {
            bundles[p].original = sh_package_runtime_source_bundle(p, compiled->sources->fingerprints[p]);
            CHECK(bundles[p].original);
            memcpy(bundles[p].compiler_tree, compiled->sources->fingerprints[p], 32);
            snprintf(bundles[p].source_name, sizeof(bundles[p].source_name), "%s", names[p]);
        }
        document = sh_package_occurrence_prepare_document(&uses, bundles, bundle_count, error, sizeof(error));
        if (!document) fprintf(stderr, "provider rows document: %s\n", error);
        CHECK(document && sh_package_occurrence_document_rows(document, records, 4));
    }
    g_tracker = sh_package_occurrence_tracker_create(); CHECK(g_tracker);
    sh_package_editor_bind(NULL, NULL); sh_package_editor_bind_rows(&api);
    g_copy_ctor = g_copy_assign = g_move_assign = g_flat_assign = nr_copy; g_body_dtor = g_flat_dtor = nr_destroy;
    g_clone = nr_clone; g_from = nr_from;
    *(void **)(map + 0x20) = objects; *(int *)(map + 0x28) = 4;
    CHECK(document && sh_package_editor_attach(SH_PACKAGE_EDITOR_MAP, map, document, error, sizeof(error)));
    if (g_failed != before) goto done;
    sh_package_editor_rows_retire(); /* reopened: every record re-resolves by name */
    /* An earlier map's record restores its exact row under its own supplier,
     * whatever the tie order, and capture records the portable identity. */
    nr_fill(&native, &live[variant], &live[shared]);
    CHECK(nr_build(objects, record, &pending) == &live[shared] && !pending && !strcmp(record, identity[0]));
    CHECK(nr_record(PE_FLAT, objects) && !strcmp(nr_record(PE_FLAT, objects), identity[0]));
    /* Equal physical row, separate owner: the portable record restores the
     * same decl under c's supplier. */
    sh_package_editor_rows_retire(); nr_fill(&live[shared], &native, &live[variant]);
    CHECK(nr_build(objects + 0x1b0, record, &pending) == &live[shared] && !pending && !strcmp(record, identity[0]));
    /* The variant's owner takes the variant, never the shared row it ties with. */
    CHECK(nr_build(objects + 0x360, record, &pending) == &live[variant] && !pending && !strcmp(record, identity[0]));
    /* The variant's earlier token under another owner's supplier (c): refused,
     * built native, the record kept unresolved. */
    CHECK(nr_build(objects + 0x510, record, &pending) == &native && pending && !strcmp(record, linked[variant]));
    /* Wrong row only: the variant's owner keeps its choice unresolved, built
     * native, and never authors the shared row. */
    sh_package_editor_rows_retire(); nr_fill(&live[shared], &native, NULL);
    CHECK(nr_build(objects + 0x360, record, &pending) == &native && pending && !strcmp(record, identity[0]));
    CHECK(nr_record(PE_FLAT, objects + 0x360) && !strcmp(nr_record(PE_FLAT, objects + 0x360), identity[0]));
    /* Retail runs production declaration mode: stock rows have no development
     * source record, and every row here restates the stock key. The native
     * resource index alone proves the stock row native, so an unavailable
     * choice builds the stock row, never the first private alias. */
    rl_sources = 0; sh_decl_server_test_rows_production(nr_native_index);
    CHECK(sh_decl_server_row_class(&native, NULL, 0) == 0 &&
          !strcmp(nr_index_path, "generated/decls/snapeditorentitydef/player/start.decl"));
    CHECK(sh_decl_server_row_class(&live[shared], NULL, 0) == 1 && sh_decl_server_row_class(&live[variant], NULL, 0) == 1);
    sh_package_editor_rows_retire(); nr_fill(&live[shared], &native, NULL); /* wrong row only, first */
    CHECK(nr_build(objects + 0x360, record, &pending) == &native && pending && !strcmp(record, identity[0]));
    sh_package_editor_rows_retire(); nr_fill(&live[shared], &native, &live[variant]);
    CHECK(nr_build(objects + 0x510, record, &pending) == &native && pending && !strcmp(record, linked[variant]));
    CHECK(nr_build(objects + 0x360, record, &pending) == &live[variant] && !pending && !strcmp(record, identity[0]));
    /* The lineage persists across publications: wrong row only, then both. */
    CHECK(nr_republish(&shaped, rows + shared, 1));
    nr_fill(&live[shared], &native, NULL);
    CHECK(nr_build(objects + 0x360, record, &pending) == &native && pending && !strcmp(record, identity[0]));
    CHECK(nr_republish(&shaped, rows, 2) && sh_decl_server_test_rows_count() == 2);
    nr_fill(&live[shared], &native, &live[variant]);
    CHECK(nr_build(objects + 0x360, record, &pending) == &live[variant] && !pending && !strcmp(record, identity[0]));
    /* No positive proof (absent from the index, or unreadable): unknown,
     * whatever source records say; the choice stays unresolved. */
    for (int proof = 0; proof < 2; proof++) {
        nr_index = proof ? -1 : 0; rl_sources = 1;
        CHECK(sh_decl_server_row_class(&native, NULL, 0) == -1);
        sh_package_editor_rows_retire(); nr_fill(&live[shared], &native, NULL);
        CHECK(nr_build(objects + 0x360, record, &pending) == &live[shared] && pending && !strcmp(record, identity[0]));
    }
    nr_index = 1; sh_decl_server_test_rows_production(NULL);
    sh_package_editor_rows_retire(); nr_fill(&live[shared], &native, NULL);
    CHECK(nr_build(objects + 0x360, record, &pending) == &native && pending && !strcmp(record, identity[0]));
    /* Saved: each owner's own choice; the shared row keeps its two owners. */
    exported = sh_package_editor_snapshot(SH_PACKAGE_EDITOR_MAP, map, error, sizeof(error));
    CHECK(exported);
    if (exported) {
        const sh_package_provenance *value = sh_package_occurrence_provenance(exported);
        for (int o = 0; o < 4; o++) {
            char owner[16]; const char *row = NULL;
            snprintf(owner, sizeof(owner), "entity/%d", o);
            CHECK(sh_package_provenance_row_get(value, owner, &row) == 1);
            CHECK(row && !strcmp(row, o == 3 ? linked[variant] : identity[0]));
        }
        CHECK(value && value->context_count == 3 && value->use_count == 4);
    }
    {   /* Unowned, never-saved objects. A selection keeps its exact publication
         * in this process only: it restores across retirement and reorder,
         * travels with copy, move and clone, is refused while only a dependency
         * variant carries its portable identity, and under a later owner wins
         * only through that owner's supplier. Native-only content gains no
         * record or ownership. */
        static unsigned char loose[4 * 0x1b0];
        unsigned char *copied = loose + 0x1b0, *cloned = loose + 0x360, *plain_flat = loose + 0x510;
        sh_compiled_resource reversed[2];
        uintptr_t body[2] = {0}, moved[2] = {0};
        const void *placed = &live[shared], *plain = &native, *other = &live[variant];
        sh_package_occurrence_snapshot *held;
        size_t row_base = g_row_count, rows_now, lineage_base;
        int loose_pending;
        reversed[0] = rows[1]; reversed[1] = rows[0];
        sh_package_editor_rows_retire();
        pe_row_capture(PE_FLAT, loose, &placed); /* placed from the shared row's palette entry */
        pe_row_capture(PE_FLAT, plain_flat, &plain); /* placed from the native row */
        CHECK(nr_record(PE_FLAT, loose) && !strcmp(nr_record(PE_FLAT, loose), identity[0]));
        CHECK(nr_exact(PE_FLAT, loose) && !strcmp(nr_exact(PE_FLAT, loose), linked[shared]));
        CHECK(!nr_record(PE_FLAT, plain_flat) && g_row_count == row_base + 1);
        /* Copied; constructed with the variant first, moved, then cloned. */
        pe_flat_assign(copied, loose);
        nr_fill(&live[variant], &live[shared], &native);
        pe_from(nr_palette, NULL, loose, body);
        CHECK(body[0] == (uintptr_t)&live[shared] && !pe_row_pending(PE_BODY, body));
        pe_move_assign(moved, body); moved[0] = body[0]; body[0] = 0;
        CHECK(!nr_record(PE_BODY, body) && nr_exact(PE_BODY, moved) && !strcmp(nr_exact(PE_BODY, moved), linked[shared]));
        pe_clone(moved, cloned, 0); pe_body_dtor(moved); pe_body_dtor(body);
        for (int k = 0; k < 3; k++) {
            const char *exact = nr_exact(PE_FLAT, loose + k * 0x1b0);
            CHECK(exact && !strcmp(exact, linked[shared]) && (!k || exact != nr_exact(PE_FLAT, loose)));
        }
        CHECK(g_row_count == row_base + 3);
        /* Retired, republished in reverse order: each restores its exact row
         * whatever the palette order; the native placement stays native. */
        CHECK(nr_publish(&shaped, reversed, 2));
        nr_fill(&live[variant], &native, &live[shared]);
        for (int k = 0; k < 3; k++) {
            CHECK(nr_build(loose + k * 0x1b0, record, &loose_pending) == &live[shared] && !loose_pending && !strcmp(record, identity[0]));
            CHECK(nr_exact(PE_FLAT, loose + k * 0x1b0) && !strcmp(nr_exact(PE_FLAT, loose + k * 0x1b0), linked[shared]));
        }
        CHECK(nr_build(plain_flat, record, &loose_pending) == &native && !loose_pending && !*record && !nr_record(PE_FLAT, plain_flat));
        held = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, plain_flat)); CHECK(!held); sh_package_occurrence_release(held);
        /* Only the variant carries the portable identity: each refuses, builds
         * native and keeps its exact choice; published again, it restores. */
        CHECK(nr_publish(&shaped, rows + variant, 1));
        nr_fill(&live[variant], &native, NULL);
        for (int k = 0; k < 3; k++) {
            CHECK(nr_build(loose + k * 0x1b0, record, &loose_pending) == &native && loose_pending && !strcmp(record, identity[0]));
            CHECK(nr_record(PE_FLAT, loose + k * 0x1b0) && !strcmp(nr_record(PE_FLAT, loose + k * 0x1b0), identity[0]));
            CHECK(nr_exact(PE_FLAT, loose + k * 0x1b0) && !strcmp(nr_exact(PE_FLAT, loose + k * 0x1b0), linked[shared]));
        }
        CHECK(nr_publish(&shaped, rows, 2));
        nr_fill(&live[variant], &native, &live[shared]);
        for (int k = 0; k < 3; k++)
            CHECK(nr_build(loose + k * 0x1b0, record, &loose_pending) == &live[shared] && !loose_pending && !strcmp(record, identity[0]));
        /* A published row whose exact identity cannot be read is unknown: the
         * previous record stays. */
        {
            sh_package_editor_rows_api none = api; none.exact = nr_exact_none;
            sh_package_editor_bind_rows(&none);
            pe_row_capture(PE_FLAT, loose, &other);
            CHECK(nr_exact(PE_FLAT, loose) && !strcmp(nr_exact(PE_FLAT, loose), linked[shared]));
            sh_package_editor_bind_rows(&api);
        }
        /* Owners attached later: a's supplier supplies the exact selection;
         * b's supplies only the variant and is refused. */
        held = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, objects));
        CHECK(held && sh_package_occurrence_set(g_tracker, pe_key(PE_FLAT, copied), held)); sh_package_occurrence_release(held);
        held = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, objects + 0x360));
        CHECK(held && sh_package_occurrence_set(g_tracker, pe_key(PE_FLAT, cloned), held)); sh_package_occurrence_release(held);
        sh_package_editor_rows_retire(); nr_fill(&live[variant], &native, &live[shared]);
        CHECK(nr_build(copied, record, &loose_pending) == &live[shared] && !loose_pending && !strcmp(record, identity[0]));
        CHECK(nr_build(cloned, record, &loose_pending) == &native && loose_pending && !strcmp(record, identity[0]));
        CHECK(nr_exact(PE_FLAT, cloned) && !strcmp(nr_exact(PE_FLAT, cloned), linked[shared]));
        /* Repeated republication keeps one record per object; destruction
         * returns records and ownership to baseline. */
        rows_now = g_row_count; lineage_base = sh_decl_server_test_rows_count();
        CHECK(rows_now == row_base + 3);
        for (int r = 0; r < 4; r++) {
            CHECK(nr_publish(&shaped, r & 1 ? reversed : rows, 2));
            nr_fill(&live[variant], &native, &live[shared]);
            CHECK(nr_build(loose, record, &loose_pending) == &live[shared] && !loose_pending);
            CHECK(g_row_count == rows_now && sh_decl_server_test_rows_count() == lineage_base);
        }
        /* An owner whose selection supplies no row for this key is no source
         * for it: an editor-only package the map never depends on. In
         * production declaration mode a captured exact choice keeps its
         * identity across publications, owned or not: wrong-only it builds the
         * native original and stays pending; a retired declaration is never
         * current, even if a palette still held it; restored, it is the same
         * row again whatever the order. Persisted portable and legacy records
         * under that owner, and an owner selecting another row for the key,
         * stay unavailable. */
        {
            static unsigned char spare[3 * 0x1b0];
            sh_package_provenance none = {0};
            sh_package_occurrence_snapshot *unrelated = NULL;
            const sh_package_provenance_context *own = supply[variant].contexts + supply[variant].bindings[0].contexts[0];
            size_t at = sh_package_provenance_context_add(&none, own->mode, (const unsigned char (*)[32])own->trees,
                own->tree_count, "unrelated/gameplay");
            sh_package_provenance_site site = {SH_PACKAGE_USE_OBJECT, "entity/9", "/m8:inherit"};
            CHECK(at != SIZE_MAX && sh_package_provenance_bind(&none, "entitydef", "unrelated/gameplay", "smp/gameplay", &at, 1) &&
                  sh_package_provenance_use_set(&none, &site, at, &at, 1));
            sh_package_occurrence_prepare_entity(&none, 9, bundles, bundle_count, &unrelated, error, sizeof(error));
            if (!unrelated) fprintf(stderr, "unrelated owner document: %s\n", error);
            sh_package_provenance_free(&none);
            CHECK(unrelated);
            rl_sources = 0; sh_decl_server_test_rows_production(nr_native_index);
            CHECK(nr_publish(&shaped, rows, 2)); nr_fill(&live[variant], &native, &live[shared]);
            pe_row_capture(PE_FLAT, spare, &placed);
            AcquireSRWLockExclusive(&g_row_lock);
            CHECK(pe_row_store(PE_FLAT, spare + 0x1b0, &(pe_row){PE_FLAT, (uintptr_t)(spare + 0x1b0), &live[shared], g_row_generation, 0, identity[0], NULL}));
            CHECK(pe_row_store(PE_FLAT, spare + 0x360, &(pe_row){PE_FLAT, (uintptr_t)(spare + 0x360), &live[shared], g_row_generation, 0, linked[shared], NULL}));
            ReleaseSRWLockExclusive(&g_row_lock);
            for (int k = 0; k < 3; k++) CHECK(unrelated && sh_package_occurrence_set(g_tracker, pe_key(PE_FLAT, spare + k * 0x1b0), unrelated));
            sh_package_occurrence_release(unrelated);
            for (int order = 0; order < 2; order++) {
                sh_package_editor_rows_retire();
                if (order) nr_fill(&live[shared], &native, &live[variant]);
                CHECK(nr_build(spare, record, &loose_pending) == &live[shared] && !loose_pending && !strcmp(record, identity[0]));
                CHECK(nr_exact(PE_FLAT, spare) && !strcmp(nr_exact(PE_FLAT, spare), linked[shared]));
            }
            CHECK(nr_republish(&shaped, rows + variant, 1)); nr_fill(&live[variant], &native, NULL); /* wrong-only */
            CHECK(nr_build(spare, record, &loose_pending) == &native && loose_pending && !strcmp(record, identity[0]));
            CHECK(nr_exact(PE_FLAT, spare) && !strcmp(nr_exact(PE_FLAT, spare), linked[shared]));
            sh_package_editor_rows_retire(); nr_fill(&live[variant], &native, &live[shared]); /* retired, still listed */
            CHECK(nr_build(spare, record, &loose_pending) != &live[shared] && loose_pending && !strcmp(record, identity[0]));
            CHECK(nr_republish(&shaped, reversed, 2)); nr_fill(&live[variant], &native, &live[shared]); /* restored */
            CHECK(nr_build(spare, record, &loose_pending) == &live[shared] && !loose_pending && !strcmp(record, identity[0]));
            CHECK(nr_build(spare + 0x1b0, record, &loose_pending) == &native && loose_pending && !strcmp(record, identity[0]));
            CHECK(nr_build(spare + 0x360, record, &loose_pending) == &native && loose_pending && !strcmp(record, linked[shared]));
            CHECK(!nr_exact(PE_FLAT, spare + 0x1b0) && !nr_exact(PE_FLAT, spare + 0x360));
            CHECK(nr_build(cloned, record, &loose_pending) == &native && loose_pending && !strcmp(record, identity[0]));
            CHECK(nr_exact(PE_FLAT, cloned) && !strcmp(nr_exact(PE_FLAT, cloned), linked[shared]));
            for (int k = 0; k < 3; k++) pe_flat_dtor(spare + k * 0x1b0);
            CHECK(g_row_count == rows_now);
            rl_sources = 1; sh_decl_server_test_rows_production(NULL);
            CHECK(nr_publish(&shaped, reversed, 2)); nr_fill(&live[variant], &native, &live[shared]);
        }
        /* Allocation failing after the native operation. Capturing the same
         * choice updates in place and cannot fail. A changed choice that
         * cannot be recorded, or a copy or clone whose record cannot be made,
         * leaves the object lost: neither its stale record nor none, never
         * captured or exported, and no construction substitutes a row. */
        {
            uintptr_t source[2] = {(uintptr_t)&live[variant], 0};
            pe_alloc_fail_after = 0;
            pe_row_capture(PE_FLAT, loose, &placed);
            CHECK(!g_faulted && !nr_lost(PE_FLAT, loose) && !pe_row_pending(PE_FLAT, loose));
            CHECK(!strcmp(nr_record(PE_FLAT, loose), identity[0]) && !strcmp(nr_exact(PE_FLAT, loose), linked[shared]));
            pe_row_capture(PE_FLAT, loose, &other); /* the variant replaced it natively */
            pe_alloc_fail_after = -1;
            CHECK(nr_faulted() && nr_lost(PE_FLAT, loose) && g_row_count == rows_now);
            nr_fill(&live[variant], &native, &live[shared]);
            CHECK(nr_build(loose, record, &loose_pending) == &native && loose_pending && !*record && nr_lost(PE_FLAT, loose));
            CHECK(nr_publish(&shaped, rows + variant, 1));
            nr_fill(&live[variant], NULL, NULL); /* engine order alone would build and capture the variant */
            nr_build(loose, record, &loose_pending);
            CHECK(loose_pending && !*record && nr_lost(PE_FLAT, loose));
            nr_fill(&live[variant], &native, NULL);
            CHECK(nr_build(loose, record, &loose_pending) == &native && loose_pending && nr_lost(PE_FLAT, loose));
            CHECK(nr_publish(&shaped, rows, 2));
            nr_fill(&live[variant], &native, &live[shared]);
            pe_row_capture(PE_FLAT, loose, &placed); /* reselected: recorded again */
            CHECK(!nr_lost(PE_FLAT, loose) && !strcmp(nr_exact(PE_FLAT, loose), linked[shared]));
            CHECK(nr_build(loose, record, &loose_pending) == &live[shared] && !loose_pending && !strcmp(record, identity[0]));
            /* Copy: the destination's shared record is stale once the variant
             * selection is copied over it. */
            pe_row_capture(PE_FLAT, plain_flat, &other);
            CHECK(nr_exact(PE_FLAT, plain_flat) && !strcmp(nr_exact(PE_FLAT, plain_flat), linked[variant]));
            CHECK(nr_exact(PE_FLAT, copied) && !strcmp(nr_exact(PE_FLAT, copied), linked[shared]));
            pe_alloc_fail_after = 0;
            pe_flat_assign(copied, plain_flat);
            pe_alloc_fail_after = -1;
            CHECK(nr_faulted() && nr_lost(PE_FLAT, copied) && !nr_lost(PE_FLAT, plain_flat));
            /* Clone: a body holding the variant serialized over the shared one. */
            pe_row_capture(PE_BODY, source, source);
            CHECK(nr_exact(PE_BODY, source) && !strcmp(nr_exact(PE_BODY, source), linked[variant]));
            pe_alloc_fail_after = 0;
            pe_clone(source, cloned, 0);
            pe_alloc_fail_after = -1;
            CHECK(nr_faulted() && nr_lost(PE_FLAT, cloned));
            pe_body_dtor(source);
            sh_package_editor_rows_retire(); nr_fill(&live[variant], &native, &live[shared]);
            CHECK(nr_build(copied, record, &loose_pending) == &native && loose_pending && nr_lost(PE_FLAT, copied));
            CHECK(nr_build(cloned, record, &loose_pending) == &native && loose_pending && nr_lost(PE_FLAT, cloned));
            pe_row_capture(PE_FLAT, plain_flat, &plain);
            CHECK(!nr_record(PE_FLAT, plain_flat) && !nr_record(PE_BODY, source) && g_row_count == rows_now);
        }
        for (int k = 0; k < 4; k++) pe_flat_dtor(loose + k * 0x1b0);
        CHECK(g_row_count == row_base);
        for (int k = 0; k < 4; k++) {
            held = sh_package_occurrence_get(g_tracker, pe_key(PE_FLAT, loose + k * 0x1b0));
            CHECK(!held); sh_package_occurrence_release(held);
        }
    }
done:
    sh_package_occurrence_release(exported); sh_package_occurrence_release(document);
    sh_package_editor_bind_rows(NULL);
    g_copy_ctor = g_copy_assign = g_move_assign = g_flat_assign = NULL; g_body_dtor = g_flat_dtor = NULL;
    g_clone = NULL; g_from = NULL; /* never installed: nothing to unpatch */
    if (g_tracker) CHECK(sh_package_editor_uninstall());
    for (size_t p = 0; p < bundle_count; p++) sh_package_bundle_release(bundles[p].original);
    for (int i = 0; i < 2; i++) sh_package_provenance_free(&supply[i]);
    sh_package_provenance_free(&uses);
    if (compiled) sh_package_runtime_release();
    rl_live[5] = rl_live[6] = rl_live[7] = NULL; rl_sources = 1; nr_index = 1;
    sh_decl_server_test_rows_production(NULL);
    sh_decl_server_test_rows_bindings(NULL); sh_decl_server_test_rows_manager(NULL, NULL);
    sh_decl_server_test_set_reconstruct(NULL, NULL); sh_decl_server_test_rows_reset();
    sh_package_runtime_test_installed_links(0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(runtime_original, NULL);
    for (size_t i = 0; i < fixture.count; i++) free(fixture.regions[i].address);
    CHECK(inventory_remove_tree(root));
}
int main(void)
{
    static const char *const authored[3] = {"a", "b", "c"}, *const reordered[3] = {"y", "a", "x"};
    char order[2][80] = {{0}}, element[80] = "";
    test_provider_editor_rows(0, authored, order[0]);
    test_provider_editor_rows(0, reordered, order[1]); /* the variant now publishes first */
    CHECK(*order[0] && !strcmp(order[0], order[1]));
    test_provider_editor_rows(1, authored, element);
    CHECK(*element && strcmp(element, order[0]));
    if (g_failed) { fprintf(stderr, "%d provider editor row check(s) failed\n", g_failed); return 1; }
    puts("provider editor rows tests passed");
    return 0;
}
