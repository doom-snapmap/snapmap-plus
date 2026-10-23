#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/audio_state_native.c"

static int failures,allocations,calls,fail_at,free_calls,free_fault,skip_ensure,lose_manager;
static void *tracked[64],*manager;
static sh_audio_state_table live,*state;
static sh_audio_state_native native;
static CRITICAL_SECTION lock;
static int pool=7,consumer;
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
        if(free_fault)RaiseException(0xe0100083,0,0,NULL);return;}
    CHECK(0);
}
static int ensure(sh_audio_state_table *table,uint32_t id)
{
    CHECK(table!=&live && table->count==0 && table->capacity==1 && table->items);
    if(skip_ensure)return 1;
    sh_audio_state_group *group=allocate(pool,sizeof(*group));if(!group)return 2;
    group->reserved=0xabcdef;table->items[0]=(sh_audio_state_entry){id,0,group};table->count=1;return 1;
}
static void destroy(sh_audio_state_group *group)
{
    CHECK(!group->consumers);
    if(group->rules.items)release(pool,group->rules.items);
    memset(&group->rules,0,sizeof(group->rules));
}
static void reset(void)
{
    CHECK(!native.pending && !allocations);memset(&live,0,sizeof(live));state=&live;manager=&consumer;
    calls=fail_at=free_calls=free_fault=skip_ensure=lose_manager=0;pool=7;
    native=(sh_audio_state_native){ensure,destroy,allocate,release,&pool,&state,&manager,&lock,NULL};
    EnterCriticalSection(&lock);
    live.items=allocate(pool,2*sizeof(*live.items));live.count=live.capacity=2;
    for(unsigned i=0;i<2;i++) {
        sh_audio_state_entry entry={0};sh_audio_state_table table={&entry,0,1};CHECK(ensure(&table,i ? 99 : 10)==1);
        live.items[i]=entry;entry.group->duration=100;entry.group->current=22;entry.group->consumers=&consumer;
    }
    sh_audio_state_group *g=live.items[0].group;
    g->rules.items=allocate(pool,2*sizeof(sh_audio_state_rule));g->rules.count=g->rules.capacity=2;
    g->rules.items[0]=(sh_audio_state_rule){3,4,40};g->rules.items[1]=(sh_audio_state_rule){1,2,20};
    LeaveCriticalSection(&lock);calls=free_calls=0;CHECK(allocations==4);
}
static void clear_live(void)
{
    EnterCriticalSection(&lock);
    for(uint32_t i=0;i<live.count;i++){live.items[i].group->consumers=NULL;destroy(live.items[i].group);release(pool,live.items[i].group);}
    if(live.items)release(pool,live.items);memset(&live,0,sizeof(live));LeaveCriticalSection(&lock);CHECK(!allocations);
}
static void word(unsigned char *body,size_t *at,uint32_t n)
{ memcpy(body+*at,&n,4);*at+=4; }
static size_t body(unsigned char *out,int edited,int added,int empty)
{
    size_t at=0;float threshold=-48;uint16_t voices=64;
    memcpy(out,&threshold,4);memcpy(out+4,&voices,2);at=6;word(out,&at,added ? 2 : 1);
    word(out,&at,10);word(out,&at,edited ? 200 : 100);word(out,&at,empty ? 0 : 2);
    if(!empty){word(out,&at,1);word(out,&at,2);word(out,&at,edited ? 70 : 20);word(out,&at,3);word(out,&at,4);word(out,&at,40);}
    if(added){word(out,&at,40);word(out,&at,300);word(out,&at,1);word(out,&at,1);word(out,&at,9);word(out,&at,90);}
    word(out,&at,0);word(out,&at,0);return at;
}
static sh_audio_shared_policy plan(int edited,int added,int empty)
{
    unsigned char bytes[256];size_t length=body(bytes,edited,added,empty);char error[256];sh_audio_shared_policy policy={0};
    sh_audio_shared_input input={"STMG",bytes,length,0,0};CHECK(sh_audio_shared_policy_compile(&input,1,&policy,error,sizeof(error)));return policy;
}
static void test_transition(void)
{
    reset();sh_audio_shared_policy p=plan(1,1,0);sh_audio_state_table before=live;
    sh_audio_state_group *old=live.items[0].group,*unrelated=live.items[1].group;sh_audio_state_rules rules=old->rules;
    sh_audio_state_change c={0},other={0};char error[256];
    CHECK(sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error))==1 && !*error);
    CHECK(native.pending==&c && live.count==3 && live.items!=before.items && allocations==9);
    CHECK(live.items[0].group==old && live.items[1].group==unrelated && old->duration==200 && old->current==22);
    CHECK(old->consumers==&consumer && old->reserved==0xabcdef && old->rules.items!=rules.items);
    CHECK(ast_find(live,40)->duration==300 && ast_find(live,40)->current==0 && !ast_find(live,40)->consumers);
    CHECK(!sh_audio_state_native_apply(&native,&p,&other,error,sizeof(error)) && !other.owner);
    CHECK(!sh_audio_state_native_bind(&native,NULL,NULL,0,NULL) && native.pending==&c);
    CHECK(sh_audio_state_native_ready(&c,error,sizeof(error)));
    sh_audio_shared_policy_free(&p);
    old->current=77;old->consumers=NULL;
    CHECK(sh_audio_state_native_restore(&c,error,sizeof(error)) && !native.pending && allocations==4);
    CHECK(!memcmp(&live,&before,sizeof(live)) && !memcmp(&old->rules,&rules,sizeof(rules)) && old->duration==100);
    CHECK(old->current==77 && !old->consumers && old->reserved==0xabcdef);
    p=plan(1,1,0);CHECK(sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error))==1);
    sh_audio_state_table after=live;sh_audio_state_group *added=ast_find(live,40);added->consumers=&consumer;
    CHECK(sh_audio_state_native_ready(&c,error,sizeof(error)));int freed=free_calls;
    sh_audio_state_native_accept(&c);CHECK(c.owner && free_calls==freed && !sh_audio_state_native_restore(&c,error,sizeof(error)));
    CHECK(sh_audio_state_native_release(&c,error,sizeof(error)) && !native.pending && allocations==6);
    CHECK(!memcmp(&after,&live,sizeof(live)) && ast_find(live,40)==added && added->consumers==&consumer);
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_equal_empty_absent(void)
{
    reset();char error[256];sh_audio_state_change c={0};sh_audio_shared_policy p=plan(0,0,0);
    sh_audio_state_table before=live;sh_audio_state_group *g=live.items[0].group;sh_audio_state_rules rules=g->rules;
    CHECK(sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error))==1 && !c.rows[0].changed);
    CHECK(!memcmp(&live,&before,sizeof(live)) && !memcmp(&g->rules,&rules,sizeof(rules)));
    CHECK(sh_audio_state_native_ready(&c,error,sizeof(error)));sh_audio_state_native_accept(&c);
    CHECK(sh_audio_state_native_release(&c,error,sizeof(error)) && allocations==4);sh_audio_shared_policy_free(&p);
    p=plan(0,0,1);CHECK(sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error))==1 && !g->rules.items && !g->rules.count);
    CHECK(sh_audio_state_native_restore(&c,error,sizeof(error)) && !memcmp(&g->rules,&rules,sizeof(rules)));
    sh_audio_shared_policy_free(&p);sh_audio_shared_policy absent={0};int allocated=calls;
    CHECK(sh_audio_state_native_apply(&native,&absent,&c,error,sizeof(error))==1 && calls==allocated);
    CHECK(sh_audio_state_native_restore(&c,error,sizeof(error)));clear_live();
}
static void test_failures(void)
{
    sh_audio_shared_policy p=plan(1,1,0);char error[256];
    for(int fail=1;fail<=5;fail++) {
        reset();sh_audio_state_table before=live;sh_audio_state_group group=*live.items[0].group;
        sh_audio_state_change c={0};fail_at=fail;
        CHECK(!sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error)) && !native.pending && !c.owner);
        CHECK(allocations==4 && !memcmp(&before,&live,sizeof(live)) && !memcmp(&group,live.items[0].group,sizeof(group)));
        fail_at=0;clear_live();
    }
    reset();sh_audio_state_change c={0};skip_ensure=1;
    CHECK(!sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error)) && !native.pending);skip_ensure=0;
    lose_manager=1;CHECK(sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error))==-1 && c.owner);
    CHECK(!sh_audio_state_native_restore(&c,error,sizeof(error)));manager=&consumer;lose_manager=0;
    CHECK(sh_audio_state_native_restore(&c,error,sizeof(error)) && allocations==4);
    CHECK(sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error))==1);
    sh_audio_state_native_accept(&c);free_fault=1;
    CHECK(!sh_audio_state_native_release(&c,error,sizeof(error)) && c.uncertain && native.pending==&c);
    int freed=free_calls;CHECK(!sh_audio_state_native_release(&c,error,sizeof(error)) && free_calls==freed);
    CHECK(!sh_audio_state_native_restore(&c,error,sizeof(error)));
    /* Only the fixture allocator knows which free completed before the fault. */
    for(unsigned i=0;i<64;i++)if(tracked[i]){free(tracked[i]);tracked[i]=NULL;}
    allocations=0;native.pending=NULL;ast_rows_free(&c);memset(&live,0,sizeof(live));free_fault=0;
    sh_audio_shared_policy_free(&p);
}
static DWORD WINAPI wrong_owner(void *argument)
{
    char error[256];sh_audio_state_change *c=argument;
    CHECK(!sh_audio_state_native_ready(c,error,sizeof(error)) && !sh_audio_state_native_restore(c,error,sizeof(error)));
    sh_audio_state_native_accept(c);CHECK(c->state==AST_APPLIED);CHECK(!sh_audio_state_native_release(c,error,sizeof(error)));return 0;
}
static DWORD WINAPI observe_lock(void *unused)
{
    (void)unused;CHECK(WaitForSingleObject(attempted,5000)==WAIT_OBJECT_0);
    int acquired=TryEnterCriticalSection(&lock);CHECK(!acquired);if(acquired)LeaveCriticalSection(&lock);
    SetEvent(checked);return 0;
}
static void test_ownership(void)
{
    reset();char error[256];sh_audio_state_change c={0};sh_audio_shared_policy p=plan(1,1,0);
    attempted=CreateEventA(NULL,TRUE,FALSE,NULL);checked=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE thread=CreateThread(NULL,0,observe_lock,NULL,0,NULL);CHECK(thread);
    CHECK(sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error))==1);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);CloseHandle(attempted);CloseHandle(checked);attempted=checked=NULL;
    thread=CreateThread(NULL,0,wrong_owner,&c,0,NULL);CHECK(thread);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);
    sh_audio_state_group *g=ast_find(live,10),*added=ast_find(live,40);added->consumers=&consumer;
    CHECK(!sh_audio_state_native_restore(&c,error,sizeof(error)) && c.owner && g->duration==200 && live.count==3);
    CHECK(sh_audio_state_native_ready(&c,error,sizeof(error)));added->consumers=NULL;
    g->rules.items[0].duration++;CHECK(!sh_audio_state_native_ready(&c,error,sizeof(error)));
    CHECK(!sh_audio_state_native_restore(&c,error,sizeof(error)));g->rules.items[0].duration--;
    sh_audio_state_rule saved=g->rules.items[0];g->rules.items[0]=g->rules.items[1];
    CHECK(!sh_audio_state_native_ready(&c,error,sizeof(error)));g->rules.items[0]=saved;
    added->duration++;CHECK(!sh_audio_state_native_ready(&c,error,sizeof(error)));added->duration--;
    added->rules.items[0].duration++;CHECK(!sh_audio_state_native_ready(&c,error,sizeof(error)));added->rules.items[0].duration--;
    live.items[1].id++;CHECK(!sh_audio_state_native_restore(&c,error,sizeof(error)));live.items[1].id--;
    sh_audio_state_table other=live;state=&other;CHECK(!sh_audio_state_native_restore(&c,error,sizeof(error)));state=&live;
    pool++;CHECK(!sh_audio_state_native_restore(&c,error,sizeof(error)));pool--;
    CHECK(sh_audio_state_native_restore(&c,error,sizeof(error)) && allocations==4);
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_malformed(void)
{
    reset();char error[256];sh_audio_state_change c={0};sh_audio_shared_policy p=plan(1,1,0);
    size_t group=0,transition=0;
    for(size_t i=0;i<p.count;i++){if(p.settings[i].record.kind==SH_AUDIO_SHARED_GROUP)group=i;if(p.settings[i].record.kind==SH_AUDIO_SHARED_TRANSITION)transition=i;}
    sh_audio_shared_record before=p.settings[group].record;
    p.settings[group].record.length--;CHECK(!sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error)) && !calls);
    p.settings[group].record=before;p.settings[group].record.key[0]++;
    CHECK(!sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error)) && !calls);p.settings[group].record=before;
    before=p.settings[transition].record;p.settings[transition].record.key[0]=900;
    CHECK(!sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error)) && !calls);p.settings[transition].record=before;
    p.settings[transition].record.offset=1;CHECK(!sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error)) && !calls);
    p.settings[transition].record=before;live.items[1].id=10;
    CHECK(!sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error)) && !calls && !native.pending);live.items[1].id=99;
    sh_audio_shared_policy_free(&p);clear_live();
}
static void test_composed_rules(void)
{
    reset();char error[256];unsigned char baseline[256],a[256],b[256];
    size_t length=body(baseline,0,0,0);memcpy(a,baseline,length);memcpy(b,baseline,length);
    uint32_t duration=70;memcpy(a+30,&duration,4);duration=90;memcpy(b+42,&duration,4);
    sh_audio_shared_input inputs[]={{"STMG",baseline,length,0,1},{"STMG",a,length,1,0},{"STMG",b,length,2,0},{"STMG",a,length,3,0}};
    sh_audio_shared_policy p={0};CHECK(sh_audio_shared_policy_compile(inputs,4,&p,error,sizeof(error)));
    sh_audio_state_change c={0};CHECK(sh_audio_state_native_apply(&native,&p,&c,error,sizeof(error))==1);
    sh_audio_state_group *g=ast_find(live,10);CHECK(live.count==2 && g->rules.count==2 && g->rules.items[0].duration==70 && g->rules.items[1].duration==90);
    CHECK(sh_audio_state_native_restore(&c,error,sizeof(error)));sh_audio_shared_policy_free(&p);clear_live();
}
int main(void)
{
    InitializeCriticalSection(&lock);test_transition();test_equal_empty_absent();test_failures();
    test_ownership();test_malformed();test_composed_rules();DeleteCriticalSection(&lock);
    if(failures){fprintf(stderr,"%d state failures\n",failures);return 1;}puts("native state group publication and recovery passed");return 0;
}
