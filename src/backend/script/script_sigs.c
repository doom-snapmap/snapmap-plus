/* script_sigs.c -- the private signatures of the mod-script hooks.
 * Kept apart so the two-image test (tests/script_sigs_test.c) links the table
 * without the engine side. Order matches enum sh_script_sig in script_hooks.h.
 *
 * Build 13954591 (shared Ghidra, snaphak-re logs/shared-ghidra/sc3-* and
 * sc1-*; every pattern is the only match in each executable):
 *   ScriptTick           VK 0x140E9C9D0  GL 0x140E9BF10  the SuperScript per-frame wrapper idGameLocal::RunFrame
 *                        calls once per game frame after every entity has thought (VK RunFrame 0x1403AE730 at
 *                        +0x3197, GL 0x1403ADE50). `ff 52 20` (its slot +0x20 call) separates it from the
 *                        slot-+0x18 sibling that follows it (VK 0x140E9C890 / GL 0x140E9BDC0). 15-byte window.
 *   ScriptSpawnIdLookup  VK 0x140393220  GL 0x140392AF0  idGameLocal vtable +0x7F0: spawn id -> object, NULL
 *                        when the entity is gone (script_handles.h). Checked against the live vtable; not hooked.
 *   ScriptIsClient       VK 0x140398370  GL 0x140397C80  idGameLocal::IsClient (the companions' pattern).
 *   ScriptOnDamage       VK 0x1408283A0  GL 0x140827560  OnDamage callback dispatcher (entity, attacker, amount);
 *                        15-byte window.
 *   ScriptKilled         VK 0x140400130  GL 0x1403FF610  idAI2::Killed; 21-byte window, before its stack probe.
 *   ScriptRelayDoAction  VK 0x140A0C430  GL 0x140A0B630  idSnapMapLogic_Relay::DoAction(this, activator);
 *                        16-byte window, before its RIP-relative load.
 *   ScriptOnAnimNotify   VK 0x140827F70  GL 0x140827130  OnAnimNotify callback dispatcher (entity, message);
 *                        15-byte window. Its bytes equal the OnMoverNotify dispatcher's (VK 0x140828990,
 *                        GL 0x140827B50) except for the callback-name string each loads at +0x4C, so it is
 *                        resolved by pattern plus that string (sh_script_resolve_named). */
#include <stddef.h>
#include "script_hooks.h"

const sig_entry SH_SCRIPT_SIGNATURES[] = {
    { "ScriptTick",
      "40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 89 74 24 58 48 8B F1 48 8B 79 10 "
      "48 85 FF 74 ?? 48 89 7C 24 28 48 8B 07 48 8B D1 48 8B CF FF 50 08 48 8B D8 48 89 44 24 30 "
      "48 8B 4E 10 48 8B 11 FF 52 20",
      0xE9C9D0u, 0xE9BF10u },
    { "ScriptSpawnIdLookup",
      "4C 63 12 4C 8B C1 44 3B 15 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 44 3B D0 75 08 "
      "48 8B 81 E8 53 06 00 C3",
      0x393220u, 0x392AF0u },
    { "ScriptIsClient",
      "40 55 48 83 EC 40 83 3D ?? ?? ?? ?? 00 48 8B E9 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
      "48 89 5C 24 50 48 89 74 24 58 48 89 7C 24 60 48 8B 01 4C 89 74 24 38 4C 89 7C 24 30 "
      "FF 90 A0 01 00 00 45 33 F6",
      0x398370u, 0x397C80u },
    { "ScriptOnDamage",
      "40 53 55 41 57 48 83 EC 40 8B 81 70 0C 00 00 33 DB 0F 29 74 24 30 0F 28 F2 44 8B FA 48 8B E9 "
      "85 C0 0F 8E 8E 00 00 00",
      0x8283A0u, 0x827560u },
    { "ScriptKilled",
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D8 EF FF FF B8 28 11 00 00 E8 ?? ?? ?? ?? "
      "48 2B E0 48 C7 45 70 FE FF FF FF 0F 29 B4 24 10 11 00 00 0F 29 BC 24 00 11 00 00",
      0x400130u, 0x3FF610u },
    { "ScriptRelayDoAction",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 "
      "75 27 38 83 89 0C 00 00 74 08 38 83 88 0C 00 00 75 17 4C 8B C7 C6 83 89 0C 00 00 01 BA 08 00 00 00",
      0xA0C430u, 0xA0B630u },
    { NULL, NULL, 0, 0 }
};

const sh_script_named_sig SH_SCRIPT_ANIM_NOTIFY_SIGNATURE = {
    { "ScriptOnAnimNotify",
      "40 53 55 41 57 48 83 EC 30 8B 81 70 0C 00 00 33 DB 4C 8B FA 48 8B E9 85 C0 0F 8E 88 00 00 00 "
      "48 89 7C 24 58 8B FB 48 89 74 24 50 4C 89 74 24 60 48 85 FF 78 54 3B D8 7D 50 48 8B 85 68 0C 00 00 "
      "48 8B 34 F8 48 85 F6 74 40 48 8B 06 48 8D 15",
      0x827F70u, 0x827130u },
    0x4C,
    "OnAnimNotify"
};

/* ---- named signatures ------------------------------------------------------ */

#include <windows.h>
#include <string.h>

#define NAMED_PATTERN_MAX 256

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "48 8B ?? ..." into bytes and a mask (1 = must match). Returns the length, 0 on a bad pattern. */
static size_t parse_pattern(const char *p, uint8_t *bytes, uint8_t *mask, size_t cap)
{
    size_t n = 0;
    while (p && *p) {
        if (*p == ' ') { p++; continue; }
        if (n >= cap) return 0;
        if (*p == '?') {
            bytes[n] = 0; mask[n] = 0; n++;
            p++;
            if (*p == '?') p++;
            continue;
        }
        if (hexval(p[0]) < 0 || hexval(p[1]) < 0) return 0;
        bytes[n] = (uint8_t)(hexval(p[0]) * 16 + hexval(p[1]));
        mask[n] = 1;
        n++;
        p += 2;
    }
    return n;
}

static int matches_at(const uint8_t *at, const uint8_t *bytes, const uint8_t *mask, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (mask[i] && at[i] != bytes[i]) return 0;
    return 1;
}

/* 1 when the lea rdx,[rip+disp32] at `lea` names a NUL-terminated `name`
 * inside the image. */
static int loads_name(const uint8_t *base, size_t image_size, const uint8_t *lea, const char *name)
{
    int32_t disp;
    size_t len = strlen(name);
    intptr_t target;
    if (lea[0] != 0x48 || lea[1] != 0x8D || lea[2] != 0x15) return 0;
    memcpy(&disp, lea + 3, 4);
    target = (intptr_t)(lea + 7 - base) + disp;
    if (target < 0 || (size_t)target + len + 1 > image_size) return 0;
    return !memcmp(base + target, name, len) && base[target + len] == 0;
}

sig_status sh_script_resolve_named(const uint8_t *module_base, const sh_script_named_sig *s, sig_result *out)
{
    uint8_t bytes[NAMED_PATTERN_MAX], mask[NAMED_PATTERN_MAX];
    size_t n, image_size;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *sec;
    int i, found = 0;
    uintptr_t hit = 0;
    if (out) memset(out, 0, sizeof *out);
    if (!s || !out) return SIG_BAD_PATTERN;
    out->name = s->sig.name;
    n = parse_pattern(s->sig.pattern, bytes, mask, sizeof bytes);
    if (!n || s->lea_offset + 3 > n || !s->callback) { out->status = SIG_BAD_PATTERN; return SIG_BAD_PATTERN; }
    dos = (const IMAGE_DOS_HEADER *)module_base;
    if (!module_base || dos->e_magic != IMAGE_DOS_SIGNATURE) { out->status = SIG_BAD_MODULE; return SIG_BAD_MODULE; }
    nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        out->status = SIG_BAD_MODULE;
        return SIG_BAD_MODULE;
    }
    image_size = nt->OptionalHeader.SizeOfImage;
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        size_t va = sec[i].VirtualAddress, size = sec[i].Misc.VirtualSize, k;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < n || va + size > image_size) continue;
        for (k = 0; k + n <= size; k++) {
            const uint8_t *at = module_base + va + k;
            if (at[0] != bytes[0] || !matches_at(at, bytes, mask, n)) continue;
            if (!loads_name(module_base, image_size, at + s->lea_offset, s->callback)) continue;
            found++;
            hit = (uintptr_t)at;
        }
    }
    if (!found) { out->status = SIG_NOT_FOUND; return SIG_NOT_FOUND; }
    if (found > 1) { out->status = SIG_AMBIGUOUS; return SIG_AMBIGUOUS; }
    out->status = SIG_OK;
    out->addr = hit;
    out->rva = (uint32_t)(hit - (uintptr_t)module_base);
    return SIG_OK;
}
