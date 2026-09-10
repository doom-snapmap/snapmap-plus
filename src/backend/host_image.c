/* host_image.c -- see host_image.h. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "host_image.h"

/* Accepted host names; the first also identifies the Vulkan RVA fallback gate. */
static const char *const k_doom_names[] = {
    "DOOMx64vk.exe",   /* Vulkan  */
    "DOOMx64.exe",     /* OpenGL  */
};

static const uint8_t *g_base = NULL;
static size_t         g_size = 0;
static char           g_name[64] = { 0 };
static LONG           g_resolved = 0;   /* 0 = not tried, 1 = tried (success or failure) */
static const char * volatile g_renderer = NULL;   /* "vulkan"/"opengl" once definite; NULL until then */

static const char *basename_of(const char *path)
{
    const char *p = path, *last = path;
    for (; *p; p++) {
        if (*p == '\\' || *p == '/') last = p + 1;
    }
    return last;
}

static void resolve_once(void)
{
    char path[MAX_PATH];
    DWORD n;
    const char *leaf;
    size_t i;
    HMODULE h;

    if (InterlockedCompareExchange(&g_resolved, 1, 0) != 0)
        return;

    /* The process image is the host; its basename gates supported executable names. */
    h = GetModuleHandleA(NULL);
    if (h == NULL)
        return;

    n = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (n == 0 || n >= sizeof path)
        return;

    leaf = basename_of(path);
    for (i = 0; i < sizeof k_doom_names / sizeof k_doom_names[0]; i++) {
        if (_stricmp(leaf, k_doom_names[i]) == 0) {
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
            IMAGE_NT_HEADERS *nt;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return;
            nt = (IMAGE_NT_HEADERS *)((uint8_t *)h + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return;
            g_size = (size_t)nt->OptionalHeader.SizeOfImage;
            g_base = (const uint8_t *)h;
            strncpy_s(g_name, sizeof g_name, leaf, _TRUNCATE);
            return;
        }
    }
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
    /* This gate checks the Vulkan basename, not a hash or version.
     * The extraction-build fingerprint is documented in signatures.c. */
    return sh_host_image_base() != NULL && _stricmp(g_name, k_doom_names[0]) == 0;
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
