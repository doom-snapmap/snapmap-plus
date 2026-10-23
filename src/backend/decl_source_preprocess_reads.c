#include <stdint.h>
#include <string.h>
#include "decl_source_preprocess_reads.h"
#include "decl_default_allocator.h"
#include "decl_default_reads.h"
#include "decl_diagnostic_reads.h"
#include "decl_source_file_reads.h"
#include "native_pin_hash.h"

typedef struct dsp_pin {uint32_t rva;size_t length;unsigned char digest[32];} dsp_pin;
#include "decl_source_preprocess_read_pins.h"
typedef struct dsp_probe {sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in;} dsp_probe;
static int dsp_fail(dsp_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{p->r->reason=why;p->r->offending_address=at;return 0;}
static int dsp_pin_check(dsp_probe *p,const dsp_pin *pin)
{
    unsigned char bytes[256],digest[32];sh_native_pin_hash hash;sh_native_pin_hash_init(&hash);
    if(pin->rva>p->in->image_size || pin->length>p->in->image_size-pin->rva)
        return dsp_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    for(size_t offset=0;offset<pin->length;offset+=sizeof bytes){
        size_t size=pin->length-offset;uintptr_t at=p->in->image_base+pin->rva+offset;
        if(size>sizeof bytes)size=sizeof bytes;
        if(p->in->work_limit && p->r->work>=p->in->work_limit)return dsp_fail(p,SH_DEFAULT_READ_BUDGET,at);
        p->r->work++;
        if(!p->in->read(p->in->read_context,at,bytes,size))return dsp_fail(p,SH_DEFAULT_READ_MEMORY,at);
        sh_native_pin_hash_update(&hash,bytes,size);
    }
    sh_native_pin_hash_finish(&hash,digest);
    return !memcmp(digest,pin->digest,sizeof digest) || dsp_fail(p,SH_DEFAULT_READ_PIN,p->in->image_base+pin->rva);
}
int sh_decl_source_preprocess_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    dsp_probe p={r,in};
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->image_base || !in->image_size ||
       in->image_size>UINTPTR_MAX-in->image_base || !in->read || !in->resource || !in->default_callback ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return dsp_fail(&p,SH_DEFAULT_READ_INPUT,0);
    for(unsigned i=0;i<dsp_pin_counts[in->renderer];i++)if(!dsp_pin_check(&p,dsp_pins[in->renderer]+i))return 0;
    /* Native CopyDefines constructs new tokens and never dispatches through
     * the original define token's vtable. No name is excluded here. The
     * zero-name predicate checks the global chain without banning shadows. */
    if(!sh_decl_default_names_unshadowed(r,in,NULL,0) ||
       !sh_decl_default_allocator_read_check(r,in) || !sh_decl_default_crt_allocation_read_check(r,in) ||
       !sh_decl_default_token_read_check(r,in) || !sh_decl_source_file_read_check(r,in) ||
       !sh_decl_diagnostic_read_check(r,in,1) || !sh_decl_diagnostic_read_check(r,in,4))return 0;
    return 1;
}
