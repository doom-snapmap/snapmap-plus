#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/audio_bank_commands.c"

static int failures, initialized, submitted, waited, callbacks;
static int init_result, submit_result, bank_result, threaded, throw_submit;
static HANDLE queued, allowed, finished;
static abc_command copied;
static abc_completion *seen;
static void *manager, *callback;
static sh_audio_bank_commands api;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while (0)

static void completed(uint32_t id,const void *memory,int result,int pool,void *cookie)
{
    abc_completion *completion=(abc_completion *)cookie;
    CHECK(id==777 && !memory && pool==-1 && completion==seen);
    ++callbacks; completion->result=result; completion->pool=pool;
    CHECK(SetEvent(completion->event));
}
static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    CHECK(WaitForSingleObject(allowed,5000)==WAIT_OBJECT_0);
    ((void (*)(uint32_t,const void *,int,int,void *))copied.callback)
        (copied.id,copied.memory,bank_result,copied.pool,copied.completion);
    return 0;
}
static int initialize(void *self,abc_completion *completion)
{
    CHECK(self==manager); ++initialized; seen=completion;
    if(init_result!=1)return init_result;
    if(threaded) { completion->event=CreateEventA(NULL,FALSE,FALSE,NULL); CHECK(completion->event); }
    return init_result;
}
static int submit(void *self,const abc_command *command)
{
    CHECK(self==manager); ++submitted;
    CHECK(command->operation==0 && command->mode==1 && command->id==777 && command->pool==-1);
    CHECK(command->completion==seen && command->callback==callback);
    CHECK(!command->memory && !command->length);
    CHECK(!command->padding && !command->padding2 && !command->padding3);
    copied=*command;
    if(throw_submit)RaiseException(0xe0000710,0,0,NULL);
    if(submit_result==1 && threaded) {
        HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);
        CHECK(thread); if(thread)CloseHandle(thread);
    }
    CHECK(SetEvent(queued)); return submit_result;
}
static int wait_result(void *self,abc_completion *completion,int result)
{
    CHECK(self==manager && completion==seen && result==submit_result); ++waited;
    if(threaded) {
        if(result==1) { CHECK(WaitForSingleObject(completion->event,5000)==WAIT_OBJECT_0); result=completion->result; }
        CHECK(CloseHandle(completion->event)); completion->event=NULL;
    } else if(result==1)result=bank_result;
    return result;
}
static void *table[6]={NULL,NULL,NULL,(void *)submit,(void *)initialize,(void *)wait_result};
static void **object=table;
static void reset(int use_thread)
{
    initialized=submitted=waited=callbacks=0; seen=NULL;
    init_result=submit_result=bank_result=1; throw_submit=0; threaded=use_thread;
    memset(&copied,0,sizeof(copied)); memset(&api,0,sizeof(api));
    manager=&object; callback=threaded?(void *)completed:NULL;
    api.manager_slot=&manager; api.callback_slot=&callback;
    ResetEvent(queued); ResetEvent(allowed); ResetEvent(finished);
}
static DWORD WINAPI load(void *out)
{
    *(int *)out=sh_audio_bank_commands_load(&api,777); SetEvent(finished); return 0;
}
static void test_commands(void)
{
    int result=0;
    reset(0);
    CHECK(sh_audio_bank_commands_load(&api,777)==1);
    CHECK(initialized==1 && submitted==1 && waited==1 && !api.uncertain && !callbacks);
    bank_result=69;
    CHECK(sh_audio_bank_commands_load(&api,777)==69); /* Do not turn borrowed ownership into success. */
    bank_result=7;
    CHECK(sh_audio_bank_commands_load(&api,777)==7);
    reset(0); init_result=2;
    CHECK(sh_audio_bank_commands_load(&api,777)==2 && initialized==1 && !submitted && !waited);
    reset(1); submit_result=2;
    CHECK(sh_audio_bank_commands_load(&api,777)==2 && initialized==1 && submitted==1 && waited==1 && !callbacks);
    reset(1); bank_result=69;
    HANDLE thread=CreateThread(NULL,0,load,&result,0,NULL); CHECK(thread);
    CHECK(WaitForSingleObject(queued,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(finished,0)==WAIT_TIMEOUT); /* The provider cannot publish before completion. */
    CHECK(SetEvent(allowed));
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); CHECK(CloseHandle(thread));
    CHECK(result==69 && initialized==1 && submitted==1 && waited==1 && callbacks==1 && !api.uncertain);
    reset(0); manager=NULL;
    CHECK(sh_audio_bank_commands_load(&api,777)==2 && !initialized);
    manager=(void *)1;
    CHECK(sh_audio_bank_commands_load(&api,777)==2 && !initialized);
    reset(0);
    CHECK(sh_audio_bank_commands_load(&api,0)==2 && !initialized);
    /* An exception after native submission must not leave a dangling callback
     * cookie or authorize a second command while the first outcome is unknown. */
    reset(1); throw_submit=1;
    CHECK(sh_audio_bank_commands_load(&api,777)==-1 && api.uncertain && seen==(abc_completion *)api.completion && !waited);
    CHECK(sh_audio_bank_commands_load(&api,777)==-1 && submitted==1);
    CHECK(!sh_audio_bank_commands_bind(&api,NULL,NULL,0) && api.uncertain);
    completed(777,NULL,1,-1,copied.completion);
    CHECK(WaitForSingleObject(seen->event,0)==WAIT_OBJECT_0 && seen->result==1);
    CHECK(CloseHandle(seen->event)); api.uncertain=0;
}

/* A synthetic PE exercises rejection without calling addresses in the image. */
static void put_slot(unsigned char *p,unsigned char reg,unsigned char *target)
{
    int32_t delta=(int32_t)(target-(p+7));
    p[0]=0x48; p[1]=0x8b; p[2]=reg; memcpy(p+3,&delta,4);
}
static void test_binding(void)
{
    unsigned char *image=calloc(1,0x4000), *entry=image+0x1000;
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)image;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(image+0x100);
    IMAGE_SECTION_HEADER *sections;
    sig_result signature={"AudioBankLoad",SIG_OK,(uintptr_t)entry,0x1000};
    sh_audio_bank_commands binding={0};
    CHECK(image); if(!image)return;
    dos->e_magic=IMAGE_DOS_SIGNATURE; dos->e_lfanew=0x100;
    nt->Signature=IMAGE_NT_SIGNATURE; nt->FileHeader.NumberOfSections=2;
    nt->FileHeader.SizeOfOptionalHeader=sizeof(nt->OptionalHeader);
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC; nt->OptionalHeader.SizeOfImage=0x4000;
    sections=IMAGE_FIRST_SECTION(nt);
    sections[0].VirtualAddress=0x1000; sections[0].Misc.VirtualSize=0x1000;
    sections[0].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;
    sections[1].VirtualAddress=0x3000; sections[1].Misc.VirtualSize=0x1000;
    sections[1].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    put_slot(entry+0x1e,0x0d,image+0x3000);
    put_slot(entry+0x68,0x0d,image+0x3000);
    put_slot(entry+0xac,0x0d,image+0x3000);
    put_slot(entry+0x41,0x05,image+0x3010);
    entry[0x28]=entry[0xa9]=0xff; entry[0x29]=entry[0xaa]=0x50;
    entry[0x2a]=0x20; entry[0xab]=0x18;
    entry[0xbe]=0x41; entry[0xbf]=0xff; entry[0xc0]=0x51; entry[0xc1]=0x28;
    CHECK(sh_audio_bank_commands_bind(&binding,image,&signature,1));
    CHECK(binding.manager_slot==(void **)(image+0x3000) && binding.callback_slot==(void **)(image+0x3010));
    CHECK(sh_audio_bank_commands_load(&binding,777)==2); /* Startup slot is empty. */
    entry[0xc1]=0x18;
    CHECK(!sh_audio_bank_commands_bind(&binding,image,&signature,1) && !binding.manager_slot);
    entry[0xc1]=0x28;
    put_slot(entry+0xac,0x0d,image+0x3010);
    CHECK(!sh_audio_bank_commands_bind(&binding,image,&signature,1));
    put_slot(entry+0xac,0x0d,image+0x3000);
    sections[1].Characteristics|=IMAGE_SCN_MEM_EXECUTE;
    CHECK(!sh_audio_bank_commands_bind(&binding,image,&signature,1));
    sections[1].Characteristics&=~IMAGE_SCN_MEM_EXECUTE;
    signature.status=SIG_OK_HOOKED;
    CHECK(!sh_audio_bank_commands_bind(&binding,image,&signature,1));
    signature.status=SIG_AMBIGUOUS;
    CHECK(!sh_audio_bank_commands_bind(&binding,image,&signature,1));
    signature.status=SIG_OK;
    CHECK(sh_audio_bank_commands_bind(&binding,image,&signature,1));
    {
        sh_audio_registry registry={0};
        sig_result all[3]={signature,{"AudioEventPost",SIG_OK,(uintptr_t)(image+0x1200),0x1200},
            {"AudioBankPrepare",SIG_OK,(uintptr_t)(image+0x1300),0x1300}};
        static const unsigned char add[]={0x48,0x81,0xc1,0xa8,0x12,0,0};
        put_slot(image+0x121a,0x0d,image+0x3020);memcpy(image+0x1221,add,sizeof(add));
        put_slot(image+0x1326,0x0d,image+0x3040);image[0x1327]=0x8d;
        CHECK(sh_audio_registry_bind(&registry,image,all,3));
        CHECK(registry.index_slot==(void **)(image+0x3020) && registry.bank_lock==(CRITICAL_SECTION *)(image+0x3040));
        char error[128];CHECK(sh_audio_registry_occupied(&registry,42,error,sizeof(error))==-1 && *error);
        image[0x1224]=0xa0;
        CHECK(!sh_audio_registry_bind(&registry,image,all,3) && !registry.index_slot);
        image[0x1224]=0xa8;
        all[1].status=SIG_AMBIGUOUS;CHECK(!sh_audio_registry_bind(&registry,image,all,3));
        all[1].status=SIG_OK;CHECK(sh_audio_registry_bind(&registry,image,all,3));
    }
    free(image);
}

static sh_audio_registry probe;
static HANDLE probe_entered,probe_done;
static uint32_t probe_id;
static DWORD WINAPI probe_worker(void *out)
{
    char error[128];SetEvent(probe_entered);
    *(int *)out=sh_audio_registry_occupied(&probe,probe_id,error,sizeof(error));
    SetEvent(probe_done);return 0;
}
static void ptr(unsigned char *at,void *value) { memcpy(at,&value,sizeof(value)); }
static void number(unsigned char *at,uint32_t value) { memcpy(at,&value,sizeof(value)); }
static void test_occupancy(void)
{
    unsigned char *index=calloc(1,0x4aa0),*banks=calloc(1,0xca0);
    unsigned char object[32]={0},peer[32]={0},media[64]={0},usage[128]={0};
    CRITICAL_SECTION bank_lock;char error[128];uint32_t id=421;int result=0;
    CHECK(index && banks);if(!index || !banks){free(index);free(banks);return;}
    for(size_t i=0;i<12;i++)InitializeCriticalSection((CRITICAL_SECTION *)(index+i*0x638));
    InitializeCriticalSection((CRITICAL_SECTION *)(banks+0x48));InitializeCriticalSection(&bank_lock);
    probe=(sh_audio_registry){(void **)&banks,(void **)&index,&bank_lock};
    CHECK(sh_audio_registry_occupied(&probe,id,error,sizeof(error))==0 && !*error);
    number(object+0x10,id);number(object+0x14,77);number(peer+0x10,id+193);ptr(peer+8,object);
    for(size_t i=0;i<12;i++) {
        unsigned char *slot=index+i*0x638+0x28+(id%193)*8;ptr(slot,peer);
        CHECK(sh_audio_registry_occupied(&probe,id,error,sizeof(error))==1);
        uint32_t refs;memcpy(&refs,object+0x14,4);CHECK(refs==77);
        ptr(slot,NULL);
    }
    number(media+8,id);ptr(banks+0x80+(id%193)*8,media);
    CHECK(sh_audio_registry_occupied(&probe,id,error,sizeof(error))==1);
    ptr(banks+0x80+(id%193)*8,NULL);
    number(usage+0x10,id);ptr(usage+0x18,(void *)0xabcdef00);
    ptr(banks+0x690+192*8,usage); /* Memory-backed bank in a different bucket. */
    CHECK(sh_audio_registry_occupied(&probe,id,error,sizeof(error))==1);
    ptr(banks+0x690+192*8,NULL);
    unsigned char *slot=index+7*0x638+0x28+(id%193)*8;
    ptr(slot,peer);ptr(peer+8,peer);
    CHECK(sh_audio_registry_occupied(&probe,id,error,sizeof(error))==-1 && *error);
    ptr(peer+8,object);ptr(object+8,(void *)1);number(object+0x10,id+386);
    CHECK(sh_audio_registry_occupied(&probe,id,error,sizeof(error))==-1 && *error);
    ptr(object+8,NULL);number(object+0x10,id);
    /* Both the error paths above must release the table lock, and ordinary
     * queries must wait for the native writer before inspecting its chain. */
    probe_entered=CreateEventA(NULL,TRUE,FALSE,NULL);probe_done=CreateEventA(NULL,TRUE,FALSE,NULL);probe_id=id;
    CRITICAL_SECTION *lock=(CRITICAL_SECTION *)(index+7*0x638);
    EnterCriticalSection(lock);
    HANDLE thread=CreateThread(NULL,0,probe_worker,&result,0,NULL);CHECK(thread);
    CHECK(WaitForSingleObject(probe_entered,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(probe_done,0)==WAIT_TIMEOUT);
    LeaveCriticalSection(lock);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0 && result==1);
    CloseHandle(thread);CloseHandle(probe_entered);CloseHandle(probe_done);
    for(size_t i=0;i<12;i++)DeleteCriticalSection((CRITICAL_SECTION *)(index+i*0x638));
    DeleteCriticalSection((CRITICAL_SECTION *)(banks+0x48));DeleteCriticalSection(&bank_lock);
    free(index);free(banks);
}
/* Usage slots: file loads use memory key zero; playback items hold slot
 * references. The probe reads them under the bank lock and never writes. */
static void test_bank_usage(void)
{
    unsigned char *banks=calloc(1,0xca0),first[0x80]={0},other[0x80]={0},memory_bank[0x80]={0};
    CRITICAL_SECTION bank_lock;char error[128];sh_audio_bank_usage usage;uint32_t id=421;
    CHECK(banks);if(!banks)return;
    InitializeCriticalSection(&bank_lock);
    sh_audio_registry registry={(void **)&banks,NULL,&bank_lock};
    CHECK(sh_audio_registry_bank(&registry,id,&usage,error,sizeof(error))==0 && !usage.references);
    /* A memory-backed slot with the same ID is a different load, not ours. */
    number(memory_bank+0x10,id);ptr(memory_bank+0x18,(void *)0x1000);number(memory_bank+0x70,5);
    number(other+0x10,id+193);ptr(other+0x20,memory_bank);ptr(banks+0x690+(id%193)*8,other);
    CHECK(sh_audio_registry_bank(&registry,id,&usage,error,sizeof(error))==0);
    number(first+0x10,id);number(first+0x70,3);number(first+0x74,1);first[0x78]=1;ptr(memory_bank+0x20,first);
    CHECK(sh_audio_registry_bank(&registry,id,&usage,error,sizeof(error))==1);
    CHECK(usage.references==3 && usage.prepared==1 && usage.loaded==1);
    uint32_t refs;memcpy(&refs,first+0x70,4);CHECK(refs==3);
    first[0x78]=0;CHECK(sh_audio_registry_bank(&registry,id,&usage,error,sizeof(error))==1 && !usage.loaded);
    /* Corrupt chains and a missing manager are unavailable, never "absent". */
    ptr(first+0x20,other);
    CHECK(sh_audio_registry_bank(&registry,id+1,&usage,error,sizeof(error))==-1 && *error);
    ptr(first+0x20,NULL);
    CHECK(sh_audio_registry_bank(&registry,0,&usage,error,sizeof(error))==-1);
    void *none=NULL;registry.manager_slot=&none;
    CHECK(sh_audio_registry_bank(&registry,id,&usage,error,sizeof(error))==-1 && !usage.references);
    /* The probe waits for a native writer holding the bank lock. */
    registry.manager_slot=(void **)&banks;
    EnterCriticalSection(&bank_lock);
    CHECK(sh_audio_registry_bank(&registry,id,&usage,error,sizeof(error))==1); /* recursive owner */
    LeaveCriticalSection(&bank_lock);
    DeleteCriticalSection(&bank_lock);free(banks);
}
int main(void)
{
    queued=CreateEventA(NULL,TRUE,FALSE,NULL); allowed=CreateEventA(NULL,TRUE,FALSE,NULL);
    finished=CreateEventA(NULL,TRUE,FALSE,NULL); CHECK(queued && allowed && finished);
    test_commands(); test_binding(); test_occupancy(); test_bank_usage();
    CloseHandle(queued); CloseHandle(allowed); CloseHandle(finished);
    if(failures)return 1;
    puts("native numeric bank command tests passed"); return 0;
}
