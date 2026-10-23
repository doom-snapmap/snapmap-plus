#include "audio_shared_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct asp_row {
    sh_audio_shared_record record;
    const unsigned char *body;
    size_t scope,input,order,index;
    int baseline;
} asp_row;
typedef struct asp_compare { const sh_audio_shared_exact *exact; int failed; } asp_compare;
static int asp_fail(char *error,size_t capacity,const char *message)
{ if(error && capacity)snprintf(error,capacity,"%s",message);return 0; }
static int asp_key(const sh_audio_shared_record *a,const sh_audio_shared_record *b)
{
    if(a->kind!=b->kind)return a->kind<b->kind ? -1 : 1;
    for(size_t i=0;i<3;i++)if(a->key[i]!=b->key[i])return a->key[i]<b->key[i] ? -1 : 1;
    return 0;
}
static int asp_order(const void *left,const void *right)
{
    const asp_row *a=left,*b=right;int key=asp_key(&a->record,&b->record);if(key)return key;
    if(a->baseline!=b->baseline)return a->baseline ? -1 : 1;
    if(a->scope!=b->scope)return a->scope<b->scope ? -1 : 1;
    return a->order<b->order ? -1 : a->order>b->order;
}
static int asp_bytes(const asp_row *a,const asp_row *b)
{
    size_t length=a->record.length<b->record.length ? a->record.length : b->record.length;
    int compared=length ? memcmp(a->body,b->body,length) : 0;
    return compared ? compared : a->record.length<b->record.length ? -1 : a->record.length>b->record.length;
}
/* Byte equality is necessary but, with graph identities, not sufficient: equal
 * cooked numbers can name different scoped private definitions. */
static int asp_same(asp_compare *c,const asp_row *a,const asp_row *b)
{
    if(asp_bytes(a,b))return 0;
    if(!c->exact || (a->input==b->input && a->index==b->index))return 1;
    int equal=c->exact->equal(c->exact->context,a->input,a->index,b->input,b->index);
    if(equal<0)c->failed=1;
    return equal==1;
}
static int asp_rank(asp_compare *c,const asp_row *a,const asp_row *b)
{
    int order=asp_bytes(a,b);
    if(order || !c->exact || (a->input==b->input && a->index==b->index))return order;
    const unsigned char *x=c->exact->identity(c->exact->context,a->input,a->index);
    const unsigned char *y=c->exact->identity(c->exact->context,b->input,b->index);
    if(!x || !y){c->failed=1;return 0;}
    return memcmp(x,y,32);
}
static uint16_t asp_u16(const unsigned char *p) { return (uint16_t)(p[0]|(uint16_t)p[1]<<8); }
static int asp_effective(const asp_row *row)
{
    if(row->record.kind==SH_AUDIO_SHARED_VOICES)return asp_u16(row->body)!=0;
    if(row->record.kind==SH_AUDIO_SHARED_THRESHOLD) {
        float value;memcpy(&value,row->body,4);return value>=-96.3f && value<=0;
    }
    return 1;
}
static int asp_constraint(const asp_row *candidate,const asp_row *winner)
{
    if(candidate->record.kind==SH_AUDIO_SHARED_VOICES)
        return asp_u16(candidate->body)>asp_u16(winner->body);
    float a,b;memcpy(&a,candidate->body,4);memcpy(&b,winner->body,4);
    int valid_a=a>=-96.3f && a<=0,valid_b=b>=-96.3f && b<=0;
    return valid_a && (!valid_b || a<b);
}
void sh_audio_shared_policy_free(sh_audio_shared_policy *policy)
{
    if(!policy)return;
    for(size_t i=0;i<policy->count;i++){free(policy->settings[i].body);free(policy->settings[i].contributors);}
    free(policy->settings);memset(policy,0,sizeof(*policy));
}
int sh_audio_shared_policy_copy(const sh_audio_shared_policy *policy,sh_audio_shared_policy *out)
{
    sh_audio_shared_policy copy={0};
    if(!out)return 0;memset(out,0,sizeof(*out));
    if(!policy || (policy->count && !policy->settings) || policy->count>SIZE_MAX/sizeof(*copy.settings))return 0;
    copy.settings=calloc(policy->count ? policy->count : 1,sizeof(*copy.settings));if(!copy.settings)return 0;
    for(size_t i=0;i<policy->count;i++) {
        const sh_audio_shared_setting *from=policy->settings+i;sh_audio_shared_setting *to=copy.settings+copy.count++;
        *to=*from;to->body=NULL;to->contributors=NULL;to->contributor_count=0;
        to->body=malloc(from->record.length ? from->record.length : 1);if(!to->body)goto failed;
        if(from->record.length)memcpy(to->body,from->body,from->record.length);
        if(from->contributor_count) {
            if(from->contributor_count>SIZE_MAX/sizeof(*to->contributors))goto failed;
            to->contributors=malloc(from->contributor_count*sizeof(*to->contributors));if(!to->contributors)goto failed;
            memcpy(to->contributors,from->contributors,from->contributor_count*sizeof(*to->contributors));
            to->contributor_count=from->contributor_count;
        }
    }
    *out=copy;return 1;
failed:
    sh_audio_shared_policy_free(&copy);return 0;
}
int sh_audio_shared_policy_compile_exact(const sh_audio_shared_input *inputs,size_t count,
    const sh_audio_shared_exact *exact,sh_audio_shared_policy *out,char *error,size_t capacity)
{
    asp_row *rows=NULL;size_t used=0,allocated=0;sh_audio_shared_policy policy={0};int ok=0;
    asp_compare compare={exact,0};
    if(error && capacity)*error=0;if(!out)return 0;memset(out,0,sizeof(*out));
    if(count && !inputs)return asp_fail(error,capacity,"shared audio inputs are missing");
    if(exact && (!exact->equal || !exact->identity))return asp_fail(error,capacity,"shared audio identities are incomplete");
    for(size_t i=0;i<count;i++) {
        sh_audio_hirc parsed={0};const sh_audio_shared_input *input=inputs+i;
        if(sh_audio_shared_chunk_read(input->tag,input->body,input->length,&parsed,error,capacity)!=1)goto done;
        if(parsed.shared_count>SIZE_MAX-used || used+parsed.shared_count>SIZE_MAX/sizeof(*rows)) {
            sh_audio_hirc_free(&parsed);goto memory;
        }
        size_t need=used+parsed.shared_count;
        if(need>allocated) {
            size_t next=allocated ? allocated : 32;
            while(next<need) { if(next>SIZE_MAX/sizeof(*rows)/2){next=need;break;}next*=2; }
            asp_row *grown=realloc(rows,next*sizeof(*rows));
            if(!grown){sh_audio_hirc_free(&parsed);goto memory;}rows=grown;allocated=next;
        }
        for(size_t j=0;j<parsed.shared_count;j++) {
            sh_audio_shared_record record=parsed.shared[j];
            rows[used]=(asp_row){record,(const unsigned char *)input->body+record.offset,
                input->baseline ? 0 : input->scope,i,used,j,!!input->baseline};used++;
        }
        sh_audio_hirc_free(&parsed);
    }
    if(used>1)qsort(rows,used,sizeof(*rows),asp_order);
    /* PLAT is a compatibility check on every load, not a last-write setting.
     * Validate before collapsing a scope so an earlier incompatible bank cannot
     * be hidden by a later valid label from the same package. */
    const asp_row *platform=NULL;
    for(size_t i=0;i<used;i++)if(rows[i].record.kind==SH_AUDIO_SHARED_PLATFORM) {
        if(platform && asp_bytes(platform,rows+i)) {
            asp_fail(error,capacity,"audio bank platform label differs from the target");goto done;
        }
        platform=rows+i;
    }
    /* Native repeated definitions in one scope are ordered; unrelated peer
     * scopes are not an ordered overlay. Collapse those cases separately. */
    size_t unique=0;
    for(size_t i=0;i<used;) {
        size_t end=i+1;
        while(end<used && !asp_key(&rows[i].record,&rows[end].record) &&
            rows[i].scope==rows[end].scope && rows[i].baseline==rows[end].baseline)end++;
        size_t last=i;
        for(size_t j=i+1;j<end;j++)if(asp_effective(rows+j) || !asp_effective(rows+last))last=j;
        rows[unique++]=rows[last];i=end;
    }
    if(unique>SIZE_MAX/sizeof(*policy.settings))goto memory;
    policy.settings=calloc(unique ? unique : 1,sizeof(*policy.settings));if(!policy.settings)goto memory;
    for(size_t i=0;i<unique;) {
        size_t end=i+1;while(end<unique && !asp_key(&rows[i].record,&rows[end].record))end++;
        const asp_row *baseline=rows[i].baseline ? rows+i : NULL,*winner=rows+i;
        int changed=0;sh_audio_shared_kind kind=winner->record.kind;
        if(kind==SH_AUDIO_SHARED_THRESHOLD || kind==SH_AUDIO_SHARED_VOICES) {
            for(size_t j=i+1;j<end;j++)if(asp_constraint(rows+j,winner) ||
                (!asp_constraint(winner,rows+j) && asp_rank(&compare,rows+j,winner)<0))winner=rows+j;
            changed=!winner->baseline && (!baseline || !asp_same(&compare,winner,baseline));
        } else {
            for(size_t j=i;j<end;j++) {
                const asp_row *candidate=rows+j;
                int edited=!candidate->baseline && (!baseline || !asp_same(&compare,candidate,baseline));
                if(edited>changed || (edited==changed && asp_rank(&compare,candidate,winner)<0)) {
                    winner=candidate;changed=edited;
                }
            }
        }
        sh_audio_shared_setting *setting=policy.settings+policy.count++;
        setting->record=winner->record;setting->record.offset=0;
        setting->input=winner->input;setting->index=winner->index;setting->alternatives=end-i;
        setting->edited=changed;
        setting->body=malloc(winner->record.length ? winner->record.length : 1);
        setting->contributors=malloc((end-i)*sizeof(*setting->contributors));
        if(!setting->body || !setting->contributors)goto memory;
        memcpy(setting->body,winner->body,winner->record.length);
        for(size_t j=i;j<end;j++)if(asp_same(&compare,rows+j,winner))
            setting->contributors[setting->contributor_count++]=(sh_audio_shared_contributor){rows[j].input,rows[j].index};
        if(compare.failed){asp_fail(error,capacity,"cannot compare retained shared audio recipes");goto done;}
        i=end;
    }
    *out=policy;memset(&policy,0,sizeof(policy));ok=1;goto done;
memory: asp_fail(error,capacity,"cannot allocate shared audio policy");
done: free(rows);sh_audio_shared_policy_free(&policy);return ok;
}
int sh_audio_shared_policy_compile(const sh_audio_shared_input *inputs,size_t count,
    sh_audio_shared_policy *out,char *error,size_t capacity)
{ return sh_audio_shared_policy_compile_exact(inputs,count,NULL,out,error,capacity); }

typedef struct asp_bytes_out { unsigned char *body;size_t length,allocated; } asp_bytes_out;
static int asp_emit(asp_bytes_out *out,const void *bytes,size_t length)
{
    if(length>SIZE_MAX-out->length)return 0;
    size_t need=out->length+length;
    if(need>out->allocated) {
        size_t next=out->allocated ? out->allocated : 128;
        while(next<need){if(next>SIZE_MAX/2){next=need;break;}next*=2;}
        unsigned char *grown=realloc(out->body,next);if(!grown)return 0;out->body=grown;out->allocated=next;
    }
    if(length)memcpy(out->body+out->length,bytes,length);out->length+=length;return 1;
}
static int asp_word(asp_bytes_out *out,size_t value)
{
    if(value>UINT32_MAX)return 0;
    unsigned char bytes[4];for(unsigned i=0;i<4;i++)bytes[i]=(unsigned char)(value>>(i*8));
    return asp_emit(out,bytes,sizeof(bytes));
}
static size_t asp_lower(const sh_audio_shared_policy *p,sh_audio_shared_kind kind,uint32_t key)
{
    sh_audio_shared_record wanted={kind,{key,0,0},0,0};size_t first=0,last=p->count;
    while(first<last) {size_t middle=first+(last-first)/2;
        if(asp_key(&p->settings[middle].record,&wanted)<0)first=middle+1;else last=middle;}
    return first;
}
static const sh_audio_shared_setting *asp_find(const sh_audio_shared_policy *p,sh_audio_shared_kind kind,uint32_t key)
{
    size_t i=asp_lower(p,kind,key);
    return i<p->count && p->settings[i].record.kind==kind && p->settings[i].record.key[0]==key ? p->settings+i : NULL;
}
static size_t asp_count(const sh_audio_shared_policy *p,sh_audio_shared_kind kind)
{ size_t count=0;for(size_t i=0;i<p->count;i++)count+=p->settings[i].record.kind==kind;return count; }
static int asp_setting(asp_bytes_out *out,const sh_audio_shared_setting *s)
{ return s && asp_emit(out,s->body,s->record.length); }
int sh_audio_shared_policy_chunk(const sh_audio_shared_policy *p,const char *tag,
    unsigned char **body,size_t *length,char *error,size_t capacity)
{
    asp_bytes_out out={0};sh_audio_hirc check={0};
    if(body)*body=NULL;if(length)*length=0;if(error && capacity)*error=0;
    if(!p || !tag || !body || !length)return asp_fail(error,capacity,"shared audio chunk output is missing");
    if(!strcmp(tag,"STMG")) {
        if(!asp_count(p,SH_AUDIO_SHARED_THRESHOLD))return 1;
        if(!asp_setting(&out,asp_find(p,SH_AUDIO_SHARED_THRESHOLD,0)) ||
            !asp_setting(&out,asp_find(p,SH_AUDIO_SHARED_VOICES,0)) || !asp_word(&out,asp_count(p,SH_AUDIO_SHARED_GROUP)))goto failed;
        for(size_t i=0;i<p->count;i++)if(p->settings[i].record.kind==SH_AUDIO_SHARED_GROUP) {
            uint32_t group=p->settings[i].record.key[0];
            size_t first=asp_lower(p,SH_AUDIO_SHARED_TRANSITION,group),end=first;
            while(end<p->count && p->settings[end].record.kind==SH_AUDIO_SHARED_TRANSITION &&
                p->settings[end].record.key[0]==group)end++;
            if(!asp_setting(&out,p->settings+i) || !asp_word(&out,end-first))goto failed;
            for(size_t j=first;j<end;j++)if(!asp_setting(&out,p->settings+j))goto failed;
        }
        for(unsigned kind=SH_AUDIO_SHARED_SWITCH;kind<=SH_AUDIO_SHARED_RTPC;kind++) {
            if(!asp_word(&out,asp_count(p,(sh_audio_shared_kind)kind)))goto failed;
            for(size_t i=0;i<p->count;i++)if((unsigned)p->settings[i].record.kind==kind) {
                if(!asp_setting(&out,p->settings+i))goto failed;
                if(kind==SH_AUDIO_SHARED_RTPC) {
                    const sh_audio_shared_setting *binding=asp_find(p,SH_AUDIO_SHARED_BUILTIN,p->settings[i].record.key[0]);
                    unsigned char none=0;
                    if(!asp_emit(&out,binding ? binding->body : &none,1))goto failed;
                }
            }
        }
    } else if(!strcmp(tag,"ENVS")) {
        if(!asp_count(p,SH_AUDIO_SHARED_ENVIRONMENT))return 1;
        for(uint32_t i=0;i<6;i++)if(!asp_setting(&out,asp_find(p,SH_AUDIO_SHARED_ENVIRONMENT,i)))goto failed;
    } else if(!strcmp(tag,"PLAT")) {
        const sh_audio_shared_setting *platform=asp_find(p,SH_AUDIO_SHARED_PLATFORM,0);
        if(!platform)return 1;if(!asp_setting(&out,platform))goto failed;
    } else goto failed;
    if(sh_audio_shared_chunk_read(tag,out.body,out.length,&check,error,capacity)!=1)goto failed;
    sh_audio_hirc_free(&check);*body=out.body;*length=out.length;return 1;
failed:
    free(out.body);sh_audio_hirc_free(&check);
    if(!error || !capacity || !*error)asp_fail(error,capacity,"cannot assemble shared audio policy chunk");return 0;
}
