#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "resource_joint_reads.h"
#include "resource_graph.h"
#include "resource_graph_native.h"
#include "resource_instances.h"
#include "execution_context.h"
#include "host_image.h"
#include "hook.h"

typedef struct jr_binding { uint32_t list,symbol,continuation,holder; } jr_binding;
static const jr_binding jr_bindings[2]={
    {0x149e120,0x1464d90,0x149e2b1,0x36f9270},
    {0x14aab20,0x1470d60,0x14aacb1,0x4df6cd0}
};
/* Whole position-independent instructions: 19 and 15 bytes respectively. */
static const unsigned char jr_list_bytes[19]={0x40,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xb0,0x03,0,0};
static const unsigned char jr_symbol_bytes[15]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x40};
static const unsigned char jr_site_bytes[16]={0xff,0x50,0x38,0x49,0x8b,0x4f,0x40,0x0f,0xb7,0x51,0x0e,0x48,0x03,0xd1,0x41,0x8b};
typedef void (*jr_list_fn)(void *,void *,const char *,void *);
typedef short *(*jr_symbol_fn)(void *,short *,const char *);
typedef struct jr_scope {
    uint64_t context,serial;
    void *skeleton,*model;
    uint64_t skeleton_token,model_token,skeleton_read_token,model_read_token;
    int blocked;
    struct jr_scope *next;
} jr_scope;
static SRWLOCK jr_lock=SRWLOCK_INIT;
static jr_scope *jr_scopes;
static volatile long long jr_serial;
static const uint8_t *jr_base;
static int jr_renderer;
static int (*jr_observe)(void *);
static jr_list_fn jr_list_original;
static jr_symbol_fn jr_symbol_original;
static volatile LONG jr_ready,jr_attempted;
#ifdef SH_RESOURCE_JOINT_READS_TESTING
static int jr_fail_alloc;
#endif
static void jr_unknown(void)
{
    int returned=0;
    __try { if(jr_observe) { (void)jr_observe(NULL);returned=1; } }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
    if(!returned) {
        /* An observer failure must not replace a native result/exception. */
        __try { sh_resource_graph_incomplete();(void)sh_resource_graph_native_observe_resource(NULL); }
        __except(EXCEPTION_EXECUTE_HANDLER) { }
    }
}
static int jr_observed(void *resource)
{
    __try { return jr_observe && jr_observe(resource); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int jr_pointer(const void *object,size_t offset,void **out)
{
    if(!object || (uintptr_t)object>UINTPTR_MAX-offset-sizeof(void *)) return 0;
    __try { memcpy(out,(const uint8_t *)object+offset,sizeof(*out)); return 1; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int jr_symbol_identity(void *actual)
{
    void *holder=NULL,*table=NULL,*method=NULL;
    const jr_binding *b=jr_bindings+jr_renderer;
    return jr_pointer(jr_base,b->holder,&holder) && holder && (!actual || holder==actual) &&
        jr_pointer(holder,0,&table) && jr_pointer(table,0x38,&method) && method==jr_base+b->symbol;
}
static jr_scope *jr_top(uint64_t context)
{ for(jr_scope *p=jr_scopes;p;p=p->next) if(p->context==context) return p; return NULL; }
static void jr_retire(uint64_t context)
{
    AcquireSRWLockExclusive(&jr_lock);
    jr_scope **link=&jr_scopes;
    while(*link) { jr_scope *p=*link; if(p->context==context) { *link=p->next;free(p); } else link=&p->next; }
    ReleaseSRWLockExclusive(&jr_lock);
    /* The graph retires its own failed frame. Never report against the live
     * context that happens to drain this abandoned fiber's metadata. */
}
static sh_execution_owner jr_owner={jr_retire};
static uint64_t jr_enter(void *skeleton,void *model,uint64_t *context)
{
    *context=sh_execution_context(&jr_owner,1);
    jr_scope *scope=NULL; uint64_t serial=sh_execution_next(&jr_serial);
#ifdef SH_RESOURCE_JOINT_READS_TESTING
    if(!jr_fail_alloc)
#endif
    if(*context && serial) scope=malloc(sizeof(*scope));
    if(!scope) {
        /* An unrecorded nested call must never borrow its caller's arguments. */
        AcquireSRWLockExclusive(&jr_lock);
        jr_scope *parent=*context?jr_top(*context):NULL; if(parent) parent->blocked=1;
        ReleaseSRWLockExclusive(&jr_lock); jr_unknown(); return 0;
    }
    *scope=(jr_scope){*context,serial,skeleton,model,
        sh_resource_instances_token(skeleton),sh_resource_instances_token(model),0,0,0,NULL};
    AcquireSRWLockExclusive(&jr_lock);scope->next=jr_scopes;jr_scopes=scope;ReleaseSRWLockExclusive(&jr_lock);
    if(!jr_symbol_identity(NULL)) jr_unknown();
    return serial;
}
static void jr_leave(uint64_t context,uint64_t serial,int returned)
{
    if(!serial) return;
    uint64_t current=sh_execution_context(&jr_owner,0); int ordered=0,found=0,stable=1;
    jr_scope completed={0};
    AcquireSRWLockExclusive(&jr_lock);
    jr_scope *top=jr_top(context);
    ordered=current==context && top && top->serial==serial;
    jr_scope **link=&jr_scopes;
    while(*link) { jr_scope *p=*link; if(p->context==context && p->serial==serial) {
        completed=*p;
        *link=p->next;free(p);found=1;break; } link=&p->next; }
    if(!ordered) for(jr_scope *p=jr_scopes;p;p=p->next) if(p->context==context) p->blocked=1;
    ReleaseSRWLockExclusive(&jr_lock);
    if(found)stable=(!completed.model_read_token || sh_resource_instances_token(completed.model)==completed.model_read_token) &&
                    (!completed.skeleton_read_token || sh_resource_instances_token(completed.skeleton)==completed.skeleton_read_token);
    if(!returned || !ordered || !found || !stable || !sh_resource_joint_reads_ready() || !jr_symbol_identity(NULL)) jr_unknown();
}
static void jr_reached(void *symbol,uintptr_t caller)
{
    if(!sh_resource_graph_native_observing() ||
       caller!=(uintptr_t)jr_base+jr_bindings[jr_renderer].continuation) return;
    if(!sh_resource_joint_reads_ready()) { jr_unknown();return; }
    uint64_t context=sh_execution_context(&jr_owner,0);
    void *skeleton=NULL,*model=NULL,*related=NULL;int valid=0;
    uint64_t serial=0,model_entry=0,skeleton_entry=0,model_read=0,skeleton_read=0;
    AcquireSRWLockShared(&jr_lock);
    jr_scope *scope=context?jr_top(context):NULL;
    if(scope && !scope->blocked) {
        skeleton=scope->skeleton;model=scope->model;serial=scope->serial;valid=1;
        model_entry=scope->model_token;skeleton_entry=scope->skeleton_token;
        model_read=scope->model_read_token;skeleton_read=scope->skeleton_read_token;
    }
    ReleaseSRWLockShared(&jr_lock);
    if(!valid || !skeleton || !jr_symbol_identity(symbol) ||
       !jr_pointer(model,0x138,&related) || related!=skeleton) { jr_unknown();return; }
    uint64_t model_before=sh_resource_instances_token(model),skeleton_before=sh_resource_instances_token(skeleton);
    if((model_entry && model_before!=model_entry) || (skeleton_entry && skeleton_before!=skeleton_entry) ||
       (model_read && model_before!=model_read) || (skeleton_read && skeleton_before!=skeleton_read)) {
        jr_unknown();return;
    }
    /* The MD6 caller already consumed this model's skeleton relationship;
     * the very next native instruction now consumes the skeleton header. */
    int a=jr_observed(model),b=jr_observed(skeleton);
    uint64_t model_after=sh_resource_instances_token(model),skeleton_after=sh_resource_instances_token(skeleton);
    if(!a || !b || !model_after || !skeleton_after ||
       (model_before && model_after!=model_before) || (skeleton_before && skeleton_after!=skeleton_before) ||
       !jr_pointer(model,0x138,&related) || related!=skeleton || !jr_symbol_identity(symbol) ||
       !sh_resource_joint_reads_ready() || sh_execution_context(&jr_owner,0)!=context) { jr_unknown();return; }
    AcquireSRWLockExclusive(&jr_lock);scope=jr_top(context);
    valid=scope && scope->serial==serial && !scope->blocked;
    if(valid) { scope->model_read_token=model_after;scope->skeleton_read_token=skeleton_after; }
    ReleaseSRWLockExclusive(&jr_lock);
    if(!valid)jr_unknown();
}
static void jr_list_hook(void *skeleton,void *model,const char *pattern,void *indices)
{
    uint64_t context=0,serial=0;int returned=0;
    if(sh_resource_graph_native_observing()) {
        if(sh_resource_joint_reads_ready())serial=jr_enter(skeleton,model,&context);
        else jr_unknown();
    }
    __try { jr_list_original(skeleton,model,pattern,indices);returned=1; }
    __finally { jr_leave(context,serial,returned); }
}
static short *jr_symbol_call(void *symbol,short *result,const char *name,uintptr_t caller)
{
    short *value=NULL;int returned=0;
    int observing=sh_resource_graph_native_observing() &&
        caller==(uintptr_t)jr_base+jr_bindings[jr_renderer].continuation;
    __try { value=jr_symbol_original(symbol,result,name);returned=1; }
    __finally { if(observing && !returned)jr_unknown(); }
    jr_reached(symbol,caller);return value;
}
static short *jr_symbol_hook(void *symbol,short *result,const char *name)
{ return jr_symbol_call(symbol,result,name,(uintptr_t)_ReturnAddress()); }
int sh_resource_joint_reads_install(const uint8_t *base,int (*observe)(void *))
{
    if(InterlockedCompareExchange(&jr_attempted,1,0)) return sh_resource_joint_reads_ready();
    int renderer=sh_host_is_vulkan();
    if(!base || !observe || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer)) return 0;
    const jr_binding *b=jr_bindings+renderer;
    __try {
        if(memcmp(base+b->list,jr_list_bytes,sizeof jr_list_bytes) ||
           memcmp(base+b->symbol,jr_symbol_bytes,sizeof jr_symbol_bytes) ||
           memcmp(base+b->continuation-3,jr_site_bytes,sizeof jr_site_bytes)) return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    jr_base=base;jr_renderer=renderer;jr_observe=observe;
    void *list=hook_prepare((void *)(base+b->list),jr_list_hook,sizeof jr_list_bytes),*symbol=NULL;
    if(!list) return 0;
    symbol=hook_prepare((void *)(base+b->symbol),jr_symbol_hook,sizeof jr_symbol_bytes);
    if(!symbol) { (void)hook_unpatch(list);return 0; }
    jr_list_original=(jr_list_fn)list;jr_symbol_original=(jr_symbol_fn)symbol;
    if(hook_commit(symbol)!=B2_PATCH_OK || hook_commit(list)!=B2_PATCH_OK) {
        (void)hook_unpatch(symbol);(void)hook_unpatch(list);return 0;
    }
    InterlockedExchange(&jr_ready,1);return 1;
}
int sh_resource_joint_reads_ready(void)
{
    if(!InterlockedCompareExchange(&jr_ready,0,0) || !jr_base || !jr_list_original || !jr_symbol_original ||
       sh_host_is_vulkan()!=jr_renderer || !sh_host_is_pinned_renderer_build(jr_renderer) ||
       !hook_is_installed((void *)jr_list_original) || !hook_is_installed((void *)jr_symbol_original))return 0;
    const jr_binding *b=jr_bindings+jr_renderer;
    const uint32_t rvas[]={b->list,b->symbol};
    const size_t lengths[]={sizeof jr_list_bytes,sizeof jr_symbol_bytes};
    void *detours[]={jr_list_hook,jr_symbol_hook};
    __try {
        static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
        for(unsigned i=0;i<2;i++) {
            const unsigned char *entry=jr_base+rvas[i];void *actual=NULL;
            if(memcmp(entry,jump,6))return 0;
            memcpy(&actual,entry+6,8);if(actual!=detours[i])return 0;
            for(size_t j=14;j<lengths[i];j++)if(entry[j]!=0x90)return 0;
        }
        return !memcmp(jr_base+b->continuation-3,jr_site_bytes,sizeof jr_site_bytes);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
