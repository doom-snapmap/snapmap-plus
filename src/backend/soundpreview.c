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

static const uint8_t   *g_slot;        /* address OF the sound-world pointer, not the world itself */
static snd_preview_fn   g_preview;
static snd_stop_fn      g_stop;        /* from the world's VTABLE, not a signature -- see sp_bind_stop */
static void            *g_cmdsys;
static int              g_installed;
static int              g_stop_bound;  /* 0 = not tried, 1 = bound, -1 = tried and refused */

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

int sh_soundpreview_install(const sig_result *results, size_t n,
                            const uint8_t *module_base, void *cmdsys)
{
    if (g_installed) return g_preview != NULL;
    g_installed = 1;
    (void)module_base;
    g_cmdsys = cmdsys;

    const uint8_t *acc = (const uint8_t *)sig_addr_by_name(results, n, "SoundWorldLea");
    if (acc) g_slot = sp_decode_rip_any(acc);
    g_preview = (snd_preview_fn)sig_addr_by_name(results, n, "SoundPreview");
    /* g_stop is NOT resolved here. It comes from the sound world's own vtable at first use --
     * see sp_bind_stop for why a signature cannot find it. */

    if (!g_slot || !g_preview) {
        char l[200];
        _snprintf_s(l, sizeof l, _TRUNCATE,
            "B2: soundpreview DISABLED (worldSlot=%p preview=%p) -- sound auditioning unavailable",
            (void *)g_slot, (void *)g_preview);
        backend_log(l);
        g_preview = NULL;
        return 0;
    }
    char l[200];
    _snprintf_s(l, sizeof l, _TRUNCATE,
        "B2: soundpreview installed (worldSlot=%p preview=%p; stop binds from the vtable on first use)",
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

/* The UI holds this open for as long as the asset browser is on screen, so the mode is established
 * before the first click rather than during it, and survives between clicks. */
void sh_soundpreview_set_session(int on)
{
    if (on) { g_session = 1; sp_enter_mode(); return; }
    g_session = 0;
    sp_stop_emitter();
    sp_leave_mode();
}

void sh_soundpreview_stop(void)
{
    sp_stop_emitter();
    /* With the browser still open, stay in preview mode: the next Play should be instant, and a
     * background-audio suspend/resume between clicks is the very thing being avoided. The session
     * ending is what tears the mode down. */
    if (!g_session) sp_leave_mode();
}

int sh_soundpreview_play(const char *name)
{
    if (!g_preview || !name || !name[0]) return 0;

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
        return 0;
    }

    void *w = sp_world();
    if (!w) { backend_log("soundpreview: no sound world (not in a map yet?)"); return 0; }

    /* A preview we cannot stop is worse than no preview -- it is exactly testSound's failure -- so
     * the stop has to be in hand BEFORE anything is allowed to start. */
    if (!sp_bind_stop(w)) {
        backend_log("soundpreview: REFUSED -- StopSound could not be bound, so nothing is played");
        return 0;
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
        return 0;
    }
    return 1;
}
