/* nav_preview_sites.h -- the byte checks nav_preview.c makes on the engine's code, kept in one header so
 * sh_nav_preview_install and the image tests (tests/detour_windows_test.c, tests/nav_preview_sites_test.c) run the
 * same code on each build 13954591 executable. */
#ifndef SH_NAV_PREVIEW_SITES_H
#define SH_NAV_PREVIEW_SITES_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The two heads the post-process function may have, each whole instructions, stolen whole by the detour. The
 * function itself is a pointer the engine stores at run time in the cell NavPostProcessSlot loads. */
static const unsigned char SH_NAV_POST_VK_HEAD[] = {
    0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x48,0x83,0xec,0x60,
    0x65,0x48,0x8b,0x04,0x25,0x58,0,0,0 };
static const unsigned char SH_NAV_POST_GL_HEAD[] = {
    0x40,0x53,0x57,0x48,0x83,0xec,0x68,0x65,0x48,0x8b,0x04,0x25,0x58,0,0,0 };

/* NavPostProcessSlot is `mov rdx,[rip+rel32]`: the cell it loads, or NULL when the site is not that load. */
static __inline const unsigned char *sh_nav_post_cell(const unsigned char *load)
{
    int32_t rel;
    if (memcmp(load, "\x48\x8b\x15", 3)) return NULL;
    memcpy(&rel, load + 3, 4);
    return load + 7 + rel;
}

/* The bytes to steal at `post`: the length of the Vulkan head (*gl = 0) or the OpenGL head (*gl = 1), or 0 when
 * `post` begins with neither. */
static __inline size_t sh_nav_post_head(const unsigned char *post, int *gl)
{
    if (!memcmp(post, SH_NAV_POST_VK_HEAD, sizeof SH_NAV_POST_VK_HEAD)) { *gl = 0; return sizeof SH_NAV_POST_VK_HEAD; }
    if (!memcmp(post, SH_NAV_POST_GL_HEAD, sizeof SH_NAV_POST_GL_HEAD)) { *gl = 1; return sizeof SH_NAV_POST_GL_HEAD; }
    return 0;
}

/* The native immediate renderer: the first RIP-relative `lea rcx` in the first 96 bytes of the line consumer
 * (NavRenderLines) names it on both verified builds. NULL when there is none. */
static __inline const unsigned char *sh_nav_immediate(const unsigned char *draw)
{
    size_t i;
    int32_t rel;
    for (i = 0; i + 7 <= 96; i++)
        if (!memcmp(draw + i, "\x48\x8d\x0d", 3)) {
            memcpy(&rel, draw + i + 3, 4);
            return draw + i + 7 + rel;
        }
    return NULL;
}
#endif
