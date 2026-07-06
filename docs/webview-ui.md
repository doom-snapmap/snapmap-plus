# Experimental WebView2 frontend (HTML "SnapHak Studio")

An alternative, **Qt-free** build of `snaphakui.dll` that renders the "SnapHak Studio" UI as HTML/CSS/JS
in a Microsoft Edge **WebView2** control instead of a Qt widget tree. It is a drop-in replacement for the
Qt frontend: same `snaphak_ui_init` entry (export ordinal 10), the same backend interface contract
(`src/common/snaphak_iface.h`), and the same manual 30 Hz think-loop draining the work-queue (`+0x1a0`).
The backend (`XINPUT1_3.dll`) is unchanged.

**Status: experimental / proof-of-concept.** This is a *parallel* frontend, not a replacement of the Qt
one. The default `build.ps1` still builds the faithful Qt UI; this frontend is built with a separate,
opt-in script (`build-webview.ps1`). It is not wired into CI.

## Why

- Drop the ~18 MB Qt runtime shipped in the overlay (`Qt5Core` / `Qt5Gui` / `Qt5Widgets` + the `qwindows`
  platform plugin).
- Iterate on the UI in HTML/CSS/JS instead of a hand-written Qt `setupUi`.
- Combine the Entities + Entity-State tabs into a single view.

Trade-off: it depends on the Microsoft Edge **WebView2 runtime** (preinstalled on Windows 11 and most
Windows 10) instead of the bundled Qt DLLs. The compiled DLL is ~120 KB (vs ~1.5 MB for the Qt one) and
statically links the WebView2 loader, so no extra loader DLL ships.

## Files

| File | What |
|---|---|
| `src/ui/webview/snaphak_ui_webview.cpp` | The WebView2 host: the `snaphak_ui_init` entry, a Win32 window, the WebView2 bring-up, the 30 Hz think-loop, and the JS <-> native bridge. |
| `src/ui/webview/mockup.html` | The UI (HTML/CSS/JS), embedded into the DLL at build time. Self-populates with sample data when opened in a plain browser (a "preview mode", inert in DOOM). |
| `src/ui/build-webview.ps1` | Builds a Qt-free `build/snaphakui.dll`: fetches the WebView2 SDK from NuGet into `build/` (gitignored), statically links the loader, embeds the HTML. Reuses the unchanged `sl_exports.cpp` + `snaphakui.def`. |

## Build + deploy

```powershell
# from src/ui/
powershell -NoProfile -ExecutionPolicy Bypass -File build-webview.ps1   # -> build/snaphakui.dll (Qt-free)
# from the repo root
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1          # -> dist/
installer\snaphak.exe install --local dist                               # deploy (DOOM must be closed)
```

Do **not** run `build.ps1` after `build-webview.ps1` -- it rebuilds the Qt `snaphakui.dll` and overwrites
the WebView2 one. Runtime log: `<DOOM>\snaphak_logs\webview_poc.log`. The overlay still copies the three
Qt DLLs (dead weight for this frontend); stripping them from `package.ps1` is a pending cleanup.

## How it maps to the backend interface

The frontend holds no engine addresses; it calls the backend only through the vtable slots pinned in
`src/common/snaphak_iface.h`:

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
| Camera Origin (X/Y/Z + Lock Position) | `get_editor_vec3` +0x08, `set_editor_vec3` +0x00 |
| Installed version readout | reads `%LOCALAPPDATA%\open-snaphak\install.json` (written by the installer) |
| Deselect (explicit button, "Select in 3D editor" mode) | `clear_selection` +0x148 |
| Live "Create from selection (N)" button count | `get_selection` +0x150, polled every ~330 ms independent of the sync checkboxes |
| Prefabs list, detail pane, delete/rename, folders (create/rename/delete/move) | `resolve_prefab_path` +0xc0 only -- pure Win32 file/directory ops (`FindFirstFileA`, `DeleteFileA`, `MoveFileA`, `CreateDirectoryA`, `RemoveDirectoryA`) on the resolved path. No other engine slot involved, unaffected by the +0xb0 issues below. |
| Create from selection | `serialize_selection` +0xb0 -- works for normal selections; see Known limitations for a remaining intermittent edge case |
| Load / Place -- **not yet implemented, see Known limitations** | would need `apply_edit` kind=2 (stage + `PasteInstantiate` + enter grab mode) |

Heavy engine writes (Save, Delete, Select-in-editor) are snapshotted in the JS message callback and
applied on the next think-loop frame under the loop mutex -- mirroring the Qt frontend's flag-word
dispatch, which keeps them off the re-entrant callback and on the main-thread execution point.

## Implemented (changelog)

- WebView2 host DLL that opens in DOOM, gated to the SnapMap editor (hidden on the menu).
- Live entity list: walks `0..entity_count`, keeps `is_valid_id`, skips `NULL_` placeholder slots, shows
  real id-strings + displaynames, sorted; auto-refreshes on a content signature (no needless re-renders).
- Dev-layer hidden entities filtered out by default, with a "Show hidden entities" toggle (greyed/italic).
- Live entity count (drops when entities are deleted).
- State editor: reads an entity's decl source + class / inherit / displayname; "Save to Decl" commits.
- Class / Inherit as an editable combobox with autocomplete (full list on the arrow, type to filter,
  free-text still allowed); blank class/inherit is blocked with an error toast.
- Multi-select (click / Ctrl-click / Shift-click, no text-highlight); the state editor steps aside for a
  placeholder when 2+ are selected (its actions live in the context menu).
- Right-click context menu: Copy ID (clipboard), Delete, Push to stack 0 (a stub -- see limitations).
- Sliding toasts for Copy / Save / Delete / Push, color-coded (success / warning / error).
- "Follow editor selection" (editor selection -> list, any N) and "Select in 3D editor" (list selection ->
  editor, hidden entities skipped). The two are mutually exclusive to avoid a selection feedback loop.
- Camera Origin bar (always visible): X/Y/Z fields track the live editor camera; "Lock Position" pins it
  (writes the stored vec3 every frame); a committed field edit writes back. Mirrors the Qt camera sync.
- Modern light/dark theme with a menu bar toggle (remembered via localStorage); a menu bar with a Settings
  placeholder for future feature toggles. Native controls (scrollbars, checkboxes) follow the theme.
- Installed-version + connection status in the status bar.
- Browser preview mode: `mockup.html` self-populates with sample data and is fully interactive when
  opened without a WebView2 host (for fast UI iteration); inert in DOOM.
- Default window size bumped to 1440x900 (from 1040x720) so the Entities and Prefabs tabs fit without a
  manual resize on first launch.
- Explicit **Deselect** button next to "Select in 3D editor" (only visible while that mode is on): calls
  `clear_selection` directly. A native click on empty space in the 3D view doesn't clear a selection that
  was set via `add_to_selection` (confirmed: a purely native selection deselects fine on its own -- only
  our externally-driven selection gets stuck), and the root cause is unRE'd in this codebase, so this is a
  reliable escape hatch rather than a fix for the underlying click behavior.
- **Prefabs tab, wired to the real filesystem** (`%USERPROFILE%\snaphak\prefabs\`) -- no fake/mockup data:
  - Live list of real `.json` prefab files, refreshed from disk on every Prefabs-tab click; an empty-state
    message when there are none yet.
  - Detail pane on selecting a prefab: real entity count and a per-`className` tally, read directly from
    the file (a targeted "find key -> read quoted value" scan, not a full JSON parser -- same approach as
    the JS<->native command parsing). Description/Tags fields are visibly disabled ("a later step"): the
    prefab JSON has no metadata field, so there's nothing to show until a sidecar file is designed.
  - Delete and Rename are real file operations (`DeleteFileA` / `MoveFileA`), each with a collision/confirm
    guard client-side and a safe no-overwrite guarantee native-side.
  - **Folders**: one real level of subdirectories under `prefabs\` (no nested-within-nested) -- the
    directory *is* the source of truth, no separate manifest file to desync. New Folder button, drag-and-
    drop a prefab between folders/root, folder Rename, and folder Delete (moves any remaining contents back
    to the root list, then removes the now-empty directory). Folders render above root-level items.
  - Filter/search box narrows the list client-side over the last real fetch (same pattern as the Entities
    tab's filter); folders with zero matches are hidden while filtering, with a "No matches." empty state.
  - "Create from selection" works end-to-end (see Known limitations for a remaining intermittent edge
    case). "Load / Place" is still a "coming soon" toast -- not yet implemented.
  - Live "Create from selection (N)" button count and the create modal's "From N selected entities" text
    both track the real editor selection continuously (two separate display bugs fixed: the count used to
    silently cap at 64 regardless of the real selection size, and the modal text never updated at all --
    it was stuck on its original static placeholder).

## Known limitations / TODO

- **Create from selection: FIXED (2026-07-06).** `serialize_selection` (+0xb0) used to hard-crash DOOM
  outright. Root cause: `apply_engine.c`'s `PREFAB_TEMP_SIZE` (was `0x220`, "generous" per the old comment)
  was far too small for the real `idSnapEntityPrefab` object -- a stack buffer overflow on every call.
  Because the overwritten bytes land on valid, mapped stack memory (just not memory meant for this
  object), it was never a clean access violation, so neither the fault-shield VEH nor our own SEH guard
  ever caught it -- that's why it crashed DOOM outright instead of failing gracefully. Fixed by bumping
  `PREFAB_TEMP_SIZE` to `0x2000`. The Qt Prefabs tab was always a "Coming soon" stub (see `sh_tabs.cpp`),
  so this backend path had never been exercised by either frontend before this webview UI's first real
  call into it.
- **Create from selection: a SEPARATE, intermittent crash remains -- likely tied to 3D-view HOVER state,
  not selection size/complexity.** Initially looked size-related (first repro was a large ~60-entity
  selection), but a second test round disproved that: the *identical* tiny 2-entity selection both
  succeeded and failed across repeated attempts with no reselection in between. The user's own empirical
  finding: hovering the mouse over one of the selected entities in the 3D view right before clicking
  Create makes it reliably succeed; not hovering anything reliably fails. Two distinct crash locations seen
  so far inside the engine's `populate()` function, both a `c0000005 ACCESS_VIOLATION` reading a near-null
  address, both caught cleanly by the fault-shield (recovers by exiting the editor to the menu, not
  crashing DOOM outright). Working theory: `populate()` reads the selection object's separate `hovered_id`
  field (see `slot_hovered_id`, `selObj+0x2c`) unconditionally and dereferences through it without a
  "nothing hovered" sentinel check -- consistent with the near-null crash signature and with hover state
  (not selection content) determining success/failure. Not yet confirmed, and the two crash locations
  aren't reconciled against each other -- possible the first (size-theory) repro was actually the same
  hover-state issue and the tester simply wasn't hovering an entity at the time. No UI hint/workaround
  added deliberately -- the hover behavior isn't reliable/consistent enough yet to tell users to rely on
  it. Deferred (time-constrained); needs further investigation to confirm what `populate()` dereferences,
  and more create-from-selection testing generally.
- **Load / Place is a separate, simpler gap: not yet implemented (not a crash).** The plan is `apply_edit`
  kind=2 (stage into the paste slot, `PasteInstantiate`, enter grab mode) mirroring the working
  Timeline-spawn precedent, plus an auto-`clear_selection` first so nothing else is selected when the
  placed prefab lands. Delete/Rename/Folders are unaffected by any of the above -- they're pure Win32 file
  ops through `resolve_prefab_path` (+0xc0) only, no `serialize_selection` or `populate()` involved.
- **Timelines / Timeline Editor** tab is not ported (the Qt frontend has it, and per `sh_timeline.cpp` even
  the OG Qt behavior has a faithfully-reproduced "Create New Timeline" brokenness). Deferred.
- **Push to stack 0** is a stub: the SnapStack subsystem (`snapstack.cpp`) is Qt-bound and its consuming
  ops are not ported to this frontend.
- Editing an entity's decl does not re-present it live in the editor (a decl commit updates the definition
  but not the already-spawned instance -- same as Save-to-Decl in the Qt UI). A live in-editor re-present
  via the engine's per-entity refresh is a possible future experiment.
- Undo covers only unsaved edits (the Revert button + the textarea's native undo); undoing a committed Save
  is not implemented.
- The overlay still ships the three Qt DLLs, and `build-webview.ps1` fetches the newest WebView2 SDK (a
  prerelease); both are pending cleanups (strip the Qt DLLs from `package.ps1`; pin the SDK).
- Not wired into CI (CI builds the Qt path via `build.ps1`).

## Preview mode

Open `src/ui/webview/mockup.html` directly in a browser to see and click through the UI with fake data --
useful for iterating on layout/behavior without building or deploying. This preview branch only runs when
there is no WebView2 host, so it has no effect inside DOOM.
