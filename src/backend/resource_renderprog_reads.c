#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>
#include "resource_renderprog_reads.h"
#include "resource_graph.h"
#include "resource_graph_native.h"
#include "resource_instances.h"
#include "host_image.h"
#include "hook.h"
#include "native_pin_hash.h"
#include "resource_renderprog_read_site.h"

typedef struct rp_route { uint32_t rva;size_t length;unsigned char stolen,relocation;const unsigned char *digest,*tail_digest; } rp_route;
typedef struct rp_image {
    uint32_t parser;size_t parser_length;const unsigned char *parser_digest;
    uint32_t ready_return,index_return,holder,object,table,flag,list,unwind;
    uint32_t stage;size_t stage_length;const unsigned char *stage_digest;uint32_t find_return;
    rp_route routes[2];
} rp_image;
#include "resource_renderprog_read_pins.h"
typedef struct rp_sampler_site {uint32_t rva;size_t length;unsigned argument_register;} rp_sampler_site;
typedef struct rp_sampler_region {uint32_t rva;size_t length;const unsigned char *digest;} rp_sampler_region;
typedef struct rp_sampler_image {
    uint32_t begin,end,unwind;rp_sampler_site sites[5];rp_sampler_region regions[3];
} rp_sampler_image;
#include "resource_renderprog_read_site_pins.h"
static rp_read_site rp_sampler_hooks[5];
typedef struct rp_flag_image {
    uint32_t begin,end,unwind,site;size_t length;const unsigned char *digest;
    rp_sampler_region regions[3];
} rp_flag_image;
#include "resource_renderprog_flag_pins.h"
static rp_read_site rp_flag_hook;
typedef unsigned char (*rp_ready_fn)(void *);
typedef void *(*rp_index_fn)(void *,int,uintptr_t,uintptr_t);
typedef const char *(*rp_name_fn)(void *,int);
static const uint32_t rp_name_rvas[2]={0x19211b0,0x192e560};
static const uint32_t rp_name_managers[2]={0x3e5e248,0x555bb90};
static const unsigned char rp_name_code[16]={0x48,0x63,0xc2,0x48,0x8b,0x04,0xc1,0x48,0x8b,0x40,0x08,0xc3,0xcc,0xcc,0xcc,0xcc};
static const uint8_t *rp_base;
static int rp_renderer;
static int (*rp_observe)(void *);
static rp_ready_fn rp_ready_original;
static rp_index_fn rp_index_original;
static rp_name_fn rp_name_original;
static volatile LONG rp_installed,rp_attempted;
static unsigned char rp_ready_hook(void *);
static void *rp_index_hook(void *,int,uintptr_t,uintptr_t);
static void rp_sampler_reached(void *);
static void rp_name_reached(void *);
static void rp_flag_reached(void *);
static const char *rp_name_hook(void *,int);

static int rp_digest(const unsigned char *bytes,size_t length,const unsigned char expected[32])
{
    sh_native_pin_hash hash;unsigned char actual[32];
    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,bytes,length);
    sh_native_pin_hash_finish(&hash,actual);return !memcmp(actual,expected,sizeof actual);
}
static int rp_stage_matches(const uint8_t *base,int renderer,int patched)
{
    const rp_image *image=rp_images+renderer;const rp_sampler_image *s=rp_sampler_images+renderer;
    sh_native_pin_hash hash;unsigned char actual[32];size_t offset=0;
    sh_native_pin_hash_init(&hash);
    for(size_t i=0;i<5;i++) {
        size_t next=s->sites[i].rva-image->stage;
        const unsigned char *signature=i<2?(i?rp_sampler_append:rp_sampler_compare):rp_parm_name;
        size_t length=i<2?(i?sizeof rp_sampler_append:sizeof rp_sampler_compare):sizeof rp_parm_name;
        if(next<offset || next>image->stage_length || length!=s->sites[i].length ||
           length>image->stage_length-next)return 0;
        sh_native_pin_hash_update(&hash,base+image->stage+offset,next-offset);
        if(patched) {if(!rp_site_ready(rp_sampler_hooks+i))return 0;}
        else if(memcmp(base+s->sites[i].rva,signature,length))return 0;
        /* Only the owned, separately checked short hook window is restored
         * in the digest. Every other original byte still participates. */
        sh_native_pin_hash_update(&hash,signature,length);offset=next+length;
    }
    sh_native_pin_hash_update(&hash,base+image->stage+offset,image->stage_length-offset);
    sh_native_pin_hash_finish(&hash,actual);
    if(memcmp(actual,image->stage_digest,sizeof actual))return 0;
    for(size_t i=0;i<3;i++)if(!rp_digest(base+s->regions[i].rva,s->regions[i].length,s->regions[i].digest))return 0;
    for(size_t i=0;i<5;i++) {
        DWORD64 actual_base=0;PRUNTIME_FUNCTION native=RtlLookupFunctionEntry((DWORD64)(uintptr_t)base+s->sites[i].rva,&actual_base,NULL);
        if(!native || actual_base!=(DWORD64)(uintptr_t)base || native->BeginAddress!=s->begin ||
           native->EndAddress!=s->end || native->UnwindData!=s->unwind)return 0;
    }
    return 1;
}
static int rp_flag_matches(const uint8_t *base,int renderer,int patched)
{
    const rp_flag_image *f=rp_flag_images+renderer;size_t offset=f->site-f->begin;
    sh_native_pin_hash hash;unsigned char actual[32];
    if(f->site<f->begin || offset>f->length || sizeof rp_flag_test>f->length-offset)return 0;
    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,base+f->begin,offset);
    if(patched){if(!rp_site_ready(&rp_flag_hook))return 0;}
    else if(memcmp(base+f->site,rp_flag_test,sizeof rp_flag_test))return 0;
    sh_native_pin_hash_update(&hash,rp_flag_test,sizeof rp_flag_test);offset+=sizeof rp_flag_test;
    sh_native_pin_hash_update(&hash,base+f->begin+offset,f->length-offset);
    sh_native_pin_hash_finish(&hash,actual);if(memcmp(actual,f->digest,sizeof actual))return 0;
    for(size_t i=0;i<3;i++)if(!rp_digest(base+f->regions[i].rva,f->regions[i].length,f->regions[i].digest))return 0;
    DWORD64 actual_base=0;
    PRUNTIME_FUNCTION native=RtlLookupFunctionEntry((DWORD64)(uintptr_t)base+f->site,&actual_base,NULL);
    return native && actual_base==(DWORD64)(uintptr_t)base && native->BeginAddress==f->begin &&
        native->EndAddress==f->end && native->UnwindData==f->unwind;
}

static void rp_unknown(void)
{
    int notified=0;
    __try { if(rp_observe){(void)rp_observe(NULL);notified=1;} }
    __except(EXCEPTION_EXECUTE_HANDLER){notified=0;}
    if(!notified) {
        __try{(void)sh_resource_graph_native_observe_resource(NULL);}
        __except(EXCEPTION_EXECUTE_HANDLER){sh_resource_graph_incomplete();}
    }
}
/* Every observed payload below is used on the native normal-return path.
 * This is deliberately not a prediction from an earlier Find or a name. */
typedef struct rp_snapshot {
    uintptr_t resource,array,association;int32_t count;
    uint64_t value;unsigned char flags;
} rp_snapshot;
static int rp_snapshot_read(int index_route,void *receiver,int index,void *returned,rp_snapshot *s)
{
    const rp_image *image=rp_images+rp_renderer;memset(s,0,sizeof *s);
    __try {
        if(!index_route) {
            if(receiver!=(void *)(rp_base+image->object) ||
               *(void *const *)(rp_base+image->holder)!=receiver ||
               *(void *const *)receiver!=rp_base+image->table ||
               *(void *const *)(rp_base+image->table+0x40)!=rp_base+image->routes[0].rva)return 0;
            memcpy(&s->resource,rp_base+image->flag,8);
            if(!s->resource || s->resource>UINTPTR_MAX-0x60)return 0;
            memcpy(&s->value,(const uint8_t *)s->resource+0x58,8);
        } else if(index_route==1) {
            if(receiver!=(void *)(rp_base+image->list) || index<0 || !returned)return 0;
            memcpy(&s->count,(const uint8_t *)receiver+0x28,4);
            if(index>=s->count)return 0;
            memcpy(&s->array,(const uint8_t *)receiver+0x20,8);
            if(!s->array || (uintptr_t)index>(UINTPTR_MAX-s->array)/8)return 0;
            memcpy(&s->association,(const void *)(s->array+(uintptr_t)index*8),8);
            if(s->association!=(uintptr_t)returned)return 0;
            s->resource=(uintptr_t)returned;if(s->resource>UINTPTR_MAX-0x1830)return 0;
            memcpy(&s->value,(const uint8_t *)returned+0x1828,8);
        } else if(index_route==2) {
            if(!returned || (uintptr_t)returned>UINTPTR_MAX-0x70)return 0;
            s->resource=(uintptr_t)returned;
            memcpy(&s->count,(const uint8_t *)returned+0x6c,4);
            if(s->count==13) {
                memcpy(&s->flags,(const uint8_t *)returned+0x64,1);
                memcpy(&s->association,(const uint8_t *)returned+0x58,8);
                if(s->association) {
                    if(s->association>UINTPTR_MAX-0x6c)return 0;
                    memcpy(&s->value,(const uint8_t *)s->association+0x68,4);
                }
            }
        } else if(index_route==6) {
            /* This reached RAX is the later translator load, independent of
             * the earlier Ready-return observation of the same static slot. */
            memcpy(&s->association,rp_base+image->flag,8);
            if(!returned || s->association!=(uintptr_t)returned ||
               (uintptr_t)returned>UINTPTR_MAX-0x60)return 0;
            s->resource=(uintptr_t)returned;
            memcpy(&s->value,(const uint8_t *)returned+0x58,8);
        } else if(index_route==5) {
            uintptr_t manager=0;int32_t actual_index;
            memcpy(&manager,rp_base+rp_name_managers[rp_renderer],8);
            if(!manager || (void *)manager!=receiver || index<0 || index>=0x93f ||
               (uintptr_t)index>(UINTPTR_MAX-manager)/8)return 0;
            s->array=manager;s->count=index;
            memcpy(&s->resource,(const void *)(manager+(uintptr_t)index*8),8);
            if(!s->resource || s->resource>UINTPTR_MAX-0x6c)return 0;
            memcpy(&actual_index,(const uint8_t *)s->resource+0x68,4);
            if(actual_index!=index)return 0;
            memcpy(&s->value,(const uint8_t *)s->resource+8,8);
        } else {
            if(!returned || (uintptr_t)returned>UINTPTR_MAX-0x6c)return 0;
            s->resource=(uintptr_t)returned;
            memcpy(&s->value,(const uint8_t *)returned+(index_route==3?0x68:8),index_route==3?4:8);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return 1;
}
static void rp_reached(int index_route,void *receiver,int index,void *returned)
{
    int valid=0;
    __try {
        rp_snapshot before,after;
        if(sh_resource_renderprog_reads_ready() && rp_snapshot_read(index_route,receiver,index,returned,&before)) {
            uintptr_t resources[]={before.resource,index_route==2?before.association:0};
            uint64_t prior[2]={0},tokens[2]={0};int captured=1;
            for(size_t i=0;i<2;i++)if(resources[i] && (i==0 || resources[i]!=resources[0])) {
                prior[i]=sh_resource_instances_token((void *)resources[i]);
                if(!rp_observe || !rp_observe((void *)resources[i])){captured=0;break;}
                tokens[i]=sh_resource_instances_token((void *)resources[i]);
                if(!tokens[i] || (prior[i] && tokens[i]!=prior[i])){captured=0;break;}
            }
            if(captured) {
                valid=rp_snapshot_read(index_route,receiver,index,returned,&after) &&
                    !memcmp(&before,&after,sizeof before) && sh_resource_renderprog_reads_ready();
                for(size_t i=0;i<2;i++)if(tokens[i] && sh_resource_instances_token((void *)resources[i])!=tokens[i])valid=0;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){valid=0;}
    if(!valid)rp_unknown();
}
static unsigned char rp_ready_at(void *receiver,uintptr_t caller)
{
    /* Both native parsers load flag+58 before testing the Ready result. Even
     * AL==0 therefore observes this actual flag. A throwing Ready has not
     * reached that load and forwards its original exception without capture. */
    unsigned char result=rp_ready_original(receiver);
    DWORD error=GetLastError();
    __try{if(sh_resource_graph_native_observing() && rp_base &&
       caller==(uintptr_t)rp_base+rp_images[rp_renderer].ready_return)rp_reached(0,receiver,0,NULL);}
    __finally{SetLastError(error);}
    return result;
}
static void *rp_index_at(void *receiver,int index,uintptr_t third,uintptr_t fourth,uintptr_t caller)
{
    void *result=rp_index_original(receiver,index,third,fourth);
    DWORD error=GetLastError();
    __try{if(sh_resource_graph_native_observing() && rp_base &&
       caller==(uintptr_t)rp_base+rp_images[rp_renderer].index_return)rp_reached(1,receiver,index,result);}
    __finally{SetLastError(error);}
    return result;
}
static unsigned char rp_ready_hook(void *receiver){return rp_ready_at(receiver,(uintptr_t)_ReturnAddress());}
static void *rp_index_hook(void *receiver,int index,uintptr_t third,uintptr_t fourth)
{return rp_index_at(receiver,index,third,fourth,(uintptr_t)_ReturnAddress());}
void sh_resource_renderprog_reads_find_return(void *resource,uintptr_t caller)
{
    if(sh_resource_graph_native_observing() && rp_base &&
       caller==(uintptr_t)rp_base+rp_images[rp_renderer].find_return)
        rp_reached(2,NULL,0,resource);
}
static void rp_inline_reached(void *resource,int route)
{
    DWORD error=GetLastError();
    __try {
        __try{if(sh_resource_graph_native_observing())rp_reached(route,NULL,0,resource);}
        __except(EXCEPTION_EXECUTE_HANDLER){rp_unknown();}
    } __finally {SetLastError(error);}
}
static void rp_sampler_reached(void *resource){rp_inline_reached(resource,3);}
static void rp_name_reached(void *resource){rp_inline_reached(resource,4);}
static void rp_flag_reached(void *resource){rp_inline_reached(resource,6);}
static const char *rp_name_hook(void *manager,int index)
{
    DWORD entry_error=GetLastError();int active=sh_resource_graph_native_observing(),valid=0,returned=0;rp_snapshot before={0},after;
    uint64_t token=0;const char *result=NULL;
    __try {
        if(active && sh_resource_renderprog_reads_ready() && rp_snapshot_read(5,manager,index,NULL,&before)) {
            rp_reached(5,manager,index,NULL);token=sh_resource_instances_token((void *)before.resource);valid=token!=0;
        } else if(active)rp_unknown();
    } __except(EXCEPTION_EXECUTE_HANDLER){if(active)rp_unknown();}
    SetLastError(entry_error);
    __try {result=rp_name_original(manager,index);returned=1;}
    __finally {
        DWORD error=GetLastError();
        __try {
            if(active && (!returned || !valid || (uintptr_t)result!=before.value ||
               !rp_snapshot_read(5,manager,index,NULL,&after) || memcmp(&before,&after,sizeof before) ||
               sh_resource_instances_token((void *)before.resource)!=token || !sh_resource_renderprog_reads_ready()))rp_unknown();
        } __except(EXCEPTION_EXECUTE_HANDLER){if(active)rp_unknown();}
        SetLastError(error);
    }
    return result;
}
int sh_resource_renderprog_reads_install(const uint8_t *base,int (*observe)(void *))
{
    if(InterlockedCompareExchange(&rp_attempted,1,0))return sh_resource_renderprog_reads_ready();
    int renderer=sh_host_is_vulkan();
    if(!base || !observe || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer))return 0;
    const rp_image *image=rp_images+renderer;
    __try {
        if(!rp_digest(base+image->parser,image->parser_length,image->parser_digest))return 0;
        if(!rp_stage_matches(base,renderer,0))return 0;
        if(!rp_flag_matches(base,renderer,0))return 0;
        for(size_t i=0;i<2;i++)if(!rp_digest(base+image->routes[i].rva,image->routes[i].length,image->routes[i].digest))return 0;
        if(memcmp(base+rp_name_rvas[renderer],rp_name_code,sizeof rp_name_code))return 0;
        if(image->unwind && memcmp(base+image->unwind,"\x01\x04\x01\x00\x04\x42\x00\x00",8))return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    rp_base=base;rp_renderer=renderer;rp_observe=observe;
    void *prepared[3]={0};void *detours[]={rp_ready_hook,rp_index_hook,rp_name_hook};
    for(size_t i=0;i<5;i++) {
        const rp_sampler_site *site=rp_sampler_images[renderer].sites+i;
        if(!rp_site_prepare(rp_sampler_hooks+i,(unsigned char *)base+site->rva,
                            i<2?(i?rp_sampler_append:rp_sampler_compare):rp_parm_name,site->length,
                            site->argument_register,i<2?rp_sampler_reached:rp_name_reached))goto failed;
    }
    if(!rp_site_prepare(&rp_flag_hook,(unsigned char *)base+rp_flag_images[renderer].site,
                        rp_flag_test,sizeof rp_flag_test,0,rp_flag_reached))goto failed;
    const rp_route *r=image->routes;
    prepared[0]=r->relocation?hook_prepare_rip_indirect_call((void *)(base+r->rva),detours[0],r->stolen,4):
        hook_prepare((void *)(base+r->rva),detours[0],r->stolen);
    if(!prepared[0])goto failed;
    r++;
    prepared[1]=hook_prepare((void *)(base+r->rva),detours[1],r->stolen);
    if(!prepared[1])goto failed;
    prepared[2]=hook_prepare((void *)(base+rp_name_rvas[renderer]),detours[2],14);
    if(!prepared[2])goto failed;
    rp_ready_original=(rp_ready_fn)prepared[0];rp_index_original=(rp_index_fn)prepared[1];
    rp_name_original=(rp_name_fn)prepared[2];
    if(!sh_resource_graph_native_set_find_return_observer(sh_resource_renderprog_reads_find_return))goto failed;
    for(size_t i=0;i<3;i++)if(hook_commit(prepared[i])!=B2_PATCH_OK)goto failed;
    for(size_t i=0;i<5;i++)if(!rp_site_commit(rp_sampler_hooks+i))goto failed;
    if(!rp_site_commit(&rp_flag_hook))goto failed;
    InterlockedExchange(&rp_installed,1);return 1;
failed:
    /* Retain callbacks when failed native rollback leaves a trampoline live. */
    (void)rp_site_release(&rp_flag_hook);
    for(size_t i=5;i;i--)(void)rp_site_release(rp_sampler_hooks+i-1);
    for(size_t i=3;i;i--)if(prepared[i-1])(void)hook_unpatch(prepared[i-1]);
    return 0;
}
int sh_resource_renderprog_reads_ready(void)
{
    if(!InterlockedCompareExchange(&rp_installed,0,0) || !rp_base || sh_host_is_vulkan()!=rp_renderer ||
       !sh_host_is_pinned_renderer_build(rp_renderer))return 0;
    if(!sh_resource_graph_native_find_return_observer_ready(sh_resource_renderprog_reads_find_return))return 0;
    const rp_image *image=rp_images+rp_renderer;
    void *trampolines[]={rp_ready_original,rp_index_original,rp_name_original},*detours[]={rp_ready_hook,rp_index_hook,rp_name_hook};
    __try {
        if(*(void *const *)(rp_base+image->holder)!=rp_base+image->object ||
           *(void *const *)(rp_base+image->object)!=rp_base+image->table ||
           *(void *const *)(rp_base+image->table+0x40)!=rp_base+image->routes[0].rva)return 0;
        if(!rp_digest(rp_base+image->parser,image->parser_length,image->parser_digest))return 0;
        if(!rp_stage_matches(rp_base,rp_renderer,1))return 0;
        if(!rp_flag_matches(rp_base,rp_renderer,1))return 0;
        if(image->unwind && memcmp(rp_base+image->unwind,"\x01\x04\x01\x00\x04\x42\x00\x00",8))return 0;
        for(size_t i=0;i<2;i++) {
            const rp_route *r=image->routes+i;const unsigned char *entry=rp_base+r->rva;void *destination=NULL;
            if(!trampolines[i] || !hook_is_installed(trampolines[i]) || memcmp(entry,"\xff\x25\x00\x00\x00\x00",6))return 0;
            memcpy(&destination,entry+6,8);if(destination!=detours[i])return 0;
            for(size_t j=14;j<r->stolen;j++)if(entry[j]!=0x90)return 0;
            if(!rp_digest(entry+r->stolen,r->length-r->stolen,r->tail_digest))return 0;
        }
        const unsigned char *entry=rp_base+rp_name_rvas[rp_renderer];void *destination=NULL;
        if(!trampolines[2] || !hook_is_installed(trampolines[2]) || memcmp(entry,"\xff\x25\0\0\0\0",6))return 0;
        memcpy(&destination,entry+6,8);
        if(destination!=detours[2] || memcmp(entry+14,rp_name_code+14,2))return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return 1;
}
