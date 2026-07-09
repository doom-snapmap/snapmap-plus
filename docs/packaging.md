# Packaging — the deployable bundle

`package-qt.ps1` assembles the **deployable overlay** into `dist/`: the two clone DLLs (built by
`build-qt.ps1`) plus the Qt 5.9.9 runtime, laid out exactly as it drops into a DOOM install. This is the
artifact an end user installs; `dist/` is gitignored (binaries are never committed — the Qt runtime is
copied from the contributor's Qt SDK at package time).

## The bundle — 6 files

| File | Source | What |
|---|---|---|
| `XINPUT1_3.dll` | `build/` | the backend: XInput proxy + hook layer + cvar-unlock + fault-shield (all merged in) |
| `snaphak\snaphakui.dll` | `build/qt/` | the frontend: the "SnapHak Studio" Qt window. (The experimental Qt-free webview frontend builds to `build/webview/` instead and packages via `package-webview.ps1` -- see [`webview-ui.md`](webview-ui.md).) |
| `snaphak\Qt5Core.dll` | Qt SDK | Qt 5.9.9 runtime (5,842,040 B) |
| `snaphak\Qt5Gui.dll` | Qt SDK | Qt 5.9.9 runtime (6,061,688 B) |
| `snaphak\Qt5Widgets.dll` | Qt SDK | Qt 5.9.9 runtime (5,599,352 B) |
| `platforms\qwindows.dll` | Qt SDK | Qt platform plugin — **required or Qt won't start**; must sit beside `DOOMx64vk.exe` so Qt finds it (1,357,432 B) |

The two clone DLLs are ~384 KB (backend) and ~1.57 MB (frontend); their exact size shifts per build
(MSVC embeds a build timestamp), so equivalence is judged on the export/ordinal surface, not byte size.
The four Qt files are fixed SDK images.

## Layout

```
<DOOM install root>/            # the folder with DOOMx64vk.exe
├── XINPUT1_3.dll
├── snaphak/
│   ├── snaphakui.dll
│   ├── Qt5Core.dll  Qt5Gui.dll  Qt5Widgets.dll
└── platforms/qwindows.dll        # beside DOOMx64vk.exe -- where Qt searches for its platform plugin
```

> **Why `platforms\` and not `plugins\platforms\`:** the frontend sets no Qt plugin path in code, so Qt only
> searches `<DOOMx64vk.exe dir>\platforms\` on a machine without Qt installed. A dev box with Qt installed
> finds the plugin via Qt's build-time path regardless — which is why a too-deep `plugins\platforms\` layout
> appears to work in development but fails on a clean machine with *"could not find or load the Qt platform
> plugin windows"*.

`dist/` mirrors this tree. The installer (`snaphak.exe`) — or a manual drop-in — merges it into the DOOM
root. The "SnapHak Studio" window opens in the SnapMap editor (run `sh` in the console if it doesn't
auto-open).

## What's required vs. optional

**Required:** the two clone DLLs + the mandatory Qt runtime (`Qt5Core`/`Qt5Gui`/`Qt5Widgets` + the
`qwindows` platform plugin). This is the irreducible floor — it can't go lower without static-linking Qt
(not worth the effort/licensing).

**Deliberately dropped** (live-verified the UI still renders without them):
- **`dinput8.dll`** — the cvar-unlock it used to carry is merged into the backend `XINPUT1_3.dll`; DOOM
  loads the real `System32\dinput8.dll`.
- **`winmm.dll`** — the fault-shield it used to carry is merged into the backend; DOOM loads the real
  `System32\winmm.dll`. (Never re-ship a `winmm.dll`: a stub shadowing System32's copy kills the launch
  before any logging.)
- **`Qt5Svg.dll`** — not imported by the frontend; the UI renders without it.
- **`lua51.dll`** — the Lua tab is a stub; not imported; the UI renders without it.
- **`ds_descriptions.json`** — the original loaded event descriptions from this file; the clone compiles
  its descriptions in, so it's unused.

## Overrides are NOT shipped

The bundle ships **no override decls** — those are per-user configuration. At runtime the tool reads your
own from `%USERPROFILE%\snaphak\overrides\` (pure file-shadow data, e.g. to make extra editor entities
placeable). Slot your own overrides there; the bundle provides none.

## Build-target anchor (portability)

The clone is built against a specific DOOM build:

```
DOOMx64vk.exe  SHA256  139763E94F1A75B5310179F9EEEB8A949A1F53C49ACBC722FCFC5DFE7BB6D323
```

A DOOM update changes this hash, which means a re-port (signature re-resolve + build-specific offset
re-derive). The clone is built to survive that: engine functions are signature-resolved (fail-loud),
data globals are RIP-decoded, and build-specific offsets carry re-derive recipes. The auto-re-patcher
that automates this on each DOOM update is future work (see the release workflow, `.github/workflows/release.yml`).
