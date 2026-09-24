#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/audio_environment_native.c"

static int failures,calls,fail_at,free_calls,free_fault,skip_set,lose_manager,allocations;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);failures++;}} while(0)
static sh_audio_environment live,*environment;
static void *manager;
static CRITICAL_SECTION lock;
static sh_audio_environment_native native;
static void *tracked[64];
static HANDLE attempted,checked;
static void *allocate(size_t bytes)
{
    void *p=malloc(bytes);CHECK(p);
    for(unsigned i=0;i<64;i++)if(!tracked[i]){tracked[i]=p;allocations++;return p;}
    CHECK(0);return NULL;
}
static void release(void *p)
{
    for(unsigned i=0;i<64;i++)if(tracked[i]==p){tracked[i]=NULL;allocations--;free(p);return;}
    CHECK(0);
}
static int curve_set(sh_audio_native_curve *curve,const void *points,uint32_t count,uint32_t scaling)
{
    CHECK(lock.OwningThread==(HANDLE)(uintptr_t)GetCurrentThreadId());
    if(curve->points) {
        free_calls++;release(curve->points);
        if(free_fault)RaiseException(0xe0100081,0,0,NULL);
    }
    memset(curve,0,sizeof(*curve));
    if(!points || !count)return 31;
    if(calls==fail_at)return 52;
    curve->points=allocate((size_t)count*12);curve->count=count;curve->scaling=scaling;
    memcpy(curve->points,points,(size_t)count*12);return 1;
}
static int environment_set(sh_audio_environment *value,int row,int column,uint32_t count,const void *points,uint32_t scaling)
{
    calls++;CHECK(value!=&live);
    if(attempted){SetEvent(attempted);CHECK(WaitForSingleObject(checked,5000)==WAIT_OBJECT_0);}
    if(skip_set)return 1;
    int result=curve_set(value->curves+row*3+column,points,count,scaling);
    /* Fixture models only the observed native first-column normalization. */
    if(result==1 && !column) {
        sh_audio_native_curve *c=value->curves+row*3;
        if(!scaling)c->scaling=4;
        if(scaling==2) {
            for(uint32_t i=0;i<count;i++){float y;memcpy(&y,(char *)c->points+i*12+4,4);y+=1;memcpy((char *)c->points+i*12+4,&y,4);}
            c->scaling=0;
        }
    }
    if(lose_manager)manager=NULL;
    return result;
}
static void point(unsigned char *body,float x,float y)
{ uint32_t interpolation=4;memcpy(body,&x,4);memcpy(body+4,&y,4);memcpy(body+8,&interpolation,4); }
static void env_body(unsigned char *body,float offset)
{
    for(unsigned i=0;i<6;i++) {
        unsigned char *p=body+i*28;p[0]=1;p[1]=(unsigned char)(i==0 ? 2 : 0);p[2]=2;p[3]=0;
        point(p+4,0,offset+(float)i);point(p+16,100,offset+(float)i+2);
    }
}
static sh_audio_shared_policy plan(float offset)
{
    unsigned char body[168];env_body(body,offset);sh_audio_shared_policy out={0};char error[256];
    sh_audio_shared_input input={"ENVS",body,sizeof(body),0,0};
    CHECK(sh_audio_shared_policy_compile(&input,1,&out,error,sizeof(error)));return out;
}
static void reset(void)
{
    CHECK(!native.pending);CHECK(!allocations);
    memset(&live,0,sizeof(live));manager=&live;environment=&live;
    calls=fail_at=free_calls=free_fault=skip_set=lose_manager=0;
    native=(sh_audio_environment_native){environment_set,curve_set,&environment,&manager,&lock,NULL};
    unsigned char body[168];env_body(body,0);sh_audio_environment initial={0};
    EnterCriticalSection(&lock);
    for(unsigned i=0;i<6;i++){initial.enabled[i]=1;CHECK(environment_set(&initial,(int)i/3,(int)i%3,2,body+i*28+4,body[i*28+1])==1);}
    LeaveCriticalSection(&lock);live=initial;calls=free_calls=0;CHECK(allocations==6);
}
static void clear_live(void)
{
    EnterCriticalSection(&lock);
    for(unsigned i=0;i<6;i++)CHECK(curve_set(live.curves+i,NULL,0,0)==31);
    LeaveCriticalSection(&lock);CHECK(!allocations);
}
static void test_transition(void)
{
    reset();sh_audio_shared_policy p=plan(10);sh_audio_environment before=live;
    sh_audio_environment_change change={0},other={0};char error[256];
    CHECK(sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error))==1 && !*error);
    CHECK(allocations==12 && change.changed==63 && change.installed==63 && native.pending==&change);
    CHECK(live.curves[0].scaling==0 && live.curves[3].scaling==4);
    CHECK(!sh_audio_environment_native_apply(&native,&p,&other,error,sizeof(error)) && !other.owner);
    CHECK(!sh_audio_environment_native_bind(&native,NULL,NULL,0,NULL) && native.pending==&change);
    CHECK(sh_audio_environment_native_ready(&change,error,sizeof(error)));
    sh_audio_shared_policy_free(&p); /* Receipt owns the copied native data. */
    CHECK(sh_audio_environment_native_restore(&change,error,sizeof(error)) && !change.owner);
    CHECK(!memcmp(&before,&live,sizeof(live)) && allocations==6 && free_calls==6);
    p=plan(20);CHECK(sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error))==1);
    CHECK(sh_audio_environment_native_ready(&change,error,sizeof(error)));
    sh_audio_environment after=live;int freed=free_calls;
    sh_audio_environment_native_accept(&change);CHECK(change.owner && allocations==12 && free_calls==freed);
    CHECK(!sh_audio_environment_native_restore(&change,error,sizeof(error)));
    CHECK(sh_audio_environment_native_release(&change,error,sizeof(error)) && !native.pending);
    CHECK(allocations==6 && !memcmp(&after,&live,sizeof(live)));
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_noops_and_empty(void)
{
    reset();sh_audio_environment before=live;sh_audio_shared_policy p=plan(0);char error[256];
    sh_audio_environment_change change={0};
    CHECK(sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error))==1);
    CHECK(!change.changed && allocations==12 && !memcmp(&live,&before,sizeof(live)));
    sh_audio_environment_native_accept(&change);CHECK(sh_audio_environment_native_release(&change,error,sizeof(error)) && allocations==6);
    sh_audio_shared_policy_free(&p);
    unsigned char empty[24]={0};for(unsigned i=0;i<6;i++)empty[i*4]=1;
    sh_audio_shared_input input={"ENVS",empty,sizeof(empty),0,0};
    CHECK(sh_audio_shared_policy_compile(&input,1,&p,error,sizeof(error)));
    CHECK(sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error))==1 && change.changed==63);
    for(unsigned i=0;i<6;i++)CHECK(!live.curves[i].points && !live.curves[i].count && live.enabled[i]);
    CHECK(sh_audio_environment_native_restore(&change,error,sizeof(error)) && !memcmp(&live,&before,sizeof(live)));
    sh_audio_shared_policy_free(&p);
    sh_audio_shared_policy absent={0};CHECK(sh_audio_environment_native_apply(&native,&absent,&change,error,sizeof(error))==1);
    CHECK(!change.changed && allocations==6);CHECK(sh_audio_environment_native_restore(&change,error,sizeof(error)));
    clear_live();
}
static void test_failures(void)
{
    sh_audio_shared_policy p=plan(15);char error[256];
    for(int fail=1;fail<=6;fail++) {
        reset();sh_audio_environment before=live;fail_at=fail;sh_audio_environment_change change={0};
        CHECK(!sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error)) && *error);
        CHECK(!change.owner && !native.pending && allocations==6 && !memcmp(&before,&live,sizeof(live)));
        fail_at=0;clear_live();
    }
    reset();sh_audio_environment_change change={0};sh_audio_environment before=live;
    skip_set=1;CHECK(!sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error)) && !native.pending);
    CHECK(!memcmp(&before,&live,sizeof(live)) && allocations==6);skip_set=0;
    lose_manager=1;CHECK(sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error))==-1);
    CHECK(change.owner && !change.installed && !memcmp(&before,&live,sizeof(live)));
    CHECK(!sh_audio_environment_native_restore(&change,error,sizeof(error)));
    manager=&live;lose_manager=0;CHECK(sh_audio_environment_native_restore(&change,error,sizeof(error)) && allocations==6);
    CHECK(sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error))==1);
    sh_audio_environment_native_accept(&change);free_fault=1;
    CHECK(!sh_audio_environment_native_release(&change,error,sizeof(error)) && change.uncertain && native.pending==&change);
    int freed=free_calls;CHECK(!sh_audio_environment_native_release(&change,error,sizeof(error)) && free_calls==freed);
    CHECK(!sh_audio_environment_native_restore(&change,error,sizeof(error)));
    /* Explicit fixture teardown: ownership was uncertain after native free.
     * Production must retain it; only the fixture allocator knows what freed. */
    free_fault=0;for(unsigned i=0;i<64;i++)if(tracked[i]){free(tracked[i]);tracked[i]=NULL;}
    allocations=0;native.pending=NULL;memset(&live,0,sizeof(live));
    sh_audio_shared_policy_free(&p);
}
static DWORD WINAPI wrong_owner(void *argument)
{
    char error[256];sh_audio_environment_change *c=argument;
    CHECK(!sh_audio_environment_native_ready(c,error,sizeof(error)));
    CHECK(!sh_audio_environment_native_restore(c,error,sizeof(error)));
    sh_audio_environment_native_accept(c);CHECK(c->state==AE_APPLIED);return 0;
}
static DWORD WINAPI observe_lock(void *unused)
{
    (void)unused;CHECK(WaitForSingleObject(attempted,5000)==WAIT_OBJECT_0);
    int acquired=TryEnterCriticalSection(&lock);CHECK(!acquired);if(acquired)LeaveCriticalSection(&lock);
    SetEvent(checked);return 0;
}
static void test_ownership(void)
{
    reset();char error[256];sh_audio_environment_change change={0};sh_audio_shared_policy p=plan(10);
    attempted=CreateEventA(NULL,TRUE,FALSE,NULL);checked=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE thread=CreateThread(NULL,0,observe_lock,NULL,0,NULL);CHECK(thread);
    CHECK(sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error))==1);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    CloseHandle(attempted);CloseHandle(checked);attempted=checked=NULL;
    thread=CreateThread(NULL,0,wrong_owner,&change,0,NULL);CHECK(thread);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    live.enabled[2]=0;CHECK(!sh_audio_environment_native_ready(&change,error,sizeof(error)));
    CHECK(!sh_audio_environment_native_restore(&change,error,sizeof(error)) && !live.enabled[2]);live.enabled[2]=1;
    sh_audio_environment other=live;environment=&other;
    CHECK(!sh_audio_environment_native_restore(&change,error,sizeof(error)));environment=&live;
    CHECK(sh_audio_environment_native_restore(&change,error,sizeof(error)) && allocations==6);
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_malformed(void)
{
    reset();char error[256];sh_audio_environment_change change={0};sh_audio_shared_policy p=plan(10);
    sh_audio_shared_record record=p.settings[0].record;
    p.settings[0].record.length--;CHECK(!sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error)) && !calls);
    p.settings[0].record=record;p.settings[0].record.key[0]=6;
    CHECK(!sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error)) && !calls);
    p.settings[0].record=record;p.settings[0].record.key[0]=1;
    CHECK(!sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error)) && !calls);
    p.settings[0].record=record;p.settings[0].record.offset=1;
    CHECK(!sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error)) && !calls);
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_composed_edits(void)
{
    reset();char error[256];unsigned char baseline[168],a[168],b[168];env_body(baseline,0);
    memcpy(a,baseline,sizeof(a));memcpy(b,baseline,sizeof(b));
    a[28]=0;b[112]=0;
    sh_audio_shared_input inputs[]={{"ENVS",baseline,sizeof(baseline),0,1},
        {"ENVS",a,sizeof(a),1,0},{"ENVS",b,sizeof(b),2,0}};
    sh_audio_shared_policy p={0};CHECK(sh_audio_shared_policy_compile(inputs,3,&p,error,sizeof(error)));
    sh_audio_environment_change change={0};sh_audio_environment before=live;
    CHECK(sh_audio_environment_native_apply(&native,&p,&change,error,sizeof(error))==1);
    CHECK(change.changed==((1u<<1)|(1u<<4)) && !live.enabled[1] && !live.enabled[4]);
    CHECK(live.curves[0].points==before.curves[0].points && live.curves[3].points==before.curves[3].points);
    live.enabled[5]=0; /* An unowned control remains outside this receipt. */
    CHECK(sh_audio_environment_native_restore(&change,error,sizeof(error)) && !live.enabled[5]);
    CHECK(live.enabled[1] && live.enabled[4] && allocations==6);
    sh_audio_shared_policy_free(&p);clear_live();
}
static void relative(unsigned char *p,const unsigned char *prefix,size_t n,const unsigned char *target)
{ int32_t delta=(int32_t)(target-(p+n+4));memcpy(p,prefix,n);memcpy(p+n,&delta,4); }
static void test_binding(void)
{
    unsigned char *image=calloc(1,0x4000);CHECK(image);if(!image)return;
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)image;dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x100;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(image+0x100);nt->Signature=IMAGE_NT_SIGNATURE;
    nt->FileHeader.NumberOfSections=2;nt->FileHeader.SizeOfOptionalHeader=sizeof(nt->OptionalHeader);
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=0x4000;
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    sections[0].VirtualAddress=0x1000;sections[0].Misc.VirtualSize=0x1000;sections[0].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;
    sections[1].VirtualAddress=0x3000;sections[1].Misc.VirtualSize=0x1000;sections[1].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    const unsigned char rax[]={0x48,0x8b,0x05},rcx[]={0x48,0x8b,0x0d},call[]={0xe8};
    relative(image+0x108d,rax,3,image+0x3000);relative(image+0x1127,rcx,3,image+0x3000);
    relative(image+0x1137,call,1,image+0x1200);relative(image+0x1237,call,1,image+0x1300);
    sig_result signatures[3]={{"AudioEnvironmentRead",SIG_OK,(uintptr_t)(image+0x1000),0x1000},
        {"AudioEnvironmentSet",SIG_OK,(uintptr_t)(image+0x1200),0x1200}};
    sh_audio_limits_native limits={0};limits.manager_slot=(void **)(image+0x3010);limits.render_lock=(CRITICAL_SECTION *)(image+0x3080);
    sh_audio_environment_native bound={0};CHECK(sh_audio_environment_native_bind(&bound,image,signatures,2,&limits));
    CHECK((uintptr_t)bound.set_curve==(uintptr_t)(image+0x1300) && bound.environment_slot==(sh_audio_environment **)(image+0x3000));
    signatures[2]=signatures[0];CHECK(!sh_audio_environment_native_bind(&bound,image,signatures,3,&limits));
    signatures[0].status=SIG_OK_HOOKED;CHECK(!sh_audio_environment_native_bind(&bound,image,signatures,2,&limits));signatures[0].status=SIG_OK;
    relative(image+0x1127,rcx,3,image+0x3010);CHECK(!sh_audio_environment_native_bind(&bound,image,signatures,2,&limits));
    relative(image+0x1127,rcx,3,image+0x3000);relative(image+0x1137,call,1,image+0x1300);
    CHECK(!sh_audio_environment_native_bind(&bound,image,signatures,2,&limits));relative(image+0x1137,call,1,image+0x1200);
    relative(image+0x1237,call,1,image+0x3000);CHECK(!sh_audio_environment_native_bind(&bound,image,signatures,2,&limits));
    relative(image+0x1237,call,1,image+0x1300);sections[1].Characteristics|=IMAGE_SCN_MEM_EXECUTE;
    CHECK(!sh_audio_environment_native_bind(&bound,image,signatures,2,&limits));sections[1].Characteristics&=~IMAGE_SCN_MEM_EXECUTE;
    CHECK(sh_audio_environment_native_bind(&bound,image,signatures,2,&limits));free(image);
}
int main(void)
{
    InitializeCriticalSection(&lock);
    test_transition();test_noops_and_empty();test_failures();test_ownership();test_malformed();test_composed_edits();test_binding();
    DeleteCriticalSection(&lock);CHECK(!allocations);
    printf("audio_environment_native_test: %s\n",failures ? "FAIL" : "PASS");return !!failures;
}
