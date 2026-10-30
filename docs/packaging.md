# Packaging — the deployable bundle

`package.ps1` assembles the **deployable overlay** into `dist/`: the two clone DLLs (built by `build.ps1`),
laid out exactly as they drop into a DOOM install. This is the artifact an end user installs; `dist/` is
gitignored (binaries are never committed).

## The bundle — 2 files

| File | Source | What |
|---|---|---|
| `XINPUT1_4.dll` | `build/` | the backend: XInput proxy + hook layer + cvar-unlock + fault-shield + the SnapStack subsystem (all merged in) |
| `snapmap-plus\snapmap-plus-ui.dll` | `build/webview/` | the frontend: the Snapmap+ UI, rendered in a Microsoft Edge **WebView2** control (HTML/CSS/JS), with the HTML embedded in the DLL |

Plus `MANIFEST.sha256` (the installer's file list + per-file hash verify). **Nothing else ships** — the
frontend renders in the **system-installed WebView2 runtime** (preinstalled on Windows 11; the evergreen
runtime on most Windows 10), so there is no UI-toolkit runtime to bundle. The two DLLs' exact sizes shift per build
(MSVC embeds a build timestamp), so equivalence is judged on the export/ordinal surface, not byte size.

Persistent settings do not add a payload file. The backend creates
`%LOCALAPPDATA%\snapmap-plus\config.json` at runtime when Snapmap+ first starts, so the deployable
overlay remains the same two DLLs. Configuration is runtime-owned and installer-preserved.

## Layout

```
<DOOM install root>/            # the folder with DOOMx64vk.exe and DOOMx64.exe
├── XINPUT1_4.dll
└── snapmap-plus/
    └── snapmap-plus-ui.dll
```

`dist/` mirrors this tree. The installer (`snapmap-plus.exe`) — or a manual drop-in — merges it into the DOOM
root. The Snapmap+ window opens in the SnapMap editor (run `sh` in the console if it doesn't
auto-open).

## The runtime dependency: WebView2

The frontend renders in Microsoft's **WebView2 runtime** rather than bundling a UI toolkit. It's part of
Windows 11 and is pushed to most Windows 10 machines via Microsoft Edge. On a machine that somehow lacks it, the
Snapmap+ window can't render — so **`snapmap-plus.exe` ensures it**: on `install` (and `update`) it checks the runtime's
registry key and, if it's absent, offers to download + run Microsoft's evergreen bootstrapper (`/silent /install`;
auto under `--yes`). This never blocks the mod install — the DLLs deploy regardless; on the common case
(Win11 / updated Win10) the check is a no-op since the runtime is already present.

## What's deliberately NOT shipped

- **`dinput8.dll`** — the cvar-unlock is merged into the backend `XINPUT1_4.dll` instead of riding a second
  proxy DLL; DOOM loads the real `System32\dinput8.dll`.
- **`winmm.dll`** — the fault-shield is merged into the backend too; DOOM loads the real
  `System32\winmm.dll`. (Never ship a `winmm.dll`: a stub shadowing System32's copy kills the launch
  before any logging.)

## Per-user data is NOT shipped

The bundle ships no player content. At startup the backend creates `%LOCALAPPDATA%\snapmap-plus\`
with its `strings\` and `prefabs\` folders, and nothing else. Snapmap+'s small built-in default
declarations are served from memory by the backend DLL. The companion window keeps its WebView2
profile in `cache\webview2\`, regenerable data the installer never backs up or copies (an older
top-level `webview2\` folder is renamed into `cache\` at the window's first start).

### Mods

The bundle ships no mods. Installed mods live in `%LOCALAPPDATA%\snapmap-plus\mods\`, which the backend
reads at startup but never creates there: the folder appears when `sh_mods install` adds a mod or a map
carrying a first-generation package is converted into a mod folder. Nothing is loaded from the retired
`%LOCALAPPDATA%\snapmap-plus\overrides\` tree, and no `package-cache` or `package-staging` folder is
created. Folders that earlier releases left are not touched; you can keep or delete them. The installer
does not touch `mods\`. See [Mods](architecture.md#mods).

The bundle also ships no `config.json`. That file is runtime-owned player data, separate from the
installer's executable and `install.json` record. The installer never rewrites it and preserves it across
update, uninstall, and reinstall; deleting it yourself resets preferences because the backend recreates
defaults on the next startup.

## Supported build and the final 2018 release

Snapmap+ targets one DOOM 2016 release: the April 2024 update, Steam build 13954591. It
ships **two** executables built from one source tree, `DOOMx64vk.exe` (Vulkan) and
`DOOMx64.exe` (OpenGL); the game relaunches itself into the other one when the
`r_renderAPI` cvar changes, so a player can be in either from one launch. Both are
supported by the same Snapmap+ build. They are not SteamStub-wrapped, and both import
`XINPUT1_4.dll`:

```
DOOMx64vk.exe  SHA256  3D8B4393649F80BDAE85C19AE2C985C45EC9AC54F74AB04A18816E30EAD63C21
DOOMx64.exe    SHA256  1BDDECAA4E86850412F96E3117978BCE1EC7BB701AE1B5A33120C21E4421E51E
```

These are `SH_KNOWN_BUILDS` in `host_image.c`; the installer and `installer/install.ps1`
carry the same digests (a Go test checks both). Each entry also names the Steam build and the
binary name DOOM's console prints (`20240321-104810-ginger-fuchsia`), from which the backend,
the installer and the relay build the one game-identity string reports carry, e.g.
`DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)` ([feedback pipeline](feedback.md)). A re-port adds the new
build to those tables and to the relay's `GAME_BUILDS` allowlist. Every signature `rva_vk`/`rva_gl`, every
globals-table RVA and every `SH_RVA` constant describes these two images; the two
executables are separate links with no uniform delta between them.

**v0.2.1-beta.19 is the final release for the 2018 build** (Steam build 2642713, whose
SteamStub-wrapped executables are `DOOMx64vk.exe` 139763E94F1A75B5310179F9EEEB8A949A1F53C49ACBC722FCFC5DFE7BB6D323
and `DOOMx64.exe` D46CA967824DA6FFC40F1CE0B6D7F4AFA868FE12B795124D3E31A1BDB646A8E5). The
installer and `install.ps1` identify that build by those digests and install
v0.2.1-beta.19 by tag: both download that release's own `snapmap-plus.exe`, check it against the
size and digest GitHub publishes, and run its `install`, so its own steps (converting `overrides\`
packages, the `my-overrides` starter) happen as they always did. The Go installer runs it as
`snapmap-plus-v0.2.1-beta.19.exe` and removes that copy from `%LOCALAPPDATA%\snapmap-plus` afterwards,
so the current installer stays the one kept there and the 2018 install keeps routing to that release on
every update and reinstall. That release and its assets (`install.ps1`, `snapmap-plus.exe`,
`snapmap-plus-bundle.zip`, `snapmap-plus-symbols.zip`) must stay published unchanged.
`release.yml` must never delete, rebuild or overwrite it; it refuses to run for that tag.
The tag lives in one constant, `legacyReleaseTag` in `installer/doombuild.go` (mirrored in
`install.ps1`).

A future DOOM update changes these hashes, which means a re-port: re-derive the
signatures and globals with `tools/signatures`, re-verify every engine offset, and update
the digest tables. The backend refuses an unrecognized executable rather than guessing.
