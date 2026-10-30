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

`mods_tab.js` renders the tab from the document `backend/modui.c` builds behind the `mods` slot.
Showing the tab and the refresh button send `modsAction {action: "rescan"}`. Every action is
answered with `modsActionResult` and then a fresh `mods` document, one job at a time, and nothing
polls. The page renders only from the last document and the last dry run, never from its own
bookkeeping, so counts, manifest rows and the published state are always what the backend just
read. Every bundle's state, conflicts, shadows and verification stay in `sh_mods doctor [verify]`.

**List pane.** The panel head carries the mod count and, when any mod needs attention, a yellow
badge button with that number; pressing it (`aria-pressed`) shows only those rows. One toolbar row
holds the filter, refresh, Open mods folder, Migrate (only while `migrate.pending > 0`) and install.
Rows are grouped under collapsible headers (the prefab tree's folder head and caret) by origin:
Your mods (no origin, or `migration`: those were the owner's overrides), Installed (`install`) and
From maps you played (`download`, `delivery`). The collapsed groups are the setting
`mods.collapsed_groups`, a list of group ids (`yours`, `installed`, `maps`) saved through the
configuration service like `prefabs.open_folders`; `config.c` must add it (`SH_JSON_ARRAY`, `"[]"`,
UI read and write), and until it does the backend refuses it and the groups fold for the session
only. Browser preview never saves it. A filter or the attention badge shows matches in every group;
a header folded while filtering is not saved. Each
row is a status icon, the name, and `id · version · size`. The three icons cover every backend
state: a green check (`installed`), a yellow triangle (`attention` or `duplicateId`; the hover
title is the cause) and a grey info dot (`installed` with `servedBy` set). The list is a plain list
scrolled natively by the panel body; arrows, Home and End move the selection over the rows shown.

**Details pane.** The selected mod's name, icon and one sentence ("Installed and serving 24
files, 476 more read from your install", "Needs attention: ..."), then About, What it adds, Files (carried count and size, the files
read from the installed game, and the carried paths in the Limits section's `details` primitive),
Requirements (console settings and the note that every player needs Snapmap+) and Sharing. The head
actions are Verify, Rebuild and Uninstall. With nothing selected the pane explains how mods work.

**Map modal.** The map button in the list pane's head opens the decl editor's focus modal
(`.modal.decl-focus-modal` in `#modsMapModal`; the backdrop and Escape close it, Tab stays inside,
focus returns to the opener) with the open map's details and one row per mod the map is linked to,
each with Remove from map. Remove from map and Uninstall first send a dry run; its answer is the
consequence list the confirmation shows, with exactly two buttons (Cancel, and Delete or
Uninstall). After the real action the next document redraws the modal; a map left with no mods
reads as vanilla.

In the browser preview, `modsTab.standIn` stands in for the host: it answers each action with a
result and a fresh document and changes its copy as the backend would, starting from
`tests/fixtures/mods_tab_payload_v3.json` (the 36 folders of the owner's store; the page's
`PREVIEW_MODS` must equal it). `tests/modui_test.c` checks the v2 document against a temporary
store, including that every key of `tests/fixtures/mods_tab_payload.json` (v2) is one the backend
writes; `tests/mods_tab_test.js` renders both fixtures.

### Payload v3 (to implement in `modui.c`)

v3 is v2 plus the fields below, with `"v": 3`. Every v2 key stays. Each new field is optional: the
page leaves out what is absent, so a v2 document still renders. When the backend writes v3, point
`modui_test`'s key check at the v3 fixture.

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
| `"requirements"` | object | `{cvars: {name: "value"}}` from mod.json's requirements; `{cvars: {}}` when none. |
| `"editorRowNames"` | string[] | Display names of the Toybox rows it adds (`editorRows` resolved through strings when available, else the decl names). |
| `"carriedPaths"` | string[] | Carried request paths below `assets/` (stock pointers excluded), sorted. May be capped; the page says how many more there are. |
| `"publishedMaps"` | number | How many of the player's published maps record it; absent when unknown. |

`files` stays every file including stock pointers, and `pointers` the stock pointers. The page
derives the carried count as files minus pointers: the head sentence counts carried files only, with
the pointers as a second clause ("Installed and serving 24 files, 476 more read from your install"),
and `size` is the carried bytes.

`map` keeps its v2 keys (`entities` is -1 when no map is open, `names`, `rows`, `placed`) and adds:

| Key | Type | Meaning |
|---|---|---|
| `name`, `id` | string | The map's name and saved-map id (the page shows 16 characters). |
| `saved` | number | Unix seconds of the last save; 0 when unknown. |
| `published` | object | `{state: "published" or "unpublished", at}`. |
| `uploaded` | object | `{claimed, at}`: whether a mod-store claim exists for this map. |
| `tagged` | bool | The search tag `mods` is present. |
| `mods` | array | One per `smpmod1.` manifest row: `{key, id, version, state, "placedEntities", placedCount, "references", size}`. `key` is the installed folder ("" when not installed); `state` is `installed`, `superseded`, `unindexed`, `skipped` or `missing`; `placedEntities` are display names; `references` are `[{kind, name}]`, what removing the mod from the map severs: `kind` is `wire` (a wire to one of its placed entities) or `timeline` (a timeline event referencing one); the page ignores other kinds; `size` is carried bytes. |
| `"vanilla"` | bool | The map lists no mods and plays without Snapmap+. |

### Actions

`arg` is a mod's `key` (for a map row that is not installed, its `id`). Each is answered with
`modsActionResult {action, ok, message, data}` and then a fresh `mods` document whose `map` is read
again.

| Action | Meaning |
|---|---|
| `rescan`, `install`, `migrate`, `openFolder` | As in v2. `openFolder` may now carry `arg`, a folder name under `mods\` to open instead of the mods folder; today's host ignores it. |
| `rebuild` | Build the folder again from its files (as `sh_mods build`). |
| `verify` | Rehash that one build's files (today's host maps `verify` to the whole-store check). |
| `mapRemoveModPreview` | Dry run: `data.preview` is the consequence list of removing the mod from the open map: placed entities, and the wires and timeline references to them. Toybox rows are not consequences, so it carries no `editorRows`. Changes nothing. |
| `mapRemoveMod` | Delete the placed entities that use the mod, sever the wires and timeline references to them, drop the manifest row and, with the last row, the `mods` tag. The published copy changes only when the map is published again. |
| `uninstallPreview` | Dry run: `data.preview` for uninstalling: the map part when the open map uses the mod, the Toybox rows it takes out of the editor (`editorRows`, whether or not the map uses it), and the installation part. |
| `uninstall` | `mapRemoveMod` on the open map when it uses the mod, then v2's `remove`: a folder the product made is deleted, the author's own folder moves to `removed-mods\`. Closed maps are not edited; they ask for the mod again when loaded. Replaces `remove`. |

`data.preview` is `{action, key, name, map, install}`, plus `editorRows` (display names) for
`uninstallPreview` only. `map` is null when the open map does not use
the mod, else `{name, entities: [display names], entityCount, references: [{kind, name}],
manifestRow, tagRemoved, vanillaAfter, published}`; `install` is null for `mapRemoveMod`, else
`{folder, disposition: "delete" or "move", movedTo}`. A refused dry run is `ok: false` with the
reason in `message`. Until the backend lane lands, the host answers `rebuild`, `uninstall` and the
map actions with "Unknown mods action."

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
