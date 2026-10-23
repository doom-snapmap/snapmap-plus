/* Reuse the actual paired generic-envelope binding fixture. */
#define main sh_common_default_test_main
#include "decl_default_reads_test.c"
#undef main
#include "../src/backend/decl_string_default_reads.c"
static int diagnostic_ready,diagnostic_calls;
int sh_decl_diagnostic_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in,unsigned severity)
{
    CHECK(in && severity==4 && !r->complete);diagnostic_calls++;
    if(!diagnostic_ready)r->reason=SH_DEFAULT_READ_CALLBACK;return diagnostic_ready;
}

static uintptr_t string_address(fixture *f,unsigned index)
{ return (uintptr_t)f->image+sdr_pins[f->input.renderer][index].rva; }
static int string_start(fixture *f,sh_resource_default_read_receipt *r)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    CHECK(!sh_decl_default_read_begin(r,&f->input));
    if(r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
    return sh_decl_string_default_read_begin(r,&f->input);
}
static void string_cases(int renderer)
{
    fixture f; sh_resource_default_read_receipt r; size_t work;
    const char *names[]={"", "play_example", "STOP_example", "folder/example.wav",
        "Folder\\Example.WAV", "play_folder/example", "play_folder\\example",
        "folder/.hidden", "folder/end/", "PLAY_\xC0"};
    uintptr_t name; static const uint32_t suppress[]={0x4b21b00,0x62237f0};
    setup(&f,renderer); name=(uintptr_t)f.image+0x3000;
    for(unsigned i=0;i<SDR_PIN_COUNT;i++)
        memcpy((void *)string_address(&f,i),sdr_pins[renderer][i].bytes,sdr_pins[renderer][i].length);
    ptr(f.table,0x98,string_address(&f,SDR_PARSER));
    ptr(f.table,0xa0,string_address(&f,SDR_RELEASE));
    ptr(f.object,8,name);
    for(unsigned i=0;i<sizeof names/sizeof names[0];i++) {
        strcpy((char *)name,names[i]);
        CHECK(string_start(&f,&r) && r.complete && r.routine==SH_DEFAULT_READ_ROUTINE_DECLARATION);
        CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
        CHECK(!sh_decl_string_default_read_begin(&r,&f.input));
        CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    }
    /* This FNV-1 preimage follows the existing play_ branch and reaches the
     * native level-4 diagnostic; a recognized parser alone cannot certify it. */
    strcpy((char *)name,"play_xsoanfw");
    CHECK(!string_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);
    diagnostic_ready=1;int calls=diagnostic_calls;CHECK(string_start(&f,&r) && diagnostic_calls==calls+1);
    diagnostic_ready=0;
    number(f.image,suppress[renderer],1); CHECK(string_start(&f,&r));
    f.denied=(uintptr_t)f.image+suppress[renderer];
    CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
    number(f.image,suppress[renderer],0);
    strcpy((char *)name,"folder/xsoanfw.wav");
    CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    strcpy((char *)name,"play_xsoanfw");
    memset((void *)(name+12),'a',600); *((char *)name+612)=0;
    CHECK(string_start(&f,&r)); /* Native fixed destination remains empty. */
    memset((void *)name,'a',255); *((char *)name+255)=0; CHECK(string_start(&f,&r));
    *((char *)name+255)='a'; *((char *)name+256)=0; CHECK(string_start(&f,&r));
    ptr(f.object,8,0); CHECK(string_start(&f,&r)); ptr(f.object,8,name);
    strcpy((char *)name,"play_example"); CHECK(string_start(&f,&r)); work=r.work;
    f.input.work_limit=work-1;
    CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET); f.input.work_limit=0;
    f.denied=name+3; CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
    f.denied=(uintptr_t)f.object+8;
    CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
    for(unsigned i=0;i<SDR_V_PARSER;i++) for(unsigned j=0;j<sdr_pins[renderer][i].length;j++) {
        *(unsigned char *)(string_address(&f,i)+j)^=1;
        CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN && !r.complete);
        *(unsigned char *)(string_address(&f,i)+j)^=1;
    }
    ptr(f.table,0xa0,string_address(&f,SDR_RELEASE)+1); CHECK(!string_start(&f,&r));
    ptr(f.table,0xa0,string_address(&f,SDR_RELEASE));
    f.object[0x60]=1; CHECK(!string_start(&f,&r)); f.object[0x60]=0;
    CHECK(string_start(&f,&r)); CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(string_start(&f,&r)); CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    CHECK(!sh_resource_default_read_begin(&r,&f.input));
    f.input.read_context=NULL;
    CHECK(!sh_decl_string_default_read_begin(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);
    f.input.read_context=&f;
    f.input.pinned_image=0; CHECK(!string_start(&f,&r)); f.input.pinned_image=1;
    f.input.renderer=1-renderer; CHECK(!string_start(&f,&r)); f.input.renderer=renderer;
    f.input.image_size=0x1000; CHECK(!string_start(&f,&r)); f.input.image_size=f.size;
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void string_image_case(const char *path,int renderer)
{
    FILE *file=fopen(path,"rb"); unsigned char *raw; long length;
    IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *section;
    image_case(path,renderer);
    CHECK(file); if(!file)return;
    fseek(file,0,SEEK_END); length=ftell(file); rewind(file);
    raw=malloc(length); CHECK(raw); if(!raw){fclose(file);return;}
    CHECK(fread(raw,1,length,file)==(size_t)length); fclose(file);
    nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    section=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<SDR_PIN_COUNT;i++) {
        const sdr_pin *pin=&sdr_pins[renderer][i]; int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
            size_t rva=section[j].VirtualAddress,size=section[j].SizeOfRawData;
            if(pin->rva>=rva && pin->rva-rva<=size && pin->length<=size-(pin->rva-rva)) {
                size_t offset=section[j].PointerToRawData+(pin->rva-rva);
                CHECK(offset<=(size_t)length && pin->length<=(size_t)length-offset);
                CHECK(!memcmp(raw+offset,pin->bytes,pin->length)); found=1; break;
            }
        }
        CHECK(found);
    }
    {
        const sdr_pin *pin=&sdr_pins[renderer][SDR_V_TEXT]; int32_t relative; int found=0;
        int64_t target; memcpy(&relative,pin->bytes+3,4); target=(int64_t)pin->rva+7+relative;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
            size_t rva=section[j].VirtualAddress,size=section[j].SizeOfRawData;
            if(target>=0 && (uint64_t)target>=rva && (uint64_t)target-rva<=size &&
               sizeof sdr_viseme_definition<=size-((uint64_t)target-rva)) {
                size_t offset=section[j].PointerToRawData+((size_t)target-rva);
                CHECK(!memcmp(raw+offset,sdr_viseme_definition,sizeof sdr_viseme_definition)); found=1; break;
            }
        }
        CHECK(found);
    }
    free(raw);
}
static void viseme_cases(int renderer)
{
    fixture f; sh_resource_default_read_receipt r; int32_t relative; uintptr_t text,node,name;
    const char *names[]={"phonemeSet","visemeSilence","viseme_REST","phonemeSilence","x","visemes","phonemes"};
    setup(&f,renderer);
    for(unsigned i=0;i<SDR_PIN_COUNT;i++)
        memcpy((void *)string_address(&f,i),sdr_pins[renderer][i].bytes,sdr_pins[renderer][i].length);
    ptr(f.table,0x98,string_address(&f,SDR_V_PARSER));
    ptr(f.table,0xa0,string_address(&f,SDR_V_RELEASE));
    ptr(f.table,0x88,string_address(&f,SDR_V_TEXT));
    memcpy(&relative,(void *)(string_address(&f,SDR_V_TEXT)+3),4);
    text=string_address(&f,SDR_V_TEXT)+7+relative;
    memcpy((void *)text,sdr_viseme_definition,sizeof sdr_viseme_definition);
    CHECK(string_start(&f,&r) && r.complete);
    /* Cleanup never consults an element callback: both native count fields
     * become zero for owned, borrowed and inline storage flags alike. */
    ptr(f.object,0x88,(uintptr_t)f.image+0x4000); number(f.object,0x90,2); number(f.object,0x94,3);
    ptr(f.object,0xa0,(uintptr_t)f.image+0x5000); number(f.object,0xa8,3); number(f.object,0xac,4);
    number(f.object,0x60,7); ptr(f.object,0x68,(uintptr_t)f.image+0x6000); number(f.object,0x70,(int)0xc0000020u);
    for(unsigned flag=0;flag<256;flag++) {
        f.object[0x9b]=(unsigned char)flag; f.object[0xb3]=(unsigned char)(255-flag);
        CHECK(string_start(&f,&r));
    }
    node=(uintptr_t)f.image+0x2000;name=node+0x100;
    ptr(f.image,ddp_define_heads[renderer],node); ptr((void *)node,0x10,name); ptr((void *)node,0x50,0);
    for(unsigned i=0;i<sizeof names/sizeof names[0];i++) {
        strcpy((char *)name,names[i]); CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP && !r.complete);
    }
    strcpy((char *)name,"PHONEMESET"); CHECK(string_start(&f,&r));
    strcpy((char *)name,"phonemeSet_extra"); CHECK(string_start(&f,&r));
    ptr(f.image,ddp_define_heads[renderer],0);
    for(unsigned i=SDR_V_PARSER;i<SDR_PIN_COUNT;i++) for(unsigned j=0;j<sdr_pins[renderer][i].length;j++) {
        *(unsigned char *)(string_address(&f,i)+j)^=1;
        CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        *(unsigned char *)(string_address(&f,i)+j)^=1;
    }
    *(char *)(text+sizeof sdr_viseme_definition-2)='x';
    CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
    memcpy((void *)text,sdr_viseme_definition,sizeof sdr_viseme_definition);
    f.denied=text; CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
    CHECK(string_start(&f,&r)); CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_decl_string_default_read_begin(&r,&f.input));
    f.input.work_limit=1; CHECK(!string_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);
    VirtualFree(f.image,0,MEM_RELEASE);
}
int main(int argc,char **argv)
{
    if(argc>2)string_image_case(argv[1],atoi(argv[2])!=0);
    else {string_cases(0);string_cases(1);viseme_cases(0);viseme_cases(1);}
    printf("string declaration defaults: %d failure(s)\n",failures); return failures?1:0;
}
