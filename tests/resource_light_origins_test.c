#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define SH_RESOURCE_LIGHT_ORIGINS_TESTING
#include "../src/backend/resource_light_origins.c"
static int failures,leases[16],captures,allowed=1,renderer=1,pinned=1,prepares,commits,removes,install_fault;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); failures++; } } while(0)
int sh_host_is_vulkan(void) { return renderer; }
int sh_host_is_pinned_renderer_build(int r) { return pinned && r==renderer; }
int sh_package_runtime_resource_observation_allowed(void) { return allowed; }
static int source_faults;
void sh_package_runtime_source_cleanup_failed(void) { source_faults++; }
void *hook_prepare(void *target,void *detour,size_t n) { (void)detour; CHECK(n>=14); prepares++; return prepares==install_fault?NULL:target; }
void *hook_prepare_rip_cmp32_imm8(void *target,void *detour,size_t n,size_t offset)
{ CHECK(n==16 && offset==6); return hook_prepare(target,detour,n); }
sh_patch_status hook_commit(void *p) { CHECK(p); commits++; return B2_PATCH_OK; }
int hook_unpatch(void *p) { CHECK(p); removes++; return 1; }
static int promote(const void *capture,void **out)
{ int generation=*(const int *)capture; int *lease=malloc(sizeof(*lease)); if(!lease) return 0; *lease=generation; leases[generation]++; *out=lease; return 1; }
static void capture_release(void *capture) { captures--; free(capture); }
static void lease_release(void *lease) { CHECK(leases[*(int *)lease]>0); leases[*(int *)lease]--; free(lease); }
static sh_resource_instance_origin *origin(int generation)
{ int *capture=malloc(sizeof(*capture)); CHECK(capture); *capture=generation; captures++; return sh_resource_instances_origin(capture,0); }
static unsigned char light[0xa70],committed[0xbc0],material[4][16],model[0x1000],decl[0x80],stages[0x6e0],runtime_stages[0xd0],children[0x38];
static void *fallback[3],*selected; static int native_mode,native_calls;
static int suppress_commit;
static int scalar_calls,scalar_throw,scalar_replace;
static uintptr_t scalar_key=0x1234; static uint32_t scalar_bits=0x3fa00000;
static void *scalar_expected;
static void put(void *object,size_t offset,void *value) { *(void **)((unsigned char *)object+offset)=value; }
static void *native_ctor(void *object) { put(object,0xa68,committed); return object; }
static void native_destroy(void *object)
{ CHECK(object==light); native_calls++; if(native_mode==2) RaiseException(0xe0311100,0,0,NULL); }
static void native_commit(void *object)
{
    CHECK(object==light); native_calls++;
    if(native_mode==1) return;
    CHECK(leases[1]+leases[5]+leases[6]+leases[7]>0 && leases[2]+leases[8]>0 && leases[3]>0 && leases[4]>0);
    put(committed,0xa8,*(void **)(light+0xa0)); put(committed,0xba0,selected);
    committed[0xbac]=*(unsigned char *)selected;
    if(native_mode==2) RaiseException(0xe0311100,0,0,NULL);
    if(native_mode==3) {
        sh_resource_instances_construct(light,1); sh_resource_instances_construct(committed,1);
    }
}
static void native_scalar(void *object,uintptr_t name,float value)
{
    uint32_t bits; memcpy(&bits,&value,sizeof(bits));
    CHECK(object==(scalar_expected?scalar_expected:light) && name==scalar_key && bits==scalar_bits); scalar_calls++;
    if(scalar_throw) RaiseException(0xe0311101,0,0,NULL);
    if(scalar_replace) { sh_resource_instances_destroyed(object,sh_resource_instances_token(object),1); sh_resource_instances_construct(object,1); }
}
static void native_emit(void *object,uintptr_t a,uintptr_t b,uintptr_t c)
{
    CHECK(object==model && a==1 && b==2 && c==3); CHECK(leases[1]>0);
    put(light,0xa0,*(void **)(stages+0x68));
    rl_scalar(light,0x1234,1.25f);
    if(native_mode==2) RaiseException(0xe0311100,0,0,NULL);
}
static void setup(void)
{
    CHECK(sh_resource_instances_bind((sh_resource_instance_api){promote,capture_release,lease_release}));
    for(int i=0;i<4;i++) { sh_resource_instance_origin *o=origin(i+1); CHECK(o && sh_resource_instances_publish(material[i],o)); sh_resource_instances_origin_release(o); }
    for(int i=0;i<3;i++) { fallback[i]=material[i+1]; g_fallback[i]=fallback+i; }
    g_ready=1; g_ctor=native_ctor; g_destroy=native_destroy; g_commit=native_commit; g_emit=native_emit; g_scalar=native_scalar;
    g_suppress_commit=&suppress_commit;
    CHECK(rl_ctor(light)==light); put(light,0xa0,material[0]); selected=material[1];
    sh_resource_instances_construct(model,1); put(model,0xeb0,decl); put(model,0xec8,runtime_stages);
    *(int *)(model+0xed8)=1; put(decl,0x68,stages); *(int *)(decl+0x70)=1;
    stages[0x58]=1; *(int16_t *)(stages+0x70)=1; put(stages,0x68,material[0]);
    put(runtime_stages,0x88,children); children[0x2c]=1; put(children,0x30,light);
}
static void lifetime(void)
{
    rl_emit(model,1,2,3); CHECK(leases[1]==1 && !leases[2]);
    rl_commit(light); CHECK(leases[1]==1 && leases[2]==1 && !leases[3] && !leases[4]);
    /* Disabling/unlinking a light is not destruction. Child and committed
     * copies keep independent exact roots through a later field overwrite. */
    CHECK(rl_assign(light,sh_resource_instances_token(light),0xa0,NULL,NULL)); put(light,0xa0,NULL);
    CHECK(leases[1]==1 && leases[2]==1);
    put(light,0xa0,material[0]);
    sh_resource_instance_origin *replacement=origin(5); CHECK(sh_resource_instances_publish(material[0],replacement));
    sh_resource_instances_origin_release(replacement);
    native_mode=2; int caught=0;
    __try { rl_commit(light); } __except(EXCEPTION_EXECUTE_HANDLER) { caught=1; }
    CHECK(caught && leases[1]==1 && leases[5]==1); /* partial copy protects both */
    native_mode=0; rl_commit(light); CHECK(!leases[1] && leases[5]==1 && leases[2]==1 && !leases[3] && !leases[4]);
    native_mode=1; suppress_commit=1; selected=material[2]; rl_commit(light);
    CHECK(leases[2]==1); /* skipped native copy cannot retire previous selected */
    native_mode=0; suppress_commit=0; rl_commit(light); CHECK(!leases[2] && leases[3]==1);
    native_mode=2; caught=0;
    __try { rl_destroy(light); } __except(EXCEPTION_EXECUTE_HANDLER) { caught=1; }
    CHECK(caught && leases[5]==1 && leases[3]==1);
    native_mode=0; rl_destroy(light); CHECK(!leases[5] && !leases[3]);
}
static void *main_fiber;
static void WINAPI abandon(void *unused)
{ (void)unused; CHECK(rl_commit_begin(light).serial); SwitchToFiber(main_fiber); }
static void fiber_and_reuse(void)
{
    CHECK(rl_ctor(light)==light); put(light,0xa0,material[0]); selected=material[1];
    main_fiber=ConvertThreadToFiber(NULL); CHECK(main_fiber);
    void *fiber=CreateFiber(0,abandon,NULL); CHECK(fiber); SwitchToFiber(fiber); DeleteFiber(fiber);
    sh_execution_context(NULL,0); CHECK(!g_receipts); CHECK(leases[5] && leases[2] && leases[3] && leases[4]);
    rl_commit(light); CHECK(leases[5]==1 && leases[2]==1 && !leases[3] && !leases[4]);
    CHECK(ConvertFiberToThread()); main_fiber=NULL;
    /* A reentrant constructor creates another storage lifetime. The old
     * receipt cannot adopt its fields or retire its independent ownership. */
    native_mode=3; rl_commit(light); CHECK(sh_resource_instances_complete(light,sh_resource_instances_token(light)));
    native_mode=0; rl_destroy(light); CHECK(!leases[5] && !leases[2]);
}
static void borrowed_pool(void)
{
    unsigned char world[0x9130]={0},pool[0x18]={0}; void *slots[2]={NULL,light};
    CHECK(rl_ctor(light)==light); put(children,0x30,NULL); put(model,0xc0,world);
    put(world,0x9128,pool); put(pool,8,slots); *(int *)(pool+0x10)=2;
    *(int *)(model+0xea4)=1; *(int *)(model+0xea8)=2;
    rl_scope scope=rl_emit_begin(model); CHECK(scope.serial && leases[5]==1);
    put(light,0xa0,material[0]); rl_emit_end(scope); CHECK(leases[5]==1);
    sh_resource_instance_origin *actual=sh_resource_instances_field_origin(light,sh_resource_instances_token(light),(uintptr_t)light+0xa0);
    CHECK(actual); sh_resource_instances_origin_release(actual);
    rl_destroy(light); CHECK(!leases[5]);
    CHECK(rl_ctor(light)==light); scope=rl_emit_begin(model); CHECK(scope.serial);
    *(int *)(pool+0x10)=1; rl_emit_end(scope); /* changed native pool bounds cannot certify a write */
    CHECK(!sh_resource_instances_field_origin(light,sh_resource_instances_token(light),(uintptr_t)light+0xa0));
    CHECK(leases[5]==1); /* potential read protected on the model until verified reset/destruction */
    sh_resource_instances_unprotect(model,sh_resource_instances_token(model)); CHECK(!leases[5]);
    rl_destroy(light); put(model,0xc0,NULL); put(children,0x30,light);
}
static void install_controls(void)
{
    g_ready=0; g_ctor=NULL; g_destroy=NULL; g_commit=NULL; g_emit=NULL; g_scalar=NULL;
    unsigned char *base=VirtualAlloc(NULL,0x1800000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(base);
    for(renderer=0;renderer<2;renderer++) {
        memcpy(base+g_rvas[renderer][0],g_ctor_head,sizeof(g_ctor_head));
        memcpy(base+g_rvas[renderer][1],g_destroy_head,sizeof(g_destroy_head));
        memcpy(base+g_rvas[renderer][2],g_commit_head[renderer],sizeof(g_commit_head[renderer]));
        memcpy(base+g_rvas[renderer][3],g_emit_head,sizeof(g_emit_head));
        memcpy(base+g_rvas[renderer][4],g_scalar_head,sizeof(g_scalar_head));
        prepares=commits=removes=0; pinned=0; CHECK(!sh_resource_light_origins_install(base) && !prepares); pinned=1;
        base[g_rvas[renderer][2]]^=1; CHECK(!sh_resource_light_origins_install(base) && !prepares); base[g_rvas[renderer][2]]^=1;
        base[g_rvas[renderer][4]]^=1; CHECK(!sh_resource_light_origins_install(base) && !prepares); base[g_rvas[renderer][4]]^=1;
        install_fault=3; CHECK(!sh_resource_light_origins_install(base) && removes==4); install_fault=0;
        prepares=commits=removes=0; CHECK(sh_resource_light_origins_install(base) && prepares==5 && commits==5);
        CHECK(sh_resource_light_origins_ready()); CHECK(sh_resource_light_origins_uninstall() && removes==5);
    }
    VirtualFree(base,0,MEM_RELEASE);
}
static int changed(void *context,void *resource) { return resource==context; }
static void refresh_controls(void)
{
    CHECK(rl_ctor(light)==light); put(light,0xa0,material[0]); selected=material[1]; rl_commit(light);
    sh_resource_instance_origin *next=origin(6); CHECK(sh_resource_instances_publish(material[0],next)); sh_resource_instances_origin_release(next);
    int before=native_calls; char error[128]={0};
    CHECK(sh_resource_light_origins_refresh(changed,material[0],error,sizeof(error)));
    CHECK(native_calls==before+1 && !leases[5] && leases[6]==1); /* child and committed deduplicated */
    before=native_calls; CHECK(sh_resource_light_origins_refresh(changed,material[3],error,sizeof(error)));
    CHECK(native_calls==before); /* no affected live light */
    next=origin(8); CHECK(sh_resource_instances_publish(material[1],next)); sh_resource_instances_origin_release(next);
    material[1][0]=9;
    CHECK(sh_resource_light_origins_refresh(changed,material[1],error,sizeof(error)));
    CHECK(!leases[2] && leases[8]==1 && committed[0xbac]==9);
    next=origin(7); CHECK(sh_resource_instances_publish(material[0],next)); sh_resource_instances_origin_release(next);
    native_mode=2; CHECK(!sh_resource_light_origins_refresh(changed,material[0],error,sizeof(error)));
    CHECK(leases[6]==1 && leases[7]==1);
    native_mode=0; CHECK(sh_resource_light_origins_refresh(changed,material[0],error,sizeof(error)));
    CHECK(!leases[6] && leases[7]==1);
    native_mode=3; CHECK(!sh_resource_light_origins_refresh(changed,material[0],error,sizeof(error)));
    CHECK(!leases[7] && !leases[8]); native_mode=0; rl_destroy(light);
}
static void lost_commit_controls(void)
{
    uint64_t token=sh_resource_instances_token(light),other=sh_resource_instances_token(committed);
    g_allocation_fault=1; rl_commit(light);
    CHECK(sh_resource_instances_faulted(light,token) && sh_resource_instances_faulted(committed,other));
    for(int i=1;i<=4;i++) CHECK(leases[i]==1);
    CHECK(rl_assign(light,token,0xa0,NULL,NULL)); put(light,0xa0,NULL);
    CHECK(sh_resource_instances_faulted(light,token) && leases[1]==1); /* partial edit is not a complete copy */
    put(light,0xa0,material[0]); char error[128];
    CHECK(sh_resource_light_origins_reconcile(committed,other,error,sizeof(error))==1);
    CHECK(!sh_resource_instances_faulted(light,token) && !sh_resource_instances_faulted(committed,other));
    CHECK(leases[1]==1 && leases[2]==1 && !leases[3] && !leases[4]);
    CHECK(sh_resource_light_origins_reconcile(model,sh_resource_instances_token(model),error,sizeof(error))==-1);
    rl_destroy(light); CHECK(!leases[1] && !leases[2] && !source_faults);
    CHECK(rl_ctor(light)==light); put(light,0xa0,material[0]);
}
static void scalar_controls(void)
{
    scalar_key=UINT64_C(0x9876543210); scalar_bits=0x80000000; float special; memcpy(&special,&scalar_bits,4);
    rl_scalar(light,scalar_key,special); CHECK(leases[1]==1);
    put(light,0xa0,material[1]); scalar_throw=1; int caught=0;
    __try { rl_scalar(light,scalar_key,special); } __except(EXCEPTION_EXECUTE_HANDLER) { caught=1; }
    CHECK(caught && !leases[1] && leases[2]==1); scalar_throw=0;
    uint64_t token=sh_resource_instances_token(light); scalar_replace=1;
    rl_scalar(light,scalar_key,special); CHECK(sh_resource_instances_token(light)!=token && !leases[2]); scalar_replace=0;
    unsigned char unknown[0xa70]={0}; scalar_expected=unknown;
    rl_scalar(unknown,scalar_key,special); CHECK(!leases[1] && !leases[2]); scalar_expected=NULL;
    CHECK(rl_ctor(light)==light); put(light,0xa0,material[0]); scalar_key=0x1234; scalar_bits=0x3fa00000;
}
static void lost_emit_controls(void)
{
    uint64_t token=sh_resource_instances_token(model),child=sh_resource_instances_token(light),copy=sh_resource_instances_token(committed);
    g_allocation_fault=1; rl_emit(model,1,2,3);
    CHECK(sh_resource_instances_faulted(model,token) && sh_resource_instances_faulted(light,child) && sh_resource_instances_faulted(committed,copy));
    CHECK(leases[1]==1);
    sh_resource_instances_destroyed(model,token,1); CHECK(leases[1]==1); /* escaped children own the group independently */
    sh_resource_instances_construct(model,1);
    rl_commit(light); CHECK(!sh_resource_instances_faulted(light,child) && !sh_resource_instances_faulted(committed,copy));
    CHECK(leases[1]==1 && leases[2]==1); rl_destroy(light); CHECK(!leases[1] && !leases[2]);
    CHECK(rl_ctor(light)==light); put(light,0xa0,material[0]);
}
static VOID WINAPI emergency_emit_fiber(void *unused)
{
    (void)unused; g_allocation_fault=1; rl_scope scope=rl_emit_begin(model); CHECK(scope.emergency);
    SwitchToFiber(main_fiber);
}
static void emergency_fiber_controls(void)
{
    int converted=!IsThreadAFiber(); main_fiber=converted?ConvertThreadToFiber(NULL):GetCurrentFiber(); CHECK(main_fiber);
    void *fiber=CreateFiber(0,emergency_emit_fiber,NULL); CHECK(fiber);
    if(fiber) {
        SwitchToFiber(fiber); CHECK(leases[1]==1); DeleteFiber(fiber); sh_execution_context(NULL,0);
        sh_resource_instance_origin *o=sh_resource_instances_lookup(material[0]); CHECK(o && !sh_resource_instances_fault_group_active(o));
        sh_resource_instances_origin_release(o);
        sh_resource_instances_destroyed(model,sh_resource_instances_token(model),1); CHECK(leases[1]==1);
        rl_destroy(light); CHECK(!leases[1]); sh_resource_instances_construct(model,1);
        CHECK(rl_ctor(light)==light); put(light,0xa0,material[0]);
    }
    if(converted) CHECK(ConvertFiberToThread());
}
int main(void)
{
    setup(); lost_commit_controls(); scalar_controls(); lost_emit_controls(); emergency_fiber_controls(); lifetime(); fiber_and_reuse(); borrowed_pool(); refresh_controls();
    sh_resource_instances_destroyed(model,sh_resource_instances_token(model),1);
    for(int i=0;i<4;i++) sh_resource_instances_forget(material[i]);
    CHECK(!captures); for(int i=0;i<16;i++) CHECK(!leases[i]); CHECK(!g_receipts);
    install_controls();
    if(failures) fprintf(stderr,"%d failures\n",failures); else puts("resource_light_origins_test: PASS");
    return failures?1:0;
}
