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
| `storemigrate.go` | Placing and running the store migration tool, `snapmap-plus-store.exe`. |
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
snapmap-plus install   [--doom <path>] [--local <dist-dir>] [--release <tag>] [--beta] [--force] [--yes]
snapmap-plus update    [--doom <path>] [--release <tag>] [--beta] [--no-self] [--force] [--yes]
snapmap-plus uninstall [--doom <path>] [--yes]
snapmap-plus changelog [<version>|latest|all]
snapmap-plus status
snapmap-plus version
snapmap-plus help
```

Any command also takes `--plain` (see [Output](#output)).

With no arguments, the executable opens an interactive prompt. Installation,
update and uninstall ask for confirmation after checks; `--yes` or `-y` skips
that prompt, and input that ends before an answer counts as no. Both DOOM executables
must be closed before changing installed DLLs: the check looks for any running
`DOOMx64vk.exe` or `DOOMx64.exe` process by name, wherever it was started from.
The Go lifecycle tests use temporary game and profile directories with a controlled
process check; they can run while your real game is open.

`--doom` overrides Steam discovery. `--local` reads the repository's packaged
`dist/` directory. Downloads use the latest stable release by default, with a
reported fallback to the newest beta while no stable release exists. `--beta`
selects the newest beta; `--release` selects a specific tag. Updates also refresh
the installer unless `--no-self` is supplied.

The installer identifies the DOOM build from the executables' SHA-256 before it
downloads anything. On the 2018 build (Steam build 2642713) install, update and
reinstall download v0.2.1-beta.19's own `snapmap-plus.exe`, verify it, and run its
`install` under the name `snapmap-plus-v0.2.1-beta.19.exe`, so that release's own
steps happen; its copy of itself is then removed from `%LOCALAPPDATA%\snapmap-plus`
and the current installer remains the stable copy (see
[packaging](../docs/packaging.md#supported-build-and-the-final-2018-release)).

Release bundles also carry v0.2.1-beta.19's `XINPUT1_3.dll` and
`snapmap-plus\snapmap-plus-ui.dll` byte for byte, so an update by an installer without
build detection never changes a 2018 install's working files (see
[packaging](../docs/packaging.md#the-bundle)). On the April 2024 build this installer
installs only `XINPUT1_4.dll` and `snapmap-plus\snapmap-plus-ui-13954591.dll`, and
removes the two 2018 files when an earlier installer left them: recorded ones, and
unrecorded ones whose bytes are the bundle's. In the interactive prompt, a 2018 install
whose record names another release is offered the update that restores it.

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

## Output

Everything the installer prints goes through `out.go`. Each phase (detecting DOOM,
migrating player data, installing files, updating, uninstalling, status) opens with a
title and a rule, details are aligned `Label:  value` lines, and each step ends in one
result line marked `ok` (done), `--` (skipped or unchanged), `!!` (warning, written to
stderr) or `xx` (error, written to stderr). Colour is used only when the stream is a
console that accepts virtual-terminal processing; redirected output never carries escape
codes. `--plain`, accepted with any command or on its own for the interactive prompt, and a
non-empty `NO_COLOR` turn colour off. The changelog renderer stays colourless.

## Installation and player data

The installer verifies every manifest-listed bundle file before changing DOOM.
Pre-existing files are backed up as `<file>.snapmap-plus-bak`; installed paths
and backups are recorded in `%LOCALAPPDATA%\snapmap-plus\install.json`. There is one
record, so installing into a second DOOM folder replaces the record of the first;
uninstall the first folder before installing into another.
Uninstall reverses that record and removes created directories only when empty.

Legacy SnapHak detection requires identifying files from that tool, so an
unrelated proxy DLL alone is insufficient. Migration removes the recognized
legacy runtime and copies player data from `%USERPROFILE%\snaphak` into the new
data directory. Existing destination files keep their contents. Differing source
files and failed copies remain in the legacy location for you to reconcile.
Migration compares file contents before retiring a source and removes only empty
directories; read errors and links prevent retirement. Unknown files in the old
metadata directory also remain there. The installer scaffolds `strings\` and
`prefabs\` only; this migration never copies, creates or removes the retired
package folders (`overrides`, `package-cache`, `package-staging`,
`override-backups`). The store tool below handles `overrides\`.

Stable installer copies are tracked by name and SHA-256 in `installer-files.json`,
and by release in `installer-versions.json`. An installer older than the stable copy
refuses to replace it, naming both versions, unless `--force` is given; a copy left by
an earlier installer carries no version and is replaced as before.
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

## Mods and retired package folders

DOOM converts nothing at startup. On install and update, after the DLLs are
placed and before it reports success, the installer runs the bundle's store
migration tool, `snapmap-plus-store.exe` (`storemigrate.go`, source in
`src/store/`):

1. It checks the tool against the bundle's `TOOLS.sha256` and keeps it beside
   `snapmap-plus.exe` in `%LOCALAPPDATA%\snapmap-plus`, recorded in
   `installer-files.json` like the installer's own copies, so uninstall's rules
   for those copies apply to it. A copy you changed is kept, and the bundle's
   verified copy runs instead.
2. It runs `snapmap-plus-store check "%LOCALAPPDATA%\snapmap-plus"`, which lists
   what it finds from earlier releases and changes nothing.
3. When check finds work (exit 1), it runs `snapmap-plus-store migrate
   "%LOCALAPPDATA%\snapmap-plus" --doom "<the DOOM folder it installed into>"`.

Every line the tool prints is shown, indented. The tool converts `mods\` folders
of the retired layout to one folder per mod with a `mod.json` (`bundle.json`,
`.state.json`, `blobs\` and `legacy\` go, hard-linked files get their own
copies, the old record files are folded into `mods\snapmap-plus.json`), sends
the retired recycle bin `removed-mods\` to the Windows Recycle Bin, and moves
the packages in `overrides\` into `mods\` once, leaving `overrides\` unchanged
and removing only an empty `override-backups\`. Everything it deletes goes to
the Recycle Bin. A missing tool, an altered one, or a check or migrate that
does not finish is a warning, never a failed install; the warning gives the
command to run by hand with DOOM closed,
`"%LOCALAPPDATA%\snapmap-plus\snapmap-plus-store.exe" migrate "%LOCALAPPDATA%\snapmap-plus" --doom "<DOOM folder>"`.
The installer itself never creates, changes or removes `mods\` or the retired
package folders (see [Mods](../docs/capabilities.md#mods)). In the game,
`sh_mods migrate` repeats the move of `overrides\`.

## Private release repositories

Public releases need no token. For a private fork, use a fine-grained token
with repository Contents read access through `SNAPMAP_PLUS_TOKEN`, `--token`,
or `snapmap-plus set-token <token>`. The last option stores it in
`%LOCALAPPDATA%\snapmap-plus\token`.
