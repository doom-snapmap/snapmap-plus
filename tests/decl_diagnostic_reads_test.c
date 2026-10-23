#define main sh_diagnostic_common_test_main
#include "decl_default_reads_test.c"
#undef main
/* RenderProg admission is exercised by its own composed contract suites. */
int sh_decl_renderprog_default_read_begin(sh_resource_default_read_receipt *r,
                                         const sh_resource_default_read_input *in)
{ (void)r; (void)in; return 0; }
#define SH_MATERIAL_DEFAULT_FIXTURE_ONLY
#include "decl_material_default_reads_test.c"
#undef SH_MATERIAL_DEFAULT_FIXTURE_ONLY
#define SH_MD6_DEFAULT_FIXTURE_ONLY
#include "decl_md6_default_reads_test.c"
#undef SH_MD6_DEFAULT_FIXTURE_ONLY
#include "../src/backend/resource_presentation.h"
static int presentation_ready=1,diagnostic_allocation_failure;
int sh_resource_presentation_ready(void) {return presentation_ready;}
static void *diagnostic_realloc(void *p,size_t size)
{if(diagnostic_allocation_failure){diagnostic_allocation_failure=0;return NULL;}return realloc(p,size);}
#define realloc diagnostic_realloc
#include "../src/backend/decl_diagnostic_reads.c"
#undef realloc
#include "../src/backend/decl_animweb_default_reads.c"
#include "../src/backend/decl_string_default_reads.c"
#include "../src/backend/decl_numeric_default_reads.c"
#include "../src/backend/decl_custom_default_reads.c"
#include "../src/backend/decl_particle_default_reads.c"
/* These observer readiness seams are independently qualified by their own
 * suites. The diagnostic predicate and all six caller bodies below are real. */
int sh_resource_parm_reads_ready(void) {return 1;}
int sh_resource_joint_reads_ready(void) {return 1;}
int sh_resource_model_reads_default_ready(const char *family) {CHECK(!strcmp(family,"particle") || !strcmp(family,"material"));return 1;}
int sh_resource_material_reads_ready(void) {return 1;}
int sh_resource_md6_reads_ready(void) {return 1;}
int sh_resource_metadata_reads_ready(void) {return 1;}
static uintptr_t diag_at(fixture *f,size_t offset) {return (uintptr_t)f->image+offset;}
static uintptr_t diag_address(fixture *f,unsigned kind)
{return diag_at(f,dgr_profiles[f->input.renderer].pins[kind].rva);}
static int diag_start(fixture *f,sh_resource_default_read_receipt *r,unsigned severity)
{CHECK(!sh_resource_default_read_begin(r,&f->input));return sh_decl_diagnostic_read_check(r,&f->input,severity);}
static void diag_list(fixture *f,size_t offset,size_t data,int count,int capacity,int flags)
{
    ptr(f->image,offset,data?diag_at(f,data):0);number(f->image,offset+8,count);number(f->image,offset+0xc,capacity);
    f->image[offset+0x13]=(unsigned char)flags;
}
static void diag_string(fixture *f,size_t offset,const char *text)
{
    ptr(f->image,offset,diag_at(f,0x28000));ptr(f->image,0x28000,diag_address(f,DR_STRING_DELETE));
    number(f->image,offset+8,(int)strlen(text));ptr(f->image,offset+0x10,diag_at(f,offset+0x1c));
    number(f->image,offset+0x18,0x80000014);memcpy(f->image+offset+0x1c,text,strlen(text)+1);
}
static void diag_node(fixture *f,size_t offset,unsigned kind,size_t next,int enabled,int forced,int severity)
{
    ptr(f->image,offset,diag_at(f,offset+0x80));ptr(f->image,offset+0x88,diag_address(f,kind));
    ptr(f->image,offset+8,next?diag_at(f,next):0);f->image[offset+0x10]=(unsigned char)enabled;
    f->image[offset+0x11]=(unsigned char)forced;number(f->image,offset+0x14,severity);
}
static void diag_io_setup(fixture *f)
{
    const dgr_io_profile *io=dgr_io_profiles+f->input.renderer;
    const size_t fs_slots[]={0x20,0x88,0xe0,0x118,0x120};
    const size_t file_slots[]={0,0x18,0x30,0x40,0x58,0xa8};
    const size_t client_slots[]={0,8,0x10,0x18,0x20,0x28,0x38,0x50,0x68};
    for(unsigned k=0;k<io->count;k++)memcpy(f->image+io->pins[k].rva,io->pins[k].bytes,io->pins[k].length);
    ptr(f->image,io->holder,diag_at(f,io->object));ptr(f->image,io->object,diag_at(f,io->table));
    for(unsigned i=0;i<5;i++)ptr(f->image,io->table+fs_slots[i],diag_at(f,io->pins[io->fs[i]].rva));
    for(unsigned k=0;k<3;k++)for(unsigned i=0;i<6;i++)
        ptr(f->image,io->tables[k]+file_slots[i],diag_at(f,io->pins[io->files[k][i]].rva));
    for(unsigned k=0;k<2;k++)for(unsigned i=0;i<9;i++)
        ptr(f->image,io->tables[k+3]+client_slots[i],diag_at(f,io->pins[io->clients[k][i]].rva));
    const size_t bases[]={0x30,0x90,0xc0,0xf0,0x120,0x150};
    for(unsigned i=0;i<6;i++)diag_string(f,io->object+bases[i],"");
}
static void diag_setup(fixture *f,int renderer)
{
    setup(f,renderer);const dgr_profile *p=dgr_profiles+renderer;
    for(unsigned k=0;k<DR_PINS;k++)memcpy((void *)diag_address(f,k),p->pins[k].bytes,p->pins[k].length);
    number(f->image,p->hash_sentinel,-1);
    ptr(f->image,p->head,diag_at(f,0x30000));
    diag_node(f,0x30000,DR_SYS,0x30100,0,1,0);
    diag_node(f,0x30100,DR_CON,0x30200,0,0,1);
    diag_node(f,0x30200,DR_STORE,0x30300,1,0,3);
    diag_node(f,0x30300,DR_FILE_LISTENER,0x30400,0,0,1);
    diag_node(f,0x30400,DR_REFRESH,0x1000008,0,0,1);
    diag_node(f,0x1000008,DR_CONSOLE_THUNK,0,0,1,0);
    ptr(f->image,0x30230,diag_at(f,p->hash_sentinel));ptr(f->image,0x30238,diag_at(f,p->hash_sentinel));
    number(f->image,0x30240,0x400);number(f->image,0x30244,0x400);number(f->image,0x3024c,0x3ff);
    diag_io_setup(f);
}
static void diag_output_file(fixture *f,unsigned kind,unsigned client_kind)
{
    const dgr_io_profile *io=dgr_io_profiles+f->input.renderer;
    memset(f->image+0x50000,0,0x108);
    ptr(f->image,0x30318,diag_at(f,0x50000));ptr(f->image,0x50000,diag_at(f,io->tables[kind]));
    if(kind==2) {
        diag_string(f,0x500a8,"qconsole.log");f->image[0x50008]=1;
        ptr(f->image,0x500f8,diag_at(f,0x56000));
        ptr(f->image,0x56000,diag_at(f,io->tables[client_kind+3]));
    } else {diag_string(f,0x50008,"qconsole.log");diag_string(f,0x50038,"");}
}
static void diag_populated(fixture *f)
{
    const dgr_profile *p=dgr_profiles+f->input.renderer;
    diag_list(f,0x30218,0x60000,1,2,0);diag_string(f,0x60000,"old");diag_string(f,0x60038,"");
    ptr(f->image,0x30230,diag_at(f,0x62000));ptr(f->image,0x30238,diag_at(f,0x63000));
    number(f->image,0x30240,1);number(f->image,0x30244,2);number(f->image,0x3024c,0);
    number(f->image,0x30250,-1);number(f->image,0x62000,0);number(f->image,0x63000,-1);
    number(f->image,p->log_mode,2);ptr(f->image,p->log_name,diag_at(f,0x51000));f->image[0x51000]=0;
    ptr(f->image,0x30318,diag_at(f,0x50000));ptr(f->image,0x50000,diag_at(f,0x52000));
    const unsigned methods[]={DR_FILE_NAME,DR_FILE_DELETE,DR_FILE_WRITE,DR_FILE_FLUSH};
    const size_t slots[]={0x18,0,0x30,0xa8};
    for(size_t i=0;i<4;i++)ptr(f->image,0x52000+slots[i],diag_address(f,methods[i]));
    diag_string(f,0x50008,"qconsole.log");diag_string(f,0x50038,"");
    number(f->image,p->refresh,1);f->image[0x30420]=1;ptr(f->image,0x30418,diag_at(f,0x54000));
    ptr(f->image,0x54000,diag_at(f,0x55000));ptr(f->image,0x550e0,diag_address(f,DR_PRESENT));
    diag_list(f,0x1000000+0x61d2b8,0x70000,1,2,0);
    diag_string(f,0x70000,"queued");diag_string(f,0x70030,"");
}
static void diag_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;diag_setup(&f,renderer);
    const dgr_profile *p=dgr_profiles+renderer;
    CHECK(diag_start(&f,&r,4) && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    CHECK(diag_start(&f,&r,1));CHECK(!diag_start(&f,&r,6) && r.reason==SH_DEFAULT_READ_INPUT);
    ptr(f.image,0x30288,diag_address(&f,DR_STORE)+1);
    CHECK(diag_start(&f,&r,1));CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_CALLBACK);
    number(f.image,p->threshold,5);CHECK(diag_start(&f,&r,4));number(f.image,p->threshold,0);
    ptr(f.image,p->thread,(uint64_t)GetCurrentThreadId()+1);
    CHECK(diag_start(&f,&r,4));f.image[0x30211]=1;
    CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_CALLBACK);
    f.image[0x30211]=0;ptr(f.image,p->thread,0);ptr(f.image,0x30288,diag_address(&f,DR_STORE));
    diag_populated(&f);CHECK(diag_start(&f,&r,4));
    ptr(f.image,0x30288,diag_address(&f,DR_STORE)+1);
    CHECK(!diag_start(&f,&r,1) && r.reason==SH_DEFAULT_READ_CALLBACK); /* File IO can raise returning level4. */
    number(f.image,p->threshold,5);CHECK(diag_start(&f,&r,1));number(f.image,p->threshold,0);
    ptr(f.image,0x30288,diag_address(&f,DR_STORE));
    for(unsigned k=0;k<DR_PINS;k++)for(unsigned j=p->pins[k].skip;j<p->pins[k].length;j++) {
        *(unsigned char *)(diag_address(&f,k)+j)^=1;
        CHECK(!diag_start(&f,&r,k==DR_INFO?1:4) && r.reason==SH_DEFAULT_READ_PIN);
        *(unsigned char *)(diag_address(&f,k)+j)^=1;
    }
    /* Fresh and reopened append must validate the concrete FS factory path,
     * every possible newly constructed file and both native client tables. */
    const dgr_io_profile *io=dgr_io_profiles+renderer;
    ptr(f.image,0x30318,0);CHECK(diag_start(&f,&r,4));
    for(unsigned k=0;k<io->count;k++)for(unsigned j=0;j<io->pins[k].length;j++) {
        f.image[io->pins[k].rva+j]^=1;
        CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_PIN);
        f.image[io->pins[k].rva+j]^=1;
    }
    ptr(f.image,io->table+0x118,diag_at(&f,io->pins[io->fs[3]].rva)+1);
    CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_CALLBACK);diag_io_setup(&f);
    f.image[0x30320]=1;ptr(f.image,io->holder,0);CHECK(diag_start(&f,&r,4));
    f.image[0x30320]=0;diag_io_setup(&f);
    for(unsigned kind=0;kind<3;kind++) {
        diag_output_file(&f,kind,0);CHECK(diag_start(&f,&r,4));
        memcpy(f.image+0x51000,"new.log",8);CHECK(diag_start(&f,&r,4));f.image[0x51000]=0;
    }
    diag_output_file(&f,2,1);CHECK(diag_start(&f,&r,4));
    ptr(f.image,0x56000,diag_at(&f,0x57000));CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_CALLBACK);
    diag_output_file(&f,2,1);ptr(f.image,0x30318,0);
    diag_list(&f,io->mounts+8,0x80000,1,2,0);diag_string(&f,0x80000,"host");diag_string(&f,0x80038,"");
    ptr(f.image,0x80030,diag_at(&f,0x56000));CHECK(diag_start(&f,&r,4));
    ptr(f.image,0x56000,diag_at(&f,0x57000));CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_CALLBACK);
    diag_output_file(&f,2,1);number(f.image,io->mounts+0x10,3);
    CHECK(diag_start(&f,&r,4)); /* Existing matching file does not consult mounts. */
    ptr(f.image,0x30318,0);
    CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    diag_list(&f,io->mounts+8,0,0,0,0);diag_output_file(&f,0,0);
    presentation_ready=0;CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_CALLBACK);
    f.image[0x30420]=0;CHECK(diag_start(&f,&r,4));f.image[0x30420]=1;presentation_ready=1;
    ptr(f.image,0x550e0,diag_address(&f,DR_PRESENT)+1);
    CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,0x550e0,diag_address(&f,DR_PRESENT));
    ptr(f.image,0x70030,diag_at(&f,0x29000));ptr(f.image,0x29000,diag_address(&f,DR_STRING_DELETE)+1);
    CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_CALLBACK);
    number(f.image,0x1000000+0x61d2c0,0);CHECK(diag_start(&f,&r,4));
    number(f.image,0x1000000+0x61d2c0,1);ptr(f.image,0x70030,diag_at(&f,0x28000));
    f.denied=diag_at(&f,0x70030);CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    number(f.image,0x63000,0);CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    number(f.image,0x63000,-1);number(f.image,0x62000,2);
    CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.image,0x62000,0);
    ptr(f.image,0x60038,diag_at(&f,0x29000));CHECK(diag_start(&f,&r,4)); /* Static WarningList cleanup ignores this vtable. */
    ptr(f.image,0x60038,diag_at(&f,0x28000));
    ptr(f.image,0x1000010,diag_at(&f,0x30000));CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr(f.image,0x1000010,0);
    f.change_at=diag_at(&f,p->head);CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    f.change_at=0;ptr(f.image,p->head,diag_at(&f,0x30000));
    diagnostic_allocation_failure=1;CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_ALLOCATION);
    f.input.work_limit=80;CHECK(!diag_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(diag_start(&f,&r,4));f.input.read_context=NULL;
    CHECK(!sh_decl_diagnostic_read_check(&r,&f.input,4) && r.reason==SH_DEFAULT_READ_INPUT);f.input.read_context=&f;
    CHECK(diag_start(&f,&r,4));r.complete=1;r.reason=SH_DEFAULT_READ_OK;
    CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(diag_start(&f,&r,4));r.complete=1;r.reason=SH_DEFAULT_READ_OK;
    CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(diag_start(&f,&r,4));r.complete=1;r.reason=SH_DEFAULT_READ_OK;
    CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void diag_image_case(const char *path,int renderer)
{
    image_case(path,renderer);FILE *file=fopen(path,"rb");long length;unsigned char *raw,*image;
    IMAGE_NT_HEADERS64 *nt;IMAGE_SECTION_HEADER *section;
    CHECK(file);if(!file)return;fseek(file,0,SEEK_END);length=ftell(file);rewind(file);raw=malloc(length);
    CHECK(raw);if(!raw){fclose(file);return;}CHECK(fread(raw,1,length,file)==(size_t)length);fclose(file);
    nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);section=IMAGE_FIRST_SECTION(nt);
    image=VirtualAlloc(NULL,nt->OptionalHeader.SizeOfImage,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(image);if(!image){free(raw);return;}
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        CHECK(section[i].PointerToRawData+(size_t)section[i].SizeOfRawData<=(size_t)length);
        CHECK(section[i].VirtualAddress+(size_t)section[i].SizeOfRawData<=nt->OptionalHeader.SizeOfImage);
        memcpy(image+section[i].VirtualAddress,raw+section[i].PointerToRawData,section[i].SizeOfRawData);
    }
    for(unsigned k=0;k<DR_PINS;k++) {
        const dgr_pin *pin=dgr_profiles[renderer].pins+k;
        CHECK(pin->rva+pin->length<=nt->OptionalHeader.SizeOfImage);
        CHECK(!memcmp(image+pin->rva,pin->bytes,pin->length));
    }
    const dgr_io_profile *io=dgr_io_profiles+renderer;
    for(unsigned k=0;k<io->count;k++) {
        const dgr_pin *pin=io->pins+k;
        CHECK(pin->rva+pin->length<=nt->OptionalHeader.SizeOfImage);
        CHECK(!memcmp(image+pin->rva,pin->bytes,pin->length));
    }
    VirtualFree(image,0,MEM_RELEASE);free(raw);
    printf("Diagnostic reads: %s native pins verified\n",renderer?"Vulkan":"OpenGL");
}
static void diag_caller_literal(fixture *f,uintptr_t getter,const void *literal,size_t length)
{
    int32_t relative;memcpy(&relative,(void *)(getter+3),4);
    memcpy((void *)(getter+7+relative),literal,length);
}
static int diag_caller_start(fixture *f,sh_resource_default_read_receipt *r,unsigned kind)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    CHECK(!sh_decl_default_read_begin(r,&f->input));
    if(r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    (void)kind;
    return sh_decl_custom_default_read_begin(r,&f->input);
}
static void diag_composition_cases(int renderer)
{
    for(unsigned kind=0;kind<6;kind++) {
        fixture f;sh_resource_default_read_receipt r;diag_setup(&f,renderer);diag_populated(&f);
        uintptr_t parser=0,release=0,getter=0;
        if(kind==0) {
            for(unsigned i=0;i<AW_PINS;i++)memcpy(f.image+aw_pins[renderer][i].rva,aw_pins[renderer][i].bytes,aw_pins[renderer][i].length);
            parser=diag_at(&f,aw_pins[renderer][AW_PARSE].rva);release=diag_at(&f,aw_pins[renderer][AW_RELEASE].rva);
            getter=diag_at(&f,aw_pins[renderer][AW_TEXT].rva);diag_caller_literal(&f,getter,aw_definition,sizeof aw_definition);
            f.object[0x15a]=1;
        } else if(kind==1) {
            for(unsigned i=0;i<SDR_PIN_COUNT;i++)memcpy(f.image+sdr_pins[renderer][i].rva,sdr_pins[renderer][i].bytes,sdr_pins[renderer][i].length);
            parser=diag_at(&f,sdr_pins[renderer][SDR_PARSER].rva);release=diag_at(&f,sdr_pins[renderer][SDR_RELEASE].rva);
            getter=address(&f,DDP_TEXT);ptr(f.object,8,diag_at(&f,0x4000));strcpy((char *)f.image+0x4000,"play_xsoanfw");
        } else if(kind==2) {
            for(unsigned i=0;i<CDR_PIN_COUNT;i++)memcpy(f.image+cdr_pins[renderer][i].rva,cdr_pins[renderer][i].bytes,cdr_pins[renderer][i].length);
            parser=diag_at(&f,cdr_pins[renderer][CDR_AF_PARSER].rva);release=diag_at(&f,cdr_pins[renderer][CDR_AF_RELEASE].rva);
            getter=diag_at(&f,cdr_pins[renderer][CDR_AF_TEXT].rva);diag_caller_literal(&f,getter,cdr_af_definition,sizeof cdr_af_definition);
        } else if(kind==3) {
            static const char *const names[]={"none","constant","minmax","curve","curve_scale_bias","curve_use_variance_mod_constant","curve_mod_curve","curve_add_curve","parametricEval","parametricIntegrate","parametricIntegrateMinMax"};
            for(unsigned i=0;i<PDR_PIN_COUNT;i++)memcpy(f.image+pdr_pins[renderer][i].rva,pdr_pins[renderer][i].bytes,pdr_pins[renderer][i].length);
            parser=diag_at(&f,pdr_pins[renderer][PDR_PARSER].rva);release=diag_at(&f,pdr_pins[renderer][PDR_RELEASE].rva);
            getter=diag_at(&f,pdr_pins[renderer][PDR_TEXT].rva);diag_caller_literal(&f,getter,pdr_definition,sizeof pdr_definition);
            const uint32_t rows[]={0x36f9190,0x4df6bf0},holder[]={0x36fb530,0x4df9100};
            const uint32_t dep[]={0x17a4f70,0x17b24b0},parent[]={0x17a5190,0x17b26d0};
            for(unsigned i=0;i<11;i++){strcpy((char *)f.image+0x3000+i*64,names[i]);ptr(f.image,rows[renderer]+i*8,diag_at(&f,0x3000+i*64));}
            ptr(f.image,holder[renderer],diag_at(&f,0x5000));ptr(f.image,0x5000,diag_at(&f,0x5100));
            ptr(f.image,0x5190,diag_at(&f,dep[renderer]));ptr(f.image,0x5198,diag_at(&f,parent[renderer]));
        } else if(kind==4) {
            material_bind(&f,renderer);
            parser=material_address(&f,MDR_PARSE);release=material_address(&f,MDR_RELEASE);
            getter=material_address(&f,MDR_TEXT);
        } else {
            mdp_bind(&f,renderer);
            parser=mdp_test_at(&f,MDP_PARSER);release=mdp_test_at(&f,MDP_RELEASE);
            getter=mdp_test_at(&f,MDP_TEXT);
        }
        ptr(f.table,0x98,parser);ptr(f.table,0xa0,release);ptr(f.table,0x88,getter);
        CHECK(diag_caller_start(&f,&r,kind) && r.complete && r.reason==SH_DEFAULT_READ_OK);
        CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
        /* Every caller reaches a real mutable stock receiver. A foreign
         * callback must prevent completion before native execution. */
        ptr(f.image,0x30288,diag_address(&f,DR_STORE)+1);
        CHECK(!diag_caller_start(&f,&r,kind) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);
        ptr(f.image,0x30288,diag_address(&f,DR_STORE));
        presentation_ready=0;CHECK(!diag_caller_start(&f,&r,kind) && !r.complete);presentation_ready=1;
        CHECK(diag_caller_start(&f,&r,kind));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
        VirtualFree(f.image,0,MEM_RELEASE);
    }
}
#ifndef SH_DIAGNOSTIC_FIXTURE_ONLY
int main(int argc,char **argv)
{
    if(argc>2)diag_image_case(argv[1],atoi(argv[2])!=0);else{diag_cases(0);diag_cases(1);diag_composition_cases(0);diag_composition_cases(1);}
    printf("Diagnostic reads: %d failure(s)\n",failures);return failures?1:0;
}
#endif
