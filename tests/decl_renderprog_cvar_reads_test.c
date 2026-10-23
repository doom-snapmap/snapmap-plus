#define main sh_rpc_common_test_main
#include "decl_default_reads_test.c"
#undef main
#include "../src/backend/decl_renderprog_cvar_reads.c"
static uintptr_t rpc_at(fixture *f,size_t offset){return (uintptr_t)f->image+offset;}
static int rpc_start(fixture *f,sh_resource_default_read_receipt *r)
{CHECK(!sh_resource_default_read_begin(r,&f->input));return sh_decl_renderprog_cvar_read_check(r,&f->input);}
static void rpc_setup(fixture *f,int renderer)
{
    setup(f,renderer);const rpc_profile *p=rpc_profiles+renderer;
    ptr(f->image,p->cvars,rpc_at(f,0x30000));ptr(f->image,0x30000,rpc_at(f,p->cvar_table));
    ptr(f->image,p->commands,rpc_at(f,0x40000));ptr(f->image,0x40000,rpc_at(f,p->command_table));
    ptr(f->image,p->cvar_table+8,rpc_at(f,rpc_pins[renderer][RPC_COUNT].rva));
    ptr(f->image,p->cvar_table+0x10,rpc_at(f,rpc_pins[renderer][RPC_INDEX].rva));
    ptr(f->image,p->command_table+0x10,rpc_at(f,rpc_pins[renderer][RPC_MODE].rva));
    for(unsigned i=0;i<RPC_PINS;i++){const rpc_pin *pin=rpc_pins[renderer]+i;CHECK(sh_native_pin_fixture_copy(renderer,pin->rva,f->image+pin->rva,pin->length));}
}
static void rpc_rows(fixture *f,size_t header,size_t data,int count)
{ptr(f->image,0x30000+header,rpc_at(f,data));number(f->image,0x30008+header,count);number(f->image,0x3000c+header,count);}
static void rpc_entry(fixture *f,size_t list,int index,size_t object,unsigned flags)
{
    ptr(f->image,list+(size_t)index*8,rpc_at(f,object));number(f->image,object+0x58,(int)flags);
    ptr(f->image,object+0x40,rpc_at(f,object+0x80));strcpy((char *)f->image+object+0x80,"test_shader_value");
    ptr(f->image,object+0x10,rpc_at(f,object+0xc0));strcpy((char *)f->image+object+0xc0,"vec4(1, 2, 3, 4)");
    number(f->image,object+0x30,-17);number(f->image,object+0x34,0x3f400000);
}
static void rpc_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;rpc_setup(&f,renderer);
    CHECK(rpc_start(&f,&r) && r.state==1 && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    rpc_rows(&f,8,0x70000,8);rpc_rows(&f,0x20,0x71000,1);
    for(unsigned i=0;i<8;i++)rpc_entry(&f,0x70000,(int)i,0x80000+i*0x100,0x100000|i);
    rpc_entry(&f,0x71000,0,0x90000,0x100002);
    CHECK(rpc_start(&f,&r));
    f.denied=rpc_at(&f,0x30020);CHECK(rpc_start(&f,&r));f.denied=0;
    f.denied=rpc_at(&f,0x80058);CHECK(!rpc_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
    f.denied=rpc_at(&f,0x800c0);CHECK(!rpc_start(&f,&r));
    number(f.image,0x80058,0);CHECK(rpc_start(&f,&r));number(f.image,0x80058,0x100000);f.denied=0;
    f.denied=rpc_at(&f,0x80340);CHECK(rpc_start(&f,&r));f.denied=0; /* Ignored type3 never formats its name. */
    f.denied=rpc_at(&f,0x80230);CHECK(!rpc_start(&f,&r));f.denied=0;
    number(f.image,0x600a8,1);f.denied=rpc_at(&f,0x80058);CHECK(rpc_start(&f,&r));f.denied=0;
    number(f.image,0x600a8,-1);CHECK(rpc_start(&f,&r));number(f.image,0x600a8,0);
    ptr(f.image,0x80040,0);ptr(f.image,0x80010,0);CHECK(rpc_start(&f,&r));
    char formatted[40];const char *volatile null_string=NULL;
    CHECK(snprintf(formatted,sizeof formatted,"%s %s",null_string,null_string)>0 && !strcmp(formatted,"(null) (null)"));
    rpc_entry(&f,0x70000,0,0x80000,0x100000);
    number(f.image,0x30014,7);CHECK(!rpc_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);number(f.image,0x30014,8);
    number(f.image,0x30010,0);f.denied=rpc_at(&f,0x30008);CHECK(rpc_start(&f,&r));number(f.image,0x30010,-1);CHECK(rpc_start(&f,&r));f.denied=0;number(f.image,0x30010,8);
    f.change_at=rpc_at(&f,0x30008);CHECK(!rpc_start(&f,&r) && r.reason==SH_DEFAULT_READ_OWNERSHIP);f.change_at=0;rpc_rows(&f,8,0x70000,8);
    const rpc_profile *p=rpc_profiles+renderer;
    ptr(f.image,p->command_table+0x10,rpc_at(&f,rpc_pins[renderer][RPC_MODE].rva)+1);
    CHECK(!rpc_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);ptr(f.image,p->command_table+0x10,rpc_at(&f,rpc_pins[renderer][RPC_MODE].rva));
    for(unsigned i=0;i<RPC_PINS;i++){size_t offsets[]={0,rpc_pins[renderer][i].length/2,rpc_pins[renderer][i].length-1};
        for(unsigned j=0;j<3;j++){unsigned char *at=f.image+rpc_pins[renderer][i].rva+offsets[j];
            *at^=1;CHECK(!rpc_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*at^=1;}}
    CHECK(rpc_start(&f,&r));f.input.work_limit=r.work-1;CHECK(!rpc_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(rpc_start(&f,&r));f.input.resource++;CHECK(!sh_decl_renderprog_cvar_read_check(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.resource--;
    CHECK(rpc_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1)); /* Helper cannot complete its caller. */
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void rpc_image_case(const char *path,int renderer)
{
    FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    unsigned char *raw=malloc(size);CHECK(raw);if(!raw){fclose(f);return;}CHECK(fread(raw,1,size,f)==(size_t)size);fclose(f);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<RPC_PINS;i++){const rpc_pin *pin=rpc_pins[renderer]+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData && pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){
                size_t offset=s->PointerToRawData+pin->rva-s->VirtualAddress;CHECK(offset<=(size_t)size && pin->length<=(size_t)size-offset && sh_native_pin_fixture_digest(raw+offset,pin->length,pin->hash));found=1;break;}}
        CHECK(found);}
    const rpc_profile *b=rpc_profiles+renderer;
    const uint32_t slots[]={b->cvar_table+8,b->cvar_table+0x10,b->command_table+0x10};
    for(unsigned i=0;i<3;i++){int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){IMAGE_SECTION_HEADER *s=sections+j;
            if(slots[i]>=s->VirtualAddress && slots[i]-s->VirtualAddress<=s->SizeOfRawData && 8<=s->SizeOfRawData-(slots[i]-s->VirtualAddress)){
                size_t offset=s->PointerToRawData+slots[i]-s->VirtualAddress;uintptr_t callback;
                CHECK(offset<=(size_t)size && 8<=(size_t)size-offset);memcpy(&callback,raw+offset,8);
                CHECK(callback==nt->OptionalHeader.ImageBase+rpc_pins[renderer][i].rva);found=1;break;}}
        CHECK(found);}
    free(raw);
}
int main(int argc,char **argv)
{if(argc==3)rpc_image_case(argv[1],atoi(argv[2])!=0);else{rpc_cases(0);rpc_cases(1);}printf("RenderProg cvar reads: %d failure(s)\n",failures);return failures?1:0;}
