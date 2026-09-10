/* Execute the contents gate in a synthetic native frame, never game bytes. */
#include "../src/backend/nav_play.c"

static int enabled=1;
void backend_log(const char *message) {(void)message;}
int sh_config_get_bool(const char *key,int *value,unsigned *flags)
{(void)key;(void)flags;*value=enabled;return 1;}
void sh_nav_bake_build_begin(void) {}
void sh_nav_bake_build_end(void) {}
void sh_nav_bake_enable_instances(int on) {(void)on;}
int sh_nav_bake_instance_name(int i,const char *n,char *out,size_t cap)
{(void)i;(void)n;(void)out;(void)cap;return 0;}

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
    printf("nav_play_test: %d failures\n",failed);return failed?1:0;
}
