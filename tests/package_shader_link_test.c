/* Synthetic shader units through the production source reader and linker. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "package_source_link.h"
#include "package_native_assets.h"
#include "package_shader_receipt.h"
#include "native_resource_id.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "virtual_material_runtime.h"
sh_virtual_material_view *sh_virtual_material_runtime_prepare(
    const sh_virtual_material_runtime_binding *bindings, size_t count, char *error, size_t capacity)
{ (void)bindings; (void)count; (void)error; (void)capacity; assert(0); return NULL; }
int sh_virtual_material_runtime_retire(sh_virtual_material_view **view)
{ assert(!view || !*view); return 1; }

typedef struct bytes { unsigned char data[2048]; size_t length; } bytes;
static void raw(bytes *b, const void *data, size_t n)
{ assert(n <= sizeof(b->data) - b->length); memcpy(b->data + b->length, data, n); b->length += n; }
static void zeros(bytes *b, size_t n)
{ assert(n <= sizeof(b->data) - b->length); memset(b->data + b->length, 0, n); b->length += n; }
static void be(bytes *b, uint32_t n)
{ unsigned char p[4] = {(unsigned char)(n >> 24),(unsigned char)(n >> 16),(unsigned char)(n >> 8),(unsigned char)n}; raw(b,p,4); }
static void le(bytes *b, uint32_t n)
{ unsigned char p[4] = {(unsigned char)n,(unsigned char)(n >> 8),(unsigned char)(n >> 16),(unsigned char)(n >> 24)}; raw(b,p,4); }
static void str(bytes *b, const char *s) { le(b,(uint32_t)strlen(s)); raw(b,s,strlen(s)); }
static const char declaration[] = "{ parms { tex \"textures/source\" } hlsl_vp { // opaque\n void main(){} } }\n";
static const char parameter[] = "{ Tex2D textures/source }";
static const char flag_source[] = "{ fixed_flag_fixture }";
static const char vertex[] = "#version 450\nvoid main(){}";
static bytes program(int permutations)
{
    bytes b = {0};
    be(&b,0xb68800ad); be(&b,sh_package_shader_declaration_checksum((const unsigned char *)declaration,strlen(declaration)));
    zeros(&b,4); str(&b,""); zeros(&b,9); be(&b,1);
    zeros(&b,2); str(&b,"tex"); str(&b,"textures/source");
    for (unsigned i=0;i<21;i++) { be(&b,i ? 0 : 1); if (!i) str(&b,"tex"); }
    be(&b,1); str(&b,"decls/renderprogs/common.inc"); be(&b,123);
    be(&b,1); str(&b,"feature"); zeros(&b,8); be(&b,permutations ? 1 : 0);
    if (permutations) { be(&b,0); be(&b,1); }
    zeros(&b,192); zeros(&b,10); str(&b,vertex); zeros(&b,10); str(&b,""); zeros(&b,10); str(&b,""); zeros(&b,36);
    return b;
}
static bytes module(void)
{
    bytes b={0}; be(&b,4); be(&b,sh_package_shader_checksum((const unsigned char *)vertex,strlen(vertex)));
    be(&b,0); be(&b,20); le(&b,0x07230203); le(&b,0x10000); le(&b,0); le(&b,1); le(&b,0); return b;
}
typedef struct region { unsigned char *body; size_t length; } region;
typedef struct fixture {
    region regions[64]; size_t region_count;
    uintptr_t registry, reflection;
    sh_package_compilation compiled;
    sh_compiled_resource files[20];
    int deny, kind, index, mutate, calls, admit_calls, absent, read_failure, flag_deny, flag_mutate, flag_calls;
    int campaign, stale, include_status;
} fixture;
static unsigned char *memory(fixture *f, size_t length)
{
    unsigned char *p=calloc(1,length); assert(p && f->region_count<64);
    f->regions[f->region_count++]=(region){p,length}; return p;
}
static void ptr(unsigned char *p,size_t at,const void *v) { memcpy(p+at,&v,sizeof(v)); }
static void num(unsigned char *p,size_t at,int v) { memcpy(p+at,&v,4); }
static int read_memory(void *context,uintptr_t address,void *out,size_t length)
{
    fixture *f=context;
    for(size_t i=0;i<f->region_count;i++) {
        uintptr_t base=(uintptr_t)f->regions[i].body;
        if(address>=base && address-base<=f->regions[i].length && length<=f->regions[i].length-(size_t)(address-base)) {
            memcpy(out,(const void *)address,length); return 1;
        }
    }
    return 0;
}
static int order(const void *a,const void *b)
{ return strcmp(((const sh_compiled_resource *)a)->engine_path,((const sh_compiled_resource *)b)->engine_path); }
static void add(fixture *f,const char *path,const char *family,const char *name,const void *body,size_t length)
{
    sh_compiled_resource *file=f->files+f->compiled.resource_count++;
    assert(f->compiled.resource_count<=20);
    *file=(sh_compiled_resource){0}; file->engine_path=_strdup(path);
    file->type=family ? _strdup(family) : NULL; file->name=name ? _strdup(name) : NULL;
    file->body=malloc(length+1); assert(file->engine_path && file->body);
    memcpy(file->body,body,length); file->body[length]=0; file->body_length=length; file->source=SIZE_MAX;
}
static void setup(fixture *f,int permutations)
{
    memset(f,0,sizeof(*f)); f->kind=1; f->index=17;
    f->registry=(uintptr_t)memory(f,0x18);
    unsigned char *reflection=memory(f,0xb0),*container=memory(f,0x30),*records=memory(f,2*0x38);
    unsigned char *objects=memory(f,32),*pointers=memory(f,32),*name=memory(f,7);
    memcpy(name,"idStr",6); ptr(reflection,0,container); ptr(container,0x20,records); ptr(records,0,name);
    ptr(reflection,0x88,objects); num(reflection,0x90,2); ptr(reflection,0xa0,pointers); num(reflection,0xa8,2);
    f->reflection=(uintptr_t)reflection; f->compiled.resources=f->files;
    bytes binary=program(permutations),spirv=module();
    add(f,"generated/decls/renderprog/fixture.decl","renderprog","fixture",declaration,strlen(declaration));
    add(f,"generated/renderprogs/fixture_pc_vulkan.bin",NULL,NULL,binary.data,binary.length);
    add(f,"generated/renderprogs/fixture_pc.bin",NULL,NULL,binary.data,binary.length);
    add(f,"generated/spirv/fixture.vspv",NULL,NULL,spirv.data,spirv.length);
    add(f,"generated/decls/renderparm/tex.decl","renderparm","tex",parameter,strlen(parameter));
    add(f,"generated/decls/renderprogflag/feature.decl","renderprogflag","feature",flag_source,strlen(flag_source));
    add(f,"generated/image/textures/source.bimage","image","textures/source","image",5);
    add(f,"decls/renderprogs/common.inc",NULL,NULL,"// retained include\n",20);
    if(permutations) {
        const char *name="generated/renderprogs/permutations/fixture_0000000000000001.decl";
        binary=program(0);
        add(f,name,NULL,NULL,declaration,strlen(declaration));
        add(f,"generated/renderprogs/permutations/fixture_0000000000000001_pc_vulkan.bin",NULL,NULL,binary.data,binary.length);
        add(f,"generated/renderprogs/permutations/fixture_0000000000000001_pc.bin",NULL,NULL,binary.data,binary.length);
        add(f,"generated/spirv/generated/renderprogs/permutations/fixture_0000000000000001.vspv",NULL,NULL,spirv.data,spirv.length);
    }
    qsort(f->files,f->compiled.resource_count,sizeof(*f->files),order);
}
static void clear(fixture *f)
{
    for(size_t i=0;i<f->compiled.resource_count;i++) {
        free((char *)f->files[i].engine_path);free(f->files[i].type);free(f->files[i].name);free(f->files[i].body);
    }
    for(size_t i=0;i<f->region_count;i++)free(f->regions[i].body);
}
static int abi(void *context,const char *name,sh_decl_source source,int *kind,uint32_t *index,char *error,size_t capacity)
{
    fixture *f=context; (void)error;(void)capacity; f->calls++;
    if(f->deny || strcmp(name,"tex") || !source.text || source.length!=strlen(parameter) || memcmp(source.text,parameter,source.length))return 0;
    *kind=f->kind;*index=(uint32_t)(f->index+(f->mutate ? f->calls : 0));return 1;
}
static int original(void *context,const char *path,unsigned char **body,size_t *length)
{
    fixture *f=context; *body=NULL;*length=0;
    if(f->read_failure)return -1;
    if(f->absent)return 0;
    const sh_compiled_resource *file=sh_package_compilation_find(&f->compiled,path);
    if(!file)return 0;
    *body=malloc(file->body_length+1);assert(*body);memcpy(*body,file->body,file->body_length+1);*length=file->body_length;
    if(f->stale && strstr(path,"_pc_vulkan.bin"))(*body)[0]^=1; /* An installed original cooked differently. */
    if (f->include_status && !strcmp(path,"decls/renderprogs/common.inc")) return f->include_status;
    return f->campaign ? 2 : 1;
}
static int flag_abi(void *context,const char *name,sh_decl_source source,uint64_t *mask,char *error,size_t capacity)
{
    fixture *f=context; (void)error; (void)capacity; f->flag_calls++;
    if(f->flag_deny || strcmp(name,"feature") || !source.text || source.length!=strlen(flag_source) ||
        memcmp(source.text,flag_source,source.length))return 0;
    *mask=UINT64_C(1)<<32;if(f->flag_mutate)*mask+=(uint64_t)f->flag_calls;return 1;
}
static int admit(void *context,size_t scope,const char *family,const char *name,unsigned char semantics[32],char *error,size_t capacity)
{ fixture *f=context;(void)scope;(void)family;(void)name;(void)error;(void)capacity;memset(semantics,0,32);f->admit_calls++;return 1; }
static int semantics(void *context,size_t scope,const char *family,const char *name,unsigned char value[32],char *error,size_t capacity)
{ (void)context;(void)scope;(void)family;(void)name;(void)error;(void)capacity;memset(value,0,32);return 1; }
static sh_package_source_link *open_link(fixture *f,sh_package_shader_renderer renderer,char *error)
{
    const sh_package_compilation *scope=&f->compiled;
    sh_package_source_shader shader={renderer,f,abi,flag_abi};
    sh_package_source_link_native native={f,admit,semantics,&shader};
    return sh_package_source_link_open(&scope,1,NULL,original,f,
        (sh_decl_registry_source){f,read_memory,f->registry,NULL},f->reflection,&native,error,512);
}
typedef struct binding { int change, parameter; size_t calls; } binding;
static int bind_fixture(void *context,const char *slot,const char *family,const char *name,const char **replacement)
{
    binding *b=context; assert(slot && name);b->calls++;*replacement=NULL;
    if(b->parameter && !strcmp(family,"renderparm")){*replacement="private/tex";return 1;}
    if(b->change && !strcmp(family,"image")){*replacement="private/texture";return 1;}return 0;
}
typedef struct path_check { int permutation,decl,bin,stage; } path_check;
static int paths(void *context,sh_package_shader_path_kind kind,const char *old,const char *linked)
{
    path_check *p=context;assert(old && linked);
    if(kind==SH_PACKAGE_SHADER_PERMUTATION){p->permutation++;assert(strstr(linked,"private/fixture_0000000000000001.decl"));}
    if(kind==SH_PACKAGE_SHADER_DECLARATION){p->decl++;assert(strstr(linked,"private/fixture"));}
    if(kind==SH_PACKAGE_SHADER_BINARY){p->bin++;assert(strstr(linked,"private/fixture"));}
    if(kind==SH_PACKAGE_SHADER_SPIRV){p->stage++;assert(strstr(linked,"private/fixture"));}
    if(kind==SH_PACKAGE_SHADER_SOURCE_INCLUDE)assert(!strcmp(old,linked));
    return 1;
}
static void complete_and_bind(sh_package_shader_renderer renderer)
{
    fixture f;setup(&f,1);char error[512];
    sh_package_source_link *s=open_link(&f,renderer,error);if(!s)fprintf(stderr,"%s\n",error);assert(s);
    const sh_package_link_inputs *inputs=sh_package_source_link_inputs(s);
    size_t unit=sh_package_link_inputs_find(inputs,0,"renderprog","fixture");assert(unit!=SIZE_MAX && inputs->count==4);
    const sh_package_link_unit *u=inputs->units+unit;assert(u->file_count==(renderer==SH_PACKAGE_SHADER_VULKAN ? 7u : 5u));
    path_check p={0};assert(sh_package_source_link_shader_paths(s,unit,"private/fixture",paths,&p,error,sizeof(error)));
    assert(p.permutation==1 && p.decl==2 && p.bin==2 && p.stage==(renderer==SH_PACKAGE_SHADER_VULKAN ? 2 : 0));
    unsigned char *changed_decl=NULL,*changed_bin=NULL;size_t decl_length=0,bin_length=0;
    for(size_t i=0;i<u->file_count;i++) {
        const sh_compiled_resource *file=u->files[i];size_t length=0;binding b={0};
        int companion = strstr(file->engine_path,".vspv") || strstr(file->engine_path,".inc");
        int abi_calls = f.calls, flag_calls = f.flag_calls;
        unsigned char *out=sh_package_source_link_bind_file(s,unit,file,file->body,file->body_length,bind_fixture,&b,&length,error,sizeof(error));
        assert(out && length==file->body_length && !memcmp(out,file->body,length));free(out);
        if(companion) {
            /* A frozen exact companion must not decode the owning program
             * again, but foreign or truncated bytes are still rejected. */
            assert(f.calls==abi_calls && f.flag_calls==flag_calls && !b.calls);
            unsigned char *changed=malloc(file->body_length);assert(changed && file->body_length);
            memcpy(changed,file->body,file->body_length);changed[file->body_length-1]^=1;
            assert(!sh_package_source_link_bind_file(s,unit,file,changed,file->body_length,
                bind_fixture,&b,&length,error,sizeof(error)));
            assert(strstr(error,"companion differs") && !length);
            assert(!sh_package_source_link_bind_file(s,unit,file,file->body,file->body_length-1,
                bind_fixture,&b,&length,error,sizeof(error)));
            assert(!length);free(changed);
        }
        b=(binding){1,0,0};out=sh_package_source_link_bind_file(s,unit,file,file->body,file->body_length,bind_fixture,&b,&length,error,sizeof(error));
        if(!out)fprintf(stderr,"bind %s: %s\n",file->engine_path,error);assert(out);
        if(!strcmp(file->engine_path,"generated/decls/renderprog/fixture.decl")){changed_decl=out;decl_length=length;assert(strstr((char *)out,"private/texture"));}
        else if(!strcmp(file->engine_path,renderer==SH_PACKAGE_SHADER_VULKAN ? "generated/renderprogs/fixture_pc_vulkan.bin" : "generated/renderprogs/fixture_pc.bin")){changed_bin=out;bin_length=length;}
        else {if(strstr(file->engine_path,".vspv") || strstr(file->engine_path,".inc"))assert(length==file->body_length && !memcmp(out,file->body,length));free(out);}
    }
    assert(changed_decl && changed_bin && bin_length);
    uint32_t checksum=(uint32_t)changed_bin[4]<<24|(uint32_t)changed_bin[5]<<16|(uint32_t)changed_bin[6]<<8|changed_bin[7];
    assert(checksum==sh_package_shader_declaration_checksum(changed_decl,decl_length));free(changed_decl);free(changed_bin);
    binding b={0,1,0};size_t length=0;const sh_compiled_resource *file=u->files[0];
    for(size_t i=0;i<u->file_count;i++)if(strstr(u->files[i]->engine_path,".bin"))file=u->files[i];
    unsigned char *bad=sh_package_source_link_bind_file(s,unit,file,file->body,file->body_length,bind_fixture,&b,&length,error,sizeof(error));assert(!bad && strstr(error,"fixed native ABI"));
    sh_package_link_classes identity={0};assert(sh_package_source_link_identify(s,&identity,error,sizeof(error)));assert(identity.count==4);
    sh_package_link_classes_free(&identity);sh_package_source_link_close(s);clear(&f);
}
static void write_file(const char *path,const void *body,size_t length)
{ FILE *file=fopen(path,"wb");assert(file);assert(!length || fwrite(body,1,length,file)==length);assert(!fclose(file)); }
static void include_index(const char *path,const char *program)
{
    bytes b={0};unsigned char head[0x20]={5,'S','E','R'},tail[21]={0};
    raw(&b,head,sizeof(head));be(&b,1);
    be(&b,0);str(&b,"renderProg");str(&b,program);str(&b,"decls/renderprogs/common.inc");raw(&b,tail,sizeof(tail));
    uint32_t size=(uint32_t)b.length-0x20;
    b.data[4]=(unsigned char)(size>>24);b.data[5]=(unsigned char)(size>>16);b.data[6]=(unsigned char)(size>>8);b.data[7]=(unsigned char)size;
    write_file(path,b.data,b.length);
}
/* Only a changed shared include fans out to the SnapMap programs beneath it.
 * An exact stock copy stays delivery data; campaign copies and failed reads
 * cannot be mistaken for unchanged SnapMap data. */
static void include_fanout_snapmap_only(void)
{
    static const char *const archives[]={"snap_gameresources.resources","snap_gameresources.patch",
        "gameresources.resources","gameresources.patch","snap_gameresources.pindex","gameresources.pindex"};
    fixture f;setup(&f,0);char error[512],temporary[MAX_PATH],base[MAX_PATH+64],path[MAX_PATH+128];
    assert(GetTempPathA(sizeof(temporary),temporary));
    snprintf(base,sizeof(base),"%sshader-include-catalog-%lu",temporary,(unsigned long)GetCurrentProcessId());
    assert(CreateDirectoryA(base,NULL) || GetLastError()==ERROR_ALREADY_EXISTS);
    for(size_t i=0;i<4;i++){snprintf(path,sizeof(path),"%s/%s",base,archives[i]);write_file(path,"",0);}
    snprintf(path,sizeof(path),"%s/%s",base,archives[4]);include_index(path,"fixture");
    snprintf(path,sizeof(path),"%s/%s",base,archives[5]);include_index(path,"campaignonly");
    sh_resource_catalog *catalog=sh_resource_catalog_open(base,error,sizeof(error));assert(catalog);
    sh_compiled_resource include=*sh_package_compilation_find(&f.compiled,"decls/renderprogs/common.inc");
    sh_package_compilation supplied={0};supplied.resources=&include;supplied.resource_count=1;
    const sh_package_compilation *scope=&supplied;
    sh_package_source_shader shader={SH_PACKAGE_SHADER_VULKAN,&f,abi,flag_abi};
    sh_package_source_link_native native={&f,admit,semantics,&shader};
    unsigned char *authored=malloc(include.body_length+1);assert(authored);
    memcpy(authored,include.body,include.body_length+1);include.body=authored;
    for(int mode=0;mode<5;mode++) {
        authored[3]=mode==1 ? 'x' : 'r'; /* Same length, different comment bytes. */
        f.include_status=mode==2 ? 2 : mode==3 ? 3 : 0;f.read_failure=mode==4;
        sh_package_source_link *s=sh_package_source_link_open(&scope,1,catalog,original,&f,
            (sh_decl_registry_source){&f,read_memory,f.registry,NULL},f.reflection,&native,error,sizeof(error));
        if(mode==4) { assert(!s && strstr(error,"original resource could not be captured"));continue; }
        if(!s)fprintf(stderr,"include fan-out %d: %s\n",mode,error);assert(s);
        const sh_package_link_inputs *inputs=sh_package_source_link_inputs(s);
        assert((sh_package_link_inputs_find(inputs,0,"renderprog","fixture")!=SIZE_MAX)==(mode!=0));
        assert(sh_package_link_inputs_find(inputs,0,"renderprog","campaignonly")==SIZE_MAX);
        if(mode==0) { assert(!inputs->count);assert(supplied.resource_count==1 && include.body==authored); }
        sh_package_source_link_close(s);
    }
    free(authored);sh_resource_catalog_close(catalog);clear(&f);
    for(size_t i=0;i<6;i++){snprintf(path,sizeof(path),"%s/%s",base,archives[i]);assert(DeleteFileA(path));}
    assert(RemoveDirectoryA(base));
}
/* A render program cannot be renamed. One SnapMap does not ship keeps the name
 * its scope gives it (binding 2) even when re-cooked; a changed SnapMap one
 * cannot keep it. */
static void unshipped_program(void)
{
    static const struct { int absent, campaign, stale, binding; } cases[] = {
        {0, 0, 0, 1}, {0, 0, 1, 0}, {0, 1, 0, 2}, {0, 1, 1, 2}, {1, 0, 0, 2}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fixture f; setup(&f, 0); char error[512];
        f.absent = cases[i].absent; f.campaign = cases[i].campaign; f.stale = cases[i].stale;
        sh_package_source_link *s = open_link(&f, SH_PACKAGE_SHADER_VULKAN, error);
        if (!s) fprintf(stderr, "unshipped program %zu: %s\n", i, error);
        assert(s);
        const sh_package_link_inputs *inputs = sh_package_source_link_inputs(s);
        size_t unit = sh_package_link_inputs_find(inputs, 0, "renderprog", "fixture");
        assert(unit != SIZE_MAX && inputs->units[unit].original_binding == cases[i].binding);
        sh_package_source_link_close(s); clear(&f);
    }
}
static void reject_incomplete(void)
{
    char error[512];
    for(int mode=0;mode<8;mode++) {
        fixture f;setup(&f,1);
        if(mode==0)f.deny=1;
        if(mode==1)f.kind=0;
        if(mode==2)f.mutate=1;
        if(mode==3){sh_compiled_resource *r=(sh_compiled_resource *)sh_package_compilation_find(&f.compiled,"generated/renderprogs/fixture_pc_vulkan.bin");r->body[4]^=1;}
        if(mode==4){sh_compiled_resource *r=(sh_compiled_resource *)sh_package_compilation_find(&f.compiled,"generated/spirv/fixture.vspv");r->body[4]^=1;}
        if(mode==5){f.read_failure=1;}
        if(mode==6)f.flag_deny=1;
        if(mode==7)f.flag_mutate=1;
        sh_package_source_link *s=open_link(&f,SH_PACKAGE_SHADER_VULKAN,error);assert(!s && *error);assert(!f.admit_calls);clear(&f);
    }
}
static unsigned char *native_object(fixture *f,unsigned char *manager,const char *type,
    const char *name,const char *source,int kind,uint32_t index)
{
    unsigned char *object=memory(f,0x90),*names=memory(f,strlen(name)+1),*types=memory(f,strlen(type)+1);
    unsigned char *array=memory(f,8),*text=memory(f,strlen(source)+1);
    strcpy((char *)names,name);strcpy((char *)types,type);strcpy((char *)text,source);
    ptr(manager,8,types);ptr(manager,0x20,array);num(manager,0x28,1);ptr(array,0,object);
    ptr(object,8,names);ptr(object,0x10,manager);ptr(object,0x38,text);num(object,0x40,(int)strlen(source));
    uint32_t id=0;assert(sh_native_resource_id(type,name,&id));num(object,0x20,(int)id);
    num(object,0x68,(int)index);num(object,0x6c,kind);return object;
}
typedef struct prepare_context {
    fixture *fixture;
    sh_package_native_assets *assets;
    unsigned char *object;
    int mode, calls, recoveries, recovery_fails;
} prepare_context;
static int prepare_shader(void *context,const sh_package_shader_request *request,char *error,size_t capacity)
{
    prepare_context *c=context;(void)error;(void)capacity;c->calls++;
    /* Reentry proves that native preparation does not hold the asset lock. */
    assert(sh_package_native_assets_shader_pending(c->assets)==0);
    assert(!strcmp(request->family,"renderparm") && !strcmp(request->name,"tex"));
    assert(request->source_length==strlen(parameter) && !memcmp(request->source,parameter,strlen(parameter)));
    assert(request->resource==(uintptr_t)c->object && request->task_id);
    if(c->mode==1)return 1; /* A claimed success is not positive input evidence. */
    sh_package_shader_receipt_frame receipt;
    sh_package_shader_receipt_begin(&receipt,request->resource,request->family,request->name);
    sh_package_shader_receipt_retained(request->source,request->source_length);
    if(c->mode==2)c->object[0x58]=19;
    sh_package_shader_receipt_end(&receipt,c->mode!=2,read_memory,c->fixture);
    return c->mode!=2;
}
static int recover_shader(void *context,const sh_package_shader_preparation *batch,char *error,size_t capacity)
{
    prepare_context *c=context;(void)error;(void)capacity;c->recoveries++;
    assert(sh_package_shader_preparation_assets(batch)==c->assets);
    assert(sh_package_native_assets_shader_pending(c->assets)==0);
    if(c->recovery_fails)return 0;
    /* Fixture recovery restores the prior provider's native default. The
     * production callback must invoke its existing activation recovery. */
    c->object[0x58]=0;sh_package_shader_receipt_invalidate((uintptr_t)c->object);return 1;
}
static void preparation_contract(void)
{
    fixture f;setup(&f,0);char error[512];
    unsigned char *head=memory(&f,8),*manager=memory(&f,0x30);ptr(head,0,manager);
    unsigned char *object=native_object(&f,manager,"renderParm","tex",parameter,1,0x410);
    memset(object+0x38,0,16);
    sh_package_native_assets *assets=sh_package_native_assets_open("C:/shader-prepare-fixture");assert(assets);
    sh_native_resource_registry_source registry={&f,read_memory,(uintptr_t)head};
    assert(sh_package_native_assets_configure_shader(assets,SH_PACKAGE_SHADER_OPENGL,registry,original,&f,error,sizeof(error)));
    sh_package_library_native adapter=sh_package_native_assets_adapter(assets);
    for(int campaign=0;campaign<2;campaign++) {
        unsigned char *captured=NULL;size_t length=0;f.campaign=campaign;
        assert(sh_package_native_assets_shader_original_read(assets,
            "generated/decls/renderparm/tex.decl",&captured,&length)==(campaign?2:1));
        assert(length==strlen(parameter) && !memcmp(captured,parameter,length));free(captured);
        assert(!sh_package_native_assets_shader_original_read(assets,"missing.inc",&captured,&length));
        assert(!captured && !length);
    }
    f.campaign=0;
    const sh_package_source_shader *shader=adapter.source.shader;int kind;uint32_t index;
    sh_decl_source source={parameter,strlen(parameter)};
    prepare_context c={&f,assets,object,0,0,0,0};uint64_t task_id=0;
    for(int mode=0;mode<4;mode++) {
        sh_package_shader_receipt_invalidate((uintptr_t)object);
        assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
        assert(strstr(error,"environment needs") && sh_package_native_assets_shader_pending(assets)==1);
        assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
        assert(sh_package_native_assets_shader_pending(assets)==1);
        sh_package_shader_preparation *batch=sh_package_native_assets_shader_take(assets);assert(batch);
        assert(!sh_package_native_assets_shader_pending(assets) && sh_package_shader_preparation_count(batch)==1);
        const sh_package_shader_request *request=sh_package_shader_preparation_request(batch,0);assert(request);
        assert(!sh_package_shader_preparation_request(batch,1));
        if(!task_id)task_id=request->task_id;else assert(request->task_id==task_id);
        c.mode=mode==3 ? 2 : mode;c.recovery_fails=mode==3;
        int result=sh_package_shader_preparation_run(batch,prepare_shader,recover_shader,&c,error,sizeof(error));
        assert(result==(mode==0 ? 1 : mode==3 ? -1 : 0));
        assert(sh_package_shader_preparation_run(batch,prepare_shader,recover_shader,&c,error,sizeof(error))==-1);
        if(mode==0)assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)) && kind==1 && index==0x410);
        else {
            assert(c.recoveries==mode);
            assert(object[0x58]==(mode==3 ? 19 : 0));
            assert(!sh_package_shader_receipt_matches((uintptr_t)object,"renderparm","tex",source.text,source.length,object,0x90));
        }
        sh_package_shader_preparation_free(batch);
    }
    assert(c.calls==4);sh_package_native_assets_close(assets);clear(&f);
}
typedef struct read_counter { fixture *fixture; size_t count; } read_counter;
static int counted_read(void *context,uintptr_t address,void *out,size_t length)
{ read_counter *c=context;c->count++;return read_memory(c->fixture,address,out,length); }
/* One compilation's checks share a native identity snapshot: a later check
 * reads only what it revalidates. A change during the scope fails that check
 * and the scope, and unscoped checks afterward open their own snapshot. */
static void shader_scope_contract(void)
{
    fixture f;setup(&f,0);char error[512];
    unsigned char *head=memory(&f,8),*manager=memory(&f,0x30);ptr(head,0,manager);
    unsigned char *object=native_object(&f,manager,"renderParm","tex",parameter,1,0x410);
    read_counter reads={&f,0};uint32_t id=0;assert(sh_native_resource_id("renderParm","tex",&id));
    sh_package_native_assets *assets=sh_package_native_assets_open("C:/shader-scope-fixture");assert(assets);
    sh_native_resource_registry_source registry={&reads,counted_read,(uintptr_t)head};
    assert(sh_package_native_assets_configure_shader(assets,SH_PACKAGE_SHADER_OPENGL,registry,original,&f,error,sizeof(error)));
    sh_package_library_native adapter=sh_package_native_assets_adapter(assets);
    const sh_package_source_shader *shader=adapter.source.shader;int kind=0;uint32_t index=0;
    sh_decl_source source={parameter,strlen(parameter)};
    sh_package_shader_receipt_invalidate((uintptr_t)object);
    size_t start=reads.count;
    assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)) && kind==1 && index==0x410);
    size_t unscoped=reads.count-start;
    sh_package_native_assets_shader_scope_begin(assets);
    assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
    start=reads.count;
    assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)) && kind==1 && index==0x410);
    assert(reads.count-start<unscoped);
    assert(sh_package_native_assets_shader_scope_end(assets,error,sizeof(error)) && !*error);
    sh_package_native_assets_shader_scope_begin(assets);
    assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
    num(object,0x20,17);
    assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
    assert(!sh_package_native_assets_shader_scope_end(assets,error,sizeof(error)) && *error);
    num(object,0x20,(int)id);
    assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)) && kind==1);
    sh_package_native_assets_close(assets);clear(&f);
}
typedef struct cook_context {
    fixture *fixture;
    sh_package_native_assets *assets;
    unsigned char *programs[2];
    int mode, recovered, cold;
} cook_context;
static const char permutation_name[]="generated/renderprogs/permutations/fixture_0000000000000001.decl";
static void remove_file(fixture *f,const char *path)
{
    for(size_t i=0;i<f->compiled.resource_count;i++)if(!strcmp(f->files[i].engine_path,path)) {
        free((char *)f->files[i].engine_path);free(f->files[i].type);free(f->files[i].name);free(f->files[i].body);
        memmove(f->files+i,f->files+i+1,(--f->compiled.resource_count-i)*sizeof(*f->files));return;
    }
    assert(0);
}
static void inactive_permutations(sh_package_shader_renderer renderer)
{
    fixture f;setup(&f,1);char error[512];bytes root=program(0);
    const char *root_path=renderer==SH_PACKAGE_SHADER_OPENGL ? "generated/renderprogs/fixture_pc.bin" :
        "generated/renderprogs/fixture_pc_vulkan.bin";
    remove_file(&f,root_path);add(&f,root_path,NULL,NULL,root.data,root.length);
    remove_file(&f,renderer==SH_PACKAGE_SHADER_OPENGL ? "generated/renderprogs/permutations/fixture_0000000000000001_pc.bin" :
        "generated/renderprogs/permutations/fixture_0000000000000001_pc_vulkan.bin");
    if(renderer==SH_PACKAGE_SHADER_VULKAN)
        remove_file(&f,"generated/spirv/generated/renderprogs/permutations/fixture_0000000000000001.vspv");
    qsort(f.files,f.compiled.resource_count,sizeof(*f.files),order);
    sh_package_source_link *link=open_link(&f,renderer,error);
    if(!link)fprintf(stderr,"inactive permutation: %s\n",error);assert(link);
    const sh_package_link_inputs *inputs=sh_package_source_link_inputs(link);
    size_t owner=sh_package_link_inputs_find(inputs,0,"renderprog","fixture");assert(owner!=SIZE_MAX);
    for(size_t i=0;i<inputs->units[owner].file_count;i++)
        assert(!strstr(inputs->units[owner].files[i]->engine_path,"permutations/"));
    sh_package_source_link_close(link);
    sh_compiled_resource *permutation=(sh_compiled_resource *)sh_package_compilation_find(&f.compiled,permutation_name);
    assert(permutation);permutation->body[2]^=1;
    assert(!open_link(&f,renderer,error) && strstr(error,"companion owner"));
    permutation->body[2]^=1;
    /* A selected permutation still needs this renderer's full companions. */
    sh_compiled_resource *selected=(sh_compiled_resource *)sh_package_compilation_find(&f.compiled,root_path);
    root=program(1);free(selected->body);selected->body=malloc(root.length);assert(selected->body);
    memcpy(selected->body,root.data,root.length);selected->body_length=root.length;
    assert(!open_link(&f,renderer,error));clear(&f);
}
static int cooked_original(void *context,const char *path,unsigned char **body,size_t *length)
{
    cook_context *c=context;int present=sh_package_native_assets_shader_read(c->assets,path,body,length);
    return present ? present : original(c->fixture,path,body,length);
}
static int prepare_program(void *context,const sh_package_shader_request *request,char *error,size_t capacity)
{
    cook_context *c=context;(void)error;(void)capacity;
    assert(!strcmp(request->family,"renderprog") && !strcmp(request->name,"fixture"));
    assert((request->resource==0)==c->cold);
    assert(!sh_package_native_assets_shader_pending(c->assets));
    sh_package_shader_receipt_program_scope(1);
    for(unsigned i=0;i<2;i++) {
        if(i && c->mode==3) { sh_package_shader_receipt_program_scope(0);sh_package_shader_receipt_program_scope(1); }
        sh_package_shader_receipt_frame frame;bytes binary=program(i==0);
        sh_package_shader_cook_begin(&frame,(uintptr_t)c->programs[i],SH_PACKAGE_SHADER_OPENGL,i ? permutation_name : "fixture");
        sh_package_shader_receipt_retained(declaration,strlen(declaration)-1);
        const char *include="// retained include\n";
        sh_package_shader_receipt_bytes("decls/renderprogs/common.inc",20,0,include,c->mode==2 ? 19 : 20);
        if(c->mode==4)sh_package_shader_receipt_bytes("generated/decls/renderprog/private-parent.decl",7,0,"changed",7);
        if(c->mode!=1)sh_package_shader_receipt_program_compiled((uintptr_t)c->programs[i]);
        (void)sh_package_shader_receipt_program_wrote((uintptr_t)c->programs[i],binary.data+8,binary.length-8);
        sh_package_shader_cook_end(&frame,1);
    }
    sh_package_shader_receipt_program_scope(0);return 1;
}
static int recover_program(void *context,const sh_package_shader_preparation *batch,char *error,size_t capacity)
{
    cook_context *c=context;(void)batch;(void)error;(void)capacity;c->recovered++;
    for(unsigned i=0;i<2;i++)sh_package_shader_receipt_invalidate((uintptr_t)c->programs[i]);return 1;
}
static sh_package_source_link *open_cooked_link(cook_context *c,char *error)
{
    fixture *f=c->fixture;const sh_package_compilation *scope=&f->compiled;
    sh_package_library_native native=sh_package_native_assets_adapter(c->assets);
    return sh_package_source_link_open(&scope,1,NULL,cooked_original,c,
        (sh_decl_registry_source){f,read_memory,f->registry,NULL},f->reflection,&native.source,error,512);
}
typedef struct frontier_fixture { fixture *source; int calls, reject; } frontier_fixture;
static int frontier_parameter(void *context,const char *name,sh_decl_source source,int *kind,uint32_t *index,char *error,size_t capacity)
{ return abi(((frontier_fixture *)context)->source,name,source,kind,index,error,capacity); }
static int frontier_flag(void *context,const char *name,sh_decl_source source,uint64_t *mask,char *error,size_t capacity)
{ return flag_abi(((frontier_fixture *)context)->source,name,source,mask,error,capacity); }
static int queue_frontier(void *context,const char *name,sh_decl_source source,char *error,size_t capacity)
{
    frontier_fixture *f=context;f->calls++;
    assert(source.length==strlen(declaration) && !memcmp(source.text,declaration,source.length));
    if(f->reject && !strcmp(name,"third")) { snprintf(error,capacity,"incompatible original source");return 0; }
    snprintf(error,capacity,"queued %s",name);return 1;
}
/* Missing programs form one preparation frontier. None is accepted until the
 * exact cooked output exists; a later real failure must survive earlier queues. */
static void preparation_frontier(void)
{
    fixture f;setup(&f,0);char error[512];frontier_fixture pending={&f};
    remove_file(&f,"generated/renderprogs/fixture_pc.bin");
    add(&f,"generated/decls/renderprog/second.decl","renderprog","second",declaration,strlen(declaration));
    add(&f,"generated/decls/renderprog/third.decl","renderprog","third",declaration,strlen(declaration));
    qsort(f.files,f.compiled.resource_count,sizeof(*f.files),order);
    sh_package_source_shader shader={SH_PACKAGE_SHADER_OPENGL,&pending,frontier_parameter,frontier_flag,queue_frontier};
    sh_package_source_link_native native={0};native.shader=&shader;
    sh_decl_registry_source registry={&f,read_memory,f.registry,NULL};
    const sh_package_compilation *scope=&f.compiled;sh_package_source_failure failure={0};
    assert(!sh_package_source_link_open_attributed(&scope,1,NULL,original,&f,registry,f.reflection,
        &native,NULL,&failure,error,sizeof(error)));
    if(pending.calls!=3 || failure.kind!=SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT || !strstr(error,"queued fixture"))
        fprintf(stderr,"frontier calls=%d kind=%d: %s\n",pending.calls,failure.kind,error);
    assert(pending.calls==3 && failure.kind==SH_PACKAGE_SOURCE_FAILURE_ENVIRONMENT && strstr(error,"queued fixture"));
    pending=(frontier_fixture){&f,0,1};
    assert(!sh_package_source_link_open_attributed(&scope,1,NULL,original,&f,registry,f.reflection,
        &native,NULL,&failure,error,sizeof(error)));
    assert(pending.calls==3 && strstr(error,"incompatible original source"));
    /* PREPARING owns no returned source, binary or decoded arrays. */
    sh_package_source_reader *reader=sh_package_source_reader_open(&f.compiled,NULL,original,&f,
        registry,f.reflection,error,sizeof(error));assert(reader);
    assert(sh_package_source_reader_shader(reader,&shader));
    unsigned char *decl=NULL,*binary=NULL;size_t dl=0,bl=0;sh_package_shader_program decoded={0};
    assert(sh_package_source_shader_open(reader,"fixture",&decl,&dl,&binary,&bl,&decoded,error,sizeof(error))==SH_PACKAGE_SOURCE_SHADER_PREPARING);
    assert(!decl && !binary && !dl && !bl && !decoded.reference_count);
    f.read_failure=1;
    assert(sh_package_source_shader_open(reader,"missing",&decl,&dl,&binary,&bl,&decoded,error,sizeof(error))==SH_PACKAGE_SOURCE_SHADER_ERROR);
    assert(pending.calls==4);f.read_failure=0;sh_package_source_reader_close(reader);
    /* A retry consumes complete bytes through the ordinary validation route. */
    bytes cooked=program(0);
    add(&f,"generated/renderprogs/fixture_pc.bin",NULL,NULL,cooked.data,cooked.length);
    add(&f,"generated/renderprogs/second_pc.bin",NULL,NULL,cooked.data,cooked.length);
    add(&f,"generated/renderprogs/third_pc.bin",NULL,NULL,cooked.data,cooked.length);
    qsort(f.files,f.compiled.resource_count,sizeof(*f.files),order);
    sh_package_source_link *link=open_link(&f,SH_PACKAGE_SHADER_OPENGL,error);
    if(!link)fprintf(stderr,"frontier retry: %s\n",error);assert(link);
    assert(sh_package_link_inputs_find(sh_package_source_link_inputs(link),0,"renderprog","third")!=SIZE_MAX);
    sh_package_source_link_close(link);clear(&f);
}
static void source_only_opengl(int cold)
{
    fixture f;setup(&f,1);char error[512];
    remove_file(&f,"generated/renderprogs/fixture_pc.bin");
    remove_file(&f,"generated/renderprogs/permutations/fixture_0000000000000001_pc.bin");
    remove_file(&f,permutation_name);
    unsigned char *head=memory(&f,8),*manager=memory(&f,0x30),*flag_manager=memory(&f,0x30),*program_manager=memory(&f,0x30);
    ptr(head,0,manager);ptr(manager,0x18,flag_manager);ptr(flag_manager,0x18,program_manager);
    (void)native_object(&f,manager,"renderParm","tex",parameter,1,0x410);
    (void)native_object(&f,flag_manager,"renderProgFlag","feature",flag_source,0,0);
    cook_context c={0};c.fixture=&f;c.cold=cold;
    c.programs[0]=native_object(&f,program_manager,"renderProg","fixture",declaration,0,0);
    c.programs[1]=native_object(&f,program_manager,"renderProg",permutation_name,declaration,0,0);
    unsigned char *array=memory(&f,16);ptr(array,0,c.programs[0]);ptr(array,8,c.programs[1]);ptr(program_manager,0x20,array);num(program_manager,0x28,2);
    if(cold)num(program_manager,0x28,0); /* Cooking must not create a GPU resource slot. */
    c.assets=sh_package_native_assets_open("C:/shader-cook-fixture");assert(c.assets);
    assert(sh_package_native_assets_configure_shader(c.assets,SH_PACKAGE_SHADER_OPENGL,
        (sh_native_resource_registry_source){&f,read_memory,(uintptr_t)head},original,&f,error,sizeof(error)));
    for(c.mode=1;c.mode<=4;c.mode++) {
        assert(!open_cooked_link(&c,error));assert(sh_package_native_assets_shader_pending(c.assets)==1);
        sh_package_shader_preparation *batch=sh_package_native_assets_shader_take(c.assets);assert(batch);
        assert(sh_package_shader_preparation_run(batch,prepare_program,recover_program,&c,error,sizeof(error))==0);
        sh_package_shader_preparation_free(batch);assert(c.recovered==c.mode);
        unsigned char *body=NULL;size_t length=0;
        assert(!sh_package_native_assets_shader_read(c.assets,"generated/renderprogs/fixture_pc.bin",&body,&length));
    }
    c.mode=0;assert(!open_cooked_link(&c,error));
    sh_package_shader_preparation *batch=sh_package_native_assets_shader_take(c.assets);assert(batch);
    assert(sh_package_shader_preparation_run(batch,prepare_program,recover_program,&c,error,sizeof(error))==1);
    sh_package_shader_preparation_free(batch);
    sh_package_source_link *link=open_cooked_link(&c,error);if(!link)fprintf(stderr,"source cook: %s\n",error);assert(link);
    unsigned char *body=NULL;size_t length=0;
    assert(sh_package_native_assets_shader_read(c.assets,"generated/decls/renderprog/fixture.decl",&body,&length)==1);
    assert(length==strlen(declaration) && !memcmp(body,declaration,length));free(body);
    assert(sh_package_native_assets_shader_read(c.assets,"generated/renderprogs/fixture_pc.bin",&body,&length)==1);
    bytes expected=program(1);assert(length==expected.length && !memcmp(body,expected.data,length));free(body);
    assert(!sh_package_native_assets_shader_pending(c.assets));
    assert(*(uint32_t *)(program_manager+0x28)==(cold?0u:2u));
    sh_package_source_link_close(link);sh_package_native_assets_close(c.assets);clear(&f);
}
static void native_contract(void)
{
    fixture f;setup(&f,0);char error[512];
    unsigned char *head=memory(&f,8),*manager=memory(&f,0x30),*flag_manager=memory(&f,0x30);
    ptr(head,0,manager);ptr(manager,0x18,flag_manager);
    unsigned char *parameter_object=native_object(&f,manager,"renderParm","tex",parameter,1,0x410);
    unsigned char *flag_object=native_object(&f,flag_manager,"renderProgFlag","feature",flag_source,0,0);
    uint64_t flag_mask=UINT64_C(1)<<42;memcpy(flag_object+0x58,&flag_mask,8);
    sh_package_native_assets *assets=sh_package_native_assets_open("C:/shader-fixture");assert(assets);
    sh_native_resource_registry_source registry={&f,read_memory,(uintptr_t)head};
    assert(sh_package_native_assets_configure_shader(assets,SH_PACKAGE_SHADER_VULKAN,registry,original,&f,error,sizeof(error)));
    sh_package_library_native adapter=sh_package_native_assets_adapter(assets);assert(adapter.source.shader);
    const sh_package_source_shader *shader=adapter.source.shader;int kind=-1;uint32_t index=0;uint64_t mask=0;
    sh_decl_source source={parameter,strlen(parameter)};
    assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error))==1 && kind==1 && index==0x410);
    assert(shader->flag(shader->context,"feature",(sh_decl_source){flag_source,strlen(flag_source)},&mask,error,sizeof(error))==1 && mask==flag_mask);
    assert(!shader->parameter(shader->context,"tex",(sh_decl_source){NULL,0},&kind,&index,error,sizeof(error)));
    assert(!shader->parameter(shader->context,"tex",(sh_decl_source){"{ Tex2D changed }",17},&kind,&index,error,sizeof(error)));
    num(parameter_object,0x68,0x93f);assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
    num(parameter_object,0x68,17);assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
    num(parameter_object,0x68,0x410);num(parameter_object,0x6c,0);assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
    num(parameter_object,0x6c,1);parameter_object[0x2c]=4;assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
    parameter_object[0x2c]=0;uintptr_t text=0;memcpy(&text,parameter_object+0x38,8);((char *)text)[2]='X';
    assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));((char *)text)[2]='T';
    assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error))==1);
    sh_package_shader_receipt_frame receipt;
    sh_package_shader_receipt_begin(&receipt,(uintptr_t)parameter_object,"renderparm","tex");
    sh_package_shader_receipt_bytes("generated/decls/renderparm/tex.decl",source.length,0,source.text,source.length);
    memset(parameter_object+0x38,0,16); /* Exact input was consumed, then retired. */
    sh_package_shader_receipt_end(&receipt,1,read_memory,&f);
    assert(shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error))==1);
    parameter_object[0x58]^=1;assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));parameter_object[0x58]^=1;
    sh_package_shader_receipt_invalidate((uintptr_t)parameter_object);
    assert(!shader->parameter(shader->context,"tex",source,&kind,&index,error,sizeof(error)));
    assert(!sh_package_native_assets_configure_shader(assets,SH_PACKAGE_SHADER_OPENGL,registry,original,&f,error,sizeof(error)));
    sh_package_native_assets_close(assets);clear(&f);
}
int main(void)
{ complete_and_bind(SH_PACKAGE_SHADER_VULKAN);complete_and_bind(SH_PACKAGE_SHADER_OPENGL);include_fanout_snapmap_only();unshipped_program();reject_incomplete();native_contract();preparation_contract();shader_scope_contract();inactive_permutations(SH_PACKAGE_SHADER_OPENGL);inactive_permutations(SH_PACKAGE_SHADER_VULKAN);preparation_frontier();source_only_opengl(0);source_only_opengl(1);puts("package_shader_link_test: PASS");return 0; }
