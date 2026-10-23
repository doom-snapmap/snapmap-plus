#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/rig_default.c"
static int failures, pinned=-1, resized, parsed, constructed, destroyed, forwarded, mode;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); failures++; } } while(0)
int sh_host_is_pinned_renderer_build(int r) { return r==pinned; }
static unsigned char bytes[128], group[0x50];
static void *groups[1];
static unsigned char resize_fixture(void *list, int count)
{
    resized++; CHECK(count==85);
    if (mode==1) return 0;
    *(void **)list=bytes; *(int *)((uint8_t *)list+12)=128;
    return 1;
}
static void ctor_fixture(void *s) { constructed++; memset(s,0,48); *(int *)s=0x1234; }
static void dtor_fixture(void *s) { destroyed++; CHECK(*(int *)s==0x1234); }
static uintptr_t parse_fixture(void *resource, void *error, unsigned char disabled)
{
    uint8_t *o=resource;
    static const char expected[]="{\n \"groups\": [\n  {\n   \"name\": \"Default\",\n   \"layer\": 0,\n   \"components\": []\n  }\n ]\n}\n";
    parsed++; CHECK(!disabled && *(int *)error==0x1234);
    CHECK(*(int *)(o+0x58)==85 && !memcmp(*(void **)(o+0x50),expected,85));
    if (mode==2) return 14;
    if (mode==3) RaiseException(0xe1235678,0,0,NULL);
    if (mode==4) return 0; /* Native success without materializing a group is not accepted. */
    memset(group,0,sizeof group);
    *(int *)(group+8)=7; *(const char **)(group+0x10)="Default";
    groups[0]=group; *(void **)(o+0x38)=groups;
    *(int *)(o+0x40)=1; *(int *)(o+0x44)=1;
    return 0;
}
static void other_default(void *p) { CHECK(p!=NULL); forwarded++; }
static void fixtures(void)
{
    rig_resize=resize_fixture; rig_parse=parse_fixture; rig_ctor=ctor_fixture; rig_dtor=dtor_fixture;
}
static uint8_t *synthetic(int r)
{
    uint8_t *b=VirtualAlloc(NULL,0x6000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    const rig_profile *p=rig_profiles+r;
    const rig_json_binding *j=rig_json_profiles+r;
    if (!b) return NULL;
    memcpy(b+p->default_fn,rig_default_head,sizeof rig_default_head);
    memcpy(b+p->read_fn,rig_read_head,sizeof rig_read_head);
    memcpy(b+p->parse_fn,rig_parse_head,sizeof rig_parse_head);
    memcpy(b+p->resize_fn,rig_resize_head,sizeof rig_resize_head);
    memcpy(b+p->string_ctor,p->ctor_head,sizeof p->ctor_head);
    memcpy(b+p->string_dtor,p->dtor_head,sizeof p->dtor_head);
    memcpy(b+p->text,rig_text,sizeof rig_text-1);
    *(void **)(b+p->table+0x40)=b+p->read_fn;
    *(void **)(b+p->table+0x50)=b+p->default_fn;
    *(void **)(b+j->holder)=b+j->object;
    *(void **)(b+j->object)=b+j->table;
    memcpy(b+j->getter,j->getter_head,j->getter_length);
    for (unsigned i=0;i<4;i++) {
        memcpy(b+j->methods[i].function,j->methods[i].head,16);
        *(void **)(b+j->table+j->methods[i].slot)=b+j->methods[i].function;
    }
    return b;
}
static void cases(int r)
{
    uint8_t *b=synthetic(r), object[0x80]={0};
    const rig_profile *p=rig_profiles+r;
    CHECK(b); if (!b) return;
    pinned=!r; CHECK(!sh_rig_default_install(b,0x2600000,r));
    pinned=r;
    CHECK(!sh_rig_default_install(b,p->text+84,r));
    b[p->text]^=1; CHECK(!sh_rig_default_install(b,0x2600000,r)); b[p->text]^=1;
    b[p->parse_fn]^=1; CHECK(!sh_rig_default_install(b,0x2600000,r)); b[p->parse_fn]^=1;
    *(void **)(b+p->table+0x40)=b+p->default_fn;
    CHECK(!sh_rig_default_install(b,0x2600000,r));
    *(void **)(b+p->table+0x40)=b+p->read_fn;
    CHECK(sh_rig_default_install(b,0x6000000,r) && sh_rig_default_ready("md6Rig")); fixtures();
    CHECK(sh_rig_default_invoke(object,other_default));
    CHECK(forwarded==r+1);
    for (mode=0;mode<=4;mode++) {
        int caught=0;
        memset(object,0,sizeof object); *(void **)object=b+p->table;
        resized=parsed=constructed=destroyed=0;
        __try { CHECK(sh_rig_default_invoke(object,b+p->default_fn)==(mode==0)); }
        __except(GetExceptionCode()==0xe1235678 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught=1; }
        CHECK(resized==1 && parsed==(mode!=1) && constructed==(mode!=1) && destroyed==(mode!=1));
        CHECK(caught==(mode==3));
        if (!mode) CHECK(sh_rig_default_observed(object,b+p->default_fn));
    }
    mode=0; memset(object,0,sizeof object); *(void **)object=b+p->table;
    {
        const rig_json_binding *j=rig_json_profiles+r;
        resized=parsed=0;
        *(void **)(b+j->holder)=b+j->object+8;
        CHECK(!sh_rig_default_invoke(object,b+p->default_fn) && !resized && !parsed);
        *(void **)(b+j->holder)=b+j->object;
        *(void **)(b+j->object)=b+j->table+8;
        CHECK(!sh_rig_default_invoke(object,b+p->default_fn) && !resized && !parsed);
        *(void **)(b+j->object)=b+j->table;
        /* Every getter instruction is part of the fixed-holder proof, including
         * the final return beyond the short entry pin. */
        for(unsigned i=0;i<j->getter_length;i++) {
            b[j->getter+i]^=1;
            CHECK(!sh_rig_default_invoke(object,b+p->default_fn) && !resized && !parsed);
            b[j->getter+i]^=1;
        }
        for(unsigned i=0;i<4;i++) {
            *(void **)(b+j->table+j->methods[i].slot)=b+j->methods[i].function+1;
            CHECK(!sh_rig_default_invoke(object,b+p->default_fn) && !resized && !parsed);
            *(void **)(b+j->table+j->methods[i].slot)=b+j->methods[i].function;
            b[j->methods[i].function]^=1;
            CHECK(!sh_rig_default_invoke(object,b+p->default_fn) && !resized && !parsed);
            b[j->methods[i].function]^=1;
        }
    }
    *(int *)(object+0x40)=1; resized=0;
    CHECK(!sh_rig_default_invoke(object,b+p->default_fn) && !resized);
    *(int *)(object+0x40)=0; *(int *)(object+0x58)=2; *(int *)(object+0x5c)=1;
    CHECK(!sh_rig_default_invoke(object,b+p->default_fn) && !resized);
    memset(object,0,sizeof object);
    CHECK(!sh_rig_default_invoke(object,b+p->default_fn) && !resized);
    CHECK(!sh_rig_default_observed(object,b+p->default_fn));
    VirtualFree(b,0,MEM_RELEASE);
}
/* Map sections without executing them. Validate the two raw vtable addresses
 * against the PE image base before relocating those slots for the install check. */
static void image_case(const char *path,int r)
{
    FILE *f=fopen(path,"rb");
    long length;
    uint8_t *file,*base;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *s;
    const rig_profile *p=rig_profiles+r;
    CHECK(f); if (!f) return;
    fseek(f,0,SEEK_END); length=ftell(f); fseek(f,0,SEEK_SET);
    file=malloc((size_t)length); CHECK(file);
    if (!file) { fclose(f); return; }
    CHECK(fread(file,1,(size_t)length,f)==(size_t)length); fclose(f);
    nt=(const IMAGE_NT_HEADERS64 *)(file+((IMAGE_DOS_HEADER *)file)->e_lfanew);
    s=IMAGE_FIRST_SECTION(nt);
    base=VirtualAlloc(NULL,nt->OptionalHeader.SizeOfImage,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(base); if (!base) { free(file); return; }
    memcpy(base,file,nt->OptionalHeader.SizeOfHeaders);
    for (unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        size_t size=s[i].SizeOfRawData;
        CHECK(s[i].PointerToRawData+size<=(size_t)length &&
              s[i].VirtualAddress+size<=nt->OptionalHeader.SizeOfImage);
        memcpy(base+s[i].VirtualAddress,file+s[i].PointerToRawData,size);
    }
    CHECK(*(uint64_t *)(base+p->table+0x40)==nt->OptionalHeader.ImageBase+p->read_fn);
    CHECK(*(uint64_t *)(base+p->table+0x50)==nt->OptionalHeader.ImageBase+p->default_fn);
    *(void **)(base+p->table+0x40)=base+p->read_fn;
    *(void **)(base+p->table+0x50)=base+p->default_fn;
    pinned=r; CHECK(sh_rig_default_install(base,nt->OptionalHeader.SizeOfImage,r));
    {
        const rig_json_binding *j=rig_json_profiles+r;
        CHECK(*(uint64_t *)(base+j->holder)==nt->OptionalHeader.ImageBase+j->object);
        CHECK(*(uint64_t *)(base+j->object)==nt->OptionalHeader.ImageBase+j->table);
        *(void **)(base+j->holder)=base+j->object;
        *(void **)(base+j->object)=base+j->table;
        for(unsigned i=0;i<4;i++) {
            CHECK(*(uint64_t *)(base+j->table+j->methods[i].slot)==nt->OptionalHeader.ImageBase+j->methods[i].function);
            *(void **)(base+j->table+j->methods[i].slot)=base+j->methods[i].function;
        }
        CHECK(rig_json_binding_ready());
    }
    printf("rig default %s pins verified\n",r?"Vulkan":"OpenGL");
    VirtualFree(base,0,MEM_RELEASE); free(file);
}
int main(int argc,char **argv)
{
    if (argc>2) image_case(argv[1],atoi(argv[2])!=0);
    else { cases(0); cases(1); }
    printf("rig defaults: %d failure(s)\n",failures);
    return failures?1:0;
}
