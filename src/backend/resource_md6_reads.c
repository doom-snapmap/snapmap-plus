#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "resource_md6_reads.h"
#include "resource_graph_native.h"
#include "resource_graph.h"
#include "resource_instances.h"
#include "host_image.h"
#include "hook.h"
#include "decl_md6_rig_reads.h"

typedef struct md6_read_site { uint32_t caller;unsigned char call[5]; } md6_read_site;
#include "resource_md6_read_pins.h"
static const unsigned char *g_md6_base;
static int g_md6_renderer=-1;
static int (*g_md6_observe)(void *);
typedef void (*md6_rig_fn)(void *,void *);
static md6_rig_fn g_md6_rig_original;
static int g_md6_rig_ready;
static const uint32_t md6_rig_rvas[2]={0x3928e0,0x392e80};
static const unsigned char md6_rig_prefix[16]={0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x8b,0xf9};
static void md6_rig_hook(void *setup,void *resource);
static void md6_returned(void *model,uintptr_t caller);

static int md6_sites(void)
{
    if(!g_md6_base || g_md6_renderer<0 || g_md6_renderer>1 ||
       !sh_host_is_pinned_renderer_build(g_md6_renderer) || sh_host_is_vulkan()!=g_md6_renderer)return 0;
    __try {
        for(unsigned i=0;i<2;i++) {
            const md6_read_site *site=&md6_sites_by_renderer[g_md6_renderer][i];
            if(memcmp(g_md6_base+site->caller-5,site->call,5))return 0;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
int sh_resource_md6_reads_ready(void)
{
    if(!g_md6_observe || !md6_sites() || !g_md6_rig_ready || !g_md6_rig_original ||
       !hook_is_installed((void *)g_md6_rig_original) ||
       !sh_resource_graph_native_return_observer_ready(md6_returned))return 0;
    __try {
        const unsigned char *entry=g_md6_base+md6_rig_rvas[g_md6_renderer];void *target;
        static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
        if(memcmp(entry,jump,6) || entry[14]!=0x90 || entry[15]!=0x90)return 0;
        memcpy(&target,entry+6,sizeof target);return target==md6_rig_hook;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void md6_unknown(void)
{
    __try { if(g_md6_observe)g_md6_observe(NULL); }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
    sh_resource_graph_incomplete();
    __try { sh_resource_graph_native_observe_resource(NULL); }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
}
static void md6_returned(void *model,uintptr_t caller)
{
    if(!g_md6_base || g_md6_renderer<0 || g_md6_renderer>1 ||
       !sh_resource_graph_native_observing())return;
    int reached=0;
    for(unsigned i=0;i<2;i++)
        if(caller==(uintptr_t)g_md6_base+md6_sites_by_renderer[g_md6_renderer][i].caller)reached=1;
    if(!reached)return;
    if(!sh_resource_md6_reads_ready()) { md6_unknown();return; }
    /* The native caller first checks a nullable model, then consumes that
     * exact model's skeleton pointer. No lookup by model or skeleton name is
     * introduced; a warm object follows the same actual-read route. */
    if(!model)return;
    int complete=0;
    __try {
        uint64_t prior_model_token=sh_resource_instances_token(model);
        if(!g_md6_observe(model))__leave;
        uint64_t model_token=sh_resource_instances_token(model);
        if(!model_token || (prior_model_token && prior_model_token!=model_token))__leave;
        void *skeleton=*(void **)((unsigned char *)model+0x138);
        if(skeleton) {
            uint64_t prior_skeleton_token=sh_resource_instances_token(skeleton);
            if(!g_md6_observe(skeleton))__leave;
            uint64_t skeleton_token=sh_resource_instances_token(skeleton);
            if(!skeleton_token || (prior_skeleton_token && prior_skeleton_token!=skeleton_token) ||
               model_token!=sh_resource_instances_token(model) ||
               skeleton!=*(void **)((unsigned char *)model+0x138))__leave;
            /* These are the native parser's following payload pointers.
             * Reading their identity does not substitute a later generation. */
            void *header=*(void **)((unsigned char *)skeleton+0x40);
            void *symbols=*(void **)((unsigned char *)skeleton+0x50);
            if(skeleton_token!=sh_resource_instances_token(skeleton) ||
               header!=*(void **)((unsigned char *)skeleton+0x40) ||
               symbols!=*(void **)((unsigned char *)skeleton+0x50))__leave;
        }
        if(model_token!=sh_resource_instances_token(model) ||
           skeleton!=*(void **)((unsigned char *)model+0x138))__leave;
        complete=1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { }
    if(!complete)md6_unknown();
}
static int md6_read(void *context,uintptr_t address,void *out,size_t length)
{
    (void)context;
    if(!address || length>UINTPTR_MAX-address)return 0;
    __try { memcpy(out,(const void *)address,length);return 1; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void md6_rig_hook(void *setup,void *resource)
{
    int observing=sh_resource_graph_native_observing(),qualified=0,returned=0;
    void *model=NULL,*skeleton=NULL;uint64_t rt=0,mt=0,st=0;
    if(observing) {
        __try {
            if(!sh_resource_md6_reads_ready() || !resource ||
               (uintptr_t)resource>UINTPTR_MAX-0x430 ||
               setup!=(unsigned char *)resource+0x308)__leave;
            rt=sh_resource_instances_token(resource);
            if(!rt)__leave;
            model=*(void **)((unsigned char *)resource+0x60);
            if(!model)__leave;
            uint64_t before=sh_resource_instances_token(model);
            if(!g_md6_observe(model))__leave;
            mt=sh_resource_instances_token(model);
            if(!mt || (before && before!=mt))__leave;
            skeleton=*(void **)((unsigned char *)model+0x138);
            if(!skeleton)__leave;
            before=sh_resource_instances_token(skeleton);
            if(!g_md6_observe(skeleton))__leave;
            st=sh_resource_instances_token(skeleton);
            if(!st || (before && before!=st) ||
               rt!=sh_resource_instances_token(resource) || mt!=sh_resource_instances_token(model) ||
               model!=*(void **)((unsigned char *)resource+0x60) ||
               skeleton!=*(void **)((unsigned char *)model+0x138))__leave;
            uintptr_t table=*(uintptr_t *)resource;
            sh_resource_default_read_input input={(uintptr_t)g_md6_base,sh_host_image_size(),g_md6_renderer,
                sh_host_is_pinned_renderer_build(g_md6_renderer),(uintptr_t)resource,
                *(uintptr_t *)(table+0x50),md6_read,NULL,0};
            sh_resource_default_read_receipt receipt={0};
            receipt.invocation=input;receipt.state=1;receipt.reason=SH_DEFAULT_READ_UNSUPPORTED;
            qualified=sh_decl_md6_rig_read_check(&receipt,&input) &&
                sh_decl_md6_rig_bound_read_check(&receipt,&input,(uintptr_t)model,(uintptr_t)skeleton);
        } __except(EXCEPTION_EXECUTE_HANDLER) { qualified=0; }
        if(!qualified)md6_unknown();
    }
    /* The native callback and exception are never replaced by a probe. The
     * retained source graph owns qualification, including actual child reads. */
    __try { g_md6_rig_original(setup,resource);returned=1; }
    __finally {
        if(observing) {
            int stable=0;
            __try {
                stable=qualified && returned && sh_resource_md6_reads_ready() &&
                    rt==sh_resource_instances_token(resource) && mt==sh_resource_instances_token(model) &&
                    st==sh_resource_instances_token(skeleton) &&
                    model==*(void **)((unsigned char *)resource+0x60) &&
                    skeleton==*(void **)((unsigned char *)model+0x138);
            } __except(EXCEPTION_EXECUTE_HANDLER) { }
            if(!stable)md6_unknown();
        }
    }
}
int sh_resource_md6_reads_install(const unsigned char *base,int (*observe)(void *))
{
    if(g_md6_base)return base==g_md6_base && observe==g_md6_observe && sh_resource_md6_reads_ready();
    int renderer=sh_host_is_vulkan();
    if(!base || !observe || renderer<0 || renderer>1)return 0;
    g_md6_base=base;g_md6_renderer=renderer;g_md6_observe=observe;
    int intact=0;
    __try { intact=!memcmp(base+md6_rig_rvas[renderer],md6_rig_prefix,sizeof md6_rig_prefix); }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
    if(!md6_sites() || !intact || !sh_resource_graph_native_set_return_observer(md6_returned)) {
        g_md6_base=NULL;g_md6_renderer=-1;g_md6_observe=NULL;return 0;
    }
    void *trampoline=hook_prepare((void *)(base+md6_rig_rvas[renderer]),md6_rig_hook,sizeof md6_rig_prefix);
    if(!trampoline)return 0;
    g_md6_rig_original=(md6_rig_fn)trampoline;
    if(hook_commit(trampoline)!=B2_PATCH_OK) { (void)hook_unpatch(trampoline);return 0; }
    g_md6_rig_ready=1;
    return sh_resource_md6_reads_ready();
}
