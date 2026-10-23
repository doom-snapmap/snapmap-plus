/* Native owners of copied material sizes (R-309, R-321).
 *
 * The material size getters (VK 0x140d75b40 / 0x140d75d40, GL 0x140d75db0 / 0x140d75fb0) are the route every
 * found owner uses to copy an image's size. An in-place image rebuild keeps every holder's pointer valid but not
 * those copies, so each getter call is attributed to its caller through a table of the 14 direct callers' call
 * sites per pinned executable, verified byte for byte at install:
 *
 *   transient    SWF shape draw, renderer debug tools, this module's diagnostics: the value is used per call;
 *   sprite       sprite SetMaterial and the script material setter: copied into the sprite (+0x68 / +0x6a);
 *   forward      the MP medal passes the sizes to its next SetMaterial as explicit axes;
 *   widget       codex content updates (Snap and Dossier vtable slot 3): menu layout from the sizes;
 *   credits      the credits item layout;
 *   hotBox       idRenderModelHotBox geometry;
 *   font         FontRead's fontInfo copy.
 *
 * Each owner type has a same-pass contract: derived sprite axes are recomputed through the getters, keeping
 * explicit axes and script writes; a codex widget's own update re-runs its layout; hotBox geometry is returned to
 * its constructed state so the native update rebuilds it; a font is rebuilt by the resident pass. Credits layouts
 * and script reads of a derived axis have no native refresh, so a rebuild of the image they copied waits until
 * their owner is destroyed. Records retire with their owners: a sprite, hotBox or menu widget destructor, or the
 * movie's destruction. A read that no owner takes stays with the image's history (ESCAPED).
 *
 * In-flight attribution belongs to an execution context, not an OS thread: DOOM runs SuperScript on Win32 fibers,
 * so a routine can be suspended inside a bracket while another fiber runs on the same thread, and resume on a
 * different thread. Brackets, the getter's read collection and pending forward reads are recorded per execution
 * context (execution_context.h) in this module's memory. When a context retires (at the first lookup after its fiber
 * is deleted or its thread exits), its open brackets end as routines that raised and its pending reads escape; its
 * stack is never read. */
#ifndef SH_SIZE_OWNERS_H
#define SH_SIZE_OWNERS_H
#include <stddef.h>
#include <stdint.h>

enum { SH_SIZE_AXIS_WIDTH = 1, SH_SIZE_AXIS_HEIGHT = 2 };
typedef enum sh_size_site {
    SH_SIZE_SITE_UNKNOWN = 0,
    SH_SIZE_SITE_TRANSIENT,
    SH_SIZE_SITE_SPRITE,
    SH_SIZE_SITE_FORWARD,
    SH_SIZE_SITE_WIDGET,
    SH_SIZE_SITE_CREDITS,
    SH_SIZE_SITE_HOTBOX,
    SH_SIZE_SITE_FONT
} sh_size_site;
typedef enum sh_size_bracket_kind {
    SH_SIZE_BRACKET_SPRITE = 1,     /* SetMaterial(sprite, name, width, height) */
    SH_SIZE_BRACKET_SCRIPT,         /* script material setter: the sprite is the script object's +0xc0 */
    SH_SIZE_BRACKET_FONT,           /* FontRead: owner identity "font" + name */
    SH_SIZE_BRACKET_HOTBOX,         /* hotBox geometry build */
    SH_SIZE_BRACKET_WIDGET,         /* codex content update (vtable slot 3) */
    SH_SIZE_BRACKET_CREDITS,        /* credits item layout: the item sprite */
    SH_SIZE_BRACKET_COLLECT         /* one native getter call: the images it reads */
} sh_size_bracket_kind;
typedef struct sh_size_read { char *image; void *material; unsigned axis; } sh_size_read;
/* A handle to an open bracket; the bracket's record (its reads and what its end needs) is this module's. */
typedef struct sh_size_bracket {
    uint64_t context;               /* the execution context that began it (0: not recorded) */
    uint64_t serial;                /* its record there; serials are never reused */
    sh_size_bracket_kind kind;
    void *object;
    unsigned axis;                  /* COLLECT: the getter's axis (object is its material) */
} sh_size_bracket;

/* One getter call measured again: its value and the images it read (owned by the caller). */
typedef int (*sh_size_measure)(void *material, unsigned axis, int *value, char ***images, size_t *count);

/* Bind the call-site table for this pinned executable: every listed site must be a direct call to the
 * resolved getter. own_base/own_size: this module's image, whose own calls are diagnostics. 0 leaves
 * attribution off: every read is then unattributed, as before. */
int sh_size_owners_bind_sites(const uint8_t *base, int vulkan, const void *height, const void *width,
    const void *own_base, size_t own_size);
/* Owner hooks installed and the measure route set: attribution and refresh are available. */
void sh_size_owners_enable(sh_size_measure measure);
/* hotBox update's saved-scale offset (three floats), decoded from the update body at install. */
void sh_size_owners_hotbox_layout(size_t saved_scale_offset);
int sh_size_owners_enabled(void);
sh_size_site sh_size_owners_site(const void *return_address);

/* Brackets around native copy routines, entered by their hooks on the calling execution context. A bracket that
 * cannot be recorded attributes nothing; a record that cannot be allocated stops all attribution (fail closed). */
void sh_size_owners_begin(sh_size_bracket *bracket, sh_size_bracket_kind kind, void *object, const char *owner,
    const void *frame, int width, int height);
/* completed 0: the routine raised; its reads stay unattributed and the owner's record is dropped. A bracket ends on
 * the context that began it; brackets begun above it and not ended end as routines that raised. */
void sh_size_owners_end(sh_size_bracket *bracket, int completed);
/* One getter call: the images it read (identified), the caller's return address and frame, the material
 * and the axis. 1 when an owner on this execution stack took every read; 0 leaves them to the caller
 * (ESCAPED). */
int sh_size_owners_read(const void *return_address, const void *frame, void *material, unsigned axis,
    char *const *images, size_t count);
/* The getter's read collection. Hooked finds report what they return while one is open on this stack
 * (sh_size_owners_note; NULL: an object that could not be identified, which fails the call). End attributes
 * the reads to the caller's owner, or hands them over when names is not NULL (the refresh's measurement);
 * 1 when every read was attributed or handed over. */
void sh_size_owners_collect_begin(sh_size_bracket *call, void *material, unsigned axis);
int sh_size_owners_collecting(void);
void sh_size_owners_note(const char *image);
int sh_size_owners_collect_end(sh_size_bracket *call, int completed, const void *return_address, const void *frame,
    char ***names, size_t *count);
/* Script native vars on a MovieClip object (its sprite at +0xc0): an explicit write of an axis (completed 0:
 * the setter raised, so the sprite's provenance is unknown), and a read of an axis into script state. */
void sh_size_owners_explicit(void *script_object, unsigned axis, int completed);
void sh_size_owners_script_read(void *script_object, unsigned axis);
/* Owner lifetimes. */
void sh_size_owners_sprite_destroyed(void *sprite);
void sh_size_owners_hotbox_destroyed(void *hotbox);
void sh_size_owners_widget_destroyed(void *widget);
void sh_size_owners_movie_destroyed(void *movie);

/* For images about to be rebuilt in place: the fonts that copied their sizes (each is rebuilt with them; the
 * caller frees the array and its names) and, when an owner's copy has no native refresh, 0 with that owner. */
int sh_size_owners_plan(const char *const *images, size_t count, char ***fonts, size_t *font_count,
    char *blocker, size_t capacity);
/* After those images reloaded: recompute derived sprite axes, re-run codex widget updates and return hotBox
 * geometry to its constructed state. 0 with the reason when a record cannot be refreshed safely.
 * A forward pass (restoring 0) journals every sprite value before writing it, and refuses while a failed
 * restoration still owns values. A restoring pass first writes the journaled values back (newest first, without
 * allocating): an entry ends restored, obsolete (its sprite or movie is gone, or the address holds another
 * object), preserved (another writer changed it after the forward pass; never overwritten) or stays pending
 * when its write fails, which fails the refresh and keeps the obligation for a retry. Then it refreshes the
 * sprites the journal does not cover. */
int sh_size_owners_refresh(const char *const *images, size_t count, int restoring, char *error, size_t capacity);
/* The pass that wrote the journal committed, or a restoration succeeded: its entries are settled. */
void sh_size_owners_journal_clear(void);
size_t sh_size_owners_journal_count(void);
size_t sh_size_owners_journal_pending(void);

size_t sh_size_owners_count(void);
#ifdef SH_SIZE_OWNERS_TESTING
void sh_size_owners_test_reset(void);
/* Tests bind a synthetic table: sites[i] return addresses with classes[i]. */
void sh_size_owners_test_sites(const void *const *returns, const sh_size_site *classes, size_t count);
/* Execution contexts with an open bracket or pending reads, and bracket records not yet released. */
size_t sh_size_owners_test_contexts(void);
size_t sh_size_owners_test_records(void);
#endif
#endif
