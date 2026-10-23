#include <stdint.h>
#include <string.h>
#include "decl_custom_default_reads.h"
#include "decl_default_reads.h"
#include "decl_default_allocator.h"
#include "decl_string_default_reads.h"
#include "decl_numeric_default_reads.h"
#include "decl_animweb_default_reads.h"
#include "decl_particle_default_reads.h"
#include "decl_material_default_reads.h"
#include "decl_md6_default_reads.h"
#include "decl_renderprog_default_reads.h"
#include "decl_diagnostic_reads.h"
#include "resource_joint_reads.h"

typedef struct cdr_pin { uint32_t rva; unsigned length; unsigned char bytes[20]; } cdr_pin;
#include "decl_custom_default_read_pins.h"
enum { CDR_COMMON, CDR_FLARE_RELEASE, CDR_VOICE_RELEASE, CDR_WALK_RELEASE,
       CDR_FX_PARSER, CDR_FX_RELEASE, CDR_FX_POST, CDR_SKINS_PARSER,
       CDR_SKINS_RELEASE, CDR_SKINS_TEXT, CDR_ENTITY_PARSER, CDR_ENTITY_RELEASE,
       CDR_ENTITY_TEXT, CDR_FLAG_PARSER, CDR_FLAG_RELEASE, CDR_FLAG_TEXT,
       CDR_TOKEN_DELETE, CDR_FILE_DELETE, CDR_FILE_WRITE, CDR_FILE_PRINTF,
       CDR_DETAIL_PARSER, CDR_DETAIL_TEXT, CDR_BREAKABLE_PARSER, CDR_BREAKABLE_TEXT,
       CDR_CLOTH_PARSER, CDR_CLOTH_RELEASE, CDR_CLOTH_TEXT,
       CDR_FOLIAGE_PARSER, CDR_FOLIAGE_RESET, CDR_FOLIAGE_TEXT,
       CDR_READ_INTEGER, CDR_READ_FLOAT, CDR_DECL_FIND,
       CDR_AF_PARSER, CDR_AF_RELEASE, CDR_AF_TEXT, CDR_AF_SETTINGS,
       CDR_AF_BODY, CDR_AF_BODY_RELEASE, CDR_AF_CONSTRAINT_RELEASE,
       CDR_AF_BODY_CTOR, CDR_AF_BODY_DEFAULTS, CDR_AF_CONTENTS,
       CDR_AF_VECTOR, CDR_AF_MODEL_JOINTS, CDR_READ_BOOL,
       CDR_PIN_COUNT };
static const uint32_t cdr_token_tables[2]={0x1fedea8,0x1fd6228};
static const uint32_t cdr_file_tables[2]={0x27a5f60,0x278fd40};
typedef struct cdr_probe {
    sh_resource_default_read_receipt *receipt;
    const sh_resource_default_read_input *input;
} cdr_probe;
static int cdr_fail(cdr_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{ p->receipt->reason=why; p->receipt->offending_address=at; return 0; }
static int cdr_read(cdr_probe *p,uintptr_t at,size_t offset,void *out,size_t size)
{
    if (!at || offset>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+offset))
        return cdr_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if (!p->input->read(p->input->read_context,at+offset,out,size))
        return cdr_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
    p->receipt->work++;
    return 1;
}
static uintptr_t cdr_address(const cdr_probe *p,unsigned which)
{ return p->input->image_base+cdr_pins[p->input->renderer][which].rva; }
static int cdr_pin_check(cdr_probe *p,unsigned which)
{
    unsigned char bytes[20];
    const cdr_pin *pin=cdr_pins[p->input->renderer]+which;
    uintptr_t at=cdr_address(p,which);
    if (pin->rva>p->input->image_size || pin->length>p->input->image_size-pin->rva)
        return cdr_fail(p,SH_DEFAULT_READ_IMAGE,at);
    if (!cdr_read(p,at,0,bytes,pin->length)) return 0;
    return !memcmp(bytes,pin->bytes,pin->length) || cdr_fail(p,SH_DEFAULT_READ_PIN,at);
}
static int cdr_null(cdr_probe *p,size_t offset)
{
    uintptr_t pointer;
    return cdr_read(p,p->input->resource,offset,&pointer,sizeof pointer) &&
        (!pointer || cdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,p->input->resource+offset));
}
static int cdr_image_pointer(cdr_probe *p,size_t rva,uintptr_t *out)
{
    if (rva>p->input->image_size || sizeof *out>p->input->image_size-rva)
        return cdr_fail(p,SH_DEFAULT_READ_IMAGE,p->input->image_base);
    return cdr_read(p,p->input->image_base,rva,out,sizeof *out);
}
static int cdr_dispatch(cdr_probe *p,uint32_t table,unsigned slot,unsigned function)
{
    uintptr_t actual;
    if (!cdr_image_pointer(p,(size_t)table+slot,&actual) || !cdr_pin_check(p,function)) return 0;
    return actual==cdr_address(p,function) ||
        cdr_fail(p,SH_DEFAULT_READ_CALLBACK,p->input->image_base+table+slot);
}
static int cdr_literal(cdr_probe *p,uintptr_t table,unsigned pin,const char *expected,size_t size)
{
    unsigned char bytes[128]; uintptr_t actual,text;
    if (!cdr_pin_check(p,pin) ||
        !cdr_read(p,table,0x88,&actual,sizeof actual)) return 0;
    if (actual!=cdr_address(p,pin)) return cdr_fail(p,SH_DEFAULT_READ_UNSUPPORTED,actual);
    if (!sh_decl_default_envelope_read_check(p->receipt,p->input,&text)) return 0;
    for (size_t offset=0;offset<size;) {
        size_t count=size-offset;
        if (count>sizeof bytes) count=sizeof bytes;
        if (!cdr_read(p,text,offset,bytes,count)) return 0;
        if (memcmp(bytes,expected+offset,count)) return cdr_fail(p,SH_DEFAULT_READ_PIN,text+offset);
        offset+=count;
    }
    return 1;
}
static int cdr_named_constant(cdr_probe *p,size_t row,size_t rva,
                              unsigned expected,const char *name,size_t length)
{
    uintptr_t actual; unsigned subtype; char text[32];
    if (row>p->input->image_size || 16>p->input->image_size-row ||
        rva>p->input->image_size || length>sizeof text || length>p->input->image_size-rva)
        return cdr_fail(p,SH_DEFAULT_READ_IMAGE,p->input->image_base);
    if (!cdr_image_pointer(p,row,&actual) ||
        !cdr_read(p,p->input->image_base,row+8,&subtype,sizeof subtype)) return 0;
    if (actual!=p->input->image_base+rva)
        return cdr_fail(p,SH_DEFAULT_READ_CALLBACK,p->input->image_base+row);
    if (!cdr_read(p,actual,0,text,length)) return 0;
    return (subtype==expected && !memcmp(text,name,length)) ||
        cdr_fail(p,SH_DEFAULT_READ_PIN,p->input->image_base+row);
}
static int cdr_input_valid(cdr_probe *p)
{
    sh_resource_default_read_receipt *r=p->receipt;
    const sh_resource_default_read_input *in=p->input;
    if (!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED ||
        in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read ||
        !in->image_base || !in->image_size || in->image_size>UINTPTR_MAX-in->image_base)
        return 0;
    /* Continue only the request whose begin established this receipt. No raw
     * struct comparison: padding is not part of invocation identity. */
    if (r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
        r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
        r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
        r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
        r->invocation.work_limit!=in->work_limit) return cdr_fail(p,SH_DEFAULT_READ_INPUT,in->resource);
    return 1;
}
int sh_decl_custom_default_token_read_check(sh_resource_default_read_receipt *r,
                                             const sh_resource_default_read_input *in)
{
    cdr_probe p={r,in};
    if (!cdr_input_valid(&p) || !sh_decl_default_token_read_check(r,in)) return 0;
    if (in->work_limit && r->work>in->work_limit) return cdr_fail(&p,SH_DEFAULT_READ_BUDGET,in->resource);
    return 1;
}
int sh_decl_custom_default_read_begin(sh_resource_default_read_receipt *r,
                                     const sh_resource_default_read_input *in)
{
    cdr_probe p={r,in};
    uintptr_t table,parser,release;
    unsigned selected;
    if (!cdr_input_valid(&p)) return 0;
    if (!cdr_read(&p,in->resource,0,&table,sizeof table) ||
        !cdr_read(&p,table,0x98,&parser,sizeof parser) ||
        !cdr_read(&p,table,0xa0,&release,sizeof release)) return 0;
    if (parser==cdr_address(&p,CDR_COMMON)) {
        for (selected=CDR_FLARE_RELEASE;selected<=CDR_WALK_RELEASE;selected++)
            if (release==cdr_address(&p,selected)) break;
        if (selected>CDR_WALK_RELEASE) return 0;
    } else if (parser==cdr_address(&p,CDR_FX_PARSER) && release==cdr_address(&p,CDR_FX_RELEASE))
        selected=CDR_FX_RELEASE;
    else if (parser==cdr_address(&p,CDR_SKINS_PARSER) && release==cdr_address(&p,CDR_SKINS_RELEASE))
        selected=CDR_SKINS_RELEASE;
    else if (parser==cdr_address(&p,CDR_ENTITY_PARSER) && release==cdr_address(&p,CDR_ENTITY_RELEASE))
        selected=CDR_ENTITY_RELEASE;
    else if (parser==cdr_address(&p,CDR_FLAG_PARSER) && release==cdr_address(&p,CDR_FLAG_RELEASE))
        selected=CDR_FLAG_RELEASE;
    else if (parser==cdr_address(&p,CDR_DETAIL_PARSER) && release==cdr_address(&p,CDR_FLAG_RELEASE))
        selected=CDR_DETAIL_PARSER;
    else if (parser==cdr_address(&p,CDR_BREAKABLE_PARSER) && release==cdr_address(&p,CDR_FLAG_RELEASE))
        selected=CDR_BREAKABLE_PARSER;
    else if (parser==cdr_address(&p,CDR_CLOTH_PARSER) && release==cdr_address(&p,CDR_CLOTH_RELEASE))
        selected=CDR_CLOTH_RELEASE;
    else if (parser==cdr_address(&p,CDR_FOLIAGE_PARSER) && release==cdr_address(&p,CDR_FLAG_RELEASE))
        selected=CDR_FOLIAGE_PARSER;
    else if (parser==cdr_address(&p,CDR_AF_PARSER) && release==cdr_address(&p,CDR_AF_RELEASE))
        selected=CDR_AF_RELEASE;
    else {
        if (sh_decl_string_default_read_begin(r,in)) return 1;
        if (r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
        if (sh_decl_numeric_default_read_begin(r,in)) return 1;
        if (r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
        if (sh_decl_animweb_default_read_begin(r,in)) return 1;
        if (r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
        if (sh_decl_particle_default_read_begin(r,in)) return 1;
        if (r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
        if (sh_decl_material_default_read_begin(r,in)) return 1;
        if (r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
        if (sh_decl_md6_default_read_begin(r,in)) return 1;
        if (r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
        return sh_decl_renderprog_default_read_begin(r,in);
    }
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION;
    if (selected==CDR_AF_RELEASE) {
        for (unsigned i=CDR_AF_PARSER;i<=CDR_READ_BOOL;i++)
            if (!cdr_pin_check(&p,i)) return 0;
        if (!cdr_pin_check(&p,CDR_READ_FLOAT) || !cdr_pin_check(&p,CDR_DECL_FIND) ||
            !cdr_literal(&p,table,CDR_AF_TEXT,cdr_af_definition,sizeof cdr_af_definition) ||
            !sh_decl_default_names_unshadowed(r,in,cdr_af_names,sizeof cdr_af_names/sizeof cdr_af_names[0]) ||
            !sh_decl_diagnostic_read_check(r,in,4)) return 0;
        if (!sh_resource_joint_reads_ready()) return cdr_fail(&p,SH_DEFAULT_READ_CALLBACK,in->resource);
        /* Release destroys owned bodies/constraints through raw frees and
         * sized delete, then resets both counts. Fixed settings/body values
         * use own numeric/vector/contents storage. Native Find still records
         * its actual returned MD6 declaration; NULL reaches the qualified
         * warning. GetJointList observes the actual model and skeleton only
         * at its reached name-token witness, including missing-joint warnings. */
    } else if (selected==CDR_SKINS_RELEASE) {
        if (!cdr_pin_check(&p,CDR_SKINS_PARSER) || !cdr_pin_check(&p,CDR_SKINS_RELEASE) ||
            !cdr_literal(&p,table,CDR_SKINS_TEXT,"{\n}\n",5) || !cdr_null(&p,0x58)) return 0;
        /* Release resets count+60, and the parser's first token is the closing
         * brace. Neither mapping bodies nor material lookups are reached. */
    } else if (selected==CDR_CLOTH_RELEASE || selected==CDR_FOLIAGE_PARSER) {
        static const char *const cloth_names[]={"material","_default","width","height","hspacing","vspacing"};
        static const char *const foliage_names[]={"material","foliage/default","quadWidth","quadHeight","shape","autosprites"};
        int foliage=selected==CDR_FOLIAGE_PARSER;
        if (!cdr_pin_check(&p,foliage?CDR_FOLIAGE_PARSER:CDR_CLOTH_PARSER) ||
            !cdr_pin_check(&p,foliage?CDR_FLAG_RELEASE:CDR_CLOTH_RELEASE) ||
            !cdr_pin_check(&p,CDR_READ_INTEGER) || !cdr_pin_check(&p,CDR_DECL_FIND) ||
            !cdr_literal(&p,table,foliage?CDR_FOLIAGE_TEXT:CDR_CLOTH_TEXT,
                foliage?cdr_foliage_definition:cdr_cloth_definition,
                foliage?sizeof cdr_foliage_definition:sizeof cdr_cloth_definition) ||
            !sh_decl_default_names_unshadowed(r,in,foliage?foliage_names:cloth_names,6)) return 0;
        if (foliage) {
            if (!cdr_pin_check(&p,CDR_FOLIAGE_RESET) ||
                !cdr_named_constant(&p,cdr_shape_rows[in->renderer],cdr_shape_text[in->renderer],
                                     0,"autosprites",sizeof "autosprites")) return 0;
        } else if (!cdr_pin_check(&p,CDR_READ_FLOAT)) return 0;
        /* Cleanup/reset only frees owned raw storage through the qualified
         * allocator for every native storage flag; no element callback.
         * Fixed decimal12/64 tokens take local digit accumulation. The sole
         * material Find stores its returned identity, observed on cached/miss
         * paths by the required Lookup/Materialize hooks at request finish. */
    } else if (selected==CDR_BREAKABLE_PARSER) {
        if (!cdr_pin_check(&p,CDR_BREAKABLE_PARSER) || !cdr_pin_check(&p,CDR_FLAG_RELEASE) ||
            !cdr_literal(&p,table,CDR_BREAKABLE_TEXT,cdr_breakable_definition,sizeof cdr_breakable_definition) ||
            !cdr_named_constant(&p,cdr_brace_rows[in->renderer],cdr_brace_text[in->renderer],47,"}",2)) return 0;
        /* Generic MakeDefault already consumed the opening brace. Exact empty
         * text makes CheckTokenType(5,47) succeed by peeking/unreading '}', so
         * reset, model lookup and missing-model diagnostics are not reached. */
    } else if (selected==CDR_DETAIL_PARSER) {
        static const char *const names[]={"model","_default"};
        if (!cdr_pin_check(&p,CDR_DETAIL_PARSER) || !cdr_pin_check(&p,CDR_FLAG_RELEASE) ||
            !cdr_literal(&p,table,CDR_DETAIL_TEXT,cdr_detail_definition,sizeof cdr_detail_definition) ||
            !sh_decl_default_names_unshadowed(r,in,names,sizeof names/sizeof names[0])) return 0;
        /* Both fixed names use SourceReadToken. With exact define shadows
         * excluded, model only stores the interned _default bytes in own+58.
         * No model lookup or colorVariance branch is reached. */
    } else if (selected==CDR_ENTITY_RELEASE || selected==CDR_FLAG_RELEASE) {
        int entity=selected==CDR_ENTITY_RELEASE;
        static const char *const names[]={"inherit","class","edit"};
        unsigned parser_pin=entity?CDR_ENTITY_PARSER:CDR_FLAG_PARSER;
        if (!cdr_pin_check(&p,parser_pin) || !cdr_pin_check(&p,selected) ||
            !cdr_literal(&p,table,entity?CDR_ENTITY_TEXT:CDR_FLAG_TEXT,
                entity?cdr_entity_definition:cdr_flag_definition,
                entity?sizeof cdr_entity_definition:sizeof cdr_flag_definition) ||
            !cdr_dispatch(&p,cdr_token_tables[in->renderer],0,CDR_TOKEN_DELETE)) return 0;
        if (entity && (!sh_decl_default_names_unshadowed(r,in,names,sizeof names/sizeof names[0]) ||
            !cdr_dispatch(&p,cdr_file_tables[in->renderer],0,CDR_FILE_DELETE) ||
            !cdr_dispatch(&p,cdr_file_tables[in->renderer],0x30,CDR_FILE_WRITE) ||
            !cdr_dispatch(&p,cdr_file_tables[in->renderer],0x88,CDR_FILE_PRINTF))) return 0;
        /* EntityDef's empty parent excludes lookup/copy; both expansion modes
         * use owned text. Expansion builds one empty edit node in a fresh raw
         * KV tree, then prints/frees it through constructed memory-file/token
         * types. RenderProgFlag reads constant integer zero and stores+58. */
    } else {
        if (!sh_decl_default_common_body_read_check(r,in) || !cdr_pin_check(&p,CDR_COMMON) ||
            !cdr_pin_check(&p,selected)) return 0;
        if (selected==CDR_FX_RELEASE) {
            if (!cdr_pin_check(&p,CDR_FX_PARSER) || !cdr_pin_check(&p,CDR_FX_POST) ||
                !cdr_null(&p,0x68) || !cdr_null(&p,0x80)) return 0;
            /* Release and the parser reset stage count; post resets cooked count
             * before both positive-count loops. Neither element callbacks nor the
             * material registry are reached by the empty common body. */
        } else if (!cdr_null(&p,selected==CDR_VOICE_RELEASE?0x70:0x68)) return 0;
    }
    if (in->work_limit && r->work>in->work_limit)
        return cdr_fail(&p,SH_DEFAULT_READ_BUDGET,in->resource);
    r->complete=1; r->reason=SH_DEFAULT_READ_OK;
    return 1;
}
