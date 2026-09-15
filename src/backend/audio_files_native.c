#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "audio_files_native.h"
#include "audio_files.h"
#include "package_runtime.h"
#include "resource_graph.h"
#include "backend_log.h"

typedef int (*afn_id_fn)(void *, uint32_t, int, const void *, unsigned char *, sh_audio_file_descriptor *);
typedef int (*afn_name_fn)(void *, const wchar_t *, int, const void *, unsigned char *, sh_audio_file_descriptor *);
static afn_id_fn g_afn_id;
static afn_name_fn g_afn_name;
static void *g_afn_object;
static const wchar_t *g_afn_language;
static volatile LONG g_afn_ready, g_afn_attempted;

int sh_audio_files_native_language(wchar_t *out, size_t capacity)
{
    size_t length;
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!InterlockedCompareExchange(&g_afn_ready, 0, 0)) return 0;
    __try {
        if (!g_afn_language || (length = wcsnlen_s(g_afn_language, 260)) == 260 || length >= capacity) return 0;
        memcpy(out, g_afn_language, (length+1)*sizeof(*out)); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 0; }
}

static int afn_provider(void *context, const char *path, FILE **stream, uint64_t *length)
{
    int result;
    (void)context;
    result = sh_package_runtime_open_file(path, stream, length);
    if (result > 0) sh_resource_graph_file(path);
    return result;
}

/* Prefix buffers are the engine's three 260-wide-character arrays, not author
 * path limits. The language buffer is copied before provider lookup. */
static int afn_request(void *self, const void *flags, unsigned char copy[25],
    wchar_t bank[260], wchar_t media[260], wchar_t language[260], uint32_t *device)
{
    __try {
        const wchar_t *b = (const wchar_t *)((const unsigned char *)self+0x220);
        const wchar_t *m = (const wchar_t *)((const unsigned char *)self+0x428);
        size_t bn = wcsnlen_s(b, 260), mn = wcsnlen_s(m, 260), ln;
        if (bn == 260 || mn == 260 || !g_afn_language) return 0;
        ln = wcsnlen_s(g_afn_language, 260);
        if (ln == 260) return 0;
        memcpy(bank, b, (bn+1)*sizeof(wchar_t));
        memcpy(media, m, (mn+1)*sizeof(wchar_t));
        memcpy(language, g_afn_language, (ln+1)*sizeof(wchar_t));
        if (flags) memcpy(copy, flags, 25);
        *device = *(const uint32_t *)((const unsigned char *)self+0x630);
        return *device != UINT32_MAX;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int sh_audio_files_native_bank_paths(const char *filename, char **localized, char **fallback)
{
    wchar_t name[260], bank[260], media[260], language[260];
    unsigned char flags[25] = {0};
    uint32_t device;
    if (!localized || !fallback) return 0;
    *localized = NULL; *fallback = NULL;
    if (!filename || !sh_audio_files_native_ready() ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, name, 260) ||
        !afn_request(g_afn_object, NULL, flags, bank, media, language, &device)) return 0;
    flags[24] = 1;
    if (sh_audio_file_path(name, 0, 0, flags, bank, media, language, localized) != 1) return 0;
    flags[24] = 0;
    if (sh_audio_file_path(name, 0, 0, flags, bank, media, language, fallback) != 1) {
        free(*localized); *localized = NULL; return 0;
    }
    return 1;
}

int sh_audio_files_native_bank_prefix(char *out, size_t capacity)
{
    wchar_t bank[260], media[260], language[260];
    unsigned char flags[25] = {0};
    uint32_t device;
    int length;
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!sh_audio_files_native_ready() ||
        !afn_request(g_afn_object, NULL, flags, bank, media, language, &device)) return 0;
    if (!bank[0]) return 1;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, bank, -1, out, (int)capacity, NULL, NULL);
    if (!length) { out[0] = 0; return 0; }
    for (char *p = out; *p; ++p) {
        if (*p == '\\') *p = '/';
        else if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    }
    return 1;
}

static int afn_open(void *self, const wchar_t *name, uint32_t id, int by_id,
    int mode, const void *flags, unsigned char *sync, sh_audio_file_descriptor *out)
{
    wchar_t bank[260], media[260], language[260];
    unsigned char copied_flags[25];
    uint32_t device;
    char *path = NULL;
    int result;
    if (!InterlockedCompareExchange(&g_afn_ready, 0, 0) || self != g_afn_object || mode != 0)
        return 0;
    if (!afn_request(self, flags, copied_flags, bank, media, language, &device)) return 0;
    result = sh_audio_file_path(name, id, by_id, flags ? copied_flags : NULL,
        bank, media, language, &path);
    if (result > 0) {
        result = sh_audio_file_open(path, device, afn_provider, NULL, sync, out);
        free(path);
    }
    if (result < 0) backend_log("audio files: active package stream could not be opened");
    return result;
}

static int afn_open_id(void *self, uint32_t id, int mode, const void *flags,
    unsigned char *sync, sh_audio_file_descriptor *out)
{
    int result = afn_open(self, NULL, id, 1, mode, flags, sync, out);
    return result ? (result > 0 ? 1 : 2) : g_afn_id(self, id, mode, flags, sync, out);
}
static int afn_open_name(void *self, const wchar_t *name, int mode, const void *flags,
    unsigned char *sync, sh_audio_file_descriptor *out)
{
    int result = afn_open(self, name, 0, 0, mode, flags, sync, out);
    return result ? (result > 0 ? 1 : 2) : g_afn_name(self, name, mode, flags, sync, out);
}

static const unsigned char *afn_clean(const sig_result *results, size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i)
        if (results[i].name && !strcmp(results[i].name, name) && results[i].status == SIG_OK)
            return (const unsigned char *)results[i].addr;
    return NULL;
}
static int afn_section(const unsigned char *image, const void *address, size_t length,
    DWORD required, DWORD forbidden)
{
    uintptr_t rva;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *section;
    if (!image || !address || (uintptr_t)address < (uintptr_t)image) return 0;
    rva = (uintptr_t)address-(uintptr_t)image;
    __try {
        dos = (const IMAGE_DOS_HEADER *)image;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return 0;
        nt = (const IMAGE_NT_HEADERS64 *)(image+dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            rva >= nt->OptionalHeader.SizeOfImage || length > nt->OptionalHeader.SizeOfImage-rva) return 0;
        section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            size_t span = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
            if (rva < section->VirtualAddress || rva-section->VirtualAddress >= span ||
                length > span-(rva-section->VirtualAddress)) continue;
            return (section->Characteristics & required) == required &&
                !(section->Characteristics & forbidden);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}
static const unsigned char *afn_lea(const unsigned char *p, unsigned char reg)
{
    int32_t relative;
    if (p[0] != 0x48 || p[1] != 0x8d || p[2] != reg) return NULL;
    memcpy(&relative, p+3, 4);
    return p+7+relative;
}

int sh_audio_files_native_install(const sig_result *results, size_t count,
    const unsigned char *image)
{
    const unsigned char *ctor, *language, *id, *name, *object, *locale;
    void **table;
    DWORD old, ignored;
    int applied = 0, restored;
    if (InterlockedCompareExchange(&g_afn_attempted, 1, 0)) return sh_audio_files_native_ready();
    ctor = afn_clean(results, count, "AudioFileResolverInit");
    language = afn_clean(results, count, "AudioFileLanguage");
    id = afn_clean(results, count, "AudioFileOpenId");
    name = afn_clean(results, count, "AudioFileOpenName");
    if (!afn_section(image, ctor, 0x18, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE) ||
        !afn_section(image, language, 8, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE) ||
        !afn_section(image, id, 1, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE) ||
        !afn_section(image, name, 1, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE)) goto failed;
    __try {
        object = afn_lea(ctor+4, 0x0d);
        table = (void **)afn_lea(ctor+0x10, 0x05);
        locale = afn_lea(language, 0x05);
        if (((uintptr_t)table & (sizeof(void *)-1)) ||
            !afn_section(image, table, 24, IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE) ||
            !afn_section(image, object, 0x658, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) ||
            !afn_section(image, locale, 2, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) ||
            table[1] != id || table[2] != name) goto failed;
        g_afn_id = (afn_id_fn)id;
        g_afn_name = (afn_name_fn)name;
        g_afn_object = (void *)object;
        g_afn_language = (const wchar_t *)locale;
        if (!VirtualProtect(table+1, 2*sizeof(void *), PAGE_READWRITE, &old)) goto failed;
        /* Pointer publication cannot expose a torn instruction or pointer. A
         * partial installation stays native while ready is false. */
        if (InterlockedCompareExchangePointer(table+1, (void *)afn_open_id, (void *)id) == id) {
            if (InterlockedCompareExchangePointer(table+2, (void *)afn_open_name, (void *)name) == name) applied = 1;
            else InterlockedCompareExchangePointer(table+1, (void *)id, (void *)afn_open_id);
        }
        restored = VirtualProtect(table+1, 2*sizeof(void *), old, &ignored) != 0;
        if (!applied || !restored) goto failed;
    } __except (EXCEPTION_EXECUTE_HANDLER) { goto failed; }
    InterlockedExchange(&g_afn_ready, 1);
    backend_log("audio files: native bank/media opens resolve immutable package files");
    return 1;
failed:
    backend_log("audio files: native resolver binding unavailable; existing routes retained");
    return 0;
}

int sh_audio_files_native_ready(void)
{ return InterlockedCompareExchange(&g_afn_ready, 0, 0) != 0; }
