/* Serve the file shadow's claims to the resource manager's file IO once a map
 * is running (R-309).
 *
 * idFileResource and idJsonResource Load branch on the map-load state. Below
 * 2 they build a named idFile_Memory and fill it through the provider's open
 * slot (+0xf8), which the shadow replaces. From 2 they ask the resource
 * manager's file open (vtable +0x68), which reads a loose file only when
 * resource_loadLooseAssets is set and otherwise only the resource containers;
 * idBackgroundLoader::GetFile and the manager's own file helper fall back to
 * the same method. A shadow-owned name first loaded then became an empty
 * memory file and its consumer failed silently.
 *
 * The swapped method claims the name exactly as an ordinary shadow open would.
 * A claimed name is served as the object the engine's own loose branch returns:
 * operator new, the named idFile_Memory constructor, then the engine's
 * open-into helper, which receives the claimed stream through the provider's
 * open (a same-thread handoff) and copies it into an engine allocation.
 * Provenance and receipts are therefore recorded as below state 2. An owned
 * name the shadow cannot supply stays unavailable instead of reaching a
 * container payload; every other name keeps the native method.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "backend_log.h"
#include "overrides.h"
#include "resource_file_visibility.h"

#define RFV_OPEN_SLOT 0x68u
/* Offsets inside the clean matches of the signatures in signatures.c. */
#define RFV_CTOR_VTABLE_LEA  0x1bu   /* LEA RAX,[rip+disp32]; MOV [RCX],RAX */
#define RFV_LOAD_OPEN_CALL   0x3cu   /* CALL [RAX+0x68], Load's state-2 branch */
#define RFV_LOAD_NEW_SIZE    0xa8u   /* MOV ECX,0x180 before the call below */
#define RFV_LOAD_NEW_CALL    0xadu   /* CALL operator new */
#define RFV_LOAD_MEMORY_CALL 0xc3u   /* CALL idFile_Memory(name) */
#define RFV_LOAD_FILL_CALL   0xfeu   /* CALL the open-into helper: the match ends at its opcode */
#define RFV_MEMORY_FILE_BYTES 0x180u

typedef void *(*rfv_open_fn)(void *manager, const char *name);
typedef void *(*rfv_new_fn)(size_t bytes);
typedef void (*rfv_delete_fn)(void *block, size_t bytes);
typedef void *(*rfv_memory_fn)(void *file, const char *name);
/* (unused owner, idFile **file, name, required): 0 on success. */
typedef unsigned char (*rfv_fill_fn)(void *owner, void **file, const char *name, unsigned char required);

static rfv_open_fn g_open;
static rfv_new_fn g_new;
static rfv_delete_fn g_delete;
static rfv_memory_fn g_memory;
static rfv_fill_fn g_fill;
static void **g_slot;
static volatile LONG g_served, g_failures;

static void rfv_release(void *file)
{
    __try { ((void (*)(void *, int))(*(void ***)file)[0])(file, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static void rfv_report(const char *what, const char *name)
{
    char line[512];
    InterlockedIncrement(&g_failures);
    _snprintf_s(line, sizeof(line), _TRUNCATE, "resource-file visibility: %s for '%s'; the owned file stays unavailable",
                what, name ? name : "");
    backend_log(line);
}

static void *rfv_open_hook(void *manager, const char *name)
{
    void *stream = NULL, *memory = NULL, *file = NULL, *left;
    unsigned char failed = 1;
    const char *why = "no native memory file";
    if (!sh_overrides_claim_open(name, &stream)) return g_open(manager, name);
    if (!stream) return NULL;   /* an owned name never resolves to a container payload */
    sh_overrides_pending_open(name, stream);
    /* The new-expression of idFileResource Load. If the constructor raises,
     * its object never existed: release only the raw block, with the sized
     * delete that expression's own unwind uses, as the engine would. */
    __try {
        memory = g_new(RFV_MEMORY_FILE_BYTES);
        if (memory) file = g_memory(memory, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (memory && !file) g_delete(memory, RFV_MEMORY_FILE_BYTES);
        file = NULL; why = memory ? "native memory-file constructor faulted" : "native allocation faulted";
    }
    if (file) {
        why = "native fill failed";
        __try { failed = g_fill(NULL, &file, name, 1); }
        __except (EXCEPTION_EXECUTE_HANDLER) { failed = 1; why = "native fill faulted"; }
    }
    left = sh_overrides_pending_take();   /* still pending: the fill never opened it */
    if (left) sh_overrides_stream_close(left);
    if (failed) {
        if (file) rfv_release(file);       /* constructed: its own deleting destructor */
        rfv_report(why, name);
        return NULL;
    }
    if (InterlockedIncrement(&g_served) == 1) {
        char line[512];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "resource-file visibility: first running-state file served through the provider: '%s'", name);
        backend_log(line);
    }
    return file;
}

static const uint8_t *rfv_clean(const sig_result *results, size_t count, const char *name)
{
    for (size_t i = 0; results && i < count; i++)
        if (results[i].name && !strcmp(results[i].name, name))
            return results[i].status == SIG_OK ? (const uint8_t *)results[i].addr : NULL;
    return NULL;
}

/* Section characteristics of an image address, from the image's own headers. */
static int rfv_section(const uint8_t *module_base, const void *address, DWORD *characteristics)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS64 *nt;
        const IMAGE_SECTION_HEADER *section;
        uintptr_t rva;
        if (!module_base || (uintptr_t)address < (uintptr_t)module_base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        rva = (uintptr_t)address - (uintptr_t)module_base;
        section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            uintptr_t start = section->VirtualAddress;
            uintptr_t span = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
            if (rva >= start && rva < start + span) { *characteristics = section->Characteristics; return 1; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}
static int rfv_code(const uint8_t *module_base, const void *address)
{
    DWORD c = 0;
    return rfv_section(module_base, address, &c) && (c & IMAGE_SCN_MEM_EXECUTE);
}
static int rfv_rodata(const uint8_t *module_base, const void *address)
{
    DWORD c = 0;
    return rfv_section(module_base, address, &c) && (c & IMAGE_SCN_MEM_READ) &&
           !(c & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE));
}
/* A loaded image holds relocated pointers; a raw mapping of the file (the
 * two-image tests) keeps preferred-base ones, mapped back here. Anything else,
 * such as another component's method, is NULL. */
static const void *rfv_image_pointer(const uint8_t *module_base, const void *value)
{
    __try {
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)
            (module_base + ((const IMAGE_DOS_HEADER *)module_base)->e_lfanew);
        uintptr_t size = nt->OptionalHeader.SizeOfImage, preferred = (uintptr_t)nt->OptionalHeader.ImageBase;
        if ((uintptr_t)value - (uintptr_t)module_base < size) return value;
        if ((uintptr_t)value - preferred < size) return module_base + ((uintptr_t)value - preferred);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return NULL;
}
static const uint8_t *rfv_call(const uint8_t *call)
{
    int32_t displacement;
    if (call[0] != 0xe8) return NULL;
    memcpy(&displacement, call + 1, sizeof(displacement));
    return call + 5 + displacement;
}

/* The engine's own unwind for FileResourceLoad's new-expression (R-309):
 * its RUNTIME_FUNCTION and __CxxFrameHandler3 FuncInfo give the state that
 * covers the named constructor call, and that state's action funclet frees
 * the block Load stored at [rsp+0x60] with operator delete(block, 0x180).
 * Read from the image's exception directory; NULL unless every step matches. */
static const uint8_t *rfv_unwind_delete(const uint8_t *module_base, const uint8_t *load)
{
    static const uint8_t funclet[] = {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea,
                                      0xba, 0x80, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x4d, 0x60, 0xe8};
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)
        (module_base + ((const IMAGE_DOS_HEADER *)module_base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *directory = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const RUNTIME_FUNCTION *functions = (const RUNTIME_FUNCTION *)(module_base + directory->VirtualAddress);
    uint32_t rva = (uint32_t)(load - module_base), ip = rva + RFV_LOAD_MEMORY_CALL + 5, info = 0;
    const uint8_t *unwind, *func_info, *action;
    uint32_t magic, map_count;
    int32_t state = -1, max_state;
    for (size_t i = 0; i < directory->Size / sizeof(*functions); i++)
        if (functions[i].BeginAddress == rva) { info = functions[i].UnwindData; break; }
    if (!info) return NULL;
    unwind = module_base + info;
    /* Version 1 with a language handler (an unwind-only frame registers it as
     * the termination handler) and no chaining. */
    if ((unwind[0] & 7) != 1 || !(unwind[0] >> 3 & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) ||
        (unwind[0] >> 3 & UNW_FLAG_CHAININFO)) return NULL;
    func_info = module_base + *(const uint32_t *)(unwind + 4 + 2 * ((unwind[2] + 1) & ~1) + 4);
    magic = *(const uint32_t *)func_info;
    if (magic < 0x19930520u || magic > 0x19930522u) return NULL;
    max_state = *(const int32_t *)(func_info + 4);
    map_count = *(const uint32_t *)(func_info + 20);
    for (uint32_t i = 0; i < map_count; i++) {
        const uint8_t *entry = module_base + *(const uint32_t *)(func_info + 24) + 8u * i;
        if (*(const uint32_t *)entry > ip) break;
        state = *(const int32_t *)(entry + 4);
    }
    if (state < 0 || state >= max_state) return NULL;
    action = module_base + *(const uint32_t *)(module_base + *(const uint32_t *)(func_info + 8) + 8u * state + 4);
    if (memcmp(action, funclet, sizeof(funclet))) return NULL;
    return rfv_call(action + sizeof(funclet) - 1);
}

int sh_resource_file_visibility_decode(const sig_result *results, size_t count,
                                       const uint8_t *module_base,
                                       sh_resource_file_visibility_native *out)
{
    const uint8_t *ctor = rfv_clean(results, count, "ResourceManagerConstruct");
    const uint8_t *open = rfv_clean(results, count, "ResourceManagerOpenFile");
    const uint8_t *load = rfv_clean(results, count, "FileResourceLoad");
    sh_resource_file_visibility_native native = {0};
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!ctor || !open || !load) return 0;
    __try {
        const uint8_t *lea = ctor + RFV_CTOR_VTABLE_LEA;
        const uint8_t *vtable;
        int32_t displacement;
        if (lea[0] != 0x48 || lea[1] != 0x8d || lea[2] != 0x05 || lea[7] != 0x48 || lea[8] != 0x89 || lea[9] != 0x01)
            return 0;
        memcpy(&displacement, lea + 3, sizeof(displacement));
        vtable = lea + 7 + displacement;
        if (!rfv_rodata(module_base, vtable)) return 0;
        native.slot = (void **)(vtable + RFV_OPEN_SLOT);
        native.open = (void *)rfv_image_pointer(module_base, *native.slot);
        /* The slot must hold the method this module replaces. */
        if (native.open != (void *)open) return 0;
        if (memcmp(load + RFV_LOAD_OPEN_CALL, "\xff\x50\x68", 3) ||
            memcmp(load + RFV_LOAD_NEW_SIZE, "\xb9\x80\x01\x00\x00", 5)) return 0;
        native.operator_new = (void *)rfv_call(load + RFV_LOAD_NEW_CALL);
        native.memory_file = (void *)rfv_call(load + RFV_LOAD_MEMORY_CALL);
        native.fill = (void *)rfv_call(load + RFV_LOAD_FILL_CALL);
        native.operator_delete = (void *)rfv_unwind_delete(module_base, load);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!rfv_code(module_base, native.operator_new) || !rfv_code(module_base, native.memory_file) ||
        !rfv_code(module_base, native.fill) || !rfv_code(module_base, native.operator_delete)) return 0;
    *out = native;
    return 1;
}

int sh_resource_file_visibility_install(const sig_result *results, size_t count,
                                        const uint8_t *module_base)
{
    sh_resource_file_visibility_native native;
    char line[512];
    DWORD old;
    if (g_slot) return 1;
    if (!sh_resource_file_visibility_decode(results, count, module_base, &native)) {
        backend_log("resource-file visibility REFUSED: the resource manager's file open, its vtable slot or the "
                    "file-resource helpers did not decode from clean signatures; running-state file and JSON "
                    "resources keep the native route");
        return 0;
    }
    g_open = (rfv_open_fn)native.open;
    g_new = (rfv_new_fn)native.operator_new;
    g_delete = (rfv_delete_fn)native.operator_delete;
    g_memory = (rfv_memory_fn)native.memory_file;
    g_fill = (rfv_fill_fn)native.fill;
    if (!VirtualProtect(native.slot, sizeof(void *), PAGE_READWRITE, &old)) {
        backend_log("resource-file visibility REFUSED: VirtualProtect on the file-open slot failed");
        return 0;
    }
    /* One aligned pointer store, published after every native it calls. */
    InterlockedExchangePointer(native.slot, (void *)rfv_open_hook);
    VirtualProtect(native.slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), native.slot, sizeof(void *));
    g_slot = native.slot;
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "resource-file visibility installed: shadow-owned names answer the resource manager's file open "
                "(slot rva=0x%llx, method rva=0x%llx)",
                (unsigned long long)((const uint8_t *)native.slot - module_base),
                (unsigned long long)((const uint8_t *)native.open - module_base));
    backend_log(line);
    return 1;
}

int sh_resource_file_visibility_uninstall(void)
{
    DWORD old;
    if (!g_slot || !g_open) return 0;
    if (!VirtualProtect(g_slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer(g_slot, (void *)g_open);
    VirtualProtect(g_slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_slot, sizeof(void *));
    g_slot = NULL;
    backend_log("resource-file visibility uninstalled");
    return 1;
}

unsigned long sh_resource_file_visibility_served(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_served, 0, 0);
}
int sh_resource_file_visibility_dispatch_ready(const sh_resource_file_visibility_native *expected)
{
    int ready=0;DWORD error=GetLastError();
    __try {
        ready=expected && expected->slot && g_slot==expected->slot && *g_slot==(void *)rfv_open_hook &&
            expected->open && (void *)g_open==expected->open &&
            expected->operator_new && (void *)g_new==expected->operator_new &&
            expected->operator_delete && (void *)g_delete==expected->operator_delete &&
            expected->memory_file && (void *)g_memory==expected->memory_file &&
            expected->fill && (void *)g_fill==expected->fill;
    }__except(EXCEPTION_EXECUTE_HANDLER){ready=0;}
    SetLastError(error);return ready;
}

#ifdef SH_RESOURCE_FILE_VISIBILITY_TESTING
void *sh_resource_file_visibility_test_bind(void *open, void *operator_new, void *operator_delete,
                                            void *memory_file, void *fill)
{
    g_open = (rfv_open_fn)open;
    g_new = (rfv_new_fn)operator_new;
    g_delete = (rfv_delete_fn)operator_delete;
    g_memory = (rfv_memory_fn)memory_file;
    g_fill = (rfv_fill_fn)fill;
    g_served = g_failures = 0;
    return (void *)rfv_open_hook;
}
#endif
