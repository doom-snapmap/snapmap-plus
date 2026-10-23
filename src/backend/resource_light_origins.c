#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include "resource_light_origins.h"
#include "resource_instances.h"
#include "package_runtime.h"
#include "execution_context.h"
#include "host_image.h"
#include "hook.h"

typedef void *(*rl_ctor_fn)(void *);
typedef void (*rl_light_fn)(void *);
typedef void (*rl_emit_fn)(void *,uintptr_t,uintptr_t,uintptr_t);
typedef void (*rl_scalar_fn)(void *,uintptr_t,float);
static rl_ctor_fn g_ctor;
static rl_light_fn g_destroy,g_commit;
static rl_emit_fn g_emit;
static rl_scalar_fn g_scalar;
static void **g_fallback[3];
static const volatile int *g_suppress_commit;
static volatile LONG g_ready;
static char g_light_kind,g_committed_kind;
static SRWLOCK g_lock=SRWLOCK_INIT;
static volatile LONG64 g_serial;
#ifdef SH_RESOURCE_LIGHT_ORIGINS_TESTING
static int g_allocation_fault;
#endif
static void *rl_allocate(size_t bytes)
{
#ifdef SH_RESOURCE_LIGHT_ORIGINS_TESTING
    if(g_allocation_fault>0 && !--g_allocation_fault) return NULL;
#endif
    return calloc(1,bytes);
}
typedef struct rl_input { void *resource; sh_resource_instance_origin *origin; } rl_input;
typedef struct rl_receipt {
    struct rl_receipt *next;
    uint64_t context,serial,token,committed_token;
    void *object,*backing,*array,*stages,*committed;
    void *world,*pool,*pool_array;
    int pool_first,pool_end;
    size_t count;
    int kind;
    rl_input input[];
} rl_receipt;
typedef struct rl_scope { uint64_t context,serial,token; void *object; int emergency; } rl_scope;
static rl_receipt *g_receipts;
static void rl_retire(uint64_t context);
static sh_execution_owner g_owner={rl_retire};
enum { RL_EMIT,RL_COMMIT };
static sh_resource_instance_origin *rl_hold(void *resource)
{
    sh_resource_instance_origin *o=sh_resource_instances_lookup(resource);
    int held=o && sh_resource_instances_origin_hold(o);
    sh_resource_instances_origin_release(o); return held?o:NULL;
}
static void rl_fault(void *object,uint64_t token,sh_resource_instance_origin *origin)
{
    if(!origin || (token && sh_resource_instances_token(object)!=token)) return;
    if(!sh_resource_instances_fault(object,token,origin)) {
        sh_resource_instances_fault(NULL,0,origin); sh_package_runtime_source_cleanup_failed();
    }
}
static void rl_free(rl_receipt *r,int abandoned)
{
    if(!r) return;
    for(size_t i=0;i<r->count;i++) {
        /* Retirement has no native reads. A partial native write may still
         * own any consumed input, so preserve it until a verified reset or
         * the captured storage lifetime's completed destructor. */
        if(abandoned && r->input[i].origin) {
            void *object=r->kind==RL_COMMIT?r->committed:r->object;
            uint64_t token=r->kind==RL_COMMIT?r->committed_token:r->token;
            if(!sh_resource_instances_protect(object,token,r->input[i].origin)) rl_fault(object,token,r->input[i].origin);
        }
        sh_resource_instances_origin_unhold(r->input[i].origin);
    }
    free(r);
}
static void rl_retire(uint64_t context)
{
    rl_receipt *removed=NULL;
    AcquireSRWLockExclusive(&g_lock);
    for(rl_receipt **p=&g_receipts;*p;) {
        rl_receipt *r=*p;
        if(r->context!=context) { p=&r->next; continue; }
        *p=r->next; r->next=removed; removed=r;
    }
    ReleaseSRWLockExclusive(&g_lock);
    while(removed) { rl_receipt *next=removed->next; rl_free(removed,1); removed=next; }
    sh_resource_instances_fault_scope_retire(context);
}
static rl_scope rl_publish(rl_receipt *r)
{
    rl_scope scope={0};
    if(!r) return scope;
    r->context=sh_execution_context(&g_owner,1); r->serial=sh_execution_next(&g_serial);
    if(!r->context || !r->serial) { rl_free(r,1); return scope; }
    scope=(rl_scope){r->context,r->serial};
    AcquireSRWLockExclusive(&g_lock); r->next=g_receipts; g_receipts=r; ReleaseSRWLockExclusive(&g_lock);
    return scope;
}
static rl_receipt *rl_take(rl_scope scope)
{
    if(!scope.serial || sh_execution_context(NULL,0)!=scope.context) return NULL;
    rl_receipt *r=NULL;
    AcquireSRWLockExclusive(&g_lock);
    for(rl_receipt **p=&g_receipts;*p;p=&(*p)->next) if((*p)->context==scope.context && (*p)->serial==scope.serial) {
        r=*p; *p=r->next; break;
    }
    ReleaseSRWLockExclusive(&g_lock); return r;
}
static int rl_assign(void *object,uint64_t token,size_t offset,void *resource,sh_resource_instance_origin *origin)
{
    if(!token) token=sh_resource_instances_construct(object,0);
    if(!token) return 0;
    if(resource && !origin) {
        sh_resource_instance_origin *old=sh_resource_instances_field_origin(object,token,(uintptr_t)object+offset);
        int protected_old=!old || sh_resource_instances_protect(object,token,old);
        sh_resource_instances_origin_release(old); if(!protected_old) return 0;
    }
    return sh_resource_instances_assign(object,token,(uintptr_t)object+offset,resource,origin);
}
static void *rl_ctor(void *light)
{
    void *result=g_ctor(light);
    if(result==light && InterlockedCompareExchange(&g_ready,0,0)) {
        uint64_t token=sh_resource_instances_construct(light,1);
        sh_resource_instances_classify(light,token,&g_light_kind,light);
        if(!sh_resource_instances_fault_scope_adopt(sh_execution_context(NULL,0),light,token)) sh_package_runtime_source_cleanup_failed();
        __try {
            void *committed=*(void **)((unsigned char *)light+0xa68);
            if(committed) {
                token=sh_resource_instances_construct(committed,1);
                sh_resource_instances_classify(committed,token,&g_committed_kind,light);
                if(!sh_resource_instances_fault_scope_adopt(sh_execution_context(NULL,0),committed,token)) sh_package_runtime_source_cleanup_failed();
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) { }
    }
    return result;
}
static void rl_destroy(void *light)
{
    uint64_t token=sh_resource_instances_token(light),committed_token=0;
    void *committed=NULL; int returned=0;
    __try { committed=*(void **)((unsigned char *)light+0xa68); committed_token=sh_resource_instances_token(committed); }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
    __try { g_destroy(light); returned=1; }
    __finally {
        sh_resource_instances_destroyed(committed,committed_token,returned);
        sh_resource_instances_destroyed(light,token,returned);
    }
}
static void rl_commit_fault(void *light,void *committed)
{
    uint64_t token=sh_resource_instances_token(light),other=sh_resource_instances_token(committed);
    for(size_t i=0;i<4;i++) {
        void *resource=i?*g_fallback[i-1]:*(void **)((unsigned char *)light+0xa0);
        sh_resource_instance_origin *origin=rl_hold(resource);
        if(origin) { rl_fault(light,token,origin); rl_fault(committed,other,origin); }
        sh_resource_instances_origin_unhold(origin);
    }
}
static rl_scope rl_commit_begin(void *light)
{
    rl_receipt *r=NULL;
    if(!InterlockedCompareExchange(&g_ready,0,0) || !sh_package_runtime_resource_observation_allowed()) return (rl_scope){0};
    __try {
        if(!g_suppress_commit || *g_suppress_commit) __leave;
        void *committed=*(void **)((unsigned char *)light+0xa68);
        if(!committed) __leave;
        if(!(r=rl_allocate(sizeof(*r)+4*sizeof(rl_input)))) { rl_commit_fault(light,committed); __leave; }
        r->kind=RL_COMMIT; r->object=light; r->committed=committed; r->count=4;
        r->token=sh_resource_instances_token(light);
        if(!r->token) r->token=sh_resource_instances_construct(light,0);
        r->committed_token=sh_resource_instances_token(committed);
        if(!r->committed_token) r->committed_token=sh_resource_instances_construct(committed,0);
        if(!r->token || !r->committed_token) { rl_free(r,1); r=NULL; __leave; }
        if(!sh_resource_instances_classify(light,r->token,&g_light_kind,light) ||
            !sh_resource_instances_classify(committed,r->committed_token,&g_committed_kind,light)) {
            rl_free(r,1); r=NULL; __leave;
        }
        r->input[0].resource=*(void **)((unsigned char *)light+0xa0);
        for(size_t i=0;i<3;i++) r->input[i+1].resource=*g_fallback[i];
        for(size_t i=0;i<4;i++) {
            r->input[i].origin=rl_hold(r->input[i].resource);
            if(r->input[i].origin) sh_resource_instances_protect(committed,r->committed_token,r->input[i].origin);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { rl_free(r,1); r=NULL; }
    return rl_publish(r);
}
static int rl_commit_end(rl_scope scope,int returned)
{
    rl_receipt *r=rl_take(scope); int replaced=0;
    if(!r) return 0;
    __try {
        if(returned && g_suppress_commit && !*g_suppress_commit && sh_resource_instances_token(r->object)==r->token &&
            sh_resource_instances_token(r->committed)==r->committed_token &&
            *(void **)((unsigned char *)r->object+0xa68)==r->committed) {
            void *source=*(void **)((unsigned char *)r->object+0xa0);
            void *copied=*(void **)((unsigned char *)r->committed+0xa8);
            void *selected=*(void **)((unsigned char *)r->committed+0xba0);
            rl_input *actual=NULL;
            for(size_t i=0;i<4;i++) if(r->input[i].resource==selected && r->input[i].origin) { actual=&r->input[i]; break; }
            if(source==r->input[0].resource && copied==source && actual) {
                int a=rl_assign(r->object,r->token,0xa0,source,r->input[0].origin);
                int b=rl_assign(r->committed,r->committed_token,0xa8,copied,r->input[0].origin);
                int c=rl_assign(r->committed,r->committed_token,0xba0,selected,actual->origin);
                replaced=a && b && c && (!source || r->input[0].origin);
                if(replaced) {
                    sh_resource_instances_unprotect(r->committed,r->committed_token);
                    sh_resource_instances_unprotect(r->object,r->token);
                    sh_resource_instances_fault_recovered(r->committed,r->committed_token);
                    sh_resource_instances_fault_recovered(r->object,r->token);
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { }
    rl_free(r,!replaced); return replaced;
}
static void rl_commit(void *light)
{
    rl_scope scope=rl_commit_begin(light); int returned=0;
    __try { g_commit(light); returned=1; }
    __finally { rl_commit_end(scope,returned); }
}
static void rl_scalar(void *light,uintptr_t name,float value)
{
    sh_resource_instance_origin *origin=NULL; uint64_t token=sh_resource_instances_token(light);
    /* This fixed-ABI native leaf follows the emitter's shader store, before
     * its next native call can consume or suspend with that field. Other
     * callers qualify only an actual constructor-classified light lifetime. */
    if(InterlockedCompareExchange(&g_ready,0,0) && sh_package_runtime_resource_observation_allowed() &&
        sh_resource_instances_class_owner(light,token,&g_light_kind)==light) __try {
        void *shader=*(void **)((unsigned char *)light+0xa0);
        origin=rl_hold(shader);
        if(!rl_assign(light,token,0xa0,shader,origin)) rl_fault(light,token,origin);
    } __except(EXCEPTION_EXECUTE_HANDLER) { }
    __try { g_scalar(light,name,value); }
    __finally { sh_resource_instances_origin_unhold(origin); }
}
static void rl_emergency_adopt(void *model,uint64_t token,void *light)
{
    if(!light) return;
    uint64_t other=sh_resource_instances_token(light);
    if(!other) other=sh_resource_instances_construct(light,0);
    if(!sh_resource_instances_fault_share(model,token,light,other)) { if(!other) sh_package_runtime_source_cleanup_failed(); return; }
    void *committed=*(void **)((unsigned char *)light+0xa68);
    if(committed) {
        uint64_t copy=sh_resource_instances_token(committed);
        if(!copy) copy=sh_resource_instances_construct(committed,0);
        sh_resource_instances_fault_share(model,token,committed,copy);
    }
}
static rl_scope rl_emit_emergency(void *model,int count,void *backing)
{
    rl_scope scope={0}; uint64_t token=sh_resource_instances_token(model);
    uint64_t context=sh_execution_context(&g_owner,1);
    void *stages=*(void **)((unsigned char *)backing+0x68);
    if(count && (!stages || (uintptr_t)stages>UINTPTR_MAX-(size_t)count*0x6e0)) return scope;
    for(int i=0;i<count;i++) {
        void *material=*(void **)((unsigned char *)stages+(size_t)i*0x6e0+0x68);
        sh_resource_instance_origin *origin=rl_hold(material);
        rl_fault(model,token,origin);
        if((!token || !context) && origin) { sh_resource_instances_fault(NULL,0,origin); sh_package_runtime_source_cleanup_failed(); }
        sh_resource_instances_origin_unhold(origin);
    }
    if(!sh_resource_instances_fault_scope_begin(model,token,context)) return scope;
    scope.context=context; scope.object=model; scope.token=token; scope.emergency=1;
    __try {
        void *runtime=*(void **)((unsigned char *)model+0xec8);
        if(count && (!runtime || (uintptr_t)runtime>UINTPTR_MAX-(size_t)count*0xd0)) __leave;
        for(int i=0;i<count;i++) {
            unsigned char *stage=(unsigned char *)stages+(size_t)i*0x6e0;
            int children=*(int16_t *)(stage+0x70); void *rows=*(void **)((unsigned char *)runtime+(size_t)i*0xd0+0x88);
            if(!stage[0x58] || children<=0 || !rows || (uintptr_t)rows>UINTPTR_MAX-(size_t)children*0x38) continue;
            for(int j=0;j<children;j++) {
                unsigned char *row=(unsigned char *)rows+(size_t)j*0x38;
                if(row[0x2c]) rl_emergency_adopt(model,token,*(void **)(row+0x30));
            }
        }
        void *world=*(void **)((unsigned char *)model+0xc0);
        if(world) {
            void *pool=*(void **)((unsigned char *)world+(sh_host_is_vulkan()?0x9128:0x90e8));
            int first=*(int *)((unsigned char *)model+0xea4),last=*(int *)((unsigned char *)model+0xea8);
            if(pool && first>=0 && last>=first && last<=*(int *)((unsigned char *)pool+0x10)) {
                void **items=*(void ***)((unsigned char *)pool+8);
                if(items && (uintptr_t)items<=UINTPTR_MAX-(size_t)last*sizeof(void *))
                    for(int i=first;i<last;i++) rl_emergency_adopt(model,token,items[i]);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { }
    return scope;
}
static rl_scope rl_emit_begin(void *model)
{
    rl_receipt *r=NULL;
    if(!InterlockedCompareExchange(&g_ready,0,0) || !sh_package_runtime_resource_observation_allowed()) return (rl_scope){0};
    __try {
        int n=*(int *)((unsigned char *)model+0xed8);
        void *backing=*(void **)((unsigned char *)model+0xeb0);
        if(!backing || n<0 || n>INT_MAX/0x6e0 || n!=*(int *)((unsigned char *)backing+0x70) ||
            (size_t)n>(SIZE_MAX-sizeof(*r))/sizeof(rl_input)) __leave;
        if(!(r=rl_allocate(sizeof(*r)+(size_t)n*sizeof(rl_input)))) return rl_emit_emergency(model,n,backing);
        r->kind=RL_EMIT; r->object=model; r->token=sh_resource_instances_token(model); r->backing=backing; r->count=(size_t)n;
        r->array=*(void **)((unsigned char *)model+0xec8); r->stages=*(void **)((unsigned char *)backing+0x68);
        r->world=*(void **)((unsigned char *)model+0xc0);
        if(r->world) {
            r->pool=*(void **)((unsigned char *)r->world+(sh_host_is_vulkan()?0x9128:0x90e8));
            if(r->pool) r->pool_array=*(void **)((unsigned char *)r->pool+8);
        }
        r->pool_first=*(int *)((unsigned char *)model+0xea4); r->pool_end=*(int *)((unsigned char *)model+0xea8);
        if(!r->token || (n && (!r->array || !r->stages ||
            (uintptr_t)r->array>UINTPTR_MAX-(size_t)n*0xd0 ||
            (uintptr_t)r->stages>UINTPTR_MAX-(size_t)n*0x6e0))) { rl_free(r,1); r=NULL; __leave; }
        for(int i=0;i<n;i++) {
            r->input[i].resource=*(void **)((unsigned char *)r->stages+(size_t)i*0x6e0+0x68);
            r->input[i].origin=rl_hold(r->input[i].resource);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { rl_free(r,1); r=NULL; }
    return rl_publish(r);
}
static void rl_emit_end(rl_scope scope)
{
    if(scope.emergency) { sh_resource_instances_fault_scope_end(scope.object,scope.token,scope.context); return; }
    rl_receipt *r=rl_take(scope); int verified=0;
    if(!r) return;
    __try {
        if(sh_resource_instances_token(r->object)!=r->token ||
            *(void **)((unsigned char *)r->object+0xeb0)!=r->backing ||
            *(void **)((unsigned char *)r->object+0xec8)!=r->array ||
            *(void **)((unsigned char *)r->backing+0x68)!=r->stages ||
            *(int *)((unsigned char *)r->object+0xed8)!=(int)r->count ||
            *(int *)((unsigned char *)r->backing+0x70)!=(int)r->count) __leave;
        verified=1;
        int pool_next=r->pool_first;
        for(size_t i=0;i<r->count;i++) {
            unsigned char *stage=(unsigned char *)r->stages+i*0x6e0;
            int n=*(int16_t *)(stage+0x70);
            if(!stage[0x58] || n<=0) continue;
            unsigned char *children=*(unsigned char **)((unsigned char *)r->array+i*0xd0+0x88);
            if(!children || (uintptr_t)children>UINTPTR_MAX-(size_t)n*0x38) { verified=0; continue; }
            for(int j=0;j<n;j++) {
                unsigned char *row=children+(size_t)j*0x38;
                void *light=*(void **)(row+0x30);
                if(!row[0x2c]) continue;
                if(!light) {
                    /* Native borrows a pool slot without writing row+30.
                     * Reproduce only that proven integer range/array read. */
                    if(pool_next==r->pool_end) break;
                    if(!r->world || !r->pool || !r->pool_array || pool_next<0 || pool_next>r->pool_end ||
                        *(void **)((unsigned char *)r->object+0xc0)!=r->world ||
                        *(void **)((unsigned char *)r->world+(sh_host_is_vulkan()?0x9128:0x90e8))!=r->pool ||
                        *(void **)((unsigned char *)r->pool+8)!=r->pool_array ||
                        r->pool_end>*(int *)((unsigned char *)r->pool+0x10) ||
                        (uintptr_t)r->pool_array>UINTPTR_MAX-(size_t)r->pool_end*sizeof(void *)) { verified=0; break; }
                    light=((void **)r->pool_array)[pool_next++];
                    if(!light) { verified=0; continue; }
                }
                if(*(void **)((unsigned char *)light+0xa0)!=r->input[i].resource ||
                    !rl_assign(light,sh_resource_instances_token(light),0xa0,r->input[i].resource,r->input[i].origin)) verified=0;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { verified=0; }
    rl_free(r,!verified);
}
static void rl_emit(void *model,uintptr_t a,uintptr_t b,uintptr_t c)
{
    rl_scope scope=rl_emit_begin(model);
    __try { g_emit(model,a,b,c); }
    __finally { rl_emit_end(scope); }
}
typedef struct rl_refresh_context {
    int (*changed)(void *,void *); void *context;
    void **visited; size_t count,capacity;
    char *error; size_t error_capacity;
} rl_refresh_context;
static size_t rl_hash(void *object,size_t capacity)
{ uintptr_t h=(uintptr_t)object>>3; h^=h>>33; h*=UINT64_C(0xff51afd7ed558ccd); h^=h>>33; return h&(capacity-1); }
static int rl_seen(rl_refresh_context *c,void *object)
{
    if(c->count>=c->capacity/2) {
        size_t n=c->capacity?c->capacity*2:64;
        if(n<c->capacity || n>SIZE_MAX/sizeof(void *)) return -1;
        void **slots=calloc(n,sizeof(void *)); if(!slots) return -1;
        for(size_t i=0;i<c->capacity;i++) if(c->visited[i]) {
            size_t at=rl_hash(c->visited[i],n); while(slots[at]) at=(at+1)&(n-1); slots[at]=c->visited[i];
        }
        free(c->visited); c->visited=slots; c->capacity=n;
    }
    size_t at=rl_hash(object,c->capacity);
    while(c->visited[at]) { if(c->visited[at]==object) return 1; at=(at+1)&(c->capacity-1); }
    c->visited[at]=object; c->count++; return 0;
}
static int rl_changed(void *context,void *resource)
{ rl_refresh_context *c=context; return c->changed(c->context,resource); }
static int rl_refresh_visit(void *context,void *object,uint64_t token)
{
    rl_refresh_context *c=context;
    void *light=sh_resource_instances_class_owner(object,token,&g_light_kind);
    if(!light) light=sh_resource_instances_class_owner(object,token,&g_committed_kind);
    if(!light) return 1;
    uint64_t light_token=sh_resource_instances_token(light);
    if(!light_token || sh_resource_instances_class_owner(light,light_token,&g_light_kind)!=light) return 0;
    int seen=rl_seen(c,light); if(seen) return seen>0;
    int relevant=0;
    __try {
        void *committed=*(void **)((unsigned char *)light+0xa68);
        uint64_t committed_token=sh_resource_instances_token(committed);
        if(!committed_token || sh_resource_instances_class_owner(committed,committed_token,&g_committed_kind)!=light) return 0;
        relevant=c->changed(c->context,*(void **)((unsigned char *)light+0xa0)) ||
            c->changed(c->context,*(void **)((unsigned char *)committed+0xba0));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if(!relevant) return 1;
    rl_scope scope=rl_commit_begin(light); int returned=0;
    __try { g_commit(light); returned=1; } __except(EXCEPTION_EXECUTE_HANDLER) { }
    return rl_commit_end(scope,returned);
}
int sh_resource_light_origins_refresh(int (*changed)(void *,void *),void *context,char *error,size_t capacity)
{
    if(!changed || !sh_resource_light_origins_ready() || !sh_package_runtime_resource_observation_allowed()) return 0;
    rl_refresh_context c={changed,context,NULL,0,0,error,capacity};
    int ok=sh_resource_instances_holders_where(rl_changed,rl_refresh_visit,&c);
    free(c.visited);
    if(!ok && error && capacity) snprintf(error,capacity,"native light refresh did not verify its completed committed copy");
    return ok;
}
static int rl_all_changed(void *context,void *resource) { (void)context; return resource!=NULL; }
int sh_resource_light_origins_reconcile(void *object,uint64_t token,char *error,size_t capacity)
{
    if(sh_resource_instances_token(object)!=token) return 1;
    if(!sh_resource_instances_class_owner(object,token,&g_light_kind) &&
        !sh_resource_instances_class_owner(object,token,&g_committed_kind)) return -1;
    if(!sh_resource_light_origins_ready() || !sh_package_runtime_resource_observation_allowed()) return 0;
    rl_refresh_context c={rl_all_changed,NULL,NULL,0,0,error,capacity};
    int ok=rl_refresh_visit(&c,object,token); free(c.visited); return ok;
}
static const uint32_t g_rvas[2][5]={{0x1733970,0x1733a30,0x1733b20,0x14616f0,0x17344f0},{0x1740cd0,0x1740d90,0x1740e80,0x146d750,0x1741850}};
static const uint32_t g_fallback_rvas[2]={0x41439f0,0x5845f40};
static const unsigned char g_ctor_head[19]={0x48,0x89,0x4c,0x24,8,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff};
static const unsigned char g_destroy_head[20]={0x48,0x89,0x5c,0x24,8,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x89,0x60,0xa,0,0};
static const unsigned char g_emit_head[20]={0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x88,0,0,0};
static const unsigned char g_scalar_head[18]={0x0f,0xc6,0xd2,0,0x48,0x83,0xec,0x38,0x0f,0x29,0x54,0x24,0x20,0x4c,0x8d,0x44,0x24,0x20};
static const unsigned char g_commit_head[2][16]={{0x40,0x53,0x48,0x83,0xec,0x20,0x83,0x3d,0xe3,0x14,0xa1,2,0,0x48,0x8b,0xd9},{0x40,0x53,0x48,0x83,0xec,0x20,0x83,0x3d,0xd3,0x66,0x10,4,0,0x48,0x8b,0xd9}};
int sh_resource_light_origins_uninstall(void)
{
    InterlockedExchange(&g_ready,0);
    if(g_scalar && hook_unpatch((void *)g_scalar)) g_scalar=NULL;
    if(g_emit && hook_unpatch((void *)g_emit)) g_emit=NULL;
    if(g_commit && hook_unpatch((void *)g_commit)) g_commit=NULL;
    if(g_destroy && hook_unpatch((void *)g_destroy)) g_destroy=NULL;
    if(g_ctor && hook_unpatch((void *)g_ctor)) g_ctor=NULL;
    return !g_emit && !g_commit && !g_destroy && !g_ctor && !g_scalar;
}
int sh_resource_light_origins_install(const uint8_t *base)
{
    if(sh_resource_light_origins_ready()) return 1;
    int renderer=sh_host_is_vulkan();
    if(!base || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer) ||
        g_ctor || g_destroy || g_commit || g_emit || g_scalar) return 0;
    __try {
        if(memcmp(base+g_rvas[renderer][0],g_ctor_head,sizeof(g_ctor_head)) ||
            memcmp(base+g_rvas[renderer][1],g_destroy_head,sizeof(g_destroy_head)) ||
            memcmp(base+g_rvas[renderer][2],g_commit_head[renderer],sizeof(g_commit_head[renderer])) ||
            memcmp(base+g_rvas[renderer][3],g_emit_head,sizeof(g_emit_head)) ||
            memcmp(base+g_rvas[renderer][4],g_scalar_head,sizeof(g_scalar_head))) return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    for(size_t i=0;i<3;i++) g_fallback[i]=(void **)(base+g_fallback_rvas[renderer]+i*8);
    int32_t suppression_displacement; memcpy(&suppression_displacement,base+g_rvas[renderer][2]+8,4);
    g_suppress_commit=(const volatile int *)(base+g_rvas[renderer][2]+13+suppression_displacement);
    g_ctor=(rl_ctor_fn)hook_prepare((void *)(base+g_rvas[renderer][0]),rl_ctor,19);
    g_destroy=(rl_light_fn)hook_prepare((void *)(base+g_rvas[renderer][1]),rl_destroy,20);
    g_commit=(rl_light_fn)hook_prepare_rip_cmp32_imm8((void *)(base+g_rvas[renderer][2]),rl_commit,16,6);
    g_emit=(rl_emit_fn)hook_prepare((void *)(base+g_rvas[renderer][3]),rl_emit,20);
    g_scalar=(rl_scalar_fn)hook_prepare((void *)(base+g_rvas[renderer][4]),rl_scalar,18);
    if(!g_ctor || !g_destroy || !g_commit || !g_emit || !g_scalar || hook_commit((void *)g_ctor)!=B2_PATCH_OK ||
        hook_commit((void *)g_destroy)!=B2_PATCH_OK || hook_commit((void *)g_commit)!=B2_PATCH_OK ||
        hook_commit((void *)g_emit)!=B2_PATCH_OK || hook_commit((void *)g_scalar)!=B2_PATCH_OK) {
        sh_resource_light_origins_uninstall(); return 0;
    }
    InterlockedExchange(&g_ready,1); return 1;
}
int sh_resource_light_origins_ready(void) { return InterlockedCompareExchange(&g_ready,0,0)!=0; }
