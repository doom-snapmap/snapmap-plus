#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "resource_model_reads.h"
#include "resource_graph_native.h"
#include "resource_parm_reads.h"
#include "virtual_texture_provider.h"
#include "host_image.h"
#include "hook.h"

enum { MR_BOUNDS, MR_COMPARE, MR_UPDATE, MR_BIND, MR_PARTICLE_MATERIAL, MR_ROUTES };
typedef int (*mr_reader)(void *,uintptr_t,void *,size_t);
typedef struct mr_probe {
    mr_reader read; void *context; const uint8_t *base; int renderer;
    int (*observe)(void *); uint64_t hash; size_t remaining; int failed, eligible;
} mr_probe;
typedef struct mr_route { uint32_t rva; unsigned char length, relocation, offset, bytes[19]; } mr_route;
/* Filled from the independently pinned images; the compared window includes
 * every stolen instruction, including the displaced relative operand. */
static const mr_route g_routes[2][MR_ROUTES] = {
    {
        {0x14ba060,18,0,0,{0x40,0x53,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x83,0x79,0x78,0x00,0x45,0x0f,0xb6,0xf8}},
        {0xd76860,14,2,12,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x3b,0xd1,0x75,0x08}},
        {0xd76ab0,19,0,0,{0x4c,0x8b,0xdc,0x55,0x53,0x49,0x8d,0xab,0x88,0xfd,0xff,0xff,0x48,0x81,0xec,0x68,0x03,0x00,0x00}},
        {0xd79bf0,17,1,10,{0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xec,0x50,0x48,0x8b,0x05,0x6f,0x00,0x15,0x03}},
        {0x18a6b00,19,2,6,{0x4c,0x8b,0xc1,0x48,0x85,0xd2,0x0f,0x84,0xd8,0,0,0,0x0f,0xb7,0x82,0xa0,1,0,0}}
    }, {
        {0x14c6b10,18,0,0,{0x40,0x53,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x83,0x79,0x78,0x00,0x45,0x0f,0xb6,0xf8}},
        {0xd76620,14,2,12,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x3b,0xd1,0x75,0x08}},
        {0xd76870,19,0,0,{0x4c,0x8b,0xdc,0x55,0x53,0x49,0x8d,0xab,0x88,0xfd,0xff,0xff,0x48,0x81,0xec,0x68,0x03,0x00,0x00}},
        {0xd799b0,17,1,10,{0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xec,0x50,0x48,0x8b,0x05,0x3f,0xdc,0x84,0x04}},
        {0x18b36c0,19,2,6,{0x4c,0x8b,0xc1,0x48,0x85,0xd2,0x0f,0x84,0xd8,0,0,0,0x0f,0xb7,0x82,0xa0,1,0,0}}
    }
};
static const uint32_t g_skip[2]={0x4145520,0x5847a70};
typedef void (*mr_bounds_fn)(void *,unsigned char,unsigned char);
typedef unsigned char (*mr_compare_fn)(void *,void *);
typedef void (*mr_update_fn)(void *);
typedef void (*mr_bind_fn)(void *,void *);
static mr_bounds_fn g_bounds;
static mr_compare_fn g_compare;
static mr_update_fn g_update;
static mr_bind_fn g_bind;
static mr_bind_fn g_particle_material;
static const uint8_t *g_base;
static int g_renderer;
static int (*g_observe)(void *);
static int (*g_origin)(sh_resource_instance_origin *);
static volatile LONG g_ready,g_attempted;

static int mr_native_read(void *context,uintptr_t address,void *out,size_t length)
{
    (void)context;
    __try { memcpy(out,(const void *)address,length); return 1; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int mr_read(mr_probe *p,uintptr_t object,size_t offset,void *out,size_t length)
{
    if(p->failed || !object || object>UINTPTR_MAX-offset || length>p->remaining ||
        length>UINTPTR_MAX-(object+offset) || !p->read(p->context,object+offset,out,length)) {
        p->failed=1; return 0;
    }
    const unsigned char *bytes=out;
    for(size_t i=0;i<length;i++) { p->hash^=bytes[i]; p->hash*=UINT64_C(1099511628211); }
    p->remaining-=length; return 1;
}
static uintptr_t mr_pointer(mr_probe *p,uintptr_t object,size_t offset)
{ uintptr_t value=0; mr_read(p,object,offset,&value,sizeof(value)); return value; }
static int32_t mr_integer(mr_probe *p,uintptr_t object,size_t offset)
{ int32_t value=0; mr_read(p,object,offset,&value,sizeof(value)); return value; }
static unsigned char mr_byte(mr_probe *p,uintptr_t object,size_t offset)
{ unsigned char value=0; mr_read(p,object,offset,&value,sizeof(value)); return value; }
static void mr_observe(mr_probe *p,uintptr_t object)
{
    if(!object || (p->observe && !p->observe((void *)object))) { p->failed=1;return; }
    uint64_t token=sh_resource_instances_token((void *)object);
    if(!token) { p->failed=1;return; }
    const unsigned char *bytes=(const unsigned char *)&token;
    for(size_t i=0;i<sizeof token;i++) { p->hash^=bytes[i];p->hash*=UINT64_C(1099511628211); }
}
static void mr_bytes(mr_probe *p,uintptr_t address,size_t length)
{
    unsigned char bytes[128];
    while(length && !p->failed) {
        size_t part=length<sizeof(bytes)?length:sizeof(bytes);
        if(!mr_read(p,address,0,bytes,part)) break;
        address+=part; length-=part;
    }
}
static void mr_list(mr_probe *p,uintptr_t object,size_t count_offset,size_t stride)
{
    int32_t count=mr_integer(p,object,count_offset);
    if(count<0 || (size_t)count>SIZE_MAX/stride) { p->failed=1; return; }
    if(count) {
        uintptr_t data=mr_pointer(p,object,count_offset-8);
        mr_bytes(p,data,(size_t)count*stride);
    }
}
static int mr_skip(mr_probe *p)
{ return mr_integer(p,(uintptr_t)p->base,g_skip[p->renderer])!=0; }
static void mr_bounds(mr_probe *p,uintptr_t model)
{
    int32_t count=mr_integer(p,model,0x78);
    if(count<0) { p->failed=1; return; }
    if(!count || p->failed) return;
    uintptr_t surfaces=mr_pointer(p,model,0x70), material=mr_pointer(p,surfaces,0);
    mr_observe(p,material);
    if(mr_integer(p,material,0x6c)>0 || p->failed) return;
    for(int32_t i=0;i<count && !p->failed;i++) {
        if((size_t)i>SIZE_MAX/0x78) { p->failed=1; break; }
        size_t offset=(size_t)i*0x78;
        uintptr_t geometry=mr_pointer(p,surfaces,offset+0x18);
        material=mr_pointer(p,surfaces,offset);
        if(!geometry) { p->failed=1; break; }
        mr_observe(p,material); mr_byte(p,material,0x68);
    }
}
/* The comparison reads mutable material pointer fields, then immutable native
 * identity-name pointers of paired nonnull programs. It does not read program
 * shader content. Actual program content is observed separately by mr_update. */
static int mr_equal_identity(mr_probe *p,uintptr_t a,uintptr_t b,size_t offset)
{
    uintptr_t right=mr_pointer(p,b,offset), left=mr_pointer(p,a,offset);
    if(!left || !right) return left==right;
    return mr_pointer(p,left,8)==mr_pointer(p,right,8);
}
static void mr_compare(mr_probe *p,uintptr_t a,uintptr_t b)
{
    if(a==b) return;
    mr_observe(p,a);
    if((mr_byte(p,a,0x6a)&2) || p->failed) return;
    mr_observe(p,b);
    static const size_t common[]={0xf0,0xf8,0x110,0x130};
    for(size_t i=0;i<sizeof(common)/sizeof(*common);i++)
        if(!mr_equal_identity(p,a,b,common[i]) || p->failed) return;
    if(!(mr_byte(p,a,0x69)&1)) {
        if(mr_pointer(p,a,0x100)) return;
        if(mr_pointer(p,b,0x100)) return;
        if(mr_pointer(p,a,0x108)) return;
        mr_pointer(p,b,0x108);
    } else {
        if(!mr_equal_identity(p,a,b,0x100) || p->failed) return;
        mr_equal_identity(p,a,b,0x108);
    }
    /* Membership reads only the two actual fixed definition indices and own
     * op lists; fixed ABI guards preserve their metadata. Evaluated default
     * payloads, if reached elsewhere, use resource_parm_reads. */
}
static void mr_program(mr_probe *p,uintptr_t program)
{
    mr_observe(p,program);
    uintptr_t data=mr_pointer(p,program,0x70);
    if(!data || p->failed) return;
    /* The renamed program is another registered resource, not an inline
     * allocation owned solely by the outer program. Its generation supplies
     * the lists actually consumed by the native collector. */
    mr_observe(p,data);
    for(size_t stage=0;stage<3 && !p->failed;stage++) {
        for(size_t item=0;item<2;item++) mr_list(p,data,0x120+(stage*2+item)*0xd8,2);
        mr_list(p,data,0x630+stage*0x58,2); mr_list(p,data,0x738+stage*0x58,2);
        mr_list(p,data,0x840+stage*0x28,2); mr_list(p,data,0x9a8+stage*0x38,2);
    }
}
static void mr_update(mr_probe *p,uintptr_t material)
{
    if(mr_skip(p) || p->failed) return;
    mr_observe(p,material);
    int32_t count=mr_integer(p,material,0x78);
    if(count<0 || (size_t)count>SIZE_MAX/16) { p->failed=1; return; }
    if(count) {
        mr_bytes(p,mr_pointer(p,material,0x70),(size_t)count*8);
        mr_bytes(p,mr_pointer(p,material,0x88),(size_t)count*16);
    }
    for(size_t i=0;i<9 && !p->failed;i++) {
        uintptr_t program=mr_pointer(p,material,0xf0+i*8);
        if(program) mr_program(p,program);
    }
}
static void mr_bind(mr_probe *p,uintptr_t block,uintptr_t provider)
{
    if(mr_skip(p) || p->failed) return;
    uintptr_t mode=mr_pointer(p,provider,0x3d8);
    if(!mode || p->failed) return;
    p->eligible=1;
    if(block<0x70) { p->failed=1; return; }
    mr_observe(p,block-0x70);
    mr_integer(p,mode,0); mr_integer(p,provider,0x1c); mr_integer(p,provider,0x2c0);
    uintptr_t record=mr_pointer(p,mode,p->renderer?0x100:0x160);
    uintptr_t definition=record ? mr_pointer(p,record,0x10) :
        mr_pointer(p,(uintptr_t)p->base,p->renderer?0x47da7f0:0x30dcdc0);
    mr_integer(p,definition,0x68);
    uintptr_t manager=0;
    const size_t provider_fields[]={0x3f0,0x3e8}, mode_fields[]={0xe0,0xb0,0xb8,0xd0};
    for(size_t i=0;i<2 && !p->failed;i++) if(!mr_pointer(p,provider,provider_fields[i])) {
        if(!manager) manager=mr_pointer(p,(uintptr_t)p->base,p->renderer?0x47d42d0:0x30d6fa0);
        mr_pointer(p,manager,0x20);
    }
    for(size_t i=0;i<4 && !p->failed;i++) if(!mr_pointer(p,mode,mode_fields[i])) {
        if(!manager) manager=mr_pointer(p,(uintptr_t)p->base,p->renderer?0x47d42d0:0x30d6fa0);
        mr_pointer(p,manager,0x20);
    }
    /* Pointer copies are owned through this actual provider generation, not
     * falsely attributed to an image file or a current provider selected by name. */
}
static void mr_particle_material(mr_probe *p,uintptr_t material)
{
    if(!material) return;
    mr_observe(p,material);
    mr_bytes(p,material+0x1a0,4);mr_bytes(p,material+0x58,16);
    unsigned char flags=mr_byte(p,material,0x68);
    mr_pointer(p,material,0x100);
    uintptr_t program=mr_pointer(p,material,0x110);
    /* Native stage bit1 is assigned from material bit3 before this branch.
     * A copied program pointer alone does not constitute a program-body read. */
    if(p->failed || (flags&8) || !program) return;
    mr_observe(p,program);
    if(mr_byte(p,program,0x169b)) mr_bytes(p,program+0xe0,8);
    else mr_bytes(p,material+0xd0,8);
}
static mr_probe mr_begin(int renderer,const uint8_t *base,mr_reader read,void *context,int (*observe)(void *))
{
    mr_probe p={read,context,base,renderer,observe,UINT64_C(14695981039346656037),64u*1024u*1024u,0,0};
    if(renderer<0 || renderer>1 || !read || !base) p.failed=1;
    return p;
}
static void mr_probe_kind(mr_probe *p,int kind,void *a,void *b)
{
    if(p->failed) return;
    if(kind==MR_BOUNDS) mr_bounds(p,(uintptr_t)a);
    else if(kind==MR_COMPARE) mr_compare(p,(uintptr_t)a,(uintptr_t)b);
    else if(kind==MR_UPDATE) mr_update(p,(uintptr_t)a);
    else if(kind==MR_BIND) mr_bind(p,(uintptr_t)a,(uintptr_t)b);
    else if(kind==MR_PARTICLE_MATERIAL) mr_particle_material(p,(uintptr_t)b);
    else p->failed=1;
}
static int mr_collect(int kind,void *a,void *b,mr_probe *out)
{
    *out=mr_begin(g_renderer,g_base,mr_native_read,NULL,g_observe);
    mr_probe_kind(out,kind,a,b); if(out->failed) g_observe(NULL); return !out->failed;
}
static void mr_finish(int kind,void *a,void *b,const mr_probe *before,int returned)
{
    mr_probe after=mr_begin(g_renderer,g_base,mr_native_read,NULL,NULL);
    if(returned) mr_probe_kind(&after,kind,a,b);
    if(!returned || before->failed || after.failed || before->hash!=after.hash) g_observe(NULL);
}
static void mr_bounds_hook(void *model,unsigned char prepare,unsigned char tangents)
{
    int observing=sh_resource_model_reads_ready() && sh_resource_graph_native_observing(), returned=0; mr_probe p;
    if(observing) mr_collect(MR_BOUNDS,model,NULL,&p);
    __try { g_bounds(model,prepare,tangents); returned=1; }
    __finally { if(observing) mr_finish(MR_BOUNDS,model,NULL,&p,returned); }
}
static unsigned char mr_compare_hook(void *a,void *b)
{
    int observing=sh_resource_model_reads_ready() && sh_resource_graph_native_observing(), returned=0; unsigned char result=0; mr_probe p;
    if(observing) mr_collect(MR_COMPARE,a,b,&p);
    __try { result=g_compare(a,b); returned=1; }
    __finally { if(observing) mr_finish(MR_COMPARE,a,b,&p,returned); }
    return result;
}
static void mr_update_hook(void *material)
{
    int observing=sh_resource_model_reads_ready() && sh_resource_graph_native_observing(), returned=0; mr_probe p;
    if(observing) mr_collect(MR_UPDATE,material,NULL,&p);
    __try { g_update(material); returned=1; }
    __finally { if(observing) mr_finish(MR_UPDATE,material,NULL,&p,returned); }
}
static void mr_bind_hook(void *block,void *provider)
{
    int observing=sh_resource_model_reads_ready() && sh_resource_graph_native_observing(), returned=0; mr_probe p;
    sh_vt_provider_read receipt={0};
    if(observing) {
        mr_collect(MR_BIND,block,provider,&p);
        if(p.eligible && (!sh_vt_provider_read_begin(provider,&receipt) || !g_origin(receipt.origin))) g_observe(NULL);
    }
    __try { g_bind(block,provider); returned=1; }
    __finally {
        if(observing) {
            mr_finish(MR_BIND,block,provider,&p,returned);
            if(receipt.active && !sh_vt_provider_read_finish(&receipt,returned,1)) g_observe(NULL);
        }
    }
}
static void mr_particle_material_hook(void *stage,void *material)
{
    int observing=sh_resource_model_reads_ready() && sh_resource_graph_native_observing(),returned=0;mr_probe p;
    if(observing) mr_collect(MR_PARTICLE_MATERIAL,stage,material,&p);
    __try { g_particle_material(stage,material);returned=1; }
    __finally { if(observing) mr_finish(MR_PARTICLE_MATERIAL,stage,material,&p,returned); }
}
int sh_resource_model_reads_install(const uint8_t *base,int (*observe)(void *),int (*origin)(sh_resource_instance_origin *))
{
    if(InterlockedCompareExchange(&g_attempted,1,0)) return sh_resource_model_reads_ready();
    int renderer=sh_host_is_vulkan();
    if(!base || !observe || !origin || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer)) return 0;
    __try { for(size_t i=0;i<MR_ROUTES;i++) {
        const mr_route *r=g_routes[renderer]+i;
        if(memcmp(base+r->rva,r->bytes,r->length)) return 0;
    } } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_base=base; g_renderer=renderer; g_observe=observe; g_origin=origin;
    void *detours[]={mr_bounds_hook,mr_compare_hook,mr_update_hook,mr_bind_hook,mr_particle_material_hook}, *prepared[MR_ROUTES]={0};
    for(size_t i=0;i<MR_ROUTES;i++) {
        const mr_route *r=g_routes[renderer]+i; void *target=(void *)(base+r->rva);
        prepared[i]=r->relocation==1 ? hook_prepare_rip_load(target,detours[i],r->length,r->offset) :
            r->relocation==2 ? hook_prepare_conditional_jump(target,detours[i],r->length,r->offset) : hook_prepare(target,detours[i],r->length);
        if(!prepared[i]) goto failed;
    }
    g_bounds=(mr_bounds_fn)prepared[MR_BOUNDS]; g_compare=(mr_compare_fn)prepared[MR_COMPARE];
    g_update=(mr_update_fn)prepared[MR_UPDATE]; g_bind=(mr_bind_fn)prepared[MR_BIND];
    g_particle_material=(mr_bind_fn)prepared[MR_PARTICLE_MATERIAL];
    for(size_t i=0;i<MR_ROUTES;i++) if(hook_commit(prepared[i])!=B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_ready,1); return 1;
failed:
    /* Retain callbacks whose failed removal still owns a reachable detour. */
    for(size_t i=MR_ROUTES;i;i--) if(prepared[i-1]) (void)hook_unpatch(prepared[i-1]);
    return 0;
}
int sh_resource_model_reads_ready(void)
{
    if(!InterlockedCompareExchange(&g_ready,0,0) || !g_base ||
       sh_host_is_vulkan()!=g_renderer || !sh_host_is_pinned_renderer_build(g_renderer)) return 0;
    void *trampolines[]={g_bounds,g_compare,g_update,g_bind,g_particle_material};
    void *detours[]={mr_bounds_hook,mr_compare_hook,mr_update_hook,mr_bind_hook,mr_particle_material_hook};
    __try { for(size_t i=0;i<MR_ROUTES;i++) {
        static const unsigned char jump[6]={0xff,0x25,0,0,0,0};void *target=NULL;
        const unsigned char *entry=g_base+g_routes[g_renderer][i].rva;
        if(!trampolines[i] || !hook_is_installed(trampolines[i]) || memcmp(entry,jump,6)) return 0;
        memcpy(&target,entry+6,8);if(target!=detours[i]) return 0;
    } } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
int sh_resource_model_reads_default_ready(const char *family)
{
    if(family && (!_stricmp(family,"model") || !_stricmp(family,"particle"))) return sh_resource_model_reads_ready();
    if(family && (!_stricmp(family,"discreteanimation") || !_stricmp(family,"material")))
        return sh_resource_model_reads_ready() && sh_vt_provider_provenance_ready() && sh_resource_parm_reads_ready();
    return 1;
}

#ifdef SH_RESOURCE_MODEL_READS_TESTING
int sh_resource_model_reads_test_probe(int kind,int renderer,const uint8_t *base,void *a,void *b,
    sh_resource_model_reader read,void *context,int (*observe)(void *))
{
    mr_probe p=mr_begin(renderer,base,read,context,observe); mr_probe_kind(&p,kind,a,b);
    if(p.failed && observe) observe(NULL); return !p.failed;
}
void sh_resource_model_reads_test_reset(void)
{ InterlockedExchange(&g_ready,0); InterlockedExchange(&g_attempted,0); }
#endif
