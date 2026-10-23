/* Actual generic invocation, shared allocator and CRT checks compose here. */
#ifndef SH_MD6_RELEASE_NO_COMMON
#define main sh_md6_common_test_main
#include "decl_default_reads_test.c"
#undef main
#endif
static int m6r_fail_allocation;
static void *m6r_test_realloc(void *p,size_t size)
{if(m6r_fail_allocation){m6r_fail_allocation=0;return NULL;}return realloc(p,size);}
#define realloc m6r_test_realloc
#include "../src/backend/decl_md6_release_reads.c"
#undef realloc
static uintptr_t md6_at(fixture *f,size_t offset) {return (uintptr_t)f->image+offset;}
static void md6_list(fixture *f,size_t header,size_t data,int count,int capacity,int flags)
{
    ptr(f->image,header,data?md6_at(f,data):0);number(f->image,header+8,count);number(f->image,header+0xc,capacity);
    f->image[header+0x13]=(unsigned char)flags;
}
static int md6_start(fixture *f,sh_resource_default_read_receipt *r)
{CHECK(!sh_resource_default_read_begin(r,&f->input));return sh_decl_md6_release_read_check(r,&f->input);}
static void md6_bind(fixture *f,int renderer)
{
    f->input.resource=md6_at(f,0x10000);
    ptr(f->image,0x10000,(uintptr_t)f->table);
    const m6r_profile *p=m6r_profiles+renderer;
    for(unsigned i=0;i<23;i++)memcpy(f->image+p->pins[i].rva,p->pins[i].bytes,p->pins[i].length);
    ptr(f->table,0xa0,md6_at(f,p->pins[0].rva));
    ptr(f->image,0x10248,md6_at(f,p->sentinel));ptr(f->image,0x10250,md6_at(f,p->sentinel));
    ptr(f->image,0x102c0,md6_at(f,p->sentinel));
}
static void md6_setup(fixture *f,int renderer)
{setup(f,renderer);md6_bind(f,renderer);}
static void md6_populated(fixture *f)
{
    ptr(f->image,0x10060,0xdeadbeef);ptr(f->image,0x10098,0xcafebabe); /* Native clears, never destroys these objects here. */
    ptr(f->image,0x10078,md6_at(f,0x93000));number(f->image,0x10070,7);number(f->image,0x10080,0x40000010);
    md6_list(f,0x100c0,0x30000,1,2,0);ptr(f->image,0x30000,md6_at(f,0x31000));ptr(f->image,0x30008,0xdeadbeef);
    md6_list(f,0x31010,0x32000,1,2,0);md6_list(f,0x31028,0x33000,1,2,3);md6_list(f,0x31040,0x34000,1,2,4);
    for(unsigned i=0;i<13;i++)md6_list(f,0x100d8+i*0x18,0x20000+i*0x1000,1,3,0);
    md6_list(f,0x10210,0x40000,1,2,0);
    md6_list(f,0x40130,0x42000,1,2,0);md6_list(f,0x40278,0x43000,0,1,0);
    ptr(f->image,0x40000,0xdeadbeef);ptr(f->image,0x40010,md6_at(f,0x44000));number(f->image,0x40018,0x40000008);
    ptr(f->image,0x40158,md6_at(f,0x40178));number(f->image,0x40160,0x40000100); /* Static inline buffer detached before direct destructor. */
    md6_list(f,0x10230,0x50000,0,2,0);
    ptr(f->image,0x50000,0xdeadbeef);ptr(f->image,0x50020,0xcafebabe); /* Empty atom reset never reads the former name. */
    md6_list(f,0x50008,0x51000,1,2,0);md6_list(f,0x50028,1,0,1,1);
    ptr(f->image,0x51020,md6_at(f,0x52000));ptr(f->image,0x51060,md6_at(f,0x53000));
    ptr(f->image,0x51030,md6_at(f,0x54008));ptr(f->image,0x54000,3);
    md6_list(f,0x10278,0x60000,1,2,0);ptr(f->image,0x60000,0xdeadbeef);
    ptr(f->image,0x60010,md6_at(f,0x62000));number(f->image,0x60018,0x40000008);
    md6_list(f,0x10290,0x70000,1,2,0);ptr(f->image,0x70020,md6_at(f,0x71000));ptr(f->image,0x70048,md6_at(f,0x72000));
    md6_list(f,0x102a8,0x80000,1,2,0);md6_list(f,0x80010,0x81000,1,2,0);md6_list(f,0x80038,0x82000,1,3,4);
    ptr(f->image,0x10248,md6_at(f,0x90000));ptr(f->image,0x10250,md6_at(f,0x91000));
    ptr(f->image,0x102c0,md6_at(f,0x92000));number(f->image,0x102d0,16);
}
static void md6_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;md6_setup(&f,renderer);
    const m6r_profile *p=m6r_profiles+renderer;
    CHECK(md6_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    md6_populated(&f);CHECK(md6_start(&f,&r));
    for(unsigned i=0;i<23;i++)for(unsigned j=0;j<p->pins[i].length;j++) {
        f.image[p->pins[i].rva+j]^=1;CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
        f.image[p->pins[i].rva+j]^=1;
    }
    const size_t headers[]={0x10210,0x10230,0x10278,0x10290,0x102a8};
    for(unsigned flags=0;flags<256;flags++) {
        for(unsigned i=0;i<5;i++)f.image[headers[i]+0x13]=(unsigned char)flags;
        f.image[0x100d3]=(unsigned char)flags;
        f.image[0x100d8+0x13]=(unsigned char)flags;f.image[0x50008+0x13]=(unsigned char)flags;
        CHECK(md6_start(&f,&r));
    }
    for(unsigned i=0;i<5;i++)f.image[headers[i]+0x13]=0;
    f.image[0x100d3]=0;
    f.image[0x100d8+0x13]=0;f.image[0x50008+0x13]=0;
    f.denied=md6_at(&f,0x40278);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);
    f.image[0x10223]=1;CHECK(md6_start(&f,&r));f.image[0x10223]=0;f.denied=0;
    f.denied=md6_at(&f,0x30008);CHECK(md6_start(&f,&r));f.denied=0;
    number(f.image,0x100c8,2);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);number(f.image,0x100c8,1);
    f.image[0x100d3]=1;f.denied=md6_at(&f,0x31010);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);
    f.image[0x100d3]=0;f.denied=0;
    ptr(f.image,0x30000,md6_at(&f,0x10000));CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr(f.image,0x30000,md6_at(&f,0x31000));ptr(f.image,0x100f0,md6_at(&f,0x20000));
    CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);ptr(f.image,0x100f0,md6_at(&f,0x21000));
    ptr(f.image,0x54000,UINT64_MAX);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);ptr(f.image,0x54000,3);
    f.denied=md6_at(&f,0x54000);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    ptr(f.image,0x51030,7);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);ptr(f.image,0x51030,md6_at(&f,0x54008));
    ptr(f.image,0x54000,0);CHECK(md6_start(&f,&r));ptr(f.image,0x54000,3);
    number(f.image,0x102d0,-1);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);
    ptr(f.image,0x102c0,md6_at(&f,p->sentinel));CHECK(md6_start(&f,&r));ptr(f.image,0x102c0,md6_at(&f,0x92000));number(f.image,0x102d0,16);
    ptr(f.image,0x102c0,md6_at(&f,0x90000));CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr(f.image,0x102c0,md6_at(&f,0x92000));
    number(f.image,0x1021c,0);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.image,0x1021c,2);
    m6r_fail_allocation=1;CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_ALLOCATION);
    CHECK(md6_start(&f,&r));f.input.work_limit=r.work-1;CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    ptr(f.table,0xa0,md6_at(&f,p->pins[0].rva)+1);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    ptr(f.table,0xa0,md6_at(&f,p->pins[0].rva));
    const dpa_profile *allocator=dpa_profiles+renderer;uintptr_t old;
    memcpy(&old,f.image+allocator->table+0x10,8);ptr(f.image,allocator->table+0x10,old+1);
    CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,allocator->table+0x10,old);
    number(f.image,dpa_crt_profiles[renderer].mode,1);CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    number(f.image,dpa_crt_profiles[renderer].mode,0);
    f.input.image_size=0x1000;CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_IMAGE);f.input.image_size=f.size;
    f.input.pinned_image=0;CHECK(!md6_start(&f,&r) && r.reason==SH_DEFAULT_READ_IMAGE);f.input.pinned_image=1;
    CHECK(md6_start(&f,&r));f.input.read_context=NULL;
    CHECK(!sh_decl_md6_release_read_check(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.read_context=&f;
    CHECK(md6_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1)); /* Helper never completes the caller's parser. */
    CHECK(md6_start(&f,&r));r.complete=1;r.reason=SH_DEFAULT_READ_OK;
    CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(md6_start(&f,&r));r.complete=1;r.reason=SH_DEFAULT_READ_OK;
    CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    CHECK(md6_start(&f,&r));r.complete=1;r.reason=SH_DEFAULT_READ_OK;
    CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void md6_image_case(const char *path,int renderer)
{
    image_case(path,renderer);FILE *file=fopen(path,"rb");CHECK(file);if(!file)return;
    fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);unsigned char *raw=malloc(length);CHECK(raw);
    if(!raw){fclose(file);return;}CHECK(fread(raw,1,length,file)==(size_t)length);fclose(file);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<23;i++) {
        const m6r_pin *pin=m6r_profiles[renderer].pins+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
            IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)) {
                size_t offset=s->PointerToRawData+pin->rva-s->VirtualAddress;
                CHECK(offset<=(size_t)length && pin->length<=(size_t)length-offset && !memcmp(raw+offset,pin->bytes,pin->length));found=1;break;
            }
        }CHECK(found);
    }free(raw);printf("MD6 Release: %s exact native bodies verified\n",renderer?"Vulkan":"OpenGL");
}
#ifndef SH_MD6_RELEASE_FIXTURE_ONLY
int main(int argc,char **argv)
{
    if(argc==3)md6_image_case(argv[1],atoi(argv[2])!=0);else{md6_cases(0);md6_cases(1);}
    printf("MD6 Release: %d failure(s)\n",failures);return failures?1:0;
}
#endif
