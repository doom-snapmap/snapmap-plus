#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include "audio_banks_native.h"
#include "audio_banks.h"
#include "audio_files_native.h"
#include "package_runtime.h"
#include "backend_log.h"

typedef int (*ab_load_fn)(const char *name, int pool, uint32_t *id);
typedef int (*ab_unload_fn)(uint32_t id, const void *memory, int *pool);
static ab_load_fn g_load;
static ab_unload_fn g_unload;
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

static int an_error(char *error, size_t capacity, const char *message)
{ if (error && capacity) snprintf(error, capacity, "%s", message); return 0; }

void sh_audio_banks_native_install(const sig_result *results, size_t count)
{
    char *slash;
    DWORD length;
    g_load = (ab_load_fn)sig_addr_by_name(results, count, "AudioBankLoad");
    g_unload = (ab_unload_fn)sig_addr_by_name(results, count, "AudioBankUnload");
    length = GetModuleFileNameA(NULL, g_directory, sizeof(g_directory));
    if (!length || length >= sizeof(g_directory) || !(slash = strrchr(g_directory, '\\'))) {
        g_directory[0] = 0; return;
    }
    *slash = 0;
    if (strcat_s(g_directory, sizeof(g_directory), "\\base\\sound\\soundbanks\\pc"))
        g_directory[0] = 0;
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
    (void)context;
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

int sh_audio_banks_native_activate(const uint32_t *events, size_t count,
    const sh_package_audio_bank *packages, size_t package_count,
    const sh_package_audio_media *media, size_t media_count,
    char *error, size_t capacity)
{
    unsigned char *wanted = NULL;
    const sh_package_file_identity **content = NULL;
    const sh_package_audio_bank **effective = NULL;
    size_t matched = 0, needed = 0, owned = 0, replaced = 0;
    size_t dependencies = 0, optional = 0, carried = 0, streamed = 0;
    char carried_detail[256], missing_detail[256];
    int result = 0;
    sh_audio_bank_ops ops = {NULL, an_load, an_unload};
    carried_detail[0] = missing_detail[0] = 0;
    if ((count && !events) || (package_count && !packages) || (media_count && !media))
        return an_error(error, capacity, "audio activation snapshot is missing");
    if (!count && !package_count && !media_count && !g_catalog_ready) return 1;
    if (!g_load || !g_unload || !g_directory[0])
        return an_error(error, capacity, "native audio bank activation is unavailable");
    if (g_catalog_ready && an_stale()) { an_clear_catalog(); g_catalog_ready = 0; }
    if (!g_catalog_ready && !an_build(error, capacity)) return 0;
    if (!an_extend(packages, package_count, error, capacity)) return 0;
    if (g_count > SIZE_MAX / sizeof(*content) || g_count > SIZE_MAX / sizeof(*effective))
        return an_error(error, capacity, "audio bank selection is too large");
    wanted = calloc(g_count ? g_count : 1, 1);
    content = calloc(g_count ? g_count : 1, sizeof(*content));
    effective = calloc(g_count ? g_count : 1, sizeof(*effective));
    if (!wanted || !content || !effective) {
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
    }
    for (size_t j = 0; j < count; ++j) {
        int found = 0;
        for (size_t i = 0; i < g_count; ++i) {
            const sh_audio_bank *bank = effective[i] ? effective[i]->metadata : g_banks+i;
            if (sh_audio_bank_has_event(bank, events[j])) { wanted[i] = 1; found = 1; }
        }
        matched += found;
    }
    /* Close over the banks a wanted bank names explicitly. A play action records
     * the bank holding its target, so this is read evidence, not a guess. */
    for (int changed = 1; changed; ) {
        changed = 0;
        for (size_t i = 0; i < g_count; ++i) {
            const sh_audio_bank *bank = effective[i] ? effective[i]->metadata : g_banks+i;
            if (!wanted[i] || !bank->bank_count) continue;
            for (size_t j = 0; j < g_count; ++j) {
                if (wanted[j] || !sh_audio_bank_needs_bank(bank, g_slots[j].id)) continue;
                wanted[j] = 1; ++dependencies; changed = 1;
            }
        }
    }
    /* A play target or an event's own action that no bank defines is a real
     * missing dependency and refuses the pass. A control action's target may
     * legally be absent, so those are counted, never loaded for. */
    for (size_t i = 0; i < g_count; ++i) if (wanted[i]) {
        const sh_audio_bank *bank = effective[i] ? effective[i]->metadata : g_banks+i;
        optional += bank->optional_count;
        if (bank->required_count && !missing_detail[0])
            snprintf(missing_detail, sizeof(missing_detail),
                "audio bank '%s' plays object %u, which no bank defines",
                g_slots[i].name, bank->required[0]);
    }
    if (missing_detail[0]) { an_error(error, capacity, missing_detail); goto done; }
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
    result = sh_audio_banks_reconcile(g_slots, g_count, wanted, content, ops, error, capacity);
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
        an_retire_slots(packages, package_count);
    }
done:
    free(wanted); free(content); free(effective); return result;
}
