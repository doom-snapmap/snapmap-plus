/* Reuse the real common envelope, allocator and token callback fixture. */
#define main sh_numeric_common_test_main
#include "decl_default_reads_test.c"
#undef main
#include "../src/backend/decl_numeric_default_reads.c"
static int parm_ready=1;
int sh_resource_parm_reads_ready(void) { return parm_ready; }

static uintptr_t numeric_address(fixture *f,unsigned which)
{ return (uintptr_t)f->image+ndp_pins[f->input.renderer][which].rva; }
static uintptr_t numeric_text(fixture *f,unsigned which)
{
    int32_t relative; uintptr_t entry=numeric_address(f,which);
    memcpy(&relative,(void *)(entry+3),4); return entry+7+relative;
}
static void numeric_select(fixture *f,unsigned which)
{
    ptr(f->table,0x98,numeric_address(f,which)); ptr(f->table,0xa0,numeric_address(f,which+1));
    ptr(f->table,0x88,numeric_address(f,which+2));
}
static int numeric_start(fixture *f,sh_resource_default_read_receipt *r)
{
    CHECK(!sh_resource_default_read_begin(r,&f->input));
    CHECK(!sh_decl_default_read_begin(r,&f->input));
    if(r->reason!=SH_DEFAULT_READ_UNSUPPORTED) return 0;
    return sh_decl_numeric_default_read_begin(r,&f->input);
}
static uintptr_t numeric_manager(fixture *f)
{ return (uintptr_t)f->image+0x500000; }
static uintptr_t numeric_definition(fixture *f,unsigned i)
{ return (uintptr_t)f->image+0x540000+(uintptr_t)i*0x100; }
static uintptr_t numeric_temporary(fixture *f,unsigned i)
{ return (uintptr_t)f->image+0x550000+(uintptr_t)i*0x100; }
static void numeric_setup(fixture *f,int renderer)
{
    uintptr_t manager;
    setup(f,renderer); manager=numeric_manager(f);
    for(unsigned i=0;i<NDP_PINS;i++)
        memcpy((void *)numeric_address(f,i),ndp_pins[renderer][i].bytes,ndp_pins[renderer][i].length);
    memcpy((void *)numeric_text(f,NDP_TABLE_TEXT),ndp_table_definition,sizeof ndp_table_definition);
    memcpy((void *)numeric_text(f,NDP_PARM_TEXT),ndp_parm_definition,sizeof ndp_parm_definition);
    memcpy((void *)numeric_text(f,NDP_ENV_TEXT),ndp_env_definition,sizeof ndp_env_definition);
    ptr(f->image,ndp_managers[renderer],manager);
    ptr(f->object,0x60,(uintptr_t)f->image+0x3000);
    for(unsigned i=1;i<NDP_ENV_KEYS;i++) {
        uintptr_t object=numeric_definition(f,i),name=(uintptr_t)f->image+0x560000+i*0x80;
        ptr((void *)object,0,(uintptr_t)f->image+ndp_parm_tables[renderer]);
        ptr((void *)object,8,name); memcpy((void *)name,ndp_env_names[i],strlen(ndp_env_names[i])+1);
        number((void *)object,0x68,(int)i); number((void *)object,0x6c,0);
        ptr((void *)manager,i*8,object);
    }
    for(unsigned i=0;i<128;i++) {
        uintptr_t object=numeric_temporary(f,i); int index=(int)(NDP_ENV_KEYS+i);
        ptr((void *)object,0,(uintptr_t)f->image+ndp_parm_tables[renderer]);
        ptr((void *)object,8,(uintptr_t)f->image+0x570000);
        memcpy(f->image+0x570000,"temporary",10);
        number((void *)object,0x68,index);number((void *)object,0x6c,0);number((void *)object,0x74,2);
        ptr((void *)manager,(size_t)index*8,object);
        ptr(f->image,ndp_temporary_heads[renderer]+i*8,object);
    }
    ptr(f->image,ndp_env_handles[renderer][0],numeric_definition(f,1));
    ptr(f->image,ndp_env_handles[renderer][1],numeric_definition(f,2));
}
static void numeric_cases(int renderer)
{
    fixture f; sh_resource_default_read_receipt r; numeric_setup(&f,renderer);
    uintptr_t manager=numeric_manager(&f),curve=(uintptr_t)f.image+0x3000;
    for(unsigned selected=0;selected<NDP_PINS;selected+=3) {
        numeric_select(&f,selected);
        if(selected==NDP_PARM) number(f.object,0x68,0x93f);
        CHECK(numeric_start(&f,&r) && r.complete && r.reason==SH_DEFAULT_READ_OK);
        CHECK(sh_resource_default_read_finish(&r,&f.input,1,1));
        CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
        CHECK(!sh_decl_numeric_default_read_begin(&r,&f.input));
        CHECK(numeric_start(&f,&r)); CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
        CHECK(numeric_start(&f,&r)); CHECK(!sh_resource_default_read_finish(&r,&f.input,1,0));
        for(unsigned role=selected;role<selected+3;role++) {
            for(unsigned byte=0;byte<ndp_pins[renderer][role].length;byte++) {
                *(unsigned char *)(numeric_address(&f,role)+byte)^=1;
                CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN && !r.complete);
                *(unsigned char *)(numeric_address(&f,role)+byte)^=1;
            }
        }
        uintptr_t text=numeric_text(&f,selected+2);
        *(char *)text='x'; CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN); *(char *)text='{';
        ptr(f.table,0xa0,numeric_address(&f,selected+1)+1);
        CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);numeric_select(&f,selected);
        ptr(f.table,0x88,numeric_address(&f,selected+2)+1);
        CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);numeric_select(&f,selected);
        f.denied=text;CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
        f.input.work_limit=1;CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    }
    numeric_select(&f,NDP_TABLE);
    memset((void *)curve,0xa5,0x220); CHECK(numeric_start(&f,&r)); /* Old count/values are reset. */
    f.denied=curve;CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    ptr(f.object,0x60,0);CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);ptr(f.object,0x60,curve);
    {
        uintptr_t node=(uintptr_t)f.image+0x4000,name=(uintptr_t)f.image+0x4100;
        ptr(f.image,ddp_define_heads[renderer],node);ptr((void *)node,0x10,name);
        memcpy((void *)name,"clamp",6);CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        memcpy((void *)name,"CLAMP",6);CHECK(numeric_start(&f,&r));
        ptr((void *)node,0x50,node);CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        ptr((void *)node,0x50,0);ptr(f.image,ddp_define_heads[renderer],0);
    }
    numeric_select(&f,NDP_PARM);
    for(int index=0;index<0x940;index+=0x93f) {number(f.object,0x68,index);CHECK(numeric_start(&f,&r));}
    number(f.object,0x68,-1);CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);
    number(f.object,0x68,0x93f);
    number((void *)manager,0x10190,1);ptr((void *)manager,0x10188,(uintptr_t)f.image+0x580000);
    ptr(f.image,0x580000,(uintptr_t)f.image+0x590000);number(f.image,0x590000+0xb97c,0xffffffff);
    CHECK(numeric_start(&f,&r)); /* Dirty rollback is an owned numeric contract. */
    f.denied=(uintptr_t)f.image+0x590000+0xb97c;
    CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    number((void *)manager,0x10190,-1);CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    number((void *)manager,0x10190,0);
    numeric_select(&f,NDP_ENV);
    CHECK(numeric_start(&f,&r));
    parm_ready=0;CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);parm_ready=1;
    for(unsigned i=1;i<NDP_ENV_KEYS;i++) {
        uintptr_t object=numeric_definition(&f,i);
        number((void *)object,0x6c,1);CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_TYPE);
        number((void *)object,0x6c,0);
    }
    ptr((void *)manager,8,0);CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr((void *)manager,8,numeric_definition(&f,1));
    ptr((void *)numeric_definition(&f,1),0x18,1);
    CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    ptr((void *)numeric_definition(&f,1),0x18,0);
    *(unsigned char *)(numeric_definition(&f,1)+0x2c)=1;
    CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    *(unsigned char *)(numeric_definition(&f,1)+0x2c)=0;
    number((void *)numeric_definition(&f,1),0x68,2);
    CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    number((void *)numeric_definition(&f,1),0x68,1);
    number((void *)numeric_temporary(&f,127),0x74,0);
    CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
    number((void *)numeric_temporary(&f,127),0x74,2);
    ptr(f.image,ndp_temporary_heads[renderer],0);CHECK(numeric_start(&f,&r));
    ptr(f.image,ndp_temporary_heads[renderer],numeric_temporary(&f,0));
    ptr(f.image,ndp_env_handles[renderer][0],0);
    CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_BOUNDS);
    ptr(f.image,ndp_env_handles[renderer][0],numeric_definition(&f,1));
    {
        uintptr_t node=(uintptr_t)f.image+0x4000,name=(uintptr_t)f.image+0x4100;
        ptr(f.image,ddp_define_heads[renderer],node);ptr((void *)node,0x10,name);
        memcpy((void *)name,ndp_env_names[NDP_ENV_KEYS-1],strlen(ndp_env_names[NDP_ENV_KEYS-1])+1);
        CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);
        ptr(f.image,ddp_define_heads[renderer],0);
    }
    f.object[0x6b]=1; f.object[0x83]=1; /* Borrowed old numeric buffers. */
    number(f.object,0x60,71); number(f.object,0x78,71);CHECK(numeric_start(&f,&r));
    f.denied=(uintptr_t)f.object+0x58;
    CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    CHECK(numeric_start(&f,&r)); f.input.read_context=NULL;
    CHECK(!sh_decl_numeric_default_read_begin(&r,&f.input)); f.input.read_context=&f;
    CHECK(numeric_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,0,1));
    f.input.renderer=1-renderer;CHECK(!numeric_start(&f,&r));f.input.renderer=renderer;
    f.input.pinned_image=0;CHECK(!numeric_start(&f,&r));f.input.pinned_image=1;
    f.input.image_size=0x1000;CHECK(!numeric_start(&f,&r));f.input.image_size=f.size;
    numeric_select(&f,NDP_TABLE);ptr(f.table,0x98,(uintptr_t)f.image+0x2000);
    CHECK(!numeric_start(&f,&r) && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void numeric_image_case(const char *path,int renderer)
{
    image_case(path,renderer);
    FILE *file=fopen(path,"rb");long length;uint8_t *raw,*image;
    IMAGE_NT_HEADERS64 *nt;IMAGE_SECTION_HEADER *section;
    CHECK(file);if(!file)return;
    fseek(file,0,SEEK_END);length=ftell(file);rewind(file);raw=malloc(length);CHECK(raw);
    if(!raw){fclose(file);return;}CHECK(fread(raw,1,length,file)==(size_t)length);fclose(file);
    nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    image=VirtualAlloc(NULL,nt->OptionalHeader.SizeOfImage,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(image);if(!image){free(raw);return;}section=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        CHECK(section[i].PointerToRawData+(size_t)section[i].SizeOfRawData<=(size_t)length);
        CHECK(section[i].VirtualAddress+(size_t)section[i].SizeOfRawData<=nt->OptionalHeader.SizeOfImage);
        memcpy(image+section[i].VirtualAddress,raw+section[i].PointerToRawData,section[i].SizeOfRawData);
    }
    for(unsigned i=0;i<NDP_PINS;i++) {
        const ndp_pin *pin=ndp_pins[renderer]+i;
        CHECK(pin->rva+pin->length<=nt->OptionalHeader.SizeOfImage);
        CHECK(!memcmp(image+pin->rva,pin->bytes,pin->length));
    }
    const char *definitions[]={ndp_table_definition,ndp_parm_definition,ndp_env_definition};
    const size_t sizes[]={sizeof ndp_table_definition,sizeof ndp_parm_definition,sizeof ndp_env_definition};
    for(unsigned i=0;i<3;i++) {
        const ndp_pin *pin=ndp_pins[renderer]+i*3+2;int32_t relative;
        memcpy(&relative,pin->bytes+3,4);uintptr_t target=(uintptr_t)pin->rva+7+relative;
        CHECK(target<nt->OptionalHeader.SizeOfImage && sizes[i]<=nt->OptionalHeader.SizeOfImage-target);
        CHECK(!memcmp(image+target,definitions[i],sizes[i]));
    }
    VirtualFree(image,0,MEM_RELEASE);free(raw);
    printf("numeric declaration defaults: %s native pins verified\n",renderer?"Vulkan":"OpenGL");
}
int main(int argc,char **argv)
{
    if(argc>2)numeric_image_case(argv[1],atoi(argv[2])!=0);
    else {numeric_cases(0);numeric_cases(1);}
    printf("numeric declaration defaults: %d failure(s)\n",failures);return failures?1:0;
}
