/* Reversible 14-byte absolute detours. Callers choose the stolen-byte count;
 * this module neither decodes nor relocates instructions for its trampolines. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "hook.h"
#include "patch.h"

static void write_absjmp(uint8_t *p, void *dest)
{
    p[0] = 0xFF; p[1] = 0x25;                  /* jmp qword ptr [rip+0] */
    p[2] = p[3] = p[4] = p[5] = 0x00;
    *(uint64_t *)(p + 6) = (uint64_t)dest;     /* the absolute 8-byte target follows inline */
}

/* Fixed records retain original bytes and trampoline ownership until unpatch. */
#define MAX_PATCHES 64
typedef struct patch_record {
    void   *target;            /* engine fn whose prologue we overwrote */
    void   *tramp;             /* the trampoline (also this record's identity key) */
    void   *detour;
    size_t stolen;
    int    installed;
    sh_patch_handle patch;    /* Includes partial writes and pending protection restoration. */
} patch_record;

static patch_record g_patches[MAX_PATCHES];
static int          g_patch_count = 0;   /* high-water count of slots ever used */

void *hook_prepare(void *target, void *detour, size_t stolen)
{
    if (!target || !detour || stolen < 14 || stolen > 48) return NULL;

    patch_record *rec = NULL;
    for (int i = 0; i < g_patch_count; i++) {
        if (g_patches[i].tramp && g_patches[i].target == target) return NULL;
        if (!g_patches[i].patch.live && !g_patches[i].tramp) rec = &g_patches[i];
    }
    if (!rec) {
        if (g_patch_count >= MAX_PATCHES) return NULL;
        rec = &g_patches[g_patch_count++];
    }

    uint8_t *tramp = (uint8_t *)VirtualAlloc(NULL, stolen + 14, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
    __try { memcpy(tramp, target, stolen); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return NULL;
    }
    write_absjmp(tramp + stolen, (uint8_t *)target + stolen);
    FlushInstructionCache(GetCurrentProcess(), tramp, stolen + 14);
    rec->target = target;
    rec->tramp  = tramp;
    rec->detour = detour;
    rec->stolen = stolen;
    rec->installed = 0;
    memset(&rec->patch, 0, sizeof rec->patch);
    return tramp;
}

static patch_record *find_record(void *tramp)
{
    if (!tramp) return NULL;
    for (int i = 0; i < g_patch_count; i++)
        if (g_patches[i].tramp == tramp) return &g_patches[i];
    return NULL;
}

sh_patch_status hook_commit(void *tramp)
{
    patch_record *rec = find_record(tramp);
    if (!rec) return B2_PATCH_REFUSED_BADARG;
    if (rec->installed) return B2_PATCH_OK;
    if (rec->patch.live) return B2_PATCH_FAIL_ROLLBACK;
    uint8_t patch[48];
    write_absjmp(patch, rec->detour);
    if (rec->stolen > 14) memset(patch + 14, 0x90, rec->stolen - 14);
    MemoryBarrier(); /* The caller's original callback and detour dependencies precede target writes. */
    sh_patch_status result = code_patch(rec->target, tramp, patch, rec->stolen, &rec->patch);
    if (result == B2_PATCH_OK) rec->installed = 1;
    return result;
}

int hook_is_installed(void *tramp)
{
    patch_record *rec = find_record(tramp);
    return rec && rec->installed;
}

static int revert_record(patch_record *rec)
{
    if (!rec->tramp) return 0;
    rec->installed = 0;
    if (rec->patch.live && code_unpatch(&rec->patch) != B2_PATCH_OK) return 0;
    if (!VirtualFree(rec->tramp, 0, MEM_RELEASE)) return 0;
    rec->tramp = NULL;
    return 1;
}

int hook_unpatch(void *tramp)
{
    for (int i = 0; i < g_patch_count; i++) {
        if (g_patches[i].tramp && g_patches[i].tramp == tramp)
            return revert_record(&g_patches[i]);
    }
    return 0;
}

int hook_unpatch_all(void)
{
    int n = 0;
    for (int i = g_patch_count - 1; i >= 0; i--)   /* Walk records in reverse slot order. */
        if (g_patches[i].tramp) n += revert_record(&g_patches[i]);
    return n;
}

int hook_installed_count(void)
{
    int n = 0;
    for (int i = 0; i < g_patch_count; i++) if (g_patches[i].installed) n++;
    return n;
}

int hook_owned_count(void)
{
    int n = 0;
    for (int i = 0; i < g_patch_count; i++) if (g_patches[i].tramp) n++;
    return n;
}
