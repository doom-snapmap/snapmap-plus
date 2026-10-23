#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "resource_metadata_reads.h"
#include "host_image.h"
#include "native_pin_hash.h"
static int rmd_digest(const unsigned char *bytes,size_t length,const unsigned char expected[32])
{
    sh_native_pin_hash hash;unsigned char actual[32];
    sh_native_pin_hash_init(&hash);sh_native_pin_hash_update(&hash,bytes,length);
    sh_native_pin_hash_finish(&hash,actual);return !memcmp(actual,expected,32);
}

#include "resource_metadata_read_pins.h"
static const uint8_t *g_base;
static int g_renderer;
typedef struct rmd_binding {
    uint32_t entry,parent,state,fs_holder,fs_object,fs_table,timestamp,metadata;
    const unsigned char *entry_bytes,*parent_bytes,*timestamp_bytes,*metadata_bytes;
    size_t entry_length,parent_length,timestamp_length,metadata_length;
} rmd_binding;
static const rmd_binding rmd_bindings[2]={
 {0x17a4f70,0x17a5190,0x56e4a08,0x3eb98a8,0x5895b90,0x27ae6c0,0x1a48050,0x1a48080,
  rmd_depend_gl,rmd_parent_gl,rmd_fs_timestamp_gl,rmd_fs_metadata_gl,
  540,38,46,769},
 {0x17b24b0,0x17b26d0,0x6dde198,0x55b7288,0x6f8f320,0x27984a0,0x1a55b70,0x1a55ba0,
  rmd_depend_vk,rmd_parent_vk,rmd_fs_timestamp_vk,rmd_fs_metadata_vk,
  540,38,46,769}
};
static int rmd_pins(const uint8_t *base,int renderer)
{
    if(!base || renderer<0 || renderer>1 || !sh_host_is_pinned_renderer_build(renderer)) return 0;
    const rmd_binding *b=rmd_bindings+renderer;
    __try {
        return rmd_digest(base+b->entry,b->entry_length,b->entry_bytes) &&
            rmd_digest(base+b->parent,b->parent_length,b->parent_bytes) &&
            rmd_digest(base+b->timestamp,b->timestamp_length,b->timestamp_bytes) &&
            rmd_digest(base+b->metadata,b->metadata_length,b->metadata_bytes);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int sh_resource_metadata_reads_bind(const uint8_t *base)
{
    int renderer=sh_host_is_vulkan();
    if(!rmd_pins(base,renderer))return 0;
    g_base=base;g_renderer=renderer;return 1;
}
int sh_resource_metadata_reads_ready(void)
{
    if(sh_host_is_vulkan()!=g_renderer || !rmd_pins(g_base,g_renderer))return 0;
    const rmd_binding *b=rmd_bindings+g_renderer;
    __try {
        void *fs=*(void **)(g_base+b->fs_holder);
        if(fs!=g_base+b->fs_object || *(void **)fs!=g_base+b->fs_table ||
           (*(void ***)fs)[0xa8/8]!=g_base+b->timestamp) return 0;
        static const uint32_t initialized[2]={0x1a48aa0,0x1a565c0};
        static const unsigned char body[8]={0x83,0x79,0x10,0,0x0f,0x9f,0xc0,0xc3};
        return (*(void ***)fs)[0x20/8]==g_base+initialized[g_renderer] &&
            !memcmp(g_base+initialized[g_renderer],body,sizeof body);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* The exact void source dependency helper updates dependency names/indices and
 * a timestamp cache. Its file abstraction returns metadata, never declaration
 * payload. No graph or instance scope is suppressed: any nested real reads
 * remain observable. The provider records file dependencies at actual Read,
 * including empty/EOF results, instead of charging metadata-only Open calls. */
