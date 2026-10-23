#include "native_pin_fixture.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/resource_metadata_reads.c"
static int failures,renderer,pinned=1;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x);failures++; } } while(0)
int sh_host_is_vulkan(void) { return renderer; }
int sh_host_is_pinned_renderer_build(int r) { return pinned && r==renderer; }
static void put_pointer(void *at,size_t off,void *value) { memcpy((unsigned char *)at+off,&value,sizeof value); }
static void image_case(const char *path,int which)
{
    FILE *f=fopen(path,"rb");CHECK(f);if(!f)return;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    unsigned char *raw=malloc(size);CHECK(raw);if(!raw){fclose(f);return;}CHECK(fread(raw,1,size,f)==(size_t)size);fclose(f);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64 *)(raw+((IMAGE_DOS_HEADER *)raw)->e_lfanew);
    IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);const rmd_binding *b=rmd_bindings+which;
    uint32_t rvas[]={b->entry,b->parent,b->timestamp,b->metadata};
    const unsigned char *bytes[]={b->entry_bytes,b->parent_bytes,b->timestamp_bytes,b->metadata_bytes};
    size_t lengths[]={b->entry_length,b->parent_length,b->timestamp_length,b->metadata_length};
    for(unsigned i=0;i<4;i++) { int found=0;for(unsigned j=0;j<nt->FileHeader.NumberOfSections;j++)
      if(rvas[i]>=s[j].VirtualAddress && rvas[i]-s[j].VirtualAddress<=s[j].SizeOfRawData &&
         lengths[i]<=s[j].SizeOfRawData-(rvas[i]-s[j].VirtualAddress)) {
        size_t off=s[j].PointerToRawData+rvas[i]-s[j].VirtualAddress;
        CHECK(off<=(size_t)size && lengths[i]<=(size_t)size-off);CHECK(sh_native_pin_fixture_digest(raw+off,lengths[i],bytes[i]));found=1;break;
      }CHECK(found); }
    free(raw);
}
int main(int argc,char **argv)
{
    if(argc==3) { image_case(argv[1],atoi(argv[2]));if(!failures)puts("resource_metadata_reads_image_test: PASS");return failures?1:0; }
    unsigned char *base=VirtualAlloc(NULL,0x7000000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(base);if(!base)return 2;
    for(renderer=0;renderer<2;renderer++) {
        const rmd_binding *b=rmd_bindings+renderer;
        CHECK(sh_native_pin_fixture_copy(renderer,b->entry,base+b->entry,b->entry_length));CHECK(sh_native_pin_fixture_copy(renderer,b->parent,base+b->parent,b->parent_length));
        CHECK(sh_native_pin_fixture_copy(renderer,b->timestamp,base+b->timestamp,b->timestamp_length));CHECK(sh_native_pin_fixture_copy(renderer,b->metadata,base+b->metadata,b->metadata_length));
        const uint32_t init[]={0x1a48aa0,0x1a565c0};const unsigned char ib[]={0x83,0x79,0x10,0,0x0f,0x9f,0xc0,0xc3};
        put_pointer(base,b->fs_holder,base+b->fs_object);put_pointer(base,b->fs_object,base+b->fs_table);
        put_pointer(base,b->fs_table+0xa8,base+b->timestamp);put_pointer(base,b->fs_table+0x20,base+init[renderer]);memcpy(base+init[renderer],ib,8);
        pinned=0;CHECK(!sh_resource_metadata_reads_bind(base));pinned=1;
        CHECK(sh_resource_metadata_reads_bind(base) && sh_resource_metadata_reads_ready());
        uint32_t rvas[]={b->entry,b->parent,b->timestamp,b->metadata};
        size_t lengths[]={b->entry_length,b->parent_length,b->timestamp_length,b->metadata_length};
        for(unsigned n=0;n<4;n++)for(size_t i=0;i<lengths[n];i++) {
            base[rvas[n]+i]^=1;CHECK(!sh_resource_metadata_reads_ready());base[rvas[n]+i]^=1;
        }
        put_pointer(base,b->fs_holder,NULL);CHECK(!sh_resource_metadata_reads_ready());put_pointer(base,b->fs_holder,base+b->fs_object);
        put_pointer(base,b->fs_table+0xa8,base+b->timestamp+1);CHECK(!sh_resource_metadata_reads_ready());put_pointer(base,b->fs_table+0xa8,base+b->timestamp);
        put_pointer(base,b->fs_table+0x20,NULL);CHECK(!sh_resource_metadata_reads_ready());put_pointer(base,b->fs_table+0x20,base+init[renderer]);
        CHECK(sh_resource_metadata_reads_ready());
        CHECK(sh_native_pin_fixture_digest(base+b->entry,b->entry_length,b->entry_bytes)); /* No hook installed. */
    }
    g_base=NULL;VirtualFree(base,0,MEM_RELEASE);CHECK(!sh_resource_metadata_reads_ready());
    if(!failures)puts("resource_metadata_reads_test: PASS");return failures?1:0;
}
