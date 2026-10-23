#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "resource_late_model_reads.h"
#include "resource_graph.h"
#include "resource_graph_native.h"
#include "execution_context.h"
#include "host_image.h"
#include "hook.h"
#include "native_pin_hash.h"

enum {LM_HOT_CTOR,LM_WATER_LOAD,LM_HOT_BUILD};
typedef struct lm_pin {uint32_t rva,length;unsigned skip;const unsigned char *digest,*tail_digest;} lm_pin;
typedef struct lm_profile {
    uint32_t hot_ctor,water_load,hot_build,post,hot_field,water_field,memory_table;
    uint32_t file_site,decl_sites[3],memory_methods[4];
    const lm_pin *pins;size_t pin_count;
} lm_profile;
#include "resource_late_model_read_pins.h"
typedef struct lm_read {
    void *resource;uint64_t token;sh_resource_instance_origin *origin;
    void *file,*data;uint64_t length,contents;int file_input;
    struct lm_read *next;
} lm_read;
typedef struct lm_frame {
    sh_resource_late_model_scope scope,parent;
    void *model;uint64_t model_token;int kind,failed,borrowed;
    void *backing,*material;uint64_t rectangles;int rectangle_count;unsigned char flags;
    lm_read *reads;struct lm_frame *next;
} lm_frame;
typedef struct lm_context {uint64_t id;unsigned blocked;struct lm_context *next;} lm_context;
typedef struct lm_binding {
    void *model,*resource;uint64_t model_token,resource_token;size_t offset;
    struct lm_binding *next;
} lm_binding;
static SRWLOCK lm_lock=SRWLOCK_INIT;
static lm_frame *lm_frames;
static lm_context *lm_contexts;
static lm_binding *lm_bindings;
static volatile long long lm_serial;
static const unsigned char *lm_base;
static int lm_renderer=-1;
static sh_resource_late_model_api lm_api;
static void *(*lm_ctor_original)(void *,void *);
static uint64_t (*lm_water_original)(void *,void *);
static void (*lm_post_original)(void *,unsigned char);
static volatile LONG lm_ready,lm_attempted;
#ifdef SH_RESOURCE_LATE_MODEL_READS_TESTING
static int lm_fail_allocation;
#endif
static void *lm_allocate(size_t size)
{
#ifdef SH_RESOURCE_LATE_MODEL_READS_TESTING
    if(lm_fail_allocation){lm_fail_allocation=0;return NULL;}
#endif
    return calloc(1,size);
}
static int lm_pointer(const void *object,size_t offset,void *out,size_t size)
{
    if(!object || offset>UINTPTR_MAX-(uintptr_t)object || size>UINTPTR_MAX-((uintptr_t)object+offset))return 0;
    __try {memcpy(out,(const unsigned char *)object+offset,size);return 1;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static void lm_unknown(void)
{
    __try {sh_resource_graph_incomplete();(void)sh_resource_graph_native_observe_resource(NULL);}
    __except(EXCEPTION_EXECUTE_HANDLER){}
}
static int lm_visible(sh_resource_late_model_scope scope)
{__try{return lm_api.visible && lm_api.visible(scope);}__except(EXCEPTION_EXECUTE_HANDLER){lm_unknown();return 0;}}
static void lm_read_free(lm_read *r)
{while(r){lm_read *next=r->next;sh_resource_instances_origin_unhold(r->origin);sh_resource_instances_origin_release(r->origin);free(r);r=next;}}
static void lm_retire(uint64_t context)
{
    lm_frame *retired=NULL;
    AcquireSRWLockExclusive(&lm_lock);
    lm_frame **slot=&lm_frames;
    while(*slot){lm_frame *f=*slot;if(f->scope.context==context){*slot=f->next;f->next=retired;retired=f;}else slot=&f->next;}
    lm_context **c=&lm_contexts;
    while(*c){if((*c)->id==context){lm_context *old=*c;*c=old->next;free(old);break;}c=&(*c)->next;}
    ReleaseSRWLockExclusive(&lm_lock);
    /* Root retires its own DERIVED/fault frames, with no deleted stack access. */
    while(retired){lm_frame *next=retired->next;lm_read_free(retired->reads);free(retired);retired=next;}
}
static sh_execution_owner lm_owner={lm_retire};
static lm_frame *lm_top(uint64_t context)
{for(lm_frame *f=lm_frames;f;f=f->next)if(f->scope.context==context)return f;return NULL;}
static lm_frame *lm_find(sh_resource_late_model_scope scope)
{lm_frame *f=lm_top(scope.context);return f && f->scope.serial==scope.serial?f:NULL;}
static lm_context *lm_context_find(uint64_t id)
{for(lm_context *c=lm_contexts;c;c=c->next)if(c->id==id)return c;return NULL;}
static void lm_failed(sh_resource_late_model_scope scope)
{AcquireSRWLockExclusive(&lm_lock);lm_frame *f=lm_find(scope);if(f)f->failed=1;ReleaseSRWLockExclusive(&lm_lock);}
static int lm_digest(const unsigned char *bytes,size_t length,const unsigned char expected[32])
{
    sh_native_pin_hash hash;unsigned char actual[32];
    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,bytes,length);
    sh_native_pin_hash_finish(&hash,actual);return !memcmp(actual,expected,sizeof actual);
}
static int lm_pin_check(const lm_pin *p)
{__try{return lm_digest(lm_base+p->rva+p->skip,p->length-p->skip,p->tail_digest);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
static int lm_shared_ready(void)
{__try{return lm_api.ready && lm_api.ready();}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
static void *lm_ctor_hook(void *model,void *name);
static uint64_t lm_water_hook(void *model,void *path);
static void lm_post_hook(void *model,unsigned char bounds);
int sh_resource_late_model_reads_ready(void)
{
    if(!InterlockedCompareExchange(&lm_ready,0,0) || !lm_base || lm_renderer<0 || lm_renderer>1 ||
       sh_host_is_vulkan()!=lm_renderer || !sh_host_is_pinned_renderer_build(lm_renderer) || !lm_shared_ready())return 0;
    const lm_profile *p=lm_profiles+lm_renderer;
    void *tramps[]={lm_ctor_original,lm_water_original,lm_post_original},*detours[]={lm_ctor_hook,lm_water_hook,lm_post_hook};
    const uint32_t entries[]={p->hot_ctor,p->water_load,p->post};const unsigned stolen[]={15,14,14};
    __try {
        for(unsigned i=0;i<3;i++) {
            static const unsigned char jump[6]={0xff,0x25,0,0,0,0};void *target;
            if(!tramps[i] || !hook_is_installed(tramps[i]) || memcmp(lm_base+entries[i],jump,6))return 0;
            memcpy(&target,lm_base+entries[i]+6,8);if(target!=detours[i])return 0;
            for(unsigned j=14;j<stolen[i];j++)if(lm_base[entries[i]+j]!=0x90)return 0;
        }
        for(size_t i=0;i<p->pin_count;i++)if(!lm_pin_check(p->pins+i))return 0;
    }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return 1;
}
static sh_resource_late_model_scope lm_enter(void *model,int kind)
{
    sh_resource_late_model_scope scope={0};
    if(!sh_resource_late_model_reads_ready()) {
        /* A nested unobserved call must not lend its reads to an outer frame. */
        scope.context=sh_execution_context(NULL,0);
        AcquireSRWLockExclusive(&lm_lock);lm_context *c=lm_context_find(scope.context);
        if(c){c->blocked++;lm_frame *outer=lm_top(scope.context);if(outer)outer->failed=1;}else scope.context=0;
        ReleaseSRWLockExclusive(&lm_lock);lm_unknown();return scope;
    }
    scope.context=sh_execution_context(&lm_owner,1);scope.serial=sh_execution_next(&lm_serial);
    if(!scope.context || !scope.serial){lm_unknown();return (sh_resource_late_model_scope){0};}
    lm_frame *f=lm_allocate(sizeof(*f));lm_context *fresh=NULL;
    AcquireSRWLockExclusive(&lm_lock);lm_context *c=lm_context_find(scope.context);ReleaseSRWLockExclusive(&lm_lock);
    if(!c)fresh=lm_allocate(sizeof(*fresh));
    AcquireSRWLockExclusive(&lm_lock);c=lm_context_find(scope.context);
    if(!c && fresh){fresh->id=scope.context;fresh->next=lm_contexts;lm_contexts=fresh;c=fresh;fresh=NULL;}
    if(!f || !c || c->blocked) {
        if(c){c->blocked++;lm_frame *outer=lm_top(scope.context);if(outer)outer->failed=1;}
        ReleaseSRWLockExclusive(&lm_lock);free(f);free(fresh);lm_unknown();scope.serial=0;return scope;
    }
    ReleaseSRWLockExclusive(&lm_lock);free(fresh);
    f->scope=scope;f->model=model;f->kind=kind;
    f->model_token=kind==LM_HOT_CTOR?0:sh_resource_instances_token(model);
    sh_resource_late_model_scope enclosing={0};
    AcquireSRWLockExclusive(&lm_lock);lm_frame *outer=lm_top(scope.context);
    if(kind==LM_HOT_BUILD && outer && outer->model==model && outer->model_token==f->model_token)enclosing=outer->parent;
    ReleaseSRWLockExclusive(&lm_lock);
    if(enclosing.serial && lm_visible(enclosing)){f->parent=enclosing;f->borrowed=1;}
    else __try {f->parent=lm_api.begin(model,kind==LM_HOT_CTOR?SH_LATE_MODEL_CONSTRUCT:
        kind==LM_WATER_LOAD?SH_LATE_MODEL_WATER_APPEND:SH_LATE_MODEL_HOT_REBUILD);}__except(EXCEPTION_EXECUTE_HANDLER){f->failed=1;lm_unknown();}
    if(!f->parent.context || !f->parent.serial)f->failed=1;
    AcquireSRWLockExclusive(&lm_lock);f->next=lm_frames;lm_frames=f;ReleaseSRWLockExclusive(&lm_lock);
    return scope;
}
static int lm_file(lm_read *r)
{
    const lm_profile *p=lm_profiles+lm_renderer;void *table,*method;
    const unsigned slots[]={0,0x28,0x58,0xe0};
    if(!lm_pointer(r->resource,0x38,&r->file,8) || !r->file || !lm_pointer(r->file,0,&table,8))return 0;
    for(unsigned i=0;i<4;i++) {
        if(!lm_pointer(lm_base+p->memory_table,slots[i],&method,8) || method!=lm_base+p->memory_methods[i] ||
           !lm_pointer(table,slots[i],&method,8) || method!=lm_base+p->memory_methods[i])return 0;
    }
    if(!lm_pointer(r->file,0x168,&r->data,8) || !lm_pointer(r->file,0x150,&r->length,8) ||
       r->length>SIZE_MAX || r->length>UINTPTR_MAX-(uintptr_t)r->data || (r->length && !r->data))return 0;
    r->contents=UINT64_C(14695981039346656037);unsigned char bytes[128];
    for(size_t at=0;at<(size_t)r->length;) {
        size_t n=(size_t)r->length-at;if(n>sizeof bytes)n=sizeof bytes;
        if(!lm_pointer(r->data,at,bytes,n))return 0;
        for(size_t i=0;i<n;i++){r->contents^=bytes[i];r->contents*=UINT64_C(1099511628211);}at+=n;
    }return 1;
}
/* exact transfers one metadata reference; no lookup replaces this witnessed
 * field origin. NULL requests the source at this actual synchronous read. */
static int lm_record(sh_resource_late_model_scope scope,void *resource,int file_input,
                     sh_resource_instance_origin *exact,uint64_t exact_token)
{
    sh_resource_late_model_scope parent={0};void *model=NULL;uint64_t model_token=0;
    AcquireSRWLockExclusive(&lm_lock);lm_frame *f=lm_find(scope);
    if(f){parent=f->parent;model=f->model;model_token=f->model_token;}
    ReleaseSRWLockExclusive(&lm_lock);
    if(!model || !lm_visible(parent)){sh_resource_instances_origin_release(exact);return 0;}
    uint64_t token=sh_resource_instances_token(resource);
    sh_resource_instance_origin *origin=exact?exact:sh_resource_instances_lookup(resource);
    int held=origin && sh_resource_instances_origin_hold(origin),ok=resource && token && held && (!exact_token || token==exact_token);
    uint64_t current=sh_resource_instances_token(model);
    AcquireSRWLockExclusive(&lm_lock);f=lm_find(scope);
    if(f && f->kind==LM_HOT_CTOR && !f->model_token)f->model_token=current;
    if(f)model_token=f->model_token;
    ReleaseSRWLockExclusive(&lm_lock);
    if(!current || current!=model_token)ok=0;
    __try {if(!lm_api.input(parent,resource,token,held?origin:NULL))ok=0;}
    __except(EXCEPTION_EXECUTE_HANDLER){ok=0;lm_unknown();}
    lm_read *read=lm_allocate(sizeof(*read));
    if(read){read->resource=resource;read->token=token;read->origin=origin;read->file_input=file_input;if(file_input && !lm_file(read))ok=0;}
    if(!ok || !read) {
        lm_failed(scope);
        if(held)(void)sh_resource_instances_fault(model,model_token,origin);
        else (void)sh_resource_instances_mark_fault(model,model_token);
    }
    if(read && held) {
        AcquireSRWLockExclusive(&lm_lock);f=lm_find(scope);
        if(f){read->next=f->reads;f->reads=read;read=NULL;origin=NULL;}
        ReleaseSRWLockExclusive(&lm_lock);
    }
    free(read);if(held && origin)sh_resource_instances_origin_unhold(origin);sh_resource_instances_origin_release(origin);
    return ok;
}
void sh_resource_late_model_returned(void *resource,uintptr_t caller)
{
    if(!lm_base || lm_renderer<0 || lm_renderer>1 || !resource)return;
    uint64_t context=sh_execution_context(NULL,0);sh_resource_late_model_scope scope={0},parent={0};int kind=-1,blocked=0;
    AcquireSRWLockExclusive(&lm_lock);lm_context *c=lm_context_find(context);lm_frame *f=lm_top(context);
    if(c)blocked=c->blocked!=0;if(f){scope=f->scope;parent=f->parent;kind=f->kind;}ReleaseSRWLockExclusive(&lm_lock);
    if(!scope.serial || blocked || kind==LM_HOT_BUILD || !lm_visible(parent))return;
    const lm_profile *p=lm_profiles+lm_renderer;int file_input=kind==LM_WATER_LOAD && caller==(uintptr_t)lm_base+p->file_site;
    int decl=0;for(unsigned i=0;i<3;i++)if(caller==(uintptr_t)lm_base+p->decl_sites[i])decl=1;
    if(!file_input && !decl)return;
    if(!sh_resource_late_model_reads_ready()){lm_failed(scope);lm_unknown();return;}
    (void)lm_record(scope,resource,file_input,NULL,0);
}
static int lm_commit(lm_frame *f,size_t offset)
{
    void *actual=NULL;if(!lm_pointer(f->model,offset,&actual,8) || !actual)return 0;
    lm_read *chosen=NULL;
    for(lm_read *r=f->reads;r;r=r->next)if(!r->file_input && r->resource==actual) {
        if(chosen && (chosen->token!=r->token || chosen->origin!=r->origin))return 0;chosen=r;
    }
    if(!chosen || sh_resource_instances_token(actual)!=chosen->token ||
       !sh_resource_instances_assign(f->model,f->model_token,(uintptr_t)f->model+offset,actual,chosen->origin))return 0;
    lm_binding *fresh=lm_allocate(sizeof(*fresh));if(!fresh)return 0;
    *fresh=(lm_binding){f->model,actual,f->model_token,chosen->token,offset,NULL};
    AcquireSRWLockExclusive(&lm_lock);lm_binding **at=&lm_bindings;
    while(*at){lm_binding *b=*at;if(b->model==f->model && (b->model_token!=f->model_token || b->offset==offset)){*at=b->next;free(b);}else at=&b->next;}
    fresh->next=lm_bindings;lm_bindings=fresh;ReleaseSRWLockExclusive(&lm_lock);return 1;
}
static int lm_rectangle_hash(void *backing,int count,uint64_t *hash)
{
    uintptr_t data;unsigned char bytes[128];*hash=UINT64_C(14695981039346656037);
    if(count<0 || (size_t)count>SIZE_MAX/0x14 || !lm_pointer(backing,0x70,&data,8))return 0;
    size_t length=(size_t)count*0x14;
    if(length>UINTPTR_MAX-data || (length && !data))return 0;
    for(size_t at=0;at<length;) {
        size_t n=length-at;if(n>sizeof bytes)n=sizeof bytes;
        if(!lm_pointer((void *)data,at,bytes,n))return 0;
        for(size_t i=0;i<n;i++){*hash^=bytes[i];*hash*=UINT64_C(1099511628211);}at+=n;
    }return 1;
}
static void lm_leave(sh_resource_late_model_scope scope,int returned,int success)
{
    if(!scope.context)return;
    if(scope.context!=sh_execution_context(NULL,0)){lm_failed(scope);lm_unknown();return;}
    AcquireSRWLockExclusive(&lm_lock);lm_context *c=lm_context_find(scope.context);
    if(!scope.serial){if(c && c->blocked)c->blocked--;ReleaseSRWLockExclusive(&lm_lock);return;}
    lm_frame *f=lm_frames,*discard=NULL,**tail=&discard;
    while(f && (f->scope.context!=scope.context || f->scope.serial!=scope.serial))f=f->next;
    if(f) {
        lm_frame **at=&lm_frames;
        while(*at) {
            lm_frame *p=*at;
            if(p->scope.context!=scope.context){at=&p->next;continue;}
            *at=p->next;p->next=NULL;
            if(p==f)break;
            p->failed=1;*tail=p;tail=&p->next;
        }
        if(discard)f->failed=1;
    } else {for(lm_frame *p=lm_frames;p;p=p->next)if(p->scope.context==scope.context)p->failed=1;}
    ReleaseSRWLockExclusive(&lm_lock);if(!f){lm_unknown();return;}
    /* A broken bracket is retired in actual nesting order. No heap receipt
     * can remain current and absorb later unrelated calls. Root independently
     * faults its retained inputs before each adapter hold is released. */
    while(discard) {
        lm_frame *next=discard->next;
        for(lm_read *r=discard->reads;r;r=r->next)(void)sh_resource_instances_fault(discard->model,discard->model_token,r->origin);
        __try {
            if(discard->borrowed)(void)lm_api.input(discard->parent,NULL,0,NULL);
            else lm_api.end(discard->parent,discard->model,0,0);
        }__except(EXCEPTION_EXECUTE_HANDLER){lm_unknown();}
        lm_read_free(discard->reads);free(discard);discard=next;
    }
    int ok=returned && success && !f->failed && f->model_token && sh_resource_instances_token(f->model)==f->model_token && sh_resource_late_model_reads_ready();
    int file_seen=0;
    for(lm_read *r=f->reads;r;r=r->next) {
        if(sh_resource_instances_token(r->resource)!=r->token)ok=0;
        sh_resource_instance_origin *current=sh_resource_instances_lookup(r->resource);
        if(current!=r->origin) {
            ok=0;
            int held=current && sh_resource_instances_origin_hold(current);
            if(held) {
                __try {(void)lm_api.input(f->parent,r->resource,sh_resource_instances_token(r->resource),current);}
                __except(EXCEPTION_EXECUTE_HANDLER){lm_unknown();}
                (void)sh_resource_instances_fault(f->model,f->model_token,current);
                sh_resource_instances_origin_unhold(current);
            }
        }
        sh_resource_instances_origin_release(current);
        if(r->file_input){file_seen=1;lm_read after={0};after.resource=r->resource;
            if(!lm_file(&after) || after.file!=r->file || after.data!=r->data || after.length!=r->length ||
               after.contents!=r->contents)ok=0;}
    }
    if(f->kind==LM_WATER_LOAD && !file_seen)ok=0;
    if(ok && f->kind==LM_HOT_BUILD) {
        void *backing=NULL,*material=NULL;unsigned char flags;int count;uint64_t hash;
        if(!lm_pointer(f->model,lm_profiles[lm_renderer].hot_field,&backing,8) || backing!=f->backing ||
           !lm_pointer(backing,0x2c,&flags,1) || flags!=f->flags)ok=0;
        else if(!(flags&4)) {
            if(!lm_pointer(backing,0x78,&count,4) || count!=f->rectangle_count)ok=0;
            else if(count && (!lm_pointer(backing,0x68,&material,8) || material!=f->material ||
                    !lm_rectangle_hash(backing,count,&hash) || hash!=f->rectangles))ok=0;
        }
    }
    if(ok && f->kind!=LM_HOT_BUILD)ok=lm_commit(f,f->kind==LM_HOT_CTOR?lm_profiles[lm_renderer].hot_field:lm_profiles[lm_renderer].water_field);
    if(!ok)for(lm_read *r=f->reads;r;r=r->next)(void)sh_resource_instances_fault(f->model,f->model_token,r->origin);
    if(f->borrowed) {
        if(!ok) {
            AcquireSRWLockExclusive(&lm_lock);lm_frame *outer=lm_top(scope.context);
            if(outer && outer->parent.context==f->parent.context && outer->parent.serial==f->parent.serial)outer->failed=1;
            ReleaseSRWLockExclusive(&lm_lock);
            __try {(void)lm_api.input(f->parent,NULL,0,NULL);}__except(EXCEPTION_EXECUTE_HANDLER){lm_unknown();}
        }
    } else __try {lm_api.end(f->parent,f->model,returned,ok);}__except(EXCEPTION_EXECUTE_HANDLER){lm_unknown();}
    lm_read_free(f->reads);free(f);
}
sh_resource_late_model_scope sh_resource_late_model_hotspot_begin(void *model)
{
    sh_resource_late_model_scope scope=lm_enter(model,LM_HOT_BUILD);if(!scope.serial)return scope;
    size_t offset=lm_profiles[lm_renderer].hot_field;void *backing=NULL;uint64_t token=sh_resource_instances_token(model),source=0;
    int known=0;
    if(lm_pointer(model,offset,&backing,8)) {
        lm_read *pending=NULL;
        AcquireSRWLockExclusive(&lm_lock);
        for(lm_binding *b=lm_bindings;b;b=b->next)if(b->model==model && b->model_token==token && b->offset==offset && b->resource==backing){source=b->resource_token;known=1;break;}
        /* Constructor has stored the result but has not returned yet. */
        if(!known)for(lm_frame *f=lm_frames;f;f=f->next)if(f->scope.context==scope.context && f->kind==LM_HOT_CTOR && f->model==model && f->model_token==token)
            for(lm_read *r=f->reads;r;r=r->next)if(r->resource==backing){source=r->token;known=1;pending=r;}
        ReleaseSRWLockExclusive(&lm_lock);
        /* The outer native ctor has positively stored this exact returned
         * pointer before entering Build. Its held receipt remains active. */
        if(pending && !sh_resource_instances_assign(model,token,(uintptr_t)model+offset,backing,pending->origin))lm_failed(scope);
        sh_resource_instance_origin *origin=known?sh_resource_instances_field_origin(model,token,(uintptr_t)model+offset):NULL;
        sh_resource_instance_origin *current=sh_resource_instances_lookup(backing);
        if(origin && origin!=current) {
            lm_failed(scope);(void)sh_resource_instances_fault(model,token,origin);
            sh_resource_instances_origin_release(origin);origin=current;current=NULL;
        }
        sh_resource_instances_origin_release(current);
        /* During construction the actual stored result is still owned by its
         * outer return receipt, before the final field assignment commits. */
        (void)lm_record(scope,backing,0,origin,source);
    }
    if(!known || !source || source!=sh_resource_instances_token(backing))lm_failed(scope);
    unsigned char flags=0;int count=0;void *material=NULL;
    if(!lm_pointer(backing,0x2c,&flags,1))lm_failed(scope);
    else if(!(flags&4)) {
        if(!lm_pointer(backing,0x78,&count,4) || count<0)lm_failed(scope);
        else if(count) {
            if(!lm_pointer(backing,0x68,&material,8) || !material)lm_failed(scope);
            else (void)lm_record(scope,material,0,NULL,0);
        }
    }
    uint64_t hash=0;if(!(flags&4) && count>0 && !lm_rectangle_hash(backing,count,&hash))lm_failed(scope);
    AcquireSRWLockExclusive(&lm_lock);lm_frame *f=lm_find(scope);
    if(f){f->backing=backing;f->material=material;f->flags=flags;f->rectangle_count=count;f->rectangles=hash;}
    ReleaseSRWLockExclusive(&lm_lock);
    return scope;
}
void sh_resource_late_model_hotspot_end(sh_resource_late_model_scope scope,int returned)
{lm_leave(scope,returned,returned);}
void sh_resource_late_model_size_input(void *image)
{
    uint64_t context=sh_execution_context(NULL,0);sh_resource_late_model_scope scope={0},parent={0};int blocked=0;
    AcquireSRWLockExclusive(&lm_lock);lm_context *c=lm_context_find(context);lm_frame *f=lm_top(context);
    if(c)blocked=c->blocked!=0;if(f && f->kind==LM_HOT_BUILD){scope=f->scope;parent=f->parent;}ReleaseSRWLockExclusive(&lm_lock);
    if(scope.serial && !blocked && lm_visible(parent))(void)lm_record(scope,image,0,NULL,0);
}
void sh_resource_late_model_destroyed(void *model,uint64_t token,int completed)
{
    if(!completed || !model || !token)return;
    AcquireSRWLockExclusive(&lm_lock);lm_binding **at=&lm_bindings;
    while(*at){lm_binding *b=*at;if(b->model==model && b->model_token==token){*at=b->next;free(b);}else at=&b->next;}
    for(lm_frame *f=lm_frames;f;f=f->next)if(f->model==model && f->model_token==token)f->failed=1;
    ReleaseSRWLockExclusive(&lm_lock);
}
static void *lm_ctor_hook(void *model,void *name)
{
    sh_resource_late_model_scope scope=lm_enter(model,LM_HOT_CTOR);void *result=NULL;int returned=0;
    __try {result=lm_ctor_original(model,name);returned=1;}
    __finally {lm_leave(scope,returned,result==model);}
    return result;
}
static uint64_t lm_water_hook(void *model,void *path)
{
    sh_resource_late_model_scope scope=lm_enter(model,LM_WATER_LOAD);uint64_t result=0;int returned=0;
    __try {result=lm_water_original(model,path);returned=1;}
    __finally {lm_leave(scope,returned,result!=0);}
    return result;
}
static void lm_post_hook(void *model,unsigned char bounds)
{
    uint64_t context=sh_execution_context(NULL,0),token=sh_resource_instances_token(model);
    sh_resource_late_model_scope scope={0},parent={0};
    AcquireSRWLockExclusive(&lm_lock);lm_context *c=lm_context_find(context);lm_frame *f=lm_top(context);
    if(f && f->model==model && (!c || !c->blocked)){scope=f->scope;parent=f->parent;}
    ReleaseSRWLockExclusive(&lm_lock);
    void **expected=NULL,*surfaces=NULL,*override=NULL;int count=0,flags=0,watch=0,returned=0;
    if(scope.serial && lm_visible(parent)) {
        watch=1;
        if(!sh_resource_late_model_reads_ready() || !lm_pointer(model,0x120,&flags,4) ||
           !lm_pointer(model,0x118,&override,8) || !lm_pointer(model,0xae8,&count,4) || count<0 ||
           (size_t)count>SIZE_MAX/sizeof(void *) || !lm_pointer(model,0xae0,&surfaces,8))lm_failed(scope);
        else if((flags || override) && count) {
            expected=lm_allocate((size_t)count*sizeof(void *));if(!expected)lm_failed(scope);
            for(int i=0;i<count;i++) {
                void *material=NULL;int protected_material=0;
                if((size_t)i>SIZE_MAX/0x60 || !lm_pointer(surfaces,(size_t)i*0x60,&material,8)) {lm_failed(scope);break;}
                void *value=material;
                if(material) {
                    (void)lm_record(scope,material,0,NULL,0);
                    if(!lm_pointer(material,0x190,&protected_material,4))lm_failed(scope);
                    else if(protected_material!=1 && override){value=override;(void)lm_record(scope,override,0,NULL,0);}
                }
                if(expected)expected[i]=value;
            }
        }
    }
    __try {lm_post_original(model,bounds);returned=1;}
    __finally {
        if(watch) {
            void *actual=NULL;int after=0;
            if(!returned || token!=sh_resource_instances_token(model) || !lm_pointer(model,0xae0,&actual,8) || actual!=surfaces ||
               !lm_pointer(model,0xae8,&after,4) || after!=count)lm_failed(scope);
            else if(expected)for(int i=0;i<count;i++)if(!lm_pointer(surfaces,(size_t)i*0x60,&actual,8) || actual!=expected[i]){lm_failed(scope);break;}
        }
        free(expected);
    }
}
int sh_resource_late_model_reads_install(const unsigned char *base,sh_resource_late_model_api api)
{
    if(InterlockedCompareExchange(&lm_attempted,1,0))return sh_resource_late_model_reads_ready();
    int renderer=sh_host_is_vulkan();
    if(!base || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer) ||
       !api.begin || !api.visible || !api.input || !api.end || !api.ready)return 0;
    lm_base=base;lm_renderer=renderer;lm_api=api;const lm_profile *p=lm_profiles+renderer;
    __try {for(size_t i=0;i<p->pin_count;i++) {
        const lm_pin *pin=p->pins+i;unsigned skip=pin->rva==p->hot_ctor || pin->rva==p->water_load || pin->rva==p->post?0:pin->skip;
        if(!lm_digest(base+pin->rva+skip,pin->length-skip,skip?pin->tail_digest:pin->digest))return 0;
    }}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    lm_ctor_original=hook_prepare((void *)(base+p->hot_ctor),lm_ctor_hook,15);
    lm_water_original=hook_prepare((void *)(base+p->water_load),lm_water_hook,14);
    lm_post_original=hook_prepare((void *)(base+p->post),lm_post_hook,14);
    if(!lm_ctor_original || !lm_water_original || !lm_post_original || hook_commit(lm_ctor_original)!=B2_PATCH_OK ||
       hook_commit(lm_water_original)!=B2_PATCH_OK || hook_commit(lm_post_original)!=B2_PATCH_OK)goto failed;
    InterlockedExchange(&lm_ready,1);if(sh_resource_late_model_reads_ready())return 1;
    InterlockedExchange(&lm_ready,0);
failed:
    if(lm_post_original && hook_unpatch(lm_post_original))lm_post_original=NULL;
    if(lm_water_original && hook_unpatch(lm_water_original))lm_water_original=NULL;
    if(lm_ctor_original && hook_unpatch(lm_ctor_original))lm_ctor_original=NULL;
    return 0;
}
