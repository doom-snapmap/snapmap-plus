#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/backend/audio_limits_native.c"

static int failures,threshold_calls,voice_calls,mode;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);failures++;}} while(0)
static sh_audio_limits_native native;
static sh_audio_limits_value live;
static CRITICAL_SECTION render_lock;
static void *manager;
static HANDLE attempted,checked;
static int set_threshold(float value,int priority)
{
    threshold_calls++;
    CHECK(render_lock.OwningThread==(HANDLE)(uintptr_t)GetCurrentThreadId());
    if(attempted) {SetEvent(attempted);CHECK(WaitForSingleObject(checked,5000)==WAIT_OBJECT_0);}
    if(mode==1)return 1; /* Native success is allowed to leave the value alone. */
    if(mode==2)return 2;
    if(value < -96.3f || value>0)return 31;
    if(priority<=live.threshold_priority) {
        live.threshold_priority=priority;live.threshold=value;
        live.amplitude=powf(10,value/20);
    }
    if(mode==3)RaiseException(0xe0100011,0,0,NULL);
    return 1;
}
static int set_voices(uint16_t value,int priority)
{
    voice_calls++;
    CHECK(render_lock.OwningThread==(HANDLE)(uintptr_t)GetCurrentThreadId());
    if(mode==4)return 1;
    if(mode==5)return 2;
    if(value && priority<=live.voices_priority){live.voices=value;live.voices_priority=priority;}
    if(mode==6 || mode==7) {
        if(mode==7)manager=NULL;
        RaiseException(0xe0100012,0,0,NULL);
    }
    return 1;
}
static void reset(void)
{
    CHECK(!native.pending);
    live=(sh_audio_limits_value){-48.0f,0.003981071f,0,1,64};
    manager=&live;mode=threshold_calls=voice_calls=0;
    native=(sh_audio_limits_native){set_threshold,set_voices,&live.threshold,&live.amplitude,
        &live.threshold_priority,&live.voices_priority,&live.voices,&manager,&render_lock,NULL};
}
static sh_audio_shared_policy policy(float threshold,uint16_t voices)
{
    /* Run the real shared compiler, rather than hand-building its ordinary output. */
    unsigned char stmg[18]={0};memcpy(stmg,&threshold,4);memcpy(stmg+4,&voices,2);
    sh_audio_shared_input input={"STMG",stmg,sizeof(stmg),0,0};
    sh_audio_shared_policy out={0};char error[256];
    CHECK(sh_audio_shared_policy_compile(&input,1,&out,error,sizeof(error)));return out;
}
static void test_apply_restore(void)
{
    reset();sh_audio_limits_change change={0},other={0};char error[256];
    sh_audio_limits_value before=live;sh_audio_shared_policy plan=policy(-72,256);
    CHECK(sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error))==1);
    CHECK(!*error && native.pending==&change && threshold_calls==1 && voice_calls==1);
    CHECK(live.threshold==-72 && live.voices==256 && live.threshold_priority==0 && live.voices_priority==1);
    CHECK(live.amplitude!=before.amplitude && change.changed==(AL_THRESHOLD|AL_VOICES));
    CHECK(sh_audio_limits_native_ready(&change,error,sizeof(error)));
    CHECK(!sh_audio_limits_native_apply(&native,&plan,&other,error,sizeof(error)) && !other.owner);
    CHECK(!sh_audio_limits_native_bind(&native,NULL,NULL,0) && native.pending==&change);
    CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)));
    CHECK(!change.owner && !native.pending && al_equal(&live,&before,3));
    CHECK(!sh_audio_limits_native_restore(&change,error,sizeof(error)));
    /* Commit makes the next transition capture the installed result; rollback
     * of that later transition restores the accepted package's values exactly. */
    CHECK(sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error))==1);
    CHECK(sh_audio_limits_native_ready(&change,error,sizeof(error)));
    sh_audio_limits_native_accept(&change);CHECK(!native.pending && !change.owner);
    before=live;sh_audio_shared_policy replacement=policy(-60,128);
    CHECK(sh_audio_limits_native_apply(&native,&replacement,&change,error,sizeof(error))==1);
    CHECK(live.threshold==-60 && live.voices==128);
    CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)) && al_equal(&live,&before,3));
    sh_audio_shared_policy_free(&replacement);sh_audio_shared_policy_free(&plan);
}
static void test_failure(void)
{
    sh_audio_shared_policy plan=policy(-72,256);char error[256];
    for(int fail=1;fail<=7;fail++) {
        reset();mode=fail;sh_audio_limits_value before=live;sh_audio_limits_change change={0};
        int result=sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error));
        CHECK(result==(fail==7 ? -1 : 0) && *error);
        if(fail==7) {
            CHECK(change.uncertain && native.pending==&change);
            CHECK(!sh_audio_limits_native_ready(&change,error,sizeof(error)));
            sh_audio_limits_native_accept(&change);CHECK(native.pending==&change);
            manager=&live;CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)));
        }
        CHECK(!native.pending && !change.owner && al_equal(&live,&before,3));
        CHECK(TryEnterCriticalSection(&render_lock));LeaveCriticalSection(&render_lock);
        mode=0;CHECK(sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error))==1);
        CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)));
    }
    reset();manager=NULL;sh_audio_limits_change change={0};
    CHECK(!sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error)) && !native.pending && !threshold_calls);
    manager=&live;live.amplitude=NAN;
    CHECK(!sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error)) && !native.pending);
    sh_audio_shared_policy_free(&plan);
}
static void test_noops_and_drift(void)
{
    reset();char error[256];sh_audio_limits_change change={0};
    sh_audio_shared_policy same=policy(live.threshold,live.voices);
    CHECK(sh_audio_limits_native_apply(&native,&same,&change,error,sizeof(error))==1);
    CHECK(!change.changed && !threshold_calls && !voice_calls);
    live.voices=65;
    CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)) && live.voices==65);
    sh_audio_shared_policy_free(&same);
    const float invalid[]={NAN,INFINITY,-INFINITY,1,-100};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        sh_audio_shared_policy noop=policy(invalid[i],0);
        CHECK(sh_audio_limits_native_apply(&native,&noop,&change,error,sizeof(error))==1);
        CHECK(!change.changed && !threshold_calls && !voice_calls);
        CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)));
        sh_audio_shared_policy_free(&noop);
    }
    sh_audio_shared_policy plan=policy(-72,256);
    CHECK(sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error))==1);
    live.threshold=-32;
    CHECK(!sh_audio_limits_native_ready(&change,error,sizeof(error)));
    CHECK(!sh_audio_limits_native_restore(&change,error,sizeof(error)) && native.pending==&change && live.threshold==-32);
    live=change.after;
    CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)));
    sh_audio_shared_policy_free(&plan);
    /* Modifying an unowned scalar is not grounds to overwrite it on rollback. */
    plan=policy(-72,live.voices);
    CHECK(sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error))==1);
    live.voices=512;CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)) && live.voices==512);
    sh_audio_shared_policy_free(&plan);
    reset();plan=policy(-96.3f,65535);
    CHECK(sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error))==1);
    CHECK(live.voices==65535 && live.threshold==-96.3f);
    CHECK(sh_audio_limits_native_restore(&change,error,sizeof(error)));
    sh_audio_shared_policy_free(&plan);
}
static void test_malformed(void)
{
    reset();char error[256];sh_audio_limits_change change={0};sh_audio_shared_policy plan=policy(-72,256);
    for(size_t i=0;i<plan.count;i++) {
        plan.settings[i].record.length++;
        CHECK(!sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error)) && !threshold_calls && !voice_calls);
        plan.settings[i].record.length--;
        plan.settings[i].record.key[1]=1;
        CHECK(!sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error)));
        plan.settings[i].record.key[1]=0;
    }
    sh_audio_shared_setting duplicate[2]={plan.settings[0],plan.settings[0]};
    sh_audio_shared_policy broken={duplicate,2};
    CHECK(!sh_audio_limits_native_apply(&native,&broken,&change,error,sizeof(error)));
    broken.settings=NULL;CHECK(!sh_audio_limits_native_apply(&native,&broken,&change,error,sizeof(error)));
    CHECK(!native.pending && !threshold_calls && !voice_calls);
    sh_audio_shared_policy_free(&plan);
}
static DWORD WINAPI check_lock(void *unused)
{
    (void)unused;CHECK(WaitForSingleObject(attempted,5000)==WAIT_OBJECT_0);
    int acquired=TryEnterCriticalSection(&render_lock);CHECK(!acquired);
    if(acquired)LeaveCriticalSection(&render_lock);
    SetEvent(checked);return 0;
}
static DWORD WINAPI wrong_owner(void *receipt)
{
    sh_audio_limits_change *change=receipt;char error[128];
    CHECK(!sh_audio_limits_native_ready(change,error,sizeof(error)));
    CHECK(!sh_audio_limits_native_restore(change,error,sizeof(error)));
    sh_audio_limits_native_accept(change);return 0;
}
static void test_threads(void)
{
    reset();sh_audio_shared_policy plan=policy(-72,256);sh_audio_limits_change change={0};char error[256];
    attempted=CreateEventA(NULL,TRUE,FALSE,NULL);checked=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE thread=CreateThread(NULL,0,check_lock,NULL,0,NULL);CHECK(thread && attempted && checked);
    CHECK(sh_audio_limits_native_apply(&native,&plan,&change,error,sizeof(error))==1);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    CloseHandle(attempted);CloseHandle(checked);attempted=checked=NULL;
    thread=CreateThread(NULL,0,wrong_owner,&change,0,NULL);CHECK(thread);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    CHECK(native.pending==&change && sh_audio_limits_native_restore(&change,error,sizeof(error)));
    sh_audio_shared_policy_free(&plan);
}
static void rip(unsigned char *p,const unsigned char *prefix,size_t n,const unsigned char *target)
{ int32_t relative=(int32_t)(target-(p+n+4));memcpy(p,prefix,n);memcpy(p+n,&relative,4); }
static void test_binding(void)
{
    unsigned char *image=calloc(1,0x4000);CHECK(image);if(!image)return;
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)image;dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x100;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(image+0x100);nt->Signature=IMAGE_NT_SIGNATURE;
    nt->FileHeader.NumberOfSections=2;nt->FileHeader.SizeOfOptionalHeader=sizeof(nt->OptionalHeader);
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=0x4000;
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    sections[0].VirtualAddress=0x1000;sections[0].Misc.VirtualSize=0x1000;
    sections[0].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;
    sections[1].VirtualAddress=0x3000;sections[1].Misc.VirtualSize=0x1000;
    sections[1].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    const unsigned char cmp[]={0x3b,0x15},store[]={0x89,0x15},voices[]={0x66,0x89,0x0d};
    const unsigned char threshold[]={0xf3,0x0f,0x11,0x3d},amplitude[]={0xf3,0x0f,0x11,0x35};
    const unsigned char lea[]={0x48,0x8d,0x0d},slot[]={0x48,0x8b,0x0d};
    rip(image+0x102a,cmp,2,image+0x3000);rip(image+0x1049,store,2,image+0x3000);
    rip(image+0x110b,cmp,2,image+0x3004);rip(image+0x1113,store,2,image+0x3004);
    rip(image+0x10d4,threshold,4,image+0x3024);rip(image+0x10dc,amplitude,4,image+0x3020);
    rip(image+0x1119,voices,3,image+0x3010);rip(image+0x121c,lea,3,image+0x3080);
    rip(image+0x131e,slot,3,image+0x3060);
    sig_result signatures[5]={{"AudioThresholdSet",SIG_OK,(uintptr_t)(image+0x1000),0x1000},
        {"AudioVoiceLimitSet",SIG_OK,(uintptr_t)(image+0x1100),0x1100},
        {"AudioRenderTick",SIG_OK,(uintptr_t)(image+0x1200),0x1200},
        {"AudioBankLoad",SIG_OK,(uintptr_t)(image+0x1300),0x1300}};
    sh_audio_limits_native bound={0};CHECK(sh_audio_limits_native_bind(&bound,image,signatures,4));
    CHECK(bound.threshold==(float *)(image+0x3024) && bound.amplitude==(float *)(image+0x3020));
    CHECK(bound.render_lock==(CRITICAL_SECTION *)(image+0x3080) && bound.manager_slot==(void **)(image+0x3060));
    signatures[4]=signatures[0];CHECK(!sh_audio_limits_native_bind(&bound,image,signatures,5) && !bound.set_threshold);
    for(size_t i=0;i<4;i++) {
        signatures[i].status=SIG_OK_HOOKED;CHECK(!sh_audio_limits_native_bind(&bound,image,signatures,4));signatures[i].status=SIG_OK;
    }
    image[0x10dc]=0;CHECK(!sh_audio_limits_native_bind(&bound,image,signatures,4));image[0x10dc]=0xf3;
    rip(image+0x1049,store,2,image+0x3010);CHECK(!sh_audio_limits_native_bind(&bound,image,signatures,4));
    rip(image+0x1049,store,2,image+0x3000);
    rip(image+0x121c,lea,3,image+0x3000);CHECK(!sh_audio_limits_native_bind(&bound,image,signatures,4));
    rip(image+0x121c,lea,3,image+0x3080);
    sections[1].Characteristics|=IMAGE_SCN_MEM_EXECUTE;CHECK(!sh_audio_limits_native_bind(&bound,image,signatures,4));
    sections[1].Characteristics&=~IMAGE_SCN_MEM_EXECUTE;
    sections[0].Misc.VirtualSize=0xe0;CHECK(!sh_audio_limits_native_bind(&bound,image,signatures,4));
    sections[0].Misc.VirtualSize=0x1000;CHECK(sh_audio_limits_native_bind(&bound,image,signatures,4));
    free(image);
}
int main(void)
{
    InitializeCriticalSection(&render_lock);
    test_apply_restore();test_failure();test_noops_and_drift();test_malformed();test_threads();test_binding();
    DeleteCriticalSection(&render_lock);
    printf("audio_limits_native_test: %s\n",failures ? "FAIL" : "PASS");return !!failures;
}
