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
| `legacy.go`, `userdata.go`, `overrides_migrate.go` | Legacy migration and player-data handling. |
| `selfinstall.go`, `selfupdate.go`, `webview2.go` | Installer updates and WebView2 runtime setup. |
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
data directory. Existing destination files win name collisions. Migration removes
the old source tree after checking destination paths, without comparing file
contents. The former shared `overrides/generated` layout is migrated into a
marked `my-overrides` package using the same rule.

`config.json` belongs to the backend. Install, update, uninstall and reinstall
do not create, parse or rewrite it. Overrides, prefabs and rawmaps also remain
player-owned. The Go suite checks these preservation rules across an install
and uninstall cycle.

The installer checks for WebView2 and offers Microsoft's runtime bootstrapper
when needed. See [packaging](../docs/packaging.md) for the installed file contract.

## Private release repositories

Public releases need no token. For a private fork, use a fine-grained token
with repository Contents read access through `SNAPMAP_PLUS_TOKEN`, `--token`,
or `snapmap-plus set-token <token>`. The last option stores it in
`%LOCALAPPDATA%\snapmap-plus\token`.
