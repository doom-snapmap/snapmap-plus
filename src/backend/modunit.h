/* modunit.h -- the unit: the closure of one placed object inside one mod build (docs/architecture.md, "Units").
 *
 * A unit is everything one placed object needs from the mod it comes from, as the build recorded it: the files it
 * carries, by path, size and SHA-256, the DOOM and product files it uses (pointers, never carried), and its share of
 * the author's mod.json. It is named by the SHA-256 of its canonical manifest, so it never changes, and the same
 * object from two builds whose closure did not change is the same unit, whatever else the pack around it holds.
 * Nothing in it names the mod's salt, name, author or description: one unit serves under any namespace.
 *
 * The root (sh_unit_of) is a name a map places: an entityDef the mod adds (directly placed, or the def a Toybox row
 * places), a Toybox row, a module, or SH_UNIT_WHOLE for a map-level row (the mod used as a whole). Nothing enumerates
 * a mod's objects ahead of time: a unit is computed when a map that places its root leaves the machine.
 *
 * The closure, from the root:
 *   seeds    the root's file; for an entityDef, the mod's Toybox rows that place it; every DOOM file the mod changes
 *            that is not an editor tweak (edits and replacements apply wherever the mod is used), and every DOOM file
 *            its folder holds unchanged (a surface, kept as a pointer: wherever the mod is used the engine finds it,
 *            whatever names it, a DOOM file included), whose edges never
 *            reach the mod's own entityDefs (such a file, the conductor's list, names every object; each comes from
 *            its own root); the md6Defs mod.json's animation_events and attachment_tags name (the product adds them
 *            to the player of every map that uses the mod); every decl of a type the game finds by enumeration
 *            (sh_unit_enumerated_type) that is tied to none of the mod's entityDefs, and, once one of them is kept,
 *            the enumerated decls tied to it: by a need, or by its AI type (aiType, memberType), which a DOOM file
 *            the mod changes pairs with the def in one block; the requests players asked for and did not get
 *            (sh_unit_env.asked) that the build holds, kept with the unit that holds the decl that asked (`by`), or
 *            with every unit when no decl is named
 *   edges    every need the build recorded for a kept file; every name a kept text spells under the mod's namespace
 *            (a decl read as the engine reads it, with its names in the author's spelling mapped to the engine's,
 *            so the prefix rule of modauthor.h decides) and every name a kept binary spells under the namespace it
 *            was written with (the build's "spelled"); a name reaches the file of that name with its extensions cut
 *            (so an image's every extension) and the file of each of its leading folders (a node of an animWeb names
 *            the animWeb)
 *   whole    once reached: a sound bank for each kept sound decl's event (the one bank that holds it, or, when
 *            several do, one of them unless a kept bank already holds it), with the sound decls of all its events (the
 *            serving index names a bank's events by the decls beside it); a render program with its
 *            permutations, cooked binaries and SPIR-V; a module's every file below its own name; a model with its
 *            mesh and skeleton (the build's needs already say so)
 *   scripts  an entry script that binds a kept entityDef or names one in a string literal (one that binds a name
 *            only while it runs, or names none of the mod's defs, once any entityDef is kept); every module a kept
 *            script requires; every string literal of a kept script as a name of the
 *            mod (in the author's spelling, the engine's, mod-relative, or after a decl type's folder) and, when it
 *            ends in '/', '_', '-' or '.', as a prefix of names
 *   boards   a virtual-texture family is cut at the square: a material that names its family by landpagefile keeps
 *            the squares (_sq<N>.mega2) its virtualmapping covers; a material whose mapping cannot be read keeps the
 *            whole family
 *   pointers every DOOM or product file a kept file's needs reach that the build does not carry (sh_unit_env.game
 *            says which archive holds it and its SHA-256), and every pointer of the build a kept file reaches; a
 *            pointer is never carried and its own needs are DOOM's
 * There is no size rule: a unit is as big as its closure.
 *
 * The canonical manifest (SPEC-distribution 3.4): keys sorted by their UTF-8 bytes, no whitespace, integers plain:
 *   {"files":[{"path","sha256","size"}...],"format":1,"game":[{"at"?,"path","sha256","source"}...],
 *    "mod":{...},"root":{"kind","name"},"spelled"?:"<namespace>"}
 * files and game sorted by path; a game entry's "at" is the mod's own path where the build holds DOOM's bytes at a
 * name of its own (a borrow), "source" the archive that holds it ("gameresources", "snap_gameresources", "loose") or
 * "product"; "mod" the author's mod.json keys the player's build reads: requirements whole, and strings, hud,
 * companions, scenes, animation_events and attachment_tags cut to the entries the kept files name (any other key,
 * "version" included, changes nothing served and is left out, so it never moves the hash); "spelled" the
 * namespace the unit's carried binaries spell, when they spell one.
 *
 * The proof (sh_unit_of): every carried file is the build's own bytes (each is read and hashed against the build),
 * and every need a kept file resolves inside the build resolves inside the unit. A unit that fails either is the
 * builder's defect, never the author's: the caller sends the whole build for that row and logs the reason.
 *
 * Pure file and text work: no engine calls. Any thread. */
#ifndef SNAPMAP_PLUS_MODUNIT_H
#define SNAPMAP_PLUS_MODUNIT_H

#include <stddef.h>
#include "modbundle.h"

#define SH_UNIT_FORMAT 1
#define SH_UNIT_WHOLE  "*"

/* Root kinds, as the manifest's root.kind spells them: "entity", "toybox", "module", "mod". */
#define SH_UNIT_ENTITY 1
#define SH_UNIT_TOYBOX 2
#define SH_UNIT_MODULE 3
#define SH_UNIT_MOD    4

/* A request players of a map asked for and did not get (the store's asked list, 3.8), and the decl whose load asked ("" or
 * NULL when not known); either may name the namespace it was served under: the walk reads any mods/<x>/ as the
 * build's own. */
typedef struct sh_unit_asked { const char *request, *by; } sh_unit_asked;

/* Whether DOOM or the product holds `request` (lower case): 1 with its SHA-256 and where it is ("gameresources",
 * "snap_gameresources", "loose" or "product") in `source`; 0 when neither holds it; -1 when the game's files
 * cannot be read. */
typedef int (*sh_unit_game_fn)(void *context, const char *request, char sha256[SH_MB_HASH_HEX + 1], char *source,
                               size_t cap);

typedef struct sh_unit_env {
    sh_unit_game_fn game;        /* required */
    void *game_context;
    const sh_unit_asked *asked;  /* the mod's asked requests, or NULL */
    size_t asked_count;
} sh_unit_env;

typedef struct sh_unit {
    int kind;                    /* SH_UNIT_* */
    char *root;                  /* the root's author name (entities/tyrant), SH_UNIT_WHOLE for the mod */
    char hash[SH_MB_HASH_HEX + 1];   /* SHA-256 of the manifest: the unit's identity */
    char *manifest;              /* the canonical bytes, NUL-terminated */
    size_t manifest_len;
    size_t files, pointers;      /* carried files; game and product pointers */
    unsigned long long bytes;    /* carried bytes */
    unsigned char *keep;         /* per file of the bundle the unit was made from: 1 carried or pointed to */
    long *from;                  /* per kept file: the file whose edge brought it in, -1 for a seed (why a file is in) */
    char spelled[SH_MB_ID_MAX + 1];
} sh_unit;

/* The unit of `name` in `b` (a built mod, loaded or just built, whose files are in `folder`): `name` is a placed
 * name as a map's text writes it (mods/<salt>/x, or a module's request), an author name (entities/x), or
 * SH_UNIT_WHOLE. 1 with *out filled (release with sh_unit_free); 0 with the reason in *why (malloc'd): the name is
 * none of the mod's objects, a file changed since the build, a proof failed, the game's files cannot be read, or
 * memory ran out. */
int  sh_unit_of(const sh_mb_bundle *b, const char *folder, const char *name, const sh_unit_env *env, sh_unit *out,
                char **why);
void sh_unit_free(sh_unit *u);

/* Write the unit's manifest at <content root>\units\<hash>.json (a file already there with those bytes is kept).
 * The content root is the mods folder's .content (SPEC-distribution 2.11). 1 on success; 0 with *why. */
int  sh_unit_store(const sh_unit *u, const char *content_root, char **why);

/* The kept files' bytes for a composition: the local path of the unit file at `path` (the mod's own path), malloc'd,
 * or NULL when it is not here. */
typedef char *(*sh_unit_source_fn)(void *context, const char *path);

/* A composition (SPEC-distribution 2.13): a mod folder made at `dest` (it must not exist) from the units whose
 * manifests are given, under the namespace `salt` and the display name `name`: every carried file of every unit,
 * each checked against its SHA-256 as it is copied (a path two units carry must be one file), a mod.json holding
 * `name` and the units' mod shares merged, built with `resolver` (installed mods are never consulted) and written
 * with the block's "units_of" naming the units. 1 with the build in *out (release with sh_mb_free); 0 with *why
 * and `dest` removed. */
int  sh_unit_compose(const char *const *manifests, const size_t *lens, size_t count, sh_unit_source_fn source,
                     void *source_context, const char *salt, const char *name, const sh_mb_resolver *resolver,
                     const char *dest, sh_mb_bundle *out, char **why);

/* A file of no unit that a composition carries (a stand-in for an absent object, SPEC-distribution D60), at the
 * mod's own path; written only where no unit puts a file. */
typedef struct sh_unit_extra { const char *path; const char *text; size_t len; } sh_unit_extra;

/* sh_unit_compose with `extras` and, when `link` is 1, each unit file hard-linked from its source where the volume
 * allows (a copy otherwise); every file is checked against its SHA-256 either way. */
int  sh_unit_compose_ex(const char *const *manifests, const size_t *lens, size_t count, sh_unit_source_fn source,
                        void *source_context, const sh_unit_extra *extras, size_t extra_count, int link,
                        const char *salt, const char *name, const sh_mb_resolver *resolver, const char *dest,
                        sh_mb_bundle *out, char **why);

/* 1 when `type` (a decl type, any case) is one the game finds by enumeration, never by a name a file spells (the
 * conductor numbers the AI scores and encounters). */
int  sh_unit_enumerated_type(const char *type);

/* The name key of the n bytes at s under mods/<ns>/: lower case, '\' as '/', cut at '$', every character but
 * [a-z0-9/._] as '_', the last segment's extensions cut, written under mods/<salt>/ (salt NULL: ns). malloc'd, or
 * NULL when no name under mods/<ns>/ starts there. */
char *sh_unit_key(const char *s, size_t n, const char *ns, const char *salt);

#endif /* SNAPMAP_PLUS_MODUNIT_H */
