#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/resource_model_reads.c"

static int renderer,allow_image=1,commits,removes,prepares,fail_prepare=-1,fail_commit=-1,fail_remove;
static int vt_ready=1,parm_ready=1,observing=1,installed=1;
static uint64_t lifetime_token=1;
uint64_t sh_resource_instances_token(void *resource) { return resource?lifetime_token:0; }
static void *prepared_targets[MR_ROUTES],*prepared_detours[MR_ROUTES];static size_t prepared_lengths[MR_ROUTES];static int prepared_kinds[MR_ROUTES];
int sh_host_is_vulkan(void) { return renderer; }
int sh_host_is_pinned_renderer_build(int r) { return allow_image && r==renderer; }
int sh_resource_graph_native_observing(void) { return observing; }
int sh_vt_provider_provenance_ready(void) { return vt_ready; }
int sh_resource_parm_reads_ready(void) { return parm_ready; }
int sh_vt_provider_read_begin(void *provider,sh_vt_provider_read *receipt) { (void)provider;(void)receipt;return 0; }
int sh_vt_provider_read_finish(sh_vt_provider_read *receipt,int returned,int ordered)
{ (void)receipt;(void)returned;(void)ordered;return 0; }
static void *prepare(void *target,void *detour,size_t size,int kind)
{
    int i=prepares++;if(i==fail_prepare) return NULL;assert(i<MR_ROUTES);
    prepared_targets[i]=target;prepared_detours[i]=detour;prepared_lengths[i]=size;prepared_kinds[i]=kind;return target;
}
void *hook_prepare(void *target,void *detour,size_t size) { assert(detour);return prepare(target,detour,size,0); }
void *hook_prepare_rip_load(void *target,void *detour,size_t size,size_t offset)
{ assert(detour && offset==10);return prepare(target,detour,size,1); }
void *hook_prepare_conditional_jump(void *target,void *detour,size_t size,size_t offset)
{ assert(detour && (offset==12 || offset==6));return prepare(target,detour,size,2); }
sh_patch_status hook_commit(void *tramp)
{
    assert(tramp);int i=commits++;if(i==fail_commit)return B2_PATCH_FAIL_PROTECT;
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    memcpy(tramp,jump,6);memcpy((unsigned char *)tramp+6,prepared_detours+i,8);return B2_PATCH_OK;
}
int hook_is_installed(void *tramp) { return tramp && installed; }
int hook_unpatch(void *tramp) {
    assert(tramp);removes++;if(fail_remove)return 0;
    for(int i=0;i<prepares && i<MR_ROUTES;i++)if(prepared_targets[i]==tramp)
        memcpy(tramp,g_routes[renderer][i].bytes,g_routes[renderer][i].length);
    return 1;
}

typedef struct region { uintptr_t address;unsigned char *data;size_t length; } region;
typedef struct fixture { region regions[64];size_t count;uintptr_t denied;int skip;uintptr_t definition_holder,manager_holder; } fixture;
#define BASE ((const uint8_t *)UINT64_C(0x140000000))
static void *observed[64],*reject;static size_t observed_count;static int unknown;
static int observe(void *object)
{ if(!object) { unknown++;return 0; } assert(observed_count<64);observed[observed_count++]=object;return object!=reject; }
static int observe_origin(sh_resource_instance_origin *origin) { return origin!=NULL; }
static void clear(void) { observed_count=0;unknown=0;reject=NULL; }
static void add(fixture *f,void *data,size_t length)
{ assert(f->count<64);f->regions[f->count++]=(region){(uintptr_t)data,data,length}; }
static void put(void *object,size_t offset,uintptr_t value) { memcpy((unsigned char *)object+offset,&value,8); }
static void number(void *object,size_t offset,int32_t value) { memcpy((unsigned char *)object+offset,&value,4); }
static int read_memory(void *context,uintptr_t address,void *out,size_t length)
{
    fixture *f=context;if(address==f->denied) return 0;
    if(address==(uintptr_t)BASE+g_skip[renderer] && length==4) { memcpy(out,&f->skip,4);return 1; }
    if(address==(uintptr_t)BASE+(renderer?0x47da7f0:0x30dcdc0) && length==8) { memcpy(out,&f->definition_holder,8);return 1; }
    if(address==(uintptr_t)BASE+(renderer?0x47d42d0:0x30d6fa0) && length==8) { memcpy(out,&f->manager_holder,8);return 1; }
    for(size_t i=0;i<f->count;i++) {
        region *r=f->regions+i;
        if(address>=r->address && address-r->address<=r->length && length<=r->length-(address-r->address)) {
            memcpy(out,r->data+(address-r->address),length);return 1;
        }
    }
    return 0;
}
static int probe(fixture *f,int kind,void *a,void *b)
{ clear();return sh_resource_model_reads_test_probe(kind,renderer,BASE,a,b,read_memory,f,observe); }
static void controls(int r)
{
    renderer=r;fixture f={0};
    unsigned char model[0x100]={0},surfaces[2*0x78]={0},a[0x200]={0},b[0x200]={0},geometry[0x20]={0};
    unsigned char program[0x90]={0},program2[0x90]={0},payload[0xb00]={0},provider[0x458]={0};
    unsigned char mode[0x168]={0},record[0x20]={0},definition[0x90]={0},manager[0x28]={0};
    uint16_t index[3]={3,4,5};
    add(&f,model,sizeof(model));add(&f,surfaces,sizeof(surfaces));add(&f,a,sizeof(a));add(&f,b,sizeof(b));
    add(&f,geometry,sizeof(geometry));add(&f,program,sizeof(program));add(&f,program2,sizeof(program2));
    add(&f,payload,sizeof(payload));add(&f,provider,sizeof(provider));add(&f,mode,sizeof(mode));
    add(&f,record,sizeof(record));add(&f,definition,sizeof(definition));add(&f,manager,sizeof(manager));add(&f,index,sizeof(index));
    put(model,0x70,(uintptr_t)surfaces);put(surfaces,0,(uintptr_t)a);put(surfaces,0x18,(uintptr_t)geometry);
    put(surfaces,0x78,(uintptr_t)b);put(surfaces,0x90,(uintptr_t)geometry);
    assert(probe(&f,MR_BOUNDS,model,NULL) && !observed_count);
    number(model,0x78,2);assert(probe(&f,MR_BOUNDS,model,NULL) && observed_count==3);
    number(a,0x6c,1);f.denied=(uintptr_t)surfaces+0x18;
    assert(probe(&f,MR_BOUNDS,model,NULL) && observed_count==1);number(a,0x6c,0);
    assert(!probe(&f,MR_BOUNDS,model,NULL) && unknown);f.denied=0;
    number(model,0x78,-1);assert(!probe(&f,MR_BOUNDS,model,NULL));number(model,0x78,2);
    number(model,0x78,3);assert(!probe(&f,MR_BOUNDS,model,NULL));number(model,0x78,2);

    assert(probe(&f,MR_COMPARE,NULL,NULL) && !observed_count);
    a[0x6a]=2;assert(probe(&f,MR_COMPARE,a,(void *)1) && observed_count==1);a[0x6a]=0;
    put(a,0xf0,(uintptr_t)program);put(program,8,0x11223344);
    f.denied=(uintptr_t)a+0xf8;assert(probe(&f,MR_COMPARE,a,b) && observed_count==2);f.denied=0;
    put(b,0xf0,(uintptr_t)program2);put(program2,8,0x11223344);
    assert(probe(&f,MR_COMPARE,a,b) && observed_count==2);
    put(a,0x100,1);assert(probe(&f,MR_COMPARE,a,b));a[0x69]=1;
    assert(probe(&f,MR_COMPARE,a,b)); /* One null avoids dereferencing the other. */
    put(b,0x100,1);assert(!probe(&f,MR_COMPARE,a,b));
    put(a,0x100,(uintptr_t)program);put(b,0x100,(uintptr_t)program2);
    assert(probe(&f,MR_COMPARE,a,b));
    clear();reject=a;assert(!sh_resource_model_reads_test_probe(MR_COMPARE,r,BASE,a,b,read_memory,&f,observe) && unknown);

    memset(a,0,sizeof(a));memset(program,0,sizeof(program));put(program,0x70,(uintptr_t)payload);
    for(size_t i=0;i<9;i++) put(a,0xf0+i*8,(uintptr_t)program);
    assert(probe(&f,MR_UPDATE,a,NULL) && observed_count==19 && observed[2]==payload);
    clear();reject=payload;
    assert(!sh_resource_model_reads_test_probe(MR_UPDATE,r,BASE,a,NULL,read_memory,&f,observe) && unknown);
    put(program,0x70,0);assert(probe(&f,MR_UPDATE,a,NULL) && observed_count==10);
    put(program,0x70,(uintptr_t)payload);
    for(size_t stage=0;stage<3;stage++) {
        const size_t offsets[]={0x120+stage*2*0xd8,0x120+(stage*2+1)*0xd8,
            0x630+stage*0x58,0x738+stage*0x58,0x840+stage*0x28,0x9a8+stage*0x38};
        for(size_t i=0;i<6;i++) { number(payload,offsets[i],3);put(payload,offsets[i]-8,(uintptr_t)index); }
    }
    assert(probe(&f,MR_UPDATE,a,NULL));f.denied=(uintptr_t)index+2; /* Reader sees full arrays, not individual elements. */
    f.denied=(uintptr_t)index;assert(!probe(&f,MR_UPDATE,a,NULL));f.denied=0;
    number(payload,0x120,-1);assert(!probe(&f,MR_UPDATE,a,NULL));number(payload,0x120,3);
    f.skip=1;assert(probe(&f,MR_UPDATE,(void *)1,NULL) && !observed_count);f.skip=0;

    memset(a,0,sizeof(a));put(provider,0x3d8,(uintptr_t)mode);
    put(mode,r?0x100:0x160,(uintptr_t)record);put(record,0x10,(uintptr_t)definition);
    f.definition_holder=(uintptr_t)definition;f.manager_holder=(uintptr_t)manager;
    assert(probe(&f,MR_BIND,a+0x70,provider) && observed_count==1); /* Nullable copied image fallback. */
    put(mode,r?0x100:0x160,0);assert(probe(&f,MR_BIND,a+0x70,provider));
    f.denied=(uintptr_t)definition+0x68;assert(!probe(&f,MR_BIND,a+0x70,provider));f.denied=0;
    f.denied=(uintptr_t)manager+0x20;assert(!probe(&f,MR_BIND,a+0x70,provider));
    put(provider,0x3f0,1);put(provider,0x3e8,2);
    put(mode,0xe0,3);put(mode,0xb0,4);put(mode,0xb8,5);put(mode,0xd0,6);
    assert(probe(&f,MR_BIND,a+0x70,provider)); /* Pointer bits need no image dereference. */
    f.denied=0;put(provider,0x3d8,0);assert(probe(&f,MR_BIND,(void *)1,provider) && !observed_count);
    f.skip=1;assert(probe(&f,MR_BIND,(void *)1,(void *)1) && !observed_count);
    assert(!sh_resource_model_reads_test_probe(MR_BIND,-1,BASE,a,provider,read_memory,&f,observe));
}
static void installation(void)
{
    unsigned char *base=VirtualAlloc(NULL,0x1900000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);assert(base);
    for(renderer=0;renderer<2;renderer++) {
        for(size_t i=0;i<MR_ROUTES;i++) memcpy(base+g_routes[renderer][i].rva,g_routes[renderer][i].bytes,g_routes[renderer][i].length);
        for(size_t route=0;route<MR_ROUTES;route++) for(size_t byte=0;byte<g_routes[renderer][route].length;byte++) {
            sh_resource_model_reads_test_reset();prepares=commits=removes=0;
            base[g_routes[renderer][route].rva+byte]^=1;
            assert(!sh_resource_model_reads_install(base,observe,observe_origin) && !prepares);
            base[g_routes[renderer][route].rva+byte]^=1;
        }
        for(int failed=0;failed<MR_ROUTES;failed++) {
            sh_resource_model_reads_test_reset();prepares=commits=removes=0;fail_prepare=failed;
            assert(!sh_resource_model_reads_install(base,observe,observe_origin) && removes==failed);fail_prepare=-1;
            sh_resource_model_reads_test_reset();prepares=commits=removes=0;fail_commit=failed;
            assert(!sh_resource_model_reads_install(base,observe,observe_origin) && removes==MR_ROUTES);fail_commit=-1;
        }
        sh_resource_model_reads_test_reset();prepares=commits=removes=0;
        assert(sh_resource_model_reads_install(base,observe,observe_origin) && prepares==MR_ROUTES && commits==MR_ROUTES);
        installed=0;assert(!sh_resource_model_reads_ready() && !sh_resource_model_reads_default_ready("material"));installed=1;
        allow_image=0;assert(!sh_resource_model_reads_ready());allow_image=1;
        for(size_t i=0;i<MR_ROUTES;i++) {
            base[g_routes[renderer][i].rva+6]^=1;assert(!sh_resource_model_reads_ready());base[g_routes[renderer][i].rva+6]^=1;
        }
        assert(sh_resource_model_reads_default_ready("model") && sh_resource_model_reads_default_ready("discreteanimation") && sh_resource_model_reads_default_ready("Material"));
        vt_ready=0;assert(sh_resource_model_reads_default_ready("model") && !sh_resource_model_reads_default_ready("discreteanimation") && !sh_resource_model_reads_default_ready("material"));vt_ready=1;
        parm_ready=0;assert(!sh_resource_model_reads_default_ready("discreteanimation") && !sh_resource_model_reads_default_ready("material"));parm_ready=1;
        for(size_t i=0;i<MR_ROUTES;i++) { assert(prepared_targets[i]==base+g_routes[renderer][i].rva);
            assert(prepared_lengths[i]==g_routes[renderer][i].length && prepared_kinds[i]==g_routes[renderer][i].relocation); }
    }
    VirtualFree(base,0,MEM_RELEASE);sh_resource_model_reads_test_reset();
    assert(!sh_resource_model_reads_default_ready("model") && !sh_resource_model_reads_default_ready("discreteanimation") && !sh_resource_model_reads_default_ready("material"));
    const char *other[]={"image","md6rig","skeleton","aas","json","file","animpara"};
    for(size_t i=0;i<sizeof(other)/sizeof(*other);i++) assert(sh_resource_model_reads_default_ready(other[i]));
}
static int particle_calls,particle_mutate,particle_throw,particle_reborn;
static void particle_original(void *stage,void *material)
{
    assert(stage && material);particle_calls++;
    if(particle_throw) RaiseException(0xe0123456,0,0,NULL);
    if(particle_mutate) ((unsigned char *)material)[0x68]^=8;
    if(particle_reborn)lifetime_token++;
}
static void particle_cases(int r)
{
    fixture f={0};unsigned char material[0x1a4]={0},program[0x16a0]={0},stage[0x6e0]={0};
    renderer=r;add(&f,material,sizeof material);add(&f,program,sizeof program);
    assert(probe(&f,MR_PARTICLE_MATERIAL,stage,NULL) && !observed_count);
    assert(probe(&f,MR_PARTICLE_MATERIAL,stage,material) && observed_count==1);
    put(material,0x110,(uintptr_t)program);
    assert(probe(&f,MR_PARTICLE_MATERIAL,stage,material) && observed_count==2 && observed[1]==program);
    f.denied=(uintptr_t)program+0xe0;assert(probe(&f,MR_PARTICLE_MATERIAL,stage,material));
    program[0x169b]=1;assert(!probe(&f,MR_PARTICLE_MATERIAL,stage,material) && unknown);f.denied=0;
    assert(probe(&f,MR_PARTICLE_MATERIAL,stage,material) && observed_count==2);
    material[0x68]=8;f.denied=(uintptr_t)program+0x169b;
    assert(probe(&f,MR_PARTICLE_MATERIAL,stage,material) && observed_count==1);
    material[0x68]=0;assert(!probe(&f,MR_PARTICLE_MATERIAL,stage,material));f.denied=0;
    clear();reject=program;
    assert(!sh_resource_model_reads_test_probe(MR_PARTICLE_MATERIAL,r,BASE,stage,material,read_memory,&f,observe) && unknown);
    unsigned char *base=VirtualAlloc(NULL,0x1900000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);assert(base);
    for(size_t i=0;i<MR_ROUTES;i++)memcpy(base+g_routes[r][i].rva,g_routes[r][i].bytes,g_routes[r][i].length);
    sh_resource_model_reads_test_reset();prepares=commits=removes=0;
    assert(sh_resource_model_reads_install(base,observe,observe_origin));g_particle_material=particle_original;
    clear();mr_particle_material_hook(stage,material);assert(observed_count==2 && !unknown);
    observing=0;clear();int calls=particle_calls;mr_particle_material_hook(stage,material);
    assert(!observed_count && !unknown && particle_calls==calls+1);observing=1;
    clear();particle_mutate=1;mr_particle_material_hook(stage,material);assert(unknown);particle_mutate=0;material[0x68]=0;
    clear();particle_reborn=1;mr_particle_material_hook(stage,material);assert(unknown);particle_reborn=0;
    clear();lifetime_token=0;mr_particle_material_hook(stage,material);assert(unknown);lifetime_token=1;
    clear();particle_throw=1;int caught=0;
    __try { mr_particle_material_hook(stage,material); }
    __except(GetExceptionCode()==0xe0123456?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { caught=1; }
    particle_throw=0;assert(caught && unknown);g_ready=0;VirtualFree(base,0,MEM_RELEASE);
}
static void image_case(const char *path,int r)
{
    FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);assert(length>0);
    unsigned char *raw=malloc((size_t)length);assert(raw);assert(fread(raw,1,length,f)==(size_t)length);fclose(f);
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)raw;assert(dos->e_magic==IMAGE_DOS_SIGNATURE);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+dos->e_lfanew);assert(nt->Signature==IMAGE_NT_SIGNATURE);
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<MR_ROUTES;i++) { const mr_route *pin=g_routes[r]+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
            IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)) {
                size_t offset=s->PointerToRawData+pin->rva-s->VirtualAddress;
                assert(offset<=(size_t)length && pin->length<=(size_t)length-offset && !memcmp(raw+offset,pin->bytes,pin->length));found=1;break;
            }
        } assert(found);
    }free(raw);
}
int main(int argc,char **argv)
{
    if(argc==3) { image_case(argv[1],atoi(argv[2]));puts("resource_model_reads_image_test: passed");return 0; }
    controls(0);controls(1);particle_cases(0);particle_cases(1);installation();
    puts("resource_model_reads_test: passed");return 0;
}
