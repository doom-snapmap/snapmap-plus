#include "audio_limits_native.h"
#include "audio_native_image.h"
#include <string.h>
#include <stdio.h>

enum { AL_THRESHOLD=1, AL_VOICES=2 };
static int al_fail(char *error,size_t capacity,const char *message)
{ if(error && capacity)snprintf(error,capacity,"%s",message);return 0; }
int sh_audio_limits_native_bind(sh_audio_limits_native *native,const unsigned char *image,
    const sig_result *results,size_t count)
{
    const char *names[]={"AudioThresholdSet","AudioVoiceLimitSet","AudioRenderTick","AudioBankLoad"};
    const unsigned char *entry[4]={0};sh_audio_limits_native bound={0};
    if(!native || native->pending)return 0;memset(native,0,sizeof(*native));
    if(count && !results)return 0;
    for(size_t i=0;i<count;i++)for(size_t j=0;j<4;j++)if(results[i].name && !strcmp(results[i].name,names[j])) {
        if(entry[j] || results[i].status!=SIG_OK)return 0;entry[j]=(const unsigned char *)results[i].addr;
    }
    const size_t spans[]={0xe4,0x26,0x29,0x25};
    for(size_t i=0;i<4;i++)if(!ani_section(image,entry[i],spans[i],IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE,IMAGE_SCN_MEM_WRITE))return 0;
    __try {
        static const unsigned char cmp[]={0x3b,0x15},store[]={0x89,0x15},voice[]={0x66,0x89,0x0d};
        static const unsigned char threshold[]={0xf3,0x0f,0x11,0x3d},amplitude[]={0xf3,0x0f,0x11,0x35};
        static const unsigned char lea[]={0x48,0x8d,0x0d},slot[]={0x48,0x8b,0x0d};
        bound.threshold_priority=ani_rip(entry[0]+0x2a,cmp,sizeof(cmp));
        bound.voices_priority=ani_rip(entry[1]+0xb,cmp,sizeof(cmp));
        if(!bound.threshold_priority || !bound.voices_priority ||
            bound.threshold_priority!=ani_rip(entry[0]+0x49,store,sizeof(store)) ||
            bound.voices_priority!=ani_rip(entry[1]+0x13,store,sizeof(store)))return 0;
        bound.threshold=ani_rip(entry[0]+0xd4,threshold,sizeof(threshold));
        bound.amplitude=ani_rip(entry[0]+0xdc,amplitude,sizeof(amplitude));
        bound.voices=ani_rip(entry[1]+0x19,voice,sizeof(voice));
        bound.render_lock=ani_rip(entry[2]+0x1c,lea,sizeof(lea));
        bound.manager_slot=ani_rip(entry[3]+0x1e,slot,sizeof(slot));
        if((uintptr_t)bound.threshold!=(uintptr_t)bound.amplitude+4 ||
            (uintptr_t)bound.voices_priority!=(uintptr_t)bound.threshold_priority+4)return 0;
        const void *data[]={bound.threshold,bound.amplitude,bound.threshold_priority,
            bound.voices_priority,bound.voices,bound.render_lock,bound.manager_slot};
        const size_t sizes[]={4,4,4,4,2,sizeof(CRITICAL_SECTION),sizeof(void *)};
        for(size_t i=0;i<7;i++) {
            size_t alignment=sizes[i]>=8 ? 8 : sizes[i];
            if(((uintptr_t)data[i]&(alignment-1)) || !ani_section(image,data[i],sizes[i],
                IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_WRITE,IMAGE_SCN_MEM_EXECUTE))return 0;
            for(size_t j=0;j<i;j++)if((uintptr_t)data[i]<(uintptr_t)data[j]+sizes[j] &&
                (uintptr_t)data[j]<(uintptr_t)data[i]+sizes[i])return 0;
        }
        bound.set_threshold=(int (*)(float,int))entry[0];
        bound.set_voices=(int (*)(uint16_t,int))entry[1];
        *native=bound;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static sh_audio_limits_value al_read(const sh_audio_limits_native *native)
{
    sh_audio_limits_value value={0};
    value.threshold=*native->threshold;value.amplitude=*native->amplitude;
    value.threshold_priority=*native->threshold_priority;value.voices_priority=*native->voices_priority;
    value.voices=*native->voices;return value;
}
static int al_equal(const sh_audio_limits_value *a,const sh_audio_limits_value *b,unsigned fields)
{
    return (!(fields&AL_THRESHOLD) || (!memcmp(&a->threshold,&b->threshold,4) &&
        !memcmp(&a->amplitude,&b->amplitude,4) && a->threshold_priority==b->threshold_priority)) &&
        (!(fields&AL_VOICES) || (a->voices==b->voices && a->voices_priority==b->voices_priority));
}
static void al_write(sh_audio_limits_native *native,const sh_audio_limits_value *value,unsigned fields)
{
    /* These setters own only scalar globals. No object, allocation, subscription
     * or notification is bypassed. The render lock covers the entire snapshot. */
    if(fields&AL_THRESHOLD) {
        *native->threshold=value->threshold;*native->amplitude=value->amplitude;
        *native->threshold_priority=value->threshold_priority;
    }
    if(fields&AL_VOICES) {*native->voices=value->voices;*native->voices_priority=value->voices_priority;}
}
static int al_bound(const sh_audio_limits_native *native)
{
    return native && native->set_threshold && native->set_voices && native->threshold && native->amplitude &&
        native->threshold_priority && native->voices_priority && native->voices && native->render_lock && native->manager_slot;
}
static int al_policy(const sh_audio_shared_policy *policy,sh_audio_limits_value *value,unsigned *fields)
{
    unsigned seen=0;*fields=0;memset(value,0,sizeof(*value));
    if(!policy || (policy->count && !policy->settings))return 0;
    for(size_t i=0;i<policy->count;i++) {
        const sh_audio_shared_setting *setting=policy->settings+i;
        unsigned bit=setting->record.kind==SH_AUDIO_SHARED_THRESHOLD ? AL_THRESHOLD :
            setting->record.kind==SH_AUDIO_SHARED_VOICES ? AL_VOICES : 0;
        if(!bit)continue;
        if((seen&bit) || !setting->body || setting->record.offset ||
            setting->record.length!=(bit==AL_THRESHOLD ? 4 : 2) ||
            setting->record.key[0] || setting->record.key[1] || setting->record.key[2])return 0;
        seen|=bit;
        if(bit==AL_THRESHOLD) {
            memcpy(&value->threshold,setting->body,4);
            if(value->threshold>=-96.3f && value->threshold<=0)*fields|=bit;
        } else {memcpy(&value->voices,setting->body,2);if(value->voices)*fields|=bit;}
    }
    return 1;
}
static void al_forget(sh_audio_limits_change *change)
{
    change->owner->pending=NULL;memset(change,0,sizeof(*change));
}
int sh_audio_limits_native_apply(sh_audio_limits_native *native,const sh_audio_shared_policy *policy,
    sh_audio_limits_change *change,char *error,size_t capacity)
{
    sh_audio_limits_value wanted={0};unsigned fields=0;volatile int held=0,result=0,entered=0;
    if(error && capacity)*error=0;
    if(!al_bound(native) || !change || change->owner || native->pending)
        return al_fail(error,capacity,"audio playback settings adapter is unavailable or already has a pending change");
    if(!al_policy(policy,&wanted,&fields))return al_fail(error,capacity,"compiled audio playback settings are malformed");
    __try {
        __try {
            EnterCriticalSection(native->render_lock);held=1;
            __try {
            if(*native->manager_slot) {
                sh_audio_limits_value before=al_read(native);
                if(before.threshold>=-96.3f && before.threshold<=0 && before.amplitude>0 && before.amplitude<=1 && before.voices) {
                    *change=(sh_audio_limits_change){native,before,before,GetCurrentThreadId(),0,0};
                    native->pending=change;entered=1;result=1;
                    if((fields&AL_THRESHOLD) && memcmp(&before.threshold,&wanted.threshold,4)) {
                        change->changed|=AL_THRESHOLD;
                        result=native->set_threshold(wanted.threshold,before.threshold_priority)==1;
                        sh_audio_limits_value after=al_read(native);change->after=after;
                        result=result && !memcmp(&after.threshold,&wanted.threshold,4) && after.threshold_priority==before.threshold_priority &&
                            after.amplitude>0 && after.amplitude<=1 && al_equal(&before,&after,AL_VOICES);
                    }
                    if(result && (fields&AL_VOICES) && before.voices!=wanted.voices) {
                        change->changed|=AL_VOICES;
                        sh_audio_limits_value previous=change->after;
                        result=native->set_voices(wanted.voices,before.voices_priority)==1;
                        sh_audio_limits_value after=al_read(native);change->after=after;
                        result=result && after.voices==wanted.voices && after.voices_priority==before.voices_priority &&
                            al_equal(&previous,&after,AL_THRESHOLD);
                    }
                    if(!result) {
                        al_write(native,&before,AL_THRESHOLD|AL_VOICES);
                        al_forget(change);entered=0;
                    }
                }
            }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                result=0;
                if(entered) {
                    change->uncertain=1;
                    if(*native->manager_slot) {
                        /* Recover before releasing the render lock, so playback
                         * cannot observe a partially failed scalar update. */
                        al_write(native,&change->before,change->changed);
                        al_forget(change);entered=0;
                    }
                }
            }
        } __finally {if(held)LeaveCriticalSection(native->render_lock);}
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result=0;
    }
    if(!result)al_fail(error,capacity,"native audio playback settings could not be applied");
    /* The enclosing activation retains failed recovery. Both scalar setters
     * are synchronous; no native callback borrows this receipt. */
    if(!result && entered){change->uncertain=1;return -1;}
    return result;
}
static int al_check(sh_audio_limits_change *change,int restore,char *error,size_t capacity)
{
    volatile int held=0;int result=0;
    if(error && capacity)*error=0;
    if(!change || !change->owner || change->owner->pending!=change || change->thread!=GetCurrentThreadId())
        return al_fail(error,capacity,"audio playback settings receipt does not belong to this transition");
    sh_audio_limits_native *native=change->owner;
    __try {
        __try {
            EnterCriticalSection(native->render_lock);held=1;
            if(*native->manager_slot) {
                sh_audio_limits_value current=al_read(native);
                if((restore && change->uncertain) || (!change->uncertain && al_equal(&current,&change->after,change->changed))) {
                    if(restore)al_write(native,&change->before,change->changed);
                    result=1;
                }
            }
        } __finally {if(held)LeaveCriticalSection(native->render_lock);}
    } __except(EXCEPTION_EXECUTE_HANDLER) {result=0;}
    if(result && restore)al_forget(change);
    return result ? 1 : al_fail(error,capacity,"audio playback settings changed outside the pending transition or are unavailable");
}
int sh_audio_limits_native_restore(sh_audio_limits_change *change,char *error,size_t capacity)
{ return al_check(change,1,error,capacity); }
int sh_audio_limits_native_ready(sh_audio_limits_change *change,char *error,size_t capacity)
{ return al_check(change,0,error,capacity); }
void sh_audio_limits_native_accept(sh_audio_limits_change *change)
{
    if(change && change->owner && change->owner->pending==change && change->thread==GetCurrentThreadId() && !change->uncertain)
        al_forget(change);
}
