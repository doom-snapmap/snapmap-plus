/* entity.c -- see entity.h. The entity/spawn console commands (sh_dumpdef,
 * sh_spawninfo, sh_spawn). Ports of OG XINPUT1_3 FUN_180021e60 / FUN_180024d90 / FUN_180021c90.
 *
 * The three handlers share sh_commands' console ABI (idCmdArgs / cmd_argv / sh_printf) + the global-
 * decode scanner (sh_decode_rip_slot / sh_safe_read) via commands.h, so there is ONE decode path and
 * ONE Printf wrapper across the whole command surface. The entity-specific engine deps (gameMgr global + the
 * +0x48/+0x498/+0x340 vtable slots + SpawnByEntityDef) are resolved/cached by sh_entity_install.
 *
 * Every engine deref is SEH-guarded and non-null gated -- a wrong/shifted build offset degrades to a
 * clean printed error, never a crash. sh_spawn (the only MUTATING handler) additionally GUARDS its
 * teleport against a bogus GetOrigin result (NaN / |coord|>1e6 -> skip the teleport) so a wrong +0x340
 * slot cannot teleport the player to garbage.
 *
 * Clean-room: ported from our own RE (the b2-t3-re foundation report). Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "entity.h"
#include "commands.h"
#include "clipboard.h"
#include "backend_log.h"
#include "dumpmap_path.h"

/* ------------------------------------------------------------------------ engine fn typedefs ------ */

/* SpawnByEntityDef(gameMgr, name, entitydef) -> idEntity* (engine 0x315AF0; sig "SpawnByEntityDef").
 * Arg order CONFIRMED by the b2-t3-re foundation (cross-confirmed live): param2 = the spawned-entity
 * NAME (argv[2]), param3 = the entityDef NAME (argv[1]). Returns the spawned idEntity* (or NULL). */
typedef void *(*spawn_by_def_fn)(void *gamemgr, const char *name, const char *entitydef);

/* The three vtable-slot virtual calls (build-portable INDICES, decompile-verified -- NOT in the source-
 * of-record). All __fastcall(self, ...). We read the fn pointer from *(*self + slot) under SEH. */
typedef void  (*exec_cmd_text_fn)(void *cmdsys, const char *text);          /* cmdSystem  +0x48 idx9   */
typedef void *(*find_entity_fn)(void *gamemgr, const char *name);          /* gameMgr    +0x498 idx147 */
typedef void  (*get_origin_fn)(void *ent, float *out_vec3);                /* idEntity   +0x340 idx104 */

/* sh_dumpmap (T5): MapGetter(gameMgr) -> the live SnapMap object (sig 0x31AD60); MapWriter(map, path)
 * writes it to a file (sig 0x182B740; no-ops off a v5 map + Printf's its own status). Both BLACK BOXES --
 * no struct offset in the clone's reach (the gameMgr+0x29a0b0 / map+0x38 reads are internal to them). */
typedef void *(*map_getter_fn)(void *gamemgr);
typedef int   (*map_writer_fn)(void *map, const char *path);

#define VSLOT_EXEC_CMD_TEXT  0x48
#define VSLOT_FIND_ENTITY    0x498
#define VSLOT_GET_ORIGIN     0x340

/* sh_dumpdef resolved-def-text chain (DIRECT, the engine's source-of-record idlib schema):
 *   idEntity.entityDef @ +0x6D0 -> idDeclEntityDef.entityStateWithInheritanceText(idStr) @ +0x130 ->
 *   idStr.data @ +0x10  ==>  ent+0x6d0 then +0x140 (0x130+0x10) = the def-text char*.
 * 2021 offsets MATCH the live schema byte-for-byte, but build-sensitive -> SEH-guarded + non-null. */
#define ENT_ENTITYDEF_OFF    0x6d0
#define ENTITYDEF_TEXT_OFF   0x140

/* gameMgr known-offset FALLBACK (OG *(engineBase+0x56ffb90)); used only if the GameMgrLea decode fails. */
#define GAMEMGR_KNOWN_RVA    0x56ffb90u

/* ------------------------------------------------------------------------- module state ----------- */

/* We cache the gameMgr global SLOT (the address of the pointer), NOT the deref'd object: the idGameLocal
 * game/entity manager is constructed only when a game/map loads, so *(slot) is NULL at our early
 * deferred-install time (cmdSystem exists at startup, gameMgr does not). The handlers deref the slot
 * LAZILY (get_gamemgr) at invocation time, when a map/playtest is live. */
static const uint8_t   *g_gamemgr_slot = NULL;   /* address of the gameMgr global pointer (deref lazily) */
static void            *g_cmdsys       = NULL;   /* reused from sh_resolve_cmdsys (for ExecuteCommandText) */
static spawn_by_def_fn  g_spawn_by_def = NULL;   /* SpawnByEntityDef (sig 0x315AF0) */
static map_getter_fn    g_map_getter   = NULL;   /* MapGetter (sig 0x31AD60) -- T5 sh_dumpmap */
static map_writer_fn    g_map_writer   = NULL;   /* MapWriter (sig 0x182B740) -- T5 sh_dumpmap */
static volatile LONG    g_installed    = 0;      /* one-shot install latch */

/* ------------------------------------------------------------------ gameMgr-global decode ---------
 * REUSE the shared sh_decode_rip_slot (commands.c) -- the GameMgrLea accessor's prologue is
 * `MOV RAX,[rip+gameMgr]` (48 8B 05 at byte offset 0), which the 4-opcode scanner catches. Return the
 * SLOT (the global's address); we do NOT deref-and-cache here because the manager is NULL this early.
 * Fallback: module_base + 0x56ffb90. Accept any READABLE slot (the value may legitimately be NULL now). */
const uint8_t *sh_resolve_gamemgr_slot(const sig_result *results, size_t n, const uint8_t *module_base)
{
    void *accessor = (void *)sig_addr_by_name(results, n, "GameMgrLea");
    if (accessor) {
        const uint8_t *slot = sh_decode_rip_slot((const uint8_t *)accessor);
        if (slot) {
            void *probe = NULL;
            if (sh_safe_read(slot, (uint8_t *)&probe, sizeof probe)) {   /* readable; value may be NULL now */
                char line[128];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: gameMgr slot decoded=%p (current=%p, lazy-deref) (portable)", (void *)slot, probe);
                backend_log(line);
                return slot;
            }
        }
        backend_log("B2: gameMgr portable decode failed -- trying known-offset fallback");
    }
    if (module_base) {
        const uint8_t *slot = module_base + GAMEMGR_KNOWN_RVA;
        void *probe = NULL;
        if (sh_safe_read(slot, (uint8_t *)&probe, sizeof probe)) {
            char line[128];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: gameMgr slot fallback=base+0x56ffb90=%p (current=%p, lazy-deref)", (void *)slot, probe);
            backend_log(line);
            return slot;
        }
    }
    backend_log("B2: gameMgr slot UNRESOLVED -- entity commands cannot fire");
    return NULL;
}

/* Lazily deref the gameMgr slot. The manager is non-NULL only once a game/map is live (editor playtest /
 * loaded map) -- in the bare shell it is NULL and the handlers report "not available". SEH-guarded. */
static void *get_gamemgr(void)
{
    if (!g_gamemgr_slot) return NULL;
    void *obj = NULL;
    if (sh_safe_read(g_gamemgr_slot, (uint8_t *)&obj, sizeof obj)) return obj;
    return NULL;
}

/* ----------------------------------------------------------- SEH-guarded vtable-slot helpers ------
 * Read the fn ptr from *(*self + slot) and call it, all under SEH. A wrong slot index, a NULL self, or
 * an unreadable vtable degrades to a no-op (the *_ok out-param reports success to the caller). */

static void *read_vfn(void *self, size_t slot)
{
    __try {
        if (!self) return NULL;
        const uint8_t *vtbl = *(const uint8_t * const *)self;          /* *self = the vtable */
        if (!vtbl) return NULL;
        return *(void * const *)(vtbl + slot);                          /* vtbl[slot/8] */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* gameMgr +0x498 FindEntity(self, name) -> idEntity*. Returns NULL on any fault / missing slot. */
static void *gm_find_entity(void *gm, const char *name)
{
    if (!gm || !name) return NULL;
    find_entity_fn fn = (find_entity_fn)read_vfn(gm, VSLOT_FIND_ENTITY);
    if (!fn) return NULL;
    __try {
        return fn(gm, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* idEntity +0x340 GetOrigin(self, &vec3[3]). Returns 1 if the call ran (out is then filled), 0 on
 * fault / missing slot. out is pre-zeroed so a fault leaves a defined (and bogus-guard-rejected) value. */
static int ent_get_origin(void *ent, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!ent) return 0;
    get_origin_fn fn = (get_origin_fn)read_vfn(ent, VSLOT_GET_ORIGIN);
    if (!fn) return 0;
    __try {
        fn(ent, out);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = out[1] = out[2] = 0.0f;
        return 0;
    }
}

/* cmdSystem +0x48 ExecuteCommandText(self, text). Returns 1 if the call ran, 0 on fault / missing slot. */
static int cmd_exec_text(void *cmdsys, const char *text)
{
    if (!cmdsys || !text) return 0;
    exec_cmd_text_fn fn = (exec_cmd_text_fn)read_vfn(cmdsys, VSLOT_EXEC_CMD_TEXT);
    if (!fn) return 0;
    __try {
        fn(cmdsys, text);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* SEH-guarded two-hop read of the resolved-def-text char* (ent+0x6d0 -> +0x140). NULL on any fault. */
static const char *ent_def_text(void *ent)
{
    __try {
        if (!ent) return NULL;
        void *def = *(void * const *)((const uint8_t *)ent + ENT_ENTITYDEF_OFF);
        if (!def) return NULL;
        const char *txt = *(const char * const *)((const uint8_t *)def + ENTITYDEF_TEXT_OFF);
        return txt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* A coordinate is "bogus" if it is NaN/Inf or absurdly large -- a wrong GetOrigin slot would read a
 * vtable ptr / object field as a float and yield exactly such garbage. */
static int coord_is_bogus(float v)
{
    if (v != v) return 1;                 /* NaN */
    if (!(v > -3.0e38f && v < 3.0e38f)) return 1;  /* +/-Inf or out of float range */
    if (v > 1.0e6f || v < -1.0e6f) return 1;       /* implausible for a map coordinate */
    return 0;
}

/* ----------------------------------------------------------------------------- handlers ----------
 * Non-static (extern-declared in commands.c) so CMD_TABLE references them directly. */

/* [9] sh_dumpdef <entity name> (READ-ONLY) -- find the live entity, read its resolved entityDef text
 * (ent+0x6d0 -> +0x140), print it + copy to the clipboard. Port of OG FUN_180021e60. */
void h_sh_dumpdef(idCmdArgs *a)
{
    const char *name = cmd_argv(a, 1);
    if (name == NULL) {
        sh_printf("usage: sh_dumpdef <entity name>\n");
        return;
    }
    void *gm = get_gamemgr();
    if (gm == NULL) {
        sh_printf("sh_dumpdef: gameMgr not available -- load a map / start a playtest first.\n");
        return;
    }

    void *ent = gm_find_entity(gm, name);
    if (ent == NULL) {
        sh_printf("sh_dumpdef: no entity named '%s'.\n", name);
        return;
    }

    const char *txt = ent_def_text(ent);
    if (txt == NULL) {
        sh_printf("sh_dumpdef: entity '%s' has no resolved entityDef text.\n", name);
        return;
    }

    sh_printf("%s\n", txt);
    if (sh_clipboard_set(txt))
        sh_printf("sh_dumpdef: copied the entityDef of '%s' to the clipboard.\n", name);
}

/* [21] sh_spawninfo (READ-ONLY) -- run the engine `getviewpos` (writes the current view pos/orientation
 * to the clipboard), read it back ("x y z pitch yaw"), build a mat3 from pitch/yaw, format the
 * spawnOrientation/spawnPosition decl text, copy it to the clipboard + print it. Port of OG FUN_180024d90.
 *
 * The mat3 is built with the idTech angle convention: yaw about +Z, pitch about +Y (forward = +X),
 * column-major rows printed as the engine reads a mat3 decl. (The getviewpos 5-float clipboard format
 * is flagged to live-confirm at FIRE -- if it differs, only the parse below changes, not the offsets.) */
void h_sh_spawninfo(idCmdArgs *a)
{
    (void)a;
    if (g_cmdsys == NULL) {
        sh_printf("sh_spawninfo: cmdSystem unresolved -- cannot run getviewpos.\n");
        return;
    }

    if (!cmd_exec_text(g_cmdsys, "getviewpos")) {
        sh_printf("sh_spawninfo: getviewpos dispatch failed.\n");
        return;
    }

    char clip[512];
    if (!sh_clipboard_get(clip, (int)sizeof clip)) {
        sh_printf("sh_spawninfo: could not read getviewpos result from the clipboard.\n");
        return;
    }

    float x = 0, y = 0, z = 0, pitch = 0, yaw = 0;
    if (sscanf_s(clip, "%f %f %f %f %f", &x, &y, &z, &pitch, &yaw) < 5) {
        sh_printf("sh_spawninfo: unexpected getviewpos format ('%s').\n", clip);
        return;
    }

    /* angles (deg) -> mat3, EXACTLY as OG FUN_180024d90 (a 3-angle idAngles::ToMat3 with roll hardcoded
     * to 0: sinf(0)/cosf(0)). A = pitch (4th float), B = yaw (5th float). Derived element-for-element from
     * the OG decompile (DAT_180038830 = the 0x80000000 float sign-mask = negate):
     *   mat[0] = { cB*cA,  cB*sA,  -sB }
     *   mat[1] = { -sA,    cA,     0   }
     *   mat[2] = { sB*cA,  sB*sA,  cB  }   (roll=0 collapses OG's full 3-angle form to this). */
    const double DEG2RAD = 3.14159265358979323846 / 180.0;
    double sA = sin(pitch * DEG2RAD), cA = cos(pitch * DEG2RAD);   /* A = pitch (4th) */
    double sB = sin(yaw   * DEG2RAD), cB = cos(yaw   * DEG2RAD);   /* B = yaw   (5th) */

    float m00 = (float)(cB * cA),   m01 = (float)(cB * sA),   m02 = (float)(-sB);
    float m10 = (float)(-sA),       m11 = (float)(cA),        m12 = 0.0f;
    float m20 = (float)(sB * cA),   m21 = (float)(sB * sA),   m22 = (float)(cB);

    /* OG format string VERBATIM (cmd_0x24d90 L72): note mat[1]'s line is "z=%f" (no spaces) where mat[0]
     * and mat[2] are "z = %f"; OG writes a fixed 0x800 buffer and ends at "}" (no trailing newline). */
    char out[0x800];
    _snprintf_s(out, sizeof out, _TRUNCATE,
        "spawnOrientation = {\n\tmat = {\n"
        "\t\tmat[0] = {\n\t\t\tx = %f;\n\t\t\ty = %f;\n\t\t\tz = %f;\n\t\t}\n"
        "\t\tmat[1] = {\n\t\t\tx = %f;\n\t\t\ty = %f;\n\t\t\tz=%f;\n\t\t}\n"
        "\t\tmat[2] = {\n\t\t\tx = %f;\n\t\t\ty = %f;\n\t\t\tz = %f;\n\t\t}\n"
        "\t}\n}\nspawnPosition = {\n\tx = %f;\n\ty = %f;\n\tz = %f;\n}",
        m00, m01, m02, m10, m11, m12, m20, m21, m22, x, y, z);

    sh_printf("%s", out);
    if (sh_clipboard_set(out))
        sh_printf("sh_spawninfo: copied spawnOrientation/spawnPosition to the clipboard.\n");
}

/* [8] sh_spawn <entitydef> <entity name after spawning> (MUTATING) -- spawn an entityDef by name at the
 * player, then teleport the new entity onto the player's origin. Port of OG FUN_180021c90.
 *   FindEntity("player1") -> SpawnByEntityDef(gameMgr, name=argv[2], def=argv[1]) ->
 *   GetOrigin(spawned, &v) -> [GUARD bogus v] -> ExecuteCommandText("ai_ScriptCmdEnt %s teleport ...").
 * The teleport is GUARDED: if any origin coordinate is NaN/Inf/implausible (which a WRONG GetOrigin slot
 * would produce by reading a non-float as a float), we SKIP the teleport + warn, so we never teleport to
 * garbage. Every engine deref is SEH-guarded. */
void h_sh_spawn(idCmdArgs *a)
{
    const char *entitydef = cmd_argv(a, 1);
    const char *spawnname = cmd_argv(a, 2);
    if (entitydef == NULL || spawnname == NULL) {
        sh_printf("usage: sh_spawn <entitydef> <entity name after spawning>\n");
        return;
    }
    void *gm = get_gamemgr();
    if (gm == NULL) {
        sh_printf("sh_spawn: gameMgr not available -- load a map / start a playtest first.\n");
        return;
    }
    if (g_spawn_by_def == NULL) {
        sh_printf("sh_spawn: SpawnByEntityDef unresolved -- cannot spawn.\n");
        return;
    }

    /* OG order (cmd_0x21c90): FindEntity("player1") FIRST, then SpawnByEntityDef UNCONDITIONALLY, then
     * teleport ONLY if (player1 != NULL && spawned != NULL) -- the teleport brings the spawned entity to
     * the PLAYER's origin (GetOrigin is read on player1, NOT on the spawned entity). */
    void *player = gm_find_entity(gm, "player1");

    void *spawned = NULL;
    __try {
        spawned = g_spawn_by_def(gm, spawnname, entitydef);   /* (gameMgr, name=argv[2], def=argv[1]) */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spawned = NULL;
    }
    if (spawned == NULL) {
        sh_printf("sh_spawn: SpawnByEntityDef('%s' as '%s') returned NULL.\n", entitydef, spawnname);
        return;
    }
    sh_printf("sh_spawn: spawned entityDef '%s' as '%s'.\n", entitydef, spawnname);

    /* OG gate: no teleport unless player1 was found (matches FUN_180021c90's `if (player1 && spawned)`). */
    if (player == NULL) {
        sh_printf("sh_spawn: player1 not found -- spawned but not teleporting (OG-faithful).\n");
        return;
    }

    /* GetOrigin on the PLAYER (player1), per OG -- the spawned entity is teleported TO the player. */
    float v[3];
    if (!ent_get_origin(player, v)) {
        sh_printf("sh_spawn: GetOrigin(player1) failed -- skipping teleport.\n");
        return;
    }

    /* GUARD: a wrong +0x340 slot would read garbage as the origin -> never teleport to it. */
    if (coord_is_bogus(v[0]) || coord_is_bogus(v[1]) || coord_is_bogus(v[2])) {
        sh_printf("sh_spawn: GetOrigin(player1) returned a bogus position (%f %f %f) -- skipping teleport "
                  "(GetOrigin slot may be wrong on this build).\n", v[0], v[1], v[2]);
        return;
    }

    if (g_cmdsys == NULL) {
        sh_printf("sh_spawn: cmdSystem unresolved -- spawned but cannot teleport.\n");
        return;
    }

    char cmd[256];
    _snprintf_s(cmd, sizeof cmd, _TRUNCATE,
        "ai_ScriptCmdEnt %s teleport %f %f %f", spawnname, v[0], v[1], v[2]);
    if (cmd_exec_text(g_cmdsys, cmd))
        sh_printf("sh_spawn: teleported '%s' to the player at (%f %f %f).\n", spawnname, v[0], v[1], v[2]);
    else
        sh_printf("sh_spawn: teleport dispatch failed.\n");
}

/* sh_dumpmap output-path resolution (ours, not in the original) -------------------------------------
 *
 * The writer takes a GAME-RELATIVE path rooted at <game dir>\base\ and gives no clue where the file
 * landed: it Printf's "writing %s..." BEFORE the open and prints nothing at all on success, so a
 * perfectly good dump reads as a command that did nothing. It also silently CLOBBERS a previous dump of
 * the same name, and forces the extension, so the name on disk is not necessarily the one that was
 * typed. We resolve the destination ourselves first -- default subdirectory, collision-free filename --
 * hand the writer that path, then STAT the result so the line we print is a file we have actually seen.
 *
 * The string half of the resolution lives in dumpmap_path.h (pure, unit-tested off-game); what is left
 * here is everything that touches the disk. */
static BOOL dumpmap_base_dir(char *out, size_t outcap)
{
    char exe[MAX_PATH] = {0};
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return FALSE;
    char *slash = strrchr(exe, '\\');
    if (!slash) return FALSE;
    *slash = '\0';
    _snprintf_s(out, outcap, _TRUNCATE, "%s\\base", exe);
    return TRUE;
}

static BOOL dumpmap_file_size(const char *ospath, unsigned long long *size_out)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(ospath, GetFileExInfoStandard, &fad)) return FALSE;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return FALSE;
    if (size_out) *size_out = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return TRUE;
}

/* mkdir -p over every directory component (the last component is the file). The file system may well
 * create these itself; doing it here makes a typed subdirectory behave the same either way. Every
 * failure is ignored on purpose -- an already-existing directory and a bare drive prefix both fail. */
static void dumpmap_make_dirs(char *ospath)
{
    for (char *p = ospath; *p; p++) {
        if (*p != '\\') continue;
        *p = '\0';
        CreateDirectoryA(ospath, NULL);
        *p = '\\';
    }
}

/* Resolve the typed name into the game-relative path we hand the writer (`rel`) and the OS path it
 * lands at (`os`). `base` may be "" when the game directory could not be located: we then skip the
 * collision check and leave `os` empty, degrading to the original pass-the-name-through behaviour.
 * FALSE = the name is unusable; *why is then a one-line reason fit to print. */
static BOOL dumpmap_resolve(const char *arg, const char *base, char *rel, size_t relcap,
                            char *os, size_t oscap, const char **why)
{
    *why = NULL;
    os[0] = '\0';

    if (!dumpmap_validate(arg, why))
        return FALSE;

    char stem[MAX_PATH];
    if (!dumpmap_stem(arg, stem, sizeof stem)) {
        *why = "that name has no filename in it";
        return FALSE;
    }

    if (base[0] == '\0') {
        dumpmap_candidate(stem, 1, rel, relcap);
        return TRUE;
    }

    /* Never clobber an earlier dump: name.map -> name_2.map -> name_3.map -> ... */
    for (int seq = 1; seq <= DUMPMAP_MAX_SEQ; seq++) {
        dumpmap_candidate(stem, seq, rel, relcap);
        dumpmap_ospath(base, rel, os, oscap);
        if (!dumpmap_file_size(os, NULL))
            return TRUE;
    }
    *why = "too many dumps already saved under that name";
    return FALSE;
}

/* [6] sh_dumpmap <mapfile> (T5, READ-ONLY-ish: writes a file) -- dump the live SnapMap to a file. Port of
 * OG FUN_180021c20: argc<2 -> "You need to provide a mapfile to write to"; map = MapGetter(gameMgr);
 * ok = MapWriter(map, argv[1]); if (!ok) "Failed to write map file <path>". MapWriter no-ops off a v5 map
 * + Printf's its own "writing %s..." status. OG passes MapGetter's result straight into MapWriter (no null
 * check); we SEH-guard both calls + null-gate the map so a no-map context degrades to a clean line, not a crash. */
void h_sh_dumpmap(idCmdArgs *a)
{
    const char *path = cmd_argv(a, 1);
    if (path == NULL) {
        sh_printf("You need to provide a mapfile to write to\n");   /* OG verbatim */
        sh_printf("usage: sh_dumpmap <name>  -- writes <game dir>\\base\\%s\\<name>.map\n", DUMPMAP_SUBDIR);
        return;
    }
    void *gm = get_gamemgr();
    if (gm == NULL) {
        sh_printf("sh_dumpmap: gameMgr not available -- load a map / start a playtest first.\n");
        return;
    }
    if (g_map_getter == NULL || g_map_writer == NULL) {
        sh_printf("sh_dumpmap: MapGetter/MapWriter unresolved.\n");
        return;
    }

    void *map = NULL;
    __try { map = g_map_getter(gm); }
    __except (EXCEPTION_EXECUTE_HANDLER) { map = NULL; }
    if (map == NULL) {
        sh_printf("sh_dumpmap: no active map.\n");
        return;
    }

    char base[MAX_PATH] = {0};
    char rel[MAX_PATH]  = {0};
    char os[MAX_PATH]   = {0};
    const char *why = NULL;
    if (!dumpmap_base_dir(base, sizeof base))
        base[0] = '\0';                     /* unknown game dir -> no collision check, no full path */
    if (!dumpmap_resolve(path, base, rel, sizeof rel, os, sizeof os, &why)) {
        sh_printf("sh_dumpmap: %s.\n", why);
        sh_printf("usage: sh_dumpmap <name>  -- writes <game dir>\\base\\%s\\<name>.map\n", DUMPMAP_SUBDIR);
        return;
    }
    if (os[0] != '\0')
        dumpmap_make_dirs(os);

    int ok = 0;
    __try { ok = g_map_writer(map, rel); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) {
        sh_printf("Failed to write map file %s\n", rel);   /* OG verbatim; MapWriter also Printf's "writing %s..." */
        return;
    }

    unsigned long long bytes = 0;
    if (os[0] != '\0' && dumpmap_file_size(os, &bytes))
        sh_printf("sh_dumpmap: wrote %s (%llu bytes)\n", os, bytes);
    else
        sh_printf("sh_dumpmap: wrote '%s' (game-relative -- look under <game dir>\\base\\)\n", rel);
}

/* ------------------------------------------------- player cheat commands (OG / DLM parity) ----------
 * OG SnapHak ships emoose's DoomLegacyMod (dinput8), which ADDS 5 custom console commands the stock SnapMap
 * editor lacks: noClip / infiniteHealth / noPlayerDeath / noPlayerKill / noTarget. Each is a pure TOGGLE of
 * one runtime bit on the local idPlayer's cheat-flags byte (DLM builds an idClientGameMsg_PlayerCommand_*
 * whose server setter just flips that bit). We reproduce them clean-room: resolve the local player the same
 * way sh_spawn does -- FindEntity("player1") -- and XOR the bit directly.
 *
 * THE ENFORCEMENT BITS (what the engine actually reads). DIRECT from the 5 message setters
 * (DOOM 0x14035fa50..0x14035fb50: `movzx eax,[rdx+OFF]; <toggle BIT>; mov [rdx+OFF],cl`) + the godMode
 * enforcement getter 0x140b72ca0 (`*(byte*)(player+0x45ea8) & 2`). TRAP: idPlayer's reflected bool PROPERTIES
 * (godMode/infiniteHealth/...) touch the bit ONE LOWER (runtime = property<<1) and are NOT what enforcement
 * reads -- we toggle the RUNTIME bit.
 *
 * BUILD-SPECIFIC OFFSETS (re-derive per DOOM build -- portability discipline; idPlayer struct offsets, NOT
 * sig-resolvable). Recipe: disasm the 5 setters above for OFF+BIT; cross-check the flags byte via the godMode
 * getter 0x140b72ca0, or reflection (FindTypeInfoByName("idPlayer") field "godMode" -> the flags byte). */
#define PLAYER_CHEAT_FLAGS_OFF  0x45ea8u   /* infiniteHealth 0x04 / noPlayerDeath 0x08 / noPlayerKill 0x10 / noTarget 0x20 (godMode 0x02) */
#define PLAYER_NOCLIP_OFF       0x0ce46u   /* noclip = bit 0x04 in a SEPARATE idPlayer byte */
#define CHEAT_BIT_INFHEALTH     0x04u
#define CHEAT_BIT_NOPLYDEATH    0x08u
#define CHEAT_BIT_NOPLYKILL     0x10u
#define CHEAT_BIT_NOTARGET      0x20u
#define CHEAT_BIT_NOCLIP        0x04u

/* SEH-guarded pure toggle of `bit` in the idPlayer byte at `field_off` (the OG/DLM setters ignore the arg).
 * Resolves the local player exactly as sh_spawn does (FindEntity("player1")); needs a live map/playtest. */
static void cheat_toggle(uint32_t field_off, uint8_t bit, const char *label)
{
    void *gm = get_gamemgr();
    if (gm == NULL) {
        sh_printf("%s: load a map / start a playtest first.\n", label);
        return;
    }
    void *player = gm_find_entity(gm, "player1");
    if (player == NULL) {
        sh_printf("%s: no local player (start a playtest first).\n", label);
        return;
    }
    int on = 0, ok = 0;
    __try {
        uint8_t *p = (uint8_t *)player + field_off;
        *p ^= bit;
        on = (*p & bit) != 0;
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (ok)
        sh_printf("%s %s\n", label, on ? "ON" : "OFF");
    else
        sh_printf("%s: player cheat-flags unreadable (offset may be wrong on this build).\n", label);
}

void h_noclip(idCmdArgs *a)         { (void)a; cheat_toggle(PLAYER_NOCLIP_OFF,      CHEAT_BIT_NOCLIP,     "noClip"); }
void h_infinitehealth(idCmdArgs *a) { (void)a; cheat_toggle(PLAYER_CHEAT_FLAGS_OFF, CHEAT_BIT_INFHEALTH,  "infiniteHealth"); }
void h_noplayerdeath(idCmdArgs *a)  { (void)a; cheat_toggle(PLAYER_CHEAT_FLAGS_OFF, CHEAT_BIT_NOPLYDEATH, "noPlayerDeath"); }
void h_noplayerkill(idCmdArgs *a)   { (void)a; cheat_toggle(PLAYER_CHEAT_FLAGS_OFF, CHEAT_BIT_NOPLYKILL,  "noPlayerKill"); }
void h_notarget(idCmdArgs *a)       { (void)a; cheat_toggle(PLAYER_CHEAT_FLAGS_OFF, CHEAT_BIT_NOTARGET,   "noTarget"); }

/* ------------------------------------------------------------------------------- install ---------- */

int sh_entity_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;   /* one-shot */
    if (module_base == NULL) {
        backend_log("B2: entity install SKIPPED -- module base NULL");
        return 0;
    }

    g_gamemgr_slot = sh_resolve_gamemgr_slot(results, n, module_base);
    g_cmdsys       = cmdsys;                                              /* reuse sh_commands' decode */
    g_spawn_by_def = (spawn_by_def_fn)sig_addr_by_name(results, n, "SpawnByEntityDef");
    g_map_getter   = (map_getter_fn)sig_addr_by_name(results, n, "MapGetter");
    g_map_writer   = (map_writer_fn)sig_addr_by_name(results, n, "MapWriter");

    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: entity install -- gameMgr slot=%p (lazy-deref) cmdSystem=%p SpawnByEntityDef=%p MapGetter=%p MapWriter=%p "
        "(sh_dumpdef/sh_spawninfo/sh_spawn/sh_dumpmap wired; GetOrigin-on-player; teleport bogus-vec3 guarded)",
        (void *)g_gamemgr_slot, g_cmdsys, (void *)g_spawn_by_def, (void *)g_map_getter, (void *)g_map_writer);
    backend_log(line);
    return 1;
}
