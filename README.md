# open-snaphak

An open-source, clean-room reimplementation of **SnapHak** — Chrispy's closed-source modding tool for
DOOM 2016's in-game **SnapMap** level editor. It builds to two drop-in DLLs that, deployed into a stock
DOOM 2016 install, reproduce SnapHak's editor extensions: the console-command/cvar hook layer and the
"SnapHak Studio" Qt window.

**This repo ships NO DOOM or SnapHak bytes.** Every line is built from the project's own reverse-engineering
of the engine and the original tool — no decompiled or copied binary content. The original SnapHak is
closed-source; this is an independent, ground-up reimplementation. Legitimate single-player game-modding
research; the third-party runtime it links against (Qt, the DOOM engine) is not included.

## Repository layout

| Path | What |
|---|---|
| `src/backend/` | the backend DLL (`XINPUT1_3.dll`): the hook layer, 24 console commands, 9 cvars, cvar-unlock, and the resident fault-shield |
| `src/ui/` | the frontend DLL (`snaphakui.dll`): the Qt **"SnapHak Studio"** window |
| `src/common/` | the shared backend↔frontend interface ABI (`snaphak_iface.h`) |
| `src/fault_shield/` | the recover-in-place vectored-exception fault shield (compiled into the backend) |
| `build-backend.ps1` / `build-qt.ps1` / `build-webview.ps1` | compile the DLLs → `build/` (backend only · backend+Qt · backend + the experimental webview UI) |
| `package-qt.ps1` / `package-webview.ps1` | assemble the deployable overlay → `dist/` (Qt, with its runtime bundled · webview, no Qt) |
| `installer/` | `snaphak.exe` — the end-user install / update / uninstall CLI (Go) |
| `docs/` | architecture · fidelity · capabilities · packaging |

`build/` and `dist/` are gitignored — the **source is the deliverable**; the binaries are rebuilt.

## Quick start (players)

You do **not** need to build anything. Get `snaphak.exe` from the latest release and **double-click it** — it
auto-detects your DOOM install via Steam, asks you to confirm, and installs. (From a terminal: `snaphak install`.)

`snaphak.exe` installs itself to `%LOCALAPPDATA%\open-snaphak\`. Run it again any time for `snaphak update`,
`snaphak status`, `snaphak version`, and `snaphak uninstall` (which restores DOOM to vanilla and leaves your
`%USERPROFILE%\snaphak` data untouched). See [`installer/README.md`](installer/README.md).

> Releases are produced by CI. Until the first release is published, build from source (below).

## Build from source

**Requirements** (exact download links + the Qt 5.9.9 install walkthrough are in [`docs/contributing.md`](docs/contributing.md))
- **MSVC 2022 Build Tools** (the "Desktop development with C++" workload)
- **Qt 5.9.9** for MSVC 2017 64-bit at `C:\Qt\5.9.9\msvc2017_64` (override with `-QtDir`)
- **Go 1.21+** (only to build the installer)

```powershell
# 1. compile both DLLs -> build/XINPUT1_3.dll + build/qt/snaphakui.dll
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build-qt.ps1

# 2. assemble the deployable overlay -> dist/ (the 6-file DOOM tree: DLLs + Qt runtime)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File package-qt.ps1

# 3. (optional) build the installer
cd installer ; go build -o snaphak.exe .
```

(There's also an experimental Qt-free WebView2 frontend -- `build-webview.ps1` / `package-webview.ps1` --
see [`docs/webview-ui.md`](docs/webview-ui.md). Not the default; CI still builds the Qt path above.)

## Deploy a local build (contributors / testing)

Deploy your fresh `dist/` into your own DOOM with the installer's **local** mode — the same path end users
take, just from your build instead of a release:

```
installer\snaphak.exe install --local dist
```

`snaphak.exe uninstall` reverses it. (Or drop `dist\*` into the DOOM root by hand — `dist/` mirrors the exact
overlay tree.) Launch DOOM, enter the SnapMap editor; the "SnapHak Studio" window opens (run `sh` in the
console if it doesn't). DOOM keeps using the real `XInput1_3.dll` in System32 for controller input — the
backend forwards every XInput export through to it.

## Versioning & releases

Versions follow **semantic versioning** — `vMAJOR.MINOR.PATCH` (e.g. `v0.1.0`). **The git tag is the version**;
there is no `VERSION` file to maintain. One tag = one release containing **both** the mod bundle and
`snaphak.exe`, both stamped with that tag.

Cut a release (maintainer):

```
git tag v0.1.0
git push origin v0.1.0      # fires .github/workflows/release.yml
```

CI builds the DLLs + the installer (stamping `snaphak.exe` via `-ldflags -X main.version=v0.1.0`), packages the
overlay, and publishes a GitHub Release with `snaphak-bundle.zip` + `snaphak.exe` + `install.ps1`.

**Release channels** (set by the *tag*, not a branch):
- **Stable** — a plain tag `v0.3.0`. This is what end users' `snaphak update` gets.
- **Beta** — a pre-release tag `v0.3.0-beta.1` (any tag with a `-`; CI auto-marks it a GitHub pre-release). It's
  excluded from "latest", so end users never receive it. Beta testers opt in:
  `snaphak install --release v0.3.0-beta.1`.

Pin any version explicitly with `--release <tag>` on `install` or `update`.

- **`snaphak version`** prints the installer's version (and the installed mod version, if any).
- **`snaphak update`** pulls the latest release; **`snaphak status`** shows what's installed.
- A local/dev build reports `dev` (unstamped) or `local` (a `--local` install) — never a release number.

**Surviving DOOM updates (planned):** the clone resolves engine functions by *signature*, so many DOOM patches
need no rebuild at all. When a patch shifts things enough to require one, an **auto-re-patcher** CI job
(re-resolve signatures against the new DOOM build → rebuild → if green, publish a compatible release) is the
intended automation. Stubbed for now (see `release.yml`).

## Contributing

Contributions are welcome. **New here?** The full guide — fresh-machine setup (Git, MSVC, Qt 5.9.9, Go), the
build → package → test loop, the pull-request workflow, and the rule that the `docs/` are updated alongside
code — is in **[`docs/contributing.md`](docs/contributing.md)**. The short version:

1. **Fork** this repo (or branch, if you have write access).
2. Make your change under `src/`. Build (`build-qt.ps1`), package (`package-qt.ps1`), and test it in your own DOOM
   via `installer\snaphak.exe install --local dist`.
3. Open a **pull request** against `main`. The CI gate runs a security scan (no new binaries · capability-surface
   scan · gitleaks), the Windows build + package, the XInput ordinal-parity check, the C unit tests
   (`tests\run-tests.ps1`), and the installer's `gofmt`/`vet`/`test`; a maintainer reviews and merges. Tagged,
   reviewed commits are what produce releases.

**Keep PRs clean:**
- **No binaries.** Never commit a `.dll`/`.exe`/`.obj`/etc. — they're gitignored and **CI rejects any PR that
  adds one**. The source is the only deliverable; CI builds the binaries.
- **Clean-room only.** Contribute your **own** RE/implementation. Do not paste decompiled or copyrighted
  DOOM/SnapHak content.
- **Match the surrounding code** — the backend is plain C, the UI is Qt/C++; keep source **pure ASCII** (the
  PowerShell build reads BOM-less UTF-8 as Windows-1252). Run **`gofmt`** on anything in `installer/`.

Because the tool injects into DOOM, the release channel is a supply-chain target. PR CI runs in a
**secretless** sandbox (it cannot publish or touch signing keys), a maintainer reviews every diff, and a scan
flags any newly-introduced network / process-spawn / persistence code — the tool has no legitimate reason to
do any of that.

### Generated headers — don't hand-edit

A few committed headers are **generated data tables** derived from the project's reverse-engineering of the
engine and the original tool, not hand-authored source: `src/ui/sh_*.h` (entity descriptions, event
catalog/docs, asset lists, the inherit/class universe) and `src/backend/class_universe.h`. They're checked in
so the repo builds standalone — treat them as **vendored**: don't hand-edit them in a PR; open an issue
describing the change instead.

## Architecture & reference

| Doc | What |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | the 3-object model (window / controller / backend-owned interface), the 30 Hz think-loop, the 77-slot interface vtable, the backend↔frontend boundary |
| [`docs/fidelity.md`](docs/fidelity.md) | the original's quirks the clone reproduces on purpose, and the one sanctioned divergence (the fault-shield) |
| [`docs/capabilities.md`](docs/capabilities.md) | the full feature inventory — every console command, cvar, SnapStack op, and GUI tab |
| [`docs/packaging.md`](docs/packaging.md) | the deployable bundle: the 6-file overlay + the Qt runtime |

## Overrides (runtime, not shipped)

At runtime the tool reads per-user **override decls** from `%USERPROFILE%\snaphak\overrides\` (a file-shadow
over the engine's resource loader — e.g. to make extra editor entities placeable). **This repo ships none.**
Drop your own decls there and the tool picks them up. Runtime logs go to `<DOOM>\snaphak_logs\`.

## License

MIT — see [`LICENSE`](LICENSE).
