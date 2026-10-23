#include "native_pin_fixture.h"
#define main rpr_common_main
#include "decl_default_reads_test.c"
#undef main
static int rpr_missing_export;
static uintptr_t rpr_test_export(int renderer,int resolver)
{return rpr_missing_export?0:UINT64_C(0x7ffb00001000)+(renderer?0x2000:resolver?0x1000:0);}
#define SH_RENDERPROG_RELEASE_EXPORT(renderer,resolver) rpr_test_export(renderer,resolver)
#include "../src/backend/decl_renderprog_release_reads.c"
static uintptr_t rpr_at(fixture *f,size_t offset){return (uintptr_t)f->image+offset;}
static void rpr_setup(fixture *f,int renderer)
{
    setup(f,renderer);rpr_missing_export=0;
    f->input.resource=rpr_at(f,0x10000);ptr(f->image,0x10000,(uintptr_t)f->table);
    ptr(f->table,0xa0,rpr_at(f,rpr_pins[renderer?RPR_VK_RELEASE:RPR_GL_RELEASE].rva));
    for(unsigned i=0;i<RPR_PINS;i++)if(rpr_pins[i].renderer==renderer)
        CHECK(sh_native_pin_fixture_copy(renderer,rpr_pins[i].rva,f->image+rpr_pins[i].rva,rpr_pins[i].length));
    ptr(f->image,renderer?0x1fb6c10:0x59d1c60,rpr_test_export(renderer,0));
    ptr(f->image,0x1fcdcd8,rpr_test_export(0,1));ptr(f->image,0x583f4a8,0x12345678);
    ptr(f->image,0x27a1480,rpr_at(f,rpr_pins[RPR_GL_STRING_DTOR].rva));
}
static int rpr_start(fixture *f,sh_resource_default_read_receipt *r)
{CHECK(!sh_resource_default_read_begin(r,&f->input));return sh_decl_renderprog_release_read_check(r,&f->input);}
static void rpr_handles(fixture *f,uint64_t a,uint64_t b,uint64_t c)
{ptr(f->image,0x11840,a);ptr(f->image,0x11848,b);ptr(f->image,0x11850,c);}
static void rpr_header(fixture *f,size_t at,size_t data,int count,int capacity,unsigned char flag)
{ptr(f->image,at,data?rpr_at(f,data):0);number(f->image,at+8,count);number(f->image,at+12,capacity);f->image[at+0x13]=flag;}
static void rpr_string_row(fixture *f,size_t at,int owned)
{ptr(f->image,at,rpr_at(f,0x27a1480));ptr(f->image,at+0x10,rpr_at(f,at+0x1c));number(f->image,at+0x18,owned?0x40000014:0x80000014);}
static void rpr_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;rpr_setup(&f,renderer);
    CHECK(rpr_start(&f,&r) && r.state==1 && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    rpr_missing_export=1;CHECK(rpr_start(&f,&r));rpr_handles(&f,0,0x9876,0);
    CHECK((rpr_start(&f,&r)!=0)==!renderer);rpr_missing_export=0;CHECK(rpr_start(&f,&r));
    rpr_handles(&f,7,8,9);CHECK(rpr_start(&f,&r));
    uint32_t slot=renderer?0x1fb6c10:0x59d1c60;
    ptr(f.image,slot,0x1234);CHECK(!rpr_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    ptr(f.image,slot,rpr_test_export(renderer,0));
    if(renderer) {ptr(f.image,0x583f4a8,0);CHECK(!rpr_start(&f,&r));ptr(f.image,0x583f4a8,0x1234);}
    else {ptr(f.image,0x1fcdcd8,0x1234);CHECK(!rpr_start(&f,&r));ptr(f.image,0x1fcdcd8,rpr_test_export(0,1));}
    f.change_at=rpr_at(&f,slot);CHECK(!rpr_start(&f,&r) && r.reason==SH_DEFAULT_READ_INVOCATION);
    f.change_at=0;ptr(f.image,slot,rpr_test_export(renderer,0));
    f.denied=rpr_at(&f,0x11840);CHECK(!rpr_start(&f,&r));f.denied=0;
    if(!renderer) {
        size_t ctx=0x10078;
        const size_t lists[]={0x38,0x9d0};
        for(unsigned list=0;list<2;list++)for(unsigned flag=0;flag<6;flag++) {
            rpr_header(&f,ctx+lists[list],0x50000,0,2,(unsigned char)flag);
            rpr_string_row(&f,0x50000,0);rpr_string_row(&f,0x50030,1);
            CHECK(rpr_start(&f,&r));
            f.denied=rpr_at(&f,0x50030);int own=flag==0 || flag==3 || flag==4;
            CHECK((rpr_start(&f,&r)!=0)==!own);f.denied=0;
            ptr(f.image,0x50030,0);CHECK((rpr_start(&f,&r)!=0)==!own);
            rpr_header(&f,ctx+lists[list],0,0,0,0);
        }
        rpr_header(&f,ctx+0xa0,0x60000,-1,4,1);CHECK(rpr_start(&f,&r));
        f.denied=rpr_at(&f,0x60007);CHECK(!rpr_start(&f,&r));f.denied=0;
        number(f.image,ctx+0xac,-1);CHECK(!rpr_start(&f,&r));number(f.image,ctx+0xac,0);
        f.denied=rpr_at(&f,0x60000);CHECK(rpr_start(&f,&r));f.denied=0;
        rpr_header(&f,ctx+0x38,0x50000,9,0,0);CHECK(rpr_start(&f,&r));
        rpr_header(&f,ctx+0x38,0,0,0,0);
        for(unsigned i=0;i<3;i++) {
            const size_t headers[]={0xa0+i*0x1b0,0x178+i*0x1b0,0x5b0+i*0x58,0x6b8+i*0x58,0x7c0+i*0x28,0x838+i*0x28,0x8b0+i*0x28,0x928+i*0x38};
            for(unsigned j=0;j<8;j++){f.denied=rpr_at(&f,ctx+headers[j]);CHECK(!rpr_start(&f,&r));}f.denied=0;
        }
    }
    for(unsigned i=0;i<RPR_PINS;i++)if(rpr_pins[i].renderer==renderer) {
        size_t offsets[]={0,rpr_pins[i].length/2,rpr_pins[i].length-1};
        for(unsigned j=0;j<3;j++){unsigned char *at=f.image+rpr_pins[i].rva+offsets[j];*at^=1;CHECK(!rpr_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*at^=1;}
    }
    CHECK(rpr_start(&f,&r));f.input.work_limit=r.work-1;CHECK(!rpr_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(rpr_start(&f,&r));f.input.resource++;CHECK(!sh_decl_renderprog_release_read_check(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.resource--;
    CHECK(rpr_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static unsigned rpr_deleted,rpr_freed;static uint64_t rpr_last_handle;
static void rpr_vk_delete(void *device,uint64_t handle,void *allocator)
{CHECK((uintptr_t)device==0x12345678 && !allocator);rpr_deleted++;rpr_last_handle=handle;}
static void rpr_gl_delete(unsigned handle){rpr_deleted++;rpr_last_handle=handle;}
static void rpr_free(void *self,void *block,unsigned tag)
{CHECK(self && block && tag==0x10);rpr_freed++;}
static void rpr_native_fixture(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;rpr_setup(&f,renderer);rpr_handles(&f,11,22,33);
    size_t ctx=0x10078;
    if(!renderer) {
        rpr_header(&f,ctx+0x38,0x50000,0,2,0);rpr_string_row(&f,0x50000,0);rpr_string_row(&f,0x50030,1);
        rpr_header(&f,ctx+0x9d0,0x51000,0,1,3);rpr_string_row(&f,0x51000,0);
        rpr_header(&f,ctx+0x50,0x52000,1,1,4);
        rpr_header(&f,ctx+0x15e8,0x53000,1,1,1); /* Borrowed pointer must survive reset. */
        rpr_header(&f,ctx+0xa0,0x54000,2,4,2);memset(f.image+0x54000,0x55,8);
    }
    CHECK(rpr_start(&f,&r));
    /* Execute retained native release/reset bytes with controlled external
     * allocation/GPU fixtures. This is a code-semantics check, not a live
     * renderer or an admission after replacing the qualified callbacks. */
    for(unsigned i=0;i<RPR_PINS;i++)if(rpr_pins[i].renderer==renderer){DWORD old;CHECK(VirtualProtect(f.image+rpr_pins[i].rva,rpr_pins[i].length,PAGE_EXECUTE_READWRITE,&old));}
    ptr(f.image,renderer?0x1fb6c10:0x59d1c60,(uintptr_t)(renderer?(void *)rpr_vk_delete:(void *)rpr_gl_delete));
    if(!renderer)ptr(f.image,dpa_profiles[0].table+0x10,(uintptr_t)rpr_free);
    rpr_deleted=rpr_freed=0;rpr_last_handle=0;
    ((void (*)(void *))(f.image+rpr_pins[renderer?RPR_VK_RELEASE:RPR_GL_RELEASE].rva))((void *)f.input.resource);
    CHECK(rpr_deleted==(renderer?3u:2u) && rpr_last_handle==(renderer?33u:11u));
    uint64_t zero=1;memcpy(&zero,f.image+0x11840,8);CHECK(!zero);memcpy(&zero,f.image+0x11848,8);CHECK(!zero);memcpy(&zero,f.image+0x11850,8);CHECK(!zero);
    if(!renderer) {CHECK(rpr_freed==4);memcpy(&zero,f.image+ctx+0x15e8,8);CHECK(zero==rpr_at(&f,0x53000));memcpy(&zero,f.image+0x54000,8);CHECK(!zero);}
    else {unsigned short status;memcpy(&status,f.image+0x583fbbc,2);CHECK(status==0x101);}
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void rpr_image_case(const char *path,int renderer)
{
    FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    unsigned char *raw=malloc(size);CHECK(raw);if(!raw){fclose(f);return;}CHECK(fread(raw,1,size,f)==(size_t)size);fclose(f);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<RPR_PINS;i++)if(rpr_pins[i].renderer==renderer){const rpr_pin *pin=rpr_pins+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){
                size_t off=s->PointerToRawData+pin->rva-s->VirtualAddress;CHECK(off<=(size_t)size && pin->length<=(size_t)size-off && sh_native_pin_fixture_digest(raw+off,pin->length,pin->hash));found=1;break;}}
        CHECK(found);}
    free(raw);
}
int main(int argc,char **argv)
{if(argc==3)rpr_image_case(argv[1],atoi(argv[2])!=0);else {rpr_cases(0);rpr_cases(1);rpr_native_fixture(0);rpr_native_fixture(1);}printf("RenderProg release reads: %d failure(s)\n",failures);return failures?1:0;}
