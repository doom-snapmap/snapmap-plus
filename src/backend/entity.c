/* Entity, map-export, spawn-position, and player-cheat console commands.
 * Engine dependencies are cached at installation; the live game manager is read
 * on invocation. Engine access faults are caught and reported by each handler. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "entity.h"
#include "commands.h"
#include "clipboard.h"
#include "engine_globals.h"
#include "host_image.h"
#include "backend_log.h"
#include "dumpmap_path.h"

/* Engine call contracts. */

/* Engine argument order is spawned name, then entityDef name. */
typedef void *(*spawn_by_def_fn)(void *gamemgr, const char *name, const char *entitydef);

/* Byte offsets in engine vtables; validate these slots when porting builds. */
typedef void  (*exec_cmd_text_fn)(void *cmdsys, const char *text);          /* cmdSystem  +0x48 idx9   */
typedef void *(*find_entity_fn)(void *gamemgr, const char *name);          /* gameMgr    +0x498 idx147 */
typedef void  (*get_origin_fn)(void *ent, float *out_vec3);                /* idEntity   +0x340 idx104 */

/* MapGetter returns the current map; MapWriter handles supported map serialization. */
typedef void *(*map_getter_fn)(void *gamemgr);
typedef int   (*map_writer_fn)(void *map, const char *path);

#define VSLOT_EXEC_CMD_TEXT  0x48
#define VSLOT_FIND_ENTITY    0x498
#define VSLOT_GET_ORIGIN     0x340

/* Resolved text: entity+0x6D0 -> entityDef; idStr@+0x130 has data@+0x10. */
#define ENT_ENTITYDEF_OFF    0x6d0
#define ENTITYDEF_TEXT_OFF   0x140

/* Pinned Vulkan fallback; host_image requires a verified executable hash. */
#define GAMEMGR_KNOWN_RVA    0x56ffb90u

/* Cached dependencies. */

/* Cache the singleton slot, not its value: the game manager can be NULL at
 * startup and is read lazily after a map loads. */
static const uint8_t   *g_gamemgr_slot = NULL;
static void            *g_cmdsys       = NULL;
static spawn_by_def_fn  g_spawn_by_def = NULL;
static map_getter_fn    g_map_getter   = NULL;
static map_writer_fn    g_map_writer   = NULL;
static volatile LONG    g_installed    = 0;

/* Decode GameMgrLea or an independent global anchor. Return the readable
 * pointer slot even when its current value is NULL. */
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
        backend_log("B2: gameMgr portable decode failed -- trying the signed data-global anchor");
    }
    /* Independent anchor survives a detour on the GameMgrLea prologue. */
    if (module_base) {
        glb_status gst = GLB_UNKNOWN_NAME;
        uintptr_t decoded = glb_resolve(module_base, "game_manager_slot", &gst);
        if (decoded) {
            const uint8_t *slot = (const uint8_t *)decoded;
            void *probe = NULL;
            if (sh_safe_read(slot, (uint8_t *)&probe, sizeof probe)) {
                char line[128];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: gameMgr slot glb=%p (current=%p, lazy-deref) (portable)", (void *)slot, probe);
                backend_log(line);
                return slot;
            }
        } else {
            char line[128];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: gameMgr glb anchor unresolved (status=%d)", (int)gst);
            backend_log(line);
        }
    }
    /* Last resort: pinned Vulkan RVA, allowed by the filename gate only. */
    if (module_base && sh_host_is_pinned_rva_build()) {
        const uint8_t *slot = module_base + GAMEMGR_KNOWN_RVA;
        void *probe = NULL;
        if (sh_safe_read(slot, (uint8_t *)&probe, sizeof probe)) {
            char line[128];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: gameMgr slot pinned-build fallback=base+0x56ffb90=%p (current=%p, lazy-deref)",
                (void *)slot, probe);
            backend_log(line);
            return slot;
        }
    }
    backend_log("B2: gameMgr slot UNRESOLVED -- entity commands cannot fire");
    return NULL;
}

/* Read the manager at invocation; a bare shell may have no live game. */
static void *get_gamemgr(void)
{
    if (!g_gamemgr_slot) return NULL;
    void *obj = NULL;
    if (sh_safe_read(g_gamemgr_slot, (uint8_t *)&obj, sizeof obj)) return obj;
    return NULL;
}

/* Guarded engine vtable calls. */

static void *read_vfn(void *self, size_t slot)
{
    __try {
        if (!self) return NULL;
        const uint8_t *vtbl = *(const uint8_t * const *)self;
        if (!vtbl) return NULL;
        return *(void * const *)(vtbl + slot);
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

/* Return 1 after GetOrigin succeeds, or 0 for a missing slot or memory fault. */
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

/* Reject non-finite or implausibly large coordinates before teleporting. */
static int coord_is_bogus(float v)
{
    if (v != v) return 1;                 /* NaN */
    if (!(v > -3.0e38f && v < 3.0e38f)) return 1;  /* +/-Inf or out of float range */
    if (v > 1.0e6f || v < -1.0e6f) return 1;       /* implausible for a map coordinate */
    return 0;
}

/* Console handlers registered in commands.c. */

/* Print and copy a named live entity's resolved definition. */
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

/* Read getviewpos clipboard output as x y z pitch yaw, then format the
 * spawnPosition and spawnOrientation declaration fields. */
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

    /* Preserve the command's matrix convention with roll fixed to zero. */
    const double DEG2RAD = 3.14159265358979323846 / 180.0;
    double sA = sin(pitch * DEG2RAD), cA = cos(pitch * DEG2RAD);
    double sB = sin(yaw   * DEG2RAD), cB = cos(yaw   * DEG2RAD);

    float m00 = (float)(cB * cA),   m01 = (float)(cB * sA),   m02 = (float)(-sB);
    float m10 = (float)(-sA),       m11 = (float)(cA),        m12 = 0.0f;
    float m20 = (float)(sB * cA),   m21 = (float)(sB * sA),   m22 = (float)(cB);

    /* Preserve the original output spacing and lack of a trailing newline. */
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

/* Spawn a named entity, then teleport it to player1 if its origin is readable
 * and plausible. A missing player does not prevent the spawn itself. */
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

    /* Find player1 first, but spawn even if the lookup misses. */
    void *player = gm_find_entity(gm, "player1");

    void *spawned = NULL;
    __try {
        spawned = g_spawn_by_def(gm, spawnname, entitydef);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spawned = NULL;
    }
    if (spawned == NULL) {
        sh_printf("sh_spawn: SpawnByEntityDef('%s' as '%s') returned NULL.\n", entitydef, spawnname);
        return;
    }
    sh_printf("sh_spawn: spawned entityDef '%s' as '%s'.\n", entitydef, spawnname);


    if (player == NULL) {
        sh_printf("sh_spawn: player1 not found -- spawned but not teleporting (OG-faithful).\n");
        return;
    }

    /* Read the destination from the player, not the newly spawned entity. */
    float v[3];
    if (!ent_get_origin(player, v)) {
        sh_printf("sh_spawn: GetOrigin(player1) failed -- skipping teleport.\n");
        return;
    }


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

/* MapWriter uses paths relative to base/, forces the extension, and can overwrite
 * existing dumps. Choose a free name and inspect the written file to report its
 * actual destination. Pure path rules live in dumpmap_path.h. */
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

/* Create parent directories; existing paths and drive prefixes may fail harmlessly. */
static void dumpmap_make_dirs(char *ospath)
{
    for (char *p = ospath; *p; p++) {
        if (*p != '\\') continue;
        *p = '\0';
        CreateDirectoryA(ospath, NULL);
        *p = '\\';
    }
}

/* Produce game-relative and OS paths. Without a base directory, skip collision
 * checks and leave os empty. Return FALSE with a printable reason for invalid names. */
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

/* Export the live map using a validated destination and report the resulting file. */
void h_sh_dumpmap(idCmdArgs *a)
{
    const char *path = cmd_argv(a, 1);
    if (path == NULL) {
        sh_printf("You need to provide a mapfile to write to\n");
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
        sh_printf("Failed to write map file %s\n", rel);
        return;
    }

    unsigned long long bytes = 0;
    if (os[0] != '\0' && dumpmap_file_size(os, &bytes))
        sh_printf("sh_dumpmap: wrote %s (%llu bytes)\n", os, bytes);
    else
        sh_printf("sh_dumpmap: wrote '%s' (game-relative -- look under <game dir>\\base\\)\n", rel);
}

/* Player cheat commands toggle runtime enforcement bits. Reflected properties
 * use bits one position lower and must not be substituted here.
 * To rederive offsets, inspect the idClientGameMsg_PlayerCommand_* setters
 * (pinned Vulkan 0x35FA50..0x35FB50) and cross-check the godMode getter 0xB72CA0. */
#define PLAYER_CHEAT_FLAGS_OFF  0x45ea8u   /* infiniteHealth 0x04 / noPlayerDeath 0x08 / noPlayerKill 0x10 / noTarget 0x20 (godMode 0x02) */
#define PLAYER_NOCLIP_OFF       0x0ce46u   /* noclip = bit 0x04 in a SEPARATE idPlayer byte */
#define CHEAT_BIT_INFHEALTH     0x04u
#define CHEAT_BIT_NOPLYDEATH    0x08u
#define CHEAT_BIT_NOPLYKILL     0x10u
#define CHEAT_BIT_NOTARGET      0x20u
#define CHEAT_BIT_NOCLIP        0x04u

/* Toggle a runtime player bit in a live map/playtest; command arguments are ignored. */
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

/* Dependency installation. */

int sh_entity_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;
    if (module_base == NULL) {
        backend_log("B2: entity install SKIPPED -- module base NULL");
        return 0;
    }

    g_gamemgr_slot = sh_resolve_gamemgr_slot(results, n, module_base);
    g_cmdsys       = cmdsys;
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
