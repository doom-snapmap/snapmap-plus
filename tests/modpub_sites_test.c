/* modpub_sites_test.c -- the published-map gates' and the publish hooks' bind checks (modpub_sites.c
 * sh_modpub_check_sites and sh_modpublish_check_sites, the functions the DLL's two binds run) against each build
 * 13954591 executable given on the command line:
 *   - every site resolves at its recorded RVA, as the boot resolves it (sig_resolve_all), with the shell global;
 *   - every byte check holds on the executable's own code, and what it decodes lands inside the image (the lobby's
 *     leave slot included, which the binds treat as optional);
 *   - the lobby's parameters slot CacheReady calls is the one modlobby.c reads (+0x2B8);
 *   - a changed byte is refused by name.
 * Run via run-tests.ps1 with -Doom / -DoomAlt; each image in its own process. */
#include "image_sites.h"
#include "engine_globals.h"
#include "modpublished.h"
#include "modpublish.h"

static int in_image(const uint8_t *base, const void *p)
{
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    return p && (const uint8_t *)p > base && (const uint8_t *)p < base + nt->OptionalHeader.SizeOfImage;
}

static void check_image(const char *path)
{
    const sh_build_identity *identity = NULL;
    uint8_t *base = image_open(path, &identity);
    sig_result *results;
    size_t n, cap = sig_db_count();
    int vk;
    sh_modpub_sites ps;
    sh_modpub_bound pb;
    sh_modpublish_sites hs;
    sh_modpublish_bound hb;
    const char *failed;
    if (!base) return;
    vk = identity->vulkan;
    results = (sig_result *)calloc(cap, sizeof *results);
    if (!results) { g_image_fail = 1; free(base); return; }
    n = sig_resolve_all(base, results, cap);

    /* The published-map gates (modpublished.c eng_bind). */
    ps.on_snap = image_site(base, results, n, "PublishedOnSnapPlayDone", vk);
    ps.browser = image_site(base, results, n, "PublishedBrowserPlayDone", vk);
    ps.ready = image_site(base, results, n, "PublishedMapCacheReady", vk);
    ps.lobby = image_site(base, results, n, "CacheMapForLobby", vk);
    ps.feed = image_site(base, results, n, "PublishedFeedPlayDone", vk);
    ps.lobby_launch = image_site(base, results, n, "PublishedMapLobbyLaunch", vk);
    ps.offline_launch = image_site(base, results, n, "PublishedMapOfflineLaunch", vk);
    ps.read = image_site(base, results, n, "PublishedMapRead", vk);
    ps.limit = image_site(base, results, n, "SnapMapReadSizeLimit", vk);
    ps.selection = image_site(base, results, n, "PublishedMapSelectionConstruct", vk);
    ps.parse = image_site(base, results, n, "DeserializeFromJson", vk);
    ps.str_ctor = image_site(base, results, n, "IdStrCtor", vk);
    ps.str_dtor = image_site(base, results, n, "IdStrDtor", vk);
    ps.shell_slot = (const void *)glb_resolve(base, "shell_ptr_slot", NULL);
    IMG_CHECK(in_image(base, ps.shell_slot));
    failed = sh_modpub_check_sites(&ps, &pb);
    printf("  the published-map gates' checks: %s\n", failed ? failed : "all hold");
    IMG_CHECK(failed == NULL);
    if (!failed) {
        IMG_CHECK(in_image(base, pb.metadata_copy) && in_image(base, pb.snap_params) && in_image(base, pb.snap_source));
        IMG_CHECK(in_image(base, pb.browser_flag) && in_image(base, pb.hide_progress) && in_image(base, pb.session_slot));
        IMG_CHECK(in_image(base, pb.system) && in_image(base, pb.wait_text) && in_image(base, pb.show_wait));
        IMG_CHECK(in_image(base, pb.op_new) && in_image(base, pb.vft_ok) && in_image(base, pb.vft_fail) &&
                  in_image(base, pb.fail_fn));
        /* the lobby's parameters: the slot modlobby.c's LB_PARAMETERS names; the leave slot 0x2F0 */
        IMG_CHECK(pb.slot_parameters * sizeof(void *) == 0x2B8);
        IMG_CHECK(pb.slot_leave * sizeof(void *) == 0x2F0);
        printf("  session lobby slot +0x%zX, parameters +0x%zX, leave +0x%zX\n", pb.slot_lobby * sizeof(void *),
               pb.slot_parameters * sizeof(void *), pb.slot_leave * sizeof(void *));
        /* a changed byte is refused by name */
        {
            uint8_t *at = (uint8_t *)ps.lobby + 0x8B, keep = *at;
            *at ^= 0x01;
            failed = sh_modpub_check_sites(&ps, &pb);
            IMG_CHECK(failed && strstr(failed, "lobby cache +0x8A"));
            *at = keep;
            IMG_CHECK(sh_modpub_check_sites(&ps, &pb) == NULL);
        }
    }

    /* The publish hooks (modpublish.c eng_bind). */
    hs.fail = image_site(base, results, n, "PublishFail", vk);
    hs.copy = image_site(base, results, n, "MapInfoCopyToMyMaps", vk);
    hs.browser = image_site(base, results, n, "BrowserCopyToMyMaps", vk);
    hs.start = image_site(base, results, n, "PublishStart", vk);
    hs.after_save = image_site(base, results, n, "PublishAfterSave", vk);
    hs.str_ctor = ps.str_ctor;
    hs.str_dtor = ps.str_dtor;
    failed = sh_modpublish_check_sites(&hs, &hb);
    printf("  the publish hooks' checks: %s\n", failed ? failed : "all hold");
    IMG_CHECK(failed == NULL);
    if (!failed) {
        IMG_CHECK(in_image(base, hb.toast) && in_image(base, hb.screen_shell) && in_image(base, hb.shell_manager));
        IMG_CHECK(in_image(base, hb.record) && in_image(base, hb.entry) && in_image(base, hb.browser_state));
        IMG_CHECK(in_image(base, hb.hide_progress) && in_image(base, hb.tags_grow) && in_image(base, hb.idstr_assign));
        {
            uint8_t *at = (uint8_t *)hs.start + 0x1F5, keep = *at;
            *at ^= 0x04;
            failed = sh_modpublish_check_sites(&hs, &hb);
            IMG_CHECK(failed && strstr(failed, "PublishStart +0x1F5"));
            *at = keep;
            IMG_CHECK(sh_modpublish_check_sites(&hs, &hb) == NULL);
        }
    }
    free(results);
    (void)sh_host_test_bind_image(NULL, NULL);
    free(base);
}

static int synthetic(void)
{
    sh_modpub_sites ps;
    sh_modpublish_sites hs;
    memset(&ps, 0, sizeof ps);
    memset(&hs, 0, sizeof hs);
    /* No site, no bind: never a crash or a pass. */
    if (!sh_modpub_check_sites(NULL, NULL) || !sh_modpub_check_sites(&ps, NULL) ||
        !sh_modpublish_check_sites(NULL, NULL) || !sh_modpublish_check_sites(&hs, NULL)) {
        printf("FAIL a missing site passed\n");
        return 1;
    }
    return 0;
}

IMAGE_TEST_MAIN("modpub_sites_test", check_image, synthetic)
