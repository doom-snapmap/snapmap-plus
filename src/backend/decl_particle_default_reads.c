#include <stdint.h>
#include <string.h>
#include "decl_particle_default_reads.h"
#include "decl_default_reads.h"
#include "decl_diagnostic_reads.h"
#include "resource_model_reads.h"
#include "resource_metadata_reads.h"
typedef struct pdr_pin { uint32_t rva;unsigned length;unsigned char bytes[20]; } pdr_pin;
#include "decl_particle_default_read_pins.h"
typedef struct pdr_probe { sh_resource_default_read_receipt *r;const sh_resource_default_read_input *in; } pdr_probe;
static int pdr_fail(pdr_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{ p->r->reason=why;p->r->offending_address=at;return 0; }
static int pdr_read(pdr_probe *p,uintptr_t at,size_t off,void *out,size_t length)
{
    if(!at || off>UINTPTR_MAX-at || length>UINTPTR_MAX-(at+off))return pdr_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->in->work_limit && p->r->work>=p->in->work_limit)return pdr_fail(p,SH_DEFAULT_READ_BUDGET,at+off);
    p->r->work++;return p->in->read(p->in->read_context,at+off,out,length) || pdr_fail(p,SH_DEFAULT_READ_MEMORY,at+off);
}
static uintptr_t pdr_address(pdr_probe *p,unsigned i) { return p->in->image_base+pdr_pins[p->in->renderer][i].rva; }
static int pdr_pin_check(pdr_probe *p,unsigned i)
{
    unsigned char bytes[20];const pdr_pin *pin=pdr_pins[p->in->renderer]+i;
    if(pin->rva>p->in->image_size || pin->length>p->in->image_size-pin->rva)return pdr_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    return pdr_read(p,pdr_address(p,i),0,bytes,pin->length) &&
        (!memcmp(bytes,pin->bytes,pin->length) || pdr_fail(p,SH_DEFAULT_READ_PIN,pdr_address(p,i)));
}
static int pdr_pointer(pdr_probe *p,uint32_t rva,uintptr_t *out)
{
    if(rva>p->in->image_size || sizeof *out>p->in->image_size-rva)return pdr_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    return pdr_read(p,p->in->image_base,rva,out,sizeof *out);
}
static unsigned char pdr_lower(unsigned char c) { return c>='A' && c<='Z' ? c+('a'-'A'):c; }
static int pdr_enum(pdr_probe *p,const char *expected,unsigned wanted)
{
    static const uint32_t tables[2]={0x36f9190,0x4df6bf0};
    /* Native enum lookup uses the first case-insensitive match. Later rows
     * are never reached; pointer identity and unrelated row spelling do not
     * decide the selected numeric/curve path. */
    for(unsigned i=0;i<=wanted;i++) {
        uintptr_t text;if(!pdr_pointer(p,tables[p->in->renderer]+i*8,&text))return 0;
        size_t j=0;int equal=1;
        for(;;j++) { unsigned char actual;if(!pdr_read(p,text,j,&actual,1))return 0;
            if(pdr_lower(actual)!=pdr_lower((unsigned char)expected[j])){equal=0;break;}
            if(!expected[j])break;
        }
        if(equal)return i==wanted || pdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,text);
    }
    return pdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,p->in->image_base+tables[p->in->renderer]);
}
static const char pdr_definition[]=
"{\n\t{\n\t\tmaterial\ttextures/effects/particles/default\n\t\tcount\t\t5\n"
"\t\ttime\t\tconstant 1.0\n\t\tspeedZ\t\tparametricIntegrate \"150.000\"\n\t}\n}\n";
int sh_decl_particle_default_read_begin(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{
    pdr_probe p={r,in};uintptr_t table,parser,release,getter,text,manager,manager_table,method;
    static const char *const names[]={"material","textures/effects/particles/default","count","time","constant","speedZ","parametricIntegrate"};
    static const uint32_t manager_holder[2]={0x36fb530,0x4df9100};
    static const uint32_t parent[2]={0x17a5190,0x17b26d0},dependency[2]={0x17a4f70,0x17b24b0};
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED ||
       in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->image_base ||
       !in->image_size || in->image_size>UINTPTR_MAX-in->image_base)return 0;
    if(r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context || r->invocation.work_limit!=in->work_limit)
        return pdr_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    if(!pdr_read(&p,in->resource,0,&table,sizeof table) || !pdr_read(&p,table,0x98,&parser,sizeof parser) ||
       !pdr_read(&p,table,0xa0,&release,sizeof release))return 0;
    if(parser!=pdr_address(&p,PDR_PARSER) || release!=pdr_address(&p,PDR_RELEASE))return 0;
    if(!pdr_read(&p,table,0x88,&getter,sizeof getter))return 0;
    if(getter!=pdr_address(&p,PDR_TEXT))return pdr_fail(&p,SH_DEFAULT_READ_UNSUPPORTED,getter);
    for(unsigned i=0;i<PDR_PIN_COUNT;i++)if(!pdr_pin_check(&p,i))return 0;
    if(!sh_decl_default_envelope_read_check(r,in,&text))return 0;
    unsigned char bytes[sizeof pdr_definition];
    if(!pdr_read(&p,text,0,bytes,sizeof bytes))return 0;
    if(memcmp(bytes,pdr_definition,sizeof bytes))return pdr_fail(&p,SH_DEFAULT_READ_PIN,text);
    if(!sh_decl_default_names_unshadowed(r,in,names,sizeof names/sizeof names[0]) ||
       !pdr_enum(&p,"constant",1) || !pdr_enum(&p,"parametricIntegrate",9))return 0;
    if(!pdr_pointer(&p,manager_holder[in->renderer],&manager) ||
       !pdr_read(&p,manager,0,&manager_table,sizeof manager_table) ||
       !pdr_read(&p,manager_table,0x98,&method,sizeof method))return 0;
    if(method!=in->image_base+parent[in->renderer])return pdr_fail(&p,SH_DEFAULT_READ_CALLBACK,method);
    if(!pdr_read(&p,manager_table,0x90,&method,sizeof method))return 0;
    if(method!=in->image_base+dependency[in->renderer])return pdr_fail(&p,SH_DEFAULT_READ_CALLBACK,method);
    if(!sh_resource_model_reads_default_ready("particle") || !sh_resource_metadata_reads_ready())
        return pdr_fail(&p,SH_DEFAULT_READ_CALLBACK,in->resource);
    if(!sh_decl_diagnostic_read_check(r,in,4))return 0;
    /* Release drops stage/curve counts for every native storage flag, with
     * allocator-only disposal and no element destructor callbacks. Reset
     * sets all parameter modes and clears mesh. Reused borrowed stages may
     * retain GPU bit5: both material branches are covered by the actual
     * material/program read witness, and implicit default material handles
     * are captured by the native parser observer. No fresh-buffer assumption.
     * The fixed literal selects scalar modes2/6; it cannot enter curve lookup.
     * Timestamp bookkeeping supplies no particle payload. File edges are
     * recorded only at actual Read; nested genuine reads remain observable. */
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION;r->complete=1;r->reason=SH_DEFAULT_READ_OK;return 1;
}
