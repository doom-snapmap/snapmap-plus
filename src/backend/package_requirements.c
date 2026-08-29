/* package_requirements.c -- declarative, allowlisted package runtime policy.
 *
 * Downloaded override packages must not become arbitrary console-script
 * launchers. This service accepts only product-audited cvar/value pairs from
 * restart-only package requirement files, waits until the engine has finished
 * startup declaration parsing, then queues the idempotent settings once.
 *
 * The initial allowlist contains the two cut-content blacklist gates. Applying
 * them during load_state==2 has historically made cold boot fragile, so the
 * RUNNING gate is a safety boundary, not a convenience delay. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_log.h"
#include "packages.h"
#include "package_requirements.h"

#define PR_MAX_FILES            64u
#define PR_MAX_FILE_BYTES       (64u * 1024u)
#define PR_MAX_TOTAL_BYTES      (256u * 1024u)
#define PR_LOAD_STATE_RVA       0x6dde198u
#define PR_LOAD_STATE_RUNNING   3

enum {
    PR_STATE_NEW = 0,
    PR_STATE_INSTALLING,
    PR_STATE_ARMED,
    PR_STATE_APPLYING,
    PR_STATE_DONE,
    PR_STATE_FAILED
};

typedef void (*pr_buffer_command_fn)(void *cmdsys, const char *text);
typedef void (*pr_execute_buffer_fn)(void *cmdsys);

typedef struct pr_file {
    char path[MAX_PATH];
    char name[MAX_PATH];
} pr_file;

typedef struct pr_allowed {
    const char *name;
    const char *value;
    int requested;
} pr_allowed;

/* Deliberately tiny. Expanding this table is a product/security decision. */
static pr_allowed g_allowed[] = {
    { "g_useImageBlackList", "0", 0 },
    { "g_useResourceBlackList", "0", 0 }
};

static volatile LONG g_state = PR_STATE_NEW;
static const uint8_t *g_module_base;
static void *g_cmdsys;
static pr_buffer_command_fn g_buffer_command;
static size_t g_requirement_count;
static size_t g_manifest_count;

/* Re-capture and re-apply the package requirements after a mid-session install.
 *
 * WHY THIS IS REQUIRED, NOT OPTIONAL. The cut-content blacklist matcher refuses a name BEFORE
 * the type parser ever sees it, so a package made of cut content (the Cyberdemon is listed in
 * both blacklists) simply does not load while the gates are up. Measured 2026-08-27: with the
 * gates still up, a runtime registration pass refused 296 of 296 candidates -- one md6Def's
 * native DeclFind returned null and the service treats that as terminal. Applying the two
 * cvars first took REFUSED to 0. Registering identities whose bytes the blacklist will refuse
 * is worse than not registering them at all.
 *
 * Unlike the override/decl tables, nothing here is memory the engine retains -- these are
 * queued cvar commands -- so the state can simply be reset rather than retired.
 *
 * Reuses the cmd-system pointers captured at install. Returns 1 when the settings are live. */
int sh_package_requirements_rearm(const char *data_root, void *execute_command_buffer,
                                  int user_layer_enabled)
{
    if (!g_cmdsys || !g_buffer_command) {
        backend_log("package-requirements RE-ARM refused: the command system was never captured");
        return 0;
    }
    g_requirement_count = 0;
    g_manifest_count = 0;
    InterlockedExchange(&g_state, PR_STATE_NEW);
    if (!sh_package_requirements_install(data_root, g_module_base, g_cmdsys,
                                         (void *)g_buffer_command, user_layer_enabled))
        return 0;
    return sh_package_requirements_apply_now(execute_command_buffer);
}

#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
static volatile int *g_test_load_state;
#endif

static int pr_fail(const char *reason)
{
    char line[512];
    size_t i;
    for (i = 0; i < sizeof(g_allowed) / sizeof(g_allowed[0]); i++)
        g_allowed[i].requested = 0;
    g_requirement_count = 0;
    g_manifest_count = 0;
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "package-requirements REFUSED: %s; zero settings admitted",
                reason ? reason : "unknown failure");
    backend_log(line);
    InterlockedExchange(&g_state, PR_STATE_FAILED);
    return 0;
}

static int pr_has_suffix_ci(const char *value, const char *suffix)
{
    size_t vl, sl;
    if (!value || !suffix) return 0;
    vl = strlen(value);
    sl = strlen(suffix);
    return vl >= sl && _stricmp(value + vl - sl, suffix) == 0;
}

static int __cdecl pr_file_qsort(const void *left, const void *right)
{
    const pr_file *a = (const pr_file *)left;
    const pr_file *b = (const pr_file *)right;
    int c = strcmp(a->name, b->name);
    return c ? c : strcmp(a->path, b->path);
}

static unsigned char *pr_read_regular_file(const char *path, size_t *length)
{
    HANDLE file;
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    DWORD got = 0;
    unsigned char *body;
    *length = 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                       FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (unsigned long long)size.QuadPart > PR_MAX_FILE_BYTES) {
        CloseHandle(file);
        return NULL;
    }
    body = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)size.QuadPart + 1u);
    if (!body) {
        CloseHandle(file);
        return NULL;
    }
    if (size.QuadPart &&
        (!ReadFile(file, body, (DWORD)size.QuadPart, &got, NULL) ||
         got != (DWORD)size.QuadPart)) {
        HeapFree(GetProcessHeap(), 0, body);
        CloseHandle(file);
        return NULL;
    }
    CloseHandle(file);
    body[(size_t)size.QuadPart] = '\0';
    *length = (size_t)size.QuadPart;
    return body;
}

static int pr_admit(const char *kind, const char *name, const char *value)
{
    size_t i;
    if (strcmp(kind, "cvar") != 0) return 0;
    for (i = 0; i < sizeof(g_allowed) / sizeof(g_allowed[0]); i++) {
        if (strcmp(name, g_allowed[i].name) != 0) continue;
        if (strcmp(value, g_allowed[i].value) != 0) return 0;
        if (!g_allowed[i].requested) {
            g_allowed[i].requested = 1;
            g_requirement_count++;
        }
        /* Several packages may need the same setting -- the Cyberdemon and any
         * other cut-content package both need the blacklists off. The first
         * request queues the command and the rest compose into it, so the
         * command is issued exactly once and no package has to know about the
         * others. A DIFFERENT value for an allowlisted name is refused above
         * rather than resolved by ordering. */
        return 1;
    }
    return 0;
}

static int pr_parse_file(unsigned char *body, size_t length)
{
    size_t position = 0;
    while (position < length) {
        size_t start = position, end, i;
        char *line, *tab1, *tab2;
        while (position < length && body[position] != '\n') position++;
        end = position;
        if (position < length) position++;
        if (end > start && body[end - 1] == '\r') end--;
        for (i = start; i < end; i++) {
            unsigned char c = body[i];
            if (c != '\t' && (c < 0x20u || c > 0x7eu)) return 0;
        }
        body[end] = '\0';
        line = (char *)(body + start);
        if (!line[0] || line[0] == '#') continue;
        tab1 = strchr(line, '\t');
        if (!tab1) return 0;
        tab2 = strchr(tab1 + 1, '\t');
        if (!tab2 || strchr(tab2 + 1, '\t')) return 0;
        *tab1 = '\0';
        *tab2 = '\0';
        if (!line[0] || !tab1[1] || !tab2[1] ||
            !pr_admit(line, tab1 + 1, tab2 + 1)) return 0;
    }
    return 1;
}

/* The package set is 26 KB and these captures already carry large frames, so it
 * lives in static storage rather than on the stack: putting it on the stack
 * tripped the /GS guard and terminated DOOM with 0xC0000409. Each capture is a
 * guarded one-shot on a single thread, so a shared buffer is safe here. */
static sh_package g_packages[SH_PACKAGES_MAX];

static int pr_capture(const char *data_root)
{
    char directory[MAX_PATH], pattern[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA root_info;
    WIN32_FIND_DATAA found;
    HANDLE search;
    pr_file files[PR_MAX_FILES];
    size_t count = 0, total = 0, i;
    DWORD error;
    size_t package_count = 0, package_index;

    if (!data_root || !data_root[0])
        return pr_fail("requirements root path is invalid or too long");
    /* Every installed package may request its own restart-only settings; they
     * are gathered into one set and still pass the same allowlist. */
    if (!sh_packages_enumerate(data_root, g_packages, SH_PACKAGES_MAX, &package_count))
        return pr_fail("the overrides package directory could not be enumerated completely");

    for (package_index = 0; package_index < package_count; package_index++) {
        if (!sh_package_subdir(&g_packages[package_index], "requirements",
                               directory, sizeof(directory)))
            return pr_fail("a package requirements path exceeded its bounded length");

        if (!GetFileAttributesExA(directory, GetFileExInfoStandard, &root_info)) {
            error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                continue;                   /* a package may request nothing */
            return pr_fail("requirements directory metadata read failed");
        }
        if (!(root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return pr_fail("requirements root is not a regular directory");
        if (_snprintf_s(pattern, sizeof(pattern), _TRUNCATE,
                        "%s\\*.requirements", directory) < 0)
            return pr_fail("requirements search path is too long");

        search = FindFirstFileA(pattern, &found);
        if (search == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) continue;
            return pr_fail("requirements enumeration could not start");
        }
        for (;;) {
            if ((found.dwFileAttributes &
                 (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !pr_has_suffix_ci(found.cFileName, ".requirements")) {
                FindClose(search);
                return pr_fail("requirements enumeration found a non-regular entry");
            }
            if (count >= PR_MAX_FILES ||
                _snprintf_s(files[count].path, sizeof(files[count].path), _TRUNCATE,
                            "%s\\%s", directory, found.cFileName) < 0) {
                FindClose(search);
                return pr_fail("requirements file count or path limit exceeded");
            }
            strncpy_s(files[count].name, sizeof(files[count].name),
                      found.cFileName, _TRUNCATE);
            count++;
            if (!FindNextFileA(search, &found)) break;
        }
        error = GetLastError();
        FindClose(search);
        if (error != ERROR_NO_MORE_FILES)
            return pr_fail("requirements enumeration ended unexpectedly");
    }

    if (!count) {
        backend_log("package-requirements idle: no *.requirements files");
        InterlockedExchange(&g_state, PR_STATE_DONE);
        return 1;
    }
    qsort(files, count, sizeof(files[0]), pr_file_qsort);
    for (i = 0; i < count; i++) {
        size_t length = 0;
        unsigned char *body = pr_read_regular_file(files[i].path, &length);
        if (!body || total > PR_MAX_TOTAL_BYTES - length) {
            if (body) HeapFree(GetProcessHeap(), 0, body);
            return pr_fail("requirements file read or total byte limit failed");
        }
        total += length;
        if (!pr_parse_file(body, length)) {
            char detail[MAX_PATH + 128];
            HeapFree(GetProcessHeap(), 0, body);
            /* With several packages installed, "a bad row" without the file is
             * not actionable: the author has to be told which package asked. */
            _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                        "unsupported or malformed requirement row in %s",
                        files[i].path);
            return pr_fail(detail);
        }
        HeapFree(GetProcessHeap(), 0, body);
    }
    g_manifest_count = count;
    if (!g_requirement_count) {
        backend_log("package-requirements idle: manifests contain no settings");
        InterlockedExchange(&g_state, PR_STATE_DONE);
        return 1;
    }
    return 1;
}

static int pr_read_load_state(int *value)
{
#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
    if (g_test_load_state) {
        *value = *g_test_load_state;
        return 1;
    }
#endif
    if (!g_module_base) return 0;
    __try {
        *value = *(const volatile int *)(g_module_base + PR_LOAD_STATE_RVA);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int sh_package_requirements_install(const char *data_root,
                                    const uint8_t *module_base,
                                    void *cmdsys,
                                    void *buffer_command,
                                    int user_layer_enabled)
{
    char line[256];
    /* Stash the engine pointers FIRST, before any early return can skip them. A launch whose
     * packages carry no requirements still needs them retained, because a package installed
     * LATER will -- and sh_package_requirements_rearm has no other way to reach the command
     * system. Assigning these only on the success path made the runtime re-arm refuse with
     * "the command system was never captured". */
    if (module_base) g_module_base = module_base;
    if (cmdsys) g_cmdsys = cmdsys;
    if (buffer_command) g_buffer_command = (pr_buffer_command_fn)buffer_command;
    if (InterlockedCompareExchange(&g_state, PR_STATE_INSTALLING, PR_STATE_NEW) != PR_STATE_NEW)
        return 0;
    if (!user_layer_enabled) {
        backend_log("package-requirements disabled for this launch with the user override layer");
        InterlockedExchange(&g_state, PR_STATE_DONE);
        return 1;
    }
    if (!pr_capture(data_root)) return 0;
    if (InterlockedCompareExchange(&g_state, PR_STATE_DONE, PR_STATE_DONE) == PR_STATE_DONE)
        return 1;
#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
    if ((!module_base && !g_test_load_state) || !cmdsys || !buffer_command)
#else
    if (!module_base || !cmdsys || !buffer_command)
#endif
        return pr_fail("command-system or load-state dependency missing");

    g_module_base = module_base;
    g_cmdsys = cmdsys;
    g_buffer_command = (pr_buffer_command_fn)buffer_command;
    InterlockedExchange(&g_state, PR_STATE_ARMED);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "package-requirements captured: %zu file(s), %zu safe cvar(s); waiting for load-state RUNNING",
                g_manifest_count, g_requirement_count);
    backend_log(line);
    sh_package_requirements_poll();
    return 1;
}

/* Build the admitted command text, buffer it, and -- when `execute` is supplied -- drain the engine
 * command buffer so the values are live before the caller's very next engine call. The claim on
 * ARMED is a CAS, so whichever entry point runs first wins and the other becomes a no-op. */
static void pr_apply(pr_execute_buffer_fn execute, const char *when)
{
    char command[160] = "";
    char line[224];
    size_t used = 0, i;
    int queued = 0;

    if (InterlockedCompareExchange(&g_state, PR_STATE_APPLYING, PR_STATE_ARMED) != PR_STATE_ARMED)
        return;
    for (i = 0; i < sizeof(g_allowed) / sizeof(g_allowed[0]); i++) {
        int n;
        if (!g_allowed[i].requested) continue;
        n = _snprintf_s(command + used, sizeof(command) - used, _TRUNCATE,
                        "%s %s\n", g_allowed[i].name, g_allowed[i].value);
        if (n < 0) {
            pr_fail("admitted command buffer exceeded its fixed bound");
            return;
        }
        used += (size_t)n;
    }
    __try {
        g_buffer_command(g_cmdsys, command);
        /* Buffering only appends text. Nothing drains the command buffer between the decl server's
         * publication point inside idCommonLocal::Init and the engine's whole-registry promotion, so
         * that path must run the engine's own drain itself or the gates land far too late. */
        if (execute) execute(g_cmdsys);
        queued = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        queued = 0;
    }
    if (!queued) {
        pr_fail("safe cvar command enqueue failed");
        return;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "package-requirements applied: %zu safe cvar(s) %s once %s",
                g_requirement_count, execute ? "executed" : "queued", when);
    backend_log(line);
    InterlockedExchange(&g_state, PR_STATE_DONE);
}

void sh_package_requirements_poll(void)
{
    int load_state = -1;
    if (InterlockedCompareExchange(&g_state, PR_STATE_ARMED, PR_STATE_ARMED) != PR_STATE_ARMED)
        return;
    if (!pr_read_load_state(&load_state) || load_state != PR_LOAD_STATE_RUNNING)
        return;
    pr_apply(NULL, "at load-state RUNNING");
}

/* THE STARTUP-FRAGILITY BOUNDARY, RESTATED. The RUNNING gate above exists because applying the
 * blacklist gates while the engine was still parsing startup declarations made cold boot fragile.
 * That hazard is about being MID-PARSE, not about the load-state number.
 *
 * The window this entry point is called from is quiescent by measurement, not by assumption. A live
 * capture (Frida interceptor on the promotion, 2026-08-26) timed the boot: the last of the engine's
 * 64 startup decl loads served through our own provider completed 3.637s BEFORE the engine's
 * whole-registry resource promotion (0x1801830, called from idCommonLocal::Init at 0x17C6479), and
 * load-state RUNNING arrived 0.821s AFTER it. The decl server publishes in that gap. Startup decl
 * parsing is over; nothing else is in flight.
 *
 * Applying here is also the only ordering that still means anything: the gates are read by the
 * blacklist matcher (0x31D0B0) inside idResourceList::LoadResource (0x1801380) and the static
 * resource-handle resolve (0x18008F0), both of which refuse a blacklisted name before the type
 * parser ever sees it. If they are not live before the decl server publishes, cut content does not
 * load at all.
 *
 * `execute_command_buffer` is the engine's own drain (CmdExecuteBuffer, 0x1AA46B0). Passing NULL
 * degrades this to a plain buffer, which is exactly the pre-existing RUNNING behaviour. */
int sh_package_requirements_apply_now(void *execute_command_buffer)
{
    LONG state = InterlockedCompareExchange(&g_state, PR_STATE_ARMED, PR_STATE_ARMED);
    if (state == PR_STATE_DONE) return 1;
    if (state != PR_STATE_ARMED) return 0;
    pr_apply((pr_execute_buffer_fn)execute_command_buffer, "before the engine boot promotion");
    return InterlockedCompareExchange(&g_state, PR_STATE_DONE, PR_STATE_DONE) == PR_STATE_DONE;
}

#ifdef SH_PACKAGE_REQUIREMENTS_TESTING
void sh_package_requirements_test_reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(g_allowed) / sizeof(g_allowed[0]); i++)
        g_allowed[i].requested = 0;
    g_module_base = NULL;
    g_cmdsys = NULL;
    g_buffer_command = NULL;
    g_test_load_state = NULL;
    g_requirement_count = 0;
    g_manifest_count = 0;
    InterlockedExchange(&g_state, PR_STATE_NEW);
}

void sh_package_requirements_test_set_load_state(volatile int *state)
{
    g_test_load_state = state;
}

size_t sh_package_requirements_test_count(void)
{
    return g_requirement_count;
}
#endif
