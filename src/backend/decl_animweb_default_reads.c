#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "decl_animweb_default_reads.h"
#include "decl_default_reads.h"
#include "decl_diagnostic_reads.h"

typedef struct aw_pin { uint32_t rva; unsigned length; unsigned char bytes[96]; } aw_pin;
enum { AW_PARSE,AW_RELEASE,AW_TEXT,AW_DELETE,AW_DESTRUCT,AW_PINS };
/* ANIMWEB_NATIVE_CONTRACTS_BEGIN */
static const aw_pin aw_pins[2][AW_PINS]={
{
    {0x2651e0,20,{0x48,0x8b,0xc4,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xc0,0x0a,0x00,0x00,0x48}},
    {0x264360,20,{0x40,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}},
    {0x263cc0,8,{0x48,0x8d,0x05,0x69,0xbb,0xd8,0x01,0xc3}},
    {0x2619a0,52,{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8,0xac,0x62,0x00,0x00,0xf6,0xc3,0x01,0x74,0x0d,0xba,0x40,0x00,0x00,0x00,0x48,0x8b,0xcf,0xe8,0x16,0x5e,0xbd,0x01,0x48,0x8b,0xc7,0x48,0x8b,0x5c,0x24,0x30,0x48,0x83,0xc4,0x20,0x5f,0xc3}},
    {0x267c60,11,{0x48,0x8d,0x05,0xc1,0x7b,0xd8,0x01,0x48,0x89,0x01,0xc3}}
},
{
    {0x2658d0,20,{0x48,0x8b,0xc4,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xc0,0x0a,0x00,0x00,0x48}},
    {0x2648b0,20,{0x40,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48}},
    {0x264210,8,{0x48,0x8d,0x05,0x99,0x39,0xd7,0x01,0xc3}},
    {0x262090,52,{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,0xe8,0xac,0x64,0x00,0x00,0xf6,0xc3,0x01,0x74,0x0d,0xba,0x40,0x00,0x00,0x00,0x48,0x8b,0xcf,0xe8,0xe6,0xb7,0xbb,0x01,0x48,0x8b,0xc7,0x48,0x8b,0x5c,0x24,0x30,0x48,0x83,0xc4,0x20,0x5f,0xc3}},
    {0x268550,11,{0x48,0x8d,0x05,0x51,0xf6,0xd6,0x01,0x48,0x89,0x01,0xc3}}
}
};
static const uint32_t aw_default_rvas[2]={0x179de30,0x17ab340};
static const char aw_definition[]=
"{\n"
"\tprops {\n"
"\t}\n"
"states {\n"
"\t}\n"
"\tsubWebs {\n"
"\t}\n"
"}\n";
/* ANIMWEB_NATIVE_CONTRACTS_END */
typedef struct aw_probe {
    sh_resource_default_read_receipt *receipt;
    const sh_resource_default_read_input *input;
    uintptr_t *owners;
    size_t count,capacity;
} aw_probe;
typedef struct aw_list { uintptr_t data; int count,capacity; unsigned char flags; } aw_list;
static int aw_fail(aw_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{ p->receipt->reason=why;p->receipt->offending_address=at;return 0; }
static int aw_read(aw_probe *p,uintptr_t at,size_t offset,void *out,size_t size)
{
    if(!at || offset>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+offset))
        return aw_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->receipt->work==SIZE_MAX || (p->input->work_limit && p->receipt->work>=p->input->work_limit))
        return aw_fail(p,SH_DEFAULT_READ_BUDGET,at+offset);
    if(!p->input->read(p->input->read_context,at+offset,out,size))
        return aw_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
    p->receipt->work++;return 1;
}
static int aw_owned(unsigned char flags) { return flags==0 || flags==3 || flags==4; }
static int aw_extent(aw_probe *p,uintptr_t at,size_t size)
{ return (at && size<=UINTPTR_MAX-at) || aw_fail(p,SH_DEFAULT_READ_BOUNDS,at); }
static int aw_owner(aw_probe *p,uintptr_t address)
{
    if(!address)return 1;
    for(size_t i=0;i<p->count;i++) if(p->owners[i]==address)
        return aw_fail(p,SH_DEFAULT_READ_OWNERSHIP,address);
    if(p->count==p->capacity) {
        size_t capacity=p->capacity?p->capacity*2:32;
        if(capacity<p->capacity || capacity>SIZE_MAX/sizeof *p->owners)
            return aw_fail(p,SH_DEFAULT_READ_ALLOCATION,address);
        void *next=realloc(p->owners,capacity*sizeof *p->owners);
        if(!next)return aw_fail(p,SH_DEFAULT_READ_ALLOCATION,address);
        p->owners=next;p->capacity=capacity;
    }
    p->owners[p->count++]=address;return 1;
}
static int aw_list_read(aw_probe *p,uintptr_t at,aw_list *out)
{
    unsigned char bytes[0x18];
    if(!aw_read(p,at,0,bytes,sizeof bytes))return 0;
    memcpy(&out->data,bytes,8);memcpy(&out->count,bytes+8,4);memcpy(&out->capacity,bytes+0xc,4);
    out->flags=bytes[0x13];
    if(out->count<0 || out->capacity<0 || out->count>out->capacity || (out->count && !out->data))
        return aw_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
    return 1;
}
/* A numerical buffer's contents are not followed by the native free. Record
 * only the actual owned allocation, not a guessed element type or extent. */
static int aw_raw(aw_probe *p,uintptr_t list)
{
    uintptr_t data;unsigned char flags;
    return aw_read(p,list,0,&data,sizeof data) && aw_read(p,list,0x13,&flags,1) &&
        (!aw_owned(flags) || aw_owner(p,data));
}
static int aw_array(aw_probe *p,uintptr_t list,size_t stride,aw_list *out)
{
    if(!aw_list_read(p,list,out))return 0;
    if(!aw_owned(out->flags) || !out->data)return 1;
    if((size_t)out->capacity>SIZE_MAX/stride ||
       (size_t)out->capacity*stride>UINTPTR_MAX-out->data)
        return aw_fail(p,SH_DEFAULT_READ_BOUNDS,list);
    return aw_owner(p,out->data);
}
/* Root layer/subweb arrays and nested transition maps destroy every actual
 * capacity record, whose sole cleanup is a numerical list at member_offset. */
static int aw_tuples(aw_probe *p,uintptr_t list,size_t stride,size_t member_offset)
{
    aw_list array;
    if(!aw_array(p,list,stride,&array))return 0;
    if(aw_owned(array.flags) && array.data) for(size_t i=0;i<(size_t)array.capacity;i++)
        if(!aw_raw(p,array.data+i*stride+member_offset))return 0;
    return 1;
}
static int aw_property(aw_probe *p,uintptr_t at)
{
    uintptr_t list;
    /* Both optional headers are independently deleted even when their
     * numerical buffer is borrowed; the second has nested capacity records. */
    if(!aw_read(p,at,0x30,&list,sizeof list))return 0;
    if(list && (!aw_owner(p,list) || !aw_raw(p,list)))return 0;
    if(!aw_read(p,at,0x38,&list,sizeof list))return 0;
    if(list && (!aw_owner(p,list) || !aw_tuples(p,list,0x20,8)))return 0;
    return aw_raw(p,at+0x18);
}
static int aw_node(aw_probe *p,uintptr_t at)
{
    aw_list properties;
    if(!aw_extent(p,at,0x50) || !aw_owner(p,at) || !aw_raw(p,at+0x20) ||
       !aw_array(p,at+0x38,0x40,&properties))return 0;
    if(aw_owned(properties.flags) && properties.data) for(size_t i=0;i<(size_t)properties.capacity;i++)
        if(!aw_property(p,properties.data+i*0x40))return 0;
    return 1;
}
static uintptr_t aw_address(aw_probe *p,unsigned which)
{ return p->input->image_base+aw_pins[p->input->renderer][which].rva; }
static int aw_pin_check(aw_probe *p,unsigned which)
{
    unsigned char bytes[96];const aw_pin *pin=aw_pins[p->input->renderer]+which;
    if(pin->rva>p->input->image_size || pin->length>sizeof bytes || pin->length>p->input->image_size-pin->rva)
        return aw_fail(p,SH_DEFAULT_READ_IMAGE,p->input->image_base);
    if(!aw_read(p,aw_address(p,which),0,bytes,pin->length))return 0;
    return !memcmp(bytes,pin->bytes,pin->length) || aw_fail(p,SH_DEFAULT_READ_PIN,aw_address(p,which));
}
static int aw_virtual_array(aw_probe *p,uintptr_t list)
{
    aw_list array;
    if(!aw_array(p,list,0x40,&array))return 0;
    if(aw_owned(array.flags) && array.data) for(size_t i=0;i<(size_t)array.capacity;i++) {
        uintptr_t object=array.data+i*0x40,table,callback;
        if(!aw_read(p,object,0,&table,sizeof table) || !aw_read(p,table,0,&callback,sizeof callback))return 0;
        if(callback!=aw_address(p,AW_DELETE))return aw_fail(p,SH_DEFAULT_READ_CALLBACK,callback);
        if(!aw_pin_check(p,AW_DELETE) || !aw_pin_check(p,AW_DESTRUCT))return 0;
        /* Actual native callback receives flag0. Its complete direct child
         * only reassigns this element's vtable; no payload/resource is read. */
    }
    return 1;
}
static int aw_cleanup(aw_probe *p)
{
    uintptr_t resource=p->input->resource,node,raw;aw_list nodes;
    if(!aw_owner(p,resource) || !aw_array(p,resource+0x58,8,&nodes))return 0;
    /* Node destruction is by COUNT regardless of outer array ownership. */
    if(nodes.count && !aw_extent(p,nodes.data,(size_t)nodes.count*8))return 0;
    for(size_t i=0;i<(size_t)nodes.count;i++) {
        if(!aw_read(p,nodes.data,i*8,&node,sizeof node))return 0;
        if(node && !aw_node(p,node))return 0;
    }
    if(!aw_tuples(p,resource+0x118,0x28,8) || !aw_tuples(p,resource+0x70,0x38,0) ||
       !aw_virtual_array(p,resource+0xb8))return 0;
    const size_t primitive[]={0x88,0xa0,0xd0,0xe8};
    for(size_t i=0;i<sizeof primitive/sizeof primitive[0];i++)
        if(!aw_raw(p,resource+primitive[i]))return 0;
    return aw_read(p,resource,0x130,&raw,sizeof raw) && aw_owner(p,raw);
}
static int aw_valid(aw_probe *p)
{
    sh_resource_default_read_receipt *r=p->receipt;
    const sh_resource_default_read_input *in=p->input;
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED ||
       in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->resource ||
       !in->image_base || !in->image_size || in->image_size>UINTPTR_MAX-in->image_base)return 0;
    if(r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return aw_fail(p,SH_DEFAULT_READ_INPUT,in->resource);
    return in->default_callback==in->image_base+aw_default_rvas[in->renderer];
}
int sh_decl_animweb_default_read_begin(sh_resource_default_read_receipt *r,
                                      const sh_resource_default_read_input *in)
{
    aw_probe p={r,in,NULL,0,0};uintptr_t table,parser,release,actual,text;unsigned char composite;
    unsigned char definition[sizeof aw_definition];
    static const char *const names[]={"props","states","subWebs"};
    int ok=0;
    if(!aw_valid(&p) || !aw_read(&p,in->resource,0,&table,sizeof table) ||
       !aw_read(&p,table,0x98,&parser,sizeof parser))return 0;
    if(parser!=aw_address(&p,AW_PARSE))return 0;
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION;
    if(!aw_extent(&p,in->resource,0x15b))return 0;
    if(!aw_read(&p,table,0xa0,&release,sizeof release) || !aw_read(&p,table,0x50,&actual,sizeof actual))return 0;
    if(release!=aw_address(&p,AW_RELEASE) || actual!=in->default_callback)
        return aw_fail(&p,SH_DEFAULT_READ_CALLBACK,release!=aw_address(&p,AW_RELEASE)?release:actual);
    if(!aw_pin_check(&p,AW_PARSE) || !aw_pin_check(&p,AW_RELEASE) || !aw_pin_check(&p,AW_TEXT) ||
       !aw_read(&p,table,0x88,&actual,sizeof actual))return 0;
    if(actual!=aw_address(&p,AW_TEXT))return aw_fail(&p,SH_DEFAULT_READ_CALLBACK,actual);
    if(!sh_decl_default_envelope_read_check(r,in,&text) || !aw_read(&p,text,0,definition,sizeof definition))return 0;
    if(memcmp(definition,aw_definition,sizeof definition))return aw_fail(&p,SH_DEFAULT_READ_PIN,text);
    if(!aw_read(&p,in->resource,0x15a,&composite,1) ||
       !sh_decl_default_names_unshadowed(r,in,names,composite?1:3))return 0;
    if(composite && !sh_decl_diagnostic_read_check(r,in,1))return 0;
    if(aw_cleanup(&p)) {
        /* Native release and the empty literal leave all own counts zero.
         * Both composite and normal routes skip reference-bearing loops.
         * Tail resizers request zero; borrowed buffers remain borrowed, owned
         * buffers were retired by the witnessed cleanup above. The unconditional
         * size0 allocation uses the shared allocator. Composite timing/name
         * output requires the actual diagnostic receiver contract above. */
        r->complete=1;r->reason=SH_DEFAULT_READ_OK;ok=1;
    }
    free(p.owners);return ok;
}
