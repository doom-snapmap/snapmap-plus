# WebView2 frontend

`snapmap-plus-ui.dll` hosts the Snapmap+ companion window. Its HTML, CSS and
JavaScript are embedded at build time and rendered by the system WebView2
runtime. The native host owns the window, message bridge and UI thread; engine
operations use the interface in `src/common/snapmap_plus_iface.h`.

## Source layout

| File | Responsibility |
|---|---|
| `src/ui/webview/snapmap_plus_ui_webview.cpp` | Window, WebView2 startup, message bridge, file operations and polling. |
| `src/ui/webview/mockup.html` | Layout, controls, entity and timeline editors, asset browser and every player-facing message. |
| `src/ui/webview/page_keys.js` | Keyboard ownership, the dialog stack (Escape, Tab kept inside, focus restored), menu and tab-strip arrow keys. No page state; `tests/page_keys_test.js`. |
| `src/ui/webview/preview_standin.js` | The browser preview's stand-in host: a fake `window.chrome.webview` answering the page with sample data. Loaded only by a page opened from disk; `embed-page.ps1` leaves it out of the DLL. |
| `src/ui/webview/studio.css` | Shared layout, controls and light/dark themes. |
| `src/ui/webview/decl_language.js` | Declaration tokens, parsing, schema checks, highlighting and completion; each instance owns its schema cache. |
| `src/ui/webview/prefab_viewport.js` | Prefab rendering, mesh transport, framing and orbit controls. |
| `src/ui/webview/prefab_transform.js` | Sparse saved transforms and inherited scale composition. |
| `src/ui/webview/mods_tab.js` | The Mods tab: payload model, the list (Your mods, and one group per downloaded map), the selected mod's details, the map modal and its confirmations, the action messages, and the browser preview's stand-in host. |
| `src/ui/webview/prefab_budget.js` | What a prefab adds to the editor's limits: block sums, the five HUD meter ratios, the reached rule, the meter colour levels and a bounded memo. No DOM. |
| `src/ui/webview/prefab_host.h` | The host's Prefabs tab decisions: Create's precheck, a prefab's kind by its file's `~type`, and the live usage gate. Pure; `tests/prefab_host_test.cpp`. |
| `src/ui/webview/player_text.h` | The description text a player sees from the generated entity and event tables: study notes (addresses, offsets, handler and decompile wording, runtime class and field lists) and name-only entries are left out. Pure; `tests/player_text_test.cpp`. |
| `src/ui/webview/report_scrub.h` | Crash-report attachments: the account, profile-folder and machine names scrubbed as UTF-8, ANSI, UTF-16LE and short-path text; whole-line log tails; the logs trimmed oldest-first to fit the relay's 64 KB cap. `tests/report_scrub_test.c`. |
| `src/ui/webview/game_identity.h` | The game identity (`--game=`) and the backend's release (`--version=`) from the startup arguments. `tests/game_identity_arg_test.cpp`. |
| `src/ui/webview/folder_watch.h` | The debounce behind the prefabs folder watcher. `tests/folder_watch_test.c`. |
| `src/ui/webview/growing_text_buffer.h`, `serialization_buffer.h` | Retained buffers that grow for large declarations and serialized entities, up to 32 MiB. |
| `src/ui/webview/webview_profile.h` | The WebView2 profile folder under `cache\`. |
| `src/ui/webview/rawmap_picker.h` | The file dialogs' types and the folder each opens in (its key in `config.json`, its default). Pure; `tests/rawmap_picker_test.cpp`. |
| `src/ui/webview/schema_slice.js` | Declaration schema slicing for the property editor. |
| `src/ui/webview/config_message.{h,cpp}` | Bounded extraction of configuration messages. |
| `src/ui/webview/webview_json.{h,cpp}` | Text encoding, JSON escaping and field access for WebView messages. |
| `src/ui/webview/crash_pending.h` | Pending-report ordering and complete filename-inventory change detection. |
| `src/ui/webview/theme_bootstrap.{h,cpp}` | Saved-theme validation and initial document styling. |
| `src/ui/webview/LUCIDE_LICENSE.md` | License for embedded Lucide icons. |
| `src/ui/build.ps1` | SDK retrieval (the WebView2 package is pinned by version and SHA-256), embedded assets and frontend compilation. |
| `src/ui/embed-page.ps1` | Assemble the page's local assets into the embedded page, refusing missing or duplicate assets, and leave the preview stand-in out. |
| `src/ui/snapmap-plus-ui.def` | The DLL's one export, `sh_ui_init` at ordinal 10. |

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

Open `src/ui/webview/mockup.html` in a browser for sample-data preview. Its
`preview_standin.js` installs a stand-in `window.chrome.webview` that answers the
page with sample data and keeps the theme in browser storage; the page itself is
unchanged and never calls the engine there. The DLL's page has no stand-in and no
browser storage: settings go through the backend configuration service. See [contributing](contributing.md) for
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
| Create prefab | `serialize_selection` at `+0xB0`; the engine's copy check (PrefabPopulate) requires the hovered entity to be one of the selected ones. In one `run_on_main` call the host reads `get_selection` and `hovered_id` (`prefab_host.h`), serializes and reads the grab-frame verdict, then writes the file on its own thread. It answers `createPrefabResult` 0 nothing selected, 2 nothing hovered, 4 the hovered entity is not selected, 5 the copy check refused for its other causes (a wired or referenced entity left out, or one the editor does not copy; the backend logs its status), 6 the file was not written whole (the partial file is removed), 7 the game did not run the request in time, -1 the name is one Windows cannot hold. |
| Load / Place | `apply_edit` at `+0xD0`, kind 2: stage the prefab and request the native paste action. The game thread decides on its next frame whether the paste was queued or the prefab only staged, and why (`prefab_watch`'s `place_seq`, `place_result`, `place_reason`); the host waits up to 2.5 s for that and posts `loadPrefabResult` with `placed`. |
| Prefabs tab watch | `prefab_watch` at `+0x380` (ext 35): the modules selected in Blueprint, whether a Load / Place is held in the 3D view (`SH_PREFAB_HELD_OBJECTS` while EntityMode `+0x1AC` reads 4 after a queued paste, `SH_PREFAB_HELD_MODULES` from a module placement's install to Space or Esc) and the last paste decision. Guarded reads from the host's thread, nothing waits for the game. |
| Save Timeline | `apply_sync` at `+0x290`, kind 0, with the target entity ID. |
| Timeline tile | `timeline_tile` at `+0x368` reports the toybox tile's palette row, its prototype record class and the rebind counts. The page does not call it; the `+0x298` normaliser is retired. |
| Mods tab | `mods` at `+0x378`: LIST and RUN build a document on the game thread (call them inside `run_on_main`), except the file work of Install, New mod and Refresh, which the window runs on its own worker with no time limit before APPLY brings the change into the game; FETCH copies it on any thread, ROOT returns the store folder, STORE says whether a store write holds the store lock. See [Mods tab](#mods-tab). |
| Prefab limits | `budget` at `+0x370`: TABLE copies the budget table the backend built after the last palette build, USAGE the map's limits, usage and reached byte from the last editor frame. |
| Persistent settings | `config_get_json` at `+0x2B0` and `config_set_json` at `+0x2B8`. |
| Prefab geometry | Model lookup and request/completion slots at `+0x308` through `+0x320`. `resolve_prefab_defaults` (`+0x320`) also reports a palette-row model (`SH_PREFAB_DEFAULT_EDITOR`) and, given six floats, that row's `renderInfo.size` (`SH_PREFAB_DEFAULT_SIZE`); see [Prefabs 3D preview](#prefabs-3d-preview). |
| File menu rawmap I/O | `rawmap_status` at `+0x328`, `rawmap_configure` at `+0x330` and `rawmap_load_now` at `+0x338`. The picker is host-side and touches no engine state; its file types come from `rawmap_picker.h` (Load lists `.json` and `.zip`; Save As puts `.zip` first when the status's `mapMods` is above zero, read on the UI thread before the picker thread starts). Load Rawmap opens the picked file in the editor at once, after the page's confirm; nothing is kept for a later map, and Open <name> Again reopens the last opened file. A file `rawmap_configure` refuses (a prefab's export, an earlier release's `.zip`) is logged by the backend (`B1: rawmap: load of <path> refused: <reason>`), printed to the console on the next game tick ("The rawmap was not loaded: <reason>.") and logged by the host (`rawmap load: refused`), besides the page's error toast with the backend's reason (the host marks a refusal with `error` on `rawmapStatus`). The status's `lastZip` fields name the export the last save wrote, and the path readout says to share it. |
| File dialog folders | Every dialog opens in the last folder the player used for its kind, kept in `config.json`: `dialogs.rawmap_load`, `dialogs.rawmap_save`, `dialogs.prefab_export`, `dialogs.prefab_import` (UI read and write, `""` until first used). Without a kept folder, or when it no longer exists, the prefab dialogs open in Downloads (`SHGetKnownFolderPath(FOLDERID_Downloads)`) and the rawmap dialogs in the rawmaps folder, `%LOCALAPPDATA%\snapmap-plus`. The host reads the folder on the UI thread before the dialog's thread starts and keeps the picked file's folder when the dialog returns. |
| View menu navigation | `navmesh_view` at `+0x340` reports whether the preview is out of date and, when asked, requests an update on the game's next frame. |
| Engine edits | `run_on_main` at `+0x348`. Selection, delete, Entity State save, camera writes, Create Prefab's serialize and the entity serialize behind opening a timeline and resolving an event's target run on the game's thread; the window waits up to one second for the game to start the call, then drops it and logs it. |
| Install check | At load the window compares the backend's interface-table size with its own and, when the backend passes `--version=`, its release with its own compiled-in `SNAPMAP_PLUS_VERSION` (`dev` for a local build). A mismatch shows one notice, "Reinstall Snapmap+", and the window calls nothing else. |

Prefab files use the path returned by `resolve_prefab_path` (UTF-8); every file call
on it is the wide Windows one. Names are refused when Windows cannot hold them
(`prefab_host.h`: reserved characters, a trailing dot or space, device names such as
CON). Rename, delete and folder operations are native filesystem work (an older
prefab's read-only sidecar moves with it), and a failure carries its `reason`
(`exists`, `inUse`, `missing`, `notEmpty`) so the page can say what to do. Feedback and
crash submission use a short-lived HTTPS worker in the host and the
[feedback pipeline](feedback.md).

## Page and host messages added for the tab trees

| Direction | Message | Meaning |
|---|---|---|
| host to page | `editorMode {entityMode, blueprint, modules}` | The editor state, posted on change: module prefabs place only in Blueprint, object prefabs and new timelines in Object mode. `modules` is the number of modules selected in Blueprint (0 outside it, -1 when the backend cannot say); with `selCount {count}`, the selected entities, it gates Create (below). |
| host to page | `createPrefabResult {result, name}` | 1 saved; 0 nothing selected; 2 nothing hovered; 3 no usable grab frame; 4 the hovered entity is not selected ("The cursor is over an entity that is not selected -- hover one of the selected entities in the 3D view, then try again"); 5 the copy check refused the selection; 6 not written whole; 7 the game did not answer; -1 the name was refused. A corrected grab frame is logged, not shown. |
| page to host | `movePrefabs {names, fromFolders, toFolder}` / `deletePrefabs {names, folders}` | A multi-selection drop or delete, one tick, one `movePrefabsResult` / `deletePrefabsResult`, one list refresh. |
| page to host | `flyTo {eid}` | Apply kind 4: the EditorFrame detour switches to Object mode and aims the camera at the entity; `flyToResult` confirms the queue only. |
| page to host | `createModulePrefab {name}` | Save the modules selected in Blueprint, with their entities, as a module prefab (`~type` `idSnapMap`); `createModulePrefabResult` carries the view of the summary the backend wrote into the file's block. `loadPrefab` routes by the file's `~type` and answers with `prefabKind`. |
| page to host | `budgetTable` | Ask once for the budget table; the host answers `budgetTable {data}` (null until an editor has been entered) and re-sends it from its 330 ms poll whenever the table generation changes, that is after a palette rebuild. |
| page to host | `budgetUsage` | The map's current `{limits, usage, reached}` as `budgetUsage {data, held}`; asked when the Prefabs tab opens and on each prefab selection. Once asked, the host posts it again from its 330 ms poll whenever the usage text or the held prefab changed (`prefab_host.h`'s gate), never an unchanged value. `held` is `{name, folder, kind}` of the prefab held in the 3D view after Load / Place, or null. |
| host to page | `loadPrefabResult {result, name, placed, reason}` | For an object prefab, posted once the game thread has decided: `placed` is `queued` ("<name>" is in hand: position it and press Space, or Esc to cancel), `staged` ("<name>" is staged but not in hand because <reason>: press Ctrl+V in the 3D view to place it; for a hovered entity, move the cursor off it first) or `unknown` (no decision within 2.5 s). Module prefabs and the mod outcomes below keep their own fields. |
| host to page | `prefabDetail {..., folder, sig}` | The selected prefab now also carries its folder and a `size:mtime` content signature, and a module prefab carries its scene, so the Limits section can count its entities. |
| page to host | `setDisplayNames {eids, names}` | Batch display-name writes through the `set_displayname` slot; the Timelines tree uses them for folder moves and renames. |
| host to page | `prefabs {..., mods}` / `prefabDetail {..., modsStatus}` | The mods each prefab's block (else its older sidecar) records, keyed like `meta`, as escaped `MODS_STATUS` JSON (`{count, missing, mods: [{id, version, bundle, size, installed}]}`); see [Prefabs and their mods](#prefabs-and-their-mods). |
| host to page | `loadPrefabResult {result: 3 \| -7, message}` | 3: the placement is held while the mod store is asked for its missing mods; -7: refused because of its mods (the message says why, or is empty when the game's own notice did). |
| page to host | `exportPrefab {name, folder}` / `importPrefab` | Open the Windows Save dialog for a prefab's ZIP, or the Open dialog for a ZIP or an extracted export's `.json`; the host answers `prefabFileResult {op, ok, message, name?, cancelled?}` and refreshes the list after an import. |

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

The preview carries no orbit hints; dragging orbits, the wheel zooms and a
double-click frames the prefab. Its top-left row holds the "Blueprint prefab"
pill (module prefabs only) and the status in one flex row, so an object
prefab's status starts at the corner. A key button at the bottom left opens the
legend of entity colours upwards (`pfLegendSetOpen` and `pfLegendAction` in the
page, marked for test extraction). It is collapsed by default and stays open
across object-prefab selections until the button, Escape with the focus on it,
or a press elsewhere in the preview closes it. A module prefab's footprint
hides the control. The open list stays inside the preview (the control is at
most the preview's height less 12px, the list scrolls past that), and at the
smallest preview height, 130px when the window is at most 520px tall, the
status row is hidden while the list is open (`pc-legend-open` on the preview),
because two columns of four reach it there.

A prefab's kind is its file's top-level `~type`: `idSnapMap` is a module
prefab, with or without a `snapmapPlus` block, and `idSnapEntityPrefab` an
object prefab. The list (`kinds`), Prefab Details (`prefabKind`, always sent)
and Load / Place decide it the same way; the list reads the type from the last
256 KB of the file with the block left out, and only a file whose tail names no
type falls back to the view's `kind`. A module prefab without a block has no
summary, so its footprint corner counts its modules and objects from the scene.

## Creating a prefab

Create (the list head's plus) and the Create dialog's confirm follow the
editor's live selection (`pfCreateGate`, marked for test extraction): in Object
mode the selected entities (`selCount`), in Blueprint the selected modules
(`editorMode.modules`). With none selected both are disabled with the tooltip
"Select entities first" or "Select modules first", and they enable as soon as a
selection exists; the open dialog follows changes too. A backend that cannot
count modules (-1) leaves the gate to the engine, and the host's precheck and
the engine's own refusals stay the last line of defence.
`tests/prefab_details_test.js` lifts the gate, the Load / Place toast
(`pfPlaceToast`), the kind decision and the held Limits rule from the page and
runs them, with the layout checks for the legend and the Limits table.

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

The section is one table: the "Limits" head is the first column head, beside
"This prefab" and "Map after", then a row per meter with a thin bar (the map's
use now faint, this prefab's share solid). `prefabBudget.meterLevel` colours
"Map after" and the bar: below 75% (`WARN_AT`) the info colour, from 75% up to
the limit the warning colour, at 100% (`OVER_AT`) and over the danger colour.
Every meter the editor would report as reached is therefore red; DEMONS at
exactly 64 of 64 is red as well, although the editor's flag waits for the 65th.
The text uses `--infoText`, `--warnText` and `--dangerText`, the accent and
danger colours tuned to at least 4.5:1 on `--panel` in both themes; the bars use
`--accent`, `--warnText` and `--danger`. Tooltips give the raw counts and the
current use. Details lists the twelve summed block fields, muted, in as many
columns as the pane allows. The four columns are `minmax` tracks (54-76px,
24-150px, 58-74px, 58-74px, 12px gaps), so the table fits the narrowest Prefab
Details pane (297px, 282px of content) without a horizontal scrollbar.

"Map after" is live. Every `budgetUsage` the host sends re-renders the rows and
bars, and the host sends one whenever the map's usage changes. The rule while a
prefab is held: the editor's usage already counts a Load / Place that is held in
the 3D view, before Space (the paste instantiates the entities, and a module
placement installs its modules, before the grab; snaphak-re measured the usage
held and after Space identical for object and module prefabs on both
renderers). So while the selected prefab is the held one (`held` names its name
and folder), "Map after" is the live usage itself and the bar's faint current
part grows to it, with no solid share added; "This prefab" still shows the
prefab's own share and a note says it is held. After Space or Esc `held` is
null and the row reads usage plus this prefab again, which after Space is
placing it a second time. Any other prefab, held or not, uses that ordinary
rule (`prefabBudget.meters(add, usage, limits, held)`).

## Prefabs and their mods

A prefab is one file of plain JSON. Its description, tags, a module prefab's summary and the
mods its entities and modules need live in the file's own top-level
`snapmapPlus` block, after the engine's members (`backend/prefab_block.h`,
`backend/prefab_mods.c`, see [architecture](architecture.md#prefabs-that-name-mods)).
Create from Selection removes the comments the engine's serializer writes
before each member (`sh_prefab_block_strip_comments`, the backend's function,
which `MODS_RECORD` applies again), writes the file and then asks the backend
to write the block into it (`MODS_RECORD`); a module prefab's save
writes the file with its block. Description and tag edits go through
`META_WRITE`, which rewrites the block in place and keeps its summary and mod
rows; clearing both fields clears them. No sidecar is written. The host never
hands the block to the engine: Load / Place strips it after the mods are
resolved and before the paste or the module merge, and Prefab Details sends
the page the scene without it (`prefabTransform.parseScene` also drops the key,
so the preview and Limits never see it).

The page reads a prefab's details as the view, the flat object
`{desc, tags, ...summary..., mods}` that `META_READ` returns from the block.
A prefab saved before the block has none; when a `<name>.meta.json` sits beside
it, the view is that sidecar, read-only: the first description or tag edit
writes the block into the file and leaves the sidecar as it was. The list
reads each prefab's view from at most the last 1 MB of its file (where the
block is) for its tags, module kind and mods; it still never sends a scene,
and a `.meta.json` is never listed as a prefab.

The list shows a pill per prefab that records mods: "Mods: <id>" when every
one is installed, "Needs mods: <id>" (warning colours) naming the missing ones
otherwise, "+N" when there are more, and every mod with its state in the
tooltip; the filter matches "mods" and "needs mods". A mod counts as installed
under the asset rule (an installed folder of its id provides every name the
prefab's row lists, whatever its hash; `MODS_STATUS` rows carry `state`
`served`, `missing` or `incomplete` and the refusal sentence as `cause`).
Prefab Details shows a Mods section under the module warning: one row per mod
with Installed or Not installed, and a line saying that Load / Place offers to download missing mods
when the mod store has them and otherwise to ask for the prefab's ZIP export.
The module warning stays above it; a module one of the rows provides is a
mod, not a missing room.

Load / Place asks the backend first (`MODS_RESOLVE`, with the whole file) for
either kind of prefab; one that names no mods is answered without the game. It
answers place now, held (the host follows it with `MODS_POLL` on each
think-loop turn and places the prefab again, through the same Load / Place,
once the mods are installed), a module the player cannot get, or refused. The
page shows the backend's text as a toast. Export (Prefab Details head) always
writes one ZIP, `<name>.json` and each mod as it is installed,
`mods/<id>/mod.json` and `mods/<id>/assets/...`, through the rawmap pickers'
Save dialog with a "Prefab export (*.zip)" type, proposing `<name>.zip`; it and
Import open in the last folder used for each, Downloads the first time (see
File dialog folders above).
Import (the list head) uses the Open dialog with "Prefab exports (*.zip,
*.json)": a ZIP, or the `.json` inside an extracted export folder, whose
`mods/` folder holds the mod folders (`rawmap_picker.h`, `for_prefab_dialog`).
Import is the ordinary install of each mod folder, as the Mods tab's install
button does it; a carried folder whose id an installed folder declares and
whose names that folder provides is skipped. The backend reads and writes them
on the UI thread, with no network.

The list follows the prefabs folder: the host's folder watcher (`folder_watch.h`)
sends a fresh `prefabs` list 300 ms after the last change of a burst under
`%LOCALAPPDATA%\snapmap-plus\prefabs\`, the product's own writes included. The
page renders it as any list and keeps its selection while that prefab exists;
otherwise Prefab Details is cleared. The
pure helpers sit between `// --- prefab mods (pure) ---` markers in
`mockup.html` and are tested by `tests/prefab_mods_test.js`. A list row's
leading text node is the prefab's file name and its Blueprint and Mods pills
follow it, so Rename takes the name with `pfItemName` and restores the row's
markup when the field closes (`tests/prefab_rename_test.js`).

## Mods tab

`mods_tab.js` renders the tab from the document `backend/modui.c` builds behind the `mods` slot.
Showing the tab and the Refresh button send `modsAction {action: "rescan"}`. Every action is
answered with `modsActionResult` and then a fresh `mods` document (the Files view's two reads,
`modFiles` and `modFileText`, with the result alone), one job at a time, and nothing polls. The
mods folder is not watched: an author's edit shows after the next Refresh, which the empty state,
How mods work and New mod's result say ("After you change a mod's files, press Refresh to see the
change."). The page renders only from the last document and the last dry run, never from its own
bookkeeping. Every folder's state, conflicts, replaced game files and verification stay in
`sh_mods doctor [verify]`.

**Vocabulary.** Nothing a player reads names a salt, a hash, a build number, "bundle", "key",
"pointer", "surface", "legacy" or "migrate". The backend says every cause, warning and result as
plain sentences (`mu_plain_cause`, `mu_plain`: the builder's shape and build causes are rewritten,
a salt in a path becomes `<mod id>`); the mod's salt reaches the page only as `salt`, which the
Files view's paths show and **Copy mod id** copies (for authors writing paths and strings by hand).
`tests/mods_tab_test.js` and `tests/modui_test.c` check every text a player reads against that list.

**List pane.** The panel head is the Prefabs tab's: the title, the mod count badge, and six icon
buttons: the attention filter (a yellow triangle, shown only when a mod needs attention; its hover
carries the count, "1 mod needs attention: show only these"), This map, Refresh, Open mods folder,
New mod (`folder-plus`) and install (primary). At the pane's 220 px minimum all six stay inside the
pane and the label is clipped. The toolbar holds the filter alone; it matches name, folder,
organizing folder and the map's name. The install dialog allows several folders, joined with
newlines in the action's argument; a picked folder without `mod.json` stands for every mod folder
below it, and the result says how many were installed and how many were not, with the first one's
plain cause.

The list has two kinds of groups, each a Prefabs folder row (caret, title, count) that closes for
the session: **Your mods** (the root of `mods\`), holding the organizing folders of `mods\` first
(their own caret rows, `group`) and then the mods at the top; and one group per downloaded map
(`place`, `mods\maps\<map title>\`), titled with the map's name (`placeTitle`), holding the mods
that map brought. A closed group with a mod that needs attention shows the triangle on its row; a
filter, the attention filter and selecting a mod inside open it. Inside a group the rows are sorted
by display name without case, then by folder. Each row is one status glyph, the mod's name and
its size (a mod has no version). The glyph is a yellow triangle when the mod needs attention (`state`
`attention`; its hover is the cause), else the map icon when the open map uses it (its `key` is an
entry of the document's `map.mods`), else a green check (`ready`; hover "Ready to use.", or for a
downloaded map's mod "Came with the map <name>; only that map uses it."). Arrows, Home and End move
the selection over the rows shown.

**Details pane.** The selected mod's name, glyph and one sentence: "Ready to use.", the downloaded
map's line, or the backend's one plain sentence saying what is wrong and what to do (`cause`, for
example "This mod could not be built: ...; fix that in its folder and press Refresh."). A mod that
carries at least half of its files at the same bytes as another of the player's mods shows the
informational hint "Carries N files that <name> also carries. They stay two separate mods."
(`sameFilesAs`); nothing is merged and it is never attention. Then the sections:

- **About**: Description, Version, Author, Came with (a downloaded map's name), Folder (opens it in
  Explorer: `openFolder` with the row's `key`) and the Copy mod id link.
- **What it adds**: its Toybox rows by name (`editorRowNames`), or "Adds no Toybox rows.".
- **What it changes in the game** (`facts`, only for a built mod): the game files it edits, in
  words ("Edits 2 game files; a map that uses this mod gets the game's text with these edits.", one
  line per file: fields set and removed, list entries added, named entries added or replaced,
  entries added and removed), the game files a received folder replaces, the encounters it lists,
  the game sound banks it loads with this session's state, and its editor tweaks (on this computer
  only); else "Changes nothing of the game; it only adds.".
- **What it borrows from the game** (`borrows`): a count that opens to the game files it reads from
  the install, named by the game file, with the note that game files are never stored or uploaded.
- **Console settings**: the cvars it needs and the commands run when it is used and no longer used,
  then "Every player needs Snapmap+ to play a map that uses this mod." (for a folder of editor tweaks
  only, `facts.editorOnly`, "Changes only this computer's editor; maps made with it need nothing.").
- **Files**: its file count and size and a link to the Files view.
- **Warnings**: the builder's warnings in plain words (`warnings`); a warning never makes a mod
  need attention.
- **Sharing**: how many of the player's published maps use it (not for a downloaded map's mod).

The head actions are, left to right, Files (`folder-tree`), Use in this map (Remove from map once
the open map uses the mod; shown only while a map is open; never for a folder of editor tweaks only;
disabled for a mod that needs attention and for a downloaded map's mod, which serves only its map)
and Uninstall. Refresh is the one action that builds: there is no Rebuild.
Use in this map sends `mapAddMod` (no dry run: it deletes nothing). With nothing selected the pane
explains how mods work.

**Map modal.** The map button opens the decl editor's focus modal (`#modsMapModal`; the backdrop and
Escape close it, Tab stays inside, focus returns to the opener) with the open map's details (name,
published and whether its mods were uploaded with it, the search tag `mods`, how many mods and their
size for a player to download, last save) and one row per mod the map uses: its name, its
size, the entities placed from it and the wires and Timelines pointing at them, each with Remove
from map. A map-level mod (`attached`) reads "Used by the whole map", or "Used by the whole map,
nothing placed" (`nothingPlaced`). A mod the map needs that is not installed reads "not installed on
this computer; playing the map downloads it". When several of the map's mods edit one field of a
game file to different values (`registryClashes`), **Choices between mods** shows one select per
slot ("<path> in <game file>") naming the mods; a change sends `chooseRegistry` with
`"<slot>\t<salt>"`. Remove from map and Uninstall first send a dry run; its answer is the
consequence list the confirmation shows, with exactly two buttons (Cancel, and Delete from map or
Uninstall). A map left with no mods reads as vanilla.

**Files modal (read-only).** Files opens the selected mod's folder in the standard modal at the
expanded declaration editor's size (`#modsFilesModal`; Escape and the backdrop close it, Tab stays
inside, focus returns to the opener; every render keeps focus inside). Its head is the mod's name
and the details pane's sentence. On the left, a filter and the folder as a tree with the Prefabs
primitives (a folder holding only one folder shown as one row; a folder above `mods/` keeps its own
row; the paths here show the mod's own `mods/<salt>/` segment); a file row carries one glyph at
most: the triangle when the cause names it, else the `gamepad-2` glyph for the game's own bytes. On
the right: the path once, the file's class as one phrase ("New file", "Your edit of a game file", "A
game file under this mod's own name", "A game file SnapMap cannot see on its own", "Replaces a game
file", "An editor tweak, on this computer only", "About the mod: ..." for `mod.json`, "Not read yet"
for a file the build does not hold), where a game file comes from, and a text file's text in the
declaration editor primitive, read-only (`mod.json` without the part Snapmap+ writes). The plain cause
naming the file is shown in the Entity State problem strip. The two reads never set the page's busy
state or its status line.

**New mod.** The header's New mod button, the empty list's card and the list's right-click menu
("New mod...") open a small modal (Name, Cancel, Create; Enter creates, Escape cancels) that sends
`newMod`. Its answer ("Created <name> in mods\<folder>. Its Example crate is in the Toybox. After you
change its files, press Refresh to see the change.") selects the new mod and opens the Files modal on
the example's Toybox row, with one line above the editor: "Place Example crate from the Toybox in
your map. To change the mod, edit this file in the mod's folder, then press Refresh in the Mods
tab."

**Uninstall goes to the Windows Recycle Bin.** Its confirmation reads "Undo history is cleared. The
folder can be restored from the Recycle Bin by hand. Uninstalling:" (with a map open; without one,
the second sentence alone) and lists the map part, the Toybox rows it removes, "Moves the folder
mods\<folder> to the Windows Recycle Bin." and "N of your saved maps use it; they will not open until
a mod folder provides its files again" (`install.savedMaps` above 0), or, when another folder of the
mod stays installed (`install.keptBuild`: the player's own, or the copy a map brought), "Another
folder of this mod stays installed (mods\<folder>); maps that use the mod open with it."

A pushed document: once the page has asked for the document, the host reads the backend's
generation every 500 ms and, when it has moved past the last document's while no job of its own
runs, posts `{"kind": "mods", "pushed": true, "data": ...}`, so a console verb (`sh_mods map add`,
`sh_mods uninstall`, `sh_mods choose`, a build or rescan) reaches an open tab. The page renders it
as any document and keeps its status line.

In the browser preview, `preview_standin.js` answers the document and Refresh with
`tests/fixtures/mods_tab_payload_v4.json` (`PREVIEW_MODS` must equal it) and the Files view from
`PREVIEW_MOD_FILES`; every other action says it runs in the game. `mods_tab.js`, which the release
page carries, holds no stand-in. `tests/mods_tab_test.js` drives the page through its own fake host,
and `tests/modui_test.c` checks the document against a temporary store and a fake editor, including
that every key of that fixture is one the backend writes.

### Colour in the details pane

The details pane colours a thing by its kind with the declaration editor's own token hues
(`studio.css`, both themes). Colour marks a kind, never decoration; every coloured part, or the line
holding it, has a hover title naming its meaning.

| Thing | Class | Hue token | Meaning (hover title) |
|---|---|---|---|
| A path's folder | `mk-dir` | `--muted` | where the file sits |
| A path's own `mods/<salt>/` segment (Files view) | `mk-own` | `--tkStr` | "the mod's own file" |
| A game path's file name (Edits, Replaces, Borrows) | `mk-game` | `--tkBool` | "a game file this mod edits", "replaces", "reads from your install" |
| A game sound bank | `mk-game` | `--tkBool` | "a game sound bank, read from your install" |
| What an edit sets or adds | `mk-set` | `--tkNum` | "what this mod sets or adds in the game's text" |
| What an edit removes | `mk-removed` | `--muted` | "what this mod removes from the game's text" |
| A console setting's name | `mk-key` | `--tkKey` | "a console setting this mod needs" |
| A console setting's value: a number, else a string | `mk-num`, `mk-str` | `--tkNum`, `--tkStr` | "the value it is set to while a map uses this mod" |
| A Toybox row name, an encounter | `mk-str` | `--tkStr` | "a Toybox row this mod adds" |

### Payload v4 (`modui.c`)

`{"v": 4, "gen", "root", "storeReadable", "mods": [...], "map": {...}}`. There is no `migrate`
object and no build list: one folder is one row.

Per entry of `mods`:

| Key | Type | Meaning |
|---|---|---|
| `"key"` | string | The folder's path under `mods\` (`campaign-interactables\doors`, `maps\Hell Guard Arena\Hell Guards`): what every action names. |
| `"salt"` | string | The mod's identity (its `snapmap-plus.salt`); "" before its first build. The page uses it only for the Files view's paths and Copy mod id. |
| `"name"`, `"author"`, `"description"` | string | The author's part of `mod.json` (a folder that is not built is named by its `mod.json` when that parses). |
| `"size"`, `"files"` | number | The carried bytes and files (game files read from the install not counted). |
| `"state"` | string | `ready` or `attention`. |
| `"cause"` | string | For `attention`: one plain sentence saying what is wrong and what to do. |
| `"editorRows"`, `"editorRowNames"` | number, string[] | The Toybox rows it adds and their display names (strings resolved, else the decl name without the mod's namespace). |
| `"folder"`, `"group"` | string | The folder's own name, and the organizing folder it sits in at the root ("" at the top and for a map's mods). |
| `"place"`, `"placeTitle"` | string | For a mod a downloaded map brought: its place `maps\<segment>` and the map's name; "" for the player's own. |
| `"requirements"` | object | The builder's canonical `{commands?: {activate?, deactivate?}, cvars: {name: "value"}}`. |
| `"facts"` | object | For a built mod: `{adds, borrows, surfaces, editor, replaces, editorOnly, editorTweaks, replacements: [{path, edits}], edits: [{file, ops, set, remove, list, keyed, bag, drop}], registries, stockBanks?}`; `registries.stringEdits` lists the game strings the mod changes (shown as "Changes the text of N game strings"). |
| `"borrows"` | object | For a built mod: `{paths: [the game file each borrowed file reads], count}`. |
| `"warnings"` | string[] | The builder's warnings, in plain words. |
| `"publishedMaps"` | number | How many of the player's published maps record its build; absent when unknown. |
| `"sameFilesAs"` | object | `{name, folder, count}` when another of the player's mods carries at least half of its files at equal bytes; absent otherwise. |

`map` keeps `entities` (-1 when no map is open), `names`, `rows` (`{salt, name, build, size,
state, cause, key, nameCount, names, attached}`: `state` active, incomplete, skipped or missing,
`cause` the plain refusal) and `placed` (`{salt, build, size, key, viaNeeds, inManifest,
nameCount, names}`), and while a map is open in the editor adds `name`, `saved`, `published`
(`{state, at}`), `uploaded` (`{claimed, at}`), `tagged`, `mods` (one `{key, salt, name,
state, "placedEntities", placedCount, placedModules, "references", size, inManifest, viaNeeds,
"attached", nothingPlaced, build}` per mod the map uses), `"vanilla"` and `registryClashes`
(`{slot, providers: [{id: salt, name}], chosen, serving}`).

### Actions

`arg` is a mod's `key` (for a map row that is not installed, its salt). Each is answered with
`modsActionResult {action, ok, message, data}` and then a fresh `mods` document.

| Action | Meaning |
|---|---|
| `rescan` | Refresh: the scan (new folders get their salt, copies a salt of their own, changed folders are built), the serving index, and what changed brought into the running game. Its message is only the sentence a change still needs ("Some of the changes take effect only after you restart DOOM.", "... when you next open the map.", "... when you are back in the editor."), shown even though Refresh is otherwise quiet. |
| `install` | Install one or more folders (the native picker). |
| `openFolder` | The host's own: opens the mods folder, or with `arg` that folder under it. |
| `mapAddMod` | Attach the mod to the open map without placing anything (a map-level row for the build of its folder at the root); the next save writes it. A folder a downloaded map or a prefab brought names its mod: the root folder of its salt is attached, and without one the answer sends the player to Add to my mods. The window never sets where a map finds its mods. |
| `chooseRegistry` | `"<slot>\t<salt>"` (an empty salt clears): the open map's choice of whose edit serves that slot. |
| `mapRemoveModPreview`, `mapRemoveMod` | The dry run and the removal of a mod from the open map: its placed entities in one undo step, the wires and timeline references to them, its row, and with the last mod the search tag. |
| `uninstallPreview`, `uninstall` | The dry run and the uninstall: the open map cleaned of the mod, its undo history cleared, the folder moved to the Recycle Bin, the rescan. |
| `modFiles`, `modFileText` | The Files view's reads (no fresh document follows). |
| `addToMine` | A downloaded map's mod (a folder under `mods\maps\`): copied to the root of `mods\` as one of the player's own mods under the same salt, which their own maps then use; `data` is `{key}`, the new folder. A mod already at the root answers its folder. File work off the game's thread, like Install. |
| `newMod` | `arg` is a display name: `mods\<name>\` with `mod.json`, a new salt and a working example (an entityDef and its Toybox row), built at once; `data` is `{key, file}`. |

An action the host does not know is answered "Unknown mods action."; an action number the
backend does not know is answered "Unknown action.".

`data.preview` is `{action, key, name, map, install}`, plus `editorRows` for `uninstallPreview`.
`map` is null when the open map does not use the mod, else `{name, entities, entityCount,
references: [{kind, name}], manifestRow, tagRemoved, vanillaAfter, published, attached?}`; `install`
is null for `mapRemoveMod`, else `{folder, disposition: "recycle", "savedMaps", "undoCleared",
keptBuild?}`. A refused dry run is `ok: false` with the plain reason in `message`.

| Console verb | Tab equivalent |
|---|---|
| `sh_mods map` | the map modal |
| `sh_mods map add "<mod folder>"` | Use in this map (`mapAddMod`) |
| `sh_mods map remove "<mod folder>" [--yes]` | Remove from map |
| `sh_mods uninstall "<mod folder>" [--yes]` | Uninstall (every folder of the mod: the player's own and the copies maps brought) |
| `sh_mods choose "<slot>" "<mod folder>"` | Choices between mods (`chooseRegistry`) |
| `sh_mods rebuild "<folder>"` | Rebuild (`rebuild`) |
| `sh_mods install "<folder>"` | the install button (`install`) |
| `sh_mods rescan` | Refresh (`rescan`) |
| `sh_mods new "<name>"` | New mod (`newMod`) |
| `sh_mods verify "<folder>"`, `sh_mods doctor [verify]` | none (every folder's state, conflicts, replaced game files, a check of every file) |

## Timeline events

The event picker is fed by `enumEvents` from the generated `sh_event_catalog.h`, which on
build 13954591 lists the same 1611 events and argument types as the live registry
(`sh_event list`). Save builds each call with `tlBuildPatchedEntityJson`: the event under
the engine's `"\neventHandle_t eventDef"` key and each argument keyed by its type in
signature order. `tlRuntimeTargetNote` shows a note under **Runs on** when the target is
`player1` to `player8`, because a timeline resolves its targets when the map starts, before
any player exists. The page asks for the catalogue (and the event docs) when the first
timeline opens and renders that timeline before they arrive, so the `events` and
`eventDocs` handlers render the open timeline again; until then an argument is labelled by
its stored type ("bool  (not in the event catalogue)"). `tests/timeline_eventcall_test.js`
lifts these functions from the page and checks the saved shape, the reopen, the note and
that every named bool argument is labelled by its catalogue name.

## Keyboard, dialogs and messages

Keys belong to the control they are typed in (`page_keys.js`): arrow keys, Delete and
Escape act on a list only from its filter box or when no text field, list box, dialog or
menu has focus. Every dialog opens through one stack: Escape closes the top one (after
any popup inside it used the key), Tab stays inside, and focus returns to where it was.
Menus, right-click menus and the tab strip (`role="tablist"`) walk with the arrow keys;
Shift+F10 or the menu key opens the selected row's right-click menu. Toasts are read out
(a polite live region); an error toast is an alert and stays until it is dismissed.

A message the player sees says the cause and the next step in plain words. It never
points at a log, guesses ("editor down?"), names a console command, or uses the
reverse-engineering vocabulary; `tests/page_messages_test.js` scans every string the page
and the host can show. Each tab's empty state says what the tab is for.

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

The prefabs folder watcher runs on its own thread (`ReadDirectoryChangesW`) and only notes
changes; the think loop decides when the Prefabs list refreshes (`folder_watch.h`, tested by
`tests/folder_watch_test.c`) and logs one `watch:` line per refresh.

Camera polling runs at the loop cadence and posts only changed coordinates.
The prefab list never reads a prefab scene: previews, and the Limits section,
are only ever loaded for the one selected prefab. That scene is parsed once
and the parsed document is shared by the 3D preview, the AI Navigation count
and the Limits sum, which is memoized (32 entries) by folder, name, file
signature and table generation, so reselecting a prefab costs one cache hit
and the meters are recomputed from the latest usage. The budget table is held
once by the page and looked up by key. The map's usage is read at the same
330 ms cadence once the page has asked for it and posted only when it or the
held prefab changed.
Entity state and selection are sampled roughly every 330 ms: the backend exposes no
generation for entity or declaration edits, so the host copies the shown entity's
declaration and compares a hash (the budget table and the Mods tab already follow the
backend's generations). Timeline scanning
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
- A new timeline is found by comparing the timeline list before and after its
  paste: the backend does not report the pasted entity's ID.
- The File menu answers in browser preview with the default path pair, so the
  readout and layout can be checked outside the game. Load and Save As are inert
  there and report preview only.

Rawmap saves capture the live map and destination as one queued request. The
frame checks editor state and the load generation before serializing; a new
map can reuse the same pointer. Save Rawmap resolves its destination in the
backend rather than sending a path from an earlier page status. Neither a
refused save nor a cancelled request falls back to another map on disk.
