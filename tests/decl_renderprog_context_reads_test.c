#define main sh_rpx_common_test_main
#include "decl_default_reads_test.c"
#undef main
#include "../src/backend/decl_renderprog_context_reads.c"

static const uint32_t rpx_tables[2]={0x27a1480,0x278b270};
static const uint32_t rpx_dtors[2]={0x19eff00,0x19fdca0};
static void rpx_test_string(fixture *f,size_t offset)
{
    ptr(f->image,offset,(uintptr_t)f->image+rpx_tables[f->input.renderer]);
    ptr(f->image,offset+0x10,(uintptr_t)f->image+offset+0x1c);
    number(f->image,offset+0x18,0x80000014u);
}
static void rpx_setup(fixture *f,int renderer)
{
    setup(f,renderer);f->input.resource=(uintptr_t)f->image+0x10000;
    ptr(f->image,0x10000,(uintptr_t)f->table);
    ptr(f->image,rpx_tables[renderer],(uintptr_t)f->image+rpx_dtors[renderer]);
    rpx_test_string(f,0x10078+8);
    for(unsigned i=0;i<64;i++)rpx_test_string(f,0x10078+0x9e8+i*0x30);
    for(unsigned i=0;i<3;i++)rpx_test_string(f,0x10078+0x16f8+i*0x30);
}
static int rpx_start(fixture *f,sh_resource_default_read_receipt *r)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    return sh_decl_renderprog_context_read_check(r,&f->input);
}
static void rpx_header(fixture *f,size_t at,size_t data,int count,int capacity,unsigned char flag)
{
    ptr(f->image,at,(uintptr_t)f->image+data);number(f->image,at+8,count);number(f->image,at+12,capacity);
    f->image[at+0x13]=flag;
}
static void rpx_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;rpx_setup(&f,renderer);
    CHECK(rpx_start(&f,&r) && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    /* A count-zero string allocation still contains destructible capacity.
     * All ownership flags retain their native meaning and are accepted. */
    rpx_header(&f,0x10078+0x38,0x50000,0,2,0);
    rpx_test_string(&f,0x50000);rpx_test_string(&f,0x50030);
    for(unsigned flag=0;flag<5;flag++){f.image[0x10078+0x38+0x13]=(unsigned char)flag;CHECK(rpx_start(&f,&r));}
    ptr(f.image,0x50030,0);CHECK(!rpx_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    rpx_test_string(&f,0x50030);
    f.denied=(uintptr_t)f.image+0x50030;CHECK(!rpx_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    number(f.image,0x50030+8,21);CHECK(!rpx_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.image,0x50030+8,0);
    /* Numeric/handle backing is inspected as storage, never resolved as a
     * resource. Arbitrary words therefore do not create dependency records. */
    rpx_header(&f,0x10078+0x70,0x51000,2,2,2);memset(f.image+0x51000,0xff,16);CHECK(rpx_start(&f,&r));
    number(f.image,0x10078+0x70+8,3);CHECK(!rpx_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.image,0x10078+0x70+8,2);
    ptr(f.image,0x10078+0x70,UINTPTR_MAX-7);CHECK(!rpx_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);
    ptr(f.image,0x10078+0x70,(uintptr_t)f.image+0x51000);
    uintptr_t table=(uintptr_t)f.image+rpx_tables[renderer],callback=(uintptr_t)f.image+rpx_dtors[renderer];
    ptr((void *)table,0,callback+1);CHECK(!rpx_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr((void *)table,0,callback);
    f.change_at=table;CHECK(!rpx_start(&f,&r) && r.reason==SH_DEFAULT_READ_INVOCATION);f.change_at=0;ptr((void *)table,0,callback);
    CHECK(rpx_start(&f,&r));f.input.work_limit=r.work-1;CHECK(!rpx_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(rpx_start(&f,&r));f.input.resource++;CHECK(!sh_decl_renderprog_context_read_check(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.resource--;
    CHECK(rpx_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_decl_renderprog_context_read_check(&r,&f.input));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void rpx_image(const char *path,int renderer)
{
    FILE *file=fopen(path,"rb");CHECK(file);if(!file)return;
    fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);unsigned char *raw=malloc(length);CHECK(raw);
    if(!raw){fclose(file);return;}CHECK(fread(raw,1,length,file)==(size_t)length);fclose(file);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    int found=0;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        IMAGE_SECTION_HEADER *s=sections+i;uint32_t rva=rpx_tables[renderer];
        if(rva>=s->VirtualAddress && rva-s->VirtualAddress<=s->SizeOfRawData && 8<=s->SizeOfRawData-(rva-s->VirtualAddress)) {
            size_t offset=s->PointerToRawData+rva-s->VirtualAddress;uintptr_t callback=0;
            CHECK(offset<=(size_t)length && 8<=(size_t)length-offset);
            if(offset<=(size_t)length && 8<=(size_t)length-offset)memcpy(&callback,raw+offset,8);
            CHECK(callback==nt->OptionalHeader.ImageBase+rpx_dtors[renderer]);found=1;break;
        }
    }
    CHECK(found);free(raw);
}
int main(int argc,char **argv)
{if(argc==3)rpx_image(argv[1],atoi(argv[2])!=0);else{rpx_cases(0);rpx_cases(1);}printf("RenderProg context storage: %d failure(s)\n",failures);return failures?1:0;}
