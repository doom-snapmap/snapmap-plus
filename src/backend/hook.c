/* Reversible 14-byte absolute detours. Callers choose the stolen-byte count;
 * this module neither decodes nor relocates instructions for its trampolines. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "hook.h"

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
    uint8_t orig[48];          /* the original `stolen` prologue bytes */
    size_t  stolen;            /* how many bytes were saved/overwritten */
    int     live;              /* 1 = patched, 0 = reverted / free slot */
} patch_record;

static patch_record g_patches[MAX_PATCHES];
static int          g_patch_count = 0;   /* high-water count of slots ever used */

void *install_inline_hook(void *target, void *detour, size_t stolen)
{
    if (stolen < 14 || stolen > 48) return NULL;

    patch_record *rec = NULL;
    for (int i = 0; i < g_patch_count; i++) {
        if (!g_patches[i].live) { rec = &g_patches[i]; break; }
    }
    if (!rec) {
        if (g_patch_count >= MAX_PATCHES) return NULL;
        rec = &g_patches[g_patch_count++];
    }

    uint8_t *tramp = (uint8_t *)VirtualAlloc(NULL, stolen + 14, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
    memcpy(tramp, target, stolen);
    write_absjmp(tramp + stolen, (uint8_t *)target + stolen);

    DWORD old;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return NULL;
    }
    memcpy(rec->orig, target, stolen);
    rec->target = target;
    rec->tramp  = tramp;
    rec->stolen = stolen;
    rec->live   = 1;

    uint8_t patch[48];
    write_absjmp(patch, detour);
    if (stolen > 14) memset(patch + 14, 0x90, stolen - 14);     /* Pad the overwritten region after the jump. */
    memcpy(target, patch, stolen);
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return tramp;
}

static int revert_record(patch_record *rec)
{
    if (!rec->live) return 0;
    DWORD old;
    if (VirtualProtect(rec->target, rec->stolen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(rec->target, rec->orig, rec->stolen);
        VirtualProtect(rec->target, rec->stolen, old, &old);
        FlushInstructionCache(GetCurrentProcess(), rec->target, rec->stolen);
    }
    if (rec->tramp) VirtualFree(rec->tramp, 0, MEM_RELEASE);
    rec->live = 0;
    return 1;
}

int hook_unpatch(void *tramp)
{
    for (int i = 0; i < g_patch_count; i++) {
        if (g_patches[i].live && g_patches[i].tramp == tramp)
            return revert_record(&g_patches[i]);
    }
    return 0;
}

int hook_unpatch_all(void)
{
    int n = 0;
    for (int i = g_patch_count - 1; i >= 0; i--)   /* Walk records in reverse slot order. */
        if (g_patches[i].live) n += revert_record(&g_patches[i]);
    return n;
}

int hook_installed_count(void)
{
    int n = 0;
    for (int i = 0; i < g_patch_count; i++) if (g_patches[i].live) n++;
    return n;
}
