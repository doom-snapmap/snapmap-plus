# The Snapmap+ frontend (WebView2 / HTML)

`snapmap-plus-ui.dll` renders the Snapmap+ UI as HTML/CSS/JS in a Microsoft Edge **WebView2** control.
It is **the** frontend: the `sh_ui_init` entry (export ordinal 10), the backend interface contract
(`src/common/snapmap_plus_iface.h`), and a manual 30 Hz think-loop draining the work-queue (`+0x1a0`), driven
by the backend (`XINPUT1_3.dll`) over that interface.

**Status: the shipped frontend.** It is built by the default `build.ps1`, packaged by `package.ps1`,
and covered by CI. The UI renders in the system-installed WebView2 runtime, so no UI-toolkit runtime
ships in the overlay.

## Why HTML/WebView2

- Nothing to bundle: the UI renders in the system WebView2 runtime, keeping the overlay at two DLLs —
  and there is no UI SDK for contributors to install.
- Iterate on the UI in plain HTML/CSS/JS, with a browser preview mode for fast layout work.
- The entity list and the entity-state editor share a single view.

Trade-off: it depends on the Microsoft Edge **WebView2 runtime** (preinstalled on Windows 11 and most
Windows 10) instead of a bundled toolkit. The DLL statically links the WebView2 loader, so no extra loader
DLL ships.

## Files

| File | What |
|---|---|
| `src/ui/webview/snapmap_plus_ui_webview.cpp` | The WebView2 host: the `sh_ui_init` entry, a Win32 window, the WebView2 bring-up, the 30 Hz think-loop, and the JS <-> native bridge. |
| `src/ui/webview/mockup.html` | The UI (HTML/CSS/JS), embedded into the DLL at build time. Self-populates with sample data when opened in a plain browser (a "preview mode", inert in DOOM). |
| `src/ui/webview/prefab_viewport.js` | The Prefab Details WebGL2 renderer: local-scene reconstruction, orbit/zoom controls, installed-mesh transport, bounds/framing, and resize-driven drawing. The build inlines it into the embedded page. |
| `src/ui/webview/LUCIDE_LICENSE.md` | License notice for the small Lucide SVG subset embedded in `mockup.html`. |
| `src/ui/webview/theme_bootstrap.{h,cpp}` | The small pure helper that validates the registered theme JSON and seeds the embedded document's root class before WebView2 navigation. |
| `src/ui/build.ps1` | Builds `build/webview/snapmap-plus-ui.dll`: fetches the WebView2 SDK from NuGet into `build/` (gitignored), statically links the loader, embeds the HTML. Reuses `sl_exports.cpp` + `snapmap-plus-ui.def`. Invoked by the repo-root `build.ps1` (backend + frontend in lockstep). |

## Build + deploy

```powershell
# from the repo root -- builds the backend (build/XINPUT1_3.dll) + the frontend (build/webview/snapmap-plus-ui.dll)
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
# assemble the lean overlay (2 files: XINPUT1_3.dll + snapmap-plus/snapmap-plus-ui.dll)
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1           # -> dist/
installer\snapmap-plus.exe install --local dist --yes                     # deploy (DOOM must be closed)
```

`package.ps1` assembles a lean overlay that ships **only** the two clone DLLs (the WebView2 runtime is
system-installed). Runtime log: `<DOOM>\snapmap-plus\logs\snapmap-plus-ui.log` (called
`webview_poc.log` in v0.2.1-beta.5 and earlier).

## How it maps to the backend interface

The frontend holds no engine addresses; it calls the backend only through the vtable slots pinned in
`src/common/snapmap_plus_iface.h`:

| UI feature | Interface slot(s) |
|---|---|
| Entity list (valid ids, id-strings, displaynames) | `entity_count` +0x10, `is_valid_id` +0x28, `id_to_string` +0x18, `get_displayname` +0x58 |
| Hidden (dev-layer) filter | `id_dev_layer_hidden` +0x280 |
| Window shown only in the editor | `editor_ready_poll` +0x88 |
| State editor read | `get_declsource_copy` +0x30, `get_classname_copy` +0x48, `get_inherit_copy` +0x50, `get_displayname` +0x58 |
| Save to Decl | `apply_class_inherit` +0x268, `set_classname` +0x78, `set_inherit` +0x80, `set_entity_0x170` +0x128, `rebuild_set_declsource` +0x40 |
| Delete (context menu) | `selection_guard` +0x130 |
| Synchronize with editor (editor -> list) | `get_selection` +0x150 |
| Select in editor (list -> editor) | `clear_selection` +0x148, `add_to_selection` +0x138 |
| Class / Inherit autocomplete | `enum_valid_classes` +0x270, `enum_inherits` +0x278 |
| Camera origin footer (editable X/Y/Z and Lock position) | `get_editor_vec3` +0x08 publishes live coordinates; `camSet` / `camLock` write through `set_editor_vec3` +0x00 once or on every locked frame |
| Installed version readout | reads `%LOCALAPPDATA%\snapmap-plus\install.json` (written by the installer) |
| Persistent settings (Light / Dark and Entities controls) | `config_get_json` +0x2B0, `config_set_json` +0x2B8 — registered UTF-8 JSON fragments owned by the backend |
| Deselect (click a blank structural surface) | One page-level router calls `clear_selection` +0x148 for the app-wide entity selection and also tears down the active Prefab/Timeline selection; every right-hand inspection/editor pane, including expanded Decl Text, is protected workspace |
| List-driven selections behave natively (empty-space click deselects; Delete / Move / bottom-bar controls all apply) | `add_to_selection` +0x138, `clear_selection` +0x148 and `remove_from_selection` +0x130 additionally sync the editor's EntityMode selection-state field (`editor+0x22330`, state `+0x1ac`, dirty `+0xBB8`) -- the field the engine's own empty-space-click handler consults. Direct SEH-guarded field writes, gated on the editor already being in EntityMode. Re-derive recipe at the constant block in `src/backend/iface_engine.c`. |
| Entities list clears its highlight on a native deselect ("Select in 3D" mode) | the existing `selCount` broadcast (`get_selection` +0x150, ~330 ms poll); the UI now acts on its >0 -> 0 transition |
| Live Create-from-Selection icon state/count tooltip | `get_selection` +0x150, polled every ~330 ms independent of the sync checkboxes |
| Prefabs list, detail pane, delete/rename, folders (create/rename/delete/move) | `resolve_prefab_path` +0xc0 only -- pure Win32 file/directory ops (`FindFirstFileA`, `DeleteFileA`, `MoveFileA`, `CreateDirectoryA`, `RemoveDirectoryA`) on the resolved path. No other engine slot involved, unaffected by the +0xb0 issues below. |
| Create from selection | `serialize_selection` +0xb0 |
| Load / Place | `apply_edit` kind=1 (the same staging path `sh mkcmd` uses) -- stages into the paste slot only; the user presses Ctrl+V themselves. See [`webview-ui-history.md`](webview-ui-history.md) for why this is stage-only rather than fully automated. |
| Timelines list (dual-add `idTarget_Timeline` / `idEncounterManager`) | `get_classname_copy` +0x48 -- change-gated (see [`webview-ui-history.md`](webview-ui-history.md)), not a fixed timer |
| Open a timeline (tabs + events) | `serialize_entity` +0xc8 -- the same slot Save-to-Decl and Push-to-stack already use, JSON-parsed client-side |
| Timeline event-arg dropdowns (decl / enum / per-entity asset lists) | `enum_decls_of_resclass` +0x110 -- the same shared slot for both decl-name and enum-member enumeration |
| Save Timeline (commit `componentTimeLine` / `encounterComponent`) | `apply_edit` kind=0 -- the same path Save-to-Decl already uses, id-targeted instead of paste-targeted |
| Send feedback (the bottom-right help-icon dialog) | no engine slot -- the page posts `reportSubmit` with an opaque JSON payload; the host POSTs it to the feedback relay on a short-lived worker thread (WinHTTP, the frontend's only network touch -- see the capability note in `snapmap_plus_ui_webview.cpp`) and answers `reportResult {ok, mode, number}` -> green/red toast. Pipeline: [`feedback.md`](feedback.md) |
| Crash-report dialog (auto-opens on a recorded crash) | no engine slot -- the host polls `<game>\snapmap-plus\crash\` (~2 s) for a crash record the backend wrote at fault time and posts `crashPending {record, count}`; the page auto-opens the dialog. Send composes the payload host-side (`crashSubmit` -> `category:"crash"`, optional anonymized log tails) and rides the SAME WinHTTP thread + `reportResult` as feedback; `crashDismiss` clears the pending record. Pipeline: [`feedback.md`](feedback.md) |

### Configuration bridge and settings startup

The page never opens `%LOCALAPPDATA%\snapmap-plus\config.json` itself. Production reads and writes use
generic `configGet {key}` / `configSet {key, valueJson}` WebMessages; the host bounds and decodes those
fields, calls the backend's registered service through `+0x2B0` / `+0x2B8`, and returns `configValue`,
`configSetResult`, or the one-shot `configStatus`. Values stay as complete JSON fragments across the
boundary so a future non-string setting does not need a bespoke message or vtable slot.

Before `NavigateToString`, the host reads `theme`. For `"dark"` it injects `class="dark"` into the
embedded `<html lang="en">` marker; light and invalid/unavailable values retain the light default. The
native host starts hidden and can be shown only after a successful `NavigationCompleted`, so the first
visible frame already has the saved colors and a failed navigation never exposes a blank controller.

The page also sends one startup `configGet` each for `entities.show_hidden` and
`entities.selection_mode`. It aggregates both responses before changing either Entity control, so startup
side effects run only after the pair is known. The single selection-mode value makes Follow Selection and
Select in 3D mutually exclusive; restoring `select_in_3d` updates the control without pushing an empty
list selection into the editor. Choosing Light Theme or Dark Theme from View, clicking Show Hidden, or
changing either Entity direction applies the choice immediately, then asks the backend to persist it. A rejected or session-only result keeps the
session behavior but warns that it could not be saved. Startup status similarly warns when a corrupt file
was backed up, a newer schema was left untouched, or the service had to fall back to volatile settings.
`localStorage` is used **only** when `mockup.html` is opened directly in a normal browser (`PREVIEW`); it
is never the in-game source of truth. The file format and recovery rules are documented in
[`architecture.md`](architecture.md#persistent-configuration).

Heavy engine writes (Save, Delete, Select-in-editor) are snapshotted in the JS message callback and
applied on the next think-loop frame under the loop mutex, keeping them off the re-entrant callback.
That think-loop frame runs on the **frontend's own UI/think-loop thread**, not DOOM's main thread -- the
same thread where the inline class/inherit apply-guard (`sh_iface_class_inherit_ok`) fails open. The path
that actually runs on DOOM's main thread, guarded by `ExecuteCommandBuffer`, is the *scheduled* `+0xd0`
apply (`clone_bss_apply`) -- a different, deferred path, kept only as an old-backend fallback and for
prefab/mkcmd staging (see [`backend-changes.md`](backend-changes.md) for why decl-edits must NOT go
through it).

## Change history

Moved to [`webview-ui-history.md`](webview-ui-history.md).

## Known limitations / TODO

Genuinely open items only -- fixed bugs and completed work live in
[`webview-ui-history.md`](webview-ui-history.md), not here.

- **~~Save Timeline only works after a map save+reload~~ / ~~open question: zero feedback on a fresh
  entity~~ — both RESOLVED 2026-07-13.** These two former limitations were the same bug and are fixed. The
  "silently halts partway through the JS save handler, no exception, no hang, yet completes fine in the
  browser preview" mystery was an **uncaught `TypeError`** from `tlBuildPatchedEntityJson`: a fresh
  Timeline's `"edit":null` slipped past a `typeof` guard and the next property assignment threw (the browser
  preview never reproduced it because its sample data has a real `edit` object, never `null`). Plus the
  crash half was our deferred-apply bug, not the engine. Fresh placement *and* reclass now save immediately,
  no workaround, verified in-game. See the 2026-07-13 entry in [`webview-ui-history.md`](webview-ui-history.md) and the retraction in
  [`fidelity.md`](fidelity.md).
- **"Use current selection" (2026-07-14, RESOLVED).** The Timeline "Runs on" picker's button now reads the
  live 3D-editor selection via a fresh on-demand native round-trip. See the 2026-07-14 entry in [`webview-ui-history.md`](webview-ui-history.md).
- **"Create New Timeline" stays disabled.** Earlier reverse-engineering concluded a clone-side
  create path is impossible (both a from-scratch spawn and a reclass-morph corrupted the map in that
  work) -- but a live decompile of the original's `retranslateUi` this session found a real
  `"Create New Timeline"` button and string in the *original* binary, which contradicts that assumption.
  Not re-investigated yet; flagging it here so the disabled state doesn't get treated as settled.
- **SnapStack ported to the backend (2026-07-13/14, RESOLVED).** The SnapStack subsystem (stores + all 20
  `sh` handlers) lives in the shared backend (`src/backend/snapstack.c` + `json_patch.c`) — one
  implementation, one store. "Push to stack 0" reaches it via the `+0x2A0` `push_to_stack` slot, and the
  shared handlers commit every `kind=0` decl-edit through the synchronous `+0x290` `apply_sync` path (which
  closes the inline-vs-deferred footgun). Backend-exclusive `chkstk`/`chkgrp`/`clrgrp`/`snapstack_diag`
  commands exist for store inspection/management. Full design + the `json_patch` empty-`edit` fix are in
  [`backend-changes.md`](backend-changes.md) (2026-07-13 entry). **Still open:** `mkcmd` is ported but not
  live-verified.
- Editing an entity's decl does not re-present it live in the editor (a decl commit updates the definition
  but not the already-spawned instance -- same as Save-to-Decl). A live in-editor re-present
  via the engine's per-entity refresh is a possible future experiment.
- Undo covers only unsaved edits (the Revert button + the textarea's native undo); undoing a committed Save
  is not implemented.

## Preview mode

Open `src/ui/webview/mockup.html` directly in a browser to see and click through the UI with fake data --
useful for iterating on layout/behavior without building or deploying. This preview branch only runs when
there is no WebView2 host, so it has no effect inside DOOM. The preview remembers its theme in browser
`localStorage`; production uses the backend-owned `config.json` service instead.
