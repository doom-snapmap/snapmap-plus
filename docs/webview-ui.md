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
| `src/ui/webview/mods_tab.js` | The Mods tab: payload model, the grouped installed-mods list, the selected mod's details, the map modal and its confirmations, the action messages, and the browser preview's stand-in host. |
| `src/ui/webview/prefab_budget.js` | What a prefab adds to the editor's limits: block sums, the five HUD meter ratios, the reached rule, the meter colour levels and a bounded memo. No DOM. |
| `src/ui/webview/prefab_host.h` | The host's Prefabs tab decisions: Create's precheck, a prefab's kind by its file's `~type`, and the live usage gate. Pure; `tests/prefab_host_test.cpp`. |
| `src/ui/webview/rawmap_picker.h` | The file dialogs' types and the folder each opens in (its key in `config.json`, its default). Pure; `tests/rawmap_picker_test.cpp`. |
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
| Create prefab | `serialize_selection` at `+0xB0`; the engine's copy check (PrefabPopulate) requires the hovered entity to be one of the selected ones. The host checks `get_selection` and `hovered_id` first (`prefab_host.h`) and answers `createPrefabResult` 0 nothing selected, 2 nothing hovered, 4 the hovered entity is not selected, or, when the serializer still returns nothing, 5 (the check refused for its other causes: a wired or referenced entity left out, or one the editor does not copy; the backend logs its status). |
| Load / Place | `apply_edit` at `+0xD0`, kind 2: stage the prefab and request the native paste action. The game thread decides on its next frame whether the paste was queued or the prefab only staged, and why (`prefab_watch`'s `place_seq`, `place_result`, `place_reason`); the host waits up to 2.5 s for that and posts `loadPrefabResult` with `placed`. |
| Prefabs tab watch | `prefab_watch` at `+0x380` (ext 35): the modules selected in Blueprint, whether a Load / Place is held in the 3D view (`SH_PREFAB_HELD_OBJECTS` while EntityMode `+0x1AC` reads 4 after a queued paste, `SH_PREFAB_HELD_MODULES` from a module placement's install to Space or Esc) and the last paste decision. Guarded reads from the host's thread, nothing waits for the game. |
| Save Timeline | `apply_sync` at `+0x290`, kind 0, with the target entity ID. |
| Timeline tile | `timeline_tile` at `+0x368` reports the toybox tile's palette row, its prototype record class and the rebind counts. The page does not call it; the `+0x298` normaliser is retired. |
| Mods tab | `mods` at `+0x378`: LIST and RUN build a document on the game thread (call them inside `run_on_main`), FETCH copies it on any thread, ROOT returns the store folder, WAIT follows a migration from any thread. See [Mods tab](#mods-tab). |
| Prefab limits | `budget` at `+0x370`: TABLE copies the budget table the backend built after the last palette build, USAGE the map's limits, usage and reached byte from the last editor frame. |
| Persistent settings | `config_get_json` at `+0x2B0` and `config_set_json` at `+0x2B8`. |
| Prefab geometry | Model lookup and request/completion slots at `+0x308` through `+0x320`. `resolve_prefab_defaults` (`+0x320`) also reports a palette-row model (`SH_PREFAB_DEFAULT_EDITOR`) and, given six floats, that row's `renderInfo.size` (`SH_PREFAB_DEFAULT_SIZE`); see [Prefabs 3D preview](#prefabs-3d-preview). |
| File menu rawmap I/O | `rawmap_status` at `+0x328`, `rawmap_configure` at `+0x330` and `rawmap_load_now` at `+0x338`. The picker is host-side and touches no engine state; its file types come from `rawmap_picker.h` (Load lists `.json` and `.zip`; Save As puts `.zip` first when the status's `mapMods` is above zero, read on the UI thread before the picker thread starts). Loading stages a file: the swap substitutes it into the next map the engine parses. A file `rawmap_configure` refuses (a prefab's export, an earlier release's `.zip`) is logged by the backend (`B1: rawmap: load of <path> refused: <reason>`), printed to the console on the next game tick ("The rawmap was not loaded: <reason>.") and logged by the host (`rawmap load: refused`), besides the page's "Refused:" toast. The status's `lastZip` fields name the export the last save wrote, and the path readout says to share it. |
| File dialog folders | Every dialog opens in the last folder the player used for its kind, kept in `config.json`: `dialogs.rawmap_load`, `dialogs.rawmap_save`, `dialogs.prefab_export`, `dialogs.prefab_import` (UI read and write, `""` until first used). Without a kept folder, or when it no longer exists, the prefab dialogs open in Downloads (`SHGetKnownFolderPath(FOLDERID_Downloads)`) and the rawmap dialogs in the rawmaps folder, `%LOCALAPPDATA%\snapmap-plus`. The host reads the folder on the UI thread before the dialog's thread starts and keeps the picked file's folder when the dialog returns. |
| View menu navigation | `navmesh_view` at `+0x340` reports whether the preview is out of date and, when asked, requests an update on the game's next frame. |
| Engine edits | `run_on_main` at `+0x348`. Selection, delete, Entity State save and camera writes run on the game's thread; the page waits up to one second, then drops the edit and logs it. |

Prefab files use the path returned by `resolve_prefab_path`; their rename,
delete and folder operations are native filesystem work (an older prefab's
read-only sidecar moves with it). Feedback and
crash submission use a short-lived HTTPS worker in the host and the
[feedback pipeline](feedback.md).

## Page and host messages added for the tab trees

| Direction | Message | Meaning |
|---|---|---|
| host to page | `editorMode {entityMode, blueprint, modules}` | The editor state, posted on change: module prefabs place only in Blueprint, object prefabs and new timelines in Object mode. `modules` is the number of modules selected in Blueprint (0 outside it, -1 when the backend cannot say); with `selCount {count}`, the selected entities, it gates Create (below). |
| host to page | `createPrefabResult {result, name, grab?}` | 1 saved; 0 nothing selected; 2 nothing hovered; 3 no usable grab frame; 4 the hovered entity is not selected ("The cursor is over an entity that is not selected -- hover one of the selected entities in the 3D view, then try again"); 5 the copy check refused the selection; -1 failed. |
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
P29's module warning stays above it; a module one of the rows provides is a
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
`mockup.html` and are tested by `tests/prefab_mods_test.js`.

## Mods tab

`mods_tab.js` renders the tab from the document `backend/modui.c` builds behind the `mods` slot.
Showing the tab and the refresh button send `modsAction {action: "rescan"}`. Every action is
answered with `modsActionResult` and then a fresh `mods` document, one job at a time, and nothing
polls. The host's folder watcher also asks for the rescan by itself 1500 ms after the mods folder
goes quiet (`SH_MODS_ACT_WATCH_RESCAN`, deferred by the backend while Play runs, a map loads or a
store write holds the lock, and asked again two seconds later); it posts only the fresh `mods`
document, never a `modsActionResult`, and changes seen while the product's own store lock is held,
or within a second after, are ignored (`SH_MODS_STORE`). The page renders only from the last document and the last dry run, never from its own
bookkeeping, so counts, manifest rows and the published state are always what the backend just
read. Every bundle's state, conflicts, shadows and verification stay in `sh_mods doctor [verify]`.

**List pane.** The panel head carries the mod count and, when any mod needs attention, a yellow
badge button with that number; pressing it (`aria-pressed`) shows only those rows. One toolbar row
holds the filter, refresh, Open mods folder, Migrate (only while `migrate.pending > 0`) and install.
Rows are grouped under collapsible headers (the prefab tree's folder head and caret) by origin:
Your mods (no origin, or `migration`: those were the owner's overrides), Installed (`install`) and
From maps you played (`download`, `delivery`). The collapsed groups are the setting
`mods.collapsed_groups`, a list of group ids (`yours`, `installed`, `maps`) saved through the
configuration service like `prefabs.open_folders` (`config.c`: `SH_JSON_ARRAY`, `"[]"`, UI read and
write). Browser preview never saves it. A filter or the attention badge shows matches in every group;
a header folded while filtering is not saved. Each
row is a status icon, the name, and `id · version · size`. The three icons cover every backend
state: a green check (`installed`), a yellow triangle (`attention` or `duplicateId`; the hover
title is the cause) and a grey info dot (`installed` with `servedBy` set). The list is a plain list
scrolled natively by the panel body; arrows, Home and End move the selection over the rows shown.

**Details pane.** The selected mod's name, icon and one sentence ("Installed and serving 24
files, 476 more read from your install", "Needs attention: ..."), then About, What it adds, Files (carried count and size, the files
read from the installed game, and the carried paths in the Limits section's `details` primitive),
Requirements (console settings, the commands run when the mod is active and when it is removed, and
the note that every player needs Snapmap+) and Sharing. About carries an Applies line when the
backend sends the mod's `scope`: "Only in maps that use it" for a map-scoped mod with shadows or
requirements, "In every map" for `always` (with "it changes the editor itself" when `scopeAuto`),
nothing for a mod that only adds new names. The head actions are Use in this map, Verify, Rebuild
and Uninstall. Use in this map shows only while a map is open in the editor; it sends `mapAddMod`
for the selected mod (no dry run: it deletes nothing) and, once the map lists the mod, reads Remove
from map and opens the same confirmation as the modal's row, which closes after the action. With
nothing selected the pane explains how mods work.

**Map modal.** The map button in the list pane's head opens the decl editor's focus modal
(`.modal.decl-focus-modal` in `#modsMapModal`; the backdrop and Escape close it, Tab stays inside,
focus returns to the opener) with the open map's details and one row per mod the map is linked to,
each with Remove from map. A map-level mod (`attached`) reads "Used by the whole map" instead of its
placed-entity count, and removing it deletes nothing. Remove from map and Uninstall first send a dry run; its answer is the
consequence list the confirmation shows, with exactly two buttons (Cancel, and Delete or
Uninstall). After the real action the next document redraws the modal; a map left with no mods
reads as vanilla.

**Uninstall goes to the Windows Recycle Bin.** Its confirmation reads "Undo history is cleared. The
folder can be restored from the Recycle Bin by hand. Uninstalling:" (when `install.undoCleared`;
without it, "The folder can be restored from the Recycle Bin by hand. Uninstalling:") and lists,
after the map part and the Toybox rows, "Moves the folder mods\<folder> to the Windows Recycle Bin."
and "N of your saved maps use it; they will not open until a mod folder provides its assets again"
(when `install.savedMaps` is above 0). The result reads "Its folder was moved to the Recycle Bin",
or, when the bin could not take it, "Its folder was deleted because the Recycle Bin could not take it
(<why>)". The product keeps no bin of its own: the footer has no removed-mods line and the page sends
no `removedEmpty`; a document's `removed` object, sent by an older backend, is not read.

In the browser preview, `modsTab.standIn` stands in for the host: it answers each action with a
result and a fresh document and changes its copy as the backend would, starting from
`tests/fixtures/mods_tab_payload_v3.json` (the 36 folders of the owner's store; the page's
`PREVIEW_MODS` must equal it). `tests/modui_test.c` checks the v3 document against a temporary
store and a fake editor, including that every key of `tests/fixtures/mods_tab_payload_v3.json` is
one the backend writes; `tests/mods_tab_test.js` renders both fixtures (`mods_tab_payload.json` is
the v2 document an older backend sends).

### Payload v3 (`modui.c`)

v3 is v2 plus the fields below, with `"v": 3`. Every v2 key stays. Each new field is optional: the
page leaves out what is absent, so a v2 document still renders.

Per entry of `mods`:

| Key | Type | Meaning |
|---|---|---|
| `"description"` | string | mod.json's description, "" when none. |
| `"origin"` | string | The product block's origin: `install`, `download`, `migration`, `delivery`, or "" for a folder the author made. |
| `"migratedFrom"` | string | For `migration`: the folder it was built from, as `mu_origin` writes it (`overrides\hell-guards`). |
| `"servedBy"` | string | The key of another folder whose build serves this mod's paths (superseded); "" when it serves its own. Drives the grey icon. |
| `"builtAt"` | number | Unix seconds of the product block's `built`; 0 when unknown. |
| `"bundle"` | string | The full bundle hash (64 hex). The page shows the first 16 for support questions. |
| `"counts"` | object | `{shadows, sounds, movies, modules, vt}`: replaced stock files, sound banks, movies, snapModule decls, virtual textures. |
| `"requirements"` | object | The builder's canonical requirements, `{commands?: {activate?: [line], deactivate?: [line]}, cvars: {name: "value"}}`; `{cvars: {}}` when none. Whether each is live is in `sh_mods status` and `sh_mods doctor`, not the payload. |
| `"editorRowNames"` | string[] | Display names of the Toybox rows it adds (`editorRows` resolved through strings when available, else the decl names). |
| `"carriedPaths"` | string[] | Carried request paths below `assets/` (stock pointers excluded), sorted. May be capped; the page says how many more there are. |
| `"publishedMaps"` | number | How many of the player's published maps record it; absent when unknown. |
| `"scope"` | string | Where its shadows and requirements apply: `map` (only while a map that lists it is open or played) or `always`; absent when the serving index does not hold it. |
| `"scopeAuto"` | bool | `always` because it shadows editor-time content without a `scope` key in its mod.json. |
| `"scoped"` | bool | `map` and it has shadows or requirements, so the open map decides whether they apply. |

`files` stays every file including stock pointers, and `pointers` the stock pointers. The page
derives the carried count as files minus pointers: the head sentence counts carried files only, with
the pointers as a second clause ("Installed and serving 24 files, 476 more read from your install"),
and `size` is the carried bytes.

`map` keeps its v2 keys (`entities` is -1 when no map is open, `names`, `rows`, `placed`) and adds:

| Key | Type | Meaning |
|---|---|---|
| `name`, `id` | string | The map's name (its last local save's `game.details`, "" before the first save) and saved-map id (the editor's local id; the page shows 16 characters). |
| `saved` | number | Unix seconds of the last local save (`game.details` `modified`); 0 when unknown. |
| `published` | object | `{state: "published", at}` when the record's `links` section holds an item published from this map (written by a publish with mods from this installation, dropped at its unpublish); `at` is the link's time. Absent otherwise: the product cannot tell an unpublished map from one published without mods or elsewhere, so it never sends `unpublished`. |
| `uploaded` | object | `{claimed, at}`, sent with `published`: whether the record's `claims` section holds that item's claim, and its time. |
| `tagged` | bool | The search tag `mods` is in the editor's copy of the map's tags (what the next local save writes); absent when it cannot be read. |
| `mods` | array | One per `smpmod1.` manifest row of the map's record, and one per mod its placed entities reach that has no row yet (the next save adds it): `{key, id, version, state, "placedEntities", placedCount, placedModules, "references", size, inManifest, viaNeeds}`. `key` is the installed folder ("" when not installed); `state` is `installed`, `superseded`, `incomplete` (a folder of its id is installed but lacks a name the map's row lists), `skipped` or `missing`, judged by the asset rule ([architecture](architecture.md#mods)), so a row pinning another build of an installed id is `installed`; `rows[].state` is `active`, `superseded`, `"incomplete"`, `skipped` or `missing` the same way, with the refusal sentence in `cause`; `placedEntities` are display names (the entity's label, else its Toybox row's name, else the last part of what it inherits, then `#` and its id; the first 64); `references` are `[{kind, name}]`, what removing the mod from the map severs: `kind` is `wire` (a wire with one end on one of its placed entities) or `timeline` (a Timeline whose entity references name one); the page ignores other kinds; `size` is carried bytes. `placedModules` counts Blueprint modules placed from it, `inManifest` says the row is in the record and `viaNeeds` that only another mod's needs reach it; the page does not read these three. `"attached"` says its row is map-level (Use in this map, `sh_mods map add`): the map uses the mod without placing it. `"scope"` and `"active"` say where it applies and whether its shadows serve now. |
| `"vanilla"` | bool | The map lists no mods and plays without Snapmap+. |

The v3 map keys are sent only while a map is open in the editor (`entities` is not -1).

The document carries no `removed` object: the recycle bin is retired (an older backend sent one; the
page ignores it).

### Actions

`arg` is a mod's `key` (for a map row that is not installed, its `id`). Each is answered with
`modsActionResult {action, ok, message, data}` and then a fresh `mods` document whose `map` is read
again.

| Action | Meaning |
|---|---|
| `rescan`, `install`, `migrate`, `openFolder` | As in v2. `openFolder` may carry `arg`, a folder name under `mods\` (no separators) that the host opens instead of the mods folder. |
| `rebuild` | Build the folder again from its files (as `sh_mods build`), then rescan. |
| `verify` | Rehash that one folder's build (`""` still checks every build). |
| `mapAddMod` | Attach the mod to the open map without placing anything: the map's manifest record gets a map-level row for the build that serves (marked `attached`), its shadows and requirements apply at once, and the next save writes the row, so publishing uploads it, a player who opens the map is offered it, and the `mods` tag follows. Refused with no map open, a mod that is not installed or not in use, Play running or a map loading. |
| `mapRemoveModPreview` | Dry run: `data.preview` is the consequence list of removing the mod from the open map: placed entities, and the wires and timeline references to them. Toybox rows are not consequences, so it carries no `editorRows`. Changes nothing. |
| `mapRemoveMod` | Delete the placed entities that use the mod, sever the wires and timeline references to them, drop the manifest row and, with the last row, the `mods` tag. The published copy changes only when the map is published again. |
| `uninstallPreview` | Dry run: `data.preview` for uninstalling: the map part when the open map uses the mod, the Toybox rows it takes out of the editor (`editorRows`, whether or not the map uses it), and the installation part. |
| `uninstall` | `mapRemoveMod` on the open map when it uses the mod (a refused delete stops it with nothing deleted), the open map's undo and redo history cleared to the current state (after the delete's own commit), then the folder moved to the Windows Recycle Bin (deleted outright, and the message says so, when the bin cannot take it), whatever its origin, and its aliases dropped, then the rescan. Closed maps are not edited; one that uses the mod refuses to open until a mod folder provides its assets again. Refused while a map is open on a build whose undo clear is unavailable. Replaces `remove`. |
| `removedEmpty` | Retired with the product's own bin; the page no longer sends it and the backend answers "Uninstalled mods go to the Windows Recycle Bin." as a refusal. |

`data.preview` is `{action, key, name, map, install}`, plus `editorRows` (display names) for
`uninstallPreview` only. `map` is null when the open map does not use
the mod, else `{name, entities: [display names], entityCount, references: [{kind, name}],
manifestRow, tagRemoved, vanillaAfter, published, attached?}` (`attached` true for a map-level row); `install` is null for `mapRemoveMod`, else
`{folder, disposition, "savedMaps", "undoCleared"}`: `disposition` is `recycle`; `savedMaps` counts the
local saves in My Maps whose `map.decl` carries a row of the mod (-1 when the saves could not be read);
`undoCleared` is true when a map is open, whose undo history the uninstall clears. An older backend
sent `move` with `movedTo`, and then `delete`; the page reads any of them as `recycle`. A refused dry run is `ok: false` with the
reason in `message`: the open map does not use the mod, the mod is not installed, Play is running or
a map is loading, the mod is placed as Blueprint modules (removed in Blueprint mode), or only another
mod the map uses needs it. The real actions refuse the same way, and also when the editor refuses
the delete (it is not in Object mode, an entity or wire is held, or another object refers to an
entity, where the editor would ask first); nothing changes then.

The backend runs `mapRemoveMod` as: the placed entities in one `sh_editor_delete_batch` (DOOM's own
Delete Selection, one undo step "Delete Selection"; the editor drops the wires to them with the
entities), the mod's row dropped from the map's manifest record (the
next save rebuilds the rows from the placed entities, so it writes none for the mod), and with the
last mod the search tag removed from the editor's copy of the map's tags. `uninstall` runs that on
the open map first, then clears the undo history (`editor_undo.c`, see
[architecture](architecture.md#the-mods-tabs-map-scope)), moves the folder to the Recycle Bin (as `remove`) and rescans; the rescan's refresh rebuilds
the editor palette without the mod's rows (when the editor refuses the rebuild because an edit is
still finishing, it is retried once the editor is free). `sh_mods map`, `sh_mods map remove "<id>"
[--yes]` and `sh_mods uninstall "<id>" [--yes]` print the same dry runs and run the same actions;
`sh_mods map add "<id>"` runs `mapAddMod`.

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

The folder watchers run on their own thread (`ReadDirectoryChangesW`) and only note changes; the
think loop decides when the Prefabs list or the Mods tab refreshes (`folder_watch.h`, tested by
`tests/folder_watch_test.c`) and logs one `watch:` line per refresh or rescan.

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
