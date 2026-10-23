/* Execute the private sampler callsite bridge and its registered unwind. */
#define WIN32_LEAN_AND_MEAN
#define SH_PATCH_TESTING
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
static int fail_add,fail_delete,adds,deletes;
static BOOLEAN NTAPI site_test_add(PRUNTIME_FUNCTION t,DWORD n,DWORD64 b)
{adds++;return fail_add?FALSE:RtlAddFunctionTable(t,n,b);}
static BOOLEAN NTAPI site_test_delete(PRUNTIME_FUNCTION t)
{deletes++;return fail_delete?FALSE:RtlDeleteFunctionTable(t);}
#define RtlAddFunctionTable site_test_add
#define RtlDeleteFunctionTable site_test_delete
#include "../src/backend/resource_renderprog_read_site.h"
#undef RtlAddFunctionTable
#undef RtlDeleteFunctionTable
void backend_log(const char *s){(void)s;}
#include "../src/backend/hook.c"
#include "../src/backend/patch.c"
static int failures,calls,raise_callback,callback_finally,caller_finally;
static void *last_sampler;
#define CHECK(x) do{if(!(x)){printf("FAIL %d: %s\n",__LINE__,#x);failures++;}}while(0)
typedef struct site_capture {
    void *sampler,*array;uint64_t index,r14,r12,mask;
    uint64_t rax,rcx,rdx,r8,r9,r10,r11,rsi,rbx,r15,flags;
    unsigned char xmm[6][16];uint32_t mxcsr;
} site_capture;
typedef void (*site_fixture)(site_capture *);
static void observer(void *sampler)
{
    DWORD error=GetLastError();last_sampler=sampler;calls++;
    __try {
        volatile double x=1.25,y=3.5,z=x/y;(void)z;
        _mm_setcsr(_mm_getcsr()^0x6000);
        if(raise_callback)RaiseException(0xe0328001,0,0,NULL);
    } __finally {callback_finally++;SetLastError(error);}
}
static int caught(site_fixture fn,site_capture *capture,DWORD expected)
{
    int result=0;
    __try {
        __try{fn(capture);}
        __finally{caller_finally++;}
    } __except(GetExceptionCode()==expected?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){result=1;}
    return result;
}
static void emit(unsigned char *code,size_t *p,const void *bytes,size_t size)
{memcpy(code+*p,bytes,size);*p+=size;}
#define EMIT(...) do{static const unsigned char b[]=__VA_ARGS__;emit(code,&p,b,sizeof b);}while(0)
static size_t fixture(unsigned char *code,int mode,size_t *at)
{
    size_t p=0;
    EMIT({0x41,0x57,0x56,0x53,0x41,0x56,0x41,0x54,0x41,0x55,0x55,0x48,0x83,0xec,0x20});
    EMIT({0x48,0x89,0xca,0x4c,0x8b,0x39,0x48,0x8b,0x41,0x08,
          0x4c,0x8b,0x71,0x18,0x4c,0x8b,0xe1,0x4d,0x8b,0xef});
    EMIT({0xbe,0x11,0x22,0x33,0x44,0xbb,0x55,0x66,0x77,0x88});
    /* Native RBP addresses the mask while the private bridge borrows RSP. */
    EMIT({0x48,0x8d,0xa9});int32_t frame=(int32_t)offsetof(site_capture,mask)-0x54f0;emit(code,&p,&frame,4);
    /* Seed all volatile GPRs other than the native array/index/record inputs. */
    for(unsigned reg=0;reg<4;reg++) {
        unsigned char op[]={0x49,(unsigned char)(0xb8+reg)};emit(code,&p,op,2);
        uint64_t value=UINT64_C(0x0123456789abcdef)+reg;emit(code,&p,&value,8);
    }
    for(unsigned reg=0;reg<6;reg++) {
        unsigned char op[]={0x66,0x0f,0xef,(unsigned char)(0xc0+reg*9)};emit(code,&p,op,4);
    }
    EMIT({0x31,0xc9,0xf9,0x48,0x8b,0x4a,0x10}); /* stable flags; native index */
    *at=p;emit(code,&p,mode==3?rp_flag_test:mode==2?rp_parm_name:mode?rp_sampler_append:rp_sampler_compare,(mode==1 || mode==3)?11:8);
    /* The name span intentionally changes RDX. Record that native value via
     * preserved R12, then use RDX as the output base for the other fields. */
    EMIT({0x49,0x89,0x94,0x24});uint32_t off=offsetof(site_capture,rdx);emit(code,&p,&off,4);
    EMIT({0x4c,0x89,0xe2});
    /* PUSHFQ/POP memory preserves all GPRs while capturing native flags. */
    EMIT({0x9c,0x8f,0x82});off=offsetof(site_capture,flags);emit(code,&p,&off,4);
    const unsigned char rex[]={0x48,0x48,0x48,0x4c,0x4c,0x4c,0x4c,0x48,0x48,0x4c};
    const unsigned char modrm[]={0x82,0x8a,0x92,0x82,0x8a,0x92,0x9a,0xb2,0x9a,0xba};
    for(size_t i=0;i<10;i++) {
        if(i==2)continue;
        unsigned char op[]={rex[i],0x89,modrm[i]};emit(code,&p,op,3);
        off=(uint32_t)(offsetof(site_capture,rax)+i*8);emit(code,&p,&off,4);
    }
    for(unsigned reg=0;reg<6;reg++) {
        unsigned char op[]={0xf3,0x0f,0x7f,(unsigned char)(0x82+reg*8)};emit(code,&p,op,4);
        off=(uint32_t)(offsetof(site_capture,xmm)+reg*16);emit(code,&p,&off,4);
    }
    EMIT({0x0f,0xae,0x9a});off=offsetof(site_capture,mxcsr);emit(code,&p,&off,4);
    EMIT({0x48,0x83,0xc4,0x20,0x5d,0x41,0x5d,0x41,0x5c,0x41,0x5e,0x5b,0x5e,0x41,0x5f,0xc3});
    const unsigned char unwind[]={1,15,8,0,15,0x32,11,0x50,10,0xd0,8,0xc0,6,0xe0,4,0x30,3,0x60,2,0xf0};
    memcpy(code+1024,unwind,sizeof unwind);return p;
}
#undef EMIT
static void virtual_unwind(const rp_read_site *s,size_t pc,size_t allocation)
{
    uint64_t stack[128]={0};CONTEXT c={0};PVOID data=NULL;DWORD64 base=0,frame=0;
    uint64_t return_pc=UINT64_C(0x123456789abcdef0);stack[allocation/8]=return_pc;
    c.Rip=(DWORD64)(uintptr_t)s->code+pc;c.Rsp=(DWORD64)(uintptr_t)stack;
    PRUNTIME_FUNCTION entry=RtlLookupFunctionEntry(c.Rip,&base,NULL);CHECK(entry);if(!entry)return;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,c.Rip,entry,&c,&data,&frame,NULL);
    CHECK(c.Rip==return_pc && c.Rsp==(DWORD64)(uintptr_t)stack+allocation+8);
}
static void one_case(int mode,uint64_t mask)
{
    unsigned char *code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);CHECK(code);if(!code)return;
    size_t at=0,length=fixture(code,mode,&at);
    RUNTIME_FUNCTION source={0,(DWORD)length,1024};CHECK(RtlAddFunctionTable(&source,1,(DWORD64)(uintptr_t)code));
    unsigned char sampler[112]={0};int32_t value=7;memcpy(sampler+0x68,&value,4);int16_t values[]={7,-5};
    site_capture a={sampler,values,0,UINT64_C(0xabc000),UINT64_C(0xdef000),mask},b=a;
    uintptr_t name=(uintptr_t)"parm_name";memcpy(sampler+8,&name,8);
    uint64_t flag=0x20;memcpy(sampler+0x58,&flag,8);
    if(mode>=2)a.array=b.array=sampler;
    DWORD csr=_mm_getcsr();SetLastError(0x4567);((site_fixture)code)(&a);CHECK(!calls);
    CHECK(a.r8==UINT64_C(0x0123456789abcdef) && a.r15==(uintptr_t)sampler);
    CHECK(mode==3?(a.rcx==flag && !!(a.flags&0x40)==!(mask&flag)):mode==2?(a.rcx==name && a.rdx==name):mode?(a.rsi==7 && a.rbx==b.r14+0x6b8):(a.rcx==7 && (a.flags&0x40)));
    rp_read_site site={0};unsigned char *target=code+at;const unsigned char *want=mode==3?rp_flag_test:mode==2?rp_parm_name:mode?rp_sampler_append:rp_sampler_compare;size_t span=(mode==1 || mode==3)?11:8;unsigned argument=mode>=2?0:15;
    CHECK(!rp_site_prepare(&site,target,want,span-1,argument,observer));
    CHECK(!rp_site_prepare(&site,target,want,span,argument==15?0:15,observer));
    target[0]^=1;CHECK(!rp_site_prepare(&site,target,want,span,argument,observer));target[0]^=1;
    fail_add=1;CHECK(!rp_site_prepare(&site,target,want,span,argument,observer) && !site.code);fail_add=0;
    CHECK(rp_site_prepare(&site,target,want,span,argument,observer));CHECK(!rp_site_ready(&site));
    virtual_unwind(&site,0,0);virtual_unwind(&site,1,8);virtual_unwind(&site,2,16);
    virtual_unwind(&site,4,32);virtual_unwind(&site,12,64);virtual_unwind(&site,19,616);
    virtual_unwind(&site,site.flags_push,616);virtual_unwind(&site,site.flags_push+7,624);
    virtual_unwind(&site,site.body,616);virtual_unwind(&site,site.stack_restored-8,616);virtual_unwind(&site,site.stack_restored,0);
    CHECK(rp_site_commit(&site) && rp_site_ready(&site));SetLastError(0x4567);((site_fixture)code)(&b);
    CHECK(calls==1 && last_sampler==sampler && callback_finally==1);
    if(mode!=2)a.rdx=(uintptr_t)&b;
    CHECK(!memcmp((unsigned char *)&a+offsetof(site_capture,rax),(unsigned char *)&b+offsetof(site_capture,rax),sizeof a-offsetof(site_capture,rax)));
    CHECK(GetLastError()==0x4567 && _mm_getcsr()==csr);
    target[span-1]^=1;CHECK(!rp_site_ready(&site));target[span-1]^=1;
    raise_callback=1;CHECK(caught((site_fixture)code,&b,0xe0328001));raise_callback=0;_mm_setcsr(csr);
    CHECK(callback_finally==2 && caller_finally==1);
    if(mode>=2)b.array=NULL;else b.sampler=NULL;
    CHECK(caught((site_fixture)code,&b,EXCEPTION_ACCESS_VIOLATION));CHECK(caller_finally==2);
    fail_delete=1;CHECK(!rp_site_release(&site) && site.code && site.registered && !site.patch.live);fail_delete=0;
    CHECK(!memcmp(target,want,span));CHECK(caught((site_fixture)code,&b,EXCEPTION_ACCESS_VIOLATION));CHECK(caller_finally==3);
    CHECK(rp_site_release(&site) && !site.code);CHECK(RtlDeleteFunctionTable(&source));VirtualFree(code,0,MEM_RELEASE);
    calls=callback_finally=caller_finally=0;
}
typedef const char *(*leaf_name_fn)(void *,int);
static leaf_name_fn leaf_original;static int leaf_finally;
static const char *leaf_detour(void *manager,int index)
{
    const char *result=NULL;
    __try{result=leaf_original(manager,index);}
    __finally{leaf_finally++;}return result;
}
static int leaf_fault(leaf_name_fn fn,void *manager)
{
    int caught=0;
    __try{fn(manager,1);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){caught=1;}
    return caught;
}
static void name_leaf(void)
{
    static const unsigned char native[]={0x48,0x63,0xc2,0x48,0x8b,0x04,0xc1,0x48,0x8b,0x40,0x08,0xc3,0xcc,0xcc,0xcc,0xcc};
    unsigned char *code=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);CHECK(code);if(!code)return;
    memcpy(code,native,sizeof native);const char *name="leaf_name";uintptr_t object[]={0,(uintptr_t)name};void *manager[]={NULL,object};
    CHECK(((leaf_name_fn)code)(manager,1)==name);manager[1]=NULL;CHECK(leaf_fault((leaf_name_fn)code,manager));manager[1]=object;
    leaf_original=(leaf_name_fn)hook_prepare(code,leaf_detour,14);CHECK(leaf_original);if(!leaf_original)return;
    CHECK(leaf_original(manager,1)==name);manager[1]=NULL;CHECK(leaf_fault(leaf_original,manager));manager[1]=object;
    CHECK(hook_commit(leaf_original)==B2_PATCH_OK);CHECK(((leaf_name_fn)code)(manager,1)==name && leaf_finally==1);
    manager[1]=NULL;CHECK(leaf_fault((leaf_name_fn)code,manager) && leaf_finally==2);
    CHECK(hook_unpatch(leaf_original));CHECK(!memcmp(code,native,sizeof native));VirtualFree(code,0,MEM_RELEASE);
}
int main(void)
{
    one_case(0,0);one_case(1,0);one_case(2,0);one_case(3,0);one_case(3,0x20);name_leaf();
    printf("resource_renderprog_read_site_test: %s\n",failures?"FAIL":"PASS");return failures?1:0;
}

