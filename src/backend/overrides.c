/* Resource-provider open hook and native idFile streams. Package resources
 * come from immutable compiled snapshots; existing streams retain their bytes
 * across a refresh. See overrides.h for built-in and map-resource ordering.
 * Each provider path has an SEH boundary and preserves native file semantics.
 */
#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <shlobj.h>
#include <intrin.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "overrides.h"
#include "backend_log.h"
#include "perf.h"
#include "decl_text.h"
#include "packages.h"
#include "package_runtime.h"
#include "resource_graph.h"
#include "resource_graph_native.h"
#include "resource_instances.h"
#include "virtual_texture_provider.h"
#include "package_shader_receipt.h"
#include "grid_room_asset.h"
#include "user_overrides.h"
#include "navmesh.h"                /* baked AI navigation, served under the module's own names */
#include "nav_bake.h"           /* navigation baked from the map's own marked volumes */
#include "overrides_baked.h"        /* the built-in "*Custom"-tab default decls (Timeline + Unknown) */

/* Provider open slot offset, +0xf8. Pinned Vulkan evidence: vtable RVA
 * 0x27984a0 and open-slot RVA 0x2798598.
 */
#define OPEN_SLOT_OFFSET 0xf8

/* Pinned Vulkan RVAs for audit and signature repair; they do not gate
 * installation. The supported builds share the idFile ABI while function
 * addresses differ. Installation requires clean SIG_OK matches, host-image
 * containment and a decoded read-only provider vtable. The stream below
 * implements the audited 31-slot ABI.
 */
#define OV_PINNED_RES_PROVIDER_CTOR_RVA 0x1A51070u
#define OV_PINNED_IDFILE_READSTR_RVA    0x0267390u
#define OV_PINNED_IDFILE_COMPARE_RVA    0x0267290u
#define OV_PINNED_IDFILE_WRITESTR_RVA   0x0268470u
#define OV_PINNED_PROVIDER_VTABLE_RVA   0x27984A0u

/* Engine ABI: idFile* open(self, name, uint8 b1, uint8 b2, uint mode). Mode
 * >= 2 bypasses shadowing; preserve both byte arguments when chaining.
 */
typedef void *(*open_fn_t)(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode);

static open_fn_t  g_orig_open  = NULL;   /* the saved engine resource-open (the slot's original value) */
static void     **g_slot       = NULL;   /* the live vtable slot we patched (for uninstall) */
static PVOID volatile g_file_return_observer;
static PVOID volatile g_file_open_guard;
static SRWLOCK g_file_observer_lock=SRWLOCK_INIT;
static volatile LONG g_shadow_count = 0;
typedef struct ov_internal_decl {
    char *name;                   /* exact lower-case decltree/<type>/<name>.decl */
    unsigned char *body;          /* owned by the published snapshot */
    size_t body_length;
} ov_internal_decl;

static SRWLOCK g_internal_lock = SRWLOCK_INIT;
static ov_internal_decl *g_internal_decls;
static size_t g_internal_decl_count;
/* The published keys' positions in key order (R-398): the engine opens every
 * decltree source by name and each registration pass asks about every
 * candidate, so a lookup is a binary search instead of a walk of ~6,000 keys.
 * NULL only when its allocation failed; lookups then walk the table. */
static size_t *g_internal_index;
static volatile LONG g_internal_decl_table_state;

enum {
    OV_INTERNAL_DECL_TABLE_NEW = 0,
    OV_INTERNAL_DECL_TABLE_INSTALLING = 1,
    OV_INTERNAL_DECL_TABLE_READY = 2,
    OV_INTERNAL_DECL_TABLE_FAILED = 3
};

/* Application data root; authored packages live in its overrides directory. */
static char g_root[MAX_PATH] = {0};

/* Native idFile stream head: +0x00 vtable, +0x08 FILE*, +0x10 name, +0x18
 * length, +0x20 short flag. All 31 slots use our stream object; its
 * destructor releases it with HeapFree.
 *
 * fp selects disk backing. Otherwise buf is read-only memory, pos is its
 * cursor, and owns_buf controls whether the destructor frees the payload.
 * Static baked text is borrowed.
 */
typedef struct ov_stream {
    void        *vtable;     /* +0x00 */
    FILE        *fp;         /* +0x08 (NULL for a memory-backed stream) */
    const char  *name;       /* +0x10 (points at the heap-dup'd name appended after the struct) */
    long long    length;     /* +0x18 */
    short        flag16;     /* +0x20 (OG sets 1) */
    const unsigned char *buf;/* memory backing (baked static text, or an owned heap copy) */
    long long    pos;        /* memory-backing read cursor */
    int          owns_buf;   /* 1 -> dtor HeapFrees buf */
} ov_stream;

/* Native virtual-method implementations; self arrives in RCX. */

static void  ov_dtor(ov_stream *s)                                   /* [0] close + free(this) */
{
    if (s) {
        if (s->fp) { fclose(s->fp); s->fp = NULL; }
        if (s->buf && s->owns_buf) HeapFree(GetProcessHeap(), 0, (void *)s->buf);
        s->buf = NULL; s->length = 0; s->name = NULL; s->flag16 = 0;
        HeapFree(GetProcessHeap(), 0, s);
    }
}
static long long ov_ret0_a(ov_stream *s)        { (void)s; return 0; }   /* [1] return 0 */
static long long ov_length(ov_stream *s)        { return s ? s->length : 0; }            /* [3] *(this+0x18) */
static const char *ov_name(ov_stream *s)        { return s ? s->name : NULL; }           /* [4] *(this+0x10) */
static long long ov_read(ov_stream *s, void *buf, uint64_t n)          /* [5] fread(buf,1,n,fp) */
{
    if (!s || !buf) return 0;
    if (n > (uint64_t)SIZE_MAX || n > (uint64_t)INT64_MAX) return 0;
    if (s->fp) return (long long)fread(buf, 1, (size_t)n, s->fp);
    if (s->buf) {                                        /* memory backing: bounded copy + cursor */
        if (s->length < 0 || s->pos < 0 || s->pos > s->length) return 0;
        long long avail = s->length - s->pos;
        long long take  = (avail < (long long)n) ? avail : (long long)n;
        if (take <= 0) return 0;
        memcpy(buf, s->buf + s->pos, (size_t)take);
        s->pos += take;
        return take;
    }
    return 0;
}
static long long ov_write(ov_stream *s, const void *buf, uint64_t n)       /* [6] fwrite(buf,1,n,fp); memory form is read-only */
{
    if (!s || !s->fp || !buf) return 0;
    if (n > (uint64_t)SIZE_MAX || n > (uint64_t)INT64_MAX) return 0;
    return (long long)fwrite(buf, 1, (size_t)n, s->fp);
}
static int       ov_seek(ov_stream *s, long long off, int origin);       /* fwd-decl ([14]) */
/* [7] Match native read-at (Vulkan RVA 0x1a1b520): Seek with ABS=2, then
 * Read.
 */
static long long ov_seekread(ov_stream *s, long long off, void *buf, uint64_t n)
{
    if (!s || ov_seek(s, off, 2) != 0) return 0;
    return ov_read(s, buf, n);
}
/* [8] The native helper at RVA 0x1a1c220 calls Seek(this,off,ABS=2), then Write(this,buf,len). */
static long long ov_seekwrite(ov_stream *s, long long off, const void *buf, uint64_t n)
{
    if (!s || ov_seek(s, off, 2) != 0) return 0;
    return ov_write(s, buf, n);
}
static int       ov_lock(ov_stream *s)          { if (s && s->fp) _lock_file(s->fp);   return 1; }   /* [9] */
static int       ov_unlock(ov_stream *s)        { if (s && s->fp) _unlock_file(s->fp); return 1; }   /* [10] */
static long long ov_length_byseek(ov_stream *s)                      /* [11] stored length */
{
    return s && s->length >= 0 ? s->length : 0;
}
/* +0x60 SetLength always fails: provider streams are read-only. */
static int       ov_set_length(ov_stream *s, long long requested)
{
    (void)s;
    (void)requested;
    return 0;
}
static long long ov_tell(ov_stream *s)                                                                /* [13] ftell */
{
    if (!s) return 0;
    if (s->fp) return _ftelli64(s->fp);
    return s->buf ? s->pos : 0;
}

static int ov_checked_add_i64(long long base, long long offset, long long *out)
{
    if (!out || (offset > 0 && base > LLONG_MAX - offset) ||
        (offset < 0 && base < LLONG_MIN - offset)) return 0;
    *out = base + offset;
    return 1;
}

static int       ov_seek(ov_stream *s, long long off, int origin)    /* [14] idFile: 0=CUR, 1=END, 2=ABS */
{
    if (!s) return -1;
    if (!s->fp) {                                        /* memory backing: move the cursor, clamped */
        if (!s->buf) return -1;
        long long p;
        if (s->length < 0 || s->pos < 0 || s->pos > s->length) return -1;
        if (origin == 0) {                                /* CUR */
            if (!ov_checked_add_i64(s->pos, off, &p)) return -1;
        }
        else if (origin == 1) {                           /* END */
            if (!ov_checked_add_i64(s->length, off, &p)) return -1;
        }
        else if (origin == 2) p = off;                   /* ABS */
        else return -1;                                  /* invalid origin: refuse, preserve cursor */
        if (p < 0) p = 0;
        if (p > s->length) p = s->length;
        s->pos = p;
        return 0;
    }
    int o;
    if (origin == 0) o = SEEK_CUR;
    else if (origin == 1) o = SEEK_END;
    else if (origin == 2) o = SEEK_SET;
    else return -1;
    return _fseeki64(s->fp, off, o);
}
static long long ov_vprintf(ov_stream *s, const char *fmt, va_list ap)   /* [15] vfprintf */
{
    if (!s || !s->fp || !fmt) return 0;
    return (long long)vfprintf(s->fp, fmt, ap);
}
/* Slots [15] and [16] share the native C-varargs printf shape. Both use this
 * FILE-backed thunk; memory streams return zero.
 */
static long long ov_printf_thunk(ov_stream *s, const char *fmt, ...)     /* [15]/[16] varargs entry */
{
    va_list ap; long long r;
    va_start(ap, fmt);
    r = ov_vprintf(s, fmt, ap);
    va_end(ap);
    return r;
}
static long long ov_ret0_c(ov_stream *s)        { (void)s; return 0; }   /* [17] return 0 */
static long long ov_ret0_d(ov_stream *s)        { (void)s; return 0; }   /* [18] return 0 */
static long long ov_ret0_e(ov_stream *s)        { (void)s; return 0; }   /* [19] return 0 */
/* Read-only streams report no writable/physical provider flag. */
static char      ov_provider_flag(ov_stream *s) { (void)s; return 0; }                    /* [20] */
static void      ov_flush_a(ov_stream *s)                                                /* [21] flush + refresh size */
{
    if (s && s->fp) {
        long long pos, length;
        fflush(s->fp);
        pos = _ftelli64(s->fp);
        if (pos >= 0 && _fseeki64(s->fp, 0, SEEK_END) == 0 &&
            (length = _ftelli64(s->fp)) >= 0) s->length = length;
        if (pos >= 0) _fseeki64(s->fp, pos, SEEK_SET);
    }
}
static void      ov_flush_b(ov_stream *s)       { if (s && s->fp) fflush(s->fp); }        /* [22] fflush */
static long long ov_ret1(ov_stream *s)          { (void)s; return 1; }   /* [23] return 1 */
static long long ov_drive_type(ov_stream *s)   { (void)s; return 0; }   /* [24] drive/storage type */
static long long ov_storage_true(ov_stream *s) { (void)s; return 1; }   /* [25] native bool true */
static long long ov_storage_zero_a(ov_stream *s) { (void)s; return 0; } /* [26] reserved zero */
static long long ov_storage_zero_b(ov_stream *s) { (void)s; return 0; } /* [27] reserved zero */

/* Shared idFile vtable is a 31-entry table. Native idStr helpers occupy the final three
 * slots; all must resolve cleanly before the provider hook is published.
 * Preserve slot order even where several methods share an implementation.
 */
static void *g_stream_vtable[31] = {
    (void *)ov_dtor,          /* 0  +0x00 close/dtor */
    (void *)ov_ret0_a,        /* 1  +0x08 */
    (void *)ov_ret1,          /* 2  +0x10 native constant true */
    /* +0x18 is GetName: idLexer copies this pointer into an idStr when
     * reading renderprogs and includes. GetLength is +0x58; returning a
     * length here would treat an integer as a string pointer.
     */
    (void *)ov_name,          /* 3  +0x18 GetName -> name char* */
    (void *)ov_name,          /* 4  +0x20 GetFullPath -> name char* */
    (void *)ov_read,          /* 5  +0x28 Read */
    (void *)ov_write,         /* 6  +0x30 Write */
    (void *)ov_seekread,      /* 7  +0x38 */
    (void *)ov_seekwrite,      /* 8  +0x40 Seek(ABS) + Write */
    (void *)ov_lock,          /* 9  +0x48 Lock */
    (void *)ov_unlock,        /* 10 +0x50 Unlock */
    (void *)ov_length_byseek, /* 11 +0x58 */
    (void *)ov_set_length,    /* 12 +0x60 SetLength -> read-only failure */
    (void *)ov_tell,          /* 13 +0x68 Tell */
    (void *)ov_seek,          /* 14 +0x70 Seek */
    (void *)ov_printf_thunk,  /* 15 +0x78 vfprintf */
    (void *)ov_printf_thunk,  /* 16 +0x80 vfprintf */
    (void *)ov_ret0_c,        /* 17 +0x88 */
    (void *)ov_ret0_d,        /* 18 +0x90 */
    (void *)ov_ret0_e,        /* 19 +0x98 */
    (void *)ov_provider_flag, /* 20 +0xa0 */
    (void *)ov_flush_a,       /* 21 +0xa8 Flush */
    (void *)ov_flush_b,       /* 22 +0xb0 Flush */
    (void *)ov_ret1,          /* 23 +0xb8 */
    (void *)ov_drive_type,    /* 24 +0xc0 drive/storage type = 0 */
    (void *)ov_storage_true,  /* 25 +0xc8 native bool true */
    (void *)ov_storage_zero_a,/* 26 +0xd0 = 0 */
    (void *)ov_storage_zero_b,/* 27 +0xd8 = 0 */
    NULL,                      /* 28 +0xe0 native ReadString helper, published after clean SIG_OK */
    NULL,                      /* 29 +0xe8 native Compare helper, published after clean SIG_OK */
    NULL,                      /* 30 +0xf0 native WriteString helper, published after clean SIG_OK */
};

enum {
    OV_STREAM_HELPERS_NEW = 0,
    OV_STREAM_HELPERS_INSTALLING = 1,
    OV_STREAM_HELPERS_READY = 2,
    OV_STREAM_HELPERS_FAILED = 3
};
static volatile LONG g_stream_helpers_state = OV_STREAM_HELPERS_NEW;

/* Configure all three native idStr slots before publishing the provider hook.
 * Missing or hooked helpers leave a terminal refusal state.
 */
static int ov_stream_helpers_install(void *read_string, int read_clean,
                                     void *compare, int compare_clean,
                                     void *write_string, int write_clean)
{
    LONG state;
    if (!read_string || !compare || !write_string || read_clean != 1 ||
        compare_clean != 1 || write_clean != 1) {
        if (InterlockedCompareExchange(&g_stream_helpers_state,
                                       OV_STREAM_HELPERS_FAILED,
                                       OV_STREAM_HELPERS_NEW) == OV_STREAM_HELPERS_NEW)
            return 0;
        state = InterlockedCompareExchange(&g_stream_helpers_state,
                                           OV_STREAM_HELPERS_NEW,
                                           OV_STREAM_HELPERS_NEW);
        return state == OV_STREAM_HELPERS_READY &&
               g_stream_vtable[28] == read_string &&
               g_stream_vtable[29] == compare &&
               g_stream_vtable[30] == write_string;
    }
    state = InterlockedCompareExchange(&g_stream_helpers_state,
                                       OV_STREAM_HELPERS_INSTALLING,
                                       OV_STREAM_HELPERS_NEW);
    if (state != OV_STREAM_HELPERS_NEW) {
        return state == OV_STREAM_HELPERS_READY &&
               g_stream_vtable[28] == read_string &&
               g_stream_vtable[29] == compare &&
               g_stream_vtable[30] == write_string;
    }

    /* Publish READY only after all helper pointers are set.
     * InterlockedExchange supplies the memory barrier.
     */
    g_stream_vtable[28] = read_string;
    g_stream_vtable[29] = compare;
    g_stream_vtable[30] = write_string;
    InterlockedExchange(&g_stream_helpers_state, OV_STREAM_HELPERS_READY);
    return 1;
}

/* Construct a stream over an already-open FILE* + its known length. Allocates the object + a copy of
 * `name` after it (so the Name slot returns a stable pointer). NULL on alloc failure (caller fcloses). */
static ov_stream *make_stream(FILE *fp, long long length, const char *name)
{
    size_t namelen = name ? strlen(name) : 0;
    if (length < 0 || namelen > SIZE_MAX - sizeof(ov_stream) - 1) return NULL;
    ov_stream *s = (ov_stream *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                          sizeof(ov_stream) + namelen + 1);
    if (!s) return NULL;
    char *namecopy = (char *)(s + 1);
    if (name) memcpy(namecopy, name, namelen);
    namecopy[namelen] = '\0';
    s->vtable = g_stream_vtable;
    s->fp     = fp;
    s->name   = namecopy;
    s->length = length;
    s->flag16 = 1;   /* Native stream flag at +0x20. */
    return s;
}

void *sh_overrides_stream_take_file(FILE *file, uint64_t length, const char *name)
{
    ov_stream *stream = NULL;
    if (file && name && *name && length <= LLONG_MAX &&
        InterlockedCompareExchange(&g_stream_helpers_state, OV_STREAM_HELPERS_NEW,
            OV_STREAM_HELPERS_NEW) == OV_STREAM_HELPERS_READY &&
        _fseeki64(file, 0, SEEK_SET) == 0)
        stream = make_stream(file, (long long)length, name);
    if (!stream && file) fclose(file);
    return stream;
}

/* Construct a MEMORY-backed stream over `buf`/`length`. owns_buf=1 hands the (heap) buffer to the
 * stream's dtor; owns_buf=0 leaves it (the static baked text). NULL on alloc failure. */
static ov_stream *make_mem_stream(const unsigned char *buf, long long length, const char *name, int owns_buf)
{
    if (length < 0 || (!buf && length != 0)) return NULL;
    ov_stream *s = make_stream(NULL, length, name);
    if (!s) return NULL;
    s->buf      = buf;
    s->pos      = 0;
    s->owns_buf = owns_buf;
    return s;
}

void *sh_overrides_stream_copy_bytes(const void *bytes, size_t length, const char *name)
{
    if ((!bytes && length) || length > LLONG_MAX || !name || !*name ||
        InterlockedCompareExchange(&g_stream_helpers_state, OV_STREAM_HELPERS_NEW,
            OV_STREAM_HELPERS_NEW) != OV_STREAM_HELPERS_READY) return NULL;
    unsigned char *owned = HeapAlloc(GetProcessHeap(), 0, length ? length : 1u);
    if (!owned) return NULL;
    if (length) memcpy(owned, bytes, length);
    ov_stream *stream = make_mem_stream(owned, (long long)length, name, 1);
    if (!stream) HeapFree(GetProcessHeap(), 0, owned);
    return stream;
}

/* Resource names normally map to overrides/<name>. The shader-include branch
 * applies only when the first ".inc" occurrence ends the name; it maps to
 * overrides/shader_includes/<relative>. In that branch, strip the first path
 * component unless the name starts with "includes". Normalize separators
 * after joining the data root.
 */

static void default_root(char *out, size_t cap)
{
    char base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus", base);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snapmap-plus");
}

static void resolve_root(char *out, size_t cap)
{
    if (g_root[0]) strncpy_s(out, cap, g_root, _TRUNCATE);
    else default_root(out, cap);
}

static int ov_internal_decl_char(unsigned char c, int allow_slash)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || (allow_slash && c == '.');
}

static int ov_internal_decl_key(const char *type, const char *name,
                                char **out_key)
{
    size_t type_len, name_len, total, i, segment_start;
    char *key;
    if (!type || !name || !type[0] || !name[0] || !out_key) return 0;
    type_len = strlen(type);
    name_len = strlen(name);
    if (type_len > 64 || name_len > 512 ||
        type_len > SIZE_MAX - name_len - sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 6)
        return 0;
    for (i = 0; i < type_len; i++)
        if (!ov_internal_decl_char((unsigned char)type[i], 0)) return 0;
    segment_start = 0;
    for (i = 0; i <= name_len; i++) {
        int at_end = i == name_len;
        if (!at_end && name[i] != '/') {
            if (!ov_internal_decl_char((unsigned char)name[i], 1)) return 0;
            continue;
        }
        if (i == segment_start ||
            (i - segment_start == 1 && name[segment_start] == '.') ||
            (i - segment_start == 2 && name[segment_start] == '.' &&
             name[segment_start + 1] == '.')) return 0;
        segment_start = i + 1;
    }
    total = sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 1 + type_len + 1 +
            name_len + 5 + 1;
    key = (char *)HeapAlloc(GetProcessHeap(), 0, total);
    if (!key) return 0;
    memcpy(key, SH_OVERRIDES_INTERNAL_DECL_PREFIX,
           sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 1);
    memcpy(key + sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 1, type, type_len);
    key[sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 1 + type_len] = '/';
    memcpy(key + sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 1 + type_len + 1,
           name, name_len);
    memcpy(key + sizeof(SH_OVERRIDES_INTERNAL_DECL_PREFIX) - 1 + type_len + 1 + name_len,
           ".decl", 5);
    key[total - 1] = '\0';
    for (i = 0; i + 1 < total; i++) {
        if (key[i] >= 'A' && key[i] <= 'Z')
            key[i] = (char)(key[i] - 'A' + 'a');
    }
    *out_key = key;
    return 1;
}

static void ov_internal_decl_table_free(ov_internal_decl *entries, size_t count)
{
    size_t i;
    if (!entries) return;
    for (i = 0; i < count; i++) {
        if (entries[i].name) HeapFree(GetProcessHeap(), 0, entries[i].name);
        if (entries[i].body) HeapFree(GetProcessHeap(), 0, entries[i].body);
    }
    HeapFree(GetProcessHeap(), 0, entries);
}

static int __cdecl ov_internal_index_compare(void *context, const void *a, const void *b)
{
    const ov_internal_decl *table = (const ov_internal_decl *)context;
    return strcmp(table[*(const size_t *)a].name, table[*(const size_t *)b].name);
}
/* Positions 0..count of table (every name present) in key order; NULL on allocation failure. */
static size_t *ov_internal_index_build(const ov_internal_decl *table, size_t count)
{
    size_t *index;
    if (!count || count > SIZE_MAX / sizeof(*index)) return NULL;
    if (!(index = (size_t *)HeapAlloc(GetProcessHeap(), 0, count * sizeof(*index)))) return NULL;
    for (size_t i = 0; i < count; i++) index[i] = i;
    qsort_s(index, count, sizeof(*index), ov_internal_index_compare, (void *)table);
    return index;
}
/* The table position holding exactly key, or SIZE_MAX. */
static size_t ov_internal_index_find(const ov_internal_decl *table, const size_t *index, size_t count, const char *key)
{
    size_t low = 0, high = count;
    if (!index) {
        for (size_t i = 0; i < count; i++) if (table[i].name && !strcmp(key, table[i].name)) return i;
        return SIZE_MAX;
    }
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(key, table[index[middle]].name);
        if (!order) return index[middle];
        if (order < 0) high = middle; else low = middle + 1;
    }
    return SIZE_MAX;
}
static void ov_internal_index_replace(size_t *index)
{
    if (g_internal_index) HeapFree(GetProcessHeap(), 0, g_internal_index);
    g_internal_index = index;
}

static int ov_internal_decl_table_publish(const sh_overrides_internal_decl_entry *entries, size_t count, int provider_ready, int user_enabled);
static ov_stream * open_internal_decl(const char *name, int *matched);
static ov_stream *open_internal_decl_captured(const char *path, int *matched,
    const char *family, const char *name, sh_package_resource_capture **capture);
static void ov_source_attach(void *stream, const char *path, sh_package_resource_capture *capture, int baseline);
#ifdef SH_OVERRIDES_TESTING
static int (*g_test_vt_recording)(void);
static int (*g_test_vt_observed)(sh_resource_instance_origin *);
void sh_overrides_test_vt_observer(int (*recording)(void), int (*observed)(sh_resource_instance_origin *))
{ g_test_vt_recording=recording; g_test_vt_observed=observed; }
#endif
static int ov_vt_recording(void)
{
#ifdef SH_OVERRIDES_TESTING
    if (g_test_vt_recording) return g_test_vt_recording();
#endif
    return sh_vt_provider_source_recording();
}
static int ov_vt_observed(sh_resource_instance_origin *origin)
{
#ifdef SH_OVERRIDES_TESTING
    if (g_test_vt_observed) return g_test_vt_observed(origin);
#endif
    return sh_vt_provider_source_observed(origin);
}
static int ov_source_target(const char *path, char family[128], char name[4096])
{
    if (sh_resource_graph_native_source_target(family, name) &&
        sh_package_runtime_resource_path_matches(path, family, name)) return 1;
    *family = *name = 0; return 0;
}
size_t sh_overrides_internal_decl_published_count(void);
int sh_overrides_internal_decl_published(const char *name);
int sh_overrides_internal_decl_table_can_install(void);
static int ov_internal_decl_table_merge(const sh_overrides_internal_decl_entry *entries, size_t count, int provider_ready);

static int ov_internal_decl_table_publish_locked(
    const sh_overrides_internal_decl_entry *entries, size_t count,
    int provider_ready, int user_enabled)
{
    ov_internal_decl *copy;
    size_t i, j;
    LONG expected;
    if (!provider_ready || !user_enabled || !entries || count == 0 ||
        count > SIZE_MAX / sizeof(copy[0])) return 0;
    expected = InterlockedCompareExchange(&g_internal_decl_table_state,
                                          OV_INTERNAL_DECL_TABLE_INSTALLING,
                                          OV_INTERNAL_DECL_TABLE_NEW);
    if (expected != OV_INTERNAL_DECL_TABLE_NEW) return 0;
    copy = (ov_internal_decl *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         count * sizeof(copy[0]));
    if (!copy) {
        InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_FAILED);
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (!entries[i].body || entries[i].body_length == 0 ||
            entries[i].body_length > (size_t)INT64_MAX ||
            !ov_internal_decl_key(entries[i].type, entries[i].name, &copy[i].name)) {
            ov_internal_decl_table_free(copy, count);
            InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_FAILED);
            return 0;
        }
        copy[i].body = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                                  entries[i].body_length);
        if (!copy[i].body) {
            ov_internal_decl_table_free(copy, count);
            InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_FAILED);
            return 0;
        }
        memcpy(copy[i].body, entries[i].body, entries[i].body_length);
        copy[i].body_length = entries[i].body_length;
    }
    {
        /* One key per identity: the sorted index makes a duplicate adjacent. */
        size_t *index = ov_internal_index_build(copy, count);
        int duplicate = 0;
        for (i = 0; i < count && !duplicate; i++) {
            if (index) duplicate = i + 1 < count && !strcmp(copy[index[i]].name, copy[index[i + 1]].name);
            else for (j = 0; j < i && !duplicate; j++) duplicate = !strcmp(copy[i].name, copy[j].name);
        }
        if (duplicate) {
            if (index) HeapFree(GetProcessHeap(), 0, index);
            ov_internal_decl_table_free(copy, count);
            InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_FAILED);
            return 0;
        }
        ov_internal_index_replace(index);
    }
    g_internal_decls = copy;
    g_internal_decl_count = count;
    /* Publish only complete keys and bodies; open streams own separate copies. */
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_READY);
    return 1;
}

static int ov_internal_decl_registered_locked(const char *name)
{
    if (!name || !sh_user_overrides_enabled_for_launch() ||
        InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) !=
            OV_INTERNAL_DECL_TABLE_READY) return 0;
    return ov_internal_index_find(g_internal_decls, g_internal_index, g_internal_decl_count, name) != SIZE_MAX;
}

size_t sh_overrides_internal_decl_published_count_locked(void)
{
    if (InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) !=
        OV_INTERNAL_DECL_TABLE_READY)
        return 0;
    return g_internal_decl_count;
}

int sh_overrides_internal_decl_published_locked(const char *name)
{
    size_t i;
    char key[1024];
    size_t length;
    if (!name || !sh_user_overrides_enabled_for_launch() ||
        InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) !=
            OV_INTERNAL_DECL_TABLE_READY) return 0;
    /* Every key is lower case: a case-insensitive match is the lower-cased name's exact key. */
    length = strlen(name);
    if (length < sizeof(key)) {
        for (i = 0; i <= length; i++) key[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] - 'A' + 'a') : name[i];
        return ov_internal_index_find(g_internal_decls, g_internal_index, g_internal_decl_count, key) != SIZE_MAX &&
            sh_package_runtime_decl_alias_exists(name);
    }
    for (i = 0; i < g_internal_decl_count; i++)
        if (_stricmp(name, g_internal_decls[i].name) == 0)
            return sh_package_runtime_decl_alias_exists(name);
    return 0;
}

int sh_overrides_internal_decl_table_can_install_locked(void)
{
    return g_orig_open != NULL && sh_user_overrides_enabled_for_launch() &&
           InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) ==
               OV_INTERNAL_DECL_TABLE_NEW;
}

/* A runtime merge replaces an immutable table. Existing opens own their
 * bytes, so publication never hides the old table or invalidates a stream. */
void sh_overrides_internal_decl_table_reopen(void)
{
    backend_log("B1: overrides internal decl table remains available during refresh");
}

static int ov_internal_decl_table_merge_locked(
    const sh_overrides_internal_decl_entry *entries, size_t count, int provider_ready)
{
    ov_internal_decl *merged;
    ov_internal_decl *old = g_internal_decls;
    size_t old_count = g_internal_decl_count;
    size_t total, i, j, at = 0, changed = 0, unchanged = 0;
    size_t *supplied = NULL;
    char line[256];

    if (!provider_ready || !sh_user_overrides_enabled_for_launch()) return 0;
    if (!entries || count == 0) return 0;
    if (count > SIZE_MAX - old_count) return 0;
    total = old_count + count;
    if (total > SIZE_MAX / sizeof(merged[0])) return 0;
    merged = (ov_internal_decl *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                           total * sizeof(merged[0]));
    if (!merged) return 0;

    /* The new entries first, so a key collision resolves in their favour below. */
    for (i = 0; i < count; i++) {
        if (!entries[i].body || entries[i].body_length == 0 ||
            entries[i].body_length > (size_t)INT64_MAX ||
            !ov_internal_decl_key(entries[i].type, entries[i].name, &merged[at].name)) {
            ov_internal_decl_table_free(merged, total);
            return 0;
        }
        merged[at].body = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                                     entries[i].body_length);
        if (!merged[at].body) {
            ov_internal_decl_table_free(merged, total);
            return 0;
        }
        memcpy(merged[at].body, entries[i].body, entries[i].body_length);
        merged[at].body_length = entries[i].body_length;
        at++;
    }
    /* The supplied keys in order: a duplicate within the new set is adjacent,
     * and each old key finds the entry superseding it by binary search. */
    supplied = ov_internal_index_build(merged, count);
    for (i = 0; i < count; i++) {
        int duplicate = 0;
        if (supplied) duplicate = i + 1 < count && !strcmp(merged[supplied[i]].name, merged[supplied[i + 1]].name);
        else for (j = 0; j < i && !duplicate; j++) duplicate = !strcmp(merged[i].name, merged[j].name);
        if (duplicate) {
            if (supplied) HeapFree(GetProcessHeap(), 0, supplied);
            ov_internal_decl_table_free(merged, total);
            return 0;                      /* duplicate within the new set */
        }
    }

    /* Copy unchanged identities into the replacement snapshot. */
    for (i = 0; i < old_count; i++) {
        int superseded = 0;
        if (!old[i].name || !old[i].body) continue;
        j = ov_internal_index_find(merged, supplied, count, old[i].name);
        superseded = j != SIZE_MAX;
        if (superseded) {
            if (merged[j].body_length == old[i].body_length &&
                memcmp(merged[j].body, old[i].body, old[i].body_length) == 0) unchanged++;
            else changed++;
            continue;
        }
        merged[at].name = (char *)HeapAlloc(GetProcessHeap(), 0, strlen(old[i].name) + 1);
        merged[at].body = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, old[i].body_length);
        if (!merged[at].name || !merged[at].body) {
            if (supplied) HeapFree(GetProcessHeap(), 0, supplied);
            ov_internal_decl_table_free(merged, total);
            return 0;
        }
        strcpy_s(merged[at].name, strlen(old[i].name) + 1, old[i].name);
        memcpy(merged[at].body, old[i].body, old[i].body_length);
        merged[at].body_length = old[i].body_length;
        at++;
    }

    if (supplied) HeapFree(GetProcessHeap(), 0, supplied);
    g_internal_decls = merged;
    g_internal_decl_count = at;
    ov_internal_index_replace(ov_internal_index_build(merged, at));
    ov_internal_decl_table_free(old, old_count);
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_READY);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: overrides internal decl table MERGED -- %zu supplied (%zu added, %zu changed, %zu unchanged) "
                "+ %zu carried forward = %zu published (previous snapshot released)",
                count, count - changed - unchanged, changed, unchanged, at - count, at);
    backend_log(line);
    return 1;
}

int sh_overrides_internal_decl_table_install(
    const sh_overrides_internal_decl_entry *entries, size_t count)
{
    return ov_internal_decl_table_publish(entries, count, g_orig_open != NULL,
                                          sh_user_overrides_enabled_for_launch());
}

#ifdef SH_OVERRIDES_TESTING
void sh_overrides_test_internal_decl_table_reset(void)
{
    /* Test-only reset occurs before any engine thread can retain a stream. */
    ov_internal_decl_table_free(g_internal_decls, g_internal_decl_count);
    g_internal_decls = NULL;
    g_internal_decl_count = 0;
    ov_internal_index_replace(NULL);
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_NEW);
}

int sh_overrides_test_internal_decl_table_install(
    const sh_overrides_internal_decl_entry *entries, size_t count)
{
    return ov_internal_decl_table_publish(entries, count, 1,
                                          sh_user_overrides_enabled_for_launch());
}

int sh_overrides_test_internal_decl_registered(const char *key)
{
    int registered;
    AcquireSRWLockShared(&g_internal_lock);
    registered = ov_internal_decl_registered_locked(key);
    ReleaseSRWLockShared(&g_internal_lock);
    return registered;
}

void *sh_overrides_test_internal_decl_open(const char *name)
{
    int matched = 0;
    return open_internal_decl(name, &matched);
}

void *sh_overrides_test_memory_stream(const unsigned char *body, size_t length)
{
    unsigned char *copy;
    ov_stream *stream;
    if (!body || length > INT64_MAX) return NULL;
    copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, length ? length : 1u);
    if (!copy) return NULL;
    memcpy(copy, body, length);
    stream = make_mem_stream(copy, (long long)length, "memory-test", 1);
    if (!stream) HeapFree(GetProcessHeap(), 0, copy);
    return stream;
}

long long sh_overrides_test_stream_read(void *stream, void *buffer, uint64_t length)
{
    return ((long long (*)(void *,void *,uint64_t))(*(void ***)stream)[5])(stream,buffer,length);
}

long long sh_overrides_test_stream_read_at(void *stream, long long offset,
                                            void *buffer, uint64_t length)
{
    return ov_seekread((ov_stream *)stream, offset, buffer, length);
}

long long sh_overrides_test_stream_write(void *stream, const void *buffer, uint64_t length)
{
    return ov_write((ov_stream *)stream, buffer, length);
}

long long sh_overrides_test_stream_write_at(void *stream, long long offset,
                                             const void *buffer, uint64_t length)
{
    return ov_seekwrite((ov_stream *)stream, offset, buffer, length);
}

int sh_overrides_test_stream_seek(void *stream, long long offset, int origin)
{
    return ov_seek((ov_stream *)stream, offset, origin);
}

long long sh_overrides_test_stream_length(void *stream)
{
    return ov_length_byseek((ov_stream *)stream);
}

int sh_overrides_test_stream_true_flag(void *stream)
{
    return (int)((long long(*)(ov_stream *))g_stream_vtable[2])((ov_stream *)stream);
}

int sh_overrides_test_stream_set_length(void *stream, long long length)
{
    return ((int(*)(ov_stream *, long long))g_stream_vtable[12])((ov_stream *)stream, length);
}

size_t sh_overrides_test_stream_vtable_slots(void)
{
    return sizeof g_stream_vtable / sizeof g_stream_vtable[0];
}

void *sh_overrides_test_stream_vtable_slot(size_t index)
{
    return index < sizeof g_stream_vtable / sizeof g_stream_vtable[0] ?
           g_stream_vtable[index] : NULL;
}

int sh_overrides_test_stream_helpers_configure(void *read_string, int read_clean,
                                                void *compare, int compare_clean,
                                                void *write_string, int write_clean)
{
    return ov_stream_helpers_install(read_string, read_clean, compare, compare_clean,
                                     write_string, write_clean);
}

int sh_overrides_test_stream_helpers_ready(void)
{
    return InterlockedCompareExchange(&g_stream_helpers_state,
                                      OV_STREAM_HELPERS_NEW,
                                      OV_STREAM_HELPERS_NEW) == OV_STREAM_HELPERS_READY;
}

void sh_overrides_test_stream_helpers_reset(void)
{
    g_stream_vtable[28] = NULL;
    g_stream_vtable[29] = NULL;
    g_stream_vtable[30] = NULL;
    InterlockedExchange(&g_stream_helpers_state, OV_STREAM_HELPERS_NEW);
}

void *sh_overrides_test_stream_open_file(const char *path)
{
    FILE *fp;
    long long length;
    ov_stream *stream;
    if (!path) return NULL;
    fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    length = _ftelli64(fp);
    if (length < 0 || _fseeki64(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    stream = make_stream(fp, length, path);
    if (!stream) fclose(fp);
    return stream;
}

void sh_overrides_test_stream_close(void *stream)
{
    if(stream) ((void (*)(void *,unsigned))(*(void ***)stream)[0])(stream,1);
}
#endif

int sh_overrides_get_root(char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    resolve_root(out, cap);
    return out[0] != '\0';
}

/* ============================================================ package resolution ==================*/

/* The compiler publishes one complete inventory. Resource opens retain that
 * snapshot; a failed refresh leaves it available. */
static volatile LONG g_ov_pkg_generation;

unsigned long sh_overrides_rescan_packages_activated(sh_package_activation_fn activate, void *context)
{
    char root[MAX_PATH], line[192];
    const sh_package_compilation *compiled;
    size_t count;
    LONG generation;
    if (!sh_user_overrides_enabled_for_launch()) return 0;
    resolve_root(root, sizeof(root));
    if (!root[0] || !sh_package_runtime_refresh_activated(root, (sh_package_activation_guard){
            sh_nav_bake_source_update_begin, sh_nav_bake_source_update_end}, activate, context)) {
        backend_log("B1: overrides package inventory REFUSED -- preparation or activation failed");
        return SH_OVERRIDES_RESCAN_FAILED;
    }
    compiled = sh_package_runtime_acquire();
    count = compiled ? compiled->sources->package_count : 0;
    sh_package_runtime_release();
    generation = InterlockedIncrement(&g_ov_pkg_generation);
    snprintf(line, sizeof(line),
        "B1: overrides package list RE-SCANNED -- %zu package(s) now visible (generation %ld)",
        count, (long)generation);
    backend_log(line);
    /* This diagnostic return shares a Win32 unsigned-long ABI with existing
     * callers. Success must never collide with the failure sentinel. */
    return count >= SH_OVERRIDES_RESCAN_FAILED ? SH_OVERRIDES_RESCAN_FAILED - 1 : (unsigned long)count;
}

unsigned long sh_overrides_rescan_packages(void)
{
    return sh_overrides_rescan_packages_activated(NULL, NULL);
}

int sh_overrides_activate_map(sh_package_map_plan *plan, sh_package_activation_fn activate, void *context)
{
    if (!sh_user_overrides_enabled_for_launch()) return 0;
    if (!sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){
        sh_nav_bake_source_update_begin, sh_nav_bake_source_update_end}, activate, context)) return 0;
    InterlockedIncrement(&g_ov_pkg_generation);
    backend_log(plan ? "B1: prepared map resource provider activated" : "B1: local resource provider restored");
    return 1;
}

int sh_overrides_activate_occurrences(const sh_package_occurrence_snapshot *map,
    sh_package_activation_fn activate, void *context)
{
    if (!sh_user_overrides_enabled_for_launch()) return 0;
    if (!sh_package_runtime_activate_occurrences(map, (sh_package_activation_guard){
        sh_nav_bake_source_update_begin, sh_nav_bake_source_update_end}, activate, context)) return 0;
    InterlockedIncrement(&g_ov_pkg_generation);
    return 1;
}

/* ====================================================== built-in default lookup + validation =======*/

/* Path-tolerant name compare for the baked table: case-insensitive, '/' == '\\' (the engine asks with
 * forward slashes; be robust to either). */
static int ov_name_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

/* The baked default for `name`, or NULL if `name` is not a built-in. */
static const ov_baked_decl_t *find_baked(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]; i++)
        if (ov_name_eq(name, g_ov_baked_decls[i].name)) return &g_ov_baked_decls[i];
    return NULL;
}

/* Capture the provider object from the first hook call so original-resource
 * reads can reuse its native self pointer.
 */
static void *volatile g_provider_self = NULL;

static unsigned char *read_installed_resource(const char *name, size_t *out_len)
{
    typedef long long (*ov_len_fn)(void *self);
    typedef long long (*ov_read_fn)(void *self, void *buf, uint64_t n);
    typedef void      (*ov_close_fn)(void *self);
    void *f = NULL;
    void **vt;
    unsigned char *buf = NULL;
    long long len;

    if (out_len) *out_len = 0;
    if (!name || !g_orig_open || !g_provider_self) return NULL;

    __try {
        /* mode 2 is the hook's own no-shadow guard, so this cannot re-enter us
         * even though the slot still points at ov_open_hook. */
        f = g_orig_open(g_provider_self, name, 0xff, 0xff, 2);
        if (!f) return NULL;
        vt = *(void ***)f;
        if (!vt) return NULL;

        len = ((ov_len_fn)vt[11])(f);                    /* +0x58 GetLength */
        if (len <= 0 || (unsigned long long)len > SH_SMNAV_MAX_PAYLOAD) {
            ((ov_close_fn)vt[0])(f);
            return NULL;
        }
        buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)len);
        if (!buf) {
            ((ov_close_fn)vt[0])(f);
            return NULL;
        }
        if (((ov_read_fn)vt[5])(f, buf, (uint64_t)len) != len) {   /* +0x28 Read */
            HeapFree(GetProcessHeap(), 0, buf);
            ((ov_close_fn)vt[0])(f);
            return NULL;
        }
        ((ov_close_fn)vt[0])(f);                         /* +0x00 close + free */
        if (out_len) *out_len = (size_t)len;
        return buf;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Release our output buffer and report failure if a native stream
         * call faults.
         */
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        if (out_len) *out_len = 0;
        return NULL;
    }
}

/* Inputs to product transforms use the same published package result as a
 * direct engine read. Stay below Grid/navigation here so derived reads cannot
 * recurse into themselves. An owned read failure must not select stock bytes. */
static unsigned char *read_effective_resource(const char *name, size_t *out_len)
{
    unsigned char *compiled = NULL, *copy;
    const unsigned char *source;
    const ov_baked_decl_t *baked;
    size_t length = 0;
    int claimed = 0;
    if (out_len) *out_len = 0;
    if (!name || !out_len) return NULL;
    if (sh_user_overrides_enabled_for_launch())
        claimed = sh_package_runtime_read(name, &compiled, &length);
    if (claimed < 0) { free(compiled); return NULL; }
    if (claimed) source = compiled;
    else {
        baked = find_baked(name);
        if (!baked) {
            copy = read_installed_resource(name, out_len);
            if (copy) sh_resource_graph_file(name);
            return copy;
        }
        source = (const unsigned char *)baked->text; length = baked->len;
    }
    copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, length ? length : 1u);
    if (copy) {
        if (length) memcpy(copy, source, length);
        *out_len = length;
        sh_resource_graph_file(name);
    }
    free(compiled); return copy;
}

typedef struct grid_source_context {
    const char *target;
    sh_resource_graph_frame frame;
    int active;
} grid_source_context;

static unsigned char *grid_read_effective(void *context,const char *name,size_t *length)
{
    grid_source_context *source = context;
    /* The transform owns an exact file identity even when a preview requests
     * it outside a native parse. Later native loads can follow these inputs. */
    if (!source->active) {
        sh_resource_graph_begin(&source->frame, "", source->target);
        source->active = 1;
    }
    return read_effective_resource(name,length);
}
static void grid_release_effective(void *context,void *bytes)
{
    (void)context;HeapFree(GetProcessHeap(),0,bytes);
}

static int grid_aas_name(const char *name)
{return name&&(strstr(name,".aas_")||strstr(name,".baas_"));}
static int grid_open_validated(const char *name,unsigned char **out,size_t *length)
{
    grid_source_context source = {0};
    int claimed = -1;
    source.target = name;
    __try {
        char *canonical = NULL, *normalized = sh_package_engine_path(name);
        int identity = normalized ? sh_grid_asset_canonical(normalized, &canonical) : 0;
        free(normalized);
        /* A compiler-owned private output already includes its generated
         * contribution and every authored input. Do not generate over it. */
        if (identity > 0 && sh_user_overrides_enabled_for_launch())
            claimed = sh_package_runtime_read(name, out, length);
        else claimed = identity < 0 ? -1 : 0;
        free(canonical);
        if (!claimed) claimed=sh_grid_asset_open(name,grid_read_effective,grid_release_effective,&source,out,length);
        if(claimed>0&&grid_aas_name(name)){
            char error[192];
            if(!sh_navmesh_validate_aas(*out,*length,error,sizeof error)){
                backend_log(error);free(*out);*out=NULL;*length=0;claimed=-1;
            }
        }
    } __finally {
        if (source.active) sh_resource_graph_end(&source.frame, !AbnormalTermination() && claimed > 0);
    }
    return claimed;
}

unsigned char *sh_overrides_read_engine_resource(const char *name,size_t *out_len)
{
    unsigned char *generated=NULL,*copy=NULL;size_t length=0;int claimed;
    if(out_len)*out_len=0;
    if(!name)return NULL;
    /* Navigation consumes the resized Grid result where applicable, otherwise
     * the compiled base. Neither path can re-enter map navigation generation. */
    claimed=grid_open_validated(name,&generated,&length);
    if(!claimed)return read_effective_resource(name,out_len);
    if(claimed>0){copy=(unsigned char*)HeapAlloc(GetProcessHeap(),0,length);
        if(copy){memcpy(copy,generated,length);if(out_len)*out_len=length;}}
    free(generated);return copy;
}

static int ov_owned(void **out, void *stream) { *out = stream; return 1; }
static int ov_owned_source(void **out, void *stream, const char *path,
    sh_package_resource_capture *capture, int baseline)
{
    ov_source_attach(stream, path, capture, baseline);
    return ov_owned(out, stream);
}

/* The shadow's claim for a named open, never chaining to the engine. Returns 1
 * when the shadow owns the name: *out is its stream, or NULL when that owned
 * name is unavailable, which never resolves to another payload. Returns 0
 * when the name belongs to the engine. Called for mode < 2 opens only. */
static int ov_claim(const char *name, void **out)
{
    char source_family[128], source_name[4096];
    sh_package_resource_capture *capture = NULL;
    int source_target = ov_source_target(name, source_family, source_name);
    int capture_file = !source_target && ov_vt_recording();
    const char *capture_family = source_target ? source_family : NULL;
    const char *capture_name = source_target ? source_name : NULL;
    /* Source preparation is a synchronous original-only read scope. Includes
     * and ancestors must use the same captured installed view as the root,
     * including when campaign files are outside the active SnapMap mount. */
    {
        unsigned char *bytes = NULL, *owned = NULL; size_t length = 0;
        int claimed = sh_resource_graph_native_cook_input(name, &bytes, &length);
        if (claimed) {
            ov_stream *stream = NULL;
            if (claimed > 0 && length <= LLONG_MAX) {
                owned = HeapAlloc(GetProcessHeap(), 0, length ? length : 1u);
                if (owned) {
                    if (length) memcpy(owned, bytes, length);
                    stream = make_mem_stream(owned, (long long)length, name, 1);
                }
            }
            free(bytes);
            if (!stream && owned) HeapFree(GetProcessHeap(), 0, owned);
            return ov_owned(out, stream);
        }
    }

    /* Let embedded or marked-volume navigation take precedence over the
     * resized room's base AAS; its source reader still resolves that base. */
    if(grid_aas_name(name)){
        unsigned char *nav=NULL;size_t length=0;
        if(!sh_navmesh_open(name,&nav,&length))
            sh_nav_bake_open(name,sh_overrides_read_engine_resource,&nav,&length);
        if(nav){void *stream=make_mem_stream(nav,(long long)length,name,1);
            if(stream)return ov_owned_source(out,stream,name,NULL,0);HeapFree(GetProcessHeap(),0,nav);return ov_owned(out,NULL);}
    }

    {
        unsigned char *generated=NULL,*owned=NULL;size_t bytes=0;
        int claimed=grid_open_validated(name,&generated,&bytes);
        if(claimed){
            ov_stream *stream=NULL;
            if(claimed>0&&generated&&bytes){
                owned=(unsigned char*)HeapAlloc(GetProcessHeap(),0,bytes);
                if(owned){memcpy(owned,generated,bytes);stream=make_mem_stream(owned,(long long)bytes,name,1);}
            }
            free(generated);
            if(!stream&&owned)HeapFree(GetProcessHeap(),0,owned);
            /* A malformed or unsupported private resource cannot resolve to
             * another room's stock payload. */
            return ov_owned_source(out, stream, name, NULL, 0);
        }
    }

    /* Map-carried navigation precedes disk overrides and is independent of
     * the launch user-layer gate.
     */
    if (!grid_aas_name(name)) {
        unsigned char *nav = NULL;
        size_t nav_len = 0;
        /* Embedded navigation takes precedence over dynamically generated output. */
        if (!sh_navmesh_open(name, &nav, &nav_len))
            sh_nav_bake_open(name, sh_overrides_read_engine_resource, &nav, &nav_len);
        if (nav) {
            ov_stream *s = make_mem_stream(nav, (long long)nav_len, name, 1);
            if (s) return ov_owned_source(out, s, name, NULL, 0);
            HeapFree(GetProcessHeap(), 0, nav);
        }
    }

    {
        ov_stream *internal = NULL;
        int internal_match = 0;
        SH_PERF_BEGIN(t0);
        __try {
            internal = open_internal_decl_captured(name, &internal_match, capture_family, capture_name,
                (source_target || capture_file) ? &capture : NULL);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            internal = NULL;
        }
        SH_PERF_END(SH_PERF_INTERNAL_DECL, t0);
        if (internal_match) {
            /* A published identity is authoritative. Do not let an
             * allocation/read failure fall through to a physical file or the
             * packaged resource under the same canonical name. */
            if (!internal) { sh_package_runtime_resource_capture_release(capture); return ov_owned(out, NULL); }
            unsigned long n = (unsigned long)InterlockedIncrement(&g_shadow_count);
            char line[MAX_PATH + 160];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B1: overrides internal decl FIRED for '%s' (%lld bytes) [#%lu]",
                        name, internal->length, n);
            backend_log(line);
            return ov_owned_source(out, internal, name, capture, 0);
        }
    }

    if (sh_user_overrides_enabled_for_launch()) {
        FILE *file = NULL;
        uint64_t file_length = 0;
        unsigned char *bytes = NULL;
        size_t length = 0;
        int claimed = sh_package_runtime_open_file_captured(name, &file, &file_length, capture_family, capture_name,
            (source_target || capture_file) ? &capture : NULL);
        if (claimed) {
            ov_stream *stream = claimed > 0 && file_length <= LLONG_MAX ?
                make_stream(file, (long long)file_length, name) : NULL;
            if (!stream && file) fclose(file);
            if (stream) InterlockedIncrement(&g_shadow_count);
            return ov_owned_source(out, stream, name, capture, 0);
        }
        claimed = sh_package_runtime_read_captured(name, &bytes, &length, capture_family, capture_name,
            (source_target || capture_file) ? &capture : NULL);
        if (claimed) {
            unsigned char *owned = NULL;
            ov_stream *stream = NULL;
            if (claimed > 0) {
                owned = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, length ? length : 1u);
                if (owned) {
                    if (length) memcpy(owned, bytes, length);
                    stream = make_mem_stream(owned, (long long)length, name, 1);
                }
            }
            free(bytes);
            if (!stream && owned) HeapFree(GetProcessHeap(), 0, owned);
            if (stream) InterlockedIncrement(&g_shadow_count);
            return ov_owned_source(out, stream, name, capture, 0);
        }
    }

    {
        ov_stream *s = NULL;
        const char *src = NULL;
        __try {
            const ov_baked_decl_t *baked = find_baked(name);
            int user_on = sh_user_overrides_enabled_for_launch();
            if (s == NULL && baked != NULL) {
                s = make_mem_stream((const unsigned char *)baked->text, (long long)baked->len, name, 0);
                if (s && src == NULL)
                    src = user_on ? "built-in" : "built-in (user layer disabled by config)";
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s = NULL;   /* any fault in the shadow path -> fall through to the original open */
        }
        if (s != NULL) {
            unsigned long n = (unsigned long)InterlockedIncrement(&g_shadow_count);
            char line[MAX_PATH + 160];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B1: overrides file-shadow FIRED [%s] for '%s' (%lld bytes) [#%lu]",
                        src ? src : "?", name, s->length, n);
            backend_log(line);
            return ov_owned_source(out, s, name, NULL, 1);
        }

    }
    return 0;
}

static void *ov_open_body(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode)
{
#ifndef SH_OVERRIDES_TESTING
    if (g_orig_open == NULL) return NULL;   /* defensive: never happens once installed */
#endif
    if (g_provider_self == NULL) g_provider_self = self;

    if (mode < 2 && name) {
        void *claimed = NULL;
        if (ov_claim(name, &claimed)) return claimed;
    }

    /* No shadow, or mode guard: chain with the original byte arguments. */
    {
        void *chained;
        SH_PERF_BEGIN(t0);
        chained = g_orig_open ? g_orig_open(self, name, (unsigned char)(b1 & 0xff), (unsigned char)(b2 & 0xff), mode) : NULL;
        SH_PERF_END(SH_PERF_ENGINE_OPEN, t0);
        if (chained && name && mode < 2) ov_source_attach(chained, name, NULL, 1);
        return chained;
    }
}

/* Observe the bytes actually returned to a fixed shader declaration's parser.
 * Keep the original idFile object and every name/time/seek/provider method.
 * Only Read and destruction are wrapped in a private copy of the audited base
 * vtable. No extra file reads, source replacement or inferred baseline occurs.
 * Files whose metadata cannot be observed keep their original behavior and do
 * not acquire a positive receipt. */
typedef struct ov_source_fault {
    void *consumer, *input;
    uint64_t consumer_token, input_token;
    struct ov_source_fault *next;
} ov_source_fault;
typedef struct ov_receipt_stream {
    void *table[31];
    void **original;
    char *path;
    char *family, *name;
    sh_package_resource_capture *capture;
    sh_package_selector_hold *opening;
    int baseline;
    sh_resource_instance_origin *vt_origin;
    int vt_origin_tried, vt_fault, source_unknown, closed;
    volatile LONG references;
    uint64_t source_serial;
    ov_source_fault first_fault;
    struct ov_receipt_stream *source_next;
    struct ov_receipt_stream *failed_next;
    struct ov_receipt_stream *active_next;
    void *active_stream;
    void *failed_stream;
} ov_receipt_stream;
static SRWLOCK g_receipt_active_lock=SRWLOCK_INIT;
static ov_receipt_stream *g_receipt_active;
static void ov_receipt_unlink_active(ov_receipt_stream *receipt)
{
    AcquireSRWLockExclusive(&g_receipt_active_lock);
    ov_receipt_stream **slot=&g_receipt_active;
    while(*slot && *slot!=receipt)slot=&(*slot)->active_next;
    if(*slot)*slot=receipt->active_next;
    receipt->active_next=NULL;receipt->active_stream=NULL;
    ReleaseSRWLockExclusive(&g_receipt_active_lock);
}
static SRWLOCK g_receipt_failed_lock = SRWLOCK_INIT;
static ov_receipt_stream *g_receipt_failed;
static SRWLOCK g_receipt_sources_lock = SRWLOCK_INIT;
static ov_receipt_stream *g_receipt_sources;
static uint64_t g_receipt_source_serial, g_receipt_source_generation;
static void ov_receipt_release(ov_receipt_stream *receipt)
{
    if (InterlockedDecrement(&receipt->references)) return;
    sh_resource_instances_origin_unhold(receipt->vt_origin);
    sh_package_runtime_selectors_release(receipt->opening);
    sh_package_runtime_resource_capture_release(receipt->capture);
    ov_source_fault *fault = receipt->first_fault.next;
    while (fault) { ov_source_fault *next = fault->next; free(fault); fault = next; }
    free(receipt->family); free(receipt->name); free(receipt->path); free(receipt);
}
static void ov_receipt_source_fault(ov_receipt_stream *receipt)
{
    ov_source_fault value = {0};
    if (!sh_resource_graph_native_source_fault_target(&value.consumer, &value.consumer_token) ||
        !sh_resource_graph_native_source_fault_input(&value.input, &value.input_token) ||
        !value.consumer || !value.consumer_token || !value.input || !value.input_token) {
        receipt->source_unknown = 1;
        sh_package_runtime_source_cleanup_failed();
        return;
    }
    AcquireSRWLockExclusive(&g_receipt_sources_lock);
    ov_source_fault *last = &receipt->first_fault;
    if (!last->consumer) {
        *last = value;
        receipt->source_serial = ++g_receipt_source_serial;
        InterlockedIncrement(&receipt->references); /* List owns the exact opening. */
        receipt->source_next = g_receipt_sources; g_receipt_sources = receipt;
    } else {
        while (last->consumer != value.consumer || last->consumer_token != value.consumer_token ||
               last->input != value.input || last->input_token != value.input_token) {
            if (!last->next) {
                last->next = malloc(sizeof(*last));
                if (!last->next) { receipt->source_unknown = 1; break; }
                *last->next = value;
            }
            last = last->next;
        }
    }
    ++g_receipt_source_generation; /* Includes a repeated failure of a recovered token. */
    ReleaseSRWLockExclusive(&g_receipt_sources_lock);
    if (receipt->source_unknown) sh_package_runtime_source_cleanup_failed();
}
/* Pin one list member before calling the instance registry. No registry or
 * provider callback executes under the receipt lock. Serial order makes list
 * removal safe without a snapshot allocation on the failure path. */
static ov_receipt_stream *ov_source_next(uint64_t after)
{
    ov_receipt_stream *result = NULL;
    AcquireSRWLockShared(&g_receipt_sources_lock);
    for (ov_receipt_stream *p = g_receipt_sources; p; p = p->source_next)
        if (p->source_serial > after && (!result || p->source_serial < result->source_serial)) result = p;
    if (result) InterlockedIncrement(&result->references);
    ReleaseSRWLockShared(&g_receipt_sources_lock);
    return result;
}
void sh_overrides_collect_source_receipts(void)
{
    uint64_t serial = 0;
    ov_receipt_stream *receipt;
    while ((receipt = ov_source_next(serial)) != NULL) {
        serial = receipt->source_serial;
        AcquireSRWLockShared(&g_receipt_sources_lock);
        int closed = receipt->closed;
        ReleaseSRWLockShared(&g_receipt_sources_lock);
        int recovered = closed;
        /* A completed stream cannot append another source-consumer edge. */
        if (closed) for (ov_source_fault *f = &receipt->first_fault; f; f = f->next)
            if (!sh_resource_graph_native_source_fault_recovered(f->consumer, f->consumer_token)) { recovered = 0; break; }
        if (recovered) {
            int removed = 0;
            AcquireSRWLockExclusive(&g_receipt_sources_lock);
            ov_receipt_stream **link = &g_receipt_sources;
            while (*link && *link != receipt) link = &(*link)->source_next;
            if (*link) { *link = receipt->source_next; ++g_receipt_source_generation; removed = 1; }
            ReleaseSRWLockExclusive(&g_receipt_sources_lock);
            if (removed) ov_receipt_release(receipt);
        }
        ov_receipt_release(receipt);
    }
}
int sh_overrides_source_receipts_admit(void *const *resources, size_t count, char *error, size_t capacity)
{
    sh_overrides_collect_source_receipts();
    AcquireSRWLockShared(&g_receipt_sources_lock);
    uint64_t generation = g_receipt_source_generation;
    ReleaseSRWLockShared(&g_receipt_sources_lock);
    uint64_t serial = 0; int allowed = !count || resources != NULL;
    ov_receipt_stream *receipt;
    while (allowed && (receipt = ov_source_next(serial)) != NULL) {
        serial = receipt->source_serial;
        ov_source_fault *fault = &receipt->first_fault;
        while (fault) {
            AcquireSRWLockShared(&g_receipt_sources_lock);
            ov_source_fault value = *fault;
            ReleaseSRWLockShared(&g_receipt_sources_lock);
            for (size_t i = 0; i < count; ++i)
                if (resources[i] == value.input &&
                    !sh_resource_graph_native_source_fault_recovered(value.consumer, value.consumer_token)) {
                    allowed = 0; break;
                }
            if (!allowed) break;
            fault = value.next;
        }
        ov_receipt_release(receipt);
    }
    AcquireSRWLockShared(&g_receipt_sources_lock);
    if (generation != g_receipt_source_generation) allowed = 0;
    ReleaseSRWLockShared(&g_receipt_sources_lock);
    if (!allowed && error && capacity) snprintf(error, capacity, "native source consumer recovery pending");
    return allowed;
}
static void ov_receipt_quarantine(ov_receipt_stream *receipt, void *stream)
{
    receipt->failed_stream = stream;
    AcquireSRWLockExclusive(&g_receipt_failed_lock);
    receipt->failed_next = g_receipt_failed; g_receipt_failed = receipt;
    ReleaseSRWLockExclusive(&g_receipt_failed_lock);
    sh_package_runtime_source_cleanup_failed();
}
static void ov_receipt_vt_read(ov_receipt_stream *receipt)
{
    if (!ov_vt_recording()) return;
    if (!receipt->vt_origin_tried) {
        receipt->vt_origin_tried = 1;
        if (sh_package_runtime_resource_observation_allowed() &&
            (receipt->capture || receipt->baseline)) {
            sh_package_resource_capture *capture = sh_package_runtime_resource_capture_retain(receipt->capture);
            sh_resource_instance_origin *origin = sh_resource_instances_origin(capture, receipt->baseline);
            if (!origin) sh_package_runtime_resource_capture_release(capture);
            if (origin) {
                if (sh_resource_instances_origin_hold(origin)) receipt->vt_origin = origin;
                sh_resource_instances_origin_release(origin);
            }
        }
    }
    if (!ov_vt_observed(receipt->vt_origin)) receipt->vt_fault = 1;
}
static long long ov_receipt_read_body(ov_receipt_stream *receipt,void *self,void *body,uint64_t wanted)
{
    typedef long long (*read_fn)(void *, void *, uint64_t);
    typedef long long (*value_fn)(void *);
    long long offset = -1, total = -1;
    int observing = sh_package_shader_receipt_wants(receipt->path);
    if (observing) {
        __try {
            offset = ((value_fn)receipt->original[13])(self);
            total = ((value_fn)receipt->original[11])(self);
        } __except (EXCEPTION_EXECUTE_HANDLER) { offset = total = -1; }
    }
    long long got = ((read_fn)receipt->original[5])(self, body, wanted);
    DWORD native_error=GetLastError();
    /* A positive-sized request consumes the file's payload or its established
     * empty/EOF result. Open, timestamps, lengths and zero-sized reads alone
     * do not consume payload and must not add a package dependency. */
    int consumed = wanted && got >= 0 && (uint64_t)got <= wanted;
    if (consumed) sh_resource_graph_file(receipt->path);
    else if (wanted) sh_resource_graph_incomplete();
    if (consumed && receipt->family) {
        int observed = sh_resource_graph_native_source_observed(receipt->family, receipt->name,
            sh_package_runtime_resource_capture_retain(receipt->capture), receipt->baseline);
        if (observed < 0) {
            /* A matching native request consumed bytes but could not record
             * their origin. Preserve the actual opening lease, not merely a
             * capture whose lease has already transferred to this receipt. */
            ov_receipt_source_fault(receipt);
        }
    }
    if (consumed) ov_receipt_vt_read(receipt);
    if (observing) {
        __try {
            if (offset >= 0 && total > 0 && got >= 0 && (uint64_t)got <= wanted &&
                (uint64_t)total <= SIZE_MAX && (uint64_t)offset <= SIZE_MAX && (uint64_t)got <= SIZE_MAX)
                sh_package_shader_receipt_bytes(receipt->path, (size_t)total, (size_t)offset, body, (size_t)got);
            else sh_package_shader_receipt_bytes(receipt->path, 0, 0, NULL, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sh_package_shader_receipt_bytes(receipt->path, 0, 0, NULL, 0);
        }
    }
    SetLastError(native_error);return got;
}
static long long ov_receipt_read(void *self,void *body,uint64_t wanted)
{
    ov_receipt_stream *receipt=*(ov_receipt_stream **)self;
    /* Original Read may reenter the deleting callback. Keep this metadata and
     * its existing opening share alive without retaining native self. */
    InterlockedIncrement(&receipt->references);long long got=0;
    __try {got=ov_receipt_read_body(receipt,self,body,wanted);}
    __finally {DWORD error=GetLastError();ov_receipt_release(receipt);SetLastError(error);}
    return got;
}
static void *ov_receipt_destroy(void *self, unsigned flags)
{
    ov_receipt_stream *receipt = *(ov_receipt_stream **)self;
    typedef void *(*destroy_fn)(void *, unsigned);
    destroy_fn original = (destroy_fn)receipt->original[0];
    ov_receipt_unlink_active(receipt);
    *(void ***)self = receipt->original;
    void *result = NULL; int returned = 0;
    __try { result = original(self, flags); returned = 1; }
    __finally {
        if (returned && !receipt->source_unknown &&
            (!receipt->vt_fault || (!receipt->opening && !receipt->capture))) {
            AcquireSRWLockExclusive(&g_receipt_sources_lock);
            receipt->closed = 1;
            ReleaseSRWLockExclusive(&g_receipt_sources_lock);
            ov_receipt_release(receipt); /* Pending consumers retain their own reference. */
            sh_overrides_collect_source_receipts();
        } else {
            /* The native object may be partially destroyed or already freed.
             * Keep the opening fence without touching it or retrying cleanup.
             * This process cannot safely activate another provider afterward. */
            ov_receipt_quarantine(receipt, returned ? NULL : self);
            backend_log("native source stream cleanup interrupted; opening resources retained");
        }
    }
    return result;
}
static int ov_receipt_attach_full(void *stream, const char *path, const char *family, const char *name,
    sh_package_resource_capture *capture, int baseline)
{
    ov_receipt_stream *receipt = calloc(1, sizeof(*receipt));
    if (!receipt) return 0;
    receipt->references = 1;
    receipt->path = _strdup(path);
    if (family) { receipt->family = _strdup(family); receipt->name = _strdup(name); }
    if (!receipt->path || (family && (!receipt->family || !receipt->name))) {
        free(receipt->path); free(receipt->family); free(receipt->name); free(receipt); return 0;
    }
    receipt->capture = capture; receipt->baseline = baseline;
    receipt->opening = sh_package_runtime_resource_capture_take_opening_hold(capture);
    if (capture && !receipt->opening) {
        free(receipt->path); free(receipt->family); free(receipt->name); free(receipt); return 0;
    }
    int attached = 0;
    __try {
        receipt->original = *(void ***)stream;
        if (receipt->original && receipt->original[0] && receipt->original[5] &&
            receipt->original[11] && receipt->original[13] &&
            receipt->original[5] != (void *)ov_receipt_read) {
            memcpy(receipt->table, receipt->original, sizeof(receipt->table));
            receipt->table[0] = (void *)ov_receipt_destroy; receipt->table[5] = (void *)ov_receipt_read;
            *(void ***)stream = receipt->table; attached = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { attached = 0; }
    if(attached) {
        AcquireSRWLockExclusive(&g_receipt_active_lock);
        receipt->active_stream=stream;receipt->active_next=g_receipt_active;g_receipt_active=receipt;
        ReleaseSRWLockExclusive(&g_receipt_active_lock);
    }
    if (!attached) {
        /* A successful native open can still be consumed without our wrapper.
         * Retain its atomic opening fence when the adapter cannot be attached. */
        if (capture) { receipt->capture = NULL; ov_receipt_quarantine(receipt, stream); }
        else { free(receipt->path); free(receipt->family); free(receipt->name); free(receipt); }
    }
    return attached;
}
static void ov_receipt_attach(void *stream, const char *path)
{
    int graph = sh_resource_graph_recording();
    if (graph || sh_package_shader_receipt_wants(path)) {
        __try { if (stream && (*(void ***)stream)[5] == (void *)ov_receipt_read) return; }
        __except (EXCEPTION_EXECUTE_HANDLER) { if (graph) sh_resource_graph_incomplete(); return; }
        if (!ov_receipt_attach_full(stream, path, NULL, NULL, NULL, 0) && graph)
            sh_resource_graph_incomplete();
    }
}
static void ov_source_attach(void *stream, const char *path, sh_package_resource_capture *capture, int baseline)
{
    sh_overrides_collect_source_receipts();
    char family[128], name[4096];
    int target = ov_source_target(path, family, name), vt = ov_vt_recording();
    if (stream && (target || vt)) {
        if (ov_receipt_attach_full(stream, path, target ? family : NULL, target ? name : NULL, capture, baseline)) return;
        /* The source may be consumed without a witness. Retain no stale
         * baseline/capture association when a native read succeeds. */
        if (target) sh_resource_graph_native_source_observed(family, name, NULL, 0);
        if (vt) ov_vt_observed(NULL);
        /* If attachment failed before taking the hold, the capture still
         * owns it. Quarantine consumes this reference without allocation. */
        if (capture) sh_package_runtime_source_capture_failed(capture);
        return;
    }
    sh_package_runtime_resource_capture_release(capture);
}
#ifdef SH_OVERRIDES_TESTING
size_t sh_overrides_test_pending_source_count(void)
{
    size_t count = 0;
    AcquireSRWLockShared(&g_receipt_sources_lock);
    for (ov_receipt_stream *p = g_receipt_sources; p; p = p->source_next) count++;
    ReleaseSRWLockShared(&g_receipt_sources_lock);
    return count;
}
size_t sh_overrides_test_failed_source_count(void)
{
    size_t count = 0;
    AcquireSRWLockShared(&g_receipt_failed_lock);
    for (ov_receipt_stream *p = g_receipt_failed; p; p = p->failed_next) count++;
    ReleaseSRWLockShared(&g_receipt_failed_lock);
    return count;
}
void sh_overrides_test_receipt_attach(void *stream, const char *path)
{ ov_receipt_attach(stream, path); }
void sh_overrides_test_source_attach(void *stream, const char *path,
    sh_package_resource_capture *capture, int baseline)
{ ov_source_attach(stream, path, capture, baseline); }
#endif

/* A stream the resource-file route already claimed, handed to the engine's own
 * fill of a native memory file: the next open of exactly this name pointer on
 * this thread returns it instead of claiming the name again. */
static __declspec(thread) const char *g_pending_name;
static __declspec(thread) void *g_pending_stream;

/* Every resource the engine opens passes through here, so its cost is counted. */
static __declspec(noinline) void *ov_open_hook(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode)
{
    uintptr_t caller=(uintptr_t)_ReturnAddress();
    uint64_t stamp=0;
    sh_overrides_file_open_guard guard=(sh_overrides_file_open_guard)
        InterlockedCompareExchangePointer(&g_file_open_guard,NULL,NULL);
    if(guard){DWORD error=GetLastError();__try {stamp=guard(caller,self);}
        __except(EXCEPTION_EXECUTE_HANDLER){sh_resource_graph_native_observe_resource(NULL);}SetLastError(error);}
    SH_PERF_BEGIN(t0);
    void *s;
    if (g_pending_stream && name && name == g_pending_name && mode < 2) {
        s = g_pending_stream; g_pending_stream = NULL; g_pending_name = NULL;
    } else s = ov_open_body(self, name, b1, b2, mode);
    if (s && mode < 2 && name) {
        ov_receipt_attach(s, name);
    }
    SH_PERF_END(SH_PERF_OVERRIDE_OPEN, t0);
    sh_overrides_file_return_observer observer=(sh_overrides_file_return_observer)
        InterlockedCompareExchangePointer(&g_file_return_observer,NULL,NULL);
    if(observer) {
        DWORD error=GetLastError();
        __try {observer(s,caller,self,stamp);}
        __except(EXCEPTION_EXECUTE_HANDLER){sh_resource_graph_native_observe_resource(NULL);}
        SetLastError(error);
    }
    return s;
}
int sh_overrides_set_file_return_observer(sh_overrides_file_open_guard guard,sh_overrides_file_return_observer observer)
{
    if(!observer || !guard)return 0;int ok=0;
    AcquireSRWLockExclusive(&g_file_observer_lock);
    void *prior=InterlockedCompareExchangePointer(&g_file_return_observer,NULL,NULL);
    if(!prior){InterlockedExchangePointer(&g_file_open_guard,(void *)guard);InterlockedExchangePointer(&g_file_return_observer,(void *)observer);ok=1;}
    else ok=prior==(void *)observer && (void *)guard==InterlockedCompareExchangePointer(&g_file_open_guard,NULL,NULL);
    ReleaseSRWLockExclusive(&g_file_observer_lock);return ok;
}
int sh_overrides_file_return_observer_ready(sh_overrides_file_open_guard guard,sh_overrides_file_return_observer observer,
    const void *provider_table,const void *native_open)
{
    int ready=0;DWORD error=GetLastError();
    __try {
        ready=guard && observer && (void *)guard==InterlockedCompareExchangePointer(&g_file_open_guard,NULL,NULL) &&
            (void *)observer==InterlockedCompareExchangePointer(&g_file_return_observer,NULL,NULL) &&
            provider_table && native_open && g_orig_open==(open_fn_t)native_open &&
            g_slot==(void **)((const unsigned char *)provider_table+OPEN_SLOT_OFFSET) && *g_slot==(void *)ov_open_hook;
    }__except(EXCEPTION_EXECUTE_HANDLER){ready=0;}
    SetLastError(error);return ready;
}
int sh_overrides_file_dispatch_snapshot(void *stream,sh_overrides_file_dispatch *out)
{
    if(!out)return 0;memset(out,0,sizeof(*out));if(!stream)return 0;
    DWORD error=GetLastError();int ok=0;const unsigned slots[]={0,3,5,11,19};
    AcquireSRWLockShared(&g_receipt_active_lock);
    __try {
        void **table=*(void ***)stream,**original=table;int receipt=0;
        for(ov_receipt_stream *r=g_receipt_active;r;r=r->active_next)if(table==r->table && r->active_stream==stream) {
            if(table[0]!=(void *)ov_receipt_destroy || table[5]!=(void *)ov_receipt_read || !r->original || r->references<=0)__leave;
            for(unsigned i=1;i<31;i++)if(i!=5 && table[i]!=r->original[i])__leave;
            original=r->original;receipt=1;out->shader=sh_package_shader_receipt_wants(r->path);break;
        }
        /* Our callbacks on an unregistered/counterfeit/stale table do not
         * establish allocation lineage, even if the addresses were copied. */
        if(!original || (!receipt && (table[0]==(void *)ov_receipt_destroy || table[5]==(void *)ov_receipt_read)))__leave;
        out->table=(uintptr_t)original;out->receipt=receipt;out->product=original==g_stream_vtable;
        out->read_string=(uintptr_t)original[28];out->tell=(uintptr_t)original[13];
        for(unsigned i=0;i<5;i++){out->slots[i]=(uintptr_t)original[slots[i]];if(!out->slots[i])__leave;}
        if(out->product) {
            void *expected[]={ov_dtor,ov_name,ov_read,ov_length_byseek,ov_ret0_e};
            for(unsigned i=0;i<5;i++)if(out->slots[i]!=(uintptr_t)expected[i])__leave;
            if(out->tell!=(uintptr_t)ov_tell || original[14]!=(void *)ov_seek)__leave;
        }
        ok=1;
    }__except(EXCEPTION_EXECUTE_HANDLER){ok=0;}
    ReleaseSRWLockShared(&g_receipt_active_lock);if(!ok)memset(out,0,sizeof(*out));SetLastError(error);return ok;
}

int sh_overrides_claim_open(const char *name, void **stream)
{
    if (stream) *stream = NULL;
    if (!stream || !name || !*name) return 0;
#ifndef SH_OVERRIDES_TESTING
    if (g_orig_open == NULL) return 0;   /* only while the shadow serves ordinary opens */
#endif
    return ov_claim(name, stream);
}
void sh_overrides_pending_open(const char *name, void *stream)
{
    g_pending_name = name; g_pending_stream = stream;
}
void *sh_overrides_pending_take(void)
{
    void *stream = g_pending_stream;
    g_pending_stream = NULL; g_pending_name = NULL;
    return stream;
}
void sh_overrides_stream_close(void *stream)
{
    if (stream) ((void (*)(void *, int))(*(void ***)stream)[0])(stream, 1);
}

#ifdef SH_OVERRIDES_TESTING
void *sh_overrides_test_open(const char *name, unsigned int mode)
{
    return ov_open_hook(NULL, name, 0xff, 0xff, mode);
}
#endif

/* Decode the first LEA RAX,[rip+disp32] in the resolved provider constructor
 * to find its vtable. Installation then requires the target to lie in a read-
 * only host section; pinned RVAs are diagnostic only.
 */
#define LEA_SCAN_WINDOW 0x40

static int safe_read_n(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void *decode_vtable_global(const uint8_t *ctor_fn)
{
    uint8_t b[LEA_SCAN_WINDOW];
    if (!safe_read_n(ctor_fn, b, sizeof b)) return NULL;
    for (int i = 0; i + 7 <= LEA_SCAN_WINDOW; i++) {
        if (b[i] == 0x48 && b[i + 1] == 0x8D && b[i + 2] == 0x05) {     /* LEA RAX,[rip+disp32] */
            int32_t disp;
            memcpy(&disp, &b[i + 3], 4);
            const uint8_t *rip_next = ctor_fn + i + 7;
            return (void *)(rip_next + disp);
        }
    }
    return NULL;
}

/* SEH-guarded host-image bounds checks. Invalid or unreadable PE headers
 * refuse installation.
 */

/* SizeOfImage from the host's optional header. 0/failure => 0. */
static int ov_image_size(const uint8_t *module_base, uint32_t *out_size)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS64 *nt;
        if (!module_base || !out_size) return 0;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
        *out_size = nt->OptionalHeader.SizeOfImage;
        return *out_size != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Check host-image containment; this does not identify the function. */
static int ov_address_in_image(const uint8_t *module_base, const void *address)
{
    uintptr_t base = (uintptr_t)module_base;
    uintptr_t value = (uintptr_t)address;
    uint32_t size = 0;
    if (!module_base || !address || value < base) return 0;
    if (!ov_image_size(module_base, &size)) return 0;
    return value - base < (uintptr_t)size;
}

/* Require READ with neither WRITE nor EXECUTE on the containing section. This
 * checks whether a decoded vtable address is plausible, not its identity.
 */
static int ov_address_in_readonly_section(const uint8_t *module_base, const void *address)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        const IMAGE_NT_HEADERS64 *nt;
        const IMAGE_SECTION_HEADER *sec;
        uintptr_t rva;
        unsigned int i;
        if (!module_base || !address || (uintptr_t)address < (uintptr_t)module_base) return 0;
        rva = (uintptr_t)address - (uintptr_t)module_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        sec = (const IMAGE_SECTION_HEADER *)IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            uintptr_t start = (uintptr_t)sec->VirtualAddress;
            uintptr_t span  = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            if (rva < start || rva >= start + span) continue;
            return (sec->Characteristics & IMAGE_SCN_MEM_READ) != 0 &&
                   (sec->Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) == 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Require clean SIG_OK matches for the constructor and all three native
 * helpers, plus host-image containment. Hook-tolerant fallbacks are refused
 * because the constructor is decoded and helpers become callable vtable
 * entries.
 */
static int ov_supported_build_abi(const uint8_t *module_base,
                                  const void *ctor_fn, int ctor_status_ok,
                                  const void *read_string_fn, int read_string_status_ok,
                                  const void *compare_fn, int compare_status_ok,
                                  const void *write_string_fn, int write_string_status_ok)
{
    return ctor_status_ok == 1 && read_string_status_ok == 1 &&
           compare_status_ok == 1 && write_string_status_ok == 1 &&
           ov_address_in_image(module_base, ctor_fn) &&
           ov_address_in_image(module_base, read_string_fn) &&
           ov_address_in_image(module_base, compare_fn) &&
           ov_address_in_image(module_base, write_string_fn);
}

#ifdef SH_OVERRIDES_TESTING
int sh_overrides_test_supported_build_abi(const uint8_t *module_base,
                                          const void *ctor, int ctor_status_ok,
                                          const void *read_string, int read_string_status_ok,
                                          const void *compare, int compare_status_ok,
                                          const void *write_string, int write_string_status_ok)
{
    return ov_supported_build_abi(module_base, ctor, ctor_status_ok,
                                  read_string, read_string_status_ok,
                                  compare, compare_status_ok,
                                  write_string, write_string_status_ok);
}

int sh_overrides_test_address_in_readonly_section(const uint8_t *module_base, const void *address)
{
    return ov_address_in_readonly_section(module_base, address);
}
#endif

/* ============================================================ the install (slot swap) ==============*/

int sh_overrides_install(const uint8_t *module_base,
                         void *ctor_fn, int ctor_status_ok,
                         void *read_string_fn, int read_string_status_ok,
                         void *compare_fn, int compare_status_ok,
                         void *write_string_fn, int write_string_status_ok)
{
    char line[MAX_PATH + 128];

    if (!g_root[0]) default_root(g_root, sizeof g_root);

    if (ctor_fn == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- ResProviderCtor not resolved");
        return 0;
    }
    if (!ctor_status_ok) {
        /* A hooked constructor may obscure the vtable LEA; refuse fallback matches. */
        backend_log("B1: overrides file-shadow SKIPPED -- ResProviderCtor via hook-tolerant fallback "
                    "(prologue may be hooked); not decoding the vtable LEA from a detoured prologue");
        return 0;
    }
    if (!read_string_fn || !compare_fn || !write_string_fn ||
        read_string_status_ok != 1 || compare_status_ok != 1 ||
        write_string_status_ok != 1) {
        ov_stream_helpers_install(read_string_fn, read_string_status_ok,
                                  compare_fn, compare_status_ok,
                                  write_string_fn, write_string_status_ok);
        backend_log("B1: overrides file-shadow SKIPPED -- idFile native idStr helpers did not all resolve cleanly");
        return 0;
    }
    if (!ov_supported_build_abi(module_base, ctor_fn, ctor_status_ok,
                                read_string_fn, read_string_status_ok,
                                compare_fn, compare_status_ok,
                                write_string_fn, write_string_status_ok)) {
        backend_log("B1: overrides file-shadow SKIPPED -- resolved engine locations are not clean unique "
                    "signature matches inside the host image");
        return 0;
    }
    if (g_orig_open != NULL) {
        backend_log("B1: overrides file-shadow already installed");
        return 1;
    }

    if (!ov_stream_helpers_install(read_string_fn, read_string_status_ok,
                                   compare_fn, compare_status_ok,
                                   write_string_fn, write_string_status_ok)) {
        backend_log("B1: overrides file-shadow SKIPPED -- idFile helper table publication refused");
        return 0;
    }

    void *vtable = decode_vtable_global((const uint8_t *)ctor_fn);
    if (vtable == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- could not decode the vtable LEA "
                    "(ResProviderCtor layout shifted?)");
        return 0;
    }
    /* Require the decoded vtable to lie in a read-only host section. The
     * pinned Vulkan RVA is for audit only.
     */
    if (!ov_address_in_image(module_base, vtable) ||
        !ov_address_in_readonly_section(module_base, vtable)) {
        backend_log("B1: overrides file-shadow SKIPPED -- decoded provider vtable is not a read-only "
                    "location inside the host image (the LEA decode did not land on a vtable)");
        return 0;
    }

    void **slot = (void **)((uint8_t *)vtable + OPEN_SLOT_OFFSET);

    /* Temporarily make the read-only vtable page writable, replace the eight-
     * byte open slot, then restore protection.
     */
    void *orig = NULL;
    if (!safe_read_n((const uint8_t *)slot, (uint8_t *)&orig, sizeof orig) || orig == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- open vtable slot unreadable / null");
        return 0;
    }

    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        backend_log("B1: overrides file-shadow FAIL -- VirtualProtect(slot) failed");
        return 0;
    }
    g_orig_open = (open_fn_t)orig;
    *slot = (void *)ov_open_hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    g_slot = slot;

    /* Initial package compilation belongs to the main-thread pre-promotion
     * boundary. Arming this provider must not wait for native reader setup. */
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: overrides file-shadow installed (vtable=%p slot+0x%x=%p, orig open=%p); root=%s\\overrides; "
        "built-in defaults: %u from memory",
        vtable, OPEN_SLOT_OFFSET, (void *)slot, orig, g_root,
        (unsigned)(sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]));
    backend_log(line);
    return 1;
}

int sh_overrides_set_root(const char *path)
{
    InterlockedIncrement(&g_ov_pkg_generation);
    if (path == NULL || path[0] == '\0') {
        default_root(g_root, sizeof g_root);
        return 1;
    }
    strncpy_s(g_root, sizeof g_root, path, _TRUNCATE);
    return g_root[0] != '\0';
}

unsigned long sh_overrides_shadow_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_shadow_count, 0, 0);
}

int sh_overrides_uninstall(void)
{
    if (g_slot == NULL || g_orig_open == NULL) return 0;
    DWORD old;
    if (VirtualProtect(g_slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *g_slot = (void *)g_orig_open;
        VirtualProtect(g_slot, sizeof(void *), old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_slot, sizeof(void *));
    }
    backend_log("B1: overrides file-shadow uninstalled (vtable slot restored)");
    g_slot = NULL;
    g_orig_open = NULL;
    return 1;
}

static int ov_internal_decl_table_publish(const sh_overrides_internal_decl_entry *entries, size_t count, int provider_ready, int user_enabled)
{
    int result;
    AcquireSRWLockExclusive(&g_internal_lock);
    __try { result = ov_internal_decl_table_publish_locked(entries, count, provider_ready, user_enabled); }
    __finally { ReleaseSRWLockExclusive(&g_internal_lock); }
    return result;
}

static ov_stream *open_internal_decl_captured(const char *path, int *matched,
    const char *family, const char *name, sh_package_resource_capture **capture)
{
    int registered;
    AcquireSRWLockShared(&g_internal_lock);
    __try { registered = ov_internal_decl_registered_locked(path); }
    __finally { ReleaseSRWLockShared(&g_internal_lock); }
    if (matched) *matched = registered;
    if (!registered) return NULL;
    /* Never wait for provider publication while holding its table's lock.
     * Registration only owns this alias; the atomic provider read below
     * chooses its current exact bytes and opening lifetime. */
    size_t length = 0;
    unsigned char *current = NULL;
    int claimed = sh_package_runtime_read_decl_alias_captured(path, &current, &length, family, name, capture);
    if (claimed != 1) { free(current); return NULL; }
    unsigned char *body = HeapAlloc(GetProcessHeap(), 0, length ? length : 1u);
    if (!body) { free(current); return NULL; }
    if (length) memcpy(body, current, length);
    free(current);
    ov_stream *stream = make_mem_stream(body, (long long)length, path, 1);
    if (!stream) HeapFree(GetProcessHeap(), 0, body);
    return stream;
}
static ov_stream *open_internal_decl(const char *name, int *matched)
{ return open_internal_decl_captured(name, matched, NULL, NULL, NULL); }

size_t sh_overrides_internal_decl_published_count(void)
{
    size_t result;
    AcquireSRWLockShared(&g_internal_lock);
    __try { result = sh_overrides_internal_decl_published_count_locked(); }
    __finally { ReleaseSRWLockShared(&g_internal_lock); }
    return result;
}

int sh_overrides_internal_decl_published(const char *name)
{
    int result;
    AcquireSRWLockShared(&g_internal_lock);
    __try { result = sh_overrides_internal_decl_published_locked(name); }
    __finally { ReleaseSRWLockShared(&g_internal_lock); }
    return result;
}

int sh_overrides_internal_decl_table_can_install(void)
{
    int result;
    AcquireSRWLockShared(&g_internal_lock);
    __try { result = sh_overrides_internal_decl_table_can_install_locked(); }
    __finally { ReleaseSRWLockShared(&g_internal_lock); }
    return result;
}

static int ov_internal_decl_table_merge(const sh_overrides_internal_decl_entry *entries, size_t count, int provider_ready)
{
    int result;
    AcquireSRWLockExclusive(&g_internal_lock);
    __try { result = ov_internal_decl_table_merge_locked(entries, count, provider_ready); }
    __finally { ReleaseSRWLockExclusive(&g_internal_lock); }
    return result;
}

int sh_overrides_internal_decl_table_merge(const sh_overrides_internal_decl_entry *entries, size_t count)
{
    return ov_internal_decl_table_merge(entries, count, g_orig_open != NULL);
}

#ifdef SH_OVERRIDES_TESTING
int sh_overrides_test_internal_decl_table_merge(const sh_overrides_internal_decl_entry *entries, size_t count)
{
    return ov_internal_decl_table_merge(entries, count, 1);
}
#endif
