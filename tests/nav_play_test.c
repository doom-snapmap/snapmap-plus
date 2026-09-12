/* Execute the contents gate in a synthetic native frame, never game bytes. */
#include "../src/backend/nav_play.c"

static int enabled=1;
void backend_log(const char *message) {(void)message;}
void sh_map_render_build(void *map) {(void)map;}
int sh_config_get_bool(const char *key,int *value,unsigned *flags)
{(void)key;(void)flags;*value=enabled;return 1;}
void sh_nav_bake_build_begin(void) {}
void sh_nav_bake_build_end(void) {}
void sh_nav_bake_enable_instances(int on) {(void)on;}
int sh_nav_bake_instance_name(int i,const char *n,char *out,size_t cap)
{(void)i;(void)n;(void)out;(void)cap;return 0;}

static int publication_failures, publication_writes;
static void check_original_published(void *target)
{
    (void)target;
    publication_writes++;
    if (!g_orig || g_orig(NULL,NULL,NULL) != 42) publication_failures++;
}

static int test_hook_publication(void)
{
    unsigned char *code=VirtualAlloc(NULL,64,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    int before=hook_owned_count();
    if(!code)return 1;
    memset(code,0x90,15);code[15]=0xb8;code[16]=42;code[17]=code[18]=code[19]=0;code[20]=0xc3;
    FlushInstructionCache(GetCurrentProcess(),code,64);
    sh_patch_test_observe_write(check_original_published);
    /* Fail commit protection, internal rollback, and the caller's immediate cleanup. */
    sh_patch_test_faults(2|4|8,0,0);
    if(sh_nav_play_install(code,1)||!g_orig||hook_is_installed((void *)g_orig)||
       hook_owned_count()!=before+1)publication_failures++;
    if(((snapbuild_fn_t)code)(NULL,NULL,NULL)!=42)publication_failures++;
    sh_patch_test_faults(1,0,0);
    if(sh_nav_play_install(code,1)||!g_orig||hook_owned_count()!=before+1)publication_failures++;
    sh_patch_test_faults(0,0,0);
    if(!sh_nav_play_install(code,1)||!hook_is_installed((void *)g_orig)||
       hook_owned_count()!=before+1||!publication_writes)publication_failures++;
    if(!sh_nav_play_install(NULL,0))publication_failures++;
    if(!hook_unpatch((void *)g_orig))publication_failures++;
    else g_orig=NULL;
    sh_patch_test_observe_write(NULL);
    if(hook_owned_count()!=before)publication_failures++;
    VirtualFree(code,0,MEM_RELEASE);
    return publication_failures;
}

int main(void)
{
    unsigned char body[]={
        0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,
        0x80,0xbb,0x8e,0x0c,0,0,0,0x74,0x0a,
        0x81,0xa3,0x94,0x0c,0,0,0xff,0xff,0xfd,0xff,
        0x8b,0x83,0x94,0x0c,0,0,0x48,0x83,0xc4,0x20,0x5b,0xc3};
    unsigned char entity[0xd00]={0};
    unsigned char *code=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    sig_result site={"BlockingVolumeObstacleGate",SIG_OK,0,0};
    unsigned (*update)(const unsigned char *);DWORD old;int a,b,c,d,failed=0;
    if(!code)return 1;
    memcpy(code,body,sizeof body);
    VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old);
    FlushInstructionCache(GetCurrentProcess(),code,sizeof body);
    site.addr=(uintptr_t)(code+8);update=(unsigned (*)(const unsigned char *))code;
#ifdef SH_PATCH_TESTING
    sh_patch_test_faults(2|8,0,0);
    if(sh_nav_play_install_volume_contents(&site,1)||!g_volume_contents_patch.live||
       !g_volume_contents_relay||g_volume_contents_ready)failed++;
    sh_patch_test_faults(0,0,0);
#endif
    if(!sh_nav_play_install_volume_contents(&site,1))return 1;
    for(a=0;a<2;a++)for(b=0;b<2;b++)for(c=0;c<2;c++)for(d=0;d<2;d++) {
        unsigned contents=0x482089a,expected;
        enabled=a;entity[0xc8e]=(unsigned char)b;
        entity[0xc89]=(unsigned char)c;entity[0x3ea]=(unsigned char)(d?0x40:0);
        contents|=0x20010;memcpy(entity+0xc94,&contents,4);
        expected=(b||(a&&c&&d))?contents&~0x20000u:contents;
        if(update(entity)!=expected||entity[0xc8e]!=b||entity[0x3ea]!=(d?0x40:0))failed++;
    }
    if(code_unpatch(&g_volume_contents_patch)!=B2_PATCH_OK)failed++;
    if(memcmp(code,body,sizeof body))failed++;
    VirtualFree(g_volume_contents_relay,0,MEM_RELEASE);
    VirtualFree(code,0,MEM_RELEASE);
    failed+=test_hook_publication();
    printf("nav_play_test: %d failures\n",failed);return failed?1:0;
}
