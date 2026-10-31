/* editor_undo.c -- see editor_undo.h. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "editor_undo.h"
#ifndef SH_EDITOR_UNDO_TESTING
#include "backend_log.h"
#include "iface_engine.h"
#include "process_heap_scope.h"
#endif

#define EU_MAP_OFF      0x204c8   /* editor -> idSnapMapEdit *, null outside a session */
#define EU_SCREEN_OFF   0x21088   /* editor screen; null while the editor is hidden */
#define EU_MAX_ENTRIES  (1 << 20)

static int eu_read_ptr(const void *p, void **out)
{
    __try { *out = *(void *const *)p; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = NULL; return 0; }
}

static int eu_read_i32(const void *p, int *out)
{
    __try { *out = *(const int *)p; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return 0; }
}

/* The header, validated: 1 with its fields. */
static int eu_header(uint8_t *list, uint8_t **data, int *count, int *cap, uint8_t **cursor)
{
    void *d = NULL, *c = NULL;
    if (!list || !eu_read_ptr(list, &d) || !eu_read_i32(list + SH_EDITOR_UNDO_COUNT_OFF, count) ||
        !eu_read_i32(list + SH_EDITOR_UNDO_CAP_OFF, cap) || !eu_read_ptr(list + SH_EDITOR_UNDO_CURSOR_OFF, &c))
        return 0;
    *data = (uint8_t *)d;
    *cursor = (uint8_t *)c;
    if (*count < 0 || *cap < *count || *cap > EU_MAX_ENTRIES || (*count && !d)) return 0;
    /* The cursor names an entry (or, for an empty list, the slot before the
     * first, which is where a fresh list's push starts from). */
    if (*count) {
        uintptr_t off;
        if ((uintptr_t)c < (uintptr_t)d) return 0;
        off = (uintptr_t)c - (uintptr_t)d;
        if (off % sizeof(void *) || off / sizeof(void *) >= (uintptr_t)*count) return 0;
    }
    return 1;
}

int sh_editor_undo_truncate(uint8_t *editor, sh_editor_undo_push_fn push, int *before, int *after)
{
    void *l = NULL;
    uint8_t *list, *data = NULL, *cursor = NULL, *first = NULL;
    int count = -1, cap = 0, ok = 0;
    if (before) *before = -1;
    if (after) *after = -1;
    if (!editor || !push || !eu_read_ptr(editor + SH_EDITOR_UNDO_LIST_OFF, &l) || !l) return 0;
    list = (uint8_t *)l;
    if (!eu_header(list, &data, &count, &cap, &cursor)) return 0;
    if (before) *before = count;
    __try {
        /* The push starts by advancing the cursor one entry, then destroys
         * every entry from there to the end: from the slot before the first,
         * that is all of them. */
        *(uint8_t **)(list + SH_EDITOR_UNDO_CURSOR_OFF) = data - sizeof(void *);
        push(list, editor);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) return -1;
    if (!eu_header(list, &data, &count, &cap, &cursor)) return -1;
    if (after) *after = count;
    if (count != 1 || cursor != data || !eu_read_ptr(data, (void **)&first) || !first) return -1;
    return 1;
}

uintptr_t sh_editor_undo_decode_push(const uint8_t *commit, const uint8_t *image, size_t image_size)
{
    const uint8_t *at, *wrapper, *push;
    int rel = 0;
    if (!commit || !image || !image_size) return 0;
    __try {
        at = commit + SH_EDITOR_UNDO_COMMIT_CALL;
        if (at[0] != 0xE8) return 0;
        memcpy(&rel, at + 1, sizeof rel);
        wrapper = at + 5 + rel;
        if (wrapper < image || wrapper + 8 > image + image_size) return 0;
        /* mov rcx,[rcx]; jmp rel32 */
        if (wrapper[0] != 0x48 || wrapper[1] != 0x8B || wrapper[2] != 0x09 || wrapper[3] != 0xE9) return 0;
        memcpy(&rel, wrapper + 4, sizeof rel);
        push = wrapper + 8 + rel;
        if (push < image || push >= image + image_size) return 0;
        return (uintptr_t)push;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

#ifndef SH_EDITOR_UNDO_TESTING
static sh_editor_undo_push_fn g_push;
static sh_process_heap_api g_heap;
static int g_heap_ok;
static volatile LONG g_faulted;

int sh_editor_undo_install(const sig_result *results, size_t n, const uint8_t *module_base)
{
    const uint8_t *commit = (const uint8_t *)sig_addr_by_name(results, n, "EditorCommitEdit");
    size_t image_size = 0;
    char line[200];
    if (module_base) {
        __try {
            const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
            const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
            image_size = nt->OptionalHeader.SizeOfImage;
        } __except (EXCEPTION_EXECUTE_HANDLER) { image_size = 0; }
    }
    g_push = (sh_editor_undo_push_fn)sh_editor_undo_decode_push(commit, module_base, image_size);
    g_heap_ok = sh_process_heap_bind(&g_heap, results, n, module_base);
    _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: undo clear: push %s (rva 0x%llx)%s",
                g_push ? "decoded from EditorCommitEdit" : "NOT decoded; Uninstall refuses while a map is open",
                g_push ? (unsigned long long)((uintptr_t)g_push - (uintptr_t)module_base) : 0ull,
                g_heap_ok ? "" : "; no process-heap scope");
    backend_log(line);
    return g_push && g_heap_ok;
}

static uint8_t *eu_editor_map_open(void)
{
    uint8_t *ed = (uint8_t *)sh_iface_engine_editor_base();
    void *map = NULL, *screen = NULL;
    if (!ed || !eu_read_ptr(ed + EU_MAP_OFF, &map) || !map) return NULL;
    if (!eu_read_ptr(ed + EU_SCREEN_OFF, &screen) || !screen) return NULL;
    return ed;
}

int sh_editor_undo_clear(int probe)
{
    sh_process_heap_scope scope;
    uint8_t *ed = eu_editor_map_open();
    int r, before = -1, after = -1;
    char line[200];
    if (!ed) return 0;
    if (!g_push || !g_heap_ok || InterlockedCompareExchange(&g_faulted, 0, 0)) return -1;
    if (probe) return 1;
    if (!sh_process_heap_enter(&g_heap, &scope)) {
        backend_log("MODS: undo clear: the process-heap scope could not be entered; the history was kept");
        return -1;
    }
    r = sh_editor_undo_truncate(ed, g_push, &before, &after);
    if (!sh_process_heap_leave(&scope)) {
        InterlockedExchange(&g_faulted, 1);
        backend_log("MODS: undo clear: the process-heap scope did not restore; the clear is off for this session");
        r = -1;
    }
    if (r < 0) InterlockedExchange(&g_faulted, 1);
    _snprintf_s(line, sizeof line, _TRUNCATE, "MODS: undo clear: %s (%d entr%s before, %d after)",
                r > 0 ? "the undo and redo history now holds only the current state"
                      : r == 0 ? "the undo list was not recognised; nothing changed" : "FAILED; the clear is off for this session",
                before, before == 1 ? "y" : "ies", after);
    backend_log(line);
    return r > 0 ? 1 : -1;
}
#endif
