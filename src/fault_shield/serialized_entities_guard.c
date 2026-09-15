/* Snapshot writing already skips expired entity handles. Swap-removal must do
 * the same when updating the moved entity's serialized index. Leave the native
 * handle move, count reduction and removed entity's state reset intact.
 * Only NULL is handled; invalid non-NULL pointers still reach normal diagnostics. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "serialized_entities_guard.h"
#include "../backend/patch.h"
#include "../backend/backend_log.h"

static const uint8_t index_write[] = {0x89,0xa8,0x64,0x0b,0x00,0x00};
static sh_patch_handle g_index_patch;
static uint8_t *g_index_relay;
static int g_index_ready;

/* The original MOV preserves flags. Neither path may clobber a register or
 * flags before resuming. A separate writable page holds the diagnostic count;
 * published code stays executable/read-only. No callback or engine call runs
 * from this instruction boundary. */
static int index_relay_emit(uint8_t *relay, size_t page, uintptr_t resume)
{
    const uint8_t code[] = {
        0x9c,                           /* pushfq */
        0x48,0x85,0xc0,                 /* test rax,rax */
        0x75,0x0a,                      /* jnz live */
        0xf0,0xff,0x05,0,0,0,0,         /* lock inc dword [rip+counter] */
        0x9d,                           /* popfq (empty handle) */
        0xeb,0x07,                      /* jmp done */
        0x9d,                           /* live: popfq before a possible fault */
        0x89,0xa8,0x64,0x0b,0,0,        /* original index write */
        0xff,0x25,0,0,0,0,0,0,0,0,0,0,0,0 /* jmp [rip+resume] */
    };
    DWORD old;
    int32_t displacement;
    if (page < sizeof code || page > INT32_MAX) return 0;
    displacement = (int32_t)page - 13;
    memcpy(relay, code, sizeof code);
    memcpy(relay + 9, &displacement, sizeof displacement);
    memcpy(relay + 29, &resume, sizeof resume);
    *(LONG *)(relay + page) = 0;
    return VirtualProtect(relay, page, PAGE_EXECUTE_READ, &old) &&
           FlushInstructionCache(GetCurrentProcess(), relay, sizeof code);
}

static uint8_t *index_relay_allocate(uintptr_t site)
{
    SYSTEM_INFO info;
    uintptr_t step, center, delta, minimum, maximum;
    size_t bytes;
    GetSystemInfo(&info);
    step = info.dwAllocationGranularity;
    bytes = (size_t)info.dwPageSize * 2;
    center = site & ~(step - 1);
    minimum = (uintptr_t)info.lpMinimumApplicationAddress;
    maximum = (uintptr_t)info.lpMaximumApplicationAddress - (bytes - 1);
    for (delta = 0; delta < 0x7fff0000u; delta += step) {
        uintptr_t candidates[] = {center >= delta ? center - delta : 0, center + delta};
        int i;
        for (i = 0; i < 2; ++i) {
            MEMORY_BASIC_INFORMATION mbi;
            uint8_t *relay;
            uintptr_t address = candidates[i];
            intptr_t relative;
            if (address < minimum || address > maximum ||
                !VirtualQuery((void *)address, &mbi, sizeof mbi) || mbi.State != MEM_FREE)
                continue;
            relative = (intptr_t)address - (intptr_t)(site + 5);
            if (relative < INT32_MIN || relative > INT32_MAX) continue;
            relay = VirtualAlloc((void *)address, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!relay) continue;
            if (index_relay_emit(relay, info.dwPageSize, site + sizeof index_write))
                return relay;
            VirtualFree(relay, 0, MEM_RELEASE);
            return NULL;
        }
    }
    return NULL;
}

int sh_serialized_entities_guard_install(const sig_result *results, size_t count)
{
    const sig_result *site = NULL;
    uint8_t replacement[sizeof index_write] = {0xe9,0,0,0,0,0x90};
    int32_t relative;
    sh_patch_status status;
    size_t i;
    if (g_index_ready) return 1;
    /* A failed patch can still reference the relay. Restore first; keep both
     * the handle and allocation if restoring bytes or protection fails. */
    if (g_index_patch.live && code_unpatch(&g_index_patch) != B2_PATCH_OK) return 0;
    if (g_index_relay) {
        VirtualFree(g_index_relay, 0, MEM_RELEASE);
        g_index_relay = NULL;
    }
    if (results) for (i = 0; i < count; ++i) {
        if (results[i].name && !strcmp(results[i].name, "SerializedEntityIndexWrite")) {
            site = &results[i];
            break;
        }
    }
    if (!site || site->status != SIG_OK || !site->addr) {
        backend_log("serialized-entity-guard: unresolved instruction; NOT installed");
        return 0;
    }
    g_index_relay = index_relay_allocate(site->addr);
    if (!g_index_relay) {
        backend_log("serialized-entity-guard: relay allocation failed; NOT installed");
        return 0;
    }
    relative = (int32_t)((intptr_t)g_index_relay - (intptr_t)(site->addr + 5));
    memcpy(replacement + 1, &relative, sizeof relative);
    status = code_patch_sig(site, index_write, replacement, sizeof replacement, &g_index_patch);
    if (status != B2_PATCH_OK) {
        char message[160];
        _snprintf_s(message, sizeof message, _TRUNCATE,
            "serialized-entity-guard: %s; rollback pending=%d",
            sh_patch_status_str(status), g_index_patch.live);
        backend_log(message);
        if (!g_index_patch.live) {
            VirtualFree(g_index_relay, 0, MEM_RELEASE);
            g_index_relay = NULL;
        }
        return 0;
    }
    g_index_ready = 1;
    {
        SYSTEM_INFO info;
        char message[160];
        GetSystemInfo(&info);
        _snprintf_s(message, sizeof message, _TRUNCATE,
            "serialized-entity-guard: armed at RVA 0x%X; skipped-empty counter=%p",
            site->rva, g_index_relay + info.dwPageSize);
        backend_log(message);
    }
    return 1;
}
