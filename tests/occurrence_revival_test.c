/* M115: a prefab's plan-bearing additive admission through the production
 * decl_server path. The composition is the real older/newer shared-parent one
 * (its helpers are copied from package_runtime_context_test.c; the admitted
 * bodies are pinned by hash in both tests), the resident layer is the real
 * one, and the native declaration world parses whatever the published table
 * holds at the moment of each read, so no parse can silently use old bytes. */
#define SH_DECL_SERVER_REAL_RESIDENT
#define main decl_server_fixture_main
#include "decl_server_test.c"
#undef main
#include "../src/backend/resource_resident.c"
#include "map_provenance.h"
#include "package_sources.h"
#include "package_bundle.h"
#include "package_occurrence.h"
#include "m116_retained_fixtures.h"
#include "m117_retained_fixtures.h"
#include "map_shards.h"
#include "resource_catalog.h"
#include "native_reflection_replay.h"
#include "package_native_assets.h"
#include "package_archive.h"
#include "raw_deflate.h"
#include "retained_stock_closure.h"

/* Neither declaration class here derives idDeclTypeInfo. */
int sh_typeinfo_class_derives(const char *name, const char *base) { (void)name; (void)base; return 0; }
static char root[MAX_PATH];
/* This process's private root. A child adopts the new root its parent
 * allocated (RV_ROOT_VARIABLE) and the parent retires it after the child
 * exits: the package runtime keeps its snapshot bundles open, delete-on-close,
 * for the process lifetime, so the tree cannot be removed from inside (R-309).
 * A process that allocated its own root removes it when it ends. */
#define RV_ROOT_VARIABLE "OCCURRENCE_REVIVAL_ROOT"
static char g_rv_root[MAX_PATH];
static int g_rv_adopted;
/* Each case's private root is a new directory under a short unique temp name,
 * as the suite's other tests allocate theirs. GetTempFileNameA passes over the
 * names earlier runs' preserved roots hold, and the root must then be created
 * here: an existing directory is never accepted. The previous name, derived
 * from the process ID, met a preserved root once that ID was reused (R-307).
 * A nonzero unique fixes the name, for the collision control. */
static int rv_new_root(char *path, size_t capacity, unsigned unique)
{
    char temp[MAX_PATH], name[MAX_PATH];
    *path = 0;
    if (!GetTempPathA(sizeof(temp), temp) || !GetTempFileNameA(temp, "orv", unique, name) ||
        (!unique && !DeleteFileA(name)) || !CreateDirectoryA(name, NULL)) return 0;
    for (char *c = name; *c; c++) if (*c == '\\') *c = '/';
    snprintf(path, capacity, "%s", name);
    return 1;
}
static int rv_case_root(char *path, size_t capacity)
{
    int made;
    if (g_rv_adopted) { snprintf(path, capacity, "%s", g_rv_root); return 1; }
    made = rv_new_root(path, capacity, 0);
    if (!made) fprintf(stderr, "occurrence_revival_test: no new private root (error %lu)\n", GetLastError());
    else snprintf(g_rv_root, sizeof(g_rv_root), "%s", path);
    return made;
}
/* Remove a private root completely, naming every entry that cannot be
 * removed and its Windows error. Unlike inventory_remove_tree it does not
 * stop at the first failure, so a report is complete. */
static int rv_remove_root(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    int removed = 1;
    snprintf(pattern, sizeof(pattern), "%s/*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
            snprintf(child, sizeof(child), "%s/%s", path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removed &= rv_remove_root(child);
            else if (!DeleteFileA(child)) {
                fprintf(stderr, "occurrence_revival_test: cannot remove %s (error %lu)\n", child, GetLastError());
                removed = 0;
            }
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    if (!RemoveDirectoryA(path)) {
        fprintf(stderr, "occurrence_revival_test: cannot remove %s (error %lu)\n", path, GetLastError());
        removed = 0;
    }
    return removed;
}
static void create(const char *path, const char *body)
{
    char full[MAX_PATH], *slash;
    HANDLE file;
    DWORD written = 0;
    snprintf(full, sizeof(full), "%s/%s", root, path);
    for (slash = strchr(full + strlen(root) + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0; CreateDirectoryA(full, NULL); *slash = '/';
    }
    file = CreateFileA(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    CHECK(WriteFile(file, body, (DWORD)strlen(body), &written, NULL) && written == strlen(body));
    CloseHandle(file);
}
static int m109_activate(void *context, int restoring, const sh_package_changes *changes, char *error, size_t capacity);
#define INLINE_VARIABLES "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},\"entities\":["
/* ---- Copied verbatim from package_runtime_context_test.c: the native type metadata
 * readers, the stock baseline and the m113 shared-parent composition. ---- */
/* Native metadata for a small typed declaration family. The admission walk
 * reads these registrations, never a locally loaded declaration's state. */
typedef struct admission_region { void *address; size_t length; } admission_region;
typedef struct admission_fixture {
    admission_region regions[128]; size_t count;
    void *registry, *reflection;
} admission_fixture;
static void *admission_alloc(admission_fixture *f, size_t length)
{
    void *p = calloc(1, length); CHECK(p && f->count < 128);
    if (p && f->count < 128) f->regions[f->count++] = (admission_region){p, length};
    return p;
}
static void admission_pointer(void *p, size_t at, const void *value)
{ uintptr_t address = (uintptr_t)value; memcpy((char *)p + at, &address, 8); }
static void admission_number(void *p, size_t at, int value)
{ memcpy((char *)p + at, &value, 4); }
static char *admission_string(admission_fixture *f, const char *text)
{ char *copy = admission_alloc(f, strlen(text) + 1); if (copy) strcpy(copy, text); return copy; }
static int admission_read(void *context, uintptr_t address, void *out, size_t length)
{
    admission_fixture *f = context;
    for (size_t i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].address;
        if (address >= start && address - start <= f->regions[i].length &&
            length <= f->regions[i].length - (size_t)(address - start)) {
            memcpy(out, (void *)address, length); return 1;
        }
    }
    return 0;
}
static const char *const library_editor_rows[] = {"inlinestock/row_base", "inlinestock/base_row"};
static int library_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    const char *text = !strcmp(path, "generated/decls/entitydef/inlinestock/base.decl") ?
            "{\n\tclass = \"Entity\";\n\teditorVars {\n\t\tplaceable = true;\n\t\tcolor = ( 1, 1, 1, 1 );\n\t}\n"
            "\tedit = {\n\t\ttarget = \"\";\n\t\thealth = 1;\n\t}\n}\n" :
        /* The stock world SnapMap play inherits names no AI spawn pool (R-203). */
        !strcmp(path, "generated/decls/entitydef/worldspawn.decl") ?
            "{ class = \"idWorldspawn\"; edit = { globalAIsettings = \"default\"; } }" :
        /* M108: the stock meaning of the spelling the killer package also defines. */
        !strcmp(path, "generated/decls/entitydef/kill/damage.decl") ? "{ class = \"Entity\"; edit = { health = 3; } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/row_base.decl") ?
            "{\n\tedit = {\n\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/base_row.decl") ?
            "{\n\tinherit = \"inlinestock/row_base\";\n\tedit = {\n\t\tentityDef = \"inlinestock/base\";\n\t}\n}\n" :
        /* A stock definition with no state, and its shipped row (R-212). */
        !strcmp(path, "generated/decls/entitydef/inlinestock/empty.decl") ? "{\n\tclass = \"Entity\";\n}\n" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/empty_row.decl") ?
            "{\n\tinherit = \"inlinestock/row_base\";\n\tedit = {\n\t\tentityDef = \"inlinestock/empty\";\n\t}\n}\n" : NULL;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* M108: lease_collection_setup plus an actual-reader class pair. Entity does
 * not reflect damageType; idTestKillAction reads it as an entity declaration
 * and idTestDamageText as an int, so publication leaves it unbound. */
static void reader_collection_setup(admission_fixture *f)
{
    const char *classes[] = {"idDeclEntityDef", "Entity", "idDeclTypeInfo", "int",
        "idDeclSnapEditorEntity", "idList < EntityRefs >", "idDeclTypeInfoGraph", "idTestKillAction", "idTestDamageText"};
    const char *types[] = {"entityDef", "snapEditorEntityDef"};
    const size_t indices[] = {0, 4};
    memset(f, 0, sizeof(*f));
    f->registry = admission_alloc(f, 0x18);
    void *managers = admission_alloc(f, 16);
    admission_pointer(f->registry, 8, managers); admission_number(f->registry, 0x10, 2);
    for (size_t i = 0; i < 2; i++) {
        void *manager = admission_alloc(f, 0x90); admission_pointer(managers, i * 8, manager);
        admission_pointer(manager, 8, admission_string(f, types[i]));
        admission_pointer(manager, 0x10, admission_string(f, classes[indices[i]]));
    }
    f->reflection = admission_alloc(f, 0xb0);
    void *container = admission_alloc(f, 0x30), *records = admission_alloc(f, 10 * 0x38);
    void *objects = admission_alloc(f, 10 * 16), *pointers = admission_alloc(f, 10 * 16);
    admission_pointer(f->reflection, 0, container); admission_pointer(container, 0x20, records);
    admission_pointer(f->reflection, 0x88, objects); admission_number(f->reflection, 0x90, 10);
    admission_pointer(f->reflection, 0xa0, pointers); admission_number(f->reflection, 0xa8, 10);
    for (size_t i = 0; i < 9; i++) {
        admission_pointer(records, i * 0x38, admission_string(f, classes[i]));
        admission_pointer(records, i * 0x38 + 8, admission_string(f, i == 4 || i == 6 ? "idDeclTypeInfo" : i > 6 ? "Entity" : ""));
    }
    admission_pointer(pointers, 8, (void *)0x10); admission_pointer(pointers, 4 * 16 + 8, (void *)0x10);
    admission_pointer(objects, 5 * 16 + 8, (void *)0x80);
    admission_pointer(objects, 6 * 16 + 8, (void *)0x30);
    void *entity = admission_alloc(f, 3 * 0x48), *editor = admission_alloc(f, 3 * 0x48);
    void *element = admission_alloc(f, 0x48), *kill = admission_alloc(f, 2 * 0x48), *text = admission_alloc(f, 2 * 0x48);
    admission_pointer(records, 0x38 + 0x20, entity); admission_pointer(records, 4 * 0x38 + 0x20, editor);
    admission_pointer(records, 5 * 0x38 + 0x18, element);
    admission_pointer(records, 7 * 0x38 + 0x20, kill); admission_pointer(records, 8 * 0x38 + 0x20, text);
    void *rows[] = {entity, (char *)entity + 0x48, editor, element, (char *)editor + 0x48, kill, text};
    const char *field_types[] = {"idDeclEntityDef", "int", "idList < EntityRefs >", "idDeclEntityDef", "idDeclEntityDef",
        "idDeclEntityDef", "int"};
    const char *operators[] = {"*", "", "", "*", "*", "*", ""}, *names[] = {"target", "health", "inputs", "element",
        "entityDef", "damageType", "damageType"};
    for (size_t i = 0; i < 7; i++) {
        admission_pointer(rows[i], 0, admission_string(f, field_types[i]));
        admission_pointer(rows[i], 8, admission_string(f, operators[i]));
        admission_pointer(rows[i], 0x10, admission_string(f, names[i])); admission_number(rows[i], 0x1c, 8);
    }
}
#define M108_ENTITY(cls, edit) "{\"entityDef\":{\"className\":\"" cls "\",\"inherit\":\"kill/base\",\"name\":\"\"," \
    "\"state\":{\"edit\":{" edit "}},\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":26}}"
static void m113_shared_package(const char *base, const char *name, const char *health)
{
    char path[MAX_PATH], text[160];
    snprintf(path, sizeof(path), "%s/overrides/%s/package.json", base, name);
    snprintf(text, sizeof(text), "{\"id\":\"m113-%s\",\"name\":\"%s\"}", name, name); create(path, text);
    snprintf(path, sizeof(path), "%s/overrides/%s/assets/generated/decls/entitydef/kill/base.decl", base, name);
    create(path, "{ inherit = \"inlinestock/base\"; edit = { health = 5; damageType = \"kill/damage\"; } }");
    snprintf(path, sizeof(path), "%s/overrides/%s/assets/generated/decls/entitydef/kill/damage.decl", base, name);
    snprintf(text, sizeof(text), "{ class = \"Entity\"; edit = { health = %s; } }", health); create(path, text);
    snprintf(path, sizeof(path), "%s/overrides/%s/assets/generated/decls/snapeditorentitydef/kill/base_row.decl", base, name);
    create(path, "{ inherit = \"inlinestock/base_row\"; edit = { entityDef = \"kill/base\"; } }");
}
static char *m113_author(const char *data, const char *doc, size_t *length)
{
    char error[2048] = "", *bound, *prepared = NULL, *attached = NULL, *captured = NULL, *saved = NULL;
    size_t bound_length = 0, prepared_length = 0, n = 0, captured_length = 0, rows_count = 0;
    sh_package_map_plan *plan = NULL;
    sh_package_provenance carried = {0};
    sh_package_occurrence_snapshot *world = NULL, *save = NULL;
    int present = 0;
    *length = 0;
    bound = sh_package_runtime_prepare_local_document(data, doc, strlen(doc), &bound_length, &plan, error, sizeof(error));
    CHECK(bound && plan && sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, m109_activate, NULL));
    prepared = bound ? sh_map_provenance_strip(bound, bound_length, &prepared_length, error, sizeof(error)) : NULL;
    CHECK(prepared && sh_map_provenance_read(bound, bound_length, &carried, &present, error, sizeof(error)));
    attached = prepared ? sh_package_runtime_capture_document(NULL, present ? &carried : NULL, NULL, 0, prepared, prepared_length,
        &n, &world, NULL, NULL, error, sizeof(error)) : NULL;
    CHECK(attached && (!present || sh_package_occurrence_carry_rows(world, &carried)));
    const sh_package_bundle_binding *rows = world ? sh_package_occurrence_bundles(world, &rows_count) : NULL;
    captured = attached ? sh_package_runtime_capture_document(NULL, sh_package_occurrence_provenance(world), rows, rows_count,
        prepared, prepared_length, &captured_length, &save, NULL, NULL, error, sizeof(error)) : NULL;
    saved = captured ? sh_map_provenance_write(captured, captured_length, sh_package_occurrence_provenance(save), length,
        error, sizeof(error)) : NULL;
    if (!saved) fprintf(stderr, "m113 paste author: %s\n", error);
    CHECK(saved != NULL);
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, m109_activate, NULL));
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    if (prepared) HeapFree(GetProcessHeap(), 0, prepared);
    free(attached); free(captured); sh_package_map_plan_free(plan); sh_package_provenance_free(&carried);
    sh_package_occurrence_release(world); sh_package_occurrence_release(save);
    return saved;
}
/* The deliveries a carried map source root holds (all of its packages). */
static size_t m113_deliveries(const char *source_root, sh_package_bundle_binding rows[2])
{
    char error[512];
    size_t count = 0;
    sh_package_sources *sources = sh_package_sources_scan(source_root, error, sizeof(error));
    for (size_t p = 0; sources && p < sources->package_count && p < 2; p++, count++) {
        strcpy_s(rows[p].source_name, sizeof(rows[p].source_name), sources->packages[p].name);
        memcpy(rows[p].compiler_tree, sources->fingerprints[p], 32);
        rows[p].original = sh_package_bundle_capture(sources, p, source_root, error, sizeof(error));
    }
    sh_package_sources_free(sources);
    return count;
}
/* A saved document's ownership as an import captures it (prefab_import.c
 * pi_prepare): prepared against its own carried sources, rebound, then
 * captured with its carrier against that plan. *native is its native JSON. */
static sh_package_occurrence_snapshot *m113_import(const char *data, const char *source_root, const char *saved, size_t length,
    char **native, size_t *native_length, sh_package_map_plan **kept_plan)
{
    char error[2048] = "", *rebound = NULL, *stripped = NULL;
    size_t rebound_length = 0, stripped_length = 0, count;
    sh_package_bundle_binding rows[2] = {{0}};
    sh_package_provenance kept = {0}, carrier = {0};
    sh_package_occurrence_snapshot *ownership = NULL;
    int present = 0;
    *native = NULL; *native_length = 0; *kept_plan = NULL;
    count = m113_deliveries(source_root, rows);
    CHECK(count && sh_map_provenance_read(saved, length, &kept, &present, error, sizeof(error)) && present);
    sh_package_map_plan *plan = sh_package_runtime_prepare_map_document(data, source_root, &kept, rows, count, saved, length,
        error, sizeof(error));
    rebound = plan ? sh_package_map_plan_rebind(plan, &kept, saved, length, &rebound_length, error, sizeof(error)) : NULL;
    stripped = rebound ? sh_map_provenance_strip(rebound, rebound_length, &stripped_length, error, sizeof(error)) : NULL;
    CHECK(stripped && sh_map_provenance_read(rebound, rebound_length, &carrier, &present, error, sizeof(error)) && present);
    *native = stripped ? sh_package_runtime_capture_document(plan, &carrier, NULL, 0, stripped, stripped_length,
        native_length, &ownership, NULL, NULL, error, sizeof(error)) : NULL;
    if (!*native || !ownership) fprintf(stderr, "m113 paste import: %s\n", error);
    CHECK(*native && ownership && sh_package_occurrence_carry_rows(ownership, &carrier));
    if (rebound) HeapFree(GetProcessHeap(), 0, rebound);
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    *kept_plan = plan; sh_package_provenance_free(&kept); sh_package_provenance_free(&carrier);
    for (size_t i = 0; i < 2; i++) sh_package_bundle_release(rows[i].original);
    return ownership;
}
/* ---- End of the copied composition. ---- */

/* ---- The native declaration world. Managers keep the engine layout (type +8,
 * class +0x10, next +0x18, items +0x20, count +0x28); objects keep the resource
 * layout (name +8, manager +0x10, record +0x18, level +0x28, state +0x2c,
 * resolved entityDef +0x1c8). A parse reads the table published at that moment;
 * with no published bytes it completes at the engine default. ---- */
#define RV_OBJECTS 64
#define RV_PARSED 0x300  /* generation of the bytes the last parse read; -1 constructed, 0 default */
#define RV_BODY 0x308
#define RV_REBUILDS 0x310
#define RV_PARSES 0x314
#define RV_LOWEST 0x318  /* lowest generation any parse read since the admission began */
enum { RV_ENTITY, RV_EDITOR, RV_FOREIGN, RV_MANAGERS };
static const char *const rv_types[RV_MANAGERS] = {"entitydef", "snapeditorentitydef", "entitydef"};
static const char *const rv_classes[RV_MANAGERS] = {"idDeclEntityDef", "idDeclSnapEditorEntity", "idDeclEntityDef"};
static unsigned char rv_managers[RV_MANAGERS][0x90];
static void *rv_items[RV_MANAGERS][RV_OBJECTS];
static unsigned char rv_objects[RV_OBJECTS][0x600];
static size_t rv_object_count;
static void *rv_head, *rv_renderer_ptr, *rv_resource_table[12], *rv_render_table[35];
static unsigned char rv_renderer[0x20], rv_heap_object[0xd0];
static DWORD rv_thread;
/* rv_foreign: the decl server's entitydef manager is one the resident walk never
 * reaches. rv_fault_name: the next parse of that identity raises. */
static int rv_strings, rv_foreign;
static const char *rv_fault_name;
/* A retirement default that reads an entitydef as it is built (rv_default_read), as a hooked find records it;
 * RV_SEEN keeps the parse generation that read met (-2: nothing found). */
static unsigned char *rv_default_reader;
static const char *rv_default_read;
#define RV_SEEN 0x31c
typedef struct rv_source { char key[256]; unsigned char *body; size_t length; int generation; } rv_source;
static rv_source rv_table[64];
static size_t rv_table_count;
static int rv_generation;
static char rv_records[64][256];
static size_t rv_record_count;

static const char *rv_type_of(const unsigned char *object)
{ return *(const char **)(*(unsigned char *const *)(object + 0x10) + 8); }
static const char *rv_name_of(const unsigned char *object) { return *(const char *const *)(object + 8); }
static rv_source *rv_source_of(const char *type, const char *name)
{
    char key[256];
    snprintf(key, sizeof(key), "%s:%s", type, name);
    for (size_t i = 0; i < rv_table_count; i++) if (!_stricmp(rv_table[i].key, key)) return rv_table + i;
    return NULL;
}
/* Replaced bytes stay allocated: an object may still point at what it read. */
static void rv_publish(const char *type, const char *name, const unsigned char *body, size_t length)
{
    rv_source *source = rv_source_of(type, name);
    if (!source) {
        CHECK(rv_table_count < 64);
        if (rv_table_count >= 64) return;
        source = rv_table + rv_table_count++;
        snprintf(source->key, sizeof(source->key), "%s:%s", type, name);
    }
    source->body = malloc(length + 1);
    if (!source->body) return;
    memcpy(source->body, body, length); source->body[length] = 0;
    source->length = length; source->generation = ++rv_generation;
}
static int rv_merge(const sh_overrides_internal_decl_entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) rv_publish(entries[i].type, entries[i].name, entries[i].body, entries[i].body_length);
    return 1;
}
static int rv_recorded(const char *type, const char *name)
{
    char key[256];
    snprintf(key, sizeof(key), "%s:%s", type, name);
    for (size_t i = 0; i < rv_record_count; i++) if (!_stricmp(rv_records[i], key)) return 1;
    return 0;
}
static void rv_record(const char *type, const char *name)
{
    if (rv_recorded(type, name) || rv_record_count >= 64) return;
    snprintf(rv_records[rv_record_count++], sizeof(rv_records[0]), "%s:%s", type, name);
}
static int rv_manager_index(const void *manager)
{
    for (int i = 0; i < RV_MANAGERS; i++) if (manager == rv_managers[i]) return i;
    return -1;
}
static unsigned char *rv_object(int manager, const char *name)
{
    int count = *(int *)(rv_managers[manager] + 0x28);
    for (int i = 0; i < count; i++) if (!_stricmp(rv_name_of(rv_items[manager][i]), name)) return rv_items[manager][i];
    return NULL;
}
static unsigned char *rv_new(int manager, const char *name)
{
    int *count = (int *)(rv_managers[manager] + 0x28);
    unsigned char *object;
    CHECK(rv_object_count < RV_OBJECTS && *count < RV_OBJECTS);
    if (rv_object_count >= RV_OBJECTS || *count >= RV_OBJECTS) return NULL;
    object = rv_objects[rv_object_count++]; memset(object, 0, sizeof(rv_objects[0]));
    *(void ***)object = rv_resource_table; *(char **)(object + 8) = _strdup(name);
    *(void **)(object + 0x10) = rv_managers[manager]; *(unsigned int *)(object + 0x28) = RR_LEVEL_PERMANENT;
    *(int *)(object + RV_PARSED) = -1; *(int *)(object + RV_LOWEST) = INT_MAX;
    rv_items[manager][(*count)++] = object;
    return object;
}
static void *rv_find(void *manager, const char *name, unsigned char make_default);
static void rv_parse(unsigned char *object)
{
    const char *type = rv_type_of(object), *name = rv_name_of(object);
    const rv_source *source = rv_source_of(type, name);
    int generation = source ? source->generation : 0;
    CHECK(*(unsigned int *)(object + 0x28) == RR_LEVEL_PERMANENT && !(object[0x2c] & RR_PENDING));
    (*(int *)(object + RV_PARSES))++;
    if (*(int *)(object + RV_LOWEST) > generation) *(int *)(object + RV_LOWEST) = generation;
    if (rv_fault_name && !_stricmp(rv_fault_name, name)) { rv_fault_name = NULL; RaiseException(0xe0115001u, 0, 0, NULL); }
    *(void **)(object + 0x1c8) = NULL;
    *(int *)(object + RV_PARSED) = generation;
    *(const unsigned char **)(object + RV_BODY) = source ? source->body : NULL;
    if (!source) { object[0x2c] |= RR_DEFAULT; return; }
    object[0x2c] &= (unsigned char)~RR_DEFAULT;
    if (!_stricmp(type, "snapeditorentitydef")) {
        const char *at = strstr((const char *)source->body, "entityDef = \"");
        char target[160] = "";
        if (at) sscanf_s(at + 13, "%159[^\"]", target, (unsigned)sizeof(target));
        if (target[0]) *(void **)(object + 0x1c8) = rv_find(rv_managers[RV_ENTITY], target, 0);
    }
}
/* DeclFind: a pending object parses on lookup; a registered source with no
 * object is constructed and parsed even without make-default. */
static void *rv_find(void *manager, const char *name, unsigned char make_default)
{
    int index = rv_manager_index(manager);
    unsigned char *object;
    if (index < 0 || !name) return NULL;
    object = rv_object(index, name);
    if (object) {
        if (object[0x2c] & RR_PENDING) { object[0x2c] &= (unsigned char)~RR_PENDING; rv_parse(object); }
        return object;
    }
    if (!make_default && !rv_recorded(rv_types[index], name)) return NULL;
    object = rv_new(index, name);
    if (object) rv_parse(object);
    return object;
}
static void *rv_lookup(void *manager, const char *name) { return rv_find(manager, name, 0); }
static void rv_load(void *self) { rv_parse(self); }
static void rv_reconstruct(void *self)
{
    unsigned char *object = self;
    CHECK(*(unsigned int *)(object + 0x28) != RR_LEVEL_PERMANENT);
    object[0x2c] = 0; *(void **)(object + 0x18) = NULL; *(void **)(object + 0x1c8) = NULL;
    *(int *)(object + RV_PARSED) = -1; *(const unsigned char **)(object + RV_BODY) = NULL;
    (*(int *)(object + RV_REBUILDS))++;
}
static void rv_make_default(void *self)
{
    unsigned char *object = self;
    CHECK((object[0x2c] & RR_DEFAULT) && !(object[0x2c] & RR_PENDING));
    *(int *)(object + RV_PARSED) = 0; *(const unsigned char **)(object + RV_BODY) = NULL;
    *(void **)(object + 0x1c8) = NULL;
    if (object == rv_default_reader && rv_default_read) {
        unsigned char *read = rv_find(rv_managers[RV_ENTITY], rv_default_read, 0);
        *(int *)(object + RV_SEEN) = read ? *(int *)(read + RV_PARSED) : -2;
        if (sh_resource_graph_recording()) {
            if (read) sh_resource_graph_reference("entitydef", rv_default_read);
            else sh_resource_graph_request("entitydef", rv_default_read);
        }
    }
}
static void *rv_path_get(void *self, void *out, int mode)
{
    char path[512];
    if (mode) snprintf(path, sizeof(path), "unused");
    else snprintf(path, sizeof(path), "generated/decls/%s/%s.decl", rv_type_of(self), rv_name_of(self));
    memset(out, 0, 0x30); *(int *)((unsigned char *)out + 8) = (int)strlen(path);
    *(char **)((unsigned char *)out + 0x10) = _strdup(path); rv_strings++;
    return out;
}
static void rv_string_free(void *self) { free(*(void **)((unsigned char *)self + 0x10)); rv_strings--; }
static void *rv_heap_get(void) { return rv_heap_object; }
static void rv_heap_push(void *self, int index)
{
    int *depth = (int *)((unsigned char *)self + 0xc4);
    *(int *)((unsigned char *)self + 0x44 + 4 * (*depth)++) = index;
}
static void rv_heap_pop(void *self) { --*(int *)((unsigned char *)self + 0xc4); }
static int rv_mode(void) { return 3; }
static void rv_synchronize(void *self, unsigned char a, unsigned char b, unsigned char c)
{ (void)self; (void)a; (void)b; (void)c; }
static void rv_update(void *self) { (void)self; }
static void rv_suspend(void *self, int delta) { *(int *)((unsigned char *)self + 0x14) += delta; }
static void rv_rebind(void) { }
static void rv_material(void *material) { (void)material; }
static void rv_fonts(void *fonts) { (void)fonts; }
static void *rv_type_by_name(void *registry, const char *name)
{
    (void)registry;
    if (!_stricmp(name, "entitydef")) return rv_managers[rv_foreign ? RV_FOREIGN : RV_ENTITY];
    if (!_stricmp(name, "snapeditorentitydef")) return rv_managers[RV_EDITOR];
    return NULL;
}
static void *rv_source_find(void *manager, const char *name)
{
    int index = rv_manager_index(manager);
    return index >= 0 && rv_recorded(rv_types[index], name) ? (void *)rv_records : NULL;
}
/* Native DeclRegisterFile: records the source "<type>/<name>.decl" for its
 * manager, so a later lookup constructs and parses it. An admission never
 * registers a shadowed source (a revived shell) again. */
static unsigned char rv_register_file(void *registry, const void *source, void *manager)
{
    const char *path = *(const char *const *)source, *slash = path ? strchr(path, '/') : NULL;
    char type[64], name[192];
    size_t length = slash ? strlen(slash + 1) : 0;
    (void)registry; (void)manager;
    if (!slash || (size_t)(slash - path) >= sizeof(type) || length <= 5 || length - 5 >= sizeof(name)) return 0;
    memcpy(type, path, (size_t)(slash - path)); type[slash - path] = 0;
    memcpy(name, slash + 1, length - 5); name[length - 5] = 0;
    if (rv_recorded(type, name)) { CHECK(!"an admitted shadowed source registered again"); return 0; }
    rv_record(type, name); return 1;
}
static void *rv_ctor(void *self, const char *source) { *(const char **)self = source; return self; }
static void rv_dtor(void *self) { (void)self; }
static struct { unsigned char anchor[32]; void *registry; } rv_image;
static void *rv_vtable[12], *rv_registry;
static void rv_bind(void)
{
    int displacement = (int)((unsigned char *)&rv_image.registry - (rv_image.anchor + 0x17));
    rv_vtable[7] = (void *)rv_register_file; rv_vtable[11] = (void *)rv_type_by_name;
    rv_registry = rv_vtable; rv_image.registry = &rv_registry;
    memcpy(rv_image.anchor + 0x10, "\x48\x8b\x0d", 3); memcpy(rv_image.anchor + 0x13, &displacement, 4);
    sh_decl_server_test_bind_native(rv_image.anchor, rv_type_by_name, rv_register_file, rv_find,
        rv_source_find, rv_ctor, rv_dtor);
    sh_decl_server_test_set_generic_load(rv_load);
    sh_decl_server_test_set_reconstruct(rv_reconstruct, NULL);
    g_table_merge_hook = rv_merge;
}
static void rv_reset(void)
{
    memset(rv_managers, 0, sizeof(rv_managers)); memset(rv_items, 0, sizeof(rv_items));
    memset(rv_renderer, 0, sizeof(rv_renderer)); memset(rv_heap_object, 0, sizeof(rv_heap_object));
    rv_object_count = rv_table_count = rv_record_count = 0;
    rv_generation = rv_strings = rv_foreign = 0; rv_fault_name = NULL;
    rv_default_reader = NULL; rv_default_read = NULL;
    for (int i = 0; i < RV_MANAGERS; i++) {
        *(const char **)(rv_managers[i] + 8) = rv_types[i]; *(const char **)(rv_managers[i] + 0x10) = rv_classes[i];
        *(void ***)(rv_managers[i] + 0x20) = rv_items[i];
    }
    *(void **)(rv_managers[RV_ENTITY] + 0x18) = rv_managers[RV_EDITOR];
    rv_resource_table[0x38 / 8] = (void *)rv_path_get; rv_resource_table[0x50 / 8] = (void *)rv_make_default;
    rv_render_table[0x80 / 8] = (void *)rv_synchronize; rv_render_table[0x108 / 8] = (void *)rv_update;
    rv_render_table[0x110 / 8] = (void *)rv_suspend;
    *(void ***)rv_renderer = rv_render_table; rv_renderer_ptr = rv_renderer; rv_head = rv_managers[RV_ENTITY];
    rv_thread = GetCurrentThreadId();
    g_rr.head = &rv_head; g_rr.renderer = &rv_renderer_ptr; g_rr.thread = &rv_thread;
    g_rr.heap = (sh_process_heap_api){rv_heap_get, rv_heap_push, rv_heap_pop};
    g_rr.mode = rv_mode; g_rr.string_free = rv_string_free;
    g_rr.reconstruct = rv_reconstruct; g_rr.load = rv_load; g_rr.lookup = rv_lookup;
    g_rr.rebind = rv_rebind; g_rr.rebind_material = rv_material; g_rr.world_text_fonts = rv_fonts;
    while (g_rr_retirement_count) {
        rr_retirement *row = g_rr_retirements + --g_rr_retirement_count;
        free(row->type); free(row->name);
    }
    rr_free(g_rr_recovery); g_rr_recovery = NULL;
}
static int rv_retired(const void *object, const char *name)
{
    for (size_t i = 0; i < g_rr_retirement_count; i++)
        if (g_rr_retirements[i].resource == object && !_stricmp(g_rr_retirements[i].name, name)) return 1;
    return 0;
}

/* The map activation as the decl server's map pass meets the native world: the
 * activation's added bodies published and recorded, the production resident
 * pass over its complete changes (retiring what it removes), then each added
 * identity constructed and parsed. Registration is not under test here. */
static int m109_activate(void *context, int restoring, const sh_package_changes *changes, char *error, size_t capacity)
{
    const sh_package_compilation *compiled;
    sh_resource_resident *pass;
    int ok;
    (void)context;
    if (!changes) return 1;
    compiled = sh_package_runtime_acquire();
    for (size_t i = 0; compiled && i < changes->added_count; i++) {
        char path[512];
        const sh_compiled_resource *resource;
        snprintf(path, sizeof(path), "generated/decls/%s/%s.decl", changes->added[i].type, changes->added[i].name);
        resource = sh_package_compilation_find(compiled, path);
        if (!resource || !resource->body) continue;
        rv_publish(changes->added[i].type, changes->added[i].name, resource->body, resource->body_length);
        rv_record(changes->added[i].type, changes->added[i].name);
    }
    if (compiled) sh_package_runtime_release();
    pass = sh_resource_resident_begin(changes, restoring, 0, error, capacity);
    ok = pass && sh_resource_resident_reconstruct(pass, error, capacity) && sh_resource_resident_drain(pass, error, capacity);
    if (pass) ok = sh_resource_resident_end(pass, ok, error, capacity) && ok;
    if (!ok) { fprintf(stderr, "composition activation: %s\n", error); return 0; }
    for (size_t i = 0; i < changes->added_count; i++) {
        int manager = !_stricmp(changes->added[i].type, "entitydef") ? RV_ENTITY :
            !_stricmp(changes->added[i].type, "snapeditorentitydef") ? RV_EDITOR : -1;
        if (manager >= 0 && rv_recorded(changes->added[i].type, changes->added[i].name))
            rv_find(rv_managers[manager], changes->added[i].name, 1);
    }
    return 1;
}

typedef struct rv_hooks { int fail, prepared, finalized, rolled_back; } rv_hooks;
static int rv_prepare(void *context, char *error, size_t capacity)
{
    rv_hooks *hooks = context;
    hooks->prepared++;
    if (hooks->fail == 1) { snprintf(error, capacity, "fixture staging refused"); return 0; }
    return 1;
}
static int rv_finalize(void *context, char *error, size_t capacity)
{
    rv_hooks *hooks = context;
    hooks->finalized++;
    if (hooks->fail == 2) { snprintf(error, capacity, "fixture finalization refused"); return 0; }
    return 1;
}
static int rv_rollback(void *context, char *error, size_t capacity)
{ (void)error; (void)capacity; ((rv_hooks *)context)->rolled_back++; return 1; }
static uint64_t rv_fnv(const unsigned char *bytes, size_t length)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; i++) { hash ^= bytes[i]; hash *= 0x100000001b3ull; }
    return hash;
}

/* The identities the real composition's admission adds, and the FNV-1a 64 of
 * their exact bodies (package_runtime_context_test pins the same values). */
static const char rv_entity_name[] = "smp/9f8791aaf4dde1c286ab86af62b2700fb1b8fd7e53fa53f30c29e82a0987188a";
static const char rv_row_name[] = "smp/076ab7ac5022e5950542216144cce133d826616d070d32438ba270c765bb3ed2";
#define RV_ENTITY_BODY_HASH 0x003e49f0c0ea99e3ull
#define RV_ROW_BODY_HASH 0x354702695f26e95dull
enum { RV_VALID, RV_FAULT_PARSE, RV_FAULT_STAGING, RV_FAULT_FINALIZE, RV_LIVE_CONSUMER, RV_FOREIGN_DEFAULT,
    RV_WRONG_POINTER, RV_PRIOR_CONSUMER, RV_PRIOR_GUARD, RV_RETIRED_CONSUMER, RV_RETIRED_STAGING, RV_RETIRED_PARSE,
    RV_RETIRED_UNRELATED, RV_RETIRED_RECORD, RV_RETIRED_NEVER, RV_CASES };
static const char *const rv_case_names[RV_CASES] = {"valid", "parse fault", "staging fault", "finalization fault",
    "live consumer", "default never retired", "live object of another manager", "prior consumer after preparation",
    "consumer predating the admission", "retired default consumer", "retired default consumer, staging fault",
    "retired default consumer, parse fault", "retired default of another identity",
    "retired default with a native record", "default consumer this provider never retired"};
static const char *const rv_refusals[RV_CASES] = {NULL, NULL, NULL, NULL, "selected an existing native consumer",
    "never retired", "additive source already has a live object", NULL, NULL, NULL, NULL, NULL, NULL,
    "selected an existing native consumer", "selected an existing native consumer"};
/* R-349: a third entitydef this provider retired, whose retirement default asked for newer's variant (or, for
 * the unrelated control, another identity). The admission refreshes it: no refusal, its default rebuilt once
 * the variant's addition exists. A native record or a missing retirement row keeps it an existing consumer. */
static const char rv_consumer_name[] = "smp/retired-default-consumer";
static int rv_retired_case(int which) { return which >= RV_RETIRED_CONSUMER && which <= RV_RETIRED_NEVER; }
static int rv_refreshed_case(int which)
{ return which == RV_RETIRED_CONSUMER || which == RV_RETIRED_STAGING || which == RV_RETIRED_PARSE; }

/* M118 (R-274): live request 6. A pre-existing object of A's context resolved
 * the addition while the admission prepared it -- after the ownership check --
 * and the palette then refused. That edge is the admission's own change, so
 * recovery rebuilds the object from the restored provider (the gate-4 build
 * refused instead and left the admission to a later retry). Here the object
 * resolves the variant when the admission publishes its bytes. A consumer whose
 * edge predates the admission, deselected then by its tier, still refuses. */
static unsigned char *rv_prior;
static int rv_prior_raise;
static int rv_merge_resolved(const sh_overrides_internal_decl_entry *entries, size_t count)
{
    sh_resource_graph_frame frame;
    if (!rv_merge(entries, count)) return 0;
    if (rv_prior && rv_prior_raise) *(unsigned int *)(rv_prior + RR_LEVEL_OFFSET) = RR_LEVEL_PERMANENT;
    else if (rv_prior) {
        sh_resource_graph_begin(&frame, "entitydef", rv_name_of(rv_prior));
        sh_resource_graph_reference("entitydef", rv_entity_name); sh_resource_graph_end(&frame, 1);
    }
    return 1;
}

/* Forced collision (R-307): a fixed root name that already exists and holds a
 * delivery, as the preserved root gate5 met did, is refused and left intact,
 * and the next allocation is another new directory. The control removes only
 * what it created. */
static void rv_root_collision_control(void)
{
    char planted[MAX_PATH] = "", again[MAX_PATH] = "", fresh[MAX_PATH] = "", held[MAX_PATH];
    unsigned unique = 0;
    int failed = g_failed;
    HANDLE file;
    /* A fixed name no earlier run holds. */
    for (unsigned tries = 0; tries < 64 && !*planted; tries++) {
        unique = 0xf000u | ((GetCurrentProcessId() + tries) & 0xfffu);
        rv_new_root(planted, sizeof(planted), unique);
    }
    CHECK(*planted); if (!*planted) return;
    snprintf(held, sizeof(held), "%s/map-a", planted); CHECK(CreateDirectoryA(held, NULL));
    snprintf(held, sizeof(held), "%s/map-a/package.json", planted);
    file = CreateFileA(held, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE); if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    int reused = rv_new_root(again, sizeof(again), unique);
    DWORD refused = GetLastError();
    CHECK(!reused && !*again && refused == ERROR_ALREADY_EXISTS);
    CHECK(GetFileAttributesA(held) != INVALID_FILE_ATTRIBUTES);
    CHECK(rv_new_root(fresh, sizeof(fresh), 0) && _stricmp(fresh, planted));
    CHECK(inventory_remove_tree(planted));
    if (*fresh) CHECK(inventory_remove_tree(fresh));
    if (g_failed == failed)
        fprintf(stderr, "occurrence_revival_test: an existing root is refused and kept; the next root is new\n");
}

static void rv_case(int which)
{
    static const char doc_a[] = INLINE_VARIABLES M108_ENTITY("idTestKillAction", "") "]}";
    static const char doc_p[] = INLINE_VARIABLES M108_ENTITY("idTestKillAction", "") ","
        M108_ENTITY("idTestKillAction", "\"damageType\":\"kill/damage\"") "]}";
    admission_fixture f;
    char data[MAX_PATH], map_a[MAX_PATH], map_p[MAX_PATH], home[MAX_PATH], away[MAX_PATH], dir[MAX_PATH];
    char error[2048] = "", *saved_a = NULL, *saved_p = NULL, *native_p = NULL, *reopened_a = NULL;
    size_t saved_a_length = 0, saved_p_length = 0, native_p_length = 0, reopened_a_length = 0, before_count = 0;
    sh_package_bundle_binding rows_a[2] = {{0}};
    sh_package_provenance kept_a = {0};
    sh_package_map_plan *plan_a = NULL, *plan_p = NULL;
    sh_package_occurrence_snapshot *own_p = NULL;
    unsigned char before[RV_OBJECTS][0x20], *entity, *row;
    int present = 0, published_before, rebuilt_entity, result, prior_rebuilds = 0;
    unsigned int prior_level = 0;
    unsigned char *prior_object = NULL;
    rv_hooks hooks_state = {0};
    sh_decl_server_occurrence_hooks hooks = {&hooks_state, rv_prepare, rv_finalize, rv_rollback};

    reader_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    CHECK(rv_case_root(root, sizeof(root))); if (!*root) return;
    snprintf(data, sizeof(data), "%s/m113-paste", root);
    snprintf(map_a, sizeof(map_a), "%s/m113-paste-a", root); snprintf(map_p, sizeof(map_p), "%s/m113-paste-p", root);
    snprintf(home, sizeof(home), "%s/overrides/older", data); snprintf(away, sizeof(away), "%s/overrides/older", map_a);
    CreateDirectoryA(map_a, NULL); snprintf(dir, sizeof(dir), "%s/overrides", map_a); CreateDirectoryA(dir, NULL);
    rv_reset();
    sh_decl_server_test_reset_install();
    m113_shared_package("m113-paste", "older", "7");
    m113_shared_package("m113-paste-p", "newer", "70");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(library_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(library_editor_rows, sizeof(library_editor_rows) / sizeof(*library_editor_rows));
    sh_package_runtime_test_variants(1);
    /* 1. Map A authored with older installed; prefab P authored with newer. */
    CHECK(sh_package_runtime_refresh(data));
    saved_a = m113_author(data, doc_a, &saved_a_length);
    int moved = MoveFileA(home, away) != 0;
    DWORD move_error = moved ? 0 : GetLastError();
    if (!moved) fprintf(stderr, "m115 revival move: MoveFileA %s -> %s failed, error %lu\n", home, away, (unsigned long)move_error);
    m113_shared_package("m113-paste", "newer", "70");
    int refreshed = moved && sh_package_runtime_refresh(data);
    if (moved && !refreshed) fprintf(stderr, "m115 revival move: runtime refresh after the move failed\n");
    CHECK(moved && refreshed);
    saved_p = m113_author(data, doc_p, &saved_p_length);
    /* 2. A reopened from its carried older source and activated. */
    size_t count_a = m113_deliveries(map_a, rows_a);
    CHECK(count_a == 1 && saved_a && sh_map_provenance_read(saved_a, saved_a_length, &kept_a, &present, error, sizeof(error)) && present);
    plan_a = count_a == 1 && saved_a ? sh_package_runtime_prepare_map_document(data, map_a, &kept_a, rows_a, 1, saved_a,
        saved_a_length, error, sizeof(error)) : NULL;
    reopened_a = plan_a ? sh_package_map_plan_rebind(plan_a, &kept_a, saved_a, saved_a_length, &reopened_a_length, error,
        sizeof(error)) : NULL;
    int opened = reopened_a && sh_package_runtime_activate_prepared_map(plan_a, (sh_package_activation_guard){0}, m109_activate, NULL);
    if (!opened) {
        char runtime[1024] = "";
        sh_package_runtime_error(runtime, sizeof(runtime));
        fprintf(stderr, "m115 revival open A: %s | %s\n", error, runtime);
    }
    CHECK(opened);
    /* 3. P's import: its plan and ownership. */
    own_p = saved_p ? m113_import(data, map_p, saved_p, saved_p_length, &native_p, &native_p_length, &plan_p) : NULL;
    CHECK(plan_p && own_p);
    /* What the earlier activations left: newer's variant and its editor row
     * retired as defaulted shells by this provider. */
    entity = rv_object(RV_ENTITY, rv_entity_name); row = rv_object(RV_EDITOR, rv_row_name);
    CHECK(entity && row);
    if (!entity || !row || !plan_p || !own_p) goto done;
    CHECK((entity[0x2c] & RR_DEFAULT) && !(entity[0x2c] & RR_PENDING) && !*(void **)(entity + 0x18) && rv_retired(entity, rv_entity_name));
    CHECK((row[0x2c] & RR_DEFAULT) && !(row[0x2c] & RR_PENDING) && !*(void **)(row + 0x18) && rv_retired(row, rv_row_name));
    CHECK(rv_recorded("entitydef", rv_entity_name) && rv_recorded("snapeditorentitydef", rv_row_name));
    if (which == RV_LIVE_CONSUMER || which == RV_PRIOR_CONSUMER || which == RV_PRIOR_GUARD) {
        /* A live object of A's context whose parse references newer's variant. */
        sh_resource_graph_frame frame;
        unsigned char *consumer = NULL;
        for (int i = 0; i < *(int *)(rv_managers[RV_ENTITY] + 0x28) && !consumer; i++) {
            unsigned char *object = rv_items[RV_ENTITY][i];
            if (object != entity && !(object[0x2c] & RR_DEFAULT)) consumer = object;
        }
        CHECK(consumer != NULL);
        if (consumer && which == RV_PRIOR_CONSUMER) rv_prior = consumer;
        else if (consumer) {
            if (which == RV_PRIOR_GUARD) {
                rv_prior = consumer; rv_prior_raise = 1;
                prior_level = *(unsigned int *)(consumer + RR_LEVEL_OFFSET);
                *(unsigned int *)(consumer + RR_LEVEL_OFFSET) = 2;
            }
            sh_resource_graph_begin(&frame, "entitydef", rv_name_of(consumer));
            sh_resource_graph_reference("entitydef", rv_entity_name); sh_resource_graph_end(&frame, 1);
        }
    } else if (which == RV_FOREIGN_DEFAULT) {
        while (g_rr_retirement_count) {
            rr_retirement *retired = g_rr_retirements + --g_rr_retirement_count;
            free(retired->type); free(retired->name);
        }
    } else if (which == RV_WRONG_POINTER) {
        unsigned char *other = rv_new(RV_FOREIGN, rv_entity_name);
        if (other) rv_parse(other);
        rv_foreign = 1;
    } else if (which == RV_FAULT_PARSE || which == RV_RETIRED_PARSE) rv_fault_name = rv_row_name;
    unsigned char *consumer_shell = NULL;
    int consumer_rebuilds = 0;
    if (rv_retired_case(which)) {
        sh_resource_graph_frame frame;
        rr_entry noted = {0};
        consumer_shell = rv_new(RV_ENTITY, rv_consumer_name);
        CHECK(consumer_shell != NULL); if (!consumer_shell) goto done;
        consumer_shell[0x2c] = RR_DEFAULT; *(int *)(consumer_shell + RV_PARSED) = 0;
        noted.resource = consumer_shell; noted.type = "entitydef"; noted.name = (char *)rv_consumer_name; noted.retired = 1;
        if (which != RV_RETIRED_NEVER) rr_retirement_note(&noted);
        if (which == RV_RETIRED_RECORD) *(void **)(consumer_shell + 0x18) = rv_records;
        rv_default_reader = consumer_shell;
        rv_default_read = which == RV_RETIRED_UNRELATED ? "unrelated/missing" : rv_entity_name;
        /* Its retirement default's frame: the name it asked for and did not find (R-349). */
        sh_resource_graph_begin_requests(&frame, "entitydef", rv_consumer_name);
        sh_resource_graph_request("entitydef", rv_default_read); sh_resource_graph_end(&frame, 1);
        consumer_rebuilds = *(int *)(consumer_shell + RV_REBUILDS);
    }
    hooks_state.fail = which == RV_FAULT_STAGING || which == RV_RETIRED_STAGING ? 1 : which == RV_FAULT_FINALIZE ? 2 : 0;
    before_count = rv_object_count;
    for (size_t i = 0; i < before_count; i++) {
        *(int *)(rv_objects[i] + RV_LOWEST) = INT_MAX;
        before[i][0] = rv_objects[i][0x2c]; memcpy(before[i] + 4, rv_objects[i] + RV_PARSED, 0x18);
    }
    published_before = rv_generation; rebuilt_entity = *(int *)(entity + RV_REBUILDS);
    /* 4. The admission into the open map: decl_server's production path. */
    g_occurrence_frame = g_user_enabled = 1;
    g_boundary_thread = GetCurrentThreadId(); g_boundary_editor[9] = 1;
    g_boundary_resource_head = rv_managers[RV_ENTITY];
    snprintf(g_fixture_root, sizeof(g_fixture_root), "%s", data);
    sh_decl_server_test_occurrence_ready(g_boundary_editor);
    rv_bind();
    prior_object = rv_prior; prior_rebuilds = rv_prior ? *(int *)(rv_prior + RV_REBUILDS) : 0;
    if (rv_prior) { g_table_merge_hook = rv_merge_resolved; g_palette_stub_fail = 1; }
    if (consumer_shell) log_all_reset();
    result = sh_decl_server_admit_occurrences(plan_p, own_p, &hooks, error, sizeof(error));
    g_palette_stub_fail = 0;
    fprintf(stderr, "m115 revival %s: result %d: %s\n", rv_case_names[which], result, error);
    if (which == RV_PRIOR_CONSUMER && prior_object) {
        char runtime[1024] = "";
        /* Its edge came from this admission's preparation: the synchronous
         * recovery rebuilds it from the restored provider, with the pair. */
        sh_package_runtime_error(runtime, sizeof(runtime));
        fprintf(stderr, "m118 prior consumer: recovery rebuilt %s %d time(s), pending %d%s%s\n", rv_name_of(prior_object),
            *(int *)(prior_object + RV_REBUILDS) - prior_rebuilds, sh_decl_server_occurrence_recovery_pending(&hooks_state),
            *runtime ? ": " : "", runtime);
        CHECK(result != SH_PACKAGE_ADMISSION_READY && strstr(error, "fixture palette refused") != NULL &&
            strstr(error, "recovery failed") == NULL);
        CHECK(*(int *)(prior_object + RV_REBUILDS) > prior_rebuilds && sh_package_runtime_ready() && !*runtime);
    } else if (which == RV_PRIOR_GUARD && prior_object) {
        char expected[512];
        snprintf(expected, sizeof(expected), "additive consumer recovery failed: additive recovery selected a pre-existing "
            "native consumer: entitydef:%s (a recorded consumer of a changed identity;", rv_name_of(prior_object));
        /* Its edge predates the admission, so the admission does not own it:
         * recovery refuses, touches nothing, and stays retained. */
        CHECK(result != SH_PACKAGE_ADMISSION_READY && strstr(error, expected) != NULL);
        CHECK(sh_decl_server_occurrence_recovery_pending(&hooks_state) && !sh_package_runtime_ready() &&
            *(int *)(prior_object + RV_REBUILDS) == prior_rebuilds && !(entity[0x2c] & RR_DEFAULT));
        /* Back at the tier it had when the admission began, the next poll
         * completes without rebuilding it. */
        *(unsigned int *)(prior_object + RR_LEVEL_OFFSET) = 2;
        sh_decl_server_occurrence_maintenance();
        *(unsigned int *)(prior_object + RR_LEVEL_OFFSET) = prior_level;
        fprintf(stderr, "m118 consumer predating the admission: refused, then retained recovery %s\n",
            sh_decl_server_occurrence_recovery_pending(&hooks_state) ? "still pending" : "completed");
        CHECK(*(int *)(prior_object + RV_REBUILDS) == prior_rebuilds && sh_package_runtime_ready());
    } else if (sh_decl_server_occurrence_recovery_pending(&hooks_state)) sh_decl_server_occurrence_maintenance();
    CHECK(!sh_decl_server_occurrence_recovery_pending(&hooks_state));
    if (consumer_shell) {
        const rv_source *variant_source = rv_source_of("entitydef", rv_entity_name);
        int rebuilt = *(int *)(consumer_shell + RV_REBUILDS) - consumer_rebuilds;
        if (rv_refreshed_case(which)) {
            /* Owned and refreshed: rebuilt once the variant's addition existed (and, after the staging fault,
             * again by the restoration under the restored provider). A publication fault comes before the
             * refresh, so only the restoration rebuilds it. Always a retired default this provider keeps. */
            if (!strstr(g_log_all, "refreshes retired entitydef:smp/retired-default-consumer"))
                fprintf(stderr, "m131 refresh %s: rebuilt %d seen %d\n%s\n", rv_case_names[which], rebuilt,
                    *(int *)(consumer_shell + RV_SEEN), g_log_all);
            CHECK(strstr(g_log_all, "refreshes retired entitydef:smp/retired-default-consumer") != NULL);
            CHECK(rebuilt == (which == RV_RETIRED_STAGING ? 2 : 1));
            CHECK((consumer_shell[0x2c] & RR_DEFAULT) && !(consumer_shell[0x2c] & RR_PENDING) &&
                !*(void **)(consumer_shell + 0x18) && rv_retired(consumer_shell, rv_consumer_name));
            CHECK(which != RV_RETIRED_CONSUMER ? *(int *)(consumer_shell + RV_SEEN) == 0 :
                variant_source && *(int *)(consumer_shell + RV_SEEN) == variant_source->generation);
        } else {
            /* Untouched: not this addition's consumer, or still an existing one. */
            CHECK(!rebuilt && (consumer_shell[0x2c] & RR_DEFAULT) &&
                rv_retired(consumer_shell, rv_consumer_name) == (which != RV_RETIRED_NEVER));
            if (which != RV_RETIRED_UNRELATED) CHECK(strstr(error, rv_consumer_name) != NULL);
        }
    }
    if (which == RV_VALID || which == RV_RETIRED_CONSUMER || which == RV_RETIRED_UNRELATED) {
        const rv_source *entity_source = rv_source_of("entitydef", rv_entity_name);
        const rv_source *row_source = rv_source_of("snapeditorentitydef", rv_row_name);
        CHECK(result == SH_PACKAGE_ADMISSION_READY && hooks_state.prepared == 1 && hooks_state.finalized == 1 &&
            !hooks_state.rolled_back);
        CHECK(entity_source && row_source && entity_source->generation > published_before &&
            row_source->generation > published_before);
        if (entity_source && row_source) {
            const rv_source *sources[2] = {entity_source, row_source};
            unsigned char *objects[2] = {entity, row};
            uint64_t hashes[2] = {rv_fnv(entity_source->body, entity_source->length), rv_fnv(row_source->body, row_source->length)};
            fprintf(stderr, "m115 revival bodies: entitydef %zu bytes 0x%016llx, editor row %zu bytes 0x%016llx\n",
                entity_source->length, (unsigned long long)hashes[0], row_source->length, (unsigned long long)hashes[1]);
            CHECK(hashes[0] == RV_ENTITY_BODY_HASH && hashes[1] == RV_ROW_BODY_HASH);
            /* Each revived shell parsed its addition from exactly the bytes this
             * admission published, and never from anything older. */
            for (int i = 0; i < 2; i++) {
                CHECK(!(objects[i][0x2c] & (RR_DEFAULT | RR_PENDING | 1u)) && !*(void **)(objects[i] + 0x18));
                CHECK(*(int *)(objects[i] + RV_PARSED) == sources[i]->generation &&
                    *(int *)(objects[i] + RV_LOWEST) == sources[i]->generation &&
                    *(const unsigned char **)(objects[i] + RV_BODY) == sources[i]->body);
            }
        }
        CHECK(*(void **)(row + 0x1c8) == entity);
        CHECK(!rv_retired(entity, rv_entity_name) && !rv_retired(row, rv_row_name));
    } else if (!rv_refusals[which]) {
        /* A fault after the first native mutation: recovery retires both again. */
        CHECK(result != SH_PACKAGE_ADMISSION_READY && *(int *)(entity + RV_REBUILDS) > rebuilt_entity);
        CHECK((entity[0x2c] & RR_DEFAULT) && !(entity[0x2c] & RR_PENDING) && !*(void **)(entity + 0x18));
        CHECK((row[0x2c] & RR_DEFAULT) && !(row[0x2c] & RR_PENDING) && !*(void **)(row + 0x18));
        CHECK(rv_retired(entity, rv_entity_name) && rv_retired(row, rv_row_name));
        CHECK(which == RV_FAULT_PARSE || which == RV_RETIRED_PARSE || which == RV_PRIOR_CONSUMER || which == RV_PRIOR_GUARD ?
            !hooks_state.prepared :
            hooks_state.prepared == 1 && hooks_state.rolled_back >= 1);
    } else {
        /* Refused before any native mutation. */
        CHECK(result != SH_PACKAGE_ADMISSION_READY && strstr(error, rv_refusals[which]) != NULL);
        CHECK(*(int *)(entity + RV_REBUILDS) == rebuilt_entity && (entity[0x2c] & RR_DEFAULT) && !(entity[0x2c] & RR_PENDING));
        CHECK((row[0x2c] & RR_DEFAULT) && !(row[0x2c] & RR_PENDING));
        CHECK(which == RV_FOREIGN_DEFAULT || (rv_retired(entity, rv_entity_name) && rv_retired(row, rv_row_name)));
    }
    /* Every other object, A's context included, kept its state and bytes. */
    for (size_t i = 0; i < before_count; i++) {
        unsigned char *object = rv_objects[i];
        if (object == entity || object == row) continue;
        if (object == consumer_shell && rv_refreshed_case(which)) continue;
        if (which == RV_PRIOR_CONSUMER && object == prior_object) {
            /* Rebuilt: the same state, generation and bytes as before the admission. */
            CHECK(object[0x2c] == before[i][0] && !memcmp(object + RV_PARSED, before[i] + 4, 0x10) &&
                *(int *)(object + RV_LOWEST) == *(int *)(object + RV_PARSED));
            continue;
        }
        CHECK(object[0x2c] == before[i][0] && !memcmp(object + RV_PARSED, before[i] + 4, 0x18));
    }
    CHECK(!rv_strings && !*(int *)(rv_heap_object + 0xc4) && !*(int *)(rv_renderer + 0x14));
    CHECK(!g_rr_active && !g_rr_recovery && !g_rr_scope_recovery);
    if (which == RV_LIVE_CONSUMER) {
        sh_resource_graph_frame frame;
        for (int i = 0; i < *(int *)(rv_managers[RV_ENTITY] + 0x28); i++) {
            sh_resource_graph_begin(&frame, "entitydef", rv_name_of(rv_items[RV_ENTITY][i])); sh_resource_graph_end(&frame, 1);
        }
    }
done:
    g_table_merge_hook = NULL; g_occurrence_frame = 0; g_boundary_editor[9] = 0; g_boundary_resource_head = NULL;
    sh_decl_server_test_reset_install();
    if (reopened_a) HeapFree(GetProcessHeap(), 0, reopened_a);
    free(saved_a); free(saved_p); free(native_p);
    sh_package_map_plan_free(plan_a); sh_package_map_plan_free(plan_p); sh_package_occurrence_release(own_p);
    sh_package_provenance_free(&kept_a);
    for (size_t i = 0; i < 2; i++) sh_package_bundle_release(rows_a[i].original);
    sh_package_runtime_test_editor_rows(NULL, 0); sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}

/* M116 (R-271): the request-6 composition. Map A carries the older package and
 * the library installs the newer one; each package's own row places the one
 * parent both author with equal text (deduplicated), and each binds its own
 * private damage. Live, the rows differ by their package-qualified label; here
 * by the stock row each package's entry extends. A copy bound in the newer
 * context names newer's variant, and only the newer context's row presents
 * it. Pairing A's older row with that variant made a second row naming one
 * entityDef, which the additive palette's exact-name contract refuses. */
static void m116_package(const char *base, const char *name, const char *health, const char *row_parent)
{
    char path[MAX_PATH], text[160];
    m113_shared_package(base, name, health);
    snprintf(path, sizeof(path), "%s/overrides/%s/assets/generated/decls/snapeditorentitydef/kill/base_row.decl", base, name);
    snprintf(text, sizeof(text), "{ inherit = \"%s\"; edit = { entityDef = \"kill/base\"; } }", row_parent); create(path, text);
}
/* The served text of type/name in compiled, NUL-terminated, or NULL. */
static char *m116_served(const sh_package_compilation *compiled, const char *type, const char *name)
{
    char path[512], reason[256];
    size_t length = 0;
    snprintf(path, sizeof(path), "generated/decls/%s/%s.decl", type, name);
    const sh_compiled_resource *resource = compiled ? sh_package_compilation_find(compiled, path) : NULL;
    unsigned char *body = resource ? sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, reason, sizeof(reason)) : NULL;
    char *text = body ? malloc(length + 1) : NULL;
    if (text) { memcpy(text, body, length); text[length] = 0; }
    free(body); return text;
}
/* The quoted value following key in text, copied to out. */
static int m116_value(const char *text, const char *key, char *out, size_t capacity)
{
    const char *at = text ? strstr(text, key) : NULL, *open = at ? strchr(at, '"') : NULL, *close = open ? strchr(open + 1, '"') : NULL;
    if (!close || (size_t)(close - open - 1) >= capacity) return 0;
    memcpy(out, open + 1, (size_t)(close - open - 1)); out[close - open - 1] = 0; return 1;
}
/* The rows compiled serves that place entityDef, written to rows (names). */
/* The effective authored source of type/name: which package's bytes it serves. */
static const char *m116_source(const sh_package_compilation *compiled, const char *type, const char *name)
{
    for (size_t i = 0; compiled && compiled->sources && i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        if (resource->type && resource->name && !_stricmp(resource->type, type) && !strcmp(resource->name, name))
            return resource->source < compiled->sources->file_count ? compiled->sources->files[resource->source].absolute : NULL;
    }
    return NULL;
}
static int m116_from(const char *path, const char *package)
{
    char needle[96];
    snprintf(needle, sizeof(needle), "overrides/%s/", package);
    for (const char *at = path; at && *at; at++) {
        size_t i = 0;
        while (needle[i] && (at[i] == needle[i] || (needle[i] == '/' && at[i] == '\\'))) i++;
        if (!needle[i]) return 1;
    }
    return 0;
}
static size_t m116_rows_placing(const sh_package_compilation *compiled, const char *entity, char rows[][80], size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        char value[128] = "", *text;
        if (!resource->type || !resource->name || strcmp(resource->type, "snapeditorentitydef")) continue;
        text = m116_served(compiled, "snapeditorentitydef", resource->name);
        if (text && m116_value(text, "entityDef", value, sizeof(value)) && !strcmp(value, entity) && count < capacity)
            snprintf(rows[count++], 80, "%s", resource->name);
        free(text);
    }
    return count;
}
static void rv_presentations(int fault)
{
    static const char doc_a[] = INLINE_VARIABLES M108_ENTITY("idTestKillAction", "") "]}";
    static const char doc_p[] = INLINE_VARIABLES M108_ENTITY("idTestKillAction", "") ","
        M108_ENTITY("idTestKillAction", "\"damageType\":\"kill/damage\"") "]}";
    admission_fixture f;
    char data[MAX_PATH], map_a[MAX_PATH], map_p[MAX_PATH], home[MAX_PATH], away[MAX_PATH], dir[MAX_PATH];
    char error[2048] = "", *saved_a = NULL, *saved_p = NULL, *native_p = NULL, *reopened_a = NULL;
    char variant[80] = "", parent[80] = "", rows[8][80], child_rows[8][80];
    size_t saved_a_length = 0, saved_p_length = 0, native_p_length = 0, reopened_a_length = 0;
    sh_package_bundle_binding rows_a[2] = {{0}};
    sh_package_provenance kept_a = {0};
    sh_package_map_plan *plan_a = NULL, *plan_p = NULL;
    sh_package_occurrence_snapshot *own_p = NULL;
    int present = 0, result;
    size_t before_count = 0;
    unsigned char before[RV_OBJECTS][0x20];
    rv_hooks hooks_state = {0};
    sh_decl_server_occurrence_hooks hooks = {&hooks_state, rv_prepare, rv_finalize, rv_rollback};

    reader_collection_setup(&f);
    sh_decl_registry_source registry = {&f, admission_read, (uintptr_t)f.registry, NULL};
    CHECK(rv_case_root(root, sizeof(root))); if (!*root) return;
    snprintf(data, sizeof(data), "%s/m113-paste", root);
    snprintf(map_a, sizeof(map_a), "%s/m113-paste-a", root); snprintf(map_p, sizeof(map_p), "%s/m113-paste-p", root);
    snprintf(home, sizeof(home), "%s/overrides/older", data); snprintf(away, sizeof(away), "%s/overrides/older", map_a);
    CreateDirectoryA(map_a, NULL); snprintf(dir, sizeof(dir), "%s/overrides", map_a); CreateDirectoryA(dir, NULL);
    rv_reset();
    sh_decl_server_test_reset_install();
    m116_package("m113-paste", "older", "9999", "inlinestock/base_row");
    m116_package("m113-paste-p", "newer", "7777", "inlinestock/row_base");
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(library_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)f.reflection);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(library_editor_rows, sizeof(library_editor_rows) / sizeof(*library_editor_rows));
    sh_package_runtime_test_variants(1);
    CHECK(sh_package_runtime_refresh(data));
    saved_a = m113_author(data, doc_a, &saved_a_length);
    int moved = MoveFileA(home, away) != 0;
    m116_package("m113-paste", "newer", "7777", "inlinestock/row_base");
    CHECK(moved && sh_package_runtime_refresh(data));
    saved_p = m113_author(data, doc_p, &saved_p_length);
    size_t count_a = m113_deliveries(map_a, rows_a);
    CHECK(count_a == 1 && saved_a && sh_map_provenance_read(saved_a, saved_a_length, &kept_a, &present, error, sizeof(error)) && present);
    plan_a = count_a == 1 && saved_a ? sh_package_runtime_prepare_map_document(data, map_a, &kept_a, rows_a, 1, saved_a,
        saved_a_length, error, sizeof(error)) : NULL;
    reopened_a = plan_a ? sh_package_map_plan_rebind(plan_a, &kept_a, saved_a, saved_a_length, &reopened_a_length, error,
        sizeof(error)) : NULL;
    int opened = reopened_a && sh_package_runtime_activate_prepared_map(plan_a, (sh_package_activation_guard){0}, m109_activate, NULL);
    if (!opened) fprintf(stderr, "m116 presentations open A: %s\n", error);
    CHECK(opened);
    own_p = saved_p ? m113_import(data, map_p, saved_p, saved_p_length, &native_p, &native_p_length, &plan_p) : NULL;
    CHECK(plan_p && own_p);
    if (!opened || !plan_p || !own_p) goto done;
    /* The plan reaching admission: one variant of the shared parent, bound to
     * newer's private damage, presented by exactly the newer context's row,
     * while A's view serves both contexts' rows placing that parent. */
    {
        const sh_package_compilation *planned = sh_package_map_plan_compilation(plan_p);
        const sh_package_compilation *serving = sh_package_runtime_acquire();
        size_t variants = 0, children, placing;
        char *text = NULL, damage[128] = "", *damage_text = NULL;
        for (size_t i = 0; planned && i < planned->resource_count; i++) {
            const sh_compiled_resource *resource = planned->resources + i;
            if (!resource->generated_layer || !resource->type || strcmp(resource->type, "entitydef")) continue;
            variants++; snprintf(variant, sizeof(variant), "%s", resource->name);
        }
        CHECK(variants == 1);
        text = m116_served(planned, "entitydef", variant);
        CHECK(text && m116_value(text, "inherit", parent, sizeof(parent)) && m116_value(text, "damageType", damage, sizeof(damage)));
        damage_text = m116_served(planned, "entitydef", damage);
        CHECK(damage_text && strstr(damage_text, "7777"));
        fprintf(stderr, "m116 presentations plan: variant %s of %s binds damage %s\n", variant, parent, damage);
        placing = m116_rows_placing(serving, parent, rows, 8);
        CHECK(placing == 2);
        for (size_t i = 0; i < placing; i++) fprintf(stderr, "m116 presentations: A's view row %s places the shared parent\n", rows[i]);
        children = m116_rows_placing(planned, variant, child_rows, 8);
        CHECK(children == 1);
        for (size_t i = 0; i < children; i++) {
            char *child = m116_served(planned, "snapeditorentitydef", child_rows[i]), row[80] = "", *row_text = NULL;
            const char *source;
            CHECK(child && m116_value(child, "inherit", row, sizeof(row)));
            row_text = m116_served(serving, "snapeditorentitydef", row);
            if (!row_text) row_text = m116_served(planned, "snapeditorentitydef", row);
            /* Both packages author byte-identical rows (the retained sources),
             * so the row is identified by binding context: its served bytes
             * are the newer library package's, never A's carried older copy. */
            source = m116_source(serving, "snapeditorentitydef", row);
            if (!source) source = m116_source(planned, "snapeditorentitydef", row);
            CHECK(row_text && source && m116_from(source, "newer") && !m116_from(source, "older"));
            fprintf(stderr, "m116 presentations plan: child row %s presents the variant through %s (%s)\n", child_rows[i], row,
                source ? source : "no source");
            free(child); free(row_text);
        }
        free(text); free(damage_text);
        if (serving) sh_package_runtime_release();
    }
    g_occurrence_frame = g_user_enabled = 1;
    g_boundary_thread = GetCurrentThreadId(); g_boundary_editor[9] = 1;
    g_boundary_resource_head = rv_managers[RV_ENTITY];
    snprintf(g_fixture_root, sizeof(g_fixture_root), "%s", data);
    sh_decl_server_test_occurrence_ready(g_boundary_editor);
    rv_bind();
    before_count = rv_object_count;
    for (size_t i = 0; i < before_count; i++) {
        before[i][0] = rv_objects[i][0x2c]; memcpy(before[i] + 4, rv_objects[i] + RV_PARSED, 0x18);
    }
    g_palette_stub_fail = fault;
    result = sh_decl_server_admit_occurrences(plan_p, own_p, &hooks, error, sizeof(error));
    g_palette_stub_fail = 0;
    fprintf(stderr, "m116 presentations%s admission: result %d: %s\n", fault ? " (palette fault)" : "", result, error);
    if (sh_decl_server_occurrence_recovery_pending(&hooks_state)) {
        sh_decl_server_occurrence_maintenance();
        fprintf(stderr, "m116 presentations: retained recovery %s\n",
            sh_decl_server_occurrence_recovery_pending(&hooks_state) ? "still pending" : "completed");
    }
    CHECK(!sh_decl_server_occurrence_recovery_pending(&hooks_state));
    if (fault) {
        /* A palette-stage fault after native publication: recovery is atomic.
         * The variant and its row are retired shells again, nothing staged,
         * and every object that existed before the admission is unchanged. */
        unsigned char *entity = rv_object(RV_ENTITY, variant), *row = rv_object(RV_EDITOR, child_rows[0]);
        CHECK(result != SH_PACKAGE_ADMISSION_READY && strstr(error, "fixture palette refused") && !hooks_state.prepared);
        CHECK(entity && (entity[0x2c] & RR_DEFAULT) && !(entity[0x2c] & RR_PENDING) && rv_retired(entity, variant));
        CHECK(row && (row[0x2c] & RR_DEFAULT) && !(row[0x2c] & RR_PENDING) && rv_retired(row, child_rows[0]));
        for (size_t i = 0; i < before_count; i++) {
            unsigned char *object = rv_objects[i];
            if (object == entity || object == row) continue;
            CHECK(object[0x2c] == before[i][0] && !memcmp(object + RV_PARSED, before[i] + 4, 0x18));
        }
        CHECK(!g_palette_stub_count && !g_rr_active && !g_rr_recovery && !g_rr_scope_recovery);
        goto done;
    }
    CHECK(result == SH_PACKAGE_ADMISSION_READY && hooks_state.prepared == 1 && hooks_state.finalized == 1 && !hooks_state.rolled_back);
    /* Exactly one editor row resolves the variant: its newer row's child. The
     * palette holds one declaration for the variant's name. */
    {
        unsigned char *entity = rv_object(RV_ENTITY, variant);
        size_t resolving = 0, held = 0;
        for (int i = 0; entity && i < *(int *)(rv_managers[RV_EDITOR] + 0x28); i++) {
            unsigned char *object = rv_items[RV_EDITOR][i];
            if (*(unsigned char **)(object + 0x1c8) != entity) continue;
            resolving++; fprintf(stderr, "m116 presentations: editor row %s resolves the variant\n", rv_name_of(object));
            CHECK(!strcmp(rv_name_of(object), child_rows[0]));
        }
        for (size_t e = 0; e < g_palette_stub_count; e++) held += !strcmp(g_palette_stub[e].name, variant);
        CHECK(entity && resolving == 1 && held == 1);
    }
done:
    g_table_merge_hook = NULL; g_occurrence_frame = 0; g_boundary_editor[9] = 0; g_boundary_resource_head = NULL;
    sh_decl_server_test_reset_install();
    if (reopened_a) HeapFree(GetProcessHeap(), 0, reopened_a);
    free(saved_a); free(saved_p); free(native_p);
    sh_package_map_plan_free(plan_a); sh_package_map_plan_free(plan_p); sh_package_occurrence_release(own_p);
    sh_package_provenance_free(&kept_a);
    for (size_t i = 0; i < 2; i++) sh_package_bundle_release(rows_a[i].original);
    sh_package_runtime_test_editor_rows(NULL, 0); sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_dispose();
    for (size_t i = 0; i < f.count; i++) free(f.regions[i].address);
}

/* Each case composes from scratch in its own process: the package runtime keeps
 * process-wide authoring state that a disposal does not return to its start. */
/* M116: the retained actual request-6 inputs. The m115-r prefab carrier (four
 * entities, its exact delivered newer package and recorded bindings) is read
 * through production provenance decoding, map-document planning and capture
 * (m113_import mirrors prefab_import.c pi_prepare), then admitted. */
static void m116_lay(const char *base)
{
    char path[MAX_PATH];
    for (size_t i = 0; i < sizeof(m116_newer) / sizeof(*m116_newer); i++) {
        snprintf(path, sizeof(path), "%s/overrides/zz-m113-newer/%s", base, m116_newer[i].relative);
        for (char *c = path + strlen(base) + 1; *c; c++) if (*c == '/') { *c = 0; CreateDirectoryA(path, NULL); *c = '/'; }
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        CHECK(file != INVALID_HANDLE_VALUE && WriteFile(file, m116_newer[i].bytes, (DWORD)m116_newer[i].length, &written, NULL) &&
            written == m116_newer[i].length);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
}
/* M117: the reflected contracts the retained composition reaches, laid out as
 * the native registry and type records are (R-136, R-255; offsets and sizes are
 * the pinned images' records). The kill parent's class idSnapMapAction does not
 * reflect damageType; the placed idSnapMapAction_Kill reads it as idDeclDamage *,
 * so the package's binding of it is reader-bound. idDeclDamage is the damage
 * family's registered type: its damageParms members are read by the reflected
 * body reader, and aiThreatOverrideDamageDecl is a declaration pointer. */
static const char *const retained_editor_rows[] = {"action/player/kill"};
/* The retained case's fixture dependencies (arguments <doom-base> <replay>):
 * the installed game catalog supplies every stock source, as production's
 * library originals do, and a captured native reflection snapshot supplies
 * the reflected records, enums, readers and declaration managers
 * (native_reflection_replay.h). A package native name is a closure identity
 * over exact stock sources and their reflected semantics, so neither has a
 * stand-in (R-272). The installation's native assets (the directory holding
 * <doom-base>) supply production's page-provider adapter and loose VMTR
 * tables, read after the catalog in pr_original's order. */
static sh_resource_catalog *g_rt_catalog;
static native_replay g_rt_native;
static sh_package_native_assets *g_rt_assets;
/* "uncorrected": M116's context-row correction disabled (the control). */
static int g_rt_uncorrected;
/* "untyped": compiles without the replayed native schema, as this fixture did
 * before R-274. Production always compiles entity definitions typed (their
 * served text is the composer's canonical emission), so an untyped compile
 * derives another closure identity for the shared parent; the control must
 * detect that the effective parent changed instead of resolving it silently. */
static int g_rt_untyped;
/* The retained case's pinned inputs (R-274). The replay is the research capture
 * tool's line file for capture r3 (VK 5ad2548c image, 1204 ledger receipts of
 * backend 30263e51); the installation is the pinned pre-April-2024 Vulkan build
 * whose catalog supplies every stock source. A mismatch is a refusal, never a
 * silently different closure. tests\run-tests.ps1 supplies both. */
#define RT_REPLAY_SHA256 "d0f406bd7f94c5a1a5fd4f779ed5b7ca2339c5562398fd5a172fbf75fdff68ef"
#define RT_DOOM_VK_SHA256 "139763e94f1a75b5310179f9eeeb8a949a1f53c49acbc722fcfc5dfe7bb6d323"
static int rt_file_matches(const char *path, const char *expected, char *error, size_t capacity)
{
    FILE *file = NULL;
    unsigned char *bytes = NULL;
    long size = -1;
    sh_package_file_identity identity;
    char hex[65];
    int ok = 0;
    if (!fopen_s(&file, path, "rb") && file && !fseek(file, 0, SEEK_END) && (size = ftell(file)) > 0 && !fseek(file, 0, SEEK_SET) &&
        (bytes = malloc((size_t)size)) && fread(bytes, 1, (size_t)size, file) == (size_t)size &&
        sh_package_bytes_identity(bytes, (size_t)size, &identity)) {
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", identity.digest[i]);
        ok = !strcmp(hex, expected);
        if (!ok) snprintf(error, capacity, "%s has SHA-256 %s, not the pinned %s", path, hex, expected);
    } else snprintf(error, capacity, "%s is unreadable", path);
    if (file) fclose(file);
    free(bytes); return ok;
}
/* The native name a saved binding table gives one authored original. */
static const char *rt_saved_name(const sh_package_provenance *saved, const char *family, const char *original)
{
    for (size_t i = 0; i < saved->binding_count; i++) {
        const sh_package_provenance_binding *b = saved->bindings + i;
        if (b->family && b->name && b->original && !_stricmp(b->family, family) && !strcmp(b->original, original)) return b->name;
    }
    return NULL;
}
/* The shader source receipts the captured process's backend ledger held
 * (R-273), replayed in ledger order over the replayed objects. They are the
 * backend's own observations; the ordinary matchers decide what they prove.
 * The controls replay the same ledger without omit and with alter's header
 * byte +0x48 changed. */
static int retained_receipts(const native_replay *r, const native_replay_receipt *omit,
    const native_replay_receipt *alter, char *error, size_t capacity)
{
    for (size_t k = 0; k < r->receipt_count; k++) {
        const native_replay_receipt *x = &r->receipts[k];
        if (x == omit) continue;
        sh_package_shader_receipt_observed_input *inputs = x->input_count ? calloc(x->input_count, sizeof(*inputs)) : NULL;
        unsigned char header[0x90], *object = NULL;
        int ok;
        if (x->input_count && !inputs) { snprintf(error, capacity, "out of memory"); return 0; }
        for (size_t i = 0; i < x->input_count; i++)
            inputs[i] = (sh_package_shader_receipt_observed_input){x->inputs[i].path, x->inputs[i].body, x->inputs[i].seen,
                x->inputs[i].length, x->inputs[i].covered, x->inputs[i].failed};
        if (x->cook) memcpy(header, x->header, sizeof(header));
        else if (!(object = native_replay_receipt_header(r, x, header))) {
            free(inputs); snprintf(error, capacity, "receipt %zu names no replayed object", k); return 0;
        }
        if (x == alter) header[0x48] ^= 1;
        ok = sh_package_shader_receipt_test_replay(&(sh_package_shader_receipt_observation){x->cook, x->renderer,
            (uintptr_t)object, x->epoch, x->family, x->name, x->body, x->length, x->header_length, x->retained, header,
            x->metadata, x->metadata_length, inputs, x->input_count, x->ticket, x->preparation_ticket});
        free(inputs);
        if (!ok) { snprintf(error, capacity, "receipt %zu (%s %s) cannot be replayed", k, x->family ? x->family : "cook", x->name ? x->name : "?"); return 0; }
    }
    return 1;
}
/* Every stock path the retained composition reads must be the pinned closure
 * (retained_stock_closure.h): the same reader, length, SHA-256 and catalog
 * provider rows, or the same absence. The game executable and the replay pins
 * say nothing about DOOM/base, which can change on its own. RT_STOCK_RECORD=
 * <file> appends the reads instead (tests\run-tests.ps1 -RecordRetainedClosure).
 * RT_STOCK_CHANGE=<path> is the refusal control: the composition consumes that
 * path with its first byte inverted, as if the installed file had changed. */
static FILE *g_rt_stock_record;
static const char *g_rt_stock_change;
static char g_rt_stock_fault[1024];
static size_t g_rt_stock_reads, g_rt_stock_pinned;
static int rt_stock_observe(const char *path, const char *source, const unsigned char *body, size_t length)
{
    const sh_resource_catalog_entry *const *rows = NULL;
    sh_package_file_identity identity;
    char hex[65] = "-", rows_text[96] = "-";
    size_t count = strcmp(source, "snapmap") && strcmp(source, "campaign") ? 0 :
        sh_resource_catalog_find_provider(g_rt_catalog, path, &rows);
    if (body) {
        if (!sh_package_bytes_identity(body, length, &identity)) {
            snprintf(g_rt_stock_fault, sizeof(g_rt_stock_fault), "%s could not be hashed", path); return 0;
        }
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", identity.digest[i]);
    }
    if (count && rows) snprintf(rows_text, sizeof(rows_text), "%zu:%u:%llu:%u", count, (unsigned)rows[0]->archive,
        (unsigned long long)rows[0]->offset, (unsigned)rows[0]->stored_size);
    g_rt_stock_reads++;
    if (g_rt_stock_record) { fprintf(g_rt_stock_record, "%s\t%s\t%zu\t%s\t%s\n", path, source, length, hex, rows_text); return 1; }
    for (size_t i = 0; i < sizeof(rt_stock_closure) / sizeof(*rt_stock_closure); i++) {
        const rt_stock_pin *pin = rt_stock_closure + i;
        if (strcmp(pin->path, path)) continue;
        if (strcmp(pin->source, source) || pin->length != length || strcmp(pin->sha256, hex) || strcmp(pin->rows, rows_text)) {
            if (!*g_rt_stock_fault) snprintf(g_rt_stock_fault, sizeof(g_rt_stock_fault),
                "%s read %zu bytes (SHA-256 %s) from %s rows %s; the pinned closure has %zu bytes (%s) from %s rows %s",
                path, length, hex, source, rows_text, pin->length, pin->sha256, pin->source, pin->rows);
            return 0;
        }
        g_rt_stock_pinned++; return 1;
    }
    if (!*g_rt_stock_fault) snprintf(g_rt_stock_fault, sizeof(g_rt_stock_fault),
        "%s (%s, %zu bytes, SHA-256 %s) is not in the pinned closure", path, source, length, hex);
    return 0;
}
static int retained_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (!g_rt_catalog || !g_rt_assets) return -1;
    const char *source = "snapmap";
    int found = sh_resource_catalog_read_path(g_rt_catalog, path, body, length);
    if (found == 2) source = "campaign";
    if (!found) {
        free(*body); *body = NULL; *length = 0;
        found = sh_package_native_assets_read(g_rt_assets, path, body, length);
        source = "assets";
    }
    if (found > 0 && g_rt_stock_change && !strcmp(g_rt_stock_change, path) && *body && *length) (*body)[0] ^= 0xff;
    /* An absent path is consumed too: a file appearing there changes the result. */
    if (found >= 0 && !rt_stock_observe(path, found ? source : "absent", found ? *body : NULL, found ? *length : 0)) found = -1;
    if (found <= 0) { free(*body); *body = NULL; *length = 0; }
    return found;
}
/* The private map source context sh_mpkg_context_open builds (map_package.c),
 * through the same linked primitives: each carried delivery chunk decoded
 * strictly, its complete tree checked against the delivery key, the original
 * retained as the plan's bundle, and the tree unpacked under the private root's
 * overrides with the delivery key as its source name. */
static size_t rt_context(const char *json, size_t length, const char *data, const char *root,
    sh_package_bundle_binding *rows, size_t capacity)
{
    size_t at = 0, count = 0, header_length = 0, chunk_length = 0;
    const char *header = NULL, *chunk = NULL;
    char overrides[MAX_PATH], error[512] = "";
    CreateDirectoryA(root, NULL);
    snprintf(overrides, sizeof(overrides), "%s/overrides", root); CreateDirectoryA(overrides, NULL);
    while (count < capacity && sh_shard_next(json, length, "smpkg.", 6, &at, &header, &header_length, &chunk, &chunk_length)) {
        char key[256] = "", id[SH_PACKAGE_ID_CAP] = "", destination[MAX_PATH], *dot;
        unsigned char expected[32], tree[32], fingerprint[32], *payload = NULL;
        size_t payload_length = 0;
        int reason = 0, dots = 0;
        sh_shard_chunk part = {chunk, chunk_length, 1};
        CHECK(chunk && header_length > 6 && header_length - 6 < sizeof(key));
        if (!chunk || header_length <= 6 || header_length - 6 >= sizeof(key)) break;
        /* <key>.<index>.<total>.<digest>: each delivery here is one chunk. */
        memcpy(key, header + 6, header_length - 6);
        for (dot = key + strlen(key); dot > key && dots < 3;) if (*--dot == '.' && ++dots == 3) *dot = 0;
        CHECK(dots == 3 && strlen(key) < SH_PACKAGE_ID_CAP);
        payload = sh_shard_b64_decode(&part, 1, (size_t)64 << 20, &payload_length, &reason);
        CHECK(payload && sh_package_bundle_parse_key(key, expected, NULL) &&
            sh_package_archive_tree_identity(payload, payload_length, tree, error, sizeof(error)) && !memcmp(expected, tree, 32));
        CHECK(payload && sh_package_archive_identity(payload, payload_length, id, fingerprint, error, sizeof(error)));
        snprintf(destination, sizeof(destination), "%s/%s", overrides, key);
        rows[count].original = payload ? sh_package_bundle_from_archive(payload, payload_length, data, error, sizeof(error)) : NULL;
        CHECK(rows[count].original && sh_package_archive_unpack(payload, payload_length, destination, NULL, error, sizeof(error)));
        strcpy_s(rows[count].source_name, sizeof(rows[count].source_name), key);
        memcpy(rows[count].compiler_tree, fingerprint, 32);
        fprintf(stderr, "m117 context: delivery %s (package %s, %zu bytes)%s%s\n", key, id, payload_length, error[0] ? ": " : "", error);
        if (payload) HeapFree(GetProcessHeap(), 0, payload);
        count++;
    }
    return count;
}
/* The selection inputs of one family's saved uses: each use's site, binding
 * and supplier contexts, the opened delivery identities and the plan's
 * compiled resources of that family. */
static void rt_hex(const char *label, const unsigned char *bytes)
{
    fprintf(stderr, "%s", label);
    for (int i = 0; i < 32; i++) fprintf(stderr, "%02x", bytes[i]);
    fprintf(stderr, "\n");
}
static void rt_explain(const sh_package_map_plan *plan, const sh_package_provenance *saved,
    const sh_package_bundle_binding *rows, size_t row_count, const char *family)
{
    for (size_t i = 0; i < row_count; i++) {
        fprintf(stderr, "m117 explain: row %zu source %s\n", i, rows[i].source_name);
        rt_hex("m117 explain:   compiler tree ", rows[i].compiler_tree);
    }
    for (size_t i = 0; i < saved->context_count; i++) {
        fprintf(stderr, "m117 explain: saved context %zu mode %u trees %zu component %s\n", i, saved->contexts[i].mode,
            saved->contexts[i].tree_count, saved->contexts[i].component ? saved->contexts[i].component : "(none)");
        for (size_t t = 0; t < saved->contexts[i].tree_count; t++) rt_hex("m117 explain:   tree ", saved->contexts[i].trees[t]);
    }
    for (size_t i = 0; i < saved->binding_count; i++) if (!_stricmp(saved->bindings[i].family, family)) {
        fprintf(stderr, "m117 explain: binding %zu %s %s <- %s contexts", i, saved->bindings[i].family, saved->bindings[i].name,
            saved->bindings[i].original);
        for (size_t c = 0; c < saved->bindings[i].context_count; c++) fprintf(stderr, " %zu", saved->bindings[i].contexts[c]);
        fprintf(stderr, "\n");
    }
    for (size_t i = 0; i < saved->use_count; i++) if (!_stricmp(saved->bindings[saved->uses[i].binding].family, family)) {
        fprintf(stderr, "m117 explain: use %zu kind %u owner %s reference %s binding %zu row %s contexts", i, saved->uses[i].kind,
            saved->uses[i].owner, saved->uses[i].reference, saved->uses[i].binding, saved->uses[i].row ? saved->uses[i].row : "(none)");
        for (size_t c = 0; c < saved->uses[i].context_count; c++) fprintf(stderr, " %zu", saved->uses[i].contexts[c]);
        fprintf(stderr, "\n");
    }
    const sh_package_compilation *compiled = plan ? sh_package_map_plan_compilation(plan) : NULL;
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *resource = compiled->resources + i;
        if (resource->type && !_stricmp(resource->type, family))
            fprintf(stderr, "m117 explain: compiled %s (%s %s) sources %zu baseline %d composed %d native %d restored %d\n",
                resource->engine_path, resource->type, resource->name ? resource->name : "", resource->source_count,
                resource->baseline_known, resource->composed, resource->native_original, resource->restored_original);
    }
    if (!plan) fprintf(stderr, "m117 explain: no plan\n");
}
/* The older map A's native save: a 4-byte save checksum, then zlib(JSON). The
 * deflate stream is decoded by the product's map.decl decoder (raw_deflate.h)
 * and the zlib header and Adler-32 trailer are verified. */
static char *rt_decode_save(const unsigned char *save, size_t length, size_t *out_length)
{
    size_t capacity = (size_t)1 << 20, n = 0;
    unsigned char *json = length > 10 ? malloc(capacity + 1) : NULL;
    uint32_t a = 1, b = 0, adler = 0;
    *out_length = 0;
    CHECK(json && save[4] == 0x78 && !(save[5] & 0x20) && ((save[4] << 8) | save[5]) % 31 == 0);
    if (!json) return NULL;
    n = sh_inflate_raw_upto(save + 6, length - 10, json, capacity);
    for (size_t i = 0; i < n; i++) { a = (a + json[i]) % 65521; b = (b + a) % 65521; }
    adler = (uint32_t)save[length - 4] << 24 | (uint32_t)save[length - 3] << 16 | (uint32_t)save[length - 2] << 8 | save[length - 1];
    CHECK(n && ((b << 16) | a) == adler);
    if (!n) { free(json); return NULL; }
    json[n] = 0; *out_length = n; return (char *)json;
}
static int rt_every(const char *header, size_t length, void *context) { (void)header; (void)length; (void)context; return 1; }
/* sh_prefab_carrier_read's native document (prefab.c pf_native): the delivery
 * envelope removed (sh_mpkg_strip's every-package shard strip), then the
 * provenance envelope. */
static char *rt_native(const char *json, size_t length, size_t *out_length, char *error, size_t capacity)
{
    size_t stripped_length = 0;
    unsigned elements = 0, runs = 0;
    int failed = 0;
    char *stripped = sh_shard_strip(json, length, "smpkg.", 6, rt_every, NULL, 4096, &stripped_length, &elements, &runs, &failed);
    char *out = stripped ? sh_map_provenance_strip(stripped, stripped_length, out_length, error, capacity) : NULL;
    CHECK(stripped && elements == 1 && out);
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    return out;
}
static const char *rt_binding_name(const sh_package_provenance *provenance, size_t binding)
{ return provenance && binding < provenance->binding_count ? provenance->bindings[binding].name : NULL; }
/* M117: the retained request-6 composition through the production pipeline.
 * The older map A is its exact native save (eleven entities; its carried
 * m113-older delivery and binding table), decoded and reopened from its own
 * carried sources with the newer package installed. The m115-r prefab (four
 * entities: ComboStart, Listener_Simple, two idSnapMapAction_Kill; its carried
 * m113-newer delivery and bindings) is prepared as prefab_import.c pi_prepare
 * does and admitted into the open A through decl_server. */
/* The registry's source-existence check as production's (decl_server.c
 * ds_registry_source_exists in production source mode): a source exists when
 * the resource container holds it (the stock baseline here), the published
 * generated table does (what an activation merged, rv_table), or a native
 * source record was registered for it (rv_records). */
static int retained_source_exists(void *context, uintptr_t manager, const char *type, const char *name)
{
    char path[512];
    unsigned char *body = NULL;
    size_t length = 0;
    int stock;
    (void)context; (void)manager;
    if (!type || !name) return -1;
    snprintf(path, sizeof(path), "generated/decls/%s/%s.decl", type, name);
    for (char *c = path; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    stock = retained_baseline(NULL, path, &body, &length);
    free(body);
    return stock > 0 || rv_source_of(type, name) || rv_recorded(type, name) ? 1 : 0;
}
static void rv_retained(void)
{
    static const char *const classes[4] = {"idSnapMapGameEntity_ComboStart", "idSnapMapListener_Simple",
        "idSnapMapAction_Kill", "idSnapMapAction_Kill"};
    char root[MAX_PATH], data[MAX_PATH], map_a[MAX_PATH], map_p[MAX_PATH], error[2048] = "";
    char *doc_a = NULL, *reopened_a = NULL, *rebound_p = NULL, *native_p = NULL, *native = NULL;
    char variant[80] = "", parent[80] = "", damage[128] = "", rows[8][80], child_rows[8][80];
    size_t doc_a_length = 0, reopened_a_length = 0, rebound_p_length = 0, native_p_length = 0, native_length = 0;
    size_t count_a = 0, count_p = 0, entities = 0, before_count = 0;
    sh_package_bundle_binding rows_a[2] = {{0}}, rows_p[2] = {{0}};
    sh_package_provenance kept_a = {0}, kept_p = {0}, carrier = {0};
    sh_package_map_plan *plan_a = NULL, *plan_p = NULL;
    sh_package_occurrence_snapshot *own = NULL;
    sh_package_missing missing = {0};
    sh_package_source_graph_report report = {0};
    unsigned char before[RV_OBJECTS][0x20];
    int present = 0, result = 0;
    rv_hooks hooks_state = {0};
    sh_decl_server_occurrence_hooks hooks = {&hooks_state, rv_prepare, rv_finalize, rv_rollback};

    CHECK(g_rt_catalog && g_rt_native.arena && g_rt_assets);
    if (!g_rt_catalog || !g_rt_native.arena || !g_rt_assets) return;
    sh_decl_registry_source registry = {&g_rt_native, native_replay_read, g_rt_native.registry, retained_source_exists};
    sh_package_library_native installed = sh_package_native_assets_adapter(g_rt_assets);
    CHECK(rv_case_root(root, sizeof(root))); if (!*root) return;
    snprintf(data, sizeof(data), "%s/data", root); snprintf(map_a, sizeof(map_a), "%s/map-a", root);
    snprintf(map_p, sizeof(map_p), "%s/prefab-p", root);
    CreateDirectoryA(data, NULL);
    rv_reset();
    sh_decl_server_test_reset_install();
    /* Whatever the runtime merges into the published table from here on is
     * what the existence check and the native parses see. */
    g_table_merge_hook = rv_merge;
    m116_lay(data);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(retained_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, g_rt_native.reflection);
    sh_package_runtime_test_typed_compile(!g_rt_untyped);
    sh_package_runtime_test_atlas(installed.atlas, g_rt_assets);
    sh_package_runtime_test_shader_adapter(installed.source.shader);
    sh_package_runtime_test_installed_links(1);
    sh_package_runtime_test_editor_rows(retained_editor_rows, sizeof(retained_editor_rows) / sizeof(*retained_editor_rows));
    sh_package_runtime_test_variants(1); sh_package_runtime_test_variant_rows_any(g_rt_uncorrected);
    if (!sh_package_runtime_refresh(data)) {
        char runtime[1024] = "";
        sh_package_runtime_error(runtime, sizeof(runtime));
        fprintf(stderr, "m117 retained refresh: %s\n", runtime);
        CHECK(0);
    }
    /* 1. A's exact save decoded, its carried sources opened, reopened and activated. */
    doc_a = rt_decode_save(m117_map_a, sizeof(m117_map_a), &doc_a_length);
    fprintf(stderr, "m117 retained A: %zu JSON bytes, fnv 0x%016llx\n", doc_a_length,
        doc_a ? (unsigned long long)rv_fnv((const unsigned char *)doc_a, doc_a_length) : 0ull);
    CHECK(doc_a && doc_a_length == 19819);
    if (!doc_a) goto done;
    count_a = rt_context(doc_a, doc_a_length, data, map_a, rows_a, 2);
    CHECK(count_a == 1 && sh_map_provenance_read(doc_a, doc_a_length, &kept_a, &present, error, sizeof(error)) && present);
    /* A was activated earlier in this process (it was authored and saved in the
     * live session): its map-scoped declaration names were registered then,
     * and native source registration lasts for the process (the m115d log
     * REGISTERED smp/22b369ac at 12:06:45 and REUSED it). Only that prior
     * registration is modeled, from A's own binding table; nothing is published. */
    for (size_t i = 0; i < kept_a.binding_count; i++) {
        const sh_package_provenance_binding *binding = kept_a.bindings + i;
        if (binding->family && binding->name && _stricmp(binding->family, "entitydef") &&
            _stricmp(binding->family, "snapeditorentitydef")) {
            rv_record(binding->family, binding->name);
            fprintf(stderr, "m117 retained: A's earlier registration %s %s (%s)\n", binding->family, binding->name, binding->original);
        }
    }
    plan_a = count_a == 1 ? sh_package_runtime_prepare_map_document(data, map_a, &kept_a, rows_a, count_a, doc_a, doc_a_length,
        error, sizeof(error)) : NULL;
    reopened_a = plan_a ? sh_package_map_plan_rebind(plan_a, &kept_a, doc_a, doc_a_length, &reopened_a_length, error,
        sizeof(error)) : NULL;
    int opened = reopened_a && sh_package_runtime_activate_prepared_map(plan_a, (sh_package_activation_guard){0}, m109_activate, NULL);
    if (!opened) {
        char runtime[1024] = "";
        sh_package_runtime_error(runtime, sizeof(runtime));
        fprintf(stderr, "m117 retained open A: %s | %s\n", error, runtime);
        rt_explain(plan_a, &kept_a, rows_a, count_a, "damage");
        rt_explain(plan_a, &kept_a, rows_a, count_a, "entitydef");
    }
    CHECK(opened);
    if (!opened) goto done;
    {
        /* A's reopen derives its map-scoped names afresh; they must be the
         * names A saved (damage smp/22b369ac..., entity smp/5ec17642...). */
        const sh_package_compilation *planned_a = sh_package_map_plan_compilation(plan_a);
        for (size_t i = 0; planned_a && i < planned_a->resource_count; i++) {
            const sh_compiled_resource *resource = planned_a->resources + i;
            if (resource->type && (!_stricmp(resource->type, "damage") || !_stricmp(resource->type, "entitydef")) &&
                !strncmp(resource->name, "smp/", 4))
                fprintf(stderr, "m117 retained A derived: %s %s%s (%zu bytes: %.*s)\n", resource->type, resource->name,
                    resource->generated_layer ? " (generated)" : "", resource->body_length,
                    (int)(resource->body_length < 220 ? resource->body_length : 220), resource->body ? (const char *)resource->body : "");
        }
        for (size_t i = 0; i < kept_a.binding_count; i++)
            fprintf(stderr, "m117 retained A saved: %s %s (%s)\n", kept_a.bindings[i].family ? kept_a.bindings[i].family : "?",
                kept_a.bindings[i].name ? kept_a.bindings[i].name : "?", kept_a.bindings[i].original ? kept_a.bindings[i].original : "?");
        /* The reopen derives the exact names A saved: the shared parent and the
         * damage are closure identities, not historical aliases resolved through
         * A's binding table (R-274). The parent's served text is the typed
         * composer's canonical emission of the authored body. */
        const char *saved_parent = rt_saved_name(&kept_a, "entitydef", "m113/kill_base");
        const char *saved_damage = rt_saved_name(&kept_a, "damage", "damage/zion/player/coop/kill");
        const sh_compiled_resource *derived_parent = NULL, *derived_damage = NULL;
        char path[160];
        CHECK(saved_parent && saved_damage);
        if (saved_parent && planned_a) {
            snprintf(path, sizeof(path), "generated/decls/entitydef/%s.decl", saved_parent);
            derived_parent = sh_package_compilation_find(planned_a, path);
        }
        if (saved_damage && planned_a) {
            snprintf(path, sizeof(path), "generated/decls/damage/%s.decl", saved_damage);
            derived_damage = sh_package_compilation_find(planned_a, path);
        }
        CHECK(derived_damage && derived_damage->type && !derived_damage->generated_layer);
        if (g_rt_untyped) {
            /* The control stops here: its effective parent is another declaration. */
            CHECK(!derived_parent);
            fprintf(stderr, "m118 untyped control: A's reopen does not derive its saved parent %s\n", saved_parent ? saved_parent : "?");
            goto done;
        }
        static const char canonical_parent[] = "{\ninherit = \"snapmaps/action/input_base\";\nedit = {\n"
            "damageType = \"damage/zion/player/coop/kill\";\n}\n}\n";
        CHECK(derived_parent && derived_parent->type && !derived_parent->generated_layer && derived_parent->body &&
            derived_parent->body_length == sizeof(canonical_parent) - 1 &&
            !memcmp(derived_parent->body, canonical_parent, sizeof(canonical_parent) - 1));
    }
    /* 2. The prefab's import (pi_prepare): its carried context, document bind,
     * carrier, capture with its recorded bindings and rows, usage check. */
    count_p = rt_context((const char *)m116_prefab, sizeof(m116_prefab), data, map_p, rows_p, 2);
    CHECK(count_p == 1 && sh_map_provenance_read((const char *)m116_prefab, sizeof(m116_prefab), &kept_p, &present, error,
        sizeof(error)) && present);
    plan_p = count_p == 1 ? sh_package_runtime_prepare_map_document(data, map_p, &kept_p, rows_p, count_p,
        (const char *)m116_prefab, sizeof(m116_prefab), error, sizeof(error)) : NULL;
    rebound_p = plan_p ? sh_package_map_plan_rebind(plan_p, &kept_p, (const char *)m116_prefab, sizeof(m116_prefab),
        &rebound_p_length, error, sizeof(error)) : NULL;
    native_p = rebound_p ? rt_native(rebound_p, rebound_p_length, &native_p_length, error, sizeof(error)) : NULL;
    CHECK(native_p && sh_map_provenance_read(rebound_p, rebound_p_length, &carrier, &present, error, sizeof(error)) && present);
    native = native_p ? sh_package_runtime_capture_document(plan_p, &carrier, NULL, 0, native_p, native_p_length, &native_length,
        &own, NULL, NULL, error, sizeof(error)) : NULL;
    CHECK(native && own && sh_package_occurrence_carry_rows(own, &carrier));
    if (!native || !own) { fprintf(stderr, "m117 retained import: %s\n", error); goto done; }
    {
        int usage = sh_package_map_plan_usage_missing(plan_p, registry, g_rt_native.reflection, native, native_length, &missing,
            &report, error, sizeof(error));
        if (!usage || missing.count) {
            fprintf(stderr, "m117 retained usage: result %d, %zu missing of %zu checked: %s\n", usage, missing.count, missing.checked, error);
            for (size_t i = 0; i < missing.count && i < 16; i++) fprintf(stderr, "m117 retained missing: %s\n", missing.paths[i]);
        }
        CHECK(usage && !missing.count);
    }
    for (const char *at = native; (at = strstr(at, "\"className\"")); at++) {
        char name[96] = "";
        if (m116_value(at + 11, "", name, sizeof(name)) && entities < 4) CHECK(!strcmp(name, classes[entities]));
        entities++;
    }
    CHECK(entities == 4);
    /* 3. The plan's identities: each prefab occurrence's typed use and exact
     * row, the one reader-bound variant of the shared kill parent bound to the
     * newer damage, A's view's rows placing that parent, and the variant's
     * child rows with the row each presents it through. */
    {
        const sh_package_provenance *carried = sh_package_occurrence_provenance(own);
        const sh_package_compilation *planned = sh_package_map_plan_compilation(plan_p);
        const sh_package_compilation *serving = sh_package_runtime_acquire();
        size_t variants = 0, children = 0, placing = 0;
        char *text = NULL, *damage_text = NULL;
        for (size_t i = 0; carried && i < carried->use_count; i++) {
            const char *bound = rt_binding_name(carried, carried->uses[i].binding);
            fprintf(stderr, "m117 retained use %zu: %s %s -> %s (%s %s)%s%s\n", i, carried->uses[i].owner ? carried->uses[i].owner : "?",
                carried->uses[i].reference ? carried->uses[i].reference : "?", bound ? bound : "?",
                carried->uses[i].binding < carried->binding_count ? carried->bindings[carried->uses[i].binding].family : "?",
                carried->uses[i].binding < carried->binding_count ? carried->bindings[carried->uses[i].binding].original : "?",
                carried->uses[i].row ? " row " : "", carried->uses[i].row ? carried->uses[i].row : "");
        }
        for (size_t i = 0; i < 4; i++) {
            const char *row = sh_package_occurrence_document_row(own, i);
            fprintf(stderr, "m117 retained occurrence %zu (%s): row %s\n", i, classes[i], row ? row : "none");
        }
        for (size_t i = 0; planned && i < planned->resource_count; i++) {
            const sh_compiled_resource *resource = planned->resources + i;
            if (!resource->generated_layer || !resource->type || strcmp(resource->type, "entitydef")) continue;
            variants++; snprintf(variant, sizeof(variant), "%s", resource->name);
        }
        CHECK(variants == 1);
        text = m116_served(planned, "entitydef", variant);
        CHECK(text && m116_value(text, "inherit", parent, sizeof(parent)) && m116_value(text, "damageType", damage, sizeof(damage)));
        damage_text = m116_served(planned, "damage", damage);
        if (!damage_text) damage_text = m116_served(serving, "damage", damage);
        CHECK(damage_text && strstr(damage_text, "7777"));
        fprintf(stderr, "m117 retained plan: variant %s of %s binds damage %s\n", variant, parent, damage);
        {
            /* P's saved bind names the same shared parent A saved (R-274). */
            const char *saved = rt_saved_name(&kept_p, "entitydef", "m113/kill_base");
            CHECK(saved && !strcmp(parent, saved));
        }
        placing = m116_rows_placing(serving, parent, rows, 8);
        for (size_t i = 0; i < placing; i++) {
            const char *source = m116_source(serving, "snapeditorentitydef", rows[i]);
            fprintf(stderr, "m117 retained: A's view row %s places the shared parent (%s)\n", rows[i], source ? source : "no source");
        }
        CHECK(placing == 2);
        children = m116_rows_placing(planned, variant, child_rows, 8);
        for (size_t i = 0; i < children; i++) {
            char *child = m116_served(planned, "snapeditorentitydef", child_rows[i]), row[80] = "";
            const char *source = NULL;
            CHECK(child && m116_value(child, "inherit", row, sizeof(row)));
            source = m116_source(serving, "snapeditorentitydef", row);
            if (!source) source = m116_source(planned, "snapeditorentitydef", row);
            fprintf(stderr, "m117 retained plan: child row %s presents the variant through %s (%s)\n", child_rows[i], row,
                source ? source : "no source");
            /* Only the newer context's row presents it: never A's carried older
             * delivery, unless the control disabled that correction. */
            CHECK(source && (g_rt_uncorrected || !m116_from(source, rows_a[0].source_name)));
            free(child);
        }
        /* Uncorrected, A's older row is served beside the plan (one more native object). */
        CHECK(g_rt_uncorrected ? children >= 1 : children == 1);
        free(text); free(damage_text);
        if (serving) sh_package_runtime_release();
    }
    /* 4. The admission into the open A: decl_server's production path. */
    g_occurrence_frame = g_user_enabled = 1;
    g_boundary_thread = GetCurrentThreadId(); g_boundary_editor[9] = 1;
    g_boundary_resource_head = rv_managers[RV_ENTITY];
    snprintf(g_fixture_root, sizeof(g_fixture_root), "%s", data);
    sh_decl_server_test_occurrence_ready(g_boundary_editor);
    rv_bind();
    before_count = rv_object_count;
    for (size_t i = 0; i < before_count; i++) {
        before[i][0] = rv_objects[i][0x2c]; memcpy(before[i] + 4, rv_objects[i] + RV_PARSED, 0x18);
    }
    fprintf(stderr, "m117 retained: %zu native objects exist before the admission\n", before_count);
    result = sh_decl_server_admit_occurrences(plan_p, own, &hooks, error, sizeof(error));
    fprintf(stderr, "m117 retained admission: result %d: %s\n", result, error);
    if (sh_decl_server_occurrence_recovery_pending(&hooks_state)) {
        sh_decl_server_occurrence_maintenance();
        fprintf(stderr, "m117 retained: recovery %s\n", sh_decl_server_occurrence_recovery_pending(&hooks_state) ? "still pending" : "completed");
    }
    CHECK(!sh_decl_server_occurrence_recovery_pending(&hooks_state));
    if (g_rt_uncorrected) {
        /* A's carried older row also presents the variant: palette preparation
         * refuses the second row naming it (R-267), and nothing A made changes. */
        fprintf(stderr, "m117 retained uncorrected: prepared %d, finalized %d, rolled back %d\n",
            hooks_state.prepared, hooks_state.finalized, hooks_state.rolled_back);
        CHECK(result != SH_PACKAGE_ADMISSION_READY && strstr(error, "palette exact name already belongs to a different declaration"));
        CHECK(!hooks_state.finalized);
        for (size_t i = 0; i < before_count; i++) {
            unsigned char *object = rv_objects[i];
            if (rv_retired(object, rv_name_of(object))) continue;
            if (object[0x2c] != before[i][0] || memcmp(object + RV_PARSED, before[i] + 4, 0x18))
                fprintf(stderr, "m117 retained uncorrected: prior object %s changed\n", rv_name_of(object));
            CHECK(object[0x2c] == before[i][0] && !memcmp(object + RV_PARSED, before[i] + 4, 0x18));
        }
    } else {
    CHECK(result == SH_PACKAGE_ADMISSION_READY && hooks_state.prepared == 1 && hooks_state.finalized == 1 && !hooks_state.rolled_back);
    {
        unsigned char *entity = rv_object(RV_ENTITY, variant);
        size_t resolving = 0, held = 0;
        for (int i = 0; entity && i < *(int *)(rv_managers[RV_EDITOR] + 0x28); i++) {
            unsigned char *object = rv_items[RV_EDITOR][i];
            if (*(unsigned char **)(object + 0x1c8) != entity) continue;
            resolving++; fprintf(stderr, "m117 retained: editor row %s resolves the variant\n", rv_name_of(object));
            CHECK(!strcmp(rv_name_of(object), child_rows[0]));
        }
        for (size_t e = 0; e < g_palette_stub_count; e++) held += !strcmp(g_palette_stub[e].name, variant);
        CHECK(entity && resolving == 1 && held == 1);
        /* Every object A's activation made kept its state and parsed bytes. */
        for (size_t i = 0; i < before_count; i++) {
            unsigned char *object = rv_objects[i];
            if (object == entity || rv_retired(object, rv_name_of(object))) continue;
            if (object[0x2c] != before[i][0] || memcmp(object + RV_PARSED, before[i] + 4, 0x18))
                fprintf(stderr, "m117 retained: prior object %s changed\n", rv_name_of(object));
            CHECK(object[0x2c] == before[i][0] && !memcmp(object + RV_PARSED, before[i] + 4, 0x18));
        }
    }
    }
    CHECK(!g_rr_active && !g_rr_recovery && !g_rr_scope_recovery);
done:
    g_table_merge_hook = NULL; g_occurrence_frame = 0; g_boundary_editor[9] = 0; g_boundary_resource_head = NULL;
    sh_decl_server_test_reset_install();
    free(doc_a); free(native);
    if (reopened_a) HeapFree(GetProcessHeap(), 0, reopened_a);
    if (rebound_p) HeapFree(GetProcessHeap(), 0, rebound_p);
    if (native_p) HeapFree(GetProcessHeap(), 0, native_p);
    sh_package_missing_free(&missing);
    sh_package_map_plan_free(plan_a); sh_package_map_plan_free(plan_p); sh_package_occurrence_release(own);
    sh_package_provenance_free(&kept_a); sh_package_provenance_free(&kept_p); sh_package_provenance_free(&carrier);
    for (size_t i = 0; i < 2; i++) { sh_package_bundle_release(rows_a[i].original); sh_package_bundle_release(rows_p[i].original); }
    sh_package_runtime_test_editor_rows(NULL, 0); sh_package_runtime_test_installed_links(0);
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0); sh_package_runtime_test_atlas(NULL, NULL);
    sh_package_runtime_test_shader_adapter(NULL); sh_package_runtime_test_variant_rows_any(0);
    sh_package_runtime_test_typed_compile(0);
    sh_package_runtime_test_dispose();
}
/* M117: a replayed receipt is evidence only through the ordinary matcher. The
 * installed adapter certifies textless renderParm stagesort from its observed
 * receipt against the catalog original and the object's current header; the
 * same ledger with that observation's header changed, without it, or with it
 * invalidated is refused, and so are another name and an unobserved object. */
static void retained_receipt_controls(void)
{
    static const char path[] = "generated/decls/renderparm/stagesort.decl";
    const native_replay_receipt *x = NULL;
    unsigned char *object = NULL, *source = NULL, header[0x90], current[0x90], changed[0x90];
    size_t length = 0;
    char why[128], error[1024];
    int kind = -1; uint32_t index = 0;
    if (!g_rt_catalog || !g_rt_assets || !g_rt_native.receipt_count) { CHECK(!"retained receipt controls need the replay"); return; }
    const sh_package_source_shader *shader = sh_package_native_assets_adapter(g_rt_assets).source.shader;
    for (size_t k = 0; k < g_rt_native.receipt_count && !x; k++) {
        const native_replay_receipt *y = &g_rt_native.receipts[k];
        if (!y->cook && y->family && y->name && !strcmp(y->family, "renderparm") && !strcmp(y->name, "stagesort")) x = y;
    }
    CHECK(x && (object = native_replay_receipt_header(&g_rt_native, x, header)) != NULL);
    CHECK(sh_resource_catalog_read_path(g_rt_catalog, path, &source, &length) >= 1 && source);
    CHECK(source && rt_stock_observe(path, "snapmap", source, length));
    CHECK(shader && shader->parameter);
    if (!object || !source || !shader || !shader->parameter) goto done;
    CHECK(native_replay_read(&g_rt_native, (uintptr_t)object, current, sizeof(current)));
    CHECK(!*(uint64_t *)(current + 0x38)); /* the engine kept no text */
    sh_package_shader_receipt_refusal((uintptr_t)object, "renderparm", "stagesort", source, length, current, sizeof(current), why, sizeof(why));
    fprintf(stderr, "m117 receipt control observed: %s\n", why);
    CHECK(!strcmp(why, "receipt matches"));
    CHECK(shader->parameter(shader->context, "stagesort", (sh_decl_source){(const char *)source, length}, &kind, &index, error, sizeof(error)) == 1);
    fprintf(stderr, "m117 receipt control observed: stagesort kind %d index %u\n", kind, index);
    memcpy(changed, current, sizeof(changed)); changed[0x48] ^= 1;
    sh_package_shader_receipt_refusal((uintptr_t)object, "renderparm", "stagesort", source, length, changed, sizeof(changed), why, sizeof(why));
    fprintf(stderr, "m117 receipt control changed header: %s\n", why);
    CHECK(!strcmp(why, "receipt header differs at +0x48"));
    source[0] ^= 0x20;
    sh_package_shader_receipt_refusal((uintptr_t)object, "renderparm", "stagesort", source, length, current, sizeof(current), why, sizeof(why));
    source[0] ^= 0x20;
    fprintf(stderr, "m117 receipt control changed source: %s\n", why);
    CHECK(!strcmp(why, "receipt source text differs"));
    sh_package_shader_receipt_refusal((uintptr_t)object, "renderparm", "stagesort_other", source, length, current, sizeof(current), why, sizeof(why));
    fprintf(stderr, "m117 receipt control other name: %s\n", why);
    CHECK(!strcmp(why, "receipt names another object"));
    sh_package_shader_receipt_refusal((uintptr_t)current, "renderparm", "stagesort", source, length, current, sizeof(current), why, sizeof(why));
    fprintf(stderr, "m117 receipt control unobserved object: %s\n", why);
    CHECK(!strcmp(why, "no source receipt"));
    /* The same ledger replayed with the observation mismatched, missing and
     * invalidated: the installed adapter refuses each and asks for preparation. */
    for (int control = 0; control < 3; control++) {
        static const char *const names[] = {"mismatched", "missing", "invalidated"};
        static const char *const reasons[] = {"receipt header differs at +0x48", "no source receipt", "no completed source receipt"};
        sh_package_shader_receipt_test_reset();
        CHECK(retained_receipts(&g_rt_native, control == 1 ? x : NULL, control == 0 ? x : NULL, error, sizeof(error)));
        if (control == 2) sh_package_shader_receipt_invalidate((uintptr_t)object);
        int result = shader->parameter(shader->context, "stagesort", (sh_decl_source){(const char *)source, length}, &kind, &index, error, sizeof(error));
        fprintf(stderr, "m117 receipt control %s observation: result %d: %s\n", names[control], result, error);
        CHECK(result != 1 && strstr(error, reasons[control]));
    }
    sh_package_shader_receipt_test_reset();
    CHECK(retained_receipts(&g_rt_native, NULL, NULL, error, sizeof(error)));
    CHECK(shader->parameter(shader->context, "stagesort", (sh_decl_source){(const char *)source, length}, &kind, &index, error, sizeof(error)) == 1);
done:
    free(source);
}

/* Run one child with a new private root owned here: the child adopts it and
 * it is retired after the child exits, once the child's process-lifetime
 * handles are closed. Returns the child's exit code (1 if it did not run). */
static DWORD rv_run_child(const char *command, int *retired)
{
    char owned[MAX_PATH] = "", line[4096];
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    DWORD code = 1;
    CHECK(rv_new_root(owned, sizeof(owned), 0)); if (!*owned) return code;
    snprintf(line, sizeof(line), "%s", command);
    CHECK(SetEnvironmentVariableA(RV_ROOT_VARIABLE, owned));
    fflush(stdout); fflush(stderr);
    if (CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    } else CHECK(!"the child process could not start");
    SetEnvironmentVariableA(RV_ROOT_VARIABLE, NULL);
    if (rv_remove_root(owned) && GetFileAttributesA(owned) == INVALID_FILE_ATTRIBUTES) { if (retired) ++*retired; }
    else CHECK(!"a child's private root could not be retired after it exited");
    return code;
}

int main(int argc, char **argv)
{
    char exe[MAX_PATH], command[2 * MAX_PATH + 32], marker[MAX_PATH] = "", held[MAX_PATH];
    const char *adopted = getenv(RV_ROOT_VARIABLE);
    int retired = 0;
    /* A parent's new root: this process uses it and the parent retires it. */
    if (adopted && *adopted) {
        DWORD attributes = GetFileAttributesA(adopted);
        CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
        snprintf(g_rv_root, sizeof(g_rv_root), "%s", adopted); g_rv_adopted = 1;
    }
    /* The retained run itself is a child, so its root outlives its handles. */
    if (argc > 1 && !strcmp(argv[1], "retained") && !g_rv_adopted) {
        DWORD code = rv_run_child(GetCommandLineA(), &retired);
        if (!code && !g_failed)
            fprintf(stderr, "occurrence_revival_test: the retained run's root was retired after it exited\n");
        return code ? (int)code : g_failed != 0;
    }
    /* "retained <doom-base> <replay>" runs the retained request-6 composition
     * (M117) over the installed catalog and a captured native snapshot replay
     * (R-273), whose observed shader source receipts certify the textless
     * stock parameters, followed by the receipt controls. "uncorrected" as a
     * fifth argument is M116's correction-disabled control and "untyped" the
     * untyped-compile control. Its dependencies are the pinned installation and
     * snapshot (RT_*_SHA256); tests\run-tests.ps1 runs all three modes. */
    if (argc > 1 && !strcmp(argv[1], "retained")) {
        char path[MAX_PATH], error[1024] = "", *leaf;
        const char *record = getenv("RT_STOCK_RECORD");
        g_rt_stock_change = getenv("RT_STOCK_CHANGE");
        g_rt_uncorrected = argc > 4 && !strcmp(argv[4], "uncorrected");
        if (record && *record && (fopen_s(&g_rt_stock_record, record, "ab") || !g_rt_stock_record)) {
            fprintf(stderr, "occurrence_revival_test: RT_STOCK_RECORD %s cannot be opened\n", record); return 2;
        }
        g_rt_untyped = argc > 4 && !strcmp(argv[4], "untyped");
        g_rt_catalog = argc > 2 ? sh_resource_catalog_open(argv[2], error, sizeof(error)) : NULL;
        /* Production's doom_base is <doom_root>\base and its native assets open
         * <doom_root>; the same relation holds here. */
        snprintf(path, sizeof(path), "%s", argc > 2 ? argv[2] : "");
        leaf = strrchr(path, '\\'); if (!leaf || (strrchr(path, '/') && strrchr(path, '/') > leaf)) leaf = strrchr(path, '/');
        if (g_rt_catalog && leaf && !_stricmp(leaf + 1, "base")) { *leaf = 0; g_rt_assets = sh_package_native_assets_open(path); }
        if (g_rt_assets) {
            char pinned[MAX_PATH + 32];
            snprintf(pinned, sizeof(pinned), "%s\\DOOMx64vk.exe", path);
            if (!rt_file_matches(pinned, RT_DOOM_VK_SHA256, error, sizeof(error)) ||
                (argc > 3 && !rt_file_matches(argv[3], RT_REPLAY_SHA256, error, sizeof(error)))) {
                fprintf(stderr, "occurrence_revival_test: retained inputs are not the pinned ones (R-274): %s\n", error);
                sh_resource_catalog_close(g_rt_catalog); sh_package_native_assets_close(g_rt_assets);
                return 3;
            }
        }
        if (!g_rt_catalog || !g_rt_assets) fprintf(stderr, "occurrence_revival_test: retained needs the game catalog and installation: %s\n",
            argc > 2 ? (*error ? error : "<doom-base> must be the installation's base directory") : "no <doom-base>");
        else if (argc < 4 || !native_replay_load(&g_rt_native, argv[3], error, sizeof(error)))
            fprintf(stderr, "occurrence_revival_test: retained needs the reflection replay: %s\n", argc > 3 ? error : "no <replay>");
        else if (strcmp(g_rt_native.renderer, "vk") || !sh_package_native_assets_configure_shader_catalog(g_rt_assets,
            SH_PACKAGE_SHADER_VULKAN, (sh_native_resource_registry_source){&g_rt_native, native_replay_read, g_rt_native.resources},
            argv[2], error, sizeof(error))) {
            /* The shader ABI reads the snapshot's native resources under its own renderer. */
            fprintf(stderr, "occurrence_revival_test: retained shader ABI needs a Vulkan replay: %s\n", error);
            native_replay_free(&g_rt_native);
        } else if (!retained_receipts(&g_rt_native, NULL, NULL, error, sizeof(error))) {
            fprintf(stderr, "occurrence_revival_test: retained receipts: %s\n", error);
            native_replay_free(&g_rt_native);
        } else fprintf(stderr, "occurrence_revival_test: reflection replay %s %.8s: %zu classes, %zu fields, %zu enums, "
            "%zu managers (%zu loaded), %zu resource managers (%zu resources, %zu observed shader headers), %zu observed receipts\n",
            g_rt_native.renderer, g_rt_native.pin,
            g_rt_native.classes, g_rt_native.fields, g_rt_native.enums, g_rt_native.managers, g_rt_native.loaded,
            g_rt_native.resource_managers, g_rt_native.resource_entries, g_rt_native.shader_headers, g_rt_native.receipt_count);
        rv_root_collision_control();
        rv_retained();
        if (!g_rt_uncorrected) retained_receipt_controls();
        if (g_rt_stock_record) fclose(g_rt_stock_record);
        else if (g_rt_catalog && g_rt_assets) {
            if (*g_rt_stock_fault) {
                fprintf(stderr, "occurrence_revival_test: retained stock inputs are not the pinned ones (R-274): %s\n",
                    g_rt_stock_fault);
                g_failed = g_failed ? g_failed : 1;
            } else fprintf(stderr, "occurrence_revival_test: %zu stock read(s) matched the %zu-path pinned closure\n",
                g_rt_stock_reads, sizeof(rt_stock_closure) / sizeof(*rt_stock_closure));
            CHECK(!*g_rt_stock_fault && g_rt_stock_reads == g_rt_stock_pinned && g_rt_stock_reads);
        }
        if (g_rt_catalog) sh_resource_catalog_close(g_rt_catalog);
        sh_package_native_assets_close(g_rt_assets);
        native_replay_free(&g_rt_native);
        if (*g_rv_root && !g_rv_adopted) CHECK(rv_remove_root(g_rv_root));
        return *g_rt_stock_fault && !g_rt_stock_record ? 3 : g_failed != 0;
    }
    if (argc > 1) {
        int which = atoi(argv[1]);
        if (which < 0 || which > RV_CASES + 2) return 2;
        if (which == RV_CASES + 2) rv_retained();
        else if (which >= RV_CASES) rv_presentations(which - RV_CASES); else rv_case(which);
        if (*g_rv_root && !g_rv_adopted && !rv_remove_root(g_rv_root))
            fprintf(stderr, "occurrence_revival_test: private root %s remains: this process still holds its "
                "delete-on-close package snapshots; run the cases through the parent to retire it\n", g_rv_root);
        return g_failed != 0;
    }
    rv_root_collision_control();
    /* An unrelated root must survive every child's retirement. */
    CHECK(rv_new_root(marker, sizeof(marker), 0));
    snprintf(held, sizeof(held), "%s/marker", marker);
    if (*marker) {
        HANDLE file = CreateFileA(held, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE); if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    for (int which = 0; which <= RV_CASES + 1; which++) {
        DWORD code;
        snprintf(command, sizeof(command), "\"%s\" %d", exe, which);
        code = rv_run_child(command, &retired);
        if (code) fprintf(stderr, "m115 revival %s: case failed (exit %lu)\n", which < RV_CASES ? rv_case_names[which] : which == RV_CASES ? "two presentations" : which == RV_CASES + 1 ? "two presentations, palette fault" : "retained request-6 composition", (unsigned long)code);
        CHECK(code == 0);
    }
    CHECK(*marker && GetFileAttributesA(held) != INVALID_FILE_ATTRIBUTES);
    if (*marker) CHECK(rv_remove_root(marker));
    if (retired == RV_CASES + 2 && !g_failed)
        fprintf(stderr, "occurrence_revival_test: each of the %d child roots was retired after its child exited; "
            "an unrelated root survived\n", retired);
    if (g_failed) fprintf(stderr, "occurrence_revival_test: %d failure(s)\n", g_failed);
    else printf("occurrence_revival_test: a prefab admission revives this provider's retired shells from the bytes it publishes\n");
    return g_failed != 0;
}
