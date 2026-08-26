/* decl_server.c -- see decl_server.h.
 *
 * The ordinary overrides layer is demand-driven: it can replace bytes only
 * after DOOM asks for an already-registered source path. This service handles
 * the complementary case. At startup it snapshots user files under
 * overrides/generated/decls plus installed-resource links admitted by
 * resource_bridge.c and derives each decl's type and logical name. It registers
 * one private command immediately, but does not queue it until the engine has
 * reached load_state RUNNING. DOOM drains that command on its
 * main thread, where we use the engine's own decl-registry virtual methods:
 *
 *   +0x38  Register and scan one decl source file (bool(registry, const idStr *, default type))
 *   +0x58  Find decl type by short name
 *
 * An existing logical identity is never replaced here; it remains a normal
 * file-shadow override. The main-thread command classifies every candidate
 * source-first with DeclSourceFind and only falls back to lookup-only DeclFind,
 * publishes only absent identities as an immutable exact decltree table through
 * the already-installed override provider, and asks DOOM to scan each source
 * once in dependency order. After every scan
 * succeeds, genuinely new snapEditorEntityDef identities are materialized with
 * the native make-default DeclFind path so the live palette manager can see
 * them. The snapshot and table are process-lifetime and one-shot: no watcher,
 * hot reload, retry, or unload.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_log.h"
#include "decl_server.h"
#include "hook.h"
#include "package_requirements.h"
#include "decl_server_path.h"
#include "decl_text.h"
#include "overrides.h"
#include "palette_refresh.h"
#include "decl_visibility.h"
#include "packages.h"
#include "resource_bridge.h"
#include "user_overrides.h"

#define DS_INTERNAL_COMMAND     "snapmap_plus_decl_server_apply"
#define DS_MAX_CANDIDATES       512
#define DS_MAX_DISCOVERED       (DS_MAX_CANDIDATES * 8)
#define DS_MAX_DEPTH            16
#define DS_MAX_FILE_BYTES       (1024u * 1024u)
#define DS_MAX_TOTAL_BYTES      (16u * 1024u * 1024u)
#define DS_REGISTRY_REGISTER_FILE_SLOT 0x38u
#define DS_REGISTRY_TYPE_SLOT          0x58u
#define DS_IDSTR_SIZE          0x30u
#define DS_ANCHOR_MOV_OFFSET    0x10u
#define DS_ANCHOR_MOV_LENGTH    7u
#define DS_PINNED_ANCHOR_RVA    0x184E1D0u
#define DS_PINNED_TYPE_RVA      0x17B43B0u
#define DS_PINNED_REGISTER_RVA  0x17B7330u
#define DS_PINNED_FIND_RVA      0x17B36F0u
#define DS_PINNED_SOURCE_FIND_RVA 0x17B34B0u
#define DS_PINNED_IDSTR_CTOR_RVA 0x19FCEF0u
#define DS_PINNED_IDSTR_DTOR_RVA 0x19FD120u
/* The engine's whole-registry resource promotion, and the engine's own command-buffer drain. See
 * "publishing into the engine's boot snapshot" below for why the decl server hooks the first and
 * calls the second. */
#define DS_PINNED_BOOT_PROMOTE_RVA 0x1801830u
#define DS_PINNED_CMD_EXECUTE_RVA  0x1AA46B0u
/* The resource level the map-transition purge tests, and the value that escapes it. This service
 * READS these and never writes them: the whole point of publishing before the engine's promotion is
 * that the engine does the writing. The read exists so a live run can PROVE the ordering worked
 * instead of asserting it -- see ds_report_promotion_outcome. */
#define DS_RESOURCE_LEVEL_OFFSET   0x28u
#define DS_RESOURCE_LEVEL_STATIC   4u
/* The promotion's first three instructions -- push rbx / sub rsp,0x30 / mov qword [rsp+0x20],-2 --
 * are 15 bytes of whole, position-independent code with no rip-relative operand and no relative
 * branch, so they are safe to steal for the 14-byte absolute jump. The next instruction is a rel32
 * call and must not be touched. */
#define DS_BOOT_PROMOTE_STOLEN_BYTES 15u
#define DS_DECL_STATE_OFFSET    0x2cu
#define DS_DECL_IN_PROGRESS    0x01u
#define DS_DECL_ENTITYDEF_OFFSET 0x1c8u

enum {
    DS_STATE_NEW = 0,
    DS_STATE_INSTALLING,
    DS_STATE_ARMED,
    DS_STATE_QUEUED,
    DS_STATE_APPLYING,
    DS_STATE_DONE,
    DS_STATE_FAILED
};

typedef void (*add_command_fn)(void *cmdsys, const char *name, void *handler,
                               const char *help, void *arg_comp, unsigned int flags);
typedef void *(*decl_type_by_name_fn)(void *registry, const char *short_name);
typedef unsigned char (*decl_register_file_fn)(void *registry, const void *source_name,
                                               void *default_type_manager);
typedef void *(*idstr_ctor_fn)(void *self, const char *source);
typedef void (*idstr_dtor_fn)(void *self);
typedef void *(__fastcall *decl_source_find_fn)(void *type_manager,
                                                const char *logical_name);
typedef void *(*decl_find_fn)(void *type_manager, const char *logical_name,
                              unsigned char make_default);
typedef int (*decl_palette_refresh_fn)(void);
typedef void (*resource_promote_static_fn)(void);
typedef void (*cmd_execute_buffer_fn)(void *cmdsys);
typedef HANDLE (WINAPI *ds_find_first_fn)(LPCSTR pattern, LPWIN32_FIND_DATAA found);
typedef BOOL (WINAPI *ds_find_next_fn)(HANDLE search, LPWIN32_FIND_DATAA found);
typedef BOOL (WINAPI *ds_find_close_fn)(HANDLE search);
typedef DWORD (WINAPI *ds_get_last_error_fn)(void);

typedef struct ds_candidate {
    char type[SH_DECL_SERVER_TYPE_CAP];
    char name[SH_DECL_SERVER_NAME_CAP];
    char source[SH_DECL_SERVER_SOURCE_CAP];
    char *body;
    size_t body_length;
    int outcome;
    int shadow_kind;
} ds_candidate;

enum {
    DS_CANDIDATE_UNCLASSIFIED = 0,
    DS_CANDIDATE_MISSING,
    DS_CANDIDATE_SHADOWED,
    DS_CANDIDATE_REFUSED,
    DS_CANDIDATE_NON_PALETTE
};

enum {
    DS_SHADOW_NONE = 0,
    DS_SHADOW_SOURCE,
    DS_SHADOW_LIVE
};

enum {
    DS_CLASSIFY_MISSING = 1,
    DS_CLASSIFY_SHADOWED_SOURCE,
    DS_CLASSIFY_SHADOWED_LIVE,
    DS_CLASSIFY_REFUSED_TYPE,
    DS_CLASSIFY_TERMINAL
};

enum {
    DS_LOOKUP_TYPE = 1,
    DS_LOOKUP_SOURCE,
    DS_LOOKUP_LIVE
};

/* A source record is the engine's authoritative existence signal. DeclFind
 * with makeDefault=0 is only a fallback for a live decl that predates (or was
 * created independently of) its source record. Keep both calls in this small
 * helper so the source-first order is exercised by production and tests alike. */
static int ds_classify_candidate(const ds_candidate *candidate, void *registry,
                                 decl_type_by_name_fn type_by_name,
                                 decl_source_find_fn source_find,
                                 decl_find_fn find_decl, const char **reason)
{
    void *type_manager = NULL;
    void *source_record = NULL;
    void *live_decl = NULL;
    int lookup_stage = DS_LOOKUP_TYPE;
    int fault = 0;

    if (reason) *reason = NULL;
    if (!candidate || !registry || !type_by_name || !source_find || !find_decl) {
        if (reason) *reason = "source-first classification ABI was unavailable";
        return DS_CLASSIFY_TERMINAL;
    }

    __try {
        type_manager = type_by_name(registry, candidate->type);
        if (type_manager) {
            lookup_stage = DS_LOOKUP_SOURCE;
            source_record = source_find(type_manager, candidate->name);
            if (!source_record) {
                lookup_stage = DS_LOOKUP_LIVE;
                live_decl = find_decl(type_manager, candidate->name, 0);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fault = 1;
    }

    if (fault) {
        if (reason) {
            if (lookup_stage == DS_LOOKUP_SOURCE)
                *reason = "engine exception during native source-record lookup";
            else if (lookup_stage == DS_LOOKUP_LIVE)
                *reason = "engine exception during live decl lookup";
            else
                *reason = "engine exception during decl type lookup";
        }
        return DS_CLASSIFY_TERMINAL;
    }
    if (!type_manager) {
        if (reason) *reason = "unknown or unsupported decl type directory";
        return DS_CLASSIFY_REFUSED_TYPE;
    }
    if (source_record) {
        if (reason) *reason = "native source record already exists";
        return DS_CLASSIFY_SHADOWED_SOURCE;
    }
    if (live_decl) {
        if (reason) *reason = "live decl object already exists without a source record";
        return DS_CLASSIFY_SHADOWED_LIVE;
    }
    return DS_CLASSIFY_MISSING;
}

typedef struct ds_discovered {
    char absolute[MAX_PATH];
    char type[SH_DECL_SERVER_TYPE_CAP];
    char name[SH_DECL_SERVER_NAME_CAP];
    char source[SH_DECL_SERVER_SOURCE_CAP];
    /* Which package published this. With several installed, a bare identity in
     * a refusal message is not actionable -- the author needs to be told which
     * two packages disagree. */
    char package[SH_PACKAGE_NAME_CAP];
    size_t linked_index;
    int linked;
} ds_discovered;

typedef struct ds_discovery {
    ds_discovered *items;
    size_t count;
    size_t capacity;
} ds_discovery;

enum {
    DS_ENUM_ERROR = -1,
    DS_ENUM_DONE = 0,
    DS_ENUM_FOUND = 1
};

static volatile LONG g_state = DS_STATE_NEW;
static volatile LONG g_registration_succeeded = 0;
static ds_candidate *g_candidates;
static int g_candidate_count;
static int g_capture_refused;
static size_t g_total_bytes;
static void *g_cmdsys;
static add_command_fn g_add_command;
static const uint8_t *g_module_base;
static const uint8_t *g_registry_anchor;
static decl_type_by_name_fn g_expected_type_by_name;
static decl_register_file_fn g_expected_register_file;
static idstr_ctor_fn g_idstr_ctor;
static idstr_dtor_fn g_idstr_dtor;
static decl_source_find_fn g_find_source;
static decl_find_fn g_find_decl;
static cmd_execute_buffer_fn g_execute_commands;
static resource_promote_static_fn g_boot_promotion_original;
static volatile LONG g_boot_promotion_entered;
static char g_probe_type[SH_DECL_SERVER_TYPE_CAP];
static char g_probe_name[SH_DECL_SERVER_NAME_CAP];
static ds_find_first_fn g_find_first = FindFirstFileA;
static ds_find_next_fn g_find_next = FindNextFileA;
static ds_find_close_fn g_find_close = FindClose;
static ds_get_last_error_fn g_get_last_error = GetLastError;

static void ds_log(const char *status, const char *subject, const char *reason)
{
    char line[512];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "decl-server %s: '%s'%s%s",
                status ? status : "INFO", subject ? subject : "",
                reason && reason[0] ? " -- " : "", reason ? reason : "");
    backend_log(line);
}

static void ds_log_win32_refusal(const char *subject, const char *operation, DWORD error)
{
    char reason[128];
    _snprintf_s(reason, sizeof(reason), _TRUNCATE,
                "%s failed with Win32 error %lu", operation, (unsigned long)error);
    ds_log("REFUSED", subject, reason);
}

static int ds_find_first_status(HANDLE search, DWORD error)
{
    if (search != INVALID_HANDLE_VALUE) return DS_ENUM_FOUND;
    return error == ERROR_FILE_NOT_FOUND ? DS_ENUM_DONE : DS_ENUM_ERROR;
}

static int ds_find_next_status(BOOL found, DWORD error)
{
    if (found) return DS_ENUM_FOUND;
    return error == ERROR_NO_MORE_FILES ? DS_ENUM_DONE : DS_ENUM_ERROR;
}

static int ds_root_attributes_status(DWORD attributes, DWORD error)
{
    if (attributes != INVALID_FILE_ATTRIBUTES) return DS_ENUM_FOUND;
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return DS_ENUM_DONE;
    return DS_ENUM_ERROR;
}

#ifdef SH_DECL_SERVER_TESTING
void sh_decl_server_test_set_find_api(const sh_decl_server_test_find_api *api)
{
    if (!api || !api->find_first || !api->find_next || !api->find_close ||
        !api->get_last_error) return;
    g_find_first = api->find_first;
    g_find_next = api->find_next;
    g_find_close = api->find_close;
    g_get_last_error = api->get_last_error;
}

void sh_decl_server_test_reset_find_api(void)
{
    g_find_first = FindFirstFileA;
    g_find_next = FindNextFileA;
    g_find_close = FindClose;
    g_get_last_error = GetLastError;
}

int sh_decl_server_test_find_first_status(int found, unsigned long error)
{
    HANDLE search = found ? (HANDLE)(uintptr_t)1 : INVALID_HANDLE_VALUE;
    return ds_find_first_status(search, (DWORD)error);
}

int sh_decl_server_test_find_next_status(int found, unsigned long error)
{
    return ds_find_next_status(found ? TRUE : FALSE, (DWORD)error);
}

int sh_decl_server_test_root_attributes_status(int found, unsigned long error)
{
    return ds_root_attributes_status(found ? FILE_ATTRIBUTE_DIRECTORY : INVALID_FILE_ATTRIBUTES,
                                     (DWORD)error);
}
#endif

static int ds_safe_read(const void *source, void *destination, size_t length)
{
    __try {
        memcpy(destination, source, length);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int ds_ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

/* A candidate body must be exactly one top-level brace block. Whitespace and
 * comments may surround it, but a second block or any other outer token is
 * refused so a candidate cannot inject another type/name header into the
 * mixed source. */
static int ds_decl_body_is_single_block(const unsigned char *text, size_t length)
{
    size_t i = 0;
    size_t depth = 0;
    int root_seen = 0;
    int root_closed = 0;
    int in_quote = 0;
    int escaped = 0;
    int line_comment = 0;
    int block_comment = 0;

    if (!sh_decl_text_well_formed(text, length)) return 0;
    while (i < length) {
        unsigned char c = text[i];
        unsigned char next = i + 1 < length ? text[i + 1] : 0;
        if (line_comment) {
            if (c == '\n') line_comment = 0;
            i++;
            continue;
        }
        if (block_comment) {
            if (c == '*' && next == '/') {
                block_comment = 0;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (in_quote) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') in_quote = 0;
            i++;
            continue;
        }
        if (c == '/' && next == '/') {
            line_comment = 1;
            i += 2;
            continue;
        }
        if (c == '/' && next == '*') {
            block_comment = 1;
            i += 2;
            continue;
        }
        if (depth == 0) {
            if (ds_ascii_space(c)) {
                i++;
                continue;
            }
            if (c != '{' || root_seen) return 0;
            root_seen = 1;
            depth = 1;
            i++;
            continue;
        }
        if (c == '"') in_quote = 1;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) root_closed = 1;
        }
        i++;
    }
    return root_seen && root_closed && depth == 0 && !in_quote &&
           !escaped && !block_comment;
}

static int ds_checked_add(size_t *value, size_t add)
{
    if (!value || *value > SIZE_MAX - add) return 0;
    *value += add;
    return 1;
}

#ifdef SH_DECL_SERVER_TESTING
int sh_decl_server_test_body_is_single_block(const unsigned char *body, size_t length)
{
    return ds_decl_body_is_single_block(body, length);
}

int sh_decl_server_test_checked_add(size_t value, size_t add, size_t *result)
{
    if (!result || !ds_checked_add(&value, add)) return 0;
    *result = value;
    return 1;
}

int sh_decl_server_test_classify_candidate(
    const char *type, const char *name, void *registry,
    sh_decl_server_test_type_by_name_fn type_by_name,
    sh_decl_server_test_source_find_fn source_find,
    sh_decl_server_test_find_decl_fn find_decl)
{
    ds_candidate candidate;
    const char *reason = NULL;

    memset(&candidate, 0, sizeof(candidate));
    if (!type || !name ||
        strcpy_s(candidate.type, sizeof(candidate.type), type) != 0 ||
        strcpy_s(candidate.name, sizeof(candidate.name), name) != 0)
        return DS_CLASSIFY_TERMINAL;
    return ds_classify_candidate(&candidate, registry,
                                 (decl_type_by_name_fn)type_by_name,
                                 (decl_source_find_fn)source_find,
                                 (decl_find_fn)find_decl, &reason);
}

#endif

static const sig_result *ds_result(const sig_result *results, size_t count, const char *name)
{
    size_t i;
    if (!results || !name) return NULL;
    for (i = 0; i < count; i++)
        if (results[i].name && strcmp(results[i].name, name) == 0) return &results[i];
    return NULL;
}

/* This service calls these engine functions directly. The hook-tolerant
 * known-RVA fallback is useful for transparent detours elsewhere, but it is
 * not a clean ABI proof for a one-shot native source publication. */
static uintptr_t ds_clean_addr(const sig_result *results, size_t count,
                               const char *name)
{
    const sig_result *result = ds_result(results, count, name);
    return result && result->status == SIG_OK ? result->addr : 0;
}

/* This service also reads a pinned load-state data RVA and decodes a pinned
 * registry layout. Clean signatures prove call targets, but not that those
 * build-specific data/layout assumptions match. Require every independent
 * anchor to occupy the audited Steam-build RVA before arming. */
static int ds_clean_at_pinned_rva(const sig_result *results, size_t count,
                                  const char *name, const uint8_t *module_base,
                                  uintptr_t expected_rva)
{
    const sig_result *result = ds_result(results, count, name);
    return result && result->status == SIG_OK && result->addr && module_base &&
           result->rva == expected_rva &&
           result->addr == (uintptr_t)module_base + expected_rva;
}

static void ds_free_candidates(void)
{
    int i;
    if (g_candidates) {
        for (i = 0; i < g_candidate_count; i++) {
            if (g_candidates[i].body) HeapFree(GetProcessHeap(), 0, g_candidates[i].body);
            g_candidates[i].body = NULL;
        }
        HeapFree(GetProcessHeap(), 0, g_candidates);
    }
    g_candidates = NULL;
    g_candidate_count = 0;
    g_total_bytes = 0;
}

static void ds_free_discovery(ds_discovery *discovery)
{
    if (!discovery) return;
    if (discovery->items) HeapFree(GetProcessHeap(), 0, discovery->items);
    discovery->items = NULL;
    discovery->count = 0;
    discovery->capacity = 0;
}

static int ds_grow_discovery(ds_discovery *discovery)
{
    size_t next_capacity;
    size_t bytes;
    ds_discovered *grown;
    if (!discovery || discovery->count >= DS_MAX_DISCOVERED) return 0;
    next_capacity = discovery->capacity ? discovery->capacity * 2u : 64u;
    if (next_capacity > DS_MAX_DISCOVERED) next_capacity = DS_MAX_DISCOVERED;
    bytes = next_capacity * sizeof(discovery->items[0]);
    if (discovery->items) {
        grown = (ds_discovered *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             discovery->items, bytes);
    } else {
        grown = (ds_discovered *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    }
    if (!grown) return 0;
    discovery->items = grown;
    discovery->capacity = next_capacity;
    return 1;
}

static char *ds_read_file(const char *path, size_t *out_length, const char **reason)
{
    HANDLE file;
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    DWORD got = 0;
    char *body;

    *out_length = 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                       FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        *reason = "open failed";
        return NULL;
    }
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(file);
        *reason = "directory or reparse-point file refused";
        return NULL;
    }
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (LONGLONG)DS_MAX_FILE_BYTES) {
        CloseHandle(file);
        *reason = "file is empty or exceeds the 1 MiB cap";
        return NULL;
    }
    if (g_total_bytes + (size_t)size.QuadPart > DS_MAX_TOTAL_BYTES) {
        CloseHandle(file);
        *reason = "launch snapshot exceeds the 16 MiB total cap";
        return NULL;
    }
    body = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)size.QuadPart + 1);
    if (!body) {
        CloseHandle(file);
        *reason = "allocation failed";
        return NULL;
    }
    if (!ReadFile(file, body, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart) {
        HeapFree(GetProcessHeap(), 0, body);
        CloseHandle(file);
        *reason = "short read";
        return NULL;
    }
    CloseHandle(file);
    *out_length = (size_t)size.QuadPart;

    /* The engine lexer expects text, not a transport marker. Normalize a UTF-8
     * BOM at the boundary; all other bytes remain exact. */
    if (*out_length >= 3 && (unsigned char)body[0] == 0xef &&
        (unsigned char)body[1] == 0xbb && (unsigned char)body[2] == 0xbf) {
        memmove(body, body + 3, *out_length - 3);
        *out_length -= 3;
    }
    body[*out_length] = '\0';
    if (!sh_decl_text_well_formed((const unsigned char *)body, *out_length)) {
        HeapFree(GetProcessHeap(), 0, body);
        *reason = "structural validation failed (NUL, braces, quote, or comment)";
        return NULL;
    }
    if (!ds_decl_body_is_single_block((const unsigned char *)body, *out_length)) {
        HeapFree(GetProcessHeap(), 0, body);
        *reason = "decl body requires exactly one top-level brace block";
        return NULL;
    }
    return body;
}

static char *ds_read_linked(size_t index, size_t *out_length, const char **reason)
{
    char *body = NULL;
    size_t length = 0;
    if (!sh_resource_bridge_read_decl(index, &body, &length, reason) || !body) return NULL;
    if (!length || length > DS_MAX_FILE_BYTES) {
        HeapFree(GetProcessHeap(), 0, body);
        *reason = "linked decl is empty or exceeds the 1 MiB cap";
        return NULL;
    }
    if (g_total_bytes + length > DS_MAX_TOTAL_BYTES) {
        HeapFree(GetProcessHeap(), 0, body);
        *reason = "launch snapshot exceeds the 16 MiB total cap";
        return NULL;
    }
    if (length >= 3 && (unsigned char)body[0] == 0xef &&
        (unsigned char)body[1] == 0xbb && (unsigned char)body[2] == 0xbf) {
        memmove(body, body + 3, length - 3);
        length -= 3;
        body[length] = '\0';
    }
    if (!sh_decl_text_well_formed((const unsigned char *)body, length)) {
        HeapFree(GetProcessHeap(), 0, body);
        *reason = "structural validation failed (NUL, braces, quote, or comment)";
        return NULL;
    }
    if (!ds_decl_body_is_single_block((const unsigned char *)body, length)) {
        HeapFree(GetProcessHeap(), 0, body);
        *reason = "decl body requires exactly one top-level brace block";
        return NULL;
    }
    *out_length = length;
    return body;
}

static int ds_identity_equal(const ds_discovered *item,
                             const char *type, const char *name);

/* Compare two decl files byte for byte in bounded chunks. Two packages shipping
 * the SAME identity with the SAME bytes is a duplicate, not a disagreement: it
 * happens whenever a shared prerequisite is vendored into more than one package,
 * and refusing it would punish the author for being self-contained. Returns 1
 * when both files are readable and identical. */
static int ds_files_identical(const char *left_path, const char *right_path)
{
    HANDLE left = INVALID_HANDLE_VALUE, right = INVALID_HANDLE_VALUE;
    LARGE_INTEGER left_size, right_size;
    unsigned char left_chunk[4096], right_chunk[4096];
    int identical = 0;

    left = CreateFileA(left_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    right = CreateFileA(right_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (left == INVALID_HANDLE_VALUE || right == INVALID_HANDLE_VALUE) goto done;
    if (!GetFileSizeEx(left, &left_size) || !GetFileSizeEx(right, &right_size)) goto done;
    if (left_size.QuadPart != right_size.QuadPart) goto done;
    if (left_size.QuadPart > (LONGLONG)DS_MAX_FILE_BYTES) goto done;
    for (;;) {
        DWORD got_left = 0, got_right = 0;
        if (!ReadFile(left, left_chunk, sizeof(left_chunk), &got_left, NULL) ||
            !ReadFile(right, right_chunk, sizeof(right_chunk), &got_right, NULL)) goto done;
        if (got_left != got_right) goto done;
        if (!got_left) break;
        if (memcmp(left_chunk, right_chunk, got_left) != 0) goto done;
    }
    identical = 1;
done:
    if (right != INVALID_HANDLE_VALUE) CloseHandle(right);
    if (left != INVALID_HANDLE_VALUE) CloseHandle(left);
    return identical;
}

static int ds_discover_one(ds_discovery *discovery, const char *package,
                           const char *absolute_path, const char *relative_path)
{
    ds_discovered *item;
    const char *reason = NULL;
    size_t existing;
    char type[SH_DECL_SERVER_TYPE_CAP];
    char name[SH_DECL_SERVER_NAME_CAP];
    char source[SH_DECL_SERVER_SOURCE_CAP];

    if (!sh_decl_server_identity_from_relative(relative_path,
                                               type, sizeof(type), name, sizeof(name),
                                               source, sizeof(source), &reason)) {
        ds_log("REFUSED", relative_path, reason);
        g_capture_refused++;
        return 1;
    }
    /* Compose an identical twin from another package instead of admitting a
     * second copy: the ambiguity rule downstream would otherwise refuse BOTH
     * and the identity would vanish from a perfectly consistent install. */
    for (existing = 0; existing < discovery->count; existing++) {
        ds_discovered *other = &discovery->items[existing];
        char line[512];
        if (other->linked || !ds_identity_equal(other, type, name)) continue;
        if (!ds_files_identical(other->absolute, absolute_path)) break;
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "decl-server COMPOSED: %s is byte-identical in packages '%s' and "
                    "'%s'; the first copy serves it", source, other->package, package);
        backend_log(line);
        return 1;
    }
    if (discovery->count >= DS_MAX_DISCOVERED) {
        ds_log("REFUSED", source,
               "discovery metadata exceeds the 4096-entry safety cap; whole snapshot refused");
        g_capture_refused++;
        return 0;
    }
    if (discovery->count == discovery->capacity && !ds_grow_discovery(discovery)) {
        ds_log("REFUSED", source, "discovery metadata allocation failed; whole snapshot refused");
        g_capture_refused++;
        return 0;
    }
    item = &discovery->items[discovery->count++];
    strcpy_s(item->absolute, sizeof(item->absolute), absolute_path);
    strcpy_s(item->type, sizeof(item->type), type);
    strcpy_s(item->name, sizeof(item->name), name);
    strcpy_s(item->source, sizeof(item->source), source);
    strcpy_s(item->package, sizeof(item->package), package ? package : "");
    return 1;
}

static int ds_identity_equal(const ds_discovered *item,
                             const char *type, const char *name)
{
    return item && type && name && _stricmp(item->type, type) == 0 &&
           _stricmp(item->name, name) == 0;
}

/* Linked manifests provide type/name strings instead of a path. Run those
 * strings through the same bounded grammar as local generated paths before a
 * candidate enters ordering or table publication. This keeps the provider's
 * exact-key builder from becoming the first validator for a linked row. */
static int ds_validate_identity_parts(const char *type, const char *name,
                                      const char **reason)
{
    char relative[SH_DECL_SERVER_TYPE_CAP + SH_DECL_SERVER_NAME_CAP + 8];
    char parsed_type[SH_DECL_SERVER_TYPE_CAP];
    char parsed_name[SH_DECL_SERVER_NAME_CAP];
    char parsed_source[SH_DECL_SERVER_SOURCE_CAP];
    const char *path_reason = NULL;
    if (reason) *reason = NULL;
    if (!type || !name || !type[0] || !name[0]) {
        if (reason) *reason = "linked decl identity is empty";
        return 0;
    }
    if (_snprintf_s(relative, sizeof(relative), _TRUNCATE, "%s/%s.decl",
                    type, name) < 0) {
        if (reason) *reason = "linked decl identity is too long";
        return 0;
    }
    if (!sh_decl_server_identity_from_relative(
            relative, parsed_type, sizeof(parsed_type), parsed_name,
            sizeof(parsed_name), parsed_source, sizeof(parsed_source),
            &path_reason)) {
        if (reason) *reason = path_reason ? path_reason : "linked decl identity refused";
        return 0;
    }
    if (_stricmp(parsed_type, type) != 0 || _stricmp(parsed_name, name) != 0) {
        if (reason) *reason = "linked decl identity is not canonical";
        return 0;
    }
    return 1;
}

static int ds_discover_linked(ds_discovery *discovery)
{
    size_t count = sh_resource_bridge_decl_count();
    size_t index;
    for (index = 0; index < count; index++) {
        const char *type = NULL, *name = NULL, *source = NULL;
        ds_discovered *item;
        size_t i;
        int local_wins = 0;
        if (!sh_resource_bridge_decl_metadata(index, &type, &name, &source) ||
            !type || !name || !source) {
            backend_log("decl-server REFUSED: linked decl metadata was incomplete; whole snapshot refused");
            g_capture_refused++;
            return 0;
        }
        {
            const char *reason = NULL;
            if (!ds_validate_identity_parts(type, name, &reason)) {
                ds_log("REFUSED", source, reason);
                g_capture_refused++;
                continue;
            }
        }
        for (i = 0; i < discovery->count; i++) {
            if (!discovery->items[i].linked &&
                ds_identity_equal(&discovery->items[i], type, name)) {
                ds_log("LINKED-SHADOWED", source,
                       "same-identity local generated decl wins for this launch");
                local_wins = 1;
                break;
            }
        }
        if (local_wins) continue;
        if (discovery->count >= DS_MAX_DISCOVERED) {
            ds_log("REFUSED", source,
                   "combined discovery metadata exceeds the 4096-entry safety cap; whole snapshot refused");
            g_capture_refused++;
            return 0;
        }
        if (discovery->count == discovery->capacity && !ds_grow_discovery(discovery)) {
            ds_log("REFUSED", source, "combined discovery metadata allocation failed; whole snapshot refused");
            g_capture_refused++;
            return 0;
        }
        item = &discovery->items[discovery->count++];
        strcpy_s(item->type, sizeof(item->type), type);
        strcpy_s(item->name, sizeof(item->name), name);
        strcpy_s(item->source, sizeof(item->source), source);
        strcpy_s(item->package, sizeof(item->package), "linked");
        item->linked_index = index;
        item->linked = 1;
    }
    return 1;
}

static int ds_walk(ds_discovery *discovery, const char *package,
                   const char *directory, const char *relative, int depth)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    DWORD error;

    if (depth > DS_MAX_DEPTH) {
        ds_log("REFUSED", relative, "directory nesting exceeds 16 levels");
        g_capture_refused++;
        return 1;
    }
    if (_snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", directory) < 0) {
        ds_log("REFUSED", relative, "directory path exceeds MAX_PATH");
        g_capture_refused++;
        return 1;
    }
    search = g_find_first(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        error = g_get_last_error();
        if (ds_find_first_status(search, error) == DS_ENUM_DONE) return 1;
        ds_log_win32_refusal(relative[0] ? relative : "generated/decls",
                            "FindFirstFileA", error);
        g_capture_refused++;
        return 0;
    }
    for (;;) {
        char child[MAX_PATH];
        char child_relative[MAX_PATH];
        if (strcmp(found.cFileName, ".") != 0 && strcmp(found.cFileName, "..") != 0) {
            if (_snprintf_s(child, sizeof(child), _TRUNCATE,
                            "%s\\%s", directory, found.cFileName) < 0 ||
                _snprintf_s(child_relative, sizeof(child_relative), _TRUNCATE, "%s%s%s",
                            relative, relative[0] ? "\\" : "", found.cFileName) < 0) {
                ds_log("REFUSED", found.cFileName, "path exceeds MAX_PATH");
                g_capture_refused++;
            } else if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    ds_log("REFUSED", child_relative, "reparse-point directory");
                    g_capture_refused++;
                } else if (!ds_walk(discovery, package, child, child_relative, depth + 1)) {
                    g_find_close(search);
                    return 0;
                }
            } else if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                ds_log("REFUSED", child_relative, "reparse-point file");
                g_capture_refused++;
            } else if (!ds_discover_one(discovery, package, child, child_relative)) {
                g_find_close(search);
                return 0;
            }
        }

        if (g_find_next(search, &found)) continue;
        error = g_get_last_error();
        if (ds_find_next_status(FALSE, error) == DS_ENUM_DONE) break;
        ds_log_win32_refusal(relative[0] ? relative : "generated/decls",
                            "FindNextFileA", error);
        g_capture_refused++;
        g_find_close(search);
        return 0;
    }
    g_find_close(search);
    return 1;
}

#ifdef SH_DECL_SERVER_TESTING
int sh_decl_server_test_walk(const char *directory, const char *relative,
                             size_t *retained_count)
{
    ds_discovery discovery = {0};
    int ok;
    g_capture_refused = 0;
    ok = ds_walk(&discovery, "test", directory ? directory : "test",
                 relative ? relative : "", 0);
    if (retained_count) *retained_count = ok ? discovery.count : 0;
    ds_free_discovery(&discovery);
    return ok;
}
#endif

/* The package set is 26 KB and these captures already carry large frames, so it
 * lives in static storage rather than on the stack: putting it on the stack
 * tripped the /GS guard and terminated DOOM with 0xC0000409. Each capture is a
 * guarded one-shot on a single thread, so a shared buffer is safe here. */
static sh_package g_packages[SH_PACKAGES_MAX];

static int ds_capture_snapshot(void)
{
    char root[MAX_PATH];
    char directory[MAX_PATH];
    size_t package_count = 0, package_index;
    DWORD attributes;
    DWORD error;
    ds_discovery discovery = {0};
    sh_decl_server_order_item *ordered = NULL;
    size_t i;
    int ok = 0;

    if (!sh_overrides_get_root(root, sizeof(root))) {
        backend_log("decl-server REFUSED: could not resolve the overrides root");
        g_capture_refused++;
        return 0;
    }
    /* One decls root per installed package, walked into a single discovery set
     * so the existing case-insensitive collision rule sees identities from every
     * package at once and refuses an ambiguous one no matter who published it. */
    if (!sh_packages_enumerate(root, g_packages, SH_PACKAGES_MAX, &package_count)) {
        backend_log("decl-server REFUSED: the overrides package directory could not be enumerated completely");
        g_capture_refused++;
        return 0;
    }
    if (package_count == 0) {
        backend_log("decl-server idle: no override packages installed");
    }
    for (package_index = 0; package_index < package_count; package_index++) {
        if (!sh_package_subdir(&g_packages[package_index], "decls", directory,
                               sizeof(directory))) {
            backend_log("decl-server REFUSED: a package decls path exceeded its bounded length");
            g_capture_refused++;
            goto done;
        }
        attributes = GetFileAttributesA(directory);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            error = GetLastError();
            if (ds_root_attributes_status(attributes, error) != DS_ENUM_DONE) {
                ds_log_win32_refusal(g_packages[package_index].name,
                                     "GetFileAttributesA", error);
                g_capture_refused++;
                goto done;
            }
            continue;                    /* a package may carry no decls at all */
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            backend_log("decl-server REFUSED: a package decls root is not a regular directory");
            g_capture_refused++;
            goto done;
        }
        if (!ds_walk(&discovery, g_packages[package_index].name, directory, "", 0))
            goto done;
    }
    if (!sh_resource_bridge_gate_ok()) {
        backend_log("decl-server REFUSED: installed-resource manifest snapshot/provider is unavailable; whole decl snapshot refused");
        g_capture_refused++;
        goto done;
    }
    if (!ds_discover_linked(&discovery)) goto done;
    if (discovery.count == 0) {
        ok = 1;
        goto done;
    }

    ordered = (sh_decl_server_order_item *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, discovery.count * sizeof(ordered[0]));
    if (!ordered) {
        backend_log("decl-server REFUSED: ordering-table allocation failed; whole snapshot refused");
        g_capture_refused++;
        goto done;
    }
    for (i = 0; i < discovery.count; i++) {
        ordered[i].type = discovery.items[i].type;
        ordered[i].name = discovery.items[i].name;
        ordered[i].source = discovery.items[i].source;
        ordered[i].value = &discovery.items[i];
    }
    sh_decl_server_order_and_admit(ordered, discovery.count, DS_MAX_CANDIDATES);

    g_candidates = (ds_candidate *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                             sizeof(ds_candidate) * DS_MAX_CANDIDATES);
    if (!g_candidates) {
        backend_log("decl-server REFUSED: candidate-table allocation failed");
        g_capture_refused++;
        goto done;
    }
    for (i = 0; i < discovery.count; i++) {
        ds_discovered *item = (ds_discovered *)ordered[i].value;
        ds_candidate *candidate;
        const char *reason = NULL;
        char *body;
        size_t body_length;

        if (ordered[i].duplicate) {
            char detail[320];
            /* Identical copies were already composed away at discovery, so a
             * survivor here is a real disagreement: two packages claim one
             * identity with different bytes and neither can silently win. */
            _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                        "case-insensitive duplicate type/name identity published by "
                        "package '%s' with differing content", item->package);
            ds_log("REFUSED", item->source, detail);
            g_capture_refused++;
            continue;
        }
        if (!ordered[i].admitted) {
            ds_log("REFUSED", item->source, "launch snapshot exceeds the 512-decl cap");
            g_capture_refused++;
            continue;
        }
        body = item->linked ? ds_read_linked(item->linked_index, &body_length, &reason) :
                              ds_read_file(item->absolute, &body_length, &reason);
        if (!body) {
            ds_log("REFUSED", item->source, reason);
            g_capture_refused++;
            continue;
        }
        candidate = &g_candidates[g_candidate_count++];
        strcpy_s(candidate->type, sizeof(candidate->type), item->type);
        strcpy_s(candidate->name, sizeof(candidate->name), item->name);
        strcpy_s(candidate->source, sizeof(candidate->source), item->source);
        candidate->body = body;
        candidate->body_length = body_length;
        g_total_bytes += body_length;
    }

    if (g_candidate_count > 1) {
        sh_decl_reference_item *references = NULL;
        ds_candidate *dependency_order = NULL;
        size_t edge_count = 0;
        size_t cycle_count = 0;

        references = (sh_decl_reference_item *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            (size_t)g_candidate_count * sizeof(references[0]));
        dependency_order = (ds_candidate *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            (size_t)g_candidate_count * sizeof(dependency_order[0]));
        if (!references || !dependency_order) {
            backend_log("decl-server REFUSED: dependency-order allocation failed; whole snapshot refused");
            g_capture_refused++;
            if (dependency_order) HeapFree(GetProcessHeap(), 0, dependency_order);
            if (references) HeapFree(GetProcessHeap(), 0, references);
            goto done;
        }
        for (i = 0; i < (size_t)g_candidate_count; i++) {
            references[i].name = g_candidates[i].name;
            references[i].text = (const unsigned char *)g_candidates[i].body;
            references[i].text_length = g_candidates[i].body_length;
            references[i].value = (void *)(uintptr_t)(i + 1u);
        }
        if (!sh_decl_text_order_by_references(references,
                                              (size_t)g_candidate_count,
                                              &edge_count, &cycle_count)) {
            backend_log("decl-server REFUSED: dependency ordering failed (allocation or explicit inheritance cycle); whole snapshot refused");
            g_capture_refused++;
            HeapFree(GetProcessHeap(), 0, dependency_order);
            HeapFree(GetProcessHeap(), 0, references);
            goto done;
        }
        for (i = 0; i < (size_t)g_candidate_count; i++) {
            size_t source_index = (size_t)(uintptr_t)references[i].value - 1u;
            dependency_order[i] = g_candidates[source_index];
        }
        memcpy(g_candidates, dependency_order,
               (size_t)g_candidate_count * sizeof(g_candidates[0]));
        {
            char line[224];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "decl-server dependency order: %zu quoted identity edge(s), %zu SCC cycle member(s); deterministic component order",
                        edge_count, cycle_count);
            backend_log(line);
        }
        HeapFree(GetProcessHeap(), 0, dependency_order);
        HeapFree(GetProcessHeap(), 0, references);
    }
    ok = 1;

done:
    if (ordered) HeapFree(GetProcessHeap(), 0, ordered);
    ds_free_discovery(&discovery);
    if (!ok) ds_free_candidates();
    return ok;
}

static int ds_decode_registry(void **out_registry,
                              decl_type_by_name_fn *out_type_by_name,
                              decl_register_file_fn *out_register_file)
{
    uint8_t instruction[DS_ANCHOR_MOV_LENGTH];
    int32_t displacement;
    const uint8_t *slot;
    void *registry = NULL;
    void **vtable = NULL;
    void *type_method = NULL;
    void *register_method = NULL;

    if (!g_registry_anchor ||
        !ds_safe_read(g_registry_anchor + DS_ANCHOR_MOV_OFFSET, instruction, sizeof(instruction)) ||
        instruction[0] != 0x48 || instruction[1] != 0x8b || instruction[2] != 0x0d) return 0;
    memcpy(&displacement, instruction + 3, sizeof(displacement));
    slot = (const uint8_t *)((uintptr_t)g_registry_anchor + DS_ANCHOR_MOV_OFFSET +
                            DS_ANCHOR_MOV_LENGTH + (intptr_t)displacement);
    if (!ds_safe_read(slot, &registry, sizeof(registry)) || !registry ||
        !ds_safe_read(registry, &vtable, sizeof(vtable)) || !vtable ||
        !ds_safe_read((const uint8_t *)vtable + DS_REGISTRY_REGISTER_FILE_SLOT,
                      &register_method, sizeof(register_method)) ||
        !ds_safe_read((const uint8_t *)vtable + DS_REGISTRY_TYPE_SLOT,
                      &type_method, sizeof(type_method))) return 0;
    if (type_method != (void *)g_expected_type_by_name ||
        register_method != (void *)g_expected_register_file) return 0;
    *out_registry = registry;
    *out_type_by_name = (decl_type_by_name_fn)type_method;
    *out_register_file = (decl_register_file_fn)register_method;
    return 1;
}

static void ds_refuse_remaining(int start, const char *reason, int *refused)
{
    int i;
    for (i = start; i < g_candidate_count; i++) {
        if (g_candidates[i].outcome == DS_CANDIDATE_REFUSED ||
            g_candidates[i].outcome == DS_CANDIDATE_SHADOWED ||
            g_candidates[i].outcome == DS_CANDIDATE_NON_PALETTE) continue;
        ds_log("REFUSED", g_candidates[i].source, reason);
        g_candidates[i].outcome = DS_CANDIDATE_REFUSED;
        (*refused)++;
    }
}

static int ds_publish_missing_table(int missing_count)
{
    sh_overrides_internal_decl_entry *entries;
    size_t i;
    int at = 0;
    int published;

    if (missing_count <= 0 ||
        (size_t)missing_count > SIZE_MAX / sizeof(entries[0])) return 0;
    entries = (sh_overrides_internal_decl_entry *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        (size_t)missing_count * sizeof(entries[0]));
    if (!entries) return 0;
    for (i = 0; i < (size_t)g_candidate_count; i++) {
        ds_candidate *candidate = &g_candidates[i];
        if (candidate->outcome != DS_CANDIDATE_MISSING) continue;
        entries[at].type = candidate->type;
        entries[at].name = candidate->name;
        entries[at].body = (const unsigned char *)candidate->body;
        entries[at].body_length = candidate->body_length;
        at++;
    }
    published = at == missing_count &&
                sh_overrides_internal_decl_table_install(entries,
                                                         (size_t)missing_count);
    HeapFree(GetProcessHeap(), 0, entries);
    return published;
}

enum {
    DS_REGISTER_OK = 1,
    DS_REGISTER_CONSTRUCTOR_EXCEPTION = 0,
    DS_REGISTER_SCANNER_EXCEPTION = -1,
    DS_REGISTER_SCANNER_FALSE = -2,
    DS_REGISTER_DESTRUCTOR_EXCEPTION = -3
};

typedef union ds_idstr {
    void *alignment;
    unsigned char bytes[DS_IDSTR_SIZE];
} ds_idstr;

static int ds_candidate_source_name(const ds_candidate *candidate,
                                    char *out, size_t out_cap)
{
    size_t i;
    int written;
    if (!candidate || !out || out_cap == 0) return 0;
    written = _snprintf_s(out, out_cap, _TRUNCATE, "%s/%s.decl",
                          candidate->type, candidate->name);
    if (written < 0) return 0;
    for (i = 0; out[i]; i++)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = (char)(out[i] - 'A' + 'a');
    return 1;
}

/* DeclRegisterFile's source parameter is an idStr object, not a C string. Keep
 * the marshalling boundary tiny and explicit: construct one native temporary,
 * scan exactly this candidate once, then destroy it. Every exception or false
 * native result is terminal. There is intentionally no alternate call shape or
 * retry path. */
static int ds_register_candidate_source(void *registry,
                                        const char *source_name,
                                        decl_register_file_fn register_file,
                                        idstr_ctor_fn ctor,
                                        idstr_dtor_fn dtor)
{
    ds_idstr idstr;
    int constructed = 0;
    int scan_ok = 0;
    int scan_fault = 0;
    int dtor_fault = 0;

    if (!registry || !source_name || !register_file || !ctor || !dtor)
        return DS_REGISTER_SCANNER_FALSE;
    memset(&idstr, 0, sizeof(idstr));

    __try {
        ctor(&idstr, source_name);
        constructed = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return DS_REGISTER_CONSTRUCTOR_EXCEPTION;
    }

    __try {
        scan_ok = register_file(registry, &idstr, NULL) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        scan_fault = 1;
    }

    if (constructed) {
        __try {
            dtor(&idstr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            dtor_fault = 1;
        }
    }

    if (dtor_fault) return DS_REGISTER_DESTRUCTOR_EXCEPTION;
    if (scan_fault) return DS_REGISTER_SCANNER_EXCEPTION;
    if (!scan_ok) return DS_REGISTER_SCANNER_FALSE;
    return DS_REGISTER_OK;
}

#ifdef SH_DECL_SERVER_TESTING
int sh_decl_server_test_register_candidate(
    void *registry,
    const char *source_name,
    sh_decl_server_test_idstr_ctor_fn ctor,
    sh_decl_server_test_idstr_dtor_fn dtor,
    sh_decl_server_test_register_file_fn register_file)
{
    return ds_register_candidate_source(registry, source_name,
                                        (decl_register_file_fn)register_file,
                                        (idstr_ctor_fn)ctor,
                                        (idstr_dtor_fn)dtor);
}
#endif

/* Native materialization contract, established by static RE of the pinned
 * build and recorded in the campaign findings:
 *
 *   - idDecl+0x2c bit 0x01 is "parse in progress". The decl parser refuses a
 *     parent that still has it set.
 *   - idDecl+0x2c bit 0x02 is "pending load". The manager lookup helper
 *     (RVA 0x1800A40) clears it and runs the generic load (RVA 0x17FF5F0)
 *     before returning, so an ordinary lookup already leaves a live object
 *     loaded. At load state RUNNING that load reaches the type-specific
 *     production-file path (RVA 0x17AAB70), which reads the decl through the
 *     file system open-by-name slot this product already provides and parses
 *     it with the type's own parser.
 *   - idDecl+0x2c bit 0x04 only records that the generic load acquired a
 *     source. No engine consumer uses it as an admission condition, so it is
 *     diagnostic here and never terminal.
 *   - The Snap palette builder (RVA 0x54AEE0) enumerates every live
 *     snapEditorEntityDef in its manager by index and admits exactly those the
 *     native validator (RVA 0x4F8180) accepts: a resolved entityDef at +0x1c8,
 *     every output at +0x440/+0x448 carrying flag 0x20 at +0x3cd, and every
 *     input at +0x458/+0x460 carrying flag 0x10.
 *   - The editor-entity and entityDef parsers resolve their own inherit,
 *     entityDef and game-ref edges with makeDefault=0, which succeeds only
 *     when the target already has an object in its manager.
 *
 * Materialization is therefore not a typed graph walk. It is: give every
 * registered identity a live object in its manager in registration order, then
 * require the native palette contract of each new editor entity. That is
 * generic across decl types and holds no package-specific knowledge.
 */
#define DS_DECL_OUTPUTS_PTR_OFFSET 0x440u
#define DS_DECL_OUTPUTS_NUM_OFFSET 0x448u
#define DS_DECL_INPUTS_PTR_OFFSET  0x458u
#define DS_DECL_INPUTS_NUM_OFFSET  0x460u
#define DS_TARGET_FLAGS_OFFSET     0x3cdu
#define DS_TARGET_OUTPUT_FLAG      0x20u
#define DS_TARGET_INPUT_FLAG       0x10u
#define DS_MAX_TARGETS             4096

typedef struct ds_materialize_context {
    void *registry;
    decl_type_by_name_fn type_by_name;
    decl_source_find_fn source_find;
    decl_find_fn find_decl;
    int *materialized;
} ds_materialize_context;

static int ds_copy_dependency_name(const unsigned char *name, size_t name_length,
                                   char *out, size_t out_capacity)
{
    if (!name || !out || out_capacity == 0 || name_length == 0 ||
        name_length >= out_capacity) return 0;
    memcpy(out, name, name_length);
    out[name_length] = '\0';
    return 1;
}

static int ds_read_decl_state(void *decl, unsigned char *state)
{
    return decl && ds_safe_read((const uint8_t *)decl + DS_DECL_STATE_OFFSET,
                                state, sizeof(*state));
}

/* A live object is usable as a parse-time dependency when the engine can read
 * it and it is not mid-parse. This mirrors the only state the native parsers
 * test before consuming a resolved decl. */
static int ds_decl_usable(void *decl, const char **reason)
{
    unsigned char state = 0;

    if (reason) *reason = NULL;
    if (!decl) {
        if (reason) *reason = "native DeclFind returned a null decl";
        return 0;
    }
    if (!ds_read_decl_state(decl, &state)) {
        if (reason) *reason = "decl state byte was unreadable";
        return 0;
    }
    if ((state & DS_DECL_IN_PROGRESS) != 0) {
        if (reason) *reason = "decl remained in progress";
        return 0;
    }
    return 1;
}

/* One half of the native palette validator: every entry of a resolved target
 * array must be present and carry its direction flag. Counts are bounded so a
 * wrong or partially written object cannot turn this into an unbounded walk. */
static int ds_targets_match(void *decl, size_t pointer_offset,
                            size_t count_offset, unsigned char mask,
                            const char *unreadable, const char *null_entry,
                            const char *wrong_flag, const char **reason)
{
    void **entries = NULL;
    int count = 0;
    int i;

    if (!ds_safe_read((const uint8_t *)decl + pointer_offset, &entries,
                      sizeof(entries)) ||
        !ds_safe_read((const uint8_t *)decl + count_offset, &count,
                      sizeof(count))) {
        if (reason) *reason = unreadable;
        return 0;
    }
    if (count < 0 || count > DS_MAX_TARGETS || (count != 0 && !entries)) {
        if (reason) *reason = unreadable;
        return 0;
    }
    for (i = 0; i < count; i++) {
        void *target = NULL;
        unsigned char flags = 0;
        if (!ds_safe_read(entries + i, &target, sizeof(target)) || !target) {
            if (reason) *reason = null_entry;
            return 0;
        }
        if (!ds_safe_read((const uint8_t *)target + DS_TARGET_FLAGS_OFFSET,
                          &flags, sizeof(flags)) || (flags & mask) == 0) {
            if (reason) *reason = wrong_flag;
            return 0;
        }
    }
    return 1;
}

/* The exact admission condition the native palette builder applies to every
 * live snapEditorEntityDef. Anything this rejects would be dropped by the
 * builder anyway, with the same meaning. */
static int ds_sedef_palette_ready(void *decl, const char **reason)
{
    void *entity_def = NULL;

    if (!ds_decl_usable(decl, reason)) return 0;
    if (!ds_safe_read((const uint8_t *)decl + DS_DECL_ENTITYDEF_OFFSET,
                      &entity_def, sizeof(entity_def))) {
        if (reason) *reason = "resolved entityDef pointer was unreadable";
        return 0;
    }
    if (!entity_def) {
        if (reason)
            *reason = "snapEditorEntityDef has no resolved entityDef; the native palette validator rejects it";
        return 0;
    }
    if (!ds_targets_match(decl, DS_DECL_OUTPUTS_PTR_OFFSET,
                          DS_DECL_OUTPUTS_NUM_OFFSET, DS_TARGET_OUTPUT_FLAG,
                          "resolved output array was unreadable or out of range",
                          "resolved output target was null",
                          "resolved output target is not an output", reason))
        return 0;
    if (!ds_targets_match(decl, DS_DECL_INPUTS_PTR_OFFSET,
                          DS_DECL_INPUTS_NUM_OFFSET, DS_TARGET_INPUT_FLAG,
                          "resolved input array was unreadable or out of range",
                          "resolved input target was null",
                          "resolved input target is not an input", reason))
        return 0;
    return 1;
}

/* Diagnostics only. One line per observed object so a cold run explains a
 * refusal without a second instrumented build. */
static void ds_log_decl_state(const char *stage, const char *subject,
                              void *decl, int sedef)
{
    char detail[320];
    unsigned char state = 0;
    int readable = ds_read_decl_state(decl, &state);
    void *entity_def = NULL;
    int outputs = -1;
    int inputs = -1;

    if (readable && sedef) {
        if (!ds_safe_read((const uint8_t *)decl + DS_DECL_ENTITYDEF_OFFSET,
                          &entity_def, sizeof(entity_def)))
            entity_def = NULL;
        if (!ds_safe_read((const uint8_t *)decl + DS_DECL_OUTPUTS_NUM_OFFSET,
                          &outputs, sizeof(outputs)))
            outputs = -1;
        if (!ds_safe_read((const uint8_t *)decl + DS_DECL_INPUTS_NUM_OFFSET,
                          &inputs, sizeof(inputs)))
            inputs = -1;
    }
    _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                "%s decl=%p state=%s0x%02x entityDef=%p outputs=%d inputs=%d",
                stage ? stage : "", decl, readable ? "" : "unreadable:",
                (unsigned)state, entity_def, outputs, inputs);
    ds_log("DECLSTATE", subject ? subject : "", detail);
}

/* The new editor entities that survived the native palette contract. A
 * MISSING candidate still classified MISSING at the end of the pass is one
 * the palette builder was asked to admit. */
static int ds_admitted_root_count(void)
{
    int i, count = 0;
    for (i = 0; i < g_candidate_count; i++)
        if (g_candidates[i].outcome == DS_CANDIDATE_MISSING &&
            _stricmp(g_candidates[i].type, "snapEditorEntityDef") == 0)
            count++;
    return count;
}

/* Build the engine's own decl path for one candidate. The runtime existence
 * probe is asked in exactly this form (RVA 0x17AB4E0). */
static int ds_probe_path(int outcome, char *out, size_t out_size)
{
    int i;
    for (i = 0; i < g_candidate_count; i++) {
        if (g_candidates[i].outcome != outcome) continue;
        if (_snprintf_s(out, out_size, _TRUNCATE, "generated/decls/%s/%s.decl",
                        g_candidates[i].type, g_candidates[i].name) < 0)
            continue;
        return 1;
    }
    return 0;
}

static int ds_materialize_failure(int index, const char *type,
                                  const char *name, const char *reason)
{
    char subject[SH_DECL_SERVER_SOURCE_CAP + SH_DECL_SERVER_NAME_CAP + 16];
    char detail[512];
    const char *owner = "decl-server identity";

    if (index >= 0 && index < g_candidate_count)
        owner = g_candidates[index].source;
    _snprintf_s(subject, sizeof(subject), _TRUNCATE, "%s -> %s/%s",
                owner, type ? type : "", name ? name : "");
    _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                "materialization failed -- %s",
                reason ? reason : "native resolution failed");
    ds_log("REFUSED", subject, detail);
    if (index >= 0 && index < g_candidate_count &&
        g_candidates[index].outcome == DS_CANDIDATE_MISSING)
        g_candidates[index].outcome = DS_CANDIDATE_REFUSED;
    return 0;
}

/* Give one registered identity a live object in its own manager. A lookup is
 * tried first because the engine may already have created and loaded the
 * object from the source scan; make-default is used only when no object
 * exists, which is the same order the native loader uses. */
static int ds_materialize_identity(ds_materialize_context *context, int index)
{
    ds_candidate *candidate;
    void *type_manager = NULL;
    void *decl = NULL;
    const char *reason = NULL;
    int made_default = 0;
    int fault = 0;

    if (!context || index < 0 || index >= g_candidate_count) return 0;
    candidate = &g_candidates[index];

    __try {
        type_manager = context->type_by_name(context->registry, candidate->type);
        if (!type_manager) {
            reason = "decl type manager was null during materialization";
        } else {
            decl = context->find_decl(type_manager, candidate->name, 0);
            if (!decl) {
                decl = context->find_decl(type_manager, candidate->name, 1);
                made_default = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fault = 1;
        reason = "engine exception during identity materialization";
    }

    if (!fault && !reason && !ds_decl_usable(decl, &reason) && !reason)
        reason = "materialized identity failed native state validation";
    if (fault || reason) {
        ds_log_decl_state("after materialization", candidate->source,
                          fault ? NULL : decl, 0);
        return ds_materialize_failure(index, candidate->type, candidate->name,
                                      reason);
    }
    if (made_default && context->materialized) (*context->materialized)++;
    ds_log(made_default ? "MATERIALIZED" : "REUSED", candidate->source,
           made_default
               ? "native make-default DeclFind produced a live decl for the registered identity"
               : "registered identity already had a live decl");
    return 1;
}

typedef struct ds_edge_report {
    ds_materialize_context *context;
    const char *owner;
} ds_edge_report;

/* Diagnostic-only traversal of the typed edges a refused editor entity was
 * expected to resolve. It never changes an outcome and never refuses. */
static int ds_report_edge(const char *type, const unsigned char *name,
                          size_t name_length, void *opaque)
{
    ds_edge_report *report = (ds_edge_report *)opaque;
    char copied[SH_DECL_SERVER_NAME_CAP];
    char subject[SH_DECL_SERVER_SOURCE_CAP + SH_DECL_SERVER_NAME_CAP + 16];
    void *type_manager = NULL;
    void *source_record = NULL;
    void *decl = NULL;
    int fault = 0;

    if (!report || !report->context || !type ||
        !ds_copy_dependency_name(name, name_length, copied, sizeof(copied)))
        return 1;
    __try {
        type_manager = report->context->type_by_name(report->context->registry,
                                                     type);
        if (type_manager) {
            source_record = report->context->source_find(type_manager, copied);
            decl = report->context->find_decl(type_manager, copied, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fault = 1;
    }
    _snprintf_s(subject, sizeof(subject), _TRUNCATE, "%s -> %s/%s",
                report->owner ? report->owner : "", type, copied);
    if (fault) {
        ds_log("DECLSTATE", subject, "edge probe raised an engine exception");
        return 1;
    }
    {
        char detail[192];
        _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                    "typeManager=%p source=%p", type_manager, source_record);
        ds_log("DECLSTATE", subject, detail);
    }
    ds_log_decl_state("edge", subject, decl,
                      _stricmp(type, "snapEditorEntityDef") == 0);
    return 1;
}

static void ds_report_root_edges(ds_materialize_context *context,
                                 const ds_candidate *candidate)
{
    ds_edge_report report;
    report.context = context;
    report.owner = candidate->source;
    (void)sh_decl_text_collect_sedef_dependencies(
        (const unsigned char *)candidate->body, candidate->body_length,
        ds_report_edge, &report);
}

/* A new editor entity is the only identity this service has to admit to a
 * derived catalog, so it is the only one held to the native palette contract.
 * A single make-default retry is allowed because the native loader discards
 * and rebuilds an object whose earlier load did not complete. */
static int ds_materialize_root(ds_materialize_context *context, int index)
{
    ds_candidate *candidate;
    void *type_manager = NULL;
    void *decl = NULL;
    const char *reason = NULL;
    int made_default = 0;
    int fault = 0;

    if (!context || index < 0 || index >= g_candidate_count) return 0;
    candidate = &g_candidates[index];

    __try {
        type_manager = context->type_by_name(context->registry, candidate->type);
        if (!type_manager) {
            reason = "decl type manager was null during editor-entity materialization";
        } else {
            decl = context->find_decl(type_manager, candidate->name, 0);
            if (!ds_sedef_palette_ready(decl, &reason)) {
                reason = NULL;
                decl = context->find_decl(type_manager, candidate->name, 1);
                made_default = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fault = 1;
        reason = "engine exception during editor-entity materialization";
    }

    if (!fault && !reason && !ds_sedef_palette_ready(decl, &reason) && !reason)
        reason = "editor entity failed the native palette contract";
    if (fault || reason) {
        ds_log_decl_state(made_default ? "after make-default" : "after lookup",
                          candidate->source, fault ? NULL : decl, 1);
        if (!fault && decl) ds_report_root_edges(context, candidate);
        return ds_materialize_failure(index, candidate->type, candidate->name,
                                      reason);
    }
    ds_log_decl_state(made_default ? "after make-default" : "after lookup",
                      candidate->source, decl, 1);
    if (made_default && context->materialized) (*context->materialized)++;
    ds_log(made_default ? "MATERIALIZED" : "REUSED", candidate->source,
           "live snapEditorEntityDef satisfies the native palette validator contract");
    return 1;
}

/* Source registration creates source records, not live objects. Materialize
 * every registered identity in registration order so the editor-entity parse
 * can resolve its own edges, then hold each eligible editor entity to the
 * native palette contract. Source-only editor bodies are never materialized. */
static int ds_materialize_missing_sedefs(void *registry,
                                         decl_type_by_name_fn type_by_name,
                                         decl_source_find_fn source_find,
                                         decl_find_fn find_decl,
                                         int *materialized)
{
    ds_materialize_context context;
    int i;

    if (materialized) *materialized = 0;
    if (!registry || !type_by_name || !source_find || !find_decl) return 0;
    context.registry = registry;
    context.type_by_name = type_by_name;
    context.source_find = source_find;
    context.find_decl = find_decl;
    context.materialized = materialized;

    for (i = 0; i < g_candidate_count; i++) {
        ds_candidate *candidate = &g_candidates[i];
        if (candidate->outcome != DS_CANDIDATE_MISSING ||
            _stricmp(candidate->type, "snapEditorEntityDef") != 0)
            continue;
        if (!sh_decl_text_sedef_has_materializable_source(
                (const unsigned char *)candidate->body, candidate->body_length)) {
            candidate->outcome = DS_CANDIDATE_NON_PALETTE;
            ds_log("NON-PALETTE", candidate->source,
                   "source-only snapEditorEntityDef has no direct edit.entityDef or inherit assignment; excluded from materialization and palette admission");
        }
    }
    for (i = 0; i < g_candidate_count; i++) {
        ds_candidate *candidate = &g_candidates[i];
        if (candidate->outcome != DS_CANDIDATE_MISSING ||
            _stricmp(candidate->type, "snapEditorEntityDef") == 0)
            continue;
        if (!ds_materialize_identity(&context, i)) return 0;
    }
    for (i = 0; i < g_candidate_count; i++) {
        ds_candidate *candidate = &g_candidates[i];
        if (candidate->outcome != DS_CANDIDATE_MISSING ||
            _stricmp(candidate->type, "snapEditorEntityDef") != 0)
            continue;
        if (!ds_materialize_root(&context, i)) return 0;
    }
    return 1;
}

enum {
    DS_PHASE_FAILURE_NONE = 0,
    DS_PHASE_FAILURE_SCAN,
    DS_PHASE_FAILURE_MATERIALIZATION,
    DS_PHASE_FAILURE_PALETTE
};

/* This is the production two-phase boundary. Every missing source is scanned
 * before the first make-default lookup can materialize an editor entity. Keep
 * the ordering in one helper so the executable test seam exercises this same
 * loop rather than a test-only approximation. */
static int ds_scan_and_materialize_missing(
    void *registry, decl_type_by_name_fn type_by_name,
    decl_register_file_fn register_file, idstr_ctor_fn ctor,
    idstr_dtor_fn dtor, decl_source_find_fn source_find,
    decl_find_fn find_decl,
    decl_palette_refresh_fn palette_refresh, int *registered,
    int *materialized, int *refused, const char **failed_source,
    int *failure_phase)
{
    int i;

    if (registered) *registered = 0;
    if (materialized) *materialized = 0;
    if (failure_phase) *failure_phase = DS_PHASE_FAILURE_NONE;
    if (!registry || !type_by_name || !register_file || !ctor || !dtor ||
        !source_find || !find_decl || !palette_refresh) {
        if (failure_phase) *failure_phase = DS_PHASE_FAILURE_SCAN;
        return 0;
    }
    for (i = 0; i < g_candidate_count; i++) {
        ds_candidate *candidate = &g_candidates[i];
        char source_name[SH_DECL_SERVER_TYPE_CAP + SH_DECL_SERVER_NAME_CAP + 8];
        int registration;

        if (candidate->outcome != DS_CANDIDATE_MISSING) continue;
        if (!ds_candidate_source_name(candidate, source_name, sizeof(source_name))) {
            ds_log("REFUSED", candidate->source,
                   "native decltree source name exceeded its bounded path");
            candidate->outcome = DS_CANDIDATE_REFUSED;
            if (refused) (*refused)++;
            ds_refuse_remaining(i + 1,
                                "not registered after source-name refusal", refused);
            if (failed_source) *failed_source = candidate->source;
            if (failure_phase) *failure_phase = DS_PHASE_FAILURE_SCAN;
            return 0;
        }
        registration = ds_register_candidate_source(registry, source_name,
                                                    register_file, ctor, dtor);
        if (registration != DS_REGISTER_OK) {
            char line[512];
            const char *detail = "native source boundary refused";
            if (registration == DS_REGISTER_CONSTRUCTOR_EXCEPTION)
                detail = "native idStr constructor raised an engine exception";
            else if (registration == DS_REGISTER_SCANNER_EXCEPTION)
                detail = "native +0x38 source scan raised an engine exception and may have partially registered";
            else if (registration == DS_REGISTER_SCANNER_FALSE)
                detail = "native +0x38 source scan returned false";
            else if (registration == DS_REGISTER_DESTRUCTOR_EXCEPTION)
                detail = "native idStr destructor raised an engine exception";
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "decl-server REGISTER FAILED: '%s' -- %s; no retry",
                        candidate->source, detail);
            backend_log(line);
            candidate->outcome = DS_CANDIDATE_REFUSED;
            if (refused) (*refused)++;
            ds_refuse_remaining(i + 1,
                                "not registered after terminal native scan failure", refused);
            if (failed_source) *failed_source = candidate->source;
            if (failure_phase) *failure_phase = DS_PHASE_FAILURE_SCAN;
            return 0;
        }
        {
            char line[512];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "decl-server REGISTERED: '%s' via native source '%s' (one scan)",
                        candidate->source, source_name);
            backend_log(line);
        }
        if (registered) (*registered)++;
    }
    if (!ds_materialize_missing_sedefs(registry, type_by_name, source_find,
                                       find_decl,
                                       materialized)) {
        if (refused) (*refused)++;
        ds_refuse_remaining(0,
                            "source registered; live materialization did not complete after a terminal materialization failure",
                            refused);
        if (failure_phase) *failure_phase = DS_PHASE_FAILURE_MATERIALIZATION;
        return 0;
    }
    {
        int palette_ok = 0;
        int palette_fault = 0;
        __try {
            palette_ok = palette_refresh() ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            palette_fault = 1;
        }
        if (palette_fault || !palette_ok) {
            if (failure_phase) *failure_phase = DS_PHASE_FAILURE_PALETTE;
            return 0;
        }
    }
    {
        /* Registration only reaches the source catalog, which the engine stops
         * consulting once a map starts loading. Keep the published identities
         * answerable across that boundary. This is not allowed to fail the
         * registration that already succeeded. */
        char owned[SH_DECL_SERVER_TYPE_CAP + SH_DECL_SERVER_NAME_CAP + 32];
        char published[SH_DECL_SERVER_TYPE_CAP + SH_DECL_SERVER_NAME_CAP + 32];
        if (ds_probe_path(DS_CANDIDATE_SHADOWED, owned, sizeof(owned)) &&
            ds_probe_path(DS_CANDIDATE_MISSING, published, sizeof(published)))
            (void)sh_decl_visibility_install(g_module_base, owned, published);
        else
            backend_log("decl-visibility REFUSED: no owned/published candidate pair was available to validate the probe");
    }
    return 1;
}

#ifdef SH_DECL_SERVER_TESTING
int sh_decl_server_test_materialize_missing_sedefs(
    const sh_decl_server_test_materialize_item *items, size_t count,
    void *registry, sh_decl_server_test_type_by_name_fn type_by_name,
    sh_decl_server_test_source_find_fn source_find,
    sh_decl_server_test_find_decl_fn find_decl, int *materialized)
{
    ds_candidate *saved_candidates = g_candidates;
    int saved_count = g_candidate_count;
    ds_candidate *test_candidates = NULL;
    size_t i;
    int ok;

    if (!items || count > DS_MAX_CANDIDATES || !type_by_name || !source_find ||
        !find_decl)
        return 0;
    if (count) {
        test_candidates = (ds_candidate *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(test_candidates[0]));
        if (!test_candidates) return 0;
        for (i = 0; i < count; i++) {
            if (!items[i].type || !items[i].name ||
                strcpy_s(test_candidates[i].type, sizeof(test_candidates[i].type), items[i].type) != 0 ||
                strcpy_s(test_candidates[i].name, sizeof(test_candidates[i].name), items[i].name) != 0 ||
                strcpy_s(test_candidates[i].source, sizeof(test_candidates[i].source),
                         items[i].source ? items[i].source : items[i].name) != 0) {
                HeapFree(GetProcessHeap(), 0, test_candidates);
                return 0;
            }
            test_candidates[i].outcome = items[i].outcome;
            test_candidates[i].shadow_kind = items[i].shadow_kind ==
                SH_DECL_SERVER_TEST_SHADOW_LIVE ? DS_SHADOW_LIVE :
                items[i].shadow_kind == SH_DECL_SERVER_TEST_SHADOW_SOURCE ?
                    DS_SHADOW_SOURCE : DS_SHADOW_NONE;
            test_candidates[i].body = (char *)items[i].body;
            test_candidates[i].body_length = items[i].body_length;
        }
    }
    g_candidates = test_candidates;
    g_candidate_count = (int)count;
    ok = ds_materialize_missing_sedefs(registry,
                                       (decl_type_by_name_fn)type_by_name,
                                       (decl_source_find_fn)source_find,
                                       (decl_find_fn)find_decl, materialized);
    g_candidates = saved_candidates;
    g_candidate_count = saved_count;
    if (test_candidates) HeapFree(GetProcessHeap(), 0, test_candidates);
    return ok;
}

int sh_decl_server_test_scan_and_materialize_missing(
    const sh_decl_server_test_materialize_item *items, size_t count,
    void *registry, sh_decl_server_test_type_by_name_fn type_by_name,
    sh_decl_server_test_source_find_fn source_find,
    sh_decl_server_test_find_decl_fn find_decl,
    sh_decl_server_test_palette_refresh_fn palette_refresh,
    sh_decl_server_test_idstr_ctor_fn ctor,
    sh_decl_server_test_idstr_dtor_fn dtor,
    sh_decl_server_test_register_file_fn register_file,
    int *registered, int *materialized, int *failure_phase)
{
    ds_candidate *saved_candidates = g_candidates;
    int saved_count = g_candidate_count;
    ds_candidate *test_candidates = NULL;
    int refused = 0;
    const char *failed_source = NULL;
    size_t i;
    int ok;

    if (!items || count > DS_MAX_CANDIDATES || !type_by_name || !source_find ||
        !find_decl || !palette_refresh || !ctor || !dtor || !register_file)
        return 0;
    if (count) {
        test_candidates = (ds_candidate *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(test_candidates[0]));
        if (!test_candidates) return 0;
        for (i = 0; i < count; i++) {
            if (!items[i].type || !items[i].name ||
                strcpy_s(test_candidates[i].type, sizeof(test_candidates[i].type), items[i].type) != 0 ||
                strcpy_s(test_candidates[i].name, sizeof(test_candidates[i].name), items[i].name) != 0 ||
                strcpy_s(test_candidates[i].source, sizeof(test_candidates[i].source),
                         items[i].source ? items[i].source : items[i].name) != 0) {
                HeapFree(GetProcessHeap(), 0, test_candidates);
                return 0;
            }
            test_candidates[i].outcome = items[i].outcome;
            test_candidates[i].shadow_kind = items[i].shadow_kind ==
                SH_DECL_SERVER_TEST_SHADOW_LIVE ? DS_SHADOW_LIVE :
                items[i].shadow_kind == SH_DECL_SERVER_TEST_SHADOW_SOURCE ?
                    DS_SHADOW_SOURCE : DS_SHADOW_NONE;
            test_candidates[i].body = (char *)items[i].body;
            test_candidates[i].body_length = items[i].body_length;
        }
    }
    g_candidates = test_candidates;
    g_candidate_count = (int)count;
    ok = ds_scan_and_materialize_missing(
        registry, (decl_type_by_name_fn)type_by_name,
        (decl_register_file_fn)register_file, (idstr_ctor_fn)ctor,
        (idstr_dtor_fn)dtor, (decl_source_find_fn)source_find,
        (decl_find_fn)find_decl,
        (decl_palette_refresh_fn)palette_refresh, registered, materialized,
        &refused, &failed_source, failure_phase);
    (void)failed_source;
    g_candidates = saved_candidates;
    g_candidate_count = saved_count;
    if (test_candidates) HeapFree(GetProcessHeap(), 0, test_candidates);
    return ok;
}
#endif

static void __cdecl ds_apply_command(void)
{
    void *registry = NULL;
    decl_type_by_name_fn type_by_name = NULL;
    decl_register_file_fn register_file = NULL;
    int missing = 0, shadowed = 0, refused = g_capture_refused;
    int i;

    if (InterlockedCompareExchange(&g_state, DS_STATE_APPLYING, DS_STATE_QUEUED) != DS_STATE_QUEUED)
        return;
    if (!ds_decode_registry(&registry, &type_by_name, &register_file)) {
        ds_refuse_remaining(0, "decl registry/vtable validation failed", &refused);
        backend_log("decl-server FAILED: native registry or its +0x38/+0x58 methods did not match clean resolves");
        InterlockedExchange(&g_state, DS_STATE_FAILED);
        ds_free_candidates();
        return;
    }

    /* This one engine command thread is the sole classifier, table publisher,
     * scanner, materializer, and palette-success publisher. Complete every
     * source/live lookup before exposing any exact decltree bytes. */
    for (i = 0; i < g_candidate_count; i++) {
        ds_candidate *candidate = &g_candidates[i];
        const char *reason = NULL;
        int classification = ds_classify_candidate(
            candidate, registry, type_by_name, g_find_source, g_find_decl, &reason);

        if (classification == DS_CLASSIFY_TERMINAL) {
            ds_log("REFUSED", candidate->source, reason);
            candidate->outcome = DS_CANDIDATE_REFUSED;
            refused++;
            ds_refuse_remaining(i + 1, "not classified after source/live lookup exception", &refused);
            ds_refuse_remaining(0, "table not submitted after classification exception", &refused);
            backend_log("decl-server FAILED: source-first classification raised an engine exception; no decltree table was published");
            InterlockedExchange(&g_state, DS_STATE_FAILED);
            ds_free_candidates();
            return;
        }
        if (classification == DS_CLASSIFY_REFUSED_TYPE) {
            ds_log("REFUSED", candidate->source, reason);
            candidate->outcome = DS_CANDIDATE_REFUSED;
            refused++;
            continue;
        }
        if (classification == DS_CLASSIFY_SHADOWED_SOURCE) {
            ds_log("SHADOWED", candidate->source,
                   "native source record already exists; ordinary file-shadow remains authoritative");
            candidate->outcome = DS_CANDIDATE_SHADOWED;
            candidate->shadow_kind = DS_SHADOW_SOURCE;
            shadowed++;
            continue;
        }
        if (classification == DS_CLASSIFY_SHADOWED_LIVE) {
            ds_log("SHADOWED", candidate->source,
                   "live decl object already exists; ordinary file-shadow remains authoritative");
            candidate->outcome = DS_CANDIDATE_SHADOWED;
            candidate->shadow_kind = DS_SHADOW_LIVE;
            shadowed++;
            continue;
        }
        candidate->outcome = DS_CANDIDATE_MISSING;
        missing++;
    }

    if (missing == 0) {
        char line[256];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "decl-server table not required: 0 MISSING, %d SHADOWED, %d REFUSED; no decltree entries published",
                    shadowed, refused);
        backend_log(line);
        InterlockedExchange(&g_state, DS_STATE_DONE);
        ds_free_candidates();
        return;
    }

    if (!ds_publish_missing_table(missing)) {
        ds_refuse_remaining(0, "exact decltree table publication failed", &refused);
        backend_log("decl-server FAILED: immutable per-decl table was not accepted by the installed override provider");
        InterlockedExchange(&g_state, DS_STATE_FAILED);
        ds_free_candidates();
        return;
    }

    {
        int registered = 0;
        int failed = 0;
        int materialization_failed = 0;
        int palette_skipped = 0;
        int materialized = 0;
        int failure_phase = DS_PHASE_FAILURE_NONE;
        const char *failed_source = NULL;
        if (!ds_scan_and_materialize_missing(
                registry, type_by_name, register_file, g_idstr_ctor,
                g_idstr_dtor, g_find_source, g_find_decl,
                sh_palette_refresh_after_decl_registration, &registered,
                &materialized, &refused, &failed_source, &failure_phase)) {
            if (failure_phase == DS_PHASE_FAILURE_SCAN) {
                failed = 1;
            } else if (failure_phase == DS_PHASE_FAILURE_MATERIALIZATION) {
                materialization_failed = 1;
                backend_log("decl-server FAILED: native source scans completed but a new snapEditorEntityDef could not be materialized; no palette refresh; no retry");
            } else {
                /* NOT TERMINAL ANY MORE.
                 *
                 * A first draft of this predicted the rebuild would REFUSE during Init because the
                 * editor did not exist yet. That prediction was wrong, and the live log says so:
                 * "the palette rebuild was completed". The editor singleton is a STATIC object at a
                 * fixed data RVA (0x3056748) with the palette embedded at +0x20660, so its vtable
                 * (0x20499A0) is written by CRT static initialization, long before Init -- and that
                 * vtable identity is exactly what palette_refresh validates. The object is
                 * constructed from the start; what does not exist yet is a POPULATED roster.
                 *
                 * So the rebuild does run, against a registry publication has just extended, and it
                 * is simply no longer load-bearing: publication now precedes any roster the editor
                 * builds for real. That is why a refusal must not be terminal -- on another build or
                 * another timing it would discard a registration that completely succeeded, in
                 * order to report the absence of something nothing depends on. */
                palette_skipped = 1;
            }
        }
        if (failed) {
            char line[384];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "decl-server FAILED: %d MISSING published, %d registered before terminal failure at '%s', %d SHADOWED, %d REFUSED; exact decltree table retained; no retry",
                        missing, registered, failed_source ? failed_source : "<unknown>",
                        shadowed, refused);
            backend_log(line);
            InterlockedExchange(&g_state, DS_STATE_FAILED);
        } else if (materialization_failed) {
            char line[384];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "decl-server FAILED: %d MISSING registered, %d SHADOWED, %d REFUSED; materialization was terminal; exact decltree table retained; no retry",
                        registered, shadowed, refused);
            backend_log(line);
            InterlockedExchange(&g_state, DS_STATE_FAILED);
        } else {
            char line[448];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "decl-server registration succeeded: %d MISSING registered in dependency order, %d live objects materialized, %d of them new editor entities held to the native palette contract, %d SHADOWED, %d REFUSED; published before the engine boot promotion, so the palette rebuild was %s",
                        registered, materialized, ds_admitted_root_count(),
                        shadowed, refused,
                        palette_skipped ? "not needed (the editor had not built one yet)"
                                        : "completed");
            backend_log(line);
            /* Keep one published identity addressable past ds_free_candidates so the caller can
             * read its resource level back after the engine's promotion returns. Identity only --
             * no decl pointer is retained, so nothing here can outlive what it describes. */
            for (i = 0; i < g_candidate_count; i++) {
                if (g_candidates[i].outcome != DS_CANDIDATE_MISSING) continue;
                strcpy_s(g_probe_type, sizeof(g_probe_type), g_candidates[i].type);
                strcpy_s(g_probe_name, sizeof(g_probe_name), g_candidates[i].name);
                break;
            }
            InterlockedExchange(&g_registration_succeeded, 1);
            InterlockedExchange(&g_state, DS_STATE_DONE);
        }
    }
    ds_free_candidates();
}

/* ========================= publishing into the engine's boot snapshot ============================
 *
 * WHAT MAKES CONTENT PERMANENT. Every idResource is born map-scoped: the constructor (RVA 0x17FEAC0)
 * writes 1 or 2 into the resource level at +0x28, and the map-transition purge (0x1800E80, driven by
 * 0x1800E10 from UnloadMap 0x17C79C0 with mask 1 always and mask 2 on a full teardown) destructs
 * every entry whose level ANDs with the mask. Level 4 shares no bit with 1 or 2 and is exempt.
 *
 * Level 4 is reached in exactly one wholesale way: the engine's whole-registry promotion
 * (0x1801830), called ONCE from idCommonLocal::Init at 0x17C6479, which walks the global list of
 * per-type resource lists at 0x6217F90 and writes 4 into every entry of every list. Shipped editor
 * content is not permanent because the engine knows what it is. It is permanent because it happened
 * to be alive when that one snapshot was taken.
 *
 * WHY THE SERVICE PUBLISHES HERE AND NOT AT LOAD-STATE RUNNING. It used to publish at RUNNING, and a
 * live capture (Frida interceptor on the promotion, 2026-08-26) measured exactly how badly that
 * misses, relative to T = the promotion:
 *     T-146.9s   this service is armed; the override provider is installed
 *     T-131.6s   the provider begins serving the engine's own decls
 *     T-3.637s   the 64th and last startup decl load (snapEditorSettings/settings.decl, 70 KB)
 *     T          the engine's whole-registry promotion runs
 *     T+0.821s   load-state RUNNING
 *     T+2.267s   publication used to complete -- 2.3 seconds too late, every launch
 * Our content was therefore born map-scoped and the first playtest destroyed it, while the editor's
 * render entity kept the raw decl pointer it had cached and is never rebuilt on the return leg.
 *
 * WHY NOT PROMOTE OUR OWN CONTENT AFTERWARDS. Six attempts did, and every one failed:
 *   - a pinned md6Def whose model was not pinned -> the animator read a freed model
 *   - a surviving animWeb indexing an md6Def that had been rebuilt -> cyberdemon model, mancubus anim
 *   - a survivor enumerated by teardown after its peers were freed -> call through a freed vtable
 *   - a re-parse whose FreeData 0xFF-filled joint buffers the render thread was reading
 *   - promotion that followed one edge (md6Def -> the model at +0x60) and died on the first edge
 *     nobody had special-cased: entityDef -> edit.renderModelInfo.model
 *   - calling the engine's own promotion at RUNNING, which IS complete, but by then a map is loaded,
 *     so it also made that map's resources permanent and the engine could never free them again
 * A subset needs a closure the engine does not record: an idResource carries a name, an id, two flag
 * bytes and this level, and the purge decides by the level ALONE -- no refcount, no child list, no
 * back-reference table. The whole registry needs no closure but destroys the engine's own lifecycle,
 * because level 1 and level 2 are two deliberately different scopes, not one.
 *
 * SO WE DO NEITHER. We publish BEFORE the snapshot and let the engine take it. Our content is then
 * promoted by the same pass, at the same instant, as the content it depends on -- identical
 * treatment to shipped editor content because it IS the same treatment. No second promotion, no
 * subset, no edges, no per-type knowledge, and nothing that was map-scoped is made permanent,
 * because at 0x17C6479 no map exists yet.
 *
 * WHY IT IS SAFE TO PUBLISH HERE. The capture again: the engine's startup decl parsing finished 3.6
 * seconds earlier, and our provider had been serving the engine's own decls for over two minutes.
 * The decl registry is not merely constructed at this point -- the engine has just finished driving
 * the entire SnapMap decl type set through our file-shadow. The window is quiescent by measurement,
 * not by assumption.
 *
 * REFUSE AND CONTINUE IS THE CONTRACT. A publication failure now happens during boot, so it must
 * never be able to stop one. Everything this detour does is inside SEH, the engine's promotion is
 * called unconditionally on the way out, and a failure stays terminal exactly as it would at
 * RUNNING. The worst outcome is a launch with no published content -- never a launch that does not
 * happen. */
static void ds_publish_before_boot_promotion(void)
{
    /* The cut-content gates first, and synchronously. The blacklist matcher (0x31D0B0) is consulted
     * by idResourceList::LoadResource (0x1801380) and the static resource-handle resolve
     * (0x18008F0), which refuse a blacklisted name before the type parser ever sees it -- so a
     * package made of cut content does not load at all unless these are already live. Buffering
     * alone will not do: nothing drains the command buffer between here and the promotion. */
    if (!sh_package_requirements_apply_now((void *)g_execute_commands))
        backend_log("decl-server: package requirements were not applied before publication; cut-content gates may refuse some packages");

    /* Claim the same ARMED -> QUEUED transition the command buffer used to make. ds_apply_command
     * keeps its own QUEUED -> APPLYING claim, so its contract is unchanged; only delivery moved. */
    if (InterlockedCompareExchange(&g_state, DS_STATE_QUEUED, DS_STATE_ARMED) != DS_STATE_ARMED)
        return;
    ds_apply_command();
}

/* PROVE THE ORDERING, DO NOT ASSERT IT. Every log line this service writes about publishing before
 * the engine boot promotion is its own prose, and prose is not evidence: it would read exactly the
 * same if the hook were on the wrong function, or if the engine reached the promotion by some path
 * we never intercepted. So after the trampoline returns, read one published identity's resource
 * level straight back out of the engine.
 *
 * That reading is decisive because the level is the exact field the map-transition purge (0x1800E80)
 * tests, and the only thing that could have written 4 into it is the promotion we just called.
 * Level 4 means publication landed inside the engine's snapshot. Anything else means it did not,
 * and the line says so -- which is the failure this whole design exists to prevent, caught during
 * boot instead of at the first playtest.
 *
 * Read-only, SEH-guarded, and lookup-only (makeDefault=0, so it cannot fabricate the object it is
 * measuring). It never fails the run: a diagnostic that can break a boot is not a diagnostic. */
static void ds_report_promotion_outcome(void)
{
    void *registry = NULL;
    decl_type_by_name_fn type_by_name = NULL;
    decl_register_file_fn register_file = NULL;
    void *type_manager = NULL;
    void *decl = NULL;
    unsigned int level = 0;
    char line[352];

    if (!InterlockedCompareExchange(&g_registration_succeeded, 0, 0) || !g_probe_name[0]) return;
    if (!ds_decode_registry(&registry, &type_by_name, &register_file)) {
        backend_log("decl-server boot-promotion PROOF unavailable: the registry could not be re-decoded after the promotion; publication itself is unaffected");
        return;
    }
    __try {
        type_manager = type_by_name(registry, g_probe_type);
        decl = type_manager ? g_find_decl(type_manager, g_probe_name, 0) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        decl = NULL;
    }
    if (!decl || !ds_safe_read((const uint8_t *)decl + DS_RESOURCE_LEVEL_OFFSET,
                               &level, sizeof(level))) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "decl-server boot-promotion PROOF unavailable: published identity '%s' '%s' could not be read back after the promotion",
                    g_probe_type, g_probe_name);
        backend_log(line);
        return;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                level == DS_RESOURCE_LEVEL_STATIC
                    ? "decl-server boot-promotion PROOF: the engine whole-registry promotion returned and published identity '%s' '%s' now reads resource level %u -- it is inside the engine static set and the map-transition purge (masks 1|2) cannot free it"
                    : "decl-server boot-promotion PROOF FAILED: published identity '%s' '%s' reads resource level %u after the engine promotion returned, so publication did NOT land inside the engine snapshot and a playtest will destroy it",
                g_probe_type, g_probe_name, level);
    backend_log(line);
}

/* Runs on the engine main thread, inside idCommonLocal::Init, in place of the first 15 bytes of the
 * whole-registry promotion. Publishes, then calls the promotion through the trampoline so the
 * engine's own snapshot includes everything publication just created, then reads the result back. */
static void ds_boot_promotion_detour(void)
{
    /* The engine calls the promotion exactly once (single xref, 0x17C6479), but a detour may not
     * assume its target's call count. Publish on the first entry only; any later entry is a plain
     * pass-through. */
    if (InterlockedCompareExchange(&g_boot_promotion_entered, 1, 0) == 0) {
        __try {
            ds_publish_before_boot_promotion();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            backend_log("decl-server FAILED: publication faulted inside the engine boot promotion; the engine promotion still runs and startup continues");
            InterlockedExchange(&g_state, DS_STATE_FAILED);
        }
    }
    /* Unconditional, and last. Never NULL in practice -- the hook is not installed unless
     * install_inline_hook returned a trampoline -- but skipping it would leave the ENTIRE engine's
     * content map-scoped, which is far worse than anything publication can get wrong. */
    if (g_boot_promotion_original) g_boot_promotion_original();
    ds_report_promotion_outcome();
}


int sh_decl_server_registration_succeeded(void)
{
    return InterlockedCompareExchange(&g_registration_succeeded, 0, 0) != 0;
}

int sh_decl_server_install(const sig_result *results, size_t count,
                           const uint8_t *module_base, void *cmdsys)
{
    const sig_result *anchor;
    const sig_result *type_method;
    const sig_result *register_file;
    const sig_result *find_decl;
    const sig_result *source_find;
    uintptr_t add_command;
    uintptr_t idstr_ctor;
    uintptr_t idstr_dtor;
    uintptr_t boot_promote;
    uintptr_t execute_commands;
    int command_registered = 0;
    if (InterlockedCompareExchange(&g_state, DS_STATE_INSTALLING, DS_STATE_NEW) != DS_STATE_NEW)
        return 0;
    InterlockedExchange(&g_registration_succeeded, 0);
    if (!sh_user_overrides_enabled_for_launch()) {
        backend_log("decl-server disabled for this launch with the user override layer");
        InterlockedExchange(&g_state, DS_STATE_DONE);
        return 1;
    }
    if (!ds_capture_snapshot()) {
        InterlockedExchange(&g_state, DS_STATE_FAILED);
        ds_free_candidates();
        return 0;
    }
    if (g_candidate_count == 0) {
        InterlockedExchange(&g_state, DS_STATE_DONE);
        ds_free_candidates();
        return 1;
    }
    if (!sh_overrides_internal_decl_table_can_install()) {
        backend_log("decl-server REFUSED: exact per-decl provider table is unavailable, disabled, or already occupied");
        InterlockedExchange(&g_state, DS_STATE_FAILED);
        ds_free_candidates();
        return 0;
    }

    anchor = ds_result(results, count, "DeclRegistryAnchor");
    type_method = ds_result(results, count, "DeclTypeByName");
    register_file = ds_result(results, count, "DeclRegisterFile");
    find_decl = ds_result(results, count, "DeclFind");
    source_find = ds_result(results, count, "DeclSourceFind");
    add_command = ds_clean_addr(results, count, "AddCommand");
    idstr_ctor = ds_clean_addr(results, count, "IdStrCtor");
    idstr_dtor = ds_clean_addr(results, count, "IdStrDtor");
    /* Both are required, not optional. Without the promotion this service has no publication
     * trigger at all, and without the drain the cut-content gates would not be live when it
     * publishes. Refusing is the correct outcome on a build we cannot serve correctly. */
    boot_promote = ds_clean_addr(results, count, "ResourceStaticPromote");
    execute_commands = ds_clean_addr(results, count, "CmdExecuteBuffer");
    if (!anchor || anchor->status != SIG_OK || !type_method || !register_file || !find_decl ||
        !source_find || source_find->status != SIG_OK || !source_find->addr ||
        type_method->status != SIG_OK || register_file->status != SIG_OK ||
        find_decl->status != SIG_OK || !type_method->addr ||
        !register_file->addr || !find_decl->addr ||
        !idstr_ctor || !idstr_dtor || !boot_promote || !execute_commands ||
        !add_command || !cmdsys || !module_base ||
        !ds_clean_at_pinned_rva(results, count, "DeclRegistryAnchor", module_base, DS_PINNED_ANCHOR_RVA) ||
        !ds_clean_at_pinned_rva(results, count, "DeclTypeByName", module_base, DS_PINNED_TYPE_RVA) ||
        !ds_clean_at_pinned_rva(results, count, "DeclRegisterFile", module_base, DS_PINNED_REGISTER_RVA) ||
        !ds_clean_at_pinned_rva(results, count, "DeclFind", module_base, DS_PINNED_FIND_RVA) ||
        !ds_clean_at_pinned_rva(results, count, "DeclSourceFind", module_base, DS_PINNED_SOURCE_FIND_RVA) ||
        !ds_clean_at_pinned_rva(results, count, "IdStrCtor", module_base, DS_PINNED_IDSTR_CTOR_RVA) ||
        !ds_clean_at_pinned_rva(results, count, "IdStrDtor", module_base, DS_PINNED_IDSTR_DTOR_RVA) ||
        !ds_clean_at_pinned_rva(results, count, "ResourceStaticPromote", module_base,
                                DS_PINNED_BOOT_PROMOTE_RVA) ||
        !ds_clean_at_pinned_rva(results, count, "CmdExecuteBuffer", module_base,
                                DS_PINNED_CMD_EXECUTE_RVA)) {
        backend_log("decl-server REFUSED: clean pinned-build registry/idStr/boot-promotion ABI, command-system, or module-base dependency missing");
        InterlockedExchange(&g_state, DS_STATE_FAILED);
        ds_free_candidates();
        return 0;
    }

    g_cmdsys = cmdsys;
    g_module_base = module_base;
    g_add_command = (add_command_fn)add_command;
    g_registry_anchor = (const uint8_t *)anchor->addr;
    g_expected_type_by_name = (decl_type_by_name_fn)type_method->addr;
    g_expected_register_file = (decl_register_file_fn)register_file->addr;
    g_idstr_ctor = (idstr_ctor_fn)idstr_ctor;
    g_idstr_dtor = (idstr_dtor_fn)idstr_dtor;
    g_find_source = (decl_source_find_fn)source_find->addr;
    g_find_decl = (decl_find_fn)find_decl->addr;
    g_execute_commands = (cmd_execute_buffer_fn)execute_commands;

    /* Register the one-shot apply command. It is no longer the delivery vehicle -- the boot
     * promotion detour below calls ds_apply_command directly, already on the engine main
     * thread -- but it stays registered so the pass has a named, state-guarded entry point
     * that can be re-issued for diagnostics without a second code path. */
    __try {
        g_add_command(g_cmdsys, DS_INTERNAL_COMMAND, (void *)ds_apply_command,
                      "Snapmap+ internal one-shot decltree registration", NULL, 2u);
        command_registered = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        command_registered = 0;
    }
    if (!command_registered) {
        backend_log("decl-server REFUSED: internal main-thread command registration failed");
        InterlockedExchange(&g_state, DS_STATE_FAILED);
        ds_free_candidates();
        return 0;
    }

    /* ARM BEFORE HOOKING, never after. The engine reaches the promotion on its own schedule and
     * the detour claims ARMED -> QUEUED; if the hook were live first, a fast boot could enter it
     * while this service was still NEW and the claim would silently fail. */
    InterlockedExchange(&g_state, DS_STATE_ARMED);

    /* The publication trigger. This is the ONLY engine code patch this service installs: there is
     * still no DeclFind detour, no lookup interception, no raw object cache and no hot reload. The
     * target is an engine function we do not modify -- the detour publishes, then calls it. */
    g_boot_promotion_original =
        (resource_promote_static_fn)install_inline_hook((void *)boot_promote,
                                                        (void *)ds_boot_promotion_detour,
                                                        DS_BOOT_PROMOTE_STOLEN_BYTES);
    if (!g_boot_promotion_original) {
        backend_log("decl-server REFUSED: the engine boot-promotion detour could not be installed; nothing is published rather than publishing content a playtest would destroy");
        InterlockedExchange(&g_state, DS_STATE_FAILED);
        ds_free_candidates();
        return 0;
    }
    {
        char line[320];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "decl-server armed: immutable launch snapshot has %d candidate(s), %zu body bytes; publishes inside idCommonLocal::Init immediately before the engine whole-registry resource promotion (0x%X); no hot reload",
                    g_candidate_count, g_total_bytes, DS_PINNED_BOOT_PROMOTE_RVA);
        backend_log(line);
    }
    return 1;
}
