#ifndef SH_AUDIO_BANKS_NATIVE_H
#define SH_AUDIO_BANKS_NATIVE_H
#include "signatures.h"
#include "package_audio.h"
#include "package_audio_program.h"
#include "audio_id_reservations.h"
#include "audio_limits_native.h"
#include "audio_environment_native.h"
#include "audio_state_native.h"
#include "audio_parameter_native.h"
#include "audio_bank_gate.h"
/* Startup only: native targets must be quiescent while installing the bank
 * dispatcher gate. Bind before the declaration publication hooks. */
void sh_audio_banks_native_install(const sig_result *results, size_t count);
/* Hold this boundary across every shared-policy leg and the final provider
 * commit, after bank preparation and before taking render/provider locks.
 * End always releases the owned bank gate; incomplete resource recovery remains
 * owned by its own receipt and prevents a subsequent product transition. */
int sh_audio_banks_native_shared_begin(sh_audio_bank_scope *scope,char *error,size_t capacity);
/* Reacquire serialization only to recover retained settings. New settings
 * application is refused until those receipts have been resolved. */
int sh_audio_banks_native_shared_recover(sh_audio_bank_scope *scope,char *error,size_t capacity);
int sh_audio_banks_native_shared_ready(const sh_audio_bank_scope *scope,char *error,size_t capacity);
int sh_audio_banks_native_shared_end(sh_audio_bank_scope *scope,char *error,size_t capacity);
/* Playback-constraint leg of the shared policy transaction. It does not publish
 * buses, curves, banks or leases. The enclosing transition owns the receipt. */
int sh_audio_banks_native_limits_apply(const sh_package_audio_program *program,
    sh_audio_limits_change *change, char *error, size_t capacity);
/* Environment-curve leg retains detached native allocations until restore or
 * accept/release. Other shared controls and buses remain separate legs. */
int sh_audio_banks_native_environment_apply(const sh_package_audio_program *program,
    sh_audio_environment_change *change,char *error,size_t capacity);
int sh_audio_banks_native_state_apply(const sh_package_audio_program *program,
    sh_audio_state_change *change,char *error,size_t capacity);
int sh_audio_banks_native_parameter_apply(const sh_package_audio_program *program,
    sh_audio_parameter_change *change,char *error,size_t capacity);
/* Prepare numeric bindings without publishing audio. Caller owns the pending
 * lease until rollback or native retirement, and retains the program for that
 * entire lifetime. Uses all original IDs plus locked native occupancy. */
int sh_audio_banks_native_reserve(sh_package_audio_program *program,
    sh_package_compilation *compiled, sh_audio_originals *originals,
    sh_audio_id_lease **lease, char *error, size_t capacity);
/* Main-thread provider activation/recovery. Event IDs come from prepared native
 * sound shaders, including aliases, rather than hashes guessed from paths.
 * The bank inventory selects overrides and closes over the banks a wanted bank
 * names; the media inventory reports which changed payloads the file provider
 * serves and which a loaded bank already carries in its own data. */
int sh_audio_banks_native_activate(const uint32_t *events, size_t count,
    const sh_package_audio_bank *banks, size_t bank_count,
    const sh_package_audio_media *media, size_t media_count,
    char *error, size_t capacity);
typedef struct sh_audio_bank_addition sh_audio_bank_addition;
/* Retain exact pre-admission ownership through prefab preparation and disk
 * commit. Only one main-thread session may be active. End failure retains the
 * handle for recovery; do not free source providers while it remains owed. */
sh_audio_bank_addition *sh_audio_banks_native_additions_begin(char *error, size_t capacity);
int sh_audio_banks_native_activate_additive(sh_audio_bank_addition *addition,
    const uint32_t *events, size_t count,
    const sh_package_audio_bank *banks, size_t bank_count,
    const sh_package_audio_media *media, size_t media_count,
    char *error, size_t capacity);
int sh_audio_banks_native_additions_end(sh_audio_bank_addition **addition,
    int accepted, char *error, size_t capacity);
/* Validate before the caller's final irreversible commit. No engine call or
 * competing audio operation may intervene before accept; accept only releases
 * the retained rollback snapshot and cannot fail. */
int sh_audio_banks_native_additions_ready(sh_audio_bank_addition *addition,
    char *error, size_t capacity);
void sh_audio_banks_native_additions_accept(sh_audio_bank_addition **addition);
/* Retained-program activation, the only production route once programs are
 * compiled. programs lists the published view's programs in precedence order
 * (current map, library, retained owners); each owner reference passes to this
 * adapter, which calls release when it no longer needs one (duplicates, failed
 * transitions and fully retired programs). Leases are readied before any bank
 * load and activated only after every bank and shared-control leg succeeds.
 * Banks load outside the publication gate; shared controls publish one
 * effective policy (baseline plus per-key edits) inside it. Failure restores
 * shared controls, unloads what this call loaded when idle and keeps the
 * previous programs. Superseded programs retire later: never while any of
 * their banks has a playing item or their private IDs remain registered. */
/* serving marks a program kept only to serve consumers that began while it
 * was exposed (a rejected candidate's movies): its banks and private IDs
 * activate and the buses it introduces are constructed and configured, since
 * its sounds route to them, but it contributes no shared controls and no edit
 * of a native bus. The other programs keep authority over those. */
typedef struct sh_audio_active_program {
    void *owner;
    sh_package_audio_program *program;
    sh_audio_id_lease *lease;
    int serving;
} sh_audio_active_program;
int sh_audio_banks_native_programs_activate(const sh_audio_active_program *programs, size_t count,
    const sh_audio_shared_policy *baseline, void (*release)(void *), char *error, size_t capacity);
/* Additive editor admission inside an additions session: loads only newly
 * admitted programs, activates their leases at accept, unloads at rollback. */
int sh_audio_banks_native_programs_add(sh_audio_bank_addition *addition,
    const sh_audio_active_program *programs, size_t count, void (*release)(void *),
    char *error, size_t capacity);
/* Main-thread maintenance at a boundary holding no provider/render lock.
 * Returns programs still retiring, or -1 when retirement waits on a native
 * failure/uncertain state (reported in error). Never unloads a bank in use. */
int sh_audio_banks_native_programs_retire(char *error, size_t capacity);
size_t sh_audio_banks_native_programs_count(void);
/* After the current provider did not claim an audio file, resolve only a
 * private MEDIA identity owned by a still-retained program (active or retiring).
 * Exact provider/path lookup prevents rebinding to a different map. 1 opens,
 * 0 does not own the path, -1 means owned bytes are unavailable. No native call. */
int sh_audio_banks_native_open_media(const char *path, FILE **stream, uint64_t *length);
#endif
