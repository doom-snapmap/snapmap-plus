#define SH_MD6_RELEASE_FIXTURE_ONLY
#ifdef SH_MD6_DEFAULT_FIXTURE_ONLY
#define SH_MD6_RELEASE_NO_COMMON
#endif
#include "decl_md6_release_reads_test.c"
#undef SH_MD6_RELEASE_FIXTURE_ONLY
#undef SH_MD6_RELEASE_NO_COMMON
#include "../src/backend/decl_md6_default_reads.c"
#include "native_pin_fixture.h"
static void mdp_fixture(int renderer,uint32_t rva,void *out,size_t length)
{ if(!sh_native_pin_fixture_copy(renderer,rva,out,length))abort(); }

#ifndef SH_MD6_DEFAULT_FIXTURE_ONLY
static int mdp_ready=1,mdp_diagnostic=1;
int sh_resource_md6_reads_ready(void) { return mdp_ready; }
int sh_decl_diagnostic_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in,unsigned severity)
{ CHECK(in && severity==4 && !r->complete);if(!mdp_diagnostic)r->reason=SH_DEFAULT_READ_CALLBACK;return mdp_diagnostic; }
#endif
static uintptr_t mdp_test_at(fixture *f,unsigned index)
{ return (uintptr_t)f->image+mdp_pins[f->input.renderer][index].rva; }
static uintptr_t mdp_bind(fixture *f,int renderer)
{
    md6_bind(f,renderer);
    for(unsigned i=0;i<MDP_COUNT;i++)mdp_fixture(renderer,mdp_pins[renderer][i].rva,(void *)mdp_test_at(f,i),mdp_pins[renderer][i].length);
    ptr(f->table,0x98,mdp_test_at(f,MDP_PARSER));ptr(f->table,0x88,mdp_test_at(f,MDP_TEXT));
    int32_t rel;memcpy(&rel,(void *)(mdp_test_at(f,MDP_TEXT)+3),4);
    uintptr_t text=mdp_test_at(f,MDP_TEXT)+7+rel;memcpy((void *)text,mdp_definition,sizeof mdp_definition);
    const uint32_t common[]={0x36fb9d0,0x4df9648},engine[]={0x49103b0,0x60120a0},guard[]={0x4910670,0x6012360},tick[]={0x3e59760,0x55574d0};
    const ddp_context_binding *context=ddp_context_profiles+renderer;
    ptr(f->image,context->game_table+0x250,mdp_test_at(f,MDP_COMMON_EVENT));
    ptr(f->image,context->context_table+0x90,mdp_test_at(f,MDP_ENGINE_EVENT));
    number(f->image,guard[renderer],-2);ptr(f->image,tick[renderer],md6_at(f,0xb3000));number(f->image,0xb3010,30);
    return text;
}
static uintptr_t mdp_setup(fixture *f,int renderer)
{setup(f,renderer);return mdp_bind(f,renderer);}
#ifndef SH_MD6_DEFAULT_FIXTURE_ONLY
static int mdp_start(fixture *f,sh_resource_default_read_receipt *r)
{ CHECK(!sh_resource_default_read_begin(r,&f->input));CHECK(!sh_decl_default_read_begin(r,&f->input));return sh_decl_md6_default_read_begin(r,&f->input); }
static void mdp_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;uintptr_t text=mdp_setup(&f,renderer);
    int initial=mdp_start(&f,&r);if(!initial)fprintf(stderr,"initial renderer%d reason%d at%llx relative%llx work%zu\n",renderer,r.reason,(unsigned long long)r.offending_address,(unsigned long long)(r.offending_address-(uintptr_t)f.image),r.work);
    CHECK(initial && r.complete);CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    /* Populated prior state does not imply a fresh-only parser restriction. */
    md6_populated(&f);int populated=mdp_start(&f,&r);if(!populated)fprintf(stderr,"populated renderer%d reason%d relative%llx work%zu\n",renderer,r.reason,(unsigned long long)(r.offending_address-(uintptr_t)f.image),r.work);CHECK(populated);
    md6_list(&f,0x102f0,0xc0000,1,2,0);md6_list(&f,0xc0008,0xc1000,2,3,0);
    md6_list(&f,0xc0030,0xc2000,1,2,4);CHECK(mdp_start(&f,&r));
    md6_list(&f,0x10340,0xc3000,2,4,0);md6_list(&f,0x103f8,0xc4000,3,5,2);md6_list(&f,0x10410,0xc5000,2,4,1);
    md6_list(&f,0x103d8,0xc6000,1,2,0);ptr(f.image,0xc6010,md6_at(&f,0xc7000));number(f.image,0xc6008,3);number(f.image,0xc6018,0x40000010);strcpy((char *)f.image+0xc7000,"old");
    CHECK(mdp_start(&f,&r));
    for(unsigned flag=0;flag<256;flag++) {
        f.image[0x10303]=(unsigned char)flag;f.image[0x10353]=(unsigned char)flag;
        f.image[0x1040b]=(unsigned char)flag;f.image[0x10423]=(unsigned char)flag;
        CHECK(mdp_start(&f,&r));
    }
    f.image[0x10303]=0;
    f.denied=md6_at(&f,0xc0030);CHECK(!mdp_start(&f,&r));f.image[0x10303]=1;CHECK(mdp_start(&f,&r));f.denied=0;f.image[0x10303]=0;
    /* Reused borrowed props retain numerical child rows while fixed name is
     * rewritten. The native default does not resolve the former atoms. */
    f.image[0x10223]=1;CHECK(mdp_start(&f,&r));number(f.image,0x40018,5);CHECK(!mdp_start(&f,&r));number(f.image,0x40018,0x40000008);
    f.denied=md6_at(&f,0x40130);CHECK(!mdp_start(&f,&r));f.denied=0;f.image[0x10223]=0;
    for(unsigned i=0;i<MDP_COUNT;i++) {
        size_t offsets[]={0,mdp_pins[renderer][i].length/2,mdp_pins[renderer][i].length-1};
        for(unsigned j=0;j<3;j++){size_t at=mdp_pins[renderer][i].rva+offsets[j];f.image[at]^=1;CHECK(!mdp_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);f.image[at]^=1;}
    }
    *(char *)(text+22)^=1;CHECK(!mdp_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*(char *)(text+22)^=1;
    uintptr_t node=md6_at(&f,0xd0000),name=node+0x100;
    ptr(f.image,ddp_define_heads[renderer],node);ptr((void *)node,0x10,name);ptr((void *)node,0x50,0);
    const char *names[]={"init","mesh","userProps","jointGroups","events","aliases","props","prop","eyeInfoCollection"};
    for(unsigned i=0;i<9;i++){strcpy((char *)name,names[i]);CHECK(!mdp_start(&f,&r));}
    strcpy((char *)name,"_info");CHECK(mdp_start(&f,&r));strcpy((char *)name,"md6/default.md6mesh");CHECK(mdp_start(&f,&r));
    ptr(f.image,ddp_define_heads[renderer],0);
    const uint32_t guard[]={0x4910670,0x6012360};
    number(f.image,guard[renderer],1);CHECK(mdp_start(&f,&r));number(f.image,guard[renderer],0x7fffffff);CHECK(mdp_start(&f,&r));
    for(int cold=-1;cold<=0;cold++)for(int event=0;event<=1;event++) {
        ddp_test_engine_state(&f,cold,event);
        CHECK(mdp_start(&f,&r) && r.complete);
        CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    }
    ptr(f.image,der_profiles[renderer].sys,0);CHECK(!mdp_start(&f,&r));
    ddp_test_engine_state(&f,-2,0);
    size_t event_slot=ddp_context_profiles[renderer].context_table+0x90;
    ptr(f.image,event_slot,mdp_test_at(&f,MDP_ENGINE_EVENT)+1);CHECK(!mdp_start(&f,&r));ptr(f.image,event_slot,mdp_test_at(&f,MDP_ENGINE_EVENT));
    mdp_ready=0;CHECK(!mdp_start(&f,&r));mdp_ready=1;mdp_diagnostic=0;CHECK(!mdp_start(&f,&r));mdp_diagnostic=1;
    CHECK(mdp_start(&f,&r));f.input.work_limit=r.work-1;CHECK(!mdp_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(mdp_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(mdp_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    CHECK(mdp_start(&f,&r));CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void mdp_image(const char *path,int renderer)
{
    md6_image_case(path,renderer);FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);unsigned char *raw=malloc(size);CHECK(raw);if(!raw){fclose(f);return;}
    CHECK(fread(raw,1,size,f)==(size_t)size);fclose(f);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<MDP_COUNT;i++){const mdp_pin *pin=&mdp_pins[renderer][i];int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){
                size_t offset=s->PointerToRawData+pin->rva-s->VirtualAddress;CHECK(offset<=(size_t)size && pin->length<=(size_t)size-offset && sh_native_pin_fixture_digest(raw+offset,pin->length,pin->digest));found=1;break;}}
        CHECK(found);}
    free(raw);
}
int main(int argc,char **argv)
{ if(argc==3)mdp_image(argv[1],atoi(argv[2])!=0);else{mdp_cases(0);mdp_cases(1);}printf("MD6 default: %d failure(s)\n",failures);return failures?1:0; }
#endif

