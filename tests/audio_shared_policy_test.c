#include "../src/backend/audio_shared_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bytes { unsigned char data[65536];size_t length; } bytes;
static void emit(bytes *b,const void *p,size_t n)
{ assert(n<=sizeof(b->data)-b->length);memcpy(b->data+b->length,p,n);b->length+=n; }
static void put(bytes *b,uint32_t value,unsigned n)
{ for(unsigned i=0;i<n;i++){unsigned char v=(unsigned char)(value>>(8*i));emit(b,&v,1);} }
static uint32_t word(const unsigned char *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void header(bytes *b,float threshold,uint16_t voices,uint32_t groups)
{ emit(b,&threshold,4);put(b,voices,2);put(b,groups,4); }
static void group(bytes *b,uint32_t id,uint32_t duration,uint32_t pairs)
{ put(b,id,4);put(b,duration,4);put(b,pairs,4); }
static void pair(bytes *b,uint32_t from,uint32_t to,uint32_t duration)
{ put(b,from,4);put(b,to,4);put(b,duration,4); }
static void control(bytes *b,uint32_t id,uint32_t rtpc,int nonempty)
{
    put(b,id,4);put(b,rtpc,4);put(b,0,1);put(b,nonempty ? 1 : 0,4);
    if(nonempty){put(b,0,4);put(b,0,4);put(b,0,4);}
}
static void rtpc(bytes *b,uint32_t id,uint32_t value,unsigned selector)
{ put(b,id,4);put(b,value,4);put(b,0,4);put(b,0,4);put(b,0,4);put(b,selector,1); }
static sh_audio_shared_input input(const char *tag,bytes *body,size_t scope,int baseline)
{ return (sh_audio_shared_input){tag,body->data,body->length,scope,baseline}; }
static sh_audio_shared_policy compile(const sh_audio_shared_input *inputs,size_t count)
{
    sh_audio_shared_policy p={0};char error[512];
    if(!sh_audio_shared_policy_compile(inputs,count,&p,error,sizeof(error))){fprintf(stderr,"%s\n",error);assert(0);}
    return p;
}
static bytes chunk(const sh_audio_shared_policy *p,const char *tag)
{
    unsigned char *body=NULL;size_t length=0;char error[512];bytes b={0};
    assert(sh_audio_shared_policy_chunk(p,tag,&body,&length,error,sizeof(error)));
    if(length)emit(&b,body,length);free(body);return b;
}
static const sh_audio_shared_setting *setting(const sh_audio_shared_policy *p,
    sh_audio_shared_kind kind,uint32_t a,uint32_t b,uint32_t c)
{
    for(size_t i=0;i<p->count;i++){const sh_audio_shared_setting *s=p->settings+i;
        if(s->record.kind==kind && s->record.key[0]==a && s->record.key[1]==b && s->record.key[2]==c)return s;}
    return NULL;
}
static uint32_t value(const sh_audio_shared_policy *p,sh_audio_shared_kind kind,uint32_t a,uint32_t b,uint32_t c,size_t offset)
{
    const sh_audio_shared_setting *s=setting(p,kind,a,b,c);assert(s && offset+4<=s->record.length);return word(s->body+offset);
}
static void equivalent(const bytes *a,const bytes *b)
{ assert(a->length==b->length && !memcmp(a->data,b->data,a->length)); }
static void roundtrip(const sh_audio_shared_policy *p,const char *tag)
{
    bytes a=chunk(p,tag);sh_audio_shared_input in=input(tag,&a,0,0);
    sh_audio_shared_policy again=compile(&in,1);bytes b=chunk(&again,tag);equivalent(&a,&b);
    sh_audio_shared_policy_free(&again);
}
static void composition(void)
{
    bytes base={0},first={0},second={0};
    header(&base,-60,64,1);group(&base,10,100,1);pair(&base,1,2,100);
    put(&base,1,4);control(&base,20,30,1);put(&base,1,4);rtpc(&base,30,100,1);
    header(&first,-70,32,2);group(&first,10,80,1);pair(&first,1,2,90);group(&first,11,120,0);
    put(&first,1,4);control(&first,20,31,1);put(&first,1,4);rtpc(&first,30,80,2);
    header(&second,-50,128,2);group(&second,10,100,2);pair(&second,1,2,100);pair(&second,1,3,40);
    group(&second,12,120,0);put(&second,1,4);control(&second,20,30,1);put(&second,1,4);rtpc(&second,30,100,1);
    sh_audio_shared_input in[]={input("STMG",&base,0,1),input("STMG",&first,1,0),
        input("STMG",&second,2,0),input("STMG",&base,3,0),input("STMG",&first,4,0)};
    sh_audio_shared_policy p=compile(in,5);bytes expected=chunk(&p,"STMG");
    float threshold;memcpy(&threshold,expected.data,4);assert(threshold==-70 && expected.data[4]==128 && expected.data[5]==0);
    assert(value(&p,SH_AUDIO_SHARED_GROUP,10,0,0,4)==80);
    assert(value(&p,SH_AUDIO_SHARED_TRANSITION,10,1,2,8)==90);
    assert(value(&p,SH_AUDIO_SHARED_TRANSITION,10,1,3,8)==40);
    assert(setting(&p,SH_AUDIO_SHARED_GROUP,11,0,0) && setting(&p,SH_AUDIO_SHARED_GROUP,12,0,0));
    assert(value(&p,SH_AUDIO_SHARED_SWITCH,20,0,0,4)==31);
    assert(value(&p,SH_AUDIO_SHARED_RTPC,30,0,0,4)==80);
    assert(setting(&p,SH_AUDIO_SHARED_BUILTIN,30,0,0)->body[0]==2);
    roundtrip(&p,"STMG");
    /* Package enumeration and redundant copies cannot change the result. */
    for(size_t a=0;a<5;a++)for(size_t b=0;b<5;b++) {
        sh_audio_shared_input perm[5];memcpy(perm,in,sizeof(in));
        sh_audio_shared_input swap=perm[a];perm[a]=perm[b];perm[b]=swap;
        sh_audio_shared_policy q=compile(perm,5);bytes actual=chunk(&q,"STMG");
        equivalent(&expected,&actual);assert(q.count==p.count);sh_audio_shared_policy_free(&q);
    }
    sh_audio_shared_policy q=compile(in,3);bytes fewer=chunk(&q,"STMG");equivalent(&expected,&fewer);
    sh_audio_shared_policy_free(&q);sh_audio_shared_policy_free(&p);

    /* Two actual edits of one scalar have one deterministic winner; both
     * packages' different keys remain. This is not an overlap error. */
    second.data[14]=70;in[2]=input("STMG",&second,2,0);p=compile(in,3);
    assert(value(&p,SH_AUDIO_SHARED_GROUP,10,0,0,4)==70);
    assert(setting(&p,SH_AUDIO_SHARED_GROUP,11,0,0) && setting(&p,SH_AUDIO_SHARED_GROUP,12,0,0));
    sh_audio_shared_policy_free(&p);
}
static void ordered_noops(void)
{
    bytes first={0},later={0};header(&first,-65,100,1);group(&first,10,100,1);pair(&first,1,2,90);
    put(&first,1,4);control(&first,20,30,1);put(&first,1,4);rtpc(&first,30,100,3);
    header(&later,1,0,1);group(&later,10,120,0);put(&later,1,4);control(&later,20,999,0);
    put(&later,1,4);rtpc(&later,30,200,0);
    sh_audio_shared_input in[]={input("STMG",&first,7,0),input("STMG",&later,7,0)};
    sh_audio_shared_policy p=compile(in,2);bytes out=chunk(&p,"STMG");float threshold;
    memcpy(&threshold,out.data,4);assert(threshold==-65 && out.data[4]==100);
    assert(value(&p,SH_AUDIO_SHARED_GROUP,10,0,0,4)==120);
    assert(value(&p,SH_AUDIO_SHARED_TRANSITION,10,1,2,8)==90);
    assert(value(&p,SH_AUDIO_SHARED_SWITCH,20,0,0,4)==30);
    assert(value(&p,SH_AUDIO_SHARED_RTPC,30,0,0,4)==200);
    assert(setting(&p,SH_AUDIO_SHARED_BUILTIN,30,0,0)->body[0]==3);
    roundtrip(&p,"STMG");sh_audio_shared_policy_free(&p);
    /* Invalid header scalars are native no-ops even if they occur last. */
    uint32_t nan=0x7fc00000;memcpy(later.data,&nan,4);p=compile(in,2);out=chunk(&p,"STMG");
    memcpy(&threshold,out.data,4);assert(threshold==-65);sh_audio_shared_policy_free(&p);
    /* Repetition within one chunk follows exactly the same registration rule. */
    bytes repeated={0};header(&repeated,-60,16,2);group(&repeated,4,100,1);pair(&repeated,1,2,90);
    group(&repeated,4,80,2);pair(&repeated,1,2,70);pair(&repeated,2,3,60);
    put(&repeated,0,4);put(&repeated,0,4);in[0]=input("STMG",&repeated,7,0);p=compile(in,1);
    assert(value(&p,SH_AUDIO_SHARED_GROUP,4,0,0,4)==80);
    assert(value(&p,SH_AUDIO_SHARED_TRANSITION,4,1,2,8)==70);
    assert(value(&p,SH_AUDIO_SHARED_TRANSITION,4,2,3,8)==60);
    roundtrip(&p,"STMG");sh_audio_shared_policy_free(&p);
}
static bytes environment(unsigned changed)
{
    bytes b={0};for(unsigned i=0;i<6;i++){
        put(&b,1,1);put(&b,0,1);put(&b,1,2);put(&b,0,4);
        float y=changed==i ? .5f : 1;emit(&b,&y,4);put(&b,0,4);}
    return b;
}
static void environments(void)
{
    bytes base=environment(6),a=environment(0),b=environment(1);
    sh_audio_shared_input in[]={input("ENVS",&base,0,1),input("ENVS",&a,1,0),input("ENVS",&b,2,0)};
    sh_audio_shared_policy p=compile(in,3);bytes out=chunk(&p,"ENVS");assert(p.count==6);
    assert(!memcmp(out.data,a.data,16) && !memcmp(out.data+16,b.data+16,16));
    assert(!memcmp(out.data+32,base.data+32,64));roundtrip(&p,"ENVS");sh_audio_shared_policy_free(&p);
}
static void platform_and_failures(void)
{
    bytes a={0},b={0};put(&a,7,4);emit(&a,"Windows",7);put(&b,5,4);emit(&b,"Other",5);
    sh_audio_shared_input in[]={input("PLAT",&a,1,0),input("PLAT",&a,2,0)};
    sh_audio_shared_policy p=compile(in,2);bytes out=chunk(&p,"PLAT");equivalent(&a,&out);
    assert(p.count==1);roundtrip(&p,"PLAT");sh_audio_shared_policy_free(&p);char error[512];
    in[0]=input("PLAT",&b,1,0);in[1]=input("PLAT",&a,1,0);
    assert(!sh_audio_shared_policy_compile(in,2,&p,error,sizeof(error)) && !p.settings && !p.count);
    assert(strstr(error,"platform"));
    in[0]=input("PLAT",&a,0,1);in[1]=input("PLAT",&b,2,0);
    assert(!sh_audio_shared_policy_compile(in,2,&p,error,sizeof(error)) && !p.settings && !p.count);
    bytes good={0};header(&good,-60,64,0);put(&good,0,4);put(&good,1,4);rtpc(&good,42,100,6);
    in[0]=input("STMG",&good,0,0);
    for(size_t i=1;i<good.length;i++) {
        in[1]=in[0];in[1].scope=1;in[1].length=i;
        assert(!sh_audio_shared_policy_compile(in,2,&p,error,sizeof(error)) && !p.settings && !p.count);
    }
    p=compile(in,1);sh_audio_shared_policy_free(&p);
    good.data[good.length-1]=7;
    assert(!sh_audio_shared_policy_compile(in,1,&p,error,sizeof(error)) && !p.settings && !p.count);
    good.data[good.length-1]=0xff;
    assert(!sh_audio_shared_policy_compile(in,1,&p,error,sizeof(error)) && !p.settings && !p.count);
    good.data[good.length-1]=0;p=compile(in,1);assert(!setting(&p,SH_AUDIO_SHARED_BUILTIN,42,0,0));
    sh_audio_shared_policy_free(&p);
    p=compile(NULL,0);out=chunk(&p,"STMG");assert(!out.length);sh_audio_shared_policy_free(&p);
    in[0]=(sh_audio_shared_input){"STMG",NULL,0,0,0};p=compile(in,1);assert(!p.count);sh_audio_shared_policy_free(&p);
}
static void growth(void)
{
    bytes b={0};header(&b,-60,64,1000);
    for(uint32_t i=0;i<1000;i++){group(&b,1000-i,i,1);pair(&b,1,2,i);}
    put(&b,0,4);put(&b,0,4);sh_audio_shared_input in=input("STMG",&b,1,0);
    sh_audio_shared_policy p=compile(&in,1);assert(p.count==2002);
    for(uint32_t i=0;i<1000;i++)assert(value(&p,SH_AUDIO_SHARED_TRANSITION,1000-i,1,2,8)==i);
    roundtrip(&p,"STMG");sh_audio_shared_policy_free(&p);
}
static void corpus(const char *path)
{
    FILE *file=fopen(path,"rb");assert(file);unsigned char header_bytes[8];
    sh_audio_shared_input inputs[3]={0};unsigned char *bodies[3]={0};size_t count=0;
    while(fread(header_bytes,1,8,file)==8) {
        uint32_t length=word(header_bytes+4);
        const char *tag=!memcmp(header_bytes,"STMG",4) ? "STMG" : !memcmp(header_bytes,"ENVS",4) ? "ENVS" :
            !memcmp(header_bytes,"PLAT",4) ? "PLAT" : NULL;
        if(!tag){assert(!_fseeki64(file,length,SEEK_CUR));continue;}
        assert(count<3);bodies[count]=malloc(length ? length : 1);assert(bodies[count]);
        assert(fread(bodies[count],1,length,file)==length);
        inputs[count]=(sh_audio_shared_input){tag,bodies[count],length,0,1};count++;
    }
    assert(!ferror(file));fclose(file);assert(count==3);
    sh_audio_shared_policy p=compile(inputs,count);roundtrip(&p,"STMG");roundtrip(&p,"ENVS");roundtrip(&p,"PLAT");
    for(size_t i=0;i<count;i++){
        bytes out=chunk(&p,inputs[i].tag);
        printf("%s: %zu authored bytes, %zu composed bytes\n",inputs[i].tag,inputs[i].length,out.length);free(bodies[i]);}
    printf("%zu shared control records\n",p.count);sh_audio_shared_policy_free(&p);
}
/* Exact identities: equal cooked bytes naming different scoped definitions
 * (for example a switch driven by a private modulator) are different values. */
typedef struct exact_fixture { unsigned classes[8][4];int fail; } exact_fixture;
static int exact_equal(void *context,size_t li,size_t lr,size_t ri,size_t rr)
{
    exact_fixture *f=context;if(f->fail)return -1;
    assert(li<8 && ri<8 && lr<4 && rr<4);return f->classes[li][lr]==f->classes[ri][rr];
}
static const unsigned char *exact_identity(void *context,size_t input,size_t record)
{
    static unsigned char identities[16][32];exact_fixture *f=context;assert(input<8 && record<4);
    unsigned id=f->classes[input][record];assert(id<16);memset(identities[id],0,32);identities[id][0]=(unsigned char)(0x80^id);
    return identities[id];
}
static sh_audio_shared_policy exact_compile(const sh_audio_shared_input *inputs,size_t count,exact_fixture *f)
{
    sh_audio_shared_policy p={0};char error[512];sh_audio_shared_exact exact={f,exact_equal,exact_identity};
    if(!sh_audio_shared_policy_compile_exact(inputs,count,&exact,&p,error,sizeof(error))){fprintf(stderr,"%s\n",error);assert(0);}
    return p;
}
static void exact_identities(void)
{
    bytes body={0};header(&body,-60,64,0);put(&body,1,4);control(&body,700,555,1);put(&body,0,4);
    /* Same bytes in every input: baseline(0), authored A(1), authored B(2), copy of A(3). */
    sh_audio_shared_input inputs[4]={input("STMG",&body,0,1),input("STMG",&body,1,0),input("STMG",&body,2,0),input("STMG",&body,3,0)};
    exact_fixture f={0};
    f.classes[1][2]=3;f.classes[2][2]=5;f.classes[3][2]=3;
    sh_audio_shared_policy p=exact_compile(inputs,4,&f);
    const sh_audio_shared_setting *s=setting(&p,SH_AUDIO_SHARED_SWITCH,700,0,0);
    /* identity 0x80^3 sorts before 0x80^5: A's binding wins and its exact copy contributes. */
    assert(s && s->edited && s->input==1 && s->index==2 && s->contributor_count==2);
    assert(s->contributors[0].input==1 && s->contributors[1].input==3);
    assert(!setting(&p,SH_AUDIO_SHARED_THRESHOLD,0,0,0)->edited);
    /* Byte composition cannot see the difference and keeps the baseline. */
    sh_audio_shared_policy bytes_only=compile(inputs,4);
    assert(!setting(&bytes_only,SH_AUDIO_SHARED_SWITCH,700,0,0)->edited);sh_audio_shared_policy_free(&bytes_only);
    /* Enumeration order does not choose between different bindings. */
    sh_audio_shared_input reversed[4]={inputs[0],input("STMG",&body,1,0),input("STMG",&body,2,0),input("STMG",&body,3,0)};
    exact_fixture g={0};g.classes[1][2]=5;g.classes[2][2]=3;g.classes[3][2]=5;
    sh_audio_shared_policy q=exact_compile(reversed,4,&g);
    s=setting(&q,SH_AUDIO_SHARED_SWITCH,700,0,0);assert(s && s->edited && s->input==2 && s->contributor_count==1);
    sh_audio_shared_policy_free(&q);
    /* An authored copy equal to the native binding is a witness, not an edit. */
    exact_fixture h={0};h.classes[2][2]=5;
    q=exact_compile(inputs,3,&h);s=setting(&q,SH_AUDIO_SHARED_SWITCH,700,0,0);
    assert(s && s->edited && s->input==2);sh_audio_shared_policy_free(&q);
    exact_fixture w={0};q=exact_compile(inputs,2,&w);s=setting(&q,SH_AUDIO_SHARED_SWITCH,700,0,0);
    assert(s && !s->edited && s->input==0 && s->contributor_count==2);sh_audio_shared_policy_free(&q);
    /* Deep copies are independent of their source. */
    sh_audio_shared_policy copy={0};assert(sh_audio_shared_policy_copy(&p,&copy) && copy.count==p.count);
    for(size_t i=0;i<p.count;i++) {
        assert(copy.settings[i].body!=p.settings[i].body && !memcmp(copy.settings[i].body,p.settings[i].body,p.settings[i].record.length));
        assert(copy.settings[i].contributor_count==p.settings[i].contributor_count && copy.settings[i].edited==p.settings[i].edited);
    }
    sh_audio_shared_policy_free(&p);s=setting(&copy,SH_AUDIO_SHARED_SWITCH,700,0,0);assert(s && s->contributor_count==2);
    sh_audio_shared_policy_free(&copy);
    /* A retained-source comparison failure refuses the whole plan. */
    f.fail=1;sh_audio_shared_policy failed={0};char error[512];sh_audio_shared_exact exact={&f,exact_equal,exact_identity};
    assert(!sh_audio_shared_policy_compile_exact(inputs,4,&exact,&failed,error,sizeof(error)) && !failed.count && strstr(error,"compare"));
    sh_audio_shared_exact incomplete={&f,exact_equal,NULL};
    assert(!sh_audio_shared_policy_compile_exact(inputs,4,&incomplete,&failed,error,sizeof(error)));
}
int main(int argc,char **argv)
{
    composition();ordered_noops();environments();platform_and_failures();growth();exact_identities();
    if(argc==2)corpus(argv[1]);puts("shared audio policy checks passed");return 0;
}
