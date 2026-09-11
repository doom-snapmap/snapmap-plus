/* Resource-provider open hook and native idFile streams. See overrides.h for
 * precedence and ownership. Built-in names receive a brace/quote check before
 * a user file replaces them; other disk resources pass through unchanged.
 *
 * Installation reclaims files matching old built-in defaults, ignoring CR
 * bytes, and logs active user overrides. Each shadow path has an SEH
 * boundary; faults fall back to the original resource open where that path
 * permits it.
 */
#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "overrides.h"
#include "backend_log.h"
#include "perf.h"
#include "decl_text.h"
#include "packages.h"
#include "package_conflicts.h"
#include "resource_bridge.h"
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
static volatile LONG g_shadow_count = 0;
typedef struct ov_internal_decl {
    char *name;                   /* exact lower-case decltree/<type>/<name>.decl */
    unsigned char *body;          /* owned by the published snapshot */
    size_t body_length;
} ov_internal_decl;

static SRWLOCK g_internal_lock = SRWLOCK_INIT;
static ov_internal_decl *g_internal_decls;
static size_t g_internal_decl_count;
static volatile LONG g_internal_decl_table_state;

enum {
    OV_INTERNAL_DECL_TABLE_NEW = 0,
    OV_INTERNAL_DECL_TABLE_INSTALLING = 1,
    OV_INTERNAL_DECL_TABLE_READY = 2,
    OV_INTERNAL_DECL_TABLE_FAILED = 3
};

#define OV_INTERNAL_DECL_MAX_ENTRIES 512u

/* The overrides ROOT (holds overrides\ + overrides\shader_includes\). Default %LOCALAPPDATA%\snapmap-plus. */
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

static int ov_internal_decl_table_publish(const sh_overrides_internal_decl_entry *entries, size_t count, int provider_ready, int user_enabled);
static ov_stream * open_internal_decl(const char *name, int *matched);
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
        count > OV_INTERNAL_DECL_MAX_ENTRIES ||
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
        for (j = 0; j < i; j++) {
            if (strcmp(copy[i].name, copy[j].name) == 0) {
                ov_internal_decl_table_free(copy, count);
                InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_FAILED);
                return 0;
            }
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
    g_internal_decls = copy;
    g_internal_decl_count = count;
    /* Publish only complete keys and bodies; open streams own separate copies. */
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_READY);
    return 1;
}

static ov_stream *open_internal_decl_locked(const char *name, int *matched)
{
    size_t i;
    if (matched) *matched = 0;
    if (!name || !sh_user_overrides_enabled_for_launch() ||
        InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) !=
            OV_INTERNAL_DECL_TABLE_READY) return NULL;
    for (i = 0; i < g_internal_decl_count; i++) {
        if (strcmp(name, g_internal_decls[i].name) != 0) continue;
        if (matched) *matched = 1;
        {
            ov_stream *stream;
            size_t length = g_internal_decls[i].body_length;
            unsigned char *body = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, length);
            if (!body) return NULL;
            memcpy(body, g_internal_decls[i].body, length);
            stream = make_mem_stream(body, (long long)length, g_internal_decls[i].name, 1);
            if (!stream) HeapFree(GetProcessHeap(), 0, body);
            return stream;
        }
    }
    return NULL;
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
    if (!name || !sh_user_overrides_enabled_for_launch() ||
        InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) !=
            OV_INTERNAL_DECL_TABLE_READY) return 0;
    for (i = 0; i < g_internal_decl_count; i++)
        if (_stricmp(name, g_internal_decls[i].name) == 0) return 1;
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
    size_t total, i, j, at = 0;
    char line[192];

    if (!provider_ready || !sh_user_overrides_enabled_for_launch()) return 0;
    if (!entries || count == 0 || count > OV_INTERNAL_DECL_MAX_ENTRIES) return 0;
    if (count > SIZE_MAX - old_count) return 0;
    total = old_count + count;
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
        for (j = 0; j < at; j++) {
            if (strcmp(merged[at].name, merged[j].name) == 0) {
                ov_internal_decl_table_free(merged, total);
                return 0;                      /* duplicate within the new set */
            }
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

    /* Copy unchanged identities into the replacement snapshot. */
    for (i = 0; i < old_count; i++) {
        int superseded = 0;
        if (!old[i].name || !old[i].body) continue;
        for (j = 0; j < count; j++) {
            if (strcmp(old[i].name, merged[j].name) == 0) { superseded = 1; break; }
        }
        if (superseded) continue;
        if (at >= OV_INTERNAL_DECL_MAX_ENTRIES) {
            ov_internal_decl_table_free(merged, total);
            return 0;
        }
        merged[at].name = (char *)HeapAlloc(GetProcessHeap(), 0, strlen(old[i].name) + 1);
        merged[at].body = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, old[i].body_length);
        if (!merged[at].name || !merged[at].body) {
            ov_internal_decl_table_free(merged, total);
            return 0;
        }
        strcpy_s(merged[at].name, strlen(old[i].name) + 1, old[i].name);
        memcpy(merged[at].body, old[i].body, old[i].body_length);
        merged[at].body_length = old[i].body_length;
        at++;
    }

    g_internal_decls = merged;
    g_internal_decl_count = at;
    ov_internal_decl_table_free(old, old_count);
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_READY);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: overrides internal decl table MERGED -- %u new + %u carried forward = %u "
                "published (previous snapshot released)",
                (unsigned)count, (unsigned)(at - count), (unsigned)at);
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
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_NEW);
}

int sh_overrides_test_internal_decl_table_install(
    const sh_overrides_internal_decl_entry *entries, size_t count)
{
    return ov_internal_decl_table_publish(entries, count, 1,
                                          sh_user_overrides_enabled_for_launch());
}

void *sh_overrides_test_internal_decl_open(const char *name)
{
    int matched = 0;
    return open_internal_decl(name, &matched);
}

long long sh_overrides_test_stream_read(void *stream, void *buffer, uint64_t length)
{
    return ov_read((ov_stream *)stream, buffer, length);
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
    ov_dtor((ov_stream *)stream);
}
#endif

int sh_overrides_get_root(char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    resolve_root(out, cap);
    return out[0] != '\0';
}

/* Build a disk override path. The first terminal ".inc" match selects
 * shader_includes; other resources use the shared override tree.
 */
static int build_override_path(const char *name, char *out, size_t cap)
{
    if (!name || !name[0]) return 0;
    char root[MAX_PATH];
    resolve_root(root, sizeof root);

    /* Only the first ".inc" occurrence counts, and it must end the name. */
    const char *match = strstr(name, ".inc");
    int is_shader_include = (match != NULL && match[4] == '\0');

    const char *rel = name;
    if (is_shader_include) {
        /* Strip the first component unless the name starts with "includes". */
        const char *slash = strchr(name, '/');
        if (slash != NULL && strstr(name, "includes") != name)
            rel = slash + 1;
    }

    if (is_shader_include)
        _snprintf_s(out, cap, _TRUNCATE, "%s\\overrides\\shader_includes\\%s", root, rel);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%s\\overrides\\%s", root, name);

    /* Normalize separators. */
    for (char *p = out; *p; ++p)
        if (*p == '/') *p = '\\';
    return 1;
}

/* ============================================================ package resolution ==================*/

/* Map supported engine namespaces to package subdirectories. Decl and image
 * prefixes are stripped; shader paths retain their full generated namespace.
 * Restricting this table prevents unrelated package files from becoming
 * engine resources.
 */
typedef struct ov_namespace {
    const char *engine_prefix;   /* what the engine asks for */
    const char *package_subdir;  /* which package subdirectory may answer */
    int strip_prefix;            /* 1: path is <subdir>\<rest>; 0: <subdir>\<whole name> */
} ov_namespace;

static const ov_namespace g_ov_namespaces[] = {
    { "generated/decls/",       "decls",   1 },
    { "generated/spirv/",       "shaders", 0 },
    { "generated/renderprogs/", "shaders", 0 },
    { "generated/image/",       "images",  1 },
};

/* Cache package enumeration outside the resource-open path. Capture at
 * installation and refresh during runtime rearm.
 */
/* Readers hold the shared lock through path selection. A rescan exclusively
 * owns both buffers until it publishes a complete inventory. */
static SRWLOCK g_ov_packages_lock = SRWLOCK_INIT;
static sh_package g_ov_packages_buf[2][SH_PACKAGES_MAX];
static size_t g_ov_package_counts[2];
static volatile LONG g_ov_pkg_active;   /* 0 or 1 */
/* Report overlapping files once per process. */
static volatile LONG g_ov_conflicts_reported;
static volatile LONG g_ov_pkg_generation;

static int ov_capture_packages(void)
{
    char root[MAX_PATH];
    size_t count = 0;
    LONG active, target;
    int complete;

    resolve_root(root, sizeof root);
    AcquireSRWLockExclusive(&g_ov_packages_lock);
    active = g_ov_pkg_active;
    target = active ^ 1;
    complete = root[0] && sh_packages_enumerate(root, g_ov_packages_buf[target],
                                               SH_PACKAGES_MAX, &count);
    if (!complete) count = 0;
    g_ov_package_counts[target] = count;
    InterlockedExchange(&g_ov_pkg_active, target);   /* publish: one atomic store */
    InterlockedIncrement(&g_ov_pkg_generation);
    ReleaseSRWLockExclusive(&g_ov_packages_lock);

    /* Log overlapping package files so the selected precedence is visible. */
    if (count > 1 && !InterlockedCompareExchange(&g_ov_conflicts_reported, 1, 0))
        (void)sh_pkg_conflicts_report(root);
    if (!complete) backend_log("B1: overrides package inventory REFUSED -- enumeration incomplete");
    return complete;
}

/* Refresh package paths used by opens. New decl identities still require
 * decl_server rearm. Returns the visible package count.
 */
unsigned long sh_overrides_rescan_packages(void)
{
    char line[160];
    LONG active;
    if (!ov_capture_packages()) return SH_OVERRIDES_RESCAN_FAILED;
    AcquireSRWLockShared(&g_ov_packages_lock);
    active = InterlockedCompareExchange(&g_ov_pkg_active, 0, 0);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: overrides package list RE-SCANNED -- %u package(s) now visible (generation %ld)",
                (unsigned)g_ov_package_counts[active],
                (long)InterlockedCompareExchange(&g_ov_pkg_generation, 0, 0));
    {
        unsigned long count = (unsigned long)g_ov_package_counts[active];
        ReleaseSRWLockShared(&g_ov_packages_lock);
        backend_log(line);
        return count;
    }
}

static int ov_is_regular_file(const char *path)
{
    SH_PERF_BEGIN(t0);
    DWORD attrs = GetFileAttributesA(path);
    SH_PERF_END(SH_PERF_OVERRIDE_STAT, t0);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Names with no override file, for the package inventory that answered.
 *
 * The engine asks for the same resources over and over, and a missing-file
 * check costs about 40us here, so the miss is the answer worth keeping. Only
 * misses are kept: a hit goes on to open the file anyway, and the test entry
 * points expect a fresh answer for a name that resolves.
 *
 * A file dropped in beside a name already asked for is invisible until the
 * package list is re-scanned, which is the same contract packages have.
 */
#define OV_MISS_SLOTS     4096u
#define OV_MISS_NAME_CAP  160

typedef struct ov_miss_entry {
    unsigned hash;                    /* 0 = empty slot */
    LONG generation;
    char     name[OV_MISS_NAME_CAP];
} ov_miss_entry;

static ov_miss_entry g_ov_miss[OV_MISS_SLOTS];
static SRWLOCK       g_ov_miss_lock = SRWLOCK_INIT;

static unsigned ov_miss_hash(const char *name)
{
    unsigned h = 2166136261u;
    for (; *name; name++) { h ^= (unsigned char)*name; h *= 16777619u; }
    return h ? h : 1u;   /* 0 marks an empty slot */
}

static int ov_miss_known(const char *name, unsigned hash, LONG generation)
{
    const ov_miss_entry *e = &g_ov_miss[hash % OV_MISS_SLOTS];
    int known;
    AcquireSRWLockShared(&g_ov_miss_lock);
    known = e->hash == hash && e->generation == generation &&
            strcmp(e->name, name) == 0;
    ReleaseSRWLockShared(&g_ov_miss_lock);
    return known;
}

static void ov_miss_remember(const char *name, unsigned hash, LONG generation)
{
    ov_miss_entry *e = &g_ov_miss[hash % OV_MISS_SLOTS];
    if (strlen(name) >= OV_MISS_NAME_CAP) return;   /* too long to hold exactly */
    AcquireSRWLockExclusive(&g_ov_miss_lock);
    strcpy_s(e->name, sizeof e->name, name);
    e->hash = hash;
    e->generation = generation;
    ReleaseSRWLockExclusive(&g_ov_miss_lock);
}

/* Find a resource in the shared override tree, then installed packages
 * ordered by descending priority and case-insensitive name. Return 0 if no
 * file exists.
 */
static int ov_resolve_existing(const char *name, char *out, size_t cap)
{
    size_t i, n;
    unsigned hash;
    LONG generation;

    if (!name || !name[0] || !out || cap == 0) return 0;
    generation = InterlockedCompareExchange(&g_ov_pkg_generation, 0, 0);
    hash = ov_miss_hash(name);
    if (ov_miss_known(name, hash, generation)) { out[0] = '\0'; return 0; }
    if (!build_override_path(name, out, cap)) return 0;
    if (ov_is_regular_file(out)) return 1;

    for (n = 0; n < sizeof(g_ov_namespaces) / sizeof(g_ov_namespaces[0]); n++) {
        const ov_namespace *ns = &g_ov_namespaces[n];
        size_t prefix_length = strlen(ns->engine_prefix);
        const char *relative;

        if (_strnicmp(name, ns->engine_prefix, prefix_length) != 0) continue;
        relative = ns->strip_prefix ? name + prefix_length : name;
        if (!relative[0]) break;

        AcquireSRWLockShared(&g_ov_packages_lock);
        LONG act = InterlockedCompareExchange(&g_ov_pkg_active, 0, 0);
        size_t pkg_count = g_ov_package_counts[act];
        for (i = 0; i < pkg_count; i++) {
            char *p;
            if (_snprintf_s(out, cap, _TRUNCATE, "%s\\%s\\%s",
                            g_ov_packages_buf[act][i].root, ns->package_subdir, relative) < 0)
                continue;
            for (p = out; *p; ++p)
                if (*p == '/') *p = '\\';
            if (ov_is_regular_file(out)) {
                ReleaseSRWLockShared(&g_ov_packages_lock);
                return 1;
            }
        }
        ReleaseSRWLockShared(&g_ov_packages_lock);
        break;                      /* prefixes are disjoint; one match is all */
    }
    out[0] = '\0';
    /* A concurrent rescan cannot turn this old miss into a new one.
     * Small caller buffers also cannot poison ordinary opens. */
    if (cap >= MAX_PATH) ov_miss_remember(name, hash, generation);
    return 0;
}

#ifdef SH_OVERRIDES_TESTING
int sh_overrides_test_resolve_existing(const char *name, char *out, size_t cap)
{
    ov_capture_packages();
    return ov_resolve_existing(name, out, cap);
}

int sh_overrides_test_resolve_cached(const char *name, char *out, size_t cap)
{
    return ov_resolve_existing(name, out, cap);
}
#endif

/* SEH-guarded open of the override file; returns an ov_stream* (caller returns it to the engine) or
 * NULL if no file / open failed. On success the FILE* is owned by the stream (its dtor fcloses). */
static ov_stream *try_open_override(const char *name)
{
    char path[MAX_PATH];
    if (!ov_resolve_existing(name, path, sizeof path)) return NULL;

    /* exists? (cheap negative for the common no-override case before fopen) */
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return NULL;

    FILE *fp = NULL;
    if (fopen_s(&fp, path, "rb") != 0 || fp == NULL) return NULL;

    /* Measure with seek-end/tell, then restore the cursor. */
    long long length;
    if (_fseeki64(fp, 0, SEEK_END) != 0 || (length = _ftelli64(fp)) < 0 ||
        _fseeki64(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    ov_stream *s = make_stream(fp, length, name);
    if (!s) { fclose(fp); return NULL; }
    return s;
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

/* Read a whole file into a heap buffer (cap 8 MiB -- decls are KB-scale; a bigger file is served
 * unvalidated as a plain stream rather than slurped). NULL on absent/oversize/failure. */
#define OV_SLURP_CAP (8u * 1024u * 1024u)
static unsigned char *read_all_file(const char *path, long long *out_len)
{
    FILE *fp = NULL;
    if (fopen_s(&fp, path, "rb") != 0 || fp == NULL) return NULL;
    long long len = 0;
    if (_fseeki64(fp, 0, SEEK_END) == 0) { len = _ftelli64(fp); _fseeki64(fp, 0, SEEK_SET); }
    if (len < 0 || len > (long long)OV_SLURP_CAP) { fclose(fp); return NULL; }
    unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if ((long long)got != len) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    buf[len] = 0;
    *out_len = len;
    return buf;
}

/* Validate user replacements for built-in names before returning an owned
 * memory stream. Invalid text falls back to the built-in. Oversize files or
 * allocation failures instead try a plain file stream.
 */
static ov_stream *open_user_for_baked_name(const char *name, int *malformed)
{
    *malformed = 0;
    char path[MAX_PATH];
    if (!ov_resolve_existing(name, path, sizeof path)) return NULL;
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return NULL;

    long long len = 0;
    unsigned char *buf = read_all_file(path, &len);
    if (!buf) return try_open_override(name);            /* unusual size/alloc -> plain file stream */

    if (!sh_decl_text_well_formed(buf, (size_t)len)) {
        HeapFree(GetProcessHeap(), 0, buf);
        *malformed = 1;
        return NULL;
    }
    ov_stream *s = make_mem_stream(buf, len, name, 1);
    if (!s) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    return s;
}

/* Capture the provider object from the first hook call so original-resource
 * reads can reuse its native self pointer.
 */
static void *volatile g_provider_self = NULL;

unsigned char *sh_overrides_read_engine_resource(const char *name, size_t *out_len)
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

static void *ov_open_body(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode)
{
    if (g_orig_open == NULL) return NULL;   /* defensive: never happens once installed */
    if (g_provider_self == NULL) g_provider_self = self;

    /* Map-carried navigation precedes disk overrides and is independent of
     * the launch user-layer gate.
     */
    if (mode < 2 && name != NULL) {
        unsigned char *nav = NULL;
        size_t nav_len = 0;
        /* Embedded navigation takes precedence over dynamically generated output. */
        if (!sh_navmesh_open(name, &nav, &nav_len))
            sh_nav_bake_open(name, sh_overrides_read_engine_resource, &nav, &nav_len);
        if (nav) {
            ov_stream *s = make_mem_stream(nav, (long long)nav_len, name, 1);
            if (s) return s;
            HeapFree(GetProcessHeap(), 0, nav);
        }
    }

    if (mode < 2) {
        ov_stream *internal = NULL;
        int internal_match = 0;
        SH_PERF_BEGIN(t0);
        __try {
            internal = open_internal_decl(name, &internal_match);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            internal = NULL;
        }
        SH_PERF_END(SH_PERF_INTERNAL_DECL, t0);
        if (internal_match) {
            /* A published identity is authoritative. Do not let an
             * allocation/read failure fall through to a physical file or the
             * packaged resource under the same canonical name. */
            if (!internal) return NULL;
            unsigned long n = (unsigned long)InterlockedIncrement(&g_shadow_count);
            char line[MAX_PATH + 160];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B1: overrides internal decl FIRED for '%s' (%lld bytes) [#%lu]",
                        name, internal->length, n);
            backend_log(line);
            return internal;
        }
    }

    if (mode < 2 && name != NULL) {
        ov_stream *s = NULL;
        const char *src = NULL;
        int bridge_error = 0;
        __try {
            const ov_baked_decl_t *baked = find_baked(name);
            int user_on = sh_user_overrides_enabled_for_launch();
            if (user_on) {
                if (baked) {
                    int malformed = 0;
                    s = open_user_for_baked_name(name, &malformed);
                    if (s) src = "user";
                    else if (malformed) src = "built-in (user file malformed, refused)";
                } else {
                    s = try_open_override(name);
                    if (s) src = "user";
                }
                if (s == NULL) {
                    unsigned char *linked = NULL;
                    size_t linked_length = 0;
                    const char *linked_source = NULL;
                    int linked_status;
                    SH_PERF_BEGIN(tb);
                    linked_status = sh_resource_bridge_open(name, &linked, &linked_length,
                                                            &linked_source);
                    SH_PERF_END(SH_PERF_BRIDGE_OPEN, tb);
                    if (linked_status == SH_RESOURCE_BRIDGE_OPENED &&
                        linked_length <= (size_t)INT64_MAX) {
                        s = make_mem_stream(linked, (long long)linked_length, name, 1);
                        if (s) src = "installed resource bridge";
                        else {
                            HeapFree(GetProcessHeap(), 0, linked);
                            bridge_error = 1;
                        }
                    } else if (linked_status == SH_RESOURCE_BRIDGE_OPENED) {
                        HeapFree(GetProcessHeap(), 0, linked);
                        bridge_error = 1;
                    } else if (linked_status == SH_RESOURCE_BRIDGE_ERROR) {
                        bridge_error = 1;
                    }
                    (void)linked_source;
                }
            }
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
            return s;   /* the engine reads the override bytes through our idFile vtable */
        }
        if (bridge_error) {
            char line[MAX_PATH + 128];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "B1: installed resource bridge REFUSED engine fallback for admitted '%s'",
                        name);
            backend_log(line);
            return NULL;
        }
    }
    /* No shadow, or mode guard: chain with the original byte arguments. */
    {
        void *chained;
        SH_PERF_BEGIN(t0);
        chained = g_orig_open(self, name, (unsigned char)(b1 & 0xff), (unsigned char)(b2 & 0xff), mode);
        SH_PERF_END(SH_PERF_ENGINE_OPEN, t0);
        return chained;
    }
}

/* Every resource the engine opens passes through here, so its cost is counted. */
static void *ov_open_hook(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode)
{
    SH_PERF_BEGIN(t0);
    void *s = ov_open_body(self, name, b1, b2, mode);
    SH_PERF_END(SH_PERF_OVERRIDE_OPEN, t0);
    return s;
}

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

/* Remove legacy disk copies that match built-in defaults with CR bytes
 * ignored, allowing current in-memory defaults to serve. Keep every differing
 * file. Reclaim faults leave the existing disk shadow in place.
 */
static int file_equals_baked_ignoring_cr(const unsigned char *fbuf, size_t flen,
                                         const char *baked, size_t blen)
{
    size_t fi = 0, bi = 0;
    while (fi < flen && (char)fbuf[fi] == '\r') fi++;    /* the baked text is LF-only */
    while (fi < flen && bi < blen) {
        if ((char)fbuf[fi] == '\r') { fi++; continue; }
        if ((char)fbuf[fi] != baked[bi]) return 0;
        fi++; bi++;
        while (fi < flen && (char)fbuf[fi] == '\r') fi++;
    }
    return fi == flen && bi == blen;
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

static int ov_suffix_ci(const char *value, const char *suffix)
{
    size_t value_length, suffix_length;
    if (!value || !suffix) return 0;
    value_length = strlen(value);
    suffix_length = strlen(suffix);
    return value_length >= suffix_length &&
           _stricmp(value + value_length - suffix_length, suffix) == 0;
}

static void reclaim_baked_overrides(void)
{
    for (size_t i = 0; i < sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]; i++) {
        __try {
            char path[MAX_PATH];
            if (!build_override_path(g_ov_baked_decls[i].name, path, sizeof path)) continue;
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;   /* nothing on disk */
            long long len = 0;
            unsigned char *buf = read_all_file(path, &len);
            if (!buf) continue;
            int ours = file_equals_baked_ignoring_cr(buf, (size_t)len,
                                                     g_ov_baked_decls[i].text, g_ov_baked_decls[i].len);
            HeapFree(GetProcessHeap(), 0, buf);
            char msg[MAX_PATH + 96];
            if (ours && DeleteFileA(path)) {
                _snprintf_s(msg, sizeof msg, _TRUNCATE,
                            "B1: reclaimed previously-written default '%s' (built-in serves from memory now)",
                            g_ov_baked_decls[i].name);
                backend_log(msg);
            } else if (!ours) {
                _snprintf_s(msg, sizeof msg, _TRUNCATE,
                            "B1: user-owned override kept at built-in name '%s' (it wins over the built-in)",
                            g_ov_baked_decls[i].name);
                backend_log(msg);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* leave the file; old behavior */ }
    }
}

/* Log a bounded list of active user files and flag failed text-balance
 * checks.
 */
#define OV_AUDIT_MAX_FILES 512
#define OV_AUDIT_MAX_NAMED 24
#define OV_AUDIT_MAX_DEPTH 8
static void audit_walk(const char *dir, const char *rel, int depth, int *count, int *named, int *warned)
{
    if (depth > OV_AUDIT_MAX_DEPTH || *count >= OV_AUDIT_MAX_FILES) return;
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char sub[MAX_PATH], subrel[MAX_PATH];
        _snprintf_s(sub,    sizeof sub,    _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        _snprintf_s(subrel, sizeof subrel, _TRUNCATE, "%s%s%s", rel, rel[0] ? "/" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            audit_walk(sub, subrel, depth + 1, count, named, warned);
        } else {
            (*count)++;
            int bad = 0;
            if (ov_suffix_ci(subrel, ".decl")) {
                long long len = 0;
                unsigned char *buf = read_all_file(sub, &len);
                if (buf) {
                    bad = !sh_decl_text_well_formed(buf, (size_t)len);
                    HeapFree(GetProcessHeap(), 0, buf);
                }
            }
            if (bad) (*warned)++;
            if (*named < OV_AUDIT_MAX_NAMED || bad) {
                char msg[MAX_PATH + 96];
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "B1:   override %s'%s'",
                            bad ? "STRUCTURALLY-SUSPECT (unbalanced braces/quotes) " : "", subrel);
                backend_log(msg);
                (*named)++;
            }
        }
        if (*count >= OV_AUDIT_MAX_FILES) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void audit_user_overrides(void)
{
    __try {
        char root[MAX_PATH], dir[MAX_PATH];
        resolve_root(root, sizeof root);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\overrides", root);
        int count = 0, named = 0, warned = 0;
        audit_walk(dir, "", 0, &count, &named, &warned);
        char msg[MAX_PATH + 192];
        if (!sh_user_overrides_enabled_for_launch()) {
            _snprintf_s(msg, sizeof msg, _TRUNCATE,
                        "B1: overrides audit -- user layer DISABLED by config; "
                        "%d file(s) under %s are ignored (set sh_user_overrides 1 "
                        "and restart DOOM to re-enable)",
                        count, dir);
        } else {
            _snprintf_s(msg, sizeof msg, _TRUNCATE,
                        "B1: overrides audit -- %d user override file(s) active "
                        "under %s%s%s (bisect: set sh_user_overrides 0 and "
                        "restart DOOM)",
                        count, dir, warned ? ", " : "",
                        warned ? "with structural warnings above" : "");
        }
        backend_log(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("B1: overrides audit skipped (fault)"); }
}

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

    if (sh_user_overrides_enabled_for_launch() && !sh_resource_bridge_capture(g_root)) {
        backend_log("B1: overrides file-shadow SKIPPED -- installed resource manifest snapshot failed closed");
        return 0;
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

    sh_resource_bridge_set_provider_ready(1);
    ov_capture_packages();       /* Capture the initial package list for resource opens. */
    reclaim_baked_overrides();   /* Reclaim matching legacy copies so in-memory defaults can serve. */
    audit_user_overrides();      /* log what the user's folder actively shadows */
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
    sh_resource_bridge_set_provider_ready(0);
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

static ov_stream * open_internal_decl(const char *name, int *matched)
{
    ov_stream * result;
    AcquireSRWLockShared(&g_internal_lock);
    __try { result = open_internal_decl_locked(name, matched); }
    __finally { ReleaseSRWLockShared(&g_internal_lock); }
    return result;
}

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
