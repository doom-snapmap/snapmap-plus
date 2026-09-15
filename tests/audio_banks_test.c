#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_banks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while (0)
static void put32(unsigned char *p, uint32_t n)
{ for (int i=0;i<4;++i) p[i]=(unsigned char)(n>>(8*i)); }

static void test_identities(void)
{
    static const struct { const char *name; uint32_t id; } sounds[] = {
        {"play_sample", UINT32_C(1811189196)},
        {"PLAY_SAMPLE", UINT32_C(1811189196)},
        {"stop_sample", UINT32_C(3021552942)},
        {"sample", UINT32_C(1811189196)},
        {"folder/SAMPLE.wav", UINT32_C(1811189196)},
        {"folder\\SAMPLE.wav", UINT32_C(1811189196)},
        {"/folder/SAMPLE.wav", UINT32_C(1811189196)},
        {"//folder/SAMPLE.wav", UINT32_C(1811189196)},
        {"folder/stop_sample.wav", UINT32_C(1502305715)},
        {"play_sample.wav", UINT32_C(1457459912)},
        {"play_sample\\tail", UINT32_C(1990693854)},
        {"folder/file.part.wav", UINT32_C(1753787257)},
        {"folder/_sample", UINT32_C(3773393499)},
        {"folder/", UINT32_C(500558194)},
        {"effects/possession/fx_possess_start", UINT32_C(4248274884)},
        {"monster/arch_vile/leading_wave", UINT32_C(515078688)}
    };
    uint32_t id;
    char name[270];
    for(size_t i=0;i<sizeof(sounds)/sizeof(sounds[0]);i++) {
        CHECK(sh_audio_event_identity(sounds[i].name,&id)); CHECK(id==sounds[i].id);
    }
    CHECK(sh_audio_bank_identity("a.bnk",&id) && id==UINT32_C(84696446));
    CHECK(sh_audio_bank_identity("B.BNK",&id) && id==UINT32_C(84696445));
    CHECK(sh_audio_bank_identity("doom_weapon_sp.bnk",&id) && id==UINT32_C(835849565));
    CHECK(!sh_audio_bank_identity("locale/a.bnk",&id) && !id);
    CHECK(!sh_audio_bank_identity("locale\\a.bnk",&id) && !id);
    CHECK(!sh_audio_bank_identity(".bnk",&id) && !id);
    CHECK(!sh_audio_bank_identity("",&id) && !id);
    CHECK(!sh_audio_event_identity("",&id) && !id);
    CHECK(!sh_audio_event_identity(NULL,&id) && !id);
    CHECK(!sh_audio_event_identity("play_sample",NULL));
    CHECK(!sh_audio_event_identity("play_\xff",&id) && !id);
    CHECK(!sh_audio_bank_identity("\xff.bnk",&id) && !id);
    memset(name,'a',sizeof(name)); name[250]=0;
    CHECK(sh_audio_event_identity(name,&id)); /* Prefix fits the native string. */
    name[250]='a'; name[251]=0;
    CHECK(!sh_audio_event_identity(name,&id) && !id);
    memset(name,'a',sizeof(name)); memcpy(name,"play_",5); name[255]=0;
    CHECK(sh_audio_event_identity(name,&id)); /* Early return needs no prefix. */
    name[255]='a'; name[256]=0;
    CHECK(!sh_audio_event_identity(name,&id) && !id);
    memset(name,'a',sizeof(name)); name[259]=0;
    CHECK(sh_audio_bank_identity(name,&id));
    name[259]='a'; name[260]=0;
    CHECK(!sh_audio_bank_identity(name,&id) && !id);
}
/* Three events with empty action lists and one streamed sound, in the verified
 * v113 shapes: an event body is its identity plus a counted action list, and a
 * sound body is its identity plus a fourteen-byte source descriptor. */
#define FIXTURE_BYTES 109
static size_t fixture(unsigned char *p)
{
    memset(p,0,FIXTURE_BYTES);
    memcpy(p,"BKHD",4); put32(p+4,16); put32(p+8,113); put32(p+12,42);
    memcpy(p+24,"DATA",4); put32(p+28,3); p[32]=7;
    memcpy(p+35,"HIRC",4); put32(p+39,66); put32(p+43,4);
    for (int i=0;i<3;++i) { p[47+i*13]=4; put32(p+48+i*13,8); }
    put32(p+52,999); put32(p+65,7); put32(p+78,999);
    p[86]=2; put32(p+87,18); put32(p+91,123); /* A sound object is not an event. */
    p[99]=1; put32(p+100,4242);
    return FIXTURE_BYTES;
}
static int parse(const unsigned char *p, size_t n, sh_audio_bank *bank)
{
    char error[256]; FILE *f=tmpfile(); int result;
    CHECK(f!=NULL); if(!f) return 0;
    CHECK(fwrite(p,1,n,f)==n); result=sh_audio_bank_read(f,bank,error,sizeof(error)); fclose(f);
    if(!result) CHECK(error[0]);
    return result;
}
static void test_reader(void)
{
    unsigned char data[FIXTURE_BYTES]; size_t n=fixture(data); sh_audio_bank bank={0};
    CHECK(parse(data,n,&bank)); CHECK(bank.id==42 && bank.event_count==2);
    CHECK(bank.events && bank.events[0]==7 && bank.events[1]==999);
    CHECK(sh_audio_bank_has_event(&bank,999)); CHECK(!sh_audio_bank_has_event(&bank,123));
    /* The streamed source is a media dependency, not a carried payload. */
    CHECK(bank.media_count==1 && bank.media[0].id==4242);
    CHECK(bank.media[0].stream==1 && !bank.media[0].embedded);
    CHECK(sh_audio_bank_find_media(&bank,4242)==bank.media);
    CHECK(!sh_audio_bank_find_media(&bank,123) && !bank.bank_count);
    CHECK(!bank.required_count && !bank.optional_count);
    sh_audio_bank_free(&bank); sh_audio_bank_free(&bank);
    for(size_t i=0;i<n;++i) {
        int result=parse(data,i,&bank);
        /* Complete BKHD and optional DATA alone are valid header-only banks. */
        CHECK(result==(i==24 || i==35)); sh_audio_bank_free(&bank);
    }
    put32(data+39,0xffffffffu); CHECK(!parse(data,n,&bank)); CHECK(!bank.events && !bank.id);
    fixture(data); put32(data+43,0xffffffffu); CHECK(!parse(data,n,&bank));
    fixture(data); put32(data+48,3); CHECK(!parse(data,n,&bank));
    fixture(data); put32(data+48,0xffffffffu); CHECK(!parse(data,n,&bank));
    fixture(data); put32(data+52,0); CHECK(!parse(data,n,&bank));
    fixture(data); put32(data+8,114); CHECK(!parse(data,n,&bank));
    fixture(data); put32(data+12,0); CHECK(!parse(data,n,&bank));
    fixture(data); memcpy(data+24,"BKHD",4); CHECK(!parse(data,n,&bank));
    /* An event body that cannot hold its own counted action list is refused. */
    fixture(data); put32(data+56,1); CHECK(!parse(data,n,&bank));
    /* An unmodelled stream kind is refused instead of indexed as resident. */
    fixture(data); data[99]=3; CHECK(!parse(data,n,&bank));
    CHECK(sh_audio_bank_read(NULL,&bank,NULL,0)==-1); CHECK(sh_audio_bank_read(NULL,NULL,NULL,0)==-1);
}

/* Event -> action -> another bank, plus a carried and a streamed media source,
 * in one bank. Layout mirrors the shipped v113 objects exactly. properties and
 * ranges lengthen the two counted lists that precede the play action tail. */
static size_t dependency_bank(unsigned char *p, unsigned properties, unsigned ranges,
    uint32_t play_bank, size_t *play_bank_at)
{
    size_t at, tail;
    uint32_t action=(uint32_t)(18+properties*5+ranges*9);
    memset(p,0,256);
    memcpy(p,"BKHD",4); put32(p+4,16); put32(p+8,113); put32(p+12,42);
    memcpy(p+24,"DIDX",4); put32(p+28,24);
    put32(p+32,900); put32(p+36,0); put32(p+40,4);
    put32(p+44,901); put32(p+48,4); put32(p+52,4);
    memcpy(p+56,"DATA",4); put32(p+60,8);
    memcpy(p+72,"HIRC",4); at=80;
    put32(p+at,4); at+=4;                                  /* four objects */
    p[at]=4; put32(p+at+1,16); put32(p+at+5,7);            /* event 7 */
    put32(p+at+9,2); put32(p+at+13,50); put32(p+at+17,51); at+=21;
    p[at]=3; put32(p+at+1,action); put32(p+at+5,50);       /* play action */
    p[at+9]=0x03; p[at+10]=0x04; put32(p+at+11,600);
    tail=at+16; p[tail]=(unsigned char)properties; tail+=1+properties*5;
    p[tail]=(unsigned char)ranges; tail+=1+ranges*9;
    *play_bank_at=tail+1; put32(p+tail+1,play_bank); at+=5+action;
    p[at]=3; put32(p+at+1,18); put32(p+at+5,51);           /* stop action */
    p[at+9]=0x03; p[at+10]=0x01; put32(p+at+11,601); at+=23;
    p[at]=2; put32(p+at+1,18); put32(p+at+5,600);          /* prefetched sound */
    p[at+13]=2; put32(p+at+14,900); at+=23;
    put32(p+76,(uint32_t)(at-80));
    return at;
}
static void test_dependencies(void)
{
    unsigned char data[256]; size_t at, bank_at; sh_audio_bank bank={0};
    at=dependency_bank(data,0,0,4242,&bank_at);
    CHECK(parse(data,at,&bank));
    CHECK(bank.event_count==1 && bank.events[0]==7);
    CHECK(bank.bank_count==1 && bank.banks[0]==4242);
    /* 601 is a stop action's target and defined nowhere here: legally absent,
     * so it is optional. A play target would have been required. */
    CHECK(bank.optional_count==1 && bank.optional[0]==601 && !bank.required_count);
    CHECK(bank.media_count==2);
    CHECK(bank.media[0].id==900 && bank.media[0].stream==2 && bank.media[0].embedded);
    CHECK(bank.media[1].id==901 && !bank.media[1].stream && bank.media[1].embedded);
    CHECK(sh_audio_bank_needs_bank(&bank,4242) && !sh_audio_bank_needs_bank(&bank,42));
    sh_audio_bank_free(&bank);
    /* A play action naming its own bank is not a dependency on another bank. */
    at=dependency_bank(data,0,0,42,&bank_at);
    CHECK(parse(data,at,&bank)); CHECK(!bank.bank_count); sh_audio_bank_free(&bank);
    /* Property and range lists shift the tail; the bank identity still reads. */
    at=dependency_bank(data,2,1,4242,&bank_at);
    CHECK(parse(data,at,&bank)); CHECK(bank.bank_count==1 && bank.banks[0]==4242);
    sh_audio_bank_free(&bank);
    /* A play target this bank owns and does not define is a required reference:
     * the event cannot sound without it. A target another bank owns is not. */
    at=dependency_bank(data,0,0,42,&bank_at); put32(data+116,777);
    CHECK(parse(data,at,&bank));
    CHECK(bank.required_count==1 && bank.required[0]==777 && bank.optional_count==1);
    CHECK(!bank.bank_count);
    sh_audio_bank_free(&bank);
    at=dependency_bank(data,0,0,0,&bank_at); put32(data+116,777);
    CHECK(parse(data,at,&bank));
    CHECK(bank.required_count==1 && bank.required[0]==777 && !bank.bank_count);
    sh_audio_bank_free(&bank);
    /* The same absent target, attributed to another bank, is that bank's job. */
    at=dependency_bank(data,0,0,4242,&bank_at); put32(data+116,777);
    CHECK(parse(data,at,&bank));
    CHECK(!bank.required_count && bank.bank_count==1 && bank.banks[0]==4242);
    sh_audio_bank_free(&bank);
    /* An event action list entry the bank does not define is required too. */
    at=dependency_bank(data,0,0,4242,&bank_at); put32(data+97,778);
    CHECK(parse(data,at,&bank));
    CHECK(bank.required_count==1 && bank.required[0]==778);
    sh_audio_bank_free(&bank);
    at=dependency_bank(data,0,0,4242,&bank_at);
    CHECK(parse(data,at,&bank)); CHECK(!bank.required_count);
    sh_audio_bank_free(&bank);
    /* A property list that runs past its own object is refused. */
    at=dependency_bank(data,0,0,4242,&bank_at); data[bank_at-3]=9;
    CHECK(!parse(data,at,&bank));
    at=dependency_bank(data,0,0,4242,&bank_at);
    put32(data+28,25); CHECK(!parse(data,at,&bank)); /* DIDX rows are 12 bytes. */
}

static void test_name_aliases(void)
{
    uint32_t id=0;
    CHECK(sh_audio_bank_names_identity("a.bnk",UINT32_C(84696446)));
    CHECK(sh_audio_bank_names_identity("A.BNK",UINT32_C(84696446)));
    CHECK(!sh_audio_bank_names_identity("a.bnk",UINT32_C(84696445)));
    /* A decimal stem is the identity the native loader reopens a bank with. */
    CHECK(sh_audio_bank_names_identity("84696445.bnk",UINT32_C(84696445)));
    CHECK(sh_audio_bank_names_identity("4294967295.bnk",UINT32_MAX));
    CHECK(!sh_audio_bank_names_identity("4294967296.bnk",1));
    CHECK(!sh_audio_bank_names_identity("007.bnk",7));
    CHECK(!sh_audio_bank_names_identity("locale/7.bnk",7));
    CHECK(!sh_audio_bank_names_identity("7.bnk",0));
    CHECK(!sh_audio_bank_names_identity(NULL,7));
    /* The hashed spelling of a decimal name remains a valid second identity. */
    CHECK(sh_audio_bank_identity("7.bnk",&id) && id!=7);
    CHECK(sh_audio_bank_names_identity("7.bnk",id) && sh_audio_bank_names_identity("7.bnk",7));
    CHECK(sh_audio_media_identity("4242.wem",&id) && id==4242);
    CHECK(!sh_audio_media_identity("name.wem",&id) && !id);
    CHECK(!sh_audio_media_identity("0.wem",&id) && !id);
    CHECK(!sh_audio_media_identity("english(us)/4242.wem",&id) && !id);
}

typedef struct sparse_bank { unsigned char prefix[32], tail[25]; uint64_t tail_at; size_t bytes; int fail; } sparse_bank;
static int sparse_read(void *context,uint64_t offset,void *out,size_t length)
{
    sparse_bank *source=context;
    source->bytes+=length;
    if(source->fail) return 0;
    if(offset<=sizeof(source->prefix) && length<=sizeof(source->prefix)-offset) {
        memcpy(out,source->prefix+(size_t)offset,length); return 1;
    }
    if(offset>=source->tail_at && offset-source->tail_at<=sizeof(source->tail) &&
        length<=sizeof(source->tail)-(offset-source->tail_at)) {
        memcpy(out,source->tail+(size_t)(offset-source->tail_at),length); return 1;
    }
    CHECK(0); return 0; /* Embedded media must never be read by this indexer. */
}
static void test_streaming_source(void)
{
    sparse_bank source={0}; sh_audio_bank bank={0}; char error[256];
    memcpy(source.prefix,"BKHD",4); put32(source.prefix+4,16); put32(source.prefix+8,113);
    put32(source.prefix+12,42); memcpy(source.prefix+24,"DATA",4); put32(source.prefix+28,UINT32_MAX);
    source.tail_at=32+UINT64_C(0xffffffff);
    memcpy(source.tail,"HIRC",4); put32(source.tail+4,17); put32(source.tail+8,1);
    source.tail[12]=4; put32(source.tail+13,8); put32(source.tail+17,123);
    sh_audio_bank_source input={&source,source.tail_at+sizeof(source.tail),sparse_read};
    CHECK(sh_audio_bank_read_source(input,&bank,error,sizeof(error))==1);
    CHECK(bank.id==42 && sh_audio_bank_has_event(&bank,123) && source.bytes==57);
    sh_audio_bank_free(&bank);
    source.fail=1;
    CHECK(sh_audio_bank_read_source(input,&bank,error,sizeof(error))==-1 && !bank.id && !bank.events);
    source.fail=0; input.length=source.tail_at+20;
    CHECK(sh_audio_bank_read_source(input,&bank,error,sizeof(error))==0 && !bank.id && !bank.events);
    input.read=NULL;
    CHECK(sh_audio_bank_read_source(input,&bank,error,sizeof(error))==-1);
}

typedef struct fake_audio {
    unsigned char loaded[5];
    int load_calls, unload_calls, fail_load, fail_unload, uncertain, wrong_id;
    int fail_on_load_call, missing_unload;
    unsigned char desired[5], actual[5];
} fake_audio;
static int load(void *context, const char *name, uint32_t *id)
{
    fake_audio *f=(fake_audio *)context; int n=name[0]-'a'+1;
    ++f->load_calls; *id=(uint32_t)n;
    if(f->uncertain) { f->loaded[n]=1; return -1; }
    if(f->fail_load==n || f->load_calls==f->fail_on_load_call) return 2;
    if(f->loaded[n]) return 69;
    if(f->wrong_id) *id=4;
    f->loaded[*id]=1; f->actual[*id]=f->desired[n]; return 1;
}
static int unload(void *context, uint32_t id)
{
    fake_audio *f=(fake_audio *)context; ++f->unload_calls;
    if(f->fail_unload==(int)id) return 2;
    if(f->missing_unload) { f->loaded[id]=0; return 54; }
    CHECK(id<5 && f->loaded[id]); f->loaded[id]=0; return 1;
}
static void reset(sh_audio_bank_slot *slots, fake_audio *f)
{
    memset(f,0,sizeof(*f)); memset(slots,0,3*sizeof(*slots));
    slots[0].name="a.bnk";slots[0].id=1;
    slots[1].name="b.bnk";slots[1].id=2;
    slots[2].name="c.bnk";slots[2].id=3;
}
static void test_recovery(void)
{
    fake_audio f; sh_audio_bank_slot slots[3]; char error[256];
    const unsigned char empty[]={0,0,0}, old[]={1,0,0}, next[]={0,1,1}, all[]={1,1,1};
    sh_audio_bank_ops ops={&f,load,unload};
    reset(slots,&f); f.loaded[1]=1;
    CHECK(sh_audio_banks_reconcile(slots,3,old,NULL,ops,error,sizeof(error)));
    CHECK(!slots[0].owned);
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(f.loaded[1] && f.unload_calls==0); /* Preserve a native-owned bank. */
    reset(slots,&f);
    CHECK(sh_audio_banks_reconcile(slots,3,old,NULL,ops,error,sizeof(error)));
    CHECK(sh_audio_banks_reconcile(slots,3,old,NULL,ops,error,sizeof(error)));
    CHECK(slots[0].owned && f.load_calls==2);
    f.fail_load=3;
    CHECK(!sh_audio_banks_reconcile(slots,3,next,NULL,ops,error,sizeof(error)));
    CHECK(f.loaded[1] && f.loaded[2] && !f.loaded[3]);
    f.fail_load=0;
    CHECK(sh_audio_banks_reconcile(slots,3,old,NULL,ops,error,sizeof(error)));
    CHECK(f.loaded[1] && !f.loaded[2] && !f.loaded[3]);
    CHECK(sh_audio_banks_reconcile(slots,3,all,NULL,ops,error,sizeof(error)));
    f.fail_unload=2;
    CHECK(!sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(!f.loaded[1] && f.loaded[2] && f.loaded[3]);
    f.fail_unload=0;
    CHECK(sh_audio_banks_reconcile(slots,3,all,NULL,ops,error,sizeof(error)));
    CHECK(f.loaded[1] && f.loaded[2] && f.loaded[3]);
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(!f.loaded[1] && !f.loaded[2] && !f.loaded[3]);
    reset(slots,&f); f.wrong_id=1;
    CHECK(!sh_audio_banks_reconcile(slots,3,old,NULL,ops,error,sizeof(error)));
    CHECK(slots[0].owned && slots[0].loaded_id==4);
    f.wrong_id=0;
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(!f.loaded[4]);
    reset(slots,&f); f.uncertain=1;
    CHECK(!sh_audio_banks_reconcile(slots,3,old,NULL,ops,error,sizeof(error)));
    CHECK(slots[0].uncertain);
    CHECK(!sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(f.load_calls==1 && f.unload_calls==0); /* Do not certify unknown state. */
    reset(slots,&f); slots[2].id=1;
    CHECK(!sh_audio_banks_reconcile(slots,3,all,NULL,ops,error,sizeof(error)));
    CHECK(f.load_calls==0);
    CHECK(!sh_audio_banks_reconcile(NULL,3,all,NULL,ops,error,sizeof(error)));
    CHECK(!sh_audio_banks_reconcile(slots,3,NULL,NULL,ops,error,sizeof(error)));
}

/* Voices survive what the transaction does not touch: an unchanged bank is
 * never cycled, and a bank still wanted is never released to satisfy another.
 * Every load precedes every release, so a dependency is resident before the
 * obsolete bank holding the previous one goes away. */
static char order[64];
static size_t order_length;
static int order_load(void *context, const char *name, uint32_t *id)
{
    int result = load(context, name, id);
    if (order_length + 1 < sizeof(order)) order[order_length++] = name[0];
    return result;
}
static int order_unload(void *context, uint32_t id)
{
    int result = unload(context, id);
    if (order_length + 1 < sizeof(order)) order[order_length++] = (char)('A' + (int)id - 1);
    return result;
}
static void test_transition_order(void)
{
    fake_audio f; sh_audio_bank_slot slots[3]; char error[256];
    const unsigned char first[]={1,0,0}, second[]={0,1,1}, empty[]={0,0,0};
    sh_audio_bank_ops ops={&f,order_load,order_unload};
    reset(slots,&f); order_length=0;
    CHECK(sh_audio_banks_reconcile(slots,3,first,NULL,ops,error,sizeof(error)));
    CHECK(sh_audio_banks_reconcile(slots,3,second,NULL,ops,error,sizeof(error)));
    order[order_length]=0;
    CHECK(!strcmp(order,"abcA")); /* Loads first, then the obsolete release. */
    order_length=0;
    CHECK(sh_audio_banks_reconcile(slots,3,second,NULL,ops,error,sizeof(error)));
    order[order_length]=0;
    CHECK(!strcmp(order,"bc")); /* Already-loaded banks are rechecked, not cycled. */
    CHECK(f.unload_calls==1);
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
}

static void test_content_recovery(void)
{
    fake_audio f; sh_audio_bank_slot slots[3]; char error[256];
    const unsigned char empty[]={0,0,0}, wanted[]={1,0,0};
    sh_package_file_identity first={45,{1}}, second={45,{2}};
    const sh_package_file_identity *content[]={&first,NULL,NULL};
    sh_audio_bank_ops ops={&f,load,unload};
    reset(slots,&f); f.loaded[1]=1; f.actual[1]=7; f.desired[1]=1;
    CHECK(sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(f.actual[1]==1 && f.load_calls==2 && f.unload_calls==1);
    CHECK(slots[0].owned && slots[0].restore_native && slots[0].content_known);
    CHECK(sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(f.unload_calls==1); /* Identical compiled bytes do not reload. */
    content[0]=&second; f.desired[1]=2;
    CHECK(sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(f.actual[1]==2 && f.unload_calls==2 && slots[0].restore_native);
    f.desired[1]=7; /* The previous provider has been republished. */
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(f.loaded[1] && f.actual[1]==7 && !slots[0].owned && !slots[0].restore_native);
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(f.loaded[1] && f.unload_calls==3);

    /* A load failure AFTER native displacement still owes native restoration. */
    reset(slots,&f); content[0]=&first; f.loaded[1]=1; f.desired[1]=1;
    f.fail_on_load_call=2;
    CHECK(!sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(!f.loaded[1] && !slots[0].owned && slots[0].restore_native);
    f.desired[1]=7; f.fail_on_load_call=0;
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(f.loaded[1] && f.actual[1]==7 && !slots[0].owned && !slots[0].restore_native);

    /* Product-owned banks must recover the previous override, then retire. */
    reset(slots,&f); f.desired[1]=1;
    CHECK(sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(!slots[0].restore_native);
    content[0]=&second; f.desired[1]=2; f.fail_load=1;
    CHECK(!sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(!f.loaded[1] && !slots[0].content_known);
    content[0]=&first; f.desired[1]=1; f.fail_load=0;
    CHECK(sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(f.actual[1]==1 && slots[0].owned);
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    CHECK(!f.loaded[1]);

    /* A known absent native entry permits restoration; an uncertain one does not. */
    reset(slots,&f); f.desired[1]=1;
    CHECK(sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    f.loaded[1]=0; f.missing_unload=1; content[0]=&second; f.desired[1]=2;
    CHECK(sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(f.loaded[1] && f.actual[1]==2);
    f.missing_unload=0; f.fail_unload=1; content[0]=&first;
    CHECK(!sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(f.actual[1]==2 && slots[0].content_known && slots[0].content.digest[0]==2);
    f.fail_unload=0; content[0]=&second;
    CHECK(sh_audio_banks_reconcile(slots,3,wanted,content,ops,error,sizeof(error)));
    CHECK(sh_audio_banks_reconcile(slots,3,empty,NULL,ops,error,sizeof(error)));
    reset(slots,&f);
    CHECK(!sh_audio_banks_reconcile(slots,3,empty,content,ops,error,sizeof(error)));
    CHECK(!f.load_calls && !f.unload_calls);
}

int main(int argc,char **argv)
{
    test_identities(); test_name_aliases(); test_reader(); test_dependencies();
    test_streaming_source(); test_recovery(); test_transition_order(); test_content_recovery();
    if(argc>1 && !strcmp(argv[1],"--sound-ids")) {
        for(int i=2;i<argc;i++) {
            uint32_t id=0;
            CHECK(sh_audio_event_identity(argv[i],&id));
            printf("event\t%u\t%s\n",id,argv[i]);
        }
        return failures?1:0;
    }
    for(int i=1;i<argc;++i) {
        FILE *f=NULL; sh_audio_bank bank={0}; char error[256];
        CHECK(!fopen_s(&f,argv[i],"rb") && f);
        if(f) {
            int result=sh_audio_bank_read(f,&bank,error,sizeof(error));
            CHECK(result==1);
            if(result==1) {
                const char *name=argv[i];
                for(const char *p=name;*p;p++) if(*p=='/' || *p=='\\') name=p+1;
                CHECK(sh_audio_bank_names_identity(name,bank.id));
                printf("bank %u: %zu events %zu banks %zu required %zu optional %zu media\n",
                    bank.id,bank.event_count,bank.bank_count,bank.required_count,
                    bank.optional_count,bank.media_count);
            }
            else fprintf(stderr,"%s: %s\n",argv[i],error);
            sh_audio_bank_free(&bank); fclose(f);
        }
    }
    if(failures) return 1;
    puts("audio bank tests passed"); return 0;
}
