/* overrides.c -- see overrides.h. The OVERRIDES FILE-SHADOW resource loader.
 *
 * Swaps the engine resource-provider's open-by-name vtable slot (+0xf8) with our override-open hook.
 * On each ordinary engine open the resolution is FOUR-LAYER:
 *   1. USER     -- overrides/<name> under %LOCALAPPDATA%\snapmap-plus\ on disk (an explicit user act; wins).
 *   2. LINKED   -- exact manifest-selected bytes read on demand from the user's installed, read-only
 *                 base-game archives by resource_bridge.c. No archive is copied or changed.
 *   3. BUILT-IN -- our baked default decls (overrides_baked.h), served FROM MEMORY. Nothing is ever
 *                  written to the user's folder, so defaults update with every release and "reset to
 *                  default" is simply deleting the user's file.
 *   4. ENGINE   -- chain to the saved original engine open (the packaged resource).
 * The dynamic decl server may publish an immutable per-decl table from memory.
 * Those exact canonical decltree entries are gated with the user layer but
 * cannot be replaced by a disk or linked resource.
 * A mode>=2 recursion guard goes straight to the original (OG's `param_5 >= 2` branch). For a BUILT-IN
 * name only, a user file that fails a minimal well-formedness check (brace/quote balance) is refused and
 * the built-in default serves instead (logged) -- a garbled file there would take out the "*Custom" tab.
 * The user layer can be disabled for bisecting a broken override set through the restart-only
 * configuration snapshot; the built-in and engine layers remain active regardless.
 *
 * Install-time passes (both logged, both SEH-guarded):
 *   - RECLAIM: earlier releases WROTE the baked defaults to the user's folder if absent. A user file
 *     byte-equal to a baked default (CR bytes ignored) is provably ours-untouched -> deleted, so the
 *     memory layer serves current defaults. A differing file is user-owned -> kept, shadowing.
 *   - AUDIT: enumerate the active user override files into the log, so "what is shadowing what" is
 *     always answerable from the log alone.
 *
 * Clean-room: ported from our own RE (overrides.h header).
 * Zero OG SnapHak bytes. Every disk/engine touch is SEH-guarded -- a shadow failure degrades to a
 * vanilla engine open, never a crash.
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
#include "decl_text.h"
#include "packages.h"
#include "resource_bridge.h"
#include "user_overrides.h"
#include "overrides_baked.h"        /* the built-in "*Custom"-tab default decls (Timeline + Unknown) */

/* The engine open-by-name vtable method offset within the resource-provider vtable.
 * DIRECT: OG patches engineBase+0x2798598; the vtable is engineBase+0x27984a0 -> slot offset = 0xf8. */
#define OPEN_SLOT_OFFSET 0xf8

/* The provider stream ABI is not signature-portable. These five independently
 * resolved locations pin the one Steam build whose resource-provider vtable is
 * 31 idFile slots and whose provider open method is at +0xf8. A newer DOOM
 * build has 34 slots with shifted meanings, so matching function signatures
 * alone is insufficient and must never publish this table there. */
#define OV_PINNED_RES_PROVIDER_CTOR_RVA 0x1A51070u
#define OV_PINNED_IDFILE_READSTR_RVA    0x0267390u
#define OV_PINNED_IDFILE_COMPARE_RVA    0x0267290u
#define OV_PINNED_IDFILE_WRITESTR_RVA   0x0268470u
#define OV_PINNED_PROVIDER_VTABLE_RVA   0x27984A0u

/* The engine open method ABI (DIRECT, from OG FUN_18000b370's own call shape):
 *   void* open(void* this, const char* name, uint8 b1, uint8 b2, uint mode)   // __fastcall, returns idFile*
 * OG masks b1/b2 to 0xff when chaining. mode>=2 -> straight to original (recursion guard). */
typedef void *(*open_fn_t)(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode);

static open_fn_t  g_orig_open  = NULL;   /* the saved engine resource-open (the slot's original value) */
static void     **g_slot       = NULL;   /* the live vtable slot we patched (for uninstall) */
static volatile LONG g_shadow_count = 0;
typedef struct ov_internal_decl {
    char *name;                   /* exact lower-case decltree/<type>/<name>.decl */
    unsigned char *body;          /* process-lifetime copy */
    size_t body_length;
} ov_internal_decl;

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

/* ============================================================ our idFile-subclass stream ===========
 * A reimplementation of the pinned build's PTR_FUN_18003d050 stream (the engine idFile interface,
 * 31 virtual methods -- every slot decompiled in pb1-overrides). The object layout keeps OG's public head:
 *   +0x00 vtable   +0x08 FILE*   +0x10 name   +0x18 length   +0x20 short flag
 * The engine reads the resource through this vtable; the dtor (slot 0) frees the object with OUR
 * allocator (HeapFree) -- so we need no engine allocator/free (OG used the engine's only so the engine
 * could free it; here every method incl. the dtor is ours).
 *
 * TWO BACKINGS, one vtable: fp != NULL -> FILE*-backed (a user override file, OG-equivalent);
 * fp == NULL && buf != NULL -> MEMORY-backed (a built-in default, or a validated user file already read
 * whole). The memory form is read-only (Write/printf return 0) and tracks its own cursor in `pos`;
 * owns_buf says the dtor must HeapFree the buffer (a heap copy) vs leave it (the static baked text). */
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

/* --- the 31 vtable methods, faithful to the pinned build's slot semantics ---------------------------
 * All __fastcall(this in RCX). Behaviour matches OG FUN_18000ae00..b070 exactly, expressed in stdio. */

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
/* [7] The native helper at RVA 0x1a1b520 calls Seek(this,off,ABS=2), then Read(this,buf,len).
 *     We reproduce the same combo through our own methods (the engine fn only dispatched via the vtable). */
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
/* +0x60 is SetLength. Provider streams are deliberately read-only, including file-backed streams:
 * no caller can turn a resource shadow into a writable archive surrogate. Return zero (failure) and
 * leave both backings untouched for every request. */
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
/* OG slot [15]/[16] are the C-varargs printf forms (vfprintf into fp). The engine resource-READ path
 * never calls them; we provide a faithful vfprintf so a write path stays correct. The vtable stores a
 * single entry; both OG slots resolve to a vfprintf-to-fp, so we point both at this thunk. */
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
/* Memory and read-only override streams have no writable/physical provider
 * flag. Returning zero is the proven metadata value used by the eager reader. */
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

/* The stream vtable -- one exact 31-entry table shared by every stream we hand back (the methods are
 * stateless w.r.t. the object beyond `this`). Slot order follows the engine idFile vtable:
 * GetName @+0x18, GetFullPath @+0x20, Read @+0x28, GetLength @+0x58, GetTimestamp @+0x98 --
 * so the engine calls the right method per slot. The final three entries are the native idStr helpers
 * for this exact Steam build; they are populated as one publication after all three clean signatures
 * resolve. Until then they remain NULL and no provider hook is published. */
static void *g_stream_vtable[31] = {
    (void *)ov_dtor,          /* 0  +0x00 close/dtor */
    (void *)ov_ret0_a,        /* 1  +0x08 */
    (void *)ov_ret1,          /* 2  +0x10 native constant true */
    /* +0x18 = the engine idFile GetName slot: idLexer::LoadFile reads it and copies the result
     * into an idStr, so it must be a valid name pointer -- NOT a length. Renderprog decls (and
     * their #include files) load through this slot; entity decls use the +0x20 path below.
     * GetLength is a separate slot (+0x58, ov_length_byseek). Returning ov_length here would
     * deref the file size as a char* and crash. */
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

/* Configure the three native idStr slots before any provider object can expose this table. The
 * signatures are deliberately all-or-nothing: a missing or hook-tolerant helper leaves a terminal
 * refusal state, so we never publish a mixed native/NULL tail. */
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

    /* The table is private until the slot publication below. InterlockedExchange is a full
     * release barrier on Windows, so an engine thread cannot observe a READY state with partial
     * helper pointers. */
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
    s->flag16 = 1;   /* OG sets the +0x20 short to 1 */
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

/* ====================================================== override-file path resolution ==============
 * OG FUN_18000b110 (DIRECT, XINPUT1_3.dll decompile + disasm 0xb13b..0xb1c8). The branch selector is a ".inc"-SUFFIX test, NOT a '/' test:
 *
 *   match = strstr(name, ".inc");                       // DAT_18003e2d0 = a strstr-style substring find
 *   if (match == NULL || *(match + 4) != '\0')          // ".inc" absent, or NOT at end of the string
 *       fmt = "overrides/%s";                            //   -> COMMON case (the vast majority)
 *   else {                                               // name's first ".inc" sits at its very end
 *       if (strchr(name,'/') != NULL &&
 *           strstr(name,"includes") != name)             //   "includes" is NOT a prefix of name
 *           rel = strchr(name,'/') + 1;                   //   strip up to & incl the first '/'
 *       fmt = "overrides/shader_includes/%s";            //   -> RARE case (shader-include .inc files)
 *   }
 *   sprintf(buf, fmt, rel);  // then prepend <root>\overrides... ; all '/' -> '\'
 *
 * So shader_includes is the EXCEPTION for ".inc"-suffixed shader-include names; overrides/<name> is the
 * common path for every normal resource (env/..., models/..., fonts/... -- none end in ".inc"). DAT_18003e2d0
 * is a runtime-resolved substring-find fn-ptr (null in the static image; proven strstr-semantics by its other
 * call site FUN_180026680: `find(name,"superscriptx64.dll") != 0 -> LoadLibrary`). The full path =
 * <root>\overrides\... with all '/' -> '\'. */

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

static int ov_internal_decl_table_publish(
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
    /* Publish the table only after all keys and bodies are complete. The
     * process-lifetime table is never freed or republished after READY. */
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_READY);
    return 1;
}

static ov_stream *open_internal_decl(const char *name, int *matched)
{
    size_t i;
    if (matched) *matched = 0;
    if (!name || !sh_user_overrides_enabled_for_launch() ||
        InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) !=
            OV_INTERNAL_DECL_TABLE_READY) return NULL;
    for (i = 0; i < g_internal_decl_count; i++) {
        if (strcmp(name, g_internal_decls[i].name) != 0) continue;
        if (matched) *matched = 1;
        return make_mem_stream(g_internal_decls[i].body,
                               (long long)g_internal_decls[i].body_length,
                               g_internal_decls[i].name, 0);
    }
    return NULL;
}

int sh_overrides_internal_decl_published(const char *name)
{
    size_t i;
    if (!name || !sh_user_overrides_enabled_for_launch() ||
        InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) !=
            OV_INTERNAL_DECL_TABLE_READY) return 0;
    for (i = 0; i < g_internal_decl_count; i++)
        if (_stricmp(name, g_internal_decls[i].name) == 0) return 1;
    return 0;
}

int sh_overrides_internal_decl_table_can_install(void)
{
    return g_orig_open != NULL && sh_user_overrides_enabled_for_launch() &&
           InterlockedCompareExchange(&g_internal_decl_table_state, 0, 0) ==
               OV_INTERNAL_DECL_TABLE_NEW;
}

/* Retire the published internal decl table so a NEW one can be published mid-session.
 *
 * The table is a boot one-shot: can_install() demands state == NEW, so after the boot
 * publication every later attempt is refused and a runtime re-arm classifies its candidates
 * and then has nowhere to put them. This is the fourth and last boot-bound surface in the
 * runtime-registration chain (package list, resource-bridge manifests, decl-server snapshot,
 * this table).
 *
 * DOES NOT FREE. The test-only reset beneath this one is safe precisely because it "occurs
 * before any engine thread can retain a stream" -- at runtime that guarantee is gone: an
 * engine thread may hold an ov_stream reading straight out of a table body, and freeing under
 * it is a use-after-free. So the old table is retired and leaked, bounded by the 512-entry cap
 * and by how rarely a package is installed. Correctness over a few hundred KB. */
void sh_overrides_internal_decl_table_reopen(void)
{
    char line[160];
    size_t held = g_internal_decl_count;
    /* Reopen the STATE only. The entries stay published and stay readable, because the runtime
     * publish MERGES over them -- dropping them here is exactly the bug that made a Cyberdemon
     * re-arm break the unrelated transformations package. */
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_NEW);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: overrides internal decl table REOPENED for re-publication (%u entr(ies) "
                "retained and still served; a merge will carry them forward)",
                (unsigned)held);
    backend_log(line);
}

/* Publish `entries` MERGED over whatever the table already holds.
 *
 * Carrying the old entries forward is the whole point: a re-arm publishes only the identities
 * the CURRENT pass classified as missing, and anything the previous pass had published would
 * otherwise silently lose its decltree source -- including entries belonging to packages this
 * pass never looked at.
 *
 * Old entries are carried by POINTER. The old array is retired and leaked (a reader may be
 * inside it), but its name/body allocations remain reachable from the merged array, so they
 * stay valid and are never double-freed. A new entry whose key matches an old one wins.
 *
 * Returns 1 on success. On any failure the previously published table is left exactly as it
 * was, because the state is only moved to READY once the merged array is complete. */
int sh_overrides_internal_decl_table_merge(
    const sh_overrides_internal_decl_entry *entries, size_t count)
{
    ov_internal_decl *merged;
    ov_internal_decl *old = g_internal_decls;
    size_t old_count = g_internal_decl_count;
    size_t total, i, j, at = 0;
    char line[192];

    if (!g_orig_open || !sh_user_overrides_enabled_for_launch()) return 0;
    if (!entries || count == 0) return 0;
    if (count > SIZE_MAX - old_count) return 0;
    total = old_count + count;
    if (total > OV_INTERNAL_DECL_MAX_ENTRIES) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: overrides internal decl table MERGE refused -- %u old + %u new exceeds "
                    "the %u-entry cap", (unsigned)old_count, (unsigned)count,
                    (unsigned)OV_INTERNAL_DECL_MAX_ENTRIES);
        backend_log(line);
        return 0;
    }

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

    /* Then the old ones the new set did not supersede, carried by pointer. */
    for (i = 0; i < old_count; i++) {
        int superseded = 0;
        if (!old[i].name || !old[i].body) continue;
        for (j = 0; j < count; j++) {
            if (strcmp(old[i].name, merged[j].name) == 0) { superseded = 1; break; }
        }
        if (superseded) continue;
        merged[at].name = old[i].name;         /* borrowed, not copied: the old array leaks */
        merged[at].body = old[i].body;
        merged[at].body_length = old[i].body_length;
        at++;
    }

    g_internal_decls = merged;
    g_internal_decl_count = at;
    InterlockedExchange(&g_internal_decl_table_state, OV_INTERNAL_DECL_TABLE_READY);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: overrides internal decl table MERGED -- %u new + %u carried forward = %u "
                "published (previous array retired, entries reused by pointer)",
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

/* Build the on-disk override path for engine resource `name` into `out`. Returns 1 if a path was built
 * (always, for a non-empty name). The selection mirrors OG FUN_18000b110 EXACTLY (see the block comment
 * above): the shader_includes branch is the RARE exception for ".inc"-suffixed shader-include names; every
 * normal resource (no ".inc" suffix) takes the common overrides/<name> path. */
static int build_override_path(const char *name, char *out, size_t cap)
{
    if (!name || !name[0]) return 0;
    char root[MAX_PATH];
    resolve_root(root, sizeof root);

    /* OG's ".inc"-suffix test: find the first ".inc"; the shader branch is taken only when that match is
     * at the very end of the name (the byte after the 4-char ".inc" is the terminator) -- i.e. `match &&
     * match[4]=='\0'`. (Byte-faithful to OG's `*(strstr(name,".inc")+4) == 0`.) */
    const char *match = strstr(name, ".inc");
    int is_shader_include = (match != NULL && match[4] == '\0');

    const char *rel = name;
    if (is_shader_include) {
        /* OG: within the shader branch, strip up to & incl the first '/' UNLESS the name begins with
         * "includes" (OG: `strchr(name,'/') && strstr(name,"includes") != name -> rel = slash+1`). */
        const char *slash = strchr(name, '/');
        if (slash != NULL && strstr(name, "includes") != name)
            rel = slash + 1;
    }

    if (is_shader_include)
        _snprintf_s(out, cap, _TRUNCATE, "%s\\overrides\\shader_includes\\%s", root, rel);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%s\\overrides\\%s", root, name);

    /* normalize '/' -> '\' (OG does the same on the assembled path). */
    for (char *p = out; *p; ++p)
        if (*p == '/') *p = '\\';
    return 1;
}

/* ============================================================ package resolution ==================*/

/* The engine names every decl `generated/decls/<type>/<name>.decl` -- one flat
 * virtual namespace, regardless of who published the decl. Before packages that
 * mapped one-to-one onto `overrides\generated\decls\...`, so joining the name
 * onto the overrides root WAS the resolver.
 *
 * A package owns its own root (`overrides\cyberdemon\decls\...`), so that join
 * can never reach it: the engine has no idea the folder exists and will never
 * ask for `cyberdemon/decls/...`. Left unhandled the package's decl bodies are
 * silently never served -- the decl server registers the identity, the engine
 * opens nothing, and the parse yields an empty default with no resolved
 * entityDef. That is what kept the Cyberdemon out of the Toybox.
 *
 * So `generated/decls/<rest>` is resolved against every installed package in
 * turn as `<package root>\decls\<rest>`.
 *
 * The same problem applies to every OTHER engine namespace a package may own.
 * A custom render program is the case that forced this to be a table: the module
 * loader at RVA 0xD922D0 builds `generated/spirv/<name>.{vspv|fspv|cspv}` and
 * opens it through this very vtable slot, and its call site passes mode 0, so the
 * open hook admits it. Its pre-translated source blob arrives the same way as
 * `generated/renderprogs/<name>_pc_vulkan.bin`. Both must be package-scoped, or a
 * shader would have to live in the shared tree and two packages could clobber
 * each other's shaders on disk -- exactly what packages exist to prevent.
 *
 * Images are the fourth, and the SnapMap Toybox is what forced them. A tile draws a
 * material, that material names a .tga, and the engine resolves the pair to an image
 * resource `generated/image/<path>.bimage`. A package could already ship the material --
 * a material is an ordinary decl -- but not the pixels behind it, so a package could
 * never contribute a tile icon of its own. The prefix is STRIPPED here, unlike `shaders`:
 * every engine image name begins with that same constant, so repeating it inside each
 * package would add depth and no information.
 *
 * A package therefore serves only these enumerated namespaces, and only out of
 * the subdirectory named here. Nothing else it contains is reachable: its
 * package.json can never become an engine resource. Under `shaders` the package
 * path mirrors the engine name verbatim, which keeps the mapping obvious and
 * costs nothing to extend. */
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

/* The installed package set, captured once at install. The overrides layer is
 * already an immutable launch snapshot -- the audit, the reclaim pass and the
 * user-layer gate all read the tree exactly once -- and this is on the engine's
 * file-open path, so re-enumerating the tree per open is not an option. */
/* DOUBLE-BUFFERED so the package list can be re-scanned mid-session without a lock on the
 * open path, which is hot (every engine resource open walks it). Writers fill the inactive
 * buffer and then publish it with one InterlockedExchange; readers take the index ONCE and
 * use that buffer for the whole resolve. A reader that raced a publish sees either the old
 * complete list or the new complete list -- never a torn one. */
static sh_package g_ov_packages_buf[2][SH_PACKAGES_MAX];
static size_t g_ov_package_counts[2];
static volatile LONG g_ov_pkg_active;   /* 0 or 1 */
static volatile LONG g_ov_pkg_generation;

static void ov_capture_packages(void)
{
    char root[MAX_PATH];
    size_t count = 0;
    LONG active = InterlockedCompareExchange(&g_ov_pkg_active, 0, 0);
    LONG target = active ^ 1;              /* fill the buffer nobody is reading */

    resolve_root(root, sizeof root);
    if (!root[0]) return;
    /* A partial enumeration still resolves whatever it did find: unlike the decl
     * server, a miss here degrades to the packaged resource rather than
     * publishing a wrong identity, so serving fewer packages is safe. */
    (void)sh_packages_enumerate(root, g_ov_packages_buf[target], SH_PACKAGES_MAX, &count);
    g_ov_package_counts[target] = count;
    InterlockedExchange(&g_ov_pkg_active, target);   /* publish: one atomic store */
    InterlockedIncrement(&g_ov_pkg_generation);
}

/* Re-scan the packages folder and publish the new list to the open path.
 *
 * WHY THIS EXISTS. The open hook already stats the disk on EVERY open, so a package's bytes
 * are servable the moment they land -- but only if the package is in this list, and the list
 * used to be captured exactly once at install. That made a mid-session install invisible for
 * a reason that had nothing to do with the engine. Registration of new DECL IDENTITIES is a
 * separate, harder problem (see decl_server.c); this only makes the BYTES reachable.
 *
 * Returns the number of packages now visible. */
unsigned long sh_overrides_rescan_packages(void)
{
    char line[160];
    LONG active;
    ov_capture_packages();
    active = InterlockedCompareExchange(&g_ov_pkg_active, 0, 0);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B1: overrides package list RE-SCANNED -- %u package(s) now visible (generation %ld)",
                (unsigned)g_ov_package_counts[active],
                (long)InterlockedCompareExchange(&g_ov_pkg_generation, 0, 0));
    backend_log(line);
    return (unsigned long)g_ov_package_counts[active];
}

static int ov_is_regular_file(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* Resolve engine resource `name` to an existing file under overrides\, or 0 if
 * no layer provides it. The legacy whole-tree path is tried first so an install
 * that predates packages resolves byte-for-byte where it always did; installed
 * packages are then tried in `sh_packages_enumerate`'s deterministic order, so
 * two machines with the same packages pick the same file. */
static int ov_resolve_existing(const char *name, char *out, size_t cap)
{
    size_t i, n;

    if (!name || !name[0] || !out || cap == 0) return 0;
    if (!build_override_path(name, out, cap)) return 0;
    if (ov_is_regular_file(out)) return 1;

    for (n = 0; n < sizeof(g_ov_namespaces) / sizeof(g_ov_namespaces[0]); n++) {
        const ov_namespace *ns = &g_ov_namespaces[n];
        size_t prefix_length = strlen(ns->engine_prefix);
        const char *relative;

        if (_strnicmp(name, ns->engine_prefix, prefix_length) != 0) continue;
        relative = ns->strip_prefix ? name + prefix_length : name;
        if (!relative[0]) break;

        /* Take the active buffer index ONCE for this whole resolve, so a concurrent
         * re-scan cannot move the list out from under the loop. */
        LONG act = InterlockedCompareExchange(&g_ov_pkg_active, 0, 0);
        size_t pkg_count = g_ov_package_counts[act];
        for (i = 0; i < pkg_count; i++) {
            char *p;
            if (_snprintf_s(out, cap, _TRUNCATE, "%s\\%s\\%s",
                            g_ov_packages_buf[act][i].root, ns->package_subdir, relative) < 0)
                continue;
            for (p = out; *p; ++p)
                if (*p == '/') *p = '\\';
            if (ov_is_regular_file(out)) return 1;
        }
        break;                      /* prefixes are disjoint; one match is all */
    }
    out[0] = '\0';
    return 0;
}

#ifdef SH_OVERRIDES_TESTING
int sh_overrides_test_resolve_existing(const char *name, char *out, size_t cap)
{
    ov_capture_packages();
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

    /* size via seek-end/tell/restore (OG reads size with _ftelli64). */
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

/* USER layer open for a BUILT-IN name: slurp + validate the user's file. Well-formed -> a memory
 * stream over the heap copy (stream owns it). Malformed -> refuse (log) and let the caller serve the
 * built-in default -- a garbled file at one of these names would take out the "*Custom" tab. A file
 * the slurp can't handle (oversize/alloc) is served unvalidated as a plain stream (benefit of doubt). */
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

/* The override-open hook -- our value in the engine's open vtable slot. Same ABI as the engine method.
 * mode>=2 (OG param_5>=2) is a recursion/no-shadow guard -> straight to the original. Otherwise resolve
 * four-layer: USER disk file -> linked installed resource -> BUILT-IN baked default (from memory) ->
 * chain to the engine original.
 * The user layer alone is gated by the immutable launch snapshot. SEH-guarded so a shadow path fault
 * degrades to a vanilla open. */
static void *ov_open_hook(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode)
{
    if (g_orig_open == NULL) return NULL;   /* defensive: never happens once installed */

    if (mode < 2) {
        ov_stream *internal = NULL;
        int internal_match = 0;
        __try {
            internal = open_internal_decl(name, &internal_match);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            internal = NULL;
        }
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
                    int linked_status = sh_resource_bridge_open(name, &linked, &linked_length,
                                                                &linked_source);
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
    /* no override (or guard) -> the engine's normal open (OG masks the byte args to 0xff). */
    return g_orig_open(self, name, (unsigned char)(b1 & 0xff), (unsigned char)(b2 & 0xff), mode);
}

/* ============================================================ vtable-global LEA decode ==============
 * The engine resource-provider vtable is a .data global (can't be masked-byte sig-scanned). The ctor
 * ResProviderCtor (resolved by signature) starts with `... 48 8B D9 (MOV RBX,RCX) ; 48 8D 05 <disp32>
 * (LEA RAX,[rip+vtable]) ; 48 89 01 (MOV [RCX],RAX)`. We scan forward from the resolved entry for the
 * FIRST `48 8D 05` and decode its rip-relative disp to recover the vtable VA. The install then requires
 * the decoded address and native helper RVAs to match the audited 31-slot build before publication. */
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

/* ====================================================== install-time reclaim + audit ===============
 * RECLAIM: earlier releases wrote the built-in defaults to <root>\overrides\<name> if absent. Such a
 * file, byte-equal to the baked text with CR bytes ignored (some copies picked up CRLF endings), is
 * provably OURS-untouched -> delete it, so the in-memory built-in layer (which updates with every
 * release) serves instead. ANY difference -> the file is user-owned -> kept, and it keeps winning.
 * SEH-guarded; a reclaim failure just leaves the file shadowing (the old behavior). */
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

static int ov_address_is_pinned(const uint8_t *module_base, const void *address,
                                uintptr_t expected_rva)
{
    uintptr_t base = (uintptr_t)module_base;
    uintptr_t value = (uintptr_t)address;
    return module_base && address && value >= base && value - base == expected_rva;
}

static int ov_supported_build_abi(const uint8_t *module_base,
                                  const void *ctor_fn,
                                  const void *read_string_fn,
                                  const void *compare_fn,
                                  const void *write_string_fn)
{
    return ov_address_is_pinned(module_base, ctor_fn, OV_PINNED_RES_PROVIDER_CTOR_RVA) &&
           ov_address_is_pinned(module_base, read_string_fn, OV_PINNED_IDFILE_READSTR_RVA) &&
           ov_address_is_pinned(module_base, compare_fn, OV_PINNED_IDFILE_COMPARE_RVA) &&
           ov_address_is_pinned(module_base, write_string_fn, OV_PINNED_IDFILE_WRITESTR_RVA);
}

#ifdef SH_OVERRIDES_TESTING
int sh_overrides_test_supported_build_abi(const uint8_t *module_base,
                                          const void *ctor,
                                          const void *read_string,
                                          const void *compare,
                                          const void *write_string)
{
    return ov_supported_build_abi(module_base, ctor, read_string, compare, write_string);
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

/* AUDIT: enumerate the user's active override files into the log (count + names, bounded), flagging any
 * that fail the well-formedness tripwire -- so "what is shadowing what" is answerable from the log. */
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
        /* The ctor is only used to DECODE the vtable LEA; a hooked prologue would corrupt the decode.
         * Refuse on the hook-tolerant known_rva fallback (same conservative policy as the other ops). */
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
    if (!ov_supported_build_abi(module_base, ctor_fn, read_string_fn,
                                compare_fn, write_string_fn)) {
        backend_log("B1: overrides file-shadow SKIPPED -- resolved engine locations do not match the pinned 31-slot idFile/provider ABI");
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
    if (!ov_address_is_pinned(module_base, vtable, OV_PINNED_PROVIDER_VTABLE_RVA)) {
        backend_log("B1: overrides file-shadow SKIPPED -- decoded provider vtable does not match the pinned +0xf8 ABI");
        return 0;
    }

    void **slot = (void **)((uint8_t *)vtable + OPEN_SLOT_OFFSET);

    /* Save the original open + overwrite the slot with our hook. The slot is .data (an 8-byte pointer),
     * so we VirtualProtect RW, store, restore -- NOT install_inline_hook (that patches code). */
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
    ov_capture_packages();       /* one enumeration; the open path resolves against this snapshot */
    reclaim_baked_overrides();   /* delete OUR untouched previously-written defaults (memory layer serves now) */
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
