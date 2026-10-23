#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define SH_RESOURCE_EDITOR_TESTING
#include "../src/backend/resource_graph_native.c"
#define SH_RESOURCE_LATE_MODEL_READS_TESTING
#include "../src/backend/resource_late_model_reads.c"
#include "native_pin_fixture.h"

static int failures, calls, throw_error;
static int test_vulkan;
static const uint8_t *late_host_base;
static int owner_renderer = -1, owner_pinned;
int sh_host_is_vulkan(void) { return owner_renderer >= 0 ? owner_renderer : test_vulkan; }
const uint8_t *sh_host_image_base(void) { return late_host_base?late_host_base:(const uint8_t *)(uintptr_t)0x140000000ull; }
size_t sh_host_image_size(void) { return 0x10000000; }
int sh_host_is_pinned_rva_build(void) { return 0; }
int sh_host_is_pinned_renderer_build(int vulkan) { return owner_pinned && vulkan == owner_renderer; }
void sh_resource_resident_touch(void *resource) { (void)resource; }
int sh_resource_light_origins_install(const uint8_t *base) { (void)base; return 1; }
int sh_resource_light_origins_ready(void) { return 1; }
int sh_resource_light_origins_reconcile(void *object,uint64_t token,char *error,size_t capacity)
{ (void)object;(void)token;(void)error;(void)capacity;return -1; }
int sh_resource_light_origins_uninstall(void) { return 1; }
int sh_resource_light_origins_refresh(int (*changed)(void *,void *),void *context,char *error,size_t capacity)
{ (void)changed; (void)context; (void)error; (void)capacity; return 1; }
static void *expected_entity, *expected_reader;
static unsigned char expected_initialize;
static unsigned char entity[0x6d8], definition[0x168], manager[16], reader_data[0x1208];
static const char canonical_text[] = "edit = { model = body; }";
static unsigned int parse_result;
static void *expected_resource;
static unsigned int read_result;
static int read_delegates, read_noop, read_calls;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
struct sh_package_resource_capture { int generation, references; };
struct sh_package_selector_hold { int generation; };
static int instance_captures, instance_leases, instance_promotions, instance_lease_generation, instance_pristine = 1;
static int instance_allowed = 1;
static int instance_source_faults;
static int instance_promotion_failure,instance_admission_failure,instance_admissions;
void sh_package_runtime_source_cleanup_failed(void) { instance_source_faults++; }
int sh_overrides_source_receipts_admit(void *const *resources,size_t count,char *error,size_t capacity)
{ (void)resources;(void)count;instance_admissions++;
    if(instance_admission_failure && error && capacity) snprintf(error,capacity,"source receipt remains unreconciled");
    return !instance_admission_failure; }
static sh_package_resource_capture *instance_capture(int generation)
{
    sh_package_resource_capture *capture = malloc(sizeof(*capture)); CHECK(capture);
    if (capture) { capture->generation = generation; capture->references = 1; instance_captures++; } return capture;
}
sh_package_resource_capture *sh_package_runtime_resource_capture_retain(sh_package_resource_capture *capture)
{ CHECK(capture && capture->references > 0); capture->references++; return capture; }
int sh_package_runtime_uninitialized(void) { return instance_pristine; }
int sh_package_runtime_resource_pristine(void) { return instance_pristine; }
int sh_package_runtime_resource_observation_allowed(void) { return instance_allowed; }
int sh_package_runtime_resource_promote(const sh_package_resource_capture *capture,
    sh_package_selector_hold **out, char *error, size_t capacity)
{
    (void)error; (void)capacity; CHECK(capture && out && !*out);
    if(instance_promotion_failure) return -1;
    *out = malloc(sizeof(**out)); if (!*out) return -1;
    (*out)->generation = capture->generation; instance_lease_generation = capture->generation; instance_leases++; instance_promotions++; return 1;
}
void sh_package_runtime_resource_capture_release(sh_package_resource_capture *capture)
{ CHECK(capture && instance_captures > 0 && capture->references > 0);
    if (!--capture->references) { instance_captures--; free(capture); } }
void sh_package_runtime_selectors_release(sh_package_selector_hold *hold)
{ CHECK(hold && instance_leases > 0); instance_leases--; free(hold); }

uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
static sh_package_consumers *font_catalog;
static int font_acquires, font_releases, font_holds, font_acquire_error;
int sh_package_runtime_consumer_acquire(const char *family, const char *name, sh_package_consumer_hold *out)
{
    CHECK(!out->provider && !out->binding);
    if (strcmp(family, "material")) font_acquires++;
    if (font_acquire_error) return -1;
    out->binding = sh_package_consumers_find(font_catalog, family, name);
    if (!out->binding) return 0;
    out->provider = font_catalog; font_holds++; return 1;
}
void sh_package_runtime_consumer_release(sh_package_consumer_hold *hold)
{
    if (hold->provider) { font_releases++; font_holds--; CHECK(font_holds >= 0 && hold->binding); }
    memset(hold, 0, sizeof(*hold));
}

/* Prepublication records are fixed fixtures: the actual APIs allocate no
 * memory on constructor/Call/destructor entry. Tokens survive pointer reuse. */
typedef struct fake_movie_record { const sh_package_consumer *binding; unsigned instances, frames; } fake_movie_record;
static fake_movie_record movie_records[4];
static int movie_begins, movie_borrows, movie_ends, movie_frame_releases;
static fake_movie_record *movie_record_for(const char *name)
{
    for (size_t i = 0; i < 4; i++) if (movie_records[i].binding && !strcmp(movie_records[i].binding->name, name)) return movie_records + i;
    return NULL;
}
static void movie_records_prepare(void)
{
    memset(movie_records, 0, sizeof(movie_records));
    for (size_t i = 0; i < sh_package_consumers_count(font_catalog); i++) {
        CHECK(i < 4); if (i >= 4) break;
        movie_records[i].binding = sh_package_consumers_at(font_catalog, i);
        CHECK(sh_package_movie_plan_valid(movie_records[i].binding));
    }
}
int sh_package_runtime_movie_begin(const char *name, sh_package_movie_hold *out)
{
    CHECK(!out->binding && !out->token); movie_begins++;
    fake_movie_record *record = movie_record_for(name); if (!record) return 0;
    if (!record->instances && !record->frames) { font_holds++; font_acquires++; }
    record->instances++; record->frames++; out->binding = record->binding; out->token = record; return 1;
}
int sh_package_runtime_movie_borrow(const char *name, sh_package_movie_hold *out)
{
    CHECK(!out->binding && !out->token); movie_borrows++;
    fake_movie_record *record = movie_record_for(name); if (!record || !record->instances) return 0;
    record->frames++; out->binding = record->binding; out->token = record; return 1;
}
/* The runtime's identity query over its live movie plans (the prepared records). */
int sh_package_runtime_font_identity(const char *font, int *hash)
{
    int found = 0, first = 0;
    for (size_t i = 0; i < 4 && found >= 0; i++) {
        const char *request = movie_records[i].binding ? sh_package_movie_font_request(movie_records[i].binding, font) : NULL;
        if (!request) continue;
        int value = sh_swf_font_name_hash(request);
        if (!found) { first = value; found = 1; } else if (value != first) found = -1;
    }
    if (found == 1) *hash = first;
    return found;
}
void sh_package_runtime_movie_end_instance(const sh_package_movie_hold *hold)
{
    fake_movie_record *record = hold->token; CHECK(record && record->instances);
    if (record && record->instances) { record->instances--; movie_ends++; }
}
void sh_package_runtime_movie_release(sh_package_movie_hold *hold)
{
    fake_movie_record *record = hold->token;
    if (record) {
        CHECK(record->frames); record->frames--; movie_frame_releases++;
        if (!record->frames && !record->instances) { font_holds--; font_releases++; }
    }
    memset(hold, 0, sizeof(*hold));
}
/* This suite executes the production callback against native-shaped memory.
 * Actual detour instructions are checked separately against both game images. */
static int movie_install_fake, movie_hook_prepares, movie_hook_commits, movie_hook_removes;
static int movie_hook_prepare_fault, movie_hook_commit_fault, movie_hook_remove_fault;
static int size_install_fake; /* The size-history controls: every prepare and commit succeeds. */
/* The copy-owner install controls count their own prepares, commits and removals. */
static int owner_install_fake, owner_prepares, owner_commits, owner_removes, owner_prepare_fault, owner_commit_fault;
static int late_install_fake;
static void *late_prepare(void *target,void *detour,size_t stolen);
static sh_patch_status late_commit(void *trampoline);
void *hook_prepare(void *target, void *detour, size_t stolen)
{
    if(late_install_fake)return late_prepare(target,detour,stolen);
    (void)detour; (void)stolen;
    if (owner_install_fake) { owner_prepares++; return owner_prepares == owner_prepare_fault ? NULL : target; }
    if (size_install_fake) return target;
    if (!movie_install_fake) return NULL;
    movie_hook_prepares++;
    return movie_hook_prepares == movie_hook_prepare_fault ? NULL : target;
}
void *hook_prepare_rip_lea(void *target, void *detour, size_t stolen, size_t lea_offset)
{ (void)lea_offset; return hook_prepare(target, detour, stolen); }
sh_patch_status hook_commit(void *trampoline)
{
    if(late_install_fake)return late_commit(trampoline);
    (void)trampoline;
    if (owner_install_fake) { owner_commits++; return owner_commits == owner_commit_fault ? (sh_patch_status)1 : B2_PATCH_OK; }
    if (size_install_fake || !movie_install_fake) return B2_PATCH_OK; movie_hook_commits++;
    CHECK(g_swf_construct && g_swf_destroy && g_swf_precache_sound && g_swf_precache_material && g_swf_play_sound && g_swf_local_string && g_dictionary_index &&
        g_fontfx_register);
    return movie_hook_commits == movie_hook_commit_fault ? (sh_patch_status)1 : B2_PATCH_OK;
}
int hook_unpatch(void *trampoline)
{
    (void)trampoline;
    if (owner_install_fake) { owner_removes++; return 1; }
    movie_hook_removes++; return movie_hook_removes != movie_hook_remove_fault;
}
int hook_is_installed(void *trampoline) { return trampoline!=NULL; }
void backend_log(const char *message) { (void)message; }

/* size_owners.c has its own suite (and the resident suite links the real module): here the hooks' calls into
 * it are observed. Unattributed reads are kept with the image history, as the module does. */
static sh_size_site so_site_class = SH_SIZE_SITE_UNKNOWN;
static int so_attribute, so_collect_depth, so_collect_failed, so_collect_ends, so_bind_result = 1, so_enabled;
static char so_collected[8][64];
static size_t so_collected_count;
static const void *so_collect_ret, *so_collect_frame;
static sh_size_measure so_measure;
static size_t so_hotbox_offset;
static int so_begins, so_ends, so_end_completed = -1;
static sh_size_bracket_kind so_kind;
static void *so_object;
static const void *so_frame;
static int so_width, so_height;
static char so_owner[64];
static void *so_explicit_object, *so_script_object, *so_sprite_gone, *so_hotbox_gone, *so_widget_gone, *so_movie_gone;
static unsigned so_explicit_axis, so_script_axis;
static int so_explicit_completed = -1;
int sh_size_owners_bind_sites(const uint8_t *base, int vulkan, const void *height, const void *width,
    const void *own_base, size_t own_size)
{ (void)base; (void)vulkan; (void)height; (void)width; (void)own_base; (void)own_size; return so_bind_result; }
void sh_size_owners_enable(sh_size_measure measure) { so_measure = measure; so_enabled = measure != NULL; }
void sh_size_owners_hotbox_layout(size_t offset) { so_hotbox_offset = offset; }
int sh_size_owners_enabled(void) { return so_enabled; }
sh_size_site sh_size_owners_site(const void *return_address) { (void)return_address; return so_site_class; }
void sh_size_owners_begin(sh_size_bracket *bracket, sh_size_bracket_kind kind, void *object, const char *owner,
    const void *frame, int width, int height)
{
    memset(bracket, 0, sizeof(*bracket));
    bracket->kind = kind; bracket->object = object;
    so_begins++; so_kind = kind; so_object = object; so_frame = frame; so_width = width; so_height = height;
    strcpy_s(so_owner, sizeof(so_owner), owner ? owner : "");
}
void sh_size_owners_end(sh_size_bracket *bracket, int completed)
{ (void)bracket; so_ends++; so_end_completed = completed; }
void sh_size_owners_collect_begin(sh_size_bracket *call, void *material, unsigned axis)
{ memset(call, 0, sizeof(*call)); call->object = material; call->axis = axis; so_collect_depth++; so_collected_count = 0; so_collect_failed = 0; }
int sh_size_owners_collecting(void) { return so_collect_depth > 0; }
void sh_size_owners_note(const char *image)
{
    if (!image) { so_collect_failed = 1; return; }
    if (so_collected_count < 8) strcpy_s(so_collected[so_collected_count++], 64, image);
}
int sh_size_owners_collect_end(sh_size_bracket *call, int completed, const void *return_address, const void *frame,
    char ***names, size_t *count)
{
    (void)call;
    so_collect_depth--; so_collect_ends++;
    so_collect_ret = return_address; so_collect_frame = frame;
    int ok = completed && !so_collect_failed;
    if (names) {
        *names = NULL; *count = 0;
        if (!ok) return 0;
        *names = calloc(so_collected_count + 1, sizeof(char *));
        for (size_t i = 0; i < so_collected_count; i++) (*names)[i] = _strdup(so_collected[i]);
        *count = so_collected_count;
        return 1;
    }
    if (ok && so_attribute) return 1;
    for (size_t i = 0; i < so_collected_count; i++)
        sh_resource_graph_history_mark("image", so_collected[i], SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    return 0;
}
void sh_size_owners_explicit(void *object, unsigned axis, int completed)
{ so_explicit_object = object; so_explicit_axis = axis; so_explicit_completed = completed; }
void sh_size_owners_script_read(void *object, unsigned axis) { so_script_object = object; so_script_axis = axis; }
void sh_size_owners_sprite_destroyed(void *sprite) { so_sprite_gone = sprite; }
void sh_size_owners_hotbox_destroyed(void *hotbox) { so_hotbox_gone = hotbox; }
void sh_size_owners_widget_destroyed(void *widget) { so_widget_gone = widget; }
void sh_size_owners_movie_destroyed(void *movie) { so_movie_gone = movie; }

static void put_pointer(unsigned char *buffer, size_t offset, const void *value)
{ memcpy(buffer + offset, &value, sizeof(value)); }
/* Native idSWF construction (VK 0x285a50 / GL 0x285060) interns this spelling
 * at swf+0, and ~idSWF logs it: "swf/" unless already present (exact case),
 * ASCII lower case and '\\' to '/', then ".swf" replaces the extension. Its
 * leading-slash trim cannot apply after that prefix. The generated .bswf
 * filename is only a constructor local. */
static void native_swf_name(const char *name, char out[256])
{
    size_t n = 0;
    if (strncmp(name, "swf/", 4)) { memcpy(out, "swf/", 4); n = 4; }
    for (; *name && n < 250; name++)
        out[n++] = *name == '\\' ? '/' : *name >= 'A' && *name <= 'Z' ? (char)(*name + 32) : *name;
    out[n] = 0;
    char *slash = strrchr(out, '/'), *dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash)) *dot = 0;
    strcat_s(out, 256, ".swf");
}
static void fake_state(void *tools, void *object, void *reader, unsigned char initialize)
{
    CHECK(tools == (void *)17 && object == expected_entity && reader == expected_reader);
    CHECK(initialize == expected_initialize);
    calls++;
    sh_resource_graph_file("deferred");
    if (throw_error) RaiseException(0xe0123456, 0, 0, NULL);
}
static unsigned int fake_parse(void *resource, unsigned char read_source, void *error_text)
{
    CHECK(resource == expected_resource && read_source == 7 && error_text == (void *)19);
    sh_resource_graph_file("deferred");
    if (throw_error) RaiseException(0xe0123456, 0, 0, NULL);
    return parse_result;
}
static unsigned int fake_read(void *resource, void *error_text)
{
    CHECK(resource == expected_resource && error_text == (void *)19);
    read_calls++;
    if (read_noop) return 0;
    sh_resource_graph_file("source");
    if (throw_error) RaiseException(0xe0123456, 0, 0, NULL);
    if (read_delegates) return rg_parse_hook(resource, 7, error_text);
    sh_resource_graph_file("deferred");
    return read_result;
}
static int visit(void *context, const char *pt, const char *pn, const char *type, const char *name)
{
    unsigned *bits = (unsigned *)context;
    (void)pt; (void)pn; (void)type;
    if (!strcmp(name, "source")) *bits |= 1;
    else if (!strcmp(name, "deferred")) *bits |= 2;
    return 1;
}
static void seed(const char *type, const char *name)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_begin(&frame, type, name);
    sh_resource_graph_file("source");
    sh_resource_graph_end(&frame, 1);
}
static int walk(const char *type, const char *name, unsigned *bits)
{ *bits = 0; return sh_resource_graph_walk(type, name, visit, bits); }
static const char shader_text[] = "{ Tex2D textures/source }\r\n";
static int shader_stream = 1;
static int shader_loads, shader_load_effect;
static void shader_set_text(void *resource, const void *text, int length)
{
    CHECK(text == shader_text && length == (int)strlen(shader_text));
    put_pointer(resource, 0x38, text); length -= 2;
    memcpy((char *)resource + 0x40, &length, 4);
}
static unsigned int shader_read(void *resource, void *error_text)
{
    (void)error_text;
    if (*(unsigned char *)((char *)resource + 0x48)) return 0;
    if (shader_stream) sh_package_shader_receipt_bytes("generated/decls/renderparm/example.decl",
        strlen(shader_text), 0, shader_text, strlen(shader_text));
    rg_set_text_hook(resource, shader_text, (int)strlen(shader_text));
    memset((char *)resource + 0x38, 0, 16); /* Production text retirement. */
    return read_result;
}
static unsigned int shader_parse(void *resource, unsigned char read_source, void *error_text)
{ (void)resource; CHECK(!read_source && !error_text); return parse_result; }
static int shader_matches(void)
{
    return sh_package_shader_receipt_matches((uintptr_t)definition, "renderparm", "example",
        shader_text, strlen(shader_text), definition, 0x90);
}
static int shader_admitted(void *context) { return context == (void *)1; }
static void shader_publish_empty(void *manager, void *parameter, const void *value)
{ (void)manager; (void)parameter; (void)value; }
static uint32_t shader_buffer_absent(void *manager, const char *name)
{ (void)manager; (void)name; return UINT32_MAX; }
static void shader_reconstruct(void *resource)
{ CHECK(resource == definition && !shader_matches()); }
static void shader_load(void *resource)
{
    shader_loads++;
    rg_read_hook(resource, NULL);
    if (shader_load_effect == 1) ((unsigned char *)resource)[0x58] ^= 1;
    if (shader_load_effect == 2) ((unsigned char *)resource)[0x2c] = 4;
    if (shader_load_effect == 3) RaiseException(0xe0123456, 0, 0, NULL);
}
static void test_shader_preparation(void)
{
    char error[256]; g_load = shader_load; read_result = 0;
    g_parm_publish = shader_publish_empty; g_parm_write = shader_publish_empty; g_buffer_name = shader_buffer_absent;
    definition[0x48] = definition[0x2c] = 0;
    memset(definition + 0x38, 0, 16);
    sh_package_shader_receipt_invalidate((uintptr_t)definition);
    CHECK(!sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
        shader_text, strlen(shader_text), shader_admitted, NULL, error, sizeof(error)) && shader_loads == 0);
    sh_resource_shader_state *scope = sh_resource_graph_native_shader_state_begin(shader_admitted, (void *)1, error, sizeof(error));
    CHECK(scope);
    CHECK(sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
        shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error)) && shader_loads == 1);
    CHECK(shader_matches()); /* Preexisting retired source gained an actual parse witness. */
    CHECK(sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
        shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error)) && shader_loads == 1);
    CHECK(sh_resource_graph_native_shader_state_end(scope, 1, error, sizeof(error)) == 1);
    g_reconstruct = shader_reconstruct; rg_reconstruct_hook(definition);
    CHECK(!shader_matches());
    for (shader_load_effect = 1; shader_load_effect <= 3; shader_load_effect++) {
        scope = sh_resource_graph_native_shader_state_begin(shader_admitted, (void *)1, error, sizeof(error));
        CHECK(scope);
        sh_package_shader_receipt_invalidate((uintptr_t)definition);
        CHECK(!sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
            shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error)));
        CHECK(!shader_matches() && *error);
        if (shader_load_effect == 2) {
            CHECK(sh_resource_graph_native_shader_state_end(scope, 0, error, sizeof(error)) == -1);
            CHECK(g_shader_state == scope); /* The caller still owns recovery. */
            definition[0x2c] = 0;
        }
        CHECK(sh_resource_graph_native_shader_state_end(scope, 0, error, sizeof(error)) == 1);
        CHECK(definition[0x58] == 0);
    }
    shader_load_effect = 0;
}
static int program_mode, program_loads;
static uintptr_t program_compile(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,
    uintptr_t e,uintptr_t f,uintptr_t g,uintptr_t h,uintptr_t i)
{
    CHECK(a==1 && b==2 && c==3 && d==4 && e==5 && f==6 && g==7 && h==8 && i==9);
    return 71;
}
static void program_write(const void *metadata,void *opaque)
{
    CHECK(metadata==definition+0x78);void **vtable=*(void ***)opaque;
    if(program_mode==2)((uintptr_t (*)(void *))vtable[7])(opaque);
    else {
        const unsigned char bytes[]={1,2,3,4};
        CHECK(((long long (*)(void *,const void *,uint64_t))vtable[6])(opaque,bytes,4)==4);
    }
}
static void program_load(void *resource)
{
    program_loads++;
    rg_set_text_hook(resource,shader_text,(int)strlen(shader_text));
    if(program_mode!=1)CHECK(rg_shader_compile_hook(1,2,3,4,5,6,7,8,9)==71);
    sh_package_shader_receipt_bytes("decls/renderprogs/captured.inc",4,0,"text",4);
    memset((unsigned char *)resource+0x38,0,16);
}
static void test_program_preparation(void)
{
    char error[256];g_load=program_load;g_set_text=shader_set_text;
    g_shader_compile=program_compile;g_shader_write=program_write;
    memset(definition,0,sizeof(definition));put_pointer(manager,8,"renderProg");
    put_pointer(definition,8,"example");put_pointer(definition,0x10,manager);
    for(program_mode=0;program_mode<3;program_mode++) {
        sh_resource_shader_state *scope=sh_resource_graph_native_shader_state_begin(shader_admitted,(void *)1,error,sizeof(error));
        CHECK(scope);
        sh_package_shader_receipt_invalidate((uintptr_t)definition);
        program_loads=0;
        rg_load_hook(definition); /* Actual cold construction, before the queued callback returns. */
        CHECK(program_loads==1);
        int ok=program_mode==0;
        CHECK(program_loads==1); /* A ready GPU program is never blindly reread. */
        CHECK(!sh_package_shader_receipt_program_current());
        sh_package_shader_capture capture={0};
        CHECK(sh_package_shader_receipt_program_copy((uintptr_t)definition,"example",shader_text,
            strlen(shader_text),definition,&capture)==ok);
        if(ok)CHECK(capture.metadata_length==4 && capture.inputs && capture.inputs->length==4);
        sh_package_shader_capture_clear(&capture);
        CHECK(sh_resource_graph_native_shader_state_end(scope,ok,error,sizeof(error))==1);
    }
    g_shader_write=NULL;g_shader_compile=NULL;
}
/* Injected native workspaces exercise ownership and admission independently of
 * GPU driver acceptance. The fixture never supplies a program constructor. */
static const char cook_root[]="{ inherit base }\r\n";
static const char cook_base[]="{ hlsl_vp { inherited(); } }\n";
static const char cook_include[]="#define INCLUDED 1\n";
static const char cook_child[]="generated/renderprogs/permutations/example_0000000000000002.decl";
static const char cook_formatted[]=
    "{\n\tpermutations {\n\t\toptions FIRST+SECOND\n\t\tdefault SECOND\n\t}\n"
    "\tparms { inheritedParm }\n\tstate { rootState }\n\tflags { rootFlag }\n"
    "\tinout { inheritedLink }\n\tcomputelayout { rootLayout }\n"
    "\tshaderstorage { inheritedStorage }\n\tcompileroptions { rootOption }\n"
    "\thlsl_vp_header {inheritedHeader\n}\n\thlsl_fp_header {rootHeader\n}\n"
    "\thlsl_cp_header {computeHeader\n}\n\thlsl_vp { inherited(); }\n"
    "\thlsl_fp { fragment(); }\n\thlsl_cp { compute(); }\n}\n";
static unsigned char cook_program[0x1800],cook_flags[2][0x60],cook_flag_names[2][0x30];
static uint64_t cook_mask=2;
static unsigned char cook_sparse,cook_aniso;
static int cook_feedback,cook_validate,cook_mode,cook_reads,cook_frees,cook_missing;
static int cook_strings,cook_stages,cook_metadata,cook_lexers,cook_texts,cook_compiles,cook_lookups;
static void *cook_fs_vtable[26],*cook_heap_vtable[3],**cook_fs=cook_fs_vtable,**cook_heap=cook_heap_vtable;
static void *cook_fs_slot=&cook_fs,*cook_heap_slot=&cook_heap;
typedef struct cook_lexer { const char *body; int length; unsigned flags; } cook_lexer;
static void cook_string(void *value,const char *body)
{
    unsigned char *p=value; free(*(void **)(p+0x10));
    *(char **)(p+0x10)=_strdup(body); *(int *)(p+8)=(int)strlen(body);
}
static void *cook_string_init(void *value) { memset(value,0,0x30);cook_strings++;return value; }
static void cook_string_clear(void *value)
{ free(*(void **)((char *)value+0x10));memset(value,0,0x30);cook_strings--; }
static void *cook_stage_init(void *value)
{
    memset(value,0,0x730);cook_stages++;
    cook_string_init((char *)value+0x6c8);cook_string_init((char *)value+0x6f8);
    put_pointer(value,0x728,malloc(1)); return value;
}
static void cook_stage_reset(void *value)
{ CHECK(*(void **)((char *)value+0x728));free(*(void **)((char *)value+0x728));put_pointer(value,0x728,NULL); }
static void cook_stage_clear(void *value)
{
    CHECK(!*(void **)((char *)value+0x728));cook_stages--;
    cook_string_clear((char *)value+0x6c8);cook_string_clear((char *)value+0x6f8);
}
static void *cook_metadata_init(void *value) { memset(value,0,0x17b0);cook_metadata++;return value; }
static void cook_metadata_clear(void *value) { (void)value;cook_metadata--; }
static void cook_set_text(void *value,const void *text,int length)
{
    unsigned char *p=value;
    if (*(void **)(p+0x38)) { free(*(void **)(p+0x38));cook_texts--; }
    memset(p+0x38,0,16);if (!text)return;
    while(length>0 && (((const char *)text)[length-1]=='\r' || ((const char *)text)[length-1]=='\n' ||
        ((const char *)text)[length-1]==' '))length--;
    char *copy=malloc((size_t)length+1);memcpy(copy,text,(size_t)length);copy[length]=0;
    put_pointer(p,0x38,copy);*(int *)(p+0x40)=length;*(uint32_t *)(p+0x44)=1234;cook_texts++;
}
static void *cook_lexer_init(void *value,unsigned flags)
{ CHECK(!flags);memset(value,0,0x720);cook_lexers++;return value; }
static void cook_lexer_clear(void *value) { (void)value;cook_lexers--; }
static void cook_lexer_flags(void *value,unsigned flags) { ((cook_lexer *)value)->flags=flags; }
static unsigned char cook_lexer_load(void *value,const void *body,int length,const char *label)
{
    cook_lexer *lexer=value;lexer->body=body;lexer->length=length;
    CHECK(length>0 && ((const char *)body)[length-1]!='\n');
    if(!strcmp(label,"generated/renderprogs/base.decl"))CHECK(lexer->flags==0x4c634);
    else {
        CHECK(lexer->flags==(test_vulkan?0x4c634:0x4c434));
        CHECK(!strcmp(label,"generated/decls/renderProg/example.decl") ||
            !strncmp(label,"generated/renderprogs/permutations/",34));
    }
    return 1;
}
static unsigned char cook_lexer_expect(void *value,const char *text)
{ CHECK(((cook_lexer *)value)->body[0]=='{' && !strcmp(text,"{"));return 1; }
static unsigned char cook_lexer_error(void *value)
{ return cook_mode==3 || (cook_mode==4 && strstr(((cook_lexer *)value)->body,"permutations")); }
static unsigned char cook_lexer_warning(void *value) { (void)value;return 0; }
static int cook_native_mode(void) { return 2; }
static int cook_original(void *context,const char *path,unsigned char **body,size_t *length)
{
    const char *text=NULL; CHECK(context==cook_program);
    *body=NULL;*length=0;
    if(!strcmp(path,"generated/decls/renderprog/example.decl"))text=cook_mode==15?"wrong":cook_root;
    if(!strcmp(path,"generated/decls/renderprog/base.decl"))text=cook_base;
    if(!strcmp(path,"decls/renderprogs/base.inc")) {
        if(cook_mode==12)return 0;
        if(cook_mode==13)return -1;
        if(cook_mode==14)RaiseException(0xe0420111,0,0,NULL);
        text=cook_include;
    }
    if(text) { *body=(unsigned char *)_strdup(text);*length=strlen(text); }
    return cook_mode==16?-1:!text?0:cook_mode==17?2:1;
}
static int cook_read_file(void *fs,const char *path,void **body,void *timestamp,unsigned flags)
{
    CHECK(fs==cook_fs_slot && !timestamp && !flags);cook_reads++;
    size_t length=0;
    int claimed=sh_resource_graph_native_cook_input(path,(unsigned char **)body,&length);
    if(claimed) { if(claimed<0)cook_missing++;return claimed>0?(int)length:-1; }
    CHECK(cook_mode<11); /* Captured missing/error inputs cannot use local bytes. */
    const char *text=NULL;
    if(!strcmp(path,"generated/decls/renderprog/example.decl") && cook_mode!=1)text=cook_root;
    if(!strcmp(path,"generated/decls/renderprog/base.decl") && cook_mode!=2)text=cook_base;
    if(!strcmp(path,"decls/renderprogs/base.inc"))text=cook_include;
    if(!text)cook_missing++;
    *body=text?_strdup(text):NULL;return text?(int)strlen(text):-1;
}
static void cook_free_file(void *heap,void *body,size_t alignment)
{ CHECK(heap==cook_heap_slot && alignment==16);free(body);cook_frees++; }
static void *cook_lookup(void *owner,const char *name)
{
    cook_lookups++;
    CHECK(owner==cook_flags); /* No original lookup of a live/cold program. */
    return !strcmp(name,"FIRST")?cook_flags[0]:!strcmp(name,"SECOND")?cook_flags[1]:NULL;
}
static void *cook_name(void *value,const char *name,uint64_t mask)
{
    CHECK(mask==2);cook_string_init(value);
    if(cook_mode==10) {
        char path[160];int index=!strcmp(name,"example")?1:atoi(strrchr(name,'/')+7)+1;
        snprintf(path,sizeof(path),"generated/renderprogs/permutations/chain_%03d.decl",index);cook_string(value,path);
    } else {
        CHECK(!strcmp(name,"example") || (cook_mode==9 && !strcmp(name,cook_child)));cook_string(value,cook_child);
    }
    return value;
}
static void cook_inherited_sections(unsigned char sections[7][0x30],void *vertex)
{
    cook_string(sections[0]," { inheritedParm }");cook_string(sections[3]," { inheritedLink }");
    cook_string(sections[5]," { inheritedStorage }");
    cook_string((char *)vertex+0x6c8,"inheritedHeader\n");cook_string((char *)vertex+0x6f8,"{ inherited(); }");
}
static void cook_own_sections(unsigned char sections[7][0x30],void *fragment,void *compute)
{
    cook_string(sections[1]," { rootState }");cook_string(sections[2]," { rootFlag }");
    cook_string(sections[4]," { rootLayout }");cook_string(sections[6]," { rootOption }");
    cook_string((char *)fragment+0x6c8,"rootHeader\n");cook_string((char *)fragment+0x6f8,"{ fragment(); }");
    cook_string((char *)compute+0x6c8,"computeHeader\n");cook_string((char *)compute+0x6f8,"{ compute(); }");
}
static uintptr_t cook_compile(uintptr_t out,uintptr_t name_arg,uintptr_t config,uintptr_t lex,
    uintptr_t checksum,uintptr_t sections_arg,uintptr_t vertex,uintptr_t fragment,uintptr_t compute)
{
    const char *name=(const char *)name_arg;unsigned char (*sections)[0x30]=(void *)sections_arg;
    cook_lexer *lexer=(void *)lex;unsigned char *metadata=(void *)out;
    CHECK(checksum==1234 && ((const unsigned char *)config)[7]==1 &&
        *(const uint32_t *)config==(test_vulkan?2u:0u));cook_compiles++;cook_metadata_init(metadata);
    if(!strcmp(name,"example") || (!strcmp(name,"base") && cook_mode==6)) {
        void *parent=rg_lookup_hook(manager,"base");unsigned char inherited[0x17b0];
        CHECK(parent && parent!=cook_program && !strcmp(((rg_cook_name *)parent)->name,"base"));
        CHECK(((void *(*)(void *))(*(void ***)parent)[2])(parent)==manager);
        CHECK(rg_shader_worker_hook(parent,inherited,"base",(void *)config,sections,
            (void *)vertex,(void *)fragment,(void *)compute)==(uintptr_t)inherited);
        cook_metadata_clear(inherited);
    }
    if(!strcmp(name,"base")) {
        cook_inherited_sections(sections,(void *)vertex);
        void *include=NULL;
        int bytes=cook_read_file(cook_fs_slot,cook_mode>=11?"/decls/renderprogs/base.inc":"decls/renderprogs/base.inc",&include,NULL,0);
        if(bytes<0)g_cook->failed=1;
        else {
            CHECK(bytes==(int)strlen(cook_include) && !memcmp(include,cook_include,(size_t)bytes));
            sh_package_shader_receipt_bytes("decls/renderprogs/base.inc",(size_t)bytes,0,include,(size_t)bytes);
            cook_free_file(cook_heap_slot,include,16);
        }
    } else if(!strcmp(name,"example")) {
        cook_own_sections(sections,(void *)fragment,(void *)compute);
        put_pointer(metadata,0x9d0,cook_flag_names);*(int *)(metadata+0x9d8)=2;
        *(uint64_t *)(metadata+0x1608)=3;put_pointer(metadata,0x15e8,&cook_mask);*(int *)(metadata+0x15f0)=1;
    } else {
        CHECK(!strcmp(name,cook_child) || (cook_mode==10 && strstr(name,"/chain_")));
        CHECK(lexer->length==(int)strlen(cook_formatted)-1 && !memcmp(lexer->body,cook_formatted,(size_t)lexer->length));
        if(cook_mode==5)cook_feedback++;
        if(cook_mode==9 || (cook_mode==10 && atoi(strrchr(name,'/')+7)<64)) {
            cook_inherited_sections(sections,(void *)vertex);cook_own_sections(sections,(void *)fragment,(void *)compute);
            put_pointer(metadata,0x9d0,cook_flag_names);*(int *)(metadata+0x9d8)=2;
            *(uint64_t *)(metadata+0x1608)=3;put_pointer(metadata,0x15e8,&cook_mask);*(int *)(metadata+0x15f0)=1;
        }
    }
    return out;
}
static void cook_write(const void *metadata,void *file)
{
    (void)metadata;void **vtable=*(void ***)file;
    if(cook_mode==7)((uintptr_t (*)(void *))vtable[7])(file);
    else CHECK(((long long (*)(void *,const void *,uint64_t))vtable[6])(file,"META",4)==4);
}
static void test_isolated_program_cook(void)
{
    char error[256];unsigned char original[sizeof(cook_program)];
    memset(cook_program,0xa5,sizeof(cook_program));memcpy(original,cook_program,sizeof(original));
    put_pointer(manager,8,"renderProg");memset(cook_flags,0,sizeof(cook_flags));
    *(uint64_t *)(cook_flags[0]+0x58)=1;*(uint64_t *)(cook_flags[1]+0x58)=2;
    put_pointer(cook_flag_names[0],0x10,"FIRST");*(int *)(cook_flag_names[0]+8)=5;
    put_pointer(cook_flag_names[1],0x10,"SECOND");*(int *)(cook_flag_names[1]+8)=6;
    g_set_text=cook_set_text;g_shader_compile=cook_compile;g_shader_write=cook_write;g_lookup=cook_lookup;
    g_parm_publish=shader_publish_empty;g_parm_write=shader_publish_empty;g_buffer_name=shader_buffer_absent;
    cook_fs_vtable[25]=cook_read_file;cook_heap_vtable[2]=cook_free_file;
    g_cook_api=(rg_cook_api){0};
    g_cook_api.string_init=cook_string_init;g_cook_api.string_clear=cook_string_clear;
    g_cook_api.stage_init=cook_stage_init;g_cook_api.stage_reset=cook_stage_reset;g_cook_api.stage_clear=cook_stage_clear;
    g_cook_api.metadata_init=cook_metadata_init;g_cook_api.metadata_clear=cook_metadata_clear;
    g_cook_api.lexer_init=cook_lexer_init;g_cook_api.lexer_clear=cook_lexer_clear;
    g_cook_api.lexer_flags=cook_lexer_flags;g_cook_api.lexer_load=cook_lexer_load;g_cook_api.lexer_expect=cook_lexer_expect;
    g_cook_api.lexer_error=cook_lexer_error;g_cook_api.lexer_warning=cook_lexer_warning;
    g_cook_api.mode=cook_native_mode;g_cook_api.permutation_name=cook_name;
    g_cook_api.filesystem=&cook_fs_slot;g_cook_api.heap=&cook_heap_slot;g_cook_api.flag_manager=cook_flags;
    g_cook_api.sparse=&cook_sparse;g_cook_api.aniso=&cook_aniso;
    g_cook_api.feedback=&cook_feedback;g_cook_api.validate=&cook_validate;g_cook_api.ready=1;
    for(test_vulkan=0;test_vulkan<2;test_vulkan++)for(cook_mode=0;cook_mode<18;cook_mode++) {
        g_cook_api.renderer=test_vulkan;cook_reads=cook_frees=cook_compiles=cook_lookups=cook_missing=0;cook_feedback=0;
        sh_resource_shader_state *scope=sh_resource_graph_native_shader_state_begin(shader_admitted,(void *)1,error,sizeof(error));
        CHECK(scope);program_loads=0;
        int ok=sh_resource_graph_native_prepare_program(manager,"example",cook_mode==8?"wrong":cook_root,
            cook_mode==8?5:strlen(cook_root),cook_mode>=11?cook_original:NULL,cook_program,
            shader_admitted,(void *)1,error,sizeof(error));
        CHECK(ok==(cook_mode==0 || cook_mode==10 || cook_mode==11 || cook_mode==17));CHECK(!program_loads && !g_cook && !memcmp(original,cook_program,sizeof(original)));
        CHECK(!cook_strings && !cook_stages && !cook_metadata && !cook_lexers && !cook_texts);
        CHECK(cook_frees==cook_reads-cook_missing);
        unsigned char *outside=NULL;size_t outside_length=0;
        CHECK(!sh_resource_graph_native_cook_input("generated/decls/renderprog/example.decl",&outside,&outside_length));
        CHECK(!outside && !outside_length);
        sh_package_shader_capture capture={0};
        CHECK(sh_package_shader_cook_copy(test_vulkan,"example",cook_root,strlen(cook_root),&capture)==ok);
        if(ok) {
            CHECK(cook_compiles==(cook_mode==10?66:3) && cook_lookups==(cook_mode==10?256:4) && capture.metadata_length==4);
            int parent=0,include=0;
            for(sh_package_shader_input *input=capture.inputs;input;input=input->next) {
                if(!strcmp(input->path,"generated/decls/renderprog/base.decl"))parent++;
                if(!strcmp(input->path,"decls/renderprogs/base.inc"))include++;
            }
            CHECK(parent==1 && include==1);sh_package_shader_capture_clear(&capture);
            CHECK(sh_package_shader_cook_copy(test_vulkan,cook_mode==10?
                "generated/renderprogs/permutations/chain_064.decl":cook_child,cook_formatted,strlen(cook_formatted),&capture));
        }
        sh_package_shader_capture_clear(&capture);
        CHECK(sh_resource_graph_native_shader_state_end(scope,ok,error,sizeof(error))==1);
    }
    test_vulkan=0;g_cook_api.ready=0;g_shader_write=NULL;g_shader_compile=NULL;
}
static void test_shader_receipts(void)
{
    memset(definition, 0, sizeof(definition)); put_pointer(definition, 8, "example");
    put_pointer(definition, 0x10, manager); put_pointer(manager, 8, "renderParm");
    g_read = shader_read; g_parse = shader_parse; g_set_text = shader_set_text; read_result = parse_result = 0;
    CHECK(!shader_matches()); /* Preexisting retired text is not proof. */
    CHECK(rg_read_hook(definition, NULL) == 0 && shader_matches());
    shader_stream = 0; CHECK(rg_read_hook(definition, NULL) == 0 && shader_matches());
    read_result = 3; CHECK(rg_read_hook(definition, NULL) == 3 && !shader_matches());
    read_result = 0; CHECK(rg_read_hook(definition, NULL) == 0 && shader_matches());
    definition[0x48] = 1; put_pointer(definition, 0x38, shader_text);
    int length = (int)strlen(shader_text) - 2; memcpy(definition + 0x40, &length, 4);
    CHECK(rg_parse_hook(definition, 0, NULL) == 0 && shader_matches());
    CHECK(rg_read_hook(definition, NULL) == 0 && shader_matches()); /* Implicit no-op. */
    parse_result = 2; CHECK(rg_parse_hook(definition, 0, NULL) == 2 && !shader_matches());
    parse_result = 0; sh_package_shader_receipt_invalidate((uintptr_t)definition);
}
static unsigned char parm_manager[0x10198], parm_states[2][0xb980];
static unsigned char buffer_manager[8 + 16 * 0x20], buffers[2][0x50];
static void *state_list[2];
static int preserve_mode, publishes, state_writes, fallback_reconstructs;
static void preserve_write(void *opaque, void *parameter, const void *value)
{
    unsigned char *state = opaque;
    uint32_t index; memcpy(&index, (unsigned char *)parameter + 0x68, 4);
    CHECK(index == 5); state_writes++;
    memcpy(state + 0x70 + (size_t)index * 16, value, 16);
    (*(uint64_t *)state)++; /* Native dirty counters are advanced, not rewound. */
}
static void preserve_publish(void *opaque, void *parameter, const void *value)
{
    CHECK(opaque == parm_manager); publishes++;
    for (size_t i = 0; i < 2; i++) preserve_write(state_list[i], parameter, value);
}
static uint32_t preserve_buffer(void *opaque, const char *name)
{ CHECK(opaque == buffer_manager && !strcmp(name, "example")); return 0; }
static unsigned int preserve_read(void *resource, void *error_text)
{
    shader_read(resource, error_text);
    if (preserve_mode == 1 || preserve_mode == 2) ((unsigned char *)resource)[0x58] = 77;
    if (preserve_mode == 2) ((unsigned char *)resource)[0x6c] = 9;
    rg_parm_publish_hook(parm_manager, resource, (unsigned char *)resource + 0x58);
    if (preserve_mode != 3 && rg_buffer_name_hook(buffer_manager, "example") == 0)
        for (size_t i = 0; i < 2; i++) put_pointer(buffers[i], test_vulkan ? 0x48 : 0x18, "replacement type");
    return preserve_mode == 1 ? 5 : 0;
}
static void preserve_load(void *resource)
{
    /* Match GenericLoad's native branch: a Read failure would reconstruct the
     * resource and destroy its assigned ABI. The protected wrapper prevents it. */
    if (rg_read_hook(resource, NULL)) {
        fallback_reconstructs++;
        ((unsigned char *)resource)[0x68] = 0xff;
    }
}
static void preserve_reset(void)
{
    memset(parm_manager, 0, sizeof(parm_manager)); memset(parm_states, 0, sizeof(parm_states));
    memset(definition, 0, sizeof(definition)); memset(buffer_manager, 0, sizeof(buffer_manager));
    memset(buffers, 0, sizeof(buffers));
    put_pointer(manager, 8, "renderParm"); put_pointer(definition, 8, "example");
    put_pointer(definition, 0x10, manager); definition[0x68] = 5;
    for (size_t i = 0; i < 2; i++) {
        state_list[i] = parm_states[i];
        memset(parm_states[i] + 0xc0, (int)i + 11, 16);
        put_pointer(buffers[i], 0x18, "original type"); put_pointer(buffers[i], 0x48, "original type");
    }
    put_pointer(parm_manager, 0x10188, state_list); parm_manager[0x10190] = 2;
    put_pointer(buffer_manager, 8, "example"); buffer_manager[0x10] = 1;
    put_pointer(buffer_manager, 0x18, buffers[0]); put_pointer(buffer_manager, 0x20, buffers[1]);
    g_parm_publish = preserve_publish; g_parm_write = preserve_write; g_buffer_name = preserve_buffer;
    g_load = preserve_load; g_read = preserve_read; g_set_text = shader_set_text;
    read_result = 0; publishes = state_writes = fallback_reconstructs = 0;
    sh_package_shader_receipt_invalidate((uintptr_t)definition);
}
static void test_shader_state(void)
{
    char error[256];
    for (test_vulkan = 0; test_vulkan < 2; test_vulkan++) for (preserve_mode = 0; preserve_mode < 4; preserve_mode++) {
        preserve_reset();
        if (preserve_mode == 3) parm_states[1][0xb974] = 1; /* Active parameter block. */
        sh_resource_shader_state *scope = sh_resource_graph_native_shader_state_begin(shader_admitted, (void *)1, error, sizeof(error));
        CHECK(scope);
        int ok = sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
            shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error));
        CHECK(ok == (preserve_mode == 0));
        CHECK(!fallback_reconstructs && definition[0x68] == 5 && definition[0x6c] == 0 && definition[0x58] == 0);
        CHECK(publishes == (preserve_mode != 3)); /* Stage the entire fanout before mutation. */
        CHECK(sh_resource_graph_native_shader_state_end(scope, ok, error, sizeof(error)) == 1);
        CHECK(shader_matches() == ok);
        for (size_t i = 0; i < 2; i++) {
            unsigned char expected[16]; memset(expected, (int)i + 11, 16);
            CHECK(!memcmp(parm_states[i] + 0xc0, expected, 16));
            CHECK(!strcmp(*(const char **)(buffers[i] + (test_vulkan ? 0x48 : 0x18)), "original type"));
        }
        if (preserve_mode != 3) CHECK(state_writes >= 4 && *(uint64_t *)parm_states[0] >= 2);
    }
    preserve_mode = 0; preserve_reset();
    sh_resource_shader_state *scope = sh_resource_graph_native_shader_state_begin(shader_admitted, (void *)1, error, sizeof(error));
    CHECK(sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
        shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error)));
    state_list[1] = NULL; /* A lost registration cannot be called successful restoration. */
    CHECK(sh_resource_graph_native_shader_state_end(scope, 1, error, sizeof(error)) == -1);
    CHECK(g_shader_state == scope && !shader_matches());
    state_list[1] = parm_states[1];
    CHECK(sh_resource_graph_native_shader_state_end(scope, 1, error, sizeof(error)) == 0);
    CHECK(!g_shader_state && !shader_matches());
    preserve_reset();
    uint32_t unassigned = 0x93f; memcpy(definition + 0x68, &unassigned, 4);
    scope = sh_resource_graph_native_shader_state_begin(shader_admitted, (void *)1, error, sizeof(error));
    CHECK(!sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
        shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error)));
    CHECK(!publishes && !fallback_reconstructs && strstr(error, "assigned native parameter slot"));
    CHECK(sh_resource_graph_native_shader_state_end(scope, 0, error, sizeof(error)) == 1);
    test_vulkan = 0;
}
/* R-400: a first registration deferred past the menu. At the first SnapMap browser the render thread holds an
 * active parameter block, so preparing a fixed renderparm with a live value there is refused, exactly the live
 * "native shader preparation failed; prior provider recovery is required" of the deferred VK leg. Prepared inside
 * the boot promotion (no active block), the same object later prepares at the browser from its parse witness
 * without reloading or publishing anything. */
static void test_deferred_registration_shader_window(void)
{
    char error[256];
    for (test_vulkan = 0; test_vulkan < 2; test_vulkan++) {
        /* Without the promotion-time preparation: refused at the browser, state and value intact. */
        preserve_mode = 0; preserve_reset();
        parm_states[1][0xb974] = 1; /* the browser: the render thread's active parameter block */
        sh_resource_shader_state *scope = sh_resource_graph_native_shader_state_begin(shader_admitted, (void *)1, error, sizeof(error));
        CHECK(scope);
        CHECK(!sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
            shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error)));
        CHECK(strstr(error, "native shader preparation failed; prior provider recovery is required") != NULL);
        CHECK(!publishes && !shader_matches());
        CHECK(sh_resource_graph_native_shader_state_end(scope, 0, error, sizeof(error)) == 1);
        for (size_t i = 0; i < 2; i++) {
            unsigned char expected[16]; memset(expected, (int)i + 11, 16);
            CHECK(!memcmp(parm_states[i] + 0xc0, expected, 16));
        }
        /* With it: inside the promotion every parameter state is idle and the preparation records its witness. */
        preserve_reset();
        scope = sh_resource_graph_native_shader_state_begin(shader_admitted, (void *)1, error, sizeof(error));
        CHECK(scope && sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
            shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error)));
        CHECK(sh_resource_graph_native_shader_state_end(scope, 1, error, sizeof(error)) == 1 && shader_matches());
        int published = publishes;
        /* ...and the browser's start prepares the same object from that witness: no reload, no publication. */
        parm_states[1][0xb974] = 1;
        scope = sh_resource_graph_native_shader_state_begin(shader_admitted, (void *)1, error, sizeof(error));
        CHECK(scope && sh_resource_graph_native_prepare_shader(definition, "renderparm", "example",
            shader_text, strlen(shader_text), shader_admitted, (void *)1, error, sizeof(error)));
        CHECK(publishes == published && !fallback_reconstructs);
        CHECK(sh_resource_graph_native_shader_state_end(scope, 1, error, sizeof(error)) == 1 && shader_matches());
        parm_states[1][0xb974] = 0;
    }
    test_vulkan = 0; preserve_mode = 0; preserve_reset();
}
static void invoke(void *object, void *reader, unsigned char initialize)
{
    expected_entity = object; expected_reader = reader; expected_initialize = initialize;
    rg_state_hook((void *)17, object, reader, initialize);
}
static void test_production_read(void)
{
    sh_resource_graph_frame outer;
    unsigned bits;
    int caught = 0;
    g_read = fake_read; g_parse = fake_parse;
    expected_resource = definition;
    put_pointer(manager, 8, "material");
    put_pointer(definition, 0x10, manager);
    definition[0x48] = 0;

    /* Production reads call the typed parser without the direct-source hook. */
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(read_calls == 1 && walk("material", "example", &bits) && bits == 3);

    /* Development reads delegate after opening their file. Keep both edges,
     * including when GenericLoad already owns the outer source lifetime. */
    read_delegates = 1;
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(walk("material", "example", &bits) && bits == 3);
    sh_resource_graph_begin(&outer, "material", "example");
    sh_resource_graph_file("source");
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(sh_resource_graph_source_active("material", "example"));
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "example", &bits) && bits == 3);

    parse_result = 9;
    CHECK(rg_read_hook(definition, (void *)19) == 9);
    CHECK(!walk("material", "example", &bits));
    parse_result = 0; read_delegates = 0; read_result = 7;
    CHECK(rg_read_hook(definition, (void *)19) == 7);
    CHECK(!walk("material", "example", &bits));
    read_result = 0; throw_error = 1;
    __try { rg_read_hook(definition, (void *)19); }
    __except (GetExceptionCode() == 0xe0123456 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && !sh_resource_graph_recording());
    CHECK(!walk("material", "example", &bits));
    throw_error = 0;

    /* A successful implicit no-op must neither invent nor clear dependencies. */
    read_noop = 1; definition[0x48] = 1;
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(!walk("material", "example", &bits));
    seed("material", "example");
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(walk("material", "example", &bits) && bits == 1);
    sh_resource_graph_begin(&outer, "material", "read-parent");
    sh_resource_graph_file("source");
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(sh_resource_graph_source_active("material", "read-parent"));
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "read-parent", &bits) && bits == 1);

    /* Native temporary/invalid identities remain transparent call-throughs. */
    read_noop = 0; definition[0x48] = 0;
    put_pointer(definition, 0x10, NULL);
    sh_resource_graph_begin(&outer, "material", "read-parent");
    sh_resource_graph_file("source");
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "read-parent", &bits) && bits == 1);
    CHECK(walk("material", "example", &bits) && bits == 1);
    expected_resource = (void *)1; read_noop = 1;
    CHECK(rg_read_hook((void *)1, (void *)19) == 0);
    CHECK(!sh_resource_graph_recording());
    put_pointer(definition, 0x10, manager);
    sh_resource_graph_test_reset();
}
static unsigned char font_objects[3][0x60], font_materials[4][0x60];
static unsigned char font_manager[0x30], font_material_manager[0x30];
static void *font_head;
static int font_body_calls, font_prepares, font_optional_calls, font_unbound_calls;
static int font_failure, font_nested, font_throw, font_route_failure;
static unsigned int font_native_result;
static const char *const font_names[] = {"private/outer", "private/inner", "stock"};
static const char *const font_material_names[] = {"private/main", "private/label", "private/child", "stock/material"};
static void *font_materialize(void *owner, const char *name, unsigned char create_default, unsigned char flags)
{
    CHECK(owner == font_material_manager && !flags);
    if (!strncmp(name, "private/", 8)) {
        CHECK(!g_font_frame && !create_default); font_prepares++;
        if (font_throw == 1) RaiseException(0xe0123456, 0, 0, NULL);
        if (font_failure == 1) return NULL;
        for (size_t i = 0; i < 3; i++) if (!strcmp(name, font_material_names[i])) {
            font_materials[i][0x2c] = font_failure == 2 ? 4 : font_failure == 3 ? 2 : font_failure == 5 ? 1 : 0;
            put_pointer(font_materials[i], 0x18, font_failure == 4 ? (void *)1 : NULL);
            return font_materials[i];
        }
        CHECK(0); return NULL;
    }
    font_unbound_calls++; return font_materials[3];
}
static void *font_optional(void *owner, const char *name, unsigned char create_default)
{
    CHECK(owner == font_material_manager && name && !create_default);
    font_optional_calls++; return font_materials[3];
}
static void font_nested_load(void *resource)
{
    CHECK(resource == font_materials[3] && !g_font_frame);
    CHECK(rg_materialize_hook(font_material_manager, "fonts/private/outer/48.tga", 1, 0) == font_materials[3]);
}
static unsigned int font_nested_parse(void *resource, unsigned char source, void *error)
{
    CHECK(!source && !error); font_nested_load(resource); return 0;
}
static unsigned int font_nested_read(void *resource, void *error)
{ CHECK(!error); font_nested_load(resource); return 0; }
static void font_nested_state(void *tools, void *object, void *reader, unsigned char initialize)
{
    CHECK(!tools && !object && !reader && !initialize && !g_font_frame);
    font_nested_load(font_materials[3]);
}
static unsigned int font_native_read(void *resource, void *error)
{
    CHECK(error == (void *)19); font_body_calls++;
    if (resource == font_objects[2] || resource == (void *)1) {
        CHECK(!g_font_frame); return font_native_result;
    }
    CHECK(g_font_frame && font_holds > 0);
    if (font_throw == 2) RaiseException(0xe0123456, 0, 0, NULL);
    if (resource == font_objects[1]) {
        CHECK(rg_materialize_hook(font_material_manager, "fonts/private/inner/48.tga", 1, 0) == font_materials[2]);
        CHECK(!rg_optional_material_hook(font_material_manager, "snapmap/private/inner_snapLabel", 0));
        return 0;
    }
    CHECK(resource == font_objects[0]);
    CHECK(rg_materialize_hook(font_material_manager, "fonts/private/outer/48.tga", 1, 0) == font_materials[0]);
    CHECK(rg_materialize_hook(font_material_manager, "fonts/private/outer/64_df.tga", 1, 0) == font_materials[0]);
    CHECK(rg_optional_material_hook(font_material_manager, "snapmap/private/outer_snapLabel", 0) == font_materials[1]);
    CHECK(!rg_optional_material_hook(font_material_manager, "snapmap/private/outer_worldText", 0));
    CHECK(rg_optional_material_hook(font_material_manager, "unrelated", 0) == font_materials[3]);
    if (font_route_failure)
        CHECK(!rg_materialize_hook(font_material_manager, "fonts/private/outer/12.tga", 1, 0));
    if (font_nested) {
        rg_font_frame *parent = g_font_frame;
        rg_load_hook(font_materials[3]); CHECK(g_font_frame == parent);
        CHECK(!rg_parse_hook(font_materials[3], 0, NULL) && g_font_frame == parent);
        CHECK(!rg_read_hook(font_materials[3], NULL) && g_font_frame == parent);
        rg_state_hook(NULL, NULL, NULL, 0); CHECK(g_font_frame == parent);
        CHECK(!rg_font_read_hook(font_objects[1], error) && g_font_frame == parent && font_holds == 1);
        CHECK(!rg_font_read_hook(font_objects[2], error) && g_font_frame == parent && font_holds == 1);
    }
    return font_native_result;
}
static void test_font_construction(void)
{
    char error[256];
    sh_package_consumer_target outer[] = {
        {"font/material/48", "material", font_material_names[0]},
        {"font/material/64_df", "material", font_material_names[0]},
        {"font/snapLabel", "material", font_material_names[1]}};
    sh_package_consumer_target inner = {"font/material/48", "material", font_material_names[2]};
    sh_package_consumer owners[] = {{"font", font_names[0], outer, 3}, {"font", font_names[1], &inner, 1}};
    font_catalog = sh_package_consumers_create(owners, 2, error, sizeof(error)); CHECK(font_catalog);
    memset(font_manager, 0, sizeof(font_manager)); memset(font_material_manager, 0, sizeof(font_material_manager));
    put_pointer(font_manager, 8, "font"); put_pointer(font_manager, 0x18, font_material_manager);
    put_pointer(font_material_manager, 8, "material"); font_head = font_manager; g_resource_head = &font_head;
    for (size_t i = 0; i < 3; i++) {
        put_pointer(font_objects[i], 8, font_names[i]); put_pointer(font_objects[i], 0x10, font_manager);
    }
    for (size_t i = 0; i < 4; i++) {
        put_pointer(font_materials[i], 8, font_material_names[i]); put_pointer(font_materials[i], 0x10, font_material_manager);
    }
    g_ready = 1; g_font_read = font_native_read; g_materialize = font_materialize; g_optional_material = font_optional;
    CHECK(sh_resource_graph_native_consumer_ready("font"));
    CHECK(!sh_resource_graph_native_consumer_ready("file") && !sh_resource_graph_native_consumer_ready(NULL));
    g_resource_head = NULL; CHECK(!sh_resource_graph_native_consumer_ready("font")); g_resource_head = &font_head;
    g_load = font_nested_load; g_parse = font_nested_parse; g_read = font_nested_read; g_state = font_nested_state;
    CHECK(!rg_font_read_hook(font_objects[0], (void *)19));
    CHECK(font_body_calls == 1 && font_prepares == 2 && font_optional_calls == 1);
    CHECK(font_acquires == 1 && font_releases == 1 && !font_holds && !g_font_frame);
    /* One held complete plan serves equal targets once and captures absence;
     * nested generic/direct/instance work cannot borrow it. A child font gets
     * its own plan, while an uncompiled child preserves ordinary resolution. */
    font_nested = 1;
    CHECK(!rg_font_read_hook(font_objects[0], (void *)19));
    CHECK(font_body_calls == 4 && font_prepares == 5 && font_optional_calls == 2 && font_unbound_calls == 4);
    CHECK(font_acquires == 4 && font_releases == 3 && !font_holds && !g_font_frame);
    font_nested = 0;
    for (font_failure = 1; font_failure <= 5; font_failure++) {
        int bodies = font_body_calls, releases = font_releases;
        CHECK(rg_font_read_hook(font_objects[0], (void *)19) != 0);
        CHECK(font_body_calls == bodies && font_releases == releases + 1 && !font_holds && !g_font_frame);
    }
    font_failure = 0;
    for (font_throw = 1; font_throw <= 2; font_throw++) {
        int caught = 0, releases = font_releases;
        __try { rg_font_read_hook(font_objects[0], (void *)19); }
        __except (GetExceptionCode() == 0xe0123456 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
        CHECK(caught && font_releases == releases + 1 && !font_holds && !g_font_frame && !sh_resource_graph_recording());
    }
    font_throw = 0; font_route_failure = 1;
    CHECK(rg_font_read_hook(font_objects[0], (void *)19) != 0 && !font_holds && !g_font_frame);
    font_route_failure = 0; font_native_result = 3;
    CHECK(rg_font_read_hook(font_objects[0], (void *)19) == 3);
    CHECK(rg_font_read_hook(font_objects[2], (void *)19) == 3);
    CHECK(rg_font_read_hook((void *)1, (void *)19) == 3); /* Unknown native identity stays a call-through. */
    font_native_result = 0; font_acquire_error = 1;
    int bodies = font_body_calls;
    CHECK(rg_font_read_hook(font_objects[0], (void *)19) != 0 && font_body_calls == bodies && !font_holds);
    font_acquire_error = 0;
    g_ready = 0;
    int acquisitions = font_acquires;
    CHECK(!sh_resource_graph_native_consumer_ready("font"));
    CHECK(!rg_font_read_hook(font_objects[2], (void *)19) && font_acquires == acquisitions);
    g_ready = 1;
    rg_font_frame parent = {0}; g_font_frame = &parent;
    CHECK(!rg_font_read_hook(font_objects[2], (void *)19) && g_font_frame == &parent);
    g_font_frame = NULL;
    sh_package_consumers_free(font_catalog); font_catalog = NULL;
    CHECK(!rg_font_read_hook(font_objects[2], (void *)19) && !font_holds);
    sh_resource_graph_test_reset();
}
static unsigned char movie_swf[0x40], movie_fonts[3][0x60], movie_materials[3][0x60];
static unsigned char movie_font_manager[0x30], movie_material_manager[0x30];
static void *movie_head;
static int movie_prepares, movie_native_fonts, movie_native_materials, movie_constructs;
static int movie_prepare_failure, movie_throw, movie_expect_frame, movie_nested;
static void *movie_materialize(void *owner, const char *name, unsigned char create_default, unsigned char flags)
{
    CHECK(!flags);
    if (owner == movie_font_manager) {
        if (!strcmp(name, "smp/font1")) {
            CHECK(g_movie_frame && create_default); movie_prepares++;
            movie_fonts[0][0x2c] = movie_prepare_failure ? 4 : 0;
            return movie_fonts[0];
        }
        movie_native_fonts++; return movie_fonts[2];
    }
    CHECK(owner == movie_material_manager);
    if (!strcmp(name, "smp/mat1")) {
        CHECK(!g_movie_frame && !create_default); movie_prepares++; return movie_materials[0];
    }
    movie_native_materials++; return movie_materials[2];
}
static void *movie_optional(void *owner, const char *name, unsigned char create_default)
{
    CHECK(owner == movie_material_manager && name); (void)create_default;
    if (!strcmp(name, "smp/mat1")) { movie_prepares++; return movie_materials[0]; }
    movie_native_materials++; return movie_materials[2];
}
static void movie_nested_load(void *resource)
{
    /* Nested construction work never borrows the movie plan. */
    CHECK(!g_movie_frame && resource == movie_materials[2]);
    CHECK(rg_materialize_hook(movie_font_manager, "TT Supermolot", 1, 0) == movie_fonts[2]);
}
static void *movie_construct(void *swf, const char *name, void *parent, unsigned char flags)
{
    CHECK(swf == movie_swf && !parent && flags == 3); movie_constructs++;
    static char native[256];
    if (name) native_swf_name(name, native); else *native = 0;
    put_pointer(movie_swf, 0, native);
    if (!movie_expect_frame) {
        CHECK(!g_movie_frame);
        CHECK(rg_materialize_hook(movie_font_manager, "TT Supermolot", 1, 0) == movie_fonts[2]);
        return swf;
    }
    CHECK(g_movie_frame && font_holds == 1);
    /* The language resolver and fixed lookups reach the prepared bindings. */
    CHECK(rg_materialize_hook(movie_font_manager, "TT Supermolot", 1, 0) == movie_fonts[0]);
    CHECK(rg_optional_material_hook(movie_material_manager, "guiSolid", 1) == movie_materials[0]);
    /* Original bindings and unrelated requests keep native lookup. */
    CHECK(rg_optional_material_hook(movie_material_manager, "_white", 1) == movie_materials[2]);
    CHECK(rg_materialize_hook(movie_font_manager, "V7Gothic-Medium-P", 1, 0) == movie_fonts[2]);
    if (movie_nested) {
        rg_movie_frame *frame = g_movie_frame;
        rg_load_hook(movie_materials[2]); CHECK(g_movie_frame == frame);
    }
    if (movie_throw) RaiseException(0xe0123457, 0, 0, NULL);
    return swf;
}
static void movie_destroy(void *swf) { CHECK(swf == movie_swf); }
static void test_movie_native_names(void)
{
    /* Retained VK RegisterFontFX objects (r2-vk-served-1/-3) carry these swf+0
     * spellings; the same runs began their plans under these generated names. */
    static const char *observed[][2] = {
        {"swf/smp/97b699571aa9aee4b9a89c00986fbb/c260c7b06db253596165302d6e0e46/499d.swf",
         "generated/swf/smp/97b699571aa9aee4b9a89c00986fbb/c260c7b06db253596165302d6e0e46/499d.bswf"},
        {"swf/smp/99550851facbf9d1cdd24c7ab89908/6457555cc78d4d670659c9f7e8d63d/1703.swf",
         "generated/swf/smp/99550851facbf9d1cdd24c7ab89908/6457555cc78d4d670659c9f7e8d63d/1703.bswf"}};
    static const char *spellings[] = {"hud/panel", "HUD\\Panel.gfx", "swf/hud/panel.swf", "swf/Hud/Panel", "SWF/hud/panel"};
    sh_package_movie_names names, again; char native[256];
    for (size_t i = 0; i < 2; i++) {
        CHECK(sh_package_movie_names_for_consumer(observed[i][0], strlen(observed[i][0]), &names) == 1 &&
            !strcmp(names.movie, observed[i][1]));
        native_swf_name(observed[i][0], native); CHECK(!strcmp(native, observed[i][0]));
    }
    /* A constructor argument and its interned spelling name one movie file. */
    for (size_t i = 0; i < sizeof(spellings) / sizeof(*spellings); i++) {
        native_swf_name(spellings[i], native);
        CHECK(sh_package_movie_names_for_consumer(spellings[i], strlen(spellings[i]), &names) == 1);
        CHECK(sh_package_movie_names_for_consumer(native, strlen(native), &again) == 1 && !strcmp(names.movie, again.movie));
    }
}
static void test_movie_construction(void)
{
    char error[256];
    sh_package_consumer_target targets[] = {
        {"movie/font/tt supermolot", "font", "smp/font1"},
        {"movie/material/guisolid", "material", "smp/mat1"},
        {"movie/material/_white", "material", "_white"}};
    sh_package_consumer owner = {"file", "generated/swf/hud/panel.bswf", targets, 3};
    font_catalog = sh_package_consumers_create(&owner, 1, error, sizeof(error)); CHECK(font_catalog);
    movie_records_prepare();
    memset(movie_font_manager, 0, sizeof(movie_font_manager)); memset(movie_material_manager, 0, sizeof(movie_material_manager));
    put_pointer(movie_font_manager, 8, "font"); put_pointer(movie_font_manager, 0x18, movie_material_manager);
    put_pointer(movie_material_manager, 8, "material"); movie_head = movie_font_manager; g_resource_head = &movie_head;
    for (size_t i = 0; i < 3; i++) {
        put_pointer(movie_fonts[i], 0x10, movie_font_manager); put_pointer(movie_materials[i], 0x10, movie_material_manager);
    }
    g_ready = 1; g_materialize = movie_materialize; g_optional_material = movie_optional;
    g_swf_construct = movie_construct; g_swf_destroy = movie_destroy; g_load = movie_nested_load;
    CHECK(!sh_resource_graph_native_consumer_ready("file"));
    g_movie_ready = 1;
    CHECK(sh_resource_graph_native_consumer_ready("file"));
    font_acquires = font_releases = font_holds = 0;
    movie_expect_frame = 1; movie_nested = 1;
    CHECK(rg_swf_construct_hook(movie_swf, "hud/panel", NULL, 3) == movie_swf);
    CHECK(movie_constructs == 1 && movie_prepares == 2 && font_holds == 1 && !font_releases && !g_movie_frame);
    rg_swf_destroy_hook(movie_swf);
    CHECK(!font_holds && font_releases == 1);
    movie_nested = 0;
    /* Uncompiled or ambiguous movies keep native construction without a plan. */
    movie_expect_frame = 0;
    CHECK(rg_swf_construct_hook(movie_swf, "swf/other.swf", NULL, 3) == movie_swf && !font_holds);
    CHECK(rg_swf_construct_hook(movie_swf, "swf/a.bimage/b.swf", NULL, 3) == movie_swf && !font_holds);
    CHECK(rg_swf_construct_hook(movie_swf, NULL, NULL, 3) == movie_swf && movie_constructs == 4);
    /* Contradictory metadata is rejected before publication, not substituted
     * by native requests during construction. No adapter entry allocates. */
    targets[0].family = "image"; CHECK(!sh_package_movie_plan_valid(&owner)); targets[0].family = "font";
    /* Native exceptions unwind the held plan and restore the outer frame. */
    movie_expect_frame = 1; movie_throw = 1;
    {
        int caught = 0, releases = font_releases;
        rg_movie_frame outer = {0}; g_movie_frame = &outer;
        __try { rg_swf_construct_hook(movie_swf, "swf/hud/panel.swf", NULL, 3); }
        __except (GetExceptionCode() == 0xe0123457 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
        CHECK(caught && g_movie_frame == &outer && !font_holds && font_releases == releases + 1);
        g_movie_frame = NULL;
    }
    movie_throw = 0;
    /* Before the movie route is installed, construction is a call-through. */
    g_movie_ready = 0; movie_expect_frame = 0;
    int acquires = font_acquires;
    CHECK(rg_swf_construct_hook(movie_swf, "swf/hud/panel.swf", NULL, 3) == movie_swf && font_acquires == acquires);
    sh_package_consumers_free(font_catalog); font_catalog = NULL;
    g_swf_construct = NULL; g_load = NULL;
    sh_resource_graph_test_reset();
}
static unsigned char late_movies[3][0x40], late_callbacks[3][0xc8], late_sound_manager[16], late_material_manager[16];
static char late_filenames[3][256];
static int late_ctor_throw, late_ctor_dtor, late_ctor_reuse, late_dtor_reenter, late_dtor_throw, late_destroys, late_index_calls;
static unsigned char late_dictionary, late_other_dictionary;
typedef struct late_parameters { const char *sound, *key; int mode, disabled; } late_parameters;
static void *late_construct(void *swf, const char *name, void *parent, unsigned char flags)
{
    CHECK(name && !parent && flags == 3);
    size_t index = swf == late_movies[0] ? 0 : 1;
    native_swf_name(name, late_filenames[index]);
    put_pointer(swf, 0, late_filenames[index]);
    CHECK(g_movie_frame && g_request_hold && !g_movie_string_hold);
    if (late_ctor_reuse) {
        int reuse = late_ctor_reuse;
        late_ctor_reuse = 0;
        rg_movie_lifetime_frame *old = g_movie_constructors;
        fake_movie_record *record = old->hold->token;
        int ended = movie_ends;
        CHECK(old->instance == old && !old->ended && record->instances == 1 && record->frames == 1);
        rg_swf_destroy_hook(swf);
        CHECK(old->ended && movie_ends == ended + 1 && !record->instances && record->frames == 1);
        CHECK(rg_swf_construct_hook(swf, name, parent, flags) == swf);
        CHECK(g_movie_constructors == old && g_movie_lifetimes == old && old->ended &&
            record->instances == 1 && record->frames == 1 && movie_ends == ended + 1);
        rg_swf_destroy_hook(swf);
        CHECK(old->ended && movie_ends == ended + 2 && !record->instances && record->frames == 1);
        if (reuse == 2) RaiseException(0xe0123460, 0, 0, NULL);
    }
    if (late_ctor_dtor) rg_swf_destroy_hook(swf);
    if (late_ctor_throw) RaiseException(0xe0123460, 0, 0, NULL);
    return swf;
}
static void late_destroy(void *swf)
{
    (void)swf; late_destroys++;
    if (late_dtor_reenter) {
        late_dtor_reenter = 0;
        rg_movie_lifetime_frame *entered = g_movie_destructors;
        int ended = movie_ends;
        rg_swf_destroy_hook(swf);
        CHECK(g_movie_destructors == entered && g_movie_lifetimes == entered &&
            entered->instance->ended && movie_ends == ended + 1);
    }
    if (late_dtor_throw) RaiseException(0xe0123461, 0, 0, NULL);
}
static int late_index(void *dictionary, const char *key)
{
    late_index_calls++;
    if (dictionary == &late_dictionary && !strcmp(key, "#str_private_a")) return 1;
    if (dictionary == &late_dictionary && !strcmp(key, "#str_private_b")) return 2;
    if (!strcmp(key, "#str_key")) return 3;
    return -1;
}
static void *late_call(void *callback, void *out, void *object, void *parameters)
{
    late_parameters *p = parameters; CHECK(object == (void *)91 && out);
    sh_package_consumer_hold *entered = g_request_hold;
    if (entered) {
        int a = !strcmp(entered->binding->name, "generated/swf/latea.bswf");
        CHECK(!strcmp(rg_request_name(late_sound_manager, p->sound), a ? "private/sound_a" : "private/sound_b"));
        CHECK(!strcmp(rg_request_name(late_material_manager, "computed_fx"), a ? "private/fx_a" : "private/fx_b"));
    } else CHECK(!strcmp(rg_request_name(late_sound_manager, p->sound), p->sound));
    if (p->mode == 1 || p->mode == 2) {
        late_parameters nested = {p->sound, p->key, 0, 0}; unsigned char result[16] = {0};
        CHECK(rg_swf_local_string_hook(late_callbacks[p->mode], result, object, &nested) == result);
        CHECK(g_request_hold == entered);
        CHECK(*(const char **)result && !strcmp(*(const char **)result, p->mode == 1 ? "text_b" : "stock_text"));
    }
    if (p->mode == 3) {
        void *swf = *(void **)((unsigned char *)callback + 0xc0);
        void *old_token = movie_records[0].binding && !strcmp(movie_records[0].binding->name, "generated/swf/latea.bswf") ? movie_records : movie_records + 1;
        rg_swf_destroy_hook(swf);
        CHECK(((fake_movie_record *)old_token)->instances == 0 && ((fake_movie_record *)old_token)->frames == 1);
        CHECK(rg_swf_construct_hook(swf, "lateb", NULL, 3) == swf);
        CHECK(g_request_hold == entered && font_holds == 2); /* A frame and reused B instance. */
    }
    if (p->mode == 5) RaiseException(0xe0123462, 0, 0, NULL);
    const char *text = p->key;
    if (!p->disabled) {
        int index = rg_dictionary_index_hook(p->mode == 6 ? &late_other_dictionary : &late_dictionary, p->key);
        if (index == 1) text = "text_a";
        else if (index == 2) text = "text_b";
        else if (index == 3) text = "stock_text";
    }
    *(const char **)out = text; return out;
}
static void test_movie_late_callbacks(void)
{
    char error[256], computed[64]; strcpy_s(computed, sizeof(computed), "computed_"); strcat_s(computed, sizeof(computed), "beep");
    sh_package_consumer_target a[] = {{"request/sound/computed_beep", "sound", "private/sound_a"},
        {"request/material/computed_fx", "material", "private/fx_a"},
        {"movie/string/#str_key", "string", "#str_private_a"}, {"movie/string/#str_missing", "string", "#str_private_missing"}};
    sh_package_consumer_target b[] = {{"request/sound/computed_beep", "sound", "private/sound_b"},
        {"request/material/computed_fx", "material", "private/fx_b"},
        {"movie/string/#str_key", "string", "#str_private_b"}};
    sh_package_consumer owners[] = {{"file", "generated/swf/latea.bswf", a, 4}, {"file", "generated/swf/lateb.bswf", b, 3}};
    font_catalog = sh_package_consumers_create(owners, 2, error, sizeof(error)); CHECK(font_catalog); movie_records_prepare();
    put_pointer(late_sound_manager, 8, "sound"); put_pointer(late_material_manager, 8, "material");
    for (size_t i = 0; i < 3; i++) put_pointer(late_callbacks[i], 0xc0, late_movies[i]);
    native_swf_name("stock", late_filenames[2]); put_pointer(late_movies[2], 0, late_filenames[2]);
    g_ready = g_movie_ready = 1; g_swf_construct = late_construct; g_swf_destroy = late_destroy;
    g_swf_precache_sound = g_swf_precache_material = g_swf_play_sound = g_swf_local_string = late_call;
    g_dictionary_index = late_index; g_movie_dictionary = &late_dictionary;
    font_holds = font_releases = font_acquires = 0;
    CHECK(rg_swf_construct_hook(late_movies[0], "latea", NULL, 3) == late_movies[0]);
    CHECK(rg_swf_construct_hook(late_movies[1], "lateb", NULL, 3) == late_movies[1] && font_holds == 2);
    late_parameters p = {computed, "#str_key", 0, 0}; unsigned char out[16] = {0};
    CHECK(rg_swf_precache_sound_hook(late_callbacks[0], out, (void *)91, &p) == out && !strcmp(*(const char **)out, "stock_text"));
    CHECK(rg_swf_precache_material_hook(late_callbacks[0], out, (void *)91, &p) == out);
    CHECK(rg_swf_play_sound_hook(late_callbacks[0], out, (void *)91, &p) == out);
    CHECK(rg_swf_local_string_hook(late_callbacks[0], out, (void *)91, &p) == out && !strcmp(*(const char **)out, "text_a"));
    for (p.mode = 1; p.mode <= 2; p.mode++) {
        CHECK(rg_swf_local_string_hook(late_callbacks[0], out, (void *)91, &p) == out && !strcmp(*(const char **)out, "text_a"));
        CHECK(!g_request_hold && !g_movie_frame && !g_movie_string_hold);
    }
    p.mode = 0; p.key = "#str_missing";
    CHECK(rg_swf_local_string_hook(late_callbacks[0], out, (void *)91, &p) == out && *(const char **)out == p.key);
    p.disabled = 1; int indexed = late_index_calls;
    CHECK(rg_swf_local_string_hook(late_callbacks[0], out, (void *)91, &p) == out && *(const char **)out == p.key && late_index_calls == indexed);
    p.disabled = 0; p.key = "#str_key"; p.mode = 6;
    CHECK(rg_swf_local_string_hook(late_callbacks[0], out, (void *)91, &p) == out && !strcmp(*(const char **)out, "stock_text"));
    p.mode = 5; int caught = 0;
    __try { rg_swf_local_string_hook(late_callbacks[0], out, (void *)91, &p); }
    __except (GetExceptionCode() == 0xe0123462 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && font_holds == 2 && !g_request_hold && !g_movie_frame && !g_movie_string_hold);
    rg_swf_destroy_hook(late_movies[1]); CHECK(font_holds == 1);
    p.mode = 3;
    CHECK(rg_swf_local_string_hook(late_callbacks[0], out, (void *)91, &p) == out && !strcmp(*(const char **)out, "text_a"));
    CHECK(font_holds == 1); /* A frame retired; same pointer now owns B. */
    rg_swf_destroy_hook(late_movies[0]); CHECK(!font_holds);
    late_ctor_throw = late_ctor_dtor = 1; caught = 0; int ended = movie_ends;
    __try { rg_swf_construct_hook(late_movies[0], "latea", NULL, 3); }
    __except (GetExceptionCode() == 0xe0123460 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && movie_ends == ended + 1 && !font_holds && !g_movie_constructors && !g_movie_destructors);
    late_ctor_throw = late_ctor_dtor = 0;
    CHECK(rg_swf_construct_hook(late_movies[0], "latea", NULL, 3) == late_movies[0]);
    late_dtor_throw = 1; caught = 0; ended = movie_ends;
    __try { rg_swf_destroy_hook(late_movies[0]); }
    __except (GetExceptionCode() == 0xe0123461 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && movie_ends == ended && font_holds == 1 && !g_movie_destructors);
    late_dtor_throw = 0; rg_swf_destroy_hook(late_movies[0]); CHECK(!font_holds);
    /* Same pointer AND same pooled filename token: the ended outer ctor stays
     * entered while a new incarnation is constructed and destroyed. */
    ended = movie_ends; int released = movie_frame_releases;
    late_ctor_reuse = 1;
    CHECK(rg_swf_construct_hook(late_movies[0], "latea", NULL, 3) == late_movies[0]);
    CHECK(!late_ctor_reuse && movie_ends == ended + 2 && movie_frame_releases == released + 4 &&
        !font_holds && !g_movie_constructors && !g_movie_destructors && !g_movie_lifetimes);
    ended = movie_ends; released = movie_frame_releases; caught = 0;
    late_ctor_reuse = 2;
    __try { rg_swf_construct_hook(late_movies[0], "latea", NULL, 3); }
    __except (GetExceptionCode() == 0xe0123460 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && !late_ctor_reuse && movie_ends == ended + 2 && movie_frame_releases == released + 4 &&
        !font_holds && !g_movie_constructors && !g_movie_destructors && !g_movie_lifetimes);
    /* Reentered destruction still shares the current incarnation's marker. */
    CHECK(rg_swf_construct_hook(late_movies[0], "latea", NULL, 3) == late_movies[0]);
    ended = movie_ends; released = movie_frame_releases; late_dtor_reenter = 1;
    rg_swf_destroy_hook(late_movies[0]);
    CHECK(!late_dtor_reenter && movie_ends == ended + 1 && movie_frame_releases == released + 2 &&
        !font_holds && !g_movie_constructors && !g_movie_destructors && !g_movie_lifetimes);
    CHECK(sh_resource_graph_native_signature("SwfConstruct") && sh_resource_graph_native_signature("LocalStringDictionaryIndex"));
    g_movie_ready = 0; sh_package_consumers_free(font_catalog); font_catalog = NULL;
}
/* Private font identity (R-309): a routed font request records the native
 * hash of the request for its private form; materialize and the native font
 * read apply it, so both native typings agree with the stock font's. */
static unsigned char id_font_manager[16], id_material_manager[16], id_fonts[3][0x68], id_movie[0x40];
static char id_movie_name[256];
static const char *id_request;
static int id_reads;
static void *id_materialize(void *manager, const char *name, unsigned char create_default, unsigned char flags)
{
    (void)create_default; (void)flags;
    CHECK(manager == id_font_manager || manager == id_material_manager);
    for (int i = 0; i < 3; i++)
        if (*(const char **)(id_fonts[i] + 8) && !strcmp(*(const char **)(id_fonts[i] + 8), name)) return id_fonts[i];
    return NULL;
}
static unsigned int id_font_read(void *resource, void *error_text)
{
    /* The native read's last step (VK 0x141819c30 / GL 0x14180c5c0). */
    (void)error_text; id_reads++;
    *(int *)((unsigned char *)resource + 0x4c) = sh_swf_font_name_hash(*(const char **)((unsigned char *)resource + 8));
    return 0;
}
static void *id_construct(void *swf, const char *name, void *parent, unsigned char flags)
{
    CHECK(swf == id_movie && !parent && flags == 3);
    native_swf_name(name, id_movie_name); put_pointer(id_movie, 0, id_movie_name);
    /* Dictionary fonts resolve inside construction through the movie's routes. */
    CHECK(rg_materialize_hook(id_font_manager, id_request, 1, 0) == id_fonts[0]);
    return swf;
}
static void id_destroy(void *swf) { CHECK(swf == id_movie); }
static void test_movie_font_identity(void)
{
    char error[256];
    const int stock = 748353694, supermolot = -959555212; /* computed independently */
    sh_package_consumer_target a[] = {{"movie/font/square721 ex tl", "font", "smp/fonta"}};
    sh_package_consumer_target b[] = {{"movie/font/square721_ex_tl", "font", "smp/fonta"}};
    sh_package_consumer owners[] = {{"file", "generated/swf/ida.bswf", a, 1}, {"file", "generated/swf/idb.bswf", b, 1}};
    put_pointer(id_font_manager, 8, "font"); put_pointer(id_material_manager, 8, "material");
    for (int i = 0; i < 3; i++) put_pointer(id_fonts[i], 0x10, id_font_manager);
    put_pointer(id_fonts[0], 8, "smp/fonta"); put_pointer(id_fonts[1], 8, "tt supermolot");
    put_pointer(id_fonts[2], 8, "smp/fonta"); put_pointer(id_fonts[2], 0x10, id_material_manager);
    for (int i = 0; i < 3; i++) id_font_read(id_fonts[i], NULL);
    const int own = sh_swf_font_name_hash("smp/fonta");
    CHECK(*(int *)(id_fonts[0] + 0x4c) == own && *(int *)(id_fonts[1] + 0x4c) == supermolot);
    g_ready = g_movie_ready = 1; g_materialize = id_materialize; g_font_read = id_font_read;
    g_swf_construct = id_construct; g_swf_destroy = id_destroy;
    font_holds = font_releases = font_acquires = 0;
    /* One live plan routes the form: an already-loaded font takes the request's identity. */
    font_catalog = sh_package_consumers_create(owners, 1, error, sizeof(error)); CHECK(font_catalog); movie_records_prepare();
    id_request = "Square721 Ex TL";
    CHECK(rg_swf_construct_hook(id_movie, "ida", NULL, 3) == id_movie && *(int *)(id_fonts[0] + 0x4c) == stock);
    /* A later native read rewrites the name hash; the read hook restores the identity. */
    CHECK(!rg_font_read_hook(id_fonts[0], NULL) && *(int *)(id_fonts[0] + 0x4c) == stock);
    /* Unrouted fonts, and another family's resource of the same name, keep native values. */
    CHECK(!rg_font_read_hook(id_fonts[1], NULL) && *(int *)(id_fonts[1] + 0x4c) == supermolot);
    rg_font_identity_apply(id_fonts[2]); CHECK(*(int *)(id_fonts[2] + 0x4c) == own);
    rg_swf_destroy_hook(id_movie); CHECK(!font_holds);
    sh_package_consumers_free(font_catalog);
    /* Live plans that disagree (excluded for compiled forms by font semantics)
     * assign nothing: a native read keeps its own value and is reported. */
    font_catalog = sh_package_consumers_create(owners, 2, error, sizeof(error)); CHECK(font_catalog); movie_records_prepare();
    int conflicts = g_font_identity_conflicts, hash = 0;
    CHECK(sh_package_runtime_font_identity("smp/fonta", &hash) == -1);
    CHECK(!rg_font_read_hook(id_fonts[0], NULL) && *(int *)(id_fonts[0] + 0x4c) == own && g_font_identity_conflicts == conflicts + 1);
    sh_package_consumers_free(font_catalog);
    /* After the last routing plan retires, the form has no identity to carry. */
    font_catalog = sh_package_consumers_create(owners, 1, error, sizeof(error)); CHECK(font_catalog); movie_records_prepare();
    CHECK(sh_package_runtime_font_identity("smp/fonta", &hash) == 1 && hash == stock);
    sh_package_consumers_free(font_catalog); font_catalog = NULL; movie_records_prepare();
    CHECK(!sh_package_runtime_font_identity("smp/fonta", &hash));
    CHECK(!rg_font_read_hook(id_fonts[0], NULL) && *(int *)(id_fonts[0] + 0x4c) == own);
    g_movie_ready = 0; g_swf_construct = NULL; g_swf_destroy = NULL; g_materialize = NULL; g_font_read = NULL;
}
static unsigned char fx_movie[0x40], fx_callback[0xc8], fx_manager_fake[16], fx_materials[6][0x30];
static char fx_movie_name[256];
static void *fx_entries[8];
static int fx_count, fx_materials_used, fx_lookups, fx_indices[2];
static const char *fx_request;
/* The native registration's shape: dedup by entry name, else append DeclFind's
 * result, which this module's DeclFind hook may route. */
static int fx_register(void *swf, const char *name)
{
    CHECK(name && (swf == fx_movie || swf == late_movies[2]));
    for (int i = 0; i < fx_count; i++) if (!_stricmp(*(const char **)((unsigned char *)fx_entries[i] + 8), name)) return i;
    void *material = rg_optional_material_hook(fx_manager_fake, name, 0);
    if (!material || fx_count == 8) return -1;
    fx_entries[fx_count] = material; return fx_count++;
}
static void *fx_find(void *manager, const char *name, unsigned char create_default)
{
    CHECK(manager == fx_manager_fake && !create_default); fx_lookups++;
    for (int i = 0; i < fx_materials_used; i++) if (!strcmp(*(const char **)(fx_materials[i] + 8), name)) return fx_materials[i];
    CHECK(fx_materials_used < 6); if (fx_materials_used == 6) return NULL;
    put_pointer(fx_materials[fx_materials_used], 8, name); put_pointer(fx_materials[fx_materials_used], 0x10, fx_manager_fake);
    return fx_materials[fx_materials_used++];
}
static void *fx_precache(void *callback, void *out, void *object, void *parameters)
{
    (void)object; (void)parameters;
    void *swf = *(void **)((unsigned char *)callback + 0xc0);
    fx_indices[0] = rg_fontfx_register_hook(swf, fx_request);
    fx_indices[1] = rg_fontfx_register_hook(swf, fx_request);
    return out;
}
static void *fx_construct(void *swf, const char *name, void *parent, unsigned char flags)
{
    CHECK(swf == fx_movie && name && !parent && flags == 3);
    native_swf_name(name, fx_movie_name); put_pointer(fx_movie, 0, fx_movie_name);
    return swf;
}
static void fx_destroy(void *swf) { CHECK(swf == fx_movie); }
static void test_movie_fontfx_registration(void)
{
    char error[256]; unsigned char out[16];
    sh_package_consumer_target a[] = {{"request/material/fontfx/korataki_rg/glow/thin/yellow", "material", "smp/fxa"}};
    sh_package_consumer owners[] = {{"file", "generated/swf/fx.bswf", a, 1}};
    font_catalog = sh_package_consumers_create(owners, 1, error, sizeof(error)); CHECK(font_catalog); movie_records_prepare();
    put_pointer(fx_manager_fake, 8, "material"); put_pointer(fx_callback, 0xc0, fx_movie);
    g_ready = g_movie_ready = 1; g_swf_construct = fx_construct; g_swf_destroy = fx_destroy;
    g_swf_precache_material = fx_precache; g_optional_material = fx_find;
    g_fontfx_register = fx_register; g_fontfx_manager = fx_manager_fake;
    font_holds = font_releases = font_acquires = 0;
    CHECK(rg_swf_construct_hook(fx_movie, "fx", NULL, 3) == fx_movie && font_holds == 1);
    /* A routed late request registers the private material once and keeps one index. */
    fx_request = "fontFX/Korataki_Rg/glow/thin/yellow"; fx_count = fx_materials_used = fx_lookups = 0;
    CHECK(rg_swf_precache_material_hook(fx_callback, out, (void *)91, NULL) == out);
    CHECK(fx_indices[0] == 0 && fx_indices[1] == 0 && fx_count == 1 && fx_lookups == 1);
    CHECK(!strcmp(*(const char **)((unsigned char *)fx_entries[0] + 8), "smp/fxa"));
    /* An unrouted request in the same movie keeps its native name and dedup. */
    fx_request = "fontfx/supermolot/outline/normal/black";
    CHECK(rg_swf_precache_material_hook(fx_callback, out, (void *)91, NULL) == out);
    CHECK(fx_indices[0] == 1 && fx_indices[1] == 1 && fx_count == 2 &&
        !strcmp(*(const char **)((unsigned char *)fx_entries[1] + 8), "fontfx/supermolot/outline/normal/black"));
    /* Outside any movie context the registration is a call-through. */
    fx_request = "fontFX/Korataki_Rg/glow/thin/yellow";
    CHECK(rg_fontfx_register_hook(fx_movie, fx_request) == 2 && rg_fontfx_register_hook(fx_movie, fx_request) == 2 &&
        !strcmp(*(const char **)((unsigned char *)fx_entries[2] + 8), fx_request) && fx_count == 3);
    rg_swf_destroy_hook(fx_movie); CHECK(!font_holds);
    g_movie_ready = 0; g_swf_construct = NULL; g_swf_destroy = NULL; g_swf_precache_material = NULL;
    g_optional_material = NULL; g_fontfx_register = NULL; g_fontfx_manager = NULL;
    sh_package_consumers_free(font_catalog); font_catalog = NULL;
}
static void test_movie_hook_rollback(void)
{
    static unsigned char targets[RG_MOVIE_HOOKS][0x80], decl_find[16], fx_manager[16];
    sig_result results[RG_MOVIE_HOOKS + 1] = {0};
    for (size_t i = 0; i < RG_MOVIE_HOOKS; i++) { results[i].name = g_movie_required[i]; results[i].status = SIG_OK; results[i].addr = (uintptr_t)targets[i]; }
    /* The registration's DeclFind is a main-group target. */
    results[RG_MOVIE_HOOKS].name = "DeclFind"; results[RG_MOVIE_HOOKS].status = SIG_OK; results[RG_MOVIE_HOOKS].addr = (uintptr_t)decl_find;
    const size_t all = RG_MOVIE_HOOKS + 1;
    targets[5][0x59] = 0x48; targets[5][0x5a] = 0x8d; targets[5][0x5b] = 0x0d; targets[5][0x60] = 0xe8;
    int32_t displacement = (int32_t)((intptr_t)&late_dictionary - (intptr_t)(targets[5] + 0x60));
    memcpy(targets[5] + 0x5c, &displacement, sizeof(displacement));
    CHECK(rg_movie_dictionary_for(targets[5]) == &late_dictionary);
    targets[5][0x59] = 0; CHECK(!rg_movie_dictionary_for(targets[5])); targets[5][0x59] = 0x48;
    /* The registration target: its material-manager LEA and its call to the hooked DeclFind. */
    unsigned char *fx = targets[7];
    memcpy(fx + 0x61, "\x48\x8d\x0d", 3); fx[0x6b] = 0xe8;
    displacement = (int32_t)((intptr_t)fx_manager - (intptr_t)(fx + 0x68)); memcpy(fx + 0x64, &displacement, 4);
    displacement = (int32_t)((intptr_t)decl_find - (intptr_t)(fx + 0x70)); memcpy(fx + 0x6c, &displacement, 4);
    CHECK(sh_swf_fontfx_manager_decode(fx, decl_find) == fx_manager && !sh_swf_fontfx_manager_decode(fx, fx_manager));
    movie_install_fake = 1; g_ready = 1;
    g_swf_construct = NULL; g_swf_destroy = NULL;
    g_swf_precache_sound = g_swf_precache_material = g_swf_play_sound = g_swf_local_string = NULL;
    g_dictionary_index = NULL; g_fontfx_register = NULL;
    for (int pass = 0; pass < 3; pass++) {
        g_movie_ready = g_movie_attempted = 0;
        movie_hook_prepares = movie_hook_commits = movie_hook_removes = 0;
        movie_hook_prepare_fault = pass == 0 ? 4 : 0;
        movie_hook_commit_fault = pass ? 4 : 0; movie_hook_remove_fault = pass == 2 ? 1 : 0;
        rg_movie_install(results, all);
        CHECK(!g_movie_ready && g_movie_attempted);
        CHECK(movie_hook_removes == (pass ? RG_MOVIE_HOOKS : 3));
        CHECK(!g_swf_construct && !g_swf_destroy && !g_swf_precache_sound && !g_swf_precache_material && !g_swf_play_sound &&
            !g_swf_local_string && !g_dictionary_index);
        CHECK((g_fontfx_register != NULL) == (pass == 2));
        g_fontfx_register = NULL; /* Fake removal recovery; no executable patch exists. */
    }
    movie_hook_prepare_fault = movie_hook_commit_fault = movie_hook_remove_fault = 0;
    g_movie_ready = g_movie_attempted = 0; movie_hook_prepares = movie_hook_commits = movie_hook_removes = 0;
    results[4].status = SIG_NOT_FOUND; rg_movie_install(results, all); CHECK(!movie_hook_prepares && !g_movie_ready);
    results[4].status = SIG_OK;
    /* An undecodable registration target, or one that does not call the
     * hooked DeclFind, refuses the whole group before any patch. */
    fx[0x61] = 0; rg_movie_install(results, all);
    CHECK(!movie_hook_prepares && !g_movie_ready && !g_movie_attempted); fx[0x61] = 0x48;
    results[RG_MOVIE_HOOKS].addr = (uintptr_t)fx_manager; rg_movie_install(results, all);
    CHECK(!movie_hook_prepares && !g_movie_ready && !g_movie_attempted); results[RG_MOVIE_HOOKS].addr = (uintptr_t)decl_find;
    rg_movie_install(results, all);
    CHECK(g_movie_ready && movie_hook_commits == RG_MOVIE_HOOKS && !movie_hook_removes && g_fontfx_manager == fx_manager);
    g_movie_ready = 0; movie_install_fake = 0;
    g_swf_construct = NULL; g_swf_destroy = NULL; g_swf_precache_sound = g_swf_precache_material = g_swf_play_sound = g_swf_local_string = NULL;
    g_dictionary_index = NULL; g_movie_dictionary = NULL;
    g_fontfx_register = NULL; g_fontfx_manager = NULL;
}
static unsigned char request_materials[3][0x168], request_managers[3][16];
static const char *request_expected;
static int request_nested, request_throw;
static void *request_lookup(void *manager, const char *name)
{
    CHECK(manager == request_managers[1] || manager == request_managers[2]);
    CHECK(!strcmp(name, request_expected)); return NULL;
}
static void *request_materialize(void *manager, const char *name, unsigned char make_default, unsigned char flags)
{ CHECK(make_default == 1 && flags == 7); return request_lookup(manager, name); }
static unsigned int request_parse(void *resource, unsigned char read_source, void *error)
{
    CHECK(!read_source && !error);
    const char *bound = resource == request_materials[0] ? "smp/red" :
        resource == request_materials[1] ? "smp/green" : "textures/fx/beam";
    request_expected = bound;
    rg_lookup_hook(request_managers[1], "textures/fx/beam");
    rg_materialize_hook(request_managers[1], "textures/fx/beam", 1, 7);
    request_expected = "textures/fx/beam";
    rg_materialize_hook(request_managers[2], "textures/fx/beam", 1, 7); /* Other manager. */
    if (request_nested && resource == request_materials[0]) {
        CHECK(!rg_parse_hook(request_materials[1], 0, NULL));
        CHECK(!rg_parse_hook(request_materials[2], 0, NULL));
        request_expected = bound;
        rg_materialize_hook(request_managers[1], "textures/fx/beam", 1, 7);
    }
    if (request_throw) RaiseException(0xe0123456, 0, 0, NULL);
    return 0;
}
static unsigned int request_read(void *resource, void *error)
{ return request_parse(resource, 0, error); }
static void request_load(void *resource) { CHECK(!request_parse(resource, 0, NULL)); }
static void test_constructor_requests(void)
{
    char error[256]; int caught = 0;
    sh_package_consumer_target targets[] = {
        {"request/transsortatlas/textures/fx/beam", "transsortatlas", "smp/red"},
        {"request/transsortatlas/textures/fx/beam", "transsortatlas", "smp/green"}
    };
    sh_package_consumer owners[] = {{"material", "red", targets, 1}, {"material", "green", targets + 1, 1}};
    font_catalog = sh_package_consumers_create(owners, 2, error, sizeof(error)); CHECK(font_catalog);
    font_acquires = font_releases = font_holds = font_acquire_error = 0;
    put_pointer(request_managers[0], 8, "material");
    put_pointer(request_managers[1], 8, "transsortatlas");
    put_pointer(request_managers[2], 8, "image");
    for (size_t i = 0; i < 3; i++) {
        put_pointer(request_materials[i], 8, i == 0 ? "red" : i == 1 ? "green" : "vanilla");
        put_pointer(request_materials[i], 0x10, request_managers[0]);
    }
    g_ready = 1; g_parse = request_parse; g_read = request_read; g_load = request_load;
    g_lookup = request_lookup; g_materialize = request_materialize; request_nested = 1;
    CHECK(!rg_parse_hook(request_materials[0], 0, NULL) && !g_request_hold && !font_holds);
    CHECK(!rg_read_hook(request_materials[0], NULL) && !g_request_hold && !font_holds);
    rg_load_hook(request_materials[0]); CHECK(!g_request_hold && !font_holds);
    request_throw = 1; request_nested = 0;
    __try { rg_parse_hook(request_materials[0], 0, NULL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { caught = 1; }
    CHECK(caught && !g_request_hold && !font_holds && font_releases == 7);
    request_throw = 0;
    sh_package_consumers_free(font_catalog); font_catalog = NULL;
    g_parse = NULL; g_read = NULL; g_load = NULL; g_lookup = NULL; g_materialize = NULL;
    sh_resource_graph_test_reset();
}
/* R-321 requests at the hooked finds: a find or materialize that returns nothing records the name it asked the
 * manager for, under the manager's family, only inside a frame that records requests. A returned object is a
 * reference in every recording frame. */
static unsigned char lookup_managers[2][16], lookup_found[0x40];
static void *lookup_result;
static const char *lookup_seen;
static void *lookup_find(void *manager, const char *name)
{ (void)manager; lookup_seen = name; return lookup_result; }
static void *lookup_make(void *manager, const char *name, unsigned char make_default, unsigned char flags)
{ (void)make_default; (void)flags; return lookup_find(manager, name); }
static int lookup_consumes(const char *type, const char *name, const char *consumer_type, const char *consumer)
{
    sh_resource_graph_identity seed = {(char *)type, (char *)name};
    sh_resource_graph_impact impact = {0};
    int found = 0;
    if (sh_resource_graph_consumers(&seed, 1, &impact) == SH_RESOURCE_GRAPH_ERROR) return -1;
    for (size_t i = 0; i < impact.count; i++)
        found |= !strcmp(impact.items[i].type, consumer_type) && !strcmp(impact.items[i].name, consumer);
    sh_resource_graph_impact_free(&impact);
    return found;
}
static void test_lookup_requests(void)
{
    sh_resource_graph_frame frame, nested, paused;
    size_t nodes, complete, edges;
    sh_resource_graph_test_reset();
    put_pointer(lookup_managers[0], 8, "image"); put_pointer(lookup_managers[1], 8, "Material");
    put_pointer(lookup_found, 8, "env/default"); put_pointer(lookup_found, 0x10, lookup_managers[0]);
    g_lookup = lookup_find; g_materialize = lookup_make; lookup_result = NULL;
    CHECK(!rg_lookup_hook(lookup_managers[0], "env/outside") && !strcmp(lookup_seen, "env/outside"));
    sh_resource_graph_begin(&frame, "discreteanimation", "anim/live");
    CHECK(!rg_lookup_hook(lookup_managers[0], "env/missing-live"));
    CHECK(!rg_materialize_hook(lookup_managers[1], "mat/missing-live", 1, 0));
    lookup_result = lookup_found;
    CHECK(rg_lookup_hook(lookup_managers[0], "env/default") == lookup_found);
    lookup_result = NULL;
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin_requests(&frame, "discreteanimation", "anim/retired");
    CHECK(!rg_lookup_hook(lookup_managers[0], "env/later") && !strcmp(lookup_seen, "env/later"));
    CHECK(!rg_materialize_hook(lookup_managers[1], "mat/later", 1, 0) && !strcmp(lookup_seen, "mat/later"));
    lookup_result = lookup_found;
    CHECK(rg_materialize_hook(lookup_managers[0], "env/default", 1, 0) == lookup_found);
    lookup_result = NULL;
    sh_resource_graph_begin(&nested, "material", "nested");
    CHECK(!rg_lookup_hook(lookup_managers[0], "env/nested"));
    sh_resource_graph_end(&nested, 1);
    sh_resource_graph_pause(&paused);
    CHECK(!rg_materialize_hook(lookup_managers[0], "env/paused", 1, 0));
    sh_resource_graph_end(&paused, 0);
    sh_resource_graph_end(&frame, 1);
    CHECK(lookup_consumes("image", "env/later", "discreteanimation", "anim/retired") == 1);
    CHECK(lookup_consumes("material", "mat/later", "discreteanimation", "anim/retired") == 1);
    CHECK(lookup_consumes("image", "env/default", "discreteanimation", "anim/retired") == 1);
    CHECK(lookup_consumes("image", "env/default", "discreteanimation", "anim/live") == 1);
    CHECK(lookup_consumes("image", "env/missing-live", "discreteanimation", "anim/live") == 0);
    CHECK(lookup_consumes("material", "mat/missing-live", "discreteanimation", "anim/live") == 0);
    CHECK(lookup_consumes("image", "env/nested", "material", "nested") == 0);
    CHECK(lookup_consumes("image", "env/nested", "discreteanimation", "anim/retired") == 0);
    CHECK(lookup_consumes("image", "env/paused", "discreteanimation", "anim/retired") == 0);
    sh_resource_graph_counts(&nodes, &complete, &edges);
    /* anim/live, env/default, anim/retired, env/later, material:mat/later and material:nested. */
    CHECK(nodes == 6 && complete == 3 && edges == 5);
    g_lookup = NULL; g_materialize = NULL;
    sh_resource_graph_test_reset();
}
/* R-352: retirement default observation by the default routine an object runs. A synthetic image holds each
 * pinned routine's bytes at its pinned address, for both renderers, with the static-handle loads and slots the
 * census names. Inside a request frame, with the hooks live and a pinned renderer, a routine's known
 * static-handle reads are recorded (a declaration default's through its class's parser) and the frame may
 * complete only for a routine whose whole read inventory is established. A moved or patched routine, another
 * renderer's address, an unverifiable load or an unidentifiable slot records nothing and completes nothing. */
static unsigned char *default_image;
enum { DEFAULT_IMAGE_SPAN = 0x7000000 }; /* past the highest census slot, Vulkan 0x5558568 */
static void *default_vtable[0x100 / 8];
static unsigned char default_object[0x60], default_manager[0x10];
/* What the census's static handles hold in the game. */
static unsigned char tracemodel[0x20], uber_particles[0x20], particle_light_atlas[0x20], virtual_gui_blend[0x20];
static unsigned char default_particle[0x20], default_gpu[0x20];
static unsigned char material_manager[0x10], program_manager[0x10];
static unsigned char skeleton_animation[0x20], skeleton_animation_manager[0x10];
static const char *skeleton_animation_request;
static void *skeleton_materialize(void *manager, const char *name, unsigned char create_default, unsigned char flags)
{
    CHECK(manager == skeleton_animation_manager && create_default == 1 && flags == 0);
    CHECK(!strcmp(name, skeleton_animation_request)); return skeleton_animation;
}
static void default_image_put(uint32_t rva, const void *bytes, size_t length)
{
    CHECK(VirtualAlloc(default_image + rva, length, MEM_COMMIT, PAGE_READWRITE) != NULL);
    memcpy(default_image + rva, bytes, length);
}
static void default_route_put(const rg_default_route *route, int clear)
{
    if (clear) { unsigned char cc[22]; memset(cc, 0xcc, sizeof(cc)); default_image_put(route->rva, cc, route->length); return; }
    default_image_put(route->rva, route->bytes, route->length);
    if(route->rva==0xd944e0 || route->rva==0xd9d630) {
        int vk=route->rva==0xd9d630;
        uint32_t sampler[]={vk?0x47d9ea0:0x30dc730,vk?0x47d9eb0:0x30dc740,vk?0x2252560:0x2269740,
            vk?0xdff970:0xdf3d50,vk?0xdff2b0:0xdf3690};
        uint32_t allocator[]={vk?0x6f8e2c8:0x5894b38,vk?0x55b7190:0x3eb97b0,vk?0x278b740:0x27a1950,
            vk?0x1a03ed0:0x19f6130,vk?0x1a03fc0:0x19f6220,vk?0x1ac57a0:0x1ab7340,
            vk?0x1ac5770:0x1ab7310,vk?0x1a04ad0:0x19f6d30,vk?0x1a04b80:0x19f6de0};
        void *value=default_image+sampler[1]; default_image_put(sampler[0],&value,8);
        value=default_image+sampler[2]; default_image_put(sampler[1],&value,8);
        value=default_image+sampler[3]; default_image_put(sampler[2]+0x28,&value,8);
        value=default_image+sampler[4]; default_image_put(sampler[2]+0x30,&value,8);
        value=default_image+allocator[1]; default_image_put(allocator[0],&value,8);
        value=default_image+allocator[2]; default_image_put(allocator[1],&value,8);
        size_t slots[]={8,0x10,0x48,0x50,0x90,0x98};
        for(size_t i=0;i<6;i++) { value=default_image+allocator[3+i]; default_image_put(allocator[2]+(uint32_t)slots[i],&value,8); }
    }
}
static void *handle_object_for(const rg_default_route *route, unsigned i)
{
    if (route->kind == RG_DEFAULT_ROUTINE) return tracemodel;           /* the collision model default */
    if (route->reads == 3) return i == 2 ? virtual_gui_blend : i ? particle_light_atlas : uber_particles;
    return i ? default_gpu : default_particle;
}
static void default_reads_put(const rg_default_route *route)
{
    for (unsigned i = 0; i < route->reads; i++) {
        unsigned char load[7] = {0x48, 0x8b, 0x05};
        int32_t displacement = (int32_t)(route->read[i].slot - (route->read[i].site + 7));
        void *object = handle_object_for(route, i);
        memcpy(load + 3, &displacement, 4);
        default_image_put(route->read[i].site, load, sizeof(load));
        default_image_put(route->read[i].slot, &object, sizeof(object));
    }
}
static int default_observed_in_frame(const char *name)
{
    sh_resource_graph_frame frame;
    int observed;
    sh_resource_graph_begin_requests(&frame, "decl", name);
    observed = sh_resource_graph_native_default_observed(default_object);
    sh_resource_graph_end(&frame, observed);
    return observed;
}
static int default_names(const char *consumer, const char *type, const char *name)
{ return lookup_consumes(type, name, "decl", consumer) == 1; }
static int default_recorded(const char *consumer)
{
    return default_names(consumer, "material", "_tracemodel") + default_names(consumer, "renderProg", "uberParticles") +
        default_names(consumer, "renderProg", "particleLightAtlas") +
        default_names(consumer, "renderProg", "virtualGuiBlend") +
        default_names(consumer, "material", "textures/effects/particles/default") +
        default_names(consumer, "material", "textures/effects/particles/defaultgpu");
}
static int parser_delegates, parser_noop, parser_fails;
static unsigned int parser_capture(void *resource, unsigned char source, void *error)
{
    (void)source; (void)error; CHECK(resource == default_object);
    CHECK(!rg_lookup_hook(lookup_managers[0], "env/later"));
    return parser_fails;
}
static unsigned int parser_read(void *resource, void *error)
{
    if (parser_noop) return 0;
    if (parser_delegates) return rg_parse_hook(resource, 0, error);
    return parser_capture(resource, 0, error);
}
static void parser_load(void *resource) { (void)rg_read_hook(resource, NULL); }
static void parser_capture_cases(const rg_default_route *declaration, const rg_default_route *parser)
{
    rg_parse_fn saved_parse = g_parse; rg_read_fn saved_read = g_read;
    rg_load_fn saved_load = g_load; rg_lookup_fn saved_lookup = g_lookup;
    void *saved_result = lookup_result;
    g_parse = parser_capture; g_read = parser_read; g_load = parser_load; g_lookup = lookup_find;
    lookup_result = NULL; put_pointer(lookup_managers[0], 8, "image");
    default_vtable[0x50 / 8] = default_image + declaration->rva;
    default_vtable[0x98 / 8] = default_image + parser->rva;
    for (int route = 0; route < 4; route++) {
        sh_resource_graph_test_reset();
        put_pointer(default_object, 8, "ordinary parser");
        parser_delegates = route == 2; parser_noop = parser_fails = 0;
        if (route == 0) CHECK(!rg_parse_hook(default_object, 0, NULL));
        else if (route < 3) CHECK(!rg_read_hook(default_object, NULL));
        else rg_load_hook(default_object);
        CHECK(default_recorded("ordinary parser") == parser->reads);
        CHECK(lookup_consumes("image", "env/later", "decl", "ordinary parser"));
        CHECK(!sh_resource_graph_recording() && !g_request_hold);
        /* A native implicit-text Read is a no-op. It cannot replace the prior source graph or record
         * the parser's handles under an unrelated surrounding constructor. */
        parser_noop = 1; default_object[0x48] = 1;
        sh_resource_graph_frame outer; sh_resource_graph_begin_requests(&outer, "decl", "outer noop");
        CHECK(!rg_read_hook(default_object, NULL)); sh_resource_graph_end(&outer, 1);
        CHECK(default_recorded("ordinary parser") == parser->reads && !default_recorded("outer noop"));
        default_object[0x48] = 0; parser_noop = 0;
        /* Failed parsing keeps the observed dependency edges but does not claim successful parsing. */
        parser_fails = 1; CHECK(rg_parse_hook(default_object, 0, NULL) == 1);
        unsigned bits = 0;
        CHECK(!walk("decl", "ordinary parser", &bits));
        CHECK(default_recorded("ordinary parser") == parser->reads && !sh_resource_graph_recording());
    }
    parser_delegates = parser_noop = parser_fails = 0;
    g_parse = saved_parse; g_read = saved_read; g_load = saved_load; g_lookup = saved_lookup;
    lookup_result = saved_result; put_pointer(default_object, 8, "retired");
}
static void test_default_routes(void)
{
    LONG ready = g_ready;
    default_image = VirtualAlloc(NULL, DEFAULT_IMAGE_SPAN, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(default_image != NULL); if (!default_image) return;
    put_pointer(default_object, 0, default_vtable); put_pointer(default_object, 8, "retired");
    put_pointer(default_object, 0x10, default_manager); put_pointer(default_manager, 8, "decl");
    put_pointer(material_manager, 8, "material"); put_pointer(program_manager, 8, "renderProg");
    put_pointer(tracemodel, 8, "_tracemodel"); put_pointer(tracemodel, 0x10, material_manager);
    put_pointer(uber_particles, 8, "uberParticles"); put_pointer(uber_particles, 0x10, program_manager);
    put_pointer(particle_light_atlas, 8, "particleLightAtlas"); put_pointer(particle_light_atlas, 0x10, program_manager);
    put_pointer(virtual_gui_blend, 8, "virtualGuiBlend"); put_pointer(virtual_gui_blend, 0x10, program_manager);
    put_pointer(default_particle, 8, "textures/effects/particles/default"); put_pointer(default_particle, 0x10, material_manager);
    put_pointer(default_gpu, 8, "textures/effects/particles/defaultgpu"); put_pointer(default_gpu, 0x10, material_manager);
    /* The census, both renderers: sixteen defaults and two parsers at distinct addresses; one generic declaration
     * default; seven unconditional complete routines plus collision with both parm hooks; six static reads. */
    for (int renderer = 0; renderer < 2; renderer++) {
        int kinds[3] = {0}, complete = 0, reads = 0;
        for (size_t i = 0; i < RG_DEFAULT_ROUTES; i++) {
            const rg_default_route *route = &g_default_routes[renderer][i];
            kinds[route->kind]++; complete += route->complete != 0; reads += route->reads;
            CHECK(route->rva && route->length && route->length <= sizeof(route->bytes) && route->reads <= 3);
            CHECK(!route->complete || (route->kind == RG_DEFAULT_ROUTINE &&
                (!route->reads || route->complete == RG_DEFAULT_WITH_PARM)));
            for (size_t j = 0; j < i; j++) CHECK(g_default_routes[renderer][j].rva != route->rva);
        }
        CHECK(kinds[RG_DEFAULT_ROUTINE] == 15 && kinds[RG_DEFAULT_DECLARATION] == 1 && kinds[RG_DEFAULT_PARSER] == 2);
        CHECK(complete == 9 && reads == 6);
    }
    g_ready = 1; g_test_default_parm_ready = 1;
    for (int renderer = 0; renderer < 2; renderer++) {
        const rg_default_route *declaration = NULL, *parsers[2] = {NULL, NULL};
        sh_resource_graph_native_test_default_image(default_image, renderer);
        for (size_t i = 0; i < RG_DEFAULT_ROUTES; i++) {
            const rg_default_route *route = &g_default_routes[renderer][i];
            if (route->kind == RG_DEFAULT_DECLARATION) declaration = route;
            if (route->kind == RG_DEFAULT_PARSER) parsers[!parsers[0] ? 0 : 1] = route;
        }
        CHECK(declaration && parsers[0] && parsers[1]);
        if (!declaration || !parsers[0] || !parsers[1]) continue;
        /* Every default routine: complete exactly when established. Collision
         * additionally requires both actual parm hooks to be installed. */
        for (size_t i = 0; i < RG_DEFAULT_ROUTES; i++) {
            const rg_default_route *route = &g_default_routes[renderer][i];
            unsigned char *routine = default_image + route->rva;
            if (route->kind == RG_DEFAULT_PARSER) continue;
            sh_resource_graph_test_reset();
            default_route_put(route, 0); default_reads_put(route);
            default_vtable[0x50 / 8] = routine; default_vtable[0x98 / 8] = NULL;
            CHECK(default_observed_in_frame("retired") == (route->complete != 0));
            if (route->complete == RG_DEFAULT_WITH_PARM) {
                g_test_default_parm_ready = 0;
                CHECK(!default_observed_in_frame("missing parm hooks") &&
                    default_names("missing parm hooks", "material", "_tracemodel"));
                g_test_default_parm_ready = 1;
            }
            if(route->rva==(renderer?0xd9d630u:0xd944e0u)) {
                uint32_t sampler_table=renderer?0x2252560:0x2269740;
                uint32_t allocator_table=renderer?0x278b740:0x27a1950;
                void **slot=(void **)(default_image+sampler_table+0x28); void *saved=*slot; *slot=(void *)1;
                CHECK(!default_observed_in_frame("changed sampler binding")); *slot=saved;
                slot=(void **)(default_image+allocator_table+0x10); saved=*slot; *slot=(void *)1;
                CHECK(!default_observed_in_frame("changed allocator binding")); *slot=saved;
                CHECK(default_observed_in_frame("normal image bindings"));
            }
            if (route->rva == (renderer ? 0x14abb30u : 0x149f130u)) {
                /* R358 proves the native default derives its own directory
                 * and asks Materialize for this full animation name. This
                 * control exercises that actual hook argument; it does not
                 * replace native derivation with a guessed bare _default. */
                sh_resource_graph_frame frame; rg_materialize_fn saved = g_materialize;
                skeleton_animation_request = "models/actors/example/_default.md6anim";
                put_pointer(skeleton_animation, 8, skeleton_animation_request);
                put_pointer(skeleton_animation, 0x10, skeleton_animation_manager);
                put_pointer(skeleton_animation_manager, 8, "md6anim"); g_materialize = skeleton_materialize;
                sh_resource_graph_begin_requests(&frame, "decl", "skeleton-derived-name");
                CHECK(rg_materialize_hook(skeleton_animation_manager, skeleton_animation_request, 1, 0) == skeleton_animation);
                int observed = sh_resource_graph_native_default_observed(default_object);
                sh_resource_graph_end(&frame, observed); g_materialize = saved;
                CHECK(observed && default_names("skeleton-derived-name", "md6anim", skeleton_animation_request));
                CHECK(!default_names("skeleton-derived-name", "md6anim", "_default.md6anim"));
            }
            CHECK(default_names("retired", "material", "_tracemodel") == (route->reads == 1));
            /* Outside a request frame, with the hooks not live, unpinned, or at the other renderer's addresses:
             * nothing completes and nothing is recorded. */
            CHECK(!sh_resource_graph_native_default_observed(default_object));
            {
                sh_resource_graph_frame frame;
                sh_resource_graph_begin(&frame, "decl", "ordinary");
                CHECK(!sh_resource_graph_native_default_observed(default_object));
                sh_resource_graph_end(&frame, 1);
            }
            g_ready = 0; CHECK(!default_observed_in_frame("unready")); g_ready = 1;
            sh_resource_graph_native_test_default_image(default_image, -1);
            CHECK(!default_observed_in_frame("unpinned"));
            sh_resource_graph_native_test_default_image(default_image, !renderer);
            CHECK(!default_observed_in_frame("other renderer"));
            sh_resource_graph_native_test_default_image(default_image, renderer);
            CHECK(!default_recorded("ordinary") && !default_recorded("unready") && !default_recorded("unpinned") &&
                !default_recorded("other renderer"));
            /* A different address, or any pinned byte changed. */
            default_vtable[0x50 / 8] = routine + 1;
            CHECK(!default_observed_in_frame("moved"));
            default_vtable[0x50 / 8] = routine;
            for (size_t b = 0; b < route->length; b++) {
                routine[b] ^= 0x01;
                if (default_observed_in_frame("patched") || default_recorded("patched")) {
                    CHECK(0); fprintf(stderr, "renderer %d route %zu byte %zu\n", renderer, i, b);
                }
                routine[b] ^= 0x01;
            }
            CHECK(!default_recorded("moved"));
            if (route->reads) {
                /* The read is recorded only through the pinned load of an identifiable resource. */
                unsigned char *site = default_image + route->read[0].site;
                void **slot = (void **)(default_image + route->read[0].slot);
                *slot = NULL; CHECK(!default_observed_in_frame("unresolved handle"));
                *slot = default_object; put_pointer(default_object, 0x10, NULL);
                CHECK(!default_observed_in_frame("anonymous handle"));
                put_pointer(default_object, 0x10, default_manager); *slot = tracemodel;
                site[1] = 0x8d; CHECK(!default_observed_in_frame("lea site")); site[1] = 0x8b;
                site[3] ^= 0x08; CHECK(!default_observed_in_frame("other slot")); site[3] ^= 0x08;
                CHECK(!default_recorded("unresolved handle") && !default_recorded("anonymous handle") &&
                    !default_recorded("lea site") && !default_recorded("other slot"));
                CHECK(default_observed_in_frame("restored") && default_names("restored", "material", "_tracemodel"));
            }
            default_route_put(route, 1);
        }
        /* The generic declaration default never completes, and records its class parser's static-handle reads:
         * the material parser's three renderProgs, the particle parser's two materials. A class with any other parser,
         * or a parser moved or patched, records nothing. */
        sh_resource_graph_test_reset();
        default_route_put(declaration, 0);
        default_vtable[0x50 / 8] = default_image + declaration->rva;
        for (int p = 0; p < 2; p++) { default_route_put(parsers[p], 0); default_reads_put(parsers[p]); }
        {
            const rg_default_route *material = parsers[0]->reads == 3 ? parsers[0] : parsers[1];
            const rg_default_route *particle = parsers[0]->reads == 3 ? parsers[1] : parsers[0];
            default_vtable[0x98 / 8] = default_image + material->rva;
            CHECK(!default_observed_in_frame("material"));
            CHECK(default_names("material", "renderProg", "uberParticles") &&
                default_names("material", "renderProg", "particleLightAtlas") &&
                default_names("material", "renderProg", "virtualGuiBlend") && default_recorded("material") == 3);
            default_vtable[0x98 / 8] = default_image + particle->rva;
            CHECK(!default_observed_in_frame("particle"));
            CHECK(default_names("particle", "material", "textures/effects/particles/defaultgpu") &&
                default_names("particle", "material", "textures/effects/particles/default") && default_recorded("particle") == 2);
            parser_capture_cases(declaration, material);
            parser_capture_cases(declaration, particle);
            default_vtable[0x98 / 8] = default_image + particle->rva + 1;
            CHECK(!default_observed_in_frame("other parser") && !default_recorded("other parser"));
            default_vtable[0x98 / 8] = default_image + material->rva;
            default_image[material->rva] ^= 0x01;
            CHECK(!default_observed_in_frame("patched parser") && !default_recorded("patched parser"));
            default_image[material->rva] ^= 0x01;
            /* A parser read is recorded only by a declaration default: as a routine's own +0x50 nothing matches. */
            default_vtable[0x50 / 8] = default_image + material->rva;
            CHECK(!default_observed_in_frame("parser as default") && !default_recorded("parser as default"));
            for (int p = 0; p < 2; p++) default_route_put(parsers[p], 1);
        }
        default_route_put(declaration, 1);
    }
    /* An object whose vtable cannot be read. */
    put_pointer(default_object, 0, (void *)(uintptr_t)0x10);
    CHECK(!default_observed_in_frame("unreadable"));
    put_pointer(default_object, 0, default_vtable);
    sh_resource_graph_native_test_default_image(NULL, -2);
    g_test_default_parm_ready = -1;
    g_ready = ready;
    VirtualFree(default_image, 0, MEM_RELEASE); default_image = NULL;
    sh_resource_graph_test_reset();
}
/* Independent paired-layout fixture. Its destructor walks capacity, even at
 * logical zero, and never destroys a referenced declaration. The allocation
 * ledger catches stranded nested storage, borrowed frees and double cleanup. */
typedef struct editor_case {
    const char *family; size_t size;
    unsigned strings,lists; size_t string[9],list[8];
} editor_case;
static const editor_case editor_cases[] = {
    {"snapeditorentitydef",0x4a0,9,8,{0xa8,0xd8,0x108,0x138,0x168,0x198,0x1e0,0x240,0x2c0},
        {0x2a8,0x3b0,0x410,0x428,0x440,0x458,0x470,0x488}},
    {"snappropertyinspector_bitflag",0xb0,1,1,{0x68},{0x98}},
    {"snappropertyinspector_blockingvolumerendermodel",0xf8,2,1,{0x88,0xb8},{0x68}},
    {"snappropertyinspector_whitelistencounterdecl",0x80,0,1,{0},{0x68}},
    {"snappropertyinspector_whitelistencounterdecllist",0x80,0,1,{0},{0x68}}
};
static const editor_case *editor_kind;
static struct { void *p; int live,borrowed; } editor_blocks[1024];
static unsigned editor_blocks_count, editor_ctor_calls, editor_dtor_calls;
static int editor_admission = 1, editor_no_info, editor_ctor_fault, editor_dtor_fault;
static void *editor_vtable[3], *editor_info_vtable[6];
static void **editor_info_object = editor_info_vtable;
static void *editor_alloc(size_t bytes, int borrowed)
{
    void *p = calloc(1,bytes); CHECK(p && editor_blocks_count < 1024);
    if (p && editor_blocks_count < 1024) {
        editor_blocks[editor_blocks_count].p=p; editor_blocks[editor_blocks_count].live=1;
        editor_blocks[editor_blocks_count++].borrowed=borrowed;
    }
    return p;
}
static void editor_free(void *p)
{
    if (!p) return;
    for (unsigned i=0;i<editor_blocks_count;i++) if (editor_blocks[i].p==p && editor_blocks[i].live) {
        CHECK(!editor_blocks[i].borrowed); editor_blocks[i].live=0; free(p); return;
    }
    CHECK(0); /* Neither an unknown pointer nor an already freed one is owned. */
}
static unsigned editor_live(void)
{ unsigned n=0; for (unsigned i=0;i<editor_blocks_count;i++) n+=editor_blocks[i].live; return n; }
static void editor_string_init(unsigned char *s)
{
    /* Paired native idStr constructors: baseBuffer@28, capacity20. */
    uint32_t flags=0x80000014u;
    memset(s,0,0x30); put_pointer(s,0x10,s+0x1c); memcpy(s+0x18,&flags,4);
}
static void editor_string_fill(unsigned char *s, int borrowed)
{
    char *p=editor_alloc(48,borrowed); if (p) strcpy(p,"owned-or-borrowed-path");
    put_pointer(s,0x10,p); uint32_t flags=borrowed?0:0x40000030u;
    memcpy(s+0x18,&flags,4); int length=22; memcpy(s+8,&length,4);
}
static void editor_string_clear(unsigned char *s)
{
    void *p; uint32_t flags; memcpy(&p,s+0x10,8); memcpy(&flags,s+0x18,4);
    if (p && (flags&0x40000000u)) editor_free(p);
    memset(s+0x10,0,8); memset(s+8,0,4); flags&=0xc0000000u; memcpy(s+0x18,&flags,4);
}
static void editor_list_fill(unsigned char *list,size_t stride,int borrowed)
{
    put_pointer(list,0,editor_alloc(2*stride,borrowed)); int zero=0,two=2;
    memcpy(list+8,&zero,4); memcpy(list+12,&two,4); list[0x13]=borrowed?1:0;
}
static void editor_fill(unsigned char *live, int borrowed)
{
    for (unsigned i=0;i<editor_kind->strings;i++) {
        unsigned char *s=live+editor_kind->string[i];
        if (i==1) { int length=6; strcpy((char *)s+0x1c,"inline");
            put_pointer(s,0x10,s+0x1c); memcpy(s+8,&length,4); }
        else editor_string_fill(s,borrowed);
    }
    for (unsigned i=0;i<editor_kind->lists;i++) {
        size_t at=editor_kind->list[i],stride=8;
        if (at==0x3b0) stride=0x20;
        if (!strcmp(editor_kind->family,"snappropertyinspector_bitflag")) stride=0x38;
        if (!strcmp(editor_kind->family,"snappropertyinspector_blockingvolumerendermodel")) stride=0x40;
        unsigned char *list=live+at; editor_list_fill(list,stride,borrowed);
        unsigned char *rows=*(unsigned char **)list;
        if (at==0x3b0) for (unsigned j=0;j<2;j++) {
            unsigned char *nested=rows+j*0x20+8; editor_list_fill(nested,0x40,borrowed);
            unsigned char *controls=*(unsigned char **)nested;
            for (unsigned k=0;k<2;k++) {
                editor_string_init(controls+k*0x40); editor_string_fill(controls+k*0x40,borrowed);
                put_pointer(controls+k*0x40,0x38,(void *)0x12345678);
            }
        }
        if (stride==0x38) for (unsigned j=0;j<2;j++) {
            editor_string_init(rows+j*stride); editor_string_fill(rows+j*stride,borrowed);
        }
    }
}
static void editor_list_clear(unsigned char *list,size_t at)
{
    unsigned char *rows=*(unsigned char **)list; int capacity; memcpy(&capacity,list+12,4);
    if (list[0x13]==0 || list[0x13]==3 || list[0x13]==4) {
        if (rows && at==0x3b0) for (int j=0;j<capacity;j++) {
            unsigned char *nested=rows+j*0x20+8,*controls=*(unsigned char **)nested; int slots;
            memcpy(&slots,nested+12,4);
            if (nested[0x13]==0 || nested[0x13]==3 || nested[0x13]==4) {
                for (int k=0;controls && k<slots;k++) editor_string_clear(controls+k*0x40);
                editor_free(controls);
            }
        }
        if (rows && !strcmp(editor_kind->family,"snappropertyinspector_bitflag"))
            for (int j=0;j<capacity;j++) editor_string_clear(rows+j*0x38);
        editor_free(rows); memset(list,0,0x10);
    }
}
static void editor_destroy(void *object,unsigned flags)
{
    unsigned char *live=object; CHECK(flags==0); editor_dtor_calls++;
    for (unsigned i=0;i<editor_kind->lists;i++) {
        editor_list_clear(live+editor_kind->list[i],editor_kind->list[i]);
        if (editor_dtor_fault) { editor_dtor_fault=0; RaiseException(0xe0410010,0,0,NULL); }
    }
    for (unsigned i=0;i<editor_kind->strings;i++) editor_string_clear(live+editor_kind->string[i]);
}
static void editor_construct(void *info,void *object)
{
    unsigned char *live=object; (void)info; editor_ctor_calls++;
    memset(live,0,editor_kind->size); put_pointer(live,0,editor_vtable);
    for (unsigned i=0;i<editor_kind->strings;i++) editor_string_init(live+editor_kind->string[i]);
    for (unsigned i=0;i<editor_kind->lists;i++) { uint32_t ctrl=0x50000; memcpy(live+editor_kind->list[i]+16,&ctrl,4); }
    if (!strcmp(editor_kind->family,"snapeditorentitydef")) {
        uint64_t enum_default=3,external=0; put_pointer(live,0x68,(void *)(uintptr_t)enum_default);
        put_pointer(live,0x228,(void *)(uintptr_t)external); live[0x300]=1; live[0x388]=1;
        uint32_t tag=100; memcpy(live+0x9c,&tag,4);
    }
    if (editor_ctor_fault) { editor_ctor_fault=0; RaiseException(0xe0410011,0,0,NULL); }
}
static void *editor_info(void *object)
{ (void)object; return editor_no_info?NULL:&editor_info_object; }
static int editor_admitted(void *context) { return context==&editor_admission && editor_admission; }
static void editor_attach(unsigned char *live,unsigned char owner[16])
{
    put_pointer(owner,8,editor_kind->family); put_pointer(live,8,"collection-fixture");
    put_pointer(live,0x10,owner); live[0x20]=4; live[0x34]=0xa5;
    put_pointer(live,0x38,(void *)0x334455); put_pointer(live,0x58,(void *)0x556677); live[0x60]=1;
}
static void editor_quarantine_dispose(void)
{
    /* Only the synthetic fixture disposes ledger blocks and wrapper memory
     * directly. It never retries the interrupted native destructor. */
    while (g_editor_quarantine) {
        sh_resource_editor_state *s=g_editor_quarantine; g_editor_quarantine=s->quarantine_next;
        while (s->values) { rg_editor_value *v=s->values; s->values=v->next; free(v->scratch); free(v); }
        free(s);
    }
    g_editor_damaged=0;
}
static void test_editor_storage_scope(void)
{
    char error[256]; int committed = 0, caught = 0;
    memset(definition, 0, sizeof(definition)); memset(manager, 0, sizeof(manager));
    put_pointer(manager, 8, "entitydef"); put_pointer(definition, 8, "native-default-probe");
    put_pointer(definition, 0x10, manager);
    g_ready = 0; g_parse = fake_parse; g_read = fake_read;
    expected_resource = definition; parse_result = read_result = 1;
    read_delegates = read_noop = throw_error = 0;
    sh_resource_editor_state *state = sh_resource_graph_native_editor_state_begin(
        editor_admitted, &editor_admission, error, sizeof(error));
    CHECK(state);
    /* Native default/probe failures outside the five owning families retain
     * their native result. They must not suppress subsequent editor parsing. */
    CHECK(rg_parse_hook(definition, 7, (void *)19) == 1);
    CHECK(sh_resource_graph_native_editor_state_ok(state) && !state->values);
    CHECK(rg_read_hook(definition, (void *)19) == 1);
    CHECK(sh_resource_graph_native_editor_state_ok(state) && !state->values);
    read_delegates = 1;
    CHECK(rg_read_hook(definition, (void *)19) == 1);
    CHECK(sh_resource_graph_native_editor_state_ok(state) && !state->values);
    throw_error = 1;
    __try { rg_parse_hook(definition, 7, (void *)19); }
    __except (EXCEPTION_EXECUTE_HANDLER) { caught = 1; }
    CHECK(caught && sh_resource_graph_native_editor_state_ok(state) && !state->values);
    throw_error = read_delegates = 0; parse_result = read_result = 0;
    CHECK(sh_resource_graph_native_editor_state_end(&state, 1, &committed, error, sizeof(error)) == 1);
    CHECK(!state && committed);
}
static void test_editor_storage(void)
{
    char error[256]; int committed=0;
    editor_vtable[0]=editor_destroy; editor_vtable[2]=editor_info; editor_info_vtable[5]=editor_construct;
    for (unsigned family=0;family<5;family++) {
        editor_kind=editor_cases+family;
        for (int borrowed=0;borrowed<2;borrowed++) for (int accept=0;accept<2;accept++) {
            unsigned char live[0x4a0],original_bytes[0x4a0],owner[16]={0};
            editor_construct(NULL,live); editor_attach(live,owner); editor_fill(live,borrowed);
            if (!family) { live[0x300]=0; live[0x68]=7; put_pointer(live,0x228,(void *)0xabc);
                live[0x3d1]=0xfd; live[0x3d2]=0x9b; }
            if (family==2) { live[0x80]=1; put_pointer(live,0xe8,(void *)0xabc); }
            memcpy(original_bytes,live,editor_kind->size);
            unsigned old_blocks=editor_live();
            sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(editor_admitted,&editor_admission,error,sizeof(error));
            CHECK(state && rg_editor_read_begin(live));
            CHECK(!*(void **)(live+editor_kind->list[0]) && !*(void **)(live+0x58) && !live[0x60]);
            CHECK(live[0x20]==4 && live[0x34]==original_bytes[0x34] &&
                *(void **)(live+0x38)==(void *)0x334455);
            if (!family) CHECK(live[0x300]==1 && live[0x68]==3 && !*(void **)(live+0x228) &&
                live[0x3d1]==0xc0 && live[0x3d2]==0x9b);
            if (family==2) CHECK(!live[0x80] && !*(void **)(live+0xe8));
            editor_fill(live,0); unsigned after_first=editor_live();
            /* Nested Read->Parse must not reset the same logical read. */
            void *first=*(void **)(live+editor_kind->list[0]);
            CHECK(rg_editor_read_begin(live) && *(void **)(live+editor_kind->list[0])==first);
            rg_editor_read_end(live); rg_editor_read_end(live);
            /* A second read gets ctor defaults and retains intermediate owners. */
            CHECK(rg_editor_read_begin(live) && !*(void **)(live+editor_kind->list[0]));
            CHECK(editor_live()==after_first); editor_fill(live,0); rg_editor_read_end(live);
            /* A second reconstruction also resets the candidate, not the saved original. */
            CHECK(rg_editor_prepare(live,1) && !*(void **)(live+editor_kind->list[0])); editor_fill(live,0);
            if (!family) { live[0x300]=0; live[0x68]=9; put_pointer(live,0x228,(void *)0xdef); }
            editor_admission=0;
            CHECK(sh_resource_graph_native_editor_state_end(&state,accept,&committed,error,sizeof(error))==0 && state && !committed);
            editor_admission=1;
            CHECK(sh_resource_graph_native_editor_state_end(&state,accept,&committed,error,sizeof(error))==1 && !state && committed==accept);
            if (!accept) CHECK(!memcmp(live,original_bytes,editor_kind->size) && editor_live()==old_blocks);
            else CHECK(editor_live()==old_blocks+(borrowed?old_blocks:0));
            editor_destroy(live,0);
            /* Borrowed callers own every nested slot and remain untouched. */
            for (unsigned i=0;i<editor_blocks_count;i++) if (editor_blocks[i].live) {
                CHECK(editor_blocks[i].borrowed); free(editor_blocks[i].p); editor_blocks[i].live=0;
            }
            CHECK(!editor_live()); editor_blocks_count=0;
        }
    }
    editor_kind=editor_cases;
    for (unsigned fail=1;fail<=3;fail++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0}; editor_construct(NULL,live); editor_attach(live,owner);
        memcpy(saved,live,sizeof(live));
        sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(editor_admitted,&editor_admission,error,sizeof(error));
        if (fail<3) g_editor_allocation_fault=fail; else editor_no_info=1;
        CHECK(!rg_editor_prepare(live,0)); g_editor_allocation_fault=0; editor_no_info=0;
        CHECK(!memcmp(live,saved,sizeof(live)));
        CHECK(sh_resource_graph_native_editor_state_end(&state,1,&committed,error,sizeof(error))==1 && !state && !committed);
    }
    for (int fault=0;fault<2;fault++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0}; editor_construct(NULL,live); editor_attach(live,owner);
        editor_fill(live,0); memcpy(saved,live,sizeof(live));
        sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(editor_admitted,&editor_admission,error,sizeof(error));
        if (!fault) { editor_ctor_fault=1; CHECK(!rg_editor_prepare(live,0)); }
        else { CHECK(rg_editor_prepare(live,0)); editor_fill(live,0); editor_dtor_fault=1; }
        unsigned dtors=editor_dtor_calls;
        CHECK(sh_resource_graph_native_editor_state_end(&state,fault,&committed,error,sizeof(error))==-1 && !state && committed==fault);
        CHECK(editor_dtor_calls==dtors+(fault?1:0));
        if (!fault) CHECK(!memcmp(live,saved,sizeof(live)));
        CHECK(!sh_resource_graph_native_editor_state_begin(editor_admitted,&editor_admission,error,sizeof(error)));
        CHECK(sh_resource_graph_native_editor_state_end(&state,0,&committed,error,sizeof(error))==1);
        CHECK(editor_dtor_calls==dtors+(fault?1:0));
        editor_quarantine_dispose();
        for (unsigned i=0;i<editor_blocks_count;i++) if (editor_blocks[i].live) { free(editor_blocks[i].p); editor_blocks[i].live=0; }
        editor_blocks_count=0;
    }
    CHECK(!g_editor_state && !editor_live());
}

static unsigned editor_header_reads;
static unsigned editor_header_result;
static int editor_header_noop;
static unsigned int editor_header_read(void *resource,void *error)
{
    unsigned char *live=resource; (void)error; editor_header_reads++;
    if (editor_header_noop || live[0x48]) return 0;
    /* Native successful Read does not clear a stale DEFAULTED bit. */
    CHECK((live[0x2c]&0x0c)==0);
    editor_fill(live,0);
    if (editor_header_result) live[0x2c]|=4;
    return editor_header_result;
}
static void editor_header_load(void *resource)
{
    unsigned char *live=resource;
    put_pointer(live,0x18,NULL); live[0x24]|=1;
    if (!rg_read_hook(resource,NULL)) live[0x2c]&=0xfcu;
    else live[0x2c]|=4;
}
static void test_editor_reconstruct_header(void)
{
    char error[256]; int committed=0;
    rg_read_fn previous_read=g_read; rg_load_fn previous_load=g_load;
    LONG previous_ready=g_ready;
    g_ready=0; g_read=editor_header_read; g_load=editor_header_load;
    editor_header_reads=editor_header_result=0; editor_header_noop=0;
    editor_vtable[0]=editor_destroy; editor_vtable[2]=editor_info;
    editor_info_vtable[5]=editor_construct;
    /* Every admitted family shares the same idResource constructor header. */
    for (unsigned family=0;family<5;family++) for (unsigned initial=0;initial<3;initial++)
        for (int mode=0;mode<3;mode++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0};
        unsigned char old_low=initial==0?0:initial==1?4:15;
        editor_kind=editor_cases+family;
        editor_construct(NULL,live); editor_attach(live,owner); editor_fill(live,0);
        live[0x24]=0x81; live[0x28]=4; live[0x2c]=0xa0|old_low;
        memcpy(saved,live,editor_kind->size);
        sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(
            editor_admitted,&editor_admission,error,sizeof(error));
        CHECK(state);
        unsigned reads=editor_header_reads,dtors=editor_dtor_calls;
        for (unsigned repeat=0;repeat<2+(mode==2);repeat++) {
            if (repeat) live[0x2c]|=15; /* Candidate can itself be defaulted. */
            rg_reconstruct_hook(live);
            CHECK(sh_resource_graph_native_editor_state_ok(state));
            CHECK(live[0x2c]==0xa0);
            CHECK(!memcmp(live,saved,0x2c) && !memcmp(live+0x2d,saved+0x2d,0x2b));
            live[0x2c]|=1; /* Exercise bit0 IN_PROGRESS at Read entry. */
            editor_header_result=mode==2 && repeat==2;
            rg_load_hook(live);
            CHECK((live[0x2c]&0x0f)==(editor_header_result?4:0));
            CHECK(sh_resource_graph_native_editor_state_ok(state)==!editor_header_result);
        }
        CHECK(editor_header_reads==reads+2+(mode==2));
        unsigned snapshots=0;
        for (rg_editor_value *v=state?state->values:NULL;v;v=v->next) snapshots++;
        CHECK(snapshots==2+(mode==2));
        /* Unowned high bits may change while this transaction is active. */
        live[0x2c]=(live[0x2c]&15)|0xd0;
        int accept=mode!=0,expect_commit=mode==1;
        CHECK(sh_resource_graph_native_editor_state_end(&state,accept,&committed,error,sizeof(error))==1);
        CHECK(!state && committed==expect_commit && editor_dtor_calls==dtors+snapshots);
        CHECK(live[0x2c]==(0xd0|(expect_commit?0:old_low)));
        if (!expect_commit) {
            saved[0x2c]=0xd0|old_low;
            CHECK(!memcmp(live,saved,editor_kind->size));
        } else CHECK(!memcmp(live,saved,0x2c) && !memcmp(live+0x2d,saved+0x2d,0x2b));
        editor_destroy(live,0); CHECK(!editor_live()); editor_blocks_count=0;
        editor_header_result=0;
    }
    /* Unchanged outside scope, implicit/no-source and unrelated families do
     * not acquire header-reset or owning-payload rollback obligations. */
    editor_kind=editor_cases;
    for (unsigned control=0;control<3;control++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0};
        editor_construct(NULL,live); editor_attach(live,owner); live[0x2c]=0xaf;
        if (control==1) live[0x48]=1;
        if (control==2) put_pointer(owner,8,"entitydef");
        memcpy(saved,live,editor_kind->size);
        sh_resource_editor_state *state=control?sh_resource_graph_native_editor_state_begin(
            editor_admitted,&editor_admission,error,sizeof(error)):NULL;
        editor_header_noop=1;
        CHECK(!rg_read_hook(live,NULL) && !memcmp(live,saved,editor_kind->size));
        if (state) {
            CHECK(sh_resource_graph_native_editor_state_ok(state) && !state->values);
            CHECK(sh_resource_graph_native_editor_state_end(&state,1,&committed,error,sizeof(error))==1 && committed);
        }
        editor_header_noop=0; editor_destroy(live,0); CHECK(!editor_live());
    }
    /* Preparation cannot clear the live flags before allocation/constructor
     * success. Partial scratch construction remains quarantined, not retried. */
    for (unsigned fault=0;fault<4;fault++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0};
        editor_construct(NULL,live); editor_attach(live,owner); live[0x2c]=0xaf;
        memcpy(saved,live,editor_kind->size);
        sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(
            editor_admitted,&editor_admission,error,sizeof(error));
        CHECK(state);
        if (fault<2) g_editor_allocation_fault=fault+1;
        else if (fault==2) editor_no_info=1;
        else editor_ctor_fault=1;
        rg_reconstruct_hook(live);
        g_editor_allocation_fault=0; editor_no_info=0;
        CHECK(!sh_resource_graph_native_editor_state_ok(state) && !memcmp(live,saved,editor_kind->size));
        int ended=sh_resource_graph_native_editor_state_end(&state,1,&committed,error,sizeof(error));
        CHECK(ended==(fault==3?-1:1) && !state && !committed);
        if (fault==3) editor_quarantine_dispose();
        CHECK(!memcmp(live,saved,editor_kind->size));
        editor_destroy(live,0); CHECK(!editor_live());
    }
    CHECK(!g_editor_state && !g_editor_damaged && !editor_live());
    g_read=previous_read; g_load=previous_load; g_ready=previous_ready;
}

static unsigned char editor_direct_incoming;
static unsigned editor_direct_calls;
static unsigned int editor_direct_native(void *resource)
{
    unsigned char *live=resource;
    /* Direct native readers must receive their caller's state, including
     * bit0 IN_PROGRESS; owning-payload preparation is not reconstruction. */
    CHECK(live[0x2c]==editor_direct_incoming);
    editor_direct_calls++; editor_fill(live,0);
    live[0x2c]&=0xf0; /* Synthetic native completion consumes its low bits. */
    return 0;
}
static unsigned int editor_direct_read(void *resource,void *error)
{ CHECK(!error); return editor_direct_native(resource); }
static unsigned int editor_direct_parse(void *resource,unsigned char source,void *error)
{ CHECK(!source && !error); return editor_direct_native(resource); }
static void test_editor_header_reset_scope(void)
{
    char error[256]; int committed=0;
    rg_read_fn previous_read=g_read; rg_parse_fn previous_parse=g_parse;
    LONG previous_ready=g_ready;
    g_ready=0; g_read=editor_direct_read; g_parse=editor_direct_parse;
    editor_vtable[0]=editor_destroy; editor_vtable[2]=editor_info;
    editor_info_vtable[5]=editor_construct;
    for (unsigned family=0;family<5;family++) for (unsigned parse=0;parse<2;parse++)
        for (unsigned mixed=0;mixed<2;mixed++) for (int accept=0;accept<2;accept++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0};
        editor_kind=editor_cases+family;
        editor_construct(NULL,live); editor_attach(live,owner); editor_fill(live,0);
        live[0x2c]=0xa1; memcpy(saved,live,editor_kind->size);
        sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(
            editor_admitted,&editor_admission,error,sizeof(error));
        CHECK(state); unsigned calls_before=editor_direct_calls;
        rg_editor_value *original=NULL;
        for (unsigned repeat=0;repeat<3;repeat++) {
            editor_direct_incoming=0xa0|(1u<<repeat); live[0x2c]=editor_direct_incoming;
            if (parse) CHECK(!rg_parse_hook(live,0,NULL));
            else CHECK(!rg_read_hook(live,NULL));
            CHECK(live[0x2c]==0xa0 && sh_resource_graph_native_editor_state_ok(state));
            if (!repeat) original=rg_editor_find(live);
            CHECK(original && rg_editor_find(live)==original);
        }
        if (mixed) {
            /* The original payload was captured at direct-read entry, but
             * its untouched transient flags are not the later reset snapshot. */
            live[0x2c]=0xb6; rg_reconstruct_hook(live);
            CHECK(live[0x2c]==0xb0 && rg_editor_find(live)==original);
            live[0x2c]=0xe9; rg_reconstruct_hook(live);
            CHECK(live[0x2c]==0xe0 && rg_editor_find(live)==original);
            editor_direct_incoming=0xe1; live[0x2c]=editor_direct_incoming;
            if (parse) CHECK(!rg_parse_hook(live,0,NULL));
            else CHECK(!rg_read_hook(live,NULL));
            CHECK(live[0x2c]==0xe0);
        }
        CHECK(editor_direct_calls==calls_before+3+mixed);
        /* Native completion and an independent high-state change remain
         * authoritative when this adapter never reset the low state. */
        live[0x2c]=0xd0;
        unsigned snapshots=0,dtors=editor_dtor_calls;
        for (rg_editor_value *v=state?state->values:NULL;v;v=v->next) snapshots++;
        CHECK(snapshots==3+2*mixed);
        CHECK(sh_resource_graph_native_editor_state_end(&state,accept,&committed,error,sizeof(error))==1);
        CHECK(!state && committed==accept && editor_dtor_calls==dtors+snapshots);
        CHECK(live[0x2c]==(0xd0|(!accept && mixed?6:0)));
        if (!accept) {
            saved[0x2c]=0xd0|(mixed?6:0);
            CHECK(!memcmp(live,saved,editor_kind->size));
        }
        editor_destroy(live,0); CHECK(!editor_live()); editor_blocks_count=0;
    }
    g_read=previous_read; g_parse=previous_parse; g_ready=previous_ready;
    CHECK(!g_editor_state && !editor_live());
}

/* A rejected transaction whose header this adapter never reset leaves the
 * native caller's completed low state alone, including nonzero bits. */
static void test_editor_header_untouched_rejection(void)
{
    char error[256]; int committed=0;
    rg_read_fn previous_read=g_read; rg_parse_fn previous_parse=g_parse;
    LONG previous_ready=g_ready;
    g_ready=0; g_read=editor_direct_read; g_parse=editor_direct_parse;
    editor_vtable[0]=editor_destroy; editor_vtable[2]=editor_info;
    editor_info_vtable[5]=editor_construct;
    for (unsigned family=0;family<5;family++) for (unsigned parse=0;parse<2;parse++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0};
        editor_kind=editor_cases+family;
        editor_construct(NULL,live); editor_attach(live,owner); editor_fill(live,0);
        live[0x2c]=0xa1; memcpy(saved,live,editor_kind->size);
        sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(
            editor_admitted,&editor_admission,error,sizeof(error));
        CHECK(state);
        editor_direct_incoming=0xa1;
        if (parse) CHECK(!rg_parse_hook(live,0,NULL));
        else CHECK(!rg_read_hook(live,NULL));
        live[0x2c]=0xd4; /* The caller completed with a low bit of its own. */
        CHECK(sh_resource_graph_native_editor_state_end(&state,0,&committed,error,sizeof(error))==1);
        CHECK(!state && !committed && live[0x2c]==0xd4);
        saved[0x2c]=0xd4; CHECK(!memcmp(live,saved,editor_kind->size));
        editor_destroy(live,0); CHECK(!editor_live()); editor_blocks_count=0;
    }
    g_read=previous_read; g_parse=previous_parse; g_ready=previous_ready;
    CHECK(!g_editor_state && !editor_live());
}

static void editor_set_inline(unsigned char *s,const char *text)
{
    int length=(int)strlen(text); CHECK(length<20);
    editor_string_init(s); memcpy(s+0x1c,text,(size_t)length+1); memcpy(s+8,&length,4);
}
static void editor_check_inline(unsigned char *s,const char *text)
{
    void *data; int length; uint32_t flags;
    memcpy(&data,s+0x10,8); memcpy(&length,s+8,4); memcpy(&flags,s+0x18,4);
    CHECK(data==s+0x1c && length==(int)strlen(text) && flags==0x80000014u);
    CHECK(!memcmp(s+0x1c,text,strlen(text)+1));
}
static void test_editor_native_inline_storage(void)
{
    char error[256]; int committed;
    /* The expected offset is the paired constructor's byte28, independently
     * of the adapter. Exercise every transferred string, including hide path. */
    for (unsigned family=0;family<3;family++) for (int accept=0;accept<2;accept++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0};
        editor_kind=editor_cases+family;
        editor_construct(NULL,live); editor_attach(live,owner);
        for (unsigned i=0;i<editor_kind->strings;i++)
            editor_set_inline(live+editor_kind->string[i],"original.path");
        memcpy(saved,live,editor_kind->size);
        sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(
            editor_admitted,&editor_admission,error,sizeof(error));
        CHECK(state && rg_editor_read_begin(live));
        rg_editor_value *original=rg_editor_find(live); CHECK(original);
        for (unsigned i=0;i<editor_kind->strings;i++) {
            size_t at=editor_kind->string[i];
            editor_check_inline(live+at,"");
            if (original) editor_check_inline((unsigned char *)original->scratch+at,"original.path");
            editor_set_inline(live+at,"first.path");
        }
        rg_editor_read_end(live);
        CHECK(rg_editor_read_begin(live));
        /* The second parse resets its live strings and retains both prior
         * payloads at their own inline addresses until the decision. */
        rg_editor_value *intermediate=state?state->values:NULL;
        CHECK(intermediate && intermediate!=original);
        for (unsigned i=0;i<editor_kind->strings;i++) {
            size_t at=editor_kind->string[i];
            editor_check_inline(live+at,"");
            if (original) editor_check_inline((unsigned char *)original->scratch+at,"original.path");
            if (intermediate) editor_check_inline((unsigned char *)intermediate->scratch+at,"first.path");
            editor_set_inline(live+at,"candidate.path");
        }
        rg_editor_read_end(live);
        CHECK(sh_resource_graph_native_editor_state_end(&state,accept,&committed,error,sizeof(error))==1 &&
            !state && committed==accept);
        for (unsigned i=0;i<editor_kind->strings;i++)
            editor_check_inline(live+editor_kind->string[i],accept?"candidate.path":"original.path");
        if (!accept) CHECK(!memcmp(live,saved,editor_kind->size));
        editor_destroy(live,0); CHECK(!editor_live());
    }
    /* Native short/empty assignment may retain an external owned capacity.
     * Length alone must never reclassify that allocation as inline/borrowed. */
    editor_kind=editor_cases;
    for (int accept=0;accept<2;accept++) {
        unsigned char live[0x4a0],saved[0x4a0],owner[16]={0};
        editor_construct(NULL,live); editor_attach(live,owner);
        unsigned char *hide=live+0xd8; editor_string_fill(hide,0);
        char *old=*(char **)(hide+0x10); old[0]=0; memset(hide+8,0,4);
        memcpy(saved,live,sizeof(live));
        sh_resource_editor_state *state=sh_resource_graph_native_editor_state_begin(
            editor_admitted,&editor_admission,error,sizeof(error));
        CHECK(state && rg_editor_read_begin(live)); editor_check_inline(hide,"");
        editor_string_fill(hide,0); char *candidate=*(char **)(hide+0x10);
        strcpy(candidate,"short.path"); int length=10; memcpy(hide+8,&length,4);
        candidate[0]=0; memset(hide+8,0,4); rg_editor_read_end(live);
        CHECK(sh_resource_graph_native_editor_state_end(&state,accept,&committed,error,sizeof(error))==1 &&
            !state && committed==accept);
        CHECK(*(char **)(hide+0x10)==(accept?candidate:old) && !*(int *)(hide+8) &&
            *(uint32_t *)(hide+0x18)==0x40000030u && !(*(char **)(hide+0x10))[0]);
        CHECK(editor_live()==1);
        if (!accept) CHECK(!memcmp(live,saved,sizeof(live)));
        editor_destroy(live,0); CHECK(!editor_live()); editor_blocks_count=0;
    }
}

/* R-309 copied-size history controls: the pre-capture walk under the decoded native list lock, every
 * failure that must leave the history unbegun, and the getter reads, including reads that cannot be
 * attributed to an image, which must invalidate it for good. */
static int size_decode_ok = 1, size_acquire_fails, size_lock_null, size_acquires, size_releases;
static int size_lock_object;
static void *size_head;
static void *size_lock(void) { return size_lock_null ? NULL : &size_lock_object; }
static unsigned size_acquire(void *lock, char blocking)
{
    CHECK(lock == &size_lock_object && blocking == 1);
    if (size_acquire_fails) return 0;
    size_acquires++; return 1;
}
static void size_release(void *lock) { CHECK(lock == &size_lock_object); size_releases++; }
int sh_image_exclusion_decode(const unsigned char *purge, sh_image_exclusion_native *out)
{
    memset(out, 0, sizeof(*out));
    if (!size_decode_ok || !purge) return 0;
    out->lock = (const void *)(uintptr_t)size_lock; out->acquire = (const void *)(uintptr_t)size_acquire;
    out->release = (const void *)(uintptr_t)size_release; out->head = &size_head;
    return 1;
}
static unsigned char size_lists[2][0x30], size_manager[16], size_other_manager[16], size_images[3][0x18];
static void *size_items[3];
static unsigned char size_material[0x190];
static int size_getter_calls;
static void *size_getter_sets;
static int size_getter(void *material)
{
    size_getter_calls++;
    if (size_getter_sets) put_pointer(material, 0x188, size_getter_sets);
    return 64;
}
static void size_registry(int count)
{
    static const char *names[3] = {"env/a", "env/b", "env/c"};
    memset(size_lists, 0, sizeof(size_lists)); memset(size_images, 0, sizeof(size_images));
    put_pointer(size_manager, 8, "image"); put_pointer(size_other_manager, 8, "model");
    put_pointer(size_lists[0], 8, "model"); put_pointer(size_lists[0], 0x18, size_lists[1]);
    put_pointer(size_lists[1], 8, "image"); put_pointer(size_lists[1], 0x20, size_items);
    *(int *)(size_lists[1] + 0x28) = count;
    for (int i = 0; i < 3; i++) {
        size_items[i] = size_images[i];
        put_pointer(size_images[i], 8, names[i]); put_pointer(size_images[i], 0x10, size_manager);
    }
    size_head = size_lists[0];
    size_acquires = size_releases = 0;
}
static long long size_walk(void)
{
    rg_list_lock native = {size_lock, size_acquire, size_release, (void *const *)&size_head};
    return rg_size_precapture(&native);
}
static void size_history_checks(void)
{
    rg_list_lock missing = {0};
    sh_resource_graph_test_reset();
    /* A complete walk marks every registered image and releases the lock it took. */
    size_registry(2);
    CHECK(size_walk() == 2 && size_acquires == 1 && size_releases == 1);
    CHECK(sh_resource_graph_history("image", "env/a") == SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE &&
        sh_resource_graph_history("image", "env/b") == SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE &&
        !sh_resource_graph_history("image", "env/c"));
    /* No image list yet: nothing is registered, so nothing is missed. */
    sh_resource_graph_test_reset(); size_registry(2); put_pointer(size_lists[0], 0x18, NULL);
    CHECK(size_walk() == 0 && size_releases == 1);
    /* Every failure leaves coverage incomplete: no binding, no lock, a refused lock, a cycle past the
     * list bound, a list without a type, an implausible array, a null entry, an entry that cannot be
     * identified or is not an image, and a fault (the lock is still released). */
    CHECK(rg_size_precapture(&missing) == -1 && rg_size_precapture(NULL) == -1);
    size_registry(2); size_lock_null = 1; CHECK(size_walk() == -1 && !size_acquires); size_lock_null = 0;
    size_registry(2); size_acquire_fails = 1; CHECK(size_walk() == -1 && !size_releases); size_acquire_fails = 0;
    size_registry(2); put_pointer(size_lists[1], 0x18, size_lists[0]); CHECK(size_walk() == -1 && size_releases == 1);
    size_registry(2); put_pointer(size_lists[0], 8, NULL); CHECK(size_walk() == -1);
    size_registry(2); *(int *)(size_lists[1] + 0x28) = -1; CHECK(size_walk() == -1);
    size_registry(2); put_pointer(size_lists[1], 0x20, NULL); CHECK(size_walk() == -1);
    size_registry(2); size_items[1] = NULL; CHECK(size_walk() == -1);
    size_registry(2); put_pointer(size_images[1], 8, NULL); CHECK(size_walk() == -1);
    size_registry(2); put_pointer(size_images[1], 0x10, size_other_manager); CHECK(size_walk() == -1);
    size_registry(2); size_items[1] = (void *)(uintptr_t)0x10; CHECK(size_walk() == -1 && size_releases == 1);
    size_registry(2); size_head = (void *)(uintptr_t)0x10; CHECK(size_walk() == -1 && size_releases == 1);

    /* Install: the getters hook only with the decoded lock whose head is the globals head; the history
     * begins only after a complete walk. */
    sig_result results[3] = {
        {"MaterialImageHeight", SIG_OK, (uintptr_t)size_getter, 0},
        {"MaterialImageWidth", SIG_OK, (uintptr_t)size_getter, 0},
        {"ResourcePurgeLevel", SIG_OK, (uintptr_t)size_lists, 0}};
    void **saved_head = g_resource_head;
    LONG saved_ready = g_ready;
    g_ready = 1; size_install_fake = 1;
    for (int failure = 0; failure < 5; failure++) {
        sh_resource_graph_test_reset(); size_registry(2);
        g_size_attempted = 0; g_material_height = g_material_width = NULL;
        g_resource_head = failure == 1 ? (void **)&size_lists : (void **)&size_head;
        size_decode_ok = failure != 0;
        results[2].status = failure == 2 ? SIG_NOT_FOUND : SIG_OK;
        if (failure == 3) size_items[0] = NULL;
        results[0].status = failure == 4 ? SIG_NOT_FOUND : SIG_OK;
        rg_size_install(results, 3);
        CHECK(!sh_resource_graph_size_history_ready());
        CHECK(failure >= 3 || (!g_material_height && !g_material_width));
        results[0].status = SIG_OK;
    }
    sh_resource_graph_test_reset(); size_registry(2);
    g_size_attempted = 0; g_material_height = g_material_width = NULL;
    g_resource_head = (void **)&size_head; size_decode_ok = 1; results[2].status = SIG_OK;
    rg_size_install(results, 3);
    CHECK(sh_resource_graph_size_history_ready() && g_material_height == size_getter && g_material_width == size_getter);
    CHECK(sh_resource_graph_history("image", "env/a") == SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE);
    rg_size_install(results, 3); /* Idempotent: one attempt per process. */

    /* Getter reads: the editor image the getter left at +0x188 is marked; a null field reads nothing. */
    memset(size_material, 0, sizeof(size_material));
    size_getter_sets = size_images[2]; size_getter_calls = 0;
    CHECK(rg_material_height_hook(size_material) == 64 && size_getter_calls == 1 && !sh_size_owners_collecting());
    CHECK(sh_resource_graph_history("image", "env/c") == SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    memset(size_material, 0, sizeof(size_material)); size_getter_sets = NULL;
    CHECK(rg_material_width_hook(size_material) == 64 && sh_resource_graph_size_history_ready());
    /* A lookup inside a getter is a read of what it returns; outside, it is not. */
    rg_size_lookup(size_images[1]);
    CHECK(sh_resource_graph_history("image", "env/b") == SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE);
    {
        sh_size_bracket call;
        sh_size_owners_collect_begin(&call, size_material, SH_SIZE_AXIS_HEIGHT);
        rg_size_lookup(size_images[0]);
        sh_size_owners_collect_end(&call, 1, NULL, NULL, NULL, NULL);
    }
    CHECK(sh_resource_graph_history("image", "env/a") ==
        (SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE | SH_RESOURCE_GRAPH_HISTORY_ESCAPED));
    static unsigned char model_object[0x18];
    put_pointer(model_object, 8, "body"); put_pointer(model_object, 0x10, size_other_manager);
    {
        /* A non-image identity is not an image read. */
        sh_size_bracket call;
        sh_size_owners_collect_begin(&call, size_material, SH_SIZE_AXIS_HEIGHT);
        rg_size_lookup(model_object);
        CHECK(!so_collected_count && !so_collect_failed);
        sh_size_owners_collect_end(&call, 1, NULL, NULL, NULL, NULL);
    }
    CHECK(!sh_resource_graph_history("model", "body"));
    CHECK(sh_resource_graph_size_history_ready());
    /* An unreadable editor-image field, or a read whose object cannot be identified, invalidates the
     * history for good: a later begin does not restore it. */
    for (int mode = 0; mode < 2; mode++) {
        sh_resource_graph_test_reset(); sh_resource_graph_size_history_begin();
        if (mode == 0) {
            size_getter_sets = NULL;
            CHECK(rg_material_height_hook((void *)(uintptr_t)0x10) == 64);
        } else {
            static unsigned char anonymous[0x18];
            memset(size_material, 0, sizeof(size_material)); size_getter_sets = anonymous;
            CHECK(rg_material_height_hook(size_material) == 64);
        }
        CHECK(!sh_resource_graph_size_history_ready());
        sh_resource_graph_size_history_begin();
        CHECK(!sh_resource_graph_size_history_ready());
    }
    size_getter_sets = NULL; size_install_fake = 0;
    g_resource_head = saved_head; g_ready = saved_ready;
    g_material_height = g_material_width = NULL; g_size_attempted = 0;
    sh_resource_graph_test_reset();
}
/* R-321 copy-owner wiring. Each hook opens its owner's bracket around the original with the native
 * arguments, retires an owner before its destructor, and the getter hook collects what it reads and hands
 * it to the module with the caller's return address and frame. */
static int owner_calls, owner_raise;
static void *owner_seen[4];
static int owner_seen_ints[2];
static void owner_set_material(void *sprite, const char *name, int width, int height)
{
    owner_calls++; owner_seen[0] = sprite; owner_seen[1] = (void *)name; owner_seen_ints[0] = width; owner_seen_ints[1] = height;
    CHECK(so_begins == 1 && !so_ends && so_kind == SH_SIZE_BRACKET_SPRITE && so_object == sprite);
    if (owner_raise) RaiseException(0xe0815001, 0, 0, NULL);
}
static void owner_script_set(void *self, void *object, void *value)
{ owner_calls++; owner_seen[0] = self; owner_seen[1] = object; owner_seen[2] = value; if (owner_raise) RaiseException(0xe0815002, 0, 0, NULL); }
static void *owner_script_get(void *self, void *out, void *object)
{ owner_calls++; owner_seen[0] = self; owner_seen[1] = out; owner_seen[2] = object; return out; }
static void owner_destroy(void *object)
{
    owner_calls++; owner_seen[0] = object;
    /* Retired before the native destructor body runs. */
    CHECK(so_sprite_gone == object || so_hotbox_gone == object || so_widget_gone == object);
}
static void owner_object(void *object)
{
    owner_calls++; owner_seen[0] = object;
    CHECK(so_begins == 1 && !so_ends && so_object == object);
    if (owner_raise) RaiseException(0xe0815003, 0, 0, NULL);
}
static unsigned int owner_font_read(void *resource, void *error_text)
{
    (void)error_text; owner_calls++;
    CHECK(so_begins == 1 && !so_ends && so_kind == SH_SIZE_BRACKET_FONT && so_object == resource && !strcmp(so_owner, "owned rg"));
    return 0;
}
static void owner_swf_destroy(void *swf) { owner_calls++; CHECK(so_movie_gone == swf); }
static void owner_credits(void *screen, void *data, void *item)
{
    owner_calls++; owner_seen[0] = screen; owner_seen[1] = data; owner_seen[2] = item;
    CHECK(so_begins == 1 && so_kind == SH_SIZE_BRACKET_CREDITS && so_object == item);
}
static void owner_reset(void)
{
    owner_calls = owner_raise = 0; memset(owner_seen, 0, sizeof(owner_seen));
    so_begins = so_ends = 0; so_end_completed = -1; so_kind = 0; so_object = NULL; so_frame = NULL;
    so_explicit_object = so_script_object = so_sprite_gone = so_hotbox_gone = so_widget_gone = so_movie_gone = NULL;
    so_explicit_completed = -1;
}
static int owner_raised(void (*call)(void))
{
    __try { call(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
static unsigned char owner_sprite[0x130], owner_script[0xc8], owner_out[0x10], owner_value[0x10];
static void raise_set_material(void) { rg_set_material_hook(owner_sprite, "m", -1, 4); }
static void raise_script_set(void) { rg_script_width_set_hook(owner_seen, owner_script, owner_value); }
static void raise_object(void) { rg_hotbox_build_hook(owner_sprite); }
/* The hotBox update bodies on both renderers (m130 census): only the saved-scale displacement differs. */
static const unsigned char hotbox_update_vk[] = {0x40,0x53,0x48,0x83,0xEC,0x20,0xF3,0x0F,0x10,0x81,0xF8,0x00,0x00,0x00,
    0x48,0x8B,0xD9,0x0F,0x2E,0x81,0xC0,0x0F,0x00,0x00,0x7A,0x31,0x75,0x2F,0xF3,0x0F,0x10,0x81,0xFC,0x00,0x00,0x00,
    0x0F,0x2E,0x81,0xC4,0x0F,0x00,0x00,0x7A,0x1D,0x75,0x1B,0xF3,0x0F,0x10,0x81,0x00,0x01,0x00,0x00,0x0F,0x2E,0x81,
    0xC8,0x0F,0x00,0x00,0x7A,0x09};
static void owner_hook_checks(void)
{
    char error[16] = {0};
    /* Setters and destructors. */
    owner_reset(); g_set_material = owner_set_material;
    rg_set_material_hook(owner_sprite, "materials/x", -1, 7);
    CHECK(owner_calls == 1 && owner_seen[0] == owner_sprite && !strcmp((const char *)owner_seen[1], "materials/x"));
    CHECK(owner_seen_ints[0] == -1 && owner_seen_ints[1] == 7 && so_width == -1 && so_height == 7 && so_frame != NULL);
    CHECK(so_ends == 1 && so_end_completed == 1);
    owner_reset(); owner_raise = 1;
    CHECK(owner_raised(raise_set_material) && so_ends == 1 && so_end_completed == 0);
    owner_reset(); g_script_material_set = owner_script_set;
    rg_script_material_set_hook(owner_seen, owner_script, owner_value);
    CHECK(so_kind == SH_SIZE_BRACKET_SCRIPT && so_object == owner_script && so_ends == 1 && so_end_completed == 1);
    owner_reset(); g_script_width_set = g_script_height_set = owner_script_set;
    rg_script_height_set_hook(owner_seen, owner_script, owner_value);
    CHECK(owner_calls == 1 && so_explicit_object == owner_script && so_explicit_axis == SH_SIZE_AXIS_HEIGHT &&
        so_explicit_completed == 1);
    owner_reset(); owner_raise = 1;
    CHECK(owner_raised(raise_script_set) && so_explicit_axis == SH_SIZE_AXIS_WIDTH && so_explicit_completed == 0);
    owner_reset(); g_script_width_get = g_script_height_get = owner_script_get;
    CHECK(rg_script_width_get_hook(owner_seen, owner_out, owner_script) == owner_out);
    CHECK(owner_seen[2] == owner_script && so_script_object == owner_script && so_script_axis == SH_SIZE_AXIS_WIDTH);
    owner_reset(); g_sprite_destroy = g_hotbox_destroy = g_widget_destroy = owner_destroy;
    rg_sprite_destroy_hook(owner_sprite); CHECK(so_sprite_gone == owner_sprite && owner_calls == 1);
    rg_hotbox_destroy_hook(owner_script); CHECK(so_hotbox_gone == owner_script && owner_calls == 2);
    rg_widget_destroy_hook(owner_out); CHECK(so_widget_gone == owner_out && owner_calls == 3);
    owner_reset(); g_hotbox_build = g_snap_codex_update = g_dossier_codex_update = owner_object;
    rg_hotbox_build_hook(owner_sprite); CHECK(so_kind == SH_SIZE_BRACKET_HOTBOX && so_end_completed == 1);
    owner_reset(); rg_snap_codex_update_hook(owner_script); CHECK(so_kind == SH_SIZE_BRACKET_WIDGET && so_end_completed == 1);
    owner_reset(); rg_dossier_codex_update_hook(owner_out); CHECK(so_kind == SH_SIZE_BRACKET_WIDGET && so_object == owner_out);
    owner_reset(); owner_raise = 1; CHECK(owner_raised(raise_object) && so_end_completed == 0);
    owner_reset(); g_credits_item = owner_credits;
    rg_credits_item_hook(owner_out, owner_value, owner_sprite);
    CHECK(owner_seen[0] == owner_out && owner_seen[1] == owner_value && owner_seen[2] == owner_sprite && so_ends == 1);

    /* hotBox layout decode: VK +0xfc0; GL's body differs only in the displacement (+0xf40); anything else
     * is refused. */
    unsigned char update[sizeof(hotbox_update_vk)];
    memcpy(update, hotbox_update_vk, sizeof(update));
    CHECK(rg_hotbox_saved_offset(update) == 0xfc0);
    update[0x14] = 0x40; update[0x15] = 0x0f; update[0x27] = 0x44; update[0x28] = 0x0f; update[0x3a] = 0x48; update[0x3b] = 0x0f;
    CHECK(rg_hotbox_saved_offset(update) == 0xf40);
    update[0x27] = 0x48; CHECK(!rg_hotbox_saved_offset(update));
    memcpy(update, hotbox_update_vk, sizeof(update)); update[0x12] = 0x2f; CHECK(!rg_hotbox_saved_offset(update));
    CHECK(!rg_hotbox_saved_offset(NULL));

    /* Install: every owner signature, the movie and font hooks, a known pinned renderer, the decoded layout
     * and the verified site table, then all hooks commit, or none stays. */
    static unsigned char targets[13][32];
    sig_result results[14];
    for (size_t i = 0; i < 13; i++) results[i] = (sig_result){g_owner_required[i], SIG_OK, (uintptr_t)targets[i], 0};
    results[13] = (sig_result){g_owner_required[13], SIG_OK, (uintptr_t)hotbox_update_vk, 0};
    LONG saved_movie = g_movie_ready;
    rg_read_fn saved_font = g_font_read;
    for (int failure = 0; failure < 9; failure++) {
        owner_install_fake = 1; owner_prepares = owner_commits = owner_removes = 0;
        owner_prepare_fault = failure == 7 ? 5 : 0; owner_commit_fault = failure == 8 ? 9 : 0;
        g_movie_ready = failure != 0;
        g_font_read = failure == 1 ? NULL : (rg_read_fn)owner_destroy;
        owner_renderer = failure == 2 ? -1 : 1; owner_pinned = failure != 3;
        results[4].status = failure == 4 ? SIG_NOT_FOUND : SIG_OK;
        results[13].addr = failure == 5 ? (uintptr_t)targets[0] : (uintptr_t)hotbox_update_vk;
        so_bind_result = failure != 6; so_hotbox_offset = 0;
        g_set_material = NULL; g_widget_destroy = NULL;
        CHECK(!rg_owners_install(results, 14, targets[0], targets[1]));
        CHECK(!so_hotbox_offset);
        /* A failure after hooks were prepared removes each of them again. */
        if (failure == 7) CHECK(owner_removes == 4 && !g_set_material);
        else if (failure == 8) CHECK(owner_removes == 13 && !g_set_material && !g_widget_destroy);
        else CHECK(!owner_prepares);
    }
    owner_prepares = owner_commits = owner_removes = 0; owner_prepare_fault = owner_commit_fault = 0;
    g_movie_ready = 1; g_font_read = (rg_read_fn)owner_destroy; owner_renderer = 0; owner_pinned = 1; so_bind_result = 1;
    results[4].status = SIG_OK; results[13].addr = (uintptr_t)hotbox_update_vk;
    CHECK(rg_owners_install(results, 14, targets[0], targets[1]));
    CHECK(owner_prepares == 13 && owner_commits == 13 && !owner_removes && so_hotbox_offset == 0xfc0);
    CHECK(g_set_material == (rg_set_material_fn)(void *)targets[0] && g_widget_destroy == (rg_object_fn)(void *)targets[12]);
    owner_install_fake = 0; owner_renderer = -1; owner_pinned = 0; so_bind_result = 1;
    g_movie_ready = saved_movie; g_font_read = saved_font;
    g_set_material = NULL; g_script_material_set = g_script_width_set = g_script_height_set = NULL;
    g_script_width_get = g_script_height_get = NULL;
    g_sprite_destroy = g_hotbox_build = g_hotbox_destroy = g_snap_codex_update = g_dossier_codex_update = g_widget_destroy = NULL;
    g_credits_item = NULL;
    /* FontRead: every read brackets its font resource as the owner of what it copies. */
    {
        static unsigned char font_object[0x60], font_manager[0x10];
        put_pointer(font_manager, 8, "font"); put_pointer(font_object, 8, "owned rg"); put_pointer(font_object, 0x10, font_manager);
        LONG saved_ready = g_ready;
        rg_read_fn saved_read = g_font_read;
        int saved_acquires = font_acquires;
        g_ready = 1; g_font_read = owner_font_read; owner_reset();
        CHECK(!rg_font_read_hook(font_object, error) && owner_calls == 1);
        CHECK(so_ends == 1 && so_end_completed == 1);
        g_ready = saved_ready; g_font_read = saved_read; font_acquires = saved_acquires;
    }
    /* SwfDestroy retires what the movie owns before its native destructor runs. */
    {
        static unsigned char swf_object[0x20];
        rg_swf_destroy_fn saved_destroy = g_swf_destroy;
        LONG saved_movie_ready = g_movie_ready;
        g_swf_destroy = owner_swf_destroy; g_movie_ready = 0; owner_reset();
        rg_swf_destroy_hook(swf_object);
        CHECK(owner_calls == 1 && so_movie_gone == swf_object);
        g_swf_destroy = saved_destroy; g_movie_ready = saved_movie_ready;
    }
    (void)error;
}
/* The getter hook: transient callers run the original with no collection; any other caller's reads (the
 * editor image, and finds inside the call) are handed over with its return address and frame, and kept with
 * the image history unless an owner took them. The measurement hands its reads to the refresh. */
static void *getter_find_result;
static int collecting_getter(void *material)
{
    size_getter_calls++;
    if (getter_find_result) rg_size_lookup(getter_find_result);
    if (size_getter_sets) put_pointer(material, 0x188, size_getter_sets);
    return 32;
}
static void getter_checks(void)
{
    sh_resource_graph_test_reset(); sh_resource_graph_size_history_begin(); size_registry(3);
    g_material_height = g_material_width = collecting_getter;
    memset(size_material, 0, sizeof(size_material));
    size_getter_sets = size_images[2]; getter_find_result = size_images[1]; size_getter_calls = 0; so_collect_ends = 0;
    so_site_class = SH_SIZE_SITE_TRANSIENT;
    CHECK(rg_material_width_hook(size_material) == 32 && size_getter_calls == 1 && !so_collect_ends);
    CHECK(!sh_resource_graph_history("image", "env/c") && !sh_resource_graph_history("image", "env/b"));
    so_site_class = SH_SIZE_SITE_SPRITE; so_attribute = 1;
    memset(size_material, 0, sizeof(size_material));
    CHECK(rg_material_width_hook(size_material) == 32 && so_collect_ends == 1 && so_collect_ret && so_collect_frame);
    CHECK(so_collected_count == 2 && !strcmp(so_collected[0], "env/b") && !strcmp(so_collected[1], "env/c"));
    CHECK(!sh_resource_graph_history("image", "env/c"));
    so_attribute = 0;
    memset(size_material, 0, sizeof(size_material));
    CHECK(rg_material_height_hook(size_material) == 32);
    CHECK(sh_resource_graph_history("image", "env/c") == SH_RESOURCE_GRAPH_HISTORY_ESCAPED &&
        sh_resource_graph_history("image", "env/b") == SH_RESOURCE_GRAPH_HISTORY_ESCAPED);
    /* The measurement. */
    char **names = NULL; size_t count = 0; int value = 0;
    memset(size_material, 0, sizeof(size_material)); getter_find_result = NULL;
    CHECK(rg_size_measure(size_material, SH_SIZE_AXIS_WIDTH, &value, &names, &count) && value == 32 && count == 1 &&
        !strcmp(names[0], "env/c"));
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    /* An unreadable editor-image field fails the measurement and invalidates the history. */
    size_getter_sets = NULL;
    CHECK(!rg_size_measure((void *)(uintptr_t)0x10, SH_SIZE_AXIS_WIDTH, &value, &names, &count) && !names);
    CHECK(!sh_resource_graph_size_history_ready());
    so_site_class = SH_SIZE_SITE_UNKNOWN; so_attribute = 0; size_getter_sets = NULL; getter_find_result = NULL;
    g_material_height = g_material_width = NULL;
    sh_resource_graph_test_reset();
}

static unsigned char instance_tools[0xa8], instance_reader[0x20], instance_object[64];
static void *instance_handlers[2], *instance_value;
static void *instance_class_handlers[2];
static unsigned char instance_registry[0x28], instance_class[0x38];
static void *instance_descriptor[9];
static int instance_read_mode, instance_list_count, instance_model_reload, instance_model_throw, instance_model_destroy_throw;
static size_t instance_model_offset;
static void *instance_model_constructor(void *model, void *backing)
{
    rg_instance_model_base_hook(model);
    memset((unsigned char *)model + 0xa8, 0x17, 24); /* actual completed copied bounds */
    if (instance_model_reload) {
        rg_instance_scope source = rg_instance_source_begin(backing, 0);
        CHECK(sh_resource_graph_native_source_observed("material", "a", instance_capture(9), 0));
        rg_instance_source_end(source, 1, 0);
    }
    if (instance_model_throw) RaiseException(0xe0000068u, 0, 0, NULL);
    put_pointer(model, instance_model_offset, backing); return model;
}
static void instance_model_destructor(void *model)
{
    (void)model;
    if (instance_model_destroy_throw) RaiseException(0xe0000069u, 0, 0, NULL);
}
static void *instance_resize(void *old, int old_count, int new_count, uintptr_t tag, unsigned char initialize)
{
    (void)tag; (void)initialize;
    void **data = new_count ? calloc((size_t)new_count, sizeof(void *)) : NULL;
    if (new_count && !data) return NULL;
    if (old_count && new_count) memcpy(data, old, (size_t)(old_count < new_count ? old_count : new_count) * sizeof(void *));
    free(old); return data;
}
static void *instance_resource_ctor(void *resource) { return resource; }
static void instance_resource_destroy(void *resource)
{ CHECK(!sh_resource_instances_lookup(resource)); }
static void *instance_resource_delete(void *resource, unsigned int flags)
{ CHECK(flags == 1 && !sh_resource_instances_lookup(resource)); return resource; }
static int instance_index(void *tools, const char *type) { (void)tools; (void)type; return 0; }
static uint32_t instance_type(void *tools, void *reader, uintptr_t a, const char *type,
    const char *ops, const char *member, uint32_t mode, uintptr_t context, void **slot, unsigned char allow)
{
    (void)tools; (void)reader; (void)a; (void)type; (void)ops; (void)member; (void)mode; (void)context; (void)allow;
    if (!strcmp(type, "idList")) {
        unsigned char *list = (unsigned char *)slot;
        if (instance_list_count > 0) {
            void *data = rg_instance_resize_hook(*(void **)list, *(int *)(list + 8), instance_list_count, 0, 1);
            put_pointer(list, 0, data);
        }
        memcpy(list + 8, &instance_list_count, sizeof(instance_list_count)); return 24;
    }
    if (instance_read_mode == 1) return 8;
    if (instance_read_mode == 4) { uint32_t encoded = 17; memcpy(slot, &encoded, 4); return 8; }
    if (instance_read_mode == 3) { *slot = NULL; return 8; }
    if (instance_read_mode == 6) { *slot = instance_value; return 8; } /* integer cache: no backing lookup */
    rg_instance_returned(instance_value); *slot = instance_value;
    if (instance_read_mode == 5) RaiseException(0xe0000067u, 0, 0, NULL);
    return instance_read_mode == 2 ? 1 : 8;
}
static void instance_field(uint64_t token)
{
    rg_instance_scope scope = rg_instance_begin(RG_INSTANCE_OWNER, instance_object, token);
    __try {
        rg_instance_type_hook(instance_tools, instance_reader, 0, "idDecl", "*", "material", 0, 0,
            (void **)(instance_object + 8), 1);
    } __finally { rg_instance_owner_end(scope); }
}
static void instance_model_state(void *type, void *object, void *reader, unsigned char initialize)
{
    (void)type; (void)reader; (void)initialize;
    rg_instance_type_hook(instance_tools, instance_reader, 0, "idRenderModel", "*", "model", 0, 0,
        (void **)((unsigned char *)object + 8), 1);
}
static void *instance_main_fiber;
static void CALLBACK instance_suspended_fiber(void *resource)
{
    rg_instance_begin(RG_INSTANCE_OWNER, instance_object, sh_resource_instances_token(instance_object));
    rg_instance_returned(resource);
    SwitchToFiber(instance_main_fiber);
}
typedef struct native_path_list { void *data; int count, capacity; uintptr_t padding; } native_path_list;
typedef struct native_path_row { uintptr_t padding; native_path_list inner; uintptr_t tail; } native_path_row;
typedef struct native_path_root { void **vtable; native_path_list outer; } native_path_root;
static unsigned char native_path_type[0x68];
static void *native_path_tools, *native_path_reader, *native_path_a, *native_path_b;
static int native_path_count, native_path_write, native_path_private;
static void *native_path_get_type(void *object) { (void)object; return native_path_type; }
static int native_path_index(void *tools,const char *type)
{ (void)tools; return !strcmp(type,"OuterList") ? 0 : !strcmp(type,"InnerList") ? 1 : 2; }
static int native_path_stride(void *tools,const char *type,const char *ops)
{
    (void)tools;
    if (!strcmp(ops,"*")) return 8;
    return !strcmp(type,"NestedRow") ? sizeof(native_path_row) : !strcmp(type,"LeafRow") ? 16 : -1;
}
static uint32_t native_path_read(void *tools,void *reader,uintptr_t a,const char *type,
    const char *ops,const char *member,uint32_t mode,uintptr_t context,void **slot,unsigned char allow)
{
    if (!strcmp(type,"OuterList")) {
        native_path_list *list = (native_path_list *)slot;
        native_path_row *rows = calloc(1,sizeof(*rows)); CHECK(rows);
        if (list->count) memcpy(rows,list->data,sizeof(*rows));
        free(list->data); list->data = rows; list->count = list->capacity = 1;
        rg_instance_type_hook(tools,reader,a,"InnerList","","inner",mode,context,(void **)&rows->inner,allow);
        return sizeof(*list);
    }
    if (!strcmp(type,"InnerList")) {
        native_path_list *list = (native_path_list *)slot;
        if (native_path_count) {
            void *moved = calloc((size_t)native_path_count,16); CHECK(moved);
            if (list->count) memcpy(moved,list->data,(size_t)(list->count < native_path_count ? list->count : native_path_count)*16);
            free(list->data); list->data = moved;
        }
        list->count = list->capacity = native_path_count;
        for (int i=0; native_path_write && i<list->count; i++) for (int j=0;j<2;j++) {
            instance_value = j ? native_path_b : native_path_a;
            if (native_path_private) instance_allowed = 0;
            rg_instance_type_hook(tools,reader,a,"idDecl","*",member,mode,context,
                (void **)((unsigned char *)list->data+(size_t)i*16+(size_t)j*8),allow);
            instance_allowed = 1;
        }
        return sizeof(*list);
    }
    return instance_type(tools,reader,a,type,ops,member,mode,context,slot,allow);
}
static void test_nested_native(void *a,void *b)
{
    unsigned char tools[0xa8]={0}, registry[0x28]={0}, classes[3*0x38]={0}, reader[0x20]={0};
    void *class_handlers[6]={0}, *pointer_handlers[6]={0}, *outer[9]={0}, *inner[9]={0};
    void *vtable[1]={native_path_get_type};
    native_path_root object={vtable,{0}}, copy={vtable,{0}};
    int size = sizeof(object); memcpy(native_path_type+0x64,&size,4);
    put_pointer(tools,0,registry); put_pointer(registry,0x20,classes);
    put_pointer(classes,0x18,outer); put_pointer(classes+0x38,0x18,inner);
    put_pointer(tools,0x88,class_handlers); put_pointer(tools,0xa0,pointer_handlers);
    outer[0]="NestedRow"; outer[1]=""; outer[8]=(void *)9;
    inner[0]="LeafRow"; inner[1]=""; inner[8]=(void *)10;
    g_instance_index=native_path_index; g_instance_stride=native_path_stride; g_instance_type=native_path_read;
    native_path_tools=tools; native_path_reader=reader; native_path_a=a; native_path_b=b;
    for(int renderer=0;renderer<2;renderer++) {
        owner_renderer=renderer;
        class_handlers[1]=class_handlers[3]=(void *)(sh_host_image_base()+(renderer?0x9cd610:0x9ccdd0));
        pointer_handlers[5]=(void *)(sh_host_image_base()+(renderer?0x9ca890:0x9ca050));
        uint64_t token=sh_resource_instances_construct(&object,1);
        native_path_count=2; native_path_write=1; native_path_private=0; instance_read_mode=0;
        rg_instance_scope owner=rg_instance_begin(RG_INSTANCE_OWNER,&object,token);
        rg_instance_type_hook(tools,reader,0,"OuterList","","outer",0,0,(void **)&object.outer,1);
        rg_instance_owner_end(owner);
        size_t resources,instances,fields; sh_resource_instances_counts(&resources,&instances,&fields);
        CHECK(fields==4 && instance_leases==2 && sh_resource_instances_complete(&object,token));
        /* Every resize here is a distinct arbitrary callback. Only actual
         * typed paths and leaf bytes justify ownership after both moves. */
        native_path_write=0;
        owner=rg_instance_begin(RG_INSTANCE_OWNER,&object,token);
        rg_instance_type_hook(tools,reader,0,"OuterList","","outer",0,0,(void **)&object.outer,1);
        rg_instance_owner_end(owner);
        CHECK(instance_leases==2 && sh_resource_instances_complete(&object,token));
        inner[0]="WrongLeaf";
        CHECK(sh_resource_instances_reconcile(&object,token,rg_instance_path_resolve,NULL));
        CHECK(instance_leases==2 && !sh_resource_instances_complete(&object,token));
        inner[0]="LeafRow";
        CHECK(sh_resource_instances_reconcile(&object,token,rg_instance_path_resolve,NULL));
        CHECK(sh_resource_instances_complete(&object,token));
        /* Private readers do not borrow established current origins. The
         * unchanged actual values preserve their earlier exact associations. */
        native_path_write=1; native_path_private=1;
        owner=rg_instance_begin(RG_INSTANCE_OWNER,&object,token);
        rg_instance_type_hook(tools,reader,0,"OuterList","","outer",0,0,(void **)&object.outer,1);
        rg_instance_owner_end(owner);
        CHECK(instance_leases==2 && sh_resource_instances_complete(&object,token));
        native_path_write=native_path_private=0;
        native_path_row *rows=object.outer.data, *copy_rows=malloc(sizeof(*rows)); memcpy(copy_rows,rows,sizeof(*rows));
        copy_rows->inner.data=malloc(32); memcpy(copy_rows->inner.data,rows->inner.data,32);
        copy.outer=object.outer; copy.outer.data=copy_rows;
        uint64_t cloned=sh_resource_instances_clone_paths_checked(&copy,&object,sizeof(object),
            rg_instance_clone_match,rg_instance_path_resolve,NULL);
        CHECK(cloned && sh_resource_instances_complete(&copy,cloned));
        sh_resource_instances_destroyed(&object,token,1); CHECK(instance_leases==2);
        free(rows->inner.data); free(rows); memset(&object.outer,0,sizeof(object.outer));
        native_path_count=0; owner=rg_instance_begin(RG_INSTANCE_OWNER,&copy,cloned);
        rg_instance_type_hook(tools,reader,0,"OuterList","","outer",0,0,(void **)&copy.outer,1);
        rg_instance_owner_end(owner);
        CHECK(instance_leases==0 && sh_resource_instances_complete(&copy,cloned));
        copy_rows=copy.outer.data; free(copy_rows->inner.data); free(copy_rows); memset(&copy.outer,0,sizeof(copy.outer));
        sh_resource_instances_destroyed(&copy,cloned,1);
    }
    g_instance_index=instance_index; g_instance_stride=NULL; g_instance_type=instance_type;
}
static void test_default_origin_native(void *a,void *b)
{
    unsigned char outer[0x60]={0},inner[0x60]={0},manager[0x10]={0},unknown[0x20]={0};
    void *table[0x100/8]={0}; table[0x50/8]=(void *)1;
    put_pointer(manager,8,"material");
    put_pointer(outer,0,table); put_pointer(outer,8,"origin-outer"); put_pointer(outer,0x10,manager);
    put_pointer(inner,0,table); put_pointer(inner,8,"origin-inner"); put_pointer(inner,0x10,manager);
    sh_resource_default_origin_scope one,two; sh_resource_graph_frame graph;
    sh_resource_graph_begin_requests(&graph,"material","origin-outer");
    CHECK(sh_resource_graph_native_default_origin_begin(&one,outer)); rg_instance_returned(a);
    CHECK(instance_leases==1);
    CHECK(sh_resource_graph_native_default_origin_begin(&two,inner)); rg_instance_returned(b);
    CHECK(sh_resource_graph_native_default_origin_finish(&two,inner,1,1,1));
    rg_instance_returned(inner);
    CHECK(sh_resource_graph_native_default_origin_finish(&one,outer,1,1,1));
    sh_resource_graph_end(&graph,1);
    CHECK(instance_leases==2); /* explicit cached default payload holds children */
    sh_resource_instance_origin *origin=sh_resource_instances_lookup(outer); CHECK(origin);
    unsigned char alias[24]={0}; uint64_t token=sh_resource_instances_construct(alias,1);
    CHECK(sh_resource_instances_assign(alias,token,(uintptr_t)alias+8,outer,origin));
    sh_resource_instances_origin_release(origin);
    rg_instance_scope source=rg_instance_source_begin(outer,0);
    CHECK(sh_resource_graph_native_source_observed("material","origin-outer",NULL,1));
    rg_instance_source_end(source,1,0);
    CHECK(instance_leases==2); /* alias still owns the exact earlier composite */
    sh_resource_instances_destroyed(alias,token,1); CHECK(instance_leases==1); /* inner payload B */
    sh_resource_instances_destroyed(inner,sh_resource_instances_token(inner),1); sh_resource_instances_forget(inner);
    CHECK(instance_leases==0);
    /* Failed partial default retains every actually consumed child until a
     * later qualified complete reset, not merely until the scope disappears. */
    CHECK(sh_resource_graph_native_default_origin_begin(&one,outer)); rg_instance_returned(a);
    CHECK(!sh_resource_graph_native_default_origin_finish(&one,outer,0,1,1));
    CHECK(instance_leases==1 && !sh_resource_instances_lookup(outer));
    CHECK(sh_resource_graph_native_default_origin_begin(&one,outer));
    CHECK(sh_resource_graph_native_default_origin_finish(&one,outer,1,1,1));
    CHECK(instance_leases==0);
    origin=sh_resource_instances_lookup(outer); CHECK(origin); sh_resource_instances_origin_release(origin);
    CHECK(sh_resource_graph_native_default_origin_begin(&one,outer)); rg_instance_returned(unknown);
    CHECK(!sh_resource_graph_native_default_origin_finish(&one,outer,1,1,1));
    CHECK(!sh_resource_instances_lookup(outer));
    instance_allowed=0; CHECK(!sh_resource_graph_native_default_origin_begin(&one,outer)); instance_allowed=1;
    CHECK(instance_leases==0);
    /* A cached native child supplies its exact borrowed origin. Forgetting
     * its resource association cannot redirect the observation to another
     * provider or make the synchronous borrowed receipt lose its live share. */
    origin=sh_resource_instances_lookup(a); CHECK(origin);
    CHECK(sh_resource_graph_native_default_origin_begin(&one,outer));
    sh_resource_instances_forget(a);
    CHECK(sh_resource_graph_native_observe_origin(origin));
    CHECK(sh_resource_graph_native_observe_origin(origin)); CHECK(instance_leases==1);
    CHECK(sh_resource_graph_native_default_origin_finish(&one,outer,1,1,1));
    CHECK(sh_resource_instances_publish(a,origin)); sh_resource_instances_origin_release(origin);
    CHECK(instance_leases==1);
    CHECK(sh_resource_graph_native_default_origin_begin(&one,outer));
    CHECK(!sh_resource_graph_native_observe_origin(NULL));
    CHECK(!sh_resource_graph_native_default_origin_finish(&one,outer,1,1,1));
    CHECK(instance_leases==1); /* failed reset keeps the earlier payload */
    CHECK(sh_resource_graph_native_default_origin_begin(&one,outer));
    CHECK(sh_resource_graph_native_default_origin_finish(&one,outer,1,1,1)); CHECK(instance_leases==0);
    sh_resource_instances_destroyed(outer,sh_resource_instances_token(outer),1); sh_resource_instances_forget(outer);
    sh_resource_graph_test_reset();
}
static int presentation_visit(void *context,const char *pt,const char *pn,const char *type,const char *name)
{
    (void)pt; (void)pn; (void)type; unsigned *bits=context;
    if(!strcmp(name,"a")) *bits|=1;
    if(!strcmp(name,"b")) *bits|=2;
    return 1;
}
typedef struct presentation_inputs { const void *a,*b; unsigned bits; } presentation_inputs;
static int presentation_capture_one(void *context,const void *value)
{ CHECK(!*(const void **)context); *(const void **)context=value; return 1; }
static int presentation_captures(void *context,const void *value)
{
    presentation_inputs *inputs=context;
    CHECK(value==inputs->a || value==inputs->b);
    inputs->bits|=value==inputs->a?1:2; return 1;
}
static void test_presentation_native(void *a,void *b)
{
    unsigned char outer[0x60]={0},inner[0x60]={0},owner_object[24]={0},manager[16]={0};
    void *table[0x100/8]={0}; table[0x50/8]=(void *)1;
    put_pointer(manager,8,"material");
    put_pointer(outer,0,table); put_pointer(outer,8,"presentation-outer"); put_pointer(outer,0x10,manager);
    put_pointer(inner,0,table); put_pointer(inner,8,"presentation-inner"); put_pointer(inner,0x10,manager);
    sh_resource_graph_frame graph,child,leaf;
    sh_resource_default_origin_scope def,nested;
    sh_resource_presentation_scope one,two;
    presentation_inputs inputs={0};
    sh_resource_instance_origin *input_a=sh_resource_instances_lookup(a),*input_b=sh_resource_instances_lookup(b);
    CHECK(input_a && input_b);
    CHECK(sh_resource_instances_origin_captures(input_a,presentation_capture_one,&inputs.a));
    CHECK(sh_resource_instances_origin_captures(input_b,presentation_capture_one,&inputs.b));
    CHECK(inputs.a && inputs.b && inputs.a!=inputs.b);
    sh_resource_graph_begin(&leaf,"material","a"); sh_resource_graph_end(&leaf,1);
    sh_resource_graph_begin(&leaf,"material","b"); sh_resource_graph_end(&leaf,1);
    sh_resource_graph_begin_requests(&graph,"material","presentation-outer");
    CHECK(sh_resource_graph_native_default_origin_begin(&def,outer));
    CHECK(sh_resource_graph_native_observe_resource(a)); CHECK(instance_leases==1);
    CHECK(sh_resource_graph_native_presentation_begin(&one)); CHECK(!sh_resource_graph_recording());
    CHECK(!sh_resource_graph_native_observing());
    CHECK(sh_resource_graph_native_observe_resource(b));
    CHECK(!sh_resource_graph_native_observe_origin(NULL));
    CHECK(sh_resource_graph_native_presentation_begin(&two));
    CHECK(sh_resource_graph_native_observe_resource(b));
    sh_resource_graph_native_presentation_end(&two); CHECK(!sh_resource_graph_recording());
    /* An independently parsed renderer input records its own graph and
     * owns its actual field. Unknown drawing reads cannot poison the caller. */
    sh_resource_graph_begin_requests(&child,"material","presentation-inner");
    CHECK(sh_resource_graph_native_default_origin_begin(&nested,inner));
    CHECK(sh_resource_graph_native_observe_resource(b));
    CHECK(!sh_resource_graph_native_observe_origin(NULL));
    CHECK(!sh_resource_graph_native_default_origin_finish(&nested,inner,1,1,1));
    CHECK(sh_resource_graph_native_default_origin_begin(&nested,inner));
    CHECK(sh_resource_graph_native_observe_resource(b));
    CHECK(sh_resource_graph_native_default_origin_finish(&nested,inner,1,1,1));
    sh_resource_graph_end(&child,0); /* its own failed first observation stays incomplete */
    uint64_t owner_token=sh_resource_instances_construct(owner_object,1);
    rg_instance_scope owner=rg_instance_begin(RG_INSTANCE_OWNER,owner_object,owner_token);
    CHECK(sh_resource_graph_native_observing());
    rg_instance_scope field=rg_instance_begin(RG_INSTANCE_FIELD,owner_object,owner_token);
    rg_instance_returned(b); put_pointer(owner_object,8,b);
    rg_instance_field_end(field,(void **)(owner_object+8),NULL,1); rg_instance_owner_end(owner);
    CHECK(sh_resource_instances_complete(owner_object,owner_token) && instance_leases==2);
    sh_resource_graph_native_presentation_end(&one); CHECK(sh_resource_graph_recording_requests());
    CHECK(sh_resource_graph_native_default_origin_finish(&def,outer,1,1,1));
    sh_resource_graph_end(&graph,1);
    unsigned bits=0;
    CHECK(sh_resource_graph_walk("material","presentation-outer",presentation_visit,&bits) && bits==1);
    bits=0; CHECK(!sh_resource_graph_walk("material","presentation-inner",presentation_visit,&bits) && bits==2);
    sh_resource_instance_origin *origin=sh_resource_instances_lookup(outer); CHECK(origin);
    CHECK(sh_resource_instances_origin_captures(origin,presentation_captures,&inputs) && inputs.bits==1);
    sh_resource_instances_origin_release(origin);
    sh_resource_instances_destroyed(outer,sh_resource_instances_token(outer),1); sh_resource_instances_forget(outer);
    sh_resource_instances_destroyed(inner,sh_resource_instances_token(inner),1); sh_resource_instances_forget(inner);
    CHECK(instance_leases==1); /* independent renderer field survived both defaults */
    sh_resource_instances_destroyed(owner_object,owner_token,1); CHECK(!instance_leases);
    /* The source selector sees no outer opening through presentation, but
     * a real nested source keeps its own positive identity and provenance. */
    char family[128],name[4096];
    rg_instance_scope source=rg_instance_source_begin(a,0);
    CHECK(sh_resource_graph_native_source_target(family,name) && !strcmp(name,"a"));
    CHECK(!sh_resource_graph_native_observing());
    CHECK(sh_resource_graph_native_presentation_begin(&one));
    CHECK(!sh_resource_graph_native_source_target(family,name));
    CHECK(!sh_resource_graph_native_source_observed("material","a",NULL,1));
    rg_instance_scope inner_source=rg_instance_source_begin(b,0);
    CHECK(sh_resource_graph_native_source_target(family,name) && !strcmp(name,"b"));
    rg_instance_source_end(inner_source,1,0);
    sh_resource_graph_native_presentation_end(&one);
    CHECK(sh_resource_graph_native_source_target(family,name) && !strcmp(name,"a"));
    rg_instance_source_end(source,1,0);
    /* A lost half of the barrier cannot certify the surrounding default. */
    sh_resource_graph_begin_requests(&graph,"material","presentation-outer");
    CHECK(sh_resource_graph_native_default_origin_begin(&def,outer));
    g_instance_allocation_fault=1;
    CHECK(!sh_resource_graph_native_presentation_begin(&one)); g_instance_allocation_fault=0;
    sh_resource_graph_native_presentation_end(&one);
    CHECK(!sh_resource_graph_native_default_origin_finish(&def,outer,1,1,1));
    sh_resource_graph_end(&graph,1);
    bits=0; CHECK(!sh_resource_graph_walk("material","presentation-outer",presentation_visit,&bits));
    /* Out-of-order nested end retires both owned barriers, marks the real
     * caller incomplete, and never leaves a borrowed stack record active. */
    sh_resource_graph_begin_requests(&graph,"material","presentation-outer");
    CHECK(sh_resource_graph_native_default_origin_begin(&def,outer));
    CHECK(sh_resource_graph_native_presentation_begin(&one));
    CHECK(sh_resource_graph_native_presentation_begin(&two));
    sh_resource_graph_native_presentation_end(&one);
    sh_resource_graph_native_presentation_end(&two);
    CHECK(!sh_resource_graph_native_default_origin_finish(&def,outer,1,1,1));
    sh_resource_graph_end(&graph,1); CHECK(!sh_resource_graph_recording());
    sh_resource_instances_destroyed(outer,sh_resource_instances_token(outer),1); sh_resource_instances_forget(outer);
    CHECK(!instance_leases); sh_resource_instances_origin_release(input_a); sh_resource_instances_origin_release(input_b);
    sh_resource_graph_test_reset();
}
static int derived_contains(void *context,void *resource) { return resource==context; }
static void test_derived_origin_native(void *a,void *b)
{
    unsigned char backing[0x80]={0},manager[16]={0};
    unsigned char *model=calloc(1,0xf20); CHECK(model); if(!model) return;
    put_pointer(backing,8,"derived-baseline"); put_pointer(backing,0x10,manager); put_pointer(manager,8,"model");
    sh_resource_instance_origin *baseline=sh_resource_instances_origin(NULL,1);
    sh_resource_instance_origin *borrowed=sh_resource_instances_lookup(b); CHECK(borrowed);
    CHECK(sh_resource_instances_publish(backing,baseline));
    g_instance_model_base=instance_resource_ctor; g_instance_model_destroy=instance_model_destructor;
    CHECK(!sh_resource_graph_native_observing());
    rg_instance_scope scope=rg_instance_model_begin(model,backing);
    CHECK(sh_resource_graph_native_observing()); rg_instance_model_base_hook(model);
    put_pointer(model,0xe90,backing); memset(model+0xa8,0x43,24);
    rg_instance_returned(a); CHECK(!instance_leases); /* generic lookup is not an actual child read */
    CHECK(sh_resource_graph_native_observe_resource(a)); CHECK(instance_leases==1);
    CHECK(sh_resource_graph_native_observe_origin(borrowed)); CHECK(instance_leases==2);
    CHECK(sh_resource_graph_native_observe_resource(a)); CHECK(instance_leases==2);
    sh_resource_presentation_scope display;
    CHECK(sh_resource_graph_native_presentation_begin(&display));
    CHECK(!sh_resource_graph_native_observing()); CHECK(!sh_resource_graph_native_observe_origin(NULL));
    sh_resource_graph_native_presentation_end(&display); CHECK(sh_resource_graph_native_observing());
    rg_instance_model_end(scope,model,backing,0xe90,1);
    uint64_t token=sh_resource_instances_token(model); CHECK(token && instance_leases==2);
    CHECK(!sh_resource_graph_native_observing());
    CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,a));
    CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,backing));
    /* Borrowed file/cached origins carry their own exact generation even
     * without a resource-name association; later weak forget is irrelevant. */
    sh_resource_instances_forget(b); CHECK(instance_leases==2);
    CHECK(sh_resource_instances_publish(b,borrowed));
    sh_resource_instance_origin *model_origin=sh_resource_instances_lookup(model); CHECK(model_origin);
    presentation_inputs inputs={0};
    sh_resource_instance_origin *a_origin=sh_resource_instances_lookup(a); CHECK(a_origin);
    CHECK(sh_resource_instances_origin_captures(a_origin,presentation_capture_one,&inputs.a));
    CHECK(sh_resource_instances_origin_captures(borrowed,presentation_capture_one,&inputs.b));
    CHECK(sh_resource_instances_origin_captures(model_origin,presentation_captures,&inputs) && inputs.bits==3);
    sh_resource_instances_origin_release(model_origin); sh_resource_instances_origin_release(a_origin);
    instance_model_destroy_throw=1;
    __try { rg_instance_model_destroy_hook(model); CHECK(0); }
    __except(GetExceptionCode()==0xe0000069u?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { }
    CHECK(instance_leases==2);
    instance_model_destroy_throw=0; rg_instance_model_destroy_hook(model); CHECK(!instance_leases);
    /* Actual read-record loss holds the child against this model lifetime;
     * completion cannot publish a partial constructor composite. */
    scope=rg_instance_model_begin(model,backing); rg_instance_model_base_hook(model);
    put_pointer(model,0xe90,backing); g_instance_allocation_fault=1;
    CHECK(sh_resource_graph_native_observe_resource(a)); g_instance_allocation_fault=0;
    rg_instance_model_end(scope,model,backing,0xe90,1);
    token=sh_resource_instances_token(model);
    CHECK(sh_resource_instances_faulted(model,token) && instance_leases==1 && !sh_resource_instances_lookup(model));
    rg_instance_model_destroy_hook(model); CHECK(!instance_leases);
    unsigned char field_owner[24]={0}; uint64_t field_token=sh_resource_instances_construct(field_owner,1);
    rg_instance_scope owner=rg_instance_begin(RG_INSTANCE_OWNER,field_owner,field_token);
    rg_instance_scope field=rg_instance_begin(RG_INSTANCE_FIELD,field_owner,field_token);
    rg_instance_returned(a); CHECK(sh_resource_graph_native_observe_origin(borrowed)); put_pointer(field_owner,8,a);
    rg_instance_field_end(field,(void **)(field_owner+8),NULL,1); rg_instance_owner_end(owner);
    CHECK(instance_leases==2 && sh_resource_instances_complete(field_owner,field_token));
    CHECK(sh_resource_instances_assign(field_owner,field_token,(uintptr_t)field_owner+8,NULL,NULL));
    CHECK(!instance_leases); sh_resource_instances_destroyed(field_owner,field_token,1);
    sh_resource_instances_destroyed(backing,sh_resource_instances_token(backing),1); sh_resource_instances_forget(backing);
    sh_resource_instances_origin_release(baseline); sh_resource_instances_origin_release(borrowed); free(model);
}
static int tree_mode,tree_calls;
static void *tree_skeleton;
static sh_resource_instance_origin *tree_next;
static void *tree_constructor(void *model,void *backing)
{
    tree_calls++; CHECK(instance_leases==(tree_mode==2?0:tree_mode==4?1:2));
    rg_instance_model_base_hook(model); put_pointer(model,0xe90,backing); memset((unsigned char *)model+0xa8,0x39,24);
    if(tree_mode==1) CHECK(sh_resource_instances_publish(tree_skeleton,tree_next));
    if(tree_mode==3) RaiseException(0xe1320031,0,0,NULL);
    return model;
}
static void test_tree_inputs(void)
{
    unsigned char decl[0x80]={0},base[0x160]={0},skeleton[0x60]={0},managers[3][16]={{0}};
    unsigned char *model=calloc(1,0xf20); CHECK(model); if(!model) return;
    put_pointer(decl,8,"tree-source"); put_pointer(decl,0x10,managers[0]); put_pointer(managers[0],8,"md6def");
    put_pointer(base,8,"tree-base"); put_pointer(base,0x10,managers[1]); put_pointer(managers[1],8,"basemodel");
    put_pointer(skeleton,8,"tree-skeleton"); put_pointer(skeleton,0x10,managers[2]); put_pointer(managers[2],8,"skeleton");
    put_pointer(decl,0x60,base);
    sh_resource_instance_origin *baseline=sh_resource_instances_origin(NULL,1);
    sh_resource_instance_origin *base_origin=sh_resource_instances_origin(instance_capture(201),0);
    sh_resource_instance_origin *skeleton_origin=sh_resource_instances_origin(instance_capture(202),0);
    tree_next=sh_resource_instances_origin(instance_capture(203),0); tree_skeleton=skeleton;
    g_instance_model_tree=tree_constructor; g_instance_model_base=instance_resource_ctor; g_instance_model_destroy=instance_model_destructor;
    tree_calls=0;
    for(owner_renderer=0;owner_renderer<2;owner_renderer++) for(tree_mode=0;tree_mode<5;tree_mode++) {
        CHECK(sh_resource_instances_publish(decl,baseline) && sh_resource_instances_publish(base,base_origin) &&
            sh_resource_instances_publish(skeleton,skeleton_origin));
        put_pointer(base,0x138,tree_mode==4?NULL:skeleton);
        if(tree_mode==2) instance_allowed=0;
        int caught=0;
        __try { CHECK(rg_instance_model_tree_hook(model,decl)==model); }
        __except(GetExceptionCode()==0xe1320031?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
        instance_allowed=1; CHECK(caught==(tree_mode==3));
        uint64_t token=sh_resource_instances_token(model); CHECK(token);
        sh_resource_instance_origin *origin=sh_resource_instances_lookup(model);
        if(tree_mode==0) {
            CHECK(origin && instance_leases==2);
            CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,base));
            CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,skeleton));
        } else CHECK(!origin && instance_leases==(tree_mode==2?0:tree_mode==1?3:tree_mode==4?1:2));
        sh_resource_instances_origin_release(origin); rg_instance_model_destroy_hook(model); CHECK(!instance_leases);
        sh_resource_instances_destroyed(decl,sh_resource_instances_token(decl),1);
        sh_resource_instances_destroyed(base,sh_resource_instances_token(base),1);
        sh_resource_instances_destroyed(skeleton,sh_resource_instances_token(skeleton),1);
        sh_resource_instances_forget(decl); sh_resource_instances_forget(base); sh_resource_instances_forget(skeleton);
    }
    CHECK(tree_calls==10);
    sh_resource_instances_origin_release(baseline); sh_resource_instances_origin_release(base_origin);
    sh_resource_instances_origin_release(skeleton_origin); sh_resource_instances_origin_release(tree_next); tree_next=NULL; free(model);
}
static int cloth_ctor_mode,cloth_ctor_calls;
static void *cloth_constructor(void *model,void *backing)
{
    cloth_ctor_calls++; rg_instance_model_base_hook(model);
    memset((unsigned char *)model+0xa8,0x27,24); /* owned base defaults, no source copy */
    if(cloth_ctor_mode==1) RaiseException(0xe0137110u,0,0,NULL);
    if(cloth_ctor_mode==4) {
        sh_resource_instances_forget(backing); CHECK(sh_resource_instances_construct(backing,0));
    }
    put_pointer(model,owner_renderer?0x10e0:0x1060,cloth_ctor_mode==2?NULL:backing);
    return model;
}
static void test_cloth_instance_origins(void)
{
    unsigned char backing[0x80]={0},manager[0x10]={0},model[0x1130]={0};
    put_pointer(backing,8,"cloth/actual"); put_pointer(backing,0x10,manager); put_pointer(manager,8,"cloth");
    g_instance_model_cloth=cloth_constructor; g_instance_model_base=instance_resource_ctor;
    g_instance_model_destroy=instance_model_destructor; cloth_ctor_calls=0;
    for(int renderer=0;renderer<2;renderer++) for(int mode=0;mode<5;mode++) {
        owner_renderer=renderer; cloth_ctor_mode=mode==3?0:mode;
        sh_resource_instance_origin *origin=sh_resource_instances_origin(instance_capture(90+renderer*4+mode),0);
        CHECK(origin && sh_resource_instances_publish(backing,origin));
        if(mode==3) { CHECK(sh_resource_instances_origin_hold(origin)); instance_allowed=0; }
        if(mode==1) {
            __try { rg_instance_model_cloth_hook(model,backing); CHECK(0); }
            __except(GetExceptionCode()==0xe0137110u?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { }
        } else CHECK(rg_instance_model_cloth_hook(model,backing)==model);
        instance_allowed=1;
        uint64_t token=sh_resource_instances_token(model); CHECK(token);
        sh_resource_instance_origin *bounds=sh_resource_instances_field_origin(model,token,(uintptr_t)model+0xa8);
        CHECK(!bounds); sh_resource_instances_origin_release(bounds);
        sh_resource_instance_origin *field=sh_resource_instances_field_origin(model,token,(uintptr_t)model+(renderer?0x10e0:0x1060));
        sh_resource_instance_origin *published=sh_resource_instances_lookup(model);
        if(!mode) { CHECK(field==origin); CHECK(published && !sh_resource_instances_faulted(model,token)); }
        else CHECK(!published);
        /* The ctor proves this backing store. Deferred Cloth readers are not
         * a whole-instance completeness certificate. */
        CHECK(!sh_resource_instances_complete(model,token));
        sh_resource_instances_origin_release(published);
        sh_resource_instances_origin_release(field);
        sh_resource_instances_forget(backing);
        CHECK(instance_leases==1); /* weak source removal does not retire actual model */
        if(!mode) {
            instance_model_destroy_throw=1;
            __try { rg_instance_model_destroy_hook(model); CHECK(0); }
            __except(GetExceptionCode()==0xe0000069u?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { }
            CHECK(instance_leases==1 && sh_resource_instances_token(model)==token);
            instance_model_destroy_throw=0;
            /* Explicit fixture completion, not replay of a partial destructor. */
            sh_resource_instances_destroyed(model,token,1);
        } else rg_instance_model_destroy_hook(model);
        if(mode==3) sh_resource_instances_origin_unhold(origin);
        CHECK(!instance_leases && !sh_resource_instances_lookup(model));
        sh_resource_instances_origin_release(origin);
    }
    CHECK(cloth_ctor_calls==10);
    sh_resource_instances_destroyed(backing,sh_resource_instances_token(backing),1);
}
static int discrete_mode,discrete_ctor_calls,discrete_copy_calls;
static unsigned char *discrete_data,*discrete_decl,*discrete_material,*discrete_override;
static unsigned char discrete_runtime_rows[0x60],discrete_runtime_matrices[0x30],discrete_runtime_mask[1];
static sh_resource_instance_origin *discrete_next,*discrete_recycled;
static void discrete_copy(void *model)
{
    discrete_copy_calls++;
    if(discrete_mode==3) RaiseException(0xe1320043u,0,0,NULL);
    if(discrete_mode==8) {
        rg_instance_model_destroy_hook(model); rg_instance_model_base_hook(model);
        CHECK(sh_resource_instances_publish(model,discrete_recycled)); return;
    }
    if(discrete_mode==4) CHECK(sh_resource_instances_publish(discrete_material,discrete_next));
    size_t matrix_offset=owner_renderer?0xf88:0xf28,mask_offset=owner_renderer?0xfa0:0xf40;
    if(discrete_mode==12) return; /* successful native zero-count loops */
    put_pointer(model,matrix_offset,discrete_runtime_matrices);
    put_pointer(model,mask_offset,discrete_runtime_mask);
    *(int *)((unsigned char *)model+matrix_offset+8)=*(int *)((unsigned char *)model+matrix_offset+12)=1;
    *(int *)((unsigned char *)model+mask_offset+8)=*(int *)((unsigned char *)model+mask_offset+12)=1;
    put_pointer(model,0xae0,discrete_runtime_rows);
    *(int *)((unsigned char *)model+0xae8)=*(int *)((unsigned char *)model+0xaec)=1;
    void *source=*(void **)(*(unsigned char **)(discrete_data+0x80));
    put_pointer(discrete_runtime_rows,0,discrete_mode==1?discrete_override:discrete_mode==13?NULL:discrete_material);
    put_pointer(discrete_runtime_rows,0x18,*(void **)((unsigned char *)source+8));
    memcpy((unsigned char *)model+0xa8,*(void **)((unsigned char *)source+8),24);
    if(discrete_mode==9) put_pointer(discrete_runtime_rows,0,NULL);
}
static void *discrete_constructor(void *model,const char *name,void *decl)
{
    CHECK(!strcmp(name,"actual-data-name") && decl==discrete_decl); discrete_ctor_calls++;
    rg_instance_model_base_hook(model);
    put_pointer(model,0,(void *)(sh_host_image_base()+(owner_renderer?0x25461b0:0x255c420)));
    put_pointer(model,0xe98,decl); put_pointer(model,0xe90,discrete_data);
    if(discrete_mode==1) put_pointer(model,0x118,discrete_override);
    if(discrete_mode==10) g_instance_allocation_fault=1;
    rg_instance_discrete_build_body(model,sh_host_image_base()+(owner_renderer?0x145b1d8:0x144efc8)+(discrete_mode==7));
    g_instance_allocation_fault=0;
    if(discrete_mode==11) RaiseException(0xe132004bu,0,0,NULL);
    return model;
}
static void test_discrete_instance_origins(void)
{
    unsigned char data[0xe0]={0},decl[0x1c0]={0},material[0x1a0]={0},override_material[0x1a0]={0};
    unsigned char managers[4][16]={{0}},source[24]={0},matrices[0x30]={0},mask[1]={1};
    float geometry[6]={-1,-2,-3,4,5,6}; void *surfaces[]={source}; unsigned char model[0x1000];
    void *resources[]={data,decl,material,override_material};
    const char *families[]={"discreteanimation","breakable","material","material"};
    for(int i=0;i<4;i++) {
        put_pointer(resources[i],8,i==0?"data/actual":i==1?"breakable/actual":i==2?"material/actual":"material/override");
        put_pointer(resources[i],0x10,managers[i]); put_pointer(managers[i],8,(void *)families[i]);
    }
    put_pointer(data,0x68,matrices); *(int *)(data+0x70)=*(int *)(data+0x74)=1;
    put_pointer(data,0x80,surfaces); *(int *)(data+0x88)=*(int *)(data+0x8c)=1;
    put_pointer(decl,0x1a0,mask); *(int *)(decl+0x1a8)=*(int *)(decl+0x1ac)=1;
    *(int *)(source+4)=1; put_pointer(source,8,geometry); put_pointer(source,0x10,material);
    discrete_data=data; discrete_decl=decl; discrete_material=material; discrete_override=override_material;
    g_instance_model_discrete=discrete_constructor; g_instance_discrete_build=discrete_copy;
    g_instance_model_base=instance_resource_ctor; g_instance_model_destroy=instance_model_destructor;
    discrete_ctor_calls=discrete_copy_calls=0;
    for(int renderer=0;renderer<2;renderer++) for(int mode=0;mode<14;mode++) {
        owner_renderer=renderer; discrete_mode=mode; memset(model,0,sizeof(model));
        sh_resource_instance_origin *origins[4]={0};
        for(int i=0;i<4;i++) {
            origins[i]=sh_resource_instances_origin(instance_capture(300+mode*4+i),0); CHECK(origins[i]);
            if(mode!=2 || i!=2) CHECK(sh_resource_instances_publish(resources[i],origins[i]));
        }
        discrete_next=sh_resource_instances_origin(instance_capture(400+mode),0);
        discrete_recycled=sh_resource_instances_origin(NULL,1);
        if(mode==5) { for(int i=0;i<3;i++) CHECK(sh_resource_instances_origin_hold(origins[i])); instance_allowed=0; }
        *(int *)(data+0x88)=mode==6?-1:1;
        *(int *)(data+0x70)=*(int *)(decl+0x1a8)=mode==12?0:1;
        if(mode==12) *(int *)(data+0x88)=0;
        put_pointer(source,0x10,mode==13?NULL:material);
        int caught=0;
        __try { CHECK(rg_instance_model_discrete_hook(model,"actual-data-name",decl)==model); }
        __except((GetExceptionCode()==0xe1320043u || GetExceptionCode()==0xe132004bu)?
            EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
        g_instance_allocation_fault=0; instance_allowed=1;
        CHECK(caught==(mode==3 || mode==11));
        uint64_t token=sh_resource_instances_token(model); CHECK(token);
        sh_resource_instance_origin *published=sh_resource_instances_lookup(model);
        sh_resource_instance_origin *actual=sh_resource_instances_field_origin(model,token,(uintptr_t)model+0xe90);
        sh_resource_instance_origin *bounds=sh_resource_instances_field_origin(model,token,(uintptr_t)model+0xa8);
        if(mode==0 || mode==1 || mode==12 || mode==13) {
            CHECK(published && actual==origins[0] && bounds==(mode==12?NULL:origins[0]));
            CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,data));
            CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,decl));
            if(mode<12) CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,material));
            if(mode==1) CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,override_material));
            CHECK(instance_leases==(mode==1?4:mode>=12?2:3));
        } else if(mode==8) CHECK(published==discrete_recycled); /* stale outer scope must not erase it */
        else CHECK(!published);
        CHECK(!sh_resource_instances_complete(model,token)); /* deferred method/GPU closure is separate */
        sh_resource_instances_origin_release(published); sh_resource_instances_origin_release(actual);
        sh_resource_instances_origin_release(bounds);
        for(int i=0;i<4;i++) sh_resource_instances_forget(resources[i]);
        if(mode==0) {
            instance_model_destroy_throw=1;
            __try { rg_instance_model_destroy_hook(model); CHECK(0); }
            __except(GetExceptionCode()==0xe0000069u?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { }
            CHECK(instance_leases==3); instance_model_destroy_throw=0;
            sh_resource_instances_destroyed(model,token,1); /* fixture's positive completed boundary */
        } else rg_instance_model_destroy_hook(model);
        if(mode==5) for(int i=0;i<3;i++) sh_resource_instances_origin_unhold(origins[i]);
        CHECK(!instance_leases);
        for(int i=0;i<4;i++) {
            sh_resource_instances_destroyed(resources[i],sh_resource_instances_token(resources[i]),1);
            sh_resource_instances_origin_release(origins[i]);
        }
        sh_resource_instances_origin_release(discrete_next); sh_resource_instances_origin_release(discrete_recycled);
    }
    CHECK(discrete_ctor_calls==28 && discrete_copy_calls==28);
}

static int cloth_use_mode,cloth_init_calls,cloth_render_calls,cloth_worker_sync,cloth_worker_mode,cloth_worker_calls;
static void *cloth_worker_model;
static unsigned char cloth_simulation[0x1f8],cloth_rows[0x60],cloth_geometry[0x120];
static sh_resource_instance_origin *cloth_changed;
static void *cloth_use_decl,*cloth_use_material;
typedef struct cloth_capture_set { int current,bits; } cloth_capture_set;
static int cloth_current_captures(void *context,const void *value)
{
    cloth_capture_set *set=context; const sh_package_resource_capture *capture=value;
    if(capture->generation==500) set->bits|=1;
    else if(capture->generation==501) set->bits|=2;
    else if(capture->generation==set->current) set->bits|=4;
    else set->bits|=8;
    return 1;
}
static void cloth_initialize(void *model,void *decl,int kind,void *attachments,void *spheres,
    int width,int height,float h,float v)
{
    cloth_init_calls++; CHECK(decl==cloth_use_decl && kind==4 && attachments==(void *)0x111 && spheres==(void *)0x222);
    CHECK(width==7 && height==8 && h==1.25f && v==2.5f);
    if(cloth_use_mode==3) RaiseException(0xe1320053u,0,0,NULL);
    if(cloth_use_mode==4) CHECK(sh_resource_instances_publish(decl,cloth_changed));
    if(cloth_use_mode==5) { rg_instance_model_destroy_hook(model); rg_instance_model_base_hook(model); return; }
    put_pointer(model,owner_renderer?0x10e0:0x1060,decl);
    put_pointer(model,0xe90,cloth_simulation); put_pointer(cloth_simulation,0,decl);
    put_pointer(model,0xae0,cloth_rows); *(int *)((unsigned char *)model+0xae8)=*(int *)((unsigned char *)model+0xaec)=1;
    put_pointer(cloth_rows,0,cloth_use_mode==1?*(void **)((unsigned char *)model+0x118):cloth_use_material);
    put_pointer(cloth_rows,0x18,cloth_use_mode==6?NULL:cloth_geometry);
    size_t settings=owner_renderer?0x1120:0x10a0;
    memcpy((unsigned char *)model+settings,&width,4); memcpy((unsigned char *)model+settings+4,&height,4);
    memcpy((unsigned char *)model+settings+8,&v,4); memcpy((unsigned char *)model+settings+12,&h,4);
    memset((unsigned char *)model+0xa8,0x23,24);
    memset((unsigned char *)model+rg_cloth_output_offset(owner_renderer),0x23,24);
}
static void cloth_worker(void *descriptor)
{
    cloth_worker_calls++; unsigned char *model=cloth_worker_model;
    CHECK(descriptor==model+rg_cloth_descriptor_offset(owner_renderer));
    if(cloth_worker_mode==2) RaiseException(0xe1320072u,0,0,NULL);
    if(cloth_worker_mode==3) ((unsigned char *)descriptor)[0x20]^=1;
    if(cloth_worker_mode==4 || cloth_worker_mode==5) {
        rg_instance_model_destroy_hook(model); CHECK(instance_leases>0);
        if(cloth_worker_mode==4) { rg_instance_model_base_hook(model);
            put_pointer(model,0,(void *)(sh_host_image_base()+rg_cloth_tables[owner_renderer])); }
        return;
    }
    memset(model+rg_cloth_output_offset(owner_renderer),0x45,24);
}
static void cloth_render(void *model,void *jobs,void *vertices)
{
    cloth_render_calls++; CHECK(jobs==(void *)0x333 && vertices==(void *)0x444);
    if(cloth_use_mode==11) RaiseException(0xe132005bu,0,0,NULL);
    if(*(int *)(cloth_simulation+0x70)>=101) return;
    void *decl=*(void **)((unsigned char *)model+(owner_renderer?0x10e0:0x1060));
    memcpy((unsigned char *)model+(owner_renderer?0x1090:0x1010),(unsigned char *)decl+0x70,4);
    memcpy((unsigned char *)model+rg_cloth_descriptor_offset(owner_renderer)+0xe0,
        (unsigned char *)*(void **)cloth_simulation+0x84,4);
    put_pointer(model,rg_cloth_descriptor_offset(owner_renderer)+0x78,(unsigned char *)model+rg_cloth_output_offset(owner_renderer));
    if(cloth_worker_sync) rg_instance_cloth_worker_hook((unsigned char *)model+rg_cloth_descriptor_offset(owner_renderer));
    if(cloth_use_mode==12) CHECK(sh_resource_instances_publish(decl,cloth_changed));
}
static void test_cloth_consumed_origins(void)
{
    unsigned char decl[0xa0]={0},material[0x1a0]={0},override_material[0x1a0]={0},managers[3][16]={{0}},model[0x1130];
    void *resources[]={decl,material,override_material}; const char *families[]={"cloth","material","material"};
    for(int i=0;i<3;i++) { put_pointer(resources[i],8,i==0?"cloth/used":i==1?"cloth/material":"cloth/override");
        put_pointer(resources[i],0x10,managers[i]); put_pointer(managers[i],8,(void *)families[i]); }
    put_pointer(decl,0x90,material); cloth_use_decl=decl; cloth_use_material=material;
    g_instance_model_base=instance_resource_ctor; g_instance_model_destroy=instance_model_destructor;
    g_instance_cloth_init=cloth_initialize; g_instance_cloth_render=cloth_render; g_instance_cloth_worker=cloth_worker; cloth_worker_sync=0;
    cloth_init_calls=cloth_render_calls=0;
    for(int renderer=0;renderer<2;renderer++) for(int mode=0;mode<10;mode++) {
        owner_renderer=renderer; cloth_use_mode=mode; memset(model,0,sizeof(model)); memset(cloth_simulation,0,sizeof(cloth_simulation));
        rg_instance_model_base_hook(model); uint64_t token=sh_resource_instances_token(model); CHECK(token);
        put_pointer(model,0,(void *)(sh_host_image_base()+(renderer?0x253f068:0x25552d8)));
        if(mode==1) put_pointer(model,0x118,override_material);
        if(mode==9) put_pointer(model,0xe90,cloth_simulation); /* native same-holder reinitialization */
        sh_resource_instance_origin *origins[3];
        for(int i=0;i<3;i++) { origins[i]=sh_resource_instances_origin(instance_capture(500+mode*3+i),0);
            CHECK(origins[i]); if(mode!=2 || i!=1) CHECK(sh_resource_instances_publish(resources[i],origins[i])); }
        cloth_changed=sh_resource_instances_origin(instance_capture(600+mode),0);
        if(mode==7) g_instance_allocation_fault=1;
        if(mode==8) { CHECK(sh_resource_instances_origin_hold(origins[0]) && sh_resource_instances_origin_hold(origins[1])); instance_allowed=0; }
        int caught=0;
        __try { rg_instance_cloth_init_hook(model,decl,4,(void *)0x111,(void *)0x222,7,8,1.25f,2.5f); }
        __except(GetExceptionCode()==0xe1320053u?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
        g_instance_allocation_fault=0; instance_allowed=1; CHECK(caught==(mode==3));
        uint64_t current_token=sh_resource_instances_token(model); CHECK(current_token);
        sh_resource_instance_origin *published=sh_resource_instances_lookup(model);
        if(mode<2 || mode==9) {
            CHECK(published && instance_leases==(mode==1?3:2));
            CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,decl));
            CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,material));
            if(mode==1) CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,override_material));
        } else CHECK(!published);
        sh_resource_instances_origin_release(published);
        if(mode==0) {
            /* Render copies replace only this actual friction field. Old
             * simulation/layout and material retain their exact generation. */
            for(int turn=0;turn<3;turn++) {
                sh_resource_instance_origin *next=sh_resource_instances_origin(instance_capture(700+turn),0);
                CHECK(sh_resource_instances_publish(decl,next)); *(int *)(decl+0x70)=123+turn;
                cloth_use_mode=0; rg_instance_cloth_render_hook(model,(void *)0x333,(void *)0x444);
                sh_resource_instance_origin *field=sh_resource_instances_field_origin(model,token,(uintptr_t)model+(renderer?0x1090:0x1010));
                CHECK(field==next && instance_leases==3); sh_resource_instances_origin_release(field);
                sh_resource_instance_origin *model_origin=sh_resource_instances_lookup(model);
                cloth_capture_set captures={700+turn,0};
                CHECK(model_origin && sh_resource_instances_origin_captures(model_origin,cloth_current_captures,&captures) && captures.bits==7);
                sh_resource_instances_origin_release(model_origin);
                sh_resource_instances_origin_release(next);
            }
            cloth_use_mode=12; rg_instance_cloth_render_hook(model,(void *)0x333,(void *)0x444);
            CHECK(sh_resource_instances_faulted(model,token) && instance_leases==4);
            cloth_use_mode=11;
            __try { rg_instance_cloth_render_hook(model,(void *)0x333,(void *)0x444); CHECK(0); }
            __except(GetExceptionCode()==0xe132005bu?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { }
            *(int *)(cloth_simulation+0x70)=101; cloth_use_mode=0;
            rg_instance_cloth_render_hook(model,(void *)0x333,(void *)0x444); CHECK(instance_leases==4);
        }
        CHECK(!sh_resource_instances_complete(model,current_token));
        for(int i=0;i<3;i++) sh_resource_instances_forget(resources[i]);
        rg_instance_model_destroy_hook(model);
        if(mode==8) { sh_resource_instances_origin_unhold(origins[0]); sh_resource_instances_origin_unhold(origins[1]); }
        CHECK(!instance_leases);
        for(int i=0;i<3;i++) { sh_resource_instances_destroyed(resources[i],sh_resource_instances_token(resources[i]),1);
            sh_resource_instances_origin_release(origins[i]); }
        sh_resource_instances_origin_release(cloth_changed);
    }
    CHECK(cloth_init_calls==20 && cloth_render_calls==12);
}

static int cloth_has_generation(void *context,const void *value)
{
    const sh_package_resource_capture *capture=value; int *generation=context;
    if(capture->generation==generation[0]) generation[1]++; return 1;
}
static int cloth_origin_has(sh_resource_instance_origin *origin,int generation)
{
    int context[]={generation,0}; CHECK(origin && sh_resource_instances_origin_captures(origin,cloth_has_generation,context));
    return context[1]!=0;
}
static void test_cloth_descriptor_workers(void)
{
    unsigned char decl[0xa0]={0},material[0x1a0]={0},managers[2][16]={{0}},model[0x1130];
    void *resources[]={decl,material}; const char *families[]={"cloth","material"};
    for(int i=0;i<2;i++) { put_pointer(resources[i],8,i?"cloth/worker-material":"cloth/worker");
        put_pointer(resources[i],0x10,managers[i]); put_pointer(managers[i],8,(void *)families[i]); }
    put_pointer(decl,0x90,material); cloth_use_decl=decl; cloth_use_material=material;
    g_instance_model_base=instance_resource_ctor; g_instance_model_destroy=instance_model_destructor;
    g_instance_cloth_init=cloth_initialize; g_instance_cloth_render=cloth_render; g_instance_cloth_worker=cloth_worker;
    cloth_worker_calls=0;
    for(int renderer=0;renderer<2;renderer++) for(int mode=0;mode<8;mode++) {
        owner_renderer=renderer; cloth_worker_mode=mode; cloth_worker_sync=0; cloth_worker_model=model; cloth_use_mode=0;
        memset(model,0,sizeof(model)); memset(cloth_simulation,0,sizeof(cloth_simulation));
        rg_instance_model_base_hook(model); uint64_t token=sh_resource_instances_token(model); CHECK(token);
        put_pointer(model,0,(void *)(sh_host_image_base()+rg_cloth_tables[renderer]));
        sh_resource_instance_origin *a=sh_resource_instances_origin(instance_capture(800),0),*b=sh_resource_instances_origin(instance_capture(801),0),
            *c=sh_resource_instances_origin(instance_capture(802),0),*m=sh_resource_instances_origin(instance_capture(803),0);
        CHECK(sh_resource_instances_publish(decl,a) && sh_resource_instances_publish(material,m));
        rg_instance_cloth_init_hook(model,decl,4,(void *)0x111,(void *)0x222,7,8,1.25f,2.5f);
        CHECK(sh_resource_instances_publish(decl,b)); *(int *)(decl+0x70)=55; *(int *)(decl+0x84)=66;
        if(mode==1) cloth_worker_sync=1;
        rg_instance_cloth_render_hook(model,(void *)0x333,(void *)0x444); cloth_worker_sync=0;
        unsigned char *descriptor=model+rg_cloth_descriptor_offset(renderer),bytes[256]; memcpy(bytes,descriptor,sizeof(bytes));
        sh_resource_instance_origin *input=sh_resource_instances_copy_hold(model,token,(uintptr_t)descriptor,bytes,sizeof(bytes));
        CHECK(input && cloth_origin_has(input,800) && cloth_origin_has(input,801) && cloth_origin_has(input,803));
        sh_resource_instances_origin_unhold(input);
        if(mode==0) {
            /* A new simulation does not retire an old queued descriptor. */
            CHECK(sh_resource_instances_publish(decl,c));
            rg_instance_cloth_init_hook(model,decl,4,(void *)0x111,(void *)0x222,7,8,1.25f,2.5f);
            CHECK(!memcmp(bytes,descriptor,sizeof(bytes)));
            sh_resource_instance_origin *payload=sh_resource_instances_lookup(model);
            CHECK(payload && cloth_origin_has(payload,802) && !cloth_origin_has(payload,800) && !cloth_origin_has(payload,801));
            sh_resource_instances_origin_release(payload);
        }
        if(mode==5) g_instance_allocation_fault=1;
        if(mode==6) descriptor[0x28]^=1;
        if(mode==7) instance_allowed=0;
        uint64_t old_token=token; int caught=0;
        __try { rg_instance_cloth_worker_hook(descriptor); }
        __except(GetExceptionCode()==0xe1320072u?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
        g_instance_allocation_fault=0; instance_allowed=1; CHECK(caught==(mode==2));
        token=sh_resource_instances_token(model);
        if(mode==4) CHECK(token && token!=old_token);
        else if(mode==5) CHECK(!token);
        else if(mode==2 || mode==3 || mode==6) CHECK(sh_resource_instances_faulted(model,token));
        else if(mode!=7) {
            unsigned char bounds[24]; memcpy(bounds,model+rg_cloth_output_offset(renderer),sizeof(bounds));
            sh_resource_instance_origin *output=sh_resource_instances_copy_hold(model,token,
                (uintptr_t)model+rg_cloth_output_offset(renderer),bounds,sizeof(bounds));
            CHECK(output && cloth_origin_has(output,800) && cloth_origin_has(output,801));
            sh_resource_instances_origin_unhold(output);
        }
        if(mode==0) {
            /* The visible bounds copy owns the old worker result separately,
             * then retires it after a new-generation worker and real recopy. */
            memcpy(model+0xa8,model+rg_cloth_output_offset(renderer),24);
            rg_instance_cloth_render_body(model,(void *)0x333,(void *)0x444,sh_host_image_base()+(renderer?0x14310e5:0x1425315));
            sh_resource_instance_origin *payload=sh_resource_instances_lookup(model);
            CHECK(payload && cloth_origin_has(payload,800) && cloth_origin_has(payload,801) && cloth_origin_has(payload,802));
            sh_resource_instances_origin_release(payload);
            rg_instance_cloth_worker_hook(descriptor);
            memcpy(model+0xa8,model+rg_cloth_output_offset(renderer),24);
            rg_instance_cloth_render_body(model,(void *)0x333,(void *)0x444,sh_host_image_base()+(renderer?0x14310e5:0x1425315));
            payload=sh_resource_instances_lookup(model);
            CHECK(payload && cloth_origin_has(payload,802) && !cloth_origin_has(payload,800) && !cloth_origin_has(payload,801));
            sh_resource_instances_origin_release(payload); CHECK(instance_leases==2);
        }
        if(token) rg_instance_model_destroy_hook(model);
        sh_resource_instances_forget(decl); sh_resource_instances_forget(material);
        sh_resource_instances_destroyed(decl,sh_resource_instances_token(decl),1);
        sh_resource_instances_destroyed(material,sh_resource_instances_token(material),1);
        CHECK(!instance_leases);
        sh_resource_instances_origin_release(a); sh_resource_instances_origin_release(b);
        sh_resource_instances_origin_release(c); sh_resource_instances_origin_release(m);
    }
    CHECK(cloth_worker_calls==20); cloth_worker_sync=0;
}

static int refresh_mode,refresh_calls,refresh_model_deleted;
static uint64_t refresh_update(void *model)
{
    refresh_calls++; CHECK(*(int *)((unsigned char *)model+0xea4)==-1);
    CHECK(instance_leases>=2); /* old and replacement remain served across native */
    void *backing=*(void **)((unsigned char *)model+0xe90);
    memcpy((unsigned char *)model+0xa8,(unsigned char *)backing+0x48,24);
    if(refresh_mode==3) {
        rg_instance_model_destroy_hook(model); rg_instance_model_base_hook(model); refresh_model_deleted=1;
        return 1;
    }
    if(refresh_mode==2) RaiseException(0xe0000070u,0,0,NULL);
    return refresh_mode==1 ? 0 : 1;
}
static void test_static_instance_refresh(void)
{
    unsigned char backing[0x90]={0},manager[0x10]={0},alias[32]={0};
    void *alias_table[1]={native_path_get_type};
    int alias_size=sizeof(alias); memcpy(native_path_type+0x64,&alias_size,4);
    put_pointer(alias,0,alias_table); put_pointer(backing,8,"models/shared"); put_pointer(backing,0x10,manager);
    put_pointer(manager,8,"staticmodel");
    unsigned char *model=calloc(1,0xf20); CHECK(model); if(!model) return;
    g_instance_model_update=refresh_update; g_instance_model_base=instance_resource_ctor;
    g_instance_model_destroy=instance_model_destructor; g_instance_model_static=instance_model_constructor;
    instance_model_offset=0xe90; instance_model_reload=instance_model_throw=instance_model_destroy_throw=0;
    for(int renderer=0;renderer<2;renderer++) for(int mode=0;mode<4;mode++) {
        owner_renderer=renderer; refresh_calls=refresh_model_deleted=0; refresh_mode=mode;
        sh_resource_instance_origin *old=sh_resource_instances_origin(instance_capture(30+mode*2),0);
        sh_resource_instance_origin *next=sh_resource_instances_origin(instance_capture(31+mode*2),0);
        CHECK(old && next && sh_resource_instances_publish(backing,old));
        memset(model,0,0xf20); CHECK(rg_instance_model_static_hook(model,backing)==model);
        put_pointer(model,0,(void *)(sh_host_image_base()+(renderer?0x25502b0:0x2566520)));
        uint64_t model_token=sh_resource_instances_token(model);
        uint64_t token=sh_resource_instances_construct(alias,1);
        put_pointer(alias,8,model);
        CHECK(sh_resource_instances_assign(alias,token,(uintptr_t)alias+8,model,old));
        CHECK(instance_leases==1);
        CHECK(sh_resource_instances_publish(backing,next)); memset(backing+0x48,0x29,24);
        void *resources[]={backing,backing}; char error[256];
        int result=sh_resource_graph_native_refresh_instances(resources,2,error,sizeof(error));
        CHECK(result==(mode==0) && refresh_calls==1);
        if(mode==3) {
            CHECK(refresh_model_deleted && sh_resource_instances_token(model)!=model_token && instance_leases==1);
        } else {
            if(mode) {
                CHECK(instance_leases==2 && !sh_resource_instances_complete(model,model_token));
                refresh_mode=0;
                CHECK(sh_resource_graph_native_refresh_instances(resources,2,error,sizeof(error)) && refresh_calls==2);
            }
            CHECK(instance_leases==1 && !memcmp(model+0xa8,backing+0x48,24));
            sh_resource_instance_origin *actual=sh_resource_instances_field_origin(alias,token,(uintptr_t)alias+8);
            sh_resource_instance_origin *published=sh_resource_instances_lookup(model);
            CHECK(actual && actual==published); sh_resource_instances_origin_release(published); sh_resource_instances_origin_release(actual);
            actual=sh_resource_instances_field_origin(model,model_token,(uintptr_t)model+0xa8);
            CHECK(actual==next); sh_resource_instances_origin_release(actual);
        }
        sh_resource_instances_destroyed(alias,token,1);
        rg_instance_model_destroy_hook(model);
        CHECK(instance_leases==0);
        sh_resource_instances_forget(backing); sh_resource_instances_origin_release(old); sh_resource_instances_origin_release(next);
    }
    sh_resource_instances_destroyed(backing,sh_resource_instances_token(backing),1);
    free(model); g_instance_model_update=NULL;
}
/* Admission before a mutation (the OpenGL L2 reopen of 2026-10-22: "native holder has an unresolved actual
 * input before mutation"). A model instance reading a changed input through a field the product never
 * attributed is rebuilt whole by the refresh and rebuilt again after the mutation, so it is tolerated and
 * reported; a holder of a kind the refresh cannot rebuild reports that input for isolation and admission
 * refuses with the count, so the resident pass can leave the input unmutated instead of failing. */
static void test_unknown_input_admission(void)
{
    unsigned char backing[0x90]={0},manager[0x10]={0},extra[0x40]={0},foreign[0x200]={0};
    put_pointer(backing,8,"models/shared"); put_pointer(backing,0x10,manager); put_pointer(manager,8,"staticmodel");
    unsigned char *model=calloc(1,0xf20); CHECK(model); if(!model) return;
    g_instance_model_update=refresh_update; g_instance_model_base=instance_resource_ctor;
    g_instance_model_destroy=instance_model_destructor; g_instance_model_static=instance_model_constructor;
    instance_model_offset=0xe90; instance_model_reload=instance_model_throw=instance_model_destroy_throw=0;
    for(int renderer=0;renderer<2;renderer++) {
        const sh_resource_admit_report *items=NULL; char error[256]="";
        owner_renderer=renderer; refresh_calls=refresh_model_deleted=0; refresh_mode=0;
        sh_resource_instance_origin *old=sh_resource_instances_origin(instance_capture(70+renderer*2),0);
        sh_resource_instance_origin *next=sh_resource_instances_origin(instance_capture(71+renderer*2),0);
        CHECK(old && next && sh_resource_instances_publish(backing,old));
        memset(model,0,0xf20); CHECK(rg_instance_model_static_hook(model,backing)==model);
        put_pointer(model,0,(void *)(sh_host_image_base()+(renderer?0x25502b0:0x2566520)));
        uint64_t model_token=sh_resource_instances_token(model);
        /* A field reading extra, which no observed source ever published; the backing changes too. */
        put_pointer(model,0x100,extra);
        CHECK(sh_resource_instances_assign(model,model_token,(uintptr_t)model+0x100,extra,NULL));
        CHECK(sh_resource_instances_publish(backing,next)); memset(backing+0x48,0x31,24);
        void *changed[2]={extra,backing};
        CHECK(sh_resource_graph_native_admit_instances(changed,2,error,sizeof(error)) && refresh_calls==1);
        CHECK(sh_resource_graph_native_admit_report(&items)==1);
        if(items) {
            CHECK(items[0].tolerated && items[0].holder==model && items[0].input==extra && items[0].slot==0x100 &&
                !items[0].unresolved_path && !strcmp(items[0].holder_kind,"model instance") &&
                items[0].holder_vtable_rva==(renderer?0x25502b0u:0x2566520u));
        }
        /* The same unattributed field on a holder of a kind the refresh cannot rebuild. */
        uint64_t foreign_token=sh_resource_instances_construct(foreign,1);
        put_pointer(foreign,0,(void *)(sh_host_image_base()+0x123450));
        put_pointer(foreign,0x40,extra);
        CHECK(sh_resource_instances_assign(foreign,foreign_token,(uintptr_t)foreign+0x40,extra,NULL));
        /* The backing changes again, so the rebuilt instance holds the old and the replacement origin across native. */
        sh_resource_instance_origin *again=sh_resource_instances_origin(instance_capture(74+renderer),0);
        CHECK(again && sh_resource_instances_publish(backing,again)); memset(backing+0x48,0x33,24);
        refresh_calls=0; error[0]=0;
        CHECK(!sh_resource_graph_native_admit_instances(changed,2,error,sizeof(error)) && refresh_calls==1);
        if(!strstr(error,"1 input(s) reported for isolation")) fprintf(stderr,"admit error: %s\n",error);
        CHECK(strstr(error,"unresolved actual input before mutation") && strstr(error,"1 input(s) reported for isolation"));
        size_t count=sh_resource_graph_native_admit_report(&items); size_t isolated=0,tolerated=0;
        for(size_t i=0;i<count;i++) {
            if(items[i].holder==foreign) {
                isolated++; CHECK(!items[i].tolerated && items[i].input==extra && items[i].slot==0x40 &&
                    !strcmp(items[i].holder_kind,"unknown") && items[i].holder_vtable_rva==0x123450u);
            } else { tolerated++; CHECK(items[i].tolerated && items[i].holder==model); }
        }
        CHECK(isolated==1 && tolerated==1); /* the rebuilt model instance is tolerated, the foreign holder is not */
        /* A model instance whose backing is not changing is not rebuilt either, so its unattributed input is isolated too. */
        refresh_calls=0; error[0]=0; sh_resource_instances_destroyed(foreign,foreign_token,1); foreign_token=0;
        CHECK(!sh_resource_graph_native_admit_instances(changed,1,error,sizeof(error)) && !refresh_calls);
        count=sh_resource_graph_native_admit_report(&items);
        CHECK(count==1 && items && !items[0].tolerated && items[0].holder==model && items[0].input==extra);
        /* An unrelated set reports nothing. */
        void *unrelated[1]={backing+1};
        CHECK(sh_resource_graph_native_admit_instances(unrelated,1,error,sizeof(error)) && sh_resource_graph_native_admit_report(&items)==0);
        rg_instance_model_destroy_hook(model);
        sh_resource_instances_forget(backing); sh_resource_instances_origin_release(old); sh_resource_instances_origin_release(next);
        sh_resource_instances_origin_release(again);
        CHECK(instance_leases==0);
    }
    sh_resource_instances_destroyed(backing,sh_resource_instances_token(backing),1);
    free(model); g_instance_model_update=NULL;
}
static int particle_refresh_mode,particle_clear_calls,particle_init_calls;
static unsigned char particle_runtime[3*0xd0],particle_gpu[3*0x120],particle_raw[16];
static void *particle_constructor(void *model,void *backing)
{
    instance_model_constructor(model,backing);
    *(int *)((unsigned char *)model+0xed8)=*(int *)((unsigned char *)backing+0x70);
    put_pointer(model,0xec8,particle_runtime); put_pointer(model,0xef8,particle_gpu); put_pointer(model,0xf70,particle_raw);
    return model;
}
static void particle_clear(void *model)
{
    particle_clear_calls++; CHECK(instance_leases>=2);
    if(particle_refresh_mode==5) RaiseException(0xe0000071u,0,0,NULL);
    put_pointer(model,0xec8,NULL); put_pointer(model,0xef8,NULL); *(int *)((unsigned char *)model+0xed8)=0;
    if(particle_refresh_mode==3) { rg_instance_model_destroy_hook(model); rg_instance_model_base_hook(model); }
}
static void particle_init(void *model,int count)
{
    particle_init_calls++; CHECK(instance_leases>=2);
    CHECK(!*(void **)((unsigned char *)model+0xf70));
    CHECK(!*(int *)((unsigned char *)model+0xee8) && !*(int *)((unsigned char *)model+0xae8));
    put_pointer(model,0xec8,count?particle_runtime:NULL); put_pointer(model,0xef8,count?particle_gpu:NULL);
    put_pointer(model,0xf70,particle_raw);
    *(int *)((unsigned char *)model+0xed8)=count+(particle_refresh_mode==1);
    *(int *)((unsigned char *)model+0xf00)=count; *(int *)((unsigned char *)model+0xf10)=count;
    if(particle_refresh_mode==2) RaiseException(0xe0000072u,0,0,NULL);
}
static void test_particle_instance_refresh(void)
{
    unsigned char backing[0xc0]={0},stages[3*0x6e0]={0},model[0x1000]={0},alias[32]={0};
    void *alias_table[1]={native_path_get_type}; int size=sizeof(alias); memcpy(native_path_type+0x64,&size,4);
    put_pointer(alias,0,alias_table); put_pointer(backing,0x68,stages);
    g_instance_model_update=refresh_update; g_instance_particle_clear=particle_clear; g_instance_particle_init=particle_init;
    g_instance_model_particle=particle_constructor; instance_model_offset=0xeb0;
    instance_model_reload=instance_model_throw=instance_model_destroy_throw=0;
    for(int renderer=0;renderer<2;renderer++) for(int mode=0;mode<6;mode++) {
        owner_renderer=renderer; particle_refresh_mode=mode; particle_clear_calls=particle_init_calls=0;
        sh_resource_instance_origin *old=sh_resource_instances_origin(instance_capture(50+mode*2),0);
        sh_resource_instance_origin *next=sh_resource_instances_origin(instance_capture(51+mode*2),0);
        CHECK(sh_resource_instances_publish(backing,old)); *(int *)(backing+0x70)=2;
        memset(model,0,sizeof(model)); CHECK(rg_instance_model_particle_hook(model,backing)==model);
        put_pointer(model,0,(void *)(sh_host_image_base()+(renderer?0x2547fb0:0x255e220)));
        uint64_t token=sh_resource_instances_token(model),alias_token=sh_resource_instances_construct(alias,1);
        put_pointer(alias,8,model); CHECK(sh_resource_instances_assign(alias,alias_token,(uintptr_t)alias+8,model,old));
        *(int *)(model+0xee8)=7; *(int *)(model+0xae8)=9;
        CHECK(sh_resource_instances_publish(backing,next)); *(int *)(backing+0x70)=mode==4?0:3;
        memset(backing+0x98,0x42,24); void *batch[2]={backing,backing}; char error[256];
        int result=sh_resource_graph_native_refresh_instances(batch,2,error,sizeof(error));
        CHECK(result==(mode==0 || mode==4) && particle_clear_calls==1);
        if(mode==3) CHECK(sh_resource_instances_token(model)!=token && instance_leases==1 && !particle_init_calls);
        else {
            if(mode==1 || mode==2 || mode==5) {
                CHECK(instance_leases==2); particle_refresh_mode=0;
                CHECK(sh_resource_graph_native_refresh_instances(batch,2,error,sizeof(error)) && particle_clear_calls==2);
            }
            CHECK(*(int *)(model+0xed8)==*(int *)(backing+0x70) && !memcmp(model+0xa8,backing+0x98,24));
            CHECK(instance_leases==1);
            sh_resource_instance_origin *actual=sh_resource_instances_field_origin(model,token,(uintptr_t)model+0xed8);
            CHECK(actual==next); sh_resource_instances_origin_release(actual);
            actual=sh_resource_instances_field_origin(alias,alias_token,(uintptr_t)alias+8);
            sh_resource_instance_origin *published=sh_resource_instances_lookup(model);
            CHECK(actual && actual==published); sh_resource_instances_origin_release(published); sh_resource_instances_origin_release(actual);
        }
        sh_resource_instances_destroyed(alias,alias_token,1); rg_instance_model_destroy_hook(model); CHECK(!instance_leases);
        sh_resource_instances_forget(backing); sh_resource_instances_origin_release(old); sh_resource_instances_origin_release(next);
    }
    sh_resource_instances_destroyed(backing,sh_resource_instances_token(backing),1);
    g_instance_model_update=NULL; g_instance_particle_clear=NULL; g_instance_particle_init=NULL;
}
static void test_native_observation_faults(void *a,void *b)
{
    unsigned char holder[32]={0}; char error[256]; void *changed[2]={a,b},*target=NULL,*input=NULL;
    uint64_t token=sh_resource_instances_construct(holder,1),target_token=0,input_token=0;
    owner_renderer=0; owner_pinned=1;
    rg_instance_scope owner=rg_instance_begin(RG_INSTANCE_OWNER,holder,token);
    rg_instance_scope field=rg_instance_begin(RG_INSTANCE_FIELD,holder,token);
    g_instance_allocation_fault=1; rg_instance_returned(a); /* missing read record after actual input */
    put_pointer(holder,8,a); rg_instance_field_end(field,(void **)(holder+8),NULL,1); rg_instance_owner_end(owner);
    CHECK(sh_resource_instances_faulted(holder,token) && instance_leases==1);
    CHECK(sh_resource_instances_fault_scope_begin(holder,token,99));
    CHECK(!sh_resource_graph_native_admit_instances(changed,1,error,sizeof(error)) && strstr(error,"active observation"));
    sh_resource_instances_fault_scope_end(holder,token,99);
    CHECK(!sh_resource_graph_native_admit_instances(changed,1,error,sizeof(error)) && strstr(error,"full reconciliation"));
    CHECK(sh_resource_graph_native_admit_instances(changed+1,1,error,sizeof(error))); /* unrelated actual input */
    CHECK(sh_resource_instances_assign(holder,token,(uintptr_t)holder+8,NULL,NULL));
    CHECK(sh_resource_instances_faulted(holder,token) && instance_leases==1); /* partial edit is not recovery */
    sh_resource_instances_destroyed(holder,token,0); CHECK(instance_leases==1);
    sh_resource_instances_destroyed(holder,token,1); CHECK(!instance_leases);
    CHECK(sh_resource_graph_native_admit_instances(changed,1,error,sizeof(error)));
    /* Failure before a field scope is allocated poisons the existing owner;
     * the owner's captured actual read remains live until that lifetime ends. */
    token=sh_resource_instances_construct(holder,1); owner=rg_instance_begin(RG_INSTANCE_OWNER,holder,token);
    g_instance_allocation_fault=1; CHECK(!rg_instance_begin(RG_INSTANCE_FIELD,holder,token).serial);
    rg_instance_returned(a); rg_instance_owner_end(owner);
    CHECK(sh_resource_instances_faulted(holder,token) && instance_leases==1);
    sh_resource_instances_destroyed(holder,token,1); CHECK(!instance_leases);
    /* A failed SOURCE promotion belongs to the outer consumer, independently
     * of the source object's later reconstruction. */
    token=sh_resource_instances_construct(holder,1); owner=rg_instance_begin(RG_INSTANCE_OWNER,holder,token);
    rg_instance_scope source=rg_instance_source_begin(b,0);
    instance_promotion_failure=1;
    CHECK(sh_resource_graph_native_source_observed("material","b",instance_capture(12),0)==-1);
    instance_promotion_failure=0;
    CHECK(sh_resource_graph_native_source_fault_target(&target,&target_token) && target==holder && target_token==token);
    CHECK(sh_resource_graph_native_source_fault_input(&input,&input_token) && input==b && input_token==sh_resource_instances_token(b));
    CHECK(!sh_resource_graph_native_source_fault_recovered(target,target_token));
    rg_instance_source_end(source,1,0); rg_instance_owner_end(owner);
    CHECK(sh_resource_instances_faulted(holder,token));
    CHECK(sh_resource_instances_fault_recovered(b,input_token)); CHECK(!sh_resource_graph_native_source_fault_recovered(target,target_token));
    sh_resource_instances_destroyed(holder,token,1); CHECK(sh_resource_graph_native_source_fault_recovered(target,target_token));
    CHECK(!sh_resource_graph_native_source_fault_recovered(NULL,0));
    instance_admission_failure=1; CHECK(!sh_resource_graph_native_admit_instances(changed+1,1,error,sizeof(error)));
    instance_admission_failure=0;
}
static void test_instance_native_observation(void)
{
    unsigned char a[0x20] = {0}, b[0x20] = {0}, resource_manager[16] = {0};
    char family[128], name[4096];
    sh_resource_instance_api api = {rg_instance_promote, rg_instance_release_capture, rg_instance_release_lease};
    CHECK(sh_resource_instances_bind(api)); g_instance_ready = 1;
    put_pointer(resource_manager, 8, "material"); put_pointer(a, 8, "a"); put_pointer(a, 0x10, resource_manager);
    put_pointer(b, 8, "b"); put_pointer(b, 0x10, resource_manager);
    CHECK(!sh_resource_graph_native_source_target(family, name));
    rg_instance_scope source = rg_instance_source_begin(a, 0);
    CHECK(sh_resource_graph_native_source_target(family, name) && !strcmp(family, "material") && !strcmp(name, "a"));
    CHECK(!sh_resource_graph_native_source_observed("material", "other", instance_capture(7), 0));
    CHECK(instance_captures == 0);
    sh_package_resource_capture *chunks = instance_capture(1);
    CHECK(sh_resource_graph_native_source_observed("material", "a", sh_package_runtime_resource_capture_retain(chunks), 0));
    CHECK(instance_leases == 1); /* serving fence spans native parsing */
    int promotions = instance_promotions;
    for (int chunk = 0; chunk < 128; chunk++)
        CHECK(sh_resource_graph_native_source_observed("material", "a", sh_package_runtime_resource_capture_retain(chunks), 0));
    CHECK(instance_promotions == promotions && instance_leases == 1);
    sh_package_runtime_resource_capture_release(chunks);
    rg_instance_source_end(source, 1, 0); CHECK(instance_captures == 1 && instance_leases == 0);
    sh_resource_instance_origin *original = sh_resource_instances_lookup(a); CHECK(original);
    /* No source witness means cached/no-op, even after provider publication. */
    instance_pristine = 0; source = rg_instance_source_begin(a, 1); rg_instance_source_end(source, 1, 0);
    sh_resource_instance_origin *current = sh_resource_instances_lookup(a); CHECK(current == original);
    sh_resource_instances_origin_release(current);
    /* Fixed payload/editor rejection restores the exact prior association. */
    source = rg_instance_source_begin(a, 0);
    CHECK(sh_resource_graph_native_source_observed("material", "a", instance_capture(2), 0));
    rg_instance_source_end(source, 1, 1); CHECK(instance_captures == 1);
    current = sh_resource_instances_lookup(a); CHECK(current == original); sh_resource_instances_origin_release(current);
    source = rg_instance_source_begin(a, 0); instance_allowed = 0;
    CHECK(!sh_resource_graph_native_source_target(family, name));
    CHECK(!sh_resource_graph_native_source_observed("material", "a", instance_capture(8), 0));
    instance_allowed = 1; rg_instance_source_end(source, 1, 0); CHECK(instance_captures == 1);
    source = rg_instance_source_begin(b, 0);
    CHECK(sh_resource_graph_native_source_observed("material", "b", instance_capture(3), 0));
    rg_instance_source_end(source, 1, 0);
    sh_resource_instance_origin *saved_b=sh_resource_instances_lookup(b); CHECK(saved_b);
    test_native_observation_faults(a,b);
    CHECK(sh_resource_instances_publish(b,saved_b)); sh_resource_instances_origin_release(saved_b);
    memset(instance_object, 0, sizeof(instance_object)); memset(instance_tools, 0, sizeof(instance_tools));
    memset(instance_reader, 0, sizeof(instance_reader));
    put_pointer(instance_tools, 0xa0, instance_handlers);
    owner_renderer = 0; owner_pinned = 1;
    instance_handlers[1] = (void *)(sh_host_image_base() + 0x9ca050);
    g_instance_index = instance_index; g_instance_type = instance_type;
    uint64_t token = sh_resource_instances_construct(instance_object, 1); CHECK(token);
    instance_value = a; instance_read_mode = 0; instance_field(token);
    CHECK(instance_leases == 1 && instance_lease_generation == 1 && sh_resource_instances_complete(instance_object, token));
    instance_read_mode = 1; instance_field(token); CHECK(instance_leases == 1 && instance_lease_generation == 1);
    instance_value = b; instance_read_mode = 5;
    __try { instance_field(token); CHECK(0); }
    __except (GetExceptionCode() == 0xe0000067u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { }
    CHECK(instance_leases == 1 && instance_lease_generation == 3 && sh_resource_instances_complete(instance_object, token));
    instance_read_mode = 2; instance_field(token); CHECK(instance_leases == 1);
    instance_read_mode = 3; instance_field(token); CHECK(instance_leases == 0);
    /* A skipped unrelated pointer callback cannot create unknown resources. */
    instance_handlers[1] = (void *)1; instance_read_mode = 1; instance_field(token);
    CHECK(sh_resource_instances_complete(instance_object, token) && instance_leases == 0);
    instance_handlers[1] = (void *)(sh_host_image_base() + 0x9cd4e0);
    instance_read_mode = 4; rg_state_fn saved_state = g_state; g_state = instance_model_state;
    rg_state_hook(NULL, instance_object, instance_reader, 0); g_state = saved_state;
    CHECK(instance_leases == 0); /* +d==0 writes an encoded index, never a resource pointer. */
    instance_reader[0xd] = 1; instance_read_mode = 0; g_state = instance_model_state;
    rg_state_hook(NULL, instance_object, instance_reader, 0); g_state = saved_state;
    CHECK(instance_leases == 1 && instance_lease_generation == 3); /* +d!=0 resolves immediately. */
    sh_resource_instances_destroyed(instance_object, token, 1); CHECK(instance_leases == 0);
    /* The exact reflected list callback identifies all flat pointer lists.
     * Native resize copies existing slots before per-element reads; zero
     * logical count keeps the allocation and bypasses resize altogether. */
    g_instance_resize = instance_resize;
    put_pointer(instance_tools, 0, instance_registry); put_pointer(instance_registry, 0x20, instance_class);
    put_pointer(instance_class, 0x18, instance_descriptor); put_pointer(instance_tools, 0x88, instance_class_handlers);
    instance_descriptor[0] = "idDecl"; instance_descriptor[1] = "*";
    for (int renderer = 0; renderer < 2; renderer++) {
        owner_renderer = renderer;
        instance_class_handlers[1] = (void *)(sh_host_image_base() + (renderer ? 0x9cd610 : 0x9ccdd0));
        instance_descriptor[8] = (void *)(sh_host_image_base() + (renderer ? 0x13de8b0 : 0x13cef80));
        void **data = calloc(2, sizeof(void *)); CHECK(data); data[0] = a; data[1] = b;
        memset(instance_object, 0, sizeof(instance_object)); put_pointer(instance_object, 16, data);
        int two = 2; memcpy(instance_object + 24, &two, 4);
        token = sh_resource_instances_construct(instance_object, 1);
        sh_resource_instance_origin *origin_b = sh_resource_instances_lookup(b); CHECK(origin_b);
        CHECK(sh_resource_instances_assign(instance_object, token, (uintptr_t)data, a, original));
        CHECK(sh_resource_instances_assign(instance_object, token, (uintptr_t)(data + 1), b, origin_b));
        sh_resource_instances_origin_release(origin_b); CHECK(instance_leases == 2);
        instance_list_count = 1;
        rg_instance_scope owner = rg_instance_begin(RG_INSTANCE_OWNER, instance_object, token);
        rg_instance_type_hook(instance_tools, instance_reader, 0, "idList", "", "materials", 0, 0,
            (void **)(instance_object + 16), 1); rg_instance_owner_end(owner);
        CHECK(instance_leases == 1 && sh_resource_instances_complete(instance_object, token));
        data = *(void ***)(instance_object + 16); CHECK(data && data[0] == a);
        instance_list_count = 0; owner = rg_instance_begin(RG_INSTANCE_OWNER, instance_object, token);
        rg_instance_type_hook(instance_tools, instance_reader, 0, "idList", "", "materials", 0, 0,
            (void **)(instance_object + 16), 1); rg_instance_owner_end(owner);
        CHECK(instance_leases == 0 && *(void ***)(instance_object + 16) == data);
        size_t r, i, fields; sh_resource_instances_counts(&r, &i, &fields); CHECK(fields == 0);
        free(data); sh_resource_instances_destroyed(instance_object, token, 1);
    }
    test_nested_native(a,b);
    /* FLS owns transient live reads. Deleting a suspended fiber never needs
     * to dereference the abandoned native/C stack to release those reads. */
    token = sh_resource_instances_construct(instance_object, 0);
    int converted = !IsThreadAFiber(); instance_main_fiber = converted ? ConvertThreadToFiber(NULL) : GetCurrentFiber();
    CHECK(instance_main_fiber);
    void *fiber = CreateFiber(0, instance_suspended_fiber, b); CHECK(fiber);
    if (fiber) { SwitchToFiber(fiber); CHECK(instance_leases == 1); DeleteFiber(fiber);
        sh_execution_context(NULL, 0); CHECK(instance_leases == 1 && sh_resource_instances_faulted(instance_object,token)); }
    if (converted) CHECK(ConvertFiberToThread());
    sh_resource_instances_destroyed(instance_object, token, 1);
    /* Runtime models are distinct allocations with their OWN backing/copy
     * lifetime through deferred unlink. Entry bounds and post-load backing
     * can have different exact origins; an integer cache read observes only
     * the established derived association. */
    unsigned char runtime_model[0x1000] = {0};
    g_instance_model_base = instance_resource_ctor; g_instance_model_destroy = instance_model_destructor;
    g_instance_model_static = g_instance_model_tree = g_instance_model_particle = instance_model_constructor;
    owner_renderer = 0; instance_handlers[1] = (void *)(sh_host_image_base() + 0x9cd4e0); instance_reader[0xd] = 1;
    for (int kind = 0; kind < 3; kind++) {
        instance_model_offset = kind == 2 ? 0xeb0 : 0xe90; instance_model_reload = kind == 0;
        void *result = kind == 0 ? rg_instance_model_static_hook(runtime_model, a) :
            kind == 1 ? rg_instance_model_construct(g_instance_model_tree, runtime_model, a,0xe90) : rg_instance_model_particle_hook(runtime_model, a);
        CHECK(result == runtime_model && instance_leases == (kind ? 1 : 2));
        uint64_t model_token = sh_resource_instances_token(runtime_model); CHECK(model_token);
        token = sh_resource_instances_construct(instance_object, 1);
        instance_value = runtime_model; instance_read_mode = 6; instance_field(token);
        CHECK(sh_resource_instances_complete(instance_object, token));
        instance_read_mode = 3; instance_field(token);
        sh_resource_instances_destroyed(instance_object, token, 1);
        CHECK(instance_leases == (kind ? 1 : 2)); /* field removal/unlink is NOT actual model destruction */
        if (kind == 1) {
            instance_model_destroy_throw = 1;
            __try { rg_instance_model_destroy_hook(runtime_model); CHECK(0); }
            __except (GetExceptionCode() == 0xe0000069u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { }
            CHECK(instance_leases == 1 && sh_resource_instances_token(runtime_model) == model_token);
            instance_model_destroy_throw = 0;
            /* Fixture's explicit completed cleanup event, not an automatic
             * native destructor retry after partially completed cleanup. */
            sh_resource_instances_destroyed(runtime_model, model_token, 1);
        } else rg_instance_model_destroy_hook(runtime_model);
        CHECK(instance_leases == 0 && !sh_resource_instances_lookup(runtime_model));
    }
    /* Private/unobserved construction does not borrow an already active
     * published origin, even when a live lease could bypass fresh promotion. */
    sh_resource_instance_origin *held_model_source = sh_resource_instances_lookup(a);
    CHECK(held_model_source && sh_resource_instances_origin_hold(held_model_source));
    sh_resource_instances_origin_release(held_model_source); CHECK(instance_leases == 1);
    instance_model_reload = 0; instance_allowed = 0;
    CHECK(rg_instance_model_static_hook(runtime_model, a) == runtime_model);
    instance_allowed = 1;
    CHECK(instance_leases == 1 && !sh_resource_instances_lookup(runtime_model));
    rg_instance_model_destroy_hook(runtime_model);
    sh_resource_instances_origin_unhold(held_model_source); CHECK(instance_leases == 0);
    instance_model_throw = 1;
    __try { rg_instance_model_static_hook(runtime_model, a); CHECK(0); }
    __except (GetExceptionCode() == 0xe0000068u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { }
    CHECK(instance_leases == 1); instance_model_throw = 0; rg_instance_model_destroy_hook(runtime_model); CHECK(!instance_leases);
    /* An actual read of an unidentified/generated source clears provenance;
     * merely failing to open a source never manufactures a baseline. */
    test_default_origin_native(a,b); test_presentation_native(a,b); test_derived_origin_native(a,b); test_tree_inputs(); test_cloth_instance_origins(); test_discrete_instance_origins(); test_cloth_consumed_origins(); test_cloth_descriptor_workers(); test_static_instance_refresh(); test_particle_instance_refresh(); test_unknown_input_admission();
    source = rg_instance_source_begin(b, 0);
    CHECK(sh_resource_graph_native_source_observed("material", "b", NULL, 0));
    rg_instance_source_end(source, 1, 0); CHECK(!sh_resource_instances_lookup(b));
    g_instance_resource_ctor = instance_resource_ctor;
    token = sh_resource_instances_construct(instance_object, 1);
    CHECK(sh_resource_instances_assign(instance_object, token, (uintptr_t)(instance_object + 8), a, original));
    CHECK(rg_instance_resource_ctor_hook(a) == a && !sh_resource_instances_lookup(a));
    CHECK(instance_leases == 1); sh_resource_instances_destroyed(instance_object, token, 1); CHECK(instance_leases == 0);
    CHECK(sh_resource_instances_publish(a, original)); g_instance_resource_destroy = instance_resource_destroy;
    rg_instance_resource_destroy_hook(a); CHECK(!sh_resource_instances_lookup(a));
    CHECK(sh_resource_instances_publish(a, original)); g_instance_resource_delete = instance_resource_delete;
    CHECK(rg_instance_resource_delete_hook(a, 1) == a && !sh_resource_instances_lookup(a));
    sh_resource_instances_origin_release(original);
    sh_resource_instances_destroyed(b,sh_resource_instances_token(b),1);
    CHECK(instance_captures == 0 && instance_leases == 0);
    size_t resources, instances, fields; sh_resource_instances_counts(&resources, &instances, &fields);
    CHECK(!resources && !instances && !fields);
    g_instance_ready = 0; owner_renderer = -1; owner_pinned = 0; sh_resource_graph_test_reset();
}
static int return_notifications,return_native_calls,return_throws,return_native_throws;
static void *return_expected;
static void return_observer(void *resource,uintptr_t caller)
{
    CHECK(resource==return_expected && caller);
    CHECK(sh_resource_graph_source_active("md6def","return-parent"));
    return_notifications++;
    if(return_throws)RaiseException(0xe0137022,0,0,NULL);
    sh_resource_graph_file("actual-return-input");
}
static void return_other(void *resource,uintptr_t caller) { (void)resource;(void)caller; }
static void return_load(void *resource)
{
    CHECK(resource==return_expected);return_native_calls++;
    if(return_native_throws)RaiseException(0xe0137023,0,0,NULL);
}
static void *return_make(void *mgr,const char *name,unsigned char create,unsigned char flags)
{
    (void)mgr;CHECK(!strcmp(name,"mesh") && create==1 && flags==3);
    return_native_calls++;if(return_native_throws)RaiseException(0xe0137023,0,0,NULL);
    return return_expected;
}
static void test_return_observation(void)
{
    unsigned char resource[0x60]={0},mgr[16]={0},load_target[20]={0},make_target[20]={0};
    sh_resource_graph_frame parent;
    put_pointer(mgr,8,"basemodel");put_pointer(resource,8,"mesh");put_pointer(resource,0x10,mgr);
    return_expected=resource;g_load=return_load;g_materialize=return_make;g_ready=1;g_instance_ready=0;
    g_return_observer=NULL;return_notifications=return_native_calls=0;
    CHECK(!sh_resource_graph_native_set_return_observer(NULL));
    CHECK(sh_resource_graph_native_set_return_observer(return_observer));
    CHECK(sh_resource_graph_native_set_return_observer(return_observer));
    CHECK(!sh_resource_graph_native_set_return_observer(return_other));
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    memcpy(load_target,jump,6);memcpy(make_target,jump,6);
    put_pointer(load_target,6,(void *)rg_load_hook);put_pointer(make_target,6,(void *)rg_materialize_hook);
    g_load_target=load_target;g_materialize_target=make_target;owner_renderer=1;owner_pinned=1;
    CHECK(sh_resource_graph_native_return_observer_ready(return_observer));
    load_target[6]^=1;CHECK(!sh_resource_graph_native_return_observer_ready(return_observer));load_target[6]^=1;
    make_target[0]^=1;CHECK(!sh_resource_graph_native_return_observer_ready(return_observer));make_target[0]^=1;
    owner_pinned=0;CHECK(!sh_resource_graph_native_return_observer_ready(return_observer));owner_pinned=1;
    for(int route=0;route<2;route++) {
        sh_resource_graph_begin(&parent,"md6def","return-parent");
        if(route)CHECK(rg_materialize_hook(mgr,"mesh",1,3)==resource);else rg_load_hook(resource);
        CHECK(return_notifications==route+1 && return_native_calls==route+1);
        sh_resource_graph_end(&parent,1);
    }
    sh_resource_graph_begin(&parent,"md6def","return-parent");return_throws=1;
    CHECK(rg_materialize_hook(mgr,"mesh",1,3)==resource);CHECK(return_notifications==3);
    return_throws=0;sh_resource_graph_end(&parent,1);
    for(int route=0;route<2;route++) {
        int caught=0;return_native_throws=1;
        sh_resource_graph_begin(&parent,"md6def","return-parent");
        __try {if(route)rg_materialize_hook(mgr,"mesh",1,3);else rg_load_hook(resource);}
        __except(GetExceptionCode()==0xe0137023?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
        CHECK(caught && return_notifications==3);
        return_native_throws=0;sh_resource_graph_end(&parent,1);
    }
    rg_load_hook(resource);CHECK(return_notifications==3); /* No parent consumer. */
    g_return_observer=NULL;g_load_target=g_materialize_target=NULL;g_load=NULL;g_materialize=NULL;
    g_ready=0;owner_renderer=-1;owner_pinned=0;sh_resource_graph_test_reset();
}
/* Actual late adapter + graph + instance composition. Native game method bodies
 * are explicit shims; exact whole paired method bodies have their own PE rows. */
static void *late_native_ctor(void *,void *);
static uint64_t late_native_water(void *,void *);
static void late_native_post(void *,unsigned char);
static struct {void *target,*detour,*tramp;size_t stolen;} late_patches[3];
static unsigned late_prepares;
static unsigned char *late_model,*late_backing,*late_material,*late_image,*late_file;
static unsigned char late_rows[0x180],late_rectangles[0x28],late_memory[0x180],late_bytes[16];
static int late_mode,late_calls;
static void *late_prepare(void *target,void *detour,size_t stolen)
{
    if(late_prepares==3)return NULL;unsigned i=late_prepares++;
    void *tramp=i==0?(void *)late_native_ctor:i==1?(void *)late_native_water:(void *)late_native_post;
    late_patches[i].target=target;late_patches[i].detour=detour;late_patches[i].tramp=tramp;late_patches[i].stolen=stolen;return tramp;
}
static void late_jump(void *target,void *detour,size_t stolen)
{
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};memcpy(target,jump,6);put_pointer(target,6,detour);
    memset((unsigned char *)target+14,0x90,stolen-14);
}
static sh_patch_status late_commit(void *trampoline)
{
    for(unsigned i=0;i<late_prepares;i++)if(late_patches[i].tramp==trampoline) {
        late_jump(late_patches[i].target,late_patches[i].detour,late_patches[i].stolen);return B2_PATCH_OK;
    }return B2_PATCH_REFUSED_BADARG;
}
static int late_getter(void *material)
{
    CHECK(material==late_material);rg_size_lookup(late_image);return 64;
}
static void late_native_post(void *model,unsigned char bounds)
{
    (void)model;CHECK(bounds==1);late_calls++;
}
static void late_build(void *model)
{
    CHECK(model==late_model);late_calls++;CHECK(rg_material_width_hook(late_material)==64);
    lm_post_hook(model,1);
    if(late_mode==1)RaiseException(0xe1350033,0,0,NULL);
    if(late_mode==2)late_rectangles[0]^=1;
}
static void *late_native_ctor(void *model,void *name)
{
    CHECK(model==late_model && name==(void *)0x1234);late_calls++;
    CHECK(rg_instance_model_base_hook(model)==model);
    const lm_profile *p=lm_profiles+owner_renderer;
    rg_return_observed(late_backing,(uintptr_t)late_host_base+p->decl_sites[0]);
    put_pointer(model,p->hot_field,late_backing);rg_hotbox_build_hook(model);return model;
}
static uint64_t late_native_water(void *model,void *path)
{
    CHECK(model==late_model && path==(void *)0x5678);late_calls++;
    const lm_profile *p=lm_profiles+owner_renderer;
    rg_return_observed(late_file,(uintptr_t)late_host_base+p->file_site);
    rg_return_observed(late_material,(uintptr_t)late_host_base+p->decl_sites[1]);
    put_pointer(model,p->water_field,late_material);(*(int *)((unsigned char *)model+0xae8))++;
    lm_post_hook(model,1);return UINT64_C(0xabc0000000000001);
}
static void late_publish(void *resource,int generation)
{
    uint64_t token=sh_resource_instances_token(resource);
    if(!token)token=sh_resource_instances_construct(resource,0);
    sh_resource_instance_origin *origin=sh_resource_instances_origin(instance_capture(generation),0);CHECK(origin);
    CHECK(sh_resource_instances_origin_source(origin,resource,token));CHECK(sh_resource_instances_publish(resource,origin));
    sh_resource_instances_origin_release(origin);
}
static int late_capture_bits(void *context,const void *capture)
{
    unsigned *bits=context;int generation=((const sh_package_resource_capture *)capture)->generation;
    CHECK(generation>=1 && generation<=6);if(generation>=1 && generation<=6)*bits|=1u<<generation;return 1;
}
static unsigned late_bits(void *model)
{
    unsigned bits=0;sh_resource_instance_origin *origin=sh_resource_instances_lookup(model);
    CHECK(origin);if(origin)CHECK(sh_resource_instances_origin_captures(origin,late_capture_bits,&bits));
    sh_resource_instances_origin_release(origin);return bits;
}
static void late_bind_shared(int renderer,unsigned char *base)
{
    g_load=return_load;g_lookup=(rg_lookup_fn)return_other;g_materialize=return_make;g_optional_material=(rg_optional_material_fn)return_other;
    g_hotbox_build=late_build;g_hotbox_destroy=instance_model_destructor;g_material_height=g_material_width=late_getter;
    g_instance_model_base=instance_resource_ctor;g_instance_model_destroy=instance_model_destructor;g_instance_destroy=(rg_instance_destroy_fn)return_other;
    void **targets[]={&g_load_target,&g_lookup_target,&g_materialize_target,&g_optional_material_target,
        &g_late_hot_build_target,&g_late_hot_destroy_target,&g_late_height_target,&g_late_width_target};
    void *trampolines[]={(void *)g_load,(void *)g_lookup,(void *)g_materialize,(void *)g_optional_material,
        (void *)g_hotbox_build,(void *)g_hotbox_destroy,(void *)g_material_height,(void *)g_material_width};
    void *detours[]={rg_load_hook,rg_lookup_hook,rg_materialize_hook,rg_optional_material_hook,
        rg_hotbox_build_hook,rg_hotbox_destroy_hook,rg_material_height_hook,rg_material_width_hook};
    const unsigned lengths[]={20,19,19,20,19,15,15,15};
    const lm_profile *p=lm_profiles+renderer;
    const uint32_t rvas[]={0x1000,0x1100,0x1200,renderer?0x17b36f0:0x17a61b0,p->hot_build,
        renderer?0x1745d00:0x17389a0,0x1400,0x1500};
    for(unsigned i=0;i<8;i++){*targets[i]=base+rvas[i];late_jump(*targets[i],detours[i],lengths[i]);}
    const uint32_t life[2][3]={{0x1734780,0x1734a90,0x9d4b90},{0x1741ae0,0x1741df0,0x9d53d0}};
    void *life_detours[]={rg_instance_model_base_hook,rg_instance_model_destroy_hook,rg_instance_destroy_hook};
    const unsigned life_lengths[]={21,19,15};
    for(unsigned i=0;i<3;i++)late_jump(base+life[renderer][i],life_detours[i],life_lengths[i]);
}
static void test_late_model_composition(void)
{
    /* The earlier instance fixture binds this one-shot production core. */
    for(int renderer=0;renderer<2;renderer++) {
        unsigned char model[0x1200]={0},backing[0x400]={0},material[0x400]={0},image_resource[0x400]={0},file[0x80]={0},old[0x80]={0};
        unsigned char managers[5][16]={{0}};void *resources[]={backing,material,image_resource,file,old};
        const char *families[]={"hotspotimagedata","material","image","file","material"};
        late_model=model;late_backing=backing;late_material=material;late_image=image_resource;late_file=file;
        late_mode=late_calls=0;owner_renderer=renderer;owner_pinned=1;g_ready=g_instance_ready=1;instance_allowed=1;
        late_host_base=VirtualAlloc(NULL,0x14000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(late_host_base);if(!late_host_base)return;
        const lm_profile *p=lm_profiles+renderer;
        /* Pins are digest records (resource_late_model_read_pins.h): their bytes come from the local image. */
        for(size_t i=0;i<p->pin_count;i++)CHECK(sh_native_pin_fixture_copy(renderer,p->pins[i].rva,(void *)(late_host_base+p->pins[i].rva),p->pins[i].length));
        late_bind_shared(renderer,(unsigned char *)late_host_base);CHECK(rg_late_shared_ready());
        void *targets[]={g_load_target,g_lookup_target,g_materialize_target,g_optional_material_target,g_late_hot_build_target,
            g_late_hot_destroy_target,g_late_height_target,g_late_width_target};
        const unsigned lengths[]={20,19,19,20,19,15,15,15};
        for(unsigned i=0;i<8;i++)for(unsigned j=0;j<lengths[i];j++) {
            ((unsigned char *)targets[i])[j]^=1;CHECK(!rg_late_shared_ready());((unsigned char *)targets[i])[j]^=1;
        }
        for(unsigned i=0;i<5;i++) {
            put_pointer(resources[i],8,"late/composite");put_pointer(resources[i],0x10,managers[i]);put_pointer(managers[i],8,(void *)families[i]);
            late_publish(resources[i],(int)i+1);
        }
        memset(late_memory,0,sizeof late_memory);memset(late_bytes,0,sizeof late_bytes);memset(late_rectangles,0,sizeof late_rectangles);memset(late_rows,0,sizeof late_rows);
        put_pointer(backing,0x68,material);put_pointer(backing,0x70,late_rectangles);*(int *)(backing+0x78)=2;
        put_pointer(material,0x188,image_resource);put_pointer(model,0xae0,late_rows);
        const unsigned slots[]={0,0x28,0x58,0xe0};for(unsigned i=0;i<4;i++)put_pointer((void *)(late_host_base+p->memory_table),slots[i],(void *)(late_host_base+p->memory_methods[i]));
        put_pointer(file,0x38,late_memory);put_pointer(late_memory,0,(void *)(late_host_base+p->memory_table));put_pointer(late_memory,0x168,late_bytes);*(uint64_t *)(late_memory+0x150)=sizeof late_bytes;
        lm_attempted=lm_ready=0;late_prepares=0;late_install_fake=1;rg_late_install();late_install_fake=0;CHECK(sh_resource_late_model_reads_ready());
        CHECK(lm_ctor_hook(model,(void *)0x1234)==model);uint64_t token=sh_resource_instances_token(model);
        CHECK(late_bits(model)==((1u<<1)|(1u<<2)|(1u<<3)));CHECK(!sh_resource_instances_complete(model,token));
        CHECK(lm_bindings && !sh_resource_instances_faulted(model,token));
        /* Neither nested SOURCE nor an independent owner is this scope. */
        sh_resource_late_model_scope pending=rg_late_begin(model,SH_LATE_MODEL_HOT_REBUILD);CHECK(rg_late_visible(pending));
        rg_instance_scope source=rg_instance_source_begin(old,1);CHECK(!rg_late_visible(pending));
        sh_resource_late_model_returned(old,(uintptr_t)late_host_base+p->decl_sites[0]);rg_instance_source_end(source,0,0);
        rg_instance_scope owner=rg_instance_begin(RG_INSTANCE_OWNER,old,sh_resource_instances_token(old));CHECK(!rg_late_visible(pending));
        rg_instance_frame_free(rg_instance_take(owner));CHECK(rg_late_visible(pending));
        CHECK(!rg_late_input(pending,NULL,0,NULL));rg_late_end(pending,model,1,0);CHECK(sh_resource_instances_faulted(model,token));
        /* Reconstruct a fresh lifetime for the independent Water append path. */
        rg_instance_model_destroy_hook(model);CHECK(!lm_bindings);
        memset(model,0,sizeof model);put_pointer(model,0xae0,late_rows);rg_instance_model_base_hook(model);token=sh_resource_instances_token(model);
        CHECK(lm_water_hook(model,(void *)0x5678)==UINT64_C(0xabc0000000000001));CHECK(late_bits(model)==((1u<<2)|(1u<<4)));
        sh_resource_instance_origin *prior=sh_resource_instances_lookup(old);CHECK(sh_resource_instances_payload(model,token,prior));sh_resource_instances_origin_release(prior);
        *(int *)(model+0xae8)=1;
        CHECK(lm_water_hook(model,(void *)0x5678)==UINT64_C(0xabc0000000000001));CHECK(late_bits(model)==((1u<<2)|(1u<<4)|(1u<<5)));
        /* Old source is retained as anonymous payload, never model->model. */
        CHECK(sh_resource_instances_payload_uses(model,token,derived_contains,old));
        rg_instance_model_destroy_hook(model);CHECK(!lm_bindings);
        memset(model,0,sizeof model);put_pointer(model,0xae0,late_rows);rg_instance_model_base_hook(model);token=sh_resource_instances_token(model);*(int *)(model+0xae8)=1;
        CHECK(lm_water_hook(model,(void *)0x5678)==UINT64_C(0xabc0000000000001));CHECK(!sh_resource_instances_lookup(model) && sh_resource_instances_faulted(model,token));
        rg_instance_model_destroy_hook(model);
        /* Native exception and source mutation retain/fault actual inputs. */
        for(int mode=1;mode<=2;mode++) {
            memset(model,0,sizeof model);put_pointer(model,0xae0,late_rows);late_mode=mode;int caught=0;
            __try{CHECK(lm_ctor_hook(model,(void *)0x1234)==model);}__except(GetExceptionCode()==0xe1350033?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
            CHECK(caught==(mode==1));token=sh_resource_instances_token(model);CHECK(!sh_resource_instances_lookup(model) && sh_resource_instances_faulted(model,token));
            instance_model_destroy_throw=1;__try{rg_instance_model_destroy_hook(model);CHECK(0);}__except(GetExceptionCode()==0xe0000069?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){}
            CHECK(sh_resource_instances_token(model)==token);instance_model_destroy_throw=0;
            /* Explicit completion fixture, never replay a partial native dtor. */
            sh_resource_late_model_destroyed(model,token,1);sh_resource_instances_destroyed(model,token,1);
        }
        for(unsigned i=0;i<5;i++){sh_resource_instances_forget(resources[i]);sh_resource_instances_destroyed(resources[i],sh_resource_instances_token(resources[i]),1);}
        lm_retire(sh_execution_context(NULL,0));rg_instance_retire(sh_execution_context(NULL,0));CHECK(!instance_captures && !instance_leases);
        VirtualFree((void *)late_host_base,0,MEM_RELEASE);late_host_base=NULL;lm_ready=0;g_instance_ready=g_ready=0;
    }
    owner_renderer=-1;owner_pinned=0;sh_resource_graph_test_reset();
}

static int find_notifications,find_native_calls,find_throws,find_observer_throws;
static void find_observer(void *resource,uintptr_t caller)
{
    CHECK(resource==return_expected && caller && sh_resource_graph_source_active("md6def","return-parent"));
    find_notifications++;SetLastError(0x76543210);
    if(find_observer_throws)RaiseException(0xe1350044,0,0,NULL);
}
static void *find_native(void *manager,const char *name,unsigned char allow)
{
    CHECK(manager && !strcmp(name,"mesh") && allow==7);find_native_calls++;
    if(find_throws)RaiseException(0xe1350045,0,0,NULL);
    SetLastError(0x12345678);return return_expected;
}
static void test_find_return_observation(void)
{
    unsigned char resource[0x60]={0},manager[0x20]={0},target[20]={0};
    put_pointer(resource,8,"mesh");put_pointer(resource,0x10,manager);put_pointer(manager,8,"material");
    return_expected=resource;g_optional_material=find_native;g_ready=1;g_instance_ready=0;
    g_find_return_observer=NULL;find_notifications=find_native_calls=find_throws=find_observer_throws=0;
    CHECK(!sh_resource_graph_native_set_find_return_observer(NULL));
    CHECK(sh_resource_graph_native_set_find_return_observer(find_observer));
    CHECK(sh_resource_graph_native_set_find_return_observer(find_observer));
    CHECK(!sh_resource_graph_native_set_find_return_observer(return_other));
    late_jump(target,rg_optional_material_hook,20);g_optional_material_target=target;owner_renderer=1;owner_pinned=1;
    CHECK(sh_resource_graph_native_find_return_observer_ready(find_observer));
    for(unsigned i=0;i<20;i++){target[i]^=1;CHECK(!sh_resource_graph_native_find_return_observer_ready(find_observer));target[i]^=1;}
    CHECK(!sh_resource_graph_native_find_return_observer_ready(return_other));
    owner_pinned=0;CHECK(!sh_resource_graph_native_find_return_observer_ready(find_observer));owner_pinned=1;
    sh_resource_graph_frame parent;
    for(int pass=0;pass<3;pass++) {
        return_expected=pass==1?NULL:resource;find_observer_throws=pass==2;
        sh_resource_graph_begin(&parent,"md6def","return-parent");
        CHECK(rg_optional_material_hook(manager,"mesh",7)==return_expected);CHECK(GetLastError()==0x12345678);
        CHECK(find_notifications==pass+1 && find_native_calls==pass+1);sh_resource_graph_end(&parent,1);
    }
    find_observer_throws=0;find_throws=1;int caught=0;
    sh_resource_graph_begin(&parent,"md6def","return-parent");
    __try{rg_optional_material_hook(manager,"mesh",7);}__except(GetExceptionCode()==0xe1350045?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
    CHECK(caught && find_notifications==3);find_throws=0;sh_resource_graph_end(&parent,1);
    /* Routed plan returns the held actual object without entering native Find. */
    sh_package_consumer_target binding={"font/material/48","material","private/mapped"};
    sh_package_consumer font={"font","private/test",&binding,1};rg_font_frame route={0};
    route.hold.binding=&font;route.names[0]="private/mapped";route.materials[0]=resource;g_font_frame=&route;
    sh_resource_graph_begin(&parent,"md6def","return-parent");return_expected=resource;
    CHECK(rg_optional_material_hook(manager,"fonts/private/test/48.tga",7)==resource);
    CHECK(find_notifications==4 && find_native_calls==4 && g_font_frame==&route && !route.failed);
    sh_resource_graph_end(&parent,1);g_font_frame=NULL;
    CHECK(rg_optional_material_hook(manager,"mesh",7)==resource && find_notifications==4);
    g_find_return_observer=NULL;g_optional_material=NULL;g_optional_material_target=NULL;
    g_ready=0;owner_renderer=-1;owner_pinned=0;sh_resource_graph_test_reset();
}

int main(void)
{
    sh_resource_graph_frame outer, pause;
    unsigned bits;
    int caught = 0;
    size_history_checks();
    owner_hook_checks();
    getter_checks();
    put_pointer(entity, 0x6d0, definition);
    put_pointer(definition, 8, "example");
    put_pointer(definition, 0x10, manager);
    put_pointer(manager, 8, "entityDef");
    put_pointer(definition, 0x140, canonical_text);
    put_pointer(reader_data, 0x1160, canonical_text);
    g_state = fake_state; g_ready = 1;
    seed("entitydef", "example");
    CHECK(!walk("entitydef", "example", &bits));
    invoke(entity, reader_data, 1);
    CHECK(calls == 1 && walk("entitydef", "example", &bits) && bits == 3);

    /* Same entity, different map text must not replace the canonical graph. */
    put_pointer(reader_data, 0x1160, "a per-map edit");
    sh_resource_graph_begin(&outer, "material", "surrounding-parser");
    sh_resource_graph_file("source");
    invoke(entity, reader_data, 1);
    CHECK(sh_resource_graph_recording());
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "surrounding-parser", &bits) && bits == 1);
    CHECK(walk("entitydef", "example", &bits) && bits == 3);

    seed("entitydef", "example");
    put_pointer(reader_data, 0x1160, canonical_text);
    invoke(entity, reader_data, 0); /* Partial-update mode is not initialization. */
    CHECK(!walk("entitydef", "example", &bits));
    invoke((void *)1, reader_data, 1); /* Failed identity read is a call-through. */
    invoke(entity, (void *)1, 1);
    CHECK(calls == 5 && !sh_resource_graph_recording());
    CHECK(!walk("entitydef", "example", &bits));

    reader_data[0x1200] = 1;
    invoke(entity, reader_data, 1);
    CHECK(!walk("entitydef", "example", &bits));
    reader_data[0x1200] = 0;
    throw_error = 1;
    __try { invoke(entity, reader_data, 1); }
    __except (GetExceptionCode() == 0xe0123456 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && !sh_resource_graph_recording());
    CHECK(!walk("entitydef", "example", &bits));
    throw_error = 0;
    invoke(entity, reader_data, 1);
    CHECK(calls == 8 && walk("entitydef", "example", &bits) && bits == 3);
    seed("entitydef", "parent");
    sh_resource_graph_begin(&outer, "entitydef", "example");
    sh_resource_graph_reference("entitydef", "parent");
    sh_resource_graph_end(&outer, 1);
    definition[0x128] = 1;
    invoke(entity, reader_data, 1);
    CHECK(walk("entitydef", "example", &bits) && bits == 3);
    definition[0x128] = 0;
    invoke(entity, reader_data, 1);
    CHECK(!walk("entitydef", "example", &bits));
    sh_resource_graph_test_reset();

    g_parse = fake_parse; expected_resource = definition;
    put_pointer(manager, 8, "material");
    CHECK(rg_parse_hook(definition, 7, (void *)19) == 0);
    CHECK(walk("material", "example", &bits) && bits == 2);

    /* A generic load delegating to the direct parser has one source scope.
     * In particular, the file opened before delegation must not be lost. */
    sh_resource_graph_begin(&outer, "MATERIAL", "EXAMPLE");
    sh_resource_graph_file("source");
    CHECK(sh_resource_graph_source_active("material", "example"));
    CHECK(!sh_resource_graph_source_active("material", "different"));
    CHECK(rg_parse_hook(definition, 7, (void *)19) == 0);
    CHECK(sh_resource_graph_source_active("material", "example"));
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "example", &bits) && bits == 3);

    /* A direct failure cannot retain the previous successful source record. */
    parse_result = 5;
    CHECK(rg_parse_hook(definition, 7, (void *)19) == 5);
    CHECK(!walk("material", "example", &bits));
    parse_result = 0; throw_error = 1; caught = 0;
    __try { rg_parse_hook(definition, 7, (void *)19); }
    __except (GetExceptionCode() == 0xe0123456 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && !sh_resource_graph_recording());
    CHECK(!walk("material", "example", &bits));
    throw_error = 0;

    /* Temporary decls lacking a manager must not add their files to a parent. */
    put_pointer(definition, 0x10, NULL);
    sh_resource_graph_begin(&outer, "material", "temporary-parent");
    sh_resource_graph_file("source");
    rg_parse_hook(definition, 7, (void *)19);
    CHECK(sh_resource_graph_source_active("material", "temporary-parent"));
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "temporary-parent", &bits) && bits == 1);
    put_pointer(definition, 0x10, manager);

    /* An instance barrier hides outer source scopes, but permits an
     * independently loaded canonical dependency to acquire its own scope. */
    sh_resource_graph_begin(&outer, "material", "barrier-parent");
    sh_resource_graph_file("source");
    sh_resource_graph_pause(&pause);
    CHECK(!sh_resource_graph_source_active("material", "barrier-parent"));
    rg_parse_hook(definition, 7, (void *)19);
    CHECK(!sh_resource_graph_recording());
    sh_resource_graph_end(&pause, 0);
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "barrier-parent", &bits) && bits == 1);
    CHECK(walk("material", "example", &bits) && bits == 2);

    put_pointer(manager, 8, "entityDef");
    seed("entitydef", "example");
    invoke(entity, reader_data, 1);
    CHECK(walk("entitydef", "example", &bits));
    rg_parse_hook(definition, 7, (void *)19);
    CHECK(!walk("entitydef", "example", &bits)); /* State must be consumed again. */
    invoke(entity, reader_data, 1);
    CHECK(walk("entitydef", "example", &bits) && bits == 2);
    sh_resource_graph_test_reset();
    test_production_read();
    test_shader_receipts();
    test_shader_preparation();
    test_program_preparation();
    test_shader_state();
    test_deferred_registration_shader_window();
    test_isolated_program_cook();
    test_font_construction();
    test_movie_native_names();
    test_movie_construction();
    test_movie_late_callbacks();
    test_movie_font_identity();
    test_movie_fontfx_registration();
    test_movie_hook_rollback();
    test_constructor_requests();
    test_lookup_requests();
    test_default_routes();
    test_editor_storage_scope();
    test_editor_storage();
    test_editor_reconstruct_header();
    test_editor_header_reset_scope();
    test_editor_header_untouched_rejection();
    test_editor_native_inline_storage();
    test_instance_native_observation();
    test_return_observation();
    test_find_return_observation();
    test_late_model_composition();
    if (failures) return 1;
    puts("canonical entity-state, direct source and production read capture checks passed"); return 0;
}
