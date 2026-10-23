/* Private RenderProg sampler-load instrumentation. Install only while native
 * execution is quiescent. The accepted windows have no RSP, relative,
 * control-flow or extended-vector operand. The flag TEST uses native RBP,
 * which the bridge preserves. Their original EH state is pinned
 * by the caller. Real CALL/RET preserves the original native return state. */
#ifndef SH_RESOURCE_RENDERPROG_READ_SITE_H
#define SH_RESOURCE_RENDERPROG_READ_SITE_H
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "patch.h"

typedef struct rp_read_site {
    unsigned char *code,*target;size_t length;
    unsigned char original[11],replacement[11];
    RUNTIME_FUNCTION functions[4];int registered;
    size_t flags_push,body,stack_restored,code_length;
    sh_patch_handle patch;
} rp_read_site;
static const unsigned char rp_sampler_compare[]={0x0f,0xbf,0x0c,0x48,0x41,0x3b,0x4f,0x68};
static const unsigned char rp_sampler_append[]={0x41,0x8b,0x77,0x68,0x49,0x8d,0x9e,0xb8,0x06,0,0};
static const unsigned char rp_parm_name[]={0x48,0x8b,0x50,0x08,0x49,0x8b,0x4d,0x08};
static const unsigned char rp_flag_test[]={0x48,0x8b,0x48,0x58,0x48,0x85,0x8d,0xf0,0x54,0,0};
static unsigned char *rp_site_allocate(uintptr_t target)
{
    SYSTEM_INFO info;GetSystemInfo(&info);
    uintptr_t gran=info.dwAllocationGranularity;
    uintptr_t low=target>INT32_MAX?target-INT32_MAX:gran;
    uintptr_t high=target<UINTPTR_MAX-INT32_MAX?target+INT32_MAX:UINTPTR_MAX;
    uintptr_t at=low-low%gran;
    while(at<high) {
        MEMORY_BASIC_INFORMATION region;
        if(!VirtualQuery((void *)at,&region,sizeof region))break;
        uintptr_t end=(uintptr_t)region.BaseAddress+region.RegionSize;
        if(end<=at)break;
        if(region.State==MEM_FREE) {
            uintptr_t start=at<low?low:at;start+=((gran-start%gran)%gran);
            if(start>=low && start<high && start<end && end-start>=4096) {
                unsigned char *p=VirtualAlloc((void *)start,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
                if(p)return p;
            }
        }
        at=end;
    }
    return NULL;
}
static int rp_site_release(rp_read_site *s)
{
    if(s->patch.live && code_unpatch(&s->patch)!=B2_PATCH_OK)return 0;
    if(s->registered) {
        if(!RtlDeleteFunctionTable(s->functions))return 0;
        s->registered=0;
    }
    if(s->code && !VirtualFree(s->code,0,MEM_RELEASE))return 0;
    memset(s,0,sizeof *s);return 1;
}
static int rp_site_prepare(rp_read_site *s,unsigned char *target,const unsigned char *expected,
                           size_t length,unsigned argument_register,void (*observe)(void *))
{
    if(!s || s->code || s->patch.live || !target || !expected || !observe ||
       !((argument_register==15 && ((length==sizeof rp_sampler_compare && !memcmp(expected,rp_sampler_compare,length)) ||
         (length==sizeof rp_sampler_append && !memcmp(expected,rp_sampler_append,length)))) ||
         (argument_register==0 && ((length==sizeof rp_parm_name && !memcmp(expected,rp_parm_name,length)) ||
          (length==sizeof rp_flag_test && !memcmp(expected,rp_flag_test,length))))))return 0;
    __try{if(memcmp(target,expected,length))return 0;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    unsigned char *code=rp_site_allocate((uintptr_t)target+5);if(!code)return 0;
    s->code=code;s->target=target;s->length=length;memcpy(s->original,expected,length);
    size_t p=0;
#define RP_EMIT(...) do{static const unsigned char b[]=__VA_ARGS__;memcpy(code+p,b,sizeof b);p+=sizeof b;}while(0)
    /* PUSHFQ plus seven volatile GPRs, then 552 bytes: aligned shadow space,
     * a complete 512-byte FXSAVE64 image and the saved GPR/flags block. */
    RP_EMIT({0x9c,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53});
    RP_EMIT({0x48,0x81,0xec,0x28,0x02,0,0});
    RP_EMIT({0x48,0x0f,0xae,0x44,0x24,0x20});
    if(argument_register==15){RP_EMIT({0x4c,0x89,0xf9});}
    else {RP_EMIT({0x48,0x89,0xc1});}
    RP_EMIT({0x48,0xb8});
    memcpy(code+p,&observe,8);p+=8;RP_EMIT({0xff,0xd0});
    RP_EMIT({0x48,0x0f,0xae,0x4c,0x24,0x20});
    /* Restore registers without changing RSP: every possible fault in the
     * copied native load retains the single 616-byte allocation. */
    const unsigned char modrm[]={0x9c,0x94,0x8c,0x84,0x94,0x8c,0x84};
    for(size_t i=0;i<7;i++) {
        code[p++]=i<4?0x4c:0x48;code[p++]=0x8b;code[p++]=modrm[i];code[p++]=0x24;
        uint32_t offset=(uint32_t)(0x228+i*8);memcpy(code+p,&offset,4);p+=4;
    }
    s->flags_push=p;RP_EMIT({0xff,0xb4,0x24,0x60,0x02,0,0,0x9d});
    s->body=p;memcpy(code+p,expected,length);p+=length;
    RP_EMIT({0x48,0x8d,0xa4,0x24,0x68,0x02,0,0});
    s->stack_restored=p;code[p++]=0xc3;s->code_length=p;
#undef RP_EMIT
    /* Phase boundaries avoid applying frame allocation after POPFQ/LEA RSP.
     * UWOP_ALLOC_LARGE describes the eight volatile pushes as stack storage;
     * no nonvolatile register is borrowed by the instrumentation. */
    static const unsigned char main_info[]={1,19,10,0,19,1,69,0,12,2,10,2,8,2,6,2,4,2,3,2,2,2,1,2};
    static const unsigned char flags_info[]={1,7,3,0,7,2,0,1,77,0,0,0};
    static const unsigned char body_info[]={1,0,2,0,0,1,77,0};
    static const unsigned char ret_info[]={1,0,0,0};
    memcpy(code+512,main_info,sizeof main_info);memcpy(code+544,flags_info,sizeof flags_info);
    memcpy(code+560,body_info,sizeof body_info);memcpy(code+576,ret_info,sizeof ret_info);
    s->functions[0]=(RUNTIME_FUNCTION){0,(DWORD)s->flags_push,512};
    s->functions[1]=(RUNTIME_FUNCTION){(DWORD)s->flags_push,(DWORD)s->body,544};
    s->functions[2]=(RUNTIME_FUNCTION){(DWORD)s->body,(DWORD)s->stack_restored,560};
    s->functions[3]=(RUNTIME_FUNCTION){(DWORD)s->stack_restored,(DWORD)p,576};
    if(!RtlAddFunctionTable(s->functions,4,(DWORD64)(uintptr_t)code)){rp_site_release(s);return 0;}
    s->registered=1;
    int64_t relative=(int64_t)(uintptr_t)code-(int64_t)((uintptr_t)target+5);
    if(relative<INT32_MIN || relative>INT32_MAX){rp_site_release(s);return 0;}
    memset(s->replacement,0x90,length);s->replacement[0]=0xe8;
    int32_t rel=(int32_t)relative;memcpy(s->replacement+1,&rel,4);
    if(!FlushInstructionCache(GetCurrentProcess(),code,4096)){rp_site_release(s);return 0;}
    return 1;
}
static int rp_site_commit(rp_read_site *s)
{
    return s && s->code && s->registered && !s->patch.live &&
        code_patch(s->target,s->original,s->replacement,s->length,&s->patch)==B2_PATCH_OK;
}
static int rp_site_ready(const rp_read_site *s)
{
    if(!s || !s->code || !s->registered || !s->patch.live)return 0;
    __try{return !memcmp(s->target,s->replacement,s->length);}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
#endif
