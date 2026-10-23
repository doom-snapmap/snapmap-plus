#include <stdint.h>
#include <string.h>
#include "decl_renderprog_context_reads.h"

typedef struct rpx_probe {sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in;} rpx_probe;
static int rpx_fail(rpx_probe *p,sh_resource_default_read_reason reason,uintptr_t at)
{p->r->reason=reason;p->r->offending_address=at;return 0;}
static int rpx_read(rpx_probe *p,uintptr_t at,size_t off,void *out,size_t size)
{
    if(!at || off>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+off))return rpx_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->r->work==SIZE_MAX || (p->in->work_limit && p->r->work>=p->in->work_limit))return rpx_fail(p,SH_DEFAULT_READ_BUDGET,at+off);
    p->r->work++;
    return p->in->read(p->in->read_context,at+off,out,size) || rpx_fail(p,SH_DEFAULT_READ_MEMORY,at+off);
}
static int rpx_extent(rpx_probe *p,uintptr_t at,size_t size)
{
    unsigned char value;
    if(!size)return 1;
    if(size>UINTPTR_MAX-at)return rpx_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    return rpx_read(p,at,0,&value,1) && rpx_read(p,at,size-1,&value,1);
}
static int rpx_same(rpx_probe *p,uintptr_t at,const void *before,size_t size)
{
    unsigned char after[0x20];
    if(size>sizeof after)return rpx_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    return rpx_read(p,at,0,after,size) && (!memcmp(after,before,size) || rpx_fail(p,SH_DEFAULT_READ_INVOCATION,at));
}
static int rpx_string(rpx_probe *p,uintptr_t at)
{
    static const uint32_t tables[2]={0x27a1480,0x278b270};
    static const uint32_t destructors[2]={0x19eff00,0x19fdca0};
    unsigned char raw[0x20];uintptr_t table,data,callback;int length;uint32_t flags;
    if(tables[p->in->renderer]>p->in->image_size || 8>p->in->image_size-tables[p->in->renderer] ||
       destructors[p->in->renderer]>=p->in->image_size)return rpx_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    if(!rpx_read(p,at,0,raw,sizeof raw))return 0;
    memcpy(&table,raw,8);memcpy(&length,raw+8,4);memcpy(&data,raw+0x10,8);memcpy(&flags,raw+0x18,4);
    if(table!=p->in->image_base+tables[p->in->renderer])return rpx_fail(p,SH_DEFAULT_READ_CALLBACK,table);
    if(!rpx_read(p,table,0,&callback,8))return 0;
    if(callback!=p->in->image_base+destructors[p->in->renderer])return rpx_fail(p,SH_DEFAULT_READ_CALLBACK,callback);
    size_t capacity=flags&0x3fffffff;
    if(length<0 || (size_t)length>capacity || (length && !data))return rpx_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
    if(data && !rpx_extent(p,data,capacity?capacity:1))return 0;
    return rpx_same(p,table,&callback,8) && rpx_same(p,at,raw,sizeof raw);
}
static int rpx_list(rpx_probe *p,uintptr_t at,size_t stride,int strings)
{
    unsigned char raw[0x18];uintptr_t data;int count,capacity;
    if(!rpx_read(p,at,0,raw,sizeof raw))return 0;
    memcpy(&data,raw,8);memcpy(&count,raw+8,4);memcpy(&capacity,raw+12,4);
    if(count<0 || capacity<0 || count>capacity || (capacity && !data))return rpx_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
    if((size_t)capacity>SIZE_MAX/stride)return rpx_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(!rpx_extent(p,data,(size_t)capacity*stride))return 0;
    /* Grow/copy/destruction can visit CAPACITY slots, even with COUNT zero.
     * Borrowed and inline arrays remain valid; this probe never changes them. */
    if(strings)for(int i=0;i<capacity;i++)
        if(!rpx_string(p,data+(size_t)i*stride))return 0;
    return rpx_same(p,at,raw,sizeof raw);
}
int sh_decl_renderprog_context_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    rpx_probe p={r,in};uintptr_t ctx;
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->image_base || !in->image_size ||
       in->image_size>UINTPTR_MAX-in->image_base || !in->resource || !in->default_callback ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context || r->invocation.work_limit!=in->work_limit)
        return rpx_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    if(!rpx_extent(&p,in->resource,0x1870))return 0;
    ctx=in->resource+0x78;
    if(!rpx_list(&p,in->resource+0x58,8,0) || !rpx_string(&p,ctx+8) ||
       !rpx_list(&p,ctx+0x38,0x30,1) || !rpx_list(&p,ctx+0x50,4,0) ||
       !rpx_list(&p,ctx+0x70,8,0) || !rpx_list(&p,ctx+0x88,16,0) ||
       !rpx_list(&p,ctx+0x9d0,0x30,1) || !rpx_list(&p,ctx+0x15e8,8,0))return 0;
    for(unsigned i=0;i<3;i++) {
        if(!rpx_list(&p,ctx+0xa0+i*0x1b0,2,0) || !rpx_list(&p,ctx+0x178+i*0x1b0,2,0) ||
           !rpx_list(&p,ctx+0x5b0+i*0x58,2,0) || !rpx_list(&p,ctx+0x6b8+i*0x58,2,0) ||
           !rpx_list(&p,ctx+0x7c0+i*0x28,2,0) || !rpx_list(&p,ctx+0x838+i*0x28,2,0) ||
           !rpx_list(&p,ctx+0x8b0+i*0x28,2,0) || !rpx_list(&p,ctx+0x928+i*0x38,2,0) ||
           !rpx_string(&p,ctx+0x16f8+i*0x30))return 0;
    }
    for(unsigned i=0;i<64;i++)if(!rpx_string(&p,ctx+0x9e8+i*0x30))return 0;
    return 1;
}
