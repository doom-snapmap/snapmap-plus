#define main sh_common_default_test_main
#include "decl_default_reads_test.c"
#undef main
#include "../src/backend/decl_particle_default_reads.c"
static int model_ready=1,metadata_ready=1,diagnostic_ready=1;
int sh_resource_model_reads_default_ready(const char *family) { CHECK(!strcmp(family,"particle"));return model_ready; }
int sh_resource_metadata_reads_ready(void) { return metadata_ready; }
int sh_decl_diagnostic_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in,unsigned severity)
{ CHECK(in && severity==4 && !r->complete);if(!diagnostic_ready)r->reason=SH_DEFAULT_READ_CALLBACK;return diagnostic_ready; }
static const char *const enum_names[]={"none","constant","minmax","curve","curve_scale_bias","curve_use_variance_mod_constant","curve_mod_curve","curve_add_curve","parametricEval","parametricIntegrate","parametricIntegrateMinMax"};
static uintptr_t particle_address(fixture *f,unsigned i) { return (uintptr_t)f->image+pdr_pins[f->input.renderer][i].rva; }
static uintptr_t particle_setup(fixture *f,int renderer)
{
    setup(f,renderer);for(unsigned i=0;i<PDR_PIN_COUNT;i++)memcpy((void *)particle_address(f,i),pdr_pins[renderer][i].bytes,pdr_pins[renderer][i].length);
    ptr(f->table,0x98,particle_address(f,PDR_PARSER));ptr(f->table,0xa0,particle_address(f,PDR_RELEASE));ptr(f->table,0x88,particle_address(f,PDR_TEXT));
    int32_t relative;memcpy(&relative,(void *)(particle_address(f,PDR_TEXT)+3),4);
    uintptr_t text=particle_address(f,PDR_TEXT)+7+relative;memcpy((void *)text,pdr_definition,sizeof pdr_definition);
    const uint32_t tables[]={0x36f9190,0x4df6bf0};
    for(unsigned i=0;i<11;i++) { uintptr_t s=(uintptr_t)f->image+0x3000+i*64;strcpy((char *)s,enum_names[i]);ptr(f->image,tables[renderer]+i*8,s); }
    uintptr_t m=(uintptr_t)f->image+0x5000,vt=m+0x100;const uint32_t holder[]={0x36fb530,0x4df9100};
    const uint32_t dep[]={0x17a4f70,0x17b24b0},parent[]={0x17a5190,0x17b26d0};
    ptr(f->image,holder[renderer],m);ptr((void *)m,0,vt);ptr((void *)vt,0x90,(uintptr_t)f->image+dep[renderer]);ptr((void *)vt,0x98,(uintptr_t)f->image+parent[renderer]);
    return text;
}
static int particle_start(fixture *f,sh_resource_default_read_receipt *r)
{ CHECK(!sh_resource_default_read_begin(r,&f->input));CHECK(!sh_decl_default_read_begin(r,&f->input));return sh_decl_particle_default_read_begin(r,&f->input); }
static void particle_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;uintptr_t text=particle_setup(&f,renderer);
    CHECK(particle_start(&f,&r) && r.complete);CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_decl_particle_default_read_begin(&r,&f.input));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    /* Native release erases counts, not element contents. Borrowed stages
     * retain their storage and GPU flag; coverage includes both material paths. */
    ptr(f.object,0x68,(uintptr_t)f.image+0x6000);number(f.object,0x70,3);number(f.object,0x74,4);
    ptr(f.object,0x80,(uintptr_t)f.image+0x9000);number(f.object,0x88,9);number(f.object,0x8c,10);
    for(unsigned flag=0;flag<256;flag++) { f.object[0x7b]=(unsigned char)flag;f.object[0x93]=(unsigned char)(255-flag);f.image[0x6044]=(unsigned char)flag;CHECK(particle_start(&f,&r)); }
    for(unsigned i=0;i<PDR_PIN_COUNT;i++)for(unsigned j=0;j<pdr_pins[renderer][i].length;j++) {
        *(unsigned char *)(particle_address(&f,i)+j)^=1;CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*(unsigned char *)(particle_address(&f,i)+j)^=1;
    }
    *(char *)(text+65)^=1;CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*(char *)(text+65)^=1;
    f.denied=text;CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    uintptr_t node=(uintptr_t)f.image+0xb000,name=node+0x100;
    ptr(f.image,ddp_define_heads[renderer],node);ptr((void *)node,0x10,name);ptr((void *)node,0x50,0);
    const char *names[]={"material","textures/effects/particles/default","count","time","constant","speedZ","parametricIntegrate"};
    for(unsigned i=0;i<7;i++){strcpy((char *)name,names[i]);CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);}
    strcpy((char *)name,"150.000");CHECK(particle_start(&f,&r));strcpy((char *)name,"effects");CHECK(particle_start(&f,&r));
    strcpy((char *)name,"MATERIAL");CHECK(particle_start(&f,&r));ptr(f.image,ddp_define_heads[renderer],0);
    strcpy((char *)f.image+0x3000,"constant");CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);strcpy((char *)f.image+0x3000,"other");CHECK(particle_start(&f,&r));
    strcpy((char *)f.image+0x3040,"CONSTANT");CHECK(particle_start(&f,&r));
    strcpy((char *)f.image+0x3000+8*64,"parametricIntegrate");CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);strcpy((char *)f.image+0x3000+8*64,"parametricEval");
    f.denied=(uintptr_t)f.image+0x3000+9*64;CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    model_ready=0;CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);model_ready=1;
    metadata_ready=0;CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);metadata_ready=1;
    diagnostic_ready=0;CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);diagnostic_ready=1;
    uintptr_t vt=(uintptr_t)f.image+0x5100,old;memcpy(&old,(void *)(vt+0x98),8);ptr((void *)vt,0x98,old+1);CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr((void *)vt,0x98,old);
    CHECK(particle_start(&f,&r));size_t work=r.work;f.input.work_limit=work-1;CHECK(!particle_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(particle_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(particle_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    CHECK(!sh_resource_default_read_begin(&r,&f.input));f.input.read_context=NULL;CHECK(!sh_decl_particle_default_read_begin(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.read_context=&f;
    VirtualFree(f.image,0,MEM_RELEASE);
}
static const unsigned char *raw_at(const unsigned char *raw,size_t size,uint32_t rva,size_t length)
{
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++)if(rva>=s[i].VirtualAddress && rva-s[i].VirtualAddress<=s[i].SizeOfRawData && length<=s[i].SizeOfRawData-(rva-s[i].VirtualAddress)) {
        size_t off=s[i].PointerToRawData+rva-s[i].VirtualAddress;if(off<=size && length<=size-off)return raw+off;
    }CHECK(0);return NULL;
}
static void particle_image(const char *path,int renderer)
{
    image_case(path,renderer);FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    unsigned char *raw=malloc(size);CHECK(raw);if(!raw){fclose(f);return;}CHECK(fread(raw,1,size,f)==(size_t)size);fclose(f);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    for(unsigned i=0;i<PDR_PIN_COUNT;i++){const pdr_pin *p=&pdr_pins[renderer][i];const unsigned char *data=raw_at(raw,size,p->rva,p->length);CHECK(data && !memcmp(data,p->bytes,p->length));}
    const pdr_pin *getter=&pdr_pins[renderer][PDR_TEXT];int32_t rel;memcpy(&rel,getter->bytes+3,4);uint32_t target=getter->rva+7+rel;
    const unsigned char *data=raw_at(raw,size,target,sizeof pdr_definition);CHECK(data && !memcmp(data,pdr_definition,sizeof pdr_definition));
    const uint32_t tables[]={0x36f9190,0x4df6bf0};
    for(unsigned i=0;i<11;i++) { uint64_t address;data=raw_at(raw,size,tables[renderer]+i*8,8);CHECK(data);if(!data)continue;memcpy(&address,data,8);
      CHECK(address>=nt->OptionalHeader.ImageBase && address-nt->OptionalHeader.ImageBase<=UINT32_MAX);
      data=raw_at(raw,size,(uint32_t)(address-nt->OptionalHeader.ImageBase),strlen(enum_names[i])+1);CHECK(data && !memcmp(data,enum_names[i],strlen(enum_names[i])+1)); }
    free(raw);
}
int main(int argc,char **argv)
{ if(argc==3)particle_image(argv[1],atoi(argv[2]));else{particle_cases(0);particle_cases(1);}if(!failures)puts("decl_particle_default_reads_test: PASS");return failures?1:0; }
