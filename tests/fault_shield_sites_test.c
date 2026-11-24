/* fault_shield_sites_test.c -- the fault shield's bind checks, the product's own check code, run on each build
 * 13954591 executable named on the command line (run-tests.ps1 -Doom / -DoomAlt). A check that could never match the
 * shipped bytes leaves its guard off in every session; these run it against the real code:
 *   - recovery.c: idCommonLocal::Frame against k_frame_prologue (recovery_frame_prologue_ok), and the FatalError
 *     wrapper's level byte at +0x28 (recovery_fatalerror_site_ok);
 *   - mapload_guards.c: EventLink and InteractableSpawn against their stolen prologues;
 *   - static_model_guard.c: StaticModelUpdate's head, counter compare, list LEA and the counter == list + 0x60
 *     relation (sh_static_model_update_check), and StaticModelIncRef's head;
 *   - serialized_entities_guard.c: SerializedEntityIndexWrite's index write (sh_serialized_entities_site_ok).
 * Each site is resolved as its install resolves it: by signature name through sig_resolve_all, at the recorded RVA.
 * The four product files are compiled into this test; the stubs below stand in for the rest of the DLL. */
#include "../src/fault_shield/recovery.c"
#include "../src/fault_shield/mapload_guards.c"
#include "../src/fault_shield/static_model_guard.c"
#include "../src/fault_shield/serialized_entities_guard.c"
#include "image_sites.h"

/* ---- stand-ins for the DLL's other parts (none is reached by a check) */
uint8_t *g_doom_base;
shield_engine g_eng;
void shield_emit(const shield_fault *f) { (void)f; }
void backend_log(const char *message) { (void)message; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; (void)status; return 0; }
void sh_apply_prefab_poll_play(void) {}
int sh_engine_dialog_can_ask(void) { return 0; }
int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text)
{ (void)gdm_id; (void)button_set; (void)text; return 0; }
int sh_engine_dialog_poll(int ticket) { (void)ticket; return 0; }
void sh_engine_dialog_release(int ticket) { (void)ticket; }
void sh_commands_main_frame(void) {}
void sh_player_cheats_frame(void) {}

static void check_image(const char *path)
{
    const sh_build_identity *identity;
    uint8_t *base = image_open(path, &identity);
    sig_result *results;
    size_t n, cap = sig_db_count();
    const uint8_t *frame, *fatal, *evlink, *spawn, *update, *incref, *index_write_site;
    const uint8_t *list = NULL, *counter = NULL;
    if (!base) return;
    results = (sig_result *)calloc(cap, sizeof *results);
    if (!results) { g_image_fail = 1; free(base); return; }
    n = sig_resolve_all(base, results, cap);

    frame = image_site(base, results, n, "Frame", identity->vulkan);
    fatal = image_site(base, results, n, "FatalError7", identity->vulkan);
    evlink = image_site(base, results, n, "EventLink", identity->vulkan);
    spawn = image_site(base, results, n, "InteractableSpawn", identity->vulkan);
    update = image_site(base, results, n, "StaticModelUpdate", identity->vulkan);
    incref = image_site(base, results, n, "StaticModelIncRef", identity->vulkan);
    index_write_site = image_site(base, results, n, "SerializedEntityIndexWrite", identity->vulkan);

    if (frame) {
        int ok = recovery_frame_prologue_ok(frame);
        printf("  recovery: Frame's prologue %s\n", ok ? "holds" : "DIFFERS");
        IMG_CHECK(ok);
    }
    if (fatal) {
        int ok = recovery_fatalerror_site_ok(fatal);
        printf("  recovery: FatalError's level byte (+0x%X = 0x%02X) %s\n", FATALERROR_LEVEL_OFF,
               fatal[FATALERROR_LEVEL_OFF], ok ? "holds" : "DIFFERS");
        IMG_CHECK(ok);
    }
    if (evlink) {
        int ok = sh_evlink_prologue_ok(evlink);
        printf("  mapload: EventLink's prologue %s\n", ok ? "holds" : "DIFFERS");
        IMG_CHECK(ok);
    }
    if (spawn) {
        int ok = sh_interactable_prologue_ok(spawn);
        printf("  mapload: InteractableSpawn's prologue %s\n", ok ? "holds" : "DIFFERS");
        IMG_CHECK(ok);
    }
    if (update) {
        int rc = sh_static_model_update_check(update, &list, &counter);
        printf("  static model: StaticModelUpdate's checks %s\n",
               rc == 0 ? "hold" : rc == 1 ? "DIFFER (head or instruction)" : "DIFFER (counter is not the list's)");
        IMG_CHECK(rc == 0);
        IMG_CHECK(list && counter && counter == list + 0x60);
    }
    if (incref) {
        int ok = sh_static_model_incref_head_ok(incref);
        printf("  static model: StaticModelIncRef's head %s\n", ok ? "holds" : "DIFFERS");
        IMG_CHECK(ok);
    }
    if (index_write_site) {
        int ok = sh_serialized_entities_site_ok(index_write_site);
        printf("  serialized entities: the index write %s\n", ok ? "holds" : "DIFFERS");
        IMG_CHECK(ok);
    }
    free(results);
    (void)sh_host_test_bind_image(NULL, NULL);
    free(base);
}

/* The product's checks refuse a changed byte, so the image checks above can fail. */
static int synthetic(void)
{
    uint8_t frame[FRAME_STOLEN], fatal[FATALERROR_LEVEL_OFF + 1];
    int fail = 0;
    memcpy(frame, k_frame_prologue, FRAME_STOLEN);
    if (!recovery_frame_prologue_ok(frame)) { printf("FAIL the Frame prologue is refused\n"); fail = 1; }
    frame[FRAME_STOLEN - 1] ^= 1;
    if (recovery_frame_prologue_ok(frame)) { printf("FAIL a changed Frame prologue is accepted\n"); fail = 1; }
    memset(fatal, 0, sizeof fatal);
    fatal[FATALERROR_LEVEL_OFF] = 7;
    if (!recovery_fatalerror_site_ok(fatal)) { printf("FAIL the FatalError level byte 7 is refused\n"); fail = 1; }
    fatal[FATALERROR_LEVEL_OFF] = 6;
    if (recovery_fatalerror_site_ok(fatal)) { printf("FAIL a FatalError level byte 6 is accepted\n"); fail = 1; }
    return fail;
}

IMAGE_TEST_MAIN("fault_shield_sites_test", check_image, synthetic)
