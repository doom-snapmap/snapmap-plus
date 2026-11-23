/* script_walk.h -- where the mod-script API's on.signal hook goes: the walk over an entity's listeners, found
 * through the call inside FireOutput. Header only, so tests/script_node_test.c runs the very check script_api_game.c
 * makes against each executable's own FireOutput. */
#ifndef SH_SCRIPT_WALK_H
#define SH_SCRIPT_WALK_H

#include <windows.h>
#include <stdint.h>
#include <string.h>

#define FIRE_CALL_OFF       0x17u       /* FireOutput: the call to the walk over the entity's listeners */
#define WALK_STOLEN         14

static const uint8_t k_walk_prologue[20] = {
    0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20
};

/* The walk the call inside FireOutput (`fire`) goes to: the call at +FIRE_CALL_OFF is an E8 and its target begins
 * with k_walk_prologue. NULL otherwise (or when the bytes cannot be read). */
static void *sh_script_walk_find(const uint8_t *fire)
{
    void *found = NULL;
    if (!fire) return NULL;
    __try {
        if (fire[FIRE_CALL_OFF] == 0xE8) {
            int32_t rel;
            const uint8_t *walk;
            memcpy(&rel, fire + FIRE_CALL_OFF + 1, sizeof rel);
            walk = fire + FIRE_CALL_OFF + 5 + rel;
            if (!memcmp(walk, k_walk_prologue, sizeof k_walk_prologue)) found = (void *)walk;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { found = NULL; }
    return found;
}

#endif /* SH_SCRIPT_WALK_H */
