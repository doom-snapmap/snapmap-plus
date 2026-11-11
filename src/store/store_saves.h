/* store_saves.h -- the editor's saved maps (My Maps), for the store tool.
 *
 * Every saved map is a folder SNAPMAPS<20 hex>\ under an account folder of
 * %USERPROFILE%\Saved Games\id Software\DOOM\base\savegame.user\ (both
 * renderers share it), holding game.details and map.decl, each with a
 * 40-byte .verify sidecar. map.decl is a 4-byte header and a zlib stream of
 * the map text. A sidecar is an 8-byte salt and an HMAC-SHA256 tag over the
 * whole file, keyed by SHA-256(salt, U, "CYBERDEMON\0", the file's base name)
 * where U is the decimal SteamID64 the game binds saves to (the account
 * folder's name, or that id with its instance field cleared). The store tool
 * never guesses U: it rewrites a map only after the map's present sidecar
 * verifies with a candidate, and then signs the new bytes with that U and the
 * sidecar's own salt. game.details is left as it is (the load reads only its
 * model version). */
#ifndef SNAPMAP_PLUS_STORE_SAVES_H
#define SNAPMAP_PLUS_STORE_SAVES_H

#include <stddef.h>
#include <wchar.h>

#define SH_STORE_SIDECAR_LEN 40

/* SHA-256 of `n` bytes. 1 on success. */
int sh_store_sha256(const unsigned char *bytes, size_t n, unsigned char out[32]);

/* %USERPROFILE%\Saved Games\id Software\DOOM\base\savegame.user (the known
 * folder, then the profile's "Saved Games"), malloc'd; NULL when unknown. */
wchar_t *sh_store_saves_root(void);

/* The sidecar of `bytes` for base name `base` ("map.decl") with this salt and
 * user id. 1 on success. */
int sh_store_sidecar(const unsigned char *bytes, size_t n, const unsigned char salt[8], const char *user, const char *base,
                     unsigned char out[SH_STORE_SIDECAR_LEN]);

/* The user id the present `sidecar` of `bytes` verifies with, trying the
 * account folder's name and that id with its instance field cleared (and
 * set to 1). 1 with it in `user`, 0 when none verifies. */
int sh_store_sidecar_user(const char *account, const unsigned char *bytes, size_t n,
                          const unsigned char sidecar[SH_STORE_SIDECAR_LEN], const char *base, char *user, size_t cap);

#endif /* SNAPMAP_PLUS_STORE_SAVES_H */
