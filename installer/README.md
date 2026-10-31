# Installer

## Purpose

Builds `snapmap-plus.exe`, the Windows CLI that installs, updates and removes
the Snapmap+ DLL bundle. It detects DOOM, verifies downloads, keeps the DOOM
folder and its data folder to one layout contract and preserves player data.
The runtime uses only Go's standard library.

## Contents

| Files | Responsibility |
|---|---|
| `main.go`, `interactive.go` | CLI dispatch and the double-click interactive prompt. |
| `doom.go`, `bundle.go`, `install.go` | Game discovery, bundle validation, install, update and uninstall. |
| `doomclean.go` | The DOOM folder contract: which files are ours, the other build's pair, backups. |
| `state.go` | `install.json`, the installer's only record, and the retirement of earlier records. |
| `legacy.go`, `legacyinstall.go`, `userdata.go` | The original SnapHak, the 2018 build's release, and player data. |
| `storemigrate.go` | Placing and running the store migration tool, `snapmap-plus-store.exe`. |
| `selfinstall*.go`, `selfupdate*.go`, `webview2.go` | Installer copies, self-updates and WebView2 runtime setup. |
| `changelog*.go` | Release-note retrieval and terminal rendering. |
| `*_test.go`, `testdata/` | Installer and rendering regressions. |
| `sim/` | The simulation harness: `run.ps1` and its helper (never shipped). |
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
selects the newest beta; `--release` selects a specific tag. A stale installer
refreshes itself before it touches the game: at start, every run except
`version`, `help`, `set-token`, `uninstall`, an offline `--local` run and
`--no-self` resolves the release, replaces `snapmap-plus.exe` when the release
ships a newer one, and starts the new image with the same arguments, so the mod
work is always done by the current installer. `update` still refreshes the
installer at its end for a run that started current and found a newer release
meanwhile.

The installer identifies the DOOM build from the executables' SHA-256 before it
downloads anything. On the 2018 build (Steam build 2642713) install, update and
reinstall download v0.2.1-beta.19's own `snapmap-plus.exe`, verify it, and run its
`install` under the name `snapmap-plus-v0.2.1-beta.19.exe`, so that release's own
steps happen; its copy of itself is then removed from `%LOCALAPPDATA%\snapmap-plus`
and the current installer remains the stable copy (see
[packaging](../docs/packaging.md#supported-build-and-the-final-2018-release)).
Before and after that run the installer brings the folders to the contract below
for the 2018 pair. In the interactive prompt, a 2018 install whose record names
another release is offered the update that restores it.

Release bundles carry the April 2024 pair only. A bundle of an earlier release
also carried v0.2.1-beta.19's two files; the installer installs only the pair for
the detected build from any bundle, so an old bundle cannot put the other build's
files back, and it names each file it skipped.

Every downloaded release asset must match the size and `sha256:` digest GitHub
publishes for it; a short, failed or altered download is refused and its temporary
files are removed before anything is installed or replaced. The bundle's files are
then checked against its `MANIFEST.sha256`.

For testing, `SNAPMAP_PLUS_API_BASE` replaces `https://api.github.com` with a local
mock of the Releases API. Only a loopback URL (`127.0.0.1`, `localhost` or `[::1]`)
is accepted; any other value is ignored with a notice. `install.ps1` honours the same
variable. The Go suite runs install, update, self-update and uninstall against such a
mock, including corrupt, mismatched and interrupted downloads, and
[the simulation harness](#simulation-harness) runs the real executables against one.

## Output

Everything the installer prints goes through `out.go`. Each phase (detecting DOOM,
the earlier install it found, tidying the data folder, migrating player data,
cleaning the DOOM folder, installing files, converting mods, updating,
uninstalling, status) opens with a title and a rule, details are aligned
`Label:  value` lines, and each step ends in one result line marked `ok` (done),
`--` (skipped or unchanged), `!!` (warning, written to stderr) or `xx` (error,
written to stderr). Phases with nothing to report print nothing. Colour is used
only when the stream is a console that accepts virtual-terminal processing;
redirected output never carries escape codes. `--plain`, accepted with any command
or on its own for the interactive prompt, and a non-empty `NO_COLOR` turn colour
off. The changelog renderer stays colourless.

## Layout contract

After any install, update or migration:

- **The DOOM folder** holds exactly the pair for the detected build and nothing
  else of ours. On the April 2024 build (Steam build 13954591) that is
  `XINPUT1_4.dll` and `snapmap-plus\snapmap-plus-ui-13954591.dll`; on the 2018
  build, `XINPUT1_3.dll` and `snapmap-plus\snapmap-plus-ui.dll`. A file of ours
  found anywhere else in the folder is removed: the other build's pair, a file an
  earlier release placed and the record lists, any other file under
  `snapmap-plus\`, and the `<file>.snapmap-plus-bak` (or `.snaphak-bak`) backups
  earlier installers kept of our own DLLs. The original SnapHak's files are removed
  as before (see [Migration paths](#migration-paths)). The runtime folders the DLLs
  write while the game runs, `snapmap-plus\logs\` and `snapmap-plus\crash\`, stay
  until uninstall.
- **A file is ours** when the install record for this DOOM folder lists it, when its
  bytes are a file of the bundle being installed, or when it carries the string
  `snapmap-plus` (ASCII or UTF-16), which every Snapmap+ DLL does because it loads
  and logs to `snapmap-plus\` paths; neither the original SnapHak nor an unrelated
  XInput proxy does. A file at one of our names that is not ours, a file under
  `snapmap-plus\` that is not ours, and the proxy-shaped names of other tools at the
  top level (`dinput8.dll*`, `xinput*`, `winmm.dll*`, `snaphak*`) stay where they are
  and are listed under "Not ours, left in place".
- **No backup of our own files.** Neither XInput name exists in a vanilla DOOM
  folder, so uninstall restores vanilla by deleting the pair. The only file ever
  backed up is one that is not ours and that the pair replaces (another tool's
  `XINPUT1_4.dll`, say). Its backup is kept under the data folder's
  `cache\installer\backup\`, never in DOOM, and uninstall puts it back. A backup an
  earlier installer kept in DOOM is deleted when it is a copy of our own file, and
  moved into `cache\installer\backup\` and recorded when it is not.
- **The data folder**, `%LOCALAPPDATA%\snapmap-plus`, holds `snapmap-plus.exe`,
  `snapmap-plus-store.exe`, `install.json` and the player's own data (`config.json`,
  `prefabs\`, `strings\`, `mods\`, `rawmap.json`, `test-maps\`, `cache\`, and the
  `token` and `pinned.json` files when present). The retired `overrides\` folder of a
  release that had one stays: the store tool moves its packages and leaves it as it
  was. Empty `override-backups\`, `package-cache\` and `package-staging\` folders are
  removed; one that holds files is kept and listed.
- **Uninstall** removes every file of ours from DOOM, restores any backup from
  `cache\installer\backup\`, removes the runtime log and crash folders, the store
  tool, the data folder's installer copy and `install.json`, and leaves the
  player's data. An installer copy that is the one running cannot be removed: it
  stays, and `install.json` then records that copy alone.

### install.json

`install.json` is the installer's only record. The bookkeeping files of earlier
releases, `installer-files.json`, `installer-versions.json` and
`<installer>.updates.json`, are folded into it and removed whenever they are found,
at every start. `"version"` stays the first key: the backend and the frontend read
the first `"version"` in the file for their reports.

```json
{
  "version": "v0.3.0",
  "doom_path": "D:\\SteamLibrary\\steamapps\\common\\DOOM",
  "doom_build": "13954591",
  "installed_at": "2026-10-31T12:00:00Z",
  "files": ["XINPUT1_4.dll", "snapmap-plus\\snapmap-plus-ui-13954591.dll"],
  "backups": [{"rel": "XINPUT1_4.dll", "backup": "C:\\Users\\me\\AppData\\Local\\snapmap-plus\\cache\\installer\\backup\\XINPUT1_4.dll"}],
  "legacy_removed": ["dinput8.dll", "snaphak_algo.dll"],
  "executables": {
    "snapmap-plus.exe": {"sha256": "<hex>", "version": "v0.3.0"},
    "snapmap-plus-store.exe": {"sha256": "<hex>"}
  },
  "self_update": {
    "release": "v0.3.0",
    "checked_at": "2026-10-31T12:00:00Z",
    "placed_sha256": "<hex>",
    "set_aside": [{"path": "...\\cache\\installer\\snapmap-plus.exe.old", "sha256": "<hex>", "for": "...\\snapmap-plus.exe"}]
  }
}
```

`files` lists the pair placed; `backups` the files that were not ours and were
replaced; `legacy_removed` the original SnapHak's files a migration deleted.
`executables` ties each executable in the data folder to the bytes this installer
placed, with the release it is when known: an update replaces, and uninstall
removes, only a recorded copy whose bytes are unchanged, an installer older than
the stable copy refuses to replace it unless `--force` is given, and a copy the
player replaced is left alone. `self_update` records the last release checked, the
hash of the `snapmap-plus.exe` a self-update placed, and the replaced images still
to remove. A record with no `doom_path` holds only the installer's own state and
means "not installed".

A self-update stages and verifies the new image beside the old one, moves the old
image into `cache\installer\` (beside it when the data folder is on another
volume), puts the new one in its place and removes the old one at once. A
running image cannot be removed, so the one that started the update is recorded in
`set_aside` and removed at the next start while its bytes are unchanged and its
replacement exists; one that is still locked is retried at the start after. The
`snapmap-plus.exe.old` images earlier releases kept at the top of the data folder
are removed at every start while `snapmap-plus.exe` exists beside them. A failed
rollback reports the recovery path of the original, which is never removed.

## Migration paths

Each path prints what it found under "Earlier install", then a section for each
step that has something to report.

1. **From Chrispy's original SnapHak** (`dinput8.dll`, `snaphak_algo.dll` and the
   other files `legacy.go` recognises in DOOM; player data under
   `%USERPROFILE%\snaphak\`). Detection needs one of its own marker files, so an
   unrelated proxy DLL alone never triggers it. "Removing the original SnapHak"
   lists its files as they go, and backups an earlier install kept of them are
   discarded so uninstall cannot restore them. "Migrating player data" copies
   prefabs, strings and rawmaps into the data folder: existing destination files
   keep their contents, differing sources and failed copies stay in the old
   location, and the old folder is retired only once every file is verified.
   Then the pair is installed.
2. **From v0.2.1-beta.19** (its 2018 pair in an April 2024 folder, recorded or not,
   and packages under `overrides\`). "Cleaning the DOOM folder" removes the 2018
   pair, the April 2024 pair is installed, and "Converting mods" shows every line
   of the store tool, which moves the `overrides\` packages into `mods\`. On the
   2018 build itself v0.2.1-beta.19's own installer runs, and the 2018 pair stays.
3. **From v0.2.1-beta.20** (both pairs, backups of our own DLLs in DOOM,
   `installer-files.json`, `installer-versions.json`, `snapmap-plus.exe.updates.json`,
   `snapmap-plus.exe.old`). "Tidying the data folder" lists the retired records and
   images removed, "Cleaning the DOOM folder" the other build's pair and the backups,
   with any backup of a file that was not ours moved into `cache\installer\backup\`.
   Then the pair is installed.
4. **From the current release**: a plain update. The pair is replaced and nothing
   else changes.
5. **A fresh machine**: the pair is installed and the data folder created.

## Installation and player data

The installer verifies every manifest-listed bundle file before changing DOOM.
There is one record, so installing into a second DOOM folder replaces the record of
the first; uninstall the first folder before installing into another.

Migration of player data compares file contents before retiring a source and removes
only empty directories; read errors and links prevent retirement. Unknown files in
the old metadata directory also remain there. The installer scaffolds `strings\` and
`prefabs\` only; this migration never copies or creates the retired package folders
(`overrides`, `package-cache`, `package-staging`, `override-backups`). The store tool
below handles `overrides\`.

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
   `install.json` like the installer's own copy, so uninstall's rules for that
   copy apply to it. A copy you changed is kept, and the bundle's verified copy
   runs instead.
2. It runs `snapmap-plus-store check "%LOCALAPPDATA%\snapmap-plus"`, which lists
   what it finds from earlier releases and changes nothing.
3. When check finds work (exit 1), it runs `snapmap-plus-store migrate
   "%LOCALAPPDATA%\snapmap-plus" --doom "<the DOOM folder it installed into>"`.

Every line the tool prints is shown, indented, under "Converting mods". The tool
converts `mods\` folders of the retired layout to one folder per mod with a
`mod.json` (`bundle.json`, `.state.json`, `blobs\` and `legacy\` go, hard-linked
files get their own copies, the old record files are folded into
`mods\snapmap-plus.json`), sends the retired recycle bin `removed-mods\` to the
Windows Recycle Bin, and moves the packages in `overrides\` into `mods\` once,
leaving `overrides\` unchanged and removing only an empty `override-backups\`.
Everything it deletes goes to the Recycle Bin. A missing tool, an altered one, or a
check or migrate that does not finish is a warning, never a failed install; the
warning gives the command to run by hand with DOOM closed,
`"%LOCALAPPDATA%\snapmap-plus\snapmap-plus-store.exe" migrate "%LOCALAPPDATA%\snapmap-plus" --doom "<DOOM folder>"`.
Apart from removing empty retired folders, the installer itself never creates,
changes or removes `mods\` or the retired package folders (see
[Mods](../docs/capabilities.md#mods)). In the game, `sh_mods migrate` repeats the
move of `overrides\`.

## Simulation harness

`sim/run.ps1` runs the real installer through every migration path without GitHub,
so you can watch the whole flow and see the contract hold:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File installer\sim\run.ps1
```

It builds the DLLs and `dist/` (`build.ps1`, `package.ps1`; `-SkipNativeBuild`
reuses `build/`), two installers stamped `v0.3.0-sim.1` and `v0.3.0-sim.2`, and
the helper `snapmap-plus-sim.exe` (`sim/*.go`). It copies `DOOMx64vk.exe`,
`DOOMx64.exe` and the two archive indexes `base\gameresources.pindex` and
`base\snap_gameresources.pindex` from your DOOM folder (`-Doom`, found through
Steam when omitted; the folder is only read), so build detection and the store tool
see the real game. The helper serves a fake Releases API on a loopback port from
release folders packed from `dist/` as `release.yml` packs them, with the size and
digest fields the installer verifies, and `SNAPMAP_PLUS_API_BASE` points the
installer at it. Each scenario gets fresh scratch DOOM, data, profile and TEMP
folders under `-Scratch` (default `%TEMP%\snapmap-plus-sim`), with
`LOCALAPPDATA`, `USERPROFILE` and `TEMP` pointed at them:

| Scenario | Layout before | Steps |
|---|---|---|
| a | the original SnapHak in DOOM, prefabs, strings and a rawmap under `%USERPROFILE%\snaphak`, another tool's `dinput8.dll.doomforge.rollback` | install, uninstall |
| b | v0.2.1-beta.19's 2018 pair in the April 2024 folder, its record and `installer-files.json`, `tests/fixtures/store-retired` as the data folder (`overrides\`, the retired `mods\` layout, `removed-mods\`) | update (the store tool's lines are shown), uninstall |
| c | v0.2.1-beta.20: both pairs, three backups of our DLLs, another tool's file, `installer-files.json`, `installer-versions.json`, `snapmap-plus.exe.updates.json`, `snapmap-plus.exe.old`, an empty `override-backups\` | update, uninstall |
| d | none: `v0.3.0-sim.1` installs, then `v0.3.0-sim.2` is published | install, update (the installer updates itself first and relaunches), uninstall |

Stand-ins for the DLLs of earlier releases carry the `snapmap-plus` marker, as the
real ones do. After every step it prints the installer's full output and both
folders, every entry labelled, and checks the contract: the DOOM folder holds
exactly the vanilla files, the pair and the other tool's file; the data folder
holds nothing outside the contract, `install.json` starts with its version, names
the folder and the pair and records both executables with their hashes on disk,
`cache\installer` holds only what `install.json` records; after uninstall none of
the installer's files remain; the player's files are still there; TEMP is empty.
It ends with a summary table and exits 0 only when every step met the contract.
The full output is also written to `<scratch>\sim-output.txt` (and to `-Log`).
Scenario b's store tool sends the fixture's retired files to the Windows Recycle
Bin.

## Private release repositories

Public releases need no token. For a private fork, use a fine-grained token
with repository Contents read access through `SNAPMAP_PLUS_TOKEN`, `--token`,
or `snapmap-plus set-token <token>`. The last option stores it in
`%LOCALAPPDATA%\snapmap-plus\token`.
