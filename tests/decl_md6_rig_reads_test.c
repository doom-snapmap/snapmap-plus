#include "native_pin_fixture.h"
/* Shared invocation, allocator and CRT predicates are real in this fixture. */
#define main sh_md6_rig_common_test_main
#include "decl_default_reads_test.c"
#undef main
static int mrg_fail_allocation;
static void *mrg_test_realloc(void *p,size_t size)
{if(mrg_fail_allocation){mrg_fail_allocation=0;return NULL;}return realloc(p,size);}
#define realloc mrg_test_realloc
#include "../src/backend/decl_md6_rig_reads.c"
#undef realloc
static uintptr_t rig_at(fixture *f,size_t offset) {return (uintptr_t)f->image+offset;}
static void rig_list(fixture *f,size_t header,size_t data,int count,int capacity,int flags)
{
    ptr(f->image,header,data?rig_at(f,data):0);number(f->image,header+8,count);number(f->image,header+12,capacity);
    f->image[header+19]=(unsigned char)flags;
}
static int rig_start(fixture *f,sh_resource_default_read_receipt *r,int bound)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    if(!sh_decl_md6_rig_read_check(r,&f->input))return 0;
    return !bound || sh_decl_md6_rig_bound_read_check(r,&f->input,rig_at(f,0x40000),rig_at(f,0x41000));
}
static void rig_setup(fixture *f,int renderer)
{
    setup(f,renderer);f->input.resource=rig_at(f,0x10000);ptr(f->image,0x10000,(uintptr_t)f->table);
    const mrg_profile *p=mrg_profiles+renderer;
    for(size_t i=0;i<p->pin_count;i++)CHECK(sh_native_pin_fixture_copy(renderer,p->pins[i].rva,f->image+p->pins[i].rva,p->pins[i].length));
    ptr(f->table,0xa0,rig_at(f,p->release));
    ptr(f->image,0x10060,rig_at(f,0x40000));ptr(f->image,0x40138,rig_at(f,0x41000));ptr(f->image,0x41050,rig_at(f,0x42000));
    rig_list(f,0x10308,0x30000,1,2,0);ptr(f->image,0x30000,rig_at(f,0x31000));ptr(f->image,0x30008,0xdeadbeef);
    ptr(f->image,0x31000,0xdeadbeef); /* Direct idStr cleanup ignores the old setup vtable. */
    ptr(f->image,0x31010,rig_at(f,0x33000));number(f->image,0x31018,0x40000008);
    rig_list(f,0x31038,0x32000,1,2,0);ptr(f->image,0x32000,rig_at(f,0x34000));ptr(f->image,0x32008,0xdeadbeef);
    ptr(f->image,0x34000,rig_at(f,0x35000));ptr(f->image,0x35000,rig_at(f,p->destructors[0]));
    static const int sizes[]={0x3c,0xcc,0x84,0x70,0x2c};
    for(int k=0;k<5;k++) {
        size_t component=0x70000+k*0x100,table=0x60000+k*0x100,type=0x80000+k*0x100,index=0x90000+k*0x100;
        ptr(f->image,0x50000+k*8,rig_at(f,component));ptr(f->image,component,rig_at(f,table));
        ptr(f->image,component+8,rig_at(f,type));ptr(f->image,component+0x10,rig_at(f,index));
        number(f->image,component+0x18,sizes[k]);number(f->image,component+0x20,1);
        ptr(f->image,type,0x100+k);number(f->image,type+0x18,1);number(f->image,index,0);
        const unsigned slots[]={8,0x10,0x18,0x20,0x48};
        for(unsigned j=0;j<5;j++)ptr(f->image,table+slots[j],rig_at(f,p->getters[j]));
        ptr(f->image,table+0x50,rig_at(f,p->initializers[k]));
    }
    rig_list(f,0x420e0,0x50000,5,6,1);ptr(f->image,0x50028,0xdeadbeef);
    number(f->image,0x420d8,-1); /* Priority: first group remains row0. */
    rig_list(f,0x10320,0xa0000,1,2,1);ptr(f->image,0xa0000,rig_at(f,0xa1000));ptr(f->image,0xa0008,0xdeadbeef);
    rig_list(f,0xa1000,0xa2000,1,4,0);
    for(int row=0;row<3;row++) {
        size_t record=0xa2000+row*0x70;
        for(int k=0;k<4;k++)rig_list(f,record+k*0x18,0xb0000+(row*4+k)*0x1000,1,k==3?1024:16,0);
        ptr(f->image,record+0x68,0xdeadbeef); /* Reset/rebound before use. */
    }
}
static void rig_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;rig_setup(&f,renderer);
    const mrg_profile *p=mrg_profiles+renderer;
    CHECK(rig_start(&f,&r,1) && !r.complete && r.state==1 && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    /* All selected body bytes, including every chained unwind tail. */
    for(size_t i=0;i<p->pin_count;i++)for(size_t j=0;j<p->pins[i].length;j++) {
        f.image[p->pins[i].rva+j]^=1;CHECK(!rig_start(&f,&r,0) && r.reason==SH_DEFAULT_READ_PIN);
        f.image[p->pins[i].rva+j]^=1;
    }
    for(unsigned flag=0;flag<256;flag++) {
        f.image[0x1031b]=(unsigned char)flag;f.image[0x3104b]=(unsigned char)flag;
        f.image[0xa1013]=(unsigned char)flag;
        for(unsigned k=0;k<4;k++)f.image[0xa2000+k*0x18+0x13]=(unsigned char)flag;
        CHECK(rig_start(&f,&r,1));
    }
    f.image[0x1031b]=f.image[0x3104b]=f.image[0xa1013]=0;
    for(unsigned k=0;k<4;k++)f.image[0xa2000+k*0x18+0x13]=0;
    for(unsigned k=0;k<7;k++){ptr(f.image,0x35000,rig_at(&f,p->destructors[k]));CHECK(rig_start(&f,&r,1));}
    ptr(f.image,0x35000,rig_at(&f,p->destructors[0]));
    f.denied=rig_at(&f,0x30008);CHECK(rig_start(&f,&r,1));f.denied=0;
    number(f.image,0x10310,2);CHECK(!rig_start(&f,&r,0));number(f.image,0x10310,1);
    f.image[0x1031b]=1;f.denied=rig_at(&f,0x31038);CHECK(!rig_start(&f,&r,0));f.denied=0;f.image[0x1031b]=0;
    ptr(f.image,0x35000,0xdeadbeef);CHECK(!rig_start(&f,&r,0) && r.reason==SH_DEFAULT_READ_CALLBACK);
    ptr(f.image,0x35000,rig_at(&f,p->destructors[0]));
    f.denied=rig_at(&f,0xa2150);CHECK(rig_start(&f,&r,1));f.denied=0; /* Unreached row3 despite spare capacity. */
    number(f.image,0xa1008,0);number(f.image,0xa100c,2);f.denied=rig_at(&f,0xa2070);
    CHECK(!rig_start(&f,&r,1) && r.reason==SH_DEFAULT_READ_MEMORY); /* Owned grow destroys CAPACITY. */
    f.image[0xa1013]=2;CHECK(rig_start(&f,&r,1)); /* Borrowed grow copies COUNT0. */
    f.denied=0;
    for(unsigned flag=0;flag<256;flag++) {
        f.image[0xa1013]=(unsigned char)flag;
        CHECK(rig_start(&f,&r,1)==(flag==0 || flag==2));
    }
    f.image[0xa1013]=0;number(f.image,0xa1008,1);number(f.image,0xa100c,4);
    for(unsigned flag=0;flag<256;flag++) {
        rig_list(&f,0xa2000,0xb0000,0,0,flag);
        CHECK(rig_start(&f,&r,1)==(flag==0 || flag==2));
    }
    rig_list(&f,0xa2000,0xb0000,1,16,0);
    ptr(f.image,0x60050,0xdeadbeef);CHECK(!rig_start(&f,&r,1) && r.reason==SH_DEFAULT_READ_CALLBACK);
    number(f.image,0x90000,-1);CHECK(rig_start(&f,&r,1)); /* Rejected component never calls Init. */
    ptr(f.image,0x60018,0xdeadbeef);ptr(f.image,0x60020,0xdeadbeef);CHECK(rig_start(&f,&r,1));
    ptr(f.image,0x60018,rig_at(&f,p->getters[2]));ptr(f.image,0x60020,rig_at(&f,p->getters[3]));
    number(f.image,0x90000,0);ptr(f.image,0x60050,rig_at(&f,p->initializers[0]));
    number(f.image,0x70018,1);CHECK(!rig_start(&f,&r,1));number(f.image,0x70018,0x3c);
    number(f.image,0x7001c,-1);CHECK(!rig_start(&f,&r,1));number(f.image,0x7001c,0);
    number(f.image,0x70020,0x1000);CHECK(!rig_start(&f,&r,1));number(f.image,0x70020,1);
    number(f.image,0x70018,INT32_MAX);CHECK(!rig_start(&f,&r,1));number(f.image,0x70018,0x3c);
    ptr(f.image,0x60048,0xdeadbeef);CHECK(!rig_start(&f,&r,1));ptr(f.image,0x60048,rig_at(&f,p->getters[4]));
    ptr(f.image,0x30000,rig_at(&f,0x10000));CHECK(!rig_start(&f,&r,0) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr(f.image,0x30000,rig_at(&f,0x31000));ptr(f.image,0x31010,rig_at(&f,0x34000));
    CHECK(!rig_start(&f,&r,0) && r.reason==SH_DEFAULT_READ_OWNERSHIP);ptr(f.image,0x31010,rig_at(&f,0x33000));
    ptr(f.image,0x40138,rig_at(&f,0x31000));CHECK(!rig_start(&f,&r,1));ptr(f.image,0x40138,rig_at(&f,0x41000));
    CHECK(rig_start(&f,&r,0));CHECK(!sh_decl_md6_rig_bound_read_check(&r,&f.input,rig_at(&f,0x40001),rig_at(&f,0x41000)));
    ptr(f.image,0x10338,rig_at(&f,0x42000));ptr(f.image,0x10060,0);
    CHECK(rig_start(&f,&r,0));CHECK(sh_decl_md6_rig_bound_read_check(&r,&f.input,0,0)); /* No model preserves actual old hybrid. */
    ptr(f.image,0x10060,rig_at(&f,0x40000));ptr(f.image,0x40138,0);
    CHECK(rig_start(&f,&r,0));CHECK(sh_decl_md6_rig_bound_read_check(&r,&f.input,rig_at(&f,0x40000),0));
    ptr(f.image,0x40138,rig_at(&f,0x41000));ptr(f.image,0x41050,0);CHECK(rig_start(&f,&r,1)); /* Existing skeleton writes NULL hybrid. */
    ptr(f.image,0x41050,rig_at(&f,0x42000));
    f.change_at=rig_at(&f,0x40138);CHECK(!rig_start(&f,&r,1));f.change_at=0;ptr(f.image,0x40138,rig_at(&f,0x41000));
    f.change_at=rig_at(&f,0x60050);CHECK(!rig_start(&f,&r,1) && r.reason==SH_DEFAULT_READ_INVOCATION);
    f.change_at=0;ptr(f.image,0x60050,rig_at(&f,p->initializers[0]));
    CHECK(rig_start(&f,&r,1));f.input.read_context=NULL;CHECK(!sh_decl_md6_rig_read_check(&r,&f.input));f.input.read_context=&f;
    mrg_fail_allocation=1;CHECK(!rig_start(&f,&r,0) && r.reason==SH_DEFAULT_READ_ALLOCATION);
    CHECK(rig_start(&f,&r,1));f.input.work_limit=r.work-1;CHECK(!rig_start(&f,&r,1) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    const dpa_profile *allocator=dpa_profiles+renderer;uintptr_t old;memcpy(&old,f.image+allocator->table+0x10,8);
    ptr(f.image,allocator->table+0x10,old+1);CHECK(!rig_start(&f,&r,0));ptr(f.image,allocator->table+0x10,old);
    f.input.image_size=0x1000;CHECK(!rig_start(&f,&r,0));f.input.image_size=f.size;
    CHECK(rig_start(&f,&r,1));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(rig_start(&f,&r,1));r.complete=1;r.reason=SH_DEFAULT_READ_OK;CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(rig_start(&f,&r,1));r.complete=1;r.reason=SH_DEFAULT_READ_OK;CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void rig_image_case(const char *path,int renderer)
{
    image_case(path,renderer);FILE *file=fopen(path,"rb");CHECK(file);if(!file)return;
    fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);unsigned char *raw=malloc(length);CHECK(raw);
    if(!raw){fclose(file);return;}CHECK(fread(raw,1,length,file)==(size_t)length);fclose(file);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    const mrg_profile *p=mrg_profiles+renderer;
    for(size_t i=0;i<p->pin_count;i++) {
        const mrg_pin *pin=p->pins+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
            IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)) {
                size_t offset=s->PointerToRawData+pin->rva-s->VirtualAddress;
                CHECK(offset<=(size_t)length && pin->length<=(size_t)length-offset && sh_native_pin_fixture_digest(raw+offset,pin->length,pin->hash));found=1;
                if(i==0) {static const unsigned char entry[]={0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x8b,0xf9};CHECK(offset>=16 && !memcmp(raw+offset-16,entry,16));}
                break;
            }
        }CHECK(found);
    }free(raw);printf("MD6 rig: %s complete native bodies and installer entry verified\n",renderer?"Vulkan":"OpenGL");
}
int main(int argc,char **argv)
{
    if(argc==3)rig_image_case(argv[1],atoi(argv[2])!=0);else{rig_cases(0);rig_cases(1);}
    printf("MD6 rig: %d failure(s)\n",failures);return failures?1:0;
}
