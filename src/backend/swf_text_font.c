#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "swf_text_font.h"

void *sh_swf_fontfx_manager_decode(const unsigned char *code, const void *decl_find)
{
    __try {
        if (!code || !decl_find || memcmp(code + 0x61, "\x48\x8d\x0d", 3) || code[0x6b] != 0xe8) return NULL;
        int32_t manager, call;
        memcpy(&manager, code + 0x64, sizeof(manager)); memcpy(&call, code + 0x6c, sizeof(call));
        if (code + 0x70 + call != (const unsigned char *)decl_find) return NULL;
        return (void *)(code + 0x68 + manager);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
