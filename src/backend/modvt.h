/* modvt.h -- a mod's own lit virtual-texture board.
 *
 * A mod may carry the members of its own mega2 family (its board) at
 * virtualtextures/mods/<salt>/<name>_sq<N>.mega2 (sh_mb_vt_member; the format
 * is docs/vt-baker.md). Its materials name the board as
 * `landpagefile "mods/<salt>/<name>"` with explicit mapping lines, on the
 * stock `skinvmtr` and `skinvmtrwounds` programs. Those programs read only
 * the `vmtr*` parameters, which the renderer sets once per view from the
 * game's own board (record 1); a material's parameter block is pushed over
 * the view values for that material's draws and rolled back after them. So a
 * board needs three things, each done here on both renderers:
 *
 *   1. A page-source record of its own:
 *        SetSource(record, "mods/<salt>/<name>", "virtualtextures/mods/<salt>/<name>.pages",
 *                  flavor 2, type 2, direct 0, reader NULL)
 *      SetSource's mega2 loader opens the served _sq members through the
 *      provider and allocates the board's host page table and page-table
 *      image. The board owns its whole 2048 x 2048-page address space.
 *   2. The record pointed at the material-format page cache, the pool of
 *      resident pages the game's own board uses (record 1's cache, flavor 0:
 *      the only cache with the three-layer page array lit programs sample).
 *      Done right after SetSource, before any material is bound to the
 *      record, so no page of it is resident anywhere yet.
 *   3. Two detours. The material binder (VtMaterialBind), after the engine's
 *      own writes, also sets `vmtrPageTableMap` (the board's page-table image)
 *      and `vmtrFeedbackFloat` (its feedback word, whose source field is the
 *      record index) in the material's parameter block, for a material bound
 *      to a board only, under the very handles the per-view setter fills
 *      from record 1 (VtViewPageTable, VtViewFeedback). VirtualTextureForMaterial (VtForMaterial) returns the
 *      board's record for a material whose landpagefile names it: the engine's
 *      own lookup also compares the record's cache flavor with the
 *      material's, which no longer agree after step 2.
 * A stock material never meets a board record, so its parameters and the
 * game's own board are untouched.
 *
 * Which boards: one per board name the loaded map serves, from the build the
 * map serves (sh_modserve_each_served_here: the map's own copy of a salt, or
 * the root folder's; in the editor every mod whose rows the Toybox shows).
 *
 * When (DOOM's game thread only): the render thread and the renderer's view
 * jobs read the record table every frame (the per-view setter, and the
 * per-cache pass at VK 0x17E7830 that reads each named record's cache), and
 * SetSource fills a record's name long before its cache. The engine changes
 * VT state at one point of its frame: EndFrame first syncs the render thread
 * (renderSystem vtable +0x80), then runs the VT system's frame update
 * (VtFrameCall). Boards are changed there: a detour on that frame update runs
 * the queued sync (registrations, releases, rebinds) before the engine's own
 * update, after the job manager has also finished every job list
 * (JobsWaitAll), as the engine does before it releases records at a map unload.
 *   - Every index change, boot and Refresh queues a sync for that point.
 *   - At the start of every decl registration pass (the editor palette build, a
 *     map's activation) a board still missing is registered at once, since the
 *     material parser binds a material to the record whose name equals its
 *     landpagefile; releases always wait for the frame point. A board
 *     registered again after a release has its earlier materials bound again.
 *
 * How many: the record table has 32 slots and the feedback word a 5-bit
 * source, so the engine has records 1..31. Record 1 is the game's own board,
 * record 31 is reused by the engine on every lookup miss, and each map's own
 * unique families (the hub, module palettes) hold some of the rest; the
 * boards take what is free among 2..30. A board that finds none is refused;
 * the player is told in one sentence.
 */
#ifndef SH_MODVT_H
#define SH_MODVT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MODVT_SLOTS      32      /* pointer table slots; records 1..31 */
#define SH_MODVT_LAST_USED  30      /* the highest record a board takes */
#define SH_MODVT_NAME_MAX   192
#define SH_MODVT_PATH_MAX   (SH_MODVT_NAME_MAX + 24)
#define SH_MODVT_FLAVOR     2       /* the flavor SetSource is given (it allocates no cache) */

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
#define SH_VT_REC_SOURCE     0x2C0   /* the record's index: the feedback word's source */
#define SH_VT_REC_PAGESFILE  0x2D0
#define SH_VT_REC_READER     0x2E8
#define SH_VT_REC_MEGA2      0x2F0
#define SH_VT_REC_TEXELS     0x2FC
#define SH_VT_REC_CACHE      0x3D8   /* the physical page cache; its first int is the cache's flavor */
#define SH_VT_REC_PTIMAGE    0x3E8   /* the page-table image */
#define SH_VT_REC_HOSTPT     0x3F8

/* Material fields (both renderers). */
#define SH_VT_MAT_PARMS      0x070   /* the parameter set the binder writes */
#define SH_VT_MAT_RECORD     0x0D8   /* bound record index */
#define SH_VT_MAT_RECTYPE    0x0DC   /* that record's type */
#define SH_VT_MAT_LANDPAGE   0x0E0   /* char *: the landpagefile name */

/* ------------------------------------------------------------------ boards */

typedef struct sh_modvt_family {
    char name[SH_MODVT_NAME_MAX];        /* "mods/<salt>/<name>" */
    char path[SH_MODVT_PATH_MAX];        /* "virtualtextures/mods/<salt>/<name>.pages" */
    char bundle[SH_MODVT_NAME_MAX];      /* the mod's id, for the log and the notice */
    char bundle_hash[65];
    unsigned members;                    /* bit N-1: member _sq<N> is served */
    uint64_t bytes;                      /* sum of the members' sizes */
} sh_modvt_family;

/* The boards the loaded map serves, in index order (one per name; a name two
 * builds serve belongs to the first), in a table the caller frees with
 * free(). Returns how many (0 with *out NULL when there are none). */
int sh_modvt_collect(sh_modvt_family **out);

/* ------------------------------------------------------------------ sync */

/* The engine operations a sync uses. Production binds them to the VT system
 * (modvt.c engine half); tests supply a fake. */
typedef struct sh_modvt_ops {
    void *context;
    int (*ready)(void *context);                         /* 1 when record 1 is built */
    int (*find)(void *context, const char *name);        /* record 1..31 with that name, else -1 */
    int (*is_free)(void *context, int record);           /* 1 when record (1..30) holds no source */
    int (*set_source)(void *context, int record, const char *name, const char *path);
                                                         /* flavor 2, type 2: 1 registered, 0 refused, -1 faulted */
    int (*adopt_cache)(void *context, int record);       /* point the record at the material-format cache: 1 done */
    int (*release)(void *context, int record);           /* synchronise, then Release: 1 released */
    void *(*record)(void *context, int record);          /* the record's address */
    int (*rebind)(void *context, void *material, int record, const char *name);
                                                         /* bind a material again to the record: 1 done */
    void (*notice)(void *context, const char *text);    /* tell the player (a board found no record) */
} sh_modvt_ops;

typedef struct sh_modvt_result {
    int wanted;        /* boards the map serves */
    int registered;    /* registered by this sync */
    int kept;          /* already registered, unchanged */
    int released;      /* released by this sync */
    int pending;       /* no longer served (or another build), released at the next frame boundary */
    int refused;       /* not registered: name taken, table full, or SetSource refused */
    int full;          /* of them, because records 1..30 were in use */
    int lost;          /* records the engine no longer held under the board's name */
    int rebound;       /* materials bound again to a board registered again */
    int not_ready;     /* 1 when the VT system was not up and nothing was done */
} sh_modvt_result;

/* Bring the boards in line with `wanted`: verify the records held, release
 * what is no longer served (only when release_now; otherwise counted as
 * pending), then register every wanted board without a record, point it at
 * the material-format cache and bind its earlier materials again. Logs one
 * VT: line per change and per new refusal. Returns the number of boards
 * afterwards, or -1 when the VT system is not ready (nothing is done). */
int sh_modvt_sync(const sh_modvt_family *wanted, int n, const sh_modvt_ops *ops, const char *phase, int release_now,
                  sh_modvt_result *out);

/* The board name that owns record k (1..31), or NULL. */
const char *sh_modvt_record_owner(int k);

/* Boards held now. */
int sh_modvt_owned_count(void);

/* Forget every board and every remembered material (tests only). */
void sh_modvt_reset(void);

/* The player's sentence when a board found no free record (under 256 bytes). */
void sh_modvt_full_notice(char *out, size_t cap, const char *mod);

/* ------------------------------------------------------------------ the two detours */

/* VirtualTextureForMaterial: the board record for a material whose
 * landpagefile is `name`, else NULL (the engine's own lookup runs). */
void *sh_modvt_board_record(const char *name);

/* What the binder addition calls (production: the engine's own feedback
 * function and parameter setters; tests: a fake). */
typedef struct sh_modvt_bind_ops {
    float *(*feedback)(void *record, float *out4);                  /* the record's feedback word */
    void (*set_vec4)(void *parms, void *handle, const float *v4);
    void (*set_texture)(void *parms, void *handle, void *image);
    void *feedback_handle;                                          /* renderParm vmtrFeedbackFloat */
    void *pagetable_handle;                                         /* renderParm vmtrPageTableMap */
} sh_modvt_bind_ops;

/* After the engine's binder wrote `parms` (a material's parameter set) for
 * `record`: when the record is a board's, also set vmtrFeedbackFloat and
 * vmtrPageTableMap and remember the material for a later rebind. Returns 1
 * when it added them, 0 for every other record (nothing is touched). */
int sh_modvt_bind_board(void *parms, void *record, const sh_modvt_bind_ops *ops);

/* Materials remembered for a board name. */
int sh_modvt_material_count(const char *name);

/* ------------------------------------------------------------------ decoding */

/* What modvt reads out of the signed functions. RVAs are relative to the
 * image base. */
typedef struct sh_modvt_layout {
    uint32_t sys_rva;          /* the VT system (LEA RCX at the SetSource head) */
    uint32_t sync_rva;         /* the VT system's job synchronisation, called there first */
    uint32_t release_rva;      /* Release(record), called there next */
    uint32_t table_off;        /* record pointer table, from the VT system */
} sh_modvt_layout;

/* "48 8D 0D d32 E8 r32 48 8B CE E8 r32" in code[0..len): offset of the LEA, else -1. */
int sh_modvt_find_setsource_head(const uint8_t *code, size_t len);

/* "49 8B 85 d32 E9" in code[0..len): the disp32 (table + 8), else 0. */
uint32_t sh_modvt_find_table_disp(const uint8_t *code, size_t len);

/* Release's head: MOV RAX,RSP; PUSH RBP, and CMP DWORD [RCX+0x2C0],0 in its
 * first len bytes. */
int sh_modvt_release_head_ok(const uint8_t *code, size_t len);

/* Decode the layout from SetSource's first bytes (at ss_rva) and
 * VirtualTextureForMaterial's (at vf_rva). Returns 1 when every field was
 * found (the caller still checks Release with sh_modvt_release_head_ok),
 * else 0. */
int sh_modvt_decode_layout(const uint8_t *ss, size_t ss_len, uint32_t ss_rva, const uint8_t *vf, size_t vf_len,
                           uint32_t vf_rva, sh_modvt_layout *out);

/* The material binder (VtMaterialBind, at least 0xC0 bytes at rva): the
 * feedback function, the vec4 setter and the texture setter it calls. */
typedef struct sh_modvt_binder {
    uint32_t feedback_rva;
    uint32_t set_vec4_rva;
    uint32_t set_texture_rva;
} sh_modvt_binder;
int sh_modvt_decode_binder(const uint8_t *code, size_t len, uint32_t rva, sh_modvt_binder *out);

/* The material rebind (VtMaterialRebind, at least 0x68 bytes at rva): the VT
 * system it passes, VirtualTextureForMaterial and the binder it calls, the
 * parameter-block copy it ends with and the VT-off flag it tests. */
typedef struct sh_modvt_rebind {
    uint32_t sys_rva;
    uint32_t vf_rva;
    uint32_t binder_rva;
    uint32_t copy_rva;
    uint32_t vt_off_rva;
} sh_modvt_rebind;
int sh_modvt_decode_rebind(const uint8_t *code, size_t len, uint32_t rva, sh_modvt_rebind *out);

/* The per-view setter's two sites (VtViewPageTable, VtViewFeedback): the
 * globals holding the renderParm handles it sets from record 1, record 1's
 * page-table slot and record 1 itself, and the feedback function it calls. */
typedef struct sh_modvt_view {
    uint32_t pagetable_handle_rva;
    uint32_t pagetable_src_rva;     /* record 1 + 0x3E8 */
    uint32_t feedback_handle_rva;
    uint32_t feedback_rec_rva;      /* record 1 */
    uint32_t feedback_fn_rva;
} sh_modvt_view;
int sh_modvt_decode_view(const uint8_t *pt, size_t pt_len, uint32_t pt_rva, const uint8_t *fb, size_t fb_len,
                         uint32_t fb_rva, sh_modvt_view *out);

/* JobsWaitAll (idParallelJobManagerLocal's wait for every job list, at least
 * 0x40 bytes at rva): the job manager it waits on (its LEA RCX at +0x2E), or 0. */
uint32_t sh_modvt_decode_jobs(const uint8_t *code, size_t len, uint32_t rva);

/* VtFrameCall (EndFrame's CALL [RAX+0x80]; LEA RCX,[sys]; CALL update, at
 * least 0x16 bytes at rva): the VT system it passes and the frame update it
 * calls. Returns 1 when both were found. */
int sh_modvt_decode_frame_call(const uint8_t *code, size_t len, uint32_t rva, uint32_t *sys_rva, uint32_t *update_rva);

/* The frame update's prologue: MOV [RSP+8],RBX; MOV [RSP+0x10],RSI; PUSH RDI;
 * SUB RSP,0x50, the 15 whole position-independent bytes the detour takes. */
int sh_modvt_frame_head_ok(const uint8_t *code, size_t len);

/* ------------------------------------------------------------------ engine */
#ifndef SH_MODVT_PURE
#include "signatures.h"

/* Bind VtSetSource, VtForMaterial, VtMaterialBind, VtMaterialRebind,
 * VtViewPageTable, VtViewFeedback, JobsWaitAll and VtFrameCall, what is
 * decoded from them,
 * the process-heap scope and the main-thread identity;
 * install the two detours; listen to the serving index. Logs one VT: line.
 * Returns 1 when boards can be registered; 0 leaves every engine site as
 * shipped. */
int sh_modvt_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Called by modserve_engine.c before every decl registration pass (editor
 * palette build, map activation), on the main thread: registers boards. */
void sh_modvt_before_pass(const char *phase);

/* One status line through sh_printf (sh_mods status, sh_vt status). */
void sh_modvt_print_status(void);

/* sh_vt sync: run the sync now (console, main thread, releases included), then the status line. */
void sh_modvt_console_sync(void);

/* The bound engine surface, for the sh_vt diagnostic (vt_harness.c). */
int      sh_modvt_engine_bound(void);
int      sh_modvt_engine_vulkan(void);              /* 1 Vulkan, 0 OpenGL, -1 unknown */
int      sh_modvt_engine_ready(void);
uint8_t *sh_modvt_engine_record(int k);             /* record k (0..31) or NULL */
uint8_t *sh_modvt_engine_sys(void);
uint32_t sh_modvt_engine_table(void);
int      sh_modvt_engine_on_main(void);             /* 1 main thread, 0 not, -1 unknown */
/* SetSource / Release inside process heap 0 under SEH: 1 done (for SetSource
 * *result holds its return), 0 refused before the call, -1 faulted (*fault
 * holds the exception code). Main thread only. Release synchronises the VT
 * system's jobs first. */
int      sh_modvt_engine_set_source(int k, const char *name, const char *path, int flavor, int *result,
                                    double *ms, unsigned *fault);
int      sh_modvt_engine_release(int k, unsigned *fault);
#endif

#ifdef __cplusplus
}
#endif

#endif
