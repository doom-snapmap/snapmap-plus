/* modsound.c -- see modsound.h. Two vtable-slot swaps (Wwise open by name,
 * LoadBanks) and the engine's own per-bank submit, for banks installed mods
 * carry. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modsound.h"
#ifndef SH_MODSOUND_PURE
#include "backend_log.h"
#include "commands.h"
#include "engine_globals.h"
#include "modserve.h"
#include "process_heap_scope.h"
#endif

/* ------------------------------------------------------------------ pure */

int sh_modsound_leaf(const char *request, char *leaf, size_t cap)
{
    static const char dir[] = "sound/soundbanks/pc/";
    size_t i, n;
    if (!request || !leaf || !cap) return 0;
    for (i = 0; i < sizeof dir - 1; i++) {
        char c = request[i];
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != dir[i]) return 0;
    }
    request += sizeof dir - 1;
    n = strlen(request);
    if (n <= 4 || n + 1 > cap || _stricmp(request + n - 4, ".bnk") != 0) return 0;
    for (i = 0; i < n; i++) {
        char c = request[i];
        if (c == '/' || c == '\\' || c == ':') return 0;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        leaf[i] = c;
    }
    leaf[n] = 0;
    return 1;
}

int sh_modsound_name_matches(const wchar_t *name, const char *leaf)
{
    size_t i;
    if (!name || !leaf || !leaf[0]) return 0;
    __try {
        for (i = 0; leaf[i]; i++) {
            wchar_t w = name[i];
            char c = leaf[i];
            if (!w || w > 0x7f || w == L'/' || w == L'\\') return 0;
            if (w >= L'A' && w <= L'Z') w = (wchar_t)(w - L'A' + L'a');
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((char)w != c) return 0;
        }
        return name[i] == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static uintptr_t rip_target(const uint8_t *insn, size_t disp_off, size_t len)
{
    int32_t d;
    memcpy(&d, insn + disp_off, sizeof d);
    return (uintptr_t)(insn + len) + (intptr_t)d;
}

int sh_modsound_decode_cmd(const uint8_t *c, uintptr_t *sound_system, uintptr_t *hw_slot)
{
    /* SUB RSP,0x28; LEA RCX,[rip]; CALL; MOV RCX,[rip]; MOV RAX,[RCX]; ADD RSP,0x28; JMP [RAX+0x30] */
    static const uint8_t tail[] = { 0x48, 0x8B, 0x01, 0x48, 0x83, 0xC4, 0x28, 0x48, 0xFF, 0x60, 0x30 };
    uintptr_t ss, slot;
    if (!c) return 0;
    __try {
        if (c[0] != 0x48 || c[1] != 0x83 || c[2] != 0xEC || c[3] != 0x28) return 0;
        if (c[4] != 0x48 || c[5] != 0x8D || c[6] != 0x0D || c[11] != 0xE8) return 0;
        if (c[16] != 0x48 || c[17] != 0x8B || c[18] != 0x0D || memcmp(c + 23, tail, sizeof tail)) return 0;
        ss = rip_target(c + 4, 3, 7);
        slot = rip_target(c + 16, 3, 7);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (slot != ss + SH_MODSOUND_HW_OFF) return 0;
    if (sound_system) *sound_system = ss;
    if (hw_slot) *hw_slot = slot;
    return 1;
}

int sh_modsound_find_slots(const uint8_t *module_base, uint64_t value, void ***slots, int cap)
{
    int found = 0;
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS64 *nt;
        const IMAGE_SECTION_HEADER *sec;
        unsigned i;
        if (!module_base || dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
        nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
        sec = IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            size_t span = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            const uint64_t *q, *end;
            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) ||
                (sec->Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE))) continue;
            if (sec->SizeOfRawData < span) span = sec->SizeOfRawData;
            q = (const uint64_t *)(module_base + ((sec->VirtualAddress + 7u) & ~7u));
            end = (const uint64_t *)(module_base + sec->VirtualAddress + (span & ~(size_t)7));
            for (; q < end; q++) {
                if (*q != value) continue;
                if (slots && found < cap) slots[found] = (void **)q;
                found++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return found;
}

int sh_modsound_table_add(sh_modsound_table *t, const char *request, const char *disk_path, uint64_t size,
                          const char *bundle_id, int is_stock)
{
    char leaf[SH_MODSOUND_LEAF_MAX];
    sh_modsound_bank *b;
    int i;
    if (!t || !sh_modsound_leaf(request, leaf, sizeof leaf)) return 0;
    if (is_stock || !disk_path || !disk_path[0] || t->count >= SH_MODSOUND_MAX_BANKS) { t->skipped++; return -1; }
    for (i = 0; i < t->count; i++)
        if (!strcmp(t->banks[i].leaf, leaf)) { t->skipped++; return -1; }
    b = &t->banks[t->count];
    memset(b, 0, sizeof *b);
    memcpy(b->leaf, leaf, strlen(leaf) + 1);
    b->disk_path = _strdup(disk_path);
    if (!b->disk_path) { t->skipped++; return -1; }
    b->size = size;
    strncpy_s(b->bundle, sizeof b->bundle, bundle_id ? bundle_id : "", _TRUNCATE);
    t->count++;
    return 1;
}

void sh_modsound_table_free(sh_modsound_table *t)
{
    int i;
    if (!t) return;
    for (i = 0; i < t->count; i++) free(t->banks[i].disk_path);
    memset(t, 0, sizeof *t);
}

const sh_modsound_bank *sh_modsound_lookup(const sh_modsound_table *t, const wchar_t *name, int open_mode)
{
    int i;
    if (!t || !name || open_mode != 0) return NULL;
    for (i = 0; i < t->count; i++)
        if (sh_modsound_name_matches(name, t->banks[i].leaf)) return &t->banks[i];
    return NULL;
}

void sh_modsound_fill_desc(sh_modsound_file_desc *d, uint64_t size, void *handle, uint32_t device_id)
{
    memset(d, 0, sizeof *d);
    d->file_size = size;
    d->handle = handle;
    d->device_id = device_id;
}

/* ------------------------------------------------------------------ core */

typedef int32_t (*open_fn)(void *self, const wchar_t *name, int32_t mode, const void *flags, uint8_t *sync,
                           sh_modsound_file_desc *desc);
typedef int (*submit_fn)(void *hardware, const char *name);
typedef void (*loadbanks_fn)(void *hardware);

static open_fn g_open_orig;
static loadbanks_fn g_loadbanks_orig;
static submit_fn g_submit;
static void **g_hw_slot;              /* &soundSystem+0x140 */
static void *g_test_hw;
static sh_modsound_table *volatile g_table;   /* published; retired tables are never freed */
static volatile LONG g_gen = 1;       /* LoadBanks generation: bumped after each LoadBanks */
static volatile LONG g_opened, g_open_failed, g_submits, g_pending;
static int g_live;

/* Process heap 0 around every engine call that allocates Wwise memory.
 * Wwise's allocation hook (VK 0x1419f5b10, GL 0x141a11bc0) is the engine
 * allocator with heap -1, which takes the top of the main thread's heap
 * stack: a LoadBanks run from the console while a map is open (s_loadBanks,
 * s_restart) put the new stream packages' pools in the map heap, the map
 * unload freed them, and the next streamed open walked a freed package
 * (VK 0x141ad832c). */
#ifdef SH_MODSOUND_PURE
typedef struct ms_scope { int active; } ms_scope;
static int (*g_test_enter)(void);
static void (*g_test_leave)(void);
static int ms_heap_enter(ms_scope *s) { s->active = g_test_enter ? g_test_enter() : 1; return s->active; }
static int ms_heap_leave(ms_scope *s) { if (s->active && g_test_leave) g_test_leave(); s->active = 0; return 1; }
#define ms_log(...) ((void)0)
static int ms_on_main(void) { return 1; }
#else
static const uint8_t *g_main_thread_at;
static sh_process_heap_api g_heap;
typedef sh_process_heap_scope ms_scope;
static int ms_heap_enter(ms_scope *s) { return sh_process_heap_enter(&g_heap, s); }
static int ms_heap_leave(ms_scope *s) { return sh_process_heap_leave(s); }
static int ms_on_main(void)
{
    uint32_t id = 0;
    if (!g_main_thread_at) return -1;
    __try { id = *(const uint32_t *)g_main_thread_at; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return id && id == GetCurrentThreadId() ? 1 : 0;
}
static void ms_log(const char *fmt, ...)
{
    char line[768];
    int n;
    va_list ap;
    n = _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: sound: ");
    va_start(ap, fmt);
    _vsnprintf_s(line + n, sizeof line - (size_t)n, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
    if (ms_on_main() == 1) sh_printf("%s\n", line);
}
#endif

static void *ms_hardware(void)
{
    void *hw = NULL;
    if (g_test_hw) return g_test_hw;
    if (!g_hw_slot) return NULL;
    __try { hw = *g_hw_slot; } __except (EXCEPTION_EXECUTE_HANDLER) { hw = NULL; }
    return hw;
}

/* idSoundHardware_WWise::Init sets hardware+8 bit 1 when Wwise came up. */
static int ms_hardware_ready(void *hw)
{
    uint8_t flags = 0;
    if (!hw) return 0;
    __try { flags = ((const uint8_t *)hw)[8]; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return (flags & 2) != 0;
}

static int32_t __fastcall ms_open_name(void *self, const wchar_t *name, int32_t mode, const void *flags,
                                       uint8_t *sync, sh_modsound_file_desc *desc)
{
    const sh_modsound_table *t = g_table;
    const sh_modsound_bank *b = sh_modsound_lookup(t, name, mode);
    wchar_t wpath[MAX_PATH * 2];
    HANDLE h;
    LARGE_INTEGER size;
    uint32_t device = 0;
    if (!b || !sync || !desc) return g_open_orig(self, name, mode, flags, sync, desc);
    __try { device = *(const uint32_t *)((const uint8_t *)self + SH_MODSOUND_DEVICE_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return g_open_orig(self, name, mode, flags, sync, desc); }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b->disk_path, -1, wpath, (int)(sizeof wpath / sizeof *wpath)))
        h = INVALID_HANDLE_VALUE;
    else
        h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &size) || (uint64_t)size.QuadPart != b->size) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (InterlockedIncrement(&g_open_failed) <= 32)
            ms_log("open of %s from %s failed (missing or not %llu bytes); Wwise gets the engine's own answer",
                   b->leaf, b->bundle, (unsigned long long)b->size);
        return g_open_orig(self, name, mode, flags, sync, desc);
    }
    sh_modsound_fill_desc(desc, (uint64_t)size.QuadPart, h, device);
    *sync = 1;
    if (InterlockedIncrement(&g_opened) <= 32)
        ms_log("opened %s for Wwise from %s (%llu bytes)", b->leaf, b->bundle, (unsigned long long)b->size);
    return 1;   /* AK_Success */
}

static int ms_submit_one(void *hw, sh_modsound_bank *b, LONG gen, const char *phase)
{
    int r = 0, ok = 0;
    ms_scope heap;
    memset(&heap, 0, sizeof heap);
    if (!ms_heap_enter(&heap)) {
        ms_log("%s: %s not submitted (the process heap scope could not be entered)", phase, b->leaf);
        return 0;
    }
    __try {
        __try { r = g_submit(hw, b->leaf); ok = 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    } __finally {
        if (!ms_heap_leave(&heap)) ms_log("%s: the process heap scope did not balance", phase);
    }
    if (!ok) { ms_log("%s: submit of %s FAULTED; not retried", phase, b->leaf); InterlockedExchange(&b->submitted_gen, gen); return 0; }
    InterlockedExchange(&b->submitted_gen, gen);
    InterlockedIncrement(&g_submits);
    /* The wrapper returns AK_Success (1) when the async load was queued; its
     * callback prints the outcome. */
    ms_log("%s: submitted %s (%s) to the engine's bank load: %s", phase, b->leaf, b->bundle,
           r == 1 ? "queued; expect 'async load success id <id> : <name>'" : "refused (see the WWise line above)");
    return 1;
}

/* Submit every bank of the current table not yet submitted in this
 * generation. Main thread only. */
static int ms_pass(const char *phase)
{
    sh_modsound_table *t = g_table;
    void *hw;
    LONG gen = g_gen;
    int i, n = 0;
    if (!t || !t->count || !g_submit) { InterlockedExchange(&g_pending, 0); return 0; }
    hw = ms_hardware();
    if (!ms_hardware_ready(hw)) {
        if (!InterlockedExchange(&g_pending, 1))
            ms_log("%s: %d bank(s) indexed; the sound system is not up yet, so they are submitted at the next pass", phase,
                   t->count);
        return 0;
    }
    for (i = 0; i < t->count; i++)
        if (t->banks[i].submitted_gen != gen) n += ms_submit_one(hw, &t->banks[i], gen, phase);
    InterlockedExchange(&g_pending, 0);
    return n;
}

static void __fastcall ms_loadbanks(void *hardware)
{
    ms_scope heap;
    int scoped = 0;
    memset(&heap, 0, sizeof heap);
    /* On the main thread (s_loadBanks, s_reloadAudioEvents, s_restart, which
     * may run with a map heap on top), LoadBanks runs in process heap 0 so
     * the stream packages it loads outlive the map. */
    if (ms_on_main() == 1) {
        scoped = ms_heap_enter(&heap);
        if (!scoped) ms_log("LoadBanks: the process heap scope could not be entered; packages it loads now may not "
                            "outlive the open map");
    }
    __try {
        g_loadbanks_orig(hardware);
    } __finally {
        if (scoped && !ms_heap_leave(&heap)) ms_log("LoadBanks: the process heap scope did not balance");
    }
    /* A new Wwise lifetime (s_restart) or a rerun (s_loadBanks): submit
     * again; an already-loaded bank answers "async load failure 69". */
    InterlockedIncrement(&g_gen);
    if (ms_on_main() == 1) (void)ms_pass("LoadBanks");
    else InterlockedExchange(&g_pending, 1);
}

/* Carry the submitted generation to a rebuilt table: a bank already given to
 * Wwise under that name is not given again (Wwise keeps the first bytes for
 * a bank id until it restarts). */
static void ms_carry(sh_modsound_table *nt, const sh_modsound_table *old)
{
    int i, j;
    if (!old) return;
    for (i = 0; i < nt->count; i++)
        for (j = 0; j < old->count; j++)
            if (!strcmp(nt->banks[i].leaf, old->banks[j].leaf) && old->banks[j].submitted_gen) {
                nt->banks[i].submitted_gen = old->banks[j].submitted_gen;
                if (strcmp(nt->banks[i].disk_path, old->banks[j].disk_path) || nt->banks[i].size != old->banks[j].size)
                    ms_log("%s changed (%s); Wwise keeps the bank it loaded, so the new bytes take effect at the next "
                           "launch", nt->banks[i].leaf, nt->banks[i].bundle);
            }
    for (j = 0; j < old->count; j++) {
        int kept = 0;
        for (i = 0; i < nt->count; i++) if (!strcmp(nt->banks[i].leaf, old->banks[j].leaf)) kept = 1;
        if (!kept && old->banks[j].submitted_gen)
            ms_log("%s (%s) is no longer installed; it stays loaded until the game restarts", old->banks[j].leaf,
                   old->banks[j].bundle);
    }
}

static void ms_publish(sh_modsound_table *nt)
{
    sh_modsound_table *old = g_table;
    ms_carry(nt, old);
    InterlockedExchangePointer((void *volatile *)&g_table, nt);
    /* `old` is retired, never freed: an open on Wwise's thread may hold it. */
}

#ifdef SH_MODSOUND_TESTING
void sh_modsound_test_bind(sh_modsound_open_fn original_open, sh_modsound_loadbanks_fn original_loadbanks,
                           sh_modsound_submit_fn submit, void *hardware, const sh_modsound_table *table)
{
    sh_modsound_table *t = (sh_modsound_table *)calloc(1, sizeof *t);
    int i;
    g_open_orig = (open_fn)original_open;
    g_loadbanks_orig = (loadbanks_fn)original_loadbanks;
    g_submit = (submit_fn)submit;
    g_test_hw = hardware;
    if (t && table) {
        for (i = 0; i < table->count; i++) {
            char req[128];
            _snprintf_s(req, sizeof req, _TRUNCATE, "sound/soundbanks/pc/%s", table->banks[i].leaf);
            sh_modsound_table_add(t, req, table->banks[i].disk_path, table->banks[i].size, table->banks[i].bundle, 0);
        }
    }
    if (t) ms_publish(t);
}
int32_t sh_modsound_test_open(void *self, const wchar_t *name, int32_t mode, const void *flags, uint8_t *sync,
                              sh_modsound_file_desc *desc)
{ return ms_open_name(self, name, mode, flags, sync, desc); }
void sh_modsound_test_loadbanks(void *hardware) { ms_loadbanks(hardware); }
int sh_modsound_test_pass(void) { return ms_pass("test"); }
void sh_modsound_test_heap(int (*enter)(void), void (*leave)(void)) { g_test_enter = enter; g_test_leave = leave; }
#endif

/* ------------------------------------------------------------------ engine */
#ifndef SH_MODSOUND_PURE

typedef struct ms_collect { sh_modsound_table *t; int banks; } ms_collect;

static void ms_collect_file(void *context, const char *request, uint64_t size, const char *bundle_id,
                            const char *version, const char *bundle_hash)
{
    ms_collect *c = (ms_collect *)context;
    sh_modserve_hit hit;
    char leaf[SH_MODSOUND_LEAF_MAX];
    (void)version; (void)bundle_hash;
    if (!sh_modsound_leaf(request, leaf, sizeof leaf)) return;
    if (!sh_modserve_lookup(request, &hit)) return;
    switch (sh_modsound_table_add(c->t, request, hit.disk_path, size, bundle_id, hit.is_stock)) {
    case 1: c->banks++; break;
    case -1: ms_log("%s from %s is not delivered (a stock pointer, a duplicate name or more than %d banks)", leaf,
                    bundle_id ? bundle_id : "", SH_MODSOUND_MAX_BANKS); break;
    default: break;
    }
}

static void ms_rebuild(void)
{
    sh_modsound_table *t = (sh_modsound_table *)calloc(1, sizeof *t);
    ms_collect c;
    if (!t) { ms_log("bank table refused (out of memory); the previous one stays"); return; }
    c.t = t;
    c.banks = 0;
    sh_modserve_each_served(ms_collect_file, &c);
    ms_publish(t);
    if (t->count) ms_log("%d bank(s) indexed", t->count);
}

/* The serving index listener: any thread. */
static void ms_index_changed(void)
{
    if (!g_live) return;
    ms_rebuild();
    if (ms_on_main() == 1) (void)ms_pass("install");
    else if (g_table && g_table->count) InterlockedExchange(&g_pending, 1);
}

void sh_modsound_before_pass(const char *phase)
{
    if (!g_live || ms_on_main() != 1) return;
    (void)ms_pass(phase);
}

void sh_modsound_print_status(void)
{
    sh_modsound_table *t = g_table;
    int i, submitted = 0;
    if (!g_live) { sh_printf("sh_mods: sound banks: route unavailable on this build\n"); return; }
    for (i = 0; t && i < t->count; i++) if (t->banks[i].submitted_gen == g_gen) submitted++;
    sh_printf("sh_mods: sound banks: %d indexed, %d submitted in this sound-system lifetime, %ld opened for Wwise "
              "(%ld failed)%s\n", t ? t->count : 0, submitted, g_opened, g_open_failed,
              g_pending ? "; a submit is pending" : "");
    for (i = 0; t && i < t->count; i++)
        sh_printf("  %s  %s  %llu bytes  %s\n", t->banks[i].leaf, t->banks[i].bundle,
                  (unsigned long long)t->banks[i].size, t->banks[i].submitted_gen == g_gen ? "submitted" : "not submitted");
}

static const sig_result *ms_clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name)) return r[i].status == SIG_OK ? &r[i] : NULL;
    return NULL;
}

static int ms_swap(void **slot, void *expect, void *with)
{
    DWORD old, ignored;
    int ok;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    ok = InterlockedCompareExchangePointer(slot, with, expect) == expect;
    VirtualProtect(slot, sizeof(void *), old, &ignored);
    return ok;
}

int sh_modsound_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    const sig_result *lb = ms_clean(results, count, "AudioLoadBanks");
    const sig_result *sb = ms_clean(results, count, "AudioBankSubmit");
    const sig_result *on = ms_clean(results, count, "AudioFileOpenName");
    const sig_result *cmd = ms_clean(results, count, "SoundLoadBanksCmd");
    void **open_slots[SH_MODSOUND_MAX_SLOTS], **lb_slots[2];
    uintptr_t ss = 0, hw_slot = 0;
    int n_open, n_lb, i, swapped = 0;
    if (g_live) return 1;
    if (!lb || !sb || !on || !cmd) {
        ms_log("bank routes unavailable: %s is not a clean signature match",
               !lb ? "AudioLoadBanks" : !sb ? "AudioBankSubmit" : !on ? "AudioFileOpenName" : "SoundLoadBanksCmd");
        return 0;
    }
    g_main_thread_at = (const uint8_t *)glb_resolve(module_base, "main_thread_id", NULL);
    if (!g_main_thread_at || !sh_process_heap_bind(&g_heap, results, count, module_base)) {
        ms_log("bank routes unavailable: %s", g_main_thread_at ? "the process heap scope is unbound" : "main_thread_id is unresolved");
        return 0;
    }
    if (!sh_modsound_decode_cmd((const uint8_t *)cmd->addr, &ss, &hw_slot)) {
        ms_log("bank routes unavailable: s_loadBanks no longer reads soundSystem+0x140 as expected");
        return 0;
    }
    n_open = sh_modsound_find_slots(module_base, (uint64_t)on->addr, open_slots, SH_MODSOUND_MAX_SLOTS);
    n_lb = sh_modsound_find_slots(module_base, (uint64_t)lb->addr, lb_slots, 2);
    if (n_open < 1 || n_open > SH_MODSOUND_MAX_SLOTS || n_lb != 1) {
        ms_log("bank routes unavailable: %d read-only slot(s) hold AudioFileOpenName (1 to %d expected) and %d hold "
               "LoadBanks (1 expected)", n_open, SH_MODSOUND_MAX_SLOTS, n_lb);
        return 0;
    }
    g_open_orig = (open_fn)on->addr;
    g_loadbanks_orig = (loadbanks_fn)lb->addr;
    g_submit = (submit_fn)sb->addr;
    g_hw_slot = (void **)hw_slot;
    ms_rebuild();
    MemoryBarrier();
    for (i = 0; i < n_open; i++) {
        if (!ms_swap(open_slots[i], (void *)on->addr, (void *)ms_open_name)) {
            while (--i >= 0) ms_swap(open_slots[i], (void *)ms_open_name, (void *)on->addr);
            ms_log("bank routes unavailable: an open-by-name slot could not be swapped");
            return 0;
        }
        swapped++;
    }
    if (!ms_swap(lb_slots[0], (void *)lb->addr, (void *)ms_loadbanks))
        ms_log("LoadBanks slot not swapped; banks are submitted at main-thread passes only (s_restart drops them)");
    g_live = 1;
    sh_modserve_set_index_listener(ms_index_changed);
    ms_log("bank routes installed: open-by-name rva 0x%X in %d slot(s), LoadBanks rva 0x%X (slot rva 0x%llX), submit "
           "rva 0x%X, soundSystem rva 0x%llX; banks are submitted at the first editor entry or map load",
           on->rva, swapped, lb->rva, (unsigned long long)((const uint8_t *)lb_slots[0] - module_base), sb->rva,
           (unsigned long long)(ss - (uintptr_t)module_base));
    return 1;
}
#endif /* SH_MODSOUND_PURE */
