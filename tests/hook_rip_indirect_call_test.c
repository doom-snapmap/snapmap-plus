/* Dynamic imported-call relocation and native unwind on isolated code. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
static int fail_add,fail_delete,adds,deletes;
static BOOLEAN NTAPI test_add(PRUNTIME_FUNCTION table,DWORD count,DWORD64 base)
{adds++;return fail_add?FALSE:RtlAddFunctionTable(table,count,base);}
static BOOLEAN NTAPI test_delete(PRUNTIME_FUNCTION table)
{deletes++;return fail_delete?FALSE:RtlDeleteFunctionTable(table);}
#define RtlAddFunctionTable test_add
#define RtlDeleteFunctionTable test_delete
#include "../src/backend/hook.c"
#undef RtlAddFunctionTable
#undef RtlDeleteFunctionTable
void backend_log(const char *text){(void)text;}
#include "../src/backend/patch.c"
static int failures,callee_cleanup,detour_cleanup;
#define CHECK(x) do{if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);failures++;}}while(0)
typedef struct capture {uint64_t rax,rdx,r8,r9,stack,flags,fifth,sixth;unsigned char xmm[4][16];} capture;
typedef uint64_t (*fixture_fn)(capture *,...);
static fixture_fn original;
static uint64_t detour(capture *out,...)
{
    va_list args;va_start(args,out);double x=va_arg(args,double),y=va_arg(args,double),z=va_arg(args,double);
    uint64_t fifth=va_arg(args,uint64_t),sixth=va_arg(args,uint64_t),result=0;va_end(args);
    __try{result=original(out,x,y,z,fifth,sixth);}
    __finally{detour_cleanup++;}
    return result+17;
}
static uint64_t throwing(capture *out,...)
{
    (void)out;
    __try{RaiseException(0xe0326001,0,0,NULL);}
    __finally{callee_cleanup++;}
    return 0;
}
static uint64_t invoke(fixture_fn fn,capture *out)
{return fn(out,1.25,-2.5,3.75,UINT64_C(0x9988776655443322),UINT64_C(0xabcdef0123456789));}
static int catches(fixture_fn fn)
{
    capture out={0};int caught=0;
    __try{invoke(fn,&out);}
    __except(GetExceptionCode()==0xe0326001?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
    return caught;
}
static void unwind_continuation(fixture_fn fn)
{
    DWORD64 base=0,establisher=0;PVOID data=NULL;
    PRUNTIME_FUNCTION entry=RtlLookupFunctionEntry((DWORD64)(uintptr_t)fn+46,&base,NULL);
    CHECK(entry);if(!entry)return;
    CONTEXT context={0};uint64_t stack[32]={0};
    context.Rip=(DWORD64)(uintptr_t)fn+46;context.Rsp=(DWORD64)(uintptr_t)stack;
    stack[0]=UINT64_C(0x1111111122222222);stack[11]=UINT64_C(0x3333333344444444);
    RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,context.Rip,entry,&context,&data,&establisher,NULL);
    CHECK(context.Rip==stack[11] && context.Rsp==(DWORD64)(uintptr_t)stack+0x60);
}
static uint64_t bits(double value){uint64_t out;memcpy(&out,&value,8);return out;}
static void values(const capture *out)
{
    CHECK(out->rax==UINT64_C(0x1122334455667788));
    CHECK(out->rdx==bits(1.25) && out->r8==bits(-2.5) && out->r9==bits(3.75));
    CHECK((out->stack&15)==8);
    CHECK(out->fifth==UINT64_C(0x9988776655443322) && out->sixth==UINT64_C(0xabcdef0123456789));
    uint64_t x,y,z;memcpy(&x,out->xmm[1],8);memcpy(&y,out->xmm[2],8);memcpy(&z,out->xmm[3],8);
    CHECK(x==bits(1.25) && y==bits(-2.5) && z==bits(3.75));
}
static void fixture_case(void)
{
    unsigned char *code=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);CHECK(code);if(!code)return;
    /* The sole allocating prologue is SUB RSP,58. Copy native stack arguments
     * through volatile RAX, then seed RAX before the imported call. No other
     * stack/nonvolatile change occurs in the stolen46-byte source window. */
    static const unsigned char source[]={
        0x48,0x83,0xec,0x58,
        0x48,0x8b,0x84,0x24,0x80,0,0,0,0x48,0x89,0x44,0x24,0x20,
        0x48,0x8b,0x84,0x24,0x88,0,0,0,0x48,0x89,0x44,0x24,0x28,
        0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
        0xff,0x15,0,0,0,0,0x48,0x85,0xc0,0x0f,0x95,0xc0,
        0x48,0x83,0xc4,0x58,0xc3};
    memcpy(code,source,sizeof source);int32_t relative=256-46;memcpy(code+42,&relative,4);
    static const unsigned char callee[]={
        0x48,0x89,0x01,0x48,0x89,0x51,0x08,0x4c,0x89,0x41,0x10,0x4c,0x89,0x49,0x18,
        0x49,0x89,0xe2,0x4c,0x89,0x51,0x20,0x9c,0x41,0x5a,0x4c,0x89,0x51,0x28,
        0x4c,0x8b,0x54,0x24,0x28,0x4c,0x89,0x51,0x30,
        0x4c,0x8b,0x54,0x24,0x30,0x4c,0x89,0x51,0x38,
        0xf3,0x0f,0x7f,0x41,0x40,0xf3,0x0f,0x7f,0x49,0x50,
        0xf3,0x0f,0x7f,0x51,0x60,0xf3,0x0f,0x7f,0x59,0x70,
        0xb8,1,0,0,0,0xc3};
    memcpy(code+512,callee,sizeof callee);void **slot=(void **)(code+256);*slot=code+512;
    const unsigned char zero[]={0x33,0xc0,0xc3};memcpy(code+768,zero,sizeof zero);
    /* Version1/flags0, one ALLOC_SMALL(58) at offset4, no frame or handler. */
    const unsigned char unwind[]={1,4,1,0,4,0xa2,0,0};memcpy(code+128,unwind,8);
    RUNTIME_FUNCTION source_table={0,sizeof source,128};
    CHECK(RtlAddFunctionTable(&source_table,1,(DWORD64)(uintptr_t)code));
    CHECK(FlushInstructionCache(GetCurrentProcess(),code,4096));
    capture before={0},after={0};CHECK(invoke((fixture_fn)code,&before)==1);values(&before);
    *slot=throwing;CHECK(catches((fixture_fn)code) && callee_cleanup==1);*slot=code+512;
    size_t baseline=hook_owned_count();
    CHECK(!hook_prepare_rip_indirect_call(code,detour,46,39));
    code[128]=9;CHECK(!hook_prepare_rip_indirect_call(code,detour,46,40));code[128]=1;
    code[133]=0x92;CHECK(!hook_prepare_rip_indirect_call(code,detour,46,40));code[133]=0xa2;
    code[56]=0x90;CHECK(!hook_prepare_rip_indirect_call(code,detour,46,40));code[56]=0xc3;
    fail_add=1;CHECK(!hook_prepare_rip_indirect_call(code,detour,46,40));fail_add=0;CHECK(hook_owned_count()==baseline);
    original=(fixture_fn)hook_prepare_rip_indirect_call(code,detour,46,40);CHECK(original);if(!original)goto done;
    CHECK(invoke(original,&after)==1);values(&after);CHECK(before.flags==after.flags);
    DWORD64 lookup_base=0;PRUNTIME_FUNCTION entry=RtlLookupFunctionEntry((DWORD64)(uintptr_t)original,&lookup_base,NULL);
    CHECK(entry && lookup_base==(DWORD64)(uintptr_t)original);
    unwind_continuation(original);
    *slot=code+768;CHECK(invoke(original,&after)==0);*slot=code+512;CHECK(invoke(original,&after)==1);
    CHECK(hook_commit((void *)original)==B2_PATCH_OK);CHECK(invoke((fixture_fn)code,&after)==18);values(&after);
    *slot=throwing;CHECK(catches(original) && callee_cleanup==2);
    CHECK(catches((fixture_fn)code) && callee_cleanup==3 && detour_cleanup==2);
    *slot=code+512;CHECK(invoke((fixture_fn)code,&after)==18);
    fail_delete=1;CHECK(!hook_unpatch((void *)original));fail_delete=0;
    CHECK(hook_owned_count()==baseline+1 && !memcmp(code,source,40));
    CHECK(RtlLookupFunctionEntry((DWORD64)(uintptr_t)original,&lookup_base,NULL)!=NULL);
    CHECK(hook_unpatch((void *)original));CHECK(hook_owned_count()==baseline);
    CHECK(!RtlLookupFunctionEntry((DWORD64)(uintptr_t)original,&lookup_base,NULL));original=NULL;
    CHECK(invoke((fixture_fn)code,&after)==1);values(&after);
done:
    CHECK(RtlDeleteFunctionTable(&source_table));CHECK(VirtualFree(code,0,MEM_RELEASE));
}
int main(void)
{fixture_case();printf("hook_rip_indirect_call_test: %d failure(s)\n",failures);return failures?1:0;}
