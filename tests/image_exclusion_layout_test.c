/* The idImage option-block layout the exclusion query reads, decoded from each local game image's own
 * exclusion branch (R-389). idImage Load (VK 0x140d9d240 / GL 0x140d94100) builds the placeholder block
 * at rsp+0x40 and hands it to the creation routine, which copies it to the image at +0x54 on both
 * renderers. The format qword (5) and the 16-byte constant holding the bias 13.0f sit at different block
 * offsets: Vulkan's block keeps the format at +0x3c, OpenGL's at +0x38. The product's layout for each
 * renderer must be the one its executable writes. Requires -Doom and -DoomAlt (native fixture). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "image_exclusion.h"
#include "native_pin_fixture.h"

static int g_failed;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

typedef struct load_site { const char *label; int renderer; uint32_t load, branch; } load_site;
/* renderer: 1 Vulkan, 0 OpenGL (native_pin_fixture.h). */
static const load_site g_sites[] = {
    {"vulkan", 1, 0x00d9d240u, 0x00d9d58du},
    {"opengl", 0, 0x00d94100u, 0x00d9444du},
};

static int find(const unsigned char *code, size_t length, const unsigned char *pattern, size_t size, size_t from)
{
    for (size_t i = from; i + size <= length; i++) if (!memcmp(code + i, pattern, size)) return (int)i;
    return -1;
}

static void check_site(const load_site *site)
{
    unsigned char prologue[0x60], branch[0x80], constant[16];
    if (!sh_native_pin_fixture_copy(site->renderer, site->load, prologue, sizeof(prologue)) ||
        !sh_native_pin_fixture_copy(site->renderer, site->branch, branch, sizeof(branch))) {
        CHECK(!"the local game image could not be read"); return;
    }
    /* mov rax,rsp; push rbp, rdi, r12, r14, r15; lea rbp,[rax-0x348]; sub rsp,0x420: rbp = rsp + 0x100. */
    static const unsigned char frame[] = {0x48,0x8b,0xc4,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,
        0x48,0x8d,0xa8,0xb8,0xfc,0xff,0xff,0x48,0x81,0xec,0x20,0x04,0x00,0x00};
    CHECK(!memcmp(prologue, frame, sizeof(frame)));
    /* The '_' test and the blacklist call branch to the exclusion block. */
    int jump = find(prologue, sizeof(prologue), (const unsigned char *)"\x80\x39\x5f\x0f\x84", 5, 0);
    CHECK(jump >= 0);
    if (jump >= 0) {
        int32_t rel; memcpy(&rel, prologue + jump + 5, 4);
        CHECK(site->load + (uint32_t)jump + 3 + 6 + (uint32_t)rel == site->branch);
    }
    /* The block passed to the creation routine: lea rdx,[rsp+0x40]; mov rcx,rsi; call. */
    int pass = find(branch, sizeof(branch), (const unsigned char *)"\x48\x8d\x54\x24\x40\x48\x8b\xce\xe8", 9, 0);
    CHECK(pass > 0);
    const size_t block = 0x40, image = 0x54;
    /* state 3 at block+4 (image +0x58), width and height 8 at block+0xc/+0x10 (image +0x60/+0x64). */
    CHECK(find(branch, sizeof(branch), (const unsigned char *)"\x48\xc7\x44\x24\x44\x03\x00\x00\x00", 9, 0) >= 0);
    CHECK(find(branch, sizeof(branch), (const unsigned char *)"\xc7\x44\x24\x4c\x08\x00\x00\x00", 8, 0) >= 0);
    CHECK(find(branch, sizeof(branch), (const unsigned char *)"\xc7\x44\x24\x50\x08\x00\x00\x00", 8, 0) >= 0);
    /* mov qword [rsp+X], 5: the format, and its upper dword 0 the word after it. */
    int format = find(branch, sizeof(branch), (const unsigned char *)"\x48\xc7\x44\x24", 4, 0);
    CHECK(format == 0 && !memcmp(branch + 5, "\x05\x00\x00\x00", 4));
    size_t format_at = image + branch[4] - block;
    /* movaps xmmN,[rip+d32]; movaps [rbp+d8],xmmN: the 16-byte constant holding the bias. */
    int load = -1, store = -1;
    for (size_t i = 0; i + 7 <= sizeof(branch) && load < 0; i++)
        if (branch[i] == 0x0f && branch[i + 1] == 0x28 && (branch[i + 2] & 0xc7) == 0x05) load = (int)i;
    CHECK(load >= 0);
    if (load < 0) return;
    unsigned reg = (branch[load + 2] >> 3) & 7;
    for (size_t i = (size_t)load + 7; i + 4 <= sizeof(branch) && store < 0; i++)
        if (branch[i] == 0x0f && branch[i + 1] == 0x29 && branch[i + 2] == (unsigned char)(0x45 | reg << 3)) store = (int)i;
    CHECK(store >= 0);
    if (store < 0) return;
    int32_t disp; memcpy(&disp, branch + load + 3, 4);
    CHECK(sh_native_pin_fixture_copy(site->renderer, site->branch + (uint32_t)load + 7 + (uint32_t)disp, constant, 16));
    size_t slot = SIZE_MAX;
    for (size_t k = 0; k < 4; k++) {
        uint32_t word; memcpy(&word, constant + 4 * k, 4);
        if (word == 0x41500000u) { CHECK(slot == SIZE_MAX); slot = k; }
        else CHECK(word == 0);
    }
    CHECK(slot != SIZE_MAX);
    size_t bias_at = image + (size_t)(0x100 + (int8_t)branch[store + 3]) - block + 4 * slot;
    sh_image_exclusion_layout product;
    CHECK(sh_image_exclusion_layout_of(site->renderer, &product));
    printf("%s: format +%#zx, word +%#zx, bias +%#zx (product +%#zx, +%#zx, +%#zx)\n", site->label, format_at,
        format_at + 4, bias_at, product.format, product.usage, product.bias);
    CHECK(product.format == format_at && product.usage == format_at + 4 && product.bias == bias_at);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(g_sites) / sizeof(*g_sites); i++) check_site(g_sites + i);
    if (g_failed) { printf("image_exclusion_layout_test: %d failure(s)\n", g_failed); return 1; }
    printf("image_exclusion_layout_test: all checks passed\n");
    return 0;
}
