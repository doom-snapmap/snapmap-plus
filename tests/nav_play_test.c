/* Execute the contents gate in a synthetic native frame, never game bytes. */
#include "../src/backend/nav_play.c"
#include "image_sites.h"

static int enabled=1;
void backend_log(const char *message) {(void)message;}
void sh_map_render_build(void *map) {(void)map;}
int sh_config_get_bool(const char *key,int *value,unsigned *flags)
{(void)key;(void)flags;*value=enabled;return 1;}
/* The detour must hand the navigation snapshot the object being converted,
 * for exactly the length of the pre-build snapshot. */
static const void *g_bracket;
static int g_bracket_open, g_snapshot_saw_build, g_bracket_closed_before_build;
void sh_apply_engine_nav_build_begin(const void *map) {g_bracket=map;g_bracket_open=1;}
void sh_apply_engine_nav_build_end(void) {g_bracket_open=0;}
void sh_nav_bake_build_begin(void) {if(g_bracket_open)g_snapshot_saw_build=(int)(intptr_t)g_bracket;}
static int converting_original(void *a,void *b,void *c)
{(void)a;(void)c;if(!g_bracket_open&&b)g_bracket_closed_before_build=1;return 7;}
static int test_snapshot_reads_the_converted_map(void)
{
    int failed=0;snapbuild_fn_t saved=g_orig;
    g_orig=converting_original;
    if(nav_snapbuild_detour(NULL,(void *)(intptr_t)0x5150,NULL)!=7)failed++;
    if(g_snapshot_saw_build!=0x5150)failed++;
    if(g_bracket_open||!g_bracket_closed_before_build)failed++;
    g_orig=saved;return failed;
}
void sh_nav_bake_build_end(void) {}
void sh_nav_bake_enable_instances(int on) {(void)on;}
int sh_nav_bake_overlay_name(int i,const char *n,char *out,size_t cap) {(void)i;(void)n;(void)out;(void)cap;return 0;}
int sh_grid_native_record_instance(const void *r) {(void)r;return -1;}
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
    if(sh_nav_play_install(code)||!g_orig||hook_is_installed((void *)g_orig)||
       hook_owned_count()!=before+1)publication_failures++;
    if(((snapbuild_fn_t)code)(NULL,NULL,NULL)!=42)publication_failures++;
    sh_patch_test_faults(1,0,0);
    if(sh_nav_play_install(code)||!g_orig||hook_owned_count()!=before+1)publication_failures++;
    sh_patch_test_faults(0,0,0);
    if(!sh_nav_play_install(code)||!hook_is_installed((void *)g_orig)||
       hook_owned_count()!=before+1||!publication_writes)publication_failures++;
    if(!sh_nav_play_install(NULL))publication_failures++;
    if(!hook_unpatch((void *)g_orig))publication_failures++;
    else g_orig=NULL;
    sh_patch_test_observe_write(NULL);
    if(hook_owned_count()!=before)publication_failures++;
    VirtualFree(code,0,MEM_RELEASE);
    return publication_failures;
}

static unsigned actor_copies;
static const char *actor_expected;
static const char *project_actor(const char *inherit, const char *cls, const char *source, char **owned)
{
    *owned = NULL;
    if (strcmp(inherit, "mods/nexttest/direct") || strcmp(cls, "idAI2")) return source;
    *owned = _strdup("inert copy");
    return *owned;
}
static void capture_actor_copy(void *dest, const char *text, int len)
{
    if (dest == &actor_copies && !strcmp(text,actor_expected) && len == (!strcmp(text,"original") ? 8 : 0))
        actor_copies++;
}
static int test_play_actor_copy(void)
{
    unsigned char entry[0x150 + sizeof(void *)] = {0}, record[0x60 + sizeof(void *)] = {0};
    static const char original[] = "original";
    *(void **)(entry+0x150) = record;
    *(const char **)(record+0x58) = "mods/nexttest/direct";
    *(const char **)(record+0x60) = "idAI2";
    g_actor_text = project_actor;
    g_set_play_text = capture_actor_copy;
    actor_expected = "inert copy";
    nav_set_actor_source(&actor_copies,original,8,entry);
    actor_expected = "original";
    *(const char **)(record+0x58) = "ai/stock";
    nav_set_actor_source(&actor_copies,original,8,entry);
    *(void **)(entry+0x150) = NULL;
    nav_set_actor_source(&actor_copies,original,8,entry);
    return actor_copies != 3 || strcmp(original,"original");
}

extern const char *sh_nav_test_eligible, *sh_nav_test_instance, *sh_nav_test_parent;

static int test_direct_actor_projection(void)
{
    unsigned char entry[0x158] = {0}, record[0x68] = {0}, dest[16] = {0};
    const char *instance = "map01/placed_314", *parent = "creatures/future";
    int failed = 0;
    *(void **)(entry + 0x150) = record;
    *(const char **)(dest + 8) = instance;
    *(const char **)(record + 0x58) = parent;
    *(const char **)(record + 0x60) = "idAI2";
    sh_nav_test_eligible = parent;
    nav_set_actor_source(dest, "original", 8, entry);
    if (sh_nav_test_instance) failed++;
    g_build_map = entry;
    nav_set_actor_source(dest, "original", 8, entry);
    if (sh_nav_test_instance != instance || sh_nav_test_parent != parent) failed++;
    sh_nav_test_instance = NULL;
    *(const char **)(record + 0x60) = "idEncounterManager";
    nav_set_actor_source(dest, "original", 8, entry);
    *(const char **)(record + 0x60) = "idAI2";
    sh_nav_test_eligible = NULL;
    nav_set_actor_source(dest, "original", 8, entry);
    sh_nav_test_eligible = "mods/nexttest/direct";
    *(const char **)(record + 0x58) = sh_nav_test_eligible;
    nav_set_actor_source(dest, "original", 8, entry);
    if (sh_nav_test_instance) failed++;
    g_build_map = NULL;
    sh_nav_test_eligible = NULL;
    return failed;
}

static int synthetic_main(void)
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
    failed+=test_play_actor_copy();
    failed+=test_direct_actor_projection();
    failed+=test_hook_publication();
    failed+=test_snapshot_reads_the_converted_map();
    printf("nav_play_test: %d failures\n",failed);return failed?1:0;
}

/* The bind checks of nav_play.c (nav_play_gate_hold and nav_play_call_target, which the installs use) on each build
 * 13954591 executable named on the command line. */
static void check_image(const char *path)
{
    const sh_build_identity *identity;
    uint8_t *base = image_open(path, &identity);
    sig_result *results;
    size_t n, cap = sig_db_count();
    uintptr_t lo, hi;
    if (!base) return;
    lo = (uintptr_t)base;
    hi = lo + ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    results = (sig_result *)calloc(cap, sizeof *results);
    if (!results) { g_image_fail = 1; free(base); return; }
    n = sig_resolve_all(base, results, cap);
    const uint8_t *gate = image_site(base, results, n, "BlockingVolumeObstacleGate", identity->vulkan);
    static const char *calls[] = { "BuildAASFindCall", "BuildAASLoadCall", "PlacementModelNameCall" };
    unsigned k;
    if (gate) {
        int ok = (!memcmp(gate,k_gate_expected,sizeof k_gate_expected));
        printf("  BlockingVolumeObstacleGate bytes: %s\n", ok ? "hold" : "FAIL");
        IMG_CHECK(ok);
    }
    for (k = 0; k < 3; k++) {
        const uint8_t *site = image_site(base, results, n, calls[k], identity->vulkan);
        uintptr_t target = 0;
        if (site) {
            int ok = nav_play_call_target((uintptr_t)site, &target);
            printf("  %s E8 -> %p: %s\n", calls[k], (void *)(target - lo), ok ? "holds" : "FAIL");
            IMG_CHECK(ok);
            IMG_CHECK(target > lo && target < hi);
        }
    }
    {
        const uint8_t *record = image_site(base, results, n, "SnapEntityPlayText", identity->vulkan);
        uintptr_t getter = 0, setter = 0;
        IMG_CHECK(record && nav_play_call_target((uintptr_t)record + 7, &getter));
        IMG_CHECK(record && nav_play_call_target((uintptr_t)record + 21, &setter));
        IMG_CHECK(getter - lo == (identity->vulkan ? 0x119bb80u : 0x119adb0u));
        IMG_CHECK(setter - lo == (identity->vulkan ? 0x1563030u : 0x1564310u));
        printf("  SnapEntityPlayText: getter 0x%zx, runtime copy 0x%zx\n", (size_t)(getter-lo), (size_t)(setter-lo));
    }
    {
        int vk = identity->vulkan;
        const uint8_t *it = image_site(base, results, n, "SnapMapIterateAI", vk);
        uint32_t table = vk ? 0x24ad6a0u : 0x24c73f0u;
        uint64_t preferred = ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.ImageBase;
        uintptr_t target = 0;
        IMG_CHECK(it && *(const uint64_t *)(base + table + 0x8e8) == preferred + (it - base));
        IMG_CHECK(it && nav_play_call_target((uintptr_t)it + 0x10, &target) && target - lo == (vk ? 0x38f7e0u : 0x38f080u));
        IMG_CHECK(it && nav_play_call_target((uintptr_t)it + 0x2e, &target) && target - lo == (vk ? 0x38f750u : 0x38eff0u));
        IMG_CHECK(it && nav_play_call_target((uintptr_t)it + 0x9c, &target) && target - lo == (vk ? 0xa09ad0u : 0xa08cc0u));
        IMG_CHECK(it && !memcmp(it + 0x94, "\xba\x2b\x00\x00\x00\x49\x8b\xce", 8));
    }
    free(results);
    (void)sh_host_test_bind_image(NULL, NULL);
    free(base);
}

IMAGE_TEST_MAIN("nav_play_test", check_image, synthetic_main)
