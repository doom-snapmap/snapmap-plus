# Snapmap+

## Purpose

Snapmap+ extends DOOM 2016's SnapMap editor with entity and declaration editing,
prefabs, timelines, asset browsing, mods with Lua scripts and navigation tools. It
ships two Windows DLLs and a standalone installer, supporting the Vulkan and
OpenGL executables.

The project is an independent, clean-room implementation. It ships no DOOM or
SnapHak binary content; game assets are read from the player's installation.

## Contents

| Path | Responsibility |
|---|---|
| [src/](src/README.md) | Backend engine integration and the WebView2 companion window. |
| [installer/](installer/README.md) | Install, update and uninstall CLI. |
| [docs/](docs/README.md) | Contributor guides and cross-component contracts. |
| [tests/](tests/README.md) | Native and JavaScript regression tests. |
| [tools/](tools/README.md) | Release-note tooling and signature derivation. |
| [feedback/](feedback/README.md) | In-app feedback relay. |
| [.github/](.github/AUTOMATION.md) | CI, releases, publishing and repository policy. |
| `build.ps1`, `package.ps1` | Compile both DLLs and assemble the installable bundle. |

The website and player guide live in the separate
[snapmap-web](https://github.com/doom-snapmap/snapmap-web) repository.

## Working here

Start with [contributing](docs/contributing.md) for setup, builds, tests and
review requirements and [architecture](docs/architecture.md) for component
boundaries. Main directory READMEs explain responsibilities; source comments
document detailed ownership, limits and thread requirements.

Keep source ASCII where required by the Windows build, preserve generated
tables and license notices, and update current documentation with behavior
changes. Binaries and generated build output are ignored; CI produces release
artifacts from source.

## Install and use

Download `snapmap-plus.exe` from [Releases](https://github.com/doom-snapmap/snapmap-plus/releases)
and double-click it. The installer locates DOOM through Steam and asks before
changing files. It also accepts terminal commands:

```text
snapmap-plus install
snapmap-plus update
snapmap-plus status
snapmap-plus uninstall
```

Release binaries are not code-signed, so Windows SmartScreen may say
"Windows protected your PC" for `snapmap-plus.exe`; choose **More info**, check
that the file came from this repository's Releases page, then **Run anyway**.
`gh attestation verify snapmap-plus.exe --repo doom-snapmap/snapmap-plus` proves
the file was built here from source.

Close DOOM before installing or updating. Launch it afterward and open a map in
the SnapMap editor; the companion window appears beside the game. See the
[player guide](https://www.snapmapplus.com/snapmap-plus-guide.html).

The installer and player data live under `%LOCALAPPDATA%\snapmap-plus`.
Uninstall restores recorded backups and preserves settings, mods, prefabs
and rawmaps. Original SnapHak user data is copied forward by the installer;
see the [installer README](installer/README.md).

## Build and test

The DLL build requires MSVC 2022 Build Tools with the C++ desktop workload.
Building the installer also requires Go 1.26 or later. The frontend build
fetches its WebView2 SDK; the running application uses the system WebView2
runtime. Full setup instructions are in [contributing](docs/contributing.md).

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run-tests.ps1
```

The output is `build/XINPUT1_4.dll` and
`build/webview/snapmap-plus-ui.dll`; `package.ps1` assembles them in `dist/` with
`MANIFEST.sha256`. Deploy a local build with
`installer/snapmap-plus.exe install --local dist`.

Engine signature and global-anchor changes require the two-image test gate:
`tests/run-tests.ps1 -Doom <DOOMx64vk.exe> -DoomAlt <DOOMx64.exe>` with the
executables of the supported build (the April 2024 update, Steam build 13954591).
The [test guide](docs/contributing.md#7-run-the-tests) covers installer and
release-tool checks as well.

## Versioning & releases

The Git tag is the version. Each release contains both the DLL bundle and the
installer. Plain `vX.Y.Z` tags are stable; `vX.Y.Z-beta.N` tags are prereleases.
The installer takes the newest stable release by version; `--beta` takes the
newest release of either kind, and `--release <tag>` a specific version.
Install, update and the installer's own update never move to an older version
unless `--release` names it.

[CHANGELOG.md](CHANGELOG.md) is the release-note source. Maintainers prepare and
review its entry before pushing a tag; follow [Cutting a release](docs/contributing.md#cutting-a-release-maintainer).

## Settings and mods

The backend creates `config.json` at first launch and owns its validation and
recovery. Deleting it resets preferences on the next startup. Manual changes
are read at startup; UI settings use the registered service. The installer
preserves this file.

Mods live in `%LOCALAPPDATA%\snapmap-plus\mods\`. Authors make, install and
refresh them in the window's Mods tab or with the `sh_mods` console command; a
saved map names the mods it uses, and the next load activates them. A map that
names a mod you do not have asks first and downloads only what you approve,
verified by hash, into that map's own folder under `mods\maps\`. A mod may carry
Lua scripts that run during Play, sandboxed, on the host only.

On install and update the installer runs `snapmap-plus-store.exe`, which converts
what earlier releases left: every mod folder and saved map, the packages in
`overrides\`, and its loose files and `strings\strids.json` into the mod
`mods\my-overrides`, then sends `overrides\` and `strings\` to the Recycle Bin
once everything in them is in `mods\`. Nothing is deleted for good: an item the
Recycle Bin cannot take stays where it is, and the tool says so. A mod folder or
map in an earlier shape that the installer has not converted is refused with a
plain sentence; running the installer again converts it. See
[Mods](docs/capabilities.md#mods).

## Credits

SnapHak, Chrispy's original closed-source SnapMap tool, established the rawmap,
override, hidden-entity and companion-window workflows that Snapmap+ builds on.
Snapmap+ implements these independently and extends them for current use.

## License

MIT. See [LICENSE](LICENSE).
