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

/* The supported executables: the April 2024 update, Steam build 13954591. They
 * are not SteamStub-wrapped, so each renderer has exactly one digest. Any other
 * executable, the 2018 build included, is unsupported. */
const sh_build_identity SH_KNOWN_BUILDS[] = {
    {{0x3d,0x8b,0x43,0x93,0x64,0x9f,0x80,0xbd,0xae,0x85,0xc1,0x9a,0xe2,0xc9,0x85,0xc4,
      0x5e,0xc9,0xac,0x54,0xf7,0x4a,0xb0,0x4a,0x18,0x81,0x6e,0x30,0xea,0xd6,0x3c,0x21},
     1, "13954591-vk"},
    {{0x1b,0xdd,0xec,0xaa,0x4e,0x86,0x85,0x04,0x12,0xf9,0x6e,0x31,0x17,0x97,0x8b,0xce,
      0x1e,0xc7,0xbb,0x70,0x1a,0xe1,0xb5,0xa3,0x31,0x20,0xc2,0x1e,0x44,0x21,0xe5,0x1e},
     0, "13954591-gl"},
    {{0}, 0, NULL}
};

static const sh_build_identity *g_identity = NULL;

const sh_build_identity *sh_host_identity_for_digest(const unsigned char digest[32])
{
    size_t i;
    if (!digest) return NULL;
    for (i = 0; SH_KNOWN_BUILDS[i].label; i++)
        if (!memcmp(digest, SH_KNOWN_BUILDS[i].sha256, 32)) return &SH_KNOWN_BUILDS[i];
    return NULL;
}

/* Hash the file, not loaded code: relocations and installed detours must not
 * change build identity. Opening without write sharing pins the bytes we read. */
int sh_host_file_sha256(const wchar_t *path, unsigned char digest[32])
{
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char bytes[64 * 1024];
    DWORD count;
    int ok = 0;
    if (!path || !digest) return 0;
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
    if (BCryptFinishHash(hash, digest, 32, 0) < 0) goto done;
    ok = 1;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static const sh_build_identity *identity_of_file(const wchar_t *path)
{
    unsigned char digest[32];
    return sh_host_file_sha256(path, digest) ? sh_host_identity_for_digest(digest) : NULL;
}

static BOOL CALLBACK fingerprint_host(PINIT_ONCE once, PVOID arg, PVOID *context)
{
    static wchar_t path[32768];
    DWORD count;
    (void)once; (void)arg; (void)context;
    resolve_once();
    if (!g_base) return TRUE;
    count = GetModuleFileNameW(NULL, path, (DWORD)(sizeof path / sizeof path[0]));
    if (count && count < sizeof path / sizeof path[0]) g_identity = identity_of_file(path);
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

const sh_build_identity *sh_host_identity(void)
{
    InitOnceExecuteOnce(&g_fingerprinted, fingerprint_host, NULL, NULL);
    return g_identity;
}

int sh_host_is_supported_build(void)
{
    return sh_host_identity() != NULL;
}

#ifdef SH_HOST_IMAGE_TESTING
static const uint8_t *g_test_base;
static const sh_build_identity *g_test_identity;
const sh_build_identity *sh_host_test_bind_image(const uint8_t *base, const wchar_t *path)
{
    const sh_build_identity *id = base && path ? identity_of_file(path) : NULL;
    g_test_base = id ? base : NULL;
    g_test_identity = id;
    return id;
}
void sh_host_test_bind_identity(const uint8_t *base, const sh_build_identity *identity)
{
    g_test_base = identity ? base : NULL;
    g_test_identity = identity;
}
#endif

const sh_build_identity *sh_host_image_identity(const uint8_t *base)
{
    if (!base) return NULL;
#ifdef SH_HOST_IMAGE_TESTING
    if (base == g_test_base) return g_test_identity;
#endif
    return base == sh_host_image_base() ? sh_host_identity() : NULL;
}

int sh_host_image_is_supported(const uint8_t *base)
{
    return sh_host_image_identity(base) != NULL;
}

uint32_t sh_host_image_rva(const uint8_t *base, uint32_t rva_vk, uint32_t rva_gl)
{
    const sh_build_identity *id = sh_host_image_identity(base);
    return id ? (id->vulkan ? rva_vk : rva_gl) : 0;
}

uint32_t sh_host_rva(uint32_t rva_vk, uint32_t rva_gl)
{
    const sh_build_identity *id = sh_host_identity();
    return id ? (id->vulkan ? rva_vk : rva_gl) : 0;
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
