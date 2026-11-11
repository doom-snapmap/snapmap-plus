/* Main-thread sound auditioning with one retained, stoppable emitter. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "soundpreview.h"
#include "snapmap_plus_iface.h"   /* sh_iface_text: the refusal handed to the window */
#include "imgpreview.h"      /* sh_imgpreview_has -- the name gate in front of the engine's find */
#include "engine_globals.h"
#include "engine_build.h"
#include "engine_cvar_read.h"
#include "backend_log.h"

/* idSoundWorld PREVIEW: void*(this, uint64 *outHandle, const char *name). Slot +0x30. */
typedef void *(*snd_preview_fn)(void *world, unsigned long long *out, const char *name);
/* idSoundWorld::StopSound: void(this, uint64 handle). Slot +0x98. */
typedef void  (*snd_stop_fn)(void *world, unsigned long long handle);
/* cmdSystem vtbl +0x48 ExecuteCommandText -- the same slot sh_spawninfo uses for `getviewpos`. */
typedef void  (*cmd_exec_fn)(void *cmdSys, const char *text);
/* The main-thread bridge, same two engine functions apply_engine.c uses for clone_bss_apply. */
typedef void  (*snd_buffer_cmd_fn)(void *cmdSys, const char *text);
/* Pass NULL for both optional AddCommand help/completion arguments; this
 * internal command needs neither.
 */
typedef void  (*snd_add_command_fn)(void *cmdSys, const char *name, void *handler, void *arg4,
                                    void *arg5, unsigned int flags);

static const uint8_t   *g_slot;        /* address OF the sound-world pointer, not the world itself */
static snd_preview_fn   g_preview;
static snd_stop_fn      g_stop;        /* from the world's VTABLE, not a signature -- see sp_bind_stop */
static void            *g_cmdsys;
static int              g_installed;
static int              g_stop_bound;  /* 0 = not tried, 1 = bound, -1 = tried and refused */

/* All sound-world mutations run on the DOOM main thread through the command
 * buffer. The native start path publishes an emitter before initializing its
 * shader at +0x20a0; an audio-worker update can dereference that field during
 * an off-thread start.
 *
 * Pinned Vulkan audit anchors: StartSound_wwise 0x1854600, emitter
 * initialization 0x1846900, world update 0x1857270 and emitter update
 * 0x1847670. Queue through the same main-thread route as native console
 * auditioning; SEH around the caller cannot repair a fault in the audio job.
 */
#define SP_CMD_NAME "sh_sndprev"

typedef enum { SP_OP_PLAY = 0, SP_OP_STOP, SP_OP_SESSION_ON, SP_OP_SESSION_OFF } sp_op;

/* Preserve operation order in an eight-entry ring. On overflow, drop and log
 * the oldest pending operation to retain the latest user action.
 */
#define SP_QUEUE_MAX 8
#define SP_NAME_CAP 512
typedef struct { sp_op op; unsigned long sequence; char name[SP_NAME_CAP]; } sp_item;
static sp_item          g_queue[SP_QUEUE_MAX];
static int              g_qhead, g_qcount;
static SRWLOCK          g_qlock = SRWLOCK_INIT;
static volatile LONG    g_cmd_registered;
static volatile LONG    g_post_sequence;
static snd_buffer_cmd_fn  g_buffer_cmd;
static snd_add_command_fn g_add_command;

/* Sound-world vtable slots, both decompile-verified on the pinned build.
 *   +0x30 the PREVIEW entry point -- also resolved by signature, which is what lets us prove the
 *         vtable layout is what we think it is before trusting any other slot in it.
 *   +0x98 StopSound(handle). */
#define SP_VSLOT_PREVIEW 0x30
#define SP_VSLOT_STOP    0x98

/* Verify StopSound at vtable +0x98 with its prologue and packed-handle
 * decode. Do not scan for it: slot +0xa0 has a byte-identical twin using
 * different globals.
 */
static const uint8_t SP_STOP_PROLOGUE[] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x50,0x10, 0x57, 0x48,0x83,0xEC,0x60,
    0x48,0xC7,0x40,0xC8,0xFE,0xFF,0xFF,0xFF, 0x48,0x89,0x58,0x08, 0x48,0x89,0x70,0x18,
    0x48,0x8B,0xF9, 0x48,0x8B,0xC2, 0x48,0xC1,0xE8,0x20, 0x85,0xC0, 0x78,0x27,
    0x3B,0x81,0x78,0x1F,0x00,0x00
};

static unsigned long long g_handle;    /* the one live preview, 0 = nothing playing */
static void              *g_handle_world;   /* the sound world that handle belongs to */

/* Preview mode enables background audio plus the native solo/forced-listener
 * settings. g_session holds the mode while the browser is open; g_mode_on
 * records whether it is active. Without a session, play enters the mode and
 * stop leaves it. Entering saves the player's own three settings and leaving
 * puts exactly those back.
 */
static int g_session;
static int g_mode_on;

#define SP_SOLO_CAP 256
typedef struct sp_saved {
    int  background, force_listener;
    char solo[SP_SOLO_CAP];
} sp_saved;
static sp_saved          g_saved;
static const uint8_t    *g_cvar_slot;          /* the cvar system's global slot */

/* Why the last audition was refused, in one plain sentence; empty when it was accepted. */
static SRWLOCK g_why_lock = SRWLOCK_INIT;
static const char *g_why = "";   /* one of the WHY_ sentences, or "" */

#define WHY_UNAVAILABLE "Sound preview is not available on this game build."
#define WHY_NOT_SOUND   "That sound is not in the game's files."
#define WHY_NO_MAP      "Sounds play only inside a map."
#define WHY_BUSY        "Sound preview could not start; try again."
#define WHY_DECLINED    "The game could not play that sound."

static void sp_refuse(const char *why)
{
    AcquireSRWLockExclusive(&g_why_lock);
    g_why = why ? why : "";
    ReleaseSRWLockExclusive(&g_why_lock);
    if (why && why[0]) {
        char l[200];
        _snprintf_s(l, sizeof l, _TRUNCATE, "soundpreview: refused: %s", why);
        backend_log(l);
    }
}

char *sh_soundpreview_refusal(void)
{
    char *out = NULL;
    AcquireSRWLockShared(&g_why_lock);
    if (g_why[0]) out = sh_iface_text(g_why);
    ReleaseSRWLockShared(&g_why_lock);
    return out;
}

/* Decode RIP-relative loads for any destination register: ModRM must satisfy
 * (modrm & 0xc7) == 0x05. The shared command helper accepts fewer destination
 * registers.
 */
#define SP_SCAN_WINDOW 64
static const uint8_t *sp_decode_rip_any(const uint8_t *fn)
{
    uint8_t b[SP_SCAN_WINDOW];
    __try { for (int i = 0; i < SP_SCAN_WINDOW; i++) b[i] = fn[i]; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    for (int i = 0; i + 7 <= SP_SCAN_WINDOW; i++) {
        if (b[i] == 0x48 && (b[i + 1] == 0x8B || b[i + 1] == 0x8D) && (b[i + 2] & 0xC7) == 0x05) {
            int32_t disp;
            memcpy(&disp, &b[i + 3], 4);
            return fn + i + 7 + disp;
        }
    }
    return NULL;
}

/* Read the world slot on each use; map loads and s_restart replace the
 * object.
 */
static void *sp_world(void)
{
    if (!g_slot) return NULL;
    __try {
        void *w = *(void * const *)g_slot;
        return w;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

/* cmdSystem vtbl +0x48, the same slot and the same byte offset sh_spawninfo uses for `getviewpos`. */
#define SP_VSLOT_EXEC_CMD_TEXT 0x48
/* Bind StopSound from the live vtable on first use. Require +0x30 to equal
 * the resolved SoundPreview entry before validating +0x98. Returns 1 when
 * bound.
 */
static int sp_bind_stop(void *world)
{
    if (g_stop_bound) return g_stop_bound > 0;
    g_stop_bound = -1;
    if (!world || !g_preview) return 0;
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)world;
        if (!vtbl) return 0;
        void *slot_preview = *(void * const *)(vtbl + SP_VSLOT_PREVIEW);
        if (slot_preview != (void *)g_preview) {
            char l[200];
            _snprintf_s(l, sizeof l, _TRUNCATE,
                "soundpreview: vtable+0x30 is %p but the signature found %p; layout moved, "
                "refusing to trust +0x98", slot_preview, (void *)g_preview);
            backend_log(l);
            return 0;
        }
        const uint8_t *stop = *(const uint8_t * const *)(vtbl + SP_VSLOT_STOP);
        if (!stop) return 0;
        for (size_t i = 0; i < sizeof SP_STOP_PROLOGUE; i++)
            if (stop[i] != SP_STOP_PROLOGUE[i]) {
                char l[200];
                _snprintf_s(l, sizeof l, _TRUNCATE,
                    "soundpreview: vtable+0x98 -> %p does not look like StopSound (byte %zu); refusing",
                    (const void *)stop, i);
                backend_log(l);
                return 0;
            }
        g_stop = (snd_stop_fn)stop;
        g_stop_bound = 1;
        char l[160];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "soundpreview: stop bound from vtable+0x98 -> %p (vtable verified via +0x30)",
            (const void *)stop);
        backend_log(l);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void sp_console(const char *text)
{
    if (!g_cmdsys || !text) return;
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)g_cmdsys;
        if (!vtbl) return;
        cmd_exec_fn exec = *(cmd_exec_fn const *)(vtbl + SP_VSLOT_EXEC_CMD_TEXT);
        if (exec) exec(g_cmdsys, text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Any thread may enqueue; the registered native console command drains on the
 * main thread.
 */

static void sp_do_play(const char *name);       /* the real bodies, main-thread only */
static void sp_do_stop(void);
static void sp_do_session(int on);

static void __cdecl sp_drain_cmd(void)
{
    /* Runs on the DOOM main thread. Drain everything queued since the last frame, in order. */
    for (;;) {
        sp_item it;
        AcquireSRWLockExclusive(&g_qlock);
        int have = g_qcount > 0;
        if (have) { it = g_queue[g_qhead]; g_qhead = (g_qhead + 1) % SP_QUEUE_MAX; g_qcount--; }
        ReleaseSRWLockExclusive(&g_qlock);
        if (!have) return;

        switch (it.op) {
            case SP_OP_PLAY:        sp_do_play(it.name); break;
            case SP_OP_STOP:        sp_do_stop();        break;
            case SP_OP_SESSION_ON:  sp_do_session(1);    break;
            case SP_OP_SESSION_OFF: sp_do_session(0);    break;
        }
    }
}

/* Register SP_CMD_NAME once, lazily -- the command system is not ready at install time. */
static int sp_ensure_command(void)
{
    if (InterlockedCompareExchange(&g_cmd_registered, 1, 0) != 0) return 1;
    if (!g_add_command || !g_cmdsys) {
        InterlockedExchange(&g_cmd_registered, 0);   /* retry once the deps bind */
        return 0;
    }
    __try {
        g_add_command(g_cmdsys, SP_CMD_NAME, (void *)sp_drain_cmd, NULL, NULL, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_cmd_registered, 0);
        return 0;
    }
    backend_log("soundpreview: " SP_CMD_NAME " registered; auditions run on the main thread");
    return 1;
}

/* Queue one op and kick the buffer. Returns 1 if it was accepted for main-thread execution. */
static int sp_post(sp_op op, const char *name)
{
    if (name && strnlen_s(name, SP_NAME_CAP) == SP_NAME_CAP) {
        backend_log("soundpreview: REFUSED -- sound name exceeds the queue limit");
        return 0;
    }
    if (!g_buffer_cmd || !g_cmdsys) {
        backend_log("soundpreview: REFUSED -- no command buffer, so nothing can run on the main thread");
        return 0;
    }
    if (!sp_ensure_command()) {
        backend_log("soundpreview: REFUSED -- " SP_CMD_NAME " could not be registered");
        return 0;
    }

    unsigned long sequence = (unsigned long)InterlockedIncrement(&g_post_sequence);
    sp_item dropped;
    int dropped_oldest = 0;

    AcquireSRWLockExclusive(&g_qlock);
    if (g_qcount == SP_QUEUE_MAX) {          /* full: drop the oldest, keep the newest intent */
        dropped = g_queue[g_qhead];
        dropped_oldest = 1;
        g_qhead = (g_qhead + 1) % SP_QUEUE_MAX;
        g_qcount--;
    }
    sp_item *slot = &g_queue[(g_qhead + g_qcount) % SP_QUEUE_MAX];
    slot->op = op;
    slot->sequence = sequence;
    if (name) strncpy_s(slot->name, sizeof slot->name, name, _TRUNCATE);
    else      slot->name[0] = '\0';
    g_qcount++;
    /* Hold the queue lock through BufferCommandText. The command only appends text for a later
     * main-thread drain; holding the lock prevents an already-buffered drain from consuming this
     * item before a throwing kick can roll it back. */
    int enq = 0;
    __try { g_buffer_cmd(g_cmdsys, SP_CMD_NAME "\n"); enq = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { enq = 0; }

    if (!enq) {
        int pos = -1;
        for (int i = 0; i < g_qcount; ++i)
            if (g_queue[(g_qhead + i) % SP_QUEUE_MAX].sequence == sequence) { pos = i; break; }
        if (pos >= 0) {
            for (int i = pos; i + 1 < g_qcount; ++i)
                g_queue[(g_qhead + i) % SP_QUEUE_MAX] =
                    g_queue[(g_qhead + i + 1) % SP_QUEUE_MAX];
            g_qcount--;
        }
        if (dropped_oldest && g_qcount < SP_QUEUE_MAX) {
            g_qhead = (g_qhead + SP_QUEUE_MAX - 1) % SP_QUEUE_MAX;
            g_queue[g_qhead] = dropped;
            g_qcount++;
        }
    }
    ReleaseSRWLockExclusive(&g_qlock);

    if (!enq) backend_log("soundpreview: REFUSED -- main-thread command could not be buffered");
    else if (dropped_oldest) backend_log("soundpreview: queue full -- dropped the oldest pending operation");
    return enq;
}

int sh_soundpreview_install(const sig_result *results, size_t n,
                            const uint8_t *module_base, void *cmdsys)
{
    if (g_installed) return g_preview != NULL;
    g_installed = 1;
    g_cmdsys = cmdsys;
    g_cvar_slot = module_base ? (const uint8_t *)glb_resolve(module_base, "cvar_system_slot", NULL) : NULL;

    const uint8_t *acc = (const uint8_t *)sig_addr_by_name(results, n, "SoundWorldLea");
    if (acc) g_slot = sp_decode_rip_any(acc);
    g_preview = (snd_preview_fn)sig_addr_by_name(results, n, "SoundPreview");
    /* Require both command-buffer entry points before arming playback. */
    g_buffer_cmd  = (snd_buffer_cmd_fn) sig_addr_by_name(results, n, "BufferCommandText");
    g_add_command = (snd_add_command_fn)sig_addr_by_name(results, n, "AddCommand");
    /* g_stop is NOT resolved here. It comes from the sound world's own vtable at first use --
     * see sp_bind_stop for why a signature cannot find it. */

    if (!g_slot || !g_preview || !g_buffer_cmd || !g_add_command || !g_cvar_slot) {
        char l[260];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "soundpreview: off (worldSlot=%p preview=%p bufferCmd=%p addCmd=%p cvars=%p); sound "
            "auditioning unavailable", (void *)g_slot, (void *)g_preview,
            (void *)g_buffer_cmd, (void *)g_add_command, (const void *)g_cvar_slot);
        backend_log(l);
        g_preview = NULL;
        return 0;
    }
    char l[260];
    _snprintf_s(l, sizeof l, _TRUNCATE,
        "soundpreview: installed (worldSlot=%p preview=%p; stop binds from the vtable on first "
        "use; every audition runs on the main thread via " SP_CMD_NAME ")",
        (void *)g_slot, (void *)g_preview);
    backend_log(l);
    return 1;
}

/* Read the player's current value of one sound setting: its integer, or its text. */
static int sp_cvar_int(const char *name, int *value)
{
    return sh_engine_cvar_read_int(sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, name),
                                   value);
}

/* idCVar keeps its value as an idStr at +0: length at +0x08, data pointer at +0x10. */
static int sp_cvar_text(const char *name, char *out, size_t cap)
{
    const uint8_t *cvar = (const uint8_t *)sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, name);
    if (!cvar) return 0;
    __try {
        int len = *(const int *)(cvar + 0x08);
        const char *data = *(const char *const *)(cvar + 0x10);
        if (len < 0 || (size_t)len >= cap || !data) return 0;
        memcpy(out, data, (size_t)len);
        out[len] = '\0';
        return strchr(out, '"') == NULL;          /* restored through a quoted console line */
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Save the player's background, solo and forced-listener settings, then raise background audio.
 * The engine's preview call writes s_soloSound/s_forceListener itself, so entering the mode only
 * has to do the half the engine does not. Returns 0, changing nothing, when the settings cannot
 * be read: leaving the mode could not put them back. Idempotent. */
static int sp_enter_mode(void)
{
    if (g_mode_on) return 1;
    sp_saved saved;
    if (!sp_cvar_int("s_playSoundInBackground", &saved.background) ||
        !sp_cvar_int("s_forceListener", &saved.force_listener) ||
        !sp_cvar_text("s_soloSound", saved.solo, sizeof saved.solo)) {
        backend_log("soundpreview: the sound settings could not be read, so preview mode was not entered");
        return 0;
    }
    g_saved = saved;
    sp_console("s_playSoundInBackground 1");
    g_mode_on = 1;
    backend_log("soundpreview: preview mode on");
    return 1;
}

/* Put the player's own settings back: the solo/forced-listener pair the engine set while
 * previewing, and the background raise. Restoring solo matters even if nothing was playing --
 * leave a preview's solo set and every other sound in the editor stays muted. */
static void sp_leave_mode(void)
{
    char line[SP_SOLO_CAP + 32];
    if (!g_mode_on) return;
    _snprintf_s(line, sizeof line, _TRUNCATE, "s_soloSound \"%s\"", g_saved.solo);
    sp_console(line);
    _snprintf_s(line, sizeof line, _TRUNCATE, "s_forceListener %d", g_saved.force_listener);
    sp_console(line);
    _snprintf_s(line, sizeof line, _TRUNCATE, "s_playSoundInBackground %d", g_saved.background);
    sp_console(line);
    g_mode_on = 0;
    backend_log("soundpreview: preview mode off; the player's sound settings are back");
}

/* Stop only the current emitter between previews; keep the mode cvars
 * unchanged.
 */
static void sp_stop_emitter(void)
{
    /* A handle names an emitter of the world that started it. After a map change or s_restart
     * the slot holds another world, where the same number could be someone else's sound. */
    void *w = sp_world();
    if (g_handle && w && w == g_handle_world && g_stop) {
        unsigned long long h = g_handle;
        __try { g_stop(w, h); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    /* Cleared even if the call faulted or the world went away: keeping it can only cause a
     * second bad call later. */
    g_handle = 0;
    g_handle_world = NULL;
}

/* MAIN THREAD ONLY (reached via sp_drain_cmd). */
static void sp_do_session(int on)
{
    if (on) { g_session = 1; (void)sp_enter_mode(); return; }
    g_session = 0;
    sp_stop_emitter();
    sp_leave_mode();
}

/* MAIN THREAD ONLY (reached via sp_drain_cmd). */
static void sp_do_stop(void)
{
    sp_stop_emitter();
    /* Keep preview mode while the session is open; closing the session
     * restores audio.
     */
    if (!g_session) sp_leave_mode();
}

/* The UI holds this open for as long as the asset browser is on screen, so the mode is established
 * before the first click rather than during it, and survives between clicks. */
void sh_soundpreview_set_session(int on)
{
    sp_post(on ? SP_OP_SESSION_ON : SP_OP_SESSION_OFF, NULL);
}

void sh_soundpreview_stop(void) { sp_post(SP_OP_STOP, NULL); }

/* MAIN THREAD ONLY (reached via sp_drain_cmd). Everything here touches the live sound world. */
static void sp_do_play(const char *name)
{
    /* The catalog already admitted the name when it was queued (sh_soundpreview_play), and the
     * catalog never changes, so the main thread does not take its lock again. */
    if (!g_preview || !name || !name[0]) return;

    void *w = sp_world();
    if (!w) { sp_refuse(WHY_NO_MAP); return; }

    /* Bind stopping before starting any emitter. */
    if (!sp_bind_stop(w)) {
        backend_log("soundpreview: StopSound could not be bound, so nothing is played");
        sp_refuse(WHY_UNAVAILABLE);
        return;
    }

    /* Enter preview mode if the session has not already enabled it. */
    if (!sp_enter_mode()) { sp_refuse(WHY_UNAVAILABLE); return; }

    /* Stop the previous emitter before allocating another; keep preview mode
     * active.
     */
    sp_stop_emitter();

    unsigned long long h = 0;
    __try { g_preview(w, &h, name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }

    g_handle = h;
    g_handle_world = h ? w : NULL;
    char l[320];
    _snprintf_s(l, sizeof l, _TRUNCATE, "soundpreview: '%s' -> handle 0x%llx%s",
                name, h, h ? "" : " (engine declined)");
    backend_log(l);
    if (!h) {
        sp_refuse(WHY_DECLINED);
        /* The engine set solo on the way in and nothing is playing, so hand the editor its audio
         * back -- unless a session is open, in which case the next click needs the mode anyway. */
        if (!g_session) sp_leave_mode();
    }
}

/* Public, any thread: validate the name and world availability, then queue
 * playback. A return of 1 means accepted; later native refusal is logged on
 * the main thread.
 */
int sh_soundpreview_play(const char *name)
{
    if (!name || !name[0]) return 0;
    if (!g_preview) { sp_refuse(WHY_UNAVAILABLE); return 0; }

    /* Validate with our catalog before native find-or-create, whose missing-name behavior can
     * raise an engine error. */
    if (!sh_imgpreview_has(SH_ASSET_SOUND, name)) { sp_refuse(WHY_NOT_SOUND); return 0; }
    /* Read only the global world pointer here; emitter access stays on the main thread. */
    if (!sp_world()) { sp_refuse(WHY_NO_MAP); return 0; }

    sp_refuse("");
    if (!sp_post(SP_OP_PLAY, name)) { sp_refuse(WHY_BUSY); return 0; }
    return 1;
}
