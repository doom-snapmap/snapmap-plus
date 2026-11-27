/* modsound.c -- see modsound.h. Two vtable-slot swaps (Wwise open by name,
 * LoadBanks) and the engine's own per-bank submit, for banks installed mods
 * carry. */
#define WIN32_LEAN_AND_MEAN
#include "image_view.h"
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modsound.h"
#include "modsound_view.h"
#include "modbundle.h"
#include "modbankinfo.h"
#ifndef SH_MODSOUND_PURE
#include "backend_log.h"
#include "commands.h"
#include "engine_globals.h"
#include "modserve.h"
#include "process_heap_scope.h"
#endif

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
        const IMAGE_NT_HEADERS64 *nt = sh_pe_headers(module_base);
        const IMAGE_SECTION_HEADER *sec;
        unsigned i;
        if (!nt) return -1;
        sec = IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            size_t span = sh_pe_section_span(sec);
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
    if (is_stock || !disk_path || !disk_path[0]) { t->skipped++; return -1; }
    for (i = 0; i < t->count; i++)
        if (!strcmp(t->banks[i].leaf, leaf)) { t->skipped++; return -1; }
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 16;
        sh_modsound_bank *grown = (sh_modsound_bank *)realloc(t->banks, (size_t)cap * sizeof *grown);
        if (!grown) { t->skipped++; return -1; }
        t->banks = grown;
        t->cap = cap;
    }
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
    for (i = 0; i < t->count; i++) {
        free(t->banks[i].disk_path);
        free(t->banks[i].author_path);
    }
    free(t->banks);
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

int sh_modsound_decode_cookie_site(const uint8_t *s, uintptr_t *op_new, uintptr_t *cookie_ctor, uintptr_t *callback,
                                   uintptr_t *load_bank)
{
    /* CMP [rip],0 (7); JNZ (2); MOV ECX,0x130 (5); CALL op_new (+14); MOV [RSP+40],RAX; TEST; JZ; MOV RDX,[RSP+60];
     * MOV RCX,RAX; CALL ctor (+37); NOP; LEA RCX; MOV [RSP+20],RCX; OR R9D,-1; MOV R8,RAX; LEA RDX,[rip] (+60);
     * MOV RCX,[RSP+60]; CALL LoadBank (+72) */
    static const uint8_t mid[] = { 0x48, 0x89, 0x44, 0x24, 0x40, 0x48, 0x85, 0xC0, 0x74, 0x0E, 0x48, 0x8B, 0x54, 0x24,
                                   0x60, 0x48, 0x8B, 0xC8 };
    static const uint8_t pre_cb[] = { 0x90, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0x48, 0x89, 0x4C, 0x24, 0x20, 0x41, 0x83,
                                      0xC9, 0xFF, 0x4C, 0x8B, 0xC0, 0x48, 0x8D, 0x15 };
    static const uint8_t pre_lb[] = { 0x48, 0x8B, 0x4C, 0x24, 0x60, 0xE8 };
    uintptr_t a, b, c, d;
    if (!s) return 0;
    __try {
        if (s[0] != 0x83 || s[1] != 0x3D || s[6] != 0x00 || s[7] != 0x75) return 0;
        if (s[9] != 0xB9 || *(const uint32_t *)(s + 10) != SH_MODSOUND_COOKIE_SIZE || s[14] != 0xE8) return 0;
        if (memcmp(s + 19, mid, sizeof mid) || s[37] != 0xE8) return 0;
        if (memcmp(s + 42, pre_cb, sizeof pre_cb) || memcmp(s + 67, pre_lb, sizeof pre_lb)) return 0;
        a = rip_target(s + 14, 1, 5);
        b = rip_target(s + 37, 1, 5);
        c = rip_target(s + 60, 3, 7);
        d = rip_target(s + 72, 1, 5);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (op_new) *op_new = a;
    if (cookie_ctor) *cookie_ctor = b;
    if (callback) *callback = c;
    if (load_bank) *load_bank = d;
    return 1;
}

static sh_modsound_need *ms_need_slot(sh_modsound_needs *t, const char *hash, const char *key, int create)
{
    int i;
    for (i = 0; i < t->count; i++) if (!_stricmp(t->bundles[i].hash, hash)) return &t->bundles[i];
    if (!create) return NULL;
    if (t->count == t->cap) {
        int cap = t->cap ? t->cap * 2 : 8;
        sh_modsound_need *grown = (sh_modsound_need *)realloc(t->bundles, (size_t)cap * sizeof *grown);
        if (!grown) return NULL;
        t->bundles = grown;
        t->cap = cap;
    }
    memset(&t->bundles[t->count], 0, sizeof t->bundles[0]);
    strncpy_s(t->bundles[t->count].hash, sizeof t->bundles[0].hash, hash, _TRUNCATE);
    strncpy_s(t->bundles[t->count].key, sizeof t->bundles[0].key, key ? key : "", _TRUNCATE);
    return &t->bundles[t->count++];
}

int sh_modsound_needs_add(sh_modsound_needs *t, const char *hash, const char *key, const char *bank)
{
    sh_modsound_need *n;
    char leaf[SH_MODSOUND_LEAF_MAX];
    size_t i, len;
    int k;
    if (!t || !hash || !hash[0] || !bank) return -1;
    len = strlen(bank);
    if (!len || len >= sizeof leaf) return -1;
    for (i = 0; i <= len; i++) leaf[i] = (bank[i] >= 'A' && bank[i] <= 'Z') ? (char)(bank[i] - 'A' + 'a') : bank[i];
    /* Only a bank the game's own index lists, and only one SnapMap skips. */
    if (sh_mbi_skipped_bank(leaf, NULL) != 1) return -1;
    n = ms_need_slot(t, hash, key, 1);
    if (!n) return -1;
    for (k = 0; k < n->count; k++) if (!strcmp(n->banks[k], leaf)) return 0;
    if (n->count >= SH_MBI_BANKS_MAX) return -1;   /* every bank the game lists is in already */
    memcpy(n->banks[n->count++], leaf, len + 1);
    return 1;
}

int sh_modsound_needs_add_registries(sh_modsound_needs *t, const char *hash, const char *key, const char *registries)
{
    const char *p;
    int added = 0;
    if (!registries) return 0;
    /* The canonical text the builder writes: "banks":["a","b"]. */
    p = strstr(registries, "\"banks\":[");
    if (!p) return 0;
    p += 9;
    while (*p == '"') {
        char bank[SH_MODSOUND_LEAF_MAX];
        const char *e = strchr(p + 1, '"');
        size_t n;
        if (!e) break;
        n = (size_t)(e - p - 1);
        if (n && n < sizeof bank && !memchr(p + 1, '\\', n)) {
            memcpy(bank, p + 1, n);
            bank[n] = 0;
            if (sh_modsound_needs_add(t, hash, key, bank) == 1) added++;
        }
        p = e + 1;
        if (*p != ',') break;
        p++;
    }
    return added;
}

const sh_modsound_need *sh_modsound_needs_find(const sh_modsound_needs *t, const char *hash)
{
    int i;
    if (!t || !hash) return NULL;
    for (i = 0; i < t->count; i++) if (!_stricmp(t->bundles[i].hash, hash)) return &t->bundles[i];
    return NULL;
}

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

/* Process heap 0 around every engine call that allocates Wwise memory. Wwise's allocation hook (VK 0x1419f5b10,
 * GL 0x141a11bc0) is the engine allocator with heap -1, which takes the top of the main thread's heap stack, so a
 * LoadBanks run with a map open (s_loadBanks, s_restart) would put the stream packages' pools in the map heap and
 * the map unload would free them under the next streamed open. */
#ifdef SH_MODSOUND_PURE
typedef struct ms_scope { int active; } ms_scope;
static int (*g_test_enter)(void);
static void (*g_test_leave)(void);
static int ms_heap_enter(ms_scope *s) { s->active = g_test_enter ? g_test_enter() : 1; return s->active; }
static int ms_heap_leave(ms_scope *s) { if (s->active && g_test_leave) g_test_leave(); s->active = 0; return 1; }
static void (*g_test_sink)(const char *line);
static void ms_log(const char *fmt, ...)
{
    char line[768];
    int n;
    va_list ap;
    if (!g_test_sink) return;
    n = _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: sound: ");
    va_start(ap, fmt);
    _vsnprintf_s(line + n, sizeof line - (size_t)n, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_test_sink(line);
}
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

static int ms_submit_view(sh_modsound_bank *b, int *result);

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
        __try { if (!ms_submit_view(b, &r)) r = g_submit(hw, b->leaf); ok = 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    } __finally {
        if (!ms_heap_leave(&heap)) ms_log("%s: the process heap scope did not balance", phase);
    }
    if (!ok) { ms_log("%s: submit of %s FAULTED; not retried", phase, b->leaf); InterlockedExchange(&b->submitted_gen, gen); return 0; }
    InterlockedExchange(&b->submitted_gen, gen);
    InterlockedIncrement(&g_submits);
    /* The wrapper answers AK_Success (1) when the async load was queued (its callback prints the outcome), and also
     * for a name its listed-bank filter excludes, which loads nothing; the builder refuses such stems for a mod's
     * own bank. */
    if (r == 1 && !sh_mbi_snapmap_loads(b->leaf))
        ms_log("%s: submit of %s (%s) refused: the engine's listed-bank filter excludes the name in SnapMap (its "
               "success answer loads nothing)", phase, b->leaf, b->bundle);
    else
        ms_log("%s: submitted %s (%s) to the engine's bank load: %s", phase, b->leaf, b->bundle,
               r == 1 ? "queued; expect 'async load success id <id> : <name>'" : "refused (see the WWise line above)");
    return 1;
}

typedef void *(*ms_opnew_fn)(size_t size);
typedef void *(*ms_ctor_fn)(void *self, const char *name);
typedef void (*ms_cb_fn)(uint32_t bank_id, const void *in_memory, int32_t result, int32_t pool, void *cookie);
typedef int32_t (*ms_loadbank_fn)(const char *name, ms_cb_fn callback, void *cookie, int32_t pool, uint32_t *bank_id);

static ms_opnew_fn g_opnew;
static ms_ctor_fn g_cookie_ctor;
static ms_loadbank_fn g_loadbank;
static ms_cb_fn g_engine_cb;
static void **g_bank_manager;

/* The native memory bank is identified by both id and address. Keep one view per immutable bank name, including
 * across map changes and s_loadBanks, where the sound system can still hold the original address. */
typedef struct ms_view {
    struct ms_view *next;
    char leaf[SH_MODSOUND_LEAF_MAX];
    sh_sound_view view;
    HANDLE ready;
    LONG generation;
    volatile LONG refs;
    int preview;
} ms_view;
static ms_view *g_views;
static SRWLOCK g_retry_lock = SRWLOCK_INIT;
static ms_view *g_unload_retry;

typedef struct ms_view_result { ms_view *bank; void *native; } ms_view_result;

static void ms_view_drop(ms_view *v)
{
    if (InterlockedDecrement(&v->refs)) return;
    CloseHandle(v->ready);
    sh_sound_view_close(&v->view);
    free(v);
}

static void ms_view_callback(uint32_t id, const void *bytes, int32_t result, int32_t pool, void *cookie)
{
    ms_view_result *c = cookie;
    if (result != 1)
        ms_log("sound bank %s (id %u) finished loading with Wwise result %d", c->bank->leaf, (unsigned)id, (int)result);
    g_engine_cb(id, bytes, result, pool, c->native);
    SetEvent(c->bank->ready);
    ms_view_drop(c->bank);
    free(c);
}

static void ms_unload_callback(uint32_t id, const void *bytes, int32_t result, int32_t pool, void *cookie)
{
    ms_view *v = cookie;
    (void)id; (void)bytes; (void)pool;
    if (result == 1 || result == 54) { /* unloaded, or this (id,address) is absent after an audio reset */
        ms_log("released %s (bank id %u, native unload result %d)", v->leaf, (unsigned)v->view.id, (int)result);
        ms_view_drop(v);
        return;
    }
    ms_log("unload of %s returned %d; keeping its media until Wwise releases it", v->leaf, (int)result);
    AcquireSRWLockExclusive(&g_retry_lock);
    v->next = g_unload_retry;
    g_unload_retry = v;
    ReleaseSRWLockExclusive(&g_retry_lock);
}

static void ms_unload_unused(sh_modsound_table *t)
{
    ms_view **link = &g_views, *v, *retire;
    int i;
    if (!g_bank_manager || !*g_bank_manager) return;
    AcquireSRWLockExclusive(&g_retry_lock);
    retire = g_unload_retry;
    g_unload_retry = NULL;
    ReleaseSRWLockExclusive(&g_retry_lock);
    while ((v = *link) != NULL) {
        for (i = 0; t && i < t->count; i++) if (!strcmp(v->leaf, t->banks[i].leaf)) break;
        if (t && i < t->count && (t->banks[i].wanted || v->preview)) { link = &v->next; continue; }
        if (t && i < t->count) InterlockedExchange(&t->banks[i].submitted_gen, 0);
        *link = v->next;
        v->next = retire;
        retire = v;
    }
    while ((v = retire) != NULL) {
        int result = 2;
        ms_scope heap = {0};
        retire = v->next;
        if (ms_heap_enter(&heap)) {
            __try {
                result = sh_sound_view_queue(*g_bank_manager, &v->view, 1, ms_unload_callback, v);
            } __finally {
                if (!ms_heap_leave(&heap)) ms_log("unload: the process heap scope did not balance");
            }
        }
        if (result != 1) ms_unload_callback(v->view.id, v->view.bytes, result, -1, v);
    }
}

static int ms_submit_view(sh_modsound_bank *b, int *result)
{
    ms_view *v;
    void *cookie;
    ms_view_result *c;
    if (!g_bank_manager || !*g_bank_manager || !g_cookie_ctor || !g_engine_cb ||
        !sh_mbi_snapmap_loads(b->leaf)) return 0;
    for (v = g_views; v && _stricmp(v->leaf, b->leaf); v = v->next) {}
    if (!v) {
        v = calloc(1, sizeof *v);
        if (!v) return 0;
        if (!sh_sound_view_open(b->disk_path, b->size, &v->view)) { free(v); return 0; }
        if (v->view.id != sh_mb_fnv1_lower(b->leaf, strlen(b->leaf) - 4)) {
            sh_sound_view_close(&v->view); free(v); return 0;
        }
        v->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!v->ready) { sh_sound_view_close(&v->view); free(v); return 0; }
        strcpy_s(v->leaf, sizeof v->leaf, b->leaf);
        v->refs = 1;
        v->next = g_views;
        g_views = v;
    }
    if (v->generation == g_gen) { *result = 1; return 1; }
    c = malloc(sizeof *c);
    if (!c) return 0;
    cookie = g_opnew(SH_MODSOUND_COOKIE_SIZE);
    if (!cookie) { free(c); return 0; }
    c->native = g_cookie_ctor(cookie, b->leaf);
    c->bank = v;
    ResetEvent(v->ready);
    v->generation = g_gen;
    InterlockedIncrement(&v->refs);
    *result = sh_sound_view_queue(*g_bank_manager, &v->view, 0, ms_view_callback, c);
    /* The queue delivers no callback on immediate refusal. The native result callback owns the cookie. */
    if (*result != 1) ms_view_callback(v->view.id, v->view.bytes, *result, -1, c);
    return 1;
}

static int ms_clear_preview(void)
{
    ms_view *v;
    int changed = 0;
    for (v = g_views; v; v = v->next) {
        changed |= v->preview;
        v->preview = 0;
    }
    return changed;
}

static void ms_prepare_preview(sh_modsound_table *t, const char *sound)
{
    void *hw;
    char event[512];
    const char *leaf;
    uint32_t id;
    int i, selected = -1;
    if (!sound || _strnicmp(sound, "mods/", 5) || !t) return;
    leaf = strrchr(sound, '/');
    if (!leaf || _snprintf_s(event, sizeof event, _TRUNCATE, "play_%s", leaf + 1) < 0) return;
    id = sh_mb_fnv1_lower(event, strlen(event));
    hw = ms_hardware();
    if (!ms_hardware_ready(hw)) return;
    for (i = 0; i < t->count; i++) {
        sh_modsound_bank *b = &t->banks[i];
        size_t salt = strlen(b->bundle);
        if (_strnicmp(sound + 5, b->bundle, salt) || sound[5 + salt] != '/') continue;
        if (selected >= 0 && b->submitted_gen != g_gen) continue;
        if (sh_sound_bank_has_event(b->disk_path, b->size, id)) {
            selected = i;
            if (b->submitted_gen == g_gen) break;
        }
    }
    if (selected >= 0) {
        sh_modsound_bank *b = &t->banks[selected];
        ms_view *v;
        if (b->submitted_gen != g_gen) (void)ms_submit_one(hw, b, g_gen, "preview");
        for (v = g_views; v; v = v->next) if (!strcmp(v->leaf, b->leaf)) {
            v->preview = 1;
            (void)WaitForSingleObject(v->ready, SH_MODSOUND_STOCK_WAIT_MS);
            break;
        }
    }
}

void sh_modsound_preview(const char *sound)
{
    sh_modsound_table *t = g_table;
    int previous;
    if (ms_on_main() != 1) return;
    previous = ms_clear_preview();
    ms_prepare_preview(t, sound);
    if (previous) ms_unload_unused(t);
}
static int g_stock_live;
static unsigned g_stock_wait_ms = SH_MODSOUND_STOCK_WAIT_MS;
static sh_modsound_needs *volatile g_needs;   /* published; retired tables are never freed */

/* One stock bank asked for in this process. Slots are claimed on the main
 * thread and never released; the callback finds its slot by cookie. Each
 * bank is asked once a session, so the game's own bank count bounds them. */
#define MS_STOCK_SLOTS SH_MBI_BANKS_MAX
typedef struct ms_stock {
    char leaf[SH_MODSOUND_LEAF_MAX];
    char key[80];
    void *volatile cookie;
    HANDLE done;
    LARGE_INTEGER t0, t1;
    volatile LONG answered;   /* the callback ran */
    volatile LONG result;     /* its AKRESULT */
    volatile LONG waited_out; /* the map pass stopped waiting; the callback logs */
    uint32_t bank_id;
} ms_stock;
static ms_stock g_stocks[MS_STOCK_SLOTS];
static volatile LONG g_nstocks;

static double ms_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

/* Wwise's bank thread (or the caller, for a synchronous answer). */
static void ms_bank_cb(uint32_t bank_id, const void *in_memory, int32_t result, int32_t pool, void *cookie)
{
    LONG i, n = InterlockedCompareExchange(&g_nstocks, 0, 0);
    /* Newest first, and a slot answers once: the engine deletes each cookie, so a later cookie may reuse a deleted
     * one's address. */
    for (i = n - 1; i >= 0; i--) {
        ms_stock *s = &g_stocks[i];
        double ms;
        if (!cookie || InterlockedCompareExchangePointer((void *volatile *)&s->cookie, NULL, cookie) != cookie) continue;
        QueryPerformanceCounter(&s->t1);
        ms = ms_ms(s->t0, s->t1);
        InterlockedExchange(&s->result, result);
        InterlockedExchange(&s->answered, 1);
        sh_mbi_state_set(s->leaf, result == 1 ? SH_MBI_ST_LOADED : SH_MBI_ST_FAILED, result, ms, s->key);
        if (InterlockedCompareExchange(&s->waited_out, 0, 0)) {
            if (result == 1) ms_log("loaded stock bank %s (%.0f ms) for %s, after the map pass stopped waiting", s->leaf, ms, s->key);
            else ms_log("stock bank %s did not load (AKRESULT %d) for %s; not retried this session", s->leaf, (int)result, s->key);
        }
        if (s->done) SetEvent(s->done);
        break;
    }
    /* The engine's own callback prints its line and deletes the cookie. */
    if (g_engine_cb) g_engine_cb(bank_id, in_memory, result, pool, cookie);
}

/* Load one stock bank. Main thread, Wwise up. 1 loaded, 0 not. */
static int ms_stock_load(const char *leaf, const char *key, const char *phase)
{
    ms_stock *s;
    ms_scope heap;
    void *cookie = NULL;
    int32_t r = 0;
    uint32_t id = 0, want = 0;
    int ok = 0, called = 0;
    DWORD w = WAIT_TIMEOUT;
    LONG at = InterlockedCompareExchange(&g_nstocks, 0, 0);
    if (at >= MS_STOCK_SLOTS) {
        sh_mbi_state_set(leaf, SH_MBI_ST_REFUSED, 0, 0, key);
        ms_log("%s: stock bank %s for %s not loaded: more than %d stock banks asked for in this session", phase, leaf, key,
               MS_STOCK_SLOTS);
        return 0;
    }
    s = &g_stocks[at];
    memset(s, 0, sizeof *s);
    strncpy_s(s->leaf, sizeof s->leaf, leaf, _TRUNCATE);
    strncpy_s(s->key, sizeof s->key, key ? key : "", _TRUNCATE);
    (void)sh_mbi_skipped_bank(leaf, &want);
    s->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&g_nstocks, at + 1);
    sh_mbi_state_set(leaf, SH_MBI_ST_LOADING, 0, 0, key);
    memset(&heap, 0, sizeof heap);
    if (!ms_heap_enter(&heap)) {
        sh_mbi_state_set(leaf, SH_MBI_ST_REFUSED, 0, 0, key);
        ms_log("%s: stock bank %s for %s not loaded (the process heap scope could not be entered); not retried this session",
               phase, leaf, key);
        return 0;
    }
    __try {
        __try {
            cookie = g_opnew(SH_MODSOUND_COOKIE_SIZE);
            if (cookie) {
                g_cookie_ctor(cookie, s->leaf);
                s->cookie = cookie;
                MemoryBarrier();
                QueryPerformanceCounter(&s->t0);
                called = 1;
                r = g_loadbank(s->leaf, ms_bank_cb, cookie, -1, &id);
                s->bank_id = id;
                /* Wait for the callback inside heap 0: the bank's memory comes from the top of the main thread's
                 * heap stack. */
                if (r == 1) w = s->done ? WaitForSingleObject(s->done, g_stock_wait_ms) : WAIT_FAILED;
            }
            ok = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    } __finally {
        if (!ms_heap_leave(&heap)) ms_log("%s: the process heap scope did not balance", phase);
    }
    if (!ok) {
        sh_mbi_state_set(leaf, SH_MBI_ST_REFUSED, 0, 0, key);
        ms_log("%s: stock bank %s for %s: the engine's load FAULTED%s; not retried this session", phase, leaf, key,
               called ? " in LoadBank" : " before LoadBank");
        return 0;
    }
    if (!cookie) {
        sh_mbi_state_set(leaf, SH_MBI_ST_REFUSED, 0, 0, key);
        ms_log("%s: stock bank %s for %s not loaded (operator new returned NULL)", phase, leaf, key);
        return 0;
    }
    if (r != 1) {
        /* Wwise calls no callback for a load it refused; the cookie stays, as the wrapper's does. */
        if (!InterlockedCompareExchange(&s->answered, 0, 0)) sh_mbi_state_set(leaf, SH_MBI_ST_FAILED, r, 0, key);
        ms_log("%s: stock bank %s for %s refused by Wwise (AKRESULT %d); not retried this session", phase, leaf, key, (int)r);
        return 0;
    }
    if (want && id != want)
        ms_log("%s: stock bank %s: Wwise gave id %u, soundbanksinfo.xml %u", phase, leaf, (unsigned)id, (unsigned)want);
    if (w != WAIT_OBJECT_0 || !InterlockedCompareExchange(&s->answered, 0, 0)) {
        InterlockedExchange(&s->waited_out, 1);
        /* The callback may have run between the wait and the flag. */
        if (!InterlockedCompareExchange(&s->answered, 0, 0)) {
            ms_log("%s: stock bank %s for %s is still loading after %u ms (id %u); its events start once it finishes", phase,
                   leaf, key, g_stock_wait_ms, (unsigned)id);
            return 0;
        }
    }
    if (InterlockedCompareExchange(&s->result, 0, 0) != 1) {
        ms_log("%s: stock bank %s did not load (AKRESULT %d) for %s; not retried this session", phase, leaf,
               (int)InterlockedCompareExchange(&s->result, 0, 0), key);
        return 0;
    }
    ms_log("loaded stock bank %s (%.0f ms) for %s", leaf, ms_ms(s->t0, s->t1), key);
    return 1;
}

/* The map pass: every bank the declared bundles need, once per process. The
 * hashes are the builds that serve the map's rows (sh_modserve_row_build), so
 * each names its build as it is. */
static int ms_stock_pass(const char *const *hashes, int n)
{
    const sh_modsound_needs *t = g_needs;
    int i, k, loaded = 0;
    void *hw;
    if (!g_stock_live || !t || !t->count || !hashes || n <= 0) return 0;
    hw = ms_hardware();
    for (i = 0; i < n; i++) {
        const sh_modsound_need *need;
        const char *h = hashes[i];
        need = sh_modsound_needs_find(t, h);
        for (k = 0; need && k < need->count; k++) {
            if (sh_mbi_state_get(need->banks[k], NULL, NULL, NULL, 0) != SH_MBI_ST_NONE) continue;
            if (!ms_hardware_ready(hw)) {
                ms_log("map: stock bank %s for %s waits: the sound system is not up yet", need->banks[k], need->key);
                continue;
            }
            loaded += ms_stock_load(need->banks[k], need->key, "map");
        }
    }
    return loaded;
}

/* Submit every bank of the current table not yet submitted in this
 * generation. Main thread only. */
static int ms_pass(const char *phase)
{
    sh_modsound_table *t = g_table;
    void *hw;
    LONG gen = g_gen;
    int i, n = 0;
    if (!g_submit || ((!t || !t->count) && !g_views &&
        !InterlockedCompareExchangePointer((void *volatile *)&g_unload_retry, NULL, NULL))) {
        InterlockedExchange(&g_pending, 0); return 0;
    }
    hw = ms_hardware();
    if (!ms_hardware_ready(hw)) {
        if (!InterlockedExchange(&g_pending, 1))
            ms_log("%s: %d bank(s) indexed; the sound system is not up yet, so they are submitted at the next pass", phase,
                   t ? t->count : 0);
        return 0;
    }
#ifndef SH_MODSOUND_PURE
    if (!sh_modserve_scope_map_open() || sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_PLAY) ms_clear_preview();
    if (t) sh_modsound_select_banks(t);
#endif
    ms_unload_unused(t);
    for (i = 0; t && i < t->count; i++)
        if (t->banks[i].wanted && t->banks[i].submitted_gen != gen) n += ms_submit_one(hw, &t->banks[i], gen, phase);
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
typedef struct ms_restart { char leaf[SH_MODSOUND_LEAF_MAX]; char owner[80]; } ms_restart;
static ms_restart *g_restart;   /* grows; read and written under the lock */
static int        g_restart_count, g_restart_cap;
static SRWLOCK    g_restart_lock = SRWLOCK_INIT;

static void ms_restart_note(const char *leaf, const char *owner)
{
    int i;
    AcquireSRWLockExclusive(&g_restart_lock);
    for (i = 0; i < g_restart_count && strcmp(g_restart[i].leaf, leaf); i++) {}
    if (i == g_restart_count && g_restart_count == g_restart_cap) {
        int cap = g_restart_cap ? g_restart_cap * 2 : 16;
        ms_restart *grown = (ms_restart *)realloc(g_restart, (size_t)cap * sizeof *grown);
        if (grown) { g_restart = grown; g_restart_cap = cap; }
    }
    if (i < g_restart_cap) {
        strncpy_s(g_restart[i].leaf, sizeof g_restart[i].leaf, leaf, _TRUNCATE);
        strncpy_s(g_restart[i].owner, sizeof g_restart[i].owner, owner ? owner : "", _TRUNCATE);
        if (i == g_restart_count) g_restart_count++;
    }
    ReleaseSRWLockExclusive(&g_restart_lock);
}

int sh_modsound_restart_banks(void (*fn)(void *context, const char *bank, const char *owner), void *context)
{
    int i, n;
    AcquireSRWLockShared(&g_restart_lock);
    n = g_restart_count;
    for (i = 0; fn && i < n; i++) fn(context, g_restart[i].leaf, g_restart[i].owner);
    ReleaseSRWLockShared(&g_restart_lock);
    return n;
}

static void ms_carry(sh_modsound_table *nt, const sh_modsound_table *old)
{
    int i, j;
    if (!old) return;
    for (i = 0; i < nt->count; i++)
        for (j = 0; j < old->count; j++)
            if (!strcmp(nt->banks[i].leaf, old->banks[j].leaf) && old->banks[j].submitted_gen) {
                nt->banks[i].submitted_gen = old->banks[j].submitted_gen;
                if (strcmp(nt->banks[i].disk_path, old->banks[j].disk_path) || nt->banks[i].size != old->banks[j].size) {
                    ms_restart_note(nt->banks[i].leaf, nt->banks[i].bundle);
                    ms_log("Restart DOOM to hear the new %s: Wwise keeps the bank it loaded first", nt->banks[i].leaf);
                }
            }
    for (j = 0; j < old->count; j++) {
        int kept = 0;
        for (i = 0; i < nt->count; i++) if (!strcmp(nt->banks[i].leaf, old->banks[j].leaf)) kept = 1;
        if (!kept && old->banks[j].submitted_gen)
            ms_log("%s is no longer served; it stays loaded until DOOM restarts", old->banks[j].leaf);
    }
}

static int ms_same_table(const sh_modsound_table *a, const sh_modsound_table *b)
{
    int i;
    if (!a || !b || a->count != b->count) return 0;
    for (i = 0; i < a->count; i++)
        if (strcmp(a->banks[i].leaf, b->banks[i].leaf) || strcmp(a->banks[i].disk_path, b->banks[i].disk_path) ||
            a->banks[i].size != b->banks[i].size || strcmp(a->banks[i].build,b->banks[i].build))
            return 0;
    return 1;
}

/* Publish a rebuilt table; one that serves what the current one serves is
 * dropped, so the passes that rebuild it publish only a change. Returns 1
 * when published. */
static int ms_publish(sh_modsound_table *nt)
{
    sh_modsound_table *old = g_table;
    if (ms_same_table(nt, old)) {
        sh_modsound_table_free(nt);
        free(nt);
        return 0;
    }
    ms_carry(nt, old);
    InterlockedExchangePointer((void *volatile *)&g_table, nt);
    /* `old` is retired, never freed: an open on Wwise's thread may hold it. */
    return 1;
}

#ifdef SH_MODSOUND_TESTING
void sh_modsound_test_manager(void **slot) { g_bank_manager = slot; }
void sh_modsound_test_select(const char *salt)
{
    int i;
    for (i = 0; g_table && i < g_table->count; ++i)
        g_table->banks[i].wanted = salt && !strcmp(g_table->banks[i].bundle, salt);
}
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
            char req[SH_MODSOUND_LEAF_MAX + 32];
            _snprintf_s(req, sizeof req, _TRUNCATE, "sound/soundbanks/pc/%s", table->banks[i].leaf);
            if (sh_modsound_table_add(t, req, table->banks[i].disk_path, table->banks[i].size, table->banks[i].bundle, 0) == 1)
                t->banks[t->count - 1].wanted = 1;
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
void sh_modsound_test_bind_stock(sh_modsound_opnew_fn op_new, sh_modsound_cookie_ctor_fn ctor,
                                 sh_modsound_loadbank_fn load_bank, sh_modsound_bank_cb_fn engine_callback,
                                 const sh_modsound_needs *needs, unsigned wait_ms)
{
    sh_modsound_needs *t = (sh_modsound_needs *)calloc(1, sizeof *t);
    g_opnew = (ms_opnew_fn)op_new;
    g_cookie_ctor = (ms_ctor_fn)ctor;
    g_loadbank = (ms_loadbank_fn)load_bank;
    g_engine_cb = (ms_cb_fn)engine_callback;
    g_stock_wait_ms = wait_ms;
    g_stock_live = op_new && ctor && load_bank;
    if (t && needs) memcpy(t, needs, sizeof *t);
    InterlockedExchangePointer((void *volatile *)&g_needs, t);
}
void sh_modsound_test_log(void (*sink)(const char *line)) { g_test_sink = sink; }
void sh_modsound_test_bank_callback(uint32_t bank_id, const void *in_memory, int32_t result, int32_t pool, void *cookie)
{ ms_bank_cb(bank_id, in_memory, result, pool, cookie); }
int sh_modsound_test_stock(const char *const *bundle_hashes, int n) { return ms_stock_pass(bundle_hashes, n); }
void sh_modsound_test_stock_reset(void)
{
    LONG i;
    for (i = 0; i < g_nstocks; i++) if (g_stocks[i].done) CloseHandle(g_stocks[i].done);
    memset(g_stocks, 0, sizeof g_stocks);
    InterlockedExchange(&g_nstocks, 0);
}
#endif

#ifndef SH_MODSOUND_PURE

typedef struct ms_collect { sh_modsound_table *t; int banks; } ms_collect;

static void ms_collect_file(void *context, const char *request, uint64_t size, const char *bundle_id,
                            const char *bundle_hash)
{
    ms_collect *c = (ms_collect *)context;
    sh_modserve_hit hit;
    char leaf[SH_MODSOUND_LEAF_MAX];
    (void)size; (void)bundle_id; (void)bundle_hash;
    if (!sh_modsound_leaf(request, leaf, sizeof leaf)) return;
    if (!sh_modserve_lookup(request, &hit)) return;
    switch (sh_modsound_table_add(c->t, request, hit.disk_path, hit.size, hit.bundle_id, hit.is_stock)) {
    case 1: {
        sh_modsound_bank *b = &c->t->banks[c->t->count - 1];
        b->author_path = _strdup(hit.author_path ? hit.author_path : "");
        if (hit.bundle_hash) strncpy_s(b->build, sizeof b->build, hit.bundle_hash, _TRUNCATE);
        c->banks++;
        break;
    }
    case -1: ms_log("%s is not loaded: it is a stock pointer, another mod in use serves that name, or memory ran out",
                    leaf); break;
    default: break;
    }
}

/* Stock-bank needs: the "banks" the builder records in each build's block
 * (every build records them). */
static void ms_need_registries(void *context, const char *id, const char *hash, const char *registries)
{
    sh_modsound_needs *t = (sh_modsound_needs *)context;
    if (registries && strstr(registries, "\"banks\":[")) (void)sh_modsound_needs_add_registries(t, hash, id, registries);
}

static void ms_rebuild_needs(void)
{
    sh_modsound_needs *t = (sh_modsound_needs *)calloc(1, sizeof *t);
    int i, banks = 0;
    if (!t) { ms_log("stock-bank needs refused (out of memory); the previous ones stay"); return; }
    /* The game's bank index is read only when a build lists a bank. */
    (void)sh_modserve_each_bundle_registries(ms_need_registries, t);
    InterlockedExchangePointer((void *volatile *)&g_needs, t);   /* the old table is retired, never freed */
    for (i = 0; i < t->count; i++) banks += t->bundles[i].count;
    if (banks) {
        char list[400];
        size_t at = 0;
        int k;
        list[0] = 0;
        for (i = 0; i < t->count && at < sizeof list - 1; i++)
            for (k = 0; k < t->bundles[i].count && at < sizeof list - 1; k++) {
                int w = _snprintf_s(list + at, sizeof list - at, _TRUNCATE, "%s%s for %s", at ? ", " : "",
                                    t->bundles[i].banks[k], t->bundles[i].key);
                if (w < 0) break;
                at += (size_t)w;
            }
        /* Once per change of the list, not at every rescan. */
        static char last[400];
        if (strcmp(last, list)) {
            strcpy_s(last, sizeof last, list);
            ms_log("stock banks needed by %d bundle(s): %s; each loads at the first map load that declares it%s", t->count,
                   list, g_stock_live ? "" : " (the stock-bank route is unavailable on this build)");
        }
    }
}

/* The banks the open map serves: each served bank name and the file of the build serving it
 * (sh_modserve_lookup, in the open map's context). */
static void ms_rebuild_banks(void)
{
    sh_modsound_table *t = (sh_modsound_table *)calloc(1, sizeof *t);
    ms_collect c;
    int count;
    if (!t) { ms_log("bank table refused (out of memory); the previous one stays"); return; }
    c.t = t;
    c.banks = 0;
    sh_modserve_each_served(ms_collect_file, &c);
    count = t->count;
    if (ms_publish(t) && count) ms_log("%d bank(s) indexed", count);
}

static void ms_rebuild(void)
{
    ms_rebuild_banks();
    ms_rebuild_needs();
}

int sh_modsound_stock_before_map(const char *const *bundle_hashes, int n)
{
    if (!g_live || ms_on_main() != 1) return 0;
    return ms_stock_pass(bundle_hashes, n);
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
    /* A map pass serves the builds that map uses (one salt can be at the root
     * and in a map's place at two builds). */
    ms_rebuild_banks();
    (void)ms_pass(phase);
}

int sh_modsound_apply_rows(void)
{
    if (!g_live) return 0;
    if (ms_on_main() != 1) return g_table && g_table->count > 0;
    (void)ms_pass("map sounds");
    return 0;
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
    {
        const sh_modsound_needs *nd = g_needs;
        int k;
        for (i = 0; nd && i < nd->count; i++)
            for (k = 0; k < nd->bundles[i].count; k++) {
                int result = 0;
                double ms = 0;
                int st = sh_mbi_state_get(nd->bundles[i].banks[k], &result, &ms, NULL, 0);
                char how[64] = "";
                if (st == SH_MBI_ST_LOADED && ms > 0) _snprintf_s(how, sizeof how, _TRUNCATE, " in %.0f ms", ms);
                else if (st == SH_MBI_ST_FAILED) _snprintf_s(how, sizeof how, _TRUNCATE, " (AKRESULT %d)", result);
                sh_printf("  stock %s  needed by %s  %s%s%s\n", nd->bundles[i].banks[k], nd->bundles[i].key,
                          sh_mbi_state_word(st), how, g_stock_live ? "" : " (route unavailable on this build)");
            }
    }
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
    const sig_result *lb = sig_ok(results, count, "AudioLoadBanks");
    const sig_result *sb = sig_ok(results, count, "AudioBankSubmit");
    const sig_result *on = sig_ok(results, count, "AudioFileOpenName");
    const sig_result *cmd = sig_ok(results, count, "SoundLoadBanksCmd");
    const sig_result *s_lb = sig_ok(results, count, "AudioLoadBankAsync");
    const sig_result *s_new = sig_ok(results, count, "AudioCookieNew");
    const sig_result *s_cb = sig_ok(results, count, "AudioBankLoadCallback");
    const sig_result *s_site = sig_ok(results, count, "AudioBankCookieSite");
    uintptr_t d_new = 0, d_ctor = 0, d_cb = 0, d_lb = 0;
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
    /* Stock banks on demand: the wrapper's post-filter calls, each checked
     * against its own signature; the cookie ctor comes from the call alone. */
    if (!s_lb || !s_new || !s_cb || !s_site)
        ms_log("stock-bank route unavailable: %s is not a clean signature match",
               !s_lb ? "AudioLoadBankAsync" : !s_new ? "AudioCookieNew" : !s_cb ? "AudioBankLoadCallback" : "AudioBankCookieSite");
    else if (s_site->addr < sb->addr || s_site->addr > sb->addr + 0x200 ||
             !sh_modsound_decode_cookie_site((const uint8_t *)s_site->addr, &d_new, &d_ctor, &d_cb, &d_lb))
        ms_log("stock-bank route unavailable: the wrapper's cookie site does not decode inside AudioBankSubmit");
    else if (d_new != s_new->addr || d_cb != s_cb->addr || d_lb != s_lb->addr)
        ms_log("stock-bank route unavailable: the wrapper's calls (new rva 0x%llX, callback rva 0x%llX, LoadBank rva 0x%llX) "
               "are not the signatures' functions", (unsigned long long)(d_new - (uintptr_t)module_base),
               (unsigned long long)(d_cb - (uintptr_t)module_base), (unsigned long long)(d_lb - (uintptr_t)module_base));
    else {
        g_opnew = (ms_opnew_fn)d_new;
        g_cookie_ctor = (ms_ctor_fn)d_ctor;
        g_engine_cb = (ms_cb_fn)d_cb;
        g_loadbank = (ms_loadbank_fn)d_lb;
        g_bank_manager = sh_sound_view_manager((const uint8_t *)d_lb);
        g_stock_live = 1;
    }
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
           "rva 0x%X, soundSystem rva 0x%llX; banks are submitted at the first editor entry or map load; stock banks on "
           "demand %s", on->rva, swapped, lb->rva, (unsigned long long)((const uint8_t *)lb_slots[0] - module_base), sb->rva,
           (unsigned long long)(ss - (uintptr_t)module_base), g_stock_live ? "available (LoadBank, cookie and callback "
           "match the wrapper's)" : "unavailable");
    return 1;
}
#endif /* SH_MODSOUND_PURE */
