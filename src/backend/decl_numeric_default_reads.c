#include <stdint.h>
#include <string.h>
#include "decl_numeric_default_reads.h"
#include "decl_default_reads.h"
#include "resource_parm_reads.h"

typedef struct ndp_pin { uint32_t rva; unsigned length; unsigned char bytes[20]; } ndp_pin;
enum { NDP_TABLE, NDP_TABLE_RELEASE, NDP_TABLE_TEXT, NDP_PARM, NDP_PARM_RELEASE,
       NDP_PARM_TEXT, NDP_ENV, NDP_ENV_RELEASE, NDP_ENV_TEXT, NDP_PINS };
/* NUMERIC_NATIVE_CONTRACTS_BEGIN */
static const ndp_pin ndp_pins[2][NDP_PINS]={
{
    {0x17ae970,20,{0x48,0x8b,0xc4,0x55,0x57,0x41,0x56,0x48,0x8d,0xa8,0xd8,0xfe,0xff,0xff,0x48,0x81,0xec,0x10,0x02,0x00}},
    {0x17ae890,9,{0x48,0x8b,0x49,0x60,0xe9,0x57,0x22,0x2a,0x00}},
    {0x17ae880,8,{0x48,0x8d,0x05,0x29,0x5e,0xf6,0x00,0xc3}},
    {0xd7a440,20,{0x48,0x8b,0xc4,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x48,0xfd,0xff,0xff,0x48,0x81}},
    {0x83e380,3,{0xc2,0x00,0x00}},
    {0xd7a3e0,8,{0x48,0x8d,0x05,0xf1,0xaf,0x4c,0x01,0xc3}},
    {0x17a1cb0,20,{0x40,0x55,0x56,0x57,0x48,0x8d,0xac,0x24,0x00,0xf8,0xff,0xff,0x48,0x81,0xec,0x00,0x09,0x00,0x00,0x48}},
    {0x17a1ca0,9,{0x48,0x83,0xc1,0x58,0xe9,0x87,0x5c,0x60,0xff}},
    {0x17a1c90,8,{0x48,0x8d,0x05,0xe9,0xd6,0xf6,0x00,0xc3}}
},
{
    {0x17bbe60,20,{0x48,0x8b,0xc4,0x55,0x57,0x41,0x56,0x48,0x8d,0xa8,0xd8,0xfe,0xff,0xff,0x48,0x81,0xec,0x10,0x02,0x00}},
    {0x17bbd80,9,{0x48,0x8b,0x49,0x60,0xe9,0x87,0x28,0x2a,0x00}},
    {0x17bbd70,8,{0x48,0x8d,0x05,0xc9,0x2e,0xf4,0x00,0xc3}},
    {0xd7a200,20,{0x48,0x8b,0xc4,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x48,0xfd,0xff,0xff,0x48,0x81}},
    {0x165fa90,3,{0xc2,0x00,0x00}},
    {0xd7a1a0,8,{0x48,0x8d,0x05,0xb1,0x2f,0x4b,0x01,0xc3}},
    {0x17af1d0,20,{0x40,0x55,0x56,0x57,0x48,0x8d,0xac,0x24,0x00,0xf8,0xff,0xff,0x48,0x81,0xec,0x00,0x09,0x00,0x00,0x48}},
    {0x17af1c0,9,{0x48,0x83,0xc1,0x58,0xe9,0x47,0x25,0x60,0xff}},
    {0x17af1b0,8,{0x48,0x8d,0x05,0x59,0xa7,0xf4,0x00,0xc3}}
}
};
static const uint32_t ndp_default_rvas[2]={0x179de30,0x17ab340};
static const uint32_t ndp_managers[2]={0x3e5e248,0x555bb90};
static const uint32_t ndp_parm_tables[2]={0x2245318,0x222d098};
static const uint32_t ndp_temporary_heads[2]={0x413e280,0x58407d0};
static const uint32_t ndp_env_handles[2][2]={{0x36fb4f8,0x36fb518},{0x4df90c8,0x4df90e8}};
static const char ndp_table_definition[]=
"{ clamp { 0 } }\n";
static const char ndp_parm_definition[]=
"{ vec 0 }\n";
static const char ndp_env_definition[]=
"{\n"
"\trenderParms {\n"
"\t\tenvBlendTime\t\t\t\t\t\t\t1.2\n"
"\t\tfogColor\t\t\t\t\t\t\t\t{ 0.0, 0.0, 0.0, 0.0 }\n"
"\t\tfogScale\t\t\t\t\t\t\t\t0\n"
"\t\tfogStart\t\t\t\t\t\t\t\t0.0\n"
"\t\tfogEnd\t\t\t\t\t\t\t\t\t32000.0\n"
"\t\tfogLightOppositeColor\t\t\t\t\t{ 0.749020, 0.858824, 1.0, 1.0 }\n"
"\t\tfogLightInscatterColor\t\t\t\t\t{ 1.0, 0.870588, 0.721569, 1.0 }\n"
"\t\tfogLightInscatterIntensity\t\t\t\t1.0\n"
"\t\tfogStartDistance\t\t\t\t\t\t1024.0\n"
"\t\tfogTerminatorAngle\t\t\t\t\t\t50.0\n"
"\t\tfogDensity\t\t\t\t\t\t\t\t0.02\n"
"\t\tfogMaxOpacity\t\t\t\t\t\t\t0.0\n"
"\t\tfogHeightFalloff\t\t\t\t\t\t2.0\n"
"\t\ttransparencyLightMult\t\t\t\t\t0.15\n"
"\t\tflaresMult\t\t\t\t\t\t\t\t0.15\n"
"\t\tfogVolumeMult\t\t\t\t\t\t\t0.15\n"
"\t\tparticleMult\t\t\t\t\t\t\t0.15\n"
"\t\tdecalMult\t\t\t\t\t\t\t\t0.15\n"
"\t\tfilmicCurveHighlightsScale\t\t\t\t1.0\n"
"\t\tfilmicCurveMidtonesScale\t\t\t\t1.0\n"
"\t\tfilmicCurveShadowsScale\t\t\t\t\t1.0\n"
"\t\tfilmicCurveWhitePoint\t\t\t\t\t1.0\n"
"\t\tautoExposureMax\t\t\t\t\t\t\t16.0\n"
"\t\tautoExposureMin\t\t\t\t\t\t\t1.0\n"
"\t\tHDRSaturation\t\t\t\t\t\t\t1.0\n"
"\t\tHDRColorFilter\t\t\t\t\t\t\t{ 1, 1, 1, 1 }\n"
"\t\tmaxViewDistance\t\t\t\t\t\t\t8192.0\n"
"\t\tchromaticAberrationScale\t\t\t\t0.0\n"
"\t\tchromaticAberrationCurve\t\t\t\t0.0\n"
"\t\tvignetteScale\t\t\t\t\t\t\t0.0\n"
"\t\tvignetteCurve\t\t\t\t\t\t\t1.0\n"
"\t\tvignetteColor\t\t\t\t\t\t\t{ 0.0, 0.0, 0.0 }\n"
"\t\tbloomWeight0\t\t\t\t\t\t\t1.0\n"
"\t\tbloomWeight1\t\t\t\t\t\t\t1.0\n"
"\t\tbloomWeight2\t\t\t\t\t\t\t1.0\n"
"\t\tbloomWeight3\t\t\t\t\t\t\t1.0\n"
"\t\tbloomWeight4\t\t\t\t\t\t\t1.0\n"
"\t\tbloomWeight5\t\t\t\t\t\t\t1.0\n"
"\t\tbloomScale1\t\t\t\t\t\t\t\t0.0\n"
"\t\tbloomScale2\t\t\t\t\t\t\t\t0.0\n"
"\t\tbloomScale3\t\t\t\t\t\t\t\t0.0\n"
"\t\tbloomScale4\t\t\t\t\t\t\t\t0.0\n"
"\t\tbloomScale5\t\t\t\t\t\t\t\t0.0\n"
"\t\tfilmGrain\t\t\t\t\t\t\t\t0.014\n"
"\t\tcolorCorrectionShadowMidtoneStart\t\t0.3\n"
"\t\tcolorCorrectionShadowMidtoneEnd\t\t\t0.4\n"
"\t\tcolorCorrectionMidtoneHighlightStart\t0.6\n"
"\t\tcolorCorrectionMidtoneHighlightEnd\t\t0.7\n"
"\t\tcolorCorrectionMasterSaturation\t\t\t1.0\n"
"\t\tcolorCorrectionMasterGamma\t\t\t\t1.0\n"
"\t\tcolorCorrectionMasterGain\t\t\t\t1.0\n"
"\t\tcolorCorrectionMasterOffset\t\t\t\t0.0\n"
"\t\tcolorCorrectionMasterColor\t\t\t\t{ 1.0, 1.0, 1.0 }\n"
"\t\tcolorCorrectionShadowsSaturation\t\t1.0\n"
"\t\tcolorCorrectionShadowsGamma\t\t\t\t1.0\n"
"\t\tcolorCorrectionShadowsGain\t\t\t\t1.0\n"
"\t\tcolorCorrectionShadowsOffset\t\t\t0.0\n"
"\t\tcolorCorrectionShadowsColor\t\t\t\t{ 1.0, 1.0, 1.0 }\n"
"\t\tcolorCorrectionMidtonesSaturation\t\t1.0\n"
"\t\tcolorCorrectionMidtonesGamma\t\t\t1.0\n"
"\t\tcolorCorrectionMidtonesGain\t\t\t\t1.0\n"
"\t\tcolorCorrectionMidtonesOffset\t\t\t0.0\n"
"\t\tcolorCorrectionMidtonesColor\t\t\t{ 1.0, 1.0, 1.0 }\n"
"\t\tcolorCorrectionHighlightsSaturation\t\t1.0\n"
"\t\tcolorCorrectionHighlightsGamma\t\t\t1.0\n"
"\t\tcolorCorrectionHighlightsGain\t\t\t1.0\n"
"\t\tcolorCorrectionHighlightsOffset\t\t\t0.0\n"
"\t\tcolorCorrectionHighlightsColor\t\t\t{ 1.0, 1.0, 1.0 }\n"
"\t\tdiffuseOcclusionScale\t\t\t\t\t2.0\n"
"\t\tspecularOcclusionScale\t\t\t\t\t4.0\n"
"\t\tocclusionMaxViewDistance\t\t\t\t10000.0\n"
"\t\tssrScale\t\t\t\t\t\t\t\t1.0\n"
"\t\tssrSmoothnessThreshold\t\t\t\t\t0.3\n"
"\t\tshadowSliceStep\t\t\t\t\t\t\t512.0\n"
"\t\tshadowLogBase\t\t\t\t\t\t\t3.0\n"
"\t\tenvLightmapScale\t\t\t\t\t\t1.0\n"
"\t}\n"
"}\n";
enum { NDP_ENV_KEYS=77 };
static const char *const ndp_env_names[NDP_ENV_KEYS]={
    "renderParms",
    "envBlendTime",
    "fogColor",
    "fogScale",
    "fogStart",
    "fogEnd",
    "fogLightOppositeColor",
    "fogLightInscatterColor",
    "fogLightInscatterIntensity",
    "fogStartDistance",
    "fogTerminatorAngle",
    "fogDensity",
    "fogMaxOpacity",
    "fogHeightFalloff",
    "transparencyLightMult",
    "flaresMult",
    "fogVolumeMult",
    "particleMult",
    "decalMult",
    "filmicCurveHighlightsScale",
    "filmicCurveMidtonesScale",
    "filmicCurveShadowsScale",
    "filmicCurveWhitePoint",
    "autoExposureMax",
    "autoExposureMin",
    "HDRSaturation",
    "HDRColorFilter",
    "maxViewDistance",
    "chromaticAberrationScale",
    "chromaticAberrationCurve",
    "vignetteScale",
    "vignetteCurve",
    "vignetteColor",
    "bloomWeight0",
    "bloomWeight1",
    "bloomWeight2",
    "bloomWeight3",
    "bloomWeight4",
    "bloomWeight5",
    "bloomScale1",
    "bloomScale2",
    "bloomScale3",
    "bloomScale4",
    "bloomScale5",
    "filmGrain",
    "colorCorrectionShadowMidtoneStart",
    "colorCorrectionShadowMidtoneEnd",
    "colorCorrectionMidtoneHighlightStart",
    "colorCorrectionMidtoneHighlightEnd",
    "colorCorrectionMasterSaturation",
    "colorCorrectionMasterGamma",
    "colorCorrectionMasterGain",
    "colorCorrectionMasterOffset",
    "colorCorrectionMasterColor",
    "colorCorrectionShadowsSaturation",
    "colorCorrectionShadowsGamma",
    "colorCorrectionShadowsGain",
    "colorCorrectionShadowsOffset",
    "colorCorrectionShadowsColor",
    "colorCorrectionMidtonesSaturation",
    "colorCorrectionMidtonesGamma",
    "colorCorrectionMidtonesGain",
    "colorCorrectionMidtonesOffset",
    "colorCorrectionMidtonesColor",
    "colorCorrectionHighlightsSaturation",
    "colorCorrectionHighlightsGamma",
    "colorCorrectionHighlightsGain",
    "colorCorrectionHighlightsOffset",
    "colorCorrectionHighlightsColor",
    "diffuseOcclusionScale",
    "specularOcclusionScale",
    "occlusionMaxViewDistance",
    "ssrScale",
    "ssrSmoothnessThreshold",
    "shadowSliceStep",
    "shadowLogBase",
    "envLightmapScale"
};
/* NUMERIC_NATIVE_CONTRACTS_END */

typedef struct ndp_probe {
    sh_resource_default_read_receipt *receipt;
    const sh_resource_default_read_input *input;
} ndp_probe;
static int ndp_fail(ndp_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{ p->receipt->reason=why; p->receipt->offending_address=at; return 0; }
static int ndp_read(ndp_probe *p,uintptr_t at,size_t offset,void *out,size_t size)
{
    if(!at || offset>UINTPTR_MAX-at || size>UINTPTR_MAX-(at+offset))
        return ndp_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if(p->receipt->work==SIZE_MAX || (p->input->work_limit && p->receipt->work>=p->input->work_limit))
        return ndp_fail(p,SH_DEFAULT_READ_BUDGET,at+offset);
    if(!p->input->read(p->input->read_context,at+offset,out,size))
        return ndp_fail(p,SH_DEFAULT_READ_MEMORY,at+offset);
    p->receipt->work++; return 1;
}
static int ndp_image(ndp_probe *p,size_t rva,void *out,size_t size)
{
    if(rva>p->input->image_size || size>p->input->image_size-rva)
        return ndp_fail(p,SH_DEFAULT_READ_IMAGE,p->input->image_base);
    return ndp_read(p,p->input->image_base,rva,out,size);
}
static uintptr_t ndp_address(ndp_probe *p,unsigned which)
{ return p->input->image_base+ndp_pins[p->input->renderer][which].rva; }
static int ndp_pin_check(ndp_probe *p,unsigned which)
{
    unsigned char bytes[20]; const ndp_pin *pin=ndp_pins[p->input->renderer]+which;
    if(!ndp_image(p,pin->rva,bytes,pin->length)) return 0;
    return !memcmp(bytes,pin->bytes,pin->length) || ndp_fail(p,SH_DEFAULT_READ_PIN,ndp_address(p,which));
}
static int ndp_valid(ndp_probe *p)
{
    sh_resource_default_read_receipt *r=p->receipt;
    const sh_resource_default_read_input *in=p->input;
    if(!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED ||
       in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->resource ||
       !in->image_base || !in->image_size || in->image_size>UINTPTR_MAX-in->image_base) return 0;
    if(r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
       r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
       r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
       r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
       r->invocation.work_limit!=in->work_limit) return ndp_fail(p,SH_DEFAULT_READ_INPUT,in->resource);
    return in->default_callback==in->image_base+ndp_default_rvas[in->renderer];
}
static int ndp_literal(ndp_probe *p,uintptr_t table,unsigned which,const char *expected,size_t size)
{
    uintptr_t actual,text; unsigned char bytes[128];
    if(!ndp_pin_check(p,which) || !ndp_read(p,table,0x88,&actual,sizeof actual)) return 0;
    if(actual!=ndp_address(p,which)) return ndp_fail(p,SH_DEFAULT_READ_CALLBACK,actual);
    if(!sh_decl_default_envelope_read_check(p->receipt,p->input,&text)) return 0;
    for(size_t offset=0;offset<size;) {
        size_t count=size-offset; if(count>sizeof bytes) count=sizeof bytes;
        if(!ndp_read(p,text,offset,bytes,count)) return 0;
        if(memcmp(bytes,expected+offset,count)) return ndp_fail(p,SH_DEFAULT_READ_PIN,text+offset);
        offset+=count;
    }
    return 1; /* The shared envelope validates the constructed token callback. */
}
static unsigned char ndp_fold(unsigned char value)
{ return value>='A' && value<='Z' ? (unsigned char)(value+('a'-'A')) : value; }
/* Stop only after the actual prefix distinguishes every finite contract key.
 * This is not a truncation limit on native names or definition lists. */
static int ndp_key(ndp_probe *p,uintptr_t name,const char *const *keys,size_t count,int fold,int *match)
{
    unsigned char possible[NDP_ENV_KEYS]; size_t active=count,offset=0;
    if(count>NDP_ENV_KEYS) return ndp_fail(p,SH_DEFAULT_READ_INPUT,name);
    memset(possible,1,count); *match=-1;
    while(active) {
        unsigned char value;
        if(!ndp_read(p,name,offset,&value,1)) return 0;
        active=0;
        for(size_t i=0;i<count;i++) if(possible[i]) {
            unsigned char expected=(unsigned char)keys[i][offset];
            if((fold?ndp_fold(value):value)!=(fold?ndp_fold(expected):expected)) possible[i]=0;
            else if(!value) { *match=(int)i; return 1; }
            else active++;
        }
        offset++;
    }
    return 1;
}
static int ndp_manager(ndp_probe *p,uintptr_t *manager)
{
    int count; uintptr_t states;
    if(!ndp_image(p,ndp_managers[p->input->renderer],manager,sizeof *manager) ||
       !ndp_read(p,*manager,0x10190,&count,sizeof count) ||
       !ndp_read(p,*manager,0x10188,&states,sizeof states)) return 0;
    if(count<0 || (count && !states)) return ndp_fail(p,SH_DEFAULT_READ_OWNERSHIP,*manager+0x10188);
    /* SetDefault's actual per-state writer and rollback only copy numerical
     * values, counters, masks and owned op/rollback arrays. No copied pointer
     * is followed as a resource, and no dynamic callback is invoked. */
    for(size_t i=0;i<(size_t)count;i++) {
        uintptr_t state; uint32_t mask;
        if(!ndp_read(p,states,i*sizeof state,&state,sizeof state) ||
           !ndp_read(p,state,0xb97c,&mask,sizeof mask)) return 0;
    }
    return 1;
}
static int ndp_vector_metadata(ndp_probe *p,uintptr_t manager,uintptr_t object,int temporary)
{
    uintptr_t table,registered,error; unsigned char flags; int index,kind,usage;
    if(!ndp_read(p,object,0,&table,sizeof table) || !ndp_read(p,object,0x68,&index,sizeof index) ||
       !ndp_read(p,object,0x6c,&kind,sizeof kind)) return 0;
    if(table!=p->input->image_base+ndp_parm_tables[p->input->renderer])
        return ndp_fail(p,SH_DEFAULT_READ_TYPE,object);
    if(!ndp_read(p,object,0x18,&error,sizeof error) || !ndp_read(p,object,0x2c,&flags,1)) return 0;
    if(error || (flags&7)) return ndp_fail(p,SH_DEFAULT_READ_OWNERSHIP,object+0x18);
    if(kind || index<0 || index>=0x400) return ndp_fail(p,SH_DEFAULT_READ_TYPE,object+0x68);
    if(!ndp_read(p,manager,(size_t)index*sizeof registered,&registered,sizeof registered)) return 0;
    if(registered!=object) return ndp_fail(p,SH_DEFAULT_READ_OWNERSHIP,object+0x68);
    if(temporary && (!ndp_read(p,object,0x74,&usage,sizeof usage) ||
       (usage!=2 && !ndp_fail(p,SH_DEFAULT_READ_OWNERSHIP,object+0x74)))) return 0;
    return 1;
}
static int ndp_env(ndp_probe *p)
{
    uintptr_t manager,object,name,first;
    unsigned char found[NDP_ENV_KEYS]={0};
    if(!sh_resource_parm_reads_ready()) return ndp_fail(p,SH_DEFAULT_READ_CALLBACK,p->input->resource);
    if(!ndp_manager(p,&manager)) return 0;
    /* The literal's keys name fixed native vector declarations. Inspect the
     * actual registered objects rather than infer their kind from their names.
     * Materialize still records each actual lookup in the active native frame. */
    for(size_t i=0;i<0x93f;i++) {
        int match;
        if(!ndp_read(p,manager,i*sizeof object,&object,sizeof object)) return 0;
        if(!object) continue;
        if(!ndp_read(p,object,8,&name,sizeof name) ||
           !ndp_key(p,name,ndp_env_names+1,NDP_ENV_KEYS-1,1,&match)) return 0;
        if(match>=0) {
            if(!ndp_vector_metadata(p,manager,object,0)) return 0;
            found[match+1]=1;
        }
    }
    for(size_t i=1;i<NDP_ENV_KEYS;i++) if(!found[i])
        return ndp_fail(p,SH_DEFAULT_READ_OWNERSHIP,manager);
    if(!ndp_image(p,ndp_temporary_heads[p->input->renderer],&first,sizeof first)) return 0;
    if(first) for(size_t i=0;i<128;i++) {
        if(!ndp_image(p,(size_t)ndp_temporary_heads[p->input->renderer]+i*sizeof object,&object,sizeof object) ||
           !ndp_vector_metadata(p,manager,object,1)) return 0;
    }
    /* With first==NULL the native constructor Materializes all128 own work
     * declarations, marks each temporary, then emits numerical constant ops.
     * Existing Materialize observation supplies those actual returned sources. */
    for(size_t i=0;i<2;i++) {
        if(!ndp_image(p,ndp_env_handles[p->input->renderer][i],&object,sizeof object) ||
           !ndp_vector_metadata(p,manager,object,0)) return 0;
    }
    /* Both owned/buffer-borrowed old parm arrays are reset before parsing.
     * Their former counts and contents are not inputs. Clear/destruction only
     * free their own numeric storage via the validated common allocator. */
    unsigned char old_block[0x30];
    return ndp_read(p,p->input->resource,0x58,old_block,sizeof old_block);
}
int sh_decl_numeric_default_read_begin(sh_resource_default_read_receipt *r,
                                      const sh_resource_default_read_input *in)
{
    ndp_probe p={r,in}; uintptr_t table,parser,release,actual_default; unsigned selected;
    static const char *const table_keys[]={"clamp"},*const parm_keys[]={"vec"};
    if(!ndp_valid(&p) || !ndp_read(&p,in->resource,0,&table,sizeof table) ||
       !ndp_read(&p,table,0x98,&parser,sizeof parser) || !ndp_read(&p,table,0xa0,&release,sizeof release)) return 0;
    for(selected=0;selected<NDP_PINS;selected+=3) if(parser==ndp_address(&p,selected)) break;
    if(selected==NDP_PINS) return 0;
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION;
    if(!ndp_read(&p,table,0x50,&actual_default,sizeof actual_default)) return 0;
    if(actual_default!=in->default_callback) return ndp_fail(&p,SH_DEFAULT_READ_CALLBACK,actual_default);
    if(release!=ndp_address(&p,selected+1)) return ndp_fail(&p,SH_DEFAULT_READ_CALLBACK,release);
    if(!ndp_pin_check(&p,selected) || !ndp_pin_check(&p,selected+1)) return 0;
    if(selected==NDP_TABLE) {
        uintptr_t curve; unsigned char storage[0x220];
        if(!ndp_literal(&p,table,NDP_TABLE_TEXT,ndp_table_definition,sizeof ndp_table_definition) ||
           !sh_decl_default_names_unshadowed(r,in,table_keys,1) || !ndp_read(&p,in->resource,0x60,&curve,sizeof curve) ||
           !ndp_read(&p,curve,0,storage,sizeof storage)) return 0;
        /* Clear resets every consumed old curve flag/count before the one
         * zero sample. Sorting and clamping touch only inline float arrays. */
    } else if(selected==NDP_PARM) {
        uintptr_t manager; int index;
        if(!ndp_literal(&p,table,NDP_PARM_TEXT,ndp_parm_definition,sizeof ndp_parm_definition) ||
           !sh_decl_default_names_unshadowed(r,in,parm_keys,1) || !ndp_manager(&p,&manager) ||
           !ndp_read(&p,in->resource,0x68,&index,sizeof index)) return 0;
        if(index<0) return ndp_fail(&p,SH_DEFAULT_READ_BOUNDS,in->resource+0x68);
        /* The parser writes kind0 before registration/value parsing. An old
         * sentinel allocates a slot by immutable name identity; valid assigned
         * slots and all registered state writes remain numerical storage. */
    } else {
        if(!ndp_literal(&p,table,NDP_ENV_TEXT,ndp_env_definition,sizeof ndp_env_definition) ||
           !sh_decl_default_names_unshadowed(r,in,ndp_env_names,NDP_ENV_KEYS) || !ndp_env(&p)) return 0;
    }
    if(in->work_limit && r->work>in->work_limit) return ndp_fail(&p,SH_DEFAULT_READ_BUDGET,in->resource);
    r->complete=1; r->reason=SH_DEFAULT_READ_OK; return 1;
}
