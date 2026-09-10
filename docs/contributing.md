# Contributing to Snapmap+

Snapmap+ (repo `snapmap-plus`) is an open-source, clean-room reimplementation of **SnapHak** — Chrispy's closed-source modding
tool for DOOM 2016's in-game **SnapMap** level editor. It builds to two drop-in Windows DLLs (a backend
`XINPUT1_3.dll` and a WebView2/HTML frontend `snapmap-plus-ui.dll`) plus a Go installer (`snapmap-plus.exe`). This guide takes you
from a **fresh Windows machine** all the way to a built, tested change and an open pull request.

If anything here is wrong, missing, or unclear, fixing it is itself a welcome PR.

## Contents

1. [Ground rules](#1-ground-rules)
2. [Prerequisites (fresh Windows machine)](#2-prerequisites-fresh-windows-machine)
3. [Get the source](#3-get-the-source)
4. [Build the DLLs](#4-build-the-dlls)
5. [Package the overlay](#5-package-the-overlay)
6. [Deploy and test in DOOM](#6-deploy-and-test-in-doom)
7. [Run the tests](#7-run-the-tests)
8. [The pull-request workflow](#8-the-pull-request-workflow)
9. [Keep the docs in sync (required)](#9-keep-the-docs-in-sync-required)
10. [Generated headers — don't hand-edit](#10-generated-headers--dont-hand-edit)
11. [Reporting security issues](#11-reporting-security-issues)
12. [Repository layout](#12-repository-layout)
13. [Glossary](#13-glossary)

## 1. Ground rules

- **Clean-room only.** Contribute your **own** reverse-engineering and implementation. Never paste decompiled,
  disassembled, or copyrighted DOOM or original-SnapHak content into this repo. This repo ships no DOOM or
  SnapHak bytes, and it must stay that way.
- **No binaries — ever.** `.dll`, `.exe`, `.obj`, `.lib`, `.pdb`, `.zip`, … are gitignored, and CI **rejects
  any PR that adds one**. The source is the only deliverable; CI builds the binaries.
- **Pure ASCII source.** The PowerShell build reads BOM-less UTF-8 as Windows-1252, so keep `.c` / `.h` /
  `.cpp` / `.ps1` files ASCII-only (no smart quotes, em dashes, or accented characters in source).
- **Match the surrounding code.** The backend is plain C; the frontend is C++ (the WebView2 host) + HTML/CSS/JS
  in the WebView page and its local modules; the installer is Go (run `gofmt`).
- **Supply-chain awareness.** Because the tool loads into DOOM, releases are a supply-chain target. PR CI runs
  in a secretless sandbox (it can't publish or touch signing keys), a maintainer reviews every diff, and a scan
  flags any new network / process-spawn / persistence code — the tool has no legitimate reason for any of that.

## 2. Prerequisites (fresh Windows machine)

You need 64-bit **Windows 10 or 11** and the tools below. Install them in this order.

| # | Tool | Version | Get it from |
|---|---|---|---|
| 1 | **Git** | any recent | <https://git-scm.com/download/win> |
| 2 | **Visual Studio 2022 Build Tools** | 2022 (v17) | <https://aka.ms/vs/17/release/vs_BuildTools.exe> |
| 3 | **Go** | 1.21+ | <https://go.dev/dl/> |
| 4 | **DOOM 2016** | Steam (app 379720) | required to actually run/test the mod |

The frontend renders in the Microsoft Edge **WebView2 runtime**, preinstalled on Windows 11 and on most
Windows 10 (via Edge) — nothing to install to build or run. (Its SDK headers + static loader are fetched
from NuGet at build time; there is nothing else to install.)

**Visual Studio 2022 Build Tools.** Run the installer and tick the **"Desktop development with C++"**
workload. That installs the MSVC x64 compiler (`cl.exe`) and the Windows 10/11 SDK the build needs. The build
locates the toolchain with `vswhere`, requiring the `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`
component — included in that workload. You do **not** need the full Visual Studio IDE; the Build Tools suffice.

**Go** is only needed to build the installer (`snapmap-plus.exe`); you can skip it if you only touch the DLLs.

**DOOM 2016** (Steam app `379720`) is required to deploy and test a build in the game.

> PowerShell tip: the build scripts are run with `-ExecutionPolicy Bypass` (shown below), so you don't need to
> change your machine's execution policy.

## 3. Get the source

Fork the repo on GitHub (the **Fork** button) unless you have write access, then:

```powershell
git clone https://github.com/<your-username>/snapmap-plus.git
cd snapmap-plus
```

## 4. Build the DLLs

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

This compiles both DLLs into **`build/`**: the backend `XINPUT1_3.dll` and the frontend
`build/webview/snapmap-plus-ui.dll` (the WebView2 SDK is auto-fetched from NuGet on the first build). `build/`
is gitignored. (`build.ps1` first builds the backend, then the frontend, so the two never drift out of ABI
sync; their shared ABI is declared in `src/common/snapmap_plus_iface.h`.)

`build.ps1` is the one top-level build script (used above and by CI). Pass `-BackendOnly` to skip the
frontend when iterating on backend code alone; any extra args (e.g. `-Diag`) forward through to
`src\backend\build.ps1`. Frontend sources are in `src/ui/webview/`.

## 5. Package the overlay

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
```

This assembles the deployable **2-file overlay** into **`dist/`**: the two clone DLLs (the frontend
renders in the system-installed WebView2 runtime), laid out exactly as they drop into a DOOM install,
alongside a `MANIFEST.sha256`. `dist/` is gitignored. The manifest records both DLLs;
no game assets or per-user data are bundled.

## 6. Deploy and test in DOOM

Build the installer once, then deploy your fresh `dist/` into your own DOOM with its **local** mode:

```powershell
cd installer ; go build -o snapmap-plus.exe . ; cd ..
installer\snapmap-plus.exe install --local dist
```

It auto-detects your DOOM via Steam (or pass `--doom <path>` to your DOOM 2016 folder, the one holding
`DOOMx64vk.exe` and `DOOMx64.exe`), backs up
anything it replaces, and records the install so **`installer\snapmap-plus.exe uninstall`** restores vanilla
exactly. (You can also drop `dist\*` into the DOOM root by hand — `dist/` mirrors the overlay tree.)

Launch DOOM and enter the SnapMap editor; the **Snapmap+** window opens (run `sh` in the in-game
console if it doesn't). Either renderer will do — the backend attaches to whichever process image it was
loaded into, so `DOOMx64vk.exe` and `DOOMx64.exe` are both fine (see `-DoomAlt` below). When you're done,
`snapmap-plus.exe uninstall` returns DOOM to vanilla and leaves your modding data
(`%LOCALAPPDATA%\snapmap-plus`) untouched.

## 7. Run the tests

**The installer (Go):**

```powershell
cd installer
gofmt -l .            # must print nothing (format with: gofmt -w .)
go vet ./...
go test ./...
cd ..
```

**The native unit tests:**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1
```

The runner builds and executes the self-contained native suites, then runs the
JavaScript suites with Node 22.13 or newer. No game is needed for these checks.
They cover patch ownership and rollback, edit dispatch, package and resource
lifetimes, navigation, crash records, previews, frontend contracts and Worker
request handling. The current test manifest is `tests/run-tests.ps1`.

Run the Worker runtime suite as well when changing either service. It verifies
concurrent quotas, persistence and uploads against local storage and a GitHub
mock. See [tests/README.md](../tests/README.md) for its pinned dependency and
command. CI runs both the normal suite and the Worker runtime suite.

Three more tests scan a **real DOOM image** — an executable that's been unpacked from its Steam DRM wrapper
(e.g. with Steamless). **Running these is REQUIRED if you add or change any entry in the engine signature
table** in `src/backend/signatures.c` **or in the engine-globals table** — they are the only thing that
checks a `known_rva`, and CI cannot run them (it has no game image), so a green CI does not cover you:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1 -Doom C:\path\to\unpacked-DOOMx64vk.exe
#   sig_test     -- every engine signature resolves to its known RVA
#   hooktol_test -- the resolver's hook-tolerant fallback (prologue-clobbered functions)
#   globals_test -- every engine data global resolves, and its layout invariants hold
```

### `-DoomAlt`: the two-executable portability gate

**DOOM 2016 ships two executables, and Snapmap+ has to work in both.** `DOOMx64vk.exe` (Vulkan) and
`DOOMx64.exe` (OpenGL) are built from one source tree and linked one second apart; their import tables are
identical except for `vulkan-1.dll` vs `OPENGL32.dll`, both import `XINPUT1_3.dll`, and the game
*relaunches itself into the other one* when the `r_renderAPI` cvar changes (`0` = OpenGL, `1` = Vulkan).
There is no separate launcher binary and no Steam launch option that picks a renderer, so a player can land
in either from a single launch and we get loaded into whichever it is. Struct field layouts are identical
between the two; only addresses move — and they move by different amounts for code (roughly `-0x400` to
`-0xE460`) and for data (roughly `+0xE00000` to `+0x1000000`), with no uniform delta anywhere.

So: **a resolver that works on one image is not finished until it works on the other.** Point `-DoomAlt` at
the second unpacked executable and the suite re-runs `sig_test` and `globals_test` against it in portable
mode, where the expected RVA is not checked but *uniqueness* is:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-tests.ps1 `
  -Doom C:\path\to\unpacked-DOOMx64vk.exe -DoomAlt C:\path\to\unpacked-DOOMx64.exe
```

The runner reports the current signature and engine-global counts and requires
each entry to resolve uniquely on both images.

**Why uniqueness on both images is the actual bar.** A pattern that matches exactly once on one image tells
you almost nothing: with a few thousand functions to miss, plenty of byte strings are unique by accident.
A pattern that matches exactly once on *two independently linked images of the same code* is far more
likely to be describing the function itself rather than an accident of one link. A signature unique on only
one image is a coincidence, not an identity, and `-DoomAlt` is the only thing in the tree that can tell the
difference.

The real example is already in the source. `RenderLogStub`'s pattern is `mov [rsp+0x20],r9; ret` plus
padding — that is a *shape*, not an identity, and any number of tiny stub functions could have it. It
happens to resolve uniquely on both images, so it passes, but it passes by luck. If you find yourself
writing a pattern that short, get more bytes or anchor on something the function actually does.

### Which DOOM build addresses must come from

This matters more than it looks, and it is easy to get wrong without noticing.

Every `known_rva` in the signature table and every `pinned_rva` in the engine-globals table comes from
**one** image — the SteamStub-wrapped `DOOMx64vk.exe` with SHA256
`139763E94F1A75B5310179F9EEEB8A949A1F53C49ACBC722FCFC5DFE7BB6D323`. That is **not** the build Steam
currently ships, and it is not the OpenGL executable either. If your Ghidra project, your debugger, or your
unpacked image is anything else, every raw address you read is for a different image and must be translated
before it goes into `known_rva` or a call site. There is no single offset that converts between them: a
different build is a cluster-wise re-link, so different functions move by different — and sometimes
opposite-signed — amounts.

Normal resolution scans function bytes or decodes a global's RIP-relative code
anchor. Raw-RVA fallbacks require an exact SHA-256 match of the process's backing
executable against the reference hashes in `host_image.c`. This recognizes the
wrapped reference and its verified unpacked copy without depending on code bytes
that installed hooks have changed. The hook-tolerant fallback also requires a
recognized detour and at least six matching fixed tail bytes. Portable scans
remain available on other supported images; an unverified file cannot authorize
a raw-RVA fallback.

The trap is that a wrong address is **invisible in normal use**. Byte signatures are build-portable, so the
scanner still finds the function and your feature works exactly as intended on either build. The only thing
that breaks is the hook-tolerant fallback, which just quietly stops covering that function. `sig_test` is
what catches it — hence "required" above.

If you deliberately want to record an address from another build or the other executable (useful — we do
it), label it as such in the comment rather than putting it in a field that means "the pinned image".

An entry with `known_rva=0` explicitly has no pinned-build fallback. It must
still resolve uniquely in both images; the tests skip only its nonexistent
pinned-address comparison, not signature resolution. The weapon HUD entries
use this form.

A third test, `xinput_ordinal_test.c`, is a **runtime** cross-check of the XInput ordinal invariant — it loads
a built DLL and calls its exports by ordinal. CI verifies that same invariant *statically* with `dumpbin` (the
"XInput ordinal parity" step), so you normally don't need to run it by hand.

CI runs the self-contained native tests, JavaScript tests, and installer tests on every PR; the DOOM-image tests are local-only
(CI has no game image).

After the normal test run has built its executables, contributors with DOOM installed can also
exercise catalog parsing and metadata compaction against their own installed files:

```powershell
tests\obj\imgpreview_index_test.exe --catalog-root "C:\path\to\DOOM\base"
tests\obj\megapreview_io_test.exe --virtualtextures-root "C:\path\to\DOOM\virtualtextures"
```

These optional checks print recognized-record counts, streamed and retained metadata sizes, and the
Mega2 table bytes left on disk. They read the indexes, Wwise manifest, VMTR tables, and Mega2 headers
in place; they do not write to the game directory.

### Website changelog

The website in `site/` renders release notes from `CHANGELOG.md` through
`tools/changelog.py` and Jekyll. Each release has a Lucide copy button beside
"View on GitHub" that copies its title, date, complete notes, and release link
as Markdown for community announcements, including Discord. Successful copies
show a green check for three seconds; failed copies show an inline message.
The button is available when JavaScript and the browser Clipboard API are enabled.
When changing this feature, verify the copied content, success/reset and failure
states, keyboard access, and narrow layouts in a browser.

## 8. The pull-request workflow

1. **Branch:** `git switch -c fix/steam-path-detection` (or `feature/<thing>`).
2. **Change** code under `src/` (or `installer/`). Keep each PR focused on one thing.
3. **Build + package + test in DOOM:** `build.ps1` → `package.ps1` → `snapmap-plus.exe install --local dist`.
   A local round-trip is much faster than waiting on CI, and lets you actually see it working in DOOM rather
   than just "the build didn't fail." Always deploy both DLLs from the same build because their
   interface layouts must match.
4. **Run the tests** (section 7) — both the Go and C suites.
5. **Update the docs** your change affects (section 9).
6. **Commit** with a clear, imperative subject; a simple area prefix such as `installer:` or `backend:` is
   fine. Follow it with a body of **one to three short sentences** -- see
   [Writing the commit body](#writing-the-commit-body) below, which is a hard requirement, not a style
   preference. Do not add agent/tool attribution or generated-by trailers.
7. **Push** to your fork and open a **pull request against `main`**.
8. The **CI gate** runs automatically, as two parallel jobs: a security scan (no-new-binaries ·
   capability-surface scan · gitleaks); and the build (`build.ps1` / `package.ps1`, a bundle guard that
   asserts the overlay stays the lean 2-file set, XInput ordinal parity, the native unit tests, and the installer's
   `gofmt` / `vet` / `test`). It runs in a **secretless** sandbox — fork PRs get a read-only token and zero
   repo secrets.
9. A **maintainer reviews** and merges (changes under `.github/`, `*.ps1`, and `installer/` are
   CODEOWNERS-gated). Releases are cut from reviewed, tagged commits — see the README's
   ["Versioning & releases"](../README.md#versioning--releases).

### Writing the commit body

**Your commit body is read by the next maintainer, and by the tool that drafts the changelog.** It is no
longer published verbatim: the release notes users read live in [`CHANGELOG.md`](../CHANGELOG.md), written
once per release and reviewed before it ships. That frees a commit body to explain a change properly.

Still keep it short -- **one to three sentences, about 60 words, never more than 80.** A body that says what
changed and why is what makes a good changelog entry possible; an engineering write-up buries it.

> Prefab previews now keep a prop's saved rotation and a block's real dimensions instead of resetting them.
> Logic hexagons stay full size while I/O circles and filter diamonds render smaller, so a graph is easier to
> read at a glance.

Keep out of it: "what changed / why / status" headings, checklists, and any restatement of the subject line.
Symbol names and file paths are fine when they are what the sentence is actually about -- the body is not
published, so write it for a reader who has the diff open. Longer rationale, evidence and alternatives belong
in the pull-request description or in `docs/`, neither of which is published.

Commits whose subject starts with an internal prefix (`ci:`, `chore:`, `scrub:`, `docs:`, `test:`/`tests:`,
`release:`, `refactor:`, `build:`, `style:`, `meta:`) are filtered out of the changelog entirely and are free
to say whatever is useful. The canonical list is `INTERNAL_PREFIX_RE` in
[`tools/draft_changelog.py`](../tools/draft_changelog.py); if you change one, change both.

### Cutting a release (maintainer)

1. `gh workflow run prepare-release.yml -f version=v0.2.2-beta.1` -- drafts that release's `CHANGELOG.md`
   entry and opens a pull request. It refuses a version that is already tagged, already has an entry, or
   already has an open branch, and it refuses a malformed version string before spending anything.
2. Review the pull request. Its description lists which commit backs each bullet, so a claim can be
   spot-checked without reading the whole range. Edit the entry directly on the branch if a sentence is
   wrong -- the pull-request gate re-checks the grammar.

   **Read every bullet as a claim about the product, not as prose.** The drafter is given the commits and
   the diff of the user-facing docs, and it is told never to name a part of the interface those sources do
   not name; a draft that does is rejected outright. That guard exists because it happened: the first
   v0.2.1-beta.8 draft opened with "A Navigation tab lets you mark which Blocking Box surfaces demons are
   allowed to walk on" when no Navigation tab was ever built -- the feature is a property in DOOM's own
   object settings, and the word appeared in no commit. Nothing downstream can tell a plausible sentence
   from a true one. This review is the only place that happens, which is also why section 9's rule that a
   behaviour change updates the docs in the SAME pull request matters here: those docs are what the drafter
   is grounded on, and a release whose docs were not updated gives it nothing to be right from.
3. Merge it.
4. `git tag v0.2.2-beta.1 && git push origin v0.2.2-beta.1` -- **do this immediately after merging.** Merging
   redeploys the site, so between the merge and the tag the changelog page lists a release that cannot yet be
   downloaded.
5. `release.yml` refuses to build a tag with no entry, so step 4 cannot silently ship notes-free.

If drafting fails -- an outage, a rate limit, a declined request -- the pull request still opens, carrying the
raw commit list marked `NEEDS WRITING`. Rewrite it by hand and merge as usual. A drafting failure never blocks
a release.

If the pull request itself cannot be opened -- "Allow GitHub Actions to create and approve pull requests" is
off, possibly locked by an organisation policy -- the draft is still pushed to `changelog/<version>` and the
job log prints a compare link. Open it by hand; nothing is lost.

Correcting a past entry: edit it in `CHANGELOG.md`, then run
`python3 tools/sync_release_notes.py --apply` to push the corrected text onto the already-published GitHub
Release. Without that, `CHANGELOG.md` is the source of truth only for releases cut after it existed. The tool
is a dry run by default and saves the current bodies before changing anything.

The first stable release is a special case: there is no earlier stable tag to compare against, so the drafter
would receive the entire history. Write that entry by hand.

Before merging, the maintainer reads the complete pull request: its description, every commit message, the
full diff, review discussion, linked issues, and reported validation. Coherent commits are rebased and
preserved. Fixup-heavy history may be squashed, and the maintainer then writes a **fresh** subject and body for
the whole pull request under the rule above -- never the individual commit bodies pasted end to end, which is
exactly how a release ends up with kilobyte-long notes. A merge commit is reserved for cases where retaining
the original commit IDs or branch topology has concrete value. Every merge method must preserve the
contributor's authorship and credit.

## 9. Keep the docs in sync (required)

Update the instructions affected by a behavior change in the same contribution.
Player workflows belong in the website guide; installation belongs in the root
and installer READMEs; build, test and release procedures belong here; service
or credential changes belong in [services.md](services.md).

Retain contributor guides and cross-component contracts in `docs/`. Update
[architecture](architecture.md) when boundaries, ownership or threading change;
[WebView UI](webview-ui.md) when frontend development changes; and
[packaging](packaging.md) when the shipped layout changes. The
[capabilities inventory](capabilities.md), [compatibility guide](fidelity.md) and
[feedback pipeline](feedback.md) explain supported workflows and shared behavior.

Explain narrow backend algorithms in source and concise comments. Their detailed
investigations and historical evidence belong in snaphak-re's findings system.
External contributors can include evidence and rationale in their pull request;
the product must build and remain understandable without a research checkout.

Signature and engine-global changes still require the two-image portability
gate above. Report its result because CI cannot access those game images.

### Documentation and comment style

Keep `docs/` focused on contribution, operation and larger component boundaries.
Do not duplicate a backend implementation in its own feature document. Put
release history in `CHANGELOG.md`. Use focused directory READMEs and comments
for responsibilities, limits and non-obvious decisions.

Main directories such as `src/`, `tests/` and `tools/` have a `README.md` using
this opening structure. Keep nested guides only where they explain a separate
workflow; do not add a README to every subdirectory.

```markdown
# Directory or component name

## Purpose

One short paragraph explaining what this directory owns.

## Contents

A short list or table of the important files and subdirectories.

## Working here

Entry points, relevant commands, constraints, and links to detailed guidance.
```

Add focused sections when a component needs setup or usage instructions. Keep
file lists selective and responsibilities precise; avoid repeating full feature
inventories in several READMEs. Generated build outputs and dependency caches
do not need repository documentation. Website READMEs are contributor files and
are excluded from the published site.

Comments should explain a non-obvious decision, contract, limit or failure mode.
Keep ABI offsets, ownership, thread requirements and porting clues that a reader
cannot infer from the code. Remove stale investigation history, decorative
banners and line-by-line narration. Preserve license notices, generated content,
build directives and analyzer annotations. Do not reduce comments to meet a
percentage target.

## 10. Generated headers — don't hand-edit

A few committed headers are **generated data tables**, not hand-authored source: `src/ui/sh_*.h` (entity
descriptions, the event catalog/docs, asset lists), `src/backend/class_universe.h`, and
`src/backend/engine_globals_table.gen.h` (the engine data-global anchors, each verified to match exactly once
on both shipped executables). They're checked in so the repo builds standalone — treat them as **vendored**.
Don't hand-edit them in a PR; open an issue describing the change you need instead.

## 11. Reporting security issues

This tool injects into DOOM, so a vulnerability here is a supply-chain risk for everyone who installs a
release. **Do not open a public issue for a security problem.** Use GitHub's **private vulnerability reporting**
(the repo's **Security** tab → **Report a vulnerability**) so it can be fixed before disclosure.

## 12. Repository layout

| Path | What |
|---|---|
| `src/backend/` | the backend DLL (`XINPUT1_3.dll`): the hook layer, console commands, cvars, persistent configuration, cvar-unlock, the resident fault-shield |
| `src/ui/` | the frontend DLL (`snapmap-plus-ui.dll`): the WebView2 host, HTML page, styles, declaration module and native message codecs |
| `src/fault_shield/` | the recover-in-place vectored-exception fault shield (compiled into the backend) |
| `src/common/` | the shared backend↔frontend interface ABI (`snapmap_plus_iface.h`) |
| `installer/` | `snapmap-plus.exe` — the Go install / update / uninstall CLI |
| `tests/` | the native unit tests + `run-tests.ps1` |
| `tools/` | the changelog parser (`changelog.py`), the release-notes drafter (`draft_changelog.py`), the published-release sync (`sync_release_notes.py`) and their tests |
| `CHANGELOG.md` | the user-facing release notes -- the single source every consumer reads |
| `docs/` | contributor guides and cross-component contracts, indexed in [README.md](README.md) |
| `build.ps1` | compile the DLLs → `build/` (backend + frontend; `-BackendOnly` for backend alone) |
| `package.ps1` | assemble the deployable overlay → `dist/` (the two clone DLLs) |
| `.github/workflows/` | `ci.yml` (the PR gate) · `prepare-release.yml` (drafts a release's notes) · `release.yml` (tag-triggered release) · `pages.yml` (the website) |
| `LICENSE` | MIT |

## 13. Glossary

- **SnapMap** — DOOM 2016's in-game level editor. **Snapmap+** extends it.
- **The original SnapHak / "OG"** — Chrispy's closed-source tool that this project reimplements clean-room.
  **"The clone"** — this project's reimplementation.
- **The overlay** — the two files that deploy into a DOOM install (the backend + frontend DLLs).
- **Backend / frontend** — the backend `XINPUT1_3.dll` (the engine-side hook layer) and the frontend
  `snapmap-plus-ui.dll` (the WebView2/HTML UI); they talk over the interface ABI in `src/common/`.
- **`XINPUT1_3.dll` / ordinals** — the backend ships as an XInput proxy DLL DOOM already loads. It must export
  `XInputGetState` / `XInputSetState` at ordinals **2 / 3** (DOOM imports them *by ordinal*) and forwards every
  XInput call through to the real `System32` DLL, so the controller keeps working.
- **RVA** — relative virtual address: an offset into the DOOM image. **Signature ("sig") resolve** — locating
  an engine function by a masked byte-pattern instead of a hardcoded RVA, so the clone survives DOOM patches
  that shift addresses.
- **decl** — a DOOM engine declaration (an entity or resource definition). **cvar** — an engine console
  variable. **cvar-unlock** — re-enabling editor cvars the engine hides by default.
- **The fault-shield** — a vectored-exception handler that recovers in place from certain faults instead of
  letting the process die. Recovery is limited to the cases implemented in `src/fault_shield/`.
