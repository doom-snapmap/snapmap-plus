#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "resource_parm_reads.h"
#include "resource_graph.h"
#include "resource_graph_native.h"
#include "resource_instances.h"
#include "host_image.h"
#include "hook.h"

/* R-358: GetRenderParmFromOps_r invokes this evaluator for each executed op.
 * Opcode 0x14 uses the signed table index at op+4. The native registration
 * routines populate parallel declaration/payload arrays, and removal clears
 * both. These are table data pointers, not resource objects. Resolve through
 * the same slot and verify declaration+0x60 before recording its identity.
 * Bounds are the native registration limit, not a package-specific limit. */
typedef void (*rp_eval_fn)(const void *, const void *, const void *, const void *, void *);
typedef void *(*rp_default_fn)(void *, int);
static rp_eval_fn g_eval;
static rp_default_fn g_default;
static void **g_manager;
static int (*g_observe)(void *);
static volatile LONG g_ready, g_attempted;
static const uint8_t *g_base;
static int g_renderer;
static const uint32_t g_eval_rva[2] = {0x1923f20, 0x192ec40};
static const uint32_t g_manager_rva[2] = {0x3e5e248, 0x555bb90};
static const uint32_t g_default_rva[2] = {0x19211c0, 0x192e570};
static const unsigned char g_default_head[16] = {
    0x48,0x63,0xc2,0x48,0x8b,0x04,0xc1,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc
};
static const unsigned char g_head[2][19] = {
    {0x40,0x53,0x48,0x81,0xec,0xa0,0,0,0,0x0f,0xb7,0x19,0x4c,0x8d,0x1d,0xcd,0xc0,0x6d,0xfe},
    {0x40,0x53,0x48,0x81,0xec,0xa0,0,0,0,0x0f,0xb7,0x19,0x4c,0x8d,0x1d,0xad,0x13,0x6d,0xfe}
};
typedef struct rp_table {
    void *manager, *declaration, *payload;
    int index, count;
} rp_table;

/* 0 is an operation that does not read a table; -1 is an unobserved table
 * read. Never suppress or replace the original native operation. */
static int rp_resolve(const void *operation, void *manager, rp_table *out)
{
    __try {
        const uint16_t *op = operation;
        if ((op[0] & 0x1f) != 0x14) return 0;
        int index = (int)(int16_t)op[2];
        if (!manager) return -1;
        int count = *(int *)((unsigned char *)manager + 0x10180);
        if (count < 0 || count > 0x800 || index < 0 || index >= count) return -1;
        void *decl = ((void **)((unsigned char *)manager + 0x8180))[index];
        void *payload = ((void **)((unsigned char *)manager + 0xc180))[index];
        if (!decl || !payload || *(void **)((unsigned char *)decl + 0x60) != payload) return -1;
        *out = (rp_table){manager, decl, payload, index, count};
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static void *rp_manager(void)
{
    __try { return g_manager ? *g_manager : NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void rp_unknown(void)
{
    __try { if(g_observe && g_observe(NULL)) return; }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
    sh_resource_graph_incomplete();
    __try { sh_resource_graph_native_observe_resource(NULL); }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
}
static int rp_observe(void *resource)
{
    int ok=0;
    __try { if(resource && g_observe) ok=g_observe(resource); }
    __except(EXCEPTION_EXECUTE_HANDLER) { }
    if(!ok) rp_unknown();
    return ok;
}
/* GetRenderParmFromOps_r follows computed operand slots and reads the actual
 * default declaration returned by this leaf. Slots are assigned by native
 * AllocParmState; no slot number is assumed to name a particular declaration. */
static void *rp_default_slot(void *manager, int index)
{
    __try {
        if (!manager || manager != rp_manager() || index < 0 || index >= 0x93f) return NULL;
        void *declaration = ((void **)manager)[index];
        if (!declaration || *(int *)((unsigned char *)declaration + 0x68) != index) return NULL;
        return declaration;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *rp_default(void *manager, int index)
{
    int observing=sh_resource_graph_native_observing(),ready=sh_resource_parm_reads_ready(),returned=0;
    void *before=observing && ready ? rp_default_slot(manager,index):NULL,*result=NULL;
    uint64_t token=before ? sh_resource_instances_token(before):0;
    __try { result=g_default(manager,index);returned=1; }
    __finally { if(observing) {
        void *after=ready ? rp_default_slot(manager,index):NULL;
        if(!returned || !ready || !before || before!=result || before!=after) rp_unknown();
        else if(rp_observe(before)) {
            uint64_t current=sh_resource_instances_token(before);
            if(!current || (token && current!=token))rp_unknown();
        }
    } }
    return result;
}
static void rp_eval(const void *operation, const void *value, const void *left, const void *right, void *result)
{
    rp_table before = {0}, after = {0};
    int recorded = 0,observing=sh_resource_graph_native_observing(),returned=0;
    uint64_t token=0;
    if (observing && sh_resource_parm_reads_ready()) {
        recorded = rp_resolve(operation, rp_manager(), &before);
        if(recorded<0)rp_unknown();
        else if(recorded>0 && rp_observe(before.declaration)) {
            token=sh_resource_instances_token(before.declaration);
            if(!token)rp_unknown();
        }
    } else if(observing)rp_unknown();
    __try { g_eval(operation, value, left, right, result);returned=1; }
    __finally {
    /* A changed registration cannot establish a complete read of the copied
     * identity. Existing native synchronization is unchanged. */
    if(observing && !returned)rp_unknown();
    else if (recorded > 0 && (rp_resolve(operation, rp_manager(), &after) != 1 ||
        before.manager != after.manager || before.declaration != after.declaration ||
        before.payload != after.payload || before.index != after.index || before.count != after.count ||
        !token || sh_resource_instances_token(before.declaration)!=token)) rp_unknown();
    }
}
int sh_resource_parm_reads_install(const uint8_t *base, int (*observe)(void *))
{
    if (InterlockedCompareExchange(&g_attempted, 1, 0)) return sh_resource_parm_reads_ready();
    int renderer = sh_host_is_vulkan();
    if (!base || !observe || renderer < 0 || renderer > 1 ||
        !sh_host_is_pinned_renderer_build(renderer)) return 0;
    __try {
        if (memcmp(base + g_eval_rva[renderer], g_head[renderer], sizeof(g_head[renderer]))) return 0;
        if (memcmp(base + g_default_rva[renderer], g_default_head, sizeof(g_default_head))) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    g_observe = observe;
    g_base=base;g_renderer=renderer;
    g_manager = (void **)(base + g_manager_rva[renderer]);
    /* push; sub; movzx; RIP-relative LEA of the image base: the supported
     * trampoline expands exactly that LEA. No relative branch is stolen. */
    g_eval = (rp_eval_fn)hook_prepare_rip_lea((void *)(base + g_eval_rva[renderer]), rp_eval, 19, 12);
    /* Eight-byte leaf followed by eight verified INT3 bytes. The trampoline
     * returns at the original RET; no next-function byte is overwritten. */
    g_default = (rp_default_fn)hook_prepare((void *)(base + g_default_rva[renderer]), rp_default, 14);
    if (!g_eval || !g_default || hook_commit((void *)g_eval) != B2_PATCH_OK ||
        hook_commit((void *)g_default) != B2_PATCH_OK) {
        if(g_default) (void)hook_unpatch((void *)g_default);
        if(g_eval) (void)hook_unpatch((void *)g_eval);
        return 0;
    }
    InterlockedExchange(&g_ready, 1);
    if(!sh_resource_parm_expression_reads_install(base,observe))return 0;
    return sh_resource_parm_reads_ready();
}
int sh_resource_parm_reads_ready(void)
{
    if(!InterlockedCompareExchange(&g_ready,0,0) || !g_base || sh_host_is_vulkan()!=g_renderer ||
       !sh_host_is_pinned_renderer_build(g_renderer) || !g_eval || !g_default ||
       !hook_is_installed((void *)g_eval) || !hook_is_installed((void *)g_default)) return 0;
    const uint32_t rvas[2]={g_eval_rva[g_renderer],g_default_rva[g_renderer]};
    void *detours[2]={rp_eval,rp_default};
    __try { for(unsigned i=0;i<2;i++) {
        static const unsigned char jump[6]={0xff,0x25,0,0,0,0};void *target=NULL;
        const unsigned char *entry=g_base+rvas[i];
        if(memcmp(entry,jump,6))return 0;
        memcpy(&target,entry+6,8);if(target!=detours[i])return 0;
    } } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return sh_resource_parm_expression_reads_ready();
}

#ifdef SH_RESOURCE_PARM_READS_TESTING
void sh_resource_parm_reads_test_observe(const void *operation, void *manager)
{
    rp_table table = {0};
    int found = rp_resolve(operation, manager, &table);
    if (found) g_observe(found > 0 ? table.declaration : NULL);
}
void sh_resource_parm_reads_test_reset(void)
{
    g_eval = NULL; g_default = NULL; g_manager = NULL; g_observe = NULL;
    g_base=NULL;g_renderer=0;
    g_ready = g_attempted = 0;
}
#endif
