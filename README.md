# Snapmap+

**Snapmap+** is an open-source, clean-room reimplementation of **SnapHak** — Chrispy's closed-source
modding tool for DOOM 2016's in-game **SnapMap** level editor. It builds to two drop-in DLLs that,
deployed into a stock DOOM 2016 install, reproduce (and extend) the original's editor extensions: the
console-command/cvar hook layer and the **Snapmap+** window.

**This repo ships NO DOOM or SnapHak bytes.** Every line is built from the project's own reverse-engineering
of the engine and the original tool — no decompiled or copied binary content. The original SnapHak is
closed-source; this is an independent, ground-up reimplementation. Legitimate single-player game-modding
research; the third-party runtime it links against (the DOOM engine, Microsoft's WebView2 runtime) is not included.

## Repository layout

| Path | What |
|---|---|
| `src/backend/` | the backend DLL (`XINPUT1_3.dll`): the hook layer, 29 console commands, 10 cvars, the persistent-settings registry, cvar-unlock, and the resident fault-shield |
| `src/ui/` | the frontend DLL (`snapmap-plus-ui.dll`): the WebView2 **Snapmap+** window (`webview/` = the host + `mockup.html`) |
| `src/common/` | the shared backend↔frontend interface ABI (`snapmap_plus_iface.h`) |
| `src/fault_shield/` | the recover-in-place vectored-exception fault shield (compiled into the backend) |
| `build.ps1` | compile the DLLs → `build/` (backend + frontend; `-BackendOnly` skips the frontend) |
| `package.ps1` | assemble the deployable overlay → `dist/` (the two clone DLLs) |
| `installer/` | `snapmap-plus.exe` — the end-user install / update / uninstall CLI (Go) |
| `docs/` | contributor documentation: architecture · capabilities · fidelity · packaging · webview-ui · backend-changes |
| `site/` | the website ([doom-snapmap.github.io/snapmap-plus](https://doom-snapmap.github.io/snapmap-plus/)) — deployed by `.github/workflows/pages.yml` |

`build/` and `dist/` are gitignored — the **source is the deliverable**; the binaries are rebuilt.

## Quick start (players)

You do **not** need to build anything. Get `snapmap-plus.exe` from the latest release and **double-click it** — it
auto-detects your DOOM install via Steam, asks you to confirm, and installs. (From a terminal: `snapmap-plus install`.)

`snapmap-plus.exe` installs itself to `%LOCALAPPDATA%\snapmap-plus\` (also the home of your settings /
overrides / prefabs / rawmaps). Run it again any time for `snapmap-plus update`, `snapmap-plus status`,
`snapmap-plus version`, and `snapmap-plus uninstall` (which restores DOOM to vanilla and leaves your
modding data untouched). Coming from the **original SnapHak**? Install/update detects it in
your DOOM folder and removes its files as part of the install — your maps, prefabs and overrides carry
straight over. See [`installer/README.md`](installer/README.md).

> Releases are produced by CI. Until the first release is published, build from source (below).

## Build from source

**Requirements** (exact download links + setup are in [`docs/contributing.md`](docs/contributing.md))
- **MSVC 2022 Build Tools** (the "Desktop development with C++" workload)
- **Go 1.21+** (only to build the installer)

The frontend renders in the Microsoft Edge **WebView2 runtime** (preinstalled on Windows 11 / most Windows 10);
its SDK is auto-fetched from NuGet at build time. Nothing else to install.

```powershell
# 1. compile both DLLs -> build/XINPUT1_3.dll + build/webview/snapmap-plus-ui.dll
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build.ps1

# 2. assemble the deployable overlay -> dist/ (the 2-file DOOM tree: the two clone DLLs)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File package.ps1

# 3. (optional) build the installer
cd installer ; go build -o snapmap-plus.exe .
```

## Deploy a local build (contributors / testing)

Deploy your fresh `dist/` into your own DOOM with the installer's **local** mode — the same path end users
take, just from your build instead of a release:

```
installer\snapmap-plus.exe install --local dist
```

`snapmap-plus.exe uninstall` reverses it. (Or drop `dist\*` into the DOOM root by hand — `dist/` mirrors the exact
overlay tree.) Launch DOOM, enter the SnapMap editor; the Snapmap+ window opens (run `sh` in the
console if it doesn't). DOOM keeps using the real `XInput1_3.dll` in System32 for controller input — the
backend forwards every XInput export through to it.

## Versioning & releases

Versions follow **semantic versioning** — `vMAJOR.MINOR.PATCH` (e.g. `v0.1.0`). **The git tag is the version**;
there is no `VERSION` file to maintain. One tag = one release containing **both** the mod bundle and
`snapmap-plus.exe`, both stamped with that tag.

Cut a release (maintainer):

```
git tag v0.1.0
git push origin v0.1.0      # fires .github/workflows/release.yml
```

CI builds the DLLs + the installer (stamping `snapmap-plus.exe` via `-ldflags -X main.version=v0.1.0`), packages the
overlay, and publishes a GitHub Release with `snapmap-plus-bundle.zip` + `snapmap-plus.exe` + `install.ps1`.

**Release channels** (set by the *tag*, not a branch):
- **Stable** — a plain tag `v0.3.0`. This is what end users' `snapmap-plus update` gets.
- **Beta** — a pre-release tag `v0.3.0-beta.1` (any tag with a `-`; CI auto-marks it a GitHub pre-release). It's
  excluded from "latest", so end users never receive it. Beta testers opt in:
  `snapmap-plus install --release v0.3.0-beta.1`.

Pin any version explicitly with `--release <tag>` on `install` or `update`.

- **`snapmap-plus version`** prints the installer's version (and the installed mod version, if any).
- **`snapmap-plus update`** pulls the latest release; **`snapmap-plus status`** shows what's installed.
- A local/dev build reports `dev` (unstamped) or `local` (a `--local` install) — never a release number.

**Surviving DOOM updates (planned):** the clone resolves engine functions by *signature*, so many DOOM patches
need no rebuild at all. When a patch shifts things enough to require one, an **auto-re-patcher** CI job
(re-resolve signatures against the new DOOM build → rebuild → if green, publish a compatible release) is the
intended automation. Stubbed for now (see `release.yml`).

## Contributing

Contributions are welcome. **New here?** The full guide — fresh-machine setup (Git, MSVC, Go), the
build → package → test loop, the pull-request workflow, and the rule that the `docs/` are updated alongside
code — is in **[`docs/contributing.md`](docs/contributing.md)**. The short version:

1. **Fork** this repo (or branch, if you have write access).
2. Make your change under `src/`. Build (`build.ps1`), package (`package.ps1`), and test it in your own DOOM
   via `installer\snapmap-plus.exe install --local dist`.
3. Open a **pull request** against `main`. The CI gate runs a security scan (no new binaries · capability-surface
   scan · gitleaks), the Windows build + package, the XInput ordinal-parity check, the 31-test native suite
   (`tests\run-tests.ps1`), and the installer's `gofmt`/`vet`/`test`; a maintainer reviews and merges. Tagged,
   reviewed commits are what produce releases.

**Keep PRs clean:**
- **No binaries.** Never commit a `.dll`/`.exe`/`.obj`/etc. — they're gitignored and **CI rejects any PR that
  adds one**. The source is the only deliverable; CI builds the binaries.
- **Clean-room only.** Contribute your **own** RE/implementation. Do not paste decompiled or copyrighted
  DOOM/SnapHak content.
- **Match the surrounding code** — the backend is plain C, the frontend is C++ + HTML/CSS/JS; keep source **pure ASCII** (the
  PowerShell build reads BOM-less UTF-8 as Windows-1252). Run **`gofmt`** on anything in `installer/`.

Because the tool injects into DOOM, the release channel is a supply-chain target. PR CI runs in a
**secretless** sandbox (it cannot publish or touch signing keys), a maintainer reviews every diff, and a scan
flags any newly-introduced network / process-spawn / persistence code — the tool has no legitimate reason to
do any of that.

### Generated headers — don't hand-edit

A few committed headers are **generated data tables** derived from the project's reverse-engineering of the
engine and the original tool, not hand-authored source: `src/ui/sh_*.h` (entity descriptions, event
catalog/docs, asset lists) and `src/backend/class_universe.h`. They're checked in
so the repo builds standalone — treat them as **vendored**: don't hand-edit them in a PR; open an issue
describing the change instead.

## Architecture & reference

| Doc | What |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | the backend↔frontend boundary, the WebView2 host, the 30 Hz think-loop, the 77 original slots + append-only extension ABI, and persistent configuration |
| [`docs/fidelity.md`](docs/fidelity.md) | the original's quirks the clone reproduces on purpose, and the one sanctioned divergence (the fault-shield) |
| [`docs/capabilities.md`](docs/capabilities.md) | the full feature inventory — every console command, cvar, SnapStack op, and GUI tab |
| [`docs/packaging.md`](docs/packaging.md) | the deployable bundle: the lean 2-file overlay |

## Persistent settings

The backend owns `%LOCALAPPDATA%\snapmap-plus\config.json`. It creates the file on the first Snapmap+
startup after install (not during installation), and the Studio window's **Light / Dark**, **Show hidden**,
and Entity selection-direction choices are registered settings. The same registry owns whether player
override files load:

```json
{
  "schema_version": 1,
  "settings": {
    "theme": "light",
    "entities.show_hidden": false,
    "entities.selection_mode": "off",
    "overrides.user_enabled": true
  }
}
```

`entities.selection_mode` accepts `off`, `follow`, and `select_in_3d`; the two Entity selection directions
cannot be active together. `overrides.user_enabled` defaults to `true`. These choices survive restarts,
updates, uninstall, and reinstall. Deleting `config.json` is the supported "reset preferences" operation:
the next startup recreates the light theme, Show Hidden off, selection mode off, and user overrides enabled
defaults. Manual edits to the file are read at the next startup; a successful console write recreates a
deleted file through the same settings service. The generic backend↔frontend settings bridge already permits a future
frontend control for this setting. Unknown values are preserved when a supported-schema file is rewritten.
See [`docs/architecture.md`](docs/architecture.md) for validation, recovery, and I/O-failure behavior.

## Overrides and new decls (runtime)

Snapmap+ has three complementary runtime paths under `%LOCALAPPDATA%\snapmap-plus\overrides\`:

- **Existing decl identity:** the ordinary resource-loader file shadow remains unchanged. Your file wins
  while the user layer is enabled, then Snapmap+'s few built-in defaults (the "*Custom" palette tab, served
  from memory), then DOOM's packaged resource.
- **Genuinely new decl identity:** put a `.decl` below `generated\decls\<type>\`. At cold start the decl server
  derives the engine type from the first directory and the logical name from the remaining path. On DOOM's
  main thread it excludes identities already in the registry, copies the absent set into one immutable
  per-decl table keyed by `decltree\<type>\<logical-name>.decl`, and submits each source once through DOOM's
  native catalog scanner in dependency order. For
  example, this file:

  ```text
  overrides\generated\decls\actormodifier\actormodifier\demon\cacodemon.decl
  ```

  registers type `actormodifier` with logical name `actormodifier/demon/cacodemon`. If that identity already
  exists, the decl server does not replace it; the file stays on the ordinary shadow path instead.
- **Game-owned resource dependency:** put a metadata manifest below `generated\resources\`. Each row names an
  exact resource already present in the player's installed `gameresources.pindex`. Snapmap+ validates the
  complete manifest at cold start, then reads and decodes only the selected archive slice when DOOM requests
  it. The archives, indexes, verification sidecars, executable, and `common.mapResources` remain untouched.
  This can make campaign assets already shipped with DOOM available to a SnapMap override package; it cannot
  supply assets that are absent from the local installation or distribute them inside a published map.

Discovery is one immutable snapshot per DOOM process. There is no refresh, watcher, hot reload, or unload;
restart DOOM after adding or changing files. The backend log records existing or rejected candidates as
`SHADOWED` or `REFUSED`, then records each `MISSING` identity's native registration path and one terminal
result for the ordered sequence. A published table entry is served before ordinary layers, so a loose physical
file cannot shadow that exact new identity. DOOM's scanner remains the semantic authority for parse/materialization
errors; the log does not claim that an individual object materialized solely because its source scan returned true.
  The catalog creates text decl identities; referenced models, sounds, images, and other binary dependencies
  must be supplied by the installed-resource manifest path or already be available to the base game.
  The resource-stream ABI is currently pinned to the audited Steam build; an incompatible DOOM build is
  refused before the provider hook or dynamic decl server is published.

A broken user set can be bisected with `sh_user_overrides 0` (restore with `sh_user_overrides 1`). The command
saves `overrides.user_enabled` only when persistence succeeds and requires a DOOM restart; it disables both
the user file-shadow and user new-decl registration for the next launch, while built-in defaults and packaged
resources remain available. Run it without an argument to inspect this launch and the saved next-launch
state. Runtime logs go to
`<DOOM>\snapmap-plus\logs\`. (Content from the original SnapHak's / older releases' `%USERPROFILE%\snaphak`
folder is copied forward on install; a legacy root-level `snaphak_logs\` is folded in too.)

## Credits

Snapmap+ stands on the shoulders of **SnapHak**, the original closed-source SnapMap modding tool by
**Chrispy**. The original pioneered the rawmap format, the override file-shadow, the hidden-entity
unhide, and the in-editor companion window that this project reimplements clean-room — every feature
here traces back to what that tool proved possible. Thank you, Chrispy.

## License

MIT — see [`LICENSE`](LICENSE).
