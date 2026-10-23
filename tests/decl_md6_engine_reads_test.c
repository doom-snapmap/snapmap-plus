#include "native_pin_fixture.h"
/* Actual engine, allocator and generic predicates; only host metadata is modeled. */
#define main sh_engine_common_test_main
#include "decl_default_reads_test.c"
#undef main
#ifndef SH_DECL_ENGINE_TEST_INCLUDED
#error Common fixture must include the actual engine inspector with modeled host metadata.
#endif

static uintptr_t engine_at(fixture *f,size_t offset)
{return (uintptr_t)f->image+offset;}
static uint64_t engine_encode(uintptr_t value,uint64_t cookie)
{unsigned n=(unsigned)cookie&63;return (n?((uint64_t)value<<n)|((uint64_t)value>>(64-n)):(uint64_t)value)^cookie;}
static void engine_vector(fixture *f,uint32_t table,uintptr_t begin,uintptr_t end,uintptr_t capacity)
{
    const der_profile *b=der_profiles+f->input.renderer;uint64_t cookie;
    memcpy(&cookie,f->image+b->cookie,8);
    ptr(f->image,table,engine_encode(begin,cookie));ptr(f->image,table+8,engine_encode(end,cookie));
    ptr(f->image,table+16,engine_encode(capacity,cookie));
}
static void engine_cold(fixture *f,int guard,int event)
{ddp_test_engine_state(f,guard,event);}
static int engine_start(fixture *f,sh_resource_default_read_receipt *r,unsigned methods)
{CHECK(!sh_resource_default_read_begin(r,&f->input));return sh_decl_engine_read_check(r,&f->input,methods);}
static void engine_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;setup(&f,renderer);
    const der_profile *b=der_profiles+renderer;
    const int initialized[]={-2,-3,1,INT32_MAX};
    for(unsigned i=0;i<sizeof initialized/sizeof *initialized;i++) {
        number(f.image,b->guard,initialized[i]);
        CHECK(engine_start(&f,&r,3) && !r.complete && r.state==1 && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
        CHECK(start(&f,&r) && r.complete); /* Generic context80 composes the real inspector. */
        number(f.image,0xf1004,initialized[i]);
        CHECK(engine_start(&f,&r,3)); /* Fresh epoch skips header, same singleton. */
        CHECK(start(&f,&r) && r.complete);
        number(f.image,0xf1004,INT32_MIN); /* Stale epoch enters initialized header. */
    }
    number(f.image,b->guard,-2);ptr(f.image,b->sys,0);
    CHECK(engine_start(&f,&r,3)); /* Warm route never calls Sys; getter still reads actual thread TLS. */
    ddp_test_tls_array=0;CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_MEMORY);
    ddp_test_tls_array=engine_at(&f,0xf0000);
    f.image[b->pins[5].rva]^=1;CHECK(engine_start(&f,&r,3));f.image[b->pins[5].rva]^=1;
    f.image[b->pins[4].rva]^=1;CHECK(engine_start(&f,&r,1));
    CHECK(!engine_start(&f,&r,2) && r.reason==SH_DEFAULT_READ_PIN);f.image[b->pins[4].rva]^=1;
    ptr(f.image,b->engine_table+0x90,1);CHECK(engine_start(&f,&r,1));CHECK(!engine_start(&f,&r,3));
    ptr(f.image,b->engine_table+0x90,engine_at(&f,b->pins[4].rva));
    ptr(f.image,b->engine+0x1a0,1);CHECK(engine_start(&f,&r,1));CHECK(!engine_start(&f,&r,3));
    ptr(f.image,b->engine+0x1a0,engine_at(&f,b->member_table));
    ptr(f.image,b->engine,1);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_CALLBACK);
    ptr(f.image,b->engine,engine_at(&f,b->engine_table));

    for(int guard=-1;guard<=0;guard++)for(int event=0;event<=1;event++) {
        engine_cold(&f,guard,event);
        CHECK(engine_start(&f,&r,3) && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
        CHECK(start(&f,&r) && r.complete); /* Real generic80 cold admission, no engine execution. */
    }
    engine_cold(&f,0,0);
    /* Every byte of every selected native body, including chained unwind tails. */
    for(size_t i=0;i<b->pin_count;i++)for(size_t j=0;j<b->pins[i].length;j++) {
        f.image[b->pins[i].rva+j]^=1;
        CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_PIN);
        f.image[b->pins[i].rva+j]^=1;
    }
    ptr(f.image,b->engine,1);ptr(f.image,b->engine+0x1a0,1);
    CHECK(engine_start(&f,&r,3)); /* Cold constructor writes both tables before return. */
    ptr(f.image,b->engine,engine_at(&f,b->engine_table));ptr(f.image,b->engine+0x1a0,engine_at(&f,b->member_table));
    ptr(f.image,b->sys,0);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,b->sys,1);
    ddp_test_tls_array=0;CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_MEMORY);ddp_test_tls_array=engine_at(&f,0xf0000);
    f.denied=engine_at(&f,b->tls_index);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    number(f.image,0xf1004,0);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    number(f.image,0xf1004,INT32_MIN);number(f.image,b->tls_index,3);ptr(f.image,0xf0018,engine_at(&f,0xf2000));
    number(f.image,0xf2004,INT32_MIN);CHECK(engine_start(&f,&r,3));number(f.image,b->tls_index,0);
    ptr(f.image,0xf0000,UINTPTR_MAX-2);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_BOUNDS);
    ptr(f.image,0xf0000,engine_at(&f,0xf1000));
    ptr(f.image,b->wait,1);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_CALLBACK);
    ptr(f.image,b->event,1);CHECK(engine_start(&f,&r,3)); /* Event route never dispatches encoded CV callbacks. */
    engine_cold(&f,0,0);ddp_test_export_missing=1;CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_CALLBACK);ddp_test_export_missing=0;
    uint64_t cookie;memcpy(&cookie,f.image+b->cookie,8);
    engine_vector(&f,b->exit_table,engine_at(&f,0xe0000),engine_at(&f,0xe0040),engine_at(&f,0xe0080));
    memset(f.image+0xe0000,0x91,0x80);CHECK(engine_start(&f,&r,3));
    f.denied=engine_at(&f,0xe0040);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    ptr(f.image,b->exit_table,engine_encode(UINTPTR_MAX,cookie));
    engine_vector(&f,b->exit_fallback,engine_at(&f,0xe0000),engine_at(&f,0xe0080),engine_at(&f,0xe0080));
    CHECK(engine_start(&f,&r,3));
    engine_vector(&f,b->exit_fallback,engine_at(&f,0xe0000),engine_at(&f,0xe0090),engine_at(&f,0xe0080));
    CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    engine_vector(&f,b->exit_fallback,engine_at(&f,0xe0001),engine_at(&f,0xe0001),engine_at(&f,0xe0081));
    CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    engine_vector(&f,b->exit_fallback,0,0,8);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    engine_vector(&f,b->exit_fallback,8,8,UINT64_C(0x8000000000000010));CHECK(!engine_start(&f,&r,3));
    engine_cold(&f,0,0);CHECK(engine_start(&f,&r,3));f.input.work_limit=r.work-1;
    CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    const dpa_profile *allocator=dpa_profiles+renderer;uintptr_t old;
    memcpy(&old,f.image+allocator->table+0x10,8);ptr(f.image,allocator->table+0x10,old+1);
    CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,allocator->table+0x10,old);
    number(f.image,dpa_crt_profiles[renderer].mode,1);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_CALLBACK);
    number(f.image,dpa_crt_profiles[renderer].mode,0);
    f.change_at=engine_at(&f,b->sys);CHECK(!engine_start(&f,&r,3) && r.reason==SH_DEFAULT_READ_INVOCATION);
    f.change_at=0;ptr(f.image,b->sys,1);
    CHECK(!engine_start(&f,&r,0) && r.reason==SH_DEFAULT_READ_INPUT);
    CHECK(!engine_start(&f,&r,4) && r.reason==SH_DEFAULT_READ_INPUT);
    CHECK(engine_start(&f,&r,3));f.input.read_context=NULL;
    CHECK(!sh_decl_md6_engine_read_check(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.read_context=&f;
    CHECK(engine_start(&f,&r,3));r.state=2;CHECK(!sh_decl_md6_engine_read_check(&r,&f.input));
    CHECK(engine_start(&f,&r,3));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    f.input.image_size=0x1000;CHECK(!engine_start(&f,&r,3));f.input.image_size=f.size;
    f.input.pinned_image=0;CHECK(!engine_start(&f,&r,3));f.input.pinned_image=1;
    VirtualFree(f.image,0,MEM_RELEASE);ddp_test_tls_array=0;
}
static void engine_image_case(const char *path,int renderer)
{
    image_case(path,renderer);FILE *file=fopen(path,"rb");CHECK(file);if(!file)return;
    fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);unsigned char *raw=malloc(length);CHECK(raw);
    if(!raw){fclose(file);return;}CHECK(fread(raw,1,length,file)==(size_t)length);fclose(file);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(size_t i=0;i<der_profiles[renderer].pin_count;i++) {
        const der_pin *pin=der_profiles[renderer].pins+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) {
            IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)) {
                size_t offset=s->PointerToRawData+pin->rva-s->VirtualAddress;
                CHECK(offset<=(size_t)length && pin->length<=(size_t)length-offset && sh_native_pin_fixture_digest(raw+offset,pin->length,pin->hash));found=1;break;
            }
        }CHECK(found);
    }free(raw);printf("engine context: %s 29 complete native body pins verified\n",renderer?"Vulkan":"OpenGL");
}
int main(int argc,char **argv)
{
    if(argc==3)engine_image_case(argv[1],atoi(argv[2])!=0);else{engine_cases(0);engine_cases(1);}
    printf("engine context: %d failure(s)\n",failures);return failures?1:0;
}
