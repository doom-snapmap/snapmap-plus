/* modunit.h -- the unit: the closure of one placed object inside one mod build (docs/architecture.md, "Units").
 * A unit carries files by path, size and SHA-256, points to the DOOM and product files it uses, and holds its
 * share of mod.json; the SHA-256 of its canonical manifest (sorted keys, no whitespace) names it, so one unit
 * serves under any namespace. The closure grows from the root (an entityDef, Toybox row, module, or
 * SH_UNIT_WHOLE) through the build's needs, spelled names, script strings and whole-asset rules, with no size
 * rule. Every carried file is the build's own bytes and every need resolves inside the unit; pure file work, any thread. */
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

/* A request players of a map asked for and did not get, and the decl whose load asked ("" or NULL when not
 * known); either may name the namespace it was served under (any mods/<x>/ is read as the build's own). */
typedef struct sh_unit_asked { const char *request, *by; } sh_unit_asked;

/* 1 when DOOM or the product holds `request` (lower case), with its SHA-256 and its archive ("gameresources",
 * "snap_gameresources", "loose" or "product") in `source`; 0 when neither does; -1 when the game's files cannot be read. */
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
    int absent_root;             /* failed lookup: the built mod has no file providing the selected name */
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

/* The unit of `name` (a placed name, an author name, or SH_UNIT_WHOLE) in `b`, whose files are in `folder`:
 * 1 with *out filled (sh_unit_free), or 0 with the reason in *why (malloc'd): not the mod's object, a file
 * changed since the build, a proof failed, unreadable game files, or out of memory. */
int  sh_unit_of(const sh_mb_bundle *b, const char *folder, const char *name, const sh_unit_env *env, sh_unit *out,
                char **why);
void sh_unit_free(sh_unit *u);

/* Write the unit's manifest at <content root>\units\<hash>.json (a file already there with those bytes is kept).
 * The content root is the profile's cache/content folder (SPEC-distribution 2.11). 1 on success; 0 with *why. */
int  sh_unit_store(const sh_unit *u, const char *content_root, char **why);

/* The kept files' bytes for a composition: the local path of the unit file at `path` (the mod's own path), malloc'd,
 * or NULL when it is not here. */
typedef char *(*sh_unit_source_fn)(void *context, const char *path);

/* A composition: a mod folder made at `dest` (it must not exist) from the given unit manifests under namespace
 * `salt` and name `name`, each file checked against its SHA-256 as copied. 1 with the build in *out
 * (sh_mb_free); 0 with *why and `dest` removed. */
int  sh_unit_compose(const char *const *manifests, const size_t *lens, size_t count, sh_unit_source_fn source,
                     void *source_context, const char *salt, const char *name, const sh_mb_resolver *resolver,
                     const char *dest, sh_mb_bundle *out, char **why);

/* A file of no unit that a composition carries (a stand-in for an absent object), at the mod's own path;
 * written only where no unit puts a file. */
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
