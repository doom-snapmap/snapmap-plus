#include "native_pin_fixture.h"
/* Native default preconditions and receipt failure paths, without game execution. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/backend/resource_default_reads.c"
#include "../src/backend/decl_default_allocator.c"
static uintptr_t ddp_test_tls_array;
static int ddp_test_export_missing;
static uintptr_t ddp_test_os_export(const char *name)
{
    if(ddp_test_export_missing)return 0;
    if(!strcmp(name,"SleepConditionVariableCS"))return UINT64_C(0x7ffb00001000);
    if(!strcmp(name,"WakeAllConditionVariable"))return UINT64_C(0x7ffb00002000);
    return 0;
}
#define SH_DECL_ENGINE_TLS_ARRAY() ddp_test_tls_array
#define SH_DECL_ENGINE_OS_EXPORT(name) ddp_test_os_export(name)
#include "../src/backend/decl_md6_engine_reads.c"
#define SH_DECL_ENGINE_TEST_INCLUDED 1
#include "../src/backend/decl_default_reads.c"
static int failures;
#define CHECK(x) do { if(!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); failures++; } } while(0)
typedef struct fixture {
    uint8_t *image;
    size_t size;
    uint8_t object[0x200], table[0xb0];
    uintptr_t denied,change_at;
    sh_resource_default_read_input input;
} fixture;
static int contains(uintptr_t a,size_t n,uintptr_t b,size_t length)
{ return a>=b && a-b<=length && n<=length-(a-b); }
static int safe_read(void *context,uintptr_t a,void *out,size_t n)
{
    fixture *f=context;
    if(a==f->denied || n>UINTPTR_MAX-a) return 0;
    if(!contains(a,n,(uintptr_t)f->image,f->size) &&
       !contains(a,n,(uintptr_t)f->object,sizeof f->object) &&
       !contains(a,n,(uintptr_t)f->table,sizeof f->table)) return 0;
    memcpy(out,(void *)a,n);
    if(a==f->change_at && n==8) memset((void *)a,0,8);
    return 1;
}
static void ptr(void *at,size_t offset,uintptr_t value)
{ memcpy((uint8_t *)at+offset,&value,8); }
static void number(void *at,size_t offset,int value)
{ memcpy((uint8_t *)at+offset,&value,4); }
static uintptr_t address(fixture *f,unsigned i)
{ return (uintptr_t)f->image+ddp_pins[f->input.renderer][i].rva; }
static void ddp_test_engine_state(fixture *f,int guard,int event);
static void setup(fixture *f,int renderer)
{
    int32_t delta=9,text_relative;
    const ddp_context_binding *binding=ddp_context_profiles+renderer;
    memset(f,0,sizeof *f); f->size=0x14000000;
    f->image=VirtualAlloc(NULL,f->size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(f->image); if(!f->image) exit(2);
    f->input=(sh_resource_default_read_input){(uintptr_t)f->image,f->size,renderer,1,
        (uintptr_t)f->object,(uintptr_t)f->image+ddp_pins[renderer][DDP_DEFAULT].rva,safe_read,f,0};
    for(unsigned i=0;i<14;i++)
        memcpy((void *)address(f,i),ddp_pins[renderer][i].bytes,ddp_pins[renderer][i].length);
    memcpy(&text_relative,(void *)(address(f,DDP_TEXT)+3),4);
    memcpy((void *)(address(f,DDP_TEXT)+7+text_relative),"{ }\n",5);
    memcpy(f->image+0x1000,"\x48\x8d\x05",3);
    memcpy(f->image+0x1003,&delta,4); f->image[0x1007]=0xc3;
    ptr(f->object,0,(uintptr_t)f->table);
    ptr(f->table,0x50,address(f,DDP_DEFAULT));
    ptr(f->table,0x70,(uintptr_t)f->image+0x1000);
    ptr(f->table,0x88,address(f,DDP_TEXT));
    ptr(f->table,0x98,address(f,DDP_COMMON));
    ptr(f->table,0xa0,address(f,DDP_RELEASE));
    ptr(f->image,binding->game_holder,(uintptr_t)f->image+binding->game_object);
    ptr(f->image,binding->game_object,(uintptr_t)f->image+binding->game_table);
    ptr(f->image,binding->game_table+0x240,(uintptr_t)f->image+binding->functions[0].rva);
    ptr(f->image,binding->context_object,(uintptr_t)f->image+binding->context_table);
    ptr(f->image,binding->context_table+0x80,(uintptr_t)f->image+binding->functions[2].rva);
    number(f->image,binding->context_guard,-2);
    for(unsigned i=0;i<3;i++)
        memcpy(f->image+binding->functions[i].rva,binding->functions[i].bytes,binding->functions[i].length);
    {
        const der_profile *engine=der_profiles+renderer;
        for(size_t i=0;i<engine->pin_count;i++)
            CHECK(sh_native_pin_fixture_copy(renderer,engine->pins[i].rva,f->image+engine->pins[i].rva,engine->pins[i].length));
        ptr(f->image,engine->common_holder,(uintptr_t)f->image+engine->common);
        ptr(f->image,engine->common,(uintptr_t)f->image+engine->common_table);
        ptr(f->image,engine->common_table+0x240,(uintptr_t)f->image+engine->pins[0].rva);
        ptr(f->image,engine->common_table+0x250,(uintptr_t)f->image+engine->pins[1].rva);
        ptr(f->image,engine->engine,(uintptr_t)f->image+engine->engine_table);
        ptr(f->image,engine->engine_table+0x80,(uintptr_t)f->image+engine->pins[3].rva);
        ptr(f->image,engine->engine_table+0x90,(uintptr_t)f->image+engine->pins[4].rva);
        ptr(f->image,engine->engine+0x1a0,(uintptr_t)f->image+engine->member_table);
        ddp_test_tls_array=0;ddp_test_export_missing=0;
    }
    {
        const dpa_profile *allocator=dpa_profiles+renderer;
        ptr(f->image,allocator->holder,(uintptr_t)f->image+allocator->instance);
        ptr(f->image,allocator->instance,(uintptr_t)f->image+allocator->table);
        for(unsigned i=0;i<6;i++) {
            const dpa_method *m=allocator->methods+i;
            ptr(f->image,allocator->table+m->slot,(uintptr_t)f->image+m->rva);
            memcpy(f->image+m->rva,m->bytes,m->length);
        }
        ptr(f->image,dpa_tokens[renderer].table,(uintptr_t)f->image+dpa_tokens[renderer].rva);
        memcpy(f->image+dpa_tokens[renderer].rva,dpa_tokens[renderer].bytes,dpa_tokens[renderer].length);
        {
            const dpa_crt_profile *crt=dpa_crt_profiles+renderer;
            ptr(f->image,crt->handler,(uintptr_t)f->image+crt->default_handler);
            for(unsigned i=0;i<8;i++)
                memcpy(f->image+crt->methods[i].rva,crt->methods[i].bytes,crt->methods[i].length);
        }
    }
    ddp_test_engine_state(f,-2,0);
}
static void ddp_test_engine_state(fixture *f,int guard,int event)
{
    const der_profile *b=der_profiles+f->input.renderer;uint64_t cookie=UINT64_C(0xc83710d3e915d43b);
    ddp_test_export_missing=0;
    number(f->image,b->guard,guard);ptr(f->image,b->sys,1);number(f->image,b->tls_index,0);
    ddp_test_tls_array=(uintptr_t)f->image+0xf0000;ptr(f->image,0xf0000,(uintptr_t)f->image+0xf1000);
    number(f->image,0xf1004,INT32_MIN);ptr(f->image,b->cookie,cookie);ptr(f->image,b->event,event?0x1234:0);
    unsigned bits=(unsigned)cookie&63;
    uintptr_t wait=ddp_test_os_export("SleepConditionVariableCS"),notify=ddp_test_os_export("WakeAllConditionVariable");
    ptr(f->image,b->wait,((wait<<bits)|(wait>>(64-bits)))^cookie);
    ptr(f->image,b->notify,((notify<<bits)|(notify>>(64-bits)))^cookie);
    /* Cookie encoding of a null preferred/fallback CRT exit vector. */
    for(unsigned i=0;i<3;i++) {ptr(f->image,b->exit_table+i*8,cookie);ptr(f->image,b->exit_fallback+i*8,cookie);}
}
static int start(fixture *f,sh_resource_default_read_receipt *r)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    if(r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
    return sh_decl_default_read_begin(r,&f->input);
}
static int names_check(fixture *f,sh_resource_default_read_receipt *r,
                       const char *const *names,size_t count)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    return sh_decl_default_names_unshadowed(r,&f->input,names,count);
}
static void names_cases(fixture *f)
{
    sh_resource_default_read_receipt r;
    uintptr_t node=(uintptr_t)f->image+0x2000,other=node+0x100,name=node+0x200;
    size_t head=ddp_define_heads[f->input.renderer];
    char long_name[513]; const char *names[]={"model","_default",long_name};
    memset(long_name,'a',sizeof long_name-1); long_name[sizeof long_name-1]=0;
    CHECK(names_check(f,&r,names,3) && !r.complete);
    ptr(f->image,head,node); ptr((void *)node,0x10,name); ptr((void *)node,0x50,other);
    ptr((void *)other,0x10,node+0x500); ptr((void *)other,0x50,0);
    memcpy((void *)(node+0x500),"_default_extra",15);
    memcpy((void *)name,"MODEL",6); CHECK(names_check(f,&r,names,3) && !r.complete);
    memcpy((void *)name,"model",6);
    CHECK(!names_check(f,&r,names,3) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    memcpy((void *)name,long_name,sizeof long_name);
    CHECK(!names_check(f,&r,names,3) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ((char *)name)[sizeof long_name-2]='b'; CHECK(names_check(f,&r,names,3));
    f->input.work_limit=8;
    CHECK(!names_check(f,&r,names,3) && r.reason==SH_DEFAULT_READ_BUDGET); f->input.work_limit=0;
    memcpy((void *)name,"OTHER",6); ptr((void *)other,0x50,node);
    CHECK(!names_check(f,&r,names,3) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr((void *)other,0x50,0); f->denied=name;
    CHECK(!names_check(f,&r,names,3) && r.reason==SH_DEFAULT_READ_MEMORY); f->denied=0;
    f->change_at=(uintptr_t)f->image+head;
    CHECK(!names_check(f,&r,names,3) && r.reason==SH_DEFAULT_READ_OWNERSHIP); f->change_at=0;
    CHECK(names_check(f,&r,NULL,0) && !r.complete);
    CHECK(!names_check(f,&r,NULL,1) && r.reason==SH_DEFAULT_READ_INPUT);
    names[1]=NULL; CHECK(!names_check(f,&r,names,3) && r.reason==SH_DEFAULT_READ_INPUT);
    names[1]="_default"; CHECK(names_check(f,&r,names,3));
    r.state=2; CHECK(!sh_decl_default_names_unshadowed(&r,&f->input,names,3));
    CHECK(names_check(f,&r,names,3));
    f->input.read_context=NULL;
    CHECK(!sh_decl_default_names_unshadowed(&r,&f->input,names,3)); f->input.read_context=f;
    CHECK(names_check(f,&r,names,3));
    r.complete=1; CHECK(!sh_decl_default_names_unshadowed(&r,&f->input,names,3));
}
static void cases(int renderer)
{
    fixture f; sh_resource_default_read_receipt r; uintptr_t saved; int32_t relative;
    setup(&f,renderer);
    names_cases(&f);
    for(unsigned i=DDP_COMMON;i<14;i++) {
        ptr(f.table,0x98,address(&f,i));
        CHECK(start(&f,&r)); CHECK(r.routine==SH_DEFAULT_READ_ROUTINE_DECLARATION && r.complete);
        CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
        CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    }
    ptr(f.table,0x98,address(&f,DDP_COMMON));
    {
        const dpa_crt_profile *crt=dpa_crt_profiles+renderer;
        CHECK(!sh_resource_default_read_begin(&r,&f.input));
        CHECK(sh_decl_default_crt_allocation_read_check(&r,&f.input) && !r.complete);
        r.state=2;CHECK(!sh_decl_default_crt_allocation_read_check(&r,&f.input));
        ptr(f.image,crt->handler,0);CHECK(start(&f,&r));
        ptr(f.image,crt->handler,(uintptr_t)f.image+crt->default_handler+1);
        CHECK(!start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);
        ptr(f.image,crt->handler,(uintptr_t)f.image+crt->default_handler);
        number(f.image,crt->mode,1);
        CHECK(!start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);
        number(f.image,crt->mode,0);
        f.denied=(uintptr_t)f.image+crt->mode;
        CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
        f.denied=(uintptr_t)f.image+crt->handler;
        CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
        number(f.image,crt->throw_info,0x10);
        CHECK(!start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_CALLBACK);
        number(f.image,crt->throw_info,0);
        for(unsigned i=0;i<8;i++) {
            const dpa_method *m=crt->methods+i;
            for(unsigned k=0;k<m->length;k++) {
                f.image[m->rva+k]^=1;
                CHECK(!start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_PIN);
                f.image[m->rva+k]^=1;
            }
        }
        ptr(f.image,crt->handler,0);
        f.image[crt->methods[5].rva]^=1;
        CHECK(start(&f,&r)); /* No handler dispatch means its body is not reached. */
        f.image[crt->methods[5].rva]^=1;
        ptr(f.image,crt->handler,(uintptr_t)f.image+crt->default_handler);
        CHECK(start(&f,&r));
    }
    {
        const dpa_profile *a=dpa_profiles+renderer;
        const uint32_t places[]={a->holder,a->instance,a->instance+0x38};
        for(unsigned i=0;i<3;i++) {
            uintptr_t original;
            memcpy(&original,f.image+places[i],8); ptr(f.image,places[i],original+1);
            CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
            ptr(f.image,places[i],original);
            f.denied=(uintptr_t)f.image+places[i];
            CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
        }
        for(unsigned i=0;i<6;i++) {
            const dpa_method *m=a->methods+i;
            ptr(f.image,a->table+m->slot,(uintptr_t)f.image+m->rva+1);
            CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
            ptr(f.image,a->table+m->slot,(uintptr_t)f.image+m->rva);
            f.denied=(uintptr_t)f.image+a->table+m->slot;
            CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
            for(unsigned k=0;k<m->length;k++) {
                f.image[m->rva+k]^=1;
                CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
                f.image[m->rva+k]^=1;
            }
        }
        CHECK(!sh_resource_default_read_begin(&r,&f.input));
        CHECK(sh_decl_default_allocator_read_check(&r,&f.input) && !r.complete);
    }
    {
        const dpa_token_binding *t=dpa_tokens+renderer;
        ptr(f.image,t->table,(uintptr_t)f.image+t->rva+1);
        CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
        ptr(f.image,t->table,(uintptr_t)f.image+t->rva);
        f.denied=(uintptr_t)f.image+t->table;
        CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
        for(unsigned k=0;k<t->length;k++) {
            f.image[t->rva+k]^=1;
            CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
            f.image[t->rva+k]^=1;
        }
        CHECK(!sh_resource_default_read_begin(&r,&f.input));
        CHECK(sh_decl_default_token_read_check(&r,&f.input) && !r.complete);
        r.state=2; CHECK(!sh_decl_default_token_read_check(&r,&f.input));
    }
    {
        uintptr_t text=0; int32_t delta=9;
        memcpy(f.image+0x1100,"\x48\x8d\x05",3);
        memcpy(f.image+0x1103,&delta,4); f.image[0x1107]=0xc3;
        memcpy(f.image+0x1110,"{ custom }",11);
        ptr(f.table,0x88,(uintptr_t)f.image+0x1100);
        ptr(f.table,0x98,0x1234); ptr(f.table,0xa0,0x5678);
        f.object[0x60]=7;
        CHECK(!sh_resource_default_read_begin(&r,&f.input));
        CHECK(sh_decl_default_envelope_read_check(&r,&f.input,&text) && !r.complete);
        CHECK(text==(uintptr_t)f.image+0x1110);
        CHECK(!sh_decl_default_common_body_read_check(&r,&f.input) && !r.complete);
        f.image[0x1107]=0x90;
        CHECK(!sh_resource_default_read_begin(&r,&f.input));
        CHECK(!sh_decl_default_envelope_read_check(&r,&f.input,&text)); CHECK(!text);
        f.image[0x1107]=0xc3;
        CHECK(!sh_resource_default_read_begin(&r,&f.input));
        f.input.work_limit=1;
        CHECK(!sh_decl_default_envelope_read_check(&r,&f.input,&text)); CHECK(!text);
        f.input.work_limit=0;
        f.object[0x60]=0;
        ptr(f.table,0x88,address(&f,DDP_TEXT));
        ptr(f.table,0x98,address(&f,DDP_COMMON)); ptr(f.table,0xa0,address(&f,DDP_RELEASE));
    }
    {
        const ddp_context_binding *b=ddp_context_profiles+renderer;
        const uint32_t places[]={b->game_holder,b->game_object,b->game_table+0x240,
                                 b->context_object,b->context_table+0x80};
        for(unsigned i=0;i<5;i++) {
            uintptr_t original;
            memcpy(&original,f.image+places[i],8);
            ptr(f.image,places[i],original+1);
            CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
            ptr(f.image,places[i],original);
            f.denied=(uintptr_t)f.image+places[i];
            CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
        }
        ddp_test_engine_state(&f,0,0); CHECK(start(&f,&r));
        ddp_test_engine_state(&f,-1,1); CHECK(start(&f,&r));
        ptr(f.image,der_profiles[renderer].sys,0);
        CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
        ddp_test_engine_state(&f,-2,0); CHECK(start(&f,&r));
        for(unsigned i=0;i<3;i++) for(unsigned k=0;k<b->functions[i].length;k++) {
            uint32_t at=b->functions[i].rva+k;
            f.image[at]^=1; CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN); f.image[at]^=1;
        }
        /* The reusable body predicate must not certify custom parsers or
         * cleanup itself. Unknown callbacks remain the caller's obligation. */
        ptr(f.table,0x98,0x1234); ptr(f.table,0xa0,0x5678);
        CHECK(!sh_resource_default_read_begin(&r,&f.input));
        CHECK(sh_decl_default_common_body_read_check(&r,&f.input) && !r.complete);
        CHECK(!sh_decl_default_read_begin(&r,&f.input) && !r.complete);
        ptr(f.table,0x98,address(&f,DDP_COMMON)); ptr(f.table,0xa0,address(&f,DDP_RELEASE));
    }
    f.object[0x60]=1; CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP); f.object[0x60]=0;
    for(unsigned i=DDP_ONLINE;i<=DDP_LAYERS;i++) {
        ptr(f.table,0x98,address(&f,i)); number(f.object,0x70,1);
        CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        number(f.object,0x70,-1); CHECK(!start(&f,&r)); number(f.object,0x70,0);
    }
    ptr(f.table,0x98,address(&f,DDP_TWITCH));
    for(unsigned i=0;i<5;i++) {
        number(f.object,0x78+i*0x18,1); CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        number(f.object,0x78+i*0x18,0);
    }
    ptr(f.table,0x98,address(&f,DDP_COMMON));
    ptr(f.table,0xa0,address(&f,DDP_RELEASE)+1); CHECK(!start(&f,&r));
    ptr(f.table,0xa0,address(&f,DDP_RELEASE));
    ptr(f.table,0x98,address(&f,DDP_DEFAULT)); CHECK(!start(&f,&r));
    ptr(f.table,0x98,address(&f,DDP_COMMON));
    ptr(f.table,0x88,address(&f,DDP_TEXT)+1); CHECK(!start(&f,&r));
    ptr(f.table,0x88,address(&f,DDP_TEXT));
    f.image[0x1007]=0x90; CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    f.image[0x1007]=0xc3;
    memcpy(&relative,(void *)(address(&f,DDP_TEXT)+3),4);
    saved=address(&f,DDP_TEXT)+7+relative;
    *(char *)saved='x'; CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN); *(char *)saved='{';
    *(char *)address(&f,DDP_COMMON)^=1; CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
    *(char *)address(&f,DDP_COMMON)^=1;
    f.denied=(uintptr_t)f.object+0x60; CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY); f.denied=0;
    f.input.work_limit=1; CHECK(!start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET); f.input.work_limit=0;
    CHECK(start(&f,&r)); CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(start(&f,&r)); CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    CHECK(start(&f,&r)); f.input.read_context=NULL;
    CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1)); f.input.read_context=&f;
    f.input.pinned_image=0; CHECK(!start(&f,&r)); f.input.pinned_image=1;
    f.input.image_size=0x1000; CHECK(!start(&f,&r)); f.input.image_size=f.size;
    f.input.renderer=1-renderer; CHECK(!start(&f,&r)); f.input.renderer=renderer;
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void image_case(const char *path,int renderer)
{
    FILE *file=fopen(path,"rb"); long length;
    uint8_t *raw,*image; IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *section;
    CHECK(file); if(!file)return;
    fseek(file,0,SEEK_END); length=ftell(file); rewind(file);
    raw=malloc(length); CHECK(raw); if(!raw){fclose(file);return;}
    CHECK(fread(raw,1,length,file)==(size_t)length); fclose(file);
    nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    image=VirtualAlloc(NULL,nt->OptionalHeader.SizeOfImage,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(image); if(!image){free(raw);return;}
    section=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        CHECK(section[i].PointerToRawData+(size_t)section[i].SizeOfRawData<=(size_t)length);
        CHECK(section[i].VirtualAddress+(size_t)section[i].SizeOfRawData<=nt->OptionalHeader.SizeOfImage);
        memcpy(image+section[i].VirtualAddress,raw+section[i].PointerToRawData,section[i].SizeOfRawData);
    }
    for(unsigned i=0;i<14;i++) {
        const ddp_pin *p=&ddp_pins[renderer][i];
        CHECK(p->rva+p->length<=nt->OptionalHeader.SizeOfImage);
        CHECK(!memcmp(image+p->rva,p->bytes,p->length));
    }
    {
        const dpa_profile *a=dpa_profiles+renderer;
        for(unsigned i=0;i<6;i++) {
            const dpa_method *m=a->methods+i; uintptr_t method;
            CHECK(m->rva+m->length<=nt->OptionalHeader.SizeOfImage);
            CHECK(!memcmp(image+m->rva,m->bytes,m->length));
            memcpy(&method,image+a->table+m->slot,8);
            CHECK(method==nt->OptionalHeader.ImageBase+m->rva);
        }
        {
            const dpa_token_binding *t=dpa_tokens+renderer; uintptr_t method;
            CHECK(t->rva+t->length<=nt->OptionalHeader.SizeOfImage);
            CHECK(!memcmp(image+t->rva,t->bytes,t->length));
            memcpy(&method,image+t->table,8);
            CHECK(method==nt->OptionalHeader.ImageBase+t->rva);
        }
    }
    {
        const ddp_context_binding *b=ddp_context_profiles+renderer;
        for(unsigned i=0;i<3;i++) {
            const ddp_context_pin *p=b->functions+i;
            CHECK(p->rva+p->length<=nt->OptionalHeader.SizeOfImage);
            CHECK(!memcmp(image+p->rva,p->bytes,p->length));
        }
        CHECK(*(uint64_t *)(image+b->game_holder)==nt->OptionalHeader.ImageBase+b->game_object);
        CHECK(*(uint64_t *)(image+b->game_table+0x240)==nt->OptionalHeader.ImageBase+b->functions[0].rva);
        CHECK(*(uint64_t *)(image+b->context_table+0x80)==nt->OptionalHeader.ImageBase+b->functions[2].rva);
    }
    {
        const ddp_pin *p=&ddp_pins[renderer][DDP_TEXT]; int32_t relative;
        memcpy(&relative,image+p->rva+3,4);
        CHECK(!memcmp(image+p->rva+7+relative,"{ }\n",5));
    }
    {
        const dpa_crt_profile *crt=dpa_crt_profiles+renderer; uintptr_t handler;unsigned mode;
        CHECK(crt->handler+8<=nt->OptionalHeader.SizeOfImage && crt->mode+4<=nt->OptionalHeader.SizeOfImage);
        memcpy(&handler,image+crt->handler,8);memcpy(&mode,image+crt->mode,4);
        CHECK(handler==nt->OptionalHeader.ImageBase+crt->default_handler && mode==0);
        CHECK(!memcmp(image+crt->throw_info,"\0\0\0\0",4));
        for(unsigned i=0;i<8;i++) {
            const dpa_method *m=crt->methods+i;
            CHECK(m->rva+m->length<=nt->OptionalHeader.SizeOfImage);
            CHECK(!memcmp(image+m->rva,m->bytes,m->length));
        }
    }
    printf("declaration defaults: %s native image pins verified\n",renderer?"Vulkan":"OpenGL");
    VirtualFree(image,0,MEM_RELEASE); free(raw);
}
int main(int argc,char **argv)
{
    if(argc>2) image_case(argv[1],atoi(argv[2])!=0);
    else {cases(0);cases(1);}
    printf("declaration defaults: %d failure(s)\n",failures); return failures?1:0;
}

