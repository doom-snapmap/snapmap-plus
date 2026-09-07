/* host_image.c -- see host_image.h. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "host_image.h"

/* The two shipped DOOM 2016 executables. Order is irrelevant; both are equally supported. */
static const char *const k_doom_names[] = {
    "DOOMx64vk.exe",   /* Vulkan  */
    "DOOMx64.exe",     /* OpenGL  */
};

static const uint8_t *g_base = NULL;
static size_t         g_size = 0;
static char           g_name[64] = { 0 };
static LONG           g_resolved = 0;   /* 0 = not tried, 1 = tried (success or failure) */

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

    /* Our DLL is loaded BY the game, so the process image is DOOM -- no name lookup needed
     * to FIND it. The name is read only to refuse a non-DOOM host. */
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
    /* k_doom_names[0] is the Vulkan image, which is where every known_rva in this product
     * was extracted from. See signatures.c for the pinned build's SHA256. */
    return sh_host_image_base() != NULL && _stricmp(g_name, k_doom_names[0]) == 0;
}

int sh_host_is_vulkan(void)
{
    if (sh_host_image_base() == NULL)
        return -1;
    /* Decided by the loaded renderer library, not the executable name: the name tells you what
     * was launched, this tells you what is actually driving the GPU. */
    if (GetModuleHandleW(L"vulkan-1.dll") != NULL)
        return 1;
    if (GetModuleHandleW(L"OPENGL32.dll") != NULL)
        return 0;
    return -1;
}
