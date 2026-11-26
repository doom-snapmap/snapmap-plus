/* Resource-provider open hook and native idFile streams, which own copies of the bytes they serve. See
 * overrides.h for the resource ordering; each provider path has an SEH boundary. */
#include "image_view.h"
#include "rip_slot.h"
#include "../common/text.h"
#include "safe_read.h"
#include <windows.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "overrides.h"
#include "backend_log.h"
#include "perf.h"
#include "decl_text.h"
#include "grid_room_asset.h"
#include "navmesh.h"                /* baked AI navigation, served under the module's own names */
#include "nav_bake.h"           /* navigation baked from the map's own marked volumes */
#include "overrides_baked.h"        /* the built-in "*Custom"-tab default decls (Timeline tile, Unknown, Lift, Blocking) */
#include "engine_build.h"
#include "modserve.h"             /* installed mod bundles, after every product source */
#include "direct_actor.h"

#define OPEN_SLOT_OFFSET SH_OVERRIDES_OPEN_SLOT

/* Base idFile implementation of slot +0x50. */
#define OV_BASE_HELPER_RVA_VK           0x2997D0u
#define OV_BASE_HELPER_RVA_GL           0x299A30u

/* Engine ABI: idFile* open(self, name, uint8 b1, uint8 b2, uint mode). Mode >= 2 bypasses shadowing; both byte
 * arguments are passed through when chaining. */
typedef void *(*open_fn_t)(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode);

static open_fn_t  g_orig_open  = NULL;   /* the saved engine resource-open (the slot's original value) */
static void     **g_slot       = NULL;   /* the live vtable slot we patched (for uninstall) */
static volatile LONG g_shadow_count = 0;
/* Native idFile stream head (+0x00 vtable, +0x08 FILE*, +0x10 name, +0x18 length, +0x20 short flag), released
 * with HeapFree. fp selects disk backing; otherwise buf is read-only memory read through pos. */
typedef struct ov_stream {
    void        *vtable;     /* +0x00 */
    FILE        *fp;         /* +0x08; NULL when memory-backed */
    const char  *name;       /* +0x10; points at the name appended after the struct */
    long long    length;     /* +0x18 */
    short        flag16;     /* +0x20 (OG sets 1) */
    const unsigned char *buf;/* memory backing: static text or an owned heap copy */
    long long    pos;        /* memory-backing read cursor */
    int          owns_buf;   /* 1: the destructor frees buf */
} ov_stream;

/* Native virtual-method implementations; self arrives in RCX. Slot positions are in g_stream_vtable. */

static long long ov_zero(ov_stream *s) { (void)s; return 0; }
static long long ov_one(ov_stream *s)  { (void)s; return 1; }
static void  ov_dtor(ov_stream *s)                                   /* close + free(this) */
{
    if (s) {
        if (s->fp) { fclose(s->fp); s->fp = NULL; }
        if (s->buf && s->owns_buf) HeapFree(GetProcessHeap(), 0, (void *)s->buf);
        s->buf = NULL; s->length = 0; s->name = NULL; s->flag16 = 0;
        HeapFree(GetProcessHeap(), 0, s);
    }
}
static const char *ov_name(ov_stream *s)        { return s ? s->name : NULL; }           /* *(this+0x10) */
static long long ov_read(ov_stream *s, void *buf, uint64_t n)          /* fread(buf,1,n,fp) */
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
static long long ov_write(ov_stream *s, const void *buf, uint64_t n)       /* fwrite; memory form is read-only */
{
    if (!s || !s->fp || !buf) return 0;
    if (n > (uint64_t)SIZE_MAX || n > (uint64_t)INT64_MAX) return 0;
    return (long long)fwrite(buf, 1, (size_t)n, s->fp);
}
static int       ov_seek(ov_stream *s, long long off, int origin);       /* forward declaration */
/* Matches the native read-at (Vulkan RVA 0x1a1b520): Seek with ABS=2, then Read. */
static long long ov_seekread(ov_stream *s, long long off, void *buf, uint64_t n)
{
    if (!s || ov_seek(s, off, 2) != 0) return 0;
    return ov_read(s, buf, n);
}
/* The native helper at RVA 0x1a1c220 calls Seek(this,off,ABS=2), then Write(this,buf,len). */
static long long ov_seekwrite(ov_stream *s, long long off, const void *buf, uint64_t n)
{
    if (!s || ov_seek(s, off, 2) != 0) return 0;
    return ov_write(s, buf, n);
}
static int       ov_lock(ov_stream *s)          { if (s && s->fp) _lock_file(s->fp);   return 1; }
static int       ov_unlock(ov_stream *s)        { if (s && s->fp) _unlock_file(s->fp); return 1; }
static long long ov_length_byseek(ov_stream *s)                      /* stored length */
{
    return s && s->length >= 0 ? s->length : 0;
}
/* +0x70 SetLength always fails: provider streams are read-only. */
static int       ov_set_length(ov_stream *s, long long requested)
{
    (void)s;
    (void)requested;
    return 0;
}
static long long ov_tell(ov_stream *s)                                                                /* ftell */
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

static int       ov_seek(ov_stream *s, long long off, int origin)    /* idFile: 0=CUR, 1=END, 2=ABS */
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
static long long ov_vprintf(ov_stream *s, const char *fmt, va_list ap)   /* vfprintf */
{
    if (!s || !s->fp || !fmt) return 0;
    return (long long)vfprintf(s->fp, fmt, ap);
}
/* Two slots share the native C-varargs printf shape and this FILE-backed thunk; memory streams return zero. */
static long long ov_printf_thunk(ov_stream *s, const char *fmt, ...)     /* varargs entry */
{
    va_list ap; long long r;
    va_start(ap, fmt);
    r = ov_vprintf(s, fmt, ap);
    va_end(ap);
    return r;
}
/* Read-only streams report no writable/physical provider flag. */
static char      ov_provider_flag(ov_stream *s) { (void)s; return 0; }
static void      ov_flush_a(ov_stream *s)                                                /* flush + refresh size */
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
static void      ov_flush_b(ov_stream *s)       { if (s && s->fp) fflush(s->fp); }        /* fflush */
/* +0x18 returns false and +0xF0 zero in the engine's base idFile. */
static unsigned char ov_new_false(ov_stream *s)  { (void)s; return 0; }   /* +0x18 */
static long long ov_drive_type(ov_stream *s)   { (void)s; return 0; }   /* drive/storage type */

/* The build 13954591 idFile vtable has 34 entries; the final three (native idStr helpers) and the borrowed
 * base helper +0x50 must resolve before the provider hook is published. The assert fails the build if the
 * named slots move. */
typedef char ov_slot_layout_check[(SH_IDFILE_SLOT_CLOSE == 0 && SH_IDFILE_SLOT_TRUE_FLAG == 2 &&
                                   SH_IDFILE_SLOT_READ == 6 && SH_IDFILE_SLOT_LENGTH == 13 &&
                                   SH_IDFILE_SLOT_SET_LENGTH == 14) ? 1 : -1];
#define OV_SLOT_BASE_HELPER   10   /* +0x50 */
#define OV_SLOT_READ_STRING   31   /* +0xf8 */
#define OV_SLOT_COMPARE       32   /* +0x100 */
#define OV_SLOT_WRITE_STRING  33   /* +0x108 */
static void *g_stream_vtable[SH_OVERRIDES_STREAM_SLOTS] = {
    [SH_IDFILE_SLOT_CLOSE] = (void *)ov_dtor, /* 0  +0x00 close/dtor */
    (void *)ov_zero,        /* 1  +0x08 */
    [SH_IDFILE_SLOT_TRUE_FLAG] = (void *)ov_one, /* 2  +0x10 native constant true */
    (void *)ov_new_false,     /* 3  +0x18 base returns false */
    /* +0x20 GetName: idLexer copies this pointer into an idStr, so it must return the name pointer. */
    (void *)ov_name,          /* 4  +0x20 GetName -> name char* */
    (void *)ov_name,          /* 5  +0x28 GetFullPath -> name char* */
    [SH_IDFILE_SLOT_READ] = (void *)ov_read, /* 6  +0x30 Read */
    (void *)ov_write,         /* 7  +0x38 Write */
    (void *)ov_seekread,      /* 8  +0x40 */
    (void *)ov_seekwrite,     /* 9  +0x48 Seek(ABS) + Write */
    NULL,                     /* 10 +0x50 the engine's base helper, published at install */
    (void *)ov_lock,          /* 11 +0x58 Lock */
    (void *)ov_unlock,        /* 12 +0x60 Unlock */
    [SH_IDFILE_SLOT_LENGTH] = (void *)ov_length_byseek, /* 13 +0x68 GetLength */
    [SH_IDFILE_SLOT_SET_LENGTH] = (void *)ov_set_length, /* 14 +0x70 SetLength -> read-only failure */
    (void *)ov_tell,          /* 15 +0x78 Tell */
    (void *)ov_seek,          /* 16 +0x80 Seek */
    (void *)ov_printf_thunk,  /* 17 +0x88 vfprintf */
    (void *)ov_printf_thunk,  /* 18 +0x90 vfprintf */
    (void *)ov_zero,        /* 19 +0x98 */
    (void *)ov_zero,        /* 20 +0xa0 */
    (void *)ov_zero,        /* 21 +0xa8 */
    (void *)ov_provider_flag, /* 22 +0xb0 */
    (void *)ov_flush_a,       /* 23 +0xb8 Flush */
    (void *)ov_flush_b,       /* 24 +0xc0 Flush */
    (void *)ov_one,          /* 25 +0xc8 */
    (void *)ov_drive_type,    /* 26 +0xd0 drive/storage type = 0 */
    (void *)ov_one,  /* 27 +0xd8 native bool true */
    (void *)ov_zero,/* 28 +0xe0 = 0 */
    (void *)ov_zero,/* 29 +0xe8 = 0 */
    (void *)ov_zero,      /* 30 +0xf0 base returns 0 */
    NULL,                     /* 31 +0xf8 native ReadString helper, published after clean SIG_OK */
    NULL,                     /* 32 +0x100 native Compare helper, published after clean SIG_OK */
    NULL,                     /* 33 +0x108 native WriteString helper, published after clean SIG_OK */
};

enum {
    OV_STREAM_HELPERS_NEW = 0,
    OV_STREAM_HELPERS_INSTALLING = 1,
    OV_STREAM_HELPERS_READY = 2,
    OV_STREAM_HELPERS_FAILED = 3
};
static volatile LONG g_stream_helpers_state = OV_STREAM_HELPERS_NEW;

/* Configures all three native idStr slots before the provider hook is published; missing or hooked helpers
 * leave a terminal refusal state. */
static int ov_stream_helpers_install(void *read_string,
                                     void *compare,
                                     void *write_string)
{
    LONG state;
    if (!read_string || !compare || !write_string) {
        if (InterlockedCompareExchange(&g_stream_helpers_state,
                                       OV_STREAM_HELPERS_FAILED,
                                       OV_STREAM_HELPERS_NEW) == OV_STREAM_HELPERS_NEW)
            return 0;
        state = InterlockedCompareExchange(&g_stream_helpers_state,
                                           OV_STREAM_HELPERS_NEW,
                                           OV_STREAM_HELPERS_NEW);
        return state == OV_STREAM_HELPERS_READY &&
               g_stream_vtable[OV_SLOT_READ_STRING] == read_string &&
               g_stream_vtable[OV_SLOT_COMPARE] == compare &&
               g_stream_vtable[OV_SLOT_WRITE_STRING] == write_string;
    }
    state = InterlockedCompareExchange(&g_stream_helpers_state,
                                       OV_STREAM_HELPERS_INSTALLING,
                                       OV_STREAM_HELPERS_NEW);
    if (state != OV_STREAM_HELPERS_NEW) {
        return state == OV_STREAM_HELPERS_READY &&
               g_stream_vtable[OV_SLOT_READ_STRING] == read_string &&
               g_stream_vtable[OV_SLOT_COMPARE] == compare &&
               g_stream_vtable[OV_SLOT_WRITE_STRING] == write_string;
    }

    /* READY is published only after all helper pointers are set; InterlockedExchange is the memory barrier. */
    g_stream_vtable[OV_SLOT_READ_STRING] = read_string;
    g_stream_vtable[OV_SLOT_COMPARE] = compare;
    g_stream_vtable[OV_SLOT_WRITE_STRING] = write_string;
    InterlockedExchange(&g_stream_helpers_state, OV_STREAM_HELPERS_READY);
    return 1;
}

/* Constructs a stream over an open FILE* of known length, with a copy of `name` after the object so the Name
 * slot is stable. NULL on allocation failure (the caller fcloses). */
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

/* Constructs a memory-backed stream over `buf`/`length`; owns_buf=1 hands the heap buffer to the stream's
 * destructor, 0 leaves it (static baked text). NULL on allocation failure. */
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

#ifdef SH_OVERRIDES_TESTING
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

size_t sh_overrides_test_stream_vtable_slots(void)
{
    return sizeof g_stream_vtable / sizeof g_stream_vtable[0];
}

void *sh_overrides_test_stream_vtable_slot(size_t index)
{
    return index < sizeof g_stream_vtable / sizeof g_stream_vtable[0] ?
           g_stream_vtable[index] : NULL;
}

int sh_overrides_test_stream_helpers_configure(void *read_string,
                                                void *compare,
                                                void *write_string)
{
    return ov_stream_helpers_install(read_string, compare,
                                     write_string);
}

int sh_overrides_test_stream_helpers_ready(void)
{
    return InterlockedCompareExchange(&g_stream_helpers_state,
                                      OV_STREAM_HELPERS_NEW,
                                      OV_STREAM_HELPERS_NEW) == OV_STREAM_HELPERS_READY;
}

void sh_overrides_test_stream_helpers_reset(void)
{
    g_stream_vtable[OV_SLOT_READ_STRING] = NULL;
    g_stream_vtable[OV_SLOT_COMPARE] = NULL;
    g_stream_vtable[OV_SLOT_WRITE_STRING] = NULL;
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

#endif

/* Decls DOOM never shipped. A runtime registration loads them from the loose source tree
 * (decltree/<type>/<name>.decl) rather than generated/decls/, so these also answer that spelling. */
static const char *const g_ov_product_decls[] = { "snapeditorentitydef/custom/" };

#define OV_DEFAULT_COUNT (sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0])
static volatile LONG g_default_settled[OV_DEFAULT_COUNT];

int sh_overrides_repair_defaults(int (*visit)(void *, const char *), void *context, int *reads)
{
    int offered = 0;
    for (size_t i = 0; i < OV_DEFAULT_COUNT; i++) {
        const char *request = g_ov_baked_decls[i].name;
        if (InterlockedCompareExchange(&g_default_settled[i], 0, 0) || sh_modserve_lookup(request, NULL)) continue;
        offered++;
        if (visit(context, request)) {
            /* The provider marks a successful read during the visit. A failed parse is also tried only once. */
            if (InterlockedExchange(&g_default_settled[i], 1)) (*reads)++;
        }
    }
    return offered;
}

/* The baked default for `name`, or NULL if `name` is not a built-in. */
static const ov_baked_decl_t *find_baked(const char *name)
{
    const char *tree;
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]; i++)
        if (sh_path_eq(name, g_ov_baked_decls[i].name)) return &g_ov_baked_decls[i];
    tree = sh_path_after_prefix(name, "decltree/");
    if (!tree) return NULL;
    for (size_t i = 0; i < sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]; i++) {
        const char *rel = sh_path_after_prefix(g_ov_baked_decls[i].name, "generated/decls/");
        for (size_t k = 0; rel && k < sizeof g_ov_product_decls / sizeof g_ov_product_decls[0]; k++)
            if (sh_path_after_prefix(rel, g_ov_product_decls[k]) && sh_path_eq(tree, rel)) return &g_ov_baked_decls[i];
    }
    return NULL;
}

/* The provider object captured from the first hook call, so original-resource reads can reuse its self pointer. */
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
        /* Mode 2 is the hook's no-shadow guard, so this cannot re-enter the hook. */
        f = g_orig_open(g_provider_self, name, 0xff, 0xff, 2);
        if (!f) return NULL;
        vt = *(void ***)f;
        if (!vt) return NULL;

        /* Slot indices follow the 34-slot layout g_stream_vtable implements. */
        len = ((ov_len_fn)vt[SH_IDFILE_SLOT_LENGTH])(f);
        if (len <= 0 || (unsigned long long)len > SH_SMNAV_MAX_PAYLOAD) {
            ((ov_close_fn)vt[SH_IDFILE_SLOT_CLOSE])(f);
            return NULL;
        }
        buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)len);
        if (!buf) {
            ((ov_close_fn)vt[SH_IDFILE_SLOT_CLOSE])(f);
            return NULL;
        }
        if (((ov_read_fn)vt[SH_IDFILE_SLOT_READ])(f, buf, (uint64_t)len) != len) {
            HeapFree(GetProcessHeap(), 0, buf);
            ((ov_close_fn)vt[SH_IDFILE_SLOT_CLOSE])(f);
            return NULL;
        }
        ((ov_close_fn)vt[SH_IDFILE_SLOT_CLOSE])(f);      /* close + free */
        if (out_len) *out_len = (size_t)len;
        return buf;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* A fault in a native stream call frees the output buffer and reports failure. */
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        if (out_len) *out_len = 0;
        return NULL;
    }
}

#ifdef SH_OVERRIDES_TESTING
unsigned char *sh_overrides_test_read_installed(void *open_fn, void *provider,
                                                const char *name, size_t *out_len)
{
    open_fn_t saved_open = g_orig_open;
    void *saved_provider = g_provider_self;
    unsigned char *result;
    g_orig_open = (open_fn_t)open_fn;
    g_provider_self = provider;
    result = read_installed_resource(name, out_len);
    g_orig_open = saved_open;
    g_provider_self = saved_provider;
    return result;
}
#endif

/* Inputs to product transforms: a built-in default, otherwise the installed original. It stays below
 * Grid/navigation so derived reads cannot recurse into themselves. */
static unsigned char *read_effective_resource(const char *name, size_t *out_len)
{
    unsigned char *copy;
    const ov_baked_decl_t *baked;
    if (out_len) *out_len = 0;
    if (!name || !out_len) return NULL;
    baked = find_baked(name);
    if (!baked) return read_installed_resource(name, out_len);
    copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, baked->len ? baked->len : 1u);
    if (copy) {
        if (baked->len) memcpy(copy, baked->text, baked->len);
        *out_len = baked->len;
    }
    return copy;
}

static unsigned char *grid_read_effective(void *context,const char *name,size_t *length)
{
    (void)context;
    return read_effective_resource(name,length);
}
static void grid_release_effective(void *context,void *bytes)
{
    (void)context;HeapFree(GetProcessHeap(),0,bytes);
}

/* The engine path a resource name denotes: lower case, '/' separators, and no empty, absolute, device or
 * Windows-reserved component. Malloc-owned; NULL for a name that cannot be one. */
static char *ov_engine_path(const char *path)
{
    char *out, *part, *p;
    size_t length;
    if (!path || !(length = strlen(path)) || length >= 1024 || path[0] == '/' || path[0] == '\\') return NULL;
    out = (char *)malloc(length + 1);
    if (!out) return NULL;
    memcpy(out, path, length + 1);
    for (p = out; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') *p = '/';
        else if (c >= 'A' && c <= 'Z') *p += 'a' - 'A';
        else if (c < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' ||
                 c == '<' || c == '>' || c == '|') goto bad;
    }
    part = out;
    for (p = out; ; p++) if (*p == '/' || !*p) {
        size_t n = (size_t)(p - part), stem = 0;
        if (!n || part[n - 1] == '.' || part[n - 1] == ' ') goto bad;
        while (stem < n && part[stem] != '.') stem++;
        if ((stem == 3 && (!memcmp(part, "con", 3) || !memcmp(part, "prn", 3) ||
                          !memcmp(part, "aux", 3) || !memcmp(part, "nul", 3))) ||
            (stem == 4 && (!memcmp(part, "com", 3) || !memcmp(part, "lpt", 3)) &&
             part[3] >= '1' && part[3] <= '9')) goto bad;
        if (!*p) break;
        part = p + 1;
    }
    return out;
bad:
    free(out); return NULL;
}

static int grid_aas_name(const char *name)
{return name&&(strstr(name,".aas_")||strstr(name,".baas_"));}
static int grid_open_validated(const char *name,unsigned char **out,size_t *length)
{
    char *canonical = NULL, *normalized = ov_engine_path(name);
    int identity = normalized ? sh_grid_asset_canonical(normalized, &canonical) : 0;
    int claimed = identity < 0 ? -1 : 0;
    free(normalized);
    free(canonical);
    if (!claimed) claimed=sh_grid_asset_open(name,grid_read_effective,grid_release_effective,NULL,out,length);
    if(claimed>0&&grid_aas_name(name)){
        char error[192];
        if(!sh_navmesh_validate_aas(*out,*length,error,sizeof error)){
            backend_log(error);free(*out);*out=NULL;*length=0;claimed=-1;
        }
    }
    return claimed;
}

unsigned char *sh_overrides_read_engine_resource(const char *name,size_t *out_len)
{
    unsigned char *generated=NULL,*copy=NULL;size_t length=0;int claimed;
    if(out_len)*out_len=0;
    if(!name)return NULL;
    /* Navigation consumes the resized Grid result where applicable, otherwise the compiled base; neither path
     * re-enters map navigation generation. */
    claimed=grid_open_validated(name,&generated,&length);
    if(!claimed)return read_effective_resource(name,out_len);
    if(claimed>0){copy=(unsigned char*)HeapAlloc(GetProcessHeap(),0,length);
        if(copy){memcpy(copy,generated,length);if(out_len)*out_len=length;}}
    free(generated);return copy;
}

static void *ov_open_body(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode)
{
#ifndef SH_OVERRIDES_TESTING
    if (g_orig_open == NULL) return NULL;   /* defensive: cannot happen once installed */
#endif
    if (g_provider_self == NULL) g_provider_self = self;

    /* The editor's placement grid, cut where marked boxes cut the module floor; only the native surface rebuild
     * asks for these private names. */
    if (mode < 2 && name && (!strncmp(name, "maps/smpnavgrid/", 16) ||
                              !strncmp(name, "generated/maps/smpnavgrid/", 26))) {
        unsigned char *grid = NULL;
        size_t grid_len = 0;
        if (sh_nav_bake_overlay_open(name, &grid, &grid_len) == 1) {
            ov_stream *stream = make_mem_stream(grid, (long long)grid_len, name, 1);
            if (stream) return stream;
            HeapFree(GetProcessHeap(), 0, grid);
        }
        return NULL;
    }

    /* Embedded or marked-volume navigation takes precedence over the resized room's base AAS, which its source
     * reader still resolves. */
    if(mode<2&&grid_aas_name(name)){
        unsigned char *nav=NULL;size_t length=0;
        if(!sh_navmesh_open(name,&nav,&length))
            sh_nav_bake_open(name,sh_overrides_read_engine_resource,&nav,&length);
        if(nav){void *stream=make_mem_stream(nav,(long long)length,name,1);
            if(stream)return stream;HeapFree(GetProcessHeap(),0,nav);return NULL;}
    }

    if(mode<2&&name){
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
            /* A malformed or unsupported private resource never resolves to another room's stock payload. */
            return stream;
        }
    }

    /* Map-carried navigation precedes disk overrides and is independent of the launch user-layer gate. */
    if (mode < 2 && name != NULL && !grid_aas_name(name)) {
        unsigned char *nav = NULL;
        size_t nav_len = 0;
        /* Embedded navigation takes precedence over generated output. */
        if (!sh_navmesh_open(name, &nav, &nav_len))
            sh_nav_bake_open(name, sh_overrides_read_engine_resource, &nav, &nav_len);
        if (nav) {
            ov_stream *s = make_mem_stream(nav, (long long)nav_len, name, 1);
            if (s) return s;
            HeapFree(GetProcessHeap(), 0, nav);
        }
    }

    /* Installed mods rank below navigation and the Grid Room and above the built-in defaults and the engine, so
     * a mod's version of a built-in name is the one the editor reads. A file that cannot be read falls through. */
    if (mode < 2 && name != NULL && (sh_modserve_active() || sh_direct_actor_key(name))) {
        unsigned char *bytes = NULL;
        size_t length = 0;
        if (sh_modserve_open(name, &bytes, &length) > 0) {
            ov_stream *s = make_mem_stream(bytes, (long long)length, name, 1);
            if (s) return s;
            HeapFree(GetProcessHeap(), 0, bytes);
        } else {
            /* A file a map's own part of a mod left out is noted, never a silent hole. */
            (void)sh_modserve_note_miss(name);
        }
    }
    if (mode < 2 && name != NULL) {
        ov_stream *s = NULL;
        const char *src = NULL;
        __try {
            const ov_baked_decl_t *baked = find_baked(name);
            if (s == NULL && baked != NULL) {
                s = make_mem_stream((const unsigned char *)baked->text, (long long)baked->len, name, 0);
                if (s) {
                    InterlockedExchange(&g_default_settled[baked - g_ov_baked_decls], 1);
                    src = "built-in";
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s = NULL;   /* a fault in the shadow path falls through to the original open */
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
    }

    /* No shadow, or mode guard: chain with the original byte arguments. */
    {
        void *chained;
        SH_PERF_BEGIN(t0);
        chained = g_orig_open ? g_orig_open(self, name, (unsigned char)(b1 & 0xff), (unsigned char)(b2 & 0xff), mode) : NULL;
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

#ifdef SH_OVERRIDES_TESTING
void *sh_overrides_test_open(const char *name, unsigned int mode)
{
    return ov_open_hook(NULL, name, 0xff, 0xff, mode);
}
#endif

/* Checks host-image containment; it does not identify the function. */
static int ov_address_in_image(const uint8_t *module_base, const void *address)
{
    uintptr_t base = (uintptr_t)module_base;
    uintptr_t value = (uintptr_t)address;
    return address && value >= base && value - base < sh_pe_image_size(module_base);
}

/* Requires READ with neither WRITE nor EXECUTE on the containing section, which makes a decoded vtable address
 * plausible without identifying it. */
static int ov_address_in_readonly_section(const uint8_t *module_base, const void *address)
{
    __try {
        const IMAGE_NT_HEADERS64 *nt = sh_pe_headers(module_base);
        const IMAGE_SECTION_HEADER *sec;
        uintptr_t rva;
        unsigned int i;
        if (!module_base || !address || (uintptr_t)address < (uintptr_t)module_base) return 0;
        rva = (uintptr_t)address - (uintptr_t)module_base;
        if (!nt) return 0;
        sec = (const IMAGE_SECTION_HEADER *)IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            uintptr_t start = (uintptr_t)sec->VirtualAddress;
            uintptr_t span  = sh_pe_section_span(sec);
            if (rva < start || rva >= start + span) continue;
            return (sec->Characteristics & IMAGE_SCN_MEM_READ) != 0 &&
                   (sec->Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) == 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Only use resolved helpers and a constructor inside this host image. */
static int ov_supported_build_abi(const uint8_t *module_base,
                                  const void *ctor_fn,
                                  const void *read_string_fn,
                                  const void *compare_fn,
                                  const void *write_string_fn)
{
    return ov_address_in_image(module_base, ctor_fn) &&
           ov_address_in_image(module_base, read_string_fn) &&
           ov_address_in_image(module_base, compare_fn) &&
           ov_address_in_image(module_base, write_string_fn);
}

#ifdef SH_OVERRIDES_TESTING
int sh_overrides_test_supported_build_abi(const uint8_t *module_base,
                                          const void *ctor,
                                          const void *read_string,
                                          const void *compare,
                                          const void *write_string)
{
    return ov_supported_build_abi(module_base, ctor,
                                  read_string,
                                  compare,
                                  write_string);
}

int sh_overrides_test_address_in_readonly_section(const uint8_t *module_base, const void *address)
{
    return ov_address_in_readonly_section(module_base, address);
}
#endif

/* Slot +0x50 borrows the engine's base idFile implementation, like the idStr helpers; the recorded prologue
 * must be at its RVA. */
static const uint8_t OV_BASE_HELPER_PROLOGUE[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
                                                   0x48, 0x89, 0x74, 0x24, 0x18 };

/* Checks the base idFile helper's prologue at its recorded RVA, then the vtable decoded from the provider
 * constructor, which must be read-only inside the image. Returns NULL when both hold, else the log line naming
 * the failure; *helper_out and *vtable_out are set only once their check passes. */
const char *sh_overrides_check_sites(const uint8_t *module_base, const void *ctor_fn,
                                     const uint8_t **helper_out, void **vtable_out)
{
    uint8_t got[sizeof OV_BASE_HELPER_PROLOGUE];
    uint32_t rva = sh_host_image_rva(module_base, OV_BASE_HELPER_RVA_VK, OV_BASE_HELPER_RVA_GL);
    const uint8_t *helper = rva ? module_base + rva : NULL;
    void *vtable;

    if (helper_out) *helper_out = NULL;
    if (vtable_out) *vtable_out = NULL;
    if (!helper || !ov_address_in_image(module_base, helper) ||
        !sh_safe_read(helper, got, sizeof got) ||
        memcmp(got, OV_BASE_HELPER_PROLOGUE, sizeof OV_BASE_HELPER_PROLOGUE) != 0)
        return "B1: overrides file-shadow SKIPPED -- the base idFile helper for slot +0x50 is "
               "not at its recorded address on this executable";
    if (helper_out) *helper_out = helper;

    vtable = (void *)sh_rip_slot((const uint8_t *)ctor_fn, SH_RIP_RAX, SH_RIP_LEA);
    if (vtable == NULL)
        return "B1: overrides file-shadow SKIPPED -- could not decode the vtable LEA "
               "(ResProviderCtor layout shifted?)";
    /* The decoded vtable must lie in a read-only host section. */
    if (!ov_address_in_image(module_base, vtable) ||
        !ov_address_in_readonly_section(module_base, vtable))
        return "B1: overrides file-shadow SKIPPED -- decoded provider vtable is not a read-only "
               "location inside the host image (the LEA decode did not land on a vtable)";
    if (vtable_out) *vtable_out = vtable;
    return NULL;
}

int sh_overrides_install(const uint8_t *module_base,
                         void *ctor_fn,
                         void *read_string_fn,
                         void *compare_fn,
                         void *write_string_fn)
{
    char line[MAX_PATH + 128];

    if (ctor_fn == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- ResProviderCtor not resolved");
        return 0;
    }
    if (!read_string_fn || !compare_fn || !write_string_fn) {
        ov_stream_helpers_install(read_string_fn,
                                  compare_fn,
                                  write_string_fn);
        backend_log("B1: overrides file-shadow SKIPPED -- idFile native idStr helpers did not all resolve cleanly");
        return 0;
    }
    if (!ov_supported_build_abi(module_base, ctor_fn,
                                read_string_fn,
                                compare_fn,
                                write_string_fn)) {
        backend_log("B1: overrides file-shadow SKIPPED -- resolved engine locations are not clean unique "
                    "signature matches inside the host image");
        return 0;
    }
    if (g_orig_open != NULL) {
        backend_log("B1: overrides file-shadow already installed");
        return 1;
    }

    if (!ov_stream_helpers_install(read_string_fn,
                                   compare_fn,
                                   write_string_fn)) {
        backend_log("B1: overrides file-shadow SKIPPED -- idFile helper table publication refused");
        return 0;
    }

    /* The bind-time byte checks live in sh_overrides_check_sites, which the image-backed test also calls. */
    const uint8_t *helper = NULL;
    void *vtable = NULL;
    const char *refused = sh_overrides_check_sites(module_base, ctor_fn, &helper, &vtable);
    if (helper != NULL) g_stream_vtable[OV_SLOT_BASE_HELPER] = (void *)helper;
    if (refused != NULL) {
        backend_log(refused);
        return 0;
    }

    void **slot = (void **)((uint8_t *)vtable + OPEN_SLOT_OFFSET);

    /* The read-only vtable page is made writable only while the eight-byte open slot is replaced. */
    void *orig = NULL;
    if (!sh_safe_read((const uint8_t *)slot, (uint8_t *)&orig, sizeof orig) || orig == NULL) {
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

    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: overrides file-shadow installed (vtable=%p slot+0x%x=%p, orig open=%p); "
        "built-in defaults: %u from memory",
        vtable, OPEN_SLOT_OFFSET, (void *)slot, orig,
        (unsigned)(sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]));
    backend_log(line);
    return 1;
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
