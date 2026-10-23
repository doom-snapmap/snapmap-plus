#define SH_DIAGNOSTIC_FIXTURE_ONLY
#include "decl_diagnostic_reads_test.c"
#undef SH_DIAGNOSTIC_FIXTURE_ONLY
#include "../src/backend/decl_source_file_reads.c"
#include "native_pin_fixture.h"
/* File-owner hooks are independently exercised by the actual overrides/RFV
 * suites. This fixture executes real common/allocator/diagnostic registry and
 * Source-file predicates together; it never stubs the cached-client visitor. */
static fixture *file_fixture;static int file_renderer,file_faults;
int sh_host_is_vulkan(void){return file_renderer;}
int sh_host_is_pinned_renderer_build(int im){return im==file_renderer;}
int sh_resource_graph_native_observing(void){return 1;}
int sh_resource_graph_native_observe_resource(void *object){CHECK(!object);file_faults++;return 0;}
int sh_overrides_set_file_return_observer(sh_overrides_file_open_guard guard,sh_overrides_file_return_observer observer)
{return guard==sf_opening && observer==sf_returned;}
int sh_overrides_file_return_observer_ready(sh_overrides_file_open_guard guard,sh_overrides_file_return_observer observer,const void *table,const void *open)
{const sf_profile *b=&sf_profiles[file_renderer];return guard==sf_opening && observer==sf_returned && table==file_fixture->image+b->table && open==file_fixture->image+b->open;}
int sh_overrides_file_dispatch_snapshot(void *stream,sh_overrides_file_dispatch *out)
{(void)stream;(void)out;return 0;}
int sh_resource_file_visibility_dispatch_ready(const sh_resource_file_visibility_native *n)
{sh_resource_file_visibility_native expected;sf_native(&sf_profiles[file_renderer],(uintptr_t)file_fixture->image,&expected);return !memcmp(n,&expected,sizeof expected);}
static void file_bind(fixture *f,int renderer)
{
    diag_setup(f,renderer);file_fixture=f;file_renderer=renderer;
    const sf_profile *b=&sf_profiles[renderer];
    for(size_t i=0;i<b->count;i++)CHECK(sh_native_pin_fixture_copy(renderer,b->pins[i].rva,f->image+b->pins[i].rva,b->pins[i].length));
    const size_t slots[20]={0x20,0x68,0x90,0x120,0x140,0x80,0xa8,0xb0,0xc0,0xd8,0x100,0x108,0x118,0x88,0xe0,0x60,0x70,0xf0,0x110,0xc8};
    for(unsigned i=0;i<20;i++)ptr(f->image,b->table+slots[i],diag_at(f,b->fs[i]));
    for(unsigned i=0;i<4;i++)ptr(f->image,b->table+sf_setter_slots[i],diag_at(f,b->setters[i]));
    for(unsigned i=0;i<2;i++){ptr(f->image,b->client_tables[i]+0x30,diag_at(f,b->clients[i][2]));ptr(f->image,b->client_tables[i]+0x60,diag_at(f,b->clients[i][3]));}
    const size_t atomic_slots[3]={0,0x30,0x40};for(unsigned i=0;i<3;i++)ptr(f->image,b->atomic_table+atomic_slots[i],diag_at(f,b->atomic[i]));
    ptr(f->image,b->manager,diag_at(f,0x7000000));ptr(f->image,0x7000000,diag_at(f,b->mtable));
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)f->image;dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(f->image+0x80);nt->Signature=IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=(DWORD)f->size;
    sf_base=NULL;sf_renderer=-1;CHECK(sh_decl_source_file_reads_install(f->image));
}
static int file_start(fixture *f,sh_resource_default_read_receipt *r,int full)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    return full?sh_decl_renderprog_file_read_check(r,&f->input):sh_decl_source_file_read_check(r,&f->input);
}
static void file_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;file_bind(&f,renderer);
    const sf_profile *b=&sf_profiles[renderer];const dgr_io_profile *io=&dgr_io_profiles[renderer];
    CHECK(file_start(&f,&r,0) && !r.complete && r.state==1);
    CHECK(file_start(&f,&r,1) && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    ptr(f.image,b->vcs[0],diag_at(&f,0x93000));ptr(f.image,0x93000,diag_at(&f,0x94000));
    for(size_t i=0;i<b->vcs_method_count;i++)ptr(f.image,0x94000+i*8,diag_at(&f,b->vcs_methods[i]));
    ptr(f.image,0x93008,diag_at(&f,0x95000));ptr(f.image,0x95000,diag_at(&f,0x94000));
    CHECK(file_start(&f,&r,1)); /* Ordinary initialized, populated wrapper chain. */
    ptr(f.image,0x95008,diag_at(&f,0x93000));CHECK(!file_start(&f,&r,1)&&r.reason==SH_DEFAULT_READ_OWNERSHIP);ptr(f.image,0x95008,0);
    f.denied=diag_at(&f,0x95000);CHECK(!file_start(&f,&r,1)&&r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    ptr(f.image,0x94000+8,0xabc);CHECK(!file_start(&f,&r,1)&&r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,0x94000+8,diag_at(&f,b->vcs_methods[1]));
    for(unsigned c=0;c<2;c++){
        ptr(f.image,b->object+0x180,diag_at(&f,b->callbacks[c]));CHECK(file_start(&f,&r,1));
    }
    ptr(f.image,b->object+0x180,0);
    diag_list(&f,io->mounts+8,0x80000,1,2,0);
    diag_string(&f,0x80000,"mount");diag_string(&f,0x80038,"");ptr(f.image,0x80030,diag_at(&f,0x90000));
    for(unsigned c=0;c<2;c++){
        ptr(f.image,0x90000,diag_at(&f,b->client_tables[c]));CHECK(file_start(&f,&r,1));
        uintptr_t saved;memcpy(&saved,f.image+b->client_tables[c]+0x30,8);
        ptr(f.image,b->client_tables[c]+0x30,diag_at(&f,b->clients[1-c][2]));
        CHECK(!file_start(&f,&r,1) && r.reason==SH_DEFAULT_READ_CALLBACK);
        ptr(f.image,b->client_tables[c]+0x30,saved);
        memcpy(&saved,f.image+b->client_tables[c]+0x60,8);ptr(f.image,b->client_tables[c]+0x60,0xabc);
        CHECK(!file_start(&f,&r,1)&&r.reason==SH_DEFAULT_READ_CALLBACK);
        CHECK(file_start(&f,&r,0));ptr(f.image,b->client_tables[c]+0x60,saved);
        f.denied=diag_at(&f,0x90000);CHECK(!file_start(&f,&r,1) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    }
    f.change_at=diag_at(&f,0x90000);CHECK(!file_start(&f,&r,1) && r.reason!=SH_DEFAULT_READ_UNSUPPORTED);f.change_at=0;
    ptr(f.image,0x90000,diag_at(&f,b->client_tables[0]));CHECK(!sh_resource_default_read_begin(&r,&f.input));
    f.change_at=diag_at(&f,0x90000);CHECK(!sf_client_read(NULL,&r,&f.input,diag_at(&f,0x90000)) && r.reason==SH_DEFAULT_READ_OWNERSHIP);f.change_at=0;
    ptr(f.image,0x90000,diag_at(&f,b->client_tables[0]));CHECK(file_start(&f,&r,1));
    /* The normal default receipt remains owned by its real caller. */
    CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(sh_decl_source_file_reads_uninstall());VirtualFree(f.image,0,MEM_RELEASE);sf_base=NULL;sf_renderer=-1;
}
int main(int argc,char **argv)
{
    if(argc>2){int renderer=atoi(argv[2])!=0;diag_image_case(argv[1],renderer);file_cases(renderer);}
    else {file_cases(0);file_cases(1);}
    printf("Source file composition: %d failure(s)\n",failures);return failures?1:0;
}
