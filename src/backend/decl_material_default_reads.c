#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "decl_material_default_reads.h"
#include "decl_default_reads.h"
#include "decl_diagnostic_reads.h"
#include "native_resource_id.h"
#include "resource_material_reads.h"
#include "resource_model_reads.h"
#include "resource_parm_reads.h"

typedef struct mdr_pin { uint32_t rva;unsigned length;unsigned char bytes[20]; } mdr_pin;
typedef struct mdr_parameter { const char *name;uint32_t slot[2];int kind; } mdr_parameter;
#include "decl_material_default_read_pins.h"
enum { MDR_PARAMETERS=sizeof mdr_parameters/sizeof mdr_parameters[0] };
typedef struct mdr_probe {
    sh_resource_default_read_receipt *r;
    const sh_resource_default_read_input *in;
    uintptr_t manager,list;
    uintptr_t objects[MDR_PARAMETERS];
    int indices[MDR_PARAMETERS];
} mdr_probe;
static const uint32_t mdr_holders[2]={0x3e5e248,0x555bb90};
static const uint32_t mdr_lists[2]={0x42bb960,0x59bd650};
static const uint32_t mdr_tables[2]={0x2245318,0x222d098};
static const uint32_t mdr_temporaries[2]={0x413e280,0x58407d0};
static const char mdr_definition[]=
"{\t// DEFAULTED\n\t// DO NOT EDIT TEXT DIRECTLY!\n"
"\ttransMap _default\n\tstageProgram guiBlend\n}\n";
static int mdr_fail(mdr_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{ p->r->reason=why;p->r->offending_address=at;return 0; }
static int mdr_read(mdr_probe *p,uintptr_t at,size_t off,void *out,size_t length)
{
    if(!at || off>UINTPTR_MAX-at || length>UINTPTR_MAX-(at+off))
        return mdr_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->r->work==SIZE_MAX || (p->in->work_limit && p->r->work>=p->in->work_limit))
        return mdr_fail(p,SH_DEFAULT_READ_BUDGET,at+off);
    p->r->work++;
    return p->in->read(p->in->read_context,at+off,out,length) ||
        mdr_fail(p,SH_DEFAULT_READ_MEMORY,at+off);
}
static int mdr_image(mdr_probe *p,size_t rva,void *out,size_t length)
{
    if(rva>p->in->image_size || length>p->in->image_size-rva)
        return mdr_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    return mdr_read(p,p->in->image_base,rva,out,length);
}
static uintptr_t mdr_address(mdr_probe *p,unsigned i)
{ return p->in->image_base+mdr_pins[p->in->renderer][i].rva; }
static int mdr_pin_check(mdr_probe *p,unsigned i)
{
    unsigned char bytes[20];const mdr_pin *pin=mdr_pins[p->in->renderer]+i;
    return mdr_image(p,pin->rva,bytes,pin->length) &&
        (!memcmp(bytes,pin->bytes,pin->length) || mdr_fail(p,SH_DEFAULT_READ_PIN,mdr_address(p,i)));
}
static unsigned char mdr_fold(unsigned char c)
{ return c>='A' && c<='Z' ? c+('a'-'A'):c; }
/* A finite expected key is compared through its terminating byte. An unrelated
 * longer native name is distinguished at that byte, without truncating it. */
static int mdr_name(mdr_probe *p,uintptr_t text,const char *expected,int *equal)
{
    *equal=0;
    for(size_t i=0;;i++) {
        unsigned char c;
        if(!mdr_read(p,text,i,&c,1))return 0;
        if(mdr_fold(c)!=mdr_fold((unsigned char)expected[i]))return 1;
        if(!c){*equal=1;return 1;}
    }
}
static int mdr_kind_index(int kind,int index)
{
    if(kind==0)return index>=0 && index<0x400;
    if(kind>=1 && kind<=9)return index>=0x400 && index<0x600;
    if(kind==10)return index>=0x600 && index<0x680;
    if(kind==11)return index>=0x680 && index<0x800;
    if(kind==12)return index>=0x800 && index<0x900;
    return kind==13 && index>=0x900 && index<0x93f;
}
static int mdr_metadata(mdr_probe *p,uintptr_t object,int wanted,int temporary,int *index)
{
    uintptr_t table,owner,error,registered;unsigned char flags;int kind,usage;
    if(!mdr_read(p,object,0,&table,sizeof table) ||
       !mdr_read(p,object,0x10,&owner,sizeof owner) ||
       !mdr_read(p,object,0x18,&error,sizeof error) ||
       !mdr_read(p,object,0x2c,&flags,1) ||
       !mdr_read(p,object,0x68,index,sizeof *index) ||
       !mdr_read(p,object,0x6c,&kind,sizeof kind))return 0;
    if(table!=p->in->image_base+mdr_tables[p->in->renderer] || owner!=p->list ||
       error || (flags&7))return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,object);
    if(!mdr_kind_index(kind,*index) || (wanted>=0 && kind!=wanted) ||
       (wanted==-2 && (kind<1 || kind>9)))
        return mdr_fail(p,SH_DEFAULT_READ_TYPE,object+0x68);
    if(!mdr_read(p,p->manager,(size_t)*index*sizeof registered,&registered,sizeof registered))return 0;
    if(registered!=object)return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,object+0x68);
    if(temporary) {
        if(!mdr_read(p,object,0x74,&usage,sizeof usage))return 0;
        if(usage!=2)return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,object+0x74);
    }
    return 1;
}
/* Reproduce only the reached cached ResourceIndexForID selection. Every link
 * is checked against the actual native resource-array count; this is not a
 * traversal-depth cutoff. A cycle or malformed link cannot qualify a prefix.
 * Native still performs its own lookup and supplies actual origin evidence. */
static int mdr_lookup(mdr_probe *p,uintptr_t wanted,const char *key)
{
    uintptr_t array,buckets,next,type;uint32_t mask,bucket_mask,index,id,actual;
    int count,equal;char type_name[sizeof "renderParm"];
    if(!sh_native_resource_id("renderParm",key,&id))
        return mdr_fail(p,SH_DEFAULT_READ_ALLOCATION,wanted);
    if(!mdr_read(p,p->list,8,&type,sizeof type) ||
       !mdr_read(p,type,0,type_name,sizeof type_name))return 0;
    if(memcmp(type_name,"renderParm",sizeof type_name))return mdr_fail(p,SH_DEFAULT_READ_TYPE,type);
    if(!mdr_read(p,wanted,0x20,&actual,sizeof actual))return 0;
    if(actual!=id)return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,wanted+0x20);
    if(!mdr_read(p,p->list,0x20,&array,sizeof array) ||
       !mdr_read(p,p->list,0x28,&count,sizeof count) ||
       !mdr_read(p,p->list,0x38,&buckets,sizeof buckets) ||
       !mdr_read(p,p->list,0x40,&next,sizeof next) ||
       !mdr_read(p,p->list,0x54,&bucket_mask,sizeof bucket_mask) ||
       !mdr_read(p,p->list,0x58,&mask,sizeof mask))return 0;
    uint32_t bucket=mask&bucket_mask&id;
    if(count<0 || bucket>INT_MAX)return mdr_fail(p,SH_DEFAULT_READ_BOUNDS,p->list);
    if(!mdr_read(p,buckets,(size_t)bucket*4,&index,sizeof index))return 0;
    for(size_t step=0;index!=UINT32_MAX && step<(size_t)count;step++) {
        uintptr_t object,name;
        if(index>INT_MAX || index>=(uint32_t)count)return mdr_fail(p,SH_DEFAULT_READ_BOUNDS,array);
        if(!mdr_read(p,array,(size_t)index*sizeof object,&object,sizeof object) ||
           !mdr_read(p,object,0x20,&actual,sizeof actual))return 0;
        if(actual==id) {
            if(!mdr_read(p,object,8,&name,sizeof name))return 0;
            if(!name)return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,object);
            if(!mdr_name(p,name,key,&equal))return 0;
            /* A collision warning has its own invocation contract. Never
             * silently step over an unqualified mutable diagnostic branch. */
            if(!equal || object!=wanted)return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,object);
            return 1;
        }
        uint32_t link=mask&index;
        if(link>INT_MAX)return mdr_fail(p,SH_DEFAULT_READ_BOUNDS,next);
        if(!mdr_read(p,next,(size_t)link*4,&index,sizeof index))return 0;
    }
    return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,p->list);
}
static int mdr_parameters_ready(mdr_probe *p)
{
    uintptr_t first;
    if(!mdr_image(p,mdr_holders[p->in->renderer],&p->manager,sizeof p->manager))return 0;
    p->list=p->in->image_base+mdr_lists[p->in->renderer];
    for(unsigned i=0;i<MDR_PARAMETERS;i++) {
        const mdr_parameter *k=mdr_parameters+i;uintptr_t name;int equal;
        if(!mdr_image(p,k->slot[p->in->renderer],p->objects+i,sizeof p->objects[i]) ||
           !mdr_metadata(p,p->objects[i],k->kind,0,p->indices+i) ||
           !mdr_read(p,p->objects[i],8,&name,sizeof name) ||
           !mdr_name(p,name,k->name,&equal))return 0;
        if(!equal)return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,name);
        for(unsigned j=0;j<i;j++)if(p->indices[i]==p->indices[j])
            return mdr_fail(p,SH_DEFAULT_READ_OWNERSHIP,p->objects[i]+0x68);
        if(!mdr_lookup(p,p->objects[i],k->name))return 0;
        if(!strcmp(k->name,"videoMap")) {
            uintptr_t value;unsigned char c;
            if(!mdr_read(p,p->objects[i],0x58,&value,sizeof value))return 0;
            if(value && (!mdr_read(p,value,0,&c,1) ||
               (c && !mdr_fail(p,SH_DEFAULT_READ_UNSUPPORTED,value))))return 0;
        }
    }
    if(!mdr_image(p,mdr_temporaries[p->in->renderer],&first,sizeof first))return 0;
    if(first)for(unsigned i=0;i<128;i++) {
        uintptr_t object;int index;
        if(!mdr_image(p,(size_t)mdr_temporaries[p->in->renderer]+i*sizeof object,&object,sizeof object) ||
           !mdr_metadata(p,object,0,1,&index))return 0;
    }
    /* A null first entry selects native initialization of all128 temporary
     * vector declarations. Actual Materialize returns remain observed, as in
     * the qualified Env parser; no synthetic temporary count is invented. */
    return 1;
}
int sh_decl_material_default_read_begin(sh_resource_default_read_receipt *r,
                                        const sh_resource_default_read_input *in)
{
    mdr_probe p={0};uintptr_t table,parser,release,getter,manager,text,bink;
    unsigned char literal[sizeof mdr_definition],owned[0x30];
    static const char *const names[]={"transMap","_default","stageProgram","guiBlend"};
    p.r=r;p.in=in;
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED ||
       in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->resource ||
       !in->image_base || !in->image_size || in->image_size>UINTPTR_MAX-in->image_base)return 0;
    if(r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit)return mdr_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    if(!mdr_read(&p,in->resource,0,&table,sizeof table) ||
       !mdr_read(&p,table,0x98,&parser,sizeof parser))return 0;
    if(parser!=mdr_address(&p,MDR_PARSE))return 0;
    if(!mdr_read(&p,table,0xa0,&release,sizeof release) ||
       !mdr_read(&p,table,0x88,&getter,sizeof getter) ||
       !mdr_read(&p,table,0x70,&manager,sizeof manager))return 0;
    if(release!=mdr_address(&p,MDR_RELEASE) || getter!=mdr_address(&p,MDR_TEXT) ||
       manager!=mdr_address(&p,MDR_MANAGER))return mdr_fail(&p,SH_DEFAULT_READ_CALLBACK,release);
    /* GetString is owned by the reached videoMap observer. Its readiness
     * validates the entire original body, exact current detour and suffix. */
    if(!sh_resource_material_reads_ready())return mdr_fail(&p,SH_DEFAULT_READ_CALLBACK,in->resource);
    for(unsigned i=0;i<MDR_PINS;i++)if(i!=MDR_STRING && !mdr_pin_check(&p,i))return 0;
    if(!sh_decl_default_envelope_read_check(r,in,&text) ||
       !mdr_read(&p,text,0,literal,sizeof literal))return 0;
    if(memcmp(literal,mdr_definition,sizeof literal))return mdr_fail(&p,SH_DEFAULT_READ_PIN,text);
    if(!sh_decl_default_names_unshadowed(r,in,names,4) ||
       !mdr_read(&p,in->resource,0x198,&bink,sizeof bink))return 0;
    if(bink)return mdr_fail(&p,SH_DEFAULT_READ_UNSUPPORTED,bink);
    /* Release disposes its own optional buffer and idStr; Reset clears both
     * parm arrays, cached profile and all nine program fields for every native
     * storage flag. Constructor calls that Reset, including +198==0. */
    if(!mdr_read(&p,in->resource,0x70,owned,sizeof owned) ||
       !mdr_read(&p,in->resource,0xa0,owned,sizeof owned) ||
       !mdr_parameters_ready(&p))return 0;
    if(!sh_resource_material_reads_ready() || !sh_resource_parm_reads_ready() ||
       !sh_resource_model_reads_default_ready("material"))
        return mdr_fail(&p,SH_DEFAULT_READ_CALLBACK,in->resource);
    if(!sh_decl_diagnostic_read_check(r,in,1) || !sh_decl_diagnostic_read_check(r,in,4))return 0;
    /* The two fixed ops are texture/program constants. Distinct actual
     * indices exclude all five atlas keys and profile's three map keys.
     * Particle remapping may append its three verified vector keys; their
     * indices also differ. String defaults stay native: table names may Find,
     * land-page strings use finite platform metadata/qualified VT bind; only
     * nonempty videoMap retains its open Bink contract. Arbitrary program
     * defaults and selected children are witnessed at actual reached fields,
     * never pre-observed by fixed literal name. */
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION;r->complete=1;r->reason=SH_DEFAULT_READ_OK;return 1;
}
