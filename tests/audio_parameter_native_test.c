#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/audio_parameter_native.c"

/* Guard the second native hash table and selector-zero vector: only the first
 * table and six positive built-in lists belong to this adapter. */
typedef struct manager_fixture {
    sh_audio_parameter_table table;
    unsigned char guard[0x620];
    sh_audio_parameter_vector bindings[6];
} manager_fixture;
typedef char fixture_layout[(offsetof(manager_fixture,bindings)==0xc30) ? 1 : -1];
static manager_fixture live;
static sh_audio_parameter_table *slot;
static sh_audio_parameter_native native;
static CRITICAL_SECTION lock;
static int failures,pool=7,calls,allocations,fail_at,free_calls,free_fault,lose_manager,consumer;
static void *manager,*tracked[64];
static HANDLE attempted,checked;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);failures++;}} while(0)
static void *allocate(int p,size_t bytes)
{
    CHECK(p==pool && lock.OwningThread==(HANDLE)(uintptr_t)GetCurrentThreadId());calls++;
    if(attempted){SetEvent(attempted);CHECK(WaitForSingleObject(checked,5000)==WAIT_OBJECT_0);}
    if(calls==fail_at)return NULL;
    void *value=calloc(1,bytes);CHECK(value);if(!value)return NULL;
    for(unsigned i=0;i<64;i++)if(!tracked[i]){tracked[i]=value;allocations++;if(lose_manager)manager=NULL;return value;}
    CHECK(0);free(value);return NULL;
}
static void release(int p,void *value)
{
    CHECK(p==pool && lock.OwningThread==(HANDLE)(uintptr_t)GetCurrentThreadId());free_calls++;
    for(unsigned i=0;i<64;i++)if(tracked[i]==value){tracked[i]=NULL;allocations--;free(value);
        if(free_fault)RaiseException(0xe0100084,0,0,NULL);return;}
    CHECK(0);
}
static sh_audio_parameter_node *ensure(sh_audio_parameter_table *table,uint32_t id)
{
    CHECK(table!=&live.table && !table->count);
    sh_audio_parameter_node *node=allocate(pool,sizeof(*node));if(!node)return NULL;
    node->id=id;node->vtable=&consumer;node->reserved=0xabcdef;
    node->next=table->buckets[id%193];table->buckets[id%193]=node;table->count++;return node;
}
static void destroy(sh_audio_parameter_node *node)
{
    CHECK(ap_idle(node));
    if(node->values.items)release(pool,node->values.items);
    for(unsigned i=0;i<2;i++)if(node->subscribers[i].items)release(pool,node->subscribers[i].items);
}
static void reset(void)
{
    CHECK(!native.pending && !allocations);memset(&live,0,sizeof(live));memset(live.guard,0xa5,sizeof(live.guard));
    slot=&live.table;manager=&consumer;calls=fail_at=free_calls=free_fault=lose_manager=0;pool=7;
    native=(sh_audio_parameter_native){ensure,destroy,allocate,release,&pool,&slot,&manager,&lock,NULL};
    EnterCriticalSection(&lock);
    for(unsigned i=0;i<2;i++) {
        sh_audio_parameter_table local={0};sh_audio_parameter_node *node=ensure(&local,i ? 99 : 10);
        node->settings[0]=100;node->settings[1]=0;node->settings[2]=20;node->settings[3]=30;
        node->has_value=1;node->value=700;node->transitions=&consumer;
        node->subscribers[0]=(sh_audio_parameter_vector){&consumer,1,1};
        live.table.buckets[node->id%193]=node;live.table.count++;
    }
    live.bindings[0]=(sh_audio_parameter_vector){allocate(pool,8),2,2};
    ((uint32_t *)live.bindings[0].items)[0]=10;((uint32_t *)live.bindings[0].items)[1]=99;
    live.bindings[2]=(sh_audio_parameter_vector){allocate(pool,4),1,1};
    ((uint32_t *)live.bindings[2].items)[0]=777;
    LeaveCriticalSection(&lock);calls=free_calls=0;CHECK(allocations==4);
}
static void clear_live(void)
{
    EnterCriticalSection(&lock);
    for(unsigned i=0;i<193;i++)for(sh_audio_parameter_node *node=live.table.buckets[i];node;) {
        sh_audio_parameter_node *next=node->next;
        node->has_value=0;node->transitions=NULL;memset(node->subscribers,0,sizeof(node->subscribers));
        destroy(node);release(pool,node);node=next;
    }
    for(unsigned i=0;i<6;i++)if(live.bindings[i].items)release(pool,live.bindings[i].items);
    for(size_t i=0;i<sizeof(live.guard);i++)CHECK(live.guard[i]==0xa5);
    memset(&live,0,sizeof(live));LeaveCriticalSection(&lock);CHECK(!allocations);
}
static void word(unsigned char *out,size_t *at,uint32_t n)
{memcpy(out+*at,&n,4);*at+=4;}
static size_t body(unsigned char *out,int edited,int added,unsigned selector)
{
    size_t at=0;float threshold=-48;uint16_t voices=64;
    memcpy(out,&threshold,4);memcpy(out+4,&voices,2);at=6;
    word(out,&at,0);word(out,&at,0);word(out,&at,added ? 3 : 1);
    for(unsigned i=0;i<(added ? 3u : 1u);i++) {
        word(out,&at,10+193*i);word(out,&at,edited ? 200+i : 100);
        word(out,&at,0);word(out,&at,20);word(out,&at,30);
        out[at++]=(unsigned char)(i==0 ? selector : i==1 ? 6 : 2);
    }
    return at;
}
static sh_audio_shared_policy plan(int edited,int added,unsigned selector)
{
    unsigned char bytes[256];char error[256];sh_audio_shared_policy p={0};size_t length=body(bytes,edited,added,selector);
    sh_audio_shared_input input={"STMG",bytes,length,0,0};CHECK(sh_audio_shared_policy_compile(&input,1,&p,error,sizeof(error)));return p;
}
static int contains(unsigned index,uint32_t id)
{
    int found=0;for(uint32_t i=0;i<live.bindings[index].count;i++)found+=((uint32_t *)live.bindings[index].items)[i]==id;
    return found;
}
static void test_transition(void)
{
    reset();sh_audio_shared_policy p=plan(1,1,2);sh_audio_parameter_change c={0},other={0};char error[256];
    manager_fixture before=live;sh_audio_parameter_node *old=ap_find(slot,10),*unrelated=ap_find(slot,99);
    sh_audio_parameter_node old_value=*old,unrelated_value=*unrelated;
    CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1 && !*error);
    CHECK(native.pending==&c && live.table.count==4 && ap_find(slot,10)==old && ap_find(slot,99)==unrelated);
    CHECK(old->settings[0]==200 && old->value==700 && old->has_value && old->transitions==&consumer);
    CHECK(old->subscribers[0].items==&consumer && old->subscribers[0].count==1 && old->reserved==0xabcdef);
    CHECK(!memcmp(unrelated,&unrelated_value,sizeof(*unrelated)) && !memcmp(live.guard,before.guard,sizeof(live.guard)));
    CHECK(ap_find(slot,203)->settings[0]==201 && ap_find(slot,396)->settings[0]==202);
    CHECK(contains(0,10)==0 && contains(0,99)==1 && contains(1,10)==1 && contains(1,396)==1 && contains(5,203)==1);
    CHECK(live.bindings[2].items==before.bindings[2].items && contains(2,777)==1);
    CHECK(!sh_audio_parameter_native_apply(&native,&p,&other,error,sizeof(error)) && !other.owner);
    CHECK(!sh_audio_parameter_native_bind(&native,NULL,NULL,0,NULL) && native.pending==&c);
    CHECK(sh_audio_parameter_native_ready(&c,error,sizeof(error)));sh_audio_shared_policy_free(&p);
    old->value=800;old->subscribers[0].count=0;old->transitions=NULL;
    CHECK(sh_audio_parameter_native_restore(&c,error,sizeof(error)) && !native.pending && allocations==4);
    CHECK(!memcmp(&live,&before,sizeof(live)) && !memcmp(old->settings,old_value.settings,16));
    CHECK(old->value==800 && old->has_value && !old->subscribers[0].count && !old->transitions);
    p=plan(1,1,2);CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1);
    sh_audio_parameter_node *added=ap_find(slot,203);added->subscribers[1]=(sh_audio_parameter_vector){&consumer,1,1};
    manager_fixture after=live;int freed=free_calls;
    CHECK(sh_audio_parameter_native_ready(&c,error,sizeof(error)));sh_audio_parameter_native_accept(&c);
    CHECK(free_calls==freed && !sh_audio_parameter_native_restore(&c,error,sizeof(error)));
    CHECK(sh_audio_parameter_native_release(&c,error,sizeof(error)) && !native.pending);
    CHECK(!memcmp(&live,&after,sizeof(live)) && ap_find(slot,203)==added && added->subscribers[1].count==1);
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_equal_unbound_dedupe(void)
{
    reset();char error[256];sh_audio_parameter_change c={0};sh_audio_shared_policy p=plan(0,0,1);manager_fixture before=live;
    CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1 && !calls && !c.changed_bindings);
    CHECK(!memcmp(&before,&live,sizeof(live)));sh_audio_parameter_native_accept(&c);
    CHECK(sh_audio_parameter_native_release(&c,error,sizeof(error)) && allocations==4);sh_audio_shared_policy_free(&p);
    p=plan(0,0,0);CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1 && !contains(0,10) && contains(0,99));
    CHECK(sh_audio_parameter_native_restore(&c,error,sizeof(error)) && !memcmp(&before,&live,sizeof(live)));sh_audio_shared_policy_free(&p);
    sh_audio_shared_policy absent={0};int allocated=calls;
    CHECK(sh_audio_parameter_native_apply(&native,&absent,&c,error,sizeof(error))==1 && calls==allocated);
    CHECK(sh_audio_parameter_native_restore(&c,error,sizeof(error)));
    ((uint32_t *)live.bindings[0].items)[1]=10;((uint32_t *)live.bindings[2].items)[0]=10;
    p=plan(0,0,1);CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1);
    CHECK(contains(0,10)==1 && !live.bindings[2].count && !live.bindings[2].items);
    CHECK(sh_audio_parameter_native_restore(&c,error,sizeof(error)) && contains(0,10)==2 && contains(2,10)==1);
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_failures(void)
{
    sh_audio_shared_policy p=plan(1,1,2);char error[256];sh_audio_parameter_change c={0};
    reset();CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1);int total=calls;
    CHECK(sh_audio_parameter_native_restore(&c,error,sizeof(error)));clear_live();
    for(int fail=1;fail<=total;fail++) {
        reset();manager_fixture before=live;sh_audio_parameter_node old=*ap_find(slot,10);fail_at=fail;
        CHECK(!sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error)) && !native.pending && !c.owner);
        CHECK(allocations==4 && !memcmp(&before,&live,sizeof(live)) && !memcmp(&old,ap_find(slot,10),sizeof(old)));
        fail_at=0;clear_live();
    }
    reset();lose_manager=1;CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==-1 && c.owner);
    CHECK(!sh_audio_parameter_native_restore(&c,error,sizeof(error)));manager=&consumer;lose_manager=0;
    CHECK(sh_audio_parameter_native_restore(&c,error,sizeof(error)) && allocations==4);
    CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1);
    sh_audio_parameter_native_accept(&c);free_fault=1;
    CHECK(!sh_audio_parameter_native_release(&c,error,sizeof(error)) && c.uncertain && native.pending==&c);
    int freed=free_calls;CHECK(!sh_audio_parameter_native_release(&c,error,sizeof(error)) && free_calls==freed);
    CHECK(!sh_audio_parameter_native_restore(&c,error,sizeof(error)));
    /* The fixture alone knows a release completed before the synthetic fault. */
    for(unsigned i=0;i<64;i++)if(tracked[i]){free(tracked[i]);tracked[i]=NULL;}
    allocations=0;native.pending=NULL;ap_host_free(&c);memset(&live,0,sizeof(live));free_fault=0;
    sh_audio_shared_policy_free(&p);
}
static DWORD WINAPI wrong_owner(void *argument)
{
    char error[256];sh_audio_parameter_change *c=argument;
    CHECK(!sh_audio_parameter_native_ready(c,error,sizeof(error)) && !sh_audio_parameter_native_restore(c,error,sizeof(error)));
    sh_audio_parameter_native_accept(c);CHECK(c->state==AP_APPLIED);CHECK(!sh_audio_parameter_native_release(c,error,sizeof(error)));return 0;
}
static DWORD WINAPI observe_lock(void *unused)
{
    (void)unused;CHECK(WaitForSingleObject(attempted,5000)==WAIT_OBJECT_0);
    int acquired=TryEnterCriticalSection(&lock);CHECK(!acquired);if(acquired)LeaveCriticalSection(&lock);SetEvent(checked);return 0;
}
static void test_ownership(void)
{
    reset();char error[256];sh_audio_parameter_change c={0};sh_audio_shared_policy p=plan(1,1,2);
    attempted=CreateEventA(NULL,TRUE,FALSE,NULL);checked=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE thread=CreateThread(NULL,0,observe_lock,NULL,0,NULL);CHECK(thread);
    CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);CloseHandle(attempted);CloseHandle(checked);attempted=checked=NULL;
    thread=CreateThread(NULL,0,wrong_owner,&c,0,NULL);CHECK(thread);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    sh_audio_parameter_node *added=ap_find(slot,203);
    for(unsigned reason=0;reason<5;reason++) {
        added->has_value=reason==0;added->values.count=reason==1;added->transitions=reason==2 ? &consumer : NULL;
        added->subscribers[0].count=reason==3;added->subscribers[1].count=reason==4;
        CHECK(sh_audio_parameter_native_ready(&c,error,sizeof(error)));
        CHECK(!sh_audio_parameter_native_restore(&c,error,sizeof(error)) && c.owner && live.table.count==4);
    }
    memset(added->subscribers,0,sizeof(added->subscribers));
    added->settings[0]++;CHECK(!sh_audio_parameter_native_ready(&c,error,sizeof(error)));added->settings[0]--;
    ((uint32_t *)live.bindings[1].items)[0]++;CHECK(!sh_audio_parameter_native_restore(&c,error,sizeof(error)));
    ((uint32_t *)live.bindings[1].items)[0]--;
    sh_audio_parameter_node *saved=live.table.buckets[99];live.table.buckets[99]=NULL;
    CHECK(!sh_audio_parameter_native_restore(&c,error,sizeof(error)));live.table.buckets[99]=saved;
    pool++;CHECK(!sh_audio_parameter_native_restore(&c,error,sizeof(error)));pool--;
    sh_audio_parameter_table other=live.table;slot=&other;CHECK(!sh_audio_parameter_native_restore(&c,error,sizeof(error)));slot=&live.table;
    CHECK(sh_audio_parameter_native_restore(&c,error,sizeof(error)) && allocations==4);
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_malformed(void)
{
    reset();char error[256];sh_audio_parameter_change c={0};sh_audio_shared_policy p=plan(1,1,2);size_t parameter=0,binding=0;
    for(size_t i=0;i<p.count;i++){if(p.settings[i].record.kind==SH_AUDIO_SHARED_RTPC)parameter=i;if(p.settings[i].record.kind==SH_AUDIO_SHARED_BUILTIN)binding=i;}
    sh_audio_shared_record saved=p.settings[parameter].record;
    p.settings[parameter].record.length--;CHECK(!sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error)) && !calls);
    p.settings[parameter].record=saved;p.settings[parameter].record.key[0]++;
    CHECK(!sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error)) && !calls);p.settings[parameter].record=saved;
    unsigned char selector=p.settings[binding].body[0];p.settings[binding].body[0]=7;
    CHECK(!sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error)) && !calls);p.settings[binding].body[0]=selector;
    saved=p.settings[binding].record;p.settings[binding].record.key[0]=900;
    CHECK(!sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error)) && !calls);p.settings[binding].record=saved;
    sh_audio_parameter_node *old=ap_find(slot,10);old->next=old;
    CHECK(!sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error)) && !calls && !native.pending);old->next=NULL;
    live.bindings[0].count=3;CHECK(!sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error)) && !native.pending);
    live.bindings[0].count=2;sh_audio_shared_policy_free(&p);clear_live();
}
static void test_composition(void)
{
    reset();char error[256];unsigned char baseline[256],a[256],b[256];
    size_t length=body(baseline,0,0,1);body(a,1,0,1);body(b,0,0,2);
    sh_audio_shared_input inputs[]={{"STMG",baseline,length,0,1},{"STMG",a,length,1,0},{"STMG",b,length,2,0},{"STMG",a,length,3,0}};
    sh_audio_shared_policy p={0};CHECK(sh_audio_shared_policy_compile(inputs,4,&p,error,sizeof(error)));
    sh_audio_parameter_change c={0};CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1);
    CHECK(ap_find(slot,10)->settings[0]==200 && contains(1,10)==1 && !contains(0,10) && live.table.count==2);
    CHECK(sh_audio_parameter_native_restore(&c,error,sizeof(error)));sh_audio_shared_policy_free(&p);
    /* Zero is a native no-op within one bank stream, but a complete next map
     * without the earlier binding must not inherit that previous map's edit. */
    body(b,1,0,0);inputs[0]=(sh_audio_shared_input){"STMG",baseline,length,0,0};inputs[1]=(sh_audio_shared_input){"STMG",b,length,0,0};
    CHECK(sh_audio_shared_policy_compile(inputs,2,&p,error,sizeof(error)));
    CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1 && contains(0,10)==1);
    sh_audio_parameter_native_accept(&c);CHECK(sh_audio_parameter_native_release(&c,error,sizeof(error)));sh_audio_shared_policy_free(&p);
    CHECK(sh_audio_shared_policy_compile(inputs+1,1,&p,error,sizeof(error)));
    CHECK(sh_audio_parameter_native_apply(&native,&p,&c,error,sizeof(error))==1 && !contains(0,10));
    sh_audio_parameter_native_accept(&c);CHECK(sh_audio_parameter_native_release(&c,error,sizeof(error)));sh_audio_shared_policy_free(&p);clear_live();
}
int main(void)
{
    InitializeCriticalSection(&lock);test_transition();test_equal_unbound_dedupe();test_failures();test_ownership();test_malformed();test_composition();
    DeleteCriticalSection(&lock);if(failures){fprintf(stderr,"%d parameter failures\n",failures);return 1;}
    puts("native parameter publication and recovery passed");return 0;
}
