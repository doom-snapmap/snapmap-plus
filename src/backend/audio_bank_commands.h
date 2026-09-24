#ifndef SH_AUDIO_BANK_COMMANDS_H
#define SH_AUDIO_BANK_COMMANDS_H
#include "signatures.h"

/* Engine-owned slots, resolved without dereferencing a live bank manager. The
 * command adapter is used only at the main-thread provider transition boundary.
 * Zero-initialize before binding. A faulted submission retains its completion
 * storage: an asynchronous native callback may still own it. The adapter must
 * remain at a stable address through any such callback; do not free or copy it
 * after submission. Bind once before provider operations begin. */
typedef struct sh_audio_bank_commands {
    void **manager_slot;
    void **callback_slot;
    uint64_t completion[3];
    int uncertain;
} sh_audio_bank_commands;

int sh_audio_bank_commands_bind(sh_audio_bank_commands *commands,
    const unsigned char *image, const sig_result *results, size_t count);
/* Load an already assigned bank identity, bypassing name hashing and cached
 * ID-to-name aliases. Does not reserve IDs or grant ownership of result 69.
 * Returns native results (1 loaded, 69 already loaded); -1 means uncertain
 * effects. Never time out a queued command and release its callback storage. */
int sh_audio_bank_commands_load(sh_audio_bank_commands *commands, uint32_t id);

/* Read-only occupancy companion. Bind before audio initialization, probe only
 * at serialized main-thread provider boundaries while audio is alive. Each
 * registry is inspected under its native lock; no object reference is acquired.
 * Original-but-unloaded IDs are supplied separately by audio_originals. */
typedef struct sh_audio_registry {
    void **manager_slot, **index_slot;
    CRITICAL_SECTION *bank_lock;
} sh_audio_registry;
int sh_audio_registry_bind(sh_audio_registry *registry, const unsigned char *image,
    const sig_result *results, size_t count);
/* 0 unoccupied across all private domains, 1 occupied, -1 unavailable/corrupt.
 * Includes memory-loaded banks and resident media, not just posted events. */
int sh_audio_registry_occupied(const sh_audio_registry *registry, uint32_t id,
    char *error, size_t capacity);
#endif
