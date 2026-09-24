/* Existing flat objects serialize without clone hooks or native lifetime leaks. */
#define SH_APPLY_ENGINE_TESTING
#include <assert.h>
#include "../src/backend/apply_engine.c"

enum { FAIL_NONE, FAIL_TAG, FAIL_TREE, FAIL_STRING, FAIL_SERIALIZE, FAIL_SERIALIZE_EXCEPTION,
    FAIL_RENDER, FAIL_DATA, FAIL_LENGTH, FAIL_STRING_DESTROY, FAIL_TREE_DESTROY };
static DWORD main_id;
static unsigned char manager[8], vtable[0x88], flat[0x600];
static int failure, node_calls, nodes_created, nodes_destroyed, strings_created, strings_destroyed;
static int serializations, renders, reflection_missing, test_nested;
static void *tag_node, *tree_node;
static char *rendered_heap;
static const char *payload, *expected_type;
static size_t payload_length;
static void raise_fault(void) { RaiseException(0xe0000001, 0, 0, NULL); }
static void *reflect(void *self) { assert(self == manager); return reflection_missing ? NULL : flat; }
void *sh_typeinfo_get_declmgr(void) { return manager; }
static void node_ctor(void *node, int kind)
{
    assert(kind == 7); node_calls++;
    if ((node_calls == 1 && failure == FAIL_TAG) || (node_calls == 2 && failure == FAIL_TREE)) raise_fault();
    assert(node_calls <= 2); nodes_created++;
    if (node_calls == 1) tag_node = node; else tree_node = node;
    memset(node, 0xc7, PARSE_NODE_SIZE);
}
static void node_dtor(void *node)
{
    assert(nodes_destroyed < nodes_created);
    assert(node == (nodes_created == 2 && !nodes_destroyed ? tree_node : tag_node));
    assert(*(unsigned char *)node == 0xc7);
    nodes_destroyed++; memset(node, 0xdd, PARSE_NODE_SIZE);
    if (failure == FAIL_TREE_DESTROY && nodes_destroyed == 1) raise_fault();
}
static void *string_ctor(void *str, const char *initial)
{
    assert(initial && !*initial);
    if (failure == FAIL_STRING) raise_fault();
    strings_created++;
    *(int *)((char *)str + IDSTR_LEN_OFF) = 0;
    *(char **)((char *)str + IDSTR_DATA_OFF) = (char *)str + IDSTR_SSO_OFF;
    return str;
}
static void string_dtor(void *str)
{
    assert(strings_created == 1 && !strings_destroyed); strings_destroyed++;
    if (rendered_heap) { memset(rendered_heap, 0xdd, payload_length); free(rendered_heap); rendered_heap = NULL; }
    memset(str, 0xdd, IDSTR_SIZE);
    if (failure == FAIL_STRING_DESTROY) raise_fault();
}
static char serialize(void *reflection, const char *type, void *source, void *tree, void *tag, void *flags)
{
    assert(reflection == flat && source == flat && tree == tree_node && !flags);
    assert(!strcmp(type, expected_type) && nodes_created == 2 && strings_created == 1);
    assert(*(uint16_t *)tag == 1 && ((unsigned char *)tag)[2] == 1 && !((unsigned char *)tag)[3]);
    assert((char *)tag + 8 == tag_node); serializations++;
    if (test_nested) {
        char *out = (char *)1; size_t length = 7;
        assert(!sh_apply_engine_serialize_flat(flat, SH_APPLY_FLAT_MAP, &out, &length) && !out && !length);
    }
    if (failure == FAIL_SERIALIZE_EXCEPTION) raise_fault();
    return failure != FAIL_SERIALIZE;
}
static void render(void *tree, void *str)
{
    assert(tree == tree_node && serializations == 1); renders++;
    if (failure == FAIL_RENDER) raise_fault();
    char *bytes = (char *)str + IDSTR_SSO_OFF;
    if (payload_length >= IDSTR_SIZE - IDSTR_SSO_OFF) {
        rendered_heap = malloc(payload_length + 1); assert(rendered_heap); bytes = rendered_heap;
    }
    memcpy(bytes, payload, payload_length); bytes[payload_length] = 0;
    *(const char **)((char *)str + IDSTR_DATA_OFF) = failure == FAIL_DATA ? (const char *)1 : bytes;
    *(int *)((char *)str + IDSTR_LEN_OFF) = failure == FAIL_LENGTH ? -1 : (int)payload_length;
}
static void reset(void)
{
    assert(!rendered_heap);
    node_calls = nodes_created = nodes_destroyed = strings_created = strings_destroyed = serializations = renders = 0;
    tag_node = tree_node = NULL;
}
static void succeeds(unsigned kind)
{
    char *out = NULL; size_t length = 0;
    size_t prefix = kind == SH_APPLY_FLAT_ENTITY ? strlen("{\"entities\":[") : 0;
    size_t suffix = kind == SH_APPLY_FLAT_ENTITY ? 2 : 0;
    expected_type = kind == SH_APPLY_FLAT_MAP ? "idSnapMap" :
        kind == SH_APPLY_FLAT_PREFAB ? "idSnapEntityPrefab" : "idSnapEntity";
    reset(); assert(sh_apply_engine_serialize_flat(flat, kind, &out, &length));
    assert(out && out != payload && length == prefix + payload_length + suffix && !out[length]);
    assert(!memcmp(out + prefix, payload, payload_length));
    if (prefix) assert(!memcmp(out, "{\"entities\":[", prefix) && !memcmp(out + prefix + payload_length, "]}", 2));
    assert(nodes_created == 2 && nodes_destroyed == 2 && strings_created == 1 && strings_destroyed == 1);
    assert(serializations == 1 && renders == 1 && !rendered_heap);
    for (size_t i = 0; i < sizeof(flat); i++) assert(flat[i] == 0xa5);
    free(out);
}
int main(void)
{
    char *out; size_t length;
    memset(flat, 0xa5, sizeof(flat)); *(void **)manager = vtable;
    *(void *(**)(void *))(vtable + VSLOT_REFLECT_ACCESSOR) = reflect;
    g_ser = serialize; g_render = render; g_node_ctor = node_ctor; g_node_dtor = node_dtor;
    g_idstr_ctor = string_ctor; g_idstr_dtor = string_dtor;
    /* Clone/flat constructors/prefab helpers deliberately remain unavailable. */
    assert(!g_entity_clone && !g_def_ctor && !g_def_dtor && !g_prefab_populate);
    payload = "{\"byte\":\"\x97\"}"; payload_length = strlen(payload);
    g_main_thread_at = (const uint8_t *)&main_id;
    for (int i = 0; i < 3; i++) {
        main_id = i == 0 ? 0 : i == 1 ? GetCurrentThreadId() + 1 : GetCurrentThreadId();
        if (i == 2) g_main_thread_at = (const uint8_t *)(uintptr_t)1;
        out = (char *)1; length = 7;
        assert(!sh_apply_engine_serialize_flat(flat, SH_APPLY_FLAT_MAP, &out, &length) && !out && !length);
        assert(!node_calls && !serializations);
    }
    main_id = GetCurrentThreadId(); g_main_thread_at = (const uint8_t *)&main_id;
    assert(!sh_apply_engine_serialize_flat(flat, 999, &out, &length));
    assert(!sh_apply_engine_serialize_flat(NULL, SH_APPLY_FLAT_MAP, &out, &length));
    assert(!sh_apply_engine_serialize_flat(flat, SH_APPLY_FLAT_MAP, NULL, &length));
    assert(!sh_apply_engine_serialize_flat(flat, SH_APPLY_FLAT_MAP, &out, NULL));
    reflection_missing = 1;
    assert(!sh_apply_engine_serialize_flat(flat, SH_APPLY_FLAT_MAP, &out, &length) && !node_calls);
    reflection_missing = 0;
    g_render = NULL;
    assert(!sh_apply_engine_serialize_flat(flat, SH_APPLY_FLAT_MAP, &out, &length) && !node_calls);
    g_render = render;
    for (unsigned kind = SH_APPLY_FLAT_MAP; kind <= SH_APPLY_FLAT_ENTITY; kind++) succeeds(kind);
    expected_type = "idSnapMap";
    for (int mode = FAIL_TAG; mode <= FAIL_TREE_DESTROY; mode++) {
        reset(); failure = mode; out = (char *)1; length = 7;
        assert(!sh_apply_engine_serialize_flat(flat, SH_APPLY_FLAT_MAP, &out, &length) && !out && !length);
        assert(nodes_created == nodes_destroyed && strings_created == strings_destroyed && !rendered_heap);
    }
    failure = FAIL_NONE; test_nested = 1; succeeds(SH_APPLY_FLAT_PREFAB); test_nested = 0;
    succeeds(SH_APPLY_FLAT_MAP); /* No error or nested call leaves a stuck guard. */
    /* Complete valid native output may exceed the former 4 MiB UI cap. */
    const size_t large_length = 6u * 1024u * 1024u + 128u;
    char *large = malloc(large_length + 1); assert(large);
    memset(large, 'x', large_length); memcpy(large, "{\"note\":\"", 9);
    memcpy(large + large_length - 2, "\"}", 2); large[large_length] = 0;
    payload = large; payload_length = large_length;
    succeeds(SH_APPLY_FLAT_MAP); succeeds(SH_APPLY_FLAT_ENTITY); free(large);
    puts("apply_flat_serialize_test: PASS"); return 0;
}

/* Other apply-engine operations are outside this fixture and must not run. */
void backend_log(const char *text) { (void)text; }
sh_iface *sh_ui_get_iface(void) { return NULL; }
uintptr_t sig_addr_by_name(const sig_result *r, size_t n, const char *name) { (void)r; (void)n; (void)name; assert(0); return 0; }
void sh_map_published_poll(void) { assert(0); }
void sh_rawmap_map_context_poll(void) { assert(0); }
void sh_decl_server_rearm_poll(void) { assert(0); }
void sh_mpkg_consent_poll(void) { assert(0); }
void sh_rawmap_pending_map_poll(void) { assert(0); }
void sh_package_requirements_poll(void) { assert(0); }
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
int sh_nav_preview_install(const sig_result *r, size_t n) { (void)r; (void)n; assert(0); return 0; }
void sh_nav_preview_begin(void *world) { (void)world; assert(0); }
void sh_nav_preview_add_line(const float start[3], const float end[3], void *unused) { (void)start; (void)end; (void)unused; assert(0); }
void sh_nav_preview_publish(void) { assert(0); }
void sh_nav_preview_clear(void) { assert(0); }
void sh_nav_preview_colour(float r, float g, float b) { (void)r; (void)g; (void)b; assert(0); }
int sh_iface_engine_copy_paste_enabled(void) { assert(0); return 0; }
void sh_prefab_import_maintenance(void) { assert(0); }
sh_package_occurrence_snapshot *sh_package_occurrence_retain(sh_package_occurrence_snapshot *owner) { (void)owner; assert(0); return NULL; }
void sh_package_occurrence_release(sh_package_occurrence_snapshot *owner) { (void)owner; assert(0); }
int sh_package_editor_ready(void) { assert(0); return 0; }
int sh_package_editor_healthy(char *error, size_t capacity) { (void)error; (void)capacity; assert(0); return 0; }
sh_package_occurrence_snapshot *sh_package_editor_snapshot(unsigned kind, const void *flat, char *error, size_t capacity)
{ (void)kind; (void)flat; (void)error; (void)capacity; assert(0); return NULL; }
int sh_package_editor_attach(unsigned kind, const void *flat, const sh_package_occurrence_snapshot *owner, char *error, size_t capacity)
{ (void)kind; (void)flat; (void)owner; (void)error; (void)capacity; assert(0); return 0; }
void *sh_editor_transition_capture(const void *editor) { (void)editor; assert(0); return NULL; }
int sh_editor_transition_frame_ready(void *editor, void *map) { (void)editor; (void)map; assert(0); return 0; }
