/* Guarded code patches with restore handles, plus wrappers for hook.c detours.
 * Every write runs with the process's other threads suspended and none of them
 * executing inside, or about to return into, the bytes being changed, so no
 * thread can run a half-written instruction. Expected bytes are checked before
 * writing; failed writes roll back; a failed rollback keeps its handle. */
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "patch.h"
#include "hook.h"
#include "backend_log.h"

const char *sh_patch_status_str(sh_patch_status s)
{
    switch (s) {
        case B2_PATCH_OK:              return "OK";
        case B2_PATCH_REFUSED_BADARG:  return "REFUSED_BADARG";
        case B2_PATCH_REFUSED_SIG:     return "REFUSED_SIG";
        case B2_PATCH_REFUSED_VERIFY:  return "REFUSED_VERIFY";
        case B2_PATCH_FAIL_PROTECT:    return "FAIL_PROTECT";
        case B2_PATCH_FAIL_SEH:        return "FAIL_SEH";
        case B2_PATCH_FAIL_NOTLIVE:    return "FAIL_NOTLIVE";
        case B2_PATCH_FAIL_ROLLBACK:   return "FAIL_ROLLBACK";
        case B2_PATCH_FAIL_BUSY:       return "FAIL_BUSY";
        default:                       return "?";
    }
}

/* Guarded memory access. */

/* Return 1 if both ranges were readable, with their equality in match. */
static int safe_memcmp(const uint8_t *a, const uint8_t *b, size_t n, int *match)
{
    __try {
        int m = 1;
        for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) { m = 0; break; } }
        *match = m;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* Return 0 on a memory fault; a partial copy may already have occurred. */
static int safe_memcpy(uint8_t *dst, const uint8_t *src, size_t n)
{
    __try {
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

#ifdef SH_PATCH_TESTING
static uint64_t g_fail_protect, g_fail_write;
static unsigned g_protect_calls, g_write_calls;
static size_t g_write_prefix;
static void (*g_write_observer)(void *target);
void sh_patch_test_observe_write(void (*observer)(void *target)) { g_write_observer = observer; }
void sh_patch_test_faults(uint64_t protect_calls, uint64_t write_calls, size_t write_prefix)
{
    g_fail_protect = protect_calls; g_fail_write = write_calls;
    g_protect_calls = g_write_calls = 0; g_write_prefix = write_prefix;
}
#endif

static int patch_protect(void *target, size_t size, DWORD protection, DWORD *old)
{
#ifdef SH_PATCH_TESTING
    unsigned call = g_protect_calls++;
    if (call < 64 && (g_fail_protect & (UINT64_C(1) << call))) return 0;
#endif
    return VirtualProtect(target, size, protection, old) != 0;
}

static int patch_write(uint8_t *target, const uint8_t *bytes, size_t size)
{
#ifdef SH_PATCH_TESTING
    if (g_write_observer) g_write_observer(target);
    unsigned call = g_write_calls++;
    if (call < 64 && (g_fail_write & (UINT64_C(1) << call))) {
        safe_memcpy(target, bytes, g_write_prefix < size ? g_write_prefix : size);
        return 0;
    }
#endif
    return safe_memcpy(target, bytes, size);
}

/* Quiescing the process.
 *
 * Between quiesce() and resume_all() this thread must not allocate, log or
 * take a lock another thread could hold: every other thread is suspended. Only
 * VirtualProtect, plain copies and FlushInstructionCache run in that window.
 * A thread is inside when its RIP lies strictly within (start, start+len) --
 * RIP == start is an instruction boundary and runs the new bytes -- or when one
 * of its top stack slots is a return address strictly inside the window (a
 * thread in the callee of a call the window covers, such as a stack probe).
 * Then every thread is resumed and the attempt repeated a moment later. A
 * thread created after the thread list is taken is not covered; creating and
 * starting one takes far longer than a patch window. */

#define QUIESCE_MAX_THREADS  1024
#define QUIESCE_ATTEMPTS     100
#define QUIESCE_STACK_SLOTS  8

static SRWLOCK g_quiesce_lock = SRWLOCK_INIT;
static DWORD   g_thread_ids[QUIESCE_MAX_THREADS];
static HANDLE  g_suspended[QUIESCE_MAX_THREADS];
static int     g_suspended_count;

static void resume_all(void)
{
    for (int i = 0; i < g_suspended_count; i++) {
        ResumeThread(g_suspended[i]);
        CloseHandle(g_suspended[i]);
    }
    g_suspended_count = 0;
}

static int inside(uintptr_t value, uintptr_t start, size_t len)
{
    return value > start && value < start + len;
}

static int thread_in_window(HANDLE thread, uintptr_t start, size_t len)
{
    CONTEXT context;
    uint64_t slots[QUIESCE_STACK_SLOTS];
    memset(&context, 0, sizeof context);
    context.ContextFlags = CONTEXT_CONTROL;
    /* GetThreadContext also waits for the suspension to take effect. */
    if (!GetThreadContext(thread, &context)) return 1;
    if (inside((uintptr_t)context.Rip, start, len)) return 1;
    if (!safe_memcpy((uint8_t *)slots, (const uint8_t *)(uintptr_t)context.Rsp, sizeof slots))
        return 0;
    for (int i = 0; i < QUIESCE_STACK_SLOTS; i++)
        if (inside((uintptr_t)slots[i], start, len)) return 1;
    return 0;
}

/* Collect the other threads' ids before suspending anything: the snapshot
 * allocates, and nothing may allocate once a thread is suspended. */
static int list_threads(int *count)
{
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    THREADENTRY32 entry;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    int n = 0, ok = 1;
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    entry.dwSize = sizeof entry;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        if (n == QUIESCE_MAX_THREADS) { ok = 0; break; }
        g_thread_ids[n++] = entry.th32ThreadID;
    }
    CloseHandle(snapshot);
    *count = n;
    return ok;
}

/* Return 1 with every other thread suspended and outside the window, 0 if the
 * threads could not be listed or one stayed inside for every attempt. */
static int quiesce(uintptr_t start, size_t len)
{
    for (int attempt = 0; attempt < QUIESCE_ATTEMPTS; attempt++) {
        int count = 0, busy = 0;
        if (!list_threads(&count)) return 0;
        g_suspended_count = 0;
        for (int i = 0; i < count; i++) {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                                       g_thread_ids[i]);
            if (!thread) continue;               /* it has exited since the list was taken */
            if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); continue; }
            g_suspended[g_suspended_count++] = thread;
        }
        for (int i = 0; i < g_suspended_count && !busy; i++)
            busy = thread_in_window(g_suspended[i], start, len);
        if (!busy) return 1;
        resume_all();
        Sleep(1);
    }
    return 0;
}

typedef enum write_result { WRITE_OK, WRITE_BUSY, WRITE_PROTECT, WRITE_FAULT, WRITE_RESTORE } write_result;

/* Make the range writable, write, flush and set its protection to restore_to
 * (0: the protection it had) with every other thread quiesced. *old_protect
 * receives the protection it had. */
static write_result quiesced_write(uint8_t *t, const uint8_t *bytes, size_t len, DWORD restore_to,
                                   DWORD *old_protect)
{
    DWORD old = 0, ignored;
    write_result result = WRITE_OK;
    AcquireSRWLockExclusive(&g_quiesce_lock);
    if (!quiesce((uintptr_t)t, len)) {
        ReleaseSRWLockExclusive(&g_quiesce_lock);
        return WRITE_BUSY;
    }
    if (!patch_protect(t, len, PAGE_EXECUTE_READWRITE, &old)) result = WRITE_PROTECT;
    else {
        if (!patch_write(t, bytes, len)) result = WRITE_FAULT;
        FlushInstructionCache(GetCurrentProcess(), t, len);
        if (!patch_protect(t, len, restore_to ? restore_to : old, &ignored) && result == WRITE_OK)
            result = WRITE_RESTORE;
    }
    resume_all();
    ReleaseSRWLockExclusive(&g_quiesce_lock);
    if (old_protect) *old_protect = old;
    return result;
}

static sh_patch_status patch_failed(sh_patch_handle *handle, sh_patch_status reason)
{
    if (code_unpatch(handle) == B2_PATCH_OK) return reason;
    backend_log("patch: rollback failed; the restore handle is kept for a retry");
    return B2_PATCH_FAIL_ROLLBACK;
}

/* Append up to `n` bytes of `buf` as hex into `out` (out must hold ~3*n+1). For log diagnostics. */
static void hexdump(const uint8_t *buf, size_t n, char *out, size_t outcap)
{
    size_t p = 0;
    for (size_t i = 0; i < n && p + 3 < outcap; i++)
        p += (size_t)_snprintf_s(out + p, outcap - p, _TRUNCATE, "%02X ", buf[i]);
    if (p && out[p - 1] == ' ') out[p - 1] = '\0';
    else if (!p && outcap) out[0] = '\0';
}

/* Byte patches and restoration. */

sh_patch_status code_patch(void *target, const uint8_t *expect, const uint8_t *new_bytes,
                           size_t len, sh_patch_handle *out_handle)
{
    uint8_t bytes[B2_PATCH_MAX_BYTES];
    char line[512];
    DWORD old = 0;
    write_result written;
    if (out_handle) out_handle->live = 0;
    if (!target || !new_bytes || !out_handle || len == 0 || len > B2_PATCH_MAX_BYTES)
        return B2_PATCH_REFUSED_BADARG;

    uint8_t *t = (uint8_t *)target;

    /* Refuse before writing when expected bytes do not match. */
    if (expect) {
        int match = 0;
        if (!safe_memcmp(t, expect, len, &match)) {
            backend_log("patch: target unreadable while checking its bytes; nothing written");
            return B2_PATCH_FAIL_SEH;
        }
        if (!match) {
            char exp_hex[200], got_hex[200];
            uint8_t got[B2_PATCH_MAX_BYTES];
            int rd = safe_memcpy(got, t, len);
            hexdump(expect, len, exp_hex, sizeof exp_hex);
            if (rd) hexdump(got, len, got_hex, sizeof got_hex);
            _snprintf_s(line, sizeof line, _TRUNCATE,
                "patch: bytes at %p differ (expected [%s], found [%s]); nothing written",
                target, exp_hex, rd ? got_hex : "unreadable");
            backend_log(line);
            return B2_PATCH_REFUSED_VERIFY;
        }
    }

    /* Read the original and the new bytes before any thread is suspended. */
    if (!safe_memcpy(out_handle->orig, t, len) || !safe_memcpy(bytes, new_bytes, len)) {
        backend_log("patch: target or replacement unreadable; nothing written");
        return B2_PATCH_FAIL_SEH;
    }

    written = quiesced_write(t, bytes, len, 0, &old);
    if (written == WRITE_BUSY) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "patch: a thread stayed inside %p for every attempt; nothing written", target);
        backend_log(line);
        return B2_PATCH_FAIL_BUSY;
    }
    if (written == WRITE_PROTECT) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
            "patch: could not make %p writable; nothing written", target);
        backend_log(line);
        return B2_PATCH_FAIL_PROTECT;
    }
    out_handle->target      = target;
    out_handle->len         = len;
    out_handle->old_protect = (uint32_t)old;
    out_handle->live        = 1;
    if (written == WRITE_FAULT) return patch_failed(out_handle, B2_PATCH_FAIL_SEH);
    if (written == WRITE_RESTORE) return patch_failed(out_handle, B2_PATCH_FAIL_PROTECT);
    return B2_PATCH_OK;
}

sh_patch_status code_unpatch(sh_patch_handle *handle)
{
    if (!handle || !handle->live) return B2_PATCH_FAIL_NOTLIVE;
    if (!handle->target || handle->len == 0 || handle->len > B2_PATCH_MAX_BYTES)
        return B2_PATCH_REFUSED_BADARG;

    switch (quiesced_write((uint8_t *)handle->target, handle->orig, handle->len,
                           (DWORD)handle->old_protect, NULL)) {
    case WRITE_OK:
        handle->live = 0;
        return B2_PATCH_OK;
    case WRITE_BUSY:
        backend_log("patch: a thread stayed inside the patch for every attempt; original not restored yet");
        return B2_PATCH_FAIL_BUSY;
    case WRITE_PROTECT:
        backend_log("patch: could not make the patch writable; original not restored");
        return B2_PATCH_FAIL_PROTECT;
    case WRITE_FAULT:
        backend_log("patch: the restore write faulted");
        return B2_PATCH_FAIL_SEH;
    default:
        backend_log("patch: original bytes restored; the page protection restore needs a retry");
        return B2_PATCH_FAIL_PROTECT;
    }
}

/* Signature-gated patch operations. */

sh_patch_status code_patch_call_sig(const sig_result *r, const uint8_t expect[5],
                                    const uint8_t replacement[5], sh_patch_handle *handle)
{
    if (!handle) return B2_PATCH_REFUSED_BADARG;
    handle->live = 0;
    if (!r || r->status != SIG_OK || !r->addr) return B2_PATCH_REFUSED_SIG;
    if (!expect || !replacement || expect[0] != 0xe8 || replacement[0] != 0xe8)
        return B2_PATCH_REFUSED_BADARG;
    return code_patch((void *)r->addr, expect, replacement, 5, handle);
}

sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
                               size_t len, sh_patch_handle *out_handle)
{
    char line[160];
    if (out_handle) out_handle->live = 0;
    if (!r || r->addr == 0 || r->status != SIG_OK) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "patch: %s is not resolved; nothing written",
                    r && r->name ? r->name : "target");
        backend_log(line);
        return B2_PATCH_REFUSED_SIG;
    }
    return code_patch((void *)r->addr, expect, new_bytes, len, out_handle);
}

/* Inline detour wrappers. */

int sh_uninstall_detour(void *tramp)
{
    return hook_unpatch(tramp);
}

sh_patch_status sh_commit_detour(void *tramp) { return hook_commit(tramp); }
int sh_detour_is_installed(void *tramp) { return hook_is_installed(tramp); }

void *sh_prepare_detour_sig(const sig_result *r, void *detour, size_t stolen)
{
    char line[192];
    void *tramp;
    if (!r || r->addr == 0 || r->status != SIG_OK) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "detour: %s is not resolved; not installed",
                    r && r->name ? r->name : "target");
        backend_log(line);
        return NULL;
    }
    tramp = hook_prepare((void *)r->addr, detour, stolen);
    if (!tramp) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "detour: %s not prepared (%s, %u bytes)",
                    r->name ? r->name : "?", hook_last_failure(), (unsigned)stolen);
        backend_log(line);
    }
    return tramp;
}

/* Scratch-memory patch check. */

/* This stub returns 0x11. Change its immediate to 0x22 and restore it, checking
 * calls as well as bytes. Handwritten code keeps the patch window deterministic. */
static const uint8_t SCRATCH_CODE[] = {
    0xB8, 0x11, 0x00, 0x00, 0x00,   /* mov eax, 0x11 */
    0xC3,                           /* ret */
    0x90, 0x90                      /* pad */
};
#define SCRATCH_PATCH_OFF 1   /* the imm32 starts at byte 1 */

int sh_patch_selftest(void)
{
    typedef int (*scratch_fn)(void);
    static const uint8_t expect[4] = { 0x11, 0x00, 0x00, 0x00 };
    static const uint8_t newbytes[4] = { 0x22, 0x00, 0x00, 0x00 };
    static const uint8_t wrong[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    char line[160];
    const char *why = NULL;
    sh_patch_handle h;
    uint8_t *stub = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
    scratch_fn fn;
    if (!stub) {
        backend_log("patch self-test: FAIL (scratch allocation failed)");
        return 0;
    }
    memcpy(stub, SCRATCH_CODE, sizeof SCRATCH_CODE);
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    fn = (scratch_fn)stub;

    if (fn() != 0x11) why = "the scratch stub does not run";
    else if (code_patch(stub + SCRATCH_PATCH_OFF, expect, newbytes, 4, &h) != B2_PATCH_OK || !h.live)
        why = "a matching patch was not applied";
    else if (fn() != 0x22 || stub[SCRATCH_PATCH_OFF] != 0x22) why = "the patch did not take";
    else if (code_unpatch(&h) != B2_PATCH_OK || h.live) why = "the restore was not applied";
    else if (fn() != 0x11 || stub[SCRATCH_PATCH_OFF] != 0x11) why = "the restore did not take";
    else if (code_patch(stub + SCRATCH_PATCH_OFF, wrong, newbytes, 4, &h) != B2_PATCH_REFUSED_VERIFY ||
             h.live || stub[SCRATCH_PATCH_OFF] != 0x11 || fn() != 0x11)
        why = "a mismatched patch was not refused";

    VirtualFree(stub, 0, MEM_RELEASE);
    if (!why) {
        backend_log("patch self-test: PASS");
        return 1;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE, "patch self-test: FAIL (%s)", why);
    backend_log(line);
    return 0;
}
