/* Exercise queue detouring with authored machine code, never game bytes. */
#include "../src/backend/nav_play.c"

void backend_log(const char *message) {(void)message;}
void sh_map_render_build(void *map) {(void)map;}
int sh_config_get_bool(const char *key,int *value,unsigned *flags)
{(void)key;(void)flags;*value=1;return 1;}
void sh_nav_bake_build_begin(void) {}
void sh_nav_bake_build_end(void) {}
void sh_nav_bake_enable_instances(int on) {(void)on;}
int sh_nav_bake_instance_name(int i,const char *n,char *out,size_t cap)
{(void)i;(void)n;(void)out;(void)cap;return 0;}

static int failed, published;
#define CHECK(x) do { if (!(x)) { ++failed; printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)

static void observe_publication(void *target)
{
    sh_nav_heap_node nodes[1]={{0}}, item={40,0,1,0};
    sh_nav_heap_list list={nodes,1,1,0x03800000,0};
    (void)target;
    CHECK(g_heap_push != NULL);
    if (g_heap_push) {
        g_heap_push(NULL,&item,&list);
        CHECK(nodes[0].cost==40 && nodes[0].index==1);
    }
    ++published;
}

int main(void)
{
    /* Fifteen NOPs, then copy the pending node to the queue's first slot. */
    const unsigned char copy[]={0x49,0x8b,0x00,0x4c,0x8b,0x0a,0x4c,0x89,0x08,0xc3};
    unsigned char *code=VirtualAlloc(NULL,64,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    sh_nav_heap_node nodes[2]={{0},{0xa5a5,0xa5a5,0xa5a5,0xa5a5}};
    sh_nav_heap_list list={nodes,1,1,0x03800000,0};
    sh_nav_heap_node item={40,0,1,0};
    sig_result site={"NavSearchHeapPush",SIG_OK,0,0};
    int before=hook_owned_count();
    if(!code)return 1;
    memset(code,0x90,15);memcpy(code+15,copy,sizeof copy);
    FlushInstructionCache(GetCurrentProcess(),code,64);
    site.addr=(uintptr_t)code;
    sh_patch_test_observe_write(observe_publication);
    sh_patch_test_faults(2|4|8,0,0);
    CHECK(!nav_heap_install(&site,1));
    CHECK(g_heap_push && !hook_is_installed((void *)g_heap_push));
    CHECK(hook_owned_count()==before+1);
    sh_patch_test_faults(0,0,0);
    CHECK(nav_heap_install(&site,1));
    CHECK(published && hook_is_installed((void *)g_heap_push));
    CHECK(nav_heap_install(&site,1));
    ((nav_heap_push_fn)code)(NULL,&item,&list);
    CHECK(nodes[0].cost==40 && nodes[0].index==1);
    item.cost=3;
    ((nav_heap_push_fn)code)(NULL,&item,&list);
    CHECK(nodes[0].cost==3 && nodes[0].index==1);
    item.cost=80;
    ((nav_heap_push_fn)code)(NULL,&item,&list);
    CHECK(nodes[0].cost==3); /* Full queue retains the cheaper existing route. */
    CHECK(nodes[1].index==0xa5a5 && nodes[1].cost==0xa5a5);
    sh_patch_test_observe_write(NULL);
    CHECK(hook_unpatch((void *)g_heap_push));g_heap_push=NULL;
    CHECK(hook_owned_count()==before);
    CHECK(!memcmp(code+15,copy,sizeof copy) && code[0]==0x90);
    VirtualFree(code,0,MEM_RELEASE);
    printf("nav_heap_hook_test: %d failures\n",failed);
    return failed?1:0;
}
