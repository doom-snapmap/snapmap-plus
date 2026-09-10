# Snapmap+

## Purpose

Snapmap+ extends DOOM 2016's SnapMap editor with entity and declaration editing,
prefabs, timelines, asset browsing, override packages and navigation tools. It
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
| [site/](site/README.md) | Public website and player guide. |
| [feedback/](feedback/README.md) | In-app feedback relay. |
| [community/](community/README.md) | Website discussions, sign-in and screenshots. |
| [.github/](.github/AUTOMATION.md) | CI, releases, publishing and repository policy. |
| `build.ps1`, `package.ps1` | Compile both DLLs and assemble the installable bundle. |

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

Close DOOM before installing or updating. Launch it afterward and enter SnapMap;
run `sh` in the game console if the companion window does not open. See the
[player guide](site/snapmap-plus-guide.md).

The installer and player data live under `%LOCALAPPDATA%\snapmap-plus`.
Uninstall restores recorded backups and preserves settings, overrides, prefabs
and rawmaps. Original SnapHak user data is copied forward during migration;
see the [installer README](installer/README.md).

## Build and test

The DLL build requires MSVC 2022 Build Tools with the C++ desktop workload.
Building the installer also requires Go 1.21 or later. The frontend build
fetches its WebView2 SDK; the running application uses the system WebView2
runtime. Full setup instructions are in [contributing](docs/contributing.md).

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run-tests.ps1
```

The output is `build/XINPUT1_3.dll` and
`build/webview/snapmap-plus-ui.dll`; `package.ps1` assembles them in `dist/` with
`MANIFEST.sha256`. Deploy a local build with
`installer/snapmap-plus.exe install --local dist`.

Engine signature and global-anchor changes require the two-image test gate:
`tests/run-tests.ps1 -Doom <unpacked-image> -DoomAlt <other-renderer-image>`.
The [test guide](docs/contributing.md#7-run-the-tests) covers installer and
release-tool checks as well.

## Versioning & releases

The Git tag is the version. Each release contains both the DLL bundle and the
installer. Plain `vX.Y.Z` tags are stable; `vX.Y.Z-beta.N` tags are prereleases.
Use `--beta` for the newest beta or `--release <tag>` for a specific version.
Without a stable release, the installer reports its fallback to the newest beta.

[CHANGELOG.md](CHANGELOG.md) is the release-note source. Maintainers prepare and
review its entry before pushing a tag; follow [Cutting a release](docs/contributing.md#cutting-a-release-maintainer).

## Settings and packages

The backend creates `config.json` at first launch and owns its validation and
recovery. Deleting it resets preferences on the next startup. Manual changes
are read at startup; UI settings use the registered service. The installer
preserves this file.

Each override package has a `package.json` marker under `overrides/`, with
declarations in `decls/<type>/<logical-name>.decl`. Resource manifests can refer
to assets already installed with DOOM. Restart after manual package changes;
the map-package installation flow refreshes packages within the session.

## Credits

SnapHak, Chrispy's original closed-source SnapMap tool, established the rawmap,
override, hidden-entity and companion-window workflows that Snapmap+ builds on.
Snapmap+ implements these independently and extends them for current use.

## License

MIT. See [LICENSE](LICENSE).
