#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "resource_graph_native.h"
#include "resource_graph.h"
#include "resource_resident.h"
#include "hook.h"
#include "backend_log.h"

typedef void (*rg_load_fn)(void *resource);
typedef void *(*rg_lookup_fn)(void *manager, const char *name);
typedef void *(*rg_materialize_fn)(void *manager, const char *name, unsigned char create_default, unsigned char flags);
typedef void (*rg_state_fn)(void *type_info, void *entity, void *reader, unsigned char initialize);
typedef unsigned int (*rg_parse_fn)(void *resource, unsigned char read_source, void *error_text);
typedef unsigned int (*rg_read_fn)(void *resource, void *error_text);
static rg_load_fn g_load;
static rg_lookup_fn g_lookup;
static rg_materialize_fn g_materialize;
static rg_state_fn g_state;
static rg_parse_fn g_parse;
static rg_read_fn g_read;
static volatile LONG g_ready, g_attempted;
static const char *const g_required[] = {
    "ResourceGenericLoad", "ResourceLookup", "ResourceMaterialize",
    "SetEntityEditState", "DeclParseSource", "DeclRead"
};
int sh_resource_graph_native_signature(const char *name)
{
    size_t i;
    if (name) for (i = 0; i < sizeof(g_required) / sizeof(g_required[0]); i++)
        if (!strcmp(name, g_required[i])) return 1;
    return 0;
}

/* Both renderer initializers and idResourceList::Add establish these common
 * prefixes. Copy the identity while the native object is alive. The graph
 * retains names, never engine object addresses. */
static int rg_identity(void *resource, char type[128], char name[4096])
{
    __try {
        void *manager = *(void **)((unsigned char *)resource + 0x10);
        const char *t = manager ? *(const char **)((unsigned char *)manager + 8) : NULL;
        const char *n = *(const char **)((unsigned char *)resource + 8);
        if (!t || !n || !*t || !*n || strnlen(t, 128) >= 128 || strnlen(n, 4096) >= 4096) return 0;
        strcpy_s(type, 128, t); strcpy_s(name, 4096, n); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void rg_returned(void *resource)
{
    char type[128], name[4096];
    if (resource && rg_identity(resource, type, name)) sh_resource_graph_reference(type, name);
}
static void rg_load_hook(void *resource)
{
    char type[128], name[4096];
    sh_resource_graph_frame frame;
    int tracked = 0, parsed = 0;
    if (InterlockedCompareExchange(&g_ready, 0, 0)) {
        int identified = rg_identity(resource, type, name);
        sh_resource_graph_begin(&frame, identified ? type : NULL, identified ? name : NULL); tracked = 1;
    }
    __try {
        g_load(resource);
        /* Both native materializers reject resource+0x18 errors unless the
         * caller permits a default. GenericLoad sets flag 0x04 on its fallback
         * branch, not on successful parsing (which returns with it clear). */
        parsed = *(void **)((unsigned char *)resource + 0x18) == NULL &&
                 (*(unsigned char *)((unsigned char *)resource + 0x2c) & 4) == 0;
        if (parsed) sh_resource_resident_touch(resource);
    } __finally {
        if (tracked) sh_resource_graph_end(&frame, parsed);
    }
}
static void *rg_lookup_hook(void *manager, const char *name)
{
    void *resource = g_lookup(manager, name);
    sh_resource_resident_touch(resource);
    if (sh_resource_graph_recording()) rg_returned(resource);
    return resource;
}
static int rg_begin_source(void *resource, sh_resource_graph_frame *frame)
{
    char type[128], name[4096];
    if (!rg_identity(resource, type, name)) sh_resource_graph_pause(frame);
    else if (sh_resource_graph_source_active(type, name)) return 0;
    else sh_resource_graph_begin(frame, type, name);
    return 1;
}
/* DeclParseSource also runs directly from editing and touchDecl, without
 * GenericLoad. Unregistered temporary declarations have no manager identity
 * and must isolate their references from any surrounding canonical resource.
 * The generic load already owns a scope when it delegates to this parser. */
static unsigned int rg_parse_hook(void *resource, unsigned char read_source, void *error_text)
{
    sh_resource_graph_frame frame;
    unsigned int result = 1;
    int tracked = 0;
    if (InterlockedCompareExchange(&g_ready, 0, 0)) tracked = rg_begin_source(resource, &frame);
    __try { result = g_parse(resource, read_source, error_text); }
    __finally { if (tracked) sh_resource_graph_end(&frame, result == 0); }
    return result;
}
/* idDecl::Read directly invokes the typed parser for production files. That
 * branch never reaches DeclParseSource. Its implicit-text flag instead returns
 * success without reading anything; preserve the prior graph in that case. */
static int rg_read_has_source(void *resource)
{
    __try { return *(unsigned char *)((unsigned char *)resource + 0x48) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static unsigned int rg_read_hook(void *resource, void *error_text)
{
    sh_resource_graph_frame frame;
    unsigned int result = 1;
    int tracked = 0;
    if (InterlockedCompareExchange(&g_ready, 0, 0)) {
        if (rg_read_has_source(resource)) tracked = rg_begin_source(resource, &frame);
        else { sh_resource_graph_pause(&frame); tracked = 1; }
    }
    __try { result = g_read(resource, error_text); }
    __finally { if (tracked) sh_resource_graph_end(&frame, result == 0); }
    return result;
}
static void *rg_materialize_hook(void *manager, const char *name, unsigned char create_default, unsigned char flags)
{
    void *resource = g_materialize(manager, name, create_default, flags);
    sh_resource_resident_touch(resource);
    if (sh_resource_graph_recording()) rg_returned(resource);
    return resource;
}
/* SetEntityEditState receives the same reader layout in both renderers. Its
 * lexer starts at +0x1020 and retains the original text pointer at +0x140.
 * Only a definition's prepared state buffer can populate its shared graph;
 * inline map edits and partial parent-state applications are separate scopes.
 * These pointers are compared during the call, never retained by the graph. */
static int rg_state_identity(void *entity, void *reader, char type[128], char name[4096], int *expanded)
{
    __try {
        void *definition = *(void **)((unsigned char *)entity + 0x6d0);
        const void *source = *(const void **)((unsigned char *)reader + 0x1160);
        if (!definition || !source ||
            !rg_identity(definition, type, name) || _stricmp(type, "entityDef") ||
            source != *(const void **)((unsigned char *)definition + 0x140)) return 0;
        *expanded = *(unsigned char *)((unsigned char *)definition + 0x128) != 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void rg_state_hook(void *type_info, void *entity, void *reader, unsigned char initialize)
{
    char type[128], name[4096];
    sh_resource_graph_frame frame;
    int tracked = 0, canonical = 0, parsed = 0, expanded = 0;
    if (InterlockedCompareExchange(&g_ready, 0, 0)) {
        canonical = initialize && rg_state_identity(entity, reader, type, name, &expanded);
        if (canonical) sh_resource_graph_begin_state(&frame, type, name, expanded);
        else sh_resource_graph_pause(&frame);
        tracked = 1;
    }
    __try {
        g_state(type_info, entity, reader, initialize);
        /* The native error getter reads lexer+0x1e0 in both renderers. */
        if (canonical) parsed = *(unsigned char *)((unsigned char *)reader + 0x1200) == 0;
    } __finally {
        if (tracked) sh_resource_graph_end(&frame, parsed);
    }
}
static void *rg_clean(const sig_result *results, size_t count, const char *name)
{
    size_t i;
    for (i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name))
        return results[i].status == SIG_OK ? (void *)results[i].addr : NULL;
    return NULL;
}
int sh_resource_graph_native_install(const sig_result *results, size_t count)
{
    void *load, *lookup, *materialize, *state, *parse, *read;
    if (InterlockedCompareExchange(&g_attempted, 1, 0)) return sh_resource_graph_native_ready();
    load = rg_clean(results, count, g_required[0]);
    lookup = rg_clean(results, count, g_required[1]);
    materialize = rg_clean(results, count, g_required[2]);
    state = rg_clean(results, count, g_required[3]);
    parse = rg_clean(results, count, g_required[4]);
    read = rg_clean(results, count, g_required[5]);
    if (!load || !lookup || !materialize || !state || !parse || !read) goto failed;
    /* Whole position-independent instruction sequences verified separately
     * against Vulkan and OpenGL. Publish call-throughs before any commit. */
    g_load = (rg_load_fn)hook_prepare(load, rg_load_hook, 20);
    g_lookup = (rg_lookup_fn)hook_prepare(lookup, rg_lookup_hook, 19);
    g_materialize = (rg_materialize_fn)hook_prepare(materialize, rg_materialize_hook, 19);
    g_state = (rg_state_fn)hook_prepare(state, rg_state_hook, 20);
    g_parse = (rg_parse_fn)hook_prepare(parse, rg_parse_hook, 19);
    g_read = (rg_read_fn)hook_prepare(read, rg_read_hook, 16);
    if (!g_load || !g_lookup || !g_materialize || !g_state || !g_parse || !g_read) goto failed;
    if (hook_commit((void *)g_load) != B2_PATCH_OK ||
        hook_commit((void *)g_lookup) != B2_PATCH_OK ||
        hook_commit((void *)g_materialize) != B2_PATCH_OK ||
        hook_commit((void *)g_state) != B2_PATCH_OK ||
        hook_commit((void *)g_parse) != B2_PATCH_OK ||
        hook_commit((void *)g_read) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_ready, 1);
    backend_log("package dependencies: generic loads, production and direct source parses, canonical entity state and file requests are being recorded");
    return 1;
failed:
    /* A partial installation remains a transparent call-through. Do not free
     * executable trampolines while native threads might still execute them. */
    backend_log("package dependencies unavailable: native resource hooks did not install completely");
    return 0;
}
int sh_resource_graph_native_ready(void) { return InterlockedCompareExchange(&g_ready, 0, 0) != 0; }
