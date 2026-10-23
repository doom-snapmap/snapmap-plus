#include "decl_script_native.h"
#include "package_sources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SH_DECL_SCRIPT_NATIVE_TESTING
typedef struct sh_decl_script_api {
    void *object;
    void *(__fastcall *exchange)(void *, void *);
    uintptr_t (__fastcall *names)(void *);
    uintptr_t (__fastcall *fields)(void *, const char *);
} sh_decl_script_api;
#endif
typedef struct sn_allocation { struct sn_allocation *next; } sn_allocation;
typedef struct sn_host {
    void **vtable;
    sn_allocation *allocations;
    sh_decl_script_api api;
    int failed;
} sn_host;

static void *__fastcall sn_allocate(sn_host *host, size_t size, int tag)
{
    sn_allocation *allocation;
    (void)tag;
    if (size > SIZE_MAX - sizeof(*allocation) || !(allocation = malloc(sizeof(*allocation) + size))) {
        host->failed = 1; return NULL;
    }
    allocation->next = host->allocations; host->allocations = allocation;
    return allocation + 1;
}
static void __fastcall sn_free(sn_host *host, void *memory, int tag)
{
    sn_allocation **link;
    (void)tag;
    if (!memory) return;
    for (link = &host->allocations; *link; link = &(*link)->next) if (*link + 1 == memory) {
        sn_allocation *allocation = *link;
        *link = allocation->next; free(allocation); return;
    }
    host->failed = 1;
}
static int __fastcall sn_tag(sn_host *host) { (void)host; return -1; }
static int sn_read(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    if (!address || length > UINTPTR_MAX - address) return 0;
    __try { memcpy(out, (const void *)address, length); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int sn_lookup(void *context, const char *name, uintptr_t *table)
{
    sn_host *host = context;
    if (host->failed) return -1;
    __try { *table = host->api.fields(host->api.object, name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *table = 0; host->failed = 1; }
    return host->failed ? -1 : *table ? 1 : 0;
}
static sh_decl_script_schema *sn_capture(sh_decl_script_api api, char *error, size_t capacity)
{
    void *methods[30] = {0}, *previous = NULL;
    sn_host host = {methods, NULL, api, 0};
    sh_decl_script_schema *schema = NULL;
    int exchanged = 0;
    if (error && capacity) *error = 0;
    if (!api.object || !api.exchange || !api.names || !api.fields) return NULL;
    methods[0xd8 / 8] = (void *)sn_allocate;
    methods[0xe0 / 8] = (void *)sn_free;
    methods[0xe8 / 8] = (void *)sn_tag;
    __try {
        __try {
            previous = api.exchange(api.object, &host); exchanged = 1;
            schema = sh_decl_script_schema_capture((sh_decl_script_source){&host, sn_read,
                api.names(api.object), sn_lookup}, error, capacity);
        } __finally {
            if (exchanged) api.exchange(api.object, previous);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { host.failed = 1; }
    if (host.allocations) host.failed = 1;
    while (host.allocations) {
        sn_allocation *allocation = host.allocations;
        host.allocations = allocation->next; free(allocation);
    }
    if (host.failed) {
        sh_decl_script_schema_free(schema); schema = NULL;
        if (error && capacity) snprintf(error, capacity, "script metadata capture or context restoration failed");
    }
    return schema;
}

static sh_package_file *sn_supported_path(const wchar_t *path, char *error, size_t capacity)
{
    /* This contract is for the DOOM 2016 module shipped with both renderers.
     * A different module may execute more than metadata/string operations and
     * must be investigated before lending it this limited allocation host. */
    static const unsigned char expected[32] = {
        0xbc,0x25,0xf0,0x3b,0x4b,0x2c,0xf2,0xfb,0x01,0x23,0x01,0x7b,0x9e,0x1b,0x4e,0xe9,
        0x84,0x8f,0x93,0xa6,0xc7,0xbe,0xf0,0x24,0xc2,0xc5,0x9d,0x4a,0xf7,0xbf,0x80,0x56};
    char *utf8 = NULL;
    int size;
    sh_package_file *file = NULL;
    sh_package_file_identity identity;
    int supported = 0;
    size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL);
    if (!size || !(utf8 = malloc((size_t)size)) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8, size, NULL, NULL)) goto done;
    file = sh_package_file_capture(utf8, error, capacity);
    supported = file && sh_package_file_get_identity(file, &identity) && !memcmp(identity.digest, expected, 32);
done:
    if (!supported) { sh_package_file_release(file); file = NULL; }
    free(utf8); return file;
}
sh_decl_script_schema *sh_decl_script_native_capture(HMODULE module, char *error, size_t capacity)
{
    wchar_t path[32768];
    DWORD length;
    sh_package_file *file = NULL;
    sh_decl_script_schema *schema = NULL;
    void *(__fastcall *get_api)(int);
    sh_decl_script_api api = {0};
    if (error && capacity) *error = 0;
    if (!module || !(length = GetModuleFileNameW(module, path, 32768)) || length >= 32768) goto done;
    if (!(file = sn_supported_path(path, error, capacity))) goto done;
    get_api = (void *(__fastcall *)(int))GetProcAddress(module, "GetSuperScriptAPI");
    if (!get_api) goto done;
    __try {
        void **vtable;
        api.object = get_api(1);
        if (api.object) {
            vtable = *(void ***)api.object;
            api.exchange = (void *(__fastcall *)(void *, void *))vtable[1];
            api.names = (uintptr_t (__fastcall *)(void *))vtable[5];
            api.fields = (uintptr_t (__fastcall *)(void *, const char *))vtable[8];
            schema = sn_capture(api, error, capacity);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { schema = NULL; }
done:
    sh_package_file_release(file);
    if (!schema && error && capacity && !*error)
        snprintf(error, capacity, "loaded script module metadata contract is unavailable");
    return schema;
}
sh_decl_script_schema *sh_decl_script_native_capture_default(char *error, size_t capacity)
{
    HMODULE module = NULL;
    sh_decl_script_schema *schema = NULL;
    wchar_t path[32768], *slash;
    DWORD length;
    sh_package_file *file = NULL;
    if (error && capacity) *error = 0;
    if (!GetModuleHandleExW(0, L"superscriptx64.dll", &module)) {
        length = GetModuleFileNameW(NULL, path, 32768);
        if (!length || length >= 32768 || !(slash = wcsrchr(path, L'\\')) ||
            wcscpy_s(slash + 1, 32768 - (size_t)(slash + 1 - path), L"superscriptx64.dll") ||
            !(file = sn_supported_path(path, error, capacity))) goto done;
        module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (module) schema = sh_decl_script_native_capture(module, error, capacity);
done:
    if (module) FreeLibrary(module);
    sh_package_file_release(file);
    if (!schema && error && capacity && !*error)
        snprintf(error, capacity, "game script metadata could not be captured");
    return schema;
}
#ifdef SH_DECL_SCRIPT_NATIVE_TESTING
sh_decl_script_schema *sh_decl_script_native_test_capture(sh_decl_script_api api, char *error, size_t capacity)
{ return sn_capture(api, error, capacity); }
#endif
