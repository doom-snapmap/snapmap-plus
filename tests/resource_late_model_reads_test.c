/* Actual instance/origin and FLS core; shared graph/native bodies are explicit seams. */
#define WIN32_LEAN_AND_MEAN
#define SH_RESOURCE_INSTANCES_TESTING
#define SH_RESOURCE_LATE_MODEL_READS_TESTING
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/execution_context.c"
#include "../src/backend/resource_instances.c"
#include "../src/backend/resource_late_model_reads.c"
#include "native_pin_fixture.h"
static void lm_fixture(int renderer,uint32_t rva,void *out,size_t length)
{ if(!sh_native_pin_fixture_copy(renderer,rva,out,length))abort(); }
static int failures;
#define CHECK(x) do{if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);failures++;}}while(0)
static int renderer,shared_ready=1,hidden,unknowns,inputs,ends,last_returned,last_complete,native_calls,mode,input_fail;
static int live[32];
int sh_host_is_vulkan(void){return renderer;}
int sh_host_is_pinned_renderer_build(int r){return r==renderer;}
void sh_resource_graph_incomplete(void){unknowns++;}
int sh_resource_graph_native_observe_resource(void *r){(void)r;unknowns++;return 0;}
static void setptr(void *p,size_t at,void *value){memcpy((unsigned char *)p+at,&value,8);}
static void setint(void *p,size_t at,int value){memcpy((unsigned char *)p+at,&value,4);}
static void *getptr(void *p,size_t at){void *v;memcpy(&v,(unsigned char *)p+at,8);return v;}
static int promote(const void *capture,void **lease){int *v=malloc(sizeof(int));if(!v)return 0;*v=*(int *)capture;live[*v]++;*lease=v;return 1;}
static void capture_free(void *p){free(p);}
static void lease_free(void *p){live[*(int *)p]--;free(p);}
static void source(void *object,int generation,int fresh)
{
    uint64_t token=sh_resource_instances_token(object);if(fresh || !token)token=sh_resource_instances_construct(object,0);
    int *capture=malloc(sizeof(int));*capture=generation;
    sh_resource_instance_origin *o=sh_resource_instances_origin(capture,0);CHECK(o);
    CHECK(sh_resource_instances_origin_source(o,object,token));CHECK(sh_resource_instances_publish(object,o));sh_resource_instances_origin_release(o);
}
typedef struct root_read {sh_resource_instance_origin *origin;struct root_read *next;} root_read;
typedef struct root_frame {sh_resource_late_model_scope scope;void *model;int mode;root_read *reads;struct root_frame *next;} root_frame;
static root_frame *root_top;static uint64_t root_serial;
static sh_resource_late_model_scope begin(void *model,int kind)
{
    root_frame *f=calloc(1,sizeof *f);f->scope=(sh_resource_late_model_scope){sh_execution_context(NULL,0),++root_serial};f->model=model;f->mode=kind;f->next=root_top;root_top=f;return f->scope;
}
static int visible(sh_resource_late_model_scope scope)
{return !hidden && root_top && root_top->scope.context==scope.context && root_top->scope.serial==scope.serial;}
static int input(sh_resource_late_model_scope scope,void *resource,uint64_t token,sh_resource_instance_origin *origin)
{
    inputs++;if(!visible(scope) || !resource || !token || !origin)return 0;
    root_read *r=malloc(sizeof *r);CHECK(r);if(!r)return 0;
    CHECK(sh_resource_instances_origin_hold(origin));r->origin=origin;r->next=root_top->reads;root_top->reads=r;return !input_fail;
}
static void end(sh_resource_late_model_scope scope,void *model,int returned,int complete)
{
    CHECK(visible(scope));root_frame *f=root_top;if(!f)return;root_top=f->next;
    ends++;last_returned=returned;last_complete=complete;
    uint64_t token=sh_resource_instances_token(model);size_t count=0;for(root_read *r=f->reads;r;r=r->next)count++;
    sh_resource_instance_origin **origins=malloc((count+1)*sizeof(*origins));size_t i=0;
    for(root_read *r=f->reads;r;r=r->next)origins[i++]=r->origin;
    sh_resource_instance_origin *prior=f->mode==SH_LATE_MODEL_WATER_APPEND?sh_resource_instances_lookup(model):NULL;
    if(prior)origins[i++]=prior;
    if(complete && i) {
        sh_resource_instance_origin *composite=sh_resource_instances_origin_composite(origins,i);CHECK(composite);
        CHECK(sh_resource_instances_payload(model,token,composite));CHECK(sh_resource_instances_publish(model,composite));sh_resource_instances_origin_release(composite);
    } else for(root_read *r=f->reads;r;r=r->next)(void)sh_resource_instances_fault(model,token,r->origin);
    sh_resource_instances_origin_release(prior);free(origins);
    while(f->reads){root_read *r=f->reads;f->reads=r->next;sh_resource_instances_origin_unhold(r->origin);free(r);}free(f);
}
static int ready(void){return shared_ready;}
typedef struct patch_record {void *target,*tramp,*detour;size_t size;unsigned char bytes[48];int installed;} patch_record;
static patch_record records[3];static int prepared,commits,fail_commit,unpatched;
static unsigned char *image;
static unsigned char model[0x1200],other_model[0x1200],backing[0x400],material[0x400],override_material[0x400],image_resource[0x400],file_resource[0x80],memory_file[0x180],alien[0x400];
static unsigned char surfaces[0x120],rectangles[0x28],file_bytes[16];
static void *native_ctor(void *,void *);
static uint64_t native_water(void *,void *);
static void native_post(void *,unsigned char);
void *hook_prepare(void *target,void *detour,size_t stolen)
{
    if(prepared==3)return NULL;patch_record *r=records+prepared++;
    r->target=target;r->detour=detour;r->size=stolen;memcpy(r->bytes,target,stolen);
    r->tramp=prepared==1?(void *)native_ctor:prepared==2?(void *)native_water:(void *)native_post;return r->tramp;
}
sh_patch_status hook_commit(void *tramp)
{
    for(int i=0;i<prepared;i++)if(records[i].tramp==tramp){patch_record *r=records+i;
        if(++commits==fail_commit)return B2_PATCH_FAIL_PROTECT;
        static const unsigned char jump[6]={0xff,0x25,0,0,0,0};memcpy(r->target,jump,6);memcpy((unsigned char *)r->target+6,&r->detour,8);
        memset((unsigned char *)r->target+14,0x90,r->size-14);r->installed=1;return B2_PATCH_OK;}
    return B2_PATCH_REFUSED_BADARG;
}
int hook_is_installed(void *tramp){for(int i=0;i<prepared;i++)if(records[i].tramp==tramp)return records[i].installed;return 0;}
int hook_unpatch(void *tramp){for(int i=0;i<prepared;i++)if(records[i].tramp==tramp){memcpy(records[i].target,records[i].bytes,records[i].size);records[i].installed=0;unpatched++;return 1;}return 0;}
static void native_post(void *object,unsigned char bounds)
{
    (void)bounds;native_calls++;int flag;memcpy(&flag,(unsigned char *)object+0x120,4);
    void *replacement=getptr(object,0x118);int n;memcpy(&n,(unsigned char *)object+0xae8,4);unsigned char *rows=getptr(object,0xae0);
    if(flag || replacement)for(int i=0;i<n;i++){void *m=getptr(rows,i*0x60);int special=0;if(m)memcpy(&special,(unsigned char *)m+0x190,4);if(m && special!=1 && replacement)setptr(rows,i*0x60,replacement);}
}
static void *native_ctor(void *object,void *name)
{
    CHECK(name==(void *)0x1234);native_calls++;sh_resource_instances_construct(object,0);
    if(mode==8){hidden=1;sh_resource_late_model_returned(alien,(uintptr_t)image+lm_profiles[renderer].decl_sites[0]);hidden=0;}
    sh_resource_late_model_returned(backing,(uintptr_t)image+lm_profiles[renderer].decl_sites[0]);
    if(mode==1)RaiseException(0xe1350011,0,0,NULL);
    if(mode==2)sh_resource_instances_construct(backing,0);
    if(mode==3)source(backing,9,0);
    setptr(object,lm_profiles[renderer].hot_field,mode==9?alien:backing);
    sh_resource_late_model_scope s=sh_resource_late_model_hotspot_begin(object);
    sh_resource_late_model_size_input(image_resource);lm_post_hook(object,1);sh_resource_late_model_hotspot_end(s,1);
    return mode==4?(unsigned char *)object+8:object;
}
static uint64_t native_water(void *object,void *path)
{
    CHECK(path==(void *)0x5678);native_calls++;
    if(mode!=5)sh_resource_late_model_returned(file_resource,(uintptr_t)image+lm_profiles[renderer].file_site);
    sh_resource_late_model_returned(material,(uintptr_t)image+lm_profiles[renderer].decl_sites[1]);
    setptr(object,lm_profiles[renderer].water_field,material);lm_post_hook(object,1);
    if(mode==10)file_bytes[0]^=1;
    if(mode==6)return 0;if(mode==7)RaiseException(0xe1350022,0,0,NULL);return UINT64_C(0xabcdef0000000001);
}
static void initialize(int r)
{
    renderer=r;shared_ready=1;hidden=mode=input_fail=0;prepared=commits=fail_commit=unpatched=0;
    memset(records,0,sizeof records);memset(model,0,sizeof model);memset(other_model,0,sizeof other_model);memset(backing,0,sizeof backing);
    memset(material,0,sizeof material);memset(override_material,0,sizeof override_material);memset(surfaces,0,sizeof surfaces);
    memset(image_resource,0,sizeof image_resource);memset(file_resource,0,sizeof file_resource);memset(memory_file,0,sizeof memory_file);
    memset(alien,0,sizeof alien);memset(rectangles,0,sizeof rectangles);memset(file_bytes,0,sizeof file_bytes);
    image=VirtualAlloc(NULL,0x14000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(image);if(!image)exit(2);
    const lm_profile *p=lm_profiles+r;
    for(size_t i=0;i<p->pin_count;i++)lm_fixture(renderer,p->pins[i].rva,image+p->pins[i].rva,p->pins[i].length);
    const unsigned slots[]={0,0x28,0x58,0xe0};for(unsigned i=0;i<4;i++)setptr(image+p->memory_table,slots[i],image+p->memory_methods[i]);
    source(backing,1,1);source(material,2,1);source(image_resource,3,1);source(override_material,4,1);source(file_resource,5,1);source(alien,6,1);
    setptr(backing,0x68,material);setptr(backing,0x70,rectangles);setint(backing,0x78,2);
    setptr(model,0xae0,surfaces);setint(model,0xae8,2);setptr(surfaces,0,material);setptr(surfaces,0x60,material);
    setptr(file_resource,0x38,memory_file);setptr(memory_file,0,image+p->memory_table);setptr(memory_file,0x168,file_bytes);uint64_t size=sizeof file_bytes;memcpy(memory_file+0x148,&size,8);memcpy(memory_file+0x150,&size,8);
    lm_api=(sh_resource_late_model_api){begin,visible,input,end,ready};lm_ready=lm_attempted=0;lm_base=NULL;lm_renderer=-1;
    CHECK(sh_resource_late_model_reads_install(image,lm_api));CHECK(sh_resource_late_model_reads_ready());
}
static void retire_model(void *object,int complete)
{uint64_t token=sh_resource_instances_token(object);sh_resource_late_model_destroyed(object,token,complete);sh_resource_instances_destroyed(object,token,complete);}
static void cleanup(void)
{
    retire_model(model,1);retire_model(other_model,1);lm_retire(sh_execution_context(NULL,0));sh_resource_instances_test_reset();
    for(int i=0;i<32;i++)CHECK(live[i]==0);CHECK(!root_top);VirtualFree(image,0,MEM_RELEASE);image=NULL;
}
static void cases(int r)
{
    initialize(r);const lm_profile *p=lm_profiles+r;ends=0;
    CHECK(lm_ctor_hook(model,(void *)0x1234)==model);CHECK(last_returned && last_complete && ends==1);
    CHECK(live[1] && live[2] && live[3] && !live[6]);
    sh_resource_instance_origin *field=sh_resource_instances_field_origin(model,sh_resource_instances_token(model),(uintptr_t)model+p->hot_field);CHECK(field);sh_resource_instances_origin_release(field);
    setptr(model,0x118,override_material);sh_resource_late_model_scope s=sh_resource_late_model_hotspot_begin(model);lm_post_hook(model,1);sh_resource_late_model_hotspot_end(s,1);
    CHECK(last_complete && live[4]);CHECK(getptr(surfaces,0)==override_material);
    backing[0x2c]=4;setint(backing,0x78,-123);s=sh_resource_late_model_hotspot_begin(model);sh_resource_late_model_hotspot_end(s,1);CHECK(last_complete);
    backing[0x2c]=0;setint(backing,0x78,2);
    uint64_t token=sh_resource_instances_token(model);sh_resource_late_model_destroyed(model,token,0);CHECK(lm_bindings);
    retire_model(model,0);CHECK(live[1]);retire_model(model,1);CHECK(!lm_bindings);cleanup();
    initialize(r);mode=8;CHECK(lm_ctor_hook(model,(void *)0x1234)==model && last_complete && !live[6]);cleanup();
    for(int bad=1;bad<=4;bad++) {
        initialize(r);mode=bad;int caught=0;void *result=NULL;
        __try {result=lm_ctor_hook(model,(void *)0x1234);}__except(GetExceptionCode()==0xe1350011?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
        CHECK((bad==1)==caught);CHECK(!last_complete);if(bad==4)CHECK(result==model+8);cleanup();
    }
    initialize(r);mode=9;CHECK(lm_ctor_hook(model,(void *)0x1234)==model && !last_complete);cleanup();
    for(int water_mode=0;water_mode<=7;water_mode++)if(water_mode==0 || water_mode>=5) {
        initialize(r);sh_resource_instances_construct(model,0);mode=water_mode;int caught=0;uint64_t result=0;
        __try{result=lm_water_hook(model,(void *)0x5678);}__except(GetExceptionCode()==0xe1350022?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
        if(!water_mode){CHECK(result==UINT64_C(0xabcdef0000000001) && last_complete && live[5]);}
        else {CHECK(!last_complete);if(water_mode==5)CHECK(last_returned);if(water_mode==6)CHECK(last_returned && result==0);if(water_mode==7)CHECK(caught && !last_returned);}
        cleanup();
    }
    initialize(r);sh_resource_instances_construct(model,0);mode=10;
    CHECK(lm_water_hook(model,(void *)0x5678)==UINT64_C(0xabcdef0000000001) && !last_complete);cleanup();
    initialize(r);sh_resource_instances_construct(model,0);
    setptr(image+p->memory_table,0x58,image+1);
    CHECK(lm_water_hook(model,(void *)0x5678)==UINT64_C(0xabcdef0000000001) && !last_complete);cleanup();
    initialize(r);CHECK(lm_ctor_hook(model,(void *)0x1234)==model);int before=inputs;
    lm_fail_allocation=1;s=sh_resource_late_model_hotspot_begin(model);sh_resource_late_model_size_input(alien);sh_resource_late_model_hotspot_end(s,1);CHECK(inputs==before && !lm_top(sh_execution_context(NULL,0)));
    shared_ready=0;CHECK(!sh_resource_late_model_reads_ready());shared_ready=1;
    for(size_t i=0;i<p->pin_count;i++)for(size_t j=p->pins[i].skip;j<p->pins[i].length;j++) {image[p->pins[i].rva+j]^=1;CHECK(!sh_resource_late_model_reads_ready());image[p->pins[i].rva+j]^=1;}
    unsigned char byte=image[p->hot_ctor+6];image[p->hot_ctor+6]^=1;CHECK(!sh_resource_late_model_reads_ready());image[p->hot_ctor+6]=byte;
    input_fail=1;s=sh_resource_late_model_hotspot_begin(model);sh_resource_late_model_hotspot_end(s,1);CHECK(!last_complete);input_fail=0;
    cleanup();
    initialize(r);CHECK(lm_ctor_hook(model,(void *)0x1234)==model);
    s=sh_resource_late_model_hotspot_begin(model);shared_ready=0;
    sh_resource_late_model_scope nested=sh_resource_late_model_hotspot_begin(model);shared_ready=1;
    before=inputs;sh_resource_late_model_size_input(alien);CHECK(inputs==before);
    sh_resource_late_model_hotspot_end(nested,1);sh_resource_late_model_hotspot_end(s,1);CHECK(!last_complete);cleanup();
    initialize(r);CHECK(lm_ctor_hook(model,(void *)0x1234)==model);
    s=sh_resource_late_model_hotspot_begin(model);nested=sh_resource_late_model_hotspot_begin(model);
    sh_resource_late_model_hotspot_end(s,1);CHECK(!last_complete && !lm_top(sh_execution_context(NULL,0)) && !root_top);
    sh_resource_late_model_hotspot_end(nested,1);CHECK(!lm_top(sh_execution_context(NULL,0)));cleanup();
    initialize(r);
    for(int i=0;i<3;i++)CHECK(hook_unpatch(records[i].tramp));
    prepared=commits=unpatched=0;fail_commit=2;lm_ready=lm_attempted=0;
    CHECK(!sh_resource_late_model_reads_install(image,lm_api));CHECK(!sh_resource_late_model_reads_ready());
    CHECK(unpatched==3);for(int i=0;i<3;i++)CHECK(!records[i].installed);cleanup();
}
static void image_case(const char *path,int r)
{
    FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);unsigned char *raw=malloc(n);CHECK(raw);if(!raw){fclose(f);return;}CHECK(fread(raw,1,n,f)==(size_t)n);fclose(f);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    const lm_profile *p=lm_profiles+r;
    for(size_t i=0;i<p->pin_count;i++){const lm_pin *pin=p->pins+i;int found=0;for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){IMAGE_SECTION_HEADER *s=sections+j;if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){size_t at=s->PointerToRawData+pin->rva-s->VirtualAddress;CHECK(at<=(size_t)n && pin->length<=(size_t)n-at && sh_native_pin_fixture_digest(raw+at,pin->length,pin->digest));found=1;break;}}CHECK(found);}
    free(raw);printf("late model: %s%zu complete native body pins verified\n",r?"Vulkan ":"OpenGL ",p->pin_count);
}
int main(int argc,char **argv)
{
    CHECK(sh_resource_instances_bind((sh_resource_instance_api){promote,capture_free,lease_free}));
    if(argc==3)image_case(argv[1],atoi(argv[2])!=0);else{cases(0);cases(1);}
    printf("late model: %d failure(s)\n",failures);return failures?1:0;
}
