/* Pure category arithmetic of the Snapmap+ settings tab. Native code wraps
 * modulo 2 (and 0x80000001); the tab wraps modulo the session's count. Also
 * the float-row unwrap helper (rows are now created without the native wrap).
 * Compile settings_tab.c with /DSH_SETTINGS_TAB_PURE. */
#include "../src/backend/settings_tab.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include "image_sites.h"
static int synthetic_main(void)
{
    int c,i;
    /* Two categories reproduce the native cycle in both directions. */
    assert(sh_settings_tab_wrap(0,1,2)==1&&sh_settings_tab_wrap(1,1,2)==0);
    assert(sh_settings_tab_wrap(0,-1,2)==1&&sh_settings_tab_wrap(1,-1,2)==0);
    /* Three categories visit the Snapmap+ tab and come back. */
    for(c=0,i=0;i<3;i++)c=sh_settings_tab_wrap(c,1,3);
    assert(c==0);
    assert(sh_settings_tab_wrap(1,1,3)==2&&sh_settings_tab_wrap(2,1,3)==0);
    assert(sh_settings_tab_wrap(0,-1,3)==2&&sh_settings_tab_wrap(2,-1,3)==1);
    /* A remembered category beyond a smaller count folds back into range. */
    assert(sh_settings_tab_wrap(2,0,2)==0);
    assert(sh_settings_tab_wrap(INT_MAX,1,3)>=0&&sh_settings_tab_wrap(INT_MIN,-1,3)>=0);
    assert(sh_settings_tab_wrap(5,0,0)==0);
    /* Float rows wrap natively; the tab clamps toward the limit stepped at.
     * View Distance 0..200000 (256 / 4096): 0 down -> 200000 is 0. */
    assert(sh_settings_tab_unwrap(0,200000,0,200000,256,4096)==0);
    assert(sh_settings_tab_unwrap(200000,0,0,200000,256,4096)==200000);
    /* An off-grid start near a limit and a large step wrap the same way. */
    assert(sh_settings_tab_unwrap(100,199844,0,200000,256,4096)==0);
    assert(sh_settings_tab_unwrap(199000,4000,0,200000,256,4096)==200000);
    /* Unlock Map Bounds 16000..32671: 16000 down -> 32671 stays 16000. */
    assert(sh_settings_tab_unwrap(16000,32671,16000,32671,256,4096)==16000);
    assert(sh_settings_tab_unwrap(32671,16000,16000,32671,256,4096)==32671);
    /* Ordinary steps, steps onto a limit and steps away from it are kept. */
    assert(sh_settings_tab_unwrap(8192,7936,0,200000,256,4096)==7936);
    assert(sh_settings_tab_unwrap(256,0,0,200000,256,4096)==0);
    assert(sh_settings_tab_unwrap(0,256,0,200000,256,4096)==256);
    assert(sh_settings_tab_unwrap(200000,199744,0,200000,256,4096)==199744);
    assert(sh_settings_tab_unwrap(199744,200000,0,200000,256,4096)==200000);
    assert(sh_settings_tab_unwrap(16000,20096,16000,32671,256,4096)==20096);
    /* A long jump that is not end to end is not a wrap. */
    assert(sh_settings_tab_unwrap(200000,190000,0,200000,256,4096)==190000);
    /* Ranges too narrow to tell a wrap from a step, and bad input, pass through. */
    assert(sh_settings_tab_unwrap(0,10,0,10,4,8)==10);
    assert(sh_settings_tab_unwrap(0,200000,0,200000,0,0)==200000);
    assert(isnan(sh_settings_tab_unwrap(0,NAN,0,200000,256,4096)));
    assert(sh_settings_tab_unwrap(NAN,5,0,200000,256,4096)==5);
    printf("settings_tab_test: all passed\n");
    return 0;
}

/* The install's byte checks (settings_tab.c sh_settings_tab_check_*, the functions the bind calls) on each build
 * 13954591 executable named on the command line. */
static void check_image(const char *path)
{
    const sh_build_identity *identity;
    uint8_t *base = image_open(path, &identity);
    sig_result *results;
    size_t n, cap = sig_db_count();
    uintptr_t lo, hi;
    if (!base) return;
    lo = (uintptr_t)base;
    hi = lo + ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    results = (sig_result *)calloc(cap, sizeof *results);
    if (!results) { g_image_fail = 1; free(base); return; }
    n = sig_resolve_all(base, results, cap);
    const uint8_t *dirty = image_site(base, results, n, "RenderSettingsDirtyCall", identity->vulkan);
    const uint8_t *call = image_site(base, results, n, "SettingsPanelClearCall", identity->vulkan);
    const uint8_t *clear = image_site(base, results, n, "SettingsPanelClear", identity->vulkan);
    uintptr_t target = 0;
    if (dirty) {
        int ok = sh_settings_tab_check_dirty((uintptr_t)dirty, &target);
        printf("  RenderSettingsDirtyCall E8 + dirty prefix: %s\n", ok ? "holds" : "FAIL");
        IMG_CHECK(ok);
        IMG_CHECK(target > lo && target < hi);
    }
    if (call && clear) {
        int ok = sh_settings_tab_check_clear((uintptr_t)call, (uintptr_t)clear);
        printf("  SettingsPanelClearCall -> SettingsPanelClear: %s\n", ok ? "holds" : "FAIL");
        IMG_CHECK(ok);
        IMG_CHECK(!sh_settings_tab_check_clear((uintptr_t)call, (uintptr_t)clear + 1));   /* the check is exact */
    }
    free(results);
    (void)sh_host_test_bind_image(NULL, NULL);
    free(base);
}

IMAGE_TEST_MAIN("settings_tab_test", check_image, synthetic_main)
