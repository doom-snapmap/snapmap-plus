#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "decl_renderprog_manager_reads.h"
#include "native_pin_hash.h"

typedef struct rpm_pin {uint32_t rva;size_t length;unsigned char digest[32];} rpm_pin;
typedef struct rpm_profile {
    uint32_t holder,object,table,typed,typed_table,sampler,sampler_object,sampler_table,factory,sampler_lookup;
    uint32_t slots[4];
    uint32_t lock,lock_epoch,postload,load_factory,resource_table,resource_delete,find_id;
} rpm_profile;
#include "decl_renderprog_manager_read_pins.h"
typedef struct rpm_probe {sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in;} rpm_probe;
typedef struct rpm_array {uintptr_t data;int32_t count,capacity;unsigned char raw[0x18];} rpm_array;
static int rpm_fail(rpm_probe *p,sh_resource_default_read_reason reason,uintptr_t at)
{p->r->reason=reason;p->r->offending_address=at;return 0;}
static int rpm_read(rpm_probe *p,uintptr_t at,size_t off,void *out,size_t size)
{
    if(!at || off>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+off))return rpm_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->r->work==SIZE_MAX || (p->in->work_limit && p->r->work>=p->in->work_limit))
        return rpm_fail(p,SH_DEFAULT_READ_BUDGET,at+off);
    p->r->work++;
    return p->in->read(p->in->read_context,at+off,out,size) || rpm_fail(p,SH_DEFAULT_READ_MEMORY,at+off);
}
static int rpm_image(rpm_probe *p,uint32_t rva,size_t size)
{
    return (rva<=p->in->image_size && size<=p->in->image_size-rva) ||
        rpm_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
}
static int rpm_same(rpm_probe *p,uintptr_t at,const void *before,size_t size)
{
    unsigned char after[0x30];
    if(size>sizeof after)return rpm_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    return rpm_read(p,at,0,after,size) && (!memcmp(after,before,size) || rpm_fail(p,SH_DEFAULT_READ_INVOCATION,at));
}
static int rpm_extent(rpm_probe *p,uintptr_t at,size_t size)
{
    unsigned char bytes[256];
    for(size_t off=0;off<size;) {
        size_t n=size-off;if(n>sizeof bytes)n=sizeof bytes;
        if(!rpm_read(p,at,off,bytes,n))return 0;off+=n;
    }
    return 1;
}
static int rpm_text(rpm_probe *p,uintptr_t at)
{
    unsigned char c;
    for(size_t off=0;;off++) {
        if(!rpm_read(p,at,off,&c,1))return 0;
        if(!c)return 1;
        if(off==SIZE_MAX)return rpm_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    }
}
static int rpm_array_read(rpm_probe *p,uintptr_t at,size_t stride,rpm_array *a)
{
    if(!rpm_read(p,at,0,a->raw,sizeof a->raw))return 0;
    memcpy(&a->data,a->raw,8);memcpy(&a->count,a->raw+8,4);memcpy(&a->capacity,a->raw+12,4);
    if(a->count<0 || a->capacity<0 || a->count>a->capacity ||
       (a->capacity && !a->data))return rpm_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
    if((size_t)a->capacity>SIZE_MAX/stride)return rpm_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    return rpm_extent(p,a->data,(size_t)a->capacity*stride);
}
static int rpm_pin_check(rpm_probe *p,unsigned which)
{
    const rpm_pin *pin=rpm_pins[p->in->renderer]+which;unsigned char bytes[256],digest[32];sh_native_pin_hash hash;
    if(!rpm_image(p,pin->rva,pin->length))return 0;
    sh_native_pin_hash_init(&hash);
    for(size_t off=0;off<pin->length;) {
        size_t n=pin->length-off;if(n>sizeof bytes)n=sizeof bytes;
        if(!rpm_read(p,p->in->image_base+pin->rva,off,bytes,n))return 0;
        sh_native_pin_hash_update(&hash,bytes,n);
        off+=n;
    }
    sh_native_pin_hash_finish(&hash,digest);
    return !memcmp(digest,pin->digest,32) || rpm_fail(p,SH_DEFAULT_READ_PIN,p->in->image_base+pin->rva);
}
static int rpm_exact_pointer(rpm_probe *p,uintptr_t at,size_t off,uint32_t expected)
{
    uintptr_t actual;
    if(!rpm_image(p,expected,1) || !rpm_read(p,at,off,&actual,8))return 0;
    return actual==p->in->image_base+expected || rpm_fail(p,SH_DEFAULT_READ_CALLBACK,actual);
}
static int rpm_bindings(rpm_probe *p)
{
    const rpm_profile *b=rpm_profiles+p->in->renderer;uintptr_t base=p->in->image_base;
    static const unsigned slots[]={0x50,0x60,0x70,0x90};
    if(!rpm_image(p,b->holder,8) || !rpm_image(p,b->object,0x58) || !rpm_image(p,b->table,0x98) ||
       !rpm_image(p,b->typed,0xd8) || !rpm_image(p,b->typed_table,0x38) || !rpm_image(p,b->resource_table,8) ||
       !rpm_image(p,b->sampler,8) || !rpm_image(p,b->sampler_object,0xa0) || !rpm_image(p,b->sampler_table,0x20))return 0;
    if(!rpm_exact_pointer(p,base,b->holder,b->object) || !rpm_exact_pointer(p,base+b->object,0,b->table) ||
       !rpm_exact_pointer(p,base+b->typed,0,b->typed_table) || !rpm_exact_pointer(p,base+b->typed_table,0x30,b->factory) ||
       !rpm_exact_pointer(p,base+b->typed_table,0x18,b->load_factory) ||
       !rpm_exact_pointer(p,base+b->typed_table,0x10,b->find_id) ||
       !rpm_exact_pointer(p,base+b->resource_table,0,b->resource_delete) ||
       !rpm_exact_pointer(p,base,b->sampler,b->sampler_object) || !rpm_exact_pointer(p,base+b->sampler_object,0,b->sampler_table) ||
       !rpm_exact_pointer(p,base+b->sampler_table,0x18,b->sampler_lookup))return 0;
    for(unsigned i=0;i<4;i++)if(!rpm_exact_pointer(p,base+b->table,slots[i],b->slots[i]))return 0;
    return 1;
}
static int rpm_permutation_state(rpm_probe *p)
{
    const rpm_profile *b=rpm_profiles+p->in->renderer;uintptr_t lock,postload;int epoch;
    if(!rpm_image(p,b->lock,8) || !rpm_image(p,b->lock_epoch,4) || !rpm_image(p,b->postload,8) ||
       !rpm_read(p,p->in->image_base,b->lock,&lock,8) || !rpm_read(p,p->in->image_base,b->lock_epoch,&epoch,4) ||
       !rpm_read(p,p->in->image_base,b->postload,&postload,8))return 0;
    /* Native lazily allocates a CRITICAL_SECTION. Zero is the legal pending
     * initialization shape; a current initialized pointer is an OS object,
     * without a resource or user callback. The pinned getter owns its TLS/CRT
     * initialization. Do not impose a synthetic epoch or lock-count value. */
    if(lock && !rpm_extent(p,lock,0x28))return 0;
    /* All retained native writers clear this optional callback. A nonnull
     * value has no established native callback authority in this contract. */
    if(postload)return rpm_fail(p,SH_DEFAULT_READ_CALLBACK,postload);
    return rpm_same(p,p->in->image_base+b->lock,&lock,8) &&
        rpm_same(p,p->in->image_base+b->lock_epoch,&epoch,4) &&
        rpm_same(p,p->in->image_base+b->postload,&postload,8);
}
/* These are native array/hash bounds, not traversal depth limits. Stored
 * resource handles stay opaque; validating their memory never creates edges. */
static int rpm_hash(rpm_probe *p,uintptr_t at,int count)
{
    unsigned char raw[0x28];uintptr_t buckets,next;int size,capacity;uint32_t hash_mask,lookup;
    if(!rpm_read(p,at,0,raw,sizeof raw))return 0;
    memcpy(&buckets,raw,8);memcpy(&next,raw+8,8);memcpy(&size,raw+0x10,4);
    memcpy(&capacity,raw+0x14,4);memcpy(&hash_mask,raw+0x1c,4);memcpy(&lookup,raw+0x20,4);
    if(size<1 || capacity<0 || hash_mask!=(uint32_t)size-1 || ((uint32_t)size&((uint32_t)size-1)) ||
       (lookup && lookup!=UINT32_MAX))return rpm_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
    if(!lookup) {
        uint32_t sentinel;
        if(!rpm_read(p,buckets,0,&sentinel,4))return 0;
        if(sentinel!=UINT32_MAX || count)return rpm_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
    } else {
        if(capacity<count || (size_t)size>SIZE_MAX/4 || (size_t)capacity>SIZE_MAX/4)
            return rpm_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
        if(!rpm_extent(p,buckets,(size_t)size*4) || !rpm_extent(p,next,(size_t)capacity*4))return 0;
        for(int bucket=0;bucket<size;bucket++) {
            uint32_t index;
            if(!rpm_read(p,buckets,(size_t)bucket*4,&index,4))return 0;
            size_t steps=0;
            while(index!=UINT32_MAX) {
                if(index>=(uint32_t)count || index>=(uint32_t)capacity || steps++>=(size_t)count)
                    return rpm_fail(p,SH_DEFAULT_READ_OWNERSHIP,at);
                if(!rpm_read(p,next,(size_t)index*4,&index,4))return 0;
            }
        }
    }
    return rpm_same(p,at,raw,sizeof raw);
}
static int rpm_link(rpm_probe *p,uintptr_t node,size_t off,uintptr_t *next)
{if(!node){*next=0;return 1;}return rpm_read(p,node,off,next,8) && rpm_same(p,node+off,next,8);}
static int rpm_parent(rpm_probe *p,uintptr_t node,uintptr_t wanted,int *contains)
{
    uintptr_t slow=node,fast=node;*contains=0;
    while(node) {
        if(node==wanted)*contains=1;
        if(!rpm_link(p,node,0x88,&node) || !rpm_link(p,slow,0x88,&slow) ||
           !rpm_link(p,fast,0x88,&fast) || !rpm_link(p,fast,0x88,&fast))return 0;
        if(fast && slow==fast)return rpm_fail(p,SH_DEFAULT_READ_OWNERSHIP,fast);
    }
    return 1;
}
static int rpm_source_chain(rpm_probe *p,uintptr_t head)
{
    uintptr_t node=head,slow=head,fast=head;
    while(node) {
        unsigned char raw[0x30];uintptr_t name;int offset,length;
        if(!rpm_read(p,node,0,raw,sizeof raw))return 0;
        memcpy(&name,raw,8);memcpy(&offset,raw+0x24,4);memcpy(&length,raw+0x28,4);
        if(!rpm_text(p,name))return 0;
        if(offset<0 || length<0 || offset>INT_MAX-length)return rpm_fail(p,SH_DEFAULT_READ_BOUNDS,node);
        if(!rpm_same(p,node,raw,sizeof raw) || !rpm_link(p,node,0x18,&node) ||
           !rpm_link(p,slow,0x18,&slow) || !rpm_link(p,fast,0x18,&fast) || !rpm_link(p,fast,0x18,&fast))return 0;
        if(fast && slow==fast)return rpm_fail(p,SH_DEFAULT_READ_OWNERSHIP,fast);
    }
    return 1;
}
static int rpm_source(rpm_probe *p,uintptr_t source)
{
    uintptr_t name,chain;rpm_array include;
    if(!rpm_read(p,source,8,&name,8) || !rpm_text(p,name) || !rpm_read(p,source,0x30,&chain,8) ||
       !rpm_source_chain(p,chain) || !rpm_array_read(p,source+0x38,4,&include))return 0;
    return rpm_same(p,source+8,&name,8) && rpm_same(p,source+0x30,&chain,8) &&
        rpm_same(p,source+0x38,include.raw,sizeof include.raw);
}
static int rpm_type(rpm_probe *p,uintptr_t type)
{
    const rpm_profile *b=rpm_profiles+p->in->renderer;rpm_array resources,sources;uintptr_t name,type_name;
    if(!rpm_exact_pointer(p,type,0,b->typed_table) || !rpm_exact_pointer(p,p->in->image_base+b->typed_table,0x30,b->factory) ||
       !rpm_read(p,type,8,&type_name,8) || !rpm_text(p,type_name) ||
       !rpm_read(p,type,0x68,&name,8) || !rpm_text(p,name) ||
       !rpm_array_read(p,type+0x20,8,&resources) || !rpm_hash(p,type+0x38,resources.count) ||
       !rpm_array_read(p,type+0x98,8,&sources) || !rpm_hash(p,type+0xb0,sources.count))return 0;
    for(int i=0;i<resources.count;i++) {
        uintptr_t object,resource_name,table,source;unsigned char raw[0x30];
        if(!rpm_read(p,resources.data,(size_t)i*8,&object,8) || !rpm_read(p,object,0,raw,sizeof raw))return 0;
        memcpy(&table,raw,8);memcpy(&resource_name,raw+8,8);memcpy(&source,raw+0x18,8);
        /* GetResourceIndexForID explicitly accepts a null stored name. A
         * loaded/default resource is borrowed data until an actual consumer
         * observes it; this catalog check never invents dependency edges. */
        if(resource_name && !rpm_text(p,resource_name))return 0;
        /* Re-find(makeDefault=1) may delete an unresolved stored resource.
         * Qualify precisely that dispatched deleting slot; settled resources
         * are not dispatched here merely because they occur in the array. */
        if(source && !(raw[0x2c]&4) && !rpm_exact_pointer(p,table,0,b->resource_delete))return 0;
        if(!rpm_same(p,object,raw,sizeof raw) || !rpm_same(p,resources.data+(size_t)i*8,&object,8))return 0;
    }
    for(int i=0;i<sources.count;i++) {
        uintptr_t source;
        if(!rpm_read(p,sources.data,(size_t)i*8,&source,8) || !rpm_source_chain(p,source) ||
           !rpm_same(p,sources.data+(size_t)i*8,&source,8))return 0;
    }
    return rpm_same(p,type+8,&type_name,8) && rpm_same(p,type+0x68,&name,8) &&
        rpm_same(p,type+0x20,resources.raw,sizeof resources.raw) && rpm_same(p,type+0x98,sources.raw,sizeof sources.raw);
}
static int rpm_catalog(rpm_probe *p)
{
    const rpm_profile *b=rpm_profiles+p->in->renderer;uintptr_t dm=p->in->image_base+b->object;
    rpm_array types,sources,includes;
    if(!rpm_array_read(p,dm+8,8,&types) || !rpm_array_read(p,dm+0x20,8,&sources) ||
       !rpm_array_read(p,dm+0x38,0x28,&includes))return 0;
    for(int i=0;i<types.count;i++) {
        uintptr_t type;int contains;
        if(!rpm_read(p,types.data,(size_t)i*8,&type,8) || !rpm_parent(p,type,p->in->image_base+b->typed,&contains))return 0;
        if(contains && !rpm_type(p,type))return 0;
        if(!rpm_same(p,types.data+(size_t)i*8,&type,8))return 0;
    }
    if(!rpm_type(p,p->in->image_base+b->typed))return 0;
    for(int i=0;i<sources.count;i++) {
        uintptr_t source;
        if(!rpm_read(p,sources.data,(size_t)i*8,&source,8) || !rpm_source(p,source) ||
           !rpm_same(p,sources.data+(size_t)i*8,&source,8))return 0;
    }
    for(int i=0;i<includes.count;i++) {
        uintptr_t row=includes.data+(size_t)i*0x28,name;rpm_array handles;
        if(!rpm_read(p,row,0,&name,8) || !rpm_text(p,name) || !rpm_array_read(p,row+8,8,&handles) ||
           !rpm_same(p,row,&name,8) || !rpm_same(p,row+8,handles.raw,sizeof handles.raw))return 0;
        /* Native compares these borrowed interned source-name handles by
         * equality. It never dereferences them in this loop. */
    }
    return rpm_same(p,dm+8,types.raw,sizeof types.raw) && rpm_same(p,dm+0x20,sources.raw,sizeof sources.raw) &&
        rpm_same(p,dm+0x38,includes.raw,sizeof includes.raw);
}
static int rpm_sampler(rpm_probe *p)
{
    const rpm_profile *b=rpm_profiles+p->in->renderer;uintptr_t object=p->in->image_base+b->sampler_object;
    rpm_array rows;
    if(!rpm_array_read(p,object+0x90,0x68,&rows))return 0;
    for(int i=0;i<rows.count;i++) {
        uintptr_t row=rows.data+(size_t)i*0x68,data,payload;
        /* The native string comparison consumes the data pointer, without
         * invoking the row's string vtable or length/capacity metadata. */
        if(!rpm_read(p,row,0x10,&data,8) || !rpm_text(p,data) ||
           !rpm_read(p,row,0x60,&payload,8) || !rpm_same(p,row+0x10,&data,8) ||
           !rpm_same(p,row+0x60,&payload,8))return 0;
        /* Payload may be null. Native returns this opaque stored handle;
         * reached consumers supply its actual resource origin separately. */
    }
    return rpm_same(p,object+0x90,rows.raw,sizeof rows.raw);
}
int sh_decl_renderprog_manager_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    rpm_probe p={r,in};
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->image_base || !in->image_size ||
       in->image_size>UINTPTR_MAX-in->image_base || !in->read || !in->resource || !in->default_callback ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return rpm_fail(&p,SH_DEFAULT_READ_INPUT,0);
    for(unsigned i=0;i<rpm_pin_counts[in->renderer];i++)if(!rpm_pin_check(&p,i))return 0;
    return rpm_bindings(&p) && rpm_permutation_state(&p) && rpm_catalog(&p) && rpm_sampler(&p) &&
        rpm_permutation_state(&p) && rpm_bindings(&p);
}

