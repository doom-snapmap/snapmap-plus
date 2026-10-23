#include <stdint.h>
#include <string.h>
#include "decl_default_reads.h"
#include "decl_default_allocator.h"
#include "decl_md6_engine_reads.h"
typedef struct ddp_pin { uint32_t rva; unsigned length; unsigned char bytes[20]; } ddp_pin;
#include "decl_default_read_pins.h"
typedef struct ddp_context_pin { uint32_t rva; unsigned length; unsigned char bytes[192]; } ddp_context_pin;
typedef struct ddp_context_binding {
    uint32_t game_holder, game_object, game_table, context_object, context_table, context_guard;
    ddp_context_pin functions[3];
} ddp_context_binding;
#include "decl_default_context_bindings.h"
enum { DDP_DEFAULT, DDP_TEXT, DDP_RELEASE, DDP_COMMON, DDP_BEHAVIOR,
    DDP_ACTOR, DDP_FSM, DDP_ADVANCED, DDP_CHARGE, DDP_FOCUS, DDP_VIEW,
    DDP_ONLINE, DDP_LAYERS, DDP_TWITCH };
typedef struct ddp_probe {
    sh_resource_default_read_receipt *receipt;
    const sh_resource_default_read_input *input;
} ddp_probe;
static const uint32_t ddp_define_heads[2]={0x58b6b38,0x6fb02c8};
static int ddp_fail(ddp_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{
    p->receipt->reason=why; p->receipt->offending_address=at; return 0;
}
static int ddp_read(ddp_probe *p,uintptr_t at,size_t offset,void *out,size_t size)
{
    if (!at || offset>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+offset))
        return ddp_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if (!p->input->read(p->input->read_context,at+offset,out,size))
        return ddp_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
    p->receipt->work++;
    return 1;
}
static int ddp_image(ddp_probe *p,uintptr_t at,size_t size)
{
    uintptr_t base=p->input->image_base;
    return at>=base && at-base<=p->input->image_size && size<=p->input->image_size-(at-base);
}
static int ddp_pin_check(ddp_probe *p,unsigned which)
{
    unsigned char bytes[20];
    const ddp_pin *pin=ddp_pins[p->input->renderer]+which;
    uintptr_t at=p->input->image_base+pin->rva;
    if (!ddp_image(p,at,pin->length)) return ddp_fail(p,SH_DEFAULT_READ_IMAGE,at);
    if (!ddp_read(p,at,0,bytes,pin->length)) return 0;
    return !memcmp(bytes,pin->bytes,pin->length) || ddp_fail(p,SH_DEFAULT_READ_PIN,at);
}
static int ddp_leaf_address(ddp_probe *p,uintptr_t function,uintptr_t *target)
{
    unsigned char code[8];
    int32_t relative;
    if (!ddp_image(p,function,sizeof code) || !ddp_read(p,function,0,code,sizeof code)) return 0;
    if (code[0]!=0x48 || code[1]!=0x8d || code[2]!=0x05 || code[7]!=0xc3)
        return ddp_fail(p,SH_DEFAULT_READ_CALLBACK,function);
    memcpy(&relative,code+3,4);
    /* The instruction lies in the checked image. Use a signed difference,
     * checking both sides before converting back to an address. */
    if (relative<0 && (uint64_t)(-(int64_t)relative)>function+7)
        return ddp_fail(p,SH_DEFAULT_READ_BOUNDS,function);
    if (relative>=0 && (uint64_t)relative>UINTPTR_MAX-(function+7))
        return ddp_fail(p,SH_DEFAULT_READ_BOUNDS,function);
    *target=relative<0 ? function+7-(uintptr_t)(-(int64_t)relative) : function+7+(uintptr_t)relative;
    return ddp_image(p,*target,1) || ddp_fail(p,SH_DEFAULT_READ_IMAGE,*target);
}
static int ddp_zero_count(ddp_probe *p,size_t offset)
{
    int count;
    return ddp_read(p,p->input->resource,offset,&count,sizeof count) &&
        (!count || ddp_fail(p,SH_DEFAULT_READ_OWNERSHIP,p->input->resource+offset));
}
static int ddp_context_ready(ddp_probe *p)
{
    return sh_decl_engine_read_check(p->receipt,p->input,SH_DECL_ENGINE_READ_80);
}
static int ddp_input_valid(sh_resource_default_read_receipt *r,
                           const sh_resource_default_read_input *in)
{
    return r && in && r->state==1 && r->reason==SH_DEFAULT_READ_UNSUPPORTED &&
        !r->complete && r->invocation.image_base==in->image_base &&
        r->invocation.image_size==in->image_size && r->invocation.renderer==in->renderer &&
        r->invocation.pinned_image==in->pinned_image && r->invocation.resource==in->resource &&
        r->invocation.default_callback==in->default_callback && r->invocation.read==in->read &&
        r->invocation.read_context==in->read_context && r->invocation.work_limit==in->work_limit &&
        in->renderer>=0 && in->renderer<=1 && in->pinned_image && in->read &&
        in->resource && in->image_base && in->image_size &&
        in->image_size<=UINTPTR_MAX-in->image_base &&
        in->default_callback==in->image_base+ddp_pins[in->renderer][DDP_DEFAULT].rva;
}
int sh_decl_default_names_unshadowed(sh_resource_default_read_receipt *r,
                                     const sh_resource_default_read_input *in,
                                     const char *const *names,size_t count)
{
    ddp_probe p={r,in};
    uintptr_t head,node,fast,name,next,again,at;
    if (!ddp_input_valid(r,in)) return 0;
    if (count && !names) return ddp_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    for (size_t i=0;i<count;i++)
        if (!names[i]) return ddp_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    at=in->image_base+ddp_define_heads[in->renderer];
    if (!ddp_image(&p,at,sizeof head)) return ddp_fail(&p,SH_DEFAULT_READ_IMAGE,at);
    if (!ddp_read(&p,at,0,&head,sizeof head)) return 0;
    node=fast=head;
    while (node) {
        if (!ddp_read(&p,node,0x10,&name,sizeof name)) return 0;
        for (size_t i=0;i<count;i++) {
            size_t k=0;
            for (;;) {
                char actual;
                if (!ddp_read(&p,name,k,&actual,1)) return 0;
                if (in->work_limit && r->work>in->work_limit)
                    return ddp_fail(&p,SH_DEFAULT_READ_BUDGET,name);
                if (actual!=names[i][k]) break;
                if (!actual) return ddp_fail(&p,SH_DEFAULT_READ_OWNERSHIP,name);
                if (k==SIZE_MAX) return ddp_fail(&p,SH_DEFAULT_READ_BOUNDS,name);
                k++;
            }
        }
        if (!ddp_read(&p,node,0x50,&next,sizeof next)) return 0;
        node=next;
        /* A malformed list cannot be certified by a finite prefix. */
        if (fast) {
            if (!ddp_read(&p,fast,0x50,&fast,sizeof fast)) return 0;
            if (fast && !ddp_read(&p,fast,0x50,&fast,sizeof fast)) return 0;
        }
        if (node && node==fast) return ddp_fail(&p,SH_DEFAULT_READ_OWNERSHIP,node);
        if (in->work_limit && r->work>in->work_limit)
            return ddp_fail(&p,SH_DEFAULT_READ_BUDGET,node);
    }
    if (!ddp_read(&p,at,0,&again,sizeof again)) return 0;
    if (head!=again) return ddp_fail(&p,SH_DEFAULT_READ_OWNERSHIP,at);
    if (in->work_limit && r->work>in->work_limit)
        return ddp_fail(&p,SH_DEFAULT_READ_BUDGET,in->resource);
    return 1;
}
int sh_decl_default_envelope_read_check(sh_resource_default_read_receipt *r,
                                        const sh_resource_default_read_input *in,
                                        uintptr_t *text_out)
{
    ddp_probe p={r,in};
    uintptr_t table,actual,text_fn,manager_fn,text,manager;
    if (text_out) *text_out=0;
    if (!ddp_input_valid(r,in)) return 0;
    if (!ddp_pin_check(&p,DDP_DEFAULT)) return 0;
    if (!ddp_read(&p,in->resource,0,&table,sizeof table) ||
        !ddp_read(&p,table,0x50,&actual,sizeof actual) ||
        !ddp_read(&p,table,0x70,&manager_fn,sizeof manager_fn) ||
        !ddp_read(&p,table,0x88,&text_fn,sizeof text_fn)) return 0;
    if (actual!=in->default_callback) return ddp_fail(&p,SH_DEFAULT_READ_UNSUPPORTED,actual);
    if (!ddp_leaf_address(&p,text_fn,&text) || !ddp_leaf_address(&p,manager_fn,&manager) ||
        !ddp_context_ready(&p) || !sh_decl_default_allocator_read_check(r,in) ||
        !sh_decl_default_crt_allocation_read_check(r,in) ||
        !sh_decl_default_token_read_check(r,in)) return 0;
    if (in->work_limit && r->work>in->work_limit) return ddp_fail(&p,SH_DEFAULT_READ_BUDGET,in->resource);
    if (text_out) *text_out=text;
    return 1;
}
int sh_decl_default_common_body_read_check(sh_resource_default_read_receipt *r,
                                           const sh_resource_default_read_input *in)
{
    ddp_probe p={r,in};
    uintptr_t table,text_fn,text;
    unsigned char parsed,definition[5];
    if (!ddp_input_valid(r,in)) return 0;
    if (!ddp_pin_check(&p,DDP_COMMON) || !ddp_pin_check(&p,DDP_BEHAVIOR) ||
        !ddp_pin_check(&p,DDP_TEXT)) return 0;
    if (!ddp_read(&p,in->resource,0,&table,sizeof table) ||
        !ddp_read(&p,table,0x88,&text_fn,sizeof text_fn)) return 0;
    if (text_fn!=in->image_base+ddp_pins[in->renderer][DDP_TEXT].rva)
        return ddp_fail(&p,SH_DEFAULT_READ_UNSUPPORTED,text_fn);
    if (!sh_decl_default_envelope_read_check(r,in,&text) ||
        !ddp_read(&p,text,0,definition,sizeof definition)) return 0;
    if (memcmp(definition,"{ }\n",5)) return ddp_fail(&p,SH_DEFAULT_READ_PIN,text);
    if (!ddp_read(&p,in->resource,0x60,&parsed,1)) return 0;
    if (parsed) return ddp_fail(&p,SH_DEFAULT_READ_OWNERSHIP,in->resource+0x60);
    if (in->work_limit && r->work>in->work_limit) return ddp_fail(&p,SH_DEFAULT_READ_BUDGET,in->resource);
    return 1; /* The caller still proves its actual parser and cleanup. */
}
int sh_decl_default_read_begin(sh_resource_default_read_receipt *r,
                              const sh_resource_default_read_input *in)
{
    ddp_probe p={r,in};
    uintptr_t table,parser,release;
    unsigned selected=14;
    if (!ddp_input_valid(r,in)) return 0;
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION;
    if (!ddp_read(&p,in->resource,0,&table,sizeof table) ||
        !ddp_read(&p,table,0x98,&parser,sizeof parser) ||
        !ddp_read(&p,table,0xa0,&release,sizeof release)) return 0;
    if (release!=in->image_base+ddp_pins[in->renderer][DDP_RELEASE].rva)
        return ddp_fail(&p,SH_DEFAULT_READ_UNSUPPORTED,release);
    for (unsigned i=DDP_COMMON;i<14;i++)
        if (parser==in->image_base+ddp_pins[in->renderer][i].rva) { selected=i; break; }
    if (selected==14) return ddp_fail(&p,SH_DEFAULT_READ_UNSUPPORTED,parser);
    if (!ddp_pin_check(&p,selected) || !ddp_pin_check(&p,DDP_RELEASE) ||
        !sh_decl_default_common_body_read_check(r,in)) return 0;
    if ((selected==DDP_LAYERS || selected==DDP_ONLINE) && !ddp_zero_count(&p,0x70)) return 0;
    if (selected==DDP_TWITCH)
        for (size_t i=0;i<5;i++) if (!ddp_zero_count(&p,0x78+i*0x18)) return 0;
    if (in->work_limit && r->work>in->work_limit) return ddp_fail(&p,SH_DEFAULT_READ_BUDGET,in->resource);
    r->complete=1; r->reason=SH_DEFAULT_READ_OK;
    return 1;
}
