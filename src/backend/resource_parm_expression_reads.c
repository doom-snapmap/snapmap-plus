#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "resource_parm_reads.h"
#include "resource_graph.h"
#include "resource_graph_native.h"
#include "resource_instances.h"
#include "host_image.h"
#include "hook.h"
#include "native_pin_hash.h"

typedef struct px_pin {uint32_t rva;size_t length,skip;unsigned char digest[2][32];} px_pin;
typedef struct px_region {uint32_t begin,end,unwind;} px_region;
typedef struct px_profile {uint32_t pool,manager,targets[4];size_t stolen[4];const px_pin *pins;size_t count;const px_region *regions;size_t region_count;} px_profile;
#include "resource_parm_expression_read_pins.h"
typedef void *(*px_get_fn)(int *);
typedef void *(*px_constant_fn)(int *,void *,const void *);
typedef void *(*px_binary_fn)(int *,void *,void *,void *,void *,uint16_t);
typedef void *(*px_recursive_fn)(int *,void *,int);
static void *px_original[4];
static const unsigned char *px_base;
static int px_renderer=-1;
static int (*px_observe)(void *);
static volatile LONG px_installed;
static void *px_get(int *parser);
static void *px_constant(int *parser,void *lexer,const void *value);
static void *px_binary(int *parser,void *lexer,void *left,void *right,void *table,uint16_t operation);
static void *px_recursive(int *parser,void *lexer,int level);
static void *px_detours[4]={(void *)px_get,(void *)px_constant,(void *)px_binary,(void *)px_recursive};

/* These are actual consumed pointer/field receipts, never a scan of the pool.
 * The callback copies/retains the exact source into its current consumer; this
 * module borrows the native object only through the enclosing native call. */
typedef struct px_reference {void *resource;uint64_t token,value;size_t offset;} px_reference;
typedef struct px_selection {int *parser,index;void *resource;px_reference reference;} px_selection;
static void px_unknown(void)
{
    DWORD error=GetLastError();int recorded=0;
    __try {if(px_observe)recorded=px_observe(NULL);}__except(EXCEPTION_EXECUTE_HANDLER){}
    if(!recorded){sh_resource_graph_incomplete();__try {sh_resource_graph_native_observe_resource(NULL);}__except(EXCEPTION_EXECUTE_HANDLER){}}
    SetLastError(error);
}
static int px_pins(int raw)
{
    const px_profile *p=&px_profiles[px_renderer];
    __try {for(size_t i=0;i<p->count;i++){
        const px_pin *pin=p->pins+i;size_t skip=raw?0:pin->skip;
        if(skip>pin->length)return 0;sh_native_pin_hash hash;unsigned char digest[32];
        sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,px_base+pin->rva+skip,pin->length-skip);sh_native_pin_hash_finish(&hash,digest);
        if(memcmp(digest,pin->digest[raw?0:1],sizeof digest))return 0;
    }}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}return 1;
}
static int px_owner(unsigned i)
{
    if(i>=4 || !px_original[i] || !hook_is_installed(px_original[i]))return 0;
    const px_profile *p=&px_profiles[px_renderer];
    __try {
        const unsigned char *entry=px_base+p->targets[i];void *target=NULL;
        static const unsigned char redirect[6]={0xff,0x25,0,0,0,0};
        if(memcmp(entry,redirect,6))return 0;memcpy(&target,entry+6,8);
        if(target!=px_detours[i])return 0;
        for(size_t n=14;n<p->stolen[i];n++)if(entry[n]!=0x90)return 0;
    }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}return 1;
}
static int px_unwind(void)
{
    const px_profile *p=&px_profiles[px_renderer];
    __try {for(size_t i=0;i<p->region_count;i++){
        const px_region *r=p->regions+i;DWORD64 image=0;
        PRUNTIME_FUNCTION actual=RtlLookupFunctionEntry((DWORD64)(uintptr_t)(px_base+r->begin),&image,NULL);
        if(!actual || image!=(DWORD64)(uintptr_t)px_base || actual->BeginAddress!=r->begin ||
           actual->EndAddress!=r->end || actual->UnwindData!=r->unwind)return 0;
    }}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}return 1;
}
int sh_resource_parm_expression_reads_ready(void)
{
    if(!InterlockedCompareExchange(&px_installed,0,0) || !px_base || !px_observe || px_renderer<0 || px_renderer>1 ||
       sh_host_is_vulkan()!=px_renderer || !sh_host_is_pinned_renderer_build(px_renderer))return 0;
    for(unsigned i=0;i<4;i++)if(!px_owner(i))return 0;
    return px_pins(0) && px_unwind();
}
static int px_reference_read(void *resource,size_t offset,px_reference *out)
{
    if(!resource || !px_observe)return 0;uint64_t before=sh_resource_instances_token(resource),value=0;int observed=0;
    __try {
        if(offset)value=*(uint64_t *)((unsigned char *)resource+offset);
        observed=px_observe(resource);
        uint64_t token=sh_resource_instances_token(resource);
        if(!observed || !token || (before && token!=before) ||
           (offset && value!=*(uint64_t *)((unsigned char *)resource+offset)))return 0;
        *out=(px_reference){resource,token,value,offset};return 1;
    }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static int px_reference_same(const px_reference *ref)
{
    if(!ref->resource || !ref->token || sh_resource_instances_token(ref->resource)!=ref->token)return 0;
    __try {return !ref->offset || ref->value==*(uint64_t *)((unsigned char *)ref->resource+ref->offset);}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static int px_select(int *parser,px_selection *out)
{
    __try {
        int index=*parser;if(index<0 || index>=128)return 0;
        void *resource=((void **)(px_base+px_profiles[px_renderer].pool))[index];
        px_reference ref;if(!px_reference_read(resource,0x68,&ref))return 0;
        if(*parser!=index || ((void **)(px_base+px_profiles[px_renderer].pool))[index]!=resource)return 0;
        *out=(px_selection){parser,index,resource,ref};return 1;
    }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static int px_selected_same(const px_selection *s,void *result)
{
    __try {return result==s->resource && *s->parser==s->index+1 &&
        ((void **)(px_base+px_profiles[px_renderer].pool))[s->index]==s->resource && px_reference_same(&s->reference);}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static void *px_get(int *parser)
{
    DWORD error=GetLastError();int observing=sh_resource_graph_native_observing(),ok=1,returned=0;px_selection selected={0};void *result=NULL;
    if(observing)ok=sh_resource_parm_expression_reads_ready() && px_select(parser,&selected);
    if(!ok)px_unknown();SetLastError(error);
    __try {result=((px_get_fn)px_original[0])(parser);returned=1;}
    __finally {error=GetLastError();if(observing && (!returned || (ok && !px_selected_same(&selected,result))))px_unknown();SetLastError(error);}
    return result;
}
static void *px_constant(int *parser,void *lexer,const void *value)
{
    DWORD error=GetLastError();int observing=sh_resource_graph_native_observing(),ok=1,returned=0;px_selection selected={0};void *result=NULL;
    if(observing)ok=sh_resource_parm_expression_reads_ready() && px_select(parser,&selected);
    if(!ok)px_unknown();SetLastError(error);
    __try {result=((px_constant_fn)px_original[1])(parser,lexer,value);returned=1;}
    __finally {error=GetLastError();if(observing && (!returned || (ok && !px_selected_same(&selected,result))))px_unknown();SetLastError(error);}
    return result;
}
static void *px_binary(int *parser,void *lexer,void *left,void *right,void *table,uint16_t operation)
{
    DWORD error=GetLastError();int observing=sh_resource_graph_native_observing(),ok=1,returned=0;px_selection selected={0};
    px_reference first={0},second={0};void *manager=NULL,*result=NULL;
    if(observing){
        ok=sh_resource_parm_expression_reads_ready();
        __try {if(ok && table){manager=*(void **)(px_base+px_profiles[px_renderer].manager);
            if(!manager)ok=0;else {int count=*(int *)((unsigned char *)manager+0x10180);if(count<0 || count>0x800)ok=0;}}
        }__except(EXCEPTION_EXECUTE_HANDLER){ok=0;}
        /* Native registration reads table+60 when nonnull. For valid native
         * count<=0x800, that path cannot return the0x93f no-table sentinel;
         * only the null-table path consumes left+68. Right is always consumed. */
        if(ok)ok=px_reference_read(table?table:left,table?0x60:0x68,&first) &&
            px_reference_read(right,0x68,&second) && px_select(parser,&selected);
    }
    if(!ok)px_unknown();SetLastError(error);
    __try {result=((px_binary_fn)px_original[2])(parser,lexer,left,right,table,operation);returned=1;}
    __finally {error=GetLastError();if(observing){int same=0;
        __try {int binding=1;if(table){int count=manager?*(int *)((unsigned char *)manager+0x10180):-1;
                binding=manager==*(void **)(px_base+px_profiles[px_renderer].manager) && count>=0 && count<=0x800;}
            same=ok && binding && px_selected_same(&selected,result) && px_reference_same(&first) && px_reference_same(&second);
        }__except(EXCEPTION_EXECUTE_HANDLER){}
        if(!returned || (ok && !same))px_unknown();
    }SetLastError(error);}
    return result;
}
static void *px_recursive(int *parser,void *lexer,int level)
{
    DWORD error=GetLastError();int observing=sh_resource_graph_native_observing(),ok=1,returned=0;void *result=NULL;
    if(observing)ok=sh_resource_parm_expression_reads_ready();
    if(!ok)px_unknown();SetLastError(error);
    __try {result=((px_recursive_fn)px_original[3])(parser,lexer,level);returned=1;}
    __finally {error=GetLastError();if(observing){
        /* This actual result is the operand native returns to its consumer.
         * It includes the inline error-arm pool read that bypasses Getter.
         * Other nested selections are observed at their own exact boundary. */
        px_reference ref={0};if(!returned || (ok && (!px_reference_read(result,0,&ref) || !px_reference_same(&ref))))px_unknown();
    }SetLastError(error);}
    return result;
}
static int px_remove(void)
{
    InterlockedExchange(&px_installed,0);int ok=1;
    for(int i=3;i>=0;i--)if(px_original[i]){
        if(hook_unpatch(px_original[i]))px_original[i]=NULL;else ok=0;
    }return ok;
}
int sh_resource_parm_expression_reads_install(const uint8_t *base,int (*observe)(void *))
{
    int renderer=sh_host_is_vulkan();
    if(!base || !observe || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer))return 0;
    if(px_base && (px_base!=base || px_renderer!=renderer || px_observe!=observe))return 0;
    if(sh_resource_parm_expression_reads_ready())return 1;
    if(!px_remove())return 0;
    px_base=base;px_renderer=renderer;px_observe=observe;
    if(!px_pins(1))return 0;
    const px_profile *p=&px_profiles[renderer];
    for(unsigned i=0;i<4;i++){
        px_original[i]=hook_prepare((void *)(base+p->targets[i]),px_detours[i],p->stolen[i]);
        if(!px_original[i])goto failed;
    }
    for(unsigned i=0;i<4;i++)if(hook_commit(px_original[i])!=B2_PATCH_OK)goto failed;
    InterlockedExchange(&px_installed,1);return sh_resource_parm_expression_reads_ready();
failed:px_remove();return 0;
}
