#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_source_file_reads.c"
#include "native_pin_fixture.h"
static int failed,renderer,owned=1,rfv_owned=1,observing=1,faults;
static int factory_ready=1;
static int snapshot_writer;
static sh_overrides_file_return_observer observer;
static sh_overrides_file_dispatch dispatch;
static sh_overrides_file_dispatch nested_dispatch;static void *nested_issued;
static void *issued=(void *)0x1234;
static unsigned char *image;static size_t image_size=0x8000000;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);failed++;}}while(0)
int sh_host_is_vulkan(void){return renderer;}
int sh_host_is_pinned_renderer_build(int r){return r==renderer;}
int sh_resource_graph_native_observing(void){return observing;}
int sh_resource_graph_native_observe_resource(void *r){CHECK(!r);faults++;return 0;}
int sh_overrides_set_file_return_observer(sh_overrides_file_open_guard guard,sh_overrides_file_return_observer f){CHECK(guard==sf_opening);observer=f;return owned;}
int sh_overrides_file_return_observer_ready(sh_overrides_file_open_guard guard,sh_overrides_file_return_observer f,const void *table,const void *native)
{const sf_profile *b=&sf_profiles[renderer];return owned && guard==sf_opening && f==sf_returned && table==image+b->table && native==image+b->open;}
int sh_resource_file_visibility_dispatch_ready(const sh_resource_file_visibility_native *n)
{sh_resource_file_visibility_native expected;sf_native(&sf_profiles[renderer],(uintptr_t)image,&expected);return rfv_owned && !memcmp(n,&expected,sizeof expected);}
int sh_overrides_file_dispatch_snapshot(void *stream,sh_overrides_file_dispatch *out)
{if(stream!=issued && stream!=nested_issued)return 0;*out=stream==issued?dispatch:nested_dispatch;if(snapshot_writer){sf_writer_begin();sf_writer_end(GetLastError());}return 1;}
/* This isolated dispatch suite has explicit registry ownership seams; the
 * separate connected fixture executes the real diagnostic factory helper. */
int sh_decl_file_factory_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in,sh_decl_file_client_read_inspector inspector,void *context)
{(void)in;(void)inspector;(void)context;if(!factory_ready)r->reason=SH_DEFAULT_READ_CALLBACK;return factory_ready;}
int sh_decl_file_output_factory_read_check(sh_resource_default_read_receipt *r,const sh_resource_default_read_input *in)
{return sh_decl_file_factory_read_check(r,in,NULL,NULL);}
static void put(size_t offset,uintptr_t value){memcpy(image+offset,&value,8);}
static int setter_mode,setter_calls;
static void modeled_set30(void *self,void *callback)
{
    setter_calls++;CHECK(sf_writers==2);
    CHECK(!sf_opening((uintptr_t)image+sf_profiles[renderer].script,image+sf_profiles[renderer].object));
    memcpy((unsigned char *)self+0x188,&callback,8);SetLastError(772);
}
static void modeled_set28(void *self,void *callback)
{
    setter_calls++;CHECK(sf_writers==1);memcpy((unsigned char *)self+0x180,&callback,8);
    if(setter_mode==1)sf_set30(self,NULL);
    if(setter_mode==2)RaiseException(0xe1237788,0,0,NULL);
    SetLastError(771);
}
static void setup(int im)
{
    CHECK(sh_decl_source_file_reads_uninstall());
    renderer=im;memset(image,0,image_size);const sf_profile *b=&sf_profiles[im];
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER *)image;dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(image+0x80);nt->Signature=IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->OptionalHeader.SizeOfImage=(DWORD)image_size;
    for(size_t i=0;i<b->count;i++)CHECK(sh_native_pin_fixture_copy(im,b->pins[i].rva,image+b->pins[i].rva,b->pins[i].length));
    put(b->holder,(uintptr_t)image+b->object);put(b->object,(uintptr_t)image+b->table);
    put(b->manager,(uintptr_t)image+0x7000000);put(0x7000000,(uintptr_t)image+b->mtable);
    const size_t slots[20]={0x20,0x68,0x90,0x120,0x140,0x80,0xa8,0xb0,0xc0,0xd8,0x100,0x108,0x118,0x88,0xe0,0x60,0x70,0xf0,0x110,0xc8};for(unsigned i=0;i<20;i++)put(b->table+slots[i],(uintptr_t)image+b->fs[i]);
    for(unsigned i=0;i<2;i++){put(b->client_tables[i]+0x30,(uintptr_t)image+b->clients[i][2]);put(b->client_tables[i]+0x60,(uintptr_t)image+b->clients[i][3]);}
    const size_t atomic_slots[3]={0,0x30,0x40};for(unsigned i=0;i<3;i++)put(b->atomic_table+atomic_slots[i],(uintptr_t)image+b->atomic[i]);
    for(unsigned i=0;i<4;i++)put(b->table+sf_setter_slots[i],(uintptr_t)image+b->setters[i]);
    owned=rfv_owned=1;observing=1;sf_base=NULL;sf_renderer=-1;CHECK(sh_decl_source_file_reads_install(image));
}
static sh_resource_default_read_receipt receipt(sh_resource_default_read_input *in)
{
    sh_resource_default_read_receipt r;memset(&r,0,sizeof r);memset(in,0,sizeof *in);
    in->image_base=(uintptr_t)image;in->image_size=image_size;in->renderer=renderer;in->pinned_image=1;
    in->resource=0x111;in->default_callback=0x222;in->read=sf_memory;r.invocation=*in;r.state=1;r.reason=SH_DEFAULT_READ_UNSUPPORTED;return r;
}
static void vcs_setup(const sf_profile *b)
{
    put(b->vcs[0],(uintptr_t)image+0x7300000);put(0x7300000,(uintptr_t)image+0x7301000);
    for(size_t i=0;i<b->vcs_method_count;i++)put(0x7301000+i*8,(uintptr_t)image+b->vcs_methods[i]);
}
static void semantic(int im)
{
    setup(im);const sf_profile *b=&sf_profiles[im];sh_resource_default_read_input in;sh_resource_default_read_receipt r=receipt(&in);
    CHECK(sh_decl_source_file_read_check(&r,&in) && !r.complete && r.state==1 && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    r=receipt(&in);CHECK(sh_decl_renderprog_file_read_check(&r,&in));
    vcs_setup(b);r=receipt(&in);CHECK(sh_decl_renderprog_file_read_check(&r,&in));
    put(0x7300000+8,(uintptr_t)image+0x7302000);put(0x7302000,(uintptr_t)image+0x7301000);
    r=receipt(&in);CHECK(sh_decl_renderprog_file_read_check(&r,&in));
    put(0x7302000+8,(uintptr_t)image+0x7300000);r=receipt(&in);
    CHECK(!sh_decl_renderprog_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_OWNERSHIP);
    put(0x7302000+8,0);put(0x7302000,(uintptr_t)image+0x7303000);put(0x7303000+0x40,(uintptr_t)image+b->vcs_methods[1]);
    r=receipt(&in);CHECK(sh_decl_renderprog_file_read_check(&r,&in));
    put(0x7303000+0x40,0xabc);r=receipt(&in);CHECK(!sh_decl_renderprog_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_CALLBACK);
    put(0x7300000+8,1);r=receipt(&in);CHECK(!sh_decl_renderprog_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_MEMORY);
    put(0x7300000+8,0);put(0x7301000+8,0xabc);r=receipt(&in);CHECK(!sh_decl_renderprog_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_CALLBACK);
    put(0x7301000+8,(uintptr_t)image+b->vcs_methods[1]);
    /* The full adjunct qualifies actual output/metadata dispatch separately
     * from include-only input. No source edge is invented by these checks. */
    const size_t full_slots[5]={0xa8,0xc0,0xf0,0x110,0xc8};
    for(unsigned i=0;i<5;i++){uintptr_t saved;memcpy(&saved,image+b->table+full_slots[i],8);
        put(b->table+full_slots[i],0xabc);r=receipt(&in);CHECK(!sh_decl_renderprog_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_CALLBACK);
        r=receipt(&in);CHECK(sh_decl_source_file_read_check(&r,&in));put(b->table+full_slots[i],saved);}
    for(unsigned i=0;i<3;i++){const size_t slots[3]={0,0x30,0x40};uintptr_t saved;memcpy(&saved,image+b->atomic_table+slots[i],8);
        put(b->atomic_table+slots[i],0xabc);r=receipt(&in);CHECK(!sh_decl_renderprog_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_CALLBACK);put(b->atomic_table+slots[i],saved);}
    factory_ready=0;r=receipt(&in);CHECK(!sh_decl_source_file_read_check(&r,&in));factory_ready=1;
    for(unsigned c=0;c<2;c++){put(b->object+0x180,(uintptr_t)image+b->callbacks[c]);r=receipt(&in);CHECK(sh_decl_source_file_read_check(&r,&in));}
    rfv_owned=0;r=receipt(&in);CHECK(!sh_decl_source_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_CALLBACK);rfv_owned=1;
    for(size_t off=0x180;off<=0x1a0;off+=8){put(b->object+off,0x999);r=receipt(&in);CHECK(!sh_decl_source_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_CALLBACK);put(b->object+off,0);}
    owned=0;r=receipt(&in);CHECK(!sh_decl_source_file_read_check(&r,&in));owned=1;
    r=receipt(&in);in.resource++;CHECK(!sh_decl_source_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_INPUT);
    r=receipt(&in);in.work_limit=1;r.invocation=in;CHECK(!sh_decl_source_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_BUDGET);
    for(size_t i=0;i<b->count;i++){image[b->pins[i].rva]^=1;r=receipt(&in);CHECK(!sh_decl_source_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_PIN);image[b->pins[i].rva]^=1;}
    r=receipt(&in);in.image_size=10;r.invocation=in;CHECK(!sh_decl_source_file_read_check(&r,&in)&&r.reason==SH_DEFAULT_READ_IMAGE);
    memset(&dispatch,0,sizeof dispatch);dispatch.product=1;int before=faults;
    uint64_t stamp=sf_opening((uintptr_t)image+b->script,image+b->object);CHECK(stamp);
    observer((void *)0x1234,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==before);
    snapshot_writer=1;observer((void *)0x1234,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before);snapshot_writer=0;
    stamp=sf_opening((uintptr_t)image+b->script,image+b->object);CHECK(stamp);
    for(unsigned site=0;site<b->site_count;site++)if(b->sites[site].kind==2){
        uintptr_t caller=(uintptr_t)image+b->sites[site].return_pc;
        observer(issued,caller,image+b->object,stamp);CHECK(faults==++before);
        dispatch.read_string=(uintptr_t)image+b->files[0][7];observer(issued,caller,image+b->object,stamp);CHECK(faults==before);
    }
    dispatch.product=0;dispatch.table=(uintptr_t)image+0x7100000;
    for(unsigned row=0;row<6;row++){
        nested_issued=NULL;
        for(unsigned k=0;k<5;k++)dispatch.slots[k]=(uintptr_t)image+b->files[row][k];
        put(0x7100000+0x20,(uintptr_t)image+b->files[row][5]);put(0x7100000+0x38,(uintptr_t)image+b->files[row][6]);
        if(row==2){
            issued=image+0x7200000;nested_issued=image+0x7202000;
            put(0x7200000+0x70,(uintptr_t)image+0x7201000);put(0x7201000,(uintptr_t)nested_issued);put(0x7201000+0x80,(uintptr_t)image+0x7203000);
            put(0x7203000+0x88,(uintptr_t)nested_issued);int method=8,initialized=1;
            memcpy(image+0x7203000+0x90,&method,4);memcpy(image+0x7203000+0x64,&initialized,4);
            for(unsigned k=0;k<2;k++)put(0x7203000+0x38+k*8,(uintptr_t)image+b->zip_alloc[k]);
            memset(&nested_dispatch,0,sizeof nested_dispatch);nested_dispatch.table=(uintptr_t)image+0x7204000;
            for(unsigned k=0;k<5;k++)nested_dispatch.slots[k]=(uintptr_t)image+b->files[0][k];
            nested_dispatch.tell=(uintptr_t)image+b->files[0][8];
            put(0x7204000+0x20,(uintptr_t)image+b->files[0][5]);put(0x7204000+0x38,(uintptr_t)image+b->files[0][6]);put(0x7204000+0x70,(uintptr_t)image+b->files[0][10]);
        }
        /* NFS inspects the actual file+f8 and all three reached client
         * callbacks; copied tables are qualified by dispatch, not RTTI. */
        if(row==4){
            issued=image+0x7200000;put(0x7200000+0xf8,(uintptr_t)image+0x7201000);put(0x7201000,(uintptr_t)image+0x7202000);
            for(unsigned c=0;c<2;c++){
                const size_t off[3]={0,0x28,0x30};for(unsigned k=0;k<3;k++)put(0x7202000+off[k],(uintptr_t)image+b->clients[c][k]);
                observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==before);
                put(0x7202000+0x30,0xabc);observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before);
            }
            put(0x7200000+0xf8,0); /* Native missing client returns zero. */
        }
        observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==before);
        if(row==2){
            put(0x7204000+0x70,0xabc);observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before);put(0x7204000+0x70,(uintptr_t)image+b->files[0][10]);
            put(0x7203000+0x88,0xabc);observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before);put(0x7203000+0x88,(uintptr_t)nested_issued);
            put(0x7203000+0x38,0xabc);observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before);put(0x7203000+0x38,(uintptr_t)image+b->zip_alloc[0]);
            put(0x7201000,(uintptr_t)issued);put(0x7203000+0x88,(uintptr_t)issued);observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before);
            put(0x7201000,(uintptr_t)nested_issued);put(0x7203000+0x88,(uintptr_t)nested_issued);
            put(0x7201000+0x80,0);observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==before);put(0x7201000+0x80,(uintptr_t)image+0x7203000);
        }
        dispatch.read_string=(uintptr_t)image+b->files[row][7];dispatch.tell=(uintptr_t)image+b->files[row][8];
        dispatch.receipt=dispatch.shader=1;
        put(0x7100000+0xa0,(uintptr_t)image+b->files[row][9]);put(0x7100000+0x70,(uintptr_t)image+b->files[row][10]);
        for(unsigned site=0;site<b->site_count;site++){
            uintptr_t caller=(uintptr_t)image+b->sites[site].return_pc;
            uint64_t current=sf_opening(caller,image+b->object);CHECK(current);
            observer(issued,caller,image+b->object,current);CHECK(faults==before);
            if(b->sites[site].kind==8){
                /* GenericWrite can reach the non-decl branch after native
                 * formatting truncation. Its actual returned Length,
                 * Timestamp, Read and destructor are required. */
                for(unsigned k=0;k<5;k++){uintptr_t saved=dispatch.slots[k];dispatch.slots[k]=0xabc;
                    observer(issued,caller,image+b->object,current);CHECK(faults==++before);dispatch.slots[k]=saved;}
            }
            if(b->sites[site].kind==2){dispatch.read_string=0xabc;observer(issued,caller,image+b->object,current);CHECK(faults==++before);dispatch.read_string=(uintptr_t)image+b->files[row][7];}
            dispatch.tell=0xabc;observer(issued,caller,image+b->object,current);CHECK(faults==++before);dispatch.tell=(uintptr_t)image+b->files[row][8];
            if(b->sites[site].kind==4 || b->sites[site].kind==7){put(0x7100000+0xa0,0xabc);observer(issued,caller,image+b->object,current);CHECK(faults==++before);put(0x7100000+0xa0,(uintptr_t)image+b->files[row][9]);}
            if(b->sites[site].kind==6){put(0x7100000+0x70,0xabc);observer(issued,caller,image+b->object,current);CHECK(faults==++before);put(0x7100000+0x70,(uintptr_t)image+b->files[row][10]);}
        }
        dispatch.receipt=dispatch.shader=0;
        put(0x7100000+0x38,0xabc);observer(issued,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before);
        issued=(void *)0x1234;
    }
    nested_issued=NULL;
    observer((void *)0x1234,(uintptr_t)image+b->script+1,image+b->object,stamp);CHECK(faults==before);
    observing=0;observer((void *)0x1234,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==before);observing=1;
    observer(NULL,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==before);
    sf_writer_begin();CHECK(!sf_opening((uintptr_t)image+b->script,image+b->object));
    sf_writer_end(771);observer(NULL,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before && GetLastError()==771);
    stamp=sf_opening((uintptr_t)image+b->script,image+b->object);CHECK(stamp);
    void *saved_setters[4];memcpy(saved_setters,sf_original_setters,sizeof saved_setters);
    sf_original_setters[0]=(void *)modeled_set28;sf_original_setters[1]=(void *)modeled_set30;
    setter_mode=1;sf_set28(image+b->object,(void *)(image+b->callbacks[0]));
    CHECK(setter_calls>=2 && !sf_writers && !(sf_generation&1) && GetLastError()==771);
    setter_mode=2;int raised=0;__try {sf_set28(image+b->object,NULL);}__except(EXCEPTION_EXECUTE_HANDLER){raised=GetExceptionCode()==0xe1237788;}
    CHECK(raised && !sf_writers && !(sf_generation&1));
    memcpy(sf_original_setters,saved_setters,sizeof saved_setters);
    observer(NULL,(uintptr_t)image+b->script,image+b->object,stamp);CHECK(faults==++before);
    void *old=*(void **)(image+b->table+0x30);put(b->table+0x30,0xabc);CHECK(!sh_decl_source_file_reads_ready());put(b->table+0x30,(uintptr_t)old);
    CHECK(sh_decl_source_file_reads_ready());CHECK(sh_decl_source_file_reads_uninstall());CHECK(!sh_decl_source_file_reads_ready());
}
static void actual_image(const char *path,int im)
{
    FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    unsigned char *raw=malloc(n);CHECK(raw);if(!raw){fclose(f);return;}CHECK(fread(raw,1,n,f)==(size_t)n);fclose(f);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    const sf_profile *b=&sf_profiles[im];
    for(size_t i=0;i<b->count;i++){const sf_pin *pin=b->pins+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){
                size_t at=s->PointerToRawData+pin->rva-s->VirtualAddress;CHECK(at<=(size_t)n && pin->length<=(size_t)n-at);
                if(at<=(size_t)n && pin->length<=(size_t)n-at){sh_native_pin_hash hash;unsigned char digest[32];
                    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,raw+at,pin->length);sh_native_pin_hash_finish(&hash,digest);CHECK(!memcmp(digest,pin->digest,32));}found=1;break;}}
        CHECK(found);
    }free(raw);
}
int main(int argc,char **argv)
{
    image=VirtualAlloc(NULL,image_size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(image);if(!image)return 1;
    if(argc>2){int im=atoi(argv[2]);actual_image(argv[1],im);semantic(im);}else {semantic(0);semantic(1);}
    VirtualFree(image,0,MEM_RELEASE);if(failed)return 1;puts("source file dispatch controls passed");return 0;
}
