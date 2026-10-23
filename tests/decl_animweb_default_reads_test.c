/* Real generic envelope, allocator, token and names predicates. */
#define main sh_animweb_common_test_main
#include "decl_default_reads_test.c"
#undef main
static int aw_fail_allocation;
static void *aw_test_realloc(void *p,size_t size)
{ if(aw_fail_allocation){aw_fail_allocation=0;return NULL;}return realloc(p,size); }
#define realloc aw_test_realloc
#include "../src/backend/decl_animweb_default_reads.c"
#undef realloc
/* Storage controls use an explicit diagnostic seam; the diagnostic suite
 * composes this actual predicate with all six real receiver contracts. */
static int animweb_diagnostic_ready=1;
int sh_decl_diagnostic_read_check(sh_resource_default_read_receipt *r,
                                const sh_resource_default_read_input *in,unsigned severity)
{
    CHECK(in && severity==1 && !r->complete);
    if(!animweb_diagnostic_ready)r->reason=SH_DEFAULT_READ_CALLBACK;
    return animweb_diagnostic_ready;
}
static uintptr_t animweb_address(fixture *f,unsigned which)
{ return (uintptr_t)f->image+aw_pins[f->input.renderer][which].rva; }
static uintptr_t animweb_text(fixture *f)
{ int32_t relative;uintptr_t at=animweb_address(f,AW_TEXT);memcpy(&relative,(void *)(at+3),4);return at+7+relative; }
static int animweb_start(fixture *f,sh_resource_default_read_receipt *r)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));CHECK(!sh_decl_default_read_begin(r,&f->input));
    if(r->reason!=SH_DEFAULT_READ_UNSUPPORTED)return 0;
    return sh_decl_animweb_default_read_begin(r,&f->input);
}
static uintptr_t storage(fixture *f,size_t offset) {return (uintptr_t)f->image+offset;}
static void list(fixture *f,void *at,size_t offset,size_t data,int count,int capacity,int flags)
{
    ptr(at,offset,data?storage(f,data):0);number(at,offset+8,count);number(at,offset+0xc,capacity);
    *((unsigned char *)at+offset+0x13)=(unsigned char)flags;
}
static void animweb_setup(fixture *f,int renderer)
{
    setup(f,renderer);
    for(unsigned i=0;i<AW_PINS;i++)memcpy((void *)animweb_address(f,i),aw_pins[renderer][i].bytes,aw_pins[renderer][i].length);
    memcpy((void *)animweb_text(f),aw_definition,sizeof aw_definition);
    ptr(f->table,0x98,animweb_address(f,AW_PARSE));ptr(f->table,0xa0,animweb_address(f,AW_RELEASE));
    ptr(f->table,0x88,animweb_address(f,AW_TEXT));
}
static void populated(fixture *f)
{
    list(f,f->object,0x58,0x21000,1,2,0);ptr(f->image,0x21000,storage(f,0x22000));
    ptr(f->image,0x21008,0xdeadbeef); /* Native node cleanup uses count, not capacity. */
    list(f,f->image,0x22000+0x20,0x22100,1,2,0);
    list(f,f->image,0x22000+0x38,0x22200,1,2,0);
    list(f,f->image,0x22200+0x18,0x22500,1,2,0);
    ptr(f->image,0x22200+0x30,storage(f,0x22300));ptr(f->image,0x22200+0x38,storage(f,0x22400));
    list(f,f->image,0x22300,0x22600,1,2,0);
    list(f,f->image,0x22400,0x22700,1,2,0);
    list(f,f->image,0x22700+8,0x22800,1,2,0);list(f,f->image,0x22720+8,0x22900,0,1,0);
    list(f,f->image,0x22240+0x18,0x22a00,0,1,0); /* Spare property is destroyed. */
    list(f,f->object,0x118,0x23000,1,2,0);
    list(f,f->image,0x23000+8,0x23800,1,2,0);list(f,f->image,0x23028+8,0x23900,0,2,0);
    list(f,f->object,0x70,0x24000,1,2,0);
    list(f,f->image,0x24000,0x24800,1,2,0);list(f,f->image,0x24038,0x24900,0,2,0);
    list(f,f->object,0xb8,0x25000,1,2,0);
    ptr(f->image,0x25000,storage(f,0x26000));ptr(f->image,0x25040,storage(f,0x26010));
    ptr(f->image,0x26000,animweb_address(f,AW_DELETE));ptr(f->image,0x26010,animweb_address(f,AW_DELETE));
    ptr(f->object,0x130,storage(f,0x27000));
}
static void animweb_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;animweb_setup(&f,renderer);
    CHECK(animweb_start(&f,&r) && r.complete && r.reason==SH_DEFAULT_READ_OK);
    CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    populated(&f);CHECK(animweb_start(&f,&r));
    f.object[0x6b]=3;f.object[0x83]=4;f.object[0xcb]=4;
    *(unsigned char *)(storage(&f,0x22000)+0x4b)=3;
    CHECK(animweb_start(&f,&r));
    f.object[0x6b]=0;f.object[0x83]=0;f.object[0xcb]=0;
    *(unsigned char *)(storage(&f,0x22000)+0x4b)=0;
    for(unsigned i=0;i<AW_PINS;i++)for(unsigned j=0;j<aw_pins[renderer][i].length;j++) {
        *(unsigned char *)(animweb_address(&f,i)+j)^=1;
        CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN && !r.complete);
        *(unsigned char *)(animweb_address(&f,i)+j)^=1;
    }
    ptr(f.image,0x26010,animweb_address(&f,AW_DELETE)+1);
    CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    number(f.object,0xc0,0);CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
    f.object[0xcb]=2;CHECK(animweb_start(&f,&r)); /* Borrowed virtual array has no destructor calls. */
    f.object[0xcb]=0;ptr(f.image,0x26010,animweb_address(&f,AW_DELETE));number(f.object,0xc0,1);
    f.denied=storage(&f,0x22240)+0x30;CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);
    f.object[0x6b]=2;CHECK(!animweb_start(&f,&r)); /* Borrowed node list still destroys count nodes. */
    *(unsigned char *)(storage(&f,0x22000)+0x4b)=2;CHECK(animweb_start(&f,&r));
    *(unsigned char *)(storage(&f,0x22000)+0x4b)=0;f.object[0x6b]=0;f.denied=0;
    ptr(f.image,0x21008,storage(&f,0x22000));number(f.object,0x60,2);
    CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.object,0x60,1);
    ptr(f.image,0x22200+0x30,storage(&f,0x22200));
    CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr(f.image,0x22200+0x30,storage(&f,0x22300));
    ptr(f.object,0x130,storage(&f,0x22100));CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr(f.object,0x130,storage(&f,0x27000));
    ptr(f.image,0x21000,UINTPTR_MAX-0x20);
    CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);
    ptr(f.image,0x21000,storage(&f,0x22000));
    f.object[0x6b]=2;ptr(f.object,0x58,UINTPTR_MAX-3);
    CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);
    f.object[0x6b]=0;ptr(f.object,0x58,storage(&f,0x21000));
    number(f.object,0x60,-1);CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    number(f.object,0x60,1);number(f.object,0xc4,-1);
    CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.object,0xc4,2);
    aw_fail_allocation=1;CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_ALLOCATION);
    {
        uintptr_t node=storage(&f,0x28000),name=storage(&f,0x28100);
        ptr(f.image,ddp_define_heads[renderer],node);ptr((void *)node,0x10,name);
        memcpy((void *)name,"states",7);CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        f.object[0x15a]=1;CHECK(animweb_start(&f,&r)); /* Only props is reached for empty composite input. */
        animweb_diagnostic_ready=0;CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
        animweb_diagnostic_ready=1;
        memcpy((void *)name,"props",6);CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        memcpy((void *)name,"PROPS",6);CHECK(animweb_start(&f,&r));
        ptr((void *)node,0x50,node);CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        ptr((void *)node,0x50,0);ptr(f.image,ddp_define_heads[renderer],0);f.object[0x15a]=0;
    }
    *(char *)animweb_text(&f)='x';CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);
    *(char *)animweb_text(&f)='{';
    ptr(f.table,0xa0,animweb_address(&f,AW_RELEASE)+1);
    CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.table,0xa0,animweb_address(&f,AW_RELEASE));
    CHECK(animweb_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    CHECK(animweb_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
    f.input.work_limit=20;CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(animweb_start(&f,&r));f.input.read_context=NULL;CHECK(!sh_decl_animweb_default_read_begin(&r,&f.input));f.input.read_context=&f;
    f.input.renderer=1-renderer;CHECK(!animweb_start(&f,&r));f.input.renderer=renderer;
    f.input.pinned_image=0;CHECK(!animweb_start(&f,&r));f.input.pinned_image=1;
    f.input.image_size=0x1000;CHECK(!animweb_start(&f,&r));f.input.image_size=f.size;
    ptr(f.table,0x98,(uintptr_t)f.image+0x2000);
    CHECK(!animweb_start(&f,&r) && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void animweb_image_case(const char *path,int renderer)
{
    image_case(path,renderer);
    FILE *file=fopen(path,"rb");long length;unsigned char *raw,*image;IMAGE_NT_HEADERS64 *nt;IMAGE_SECTION_HEADER *section;
    CHECK(file);if(!file)return;fseek(file,0,SEEK_END);length=ftell(file);rewind(file);
    raw=malloc(length);CHECK(raw);if(!raw){fclose(file);return;}CHECK(fread(raw,1,length,file)==(size_t)length);fclose(file);
    nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);section=IMAGE_FIRST_SECTION(nt);
    image=VirtualAlloc(NULL,nt->OptionalHeader.SizeOfImage,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(image);if(!image){free(raw);return;}
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        CHECK(section[i].PointerToRawData+(size_t)section[i].SizeOfRawData<=(size_t)length);
        CHECK(section[i].VirtualAddress+(size_t)section[i].SizeOfRawData<=nt->OptionalHeader.SizeOfImage);
        memcpy(image+section[i].VirtualAddress,raw+section[i].PointerToRawData,section[i].SizeOfRawData);
    }
    for(unsigned i=0;i<AW_PINS;i++) {
        const aw_pin *pin=aw_pins[renderer]+i;CHECK(pin->rva+pin->length<=nt->OptionalHeader.SizeOfImage);
        CHECK(!memcmp(image+pin->rva,pin->bytes,pin->length));
    }
    const aw_pin *pin=aw_pins[renderer]+AW_TEXT;int32_t relative;memcpy(&relative,pin->bytes+3,4);
    uintptr_t target=(uintptr_t)pin->rva+7+relative;
    CHECK(target<nt->OptionalHeader.SizeOfImage && sizeof aw_definition<=nt->OptionalHeader.SizeOfImage-target);
    CHECK(!memcmp(image+target,aw_definition,sizeof aw_definition));
    VirtualFree(image,0,MEM_RELEASE);free(raw);
    printf("AnimWeb defaults: %s native pins verified\n",renderer?"Vulkan":"OpenGL");
}
int main(int argc,char **argv)
{
    if(argc>2)animweb_image_case(argv[1],atoi(argv[2])!=0);else{animweb_cases(0);animweb_cases(1);}
    printf("AnimWeb defaults: %d failure(s)\n",failures);return failures?1:0;
}
