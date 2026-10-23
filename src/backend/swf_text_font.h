#pragma once
#include <stdint.h>

/* The native case-insensitive font-name hash (VK 0x1a00480 / GL 0x19f26e0,
 * R-309). The font read stores it at idFont+0x4c (fontNameHash) from the
 * font's own resource name. Its only native readers type a font's family
 * through a static 13-pair table: the text-instance recognizer (VK 0x2b3fc0 /
 * GL 0x2b2fb0) and the per-glyph typing (VK 0x18199d0 -> 0x2b4ff0, after the
 * font's alias chain and alt-glyph ranges). Resource identity stays the name
 * at +8. A renamed private font form therefore carries the hash of the stock
 * request it serves, and the compiler keeps forms with different hashes apart. */
static __inline int sh_swf_font_name_hash(const char *name)
{
    uint32_t hash = 0;
    for (; name && *name; name++) {
        signed char c = (signed char)*name;
        if (c >= 'A' && c <= 'Z') c = (signed char)(c + 32);
        hash = hash * 31u + (uint32_t)(int32_t)c;
    }
    return (int)hash;
}
/* The material manager the font-FX registration (VK 0x289b80 / GL 0x2890a0)
 * passes to DeclFind, from its paired +0x61 LEA; NULL unless its +0x6b call
 * targets decl_find. Its list dedups by material name (swf+0x1c10). */
void *sh_swf_fontfx_manager_decode(const unsigned char *registration, const void *decl_find);
