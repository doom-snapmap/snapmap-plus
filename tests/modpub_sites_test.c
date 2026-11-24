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

    /* The cloud-save request's read of the slot: the published copy is made there (R-LB1 F6). */
    {
        const uint8_t *call = image_site(base, results, n, "CloudSaveSlotReadCall", vk);
        const uint8_t *read = image_site(base, results, n, "ReadLocalSavedMapText", vk);
        failed = sh_modpublish_check_cloud_read(call, read);
        printf("  the cloud-save read's checks: %s\n", failed ? failed : "all hold");
        IMG_CHECK(failed == NULL);
        IMG_CHECK(call == base + (vk ? 0x11BE407u : 0x11BD7D7u));
        if (!failed) {
            uint8_t *at = (uint8_t *)call - 1, keep = *at;   /* the model's local id, +0x10 */
            *at ^= 0x08;
            failed = sh_modpublish_check_cloud_read(call, read);
            IMG_CHECK(failed && strstr(failed, "cloud-save read -0x04"));
            *at = keep;
        }
    }

    /* The cloud-save request's metadata read and the declChecksum hash: the published copy's metadata names its own
     * text (R-LB1 F6). */
    {
        const uint8_t *meta = image_site(base, results, n, "CloudSaveMetaReadCall", vk);
        const uint8_t *create = image_site(base, results, n, "NewSavedMapDeclChecksumCall", vk);
        const uint8_t *local = image_site(base, results, n, "LocalSaveDeclChecksumCall", vk);
        const uint8_t *meta_read = base + (vk ? 0x11CB8C0u : 0x11CAC90u);
        const void *hash = NULL;
        failed = sh_modpublish_check_cloud_meta(meta, meta_read, create, local, &hash);
        printf("  the cloud-save metadata's checks: %s\n", failed ? failed : "all hold");
        IMG_CHECK(failed == NULL);
        IMG_CHECK(hash == (const void *)(base + (vk ? 0x2D6A90u : 0x2D68F0u)));
        if (!failed) {
            uint8_t *at = (uint8_t *)create + 7, keep = *at;   /* +0xB8 */
            *at ^= 0x10;
            failed = sh_modpublish_check_cloud_meta(meta, meta_read, create, local, &hash);
            IMG_CHECK(failed && strstr(failed, "declChecksum at +0xB8"));
            *at = keep;
        }
    }

    /* The publish screens' FINISH step (modpublish.c eng_bind_finish, R-LB1 S7). */
    {
        sh_modpublish_finish_sites fs;
        sh_modpublish_finish_bound fb;
        fs.call = image_site(base, results, n, "PublishFinishGateCall", vk);
        fs.start = hs.start;
        fs.gdm_show = image_site(base, results, n, "EditorGdmShow", vk);
        fs.answer = image_site(base, results, n, "EditorGdmAnswerCall", vk);
        failed = sh_modpublish_check_finish(&fs, &fb);
        printf("  the FINISH step's checks: %s\n", failed ? failed : "all hold");
        IMG_CHECK(failed == NULL);
        if (!failed) {
            const uint8_t *gate_expect = base + (vk ? 0x1199CC0u : 0x1198F20u);
            const uint8_t *close_expect = base + (vk ? 0x1182430u : 0x1181590u);
            const uint8_t *input_expect = base + (vk ? 0x128DAA0u : 0x128D0F0u);
            /* the decoded targets are the functions read in Ghidra on this renderer */
            IMG_CHECK(fb.gate == (const void *)gate_expect && fb.close == (const void *)close_expect &&
                      fb.input == (const void *)input_expect);
            /* the success event the resume writes after PublishStart: idStructuredLogManager::CreateInfoLog */
            IMG_CHECK(fb.event_owner == (const void *)(base + (vk ? 0x17B2690u : 0x17B3A20u)) &&
                      fb.event_log == (const void *)(base + (vk ? 0x17B0380u : 0x17B1710u)) &&
                      !strcmp(fb.event_name, "snapeditor.publish.success"));
            printf("  the success event: %s, \"%s\"\n", fb.event_name, fb.event_desc);
            {
                uint8_t *at = (uint8_t *)fs.call + 0x17, keep = *at;   /* the overwrite byte's offset, +0x21 */
                *at ^= 0x01;
                failed = sh_modpublish_check_finish(&fs, &fb);
                IMG_CHECK(failed && strstr(failed, "FINISH gate call +0x15"));
                *at = keep;
            }
            {
                uint8_t *at = (uint8_t *)fs.answer + 0x0B, keep = *at;   /* the answered flag's offset, +0x1D8 */
                *at ^= 0x08;
                failed = sh_modpublish_check_finish(&fs, &fb);
                IMG_CHECK(failed && strstr(failed, "EditorFrame dialog answer"));
                *at = keep;
            }
            IMG_CHECK(sh_modpublish_check_finish(&fs, &fb) == NULL);
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
        !sh_modpublish_check_sites(NULL, NULL) || !sh_modpublish_check_sites(&hs, NULL) ||
        !sh_modpublish_check_finish(NULL, NULL)) {
        printf("FAIL a missing site passed\n");
        return 1;
    }
    return 0;
}

IMAGE_TEST_MAIN("modpub_sites_test", check_image, synthetic)
