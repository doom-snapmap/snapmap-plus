#include "native_pin_hash.h"
#include <stdint.h>
#include <string.h>
#include "decl_renderprog_release_reads.h"
#include "decl_default_allocator.h"
#ifndef SH_RENDERPROG_RELEASE_EXPORT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static uintptr_t rpr_export(int renderer,int resolver)
{
    DWORD error=GetLastError();uintptr_t result=0;
    HMODULE module=GetModuleHandleW(renderer?L"vulkan-1.dll":L"opengl32.dll");
    if(module) {
        if(renderer)result=(uintptr_t)GetProcAddress(module,"vkDestroyShaderModule");
        else {
            PROC (WINAPI *lookup)(LPCSTR)=(PROC (WINAPI *)(LPCSTR))GetProcAddress(module,"wglGetProcAddress");
            if(resolver)result=(uintptr_t)lookup;
            else if(lookup)result=(uintptr_t)lookup("glDeleteProgram");
        }
    }
    SetLastError(error);
    return result>4 && result!=UINTPTR_MAX?result:0;
}
#define SH_RENDERPROG_RELEASE_EXPORT(renderer,resolver) rpr_export(renderer,resolver)
#endif
typedef struct rpr_pin {int renderer;uint32_t rva;size_t length;const unsigned char *hash;} rpr_pin;
#include "decl_renderprog_release_read_pins.h"
typedef struct rpr_probe {sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in;} rpr_probe;
static int rpr_fail(rpr_probe *p,sh_resource_default_read_reason reason,uintptr_t at)
{p->r->reason=reason;p->r->offending_address=at;return 0;}
static int rpr_read(rpr_probe *p,uintptr_t at,size_t offset,void *out,size_t length)
{
    if(!at || offset>UINTPTR_MAX-at || length>UINTPTR_MAX-(at+offset))return rpr_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->r->work==SIZE_MAX || (p->in->work_limit && p->r->work>=p->in->work_limit))return rpr_fail(p,SH_DEFAULT_READ_BUDGET,at+offset);
    p->r->work++;
    return p->in->read(p->in->read_context,at+offset,out,length) || rpr_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
}
static int rpr_data(rpr_probe *p,uint32_t rva,void *out,size_t length)
{
    if(rva>p->in->image_size || length>p->in->image_size-rva)return rpr_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    return rpr_read(p,p->in->image_base,rva,out,length);
}
static int rpr_same(rpr_probe *p,uintptr_t at,const void *before,size_t length)
{
    unsigned char bytes[0x20];
    if(length>sizeof bytes)return rpr_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    return rpr_read(p,at,0,bytes,length) && (!memcmp(bytes,before,length) || rpr_fail(p,SH_DEFAULT_READ_INVOCATION,at));
}
static int rpr_extent(rpr_probe *p,uintptr_t at,size_t length)
{
    unsigned char byte;
    if(!length)return 1;
    if(length>UINTPTR_MAX-at)return rpr_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    return rpr_read(p,at,0,&byte,1) && rpr_read(p,at,length-1,&byte,1);
}
static int rpr_pin_check(rpr_probe *p,unsigned index)
{
    sh_native_pin_hash hash;unsigned char digest[32];
    sh_native_pin_hash_init(&hash);
    const rpr_pin *pin=rpr_pins+index;unsigned char bytes[256];
    for(size_t off=0;off<pin->length;) {
        size_t n=pin->length-off;if(n>sizeof bytes)n=sizeof bytes;
        if(off>UINT32_MAX-pin->rva || !rpr_data(p,pin->rva+(uint32_t)off,bytes,n))return 0;
        sh_native_pin_hash_update(&hash,bytes,n);
        off+=n;
    }
    sh_native_pin_hash_finish(&hash,digest);
    return !memcmp(digest,pin->hash,sizeof digest) || rpr_fail(p,SH_DEFAULT_READ_PIN,p->in->image_base+pin->rva);
}
static int rpr_numeric(rpr_probe *p,uintptr_t header)
{
    unsigned char raw[16];uintptr_t data;int capacity;
    if(!rpr_read(p,header,0,raw,sizeof raw))return 0;
    memcpy(&data,raw,8);memcpy(&capacity,raw+12,4);
    /* These are native memset destinations. COUNT/storage flags are not read;
     * borrowed and inline backing is legal. Signed CAPACITY determines bytes. */
    if(capacity<0)return rpr_fail(p,SH_DEFAULT_READ_BOUNDS,header+12);
    return rpr_extent(p,data,(size_t)capacity*2) && rpr_same(p,header,raw,sizeof raw);
}
static int rpr_string(rpr_probe *p,uintptr_t at)
{
    unsigned char raw[0x20];uintptr_t table,callback,data;uint32_t flags;
    if(!rpr_read(p,at,0,raw,sizeof raw))return 0;
    memcpy(&table,raw,8);memcpy(&data,raw+0x10,8);memcpy(&flags,raw+0x18,4);
    if(!rpr_read(p,table,0,&callback,8))return 0;
    if(callback!=p->in->image_base+rpr_pins[RPR_GL_STRING_DTOR].rva)return rpr_fail(p,SH_DEFAULT_READ_CALLBACK,callback);
    /* The native call supplies deleting flags0. This destructor only frees an
     * owned character allocation; neither length nor string contents is read. */
    if(data && (flags&0x40000000) && !rpr_extent(p,data,1))return 0;
    return rpr_same(p,table,&callback,8) && rpr_same(p,at,raw,sizeof raw);
}
static int rpr_clear(rpr_probe *p,uintptr_t at,int strings)
{
    unsigned char raw[0x18];uintptr_t data;int capacity;
    if(!rpr_read(p,at,0,raw,sizeof raw))return 0;
    memcpy(&data,raw,8);memcpy(&capacity,raw+12,4);
    if(raw[0x13]==0 || raw[0x13]==3 || raw[0x13]==4) {
        if(data) {
            if(strings && capacity>0) {
                if((size_t)capacity>(UINTPTR_MAX-data)/0x30)return rpr_fail(p,SH_DEFAULT_READ_BOUNDS,data);
                for(int i=0;i<capacity;i++)if(!rpr_string(p,data+(size_t)i*0x30))return 0;
            }
            if(!rpr_extent(p,data,1))return 0;
        }
    }
    /* Non-owning flags retain the pointer and do not dispatch element dtors.
     * COUNT is reset unconditionally, including when no record is destroyed. */
    return rpr_same(p,at,raw,sizeof raw);
}
static int rpr_gl_storage(rpr_probe *p)
{
    uintptr_t ctx=p->in->resource+0x78;unsigned cpu_features;
    if(!rpr_extent(p,p->in->resource,0x1870) || !sh_decl_default_allocator_read_check(p->r,p->in))return 0;
    if(!rpr_data(p,0x59e1674,&cpu_features,4))return 0;
    if(!rpr_clear(p,ctx+0x70,0) || !rpr_clear(p,ctx+0x88,0))return 0;
    for(unsigned i=0;i<3;i++) {
        if(!rpr_numeric(p,ctx+0xa0+i*0x1b0) || !rpr_numeric(p,ctx+0x178+i*0x1b0) ||
           !rpr_numeric(p,ctx+0x5b0+i*0x58) || !rpr_numeric(p,ctx+0x6b8+i*0x58) ||
           !rpr_numeric(p,ctx+0x7c0+i*0x28) || !rpr_numeric(p,ctx+0x838+i*0x28) ||
           !rpr_numeric(p,ctx+0x8b0+i*0x28) || !rpr_numeric(p,ctx+0x928+i*0x38))return 0;
    }
    return rpr_clear(p,ctx+0x38,1) && rpr_clear(p,ctx+0x50,0) &&
           rpr_clear(p,ctx+0x9d0,1) && rpr_clear(p,ctx+0x15e8,0);
}
int sh_decl_renderprog_release_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    rpr_probe p={r,in};uintptr_t table,release;uint64_t handles[3];
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->image_base ||
       !in->image_size || in->image_size>UINTPTR_MAX-in->image_base ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context || r->invocation.work_limit!=in->work_limit)
        return rpr_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    if(!rpr_read(&p,in->resource,0,&table,8) || !rpr_read(&p,table,0xa0,&release,8))return 0;
    unsigned index=in->renderer?RPR_VK_RELEASE:RPR_GL_RELEASE;
    if(release!=in->image_base+rpr_pins[index].rva)return rpr_fail(&p,SH_DEFAULT_READ_CALLBACK,release);
    for(unsigned i=0;i<RPR_PINS;i++)if(rpr_pins[i].renderer==in->renderer && !rpr_pin_check(&p,i))return 0;
    if(!rpr_read(&p,in->resource,0x1840,handles,sizeof handles))return 0;
    int used=in->renderer?(handles[0] || handles[1] || handles[2]):(handles[0] || handles[2]);
    if(used) {
        uintptr_t dispatch,expected=SH_RENDERPROG_RELEASE_EXPORT(in->renderer,0);
        uint32_t slot=in->renderer?0x1fb6c10:0x59d1c60;
        if(!rpr_data(&p,slot,&dispatch,8))return 0;
        if(!expected || dispatch!=expected)return rpr_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+slot);
        if(in->renderer) {
            uintptr_t device;
            if(!rpr_data(&p,0x583f4a8,&device,8))return 0;
            if(!device)return rpr_fail(&p,SH_DEFAULT_READ_OWNERSHIP,in->image_base+0x583f4a8);
            if(!rpr_same(&p,in->image_base+0x583f4a8,&device,8))return 0;
        } else {
            uintptr_t resolver;expected=SH_RENDERPROG_RELEASE_EXPORT(0,1);
            if(!rpr_data(&p,0x1fcdcd8,&resolver,8))return 0;
            if(!expected || resolver!=expected)return rpr_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+0x1fcdcd8);
            if(!rpr_same(&p,in->image_base+0x1fcdcd8,&resolver,8))return 0;
        }
        if(!rpr_same(&p,in->image_base+slot,&dispatch,8))return 0;
    }
    if(!in->renderer && !rpr_gl_storage(&p))return 0;
    if(in->renderer) {unsigned short status;if(!rpr_data(&p,0x583fbbc,&status,2))return 0;}
    return rpr_same(&p,in->resource+0x1840,handles,sizeof handles) &&
           rpr_same(&p,table+0xa0,&release,8) && rpr_same(&p,in->resource,&table,8);
}
