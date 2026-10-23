#include "native_pin_hash.h"
#include <stdint.h>
#include <string.h>
#include "decl_renderprog_backend_reads.h"
#ifndef SH_RENDERPROG_BACKEND_EXPORT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static uintptr_t rpb_export(int renderer,const char *name,int extension)
{
    DWORD error=GetLastError();uintptr_t result=0;
    HMODULE module=GetModuleHandleW(renderer?L"vulkan-1.dll":L"opengl32.dll");
    if(module) {
        if(!extension)result=(uintptr_t)GetProcAddress(module,name);
        else {
            PROC (WINAPI *lookup)(LPCSTR)=(PROC (WINAPI *)(LPCSTR))GetProcAddress(module,"wglGetProcAddress");
            if(lookup)result=(uintptr_t)lookup(name);
        }
    }
    SetLastError(error);
    return result>4 && result!=UINTPTR_MAX?result:0;
}
#define SH_RENDERPROG_BACKEND_EXPORT(renderer,name,extension) rpb_export(renderer,name,extension)
#endif
typedef struct rpb_pin {uint32_t rva;size_t length;const unsigned char *hash;} rpb_pin;
typedef struct rpb_api {uint32_t slot;const char *name;int extension;} rpb_api;
#include "decl_renderprog_backend_read_pins.h"
typedef struct rpb_probe {sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in;} rpb_probe;
static int rpb_fail(rpb_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{p->r->reason=why;p->r->offending_address=at;return 0;}
static int rpb_read(rpb_probe *p,uint32_t rva,void *out,size_t length)
{
    if(rva>p->in->image_size || length>p->in->image_size-rva)return rpb_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    uintptr_t at=p->in->image_base+rva;
    if(p->r->work==SIZE_MAX || (p->in->work_limit && p->r->work>=p->in->work_limit))return rpb_fail(p,SH_DEFAULT_READ_BUDGET,at);
    p->r->work++;
    return p->in->read(p->in->read_context,at,out,length) || rpb_fail(p,SH_DEFAULT_READ_MEMORY,at);
}
static int rpb_pin_check(rpb_probe *p,const rpb_pin *pin)
{
    sh_native_pin_hash hash;unsigned char digest[32];
    sh_native_pin_hash_init(&hash);
    unsigned char bytes[256];
    for(size_t offset=0;offset<pin->length;) {
        size_t count=pin->length-offset;if(count>sizeof bytes)count=sizeof bytes;
        if(offset>UINT32_MAX-pin->rva)return rpb_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
        if(!rpb_read(p,pin->rva+(uint32_t)offset,bytes,count))return 0;
        sh_native_pin_hash_update(&hash,bytes,count);
        offset+=count;
    }
    sh_native_pin_hash_finish(&hash,digest);
    return !memcmp(digest,pin->hash,sizeof digest) || rpb_fail(p,SH_DEFAULT_READ_PIN,p->in->image_base+pin->rva);
}
int sh_decl_renderprog_backend_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    rpb_probe p={r,in};uintptr_t before[RPB_API_COUNT],resolver=0;unsigned char skipped=0;
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->image_base || !in->image_size ||
       in->image_size>UINTPTR_MAX-in->image_base || !in->read || !in->resource || !in->default_callback ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return rpb_fail(&p,SH_DEFAULT_READ_INPUT,0);
    for(unsigned i=0;i<rpb_pin_counts[in->renderer];i++)if(!rpb_pin_check(&p,rpb_pins[in->renderer]+i))return 0;
    /* The native GL backend returns before any API dispatch in this mode. */
    if(!in->renderer) {
        if(!rpb_read(&p,0x56e4a0c,&skipped,1))return 0;
        if(skipped) {
            unsigned char again;
            return rpb_read(&p,0x56e4a0c,&again,1) && (again==skipped || rpb_fail(&p,SH_DEFAULT_READ_INVOCATION,in->image_base+0x56e4a0c));
        }
    }
    int used_extension=0;
    for(unsigned i=0;i<rpb_api_counts[in->renderer];i++) {
        const rpb_api *api=rpb_apis[in->renderer]+i;
        if(!rpb_read(&p,api->slot,before+i,8))return 0;
        /* NULL is no callback edge at entry, not a promise that a later call
         * succeeds. Only the caller's observed native invocation can finish. */
        if(before[i]) {
            uintptr_t expected=SH_RENDERPROG_BACKEND_EXPORT(in->renderer,api->name,api->extension);
            if(!expected || before[i]!=expected)return rpb_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+api->slot);
            used_extension|=api->extension;
        }
    }
    if(used_extension) {
        uintptr_t expected=SH_RENDERPROG_BACKEND_EXPORT(0,"wglGetProcAddress",0);
        if(!rpb_read(&p,0x1fcdcd8,&resolver,8))return 0;
        if(!expected || resolver!=expected)return rpb_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+0x1fcdcd8);
    }
    for(unsigned i=0;i<rpb_api_counts[in->renderer];i++) {
        uintptr_t after;uint32_t slot=rpb_apis[in->renderer][i].slot;
        if(!rpb_read(&p,slot,&after,8))return 0;
        if(after!=before[i])return rpb_fail(&p,SH_DEFAULT_READ_INVOCATION,in->image_base+slot);
    }
    if(used_extension) {
        uintptr_t after;if(!rpb_read(&p,0x1fcdcd8,&after,8))return 0;
        if(after!=resolver)return rpb_fail(&p,SH_DEFAULT_READ_INVOCATION,in->image_base+0x1fcdcd8);
    }
    if(!in->renderer) {
        unsigned char after;if(!rpb_read(&p,0x56e4a0c,&after,1))return 0;
        if(after!=skipped)return rpb_fail(&p,SH_DEFAULT_READ_INVOCATION,in->image_base+0x56e4a0c);
    }
    return 1;
}
