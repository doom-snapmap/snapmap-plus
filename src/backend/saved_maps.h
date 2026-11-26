/* saved_maps.h -- the common reader of the player's local saved maps (My Maps).
 * Each slot folder <fs_savepath>\base\savegame.user\<profile>\SNAPMAPS<id>\ holds game.details
 * (key=value lines) and map.decl (a 4-byte checksum, then the map's JSON as zlib). */
#ifndef SNAPMAP_PLUS_SAVED_MAPS_H
#define SNAPMAP_PLUS_SAVED_MAPS_H

#include <stddef.h>

/* Bind the live cvar registry and account reader before any saved-map consumers start. */
void sh_saved_maps_bind(const void *cvar_slot, unsigned long long (*account)(void));
/* The current savegame.user folder (UTF-8, malloc'd), or NULL when unavailable. */
char *sh_saved_maps_root(void);
/* The native cloud-copy directory under fs_cachepath (UTF-8, malloc'd). */
char *sh_saved_maps_cloud_dir(int published);
#ifdef SH_SAVED_MAP_FIXTURE
void sh_saved_maps_set_root(const char *path);
#endif

/* The active account directory (wide long path, malloc'd). Offline with no account, require a single directory. */
wchar_t *sh_saved_map_profile(void);
/* The newest complete slot in that profile, for an edit without its own local ID. */
int sh_saved_map_fallback(char *out, size_t cap);

/* One slot: its local id (the folder's suffix), the cloud-saved item id its game.details names ("" for none), and
 * the map's JSON. */
typedef void (*sh_saved_map_fn)(void *context, const char *local_id, const char *item_id, const char *text, size_t len);

/* Calls `fn` for every slot of every profile whose map.decl decodes. Returns the number of maps read, 0 when
 * savegame.user does not exist, or -1 when its root cannot be resolved or listed. *unreadable
 * (optional) counts slots whose map.decl exists but does not decode; a slot without map.decl is not a map. */
int   sh_saved_maps_each(sh_saved_map_fn fn, void *context, size_t *unreadable);

/* The game.details text of the slot `local_id` in the active profile (malloc'd), or NULL. The id must be the engine's
 * own: letters and digits only, at most 40. */
char *sh_saved_map_details(const char *local_id);

#endif /* SNAPMAP_PLUS_SAVED_MAPS_H */
