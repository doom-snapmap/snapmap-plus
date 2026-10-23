#include <stdint.h>
#include <string.h>
#include "decl_md6_default_reads.h"
#include "decl_default_reads.h"
#include "decl_md6_release_reads.h"
#include "decl_md6_engine_reads.h"
#include "decl_diagnostic_reads.h"
#include "resource_md6_reads.h"
#include "native_pin_hash.h"

typedef struct mdp_pin { uint32_t rva;size_t length;const unsigned char *digest; } mdp_pin;
#include "decl_md6_default_read_pins.h"
typedef struct mdp_probe { sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in; } mdp_probe;
typedef struct mdp_list { uintptr_t data;int count,capacity;unsigned char flags; } mdp_list;
static int mdp_fail(mdp_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{ p->r->reason=why;p->r->offending_address=at;return 0; }
static int mdp_read(mdp_probe *p,uintptr_t at,size_t offset,void *out,size_t size)
{
    if(!at || offset>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+offset))
        return mdp_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->in->work_limit && p->r->work>=p->in->work_limit)
        return mdp_fail(p,SH_DEFAULT_READ_BUDGET,at+offset);
    p->r->work++;
    return p->in->read(p->in->read_context,at+offset,out,size) ||
        mdp_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
}
static int mdp_extent(mdp_probe *p,uintptr_t at,size_t size)
{
    unsigned char b;
    if(!size)return 1;
    if(size>UINTPTR_MAX-at)return mdp_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    return mdp_read(p,at,0,&b,1) && mdp_read(p,at,size-1,&b,1);
}
static uintptr_t mdp_address(mdp_probe *p,unsigned index)
{ return p->in->image_base+mdp_pins[p->in->renderer][index].rva; }
static int mdp_pinned(mdp_probe *p,unsigned index)
{
    const mdp_pin *pin=&mdp_pins[p->in->renderer][index];unsigned char bytes[256],actual[32];sh_native_pin_hash hash;
    sh_native_pin_hash_init(&hash);
    if(pin->rva>p->in->image_size || pin->length>p->in->image_size-pin->rva)
        return mdp_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    for(size_t off=0;off<pin->length;off+=sizeof bytes) {
        size_t n=pin->length-off;if(n>sizeof bytes)n=sizeof bytes;
        if(!mdp_read(p,mdp_address(p,index),off,bytes,n))return 0;
        sh_native_pin_hash_update(&hash,bytes,n);
    }
    sh_native_pin_hash_finish(&hash,actual);
    return !memcmp(actual,pin->digest,sizeof actual) || mdp_fail(p,SH_DEFAULT_READ_PIN,mdp_address(p,index));
}
static int mdp_owned(unsigned char flag) { return flag==0 || flag==3 || flag==4; }
static int mdp_list_read(mdp_probe *p,uintptr_t at,size_t stride,mdp_list *list)
{
    unsigned char raw[0x18];
    if(!mdp_read(p,at,0,raw,sizeof raw))return 0;
    memcpy(&list->data,raw,8);memcpy(&list->count,raw+8,4);
    memcpy(&list->capacity,raw+0xc,4);list->flags=raw[0x13];
    if(list->count<0 || list->capacity<0 || list->count>list->capacity ||
       (list->capacity && !list->data))return mdp_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
    if(stride && (size_t)list->capacity>SIZE_MAX/stride)return mdp_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    return mdp_extent(p,list->data,(size_t)list->capacity*stride);
}
static int mdp_string(mdp_probe *p,uintptr_t at)
{
    unsigned char bytes[0x20];uintptr_t data;int length;uint32_t flags;
    if(!mdp_read(p,at,0,bytes,sizeof bytes))return 0;
    memcpy(&length,bytes+8,4);memcpy(&data,bytes+0x10,8);memcpy(&flags,bytes+0x18,4);
    if(length<0 || (length && !data) || (uint32_t)length>(flags&0x3fffffff))
        return mdp_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
    return !data || mdp_extent(p,data,(flags&0x3fffffff)?(flags&0x3fffffff):1);
}
static int mdp_own_payloads(mdp_probe *p)
{
    mdp_list list,child;uintptr_t object=p->in->resource;
    if(!mdp_extent(p,object,0x430))return 0;
    /* Release resets props COUNT for every flag. An existing borrowed first
     * record is reused, including its previous numerical child COUNT. Its
     * child resize copies numerical records; no former atom is resolved. */
    if(!mdp_list_read(p,object+0x210,0x148,&list))return 0;
    if(!mdp_owned(list.flags) && list.capacity) {
        uintptr_t text;uint32_t capacity;
        if(!mdp_read(p,list.data,0x10,&text,8) || !mdp_read(p,list.data,0x18,&capacity,4) ||
           !mdp_extent(p,text,6) || (capacity&0x3fffffff)<6)
            return mdp_fail(p,SH_DEFAULT_READ_OWNERSHIP,list.data);
        if(!mdp_list_read(p,list.data+0x130,0x28,&child))return 0;
    }
    /* Empty groups eliminate the head-tracking lookup loop, but clearing the
     * old head-tracking storage still visits owned CAPACITY, not COUNT. */
    if(!mdp_list_read(p,object+0x2f0,0x28,&list))return 0;
    if(mdp_owned(list.flags))for(int i=0;i<list.capacity;i++)
        if(!mdp_list_read(p,list.data+(size_t)i*0x28+8,0xc,&child))return 0;
    /* These raw numerical lists are cleared and/or resized from the actual
     * skeleton header. Borrowed storage remains legal and is never erased by
     * the probe. The actual rig observer binds the model and skeleton tokens. */
    const size_t raw_headers[]={0x340,0x3f8,0x410};
    const size_t strides[]={2,2,4};
    for(unsigned i=0;i<3;i++)if(!mdp_list_read(p,object+raw_headers[i],strides[i],&list))return 0;
    /* A zero-joint skeleton reaches the native warning append. Preserve old
     * warning strings and qualify every slot a grow/copy/destructor may read. */
    if(!mdp_list_read(p,object+0x3d8,0x30,&list))return 0;
    for(int i=0;i<list.capacity;i++)if(!mdp_string(p,list.data+(size_t)i*0x30))return 0;
    return 1;
}
static int mdp_events_context(mdp_probe *p)
{
    static const uint32_t tick_holder[2]={0x3e59760,0x55574d0};
    uintptr_t object;int tick;
    /* Empty authored events still reach the native engine context. Share the
     * actual singleton/bootstrap contract with generic declaration parsing. */
    if(!sh_decl_engine_read_check(p->r,p->in,SH_DECL_ENGINE_READ_90))return 0;
    if(!mdp_read(p,p->in->image_base,tick_holder[p->in->renderer],&object,8) ||
       !mdp_read(p,object,0x10,&tick,4))return 0;
    return 1;
}
static const char mdp_definition[]=
"{\n\tinit {\n\tmesh \"md6/default.md6mesh\"\n\t}\n\tuserProps {\n\t}\n\tjointGroups {\n\t}\n"
"\tevents {\n\t}\n\taliases {\n\t}\n\tprops {\n\tprop \"_info\" {\n\t\t}\n\t}\n\teyeInfoCollection 0 {\n\t}\n}\n";
int sh_decl_md6_default_read_begin(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    mdp_probe p={r,in};uintptr_t table,parser,getter,text;
    static const char *const names[]={"init","mesh","userProps","jointGroups","events","aliases","props","prop","eyeInfoCollection"};
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->image_base ||
       !in->image_size || in->image_size>UINTPTR_MAX-in->image_base ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return mdp_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    if(!mdp_read(&p,in->resource,0,&table,8) || !mdp_read(&p,table,0x98,&parser,8))return 0;
    if(parser!=mdp_address(&p,MDP_PARSER))return 0;
    if(!mdp_read(&p,table,0x88,&getter,8))return 0;
    if(getter!=mdp_address(&p,MDP_TEXT))return mdp_fail(&p,SH_DEFAULT_READ_CALLBACK,getter);
    for(unsigned i=0;i<MDP_COUNT;i++)if(!mdp_pinned(&p,i))return 0;
    if(!sh_decl_default_envelope_read_check(r,in,&text))return 0;
    unsigned char literal[sizeof mdp_definition];
    if(!mdp_read(&p,text,0,literal,sizeof literal))return 0;
    if(memcmp(literal,mdp_definition,sizeof literal))return mdp_fail(&p,SH_DEFAULT_READ_PIN,text);
    if(!sh_decl_default_names_unshadowed(r,in,names,sizeof names/sizeof names[0]) ||
       !sh_decl_md6_release_read_check(r,in) || !mdp_own_payloads(&p) || !mdp_events_context(&p))return 0;
    if(!sh_resource_md6_reads_ready())return mdp_fail(&p,SH_DEFAULT_READ_CALLBACK,in->resource);
    if(!sh_decl_diagnostic_read_check(r,in,4))return 0;
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION;r->complete=1;r->reason=SH_DEFAULT_READ_OK;return 1;
}

