#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "resource_graph_native.h"
#include "resource_graph.h"
#include "resource_resident.h"
#include "package_shader_receipt.h"
#include "package_font.h"
#include "package_movie.h"
#include "package_runtime.h"
#include "engine_globals.h"
#include "hook.h"
#include "backend_log.h"
#include "host_image.h"

typedef void (*rg_load_fn)(void *resource);
typedef void *(*rg_lookup_fn)(void *manager, const char *name);
typedef void *(*rg_materialize_fn)(void *manager, const char *name, unsigned char create_default, unsigned char flags);
typedef void *(*rg_optional_material_fn)(void *manager, const char *name, unsigned char create_default);
typedef void (*rg_state_fn)(void *type_info, void *entity, void *reader, unsigned char initialize);
typedef unsigned int (*rg_parse_fn)(void *resource, unsigned char read_source, void *error_text);
typedef unsigned int (*rg_read_fn)(void *resource, void *error_text);
typedef void (*rg_set_text_fn)(void *resource, const void *text, int length);
typedef void (*rg_shader_write_fn)(const void *metadata, void *file);
typedef uintptr_t (*rg_shader_compile_fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rg_parm_publish_fn)(void *manager, void *parameter, const void *value);
typedef void (*rg_parm_write_fn)(void *state, void *parameter, const void *value);
typedef uint32_t (*rg_buffer_name_fn)(void *manager, const char *name);
typedef struct rg_parm_value {
    struct rg_parm_value *next;
    void *manager, *state, *parameter, *owner;
    uint32_t index, id, kind, base_depth, depth;
    unsigned char value[16];
} rg_parm_value;
typedef struct rg_fixed_value {
    struct rg_fixed_value *next;
    void *resource;
    size_t length;
    unsigned char header[0x90];
} rg_fixed_value;
typedef struct rg_buffer_value {
    struct rg_buffer_value *next;
    unsigned char *entry;
    void *name, *buffers[2], *types[2];
    unsigned char dual;
    size_t offset;
} rg_buffer_value;
struct sh_resource_shader_state {
    rg_parm_value *values;
    rg_fixed_value *fixed;
    rg_buffer_value *buffers;
    sh_resource_shader_admission admitted;
    void *context;
    DWORD thread;
    int failed;
};
static __declspec(thread) sh_resource_shader_state *g_shader_state;
static rg_load_fn g_load;
static rg_load_fn g_reconstruct;
static rg_lookup_fn g_lookup;
static rg_materialize_fn g_materialize;
static rg_state_fn g_state;
static rg_parse_fn g_parse;
static rg_read_fn g_read;
static rg_read_fn g_font_read;
static rg_optional_material_fn g_optional_material;
static void **g_resource_head;
typedef struct rg_font_frame {
    sh_package_consumer_hold hold;
    const char *names[SH_PACKAGE_FONT_MATERIAL_COUNT];
    void *materials[SH_PACKAGE_FONT_MATERIAL_COUNT];
    int failed;
} rg_font_frame;
static __declspec(thread) rg_font_frame *g_font_frame;
/* A cooked UI movie constructor holds its compiled plan while it resolves its
 * dictionary fonts (through the language resolver) and fixed GUI materials.
 * Only requests the plan binds to a different identity are prepared/routed. */
typedef void *(*rg_swf_construct_fn)(void *swf, const char *name, void *parent, unsigned char flags);
enum { RG_MOVIE_TARGETS = 64 };
typedef struct rg_movie_frame {
    sh_package_consumer_hold hold;
    const char *families[RG_MOVIE_TARGETS], *names[RG_MOVIE_TARGETS];
    void *resources[RG_MOVIE_TARGETS];
    size_t count;
    int failed;
} rg_movie_frame;
static __declspec(thread) rg_movie_frame *g_movie_frame;
static rg_swf_construct_fn g_swf_construct;
static volatile LONG g_movie_ready, g_movie_attempted;
static rg_set_text_fn g_set_text;
static rg_shader_write_fn g_shader_write;
static rg_shader_compile_fn g_shader_compile;
static rg_parm_publish_fn g_parm_publish;
static rg_parm_write_fn g_parm_write;
static rg_buffer_name_fn g_buffer_name;
static volatile LONG g_ready, g_attempted;
/* These workspaces contain native compiler-owned allocations, never a copy of
 * a live resource. Paired constructors/destructors own every nested allocation. */
typedef uintptr_t (*rg_shader_worker_fn)(void *, void *, const char *, const void *, void *, void *, void *, void *);
typedef void *(*rg_object_init_fn)(void *);
typedef void (*rg_object_clear_fn)(void *);
typedef struct rg_cook_api {
    rg_object_init_fn string_init, stage_init, metadata_init;
    rg_object_clear_fn string_clear, stage_clear, stage_reset, metadata_clear, lexer_clear;
    void *(*lexer_init)(void *, unsigned);
    void (*lexer_flags)(void *, unsigned);
    unsigned char (*lexer_load)(void *, const void *, int, const char *);
    unsigned char (*lexer_expect)(void *, const char *);
    unsigned char (*lexer_error)(void *), (*lexer_warning)(void *);
    void *(*permutation_name)(void *, const char *, uint64_t);
    int (*mode)(void);
    void **filesystem, **heap;
    const unsigned char *sparse, *aniso;
    const int *feedback, *validate;
    void *flag_manager;
    rg_shader_worker_fn worker;
    int renderer, ready;
} rg_cook_api;
static rg_cook_api g_cook_api;
typedef struct rg_cook_name {
    void **vtable;
    char *name;
    void *manager;
    struct rg_cook_name *next;
} rg_cook_name;
typedef struct rg_cook_frame {
    struct rg_cook_frame *parent;
    const char *name;
} rg_cook_frame;
typedef struct rg_cook_unit rg_cook_unit;
typedef struct rg_cook_edge { struct rg_cook_edge *next; rg_cook_unit *child; } rg_cook_edge;
struct rg_cook_unit {
    rg_cook_unit *next, *queue_next;
    rg_cook_edge *children;
    char *name;
    unsigned char *source;
    size_t length, incoming;
    int from_file;
};
typedef struct rg_cook_context {
    rg_cook_name *names;
    rg_cook_frame *frame;
    rg_cook_unit *units, *tail;
    unsigned char settings[12];
    sh_resource_shader_source_reader original;
    void *original_context;
    int failed;
} rg_cook_context;
static __declspec(thread) rg_cook_context *g_cook;
int sh_resource_graph_native_cook_input(const char *path, unsigned char **body, size_t *length)
{
    int status = -1;
    *body = NULL; *length = 0;
    if (!g_cook || !g_cook->original) return 0;
    __try {
        status = g_cook->original(g_cook->original_context, path, body, length);
        if ((status == 1 || status == 2) && *body && *length <= 16u * 1024u * 1024u) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { status = -1; }
    free(*body); *body = NULL; *length = 0;
    if (status != 0) g_cook->failed = 1;
    return -1;
}
static int rg_cook_install(void);
static void *rg_cook_lookup(void *manager, const char *name, int *handled);
static const char *const g_required[] = {
    "ResourceGenericLoad", "ResourceLookup", "ResourceMaterialize",
    "SetEntityEditState", "DeclParseSource", "DeclRead", "DeclSetText", "ResourceReconstruct",
    "ShaderSourceCompile", "ShaderMetadataWrite", "ShaderParmDefaultsPublish", "ShaderParmStateWrite",
    "ShaderBufferFindName", "FontRead", "DeclFind"
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
static int rg_receipt_read(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    __try { memcpy(out, (const void *)address, length); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void rg_parm_values_free(rg_parm_value *values)
{ while (values) { rg_parm_value *next = values->next; free(values); values = next; } }
static rg_fixed_value *rg_fixed_find(void *resource)
{
    rg_fixed_value *value = g_shader_state ? g_shader_state->fixed : NULL;
    while (value && value->resource != resource) value = value->next;
    return value;
}
static int rg_fixed_capture(void *resource)
{
    char type[128], name[4096]; unsigned char header[0x90]; size_t length = 0; uintptr_t error = 0;
    if (!g_shader_state || rg_fixed_find(resource) || !rg_identity(resource, type, name)) return 1;
    if (!_stricmp(type, "renderparm")) length = 0x90;
    else if (!_stricmp(type, "renderprogflag")) length = 0x60;
    else return 1;
    if (!rg_receipt_read(NULL, (uintptr_t)resource, header, length)) return 0;
    memcpy(&error, header + 0x18, 8);
    if (error || (header[0x2c] & 7)) return 1; /* New/pending objects have no prior fixed payload. */
    if (length == 0x90) {
        uint32_t index; memcpy(&index, header + 0x68, 4);
        if (index >= 0x93f) return 0; /* A ready object must not enter native slot allocation. */
    }
    rg_fixed_value *value = calloc(1, sizeof(*value)); if (!value) return 0;
    value->resource = resource; value->length = length; memcpy(value->header, header, length);
    value->next = g_shader_state->fixed; g_shader_state->fixed = value; return 1;
}
static int rg_fixed_restore(rg_fixed_value *value)
{
    __try {
        unsigned char *resource = value->resource;
        if (memcmp(resource, value->header, 0x18) || memcmp(resource + 0x20, value->header + 0x20, 4)) return 0;
        /* Payload fields are native defaults, fixed index/mask and source-record
         * references. String/typed-buffer text uses the engine atom pool; this
         * same-object read does not destroy their ownership or the assigned slot. */
        memcpy(resource + 0x48, value->header + 0x48, value->length - 0x48);
        return !memcmp(resource + 0x48, value->header + 0x48, value->length - 0x48) &&
            !memcmp(resource + 0x18, value->header + 0x18, 8) &&
            (resource[0x2c] & 7) == (value->header[0x2c] & 7);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rg_fixed_finish(void *resource, unsigned result)
{
    rg_fixed_value *value = rg_fixed_find(resource); unsigned char header[0x90];
    if (!value) return 0;
    int changed = !rg_receipt_read(NULL, (uintptr_t)resource, header, value->length) ||
        memcmp(header + 0x48, value->header + 0x48, value->length - 0x48);
    if (!result && !changed && !g_shader_state->failed) return 0;
    g_shader_state->failed = 1;
    if (!rg_fixed_restore(value)) return -1;
    sh_package_shader_receipt_invalidate((uintptr_t)resource); return 1;
}
static int rg_parm_registered(void *manager, void *state)
{
    void **states = *(void ***)((unsigned char *)manager + 0x10188);
    int count = *(int *)((unsigned char *)manager + 0x10190);
    if (count < 0 || count > 65536 || (count && !states)) return 0;
    for (int i = 0; i < count; i++) if (states[i] == state) return 1;
    return 0;
}
static int rg_parm_stage(sh_resource_shader_state *scope, void *manager, void *parameter)
{
    rg_parm_value *staged = NULL; int ok = 0;
    __try {
        uint32_t index = *(uint32_t *)((unsigned char *)parameter + 0x68);
        uint32_t kind = *(uint32_t *)((unsigned char *)parameter + 0x6c);
        rg_fixed_value *fixed = rg_fixed_find(parameter);
        if (index >= 0x93f) __leave;
        /* A newly constructed original has no previous live value. Its native
         * first publication initializes the newly assigned slot. */
        if (!fixed) return 1;
        if (fixed) {
            uint32_t original_index; memcpy(&original_index, fixed->header + 0x68, 4);
            if (original_index != index) __leave;
            memcpy(&kind, fixed->header + 0x6c, 4);
        }
        void **states = *(void ***)((unsigned char *)manager + 0x10188);
        int count = *(int *)((unsigned char *)manager + 0x10190);
        if (count < 0 || count > 65536 || (count && !states)) __leave;
        ok = 1;
        for (int i = 0; i < count; i++) {
            unsigned char *state = states[i];
            uint32_t base = *(uint32_t *)(state + 0xb970), depth = *(uint32_t *)(state + 0xb974);
            if (base != depth || *(uint32_t *)(state + 0xb97c)) { ok = 0; break; }
            rg_parm_value *old = scope->values;
            while (old && (old->manager != manager || old->state != state || old->index != index)) old = old->next;
            if (old) {
                if (old->parameter != parameter || old->depth != depth || old->base_depth != base ||
                    old->owner != *(void **)((unsigned char *)parameter + 0x10) ||
                    old->id != *(uint32_t *)((unsigned char *)parameter + 0x20)) { ok = 0; break; }
                continue;
            }
            rg_parm_value *item = calloc(1, sizeof(*item));
            if (!item) { ok = 0; break; }
            item->manager = manager; item->state = state; item->parameter = parameter; item->index = index;
            item->owner = *(void **)((unsigned char *)parameter + 0x10);
            item->id = *(uint32_t *)((unsigned char *)parameter + 0x20);
            item->kind = kind;
            item->base_depth = base; item->depth = depth;
            memcpy(item->value, state + 0x70 + (size_t)index * 16, 16);
            item->next = staged; staged = item;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (ok) while (staged) { rg_parm_value *item = staged; staged = item->next;
        item->next = scope->values; scope->values = item; }
    rg_parm_values_free(staged); return ok;
}
static int rg_buffer_stage(sh_resource_shader_state *scope, void *manager, uint32_t index)
{
    rg_buffer_value *item = NULL; int ok = 0;
    __try {
        unsigned char *entry = (unsigned char *)manager + 8 + (size_t)index * 0x20;
        for (rg_buffer_value *old = scope->buffers; old; old = old->next)
            if (old->entry == entry) return *(void **)entry == old->name && entry[8] == old->dual &&
                *(void **)(entry + 0x10) == old->buffers[0] && *(void **)(entry + 0x18) == old->buffers[1];
        item = calloc(1, sizeof(*item));
        if (!item) __leave;
        item->entry = entry; item->name = *(void **)entry; item->dual = entry[8];
        item->buffers[0] = *(void **)(entry + 0x10); item->buffers[1] = *(void **)(entry + 0x18);
        item->offset = sh_host_is_vulkan() ? 0x48 : 0x18;
        if (!item->name || !item->buffers[0] || (item->dual && !item->buffers[1])) __leave;
        item->types[0] = *(void **)((unsigned char *)item->buffers[0] + item->offset);
        if (item->dual) item->types[1] = *(void **)((unsigned char *)item->buffers[1] + item->offset);
        item->next = scope->buffers; scope->buffers = item; ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) free(item);
    return ok;
}
static uint32_t rg_buffer_name_hook(void *manager, const char *name)
{
    uint32_t index = g_buffer_name(manager, name);
    sh_resource_shader_state *scope = g_shader_state;
    if (scope && index < 16 && (scope->failed || !scope->admitted(scope->context) ||
        !rg_buffer_stage(scope, manager, index))) {
        scope->failed = 1;
        /* The native default parser explicitly skips the type-pointer write
         * for this absent-index result. Never expose an uncaptured buffer. */
        return UINT32_MAX;
    }
    return index;
}
static void rg_parm_publish_hook(void *manager, void *parameter, const void *value)
{
    sh_resource_shader_state *scope = g_shader_state;
    if (scope && (scope->failed || !scope->admitted(scope->context) || !rg_parm_stage(scope, manager, parameter))) {
        scope->failed = 1; return; /* Never forward an uncaptured scoped mutation. */
    }
    g_parm_publish(manager, parameter, value);
}
sh_resource_shader_state *sh_resource_graph_native_shader_state_begin(
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (g_shader_state || !sh_resource_graph_native_ready() || !g_parm_publish || !g_parm_write || !g_buffer_name ||
        !admitted || !admitted(context)) {
        if (error && capacity) snprintf(error, capacity, "shader state preservation requires admitted renderer ownership"); return NULL;
    }
    sh_resource_shader_state *scope = calloc(1, sizeof(*scope));
    if (!scope) return NULL;
    scope->admitted = admitted; scope->context = context; scope->thread = GetCurrentThreadId();
    g_shader_state = scope; sh_package_shader_receipt_program_scope(1); return scope;
}
int sh_resource_graph_native_shader_state_restore(sh_resource_shader_state *scope,
    char *error, size_t capacity)
{
    if (!scope || scope != g_shader_state || scope->thread != GetCurrentThreadId() || !scope->admitted(scope->context)) {
        if (scope && scope == g_shader_state) {
            scope->failed = 1; sh_package_shader_receipt_revoke_scope();
        }
        if (error && capacity) snprintf(error, capacity, "shader state preservation lost renderer ownership before restoration");
        return 0; /* Keep the captured obligation until the owner can restore it. */
    }
    int ok = 1;
    for (rg_fixed_value *value = scope->fixed; value; value = value->next)
        if (!rg_fixed_restore(value)) ok = 0;
    for (rg_buffer_value *item = scope->buffers; item; item = item->next) {
        __try {
            if (*(void **)item->entry != item->name || item->entry[8] != item->dual ||
                *(void **)(item->entry + 0x10) != item->buffers[0] ||
                *(void **)(item->entry + 0x18) != item->buffers[1]) { ok = 0; continue; }
            for (size_t i = 0; i < (item->dual ? 2u : 1u); i++)
                *(void **)((unsigned char *)item->buffers[i] + item->offset) = item->types[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    for (rg_parm_value *item = scope->values; item; item = item->next) {
        __try {
            unsigned char *state = item->state, *parameter = item->parameter;
            if (!rg_parm_registered(item->manager, state) || *(void **)(parameter + 0x10) != item->owner ||
                *(uint32_t *)(parameter + 0x20) != item->id || *(uint32_t *)(parameter + 0x68) != item->index ||
                *(uint32_t *)(parameter + 0x6c) != item->kind || *(uint32_t *)(state + 0xb970) != item->base_depth ||
                *(uint32_t *)(state + 0xb974) != item->depth || *(uint32_t *)(state + 0xb97c)) { ok = 0; continue; }
            g_parm_write(state, parameter, item->value);
            if (memcmp(state + 0x70 + (size_t)item->index * 16, item->value, 16) ||
                *(uint32_t *)(state + 0xb970) != item->base_depth || *(uint32_t *)(state + 0xb974) != item->depth ||
                *(uint32_t *)(state + 0xb97c)) ok = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    if (!ok) {
        scope->failed = 1; sh_package_shader_receipt_revoke_scope();
        if (error && capacity) snprintf(error, capacity, "native shader state restoration failed; candidate evidence revoked");
    }
    return ok;
}
int sh_resource_graph_native_shader_state_end(sh_resource_shader_state *scope,
    int accepted, char *error, size_t capacity)
{
    int ok = sh_resource_graph_native_shader_state_restore(scope, error, capacity);
    if (!ok) return -1; /* The owner must retain admission and retry restoration. */
    if (!ok || !accepted || scope->failed) sh_package_shader_receipt_revoke_scope();
    if (scope->failed && accepted) ok = 0;
    sh_package_shader_receipt_program_scope(0); g_shader_state = NULL;
    rg_parm_values_free(scope->values);
    while (scope->fixed) { rg_fixed_value *value = scope->fixed; scope->fixed = value->next; free(value); }
    while (scope->buffers) { rg_buffer_value *value = scope->buffers; scope->buffers = value->next; free(value); }
    free(scope);
    if (!ok && error && capacity) snprintf(error, capacity, "native shader state restoration failed; candidate evidence revoked");
    return ok;
}
static void rg_receipt_retained(void *resource)
{
    __try {
        const void *body = *(const void **)((unsigned char *)resource + 0x38);
        int length = *(int *)((unsigned char *)resource + 0x40);
        if (body && length > 0) sh_package_shader_receipt_retained(body, (size_t)length);
    } __except (EXCEPTION_EXECUTE_HANDLER) { sh_package_shader_receipt_retained(NULL, 0); }
}
static void rg_set_text_hook(void *resource, const void *text, int length)
{
    int observing = InterlockedCompareExchange(&g_ready, 0, 0) != 0;
    if (observing) sh_package_shader_receipt_text_begin((uintptr_t)resource);
    g_set_text(resource, text, length);
    if (observing) {
        __try {
            const void *body = *(const void **)((unsigned char *)resource + 0x38);
            int retained = *(int *)((unsigned char *)resource + 0x40);
            sh_package_shader_receipt_text((uintptr_t)resource, body, retained > 0 ? (size_t)retained : 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sh_package_shader_receipt_text((uintptr_t)resource, NULL, 0);
        }
    }
}
static uintptr_t rg_shader_compile_hook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,
    uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i)
{
    uintptr_t resource = sh_package_shader_receipt_program_current();
    uintptr_t result = g_shader_compile(a, b, c, d, e, f, g, h, i);
    /* A normal compiler return is only a witness. Successful native parsing,
     * exact inputs, output framing and ABI checks are still independently required. */
    sh_package_shader_receipt_program_compiled(resource); return result;
}
typedef struct rg_shader_file {
    void **vtable;
    unsigned char *body;
    size_t length, capacity;
    int failed;
} rg_shader_file;
static long long rg_shader_write(rg_shader_file *file, const void *body, uint64_t length)
{
    const size_t limit = 64u * 1024u * 1024u;
    if (file->failed || (!body && length) || length > limit || file->length > limit - (size_t)length) {
        file->failed = 1; return 0;
    }
    size_t next = file->length + (size_t)length;
    if (next > file->capacity) {
        size_t capacity = file->capacity ? file->capacity * 2 : 4096;
        if (capacity < next) capacity = next;
        if (capacity > limit) capacity = limit;
        unsigned char *grown = realloc(file->body, capacity);
        if (!grown) { file->failed = 1; return 0; }
        file->body = grown; file->capacity = capacity;
    }
    if (length) memcpy(file->body + file->length, body, (size_t)length);
    file->length = next; return (long long)length;
}
static uintptr_t rg_shader_unsupported(rg_shader_file *file)
{ file->failed = 1; return 0; }
static void rg_shader_capture(void *resource)
{
    if (!g_shader_write || (uintptr_t)resource != sh_package_shader_receipt_program_current()) return;
    /* Paired native serializers and every helper use only Write (+0x30).
     * Unexpected methods abort capture; no native stream or object is replaced. */
    void *vtable[31]; for (size_t i = 0; i < 31; i++) vtable[i] = rg_shader_unsupported;
    vtable[6] = rg_shader_write;
    rg_shader_file file = {vtable, NULL, 0, 0, 0};
    __try { g_shader_write((const unsigned char *)resource + 0x78, &file); }
    __except (EXCEPTION_EXECUTE_HANDLER) { file.failed = 1; }
    if (!file.failed) sh_package_shader_receipt_program_wrote((uintptr_t)resource, file.body, file.length);
    free(file.body);
}
static void rg_load_hook(void *resource)
{
    char type[128], name[4096];
    sh_resource_graph_frame frame;
    sh_package_shader_receipt_frame receipt;
    int tracked = 0, parsed = 0;
    rg_font_frame *font_parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    if (g_shader_state && !rg_fixed_capture(resource)) { g_shader_state->failed = 1; return; }
    if (InterlockedCompareExchange(&g_ready, 0, 0)) {
        int identified = rg_identity(resource, type, name);
        sh_resource_graph_begin(&frame, identified ? type : NULL, identified ? name : NULL); tracked = 1;
        sh_package_shader_receipt_begin(&receipt, (uintptr_t)resource, identified ? type : NULL, identified ? name : NULL);
    }
    __try {
        g_font_frame = NULL; g_movie_frame = NULL; /* Nested constructors never inherit a plan. */
        g_load(resource);
        /* Both native materializers reject resource+0x18 errors unless the
         * caller permits a default. GenericLoad sets flag 0x04 on its fallback
         * branch, not on successful parsing (which returns with it clear). */
        parsed = *(void **)((unsigned char *)resource + 0x18) == NULL &&
                 (*(unsigned char *)((unsigned char *)resource + 0x2c) & 4) == 0;
        if (parsed) { sh_resource_resident_touch(resource); rg_shader_capture(resource); }
    } __finally {
        g_font_frame = font_parent; g_movie_frame = movie_parent;
        if (rg_fixed_finish(resource, parsed ? 0 : 1)) parsed = 0;
        if (tracked) {
            sh_package_shader_receipt_end(&receipt, parsed, rg_receipt_read, NULL);
            sh_resource_graph_end(&frame, parsed);
        }
    }
}
static void rg_reconstruct_hook(void *resource)
{
    sh_package_shader_receipt_invalidate((uintptr_t)resource);
    g_reconstruct(resource);
}
static void *rg_lookup_hook(void *manager, const char *name)
{
    int handled = 0; void *probe = rg_cook_lookup(manager, name, &handled);
    if (handled) return probe;
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
    sh_package_shader_receipt_frame receipt;
    unsigned int result = 1;
    int tracked = 0, observing = InterlockedCompareExchange(&g_ready, 0, 0) != 0;
    rg_font_frame *font_parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    if (g_shader_state && !rg_fixed_capture(resource)) { g_shader_state->failed = 1; return 1; }
    if (observing) {
        char type[128], name[4096]; int identified = rg_identity(resource, type, name);
        tracked = rg_begin_source(resource, &frame);
        sh_package_shader_receipt_begin(&receipt, (uintptr_t)resource, identified ? type : NULL, identified ? name : NULL);
        if (!read_source) rg_receipt_retained(resource);
    }
    __try { g_font_frame = NULL; g_movie_frame = NULL; result = g_parse(resource, read_source, error_text); if (!result) rg_shader_capture(resource); }
    __finally {
        g_font_frame = font_parent; g_movie_frame = movie_parent;
        int restored = rg_fixed_finish(resource, result);
        if (observing) sh_package_shader_receipt_end(&receipt, result == 0 && !restored, rg_receipt_read, NULL);
        if (tracked) sh_resource_graph_end(&frame, result == 0 && !restored);
    }
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
    sh_package_shader_receipt_frame receipt;
    unsigned int result = 1;
    int tracked = 0, observing = InterlockedCompareExchange(&g_ready, 0, 0) != 0;
    int restored = 0;
    rg_font_frame *font_parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    if (g_shader_state && !rg_fixed_capture(resource)) { g_shader_state->failed = 1; return 0; }
    if (observing) {
        char type[128], name[4096]; int source = rg_read_has_source(resource);
        int identified = source && rg_identity(resource, type, name);
        if (source) tracked = rg_begin_source(resource, &frame);
        else { sh_resource_graph_pause(&frame); tracked = 1; }
        sh_package_shader_receipt_begin(&receipt, source ? (uintptr_t)resource : 0,
            identified ? type : NULL, identified ? name : NULL);
    }
    __try { g_font_frame = NULL; g_movie_frame = NULL; result = g_read(resource, error_text); if (!result) rg_shader_capture(resource); }
    __finally {
        g_font_frame = font_parent; g_movie_frame = movie_parent;
        restored = rg_fixed_finish(resource, result);
        if (observing) sh_package_shader_receipt_end(&receipt, result == 0 && !restored, rg_receipt_read, NULL);
        if (tracked) sh_resource_graph_end(&frame, result == 0 && !restored);
    }
    /* GenericLoad's zero branch performs only successful-read bookkeeping.
     * A restored preexisting fixed object must not enter its reconstruct/default
     * fallback. The independent scope/receipt failure still aborts preparation. */
    return restored == 1 ? 0 : result;
}
static int rg_font_route(void *manager, const char *name, void **resource)
{
    rg_font_frame *frame = g_font_frame; *resource = NULL;
    if (!frame || !frame->hold.binding) return 0;
    __try {
        const char *family = manager ? *(const char **)((unsigned char *)manager + 8) : NULL;
        const char *target = NULL;
        if (!family || _stricmp(family, "material")) return 0;
        int route = sh_package_font_route(frame->hold.binding, family, name, &target);
        if (!route) return 0;
        if (route == 2) return 1; /* Positively absent: do not run native source probes. */
        if (route == 1) for (size_t i = 0; i < SH_PACKAGE_FONT_MATERIAL_COUNT; i++)
            if (frame->names[i] && !strcmp(frame->names[i], target) && frame->materials[i]) {
                *resource = frame->materials[i]; return 1;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    frame->failed = 1; return 1;
}
static void *rg_font_manager(void)
{
    __try {
        void *manager = g_resource_head ? *g_resource_head : NULL;
        for (size_t i = 0; manager && i < 4096; i++) {
            const char *family = *(const char **)((unsigned char *)manager + 8);
            if (family && !_stricmp(family, "material")) return manager;
            manager = *(void **)((unsigned char *)manager + 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return NULL;
}
static int rg_font_prepare(rg_font_frame *frame)
{
    if (!sh_package_font_plan(frame->hold.binding, frame->names)) return 0;
    void *manager = rg_font_manager();
    if (!manager || !g_materialize) return 0;
    for (size_t i = 0; i < SH_PACKAGE_FONT_MATERIAL_COUNT; i++) if (frame->names[i]) {
        for (size_t j = 0; j < i; j++) if (frame->names[j] && !strcmp(frame->names[i], frame->names[j])) {
            frame->materials[i] = frame->materials[j]; break;
        }
        if (frame->materials[i]) continue;
        /* No font plan is active during dependency construction. A default or
         * failed dependency must stop before the font dereferences its material. */
        void *material = g_materialize(manager, frame->names[i], 0, 0);
        if (!material || *(void **)((unsigned char *)material + 0x18) ||
            (*(unsigned char *)((unsigned char *)material + 0x2c) & 7)) return 0;
        frame->materials[i] = material; sh_resource_resident_touch(material);
        if (sh_resource_graph_recording()) rg_returned(material);
    }
    return 1;
}
static unsigned int rg_font_read_hook(void *resource, void *error_text)
{
    if (!InterlockedCompareExchange(&g_ready, 0, 0)) return g_font_read(resource, error_text);
    rg_font_frame frame = {0}, *parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    sh_resource_graph_frame graph; int tracked = 0, compiled = 0;
    unsigned int result = 1; char family[128], name[4096];
    /* Mask even an uncompiled nested font. It must never borrow its parent's
     * bindings. Provider holds and masks unwind through native SEH exceptions. */
    __try {
        g_font_frame = NULL; g_movie_frame = NULL;
        if (!rg_identity(resource, family, name) || _stricmp(family, "font")) {
            result = g_font_read(resource, error_text); __leave;
        }
        int captured = sh_package_runtime_consumer_acquire("font", name, &frame.hold);
        if (captured < 0) __leave;
        /* An exact original glyph envelope the native loader rejects has no
         * captured requests; its native failure/default path is unchanged. */
        if (!captured || !frame.hold.binding->count) { result = g_font_read(resource, error_text); __leave; }
        compiled = 1;
        tracked = rg_begin_source(resource, &graph);
        if (!rg_font_prepare(&frame)) __leave;
        g_font_frame = &frame;
        result = g_font_read(resource, error_text);
        if (frame.failed) result = 1;
    } __finally {
        g_font_frame = parent; g_movie_frame = movie_parent;
        if (tracked) sh_resource_graph_end(&graph, result == 0);
        sh_package_runtime_consumer_release(&frame.hold);
    }
    if (result && compiled) backend_log("package font construction failed; native loader retains its normal failure/recovery path");
    return result;
}
static void *rg_manager(const char *family)
{
    __try {
        void *manager = g_resource_head ? *g_resource_head : NULL;
        for (size_t i = 0; manager && i < 4096; i++) {
            const char *label = *(const char **)((unsigned char *)manager + 8);
            if (label && !_stricmp(label, family)) return manager;
            manager = *(void **)((unsigned char *)manager + 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return NULL;
}
/* Slots are movie/<font|material>/<native request>. Only a request bound to a
 * different identity is prepared; an original binding keeps native lookup. */
static int rg_movie_prepare(rg_movie_frame *frame)
{
    const sh_package_consumer *owner = frame->hold.binding;
    for (size_t i = 0; i < owner->count; i++) {
        const sh_package_consumer_target *target = owner->targets + i;
        const char *request = NULL, *family = NULL;
        if (!strncmp(target->slot, "movie/font/", 11)) { family = "font"; request = target->slot + 11; }
        else if (!strncmp(target->slot, "movie/material/", 15)) { family = "material"; request = target->slot + 15; }
        else return 0;
        if (!target->family || _stricmp(target->family, family) || !target->name || !*target->name) return 0;
        if (!strcmp(target->name, request)) continue;
        if (frame->count == RG_MOVIE_TARGETS) return 0;
        void *manager = rg_manager(family);
        if (!manager || !g_materialize) return 0;
        /* No movie plan is active while its private dependency constructs. */
        void *resource = g_materialize(manager, target->name, 0, 0);
        if (!resource || *(void **)((unsigned char *)resource + 0x18) ||
            (*(unsigned char *)((unsigned char *)resource + 0x2c) & 7)) return 0;
        sh_resource_resident_touch(resource);
        if (sh_resource_graph_recording()) rg_returned(resource);
        frame->families[frame->count] = family; frame->names[frame->count] = target->name;
        frame->resources[frame->count++] = resource;
    }
    return 1;
}
static int rg_movie_route(void *manager, const char *name, void **resource)
{
    rg_movie_frame *frame = g_movie_frame; *resource = NULL;
    if (!frame || !frame->hold.binding) return 0;
    __try {
        const char *family = manager ? *(const char **)((unsigned char *)manager + 8) : NULL;
        const char *target = NULL;
        if (!family) return 0;
        int route = sh_package_movie_route(frame->hold.binding, family, name, &target);
        if (route == 1) for (size_t i = 0; i < frame->count; i++)
            if (!_stricmp(frame->families[i], family) && !strcmp(frame->names[i], target)) {
                *resource = frame->resources[i]; return 1;
            }
        if (!route) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    /* A contradictory or unprepared binding keeps native lookup and is reported. */
    frame->failed = 1; return 0;
}
static void *rg_swf_construct_hook(void *swf, const char *name, void *parent, unsigned char flags)
{
    if (!InterlockedCompareExchange(&g_movie_ready, 0, 0)) return g_swf_construct(swf, name, parent, flags);
    rg_movie_frame frame = {0}, *movie_parent = g_movie_frame;
    rg_font_frame *font_parent = g_font_frame;
    sh_package_movie_names names;
    void *result = NULL; int prepared = 0, compiled = 0;
    __try {
        g_movie_frame = NULL; g_font_frame = NULL; /* Nested movies never inherit a plan. */
        if (!name || sh_package_movie_names_for_consumer(name, strlen(name), &names) != 1 ||
            sh_package_runtime_consumer_acquire("file", names.movie, &frame.hold) != 1) {
            result = g_swf_construct(swf, name, parent, flags); __leave;
        }
        compiled = 1;
        prepared = rg_movie_prepare(&frame);
        if (prepared && frame.count) g_movie_frame = &frame;
        result = g_swf_construct(swf, name, parent, flags);
    } __finally {
        g_movie_frame = movie_parent; g_font_frame = font_parent;
        sh_package_runtime_consumer_release(&frame.hold);
    }
    /* A constructor cannot fail; an unbindable plan keeps the native requests. */
    if (compiled && (!prepared || frame.failed))
        backend_log("package movie construction kept native font/material lookups: its compiled plan could not be prepared");
    return result;
}
static void rg_movie_install(const sig_result *results, size_t count)
{
    void *construct = NULL;
    if (InterlockedCompareExchange(&g_movie_ready, 0, 0) || !sh_resource_graph_native_ready()) return;
    for (size_t i = 0; results && i < count; i++) if (results[i].name && !strcmp(results[i].name, "SwfConstruct")) {
        if (results[i].status != SIG_OK || !results[i].addr) return;
        construct = (void *)results[i].addr;
    }
    if (!construct || InterlockedCompareExchange(&g_movie_attempted, 1, 0)) return;
    /* Seven pushes and the frame LEA: 20 position-independent bytes, identical
     * in both retail renderers. */
    g_swf_construct = (rg_swf_construct_fn)hook_prepare(construct, rg_swf_construct_hook, 20);
    if (!g_swf_construct || hook_commit((void *)g_swf_construct) != B2_PATCH_OK) {
        backend_log("package movie routing unavailable: constructor hook did not install"); return;
    }
    InterlockedExchange(&g_movie_ready, 1);
}
static void *rg_optional_material_hook(void *manager, const char *name, unsigned char create_default)
{
    void *resource;
    if (!rg_font_route(manager, name, &resource) && !rg_movie_route(manager, name, &resource))
        return g_optional_material(manager, name, create_default);
    sh_resource_resident_touch(resource);
    if (sh_resource_graph_recording()) rg_returned(resource);
    return resource;
}
static void *rg_materialize_hook(void *manager, const char *name, unsigned char create_default, unsigned char flags)
{
    int handled = 0; void *probe = rg_cook_lookup(manager, name, &handled);
    if (handled) return probe;
    void *resource;
    if (!rg_font_route(manager, name, &resource) && !rg_movie_route(manager, name, &resource))
        resource = g_materialize(manager, name, create_default, flags);
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
    rg_font_frame *font_parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    if (InterlockedCompareExchange(&g_ready, 0, 0)) {
        canonical = initialize && rg_state_identity(entity, reader, type, name, &expanded);
        if (canonical) sh_resource_graph_begin_state(&frame, type, name, expanded);
        else sh_resource_graph_pause(&frame);
        tracked = 1;
    }
    __try {
        g_font_frame = NULL; g_movie_frame = NULL;
        g_state(type_info, entity, reader, initialize);
        /* The native error getter reads lexer+0x1e0 in both renderers. */
        if (canonical) parsed = *(unsigned char *)((unsigned char *)reader + 0x1200) == 0;
    } __finally {
        g_font_frame = font_parent; g_movie_frame = movie_parent;
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
    void *load, *lookup, *materialize, *state, *parse, *read, *set_text, *reconstruct, *shader_compile, *parm_publish, *buffer_name;
    void *font_read, *optional_material;
    if (InterlockedCompareExchange(&g_attempted, 1, 0)) {
        rg_movie_install(results, count);
        return sh_resource_graph_native_ready();
    }
    load = rg_clean(results, count, g_required[0]);
    lookup = rg_clean(results, count, g_required[1]);
    materialize = rg_clean(results, count, g_required[2]);
    state = rg_clean(results, count, g_required[3]);
    parse = rg_clean(results, count, g_required[4]);
    read = rg_clean(results, count, g_required[5]);
    set_text = rg_clean(results, count, g_required[6]);
    reconstruct = rg_clean(results, count, g_required[7]);
    shader_compile = rg_clean(results, count, g_required[8]);
    g_shader_write = (rg_shader_write_fn)rg_clean(results, count, g_required[9]);
    parm_publish = rg_clean(results, count, g_required[10]);
    g_parm_write = (rg_parm_write_fn)rg_clean(results, count, g_required[11]);
    buffer_name = rg_clean(results, count, g_required[12]);
    font_read = rg_clean(results, count, g_required[13]);
    optional_material = rg_clean(results, count, g_required[14]);
    g_resource_head = (void **)glb_resolve(sh_host_image_base(), "decl_resource_head", NULL);
    if (!load || !lookup || !materialize || !state || !parse || !read || !set_text || !reconstruct ||
        !shader_compile || !g_shader_write || !parm_publish || !g_parm_write || !buffer_name ||
        !font_read || !optional_material || !g_resource_head) goto failed;
    /* Whole position-independent instruction sequences verified separately
     * against Vulkan and OpenGL. Publish call-throughs before any commit. */
    g_load = (rg_load_fn)hook_prepare(load, rg_load_hook, 20);
    g_lookup = (rg_lookup_fn)hook_prepare(lookup, rg_lookup_hook, 19);
    g_materialize = (rg_materialize_fn)hook_prepare(materialize, rg_materialize_hook, 19);
    g_state = (rg_state_fn)hook_prepare(state, rg_state_hook, 20);
    g_parse = (rg_parse_fn)hook_prepare(parse, rg_parse_hook, 19);
    g_read = (rg_read_fn)hook_prepare(read, rg_read_hook, 16);
    g_set_text = (rg_set_text_fn)hook_prepare(set_text, rg_set_text_hook, 15);
    g_reconstruct = (rg_load_fn)hook_prepare(reconstruct, rg_reconstruct_hook, 15);
    g_shader_compile = (rg_shader_compile_fn)hook_prepare(shader_compile, rg_shader_compile_hook, 21);
    g_parm_publish = (rg_parm_publish_fn)hook_prepare(parm_publish, rg_parm_publish_hook, 20);
    g_buffer_name = (rg_buffer_name_fn)hook_prepare(buffer_name, rg_buffer_name_hook, 17);
    g_font_read = (rg_read_fn)hook_prepare(font_read, rg_font_read_hook, 18);
    g_optional_material = (rg_optional_material_fn)hook_prepare(optional_material, rg_optional_material_hook, 20);
    if (!g_load || !g_lookup || !g_materialize || !g_state || !g_parse || !g_read || !g_set_text || !g_reconstruct || !g_shader_compile || !g_parm_publish || !g_buffer_name || !g_font_read || !g_optional_material) goto failed;
    if (hook_commit((void *)g_load) != B2_PATCH_OK ||
        hook_commit((void *)g_lookup) != B2_PATCH_OK ||
        hook_commit((void *)g_materialize) != B2_PATCH_OK ||
        hook_commit((void *)g_state) != B2_PATCH_OK ||
        hook_commit((void *)g_parse) != B2_PATCH_OK ||
        hook_commit((void *)g_read) != B2_PATCH_OK ||
        hook_commit((void *)g_set_text) != B2_PATCH_OK ||
        hook_commit((void *)g_reconstruct) != B2_PATCH_OK ||
        hook_commit((void *)g_shader_compile) != B2_PATCH_OK ||
        hook_commit((void *)g_parm_publish) != B2_PATCH_OK ||
        hook_commit((void *)g_buffer_name) != B2_PATCH_OK ||
        hook_commit((void *)g_font_read) != B2_PATCH_OK ||
        hook_commit((void *)g_optional_material) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_ready, 1);
    rg_movie_install(results, count);
    rg_cook_install();
    backend_log("package dependencies: generic loads, production and direct source parses, canonical entity state and file requests are being recorded");
    return 1;
failed:
    /* A partial installation remains a transparent call-through. Do not free
     * executable trampolines while native threads might still execute them. */
    backend_log("package dependencies unavailable: native resource hooks did not install completely");
    return 0;
}
int sh_resource_graph_native_ready(void) { return InterlockedCompareExchange(&g_ready, 0, 0) != 0; }
int sh_resource_graph_native_consumer_ready(const char *family)
{
    if (family && !strcmp(family, "file"))
        return sh_resource_graph_native_ready() && InterlockedCompareExchange(&g_movie_ready, 0, 0) &&
            g_optional_material && g_materialize && g_resource_head;
    return family && !strcmp(family, "font") && sh_resource_graph_native_ready() &&
        g_font_read && g_optional_material && g_materialize && g_resource_head;
}

typedef struct rg_cook_work {
    unsigned char sections[7][0x30];
    unsigned char stages[3][0x730];
    unsigned char metadata[0x17b0];
    unsigned sections_ready, stages_ready;
    int metadata_ready;
} rg_cook_work;
static uintptr_t rg_cook_unused(void *object)
{ (void)object; if (g_cook) g_cook->failed = 1; return 0; }
static void *rg_cook_manager(rg_cook_name *object) { return object->manager; }
static void *rg_cook_lookup(void *manager, const char *name, int *handled)
{
    *handled = 0;
    if (!g_cook) return NULL;
    __try {
        const char *family = manager ? *(const char **)((unsigned char *)manager + 8) : NULL;
        if (!family || _stricmp(family, "renderprog")) return NULL;
        *handled = 1;
        if (!name || !*name || strnlen(name, 4096) == 4096 || g_cook->failed) return NULL;
        for (rg_cook_name *item = g_cook->names; item; item = item->next)
            if (item->manager == manager && !_stricmp(item->name, name)) return item;
        rg_cook_name *item = calloc(1, sizeof(*item));
        if (item) { item->name = _strdup(name); item->vtable = malloc(16 * sizeof(void *)); }
        if (!item || !item->name || !item->vtable) {
            if (item) { free(item->name); free(item->vtable); free(item); }
            g_cook->failed = 1; return NULL;
        }
        for (size_t i = 0; i < 16; i++) item->vtable[i] = rg_cook_unused;
        item->vtable[2] = rg_cook_manager; item->vtable[14] = rg_cook_manager;
        item->manager = manager; item->next = g_cook->names; g_cook->names = item;
        return item;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_cook->failed = 1; *handled = 1; return NULL; }
}
static int rg_cook_settings(unsigned char settings[12])
{
    if (!g_cook_api.ready) return 0;
    __try {
        memset(settings, 0, 12);
        uint32_t platform = g_cook_api.renderer ? 2 : 0; memcpy(settings, &platform, 4);
        if (!g_cook_api.renderer) {
            settings[4] = *g_cook_api.sparse;
            settings[5] = *g_cook_api.aniso == 0;
        }
        settings[6] = *g_cook_api.feedback != 0; settings[7] = 1;
        settings[8] = *g_cook_api.validate != 0; settings[9] = 1;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rg_cook_path(const char *name, int inherited, char path[4096])
{
    int n = snprintf(path, 4096, strstr(name, ".decl") ? "%s" : inherited ?
        "generated/renderprogs/%s.decl" : "generated/decls/renderprog/%s.decl", name);
    return n > 0 && n < 4096;
}
static int rg_cook_read(const char *path, unsigned char **body, size_t *length)
{
    void *native = NULL, *filesystem = NULL; int bytes = 0, ok = 0;
    *body = NULL; *length = 0;
    __try {
        filesystem = *g_cook_api.filesystem;
        /* Both native ReadFile implementations forward the fifth argument to
         * OpenFile and the allocation flags. Match the declaration reader's
         * zero flags instead of letting unrelated stack bytes select a mode. */
        int (*read)(void *, const char *, void **, void *, unsigned) = (void *)(*(void ***)filesystem)[25];
        bytes = read(filesystem, path, &native, NULL, 0);
        if (bytes > 0 && bytes <= 16 * 1024 * 1024 && native) {
            *body = malloc((size_t)bytes + 1);
            if (*body) {
                memcpy(*body, native, (size_t)bytes); (*body)[bytes] = 0;
                *length = (size_t)bytes; ok = 1;
                /* The returned allocation is the actual engine read, not a
                 * later catalog reread inferred from the requested name. */
                sh_package_shader_receipt_bytes(path, *length, 0, *body, *length);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (native) {
        __try {
            void *heap = *g_cook_api.heap;
            void (*release)(void *, void *, size_t) = (void *)(*(void ***)heap)[2];
            release(heap, native, 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    if (!ok) { free(*body); *body = NULL; *length = 0; }
    return ok;
}
/* Actual root/ancestor files and derived permutation text share this compiler
 * boundary. Ancestors borrow the native caller's section/stage workspaces,
 * preserving native inheritance ordering instead of reparsing author syntax. */
static int rg_cook_source(const char *name, const void *settings, int inherited,
    const void *generated, size_t generated_length, const void *expected, size_t expected_length,
    void *metadata, int *metadata_ready, void *sections, void *vertex, void *fragment, void *compute)
{
    unsigned char lexer[0x720] = {0}, declaration[0x58] = {0};
    unsigned char *body = NULL; size_t length = 0;
    char path[4096], label[4096]; int ok = 0, lexer_ready = 0, text_ready = 0;
    rg_cook_frame call = {g_cook->frame, name}; sh_package_shader_receipt_frame receipt;
    *metadata_ready = 0;
    for (rg_cook_frame *old = call.parent; old; old = old->parent)
        if (!_stricmp(old->name, name)) { g_cook->failed = 1; return 0; }
    if (!rg_cook_path(name, 0, path) ||
        !rg_cook_path(name, inherited, label)) { g_cook->failed = 1; return 0; }
    g_cook->frame = &call;
    sh_package_shader_cook_begin(&receipt, (uintptr_t)&call, g_cook_api.renderer, name);
    __try {
        if (generated) {
            if (!generated_length || generated_length > 16u * 1024u * 1024u) __leave;
            length = generated_length; body = malloc(length + 1); if (!body) __leave;
            memcpy(body, generated, length); body[length] = 0;
            sh_package_shader_receipt_bytes(path, length, 0, body, length);
        } else if (!rg_cook_read(path, &body, &length)) __leave;
        if (expected && (length != expected_length || memcmp(body, expected, length))) __leave;
        /* Native SetText supplies the exact trim/checksum contract on a fresh
         * base prefix. It never receives a registered resource or borrowed text. */
        text_ready = 1; g_set_text(declaration, body, (int)length);
        const void *text = *(const void **)(declaration + 0x38);
        int text_length = *(int *)(declaration + 0x40);
        if (!text || text_length <= 0) __leave;
        sh_package_shader_receipt_retained(text, (size_t)text_length);
        g_cook_api.lexer_init(lexer, 0); lexer_ready = 1;
        g_cook_api.lexer_flags(lexer, inherited || g_cook_api.renderer ? 0x4c630 : 0x4c430);
        if (!g_cook_api.lexer_load(lexer, text, text_length, label) ||
            !g_cook_api.lexer_expect(lexer, "{")) __leave;
        uintptr_t returned = g_shader_compile((uintptr_t)metadata, (uintptr_t)name, (uintptr_t)settings,
            (uintptr_t)lexer, *(uint32_t *)(declaration + 0x44), (uintptr_t)sections,
            (uintptr_t)vertex, (uintptr_t)fragment, (uintptr_t)compute);
        *metadata_ready = 1;
        if (returned != (uintptr_t)metadata || g_cook_api.lexer_error(lexer) ||
            g_cook_api.lexer_warning(lexer) || g_cook->failed || g_shader_state->failed) __leave;
        sh_package_shader_receipt_program_compiled((uintptr_t)&call);
        void *vtable[31]; for (size_t i = 0; i < 31; i++) vtable[i] = rg_shader_unsupported;
        vtable[6] = rg_shader_write;
        rg_shader_file file = {vtable, NULL, 0, 0, 0};
        __try { g_shader_write(metadata, &file);
            ok = !file.failed && sh_package_shader_receipt_program_wrote((uintptr_t)&call, file.body, file.length); }
        __finally { free(file.body); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    __try { if (lexer_ready) g_cook_api.lexer_clear(lexer);
        if (text_ready) g_set_text(declaration, NULL, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    free(body); sh_package_shader_cook_end(&receipt, ok); g_cook->frame = call.parent;
    if (!ok) g_cook->failed = 1;
    return ok;
}
static uintptr_t rg_shader_worker_hook(void *resource, void *metadata, const char *name,
    const void *settings, void *sections, void *vertex, void *fragment, void *compute)
{
    if (!g_cook) return g_cook_api.worker(resource, metadata, name, settings, sections, vertex, fragment, compute);
    int ready = 0;
    rg_cook_source(name, settings, 1, NULL, 0, NULL, 0, metadata, &ready, sections, vertex, fragment, compute);
    /* The native caller always copy-constructs and destroys this return value.
     * A failed read must still return a constructed empty metadata object. */
    if (!ready) g_cook_api.metadata_init(metadata);
    return (uintptr_t)metadata;
}
static int rg_cook_install(void)
{
    const unsigned char *base = sh_host_image_base(); int vk = sh_host_is_vulkan();
    if (!base || vk < 0 || !sh_host_is_pinned_renderer_build(vk)) return 0;
    /* A renderer-library observation alone must not select a different table.
     * This call-only helper was independently resolved by the signature pass. */
    if ((const void *)g_shader_write != base + (vk ? 0xd80280 : 0xd80630)) return 0;
    /* Exact pinned renderer mappings; no cross-renderer address inference.
     * The worker's first 20 bytes are complete position-independent pushes and
     * stack allocation. All other entries are call-only native helpers. */
    static const unsigned char worker_head[20] = {
        0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,
        0x56,0x41,0x57,0x48,0x81,0xec,0xf8,0x07,0,0
    };
    void *worker = (void *)(base + (vk ? 0xd7e330 : 0xd7e760));
    if (memcmp(worker, worker_head, sizeof(worker_head))) return 0;
#define RG_COOK_BIND(field, type, gl, vulkan) g_cook_api.field = (type)(base + (vk ? (vulkan) : (gl)))
    RG_COOK_BIND(string_init, rg_object_init_fn, 0x19ef2a0, 0x19fd040);
    RG_COOK_BIND(string_clear, rg_object_clear_fn, 0x19ef380, 0x19fd120);
    RG_COOK_BIND(stage_init, rg_object_init_fn, 0xd7cae0, 0xd7c890);
    RG_COOK_BIND(stage_clear, rg_object_clear_fn, 0xd7d150, 0xd7cf00);
    RG_COOK_BIND(stage_reset, rg_object_clear_fn, 0xd83460, 0xd830b0);
    RG_COOK_BIND(metadata_init, rg_object_init_fn, 0xd7c900, 0xd7c6b0);
    RG_COOK_BIND(metadata_clear, rg_object_clear_fn, 0xd7cf30, 0xd7cce0);
    RG_COOK_BIND(lexer_init, void *(*)(void *, unsigned), 0x1a007a0, 0x1a0e570);
    RG_COOK_BIND(lexer_clear, rg_object_clear_fn, 0x1a00930, 0x1a0e700);
    RG_COOK_BIND(lexer_flags, void (*)(void *, unsigned), 0x1a08c50, 0x1a16a10);
    RG_COOK_BIND(lexer_load, unsigned char (*)(void *, const void *, int, const char *), 0x1a061b0, 0x1a13f70);
    RG_COOK_BIND(lexer_expect, unsigned char (*)(void *, const char *), 0x1a057b0, 0x1a13580);
    RG_COOK_BIND(lexer_error, unsigned char (*)(void *), 0x1a05ee0, 0x1a13ca0);
    RG_COOK_BIND(lexer_warning, unsigned char (*)(void *), 0x1a05ef0, 0x1a13cb0);
    RG_COOK_BIND(mode, int (*)(void), 0x19eeb00, 0x19fc870);
    RG_COOK_BIND(permutation_name, void *(*)(void *, const char *, uint64_t), 0xd803f0, 0xd80040);
    RG_COOK_BIND(filesystem, void **, 0x3eb98a8, 0x55b7288);
    RG_COOK_BIND(heap, void **, 0x5894b38, 0x6f8e2c8);
    RG_COOK_BIND(sparse, const unsigned char *, 0x4c26269, 0);
    RG_COOK_BIND(aniso, const unsigned char *, 0x4c26266, 0);
    RG_COOK_BIND(feedback, const int *, 0x4142370, 0x58448c0);
    RG_COOK_BIND(validate, const int *, 0x413d110, 0x583a8f0);
    RG_COOK_BIND(flag_manager, void *, 0x42bbb20, 0x59bd810);
#undef RG_COOK_BIND
    g_cook_api.renderer = vk;
    g_cook_api.worker = (rg_shader_worker_fn)hook_prepare(worker, rg_shader_worker_hook, 20);
    if (!g_cook_api.worker || hook_commit((void *)g_cook_api.worker) != B2_PATCH_OK) return 0;
    g_cook_api.ready = 1; return 1;
}
static int rg_cook_string(const void *value, const char **text, size_t *length)
{
    const unsigned char *string = value; int size = *(const int *)(string + 8);
    const char *body = *(const char *const *)(string + 0x10);
    if (size < 0 || size > 16 * 1024 * 1024 || (size && (!body || memchr(body, 0, (size_t)size)))) return 0;
    *text = body ? body : ""; *length = (size_t)size; return 1;
}
static int rg_cook_text(rg_shader_file *text, const char *body, size_t length)
{ return length == 0 || rg_shader_write(text, body, length) == (long long)length; }
static int rg_cook_literal(rg_shader_file *text, const char *body)
{ return rg_cook_text(text, body, strlen(body)); }
/* The native permutation grammar consists of selected flag names and the
 * compiler's seven parsed sections plus three stage header/body pairs. It has
 * no inherited declaration token: those sections already reflect native source
 * inheritance. This writer owns only grammar formatting, not shader parsing. */
static int rg_cook_permutation(rg_cook_work *work, uint64_t selected, rg_shader_file *text)
{
    static const char *sections[] = {"parms","state","flags","inout","computelayout","shaderstorage","compileroptions"};
    static const char *stages[] = {"vp","fp","cp"};
    if (!rg_cook_literal(text, "{\n\tpermutations {\n")) return 0;
    const unsigned char *metadata = work->metadata;
    int count = *(const int *)(metadata + 0x9d8);
    const unsigned char *names = *(const unsigned char *const *)(metadata + 0x9d0);
    uint64_t options = *(const uint64_t *)(metadata + 0x1608);
    if (count < 0 || (count && !names)) return 0;
    for (unsigned group = 0; group < 2; group++) {
        int started = 0; uint64_t mask = group ? selected : options;
        for (int i = 0; i < count; i++) {
            const char *name; size_t length;
            if (!rg_cook_string(names + (size_t)i * 0x30, &name, &length) || !length || length >= 4096) return 0;
            void *flag = g_lookup(g_cook_api.flag_manager, name);
            if (!flag || *(void **)((unsigned char *)flag + 0x18) ||
                (*(unsigned char *)((unsigned char *)flag + 0x2c) & 7)) return 0;
            if (!(*(uint64_t *)((unsigned char *)flag + 0x58) & mask)) continue;
            if (!rg_cook_literal(text, started ? "+" : group ? "\t\tdefault " : "\t\toptions ") ||
                !rg_cook_text(text, name, length)) return 0;
            started = 1;
        }
        if (started && !rg_cook_literal(text, "\n")) return 0;
    }
    if (!rg_cook_literal(text, "\t}\n")) return 0;
    for (unsigned i = 0; i < 7; i++) {
        const char *body; size_t length;
        if (!rg_cook_string(work->sections[i], &body, &length)) return 0;
        if (length && (!rg_cook_literal(text, "\t") || !rg_cook_literal(text, sections[i]) ||
            !rg_cook_text(text, body, length) || !rg_cook_literal(text, "\n"))) return 0;
    }
    for (unsigned part = 0; part < 2; part++) for (unsigned i = 0; i < 3; i++) {
        const char *body; size_t length;
        if (!rg_cook_string(work->stages[i] + (part ? 0x6f8 : 0x6c8), &body, &length)) return 0;
        if (!length) continue;
        if (!rg_cook_literal(text, "\thlsl_") || !rg_cook_literal(text, stages[i]) ||
            !rg_cook_literal(text, part ? " " : "_header {") || !rg_cook_text(text, body, length) ||
            !rg_cook_literal(text, part ? "\n" : "}\n")) return 0;
    }
    return rg_cook_literal(text, "}\n") && text->length <= 16u * 1024u * 1024u;
}
/* Generated units are owned work items, not recursive native program loads.
 * One name must have one exact source. Repeated DAG dependencies share a work
 * item; cycle validation below runs before any certificate is admitted. */
static int rg_cook_enqueue(rg_cook_unit *parent, const char *name,
    const void *source, size_t length, int from_file)
{
    rg_cook_unit *unit = g_cook->units;
    for (; unit; unit = unit->next) if (!_stricmp(unit->name, name)) break;
    if (unit) {
        if (unit->length != length || memcmp(unit->source, source, length)) return 0;
    } else {
        unit = calloc(1, sizeof(*unit));
        if (unit) { unit->name = _strdup(name); unit->source = malloc(length ? length : 1); }
        if (!unit || !unit->name || !unit->source) {
            if (unit) { free(unit->name); free(unit->source); free(unit); } return 0;
        }
        memcpy(unit->source, source, length); unit->length = length; unit->from_file = from_file;
        if (g_cook->tail) g_cook->tail->next = unit; else g_cook->units = unit;
        g_cook->tail = unit;
    }
    if (parent) {
        for (rg_cook_edge *edge = parent->children; edge; edge = edge->next) if (edge->child == unit) return 1;
        rg_cook_edge *edge = malloc(sizeof(*edge)); if (!edge) return 0;
        edge->child = unit; edge->next = parent->children; parent->children = edge;
    }
    return 1;
}
static int rg_cook_acyclic(void)
{
    size_t count = 0, visited = 0; rg_cook_unit *head = NULL, *tail = NULL;
    for (rg_cook_unit *unit = g_cook->units; unit; unit = unit->next) {
        count++;
        for (rg_cook_edge *edge = unit->children; edge; edge = edge->next) {
            if (edge->child->incoming == SIZE_MAX) return 0;
            edge->child->incoming++;
        }
    }
    for (rg_cook_unit *unit = g_cook->units; unit; unit = unit->next) if (!unit->incoming) {
        if (tail) tail->queue_next = unit; else head = unit; tail = unit;
    }
    for (; head; head = head->queue_next) {
        visited++;
        for (rg_cook_edge *edge = head->children; edge; edge = edge->next) if (!--edge->child->incoming) {
            tail->queue_next = edge->child; tail = edge->child;
        }
    }
    return visited == count;
}
static int rg_cook_one(rg_cook_unit *unit)
{
    rg_cook_work *work = calloc(1, sizeof(*work)); int ok = 0;
    if (!work) { g_cook->failed = 1; return 0; }
    __try {
        for (; work->sections_ready < 7; work->sections_ready++)
            g_cook_api.string_init(work->sections[work->sections_ready]);
        for (; work->stages_ready < 3; work->stages_ready++)
            g_cook_api.stage_init(work->stages[work->stages_ready]);
        if (!rg_cook_source(unit->name, g_cook->settings, 0,
            unit->from_file ? NULL : unit->source, unit->length,
            unit->from_file ? unit->source : NULL, unit->length,
            work->metadata, &work->metadata_ready, work->sections, work->stages[0], work->stages[1], work->stages[2])) __leave;
        int count = *(int *)(work->metadata + 0x15f0);
        const uint64_t *masks = *(const uint64_t **)(work->metadata + 0x15e8);
        if (count < 0 || (count && !masks)) __leave;
        ok = 1;
        for (int i = 0; i < count && ok; i++) {
            unsigned char native_name[0x30] = {0}; int name_ready = 0;
            rg_shader_file text = {0};
            __try {
                ok = rg_cook_permutation(work, masks[i], &text);
                if (!ok) __leave;
                g_cook_api.permutation_name(native_name, unit->name, masks[i]); name_ready = 1;
                const char *child; size_t child_length;
                ok = rg_cook_string(native_name, &child, &child_length) && child_length > 0 && child_length < 4096 &&
                    rg_cook_enqueue(unit, child, text.body, text.length, 0);
            } __finally { if (name_ready) g_cook_api.string_clear(native_name); free(text.body); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    __try {
        if (work->metadata_ready) g_cook_api.metadata_clear(work->metadata);
        while (work->stages_ready) {
            unsigned char *stage = work->stages[--work->stages_ready];
            g_cook_api.stage_reset(stage); g_cook_api.stage_clear(stage);
        }
        while (work->sections_ready) g_cook_api.string_clear(work->sections[--work->sections_ready]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    free(work); if (!ok) g_cook->failed = 1; return ok;
}
int sh_resource_graph_native_prepare_program(void *manager, const char *name,
    const void *expected_source, size_t length,
    sh_resource_shader_source_reader original, void *original_context,
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity)
{
    const char *reason = "isolated shader cooking requires admitted native renderer ownership";
    int ok = 0; rg_cook_context cook = {0}; unsigned char after[12];
    if (error && capacity) *error = 0;
    if (!g_cook_api.ready) { reason = "native shader source-cook helpers are unavailable for this renderer build"; goto done; }
    if (g_cook || !g_shader_state || !admitted || !admitted(context) ||
        g_shader_state->thread != GetCurrentThreadId() || !g_shader_state->admitted(g_shader_state->context) ||
        !manager || !name || !*name || strnlen(name, 4096) == 4096 || !expected_source || !length) goto done;
    reason = "native shader source compiler is not at its production source boundary";
    __try {
        const char *family = *(const char **)((unsigned char *)manager + 8);
        if (!family || _stricmp(family, "renderprog") || g_cook_api.mode() < 2 || !rg_cook_settings(cook.settings)) __leave;
        cook.original = original; cook.original_context = original_context;
        g_cook = &cook;
        reason = "isolated native shader source cooking failed; original GPU objects remain intact";
        ok = rg_cook_enqueue(NULL, name, expected_source, length, 1);
        for (rg_cook_unit *unit = cook.units; unit && ok; unit = unit->next) ok = rg_cook_one(unit);
        ok = ok && rg_cook_acyclic() && !cook.failed && !g_shader_state->failed &&
            rg_cook_settings(after) && !memcmp(cook.settings, after, sizeof(after));
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    g_cook = NULL;
    while (cook.names) { rg_cook_name *item = cook.names; cook.names = item->next;
        free(item->vtable); free(item->name); free(item); }
    while (cook.units) {
        rg_cook_unit *unit = cook.units; cook.units = unit->next;
        while (unit->children) { rg_cook_edge *edge = unit->children; unit->children = edge->next; free(edge); }
        free(unit->name); free(unit->source); free(unit);
    }
done:
    if (!ok && g_shader_state) {
        g_shader_state->failed = 1; sh_package_shader_receipt_revoke_scope();
        if (!sh_resource_graph_native_shader_state_restore(g_shader_state, NULL, 0))
            reason = "native shader state restoration is still required before releasing renderer ownership";
    }
    if (!ok && error && capacity) snprintf(error, capacity, "%s: %s", reason, name ? name : "");
    return ok;
}

int sh_resource_graph_native_prepare_shader(void *resource, const char *family,
    const char *name, const void *expected_source, size_t length,
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity)
{
    char actual_type[128], actual_name[4096];
    unsigned char before[0x90], after[0x90]; size_t bytes = 0;
    int ok = 0, program = 0; uintptr_t native_error = 0;
    const char *reason = "shader preparation requires explicit main-thread provider admission";
    if (error && capacity) *error = 0;
    if (!family || !name || !expected_source || !length || !admitted ||
        !sh_resource_graph_native_ready() || !g_load || !admitted(context) || !g_shader_state ||
        g_shader_state->thread != GetCurrentThreadId() || !g_shader_state->admitted(g_shader_state->context)) goto done;
    if (!_stricmp(family, "renderparm")) bytes = 0x90;
    else if (!_stricmp(family, "renderprogflag")) bytes = 0x60;
    else if (!_stricmp(family, "renderprog") && g_shader_write && g_shader_compile) { bytes = 0x78; program = 1; }
    else { reason = "native source preparation requires a fixed shader ABI family"; goto done; }
    reason = "shader preparation object is not a ready registered identity";
    if (!rg_identity(resource, actual_type, actual_name) || _stricmp(family, actual_type) ||
        _stricmp(name, actual_name) || !rg_receipt_read(NULL, (uintptr_t)resource, before, bytes)) goto done;
    memcpy(&native_error, before + 0x18, sizeof(native_error));
    if (native_error || (before[0x2c] & 7)) goto done;
    if (bytes == 0x90) {
        uint32_t index; memcpy(&index, before + 0x68, 4);
        reason = "fixed shader source preparation requires an already assigned native parameter slot";
        if (index >= 0x93f) goto done;
    }
    if (program) {
        return sh_resource_graph_native_prepare_program(*(void **)(before + 0x10), name,
            expected_source, length, NULL, NULL, admitted, context, error, capacity);
    }
    if (!program && sh_package_shader_receipt_matches((uintptr_t)resource, family, name,
        expected_source, length, before, bytes)) { ok = 1; goto done; }
    reason = "implicit shader source has no verified native read branch";
    if (before[0x48]) goto done;
    reason = "native shader preparation failed; prior provider recovery is required";
    __try { rg_load_hook(resource); }
    __except (EXCEPTION_EXECUTE_HANDLER) { goto done; }
    if (g_shader_state && g_shader_state->failed) goto done;
    if (!rg_receipt_read(NULL, (uintptr_t)resource, after, bytes) ||
        !sh_package_shader_receipt_matches((uintptr_t)resource, family, name,
            expected_source, length, after, bytes)) goto done;
    /* GenericLoad changes use/pending metadata and production Read retires its
     * text. Fixed native identity, source record, defaults and index/mask must
     * remain exact; a semantic mutation belongs to the recovery transaction. */
    reason = "native shader preparation changed fixed ABI/defaults; prior provider recovery is required";
    if (memcmp(before, after, 0x24) || (!program && memcmp(before + 0x48, after + 0x48, bytes - 0x48))) {
        sh_package_shader_receipt_invalidate((uintptr_t)resource); goto done;
    }
    ok = 1;
done:
    if (!ok && g_shader_state) {
        g_shader_state->failed = 1;
        if (!sh_resource_graph_native_shader_state_restore(g_shader_state, NULL, 0))
            reason = "native shader state restoration is still required before releasing renderer ownership";
    }
    if (!ok && error && capacity) snprintf(error, capacity, "%s: %s", reason, name ? name : "");
    return ok;
}
