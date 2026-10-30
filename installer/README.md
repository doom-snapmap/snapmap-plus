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
| `legacy.go`, `userdata.go` | Earlier-install cleanup and player data. |
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

Every downloaded release asset must match the size and `sha256:` digest GitHub
publishes for it; a short, failed or altered download is refused and its temporary
files are removed before anything is installed or replaced. The bundle's files are
then checked against its `MANIFEST.sha256`.

For testing, `SNAPMAP_PLUS_API_BASE` replaces `https://api.github.com` with a local
mock of the Releases API. Only a loopback URL (`127.0.0.1`, `localhost` or `[::1]`)
is accepted; any other value is ignored with a notice. `install.ps1` honours the same
variable. The Go suite runs install, update, self-update and uninstall against such a
mock, including corrupt, mismatched and interrupted downloads.

The changelog command reads GitHub release bodies derived from reviewed
[CHANGELOG.md](../CHANGELOG.md) entries. It shows the newest release and an older
release index by default; a version, `latest`, or `all` selects another view.
Terminal output removes inline bold and code markers before wrapping the text,
while preserving command names such as `sh_pretty_on`.

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
metadata directory also remain there. The installer scaffolds `strings\` and
`prefabs\` only; retired package folders (`overrides`, `package-cache`,
`package-staging`, `override-backups`) are never copied, created or removed.

Stable installer copies are tracked by name and SHA-256 in `installer-files.json`.
Uninstall removes only recorded copies whose bytes still match, while retaining
the running image. Unrecorded executables and files replaced by the user remain.
Self-updates stage and verify the replacement before renaming the old image; a
failed rollback reports the retained original's recovery path.
An adjacent `<installer>.updates.json` records backups from completed updates.
Cleanup requires matching current-image and backup hashes. Unrecorded backups,
user-modified files and recovery images from failed replacements remain untouched.

`config.json` belongs to the backend. Install, update, uninstall and reinstall
do not create, parse or rewrite it. Prefabs, strings and rawmaps also remain
player-owned. The Go suite checks these preservation rules across an install
and uninstall cycle.

The installer checks for WebView2 and offers Microsoft's runtime bootstrapper
when needed. See [packaging](../docs/packaging.md) for the installed file contract.

## Packages

The package system is being rebuilt for the April 2024 DOOM update (Steam build
13954591). This release reads, installs and carries no packages: nothing is
loaded from `%LOCALAPPDATA%\snapmap-plus\overrides\`, no `package-cache` or
`package-staging` folder is created, and a map saved or published with packages
by an earlier release loads as a plain map without them. Folders that earlier
releases left are not touched; you can keep or delete them.
The installer therefore converts no packages and has no `migrate-overrides`
command.

## Private release repositories

Public releases need no token. For a private fork, use a fine-grained token
with repository Contents read access through `SNAPMAP_PLUS_TOKEN`, `--token`,
or `snapmap-plus set-token <token>`. The last option stores it in
`%LOCALAPPDATA%\snapmap-plus\token`.
