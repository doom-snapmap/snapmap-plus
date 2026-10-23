#include "audio_environment_native.h"
#include "audio_native_image.h"
#include <stdio.h>

typedef char ae_layout[(sizeof(sh_audio_environment)==0x68 &&
    offsetof(sh_audio_environment,curves)==8 && sizeof(sh_audio_native_curve)==16) ? 1 : -1];
enum { AE_PREPARING,AE_APPLIED,AE_RESTORED,AE_ACCEPTED };
static int ae_fail(char *error,size_t capacity,const char *message)
{ if(error && capacity)snprintf(error,capacity,"%s",message);return 0; }
int sh_audio_environment_native_bind(sh_audio_environment_native *native,
    const unsigned char *image,const sig_result *results,size_t count,const sh_audio_limits_native *limits)
{
    const unsigned char *read=NULL,*set=NULL;sh_audio_environment_native bound={0};
    if(!native || native->pending)return 0;memset(native,0,sizeof(*native));
    if(!limits || !limits->render_lock || !limits->manager_slot || (count && !results))return 0;
    for(size_t i=0;i<count;i++) {
        const unsigned char **p=results[i].name && !strcmp(results[i].name,"AudioEnvironmentRead") ? &read :
            results[i].name && !strcmp(results[i].name,"AudioEnvironmentSet") ? &set : NULL;
        if(p){if(*p || results[i].status!=SIG_OK)return 0;*p=(const unsigned char *)results[i].addr;}
    }
    DWORD code=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,data=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE;
    if(!ani_section(image,read,0x13c,code,IMAGE_SCN_MEM_WRITE) ||
        !ani_section(image,set,0x62,code,IMAGE_SCN_MEM_WRITE))return 0;
    __try {
        static const unsigned char rax[]={0x48,0x8b,0x05},rcx[]={0x48,0x8b,0x0d},call[]={0xe8};
        bound.environment_slot=ani_rip(read+0x8d,rax,sizeof(rax));
        if(!bound.environment_slot || bound.environment_slot!=ani_rip(read+0x127,rcx,sizeof(rcx)) ||
            set!=ani_rip(read+0x137,call,sizeof(call)))return 0;
        void *curve=ani_rip(set+0x37,call,sizeof(call));
        if(!ani_section(image,curve,0xb2,code,IMAGE_SCN_MEM_WRITE) || ((uintptr_t)bound.environment_slot&7) ||
            !ani_section(image,bound.environment_slot,8,data,IMAGE_SCN_MEM_EXECUTE) ||
            !ani_section(image,limits->manager_slot,8,data,IMAGE_SCN_MEM_EXECUTE) ||
            !ani_section(image,limits->render_lock,sizeof(CRITICAL_SECTION),data,IMAGE_SCN_MEM_EXECUTE))return 0;
        uintptr_t slot=(uintptr_t)bound.environment_slot,manager=(uintptr_t)limits->manager_slot,lock=(uintptr_t)limits->render_lock;
        if(slot==manager || (slot<lock+sizeof(CRITICAL_SECTION) && lock<slot+8))return 0;
        bound.set_environment=(int (*)(sh_audio_environment *,int,int,uint32_t,const void *,uint32_t))set;
        bound.set_curve=(int (*)(sh_audio_native_curve *,const void *,uint32_t,uint32_t))curve;
        bound.manager_slot=limits->manager_slot;bound.render_lock=limits->render_lock;*native=bound;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ae_policy(const sh_audio_shared_policy *policy,const sh_audio_shared_setting **slots)
{
    if(!policy || (policy->count && !policy->settings))return 0;
    for(size_t i=0;i<policy->count;i++) {
        const sh_audio_shared_setting *s=policy->settings+i;
        if(s->record.kind!=SH_AUDIO_SHARED_ENVIRONMENT)continue;
        uint32_t key=s->record.key[0];uint16_t count;
        if(key>=6 || slots[key] || s->record.key[1] || s->record.key[2] || s->record.offset ||
            !s->body || s->record.length<4)return 0;
        memcpy(&count,s->body+2,2);
        if(s->record.length!=4+(size_t)count*12)return 0;
        slots[key]=s;
    }
    return 1;
}
static int ae_curve_valid(const sh_audio_native_curve *c)
{ return c->count<=UINT16_MAX && ((!c->points && !c->count) || (c->points && c->count)); }
static int ae_equal(const sh_audio_native_curve *a,const sh_audio_native_curve *b)
{
    return a->count==b->count && a->scaling==b->scaling &&
        (!a->count || !memcmp(a->points,b->points,(size_t)a->count*12));
}
static int ae_owned(sh_audio_environment_change *c)
{ return c && c->owner && c->owner->pending==c && c->thread==GetCurrentThreadId(); }
static int ae_alive(const sh_audio_environment_change *c)
{ return *c->owner->manager_slot==c->manager && c->manager && *c->owner->environment_slot==c->target && c->target; }
static int ae_matches(const sh_audio_environment_change *c)
{
    if(!ae_alive(c))return 0;
    for(unsigned i=0;i<6;i++)if(c->installed&(1u<<i)) {
        if(c->target->enabled[i]!=c->prepared.enabled[i] ||
            memcmp(c->target->curves+i,c->prepared.curves+i,sizeof(sh_audio_native_curve)))return 0;
    }
    return 1;
}
static void ae_forget(sh_audio_environment_change *c)
{ c->owner->pending=NULL;memset(c,0,sizeof(*c)); }
static int ae_dispose(sh_audio_environment_change *c,sh_audio_native_curve *curve)
{
    if(!curve->points){memset(curve,0,sizeof(*curve));return 1;}
    /* Native clear frees first, then zeroes. An exception can therefore leave a
     * stale pointer; retain an uncertain receipt rather than double-free it. */
    c->uncertain=1;
    int result=c->owner->set_curve(curve,NULL,0,0);
    if(result!=31 || curve->points || curve->count || curve->scaling)return 0;
    c->uncertain=0;return 1;
}
static int ae_cleanup(sh_audio_environment_change *c)
{
    for(unsigned i=0;i<6;i++) {
        if(c->state==AE_ACCEPTED && (c->changed&(1u<<i))) {
            if(!ae_dispose(c,c->before.curves+i))return 0;
        } else if(!(c->installed&(1u<<i)) && !ae_dispose(c,c->prepared.curves+i))return 0;
    }
    ae_forget(c);return 1;
}
static void ae_revert(sh_audio_environment_change *c)
{
    for(unsigned i=0;i<6;i++)if(c->installed&(1u<<i)) {
        c->target->curves[i]=c->before.curves[i];c->target->enabled[i]=c->before.enabled[i];
        c->installed&=~(1u<<i);
    }
    c->state=AE_RESTORED;
}
int sh_audio_environment_native_apply(sh_audio_environment_native *native,
    const sh_audio_shared_policy *policy,sh_audio_environment_change *change,char *error,size_t capacity)
{
    const sh_audio_shared_setting *slots[6]={0};volatile int held=0,result=0;
    if(error && capacity)*error=0;
    if(!native || !native->set_environment || !native->set_curve || !native->environment_slot ||
        !native->manager_slot || !native->render_lock || native->pending || !change || change->owner)
        return ae_fail(error,capacity,"audio environment adapter is unavailable or has a pending transition");
    if(!ae_policy(policy,slots))return ae_fail(error,capacity,"compiled audio environment is malformed");
    __try {
        __try {
            EnterCriticalSection(native->render_lock);held=1;
            __try {
            sh_audio_environment *target=*native->environment_slot;void *manager=*native->manager_slot;
            if(target && manager) {
                int valid=1;for(unsigned i=0;i<6;i++)if(!ae_curve_valid(target->curves+i))valid=0;
                if(valid) {
                    memset(change,0,sizeof(*change));change->owner=native;change->target=target;change->manager=manager;
                    change->thread=GetCurrentThreadId();change->before=*target;native->pending=change;
                    result=1;
                    for(unsigned i=0;i<6 && result;i++)if(slots[i]) {
                        uint16_t count;memcpy(&count,slots[i]->body+2,2);
                        change->prepared.enabled[i]=slots[i]->body[0]!=0;
                        int status=native->set_environment(&change->prepared,(int)i/3,(int)i%3,count,
                            count ? slots[i]->body+4 : NULL,slots[i]->body[1]);
                        const sh_audio_native_curve *curve=change->prepared.curves+i;
                        result=status==(count ? 1 : 31) && curve->count==count && ae_curve_valid(curve) && ae_alive(change);
                        if(result && (target->enabled[i]!=change->prepared.enabled[i] || !ae_equal(target->curves+i,curve)))
                            change->changed|=1u<<i;
                    }
                    if(result) {
                        for(unsigned i=0;i<6;i++)if(change->changed&(1u<<i)) {
                            /* Journal before the write; recovery restores the entire
                             * slot even if an exceptional write was interrupted. */
                            change->installed|=1u<<i;target->curves[i]=change->prepared.curves[i];
                            target->enabled[i]=change->prepared.enabled[i];
                        }
                        change->state=AE_APPLIED;
                    } else if(!ae_alive(change) || !ae_cleanup(change))result=-1;
                }
            }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                result=-1;
                if(change->owner) {
                    change->uncertain=1;
                    /* Do not let playback observe an exceptional partial swap.
                     * Allocation/free uncertainty remains retained separately. */
                    if(change->installed && ae_alive(change))ae_revert(change);
                }
            }
        } __finally {if(held)LeaveCriticalSection(native->render_lock);}
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(change->owner)change->uncertain=1;result=-1;
    }
    if(result<=0)ae_fail(error,capacity,"native audio environment preparation or publication failed");
    return change->owner && result<=0 ? -1 : result<0 ? 0 : result;
}
int sh_audio_environment_native_ready(sh_audio_environment_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ae_owned(change) && change->state==AE_APPLIED && !change->uncertain) {
        __try {
            __try {EnterCriticalSection(change->owner->render_lock);held=1;result=ae_matches(change);}
            __finally {if(held)LeaveCriticalSection(change->owner->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {result=0;}
    }
    return result ? 1 : ae_fail(error,capacity,"audio environment no longer belongs to the pending transition");
}
int sh_audio_environment_native_restore(sh_audio_environment_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ae_owned(change) && change->state!=AE_ACCEPTED && !change->uncertain) {
        sh_audio_environment_native *native=change->owner;
        __try {
            __try {
                EnterCriticalSection(native->render_lock);held=1;
                if(ae_matches(change)){ae_revert(change);result=ae_cleanup(change);}
            } __finally {if(held)LeaveCriticalSection(native->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {if(change->owner)change->uncertain=1;result=0;}
    }
    return result ? 1 : ae_fail(error,capacity,"audio environment recovery retains native ownership");
}
void sh_audio_environment_native_accept(sh_audio_environment_change *change)
{ if(ae_owned(change) && change->state==AE_APPLIED && !change->uncertain)change->state=AE_ACCEPTED; }
int sh_audio_environment_native_release(sh_audio_environment_change *change,char *error,size_t capacity)
{
    volatile int held=0;int result=0;if(error && capacity)*error=0;
    if(ae_owned(change) && change->state==AE_ACCEPTED && !change->uncertain) {
        sh_audio_environment_native *native=change->owner;
        __try {
            __try {
                EnterCriticalSection(native->render_lock);held=1;
                if(ae_matches(change))result=ae_cleanup(change);
            } __finally {if(held)LeaveCriticalSection(native->render_lock);}
        } __except(EXCEPTION_EXECUTE_HANDLER) {if(change->owner)change->uncertain=1;result=0;}
    }
    return result ? 1 : ae_fail(error,capacity,"audio environment retirement retains native ownership");
}
