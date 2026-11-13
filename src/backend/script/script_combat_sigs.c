/* script_combat_sigs.c -- the private signatures of the mod-script combat
 * calls and of the damage, pain and projectile hooks, and the enum-table
 * finder. Kept apart so the two-image test (tests/script_combat_sigs_test.c)
 * links them without the engine side. Order matches enum sh_combat_sig.
 *
 * Build 13954591 (shared Ghidra, snaphak-re logs/shared-ghidra/SC6-*; each
 * pattern is the only match in each executable, at the recorded RVA):
 *   CombatDamageApply   VK 0x1403F9C40  GL 0x1403F9120  idAI2::Damage_Apply(ai, record): applies the record
 *                       idAI2::Damage built and Damage_Calculate settled (amount +0x1A30, scale +0x630,
 *                       direction +0x634, decl +0x628, zone per hit +0x1920), then calls the OnDamage
 *                       dispatcher (entity, attacker spawn id, scale * amount). 14-byte window.
 *   CombatSetPain       VK 0x140748710  GL 0x140748020  SetPainReaction(ai, painType, ms): writes the
 *                       current reaction at ai +0x5060; called by the pain states as a pain starts.
 *                       15-byte window.
 *   CombatProjLaunch    VK 0x140F3A380  GL 0x140F39840  idProjectile::Launch(projectile, owner, origin*,
 *                       axis*, ...): every projectile launch (AI weapons, ae_launchProjectile*, players)
 *                       stores the owner at +0x4788. 15-byte window.
 *   CombatRadiusDamage  VK 0x1403A21E0  GL 0x1403A1AF0  RadiusDamage(game, origin*, inflictor, attacker,
 *                       parms*, scale, bool, ignore): the ae_radiusDamage handler's call.
 *   CombatEntStartFx    VK 0x14082C270  GL 0x14082B430  idEntity StartFX(entity, condition, extraCondition):
 *                       unless the entity's +0x3EA bit 0 is set, the game clock and the FX manager's start by
 *                       condition (entity +0x30, time, condition, extra, tag -1). In every entity class's
 *                       vtable; what the game's own code calls on an effect entity.
 *   CombatEntStopFx     VK 0x14082D1D0  GL 0x14082C390  idEntity StopFX(entity, condition, extraCondition): the
 *                       FX manager's stop by condition (entity +0x30, time, condition, extra, not immediate). */
#include <stddef.h>
#include <string.h>
#include <windows.h>
#include "script_api_combat.h"

const sig_entry SH_COMBAT_SIGNATURES[] = {
    { "CombatDamageApply",
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 98 F7 FF FF",
      0x3F9C40u, 0x3F9120u },
    { "CombatSetPain",
      "4C 8B DC 45 89 43 18 57 48 81 EC 70 04 00 00 49 89 73 E0",
      0x748710u, 0x748020u },
    { "CombatProjLaunch",
      "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 98 48 81 EC 68 01 00 00 48 C7 45 E8 FE FF FF FF",
      0xF3A380u, 0xF39840u },
    { "CombatRadiusDamage",
      "48 83 EC 68 F3 0F 10 05 ?? ?? ?? ?? 48 8B 84 24 A8 00 00 00",
      0x3A21E0u, 0x3A1AF0u },
    { "CombatEntStartFx",
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 0F B6 81 EA 03 00 00 41 8B F8 F6 D0 8B F2 48 8B D9 A8 01 74 34 "
      "48 8B 0D ?? ?? ?? ?? 48 8D 54 24 40",
      0x82C270u, 0x82B430u },
    { "CombatEntStopFx",
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B D9 41 8B F8 48 8B 0D ?? ?? ?? ?? 8B F2 41 B8 01 00 00 00 "
      "48 8D 54 24 40 48 8B 01 FF 90 10 08 00 00 48 8D 4B 30 C6 44 24 20 00 44 8B CF 44 8B C6 8B 10 E8 ?? ?? ?? ?? "
      "48 8B 5C 24 48 48 8B 74 24 50 48 83 C4 30 5F C3 CC CC CC CC CC CC CC CC 48 8B C4 55 56 57 48 81 EC 80 00 00 "
      "00 48 C7 40 98 FE FF FF FF",
      0x82D1D0u, 0x82C390u },
    { NULL, NULL, 0, 0 }
};

/* ------------------------------------------------------------ names */

/* painType_t of build 13954591 (the enum table is identical on both renderers;
 * tests/script_combat_sigs_test.c compares it). */

static const char *const k_pain[SH_COMBAT_PAIN_COUNT] = {
    "PAIN_NONE", "PAIN_TWITCH", "PAIN_TWITCH_HEAVY", "PAIN_FALTER_LIGHT", "PAIN_INTERRUPT", "PAIN_STUN_LOOP",
    "PAIN_FALTER", "PAIN_PUSHBACK", "PAIN_STUN", "PAIN_STAGGER", "PAIN_STAGGER_VULNERABLE", "PAIN_KNOCKDOWN",
    "PAIN_DEATH", "PAIN_FULL_GIB"
};

const char *sh_script_combat_pain_name(int reaction)
{
    return reaction >= 0 && reaction < SH_COMBAT_PAIN_COUNT ? k_pain[reaction] : NULL;
}

/* soundChannel_t of build 13954591 (identical tables on both renderers). */
static const char *const k_channels[] = {
    "any", "voice", "voice2", "body", "body2", "body3", "body_loop", "weapon", "weapon_alert", "weapon_ambient",
    "weapon_ambient2", "weapon_loop", "weapon_charge_loop", "special_fire_loop", "item", "item2"
};

int sh_script_combat_channel(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < (int)(sizeof k_channels / sizeof k_channels[0]); i++)
        if (!_stricmp(name, k_channels[i])) return i;
    return -1;
}

/* ------------------------------------------------------------- enum tables */

#define ENUM_STRIDE 0x18u

static int image_bounds(const uint8_t *base, size_t *image_size, const IMAGE_SECTION_HEADER **sec, int *nsec)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS64 *nt;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    *image_size = nt->OptionalHeader.SizeOfImage;
    *sec = IMAGE_FIRST_SECTION(nt);
    *nsec = nt->FileHeader.NumberOfSections;
    return 1;
}

/* The string a pointer field names, when it lies in the image; else NULL. */
static const char *name_at(const uint8_t *base, size_t size, uint64_t image_base, uint64_t ptr)
{
    uint64_t rva;
    size_t k;
    if (ptr < image_base) return NULL;
    rva = ptr - image_base;
    if (rva >= size) return NULL;
    for (k = rva; k < size && k < rva + 128; k++) if (!base[k]) return (const char *)base + rva;
    return NULL;
}

uint32_t sh_script_combat_enum_find(const uint8_t *base, uint64_t pointer_base, const char *first, const char *second,
                                    const char *prefix, int *count)
{
    const IMAGE_SECTION_HEADER *sec;
    const IMAGE_NT_HEADERS64 *nt;
    size_t size, flen;
    int nsec, i;
    uint64_t image_base;
    if (count) *count = 0;
    if (!first || !second || !prefix || !image_bounds(base, &size, &sec, &nsec)) return 0;
    nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    image_base = pointer_base ? pointer_base : nt->OptionalHeader.ImageBase;
    flen = strlen(first);
    /* Each initialized, non-executable section: qword-aligned pointers to
     * `first` followed, one entry later, by a pointer to `second`. */
    for (i = 0; i < nsec; i++) {
        size_t va = sec[i].VirtualAddress, sz = sec[i].Misc.VirtualSize, k;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (!(sec[i].Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) || va + sz > size) continue;
        for (k = va; k + 2 * ENUM_STRIDE <= va + sz; k += 8) {
            uint64_t p0, p1;
            const char *n0, *n1;
            memcpy(&p0, base + k, 8);
            if (p0 < image_base || p0 - image_base >= size) continue;
            n0 = name_at(base, size, image_base, p0);
            if (!n0 || strncmp(n0, first, flen + 1)) continue;
            memcpy(&p1, base + k + ENUM_STRIDE, 8);
            n1 = name_at(base, size, image_base, p1);
            if (!n1 || strcmp(n1, second)) continue;
            if (count) {
                size_t e = k, plen = strlen(prefix);
                int n = 0;
                for (;;) {
                    uint64_t pe;
                    const char *ne;
                    if (e + 8 > va + sz) break;
                    memcpy(&pe, base + e, 8);
                    ne = name_at(base, size, image_base, pe);
                    if (!ne || strncmp(ne, prefix, plen)) break;
                    n++;
                    e += ENUM_STRIDE;
                }
                *count = n;
            }
            return (uint32_t)k;
        }
    }
    return 0;
}
