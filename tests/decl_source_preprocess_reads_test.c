#define main sh_dsp_common_test_main
#include "decl_default_reads_test.c"
#undef main
#include "../src/backend/decl_source_file_reads.h"
static int dsp_file_ok=1,dsp_diagnostic_ok=1,dsp_file_calls,dsp_diagnostic_calls;
int sh_decl_source_file_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{(void)in;dsp_file_calls++;if(!dsp_file_ok){r->reason=SH_DEFAULT_READ_CALLBACK;return 0;}return 1;}
int sh_decl_diagnostic_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in,unsigned severity)
{(void)in;CHECK(severity==1 || severity==4);dsp_diagnostic_calls++;if(!dsp_diagnostic_ok){r->reason=SH_DEFAULT_READ_CALLBACK;return 0;}return 1;}
#include "../src/backend/decl_source_preprocess_reads.c"
#include "native_pin_fixture.h"
static int dsp_start(fixture *f,sh_resource_default_read_receipt *r)
{CHECK(!sh_resource_default_read_begin(r,&f->input));return sh_decl_source_preprocess_read_check(r,&f->input);}
static void dsp_setup(fixture *f,int renderer)
{
    setup(f,renderer);
    for(unsigned i=0;i<dsp_pin_counts[renderer];i++){
        const dsp_pin *pin=dsp_pins[renderer]+i;CHECK(sh_native_pin_fixture_copy(renderer,pin->rva,f->image+pin->rva,pin->length));
    }
    dsp_file_ok=dsp_diagnostic_ok=1;dsp_file_calls=dsp_diagnostic_calls=0;
}
static void dsp_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;dsp_setup(&f,renderer);
    CHECK(dsp_start(&f,&r) && !r.complete && r.state==1 && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    CHECK(dsp_file_calls==1 && dsp_diagnostic_calls==2);
    /* Empty and populated define storage are both admitted. Names that shadow
     * builtins are data; the Source parser, not this predicate, interprets them. */
    uintptr_t first=(uintptr_t)f.image+0x70000,second=(uintptr_t)f.image+0x70100;
    ptr(f.image,ddp_define_heads[renderer],first);ptr((void *)first,0x10,second+0x80);
    strcpy((char *)(second+0x80),"vertex_fragment_global_include");ptr((void *)first,0x50,second);
    ptr((void *)second,0x10,first+0x80);strcpy((char *)(first+0x80),"texture");ptr((void *)second,0x50,0);
    CHECK(dsp_start(&f,&r));
    ptr((void *)second,0x50,first);CHECK(!dsp_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);ptr((void *)second,0x50,0);
    f.denied=first+0x50;CHECK(!dsp_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    f.change_at=(uintptr_t)f.image+ddp_define_heads[renderer];CHECK(!dsp_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);f.change_at=0;
    ptr(f.image,ddp_define_heads[renderer],first);CHECK(dsp_start(&f,&r));
    for(unsigned i=0;i<dsp_pin_counts[renderer];i++){
        const dsp_pin *pin=dsp_pins[renderer]+i;size_t offsets[]={0,pin->length/2,pin->length-1};
        for(unsigned j=0;j<3;j++){
            unsigned char *at=f.image+pin->rva+offsets[j];*at^=1;
            CHECK(!dsp_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*at^=1;
        }
    }
    dsp_file_ok=0;CHECK(!dsp_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);dsp_file_ok=1;
    dsp_diagnostic_ok=0;CHECK(!dsp_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);dsp_diagnostic_ok=1;
    const dpa_profile *allocator=dpa_profiles+renderer;
    ptr(f.image,allocator->instance+0x38,first);CHECK(!dsp_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,allocator->instance+0x38,0);
    CHECK(dsp_start(&f,&r));f.input.work_limit=r.work-1;CHECK(!dsp_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(dsp_start(&f,&r));f.input.resource++;CHECK(!sh_decl_source_preprocess_read_check(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.resource--;
    CHECK(dsp_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_decl_source_preprocess_read_check(&r,&f.input));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void dsp_image_case(const char *path,int renderer)
{
    FILE *file=fopen(path,"rb");CHECK(file);if(!file)return;
    fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);unsigned char *raw=malloc(size);CHECK(raw);
    if(!raw){fclose(file);return;}CHECK(fread(raw,1,size,file)==(size_t)size);fclose(file);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<dsp_pin_counts[renderer];i++){
        const dsp_pin *pin=dsp_pins[renderer]+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){
            IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData &&
               pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){
                size_t off=s->PointerToRawData+pin->rva-s->VirtualAddress;
                CHECK(off<=(size_t)size && pin->length<=(size_t)size-off);
                if(off<=(size_t)size && pin->length<=(size_t)size-off){sh_native_pin_hash hash;unsigned char digest[32];
                    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,raw+off,pin->length);sh_native_pin_hash_finish(&hash,digest);CHECK(!memcmp(digest,pin->digest,32));}found=1;break;
            }
        }
        CHECK(found);
    }
    free(raw);
}
int main(int argc,char **argv)
{if(argc==3){int renderer=atoi(argv[2])!=0;dsp_image_case(argv[1],renderer);dsp_cases(renderer);}else{dsp_cases(0);dsp_cases(1);}printf("Source preprocessing adjunct: %d failure(s)\n",failures);return failures?1:0;}
