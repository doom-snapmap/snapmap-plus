# Installer

## Purpose

Builds `snapmap-plus.exe`, the Windows CLI that installs, updates and removes
the Snapmap+ DLL bundle. It detects DOOM, verifies downloads, records backups
and preserves player data. The runtime uses only Go's standard library.

## Contents

| Files | Responsibility |
|---|---|
| `main.go`, `interactive.go` | CLI dispatch and the double-click interactive prompt. |
| `doom.go`, `bundle.go`, `install.go` | Game discovery, bundle validation and installation records. |
| `legacy.go`, `userdata.go`, `overrides_package.go` | Earlier-install cleanup, player data and package authoring helpers. |
| `overrides_*.go` | Conversion of override packages from earlier releases: discovery, legacy readers, installed catalog, planning and recoverable publication. |
| `selfinstall*.go`, `selfupdate.go`, `webview2.go` | Installer ownership, updates and WebView2 runtime setup. |
| `changelog*.go` | Release-note retrieval and terminal rendering. |
| `*_test.go`, `testdata/` | Installer and rendering regressions. |
| `build.ps1`, `versioninfo.json`, `snapmap-plus.manifest` | Windows build metadata. |
| `install.ps1` | Download-and-launch bootstrap script. |

## Working here

From this directory:

```powershell
gofmt -l .
go vet ./...
go test ./...
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

`gofmt -l .` should print nothing. `build.ps1 -Version v1.2.3` stamps the version
resource and executable; CI passes the release tag. The script installs
`goversioninfo` as a build tool, not a runtime dependency. Generated
`resource.syso` is ignored. Do not bump the placeholder version in
`versioninfo.json` for each release.

A plain `go build -o snapmap-plus.exe .` is useful for local checks but does not
embed the Windows resources. Use `build.ps1` for distributable executables.

## Commands

```text
snapmap-plus install   [--doom <path>] [--local <dist-dir>] [--release <tag>] [--beta] [--yes]
snapmap-plus update    [--doom <path>] [--release <tag>] [--beta] [--yes]
snapmap-plus uninstall [--doom <path>] [--yes]
snapmap-plus changelog [<version>|latest|all]
snapmap-plus status
snapmap-plus migrate-overrides [--doom <path>]
snapmap-plus version
snapmap-plus help
```

With no arguments, the executable opens an interactive prompt. Installation,
update and uninstall ask for confirmation after checks; `--yes` or `-y` skips
that prompt. Both DOOM executables must be closed before changing installed DLLs.
The Go lifecycle tests use temporary game and profile directories with a controlled
process check; they can run while your real game is open.

`--doom` overrides Steam discovery. `--local` reads the repository's packaged
`dist/` directory. Downloads use the latest stable release by default, with a
reported fallback to the newest beta while no stable release exists. `--beta`
selects the newest beta; `--release` selects a specific tag. Updates also refresh
the installer unless `--no-self` is supplied.

The changelog command reads GitHub release bodies derived from reviewed
[CHANGELOG.md](../CHANGELOG.md) entries. It shows the newest release and an older
release index by default; a version, `latest`, or `all` selects another view.

## Installation and player data

The installer verifies every manifest-listed bundle file before changing DOOM.
Pre-existing files are backed up as `<file>.snapmap-plus-bak`; installed paths
and backups are recorded in `%LOCALAPPDATA%\snapmap-plus\install.json`.
Uninstall reverses that record and removes created directories only when empty.

Legacy SnapHak detection requires identifying files from that tool, so an
unrelated proxy DLL alone is insufficient. Migration removes the recognized
legacy runtime and copies player data from `%USERPROFILE%\snaphak` into the new
data directory. Existing destination files keep their contents. Differing source
files and failed copies remain in the legacy location for you to reconcile.
Migration compares file contents before retiring a source and removes only empty
directories; read errors and links prevent retirement. Unknown files in the old
metadata directory also remain there. The installer creates a `my-overrides`
starter containing `package.json` and `assets/`. Copy engine-shaped resource
paths into `assets/`; requirements and strings belong in the descriptor.
Current descriptors and packages are preserved byte for byte.

Stable installer copies are tracked by name and SHA-256 in `installer-files.json`.
Uninstall removes only recorded copies whose bytes still match, while retaining
the running image. Unrecorded executables and files replaced by the user remain.
Self-updates stage and verify the replacement before renaming the old image; a
failed rollback reports the retained original's recovery path.
An adjacent `<installer>.updates.json` records backups from completed updates.
Cleanup requires matching current-image and backup hashes. Unrecorded backups,
user-modified files and recovery images from failed replacements remain untouched.

`config.json` belongs to the backend. Install, update, uninstall and reinstall
do not create, parse or rewrite it. Overrides, prefabs and rawmaps also remain
player-owned. The Go suite checks these preservation rules across an install
and uninstall cycle.

The installer checks for WebView2 and offers Microsoft's runtime bootstrapper
when needed. See [packaging](../docs/packaging.md) for the installed file contract.

## Converting overrides from earlier releases

Install and update convert override packages from earlier releases before any
DLL is replaced. `migrate-overrides` runs the same conversion for manual DLL
updates and for older packages copied in later. DOOM must be closed.

This includes the original SnapHak layout, before Snapmap+: content under
`%USERPROFILE%\snaphak` is verified and brought into the current data folder,
then loose `overrides/generated/decls/...` files become a current package.
Their declaration bytes are retained, and conversion keeps an original backup.
Already present destination files are never overwritten by the old-profile copy.

Format conversion is implemented in `src/backend/package_migration.c`, shared
with automatic migration of packages inside old saved maps. The installer
embeds a private build of that core; run `build-migration.ps1` before standalone
Go builds or tests. The normal repository and installer builds do this
automatically. Filesystem transactions remain in Go; map ZIP decoding and
temporary activation remain in the backend. An old map can migrate in memory
without running the installer, rewriting the saved map or restarting DOOM.

Discovery follows the runtime: a folder with `package.json` is one outer
package and other folders are groups. A package is converted when its
descriptor has no `id`, is empty, has the old
`snapmap-plus.override-package.v1` schema, when it has an old
`smpkg.digest` installation sidecar, or when its root has old namespaces with
their old content (`decls/`, `images/`, `shaders/generated/`, `resources/*.manifest`,
`requirements/*.requirements`, `strings/*.json`, `hud/weapons.json`). An unmarked
folder with those namespaces is converted in place. Loose content in
`overrides/generated`, `overrides/shader_includes`, an unmarked
`overrides/assets`, folders named after installed engine roots, and loose files
at installed engine paths becomes one new `my-overrides` package. If that folder
already contains a package, migration chooses `my-overrides-2` (or the next free
suffix) and preserves the existing package. A fresh install creates `my-overrides`
with a basic descriptor and an empty `assets/` directory.

Conversion mirrors the old readers. Declarations, images and shader programs
move to their engine paths below `assets/`; an `assets/decls` tree from a
partial manual move is recognized unless its path is installed engine content.
Requirement rows, flat string files and version 1 weapon HUD files fold into
`package.json` with the runtime's validation. Manifest rows resolve by exact
type, name and provider path in `gameresources.pindex`; records identical to
the SnapMap archive remain installed dependencies and the rest are copied as
package files. Files the old runtime never served, such as `.backup` copies,
stay outside `assets/`. Retired descriptor fields are `schema`, `version`,
`priority`, `contents` and `restart_required`; a missing `id` becomes
`local.<folder>`. Declaration contents are never edited.

A package whose content cannot be represented is reported and left unchanged:
invalid JSON or ids, unsupported requirements, conflicting strings, different
bytes for one output path, invalid declaration paths, unresolved manifest rows,
links and package markers inside old namespaces. Other packages still convert.
Storage, catalog and recovery failures stop the command, and installation stops
before replacing the runtime.

Each converted package is one transaction in
`%LOCALAPPDATA%\snapmap-plus\override-backups\migration-*`. The complete new
tree is staged and each copied byte is checked against a snapshot; the journal
is written; originals move to `original/`; the moved originals are hashed
again; only then is the new tree renamed into place. A later run finishes a
verified transaction or restores an unverified one, and never overwrites a path
that appeared in the meantime. Backups are never removed automatically.

## Private release repositories

Public releases need no token. For a private fork, use a fine-grained token
with repository Contents read access through `SNAPMAP_PLUS_TOKEN`, `--token`,
or `snapmap-plus set-token <token>`. The last option stores it in
`%LOCALAPPDATA%\snapmap-plus\token`.
