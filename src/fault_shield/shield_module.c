#include "fault_shield.h"
#include "../common/wide_path.h"

int shield_module_at(const void *address, wchar_t *path, size_t path_cap, char *name, size_t name_cap, uintptr_t *rva)
{
    HMODULE module = NULL;
    DWORD length;
    const wchar_t *leaf;
    *rva = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &module) || !module) return 0;
    *rva = (uintptr_t)address - (uintptr_t)module;
    length = GetModuleFileNameW(module, path, (DWORD)path_cap);
    if (!length || length >= path_cap) return 0;
    leaf = wcsrchr(path, L'\\');
    return sh_wpath_utf8(leaf ? leaf + 1 : path, name, name_cap) != 0;
}
