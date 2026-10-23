#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <string.h>
#include "resource_material_reads.h"
#include "resource_graph.h"
#include "resource_graph_native.h"
#include "resource_instances.h"
#include "host_image.h"
#include "hook.h"
#include "native_pin_hash.h"

static int mm_digest(const unsigned char *bytes,size_t length,const unsigned char expected[32])
{
    sh_native_pin_hash hash;unsigned char actual[32];
    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,bytes,length);
    sh_native_pin_hash_finish(&hash,actual);return !memcmp(actual,expected,sizeof actual);
}

enum { MM_COPY, MM_FLAGS, MM_HAS, MM_SORT, MM_VIDEO, MM_ROUTES };
typedef struct mm_route {
    uint32_t rva; size_t length; unsigned char stolen, relocation, offset;
    const unsigned char *digest,*tail_digest;
} mm_route;
typedef struct mm_image {
    uint32_t parser; size_t parser_length; const unsigned char *parser_digest;
    uint32_t copy_return,integer_return,has_returns[3];
    uint32_t sort_return,sort_slot,uber_slot,video_return;
    mm_route routes[MM_ROUTES];
} mm_image;
#include "resource_material_read_pins.h"

typedef int (*mm_reader)(void *,uintptr_t,void *,size_t);
typedef struct mm_generation { void *resource; uint64_t token; } mm_generation;
typedef struct mm_probe {
    mm_reader read; void *context; int (*observe)(void *);
    uint64_t hash; size_t remaining; int failed,matched;
    mm_generation generations[9]; size_t count;
} mm_probe;
typedef void (*mm_copy_fn)(void *,void *);
typedef int (*mm_integer_fn)(void *,void *,uintptr_t);
typedef unsigned char (*mm_has_fn)(void *,void *);
typedef float (*mm_sort_fn)(void *,void *,uintptr_t);
typedef void *(*mm_video_fn)(void *,void *,uintptr_t);
static const uint8_t *mm_base;
static int mm_renderer;
static int (*mm_observe)(void *);
static mm_copy_fn mm_copy_original;
static mm_integer_fn mm_integer_original;
static mm_has_fn mm_has_original;
static mm_sort_fn mm_sort_original;
static mm_video_fn mm_video_original;
static volatile LONG mm_ready,mm_attempted;

static int mm_native_read(void *context,uintptr_t address,void *out,size_t length)
{
    (void)context;
    __try { memcpy(out,(const void *)address,length);return 1; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void mm_unknown(void)
{
    /* A broken observer cannot replace the native call with its exception.
     * The NULL notification also poisons an instance-only active owner. */
    int notified=0;
    __try { if(mm_observe) { (void)mm_observe(NULL);notified=1; } }
    __except(EXCEPTION_EXECUTE_HANDLER) { notified=0; }
    if(!notified) {
        __try { (void)sh_resource_graph_native_observe_resource(NULL); }
        __except(EXCEPTION_EXECUTE_HANDLER) { sh_resource_graph_incomplete(); }
    }
}
static mm_probe mm_begin(mm_reader read,void *context,int (*observe)(void *))
{
    mm_probe p={0};p.read=read;p.context=context;p.observe=observe;
    p.hash=UINT64_C(14695981039346656037);p.remaining=64u*1024u*1024u;
    if(!read) p.failed=1;return p;
}
static int mm_read(mm_probe *p,uintptr_t object,size_t offset,void *out,size_t length)
{
    if(p->failed || !object || object>UINTPTR_MAX-offset || length>p->remaining ||
       length>UINTPTR_MAX-(object+offset) || !p->read(p->context,object+offset,out,length)) {
        p->failed=1;return 0;
    }
    const unsigned char *bytes=out;
    for(size_t i=0;i<length;i++) { p->hash^=bytes[i];p->hash*=UINT64_C(1099511628211); }
    p->remaining-=length;return 1;
}
static uintptr_t mm_pointer(mm_probe *p,uintptr_t object,size_t offset)
{ uintptr_t value=0;mm_read(p,object,offset,&value,sizeof value);return value; }
static int32_t mm_integer(mm_probe *p,uintptr_t object,size_t offset)
{ int32_t value=0;mm_read(p,object,offset,&value,sizeof value);return value; }
static void mm_bytes(mm_probe *p,uintptr_t object,size_t length)
{
    unsigned char bytes[128];
    while(length && !p->failed) {
        size_t part=length<sizeof bytes?length:sizeof bytes;
        if(!mm_read(p,object,0,bytes,part)) break;
        object+=part;length-=part;
    }
}
static void mm_resource(mm_probe *p,uintptr_t object)
{
    if(p->failed) return;
    if(!object) { p->failed=1;return; }
    for(size_t i=0;i<p->count;i++) if(p->generations[i].resource==(void *)object) return;
    if(p->count==sizeof p->generations/sizeof *p->generations) { p->failed=1;return; }
    uint64_t prior=sh_resource_instances_token((void *)object);
    if(p->observe) {
        int ok=0;
        __try { ok=p->observe((void *)object); }
        __except(EXCEPTION_EXECUTE_HANDLER) { ok=0; }
        if(!ok) { p->failed=1;return; }
    }
    uint64_t token=sh_resource_instances_token((void *)object);
    if(!token || (prior && prior!=token)) { p->failed=1;return; }
    p->generations[p->count++]=(mm_generation){(void *)object,token};
}
static void mm_copy(mm_probe *p,uintptr_t source)
{
    if(source<0xe8) { p->failed=1;return; }
    mm_resource(p,source-0xe8);
    int32_t count=mm_integer(p,source,8);
    if(count<=0 || p->failed) return;
    uintptr_t ops=mm_pointer(p,source,0),values=mm_pointer(p,source,0x18);
    if((size_t)count>SIZE_MAX/16) { p->failed=1;return; }
    /* Native Copy uses the operation count for both arrays. It never consults
     * source value-count/capacity; do not invent that extra precondition. */
    mm_bytes(p,ops,(size_t)count*8);mm_bytes(p,values,(size_t)count*16);
}
static void mm_flags(mm_probe *p,uintptr_t block)
{
    if(block<0x70) { p->failed=1;return; }
    uintptr_t material=block-0x70,program=mm_pointer(p,material,0x110);
    int skip_second=0;
    if(program) {
        mm_resource(p,program);
        skip_second=(mm_integer(p,program,0x1688)&8)!=0;
    }
    if(!skip_second) {
        program=mm_pointer(p,material,0x108);
        if(program) { mm_resource(p,program);mm_integer(p,program,0x1688); }
    }
    /* The pinned continuation next visits exactly these final nine fields.
     * Before its first HasParm call it reads each nonnull program+e0. There
     * is no intervening call, program-pointer write, exit or skipped element;
     * HasParm itself is pinned call-free/read-only. This actual field-flow
     * contract is required in addition to the instruction CFG dominator. */
    for(size_t i=0;i<9 && !p->failed;i++) {
        program=mm_pointer(p,material,0xf0+i*8);
        if(program) { uint64_t flags=0;mm_resource(p,program);mm_read(p,program,0xe0,&flags,sizeof flags); }
    }
}
static void mm_has(mm_probe *p,uintptr_t program,uintptr_t definition)
{
    mm_resource(p,program);
    int index_read=0;int32_t index=0;
    for(size_t stage=0;stage<3 && !p->failed;stage++) for(size_t list=0;list<2 && !p->failed;list++) {
        size_t offset=0x118+(stage*2+list)*0xd8;
        int32_t count=mm_integer(p,program,offset+8);
        if(count<=0 || p->failed) continue;
        uintptr_t values=mm_pointer(p,program,offset);
        for(int32_t i=0;i<count && !p->failed;i++) {
            int16_t value=0;
            if((size_t)i>SIZE_MAX/sizeof value || !mm_read(p,values,(size_t)i*sizeof value,&value,sizeof value)) break;
            if(!index_read) {
                mm_resource(p,definition);index=mm_integer(p,definition,0x68);index_read=1;
            }
            if(p->failed) break;
            if((int32_t)value==index) { p->matched=1;return; }
        }
    }
}
static void mm_sort(mm_probe *p,uintptr_t block,float value)
{
    /* Do not perform another FP comparison in the observer. Native UCOMISS
     * accepts unordered values, honors DAZ, and can trap for an unmasked
     * signaling NaN/denormal before any body read. Preserve its FP state and
     * leave that exception at the native continuation. */
    uint32_t bits;memcpy(&bits,&value,sizeof bits);
    uint32_t magnitude=bits&0x7fffffff,exponent=bits&0x7f800000;
    if(!magnitude) return;
    unsigned control=_mm_getcsr();
    if(!exponent && (control&0x40)) return;
    if((exponent==0x7f800000 && (bits&0x7fffff) && !(bits&0x400000) && !(control&0x80)) ||
       (!exponent && !(control&0x100))) { p->failed=1;return; }
    if(block<0x70) { p->failed=1;return; }
    uintptr_t program=mm_pointer(p,block-0x70,0x110);
    if(!program || p->failed) return;
    const mm_image *image=mm_images+mm_renderer;
    uintptr_t uber=mm_pointer(p,(uintptr_t)mm_base,image->uber_slot);
    if(p->failed || program==uber) return; /* Identity comparison only. */
    mm_resource(p,program);uint64_t flags=0;
    if(!mm_read(p,program,0xe0,&flags,sizeof flags)) return;
    if((flags&0x38) || (flags&(UINT64_C(1)<<17)) ||
       ((flags&0x40) && (flags&0x6000)!=0x6000)) return;
    uint32_t coverage=0;
    mm_read(p,program,0x168c,&coverage,sizeof coverage);
    uintptr_t threshold=mm_pointer(p,(uintptr_t)mm_base,image->sort_slot);
    mm_resource(p,threshold);mm_read(p,threshold,0x58,&coverage,sizeof coverage);
}
static void mm_video(mm_probe *p,uintptr_t definition,uintptr_t value)
{
    if(!value)return; /* The caller does not dereference a null string. */
    /* GetOps has already reached this actual definition's default before any
     * own constant replacement. Retain it before the returned string byte.
     * A nonempty result selects unqualified Bink construction. Never rely on
     * an earlier preprobe of definition+58 remaining the selected result. */
    unsigned char c=0;mm_resource(p,definition);
    if(mm_read(p,value,0,&c,1) && c)p->failed=1;
}
static void mm_probe_kind(mm_probe *p,int kind,void *a,void *b)
{
    if(kind==MM_COPY) mm_copy(p,(uintptr_t)b);
    else if(kind==MM_FLAGS) mm_flags(p,(uintptr_t)a);
    else if(kind==MM_HAS) mm_has(p,(uintptr_t)a,(uintptr_t)b);
    else if(kind==MM_SORT) { float value;memcpy(&value,b,sizeof value);mm_sort(p,(uintptr_t)a,value); }
    else if(kind==MM_VIDEO) mm_video(p,(uintptr_t)a,(uintptr_t)b);
    else p->failed=1;
}
static void mm_collect(int kind,void *a,void *b,mm_probe *out)
{
    *out=mm_begin(mm_native_read,NULL,mm_observe);mm_probe_kind(out,kind,a,b);
    if(out->failed) mm_unknown();
}
static int mm_generations_equal(const mm_probe *a,const mm_probe *b)
{
    if(a->count!=b->count) return 0;
    for(size_t i=0;i<a->count;i++) if(a->generations[i].resource!=b->generations[i].resource ||
        a->generations[i].token!=b->generations[i].token) return 0;
    return 1;
}
static void mm_finish(int kind,void *a,void *b,const mm_probe *before,int returned,int result)
{
    if(!returned || before->failed || !sh_resource_material_reads_ready()) { mm_unknown();return; }
    mm_probe after=mm_begin(mm_native_read,NULL,NULL);mm_probe_kind(&after,kind,a,b);
    if(after.failed || before->hash!=after.hash || !mm_generations_equal(before,&after) ||
       (kind==MM_HAS && before->matched!=(result!=0))) mm_unknown();
}
static int mm_caller(int kind,uintptr_t caller)
{
    if(!mm_base || mm_renderer<0 || mm_renderer>1) return 0;
    const mm_image *image=mm_images+mm_renderer;
    if(kind==MM_COPY) return caller==(uintptr_t)mm_base+image->copy_return;
    if(kind==MM_FLAGS) return caller==(uintptr_t)mm_base+image->integer_return;
    if(kind==MM_SORT) return caller==(uintptr_t)mm_base+image->sort_return;
    if(kind==MM_VIDEO) return caller==(uintptr_t)mm_base+image->video_return;
    for(size_t i=0;i<3;i++) if(caller==(uintptr_t)mm_base+image->has_returns[i]) return 1;
    return 0;
}
static void mm_copy_at(void *destination,void *source,uintptr_t caller)
{
    int active=sh_resource_graph_native_observing() && mm_caller(MM_COPY,caller),returned=0;mm_probe p;
    if(active) {
        if(sh_resource_material_reads_ready()) mm_collect(MM_COPY,destination,source,&p);
        else { p=mm_begin(mm_native_read,NULL,NULL);p.failed=1;mm_unknown(); }
    }
    __try { mm_copy_original(destination,source);returned=1; }
    __finally { if(active) mm_finish(MM_COPY,destination,source,&p,returned,0); }
}
static int mm_integer_at(void *block,void *definition,uintptr_t fallback,uintptr_t caller)
{
    /* This is a reached return witness: a failed native GetInteger has not
     * reached the caller's subsequent program flags and observes none. */
    int result=mm_integer_original(block,definition,fallback);
    if(sh_resource_graph_native_observing() && mm_caller(MM_FLAGS,caller)) {
        if(!sh_resource_material_reads_ready()) mm_unknown();
        else { mm_probe p;mm_collect(MM_FLAGS,block,NULL,&p);mm_finish(MM_FLAGS,block,NULL,&p,1,0); }
    }
    return result;
}
static unsigned char mm_has_at(void *program,void *definition,uintptr_t caller)
{
    int active=sh_resource_graph_native_observing() && mm_caller(MM_HAS,caller),returned=0;
    unsigned char result=0;mm_probe p;
    if(active) {
        if(sh_resource_material_reads_ready()) mm_collect(MM_HAS,program,definition,&p);
        else { p=mm_begin(mm_native_read,NULL,NULL);p.failed=1;mm_unknown(); }
    }
    __try { result=mm_has_original(program,definition);returned=1; }
    __finally { if(active) mm_finish(MM_HAS,program,definition,&p,returned,result); }
    return result;
}
static float mm_sort_at(void *block,void *definition,uintptr_t fallback,uintptr_t caller)
{
    float value=mm_sort_original(block,definition,fallback);
    if(sh_resource_graph_native_observing() && mm_caller(MM_SORT,caller)) {
        if(!sh_resource_material_reads_ready()) mm_unknown();
        else { mm_probe p;mm_collect(MM_SORT,block,&value,&p);mm_finish(MM_SORT,block,&value,&p,1,0); }
    }
    return value;
}
static void *mm_video_at(void *block,void *definition,uintptr_t fallback,uintptr_t caller)
{
    void *value=mm_video_original(block,definition,fallback);
    if(sh_resource_graph_native_observing() && mm_caller(MM_VIDEO,caller)) {
        if(!sh_resource_material_reads_ready())mm_unknown();
        else { mm_probe p;mm_collect(MM_VIDEO,definition,value,&p);mm_finish(MM_VIDEO,definition,value,&p,1,0); }
    }
    return value;
}
static void mm_copy_hook(void *a,void *b) { mm_copy_at(a,b,(uintptr_t)_ReturnAddress()); }
static int mm_integer_hook(void *a,void *b,uintptr_t c) { return mm_integer_at(a,b,c,(uintptr_t)_ReturnAddress()); }
static unsigned char mm_has_hook(void *a,void *b) { return mm_has_at(a,b,(uintptr_t)_ReturnAddress()); }
static float mm_sort_hook(void *a,void *b,uintptr_t c) { return mm_sort_at(a,b,c,(uintptr_t)_ReturnAddress()); }
static void *mm_video_hook(void *a,void *b,uintptr_t c) { return mm_video_at(a,b,c,(uintptr_t)_ReturnAddress()); }
int sh_resource_material_reads_install(const uint8_t *base,int (*observe)(void *))
{
    if(InterlockedCompareExchange(&mm_attempted,1,0)) return sh_resource_material_reads_ready();
    int renderer=sh_host_is_vulkan();
    if(!base || !observe || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer)) return 0;
    const mm_image *image=mm_images+renderer;
    __try {
        if(!mm_digest(base+image->parser,image->parser_length,image->parser_digest)) return 0;
        for(size_t i=0;i<MM_ROUTES;i++) if(!mm_digest(base+image->routes[i].rva,image->routes[i].length,image->routes[i].digest)) return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    mm_base=base;mm_renderer=renderer;mm_observe=observe;
    void *detours[]={mm_copy_hook,mm_integer_hook,mm_has_hook,mm_sort_hook,mm_video_hook},*prepared[MM_ROUTES]={0};
    for(size_t i=0;i<MM_ROUTES;i++) {
        const mm_route *r=image->routes+i;void *target=(void *)(base+r->rva);
        prepared[i]=r->relocation?hook_prepare_rip_load(target,detours[i],r->stolen,r->offset):hook_prepare(target,detours[i],r->stolen);
        if(!prepared[i]) goto failed;
    }
    mm_copy_original=(mm_copy_fn)prepared[MM_COPY];mm_integer_original=(mm_integer_fn)prepared[MM_FLAGS];mm_has_original=(mm_has_fn)prepared[MM_HAS];
    mm_sort_original=(mm_sort_fn)prepared[MM_SORT];
    mm_video_original=(mm_video_fn)prepared[MM_VIDEO];
    for(size_t i=0;i<MM_ROUTES;i++) if(hook_commit(prepared[i])!=B2_PATCH_OK) goto failed;
    InterlockedExchange(&mm_ready,1);return 1;
failed:
    /* Failed unpatch retains the still-reachable trampoline callback. */
    for(size_t i=MM_ROUTES;i;i--) if(prepared[i-1]) (void)hook_unpatch(prepared[i-1]);
    return 0;
}
int sh_resource_material_reads_ready(void)
{
    if(!InterlockedCompareExchange(&mm_ready,0,0) || !mm_base ||
       sh_host_is_vulkan()!=mm_renderer || !sh_host_is_pinned_renderer_build(mm_renderer)) return 0;
    const mm_image *image=mm_images+mm_renderer;
    void *trampolines[]={mm_copy_original,mm_integer_original,mm_has_original,mm_sort_original,mm_video_original};
    void *detours[]={mm_copy_hook,mm_integer_hook,mm_has_hook,mm_sort_hook,mm_video_hook};
    __try {
        if(!mm_digest(mm_base+image->parser,image->parser_length,image->parser_digest)) return 0;
        for(size_t i=0;i<MM_ROUTES;i++) {
            static const unsigned char jump[6]={0xff,0x25,0,0,0,0};void *target=NULL;
            const mm_route *r=image->routes+i;const unsigned char *entry=mm_base+r->rva;
            if(!trampolines[i] || !hook_is_installed(trampolines[i]) || memcmp(entry,jump,6)) return 0;
            memcpy(&target,entry+6,8);if(target!=detours[i]) return 0;
            for(size_t j=14;j<r->stolen;j++) if(entry[j]!=0x90) return 0;
            if(!mm_digest(entry+r->stolen,r->length-r->stolen,r->tail_digest)) return 0;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
#ifdef SH_RESOURCE_MATERIAL_READS_TESTING
int sh_resource_material_reads_test_probe(int kind,void *a,void *b,sh_resource_material_reader read,void *context,int (*observe)(void *))
{
    mm_probe p=mm_begin(read,context,observe);mm_probe_kind(&p,kind,a,b);
    if(p.failed && observe) {
        __try { (void)observe(NULL); } __except(EXCEPTION_EXECUTE_HANDLER) { sh_resource_graph_incomplete(); }
    }
    return !p.failed;
}
void sh_resource_material_reads_test_reset(void)
{
    mm_base=NULL;mm_observe=NULL;mm_copy_original=NULL;mm_integer_original=NULL;mm_has_original=NULL;mm_sort_original=NULL;mm_video_original=NULL;
    InterlockedExchange(&mm_ready,0);InterlockedExchange(&mm_attempted,0);
}
#endif

