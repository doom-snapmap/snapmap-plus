#include "audio_effective_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ae_row { const sh_audio_shared_setting *setting; size_t source; } ae_row;
static int ae_fail(char *error,size_t capacity,const char *message)
{ if(error && capacity)snprintf(error,capacity,"%s",message);return 0; }
static int ae_key(const sh_audio_shared_record *a,const sh_audio_shared_record *b)
{
    if(a->kind!=b->kind)return a->kind<b->kind ? -1 : 1;
    for(size_t i=0;i<3;i++)if(a->key[i]!=b->key[i])return a->key[i]<b->key[i] ? -1 : 1;
    return 0;
}
static int ae_order(const void *left,const void *right)
{
    const ae_row *a=left,*b=right;int key=ae_key(&a->setting->record,&b->setting->record);
    return key ? key : a->source<b->source ? -1 : a->source>b->source;
}
static int ae_equal(const sh_audio_shared_setting *a,const sh_audio_shared_setting *b)
{ return a->record.length==b->record.length && (!a->record.length || !memcmp(a->body,b->body,a->record.length)); }
static int ae_threshold(const sh_audio_shared_setting *s,float *value)
{
    if(s->record.length<4)return 0;
    memcpy(value,s->body,4);return *value>=-96.3f && *value<=0;
}
/* The same compatibility rule as one program: more simultaneous voices and a
 * lower audibility threshold enable every contributor's sounds. */
static int ae_better(const sh_audio_shared_setting *candidate,const sh_audio_shared_setting *winner)
{
    if(candidate->record.kind==SH_AUDIO_SHARED_VOICES) {
        if(candidate->record.length<2 || winner->record.length<2)return 0;
        unsigned a=candidate->body[0]|(unsigned)candidate->body[1]<<8,b=winner->body[0]|(unsigned)winner->body[1]<<8;
        return a>b;
    }
    float a,b;int valid_a=ae_threshold(candidate,&a),valid_b=ae_threshold(winner,&b);
    return valid_a && (!valid_b || a<b);
}
int sh_audio_effective_shared_edited(const sh_audio_shared_policy *policy)
{
    if(!policy)return 0;
    for(size_t i=0;i<policy->count;i++)if(policy->settings[i].edited)return 1;
    return 0;
}
int sh_audio_effective_shared(const sh_audio_shared_policy *baseline,
    const sh_audio_shared_policy *const *programs,size_t count,
    sh_audio_shared_policy *out,char *error,size_t capacity)
{
    ae_row *rows=NULL;size_t used=0,total=baseline ? baseline->count : 0;sh_audio_shared_policy policy={0};int ok=0;
    if(error && capacity)*error=0;
    if(!out)return 0;memset(out,0,sizeof(*out));
    if((count && !programs) || (baseline && baseline->count && !baseline->settings))
        return ae_fail(error,capacity,"shared audio program inputs are missing");
    for(size_t i=0;i<count;i++) {
        if(!programs[i] || (programs[i]->count && !programs[i]->settings) || programs[i]->count>SIZE_MAX-total)
            return ae_fail(error,capacity,"shared audio program inputs are missing");
        total+=programs[i]->count;
    }
    if(total>SIZE_MAX/sizeof(*rows) || total>SIZE_MAX/sizeof(*policy.settings))goto memory;
    rows=malloc((total ? total : 1)*sizeof(*rows));if(!rows)goto memory;
    for(size_t i=0;baseline && i<baseline->count;i++)rows[used++]=(ae_row){baseline->settings+i,0};
    for(size_t p=0;p<count;p++)for(size_t i=0;i<programs[p]->count;i++)rows[used++]=(ae_row){programs[p]->settings+i,p+1};
    if(used>1)qsort(rows,used,sizeof(*rows),ae_order);
    policy.settings=calloc(total ? total : 1,sizeof(*policy.settings));if(!policy.settings)goto memory;
    for(size_t i=0;i<used;) {
        size_t end=i+1;while(end<used && !ae_key(&rows[i].setting->record,&rows[end].setting->record))end++;
        const ae_row *base=rows[i].source ? NULL : rows+i,*winner=NULL;
        sh_audio_shared_kind kind=rows[i].setting->record.kind;
        if(kind==SH_AUDIO_SHARED_PLATFORM) {
            for(size_t j=i+1;j<end;j++)if(!ae_equal(rows[j].setting,rows[i].setting)) {
                ae_fail(error,capacity,"active audio programs disagree with the native platform label");goto done;
            }
            winner=rows+i;
        } else if(kind==SH_AUDIO_SHARED_THRESHOLD || kind==SH_AUDIO_SHARED_VOICES) {
            winner=rows+i;
            for(size_t j=i+1;j<end;j++)if(ae_better(rows[j].setting,winner->setting))winner=rows+j;
        } else {
            for(size_t j=i;j<end && !winner;j++)if(rows[j].source && rows[j].setting->edited)winner=rows+j;
            if(!winner)winner=base ? base : rows+i;
        }
        sh_audio_shared_setting *setting=policy.settings+policy.count++;
        setting->record=winner->setting->record;setting->record.offset=0;
        setting->input=winner->source;setting->index=winner->setting->index;setting->alternatives=end-i;
        setting->edited=base ? !ae_equal(winner->setting,base->setting) : 1;
        setting->body=malloc(setting->record.length ? setting->record.length : 1);if(!setting->body)goto memory;
        if(setting->record.length)memcpy(setting->body,winner->setting->body,setting->record.length);
        i=end;
    }
    *out=policy;memset(&policy,0,sizeof(policy));ok=1;goto done;
memory: ae_fail(error,capacity,"cannot allocate effective shared audio policy");
done:
    free(rows);sh_audio_shared_policy_free(&policy);return ok;
}
