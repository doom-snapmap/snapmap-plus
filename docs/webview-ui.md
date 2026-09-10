# WebView2 frontend

`snapmap-plus-ui.dll` hosts the Snapmap+ companion window. Its HTML, CSS and
JavaScript are embedded at build time and rendered by the system WebView2
runtime. The native host owns the window, message bridge and UI thread; engine
operations use the interface in `src/common/snapmap_plus_iface.h`.

## Source layout

| File | Responsibility |
|---|---|
| `src/ui/webview/snapmap_plus_ui_webview.cpp` | Window, WebView2 startup, message bridge, file operations and polling. |
| `src/ui/webview/mockup.html` | Layout, controls, entity and timeline editors, asset browser and browser preview. |
| `src/ui/webview/studio.css` | Shared layout, controls and light/dark themes. |
| `src/ui/webview/decl_language.js` | Declaration tokens, parsing, schema checks, highlighting and completion; each instance owns its schema cache. |
| `src/ui/webview/prefab_viewport.js` | Prefab rendering, mesh transport, framing and orbit controls. |
| `src/ui/webview/prefab_transform.js` | Sparse saved transforms and inherited scale composition. |
| `src/ui/webview/schema_slice.js` | Declaration schema slicing for the property editor. |
| `src/ui/webview/config_message.{h,cpp}` | Bounded extraction of configuration messages. |
| `src/ui/webview/webview_json.{h,cpp}` | Text encoding, JSON escaping and field access for WebView messages. |
| `src/ui/webview/crash_pending.h` | Pending-report ordering and complete filename-inventory change detection. |
| `src/ui/webview/theme_bootstrap.{h,cpp}` | Saved-theme validation and initial document styling. |
| `src/ui/webview/LUCIDE_LICENSE.md` | License for embedded Lucide icons. |
| `src/ui/build.ps1` | SDK retrieval, embedded assets and frontend compilation. |
| `src/ui/embed-page.ps1` | Assemble the browser's local assets into the embedded page, refusing missing or duplicate assets. |

## Build and preview

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
installer\snapmap-plus.exe install --local dist
```

Close DOOM before installing. Build both DLLs together to keep their interface
layouts matched. The frontend statically links the WebView2 loader; the runtime
is installed separately. Logs go to
`<DOOM>\snapmap-plus\logs\snapmap-plus-ui.log`.

Open `src/ui/webview/mockup.html` in a browser for sample-data preview. That mode
uses browser storage for preferences and does not call the engine. Production
uses the backend configuration service. See [contributing](contributing.md) for
the build and test requirements.

Normal builds read committed assets and write only build outputs. To refresh the
organization avatar deliberately, run `tools/update-avatar.ps1` and review the
change to `mockup.html`; `-ImagePath <png-or-jpeg>` uses a local image without a
download. Declaration-language tests run directly against the module, and
`tests/ui_assets_test.js` checks embedding and avatar updates without networking.

## Engine interface

The header defines the complete ABI. These are the main entry points:

| Feature | Interface |
|---|---|
| Entity list and state | Count, validity, classname, inherit, display name and serialization slots. |
| Selection | `get_selection` at `+0x150`, `add_to_selection` at `+0x138`, and `clear_selection` at `+0x148`. |
| Camera position | `get_editor_vec3` at `+0x08` and `set_editor_vec3` at `+0x00`. Position lock reapplies saved coordinates. |
| Create prefab | `serialize_selection` at `+0xB0`; the engine requires hovering a selected entity. |
| Load / Place | `apply_edit` at `+0xD0`, kind 2: stage the prefab and request the native paste action. Engine state can force a stage-only fallback, reported by toast. |
| Save Timeline | `apply_sync` at `+0x290`, kind 0, with the target entity ID. |
| Timeline inheritance | The normalization slot at `+0x298`. |
| Persistent settings | `config_get_json` at `+0x2B0` and `config_set_json` at `+0x2B8`. |
| Prefab geometry | Model lookup and request/completion slots at `+0x308` through `+0x320`. |
| File menu rawmap I/O | `rawmap_status` at `+0x328`, `rawmap_configure` at `+0x330` and `rawmap_load_now` at `+0x338`. The picker is host-side and touches no engine state. Loading stages a file: the swap substitutes it into the next map the engine parses. |

Prefab files and metadata use the path returned by `resolve_prefab_path`; their
rename, delete and folder operations are native filesystem work. Feedback and
crash submission use a short-lived HTTPS worker in the host and the
[feedback pipeline](feedback.md).

## Threading and polling

WebView callbacks capture pending edits. The host applies them from its UI
think-loop under the loop mutex, then pumps window messages and sleeps about
33 ms. The loop runs on the frontend thread. Engine commands and declaration
commits have a separate main-thread path in the backend.

`apply_sync` returns an applied count after the main-thread operation completes.
Off-main callers submit a copied batch through `clone_bss_apply`; missing thread
identity or transport refuses the operation. A timed-out operation already running
returns an in-progress status, so the page must not report completion or retry it
automatically. See [architecture](architecture.md#threads-and-edit-results).

Camera polling runs at the loop cadence and posts only changed coordinates.
Entity state and selection are sampled roughly every 330 ms. Timeline scanning
is change-gated. Prefab rendering redraws when scene data, input, theme or
viewport dimensions change.

## Configuration startup

Before navigation, the host reads `theme` and seeds the document's root class.
The window stays hidden until navigation succeeds. The page requests both
Entities settings before applying either, preventing a restored selection mode
from pushing an empty selection into the editor.

Messages use `configGet {key}` and `configSet {key, valueJson}`. Values remain
complete UTF-8 JSON fragments across the bridge. Replies distinguish rejection,
persistence and session-only changes; startup recovery produces one status
message. The page never opens `config.json` itself. Format, ownership and
recovery are described in [persistent configuration](architecture.md#configuration).

## Limitations

- Committing a declaration does not recreate an already spawned editor entity.
- Revert and text undo cover unsaved edits; there is no general undo for a
  committed engine edit.
- Browser preview verifies page behavior, not engine calls, native window
  behavior or gameplay. Changes to those paths need the corresponding tests.
- The File menu answers in browser preview with the default path pair, so the
  readout and layout can be checked outside the game. Load and Save As are inert
  there and report preview only.
