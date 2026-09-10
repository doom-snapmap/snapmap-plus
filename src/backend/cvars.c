/* Register the supported Snapmap+ cvars and insert them into engine lookup tables.
 * CvarRegister takes an embedded idCVar, name, default, flags, description, and
 * completion callback. Call the outer registration function so it initializes
 * the engine globals. Type bits are combined with NOCHEAT to allow console writes. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cvars.h"
#include "commands.h"
#include "signatures.h"
#include "engine_globals.h"
#include "host_image.h"
#include "backend_log.h"


typedef void (*cvar_register_fn)(void *self, const char *name, const char *def,
                                 uint32_t flags, const char *desc, void *argComp);

/* Use the engine name hash so insertion and FindCvar choose the same bucket. */
typedef int (*name_hash_fn)(const char *name);

/* Full-table offsets follow RegisterStaticVars. The developer-table alias exposes
 * this same list to the console. Resolve the singleton from CmdSystemLea +0x10,
 * then an independent global anchor, then the Vulkan-name-gated pinned RVA. */
#define CVARSYS_SLOT_RVA          0x55b7290u   /* Pinned Vulkan slot; the fallback gate checks its filename, not its hash. */
#define CVARSYS_OFF_FROM_CMDSYS   0x10         /* cvarSys .data slot == cmdSystem .data slot + 0x10 (adjacent) */
#define CVARSYS_LIST_PTR_OFF      0x08    /* idList<idCVar*> base: list-ptr */
#define CVARSYS_LIST_COUNT_OFF    0x10    /* idList count (int) */
#define CVARSYS_LIST_CAP_OFF      0x14    /* idList capacity (int) -- count==cap => engine grows */
#define CVARSYS_HASH_OFF          0x38    /* idHashIndex hash[] (bucket heads, int*) */
#define CVARSYS_CHAIN_OFF         0x40    /* idHashIndex indexChain[] (int*) */
#define CVARSYS_INDEXCHAINSZ_OFF  0x4c    /* idHashIndex indexChainSize (int) -- chain[] length */
#define CVARSYS_HASHMASK_OFF      0x54    /* idHashIndex hashMask (uint) */
#define CVARSYS_LOOKUPMASK_OFF    0x58    /* idHashIndex lookupMask (uint) */

/* Registered name in the embedded idCVar; it matches the table string we hash. */
#define IDCVAR_NAME_OFF           0x40

/* Our rows can register after the unlock sweep, so include settability up front. */
#define CVAR_FLAG_NOCHEAT         0x10u

/* Register only cvars with implemented consumers. The omitted render-count and
 * dash/meathook settings require features this product does not carry; see
 * docs/fidelity.md before restoring them. */
typedef struct cvar_row {
    const char *name;
    const char *def;
    uint32_t    type;   /* 1=BOOL, 2=INT, 4=FLOAT (ORed with CVAR_FLAG_NOCHEAT into the engine flags arg) */
    const char *desc;
} cvar_row;

static const cvar_row CVARS[] = {
    { "sh_pretty_on",                        "0",    1, "enables pretty printing of saved rawmap json" },
    { "sh_copy_reslist_to_clipboard",        "0",    1, "when sh_listres is used the contents will be copied to the clipboard" },
};
#define CVAR_COUNT ((int)(sizeof(CVARS) / sizeof(CVARS[0])))

/* Engine lists retain these objects for the process lifetime. Static backing is
 * 16-byte aligned for engine initialization and covers writes through +0x80. */
__declspec(align(16)) static uint8_t g_cvar_objs[CVAR_COUNT][0x400];

/* CvarRegister does not deduplicate; register and link each row only once. */
static volatile LONG g_installed = 0;

/* Register one cvar, SEH-guarded. Returns 1 on success, 0 if the engine call faulted. */
static int register_one(cvar_register_fn reg, int i)
{
    __try {
        reg(&g_cvar_objs[i][0], CVARS[i].name, CVARS[i].def,
            CVARS[i].type | CVAR_FLAG_NOCHEAT, CVARS[i].desc, NULL);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Read back name@+0x40 and nonzero backing bytes to check registration.
 * Return bit 1 for a matching name and bit 0 for any mutation. */
static int verify_one(int i)
{
    __try {
        const char *nm = *(const char * volatile *)(&g_cvar_objs[i][0x40]);
        int match = (nm != NULL && strcmp(nm, CVARS[i].name) == 0) ? 2 : 0;
        int mutated = 0;
        for (int b = 0; b < 0x100; b++) { if (g_cvar_objs[i][b]) { mutated = 1; break; } }
        return match | (mutated ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Engine idCVar stores integer and bool values at +0x30. */
#define IDCVAR_VALUE_INT_OFF 0x30

int sh_cvar_value_int(int index, int def)
{
    if (index < 0 || index >= CVAR_COUNT) return def;
    __try {
        return *(const volatile int32_t *)(&g_cvar_objs[index][IDCVAR_VALUE_INT_OFF]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return def;
    }
}

int sh_cvar_table_count(void)
{
    return CVAR_COUNT;
}

int sh_cvar_table_row(int index, const char **name, const char **def, const char **desc)
{
    if (index < 0 || index >= CVAR_COUNT) return 0;
    if (name) *name = CVARS[index].name;
    if (def)  *def  = CVARS[index].def;
    if (desc) *desc = CVARS[index].desc;
    return 1;
}

/* Late registration misses RegisterStaticVars, so explicitly append to the full
 * list and hash chain. Do not rerun RegisterStaticVars: its duplicate guard exits
 * the process. Refuse insertion if there is no spare capacity; do not reallocate
 * engine-owned tables. Return 1 for this inserted row, otherwise 0. */
static int cvar_findable_insert_one(uint8_t *cvarSys, name_hash_fn hashfn, int i)
{
    __try {
        const char *name = CVARS[i].name;
        void       *obj  = (void *)&g_cvar_objs[i][0];

        int  count = *(volatile int *)(cvarSys + CVARSYS_LIST_COUNT_OFF);
        int  cap   = *(volatile int *)(cvarSys + CVARSYS_LIST_CAP_OFF);
        int  ics   = *(volatile int *)(cvarSys + CVARSYS_INDEXCHAINSZ_OFF);
        if (count >= cap || count >= ics)
            return -1;

        /* Append before linking the corresponding hash-chain entry. */
        void **list = *(void ***)(cvarSys + CVARSYS_LIST_PTR_OFF);
        list[count] = obj;
        *(volatile int *)(cvarSys + CVARSYS_LIST_COUNT_OFF) = count + 1;

        /* Match FindCvar bucket math. Populated tables use lookupMask=0xFFFFFFFF,
         * so this also matches RegisterStaticVars insertion. */
        unsigned h    = (unsigned)hashfn(name);
        unsigned mask = *(volatile unsigned *)(cvarSys + CVARSYS_HASHMASK_OFF);
        unsigned look = *(volatile unsigned *)(cvarSys + CVARSYS_LOOKUPMASK_OFF);
        unsigned hb   = (h & mask) & look;
        int *bucket = *(int **)(cvarSys + CVARSYS_HASH_OFF);
        int *chain  = *(int **)(cvarSys + CVARSYS_CHAIN_OFF);
        chain[count] = bucket[hb];
        bucket[hb]   = count;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;                                               /* A memory fault leaves this insertion reported as skipped. */
    }
}

/* Resolve and read cvarSys, returning NULL if all address paths fail. */
static uint8_t *sh_resolve_cvarsys(const uint8_t *module_base)
{
    char line[160];

    /* Primary: CmdSystemLea decodes the adjacent command-system slot. */
    if (module_base) {
        for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
            if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "CmdSystemLea") != 0) continue;
            sig_result one;
            sig_status st = sig_resolve_one(module_base, &BACKEND_ENGINE_SIGNATURES[i], &one);
            if (st != SIG_OK && st != SIG_OK_HOOKED) break;   /* Try the independent global anchor next. */
            const uint8_t *cmdsys_slot = sh_decode_rip_slot((const uint8_t *)one.addr);
            if (!cmdsys_slot) break;
            const uint8_t *cvarsys_slot = cmdsys_slot + CVARSYS_OFF_FROM_CMDSYS;
            uint8_t *obj = NULL;
            if (sh_safe_read(cvarsys_slot, (uint8_t *)&obj, sizeof obj) && obj) {
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: cvarSys decoded slot=%p (cmdSystem+0x10) -> obj=%p (portable)",
                    (void *)cvarsys_slot, (void *)obj);
                backend_log(line);
                return obj;
            }
            break;
        }
        backend_log("B2: cvarSys portable decode failed -- trying the signed data-global anchor");
    }

    /* Independent anchor also works when CmdSystemLea has been detoured. */
    if (module_base) {
        glb_status gst = GLB_UNKNOWN_NAME;
        uintptr_t slot = glb_resolve(module_base, "cvar_system_slot", &gst);
        if (slot) {
            uint8_t *obj = NULL;
            if (sh_safe_read((const uint8_t *)slot, (uint8_t *)&obj, sizeof obj) && obj) {
                _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: cvarSys glb slot=%p -> obj=%p (portable)", (void *)slot, (void *)obj);
                backend_log(line);
                return obj;
            }
        } else {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: cvarSys glb anchor unresolved (status=%d)", (int)gst);
            backend_log(line);
        }
    }

    /* Last resort: pinned Vulkan RVA, gated by host filename rather than hash. */
    if (module_base && sh_host_is_pinned_rva_build()) {
        uint8_t *obj = NULL;
        __try {
            obj = *(uint8_t * volatile *)(module_base + CVARSYS_SLOT_RVA);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            obj = NULL;
        }
        if (obj) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: cvarSys pinned-build fallback *(base+0x55b7290)=%p", (void *)obj);
            backend_log(line);
            return obj;
        }
    }
    backend_log("B2: cvarSys UNRESOLVED -- cvar findable-insert will be skipped");
    return NULL;
}

int sh_cvars_install(void *cvar_register, const void *module_base)
{
    char line[160];

    if (cvar_register == NULL) {
        backend_log("B2: cvars SKIPPED -- CvarRegister not resolved");
        return 0;
    }
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) {
        backend_log("B2: cvars already registered (one-shot latch) -- skipping double-register");
        return 0;
    }

    cvar_register_fn reg = (cvar_register_fn)cvar_register;
    int ok = 0;
    for (int i = 0; i < CVAR_COUNT; i++)
        ok += register_one(reg, i);

    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: cvars registered %d/%d (register=%p, non-EXPOSE / gate-1-invisible, NOCHEAT so console sets work without dev mode; 2 of OG's 9 rows)",
        ok, CVAR_COUNT, cvar_register);
    backend_log(line);

    /* Inspect the backing objects separately from lookup-table visibility. */
    int matched = 0, mutated = 0;
    for (int i = 0; i < CVAR_COUNT; i++) {
        int r = verify_one(i);
        if (r & 2) matched++;
        if (r & 1) mutated++;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B2: cvar readback -- %d/%d name@+0x40 matched, %d/%d object-mutated (DIRECT engine-populated proof)",
        matched, CVAR_COUNT, mutated, CVAR_COUNT);
    backend_log(line);

    /* Make the late-registered rows findable through the full table and its alias. */
    uint8_t *cvarSys = (uint8_t *)sh_resolve_cvarsys((const uint8_t *)module_base);
    name_hash_fn hashfn = NULL;
    if (module_base) {
        /* Resolve the engine hash from the shared signature database. */
        for (size_t i = 0; BACKEND_ENGINE_SIGNATURES[i].name != NULL; i++) {
            if (strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "NameHash") != 0) continue;
            sig_result one;
            sig_status st = sig_resolve_one((const uint8_t *)module_base,
                                            &BACKEND_ENGINE_SIGNATURES[i], &one);
            if (st == SIG_OK || st == SIG_OK_HOOKED)
                hashfn = (name_hash_fn)one.addr;
            break;
        }
    }

    int inserted = 0, full_skips = 0, faults = 0;
    if (cvarSys == NULL || hashfn == NULL) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: cvar findable-insert SKIPPED -- cvarSys=%p hashfn=%p (module_base=%p)",
            (void *)cvarSys, (void *)hashfn, (void *)module_base);
        backend_log(line);
    } else {
        for (int i = 0; i < CVAR_COUNT; i++) {
            int r = cvar_findable_insert_one(cvarSys, hashfn, i);
            if (r == 1) inserted++;
            else if (r == -1) full_skips++;
            else faults++;
        }
        int count_after = -1, cap_after = -1;
        __try {
            count_after = *(volatile int *)(cvarSys + CVARSYS_LIST_COUNT_OFF);
            cap_after   = *(volatile int *)(cvarSys + CVARSYS_LIST_CAP_OFF);
        } __except (EXCEPTION_EXECUTE_HANDLER) { count_after = -1; cap_after = -1; }
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "B2: cvar findable-insert %d/%d (cvarSys=%p count=%d cap=%d full-skip=%d fault=%d)",
            inserted, CVAR_COUNT, (void *)cvarSys, count_after, cap_after, full_skips, faults);
        backend_log(line);
    }
    return ok;
}
