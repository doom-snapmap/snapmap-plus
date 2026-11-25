/* modauthor.h -- the mod folder an author writes, and the engine names it stands for (docs/architecture.md, "The mod
 * folder"). Pure text and path work. A name beginning with one of the mod's folders is the mod's (`materials/body`
 * -> `mods/<salt>/body`), every other name is DOOM's, and `native:<name>` names DOOM's own where it begins like
 * a mod folder. */
#ifndef SNAPMAP_PLUS_MODAUTHOR_H
#define SNAPMAP_PLUS_MODAUTHOR_H

#include <stddef.h>

/* The folder that holds DOOM's own files the mod changes, at DOOM's own paths. */
#define SH_MA_NATIVE "native/"

/* The engine request path of the author path `path` (relative to the mod folder, any case) of the mod with `salt`.
 * Returns 1 with *request (malloc'd, lower case); 0 with *cause (malloc'd sentence) when no folder takes the path,
 * or 0 with *cause NULL for a file beside mod.json. */
int sh_ma_request(const char *salt, const char *path, char **request, char **cause);

/* The author path of an engine request of the mod with `salt` (the inverse of sh_ma_request), malloc'd; a request
 * of DOOM's own is native/<request>. NULL only when out of memory. */
char *sh_ma_path(const char *salt, const char *request);

/* 1 when the top-level folder `name` (no '/', any case) is one of the mod folder's own folders. */
int sh_ma_folder(const char *name, size_t n);

/* The engine name for the name at `name` (n bytes) when it begins with a folder of the mod ("materials/body" ->
 * "mods/<salt>/body") or with native: ("native:art/x" -> "art/x"); malloc'd. NULL when the name is the game's as written
 * (or out of memory). */
char *sh_ma_engine_name(const char *salt, const char *name, size_t n);

/* Whether `key` (n bytes) is one of the mod's own strings keys: the caller's list (sh_ma_string_keys). */
typedef int (*sh_ma_key_fn)(void *ctx, const char *key, size_t n);
/* Whether `key` is one of the game's own strings keys (strkeys.h sh_strkey_is_edit). */
typedef int (*sh_ma_game_key_fn)(const char *key, size_t n);

/* Rewrites a decl text (or any text but a script) with every mod name as the engine names it: folder names,
 * native:<name>, and #str_<key> for a key own_key answers. Returns the new malloc'd text (*out_len its length),
 * or NULL when nothing changes (or out of memory: *out_len is then (size_t)-1). */
char *sh_ma_text(const char *salt, const char *text, size_t len, sh_ma_key_fn own_key, void *ctx, size_t *out_len);

/* The author's part of mod.json as the engine names it: sh_ma_text on every string, and every key of a `strings`
 * language object that is not one of the game's (is_game_key) written <salt>_<key>. Same return as sh_ma_text. */
char *sh_ma_json(const char *salt, const char *text, size_t len, sh_ma_game_key_fn is_game_key, size_t *out_len);

/* sh_ma_json's strings keys back to the author's spelling (<salt>_<key> -> <key>); same return as sh_ma_text. */
char *sh_ma_json_author_keys(const char *salt, const char *text, size_t len, size_t *out_len);

/* The mod's own strings keys in mod.json's author text (every key of a `strings` language object that is_game_key
 * does not claim), sorted, without duplicates: *keys malloc'd array of malloc'd strings, *count. 1, or 0 when out of
 * memory. Free with sh_ma_keys_free. */
int sh_ma_string_keys(const char *text, size_t len, sh_ma_game_key_fn is_game_key, char ***keys, size_t *count);
void sh_ma_keys_free(char **keys, size_t count);
/* own_key for sh_ma_text over a sorted key list; ctx is an sh_ma_keys. */
typedef struct sh_ma_keys { char **keys; size_t count; } sh_ma_keys;
int sh_ma_keys_has(void *ctx, const char *key, size_t n);

/* The refusal sentence (malloc'd; NULL out of memory) for a file at `path` that is in none of the mod's folders:
 * `dooms` 1 when DOOM holds a file at that path (move it to native/ or delete it), 0 to list the folders. */
char *sh_ma_outside_cause(const char *path, int dooms);
/* The sentence listing the folders, each with an example. */
const char *sh_ma_kind_list(void);

/* The author path an engine path stands for as one of the mod's own files (no salt in it: "generated/decls/entitydef/
 * ai/x.decl" -> "entities/ai/x.decl"), for saying where a file belongs; NULL when no folder takes it (or out of
 * memory). */
char *sh_ma_kind_path(const char *request);

#endif /* SNAPMAP_PLUS_MODAUTHOR_H */