/* rawmap_paths_test -- where a rawmap save actually goes, driven directly.
 *
 * Written because a claim about it was disputed and prose is not evidence. The disagreement was
 * about stickiness: after naming a destination ("Save Rawmap As", `sh_rawmaps save <path>`), does
 * that file become the place every later save goes? It did, and it should not have. These tests
 * drive the same functions the console and the File menu call, and check the bytes on disk.
 *
 * The rule under test, in one sentence: a named destination applies to ONE write and is spent by it;
 * the only durable way to move the destination is the "Use Rawmap as Save Path" toggle.
 *
 * No hooks, no engine, no editor -- the rules are decisions about two strings and one file write,
 * and they can be checked without a game running.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../src/backend/rawmap.c"

/* ---- STUBS -------------------------------------------------------------------------------------
 * rawmap.c is one translation unit that also holds the hooks, the navmesh embedder and the editor
 * bridge. None of that is reachable from the destination rules -- those are two strings and a file
 * write -- so the collaborators are stubbed rather than linked. A stub that gets called would be a
 * bug in the test, so the ones with a return value return failure, which the caller under test
 * treats as "not available" and reports rather than ignores. The map manifest (modmanifest.c),
 * the member (map_carrier.c) and the text helpers (map_shards.c) are linked for real; the mod
 * builder and serving layer resolve to their built-in defaults.
 */
#include "signatures.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "editor_frame.h"
#include "cvars.h"
#include "hook.h"
#include "modpublished.h"

const sig_entry BACKEND_ENGINE_SIGNATURES[] = { { NULL, NULL, 0u } };

static int log_refused;
void backend_log(const char *line)
{
    /* One line per map refused for its earlier shape (rawmap.c). */
    if (line && !strncmp(line, "CARRIER: map refused: ", 22)) log_refused++;
}
static int render_loaded_calls;
void sh_map_render_loaded(void *map) { (void)map; render_loaded_calls++; }
char *sh_map_settings_prepare_load(const char *json, size_t len, size_t *out_len)
{ (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
void  sh_map_settings_loaded(int ok) { (void)ok; }
char *sh_map_settings_on_save(const char *json, size_t len, size_t *out_len)
{ (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }

void *install_inline_hook(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; return NULL; }

sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{ (void)module_base; (void)sig; (void)out; return SIG_NOT_FOUND; }

void  sh_navmesh_build_from_map(const char *json, size_t len) { (void)json; (void)len; }
char *sh_navmesh_strip(const char *json, size_t len, size_t *out_len)
{ (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
char *sh_navmesh_embed_all(const char *json, size_t len, size_t *out_len, int *failed)
{ (void)json; (void)len; if (out_len) *out_len = 0; if (failed) *failed = 0; return NULL; }
void  sh_nav_bake_set_map(const char *json, size_t len) { (void)json; (void)len; }
static int g_maps_opened;
void  sh_nav_bake_map_opened(void) { g_maps_opened++; }

int sh_editor_frame_request_rawmap_save_to(const char *destination, char *out_msg, int msg_capacity)
{ (void)destination;
  if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
                                             "stub: no editor in this test", _TRUNCATE);
  return 0; }
int sh_editor_frame_can_rawmap_save(char *out_msg, int msg_capacity)
{ if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
                                             "stub: no editor in this test", _TRUNCATE);
  return 0; }
int sh_editor_frame_request_reload(char *out_msg, int msg_capacity)
{ if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
                                             "stub: no editor in this test", _TRUNCATE);
  return 0; }
int sh_editor_frame_can_reload(char *out_msg, int msg_capacity)
{ if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
                                             "stub: no editor in this test", _TRUNCATE);
  return 0; }

static int g_pretty_on;   /* sh_pretty_on is the only cvar rawmap.c reads */
int sh_cvar_value_int(int index, int def) { (void)index; return g_pretty_on ? 1 : def; }

/* ---- end stubs -------------------------------------------------------------------------------- */

static int g_failed;
static int g_checks;

#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static const char BODY[] = "{\"entities\":[]}";

static void eff_paths(char *load, int load_cap, char *save, int save_cap)
{
    if (load) load[0] = '\0';
    if (save) save[0] = '\0';
    sh_rawmap_get_paths(load, load_cap, save, save_cap);
}

static int save_is_default(void)
{
    char eff[MAX_PATH] = "", def[MAX_PATH] = "";
    sh_rawmap_get_paths(NULL, 0, eff, (int)sizeof eff);
    sh_rawmap_get_default_paths(NULL, 0, def, (int)sizeof def);
    return (eff[0] && _stricmp(eff, def) == 0) ? 1 : 0;
}

static int temp_path(const char *name, char *out, size_t cap)
{
    char dir[MAX_PATH];
    if (GetTempPathA((DWORD)sizeof dir, dir) == 0) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s%s", dir, name);
    return 1;
}

/* Setting the LOAD source validates the file, so it has to exist and parse as a rawmap. */
static int make_json(const char *name, char *out, size_t cap)
{
    HANDLE h;
    DWORD wrote = 0;
    if (!temp_path(name, out, cap)) return 0;
    h = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, BODY, (DWORD)(sizeof BODY - 1), &wrote, NULL);
    CloseHandle(h);
    return 1;
}

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

static void clean_state(void)
{
    sh_rawmap_swap_set_source(NULL);
    sh_rawmap_set_save_target(NULL);
}

/* --------------------------------------------------------------------------------------------- */

/* Save Rawmap As picks a file, and Save keeps writing there. */
static void a_chosen_file_stays_chosen(void)
{
    char picked[MAX_PATH], save[MAX_PATH];

    printf("\n-- a file chosen for saving is still the target after the write\n");

    clean_state();
    CHECK(save_is_default());
    CHECK(temp_path("rawmap_test_export.json", picked, sizeof picked));
    DeleteFileA(picked);

    CHECK(sh_rawmap_set_save_target(picked) == 1);
    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   chosen:           save to: %s\n", save);
    CHECK(_stricmp(save, picked) == 0);
    CHECK(!save_is_default());

    CHECK(sh_rawmap_test_write(BODY, sizeof BODY - 1) == (sizeof BODY - 1));
    CHECK(file_exists(picked));                       /* it went where it was aimed */

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after the write:  save to: %s\n", save);
    CHECK(_stricmp(save, picked) == 0);               /* THE POINT: still there */
    CHECK(!save_is_default());

    DeleteFileA(picked);
    clean_state();
}

/* A write that FAILED keeps its aim, so a retry still goes to the named file. */
static void a_failed_write_keeps_its_destination(void)
{
    char save[MAX_PATH];
    /* a path that cannot be created: a directory component that is not a directory */
    static const char bad[] = "\\\\?\\Z:\\no-such-volume\\nope.json";

    printf("\n-- a write that fails keeps the destination, so a retry is still aimed there\n");

    clean_state();
    CHECK(sh_rawmap_set_save_target(bad) == 1);
    CHECK(sh_rawmap_test_write(BODY, sizeof BODY - 1) == 0);   /* the write fails */

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after the failure: save to: %s\n", save);
    CHECK(_stricmp(save, bad) == 0);
    CHECK(!save_is_default());

    clean_state();
}

/* THE ARCHIVE RULE. Opening a rawmap must never aim saves at it. */
static void opening_a_rawmap_never_aims_saves_at_it(const char *archive)
{
    char save[MAX_PATH];

    printf("\n-- opening a rawmap leaves saves on the usual file\n");

    clean_state();
    CHECK(sh_rawmap_swap_set_source(archive) == 1);

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after opening:    save to: %s\n", save);
    CHECK(_stricmp(save, archive) != 0);              /* NOT the file that was opened */
    CHECK(save_is_default());

    clean_state();
}

/* Choosing the opened rawmap is allowed -- it just has to be asked for. */
static void choosing_the_opened_rawmap_works(const char *archive)
{
    char save[MAX_PATH];

    printf("\n-- choosing the opened rawmap aims saves at it\n");

    clean_state();
    CHECK(sh_rawmap_swap_set_source(archive) == 1);
    CHECK(sh_rawmap_set_save_target(archive) == 1);

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after choosing:   save to: %s\n", save);
    CHECK(_stricmp(save, archive) == 0);

    clean_state();
}

/* Opening a different map drops the target, so it cannot follow the person around. */
static void a_different_map_drops_the_target(void)
{
    char picked[MAX_PATH], save[MAX_PATH];

    printf("\n-- opening a different map puts saves back on the usual file\n");

    clean_state();
    CHECK(temp_path("rawmap_test_export3.json", picked, sizeof picked));
    CHECK(sh_rawmap_set_save_target(picked) == 1);
    CHECK(!save_is_default());

    sh_rawmap_clear_save_target_for_new_map();

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after the change: save to: %s\n", save);
    CHECK(save_is_default());

    clean_state();
}

/* A bare word is not a destination: it would land in DOOM's own install folder. */
static void a_bare_word_is_refused(void)
{
    printf("\n-- a name with no folder is refused\n");

    clean_state();
    CHECK(sh_rawmap_set_save_target("banana") == 0);
    CHECK(save_is_default());

    clean_state();
}

/* Picking the usual file is not a choice to keep saves off it, so nothing is held. */
static void picking_the_usual_file_holds_nothing(void)
{
    char dflt[MAX_PATH] = "", target[MAX_PATH] = "x";

    printf("\n-- picking the usual file leaves nothing in force\n");

    clean_state();
    sh_rawmap_get_default_paths(NULL, 0, dflt, (int)sizeof dflt);
    CHECK(dflt[0] != '\0');

    CHECK(sh_rawmap_set_save_target(dflt) == 1);
    sh_rawmap_get_save_target(target, (int)sizeof target);
    printf("   after picking it: target is %s\n", target[0] ? target : "(nothing)");
    CHECK(target[0] == '\0');            /* the tick stays clear */
    CHECK(save_is_default());

    clean_state();
}

/* A file chosen earlier is released when the usual file is picked. */
static void picking_the_usual_file_releases_a_chosen_one(void)
{
    char dflt[MAX_PATH] = "", picked[MAX_PATH], target[MAX_PATH] = "x";

    printf("\n-- picking the usual file releases a file chosen before it\n");

    clean_state();
    sh_rawmap_get_default_paths(NULL, 0, dflt, (int)sizeof dflt);
    CHECK(temp_path("rawmap_test_export5.json", picked, sizeof picked));
    CHECK(sh_rawmap_set_save_target(picked) == 1);
    CHECK(!save_is_default());

    CHECK(sh_rawmap_set_save_target(dflt) == 1);
    sh_rawmap_get_save_target(target, (int)sizeof target);
    CHECK(target[0] == '\0');
    CHECK(save_is_default());

    clean_state();
}

/* Write a file with exact bytes -- these tests are about what the LAST bytes say. */
static int write_file(const char *name, const char *bytes, char *out, size_t cap)
{
    HANDLE h;
    DWORD wrote = 0;
    if (!temp_path(name, out, cap)) return 0;
    h = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, bytes, (DWORD)strlen(bytes), &wrote, NULL);
    CloseHandle(h);
    return 1;
}

/* WHAT COUNTS AS A RAWMAP, which is what `sh_rawmaps list` has to decide.
 *
 * It listed every *.json it found, and the folders involved are full of JSON that is not a map:
 * config.json, install.json, pinned.json, and a prefabs tree of *.snapmap.json. A prefab is the
 * awkward one -- it has a top-level "entities" array exactly like a map does, so the obvious marker
 * does not separate them. The engine's own top-level "~type" does. */
static void looks_like_rawmap_tells_them_apart(void)
{
    char p[MAX_PATH];

    printf("\n-- what counts as a rawmap (the `list` filter)\n");

    /* a rawmap: keys come out sorted, so "~type" is second to last */
    CHECK(write_file("rawmap_sniff_map.json",
                     "{\"entities\":[],\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111}",
                     p, sizeof p));
    printf("   rawmap                        -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 1);
    DeleteFileA(p);

    /* a PREFAB. Same "entities" array, different type. This is the case that made the first
     * filter wrong. */
    CHECK(write_file("rawmap_sniff_prefab.json",
                     "{\"cameraYaw\":125.5,\"entities\":[],\"grabAxis\":0,"
                     "\"~type\":\"idSnapEntityPrefab\",\"~version\":111}",
                     p, sizeof p));
    printf("   prefab                        -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
    DeleteFileA(p);

    /* a prefab holding MAP geometry, so the string idSnapMap appears -- as a PREFIX of
     * idSnapMapCapEntity. Searching without the closing quote would call this a map. */
    CHECK(write_file("rawmap_sniff_prefab_caps.json",
                     "{\"entities\":[{\"~type\":\"idSnapMapCapEntity\"}],"
                     "\"~type\":\"idSnapEntityPrefab\",\"~version\":111}",
                     p, sizeof p));
    printf("   prefab of map caps            -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
    DeleteFileA(p);

    /* our own settings files, which sit in the DEFAULT rawmap folder */
    CHECK(write_file("rawmap_sniff_config.json",
                     "{\"schema_version\":1,\"settings\":{\"theme\":\"dark\"}}", p, sizeof p));
    printf("   config.json                   -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
    DeleteFileA(p);

    /* not JSON at all */
    CHECK(write_file("rawmap_sniff_binary.json", "\x1f\x8b\x08 not json", p, sizeof p));
    printf("   not JSON                      -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
    DeleteFileA(p);

    /* a pretty-printed rawmap: sh_pretty_on re-lays these out, so the search cannot assume there
     * is no space after the colon */
    CHECK(write_file("rawmap_sniff_pretty.json",
                     "{\n  \"entities\": [],\n  \"version\": 111,\n"
                     "  \"~type\": \"idSnapMap\",\n  \"~version\": 111\n}\n",
                     p, sizeof p));
    printf("   pretty-printed rawmap         -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 1);
    DeleteFileA(p);

    /* a rawmap whose save carries the product's member after "~type" (map_carrier.h): a
     * navigation bake pushes the root type tag far beyond the last 8 KiB */
    {
        static char big[12000 + 400];
        size_t n = 0;
        n += (size_t)sprintf_s(big + n, sizeof big - n,
                               "{\"entities\":[],\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111,"
                               "\"snapmap-plus\":{\"format\":1,\"nav\":[{\"module\":\"m\",\"class\":\"c\",\"data\":[\"");
        memset(big + n, 'A', 12000);
        n += 12000;
        n += (size_t)sprintf_s(big + n, sizeof big - n, "\"]}]}}");
        CHECK(write_file("rawmap_sniff_carrier.json", big, p, sizeof p));
        printf("   rawmap with a long carrier    -> %d\n", sh_rawmap_looks_like_rawmap(p));
        CHECK(sh_rawmap_looks_like_rawmap(p) == 1);
        DeleteFileA(p);
        /* the same member on a prefab is still not a map */
        n = 0;
        n += (size_t)sprintf_s(big + n, sizeof big - n,
                               "{\"entities\":[],\"~type\":\"idSnapEntityPrefab\",\"~version\":111,"
                               "\"snapmap-plus\":{\"format\":1,\"nav\":[{\"module\":\"m\",\"class\":\"c\",\"data\":[\"");
        memset(big + n, 'A', 12000);
        n += 12000;
        n += (size_t)sprintf_s(big + n, sizeof big - n, "\"]}]}}");
        CHECK(write_file("rawmap_sniff_carrier_prefab.json", big, p, sizeof p));
        printf("   prefab with a long carrier    -> %d\n", sh_rawmap_looks_like_rawmap(p));
        CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
        DeleteFileA(p);
    }

    /* and the real thing on this machine, if it is there -- the fixtures above are only as good as
     * their resemblance to it, so check one that the engine actually wrote */
    {
        char real[MAX_PATH] = "";
        sh_rawmap_get_default_paths(NULL, 0, real, (int)sizeof real);
        if (real[0] && file_exists(real)) {
            printf("   the real %s -> %d\n", real, sh_rawmap_looks_like_rawmap(real));
            CHECK(sh_rawmap_looks_like_rawmap(real) == 1);
        } else {
            printf("   (no rawmap.json on this machine to check against)\n");
        }
    }
}

static void refused_menu_save_changes_nothing(void)
{
    char original[MAX_PATH], requested[MAX_PATH], actual[MAX_PATH], message[192];
    sh_rawmap_configure_fn configure = NULL;
    clean_state();
    CHECK(temp_path("rawmap_review_original.json", original, sizeof original));
    CHECK(temp_path("rawmap_review_requested.json", requested, sizeof requested));
    CHECK(sh_rawmap_set_save_target(original));
    sh_rawmap_get_slots(NULL, &configure, NULL);
    CHECK(configure(NULL, NULL, requested, -1, message, sizeof message) == 0);
    sh_rawmap_get_save_target(actual, sizeof actual);
    CHECK(strcmp(actual, original) == 0);
    CHECK(strstr(message, "no editor") != NULL);
    CHECK(configure(NULL, NULL, NULL, 5, message, sizeof message) == 0);
    clean_state();
}

static void missing_overwrite_protection_refuses_opening(const char *archive)
{
    sh_rawmap_configure_fn configure = NULL;
    char before[MAX_PATH], after[MAX_PATH], message[192];
    clean_state();
    sh_rawmap_set_branch_tag(1);
    sh_rawmap_get_paths(before, sizeof before, NULL, 0);
    sh_rawmap_get_slots(NULL, &configure, NULL);
    CHECK(!sh_rawmap_load_is_safe());
    CHECK(!configure(NULL, archive, NULL, 2, message, sizeof message));
    sh_rawmap_get_paths(after, sizeof after, NULL, 0);
    CHECK(strcmp(before, after) == 0);
    CHECK(strstr(message, "overwriting the map a rawmap opens over") != NULL);
    sh_rawmap_set_branch_tag(0);
    CHECK(sh_rawmap_load_is_safe());
    sh_rawmap_set_branch_tag(1);
}

static int live_serializations;
static void *fixture_idstr_ctor(void *self, const char *initial)
{ (void)initial; memset(self, 0, IDSTR_SIZE); return self; }
static void fixture_idstr_dtor(void *self) { (void)self; }
static unsigned char fixture_serialize(void *map, void *out, unsigned char compact)
{
    (void)map; (void)compact;
    ++live_serializations;
    *(int *)((char *)out + IDSTR_LEN_OFF) = sizeof BODY - 1;
    *(const char **)((char *)out + IDSTR_DATA_OFF) = BODY;
    return 1;
}
static unsigned char fixture_map_to_json(void *map, void *out, unsigned char compact)
{ return sh_ser_detour(map, out, compact); }

static int snapshot_visits, snapshot_accept;
static int fixture_visit(const void *map, const void *out, void *ctx)
{
    CHECK(map == (void *)1 && ctx == (void *)2);
    CHECK(*(const char *const *)((const char *)out + IDSTR_DATA_OFF) == BODY);
    CHECK(g_snapshot_depth == 1);
    snapshot_visits++;
    if (snapshot_accept < 0) RaiseException(0xe0000001, 0, 0, NULL);
    return snapshot_accept;
}

static void snapshots_visit_without_save_effects(void)
{
    unsigned char str[IDSTR_SIZE] = {0};
    unsigned long before = sh_rawmap_save_count();
    int caught = 0;
    clean_state();
    g_ser_orig = fixture_serialize;
    snapshot_visits = 0; snapshot_accept = 1;
    CHECK(sh_rawmap_snapshot_inspect(fixture_map_to_json,(void *)1,str,fixture_visit,(void *)2));
    CHECK(snapshot_visits == 1 && sh_rawmap_save_count() == before);
    CHECK(!g_snapshot_depth && !g_snapshot_visit && !g_snapshot_visit_ctx);
    CHECK(sh_rawmap_snapshot(fixture_map_to_json,(void *)1,str));
    CHECK(snapshot_visits == 1);
    snapshot_accept = 0;
    CHECK(!sh_rawmap_snapshot_inspect(fixture_map_to_json,(void *)1,str,fixture_visit,(void *)2));
    snapshot_accept = -1;
    __try { sh_rawmap_snapshot_inspect(fixture_map_to_json,(void *)1,str,fixture_visit,(void *)2); }
    __except(EXCEPTION_EXECUTE_HANDLER) { caught = 1; }
    CHECK(caught && !g_snapshot_depth && !g_snapshot_visit && !g_snapshot_visit_ctx);
    CHECK(sh_rawmap_save_count() == before);
    g_ser_orig = NULL;
    clean_state();
}

static void a_live_export_writes_once(void)
{
    char target[MAX_PATH], msg[192];
    unsigned long before = sh_rawmap_save_count();
    unsigned long long wrote = 0;
    clean_state();
    CHECK(temp_path("rawmap_review_live.json", target, sizeof target));
    CHECK(sh_rawmap_set_save_target(target));
    g_map_to_json = fixture_map_to_json;
    g_ser_orig = fixture_serialize;
    g_idstr_ctor = fixture_idstr_ctor;
    g_idstr_dtor = fixture_idstr_dtor;
    live_serializations = 0;
    CHECK(sh_rawmap_write_from_live((void *)1, target, msg, sizeof msg, &wrote));
    CHECK(live_serializations == 1 && wrote == sizeof BODY - 1);
    CHECK(sh_rawmap_save_count() == before + 1);
    CHECK(g_snapshot_depth == 0);
    CHECK(strstr(msg, "Saved the open map to ") != NULL && strstr(msg, "rawmap_review_live.json") != NULL);
    g_map_to_json = NULL;
    g_ser_orig = NULL;
    g_idstr_ctor = NULL;
    g_idstr_dtor = NULL;
    DeleteFileA(target);
    clean_state();
}

static void a_live_export_is_pretty_when_asked(void)
{
    char target[MAX_PATH], msg[192], bytes[128] = {0};
    unsigned long long wrote = 0;
    DWORD got = 0;
    HANDLE h;
    clean_state();
    CHECK(temp_path("rawmap_review_pretty.json", target, sizeof target));
    g_map_to_json = fixture_map_to_json;
    g_ser_orig = fixture_serialize;
    g_idstr_ctor = fixture_idstr_ctor;
    g_idstr_dtor = fixture_idstr_dtor;
    g_pretty_on = 1;
    CHECK(sh_rawmap_write_from_live((void *)1, target, msg, sizeof msg, &wrote));
    g_pretty_on = 0;
    CHECK(wrote > sizeof BODY - 1);
    h = CreateFileA(target, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(ReadFile(h, bytes, sizeof bytes - 1, &got, NULL));
        CloseHandle(h);
        CHECK(got == wrote && strchr(bytes, '\n') != NULL);
    }
    g_map_to_json = NULL;
    g_ser_orig = NULL;
    g_idstr_ctor = NULL;
    g_idstr_dtor = NULL;
    DeleteFileA(target);
    clean_state();
}

static void an_existing_scratch_file_survives(void)
{
    char target[MAX_PATH], scratch[MAX_PATH], bytes[64] = {0};
    HANDLE h;
    DWORD got = 0;
    static const char saved[] = "previous scratch contents";
    CHECK(temp_path("rawmap_review_atomic.json", target, sizeof target));
    CHECK(write_file("rawmap_review_atomic.json.tmp", saved, scratch, sizeof scratch));
    CHECK(sh_rawmap_set_save_target(target));
    CHECK(sh_rawmap_test_write(BODY, sizeof BODY - 1) == sizeof BODY - 1);
    h = CreateFileA(scratch, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(ReadFile(h, bytes, sizeof bytes - 1, &got, NULL));
        CloseHandle(h);
        CHECK(strcmp(bytes, saved) == 0);
    }
    /* A target locked against replacement must retain its original complete bytes. */
    h = CreateFileA(target, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(sh_rawmap_test_write("replacement", 11) == 0);
        memset(bytes, 0, sizeof bytes);
        CHECK(ReadFile(h, bytes, sizeof bytes - 1, &got, NULL));
        CHECK(strcmp(bytes, BODY) == 0);
        CloseHandle(h);
    }
    DeleteFileA(scratch);
    DeleteFileA(target);
    clean_state();
}

static void large_sources_use_native_lengths(void)
{
    char path[MAX_PATH], message[256], block[65536];
    HANDLE file;
    DWORD wrote;
    size_t i, length = 0;
    char *body;
    CHECK(temp_path("rawmap_large_source.json", path, sizeof(path)));
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    memset(block, ' ', sizeof(block));
    CHECK(WriteFile(file, "{", 1, &wrote, NULL) && wrote == 1);
    for (i = 0; i < 1040; i++) {
        if (!WriteFile(file, block, sizeof(block), &wrote, NULL) || wrote != sizeof(block)) {
            CHECK(0); CloseHandle(file); DeleteFileA(path); return;
        }
    }
    CHECK(WriteFile(file, "}", 1, &wrote, NULL) && wrote == 1);
    CloseHandle(file);
    CHECK(sh_rawmap_validate_source(path, message, sizeof(message)));
    CHECK(sh_rawmap_swap_set_source(path));
    body = read_source_file_why(&length, NULL, 0);
    CHECK(body && length == (size_t)1040 * sizeof(block) + 2);
    if (body) {
        CHECK(body[0] == '{' && body[length - 1] == '}' && !body[length]);
        HeapFree(GetProcessHeap(), 0, body);
    }
    CHECK(sh_rawmap_swap_set_source(NULL));
    DeleteFileA(path); clean_state();
}

/* ---- maps in an earlier shape are refused ------------------------------------------------- */

/* A map an earlier build saved: its packages ride as "smpkg." string variables beside an
 * ordinary one, and allocCount[4] counts all three. */
static const char LEGACY_MAP[] =
    "{\"~type\":\"idSnapMap\",\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,3],"
    "\"string\":["
    "{\"info\":{\"name\":\"smpkg.acceptance.cyberdemon.0.2.0123456789abcdef\",\"~type\":\"snapVarInfo_t\"},"
    "\"initialValue\":\"QUJD\"},"
    "{\"info\":{\"name\":\"door_label\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"Open\"},"
    "{\"info\":{\"name\":\"smpkg.acceptance.cyberdemon.1.2.0123456789abcdef\",\"~type\":\"snapVarInfo_t\"},"
    "\"initialValue\":\"REVG\"}"
    "]}}";

static char g_parsed[2048];
static int g_parses;
static int fixture_deser(const char *json, void *out_map)
{
    (void)out_map;
    g_parses++;
    strncpy_s(g_parsed, sizeof g_parsed, json ? json : "", _TRUNCATE);
    return 1;
}

/* What a refused parse leaves: the engine parsed nothing, the text is as it was, and the load's
 * refusal is the plain sentence. */
static void refused_plainly(int parses_before, int refusals_before)
{
    char refusal[256];
    CHECK(g_parses == parses_before);
    CHECK(log_refused == refusals_before + 1);
    CHECK(sh_modmanifest_load_refusal(refusal, sizeof refusal) &&
          !strcmp(refusal, "This map was saved in a format this version does not read."));
    (void)sh_modmanifest_load_notes_flush(0);
}

/* Every load the parse detour sees (a local save, a published map) refuses a map in an earlier
 * shape before the engine parses it: each retired variable family, and a member whose rows name
 * an "id" or a "bundle". A map in the current shape, and one whose words only look like a
 * family's, parse as before. */
static void earlier_shape_maps_are_refused(void)
{
    static const char plain[] = "{\"~type\":\"idSnapMap\",\"entities\":[]}";
    static const char *const families[] = {
        "smpkg.a.0.1.0123456789abcdef", "smpbind.x.0123456789abcdef", "smpref.y.0123456789abcdef",
        "smpmod1.0.1", "smpprod1.v0.2.1-beta.24", "smp.render.v1", "smp.bounds.v1",
        "smnav1.ind_dlc/room.monster48.0.1.0123456789abcdef.fedcba9876543210" };
    char map[1024];
    size_t i;
    int parses, refusals;
    printf("\n-- a map in an earlier shape is refused with the plain sentence; its text is unchanged\n");
    clean_state();
    g_deser_orig = fixture_deser;

    g_parses = 0; log_refused = 0;
    CHECK(sh_deser_detour(LEGACY_MAP, (void *)1) == 0);
    refused_plainly(0, 0);
    CHECK(strstr(LEGACY_MAP, "smpkg.") != NULL);   /* the engine's own buffer is never edited */
    for (i = 0; i < sizeof families / sizeof families[0]; i++) {
        _snprintf_s(map, sizeof map, _TRUNCATE,
                    "{\"~type\":\"idSnapMap\",\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
                    "{\"info\":{\"name\":\"%s\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"QUJD\"}]}}", families[i]);
        parses = g_parses; refusals = log_refused;
        CHECK(sh_deser_detour(map, (void *)1) == 0);
        refused_plainly(parses, refusals);
    }
    {
        static const char id_rows[] = "{\"~type\":\"idSnapMap\",\"entities\":[],\"snapmap-plus\":{\"format\":1,"
            "\"mods\":[{\"id\":\"alex.pink-imp\",\"version\":\"1\",\"bundle\":\"" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" "\",\"size\":1,\"names\":[]}]}}";
        parses = g_parses; refusals = log_refused;
        CHECK(sh_deser_detour(id_rows, (void *)1) == 0);
        refused_plainly(parses, refusals);
    }

    /* The current shape is handed to the engine byte for byte. */
    g_parses = 0; log_refused = 0;
    CHECK(sh_deser_detour(plain, (void *)1) == 1);
    CHECK(g_parses == 1 && log_refused == 0 && strcmp(g_parsed, plain) == 0);
    /* A name that only looks like a family, or a family's words in a value, is the author's. */
    {
        static const char lookalike[] =
            "{\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
            "{\"info\":{\"name\":\"notes smpkg.\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"smnav1.x\"}]}}";
        g_parses = 0; log_refused = 0;
        CHECK(sh_deser_detour(lookalike, (void *)1) == 1);
        CHECK(log_refused == 0 && strcmp(g_parsed, lookalike) == 0);
    }
    g_deser_orig = NULL;
    clean_state();
}

/* The editor can reload into the same map object. Every parse still counts as a
 * new map for anything keyed on the open map, so a module copy can never keep
 * the previous map's cut placement grid. */
static void a_reload_into_the_same_map_object_is_a_new_map(void)
{
    static const char plain[] = "{\"~type\":\"idSnapMap\",\"entities\":[]}";
    void *same = (void *)1;
    unsigned long generation;
    int opened;
    printf("\n-- a reload into the same map object is a new map\n");
    clean_state();
    g_deser_orig = fixture_deser;
    generation = sh_rawmap_load_generation();
    opened = g_maps_opened;
    CHECK(sh_deser_detour(plain, same) == 1);
    CHECK(sh_deser_detour(plain, same) == 1);
    CHECK(g_maps_opened == opened + 2);   /* each parse drops the last map's cut models */
    CHECK(sh_rawmap_load_generation() == generation + 2);
    g_deser_orig = NULL;
    clean_state();
}

/* The editor reload's parse: what LoadMap's DeserializeFromJson sees between
 * sh_rawmap_prepare_reload and sh_rawmap_reload_done. */
static int open_through_reload(const char *borrowed)
{
    char why[1280];
    int rc;
    if (!sh_rawmap_prepare_reload(why, (int)sizeof why)) return -1;
    rc = sh_deser_detour(borrowed, (void *)1);
    sh_rawmap_reload_done(0, 0);
    return rc;
}

/* A rawmap opened through the reload is refused the same way. */
static void a_staged_rawmap_in_an_earlier_shape_is_refused(void)
{
    char staged[MAX_PATH];
    printf("\n-- a rawmap in an earlier shape is refused the same way\n");
    clean_state();
    CHECK(write_file("rawmap_legacy_payload.json", LEGACY_MAP, staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    sh_rawmap_set_branch_tag(0);                /* this test installs no IsBranchMap answer */
    g_deser_orig = fixture_deser;
    g_parses = 0; log_refused = 0;
    CHECK(open_through_reload("{\"~type\":\"idSnapMap\",\"entities\":[]}") == 0);
    refused_plainly(0, 0);
    sh_rawmap_set_branch_tag(1);
    g_deser_orig = NULL;
    DeleteFileA(staged);
    clean_state();
}

/* A rawmap the swap opens is a new map until a save names it: its identity is
 * the rawmap's, not the saved map whose id the reload borrowed. */
static void a_rawmap_open_is_unsaved_until_its_first_save(void)
{
    static const char plain[] = "{\"~type\":\"idSnapMap\",\"entities\":[]}";
    unsigned char str[IDSTR_SIZE] = {0};
    char staged[MAX_PATH], name[MAX_PATH];
    printf("\n-- a rawmap opened as a new map is unsaved until its first save\n");
    clean_state();
    CHECK(write_file("rawmap_open_unsaved.json", plain, staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    sh_rawmap_set_branch_tag(0);
    g_deser_orig = fixture_deser;
    CHECK(!sh_rawmap_open_unsaved(name, sizeof name) && !name[0]);
    CHECK(open_through_reload("{\"~type\":\"idSnapMap\",\"entities\":[{}]}") == 1);
    CHECK(sh_rawmap_open_unsaved(name, sizeof name) && !strcmp(name, "rawmap_open_unsaved.json"));
    /* An export of the live map (a snapshot) is not a save. */
    g_ser_orig = fixture_serialize;
    CHECK(sh_rawmap_snapshot(fixture_map_to_json, (void *)1, str));
    CHECK(sh_rawmap_open_unsaved(NULL, 0));
    /* The first save names it. */
    CHECK(sh_ser_detour((void *)1, str, 1) == 1);
    CHECK(!sh_rawmap_open_unsaved(name, sizeof name) && !name[0]);
    /* Opened again, then the engine's own map: that map is itself. */
    CHECK(open_through_reload(plain) == 1 && sh_rawmap_open_unsaved(NULL, 0));
    CHECK(sh_deser_detour(plain, (void *)1) == 1 && !sh_rawmap_open_unsaved(NULL, 0));
    g_ser_orig = NULL;
    sh_rawmap_set_branch_tag(1);
    g_deser_orig = NULL;
    DeleteFileA(staged);
    clean_state();
}

/* A save never embeds a package: the serializer's bytes reach the caller unchanged. */
static void saves_carry_no_package_payload(void)
{
    unsigned char str[IDSTR_SIZE] = {0};
    printf("\n-- a save keeps the engine's own bytes\n");
    clean_state();
    g_ser_orig = fixture_serialize;
    CHECK(sh_ser_detour((void *)1, str, 1) == 1);
    CHECK(*(const char *const *)(str + IDSTR_DATA_OFF) == BODY);
    CHECK(*(const int *)(str + IDSTR_LEN_OFF) == (int)(sizeof BODY - 1));
    g_ser_orig = NULL;
    clean_state();
}

/* ---- the mods a rawmap names ------------------------------------------------------------------
 * The engine never parses a rawmap whose mods are missing: the swap falls back to the engine's own
 * map and the rawmap hold (modpublished.c, linked for real) asks the store, then refuses or offers
 * the download. A map with mods also writes the export beside its JSON. The store, the dialog and
 * the worker are doubles behind sh_modpub_ops; the worker runs its job on the calling thread. */
#define HASH_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define MVAR(name, value) \
    "{\"info\":{\"name\":\"" name "\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":\"" value "\"," \
    "\"~type\":\"snapVarString_t\"}"
#define MROW(salt, name, version, hash, size) \
    "{\"mod\":\"" salt "@" hash "\",\"name\":\"" name "\",\"version\":\"" version "\"," \
    "\"size\":" size ",\"names\":[]}"
static const char MOD_MAP[] =
    "{\"~type\":\"idSnapMap\",\"entities\":[],\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":["
    MVAR("door_label", "RAWMAP_MARK") "]},"
    "\"snapmap-plus\":{\"format\":1,\"mods\":[" MROW("testalph", "test.alpha", "1.0", HASH_A, "1048576") "]}}";
static const char ENGINE_MAP[] = "{\"~type\":\"idSnapMap\",\"entities\":[],\"name\":\"ENGINE_MARK\"}";

extern unsigned long long g_mod_stub_export_result;
extern int g_mod_stub_export_calls;
extern char g_mod_stub_export_path[520];
extern char g_mod_stub_import_cause[MODEXPORT_CAUSE_MAX];
extern const char *g_mod_stub_import_text;
extern int g_mod_stub_import_calls;

static struct {
    int installed, store, asks, answer, answered, fetches, resumes, resume_hook, prints, toasts;
    char ask[256], print[1600], toast[1600], resume_note[256];
} R;

static int r_on_main(void *c) { (void)c; return 1; }
static int r_row_state(void *c, const sh_modmanifest_row *row, const char *subject, char *cause, size_t cap)
{
    (void)c; (void)subject;
    if (cause && cap) cause[0] = 0;
    return !strcmp(row->build, HASH_A) && R.installed ? SH_MODPUB_ROW_SERVED : SH_MODPUB_ROW_MISSING;
}
static int r_rescan(void *c) { (void)c; return 1; }
static int r_activate(void *c, const sh_modmanifest_list *rows) { (void)c; return (int)rows->count; }
static int r_can_ask(void *c) { (void)c; return 1; }
static int r_ask(void *c, unsigned gdm, unsigned set, const char *text)
{ (void)c; (void)gdm; (void)set; R.asks++; R.answered = 0; strcpy_s(R.ask, sizeof R.ask, text); return 10 + R.asks; }
static int r_poll(void *c, int ticket)
{ (void)c; (void)ticket; if (R.answered || !R.answer) return 0; R.answered = 1; return R.answer; }
static void r_release(void *c, int ticket) { (void)c; (void)ticket; }
static int r_worker_run(void *c, void (*job)(void *), void *ctx) { (void)c; job(ctx); return 1; }
static int r_fetch(void *c, const sh_modmanifest_row *row, const char *title, const char *map_id,
                   sh_modpub_progress_fn progress, volatile int *cancel)
{
    (void)c; (void)title; (void)map_id; (void)progress; (void)cancel;
    R.fetches++;
    if (!strcmp(row->build, HASH_A)) R.installed = 1;
    return 0;
}
static int r_resume(void *c, const sh_modpub_resume *r)
{
    (void)c;
    R.resumes++;
    R.resume_hook = r->hook;
    /* What the engine's resume does for a rawmap hold. */
    return sh_rawmap_reopen_after_mods(R.resume_note, sizeof R.resume_note);
}
static int r_store_probe(void *c, const char *hash, volatile int *cancel, sh_modpub_probe *out)
{
    (void)c; (void)cancel;
    memset(out, 0, sizeof *out);
    out->found = strcmp(hash, HASH_A) ? 0 : R.store;
    return out->found;
}
static void r_print(void *c, const char *line) { (void)c; R.prints++; strcpy_s(R.print, sizeof R.print, line); }

static const sh_modpub_ops RAWMAP_OPS = {
    NULL, r_on_main, r_row_state, r_rescan, r_activate, r_can_ask, r_ask, r_poll, r_release,
    r_worker_run, r_fetch, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, r_resume, NULL,
    r_print, r_store_probe
};

static void notify_toast(const char *title, const char *text)
{ (void)title; R.toasts++; strcpy_s(R.toast, sizeof R.toast, text); }
static void notify_print(const char *line) { R.prints++; strcpy_s(R.print, sizeof R.print, line); }

static void mods_begin(void)
{
    sh_modpub_test_reset();
    memset(&R, 0, sizeof R);
    sh_modpub_test_set_ops(&RAWMAP_OPS);
    sh_modmanifest_reset();
    sh_rawmap_set_notify(notify_toast, notify_print);
    sh_rawmap_set_branch_tag(0);                /* no IsBranchMap answer in this test */
    g_deser_orig = fixture_deser;
}

static void mods_end(void)
{
    g_deser_orig = NULL;
    sh_rawmap_set_branch_tag(1);
    sh_rawmap_set_notify(NULL, NULL);
    sh_modpub_test_reset();
    sh_modpub_test_set_ops(NULL);
    clean_state();
}

static void ticks(int n) { for (int i = 0; i < n; i++) sh_modpublished_poll(); }

static void a_rawmap_without_its_mods_never_reaches_the_engine(void)
{
    char staged[MAX_PATH], why[256];
    unsigned long swaps;
    printf("\n-- a rawmap whose mods are missing and not in the store is refused before the engine parses it\n");
    mods_begin();
    R.store = 0;
    CHECK(write_file("rawmap_mods_missing.json", MOD_MAP, staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    swaps = sh_rawmap_swap_count();
    g_parses = 0;
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 0);
    CHECK(strstr(why, "rawmap_mods_missing.json needs 1 mod(s) you do not have") != NULL);
    CHECK(g_prepared == NULL && !g_in_reload && g_parses == 0 && sh_rawmap_swap_count() == swaps);
    CHECK(sh_modpub_test_state() == 4 && R.prints == 1);
    CHECK(strstr(R.print, "Rawmap rawmap_mods_missing.json needs 1 mod you do not have") != NULL);
    ticks(4);
    CHECK(!sh_modpublished_holding() && R.fetches == 0 && R.resumes == 0);
    /* The denial gate's cause 3: never published, and no folder at its hash. */
    CHECK(!strcmp(R.print, "This map needs test.alpha, which is not installed and was never published. "
                           "Restore its folder from the Recycle Bin or load the map's rawmap ZIP."));
    CHECK(R.asks == 1 && !strcmp(R.ask, R.print));
    DeleteFileA(staged);
    mods_end();
}

static void a_rawmap_whose_mods_the_store_has_is_offered_then_opens(void)
{
    char staged[MAX_PATH], why[256];
    printf("\n-- a rawmap whose mods the store has: the My Maps question, the download, then it opens\n");
    mods_begin();
    R.store = 1;
    CHECK(write_file("rawmap_mods_offer.json", MOD_MAP, staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 0);
    CHECK(strstr(why, "checking whether it can download them") != NULL);
    ticks(3);
    CHECK(R.asks == 1 && !strcmp(R.ask, "This map needs 1 mod: test.alpha (1.0 MB). Download and install now?"));
    R.answer = 1;
    ticks(3);
    CHECK(R.fetches == 1 && R.installed && R.resumes == 1 && R.resume_hook == SH_MODPUB_RAWMAP_LOAD);
    /* No editor in this test, so nothing opens and the note says how to load it again. */
    CHECK(!strcmp(R.resume_note, "The mods are installed. Open a map in the editor, then load "
                                 "rawmap_mods_offer.json again from File > Load Rawmap."));

    printf("-- with the mods installed the reload keeps the bytes it checked for the parse\n");
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 1 && g_prepared && g_in_reload);
    {
        char again[MAX_PATH];
        unsigned long swaps = sh_rawmap_swap_count();
        /* The file changes between the check and the parse: the checked bytes are parsed. */
        CHECK(write_file("rawmap_mods_offer.json", ENGINE_MAP, again, sizeof again));
        g_parses = 0;
        CHECK(sh_deser_detour("{\"~type\":\"idSnapMap\",\"name\":\"BORROWED\"}", (void *)1) == 1);
        CHECK(g_parses == 1 && strstr(g_parsed, "RAWMAP_MARK") != NULL && strstr(g_parsed, "snapmap-plus") == NULL);
        CHECK(sh_rawmap_swap_count() == swaps + 1 && g_prepared != NULL);
        sh_rawmap_reload_done(0, 0);
        CHECK(!g_in_reload && g_prepared == NULL);
    }
    DeleteFileA(staged);
    mods_end();
}

/* V12: a held rawmap ZIP was imported at the hold and again at the resume, so
 * every import line was printed twice. The resume parses the bytes the hold
 * checked; a later load of the same file imports it again. */
static void a_held_zip_is_imported_once(void)
{
    char staged[MAX_PATH], why[256];
    printf("\n-- a rawmap ZIP held for its mods is imported once; the resumed load reuses that import\n");
    mods_begin();
    R.store = 1;
    CHECK(temp_path("rawmap_held_once.zip", staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    g_mod_stub_import_text = MOD_MAP;
    g_mod_stub_import_calls = 0;
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 0);
    CHECK(strstr(why, "checking whether it can download them") != NULL);
    CHECK(g_mod_stub_import_calls == 1);
    ticks(3);
    R.answer = 1;
    ticks(3);
    CHECK(R.fetches == 1 && R.installed && R.resumes == 1);
    CHECK(g_mod_stub_import_calls == 1);   /* the resume itself reads nothing */
    /* The reopened load: the hold's bytes, not a second import. */
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 1 && g_prepared && strstr(g_prepared, "RAWMAP_MARK"));
    CHECK(g_mod_stub_import_calls == 1);
    sh_rawmap_reload_done(0, 0);
    /* The next load of the file is a new load: imported again. */
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 1 && g_mod_stub_import_calls == 2);
    sh_rawmap_reload_done(0, 0);

    printf("-- a hold that never resumed: the next load imports the file again\n");
    R.installed = 0;
    R.store = 0;
    R.fetches = R.resumes = 0;
    sh_modpub_test_reset();
    sh_modpub_test_set_ops(&RAWMAP_OPS);
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 0 && g_mod_stub_import_calls == 3);
    ticks(4);
    CHECK(R.resumes == 0);
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 0 && g_mod_stub_import_calls == 4);
    ticks(4);
    g_mod_stub_import_text = NULL;
    sh_rawmap_swap_set_source(NULL);
    mods_end();
}

static void a_rawmap_whose_mods_are_installed_loads(void)
{
    char staged[MAX_PATH];
    printf("\n-- a rawmap whose mods are installed opens\n");
    mods_begin();
    R.installed = 1;
    CHECK(write_file("rawmap_mods_ok.json", MOD_MAP, staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    g_parses = 0;
    CHECK(open_through_reload(ENGINE_MAP) == 1);
    CHECK(g_parses == 1 && strstr(g_parsed, "RAWMAP_MARK") != NULL);
    CHECK(!sh_modpublished_holding() && R.prints == 0 && R.asks == 0);
    DeleteFileA(staged);
    mods_end();
}

/* A rawmap opened over a map that pins a build takes the rawmap's
 * own rows, never the borrowed map's, and once its first save gave it its
 * own saved-map id the next Save writes that map instead of asking again. */
static char g_fake_edit_id[64];
static int fake_edit_id(char *out, size_t cap)
{
    if (!g_fake_edit_id[0]) return 0;
    strcpy_s(out, cap, g_fake_edit_id);
    return 1;
}

static void a_rawmap_open_takes_its_own_rows_and_then_its_own_id(void)
{
    static const char plain[] = "{\"~type\":\"idSnapMap\",\"entities\":[]}";
    unsigned char str[IDSTR_SIZE] = {0};
    char staged[MAX_PATH], name[MAX_PATH];
    sh_modmanifest_list cur;
    printf("\n-- a rawmap opened over a map with rows takes its own rows; after its first save Save writes it\n");
    mods_begin();
    R.installed = 1;
    sh_rawmap_set_edit_id_reader(fake_edit_id);
    /* The editor has a map open whose row pins a build. */
    CHECK(sh_deser_detour(MOD_MAP, (void *)1) == 1);
    CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].build, HASH_A));
    sh_modmanifest_list_free(&cur);
    /* A rawmap without rows opened over it: none of the borrowed map's rows stays. */
    CHECK(write_file("rawmap_own_rows.json", plain, staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    CHECK(open_through_reload(MOD_MAP) == 1 && !strcmp(g_parsed, plain));
    CHECK(sh_modmanifest_current(&cur) == 0);
    sh_modmanifest_list_free(&cur);
    CHECK(sh_rawmap_open_unsaved(name, sizeof name) && !strcmp(name, "rawmap_own_rows.json"));
    /* While the edit holds the borrowed id, Save asks for a name. */
    sh_rawmap_set_branch_tag(1);
    strcpy_s(g_fake_edit_id, sizeof g_fake_edit_id, "BORROWED00000000000A");
    CHECK(sh_isbranch_detour(NULL) == 1 && sh_isbranch_detour(NULL) == 1);
    /* The first save names it: the edit now holds its own id, and the second
     * Save writes that map (the engine answers; it is no branch). */
    g_ser_orig = fixture_serialize;
    CHECK(sh_ser_detour((void *)1, str, 1) == 1);
    CHECK(!sh_rawmap_open_unsaved(NULL, 0));
    strcpy_s(g_fake_edit_id, sizeof g_fake_edit_id, "OWNID000000000000000B");
    CHECK(sh_isbranch_detour(NULL) == 0 && sh_isbranch_detour(NULL) == 0);
    /* An edit whose id cannot be read keeps the safe answer. */
    sh_rawmap_set_branch_tag(0);
    DeleteFileA(staged);
    /* A rawmap with a row opened over a map: exactly its rows, and a fresh borrow. */
    CHECK(write_file("rawmap_own_rows2.json", MOD_MAP, staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    CHECK(open_through_reload(plain) == 1 && strstr(g_parsed, "RAWMAP_MARK") != NULL);
    CHECK(sh_modmanifest_current(&cur) == 1 && !strcmp(cur.rows[0].build, HASH_A));
    sh_modmanifest_list_free(&cur);
    sh_rawmap_set_branch_tag(1);
    g_fake_edit_id[0] = 0;
    CHECK(sh_isbranch_detour(NULL) == 1);
    strcpy_s(g_fake_edit_id, sizeof g_fake_edit_id, "BORROWED00000000000C");
    CHECK(sh_isbranch_detour(NULL) == 1);
    /* A browser load of another map: the engine's map, itself. */
    CHECK(sh_deser_detour(plain, (void *)1) == 1);
    CHECK(!sh_rawmap_open_unsaved(NULL, 0) && sh_isbranch_detour(NULL) == 0);
    sh_rawmap_set_edit_id_reader(NULL);
    g_fake_edit_id[0] = 0;
    g_ser_orig = NULL;
    DeleteFileA(staged);
    mods_end();
}

static void a_refused_zip_tells_the_player_why(void)
{
    char staged[MAX_PATH], why[1280], cause[MODEXPORT_CAUSE_MAX], want[sizeof cause + 64];
    size_t n;
    printf("\n-- a ZIP the importer refuses: the player hears the importer's reason, whole, and nothing loads\n");
    mods_begin();
    CHECK(temp_path("shared-map.zip", staged, sizeof staged));
    CHECK(sh_rawmap_swap_set_source(staged));
    /* A reason longer than the old 320-byte buffer, ending in its next step. */
    strcpy_s(cause, sizeof cause, "test.alpha 1.0.0, a mod this map carries, does not install on this computer (");
    for (n = strlen(cause); n < 600; n++) cause[n] = 'x';
    cause[n] = 0;
    strcat_s(cause, sizeof cause, "); nothing was installed. Ask whoever shared it for a new export saved with this release");
    strcpy_s(g_mod_stub_import_cause, sizeof g_mod_stub_import_cause, cause);
    _snprintf_s(want, sizeof want, _TRUNCATE, "shared-map.zip was not opened: %s.", cause);
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 0);
    CHECK(!strcmp(why, want));
    CHECK(strstr(why, "backend log") == NULL);
    CHECK(g_prepared == NULL && !g_in_reload);
    sh_rawmap_poll_main();
    CHECK(R.prints == 1 && !strcmp(R.print, want));
    CHECK(R.toasts == 1 && !strcmp(R.toast, want));

    printf("-- a .json that is missing: Windows' reason, worded\n");
    CHECK(temp_path("no-such-map.json", staged, sizeof staged));
    DeleteFileA(staged);
    CHECK(sh_rawmap_swap_set_source(staged));
    CHECK(sh_rawmap_prepare_reload(why, (int)sizeof why) == 0);
    CHECK(!strncmp(why, "no-such-map.json was not opened: ", 33) && strstr(why, "is missing") && strstr(why, "try again"));
    strcpy_s(g_mod_stub_import_cause, sizeof g_mod_stub_import_cause, "the rawmap export is not linked into this test");
    sh_rawmap_swap_set_source(NULL);
    mods_end();
}

static void a_save_with_mods_writes_the_zip_beside_the_json(void)
{
    char target[MAX_PATH], zip[MAX_PATH], status[4096];
    sh_rawmap_save_report last;
    sh_rawmap_status_fn status_fn = NULL;
    printf("\n-- a save of a map that uses mods writes <name>.zip beside the .json and says to share it\n");
    mods_begin();

    CHECK(sh_rawmap_companion_zip_path("C:\\maps\\rawmap.json", zip, sizeof zip) && !strcmp(zip, "C:\\maps\\rawmap.zip"));
    CHECK(sh_rawmap_companion_zip_path("C:\\maps\\Arena.JSON", zip, sizeof zip) && !strcmp(zip, "C:\\maps\\Arena.zip"));
    CHECK(sh_rawmap_companion_zip_path("C:\\maps\\notes.txt", zip, sizeof zip) && !strcmp(zip, "C:\\maps\\notes.txt.zip"));
    CHECK(!sh_rawmap_companion_zip_path("C:\\maps\\shared.zip", zip, sizeof zip) && !zip[0]);
    CHECK(!sh_rawmap_companion_zip_path("", zip, sizeof zip));

    CHECK(temp_path("rawmap_share.json", target, sizeof target));
    CHECK(sh_rawmap_set_save_target(target));
    g_mod_stub_export_calls = 0;
    g_mod_stub_export_result = 4321;
    CHECK(sh_rawmap_test_write(MOD_MAP, sizeof MOD_MAP - 1) == sizeof MOD_MAP - 1);
    CHECK(g_mod_stub_export_calls == 1 && sh_rawmap_companion_zip_path(target, zip, sizeof zip) &&
          !_stricmp(g_mod_stub_export_path, zip));
    sh_rawmap_last_save(&last);
    CHECK(!_stricmp(last.json, target) && !_stricmp(last.zip, zip) && last.zip_bytes == 4321 && last.mods == 1);
    CHECK(R.toasts == 0);                       /* never from inside the save itself */
    sh_rawmap_poll_main();
    CHECK(R.toasts == 1 && R.prints == 1 &&
          !strcmp(R.toast, "This map uses 1 mod; share rawmap_share.zip, the .json alone will not load for others.") &&
          !strcmp(R.print, R.toast));
    sh_rawmap_poll_main();
    CHECK(R.toasts == 1);                       /* once */

    sh_rawmap_get_slots(&status_fn, NULL, NULL);
    CHECK(status_fn && status_fn(NULL, status, (int)sizeof status) > 0);
    CHECK(strstr(status, "\"lastZip\":\"") && strstr(status, "rawmap_share.zip\"") &&
          strstr(status, "\"lastZipBytes\":4321") && strstr(status, "\"lastMods\":1") &&
          strstr(status, "\"mapMods\":") && status[strlen(status) - 1] == '}');

    printf("-- an export that cannot be written says so, and the .json is still written\n");
    g_mod_stub_export_result = 0;
    CHECK(sh_rawmap_test_write(MOD_MAP, sizeof MOD_MAP - 1) == sizeof MOD_MAP - 1);
    sh_rawmap_poll_main();
    CHECK(!strcmp(R.toast, "This map uses 1 mod, but rawmap_share.zip could not be written (the rawmap export is not "
                           "linked into this test). The .json alone will not load for others."));
    sh_rawmap_last_save(&last);
    CHECK(last.zip[0] && last.zip_bytes == 0 && strstr(last.zip_error, "not linked") != NULL);

    printf("-- a map without mods writes only the .json, and says nothing\n");
    g_mod_stub_export_calls = 0;
    R.toasts = 0;
    CHECK(sh_rawmap_test_write(BODY, sizeof BODY - 1) == sizeof BODY - 1);
    sh_rawmap_poll_main();
    sh_rawmap_last_save(&last);
    CHECK(g_mod_stub_export_calls == 0 && R.toasts == 0 && !last.zip[0] && last.mods == 0 && last.json[0]);

    DeleteFileA(target);
    mods_end();
}

static void the_retired_every_load_switch_is_refused(void)
{
    sh_rawmap_configure_fn configure = NULL;
    char message[192];
    printf("\n-- the retired every-load switch is refused in plain words\n");
    clean_state();
    sh_rawmap_get_slots(NULL, &configure, NULL);
    CHECK(!configure(NULL, NULL, NULL, 1, message, sizeof message));
    CHECK(strstr(message, "File > Load Rawmap") != NULL);
    CHECK(!configure(NULL, NULL, NULL, 0, message, sizeof message));
}

/* A load path is a file to open now through the reload, never a replacement for any
 * other parse: a published read, a lobby map, or a map opened from My Maps loads as
 * itself, and nothing waits for a later map. */
static void a_load_path_never_replaces_another_parse(const char *archive)
{
    static const char engine[] = "{\"~type\":\"idSnapMap\",\"entities\":[],\"name\":\"ENGINE_MARK\"}";
    sh_rawmap_configure_fn configure = NULL;
    char message[192];
    unsigned long swaps;
    printf("\n-- a load path never replaces another parse, and Load at the menu is refused at once\n");
    clean_state();
    sh_rawmap_set_branch_tag(0);
    g_deser_orig = fixture_deser;
    CHECK(sh_rawmap_swap_set_source(archive));
    swaps = sh_rawmap_swap_count();
    g_parses = 0;
    CHECK(sh_deser_detour(engine, (void *)1) == 1);
    CHECK(g_parses == 1 && !strcmp(g_parsed, engine) && sh_rawmap_swap_count() == swaps);
    /* Load Rawmap with no live editor: refused now, the load path unchanged. */
    sh_rawmap_swap_set_source(NULL);
    sh_rawmap_get_slots(NULL, &configure, NULL);
    CHECK(!configure(NULL, archive, NULL, 2, message, sizeof message));
    CHECK(strstr(message, "no editor") != NULL);
    {
        char load[MAX_PATH], dflt[MAX_PATH];
        sh_rawmap_get_paths(load, (int)sizeof load, NULL, 0);
        sh_rawmap_get_default_paths(dflt, (int)sizeof dflt, NULL, 0);
        CHECK(!_stricmp(load, dflt));
    }
    CHECK(!sh_rawmap_open_now(archive, message, sizeof message));
    /* Inside a reload whose checked text is gone the borrowed map is never parsed. */
    InterlockedExchange(&g_in_reload, 1);
    g_parses = 0;
    CHECK(sh_deser_detour(engine, (void *)1) == 0 && g_parses == 0);
    InterlockedExchange(&g_in_reload, 0);
    g_deser_orig = NULL;
    sh_rawmap_set_branch_tag(1);
    clean_state();
}

/* A path outside the ANSI code page is read, validated and written: paths are UTF-8
 * with the wide file calls. */
static void a_unicode_path_is_read_and_written(void)
{
    /* "rawmap-" + U+00E9 + U+4E16 + ".json" in UTF-8 */
    static const char name[] = "rawmap-\xC3\xA9\xE4\xB8\x96.json";
    char dir[MAX_PATH], path[MAX_PATH], why[256], out[64] = {0};
    wchar_t wpath[MAX_PATH];
    unsigned long long wrote;
    size_t len = 0;
    char *text;
    printf("\n-- a non-ASCII path reads and writes\n");
    clean_state();
    CHECK(GetTempPathA((DWORD)sizeof dir, dir) != 0);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s%s", dir, name);
    CHECK(sh_rawmap_set_save_target(path) == 1);
    wrote = sh_rawmap_test_write(BODY, sizeof BODY - 1);
    CHECK(wrote == sizeof BODY - 1);
    CHECK(sh_rawmap_path_wide(path, wpath, MAX_PATH));
    CHECK(GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES);
    CHECK(sh_rawmap_validate_source(path, why, (int)sizeof why) == 1 && !why[0]);
    CHECK(sh_rawmap_swap_set_source(path));
    text = read_source_file_why(&len, why, sizeof why);
    CHECK(text != NULL && len == sizeof BODY - 1);
    if (text) { memcpy(out, text, len < sizeof out - 1 ? len : sizeof out - 1); HeapFree(GetProcessHeap(), 0, text); }
    CHECK(!strcmp(out, BODY));
    DeleteFileW(wpath);
    clean_state();
}

int main(void)
{
    char archive[MAX_PATH];

    printf("rawmap_paths_test\n");
    if (!make_json("rawmap_test_archive.json", archive, sizeof archive)) {
        fprintf(stderr, "could not create the temp fixture\n");
        return 1;
    }

    a_chosen_file_stays_chosen();
    a_failed_write_keeps_its_destination();
    opening_a_rawmap_never_aims_saves_at_it(archive);
    choosing_the_opened_rawmap_works(archive);
    a_different_map_drops_the_target();
    a_bare_word_is_refused();
    picking_the_usual_file_holds_nothing();
    picking_the_usual_file_releases_a_chosen_one();
    looks_like_rawmap_tells_them_apart();
    refused_menu_save_changes_nothing();
    missing_overwrite_protection_refuses_opening(archive);
    the_retired_every_load_switch_is_refused();
    a_load_path_never_replaces_another_parse(archive);
    a_unicode_path_is_read_and_written();
    an_existing_scratch_file_survives();
    a_live_export_writes_once();
    a_live_export_is_pretty_when_asked();
    snapshots_visit_without_save_effects();
    large_sources_use_native_lengths();
    earlier_shape_maps_are_refused();
    a_staged_rawmap_in_an_earlier_shape_is_refused();
    a_reload_into_the_same_map_object_is_a_new_map();
    saves_carry_no_package_payload();
    a_rawmap_open_is_unsaved_until_its_first_save();
    a_rawmap_without_its_mods_never_reaches_the_engine();
    a_rawmap_whose_mods_the_store_has_is_offered_then_opens();
    a_held_zip_is_imported_once();
    a_rawmap_whose_mods_are_installed_loads();
    a_rawmap_open_takes_its_own_rows_and_then_its_own_id();
    a_save_with_mods_writes_the_zip_beside_the_json();
    a_refused_zip_tells_the_player_why();

    DeleteFileA(archive);
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
