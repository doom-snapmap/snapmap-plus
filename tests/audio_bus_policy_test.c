#include "audio_bus_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synthetic banks pass through the real codec and exact dependency graph. */
typedef struct bytes { unsigned char data[4096];size_t size; } bytes;
typedef struct pair { uint32_t key,value; } pair;
typedef struct curve { uint32_t controller,parameter,key;unsigned mode,points; } curve;
typedef struct state_group { uint32_t key;unsigned mode;pair states[8];size_t count; } state_group;
typedef struct recipe {
    uint32_t bus,parent;unsigned kind;
    pair properties[16],effects[8],ducks[8];size_t property_count,effect_count,duck_count;
    curve curves[16];size_t curve_count;
    state_group groups[8];size_t group_count;
    unsigned tone;
    uint32_t settings[5];unsigned bypass,flags;
} recipe;
typedef struct fixture { bytes bank;sh_audio_bank_codec codec;sh_audio_graph_bank bank_input; } fixture;
typedef struct trial {
    fixture files[8];sh_audio_graph_scope scopes[8];sh_audio_link_graph *graph;
    sh_audio_bus_recipe_input inputs[8];sh_audio_bus_value_input values[8][128];size_t count;
} trial;
static void put(bytes *b,uint32_t value,size_t width)
{ assert(width<=4 && b->size+width<=sizeof(b->data));for(size_t i=0;i<width;i++)b->data[b->size++]=(unsigned char)(value>>(8*i)); }
static void zero(bytes *b,size_t width) { while(width--)put(b,0,1); }
static void patch(bytes *b,size_t at,uint32_t value)
{ assert(at+4<=b->size);for(size_t i=0;i<4;i++)b->data[at+i]=(unsigned char)(value>>(8*i)); }
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void chunk(bytes *b,const char *tag,uint32_t size)
{ assert(b->size+4<=sizeof(b->data));memcpy(b->data+b->size,tag,4);b->size+=4;put(b,size,4); }
static int read_bytes(void *context,uint64_t at,void *out,size_t length)
{ bytes *b=context;if(at>b->size || length>b->size-at)return 0;memcpy(out,b->data+(size_t)at,length);return 1; }
static void make(fixture *f,const recipe *r,int original)
{
    bytes *b=&f->bank;char error[512];
    chunk(b,"BKHD",16);put(b,113,4);put(b,42,4);zero(b,8);
    chunk(b,"HIRC",0);put(b,4,4);
    put(b,r->kind ? r->kind : 8,1);size_t size=b->size;zero(b,4);size_t start=b->size;
    put(b,r->bus,4);put(b,r->parent,4);put(b,(uint32_t)r->property_count,1);
    for(size_t i=0;i<r->property_count;i++)put(b,r->properties[i].key,1);
    for(size_t i=0;i<r->property_count;i++)put(b,r->properties[i].value,4);
    static const unsigned widths[]={1,1,2,4,1};
    for(unsigned i=0;i<5;i++)put(b,r->settings[i],widths[i]);
    zero(b,8);put(b,(uint32_t)r->duck_count,4);
    for(size_t i=0;i<r->duck_count;i++){put(b,r->ducks[i].key,4);put(b,r->ducks[i].value,4);zero(b,10);}
    put(b,(uint32_t)r->effect_count,1);if(r->effect_count)put(b,r->bypass,1);
    for(size_t i=0;i<r->effect_count;i++){put(b,r->effects[i].key,1);put(b,r->effects[i].value,4);put(b,1,1);zero(b,1);}
    zero(b,5);put(b,r->flags,1);put(b,(uint32_t)r->curve_count,2);
    for(size_t i=0;i<r->curve_count;i++) {
        const curve *c=r->curves+i;put(b,c->controller,4);put(b,0,1);put(b,c->mode,1);
        put(b,c->parameter,1);put(b,c->key,4);put(b,0,1);put(b,c->points,2);
        for(unsigned p=0;p<c->points;p++){put(b,p,4);put(b,0,4);put(b,4,4);}
    }
    put(b,(uint32_t)r->group_count,4);
    for(size_t i=0;i<r->group_count;i++) {
        const state_group *g=r->groups+i;put(b,g->key,4);put(b,g->mode,1);put(b,(uint32_t)g->count,2);
        for(size_t j=0;j<g->count;j++){put(b,g->states[j].key,4);put(b,g->states[j].value,4);}
    }
    patch(b,size,(uint32_t)(b->size-start));
    put(b,18,1);put(b,18,4);put(b,74,4);put(b,1234,4);put(b,1,4);put(b,r->tone,1);zero(b,5);
    for(uint32_t id=81;id<=82;id++){put(b,1,1);put(b,10,4);put(b,id,4);put(b,1,1);put(b,7,1);put(b,id,4);}
    patch(b,28,(uint32_t)(b->size-32));
    sh_audio_bank_source source={b,b->size,read_bytes};int ready=sh_audio_bank_codec_read(source,&f->codec,error,sizeof(error));
    if(ready!=1)fprintf(stderr,"fixture: %s\n",error);assert(ready==1);
    f->bank_input=(sh_audio_graph_bank){"sound/bus.bnk",source,&f->codec,original};
}
static trial *open_trial(const recipe *recipes,const int *baseline,size_t count)
{
    assert(count<=8);trial *t=calloc(1,sizeof(*t));assert(t);t->count=count;char error[512];
    for(size_t i=0;i<count;i++) {
        make(t->files+i,recipes+i,baseline && baseline[i]);
        t->scopes[i]=(sh_audio_graph_scope){&t->files[i].bank_input,1};
    }
    t->graph=sh_audio_link_graph_open(t->scopes,count,error,sizeof(error));
    if(!t->graph)fprintf(stderr,"graph: %s\n",error);assert(t->graph);
    for(size_t i=0;i<count;i++) {
        sh_audio_bus_recipe_input *input=t->inputs+i;const sh_audio_bank_codec *codec=&t->files[i].codec;
        assert(codec->bus_record_count<=128);input->bus=recipes[i].bus;input->kind=recipes[i].kind ? recipes[i].kind : 8;
        input->baseline=baseline && baseline[i];input->values=t->values[i];input->count=codec->bus_record_count;
        assert(sh_audio_link_graph_bus_object(t->graph,i,0,0,&input->root));
        for(size_t v=0;v<input->count;v++) {
            sh_audio_bus_value_input *value=t->values[i]+v;value->field=codec->bus_records[v];
            unsigned char *body=NULL;size_t length=0;
            assert(sh_audio_link_graph_bus_value(t->graph,i,0,v,0,&body,&length,error,sizeof(error)) && length==value->field.length);
            value->body=body;value->origin=i*128+v;
            assert(sh_audio_link_graph_bus_record(t->graph,i,0,v,&value->root));
        }
    }
    return t;
}
static void close_trial(trial *t)
{
    sh_audio_link_graph_close(t->graph);
    for(size_t i=0;i<t->count;i++) {
        for(size_t v=0;v<t->files[i].codec.bus_record_count;v++)free((void *)t->values[i][v].body);
        sh_audio_bank_codec_free(&t->files[i].codec);
    }
    free(t);
}
static sh_audio_bus_policy compile(trial *t)
{
    sh_audio_bus_policy p={0};char error[512];int ok=sh_audio_bus_policy_compile(t->graph,t->inputs,t->count,&p,error,sizeof(error));
    if(!ok)fprintf(stderr,"compile: %s\n",error);assert(ok);return p;
}
static const sh_audio_bus_setting *find(const sh_audio_bus_policy *p,uint32_t bus,sh_audio_bus_field field,uint32_t a,uint32_t b)
{
    for(size_t i=0;i<p->count;i++)if(p->settings[i].bus==bus && p->settings[i].field.field==field &&
        p->settings[i].field.key[0]==a && p->settings[i].field.key[1]==b)return p->settings+i;
    assert(0);return NULL;
}
static void check_value(const sh_audio_bus_policy *p,uint32_t bus,sh_audio_bus_field field,uint32_t key,uint32_t value)
{ const sh_audio_bus_setting *s=find(p,bus,field,key,0);assert(!s->removed && s->field.length>=4 && u32(s->body)==value); }
static void same_policy(const sh_audio_bus_policy *a,const sh_audio_bus_policy *b)
{
    assert(a->count==b->count && a->bus_count==b->bus_count);
    for(size_t i=0;i<a->bus_count;i++)assert(a->buses[i].bus==b->buses[i].bus && a->buses[i].kind==b->buses[i].kind);
    for(size_t i=0;i<a->count;i++) {
        const sh_audio_bus_setting *x=a->settings+i,*y=b->settings+i;
        assert(x->bus==y->bus && x->field.field==y->field.field && !memcmp(x->field.key,y->field.key,sizeof(x->field.key)));
        assert(x->removed==y->removed && x->field.length==y->field.length && x->contributor_count==y->contributor_count);
        if(x->field.length)assert(!memcmp(x->body,y->body,x->field.length));
    }
}
static void permutations(trial *t,const sh_audio_bus_policy *expected,size_t at)
{
    if(at==t->count){sh_audio_bus_policy p=compile(t);same_policy(expected,&p);sh_audio_bus_policy_free(&p);return;}
    for(size_t i=at;i<t->count;i++) {
        sh_audio_bus_recipe_input tmp=t->inputs[at];t->inputs[at]=t->inputs[i];t->inputs[i]=tmp;
        permutations(t,expected,at+1);tmp=t->inputs[at];t->inputs[at]=t->inputs[i];t->inputs[i]=tmp;
    }
}
static void edits_and_deletions(void)
{
    recipe r[4]={0};r[0].bus=71;r[0].properties[0]=(pair){7,10};r[0].properties[1]=(pair){8,20};r[0].property_count=2;
    r[1]=r[2]=r[0];r[1].properties[0].value=11;r[1].properties[2]=(pair){9,30};r[1].property_count++;
    r[2].properties[1].value=21;r[2].properties[2]=(pair){10,40};r[2].property_count++;r[3]=r[1];
    int baseline[]={1,0,0,0};trial *t=open_trial(r,baseline,4);sh_audio_bus_policy p=compile(t);
    check_value(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,7,11);check_value(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,8,21);
    check_value(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,9,30);check_value(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,10,40);
    const sh_audio_bus_setting *s=find(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,7,0);assert(s->contributor_count==2 && s->edited);
    /* A key no recipe changes keeps the native value and is not an edit. */
    r[3]=r[0];r[3].properties[3]=(pair){11,50};r[3].property_count=3;r[3].properties[2]=(pair){12,60};
    {
        sh_audio_bus_policy q;trial *u=open_trial(r,baseline,1);q=compile(u);
        for(size_t i=0;i<q.count;i++)assert(!q.settings[i].edited);
        sh_audio_bus_policy_free(&q);close_trial(u);
    }
    r[3]=r[1];
    for(size_t i=0;i<s->contributor_count;i++)assert(s->contributors[i].recipe==1 || s->contributors[i].recipe==3);
    permutations(t,&p,0);close_trial(t);
    /* Output bodies remain owned after every source/graph has gone away. */
    check_value(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,7,11);sh_audio_bus_policy_free(&p);
    r[1]=r[0];r[1].properties[0]=r[1].properties[1];r[1].property_count=1;r[2]=r[0];
    t=open_trial(r,baseline,3);p=compile(t);s=find(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,7,0);
    assert(s->removed && s->edited && !s->body && !s->field.length && s->contributor_count==1);
    assert(s->contributors[0].recipe==1 && s->contributors[0].value==SIZE_MAX);permutations(t,&p,0);
    sh_audio_bus_policy_free(&p);close_trial(t);
    r[2].properties[0].value=12;t=open_trial(r,baseline,3);p=compile(t);
    check_value(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,7,12);permutations(t,&p,0);sh_audio_bus_policy_free(&p);close_trial(t);
}
static void dependency_identity(void)
{
    recipe r[3]={0};r[0].bus=71;r[0].effects[0]=(pair){2,74};r[0].effect_count=1;r[0].tone=11;
    r[1]=r[2]=r[0];r[1].tone=22;trial *t=open_trial(r,NULL,3);sh_audio_bus_policy p=compile(t);
    const sh_audio_bus_setting *s=find(&p,71,SH_AUDIO_BUS_FIELD_EFFECT,2,0);
    assert(!s->removed && s->body[0]==74);size_t selected=s->recipe;
    assert(s->contributor_count==(selected==1 ? 1u : 2u));
    for(size_t i=0;i<s->contributor_count;i++)assert((s->contributors[i].recipe==1)==(selected==1));
    permutations(t,&p,0);
    /* Identical ID bytes cannot hide a fingerprint collision between dependencies. */
    size_t value=s->value%128;memcpy(t->values[1][value].root.identity,t->values[0][value].root.identity,32);
    sh_audio_bus_policy rejected={0};char error[512];
    assert(!sh_audio_bus_policy_compile(t->graph,t->inputs,3,&rejected,error,sizeof(error)) && strstr(error,"fingerprint"));
    assert(!rejected.count && !rejected.settings);sh_audio_bus_policy_free(&p);close_trial(t);
}
static void parent_forest(void)
{
    recipe r[4]={0};r[0].bus=71;r[1].bus=72;r[2]=r[0];r[2].parent=72;r[3]=r[1];r[3].parent=71;
    int baseline[]={1,1,0,0};trial *t=open_trial(r,baseline,4);sh_audio_bus_policy p=compile(t);
    uint32_t a=u32(find(&p,71,SH_AUDIO_BUS_FIELD_PARENT,0,0)->body),b=u32(find(&p,72,SH_AUDIO_BUS_FIELD_PARENT,0,0)->body);
    assert((a==72 && !b) || (!a && b==71));permutations(t,&p,0);sh_audio_bus_policy_free(&p);close_trial(t);
    /* A cycle with no baseline/author alternative is malformed, not an overlap. */
    r[0]=r[2];r[1]=r[3];t=open_trial(r,NULL,2);char error[512];
    assert(!sh_audio_bus_policy_compile(t->graph,t->inputs,2,&p,error,sizeof(error)) && strstr(error,"rooted"));
    assert(!p.settings && !p.count);close_trial(t);
    r[0].parent=999;t=open_trial(r,NULL,2);p=compile(t);check_value(&p,71,SH_AUDIO_BUS_FIELD_PARENT,0,999);
    sh_audio_bus_policy_free(&p);close_trial(t);
}
static void curve_operations(void)
{
    recipe r={0};r.bus=71;r.curves[0]=(curve){51,7,1,2,1};r.curves[1]=(curve){52,7,1,4,1};r.curve_count=2;
    trial *t=open_trial(&r,NULL,1);sh_audio_bus_policy p=compile(t);
    const sh_audio_bus_setting *s=find(&p,71,SH_AUDIO_BUS_FIELD_RTPC_CURVE,7,1);
    assert(!s->removed && u32(s->body)==52);assert(find(&p,71,SH_AUDIO_BUS_FIELD_RTPC_MODE,7,0)->body[0]==2);
    sh_audio_bus_policy_free(&p);close_trial(t);
    r.curves[1].points=0;r.curves[2]=(curve){53,7,2,5,1};r.curve_count=3;
    t=open_trial(&r,NULL,1);p=compile(t);assert(find(&p,71,SH_AUDIO_BUS_FIELD_RTPC_CURVE,7,1)->removed);
    assert(find(&p,71,SH_AUDIO_BUS_FIELD_RTPC_MODE,7,0)->body[0]==5);sh_audio_bus_policy_free(&p);close_trial(t);
    r.curves[3]=(curve){53,7,2,8,0};r.curve_count=4;t=open_trial(&r,NULL,1);p=compile(t);
    s=find(&p,71,SH_AUDIO_BUS_FIELD_RTPC_MODE,7,0);assert(s->removed && s->contributor_count==2);
    sh_audio_bus_policy_free(&p);close_trial(t);
}
static void cross_package_collections(void)
{
    recipe r[4]={0};r[0].bus=71;r[0].curves[0]=(curve){51,7,1,2,1};r[0].curves[1]=(curve){52,7,2,2,1};r[0].curve_count=2;
    r[1]=r[2]=r[3]=r[0];r[1].curves[0]=r[1].curves[1];r[1].curve_count=r[2].curve_count=1;
    int baseline[]={1,0,0,0};trial *t=open_trial(r,baseline,3);sh_audio_bus_policy p=compile(t);
    const sh_audio_bus_setting *mode=find(&p,71,SH_AUDIO_BUS_FIELD_RTPC_MODE,7,0);
    assert(mode->removed && mode->contributor_count==2);
    for(size_t i=0;i<mode->contributor_count;i++)assert(mode->contributors[i].value==SIZE_MAX && mode->contributors[i].recipe);
    permutations(t,&p,0);sh_audio_bus_policy_free(&p);close_trial(t);
    r[3].curves[2]=(curve){53,7,3,2,1};r[3].curve_count=3;
    t=open_trial(r,baseline,4);p=compile(t);assert(!find(&p,71,SH_AUDIO_BUS_FIELD_RTPC_MODE,7,0)->removed);
    assert(!find(&p,71,SH_AUDIO_BUS_FIELD_RTPC_CURVE,7,3)->removed);permutations(t,&p,0);sh_audio_bus_policy_free(&p);close_trial(t);
    memset(r,0,sizeof(r));r[0].bus=71;r[0].group_count=1;r[0].groups[0]=(state_group){10,1,{{100,81}},1};
    r[1]=r[2]=r[0];r[1].group_count=0;r[2].groups[0].states[1]=(pair){101,82};r[2].groups[0].count=2;
    t=open_trial(r,baseline,3);p=compile(t);
    assert(find(&p,71,SH_AUDIO_BUS_FIELD_STATE,10,100)->removed);
    assert(!find(&p,71,SH_AUDIO_BUS_FIELD_STATE,10,101)->removed);
    assert(!find(&p,71,SH_AUDIO_BUS_FIELD_STATE_GROUP,10,0)->removed);
    permutations(t,&p,0);sh_audio_bus_policy_free(&p);close_trial(t);
}
static void native_recipe_order(void)
{
    recipe r={0};r.bus=71;r.effects[0]=(pair){2,74};r.effects[1]=(pair){2,0};r.effect_count=2;
    r.properties[0]=(pair){0,10};r.properties[1]=(pair){0,20};r.property_count=2;
    r.ducks[0]=(pair){999,10};r.ducks[1]=(pair){999,20};r.duck_count=2;
    r.groups[0]=(state_group){10,1,{{100,81}},1};r.groups[1]=(state_group){10,2,{{100,82}},1};r.group_count=2;
    trial *t=open_trial(&r,NULL,1);sh_audio_bus_policy p=compile(t);
    check_value(&p,71,SH_AUDIO_BUS_FIELD_EFFECT,2,74);check_value(&p,71,SH_AUDIO_BUS_FIELD_DUCK,999,20);
    check_value(&p,71,SH_AUDIO_BUS_FIELD_PROPERTY,0,10);
    assert(find(&p,71,SH_AUDIO_BUS_FIELD_STATE_GROUP,10,0)->body[0]==2);
    assert(u32(find(&p,71,SH_AUDIO_BUS_FIELD_STATE,10,100)->body)==82);
    sh_audio_bus_policy_free(&p);close_trial(t);
}
static uint32_t packed(const sh_audio_bus_policy *policy,sh_audio_bus_field field,uint32_t key)
{
    uint32_t value=0,used=0;size_t count=0;
    for(size_t i=0;i<policy->count;i++) {
        const sh_audio_bus_setting *s=policy->settings+i;
        if(s->field.field!=field || s->field.key[0]!=key)continue;
        assert(!s->removed && s->field.length<=4);uint32_t bits=0;
        for(size_t b=0;b<s->field.length;b++)bits|=(uint32_t)s->body[b]<<(8*b);
        uint32_t mask=sh_audio_bus_record_mask(&s->field);
        assert(mask && !(used&mask) && !(bits&~mask));used|=mask;value|=bits;count++;
    }
    assert(count && (used==255 || used==65535));return value;
}
static size_t masked_record(const trial *t,size_t scope,sh_audio_bus_field field,uint32_t key,uint32_t mask)
{
    const sh_audio_bank_codec *c=&t->files[scope].codec;
    for(size_t i=0;i<c->bus_record_count;i++)if(c->bus_records[i].field==field &&
        c->bus_records[i].key[0]==key && c->bus_records[i].key[1]==mask)return i;
    assert(0);return SIZE_MAX;
}
static void packed_settings(void)
{
    recipe r[4]={0};r[0].bus=71;r[0].effects[0]=(pair){0,74};r[0].effect_count=1;
    r[0].settings[0]=3;r[0].settings[2]=0xfc0a;r[0].bypass=0x83;r[0].flags=0x80;
    r[1]=r[2]=r[0];r[1].settings[0]=2;r[2].settings[0]=1;
    r[1].settings[1]=1;r[2].settings[1]=2;r[1].settings[2]=0xfc19;r[2].settings[2]=0x800a;
    r[1].settings[4]=1;r[2].settings[4]=2;r[1].bypass=0x82;r[2].bypass=0x81;
    r[1].flags=0x81;r[2].flags=0x40;r[3]=r[1];
    int baseline[]={1,0,0,0};trial *t=open_trial(r,baseline,4);sh_audio_bus_policy p=compile(t);
    assert(packed(&p,SH_AUDIO_BUS_FIELD_SETTING,0)==0); /* Preserve both clears; OR would fail. */
    assert(packed(&p,SH_AUDIO_BUS_FIELD_SETTING,1)==3 && packed(&p,SH_AUDIO_BUS_FIELD_SETTING,4)==3);
    assert(packed(&p,SH_AUDIO_BUS_FIELD_SETTING,2)==0x8019); /* The ten-bit count remains atomic. */
    assert(packed(&p,SH_AUDIO_BUS_FIELD_BYPASS,0)==0x80 && packed(&p,SH_AUDIO_BUS_FIELD_FLAGS,0)==0x41);
    const sh_audio_bus_setting *s=find(&p,71,SH_AUDIO_BUS_FIELD_SETTING,0,1);assert(s->contributor_count==2);
    for(size_t i=0;i<s->contributor_count;i++)assert(s->contributors[i].recipe==1 || s->contributors[i].recipe==3);
    size_t left=masked_record(t,0,SH_AUDIO_BUS_FIELD_SETTING,0,2),right=masked_record(t,1,SH_AUDIO_BUS_FIELD_SETTING,0,2);
    sh_audio_graph_root a=t->values[0][left].root,b=t->values[1][right].root;
    assert(!memcmp(a.identity,b.identity,32) && sh_audio_link_graph_equal(t->graph,a.node,b.node));
    assert(!sh_audio_link_graph_equal(t->graph,t->inputs[0].root.node,t->inputs[1].root.node));
    permutations(t,&p,0);sh_audio_bus_policy_free(&p);close_trial(t);
    /* Without an original witness, contradictory global bits follow the stable
     * choice policy independently. No package is rejected for that overlap. */
    r[0].settings[0]=1;r[1]=r[0];r[1].settings[0]=2;
    t=open_trial(r,NULL,2);p=compile(t);assert(packed(&p,SH_AUDIO_BUS_FIELD_SETTING,0)==0);
    permutations(t,&p,0);sh_audio_bus_policy_free(&p);close_trial(t);
}
int main(void)
{
    edits_and_deletions();dependency_identity();parent_forest();curve_operations();
    cross_package_collections();native_recipe_order();packed_settings();puts("audio_bus_policy_test: passed");return 0;
}
