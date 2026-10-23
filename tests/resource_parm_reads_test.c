#define SH_RESOURCE_PARM_READS_TESTING
#include "../src/backend/resource_parm_reads.c"
#include <stdio.h>
#include <stdlib.h>

static int failures, renderer, pinned = 1, recording = 1, prepared, committed, calls, refs, unknown, mutate;
static int default_calls, default_mutate;
static void *observed, *fixture_manager;
static unsigned char *fixture_base;
static int installed=1,graph_failed,fallback_unknown,throw_observer,throw_native,token_mutate;
static uint64_t token=7;
/* The actual expression module is independently exercised by its semantic
 * and paired-image rows. This fixture isolates the older evaluator/default
 * hooks while preserving the composed install/readiness dependency. */
static int expression_ready=1;
int sh_resource_parm_expression_reads_install(const uint8_t *base,int (*observe)(void *))
{return base && observe && expression_ready;}
int sh_resource_parm_expression_reads_ready(void){return expression_ready;}
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)
int sh_host_is_vulkan(void) { return renderer; }
int sh_host_is_pinned_renderer_build(int which) { return pinned && which == renderer; }
int sh_resource_graph_recording(void) { return recording; }
int sh_resource_graph_native_observing(void) { return recording; }
void sh_resource_graph_incomplete(void) { graph_failed++; }
int sh_resource_graph_native_observe_resource(void *resource) { CHECK(!resource);fallback_unknown++;return 0; }
uint64_t sh_resource_instances_token(const void *resource) { return resource ? token:0; }
static int observe(void *resource) {
    if (resource) { refs++; observed = resource;if(throw_observer)RaiseException(0xe0134011,0,0,NULL); }
    else unknown++;return !!resource;
}
static void native_eval(const void *op,const void *v,const void *l,const void *r,void *out)
{
    (void)op; (void)v; (void)l; (void)r;
    calls++; *(int *)out = 41;
    if(token_mutate)token++;
    if(throw_native)RaiseException(0xe0134012,0,0,NULL);
    if (mutate) *(void **)((char *)fixture_manager + 0xc180 + 8) = NULL;
}
void *hook_prepare_rip_lea(void *target,void *detour,size_t stolen,size_t offset)
{
    CHECK(target == fixture_base + g_eval_rva[renderer]);
    CHECK(detour == rp_eval && stolen == 19 && offset == 12);
    prepared++; return native_eval;
}
static void *native_default(void *manager, int index)
{
    default_calls++;
    void *result = index >= 0 && index < 0x93f ? ((void **)manager)[index] : NULL;
    if (default_mutate) ((void **)manager)[index] = NULL;
    if(token_mutate)token++;
    return result;
}
void *hook_prepare(void *target,void *detour,size_t stolen)
{
    CHECK(target == fixture_base + g_default_rva[renderer]);
    CHECK(detour == rp_default && stolen == 14);
    prepared++; return native_default;
}
sh_patch_status hook_commit(void *trampoline) {
    CHECK(trampoline == native_eval || trampoline == native_default);committed++;
    unsigned char *entry=fixture_base+(trampoline==native_eval ? g_eval_rva[renderer]:g_default_rva[renderer]);
    void *detour=trampoline==native_eval ? (void *)rp_eval:(void *)rp_default;
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    memcpy(entry,jump,6);memcpy(entry+6,&detour,8);return B2_PATCH_OK;
}
int hook_is_installed(void *trampoline) { return installed && (trampoline==native_eval || trampoline==native_default); }
int hook_unpatch(void *trampoline) { CHECK(trampoline);return 1; }

int main(void)
{
    unsigned char decl[0x70] = {0}, data[16] = {0}, wrong[16] = {0};
    uint16_t op[4] = {0x14 | (15 << 10), 0, 1, 0};
    fixture_manager = calloc(1, 0x10188);
    CHECK(fixture_manager != NULL);
    if (!fixture_manager) return 1;
    *(int *)((char *)fixture_manager+0x10180)=2;
    *(void **)((char *)fixture_manager+0x8180+8)=decl;
    *(void **)((char *)fixture_manager+0xc180+8)=data;
    *(void **)(decl+0x60)=data;
    fixture_base=VirtualAlloc(NULL,0x5600000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(fixture_base != NULL);
    if (!fixture_base) return 1;
    for (renderer=0;renderer<2;renderer++) {
        sh_resource_parm_reads_test_reset();
        memcpy(fixture_base+g_eval_rva[renderer],g_head[renderer],19);
        memcpy(fixture_base+g_default_rva[renderer],g_default_head,sizeof(g_default_head));
        *(void **)(fixture_base+g_manager_rva[renderer])=fixture_manager;
        CHECK(sh_resource_parm_reads_install(fixture_base,observe));
        CHECK(sh_resource_parm_reads_install(fixture_base,observe));
        CHECK(sh_resource_parm_reads_ready());
        refs=unknown=calls=0; int result=0;
        rp_eval(op,NULL,NULL,NULL,&result);
        CHECK(calls==1 && result==41 && refs==1 && observed==decl && !unknown);
        op[0]=1; rp_eval(op,NULL,NULL,NULL,&result);
        CHECK(calls==2 && refs==1 && !unknown); op[0]=0x14;
        op[2]=0xffff; sh_resource_parm_reads_test_observe(op,fixture_manager); CHECK(unknown==1);
        op[2]=2; sh_resource_parm_reads_test_observe(op,fixture_manager); CHECK(unknown==2);
        op[2]=1; *(void **)(decl+0x60)=wrong;
        sh_resource_parm_reads_test_observe(op,fixture_manager); CHECK(unknown==3);
        *(void **)(decl+0x60)=data;
        sh_resource_parm_reads_test_observe(op,NULL); CHECK(unknown==4);
        *(int *)((char *)fixture_manager+0x10180)=0x801;
        sh_resource_parm_reads_test_observe(op,fixture_manager); CHECK(unknown==5);
        *(int *)((char *)fixture_manager+0x10180)=2;
        recording=0; rp_eval(op,NULL,NULL,NULL,&result); CHECK(refs==1 && unknown==5 && calls==3);
        recording=1; mutate=1; rp_eval(op,NULL,NULL,NULL,&result);
        CHECK(refs==2 && unknown==6 && calls==4);
        mutate=0; *(void **)((char *)fixture_manager+0xc180+8)=data;
        refs=unknown=default_calls=0;
        ((void **)fixture_manager)[7]=decl; *(int *)(decl+0x68)=7;
        CHECK(rp_default(fixture_manager,7)==decl && refs==1 && !unknown && default_calls==1);
        *(int *)(decl+0x68)=8;
        CHECK(rp_default(fixture_manager,7)==decl && refs==1 && unknown==1);
        *(int *)(decl+0x68)=7;
        CHECK(!rp_default(fixture_manager,8) && refs==1 && unknown==2);
        CHECK(!rp_default(fixture_manager,0x93f) && unknown==3);
        CHECK(!rp_default(fixture_manager,-1) && unknown==4);
        recording=0; CHECK(rp_default(fixture_manager,7)==decl && refs==1 && unknown==4); recording=1;
        default_mutate=1;
        CHECK(rp_default(fixture_manager,7)==decl && refs==1 && unknown==5);
        default_mutate=0; ((void **)fixture_manager)[7]=decl;
        *(void **)(fixture_base+g_manager_rva[renderer])=NULL;
        CHECK(rp_default(fixture_manager,7)==decl && refs==1 && unknown==6);
        *(void **)(fixture_base+g_manager_rva[renderer])=fixture_manager;
        int old_unknown=unknown,old_calls=calls;
        installed=0;CHECK(!sh_resource_parm_reads_ready());rp_eval(op,NULL,NULL,NULL,&result);
        CHECK(calls==old_calls+1 && unknown==old_unknown+1);installed=1;
        fixture_base[g_eval_rva[renderer]+6]^=1;CHECK(!sh_resource_parm_reads_ready());fixture_base[g_eval_rva[renderer]+6]^=1;
        CHECK(sh_resource_parm_reads_ready());
        old_unknown=unknown;token_mutate=1;rp_eval(op,NULL,NULL,NULL,&result);CHECK(unknown==old_unknown+1);token_mutate=0;
        old_unknown=unknown;token_mutate=1;CHECK(rp_default(fixture_manager,7)==decl);CHECK(unknown==old_unknown+1);token_mutate=0;
        old_calls=calls;old_unknown=unknown;throw_observer=1;rp_eval(op,NULL,NULL,NULL,&result);
        CHECK(calls==old_calls+1 && unknown>old_unknown && graph_failed && fallback_unknown);throw_observer=0;
        int caught=0;old_unknown=unknown;throw_native=1;
        __try { rp_eval(op,NULL,NULL,NULL,&result); }
        __except(EXCEPTION_EXECUTE_HANDLER) { caught=GetExceptionCode()==0xe0134012; }
        CHECK(caught && unknown==old_unknown+1);throw_native=0;
        memcpy(fixture_base+g_eval_rva[renderer],g_head[renderer],19);
        memcpy(fixture_base+g_default_rva[renderer],g_default_head,sizeof(g_default_head));
        sh_resource_parm_reads_test_reset(); fixture_base[g_default_rva[renderer]+15]^=1;
        CHECK(!sh_resource_parm_reads_install(fixture_base,observe));
        CHECK(!sh_resource_parm_reads_ready()); fixture_base[g_default_rva[renderer]+15]^=1;
        sh_resource_parm_reads_test_reset();
        fixture_base[g_eval_rva[renderer]]^=1;
        CHECK(!sh_resource_parm_reads_install(fixture_base,observe));
        CHECK(!sh_resource_parm_reads_ready());
        fixture_base[g_eval_rva[renderer]]^=1;
        sh_resource_parm_reads_test_reset(); pinned=0;
        CHECK(!sh_resource_parm_reads_install(fixture_base,observe)); pinned=1;
    }
    CHECK(prepared==4 && committed==4);
    VirtualFree(fixture_base,0,MEM_RELEASE); free(fixture_manager);
    printf("resource_parm_reads: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
