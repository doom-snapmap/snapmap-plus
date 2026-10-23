#include <limits.h>
#include <stdint.h>
#include <string.h>
#include "decl_string_default_reads.h"
#include "decl_default_reads.h"
#include "decl_diagnostic_reads.h"

typedef struct sdr_pin { uint32_t rva; unsigned length; unsigned char bytes[20]; } sdr_pin;
#include "decl_string_default_read_pins.h"
typedef struct sdr_probe { sh_resource_default_read_receipt *r; const sh_resource_default_read_input *in; } sdr_probe;
static int sdr_fail(sdr_probe *p,sh_resource_default_read_reason why,uintptr_t at)
{ p->r->reason=why; p->r->offending_address=at; return 0; }
static int sdr_read(sdr_probe *p,uintptr_t at,size_t off,void *out,size_t n)
{
    if (!at || off>UINTPTR_MAX-at || n>UINTPTR_MAX-(at+off)) return sdr_fail(p,SH_DEFAULT_READ_BOUNDS,at);
    if (p->in->work_limit && p->r->work>=p->in->work_limit) return sdr_fail(p,SH_DEFAULT_READ_BUDGET,at+off);
    p->r->work++;
    return p->in->read(p->in->read_context,at+off,out,n) || sdr_fail(p,SH_DEFAULT_READ_MEMORY,at+off);
}
static uintptr_t sdr_address(sdr_probe *p,unsigned pin)
{ return p->in->image_base+sdr_pins[p->in->renderer][pin].rva; }
static int sdr_pin_check(sdr_probe *p,unsigned index)
{
    unsigned char bytes[20]; const sdr_pin *pin=sdr_pins[p->in->renderer]+index;
    if (pin->rva>p->in->image_size || pin->length>p->in->image_size-pin->rva)
        return sdr_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
    return sdr_read(p,sdr_address(p,index),0,bytes,pin->length) &&
        (!memcmp(bytes,pin->bytes,pin->length) || sdr_fail(p,SH_DEFAULT_READ_PIN,sdr_address(p,index)));
}
static unsigned char sdr_lower(unsigned char c)
{ return c>='A' && c<='Z' ? (unsigned char)(c+('a'-'A')):c; }
static uint32_t sdr_hash(const unsigned char *text)
{
    uint32_t h=UINT32_C(2166136261);
    while (*text) h=h*UINT32_C(16777619)^sdr_lower(*text++);
    return h;
}
static int sdr_prefix(const unsigned char *text,const char *prefix)
{
    unsigned i; int uncertain=0;
    for (i=0;i<5;i++) {
        if (!text[i]) return 0;
        if (text[i]>=128) uncertain=1;
        else if (sdr_lower(text[i])!=(unsigned char)prefix[i]) return 0;
    }
    /* Native FindText uses CRT toupper. Non-ASCII locale-dependent comparisons
     * remain two possible branches; neither is assumed to match ASCII. */
    return uncertain?2:1;
}
static uint32_t sdr_normalized_hash(const unsigned char *source)
{
    unsigned char text[256]; size_t i,n=strlen((const char *)source),first=0,last;
    memcpy(text,source,n+1);
    for (i=0;i<n;i++) { text[i]=sdr_lower(text[i]); if (text[i]=='\\') text[i]='/'; }
    /* NormalizePath then StripPath leaves the last component. Removing leading
     * separators before that step cannot change the resulting component. */
    for (i=0;i<n;i++) if (text[i]=='/') first=i+1;
    memmove(text,text+first,n-first+1); n-=first;
    last=n;
    for (i=0;i<n;i++) if (text[i]=='.') last=i;
    n=last; text[n]=0;
    /* Native static idStr refuses a prefix insertion exceeding its capacity;
     * it leaves the previous bounded component unchanged. */
    if (n+6<=sizeof text) { memmove(text+5,text,n+1); memcpy(text,"play_",5); }
    /* Both replacement calls now have no '/' to match after StripPath. */
    return sdr_hash(text);
}
static int sdr_sound_name(sdr_probe *p)
{
    uintptr_t name; unsigned char text[256],c; size_t length=0; int a,b,has_slash=0,warning=0;
    uint32_t initial,normalized; static const uint32_t suppress[2]={0x4b21b00,0x62237f0};
    if (!sdr_read(p,p->in->resource,8,&name,sizeof name)) return 0;
    memset(text,0,sizeof text);
    if (name) for (;;) {
        if (!sdr_read(p,name,length,&c,1)) return 0;
        if (!c) break;
        if (length<sizeof text-1) text[length]=c;
        if (length==INT_MAX) return sdr_fail(p,SH_DEFAULT_READ_BOUNDS,name);
        length++;
    }
    /* The actual native call first assigns to a fixed256-byte local string.
     * Oversized names leave that initially empty string unchanged. This is a
     * reached callsite fact, not a declaration-name or package limit. */
    if (length>=sizeof text) text[0]=0;
    for (length=0;text[length];length++) if (text[length]=='/') has_slash=1;
    initial=sdr_hash(text); normalized=sdr_normalized_hash(text);
    a=sdr_prefix(text,"play_"); b=sdr_prefix(text,"stop_");
    if (has_slash || (!a && !b)) warning=normalized==0;
    else if (a==1 || b==1) warning=initial==0;
    else warning=initial==0 || normalized==0;
    if (warning) {
        int disabled; uint32_t rva=suppress[p->in->renderer];
        if (rva>p->in->image_size || sizeof disabled>p->in->image_size-rva)
            return sdr_fail(p,SH_DEFAULT_READ_IMAGE,p->in->image_base);
        if (!sdr_read(p,p->in->image_base,rva,&disabled,sizeof disabled)) return 0;
        if (!disabled && !sh_decl_diagnostic_read_check(p->r,p->in,4)) return 0;
    }
    return 1;
}
static const char sdr_viseme_definition[]=
    "{\n\tphonemeSet \"\"\n\tvisemeSilence viseme_REST\n\tphonemeSilence x\n"
    "\tvisemes {\n\t}\n\tphonemes {\n\t}\n}\n";
static int sdr_viseme(sdr_probe *p,uintptr_t table)
{
    static const char *const names[]={"phonemeSet","visemeSilence","viseme_REST",
        "phonemeSilence","x","visemes","phonemes"};
    static const unsigned shared[]={SDR_COPY_CONSTRUCT,SDR_STRING_INIT,SDR_STRING_DTOR};
    unsigned char definition[sizeof sdr_viseme_definition]; uintptr_t getter,text;
    if (!sdr_read(p,table,0x88,&getter,sizeof getter)) return 0;
    if (getter!=sdr_address(p,SDR_V_TEXT)) return sdr_fail(p,SH_DEFAULT_READ_UNSUPPORTED,getter);
    for (unsigned i=SDR_V_PARSER;i<SDR_PIN_COUNT;i++) if (!sdr_pin_check(p,i)) return 0;
    for (unsigned i=0;i<sizeof shared/sizeof shared[0];i++) if (!sdr_pin_check(p,shared[i])) return 0;
    if (!sh_decl_default_envelope_read_check(p->r,p->in,&text) ||
        !sdr_read(p,text,0,definition,sizeof definition)) return 0;
    if (memcmp(definition,sdr_viseme_definition,sizeof definition))
        return sdr_fail(p,SH_DEFAULT_READ_PIN,text);
    if (!sh_decl_default_names_unshadowed(p->r,p->in,names,sizeof names/sizeof names[0])) return 0;
    /* Release clears both counts for every native allocation-storage flag.
     * Its only indirect calls free native-owned blocks through the qualified
     * allocator. The fixed empty sections never enter element construction;
     * final lookups see zero counts and do not read any element names.
     * phonemeSet stores the empty token in the resource's own idStr; both
     * silence labels fit freshly constructed local inline strings. */
    return 1;
}
int sh_decl_string_default_read_begin(sh_resource_default_read_receipt *r,
                                      const sh_resource_default_read_input *in)
{
    sdr_probe p={r,in}; uintptr_t table,parser,release; unsigned i;
    if (!r || !in || r->state!=1 || r->complete || r->reason!=SH_DEFAULT_READ_UNSUPPORTED ||
        in->renderer<0 || in->renderer>1 || !in->pinned_image || !in->read || !in->image_base ||
        !in->image_size || in->image_size>UINTPTR_MAX-in->image_base) return 0;
    if (r->invocation.image_base!=in->image_base || r->invocation.image_size!=in->image_size ||
        r->invocation.renderer!=in->renderer || r->invocation.pinned_image!=in->pinned_image ||
        r->invocation.resource!=in->resource || r->invocation.default_callback!=in->default_callback ||
        r->invocation.read!=in->read || r->invocation.read_context!=in->read_context ||
        r->invocation.work_limit!=in->work_limit)
        return sdr_fail(&p,SH_DEFAULT_READ_INPUT,in->resource);
    if (!sdr_read(&p,in->resource,0,&table,sizeof table) || !sdr_read(&p,table,0x98,&parser,sizeof parser) ||
        !sdr_read(&p,table,0xa0,&release,sizeof release)) return 0;
    if (parser==sdr_address(&p,SDR_PARSER) && release==sdr_address(&p,SDR_RELEASE)) {
        if (!sh_decl_default_common_body_read_check(r,in)) return 0;
        for (i=0;i<SDR_V_PARSER;i++) if (!sdr_pin_check(&p,i)) return 0;
        if (!sdr_sound_name(&p)) return 0;
    } else if (parser==sdr_address(&p,SDR_V_PARSER) && release==sdr_address(&p,SDR_V_RELEASE)) {
        if (!sdr_viseme(&p,table)) return 0;
    } else return 0;
    r->routine=SH_DEFAULT_READ_ROUTINE_DECLARATION; r->complete=1; r->reason=SH_DEFAULT_READ_OK;
    return 1;
}
