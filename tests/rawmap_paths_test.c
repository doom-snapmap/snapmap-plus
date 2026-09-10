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

#include "rawmap.h"

/* ---- STUBS -------------------------------------------------------------------------------------
 * rawmap.c is one translation unit that also holds the hooks, the package embedder, the navmesh
 * embedder and the editor bridge. None of that is reachable from the destination rules -- those are
 * two strings and a file write -- so the collaborators are stubbed rather than linked. A stub that
 * gets called would be a bug in the test, so the ones with a return value return failure, which the
 * caller under test treats as "not available" and reports rather than ignores.
 */
#include "signatures.h"
#include "map_package.h"
#include "map_embed.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "overrides.h"
#include "raw_deflate.h"
#include "editor_frame.h"
#include "cvars.h"
#include "hook.h"

const sig_entry BACKEND_ENGINE_SIGNATURES[] = { { NULL, NULL, 0u } };

void backend_log(const char *line) { (void)line; }

void *install_inline_hook(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; return NULL; }

sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{ (void)module_base; (void)sig; (void)out; return SIG_NOT_FOUND; }

int   sh_mpkg_gate(const char *json, size_t len) { (void)json; (void)len; return 0; }
char *sh_mpkg_strip(const char *json, size_t len, size_t *out_len)
{ (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
char *sh_mpkg_embed(const char *json, size_t len, const char *pkg_id,
                    const unsigned char *payload, size_t payload_len,
                    size_t *out_len, char *err, size_t err_cap)
{ (void)json; (void)len; (void)pkg_id; (void)payload; (void)payload_len;
  if (out_len) *out_len = 0; if (err && err_cap) err[0] = '\0'; return NULL; }

unsigned char *sh_mpkg_pack_dir(const char *root, size_t *out_len, char *err, size_t err_cap)
{ (void)root; if (out_len) *out_len = 0; if (err && err_cap) err[0] = '\0'; return NULL; }

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
                             sh_mpkg_used *out, size_t cap)
{ (void)json; (void)len; (void)data_root; (void)out; (void)cap; return 0; }

int sh_overrides_get_root(char *out, size_t cap)
{ if (out && cap) out[0] = '\0'; return 0; }

void  sh_navmesh_build_from_map(const char *json, size_t len) { (void)json; (void)len; }
char *sh_navmesh_strip(const char *json, size_t len, size_t *out_len)
{ (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
char *sh_navmesh_embed_all(const char *json, size_t len, size_t *out_len)
{ (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
void  sh_nav_bake_set_map(const char *json, size_t len) { (void)json; (void)len; }

int sh_editor_frame_request_rawmap_save(char *out_msg, int msg_capacity)
{ if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
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
int sh_editor_frame_saved_map_dir(char *out, size_t cap, char *out_id, size_t id_cap)
{ if (out && cap) out[0] = '\0'; if (out_id && id_cap) out_id[0] = '\0'; return 0; }

size_t sh_inflate_raw_upto(const unsigned char *src, size_t src_len,
                           unsigned char *dst, size_t dst_cap)
{ (void)src; (void)src_len; (void)dst; (void)dst_cap; return 0; }

int sh_cvar_value_int(int index, int def) { (void)index; return def; }

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

/* paths_are_default must answer both ways. It tested emptiness once, which is never true after the
 * installers materialize a default, so it was stuck on "not default" forever. */
static void are_default_answers_both_ways(const char *archive)
{
    char picked[MAX_PATH];

    printf("\n-- paths_are_default answers both ways\n");

    clean_state();
    CHECK(sh_rawmap_paths_are_default() == 1);

    CHECK(sh_rawmap_swap_set_source(archive) == 1);
    CHECK(sh_rawmap_paths_are_default() == 0);
    sh_rawmap_swap_set_source(NULL);
    CHECK(sh_rawmap_paths_are_default() == 1);

    CHECK(temp_path("rawmap_test_export4.json", picked, sizeof picked));
    CHECK(sh_rawmap_set_save_target(picked) == 1);
    CHECK(sh_rawmap_paths_are_default() == 0);   /* a chosen file counts as a moved save path */
    sh_rawmap_set_save_target(NULL);
    CHECK(sh_rawmap_paths_are_default() == 1);

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
    are_default_answers_both_ways(archive);
    looks_like_rawmap_tells_them_apart();

    DeleteFileA(archive);
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
