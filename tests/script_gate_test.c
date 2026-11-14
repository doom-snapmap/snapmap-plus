/* script_gate_test.c -- the engine side of the mod-script gates
 * (src/backend/script/script_hooks.c and the serving provider of
 * script_source.c, built without SH_SCRIPT_PURE) against a faked engine:
 * the main-thread id, the load screen, the editor singleton and the frame
 * function's prologue are memory the test owns, the hook layer and the
 * serving index are stand-ins, and the runtime is real. Every map load and
 * every Play passes the gates:
 *   - a map with no mods, at the first gate of a session (nothing logged yet):
 *     both gates do nothing, without a fault, and no frame hook is prepared;
 *   - a map with a mod without scripts: the same;
 *   - a map with a scripted mod: the map gate prepares nothing, the Play gate
 *     prepares its scripts and installs the frame hook once, and the next map
 *     gate ends them;
 *   - a plain map after the scripted one does nothing again. */
#undef SH_SCRIPT_PURE
#include "../src/backend/script/script_hooks.c"
#include "../src/backend/script/script_source.c"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

/* ---- log ----------------------------------------------------------------- */

static char g_logbuf[16384];
void backend_log(const char *message)
{
    size_t n = strlen(g_logbuf);
    _snprintf_s(g_logbuf + n, sizeof g_logbuf - n, _TRUNCATE, "%s\n", message ? message : "");
}
static int log_has(const char *s) { return strstr(g_logbuf, s) != NULL; }

/* ---- the hook layer -------------------------------------------------------- */

static int g_prepared, g_committed;
static uint8_t g_tramp[64];
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; g_prepared++; return g_tramp; }
int hook_commit(void *trampoline) { (void)trampoline; g_committed++; return B2_PATCH_OK; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 1; }
int hook_is_installed(void *trampoline) { return trampoline == g_tramp && g_committed > 0; }
const char *hook_last_failure(void) { return ""; }
sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{ (void)module_base; (void)sig; memset(out, 0, sizeof *out); out->status = SIG_NOT_FOUND; return SIG_NOT_FOUND; }
uintptr_t glb_resolve(const uint8_t *base, const char *name, glb_status *status)
{ (void)base; (void)name; if (status) *status = GLB_UNKNOWN_NAME; return 0; }
void sh_feature_off(const char *feature, const char *why) { (void)feature; (void)why; }

/* ---- the rest of the engine side the gates never reach -------------------- */

void *sh_entity_gamemgr(void) { return NULL; }
int sh_companions_def_chain(void *entity, const char **names, int cap) { (void)entity; (void)names; (void)cap; return 0; }
int sh_companions_spawn_ready(void) { return 0; }
void sh_companions_set_spawn_subscriber(sh_spawn_subscriber_fn fn, sh_spawn_wanted_fn wanted) { (void)fn; (void)wanted; }
void sh_load_screen_stop_set_map_end(sh_map_end_fn fn) { (void)fn; }
int sh_script_api_install(const sig_result *results, size_t count, const uint8_t *module_base)
{ (void)results; (void)count; (void)module_base; return 0; }
int sh_script_combat_install(const sig_result *results, size_t count, const uint8_t *module_base)
{ (void)results; (void)count; (void)module_base; return 0; }
void sh_script_api_frame(void) {}
int sh_script_api_now_ms(int *ms) { if (ms) *ms = 0; return 0; }
const char *sh_script_combat_pain_name(int reaction) { (void)reaction; return ""; }
sh_script_handle sh_script_combat_launch(uint32_t projectile_spawn_id, sh_script_handle owner, int mod)
{ (void)projectile_spawn_id; (void)owner; (void)mod; return 0; }
int sh_script_combat_launch_from(sh_script_handle owner, float origin[3], float axis[9])
{ (void)owner; (void)origin; (void)axis; return 0; }
void sh_script_combat_note_hit(sh_script_handle victim, const char *decl, sh_script_handle attacker)
{ (void)victim; (void)decl; (void)attacker; }
int sh_script_combat_last_hit(sh_script_handle victim, char **decl, sh_script_handle *attacker)
{ (void)victim; if (decl) *decl = NULL; if (attacker) *attacker = 0; return 0; }
int sh_script_combat_first_death(sh_script_handle victim) { (void)victim; return 0; }
static int g_combat_resets;
void sh_script_combat_reset(void) { g_combat_resets++; }
static int g_teleport_resets;
void sh_ai_spawn_teleport_reset(void) { g_teleport_resets++; }
static int g_destructible_resets;
void sh_ai_destructible_reset(void) { g_destructible_resets++; }

/* ---- the serving index: one map's mods ------------------------------------ */

#define SALT "gatemod1"
#define HASH "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
static const char k_script[] = "api = 1\non.think(\"gate/thing\", function(self, dt) end)\n";
static int g_map_mods;      /* rows of the open map: 0 or 1 */
static int g_map_scripts;   /* the mod's scripts: 0 or 1 */

int sh_modserve_scope_hashes(char (*out)[SH_MODSERVE_HASH_HEX + 1], int cap)
{
    if (out && cap >= 1 && g_map_mods) strcpy_s(out[0], SH_MODSERVE_HASH_HEX + 1, HASH);
    return g_map_mods;
}
void sh_modserve_each_bundle(sh_modserve_bundle_fn fn, void *context)
{
    if (g_map_mods) fn(context, SALT, HASH, 1, NULL, 1, 0);
}
int sh_modserve_script_files(const char *salt, sh_modserve_script_fn fn, void *context)
{
    if (!g_map_scripts || _stricmp(salt, SALT)) return 0;
    if (fn) fn(context, "main.lua", "mods/" SALT "/scripts/main.lua", sizeof k_script - 1, "sum", HASH);
    return 1;
}
int sh_modserve_row_build(const struct sh_modmanifest_row *row, sh_modserve_bundle_ref *out)
{
    (void)row;
    memset(out, 0, sizeof *out);
    out->name = _strdup("Gate Mod");
    out->folder = _strdup("gate-mod");
    return 1;
}
void sh_modserve_bundle_ref_free(sh_modserve_bundle_ref *ref) { free(ref->name); free(ref->folder); free(ref->place); }
int sh_modserve_open(const char *request, unsigned char **bytes, size_t *length)
{
    (void)request;
    *bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, sizeof k_script - 1);
    memcpy(*bytes, k_script, sizeof k_script - 1);
    *length = sizeof k_script - 1;
    return 1;
}
int sh_modserve_open_disk(const char *request, unsigned char **bytes, size_t *length)
{
    return sh_modserve_open(request, bytes, length);
}
void sh_mb_sha256_hex(const void *data, size_t len, char out[SH_MB_HASH_HEX + 1])
{ (void)data; (void)len; strcpy_s(out, SH_MB_HASH_HEX + 1, "sum"); }

/* ---- the faked engine ------------------------------------------------------ */

static uint32_t g_main_id;
static const uint8_t *g_no_thread;        /* the load screen draws nothing */
static uint8_t g_editor_bytes[0x40];
static uint8_t g_tick_bytes[SH_SCRIPT_TICK_STOLEN];

static void fake_install(void)
{
    g_main_id = GetCurrentThreadId();
    memcpy(g_tick_bytes, k_script_tick_prologue, sizeof g_tick_bytes);
    g_tick_target = g_tick_bytes;
    g_main_tid = &g_main_id;
    g_load_screen = (const uint8_t *)&g_no_thread;
    g_editor = g_editor_bytes;
    sh_script_source_install();
    g_resolved = 1;
}

/* One gate; a fault is a failure of the gate, not of the test. */
static int gate(int which)
{
    __try { sh_script_hooks_gate(which); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

static void map(int mods, int scripts)
{
    g_map_mods = mods;
    g_map_scripts = scripts;
    g_logbuf[0] = 0;
}

int main(void)
{
    fake_install();

    /* A plain map is the first map of the session: nothing has been logged. */
    CHECK(g_log_last == NULL);
    map(0, 0);
    CHECK(gate(SH_SCRIPT_GATE_MAP_LOAD));
    CHECK(gate(SH_SCRIPT_GATE_PLAY));
    CHECK(!sh_script_hooks_open() && !g_prepared && !g_committed && !g_routing);
    CHECK(!log_has("SCRIPTS") && !log_has("Mod scripts"));
    CHECK(gate(SH_SCRIPT_GATE_MAP_LOAD));   /* and the next map */

    /* A map whose mod has no scripts. */
    map(1, 0);
    CHECK(gate(SH_SCRIPT_GATE_MAP_LOAD));
    CHECK(gate(SH_SCRIPT_GATE_PLAY));
    CHECK(!sh_script_hooks_open() && !g_prepared && !g_routing);
    CHECK(!log_has("SCRIPTS") && !log_has("Mod scripts"));

    /* A map with a scripted mod: prepared at Play, the frame hook installed once. */
    map(1, 1);
    CHECK(gate(SH_SCRIPT_GATE_MAP_LOAD));
    CHECK(!sh_script_hooks_open() && !g_prepared);
    CHECK(gate(SH_SCRIPT_GATE_PLAY));
    CHECK(sh_script_hooks_open() && g_prepared == 1 && g_committed == 1 && g_routing);
    CHECK(log_has("SCRIPTS: 1 mod(s) with scripts prepared at the Play build"));
    CHECK(gate(SH_SCRIPT_GATE_PLAY));        /* a second Play reuses the hook */
    CHECK(sh_script_hooks_open() && g_prepared == 1);
    /* The same line at the next map is said again: each map's log is whole. The map's spawn teleports and
     * destructibles are forgotten with it (the next map's scripts name their own). */
    g_logbuf[0] = 0;
    {
        int before = g_teleport_resets, dbefore = g_destructible_resets;
        CHECK(gate(SH_SCRIPT_GATE_MAP_LOAD));
        CHECK(g_teleport_resets > before);
        CHECK(g_destructible_resets > dbefore);
    }
    CHECK(!sh_script_hooks_open() && !g_routing);
    CHECK(gate(SH_SCRIPT_GATE_PLAY));
    CHECK(log_has("SCRIPTS: 1 mod(s) with scripts prepared at the Play build"));

    /* A plain map after it: the scripts end and nothing opens. */
    map(0, 0);
    CHECK(gate(SH_SCRIPT_GATE_MAP_LOAD));
    CHECK(!sh_script_hooks_open() && !g_routing);
    CHECK(gate(SH_SCRIPT_GATE_PLAY));
    CHECK(!sh_script_hooks_open() && g_prepared == 1);

    /* Another thread's call is ignored. */
    g_main_id = GetCurrentThreadId() + 1;
    map(1, 1);
    CHECK(gate(SH_SCRIPT_GATE_PLAY));
    CHECK(!sh_script_hooks_open());

    printf(g_fail ? "script_gate_test: FAILED\n" : "script_gate_test: all checks passed\n");
    return g_fail;
}
