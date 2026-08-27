/* resource_bridge.c -- sparse, manifest-selected installed-resource provider.
 *
 * This is deliberately not a virtual pindex or archive overlay. A launch-time
 * manifest names exact rows already present in the user's installed base-game
 * pindex. We retain only their metadata, then read and decode the selected
 * slices on demand through the normal SnapMap+ override idFile stream.
 * Installed files are opened read-only and never rewritten or copied. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_log.h"
#include "decl_server_path.h"
#include "raw_deflate.h"
#include "packages.h"
#include "resource_bridge.h"

#define RB_ROOT_SUFFIX            "\\overrides\\generated\\resources"
#define RB_MAX_MANIFESTS          64u
#define RB_MAX_MANIFEST_BYTES     (1024u * 1024u)
#define RB_MAX_TOTAL_MANIFEST     (4u * 1024u * 1024u)
#define RB_MAX_ENTRIES            4096u
#define RB_MAX_PINDEX_BYTES       (64u * 1024u * 1024u)
#define RB_MAX_PINDEX_ROWS        1000000u
#define RB_MAX_PINDEX_FIELD       4096u
#define RB_MAX_RESOURCE_BYTES     (64u * 1024u * 1024u)
#define RB_MAX_TOTAL_DECODED      (256u * 1024u * 1024u)
#define RB_MAX_EQUIVALENT_ROWS    16u
#define RB_COMPARE_CHUNK          (64u * 1024u)
#define RB_ALIAS_CAP              SH_DECL_SERVER_SOURCE_CAP

enum {
    RB_STATE_NEW = 0,
    RB_STATE_INSTALLING,
    RB_STATE_READY,
    RB_STATE_FAILED
};

typedef struct rb_entry {
    char type[SH_DECL_SERVER_TYPE_CAP];
    char name[SH_DECL_SERVER_NAME_CAP];
    char alias[RB_ALIAS_CAP];
    char source[SH_DECL_SERVER_SOURCE_CAP];
    unsigned long long offset;
    unsigned int size;
    unsigned int zsize;
    unsigned int matches;
    unsigned char selector;
    unsigned long long equivalent_offsets[RB_MAX_EQUIVALENT_ROWS];
} rb_entry;

typedef struct rb_manifest_file {
    char path[MAX_PATH];
    char name[MAX_PATH];
    /* Two packages may ship a like-named manifest, so a row's provenance is the
     * package plus the file, never the file alone. */
    char package[SH_PACKAGE_NAME_CAP];
} rb_manifest_file;

static volatile LONG g_state = RB_STATE_NEW;
static volatile LONG g_provider_ready;
static rb_entry *g_entries;
static size_t g_entry_count;
static size_t g_entry_capacity;
static size_t g_decl_count;
static size_t g_equivalent_row_count;
static int g_has_manifests;
static HANDLE g_archives[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
static unsigned long long g_archive_sizes[2];
static SRWLOCK g_archive_lock = SRWLOCK_INIT;
static rb_entry *g_sort_entries;
#ifdef SH_RESOURCE_BRIDGE_TESTING
static char g_test_doom_base[MAX_PATH];
#endif

static unsigned int rb_be32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static unsigned long long rb_be64(const unsigned char *p)
{
    return ((unsigned long long)rb_be32(p) << 32) | rb_be32(p + 4);
}

static unsigned int rb_le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int rb_ascii_ci_char(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    if (c == '\\') return '/';
    return c;
}

static int rb_path_cmp_ci(const char *a, const char *b)
{
    for (;;) {
        int ca = rb_ascii_ci_char((unsigned char)*a++);
        int cb = rb_ascii_ci_char((unsigned char)*b++);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
}

/* Order every key CASE-INSENSITIVELY FIRST, then break ties exactly.
 *
 * The leading case-insensitive pass is what makes rb_collapse_identical_rows
 * correct: it collapses adjacent rows using rb_path_cmp_ci on type, name AND
 * alias, so rows this comparator considers equal under that same rule must come
 * out adjacent. Sorting the alias tie-break with strcmp alone broke that -- two
 * rows differing only in the case of their alias could be separated by a third
 * row, escape the collapse, and then be refused by the case-insensitive alias
 * check below as if two packages disagreed. */
static int rb_identity_cmp(const rb_entry *a, const rb_entry *b)
{
    int c = rb_path_cmp_ci(a->type, b->type);
    if (!c) c = rb_path_cmp_ci(a->name, b->name);
    if (!c) c = rb_path_cmp_ci(a->alias, b->alias);
    if (!c) c = strcmp(a->type, b->type);
    if (!c) c = strcmp(a->name, b->name);
    if (!c) c = strcmp(a->alias, b->alias);
    if (!c) c = strcmp(a->source, b->source);
    return c;
}

static int __cdecl rb_entry_identity_qsort(const void *left, const void *right)
{
    return rb_identity_cmp((const rb_entry *)left, (const rb_entry *)right);
}

static int __cdecl rb_manifest_qsort(const void *left, const void *right)
{
    const rb_manifest_file *a = (const rb_manifest_file *)left;
    const rb_manifest_file *b = (const rb_manifest_file *)right;
    int c = strcmp(a->name, b->name);
    return c ? c : strcmp(a->path, b->path);
}

static int __cdecl rb_exact_index_qsort(const void *left, const void *right)
{
    size_t a = *(const size_t *)left;
    size_t b = *(const size_t *)right;
    int c = strcmp(g_sort_entries[a].type, g_sort_entries[b].type);
    if (!c) c = strcmp(g_sort_entries[a].name, g_sort_entries[b].name);
    if (!c) c = strcmp(g_sort_entries[a].alias, g_sort_entries[b].alias);
    return c ? c : (a < b ? -1 : a != b);
}

static int __cdecl rb_alias_index_qsort(const void *left, const void *right)
{
    size_t a = *(const size_t *)left;
    size_t b = *(const size_t *)right;
    int c = rb_path_cmp_ci(g_sort_entries[a].alias, g_sort_entries[b].alias);
    if (!c) c = strcmp(g_sort_entries[a].alias, g_sort_entries[b].alias);
    return c ? c : (a < b ? -1 : a != b);
}

static void rb_close_archives(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_archives[i] != INVALID_HANDLE_VALUE) CloseHandle(g_archives[i]);
        g_archives[i] = INVALID_HANDLE_VALUE;
        g_archive_sizes[i] = 0;
    }
}

static void rb_release_entries(void)
{
    if (g_entries) HeapFree(GetProcessHeap(), 0, g_entries);
    g_entries = NULL;
    g_entry_count = 0;
    g_entry_capacity = 0;
    g_decl_count = 0;
    g_equivalent_row_count = 0;
}

static int rb_fail(const char *reason)
{
    char line[512];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "resource-bridge REFUSED: %s; zero entries admitted",
                reason ? reason : "unknown failure");
    backend_log(line);
    rb_close_archives();
    rb_release_entries();
    InterlockedExchange(&g_provider_ready, 0);
    InterlockedExchange(&g_state, RB_STATE_FAILED);
    return 0;
}

static int rb_get_doom_base(char *out, size_t cap)
{
    char exe[MAX_PATH];
    char *slash;
#ifdef SH_RESOURCE_BRIDGE_TESTING
    if (g_test_doom_base[0]) {
        strncpy_s(out, cap, g_test_doom_base, _TRUNCATE);
        return out[0] != '\0';
    }
#endif
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return 0;
    slash = strrchr(exe, '\\');
    if (!slash) slash = strrchr(exe, '/');
    if (!slash) return 0;
    *slash = '\0';
    return _snprintf_s(out, cap, _TRUNCATE, "%s\\base", exe) >= 0;
}

static int rb_has_suffix_ci(const char *value, const char *suffix)
{
    size_t vl, sl;
    if (!value || !suffix) return 0;
    vl = strlen(value);
    sl = strlen(suffix);
    return vl >= sl && _stricmp(value + vl - sl, suffix) == 0;
}

static int rb_field_valid(const char *value, int allow_empty, int type_field)
{
    const char *segment;
    const char *p;
    size_t length;
    if (!value) return 0;
    length = strlen(value);
    if (!length) return allow_empty;
    if (value[0] == '/' || value[length - 1] == '/') return 0;
    segment = value;
    for (p = value; ; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && (c < 0x21u || c > 0x7eu || c == '\\' || c == ':')) return 0;
        if (type_field && c == '/') return 0;
        if (c == '/' || c == '\0') {
            size_t n = (size_t)(p - segment);
            if (!n || (n == 1u && segment[0] == '.') ||
                (n == 2u && segment[0] == '.' && segment[1] == '.')) return 0;
            if (!c) break;
            segment = p + 1;
        }
    }
    return 1;
}

static unsigned char *rb_read_regular_file(const char *path, size_t cap,
                                           size_t *out_length)
{
    HANDLE file;
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER size;
    DWORD got = 0;
    unsigned char *body;
    *out_length = 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                       FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (unsigned long long)size.QuadPart > cap ||
        (unsigned long long)size.QuadPart > 0xffffffffull) {
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
    body[(size_t)size.QuadPart] = 0;
    *out_length = (size_t)size.QuadPart;
    return body;
}

static int rb_grow_entries(void)
{
    size_t bytes, next_capacity;
    rb_entry *grown;
    if (g_entry_count >= RB_MAX_ENTRIES) return 0;
    if (g_entry_count < g_entry_capacity) return 1;
    next_capacity = g_entry_capacity ? g_entry_capacity * 2u : 64u;
    if (next_capacity > RB_MAX_ENTRIES) next_capacity = RB_MAX_ENTRIES;
    bytes = next_capacity * sizeof(g_entries[0]);
    if (g_entries)
        grown = (rb_entry *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        g_entries, bytes);
    else
        grown = (rb_entry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    if (!grown) return 0;
    g_entries = grown;
    g_entry_capacity = next_capacity;
    return 1;
}

static int rb_copy_field(char *destination, size_t cap,
                         const unsigned char *start, size_t length)
{
    if (length >= cap) return 0;
    memcpy(destination, start, length);
    destination[length] = '\0';
    return 1;
}

static int rb_parse_manifest(const rb_manifest_file *manifest,
                             size_t *total_manifest_bytes)
{
    unsigned char *body;
    size_t length, pos = 0, line_number = 0;
    body = rb_read_regular_file(manifest->path, RB_MAX_MANIFEST_BYTES, &length);
    if (!body) return 0;
    if (length > RB_MAX_TOTAL_MANIFEST - *total_manifest_bytes) {
        HeapFree(GetProcessHeap(), 0, body);
        return 0;
    }
    *total_manifest_bytes += length;
    while (pos < length) {
        size_t start = pos, end, tab1 = (size_t)-1, tab2 = (size_t)-1, i;
        rb_entry *entry;
        line_number++;
        while (pos < length && body[pos] != '\n') pos++;
        end = pos;
        if (pos < length) pos++;
        if (end > start && body[end - 1] == '\r') end--;
        if (end == start || body[start] == '#') continue;
        for (i = start; i < end; i++) {
            if (body[i] != '\t' && (body[i] < 0x20u || body[i] > 0x7eu)) {
                HeapFree(GetProcessHeap(), 0, body);
                return 0;
            }
            if (body[i] == '\t') {
                if (tab1 == (size_t)-1) tab1 = i;
                else if (tab2 == (size_t)-1) tab2 = i;
                else {
                    HeapFree(GetProcessHeap(), 0, body);
                    return 0;
                }
            }
        }
        if (tab1 == (size_t)-1 || tab2 == (size_t)-1 || tab1 == start || tab2 == tab1 + 1u)
        {
            HeapFree(GetProcessHeap(), 0, body);
            return 0;
        }
        if (!rb_grow_entries()) {
            HeapFree(GetProcessHeap(), 0, body);
            return 0;
        }
        entry = &g_entries[g_entry_count];
        if (!rb_copy_field(entry->type, sizeof(entry->type), body + start, tab1 - start) ||
            !rb_copy_field(entry->name, sizeof(entry->name), body + tab1 + 1u,
                           tab2 - tab1 - 1u) ||
            !rb_copy_field(entry->alias, sizeof(entry->alias), body + tab2 + 1u,
                           end - tab2 - 1u) ||
            !rb_field_valid(entry->type, 0, 1) ||
            !rb_field_valid(entry->name, 0, 0) ||
            !rb_field_valid(entry->alias, 1, 0)) {
            HeapFree(GetProcessHeap(), 0, body);
            return 0;
        }
        if (!entry->alias[0]) strcpy_s(entry->alias, sizeof(entry->alias), entry->name);
        if (_snprintf_s(entry->source, sizeof(entry->source), _TRUNCATE,
                        "%s/resources/%s:%zu", manifest->package, manifest->name,
                        line_number) < 0) {
            HeapFree(GetProcessHeap(), 0, body);
            return 0;
        }
        g_entry_count++;
    }
    HeapFree(GetProcessHeap(), 0, body);
    return 1;
}

/* APPEND this package's manifests to `files`. Every installed package
 * contributes to one set, so this must never restart at index zero: an earlier
 * version overwrote the accumulated list per package, which silently dropped
 * every manifest but the last package's, and a package with an empty resources
 * directory reset the count to zero outright. */
static int rb_collect_manifests(const char *directory, const char *package,
                                rb_manifest_file *files, size_t capacity,
                                size_t *inout_count)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    DWORD error;
    size_t count = *inout_count;
    size_t first = count;
    if (_snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.manifest", directory) < 0)
        return 0;
    search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND;   /* this package ships none */
    }
    for (;;) {
        if (count >= capacity ||
            (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            _snprintf_s(files[count].path, sizeof(files[count].path), _TRUNCATE,
                        "%s\\%s", directory, found.cFileName) < 0 ||
            strcpy_s(files[count].package, sizeof(files[count].package), package) != 0) {
            FindClose(search);
            return 0;
        }
        strcpy_s(files[count].name, sizeof(files[count].name), found.cFileName);
        count++;
        if (FindNextFileA(search, &found)) continue;
        error = GetLastError();
        FindClose(search);
        if (error != ERROR_NO_MORE_FILES) return 0;
        break;
    }
    /* Sort within the package. Packages already arrive in a deterministic
     * order, so the combined set stays reproducible. */
    qsort(files + first, count - first, sizeof(files[0]), rb_manifest_qsort);
    *inout_count = count;
    return 1;
}

/* Two packages that bridge the same resource is the NORMAL case, not an error:
 * shared gore, FX and animation assets belong to no single demon. An identical
 * row therefore composes -- the first copy serves it and the rest collapse away,
 * exactly as an identical cvar requirement does. Only a genuine disagreement is
 * refused: one provider path claimed by two different identities cannot be
 * served two ways at once, and guessing a winner would silently hand a player
 * the wrong asset. */
static int rb_collapse_identical_rows(void)
{
    size_t read, write = 1;
    if (g_entry_count < 2) return 0;
    for (read = 1; read < g_entry_count; read++) {
        const rb_entry *previous = &g_entries[write - 1];
        const rb_entry *current = &g_entries[read];
        if (rb_path_cmp_ci(previous->type, current->type) == 0 &&
            rb_path_cmp_ci(previous->name, current->name) == 0 &&
            rb_path_cmp_ci(previous->alias, current->alias) == 0)
            continue;                      /* same row from another package */
        if (write != read) g_entries[write] = g_entries[read];
        write++;
    }
    read = g_entry_count - write;
    g_entry_count = write;
    return (int)read;
}

static int rb_validate_manifest_set(void)
{
    size_t i;
    size_t *alias_order;
    int collapsed;
    if (!g_entry_count) return 0;
    qsort(g_entries, g_entry_count, sizeof(g_entries[0]), rb_entry_identity_qsort);
    collapsed = rb_collapse_identical_rows();
    if (collapsed) {
        char line[160];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "resource-bridge composed %d identical row(s) shared between packages",
                    collapsed);
        backend_log(line);
    }
    alias_order = (size_t *)HeapAlloc(GetProcessHeap(), 0,
                                      g_entry_count * sizeof(alias_order[0]));
    if (!alias_order) return 0;
    for (i = 0; i < g_entry_count; i++) alias_order[i] = i;
    g_sort_entries = g_entries;
    qsort(alias_order, g_entry_count, sizeof(alias_order[0]), rb_alias_index_qsort);
    for (i = 1; i < g_entry_count; i++) {
        const rb_entry *a = &g_entries[alias_order[i - 1]];
        const rb_entry *b = &g_entries[alias_order[i]];
        if (rb_path_cmp_ci(a->alias, b->alias) == 0) {
            char line[512];
            /* Name both rows: with several packages installed, "a collision"
             * without the owners is not an actionable diagnostic. */
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "resource-bridge REFUSED: provider path '%s' is claimed by two "
                        "different identities, %s/%s from %s and %s/%s from %s",
                        a->alias, a->type, a->name, a->source,
                        b->type, b->name, b->source);
            backend_log(line);
            HeapFree(GetProcessHeap(), 0, alias_order);
            return 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, alias_order);
    return 1;
}

static int rb_take_pindex_string(const unsigned char *data, size_t length,
                                 size_t *position, const unsigned char **value,
                                 size_t *value_length)
{
    unsigned int n;
    size_t i;
    if (*position > length || length - *position < 4u) return 0;
    n = rb_le32(data + *position);
    *position += 4u;
    if (n > RB_MAX_PINDEX_FIELD || *position > length || n > length - *position) return 0;
    for (i = 0; i < n; i++)
        if (data[*position + i] == 0 || data[*position + i] > 0x7fu) return 0;
    *value = data + *position;
    *value_length = n;
    *position += n;
    return 1;
}

static int rb_slice_cmp(const unsigned char *bytes, size_t length, const char *text)
{
    size_t tl = strlen(text), common = length < tl ? length : tl;
    int c = common ? memcmp(bytes, text, common) : 0;
    if (c) return c;
    return length < tl ? -1 : length != tl;
}

static int rb_row_cmp_entry(const unsigned char *type, size_t type_length,
                            const unsigned char *name, size_t name_length,
                            const unsigned char *alias, size_t alias_length,
                            const rb_entry *entry)
{
    int c = rb_slice_cmp(type, type_length, entry->type);
    if (!c) c = rb_slice_cmp(name, name_length, entry->name);
    if (!c) c = rb_slice_cmp(alias, alias_length, entry->alias);
    return c;
}

static rb_entry *rb_find_exact_row(const unsigned char *type, size_t type_length,
                                   const unsigned char *name, size_t name_length,
                                   const unsigned char *alias, size_t alias_length,
                                   const size_t *order)
{
    size_t lo = 0, hi = g_entry_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        rb_entry *entry = &g_entries[order[mid]];
        int c = rb_row_cmp_entry(type, type_length, name, name_length,
                                 alias, alias_length, entry);
        if (!c) return entry;
        if (c < 0) hi = mid;
        else lo = mid + 1u;
    }
    return NULL;
}

static int rb_parse_pindex(const unsigned char *data, size_t length)
{
    unsigned int rows, ordinal;
    size_t position = 0x24u, i;
    size_t *order;
    if (!data || length < 0x24u || data[0] != 0x05u || memcmp(data + 1, "SER", 3) != 0 ||
        rb_be32(data + 4) != length - 0x20u) return 0;
    rows = rb_be32(data + 0x20u);
    if (rows > RB_MAX_PINDEX_ROWS) return 0;
    order = (size_t *)HeapAlloc(GetProcessHeap(), 0, g_entry_count * sizeof(order[0]));
    if (!order) return 0;
    for (i = 0; i < g_entry_count; i++) order[i] = i;
    g_sort_entries = g_entries;
    qsort(order, g_entry_count, sizeof(order[0]), rb_exact_index_qsort);
    for (ordinal = 0; ordinal < rows; ordinal++) {
        const unsigned char *type, *name, *path;
        size_t type_length, name_length, path_length;
        rb_entry *entry;
        const unsigned char *fixed;
        if (position > length || length - position < 4u) goto failed;
        position += 4u;
        if (!rb_take_pindex_string(data, length, &position, &type, &type_length) ||
            !rb_take_pindex_string(data, length, &position, &name, &name_length) ||
            !rb_take_pindex_string(data, length, &position, &path, &path_length) ||
            position > length || length - position < 21u) goto failed;
        fixed = data + position;
        position += 21u;
        entry = rb_find_exact_row(type, type_length, name, name_length,
                                  path_length ? path : name,
                                  path_length ? path_length : name_length, order);
        if (entry) {
            unsigned long long offset = rb_be64(fixed);
            unsigned int size = rb_be32(fixed + 8);
            unsigned int zsize = rb_be32(fixed + 12);
            unsigned char selector = fixed[20];
            if (entry->matches >= RB_MAX_EQUIVALENT_ROWS) goto failed;
            if (!entry->matches) {
                entry->offset = offset;
                entry->size = size;
                entry->zsize = zsize;
                entry->selector = selector;
            } else if (entry->size != size || entry->zsize != zsize ||
                       entry->selector != selector) {
                goto failed;
            }
            entry->equivalent_offsets[entry->matches++] = offset;
        }
    }
    if (position != length) goto failed;
    HeapFree(GetProcessHeap(), 0, order);
    for (i = 0; i < g_entry_count; i++) {
        if (!g_entries[i].matches) return 0;
        g_equivalent_row_count += g_entries[i].matches - 1u;
    }
    return 1;
failed:
    HeapFree(GetProcessHeap(), 0, order);
    return 0;
}

static int rb_open_archive(const char *base, int selector)
{
    char path[MAX_PATH];
    LARGE_INTEGER size;
    const char *name = selector ? "gameresources.patch" : "gameresources.resources";
    if (_snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", base, name) < 0) return 0;
    g_archives[selector] = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                                      NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                                      FILE_FLAG_RANDOM_ACCESS, NULL);
    if (g_archives[selector] == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(g_archives[selector], &size) || size.QuadPart < 0) return 0;
    g_archive_sizes[selector] = (unsigned long long)size.QuadPart;
    return 1;
}

static int rb_read_stored_at(unsigned char selector, unsigned long long offset,
                             unsigned char *buffer, DWORD length)
{
    LARGE_INTEGER position;
    DWORD got = 0;
    int ok = 0;
    if (selector > 1u || g_archives[selector] == INVALID_HANDLE_VALUE ||
        offset > (unsigned long long)LLONG_MAX || (!buffer && length)) return 0;
    position.QuadPart = (LONGLONG)offset;
    AcquireSRWLockExclusive(&g_archive_lock);
    if (SetFilePointerEx(g_archives[selector], position, NULL, FILE_BEGIN) &&
        ReadFile(g_archives[selector], buffer, length, &got, NULL) && got == length) ok = 1;
    ReleaseSRWLockExclusive(&g_archive_lock);
    return ok;
}

static int rb_equivalent_rows_match(const rb_entry *entry)
{
    unsigned char *first = NULL;
    unsigned char *other = NULL;
    unsigned int duplicate;
    int ok = 0;
    if (!entry || entry->matches <= 1u || !entry->zsize) return 1;
    first = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, RB_COMPARE_CHUNK);
    other = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, RB_COMPARE_CHUNK);
    if (!first || !other) goto done;
    for (duplicate = 1; duplicate < entry->matches; duplicate++) {
        unsigned int position = 0;
        while (position < entry->zsize) {
            DWORD chunk = entry->zsize - position;
            if (chunk > RB_COMPARE_CHUNK) chunk = RB_COMPARE_CHUNK;
            if (!rb_read_stored_at(entry->selector, entry->equivalent_offsets[0] + position,
                                   first, chunk) ||
                !rb_read_stored_at(entry->selector,
                                   entry->equivalent_offsets[duplicate] + position,
                                   other, chunk) ||
                memcmp(first, other, chunk) != 0) goto done;
            position += chunk;
        }
    }
    ok = 1;
done:
    if (other) HeapFree(GetProcessHeap(), 0, other);
    if (first) HeapFree(GetProcessHeap(), 0, first);
    return ok;
}

static int rb_validate_slices_and_open(const char *doom_base)
{
    size_t i;
    unsigned long long total = 0;
    int need[2] = {0, 0};
    for (i = 0; i < g_entry_count; i++) {
        rb_entry *entry = &g_entries[i];
        if (entry->selector > 1u || entry->size > RB_MAX_RESOURCE_BYTES ||
            entry->zsize > RB_MAX_RESOURCE_BYTES ||
            entry->size > RB_MAX_TOTAL_DECODED - total) return 0;
        total += entry->size;
        if (!entry->size && !entry->zsize && !entry->offset) continue;
        if (!entry->size && entry->zsize) return 0;
        if (!entry->zsize) return 0;
        need[entry->selector] = 1;
    }
    if ((need[0] && !rb_open_archive(doom_base, 0)) ||
        (need[1] && !rb_open_archive(doom_base, 1))) return 0;
    for (i = 0; i < g_entry_count; i++) {
        rb_entry *entry = &g_entries[i];
        if (!entry->zsize) continue;
        unsigned int match;
        for (match = 0; match < entry->matches; match++) {
            unsigned long long offset = entry->equivalent_offsets[match];
            if (offset > g_archive_sizes[entry->selector] ||
                entry->zsize > g_archive_sizes[entry->selector] - offset) return 0;
        }
        if (!rb_equivalent_rows_match(entry)) return 0;
    }
    return 1;
}

/* The package set is 26 KB and these captures already carry large frames, so it
 * lives in static storage rather than on the stack: putting it on the stack
 * tripped the /GS guard and terminated DOOM with 0xC0000409. Each capture is a
 * guarded one-shot on a single thread, so a shared buffer is safe here. */
static sh_package g_packages[SH_PACKAGES_MAX];

/* Re-run the manifest capture so a package installed mid-session becomes resolvable without a
 * relaunch. Returns what the fresh capture returned.
 *
 * SAFETY. sh_resource_bridge_open refuses unless the state is READY, so moving the state off
 * READY fences off every reader that has not already entered the lookup. A reader ALREADY
 * inside it cannot be evicted, so the previous entry table is deliberately NOT freed -- it is
 * retired and leaked. That costs a bounded ~26 KB per package install and removes the
 * use-after-free entirely; freeing it would trade a real crash for a small saving.
 *
 * Call at a quiescent moment (the browser, no map loading). A MISS during the rebuild window
 * degrades to the packaged resource, which is the same failure this module already tolerates. */
int sh_resource_bridge_recapture(const char *data_root)
{
    char line[192];
    int ok;
    LONG previous = InterlockedExchange(&g_state, RB_STATE_INSTALLING);

    if (previous == RB_STATE_INSTALLING) {
        /* Another capture is in flight; do not race it. Restore and refuse. */
        InterlockedExchange(&g_state, previous);
        backend_log("resource-bridge RECAPTURE refused: a capture is already in flight");
        return 0;
    }

    /* Retire, do not free. See SAFETY above. */
    g_entries = NULL;
    g_entry_count = 0;
    g_entry_capacity = 0;
    g_decl_count = 0;
    g_equivalent_row_count = 0;
    g_has_manifests = 0;
    rb_close_archives();

    InterlockedExchange(&g_state, RB_STATE_NEW);
    ok = sh_resource_bridge_capture(data_root);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "resource-bridge RECAPTURE %s -- %u entr(ies), %u linked decl(s) now resolvable "
                "(previous table retired, not freed)",
                ok ? "complete" : "FAILED",
                (unsigned)g_entry_count, (unsigned)g_decl_count);
    backend_log(line);
    return ok;
}

int sh_resource_bridge_capture(const char *data_root)
{
    char directory[MAX_PATH], doom_base[MAX_PATH], pindex_path[MAX_PATH];
    rb_manifest_file manifests[RB_MAX_MANIFESTS];
    size_t package_count = 0;
    size_t manifest_count = 0, total_manifest_bytes = 0, pindex_length, i;
    unsigned char *pindex;
    DWORD attributes;
    char line[320];
    if (InterlockedCompareExchange(&g_state, RB_STATE_INSTALLING, RB_STATE_NEW) != RB_STATE_NEW)
        return InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW) == RB_STATE_READY;
    if (!data_root || !data_root[0]) return rb_fail("manifest root path is invalid");
    /* Each installed package carries its own resources directory; they are
     * collected into one set so the existing cross-manifest identity and alias
     * collision checks below cover packages as well as files. */
    if (!sh_packages_enumerate(data_root, g_packages, SH_PACKAGES_MAX, &package_count))
        return rb_fail("the overrides package directory could not be enumerated completely");
    for (i = 0; i < package_count; i++) {
        if (!sh_package_subdir(&g_packages[i], "resources", directory, sizeof(directory)))
            return rb_fail("a package resources path exceeded its bounded length");
        attributes = GetFileAttributesA(directory);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                continue;               /* a package may carry no manifests */
            return rb_fail("manifest root attributes could not be read");
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return rb_fail("manifest root is not a regular directory");
        if (!rb_collect_manifests(directory, g_packages[i].name, manifests,
                                  RB_MAX_MANIFESTS, &manifest_count))
            return rb_fail("manifest enumeration failed");
    }
    if (!manifest_count) {
        backend_log("resource-bridge idle: no *.manifest files");
        InterlockedExchange(&g_state, RB_STATE_READY);
        return 1;
    }
    g_has_manifests = 1;
    for (i = 0; i < manifest_count; i++)
        if (!rb_parse_manifest(&manifests[i], &total_manifest_bytes))
            return rb_fail("manifest parsing, bounds, or allocation failed");
    if (!rb_validate_manifest_set())
        return rb_fail("manifest identity or provider-alias collision");
    if (!rb_get_doom_base(doom_base, sizeof(doom_base)) ||
        _snprintf_s(pindex_path, sizeof(pindex_path), _TRUNCATE,
                    "%s\\gameresources.pindex", doom_base) < 0)
        return rb_fail("installed DOOM base could not be resolved");
    pindex = rb_read_regular_file(pindex_path, RB_MAX_PINDEX_BYTES, &pindex_length);
    if (!pindex) return rb_fail("installed gameresources.pindex could not be read");
    if (!rb_parse_pindex(pindex, pindex_length)) {
        HeapFree(GetProcessHeap(), 0, pindex);
        return rb_fail("manifest triples did not resolve uniquely in the installed pindex");
    }
    HeapFree(GetProcessHeap(), 0, pindex);
    if (!rb_validate_slices_and_open(doom_base))
        return rb_fail("resolved installed archive slices failed validation");
    for (i = 0; i < g_entry_count; i++)
        if (_strnicmp(g_entries[i].alias, "generated/decls/", 16) == 0 &&
            rb_has_suffix_ci(g_entries[i].alias, ".decl")) g_decl_count++;
    InterlockedExchange(&g_state, RB_STATE_READY);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "resource-bridge captured: %zu manifest(s), %zu installed virtual file(s), "
                "%zu linked decl(s), %zu byte-identical duplicate pindex row(s) collapsed; "
                "read-only sparse launch snapshot",
                manifest_count, g_entry_count, g_decl_count, g_equivalent_row_count);
    backend_log(line);
    return 1;
}

void sh_resource_bridge_set_provider_ready(int ready)
{
    InterlockedExchange(&g_provider_ready, ready ? 1 : 0);
}

int sh_resource_bridge_gate_ok(void)
{
    LONG state = InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW);
    if (state == RB_STATE_FAILED) return 0;
    if (state == RB_STATE_INSTALLING) return 0;
    if (state == RB_STATE_NEW) return 1;
    return !g_has_manifests || InterlockedCompareExchange(&g_provider_ready, 0, 0) != 0;
}

int sh_resource_bridge_has_manifests(void)
{
    return InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW) == RB_STATE_READY &&
           g_has_manifests;
}

size_t sh_resource_bridge_entry_count(void)
{
    return InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW) == RB_STATE_READY ?
           g_entry_count : 0;
}

static rb_entry *rb_find_alias(const char *name)
{
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < g_entry_count; i++)
        if (rb_path_cmp_ci(name, g_entries[i].alias) == 0) return &g_entries[i];
    return NULL;
}

static int rb_read_slice(const rb_entry *entry, unsigned char **out, size_t *out_length)
{
    unsigned char *stored = NULL, *decoded = NULL;
    DWORD got = 0;
    LARGE_INTEGER offset;
    int ok = 0;
    if (!entry || !out || !out_length) return 0;
    *out = NULL;
    *out_length = 0;
    decoded = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                         entry->size ? entry->size : 1u);
    if (!decoded) return 0;
    if (!entry->size && !entry->zsize) {
        *out = decoded;
        return 1;
    }
    if (entry->selector > 1u || g_archives[entry->selector] == INVALID_HANDLE_VALUE) goto done;
    if (entry->zsize == entry->size) stored = decoded;
    else {
        stored = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, entry->zsize);
        if (!stored) goto done;
    }
    offset.QuadPart = (LONGLONG)entry->offset;
    AcquireSRWLockExclusive(&g_archive_lock);
    if (SetFilePointerEx(g_archives[entry->selector], offset, NULL, FILE_BEGIN) &&
        ReadFile(g_archives[entry->selector], stored, entry->zsize, &got, NULL) &&
        got == entry->zsize) ok = 1;
    ReleaseSRWLockExclusive(&g_archive_lock);
    if (!ok) goto done;
    if (stored != decoded &&
        sh_inflate_raw(stored, entry->zsize, decoded, entry->size) != entry->size) {
        ok = 0;
        goto done;
    }
    *out = decoded;
    *out_length = entry->size;
    decoded = NULL;
    ok = 1;
done:
    if (stored && stored != decoded && stored != *out) HeapFree(GetProcessHeap(), 0, stored);
    if (decoded) HeapFree(GetProcessHeap(), 0, decoded);
    return ok;
}

int sh_resource_bridge_open(const char *name, unsigned char **out,
                            size_t *out_length, const char **out_source)
{
    rb_entry *entry;
    if (out) *out = NULL;
    if (out_length) *out_length = 0;
    if (out_source) *out_source = NULL;
    if (!out || !out_length ||
        InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW) != RB_STATE_READY ||
        !InterlockedCompareExchange(&g_provider_ready, 0, 0)) return SH_RESOURCE_BRIDGE_MISS;
    entry = rb_find_alias(name);
    if (!entry) return SH_RESOURCE_BRIDGE_MISS;
    if (out_source) *out_source = entry->source;
    if (!rb_read_slice(entry, out, out_length)) {
        char line[SH_DECL_SERVER_SOURCE_CAP + 128];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "resource-bridge ERROR: admitted '%s' could not be read/decoded from installed archives",
                    entry->alias);
        backend_log(line);
        return SH_RESOURCE_BRIDGE_ERROR;
    }
    return SH_RESOURCE_BRIDGE_OPENED;
}

size_t sh_resource_bridge_decl_count(void)
{
    return InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW) == RB_STATE_READY ?
           g_decl_count : 0;
}

static rb_entry *rb_decl_at(size_t index)
{
    size_t i, seen = 0;
    for (i = 0; i < g_entry_count; i++) {
        if (_strnicmp(g_entries[i].alias, "generated/decls/", 16) != 0 ||
            !rb_has_suffix_ci(g_entries[i].alias, ".decl")) continue;
        if (seen++ == index) return &g_entries[i];
    }
    return NULL;
}

int sh_resource_bridge_decl_metadata(size_t index, const char **type,
                                     const char **name, const char **source)
{
    if (InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW) != RB_STATE_READY)
        return 0;
    rb_entry *entry = rb_decl_at(index);
    if (!entry || !type || !name || !source) return 0;
    *type = entry->type;
    *name = entry->name;
    *source = entry->alias;
    return 1;
}

int sh_resource_bridge_read_decl(size_t index, char **body, size_t *length,
                                 const char **reason)
{
    rb_entry *entry;
    unsigned char *bytes = NULL;
    size_t n = 0;
    char *text;
    if (body) *body = NULL;
    if (length) *length = 0;
    if (reason) *reason = "linked decl metadata is invalid";
    if (InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW) != RB_STATE_READY)
        return 0;
    entry = rb_decl_at(index);
    if (!entry || !body || !length || !reason) return 0;
    if (!rb_read_slice(entry, &bytes, &n)) {
        *reason = "linked installed decl could not be read or decoded";
        return 0;
    }
    text = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1u);
    if (!text) {
        HeapFree(GetProcessHeap(), 0, bytes);
        *reason = "linked decl text allocation failed";
        return 0;
    }
    if (n) memcpy(text, bytes, n);
    text[n] = '\0';
    HeapFree(GetProcessHeap(), 0, bytes);
    *body = text;
    *length = n;
    *reason = NULL;
    return 1;
}

#ifdef SH_RESOURCE_BRIDGE_TESTING
void sh_resource_bridge_test_set_doom_base(const char *path)
{
    if (!path) g_test_doom_base[0] = '\0';
    else strncpy_s(g_test_doom_base, sizeof(g_test_doom_base), path, _TRUNCATE);
}

void sh_resource_bridge_test_reset(void)
{
    rb_close_archives();
    rb_release_entries();
    g_has_manifests = 0;
    g_test_doom_base[0] = '\0';
    InterlockedExchange(&g_provider_ready, 0);
    InterlockedExchange(&g_state, RB_STATE_NEW);
}

int sh_resource_bridge_test_entry_metadata(size_t index, const char **alias,
                                           size_t *decoded_bytes,
                                           size_t *stored_bytes,
                                           const char **source)
{
    if (InterlockedCompareExchange(&g_state, RB_STATE_NEW, RB_STATE_NEW) != RB_STATE_READY ||
        index >= g_entry_count || !alias || !decoded_bytes || !stored_bytes || !source) return 0;
    *alias = g_entries[index].alias;
    *decoded_bytes = g_entries[index].size;
    *stored_bytes = g_entries[index].zsize;
    *source = g_entries[index].source;
    return 1;
}
#endif
