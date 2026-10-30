/* Audit of every fixed-length detour window against a supported executable.
 * For each site the backend detours, resolve its target the way the backend
 * does (signature, signature + offset, the target of a call inside a
 * signature, or the pinned frame RVA), decode the prologue with x64_len, and
 * require: the fixed length is an instruction boundary; and, when the
 * trampoline replays the window, no RIP-relative operand or relative branch
 * lies inside it except the one instruction the hook relocates.
 * A build change that moves a boundary or brings a relative operand into a
 * window fails here. Run via run-tests.ps1 with -Doom / -DoomAlt. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x64_len.h"
#include "signatures.h"
#include "host_image.h"
#include "../src/fault_shield/engine_layout.h"

/* RVA_FRAME is an SH_RVA pair; evaluate it for the image under test rather
 * than for this test process. */
static const uint8_t *g_image;
#undef SH_RVA
#define SH_RVA(vk, gl) sh_host_image_rva(g_image, (uint32_t)(vk), (uint32_t)(gl))

enum site_kind { SITE_SIG, SITE_CALL_IN_SIG, SITE_FRAME_RVA, SITE_SLOT, SITE_ISBRANCH, SITE_DECODED };
enum site_policy {
    REPLAYS,          /* the trampoline runs: the window must be position independent */
    NEVER_REPLAYED,   /* the detour replaces the function; the trampoline never runs */
    RELOCATES_AT,     /* one RIP-relative LEA, 64-bit load or E8 at reloc is rewritten by the hook */
    STUB_PADDING      /* a never-replayed stub: its ret at reloc is followed by int3 padding */
};

typedef struct site {
    const char *owner;      /* backend source file */
    const char *name;       /* signature name (or a description for derived sites) */
    int kind;
    size_t offset;          /* SITE_SIG: added to the signature; SITE_CALL_IN_SIG: offset of the E8 */
    size_t stolen;          /* the fixed byte count the backend passes (0 for SITE_DECODED) */
    int policy;
    size_t reloc;           /* RELOCATES_AT: offset of the relocated instruction */
} site;

static const site SITES[] = {
    { "apply_engine.c",       "EditorCommitEdit",           SITE_SIG, 0, 16, REPLAYS, 0 },
    { "apply_engine.c",       "GridApplyEntityTree",        SITE_SIG, 0, 18, REPLAYS, 0 },
    { "apply_engine.c",       "PrefabGrabFrame",            SITE_SIG, 0, 19, REPLAYS, 0 },
    { "commands.c",           "RenderLogStub",              SITE_SIG, 0, 14, STUB_PADDING, 6 },
    { "commands.c",           "AddCommand",                 SITE_SIG, 0, 15, REPLAYS, 0 },
    { "editor_frame.c",       "EditorFrame",                SITE_SIG, 0, 25, REPLAYS, 0 },
    { "engine_dialog.c",      "AddDialogWrapper",           SITE_SIG, 0, 15, REPLAYS, 0 },
    { "engine_dialog.c",      "DialogAction",               SITE_SIG, 0, 16, REPLAYS, 0 },
    { "grid_room_editor.c",   "GridPropertyChanged",        SITE_SIG, 9, 17, REPLAYS, 0 },
    { "grid_room_editor.c",   "GridModuleProperties",       SITE_SIG, 0, 16, REPLAYS, 0 },
    { "grid_room_editor.c",   "GridAddVec3",                SITE_SIG, 0, 18, REPLAYS, 0 },
    { "grid_room_editor.c",   "GridBlueprintUpdate",        SITE_SIG, 0, 15, REPLAYS, 0 },
    { "grid_room_editor.c",   "GridBlueprintHelp",          SITE_SIG, 0, 15, REPLAYS, 0 },
    { "grid_room_native.c",   "GridFindModule",             SITE_SIG, 0, 16, REPLAYS, 0 },
    { "grid_room_native.c",   "GridHasModule",              SITE_SIG, 0, 16, NEVER_REPLAYED, 0 },
    { "grid_room_native.c",   "GridBuildModuleCollision",   SITE_SIG, 0, 21, REPLAYS, 0 },
    { "grid_room_native.c",   "GridContainingModule",       SITE_SIG, 0, 19, REPLAYS, 0 },
    { "grid_room_snap.c",     "GridPortalSnap",             SITE_SIG, 0, 15, REPLAYS, 0 },
    { "grid_room_snap.c",     "GridPortalAlign",            SITE_SIG, 0, 15, REPLAYS, 0 },
    { "grid_room_snap.c",     "GridQuantizeOrigin",         SITE_SIG, 0, 15, REPLAYS, 0 },
    { "grid_room_snap.c",     "GridConfirmOrigins",         SITE_SIG, 0, 17, REPLAYS, 0 },
    { "settings_tab.c",       "RenderSettingsEnter",        SITE_SIG, 0, 15, REPLAYS, 0 },
    { "settings_tab.c",       "RenderSettingsExit",         SITE_SIG, 0, 16, REPLAYS, 0 },
    { "settings_tab.c",       "RenderSettingsPopulate",     SITE_SIG, 0, 20, REPLAYS, 0 },
    { "settings_tab.c",       "RenderSettingsApply",        SITE_SIG, 0, 16, REPLAYS, 0 },
    { "settings_tab.c",       "RenderSettingsReset",        SITE_SIG, 0, 20, REPLAYS, 0 },
    { "settings_tab.c",       "RenderSettingsDirtyCall",    SITE_CALL_IN_SIG, 0, 16, REPLAYS, 0 },
    { "settings_tab.c",       "SettingsCategoryCycle",      SITE_SIG, 0, 15, NEVER_REPLAYED, 0 },
    { "settings_tab.c",       "SettingsCategorySelect",     SITE_SIG, 0, 16, NEVER_REPLAYED, 0 },
    { "nav_menu.c",           "StartMenuBind",              SITE_SIG, 0, 18, REPLAYS, 0 },
    { "nav_menu.c",           "StartMenuAction",            SITE_SIG, 0, 19, REPLAYS, 0 },
    { "nav_menu.c",           "StartMenuThink",             SITE_SIG, 0, 19, REPLAYS, 0 },
    { "nav_menu.c",           "StartMenuEnd",               SITE_SIG, 0, 16, REPLAYS, 0 },
    { "nav_play.c",           "NavSearchHeapPush",          SITE_SIG, 0, 15, REPLAYS, 0 },
    { "nav_play.c",           "SnapMapEditToSnapBuild",     SITE_SIG, 0, 15, REPLAYS, 0 },
    { "nav_preview.c",        "NavRenderStage",             SITE_SIG, 0, 16, REPLAYS, 0 },
    { "nav_preview.c",        "NavPostProcessSlot",         SITE_SLOT, 0, 0, REPLAYS, 0 },
    { "rawmap.c",             "IsBranchMap (pattern scan)", SITE_ISBRANCH, 0, 14, REPLAYS, 0 },
    { "rawmap.c",             "DeserializeFromJson",        SITE_SIG, 0, 16, REPLAYS, 0 },
    { "rawmap.c",             "SerializeToJson",            SITE_SIG, 0, 20, REPLAYS, 0 },
    { "strids.c",             "StridsSortBody",             SITE_SIG, 0, 16, REPLAYS, 0 },
    { "swf_textedit.c",       "SwfTextOnKeyCall",           SITE_SIG, 0, 15, REPLAYS, 0 },
    { "timeline_tile.c",      "SnapPaletteBuild",           SITE_SIG, 0, 18, REPLAYS, 0 },
    { "modserve_engine.c",    "DeclResourceProbe",          SITE_SIG, 0, 21, REPLAYS, 0 },
    { "modpublished.c",       "PublishedOnSnapPlayDone",    SITE_SIG, 0, 18, RELOCATES_AT, 11 },
    { "modpublished.c",       "PublishedBrowserPlayDone",   SITE_SIG, 0, 20, REPLAYS, 0 },
    { "modpublished.c",       "PublishedMapCacheReady",     SITE_SIG, 0, 15, RELOCATES_AT, 7 },
    { "modpublished.c",       "CacheMapForLobby",           SITE_SIG, 0, 20, REPLAYS, 0 },
    { "modpublished.c",       "LaunchLocalSnapMapForOfflinePlay", SITE_SIG, 0, 22, REPLAYS, 0 },
    { "modpublish.c",         "PublishAfterSave",           SITE_SIG, 0, 15, REPLAYS, 0 },
    { "modpublish.c",         "PublishFeedPost",            SITE_SIG, 0, 15, REPLAYS, 0 },
    { "modpublish.c",         "MapInfoUnpublishDone",       SITE_SIG, 0, 14, REPLAYS, 0 },
    { "modpublish.c",         "MapInfoCopyToMyMaps",        SITE_SIG, 0, 19, REPLAYS, 0 },
    { "modpublish.c",         "BrowserCopyToMyMaps",        SITE_SIG, 0, 16, REPLAYS, 0 },
    { "wiring_cleandirect.c", "ConnectOutputCreator",       SITE_SIG, 0, 15, REPLAYS, 0 },
    { "wiring_cleandirect.c", "WireConnectCreator1",        SITE_SIG, 0, 15, REPLAYS, 0 },
    { "fault_shield/mapload_guards.c", "EventLink",         SITE_SIG, 0, 15, REPLAYS, 0 },
    { "fault_shield/mapload_guards.c", "InteractableSpawn", SITE_SIG, 0, 19, REPLAYS, 0 },
    { "fault_shield/recovery.c", "Frame (RVA_FRAME)",       SITE_FRAME_RVA, 0, 15, REPLAYS, 0 },
    /* Decoded at install since 88f7a5b; listed so the table is complete. */
    { "algo.c",               "AlgoMatMul",                 SITE_DECODED, 0, 0, NEVER_REPLAYED, 0 },
    { "algo.c",               "AlgoInverse",                SITE_DECODED, 0, 0, NEVER_REPLAYED, 0 },
    { "algo.c",               "AlgoPackRGBA",               SITE_DECODED, 0, 0, NEVER_REPLAYED, 0 },
    { "algo.c",               "AlgoCurveEval",              SITE_DECODED, 0, 0, REPLAYS, 0 },
};

/* nav_preview.c accepts exactly these heads for the post-process function it
 * reads through NavPostProcessSlot at runtime; it steals the whole head. */
static const uint8_t NAV_POST_VK_HEAD[] = {
    0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x48,0x83,0xec,0x60,
    0x65,0x48,0x8b,0x04,0x25,0x58,0,0,0 };
static const uint8_t NAV_POST_GL_HEAD[] = {
    0x40,0x53,0x57,0x48,0x83,0xec,0x68,0x65,0x48,0x8b,0x04,0x25,0x58,0,0,0 };

/* rawmap.c's IsBranchMap scan: the first 16 bytes of its 39-byte pattern. */
static const uint8_t ISBRANCH_HEAD[] = {
    0x48,0x83,0xEC,0x28, 0x48,0x8B,0x89,0xC8,0x04,0x02,0x00, 0x48,0x85,0xC9, 0x74,0x10 };

static int g_fail = 0;

static uint8_t *map_pe_by_rva(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long fsz;
    uint8_t *file, *img;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
    file = (uint8_t *)malloc(fsz);
    if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); return NULL; }
    fclose(f);
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS *)(file + dos->e_lfanew);
    img = (uint8_t *)calloc(1, nt->OptionalHeader.SizeOfImage + 64);
    if (!img) { free(file); return NULL; }
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = sec[i].VirtualAddress, rs = sec[i].SizeOfRawData, po = sec[i].PointerToRawData;
        if (rs && va + rs <= nt->OptionalHeader.SizeOfImage && po + rs <= (uint32_t)fsz)
            memcpy(img + va, file + po, rs);
    }
    *size = nt->OptionalHeader.SizeOfImage;
    free(file);
    return img;
}

/* Walk instructions up to at least `stolen` bytes. Returns the first boundary
 * at or past stolen (0 if undecodable) and lists flagged instruction offsets. */
static size_t walk(const uint8_t *code, size_t stolen, char *notes, size_t cap,
                   size_t *bad_offsets, unsigned *bad_flags, size_t *nbad)
{
    size_t at = 0;
    *nbad = 0;
    notes[0] = '\0';
    while (at < stolen) {
        unsigned f = 0;
        size_t n = sh_x64_insn_len(code + at, 48 - at, &f);
        if (!n) return 0;
        if (f & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL)) {
            size_t used = strlen(notes);
            _snprintf_s(notes + used, cap - used, _TRUNCATE, "%s%s@+%u", used ? "," : "",
                        (f & SH_X64_RIP_RELATIVE) ? "rip" : "rel", (unsigned)at);
            if (*nbad < 8) { bad_offsets[*nbad] = at; bad_flags[*nbad] = f; (*nbad)++; }
        }
        if ((f & SH_X64_LEAVES) && at + n < stolen) return 0;
        at += n;
    }
    return at;
}

static const sig_result *by_name(const sig_result *r, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (r[i].name && !strcmp(r[i].name, name)) return &r[i];
    return NULL;
}

static void report(const char *label, const char *owner, const char *name, uint32_t rva,
                   size_t fixed, size_t boundary, const char *notes, const char *verdict, int ok)
{
    printf("%s %-6s | %-30s | %-28s | 0x%07x | %2u | %2u | %-14s | %s\n", ok ? "OK  " : "FAIL",
           label, owner, name, rva, (unsigned)fixed, (unsigned)boundary, notes[0] ? notes : "-", verdict);
    if (!ok) g_fail = 1;
}

static void audit_bytes(const char *label, const site *s, const uint8_t *code, uint32_t rva, size_t fixed)
{
    char notes[96];
    size_t bad_offsets[8], nbad = 0, boundary;
    unsigned bad_flags[8];
    const char *verdict = "boundary, position independent";
    int ok = 1;
    if (s->policy == STUB_PADDING) {
        /* The stub's instructions end at reloc with a ret; the rest must be int3. */
        size_t k, at = 0;
        unsigned f = 0;
        notes[0] = '\0';
        while (at < s->reloc) {
            size_t n = sh_x64_insn_len(code + at, 48 - at, &f);
            if (!n) break;
            at += n;
        }
        for (k = at; k < fixed && code[k] == 0xCC; k++) {}
        ok = at == s->reloc && (f & SH_X64_LEAVES) && k == fixed;
        report(label, s->owner, s->name, rva, fixed, ok ? fixed : 0, "",
               ok ? "stub + int3 padding, trampoline never runs" : "STUB layout changed", ok);
        return;
    }
    boundary = walk(code, fixed, notes, sizeof notes, bad_offsets, bad_flags, &nbad);
    if (!boundary) { verdict = "UNDECODABLE window"; ok = 0; }
    else if (boundary != fixed) { verdict = "MID-INSTRUCTION cut"; ok = 0; }
    else if (nbad) {
        if (s->policy == NEVER_REPLAYED) verdict = "boundary; relative operand, trampoline never runs";
        else if (s->policy == RELOCATES_AT && nbad == 1 && bad_offsets[0] == s->reloc)
            verdict = "boundary; the one relative instruction is relocated";
        else { verdict = "RELATIVE operand in a replayed window"; ok = 0; }
    }
    report(label, s->owner, s->name, rva, fixed, boundary, notes, verdict, ok);
}

static void check_image(const char *path)
{
    wchar_t wide[MAX_PATH];
    size_t image_size = 0;
    uint8_t *base = map_pe_by_rva(path, &image_size);
    const sh_build_identity *identity;
    sig_result results[SIG_RESULTS_MAX];
    size_t db = sig_db_count();
    const char *label;
    if (!base) { printf("FAIL cannot map %s\n", path); g_fail = 1; return; }
    g_image = base;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
    identity = sh_host_test_bind_image(base, wide);
    if (!identity) { printf("FAIL %s is not a supported executable\n", path); g_fail = 1; return; }
    label = identity->vulkan ? "vk" : "gl";
    printf("image %s: %s\n", path, identity->label);
    if (db > SIG_RESULTS_MAX) db = SIG_RESULTS_MAX;
    (void)sig_resolve_all(base, results, SIG_RESULTS_MAX);

    for (size_t i = 0; i < sizeof SITES / sizeof SITES[0]; i++) {
        const site *s = &SITES[i];
        const uint8_t *code = NULL;
        const sig_result *r = NULL;
        if (s->kind == SITE_SIG || s->kind == SITE_CALL_IN_SIG || s->kind == SITE_SLOT ||
            s->kind == SITE_DECODED) {
            r = by_name(results, db, s->name);
            if (!r || r->status != SIG_OK || !r->addr) {
                report(label, s->owner, s->name, 0, s->stolen, 0, "", "SIGNATURE unresolved", 0);
                continue;
            }
            code = (const uint8_t *)r->addr + s->offset;
        }
        if (s->kind == SITE_CALL_IN_SIG) {
            int32_t rel;
            if (code[0] != 0xE8) {
                report(label, s->owner, s->name, 0, s->stolen, 0, "", "no call at the recorded offset", 0);
                continue;
            }
            memcpy(&rel, code + 1, 4);
            code = code + 5 + rel;
        } else if (s->kind == SITE_FRAME_RVA) {
            code = base + RVA_FRAME;
        } else if (s->kind == SITE_ISBRANCH) {
            /* Every scan candidate carries the same fixed head. */
            const uint8_t *hit = NULL;
            for (size_t k = 0x1000; k + sizeof ISBRANCH_HEAD < image_size && !hit; k++)
                if (base[k] == 0x48 && !memcmp(base + k, ISBRANCH_HEAD, sizeof ISBRANCH_HEAD)) hit = base + k;
            if (!hit) { report(label, s->owner, s->name, 0, s->stolen, 0, "", "pattern not found", 0); continue; }
            code = hit;
        } else if (s->kind == SITE_SLOT) {
            /* mov rdx,[rip+disp] names a data slot; the function pointer in it
             * is set at run time, so audit the two heads nav_preview accepts. */
            site head = *s;
            head.name = "NavPostProcess (VK head)";
            audit_bytes(label, &head, NAV_POST_VK_HEAD, 0, sizeof NAV_POST_VK_HEAD);
            head.name = "NavPostProcess (GL head)";
            audit_bytes(label, &head, NAV_POST_GL_HEAD, 0, sizeof NAV_POST_GL_HEAD);
            continue;
        } else if (s->kind == SITE_DECODED) {
            unsigned flags = 0;
            size_t boundary = sh_x64_stolen_boundary(code, 32, 14, 32, &flags);
            int ok = boundary != 0 &&
                     (s->policy == NEVER_REPLAYED || !(flags & (SH_X64_RIP_RELATIVE | SH_X64_BRANCH_REL)));
            report(label, s->owner, s->name, (uint32_t)(code - base), 14, boundary,
                   (flags & SH_X64_RIP_RELATIVE) ? "rip" : "",
                   ok ? "decoded at install" : "DECODE refused", ok);
            continue;
        }
        audit_bytes(label, s, code, (uint32_t)(code - base), s->stolen);
    }
}

/* Source census: every file that prepares a detour, and how many preparation
 * calls it contains. A new or removed call fails here until SITES and this
 * table are updated together, so no detour escapes the window audit.
 * hook.c and patch.c hold the definitions; smoke.c detours its own scratch code. */
static const struct { const char *file; int calls; } CENSUS[] = {
    { "backend\\algo.c", 1 }, { "backend\\apply_engine.c", 1 }, { "backend\\commands.c", 2 },
    { "backend\\editor_frame.c", 1 }, { "backend\\engine_dialog.c", 2 },
    { "backend\\grid_room_editor.c", 5 }, { "backend\\grid_room_native.c", 4 },
    { "backend\\grid_room_snap.c", 4 }, { "backend\\hook.c", 4 },
    { "backend\\nav_menu.c", 4 },
    { "backend\\nav_play.c", 2 }, { "backend\\nav_preview.c", 2 },
    { "backend\\patch.c", 2 }, { "backend\\rawmap.c", 3 },
    { "backend\\smoke.c", 1 }, { "backend\\strids.c", 1 }, { "backend\\swf_textedit.c", 1 },
    { "backend\\timeline_tile.c", 1 }, { "backend\\modserve_engine.c", 1 },
    { "backend\\modpublished.c", 5 }, { "backend\\modpublish.c", 1 },
    { "backend\\wiring_cleandirect.c", 2 }, { "fault_shield\\mapload_guards.c", 2 },
    { "fault_shield\\recovery.c", 1 }, { "backend\\settings_tab.c", 3 },
};

static int count_calls(const char *text)
{
    static const char *const tokens[] = { "hook_prepare(", "hook_prepare_rip_lea(", "hook_prepare_rip_load(",
                                          "hook_prepare_relative_call(", "sh_prepare_detour_sig(" };
    int n = 0;
    for (size_t t = 0; t < sizeof tokens / sizeof tokens[0]; t++)
        for (const char *p = strstr(text, tokens[t]); p; p = strstr(p + 1, tokens[t])) n++;
    return n;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) buf[n] = '\0';
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

static void census(const char *repo)
{
    static const char *const dirs[] = { "backend", "fault_shield" };
    int files = 0;
    for (size_t d = 0; d < 2; d++) {
        char pattern[MAX_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\src\\%s\\*.c", repo, dirs[d]);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) { printf("FAIL census: no sources under %s\n", pattern); g_fail = 1; continue; }
        do {
            char path[MAX_PATH], rel[MAX_PATH];
            char *text;
            int calls, expected = 0;
            _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\src\\%s\\%s", repo, dirs[d], fd.cFileName);
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s\\%s", dirs[d], fd.cFileName);
            text = read_text(path);
            if (!text) { printf("FAIL census: cannot read %s\n", path); g_fail = 1; continue; }
            calls = count_calls(text);
            free(text);
            for (size_t i = 0; i < sizeof CENSUS / sizeof CENSUS[0]; i++)
                if (!_stricmp(CENSUS[i].file, rel)) expected = CENSUS[i].calls;
            if (calls != expected) {
                printf("FAIL census: %s has %d detour preparation call(s), the audit expects %d -- "
                       "add the new site to SITES and CENSUS\n", rel, calls, expected);
                g_fail = 1;
            }
            if (calls) files++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    printf("census: %d source files prepare detours\n", files);
    if (files != (int)(sizeof CENSUS / sizeof CENSUS[0])) {
        printf("FAIL census: expected %u files\n", (unsigned)(sizeof CENSUS / sizeof CENSUS[0]));
        g_fail = 1;
    }
}

int main(int argc, char **argv)
{
    printf("     build  | file                           | site                         | rva       | fixed | boundary | relative       | verdict\n");
    int images = 0;
    for (int i = 1; i < argc; i++) {
        DWORD attr = GetFileAttributesA(argv[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) census(argv[i]);
        else { check_image(argv[i]); images++; }
    }
    if (!images) printf("detour_windows_test: no images given (the window audit needs -Doom/-DoomAlt)\n");
    printf(g_fail ? "detour_windows_test: FAIL\n" : "detour_windows_test: PASS\n");
    return g_fail;
}
