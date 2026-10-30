/* modswf.c -- see modswf.h. One idFileResource vtable slot, answered for
 * installed mods' cooked movies by the engine's own below-2 open path. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "modswf.h"
#ifndef SH_MODSWF_PURE
#include "backend_log.h"
#include "modserve.h"
#endif

/* ------------------------------------------------------------------ pure */

int sh_modswf_is_movie(const char *name)
{
    static const char prefix[] = "generated/swf/";
    size_t n, i;
    if (!name) return 0;
    n = strlen(name);
    if (n <= sizeof prefix - 1 + 5) return 0;
    for (i = 0; i < sizeof prefix - 1; i++) {
        char c = name[i];
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != prefix[i]) return 0;
    }
    return _stricmp(name + n - 5, ".bswf") == 0;
}

static uintptr_t rel32_target(const uint8_t *at)
{
    int32_t d;
    memcpy(&d, at + 1, sizeof d);
    return (uintptr_t)(at + 5) + (intptr_t)d;
}

int sh_modswf_decode_load(const uint8_t *load, sh_modswf_engine *out)
{
    /* Fixed bytes the replacement relies on, by offset. */
    static const uint8_t state_test[] = { 0x83, 0xF8, 0x02, 0x7C };                 /* +0x29 CMP EAX,2; JL */
    static const uint8_t mgr_call[]   = { 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x53, 0x08, 0xFF, 0x50, 0x68 }; /* +0x35 */
    static const uint8_t alloc_size[] = { 0xB9, 0x80, 0x01, 0x00, 0x00 };           /* +0xA9 MOV ECX,0x180 */
    static const uint8_t ctor_args[]  = { 0x48, 0x8B, 0x53, 0x08, 0x48, 0x8B, 0xC8 };/* +0xBD MOV RDX,[RBX+8]; MOV RCX,RAX */
    /* MOV [RSP+0x20],RBP; MOV R9B,1; MOV R8,[RBX+8]; LEA RDX,[RBX+0x38]; MOV RCX,RBX; CALL */
    static const uint8_t open_args[]  = { 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0xB1, 0x01, 0x4C, 0x8B, 0x43, 0x08,
                                          0x48, 0x8D, 0x53, 0x38, 0x48, 0x8B, 0xCB, 0xE8 };
    if (!load || !out) return 0;
    memset(out, 0, sizeof *out);
    __try {
        if (load[SH_MODSWF_OFF_STATE_CALL] != 0xE8 || load[SH_MODSWF_OFF_NEW_CALL] != 0xE8 ||
            load[SH_MODSWF_OFF_CTOR_CALL] != 0xE8 || load[SH_MODSWF_OFF_OPEN_CALL] != 0xE8) return 0;
        if (memcmp(load + 0x29, state_test, sizeof state_test) ||
            memcmp(load + 0x35, mgr_call, sizeof mgr_call) ||
            memcmp(load + 0xA9, alloc_size, sizeof alloc_size) ||
            memcmp(load + 0xBD, ctor_args, sizeof ctor_args) ||
            memcmp(load + SH_MODSWF_OFF_TAIL, open_args, sizeof open_args)) return 0;
        out->state      = rel32_target(load + SH_MODSWF_OFF_STATE_CALL);
        out->alloc      = rel32_target(load + SH_MODSWF_OFF_NEW_CALL);
        out->named_ctor = rel32_target(load + SH_MODSWF_OFF_CTOR_CALL);
        out->open_into  = rel32_target(load + SH_MODSWF_OFF_OPEN_CALL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(out, 0, sizeof *out);
        return 0;
    }
    return 1;
}

int sh_modswf_open_into_uses_provider(const uint8_t *open_into)
{
    static const uint8_t call148[] = { 0xFF, 0x90, 0x48, 0x01, 0x00, 0x00 };   /* CALL [RAX+0x148] */
    size_t i;
    if (!open_into) return 0;
    __try {
        for (i = 0; i + sizeof call148 <= 0x40; i++)
            if (!memcmp(open_into + i, call148, sizeof call148)) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}

void **sh_modswf_find_slot(const uint8_t *module_base, uint64_t value)
{
    void **found = NULL;
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS64 *nt;
        const IMAGE_SECTION_HEADER *sec;
        unsigned i;
        if (!module_base || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
        nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
        sec = IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            size_t span = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            const uint64_t *q, *end;
            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) ||
                (sec->Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE))) continue;
            if (sec->SizeOfRawData < span) span = sec->SizeOfRawData;   /* initialized bytes only */
            q = (const uint64_t *)(module_base + ((sec->VirtualAddress + 7u) & ~7u));
            end = (const uint64_t *)(module_base + sec->VirtualAddress + (span & ~(size_t)7));
            for (; q < end; q++) {
                if (*q != value) continue;
                if (found) return NULL;            /* not unique */
                found = (void **)q;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return found;
}

/* ------------------------------------------------------------------ the slot */

typedef int64_t (*load_fn)(void *resource, void *arg);
typedef int (*state_fn)(void);
typedef void *(*alloc_fn)(size_t size);
typedef void *(*named_ctor_fn)(void *mem, const char *name);
typedef uint8_t (*open_into_fn)(void *resource, void **slot, const char *name, char must_exist, void *arg);
typedef void (*dtor_fn)(void *self, int flags);

static sh_modswf_engine g_engine;
static load_fn g_original;
static void **g_slot;
static volatile LONG g_served, g_failed;

#ifdef SH_MODSWF_TESTING
static int (*g_test_lookup)(const char *request, const char **bundle_id);
#define swf_log(...) ((void)0)
#else
static void swf_log(const char *fmt, ...)
{
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}
#endif

static int swf_served(const char *name, const char **bundle_id)
{
#ifdef SH_MODSWF_TESTING
    return g_test_lookup ? g_test_lookup(name, bundle_id) : 0;
#else
    sh_modserve_hit hit;
    if (!sh_modserve_lookup(name, &hit)) return 0;
    if (bundle_id) *bundle_id = hit.bundle_id;
    return 1;
#endif
}

/* resource+0x08 is the request name, +0x38 the idFile the consumer reads. */
static int64_t swf_load(void *resource, void *arg)
{
    const char *name = NULL, *bundle = "";
    unsigned char **file_slot;
    void *mem, *file;
    uint8_t r;
    __try { name = *(const char *const *)((const uint8_t *)resource + 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { name = NULL; }
    if (!name || !sh_modswf_is_movie(name) || !swf_served(name, &bundle))
        return g_original(resource, arg);

    /* The engine's below-2 branch: a named memory file at +0x38, then the
     * open-into helper (file system +0x148, the provider). */
    mem = ((alloc_fn)g_engine.alloc)(0x180);
    file = mem ? ((named_ctor_fn)g_engine.named_ctor)(mem, name) : NULL;
    if (!file) return g_original(resource, arg);   /* the engine's own allocation failure path */
    file_slot = (unsigned char **)((uint8_t *)resource + 0x38);
    if (*(void **)file_slot != file) {
        void *old = *(void **)file_slot;
        if (old) ((dtor_fn)(*(void ***)old)[0])(old, 1);
        *(void **)file_slot = file;
    }
    r = ((open_into_fn)g_engine.open_into)(resource, (void **)file_slot, name, 1, arg);
    if (r == 0) {
        LONG n = InterlockedIncrement(&g_served);
        if (n <= 32)
            swf_log("MODS: swf: served %s (%s) at load state %d", name, bundle ? bundle : "",
                    ((state_fn)g_engine.state)());
        return 0;
    }
    if (InterlockedIncrement(&g_failed) <= 32)
        swf_log("MODS: swf: open of served %s failed (%u); engine load used", name, (unsigned)r);
    return g_original(resource, arg);
}

#ifdef SH_MODSWF_TESTING
void sh_modswf_test_bind(const sh_modswf_engine *engine, sh_modswf_test_load_fn original,
                         int (*lookup)(const char *request, const char **bundle_id))
{
    g_engine = *engine;
    g_original = (load_fn)original;
    g_test_lookup = lookup;
}
int64_t sh_modswf_test_load(void *resource, void *arg) { return swf_load(resource, arg); }
#endif

#ifndef SH_MODSWF_PURE
unsigned long sh_modswf_served_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_served, 0, 0);
}

int sh_modswf_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    const sig_result *load = NULL;
    sh_modswf_engine engine;
    void **slot;
    DWORD old;
    size_t i;
    if (g_slot) return 1;
    for (i = 0; i < count; i++)
        if (results[i].name && !strcmp(results[i].name, "FileResourceLoad")) load = &results[i];
    if (!load || load->status != SIG_OK || !load->addr) {
        swf_log("MODS: swf: movies are not served -- FileResourceLoad did not resolve cleanly");
        return 0;
    }
    if (!sh_modswf_decode_load((const uint8_t *)load->addr, &engine) ||
        !sh_modswf_open_into_uses_provider((const uint8_t *)engine.open_into)) {
        swf_log("MODS: swf: movies are not served -- the Load body or its open-into helper changed shape");
        return 0;
    }
    slot = sh_modswf_find_slot(module_base, (uint64_t)load->addr);
    if (!slot) {
        swf_log("MODS: swf: movies are not served -- no unique read-only vtable slot holds FileResourceLoad");
        return 0;
    }
    g_engine = engine;
    g_original = (load_fn)load->addr;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        swf_log("MODS: swf: movies are not served -- VirtualProtect on the vtable slot failed");
        return 0;
    }
    *slot = (void *)swf_load;
    VirtualProtect(slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    g_slot = slot;
    swf_log("MODS: swf: FileResource Load slot installed (slot rva 0x%llx, load rva 0x%llx, open-into rva 0x%llx)",
            (unsigned long long)((const uint8_t *)slot - module_base),
            (unsigned long long)(load->addr - (uintptr_t)module_base),
            (unsigned long long)(engine.open_into - (uintptr_t)module_base));
    return 1;
}
#endif
