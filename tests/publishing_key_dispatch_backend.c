/* publishing_key_dispatch_backend.c -- the backend half of publishing_key_dispatch_test.cpp: a mods folder with a
 * publishing key whose backup offer is pending (the first publish made it), the Mods slot the window's worker calls
 * (modui.c sh_modui_slot: RUN, then FETCH for the answer) with the publishing key's requests bound to modclaim as the
 * product binds them (sh_modui_set_key), and the saved record read back from the mods folder's record file, as R-LV9
 * read it. */
#include "heap_out.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modclaim.h"
#include "modbundle.h"
#include "modui.h"
#include "snapmap_plus_iface.h"
#include "mod_test_util.h"
#include "text.h"
#include "../src/backend/script/script_compile.h"
#include "../src/backend/nav_bake.h"
#include "../src/backend/script/script_runtime.h"

void backend_log(const char *message) { (void)message; }
void sh_nav_bake_source_update_begin(void) {}
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped, unsigned char **out_bytes, size_t *out_len)
{ (void)name; (void)read_shipped; if (out_bytes) *out_bytes = NULL; if (out_len) *out_len = 0; return 0; }
int sh_nav_bake_overlay_open(const char *name, unsigned char **out, size_t *length)
{ (void)name; if (out) *out = NULL; if (length) *length = 0; return 0; }
int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{ (void)name; *out_bytes = NULL; *out_len = 0; return 0; }
int sh_navmesh_validate_aas(const unsigned char *p, size_t n, char *e, size_t cap)
{ (void)p; (void)n; if (e && cap) e[0] = 0; return 0; }
void sh_script_set_map_provider(sh_script_map_mods_fn fn, void *context) { (void)fn; (void)context; }
int modserve_activate_bundles(const char *const *bundle_hashes, int count) { (void)bundle_hashes; (void)count; return 0; }

static char g_top[MAX_PATH], g_mods[MAX_PATH * 2];

/* A key made by the first publish, its offer pending, and the Settings requests bound to the Mods slot. */
int pk_backend_setup(void)
{
    char keys[MAX_PATH * 2], *why = NULL;
    int made = 0;
    if (!mt_temp_dir("keydispatch", g_top, sizeof g_top)) return 0;
    _snprintf_s(keys, sizeof keys, _TRUNCATE, "%s\\publishing", g_top);
    _snprintf_s(g_mods, sizeof g_mods, _TRUNCATE, "%s\\mods", g_top);
    CreateDirectoryA(g_mods, NULL);
    modclaim_set_root(g_mods);
    modclaim_set_key_folder(keys);
    if (!modclaim_signer(NULL, &made, &why) || made != 1) { free(why); return 0; }
    modclaim_key_offer(1);
    sh_modui_set_key(modclaim_key_action, modclaim_key_status_json);   /* as modpublish.c binds them */
    return modclaim_key_offer_pending();
}

/* Where a backup is written and read (the test's stand-in for the native picker's answer). */
const char *pk_backend_backup_path(void)
{
    static char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\publishing-key.snapmap-plus-key", g_top);
    return path;
}

/* The worker's part of a Mods slot action off the game's thread: RUN, then FETCH the answer. 1 with *doc (malloc'd),
 * 0 when the slot did not take it. */
int pk_backend_run(int action, const char *arg, char **doc)
{
    sh_mods_call call;
    *doc = NULL;
    memset(&call, 0, sizeof call);
    call.op = SH_MODS_RUN;
    call.action = action;
    call.arg = arg;
    if (sh_modui_slot(NULL, &call) != 1) return 0;
    memset(&call, 0, sizeof call);
    call.op = SH_MODS_FETCH;
    if (sh_modui_slot(NULL, &call) != 1 || !call.out) return 0;
    *doc = _strdup(call.out);
    HeapFree(GetProcessHeap(), 0, call.out);
    return *doc != NULL;
}

/* The publishing key's record as saved in the mods folder's record file, its "offer" value ("" when none). */
int pk_backend_saved_offer(char *out, size_t cap)
{
    char *text = NULL;
    const char *at, *q, *end;
    out[0] = 0;
    if (sh_mb_record_get(g_mods, "publishing-key", &text) != 1 || !text) { free(text); return 0; }
    at = strstr(text, "\"offer\"");
    q = at ? strchr(at + 7, '"') : NULL;
    end = q ? strchr(q + 1, '"') : NULL;
    if (end && (size_t)(end - q - 1) < cap) { memcpy(out, q + 1, (size_t)(end - q - 1)); out[end - q - 1] = 0; }
    free(text);
    return out[0] != 0;
}

void pk_backend_teardown(void) { sh_modui_set_key(NULL, NULL); modclaim_set_root(NULL); }
