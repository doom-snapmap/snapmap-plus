/* Actual return witnesses; hook installation is a seam qualified separately. */
#define WIN32_LEAN_AND_MEAN
#define SH_PATCH_TESTING
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static PRUNTIME_FUNCTION test_native_lookup(DWORD64 pc,DWORD64 *base,PUNWIND_HISTORY_TABLE history);
#define RtlLookupFunctionEntry test_native_lookup
#include "../src/backend/resource_renderprog_reads.c"
#undef RtlLookupFunctionEntry
void backend_log(const char *s){(void)s;}
#include "../src/backend/patch.c"
#include "native_pin_fixture.h"
static void rp_fixture(int renderer,uint32_t rva,void *out,size_t length)
{ if(!sh_native_pin_fixture_copy(renderer,rva,out,length))abort(); }

static int renderer,pinned=1,observing=1,installed=1,prepares,commits,removes;
static int fail_prepare=-1,fail_commit=-1,fail_remove;
static int find_ready=1,fail_find_registration;static sh_resource_return_observer find_observer;
static void *targets[3],*destinations[3];static size_t lengths[3];static int relocated[3];
static int unknown,fallback,graph_failed,throw_observer,throw_unknown,changed_token,mutation;
static void *seen,*reject,*unknown_token;static size_t seen_count;
static unsigned char *mutated_field;
static int mutated_once;
static DWORD observe_error;
static unsigned char *lookup_base;static int lookup_ready=1;
static PRUNTIME_FUNCTION test_native_lookup(DWORD64 pc,DWORD64 *base,PUNWIND_HISTORY_TABLE history)
{
    (void)history;static RUNTIME_FUNCTION native;
    const rp_sampler_image *s=rp_sampler_images+renderer;
    const rp_flag_image *f=rp_flag_images+renderer;
    if(lookup_ready && pc>=(DWORD64)(uintptr_t)lookup_base+f->begin && pc<(DWORD64)(uintptr_t)lookup_base+f->end) {
        *base=(DWORD64)(uintptr_t)lookup_base;native=(RUNTIME_FUNCTION){f->begin,f->end,f->unwind};return &native;
    }
    if(!lookup_ready || pc<(DWORD64)(uintptr_t)lookup_base+s->begin || pc>=(DWORD64)(uintptr_t)lookup_base+s->end)return NULL;
    *base=(DWORD64)(uintptr_t)lookup_base;native=(RUNTIME_FUNCTION){s->begin,s->end,s->unwind};return &native;
}
int sh_host_is_vulkan(void){return renderer;}
int sh_host_is_pinned_renderer_build(int r){return pinned && r==renderer;}
int sh_resource_graph_native_observing(void){return observing;}
int sh_resource_graph_native_set_find_return_observer(sh_resource_return_observer observer)
{if(fail_find_registration)return 0;if(find_observer && find_observer!=observer)return 0;find_observer=observer;return 1;}
int sh_resource_graph_native_find_return_observer_ready(sh_resource_return_observer observer)
{return find_ready && find_observer==observer;}
void sh_resource_graph_incomplete(void){graph_failed++;}
int sh_resource_graph_native_observe_resource(void *object){assert(!object);fallback++;return 0;}
uint64_t sh_resource_instances_token(const void *object){return object==unknown_token?0:object?(changed_token?9:7):0;}
static int observe(void *object)
{
    if(!object){unknown++;if(throw_unknown)RaiseException(0xe0327001,0,0,NULL);return 0;}
    seen=object;seen_count++;
    if(observe_error)SetLastError(observe_error);
    if(throw_observer)RaiseException(0xe0327002,0,0,NULL);
    if(mutation==1)changed_token=1;
    if(mutation==2 && !mutated_once){(*mutated_field)^=1;mutated_once=1;}
    return object!=reject;
}
static void clear(void)
{unknown=fallback=graph_failed=throw_observer=throw_unknown=changed_token=mutation=mutated_once=0;observe_error=0;seen=NULL;seen_count=0;reject=unknown_token=NULL;}
static void *prepare(void *target,void *detour,size_t size,int relocation)
{
    int i=prepares++;assert(i<3);if(i==fail_prepare)return NULL;
    targets[i]=target;destinations[i]=detour;lengths[i]=size;relocated[i]=relocation;return target;
}
void *hook_prepare(void *target,void *detour,size_t size){return prepare(target,detour,size,0);}
void *hook_prepare_rip_indirect_call(void *target,void *detour,size_t size,size_t offset)
{assert(offset==4);return prepare(target,detour,size,1);}
sh_patch_status hook_commit(void *tramp)
{
    int i=commits++;assert(i<3);if(i==fail_commit)return B2_PATCH_FAIL_PROTECT;
    memcpy(tramp,"\xff\x25\x00\x00\x00\x00",6);memcpy((unsigned char *)tramp+6,destinations+i,8);
    if(lengths[i]>14)memset((unsigned char *)tramp+14,0x90,lengths[i]-14);return B2_PATCH_OK;
}
int hook_is_installed(void *tramp){return tramp && installed;}
int hook_unpatch(void *tramp)
{
    removes++;if(fail_remove)return 0;
    for(size_t i=0;i<3;i++)if(targets[i]==tramp) {
        if(i==2)memcpy(tramp,rp_name_code,lengths[i]);
        else rp_fixture(renderer,rp_images[renderer].routes[i].rva,tramp,lengths[i]);
    }return 1;
}
static void ptr(void *p,size_t offset,const void *value){memcpy((unsigned char *)p+offset,&value,8);}
static void integer(void *p,size_t offset,int32_t value){memcpy((unsigned char *)p+offset,&value,4);}
static unsigned char *image(void)
{
    unsigned char *base=VirtualAlloc(NULL,0x6000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(base);
    const rp_image *p=rp_images+renderer;
    rp_fixture(renderer,p->parser,base+p->parser,p->parser_length);
    rp_fixture(renderer,p->stage,base+p->stage,p->stage_length);
    const rp_flag_image *f=rp_flag_images+renderer;rp_fixture(renderer,f->begin,base+f->begin,f->length);
    for(size_t i=0;i<3;i++)rp_fixture(renderer,f->regions[i].rva,base+f->regions[i].rva,f->regions[i].length);
    for(size_t i=0;i<3;i++) {
        const rp_sampler_region *region=rp_sampler_images[renderer].regions+i;
        rp_fixture(renderer,region->rva,base+region->rva,region->length);
    }
    for(size_t i=0;i<2;i++)rp_fixture(renderer,p->routes[i].rva,base+p->routes[i].rva,p->routes[i].length);
    memcpy(base+rp_name_rvas[renderer],rp_name_code,sizeof rp_name_code);
    if(p->unwind)memcpy(base+p->unwind,"\x01\x04\x01\x00\x04\x42\x00\x00",8);
    ptr(base,p->holder,base+p->object);ptr(base,p->object,base+p->table);ptr(base,p->table+0x40,base+p->routes[0].rva);
    lookup_base=base;return base;
}
static void destroy_image(unsigned char *base)
{
    for(size_t i=0;i<5;i++)assert(rp_site_release(rp_sampler_hooks+i));
    assert(rp_site_release(&rp_flag_hook));
    VirtualFree(base,0,MEM_RELEASE);lookup_base=NULL;
}
static void reset(void)
{
    rp_base=NULL;rp_ready_original=NULL;rp_index_original=NULL;rp_name_original=NULL;rp_observe=NULL;rp_installed=rp_attempted=0;
    prepares=commits=removes=0;fail_prepare=fail_commit=-1;fail_remove=0;clear();
    find_observer=NULL;find_ready=1;fail_find_registration=0;
    lookup_ready=1;
}
static void installation(void)
{
    for(renderer=0;renderer<2;renderer++) {
        unsigned char *base=image();const rp_image *p=rp_images+renderer;
        for(size_t fn=0;fn<5;fn++) {
            uint32_t rva=fn==4?rp_name_rvas[renderer]:fn==3?p->stage:fn?p->routes[fn-1].rva:p->parser;
            size_t size=fn==4?sizeof rp_name_code:fn==3?p->stage_length:fn?p->routes[fn-1].length:p->parser_length;
            for(size_t i=0;i<size;i++) {
                reset();base[rva+i]^=1;assert(!sh_resource_renderprog_reads_install(base,observe) && !prepares);base[rva+i]^=1;
            }
        }
        if(p->unwind)for(size_t i=0;i<8;i++) {
            reset();base[p->unwind+i]^=1;assert(!sh_resource_renderprog_reads_install(base,observe) && !prepares);base[p->unwind+i]^=1;
        }
        for(size_t region=0;region<3;region++) {
            const rp_sampler_region *r=rp_sampler_images[renderer].regions+region;
            for(size_t i=0;i<r->length;i++) {
                reset();base[r->rva+i]^=1;assert(!sh_resource_renderprog_reads_install(base,observe) && !prepares);base[r->rva+i]^=1;
            }
        }
        const rp_flag_image *f=rp_flag_images+renderer;
        for(size_t i=0;i<f->length;i++) {
            reset();base[f->begin+i]^=1;assert(!sh_resource_renderprog_reads_install(base,observe) && !prepares);base[f->begin+i]^=1;
        }
        for(size_t region=0;region<3;region++)for(size_t i=0;i<f->regions[region].length;i++) {
            const rp_sampler_region *r=f->regions+region;
            reset();base[r->rva+i]^=1;assert(!sh_resource_renderprog_reads_install(base,observe) && !prepares);base[r->rva+i]^=1;
        }
        reset();lookup_ready=0;assert(!sh_resource_renderprog_reads_install(base,observe) && !prepares);
        for(int i=0;i<3;i++) {
            reset();fail_prepare=i;assert(!sh_resource_renderprog_reads_install(base,observe) && removes==i);
            reset();fail_commit=i;assert(!sh_resource_renderprog_reads_install(base,observe) && removes==3);
        }
        reset();fail_find_registration=1;assert(!sh_resource_renderprog_reads_install(base,observe) && removes==3 && !commits);
        for(size_t failed_site=0;failed_site<6;failed_site++) {
            reset();sh_patch_test_faults(UINT64_C(1)<<(failed_site*2),0,0);
            assert(!sh_resource_renderprog_reads_install(base,observe) && removes==3);
            sh_patch_test_faults(0,0,0);
            assert(sh_native_pin_fixture_digest(base+p->stage,p->stage_length,p->stage_digest));
            for(size_t i=0;i<5;i++)assert(!rp_sampler_hooks[i].code);
            assert(!rp_flag_hook.code && sh_native_pin_fixture_digest(base+f->begin,f->length,f->digest));
        }
        reset();assert(sh_resource_renderprog_reads_install(base,observe));assert(sh_resource_renderprog_reads_ready());
        assert(relocated[0]==(renderer==0) && lengths[0]==p->routes[0].stolen && lengths[1]==15);
        pinned=0;assert(!sh_resource_renderprog_reads_ready());pinned=1;
        installed=0;assert(!sh_resource_renderprog_reads_ready());installed=1;
        find_ready=0;assert(!sh_resource_renderprog_reads_ready());find_ready=1;
        assert(find_observer==sh_resource_renderprog_reads_find_return);
        base[p->parser]^=1;assert(!sh_resource_renderprog_reads_ready());base[p->parser]^=1;
        base[p->stage+1]^=1;assert(!sh_resource_renderprog_reads_ready());base[p->stage+1]^=1;
        base[f->begin+1]^=1;assert(!sh_resource_renderprog_reads_ready());base[f->begin+1]^=1;
        for(size_t i=0;i<sizeof rp_flag_test;i++) {
            base[f->site+i]^=1;assert(!sh_resource_renderprog_reads_ready());base[f->site+i]^=1;
        }
        for(size_t region=0;region<3;region++) {
            const rp_sampler_region *r=f->regions+region;
            base[r->rva]^=1;assert(!sh_resource_renderprog_reads_ready());base[r->rva]^=1;
        }
        lookup_ready=0;assert(!sh_resource_renderprog_reads_ready());lookup_ready=1;
        for(size_t region=0;region<3;region++) {
            const rp_sampler_region *r=rp_sampler_images[renderer].regions+region;
            base[r->rva]^=1;assert(!sh_resource_renderprog_reads_ready());base[r->rva]^=1;
        }
        for(size_t j=0;j<5;j++) {
            const rp_sampler_site *site=rp_sampler_images[renderer].sites+j;
            for(size_t i=0;i<site->length;i++) {
                base[site->rva+i]^=1;assert(!sh_resource_renderprog_reads_ready());base[site->rva+i]^=1;
            }
        }
        for(size_t fn=0;fn<2;fn++)for(size_t i=0;i<p->routes[fn].length;i++) {
            base[p->routes[fn].rva+i]^=1;assert(!sh_resource_renderprog_reads_ready());base[p->routes[fn].rva+i]^=1;
        }
        for(size_t i=0;i<16;i++) {
            base[rp_name_rvas[renderer]+i]^=1;assert(!sh_resource_renderprog_reads_ready());base[rp_name_rvas[renderer]+i]^=1;
        }
        ptr(base,p->object,NULL);assert(!sh_resource_renderprog_reads_ready());ptr(base,p->object,base+p->table);
        ptr(base,p->table+0x40,NULL);assert(!sh_resource_renderprog_reads_ready());ptr(base,p->table+0x40,base+p->routes[0].rva);
        assert(sh_resource_renderprog_reads_ready());assert(sh_resource_renderprog_reads_install(base,observe) && prepares==3);
        destroy_image(base);reset();
        base=image();fail_commit=1;fail_remove=1;assert(!sh_resource_renderprog_reads_install(base,observe));
        assert(rp_ready_original && rp_index_original && !sh_resource_renderprog_reads_ready());destroy_image(base);reset();
    }
}
static int native_calls,native_throw,native_cleanup;static unsigned char ready_value;static void *index_value,*expected_receiver;
static unsigned char ready_original(void *receiver)
{
    assert(receiver==expected_receiver);native_calls++;
    __try{if(native_throw)RaiseException(0xe0327003,0,0,NULL);}
    __finally{native_cleanup++;}return ready_value;
}
static void *index_original(void *receiver,int index,uintptr_t third,uintptr_t fourth)
{
    assert(receiver==expected_receiver && index==1 && third==0x1122334455667788 && fourth==0x8877665544332211);native_calls++;
    __try{if(native_throw)RaiseException(0xe0327003,0,0,NULL);}
    __finally{native_cleanup++;}return index_value;
}
static const char *name_value;static int expected_name_index,name_mutate;
static const char *name_original(void *receiver,int index)
{
    assert(receiver==expected_receiver && index==expected_name_index);native_calls++;
    __try{if(native_throw)RaiseException(0xe0327003,0,0,NULL);if(name_mutate)(*mutated_field)^=1;}
    __finally{native_cleanup++;}return name_value;
}
static void wrappers(void)
{
    for(renderer=0;renderer<2;renderer++) {
        unsigned char *base=image();const rp_image *p=rp_images+renderer;assert(sh_resource_renderprog_reads_install(base,observe));
        rp_ready_original=ready_original;rp_index_original=index_original;rp_name_original=name_original;
        unsigned char flag[0x60]={0},program[0x1830]={0};void *values[2]={NULL,program};
        ptr(base,p->flag,flag);ptr(base,p->list+0x20,values);integer(base,p->list+0x28,2);
        expected_receiver=base+p->object;uintptr_t ready=(uintptr_t)base+p->ready_return,index=(uintptr_t)base+p->index_return;
        for(int value=0;value<2;value++) {
            clear();ready_value=(unsigned char)value;
            assert(rp_ready_at(expected_receiver,ready)==value && seen_count==1 && seen==flag && !unknown);
        }
        clear();ready_value=0xfe;assert(rp_ready_at(expected_receiver,ready)==0xfe && seen==flag && !unknown);
        clear();observe_error=0x5544;SetLastError(0x1122);assert(rp_ready_at(expected_receiver,ready)==0xfe && GetLastError()==0x1122);
        clear();assert(rp_ready_at(expected_receiver,ready+1)==0xfe && !seen_count && !unknown);
        clear();observing=0;assert(rp_ready_at(expected_receiver,ready)==0xfe && !seen_count && !unknown);observing=1;
        clear();installed=0;assert(rp_ready_at(expected_receiver,ready)==0xfe && unknown);installed=1;
        clear();reject=flag;assert(rp_ready_at(expected_receiver,ready)==0xfe && unknown);
        clear();unknown_token=flag;assert(rp_ready_at(expected_receiver,ready)==0xfe && unknown);
        clear();mutation=1;assert(rp_ready_at(expected_receiver,ready)==0xfe && unknown);
        clear();mutation=2;mutated_field=flag+0x58;assert(rp_ready_at(expected_receiver,ready)==0xfe && unknown);
        clear();mutation=2;mutated_field=base+p->flag;assert(rp_ready_at(expected_receiver,ready)==0xfe && unknown);ptr(base,p->flag,flag);
        clear();throw_observer=1;assert(rp_ready_at(expected_receiver,ready)==0xfe && unknown);
        clear();throw_observer=throw_unknown=1;assert(rp_ready_at(expected_receiver,ready)==0xfe && fallback);
        clear();ptr(base,p->flag,(void *)1);assert(rp_ready_at(expected_receiver,ready)==0xfe && unknown && !seen_count);ptr(base,p->flag,flag);
        clear();native_throw=1;int caught=0,before=native_cleanup;
        __try{rp_ready_at(expected_receiver,ready);}
        __except(GetExceptionCode()==0xe0327003?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
        assert(caught && native_cleanup==before+1 && !seen_count && !unknown);native_throw=0;
        expected_receiver=base+p->list;index_value=program;
        clear();assert(rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index)==program && seen_count==1 && seen==program && !unknown);
        clear();observe_error=0x5544;SetLastError(0x1122);assert(rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index)==program && GetLastError()==0x1122);
        clear();assert(rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index+1)==program && !seen_count && !unknown);
        clear();mutation=1;assert(rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index)==program && unknown);
        clear();mutation=2;mutated_field=program+0x1828;assert(rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index)==program && unknown);
        clear();mutation=2;mutated_field=(unsigned char *)values+8;assert(rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index)==program && unknown);values[1]=program;
        clear();integer(base,p->list+0x28,1);assert(rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index)==program && unknown && !seen_count);integer(base,p->list+0x28,2);
        clear();index_value=NULL;assert(!rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index) && unknown);index_value=program;
        clear();throw_observer=1;assert(rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index)==program && unknown);
        clear();native_throw=1;caught=0;before=native_cleanup;
        __try{rp_index_at(expected_receiver,1,0x1122334455667788,0x8877665544332211,index);}
        __except(GetExceptionCode()==0xe0327003?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
        assert(caught && native_cleanup==before+1 && !seen_count && !unknown);native_throw=0;
        unsigned char parent[0x80]={0},child[0x80]={0};uintptr_t find=(uintptr_t)base+p->find_return;
        for(int kind=0;kind<19;kind++)if(kind!=13) {
            clear();integer(parent,0x6c,kind);ptr(parent,0x58,(void *)1);
            find_observer(parent,find);assert(seen_count==1 && seen==parent && !unknown);
        }
        clear();integer(parent,0x6c,13);ptr(parent,0x58,NULL);
        find_observer(parent,find);assert(seen_count==1 && seen==parent && !unknown);
        for(int bit=0;bit<2;bit++) {
            clear();parent[0x64]=(unsigned char)(bit?0x10:0);ptr(parent,0x58,child);
            find_observer(parent,find);assert(seen_count==2 && seen==child && !unknown);
        }
        clear();find_observer(parent,find+1);assert(!seen_count && !unknown);
        clear();find_observer(NULL,find);assert(!seen_count && unknown);
        clear();ptr(parent,0x58,(void *)1);find_observer(parent,find);assert(!seen_count && unknown);ptr(parent,0x58,child);
        clear();unknown_token=child;find_observer(parent,find);assert(seen_count==2 && unknown);
        clear();reject=child;find_observer(parent,find);assert(seen_count==2 && unknown);
        clear();mutation=2;mutated_field=child+0x68;find_observer(parent,find);assert(seen_count==2 && unknown);
        clear();mutation=2;mutated_field=parent+0x58;find_observer(parent,find);assert(unknown);ptr(parent,0x58,child);
        clear();throw_observer=1;find_observer(parent,find);assert(unknown);
        clear();observing=0;find_observer(parent,find);assert(!seen_count && !unknown);observing=1;
        clear();find_ready=0;find_observer(parent,find);assert(!seen_count && unknown);find_ready=1;
        clear();SetLastError(0x9988);rp_sampler_reached(child);
        assert(seen_count==1 && seen==child && !unknown && GetLastError()==0x9988);
        clear();mutation=2;mutated_field=child+0x68;rp_sampler_reached(child);assert(seen_count==1 && unknown);
        clear();unknown_token=child;rp_sampler_reached(child);assert(seen_count==1 && unknown);
        clear();reject=child;rp_sampler_reached(child);assert(unknown);
        clear();throw_observer=1;rp_sampler_reached(child);assert(unknown);
        clear();rp_sampler_reached((void *)1);assert(!seen_count && unknown);
        clear();observing=0;rp_sampler_reached(child);assert(!seen_count && !unknown);observing=1;
        clear();rp_name_reached(child);assert(seen_count==1 && seen==child && !unknown);
        clear();mutation=2;mutated_field=child+8;rp_name_reached(child);assert(unknown);
        clear();changed_token=1;reject=child;rp_name_reached(child);assert(unknown);
        clear();rp_name_reached((void *)1);assert(!seen_count && unknown);
        /* A changed later static resource must be observed at this reached
         * load, even if an earlier Ready witness observed another object. */
        ptr(base,p->flag,child);clear();observe_error=0x7788;SetLastError(0x1122);rp_flag_reached(child);
        assert(seen_count==1 && seen==child && !unknown && GetLastError()==0x1122);
        clear();rp_flag_reached(parent);assert(!seen_count && unknown);
        clear();mutation=2;mutated_field=child+0x58;rp_flag_reached(child);assert(seen==child && unknown);
        clear();mutation=2;mutated_field=base+p->flag;rp_flag_reached(child);assert(unknown);ptr(base,p->flag,child);
        clear();unknown_token=child;rp_flag_reached(child);assert(seen==child && unknown);
        clear();mutation=1;rp_flag_reached(child);assert(seen==child && unknown);
        clear();reject=child;rp_flag_reached(child);assert(unknown);
        clear();throw_observer=1;rp_flag_reached(child);assert(unknown);
        clear();rp_flag_reached(NULL);assert(!seen_count && unknown);
        clear();ptr(base,p->flag,(void *)1);rp_flag_reached((void *)1);assert(!seen_count && unknown);ptr(base,p->flag,child);
        clear();observing=0;rp_flag_reached(child);assert(!seen_count && !unknown);observing=1;
        void **manager=calloc(0x93f,8);assert(manager);manager[1]=child;integer(child,0x68,1);
        name_value="native_name";ptr(child,8,name_value);ptr(base,rp_name_managers[renderer],manager);
        expected_receiver=manager;expected_name_index=1;
        clear();observe_error=0x7788;SetLastError(0x1122);
        assert(rp_name_hook(manager,1)==name_value && seen_count==1 && seen==child && !unknown && GetLastError()==0x1122);
        clear();name_value=NULL;ptr(child,8,NULL);assert(!rp_name_hook(manager,1) && seen==child && !unknown);
        clear();name_value="native_name";assert(rp_name_hook(manager,1)==name_value && unknown);ptr(child,8,name_value);
        clear();mutation=2;mutated_field=child+8;assert(rp_name_hook(manager,1)==name_value && unknown);ptr(child,8,name_value);
        clear();mutation=2;mutated_field=(unsigned char *)manager+8;assert(rp_name_hook(manager,1)==name_value && unknown);manager[1]=child;
        clear();name_mutate=1;mutated_field=child+8;assert(rp_name_hook(manager,1)==name_value && unknown);name_mutate=0;ptr(child,8,name_value);
        clear();integer(child,0x68,2);assert(rp_name_hook(manager,1)==name_value && unknown);integer(child,0x68,1);
        clear();expected_name_index=-1;assert(rp_name_hook(manager,-1)==name_value && unknown);expected_name_index=1;
        clear();unknown_token=child;assert(rp_name_hook(manager,1)==name_value && unknown);
        clear();throw_observer=1;assert(rp_name_hook(manager,1)==name_value && unknown);
        clear();native_throw=1;caught=0;before=native_cleanup;
        __try{rp_name_hook(manager,1);}
        __except(GetExceptionCode()==0xe0327003?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
        assert(caught && native_cleanup==before+1 && seen==child && unknown);native_throw=0;
        clear();observing=0;assert(rp_name_hook(manager,1)==name_value && !seen_count && !unknown);observing=1;
        free(manager);
        destroy_image(base);reset();
    }
}
static const unsigned char *raw_at(const unsigned char *raw,size_t length,uint32_t rva,size_t size)
{
    const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)raw;assert(dos->e_magic==IMAGE_DOS_SIGNATURE);
    const IMAGE_NT_HEADERS64 *nt=(const IMAGE_NT_HEADERS64 *)(raw+dos->e_lfanew);assert(nt->Signature==IMAGE_NT_SIGNATURE);
    const IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
        const IMAGE_SECTION_HEADER *s=sections+j;
        if(rva>=s->VirtualAddress && rva-s->VirtualAddress<=s->SizeOfRawData && size<=s->SizeOfRawData-(rva-s->VirtualAddress)) {
            size_t offset=s->PointerToRawData+rva-s->VirtualAddress;assert(offset<=length && size<=length-offset);return raw+offset;
        }
    }assert(0);return NULL;
}
static void image_case(const char *path,int r)
{
    FILE *file=fopen(path,"rb");assert(file);fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);
    unsigned char *raw=malloc(length);assert(raw && fread(raw,1,length,file)==(size_t)length);fclose(file);const rp_image *p=rp_images+r;
    assert(sh_native_pin_fixture_digest(raw_at(raw,length,p->parser,p->parser_length),p->parser_length,p->parser_digest));
    assert(sh_native_pin_fixture_digest(raw_at(raw,length,p->stage,p->stage_length),p->stage_length,p->stage_digest));
    const rp_flag_image *f=rp_flag_images+r;
    assert(sh_native_pin_fixture_digest(raw_at(raw,length,f->begin,f->length),f->length,f->digest));
    assert(!memcmp(raw_at(raw,length,f->site,sizeof rp_flag_test),rp_flag_test,sizeof rp_flag_test));
    for(size_t i=0;i<3;i++)assert(sh_native_pin_fixture_digest(raw_at(raw,length,f->regions[i].rva,f->regions[i].length),f->regions[i].length,f->regions[i].digest));
    for(size_t i=0;i<3;i++) {
        const rp_sampler_region *region=rp_sampler_images[r].regions+i;
        assert(sh_native_pin_fixture_digest(raw_at(raw,length,region->rva,region->length),region->length,region->digest));
    }
    for(size_t i=0;i<2;i++)assert(sh_native_pin_fixture_digest(raw_at(raw,length,p->routes[i].rva,p->routes[i].length),p->routes[i].length,p->routes[i].digest));
    assert(!memcmp(raw_at(raw,length,rp_name_rvas[r],sizeof rp_name_code),rp_name_code,sizeof rp_name_code));
    if(p->unwind)assert(!memcmp(raw_at(raw,length,p->unwind,8),"\x01\x04\x01\x00\x04\x42\x00\x00",8));
    const IMAGE_DOS_HEADER *dos=(const IMAGE_DOS_HEADER *)raw;const IMAGE_NT_HEADERS64 *nt=(const IMAGE_NT_HEADERS64 *)(raw+dos->e_lfanew);
    uint64_t holder=0,slot=0;memcpy(&holder,raw_at(raw,length,p->holder,8),8);memcpy(&slot,raw_at(raw,length,p->table+0x40,8),8);
    assert(holder==nt->OptionalHeader.ImageBase+p->object && slot==nt->OptionalHeader.ImageBase+p->routes[0].rva);
    free(raw);puts("resource_renderprog_reads_image_test: passed");
}
int main(int argc,char **argv)
{
    if(argc==3){image_case(argv[1],atoi(argv[2]));return 0;}
    installation();wrappers();puts("resource_renderprog_reads_test: passed");return 0;
}
