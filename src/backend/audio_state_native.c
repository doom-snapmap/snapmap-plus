#include "audio_state_native.h"
#include "audio_native_image.h"
#include <stdio.h>
#include <stdlib.h>

typedef char ast_layout[(sizeof(sh_audio_state_group)==0x28 &&
    offsetof(sh_audio_state_group,consumers)==0x10 && offsetof(sh_audio_state_group,rules)==0x18 &&
    sizeof(sh_audio_state_entry)==16 && sizeof(sh_audio_state_rules)==16 && sizeof(sh_audio_state_rule)==12) ? 1 : -1];
enum { AST_PREPARING,AST_APPLIED,AST_RESTORED,AST_ACCEPTED };
struct sh_audio_state_row {
    uint32_t id, duration, desired;
    sh_audio_state_group *target, *prepared;
    sh_audio_state_rules before, after;
    sh_audio_state_rule *expected;
    size_t first, count;
    int changed, installed, added;
};
static int ast_fail(char *error,size_t capacity,const char *message)
{ if(error && capacity)snprintf(error,capacity,"%s",message);return 0; }
static uint32_t ast_word(const unsigned char *bytes)
{ uint32_t value;memcpy(&value,bytes,4);return value; }

int sh_audio_state_native_bind(sh_audio_state_native *native,const unsigned char *image,
    const sig_result *results,size_t count,const sh_audio_limits_native *limits)
{
    const unsigned char *read=NULL,*ensure=NULL;sh_audio_state_native bound={0};
    if(!native || native->pending)return 0;memset(native,0,sizeof(*native));
    if(!limits || !limits->render_lock || !limits->manager_slot || (count && !results))return 0;
    for(size_t i=0;i<count;i++) {
        const unsigned char **p=results[i].name && !strcmp(results[i].name,"AudioStateSettingsRead") ? &read :
            results[i].name && !strcmp(results[i].name,"AudioStateGroupEnsure") ? &ensure : NULL;
        if(p){if(*p || results[i].status!=SIG_OK)return 0;*p=(const unsigned char *)results[i].addr;}
    }
    DWORD code=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,data=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    if(!ani_section(image,read,0x1ca,code,IMAGE_SCN_MEM_WRITE) ||
        !ani_section(image,ensure,0xcd,code,IMAGE_SCN_MEM_WRITE))return 0;
    __try {
        static const unsigned char rcx[]={0x48,0x8b,0x0d},ecx[]={0x8b,0x0d},call[]={0xe8};
        bound.state_slot=ani_rip(read+0xf7,rcx,3);
        if(!bound.state_slot || bound.state_slot!=ani_rip(read+0x11a,rcx,3) ||
            bound.state_slot!=ani_rip(read+0x1b5,rcx,3) || ensure!=ani_rip(read+0xfe,call,1))return 0;
        bound.pool=ani_rip(ensure+0x55,ecx,2);
        if(!bound.pool || bound.pool!=ani_rip(ensure+0x9e,ecx,2))return 0;
        bound.allocate=(void *(*)(int,size_t))ani_rip(ensure+0x65,call,1);
        bound.destroy=(void (*)(sh_audio_state_group *))ani_rip(ensure+0x99,call,1);
        bound.release=(void (*)(int,void *))ani_rip(ensure+0xa7,call,1);
        if(!ani_section(image,bound.allocate,1,code,IMAGE_SCN_MEM_WRITE) ||
            !ani_section(image,bound.destroy,0x4b,code,IMAGE_SCN_MEM_WRITE) ||
            !ani_section(image,bound.release,1,code,IMAGE_SCN_MEM_WRITE) ||
            ((uintptr_t)bound.state_slot&7) || ((uintptr_t)bound.pool&3) ||
            !ani_section(image,bound.state_slot,8,data,IMAGE_SCN_MEM_EXECUTE) ||
            !ani_section(image,bound.pool,4,data,IMAGE_SCN_MEM_EXECUTE) ||
            !ani_section(image,limits->manager_slot,8,data,IMAGE_SCN_MEM_EXECUTE) ||
            !ani_section(image,limits->render_lock,sizeof(CRITICAL_SECTION),data,IMAGE_SCN_MEM_EXECUTE))return 0;
        if((void *)bound.state_slot==(void *)limits->manager_slot)return 0;
        bound.ensure=(int (*)(sh_audio_state_table *,uint32_t))ensure;
        bound.manager_slot=limits->manager_slot;bound.render_lock=limits->render_lock;*native=bound;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}

static sh_audio_state_row *ast_row(sh_audio_state_change *c,uint32_t id)
{
    size_t lo=0,hi=c->count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(c->rows[mid].id<id)lo=mid+1;else hi=mid;}
    return lo<c->count && c->rows[lo].id==id ? c->rows+lo : NULL;
}
static void ast_rows_free(sh_audio_state_change *c)
{ for(size_t i=0;i<c->count;i++)free(c->rows[i].expected);free(c->rows);c->rows=NULL;c->count=0; }
/* Shared-policy output is canonical by kind/key. Validate its independent state
 * records before invoking any native allocation or reading native group data. */
static int ast_plan(const sh_audio_shared_policy *p,sh_audio_state_change *c)
{
    if(!p || (p->count && !p->settings))return 0;
    size_t groups=0;
    for(size_t i=0;i<p->count;i++)groups+=p->settings[i].record.kind==SH_AUDIO_SHARED_GROUP;
    if(groups>UINT32_MAX || groups>SIZE_MAX/sizeof(*c->rows))return 0;
    c->rows=calloc(groups ? groups : 1,sizeof(*c->rows));if(!c->rows)return 0;
    for(size_t i=0;i<p->count;i++) {
        const sh_audio_shared_setting *s=p->settings+i;const sh_audio_shared_record *r=&s->record;
        if(r->kind!=SH_AUDIO_SHARED_GROUP)continue;
        if(!s->body || r->offset || r->length!=8 || r->key[1] || r->key[2] ||
            ast_word(s->body)!=r->key[0] || (c->count && c->rows[c->count-1].id>=r->key[0]))return 0;
        sh_audio_state_row *row=c->rows+c->count++;row->id=r->key[0];row->desired=ast_word(s->body+4);
    }
    const sh_audio_shared_record *last=NULL;
    for(size_t i=0;i<p->count;i++) {
        const sh_audio_shared_setting *s=p->settings+i;const sh_audio_shared_record *r=&s->record;
        if(r->kind!=SH_AUDIO_SHARED_TRANSITION)continue;
        if(!s->body || r->offset || r->length!=12 || ast_word(s->body)!=r->key[1] || ast_word(s->body+4)!=r->key[2])return 0;
        if(last && (r->key[0]<last->key[0] || (r->key[0]==last->key[0] &&
            (r->key[1]<last->key[1] || (r->key[1]==last->key[1] && r->key[2]<=last->key[2])))))return 0;
        sh_audio_state_row *row=ast_row(c,r->key[0]);if(!row || row->count==UINT32_MAX)return 0;
        if(!row->count)row->first=i;
        else if(row->first+row->count!=i)return 0;
        row->count++;last=r;
    }
    for(size_t i=0;i<c->count;i++) {
        sh_audio_state_row *r=c->rows+i;
        if(r->count) {
            r->expected=malloc(r->count*sizeof(*r->expected));if(!r->expected)return 0;
            for(size_t j=0;j<r->count;j++)memcpy(r->expected+j,p->settings[r->first+j].body,sizeof(*r->expected));
        }
    }
    return 1;
}
static int ast_rules_valid(sh_audio_state_rules r)
{ return r.count<=r.capacity && (!r.capacity ? !r.items : r.items!=NULL); }
static int ast_table_valid(sh_audio_state_table t)
{ return t.count<=t.capacity && (!t.capacity ? !t.items : t.items!=NULL); }
static sh_audio_state_group *ast_find(sh_audio_state_table t,uint32_t id)
{ for(uint32_t i=0;i<t.count;i++)if(t.items[i].id==id)return t.items[i].group;return NULL; }
static int ast_rule_order(const void *left,const void *right)
{
    const sh_audio_state_rule *a=left,*b=right;
    if(a->from!=b->from)return a->from<b->from ? -1 : 1;
    return a->to<b->to ? -1 : a->to>b->to;
}
static int ast_equal(sh_audio_state_row *r)
{
    if(r->before.count!=r->count)return 0;
    if(!r->count)return 1;
    /* Reuse the already allocated host witness while comparing source order.
     * Full canonical equality also detects duplicate native keys. */
    memcpy(r->expected,r->before.items,r->count*sizeof(*r->expected));
    if(r->count>1)qsort(r->expected,r->count,sizeof(*r->expected),ast_rule_order);
    return !memcmp(r->expected,r->prepared->rules.items,r->count*sizeof(*r->expected));
}
static int ast_owned(sh_audio_state_change *c)
{ return c && c->owner && c->owner->pending==c && c->thread==GetCurrentThreadId(); }
static int ast_alive(const sh_audio_state_change *c)
{ return c->manager && *c->owner->manager_slot==c->manager && *c->owner->state_slot==c->target && *c->owner->pool==c->pool; }
static int ast_matches(const sh_audio_state_change *c)
{
    if(!ast_alive(c))return 0;
    const sh_audio_state_table *table=c->table_installed ? &c->prepared : &c->before;
    if(memcmp(c->target,table,sizeof(*table)))return 0;
    if(c->table_installed && c->before.count &&
        memcmp(c->target->items,c->before.items,(size_t)c->before.count*sizeof(*table->items)))return 0;
    for(size_t i=0;i<c->count;i++) {
        const sh_audio_state_row *r=c->rows+i;
        if(c->state==AST_PREPARING || c->state==AST_RESTORED)continue;
        sh_audio_state_group *live=r->added ? r->prepared : r->target;
        if(ast_find(*c->target,r->id)!=live || live->duration!=r->desired || memcmp(&live->rules,&r->after,sizeof(r->after)) ||
            (r->count && memcmp(live->rules.items,r->expected,r->count*sizeof(*r->expected))))return 0;
    }
    return 1;
}
static void ast_free(sh_audio_state_change *c,void **pointer)
{
    if(!*pointer)return;
    c->uncertain=1;c->owner->release(c->pool,*pointer);*pointer=NULL;c->uncertain=0;
}
static int ast_cleanup(sh_audio_state_change *c)
{
    for(size_t i=0;i<c->count;i++) {
        sh_audio_state_row *r=c->rows+i;
        if(c->state==AST_ACCEPTED && r->added){r->prepared=NULL;continue;}
        if(c->state==AST_ACCEPTED && r->installed) {
            /* The prepared shell no longer owns its published vector. Old
             * storage is detached and only the receipt can release it. */
            memset(&r->prepared->rules,0,sizeof(r->prepared->rules));
            ast_free(c,(void **)&r->before.items);
        }
        if(r->prepared) {
            c->uncertain=1;c->owner->destroy(r->prepared);c->uncertain=0;
            ast_free(c,(void **)&r->prepared);
        }
    }
    if(c->state==AST_ACCEPTED && c->table_installed)ast_free(c,(void **)&c->before.items);
    else ast_free(c,(void **)&c->prepared.items);
    ast_rows_free(c);c->owner->pending=NULL;memset(c,0,sizeof(*c));return 1;
}
static int ast_can_restore(const sh_audio_state_change *c)
{
    if(!ast_matches(c))return 0;
    for(size_t i=0;i<c->count;i++)if(c->rows[i].added && c->table_installed && c->rows[i].prepared->consumers)return 0;
    return 1;
}
static void ast_revert(sh_audio_state_change *c)
{
    for(size_t i=0;i<c->count;i++) {
        sh_audio_state_row *r=c->rows+i;
        if(r->installed){r->target->duration=r->duration;r->target->rules=r->before;r->installed=0;}
    }
    if(c->table_installed){*c->target=c->before;c->table_installed=0;}
    c->state=AST_RESTORED;
}
static int ast_prepare(sh_audio_state_change *c)
{
    size_t added=0;
    /* Check every manager row, including duplicate identities, before preparing
     * a replacement table. Native tables are small and preserve insertion order. */
    if(!ast_table_valid(*c->target))return 0;
    c->before=*c->target;
    for(uint32_t i=0;i<c->before.count;i++) {
        if(!c->before.items[i].group)return 0;
        for(uint32_t j=0;j<i;j++)if(c->before.items[j].id==c->before.items[i].id)return 0;
    }
    for(size_t i=0;i<c->count;i++) {
        sh_audio_state_row *r=c->rows+i;
        r->target=ast_find(c->before,r->id);r->added=!r->target;added+=r->added;
        if(r->target && !ast_rules_valid(r->target->rules))return 0;
        /* Fixed local table storage lets the real constructor allocate exactly
         * one detached group, without owning a temporary manager array. */
        sh_audio_state_entry entry={0};sh_audio_state_table local={&entry,0,1};
        c->uncertain=1;int result=c->owner->ensure(&local,r->id);r->prepared=entry.group;c->uncertain=0;
        if(result!=1 || local.items!=&entry || local.count!=1 || local.capacity!=1 ||
            entry.id!=r->id || !r->prepared || r->prepared->consumers || r->prepared->rules.items ||
            r->prepared->rules.count || r->prepared->rules.capacity)return 0;
        r->prepared->duration=r->desired;
        if(r->count) {
            r->prepared->rules.items=c->owner->allocate(c->pool,r->count*sizeof(sh_audio_state_rule));
            if(!r->prepared->rules.items)return 0;
            r->prepared->rules.count=r->prepared->rules.capacity=(uint32_t)r->count;
            memcpy(r->prepared->rules.items,r->expected,r->count*sizeof(sh_audio_state_rule));
        }
        if(r->target) {
            r->before=r->target->rules;r->changed=r->target->duration!=r->desired || !ast_equal(r);
            r->duration=r->target->duration;
        }
        r->after=r->target && !r->changed ? r->before : r->prepared->rules;
        if(r->count)memcpy(r->expected,r->after.items,r->count*sizeof(*r->expected));
        if(!ast_alive(c))return 0;
    }
    if(added) {
        if(added>UINT32_MAX-c->before.count)return 0;
        c->prepared.count=c->prepared.capacity=c->before.count+(uint32_t)added;
        c->prepared.items=c->owner->allocate(c->pool,(size_t)c->prepared.count*sizeof(sh_audio_state_entry));
        if(!c->prepared.items)return 0;
        if(c->before.count)memcpy(c->prepared.items,c->before.items,(size_t)c->before.count*sizeof(sh_audio_state_entry));
        size_t next=c->before.count;
        for(size_t i=0;i<c->count;i++)if(c->rows[i].added)
            c->prepared.items[next++]=(sh_audio_state_entry){c->rows[i].id,0,c->rows[i].prepared};
    }
    return ast_alive(c);
}
int sh_audio_state_native_apply(sh_audio_state_native *native,const sh_audio_shared_policy *policy,
    sh_audio_state_change *change,char *error,size_t capacity)
{
    volatile int held=0,result=0;if(error && capacity)*error=0;
    if(!native || !native->ensure || !native->destroy || !native->allocate || !native->release || !native->pool ||
        !native->state_slot || !native->manager_slot || !native->render_lock || native->pending || !change || change->owner)
        return ast_fail(error,capacity,"audio state adapter is unavailable or has a pending transition");
    memset(change,0,sizeof(*change));
    if(!ast_plan(policy,change)){ast_rows_free(change);memset(change,0,sizeof(*change));return ast_fail(error,capacity,"compiled audio state groups are malformed or unavailable");}
    __try {
        __try {
            EnterCriticalSection(native->render_lock);held=1;
            __try {
                if(*native->state_slot && *native->manager_slot) {
                    change->owner=native;change->target=*native->state_slot;change->manager=*native->manager_slot;
                    change->thread=GetCurrentThreadId();change->pool=*native->pool;native->pending=change;
                    result=ast_prepare(change);
                    if(result) {
                        for(size_t i=0;i<change->count;i++) {
                            sh_audio_state_row *r=change->rows+i;
                            if(r->changed){r->installed=1;r->target->duration=r->prepared->duration;r->target->rules=r->prepared->rules;}
                        }
                        if(change->prepared.items){change->table_installed=1;*change->target=change->prepared;}
                        change->state=AST_APPLIED;
                    } else if(!ast_alive(change) || change->uncertain || !ast_cleanup(change))result=-1;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                result=-1;
                if(change->owner){change->uncertain=1;if(ast_alive(change))ast_revert(change);}
            }
        } __finally {if(held)LeaveCriticalSection(native->render_lock);}
    } __except(EXCEPTION_EXECUTE_HANDLER) {if(change->owner)change->uncertain=1;result=-1;}
    if(!change->owner){ast_rows_free(change);memset(change,0,sizeof(*change));}
    if(result<=0)ast_fail(error,capacity,"native audio state preparation or publication failed");
    return change->owner && result<=0 ? -1 : result<0 ? 0 : result;
}
int sh_audio_state_native_ready(sh_audio_state_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ast_owned(change) && change->state==AST_APPLIED && !change->uncertain) {
        __try {
            __try {EnterCriticalSection(change->owner->render_lock);held=1;result=ast_matches(change);}
            __finally {if(held)LeaveCriticalSection(change->owner->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {result=0;}
    }
    return result ? 1 : ast_fail(error,capacity,"audio state settings no longer belong to the pending transition");
}
int sh_audio_state_native_restore(sh_audio_state_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ast_owned(change) && change->state!=AST_ACCEPTED && !change->uncertain) {
        sh_audio_state_native *native=change->owner;
        __try {
            __try {
                EnterCriticalSection(native->render_lock);held=1;
                if(ast_can_restore(change)){ast_revert(change);result=ast_cleanup(change);}
            } __finally {if(held)LeaveCriticalSection(native->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {if(change->owner)change->uncertain=1;result=0;}
    }
    return result ? 1 : ast_fail(error,capacity,"audio state recovery retains native ownership or attached consumers");
}
void sh_audio_state_native_accept(sh_audio_state_change *change)
{ if(ast_owned(change) && change->state==AST_APPLIED && !change->uncertain)change->state=AST_ACCEPTED; }
int sh_audio_state_native_release(sh_audio_state_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ast_owned(change) && change->state==AST_ACCEPTED && !change->uncertain) {
        sh_audio_state_native *native=change->owner;
        __try {
            __try {EnterCriticalSection(native->render_lock);held=1;if(ast_matches(change))result=ast_cleanup(change);}
            __finally {if(held)LeaveCriticalSection(native->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {if(change->owner)change->uncertain=1;result=0;}
    }
    return result ? 1 : ast_fail(error,capacity,"audio state retirement retains native ownership");
}
