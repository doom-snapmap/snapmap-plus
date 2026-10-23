#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/resource_instances.h"

static int failures, captures, leases, promotion_failure;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)
typedef struct capture { int generation; unsigned char bytes; } capture;
typedef struct lease { const capture *source; } lease;
static void *new_capture(int generation, unsigned char bytes)
{
    capture *value = malloc(sizeof(*value)); CHECK(value);
    if (value) { value->generation = generation; value->bytes = bytes; captures++; }
    return value;
}
static int promote(const void *source, void **out)
{
    CHECK(source && out && !*out);
    if (promotion_failure) return -1;
    lease *held = malloc(sizeof(*held)); if (!held) return -1;
    held->source = source; *out = held; leases++; return 1;
}
static void release_capture(void *source) { CHECK(source && captures > 0); captures--; free(source); }
static void release_lease(void *held) { CHECK(held && leases > 0); leases--; free(held); }
static void count(size_t resources, size_t objects, size_t fields)
{
    size_t r, o, f; sh_resource_instances_counts(&r, &o, &f);
    CHECK(r == resources && o == objects && f == fields);
}
static int matches(void *context, uintptr_t slot, void *resource, const void *bytes, size_t length)
{ (void)context; return length ? !memcmp((const void *)slot, bytes, length) : *(void **)slot == resource; }
typedef struct path_list { void *data; int count, capacity; uintptr_t padding; } path_list;
typedef struct path_row { uintptr_t padding; path_list inner; uintptr_t tail; } path_row;
typedef struct path_root { uintptr_t padding; path_list outer; } path_root;
static uint64_t path_reentrant_token;
static int path_resolve(void *context, const void *object, const sh_resource_instance_path *path,
    uintptr_t *slot, void **resource)
{
    if (context && path_reentrant_token) {
        uint64_t token = path_reentrant_token; path_reentrant_token = 0;
        CHECK(sh_resource_instances_assign((void *)object, token, (uintptr_t)object, NULL, NULL));
    }
    uintptr_t base = (uintptr_t)object; size_t extent = path->root_size;
    for (size_t i = 0; i < path->count; i++) {
        const sh_resource_instance_step *step = &path->steps[i];
        if (extent < sizeof(path_list) || step->offset > extent - sizeof(path_list) ||
            step->offset > UINTPTR_MAX - base || !step->stride ||
            step->type != i + 1) return -1;
        path_list *list = (path_list *)(base + step->offset);
        if (list->count < 0 || (size_t)list->count > SIZE_MAX / step->stride ||
            (list->count && (!list->data || (size_t)list->count * step->stride > UINTPTR_MAX - (uintptr_t)list->data)))
            return -1;
        if (step->index >= (size_t)list->count) return 0;
        base = (uintptr_t)list->data + step->index * step->stride; extent = step->stride;
    }
    if (extent < sizeof(void *) || path->leaf > extent - sizeof(void *) || path->leaf > UINTPTR_MAX - base) return -1;
    *slot = base + path->leaf; *resource = *(void **)*slot; return 1;
}
static void test_native_kind(void)
{
    unsigned char object[8]={0},copy[8]={0},kind1,kind2,owner1,owner2;
    uint64_t first_owner=sh_resource_instances_construct(&owner1,1),second_owner=sh_resource_instances_construct(&owner2,1);
    uint64_t token=sh_resource_instances_construct(object,1);
    CHECK(sh_resource_instances_classify(object,token,&kind1,&owner1));
    CHECK(sh_resource_instances_classify(object,token,&kind1,&owner1));
    CHECK(!sh_resource_instances_classify(object,token,&kind2,&owner1));
    CHECK(!sh_resource_instances_classify(object,token,&kind1,&owner2));
    CHECK(sh_resource_instances_class_owner(object,token,&kind1)==&owner1);
    CHECK(!sh_resource_instances_class_owner(object,token,&kind2));
    uint64_t copied=sh_resource_instances_clone(copy,object,sizeof(object)); CHECK(copied);
    CHECK(!sh_resource_instances_class_owner(copy,copied,&kind1));
    uint64_t replaced_owner=sh_resource_instances_construct(&owner1,1); CHECK(replaced_owner!=first_owner);
    CHECK(!sh_resource_instances_class_owner(object,token,&kind1));
    CHECK(!sh_resource_instances_classify(object,token,&kind1,&owner1));
    uint64_t next=sh_resource_instances_construct(object,1); CHECK(next!=token);
    CHECK(!sh_resource_instances_class_owner(object,token,&kind1));
    CHECK(!sh_resource_instances_class_owner(object,next,&kind1));
    CHECK(!sh_resource_instances_classify(object,token,&kind1,&owner1));
    sh_resource_instances_destroyed(object,next,1); sh_resource_instances_destroyed(copy,copied,1);
    sh_resource_instances_destroyed(&owner1,replaced_owner,1); sh_resource_instances_destroyed(&owner2,second_owner,1);
}
static int fault_visits,origin_visits;
static int fault_visit(void *context,void *object,uint64_t token)
{ (void)context; CHECK(sh_resource_instances_token(object)==token && sh_resource_instances_faulted(object,token)); fault_visits++; return 1; }
static int origin_visit(void *context,const void *capture)
{ (void)context; CHECK(capture); origin_visits++; return 1; }
static void test_observation_faults(void)
{
    unsigned char one[8]={0},two[8]={0},three[8]={0},copy[8]={0},resource[8]={0};
    uint64_t t1=sh_resource_instances_construct(one,1),t2=sh_resource_instances_construct(two,1),t3=sh_resource_instances_construct(three,1);
    sh_resource_instance_origin *a=sh_resource_instances_origin(new_capture(21,'a'),0);
    sh_resource_instance_origin *b=sh_resource_instances_origin(new_capture(22,'b'),0);
    sh_resource_instance_origin *stock=sh_resource_instances_origin(NULL,1);
    CHECK(sh_resource_instances_origin_hold(a) && sh_resource_instances_origin_hold(b)); CHECK(leases==2);
    promotion_failure=1; /* existing live shares make fault recording allocation-free */
    CHECK(sh_resource_instances_fault(one,t1,a)); CHECK(sh_resource_instances_fault(one,t1,b));
    CHECK(sh_resource_instances_fault(one,t1,a)); CHECK(sh_resource_instances_fault(two,t2,a));
    CHECK(sh_resource_instances_fault(three,t3,stock)); CHECK(!sh_resource_instances_faulted(three,t3));
    CHECK(!sh_resource_instances_fault(one,t1+100,a));
    sh_resource_instances_origin_unhold(a); sh_resource_instances_origin_unhold(b); promotion_failure=0;
    CHECK(leases==2 && !sh_resource_instances_complete(one,t1));
    CHECK(sh_resource_instances_assign(one,t1,(uintptr_t)one,resource,stock));
    CHECK(!sh_resource_instances_complete(one,t1)); /* recorded fields alone cannot certify a full reset */
    fault_visits=0; CHECK(sh_resource_instances_faults(fault_visit,NULL) && fault_visits==2);
    CHECK(!sh_resource_instances_clone(copy,one,sizeof(one)));
    CHECK(sh_resource_instances_fault_recovered(one,t1)); CHECK(leases==2 && sh_resource_instances_complete(one,t1));
    sh_resource_instances_destroyed(two,t2,0); CHECK(leases==2);
    sh_resource_instances_destroyed(two,t2,1); CHECK(!leases);
    /* Independent fault groups do not retain an already repaired group's
     * exact roots merely because another unrelated holder remains faulted. */
    t2=sh_resource_instances_construct(two,1);
    CHECK(sh_resource_instances_origin_hold(a) && sh_resource_instances_origin_hold(b));
    CHECK(sh_resource_instances_fault(one,t1,a) && sh_resource_instances_fault(two,t2,b));
    sh_resource_instances_origin_unhold(a); sh_resource_instances_origin_unhold(b); CHECK(leases==2);
    CHECK(sh_resource_instances_fault_recovered(one,t1)); CHECK(leases==1);
    sh_resource_instances_destroyed(two,t2,1); CHECK(!leases);
    CHECK(sh_resource_instances_origin_hold(a)); CHECK(sh_resource_instances_fault(one,t1,a)); sh_resource_instances_origin_unhold(a);
    uint64_t replacement=sh_resource_instances_construct(one,1); CHECK(replacement!=t1 && !leases);
    CHECK(!sh_resource_instances_fault_recovered(one,t1) && !sh_resource_instances_faulted(one,replacement));
    sh_resource_instance_origin *children[2]={a,b},*composite=sh_resource_instances_origin_composite(children,2);
    origin_visits=0; CHECK(sh_resource_instances_origin_captures(stock,origin_visit,NULL) && !origin_visits);
    CHECK(sh_resource_instances_origin_captures(composite,origin_visit,NULL) && origin_visits==2);
    CHECK(sh_resource_instances_origin_hold(composite)); CHECK(sh_resource_instances_fault(three,t3,composite));
    sh_resource_instances_origin_unhold(composite); CHECK(leases==2);
    sh_resource_instances_destroyed(three,t3,1); CHECK(!leases);
    sh_resource_instances_destroyed(one,replacement,1);
    CHECK(sh_resource_instances_origin_hold(a));
    CHECK(!sh_resource_instances_fault(NULL,0,a)); sh_resource_instances_origin_unhold(a); CHECK(leases==1);
    CHECK(sh_resource_instances_origin_hold(a));
    CHECK(!sh_resource_instances_fault(NULL,0,a)); sh_resource_instances_origin_unhold(a); CHECK(leases==1);
    sh_resource_instances_origin_release(composite); sh_resource_instances_origin_release(a);
    sh_resource_instances_origin_release(b); sh_resource_instances_origin_release(stock);
    CHECK(captures==1 && leases==1); count(0,0,0);
    sh_resource_instances_test_reset(); CHECK(!captures && !leases);
}
static void test_fault_construction_scopes(void)
{
    unsigned char model[8]={0},light[8]={0},committed[8]={0};
    sh_resource_instance_origin *a=sh_resource_instances_origin(new_capture(25,'a'),0);
    sh_resource_instance_origin *b=sh_resource_instances_origin(new_capture(26,'b'),0);
    uint64_t m=sh_resource_instances_construct(model,1),l=sh_resource_instances_construct(light,1),c=sh_resource_instances_construct(committed,1);
    CHECK(sh_resource_instances_origin_hold(a) && sh_resource_instances_fault(model,m,a)); sh_resource_instances_origin_unhold(a);
    CHECK(sh_resource_instances_fault_scope_begin(model,m,11) && sh_resource_instances_fault_scope_begin(model,m,11));
    CHECK(!sh_resource_instances_fault_scope_begin(model,m,12) && sh_resource_instances_fault_group_active(a));
    CHECK(sh_resource_instances_fault_scope_adopt(12,light,l) && !sh_resource_instances_faulted(light,l));
    sh_resource_instances_destroyed(model,m,1); CHECK(!sh_resource_instances_token(model) && leases==1);
    uint64_t replacement=sh_resource_instances_construct(model,1); CHECK(replacement!=m);
    CHECK(sh_resource_instances_fault_scope_adopt(11,light,l) && sh_resource_instances_faulted(light,l));
    sh_resource_instances_fault_scope_end(model,m,11); CHECK(sh_resource_instances_fault_group_active(a));
    sh_resource_instances_destroyed(light,l,1); CHECK(leases==1);
    CHECK(sh_resource_instances_fault_scope_adopt(11,committed,c));
    sh_resource_instances_fault_scope_end(model,m,11);
    CHECK(!sh_resource_instances_fault_group_active(a) && !sh_resource_instances_faulted(model,replacement) && leases==1);
    sh_resource_instances_destroyed(committed,c,1); CHECK(!leases);
    /* A completed source reset cannot drop input still used by the active
     * old native call. New fault inputs merge into that same active group. */
    CHECK(sh_resource_instances_origin_hold(a) && sh_resource_instances_fault(model,replacement,a)); sh_resource_instances_origin_unhold(a);
    CHECK(sh_resource_instances_fault_scope_begin(model,replacement,13));
    CHECK(sh_resource_instances_fault_recovered(model,replacement) && leases==1);
    CHECK(sh_resource_instances_origin_hold(b) && sh_resource_instances_fault(model,replacement,b)); sh_resource_instances_origin_unhold(b);
    l=sh_resource_instances_construct(light,1); CHECK(sh_resource_instances_fault_scope_adopt(13,light,l));
    sh_resource_instances_fault_scope_retire(12); CHECK(leases==2 && sh_resource_instances_fault_group_active(b));
    sh_resource_instances_fault_scope_retire(13); CHECK(!sh_resource_instances_fault_group_active(a) && leases==2);
    sh_resource_instances_destroyed(model,replacement,1); CHECK(leases==2);
    sh_resource_instances_destroyed(light,l,1); CHECK(!leases);
    sh_resource_instances_origin_release(a); sh_resource_instances_origin_release(b); CHECK(!captures); count(0,0,0);
}
static void test_logical_paths(void)
{
    path_root root = {0}, copy = {0}; unsigned char resource[8];
    path_row *rows = calloc(1,sizeof(*rows)); void **leaves = calloc(4,sizeof(void *));
    CHECK(rows && leaves); root.outer = (path_list){rows,1,1,0}; rows->inner = (path_list){leaves,2,2,0};
    leaves[2] = leaves[3] = resource;
    sh_resource_instance_origin *a = sh_resource_instances_origin(new_capture(11,'a'),0);
    sh_resource_instance_origin *b = sh_resource_instances_origin(new_capture(12,'b'),0);
    uint64_t token = sh_resource_instances_construct(&root,1);
    sh_resource_instance_step steps[2] = {
        {offsetof(path_root,outer),0,sizeof(path_row),1,3,4},
        {offsetof(path_row,inner),1,2*sizeof(void *),2,5,6}};
    sh_resource_instance_path path = {7,8,sizeof(root),2,0,steps};
    CHECK(sh_resource_instances_assign_path(&root,token,(uintptr_t)(leaves+2),resource,a,&path));
    path.leaf = sizeof(void *);
    CHECK(sh_resource_instances_assign_path(&root,token,(uintptr_t)(leaves+3),resource,b,&path));
    CHECK(leases == 2 && sh_resource_instances_complete(&root,token));
    /* Both containing storage and nested data move through arbitrary native
     * callbacks. Equal pointers in two fields keep their distinct origins. */
    path_row *moved = malloc(sizeof(*moved)); memcpy(moved,rows,sizeof(*moved)); free(rows); rows = moved;
    void **next = malloc(4*sizeof(void *)); memcpy(next,leaves,4*sizeof(void *)); free(leaves); leaves = next;
    rows->inner.data = leaves; root.outer.data = rows;
    CHECK(sh_resource_instances_reconcile(&root,token,path_resolve,NULL));
    CHECK(leases == 2 && sh_resource_instances_complete(&root,token));
    root.outer.count = -1;
    CHECK(sh_resource_instances_reconcile(&root,token,path_resolve,NULL));
    CHECK(leases == 2 && !sh_resource_instances_complete(&root,token));
    root.outer.count = 1;
    CHECK(sh_resource_instances_reconcile(&root,token,path_resolve,NULL) && sh_resource_instances_complete(&root,token));
    path_reentrant_token = token;
    CHECK(!sh_resource_instances_reconcile(&root,token,path_resolve,&root));
    CHECK(leases == 2 && sh_resource_instances_complete(&root,token));
    size_t saved_stride = steps[1].stride; steps[1].stride = SIZE_MAX;
    CHECK(!sh_resource_instances_assign_path(&root,token,(uintptr_t)(leaves+3),resource,b,&path));
    steps[1].stride = saved_stride;
    /* A destination override of the same pointer still owns its own observed
     * generation. A deep clone resolves the typed path in destination data. */
    path_row *copy_rows = malloc(sizeof(*rows)); memcpy(copy_rows,rows,sizeof(*rows));
    void **copy_leaves = malloc(4*sizeof(void *)); memcpy(copy_leaves,leaves,4*sizeof(void *));
    copy_rows->inner.data = copy_leaves; copy.outer = root.outer; copy.outer.data = copy_rows;
    uint64_t destination = sh_resource_instances_construct(&copy,1);
    CHECK(sh_resource_instances_assign_path(&copy,destination,(uintptr_t)(copy_leaves+3),resource,a,&path));
    uint64_t cloned = sh_resource_instances_clone_paths_checked(&copy,&root,sizeof(root),matches,path_resolve,NULL);
    CHECK(cloned > destination && sh_resource_instances_complete(&copy,cloned) && leases == 2);
    sh_resource_instances_destroyed(&root,token,1); CHECK(leases == 1); /* destination override is A in both leaves */
    copy_leaves[3] = NULL;
    CHECK(sh_resource_instances_assign_path(&copy,cloned,(uintptr_t)(copy_leaves+3),NULL,NULL,&path));
    CHECK(leases == 1);
    copy_rows->inner.count = 1;
    CHECK(sh_resource_instances_reconcile(&copy,cloned,path_resolve,NULL));
    CHECK(leases == 0 && sh_resource_instances_complete(&copy,cloned));
    sh_resource_instances_destroyed(&copy,cloned,1);
    free(copy_leaves); free(copy_rows); free(leaves); free(rows);
    sh_resource_instances_origin_release(a); sh_resource_instances_origin_release(b);
    count(0,0,0); CHECK(captures == 0);
}
static int composite_visits;
static int composite_visit(void *context,void *object,uint64_t token)
{
    CHECK(object==context && sh_resource_instances_token(object)==token); composite_visits++; return 1;
}
static int payload_contains(void *context,void *resource) { return context==resource; }
static int payload_visited(void *context,void *object,uint64_t token)
{ (void)context; CHECK(object && sh_resource_instances_token(object)==token); composite_visits++; return 1; }
static void test_composite_ownership(void)
{
    unsigned char object[64]={0},alias[64]={0},resource[8];
    sh_resource_instance_origin *a=sh_resource_instances_origin(new_capture(41,'a'),0);
    sh_resource_instance_origin *b=sh_resource_instances_origin(new_capture(42,'b'),0);
    unsigned char other_input[8]={0};
    uint64_t input_token=sh_resource_instances_construct(resource,1);
    uint64_t other_token=sh_resource_instances_construct(other_input,1);
    CHECK(sh_resource_instances_origin_source(a,resource,input_token));
    CHECK(sh_resource_instances_origin_source(b,other_input,other_token));
    sh_resource_instance_origin *children[]={a,b,a};
    sh_resource_instance_origin *composite=sh_resource_instances_origin_composite(children,3);
    CHECK(composite && !sh_resource_instances_origin_matches(composite,NULL,1));
    CHECK(sh_resource_instances_publish(resource,composite) && leases==0);
    uint64_t token=sh_resource_instances_construct(object,1),copy=sh_resource_instances_construct(alias,1);
    CHECK(sh_resource_instances_payload(object,token,composite) && leases==2);
    CHECK(!sh_resource_instances_origin_source(composite,resource,input_token));
    CHECK(sh_resource_instances_payload_uses(object,token,payload_contains,resource));
    CHECK(sh_resource_instances_payload_uses(object,token,payload_contains,other_input));
    composite_visits=0;
    CHECK(sh_resource_instances_holders_where(payload_contains,payload_visited,resource) && composite_visits==1);
    sh_resource_instances_destroyed(resource,input_token,1);
    uint64_t reused=sh_resource_instances_construct(resource,1); CHECK(reused && reused!=input_token);
    CHECK(!sh_resource_instances_payload_uses(object,token,payload_contains,resource));
    CHECK(sh_resource_instances_payload_uses(object,token,payload_contains,other_input));
    sh_resource_instances_destroyed(resource,reused,1); sh_resource_instances_destroyed(other_input,other_token,1);
    CHECK(sh_resource_instances_assign(alias,copy,(uintptr_t)alias+8,resource,composite));
    CHECK(sh_resource_instances_payload(object,token,NULL) && leases==2);
    sh_resource_instances_destroyed(alias,copy,1); CHECK(leases==0);
    CHECK(sh_resource_instances_protect(object,token,a));
    for(int i=0;i<16;i++) CHECK(sh_resource_instances_protect(object,token,a));
    CHECK(leases==1 && !sh_resource_instances_complete(object,token));
    CHECK(!sh_resource_instances_clone(alias,object,sizeof(object)));
    sh_resource_instances_unprotect(object,token);
    CHECK(leases==0 && sh_resource_instances_complete(object,token));
    memcpy(object+8,&(void *){resource},8); memset(object+24,17,8);
    CHECK(sh_resource_instances_assign(object,token,(uintptr_t)object+8,resource,a));
    CHECK(sh_resource_instances_copy(object,token,(uintptr_t)object+24,object+24,8,resource,a));
    composite_visits=0;
    CHECK(sh_resource_instances_holders(resource,composite_visit,object) && composite_visits==1);
    CHECK(sh_resource_instances_rebind(object,token,resource,b,matches,NULL,NULL) && leases==2);
    sh_resource_instance_origin *read=sh_resource_instances_field_origin(object,token,(uintptr_t)object+8);
    CHECK(read==b); sh_resource_instances_origin_release(read);
    read=sh_resource_instances_field_origin(object,token,(uintptr_t)object+24);
    CHECK(read==a); sh_resource_instances_origin_release(read);
    *(void **)(object+8)=NULL;
    CHECK(!sh_resource_instances_rebind(object,token,resource,a,matches,NULL,NULL));
    CHECK(leases==2 && !sh_resource_instances_complete(object,token));
    *(void **)(object+8)=resource;
    CHECK(sh_resource_instances_rebind(object,token,resource,a,matches,NULL,NULL) && leases==1 &&
        sh_resource_instances_complete(object,token));
    sh_resource_instances_destroyed(object,token,1); CHECK(leases==0);
    sh_resource_instances_forget(resource);
    sh_resource_instances_origin_release(composite); sh_resource_instances_origin_release(a); sh_resource_instances_origin_release(b);
    count(0,0,0); CHECK(!captures);
}
static void test_payload_live_share(void)
{
    unsigned char object[16]={0};
    sh_resource_instance_origin *old=sh_resource_instances_origin(new_capture(511,0x51),0);
    sh_resource_instance_origin *next=sh_resource_instances_origin(new_capture(512,0x52),0);
    uint64_t token=sh_resource_instances_construct(object,1); CHECK(token);
    CHECK(!sh_resource_instances_payload_hold(object,token));
    CHECK(sh_resource_instances_publish(object,old));
    CHECK(!sh_resource_instances_payload_hold(object,token)); /* Weak mapping owns no payload. */
    CHECK(sh_resource_instances_payload(object,token,old)); CHECK(leases==1);
    promotion_failure=1;
    sh_resource_instance_origin *running=sh_resource_instances_payload_hold(object,token);
    CHECK(running==old && leases==1); promotion_failure=0;
    CHECK(!sh_resource_instances_payload_hold(object,token+1));
    CHECK(sh_resource_instances_payload(object,token,next)); CHECK(leases==2);
    sh_resource_instance_origin *current=sh_resource_instances_payload_hold(object,token); CHECK(current==next);
    CHECK(sh_resource_instances_payload(object,token,NULL)); CHECK(leases==2);
    CHECK(!sh_resource_instances_payload_hold(object,token));
    sh_resource_instances_origin_unhold(current); CHECK(leases==1);
    uint64_t replacement=sh_resource_instances_construct(object,1); CHECK(replacement>token);
    CHECK(!sh_resource_instances_payload_hold(object,token)); CHECK(!sh_resource_instances_payload_hold(object,replacement));
    sh_resource_instances_destroyed(object,replacement,1); CHECK(leases==1);
    sh_resource_instances_origin_unhold(running); CHECK(!leases);
    CHECK(!sh_resource_instances_payload_hold(object,token));
    sh_resource_instances_forget(object); sh_resource_instances_origin_release(old); sh_resource_instances_origin_release(next);
    CHECK(!captures);
}
static void test_copied_field_live_share(void)
{
    unsigned char object[32]={0},old_bytes[16],new_bytes[16];
    void *resource=(void *)(uintptr_t)0xabc0;
    memset(old_bytes,0x31,sizeof(old_bytes)); memset(new_bytes,0x72,sizeof(new_bytes));
    sh_resource_instance_origin *old=sh_resource_instances_origin(new_capture(501,0x31),0);
    sh_resource_instance_origin *next=sh_resource_instances_origin(new_capture(502,0x72),0);
    uint64_t token=sh_resource_instances_construct(object,1); CHECK(token);
    CHECK(sh_resource_instances_copy(object,token,(uintptr_t)object+8,old_bytes,16,resource,old)); CHECK(leases==1);
    CHECK(!sh_resource_instances_copy_hold(object,token,(uintptr_t)object+8,new_bytes,16));
    CHECK(!sh_resource_instances_copy_hold(object,token,(uintptr_t)object+8,old_bytes,15));
    CHECK(!sh_resource_instances_copy_hold(object,token+1,(uintptr_t)object+8,old_bytes,16));
    promotion_failure=1; /* Existing field serving share must need no callback. */
    sh_resource_instance_origin *running=sh_resource_instances_copy_hold(object,token,(uintptr_t)object+8,old_bytes,16);
    CHECK(running==old && leases==1); promotion_failure=0;
    CHECK(sh_resource_instances_copy(object,token,(uintptr_t)object+8,new_bytes,16,resource,next)); CHECK(leases==2);
    CHECK(!sh_resource_instances_copy_hold(object,token,(uintptr_t)object+8,old_bytes,16));
    sh_resource_instances_origin_unhold(running); CHECK(leases==1);
    running=sh_resource_instances_copy_hold(object,token,(uintptr_t)object+8,new_bytes,16); CHECK(running==next);
    sh_resource_instances_destroyed(object,token,0); CHECK(leases==1);
    sh_resource_instance_origin *also=sh_resource_instances_copy_hold(object,token,(uintptr_t)object+8,new_bytes,16);
    CHECK(also==next); sh_resource_instances_origin_unhold(also);
    sh_resource_instances_destroyed(object,token,1); CHECK(leases==1); /* Active worker owns an independent share. */
    CHECK(!sh_resource_instances_copy_hold(object,token,(uintptr_t)object+8,new_bytes,16));
    uint64_t replacement=sh_resource_instances_construct(object,1); CHECK(replacement>token);
    CHECK(!sh_resource_instances_copy_hold(object,token,(uintptr_t)object+8,new_bytes,16));
    sh_resource_instances_origin_unhold(running); CHECK(!leases);
    CHECK(!sh_resource_instances_copy_hold(object,replacement,(uintptr_t)object+8,new_bytes,16)); /* no resurrection */
    CHECK(sh_resource_instances_assign(object,replacement,(uintptr_t)object+8,resource,next));
    CHECK(!sh_resource_instances_copy_hold(object,replacement,(uintptr_t)object+8,new_bytes,16)); /* pointer is not copied bytes */
    sh_resource_instances_destroyed(object,replacement,1); CHECK(!leases);
    sh_resource_instances_origin_release(old); sh_resource_instances_origin_release(next); CHECK(!captures);
}
int main(void)
{
    unsigned char object[64], copy[64], resource_a[8], resource_b[8];
    sh_resource_instance_api api = {promote, release_capture, release_lease};
    CHECK(sh_resource_instances_bind(api)); CHECK(!sh_resource_instances_bind(api));
    CHECK(!sh_resource_instances_origin(NULL, 0));
    capture *captured_a = new_capture(1, 'A');
    sh_resource_instance_origin *a = sh_resource_instances_origin(captured_a, 0);
    sh_resource_instance_origin *b = sh_resource_instances_origin(new_capture(2, 'B'), 0);
    sh_resource_instance_origin *stock = sh_resource_instances_origin(NULL, 1);
    CHECK(a && b && stock && captures == 2 && leases == 0);
    CHECK(sh_resource_instances_publish(resource_a, a));
    CHECK(sh_resource_instances_publish(resource_b, stock));
    sh_resource_instance_origin *read_a = sh_resource_instances_lookup(resource_a);
    CHECK(read_a == a && !sh_resource_instances_lookup((void *)1));
    uint64_t original = sh_resource_instances_construct(object, 1); CHECK(original);
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 8), resource_a, read_a));
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 16), resource_b, stock));
    CHECK(sh_resource_instances_complete(object, original) && leases == 1);
    count(2, 1, 2);
    /* A successful source reread changes the weak mapping, not an already
     * copied field's origin. Its held snapshot still has the original bytes. */
    CHECK(sh_resource_instances_publish(resource_a, b));
    CHECK(captured_a->generation == 1 && captured_a->bytes == 'A');
    CHECK(leases == 1 && captures == 2);
    uint64_t cloned = sh_resource_instances_clone(copy, object, sizeof(object));
    CHECK(cloned && cloned != original && leases == 1 && sh_resource_instances_complete(copy, cloned));
    count(2, 2, 4);
    sh_resource_instances_origin_release(read_a);
    /* Actual overwrite, including a reader that reports failure after its
     * store: replace only this field. A skipped reader makes no call. */
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 8), resource_a, b));
    CHECK(leases == 2); count(2, 2, 4);
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 8), NULL, NULL));
    CHECK(leases == 1); count(2, 2, 3);
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 24), resource_a, NULL));
    CHECK(!sh_resource_instances_complete(object, original));
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 24), NULL, NULL));
    CHECK(sh_resource_instances_complete(object, original));
    /* Native container relocation carries surviving slots and releases the
     * removed tail. It neither drops another member nor clones every read. */
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 32), resource_a, b));
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 40), resource_a, b));
    CHECK(leases == 2);
    CHECK(sh_resource_instances_relocate(object, original, (uintptr_t)(object + 32), 16,
        (uintptr_t)(object + 48), 8));
    CHECK(leases == 2); count(2, 2, 4);
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 48), NULL, NULL));
    CHECK(leases == 1); count(2, 2, 3);
    sh_resource_instances_destroyed(copy, cloned, 0); CHECK(leases == 1);
    sh_resource_instances_destroyed(copy, cloned, 1); CHECK(leases == 0);
    /* Virtual copies may omit a source member and replace another using an
     * observed destination read. Preserve that destination override, copy
     * only pointers actually present, and never certify an omitted field. */
    memset(object, 0, sizeof(object)); memset(copy, 0, sizeof(copy));
    original = sh_resource_instances_construct(object, 1);
    for (size_t offset = 8; offset <= 24; offset += 8) {
        memcpy(object + offset, &(void *){resource_a}, sizeof(void *));
        CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + offset), resource_a, a));
    }
    memcpy(copy + 8, &(void *){resource_a}, sizeof(void *));
    memcpy(copy + 16, &(void *){resource_b}, sizeof(void *));
    uint64_t override = sh_resource_instances_construct(copy, 1);
    CHECK(sh_resource_instances_assign(copy, override, (uintptr_t)(copy + 16), resource_b, b));
    cloned = sh_resource_instances_clone_checked(copy, object, sizeof(object), matches, NULL);
    CHECK(cloned > override && !sh_resource_instances_complete(copy, cloned) && leases == 2);
    count(2, 2, 5);
    sh_resource_instances_destroyed(object, original, 1); CHECK(leases == 2);
    CHECK(sh_resource_instances_assign(copy, cloned, (uintptr_t)(copy + 8), NULL, NULL)); CHECK(leases == 1);
    sh_resource_instances_destroyed(copy, cloned, 1); CHECK(leases == 0);
    /* A transient native read shares the same lease as fields. Weak source
     * associations keep only metadata once all active uses have ended. */
    CHECK(sh_resource_instances_origin_hold(a) && sh_resource_instances_origin_hold(a) && leases == 1);
    sh_resource_instances_origin_unhold(a); CHECK(leases == 1);
    sh_resource_instances_origin_unhold(a); CHECK(leases == 0);
    /* Copied values keep their entry generation independently of a backing
     * pointer that is reread later. Checked cloning validates actual bytes. */
    memset(object, 0, sizeof(object)); memset(copy, 0, sizeof(copy));
    memset(object + 32, 0x17, 16); memcpy(object + 8, &(void *){resource_a}, sizeof(void *));
    original = sh_resource_instances_construct(object, 1);
    CHECK(sh_resource_instances_copy(object, original, (uintptr_t)(object + 32), object + 32, 16, resource_a, a));
    CHECK(sh_resource_instances_assign(object, original, (uintptr_t)(object + 8), resource_a, b));
    CHECK(leases == 2 && sh_resource_instances_complete(object, original));
    memcpy(copy, object, sizeof(copy));
    cloned = sh_resource_instances_clone_checked(copy, object, sizeof(object), matches, NULL);
    CHECK(cloned && leases == 2 && sh_resource_instances_complete(copy, cloned));
    sh_resource_instances_destroyed(object, original, 1); CHECK(leases == 2);
    memset(copy + 32, 0x29, 16);
    CHECK(sh_resource_instances_copy(copy, cloned, (uintptr_t)(copy + 32), copy + 32, 16, resource_a, b));
    CHECK(leases == 1);
    CHECK(sh_resource_instances_copy(copy, cloned, (uintptr_t)(copy + 32), copy + 32, 16, resource_a, NULL));
    CHECK(!sh_resource_instances_complete(copy, cloned));
    CHECK(sh_resource_instances_assign(copy, cloned, (uintptr_t)(copy + 32), NULL, NULL));
    CHECK(sh_resource_instances_complete(copy, cloned));
    sh_resource_instances_destroyed(copy, cloned, 1); CHECK(leases == 0);
    /* An older destructor cannot retire a new construction at the same address. */
    uint64_t replacement = sh_resource_instances_construct(object, 0);
    CHECK(replacement > original && !sh_resource_instances_complete(object, replacement));
    sh_resource_instances_destroyed(object, original, 1);
    CHECK(sh_resource_instances_token(object) == replacement);
    CHECK(!sh_resource_instances_assign(object, original, (uintptr_t)(object + 8), resource_a, b));
    CHECK(leases == 0);
    /* A failed promotion leaves the earlier field protected and explicitly
     * poisons coverage; no latest-generation or stock substitution occurs. */
    replacement = sh_resource_instances_construct(object, 1);
    CHECK(sh_resource_instances_assign(object, replacement, (uintptr_t)(object + 8), resource_a, a));
    promotion_failure = 1;
    CHECK(!sh_resource_instances_assign(object, replacement, (uintptr_t)(object + 8), resource_a, b));
    CHECK(!sh_resource_instances_complete(object, replacement) && leases == 1);
    cloned = sh_resource_instances_clone(copy, object, sizeof(object));
    CHECK(cloned && leases == 1); /* A live exact origin shares its serving lease without a fresh promotion. */
    promotion_failure = 0;
    sh_resource_instances_destroyed(object, replacement, 1); CHECK(leases == 1);
    sh_resource_instances_destroyed(copy, cloned, 1); CHECK(leases == 0);
    sh_resource_instances_forget(resource_a); sh_resource_instances_forget(resource_b);
    sh_resource_instances_origin_release(a); sh_resource_instances_origin_release(b);
    sh_resource_instances_origin_release(stock);
    count(0, 0, 0); CHECK(captures == 0 && leases == 0);
    test_logical_paths(); test_composite_ownership(); test_native_kind(); test_observation_faults(); test_fault_construction_scopes(); test_copied_field_live_share(); test_payload_live_share();
    /* Grow beyond several bucket generations, then replace and retire in a
     * different order. No small registry limit or linear all-registry walk. */
    stock = sh_resource_instances_origin(NULL, 1);
    for (uintptr_t n = 1; n <= 2048; n++) {
        void *key = (void *)(n * 128);
        CHECK(sh_resource_instances_publish(key, stock));
        CHECK(sh_resource_instances_construct(key, 1));
    }
    for (uintptr_t n = 2048; n; n--) {
        void *key = (void *)(n * 128);
        sh_resource_instance_origin *value = sh_resource_instances_lookup(key);
        CHECK(value == stock); sh_resource_instances_origin_release(value);
        uint64_t before = sh_resource_instances_token(key), after = sh_resource_instances_construct(key, 1);
        CHECK(after > before);
        sh_resource_instances_destroyed(key, before, 1); CHECK(sh_resource_instances_token(key) == after);
        sh_resource_instances_destroyed(key, after, 1); sh_resource_instances_forget(key);
    }
    sh_resource_instances_origin_release(stock); count(0, 0, 0);
    sh_resource_instances_test_reset();
    if (failures) return 1;
    puts("resource instance origins, field replacement, clone and retirement checks passed"); return 0;
}
