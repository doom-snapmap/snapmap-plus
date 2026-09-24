#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include "audio_banks_native.h"
#include "audio_banks.h"
#include "audio_bank_commands.h"
#include "audio_files_native.h"
#include "package_runtime.h"
#include "backend_log.h"
#include "hook.h"
#include "audio_effective_policy.h"

typedef int (*ab_load_fn)(const char *name, int pool, uint32_t *id);
typedef int (*ab_unload_fn)(uint32_t id, const void *memory, int *pool);
static ab_load_fn g_load;
static ab_unload_fn g_unload;
static sh_audio_bank_commands g_commands;
static sh_audio_registry g_registry;
static sh_audio_limits_native g_limits;
static sh_audio_environment_native g_environment;
static sh_audio_state_native g_state;
static sh_audio_parameter_native g_parameter;
static sh_audio_bank_gate g_bank_gate;
static void *g_bank_gate_hook;
static int g_shared_recovery;
static sh_audio_id_pool *g_ids;
static uint32_t *g_original_ids;
static size_t g_original_id_count;
static sh_audio_bank *g_banks;
static sh_audio_bank_slot *g_slots;
static size_t g_count;
static size_t g_installed_count;
static size_t g_packaged_only;
/* provisional records a catalog built from loose bytes because the installed
 * originals index was not ready. It is rebuilt once that index answers. */
static int g_catalog_ready, g_catalog_provisional;
static char g_directory[MAX_PATH];
static char g_language[MAX_PATH];
/* The engine's configured bank prefix, engine-path form with its own trailing
 * separator. Discovery walks that prefix, not an assumed plain root. */
static char g_prefix[MAX_PATH];
struct sh_audio_bank_addition {
    sh_audio_bank_slot *before;
    size_t count;
    DWORD thread;
    int prepared;
    /* Retained-program mode: programs appended from first_program and the
     * bank loads this addition took belong to it until accept or rollback. */
    int programs;
    size_t first_program;
    uint32_t *loaded;
    size_t loaded_count;
};
static sh_audio_bank_addition *g_addition;
static void an_programs_addition_rollback(sh_audio_bank_addition *addition);
static void an_programs_addition_accept(sh_audio_bank_addition *addition);

static int an_error(char *error, size_t capacity, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); return 0; }

static int an_dispatch(void *manager,const void *command)
{ return sh_audio_bank_gate_dispatch(&g_bank_gate,manager,command); }

void sh_audio_banks_native_install(const sig_result *results, size_t count)
{
    char *slash;
    DWORD length;
    g_load = (ab_load_fn)sig_addr_by_name(results, count, "AudioBankLoad");
    g_unload = (ab_unload_fn)sig_addr_by_name(results, count, "AudioBankUnload");
    if (!sh_audio_bank_commands_bind(&g_commands,
        (const unsigned char *)GetModuleHandleA(NULL), results, count))
        backend_log("audio banks: numeric command binding unavailable");
    if (!sh_audio_registry_bind(&g_registry,
        (const unsigned char *)GetModuleHandleA(NULL), results, count))
        backend_log("audio banks: read-only registry binding unavailable");
    if (!sh_audio_limits_native_bind(&g_limits,
        (const unsigned char *)GetModuleHandleA(NULL), results, count))
        backend_log("audio banks: playback settings binding unavailable");
    if (!sh_audio_environment_native_bind(&g_environment,
        (const unsigned char *)GetModuleHandleA(NULL), results, count, &g_limits))
        backend_log("audio banks: environment settings binding unavailable");
    if (!sh_audio_state_native_bind(&g_state,
        (const unsigned char *)GetModuleHandleA(NULL), results, count, &g_limits))
        backend_log("audio banks: state settings binding unavailable");
    if (!sh_audio_parameter_native_bind(&g_parameter,
        (const unsigned char *)GetModuleHandleA(NULL), results, count, &g_limits))
        backend_log("audio banks: parameter settings binding unavailable");
    if (sh_audio_bank_gate_bind(&g_bank_gate,(const unsigned char *)GetModuleHandleA(NULL),results,count,&g_limits) &&
        sh_audio_bank_gate_initialize(&g_bank_gate)) {
        g_bank_gate_hook=hook_prepare((void *)g_bank_gate.entry,an_dispatch,16);
        g_bank_gate.original=(sh_audio_bank_dispatch_fn)g_bank_gate_hook;
        if(g_bank_gate_hook && hook_commit(g_bank_gate_hook)==B2_PATCH_OK)g_bank_gate.installed=1;
        else backend_log("audio banks: publication gate installation failed; native ownership retained");
    } else backend_log("audio banks: publication gate binding unavailable");
    length = GetModuleFileNameA(NULL, g_directory, sizeof(g_directory));
    if (!length || length >= sizeof(g_directory) || !(slash = strrchr(g_directory, '\\'))) {
        g_directory[0] = 0; return;
    }
    *slash = 0;
    if (strcat_s(g_directory, sizeof(g_directory), "\\base\\sound\\soundbanks\\pc"))
        g_directory[0] = 0;
}

int sh_audio_banks_native_shared_begin(sh_audio_bank_scope *scope,char *error,size_t capacity)
{
    if(g_limits.pending || g_environment.pending || g_state.pending || g_parameter.pending || g_commands.uncertain ||
        (g_addition && (!g_addition->prepared || g_addition->thread!=GetCurrentThreadId())))
        return an_error(error,capacity,"native audio preparation or recovery must finish before shared publication");
    return sh_audio_bank_gate_begin(&g_bank_gate,scope,error,capacity);
}
int sh_audio_banks_native_shared_recover(sh_audio_bank_scope *scope,char *error,size_t capacity)
{
    DWORD thread=GetCurrentThreadId();
    if((!g_limits.pending && !g_environment.pending && !g_state.pending && !g_parameter.pending) ||
        (g_limits.pending && g_limits.pending->thread!=thread) ||
        (g_environment.pending && g_environment.pending->thread!=thread) ||
        (g_state.pending && g_state.pending->thread!=thread) ||
        (g_parameter.pending && g_parameter.pending->thread!=thread))
        return an_error(error,capacity,"no retained shared audio recovery belongs to this thread");
    if(!sh_audio_bank_gate_begin(&g_bank_gate,scope,error,capacity))return 0;
    g_shared_recovery=1;return 1;
}
int sh_audio_banks_native_shared_ready(const sh_audio_bank_scope *scope,char *error,size_t capacity)
{
    return scope && scope->owner==&g_bank_gate && sh_audio_bank_gate_ready(scope,error,capacity) &&
        (!g_limits.pending || sh_audio_limits_native_ready(g_limits.pending,error,capacity)) &&
        (!g_environment.pending || sh_audio_environment_native_ready(g_environment.pending,error,capacity)) &&
        (!g_state.pending || sh_audio_state_native_ready(g_state.pending,error,capacity)) &&
        (!g_parameter.pending || sh_audio_parameter_native_ready(g_parameter.pending,error,capacity));
}
int sh_audio_banks_native_shared_end(sh_audio_bank_scope *scope,char *error,size_t capacity)
{
    if(!scope || scope->owner!=&g_bank_gate)return an_error(error,capacity,"shared publication scope is unavailable");
    int complete=!g_limits.pending && !g_environment.pending && !g_state.pending && !g_parameter.pending;
    int released=sh_audio_bank_gate_end(scope,error,capacity);
    if(scope->owner)return released;
    g_shared_recovery=0;
    return !complete ? an_error(error,capacity,"shared audio recovery remains owned after releasing the bank gate") : released;
}

int sh_audio_banks_native_limits_apply(const sh_package_audio_program *program,
    sh_audio_limits_change *change,char *error,size_t capacity)
{
    if(g_shared_recovery)return an_error(error,capacity,"shared audio recovery must finish before new settings");
    if(!sh_audio_bank_gate_ready(g_bank_gate.scope,error,capacity))return 0;
    if(g_commands.uncertain || (g_addition && (!g_addition->prepared || g_addition->thread!=GetCurrentThreadId())))
        return an_error(error,capacity,"native bank activation must complete before publishing playback settings");
    return sh_audio_limits_native_apply(&g_limits,sh_package_audio_program_bound_shared(program),change,error,capacity);
}

int sh_audio_banks_native_environment_apply(const sh_package_audio_program *program,
    sh_audio_environment_change *change,char *error,size_t capacity)
{
    if(g_shared_recovery)return an_error(error,capacity,"shared audio recovery must finish before new settings");
    if(!sh_audio_bank_gate_ready(g_bank_gate.scope,error,capacity))return 0;
    if(g_commands.uncertain || (g_addition && (!g_addition->prepared || g_addition->thread!=GetCurrentThreadId())))
        return an_error(error,capacity,"native bank activation must complete before publishing environment settings");
    return sh_audio_environment_native_apply(&g_environment,sh_package_audio_program_bound_shared(program),change,error,capacity);
}

int sh_audio_banks_native_state_apply(const sh_package_audio_program *program,
    sh_audio_state_change *change,char *error,size_t capacity)
{
    if(g_shared_recovery)return an_error(error,capacity,"shared audio recovery must finish before new settings");
    if(!sh_audio_bank_gate_ready(g_bank_gate.scope,error,capacity))return 0;
    if(g_commands.uncertain || (g_addition && (!g_addition->prepared || g_addition->thread!=GetCurrentThreadId())))
        return an_error(error,capacity,"native bank activation must complete before publishing state settings");
    return sh_audio_state_native_apply(&g_state,sh_package_audio_program_bound_shared(program),change,error,capacity);
}

int sh_audio_banks_native_parameter_apply(const sh_package_audio_program *program,
    sh_audio_parameter_change *change,char *error,size_t capacity)
{
    if(g_shared_recovery)return an_error(error,capacity,"shared audio recovery must finish before new settings");
    if(!sh_audio_bank_gate_ready(g_bank_gate.scope,error,capacity))return 0;
    if(g_commands.uncertain || (g_addition && (!g_addition->prepared || g_addition->thread!=GetCurrentThreadId())))
        return an_error(error,capacity,"native bank activation must complete before publishing parameter settings");
    return sh_audio_parameter_native_apply(&g_parameter,sh_package_audio_program_bound_shared(program),change,error,capacity);
}

static void an_clear_catalog(void)
{
    for (size_t i = 0; i < g_count; ++i) {
        sh_audio_bank_free(g_banks + i); free((void *)g_slots[i].name);
    }
    free(g_banks); free(g_slots); g_banks = NULL; g_slots = NULL; g_count = 0;
    g_installed_count = 0; g_packaged_only = 0;
}

/* A forward window over one bank. The chunk walk steps over media payloads, so
 * a single buffer turns thousands of small header reads into a few reads. */
typedef struct an_reader {
    const char *path;             /* engine path served by the installed index */
    FILE *stream;                 /* loose original, when that index is not ready */
    uint64_t length, at;
    size_t held;
    char *error;
    size_t capacity;
    unsigned char window[65536];
} an_reader;

static int an_fill(an_reader *reader, uint64_t offset)
{
    size_t want = sizeof(reader->window);
    if (offset >= reader->length) return 0;
    if (reader->length - offset < want) want = (size_t)(reader->length - offset);
    if (reader->path) {
        if (sh_package_runtime_audio_original_read(reader->path, offset, reader->window,
            want, NULL, reader->error, reader->capacity) != 1) return 0;
    } else if (_fseeki64(reader->stream, (__int64)offset, SEEK_SET) ||
        fread(reader->window, 1, want, reader->stream) != want) return 0;
    reader->at = offset; reader->held = want; return 1;
}

static int an_read(void *context, uint64_t offset, void *out, size_t length)
{
    an_reader *reader = context;
    unsigned char *cursor = out;
    while (length) {
        size_t available;
        if (offset < reader->at || offset - reader->at >= reader->held)
            if (!an_fill(reader, offset)) return 0;
        available = reader->held - (size_t)(offset - reader->at);
        if (available > length) available = length;
        memcpy(cursor, reader->window + (size_t)(offset - reader->at), available);
        cursor += available; offset += available; length -= available;
    }
    return 1;
}

static int an_add(sh_audio_bank_source source, const char *name, char *error, size_t capacity)
{
    sh_audio_bank bank = {0};
    sh_audio_bank *banks;
    sh_audio_bank_slot *slots;
    char *copy;
    if (sh_audio_bank_read_source(source, &bank, error, capacity) != 1) return 0;
    if (!sh_audio_bank_names_identity(name, bank.id)) {
        sh_audio_bank_free(&bank);
        return an_error(error, capacity, "audio bank filename does not match its native identity");
    }
    /* One candidate per name now, so a repeated identity is a real collision
     * rather than the same bank reached through a second language directory. */
    for (size_t i = 0; i < g_count; ++i) if (g_slots[i].id == bank.id) {
        sh_audio_bank_free(&bank);
        return an_error(error, capacity, "installed audio banks have ambiguous identities");
    }
    if (g_count >= SIZE_MAX / sizeof(*banks) || g_count >= SIZE_MAX / sizeof(*slots)) goto memory;
    banks = (sh_audio_bank *)realloc(g_banks, (g_count+1)*sizeof(*banks));
    if (!banks) goto memory;
    g_banks = banks;
    slots = (sh_audio_bank_slot *)realloc(g_slots, (g_count+1)*sizeof(*slots));
    if (!slots) goto memory;
    g_slots = slots;
    copy = _strdup(name);
    if (!copy) goto memory;
    g_banks[g_count] = bank;
    memset(g_slots+g_count, 0, sizeof(*g_slots));
    g_slots[g_count].name = copy; g_slots[g_count].id = bank.id; ++g_count;
    return 1;
memory:
    sh_audio_bank_free(&bank);
    return an_error(error, capacity, "audio bank catalog allocation failed");
}

typedef struct an_names { char **items; size_t count; } an_names;

static void an_names_free(an_names *names)
{
    for (size_t i = 0; i < names->count; ++i) free(names->items[i]);
    free(names->items); names->items = NULL; names->count = 0;
}

/* One directory, no recursion: the engine mounts the bank root and exactly one
 * language directory, so a deeper sweep indexes banks it never opens. */
static int an_collect(const char *directory, int required, an_names *names,
    char *error, size_t capacity)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA data;
    HANDLE handle;
    int result = 1;
    DWORD last;
    /* Match on the returned long name: a suffix pattern would also match an
     * unrelated file through its generated short name. */
    if (snprintf(pattern, sizeof(pattern), "%s\\*", directory) >= (int)sizeof(pattern))
        return an_error(error, capacity, "installed audio bank path is too long");
    handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        last = GetLastError();
        if (!required && (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND)) return 1;
        return an_error(error, capacity, "installed audio bank directory is unavailable");
    }
    do {
        char **grown;
        size_t i, length = strlen(data.cFileName);
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (length <= 4 || _stricmp(data.cFileName + length - 4, ".bnk")) continue;
        for (i = 0; i < names->count; ++i) if (!_stricmp(names->items[i], data.cFileName)) break;
        if (i < names->count) continue;
        if (names->count >= SIZE_MAX / sizeof(*grown) - 1 ||
            !(grown = (char **)realloc(names->items, (names->count+1)*sizeof(*grown)))) {
            result = an_error(error, capacity, "audio bank catalog allocation failed"); break;
        }
        names->items = grown;
        if (!(grown[names->count] = _strdup(data.cFileName))) {
            result = an_error(error, capacity, "audio bank catalog allocation failed"); break;
        }
        ++names->count;
    } while (FindNextFileA(handle, &data));
    last = GetLastError(); FindClose(handle);
    if (result && last != ERROR_NO_MORE_FILES)
        result = an_error(error, capacity, "installed audio bank enumeration failed");
    return result;
}

static char *an_compose(const char *language, const char *name)
{
    size_t length = strlen(name) + (language ? strlen(language) : 0) + strlen(g_prefix) + 32;
    char *path = malloc(length);
    if (!path) return NULL;
    if (language) snprintf(path, length, "sound/soundbanks/pc/%s%s/%s", g_prefix, language, name);
    else snprintf(path, length, "sound/soundbanks/pc/%s%s", g_prefix, name);
    return path;
}

/* Join the installed bank root with the engine's prefix and an optional
 * language directory, in Windows form. Returns 0 when the path would not fit. */
static int an_directory_of(char *out, size_t capacity, const char *language)
{
    size_t used;
    if (snprintf(out, capacity, "%s\\%s%s", g_directory, g_prefix,
        language ? language : "") >= (int)capacity) return 0;
    for (char *p = out; *p; ++p) if (*p == '/') *p = '\\';
    used = strlen(out);
    while (used > 1 && out[used-1] == '\\') out[--used] = 0;
    return 1;
}

/* Native bank opening tries the current locale before the nonlocalized root.
 * Index whichever candidate it would actually open, through the installed
 * originals so a mounted package takes precedence over a loose file. */
static int an_index(const char *name, int indexed, char *error, size_t capacity)
{
    char *candidates[2] = {NULL, NULL};
    an_reader *reader = NULL;
    int result = 0;
    if (!sh_audio_files_native_bank_paths(name, candidates, candidates+1)) {
        if (g_language[0] && !(candidates[0] = an_compose(g_language, name))) goto allocation;
        if (!(candidates[1] = an_compose(NULL, name))) goto allocation;
    }
    reader = calloc(1, sizeof(*reader));
    if (!reader) goto allocation;
    reader->error = error; reader->capacity = capacity;
    if (indexed) for (size_t i = 0; i < 2; ++i) {
        int found;
        if (!candidates[i]) continue;
        found = sh_package_runtime_audio_original_read(candidates[i], 0, NULL, 0,
            &reader->length, error, capacity);
        if (found < 0) goto done;
        if (found) { reader->path = candidates[i]; break; }
    }
    if (!reader->path) {
        /* No installed original answered, or that index is not ready yet. */
        char path[MAX_PATH], root[MAX_PATH], localized[MAX_PATH];
        const char *directory;
        __int64 length;
        if (!an_directory_of(root, sizeof(root), NULL)) {
            an_error(error, capacity, "installed audio bank path is too long"); goto done;
        }
        directory = root;
        if (g_language[0] && an_directory_of(localized, sizeof(localized), g_language) &&
            snprintf(path, sizeof(path), "%s\\%s", localized, name) < (int)sizeof(path) &&
            !_access(path, 0)) directory = localized;
        if (snprintf(path, sizeof(path), "%s\\%s", directory, name) >= (int)sizeof(path) ||
            fopen_s(&reader->stream, path, "rb") || !reader->stream ||
            _fseeki64(reader->stream, 0, SEEK_END) || (length = _ftelli64(reader->stream)) < 0) {
            an_error(error, capacity, "installed audio bank cannot be read"); goto done;
        }
        reader->length = (uint64_t)length;
    }
    result = an_add((sh_audio_bank_source){reader, reader->length, an_read}, name, error, capacity);
    goto done;
allocation:
    an_error(error, capacity, "audio bank catalog allocation failed");
done:
    if (reader && reader->stream) fclose(reader->stream);
    free(reader); free(candidates[0]); free(candidates[1]);
    return result;
}

/* A packaged identity with no installed file name is still openable: the native
 * loader reopens a bank it knows only by its identity through a decimal file
 * stem, and the installed package index answers that name from inside the .pck
 * it lives in. Index it under that name so the catalog describes and serves it.
 * One that still cannot be read stays counted and logged rather than failing
 * the whole catalog for a bank nothing has requested. */
typedef struct an_packaged_scan {
    size_t *unnamed;
    int indexed;
    char *error;
    size_t capacity;
} an_packaged_scan;

static int an_packaged(void *context, uint32_t id, uint32_t language)
{
    an_packaged_scan *scan = context;
    char name[32], detail[320];
    (void)language;
    for (size_t i = 0; i < g_count; ++i) if (g_slots[i].id == id) return 1;
    snprintf(name, sizeof(name), "%u.bnk", (unsigned)id);
    if (an_index(name, scan->indexed, scan->error, scan->capacity)) return 1;
    snprintf(detail, sizeof(detail),
        "native audio catalog: packaged bank %u has no installed name and could not be opened (%s)",
        (unsigned)id, scan->error && scan->error[0] ? scan->error : "no reason reported");
    backend_log(detail);
    if (scan->error && scan->capacity) scan->error[0] = 0;
    ++*scan->unnamed; return 1;
}

static int an_inventory(void *context, const sh_audio_original_bank *rows, size_t count,
    char *error, size_t capacity)
{
    (void)context;
    if (g_count || count > SIZE_MAX / sizeof(*g_banks) || count > SIZE_MAX / sizeof(*g_slots))
        return an_error(error, capacity, "audio inventory destination is not empty");
    g_banks = calloc(count ? count : 1, sizeof(*g_banks));
    g_slots = calloc(count ? count : 1, sizeof(*g_slots));
    if (!g_banks || !g_slots) return an_error(error, capacity, "audio inventory allocation failed");
    for (size_t i = 0; i < count; i++) {
        if (!sh_audio_bank_copy(&rows[i].metadata, g_banks + i))
            return an_error(error, capacity, "audio inventory metadata copy failed");
        g_count++;
        g_slots[i].id = rows[i].metadata.id; g_slots[i].name = _strdup(rows[i].filename);
        if (!g_slots[i].name) return an_error(error, capacity, "audio inventory name copy failed");
    }
    return 1;
}

static int an_id_compare(const void *a, const void *b)
{ uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;return x<y ? -1 : x>y; }
static int an_id_occupied(void *context,sh_audio_identity_domain domain,uint32_t id,char *error,size_t capacity)
{
    (void)context;(void)domain;
    if(g_original_id_count && bsearch(&id,g_original_ids,g_original_id_count,sizeof(id),an_id_compare))return 2;
    return sh_audio_registry_occupied(&g_registry,id,error,capacity);
}
int sh_audio_banks_native_reserve(sh_package_audio_program *program,
    sh_package_compilation *compiled,sh_audio_originals *originals,
    sh_audio_id_lease **lease,char *error,size_t capacity)
{
    const uint32_t *ids=NULL;size_t count=0,total,used=0;uint32_t *combined=NULL;
    if(error && capacity)*error=0;
    if(!program || !compiled || !lease || *lease)return an_error(error,capacity,"invalid audio reservation transaction");
    if(sh_audio_originals_ids(originals,&ids,&count,error,capacity)!=1)return 0;
    if(count>SIZE_MAX-g_original_id_count || (total=count+g_original_id_count)>SIZE_MAX/sizeof(*combined))
        return an_error(error,capacity,"original audio identity inventory is too large");
    combined=malloc((total ? total : 1)*sizeof(*combined));
    if(!combined)return an_error(error,capacity,"cannot retain original audio identities");
    size_t a=0,b=0;
    while(a<g_original_id_count || b<count) {
        uint32_t value=b==count || (a<g_original_id_count && g_original_ids[a]<ids[b]) ? g_original_ids[a++] : ids[b++];
        if(!used || value!=combined[used-1])combined[used++]=value;
    }
    /* Process-lifetime union: a later language/prefix cannot make an earlier
     * installed identity eligible for reuse while its native objects survive. */
    free(g_original_ids);g_original_ids=combined;g_original_id_count=used;
    if(!g_ids)g_ids=sh_audio_id_pool_open(an_id_occupied,NULL);
    sh_audio_id_lease *pending=g_ids ? sh_audio_id_lease_begin(g_ids) : NULL;
    if(!pending)return an_error(error,capacity,"cannot allocate native audio reservation transaction");
    if(!sh_package_audio_program_bind(program,compiled,sh_audio_id_reserve,pending,error,capacity)) {
        sh_audio_id_lease_release(pending);return 0;
    }
    *lease=pending;return 1;
}
static int an_build(char *error, size_t capacity)
{
    an_names names = {0};
    wchar_t language[260];
    char directory[MAX_PATH];
    int indexed = sh_package_runtime_audio_originals_ready(), result = 0;
    g_language[0] = 0; g_prefix[0] = 0;
    if (!sh_audio_files_native_language(language, 260) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, language, -1,
            g_language, sizeof(g_language), NULL, NULL)) g_language[0] = 0;
    for (char *p = g_language; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    (void)sh_audio_files_native_bank_prefix(g_prefix, sizeof(g_prefix));
    if (indexed) {
        if (sh_package_runtime_audio_original_banks(g_language, g_prefix, an_inventory, NULL,
            error, capacity) != 1) goto done;
        goto captured;
    }
    if (!an_directory_of(directory, sizeof(directory), NULL)) {
        an_error(error, capacity, "installed audio bank path is too long"); goto done;
    }
    if (!an_collect(directory, 1, &names, error, capacity)) goto done;
    if (g_language[0]) {
        if (!an_directory_of(directory, sizeof(directory), g_language)) {
            an_error(error, capacity, "installed audio bank path is too long"); goto done;
        }
        if (!an_collect(directory, 0, &names, error, capacity)) goto done;
    }
    for (size_t i = 0; i < names.count; ++i)
        if (!an_index(names.items[i], indexed, error, capacity)) goto done;
    {
        an_packaged_scan scan = {&g_packaged_only, indexed, error, capacity};
        if (indexed && sh_package_runtime_audio_packaged_banks(an_packaged, &scan,
            error, capacity) < 0) goto done;
    }
captured:
    g_installed_count = g_count;
    g_catalog_ready = 1; g_catalog_provisional = !indexed || !g_language[0];
    result = 1;
done:
    an_names_free(&names);
    if (!result) an_clear_catalog();
    return result;
}

static int an_load(void *context, const char *name, uint32_t *id)
{
    uint32_t numeric;
    (void)context;
    /* A numeric filename can also be an authored name whose hash is the bank
     * identity. The indexed header decides which native overload is correct. */
    if (sh_audio_bank_numeric_identity(name, &numeric)) {
        for (size_t i = 0; i < g_count; ++i)
            if (g_slots[i].id == numeric && !_stricmp(g_slots[i].name, name)) {
                *id = numeric;
                return sh_audio_bank_commands_load(&g_commands, numeric);
            }
    }
    __try { return g_load(name, -1, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int an_unload(void *context, uint32_t id)
{
    (void)context;
    __try { return g_unload(id, NULL, NULL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

/* Retain stable native ownership slots across provider changes. New entries
 * have no installed event metadata; their prospective metadata stays in the
 * independently owned compiled snapshot. No native calls occur here. */
static int an_extend(const sh_package_audio_bank *packages, size_t count,
    char *error, size_t capacity)
{
    size_t total = g_count;
    sh_audio_bank *banks = NULL;
    sh_audio_bank_slot *slots = NULL;
    for (size_t i = 0; i < count; ++i) {
        const sh_package_audio_bank *p = packages+i;
        uint32_t id;
        if (!p->path || !p->metadata || !(id = p->metadata->id) ||
            !sh_audio_bank_names_identity(p->filename, id) ||
            (p->metadata->event_count && !p->metadata->events))
            return an_error(error, capacity, "compiled audio bank identity is invalid");
        /* The native loader opens the slot's name, so a second spelling of the
         * same identity would leave the compiled file at a path it never asks
         * for. That is a collision, not an alias of the installed bank. */
        for (size_t j = 0; j < g_count; ++j)
            if (g_slots[j].id == id && _stricmp(g_slots[j].name, p->filename))
                return an_error(error, capacity, "compiled and installed audio bank identities collide");
        for (size_t j = 0; j < i; ++j)
            if (packages[j].metadata->id == id && _stricmp(packages[j].filename, p->filename))
                return an_error(error, capacity, "compiled audio bank identities collide");
        if (total == SIZE_MAX) return an_error(error, capacity, "audio bank catalog is too large");
        ++total;
    }
    if (!count) return 1;
    if (total > SIZE_MAX / sizeof(*banks) || total > SIZE_MAX / sizeof(*slots)) goto memory;
    banks = calloc(total, sizeof(*banks)); slots = calloc(total, sizeof(*slots));
    if (!banks || !slots) goto memory;
    if (g_count) {
        memcpy(banks, g_banks, g_count*sizeof(*banks));
        memcpy(slots, g_slots, g_count*sizeof(*slots));
    }
    total = g_count;
    for (size_t i = 0; i < count; ++i) {
        size_t j;
        for (j = 0; j < total; ++j) if (slots[j].id == packages[i].metadata->id) break;
        if (j < total) continue;
        slots[total].name = _strdup(packages[i].filename);
        if (!slots[total].name) {
            for (size_t n = g_count; n < total; ++n) free((void *)slots[n].name);
            goto memory;
        }
        /* No installed metadata: the identity exists only in this provider. */
        slots[total].id = packages[i].metadata->id;
        ++total;
    }
    free(g_banks); free(g_slots);
    g_banks = banks; g_slots = slots; g_count = total; return 1;
memory:
    free(banks); free(slots);
    return an_error(error, capacity, "audio bank catalog allocation failed");
}

static int an_effective(const char *name, const sh_package_audio_bank *packages,
    size_t count, const sh_package_audio_bank **selected, char *error, size_t capacity)
{
    char *localized = NULL, *fallback = NULL;
    const sh_package_audio_bank *root = NULL;
    sh_package_original_identity original = {0};
    int result = 1;
    *selected = NULL;
    if (!sh_audio_files_native_bank_paths(name, &localized, &fallback))
        return an_error(error, capacity, "native bank lookup paths are unavailable");
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(packages[i].path, localized)) *selected = packages+i;
        if (!strcmp(packages[i].path, fallback)) root = packages+i;
    }
    /* Native name loading tries localized packed/loose originals before the
     * unlocalized package file. Do not report that shadowed file as activated. */
    if (!*selected && root) {
        result = sh_package_runtime_audio_original(localized, &original, error, capacity);
        if (result >= 0) { if (!original.scope) *selected = root; result = 1; }
    }
    free(localized); free(fallback);
    return result > 0;
}

static void an_retire_slots(const sh_package_audio_bank *packages, size_t count)
{
    size_t retained = g_installed_count;
    for (size_t i = g_installed_count; i < g_count; ++i) {
        int keep = g_slots[i].owned || g_slots[i].uncertain || g_slots[i].restore_native;
        for (size_t j = 0; !keep && j < count; ++j)
            keep = packages[j].metadata->id == g_slots[i].id;
        if (keep) {
            g_banks[retained] = g_banks[i]; g_slots[retained++] = g_slots[i];
        } else { sh_audio_bank_free(g_banks+i); free((void *)g_slots[i].name); }
    }
    g_count = retained;
}

/* A catalog built without the installed originals index indexes loose bytes a
 * mounted package may shadow. Rebuild it once that index answers, but only
 * while no slot holds native ownership or a restoration obligation. */
static int an_stale(void)
{
    if (!g_catalog_provisional || !sh_package_runtime_audio_originals_ready()) return 0;
    if (g_count != g_installed_count) return 0;
    for (size_t i = 0; i < g_count; ++i)
        if (g_slots[i].owned || g_slots[i].uncertain || g_slots[i].restore_native) return 0;
    return 1;
}

static int an_activate(sh_audio_bank_addition *addition, const uint32_t *events, size_t count,
    const sh_package_audio_bank *packages, size_t package_count,
    const sh_package_audio_media *media, size_t media_count,
    char *error, size_t capacity)
{
    unsigned char *wanted = NULL;
    const sh_package_file_identity **content = NULL;
    const sh_package_audio_bank **effective = NULL;
    const sh_audio_bank **metadata = NULL;
    size_t matched = 0, needed = 0, owned = 0, replaced = 0;
    size_t dependencies = 0, optional = 0, carried = 0, streamed = 0;
    char carried_detail[256];
    int result = 0;
    sh_audio_bank_ops ops = {NULL, an_load, an_unload};
    carried_detail[0] = 0;
    if(g_limits.pending || g_environment.pending || g_state.pending || g_parameter.pending || g_bank_gate.scope)
        return an_error(error,capacity,"playback settings must be restored or accepted before another bank activation");
    if (g_addition != addition || (addition && addition->thread != GetCurrentThreadId()))
        return an_error(error, capacity, "audio activation is owned by another native transaction");
    if ((count && !events) || (package_count && !packages) || (media_count && !media))
        return an_error(error, capacity, "audio activation snapshot is missing");
    if (!count && !package_count && !media_count && !g_catalog_ready) return 1;
    if (!g_load || !g_unload || !g_directory[0])
        return an_error(error, capacity, "native audio bank activation is unavailable");
    if (!addition && g_catalog_ready && an_stale()) { an_clear_catalog(); g_catalog_ready = 0; }
    if (!g_catalog_ready && !an_build(error, capacity)) return 0;
    if (!an_extend(packages, package_count, error, capacity)) return 0;
    if (g_count > SIZE_MAX / sizeof(*content) || g_count > SIZE_MAX / sizeof(*effective))
        return an_error(error, capacity, "audio bank selection is too large");
    wanted = calloc(g_count ? g_count : 1, 1);
    content = calloc(g_count ? g_count : 1, sizeof(*content));
    effective = calloc(g_count ? g_count : 1, sizeof(*effective));
    metadata = calloc(g_count ? g_count : 1, sizeof(*metadata));
    if (!wanted || !content || !effective || !metadata) {
        an_error(error, capacity, "audio bank selection allocation failed"); goto done;
    }
    for (size_t i = 0; i < g_count; ++i) {
        int has_package = 0;
        for (size_t j = 0; j < package_count; ++j)
            if (packages[j].metadata->id == g_slots[i].id) { has_package = 1; break; }
        if (has_package && !an_effective(g_slots[i].name, packages, package_count,
                effective+i, error, capacity)) goto done;
        if (effective[i] && effective[i]->contributed) {
            wanted[i] = 1; content[i] = &effective[i]->identity; ++replaced;
        }
        metadata[i] = effective[i] ? effective[i]->metadata : g_banks[i].id ? g_banks + i : NULL;
    }
    sh_audio_bank_closure_report closure;
    /* Available declaration scanning includes intentionally unbanked defaults;
     * the direct map-use reader requires every requested event instead. */
    if (!sh_audio_bank_closure(metadata, g_count, events, count, 0, wanted,
            &closure, error, capacity)) goto done;
    matched = closure.matched_events; dependencies = closure.dependency_banks;
    optional = closure.optional_targets;
    /* A media payload this provider changed reaches the engine through the file
     * provider, unless a bank already carries those bytes in its own data. Then
     * the file can only change a streamed tail while the bank keeps supplying
     * the resident or prefetched head, so the pass refuses with both names
     * instead of serving half of one sound. Re-cooking the bank to embed new
     * media is the game's audio build step, not this compiler's. */
    for (size_t m = 0; m < media_count && !carried_detail[0]; ++m) {
        if (!media[m].contributed) continue;
        for (size_t i = 0; i < g_count; ++i) {
            const sh_audio_bank *bank = effective[i] ? effective[i]->metadata : g_banks+i;
            const sh_audio_bank_media *row;
            if (!wanted[i]) continue;   /* Nothing plays from a bank this pass leaves out. */
            row = sh_audio_bank_find_media(bank, media[m].id);
            if (!row) continue;
            if (!row->embedded) { ++streamed; continue; }
            if (effective[i] && effective[i]->contributed) continue;
            ++carried;
            snprintf(carried_detail, sizeof(carried_detail),
                "%s cannot replace media %u: audio bank '%s' carries those bytes, "
                "so that bank must be supplied with it",
                media[m].path, media[m].id, g_slots[i].name);
            break;
        }
    }
    if (carried_detail[0]) { an_error(error, capacity, carried_detail); goto done; }
    result = addition ? sh_audio_banks_append(g_slots, g_count, wanted, content, ops, error, capacity) :
        sh_audio_banks_reconcile(g_slots, g_count, wanted, content, ops, error, capacity);
    for (size_t i = 0; i < g_count; ++i) { needed += wanted[i] != 0; owned += g_slots[i].owned != 0; }
    if (result) {
        char line[320];
        snprintf(line, sizeof(line),
            "audio-banks: %zu/%zu prepared events, %zu wanted banks (%zu by dependency), "
            "%zu compiled overrides; %zu loads owned",
            matched, count, needed, dependencies, replaced, owned);
        backend_log(line);
        if (optional || g_packaged_only || streamed) {
            snprintf(line, sizeof(line),
                "audio-banks: %zu absent optional control targets, %zu packaged identities "
                "without a native name, %zu changed media served",
                optional, g_packaged_only, streamed);
            backend_log(line);
        }
        if (!addition) an_retire_slots(packages, package_count);
    }
done:
    free(wanted); free(content); free(effective); free(metadata); return result;
}

int sh_audio_banks_native_activate(const uint32_t *events, size_t count,
    const sh_package_audio_bank *packages, size_t package_count,
    const sh_package_audio_media *media, size_t media_count, char *error, size_t capacity)
{ return an_activate(NULL, events, count, packages, package_count, media, media_count, error, capacity); }

sh_audio_bank_addition *sh_audio_banks_native_additions_begin(char *error, size_t capacity)
{
    sh_audio_bank_addition *addition;
    if(g_limits.pending || g_environment.pending || g_state.pending || g_parameter.pending || g_bank_gate.scope) { an_error(error,capacity,"shared audio settings transition is still pending");return NULL; }
    if (g_addition) { an_error(error, capacity, "another additive audio transaction is active"); return NULL; }
    addition = calloc(1, sizeof(*addition));
    if (!addition || g_count > SIZE_MAX / sizeof(*addition->before)) {
        free(addition); an_error(error, capacity, "additive audio snapshot allocation failed"); return NULL;
    }
    if (g_count) {
        addition->before = malloc(g_count * sizeof(*addition->before));
        if (!addition->before) { free(addition); an_error(error, capacity, "additive audio snapshot allocation failed"); return NULL; }
        memcpy(addition->before, g_slots, g_count * sizeof(*addition->before));
        /* Names stay catalog-owned: additive activation never clears/retires
         * existing slots, even if extending relocates the slot array. */
    }
    addition->count = g_count; addition->thread = GetCurrentThreadId();
    g_addition = addition; return addition;
}

int sh_audio_banks_native_activate_additive(sh_audio_bank_addition *addition,
    const uint32_t *events, size_t count,
    const sh_package_audio_bank *packages, size_t package_count,
    const sh_package_audio_media *media, size_t media_count, char *error, size_t capacity)
{
    if (!addition || addition != g_addition) return an_error(error, capacity, "additive audio session is unavailable");
    addition->prepared = an_activate(addition, events, count, packages, package_count, media, media_count, error, capacity);
    return addition->prepared;
}

int sh_audio_banks_native_additions_ready(sh_audio_bank_addition *addition,
    char *error, size_t capacity)
{
    if (!addition) return 1;
    if (addition != g_addition || addition->thread != GetCurrentThreadId() || !addition->prepared)
        return an_error(error, capacity, "additive audio session has not completed");
    return 1;
}

void sh_audio_banks_native_additions_accept(sh_audio_bank_addition **handle)
{
    sh_audio_bank_addition *addition = handle ? *handle : NULL;
    if (!addition) return;
    if (addition->programs && addition->prepared) an_programs_addition_accept(addition);
    free(addition->loaded); free(addition->before); free(addition); *handle = NULL; g_addition = NULL;
}

int sh_audio_banks_native_additions_end(sh_audio_bank_addition **handle,
    int accepted, char *error, size_t capacity)
{
    sh_audio_bank_addition *addition = handle ? *handle : NULL;
    if (!addition) return 1;
    if(g_limits.pending || g_environment.pending || g_state.pending || g_parameter.pending || g_bank_gate.scope)
        return an_error(error,capacity,"playback settings must be restored or accepted before ending bank activation");
    if (addition != g_addition || addition->thread != GetCurrentThreadId() ||
        (accepted && !addition->prepared)) return an_error(error, capacity, "additive audio session has not completed");
    if (!accepted && addition->programs) {
        an_programs_addition_rollback(addition); addition->prepared = 0;
    } else if (!accepted && !sh_audio_banks_restore_additions(g_slots, g_count, addition->before,
        addition->count, (sh_audio_bank_ops){NULL, an_load, an_unload}, error, capacity)) return 0;
    sh_audio_banks_native_additions_accept(handle); return 1;
}

/* Retained audio programs. Each active program's scope banks stay loaded while
 * the program is published; superseded programs retire only after their banks
 * are idle (no playback item holds a slot reference), native unload succeeded
 * and every identity only they reserved has left the native registries. Their
 * owners (and so graphs, sources and ID leases) are released only then. */
typedef struct an_pbank {
    uint32_t id;
    char filename[260];
    int numeric, owned, uncertain;
    size_t active, retiring;
} an_pbank;
typedef struct an_program {
    void *owner;
    sh_package_audio_program *program;
    sh_audio_id_lease *lease;
    size_t *banks, bank_count;
    int retiring;
} an_program;
static an_pbank *g_pbanks;
static size_t g_pbank_count;
static an_program *g_programs;
static size_t g_program_count;
static void (*g_program_release)(void *owner);
static int g_shared_published;
static sh_audio_limits_change g_tx_limits;
static sh_audio_environment_change g_tx_environment;
static sh_audio_state_change g_tx_state;
static sh_audio_parameter_change g_tx_parameter;

static int an_fail(char *error, size_t capacity, const char *format, uint32_t value)
{ if (error && capacity) snprintf(error, capacity, format, (unsigned)value); return 0; }
static size_t an_pbank_find(uint32_t id)
{
    for (size_t i = 0; i < g_pbank_count; i++) if (g_pbanks[i].id == id) return i;
    return SIZE_MAX;
}
static size_t an_pbank_add(uint32_t id, const char *filename, int numeric)
{
    size_t index = an_pbank_find(id);
    if (index != SIZE_MAX) return index;
    if (!filename || strlen(filename) >= sizeof(g_pbanks[0].filename) ||
        g_pbank_count >= SIZE_MAX / sizeof(*g_pbanks) - 1) return SIZE_MAX;
    an_pbank *grown = realloc(g_pbanks, (g_pbank_count + 1) * sizeof(*grown));
    if (!grown) return SIZE_MAX;
    g_pbanks = grown; memset(g_pbanks + g_pbank_count, 0, sizeof(*g_pbanks));
    g_pbanks[g_pbank_count].id = id; g_pbanks[g_pbank_count].numeric = numeric;
    strcpy_s(g_pbanks[g_pbank_count].filename, sizeof(g_pbanks[0].filename), filename);
    return g_pbank_count++;
}
typedef struct an_plan { size_t *banks, count, capacity; int failed; } an_plan;
static int an_plan_visit(void *context, uint32_t id, const char *filename, int numeric)
{
    an_plan *plan = context;
    size_t index = an_pbank_add(id, filename, numeric);
    if (index == SIZE_MAX) { plan->failed = 1; return 0; }
    for (size_t i = 0; i < plan->count; i++) if (plan->banks[i] == index) return 1;
    if (plan->count == plan->capacity) {
        size_t next = plan->capacity ? plan->capacity * 2 : 16;
        if (next < plan->capacity || next > SIZE_MAX / sizeof(*plan->banks)) { plan->failed = 1; return 0; }
        size_t *grown = realloc(plan->banks, next * sizeof(*grown));
        if (!grown) { plan->failed = 1; return 0; }
        plan->banks = grown; plan->capacity = next;
    }
    plan->banks[plan->count++] = index; return 1;
}
/* Drop table rows nothing references or owns, remapping program indexes. */
static void an_pbank_compact(void)
{
    size_t *map = malloc((g_pbank_count ? g_pbank_count : 1) * sizeof(*map)), kept = 0;
    if (!map) return;
    for (size_t i = 0; i < g_pbank_count; i++) {
        const an_pbank *b = g_pbanks + i;
        if (!b->active && !b->retiring && !b->owned && !b->uncertain) { map[i] = SIZE_MAX; continue; }
        map[i] = kept; g_pbanks[kept++] = *b;
    }
    for (size_t p = 0; p < g_program_count; p++)
        for (size_t j = 0; j < g_programs[p].bank_count; j++) g_programs[p].banks[j] = map[g_programs[p].banks[j]];
    g_pbank_count = kept; free(map);
}
static void an_pbank_holders(void)
{
    for (size_t i = 0; i < g_pbank_count; i++) g_pbanks[i].active = g_pbanks[i].retiring = 0;
    for (size_t p = 0; p < g_program_count; p++)
        for (size_t j = 0; j < g_programs[p].bank_count; j++) {
            an_pbank *b = g_pbanks + g_programs[p].banks[j];
            if (g_programs[p].retiring) b->retiring++; else b->active++;
        }
}
static int an_named_load(const char *filename, uint32_t id)
{
    uint32_t loaded = 0; int result;
    if (!g_load) return 2;
    __try { result = g_load(filename, -1, &loaded); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    /* The name loader hashes the stem. A different identity is not this bank. */
    if ((result == 1 || result == 69) && loaded != id) return result == 1 ? -1 : 2;
    return result;
}
/* Load a planned bank unless a native slot already serves it. Existing banks
 * are shared, not owned: their native owner decides their lifetime. */
static int an_pbank_load(size_t index, uint32_t **loaded, size_t *loaded_count, char *error, size_t capacity)
{
    an_pbank *b = g_pbanks + index; sh_audio_bank_usage usage;
    if (b->uncertain) return an_fail(error, capacity, "native audio bank %u has an uncertain earlier load or unload", b->id);
    int present = sh_audio_registry_bank(&g_registry, b->id, &usage, error, capacity);
    if (present < 0) return 0;
    if (present && usage.loaded) return 1;
    b->owned = 0; /* An owned load that vanished was unloaded externally. */
    int result = b->numeric ? sh_audio_bank_commands_load(&g_commands, b->id) : an_named_load(b->filename, b->id);
    if (result == 69) return 1;
    if (result != 1) {
        if (result < 0) b->uncertain = 1;
        return an_fail(error, capacity, result < 0 ? "native audio bank %u load had uncertain effects" :
            "native audio bank %u could not be loaded", b->id);
    }
    b->owned = 1;
    if (loaded) {
        uint32_t *grown = realloc(*loaded, (*loaded_count + 1) * sizeof(*grown));
        if (!grown) return an_fail(error, capacity, "cannot record native audio bank %u ownership", b->id);
        *loaded = grown; grown[(*loaded_count)++] = b->id;
    }
    return 1;
}
/* 1 unloaded or already gone, 0 still in use, -1 native failure/uncertain. */
static int an_pbank_unload_idle(size_t index, char *error, size_t capacity)
{
    an_pbank *b = g_pbanks + index; sh_audio_bank_usage usage; int result;
    if (!b->owned) return 1;
    if (b->uncertain) return -1;
    int present = sh_audio_registry_bank(&g_registry, b->id, &usage, error, capacity);
    if (present < 0) return -1;
    if (!present) { b->owned = 0; return 1; }
    /* Our load holds exactly one slot reference. Any other means a playing
     * item still reads this bank; native unload would stop it. */
    if (!usage.loaded || usage.references != 1 || usage.prepared) return 0;
    __try { result = g_unload ? g_unload(b->id, NULL, NULL) : 2; }
    __except (EXCEPTION_EXECUTE_HANDLER) { result = -1; }
    if (result == 1) { b->owned = 0; return 1; }
    if (result < 0) b->uncertain = 1;
    an_fail(error, capacity, "native audio bank %u could not be unloaded", b->id);
    return -1;
}
typedef struct an_vacancy { int state; } an_vacancy;
static int an_vacant(void *context, sh_audio_identity_domain domain, uint32_t id)
{
    an_vacancy *vacancy = context; (void)domain;
    int occupied = sh_audio_registry_occupied(&g_registry, id, NULL, 0);
    if (occupied) { vacancy->state = occupied < 0 ? -1 : 1; return 0; }
    return 1;
}
static void an_program_free(an_program *program)
{
    free(program->banks);
    if (g_program_release && program->owner) g_program_release(program->owner);
    memset(program, 0, sizeof(*program));
}
static int an_programs_busy(void)
{
    return g_limits.pending || g_environment.pending || g_state.pending || g_parameter.pending ||
        g_bank_gate.scope || g_commands.uncertain;
}

/* Main-thread boundary with no provider, publication or render lock held.
 * Unloads owned banks no active program needs once they are idle, then
 * releases superseded programs whose private identities left the engine.
 * Returns the number of programs still retiring, or -1 on a native failure. */
int sh_audio_banks_native_programs_retire(char *error, size_t capacity)
{
    int failed = 0; size_t remaining = 0;
    if (error && capacity) *error = 0;
    if (an_programs_busy() || g_addition) return (int)g_program_count;
    an_pbank_holders();
    for (size_t i = 0; i < g_pbank_count; i++)
        if (!g_pbanks[i].active && g_pbanks[i].owned && an_pbank_unload_idle(i, error, capacity) < 0) failed = 1;
    for (size_t p = 0; p < g_program_count;) {
        an_program *program = g_programs + p; int ready = program->retiring;
        for (size_t j = 0; ready && j < program->bank_count; j++) {
            const an_pbank *b = g_pbanks + program->banks[j];
            ready = !b->owned || b->active;
        }
        if (ready) {
            an_vacancy vacancy = {0};
            if (program->lease && !sh_audio_id_lease_exclusive(program->lease, an_vacant, &vacancy) && !vacancy.state)
                vacancy.state = -1;
            ready = !vacancy.state; failed |= vacancy.state < 0;
        }
        if (!ready) { remaining += program->retiring != 0; p++; continue; }
        an_program_free(program);
        memmove(g_programs + p, g_programs + p + 1, (g_program_count - p - 1) * sizeof(*g_programs));
        g_program_count--;
    }
    an_pbank_holders(); an_pbank_compact();
    if (failed && error && capacity && !*error) snprintf(error, capacity, "native audio retirement is waiting on an uncertain native state");
    return failed ? -1 : (int)remaining;
}

static int an_policy_views(const sh_audio_active_program *programs, size_t count,
    const sh_audio_shared_policy ***views, char *error, size_t capacity)
{
    *views = calloc(count ? count : 1, sizeof(**views));
    if (!*views) return an_error(error, capacity, "cannot compose active audio controls");
    for (size_t i = 0; i < count; i++) {
        (*views)[i] = sh_package_audio_program_bound_shared(programs[i].program);
        if (!(*views)[i]) return an_error(error, capacity, "active audio program has no bound shared controls");
    }
    return 1;
}
/* One serialized publication of every shared-control leg. Bank commands have
 * completed before this and none are submitted while it holds the gate. */
static int an_shared_publish(const sh_audio_shared_policy *effective, char *error, size_t capacity)
{
    sh_audio_bank_scope scope = {0}; int stage = 0, ok = 0;
    if (!sh_audio_bank_gate_begin(&g_bank_gate, &scope, error, capacity)) return 0;
    memset(&g_tx_limits, 0, sizeof(g_tx_limits)); memset(&g_tx_environment, 0, sizeof(g_tx_environment));
    memset(&g_tx_state, 0, sizeof(g_tx_state)); memset(&g_tx_parameter, 0, sizeof(g_tx_parameter));
    if (sh_audio_limits_native_apply(&g_limits, effective, &g_tx_limits, error, capacity) != 1) goto recover;
    stage = 1;
    if (sh_audio_environment_native_apply(&g_environment, effective, &g_tx_environment, error, capacity) != 1) goto recover;
    stage = 2;
    if (sh_audio_state_native_apply(&g_state, effective, &g_tx_state, error, capacity) != 1) goto recover;
    stage = 3;
    if (sh_audio_parameter_native_apply(&g_parameter, effective, &g_tx_parameter, error, capacity) != 1) goto recover;
    stage = 4;
    if (!sh_audio_banks_native_shared_ready(&scope, error, capacity)) goto recover;
    sh_audio_limits_native_accept(&g_tx_limits); sh_audio_environment_native_accept(&g_tx_environment);
    sh_audio_state_native_accept(&g_tx_state); sh_audio_parameter_native_accept(&g_tx_parameter);
    ok = 1;
recover:
    if (!ok) {
        /* Reverse order; a leg whose restore fails keeps its receipt and blocks
         * later transitions until recovery succeeds. */
        char ignored[256];
        if (stage >= 4) sh_audio_parameter_native_restore(&g_tx_parameter, ignored, sizeof(ignored));
        if (stage >= 3) sh_audio_state_native_restore(&g_tx_state, ignored, sizeof(ignored));
        if (stage >= 2) sh_audio_environment_native_restore(&g_tx_environment, ignored, sizeof(ignored));
        if (stage >= 1) sh_audio_limits_native_restore(&g_tx_limits, ignored, sizeof(ignored));
    }
    sh_audio_bank_gate_end(&scope, NULL, 0);
    if (ok) {
        char detail[256];
        if (g_environment.pending && !sh_audio_environment_native_release(&g_tx_environment, detail, sizeof(detail))) backend_log(detail);
        if (g_state.pending && !sh_audio_state_native_release(&g_tx_state, detail, sizeof(detail))) backend_log(detail);
        if (g_parameter.pending && !sh_audio_parameter_native_release(&g_tx_parameter, detail, sizeof(detail))) backend_log(detail);
    }
    return ok;
}
static int an_program_known(const void *owner)
{
    for (size_t i = 0; i < g_program_count; i++) if (g_programs[i].owner == owner) return 1;
    return 0;
}
static void an_release_owners(const sh_audio_active_program *programs, size_t count)
{
    for (size_t i = 0; i < count; i++) if (g_program_release && programs[i].owner) g_program_release(programs[i].owner);
}
static void an_unload_new(const uint32_t *loaded, size_t count)
{
    char ignored[256];
    an_pbank_holders();
    for (size_t i = count; i > 0; i--) {
        size_t index = an_pbank_find(loaded[i - 1]);
        if (index != SIZE_MAX && !g_pbanks[index].active) (void)an_pbank_unload_idle(index, ignored, sizeof(ignored));
    }
}
/* Plan, ready and load the programs this call newly admits. On success the
 * programs are appended (retiring=0) and own their planned bank indexes. */
static int an_programs_admit(const sh_audio_active_program *programs, size_t count, size_t **plans,
    size_t *plan_counts, uint32_t **loaded, size_t *loaded_count, char *error, size_t capacity)
{
    for (size_t i = 0; i < count; i++) {
        an_plan plan = {0};
        if (!programs[i].program || !sh_package_audio_program_loads(programs[i].program, an_plan_visit, &plan, error, capacity)) {
            free(plan.banks);
            if (!error || !capacity || !*error) an_error(error, capacity, "cannot plan active audio program banks");
            return 0;
        }
        plans[i] = plan.banks; plan_counts[i] = plan.count;
        if (programs[i].lease && !sh_audio_id_lease_active(programs[i].lease) &&
            !sh_audio_id_lease_ready(programs[i].lease, error, capacity)) return 0;
    }
    for (size_t i = 0; i < count; i++)
        for (size_t j = 0; j < plan_counts[i]; j++)
            if (!an_pbank_load(plans[i][j], loaded, loaded_count, error, capacity)) return 0;
    return 1;
}

/* Every effective edit needs a native leg that can publish and restore it. An
 * edit without one is refused explicitly rather than silently not applied. */
static int an_publishable(const sh_audio_active_program *programs, size_t count,
    const sh_audio_shared_policy *effective, char *error, size_t capacity)
{
    for (size_t i = 0; i < effective->count; i++)
        if (effective->settings[i].edited && effective->settings[i].record.kind == SH_AUDIO_SHARED_SWITCH)
            return an_error(error, capacity, "native audio switch publication is not available for this edit");
    for (size_t i = 0; i < count; i++)
        if (sh_package_audio_program_bus_edited(programs[i].program))
            return an_error(error, capacity, "native audio bus publication is not available for this edit");
    return 1;
}
int sh_audio_banks_native_programs_activate(const sh_audio_active_program *programs, size_t count,
    const sh_audio_shared_policy *baseline, void (*release)(void *), char *error, size_t capacity)
{
    size_t **plans = NULL, *plan_counts = NULL; uint32_t *loaded = NULL; size_t loaded_count = 0;
    const sh_audio_shared_policy **views = NULL; sh_audio_shared_policy effective = {0};
    an_program *next = NULL; int ok = 0;
    if (error && capacity) *error = 0;
    if (release) g_program_release = release;
    if ((count && !programs) || count > SIZE_MAX / sizeof(*next) - g_program_count) {
        an_release_owners(programs, count); return an_error(error, capacity, "invalid active audio program set");
    }
    if (an_programs_busy() || g_addition) {
        an_release_owners(programs, count);
        return an_error(error, capacity, "native audio recovery or another transaction must finish first");
    }
    plans = calloc(count ? count : 1, sizeof(*plans)); plan_counts = calloc(count ? count : 1, sizeof(*plan_counts));
    next = calloc(count + g_program_count + 1, sizeof(*next));
    if (!plans || !plan_counts || !next) { an_error(error, capacity, "cannot plan active audio programs"); goto done; }
    for (size_t i = 0; i < count; i++) for (size_t j = 0; j < i; j++) if (programs[i].owner == programs[j].owner) {
        an_error(error, capacity, "active audio program is listed twice"); goto done;
    }
    if (!an_policy_views(programs, count, &views, error, capacity) ||
        !sh_audio_effective_shared(baseline, views, count, &effective, error, capacity) ||
        !an_publishable(programs, count, &effective, error, capacity) ||
        !an_programs_admit(programs, count, plans, plan_counts, &loaded, &loaded_count, error, capacity)) goto done;
    /* An untouched native configuration stays untouched for vanilla content;
     * once an edit was published, later transitions publish the complete
     * effective policy so removed edits return to the native baseline. */
    int edited = sh_audio_effective_shared_edited(&effective);
    if ((edited || g_shared_published) && !an_shared_publish(&effective, error, capacity)) goto done;
    g_shared_published = edited;
    /* Commit: nothing below can fail. */
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        size_t known = SIZE_MAX;
        for (size_t p = 0; p < g_program_count; p++) if (g_programs[p].owner == programs[i].owner) known = p;
        if (known != SIZE_MAX) {
            next[used] = g_programs[known]; g_programs[known].owner = NULL; free(next[used].banks);
            if (g_program_release) g_program_release(programs[i].owner);
        } else next[used] = (an_program){programs[i].owner, programs[i].program, programs[i].lease, NULL, 0, 0};
        next[used].banks = plans[i]; next[used].bank_count = plan_counts[i]; plans[i] = NULL; next[used].retiring = 0;
        if (next[used].lease && !sh_audio_id_lease_active(next[used].lease)) sh_audio_id_lease_activate(next[used].lease);
        used++;
    }
    for (size_t p = 0; p < g_program_count; p++) if (g_programs[p].owner) {
        next[used] = g_programs[p]; next[used].retiring = 1; used++;
    }
    free(g_programs); g_programs = next; g_program_count = used; next = NULL; ok = 1;
done:
    if (!ok) {
        an_unload_new(loaded, loaded_count);
        an_release_owners(programs, count);
        an_pbank_holders(); an_pbank_compact();
    }
    if (plans) for (size_t i = 0; i < count; i++) free(plans[i]);
    free(plans); free(plan_counts); free(loaded); free(next); free((void *)views);
    sh_audio_shared_policy_free(&effective);
    if (ok) {
        char detail[256];
        an_pbank_holders();
        if (sh_audio_banks_native_programs_retire(detail, sizeof(detail)) < 0) backend_log(detail);
    }
    return ok;
}

/* Active-editor admission never retires or republishes shared controls: the
 * clone view keeps current metadata. New programs load their banks now and
 * activate their leases only at accept; rollback unloads what it took. */
int sh_audio_banks_native_programs_add(sh_audio_bank_addition *addition,
    const sh_audio_active_program *programs, size_t count, void (*release)(void *),
    char *error, size_t capacity)
{
    size_t **plans = NULL, *plan_counts = NULL, fresh = 0, kept = 0;
    sh_audio_active_program *admitted = NULL; int ok = 0;
    if (error && capacity) *error = 0;
    if (release) g_program_release = release;
    if (!addition || addition != g_addition || addition->thread != GetCurrentThreadId() || addition->prepared ||
        addition->programs || an_programs_busy() || (count && !programs)) {
        an_release_owners(programs, count); return an_error(error, capacity, "additive audio session is unavailable");
    }
    admitted = calloc(count ? count : 1, sizeof(*admitted));
    if (!admitted) { an_release_owners(programs, count); return an_error(error, capacity, "cannot admit audio programs"); }
    for (size_t i = 0; i < count; i++) {
        if (an_program_known(programs[i].owner)) { if (g_program_release) g_program_release(programs[i].owner); continue; }
        admitted[fresh++] = programs[i];
    }
    addition->programs = 1; addition->first_program = g_program_count;
    plans = calloc(fresh ? fresh : 1, sizeof(*plans)); plan_counts = calloc(fresh ? fresh : 1, sizeof(*plan_counts));
    if (!plans || !plan_counts || fresh > SIZE_MAX / sizeof(*g_programs) - g_program_count - 1) {
        an_error(error, capacity, "cannot plan admitted audio programs"); goto done;
    }
    if (!an_programs_admit(admitted, fresh, plans, plan_counts, &addition->loaded, &addition->loaded_count, error, capacity)) goto done;
    an_program *grown = realloc(g_programs, (g_program_count + fresh + 1) * sizeof(*grown));
    if (!grown) { an_error(error, capacity, "cannot retain admitted audio programs"); goto done; }
    g_programs = grown;
    for (size_t i = 0; i < fresh; i++) {
        g_programs[g_program_count++] = (an_program){admitted[i].owner, admitted[i].program, admitted[i].lease,
            plans[i], plan_counts[i], 0};
        plans[i] = NULL;
    }
    kept = fresh; an_pbank_holders(); ok = 1;
done:
    if (!ok) {
        an_unload_new(addition->loaded, addition->loaded_count);
        free(addition->loaded); addition->loaded = NULL; addition->loaded_count = 0;
        an_pbank_holders(); an_pbank_compact();
    }
    for (size_t i = kept; i < fresh; i++) if (g_program_release && admitted[i].owner) g_program_release(admitted[i].owner);
    if (plans) for (size_t i = 0; i < fresh; i++) free(plans[i]);
    free(plans); free(plan_counts); free(admitted);
    addition->prepared = ok;
    return ok;
}
/* Rollback of a program-mode addition: its programs leave at once and the
 * banks it loaded unload when idle (or retire later). */
static void an_programs_addition_rollback(sh_audio_bank_addition *addition)
{
    while (g_program_count > addition->first_program) an_program_free(g_programs + --g_program_count);
    an_unload_new(addition->loaded, addition->loaded_count);
    an_pbank_holders(); an_pbank_compact();
}
static void an_programs_addition_accept(sh_audio_bank_addition *addition)
{
    for (size_t p = addition->first_program; p < g_program_count; p++)
        if (g_programs[p].lease && !sh_audio_id_lease_active(g_programs[p].lease)) sh_audio_id_lease_activate(g_programs[p].lease);
}
size_t sh_audio_banks_native_programs_count(void) { return g_program_count; }
