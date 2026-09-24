/* Atomic staging against native-shaped containers and exact pointer ownership. */
#define SH_APPLY_ENGINE_TESTING
#include <assert.h>
#include "../src/backend/apply_engine.c"

struct sh_package_occurrence_snapshot { int references, source; };
typedef struct tracked_record { void *pointer; sh_package_occurrence_snapshot *owner; } tracked_record;
static tracked_record records[32];
static unsigned char editor[0x22000], map_object[8], heap_local[0xc8];
static unsigned char manager[8], manager_vtable[0x88];
static DWORD engine_thread;
static sh_package_occurrence_snapshot old_owner = { 1, 111 }, new_owner = { 1, 222 };
static int frame_ready = 1, editor_ready = 1, attach_fails, ctor_fault, parse_fault, parse_refuses;
static int dtor_fault, commit_fault, commit_refuses, commit_calls, ctor_calls, dtor_calls;
static int parser_nodes, parser_strings, nested_prepare, heap_refuses, heap_pop_fault;
static void *old_array, *new_array;
static unsigned char old_bytes[PREFAB_NATIVE_SIZE], new_bytes[PREFAB_NATIVE_SIZE];
static const char native_name[] = "same/compiled/name";

static unsigned char *slot(void) { return editor + PASTE_STAGING_OFF; }
static void fault(void) { RaiseException(0xe0000001, 0, 0, NULL); }
static int heap_depth(void) { return *(int *)(heap_local + 0xc4); }
static void *heap_get(void) { return heap_local; }
static void heap_push(void *self, int heap)
{
    assert(self == heap_local && heap == 0);
    if (!heap_refuses) {
        *(int *)(heap_local + 0x44 + heap_depth() * 4) = heap;
        ++*(int *)(heap_local + 0xc4);
    }
}
static void heap_pop(void *self)
{
    assert(self == heap_local && heap_depth() == 3);
    --*(int *)(heap_local + 0xc4);
    if (heap_pop_fault) fault();
}
static void assert_scope(void) { assert(heap_depth() == 3); }

sh_package_occurrence_snapshot *sh_package_occurrence_retain(sh_package_occurrence_snapshot *owner)
{ if (owner) { assert(owner->references > 0); ++owner->references; } return owner; }
void sh_package_occurrence_release(sh_package_occurrence_snapshot *owner)
{ if (owner) { assert(owner->references > 1); --owner->references; } }
static tracked_record *record(void *pointer)
{
    for (size_t i = 0; i < sizeof(records) / sizeof(*records); ++i)
        if (records[i].pointer == pointer) return records + i;
    return NULL;
}
static void attach_records(void *flat, sh_package_occurrence_snapshot *owner)
{
    unsigned char *entities = *(unsigned char **)((unsigned char *)flat + 0x30);
    int count = *(int *)((unsigned char *)flat + 0x38);
    for (int i = 0; i < count; ++i) {
        void *pointer = entities + i * 0x1b0;
        tracked_record *entry = record(pointer);
        assert(!entry);
        for (size_t j = 0; j < sizeof(records) / sizeof(*records); ++j) {
            if (!records[j].pointer) {
                records[j].pointer = pointer;
                records[j].owner = sh_package_occurrence_retain(owner);
                entry = records + j; break;
            }
        }
        assert(entry);
    }
}
static void assert_records(void *entities, sh_package_occurrence_snapshot *owner)
{
    for (size_t i = 0; i < 2; ++i) {
        tracked_record *entry = record((unsigned char *)entities + i * 0x1b0);
        assert(entry && entry->owner == owner && entry->owner->source == owner->source);
        assert(!strcmp((char *)entry->pointer, native_name));
    }
}
int sh_package_editor_ready(void) { return editor_ready; }
int sh_package_editor_healthy(char *error, size_t capacity) { (void)error; (void)capacity; return editor_ready; }
sh_package_occurrence_snapshot *sh_package_editor_snapshot(unsigned kind, const void *flat,
    char *error, size_t capacity)
{
    (void)error; (void)capacity;
    assert(kind == SH_PACKAGE_EDITOR_PREFAB && flat == slot());
    tracked_record *entry = record(*(void **)((unsigned char *)flat + 0x30));
    assert(entry); return sh_package_occurrence_retain(entry->owner);
}
int sh_package_editor_attach(unsigned kind, const void *flat,
    const sh_package_occurrence_snapshot *owner, char *error, size_t capacity)
{
    (void)error; (void)capacity; assert_scope();
    assert(kind == SH_PACKAGE_EDITOR_PREFAB && owner == &new_owner && flat != slot());
    if (attach_fails) return 0;
    attach_records((void *)flat, (sh_package_occurrence_snapshot *)owner); return 1;
}
void *sh_editor_transition_capture(const void *value) { return value == editor ? map_object : NULL; }
int sh_editor_transition_frame_ready(void *value, void *previous)
{ return frame_ready && value == editor && previous == map_object; }

static void init_string(unsigned char *str, const char *text)
{
    size_t length = strlen(text); assert(length < 20);
    memset(str, 0, IDSTR_SIZE); *(void **)str = manager_vtable;
    *(int *)(str + IDSTR_LEN_OFF) = (int)length;
    *(char **)(str + IDSTR_DATA_OFF) = (char *)str + IDSTR_SSO_OFF;
    *(unsigned *)(str + IDSTR_FLAGS_OFF) = 0x80000014u;
    memcpy(str + IDSTR_SSO_OFF, text, length + 1);
}
static void init_prefab(unsigned char *value)
{
    memset(value, 0, PREFAB_NATIVE_SIZE);
    for (size_t i = 0; i < 19; ++i) {
        unsigned char *header = value + 0x30 + i * 0x18;
        *(uint16_t *)(header + 0x10) = 16; header[0x12] = 5;
    }
    *(void **)(value + PREFAB_INLINE_LIST) = value + PREFAB_INLINE_ITEMS;
    *(int *)(value + PREFAB_INLINE_LIST + 8) = PREFAB_INLINE_COUNT;
    *(int *)(value + PREFAB_INLINE_LIST + 12) = PREFAB_INLINE_COUNT;
    value[PREFAB_INLINE_LIST + 0x10] = 1;
    value[PREFAB_INLINE_LIST + 0x12] = 5;
    value[PREFAB_INLINE_LIST + 0x13] = 1;
    for (size_t i = 0; i < PREFAB_INLINE_COUNT; ++i)
        init_string(value + PREFAB_INLINE_ITEMS + i * PREFAB_INLINE_STRIDE, "inline");
}
static void populate(unsigned char *value, unsigned char marker)
{
    unsigned char *entities = calloc(2, 0x1b0); assert(entities);
    *(void **)(value + 0x30) = entities;
    *(int *)(value + 0x38) = *(int *)(value + 0x3c) = 2;
    for (int i = 0; i < 2; ++i) strcpy((char *)entities + i * 0x1b0, native_name);
    value[0] = marker;
    /* Exercise an external string beside the remaining inline strings. */
    unsigned char *str = value + PREFAB_INLINE_ITEMS + 3 * PREFAB_INLINE_STRIDE;
    char *dynamic = malloc(64); assert(dynamic); strcpy(dynamic, "external persistent value");
    *(void **)(str + IDSTR_DATA_OFF) = dynamic;
    *(int *)(str + IDSTR_LEN_OFF) = (int)strlen(dynamic);
    *(unsigned *)(str + IDSTR_FLAGS_OFF) = 0xc0000040u;
    *(void **)(str + 0x30) = manager; /* Shared declaration pointer stays exact. */
    assert(ae_stage_layout(value, NULL));
}
static void prefab_ctor(void *value)
{
    assert_scope(); ++ctor_calls;
    if (ctor_fault) fault();
    init_prefab(value);
}
static void destroy_prefab(unsigned char *value)
{
    assert(ae_stage_layout(value, NULL));
    unsigned char *entities = *(unsigned char **)(value + 0x30);
    int count = *(int *)(value + 0x38);
    for (int i = 0; i < count; ++i) {
        tracked_record *entry = record(entities + i * 0x1b0);
        if (entry) { sh_package_occurrence_release(entry->owner); memset(entry, 0, sizeof(*entry)); }
    }
    free(entities);
    for (size_t i = 0; i < PREFAB_INLINE_COUNT; ++i) {
        unsigned char *str = value + PREFAB_INLINE_ITEMS + i * PREFAB_INLINE_STRIDE;
        char *data = *(char **)(str + IDSTR_DATA_OFF);
        if (data != (char *)str + IDSTR_SSO_OFF) free(data);
    }
    memset(value, 0xdd, PREFAB_NATIVE_SIZE);
}
static void prefab_dtor(void *value)
{
    assert_scope(); ++dtor_calls;
    if (dtor_fault) fault();
    destroy_prefab(value);
}
static void *reflect(void *self) { assert(self == manager); return map_object; }
void *sh_typeinfo_get_declmgr(void) { return manager; }
static void node_ctor(void *value, int kind)
{ assert_scope(); assert(kind == 0 || kind == 7); memset(value, 0, PARSE_NODE_SIZE); ++parser_nodes; }
static void node_dtor(void *value) { (void)value; assert_scope(); assert(parser_nodes); --parser_nodes; }
static void *string_ctor(void *value, const char *text)
{ assert_scope(); init_string(value, text); ++parser_strings; return value; }
static void string_dtor(void *value) { (void)value; assert_scope(); assert(parser_strings); --parser_strings; }
static void lex_ctor(void *value) { (void)value; assert_scope(); }
static char lexer(void *value, void *source, void *node, int one)
{ (void)value; (void)source; (void)node; assert_scope(); assert(one == 1); return 1; }
static char deserialize(void *reflection, const char *type, void *value, void *node, void *tag, void *flags)
{
    (void)node; assert_scope();
    assert(reflection == map_object && !strcmp(type, "idSnapEntityPrefab") && !flags);
    assert(*(uint16_t *)tag == 1 && parser_nodes == 2 && parser_strings == 3);
    if (parse_fault) fault();
    populate(value, 0x22); new_array = *(void **)((unsigned char *)value + 0x30);
    return parse_refuses ? 0 : 1;
}
static int commit_disk(char *error, size_t capacity)
{
    (void)error; (void)capacity; assert_scope(); ++commit_calls;
    assert(*(void **)(slot() + 0x30) == new_array);
    assert_records(new_array, &new_owner); assert_records(old_array, &old_owner);
    if (nested_prepare) {
        sh_apply_prefab_stage *nested = NULL;
        assert(sh_apply_prefab_stage_prepare(editor, "{}", &new_owner, 0, &nested, NULL, 0) == 0 && !nested);
    }
    if (commit_fault) fault();
    return !commit_refuses;
}
static void reset(void)
{
    assert(old_owner.references == 1 && new_owner.references == 1 && !g_stage_quarantine);
    for (size_t i = 0; i < sizeof(records) / sizeof(*records); ++i) assert(!records[i].pointer);
    memset(editor, 0, sizeof(editor)); init_prefab(slot()); populate(slot(), 0x11);
    old_array = *(void **)(slot() + 0x30); new_array = NULL; attach_records(slot(), &old_owner);
    memcpy(old_bytes, slot(), sizeof(old_bytes));
    *(void **)(editor + ED_MAP_OBJ_OFF) = map_object; g_editor = editor;
    *(void **)manager = manager_vtable;
    *(void *(**)(void *))(manager_vtable + VSLOT_REFLECT_ACCESSOR) = reflect;
    engine_thread = GetCurrentThreadId(); g_main_thread_at = (const uint8_t *)&engine_thread;
    memset(heap_local, 0, sizeof(heap_local)); *(int *)(heap_local + 0xc4) = 2;
    g_stage_heap.get = heap_get; g_stage_heap.push = heap_push; g_stage_heap.pop = heap_pop;
    g_prefab_ctor = prefab_ctor; g_prefab_dtor = prefab_dtor;
    g_node_ctor = node_ctor; g_node_dtor = node_dtor; g_lex_ctor = lex_ctor;
    g_idstr_ctor = string_ctor; g_idstr_dtor = string_dtor; g_deser = deserialize; g_lexer = lexer;
    frame_ready = editor_ready = 1;
    attach_fails = ctor_fault = parse_fault = parse_refuses = dtor_fault = 0;
    commit_fault = commit_refuses = commit_calls = ctor_calls = dtor_calls = 0;
    parser_nodes = parser_strings = nested_prepare = heap_refuses = heap_pop_fault = 0;
    g_stage_busy = g_stage_faulted = 0;
}
static void finish_slot(void)
{
    destroy_prefab(slot());
    assert(old_owner.references == 1 && new_owner.references == 1 && heap_depth() == 2);
}
static sh_apply_prefab_stage *prepare(void)
{
    sh_apply_prefab_stage *stage = NULL; char error[256];
    assert(sh_apply_prefab_stage_prepare(editor, "{}", &new_owner, 0, &stage, error, sizeof(error)) == 1);
    assert(stage && stage->ready && !stage->swapped && heap_depth() == 2);
    assert(!memcmp(slot(), old_bytes, sizeof(old_bytes)) && !parser_nodes && !parser_strings);
    assert_records(old_array, &old_owner); assert_records(new_array, &new_owner);
    memcpy(new_bytes, stage->prefab, sizeof(new_bytes)); return stage;
}
static void invalid_layouts_leave_both_objects(void)
{
    sh_apply_prefab_stage *stage = prepare();
    for (int mode = 0; mode < 7; ++mode) {
        unsigned char baseline[PREFAB_NATIVE_SIZE];
        if (mode == 0) *(int *)(slot() + 0x38) = 3;
        if (mode == 1) *(void **)(slot() + 0x30) = NULL;
        if (mode == 2) *(void **)(slot() + 0x30) = stage->prefab + 0x210;
        if (mode == 3) *(void **)(slot() + PREFAB_INLINE_LIST) = stage->prefab + PREFAB_INLINE_ITEMS;
        if (mode == 4) *(void **)(slot() + PREFAB_INLINE_ITEMS + IDSTR_DATA_OFF) = slot() + 0x15;
        if (mode == 5) *(int *)(slot() + PREFAB_INLINE_ITEMS + IDSTR_LEN_OFF) = 20;
        if (mode == 6) slot()[0x43] = 2;
        memcpy(baseline, slot(), sizeof(baseline));
        assert(ae_stage_exchange(stage) == 0 && !stage->swapped);
        assert(!memcmp(slot(), baseline, sizeof(baseline)) && !memcmp(stage->prefab, new_bytes, sizeof(new_bytes)));
        memcpy(slot(), old_bytes, sizeof(old_bytes));
    }
    assert(sh_apply_prefab_stage_abort(&stage, NULL, 0) == 1 && !stage); finish_slot();
}
/* Fake partial constructors allocate nothing. The fixture may dispose these
 * records after asserting quarantine; production deliberately has no escape. */
static void dispose_fake_quarantine(sh_apply_prefab_stage *stage, int native_exists)
{
    assert(stage && stage->quarantined);
    if (native_exists) destroy_prefab(stage->prefab);
    ae_stage_free(stage); g_stage_quarantine = NULL; g_stage_faulted = 0;
    parser_nodes = parser_strings = 0;
}
int main(void)
{
    sh_apply_prefab_stage *stage = NULL; char error[256];
    reset(); invalid_layouts_leave_both_objects();
    reset(); stage = prepare();
    assert(ae_stage_exchange(stage) == 1 && stage->swapped);
    assert(*(void **)(slot() + 0x30) == new_array && *(void **)(stage->prefab + 0x30) == old_array);
    assert_records(new_array, &new_owner); assert_records(old_array, &old_owner);
    assert(ae_stage_layout(slot(), stage->prefab) && ae_stage_layout(stage->prefab, slot()));
    assert(ae_stage_exchange(stage) == 1 && !stage->swapped);
    assert(!memcmp(slot(), old_bytes, sizeof(old_bytes)) && !memcmp(stage->prefab, new_bytes, sizeof(new_bytes)));
    assert_records(new_array, &new_owner); assert_records(old_array, &old_owner);
    assert(sh_apply_prefab_stage_abort(&stage, error, sizeof(error)) == 1 && !stage); finish_slot();

    reset(); stage = prepare(); commit_refuses = nested_prepare = 1;
    assert(sh_apply_prefab_stage_finalize(&stage, commit_disk, error, sizeof(error)) == 0);
    assert(stage && !stage->swapped && commit_calls == 1 && !dtor_calls);
    assert(!memcmp(slot(), old_bytes, sizeof(old_bytes)) && !memcmp(stage->prefab, new_bytes, sizeof(new_bytes)));
    assert_records(new_array, &new_owner); assert_records(old_array, &old_owner);
    assert(sh_apply_prefab_stage_abort(&stage, error, sizeof(error)) == 1 && !stage); finish_slot();

    reset(); stage = prepare();
    assert(sh_apply_prefab_stage_finalize(&stage, commit_disk, error, sizeof(error)) == 1 && !stage);
    assert(commit_calls == 1 && dtor_calls == 1 && old_owner.references == 1 && new_owner.references == 3);
    assert_records(new_array, &new_owner); assert(!record(old_array));
    assert(g_we_staged && g_we_staged_count == 2 && sh_apply_last_place_result() == 1); finish_slot();

    reset(); stage = prepare(); slot()[0] = 0x66;
    assert(sh_apply_prefab_stage_finalize(&stage, commit_disk, error, sizeof(error)) == 0 && !commit_calls);
    assert(slot()[0] == 0x66 && !stage->swapped);
    assert(sh_apply_prefab_stage_abort(&stage, error, sizeof(error)) == 1); finish_slot();

    for (int mode = 0; mode < 6; ++mode) {
        reset();
        if (mode == 0) frame_ready = 0;
        if (mode == 1) engine_thread++;
        if (mode == 2) editor_ready = 0;
        if (mode == 3) g_idstr_dtor = NULL;
        if (mode == 4) heap_refuses = 1;
        if (mode == 5) *(int *)(slot() + 0x38) = 3;
        unsigned char baseline[PREFAB_NATIVE_SIZE]; memcpy(baseline, slot(), sizeof(baseline));
        assert(sh_apply_prefab_stage_prepare(editor, "{}", &new_owner, 0, &stage, error, sizeof(error)) == 0 && !stage);
        assert(!ctor_calls && !memcmp(slot(), baseline, sizeof(baseline)) && new_owner.references == 1);
        memcpy(slot(), old_bytes, sizeof(old_bytes)); finish_slot();
    }
    for (int mode = 0; mode < 2; ++mode) {
        reset(); if (mode) attach_fails = 1; else parse_refuses = 1;
        assert(sh_apply_prefab_stage_prepare(editor, "{}", &new_owner, 0, &stage, error, sizeof(error)) == 0 && !stage);
        assert(ctor_calls == 1 && dtor_calls == 1 && !parser_nodes && !parser_strings);
        assert(!memcmp(slot(), old_bytes, sizeof(old_bytes)) && new_owner.references == 1); finish_slot();
    }
    for (int mode = 0; mode < 2; ++mode) {
        reset(); if (mode) parse_fault = 1; else ctor_fault = 1;
        assert(sh_apply_prefab_stage_prepare(editor, "{}", &new_owner, 0, &stage, error, sizeof(error)) == -1 && stage);
        assert(g_stage_faulted && !dtor_calls && old_owner.references == 4 && new_owner.references == 2);
        assert(sh_apply_prefab_stage_abort(&stage, error, sizeof(error)) == -1 && stage && !dtor_calls);
        assert(!memcmp(slot(), old_bytes, sizeof(old_bytes)));
        dispose_fake_quarantine(stage, mode); stage = NULL; finish_slot();
    }
    reset(); stage = prepare(); dtor_fault = 1;
    assert(sh_apply_prefab_stage_abort(&stage, error, sizeof(error)) == -1 && stage && dtor_calls == 1);
    assert(sh_apply_prefab_stage_abort(&stage, error, sizeof(error)) == -1 && dtor_calls == 1);
    dispose_fake_quarantine(stage, 1); stage = NULL; finish_slot();

    reset(); stage = prepare(); dtor_fault = 1;
    assert(sh_apply_prefab_stage_finalize(&stage, commit_disk, error, sizeof(error)) == 1 && !stage);
    assert(g_stage_quarantine && g_stage_faulted && commit_calls == 1 && dtor_calls == 1);
    assert_records(new_array, &new_owner); assert_records(old_array, &old_owner);
    assert(sh_apply_prefab_stage_prepare(editor, "{}", &new_owner, 0, &stage, error, sizeof(error)) == 0 && !stage);
    dispose_fake_quarantine(g_stage_quarantine, 1); finish_slot();

    reset(); stage = prepare(); commit_fault = 1;
    assert(sh_apply_prefab_stage_finalize(&stage, commit_disk, error, sizeof(error)) == -1 && stage && stage->swapped);
    assert(stage->quarantined && !dtor_calls && commit_calls == 1);
    assert_records(new_array, &new_owner); assert_records(old_array, &old_owner);
    dispose_fake_quarantine(stage, 1); stage = NULL; finish_slot();

    reset(); heap_pop_fault = 1;
    assert(sh_apply_prefab_stage_prepare(editor, "{}", &new_owner, 0, &stage, error, sizeof(error)) == -1 && stage);
    assert(stage->quarantined && heap_depth() == 2 && !stage->heap.active);
    dispose_fake_quarantine(stage, 1); stage = NULL; finish_slot();
    puts("apply_prefab_stage_test: PASS"); return 0;
}

void backend_log(const char *text) { (void)text; }
sh_iface *sh_ui_get_iface(void) { return NULL; }
int sh_iface_engine_copy_paste_enabled(void) { assert(0); return 0; }
uintptr_t sig_addr_by_name(const sig_result *r, size_t n, const char *name) { (void)r; (void)n; (void)name; assert(0); return 0; }
void sh_map_published_poll(void) { assert(0); }
void sh_rawmap_map_context_poll(void) { assert(0); }
void sh_decl_server_rearm_poll(void) { assert(0); }
void sh_mpkg_consent_poll(void) { assert(0); }
void sh_rawmap_pending_map_poll(void) { assert(0); }
void sh_package_requirements_poll(void) { assert(0); }
void sh_prefab_import_maintenance(void) { assert(0); }
int sh_iface_class_inherit_ok(int id, const char *c, const char *h) { (void)id; (void)c; (void)h; assert(0); return 0; }
const char *ie_resolve_id_string(int id, char *buf, int cap) { (void)id; (void)buf; (void)cap; assert(0); return NULL; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status) { (void)base; (void)name; (void)status; assert(0); return 0; }
int sh_host_is_pinned_rva_build(void) { assert(0); return 0; }
unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *len) { (void)name; (void)len; assert(0); return NULL; }
int sh_config_get_bool(const char *key, int *value, unsigned *flags) { (void)key; (void)value; (void)flags; assert(0); return 0; }
int sh_rawmap_snapshot(void *serializer, void *map, void *out) { (void)serializer; (void)map; (void)out; assert(0); return 0; }
int sh_rawmap_snapshot_inspect(void *serializer, void *map, void *out, sh_rawmap_snapshot_visit visit, void *ctx)
{ (void)serializer; (void)map; (void)out; (void)visit; (void)ctx; assert(0); return 0; }
void sh_nav_bake_refresh_live(void) { assert(0); }
unsigned long sh_nav_bake_geometry_revision(void) { return 0; }
int sh_nav_bake_refresh_volumes(int *volumes) { (void)volumes; assert(0); return 0; }
const char *sh_nav_bake_volumes_reason(void) { return ""; }
int sh_nav_bake_preview(sh_nav_bake_reader reader, sh_nav_preview_line line, sh_nav_preview_colour_fn colour, void *ctx)
{ (void)reader; (void)line; (void)colour; (void)ctx; assert(0); return 0; }
int sh_nav_bake_preview_pending(void) { assert(0); return 0; }
int sh_nav_preview_install(const sig_result *r, size_t n) { (void)r; (void)n; assert(0); return 0; }
void sh_nav_preview_begin(void *world) { (void)world; assert(0); }
void sh_nav_preview_add_line(const float start[3], const float end[3], void *unused) { (void)start; (void)end; (void)unused; assert(0); }
void sh_nav_preview_publish(void) { assert(0); }
void sh_nav_preview_clear(void) { assert(0); }
int sh_nav_preview_published(void) { assert(0); return 0; }
void sh_nav_preview_colour(float r, float g, float b) { (void)r; (void)g; (void)b; assert(0); }
