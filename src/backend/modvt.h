/* modvt.h -- private virtual-texture families carried by installed mods.
 *
 * A bundle may carry the members of a private mega2 family at
 * virtualtextures/mods/<id>/<family>_sq<N>.mega2 (sh_mb_vt_member; the format
 * is docs/vt-baker.md). Its materials name the family as
 * `landpagefile "mods/<id>/<family>"` with an explicit `virtualmapping`.
 * The engine binds such a material to the family only when a page-source
 * record with exactly that name exists when the material is parsed; otherwise
 * the material is classified as a unique family and draws with default
 * images. The engine never creates that record itself and never reads a
 * private family's .vmtr rows, so this layer creates one record per family:
 *
 *   SetSource(record, "mods/<id>/<family>", "virtualtextures/mods/<id>/<family>.pages",
 *             flavor, type 2, direct 0, reader NULL)
 *
 * SetSource's mega2 loader then opens the served _sq1.._sq16 members through
 * the provider (no .pages file exists or is needed), allocates the host page
 * table (11,184,810 bytes for the fixed 12 levels) and the page-table image.
 *
 * When: on DOOM's main thread, once the VT system has built record 1
 * (`_vmtr`, type 2), and before any material of the bundle is parsed. Every
 * decl registration pass (the editor palette build, a map's activation)
 * synchronises first, so a family is registered before the pass parses its
 * materials; an index rebuilt on the main thread (sh_mods install, rescan)
 * synchronises at once; an index rebuilt on another thread (boot, a fetch, a
 * migration) waits for the next pass. A sync is idempotent: a family that
 * already holds its record keeps it.
 *
 * Records: the table has 32 slots and records 1..31. Record 31 is never used
 * (VirtualTextureForMaterial releases it on every lookup miss), and no record
 * this layer did not create is touched: a family that finds records 1..30
 * occupied is refused with one log line, and a name another record already
 * holds (a material parsed before its family was served, or the sh_vt
 * diagnostic) is refused the same way.
 *
 * Release: a family whose bundle leaves the index (removed, replaced by
 * another build, skipped) is released only when no decl registration pass has
 * run since it was registered. After a pass its materials may hold the
 * record, and the decls themselves stay registered for the process, so the
 * record is kept until DOOM exits (logged once).
 *
 * Flavor: 2 on Vulkan. On OpenGL the engine turns a material's flavor into 4
 * only when GL_AMD_sparse_texture is present and its cvar is set; then the
 * family registers with flavor 4. Without that, a family is not registered
 * on OpenGL and one line says it cannot be served there.
 */
#ifndef SH_MODVT_H
#define SH_MODVT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MODVT_SLOTS      32      /* pointer table slots; records 1..31 */
#define SH_MODVT_LAST_USED  30      /* the highest record this layer takes */
#define SH_MODVT_NAME_MAX   192
#define SH_MODVT_PATH_MAX   (SH_MODVT_NAME_MAX + 24)
#define SH_MODVT_MAX        64      /* families tracked per sync */

/* Record fields (equal on both renderers, build 13954591). The record stride
 * differs (VK 0x458, GL 0x520), so records are only reached through the
 * pointer table. */
#define SH_VT_REC_TYPE       0x000   /* 1 unique, 2 VMTR-style */
#define SH_VT_REC_OWNER      0x008   /* table owner; rows at +0x138, count +0x140 */
#define SH_VT_REC_MAGIC      0x010   /* provider header copy */
#define SH_VT_REC_GRID       0x01C
#define SH_VT_REC_LEVELS     0x024   /* short */
#define SH_VT_REC_PATH       0x070   /* char * */
#define SH_VT_REC_NAME       0x1A0   /* char * */
#define SH_VT_REC_STREAMED   0x2C0
#define SH_VT_REC_PAGESFILE  0x2D0
#define SH_VT_REC_READER     0x2E8
#define SH_VT_REC_MEGA2      0x2F0
#define SH_VT_REC_TEXELS     0x2FC
#define SH_VT_REC_PAGETABLE  0x3D8   /* its first int is the flavor */
#define SH_VT_REC_PTIMAGE    0x3E8
#define SH_VT_REC_HOSTPT     0x3F8

/* ------------------------------------------------------------------ families */

typedef struct sh_modvt_family {
    char name[SH_MODVT_NAME_MAX];        /* "mods/<id>/<family>" */
    char path[SH_MODVT_PATH_MAX];        /* "virtualtextures/mods/<id>/<family>.pages" */
    char bundle[SH_MODVT_NAME_MAX];      /* "<id>@<version>" for the log */
    char bundle_hash[65];
    unsigned members;                    /* bit N-1: member _sq<N> is served */
    uint64_t bytes;                      /* sum of the members' sizes */
} sh_modvt_family;

/* The families the published serving index carries, in index order (one per
 * name; a name two bundles serve belongs to the first). Returns how many were
 * found (which may exceed cap). */
int sh_modvt_collect(sh_modvt_family *out, int cap);

/* ------------------------------------------------------------------ sync */

/* The engine operations a sync uses. Production binds them to the VT system
 * (modvt.c engine half); tests supply a fake. */
typedef struct sh_modvt_ops {
    void *context;
    int (*ready)(void *context);                         /* 1 when record 1 is built */
    int (*flavor)(void *context);                        /* 2 or 4; 0 when no family can be served */
    int (*find)(void *context, const char *name);        /* record 1..31 with that name, else -1 */
    int (*is_free)(void *context, int record);           /* 1 when record (1..30) holds no source */
    int (*set_source)(void *context, int record, const char *name, const char *path, int flavor);
                                                         /* 1 registered, 0 refused, -1 faulted */
    int (*release)(void *context, int record);           /* 1 released */
} sh_modvt_ops;

typedef struct sh_modvt_result {
    int wanted;        /* families in the index */
    int registered;    /* registered by this sync */
    int kept;          /* already registered, unchanged */
    int released;      /* released by this sync */
    int held;          /* left the index (or changed) but kept: a pass ran since registration */
    int refused;       /* not registered: name taken, renderer, table full, or SetSource refused */
    int full;          /* of them, because records 1..30 were in use */
    int lost;          /* records the engine no longer held under the family's name */
    int not_ready;     /* 1 when the VT system was not up and nothing was done */
} sh_modvt_result;

/* Bring the product's records in line with `wanted`: verify the records it
 * holds, release what left the index (unless a pass ran since), then register
 * every family that holds no record. Logs one VT: line per change and per new
 * refusal. Returns the number of product records afterwards, or -1 when the
 * VT system is not ready (nothing is done). */
int sh_modvt_sync(const sh_modvt_family *wanted, int n, const sh_modvt_ops *ops, const char *phase,
                  sh_modvt_result *out);

/* A decl registration pass is about to run: every family registered now may
 * have materials bound to its record from here on. */
void sh_modvt_mark_exposed(void);

/* The family name that owns record k (1..31), or NULL when the product does
 * not hold it. */
const char *sh_modvt_record_owner(int k);

/* Product records held now. */
int sh_modvt_owned_count(void);

/* Forget every product record (tests only). */
void sh_modvt_reset(void);

/* ------------------------------------------------------------------ decoding */

/* What modvt reads out of the two signed functions. RVAs are relative to the
 * image base; the GL sparse fields are 0 on Vulkan. */
typedef struct sh_modvt_layout {
    uint32_t sys_rva;          /* the VT system object (LEA RCX at the SetSource head) */
    uint32_t release_rva;      /* Release(record), the second call there */
    uint32_t table_off;        /* record pointer table, from the VT system */
    uint32_t sparse_ext_rva;   /* GL: __glewTexStorageSparseAMD */
    uint32_t sparse_cvar_rva;  /* GL: the int the sparse test reads with it */
} sh_modvt_layout;

/* "48 8D 0D d32 E8 r32 48 8B CE E8 r32" in code[0..len): offset of the LEA, else -1. */
int sh_modvt_find_setsource_head(const uint8_t *code, size_t len);

/* "49 8B 85 d32 E9" in code[0..len): the disp32 (table + 8), else 0. */
uint32_t sh_modvt_find_table_disp(const uint8_t *code, size_t len);

/* Release's head: MOV RAX,RSP; PUSH RBP, and CMP DWORD [RCX+0x2C0],0 in its
 * first len bytes. */
int sh_modvt_release_head_ok(const uint8_t *code, size_t len);

/* Decode the layout from SetSource's first bytes (at ss_rva) and
 * VirtualTextureForMaterial's (at vf_rva). `opengl` also decodes the sparse
 * test. Returns 1 when the VT system, Release and the table were found (the
 * caller still checks Release with sh_modvt_release_head_ok), else 0. */
int sh_modvt_decode_layout(const uint8_t *ss, size_t ss_len, uint32_t ss_rva, const uint8_t *vf, size_t vf_len,
                           uint32_t vf_rva, int opengl, sh_modvt_layout *out);

/* ------------------------------------------------------------------ engine */
#ifndef SH_MODVT_PURE
#include "signatures.h"

/* Bind SetSource (VtSetSource), VirtualTextureForMaterial (VtForMaterial),
 * the VT system, Release and the record table, the process-heap scope and
 * the main-thread identity, and listen to the serving index. Logs one VT:
 * line. Returns 1 when families can be registered. */
int sh_modvt_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Called by modserve_engine.c before every decl registration pass (editor
 * palette build, map activation), on the main thread. */
void sh_modvt_before_pass(const char *phase);

/* One status line through sh_printf (sh_mods status, sh_vt status). */
void sh_modvt_print_status(void);

/* sh_vt sync: run the sync now (console, main thread), then the status line. */
void sh_modvt_console_sync(void);

/* The bound engine surface, for the sh_vt diagnostic (vt_harness.c). */
int      sh_modvt_engine_bound(void);
int      sh_modvt_engine_vulkan(void);              /* 1 Vulkan, 0 OpenGL, -1 unknown */
int      sh_modvt_engine_ready(void);
int      sh_modvt_engine_flavor(void);              /* the flavor a matched material carries: 2 or 4 */
uint8_t *sh_modvt_engine_record(int k);             /* record k (0..31) or NULL */
uint8_t *sh_modvt_engine_sys(void);
uint32_t sh_modvt_engine_table(void);
int      sh_modvt_engine_on_main(void);             /* 1 main thread, 0 not, -1 unknown */
/* SetSource / Release inside process heap 0 under SEH: 1 done (for SetSource
 * *result holds its return), 0 refused before the call, -1 faulted (*fault
 * holds the exception code). Main thread only. */
int      sh_modvt_engine_set_source(int k, const char *name, const char *path, int flavor, int *result,
                                    double *ms, unsigned *fault);
int      sh_modvt_engine_release(int k, unsigned *fault);
#endif

#ifdef __cplusplus
}
#endif

#endif
