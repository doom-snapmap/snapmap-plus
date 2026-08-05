/* soundpreview.c -- see soundpreview.h for what this is and why testSound is not it. */

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
/* AddCommand's 4th and 5th arguments are help and arg-completion in SOME order, and this repo's two
 * existing declarations of it disagree about which (apply_engine.c says cb,p3,help; commands.c says
 * handler,help,argComp). AddCommand itself (0x1AA3630) only shows that they land in adjacent slots
 * of its 0x28-byte record -- param_5 at +0x10, param_4 at +0x18 -- without naming either. Since one
 * of those slots is a FUNCTION POINTER the engine may later call for tab-completion, passing a help
 * string into the wrong one is a jump into string bytes. This command is internal plumbing whose
 * help text nobody reads, so BOTH are passed NULL and the question does not have to be answered. */
typedef void  (*snd_add_command_fn)(void *cmdSys, const char *name, void *handler, void *arg4,
                                    void *arg5, unsigned int flags);

static const uint8_t   *g_slot;        /* address OF the sound-world pointer, not the world itself */
static snd_preview_fn   g_preview;
static snd_stop_fn      g_stop;        /* from the world's VTABLE, not a signature -- see sp_bind_stop */
static void            *g_cmdsys;
static int              g_installed;
static int              g_stop_bound;  /* 0 = not tried, 1 = bound, -1 = tried and refused */

/* ================================================== THE MAIN-THREAD RULE =========================
 * NOTHING in this file may touch the sound world from the calling thread. Everything that starts,
 * stops or re-modes a sound is queued and executed on the DOOM MAIN THREAD, through the engine's own
 * command buffer -- the same routing apply_engine.c uses for clone_bss_apply.
 *
 * This is not defensive style, it is a crash we shipped and hit (2026-08-05). `StartSound_wwise`
 * (RVA 0x1854600) publishes a brand-new emitter into the sound world's LIVE list before it is
 * initialised:
 *
 *     emitter = FUN_1418455e0(operator_new(0x2200));   // allocate + base ctor
 *     world[0x3EE][i] = emitter;                       // PUBLISH  (world+0x1F70)
 *     world[0x3EF]    = i + 1;                         //   and bump the count (world+0x1F78)
 *     ...
 *     FUN_141846900(emitter, world, ...);              // ONLY NOW is emitter+0x20A0 (the shader) set
 *
 * Meanwhile idSoundWorld::Update (RVA 0x1857270, called from the "Sound World Update" job at
 * 0x18520B0) walks that list on an AUDIO WORKER THREAD and does, at the top of idSound::Update
 * (0x1847670):
 *
 *     mov rax,[emitter+0x20A0]      ; the sound shader
 *     mov edi,[rax+0xA4]            ; <-- rax == 0 inside the window above -> AV at address 0xA4
 *
 * So any caller that starts a sound off the main thread races that window on every single call. We
 * were calling from the frontend's 30 Hz think-loop thread, and it faulted exactly there.
 *
 * The engine's own audition, `testSound`, does not hit this because it is a CONSOLE COMMAND: the
 * command buffer is drained on the main thread, which does not overlap the sound job. We take the
 * same route rather than inventing a safety story of our own -- if the engine's worked example is
 * safe, an identical routing is safe for the same reason.
 *
 * (The fault-shield does not save you here either: it downgrades the AV to a recoverable Error(6),
 * which is validated for MAIN-THREAD faults. Unwinding it out of the audio job instead leaves the
 * sound system's state held and the game freezes rather than recovering.) */
#define SP_CMD_NAME "sh_sndprev"

typedef enum { SP_OP_PLAY = 0, SP_OP_STOP, SP_OP_SESSION_ON, SP_OP_SESSION_OFF } sp_op;

/* A small ring rather than a single last-wins slot: ordering between ops is meaningful (a
 * session-off that arrives after a play must still stop it), so they cannot be collapsed. 8 is
 * generous -- the UI produces at most one op per click. On overflow the OLDEST is dropped and said
 * so, because the newest op is the one that reflects what the user last did. */
#define SP_QUEUE_MAX 8
typedef struct { sp_op op; char name[256]; } sp_item;
static sp_item          g_queue[SP_QUEUE_MAX];
static int              g_qhead, g_qcount;
static CRITICAL_SECTION g_qlock;
static int              g_qlock_init;
static volatile LONG    g_cmd_registered;
static snd_buffer_cmd_fn  g_buffer_cmd;
static snd_add_command_fn g_add_command;

/* Sound-world vtable slots, both decompile-verified on the pinned build.
 *   +0x30 the PREVIEW entry point -- also resolved by signature, which is what lets us prove the
 *         vtable layout is what we think it is before trusting any other slot in it.
 *   +0x98 StopSound(handle). */
#define SP_VSLOT_PREVIEW 0x30
#define SP_VSLOT_STOP    0x98

/* StopSound's prologue: the frame setup, then the handle decode that is its fingerprint --
 * SHR RAX,0x20 to take the emitter index out of the packed handle, TEST/JS, then CMP against the
 * emitter count at world+0x1F78. Used to VERIFY the vtable slot, never to search for it: the
 * function has a byte-identical twin at +0xA0 (same code, different globals), so a search returns
 * two hits and cannot pick. Verifying a pointer we already took from the right slot has no such
 * problem -- the twin only means "these bytes are a StopSound", which is exactly the claim. */
static const uint8_t SP_STOP_PROLOGUE[] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x50,0x10, 0x57, 0x48,0x83,0xEC,0x60,
    0x48,0xC7,0x40,0xC8,0xFE,0xFF,0xFF,0xFF, 0x48,0x89,0x58,0x08, 0x48,0x89,0x70,0x18,
    0x48,0x8B,0xF9, 0x48,0x8B,0xC2, 0x48,0xC1,0xE8,0x20, 0x85,0xC0, 0x78,0x27,
    0x3B,0x81,0x78,0x1F,0x00,0x00
};

static unsigned long long g_handle;    /* the one live preview, 0 = nothing playing */

/* PREVIEW MODE is the cvar state an audition needs: background audio on (Snapmap+ has focus, DOOM
 * does not) plus the engine's solo/forced-listener pair. It is entered ONCE and held, never toggled
 * per click.
 *
 * The first version toggled it per preview, and that was audibly wrong: each play ran
 * s_playSoundInBackground 0 (tearing down the previous preview) immediately followed by 1, so every
 * click suspended and resumed DOOM's whole audio engine and re-entered solo. Sounds faded in, and
 * short ones could be over before the resume finished -- which is exactly the "sometimes it does
 * not play" this was reported as.
 *
 * g_session is the UI holding the mode open for as long as the asset browser is up; g_mode_on is
 * what we have actually written. A preview can still be started without a session (the mode is
 * entered on demand and dropped on stop) -- the session only means "do not drop it between clicks". */
static int g_session;
static int g_mode_on;

/* RIP-relative decode, any destination register.
 *
 * The shared sh_decode_rip_slot in commands.c only accepts modrm 0x05 (->RAX) and 0x0D (->RCX)
 * because that is what its two callers' accessors happen to use. Our accessor loads into RBX
 * (modrm 0x1D). Rather than widen a scanner two other subsystems depend on, this takes the general
 * form: for a [rip+disp32] operand the modrm byte is mod=00 rm=101, i.e. (modrm & 0xC7) == 0x05,
 * with the register in bits 3..5. Same arithmetic otherwise. */
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

/* The live sound world, or NULL. Read through the slot EVERY time: the world is torn down and
 * rebuilt across map loads and s_restart, so a cached pointer goes stale and would be a use-after-
 * free the first time someone reloads a map with a preview handle outstanding. */
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
/* Bind StopSound from the live sound world's vtable, once, and only if the vtable proves itself
 * first: slot +0x30 must be the very function the SoundPreview signature already found. If that
 * matches, this is the vtable we reverse-engineered and slot +0x98 means what we recorded; if it
 * does not, the layout moved and we refuse rather than call an arbitrary pointer.
 *
 * Deferred to first use because the sound world does not exist at install time. Returns 1 if bound. */
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

/* ---- the main-thread queue ---------------------------------------------------------------------
 * Producers (any thread) push an op and kick the command buffer; the consumer is the registered
 * console command, which the engine runs on the main thread. See THE MAIN-THREAD RULE above. */

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
    if (!g_buffer_cmd || !g_cmdsys) {
        backend_log("soundpreview: REFUSED -- no command buffer, so nothing can run on the main thread");
        return 0;
    }
    if (!sp_ensure_command()) {
        backend_log("soundpreview: REFUSED -- " SP_CMD_NAME " could not be registered");
        return 0;
    }

    if (g_qlock_init) EnterCriticalSection(&g_qlock);
    if (g_qcount == SP_QUEUE_MAX) {          /* full: drop the oldest, keep the newest intent */
        g_qhead = (g_qhead + 1) % SP_QUEUE_MAX;
        g_qcount--;
    }
    sp_item *slot = &g_queue[(g_qhead + g_qcount) % SP_QUEUE_MAX];
    slot->op = op;
    if (name) strncpy_s(slot->name, sizeof slot->name, name, _TRUNCATE);
    else      slot->name[0] = '\0';
    g_qcount++;
    if (g_qlock_init) LeaveCriticalSection(&g_qlock);

    int enq = 0;
    __try { g_buffer_cmd(g_cmdsys, SP_CMD_NAME "\n"); enq = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { enq = 0; }
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
    /* The main-thread bridge. Without BOTH of these there is no safe way to start a sound at all,
     * so the module refuses to arm rather than falling back to calling from the caller's thread. */
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

/* Silence the current emitter and nothing else. This is what runs BETWEEN previews -- the cvars are
 * deliberately untouched, because churning them is what made playback inconsistent. */
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
    /* With the browser still open, stay in preview mode: the next Play should be instant, and a
     * background-audio suspend/resume between clicks is the very thing being avoided. The session
     * ending is what tears the mode down. */
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

    /* The gate. The engine resolves this name with the find-OR-CREATE decl primitive, which raises
     * a fatal error on a name that is not a real decl -- so an unvalidated name is not a failed
     * preview, it is a killed game. Our own container index answers the question with no engine
     * call at all, and the browser only ever offers names that came from that same index, so this
     * rejects nothing legitimate. */
    if (!sh_imgpreview_has(SH_ASSET_SOUND, name)) {
        char l[320];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "soundpreview: REFUSED '%s' -- not a soundshader in the shipped containers", name);
        backend_log(l);
        return;
    }

    void *w = sp_world();
    if (!w) { backend_log("soundpreview: no sound world (not in a map yet?)"); return; }

    /* A preview we cannot stop is worse than no preview -- it is exactly testSound's failure -- so
     * the stop has to be in hand BEFORE anything is allowed to start. */
    if (!sp_bind_stop(w)) {
        backend_log("soundpreview: REFUSED -- StopSound could not be bound, so nothing is played");
        return;
    }

    /* Mode FIRST, and only if it is not already up. With a session open this is a no-op, so the
     * audio engine is never suspended mid-click. */
    sp_enter_mode();

    /* One at a time. The engine's preview allocates a fresh emitter per call and does NOT recycle,
     * so without this the second click is audible on top of the first -- the pile-up testSound
     * suffers from. Only the emitter is stopped; the mode stays up. */
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

/* PUBLIC, any thread. Validates what can be validated WITHOUT touching the sound world -- the name
 * gate and the world's mere existence -- then hands the actual audition to the main thread.
 *
 * The return value therefore means "accepted", not "audible": a name we reject, a missing sound
 * world or a dead command buffer are still reported synchronously (which is every failure the user
 * can act on), while the engine's own "declined to start" outcome is one frame later and lands in
 * the log. That trade is deliberate -- see THE MAIN-THREAD RULE. */
int sh_soundpreview_play(const char *name)
{
    if (!g_preview || !name || !name[0]) return 0;

    /* Cheap, engine-free, and the same gate sp_do_play re-applies on the main thread. Doing it here
     * as well is what keeps a bad name a red toast instead of a silent nothing a frame later. */
    if (!sh_imgpreview_has(SH_ASSET_SOUND, name)) {
        char l[320];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "soundpreview: REFUSED '%s' -- not a soundshader in the shipped containers", name);
        backend_log(l);
        return 0;
    }
    /* Reading the world POINTER is a plain load of a global; it does not walk the emitter list and
     * is safe from any thread. Only mutating the world is not. */
    if (!sp_world()) { backend_log("soundpreview: no sound world (not in a map yet?)"); return 0; }

    return sp_post(SP_OP_PLAY, name);
}
