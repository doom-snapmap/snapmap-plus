/* modbundle_old.h -- read a mod folder of an earlier shape (store tool only).
 *
 * The game reads only the one shape (modbundle.h: mod.json format 2 with a
 * salt) and refuses every other with a plain reason. The installer's store
 * tool converts what exists once, at update; this is its reader of what an
 * earlier product wrote or accepted:
 *
 *   - mod.json with a format-1 product block: {"format": 1, "key", "build",
 *     "bundle", "built", "validated", "cause", "origin", "legacy", "rules",
 *     "shadows", "files", "stock" [{..., "inAssets", "closure"}]}
 *   - mod.json without a block, or package.json (a folder never built)
 *   - the retired author keys: id, key, shadows, stock, scope, player,
 *     extends, replaces, legacy, composes, schema (and a hud "schema")
 *
 * Linked only into the store tool (src\store\build.ps1), never into the
 * game DLL. Strings are kept as their JSON text (escaped bodies inside
 * quotes, or whole values), so a converter splices them back unchanged. */
#ifndef SNAPMAP_PLUS_MODBUNDLE_OLD_H
#define SNAPMAP_PLUS_MODBUNDLE_OLD_H

#include <stddef.h>

#define SH_MB_OLD_KEY_FROM_ID     1   /* the author block's "id" */
#define SH_MB_OLD_KEY_FROM_BLOCK  2   /* the product block's "key" */
#define SH_MB_OLD_KEY_FROM_FOLDER 3   /* the folder's own name (a folder never built) */

typedef struct sh_mb_old_file {
    char *path;                 /* "assets/..." as recorded (or as walked when there is no block) */
    char  sha256[65];           /* "" when walked */
    unsigned long long size;    /* carried size; 0 for a pointer */
    char *source;               /* a pointer's installed path, else NULL */
    int   pointer;              /* listed under the block's "stock" */
    int   absent;               /* "inAssets": false: no file under assets/ */
    int   closure;              /* "closure": true */
} sh_mb_old_file;

typedef struct sh_mb_old {
    char  descriptor[16];       /* "mod.json" or "package.json" */
    char  key[65];              /* the old key, normalized (sh_mb_id_normalize); "" when none is valid */
    int   key_from;             /* SH_MB_OLD_KEY_FROM_*, 0 when none */
    int   block_format;         /* the block's "format", 0 when there is no block */
    /* Author block, as JSON value texts (NULL when absent). */
    char *name, *version, *author, *homepage, *requirements;
    char *kept;                 /* JSON object text of every other author key that stays (strings, hud, description,
                                   companions, ...), in the author's order; "{}" when none */
    char *retired;              /* JSON object text of the retired author keys present, "{}" when none */
    /* Product block (format 1). */
    char  bundle[65];           /* "bundle" hash, "" when absent */
    unsigned long long build;   /* integer counter, 0 when absent */
    long long built;
    int   rules;
    int   validated;            /* 1 when absent or true */
    char  origin[16];           /* "" for by-hand */
    char *block_legacy;         /* the block's "legacy" object text, or NULL */
    char *block_shadows;        /* the block's "shadows" array text, or NULL */
    sh_mb_old_file *files;      /* the block's files then stock; without a block, the walked assets/ files */
    size_t file_count;
    char  cause[512];           /* why sh_mb_old_load returned 0 */
} sh_mb_old;

/* Read `folder` (UTF-8). Returns 1 when its descriptor parsed as a JSON
 * object (whatever its shape); 0 with out->cause otherwise. out is always
 * initialised; release with sh_mb_old_free. */
int  sh_mb_old_load(const char *folder, sh_mb_old *out);
void sh_mb_old_free(sh_mb_old *old);

/* 1 when the folder's descriptor is an earlier shape the store tool converts:
 * package.json, a block without a valid "salt" (format 1), or a retired author
 * key. 0 for the current shape or a folder without a descriptor. */
int  sh_mb_old_is_old(const char *folder);

#endif /* SNAPMAP_PLUS_MODBUNDLE_OLD_H */
