#include <stdint.h>
#include <string.h>
#include "decl_default_allocator.h"

typedef struct dpa_method {
    uint32_t slot,rva;
    unsigned length;
    unsigned char bytes[512];
} dpa_method;
typedef struct dpa_profile {
    uint32_t holder,instance,table;
    dpa_method methods[6];
} dpa_profile;
typedef struct dpa_token_binding {
    uint32_t table,rva;
    unsigned length;
    unsigned char bytes[96];
} dpa_token_binding;
#include "decl_default_allocator_pins.h"
typedef struct dpa_crt_profile {
    uint32_t handler,mode,default_handler,throw_info;
    dpa_method methods[8];
} dpa_crt_profile;
#include "decl_default_crt_pins.h"
typedef struct dpa_probe {
    sh_resource_default_read_receipt *receipt;
    const sh_resource_default_read_input *input;
} dpa_probe;
static int dpa_fail(dpa_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{ p->receipt->reason=why; p->receipt->offending_address=at; return 0; }
static int dpa_read(dpa_probe *p,size_t rva,void *out,size_t size)
{
    uintptr_t at=p->input->image_base;
    if (rva>p->input->image_size || size>p->input->image_size-rva)
        return dpa_fail(p,SH_DEFAULT_READ_IMAGE,at);
    at+=rva;
    if (!p->input->read(p->input->read_context,at,out,size))
        return dpa_fail(p,SH_DEFAULT_READ_MEMORY,at);
    p->receipt->work++;
    if (p->input->work_limit && p->receipt->work>p->input->work_limit)
        return dpa_fail(p,SH_DEFAULT_READ_BUDGET,at);
    return 1;
}
static int dpa_input_valid(sh_resource_default_read_receipt *r,
                           const sh_resource_default_read_input *in)
{
    if (!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED ||
        in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read ||
        !in->image_base || !in->image_size || in->image_size>UINTPTR_MAX-in->image_base ||
        r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
        r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
        r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
        r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
        r->invocation.work_limit!=in->work_limit) return 0;
    return 1;
}
int sh_decl_default_crt_allocation_read_check(sh_resource_default_read_receipt *r,
                                              const sh_resource_default_read_input *in)
{
    dpa_probe p={r,in}; const dpa_crt_profile *profile;
    uintptr_t handler; unsigned mode,attributes; unsigned char bytes[512];
    if (!dpa_input_valid(r,in)) return 0;
    profile=dpa_crt_profiles+in->renderer;
    if (!dpa_read(&p,profile->handler,&handler,sizeof handler) ||
        !dpa_read(&p,profile->mode,&mode,sizeof mode)) return 0;
    /* Both malloc/calloc can recover through the separate CRT new handler.
     * The ordinary mode0 excludes that callback; nonzero modes remain open. */
    if (mode) return dpa_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+profile->mode);
    if (handler && (profile->default_handler>=in->image_size ||
                    handler!=in->image_base+profile->default_handler))
        return dpa_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+profile->handler);
    if (handler) {
        if (!dpa_read(&p,profile->throw_info,&attributes,sizeof attributes)) return 0;
        /* Attribute0 excludes CxxThrowException's WinRT callback branch. */
        if (attributes) return dpa_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+profile->throw_info);
    }
    for (unsigned i=0;i<(handler?8u:5u);i++) {
        const dpa_method *method=profile->methods+i;
        if (method->length>sizeof bytes || !dpa_read(&p,method->rva,bytes,method->length)) return 0;
        if (memcmp(bytes,method->bytes,method->length))
            return dpa_fail(&p,SH_DEFAULT_READ_PIN,in->image_base+method->rva);
    }
    /* Native operator-new recovery is either absent or the exact throwing
     * handler. Its fixed exception path cannot return and retry allocation.
     * Unknown overridden callbacks are never inferred from native success. */
    return 1;
}
int sh_decl_default_token_read_check(sh_resource_default_read_receipt *r,
                                     const sh_resource_default_read_input *in)
{
    dpa_probe p={r,in}; const dpa_token_binding *token;
    uintptr_t actual; unsigned char bytes[96];
    if (!dpa_input_valid(r,in)) return 0;
    token=dpa_tokens+in->renderer;
    if (!dpa_read(&p,token->table,&actual,sizeof actual)) return 0;
    if (token->rva>=in->image_size || actual!=in->image_base+token->rva)
        return dpa_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+token->table);
    if (token->length>sizeof bytes || !dpa_read(&p,token->rva,bytes,token->length)) return 0;
    return !memcmp(bytes,token->bytes,token->length) ||
        dpa_fail(&p,SH_DEFAULT_READ_PIN,in->image_base+token->rva);
}
int sh_decl_default_allocator_read_check(sh_resource_default_read_receipt *r,
                                         const sh_resource_default_read_input *in)
{
    dpa_probe p={r,in}; const dpa_profile *profile;
    uintptr_t actual; unsigned char bytes[512];
    if (!dpa_input_valid(r,in)) return 0;
    profile=dpa_profiles+in->renderer;
    if (!dpa_read(&p,profile->holder,&actual,sizeof actual)) return 0;
    if (profile->instance>=in->image_size || actual!=in->image_base+profile->instance)
        return dpa_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+profile->holder);
    if (!dpa_read(&p,profile->instance,&actual,sizeof actual)) return 0;
    if (profile->table>=in->image_size || actual!=in->image_base+profile->table)
        return dpa_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+profile->instance);
    /* Allocation can invoke this handler and retry successfully. Its absence
     * excludes that unclassified callback, including recovered failures. */
    if (!dpa_read(&p,(size_t)profile->instance+0x38,&actual,sizeof actual)) return 0;
    if (actual) return dpa_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+profile->instance+0x38);
    /* These exact methods implement allocation/free, heap enter/leave and
     * intern/release storage. R358 closes their registered-resource read set.
     * This is a direct native holder, without the JSON getter's TLS guard. */
    for (unsigned i=0;i<6;i++) {
        const dpa_method *method=profile->methods+i;
        size_t slot=(size_t)profile->table+method->slot;
        if (!dpa_read(&p,slot,&actual,sizeof actual)) return 0;
        if (method->rva>=in->image_size || actual!=in->image_base+method->rva)
            return dpa_fail(&p,SH_DEFAULT_READ_CALLBACK,in->image_base+slot);
        if (method->length>sizeof bytes || !dpa_read(&p,method->rva,bytes,method->length)) return 0;
        if (memcmp(bytes,method->bytes,method->length))
            return dpa_fail(&p,SH_DEFAULT_READ_PIN,in->image_base+method->rva);
    }
    return 1;
}
