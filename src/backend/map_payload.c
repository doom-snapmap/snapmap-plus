#include "map_payload.h"
#include "map_capacity.h"
#include "patch.h"
#include "backend_log.h"
#include <stdio.h>
#include <string.h>

static sh_patch_handle g_limits[2];
static int g_ready;
static const unsigned char save_original[] = {0x3d, 0xff, 0xff, 0x9f, 0x00};
/* The native expression compares unsigned (length - 1), then branches on JA.
 * INT_MAX - 1 admits positive signed lengths and still refuses zero/negative. */
static const unsigned char save_replacement[] = {0x3d, 0xfe, 0xff, 0xff, 0x7f};
/* The reader already rejects nonpositive signed lengths. Skip only its later
 * upper-bound rejection, without changing the shared read-only size constant. */
static const unsigned char read_original[] = {0x7e, 0x07};
static const unsigned char read_replacement[] = {0xeb, 0x07};
static const struct {
    const char *name;
    size_t offset, length;
    const unsigned char *original, *replacement;
} g_sites[] = {
    {"SnapMapSaveSizeLimit", 0, sizeof(save_original), save_original, save_replacement},
    {"SnapMapReadSizeLimit", 21, sizeof(read_original), read_original, read_replacement}
};

static int current_bytes(size_t index)
{
    __try { return g_limits[index].target &&
        !memcmp(g_limits[index].target, g_sites[index].replacement, g_sites[index].length); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int sh_map_payload_remove(void)
{
    size_t i;
    int ok = 1;
    /* Check ownership before restoring either site. A failed restore retains
     * its handle; a later install must finish recovery before applying again. */
    if (g_ready)
        for (i = 0; i < 2; i++)
            if (g_limits[i].live && !current_bytes(i)) return 0;
    g_ready = 0;
    for (i = 2; i > 0; i--)
        if (g_limits[i - 1].live && code_unpatch(&g_limits[i - 1]) != B2_PATCH_OK) ok = 0;
    return ok;
}
/* The reader's CMP EAX,[RIP+disp32] ends at the JLE patched below. Its operand
 * is the read-only bound the publish and copy uploads also compare
 * (map_capacity.h). Zero when it cannot be read. */
static size_t native_text_limit(uintptr_t jle)
{
    __try {
        int displacement = *(const int *)(jle - 4);
        int value = *(const int *)(jle + (intptr_t)displacement);
        return value > 0 ? (size_t)value : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int sh_map_payload_install(const sig_result *results, size_t count)
{
    sig_result sites[2];
    size_t i, j, limit;
    if (g_ready) return current_bytes(0) && current_bytes(1);
    /* A failed write can retain a rollback handle. Recover it before retrying. */
    if (!sh_map_payload_remove()) return 0;
    /* Resolve and preflight both sites before making either size change. */
    for (i = 0; i < 2; i++) {
        const sig_result *site = NULL;
        for (j = 0; results && j < count; j++)
            if (results[j].name && !strcmp(results[j].name, g_sites[i].name)) {
                site = &results[j]; break;
            }
        if (!site || site->status != SIG_OK || !site->addr ||
            site->addr > UINTPTR_MAX - g_sites[i].offset) goto failed;
        sites[i] = *site;
        sites[i].addr += g_sites[i].offset;
        __try {
            if (memcmp((void *)sites[i].addr, g_sites[i].original, g_sites[i].length)) goto failed;
        } __except (EXCEPTION_EXECUTE_HANDLER) { goto failed; }
    }
    /* Both sites hold the original code: read the bound the reader compares. */
    limit = native_text_limit(sites[1].addr);
    if (limit) sh_map_capacity_set_limit(limit);
    for (i = 0; i < 2; i++) {
        if (code_patch_sig(&sites[i], g_sites[i].original, g_sites[i].replacement,
                           g_sites[i].length, &g_limits[i]) != B2_PATCH_OK) {
            sh_map_payload_remove();
            goto failed;
        }
    }
    g_ready = 1;
    {
        char line[256];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
            "MPKG: native map saves and cached saved/published reads accept positive idStr lengths in this "
            "process; DOOM's %zu-byte map limit still applies to publishing, to copying a published map and "
            "to cloud-saved or published maps read without Snapmap+", sh_map_capacity_limit());
        backend_log(line);
    }
    return 1;
failed:
    backend_log("MPKG: native map length repair unavailable; signature or patch verification failed");
    return 0;
}
