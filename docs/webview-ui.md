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
| `src/ui/webview/mods_tab.js` | The Mods tab: payload model, the installed-mods list, the open map, and the action messages. |
| `src/ui/webview/prefab_budget.js` | What a prefab adds to the editor's limits: block sums, the five HUD meter ratios, the reached rule and a bounded memo. No DOM. |
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
| Timeline tile | `timeline_tile` at `+0x368` reports the toybox tile's palette row, its prototype record class and the rebind counts. The page does not call it; the `+0x298` normaliser is retired. |
| Mods tab | `mods` at `+0x378`: LIST and RUN build a document on the game thread (call them inside `run_on_main`), FETCH copies it on any thread, ROOT returns the store folder, WAIT follows a migration from any thread. See [Mods tab](#mods-tab). |
| Prefab limits | `budget` at `+0x370`: TABLE copies the budget table the backend built after the last palette build, USAGE the map's limits, usage and reached byte from the last editor frame. |
| Persistent settings | `config_get_json` at `+0x2B0` and `config_set_json` at `+0x2B8`. |
| Prefab geometry | Model lookup and request/completion slots at `+0x308` through `+0x320`. `resolve_prefab_defaults` (`+0x320`) also reports a palette-row model (`SH_PREFAB_DEFAULT_EDITOR`) and, given six floats, that row's `renderInfo.size` (`SH_PREFAB_DEFAULT_SIZE`); see [Prefabs 3D preview](#prefabs-3d-preview). |
| File menu rawmap I/O | `rawmap_status` at `+0x328`, `rawmap_configure` at `+0x330` and `rawmap_load_now` at `+0x338`. The picker is host-side and touches no engine state. Loading stages a file: the swap substitutes it into the next map the engine parses. |
| View menu navigation | `navmesh_view` at `+0x340` reports whether the preview is out of date and, when asked, requests an update on the game's next frame. |
| Engine edits | `run_on_main` at `+0x348`. Selection, delete, Entity State save and camera writes run on the game's thread; the page waits up to one second, then drops the edit and logs it. |

Prefab files and metadata use the path returned by `resolve_prefab_path`; their
rename, delete and folder operations are native filesystem work. Feedback and
crash submission use a short-lived HTTPS worker in the host and the
[feedback pipeline](feedback.md).

## Page and host messages added for the tab trees

| Direction | Message | Meaning |
|---|---|---|
| host to page | `editorMode {entityMode, blueprint}` | The editor state, posted on change: module prefabs place only in Blueprint, object prefabs and new timelines in Object mode. |
| page to host | `movePrefabs {names, fromFolders, toFolder}` / `deletePrefabs {names, folders}` | A multi-selection drop or delete, one tick, one `movePrefabsResult` / `deletePrefabsResult`, one list refresh. |
| page to host | `flyTo {eid}` | Apply kind 4: the EditorFrame detour switches to Object mode and aims the camera at the entity; `flyToResult` confirms the queue only. |
| page to host | `createModulePrefab {name}` | Save the modules selected in Blueprint, with their entities, as a module prefab (`~type` `idSnapMap`); `createModulePrefabResult` carries the sidecar summary. `loadPrefab` routes by the file's `~type` and answers with `prefabKind`. |
| page to host | `budgetTable` | Ask once for the budget table; the host answers `budgetTable {data}` (null until an editor has been entered) and re-sends it from its 330 ms poll whenever the table generation changes, that is after a palette rebuild. |
| page to host | `budgetUsage` | The map's current `{limits, usage, reached}` as `budgetUsage {data}`; asked when the Prefabs tab opens and on each prefab selection. |
| host to page | `prefabDetail {..., folder, sig}` | The selected prefab now also carries its folder and a `size:mtime` content signature, and a module prefab carries its scene, so the Limits section can count its entities. |
| page to host | `setDisplayNames {eids, names}` | Batch display-name writes through the `set_displayname` slot; the Timelines tree uses them for folder moves and renames. |

## Prefabs 3D preview

`prefab_viewport.js` gives each entity a role from its class and inherit, and
each role its own drawing:

| Role | Drawn as |
|---|---|
| Prop, pickup, interactable | Its model, neutral grey (amber for interactables). A pickup spawner draws the pickup its `spawnerEntityPair.entityStatic` names. |
| Demon (`idPlaceableSnapAIEncounter`, `ai/` inherits) | The md6 its palette row draws in the editor, red, in the bind pose. |
| Player start (`ComboStart`, `player/` inherits) | The same rule; pink. |
| Blocking Box, trigger | Their shell models or a box of their saved or default size (triggers translucent with an outline; AI Navigation boxes green). |
| Logic, listener, action, filter, light, speaker | The editor's hexagon, circle and diamond glyphs. |
| Decal | A flat translucent slab of the stamp size; no texture. |

The model comes from the backend, keyed by the entity's inherit: the entityDef's
`renderModelInfo` model when it is an `.lwo`, `.bmodel` or `.md6`, otherwise the
`renderInfo.renderModelName` of the palette row that owns the inherit, read live
at the last palette build with the installed snapEditorEntityDef text as the
fallback. That row's `renderInfo.size` sizes the box a demon or player start
falls back to. The page holds no model table of its own.

The backend reads geometry from the installed resource containers on its preview
worker. An `.md6` is an md6Def whose `mesh` names a cooked `.md6mesh`; the
decoder takes every surface's bind-pose positions, normals and indices, leaves
out the surfaces the md6Def lists in its `Gore` and `Melee_Highlight` mesh kits,
and draws no attachment (the player start's shotgun). A model above 65,536
triangles is vertex-clustered down to that bound, which keeps a blob near 1 MB
(the player start: 92,238 triangles without its kits, clustered to 62,086, 1.09 MB;
the imp: 16,328 triangles, 0.35 MB).
The blob format is unchanged. The page draws at most 400,000 triangles a frame
and keeps 24 MB of meshes, evicting the least recently used mesh no entity uses.

The status line reads `N models unavailable` when an entity that should have a
model is drawn as a box because its model resolved to nothing or could not be
decoded or delivered; Blocking Boxes, triggers, glyphs and decals never count.
Models skipped by the triangle budget are reported separately.

## Prefabs Limits section

The prefab card shows, under the preview, what placing the selected prefab adds
to the editor's five HUD meters and what each meter would read afterwards.
`prefab_budget.js` applies the editor's own accounting: a placed entity costs
exactly its palette row's sedef `limits` block, with network 0 when it is
network-static (an explicit `isNetworkStatic` in the saved entity wins over the
row's default), and an entity whose inherit has no palette row costs 1 entity,
1 render model and 1 network. A module costs its own block (floors + 3 walls +
2 (portals + 2) render models and the card's memory KB), its GAME template
entities and palette misses, its VIEW entities (instantiated into the room by
the engine, so a module prefab's scene copy of one is not counted again) and
two 50 KB block bases; templates and scene entities count only when their layer
overlaps the instance's saved `layerMask` OR the snapLayers autoView mask.
The meters are NETWORK = network / limit, OBJECTS = max(entities, memory KB sum
over the KB limit sum), MEMORY = max(render models, lights, world areas,
decals), DEMONS = AI / 64 and DECALS = decals / limit; a result is red when
NETWORK, OBJECTS or MEMORY would reach 1.0 or DEMONS would pass it. The limits
and usage are always read live, never hard-coded.

## Mods tab

`mods_tab.js` renders the tab from the document `backend/modui.c` builds behind the `mods` slot:
`mods` holds one row per installed mod (the build that serves, else the newest, with
`otherBuilds` counting the rest; two folders declaring one id are a row each with
`duplicateId`; bundles under `mods\legacy\` are left out), each `installed` or `attention`
with the first cause, `migrate` counts the `overrides\` entries the migration record does not
cover (the Migrate button shows only while that is above zero), and `map` holds the open map's
manifest rows and placed closure. Showing the tab and the refresh button send
`modsAction {action: "rescan"}`; the other actions are `install` (the native folder picker),
`remove` (a row's key, after the page's confirmation; the backend refuses a mod the open map
uses, deletes laid-out builds and moves a folder built in place to `removed-mods\`), `migrate`
(the host follows the worker with WAIT before the list) and `openFolder`. Every action is
answered with `modsActionResult` and then a fresh `mods` document, one job at a time, and
nothing polls. The list is a plain list scrolled natively by the panel body (a store holds tens
of mods); the earlier windowed list rebuilt itself on every scroll event and read `scrollTop`
after removing its sizer, which clamped the position to zero. Every bundle's state, conflicts,
shadows and verification moved to `sh_mods doctor [verify]`. `tests/modui_test.c` checks the
document, the actions and the doctor against a temporary store, and `tests/mods_tab_test.js`
renders `tests/fixtures/mods_tab_payload.json`, whose keys the native test checks.

## Driving the page from a test

A file `%LOCALAPPDATA%\snapmap-plus\devtools-port` containing one port number (1024 to
65535) makes the host open Chromium's remote debugging on `127.0.0.1` at that port when
the window is created; the log says `devtools: remote debugging on ...`. Nothing is opened
when the file is absent or malformed. The research daemon's `webview` step writes and
removes this file and drives the page over it. Remove the file when you are done.

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
The prefab list never reads a prefab scene: previews, and the Limits section,
are only ever loaded for the one selected prefab. That scene is parsed once
and the parsed document is shared by the 3D preview, the AI Navigation count
and the Limits sum, which is memoized (32 entries) by folder, name, file
signature and table generation, so reselecting a prefab costs one cache hit
and the meters are recomputed from the latest usage. The budget table is held
once by the page and looked up by key.
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

Rawmap saves capture the live map and destination as one queued request. The
frame checks editor state and the load generation before serializing; a new
map can reuse the same pointer. Save Rawmap resolves its destination in the
backend rather than sending a path from an earlier page status. Neither a
refused save nor a cancelled request falls back to another map on disk.
