/* packages.h -- one override package is one folder the user can drag in. */
#ifndef BACKEND_PACKAGES_H
#define BACKEND_PACKAGES_H

#include <windows.h>
#include <stddef.h>

#define SH_PACKAGES_MAX      64
#define SH_PACKAGE_NAME_CAP  160
/* How deep below overrides\ a package may sit. Grouping folders are free; this
 * only bounds the search so a deep or looping tree cannot stall startup. */
#define SH_PACKAGES_MAX_DEPTH 8

/* An override package is any directory at any depth below
 * %LOCALAPPDATA%\snapmap-plus\overrides that contains a package.json, holding
 * its own decls, resources and requirements:
 *
 *   overrides\cyberdemon\package.json
 *   overrides\cyberdemon\decls\<type>\<logical-name>.decl
 *   overrides\cyberdemon\resources\<name>.manifest
 *   overrides\cyberdemon\requirements\<name>.requirements
 *
 * A directory WITHOUT a package.json is not a package; it is a grouping folder,
 * and the search continues inside it. That is what lets a user organise their
 * installs however they like without anything being compiled or merged:
 *
 *   overrides\editor\lifts\package.json          -> package "editor/lifts"
 *   overrides\editor\toybox\package.json         -> package "editor/toybox"
 *   overrides\demons\cyberdemon\package.json     -> package "demons/cyberdemon"
 *
 * A package is a leaf: the search does not descend into one, so a package can
 * never contain another and its own subdirectories always mean what the package
 * layout says they mean. Inside `decls`, the path IS the decl's identity
 * (`decls\<type>\<logical-name>.decl`), so that part is not free-form -- extra
 * organisation goes in the grouping folders above the package, not inside it.
 *
 * Installing is copying a folder in; uninstalling is deleting it. Nothing is
 * compiled, staged or merged anywhere, so a package cannot leave artefacts
 * behind and two packages cannot quietly overwrite each other's files on disk.
 *
 * The pre-package layout -- a single shared overrides\generated tree -- is no
 * longer enumerated as a package; the installer migrates it into a real one
 * (overrides\my-overrides) so this file carries one rule rather than two.
 *
 * That migration does not change which BYTES the engine can be served. The file
 * shadow resolves every engine resource name against the overrides root
 * directly (overrides\<engine name>), which is a separate path from package
 * resolution and is unaffected: dropping a file at overrides\<name> still
 * shadows that resource. What a package adds on top is publishing identities
 * DOOM never shipped, and being uninstallable by deleting one folder.
 *
 * Identity collisions BETWEEN packages are handled in two different places,
 * because two different mechanisms can serve a decl.
 *
 * A decl a package PUBLISHES (an identity DOOM never shipped) goes through the
 * decl server, which already composes byte-identical copies away at discovery
 * and REFUSES an identity two packages claim with differing bytes -- loudly, and
 * on both sides, so neither silently wins.
 *
 * A decl a package SHADOWS (an identity DOOM did ship, overridden on the way to
 * the parser) goes through the file shadow, which resolves by trying each
 * package in turn and taking the first file that exists. That is a real
 * precedence decision, so it is made explicitly here rather than falling out of
 * directory order: packages are ordered by descending `priority` and then by
 * name, and `package_conflicts.c` reports every identity more than one package
 * claims so the outcome is visible instead of silent.
 *
 * Priority comes from the package's own package.json ("priority": N, default 0),
 * so the person who installs two overlapping packages can settle which one wins
 * without renaming folders to game the sort. */
typedef struct sh_package {
    char name[SH_PACKAGE_NAME_CAP];  /* path below overrides\, '/'-separated */
    char root[MAX_PATH];             /* absolute path to the package folder */
    int  priority;                   /* package.json "priority", default 0 */
} sh_package;

/* Enumerate packages below `<data_root>\overrides` at any depth, in
 * deterministic case-insensitive name order so two machines see the same order.
 * Directories without a package.json are searched, not returned; reparse points
 * are skipped. Returns 1 on a complete enumeration (including "none found"), 0
 * when the tree could not be read in full or did not fit; `*count` is always
 * set. A caller that needs every package must refuse on 0 rather than run with
 * a partial set. */
int sh_packages_enumerate(const char *data_root, sh_package *out, size_t capacity,
                          size_t *count);

/* Join `<package root>\<subdirectory>` into `out`. Returns 0 when it would not
 * fit. */
int sh_package_subdir(const sh_package *package, const char *subdirectory,
                      char *out, size_t out_size);

#ifdef SH_PACKAGES_TESTING
typedef struct sh_packages_test_find_api {
    HANDLE (WINAPI *find_first)(LPCSTR pattern, LPWIN32_FIND_DATAA found);
    BOOL (WINAPI *find_next)(HANDLE search, LPWIN32_FIND_DATAA found);
    BOOL (WINAPI *find_close)(HANDLE search);
    DWORD (WINAPI *get_attributes)(LPCSTR path);
} sh_packages_test_find_api;

void sh_packages_test_set_api(const sh_packages_test_find_api *api);
void sh_packages_test_reset_api(void);
#endif

#endif /* BACKEND_PACKAGES_H */
