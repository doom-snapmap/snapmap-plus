/* Main-thread sound auditioning with one retained, stoppable emitter. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "soundpreview.h"
#include "imgpreview.h"      /* sh_imgpreview_has -- the name gate in front of the engine's find */
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
static CRITICAL_SECTION g_qlock;
static int              g_qlock_init;
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

/* Preview mode enables background audio plus the native solo/forced-listener
 * settings. g_session holds the mode while the browser is open; g_mode_on
 * records whether it is active. Without a session, play enters the mode and
 * stop leaves it.
 */
static int g_session;
static int g_mode_on;

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
                "B2: soundpreview -- vtable+0x30 is %p but the signature found %p; layout moved, "
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
                    "B2: soundpreview -- vtable+0x98 -> %p does not look like StopSound (byte %zu); refusing",
                    (const void *)stop, i);
                backend_log(l);
                return 0;
            }
        g_stop = (snd_stop_fn)stop;
        g_stop_bound = 1;
        char l[160];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "B2: soundpreview -- stop bound from vtable+0x98 -> %p (vtable verified via +0x30)",
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
        if (g_qlock_init) EnterCriticalSection(&g_qlock);
        int have = g_qcount > 0;
        if (have) { it = g_queue[g_qhead]; g_qhead = (g_qhead + 1) % SP_QUEUE_MAX; g_qcount--; }
        if (g_qlock_init) LeaveCriticalSection(&g_qlock);
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
    backend_log("B2: soundpreview -- " SP_CMD_NAME " registered (auditions now run on the main thread)");
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

    if (g_qlock_init) EnterCriticalSection(&g_qlock);
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
    if (g_qlock_init) LeaveCriticalSection(&g_qlock);

    if (!enq) backend_log("soundpreview: REFUSED -- main-thread command could not be buffered");
    else if (dropped_oldest) backend_log("soundpreview: queue full -- dropped the oldest pending operation");
    return enq;
}

int sh_soundpreview_install(const sig_result *results, size_t n,
                            const uint8_t *module_base, void *cmdsys)
{
    if (g_installed) return g_preview != NULL;
    g_installed = 1;
    (void)module_base;
    g_cmdsys = cmdsys;

    if (!g_qlock_init) { InitializeCriticalSection(&g_qlock); g_qlock_init = 1; }

    const uint8_t *acc = (const uint8_t *)sig_addr_by_name(results, n, "SoundWorldLea");
    if (acc) g_slot = sp_decode_rip_any(acc);
    g_preview = (snd_preview_fn)sig_addr_by_name(results, n, "SoundPreview");
    /* Require both command-buffer entry points before arming playback. */
    g_buffer_cmd  = (snd_buffer_cmd_fn) sig_addr_by_name(results, n, "BufferCommandText");
    g_add_command = (snd_add_command_fn)sig_addr_by_name(results, n, "AddCommand");
    /* g_stop is NOT resolved here. It comes from the sound world's own vtable at first use --
     * see sp_bind_stop for why a signature cannot find it. */

    if (!g_slot || !g_preview || !g_buffer_cmd || !g_add_command) {
        char l[260];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "B2: soundpreview DISABLED (worldSlot=%p preview=%p bufferCmd=%p addCmd=%p) -- sound "
            "auditioning unavailable", (void *)g_slot, (void *)g_preview,
            (void *)g_buffer_cmd, (void *)g_add_command);
        backend_log(l);
        g_preview = NULL;
        return 0;
    }
    char l[260];
    _snprintf_s(l, sizeof l, _TRUNCATE,
        "B2: soundpreview installed (worldSlot=%p preview=%p; stop binds from the vtable on first "
        "use; every audition runs on the main thread via " SP_CMD_NAME ")",
        (void *)g_slot, (void *)g_preview);
    backend_log(l);
    return 1;
}

int sh_soundpreview_active(void) { return g_handle != 0; }

/* Raise background audio. The engine's preview call writes s_soloSound/s_forceListener itself, so
 * entering the mode only has to do the half the engine does not. Idempotent. */
static void sp_enter_mode(void)
{
    if (g_mode_on) return;
    sp_console("s_playSoundInBackground 1");
    g_mode_on = 1;
    backend_log("soundpreview: preview mode ON (s_playSoundInBackground 1)");
}

/* Drop the whole mode: the solo/forced-listener pair the engine set, and our background raise.
 * Clearing solo matters even if nothing was playing -- leave it set and every other sound in the
 * editor stays muted. */
static void sp_leave_mode(void)
{
    sp_console("s_soloSound \"\"");
    sp_console("s_forceListener 0");
    if (g_mode_on) {
        sp_console("s_playSoundInBackground 0");
        g_mode_on = 0;
        backend_log("soundpreview: preview mode OFF");
    }
}

/* Stop only the current emitter between previews; keep the mode cvars
 * unchanged.
 */
static void sp_stop_emitter(void)
{
    void *w = sp_world();
    if (g_handle && w && g_stop) {
        unsigned long long h = g_handle;
        __try { g_stop(w, h); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    /* Cleared even if the call faulted or the world went away: the handle names an emitter in a
     * world that no longer exists, so keeping it can only cause a second bad call later. */
    g_handle = 0;
}

/* MAIN THREAD ONLY (reached via sp_drain_cmd). */
static void sp_do_session(int on)
{
    if (on) { g_session = 1; sp_enter_mode(); return; }
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
    if (!g_preview || !name || !name[0]) return;

    /* Validate with our catalog before native find-or-create, whose missing-
     * name behavior can raise an engine error.
     */
    if (!sh_imgpreview_has(SH_ASSET_SOUND, name)) {
        char l[320];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "soundpreview: REFUSED '%s' -- not a soundshader in the shipped containers", name);
        backend_log(l);
        return;
    }

    void *w = sp_world();
    if (!w) { backend_log("soundpreview: no sound world (not in a map yet?)"); return; }

    /* Bind stopping before starting any emitter. */
    if (!sp_bind_stop(w)) {
        backend_log("soundpreview: REFUSED -- StopSound could not be bound, so nothing is played");
        return;
    }

    /* Enter preview mode if the session has not already enabled it. */
    sp_enter_mode();

    /* Stop the previous emitter before allocating another; keep preview mode
     * active.
     */
    sp_stop_emitter();

    unsigned long long h = 0;
    __try { g_preview(w, &h, name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }

    g_handle = h;
    char l[320];
    _snprintf_s(l, sizeof l, _TRUNCATE, "soundpreview: '%s' -> handle 0x%llx%s",
                name, h, h ? "" : " (engine declined)");
    backend_log(l);
    if (!h) {
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
    if (!g_preview || !name || !name[0]) return 0;

    /* Repeat the catalog check here for synchronous feedback and at execution
     * time.
     */
    if (!sh_imgpreview_has(SH_ASSET_SOUND, name)) {
        char l[320];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "soundpreview: REFUSED '%s' -- not a soundshader in the shipped containers", name);
        backend_log(l);
        return 0;
    }
    /* Read only the global world pointer here; emitter access stays on the
     * main thread.
     */
    if (!sp_world()) { backend_log("soundpreview: no sound world (not in a map yet?)"); return 0; }

    return sp_post(SP_OP_PLAY, name);
}
