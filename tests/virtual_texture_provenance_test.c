#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource_instances.h"
static int fail_allocation;
static void *vt_test_calloc(size_t count,size_t size)
{ if(fail_allocation) { fail_allocation=0; return NULL; } return calloc(count,size); }
static void *vt_test_realloc(void *p,size_t size)
{ if(fail_allocation) { fail_allocation=0; return NULL; } return realloc(p,size); }
#define calloc vt_test_calloc
#define realloc vt_test_realloc
#include "../src/backend/virtual_texture_provider.c"
#undef calloc
#undef realloc

void *hook_prepare(void *target,void *detour,size_t size)
{ (void)target;(void)detour;(void)size; assert(0);return NULL; }
sh_patch_status hook_commit(void *value) { (void)value;assert(0);return B2_PATCH_OK; }
int hook_unpatch(void *value) { (void)value;assert(0);return 0; }
void backend_log(const char *message) { (void)message; }
typedef struct capture { int live,promoted,released,freed; } capture;
static capture captures[64]; static size_t next_capture;
static sh_resource_instance_origin *inputs[4], *failed_opening[16];
static size_t input_count,failed_count;
static unsigned char *test_system;
static size_t test_stride;
static void *test_head;
static int mode_object=2,owner_object,file_object,fail_set,throw_set,throw_release,cleanup_failure,nested;
static int native_releases,lock_checks;
static char source_names[32][64],source_paths[32][96];
static void check_unlocked(void);
static const char *empty="", *name="observed", *path="virtualtextures/observed.pages";
static unsigned char *test_provider(int slot) { return test_system+(size_t)slot*test_stride; }
static void put(void *object,size_t offset,const void *value) { memcpy((unsigned char *)object+offset,&value,8); }
static int promote(const void *value,void **out)
{ check_unlocked();capture *c=(capture *)value; c->live++; c->promoted++; *out=c; return 1; }
static void release_lease(void *value)
{ check_unlocked();capture *c=value; assert(c->live>0); c->live--;c->released++; }
static void release_capture(void *value) { check_unlocked();((capture *)value)->freed++; }
static sh_resource_instance_origin *input(capture **out)
{
    assert(next_capture<64); *out=captures+next_capture++;
    sh_resource_instance_origin *origin=sh_resource_instances_origin(*out,0); assert(origin); return origin;
}
static void check_unlocked(void)
{
    assert(TryAcquireSRWLockExclusive(&g_vt_lock)); ReleaseSRWLockExclusive(&g_vt_lock);
    assert(TryAcquireSRWLockExclusive(&g_vt_origin_lock)); ReleaseSRWLockExclusive(&g_vt_origin_lock); lock_checks++;
}
static void positive_read(sh_resource_instance_origin *origin)
{
    check_unlocked(); assert(sh_vt_provider_source_recording());
    if(origin) assert(sh_resource_instances_origin_hold(origin));
    int observed=sh_vt_provider_source_observed(origin);
    if(origin && !observed) { assert(failed_count<16); failed_opening[failed_count++]=origin; }
    else sh_resource_instances_origin_unhold(origin);
}
static void native_release(void *provider)
{
    check_unlocked(); native_releases++;
    if(throw_release) RaiseException(0xe0420001,0,0,NULL);
    put(provider,0x70,empty); put(provider,0x1a0,empty);
    memset(provider,0,0x60); put(provider,0x2d0,NULL); put(provider,0x2e8,NULL); put(provider,0x3d8,NULL);
}
static void native_quiesce(void *system) { assert(system==test_system);check_unlocked(); }
static unsigned char native_set(void *provider,const char *n,const char *p,int mode,int kind,unsigned char direct,void *reader)
{
    (void)mode;(void)direct;(void)reader;
    check_unlocked(); assert(!sh_vt_provider_source_recording());
    sh_vt_provider_test_release(provider);
    assert(sh_vt_provider_source_recording());
    if(nested && provider==test_provider(1)) {
        nested=0; assert(sh_vt_provider_test_set_source(test_provider(2),name,path,2,2,0,NULL));
        assert(sh_vt_provider_source_recording());
    }
    for(size_t i=0;i<input_count;i++) { positive_read(inputs[i]); positive_read(inputs[i]); }
    size_t slot=((unsigned char *)provider-test_system)/test_stride;
    snprintf(source_names[slot],sizeof(source_names[slot]),"%s",n);
    snprintf(source_paths[slot],sizeof(source_paths[slot]),"%s",p);
    put(provider,0x70,source_paths[slot]); put(provider,0x1a0,source_names[slot]); put(provider,0x3d8,&mode_object);
    memcpy(provider,&kind,4);
    put(provider,8,&owner_object); put(provider,0x2d0,&file_object);
    if(throw_set) RaiseException(0xe0420002,0,0,NULL);
    if(fail_set && cleanup_failure) sh_vt_provider_test_release(provider);
    return (unsigned char)!fail_set;
}
static void setup(size_t stride)
{
    test_stride=stride; test_system=calloc(1,32*stride+32*8); assert(test_system);
    for(int slot=0;slot<32;slot++) {
        void *provider=test_provider(slot); put(test_system,32*stride+(size_t)slot*8,provider);
        put(provider,0x70,empty);put(provider,0x1a0,empty); memcpy((unsigned char *)provider+0x2c0,&slot,4);
    }
    sh_vt_native api={0}; api.system=test_system;api.stride=stride;api.resources=&test_head;
    api.set_source=native_set;api.release=native_release;api.quiesce=native_quiesce;
    assert(sh_vt_provider_bind(&api));assert(sh_vt_provider_test_provenance());
}
static void construct(int slot)
{ assert(sh_vt_provider_test_set_source(test_provider(slot),name,path,2,2,0,NULL)); }
static void initialized(void *system)
{
    sh_vt_provider_read during={0}; assert(system==test_system);
    assert(!sh_vt_provider_read_begin(test_provider(2),&during)); construct(1);
}
static void initialized_release(void *system)
{ initialized(system);sh_vt_provider_test_release(test_provider(2)); }
static void initialized_failure(void *system)
{ assert(system==test_system);construct(1);RaiseException(0xe0420003,0,0,NULL); }
static void recover_openings(void)
{ while(failed_count) sh_resource_instances_origin_unhold(failed_opening[--failed_count]); }
static void controls(size_t stride)
{
    capture *a,*b,*c; sh_resource_instance_origin *oa=input(&a),*ob=input(&b),*oc=input(&c);
    setup(stride); inputs[0]=oa;inputs[1]=ob;input_count=2;
    assert(!sh_vt_provider_source_observed(oa)); construct(1);
    assert(a->live==1 && b->live==1);
    { const char *private_names[]={"private-observed"};char error[128];
      assert(sh_vt_provider_acquire(private_names,1,error,sizeof(error)));
      assert(sh_vt_provider_collect(NULL,NULL)==0); }
    sh_vt_provider_read old={0}; assert(sh_vt_provider_read_begin(test_provider(1),&old));
    uint64_t first=old.generation; assert(old.origin);
    inputs[0]=oc;input_count=1;construct(1);
    assert(a->live==1 && b->live==1 && c->live==1); /* Old read still serves A+B. */
    assert(!sh_vt_provider_read_finish(&old,1,1)); assert(!a->live && !b->live);
    sh_vt_provider_read current={0};assert(sh_vt_provider_read_begin(test_provider(1),&current));
    assert(current.generation!=first);assert(sh_vt_provider_read_finish(&current,1,1));
    assert(!sh_vt_provider_read_finish(&current,1,1));
    assert(sh_vt_provider_read_begin(test_provider(1),&current));
    assert(!sh_vt_provider_read_finish(&current,0,1));assert(c->live==1);
    assert(sh_vt_provider_read_begin(test_provider(1),&current));
    assert(!sh_vt_provider_read_finish(&current,1,0));
    throw_release=1;int caught=0;
    __try { sh_vt_provider_test_release(test_provider(1)); } __except(EXCEPTION_EXECUTE_HANDLER) { caught=1; }
    assert(caught && c->live==1 && !sh_vt_provider_read_begin(test_provider(1),&current));
    throw_release=0;sh_vt_provider_test_release(test_provider(1));assert(!c->live);

    /* Failed construction keeps actual input shares until storage destruction. */
    inputs[0]=oa;fail_set=1;assert(!sh_vt_provider_test_set_source(test_provider(1),name,path,2,2,0,NULL));
    assert(a->live==1 && !sh_vt_provider_read_begin(test_provider(1),&current));
    sh_vt_provider_test_release(test_provider(1));assert(!a->live);
    cleanup_failure=1;assert(!sh_vt_provider_test_set_source(test_provider(1),name,path,2,2,0,NULL));
    assert(!a->live);cleanup_failure=0;fail_set=0;
    throw_set=1;caught=0;
    __try { construct(1); } __except(EXCEPTION_EXECUTE_HANDLER) { caught=1; }
    assert(caught && a->live==1);throw_set=0;
    sh_vt_provider_test_release(test_provider(1));assert(!a->live);

    inputs[0]=oa;inputs[1]=NULL;input_count=2;construct(1);
    assert(a->live==1 && !sh_vt_provider_read_begin(test_provider(1),&current));
    sh_vt_provider_test_release(test_provider(1));assert(!a->live);
    input_count=1;fail_allocation=1;construct(1); /* Scope allocation failure. */
    assert(failed_count==2 && a->live==1 && !sh_vt_provider_read_begin(test_provider(1),&current));
    sh_vt_provider_test_release(test_provider(1));recover_openings();assert(!a->live);

    nested=1;construct(1);assert(sh_vt_provider_read_begin(test_provider(1),&current));
    assert(sh_vt_provider_read_finish(&current,1,1));
    assert(sh_vt_provider_read_begin(test_provider(2),&current));assert(sh_vt_provider_read_finish(&current,1,1));
    assert(a->live==1); /* Shared origin means one native lease. */
    assert(sh_vt_provider_read_begin(test_provider(2),&old));
    uint64_t prior_init=old.generation;
    inputs[0]=ob;sh_vt_provider_test_init(initialized);
    assert(!sh_vt_provider_read_finish(&old,1,1));
    assert(sh_vt_provider_read_begin(test_provider(1),&current));assert(sh_vt_provider_read_finish(&current,1,1));
    /* Untouched slot2 retains the exact original source, with a new receipt
     * generation. Numeric cache reset is not native source destruction. */
    assert(sh_vt_provider_read_begin(test_provider(2),&current));
    assert(current.generation!=prior_init && current.origin);
    assert(sh_vt_provider_read_finish(&current,1,1));assert(a->live==1 && b->live==1);
    sh_vt_provider_test_init(initialized_release);
    assert(!a->live && !sh_vt_provider_read_begin(test_provider(2),&current));
    inputs[0]=oc;caught=0;
    __try { sh_vt_provider_test_init(initialized_failure); } __except(EXCEPTION_EXECUTE_HANDLER) { caught=1; }
    assert(caught && !b->live && c->live==1 && !sh_vt_provider_read_begin(test_provider(1),&current));
    sh_vt_provider_test_release(test_provider(1));assert(!c->live);
    sh_vt_provider_test_reset();assert(!b->live);free(test_system);test_system=NULL;
    sh_resource_instances_origin_release(oa);sh_resource_instances_origin_release(ob);sh_resource_instances_origin_release(oc);
    assert(a->freed==1 && b->freed==1 && c->freed==1);
}
int main(void)
{
    sh_resource_instance_api api={promote,release_capture,release_lease};assert(sh_resource_instances_bind(api));
    controls(0x458);controls(0x520);
    size_t resources,instances,fields;sh_resource_instances_counts(&resources,&instances,&fields);
    assert(!resources && !instances && !fields && !failed_count && !g_vt_retained);
    printf("virtual_texture_provenance_test: passed; %d unlocked native/source boundaries\n",lock_checks);return 0;
}
