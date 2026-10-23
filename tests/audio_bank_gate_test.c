#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/audio_bank_gate.c"
#include "../src/backend/hook.h"

static int failures,calls;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);failures++;}} while(0)
void backend_log(const char *text){(void)text;}
static sh_audio_bank_gate gate;
static sh_audio_bank_scope scope;
static void *trampoline,*manager;
static CRITICAL_SECTION render;
static unsigned char *image;
static RUNTIME_FUNCTION runtime_function;
static int function_table;
static HANDLE entered,allowed,attempted,finished;
typedef struct command { int (*callback)(void *,const struct command *);int result,mode; } command;
static int callback(void *self,const command *cmd)
{
    char error[256];CHECK(self==manager);calls++;
    if(cmd->mode==1){SetEvent(entered);CHECK(WaitForSingleObject(allowed,5000)==WAIT_OBJECT_0);}
    if(cmd->mode==2)RaiseException(0xe0100082,0,0,NULL);
    if(cmd->mode==3){sh_audio_bank_scope reentrant={0};CHECK(!sh_audio_bank_gate_begin(&gate,&reentrant,error,sizeof(error)));}
    return cmd->result;
}
static int detour(void *self,const void *cmd){return sh_audio_bank_gate_dispatch(&gate,self,cmd);}
static DWORD WINAPI native_call(void *argument)
{
    command *cmd=argument;SetEvent(attempted);
    CHECK(gate.entry(manager,cmd)==cmd->result);SetEvent(finished);return 0;
}
static DWORD WINAPI publication(void *unused)
{
    (void)unused;char error[256];sh_audio_bank_scope own={0};SetEvent(attempted);
    CHECK(sh_audio_bank_gate_begin(&gate,&own,error,sizeof(error)));SetEvent(entered);
    CHECK(sh_audio_bank_gate_ready(&own,error,sizeof(error)));
    CHECK(sh_audio_bank_gate_end(&own,error,sizeof(error)));SetEvent(finished);return 0;
}
static DWORD WINAPI wrong_thread(void *unused)
{
    (void)unused;char error[256];CHECK(!sh_audio_bank_gate_ready(&scope,error,sizeof(error)));
    CHECK(!sh_audio_bank_gate_end(&scope,error,sizeof(error)) && scope.owner==&gate);return 0;
}
static void reset_events(void)
{ ResetEvent(entered);ResetEvent(allowed);ResetEvent(attempted);ResetEvent(finished); }
static void test_native_hook(void)
{
    char error[256];command cmd={callback,69,0};int before=calls;
    CHECK(gate.entry(manager,&cmd)==69 && calls==before+1 && !gate.dispatch_depth);
    CHECK(sh_audio_bank_gate_begin(&gate,&scope,error,sizeof(error)));
    CHECK(sh_audio_bank_gate_ready(&scope,error,sizeof(error)));
    reset_events();HANDLE worker=CreateThread(NULL,0,native_call,&cmd,0,NULL);CHECK(worker);
    CHECK(WaitForSingleObject(attempted,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(finished,30)==WAIT_TIMEOUT && calls==before+1);
    CHECK(sh_audio_bank_gate_end(&scope,error,sizeof(error)));
    CHECK(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0 && calls==before+2);CloseHandle(worker);
    CHECK(!gate.scope && !scope.owner);
}
static void test_waits_for_inflight(void)
{
    reset_events();command cmd={callback,1,1};
    HANDLE worker=CreateThread(NULL,0,native_call,&cmd,0,NULL);CHECK(worker);
    CHECK(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0);
    ResetEvent(entered);ResetEvent(attempted);ResetEvent(finished);
    HANDLE publisher=CreateThread(NULL,0,publication,NULL,0,NULL);CHECK(publisher);
    CHECK(WaitForSingleObject(attempted,5000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(entered,30)==WAIT_TIMEOUT);
    SetEvent(allowed);
    CHECK(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0);CloseHandle(worker);
    CHECK(WaitForSingleObject(publisher,5000)==WAIT_OBJECT_0);CloseHandle(publisher);
    CHECK(WaitForSingleObject(entered,0)==WAIT_OBJECT_0 && !gate.scope);
}
static void test_ownership_and_exceptions(void)
{
    char error[256];command cmd={callback,31,3};
    CHECK(gate.entry(manager,&cmd)==31); /* Callback cannot acquire publication. */
    CHECK(sh_audio_bank_gate_begin(&gate,&scope,error,sizeof(error)));
    sh_audio_bank_scope other={0};CHECK(!sh_audio_bank_gate_begin(&gate,&other,error,sizeof(error)));
    CHECK(!sh_audio_bank_gate_bind(&gate,NULL,NULL,0,NULL) && gate.scope==&scope);
    HANDLE thread=CreateThread(NULL,0,wrong_thread,NULL,0,NULL);CHECK(thread);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    cmd.mode=0;CHECK(gate.entry(manager,&cmd)==31 && scope.intervened);
    CHECK(!sh_audio_bank_gate_ready(&scope,error,sizeof(error)));
    CHECK(!sh_audio_bank_gate_end(&scope,error,sizeof(error)) && !scope.owner && !gate.scope);
    CHECK(sh_audio_bank_gate_begin(&gate,&scope,error,sizeof(error)));
    void *saved=manager;manager=NULL;
    CHECK(!sh_audio_bank_gate_end(&scope,error,sizeof(error)) && !scope.owner);manager=saved;
    EnterCriticalSection(&render);
    CHECK(!sh_audio_bank_gate_begin(&gate,&scope,error,sizeof(error)) && !scope.owner);
    LeaveCriticalSection(&render);
    int caught=0;cmd.mode=2;
    __try {gate.entry(manager,&cmd);} __except(EXCEPTION_EXECUTE_HANDLER) {caught=1;}
    CHECK(caught && !gate.dispatch_depth);
    CHECK(sh_audio_bank_gate_begin(&gate,&scope,error,sizeof(error)));
    CHECK(sh_audio_bank_gate_end(&scope,error,sizeof(error)));
}
static int setup(void)
{
    image=VirtualAlloc(NULL,0x4000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(image);if(!image)return 0;
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)image;dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x100;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(image+0x100);nt->Signature=IMAGE_NT_SIGNATURE;
    nt->FileHeader.NumberOfSections=2;nt->FileHeader.SizeOfOptionalHeader=sizeof(nt->OptionalHeader);
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=0x4000;
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    sections[0].VirtualAddress=0x1000;sections[0].Misc.VirtualSize=0x1000;sections[0].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE;
    sections[1].VirtualAddress=0x3000;sections[1].Misc.VirtualSize=0x1000;sections[1].Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    /* Execute the actual hook/trampoline with the proven native prologue and
     * a scratch body that invokes the command's callback. No engine runs here. */
    const unsigned char code[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x7c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xa9,
        0x48,0x83,0xec,0x20,0xff,0x12,0x48,0x83,0xc4,0x20,0x5d,0xc3};
    memcpy(image+0x1000,code,sizeof(code));
    /* The scratch callback can throw. Describe its stack allocation and saved
     * RBP so Windows can unwind through the executed trampoline into the gate's
     * finally block. The engine supplies its own image unwind metadata. */
    const unsigned char unwind[]={1,20,2,0,20,0x32,11,0x50};
    memcpy(image+0x2000,unwind,sizeof(unwind));
    runtime_function=(RUNTIME_FUNCTION){0x1000,0x1000+sizeof(code),0x2000};
    function_table=RtlAddFunctionTable(&runtime_function,1,(DWORD64)image);CHECK(function_table);
    sh_audio_limits_native limits={0};limits.manager_slot=(void **)(image+0x3000);limits.render_lock=(CRITICAL_SECTION *)(image+0x3080);
    sig_result signatures[2]={{"AudioBankDispatch",SIG_OK,(uintptr_t)(image+0x1000),0x1000}};
    CHECK(sh_audio_bank_gate_bind(&gate,image,signatures,1,&limits));
    signatures[1]=signatures[0];CHECK(!sh_audio_bank_gate_bind(&gate,image,signatures,2,&limits));
    signatures[0].status=SIG_OK_HOOKED;CHECK(!sh_audio_bank_gate_bind(&gate,image,signatures,1,&limits));signatures[0].status=SIG_OK;
    image[0x100b]=0;CHECK(!sh_audio_bank_gate_bind(&gate,image,signatures,1,&limits));image[0x100b]=0x48;
    sections[0].Characteristics|=IMAGE_SCN_MEM_WRITE;CHECK(!sh_audio_bank_gate_bind(&gate,image,signatures,1,&limits));
    sections[0].Characteristics&=~IMAGE_SCN_MEM_WRITE;CHECK(sh_audio_bank_gate_bind(&gate,image,signatures,1,&limits));
    /* For concurrency fixtures use ordinary process storage after validating
     * the mapped binding, so manager disappearance is explicit. */
    manager=&gate;gate.manager_slot=&manager;gate.render_lock=&render;
    CHECK(sh_audio_bank_gate_initialize(&gate));
    DWORD old;CHECK(VirtualProtect(image+0x1000,0x1000,PAGE_EXECUTE_READ,&old));
    FlushInstructionCache(GetCurrentProcess(),image+0x1000,sizeof(code));
    trampoline=hook_prepare((void *)gate.entry,detour,16);CHECK(trampoline);if(!trampoline)return 0;
    gate.original=(sh_audio_bank_dispatch_fn)trampoline;
    CHECK(hook_commit(trampoline)==B2_PATCH_OK);gate.installed=hook_is_installed(trampoline);
    return gate.installed;
}
int main(void)
{
    InitializeCriticalSection(&render);
    entered=CreateEventA(NULL,TRUE,FALSE,NULL);allowed=CreateEventA(NULL,TRUE,FALSE,NULL);
    attempted=CreateEventA(NULL,TRUE,FALSE,NULL);finished=CreateEventA(NULL,TRUE,FALSE,NULL);
    if(setup()){test_native_hook();test_waits_for_inflight();test_ownership_and_exceptions();}
    CHECK(!gate.scope && !gate.dispatch_depth);
    if(trampoline)CHECK(hook_unpatch(trampoline));
    if(gate.initialized)DeleteCriticalSection(&gate.lock);
    if(function_table)CHECK(RtlDeleteFunctionTable(&runtime_function));
    if(image)VirtualFree(image,0,MEM_RELEASE);
    DeleteCriticalSection(&render);CloseHandle(entered);CloseHandle(allowed);CloseHandle(attempted);CloseHandle(finished);
    printf("audio_bank_gate_test: %s\n",failures ? "FAIL" : "PASS");return !!failures;
}
