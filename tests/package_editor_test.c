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
int main(void)
{
    char temporary[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "ped", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    make_sources(); native_transfers(); atomic_hook_install();
    release_source_rows(); cleanup();
    printf("package_editor_test: %d failures\n", failures); return failures ? 1 : 0;
}
