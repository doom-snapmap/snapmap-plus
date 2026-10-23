#include "audio_parameter_native.h"
#include "audio_native_image.h"
#include <stdio.h>
#include <stdlib.h>

typedef char ap_layout[(sizeof(sh_audio_parameter_node)==0x70 &&
    offsetof(sh_audio_parameter_node,settings)==0x10 && offsetof(sh_audio_parameter_node,values)==0x30 &&
    offsetof(sh_audio_parameter_node,transitions)==0x48 && offsetof(sh_audio_parameter_node,subscribers)==0x50 &&
    sizeof(sh_audio_parameter_table)==0x610 && sizeof(sh_audio_parameter_vector)==16) ? 1 : -1];
enum { AP_PREPARING,AP_APPLIED,AP_RESTORED,AP_ACCEPTED };
struct sh_audio_parameter_row {
    uint32_t id,desired[4],before[4];
    unsigned selector;
    int seen,installed;
    sh_audio_parameter_node *target,*prepared;
};
static int ap_fail(char *error,size_t capacity,const char *message)
{if(error && capacity)snprintf(error,capacity,"%s",message);return 0;}
int sh_audio_parameter_native_bind(sh_audio_parameter_native *native,const unsigned char *image,
    const sig_result *results,size_t count,const sh_audio_limits_native *limits)
{
    const char *names[]={"AudioStateSettingsRead","AudioParameterEnsure","AudioParameterDestroy"};
    const unsigned char *entry[3]={0};sh_audio_parameter_native bound={0};
    if(!native || native->pending)return 0;memset(native,0,sizeof(*native));
    if(!limits || !limits->render_lock || !limits->manager_slot || (count && !results))return 0;
    for(size_t i=0;i<count;i++)for(size_t j=0;j<3;j++)if(results[i].name && !strcmp(results[i].name,names[j])) {
        if(entry[j] || results[i].status!=SIG_OK)return 0;entry[j]=(const unsigned char *)results[i].addr;
    }
    DWORD code=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,data=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    const size_t spans[]={0x438,0xe0,0x94};
    for(size_t i=0;i<3;i++)if(!ani_section(image,entry[i],spans[i],code,IMAGE_SCN_MEM_WRITE))return 0;
    __try {
        const unsigned char rcx[]={0x48,0x8b,0x0d},ecx[]={0x8b,0x0d},call[]={0xe8};
        bound.parameter_slot=ani_rip(entry[0]+0x390,rcx,3);
        if(!bound.parameter_slot || bound.parameter_slot!=ani_rip(entry[0]+0x3f6,rcx,3) ||
            bound.parameter_slot!=ani_rip(entry[0]+0x429,rcx,3))return 0;
        const unsigned char *defaults=ani_rip(entry[0]+0x397,call,1);
        if(!ani_section(image,defaults,0x20,code,IMAGE_SCN_MEM_WRITE) || ani_rip(defaults+0xc,call,1)!=entry[1])return 0;
        bound.pool=ani_rip(entry[1]+0x48,ecx,2);
        if(!bound.pool || bound.pool!=ani_rip(entry[2]+0x49,ecx,2) || bound.pool!=ani_rip(entry[2]+0x67,ecx,2))return 0;
        bound.allocate=(void *(*)(int,size_t))ani_rip(entry[1]+0x53,call,1);
        bound.release=(void (*)(int,void *))ani_rip(entry[2]+0x4f,call,1);
        if(!bound.release || (void *)bound.release!=ani_rip(entry[2]+0x6d,call,1) ||
            !ani_section(image,bound.allocate,1,code,IMAGE_SCN_MEM_WRITE) ||
            !ani_section(image,bound.release,1,code,IMAGE_SCN_MEM_WRITE))return 0;
        const void *slots[]={bound.pool,bound.parameter_slot,limits->manager_slot,limits->render_lock};
        const size_t sizes[]={4,8,8,sizeof(CRITICAL_SECTION)};
        for(size_t i=0;i<4;i++) {
            if(((uintptr_t)slots[i]&(i ? 7 : 3)) || !ani_section(image,slots[i],sizes[i],data,IMAGE_SCN_MEM_EXECUTE))return 0;
            for(size_t j=0;j<i;j++)if((uintptr_t)slots[i]<(uintptr_t)slots[j]+sizes[j] &&
                (uintptr_t)slots[j]<(uintptr_t)slots[i]+sizes[i])return 0;
        }
        bound.ensure=(sh_audio_parameter_node *(*)(sh_audio_parameter_table *,uint32_t))entry[1];
        bound.destroy=(void (*)(sh_audio_parameter_node *))entry[2];
        bound.manager_slot=limits->manager_slot;bound.render_lock=limits->render_lock;*native=bound;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
static sh_audio_parameter_row *ap_row(sh_audio_parameter_change *c,uint32_t id)
{
    size_t lo=0,hi=c->count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(c->rows[mid].id<id)lo=mid+1;else hi=mid;}
    return lo<c->count && c->rows[lo].id==id ? c->rows+lo : NULL;
}
static int ap_plan(const sh_audio_shared_policy *p,sh_audio_parameter_change *c)
{
    if(!p || (p->count && !p->settings))return 0;
    size_t n=0;for(size_t i=0;i<p->count;i++)n+=p->settings[i].record.kind==SH_AUDIO_SHARED_RTPC;
    if(n>UINT32_MAX || n>SIZE_MAX/sizeof(*c->rows))return 0;
    c->rows=calloc(n ? n : 1,sizeof(*c->rows));if(!c->rows)return 0;
    for(size_t i=0;i<p->count;i++) {
        const sh_audio_shared_setting *s=p->settings+i;const sh_audio_shared_record *r=&s->record;
        if(r->kind!=SH_AUDIO_SHARED_RTPC)continue;
        uint32_t id;if(!s->body || r->offset || r->length!=20 || r->key[1] || r->key[2])return 0;
        memcpy(&id,s->body,4);if(id!=r->key[0] || (c->count && c->rows[c->count-1].id>=id))return 0;
        c->rows[c->count].id=id;memcpy(c->rows[c->count++].desired,s->body+4,16);
    }
    for(size_t i=0;i<p->count;i++) {
        const sh_audio_shared_setting *s=p->settings+i;const sh_audio_shared_record *r=&s->record;
        if(r->kind!=SH_AUDIO_SHARED_BUILTIN)continue;
        if(!s->body || r->offset || r->length!=1 || r->key[1] || r->key[2] || !s->body[0] || s->body[0]>6)return 0;
        sh_audio_parameter_row *row=ap_row(c,r->key[0]);if(!row || row->selector)return 0;row->selector=s->body[0];
    }
    return 1;
}
static sh_audio_parameter_vector *ap_bindings(sh_audio_parameter_table *table)
{return (sh_audio_parameter_vector *)((unsigned char *)table+0xc30);}
static sh_audio_parameter_node *ap_find(const sh_audio_parameter_table *table,uint32_t id)
{
    sh_audio_parameter_node *node=table->buckets[id%193];uint64_t steps=0;
    for(;node && steps<=table->count;node=node->next,steps++)if(node->id==id)return node;
    return NULL;
}
static int ap_table_valid(const sh_audio_parameter_table *table)
{
    uint64_t count=0;
    for(unsigned i=0;i<193;i++)for(sh_audio_parameter_node *node=table->buckets[i];node;node=node->next) {
        if(++count>table->count || node->id%193!=i || ap_find(table,node->id)!=node)return 0;
    }
    return count==table->count;
}
static int ap_vector_valid(sh_audio_parameter_vector v)
{return v.count<=v.capacity && (!v.capacity ? !v.items : v.items!=NULL);}
static int ap_idle(const sh_audio_parameter_node *node)
{
    return !node->has_value && !node->values.count && !node->transitions &&
        !node->subscribers[0].count && !node->subscribers[1].count;
}
static int ap_owned(sh_audio_parameter_change *c)
{return c && c->owner && c->owner->pending==c && c->thread==GetCurrentThreadId();}
static int ap_alive(const sh_audio_parameter_change *c)
{return c->manager && *c->owner->manager_slot==c->manager && *c->owner->parameter_slot==c->target && *c->owner->pool==c->pool;}
static void ap_host_free(sh_audio_parameter_change *c)
{for(unsigned i=0;i<6;i++){free(c->expected[i]);c->expected[i]=NULL;}free(c->rows);c->rows=NULL;c->count=0;}
static void ap_free(sh_audio_parameter_change *c,void **p)
{if(*p){c->uncertain=1;c->owner->release(c->pool,*p);*p=NULL;c->uncertain=0;}}
static int ap_cleanup(sh_audio_parameter_change *c)
{
    for(size_t i=0;i<c->count;i++)if(c->rows[i].prepared) {
        sh_audio_parameter_row *r=c->rows+i;
        if(c->state==AP_ACCEPTED){r->prepared=NULL;continue;}
        c->uncertain=1;c->owner->destroy(r->prepared);c->uncertain=0;ap_free(c,(void **)&r->prepared);
    }
    for(unsigned i=0;i<6;i++)if(c->changed_bindings&(1u<<i)) {
        if(c->state==AP_ACCEPTED)ap_free(c,&c->old_bindings[i].items);else ap_free(c,&c->new_bindings[i].items);
    }
    ap_host_free(c);c->owner->pending=NULL;memset(c,0,sizeof(*c));return 1;
}
static int ap_prepare_bindings(sh_audio_parameter_change *c)
{
    for(unsigned i=0;i<6;i++) {
        sh_audio_parameter_vector old=ap_bindings(c->target)[i];c->old_bindings[i]=old;
        if(!ap_vector_valid(old) || c->count>UINT32_MAX-old.count)return 0;
        size_t maximum=(size_t)old.count+c->count,n=0;
        uint32_t *expected=malloc((maximum ? maximum : 1)*4);if(!expected)return 0;c->expected[i]=expected;
        for(uint32_t j=0;j<old.count;j++) {
            uint32_t id=((uint32_t *)old.items)[j];sh_audio_parameter_row *row=ap_row(c,id);
            if(row){if(row->selector!=i+1 || row->seen)continue;row->seen=1;}
            expected[n++]=id;
        }
        for(size_t j=0;j<c->count;j++)if(c->rows[j].selector==i+1 && !c->rows[j].seen) {
            expected[n++]=c->rows[j].id;c->rows[j].seen=1;
        }
        c->new_bindings[i]=old;
        if(n!=old.count || (n && memcmp(old.items,expected,n*4))) {
            c->changed_bindings|=1u<<i;c->new_bindings[i]=(sh_audio_parameter_vector){NULL,(uint32_t)n,(uint32_t)n};
            if(n) {
                c->new_bindings[i].items=c->owner->allocate(c->pool,n*4);if(!c->new_bindings[i].items)return 0;
                memcpy(c->new_bindings[i].items,expected,n*4);
            }
        }
        if(!ap_alive(c))return 0;
    }
    return 1;
}
static int ap_prepare(sh_audio_parameter_change *c)
{
    c->before=*c->target;c->after=c->before;if(!ap_table_valid(c->target))return 0;
    for(size_t i=0;i<c->count;i++) {
        sh_audio_parameter_row *r=c->rows+i;r->target=ap_find(c->target,r->id);
        if(r->target)memcpy(r->before,r->target->settings,16);
        else {
            if(c->after.count==UINT32_MAX)return 0;
            sh_audio_parameter_table local={0};c->uncertain=1;
            r->prepared=c->owner->ensure(&local,r->id);c->uncertain=0;
            if(!r->prepared)return 0;
            if(local.count!=1 || local.buckets[r->id%193]!=r->prepared || r->prepared->next ||
                r->prepared->id!=r->id || !ap_idle(r->prepared)) {c->uncertain=1;return 0;}
            memcpy(r->prepared->settings,r->desired,16);r->prepared->next=c->after.buckets[r->id%193];
            c->after.buckets[r->id%193]=r->prepared;c->after.count++;
        }
        if(!ap_alive(c))return 0;
    }
    return ap_prepare_bindings(c);
}
static int ap_matches(const sh_audio_parameter_change *c)
{
    if(!ap_alive(c) || memcmp(c->target,c->table_installed ? &c->after : &c->before,sizeof(c->before)))return 0;
    if(c->state==AP_PREPARING || c->state==AP_RESTORED)return 1;
    if(!ap_table_valid(c->target))return 0;
    for(size_t i=0;i<c->count;i++) {
        const sh_audio_parameter_row *r=c->rows+i;sh_audio_parameter_node *node=r->prepared ? r->prepared : r->target;
        if(ap_find(c->target,r->id)!=node || memcmp(node->settings,r->desired,16))return 0;
    }
    for(unsigned i=0;i<6;i++) {
        const sh_audio_parameter_vector *live=ap_bindings(c->target)+i,*expected=c->new_bindings+i;
        if(memcmp(live,expected,sizeof(*live)) || (live->count && memcmp(live->items,c->expected[i],(size_t)live->count*4)))return 0;
    }
    return 1;
}
static int ap_can_restore(const sh_audio_parameter_change *c)
{
    if(!ap_matches(c))return 0;
    for(size_t i=0;i<c->count;i++)if(c->rows[i].prepared && c->table_installed && !ap_idle(c->rows[i].prepared))return 0;
    return 1;
}
static void ap_revert(sh_audio_parameter_change *c)
{
    for(size_t i=0;i<c->count;i++)if(c->rows[i].installed) {
        memcpy(c->rows[i].target->settings,c->rows[i].before,16);c->rows[i].installed=0;
    }
    for(unsigned i=0;i<6;i++)if(c->installed_bindings&(1u<<i)) {
        ap_bindings(c->target)[i]=c->old_bindings[i];c->installed_bindings&=~(1u<<i);
    }
    if(c->table_installed){*c->target=c->before;c->table_installed=0;}
    c->state=AP_RESTORED;
}
int sh_audio_parameter_native_apply(sh_audio_parameter_native *native,const sh_audio_shared_policy *policy,
    sh_audio_parameter_change *change,char *error,size_t capacity)
{
    volatile int held=0,result=0;if(error && capacity)*error=0;
    if(!native || !native->ensure || !native->destroy || !native->allocate || !native->release || !native->pool ||
        !native->parameter_slot || !native->manager_slot || !native->render_lock || native->pending || !change || change->owner)
        return ap_fail(error,capacity,"audio parameter adapter is unavailable or has a pending transition");
    memset(change,0,sizeof(*change));
    if(!ap_plan(policy,change)){ap_host_free(change);memset(change,0,sizeof(*change));return ap_fail(error,capacity,"compiled audio parameters are malformed or unavailable");}
    __try {
        __try {
            EnterCriticalSection(native->render_lock);held=1;
            __try {
                if(*native->parameter_slot && *native->manager_slot) {
                    change->owner=native;change->target=*native->parameter_slot;change->manager=*native->manager_slot;
                    change->thread=GetCurrentThreadId();change->pool=*native->pool;native->pending=change;
                    result=ap_prepare(change);
                    if(result) {
                        for(size_t i=0;i<change->count;i++) {
                            sh_audio_parameter_row *r=change->rows+i;
                            if(r->target && memcmp(r->before,r->desired,16)){r->installed=1;memcpy(r->target->settings,r->desired,16);}
                        }
                        if(change->after.count!=change->before.count){change->table_installed=1;*change->target=change->after;}
                        for(unsigned i=0;i<6;i++)if(change->changed_bindings&(1u<<i)) {
                            change->installed_bindings|=1u<<i;ap_bindings(change->target)[i]=change->new_bindings[i];
                        }
                        change->state=AP_APPLIED;
                    } else if(!ap_alive(change) || change->uncertain || !ap_cleanup(change))result=-1;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                result=-1;if(change->owner){change->uncertain=1;if(ap_alive(change))ap_revert(change);}
            }
        } __finally {if(held)LeaveCriticalSection(native->render_lock);}
    } __except(EXCEPTION_EXECUTE_HANDLER) {if(change->owner)change->uncertain=1;result=-1;}
    if(!change->owner){ap_host_free(change);memset(change,0,sizeof(*change));}
    if(result<=0)ap_fail(error,capacity,"native audio parameter preparation or publication failed");
    return change->owner && result<=0 ? -1 : result<0 ? 0 : result;
}
int sh_audio_parameter_native_ready(sh_audio_parameter_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ap_owned(change) && change->state==AP_APPLIED && !change->uncertain) {
        __try {
            __try {EnterCriticalSection(change->owner->render_lock);held=1;result=ap_matches(change);}
            __finally {if(held)LeaveCriticalSection(change->owner->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {result=0;}
    }
    return result ? 1 : ap_fail(error,capacity,"audio parameters no longer belong to the pending transition");
}
int sh_audio_parameter_native_restore(sh_audio_parameter_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ap_owned(change) && change->state!=AP_ACCEPTED && !change->uncertain) {
        sh_audio_parameter_native *native=change->owner;
        __try {
            __try {EnterCriticalSection(native->render_lock);held=1;if(ap_can_restore(change)){ap_revert(change);result=ap_cleanup(change);}}
            __finally {if(held)LeaveCriticalSection(native->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {if(change->owner)change->uncertain=1;result=0;}
    }
    return result ? 1 : ap_fail(error,capacity,"audio parameter recovery retains native ownership or active users");
}
void sh_audio_parameter_native_accept(sh_audio_parameter_change *change)
{if(ap_owned(change) && change->state==AP_APPLIED && !change->uncertain)change->state=AP_ACCEPTED;}
int sh_audio_parameter_native_release(sh_audio_parameter_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ap_owned(change) && change->state==AP_ACCEPTED && !change->uncertain) {
        sh_audio_parameter_native *native=change->owner;
        __try {
            __try {EnterCriticalSection(native->render_lock);held=1;if(ap_matches(change))result=ap_cleanup(change);}
            __finally {if(held)LeaveCriticalSection(native->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {if(change->owner)change->uncertain=1;result=0;}
    }
    return result ? 1 : ap_fail(error,capacity,"audio parameter retirement retains native ownership");
}
