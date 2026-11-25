/* nav_preview_sites_test.c -- the editor navigation overlay's bind checks (nav_preview_sites.h, the code
 * sh_nav_preview_install runs) against each build 13954591 executable given on the command line: the overlay
 * signatures resolve at their recorded RVAs; the slot is the `mov rdx,[rip+rel32]` load the install reads, with its
 * cell inside the image; the post-process function (a pointer stored at run time) is found by its head, which exactly
 * one place begins with for the renderer and none for the other; the first RIP-relative `lea rcx` in NavRenderLines'
 * first 96 bytes names a place inside the image; on OpenGL the render-target function resolves. A changed byte at
 * the slot, a head or the line consumer is refused. Each image runs in its own process. */
#include "image_sites.h"
#include "nav_preview_sites.h"

static const uint8_t *pe_end(const uint8_t *base)
{
    return base + ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
}

static int synthetic(void)
{
    /* A load, a head and a consumer built by hand: the checks accept them and refuse a changed byte. */
    uint8_t code[256] = {0};
    int gl = 9;
    const uint8_t *cell, *im;
    memcpy(code, "\x48\x8b\x15\x10\x00\x00\x00", 7);
    cell = sh_nav_post_cell(code);
    if (cell != code + 7 + 0x10) { printf("FAIL synthetic slot\n"); return 1; }
    code[0] = 0x4c;
    if (sh_nav_post_cell(code)) { printf("FAIL synthetic slot refused\n"); return 1; }
    memcpy(code + 64, SH_NAV_POST_GL_HEAD, sizeof SH_NAV_POST_GL_HEAD);
    if (sh_nav_post_head(code + 64, &gl) != sizeof SH_NAV_POST_GL_HEAD || gl != 1) { printf("FAIL synthetic GL head\n"); return 1; }
    memcpy(code + 64, SH_NAV_POST_VK_HEAD, sizeof SH_NAV_POST_VK_HEAD);
    if (sh_nav_post_head(code + 64, &gl) != sizeof SH_NAV_POST_VK_HEAD || gl != 0) { printf("FAIL synthetic VK head\n"); return 1; }
    code[64 + 3] ^= 1;
    if (sh_nav_post_head(code + 64, &gl)) { printf("FAIL synthetic changed head\n"); return 1; }
    memcpy(code + 128 + 5, "\x48\x8d\x0d\x20\x00\x00\x00", 7);
    im = sh_nav_immediate(code + 128);
    if (im != code + 128 + 5 + 7 + 0x20) { printf("FAIL synthetic lea\n"); return 1; }
    memset(code + 128, 0x90, 100);
    memcpy(code + 128 + 90, "\x48\x8d\x0d\x20\x00\x00\x00", 7);   /* ends past byte 96: not part of the scan */
    if (sh_nav_immediate(code + 128)) { printf("FAIL synthetic lea window\n"); return 1; }
    return 0;
}

static void check_image(const char *path)
{
    const sh_build_identity *identity;
    uint8_t *base = image_open(path, &identity);
    sig_result *results;
    size_t n, cap = sig_db_count(), at, size;
    const uint8_t *slot, *lines, *cell, *immediate, *found_vk = NULL, *found_gl = NULL;
    int found = 0;
    if (!base) return;
    results = (sig_result *)calloc(cap, sizeof *results);
    if (!results) { g_image_fail = 1; free(base); return; }
    n = sig_resolve_all(base, results, cap);
    image_site(base, results, n, "NavRenderStage", identity->vulkan);
    image_site(base, results, n, "NavRenderBegin", identity->vulkan);
    image_site(base, results, n, "NavRenderEnd", identity->vulkan);
    image_site(base, results, n, "NavRenderMatrix", identity->vulkan);
    slot = image_site(base, results, n, "NavPostProcessSlot", identity->vulkan);
    lines = image_site(base, results, n, "NavRenderLines", identity->vulkan);
    size = (size_t)(pe_end(base) - base);
    if (slot) {
        uint8_t keep = slot[0];
        cell = sh_nav_post_cell(slot);
        IMG_CHECK(cell != NULL && cell > base && cell + 8 < pe_end(base));
        printf("  NavPostProcessSlot cell at RVA 0x%x\n", cell ? (unsigned)(cell - base) : 0);
        ((uint8_t *)slot)[0] = 0x4c;
        IMG_CHECK(sh_nav_post_cell(slot) == NULL);
        ((uint8_t *)slot)[0] = keep;
        IMG_CHECK(sh_nav_post_cell(slot) == cell);
    }
    /* The post-process function's head: every place in the image the product's check accepts. */
    for (at = 0; at + sizeof SH_NAV_POST_VK_HEAD < size; at++) {
        int gl = 0;
        size_t stolen;
        if (base[at] != 0x48 && base[at] != 0x40) continue;
        stolen = sh_nav_post_head(base + at, &gl);
        if (!stolen) continue;
        found++;
        if (gl) found_gl = base + at; else found_vk = base + at;
        printf("  post-process head (%s, %u bytes) at RVA 0x%x\n", gl ? "OpenGL" : "Vulkan", (unsigned)stolen, (unsigned)at);
    }
    IMG_CHECK(found == 1);
    IMG_CHECK(identity->vulkan ? (found_vk && !found_gl) : (found_gl && !found_vk));
    {
        uint8_t *head = (uint8_t *)(found_vk ? found_vk : found_gl);
        int gl;
        if (head) {
            head[5] ^= 1;
            IMG_CHECK(sh_nav_post_head(head, &gl) == 0);
            head[5] ^= 1;
            IMG_CHECK(sh_nav_post_head(head, &gl) != 0);
        }
    }
    if (lines) {
        immediate = sh_nav_immediate(lines);
        IMG_CHECK(immediate != NULL && immediate > base && immediate < pe_end(base));
        printf("  NavRenderLines immediate renderer at RVA 0x%x\n", immediate ? (unsigned)(immediate - base) : 0);
        {
            uint8_t *op = (uint8_t *)lines;
            uint8_t saved[96];
            memcpy(saved, op, sizeof saved);
            memset(op, 0x90, sizeof saved);
            IMG_CHECK(sh_nav_immediate(lines) == NULL);
            memcpy(op, saved, sizeof saved);
            IMG_CHECK(sh_nav_immediate(lines) == immediate);
        }
    }
    if (!identity->vulkan) {
        sig_result target;
        IMG_CHECK(sig_resolve_one(base, &NAV_RENDER_TARGET_GL_SIGNATURE, &target) == SIG_OK);
    }
    free(results);
    (void)sh_host_test_bind_image(NULL, NULL);
    free(base);
}

IMAGE_TEST_MAIN("nav_preview_sites_test", check_image, synthetic)
