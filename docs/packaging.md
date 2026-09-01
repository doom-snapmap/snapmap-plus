# Packaging — the deployable bundle

`package.ps1` assembles the **deployable overlay** into `dist/`: the two clone DLLs (built by `build.ps1`),
laid out exactly as they drop into a DOOM install. This is the artifact an end user installs; `dist/` is
gitignored (binaries are never committed).

## The bundle — 2 files

| File | Source | What |
|---|---|---|
| `XINPUT1_3.dll` | `build/` | the backend: XInput proxy + hook layer + cvar-unlock + fault-shield + the SnapStack subsystem (all merged in) |
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
<DOOM install root>/            # the folder with DOOMx64vk.exe
├── XINPUT1_3.dll
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

- **`dinput8.dll`** — the cvar-unlock is merged into the backend `XINPUT1_3.dll` instead of riding a second
  proxy DLL; DOOM loads the real `System32\dinput8.dll`.
- **`winmm.dll`** — the fault-shield is merged into the backend too; DOOM loads the real
  `System32\winmm.dll`. (Never ship a `winmm.dll`: a stub shadowing System32's copy kills the launch
  before any logging.)

## Per-user data is NOT shipped

The bundle ships **no player override files**. At runtime the tool reads your own from
`%LOCALAPPDATA%\snapmap-plus\overrides\`. Existing identities use the file-shadow; genuinely new text decl
identities under `overrides\generated\decls\<type>\` are registered once at cold start from one immutable
in-memory per-decl table by the decl server. Each published entry is keyed by
`decltree/<type>/<logical-name>.decl`; no aggregate source file is created or served.
Snapmap+'s small built-in defaults are instead served from memory by the backend DLL. Neither user mechanism
packages or distributes referenced binary assets.

The bundle also ships no `config.json`. That file is runtime-owned player data, separate from the
installer's executable and `install.json` record. The installer never rewrites it and preserves it across
update, uninstall, and reinstall; deleting it yourself resets preferences because the backend recreates
defaults on the next startup.

## Build-target anchor (portability)

The clone is built against a specific DOOM build:

```
DOOMx64vk.exe  SHA256  139763E94F1A75B5310179F9EEEB8A949A1F53C49ACBC722FCFC5DFE7BB6D323
```

A DOOM update changes this hash, which means a re-port (signature re-resolve + build-specific offset re-derive).
The clone is built to survive that: engine functions are signature-resolved (fail-loud), data globals are
RIP-decoded, and build-specific offsets carry re-derive recipes. The auto-re-patcher that automates this on each
DOOM update is future work.
