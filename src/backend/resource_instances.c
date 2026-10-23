#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "resource_instances.h"

struct sh_resource_instance_origin {
    volatile LONG references, exposed;
    void *capture;
    int baseline;
    SRWLOCK lock;
    size_t live;
    void *lease;
    struct sh_resource_instance_origin **children;
    size_t child_count;
    void *source;
    uint64_t source_token;
    struct sh_resource_instance_origin *fault_parent;
    size_t fault_members;
    int quarantined;
    struct sh_resource_instance_origin *quarantine_next;
    struct sh_resource_instance_origin *quarantine_retired_next;
    int untracked_quarantined;
    struct sh_resource_instance_origin *untracked_next;
};
typedef struct ri_resource { void *object; sh_resource_instance_origin *origin; struct ri_resource *next; } ri_resource;
typedef struct ri_field {
    uintptr_t slot;
    void *resource;
    int active;
    sh_resource_instance_origin *origin;
    void *value;
    size_t value_size;
    sh_resource_instance_path path;
    int unresolved;
    struct ri_field *next;
} ri_field;
typedef struct ri_instance {
    void *object;
    uint64_t token;
    uint64_t revision;
    const void *kind;
    void *owner;
    uint64_t owner_token;
    int faulted;
    sh_resource_instance_origin *fault_group;
    uint64_t active_context;
    size_t active_depth;
    int retired;
    sh_resource_instance_origin *active_group;
    struct ri_instance *active_next;
    int incomplete;
    ri_field *fields;
    struct ri_protection *protection;
    sh_resource_instance_origin *payload;
    struct ri_instance *next;
} ri_instance;
typedef struct ri_protection { sh_resource_instance_origin *origin; struct ri_protection *next; } ri_protection;
static SRWLOCK g_ri_lock = SRWLOCK_INIT;
static SRWLOCK g_ri_drain_lock = SRWLOCK_INIT;
static sh_resource_instance_api g_ri_api;
static ri_resource **g_ri_resources;
static ri_instance **g_ri_instances;
static size_t g_ri_resource_buckets, g_ri_instance_buckets, g_ri_resource_count, g_ri_instance_count;
static uint64_t g_ri_serial;
static size_t g_ri_faults;
static ri_instance *g_ri_active;
static sh_resource_instance_origin *g_ri_quarantine;
static sh_resource_instance_origin *g_ri_untracked;

static sh_resource_instance_origin *ri_retain(sh_resource_instance_origin *origin)
{ if (origin) { InterlockedExchange(&origin->exposed,1); InterlockedIncrement(&origin->references); } return origin; }
static void ri_not_live(sh_resource_instance_origin *origin);
static sh_resource_instance_origin *ri_group(sh_resource_instance_origin *origin)
{
    while(origin && origin->fault_parent!=origin) origin=origin->fault_parent;
    return origin;
}
/* The source mapping keeps only an immutable capture. Fields share a single
 * serving lease while at least one field is live, including across clones.
 * The origin lock never nests inside the map lock; callbacks enter no native
 * code and cannot reenter this module. */
static int ri_live(sh_resource_instance_origin *origin)
{
    int ok = 1;
    if (!origin) return 1;
    AcquireSRWLockExclusive(&origin->lock);
    if (origin->live == SIZE_MAX) ok = 0;
    else if (!origin->live) {
        if (!origin->baseline && g_ri_api.promote(origin->capture, &origin->lease) != 1) ok = 0;
        size_t held = 0;
        for (; ok && held < origin->child_count; held++) if (!ri_live(origin->children[held])) { ok = 0; break; }
        if (!ok) while (held) ri_not_live(origin->children[--held]);
    }
    if (ok) origin->live++;
    ReleaseSRWLockExclusive(&origin->lock); return ok;
}
static void ri_not_live(sh_resource_instance_origin *origin)
{
    void *lease = NULL; int children = 0;
    if (!origin) return;
    AcquireSRWLockExclusive(&origin->lock);
    if (origin->live && !--origin->live) { lease = origin->lease; origin->lease = NULL; children = 1; }
    ReleaseSRWLockExclusive(&origin->lock);
    if (lease) g_ri_api.release_lease(lease);
    if (children) for (size_t i = 0; i < origin->child_count; i++) ri_not_live(origin->children[i]);
}
int sh_resource_instances_origin_hold(sh_resource_instance_origin *origin)
{
    if (!origin) return 0;
    ri_retain(origin);
    if (ri_live(origin)) return 1;
    sh_resource_instances_origin_release(origin); return 0;
}
void sh_resource_instances_origin_unhold(sh_resource_instance_origin *origin)
{ if (origin) { ri_not_live(origin); sh_resource_instances_origin_release(origin); } }
int sh_resource_instances_origin_matches(const sh_resource_instance_origin *origin, const void *capture, int baseline)
{ return origin && !origin->child_count && origin->capture == capture && origin->baseline == !!baseline; }
void sh_resource_instances_origin_release(sh_resource_instance_origin *origin)
{
    if (origin && !InterlockedDecrement(&origin->references)) {
        if (origin->capture) g_ri_api.release_capture(origin->capture);
        for (size_t i = 0; i < origin->child_count; i++) sh_resource_instances_origin_release(origin->children[i]);
        free(origin->children);
        free(origin);
    }
}
static void ri_field_free(ri_field *field)
{
    while (field) {
        ri_field *next = field->next;
        if (field->active) ri_not_live(field->origin);
        sh_resource_instances_origin_release(field->origin);
        free(field->value);
        free((void *)field->path.steps);
        free(field); field = next;
    }
}
static void ri_protection_free(ri_protection *p)
{ while (p) { ri_protection *next = p->next; sh_resource_instances_origin_unhold(p->origin); free(p); p = next; } }
static void ri_fault_drain(void)
{
    sh_resource_instance_origin *retired=NULL;
    AcquireSRWLockExclusive(&g_ri_drain_lock);
    AcquireSRWLockExclusive(&g_ri_lock);
    sh_resource_instance_origin **slot=&g_ri_quarantine;
    while(*slot) {
        sh_resource_instance_origin *p=*slot;
        if(!ri_group(p)->fault_members) {
            *slot=p->quarantine_next; p->quarantined=0; p->quarantine_next=NULL;
            p->quarantine_retired_next=retired; retired=p;
        } else slot=&p->quarantine_next;
    }
    /* Detach every member before any parent origin can be released. */
    for(sh_resource_instance_origin *p=retired;p;p=p->quarantine_retired_next) p->fault_parent=NULL;
    ReleaseSRWLockExclusive(&g_ri_lock);
    while(retired) { sh_resource_instance_origin *next=retired->quarantine_retired_next;
        retired->quarantine_retired_next=NULL; sh_resource_instances_origin_unhold(retired); retired=next; }
    ReleaseSRWLockExclusive(&g_ri_drain_lock);
}
static void ri_instance_free(ri_instance *instance)
{
    if(!instance) return;
    AcquireSRWLockExclusive(&g_ri_lock);
    if(instance->active_depth) { instance->retired=1; ReleaseSRWLockExclusive(&g_ri_lock); return; }
    ReleaseSRWLockExclusive(&g_ri_lock);
    ri_field_free(instance->fields); ri_protection_free(instance->protection);
    sh_resource_instances_origin_unhold(instance->payload);
    if(instance->faulted) {
        AcquireSRWLockExclusive(&g_ri_lock); if(g_ri_faults) g_ri_faults--;
        if(instance->fault_group) { sh_resource_instance_origin *root=ri_group(instance->fault_group); if(root->fault_members) root->fault_members--; }
        ReleaseSRWLockExclusive(&g_ri_lock);
        ri_fault_drain();
    }
    free(instance);
}
static ri_instance **ri_instance_slot(const void *object)
{
    uintptr_t hash = (uintptr_t)object >> 3;
    hash ^= hash >> 33; hash *= UINT64_C(0xff51afd7ed558ccd); hash ^= hash >> 33;
    ri_instance **slot = &g_ri_instances[hash & (g_ri_instance_buckets - 1)];
    while (*slot && (*slot)->object != object) slot = &(*slot)->next;
    return slot;
}
static size_t ri_hash(const void *object, size_t buckets)
{
    uintptr_t hash = (uintptr_t)object >> 3;
    hash ^= hash >> 33; hash *= UINT64_C(0xff51afd7ed558ccd); hash ^= hash >> 33;
    return hash & (buckets - 1);
}
static int ri_resources_grow(void)
{
    if (g_ri_resource_count < g_ri_resource_buckets / 2) return 1;
    size_t count = g_ri_resource_buckets ? g_ri_resource_buckets * 2 : 64;
    if (count < g_ri_resource_buckets || count > SIZE_MAX / sizeof(ri_resource *)) return 0;
    ri_resource **buckets = calloc(count, sizeof(*buckets)); if (!buckets) return 0;
    for (size_t i = 0; i < g_ri_resource_buckets; i++) {
        ri_resource *r = g_ri_resources[i];
        while (r) { ri_resource *next = r->next; size_t at = ri_hash(r->object, count);
            r->next = buckets[at]; buckets[at] = r; r = next; }
    }
    free(g_ri_resources); g_ri_resources = buckets; g_ri_resource_buckets = count; return 1;
}
static int ri_instances_grow(void)
{
    if (g_ri_instance_count < g_ri_instance_buckets / 2) return 1;
    size_t count = g_ri_instance_buckets ? g_ri_instance_buckets * 2 : 64;
    if (count < g_ri_instance_buckets || count > SIZE_MAX / sizeof(ri_instance *)) return 0;
    ri_instance **buckets = calloc(count, sizeof(*buckets)); if (!buckets) return 0;
    for (size_t i = 0; i < g_ri_instance_buckets; i++) {
        ri_instance *r = g_ri_instances[i];
        while (r) { ri_instance *next = r->next; size_t at = ri_hash(r->object, count);
            r->next = buckets[at]; buckets[at] = r; r = next; }
    }
    free(g_ri_instances); g_ri_instances = buckets; g_ri_instance_buckets = count; return 1;
}
static uint64_t ri_next(void)
{
    if (g_ri_serial == UINT64_MAX) return 0;
    return ++g_ri_serial;
}
int sh_resource_instances_bind(sh_resource_instance_api api)
{
    int ok = 0;
    if (!api.promote || !api.release_capture || !api.release_lease) return 0;
    AcquireSRWLockExclusive(&g_ri_lock);
    if (!g_ri_resources && !g_ri_instances && !g_ri_api.promote) { g_ri_api = api; ok = 1; }
    ReleaseSRWLockExclusive(&g_ri_lock); return ok;
}
sh_resource_instance_origin *sh_resource_instances_origin(void *capture, int baseline)
{
    sh_resource_instance_origin *origin;
    if (!g_ri_api.promote || (!!capture == !!baseline)) return NULL;
    origin = calloc(1, sizeof(*origin));
    if (origin) { origin->references = 1; origin->capture = capture; origin->baseline = baseline != 0;
        InitializeSRWLock(&origin->lock); }
    return origin;
}
sh_resource_instance_origin *sh_resource_instances_origin_composite(sh_resource_instance_origin *const *children, size_t count)
{
    if ((count && !children) || count > SIZE_MAX / sizeof(*children)) return NULL;
    sh_resource_instance_origin *origin = sh_resource_instances_origin(NULL,1);
    if (!origin) return NULL;
    if (count && !(origin->children = calloc(count,sizeof(*origin->children)))) {
        sh_resource_instances_origin_release(origin); return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (!children[i]) { sh_resource_instances_origin_release(origin); return NULL; }
        size_t j = 0; while (j < origin->child_count && origin->children[j] != children[i]) j++;
        if (j == origin->child_count) origin->children[origin->child_count++] = ri_retain(children[i]);
    }
    return origin;
}
int sh_resource_instances_publish(void *resource, sh_resource_instance_origin *origin)
{
    ri_resource *fresh, **slot;
    sh_resource_instance_origin *prior = NULL;
    if (!resource || !origin || !(fresh = calloc(1, sizeof(*fresh)))) return 0;
    fresh->object = resource; fresh->origin = ri_retain(origin);
    AcquireSRWLockExclusive(&g_ri_lock);
    if (!ri_resources_grow()) { ReleaseSRWLockExclusive(&g_ri_lock);
        sh_resource_instances_origin_release(fresh->origin); free(fresh); return 0; }
    slot = &g_ri_resources[ri_hash(resource, g_ri_resource_buckets)];
    while (*slot && (*slot)->object != resource) slot = &(*slot)->next;
    if (*slot) { prior = (*slot)->origin; (*slot)->origin = fresh->origin; free(fresh); }
    else { fresh->next = NULL; *slot = fresh; g_ri_resource_count++; }
    ReleaseSRWLockExclusive(&g_ri_lock);
    sh_resource_instances_origin_release(prior); return 1;
}
void sh_resource_instances_forget(void *resource)
{
    ri_resource **slot, *removed = NULL;
    AcquireSRWLockExclusive(&g_ri_lock);
    if (!g_ri_resource_buckets) { ReleaseSRWLockExclusive(&g_ri_lock); return; }
    slot = &g_ri_resources[ri_hash(resource, g_ri_resource_buckets)];
    while (*slot && (*slot)->object != resource) slot = &(*slot)->next;
    if (*slot) { removed = *slot; *slot = removed->next; g_ri_resource_count--; }
    ReleaseSRWLockExclusive(&g_ri_lock);
    if (removed) { sh_resource_instances_origin_release(removed->origin); free(removed); }
}
sh_resource_instance_origin *sh_resource_instances_lookup(void *resource)
{
    sh_resource_instance_origin *origin = NULL;
    AcquireSRWLockShared(&g_ri_lock);
    if (g_ri_resource_buckets) for (ri_resource *r = g_ri_resources[ri_hash(resource, g_ri_resource_buckets)]; r; r = r->next)
        if (r->object == resource) { origin = ri_retain(r->origin); break; }
    ReleaseSRWLockShared(&g_ri_lock); return origin;
}
uint64_t sh_resource_instances_construct(void *object, int observed)
{
    ri_instance *fresh, *prior = NULL; uint64_t token = 0;
    if (!object || !(fresh = calloc(1, sizeof(*fresh)))) return 0;
    fresh->object = object; fresh->incomplete = !observed;
    AcquireSRWLockExclusive(&g_ri_lock);
    if (ri_instances_grow()) token = fresh->token = ri_next();
    if (token) {
        ri_instance **slot = ri_instance_slot(object);
        prior = *slot; fresh->next = prior ? prior->next : NULL; *slot = fresh;
        if (!prior) g_ri_instance_count++;
    }
    ReleaseSRWLockExclusive(&g_ri_lock);
    if (!token) free(fresh);
    ri_instance_free(prior); return token;
}
uint64_t sh_resource_instances_token(const void *object)
{
    uint64_t token;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    token = instance ? instance->token : 0;
    ReleaseSRWLockShared(&g_ri_lock); return token;
}
int sh_resource_instances_protect(void *object, uint64_t token, sh_resource_instance_origin *origin)
{
    int ok = 0; ri_protection *fresh;
    if (!object || !token || !origin || !(fresh = calloc(1,sizeof(*fresh)))) return 0;
    if (!sh_resource_instances_origin_hold(origin)) { free(fresh); return 0; }
    fresh->origin = origin;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token && instance->revision != UINT64_MAX) {
        ri_protection *p = instance->protection;
        while (p && p->origin != origin) p = p->next;
        if (!p) { fresh->next = instance->protection; instance->protection = fresh; fresh = NULL; instance->revision++; }
        ok = 1;
    }
    ReleaseSRWLockExclusive(&g_ri_lock); ri_protection_free(fresh); return ok;
}
void sh_resource_instances_unprotect(void *object, uint64_t token)
{
    ri_protection *removed = NULL;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token && instance->revision != UINT64_MAX) {
        removed = instance->protection; instance->protection = NULL; instance->revision++;
    }
    ReleaseSRWLockExclusive(&g_ri_lock); ri_protection_free(removed);
}
int sh_resource_instances_payload(void *object, uint64_t token, sh_resource_instance_origin *origin)
{
    sh_resource_instance_origin *prior = NULL; int ok = 0;
    if (!object || !token || (origin && !sh_resource_instances_origin_hold(origin))) return 0;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token && instance->revision != UINT64_MAX) {
        prior = instance->payload; instance->payload = origin; origin = NULL; instance->revision++; ok = 1;
    }
    ReleaseSRWLockExclusive(&g_ri_lock);
    sh_resource_instances_origin_unhold(prior); sh_resource_instances_origin_unhold(origin); return ok;
}
static int ri_payload_uses(const sh_resource_instance_origin *origin,int (*contains)(void *,void *),void *context)
{
    if(!origin) return 0;
    /* Identity is initialized before any retained/publication reference and
     * remains immutable. Never nest an origin/provider lock under map lock. */
    void *source=origin->source; uint64_t token=origin->source_token;
    ri_instance *actual=source && g_ri_instance_buckets?*ri_instance_slot(source):NULL;
    if(actual && actual->token==token && contains(context,source)) return 1;
    for(size_t i=0;i<origin->child_count;i++) if(ri_payload_uses(origin->children[i],contains,context)) return 1;
    return 0;
}
int sh_resource_instances_payload_uses(const void *object,uint64_t token,int (*contains)(void *,void *),void *context)
{
    if(!contains) return 0;
    int affected=0; AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(instance && instance->token==token) affected=ri_payload_uses(instance->payload,contains,context);
    ReleaseSRWLockShared(&g_ri_lock); return affected;
}
int sh_resource_instances_holders_where(int (*contains)(void *,void *),
    int (*visit)(void *,void *,uint64_t), void *context)
{
    typedef struct ri_holder { void *object; uint64_t token; } ri_holder;
    ri_holder *holders = NULL; size_t count = 0; int ok = 1;
    if (!contains || !visit) return 0;
    AcquireSRWLockShared(&g_ri_lock);
    if (g_ri_instance_count > SIZE_MAX / sizeof(*holders)) ok = 0;
    else if (g_ri_instance_count && !(holders = malloc(g_ri_instance_count * sizeof(*holders)))) ok = 0;
    if (ok) for (size_t i = 0; i < g_ri_instance_buckets; i++)
        for (ri_instance *instance = g_ri_instances[i]; instance; instance = instance->next) {
            int selected=ri_payload_uses(instance->payload,contains,context);
            for(ri_field *f=instance->fields;!selected && f;f=f->next) selected=contains(context,f->resource);
            if(selected) holders[count++]=(ri_holder){instance->object,instance->token};
        }
    ReleaseSRWLockShared(&g_ri_lock);
    for (size_t i = 0; ok && i < count; i++) ok = visit(context,holders[i].object,holders[i].token) != 0;
    free(holders); return ok;
}
typedef struct ri_one_holder { void *resource; int (*visit)(void *,void *,uint64_t); void *context; } ri_one_holder;
int sh_resource_instances_unknown_inputs(const void *object,uint64_t token,
    int (*contains)(void *,void *),void *context)
{
    if(!contains) return 1;
    int unknown=0;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(instance && instance->token==token) for(ri_field *f=instance->fields;f;f=f->next)
        if(f->resource && (!f->origin || f->unresolved) && contains(context,f->resource)) { unknown=1; break; }
    ReleaseSRWLockShared(&g_ri_lock); return unknown;
}
int sh_resource_instances_unknown_fields(const void *object,uint64_t token,
    int (*contains)(void *,void *),void *context,
    int (*visit)(void *,void *,uintptr_t,int),void *visit_context)
{
    typedef struct { void *resource; uintptr_t slot; int unresolved; } ri_unknown;
    ri_unknown *found=NULL; size_t count=0,capacity=0; int ok=1;
    if(!contains || !visit) return 0;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(instance && instance->token==token) for(ri_field *f=instance->fields;f && ok;f=f->next) {
        if(!f->resource || (f->origin && !f->unresolved) || !contains(context,f->resource)) continue;
        if(count==capacity) {
            size_t next=capacity?capacity*2:8;
            ri_unknown *grown=next<=SIZE_MAX/sizeof(*grown)?realloc(found,next*sizeof(*grown)):NULL;
            if(!grown) { ok=0; break; }
            found=grown; capacity=next;
        }
        found[count++]=(ri_unknown){f->resource,f->slot,f->unresolved};
    }
    ReleaseSRWLockShared(&g_ri_lock);
    for(size_t i=0;ok && i<count;i++) ok=visit(visit_context,found[i].resource,found[i].slot,found[i].unresolved)!=0;
    free(found); return ok;
}
static int ri_one_contains(void *context,void *resource) { return resource == ((ri_one_holder *)context)->resource; }
static int ri_one_visit(void *context,void *object,uint64_t token) {
    ri_one_holder *one=context; return one->visit(one->context,object,token);
}
int sh_resource_instances_holders(void *resource,int (*visit)(void *,void *,uint64_t),void *context)
{
    if (!resource || !visit) return 0;
    ri_one_holder one={resource,visit,context};
    return sh_resource_instances_holders_where(ri_one_contains,ri_one_visit,&one);
}
int sh_resource_instances_classify(void *object,uint64_t token,const void *kind,void *owner)
{
    int ok=0;
    if(!object || !token || !kind || !owner) return 0;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    ri_instance *actual_owner=g_ri_instance_buckets?*ri_instance_slot(owner):NULL;
    if(instance && instance->token==token && actual_owner && (!instance->kind ||
        (instance->kind==kind && instance->owner==owner && instance->owner_token==actual_owner->token))) {
        if(!instance->kind) { instance->kind=kind; instance->owner=owner; instance->owner_token=actual_owner->token; instance->revision++; }
        ok=1;
    }
    ReleaseSRWLockExclusive(&g_ri_lock); return ok;
}
void *sh_resource_instances_class_owner(const void *object,uint64_t token,const void *kind)
{
    void *owner=NULL;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(instance && instance->token==token && instance->kind==kind) {
        ri_instance *actual_owner=*ri_instance_slot(instance->owner);
        if(actual_owner && actual_owner->token==instance->owner_token) owner=instance->owner;
    }
    ReleaseSRWLockShared(&g_ri_lock); return owner;
}
int sh_resource_instances_origin_captures(sh_resource_instance_origin *origin,
    int (*visit)(void *,const void *),void *context)
{
    if(!origin || !visit) return 0;
    if(origin->capture && !visit(context,origin->capture)) return 0;
    for(size_t i=0;i<origin->child_count;i++)
        if(!sh_resource_instances_origin_captures(origin->children[i],visit,context)) return 0;
    return 1;
}
static int ri_compiled(const sh_resource_instance_origin *origin)
{
    if(!origin) return 0;
    if(origin->capture) return 1;
    for(size_t i=0;i<origin->child_count;i++) if(ri_compiled(origin->children[i])) return 1;
    return 0;
}
int sh_resource_instances_origin_source(sh_resource_instance_origin *origin,void *resource,uint64_t token)
{
    if(!origin || !resource || !token) return 0;
    int ok=0;
    AcquireSRWLockExclusive(&origin->lock);
    if(origin->source) ok=origin->source==resource && origin->source_token==token;
    else if(!InterlockedCompareExchange(&origin->exposed,0,0)) {
        origin->source_token=token; origin->source=resource; ok=1;
    }
    ReleaseSRWLockExclusive(&origin->lock); return ok;
}
static int ri_origin_sources(sh_resource_instance_origin *origin,
    int (*visit)(void *,void *,uint64_t,const sh_resource_instance_origin *),void *context,
    sh_resource_instance_origin *group)
{
    if(origin->capture) {
        AcquireSRWLockShared(&origin->lock);
        void *source=origin->source; uint64_t token=origin->source_token;
        ReleaseSRWLockShared(&origin->lock);
        if(!visit(context,source,token,group)) return 0;
    }
    for(size_t i=0;i<origin->child_count;i++) if(!ri_origin_sources(origin->children[i],visit,context,group)) return 0;
    return 1;
}
int sh_resource_instances_fault_sources(int (*visit)(void *,void *,uint64_t,const sh_resource_instance_origin *),void *context)
{
    if(!visit) return 0;
    size_t count=0,at=0; int ok=1;
    sh_resource_instance_origin **origins=NULL;
    AcquireSRWLockShared(&g_ri_lock);
    for(sh_resource_instance_origin *p=g_ri_quarantine;p;p=p->quarantine_next) count++;
    for(sh_resource_instance_origin *p=g_ri_untracked;p;p=p->untracked_next) count++;
    if(count>SIZE_MAX/sizeof(*origins) || (count && !(origins=malloc(count*sizeof(*origins))))) ok=0;
    if(ok) {
        for(sh_resource_instance_origin *p=g_ri_quarantine;p;p=p->quarantine_next) origins[at++]=ri_retain(p);
        for(sh_resource_instance_origin *p=g_ri_untracked;p;p=p->untracked_next) origins[at++]=ri_retain(p);
    }
    ReleaseSRWLockShared(&g_ri_lock);
    for(size_t i=0;i<at;i++) {
        if(ok) ok=ri_origin_sources(origins[i],visit,context,origins[i]);
        sh_resource_instances_origin_release(origins[i]);
    }
    free(origins); return ok;
}
int sh_resource_instances_fault_group_holders(const sh_resource_instance_origin *group,
    int (*visit)(void *,void *,uint64_t),void *context)
{
    typedef struct { void *object; uint64_t token; } ri_holder;
    if(!group || !visit) return 0;
    ri_holder *holders=NULL; size_t count=0; int ok=1;
    AcquireSRWLockShared(&g_ri_lock);
    sh_resource_instance_origin *root=ri_group((sh_resource_instance_origin *)group);
    if(g_ri_faults>SIZE_MAX/sizeof(*holders) || (g_ri_faults && !(holders=malloc(g_ri_faults*sizeof(*holders))))) ok=0;
    if(ok && root) for(size_t i=0;i<g_ri_instance_buckets;i++) for(ri_instance *p=g_ri_instances[i];p;p=p->next)
        if(p->faulted && ri_group(p->fault_group)==root) holders[count++]=(ri_holder){p->object,p->token};
    ReleaseSRWLockShared(&g_ri_lock);
    for(size_t i=0;ok && i<count;i++) ok=visit(context,holders[i].object,holders[i].token);
    free(holders); return ok;
}
static int ri_fault_join(ri_instance *instance,sh_resource_instance_origin *group)
{
    sh_resource_instance_origin *root=ri_group(group);
    if(!instance || !root || (!instance->faulted && g_ri_faults==SIZE_MAX)) return 0;
    if(!instance->faulted) { instance->faulted=1; if(instance->revision!=UINT64_MAX) instance->revision++; g_ri_faults++; }
    sh_resource_instance_origin *prior=ri_group(instance->fault_group);
    if(!prior) { instance->fault_group=root; root->fault_members++; }
    else if(prior!=root) { root->fault_parent=prior; prior->fault_members+=root->fault_members; }
    sh_resource_instance_origin *active=ri_group(instance->active_group),*bound=ri_group(instance->fault_group);
    if(active && active!=bound) { active->fault_parent=bound; bound->fault_members+=active->fault_members; }
    return 1;
}
static void ri_fault_untracked(sh_resource_instance_origin *group)
{
    sh_resource_instance_origin *root=ri_group(group);
    if(!root) return;
    for(sh_resource_instance_origin *p=g_ri_quarantine;p;p=p->quarantine_next)
        if(ri_group(p)==root && !p->untracked_quarantined) {
            /* The map-owned quarantine share guarantees live>0 throughout;
             * this share cannot promote or enter a provider callback. */
            if(sh_resource_instances_origin_hold(p)) {
                p->untracked_quarantined=1; p->untracked_next=g_ri_untracked; g_ri_untracked=p;
            }
        }
}
int sh_resource_instances_fault_group_active(const sh_resource_instance_origin *group)
{
    int active=0;
    AcquireSRWLockShared(&g_ri_lock);
    sh_resource_instance_origin *root=ri_group((sh_resource_instance_origin *)group);
    if(root) for(ri_instance *p=g_ri_active;p;p=p->active_next)
        if(ri_group(p->active_group)==root) { active=1; break; }
    ReleaseSRWLockShared(&g_ri_lock); return active;
}
int sh_resource_instances_fault_share(void *source,uint64_t source_token,void *object,uint64_t token)
{
    int ok=0;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *from=g_ri_instance_buckets?*ri_instance_slot(source):NULL;
    ri_instance *to=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(from && to && from->token==source_token && to->token==token)
        ok=from->fault_group?ri_fault_join(to,from->fault_group):!from->faulted;
    else if(from && from->token==source_token && !token) ri_fault_untracked(from->fault_group);
    ReleaseSRWLockExclusive(&g_ri_lock); return ok;
}
int sh_resource_instances_fault_scope_begin(void *object,uint64_t token,uint64_t context)
{
    int ok=0;
    if(!context || !token) return 0;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(instance && instance->token==token) {
        if(!instance->fault_group) ok=!instance->faulted;
        else if(instance->active_depth) {
            if(instance->active_context==context && instance->active_depth<SIZE_MAX) { instance->active_depth++; ok=1; }
        } else {
            instance->active_context=context; instance->active_depth=1;
            instance->active_group=ri_group(instance->fault_group); instance->active_group->fault_members++;
            instance->active_next=g_ri_active; g_ri_active=instance; ok=1;
        }
    }
    ReleaseSRWLockExclusive(&g_ri_lock); return ok;
}
int sh_resource_instances_fault_scope_adopt(uint64_t context,void *object,uint64_t token)
{
    int ok=1;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *to=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    for(ri_instance *p=g_ri_active;p;p=p->active_next) if(p->active_context==context) {
        if(!to || to->token!=token || !token) { if(!token) ri_fault_untracked(p->active_group); ok=0; }
        else if(!ri_fault_join(to,p->active_group)) ok=0;
    }
    ReleaseSRWLockExclusive(&g_ri_lock); return ok;
}
static ri_instance *ri_fault_scope_remove(uint64_t context,void *object,uint64_t token,int retiring)
{
    ri_instance *removed=NULL;
    ri_instance **slot=&g_ri_active;
    while(*slot) {
        ri_instance *p=*slot;
        if(p->active_context!=context || (!retiring && (p->object!=object || p->token!=token))) { slot=&p->active_next; continue; }
        if(!retiring && p->active_depth>1) { p->active_depth--; break; }
        *slot=p->active_next; p->active_next=NULL; p->active_depth=0; p->active_context=0;
        sh_resource_instance_origin *root=ri_group(p->active_group);
        if(root && root->fault_members) root->fault_members--;
        p->active_group=NULL;
        if(p->retired) { p->active_next=removed; removed=p; }
        if(!retiring) break;
    }
    return removed;
}
void sh_resource_instances_fault_scope_end(void *object,uint64_t token,uint64_t context)
{
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *removed=ri_fault_scope_remove(context,object,token,0);
    ReleaseSRWLockExclusive(&g_ri_lock);
    if(removed) ri_instance_free(removed); ri_fault_drain();
}
void sh_resource_instances_fault_scope_retire(uint64_t context)
{
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *removed=ri_fault_scope_remove(context,NULL,0,1);
    ReleaseSRWLockExclusive(&g_ri_lock);
    while(removed) { ri_instance *next=removed->active_next; removed->active_next=NULL; ri_instance_free(removed); removed=next; }
    ri_fault_drain();
}
int sh_resource_instances_fault(void *object,uint64_t token,sh_resource_instance_origin *origin)
{
    if(!origin) return 0;
    if(!ri_compiled(origin)) return 1;
    if(!sh_resource_instances_origin_hold(origin)) return 0;
    int ok=0,transferred=0;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance=object && g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(!token) {
        if(!origin->untracked_quarantined) {
            origin->untracked_quarantined=1; origin->untracked_next=g_ri_untracked; g_ri_untracked=origin; transferred=1;
        }
    }
    if(instance && instance->token==token) {
        if(instance->faulted || g_ri_faults<SIZE_MAX) {
            if(!origin->quarantined) {
                origin->quarantined=1; origin->fault_parent=origin; origin->fault_members=0;
                origin->quarantine_next=g_ri_quarantine; g_ri_quarantine=origin; transferred=1;
            }
            ok=ri_fault_join(instance,origin);
        }
    }
    ReleaseSRWLockExclusive(&g_ri_lock);
    if(!transferred) sh_resource_instances_origin_unhold(origin);
    return ok;
}
int sh_resource_instances_fault_recovered(void *object,uint64_t token)
{
    int ok=0;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(instance && instance->token==token) {
        if(instance->faulted) { instance->faulted=0; instance->revision++; if(g_ri_faults) g_ri_faults--;
            if(instance->fault_group) { sh_resource_instance_origin *root=ri_group(instance->fault_group);
                if(root->fault_members) root->fault_members--; instance->fault_group=NULL; } }
        ok=1;
    }
    ReleaseSRWLockExclusive(&g_ri_lock); ri_fault_drain(); return ok;
}
int sh_resource_instances_mark_fault(void *object,uint64_t token)
{
    int ok=0;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance=object && g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(token && instance && instance->token==token) {
        if(!instance->faulted && g_ri_faults<SIZE_MAX) { instance->faulted=1; instance->revision++; g_ri_faults++; }
        ok=instance->faulted;
    }
    ReleaseSRWLockExclusive(&g_ri_lock); return ok;
}
int sh_resource_instances_faulted(const void *object,uint64_t token)
{
    int faulted=0;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(instance && instance->token==token) faulted=instance->faulted;
    ReleaseSRWLockShared(&g_ri_lock); return faulted;
}
int sh_resource_instances_faults(int (*visit)(void *,void *,uint64_t),void *context)
{
    typedef struct ri_fault_holder { void *object; uint64_t token; } ri_fault_holder;
    if(!visit) return 0;
    ri_fault_holder *holders=NULL; size_t count=0; int ok=1;
    AcquireSRWLockShared(&g_ri_lock);
    if(g_ri_faults>SIZE_MAX/sizeof(*holders) || (g_ri_faults && !(holders=malloc(g_ri_faults*sizeof(*holders))))) ok=0;
    if(ok) for(size_t i=0;i<g_ri_instance_buckets;i++) for(ri_instance *p=g_ri_instances[i];p;p=p->next)
        if(p->faulted) holders[count++]=(ri_fault_holder){p->object,p->token};
    ReleaseSRWLockShared(&g_ri_lock);
    for(size_t i=0;ok && i<count;i++) ok=visit(context,holders[i].object,holders[i].token);
    free(holders); return ok;
}
sh_resource_instance_origin *sh_resource_instances_field_origin(const void *object, uint64_t token, uintptr_t slot)
{
    sh_resource_instance_origin *origin = NULL;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token) for (ri_field *f = instance->fields; f; f = f->next)
        if (f->slot == slot && !f->unresolved) { origin = ri_retain(f->origin); break; }
    ReleaseSRWLockShared(&g_ri_lock); return origin;
}
/* Draft for resource_instances.c: no map/origin lock nesting and no promotion. */
static sh_resource_instance_origin *ri_share_existing(sh_resource_instance_origin *origin)
{
    int held=0;
    if(origin) {
        AcquireSRWLockExclusive(&origin->lock);
        if(origin->live && origin->live<SIZE_MAX) { origin->live++; held=1; }
        ReleaseSRWLockExclusive(&origin->lock);
    }
    if(!held) { sh_resource_instances_origin_release(origin); return NULL; }
    return origin; /* metadata reference was already acquired under map lock */
}
static ri_field *ri_copy_match(ri_instance *instance,uint64_t token,uintptr_t slot,
    const void *bytes,size_t length,sh_resource_instance_origin *origin)
{
    if(!instance || instance->token!=token || !bytes || !length) return NULL;
    for(ri_field *f=instance->fields;f;f=f->next)
        if(!f->path.count && f->slot==slot && f->value_size==length && f->active && !f->unresolved &&
            f->origin && (!origin || f->origin==origin) && !memcmp(f->value,bytes,length)) return f;
    return NULL;
}
sh_resource_instance_origin *sh_resource_instances_copy_hold(const void *object,uint64_t token,
    uintptr_t slot,const void *bytes,size_t length)
{
    sh_resource_instance_origin *origin=NULL;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    ri_field *field=ri_copy_match(instance,token,slot,bytes,length,NULL);
    if(field) origin=ri_retain(field->origin);
    ReleaseSRWLockShared(&g_ri_lock);
    origin=ri_share_existing(origin); if(!origin) return NULL;
    AcquireSRWLockShared(&g_ri_lock);
    instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    int matches=ri_copy_match(instance,token,slot,bytes,length,origin)!=NULL;
    ReleaseSRWLockShared(&g_ri_lock);
    if(!matches) { sh_resource_instances_origin_unhold(origin); return NULL; }
    return origin;
}

sh_resource_instance_origin *sh_resource_instances_payload_hold(const void *object,uint64_t token)
{
    sh_resource_instance_origin *origin=NULL;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    if(instance && instance->token==token) origin=ri_retain(instance->payload);
    ReleaseSRWLockShared(&g_ri_lock);
    origin=ri_share_existing(origin); if(!origin) return NULL;
    AcquireSRWLockShared(&g_ri_lock);
    instance=g_ri_instance_buckets?*ri_instance_slot(object):NULL;
    int matches=instance && instance->token==token && instance->payload==origin;
    ReleaseSRWLockShared(&g_ri_lock);
    if(!matches) { sh_resource_instances_origin_unhold(origin); return NULL; }
    return origin;
}
static ri_field *ri_field_make(uintptr_t slot, void *resource, sh_resource_instance_origin *origin,
    const void *value, size_t value_size)
{
    ri_field *field = calloc(1, sizeof(*field));
    if (!field) return NULL;
    field->slot = slot; field->resource = resource; field->origin = ri_retain(origin);
    if (value_size) {
        field->value = malloc(value_size);
        if (!field->value) { ri_field_free(field); return NULL; }
        memcpy(field->value, value, value_size); field->value_size = value_size;
    }
    if (!ri_live(origin)) {
        ri_field_free(field); return NULL;
    }
    field->active = origin != NULL;
    return field;
}
static int ri_path_valid(const sh_resource_instance_path *path)
{
    if (!path || !path->count || !path->steps || !path->root_size ||
        path->count > SIZE_MAX / sizeof(*path->steps)) return 0;
    size_t extent = path->root_size;
    for (size_t i = 0; i < path->count; i++) {
        const sh_resource_instance_step *s = &path->steps[i];
        if (!s->type || !s->stride || extent < 12 || s->offset > extent - 12 ||
            s->index > (SIZE_MAX - s->stride) / s->stride) return 0;
        extent = s->stride;
    }
    return extent >= sizeof(void *) && path->leaf <= extent - sizeof(void *);
}
static int ri_path_copy(ri_field *field, const sh_resource_instance_path *path)
{
    if (!path || !path->count) return 1;
    if (!ri_path_valid(path)) return 0;
    sh_resource_instance_step *steps = malloc(path->count * sizeof(*steps));
    if (!steps) return 0;
    memcpy(steps, path->steps, path->count * sizeof(*steps));
    field->path = *path; field->path.steps = steps; return 1;
}
static int ri_path_equal(const sh_resource_instance_path *a, const sh_resource_instance_path *b)
{
    return a->count && a->count == b->count && a->root_type == b->root_type &&
        a->context == b->context && a->root_size == b->root_size && a->leaf == b->leaf &&
        !memcmp(a->steps, b->steps, a->count * sizeof(*a->steps));
}
static int ri_assign(void *object, uint64_t token, uintptr_t slot,
    void *resource, sh_resource_instance_origin *origin, const void *value, size_t value_size,
    const sh_resource_instance_path *path)
{
    ri_field *fresh = NULL, *prior = NULL; int ok = 0;
    if (!object || !token || !slot || (!resource && origin) || (path && !ri_path_valid(path))) return 0;
    if (resource && !(fresh = ri_field_make(slot, resource, origin, value, value_size))) {
        AcquireSRWLockExclusive(&g_ri_lock);
        ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
        if (instance && instance->token == token) instance->incomplete = 1;
        ReleaseSRWLockExclusive(&g_ri_lock); return 0;
    }
    if (fresh && !ri_path_copy(fresh, path)) { ri_field_free(fresh); return 0; }
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token && instance->revision != UINT64_MAX) {
        ri_field **field = &instance->fields;
        while (*field && !(path ? ri_path_equal(&(*field)->path, path) ||
            (!(*field)->path.count && (*field)->slot == slot) : !(*field)->path.count && (*field)->slot == slot))
            field = &(*field)->next;
        prior = *field;
        if (fresh) { fresh->next = prior ? prior->next : NULL; *field = fresh; fresh = NULL; }
        else if (prior) *field = prior->next;
        if (prior) prior->next = NULL;
        instance->revision++; ok = 1;
    }
    ReleaseSRWLockExclusive(&g_ri_lock);
    ri_field_free(prior); ri_field_free(fresh); return ok;
}
int sh_resource_instances_assign(void *object, uint64_t token, uintptr_t slot,
    void *resource, sh_resource_instance_origin *origin)
{ return ri_assign(object, token, slot, resource, origin, NULL, 0, NULL); }
int sh_resource_instances_assign_path(void *object, uint64_t token, uintptr_t slot,
    void *resource, sh_resource_instance_origin *origin, const sh_resource_instance_path *path)
{ return ri_assign(object, token, slot, resource, origin, NULL, 0, path); }
int sh_resource_instances_copy(void *object, uint64_t token, uintptr_t slot,
    const void *bytes, size_t length, void *backing, sh_resource_instance_origin *origin)
{
    if (!bytes || !length || !backing || length > UINTPTR_MAX - slot) return 0;
    return ri_assign(object, token, slot, backing, origin, bytes, length, NULL);
}
static ri_field *ri_field_snapshot(const ri_field *field, uintptr_t slot)
{
    ri_field *copy = calloc(1, sizeof(*copy)); if (!copy) return NULL;
    copy->slot = slot; copy->resource = field->resource; copy->origin = ri_retain(field->origin);
    copy->unresolved = field->unresolved;
    if (!ri_path_copy(copy, &field->path)) { ri_field_free(copy); return NULL; }
    if (field->value_size) {
        copy->value = malloc(field->value_size);
        if (!copy->value) { ri_field_free(copy); return NULL; }
        copy->value_size = field->value_size; memcpy(copy->value, field->value, field->value_size);
    }
    return copy;
}
static ri_field *ri_fields_snapshot(const ri_field *fields, int *ok)
{
    ri_field *head = NULL, **tail = &head;
    for (const ri_field *f = fields; f; f = f->next) {
        ri_field *copy = ri_field_snapshot(f, f->slot);
        if (!copy) { *ok = 0; ri_field_free(head); return NULL; }
        *tail = copy; tail = &copy->next;
    }
    return head;
}
static int ri_fields_activate(ri_field *fields)
{
    for (ri_field *f = fields; f; f = f->next) {
        if (!ri_live(f->origin)) return 0;
        f->active = f->origin != NULL;
    }
    return 1;
}
/* A logical path, rather than its old heap address, identifies a field.
 * Unresolved paths retain protection until a later verified reconciliation. */
int sh_resource_instances_reconcile(void *object, uint64_t token,
    sh_resource_instance_resolve resolve, void *context)
{
    ri_field *plan = NULL, *prior = NULL; uint64_t revision = 0; int ok = 1, found = 0;
    if (!object || !token || !resolve) return 0;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token) {
        revision = instance->revision; found = 1; plan = ri_fields_snapshot(instance->fields, &ok);
    }
    ReleaseSRWLockShared(&g_ri_lock);
    if (!found || !ok) { ri_field_free(plan); return 0; }
    ri_field **at = &plan;
    while (*at) {
        ri_field *f = *at;
        if (f->path.count) {
            uintptr_t slot = 0; void *actual = NULL;
            int result = resolve(context, object, &f->path, &slot, &actual);
            if (result == 0 || (result == 1 && !actual)) {
                *at = f->next; f->next = NULL; ri_field_free(f); continue;
            }
            if (result != 1 || !slot) f->unresolved = 1;
            else {
                f->slot = slot; f->unresolved = 0;
                if (actual != f->resource) {
                    sh_resource_instances_origin_release(f->origin); f->origin = NULL;
                    f->resource = actual;
                }
            }
        }
        at = &f->next;
    }
    if (!ri_fields_activate(plan)) { ri_field_free(plan); return 0; }
    ok = 0;
    AcquireSRWLockExclusive(&g_ri_lock);
    instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token && instance->revision == revision && revision != UINT64_MAX) {
        prior = instance->fields; instance->fields = plan; plan = NULL; instance->revision++; ok = 1;
    }
    ReleaseSRWLockExclusive(&g_ri_lock);
    ri_field_free(plan); ri_field_free(prior); return ok;
}
int sh_resource_instances_rebind(void *object,uint64_t token,void *resource,
    sh_resource_instance_origin *origin,int (*match)(void *,uintptr_t,void *,const void *,size_t),
    sh_resource_instance_resolve resolve,void *context)
{
    ri_field *plan=NULL,*prior=NULL; uint64_t revision=0; int ok=1,found=0,complete=1;
    if (!object || !token || !resource || !origin || !match) return 0;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance=g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if(instance && instance->token==token) {
        revision=instance->revision; found=1; plan=ri_fields_snapshot(instance->fields,&ok);
    }
    ReleaseSRWLockShared(&g_ri_lock);
    if(!found || !ok) { ri_field_free(plan); return 0; }
    for(ri_field *f=plan;f;f=f->next) if(!f->value_size && f->resource==resource) {
        uintptr_t slot=f->slot; void *actual=NULL;
        if(f->path.count) {
            if(!resolve || resolve(context,object,&f->path,&slot,&actual)!=1 || actual!=resource) {
                f->unresolved=1; complete=0; continue;
            }
        } else if(!match(context,slot,resource,NULL,0)) { f->unresolved=1; complete=0; continue; }
        sh_resource_instances_origin_release(f->origin); f->origin=ri_retain(origin); f->slot=slot; f->unresolved=0;
    }
    if(!ri_fields_activate(plan)) { ri_field_free(plan); return 0; }
    ok=0;
    AcquireSRWLockExclusive(&g_ri_lock);
    instance=g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if(instance && instance->token==token && instance->revision==revision && revision!=UINT64_MAX) {
        prior=instance->fields; instance->fields=plan; plan=NULL; instance->revision++; ok=1;
    }
    ReleaseSRWLockExclusive(&g_ri_lock); ri_field_free(plan); ri_field_free(prior); return ok && complete;
}
uint64_t sh_resource_instances_clone_paths_checked(void *destination, const void *source, size_t size,
    int (*match)(void *, uintptr_t, void *, const void *, size_t),
    sh_resource_instance_resolve resolve, void *context)
{
    ri_instance *fresh = NULL, *prior = NULL; ri_field *copied = NULL, *sources = NULL, *destinations = NULL;
    uint64_t source_token = 0, source_revision = 0, destination_token = 0, destination_revision = 0, token = 0;
    int ok = 1;
    if (!destination || !source || destination == source || !size ||
        size > UINTPTR_MAX - (uintptr_t)source || size > UINTPTR_MAX - (uintptr_t)destination ||
        !(fresh = calloc(1, sizeof(*fresh)))) return 0;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *src = g_ri_instance_buckets ? *ri_instance_slot(source) : NULL;
    if (src && !src->protection && !src->faulted) {
        source_token = src->token; source_revision = src->revision; fresh->incomplete = src->incomplete;
        sources = ri_fields_snapshot(src->fields, &ok);
        ri_instance *dst = match ? *ri_instance_slot(destination) : NULL;
        if (dst) {
            if(dst->faulted) ok=0;
            destination_token = dst->token; destination_revision = dst->revision;
            destinations = ri_fields_snapshot(dst->fields, &ok);
        }
    }
    ReleaseSRWLockShared(&g_ri_lock);
    if (!source_token || !ok) goto done;
    /* Actual destination writes override only this same typed logical field,
     * even when another field contains an equal native resource pointer. */
    for (ri_field *f = destinations; f; f = f->next) {
        uintptr_t slot = f->slot; void *actual = NULL;
        if (f->path.count && (!resolve || resolve(context, destination, &f->path, &slot, &actual) != 1 ||
            actual != f->resource)) continue;
        if (!match(context, slot, f->resource, f->value, f->value_size)) continue;
        ri_field *copy = ri_field_snapshot(f, slot);
        if (!copy) goto done;
        copy->unresolved = 0; copy->next = copied; copied = copy;
    }
    for (ri_field *f = sources; f; f = f->next) {
        uintptr_t slot; void *actual = NULL;
        if (f->path.count) {
            ri_field *override = copied;
            while (override && !ri_path_equal(&override->path, &f->path)) override = override->next;
            if (override) continue;
            if (!resolve || resolve(context, destination, &f->path, &slot, &actual) != 1 || actual != f->resource) {
                fresh->incomplete = 1; continue;
            }
        } else {
            size_t span = f->value_size ? f->value_size : sizeof(void *);
            if (f->slot < (uintptr_t)source || size < span || f->slot - (uintptr_t)source > size - span) {
                fresh->incomplete = 1; continue;
            }
            slot = (uintptr_t)destination + f->slot - (uintptr_t)source;
        }
        ri_field *existing = copied;
        while (existing && !(f->path.count ? ri_path_equal(&existing->path, &f->path) :
            !existing->path.count && existing->slot == slot)) existing = existing->next;
        if (existing) continue;
        if (match && !match(context, slot, f->resource, f->value, f->value_size)) {
            fresh->incomplete = 1; continue;
        }
        ri_field *copy = ri_field_snapshot(f, slot);
        if (!copy) goto done;
        copy->unresolved = 0; copy->next = copied; copied = copy;
    }
    if (!ri_fields_activate(copied)) goto done;
    AcquireSRWLockExclusive(&g_ri_lock);
    src = *ri_instance_slot(source);
    ri_instance *dst = *ri_instance_slot(destination);
    int destination_same = !match || (dst ? dst->token == destination_token &&
        dst->revision == destination_revision : !destination_token);
    if (src && src->token == source_token && src->revision == source_revision && destination_same &&
        ri_instances_grow() && (token = ri_next()) != 0) {
        ri_instance **slot = ri_instance_slot(destination);
        fresh->object = destination; fresh->token = token; fresh->fields = copied; copied = NULL;
        prior = *slot; fresh->next = prior ? prior->next : NULL; *slot = fresh; fresh = NULL;
        if (!prior) g_ri_instance_count++;
    }
    ReleaseSRWLockExclusive(&g_ri_lock);
done:
    ri_field_free(sources); ri_field_free(destinations);
    ri_field_free(copied); ri_instance_free(fresh); ri_instance_free(prior); return token;
}
uint64_t sh_resource_instances_clone_checked(void *destination, const void *source, size_t size,
    int (*match)(void *, uintptr_t, void *, const void *, size_t), void *context)
{ return sh_resource_instances_clone_paths_checked(destination, source, size, match, NULL, context); }
uint64_t sh_resource_instances_clone(void *destination, const void *source, size_t size)
{ return sh_resource_instances_clone_checked(destination, source, size, NULL, NULL); }
int sh_resource_instances_relocate(void *object, uint64_t token, uintptr_t old_start,
    size_t old_size, uintptr_t new_start, size_t new_size)
{
    ri_field *removed = NULL; int ok = 0;
    if (!object || !token || !old_start || old_size > UINTPTR_MAX - old_start ||
        (new_size && (!new_start || new_size > UINTPTR_MAX - new_start))) return 0;
    AcquireSRWLockExclusive(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token && instance->revision != UINT64_MAX) {
        ri_field **slot = &instance->fields;
        while (*slot) {
            ri_field *field = *slot;
            if (field->slot >= old_start && field->slot - old_start < old_size) {
                size_t offset = field->slot - old_start;
                size_t span = field->value_size ? field->value_size : sizeof(void *);
                if (new_size < span || offset > new_size - span) {
                    *slot = field->next; field->next = removed; removed = field; continue; }
                field->slot = new_start + offset;
            }
            slot = &field->next;
        }
        instance->revision++; ok = 1;
    }
    ReleaseSRWLockExclusive(&g_ri_lock); ri_field_free(removed); return ok;
}
void sh_resource_instances_destroyed(void *object, uint64_t token, int completed)
{
    ri_instance *removed = NULL;
    if (!completed || !object || !token) return;
    AcquireSRWLockExclusive(&g_ri_lock);
    if (!g_ri_instance_buckets) { ReleaseSRWLockExclusive(&g_ri_lock); return; }
    ri_instance **slot = ri_instance_slot(object);
    if (*slot && (*slot)->token == token) { removed = *slot; *slot = removed->next; g_ri_instance_count--; }
    ReleaseSRWLockExclusive(&g_ri_lock); ri_instance_free(removed);
}
int sh_resource_instances_complete(const void *object, uint64_t token)
{
    int complete = 0;
    AcquireSRWLockShared(&g_ri_lock);
    ri_instance *instance = g_ri_instance_buckets ? *ri_instance_slot(object) : NULL;
    if (instance && instance->token == token && !instance->incomplete && !instance->protection && !instance->faulted) {
        complete = 1;
        for (ri_field *f = instance->fields; f; f = f->next) if (!f->origin || f->unresolved) { complete = 0; break; }
    }
    ReleaseSRWLockShared(&g_ri_lock); return complete;
}
void sh_resource_instances_counts(size_t *resources, size_t *instances, size_t *fields)
{
    size_t r = 0, i = 0, f = 0;
    AcquireSRWLockShared(&g_ri_lock);
    r = g_ri_resource_count; i = g_ri_instance_count;
    for (size_t at = 0; at < g_ri_instance_buckets; at++) for (ri_instance *v = g_ri_instances[at]; v; v = v->next)
        for (ri_field *p = v->fields; p; p = p->next) f++;
    ReleaseSRWLockShared(&g_ri_lock);
    if (resources) *resources = r; if (instances) *instances = i; if (fields) *fields = f;
}
#ifdef SH_RESOURCE_INSTANCES_TESTING
void sh_resource_instances_test_reset(void)
{
    for(;;) {
        AcquireSRWLockShared(&g_ri_lock); uint64_t active=g_ri_active?g_ri_active->active_context:0; ReleaseSRWLockShared(&g_ri_lock);
        if(!active) break; sh_resource_instances_fault_scope_retire(active);
    }
    ri_resource **resources; ri_instance **instances; size_t rb, ib;
    sh_resource_instance_origin *untracked;
    AcquireSRWLockExclusive(&g_ri_lock);
    resources = g_ri_resources; instances = g_ri_instances; rb = g_ri_resource_buckets; ib = g_ri_instance_buckets;
    g_ri_resources = NULL; g_ri_instances = NULL;
    untracked=g_ri_untracked; g_ri_untracked=NULL;
    g_ri_resource_buckets = g_ri_instance_buckets = g_ri_resource_count = g_ri_instance_count = 0;
    ReleaseSRWLockExclusive(&g_ri_lock);
    for (size_t i = 0; i < rb; i++) while (resources[i]) { ri_resource *r = resources[i]; resources[i] = r->next;
        sh_resource_instances_origin_release(r->origin); free(r); }
    for (size_t i = 0; i < ib; i++) while (instances[i]) { ri_instance *r = instances[i]; instances[i] = r->next;
        ri_instance_free(r); }
    free(resources); free(instances);
    while(untracked) {
        sh_resource_instance_origin *next=untracked->untracked_next;
        untracked->untracked_next=NULL; untracked->untracked_quarantined=0;
        sh_resource_instances_origin_unhold(untracked); untracked=next;
    }
    ri_fault_drain();
}
#endif
