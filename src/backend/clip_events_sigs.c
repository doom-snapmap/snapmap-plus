/* clip_events_sigs.c -- the three event lookups clip events detours (clip_events.h), kept apart so the
 * two-image test (tests/clip_events_sigs_test.c) links them without the engine side.
 *
 * Build 13954591 (snaphak-re shared Ghidra, logs/shared-ghidra/gk/d-vk-1416d7680, d-vk-1416d7810,
 * d-vk-1416d79b0, d-gl-1416d8190; paired with tools/re/site_pair.py). Each is
 * bool f(idDeclMD6 *model, idMD6Anim *anim, int from, int to[, int filter], idList *out): it empties `out`,
 * returns false for a NULL anim, queries the model's own event table (+0x228) with the anim's name (+0x08),
 * then each parent (+0x98) into a local list merged into `out` (VK 0x1416D9920, GL 0x1416DA4B0), and returns
 * whether `out` holds a row.
 *   Md6EventsInWindow  VK 0x1416D7680  GL 0x1416D8190  the window query VK 0x1416D71D0 / GL 0x1416D7CE0;
 *                      15-byte window (mov rax,rsp; push rdi; push r14; push r15; sub rsp,0x460).
 *   Md6EventsOfType    VK 0x1416D7810  GL 0x1416D8320  with a filter; query VK 0x1416D7350 / GL 0x1416D7E60.
 *   Md6EventsOfType2   VK 0x1416D79B0  GL 0x1416D84C0  with a filter; query VK 0x1416D7500 / GL 0x1416D8010.
 * The two filtered lookups are the same bytes up to their first query call (the pattern below, 196 bytes,
 * matches both in each executable); the call's target tells them apart, so each is accepted at its recorded
 * RVA only when its E8 at +0xC3 reaches its own query, checked by that query's own pattern (each query is the
 * only match in each executable). Both filtered windows are 16 bytes (mov rax,rsp; push r12; push r14;
 * push r15; sub rsp,0x460). No window holds a relative operand. */
#include <stddef.h>
#include <string.h>
#include "clip_events.h"

#define CE_FILTERED \
    "48 8B C4 41 54 41 56 41 57 48 81 EC 60 04 00 00 48 C7 44 24 30 FE FF FF FF 48 89 58 08 48 89 68 10 " \
    "48 89 70 18 48 89 78 20 45 8B F1 45 8B F8 48 8B FA 48 8B F1 48 8B 9C 24 A8 04 00 00 45 33 E4 44 39 63 0C " \
    "7D 34 0F B6 43 13 84 C0 74 06 2C 03 3C 01 77 22 48 8B 13 48 85 D2 74 13 48 8B 0D ?? ?? ?? ?? 48 8B 01 " \
    "41 B8 10 00 00 00 FF 50 10 4C 89 23 44 89 63 0C 44 89 63 08 8B 43 0C 85 C0 41 0F 4F C4 89 43 08 48 85 FF " \
    "75 07 32 C0 E9 ?? ?? ?? ?? 48 8B 47 08 48 89 84 24 A8 04 00 00 48 8D 8E 28 02 00 00 48 89 5C 24 28 " \
    "8B AC 24 A0 04 00 00 89 6C 24 20 45 8B CE 45 8B C7 48 8D 94 24 A8 04 00 00 E8"

const sh_ce_site_sig SH_CE_SITES[SH_CE_SITE_COUNT] = {
    { { "Md6EventsInWindow",
        "48 8B C4 57 41 56 41 57 48 81 EC 60 04 00 00 48 C7 44 24 30 FE FF FF FF 48 89 58 08 48 89 68 10 "
        "48 89 70 18 41 8B E9 45 8B F0 48 8B FA 48 8B F1 48 8B 9C 24 A0 04 00 00 45 33 FF 44 39 7B 0C 7D 34 "
        "0F B6 43 13 84 C0 74 06 2C 03 3C 01 77 22 48 8B 13 48 85 D2 74 13 48 8B 0D ?? ?? ?? ?? 48 8B 01 "
        "41 B8 10 00 00 00 FF 50 10 4C 89 3B 44 89 7B 0C 44 89 7B 08 8B 43 0C 85 C0 41 0F 4F C7 89 43 08 48 85 FF "
        "75 07 32 C0 E9 ?? ?? ?? ?? 48 8B 47 08 48 89 84 24 A0 04 00 00 48 8D 8E 28 02 00 00 48 89 5C 24 20 "
        "44 8B CD 45 8B C6 48 8D 94 24 A0 04 00 00 E8",
        0x16D7680u, 0x16D8190u },
      0xB3,
      { "Md6EventsQueryWindow",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 74 24 60 33 DB 41 8B E9 "
        "45 8B F8 4C 8B F2 48 8B F9 39 5E 0C 7D 32",
        0x16D71D0u, 0x16D7CE0u },
      15 },
    { { "Md6EventsOfType", CE_FILTERED, 0x16D7810u, 0x16D8320u },
      0xC3,
      { "Md6EventsQueryType",
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 4C 8B 74 24 68 33 DB 41 8B E9 "
        "45 8B F8 48 8B F2 48 8B F9 41 39 5E 0C 7D 35",
        0x16D7350u, 0x16D7E60u },
      16 },
    { { "Md6EventsOfType2", CE_FILTERED, 0x16D79B0u, 0x16D84C0u },
      0xC3,
      { "Md6EventsQueryType2",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 74 24 68 33 DB 41 8B E9 "
        "45 8B F8 4C 8B F2 48 8B F9 39 5E 0C 7D 32",
        0x16D7500u, 0x16D8010u },
      16 },
};

sig_status sh_ce_site_resolve(const uint8_t *module_base, const sh_ce_site_sig *s, sig_result *out)
{
    sig_result q;
    sig_status st;
    const uint8_t *call;
    int32_t rel;
    if (out) memset(out, 0, sizeof *out);
    if (!s || !out) return SIG_BAD_PATTERN;
    st = sig_resolve_one(module_base, &s->fn, out);
    if (st != SIG_OK) return st;
    st = sig_resolve_one(module_base, &s->query, &q);
    call = (const uint8_t *)out->addr + s->call_offset;
    if (st == SIG_OK && call[0] == 0xE8) {
        memcpy(&rel, call + 1, 4);
        if ((uintptr_t)(call + 5 + rel) == q.addr) return SIG_OK;
    }
    /* a lookalike, or a changed query: not the lookup recorded here */
    out->status = SIG_CHANGED;
    out->addr = 0;
    return SIG_CHANGED;
}
