#include "native_pin_hash.h"
#include "decl_renderprog_default_reads.h"
#include "decl_default_reads.h"
#include "decl_source_preprocess_reads.h"
#include "decl_source_file_reads.h"
#include "decl_renderprog_backend_reads.h"
#include "decl_renderprog_context_reads.h"
#include "decl_renderprog_cvar_reads.h"
#include "decl_renderprog_release_reads.h"
#include "decl_renderprog_manager_reads.h"
#include "resource_renderprog_reads.h"
#include "resource_parm_reads.h"
#include "resource_graph_native.h"
#include <string.h>

typedef struct rpd_pin {uint32_t rva;size_t length;const unsigned char *hash;} rpd_pin;
#include "decl_renderprog_default_read_pins.h"
typedef struct rpd_profile {uint32_t parse,getter,reload,set_text,compile,holder,object,table,setter;} rpd_profile;
static const rpd_profile rpd_profiles[2]={
    {0xd7eb40,0xd7ddc0,0x179e0c0,0x179f3f0,0xd8ed30,0x3e59350,0x4b17180,0x27265c0,0x17fe600},
    {0xd7e720,0xd7da40,0x17ab5e0,0x17ac910,0xd8e940,0x5557090,0x6218e70,0x2710b40,0x180bcd0}
};
enum {RPD_DEFINITION_LENGTH=350};
static const unsigned char rpd_definition_hash[32]={0x67,0xdc,0xae,0xff,0x01,0xb0,0xbf,0x15,0x8c,0x63,0xe5,0xf7,0x95,0x65,0x60,0xa7,0x5b,0x42,0x29,0x9f,0x52,0xc3,0xe7,0x07,0x7c,0x45,0x9c,0xee,0xdd,0x5c,0x12,0x79};
typedef struct rpd_probe {sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in;} rpd_probe;
static int rpd_fail(rpd_probe *p,sh_resource_default_read_reason reason,uintptr_t at)
{p->r->reason=reason;p->r->offending_address=at;return 0;}
static int rpd_read(rpd_probe *p,uintptr_t at,size_t offset,void *out,size_t size)
{
    if(!at || offset>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+offset))return rpd_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->r->work==SIZE_MAX || (p->in->work_limit && p->r->work>=p->in->work_limit))return rpd_fail(p,SH_DEFAULT_READ_BUDGET,at+offset);
    p->r->work++;
    return p->in->read(p->in->read_context,at+offset,out,size) || rpd_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
}
static int rpd_pin_check(rpd_probe *p,const rpd_pin *pin)
{
    sh_native_pin_hash hash;unsigned char digest[32];
    sh_native_pin_hash_init(&hash);
    unsigned char actual[256];
    if(pin->rva>p->in->image_size || pin->length>p->in->image_size-pin->rva)return rpd_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    for(size_t off=0;off<pin->length;off+=sizeof actual){
        size_t size=pin->length-off;if(size>sizeof actual)size=sizeof actual;
        if(!rpd_read(p,p->in->image_base+pin->rva,off,actual,size))return 0;
        sh_native_pin_hash_update(&hash,actual,size);
    }
    sh_native_pin_hash_finish(&hash,digest);
    return !memcmp(digest,pin->hash,sizeof digest) || rpd_fail(p,SH_DEFAULT_READ_PIN,p->in->image_base+pin->rva);
}
static int rpd_parser_state(rpd_probe *p,const rpd_profile *b)
{
    uintptr_t object,table,setter,again;unsigned state;
    if(b->holder>p->in->image_size || 8>p->in->image_size-b->holder ||
       b->object>p->in->image_size || 0x2c38>p->in->image_size-b->object ||
       b->table>p->in->image_size || 0xb8>p->in->image_size-b->table)return rpd_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    if(!rpd_read(p,p->in->image_base,b->holder,&object,8))return 0;
    if(object!=p->in->image_base+b->object)return rpd_fail(p,SH_DEFAULT_READ_CALLBACK,p->in->image_base+b->holder);
    if(!rpd_read(p,object,0,&table,8) || !rpd_read(p,table,0xb0,&setter,8))return 0;
    if(table!=p->in->image_base+b->table || setter!=p->in->image_base+b->setter)
        return rpd_fail(p,SH_DEFAULT_READ_CALLBACK,table);
    /* The exact seven-byte setter writes only its own numeric parser state.
     * Existing state is legal; the native caller performs the 0/15 bracket. */
    if(!rpd_read(p,object,0x2c34,&state,4) || !rpd_read(p,p->in->image_base,b->holder,&again,8))return 0;
    if(again!=object)return rpd_fail(p,SH_DEFAULT_READ_INVOCATION,p->in->image_base+b->holder);
    return 1;
}
int sh_decl_renderprog_default_read_begin(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    rpd_probe p={r,in};uintptr_t table,parser,getter,text;unsigned char literal[RPD_DEFINITION_LENGTH],digest[32];sh_native_pin_hash hash;
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    if(in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->resource ||
       !in->default_callback || !in->image_base || !in->image_size || in->image_size>UINTPTR_MAX-in->image_base ||
       r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return rpd_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    const rpd_profile *b=rpd_profiles+in->renderer;
    if(!rpd_read(&p,in->resource,0,&table,8) || !rpd_read(&p,table,0x98,&parser,8))return 0;
    if(parser!=in->image_base+b->parse)return 0;
    if(!rpd_read(&p,table,0x88,&getter,8))return 0;
    if(getter!=in->image_base+b->getter)return rpd_fail(&p,SH_DEFAULT_READ_CALLBACK,getter);
    for(unsigned i=0;i<rpd_pin_counts[in->renderer];i++)if(!rpd_pin_check(&p,rpd_pins[in->renderer]+i))return 0;
    if(!sh_resource_graph_native_decl_flow_ready(in->image_base+b->reload,in->image_base+b->set_text,in->image_base+b->compile) ||
       !sh_resource_renderprog_reads_ready() || !sh_resource_parm_reads_ready())return rpd_fail(&p,SH_DEFAULT_READ_CALLBACK,parser);
    if(!sh_decl_default_envelope_read_check(r,in,&text) || !rpd_read(&p,text,0,literal,sizeof literal))return 0;
    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,literal,sizeof literal);sh_native_pin_hash_finish(&hash,digest);
    if(memcmp(digest,rpd_definition_hash,sizeof digest))return rpd_fail(&p,SH_DEFAULT_READ_PIN,text);
    /* Effective native defines, cache selection and actual consumed resources
     * remain authoritative. No identifier, package or shader-name exclusion. */
    if(!sh_decl_source_preprocess_read_check(r,in) || !sh_decl_renderprog_file_read_check(r,in) ||
       !sh_decl_renderprog_context_read_check(r,in) || !sh_decl_renderprog_cvar_read_check(r,in) ||
       !sh_decl_renderprog_release_read_check(r,in) || !sh_decl_renderprog_backend_read_check(r,in) ||
       !sh_decl_renderprog_manager_read_check(r,in) || !rpd_parser_state(&p,b))return 0;
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION;r->complete=1;r->reason=SH_DEFAULT_READ_OK;return 1;
}
