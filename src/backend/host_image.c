/* host_image.c -- see host_image.h. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include "host_image.h"

/* Names admit portable signature scanning; they do not authorize raw RVAs. */
static const char *const k_doom_names[] = {
    "DOOMx64vk.exe",   /* Vulkan  */
    "DOOMx64.exe",     /* OpenGL  */
};

static const uint8_t *g_base = NULL;
static size_t         g_size = 0;
static char           g_name[64] = { 0 };
static INIT_ONCE      g_resolved = INIT_ONCE_STATIC_INIT;
static INIT_ONCE      g_fingerprinted = INIT_ONCE_STATIC_INIT;
static int            g_pinned = 0;
static const char * volatile g_renderer = NULL;   /* "vulkan"/"opengl" once definite; NULL until then */

static const char *basename_of(const char *path)
{
    const char *p = path, *last = path;
    for (; *p; p++) {
        if (*p == '\\' || *p == '/') last = p + 1;
    }
    return last;
}

static BOOL CALLBACK resolve_host(PINIT_ONCE once, PVOID arg, PVOID *context)
{
    char path[MAX_PATH];
    DWORD n;
    const char *leaf;
    size_t i;
    HMODULE h;

    (void)once; (void)arg; (void)context;

    /* The process image is the host; its basename gates supported executable names. */
    h = GetModuleHandleA(NULL);
    if (h == NULL)
        return TRUE;

    n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (n == 0 || n >= sizeof path)
        return TRUE;

    leaf = basename_of(path);
    for (i = 0; i < sizeof k_doom_names / sizeof k_doom_names[0]; i++) {
        if (_stricmp(leaf, k_doom_names[i]) == 0) {
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
            IMAGE_NT_HEADERS *nt;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return TRUE;
            nt = (IMAGE_NT_HEADERS *)((uint8_t *)h + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return TRUE;
            g_size = (size_t)nt->OptionalHeader.SizeOfImage;
            g_base = (const uint8_t *)h;
            strncpy_s(g_name, sizeof g_name, leaf, _TRUNCATE);
            return TRUE;
        }
    }
    return TRUE;
}

static void resolve_once(void)
{
    InitOnceExecuteOnce(&g_resolved, resolve_host, NULL, NULL);
}

/* Hash the file, not loaded code: relocations and installed detours must not
 * change build identity. Opening without write sharing pins the bytes we read. */
static int file_is_pinned(const wchar_t *path)
{
    static const unsigned char hashes[][32] = {
        {0x13,0x97,0x63,0xe9,0x4f,0x1a,0x75,0xb5,0x31,0x01,0x79,0xf9,0xee,0xeb,0x8a,0x94,
         0x9a,0x1f,0x53,0xc4,0x9a,0xcb,0xc7,0x22,0xfc,0xfc,0x5d,0xfe,0x7b,0xb6,0xd3,0x23},
        {0x5a,0xd2,0x54,0x8c,0xea,0xb3,0xdf,0xa2,0x7f,0x27,0x1e,0x38,0x12,0x22,0xca,0x4a,
         0x84,0xdc,0xac,0x7c,0xb8,0x3c,0xce,0x94,0x46,0x44,0x4c,0x30,0xc8,0x30,0x93,0x67}
    };
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char bytes[64 * 1024], digest[32];
    DWORD count;
    int matched = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    for (;;) {
        if (!ReadFile(file, bytes, sizeof bytes, &count, NULL)) goto done;
        if (!count) break;
        if (BCryptHashData(hash, bytes, count, 0) < 0) goto done;
    }
    if (BCryptFinishHash(hash, digest, sizeof digest, 0) < 0) goto done;
    for (size_t i = 0; i < sizeof hashes / sizeof hashes[0]; i++)
        if (!memcmp(digest, hashes[i], sizeof digest)) matched = 1;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return matched;
}

static BOOL CALLBACK fingerprint_host(PINIT_ONCE once, PVOID arg, PVOID *context)
{
    wchar_t path[32768];
    DWORD count;
    (void)once; (void)arg; (void)context;
    resolve_once();
    if (!g_base) return TRUE;
    count = GetModuleFileNameW(NULL, path, (DWORD)(sizeof path / sizeof path[0]));
    if (count && count < sizeof path / sizeof path[0]) g_pinned = file_is_pinned(path);
    return TRUE;
}

const uint8_t *sh_host_image_base(void)
{
    resolve_once();
    return g_base;
}

size_t sh_host_image_size(void)
{
    resolve_once();
    return g_size;
}

const char *sh_host_image_name(void)
{
    resolve_once();
    return g_name;
}

int sh_host_is_pinned_rva_build(void)
{
    InitOnceExecuteOnce(&g_fingerprinted, fingerprint_host, NULL, NULL);
    return g_pinned;
}

#ifdef SH_HOST_IMAGE_TESTING
static const uint8_t *g_test_pinned_base;
int sh_host_test_bind_pinned_image(const uint8_t *base, const wchar_t *path)
{
    g_test_pinned_base = base && path && file_is_pinned(path) ? base : NULL;
    return g_test_pinned_base != NULL;
}
#endif

int sh_host_is_pinned_rva_image(const uint8_t *base)
{
    if (!base) return 0;
#ifdef SH_HOST_IMAGE_TESTING
    if (base == g_test_pinned_base) return 1;
#endif
    return base == sh_host_image_base() && sh_host_is_pinned_rva_build();
}

int sh_host_is_vulkan(void)
{
    if (sh_host_image_base() == NULL)
        return -1;
    /* Infer the renderer from its loaded library. */
    if (GetModuleHandleW(L"vulkan-1.dll") != NULL)
        return 1;
    if (GetModuleHandleW(L"OPENGL32.dll") != NULL)
        return 0;
    return -1;
}

const char *sh_host_renderer_name(void)
{
    const char *cached = (const char *)g_renderer;
    int vk;
    if (cached != NULL)
        return cached;
    vk = sh_host_is_vulkan();
    if (vk < 0)
        return "";          /* Retry until a renderer library is loaded. */
    cached = vk ? "vulkan" : "opengl";
    /* Racing callers store the same string literal, so a plain store is enough. */
    g_renderer = cached;
    return cached;
}
