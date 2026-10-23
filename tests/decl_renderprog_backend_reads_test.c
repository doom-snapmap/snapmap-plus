#include "native_pin_fixture.h"
#define main sh_rpb_common_test_main
#include "decl_default_reads_test.c"
#undef main
static uintptr_t rpb_test_export(int renderer,const char *name,int extension);
#define SH_RENDERPROG_BACKEND_EXPORT(renderer,name,extension) rpb_test_export(renderer,name,extension)
#include "../src/backend/decl_renderprog_backend_reads.c"
static int rpb_missing,rpb_lookup_calls;
static uintptr_t rpb_test_export(int renderer,const char *name,int extension)
{
    rpb_lookup_calls++;
    if(rpb_missing)return 0;
    if(!renderer && !strcmp(name,"wglGetProcAddress")){CHECK(!extension);return UINT64_C(0x7ffb01000000);}
    for(unsigned i=0;i<rpb_api_counts[renderer];i++){
        const rpb_api *api=rpb_apis[renderer]+i;
        if(!strcmp(name,api->name)){CHECK(extension==api->extension);return UINT64_C(0x7ffb02000000)+renderer*0x100000+i*0x100;}
    }
    CHECK(0);return 0;
}
static int rpb_start(fixture *f,sh_resource_default_read_receipt *r)
{CHECK(!sh_resource_default_read_begin(r,&f->input));return sh_decl_renderprog_backend_read_check(r,&f->input);}
static void rpb_setup(fixture *f,int renderer)
{
    setup(f,renderer);rpb_missing=rpb_lookup_calls=0;
    for(unsigned i=0;i<rpb_pin_counts[renderer];i++){
        const rpb_pin *pin=rpb_pins[renderer]+i;CHECK(sh_native_pin_fixture_copy(renderer,pin->rva,f->image+pin->rva,pin->length));
    }
    for(unsigned i=0;i<rpb_api_counts[renderer];i++){
        const rpb_api *api=rpb_apis[renderer]+i;
        ptr(f->image,api->slot,rpb_test_export(renderer,api->name,api->extension));
    }
    if(!renderer)ptr(f->image,0x1fcdcd8,rpb_test_export(0,"wglGetProcAddress",0));
    rpb_lookup_calls=0;
}
static void rpb_cases(int renderer)
{
    fixture f;sh_resource_default_read_receipt r;rpb_setup(&f,renderer);
    CHECK(rpb_start(&f,&r) && r.state==1 && !r.complete && r.reason==SH_DEFAULT_READ_UNSUPPORTED);
    CHECK(rpb_lookup_calls==(int)rpb_api_counts[renderer]+!renderer);
    for(unsigned i=0;i<rpb_api_counts[renderer];i++){
        const rpb_api *api=rpb_apis[renderer]+i;
        uintptr_t valid=rpb_test_export(renderer,api->name,api->extension);
        ptr(f.image,api->slot,valid+1);CHECK(!rpb_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
        ptr(f.image,api->slot,0);CHECK(rpb_start(&f,&r)); /* An absent optional slot is not rejected speculatively. */
        ptr(f.image,api->slot,valid);f.denied=(uintptr_t)f.image+api->slot;
        CHECK(!rpb_start(&f,&r) && r.reason==SH_DEFAULT_READ_MEMORY);f.denied=0;
        f.change_at=(uintptr_t)f.image+api->slot;
        CHECK(!rpb_start(&f,&r) && r.reason==SH_DEFAULT_READ_INVOCATION);f.change_at=0;ptr(f.image,api->slot,valid);
    }
    rpb_missing=1;CHECK(!rpb_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);rpb_missing=0;
    for(unsigned i=0;i<rpb_pin_counts[renderer];i++){
        const rpb_pin *pin=rpb_pins[renderer]+i;size_t offsets[]={0,pin->length/2,pin->length-1};
        for(unsigned j=0;j<3;j++){
            unsigned char *at=f.image+pin->rva+offsets[j];*at^=1;
            CHECK(!rpb_start(&f,&r) && r.reason==SH_DEFAULT_READ_PIN);*at^=1;
        }
    }
    if(!renderer){
        uintptr_t valid=rpb_test_export(0,"wglGetProcAddress",0);
        ptr(f.image,0x1fcdcd8,valid+1);CHECK(!rpb_start(&f,&r) && r.reason==SH_DEFAULT_READ_CALLBACK);
        ptr(f.image,0x1fcdcd8,valid);f.change_at=(uintptr_t)f.image+0x1fcdcd8;
        CHECK(!rpb_start(&f,&r) && r.reason==SH_DEFAULT_READ_INVOCATION);f.change_at=0;ptr(f.image,0x1fcdcd8,valid);
        /* Execute the retained native backend early-return getter with its real
         * RIP-relative global, then confirm the adjunct follows the same mode. */
        DWORD old;CHECK(VirtualProtect(f.image+0x19eeb50,16,PAGE_EXECUTE_READWRITE,&old));
        unsigned char (__fastcall *get_mode)(void)=(void *)(f.image+0x19eeb50);
        f.image[0x56e4a0c]=0;CHECK(get_mode()==0);
        f.image[0x56e4a0c]=7;CHECK(get_mode()==7);
        rpb_missing=1;rpb_lookup_calls=0;CHECK(rpb_start(&f,&r) && rpb_lookup_calls==0);
        f.image[0x56e4a0c]=0;CHECK(!rpb_start(&f,&r));rpb_missing=0;
    }
    CHECK(rpb_start(&f,&r));f.input.work_limit=r.work-1;
    CHECK(!rpb_start(&f,&r) && r.reason==SH_DEFAULT_READ_BUDGET);f.input.work_limit=0;
    CHECK(rpb_start(&f,&r));f.input.resource++;
    CHECK(!sh_decl_renderprog_backend_read_check(&r,&f.input) && r.reason==SH_DEFAULT_READ_INPUT);f.input.resource--;
    CHECK(rpb_start(&f,&r));CHECK(!sh_resource_default_read_finish(&r,&f.input,1,1));
    CHECK(!sh_decl_renderprog_backend_read_check(&r,&f.input));
    VirtualFree(f.image,0,MEM_RELEASE);
}
static void rpb_image_case(const char *path,int renderer)
{
    FILE *file=fopen(path,"rb");CHECK(file);if(!file)return;
    fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);unsigned char *raw=malloc(size);CHECK(raw);
    if(!raw){fclose(file);return;}CHECK(fread(raw,1,size,file)==(size_t)size);fclose(file);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<rpb_pin_counts[renderer];i++){
        const rpb_pin *pin=rpb_pins[renderer]+i;int found=0;
        for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++){
            IMAGE_SECTION_HEADER *s=sections+j;
            if(pin->rva>=s->VirtualAddress && pin->rva-s->VirtualAddress<=s->SizeOfRawData &&
               pin->length<=s->SizeOfRawData-(pin->rva-s->VirtualAddress)){
                size_t off=s->PointerToRawData+pin->rva-s->VirtualAddress;
                CHECK(off<=(size_t)size && pin->length<=(size_t)size-off && sh_native_pin_fixture_digest(raw+off,pin->length,pin->hash));found=1;break;
            }
        }
        CHECK(found);
    }
    free(raw);
}
int main(int argc,char **argv)
{if(argc==3)rpb_image_case(argv[1],atoi(argv[2])!=0);else{rpb_cases(0);rpb_cases(1);}printf("RenderProg backend dispatch: %d failure(s)\n",failures);return failures?1:0;}


