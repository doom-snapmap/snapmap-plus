/* Resolve masked signatures in mapped executable sections, independent of names.
 * Use the longest fixed run as an anchor, then verify the full pattern.
 * Multiple matches are rejected; unreadable section tails stop that section scan. */
#include "signatures.h"
#include "host_image.h"
#include <string.h>

const sig_entry NAV_RENDER_TARGET_GL_SIGNATURE = { "NavRenderTargetGL",
    "48 89 4C 24 08 53 55 57 41 55 48 83 EC 38 48 8B D9 B9 40 8D 00 00 "
    "45 8B E9 41 8B E8 48 8B FA 48 85 D2 75 18 FF 15 ?? ?? ?? ?? "
    "B9 01 00 00 00 89 4B 28",
    0x0u, 0x188FD60u
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
 * a scan miss, the recorded RVA may identify that hooked entry on the supported
 * executable (identified by hash) when the jump form and enough fixed tail bytes
 * match. It is not a portable substitute for signature resolution. */

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

/* Try the recorded-RVA hook-tolerant fallback. Returns 1 (and fills out as SIG_OK_HOOKED) if the
 * live bytes at the renderer's RVA are a detour whose post-prologue tail matches the sig; else 0. */
static int try_hooked_known_rva(const uint8_t *module_base, const sig_entry *sig,
                                const uint8_t *pat, const uint8_t *mask, size_t n, sig_result *out)
{
    uint32_t known_rva = sh_host_image_rva(module_base, sig->rva_vk, sig->rva_gl);
    if (known_rva == 0) return 0;      /* unsupported image, or no RVA recorded for this renderer */
    const uint8_t *site = module_base + known_rva;

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
            out->rva    = known_rva;
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

sig_status sig_match_recorded(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{
    uint8_t pat[SIG_MAX_PATTERN], mask[SIG_MAX_PATTERN], live[SIG_MAX_PATTERN];
    exec_section secs[MAX_SECTIONS];
    size_t n, i;
    uint32_t rva;
    int nsec, s;
    out->name = sig->name; out->status = SIG_NOT_FOUND; out->addr = 0; out->rva = 0;
    n = compile_pattern(sig->pattern, pat, mask, SIG_MAX_PATTERN);
    if (n == 0) { out->status = SIG_BAD_PATTERN; return out->status; }
    nsec = collect_exec_sections(module_base, secs, MAX_SECTIONS);
    if (nsec < 0) { out->status = SIG_BAD_MODULE; return out->status; }
    rva = sh_host_image_rva(module_base, sig->rva_vk, sig->rva_gl);
    if (rva == 0) return out->status;
    for (s = 0; s < nsec; s++)
        if (rva >= secs[s].vaddr && rva - secs[s].vaddr <= secs[s].vsize &&
            n <= secs[s].vsize - (rva - secs[s].vaddr)) break;
    if (s == nsec || !sig_safe_read(module_base + rva, live, n)) return out->status;
    for (i = 0; i < n; i++) if (mask[i] && live[i] != pat[i]) return out->status;
    out->status = SIG_OK; out->addr = (uintptr_t)(module_base + rva); out->rva = rva;
    return SIG_OK;
}

uintptr_t sig_addr_by_name(const sig_result *results, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if ((results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED) &&
            results[i].name && strcmp(results[i].name, name) == 0)
            return results[i].addr;
    return 0;
}

/* Engine signatures for DOOM 2016 build 13954591, with each function's RVA in
 * the Vulkan (SHA256 3D8B4393649F80BDAE85C19AE2C985C45EC9AC54F74AB04A18816E30EAD63C21)
 * and OpenGL (SHA256 1BDDECAA4E86850412F96E3117978BCE1EC7BB701AE1B5A33120C21E4421E51E)
 * executables. The two renderers are separate links; no uniform delta is valid.
 * A zero RVA disables fallback. Validate entry changes with
 * tests/run-tests.ps1 -Doom <13954591 Vulkan> -DoomAlt <13954591 OpenGL>.
 * CI has no game image and cannot verify these identities. */
const sig_entry BACKEND_ENGINE_SIGNATURES[] = {
    /* Resized-room containment: independently verified on Vulkan and OpenGL. */
    { "GridRefreshPlacementSurfaces",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 89 6C 24 50 "
      "48 89 74 24 58 49 8B E8 48 8B F2 48 8B D9 E8 ?? ?? ?? ?? 8B BE 58 07 00 00 3B 7B 0C", 0x1198880u, 0x1197AB0u },
    { "GridContainingModule",
      "48 8B C4 48 89 58 08 48 89 68 10 57 48 81 EC B0 00 00 00 F3 0F 10 05 ?? ?? ?? ?? 4C 8D 4C 24 40 F3 0F 10 0D ?? ?? ?? ?? 48 8B DA", 0x11EEFC0u, 0x11EE2F0u },
    { "GridModuleRay",
      "40 55 53 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 30 80 79 48 00 48 8B DA 48 8B 95 B8 00 00 00 49 8B F9 75 40 48 C7 45 88 00 00 00 00", 0x11EF740u, 0x11EEA70u },
    /* Native map rendering controls and the environment far-clip read.
     * Each binding is unique in both independently linked renderer images. */
    { "RenderSettingsEnter",
      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 9A 88 10 02 00 48 8B E9 48 8B F2 48 8B BB E0 09 00 00 48 8B CF 48 8B 07 FF 90 E8 00 00 00", 0x118F880u, 0x118EA00u },
    { "RenderSettingsExit",
      "40 53 48 83 EC 20 48 8B 82 88 10 02 00 48 8B DA 48 8B 88 E0 09 00 00 48 8B 01 FF 90 F0 00 00 00 48 8B 8B 88 10 02 00 C6 83 24 36 02 00 00 E8 ?? ?? ?? ??", 0x118F960u, 0x118EAE0u },
    { "RenderSettingsPopulate",
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 BF FF FF B8 10 41 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 45 A0 FE FF FF FF 48 89 9C 24 60 41 00 00", 0x1282DB0u, 0x1282400u },
    { "RenderSettingsApply",
      "40 53 48 83 EC 20 0F B6 01 48 8B D9 48 8B 51 48 48 81 C2 28 36 02 00 88 02 0F B6 41 01 88 42 01 0F B6 41 02 88 42 02 0F B6 41 03 88 42 03 0F B6 41 04", 0x118EB40u, 0x118DCD0u },
    { "RenderSettingsReset",
      "40 53 48 83 EC 20 48 8B 51 48 48 8B D9 48 81 C2 28 36 02 00 0F B6 02 88 01 0F B6 42 01 88 41 01 0F B6 42 02 88 41 02 0F B6 42 03 88 41 03 0F B6 42 04", 0x118EAA0u, 0x118DC30u },
    { "RenderSettingsDirtyCall",
      "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? C7 44 24 30 16 00 00 00 45 33 C0 48 8D 54 24 30 48 8B CB E8 ?? ?? ?? ?? C6 47 08 01 48 8D 15 ?? ?? ?? ??", 0x118F716u, 0x118E896u },
    { "RenderAddFloat",
      "48 8B C4 57 41 56 41 57 48 81 EC 80 00 00 00 48 C7 40 A8 FE FF FF FF 48 89 58 10 48 89 68 18 48 89 70 20 4D 8B F1 4D 8B F8 48 8B EA 48 8B F1 B9 38 01 00 00 E8 ?? ?? ?? ?? 48 8B F8 48 89 84 24 A0 00 00 00 48 85 C0 74 ?? 8B 9E E0 01 00 00 8D 53 01 89 96 E0 01 00 00 48 8B CE E8 ?? ?? ?? ?? 4C 8B C0 44 8B CB", 0x127DBE0u, 0x127D190u },
    { "RenderAddTitle",
      "48 8B C4 57 41 56 41 57 48 81 EC 90 00 00 00 48 C7 40 98 FE FF FF FF 48 89 58 10 48 89 68 18 48 89 70 20 48 8B DA 48 8B E9 B9 20 00 00 00 E8 ?? ?? ?? ??", 0x12807B0u, 0x127FD60u },
    { "RenderClipRead",
      "E8 ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? 0F 57 DB 44 0F 28 84 24 D0 00 00 00 F3 0F 10 D8 F3 0F 10 05 ?? ?? ?? ?? 0F 28 CA F3 0F 5F CB 0F 2E C1 7A ??", 0x17B8EB5u, 0x17B9F75u },
    { "RenderParmFromOp",
      "0F B7 41 02 B9 3F 09 00 00 66 3B C1 7D ?? 48 8B 0D ?? ?? ?? ?? 0F BF D0 E9 ?? ?? ?? ?? 33 C0 C3 48 83 EC 58 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40", 0x15B8330u, 0x15B91E0u },
    { "RenderVariablesResize",
      "48 89 4C 24 08 55 56 57 41 56 41 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 9C 24 88 00 00 00 8B EA 4C 8B F1 85 D2 7F ?? 0F B6 41 13 33 DB 84 C0 74 ?? 2C 03 3C 01 77 ?? 48 8B 09 48 85 C9 74 ?? 41 8B 56 0C E8 ?? ?? ?? ?? 49 89 1E 41 89 5E 0C 41 89 5E 08 E9 ?? ?? ?? ?? 8B 41 0C 3B E8 0F 84 ?? ?? ?? ?? 0F B6 49 13 80 F9 02 0F 85 ?? ?? ?? ?? 3B E8 0F 8E ?? ?? ?? ?? 41 0F B6 56 12 45 33 C0 8B CD E8 ?? ?? ?? ?? 4C 8B F8 48 89 84 24 80 00 00 00 48 85 C0 75 ?? 32 C0 E9 ?? ?? ?? ?? 41 C6 46 13 00 33 DB 89 5C 24 78 41 39 5E 08 7E ?? 48 63 C3 48 6B F0 68 49 8B 16 48 03 D6 4A 8D 0C 3E E8 ?? ?? ?? ?? 49 8B 3E 48 03 FE 48 8D 4F 38 E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 49 8B 06 48 8D 0C 06 48 89 8C 24 80 00 00 00 48 85 C9", 0x1188980u, 0x1187AE0u },
    { "DeserializeFromJson",
      "40 55 56 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 C7 44 24 68 FE FF FF FF",
      0x12466A0u, 0x1245CD0u },
    /* Native saved-map builder checks unsigned (idStr length - 1) before
     * hashing/compression. Vulkan 568cee; independently OpenGL 5682de. */
    { "SnapMapSaveSizeLimit",
      "3D FF FF 9F 00 77 12 48 8B D1 49 8B 4F 10 E8 ?? ?? ?? ?? 89 06 B0 01",
      0x11C538Eu, 0x11C475Eu },
    /* Saved/published map reader: signed positive length, then 10 MiB ceiling.
     * Vulkan 581fa3; independently OpenGL 581633. Upper branch is at +21. */
    { "SnapMapReadSizeLimit",
      "83 F8 01 7D 0A BB 94 01 00 00 E9 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? "
      "7E 07 BB 93 01 00 00 EB ?? 33 C0 48 89 44 24 38 89 44 24 40 88 44 24 44",
      0x11DDC83u, 0x11DCFB3u },
    { "SerializeToJson",
      "40 53 56 57 48 81 EC E0 00 00 00 48 C7 44 24 70 FE FF FF FF",
      0x124EC70u, 0x124E2A0u },
    { "EditorMapToJson", /* Constructs an idSnapMap from idSnapMapEdit, serializes,
                          * and destroys the temporary. No save-slot writes. */
      "40 53 56 57 48 81 EC B0 07 00 00 48 C7 44 24 20 FE FF FF FF "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 A0 07 00 00 "
      "41 0F B6 F0 48 8B FA 48 8B D9 48 8D 4C 24 30 E8 ?? ?? ?? ?? "
      "90 48 8D 54 24 30 48 8B CB E8 ?? ?? ?? ?? 44 0F B6 C6 48 8B D7",
      0x11F8EB0u, 0x11F80D0u },
    { "NavRenderBegin",
      "40 56 57 41 56 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 58 48 89 6C 24 60 48 8B EA 48 8B D9 80 B9 EC 00 00 00 00",
      0x1A660F0u, 0x1A7F2F0u },
    { "NavRenderEnd",
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 C6 81 EC 00 00 00 00",
      0x1A66310u, 0x1A7F510u },
    { "NavRenderMatrix",
      "48 89 5C 24 18 55 56 57 48 8D 6C 24 B9 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 37 48 63 81 AC FB 00 00 48 8B F2 48 63 91 B4 FB 00 00 48 8B D9 44 8B 84 91 70 94 00 00 44 0B 84 81 70 94 00 00 48 63 81 B8 FB 00 00 44 0B 84 81 70 94 00 00 48 63 81 B0 FB 00 00 44 0B 84 81 70 94 00 00 44 85 81 7C B9 00 00 74 ?? B2 01 E8 ?? ?? ?? ?? 48 8D 45 27 48 8B CE 48 8D 7B 60 48 89 7C 24 28 4C 8D 4D 17 4C 8D 45 07 48 89 44 24 20 48 8D 55 F7 E8 ?? ?? ?? ?? 0F 28 45 F7",
      0x17E4650u, 0x17E54F0u },
    { "NavRenderLines",
      "40 55 48 83 EC 30 48 8B 69 18",
      0x183A540u, 0x183B4B0u },
    { "NavRenderStage",
      "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00",
      0x17B6F60u, 0x17B81A0u },
    { "NavPostProcessSlot", /* Registered renderer job, not a renderer-specific function RVA. */
      "48 8B 15 ?? ?? ?? ?? 41 B1 01 48 8B CE E8 ?? ?? ?? ??",
      0x17B77CCu, 0x17B8A0Cu },
    { "BuildAASFindCall", /* R14 is the exact compiled module-instance record. */
      "E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 75 44 45 33 C9 45 33 C0 48 8B 55 A8 "
      "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F8 41 B4 01",
      0x114CFB9u, 0x114C1B9u },
    { "BuildAASLoadCall", /* Temporary-resource branch, released after merge. */
      "E8 ?? ?? ?? ?? 48 8B F8 41 B4 01 48 85 C0 75 23 48 8D 4D 98 E8 ?? ?? ?? ?? "
      "90 48 8D 4D C8 E8 ?? ?? ?? ?? 44 0F B6 64 24 40 48 8B 7C 24 48",
      0x114CFD7u, 0x114C1D7u },
    { "PlacementModelNameCall", /* Appends ".bmodel" to a placement-display model name;
      * R13 is the module-instance record, R12D the navigation class. */
      "E8 ?? ?? ?? ?? 90 48 8D 4D 28 E8 ?? ?? ?? ?? 90 48 8D 4D 58 E8 ?? ?? ?? ?? "
      "45 32 FF 33 FF 41 39 7E 20 7E 68",
      0x1198E7Fu, 0x11980AFu },
    { "NavSearchHeapPush", /* Same verified leaf on Vulkan and OpenGL. */
      "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 4D 8B 08 33 C9 "
      "44 0F B7 1A 49 8B D8 48 8B FA 66 41 39 49 04",
      0x496790u, 0x494E90u },
    { "BlockingVolumeObstacleGate", /* The native affectsNavmesh contents mask gate. */
      "80 BB 8E 0C 00 00 00 74 0A 81 A3 94 0C 00 00 FF FF FD FF "
      "48 8B 8B 08 08 00 00 41 83 C8 FF 8B 93 94 0C 00 00 48 8B 01 FF 50 30",
      0xAE48C8u, 0xAE3968u },
    { "SnapMapEditToSnapBuild", /* int(edit map, build map, ctx). Entry precedes all three BuildAAS calls
                                 * (+0x35C/+0x373/+0x38A), so nav_play reads live marks here
                                 * before conversion invalidates the editable entity state. */
      "48 8B C4 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 C0 FE FF FF FF "
      "48 89 58 08 48 89 70 18 48 89 78 20 4D 8B E0 48 8B F2 4C 8B E9 "
      "4C 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D E0 E8 ?? ?? ?? ?? 90 "
      "4C 8D 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D C8",
      0x1153590u, 0x1152850u },
    { "AddCommand",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
      "41 54 41 56 41 57 48 83 EC 20 48 63 69 10",
      0x297380u, 0x2975E0u },
    { "SpawnByEntityDef",
      "40 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 89 74 24 60 49 8B C0",
      0x38DB50u, 0x38D3F0u },
    { "NameHash",          /* Case-insensitive h=h*31+tolower(c), used by cvar lookup/insertion.
                            * The multiply and lowercase branch distinguish the leaf. */
      "0F B6 01 45 33 C0 4C 8B C9 84 C0 74 ?? 0F 1F 00 8D 50 BF 80 FA 19 77 ?? "
      "04 20 45 6B C0 1F 49 FF C1 0F BE C8 41 0F B6 01 44 03 C1",
      0x33D5F0u, 0x33D240u },
    { "GetDeclsOfType",
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 8B F9 48 85 DB 74 ?? "
      "0F 1F 80 00 00 00 00 48 8B 4B 10",
      0x1572010u, 0x15731A0u },
    /* Native declaration registration contracts:
     * DeclRegistryAnchor: decode MOV RCX,[rip+slot] at +0x10 from a clean entry.
     * DeclRegisterFile: registry vtable +0x38; const idStr *source points to a
     * constructed 48-byte name. An optional default type guides source scanning.
     * DeclTypeByName: registry +0x58, short type name -> type manager.
     * DeclFind: manager/name/makeDefault; callers pass 0 when classifying shadows.
     * DeclSourceFind: manager/logical name -> source record, without creating a
     * live decl. Omit .decl; the engine normalizes the name.
     * grid_room_native validates resolved methods against the live registry vtable. */
    { "DeclRegistryAnchor",
      "40 53 48 83 EC 30 48 8B D9 4C 8D 05 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
      "48 8B D3 48 8B 01 FF 90 C0 00 00 00",
      0x1686030u, 0x1687150u },
    { "DeclRegisterFile",
      "40 57 48 81 EC A0 00 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 C8 00 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 90 00 00 00 49 8B F8 48 8B D9 "
      "48 8D 4C 24 30 E8 ?? ?? ?? ?? 90 48 8D 4C 24 30 E8 ?? ?? ?? ?? "
      "48 8D 4C 24 30 E8 ?? ?? ?? ??",
      0x155E9A0u, 0x155FC70u },
    { "DeclTypeByName",
      "48 89 6C 24 18 56 48 83 EC 20 48 8B EA 48 8B F1 48 85 D2 74 ?? 80 3A 00 74 ?? "
      "48 89 5C 24 30 33 DB 48 89 7C 24 38 39 59 10",
      0x155BAF0u, 0x155CDC0u },
    { "DeclFind",
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 FD FF FF 48 81 EC 10 03 00 00 "
      "48 C7 44 24 68 FE FF FF FF 48 89 9C 24 68 03 00 00",
      0x155AE90u, 0x155C160u },
    { "DeclSourceFind",
      "48 8B C4 55 57 41 54 41 56 41 57 48 8D A8 38 FE FF FF 48 81 EC A0 02 00 00 "
      "48 C7 44 24 20 FE FF FF FF 48 89 58 18 48 89 70 20",
      0x155AC80u, 0x155BF50u },
    { "ResourceGenericLoad", /* void(idResource*). Read source and run native post-parse.
                             * Reconstruction belongs to its fallback path, not every load.
                            * DeclFind normally reaches this through the pending-load path;
                            * the decl server can use it when the pending bit survives lookup.
                            * Anchor: 0x900-byte frame and owner-vtable access. */
      "48 8B C4 57 48 81 EC 00 09 00 00 48 C7 44 24 40 FE FF FF FF 48 89 58 10 "
      "48 89 70 18 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 F0 08 00 00 48 8B F9 "
      "48 89 4C 24 38 48 8B 01 FF 50 10 48 8B C8 E8 ?? ?? ?? ?? 4C 8B C0 48 8B 57 08 "
      "48 8B CF E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ??",
      0x16C6050u, 0x16C6D30u },
    { "GameMgrLea",        /* Game-manager getter starts with MOV RAX,[rip+slot]. Decode the
                            * pointer slot and dereference lazily; it may be NULL at startup. */
      "48 8B 05 ?? ?? ?? ?? 48 83 B8 A0 54 0A 00 00 0F 94 C0",
      0xD4A710u, 0xD49DC0u },
    { "MenuThink",
      "48 8B C4 55 57 41 54 41 56 41 57 48 8D 68 98 48 81 EC 40 01 00 00 48 C7 44 24 38 FE FF FF FF",
      0xFABAA0u, 0xFAAFD0u },
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
      0x11BEE20u, 0x11BE1F0u },
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
      0x11F8EB0u, 0x11F80D0u },

    { "SnapMapAddBranchTag",
      "4C 8B DC 57 48 83 EC 60 49 C7 43 B8 FE FF FF FF 49 89 5B 10 49 89 73 18 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 48 8B F9 48 8D 15 ?? ?? ?? ?? "
      "49 8D 4B C0 E8 ?? ?? ?? ?? 90 48 81 C7 98 09 00 00 8B 77 08 83 EE 01",
      0x11F4E80u, 0x11F40A0u },


    { "WriteLocalSavedMapText",
      /* Same: no wildcards, unique at 20 bytes by both tools (target 0x576530). */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 BE FF FF",
      0x11D2DC0u, 0x11D20F0u },
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
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 90 00 00 00 48 C7 45 C7 FE FF FF FF 48 89 58 18",
      0x1182E80u, 0x1181FE0u },
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
      0x1184F40u, 0x11840A0u },
    { "MenuPump",
      "40 56 41 57 48 81 EC 18 01 00 00",
      0xF91D00u, 0xF91230u },
    { "AddToSelection",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B A9 88 00 00 00",
      0x11FAD50u, 0x11F9F70u },
    { "ClearSelection",
      "45 33 D2 44 39 91 88 00 00 00",
      0x11FB540u, 0x11FA7E0u },
    { "WireConnectCreator1", /* void(tool,world,index): output-node source creator. The detour routes
                              * bare targets to native references instead of CSR edges.
                              * MOVSXD RDI,R8D plus world+0x204C8 separates sibling creators. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 63 F8 48 8B F2 48 8B D9 83 FF FF "
      "0F 84 ?? ?? ?? ?? 48 8B 8A C8 04 02 00 48 8B 81 A0 06 00 00 48 8B 04 F8 F6 80 64 01 00 00 20",
      0x125D680u, 0x125CC70u },
    { "ConnectOutputCreator", /* void(tool,world,index): base-entity source creator.
                               * MOV R9,[RDX+0x204C8] and MOVSXD R10,[RCX+0x10]
                               * distinguish it from the other creator variants. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
      "4C 8B 8A C8 04 02 00 48 8B EA 4C 63 51 10",
      0x125D830u, 0x125CE20u },
    { "Toast",
      "40 57 48 83 EC 20 48 8B F9 48 8B 89 F0 08 00 00",
      0x1272760u, 0x1271D10u },
    /* Engine dialog text uses the descriptor's nonempty idStr instead of default GDM text. */
    { "AddDialog",
      "40 55 57 41 54 41 56 41 57 48 8D AC 24 F0 BC FF FF",
      0x102E4E0u, 0x102D780u },
    /* Raise through the shell wrapper: it sets screen+0xA8 for input handling.
     * Calling AddDialog directly can display a dialog that ignores keys. */
    { "AddDialogWrapper",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 8B DA 48 8B F9 48 8B 0D ?? ?? ?? ?? 48 8B 01 33 D2 FF 50 48 90 48 8B D3 48 8B 4F 08 E8 ?? ?? ?? ?? 48 8B 47 18",
      0xFCC6A0u, 0xFCBBE0u },
    { "ShowDialog",
      "48 8B C4 57 48 81 EC 80 00 00 00 48 C7 40 B8 FE FF FF FF 48 89 58 18 48 89 70 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 78 48 8B F2",
      0x1034180u, 0x1033420u },
    /* Hide the active widget before its descriptor callbacks are released.
     * The shell wrapper holds the native lock and updates input visibility. */
    { "ClearDialogWrapper",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 8B DA 48 8B F9 48 8B 0D ?? ?? ?? ?? 48 8B 01 33 D2 FF 50 48 90 48 8B D3 48 8B 4F 08 E8 ?? ?? ?? ?? 48 8B 07 48 8B CF FF 90 60 02 00 00",
      0xFCC750u, 0xFCBC90u },
    /* Assign text into an already-constructed idStr; do not use its constructor here. */
    { "IdStrAssignCStr",
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B FA",
      0x33A7F0u, 0x33A440u },
    /* Dialog button callbacks report their action ID here; the descriptor
     * does not contain a result byte to poll. */
    { "DialogAction",
      "40 55 56 57 41 56 41 57 48 8D AC 24 50 79 FF FF B8 B0 87 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 44 24 38 FE FF FF FF",
      0x1031CB0u, 0x1030F50u },
    { "IdStrCtor",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B DA 48 89 01 48 8B F9",
      0x33A0F0u, 0x339D40u },
    { "IdStrDtor",
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B 51 10",
      0x33A320u, 0x339F70u },
    { "EntityDefCtor",
      "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 E8 ?? ?? ?? ?? 90 48 8D 8B 68 01 00 00",
      0x1245610u, 0x1244C40u },
    { "EntityDefDtor",
      "48 89 5C 24 08 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 48 89 01 48 81 C1 30 01 00 00",
      0x15634D0u, 0x15647B0u },
    { "DeclSourceRebuild",
      "48 85 D2 0F 84 ?? ?? ?? ?? 55 56 57 41 56 41 57 48 81 EC C0 01 00 00",
      0x1564BC0u, 0x1565E90u },
    { "IdStrAssign",
      "40 53 48 83 EC 20 48 8B D9 45 33 C9 48 8D 0D ?? ?? ?? ??",
      0x32B3D0u, 0x32B040u },
    { "Lexer",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F1 41 0F B6 D9",
      0x279C60u, 0x279EB0u },
    { "StructDeserialize",
      "4C 8B DC 57 48 83 EC 70 49 C7 43 C8 FE FF FF FF 49 89 5B 10 49 89 73 18",
      0x34D660u, 0x34CFF0u },
    { "LexCtxCtor",
      "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 33 C0 "
      "C7 41 10 00 00 05 00 48 89 01 48 89 41 08 C7 41 28 00 00 05 00 48 89 41 18 "
      "48 89 41 20 48 83 C1 30",
      0x2787F0u, 0x278A40u },
    { "ParseNodeCtor",
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 89 51 08",
      0x27B6F0u, 0x27B930u },
    { "ParseNodeDtor",
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 8B 41 08",
      0x27B930u, 0x27BB70u },
    { "BufferCommandText",
      "83 B9 A8 00 02 00 00 41 B8 40 00 00 00",
      0x2974D0u, 0x297730u },
    { "EntityClone",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 81 50 01 00 00",
      0x1202000u, 0x12013D0u },
    { "StructSerialize",
      "4C 8B DC 57 48 81 EC 80 00 00 00 49 C7 43 B8 FE FF FF FF 49 89 5B 10 49 89 73 18",
      0x351900u, 0x351290u },
    { "TreeRenderJson",
      "40 57 48 81 EC E0 00 00 00 48 C7 44 24 28 FE FF FF FF",
      0x27DBB0u, 0x27DE00u },
    /* Language-string injection. */
    { "StridsSortBody",    /* idLangDict radix-sort body (0x1a2b480 wrapper -> JMP here); detour target */
      "48 89 5C 24 18 55 56 57 48 8D AC 24 10 F8 FF FF",
      0x281D60u, 0x281F50u },
    { "StridsTableLea",    /* idLangDict::GetIndexForId -- carries LEA RCX,[the strids idList global]; the
                            * build-portable anchor we decode to find the string-table descriptor global */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 48 85 D2 74 ?? 80 3A 00",
      0x2815A0u, 0x281790u },
    { "StridsInsert",      /* idList<StridEntry>::Append (32-byte element). Long sig: a near-twin idList::
                            * Append shares the prologue; uniqueness only at the element-copy body (+74B). */
      "48 89 5C 24 08 57 48 83 EC 20 8B 41 0C 48 8B FA 48 8B D9 39 41 08 75 ?? "
      "E8 ?? ?? ?? ?? 84 C0 75 ?? 83 C8 FF 48 8B 5C 24 30 48 83 C4 20 5F C3 "
      "48 63 43 08 3B 43 0C 7D ?? 48 8B C8 8B 07 48 C1 E1 05 48 03 0B 89 01 48 8B 47 08",
      0x2803D0u, 0x2805C0u },
    { "StridsHash",        /* Lowercasing FNV-1a hash used by language-string keys. */
      "48 89 5C 24 08 57 48 83 EC 20 0F B6 01",
      0x280460u, 0x280650u },
    /* Resource-provider vtable anchor. */
    { "ResProviderCtor",   /* Decode LEA RAX,[rip+vtable] from this constructor. Override-open
                            * replaces slot +0xF8 after provider-layout validation. */
      "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 33 FF C7 41 18 00 00 ?? 00 48 89 79 08 48 89 79 10 48 83 C1 30",
      0x2A28C0u, 0x2A2A90u },
    { "IdFileReadString",   /* Native idFile ReadString at +0xE0 (pinned Vulkan 0x267390).
                              * Install directly to retain its engine idStr ABI. */
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 01 48 8B DA",
      0x299850u, 0x299AB0u },
    { "IdFileCompare",      /* Native idFile/idStr comparison at +0xE8 (pinned Vulkan 0x267290).
                              * Its calling convention must not be replaced by a guessed wrapper. */
      "40 53 55 56 57 48 81 EC 98 00 00 00 48 C7 44 24 20 FE FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 00 00 00 41 8B E9",
      0x2996D0u, 0x299930u },
    { "IdFileWriteString",   /* Native WriteString at +0xF0; publish only after all three helpers are clean. */
      "40 57 48 83 EC 70 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 88 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 48 8B F9 49 8B D0",
      0x29A0B0u, 0x29A310u },
    /* Console and cvar registration. */
    { "Printf",            /* Engine message dispatch; sh_printf supplies message class 1 and va_list. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 BA FF FF",
      0x2831E0u, 0x283400u },
    { "CvarRegister",      /* Outer cvar registration initializes engine globals before forwarding
                            * to idCVarSystem::Register. Product flags include type and NOCHEAT. */
      "48 8B C4 48 89 48 08 57 48 83 EC 60 48 C7 40 E8 FE FF FF FF 48 89 58 10 48 89 68 18 "
      "48 89 70 20 41 8B F9",
      0x293870u, 0x293A90u },
    { "CmdSystemLea",      /* Bot-command registrar loads cmdSystem with MOV RCX,[rip+slot] at +6.
                            * Decode MOV as well as LEA; this is a pointer slot, not the object.
                            * The longer body distinguishes its otherwise generic prologue. */
      "40 53 48 83 EC 30 48 8B 0D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 DB 4C 8D 05 ?? ?? ?? ?? "
      "89 5C 24 28 48 8D 15 ?? ?? ?? ?? 48 89 5C 24 20 48 8B 01 FF 50 20 48 8B 0D ?? ?? ?? ?? "
      "4C 8D 0D ?? ?? ?? ??",
      0x680190u, 0x67F430u },
    /* Reflection lookups. The decl-manager accessor resolves separately from
     * a signed call site in engine_globals_table.gen.h. */
    { "FindTypeInfoByName", /* record *(reflect,name,scope). A caller consumes the return value even
                             * though the decompiler inferred void. Field-layout details live
                             * in typeinfo.c; the large-frame prologue distinguishes this lookup. */
      "40 55 56 57 48 8D AC 24 50 BE FF FF",
      0x34D7A0u, 0x34D130u },
    { "FindEnumByName",     /* enum record *(reflect,name); sh_type uses it after class lookup misses.
                             * Member-array layout is documented beside the reader in typeinfo.c. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 48 8B EA 48 8B CA "
      "E8 ?? ?? ?? ?? 23 43 24",
      0x34DBB0u, 0x34D540u },
    { "MapGetter",          /* MapGetter(gameMgr) returns or constructs the live map object. */
      "40 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 58 48 8B D9 48 8B B9 B0 A0 29 00",
      0x392D70u, 0x392640u },
    { "MapWriter",          /* MapWriter(map,path) exports v5 maps. The fixed cmp [rcx+0x38],5
                             * anchors the format gate; entity.c validates the destination. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 FE FF FF 48 81 EC B0 02 00 00 "
      "48 C7 44 24 30 FE FF FF FF 48 89 9C 24 00 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
      "48 89 85 A0 01 00 00 48 8B F2 48 8B F9 83 79 38 05",
      0x157B260u, 0x157C2F0u },
    { "SessionDevModeGetter", /* Bool getter for session+0x34C89. Devmode disable replaces its first
                               * three bytes with xor eax,eax; ret. The fixed field offset
                               * refuses incompatible layouts; unpatch restores the signature. */
      "0F B6 81 BB 49 03 00 C3 CC CC CC CC CC CC CC CC",
      0x1596A40u, 0x15979B0u },
    { "RenderLogStub",        /* Render trace sink: mov [rsp+0x20],r9; ret, followed by padding.
                               * Padding distinguishes it from another short stub and supplies
                               * 16 bytes of room. The replacement never calls a trampoline. */
      "4C 89 4C 24 20 C3 CC CC CC CC CC CC",
      0x18807B0u, 0x187E790u },
    /* Optional math overrides replace complete functions; trampolines are unused. */
    { "AlgoMatMul",        /* void(A[16],B[16],out[16]); row-major product.
                            * sh_algo_matmul accumulates in double and stores float. */
      "48 8B C4 48 81 EC E8 00 00 00 0F 10 01 4C 8D 18",
      0x2C5840u, 0x2C58A0u },
    { "AlgoInverse",       /* bool(M[16],out[16]); singular |det|<1.0000000168623835e-16
                            * returns 0 without touching output. The replacement retains this contract. */
      "48 8B C4 48 81 EC 98 01 00 00 0F 10 19 0F 10 41 10",
      0x2C5220u, 0x2C5280u },
    { "AlgoPackRGBA",      /* RGBA8 packing. The engine truncates; the override rounds half-up
                            * in double. A RIP-relative instruction starts at +10, so the
                            * 14-byte stolen window must never be executed as a trampoline. */
      "F3 0F 10 01 41 BA FF 00 00 00 F3 0F 10 0D ?? ?? ?? ??",
      0x277780u, 0x1A59EF0u },
    { "AlgoCurveEval",     /* float(curve,t,mode); count@+0x20C, times@+0x0C, values@+0x10C.
                            * The replacement supports linear/hold and Catmull-Rom modes.
                            * Only linear interpolation applies the small-value zero flush. */
      "48 89 5C 24 10 56 48 83 EC 40 8B 81 0C 02 00 00",
      0x284410u, 0x284600u },
    /* Model-builder and render-debug dependencies. Revalidate signatures and
     * separate object-layout constants when porting to another build. */
    { "Md6Ctor",           /* idMd6Builder constructor, called first. The distinctive
                            * [self+0x38]=-1 store separates otherwise similar constructors. */
      "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B D9 "
      "E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 03 48 C7 43 38 FF FF FF FF",
      0x199E3B0u, 0x1992CE0u },
    { "Md6SetOutput",      /* void(md6,output idStr). Copies output after input/options setup.
                            * Function discovery may require inspecting raw prologue bytes. */
      "40 57 48 81 EC D0 01 00 00 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 F0 01 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 C0 01 00 00 48 8B DA 48 8B F9 48 8D 4C 24 60",
      0x199EF90u, 0x19937E0u },
    { "Md6Build",          /* Final void(md6) operation compiles and releases the builder; do not
                            * treat its destructor-shaped body as a separate post-build cleanup. */
      "40 57 48 83 EC 50 48 C7 44 24 40 FE FF FF FF 48 89 5C 24 60 48 89 6C 24 68 "
      "48 89 74 24 70 48 8B D9 48 8D 05 ?? ?? ?? ?? 48 89 01 33 ED 8B F5",
      0x199E9C0u, 0x19932F0u },
    { "DefaultIdStrCtor",  /* Construct an empty idStr: data=self+0x1C, len=0, capacity=0x80000014.
                            * Distinct from IdStrCtor, which constructs from a C string. */
      "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 E8 ?? ?? ?? ?? "
      "48 8D 43 1C C7 43 18 14 00 00 80 48 89 43 10 C7 43 08 00 00 00 00 C6 00 00",
      0x33A240u, 0x339E90u },
    { "BModelBuilder",     /* BModelBuilder(out208,input,output,options idStr). Arg1 is a
                            * 0xD0 result buffer filled with 0x01; arg4 is a constructed idStr.
                            * Recheck the caller and builder argument reads when porting. */
      "40 53 55 56 57 48 81 EC 28 01 00 00 48 C7 44 24 30 FE FF FF FF 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 84 24 10 01 00 00 49 8B F1 49 8B E8 48 8B FA 48 8B D9",
      0x18A83C0u, 0x189E880u },
    { "RenderWorldGetter", /* Render-world registration window has LEA RCX,[rip+slot] at +6.
                            * Decode then dereference; it is not an object accessor.
                            * Render vtable and editor-field offsets remain separate contracts
                            * documented at their consumers in commands.c. */
      "89 B3 A8 02 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
      "48 85 C9 74 12 66 C7",
      0x1240438u, 0x123FA68u },
    { "SwfTextOnKeyCall",   /* void*(self,retbuf,thisObject,parms): SWF text-field onKey::Call.
                             * TextField+0xC0 gives its instance; parms holds scancode/isDown.
                             * Re-find via RTTI onKey/onChar classes and differing vtable slot 10,
                             * then recheck the fields used in swf_textedit.c.
                             * Newer retail RVA 0x17505E0 is not this table's pinned build. */
      "40 55 56 57 41 56 41 57 48 81 EC D0 00 00 00 48 C7 44 24 28 FE FF FF FF "
      "48 89 9C 24 00 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 C0 00 00 00 "
      "49 8B F9 49 8B E8 4C 8B F2 45 33 FF",
      0x17505E0u, 0x17517C0u },
    { "IdStrAssignFromStr", /* Assign one idStr to another, growing destination as needed.
                             * src length/data are +8/+0x10; zero flags decline buffer stealing.
                             * Do not confuse this with IdStrAssign, the interned-name helper. */
      "48 89 74 24 18 57 48 83 EC 40 48 8B F2 48 8B F9 8B 49 18 8B D1 C1 EA 1F 80 E2 01 74 ?? "
      "44 8B 46 18 41 8B C0",
      0x33A380u, 0x339FD0u },
    { "PrefabCtor",         /* idSnapEntityPrefab *(self). Initializes fields without freeing old
                             * contents, then constructs the tail at +0x120. Reinitialization
                             * can abandon dangling staging pointers without double-freeing.
                             * Newer retail RVA 0x11AC8D0 is a different extraction build. */
      "4C 8B DC 49 89 4B 08 53 48 83 EC 30 49 C7 43 E8 FE FF FF FF 48 8B D9",
      0x11AC8D0u, 0x11AB9E0u },
    { "PrefabPopulate",     /* char(prefab,editor,status*). Build from current selection.
                             * Re-find through the "Failed to create prefab" error strings.
                             * Newer retail RVA 0x11ADB30 is not the pinned build. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 F7 FF FF "
      "48 81 EC C0 09 00 00 48 C7 85 10 01 00 00 FE FF FF FF 48 89 9C 24 18 0A 00 00",
      0x11ADB30u, 0x11ACD30u },
    { "PrefabGrabFrame",    /* void(editor,body,frame[12],grabDistance*). PrefabPopulate's grab
                             * frame: clips the view-centre line (editor+0x170, angles +0x17C)
                             * against the hovered body's box (+0x2C4 bounds, or +-0.5 x
                             * +0x400/+0x404/+0x434 when decl+0x3CE & 8), keeps the midpoint
                             * even on a miss, and copies the body axis +0x294. Callers:
                             * PrefabPopulate and the selection-grab setup. Re-find via the call
                             * before PrefabPopulate's "prefab+0x24" store. */
      "40 55 53 56 57 41 56 48 8D 6C 24 C9 48 81 EC D0 00 00 00 0F 29 B4 24 C0 00 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 17 F2 0F 10 81 70 01 00 00 48 8B FA "
      "8B 81 78 01 00 00 48 8D 55 AF 48 81 C1 7C 01 00 00",
      0x12164D0u, 0x1215920u },
    { "MemLocalGet",       /* idMemLocal *(void). Resolve the accessor rather than computing the
                             * singleton address. Many magic-static getters resemble this;
                             * frame size, MOV R8D,4, and GS TLS load distinguish the pattern. */
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 8B 0D ?? ?? ?? ?? "
      "65 48 8B 04 25 58 00 00 00 41 B8 04 00 00 00 48 8B 14 C8 48 8D 1D ?? ?? ?? ??",
      0x31D4A0u, 0x31D020u },
    { "MemLocalPushHeap",  /* void(self,heapId). Push the 32-entry scope stack at +0x44, depth +0xC4.
                             * Main-thread-only: heap -1 allocation consults this same gate.
                             * PushHeap(0) selects the process heap so clipboard data survives
                             * map teardown. The overflow assertion incorrectly names PopHeap. */
      "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 16 "
      "48 63 83 C4 00 00 00 83 F8 20 7D 15 89 7C 83 44",
      0x3217A0u, 0x3213E0u },
    { "MemLocalPopHeap",   /* void(self). Pop the main-thread heap scope; underflow is fatal.
                             * Balance successful pushes with __finally across every exit.
                             * SUB [self+0xC4],1 followed by JS identifies the underflow path. */
      "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 09 83 AB C4 00 00 00 01 78",
      0x321770u, 0x3213B0u },
    { "PasteInstantiate",   /* void(prefab,editor). Instantiate staged entities and remap references.
                             * Selection must be empty: old indices map through the appended
                             * selection array. Existing entries would silently miswire the paste.
                             * The native idle action 0x5C calls this and then EnterAddPrefabGrab;
                             * calling it alone leaves incomplete placement state.
                             * Re-find via EnterAddPrefabGrab's sole caller, then the preceding
                             * call with LEA RCX,[editor+0x209A8] and MOV RDX,editor. */
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 A8 F7 FF FF "
      "48 81 EC 20 09 00 00 48 C7 45 E0 FE FF FF FF 48 89 58 18",
      0x11AF070u, 0x11AE270u },
    { "EnterAddPrefabGrab",  /* void(mode). Enter add-prefab manipulation after instantiation.
                             * mode+0x2D0 clears Duplicate (0x04), sets Add Prefab (0x08);
                             * +0x2D1 |= 1 restores selection, +0x1AC=4 enters manipulation,
                             * +0xBB8 requests redraw. Siblings differ by flavor masks,
                             * so preserve the full body when rederiving the paste path. */
      "80 A1 D0 02 00 00 FB 80 89 D0 02 00 00 08 80 89 D1 02 00 00 01 "
      "C7 81 AC 01 00 00 04 00 00 00 C6 81 B8 0B 00 00 01 C3",
      0x1254B80u, 0x12541A0u },
    { "SoundWorldLea",      /* Sound-system update reads the sound-world slot with MOV RBX,[rip+disp32].
                             * The shared decoder accepts RAX/RCX only; soundpreview uses its
                             * any-register decoder. Profile color 0xFF00FF00 anchors the body. */
      "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 1E 48 8D 15 ?? ?? ?? ?? B9 00 FF 00 FF",
      0x16890E0u, 0x168A200u },
    { "SoundPreview",       /* void*(world,outHandle,name), sound-world vtable +0x30.
                             * Editor audition sets solo/listener state and allocates an emitter;
                             * stop the previous handle before starting another. An empty/missing
                             * resolved name clears preview state, but find-or-create can fatal
                             * on invalid decls: validate names through the asset index first. */
      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 41 54 41 56 41 57 48 83 EC 50 "
      "4C 8B E1 4D 8B F0 49 8B C8 4C 8B FA E8 ?? ?? ?? ?? 33 F6 8B E8 39 35 ?? ?? ?? ?? 7E 5E",
      0x185EDD0u, 0x18600C0u },
    { "Mega2PageDecode",    /* void(header16,payload,NULL,out): CPU-only megatexture page decode.
                             * Output is five 128x128 RGBA planes at 0x10000-byte intervals;
                             * plane 0 is albedo. Preclear all 0x50000 output bytes because
                             * skipped streams remain untouched. Supply at least 0x40000
                             * zero-filled bytes after payload; the decoder reads past page data.
                             * Re-find through the renderer Transcode.cpp code path. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 30 01 00 00 48 8D 6C 24 40 "
      "48 C7 45 10 FE FF FF FF 48 89 9D 40 01 00 00 48 8B 05 ?? ?? ?? ??",
      0x1A4BC80u, 0x1A64C70u },
    { "PrefabDtor",         /* void(prefab). Destroy temporary storage after selection serialization. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 8B F9 "
      "48 81 C1 20 01 00 00 E8 ?? ?? ?? ?? 90 48 8D 8F F0 00 00 00",
      0x117DC40u, 0x117CCC0u },
    { "EntityDeshare",      /* void*(entitySlot). Make the shared 0x6F8 block unique before editing.
                             * Refcount==1 and the allocation size distinguish this helper. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 8B F9 "
      "48 8B 01 83 38 01 74 3C B9 F8 06 00 00 E8 ?? ?? ?? ?? 48 8B D8",
      0x118C2C0u, 0x118B420u },
    /* Portable function identities checked on both renderers. Generic/short leaves
     * such as decl-manager access, list growth, and visibility resolve through
     * signed call sites in engine_globals_table.gen.h instead. */
    { "RemoveFromSelection", /* void(selection [rcx], entity id [edx]). Despite the historical
                              * name this DESTROYS one entity (membership, undo record, slot
                              * release) and never edits the selected-ID array, hover or
                              * EntityMode. No product path calls it any more; window deletion
                              * uses SelectionDeleteAction. The `add rcx,0x5e0` to the map's
                              * entity tables sits in the fixed bytes, which makes it unique.
                              * Vulkan 0x59FDA0, OpenGL 0x59F510. */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8B 09 48 81 C1 E0 05 00 00",
      0x11FB8E0u, 0x11FAB80u },
    { "MaterialHeight",     /* uint(material [rcx]) -- the declared image height. Byte-identical to
                             * MaterialWidth except the final stack slot (0x60 vs 0x68), which is
                             * exactly what separates them; both are file-wide unique.
                             * Vulkan 0xD75B40, OpenGL 0xD75DB0. */
      "40 53 48 83 EC 50 F6 41 68 20 BB 01 00 00 00 89 5C 24 60 74 24 4C 8D 44 24 60",
      0x16C8EA0u, 0x16C9B80u },
    { "MaterialWidth",      /* uint(material [rcx]) -- see MaterialHeight.
                             * Vulkan 0xD75D40, OpenGL 0xD75FB0. */
      "40 53 48 83 EC 50 F6 41 68 20 BB 01 00 00 00 89 5C 24 60 74 24 4C 8D 44 24 68",
      0x16C90A0u, 0x16C9D80u },
    { "DeclPureFind",       /* the pure decl lookup the typeinfo probes call -- decl *(ctx [rcx],
                             * name [rdx]). Distinct from DeclFind (0x17B36F0), which is the registry
                             * form. Vulkan 0x18017A0, OpenGL 0x17F40E0. */
      "40 57 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 50 48 89 74 24 58 "
      "48 8B DA 48 8B F1",
      0x1572A80u, 0x1573C10u },
    { "SerializedEntityIndexWrite", /* Swap-removal's moved-entity index write.
                             * Vulkan 0x350CE1, OpenGL 0x350D31. The following
                             * count decrement and state reset identify this site. */
      "89 A8 64 0B 00 00 8B 97 70 A0 49 00 FF CA E8 ?? ?? ?? ?? "
      "33 D2 C7 86 64 0B 00 00 FF FF FF FF 48 8B CE E8 ?? ?? ?? ??",
      0x3C9011u, 0x3C89B1u },
    { "EventLink",          /* Event-link routine guarded by the fault shield.
                             * Vulkan 0x9C2370, OpenGL 0x9C1B70. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 89 6C 24 50 "
      "48 89 74 24 58 48 8B F2 48 8B E9",
      0xB44C20u, 0xB44210u },
    { "InteractableSpawn",  /* Interactable Spawn, reached through a vtable. Its frame setup
                             * distinguishes the function. Vulkan 0x1232830, OpenGL 0x12247F0. */
      "48 8B C4 55 41 54 41 55 41 56 41 57 48 8D A8 98 FE FF FF 48 81 EC 40 02 00 00 "
      "48 C7 45 80 FE FF FF FF 48 89 58 10 48 89 70 18 48 89 78 20 0F 29 70 C8 "
      "0F 29 78 B8 44 0F 29 40 A8 48 8B 05 ?? ?? ?? ??",
      0x921AC0u, 0x920BD0u },
    { "DeclResourceProbe",  /* Decl existence probe at vtable +0x78; validate the method by signature.
                             * Vulkan 0x1806100, OpenGL 0x17F8A40. */
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FF FF FF "
      "48 81 EC 88 01 00 00 48 C7 44 24 30 FE FF FF FF",
      0x1695FE0u, 0x1696E60u },
    /* Native module properties and XYZ inspector. Independently matched on
     * Vulkan/OpenGL at D0BCA0/D0B3B0, D0A0B0/D09980, D071F0/D06B70 and
     * D306F0/D2FDB0. No pinned-image RVA fallback is authorized. */
    { "GridModuleProperties",
      "40 55 56 57 41 56 41 57 48 8D AC 24 70 C0 FF FF B8 90 40 00 00 E8 ?? ?? ?? ?? "
      "48 2B E0 48 C7 44 24 60 FE FF FF FF 48 89 9C 24 C8 40 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 80 3F 00 00 49 8B F1 4D 8B F0 8B FA 48 8B D9", 0x1283FD0u, 0x1283620u },
    { "GridPropertyChanged",
      "48 85 D2 0F 84 ?? ?? ?? ?? 48 8B C4 55 56 57 48 8D 68 A1 "
      "48 81 EC A0 00 00 00 48 C7 45 F7 FE FF FF FF", 0x1282450u, 0x1281AA0u },
    { "GridAddVec3",
      "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 B8 48 81 EC 10 01 00 00 "
      "48 C7 45 90 FE FF FF FF 48 89 58 20 0F 29 70 B8 0F 29 78 A8", 0x127F6E0u, 0x127EC90u },
    { "GridSetVec3",
      "40 53 B8 80 40 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 44 24 30 FE FF FF FF "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 70 40 00 00 48 8B D9 8B 02 89 81 00 01 00 00", 0x12A8610u, 0x12A7CF0u },
    /* Native palette lookup and one-module loader, verified in both images:
     * Vulkan 5B7380/5B7910, OpenGL 5B6A70/5B7000. */
    { "GridFindModule",
      "4C 8B DC 49 89 5B 18 49 89 53 10 57 48 83 EC 40 48 63 41 18 4D 8D 4B 08 "
      "4C 8B 51 10 4D 8D 43 10 48 69 F8 98 00 00 00", 0x1212D20u, 0x1212170u },
    { "GridHasModule", /* VK 5b6f40, GL 5b6630; separate reload membership probe. */
      "48 89 54 24 10 48 83 EC 48 48 8D 05 ?? ?? ?? ?? 48 89 44 24 30 4C 8D 4C 24 50 48 63 41 18", 0x12128E0u, 0x1211D30u },
    { "GridPortalSnap", /* VK 5a9b70, GL 5a9180. */
      "40 55 53 57 41 54 41 55 41 57 48 8D 6C 24 D1 48 81 EC B8 00 00 00 45 33 FF 44 0F 29 4C 24 70 45 0F B6 E1", 0x12055B0u, 0x12049B0u },
    { "GridPortalAlign", /* VK 5a0b80, GL 5a02f0. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 60 4C 8B F9 4D 63 E0 48 8B 09 4C 8B EA", 0x11FC6C0u, 0x11FB960u },
    { "GridQuantizeOrigin", /* VK 514df0, GL 514610. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 0F 29 74 24 30 48 8B EA 0F 29 7C 24 20", 0x1175BB0u, 0x1174B40u },
    { "GridConfirmOrigins", /* VK 5a1750, GL 5a0ec0; add/move/duplicate release. */
      "40 55 56 41 57 48 83 EC 40 45 33 FF 0F 29 74 24 30 0F 28 F1 48 8B F1 41 8B EF 44 39 79 70 0F 8E ?? ?? ?? ?? 48 89 5C 24 60", 0x11FD290u, 0x11FC5A0u },
    { "GridPortalBounds", /* VK 5547d0, GL 553c80. */
      "40 53 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 38 48 8B DA 48 8B 11 4C 8B 0A", 0x11B39A0u, 0x11B2D80u },
    { "GridPortalDirection", /* VK 5548c0, GL 553d70. */
      "48 8B 01 4C 8B C9 4C 8B 00 48 63 C2 48 6B D0 1C 49 8B 80 E8 00 00 00", 0x11B3A90u, 0x11B2E70u },
    { "GridLoadModule",
      "40 55 56 57 41 56 41 57 48 81 EC 60 01 00 00 48 C7 44 24 40 FE FF FF FF "
      "48 89 9C 24 98 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 50 01 00 00 "
      "49 8B E9 49 8B F0 48 8B FA 4C 8B F9 48 8B 9C 24 B0 01 00 00", 0x12132F0u, 0x1212700u },
    /* Replace one compiled instance and recompute native portal connections.
     * Vulkan 5993A0/59DA90; OpenGL 598B10/59D200. */
    { "GridReplaceInstance",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 81 68 07 00 00 "
      "48 8B F9 48 63 DA 48 81 C1 E0 05 00 00", 0x11F4F60u, 0x11F4180u },
    { "GridReconnectPortals",
      "4C 8B DC 41 56 48 81 EC 80 00 00 00 49 C7 43 B0 FE FF FF FF 49 89 5B 10 "
      "49 89 73 18 49 89 7B 20 0F 29 74 24 70 0F 29 7C 24 60", 0x11F95D0u, 0x11F87F0u },
    /* Native collision resources cached by a module wrapper: VK 5929a0, GL 592030. */
    { "GridBuildModuleCollision",
      "40 55 56 57 41 56 41 57 48 83 EC 70 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 B8 00 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 49 63 E8 4C 8B FA", 0x11EE530u, 0x11ED860u },
    /* Native COW edit, local transform setter and bounds transform. Verified
     * VK 5a0280/545040/554cf0; GL 59f9f0/544960/5541a0. */
    { "GridEditEntity",
      "48 8B 01 44 8B D2 44 8B CA 41 83 E2 3F 49 C1 E9 06 4C 8B 80 D0 06 00 00 48 63 D2 4B 8B 04 C8 4C 0F AB D0", 0x11FBDC0u, 0x11FB060u },
    { "GridSetEntityTransform",
      "8B 02 89 81 88 02 00 00 8B 42 04 89 81 8C 02 00 00 8B 42 08 89 81 90 02 00 00 8B 42 0C 89 81 94 02 00 00", 0x11A4910u, 0x11A3B30u },
    { "GridWorldBounds",
      "48 8B C4 48 89 58 18 57 48 81 EC A0 00 00 00 0F 29 70 E8 0F 29 78 D8 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 44 24 70 F2 0F 10 41 0C 49 8B F8 8B 41 14 48 8B DA 44 8B 41 18", 0x11B3EC0u, 0x11B32A0u },
    /* Native Blueprint Grid Room properties entry; both renderers verified. */
    { "GridBlueprintUpdate", /* VK 0xcebd80, GL 0xceb760. */
      "40 53 55 57 48 83 EC 20 F6 41 10 10 49 8B D8 48 8B FA 48 8B E9 75", 0x126D910u, 0x126CF00u },
    { "GridBlueprintHelp", /* VK 0xceb950, GL 0xceb330. */
      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D BA 90 09 02 00 48 8B EA 48 8B F1 48 8D 9A A8 11 02 00", 0x126D550u, 0x126CB40u },
    { "GridSetEditorState", /* VK 0x5298a0, GL 0x5291d0. */
      "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 8B 91 18 36 02 00 E8 ?? ?? ?? ?? 48 8B D7 48 8B C8 4C 8B 00 41 FF 50 10", 0x1189580u, 0x11886E0u },
    { "GridInputPressed", /* VK 0x52d190, GL 0x52cac0. */
      "48 89 5C 24 08 57 48 83 EC 20 48 63 C2 48 8B D9 48 6B F8 38 48 8D 05 ?? ?? ?? ?? 48 03 F8 44 0F B6 4F 02 44 0F B6 47 01 0F B6 17 E8 ?? ?? ?? ?? 84 C0 74 36 48 63 47 04 48 3B 83 80 00 00 00", 0x118CB30u, 0x118BC90u },
    { "GridAddActionHelp", /* VK 0x551e40, GL 0x551300. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 80 39 00 48 8D 59 30 41 8B F0 8B FA 74 7B 44 8B 43", 0x11B13B0u, 0x11B0770u },
    { "GridPropertiesBlocked", /* VK 0x5af830, GL 0x5aeed0. */
      "4C 8B 01 44 8B CA 4D 85 C0 74 3C 48 63 49 08 41 8B 40 70 3B C8 75 1D 85 C0 7E 17 8D 41 FF 48 63 C8 49 8B 40 68 48 6B D1 78 44 85 4C 02 08 0F 94 C0 C3 3B C8 7D 11 49 8B 40 68 48 6B C9 78", 0x120B230u, 0x120A630u },
    { "GridEntityTree", /* VK 0x5404a0, GL 0x53fdc0. */
      "40 53 48 83 EC 30 4C 8B 81 50 01 00 00 48 8B DA", 0x119FDD0u, 0x119F000u },
    /* Every undoable editor action ends here: it stores the action's label in
     * the map object and pushes the undo snapshot. The one notification that
     * covers placement, move, rotate, resize, duplicate, property edit and
     * delete. */
    { "EditorCommitEdit", /* VK 0x524070, GL 0x5239b0. */
      "40 53 48 83 EC 20 48 8B D9 48 8B 89 C8 04 02 00 48 83 C1 18 E8 ?? ?? ?? ?? "
      "48 8D 4B 10 48 8B D3 E8 ?? ?? ?? ?? 48 8B 03 BA 01 00 00 00 48 8B CB "
      "48 83 C4 20 5B 48 FF A0 60 01 00 00", 0x1183D60u, 0x1182EC0u },
    /* EntityMode's Selection substate (idSnapEntityMode_Selection), used by
     * editor_delete.c to run the native Delete Selection for window requests.
     * The action's hover-clear callee is decoded from its own call site: that
     * helper has a byte-identical twin and is not signable. */
    { "SelectionDeleteAction", /* VK 0xce3b60, GL 0xce3540. Prologue through the
                                * capability test movzx eax,[rcx+0x58]; test al,2. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC D0 00 00 00 "
      "48 C7 45 AF FE FF FF FF 48 89 9C 24 28 01 00 00 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 45 17 4D 8B E8 48 89 55 9F 48 89 4D A7 0F B6 41 58 A8 02 "
      "0F 84 ?? ?? ?? ??", 0x12657B0u, 0x1264DA0u },
    { "SelectionCapabilities", /* VK 0xce5110, GL 0xce4af0. Rebuilds the capability bits over
                                * the selection plus the hovered entity: and byte [rcx+0x59],0xFE;
                                * and byte [rcx+0x58],0x16; movsxd rsi,[rcx+0x2c]. */
      "40 55 53 48 8B EC 48 83 EC 48 80 61 59 FE 48 8B D9 80 61 58 16 "
      "49 8B 88 D0 04 02 00 48 89 74 24 60 48 89 7C 24 68 49 8B F8 "
      "4C 89 74 24 78 45 32 F6 48 63 71 2C 83 FE FF 0F 84 ?? ?? ?? ?? "
      "4C 89 64 24 70 8B D6 4C 89 7C 24 40", 0x1266D60u, 0x1266350u },
    { "GridApplyEntityTree", /* VK 0x545120, GL 0x544a40. */
      "48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B F9 48 8B F2", 0x11A49F0u, 0x11A3C10u },
    /* Re-applies an entity's type-info tree: GridEntityTree (call +0x17), GridApplyEntityTree
     * (call +0x25), then the owning tree's destructor (call +0x38) and its sized delete with
     * 0x48 (call +0x45). Build 13954591 no longer inlines that cleanup in GridEntityTree, so
     * grid_room_native.c takes the destructor and delete from here. */
    { "GridReapplyEntityTree",
      "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9 48 8D 54 24 48 E8 ?? ?? ?? ?? 90 "
      "48 8B 54 24 48 48 8B CB E8 ?? ?? ?? ?? 90 48 8B 5C 24 48 48 85 DB 74 15 48 8B CB E8 ?? ?? ?? ?? "
      "BA 48 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 83 C4 30 5B C3", 0x11A5000u, 0x11A4220u },
    { "GridReadEntityProperties", /* VK 0x5415d0, GL 0x540ef0. */
      "48 8B C4 55 57 41 56 48 8D A8 48 FF FF FF 48 81 EC A0 01 00 00 48 C7 45 "
      "C8 FE FF FF FF 48 89 58 18 48 89 70 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 "
      "89 85 90 00 00 00 48 8B FA 48 8B D9 48 8B 01 F6 80 CD 03 00 00 01", 0x11A0ED0u, 0x11A0100u },
    { "GridTreeSetVec3", /* VK 0x1a66ee0, GL 0x1a593b0. */
      "40 53 55 56 57 41 54 41 56 41 57 B8 30 80 00 00 E8 ?? ?? ?? ?? 48 2B E0 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 20 80 00 00 40 32 ED 4D 8B E1 "
      "33 DB 4D 8B F8 4C 8B F2 48 8B F1 BF 78 00 00 00 44 8B CF 48 8D 15 ?? ?? "
      "?? ?? 4D 8B C6 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B 16 4C 8B C0 48 8B CE "
      "E8 ?? ?? ?? ?? 48 85 C0 75 ?? 44 8B CF 48 8D 15 ?? ?? ?? ?? 4D 8B C6 48 "
      "8D 8C 24 20 40 00 00 E8 ?? ?? ?? ?? 4D 8B C4 48 8B D0 48 8B CE E8 ?? ?? "
      "?? ?? 48 85 C0 74 ?? F3 41 0F 10 0C 9F 4C 8D 80 90 00 00 00 48 8B CE E8 "
      "?? ?? ?? ?? 40 B5 01 FF C7 48 FF C3 48 83 FB 03 7C ?? 40 0F B6 C5", 0x3580C0u, 0x357A50u },
    { "GridTreeSetFloat", /* VK 0x1a66610, GL 0x1a58ae0. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B FA 0F 29 74 24 20 4C "
      "8B C2 49 8B F1 48 8B 11 0F 28 F2 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 75 ?? "
      "4C 8B C6 48 8B D7 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 ?? 4C 8D 80 90 00 "
      "00 00 0F 28 CE 48 8B CB E8 ?? ?? ?? ?? B0 01 48 8B 5C 24 40 48 8B 74 24 "
      "48", 0x357800u, 0x357190u },
    { "StartMenuBind", /* VK 0xd18470, GL 0xd17bd0. */
      "48 89 5C 24 20 55 48 83 EC 20 48 8B E9 48 89 7C 24 38 48 8B 89 C8 01 00 00 4C 89 74 24 40 4C 8B F2 48 8B 01 FF 50 18 33 DB 41 39 5E 08 7E 29 33 FF", 0x1290560u, 0x128FBB0u },
    { "StartMenuAction", /* VK 0x534a10, GL 0x534330. */
      "40 55 56 57 48 8D AC 24 20 FF FF FF 48 81 EC E0 01 00 00 48 C7 44 24 30 FE FF FF FF 48 89 9C 24 10 02 00 00", 0x1194430u, 0x1193660u },
    { "StartMenuThink", /* VK 0x535020, GL 0x534940. */
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 80 79 2D 00 48 8B FA 0F B6 B2 ED 00 00 00 48 8B D9 75 05 40 84 F6 74 09 E8 ?? ?? ?? ?? C6 43 2D 00", 0x1194A40u, 0x1193C70u },
    { "StartMenuEnd", /* VK 0x535830, GL 0x535150. */
      "40 53 48 83 EC 20 48 8B 82 88 10 02 00 48 8B DA 33 D2 C7 80 2C 0A 00 00 00 00 00 00 48 8B 49 08 E8 ?? ?? ?? ?? 48 8B 83 88 10 02 00", 0x1195250u, 0x1194480u },
    { "StartMenuInit", /* VK 0xd163b0, GL 0xd15b10. */
      "40 55 41 54 41 55 41 56 41 57 48 8D AC 24 F0 BF FF FF B8 10 41 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 45 90 FE FF FF FF", 0x128E4B0u, 0x128DB00u },
    { "StartMenuRow", /* VK 0xd14790, GL 0xd13ef0. */
      "48 8B C4 41 56 48 83 EC 60 48 C7 40 D8 FE FF FF FF 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 8B D8", 0x128C890u, 0x128BEE0u },
    { "StartMenuSelect", /* VK 0xd149b0, GL 0xd14110. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 99 BC 01 00 00 8B F2 48 8B F9", 0x128CAB0u, 0x128C100u },
    { "SwfDuplicateSprite", /* VK 0x2a7240, GL 0x2a6200. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B 79 30 49 8B F0 8B EA 48 8B D9 48 85 FF 75 17 48 8B 51 20 48 8D 0D ?? ?? ?? ??", 0x17469E0u, 0x1747BC0u },
    { "SwfDisplayEntry", /* VK 0x2a7400, GL 0x2a63c0. */
      "48 89 5C 24 10 57 44 8B 89 88 00 00 00 45 33 DB 8B FA 48 8B D9 45 8B D3 45 85 C9 7E 3F 48 89 74 24 10 48 8B B1 80 00 00 00 0F 1F 80 00 00 00 00", 0x1746BA0u, 0x1747D80u },
    { "SwfWidgetChild", /* VK 0xf56780, GL 0xf48040. */
      "85 D2 78 17 3B 91 C0 01 00 00 7D 0F 48 8B 81 B8 01 00 00 48 63 D2 48 8B 04 D0 C3 33 C0 C3", 0x1137FE0u, 0x1137210u },
    { "SwfWidgetResolve", /* VK 0x171ac20, GL 0x170d8b0. */
      "40 53 48 83 EC 20 48 8B 41 18 48 8B D9 48 85 C0 75 09 48 8B 49 20 E8 ?? ?? ?? ?? 8B 53 40 48 8B 48 20 E8 ?? ?? ?? ?? 48 85 C0 48 89 43 08 0F 95 C0", 0xFAE870u, 0xFADD80u },
    { "SwfNestedValue", /* VK 0x2a8c50, GL 0x2a7be0. */
      "40 53 55 56 57 41 56 48 81 EC 80 00 00 00 48 C7 44 24 38 FE FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 78 49 8B F9 4C 8B F2 48 8B 84 24 D0 00 00 00", 0x1748230u, 0x1749440u },
    { "SwfTextInstance", /* VK 0x29e700, GL 0x29d860. */
      "40 53 48 83 EC 20 83 39 08 75 2B 48 8B 59 08 48 85 DB 74 22 4C 8B 03 48 8B CB 48 8B 15 ?? ?? ?? ?? 41 FF 50 08 84 C0 74 0D 48 8B 83 C0 00 00 00 48 83 C4 20 5B C3 33 C0 48 83 C4 20 5B C3 CC CC 40 53 48 83 EC 20 83 39 08 75 2D", 0x17436D0u, 0x1744490u },
    { "SwfValueDestroy", /* VK 0x29e610, GL 0x29d770. */
      "40 53 48 83 EC 20 8B 01 48 8B D9 83 F8 02 75 20 48 8B 49 08 83 69 30 01 75 0A 48 8B 01 BA 01 00 00 00 FF 10 C7 03 00 00 00 00 48 83 C4 20 5B C3", 0x17435E0u, 0x17443A0u },
    { "SwfSetText", /* VK 0x2b7830, GL 0x2b6820. */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8D 4B 38 E8 ?? ?? ?? ?? 33 FF C6 83 4C 01 00 00 00 89 BB 50 01 00 00", 0x17548B0u, 0x1755A10u },
    { "EditorBusyShow", /* VK 0x52a560, GL 0x529e90. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F2 48 8B D9 48 8B 89 88 10 02 00 BA 00 04 00 00 49 8B F8 E8 ?? ?? ?? ?? 48 8B 8B 88 10 02 00", 0x1189F90u, 0x11890F0u },
    { "EditorBusyClear", /* VK 0x524050, GL 0x523990. */
      "48 8B 89 88 10 02 00 BA 00 04 00 00 E9 ?? ?? ?? ??", 0x1183D40u, 0x1182EA0u },
    { "SwfFrameLabel", /* VK 0x2a7480, GL 0x2a6440. */
      "40 57 48 83 EC 70 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 48 8B DA 48 8B F9 48 8B CA E8 ?? ?? ?? ??", 0x1746C20u, 0x1747E00u },
    { "SwfPlayFrame", /* VK 0x2ac0b0, GL 0x2ab040. */
      "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? BA 01 00 00 00 48 8B CF 3B DA 0F 4F D3 E8 ?? ?? ?? ?? 48 8B 47 30 48 85 C0 74 0E 90", 0x174B5B0u, 0x174C7C0u },
    /* Map bounds, Blueprint zoom and the Snapmap+ settings tab (snaphak-re
     * editor-fixes-13954591 R-004). Every pattern is unique on both build
     * 13954591 executables at the recorded RVA. */
    { "CvarSetString", /* idCVar::Set(cvar, text, force); SetFloat formats then calls it. */
      "40 55 56 57 B8 70 40 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 A0 40 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 60 40 00 00 41 0F B6 D8 48 8B F2 48 8B F9 48 8B", 0x295780u, 0x2959A0u },
    { "BlueprintZoomClamp", /* Sky-view dolly: height in [1024, environmentModuleBounds * 0.5]. */
      "F3 0F 58 49 38 F3 0F 10 05 ?? ?? ?? ?? 0F 57 D2 F3 0F 10 D1 0F 28 C8 F3 0F 10 05 ?? ?? ?? ?? F3 0F 5F CA F3 0F 59 05 ?? ?? ?? ?? 0F 28 D0 0F 28 C1 F3 0F 5D D1 F3 0F 11 51 38 C3", 0x12B5130u, 0x12B4810u },
    { "OrbitDollyClamp", /* Free orbit view (mode 0) dolly: z within +/-snapEdit_cameraDistanceLimit. */
      "F3 0F 10 1D ?? ?? ?? ?? 0F 57 D2 48 8B 44 24 28 0F 28 C3 0F 57 05 ?? ?? ?? ?? F3 0F 58 48 08 F3 0F 10 D1 0F 57 C9 F3 0F 10 C8 F3 0F 5F CA", 0x12B57A0u, 0x12B4E80u },
    { "OrbitPanClamp", /* Mode 0 pan: MOVSS of snapEdit_environmentModuleBounds before the 3-axis clamp. */
      "F3 0F 10 0D ?? ?? ?? ?? 0F 28 C1 0F 57 CF F3 0F 10 D9 0F 28 D0 0F 1F 84 00 00 00 00 00 F3 0F 10 00 48 8D 40 04", 0x12B5AE3u, 0x12B51C3u },
    { "EditorClockMs", /* timeGetTime() - base; the editor camera update uses it * 0.001. */
      "48 83 EC 28 FF 15 ?? ?? ?? ?? 2B 05 ?? ?? ?? ?? 48 83 C4 28 C3", 0x32AF10u, 0x32AB90u },
    { "SettingsCategoryCycle", /* Settings state LB/RB: state+0xc = (state+0xc + delta) mod 2, repopulate. */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B C2 C7 44 24 48 31 00 00 00 49 8B F0 48 8D 54 24 48 48 8B F9 45 33 C0 48 8B C8 41 8B D9 E8 ?? ?? ?? ?? 44 8B 4F 0C 44 03 CB 79 14 41 8B C1 B9", 0x118F2D0u, 0x118E450u },
    { "SettingsCategorySelect", /* Notch click: index < 2 and different -> state+0xc = index, repopulate. */
      "41 83 F9 02 7D 78 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B D9 49 8B F0 48 8B C2 48 8B F9 39 59 0C 74 49 45 33 C0 C7 44 24 48 31 00 00 00 48 8D 54 24 48 48 8B C8 E8", 0x118F360u, 0x118E4E0u },
    { "SettingsPanelClear", /* Clears the panel's row, title and value lists and rebuilds the 17 notch hitboxes. */
      "40 53 48 83 EC 20 83 79 30 00 48 8B D9 74 7F E8 ?? ?? ?? ?? 84 C0 74 76 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 13 48 8B CB E8 ?? ?? ?? ?? 48 C7 80 A8 01 00 00 00 00 00 00 48 8B 8B 58 02 00 00 E8", 0x1281860u, 0x1280E10u },
    { "SettingsPanelClearCall", /* The clear call at the start of RenderSettingsPopulate (aligned rel32). */
      "E8 ?? ?? ?? ?? 45 33 ED 4C 89 AE D4 01 00 00 45 8D 75 02 45 85 FF 0F 84 ?? ?? ?? ?? 41 83 FF 01 0F 85", 0x1282E23u, 0x1282473u },
    { "SettingsRepopulate", /* GUI manager -> settings panel -> RenderSettingsPopulate(panel, category). */
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B F2 48 8B F9 8B 51 08 E8 ?? ?? ?? ?? 8B 57 0C 48 8B CF 48 8B D8 E8 ?? ?? ?? ?? 48 8B CB 48 8B F8 E8 ?? ?? ?? ?? 48 85 C0 75 0D 48 8B CF E8 ?? ?? ?? ?? 48 85 C0 74 ?? 8B D6", 0x1273C10u, 0x12731C0u },
    { "EditorUiEvent", /* Editor UI event/sound by id (category change is 0x31). */
      "48 89 6C 24 20 57 41 56 41 57 48 83 EC 50 48 63 02 41 0F B6 E8 4C 8B F2 48 8B F9 4C 8B BC C1 80 37 02 00 4D 85 FF 75 1D", 0x1187530u, 0x1186690u },
    { "SwfSpriteInstance", /* SWF value -> sprite instance (+0xc0); the text twin follows it. */
      "40 53 48 83 EC 20 83 39 08 75 2B 48 8B 59 08 48 85 DB 74 22 4C 8B 03 48 8B CB 48 8B 15 ?? ?? ?? ?? 41 FF 50 08 84 C0 74 0D 48 8B 83 C0 00 00 00 48 83 C4 20 5B C3 33 C0 48 83 C4 20 5B C3 CC CC 40 53 48 83 EC 20 83 39 08 75 2B", 0x1743690u, 0x1744450u },
    { "SwfSpriteSetVisible", /* (sprite, visible, propagate): Populate shows notch_0..1 and hides notch_2..16. */
      "38 51 41 74 5B 88 51 41 84 D2 74 21 48 8B 41 30 48 85 C0 74 18 66 66 66 0F 1F 84 00 00 00 00 00 C6 40 42 01 48 8B 40 30 48 85 C0 75 F3 C6 81 98 00 00 00 01 45 84 C0 74 27", 0x174E3F0u, 0x174F600u },
    { "BlueprintFarClipA", /* Editor camera update: MOVSS xmm1,[33000.0] then idCVar::SetFloat(r_zfar, xmm1, force). */
      "F3 0F 10 0D ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 41 B0 01 45 88 BE 38 04 00 00 45 88 BE 44 04 00 00 45 88 7E 2C E8", 0x1242FAAu, 0x12425DAu },
    { "BlueprintFarClipB", /* The same r_zfar 33000 write on the in-transition path of that update. */
      "F3 0F 10 0D ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 41 B0 01 E8 ?? ?? ?? ?? 41 8B F7 44 39 BF B8 02 00 00 0F 8E", 0x12430EEu, 0x124271Eu },
    /* Map bounds toggle, editor environment volume and Blueprint ground plane
     * (snaphak-re editor-fixes-13954591 R-009). */
    { "RenderAddToggle", /* Settings panel on/off row (idMenuPropertyToggle, 0xe0 bytes): (panel, id, name, &label, &help, flag, value, on, off). */
      "48 8B C4 57 41 56 41 57 48 83 EC 70 48 C7 40 A8 FE FF FF FF 48 89 58 10 48 89 68 18 48 89 70 20 4D 8B F1 49 8B E8 44 8B FA 48 8B F1 B9 E0 00 00 00 E8 ?? ?? ?? ?? 48 8B F8 48 89 84 24 90 00 00 00 48 85 C0 74 39 8B 9E E0 01 00 00 8D 53 01 89 96 E0 01 00 00 48 8B CE E8 ?? ?? ?? ?? 4C 8B C0 0F B6 84 24 C0 00 00 00 88 44 24 20", 0x12808F0u, 0x127FEA0u },
    { "EditorFogVolume", /* Editor render-world setup: _unitCube scaled 32768 and the +/-snapEdit_environmentFogBounds environment area. */
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58 48 8B 01 48 8B DA 41 B0 01 48 8D 15 ?? ?? ?? ?? 48 8B F1 FF 90 90 00 00 00 48 8B C8 48 8B F8 C7 80 F8 00 00 00 00 00 00 47 C7 80 FC 00 00 00 00 00 00 47 C7 80 00 01 00 00 00 00 00 47 48 89 98 18 01 00 00 E8 ?? ?? ?? ?? F3 0F 10 0D", 0x12397B0u, 0x1238C50u },
    { "RenderEntityUpdate", /* Marks a render entity changed (queues it for the next render-world update). */
      "40 53 48 83 EC 20 48 8B 91 C0 00 00 00 48 8B D9 C6 41 30 00 48 85 D2 74 16 48 8B 41 28 8B 48 10 48 C1 E9 08 C6 44 11 08 02 48 83 C4 20 5B C3 48 8B 01 BA 16 00 00 00 FF 50 08", 0x15D8020u, 0x15D92A0u },
    /* Blueprint module picking length (snaphak-re editor-fixes-13954591 R-011). */
    { "ModulePickHover", /* Module hover pick: MOVSS xmm2,[snapEdit_modulePickingDistance] for end = origin + dir * value. */
      "F3 0F 10 15 ?? ?? ?? ?? 41 83 CC FF 44 8B B1 58 07 00 00 0F 28 C2 F3 41 0F 59 01", 0x11F45CCu, 0x11F37ECu },
    { "ModulePickFocus", /* Module Mode focus ray: MOVSS xmm0,[snapEdit_modulePickingDistance]. */
      "F3 0F 10 05 ?? ?? ?? ?? 48 8B 86 C8 04 02 00 F3 0F 10 35", 0x1257A58u, 0x1256F58u },
    /* SnapMap world-area cell size (snaphak-re editor-fixes-13954591 R-018,
     * R-019): BuildRenderWorld's load of the 512.0 subdivision size, the
     * fifth argument of its one subdivision call. Unique on both build
     * 13954591 executables at the recorded RVAs. */
    { "WorldAreaCellLoad", /* MOVSS xmm0,[rip+disp32 -> 512.0]; MOVSS [rsp+0x20],xmm0; LEA r9; LEA r8; XOR edx,edx. */
      "F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 44 24 20 4C 8D 4C 24 50 4C 8D 85 E0 01 00 00 33 D2 49 8B CF E8", 0x114FF66u, 0x114F166u },
    /* Grid Room Size sub-editor refresh after an outside size change
     * (snaphak-re editor-fixes-13954591 R-016). Unique on both build 13954591
     * executables at the recorded RVAs. */
    { "GridPropertyDirtyRefresh", /* Property row: context lists +0x58 vs +0x70 (context vt+0x70) set or clear the dirty byte +0xc8 and relabel the row. */
      "40 53 48 83 EC 20 48 8B D9 48 8B 89 D8 00 00 00 48 8B 01 FF 50 70 0F B6 8B C8 00 00 00 84 C9 74 1D 84 C0 74 30 84 C9 74 08 48 8B CB E8 ?? ?? ?? ?? C6 83 C8 00 00 00 00 48 83 C4 20 5B C3 84 C0 75 13 84 C9 75 08 48 8B CB E8 ?? ?? ?? ?? C6 83 C8 00 00 00 01", 0x12A8E30u, 0x12A8510u },
    { "GridMenuOf", /* Widget -> root editor menu (parent chain +0x18/+0xb0); its +0xa10 is idMenuManager_ContextList_Vec. */
      "48 83 EC 28 48 8B C1 48 8B 49 18 48 85 C9 75 0C 48 8B 48 20 E8 ?? ?? ?? ?? 48 8B C8 48 8B 81 B0 00 00 00 48 85 C0 74 17 0F 1F 84 00 00 00 00 00 48 8B C8 48 8B 80 B0 00 00 00 48 85 C0 75 F1 48 83 C4 28 E9", 0x1281B00u, 0x1281150u },
    { "MenuScreenByIndex", /* Menu manager screen lookup: +0x350[index] into the +0x4d0/+0x4d8 list (the Vec screen is index 10). */
      "4C 8B C1 83 FA FF 75 03 33 C0 C3 48 63 C2 48 63 8C 81 50 03 00 00 41 39 88 D8 04 00 00 7E E9 49 8B 80 D0 04 00 00 48 8B 04 C8 C3", 0xF8F9C0u, 0xF8EEF0u },
    { "ContextListRedraw", /* Context-list screen: redraws its six item rows (name, value text, dirty marker) from the bound context +0x1b8. */
      "40 55 41 54 41 55 41 56 41 57 48 8D AC 24 10 7E FF FF B8 F0 82 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 44 24 70 FE FF FF FF 48 89 9C 24 28 83 00 00 48 89 B4 24 30 83 00 00 48 89 BC 24 38 83 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 E0 81 00 00 48 8B D9", 0x127B000u, 0x127A5B0u },
    /* Object Mode entry drop (snaphak-re editor-fixes-13954591 R-020): the
     * FPS entry solver's load of its 18364.0 downward trace segment, that
     * constant's only reader. Unique on both build 13954591 executables. */
    { "ObjectEntryDrop", /* MOVSS xmm9,[rip+disp32 -> 18364.0]; XORPS xmm7,xmm7; MOVSS xmm8,[rip+disp32 -> 1.0]; MOVSS [rsp+0x60],xmm2. */
      "F3 44 0F 10 0D ?? ?? ?? ?? 0F 57 FF F3 44 0F 10 05 ?? ?? ?? ?? F3 0F 11 54 24 60", 0x12B3823u, 0x12B2F03u },
    /* Unrestricted placement (snaphak-re editor-fixes-13954591 R-025): the two
     * tests that refuse an element with no accepted surface unless its
     * placementFlags allow air. Unique on both build 13954591 executables. */
    { "PlaceGateCommit", /* Place/commit group test: CMP dword [r12+rdx+8],0; JNZ +0x27 past TEST byte [def+0x1d0],0x30; JZ refuse. */
      "41 83 7C 14 08 00 75 27 49 8B 46 28 49 63 0C 14 48 8B 00 48 8B 80 A0 06 00 00 48 8B 0C C8 48 8B 41 08 F6 80 D0 01 00 00 30 0F 84", 0x121951Cu, 0x121896Cu },
    { "PlaceGateElement", /* Per-element ghost test: blocked +0x30, module +4 == -1, then CMP [r8+8],0; JNZ +0x2e past TEST [def+0x1d0],0x30. */
      "40 55 57 48 81 EC D8 00 00 00 41 80 78 30 00 48 8B EA 48 8B F9 75 30 41 83 78 04 FF 74 29 41 83 78 08 00 75 2E 48 8B 41 28 49 63 08 48 8B 00 48 8B 80 A0 06 00 00 48 8B 0C C8 48 8B 41 08 F6 80 D0 01 00 00 30 75 0C 32 C0", 0x12197B0u, 0x1218C00u },
    /* Boot pacing (snaphak-re boot-time-13954591 R-001, R-002). The bootstrap
     * matches these at their recorded RVAs before the full scan, so each
     * pattern must stay unique at that RVA on both executables. */
    { "BootVideoLock", /* Load-screen video draw: LEA RDI,[bink lock]; enter it; CMP [first video handle],0; JZ. */
      "48 8D 3D ?? ?? ?? ?? 48 89 7C 24 58 B2 01 48 8B CF E8 ?? ?? ?? ?? C6 44 24 60 01 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 48 8B 0B E8", 0x1901E14u, 0x18FAC93u },
    { "BootVideoSleep", /* The same draw: both handles drawn, then MOV ECX,10; CALL Sleep while the lock is held; CMP [second handle],0. */
      "80 3D ?? ?? ?? ?? 00 74 2A 44 8B CE 44 8B C5 48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 44 8B CE 44 8B C5 48 8D 15 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? B9 0A 00 00 00 E8 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? 00", 0x1901E63u, 0x18FAD41u },
    { "BootReadPoll", /* UncachedBackgroundRead completion poll: done byte [rsp+0x70], abort [rsi+0x10], MOV ECX,1; CALL Sleep. */
      "0F B6 44 24 70 84 C0 75 1B 0F B6 46 10 84 C0 75 13 B9 01 00 00 00 E8 ?? ?? ?? ?? 0F B6 44 24 70 84 C0 74 E5", 0x1938EA8u, 0x192D818u },
    /* Module prefabs (snaphak-re editor-feature-requests R-002, R-003). Every
     * entry verified on both build 13954591 executables. */
    { "SnapMapCtor", /* idSnapMap in-place ctor (0x778 bytes): writes version 0x6f at +0. VK 1245b60, GL 1245190. */
      "4C 8B DC 49 89 4B 08 56 57 41 56 48 83 EC 40 49 C7 43 C8 FE FF FF FF 49 89 5B 18 49 89 6B 20 48 8B F9 C7 01 6F 00 00 00", 0x1245B60u, 0x1245190u },
    { "SnapMapDtor", /* idSnapMap dtor, paired with SnapMapCtor by EditorMapToJson. VK fcdcf0, GL fcd230. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 89 74 24 48 48 8B F9 0F B6 81 6B 07 00 00 33 F6 84 C0 74", 0xFCDCF0u, 0xFCD230u },
    { "SnapMapValidate", /* int(idSnapMap*, res98, res88): 0 = valid. RepairAndMigrate, duplicate-id and ref checks. VK 12488e0, GL 1247f10. */
      "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 E0 E1 FF FF B8 20 1F 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 44 24 78 FE FF", 0x12488E0u, 0x1247F10u },
    { "SnapMapEditCtor", /* idSnapMapEdit(out 0xdd0, settings 0x550, idSnapMap*, res98, res88); ends in GridReconnectPortals. VK 11f1520, GL 11f0850. */
      "48 8B C4 48 89 48 08 57 41 54 41 55 41 56 41 57 48 83 EC 50 48 C7 40 A8 FE FF FF FF 48 89 58 10 48 89 68 18 48 89 70 20 49", 0x11F1520u, 0x11F0850u },
    { "SnapMapEditDtor", /* idSnapMapEdit dtor (the undo push frees with it). VK 114a9a0, GL 11499f0. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 8B D9 48 81 C1 B8 0D 00 00 E8 ?? ?? ?? ?? 90 0F B6 83 AB", 0x114A9A0u, 0x11499F0u },
    { "ModuleRedoAction", /* Module Mode redo handler: CALL Redo(editor) at +0x13, then event 0x29 and mode+0x1070 = 0x101. VK 126e4b0, GL 126daa0. */
      "48 89 5C 24 08 57 48 83 EC 20 49 8B C8 49 8B F8 48 8B DA E8 ?? ?? ?? ?? 84 C0 74 21 45 33 C0 C7 44 24 48 29 00 00 00 48", 0x126E4B0u, 0x126DAA0u },
    { "ModuleSelect", /* char(selection, module index): the Module Mode select (merge into editData+0x68). VK 11fd980, GL 11fcc90. */
      "4C 8B DC 56 57 41 56 48 83 EC 40 49 C7 43 C8 FE FF FF FF 49 89 5B 10 49 89 6B 18 4C 8B F1 33 FF C7 44 24 38 00 00 05 00", 0x11FD980u, 0x11FCC90u },
    { "EditorCursorPos", /* int2 *(editor+0x20, int2 *out): the editor cursor. VK 118dc70, GL 118cdd0. */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 0D ?? ?? ?? ?? 48 8B FA 48 8B 01 FF 90 D8 01 00 00 48 8B 0D ?? ?? ?? ?? 8B 58 10 48", 0x118DC70u, 0x118CDD0u },
    { "ModuleCursorFloor", /* float3 *(mode, float3 *out, editor, const int2 *cursor): the Blueprint floor point under the cursor. VK 1257640, GL 1256b40. */
      "40 53 48 83 EC 20 44 8B 51 0C 48 8B DA 41 83 EA 01 74 2B 41 83 FA 01 74 11 33 C9 48 8B 01 FF 50 38 48 8B C3 48 83 C4 20", 0x1257640u, 0x1256B40u },
    { "EditorBudgetUpdate", /* void(editor): recompute the editor budget; writes the limits-reached byte editor+0x236d0. VK 118b070, GL 118a1d0. */
      "40 55 B8 50 41 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 F0 40 00 00 83 3D ?? ?? ?? ?? 00", 0x118B070u, 0x118A1D0u },
    { "SnapPaletteBuild",   /* void(palette [rcx], progress [rdx]): rebuild the editor entity palette at
                             * editor+0x20660 from every snapEditorEntityDef decl. Called once, from editor
                             * initialization. The tail anchors on its first writes: selected index +0x08 = 0
                             * and end pointer +0x28 = +0x10. The first 18 bytes are whole and register-only.
                             * VK 0x1411aa710, GL 0x1411a9820. */
      "48 8B C4 56 57 41 54 41 56 41 57 48 81 EC 70 07 00 00 48 C7 44 24 50 FE FF FF FF "
      "48 89 58 18 48 89 68 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 60 07 00 00 "
      "4C 8B F2 4C 8B F9 33 DB 89 59 08 48 8B 41 10 48 89 41 28", 0x11AA710u, 0x11A9820u },
    /* Published-map and offline-play gates (modpublished.c; snaphak-re
     * mod-system-reinstatement lane N1a section 5). Unique on both build
     * 13954591 executables; steal windows audited by detour_windows_test. */
    { "PublishedOnSnapPlayDone",   /* idMenuManager_Shell::OnSnapPlay read completion (manager, request,
                                    * metadata, map): ClearDialog(0x63), progress hide, PushHistorySetCache,
                                    * PublishedMapLobbyLaunch(manager, 0). Steal 18: 11 bytes of pushes and
                                    * sub, then the cookie load MOV RAX,[RIP+d32] relocated. */
      "40 53 56 57 48 81 EC E0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 D0 00 00 00",
      0xFA4A80u, 0xFA3FB0u },
    { "PublishedBrowserPlayDone",  /* browser published-play read completion (browser, request, metadata,
                                    * map); manager at browser+0x10; lobby launch or offline fallback. Steal 20. */
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 49 8B F1 48 8B 49 10 49 8B E8 E8 ?? ?? ?? ??",
      0xFD1E30u, 0xFD1320u },
    { "PublishedMapCacheReady",    /* joiner readiness after the host map's read (manager, request, metadata,
                                    * map); never reads request. Steal 15 with the stack-probe call at +7. */
      "40 57 B8 80 17 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 C7 44 24 20 FE FF FF FF 48 89 9C 24 98 17 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 70 17 00 00 49 8B F9 49 8B D8 48 8B 89 38 0A 00 00 "
      "48 85 C9 74 ?? 48 8B 01 33 D2 FF 50 30 48 8B 0D ?? ?? ?? ?? 48 8B 01", 0xFA1D60u, 0xFA1290u },
    { "CacheMapForLobby",          /* char(manager, id idStr*, revision idStr*): 1 when the in-memory cache
                                    * holds the map (no read), else the read toward CacheReady and 0. N1a's
                                    * 28-byte head also matches VK fab300 / GL faa830, so this runs through
                                    * the cache query. Steal 20. */
      "40 55 56 57 48 81 EC 80 00 00 00 48 C7 44 24 38 FE FF FF FF 48 89 9C 24 B8 00 00 00 "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 70 49 8B E8 48 8B F2 48 8B F9 E8 ?? ?? ?? ?? "
      "48 8B 10 48 8B C8 FF 52 38 4C 8B 08 4C 8B C5 48 8B D6 48 8B C8 41 FF 91 B0 01 00 00 "
      "84 C0 74 07 B0 01 E9 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ??", 0xF9A170u, 0xF99620u },
    { "LaunchLocalSnapMapForOfflinePlay", /* (manager, localSavedModel*, idSnapMap*): My Maps play, offline
                                    * only; its caller (VK fd2080) parses into a stack map. Steal 22. */
      "40 55 53 56 57 41 56 48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 48 C7 44 24 28 FE FF FF FF",
      0xF9FCF0u, 0xF9F220u },
    { "PublishedMapRead",          /* (ctx, id, revision, ok, fail): offline parses inline at +0x125, so a
                                    * DeserializeFromJson returning to +0x12A is the offline published read.
                                    * Not hooked. */
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F8 F2 FF FF 48 81 EC 08 0E 00 00 48 C7 45 B0 FE FF FF FF "
      "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 F8 0C 00 00 49 8B D9 4D 8B F0 48 8B F2 4C 8B E1 4C 8B BD 70 0D 00 00 "
      "48 8B 0D ?? ?? ?? ?? 48 8B 01 33 D2 FF 50 48", 0x11C4AA0u, 0x11C3E70u },
    { "PublishedMapSelectionConstruct", /* allocates 0x590 at +0x45 and copy-constructs the metadata at +0x69;
                                    * the gates call that copy constructor. Not hooked. */
      "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 48 48 89 74 24 50 49 8B F0 48 8B D9 "
      "48 C7 01 00 00 00 00 89 51 08 C7 41 0C 01 00 00 00 48 8D 05 ?? ?? ?? ?? 48 89 41 10 FF 05 ?? ?? ?? ?? "
      "B9 90 05 00 00 E8 ?? ?? ?? ?? 48 8B F8 48 89 44 24 58", 0xFD5FA0u, 0xFD5490u },
    { "PublishedMapMetadataDestroy", /* idPublishedMapModel destructor, paired with the copy above. Not hooked. */
      "40 57 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 40 48 89 74 24 48 48 8B F9 0F B6 81 83 05 00 00 "
      "33 F6 84 C0 74 ?? 2C 03 3C 01 77 ?? 48 8B 89 70 05 00 00 48 85 C9 74 ?? 8B 97 7C 05 00 00 E8 ?? ?? ?? ?? "
      "90 48 89 B7 70 05 00 00 89 B7 7C 05 00 00 89 B7 78 05 00 00", 0xF969A0u, 0xF95ED0u },
    /* Publish-time mod upload, claims and the copy gate (modpublish.c; snaphak-re
     * mod-system-reinstatement lane N4). Unique on both build 13954591
     * executables; steal windows audited by detour_windows_test. */
    { "PublishAfterSave", /* the Save state's publish callback (editorLocal): vtbl+0x160(4), then TryCloudSave; nothing is sent before it. Steal 15. */
      "40 57 48 83 EC 60 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 70 48 8B F9 48 8B 01 BA 04 00 00 00 FF 90 60 01 00 00"
      , 0x119A8C0u, 0x1199B20u },
    { "PublishFail", /* (editorLocal): the publish-failed toast on editor+0x21088, then EditorBusyClear. Its twin VK 119a930 (the success toast) differs only after the RET, so the pattern ends on the padding and the next thunk's E9. Not hooked. */
      "40 53 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 00 00 00 48 8B D9 48 "
      "83 B9 88 10 02 00 00 74 50 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 58 E8 ?? ?? ?? ?? 90 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 28 E8 "
      "?? ?? ?? ?? 90 4C 8D 44 24 58 48 8D 54 24 28 48 8B 8B 88 10 02 00 E8 ?? ?? ?? ?? 90 48 8D 4C 24 28 E8 ?? ?? ?? ?? 90 48 "
      "8D 4C 24 58 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B 8C 24 88 00 00 00 48 33 CC E8 ?? ?? ?? ?? 48 81 C4 90 00 00 00 "
      "5B C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC E9"
      , 0x119ABE0u, 0x1199E40u },
    { "PublishFeedPost", /* (idSnapMapEdit*, const idPublishedMapModel&): both publish success paths; item id idStr at model+0, revision at +0x30. Steal 15. */
      "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F9 48 8B F2 B9 18 00 00 00 E8 ?? ?? ?? ?? 48 89 44 24 30 48 8B D8 48 "
      "85 C0 74 1B 48 8D 05 ?? ?? ?? ?? 48 89 7B 08 48 89 03 48 8D 05 ?? ?? ?? ?? 48 89 43 10 EB 02 33 DB B9 18 00 00 00 E8"
      , 0x11F91D0u, 0x11F83F0u },
    { "MapInfoUnpublishDone", /* MapInfo screen unpublish success (screen, ctx, const idStr &itemId), bound by the confirm at VK 1090ea0 / GL 108ffb0. Steal 14. */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 41 18 48 8B D9 48 85 C0 75 09 48 8B 49 20 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 "
      "8B F8 48 85 C0 74 3F"
      , 0x1092790u, 0x10918A0u },
    { "MapInfoCopyToMyMaps", /* MapInfo screen copy (branch) entry (screen): record +0x300 of kind 1, model at record+0x18 -> +8, then CopyRequestWrap. Steal 19. */
      "48 8B C4 57 48 81 EC 90 00 00 00 48 C7 40 B0 FE FF FF FF 48 89 58 10 48 89 70 18 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 "
      "24 80 00 00 00 48 8B D9 48 8B 41 18 48 85 C0 75 09 48 8B 49 20 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 "
      "0F 84 ?? ?? ?? ?? 8B 93 00 03 00 00 48 8B C8 E8 ?? ?? ?? ?? 48 8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 83 78 0C 01"
      , 0x1090A10u, 0x108FB20u },
    { "BrowserCopyToMyMaps", /* browser copy (branch) entry (browser, flag): record kind 5, entry model +0x28 -> +8, then CopyRequestWrap. Called after the wait widget is shown. Steal 16. */
      "48 89 6C 24 20 57 48 83 EC 40 0F B6 EA 48 8B F9 8B 51 08 48 8B 49 10 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 83 78 0C "
      "05"
      , 0xFCE5E0u, 0xFCDAE0u },
    /* Cooked movies and weapon ammo displays (modswf.c, modhud.c; snaphak-re
     * mod-system-reinstatement lane P3). Unique on both build 13954591
     * executables. Neither is a detour: modswf swaps the idFileResource
     * vtable slot that holds FileResourceLoad, modhud swaps one call's rel32. */
    { "FileResourceLoad", /* idFileResource Load (vtable +0x40): state < 2 opens through FS +0x148 (named idFile_Memory,
                           * open-into helper), else the resource manager's +0x68. modswf decodes the state getter,
                           * operator new, named ctor and open-into from this body. No steal. */
      "40 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 50 48 89 6C 24 58 48 89 74 24 68 48 8B EA 48 8B D9 "
      "E8 ?? ?? ?? ?? 83 F8 02 7C 7B 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B 53 08 FF 50 68 48 8B F8 48 8B 4B 38 48 3B C8 "
      "74 14 48 85 C9 74 0B 4C 8B 01 BA 01 00 00 00 41 FF 10 48 89 7B 38 48 83 7B 38 00 75 3F B9 80 01 00 00 "
      "E8 ?? ?? ?? ?? 48 89 44 24 60 48 85 C0 74 0D 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8 EB 02 33 FF 48 8B 4B 38 48 3B CF "
      "74 13 48 85 C9 74 0A 48 8B 01 BA 01 00 00 00 FF 10 48 89 7B 38 33 C0 EB 5B B9 80 01 00 00 E8 ?? ?? ?? ?? "
      "48 89 44 24 60 48 85 C0 74 11 48 8B 53 08 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8", 0x172B3B0u, 0x172C040u },
    { "WeaponHudModeCall", /* WeaponInfo::Update: CALL WeaponHudGameMode; TEST AL,AL; JZ; then the widget's weapon decl
                            * at RBX+0x1F8 and its +0x641 flag. The beta.19 pattern, unchanged on 13954591; modhud
                            * swaps the call's aligned rel32 (no steal). */
      "E8 ?? ?? ?? ?? 84 C0 74 45 48 8B 83 F8 01 00 00 48 85 C0 74 09 80 B8 41 06 00 00 00 75 30", 0xC73F47u, 0xC739C7u },
    { "WeaponHudGameMode", /* bool(game*): game+0x45338 -> +0x18 == 1; modhud checks the decoded call lands here. */
      "48 8B 81 38 53 04 00 83 78 18 01 0F 94 C0 C3", 0x398C50u, 0x398560u },
    /* Publish search tag and delete-time claim release (modpublish.c; snaphak-re
     * mod-system-reinstatement lane P4). Unique on both build 13954591
     * executables; evidence logs/shared-ghidra/mod-system-p4/. */
    { "MapInfoDeleteDone", /* MapInfo screen My Maps delete success (screen, ctx, const idStr &localId, const idStr &cloudItemId), bound by the delete confirm at VK 1090fc0 / GL 10900d0. Its twin MapInfoUnpublishDone differs at +0xF (mov rdi vs rbx). Steal 14. */
      "48 89 5C 24 08 57 48 83 EC 20 48 8B 41 18 48 8B F9 48 85 C0 75 09 48 8B 49 20 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
      "48 8B D8 48 85 C0 74 3F 8B 97 00 03 00 00 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 13 48 8B 08 48 85 C9 74 0B 48 8B 01 BA "
      "03 00 00 00 FF 50 08"
      , 0x1092810u, 0x1091920u },
    { "PublishStart", /* (editorLocal, overwrite): adds or removes the reserved map:dev / map:dlc1 tags on the edit's tag list (edit+0x998, idList<idStr>), arms PublishAfterSave. Not hooked: its list grow (+0xB3) and idStr assign (+0xD5) calls are bound to append the search tag. */
      "4C 8B DC 57 41 56 41 57 48 81 EC 90 00 00 00 48 C7 44 24 20 FE FF FF FF 49 89 5B 10 49 89 6B 18 49 89 73 20 48 8B 05 "
      "?? ?? ?? ??"
      , 0x119AD00u, 0x1199F60u },
    { "PublishedPrevQuery", /* (ctx, const char *parentItemId, const char *cursor, ok, fail): the maps_pub_prev_pub search for the items the player published from one cloud-saved item; ok gets idList<idPublishedMapModel> (stride 0x588, id idStr at +0). Not hooked: called by the delete check. */
      "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C8 FE FF FF 48 81 EC 38 02 00 00 48 C7 45 E0 FE FF FF FF 48 8B 05 "
      "?? ?? ?? ?? 48 33 C4 48 89 85 20 01 00 00 4D 8B E1 49 8B F0 4C 8B EA 48 8B D9 48 89 4D D0 4C 8B B5 A0 01 00 00"
      , 0x11C4160u, 0x11C3530u },
    { NULL, NULL, 0 }   /* terminator */
};
