/* modlobby.h -- a co-op lobby for a map whose content comes from the mod store (docs/architecture.md).
 *
 * The engine's lobby asks each joining peer for the map (reliable 4; the answer is reliable 0x2C, and until then its
 * verify state in the host's lobby is 1). The product holds the start (the 5 or 15 s countdown and the controller's
 * start step) while a peer of a modded map's lobby has not answered. A joiner reports its download state in the ready
 * command's (reliable 0xB6) bytes above 2, which the engine ignores, and while its content is missing clears its start
 * byte and leaves the engine's way. A lobby whose map no gate saw with content rows is never held.
 */
#ifndef SH_MODLOBBY_H
#define SH_MODLOBBY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* State codes: one byte of the ready command. The engine applies 0, 1 and 2 and ignores the rest. */
#define SH_MODLOBBY_CODE_PROGRESS 0x80   /* + percent (0..100): downloading the map's content */
#define SH_MODLOBBY_CODE_HOLDS    0xF0   /* the map's content is here */
#define SH_MODLOBBY_CODE_FAILED   0xF1   /* the content could not be had; the joiner leaves */
#define SH_MODLOBBY_CODE_STANDS_IN 0xF2  /* some of the content no player can get (removed, never uploaded, another
                                          * DOOM revision): the joiner plays the stand-ins every player composes */

enum sh_modlobby_state {
    SH_MODLOBBY_UNKNOWN = 0,   /* no code from this member (no product, or nothing said yet) */
    SH_MODLOBBY_DOWNLOADING,
    SH_MODLOBBY_HOLDS,
    SH_MODLOBBY_FAILED,
    SH_MODLOBBY_STANDS_IN
};

/* The code for a download at `done` of `total` bytes (total 0: 0 %). */
unsigned sh_modlobby_progress_code(unsigned long long done, unsigned long long total);

/* A received byte as a state: 1 with *state and *percent set when `code` is ours, else 0. */
int sh_modlobby_decode(unsigned code, int *state, int *percent);

/* The host's table: the states members sent, keyed by lobby user id, for one lobby map. */
void sh_modlobby_note(uint64_t user, unsigned code);
/* Forget every state (the lobby or its map changed). */
void sh_modlobby_forget(void);
/* The state a member sent last (SH_MODLOBBY_UNKNOWN when none), with its percent. */
int  sh_modlobby_state_of(uint64_t user, int *percent);

typedef struct sh_modlobby_member {
    const char *name;     /* the member's name as the lobby shows it */
    uint64_t user;        /* its lobby user id (the table's key) */
    int pending;          /* its peer has not answered the map check (verify state 1) */
} sh_modlobby_member;

/* The decision for a modded map's lobby: 1 when every member answered the map check, else 0 with the sentence for
 * the native dialog in `out` (under 256 bytes). */
int sh_modlobby_gate(const sh_modlobby_member *members, size_t count, char *out, size_t cap);

/* Every player must play the same bytes (a stand-in differs from its content, a fatal snapshot mismatch).
 * `host_stands_in`: the host's copy stands in. 1 when members agree with the host, else 0 with the sentence in `out`
 * (under 256 bytes); a member that said nothing is not counted. */
int sh_modlobby_standins_agree(int host_stands_in, const sh_modlobby_member *members, size_t count, char *out,
                               size_t cap);

/* The host's waiting line for the lobby's progress line; 0 (and "") when nobody is pending. */
int sh_modlobby_waiting_line(const sh_modlobby_member *members, size_t count, char *out, size_t cap);

/* Send the host this joiner's state (a code above). Main thread; does nothing without the engine binding or off a lobby peer. */
void sh_modlobby_send(unsigned code);

/* The six engine functions the lobby binds, each at its signature's address in the running image (or in a mapped
 * executable, for the image test). */
typedef struct sh_modlobby_sites {
    const uint8_t *try_start;    /* SnapmapLobbyTryToStartMatch (detoured) */
    const uint8_t *controller;   /* LobbyControllerStartMatch (detoured) */
    const uint8_t *ready;        /* LobbySetUserReady (detoured) */
    const uint8_t *peer;         /* SessionStateGameLobbyPeer (detoured) */
    const uint8_t *send;         /* SnapmapLobbySendReadyCommand (called) */
    const uint8_t *confirm;      /* LobbyPeerConfirmsMap (read only) */
} sh_modlobby_sites;

/* Checks every byte the detours and the sender rely on in the functions' own code: NULL when each holds (with the
 * session's global slot and the acting-lobby call in *session_slot and *acting_lobby), else the name of the first
 * failed check. No check reads a detour's stolen window, so hooks written or not do not change the result. */
const char *sh_modlobby_check_sites(const sh_modlobby_sites *sites, void ***session_slot, const void **acting_lobby);

#ifndef SH_MODLOBBY_PURE
#include "signatures.h"
/* Bind and install the four lobby sites and the sender (both renderers, by signature). 1 when installed; 0 leaves
 * every lobby as the engine runs it. */
int  sh_modlobby_install(const sig_result *results, size_t count, const uint8_t *module_base);
/* Main-thread tick: the host's waiting line. */
void sh_modlobby_poll(void);
#endif

#ifdef SH_MODLOBBY_TESTING
void sh_modlobby_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
