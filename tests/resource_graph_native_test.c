#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define SH_RESOURCE_EDITOR_TESTING
#include "../src/backend/resource_graph_native.c"

static int failures, calls, throw_error;
static int test_vulkan;
int sh_host_is_vulkan(void) { return test_vulkan; }
const uint8_t *sh_host_image_base(void) { return NULL; }
int sh_host_is_pinned_rva_build(void) { return 0; }
int sh_host_is_pinned_renderer_build(int vulkan) { (void)vulkan; return 0; }
void sh_resource_resident_touch(void *resource) { (void)resource; }
static void *expected_entity, *expected_reader;
static unsigned char expected_initialize;
static unsigned char entity[0x6d8], definition[0x168], manager[16], reader_data[0x1208];
static const char canonical_text[] = "edit = { model = body; }";
static unsigned int parse_result;
static void *expected_resource;
static unsigned int read_result;
static int read_delegates, read_noop, read_calls;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)

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
void *hook_prepare(void *target, void *detour, size_t stolen)
{
    (void)detour; (void)stolen;
    if (!movie_install_fake) return NULL;
    movie_hook_prepares++;
    return movie_hook_prepares == movie_hook_prepare_fault ? NULL : target;
}
sh_patch_status hook_commit(void *trampoline)
{
    (void)trampoline; if (!movie_install_fake) return B2_PATCH_OK; movie_hook_commits++;
    CHECK(g_swf_construct && g_swf_destroy && g_swf_precache_sound && g_swf_precache_material && g_swf_play_sound && g_swf_local_string && g_dictionary_index);
    return movie_hook_commits == movie_hook_commit_fault ? (sh_patch_status)1 : B2_PATCH_OK;
}
int hook_unpatch(void *trampoline)
{ (void)trampoline; movie_hook_removes++; return movie_hook_removes != movie_hook_remove_fault; }
void backend_log(const char *message) { (void)message; }

static void put_pointer(unsigned char *buffer, size_t offset, const void *value)
{ memcpy(buffer + offset, &value, sizeof(value)); }
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
    static sh_package_movie_names names;
    put_pointer(movie_swf, 0, name && sh_package_movie_names_for_consumer(name, strlen(name), &names) == 1 ? names.movie : "");
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
    sh_package_movie_names names; CHECK(!parent && flags == 3);
    size_t index = swf == late_movies[0] ? 0 : 1;
    CHECK(sh_package_movie_names_for_consumer(name, strlen(name), &names) == 1);
    strcpy_s(late_filenames[index], sizeof(late_filenames[index]), names.movie);
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
    strcpy_s(late_filenames[2], sizeof(late_filenames[2]), "generated/swf/stock.bswf"); put_pointer(late_movies[2], 0, late_filenames[2]);
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
static void test_movie_hook_rollback(void)
{
    static unsigned char targets[7][0x80]; sig_result results[7] = {0};
    for (size_t i = 0; i < 7; i++) { results[i].name = g_movie_required[i]; results[i].status = SIG_OK; results[i].addr = (uintptr_t)targets[i]; }
    targets[5][0x59] = 0x48; targets[5][0x5a] = 0x8d; targets[5][0x5b] = 0x0d; targets[5][0x60] = 0xe8;
    int32_t displacement = (int32_t)((intptr_t)&late_dictionary - (intptr_t)(targets[5] + 0x60));
    memcpy(targets[5] + 0x5c, &displacement, sizeof(displacement));
    CHECK(rg_movie_dictionary_for(targets[5]) == &late_dictionary);
    targets[5][0x59] = 0; CHECK(!rg_movie_dictionary_for(targets[5])); targets[5][0x59] = 0x48;
    movie_install_fake = 1; g_ready = 1;
    g_swf_construct = NULL; g_swf_destroy = NULL;
    g_swf_precache_sound = g_swf_precache_material = g_swf_play_sound = g_swf_local_string = NULL;
    g_dictionary_index = NULL;
    for (int pass = 0; pass < 3; pass++) {
        g_movie_ready = g_movie_attempted = 0;
        movie_hook_prepares = movie_hook_commits = movie_hook_removes = 0;
        movie_hook_prepare_fault = pass == 0 ? 4 : 0;
        movie_hook_commit_fault = pass ? 4 : 0; movie_hook_remove_fault = pass == 2 ? 1 : 0;
        rg_movie_install(results, 7);
        CHECK(!g_movie_ready && g_movie_attempted);
        CHECK(movie_hook_removes == (pass ? 7 : 3));
        CHECK(!g_swf_construct && !g_swf_destroy && !g_swf_precache_sound && !g_swf_precache_material && !g_swf_play_sound && !g_swf_local_string);
        CHECK((g_dictionary_index != NULL) == (pass == 2));
        g_dictionary_index = NULL; /* Fake removal recovery; no executable patch exists. */
    }
    movie_hook_prepare_fault = movie_hook_commit_fault = movie_hook_remove_fault = 0;
    g_movie_ready = g_movie_attempted = 0; movie_hook_prepares = movie_hook_commits = movie_hook_removes = 0;
    results[4].status = SIG_NOT_FOUND; rg_movie_install(results, 7); CHECK(!movie_hook_prepares && !g_movie_ready);
    results[4].status = SIG_OK; rg_movie_install(results, 7); CHECK(g_movie_ready && movie_hook_commits == 7 && !movie_hook_removes);
    g_movie_ready = 0; movie_install_fake = 0;
    g_swf_construct = NULL; g_swf_destroy = NULL; g_swf_precache_sound = g_swf_precache_material = g_swf_play_sound = g_swf_local_string = NULL;
    g_dictionary_index = NULL; g_movie_dictionary = NULL;
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

int main(void)
{
    sh_resource_graph_frame outer, pause;
    unsigned bits;
    int caught = 0;
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
    test_isolated_program_cook();
    test_font_construction();
    test_movie_construction();
    test_movie_late_callbacks();
    test_movie_hook_rollback();
    test_constructor_requests();
    test_editor_storage_scope();
    test_editor_storage();
    test_editor_reconstruct_header();
    test_editor_header_reset_scope();
    test_editor_header_untouched_rejection();
    test_editor_native_inline_storage();
    if (failures) return 1;
    puts("canonical entity-state, direct source and production read capture checks passed"); return 0;
}
