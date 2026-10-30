/* modstore.h -- the local mod store, %LOCALAPPDATA%\snapmap-plus\mods\.
 *
 *   blobs\<sha256>                  one read-only copy per distinct file
 *   <id>@<version>+<build>\bundle.json   the manifest with "bundle"
 *   <id>@<version>+<build>\assets\...    the tree, hard-linked to blobs, else copied
 *   <id>@<version>+<build>\.state.json   {"firstCause","installedAt","validated"}
 *   legacy\<id>@<digest16>\...      converted first-generation packages
 *
 * A mod folder dropped into mods\ by hand is built in place: bundle.json is
 * written beside its descriptor and its files are copied into blobs\.
 * Installing from elsewhere builds the source folder in place, then lays the
 * tree out under <id>@<version>+<build>\. Verification (rehash every file) runs at
 * install and on request, never at boot; enumeration trusts bundle.json and
 * checks sizes only.
 *
 * File work only, on the calling thread; no engine calls. Every writer of the
 * store serialises on modstore_lock: the install and verification below take
 * it themselves, and longer steps (one migration folder, a download install,
 * `sh_mods install` and `sh_mods migrate`, an export) hold it around their
 * whole step. A second writer waits for the first, it never fails because of
 * it; the wait is one step long (a migration folder is at most a few seconds).
 */
#ifndef SNAPMAP_PLUS_MODSTORE_H
#define SNAPMAP_PLUS_MODSTORE_H

#include <stddef.h>
#include "modbundle.h"

/* The store lock (recursive). modsync_store_lock and modsync_store_unlock
 * (modsync.h) are the same lock under their older names. A thread that finds
 * it held logs one "MODS: store: waiting" line and waits. */
void modstore_lock(void);
void modstore_unlock(void);
/* How many lock calls had to wait so far (tests, diagnostics). */
unsigned modstore_lock_waits(void);

/* id@version+build; version may hold up to 4 UTF-8 bytes per code point. */
#define SH_MS_KEY_MAX (SH_MB_ID_MAX + 1 + 4 * SH_MB_VERSION_MAX + 1 + 20)

/* The store root. NULL or "" restores the default (%LOCALAPPDATA%\snapmap-plus\mods).
 * Tests point it at a temporary folder. Returns 1 when the root is set. */
int  sh_ms_set_root(const char *root_utf8);
/* The current root as UTF-8, or 0 when %LOCALAPPDATA% cannot be resolved. */
int  sh_ms_root(char *out, size_t cap);

typedef struct sh_ms_entry {
    char *folder;               /* absolute UTF-8 folder of the bundle */
    char  key[SH_MS_KEY_MAX + 1];   /* id@version+build from the manifest, or the folder name when unbuilt */
    int   legacy;               /* under legacy\ */
    int   loaded;               /* bundle.json read and its hash matched */
    int   sizes_ok;             /* every file present with its manifest size */
    int   has_state;
    int   validated;            /* .state.json "validated" */
    long long installed_at;
    char  cause[SH_MB_CAUSE_MAX];   /* why the entry is unusable, or the state's firstCause */
    sh_mb_bundle bundle;
} sh_ms_entry;

typedef struct sh_ms_list {
    sh_ms_entry *items;
    size_t count, cap;
} sh_ms_list;

/* An entry's bundle may serve names: loaded, sizes match, and not marked
 * invalid by a previous verification. */
int  sh_ms_entry_usable(const sh_ms_entry *entry);

/* List every bundle folder under the root and legacy\. Folders with a
 * descriptor but no bundle.json are listed unloaded. Returns 1 when the root
 * could be read (an absent root is an empty list). */
int  sh_ms_enumerate(sh_ms_list *out);
void sh_ms_list_free(sh_ms_list *list);

typedef struct sh_ms_install_stats {
    unsigned blobs_written, blobs_reused;
    unsigned linked, copied;
    int in_place;               /* the source folder was inside the store */
    int replaced;               /* an earlier build of the same id@version was replaced */
} sh_ms_install_stats;

/* Build `folder` in place (writes its bundle.json), copy its files into
 * blobs\, lay out <id>@<version>+<build>\ (unless the folder is already in the store),
 * verify the result and write .state.json. `resolver` may be NULL; the store's
 * installed bundles are consulted in addition to it. On success key_out holds
 * id@version+build. bundle_out (optional) receives the built bundle; release it with
 * sh_mb_free. Returns 1 on success, else 0 with the first cause. */
int  sh_ms_install_from_folder(const char *folder, const sh_mb_resolver *resolver,
                               char *key_out, size_t key_cap, sh_ms_install_stats *stats,
                               sh_mb_bundle *bundle_out, char *cause, size_t cause_cap);

typedef struct sh_ms_verify_stats {
    unsigned files, bytes_mb, blobs_checked, entries;
} sh_ms_verify_stats;

/* Rehash every file of the bundle(s) installed as `key` against bundle.json
 * and their blobs, and record the result in .state.json. `key` is
 * id@version+build for one build, or id@version for every installed build.
 * Returns 1 when every matching entry passes. */
int  sh_ms_verify(const char *key, sh_ms_verify_stats *stats, char *cause, size_t cause_cap);

enum { SH_MS_MISS = 0, SH_MS_HIT = 1, SH_MS_DEDUPE = 2, SH_MS_CONFLICT = 3 };

typedef struct sh_ms_hit {
    int    status;
    size_t entry, file;         /* the first match */
    size_t providers;           /* distinct usable bundles providing the name */
    size_t other_entry;         /* the second provider when providers > 1 */
} sh_ms_hit;

/* Find who provides logical (type, name) across usable installed bundles:
 * provides.type and provides.name compared case-insensitively, or a file
 * whose request path is one the engine would ask for that name. The first
 * match wins. Two bundles providing it are a CONFLICT for decl names (and for
 * raw files with different bytes) and a DEDUPE for raw files with equal
 * blob hashes. Returns the status. */
int  sh_ms_resolve_need(const sh_ms_list *list, const char *type, const char *name, sh_ms_hit *hit);

/* sh_mb_exists_fn over a list: 1 when a usable bundle ships `request`. */
int  sh_ms_request_exists(void *list, const char *request);

#ifdef SH_MODSTORE_TESTING
/* Force the copy fallback instead of CreateHardLinkW. */
void sh_ms_test_force_copy(int on);
#endif

#endif /* SNAPMAP_PLUS_MODSTORE_H */
