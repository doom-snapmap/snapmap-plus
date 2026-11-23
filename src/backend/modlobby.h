/* modlobby -- a co-op lobby for a map whose content comes from the mod store
 * (docs/architecture.md "Co-op lobbies for modded maps").
 *
 * DOOM 2016's SnapMap lobby already has what holding the start needs, read in
 * the engine on both renderers:
 *
 *  - The map check. The host asks every peer that joins for the lobby's map
 *    (reliable 4); a peer answers "I hold the map" (reliable 0x2C) and until
 *    then its map-verify state in the host's lobby is 1 (asked). A peer that
 *    still holds 1 when the host starts is sent START_LOADING anyway. The
 *    product's CacheReady gate (modpublished.h) defers a joiner's answer
 *    until the map's content is here, so "verify state 1" means "does not
 *    hold this map's content yet" for a modded map.
 *  - The start. The lobby screen's Start (TryToStartMatch) arms the lobby
 *    controller's 5 or 15 second countdown, whose end starts the match
 *    (the controller's start step). Both are held here while a peer of a
 *    modded map's lobby has not answered, and the countdown goes back to
 *    idle the way the engine's own controller drops it.
 *  - The member state. A joiner sends the host a ready command (game reliable
 *    0xB6: its lobby user and one byte) through the screen's own sender; the
 *    host applies 0 toggle, 1 set and 2 unset to the member's ready flag and
 *    ignores every other byte. A product joiner sends its download state in
 *    the bytes the engine ignores (below); a product host reads them where the
 *    engine applies them (idLobby SetLobbyUserReady). A host without the
 *    product ignores them; nothing is replicated beyond the host.
 *  - The joiner's start. A joiner loads when its lobby's start byte is set
 *    (State_Game_Lobby_Peer). While its content is not here the product clears
 *    the byte and leaves the lobby the engine's way (the fallback: the host
 *    has no product, or the match was already running).
 *
 * Everything is keyed on the receiver: a lobby whose map no gate saw with
 * content rows (a vanilla map, a map that needs no mod, a multiplayer lobby)
 * is never held, and a byte the engine knows is never read as a state. */
#ifndef SH_MODLOBBY_H
#define SH_MODLOBBY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ the state codes
 * One byte of the ready command. The engine applies 0, 1 and 2 and ignores the
 * rest, so every code is above 2. */
#define SH_MODLOBBY_CODE_PROGRESS 0x80   /* + percent (0..100): downloading the map's content */
#define SH_MODLOBBY_CODE_HOLDS    0xF0   /* the map's content is here */
#define SH_MODLOBBY_CODE_FAILED   0xF1   /* the content could not be had; the joiner leaves */

enum sh_modlobby_state {
    SH_MODLOBBY_UNKNOWN = 0,   /* no code from this member (no product, or nothing said yet) */
    SH_MODLOBBY_DOWNLOADING,
    SH_MODLOBBY_HOLDS,
    SH_MODLOBBY_FAILED
};

/* The code for a download at `done` of `total` bytes (total 0: 0 %). */
unsigned sh_modlobby_progress_code(unsigned long long done, unsigned long long total);

/* A received byte as a state: 1 with *state and *percent set when `code` is one
 * of ours, 0 for a byte the engine owns (0..2) or any other. */
int sh_modlobby_decode(unsigned code, int *state, int *percent);

/* ------------------------------------------------------------ the host's table
 * The states members sent, keyed by their lobby user id, for one lobby map. */
void sh_modlobby_note(uint64_t user, unsigned code);
/* Forget every state (the lobby or its map changed). */
void sh_modlobby_forget(void);
/* The state a member sent last (SH_MODLOBBY_UNKNOWN when none), with its percent. */
int  sh_modlobby_state_of(uint64_t user, int *percent);

/* ------------------------------------------------------------ the host's gate */
typedef struct sh_modlobby_member {
    const char *name;     /* the member's name as the lobby shows it */
    uint64_t user;        /* its lobby user id (the table's key) */
    int pending;          /* its peer has not answered the map check (verify state 1) */
} sh_modlobby_member;

/* The one decision for a modded map's lobby: 1 when every member answered the
 * map check (the match may start), else 0 with the sentence in `out` (under
 * 256 bytes, the native dialog body): "The match starts when every player has
 * this map's mod content. Still getting it: Sam (42%) and Alex." */
int sh_modlobby_gate(const sh_modlobby_member *members, size_t count, char *out, size_t cap);

/* The host's waiting line, shown in the lobby's progress line while a member
 * is getting the content: "Waiting for this map's mod content: Sam 42%, Alex."
 * 0 (and "") when nobody is pending. */
int sh_modlobby_waiting_line(const sh_modlobby_member *members, size_t count, char *out, size_t cap);

/* ------------------------------------------------------------ the joiner */
/* Send the host this joiner's state (a code above). Main thread; nothing when
 * the engine binding is absent or this machine is not a lobby peer. */
void sh_modlobby_send(unsigned code);

/* ------------------------------------------------------------ the engine */
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

/* Every byte the detours and the sender rely on, checked in the functions' own code: NULL when each holds (with the
 * session's global slot and the acting-lobby call in *session_slot and *acting_lobby), else the name of the first
 * check that failed, for the log. No check reads a detour's stolen window, so the result is the same before and after
 * the lobby's own hooks are written. The bind and tests/modlobby_sites_test.c both run this function. */
const char *sh_modlobby_check_sites(const sh_modlobby_sites *sites, void ***session_slot, const void **acting_lobby);

#ifndef SH_MODLOBBY_PURE
#include "signatures.h"
/* Bind and install the four lobby sites and the sender (both renderers, by
 * signature). Returns 1 when installed; 0 leaves every lobby as the engine
 * runs it. */
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
