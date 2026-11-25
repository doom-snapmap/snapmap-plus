/* editor_delete.h -- delete editor entities through DOOM's own Delete Selection, on the main thread only.
 * The native selected-Delete action runs on exactly the requested entities under the native dispatcher's
 * gates, and releases every target before returning; the undo commit ("Delete Selection") and the return to
 * idle finish natively on later frames, and nothing is queued across frames. The engine's single-entity leaf is
 * not used: it leaves hover, the selected-ID array, capability bits, the undo commit and the idle transition undone. */

/* Every native call from the selection setup to the action runs inside a verified process-heap scope
 * (see editor_delete.c): a request whose scope cannot be verified is refused unchanged. */
#ifndef BACKEND_EDITOR_DELETE_H
#define BACKEND_EDITOR_DELETE_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Largest accepted request. */
#define SH_EDITOR_DELETE_MAX 8192

enum sh_editor_delete_status {
    SH_EDITOR_DELETE_DISPATCHED  =   1, /* native action released every target; its commit is pending */
    SH_EDITOR_DELETE_UNAVAILABLE =   0, /* an engine boundary did not resolve */
    SH_EDITOR_DELETE_NOT_MAIN    =  -1, /* called off the engine main thread */
    SH_EDITOR_DELETE_NOT_READY   =  -2, /* no editor/map/screen, changing state, or no heap scope */
    SH_EDITOR_DELETE_NOT_ENTITY  =  -3, /* the editor is not in EntityMode */
    SH_EDITOR_DELETE_BUSY        =  -4, /* see busy for the native owner of the selection */
    SH_EDITOR_DELETE_INVALID     =  -5, /* empty/oversized request, or a dead/out-of-range/built-in filter ID */
    SH_EDITOR_DELETE_RESTRICTED  =  -6, /* the editor's restriction set blocks Delete */
    SH_EDITOR_DELETE_PORTAL      =  -7, /* a target is a portal door/frame entity */
    SH_EDITOR_DELETE_PROTECTED   =  -8, /* native capabilities: not every target is deletable */
    SH_EDITOR_DELETE_REFERENCED  =  -9, /* native referenced-entity refusal */
    SH_EDITOR_DELETE_FAULT       = -10, /* an engine call faulted; see rolled_back */
    SH_EDITOR_DELETE_INCOMPLETE  = -11  /* the action returned without releasing every target */
};

enum sh_editor_delete_busy {
    SH_EDITOR_DELETE_BUSY_NONE = 0,
    SH_EDITOR_DELETE_BUSY_SUBSTATE,     /* EntityMode is neither idle nor selected */
    SH_EDITOR_DELETE_BUSY_TRANSITION,   /* a substate change from another substate is pending */
    SH_EDITOR_DELETE_BUSY_PHASE,        /* a native action is still waiting to commit */
    SH_EDITOR_DELETE_BUSY_QUEUED,       /* a native action is queued for the next frame */
    SH_EDITOR_DELETE_BUSY_MANIPULATION  /* the product manipulation guard refused */
};

typedef struct sh_editor_delete_result {
    int status;         /* sh_editor_delete_status, also the return value */
    int busy;           /* sh_editor_delete_busy detail when status is BUSY */
    int requested;      /* IDs supplied by the caller */
    int targets;        /* distinct valid targets */
    int released;       /* targets whose slot has no declaration after the action */
    int first_id;       /* first offending ID for INVALID/PORTAL, else -1 */
    int deselected;     /* selected non-targets dropped by a dispatch */
    int hover_cleared;  /* a non-target hover cleared before the action */
    int wire_cleared;   /* a hovered connection touching a target cleared */
    int rolled_back;    /* a refusal after selection setup restored the prior state */
} sh_editor_delete_result;

/* Native boundaries and the product guard: install() binds production addresses, tests bind models.
 * Byte returns follow the engine's al-only boolean convention. */
typedef struct sh_editor_delete_engine {
    uint8_t *editor;                                            /* inline editor object */
    void (*clear_selection)(void *selection);                   /* ClearSelection */
    unsigned char (*add_to_selection)(void *selection, uint32_t id);   /* AddToSelection */
    void (*clear_hover)(void *editor);                          /* decoded from the action */
    void (*refresh_capabilities)(void *substate, void *mode, void *editor);
    unsigned char (*blocked)(void *restrictions, uint32_t mask);       /* GridPropertiesBlocked */
    void (*delete_selection)(void *substate, void *mode, void *editor);
    int  (*on_main_thread)(void);        /* 1 main, 0 other, negative unknown; null skips */
    int  (*manipulation_in_progress)(void);  /* product selection guard; null skips */
    int  (*heap_enter)(void);   /* push the process heap; 1 only when the push is verified */
    int  (*heap_leave)(void);   /* pop what heap_enter pushed; 1 when the caller's scope is back */
    int  first_placed;          /* IDs below this are the map's built-in filters; never deleted */
} sh_editor_delete_engine;

/* The map's 56 built-in filters hold IDs 0..55 in both executables; the save's snapshot releases all
 * 56, so a map missing one faults the save and install() refuses them. */
#define SH_EDITOR_DELETE_FIRST_PLACED 56

/* Bind every boundary at once: 1 when all required callbacks (heap scope included) and the editor are
 * present, else unbind and return 0. Null unbinds. */
int sh_editor_delete_bind(const sh_editor_delete_engine *engine);

/* Resolve the delete, capability, blocked, selection and MemLocal heap-scope boundaries from shared
 * signature results, decode the hover-clear callee from the action, and bind. editor is the inline
 * editor object (sh_iface_engine_editor_base); manipulation_in_progress may be null. Returns 1 when bound. */
int sh_editor_delete_install(const sig_result *results, size_t n, const uint8_t *module_base,
                             const uint8_t *editor, int (*manipulation_in_progress)(void));

/* Delete ids through the native action; returns result.status. A refusal before the action leaves the
 * editor untouched except for a cleared hover; FAULT without rolled_back, and INCOMPLETE, can leave
 * partial changes (ABI contract in snapmap_plus_iface.h). */
int sh_editor_delete_batch(const int *ids, int count, sh_editor_delete_result *out);

/* 1 while a native editor action owns the selection (Selection waits to commit, idle is outside its
 * normal phase, or an action is queued); product selection and EntityMode writes must wait. */
int sh_editor_delete_native_pending(void);

/* Returns the callee of the single hover-clear call inside a native Delete Selection body when it lies
 * inside the image and has the expected shape, else 0. An adjacent twin shares its bytes in both
 * executables, so it is identified through this call, never by a scan. */
uintptr_t sh_editor_delete_decode_clear_hover(const uint8_t *action, const uint8_t *image,
                                              size_t image_size);

#endif /* BACKEND_EDITOR_DELETE_H */
