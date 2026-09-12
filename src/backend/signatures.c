/* Resolve masked signatures in mapped executable sections, independent of names.
 * Use the longest fixed run as an anchor, then verify the full pattern.
 * Multiple matches are rejected; unreadable section tails stop that section scan. */
#include "signatures.h"
#include "host_image.h"
#include <string.h>

const sig_entry NAV_RENDER_TARGET_GL_SIGNATURE = {
    "NavRenderTargetGL",
    "48 89 4C 24 08 53 55 57 41 55 48 83 EC 38 48 8B D9 B9 40 8D 00 00 "
    "45 8B E9 41 8B E8 48 8B FA 48 85 D2 75 18 FF 15 ?? ?? ?? ?? "
    "B9 01 00 00 00 89 4B 28",
    0x19231e0u
};

/* Pattern compilation and matching. */

#define SIG_MAX_PATTERN 256   /* Maximum supported pattern length. */

static int parse_token(const char *tok, size_t len, uint8_t *b, uint8_t *m)
{
    if (len == 1 && tok[0] == '?') { *b = 0; *m = 0; return 1; }
    if (len == 2 && tok[0] == '?' && tok[1] == '?') { *b = 0; *m = 0; return 1; }
    if (len != 2) return 0;
    int hi = -1, lo = -1;
    for (int i = 0; i < 2; i++) {
        char c = tok[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return 0;
        if (i == 0) hi = v; else lo = v;
    }
    *b = (uint8_t)((hi << 4) | lo);
    *m = 0xFF;
    return 1;
}

static size_t compile_pattern(const char *pattern, uint8_t *pat, uint8_t *mask, size_t cap)
{
    size_t n = 0;
    const char *p = pattern;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - tok);
        if (n >= cap) return 0;
        if (!parse_token(tok, len, &pat[n], &mask[n])) return 0;
        n++;
    }
    return n;
}

static void longest_fixed_run(const uint8_t *mask, size_t n, size_t *off, size_t *len)
{
    size_t best_off = 0, best_len = 0, cur_off = 0, run = 0;
    for (size_t i = 0; i < n; i++) {
        if (mask[i]) {
            if (run == 0) cur_off = i;
            run++;
            if (run > best_len) { best_len = run; best_off = cur_off; }
        } else {
            run = 0;
        }
    }
    *off = best_off; *len = best_len;
}

static int matches_at(const uint8_t *blob, const uint8_t *pat, const uint8_t *mask, size_t n, size_t i)
{
    for (size_t k = 0; k < n; k++)
        if (mask[k] && blob[i + k] != pat[k]) return 0;
    return 1;
}

static long find_bytes(const uint8_t *blob, size_t end, const uint8_t *needle, size_t nn, size_t start)
{
    if (nn == 0 || nn > end) return -1;
    size_t last = end - nn;
    for (size_t i = start; i <= last; i++) {
        if (blob[i] == needle[0] && memcmp(blob + i, needle, nn) == 0)
            return (long)i;
    }
    return -1;
}

/* Mapped executable sections. */

typedef struct exec_section {
    const uint8_t *base;   /* module_base + VirtualAddress (mapped) */
    uint32_t       vaddr;  /* RVA of the section */
    uint32_t       vsize;  /* mapped size to scan */
} exec_section;

#define MAX_SECTIONS 64

static int collect_exec_sections(const uint8_t *module_base, exec_section *out, int cap)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(module_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return -1;

    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = nt->FileHeader.NumberOfSections;
    int count = 0;
    for (int i = 0; i < n && count < cap; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint32_t vsize = sec[i].Misc.VirtualSize;
        if (vsize == 0) vsize = sec[i].SizeOfRawData;
        if (vsize == 0) continue;
        out[count].base  = module_base + sec[i].VirtualAddress;
        out[count].vaddr = sec[i].VirtualAddress;
        out[count].vsize = vsize;
        count++;
    }
    return count;
}

/* A detour can erase a signature prologue while leaving its tail intact. After
 * a scan miss, known_rva may identify that hooked entry on a fingerprinted image
 * when the jump form and enough fixed tail bytes match. The RVA belongs to one extraction build;
 * it is not a portable substitute for signature resolution. */

#define HOOK_MAX_STEAL   24   /* widest plausible whole-instruction steal window a detour overwrites */
#define HOOK_MIN_TAIL     6   /* require at least this many FIXED tail bytes to match (anti-coincidence) */

/* Recognize common x64 detour jumps and return their minimum opcode length.
 * Tail matching, rather than this partial decoder, validates the fallback. */
static int detour_jmp_len(const uint8_t *p)
{
    if (p[0] == 0xE9) return 5;                    /* jmp rel32 */
    if (p[0] == 0xEB) return 2;                    /* jmp rel8  */
    if (p[0] == 0xFF) {
        uint8_t modrm = p[1];
        uint8_t reg   = (uint8_t)((modrm >> 3) & 7);
        if (reg == 4) {                            /* FF /4 = jmp r/m64 (incl. FF 25 rip-relative) */
            if ((modrm & 0xC7) == 0x25) return 6;  /* FF 25 disp32 (the >2GB abs-jmp our hook.c uses) */
            return 2;                              /* other jmp r/m forms (reg/[reg]); slide finds tail */
        }
    }
    /* REX-prefixed FF /4 (rare for a detour, but be lenient) */
    if ((p[0] & 0xF0) == 0x40 && p[1] == 0xFF && ((p[2] >> 3) & 7) == 4) {
        if ((p[2] & 0xC7) == 0x25) return 7;
        return 3;
    }
    return 0;
}

/* SEH-guarded copy of up to n bytes from src into dst; returns 1 if all read, 0 on access violation. */
static int sig_safe_read(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try {
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Try the known_rva hook-tolerant fallback. Returns 1 (and fills out as SIG_OK_HOOKED) if the live
 * bytes at module_base+known_rva are a detour whose post-prologue tail matches the sig; else 0. */
static int try_hooked_known_rva(const uint8_t *module_base, const sig_entry *sig,
                                const uint8_t *pat, const uint8_t *mask, size_t n, sig_result *out)
{
    if (sig->known_rva == 0 || !sh_host_is_pinned_rva_image(module_base)) return 0;
    const uint8_t *site = module_base + sig->known_rva;

    uint8_t live[SIG_MAX_PATTERN];
    size_t  want = n < SIG_MAX_PATTERN ? n : SIG_MAX_PATTERN;
    if (!sig_safe_read(site, live, want)) return 0;       /* unreadable -> not a confident hook hit */

    int jlen = detour_jmp_len(live);
    if (jlen == 0) return 0;                               /* No recognized detour at the recorded location. */

    /* Try plausible stolen-byte windows and require enough unchanged fixed tail. */
    for (size_t k = (size_t)jlen; k <= HOOK_MAX_STEAL && k < n; k++) {
        int fixed_tail = 0, ok = 1;
        for (size_t j = k; j < n; j++) {
            if (!mask[j]) continue;
            fixed_tail++;
            if (live[j] != pat[j]) { ok = 0; break; }
        }
        if (ok && fixed_tail >= HOOK_MIN_TAIL) {
            out->status = SIG_OK_HOOKED;
            out->addr   = (uintptr_t)site;
            out->rva    = sig->known_rva;
            return 1;
        }
    }
    return 0;
}

/* Resolution. */

sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{
    out->name = sig->name;
    out->status = SIG_NOT_FOUND;
    out->addr = 0;
    out->rva = 0;

    uint8_t pat[SIG_MAX_PATTERN], mask[SIG_MAX_PATTERN];
    size_t n = compile_pattern(sig->pattern, pat, mask, SIG_MAX_PATTERN);
    if (n == 0) { out->status = SIG_BAD_PATTERN; return out->status; }

    exec_section secs[MAX_SECTIONS];
    int nsec = collect_exec_sections(module_base, secs, MAX_SECTIONS);
    if (nsec < 0) { out->status = SIG_BAD_MODULE; return out->status; }

    size_t a_off, a_len;
    longest_fixed_run(mask, n, &a_off, &a_len);
    if (a_len == 0) { out->status = SIG_BAD_PATTERN; return out->status; }  /* all-wildcard: not a real sig */
    const uint8_t *anchor = pat + a_off;

    uintptr_t found_addr = 0;
    uint32_t  found_rva = 0;
    int       hits = 0;

    for (int s = 0; s < nsec && hits < 2; s++) {
        const uint8_t *blob = secs[s].base;
        uint32_t       size = secs[s].vsize;
        if (size < n) continue;
        size_t scan_end = size;
        size_t pos = a_off;
        __try {
            for (;;) {
                long apos = find_bytes(blob, scan_end, anchor, a_len, pos);
                if (apos < 0) break;
                long start = apos - (long)a_off;
                if (start < 0) { pos = (size_t)apos + 1; continue; }
                if ((size_t)start + n > scan_end) break;
                if (matches_at(blob, pat, mask, n, (size_t)start)) {
                    hits++;
                    if (hits == 1) {
                        found_rva  = secs[s].vaddr + (uint32_t)start;
                        found_addr = (uintptr_t)(blob + start);
                    } else {
                        break;
                    }
                }
                pos = (size_t)apos + 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* Stop this section on an unreadable page; retain earlier matches. */
        }
    }

    if (hits == 0) {
        /* A missing prologue may have been replaced by a validated detour. */
        if (try_hooked_known_rva(module_base, sig, pat, mask, n, out))
            return out->status;   /* SIG_OK_HOOKED */
        out->status = SIG_NOT_FOUND;
        return out->status;
    }
    if (hits > 1)  { out->status = SIG_AMBIGUOUS; return out->status; }
    out->status = SIG_OK;
    out->addr = found_addr;
    out->rva = found_rva;
    return SIG_OK;
}

size_t sig_db_count(void)
{
    size_t n = 0;
    while (BACKEND_ENGINE_SIGNATURES[n].name != NULL) n++;
    return n;
}

size_t sig_resolve_all(const uint8_t *module_base, sig_result *results, size_t cap)
{
    size_t ok = 0;
    size_t i = 0;
    for (; BACKEND_ENGINE_SIGNATURES[i].name != NULL && i < cap; i++) {
        sig_resolve_one(module_base, &BACKEND_ENGINE_SIGNATURES[i], &results[i]);
        /* Both a clean scan hit and a hook-tolerant known_rva hit count as resolved (present+callable). */
        if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED) ok++;
    }
    return ok;
}

uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if ((results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED) &&
            results[i].name && strcmp(results[i].name, name) == 0)
            return results[i].addr;
    return 0;
}

/* Engine signatures and extraction-build references.
 * known_rva belongs to the pinned Vulkan image only:
 * wrapped SHA256 139763E94F1A75B5310179F9EEEB8A949A1F53C49ACBC722FCFC5DFE7BB6D323
 * unpacked SHA256 5AD2548CEAB3DFA27F271E381222CA4A84DCAC7CB83CCE9446444C30C8309367.
 * Other builds require individual address mapping; no uniform delta is valid.
 * A zero known_rva disables fallback. Preserve extracted patterns and validate
 * entry changes with tests/run-tests.ps1 -Doom <pinned> -DoomAlt <other-renderer>.
 * CI has no game image and cannot verify these identities. */
const sig_entry BACKEND_ENGINE_SIGNATURES[] = {
    /* Resized-room containment: independently verified on Vulkan and OpenGL. */
    { "GridRefreshPlacementSurfaces",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 89 6C 24 50 "
      "48 89 74 24 58 49 8B E8 48 8B F2 48 8B D9 E8 ?? ?? ?? ?? 8B BE 58 07 00 00 3B 7B 0C", 0 },
    { "GridContainingModule",
      "48 8B C4 48 89 58 08 48 89 68 10 57 48 81 EC B0 00 00 00 F3 0F 10 05 ?? ?? ?? ?? 4C 8D 4C 24 40 F3 0F 10 0D ?? ?? ?? ?? 48 8B DA", 0 },
    { "GridModuleRay",
      "40 55 53 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 30 80 79 48 00 48 8B DA 48 8B 95 B8 00 00 00 49 8B F9 75 40 48 C7 45 88 00 00 00 00", 0 },
    /* Native map rendering controls and the environment far-clip read.
     * Each binding is unique in both independently linked renderer images. */
    { "RenderSettingsEnter",
      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 9A 88 10 02 00 48 8B E9 48 8B F2 48 8B BB E0 09 00 00 48 8B CF 48 8B 07 FF 90 E8 00 00 00", 0 },
    { "RenderSettingsExit",
      "40 53 48 83 EC 20 48 8B 82 88 10 02 00 48 8B DA 48 8B 88 E0 09 00 00 48 8B 01 FF 90 F0 00 00 00 48 8B 8B 88 10 02 00 C6 83 24 36 02 00 00 E8 ?? ?? ?? ??", 0 },
    { "RenderSettingsPopulate",
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 BF FF FF B8 10 41 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 45 A0 FE FF FF FF 48 89 9C 24 60 41 00 00", 0 },
    { "RenderSettingsApply",
      "40 53 48 83 EC 20 0F B6 01 48 8B D9 48 8B 51 48 48 81 C2 28 36 02 00 88 02 0F B6 41 01 88 42 01 0F B6 41 02 88 42 02 0F B6 41 03 88 42 03 0F B6 41 04", 0 },
    { "RenderSettingsReset",
      "40 53 48 83 EC 20 48 8B 51 48 48 8B D9 48 81 C2 28 36 02 00 0F B6 02 88 01 0F B6 42 01 88 41 01 0F B6 42 02 88 41 02 0F B6 42 03 88 41 03 0F B6 42 04", 0 },
    { "RenderSettingsDirtyCall",
      "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? C7 44 24 30 16 00 00 00 45 33 C0 48 8D 54 24 30 48 8B CB E8 ?? ?? ?? ?? C6 47 08 01 48 8D 15 ?? ?? ?? ??", 0 },
    { "RenderAddFloat",
      "48 8B C4 57 41 56 41 57 48 81 EC 80 00 00 00 48 C7 40 A8 FE FF FF FF 48 89 58 10 48 89 68 18 48 89 70 20 4D 8B F1 4D 8B F8 48 8B EA 48 8B F1 B9 38 01 00 00 E8 ?? ?? ?? ?? 48 8B F8 48 89 84 24 A0 00 00 00 48 85 C0 74 ?? 8B 9E E0 01 00 00 8D 53 01 89 96 E0 01 00 00 48 8B CE E8 ?? ?? ?? ?? 4C 8B C0 44 8B CB", 0 },
    { "RenderAddTitle",
      "48 8B C4 57 41 56 41 57 48 81 EC 90 00 00 00 48 C7 40 98 FE FF FF FF 48 89 58 10 48 89 68 18 48 89 70 20 48 8B DA 48 8B E9 B9 20 00 00 00 E8 ?? ?? ?? ??", 0 },
    { "RenderClipRead",
      "E8 ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? 0F 57 DB 44 0F 28 84 24 D0 00 00 00 F3 0F 10 D8 F3 0F 10 05 ?? ?? ?? ?? 0F 28 CA F3 0F 5F CB 0F 2E C1 7A ??", 0 },
    { "RenderParmFromOp",
      "0F B7 41 02 B9 3F 09 00 00 66 3B C1 7D ?? 48 8B 0D ?? ?? ?? ?? 0F BF D0 E9 ?? ?? ?? ?? 33 C0 C3 48 83 EC 58 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40", 0 },
    { "RenderVariablesResize",
      "48 89 4C 24 08 55 56 57 41 56 41 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 9C 24 88 00 00 00 8B EA 4C 8B F1 85 D2 7F ?? 0F B6 41 13 33 DB 84 C0 74 ?? 2C 03 3C 01 77 ?? 48 8B 09 48 85 C9 74 ?? 41 8B 56 0C E8 ?? ?? ?? ?? 49 89 1E 41 89 5E 0C 41 89 5E 08 E9 ?? ?? ?? ?? 8B 41 0C 3B E8 0F 84 ?? ?? ?? ?? 0F B6 49 13 80 F9 02 0F 85 ?? ?? ?? ?? 3B E8 0F 8E ?? ?? ?? ?? 41 0F B6 56 12 45 33 C0 8B CD E8 ?? ?? ?? ?? 4C 8B F8 48 89 84 24 80 00 00 00 48 85 C0 75 ?? 32 C0 E9 ?? ?? ?? ?? 41 C6 46 13 00 33 DB 89 5C 24 78 41 39 5E 08 7E ?? 48 63 C3 48 6B F0 68 49 8B 16 48 03 D6 4A 8D 0C 3E E8 ?? ?? ?? ?? 49 8B 3E 48 03 FE 48 8D 4F 38 E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 49 8B 06 48 8D 0C 06 48 89 8C 24 80 00 00 00 48 85 C9", 0 },
    { "DeserializeFromJson",
      "40 55 56 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 C7 44 24 68 FE FF FF FF",
      0x5EA490u },
    { "SerializeToJson",
      "40 53 56 57 48 81 EC E0 00 00 00 48 C7 44 24 70 FE FF FF FF",
      0x5F2390u },
    { "EditorMapToJson", /* Constructs an idSnapMap from idSnapMapEdit, serializes,
                          * and destroys the temporary. No save-slot writes. */
      "40 53 56 57 48 81 EC B0 07 00 00 48 C7 44 24 20 FE FF FF FF "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 A0 07 00 00 "
      "41 0F B6 F0 48 8B FA 48 8B D9 48 8D 4C 24 30 E8 ?? ?? ?? ?? "
      "90 48 8D 54 24 30 48 8B CB E8 ?? ?? ?? ?? 44 0F B6 C6 48 8B D7",
      0x59D2F0u },
    { "NavRenderBegin",
      "40 56 57 41 56 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 58 48 89 6C 24 60 48 8B EA 48 8B D9 80 B9 EC 00 00 00 00",
      0xd72a20u },
    { "NavRenderEnd",
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 C6 81 EC 00 00 00 00",
      0xd72c40u },
    { "NavRenderMatrix",
      "48 89 5C 24 18 55 56 57 48 8D 6C 24 B9 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 37 48 63 81 AC FB 00 00 48 8B F2 48 63 91 B4 FB 00 00 48 8B D9 44 8B 84 91 70 94 00 00 44 0B 84 81 70 94 00 00 48 63 81 B8 FB 00 00 44 0B 84 81 70 94 00 00 48 63 81 B0 FB 00 00 44 0B 84 81 70 94 00 00 44 85 81 7C B9 00 00 74 ?? B2 01 E8 ?? ?? ?? ?? 48 8D 45 27 48 8B CE 48 8D 7B 60 48 89 7C 24 28 4C 8D 4D 17 4C 8D 45 07 48 89 44 24 20 48 8D 55 F7 E8 ?? ?? ?? ?? 0F 28 45 F7",
      0xd737a0u },
    { "NavRenderLines",
      "40 55 48 83 EC 30 48 8B 69 18",
      0xdec860u },
    { "NavRenderStage",
      "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00",
      0xdb9870u },
    { "NavPostProcessSlot", /* Registered renderer job, not a renderer-specific function RVA. */
      "48 8B 15 ?? ?? ?? ?? 41 B1 01 48 8B CE E8 ?? ?? ?? ??",
      0xDBA0DCu },
    { "BuildAASFindCall", /* R14 is the exact compiled module-instance record. */
      "E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 75 44 45 33 C9 45 33 C0 48 8B 55 A8 "
      "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F8 41 B4 01",
      0x4EC1E9u },
    { "BuildAASLoadCall", /* Temporary-resource branch, released after merge. */
      "E8 ?? ?? ?? ?? 48 8B F8 41 B4 01 48 85 C0 75 23 48 8D 4D 98 E8 ?? ?? ?? ?? "
      "90 48 8D 4D C8 E8 ?? ?? ?? ?? 44 0F B6 64 24 40 48 8B 7C 24 48",
      0x4EC207u },
    { "NavSearchHeapPush", /* Same verified leaf on Vulkan and OpenGL. */
      "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 4D 8B 08 33 C9 "
      "44 0F B7 1A 49 8B D8 48 8B FA 66 41 39 49 04",
      0x6C12D0u },
    { "BlockingVolumeObstacleGate", /* The native affectsNavmesh contents mask gate. */
      "80 BB 8E 0C 00 00 00 74 0A 81 A3 94 0C 00 00 FF FF FD FF "
      "48 8B 8B 08 08 00 00 41 83 C8 FF 8B 93 94 0C 00 00 48 8B 01 FF 50 30",
      0x987F18u },
    { "SnapMapEditToSnapBuild", /* int(edit map, build map, ctx). Entry precedes all three BuildAAS calls
                                 * (+0x35C/+0x373/+0x38A), so nav_play reads live marks here
                                 * before conversion invalidates the editable entity state. */
      "48 8B C4 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 C0 FE FF FF FF "
      "48 89 58 08 48 89 70 18 48 89 78 20 4D 8B E0 48 8B F2 4C 8B E9 "
      "4C 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D E0 E8 ?? ?? ?? ?? 90 "
      "4C 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D C8",
      0x4F27B0u },
    { "AddCommand",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
      "41 54 41 56 41 57 48 83 EC 20 48 63 69 10",
      0x1AA3630u },
    { "SpawnByEntityDef",
      "40 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 89 74 24 60 49 8B C0",
      0x315AF0u },
    { "NameHash",          /* Case-insensitive h=h*31+tolower(c), used by cvar lookup/insertion.
                            * The multiply and lowercase branch distinguish the leaf. */
      "0F B6 01 45 33 C0 4C 8B C9 84 C0 74 ?? 0F 1F 00 8D 50 BF 80 FA 19 77 ?? "
      "04 20 45 6B C0 1F 49 FF C1 0F BE C8 41 0F B6 01 44 03 C1",
      0x1A00480u },
    { "GetDeclsOfType",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 8B F9 48 85 DB 74 ?? "
      "0F 1F 80 00 00 00 00 48 8B 4B 10",
      0x1800D20u },
    { "SnapPaletteBuild", /* void(palette, progress). Rebuild after registering new decl identities.
                             * NULL progress is allowed; callers validate the live palette vtable. */
      "48 8B C4 56 57 41 54 41 56 41 57 48 81 EC 70 07 00 00",
      0x54AEE0u },
    /* Native declaration registration contracts:
     * DeclRegistryAnchor: decode MOV RCX,[rip+slot] at +0x10 from a clean entry.
     * DeclRegisterFile: registry vtable +0x38; const idStr *source points to a
     * constructed 48-byte name. An optional default type guides source scanning.
     * DeclTypeByName: registry +0x58, short type name -> type manager.
     * DeclFind: manager/name/makeDefault; callers pass 0 when classifying shadows.
     * DeclSourceFind: manager/logical name -> source record, without creating a
     * live decl. Omit .decl; the engine normalizes the name.
     * decl_server validates resolved methods against the live registry vtable. */
    { "DeclRegistryAnchor",
      "40 53 48 83 EC 30 48 8B D9 4C 8D 05 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
      "48 8B D3 48 8B 01 FF 90 C0 00 00 00",
      0x184E1D0u },
    { "DeclRegisterFile",
      "40 57 48 81 EC A0 00 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 C8 00 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00 49 8B F8 48 8B D9 "
      "48 8D 4C 24 30 E8 ?? ?? ?? ?? 90 48 8D 4C 24 30 E8 ?? ?? ?? ?? "
      "48 8D 4C 24 30 E8 ?? ?? ?? ??",
      0x17B7330u },
    { "DeclTypeByName",
      "48 89 6C 24 18 56 48 83 EC 20 48 8B EA 48 8B F1 48 85 D2 74 ?? 80 3A 00 74 ?? "
      "48 89 5C 24 30 33 DB 48 89 7C 24 38 39 59 10",
      0x17B43B0u },
    { "DeclFind",
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 FD FF FF 48 81 EC 10 03 00 00 "
      "48 C7 44 24 68 FE FF FF FF 48 89 9C 24 68 03 00 00",
      0x17B36F0u },
    { "DeclSourceFind",
      "48 8B C4 55 57 41 54 41 56 41 57 48 8D A8 38 FE FF FF 48 81 EC A0 02 00 00 "
      "48 C7 44 24 20 FE FF FF FF 48 89 58 18 48 89 70 20",
      0x17B34B0u },
    { "ResourceStaticPromote", /* void(void). Lock the whole resource registry and promote each entry
                            * to level 4 at resource+0x28. Map purge masks 1/2 cannot free
                            * level 4, so newly published decls survive map transitions.
                            * Anchor: mov dword [rdx+0x28],4 and the registry walk. */
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF E8 ?? ?? ?? ?? 48 8B D8 B2 01 "
      "48 8B C8 E8 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 4D 85 C0 74 ?? 0F 1F 00 "
      "41 8B 48 28 83 E9 01 48 63 C9 78 ?? 0F 1F 40 00 49 8B 40 20 48 8B 14 C8 "
      "C7 42 28 04 00 00 00",
      0x1801830u },
    { "ResourceGenericLoad", /* void(idResource*). Reload through native destruction/reconstruction,
                            * preserving identity before parsing source and running post-parse.
                            * DeclFind normally reaches this through the pending-load path;
                            * the decl server can use it when the pending bit survives lookup.
                            * Anchor: 0x900-byte frame and owner-vtable access. */
      "48 8B C4 57 48 81 EC 00 09 00 00 48 C7 44 24 40 FE FF FF FF 48 89 58 10 "
      "48 89 70 18 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 F0 08 00 00 48 8B F9 "
      "48 89 4C 24 38 48 8B 01 FF 50 10 48 8B C8 E8 ?? ?? ?? ?? 4C 8B C0 48 8B 57 08 "
      "48 8B CF E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ??",
      0x17FF5F0u },
    { "CmdExecuteBuffer",  /* void(cmdSystem). Drain both command buffers via contexts 0 and 1.
                            * Used when queued work must complete before Init returns.
                            * The second dispatch distinguishes this wrapper from its worker. */
      "40 53 48 83 EC 20 33 D2 48 8B D9 E8 ?? ?? ?? ?? BA 01 00 00 00 48 8B CB "
      "48 83 C4 20 5B E9 ?? ?? ?? ??",
      0x1AA46B0u },
    { "GameMgrLea",        /* Game-manager getter starts with MOV RAX,[rip+slot]. Decode the
                            * pointer slot and dereference lazily; it may be NULL at startup. */
      "48 8B 05 ?? ?? ?? ?? 48 83 B8 A0 54 0A 00 00 0F 94 C0",
      0xB10870u },
    { "MenuThink",
      "48 8B C4 55 57 41 54 41 56 41 57 48 8D 68 98 48 81 EC 40 01 00 00 48 C7 44 24 38 FE FF FF FF",
      0x1718600u },
    /* ---- the mint-a-new-save chain (campaign rawmap-io-contract T2) ----------------------------
     * Resolved but NOT YET CALLED. They are here so load-time resolution reports whether they scan
     * clean on the installed build -- the OpenGL image has never been cross-checked against a live
     * GL process, and a signature that resolves in a file but not in memory is exactly the failure
     * this table exists to surface early.
     *
     * WHY THIS CHAIN. Opening a rawmap through LoadMap borrows an EXISTING save slot, so the editor
     * takes that map's identity while the rawmap supplies only content -- and the next save writes
     * the rawmap over that map. Minting a save first and loading THAT makes the rawmap a genuinely
     * new map. CreateLocalSavedMapInternal mints a fresh id when its id argument is NULL.
     *
     * The chain's third function, idStr::operator=(const char *), is ALREADY in this table as
     * "IdStrAssignCStr" (0x19FD5F0) -- adding it again under a second name only produced two entries
     * resolving to one address. Two findings about it that belong with this chain:
     *
     *   - Its purpose is DIRECT, not inferred from a name. The body carries its own source paths
     *     ("...idlib\text\Str.cpp(88) : TAG_STRING", "...idlib\text/Str.h" with the assert
     *     "a <= ALLOCED_MASK") and works len@+0x08 / data@+0x10 / alloced|flags@+0x18 -- the layout
     *     this backend already reads. So it is NOT the intern-and-store-pointer function that the
     *     resolve-address discipline warns about mistaking for an idStr assign.
     *   - CAUTION for the eventual caller: on a ZEROED idStr it silently does NOTHING. With
     *     alloced==0 and the heap bit clear it takes the "static buffer too small -> return" path.
     *     So the record handed to CreateLocalSavedMapInternal cannot be a zeroed buffer; its idStr
     *     fields have to be engine-constructed first. */
    { "CreateLocalSavedMapInternal",
      /* Full prologue, zero wildcards -- no RIP-relative operand and no relative branch in the
       * window. Unique at 23 bytes: confirmed independently by extract_sig.py (PORTABLE, target
       * 0x561d70) and by a Ghidra whole-image byte search returning exactly one hit. */
      "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 E1 48 81 EC B0 00 00 00",
      0x562780u },
    /* idSnapMap::AddTag("map:branch") -- add-if-absent on the map's tag list.
     *
     * WHY THIS FUNCTION MATTERS: the editor's Save command asks the open map whether it carries
     * "map:new" or "map:branch" and, if it does, routes Save into SAVE AS -- which prompts for a
     * name and mints a fresh save slot through CreateLocalSavedMapInternal. That is the engine's own
     * "this is a new map, name it" path, the one Branch and New-from-Template use. Setting the tag on
     * a map we substituted is therefore the whole of "open a rawmap as a new map": the engine does
     * the minting, writes its own game.details checksum, and writes its own .verify sidecars, none of
     * which this project can currently generate.
     *
     * The tag is transient state, not published metadata: idSnapMap save-as REMOVES it before
     * writing and restores it only if the write failed, and the skip-rename branch removes it
     * outright. It shares the list with publish tags but under the reserved "map:" prefix -- cf. the
     * cvar snapEdit_allowReservedTags, "Can put reserved tags in publish flow."
     *
     * WILDCARDS: two RIP-relative displacements (the stack-cookie load and the lea of the "map:branch"
     * literal) and one call rel32. NOT unique at 45 bytes -- the prologue through the first lea has a
     * byte-identical twin at 0x1180590 on the pinned build, which is the kind of near-miss this
     * project has shipped before. Extended through `add rdi,0x998` -- the tag-list field offset
     * itself -- which is both the shortest distinguishing run and the most meaningful one.
     * Verified ONE hit on BOTH shipped builds; the embedded 0x998 matching in both also confirms the
     * field offset does not move between them. */
    /* idSnapMap -> JSON, the engine's own "serialize this map for saving" entry point.
     *
     * bool SnapMapToJson(void *map, idStr *out, unsigned char compact)
     *
     * THIS, not SerializeToJson (0x5F2390), is what to call to get a live map's JSON.
     * SerializeToJson's first argument is NOT the map: this function builds a ~0x770 temporary
     * snapshot object on its own stack, populates it from the map, serializes THAT, and destroys it.
     * Calling SerializeToJson with an idSnapMap* would hand it the wrong object -- which is exactly
     * the mistake the save shadow's own typedef comment invites, because the shadow only ever sees
     * the argument the engine already prepared.
     *
     * Established from the sole caller (0x568B90), which also settles two long-open questions:
     *   *param_3 = 0x6f            -> game.details "declVersion": 111
     *   FUN_141A4A480(json, len)   -> game.details "declChecksum"
     * so the decl checksum is id's own hash over the serialized JSON. That is why a sweep of 13
     * stock algorithms over 6 byte ranges against 54 real saves matched nothing.
     *
     * WILDCARDS: one RIP-relative displacement (the stack-cookie load). Unique on BOTH shipped
     * builds at this length; the trailing `E8` anchors the temp-object construction that makes this
     * function what it is. */
    { "SnapMapToJson",
      "40 53 56 57 48 81 EC B0 07 00 00 48 C7 44 24 20 FE FF FF FF "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 A0 07 00 00 "
      "41 0F B6 F0 48 8B FA 48 8B D9 48 8D 4C 24 30 E8",
      0x59D2F0u },

    { "SnapMapAddBranchTag",
      "4C 8B DC 57 48 83 EC 60 49 C7 43 B8 FE FF FF FF 49 89 5B 10 49 89 73 18 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 48 8B F9 48 8D 15 ?? ?? ?? ?? "
      "49 8D 4B C0 E8 ?? ?? ?? ?? 90 48 81 C7 98 09 00 00 8B 77 08 83 EE 01",
      0x5992C0u },

    { "WriteLocalSavedMapText",
      /* Same: no wildcards, unique at 20 bytes by both tools (target 0x576530). */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 BE FF FF",
      0x576EA0u },
    { "EditorFrame",       /* idSnapEditorLocal's per-frame Think (0x523140). The FIRST execution point
                            * this product has on DOOM's own main thread at a frame boundary -- every
                            * other main-thread route it has is the console command buffer's drain
                            * callback, which is documented freeze-prone for heavy lifecycle calls (see
                            * editor_frame.c). Identified by its own state machine: the body reads the
                            * editor state id at +0x23618 (`param_1[0x46c3]`), gates on the
                            * mid-transition field +0x224dc, dispatches through the state resolver and
                            * calls ExitEditor 0x522680 on the confirm path. 11 insns / 33 bytes with
                            * NO wildcards, PORTABLE (derived 2026-09-07 by tools/signatures/
                            * extract_sig.py: unique once on Vulkan 0x523140 and once on OpenGL
                            * 0x522a80). Every byte is register/rsp/rbp-relative -- no RIP-relative and
                            * no relative branch -- so the whole 33-byte window is a legal steal. */
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 90 00 00 00 48 C7 45 C7 FE FF FF FF",
      0x523140u },
    { "EditorLoadMap",     /* idSnapEditorLocal::LoadMap (0x525250) -- reloads/swaps the editor's map IN
                            * PLACE, without leaving the editor. The stock SnapMap UI exposes no such
                            * control. Its second argument is an idStr of which LoadMap reads ONLY the
                            * data pointer at +0x10 (a 20-hex uppercase save-directory id); confirmed by
                            * decompile, the body's single use is
                            * `FUN_1405253e0(this, *(void**)(arg+0x10))`. 8 insns / 36 bytes, PORTABLE
                            * (Vulkan 0x525250, OpenGL 0x524b80). MUST be called from a frame boundary:
                            * called cold from the menu it is a harmless no-op (the editor subsystem is
                            * uninitialised), and called from the command-buffer drain it deadlocks the
                            * main thread. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 89 6C 24 48 48 89 74 24 50 48 8B F2 48 8B D9",
      0x525250u },
    { "MenuPump",
      "40 56 41 57 48 81 EC 18 01 00 00",
      0x1702BA0u },
    { "AddToSelection",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B A9 88 00 00 00",
      0x59F210u },
    { "ClearSelection",
      "45 33 D2 44 39 91 88 00 00 00",
      0x59FA00u },
    { "WireConnectCreator1", /* void(tool,world,index): output-node source creator. The detour routes
                              * bare targets to native references instead of CSR edges.
                              * MOVSXD RDI,R8D plus world+0x204C8 separates sibling creators. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 63 F8 48 8B F2 48 8B D9 83 FF FF "
      "0F 84 ?? ?? ?? ?? 48 8B 8A C8 04 02 00 48 8B 81 A0 06 00 00 48 8B 04 F8 F6 80 64 01 00 00 20",
      0xCDB990u },
    { "ConnectOutputCreator", /* void(tool,world,index): base-entity source creator.
                               * MOV R9,[RDX+0x204C8] and MOVSXD R10,[RCX+0x10]
                               * distinguish it from the other creator variants. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
      "4C 8B 8A C8 04 02 00 48 8B EA 4C 63 51 10",
      0xCDBB40u },
    { "Toast",
      "40 57 48 83 EC 20 48 8B F9 48 8B 89 F0 08 00 00",
      0xCFA0B0u },
    /* Engine dialog text uses the descriptor's nonempty idStr instead of default GDM text. */
    { "AddDialog",
      "40 55 57 41 54 41 56 41 57 48 8D AC 24 F0 BC FF FF",
      0xE643C0u },
    /* Raise through the shell wrapper: it sets screen+0xA8 for input handling.
     * Calling AddDialog directly can display a dialog that ignores keys. */
    { "AddDialogWrapper",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 8B DA 48 8B F9 48 8B 0D ?? ?? ?? ?? 48 8B 01 33 D2 FF 50 48 90 48 8B D3 48 8B 4F 08 E8 ?? ?? ?? ?? 48 8B 47 18",
      0x17363A0u },
    { "ShowDialog",
      "48 8B C4 57 48 81 EC 80 00 00 00 48 C7 40 B8 FE FF FF FF 48 89 58 18 48 89 70 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 78 48 8B F2",
      0xE6A260u },
    /* Hide the active widget before its descriptor callbacks are released.
     * The shell wrapper holds the native lock and updates input visibility. */
    { "ClearDialogWrapper",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 8B DA 48 8B F9 48 8B 0D ?? ?? ?? ?? 48 8B 01 33 D2 FF 50 48 90 48 8B D3 48 8B 4F 08 E8 ?? ?? ?? ?? 48 8B 07 48 8B CF FF 90 60 02 00 00",
      0x1736450u },
    /* Assign text into an already-constructed idStr; do not use its constructor here. */
    { "IdStrAssignCStr",
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B FA",
      0x19FD5F0u },
    /* Dialog button callbacks report their action ID here; the descriptor
     * does not contain a result byte to poll. */
    { "DialogAction",
      "40 55 56 57 41 56 41 57 48 8D AC 24 50 79 FF FF B8 B0 87 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 44 24 38 FE FF FF FF",
      0xE67BF0u },
    { "IdStrCtor",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B DA 48 89 01 48 8B F9",
      0x19FCEF0u },
    { "IdStrDtor",
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 51 10",
      0x19FD120u },
    { "EntityDefCtor",
      "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 E8 ?? ?? ?? ?? 90 48 8D 8B 68 01 00 00",
      0x5E9400u },
    { "EntityDefDtor",
      "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 48 81 C1 30 01 00 00",
      0x17ACE70u },
    { "DeclSourceRebuild",
      "48 85 D2 0F 84 ?? ?? ?? ?? 55 56 57 41 56 41 57 48 81 EC C0 01 00 00",
      0x17AE560u },
    { "IdStrAssign",
      "40 53 48 83 EC 20 48 8B D9 45 33 C9 48 8D 0D ?? ?? ?? ??",
      0x1A03E10u },
    { "Lexer",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F1 41 0F B6 D9",
      0x1A5CD90u },
    { "StructDeserialize",
      "4C 8B DC 57 48 83 EC 70 49 C7 43 C8 FE FF FF FF 49 89 5B 10 49 89 73 18",
      0x1A1D450u },
    { "LexCtxCtor",
      "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 33 C0 "
      "C7 41 10 00 00 05 00 48 89 01 48 89 41 08 C7 41 28 00 00 05 00 48 89 41 18 "
      "48 89 41 20 48 83 C1 30",
      0x1A5BB70u },
    { "ParseNodeCtor",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 89 51 08",
      0x1A41400u },
    { "ParseNodeDtor",
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 8B 41 08",
      0x1A41640u },
    { "BufferCommandText",
      "83 B9 A8 00 02 00 00 41 B8 40 00 00 00",
      0x1AA3780u },
    { "EntityClone",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 50 01 00 00",
      0x5A6460u },
    { "StructSerialize",
      "4C 8B DC 57 48 81 EC 80 00 00 00 49 C7 43 B8 FE FF FF FF 49 89 5B 10 49 89 73 18",
      0x1A21B40u },
    { "TreeRenderJson",
      "40 57 48 81 EC E0 00 00 00 48 C7 44 24 28 FE FF FF FF",
      0x1A43730u },
    /* Language-string injection. */
    { "StridsSortBody",    /* idLangDict radix-sort body (0x1a2b480 wrapper -> JMP here); detour target */
      "48 89 5C 24 18 55 56 57 48 8D AC 24 10 F8 FF FF",
      0x1A2B490u },
    { "StridsTableLea",    /* idLangDict::GetIndexForId -- carries LEA RCX,[the strids idList global]; the
                            * build-portable anchor we decode to find the string-table descriptor global */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 74 ?? 80 3A 00",
      0x1A2ACD0u },
    { "StridsInsert",      /* idList<StridEntry>::Append (32-byte element). Long sig: a near-twin idList::
                            * Append shares the prologue; uniqueness only at the element-copy body (+74B). */
      "48 89 5C 24 08 57 48 83 EC 20 8B 41 0C 48 8B FA 48 8B D9 39 41 08 75 ?? "
      "E8 ?? ?? ?? ?? 84 C0 75 ?? 83 C8 FF 48 8B 5C 24 30 48 83 C4 20 5F C3 "
      "48 63 43 08 3B 43 0C 7D ?? 48 8B C8 8B 07 48 C1 E1 05 48 03 0B 89 01 48 8B 47 08",
      0x1A29980u },
    { "StridsHash",        /* Lowercasing FNV-1a hash used by language-string keys. */
      "48 89 5C 24 08 57 48 83 EC 20 0F B6 01",
      0x1A29B90u },
    /* Resource-provider vtable anchor. */
    { "ResProviderCtor",   /* Decode LEA RAX,[rip+vtable] from this constructor. Override-open
                            * replaces slot +0xF8 after provider-layout validation. */
      "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 "
      "48 8D 05 ?? ?? ?? ?? 48 89 01 33 FF C7 41 18 00 00 33 00",
      0x1A51070u },
    { "IdFileReadString",   /* Native idFile ReadString at +0xE0 (pinned Vulkan 0x267390).
                              * Install directly to retain its engine idStr ABI. */
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 01 48 8B DA 41 B8 04 00 00 00 "
      "C7 44 24 30 00 00 00 00 48 8D 54 24 30 48 8B F9 FF 50 28 44 8B 44 24 30 48 8B CB "
      "48 8B F0 45 85 C0 7E 2D B2 20 E8 ?? ?? ?? ?? 4C 8B 0F 48 8B CF 4C 63 44 24 30",
      0x267390u },
    { "IdFileCompare",      /* Native idFile/idStr comparison at +0xE8 (pinned Vulkan 0x267290).
                              * Its calling convention must not be replaced by a guessed wrapper. */
      "40 53 55 56 57 48 81 EC 98 00 00 00 48 C7 44 24 20 FE FF FF FF 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 84 24 88 00 00 00 41 8B E9 49 8B F8 48 8B F2 48 8B D9 48 8D 0D ?? ?? ?? ?? "
      "E8 ?? ?? ?? ?? 48 8D 4C 24 28 E8 ?? ?? ?? ?? 90 48 8B 03 48 8D 54 24 28 48 8B CB "
      "FF 90 E0 00 00 00",
      0x267290u },
    { "IdFileWriteString",   /* Native WriteString at +0xF0; publish only after all three helpers are clean. */
      "40 57 48 83 EC 70 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 88 00 00 00 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 44 24 60 48 8B F9 49 8B D0 48 8D 4C 24 30 E8 ?? ?? ?? ?? 90 8B 44 24 38 "
      "89 44 24 20 48 8B 07 41 B8 04 00 00 00 48 8D 54 24 20 48 8B CF FF 50 30",
      0x268470u },
    /* Console and cvar registration. */
    { "Printf",            /* Engine message dispatch; sh_printf supplies message class 1 and va_list. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 BA FF FF",
      0x1A08E80u },
    { "CvarRegister",      /* Outer cvar registration initializes engine globals before forwarding
                            * to idCVarSystem::Register. Product flags include type and NOCHEAT. */
      "48 8B C4 48 89 48 08 57 48 83 EC 60 48 C7 40 E8 FE FF FF FF 48 89 58 10 48 89 68 18 "
      "48 89 70 20 41 8B F9",
      0x1A04F00u },
    { "CmdSystemLea",      /* Bot-command registrar loads cmdSystem with MOV RCX,[rip+slot] at +6.
                            * Decode MOV as well as LEA; this is a pointer slot, not the object.
                            * The longer body distinguishes its otherwise generic prologue. */
      "40 53 48 83 EC 30 48 8B 0D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 DB 4C 8D 05 ?? ?? ?? ?? "
      "89 5C 24 28 48 8D 15 ?? ?? ?? ?? 48 89 5C 24 20 48 8B 01 FF 50 20 48 8B 0D ?? ?? ?? ?? "
      "4C 8D 0D ?? ?? ?? ??",
      0x717A50u },
    /* Reflection lookups. The decl-manager accessor resolves separately from
     * a signed call site in engine_globals_table.gen.h. */
    { "FindTypeInfoByName", /* record *(reflect,name,scope). A caller consumes the return value even
                             * though the decompiler inferred void. Field-layout details live
                             * in typeinfo.c; the large-frame prologue distinguishes this lookup. */
      "40 55 56 57 48 8D AC 24 50 BE FF FF",
      0x1A1D590u },
    { "FindEnumByName",     /* enum record *(reflect,name); sh_type uses it after class lookup misses.
                             * Member-array layout is documented beside the reader in typeinfo.c. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 48 8B EA 48 8B CA "
      "E8 ?? ?? ?? ?? 23 43 24",
      0x1A1DA20u },
    { "MapGetter",          /* MapGetter(gameMgr) returns or constructs the live map object. */
      "40 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 8B D9 48 8B B9 B0 A0 29 00",
      0x31AD60u },
    { "MapWriter",          /* MapWriter(map,path) exports v5 maps. The fixed cmp [rcx+0x38],5
                             * anchors the format gate; entity.c validates the destination. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 FE FF FF 48 81 EC B0 02 00 00 "
      "48 C7 44 24 30 FE FF FF FF 48 89 9C 24 00 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
      "48 89 85 A0 01 00 00 48 8B F2 48 8B F9 83 79 38 05",
      0x182B740u },
    { "SessionDevModeGetter", /* Bool getter for session+0x34C89. Devmode disable replaces its first
                               * three bytes with xor eax,eax; ret. The fixed field offset
                               * refuses incompatible layouts; unpatch restores the signature. */
      "0F B6 81 89 4C 03 00 C3",
      0x18A31D0u },
    { "RenderLogStub",        /* Render trace sink: mov [rsp+0x20],r9; ret, followed by padding.
                               * Padding distinguishes it from another short stub and supplies
                               * 16 bytes of room. The replacement never calls a trampoline. */
      "4C 89 4C 24 20 C3 CC CC CC CC CC CC",
      0xD99DC0u },
    /* Optional math overrides replace complete functions; trampolines are unused. */
    { "AlgoMatMul",        /* void(A[16],B[16],out[16]); row-major product.
                            * sh_algo_matmul accumulates in double and stores float. */
      "48 8B C4 48 81 EC E8 00 00 00 0F 10 01 4C 8D 18",
      0x1A82F10u },
    { "AlgoInverse",       /* bool(M[16],out[16]); singular |det|<1.0000000168623835e-16
                            * returns 0 without touching output. The replacement retains this contract. */
      "48 8B C4 48 81 EC 98 01 00 00 0F 10 19 0F 10 41 10",
      0x1A828F0u },
    { "AlgoPackRGBA",      /* RGBA8 packing. The engine truncates; the override rounds half-up
                            * in double. A RIP-relative instruction starts at +10, so the
                            * 14-byte stolen window must never be executed as a trampoline. */
      "F3 0F 10 01 41 BA FF 00 00 00 F3 0F 10 0D ?? ?? ?? ??",
      0x1A19470u },
    { "AlgoCurveEval",     /* float(curve,t,mode); count@+0x20C, times@+0x0C, values@+0x10C.
                            * The replacement supports linear/hold and Catmull-Rom modes.
                            * Only linear interpolation applies the small-value zero flush. */
      "48 89 5C 24 10 56 48 83 EC 40 8B 81 0C 02 00 00",
      0x1A5EB40u },
    /* Model-builder and render-debug dependencies. Revalidate signatures and
     * separate object-layout constants when porting to another build. */
    { "Md6Ctor",           /* idMd6Builder constructor, called first. The distinctive
                            * [self+0x38]=-1 store separates otherwise similar constructors. */
      "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 "
      "E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 03 48 C7 43 38 FF FF FF FF",
      0x149B8D0u },
    { "Md6SetOutput",      /* void(md6,output idStr). Copies output after input/options setup.
                            * Function discovery may require inspecting raw prologue bytes. */
      "40 57 48 81 EC D0 01 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 F0 01 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 C0 01 00 00 48 8B DA 48 8B F9 48 8D 4C 24 60",
      0x149C450u },
    { "Md6Build",          /* Final void(md6) operation compiles and releases the builder; do not
                            * treat its destructor-shaped body as a separate post-build cleanup. */
      "40 57 48 83 EC 50 48 C7 44 24 40 FE FF FF FF 48 89 5C 24 60 48 89 6C 24 68 "
      "48 89 74 24 70 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 33 ED 8B F5",
      0x149BEE0u },
    { "DefaultIdStrCtor",  /* Construct an empty idStr: data=self+0x1C, len=0, capacity=0x80000014.
                            * Distinct from IdStrCtor, which constructs from a C string. */
      "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 E8 ?? ?? ?? ?? "
      "48 8D 43 1C C7 43 18 14 00 00 80 48 89 43 10 C7 43 08 00 00 00 00 C6 00 00",
      0x19FD040u },
    { "BModelBuilder",     /* BModelBuilder(out208,input,output,options idStr). Arg1 is a
                            * 0xD0 result buffer filled with 0x01; arg4 is a constructed idStr.
                            * Recheck the caller and builder argument reads when porting. */
      "40 53 55 56 57 48 81 EC 28 01 00 00 48 C7 44 24 30 FE FF FF FF 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 84 24 10 01 00 00 49 8B F1 49 8B E8 48 8B FA 48 8B D9",
      0x14CF550u },
    { "RenderWorldGetter", /* Render-world registration window has LEA RCX,[rip+slot] at +6.
                            * Decode then dereference; it is not an object accessor.
                            * Render vtable and editor-field offsets remain separate contracts
                            * documented at their consumers in commands.c. */
      "89 B3 A8 02 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
      "48 85 C9 74 12 66 C7",
      0x5E4C28u },
    { "SwfTextOnKeyCall",   /* void*(self,retbuf,thisObject,parms): SWF text-field onKey::Call.
                             * TextField+0xC0 gives its instance; parms holds scancode/isDown.
                             * Re-find via RTTI onKey/onChar classes and differing vtable slot 10,
                             * then recheck the fields used in swf_textedit.c.
                             * Newer retail RVA 0x17505E0 is not this table's pinned build. */
      "40 55 56 57 41 56 41 57 48 81 EC D0 00 00 00 48 C7 44 24 28 FE FF FF FF "
      "48 89 9C 24 00 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 C0 00 00 00 "
      "49 8B F9 49 8B E8 4C 8B F2 45 33 FF",
      0x2B3500u },
    { "IdStrAssignFromStr", /* Assign one idStr to another, growing destination as needed.
                             * src length/data are +8/+0x10; zero flags decline buffer stealing.
                             * Do not confuse this with IdStrAssign, the interned-name helper. */
      "48 89 74 24 18 57 48 83 EC 40 48 8B F2 48 8B F9 8B 49 18 8B D1 C1 EA 1F 80 E2 01 74 ?? "
      "44 8B 46 18 41 8B C0",
      0x19FD180u },
    { "PrefabCtor",         /* idSnapEntityPrefab *(self). Initializes fields without freeing old
                             * contents, then constructs the tail at +0x120. Reinitialization
                             * can abandon dangling staging pointers without double-freeing.
                             * Newer retail RVA 0x11AC8D0 is a different extraction build. */
      "4C 8B DC 49 89 4B 08 53 48 83 EC 30 49 C7 43 E8 FE FF FF FF 48 8B D9",
      0x54D0A0u },
    { "PrefabPopulate",     /* char(prefab,editor,status*). Build from current selection.
                             * Re-find through the "Failed to create prefab" error strings.
                             * Newer retail RVA 0x11ADB30 is not the pinned build. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 F7 FF FF "
      "48 81 EC C0 09 00 00 48 C7 85 10 01 00 00 FE FF FF FF 48 89 9C 24 18 0A 00 00",
      0x54E410u },
    { "MemLocalGet",        /* idMemLocal *(void). Resolve the accessor rather than computing the
                             * singleton address. Many magic-static getters resemble this;
                             * frame size, MOV R8D,4, and GS TLS load distinguish the pattern. */
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 8B 0D ?? ?? ?? ?? "
      "65 48 8B 04 25 58 00 00 00 41 B8 04 00 00 00 48 8B 14 C8 48 8D 1D ?? ?? ?? ??",
      0x1A04BF0u },
    { "MemLocalPushHeap",  /* void(self,heapId). Push the 32-entry scope stack at +0x44, depth +0xC4.
                             * Main-thread-only: heap -1 allocation consults this same gate.
                             * PushHeap(0) selects the process heap so clipboard data survives
                             * map teardown. The overflow assertion incorrectly names PopHeap. */
      "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 16 "
      "48 63 83 C4 00 00 00 83 F8 20 7D 15 89 7C 83 44",
      0x1AC57A0u },
    { "MemLocalPopHeap",   /* void(self). Pop the main-thread heap scope; underflow is fatal.
                             * Balance successful pushes with __finally across every exit.
                             * SUB [self+0xC4],1 followed by JS identifies the underflow path. */
      "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 09 83 AB C4 00 00 00 01 78",
      0x1AC5770u },
    { "PasteInstantiate",   /* void(prefab,editor). Instantiate staged entities and remap references.
                             * Selection must be empty: old indices map through the appended
                             * selection array. Existing entries would silently miswire the paste.
                             * The native idle action 0x5C calls this and then EnterAddPrefabGrab;
                             * calling it alone leaves incomplete placement state.
                             * Re-find via EnterAddPrefabGrab's sole caller, then the preceding
                             * call with LEA RCX,[editor+0x209A8] and MOV RDX,editor. */
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 A8 F7 FF FF "
      "48 81 EC 20 09 00 00 48 C7 45 E0 FE FF FF FF 48 89 58 18",
      0x54F950u },
    { "EnterAddPrefabGrab",  /* void(mode). Enter add-prefab manipulation after instantiation.
                             * mode+0x2D0 clears Duplicate (0x04), sets Add Prefab (0x08);
                             * +0x2D1 |= 1 restores selection, +0x1AC=4 enters manipulation,
                             * +0xBB8 requests redraw. Siblings differ by flavor masks,
                             * so preserve the full body when rederiving the paste path. */
      "80 A1 D0 02 00 00 FB 80 89 D0 02 00 00 08 80 89 D1 02 00 00 01 "
      "C7 81 AC 01 00 00 04 00 00 00 C6 81 B8 0B 00 00 01 C3",
      0xCF35E0u },
    { "SoundWorldLea",      /* Sound-system update reads the sound-world slot with MOV RBX,[rip+disp32].
                             * The shared decoder accepts RAX/RCX only; soundpreview uses its
                             * any-register decoder. Profile color 0xFF00FF00 anchors the body. */
      "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 1E 48 8D 15 ?? ?? ?? ?? B9 00 FF 00 FF",
      0x18514F0u },
    { "SoundPreview",       /* void*(world,outHandle,name), sound-world vtable +0x30.
                             * Editor audition sets solo/listener state and allocates an emitter;
                             * stop the previous handle before starting another. An empty/missing
                             * resolved name clears preview state, but find-or-create can fatal
                             * on invalid decls: validate names through the asset index first. */
      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 41 54 41 56 41 57 48 83 EC 50 "
      "4C 8B E1 4D 8B F0 49 8B C8 4C 8B FA E8 ?? ?? ?? ?? 33 F6 8B E8 39 35 ?? ?? ?? ?? 7E 5E",
      0x1855660u },
    { "Mega2PageDecode",    /* void(header16,payload,NULL,out): CPU-only megatexture page decode.
                             * Output is five 128x128 RGBA planes at 0x10000-byte intervals;
                             * plane 0 is albedo. Preclear all 0x50000 output bytes because
                             * skipped streams remain untouched. Supply at least 0x40000
                             * zero-filled bytes after payload; the decoder reads past page data.
                             * Re-find through the renderer Transcode.cpp code path. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 30 01 00 00 48 8D 6C 24 40 "
      "48 C7 45 10 FE FF FF FF 48 89 9D 40 01 00 00 48 8B 05 ?? ?? ?? ??",
      0x196E140u },
    { "PrefabDtor",         /* void(prefab). Destroy temporary storage after selection serialization. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 8B F9 "
      "48 81 C1 20 01 00 00 E8 ?? ?? ?? ?? 90 48 8D 8F F0 00 00 00",
      0x51D870u },
    { "EntityDeshare",      /* void*(entitySlot). Make the shared 0x6F8 block unique before editing.
                             * Refcount==1 and the allocation size distinguish this helper. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B F9 "
      "48 8B 01 83 38 01 74 3C B9 F8 06 00 00 E8 ?? ?? ?? ?? 48 8B D8",
      0x52C920u },
    /* Portable function identities checked on both renderers. Generic/short leaves
     * such as decl-manager access, list growth, and visibility resolve through
     * signed call sites in engine_globals_table.gen.h instead. */
    { "RemoveFromSelection", /* void(editor [rcx], entity handle [edx]) -- drops one entity from the
                              * editor selection. The `add rcx,0x5e0` that walks to the selection
                              * sub-object sits in the fixed bytes, which is what makes it unique.
                              * Vulkan 0x59FDA0, OpenGL 0x59F510. */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 09 48 81 C1 E0 05 00 00",
      0x59FDA0u },
    { "MaterialHeight",     /* uint(material [rcx]) -- the declared image height. Byte-identical to
                             * MaterialWidth except the final stack slot (0x60 vs 0x68), which is
                             * exactly what separates them; both are file-wide unique.
                             * Vulkan 0xD75B40, OpenGL 0xD75DB0. */
      "40 53 48 83 EC 50 F6 41 68 20 BB 01 00 00 00 89 5C 24 60 74 24 4C 8D 44 24 60",
      0xD75B40u },
    { "MaterialWidth",      /* uint(material [rcx]) -- see MaterialHeight.
                             * Vulkan 0xD75D40, OpenGL 0xD75FB0. */
      "40 53 48 83 EC 50 F6 41 68 20 BB 01 00 00 00 89 5C 24 60 74 24 4C 8D 44 24 68",
      0xD75D40u },
    { "DeclPureFind",       /* the pure decl lookup the typeinfo probes call -- decl *(ctx [rcx],
                             * name [rdx]). Distinct from DeclFind (0x17B36F0), which is the registry
                             * form. Vulkan 0x18017A0, OpenGL 0x17F40E0. */
      "40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 89 74 24 58 "
      "48 8B DA 48 8B F1",
      0x18017A0u },
    { "EventLink",          /* Event-link routine guarded by the fault shield.
                             * Vulkan 0x9C2370, OpenGL 0x9C1B70. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 89 6C 24 50 "
      "48 89 74 24 58 48 8B F2 48 8B E9",
      0x9C2370u },
    { "InteractableSpawn",  /* Interactable Spawn, reached through a vtable. Its frame setup
                             * distinguishes the function. Vulkan 0x1232830, OpenGL 0x12247F0. */
      "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 98 FE FF FF 48 81 EC 40 02 00 00 "
      "48 C7 45 80 FE FF FF FF 48 89 58 10 48 89 70 18 48 89 78 20 0F 29 70 C8 "
      "0F 29 78 B8 44 0F 29 40 A8 48 8B 05 ?? ?? ?? ??",
      0x1232830u },
    { "DeclResourceProbe",  /* Decl existence probe at vtable +0x78; validate the method by signature.
                             * Vulkan 0x1806100, OpenGL 0x17F8A40. */
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FF FF FF "
      "48 81 EC 88 01 00 00 48 C7 44 24 30 FE FF FF FF",
      0x1806100u },
    { "WeaponHudModeCall", /* The ammo widget's mode call and declaration guard.
                             * Verified independently on Vulkan and OpenGL. No
                             * pinned-build fallback: resolve uniquely by content. */
      "E8 ?? ?? ?? ?? 84 C0 74 45 48 8B 83 F8 01 00 00 48 85 C0 74 09 "
      "80 B8 41 06 00 00 00 75 30", 0 },
    { "WeaponHudGameMode", /* bool(game*); separately verifies the decoded call target. */
      "48 8B 81 38 53 04 00 83 78 18 01 0F 94 C0 C3", 0 },
    /* Native module properties and XYZ inspector. Independently matched on
     * Vulkan/OpenGL at D0BCA0/D0B3B0, D0A0B0/D09980, D071F0/D06B70 and
     * D306F0/D2FDB0. No pinned-image RVA fallback is authorized. */
    { "GridModuleProperties",
      "40 55 56 57 41 56 41 57 48 8D AC 24 70 C0 FF FF B8 90 40 00 00 E8 ?? ?? ?? ?? "
      "48 2B E0 48 C7 44 24 60 FE FF FF FF 48 89 9C 24 C8 40 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 80 3F 00 00 49 8B F1 4D 8B F0 8B FA 48 8B D9", 0 },
    { "GridPropertyChanged",
      "48 85 D2 0F 84 ?? ?? ?? ?? 48 8B C4 55 56 57 48 8D 68 A1 "
      "48 81 EC A0 00 00 00 48 C7 45 F7 FE FF FF FF", 0 },
    { "GridAddVec3",
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 B8 48 81 EC 10 01 00 00 "
      "48 C7 45 90 FE FF FF FF 48 89 58 20 0F 29 70 B8 0F 29 78 A8", 0 },
    { "GridSetVec3",
      "40 53 B8 80 40 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 44 24 30 FE FF FF FF "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 70 40 00 00 48 8B D9 8B 02 89 81 00 01 00 00", 0 },
    /* Native palette lookup and one-module loader, verified in both images:
     * Vulkan 5B7380/5B7910, OpenGL 5B6A70/5B7000. */
    { "GridFindModule",
      "4C 8B DC 49 89 5B 18 49 89 53 10 57 48 83 EC 40 48 63 41 18 4D 8D 4B 08 "
      "4C 8B 51 10 4D 8D 43 10 48 69 F8 98 00 00 00", 0 },
    { "GridHasModule", /* VK 5b6f40, GL 5b6630; separate reload membership probe. */
      "48 89 54 24 10 48 83 EC 48 48 8D 05 ?? ?? ?? ?? 48 89 44 24 30 4C 8D 4C 24 50 48 63 41 18", 0 },
    { "GridPortalSnap", /* VK 5a9b70, GL 5a9180. */
      "40 55 53 57 41 54 41 55 41 57 48 8D 6C 24 D1 48 81 EC B8 00 00 00 45 33 FF 44 0F 29 4C 24 70 45 0F B6 E1", 0 },
    { "GridPortalAlign", /* VK 5a0b80, GL 5a02f0. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 8B F9 4D 63 E0 48 8B 09 4C 8B EA", 0 },
    { "GridQuantizeOrigin", /* VK 514df0, GL 514610. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 0F 29 74 24 30 48 8B EA 0F 29 7C 24 20", 0 },
    { "GridConfirmOrigins", /* VK 5a1750, GL 5a0ec0; add/move/duplicate release. */
      "40 55 56 41 57 48 83 EC 40 45 33 FF 0F 29 74 24 30 0F 28 F1 48 8B F1 41 8B EF 44 39 79 70 0F 8E ?? ?? ?? ?? 48 89 5C 24 60", 0 },
    { "GridPortalBounds", /* VK 5547d0, GL 553c80. */
      "40 53 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 48 8B DA 48 8B 11 4C 8B 0A", 0 },
    { "GridPortalDirection", /* VK 5548c0, GL 553d70. */
      "48 8B 01 4C 8B C9 4C 8B 00 48 63 C2 48 6B D0 1C 49 8B 80 E8 00 00 00", 0 },
    { "GridLoadModule",
      "40 55 56 57 41 56 41 57 48 81 EC 60 01 00 00 48 C7 44 24 40 FE FF FF FF "
      "48 89 9C 24 98 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 50 01 00 00 "
      "49 8B E9 49 8B F0 48 8B FA 4C 8B F9 48 8B 9C 24 B0 01 00 00", 0 },
    /* Replace one compiled instance and recompute native portal connections.
     * Vulkan 5993A0/59DA90; OpenGL 598B10/59D200. */
    { "GridReplaceInstance",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 81 68 07 00 00 "
      "48 8B F9 48 63 DA 48 81 C1 E0 05 00 00", 0 },
    { "GridReconnectPortals",
      "4C 8B DC 41 56 48 81 EC 80 00 00 00 49 C7 43 B0 FE FF FF FF 49 89 5B 10 "
      "49 89 73 18 49 89 7B 20 0F 29 74 24 70 0F 29 7C 24 60", 0 },
    /* Native collision resources cached by a module wrapper: VK 5929a0, GL 592030. */
    { "GridBuildModuleCollision",
      "40 55 56 57 41 56 41 57 48 83 EC 70 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 B8 00 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 49 63 E8 4C 8B FA", 0 },
    /* Native COW edit, local transform setter and bounds transform. Verified
     * VK 5a0280/545040/554cf0; GL 59f9f0/544960/5541a0. */
    { "GridEditEntity",
      "48 8B 01 44 8B D2 44 8B CA 41 83 E2 3F 49 C1 E9 06 4C 8B 80 D0 06 00 00 48 63 D2 4B 8B 04 C8 4C 0F AB D0", 0 },
    { "GridSetEntityTransform",
      "8B 02 89 81 88 02 00 00 8B 42 04 89 81 8C 02 00 00 8B 42 08 89 81 90 02 00 00 8B 42 0C 89 81 94 02 00 00", 0 },
    { "GridWorldBounds",
      "48 8B C4 48 89 58 18 57 48 81 EC A0 00 00 00 0F 29 70 E8 0F 29 78 D8 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 44 24 70 F2 0F 10 41 0C 49 8B F8 8B 41 14 48 8B DA 44 8B 41 18", 0 },
    /* Native Blueprint Grid Room properties entry; both renderers verified. */
    { "GridBlueprintUpdate", /* VK 0xcebd80, GL 0xceb760. */
      "40 53 55 57 48 83 EC 20 F6 41 10 10 49 8B D8 48 8B FA 48 8B E9 75", 0 },
    { "GridBlueprintHelp", /* VK 0xceb950, GL 0xceb330. */
      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D BA 90 09 02 00 48 8B EA 48 8B F1 48 8D 9A A8 11 02 00", 0 },
    { "GridSetEditorState", /* VK 0x5298a0, GL 0x5291d0. */
      "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 8B 91 18 36 02 00 E8 ?? ?? ?? ?? 48 8B D7 48 8B C8 4C 8B 00 41 FF 50 10", 0 },
    { "GridInputPressed", /* VK 0x52d190, GL 0x52cac0. */
      "48 89 5C 24 08 57 48 83 EC 20 48 63 C2 48 8B D9 48 6B F8 38 48 8D 05 ?? ?? ?? ?? 48 03 F8 44 0F B6 4F 02 44 0F B6 47 01 0F B6 17 E8 ?? ?? ?? ?? 84 C0 74 36 48 63 47 04 48 3B 83 80 00 00 00", 0 },
    { "GridAddActionHelp", /* VK 0x551e40, GL 0x551300. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 80 39 00 48 8D 59 30 41 8B F0 8B FA 74 7B 44 8B 43", 0 },
    { "GridPropertiesBlocked", /* VK 0x5af830, GL 0x5aeed0. */
      "4C 8B 01 44 8B CA 4D 85 C0 74 3C 48 63 49 08 41 8B 40 70 3B C8 75 1D 85 C0 7E 17 8D 41 FF 48 63 C8 49 8B 40 68 48 6B D1 78 44 85 4C 02 08 0F 94 C0 C3 3B C8 7D 11 49 8B 40 68 48 6B C9 78", 0 },
    { "GridEntityTree", /* VK 0x5404a0, GL 0x53fdc0. */
      "48 89 5C 24 10 57 48 83 EC 30 4C 8B 81 50 01 00 00 33 FF 89 7C 24 20 48 "
      "8B DA 4D 85 C0 74 ?? 48 8D 54 24 40 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
      "48 8B 08 48 89 38 48 8B 7C 24 40 48 89 0B 48 85 FF 74 ?? 48 8B CF E8 ?? "
      "?? ?? ?? BA 48 00 00 00 48 8B CF E8 ?? ?? ?? ??", 0 },
    { "GridApplyEntityTree", /* VK 0x545120, GL 0x544a40. */
      "48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B F9 48 8B F2 48 8B 0D "
      "?? ?? ?? ?? 48 8B 01 FF 90 40 02 00 00 F6 87 60 01 00 00 01 48 8B E8 74 "
      "?? 48 8B 15 ?? ?? ?? ?? 4C 8D 87 94 02 00 00 4C 8B C8", 0 },
    { "GridReadEntityProperties", /* VK 0x5415d0, GL 0x540ef0. */
      "48 8B C4 55 57 41 56 48 8D A8 48 FF FF FF 48 81 EC A0 01 00 00 48 C7 45 "
      "C8 FE FF FF FF 48 89 58 18 48 89 70 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 "
      "89 85 90 00 00 00 48 8B FA 48 8B D9 48 8B 01 F6 80 CD 03 00 00 01", 0 },
    { "GridTreeSetVec3", /* VK 0x1a66ee0, GL 0x1a593b0. */
      "40 53 55 56 57 41 54 41 56 41 57 B8 30 80 00 00 E8 ?? ?? ?? ?? 48 2B E0 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 20 80 00 00 40 32 ED 4D 8B E1 "
      "33 DB 4D 8B F8 4C 8B F2 48 8B F1 BF 78 00 00 00 44 8B CF 48 8D 15 ?? ?? "
      "?? ?? 4D 8B C6 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B 16 4C 8B C0 48 8B CE "
      "E8 ?? ?? ?? ?? 48 85 C0 75 ?? 44 8B CF 48 8D 15 ?? ?? ?? ?? 4D 8B C6 48 "
      "8D 8C 24 20 40 00 00 E8 ?? ?? ?? ?? 4D 8B C4 48 8B D0 48 8B CE E8 ?? ?? "
      "?? ?? 48 85 C0 74 ?? F3 41 0F 10 0C 9F 4C 8D 80 90 00 00 00 48 8B CE E8 "
      "?? ?? ?? ?? 40 B5 01 FF C7 48 FF C3 48 83 FB 03 7C ?? 40 0F B6 C5", 0 },
    { "GridTreeSetFloat", /* VK 0x1a66610, GL 0x1a58ae0. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B FA 0F 29 74 24 20 4C "
      "8B C2 49 8B F1 48 8B 11 0F 28 F2 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 75 ?? "
      "4C 8B C6 48 8B D7 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 4C 8D 80 90 00 "
      "00 00 0F 28 CE 48 8B CB E8 ?? ?? ?? ?? B0 01 48 8B 5C 24 40 48 8B 74 24 "
      "48", 0 },
    { NULL, NULL, 0 }   /* terminator */
};
