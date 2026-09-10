/* Bounded reads from the engine's full cvar registry. Unknown values refuse. */
#ifndef SH_ENGINE_CVAR_READ_H
#define SH_ENGINE_CVAR_READ_H
#include <windows.h>
#include <stdint.h>
#include <string.h>

static void *sh_engine_cvar_find(const void *system_slot, const char *name)
{
    if (!system_slot || !name) return NULL;
    __try {
        const uint8_t *system = *(const uint8_t *const *)system_slot;
        if (!system) return NULL;
        const uint8_t *const *rows = *(const uint8_t *const *const *)(system + 8);
        uint32_t count = *(const uint32_t *)(system + 0x10);
        if (!rows || count > 100000) return NULL;
        for (uint32_t i = 0; i < count; i++) {
            if (rows[i]) {
                const char *key = *(const char *const *)(rows[i] + 0x40);
                if (key && strcmp(key, name) == 0) return (void *)rows[i];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return NULL;
}

static int sh_engine_cvar_read_int(const void *cvar, int *value)
{
    if (!cvar || !value) return 0;
    __try { *value = *(const int *)((const uint8_t *)cvar + 0x30); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
#endif
