/* modslice.h -- the files of one mod build a published map needs.
 *
 * When a map is published, each mod its rows name is cut down to the files
 * that map needs, computed statically from the map's placed names and the
 * mod's build, and that cut ships in place of the whole build. Nobody sees a
 * new word for it: the row names the cut's build hash, and a player
 * downloads only those files into the map's own place.
 *
 * The cut is an ordinary built mod folder: mod.json with the source's author
 * part unchanged plus the author key "slice-of" (SH_MB_SLICE_KEY) naming the
 * source build's hash, the kept files under assets\, and, beside them,
 * source.json (SH_SLICE_SOURCE_FILE), the source build's canonical manifest,
 * whose SHA-256 is "slice-of": the mod store checks with it that every
 * carried file of the cut is a file of the source with the same hash and
 * size. "slice-of" is an author key, so it is inside the canonical manifest
 * and the build hash: two maps that need the same files of one build get one
 * hash and share one archive, and the key cannot be edited away without
 * changing the hash.
 *
 * The needed set (sh_slice_needed), deterministic and static:
 *   whole     the row is a map-level row (attached: the map uses the mod as a
 *             whole), the build carries scripts (sh_mb_sliceable: a script
 *             builds names while it runs), or nothing is placed
 *   always    every file at a game name that is not an editor tweak (the
 *             edited copies, whose edits apply per map, and a received
 *             build's replacements), and every decl of a type the game finds
 *             by enumeration rather than by name (SH_SLICE_ENUMERATED: the AI
 *             scores and encounters the conductor numbers)
 *   never     editor tweaks (files at a game name only the editor reads),
 *             and the closure pointers the build derives again
 *   seeds     every file a placed name names, every Toybox row
 *             (snapEditorEntityDef) of the mod that names one, and the
 *             requests players of an earlier cut asked for and did not get
 *             (`asked`, sh_slice_needed)
 *   walk      from each kept file (a stock pointer the folder holds, a borrow
 *             or a surface, is walked by its needs alone: its bytes are the
 *             game's): every need the build recorded for it
 *             (companions and scene spawns included), and every name its
 *             bytes spell under mods/<salt>/, text and binary alike (a decl's
 *             tokens, a mesh's materials, a movie's dictionary): a name
 *             reaches every file whose name, with the extensions of its last
 *             segment and anything after '$' cut and every character but
 *             [a-z0-9/._] read as '_', is that name or one of its leading
 *             folders (a node path inside an animWeb names the animWeb)
 *   units     a file that is reached brings its whole unit: every member of a
 *             virtual-texture family (the mod's texture board travels whole),
 *             every sound bank of the build once a sound decl is kept, every
 *             render program file once one is kept, and a module's every file
 *             below its own name
 *   fallback  a reached file that cannot be read, or one of a kind the walk
 *             does not know, ships the whole build
 * When the needed set holds every carried file but the editor tweaks, or its
 * files and the source's list of files (source.json) come to more than three
 * quarters of the whole build's carried bytes, the whole build ships: a
 * second archive of nearly the same size saves a player little.
 *
 * The cut (sh_slice_cut) stages the kept files into a folder, builds it as a
 * bundle against the game alone (no other mod), and proves it: the build
 * succeeds, every need a kept file resolved inside the source build resolves
 * inside the cut, and every carried file is the source's own bytes. Any
 * failure ships the whole build, with the reason.
 *
 * Pure file and text work; no engine calls. Any thread.
 */
#ifndef SNAPMAP_PLUS_MODSLICE_H
#define SNAPMAP_PLUS_MODSLICE_H

#include <stddef.h>
#include "modbundle.h"

/* Beside a cut's mod.json: the source build's canonical manifest. */
#define SH_SLICE_SOURCE_FILE SH_MB_SLICE_SOURCE

typedef struct sh_slice_plan {
    int whole;                       /* 1: the whole build ships (why says why) */
    char *why;                       /* malloc'd reason a whole build ships, or NULL */
    unsigned char *keep;             /* per file of the source bundle: 1 kept (NULL when whole) */
    size_t files, kept_files;        /* carried files: the source's, the kept */
    unsigned long long bytes, kept_bytes;   /* carried bytes: the source's, the kept */
} sh_slice_plan;

void sh_slice_plan_free(sh_slice_plan *plan);

/* The needed set of `src` (a built mod loaded from `folder`, its root folder)
 * for a map that places `names` (its manifest row's names; `attached` 1 for a
 * map-level row), plus `asked`: request paths players of an earlier cut asked
 * for (any case; NULL for none). Returns 1 with *plan filled (plan->whole set
 * when the whole build ships), 0 when memory runs out. */
int  sh_slice_needed(const sh_mb_bundle *src, const char *folder, const char *const *names, size_t name_count,
                     int attached, const char *const *asked, size_t asked_count, sh_slice_plan *plan);

/* Stage the plan's files of `src` (in `folder`) under `dest` (a folder that
 * does not exist yet, created), with the source's author part plus
 * "slice-of", and source.json; build it with `stock` (the game's archives;
 * the resolver's installed callback is never used) and prove it (above).
 * Returns 1 with the cut's build in *out (release with sh_mb_free) and its
 * mod.json written; 0 with the reason in *why (malloc'd), `dest` removed. */
int  sh_slice_cut(const sh_mb_bundle *src, const char *folder, const sh_slice_plan *plan, const char *dest,
                  const sh_mb_resolver *stock, sh_mb_bundle *out, char **why);

/* 1 when `type` (a decl type, any case) is one the game finds by
 * enumeration, never by a name a file spells. */
int  sh_slice_enumerated_type(const char *type);

/* The name key of the walk (above) of the n bytes at s for `salt`: malloc'd,
 * or NULL when they spell no name under mods/<salt>/. */
char *sh_slice_key(const char *s, size_t n, const char *salt);

#endif /* SNAPMAP_PLUS_MODSLICE_H */
