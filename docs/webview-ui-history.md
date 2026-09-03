# Webview UI -- change history

Newest first. Each dated entry covers one working session's worth of change; the undated **Baseline** entry
at the bottom is the original POC buildout, before this doc tracked dates per entry.

This is an engineering log for maintainers, not the product changelog. The release notes users read are in
[`CHANGELOG.md`](../CHANGELOG.md).

### 2026-09-03 -- The schema's enum value sets, corrected against the engine constants

- **Sixteen enums in `schema_slice.js` listed the wrong values**, which is what issue #76 reported: the
  Entity State tab warned that `SNAP_POI_2` "is not among the known snapPoiElement_t values" on a map
  that was perfectly valid. It listed `TYPE_ONE`, `TYPE_ONE_`, `TYPE_TWO`, `TYPE_TWO_` and so on.
- **Where those names came from.** The generator scraped DOOM's localized inspector strings, whose ids
  read `#str_snapmaps_enum_<enumType>_<LABEL>` and `#str_snapmaps_enum_<enumType>_<LABEL>_desc`. That
  gives the label an editor widget *displays*, not the constant a declaration *serializes*, and it gives
  each label twice -- once bare, once with the trailing underscore left behind by `_desc`. Both halves of
  the corruption pattern noted on 2026-07-15 (`doorState_t`, `walkState_t`, `snapAmmoFilter_t`,
  `snapDropStyle_t`) fall out of that one mistake, and the file's own header described the sets as
  "observed in DOOM's own shipped declaration data", which is how it survived review.
- **Corrected against the reflection constants**, not guessed. Each type's members were read out of the
  engine's constant table as a contiguous declaration-order block, then cross-checked against that type's
  `snappropertyinspector_enum` decl: every one matches its inspector's label order and count once
  `startValue` is applied, which is independent corroboration that the right block was picked. Sentinels
  (`_MAX`, `_INVALID`, `WALKSTATE_NOCLIP`) are included -- they are real members, and omitting one would
  just recreate a false warning on a declaration that carries it.
- **The full set:** `snapPoiElement_t`, `walkState_t`, `coopHazardDamage_t`, `snapDropStyle_t`,
  `snapAmmoFilter_t`, `snapUniversalAmmo_t`, `snapAISpawnBehavior_t`, `playerLoadoutAbility_t`,
  `snapSpawnEncounterTarget_t`, `snapSpawnEncounterLocation_t`, `snapSpawnItemLocation_t`,
  `snapWinCondition_t`, `snapWinDataSource_t`, `snapEndGameState_t`, `snapVisibilityFilter_t`,
  `snapCauseOfDeath_t`. Three of them (`snapEndGameState_t`, `snapVisibilityFilter_t`,
  `snapCauseOfDeath_t`) had no trailing-underscore twin and would have been missed by looking for the
  duplicate pattern alone; they were caught by checking every member in the file against the constant
  table instead.
- **This also fixed the value autocomplete**, which shares `SCHEMA.enums`. Typing a value for one of
  these fields used to suggest `"TYPE_ONE"` -- a token the engine rejects.
- **`tests/decl_enum_values_test.js` guards both halves**: the sixteen corrected sets must match exactly,
  and no enum anywhere in the file may carry a member alongside its trailing-underscore twin, so a
  regeneration through the old path fails the suite instead of shipping.

### 2026-08-24 -- Editable camera origin and position lock restored

- **The footer camera coordinates are editable again.** X, Y, and Z continue to follow the live editor
  camera while their fields are unfocused. Committing any field posts all three coordinates through
  `camSet`, moving the camera through the existing `set_editor_vec3` interface slot.
- **Live coordinates update at the frontend's full cadence.** The host samples the camera on every
  visible 33 ms think-loop frame instead of nesting the read in the 10-frame entity/state poll. The
  existing coordinate comparison still suppresses WebView messages while the camera is stationary.
- **Lock position is restored beside the coordinates.** Enabling it captures the displayed X/Y/Z target
  through `camLock` and reapplies that target on every frontend think-loop frame until the checkbox is
  cleared. The controls remain in the responsive footer introduced on 2026-08-17 rather than returning
  to the tab strip.

### 2026-08-17 -- Prefab transform and dimension parity

- **Saved rotations now use idTech's actual matrix layout.** Each serialized `mat[n]` is one complete
  local axis and becomes one WebGL matrix column. Missing components retain the identity matrix component
  because prefab state is a sparse patch; treating every omission as zero previously produced singular
  matrices, inverted rotations, horizontal props, and flattened geometry.
- **Saved dimensions now inherit their real entity defaults.** Prefab JSON stores only changed scale
  components. A new append-only backend bridge walks the installed entityDef inheritance chain and returns
  the model plus default X/Y/Z scale, then the viewport overlays the saved components. Blocking volumes
  therefore keep untouched dimensions such as the default 16-unit thickness instead of silently becoming
  one unit. Direct-model entities use the same resolver, and valid one-unit slabs are no longer inflated by
  the fallback proxy's old two-unit minimum.
- **Editor helpers match their in-game hierarchy and axes.** Logic hexagons remain full size; I/O circles
  and filter diamonds render at half size. Their procedural fallbacks now occupy the same thin-X / YZ plane
  as the installed editor glyphs. Block and trigger proxies are bottom-anchored like the installed unit
  meshes, even after rotation, while inverse-transpose normal lighting keeps non-uniform volumes readable.

### 2026-08-17 -- Rounded native window and visible Prefab scene

- **The captionless window now keeps DWM's native finish.** Like snapmap-midi, Snapmap+ still creates an
  ordinary managed Windows window and removes only its visible non-client area. Extending one pixel of
  frame into the client before the frame refresh preserves the Windows 11 rounded corners and drop shadow
  without giving up native resize borders, Aero Snap, minimize/maximize, or taskbar-aware maximize sizing.
- **The Prefab scene no longer clears and stops before its first draw.** The grid's disabled normal
  attribute supplied XYZ to `vertexAttrib4f` but omitted W; WebGL rejected that call after clearing the
  background, so neither the grid, decoded models, nor proxy boxes were reached. The call now supplies
  all XYZW components. Draw exceptions also replace the status overlay with an explicit render failure
  instead of silently leaving another blank canvas.

### 2026-08-17 -- Interactive Prefab Details scene preview

- **Selecting a prefab now reconstructs it in a 3D preview above Name.** Saved local
  `spawnPosition`, `spawnOrientation`, and `renderModelInfo.scale` values place each entity in one
  framed scene. Drag orbits, the wheel zooms, and double-click frames the scene again. Neutral lighting,
  an untextured grid, and simple per-class colors keep the result readable without making textures a
  dependency. Unsupported or missing solid geometry remains visible as an honestly simplified box proxy.
- **The detail layout is intentionally quiet.** The preview uses the same `--field` background as Decl
  Text. Orbit controls sit top-right and the only aggregate metadata, `N entity` / `N entities`, sits
  bottom-right. Per-class pills are gone. Name now has its own label, while Name, Description, and Tags
  inherit the exact global field font, size, padding, border, and color rules used by Entity State.
- **Installed geometry is read only when needed.** A direct `renderModelInfo.model`, or the model resolved
  from an entity's inherited definition, is sent to a bounded backend worker. It reads the matching
  BMODEL/MD6 payload from the user's installed resource containers, decodes only positions, normals, and
  indices, and transfers that neutral mesh through a WebView2 shared buffer. Snapmap+ ships, copies, and
  persists no game resource, texture, material, skeleton, or animation data.
- **The scene now distinguishes what the editor data means.** Solid props use their installed meshes;
  blocking volumes choose their visible solid/textured shell instead of `renderModels.item[0]`'s trigger
  shell; invisible triggers are faint outlined volumes and do not zoom the camera away from the physical
  prefab. Logic nodes use DOOM's installed hexagon mesh, action/listener I/O uses its circle, and filters
  use its diamond. Texture-only decals remain thin helper planes. The overlay legend documents those
  roles, orbit controls sit top-right, and the entity count sits bottom-right.
- **Multi-surface props and pickup spawners no longer degrade to cubes.** Cooked BMODEL stores a fixed
  32-byte metadata block after every surface; the initial decoder did not consume it and therefore read
  the first surface's trailer as the second material name. The decoder now handles every surface and was
  checked against the Loadout Station shelves, controls, locker parts, weapons, and editor glyphs.
  Spawner-only decls are also followed through installed `spawnerEntityPair.entityStatic` inheritance to
  the real ammo, armor, health, or equipment model.
- **Rendering is demand-driven.** A `ResizeObserver` watches the preview element itself, so native window
  resizing and shared-divider drags follow the same path. Resize/input/theme/resource changes are
  coalesced through `requestAnimationFrame`; there is no continuous render loop. Drawing-buffer size
  changes only when measured pixels change, device scale is capped, the GPU cache is bounded, and each
  frame has a triangle budget. The square coordinate grid extends farther than the scene and fades under
  a circular radial mask. Browser preview mode uses procedural transform-only stand-ins.

### 2026-08-17 -- Expanded Decl Text becomes a real modal

- **The expanded editor now uses the same modal model as Asset Browser.** The old focus-mode box was
  absolutely painted over the existing split workspace, leaving the original panel and its border visible
  underneath. The live editor column is now moved into a single bordered dialog over the standard dimmed
  backdrop; clicking the backdrop, its Lucide close button, or Escape restores it to Entity State.
- **There is still only one editor.** A temporary DOM anchor restores the exact same node, preserving the
  textarea value, selection, scroll position, undo history, syntax-highlight layer, diagnostics, and event
  listeners. Losing the single-entity inspection state also closes/restores the modal before the normal
  placeholder appears. Toasts render above modal backdrops so Save/Revert feedback remains visible.
- **Diagnostic rows no longer flash a selection highlight on hover.** They remain clickable to select the
  corresponding source token and now communicate that behavior with a pointer cursor only.

### 2026-08-17 -- Shared adjustable section splitters

- **Adjacent sections are now one workspace instead of two bordered cards.** Entities/Entity State,
  Prefabs/Prefab Details, and Timelines/Timeline Editor share a single outer surface with one vertical
  divider. Every section header occupies the same fixed 40px track, so an icon action such as Refresh
  cannot make one header sit lower than its neighbor. Both halves inherit one 10px horizontal content
  inset. The pane surfaces and header fills still meet directly at the vertical rule, while visible header
  and toolbar separators stop at that inset, matching the horizontal-line language inside Entity State.
  Long list rows are clipped with an ellipsis inside the inset instead of painting through it toward the
  divider; an entity row exposes its complete path/name on hover.
- **The divider is adjustable and accessible.** A transparent 13px hit target leaves six clickable
  pixels on either side of the one-pixel rule; hover, keyboard focus, and dragging expand that rule to a
  three-pixel accent without adding layout width.
  Dragging changes the section widths, arrow keys move it in 16px steps, Home/End reach the allowed
  bounds, and double-click restores the CSS default. Only the visible pixel participates in layout, so
  there is no painted gutter; horizontal separators use the intentional pane inset described above.
- **One position carries across all three two-section tabs.** Moving the divider in Entities, Prefabs, or
  Timelines writes one app-level width, so switching tabs keeps the line at the same horizontal position.
  A shared implementation owns pointer capture, keyboard behavior, compact-window minimum relaxation,
  resize clamping, reset, and ARIA values. Assets is intentionally exempt: its tab and modal retain the
  purpose-built three-column Asset Type / Catalog / Inspector layout with normal inter-panel spacing and
  no draggable divider.

### 2026-08-17 -- Viewport-bounded shell at compact window sizes

- **The document no longer scrolls underneath the app chrome.** The HTML root and app shell are clamped
  to the WebView2 client rectangle, the content region may shrink to zero height, and scrolling remains
  inside the individual panes. A document-level horizontal scrollbar can no longer consume the bottom
  of a short viewport or push the footer below it. Expanded Decl Text now uses a client-bounded modal,
  keeping its dialog inside the viewport while the normal header/footer remain stationary beneath the
  dimmed backdrop.
- **The footer reflows instead of defining the window width.** At compact widths, connection/selection
  and Updated/help stay on the first row while the read-only X/Y/Z group becomes a full-width second
  row with three shrinkable columns. Tabs, the two primary panes, and the asset browser also relinquish
  their desktop minimum widths rather than extending the page past the right edge.
- **Native sizing remains the single source of truth.** The HTML edge grips only request Windows' native
  move/size loop; each resulting `WM_SIZE` gives WebView2 the new client bounds, and CSS lays out inside
  those bounds. This is the same arrangement as snapmap-midi, but MIDI enforces a 960x640 native minimum
  while Plus deliberately supports smaller windows and therefore needs the responsive rules above.

### 2026-08-17 -- Camera readout moved into grouped footer

> **Superseded 2026-08-24:** the footer layout remains, but its X/Y/Z values are editable again and the
> Lock position checkbox has been restored. See the entry above.

- **The Camera Lock checkbox and editable X/Y/Z fields are removed from the tab strip.** The live camera
  origin is now a compact read-only footer group labelled `X:`, `Y:`, and `Z:`, with tabular monospace
  values so changing coordinates do not make the group jitter.
- **The footer has three explicit groups:** the connection dot/version plus Selected on the left, camera
  coordinates with a spaced divider beside them, and Updated plus the help button aligned at the right
  edge. The redundant visible `(Connected)` text is gone; the green dot is the sole visual connection
  indicator and retains an accessible label/tooltip.
- **Only the UI was retired.** The native `camSet` and `camLock` messages, one-shot camera write,
  per-frame lock, and `set_editor_vec3` call remain intact and clearly marked as dormant extension points.
  A newly navigated page is forced to receive one camera publication even when the origin has not moved.

### 2026-08-17 -- Flat controls, Lucide actions, and whitespace deselection

- **The conditional Deselect button is gone.** One page-level handler clears both the page and 3D-editor
  entity selection from blank structural space; in Prefabs and Timelines the same handler also clears the
  active list item and its detail/editor state. Buttons, fields, selectable rows, and menus keep their own
  click behavior. Every right-hand inspection/editor pane is protected, including Entity State while Decl
  Text is expanded, so ordinary editing and blank-padding clicks never destroy the inspected selection.
- **Icon-only actions now share the Lucide SVG sprite and one centering contract.** This covers window
  controls, Refresh, Copy ID, combo arrows, focus/restore, feedback, modal close, Timeline add/remove,
  event and prefab-folder removal, Prefab Create/New Folder, Timeline Create, and the asset browser's
  pin/copy/play actions. Create actions expose their full names as hover text. Dynamically rendered
  actions that were clickable spans or divs are real buttons with accessible labels; the old text `?`
  feedback glyph is now Lucide Circle Help.
- **Buttons stay put when pressed.** Active clicks highlight the border without translating the control.
  Primary actions brighten on hover, following snapmap-midi. Expanded Decl Text uses the same rules:
  its temporarily iconized Revert and Save to Decl nodes receive the shared centering class, and the
  primary Save action keeps the brighter hover state.

### 2026-08-17 -- Desktop View menu

- **Theme selection now lives in a left-aligned View menu beside the Snapmap+ brand.** The menu follows
  snapmap-midi's desktop chrome and replaces the top-right segmented Light / Dark switch. Its radio items
  show the current theme, support mouse and keyboard opening, close after a choice or outside click, and do
  not start a native window drag.
- **Theme persistence is unchanged.** Choosing Light Theme or Dark Theme applies immediately and sends the
  registered `theme` setting through `configSet` to the backend-owned `config.json`; standalone browser
  preview mode continues to use `localStorage` only.

### 2026-08-17 -- One browser preview entry point

- **The separate letterboxed phone shell has been removed.** Open `mockup.html` directly for browser UI
  review. It remains fully interactive with fake data when no WebView2 host is present. Production behavior
  is unchanged because the removed shell was never embedded into the DLL or shipped.

### 2026-08-16 -- Bounded Entities rendering and lifecycle performance evidence

- **The Entities pane no longer creates one DOM row per map entity.** The previous list publication
  replaced `innerHTML` with as many as 8,192 rows, restored `scrollTop` against that new tree, queried
  every row, and attached two listeners to each. The filtered entity array and `visibleEids` remain
  complete, but the page now mounts at most 128 fixed-pitch rows between scroll spacers. Filtering,
  Shift range selection, keyboard navigation, selection counts, and Show Hidden still operate on the
  complete logical list. Mouse and context-menu input are delegated once from the stable scroller.
- **The native and page halves now leave correlated performance evidence.** Every actual list
  publication gets a sequence number. `webview_poc.log` records native collection, change-gated
  Timeline rescan, JSON size/build/post time, and the page's matched/mounted row counts plus synchronous
  render/handler time. The complete unchanged 330 ms auto-task is summarized once per 90 calls, broken
  down into entity collection, selection sync, and displayed-state refresh; only a collection over 5 ms
  or a complete poll over 10 ms logs immediately. Editor visible/hidden transitions delimit map
  load/reload intervals, and log timestamps now include milliseconds.
- **Large synchronized selections no longer take quadratic time to order.** Follow Selection still sorts
  entity ids before hashing and publishing them, but now uses `std::sort` instead of insertion-sorting the
  full selection every 330 ms. This does not explain an unselected map-load hitch, but removes a separate
  avoidable high-entity-count path without changing the selection message's order or contents.
- **Rawmap test loads now have a pointer-independent completion signal.** The backend exports a read-only
  count of substituted `DeserializeFromJson` calls that have returned. Diagnostic clients can compare the
  count around a load instead of assuming `idSnapMap` must move; the export does no polling and adds no
  work to an ordinary editor frame.
- **This fixes a deterministic scaling defect, not an unproven diagnosis.** The occasional severe,
  minute-scale frame-drop report is not reliably reproducible. The bounded DOM removes the list's known
  startup hitch, while the event-driven diagnostics establish whether Snapmap+ was busy or idle during
  the next severe occurrence before any broader engine-side change is considered.

### 2026-08-15 -- Letterboxed phone preview (removed 2026-08-17)

- **The short-lived remote preview presented the native desktop canvas instead of reflowing it for a phone.**
  `preview.html` held `mockup.html` at the app's fixed 1440x900 design size and scaled the complete frame
  to the available viewport width. The result remained centered in a black viewport, with equal black
  space above and below rather than stretching to the phone's full height.
- **The shell was preview-only.** It was never embedded into the WebView2 DLL and did not alter the native
  window, the mockup's layout, or production behavior.
- **Pinch zoom remained under browser control.** The shell measured the stable layout viewport and ignored
  visual-viewport resize events while the user was zoomed, so a two-finger expansion enlarged the canvas
  instead of triggering an equal and opposite fit-to-width rescale.

### 2026-08-15 -- Shared Lucide chrome and compact actions

- **Plus now uses the same icon language as MIDI.** A curated, inline Lucide SVG sprite replaces the
  browser-font glyphs in the frameless window controls and the high-frequency compact actions:
  refresh, copy, focus mode, dropdowns, diagnostics, tree disclosure, pinning, timeline add/remove,
  prefab folder controls, and sound audition. The full notice lives beside the source and inside the
  embedded document so the two-DLL installed overlay retains it.
- **The window chrome now matches MIDI's spacing.** Minimize and maximize/restore are 38 px controls
  flush with the right edge. The host reports actual maximize state after resize, Aero Snap, and
  maximize/restore commands, so the square changes to the restore icon reliably. Keyboard activation
  is supported in both the native window and the standalone browser preview.
- **There is still deliberately no close control.** `WM_CLOSE` remains inert because destroying the
  editor UI mid-session cannot currently be reversed without reloading the map. The cleaner chrome
  preserves that product invariant instead of presenting a control that cannot behave safely.

### 2026-08-15 -- Asset browser requests only the catalog being viewed

- **Opening Assets no longer transfers every category.** The previous first open posted 17 catalog
  requests: all 15 categories plus both qualifier lists. The default Materials view now asks for
  Materials and its atlas-only qualifier, plus the independently stored pin list. That keeps the
  other 15 catalog payloads out of the WebView bridge and the page's JavaScript heap until the mapper
  selects their category.
- **Repeated opens cannot duplicate an in-flight request.** Each asset kind is marked pending until
  its `assetList` response arrives; both browser mounts share the guard and a two-category LRU cache.
  This is the smallest cache that can hold each mount's current view. Eviction drops names and folder
  nodes but keeps the scalar rail count. A category switch requests exactly that category, with the
  soundbank qualifier added only for Sounds.
- **Image rows now produce image previews.** The UI already identified Images as previewable, but the
  native producer accepted only material names. The request now carries the selected asset kind, so
  direct image names enter the existing bounded BC1/BC3/BC7 decode and generation-safe publish path
  without loading VMTR or losing to a same-named Material record.
- **The native catalog now follows the same demand boundary.** The base resource indexes are reduced
  to interned recognized-name/offset metadata after parsing, and irrelevant record classes from the
  broader base-game index are dropped. Wwise event/bank metadata is loaded only for Sounds by
  streaming the relevant tags; the decl-less `.vmtr` union is loaded only for Materials. Mega2
  resolves selected entries directly from disk instead of retaining complete shard tables. Preview
  bytes remain in the installed game files until a row is selected.
- **Caches release what the view no longer owns.** Finished PNG transport is consume-on-read, hidden
  mounts remove their data URI and cancel in-flight generations, late responses are checked against
  the current name, the worker sleeps on an event, and atlas decode scratch is released after idle.

### 2026-08-10 -- Asset browser: Pinned, soundbanks, four more categories, and the duplicate rows

- **Pinned**, a per-user shortlist at the top of the type rail. A star in the left gutter of every
  asset row; one shared list across all types, since the handful of things a mapper is holding at
  once is rarely all of one kind. Stored in its own `%LOCALAPPDATA%\snapmap-plus\pinned.json` rather
  than in the settings config -- a malformed pin list must not be able to reset somebody's theme.
  The list renders **flat**, with full names: everywhere else the folder tree earns its keep against
  thousands of names, but making the mapper drill to reach their own shortlist is asking them to
  navigate their answer to navigating.
- **This forced a list-vs-selection split** that is worth knowing about before touching this code.
  Every other list is one type, so `m.type` answered both "which names do I show" and "what kind of
  thing is selected". Pinned breaks that -- a pinned row can be any type regardless of which rail
  entry is lit. `abListType` keeps the list question; `abSelType` answers the selection question from
  the row itself, and the call sites that decide carriers, previewability and Apply now ask it.
- **Sounds are filed by soundbank.** Their own names carry almost no structure, so the catalog was
  one root folder of ~8,000 rows; the Wwise `<SoundBank>` grouping gives 24 sensibly-sized folders.
  The tree builder now separates *where a name sits* from *what it is called*: `abBuildTree` takes a
  `place` function for the folder path while the leaf keeps the REAL asset name, so a synthetic
  `doom_snapmaps/` prefix can never leak into Copy, selection, preview or Apply. This replaced an
  earlier bank **filter** dropdown -- two controls doing one job is how a browser gets confusing.
- **Duplicate sound rows removed, twice over.** 449 path-form `sound` decls are wrappers around a
  Wwise event already in the catalog (`scripted_events/cyberdemon/head_splat_01` and
  `Play_head_splat_01` are one sound); the first pass caught only the ones whose twin was a bare
  event, missing the 129 whose twin was another flat decl.
- **Duplicate rows removed catalog-wide.** Decal atlases listed everything twice -- 1,673 records
  for 1,024 distinct names -- because the game index is a record-per-blob table, not a catalog of
  distinct assets. Clicking one row selected both and starring one starred both, since the UI keys
  off the name. Decal atlases now reads 1,024, Images 3,422.
- **Four more categories.** **Lights** (89) applies the light *material* as `lightMaterial`, with
  Point light / Spotlight as the Create-as choice. **Models** gained 108 breakable/gib models that a
  `model`-only catalog could not see. **Perks** (190) and **SWF / Flash** (193) are reference-only,
  listed under Reference rather than Placeable so no Apply button pretends to work; SWFs are listed
  in the `swf/x.swf` form decls actually reference, not the baked `generated/swf/x.bswf` on disk.
- **"No ramp" now removes the ramp** instead of declining to write one, which on an entity that
  already carried a `ramp1` left the previous mapper's tiling in place -- the option did nothing on
  exactly the entities somebody picks it for. `renderParms` is an indexed array, so removing means
  cutting the entry, renumbering the survivors and fixing `num`.
- **The catalog Refresh button is gone.** It promised the list could be stale and made fresh, and
  neither half was true.
- Browser-preview stand-in gained a real sound + soundbank sample taken from the shipped manifest, so
  the bank tree and search can be exercised without launching the game.

### 2026-08-05 -- The Assets tab: the whole shipped catalog, previewed and placeable

- **New Assets tab**, mounted twice from one implementation: as an open-browsing tab, and as a modal
  opened from the Entities tab ("Browse assets") scoped to the current selection. Both share the
  fetched catalog and folder tree; each keeps its own type / folder / filter / selection state.
- **Materials preview as real pixels.** The backend locates the material's pages in the shipped
  megatexture set and decodes them with DOOM's own decoder in-process. Materials with no atlas rect
  (roughly half the catalog) fall back to decoding BC1/BC3/BC7 out of the `.index` containers.
- **Sounds audition** with real play/stop, through the editor's own preview path rather than the
  `testSound` console command -- which throws its emitter handle away, so it can neither be stopped
  nor prevented from stacking a new voice on every click. The preview session is scoped to **window
  focus**: background audio is only wanted while DOOM lacks focus, so blurring the Snapmap+ window
  ends the session and hands the editor its audio back.
- **The sound list is the union of two sources.** A `sound` decl and a Wwise event are different
  sets and neither contains the other; listing only decls was missing ~2,600 names, including the
  generic SnapMap VO. Deduplicated case-insensitively, since the manifest spells events `Play_Vo_...`
  and decls are lowercase.
- **Three new categories** -- Modules, Brush models, Clip models -- out of the two decl types the
  catalog previously discarded. A **module** places as one entity that is both visible and solid:
  the baked geometry goes to `renderModelInfo.model` and its paired collision to
  `clipModelInfo.clipModelName`, which the browser derives for you (232/232 pair, no orphans).
  The inherited `CLIPMODEL_AUTO` is left in place -- naming a clip model overrides the automatic
  derivation on its own.
- **Apply to selection** commits into the selected entity's decl immediately rather than staging an
  edit. One entity at a time, since it patches the decl the editor has open; with 2+ selected the
  button is disabled and the "Browse assets" entry point is too. What may be applied is decided per
  target class by a single gate: nothing applies to the player start, FX/particles/sounds are refused
  on doors and interactables, models may still be swapped on most interactables.
- **New entity** authors a one-entity prefab and stages it through the engine's own paste path, so it
  arrives held and ready to place. `grabDistance` (not `spawnPosition`) carries the placement
  distance -- the two ADD, and only `grabDistance` follows the full view ray.
- **Keyboard navigation** throughout: Up/Down move the row selection, Enter/Space open folders,
  Backspace goes up a level. Placement is deliberately *not* bound to Enter/Space -- you have to tab
  to the button -- so browsing can never place something by accident. The modal takes precedence over
  the tab when both are open.
- Tab order is alphabetical, with Assets first; the tab that opens on load is still Entities.

### 2026-07-27 -- Selection changes refused while the editor is holding something

- **Selecting from the Entities list is now refused while you are grabbing an entity or holding a
  staged prefab**, with a toast: *"Place or cancel what you are holding in the 3D view first, then
  select"*. The list highlight rolls back so the UI never claims a selection the editor didn't take.
- **Why.** The engine captures a snapshot of the selection when a manipulation starts, and its
  cancel path (Escape) restores that snapshot **indexed positionally against the live selection
  array**, with no re-validation. Change the selection in between and Escape writes each saved record
  onto the wrong entity -- swapping entity pointers inside the live map. Observed live: duplicated
  entities, entities vanishing from the map entirely, "(no module)", and hard freezes. The capture
  also stashes each entity's real layer/module index, which is why the module association is what
  visibly breaks.
- **This is a pre-existing engine bug, not a regression.** It reproduces on the v0.2.1-beta.2
  release, which has none of the selection-state work. Only *cancellation* triggers it -- accept
  paths (mouse click, controller accept, space) are unaffected.
- **Detection** is the capture's own side effect: it moves the manipulated entities onto the editor's
  scratch layer, so "any selected entity is on the scratch layer" is a precise test for "a snapshot is
  outstanding". Enforced in the backend across `add_to_selection`, `clear_selection` and
  `remove_from_selection`, and fails closed -- an unreadable editor is treated as in-progress.
  Exposed to frontends as vtable ext 11 (`manipulation_in_progress`, +0x2C0) so the UI can explain the
  refusal rather than silently doing nothing.
- Deliberately NOT keyed off the "currently held" indicator: placing a **new** entity from the palette
  sets that indicator but captures no snapshot, and is provably safe (Escape behaves correctly there).
  Keying off it would have blocked a state that never needed blocking.
- Unaffected: native click select/deselect, Create-from-selection's own "hover one of the selected
  entities" requirement, and selecting *before* grabbing (the list-assembled group grab).
- Known cosmetic issue, not addressed: while an entity is grabbed, the other selected entities render
  in the move/grab colour rather than the normal selection colour.

### 2026-07-27 -- Native 3D-viewport deselect fixed at the root

- **A selection pushed from the Entities list now behaves exactly like a native one.** Empty-space click
  deselects it, Delete deletes all of it, Move works, and every bottom-bar control applies -- for single
  and multi-entity selections, and when switching between entities. Previously only the explicit
  **Deselect** button worked, and Delete/Move misbehaved (Move could soft-lock the game).
- **Root cause** (from our own reverse-engineering):
  the editor keeps a per-mode object inline in the editor object, and its state field says whether
  anything is selected. The native click handler sets that field on a successful hit *and* adds to the
  selection array; on an empty-space miss it deliberately does nothing at all. So an empty click only
  deselects because the editor is in the "something is selected" state. `add_to_selection` wrote the
  array only, leaving the editor believing nothing was selected -- so the miss path correctly did
  nothing. That one inconsistency explains every symptom, including Delete and Move, which were never
  separate bugs. It also explains the old workaround (clicking one of the already-selected entities in
  the 3D view runs the native hit path, which sets the state regardless of how the selection was made).
- **Fix:** `src/backend/iface_engine.c` now syncs that mode state alongside every selection-array write --
  `add_to_selection` -> "selected", `clear_selection` -> "idle", and `remove_from_selection` -> "idle"
  only once the selection is empty (so a full Delete can't leave the inverse inconsistency). All writes
  are SEH-guarded and gated on the editor already being in EntityMode, matching the file's existing
  conventions. The offsets and the per-build re-derive recipe are documented at the constant block.
- **List sync:** deselecting natively in the 3D view now clears the Entities-list highlight too, in
  "Select in 3D" mode. The backend already broadcast the live editor selection count on every change;
  the UI simply wasn't acting on it reaching zero. Gated to that mode only ("Follow selection" already
  mirrors the whole editor selection, and with both off the list selection is local), to a real
  greater-than-zero-to-zero transition, and to the case where the list actually has a highlight.
- The **Deselect** button was kept as a convenience in this 2026-07-27 change. It was removed on
  2026-08-17 when blank-space deselection replaced it outside the protected Entity State pane, while
  retaining the same native `clear_selection` path.
- **The mode-state write is guarded to idle/selected only.** That field is not a two-value flag: the
  engine drives it to other values while a manipulation is in flight (grabbing an entity, holding a
  staged prefab) and for sub-screens and the logic sub-mode, and it has its own "is the mode busy"
  predicate. An unconditional write tore the editor out of the gesture mid-manipulation so its
  completion bookkeeping never ran -- caught in testing as the held object being dropped and
  permanently losing its module association. `mode_set_selection_state()` now reads the current value
  and only ever moves between idle and selected. Consequence, accepted deliberately: in a mode outside
  that pair with nothing selected, the sync sits out and that mode keeps the old (pre-fix) behavior --
  strictly better than risking editor state.
- Side effect of the guard, and a nice one: because a manipulation is no longer interrupted, you can
  hold a grabbed entity or a staged prefab, push a selection from the Entities list, place the held
  object, and still have your selection. Minor known inconsistency, not considered a problem: after
  placing, a prefab leaves the pushed selection highlighted while a pre-existing-entity grab clears it.
- **New capability that falls out of this: list-assembled group grab.** With a selection pushed from
  the Entities list, grabbing any one of the selected entities in the 3D view grabs *all* of them.
  There was no way to do this before. It makes a practical workflow possible: browse a logic chain in
  the 3D view, find the other entities you want to bring along in the Entities list, include them in
  the selection, then grab the logic-chain node you are on and move the whole group together.
  Confirmed working in logic chain mode, which also means that mode is inside the idle/selected pair
  the guard permits -- the earlier worry that the logic sub-mode might sit outside it did not
  materialise.
- Known remaining gap: if you have one entity selected from the list and then natively click a
  *different* one, the selection count is unchanged, so no broadcast fires and the list keeps
  highlighting the original. Fixing that means broadcasting selection identity, not just count, in
  "Select in 3D" mode.

### 2026-07-27 -- Keyboard paging for every list and dropdown; built-in filter entities hidden

- **ArrowUp/ArrowDown now page the Entities, Timelines and Prefabs lists.** Moving the highlight
  selects the row, exactly as clicking it does (Timelines also opens it; Prefabs also loads its
  detail card), and the row is scrolled into view. With no current selection, ArrowDown lands on the
  first row and ArrowUp on the last.
  The Entities and Prefabs handlers are scoped to the **document**, not to their filter box: "Follow
  selection" mode drives the Entities list purely from the live 3D-editor pick, so the user may never
  click into the filter box or a row at all, and a listener scoped to one input would never fire.
  Excluded from both: the Entity State editor (`#editor` -- its decl textarea needs real caret
  movement), the Prefab Details card (`#prefabCard` -- same, for the description textarea), an
  in-progress folder rename, and any `.combo` input (those have their own arrow handling, below).
- **The Inherit / Classname combos and the Timelines "Runs on" entity picker page their dropdowns.**
  Arrow keys move a highlighted option, Enter accepts it, and -- unlike the lists -- this only applies
  **while the dropdown is open**, so arrows are free for normal use otherwise. Typing still filters;
  paging a narrowed list re-renders against the current text rather than snapping back to the full
  set. The highlight reuses `.combo-opt`'s existing hover styling via a new `.active` class.
- **SnapMap's own built-in filter/droppable helper entities are excluded from every entity list and
  picker** (`renderList`, the "Runs on" picker, and the per-event entity-arg dropdown). These are the
  engine-seeded prefilters and droppable slots (`any player` / `any ai`, per-team/player/race,
  keycards, flags, power cores) -- dev-layer-only, never placed or edited by a mapper, and previously
  they buried the real entities whenever Show Hidden was on.
  Matched by the id **starting with** `snapmaps/filter/`, not containing it: a mapper's own filter
  always carries its map/module path first (e.g. `0_c_ind_cross/snapmaps/filter/ai_151`), so
  user-placed filters are unaffected. The `" (no module)"` suffix is deliberately NOT used as the
  signal -- it also appears on any real entity not yet assigned to a module.

### 2026-07-23 -- Persistent Entities controls

- **Show Hidden and the Entity selection direction now persist through the backend-owned configuration
  service.** Page startup requests `entities.show_hidden` and `entities.selection_mode`, aggregates both
  responses before applying them, and writes later changes through the existing generic config messages.
- **Selection direction is a single exclusive mode.** `off`, `follow`, and `select_in_3d` represent the
  entire state, so Follow Selection and Select in 3D cannot be enabled together. Restoring
  `select_in_3d` deliberately does not push an empty list selection to the editor; persistence failures
  keep the changed session behavior and show a warning.

### 2026-07-22 -- Persistent settings and flash-free theme startup

- **Theme choice now persists through the backend-owned configuration service.** The Light / Dark toggle
  sends generic `configSet` JSON over the matched-pair interface (`config_get_json` `+0x2B0`,
  `config_set_json` `+0x2B8`) to `%LOCALAPPDATA%\snapmap-plus\config.json`; startup reads the same
  registered `theme` value. In-game WebView storage is no longer involved — `localStorage` remains only
  as a convenience for standalone `PREVIEW` mode.
- **Dark startup is seeded before navigation.** The host patches the embedded document root from the
  saved value before `NavigateToString`, keeps the native window hidden until a successful
  `NavigationCompleted`, and then allows the normal editor-ready gate to show it. A returning dark-theme
  user gets a dark first frame instead of a light/blank flash.
- **Failure modes are visible but non-blocking.** The page keeps an unsaved theme for the session and
  warns on persistence failure; startup reports corrupt-file recovery, a deliberately untouched newer
  schema, or volatile defaults. Config messages are decoded through a bounded raw-buffer path before
  UTF-8 conversion. Focused native tests cover the JSON service, pinned ABI, message bounds, HTML bridge
  contract, and pre-navigation theme seeding.

### 2026-07-18 -- Status bar: drop the redundant entity count

- **Removed `Entities: N (hidden)` from the status bar.** The count (and hidden-count note) duplicated
  what the Entities tab's own panel-header badge already shows live, now that panel titles carry that
  context -- kept `Selected: N` only. Dropped `entCount`/`hiddenNote` from the markup and their
  assignment in `renderList()`.

### 2026-07-18 -- Crash-report dialog

- **Crash reporting**, the feedback pipeline's second producer. When the game hits a serious fault, the
  backend fault machinery writes a small JSON **crash record** (fault class, code, `module+0xRVA`, call
  stack, the engine's own error text when there is one, timestamp, version) to `<game>\snapmap-plus\crash\`
  with crash-safe file writes. The host think-loop polls that folder (~2 s, gated on the page being
  loaded) and posts `crashPending {record, count}`; the page auto-opens a **branded** crash dialog (the
  menubar logo + wordmark) -- in-session seconds after a *survived* fault, on the next launch for one
  that killed the game ("Snapmap+ crashed last session"). It shows the error + collapsed call stack,
  takes an optional description + contact, and offers an **Attach recent logs** checkbox (default on):
  the log tails are **anonymized** host-side (account / profile / machine names scrubbed to
  `<user>`/`<machine>` -- pure `report_scrub.h`, unit-tested) before they leave the box. Send composes
  the payload host-side (`crashSubmit` -> `category:"crash"`) and rides the SAME WinHTTP thread +
  `reportResult` plumbing as the feedback dialog; Dismiss (or a successful send) posts `crashDismiss`,
  which clears the pending record so it never nags twice (the full logs + any local crash dump stay on
  disk). The relay files it as a `[Crash]` issue whose auto-title embeds the crash location, so the
  signature dedup groups repeats onto one issue, each occurrence's logs a collapsed follow-up comment.
  Full pipeline: [`feedback.md`](feedback.md).

### 2026-07-18 -- Send-feedback dialog ("?"), and the focus-mode bottom-padding fix

- **In-app feedback.** A compact "?" circle sits at the statusbar's right edge (the footer counterpart
  of the menubar logo); it opens a Send-feedback modal (category / title / details / optional contact,
  plus a hidden honeypot field).
  Send posts `reportSubmit` with the whole report as one opaque JSON payload (the page composes it,
  including the installed version from the `list` message); the host ships it to the feedback relay on
  a short-lived WinHTTP thread (the think loop + WebView2 callbacks share one STA thread -- a
  synchronous POST there would freeze the UI) and the think loop relays `reportResult {ok, mode,
  number}` back. Green toast on success ("matched a known report" when the relay deduped it into an
  existing issue's comments), red toast on failure -- with the dialog left open so nothing typed is
  lost. One submit in flight at a time (button disabled + a host-side guard). Preview mode fakes the
  round-trip, including the dedup path on an identical re-send. Full pipeline: [`feedback.md`](feedback.md).
- **Focus-mode bottom padding fixed.** The expanded Decl Text editor (`.editor-col.focus-mode`,
  `position:fixed; inset:14px`) inherited the base `.editor-col`'s `height:100%`, and an explicit
  height wins over the bottom inset -- so the panel's bottom edge (border + 12px padding) sat 14px
  below the viewport and looked like missing bottom padding. `height:auto` in the focus-mode rule lets
  both insets govern; the panel now shows the same 14px gap + 12px padding on all four sides.

### 2026-07-17 -- Prefab selection gating + metadata sidecars, decl-editor line numbers, placeholder layout fix

- **Prefab Delete / Load / Place are gated on a real selection.** Both buttons start disabled ("Select a
  prefab first") and enable only when a selected prefab's details actually load; clicking blank space in
  the prefab list clears the selection (the detail card empties, the buttons grey out). Selection state
  also survives the tree's innerHTML re-renders now: the selected row is re-marked if the file still
  exists (even when the filter hides it), and the whole selection is dropped if it was deleted/renamed/
  moved -- the buttons can never act on a ghost file.
- **Prefab Description + Tags are real** (previously disabled stubs). Stored in a per-prefab
  `<name>.meta.json` **sidecar** next to the prefab file -- the prefab `.json` itself stays byte-exact
  engine JSON (it IS the staged paste payload), so metadata never goes inside it, and there is no central
  manifest to desync (the directory stays the source of truth). Saved automatically on leaving a field,
  only when changed; clearing both fields deletes the sidecar; delete/rename/move-to-folder all carry the
  sidecar along natively; the list enumeration skips `*.meta.json` so a sidecar never shows up as a
  prefab. The **filter box now matches tags as well as names** (cross-folder search -- the point of tags
  while prefabs live only on this machine). The tag map rides the `prefabs` list message, and sidecar
  bytes are re-escaped fresh rather than spliced raw -- a hand-edited/malformed sidecar can only lose its
  own tags, never invalidate the whole message (the missing-quote lesson from 2026-07-07).
- **IDE-style Tab in the Decl Text editor** (both layouts): Tab was falling through to the browser's
  default focus-move (it highlighted the next control). Now, with the completion popup closed, Tab
  inserts a real `\t` (decl text is tab-indented), a multi-line selection indents the whole block, and
  Shift+Tab outdents (one leading tab or up to 4 leading spaces). Edits go through
  `document.execCommand('insertText')` so they stay on the textarea's native undo stack and fire the
  normal `input` plumbing. With the popup open, Tab still accepts the completion, as before.
- **Decl Text editor line numbers**, in both the default and focus-mode layouts: a third gutter layer in
  the `.code-editor` stack with the same font metrics as the `pre`/`textarea`, so its rows align 1:1.
  The gutter has no scrollbar of its own -- `refreshDeclScroll()` translates the inner column by
  `-scrollTop` (scroll stays cheap; the line-number column is rebuilt only when the line COUNT changes).
  The gutter widens as the count gains digits, and `caretXY()` (the autocomplete-popup anchor) accounts
  for the shifted text origin.
- **Prefab empty state re-centered.** `#pcEmpty` carries the same flex-centering `.placeholder` class as
  the Entities tab's empty state, but every code path that re-showed it set an inline `display:block`,
  overriding the class and dropping the vertical centering after the first hide/show cycle. The show
  sites now restore `flex`.
- **Multi-select placeholder layout fix** (user-reported, from a screenshot): `#selInfo` is a flex
  CENTERING container, so the placeholder's text + `<br>` + span were being laid out as separate ROW
  flex items sitting side-by-side. `showPlaceholder()` now wraps its content in one block so multi-line
  placeholders stack as normal lines.

### 2026-07-17 -- Decl Text focus mode, dropdown sizing, and a batch of small editor fixes

A round of small, user-reported polish items on the Entities tab's decl editor, found and fixed one at a
time during live testing.

- **Decl Text focus mode.** A new toggle button next to the problem count expands the Decl Text editor into
  a distraction-free view: the ID/Inherit/Classname/Displayname fields and the description panel hide, the
  editor column takes over the window (pure CSS -- `position: fixed` on the existing `.editor-col`, no DOM
  nodes moved, so the textarea's scroll position/selection/undo history/listeners are untouched), and Save/
  Revert/the "unsaved changes" note are reparented into the same row as the toggle button for the duration
  (they normally live in `.panel-head`, which the fixed overlay covers). Escape or the button again exits.
  Went through several rounds of live visual feedback: a first pass with a separate floating pill in the
  corner and custom CSS `::before`-injected glyphs looked bad and was scrapped for something simpler --
  Revert/Save now just borrow the `.btn.icon` class the toggle button already uses (identical shape/height,
  nothing new to keep in sync) and swap their real `textContent` to a glyph while focused (the same pattern
  the toggle button itself already used), restored to their normal word on exit. Two bugs found + fixed in
  the process: the row's `align-items: baseline` shifted the buttons up/down depending on whether the
  (sometimes-empty) "unsaved changes" hint had text -- switched to `align-items: center`, stable regardless
  of content; and the first icon choice for Revert (a counterclockwise circular arrow) read too close to the
  Entities panel's clockwise refresh icon at icon size, swapped for a visually distinct hooked back-arrow.
- **Selected text was invisible in the Decl Text editor.** The editor overlays a transparent `<textarea>`
  over a token-colored `<pre>` (so syntax highlighting shows through); the selection style set the
  highlighted text's color to `transparent` too, so highlighting a range of text hid it under the selection
  background instead of showing it. Now uses the theme's `--selText` color (already defined for exactly this
  purpose, just never wired to the textarea's `::selection`).
- **Dropped the schema-status note** ("schema: `<class>` - N fields" / "not in the N-class schema") above
  the Decl Text editor -- not something users need day to day. The problem/warning count next to it (and the
  schema check that feeds it) is untouched; only the note itself is gone.
- **Classname/Inherit dropdowns were too small.** Long decl-class slugs (e.g.
  `snapmaps/editor_only/placeholder_target`) forced a horizontal scrollbar, and the list only showed a few
  options vertically. `#cInherit`/`#cClassname .combo-list` (not the base `.combo-list`, so every other
  dropdown in the app is unaffected) now gets `max-width: min(80vw, 820px)` + `max-height: 440px`, with
  `overflow-x: hidden` as a backstop against any stray horizontal scrollbar.
- **`doorState_t` enum values were wrong**, not just stale -- missing the `DOOR_STATE_` prefix entirely,
  missing two real members (`LOCKED_SECRET`, `NO_DOOR`), and padded with garbage duplicate entries. Hand-
  patched in `schema_slice.js` against the real engine enum (`DOOR_STATE_UNLOCKED/LOCKED/OPEN/LOCKED_SECRET/
  NO_DOOR`, confirmed from a live decompile). The same corruption pattern (missing prefixes / dropped members
  / garbage duplicates) was spotted in several other generated enums during this pass (`walkState_t`,
  `snapAmmoFilter_t`, `snapDropStyle_t`, and more) -- `schema_slice.js` is a static generated artifact with
  no in-repo generator to regenerate from, so those are left as-is for now and will get the same
  ground-truth-verified treatment individually as they're found, rather than guessed at in bulk.

### 2026-07-14 -- Timelines EXPLAIN box, entity-arg picker, live Runs-on selection, Clear stack 0, and stale-render fixes

A polish pass over the Timelines and Entities tabs, built on top of the SnapStack port below.

- **Entity-typed event args get a real picker.** An `entity`-typed arg (e.g. `ai/set_target`'s `target`)
  used to fall into the generic plain-text bucket -- the one gap left in the "every arg kind gets a real
  widget" work from 2026-07-08. It now renders the same combo dropdown as the "Runs on" picker, sourced
  live from `allEntities` (`{display:"<id>: <name>", value:<id>}` pairs, same split the per-entity asset
  dropdowns already use for display-name-vs-stored-value). No new native round-trip -- it reuses the
  existing `.ev-arg-combo`/`tlRenderArgCombo` machinery decl/enum/asset args already share.
- **Timeline EXPLAIN box.** Each event row gets a collapsed-by-default "What does this event do?" toggle
  showing `sh_event_docs.h`'s author-facing summary + a per-arg description line. Shipped as a new
  `enumEventDocs` command
  (`poc_send_event_docs`, same "static generated table, once per session" pattern as the event catalog),
  cached client-side as `eventDocsByName`. Three UX options were mocked up and compared -- an
  always-visible block, this collapsed toggle (reusing the Entities tab's existing `.desc-panel` pattern
  verbatim, same open/close/text-selection-guard behavior), and an arg-focus variant (a slim one-line
  summary plus a hint that follows the currently-focused arg input). The collapsed toggle shipped as the
  lower-risk, already-proven pattern; the arg-focus variant is on record as a possible future revisit.
- **"Use current selection" wired.** The Runs-on picker's button now posts a fresh on-demand read of the
  live SnapMap editor selection (not the periodic poll's cached count, since the click should reflect
  whatever's selected *right now*) and requires exactly one entity, toasting a warning naming the actual
  count (0 or N>1) otherwise.
- **Two stale-render bugs found while testing the above, both fixed:**
  - Re-clicking an already-selected entity (Entities tab) or an already-open timeline (Timelines tab) used
    to silently discard any unsaved edit -- both list-click handlers unconditionally re-fetched and
    re-applied fresh state, even when nothing had actually changed. Now a no-op when the click resolves to
    what's already loaded; a genuinely different entity/timeline still loads normally. The Entities-tab fix
    needed a new `loadedEid` (distinct from `primaryEid`, which the click handler reassigns *before* the
    guard runs, so it could never tell "new" from "same" on its own).
  - The Classname field could flash a false "invalid" red outline (or leave a real one uncorrected) on
    selecting an entity -- `showState()` validates the just-loaded classname against `classItems`
    synchronously, but `classItems` is scoped per-inherit and re-fetched *after* that validation already
    ran, so it was checking against the *previous* entity's class list. Fixed by re-running the validity
    check once the fresh `enumClasses` response actually lands.
- **Clear stack 0.** A new context-menu entry next to "Push to stack 0", for a user working purely from the
  UI who never touches the DOOM console (`sh cstk 0`). New `+0x2A8` `clear_stack` vtable slot, the
  out-of-process counterpart to `push_to_stack` -- see [`backend-changes.md`](backend-changes.md).
- **Toast wording cleanup.** Dropped "(dedup)" from the push-to-stack toast and "for Runs on" from the
  selection-count warning -- both were internal/positional details a UI-only user has no context for.

Confirmed live: entity-arg dropdown, EXPLAIN box open/close and content (including the no-doc and
no-per-arg-doc fallback cases), Runs-on live-selection single/multi-select warnings, both stale-render
fixes (edit survives a re-click; a genuinely different selection still loads), and the push/clear stack
round trip.

### 2026-07-13 -- SnapStack ported to the backend: WebView gets the full `sh` command set

The SnapStack stores + all 20 `sh` handlers moved into the shared backend (`src/backend/snapstack.c` +
`json_patch.c`), so `sh psel`/`sh acctargets`/`sh bss`/… now work from the Studio window's build. "Push to
stack 0" (previously a stub) is wired to the backend stores via a new `+0x2A0` `push_to_stack` vtable slot.

The port surfaced a real backend bug that had broken *every* decl-edit op driven from the UI: `json_patch`
produced invalid JSON when an entity's `state.edit` serialized as `null`/`{}` (an entity with no explicit
overrides — everything shown on it is inherited), which the engine's lexer rejected (`applied 0/1`).
Fixed in `json_patch.c`; see
[`backend-changes.md`](backend-changes.md) for the full root-cause. Also added backend-exclusive
`chkstk`/`chkgrp`/`clrgrp` store-management commands and confirm toasts on the previously-silent ops.

Verified in-game: 19 of the 20 ops (all but `mkcmd`, ported-but-unverified) plus the new commands.
Command reference: [`capabilities.md`](capabilities.md#snapstack-ops).

### 2026-07-13 -- Timelines: palette-inherit normalize, inline Save Timeline, and the fresh-save `typeof null` bug

A focused session on Timeline handling. Three real fixes, found by
methodical in-game testing plus, for the last one, a step-by-step
JS→native chain trace. All verified in-game on both **palette-placed and reclassed** Timelines, including
play/save/reload persistence.

- **Palette-Timeline portable-inherit normalize now works** (it didn't before). A Timeline
  placed from the in-game palette carries the non-portable `snapmaps/editor_only/placeholder_target`
  inherit; it is now rewritten to `snapmaps/unknown` via a **shared
  backend slot (`+0x298`)** (see
  [`backend-changes.md`](backend-changes.md)), called from the Timeline rescan. Porting it
  surfaced a latent **decl-source-blob-lag** bug (the Inherit box and the saved map kept showing the
  placeholder even after the raw field updated) — root-caused and fixed backend-side; see that doc.
- **Save Timeline commits inline (`+0x290`)** — no more crash on the next play/save/reload.
  Save Timeline had been using the deferred `+0xd0` schedule, the exact deferred-apply
  double-free fixed backend-side on 2026-07-12; migrated to `poc_apply_sync_seh`.
- **Fresh Timelines now save immediately, with no copy/paste or map save+reload first.**
  This had been mis-documented as a pre-existing
  *engine* limitation. It was actually a **JavaScript `typeof null === 'object'` bug** in
  `tlBuildPatchedEntityJson`: a freshly-placed/reclassed Timeline serializes as `edit = NULL;`
  (`"state":{"edit":null}`), and the `typeof x !== 'object'` guard let the `null` through; the next line
  (`edit[compKey] = …`) then threw *uncaught*, silently aborting the entire Save — no toast, nothing
  reaching the backend. Fixed with explicit `=== null` checks. A JS→native `diag` tracer (since removed)
  pinpointed it: the trace showed the save entering the correct branch and the re-serialize succeeding, but
  the line immediately after `tlBuildPatchedEntityJson` never executing — an uncaught throw inside it.
- The Entity-State panel also gained a **per-field dirty exception** so an authoritative external inherit
  correction (from the normalize above) can land on the untouched Inherit box even mid-edit of another
  field, instead of the whole-panel dirty guard freezing stale placeholder text.

> **Architectural follow-up (since done):** at the time, each decl-edit call site independently picked
> inline vs deferred — a footgun that caused the Save Timeline regression above (one reverted line silently
> dropped it back onto the crash path). The durable fix — SnapStack as shared backend handlers with one
> commit path — landed the same day; see the SnapStack entry above and
> [`backend-changes.md`](backend-changes.md).

### 2026-07-12 -- Contributor follow-ups: path-safety gate, pinned WebView2 SDK, malloc null-check

A contributor reviewing the WebView frontend PR flagged three low-risk, non-blocking items (plus two
more covered in [`backend-changes.md`](backend-changes.md)):

- **Path-safety gate.** `resolve_prefab_path` (+0xc0, backend) is a plain string concat with no
  rejection of its own -- a prefab/folder name containing `..` or a path separator would resolve
  outside the `prefabs\` tree before ever reaching `fopen`/`DeleteFileA`/`MoveFileA`/`CreateDirectoryA`/
  `RemoveDirectoryA`. Only the JS side guarded this before. Added a native `poc_valid_name()` check
  (rejects `..`, `/`, `\`, `:`, empty, and >200 chars) in `snapmap_plus_ui_webview.cpp`, wired into the two
  choke-point helpers every prefab/folder file op already funnels through (`poc_prefab_dir`,
  `poc_prefab_file_path`) plus the one direct caller (`poc_apply_create_prefab`).
- **Pinned the WebView2 SDK version.** `build-webview.ps1` fetched NuGet's `index.json` and took
  `$idx.versions[-1]` -- literally whatever NuGet listed last, prerelease or not. This wasn't
  theoretical: the SDK actually cached on disk from an earlier run was `1.0.4071-prerelease`. Replaced
  with a hardcoded `$wvPinnedVersion = "1.0.4078.44"` (the newest *stable* release at the time), to be
  bumped deliberately going forward -- needed before wiring the webview build into CI.
- **Null-checked the entity-list malloc.** `g_ents`/`g_tls` (`sh_ui_init`) were `malloc`'d with no
  null-check before use. In practice a null `g_ents` was already non-fatal -- the one write site
  (`poc_collect`) sits inside a `__try`/`__except`, so a null-deref got silently caught as a fault and
  returned an empty list -- but that's an accidental safety net, not an intentional one, and it doesn't
  cover every read site. Now `sh_ui_init` checks both allocations explicitly, logs, and aborts init
  cleanly on failure instead of relying on SEH to paper over it.

Also fixed as part of the same follow-up round: the "deferred applies run on the main-thread execution
point" doc wording above (now reads correctly) and a `docs/backend-changes.md` confirmation, by decompile,
that the Save-to-Decl setters (decl/classname/inherit/displayname) can't overflow on long input -- see
that doc for the write-up.

### 2026-07-08 -- Timeline event-arg widgets, Save Timeline shipped, crash root-cause + workaround found

> **CORRECTION (2026-07-13):** the crash root-cause and "freshly-placed needs save+reload / it's a
> pre-existing engine limitation" conclusions in this entry were later **disproven** — see the 2026-07-13
> change-history entry above. The crash was our own
> deferred-apply double-free (fixed by the `+0x290` inline commit), and the "silently stops partway through
> the save handler, no exception... never pinned down" postmortem below was a JavaScript `typeof null ===
> 'object'` throw on a `null` `state.edit` (fixed 2026-07-13). Fresh Timelines now save with no workaround.
> The event-arg widget work in this entry is unaffected and still current; the investigative narrative is
> kept as-is for history.

- **Every event-arg kind now gets a real editable widget**, not just read-only text: bool (checkbox),
  float/int/text (plain input), decl (a dropdown constrained to real engine decl names, via
  `enum_decls_of_resclass` +0x110), enum (same slot, member names instead of decl names -- one shared
  engine call for both), vec3/angles/color (a per-component field row, structured `{x,y,z}` object, not
  a space-separated string -- the engine's reader rejects the string form), and per-entity asset
  dropdowns (model/anim/tag lists scoped to the "Runs on" entity's resolved class -- an
  "exceed-the-OG" nicety, the original doesn't do this). Every widget keeps the `"<name> (<type>)"`
  label even once editable, matching the original's own `tl_arg_label` convention, so the field always
  documents the exact engine type it expects.
- **Save Timeline shipped.** Rebuilds `componentTimeLine`/`encounterComponent` from the live UI model,
  fresh-reserializes the target entity (not the stale open-time snapshot -- something else may have
  edited it, e.g. moved it in the 3D view, while the panel was open), patches in the rebuilt component,
  and commits via `apply_edit` kind=0 -- the same path Save-to-Decl already uses, id-targeted instead of
  paste-targeted. No new backend "kind" needed, as anticipated in the previous Known-limitations entry.
- **Two real correctness bugs found and fixed** while building the commit path, both by comparing
  against the original's own decompiled commit logic (`sh_timeline.cpp`'s `tl_build_event_json` /
  `tl_resolve_entity_ref`):
  - An event with no Time value was committing `"eventTime":0` instead of omitting the field entirely.
    The original only emits `eventTime` when the typed value parses as a clean non-negative integer
    (`FUN_180011a88`'s `QString::toUInt` gate); `parseTimeline` was collapsing an absent value to `0`
    on read, and the commit side re-emitted that `0` on every save. Fixed by preserving absent-ness
    through the round-trip.
  - The "Runs on" field commits its resolvable entity ref (or, typed free-text, the empty string
    fallback) *correctly resolved*, not whatever display text happened to be sitting in the box. The
    box shows `"<id>: <displayname>"` for a picked entity (matching the Entities-tab list convention),
    and typing into it wrote that *label* straight into the model; committing it verbatim produced an
    unresolvable ref (the engine interns event targets by exact name). New `tlResolveCommitRef` maps
    the stored string back to its canonical id before commit, mirroring the original's own
    `tl_resolve_entity_ref` (which re-reads the combo's *data*, not its displayed text).
- **Save Timeline's success/failure reporting made honest.** The failure path used to be silent (a
  disabled-but-clickable button doing nothing) or wrong (a "Could not open this timeline" message that
  didn't cover every failure shape). Opening now disables Save Timeline too when it fails (previously
  only Revert was disabled, leaving Save clickable-but-inert); a save failure now says plainly *"Save
  failed -- if this entity was just placed or reclassed, save the map and reload it, then try again"*
  -- see the root-cause finding below for why that's the actual fix in most cases.
- **The open timeline panel now silently auto-refreshes** on returning to the Timelines tab or
  regaining window focus (`tlMaybeAutoRefresh`), but *only* if there are no unsaved local edits. Without
  this, the panel could keep showing stale content indefinitely after something changed the entity
  outside the panel (a play-mode round-trip, another edit path) -- risking a later Save silently
  recommitting stale data over whatever actually happened. Gated on a lightweight local dirty flag
  (every real edit site now calls `tlMarkDirty()`) so an edit-in-progress is never silently clobbered.
- **Root-caused a hard crash / hang** reported after editing a Timeline, saving, playing, saving the
  map, and reloading -- an extensive investigation (log analysis, live read-only Ghidra attach/disassemble
  of the running `DOOMx64vk.exe`, and direct comparison against the genuine, unmodified original SnapHak
  2 Beta and v1.3.1 tools) that repeatedly disproved its own working hypotheses before landing on the
  real one:
  - Ruled out: JSON key-reordering in the frontend's patch step (the engine's own dump format is
    already codepoint-sorted, so this can't matter); the specific `componentTimeLine` content shape
    (a plain float-arg save round-tripped the engine's own save file byte-for-byte); `sh_target_any`
    wiring a source entity to the timeline (the identical crash reproduced with wiring never touched);
    and a suspected blank class/inherit commit (`ae_apply_one`'s own diagnostic logging showed
    class/inherit were *always* correct at commit time -- see the `backend-changes.md` entry for the
    real, narrower bug that diagnostic surfaced instead).
  - **The actual trigger is copy/paste.** Every crash this session traced back to a Timeline that had
    been copy/pasted at some point (the previous workaround for "won't save without it"). Live
    disassembly of the engine's native Ctrl+V handler (`PasteInstantiate`) shows it doing a large,
    repetitive field-by-field clone of the whole entity -- a plausible site for a subtle per-field
    copy bug that leaves the result looking completely normal (same visible data, opens fine, plays
    fine) while carrying something that only breaks much later, whenever the entity is next freshly
    rebuilt from a saved map file. This reproduced identically in the **genuine, unmodified original**
    SnapHak 2 Beta (froze hard on opening the Timeline Editor right after a copy/paste), confirming
    it's a pre-existing engine/tool limitation, not something the clone introduced.
  - **The fix that actually works: skip copy/paste, use save+reload instead.** A freshly
    placed/reclassed Timeline needs *something* to happen before Save Timeline will accept data --
    copy/paste was one way to get there (the wrong one); a plain native Save Map + Reload is another,
    and was confirmed, repeatedly, to fully unblock Save Timeline with no crash or corruption across
    many subsequent edit/save/reload cycles. (A conclusion later disproven — see the CORRECTION at the
    top of this entry.)
  - One real, narrow bug **was** found and fixed along the way (`ae_apply_one` could commit an empty
    class/inherit if the engine's own deserialize choked) -- see
    [`backend-changes.md`](backend-changes.md#2026-07-08--apply_enginec-ae_apply_one-could-commit-an-empty-classinherit).
    It's a correctness improvement, not the fix for the crash above (which reproduces in the original
    tool regardless), and is kept.
- **Postmortem -- a whole investigative branch built, tested, and reverted.** Attempted to make the
  Save Timeline result honest at a deeper level: instead of reporting success the instant the apply
  was *scheduled* (which nearly always "succeeds," regardless of whether the engine's own later,
  asynchronous drain of the request actually commits it), a new backend poll slot
  (`savetl_result`, a generation-counter pattern mirroring the existing `wire_edit_generation`) let the
  frontend wait for and report the engine's *real*, post-drain answer. This worked in principle
  (confirmed via the backend's own log showing correct `applied 1/1` results), but extensive checkpoint
  diagnostics through the entire JS save path -- reproduced identically, and completed successfully, in
  an isolated browser preview against the same code -- showed that against the real game, execution
  silently stopped partway through the save handler on a fresh entity's first attempt, before reaching
  even a trivial, heavily-guarded diagnostic line, with no thrown exception, no hang symptom, and no
  timeout ever firing. The exact mechanism was never pinned down. Given the validated save+reload
  workaround above makes this a non-blocking issue in practice, the whole branch (the new vtable slot,
  the generation-counter tracking, every `[DIAG]` checkpoint) was reverted rather than shipped
  half-understood; only the actual correctness fixes above were kept. **Flagged as a genuinely open
  question for a future session**, not a settled root cause -- see Known limitations.

### 2026-07-07 -- Timelines tab: list, open, entity tabs, event rows, Runs-on entity-picker

- **List**: any `idTarget_Timeline` / `idEncounterManager` entity is dual-added into the Timelines list
  (the same "OG quirk" `sh_tabs.cpp populate_one_entity` does), labeled by displayName (id fallback).
  The classname read is **change-gated**, not run on a fixed timer: the cheap per-poll entity scan
  (id-string + hidden flag only, unchanged from before this feature) computes a signature every
  ~330 ms as it always has, and the Timeline-specific classname rescan only runs when that signature
  actually changes -- a two-tier discipline
  (a cheap always-on check gating a rarer expensive one), not the naive "call it every poll forever"
  first attempt that broke the Entities list (see the postmortem below).
- **Open**: click a timeline -> `serialize_entity` (+0xc8) -> the raw JSON is shipped to the page ->
  `JSON.parse` + a walk of `entityDef.state.edit.componentTimeLine` / `.encounterComponent` ->
  `entityEvents.item[N]` becomes one tab per driven entity, `events.item[N]` becomes that tab's event
  rows. The parsing lives in JS against the
  engine's own valid-JSON bytes -- no hand-rolled C JSON parser needed.
- **Tabs**: labeled `Item N` (0-indexed, matching the decl's own `item[0]`/`item[1]`/... keys exactly --
  confirmed via a live, read-only Ghidra decompile of the original `snaphakui.dll`'s real tab-title code,
  which is the literal string `"item[%1]"`; ours drops the brackets for readability but keeps the
  0-indexing). A `+` tab appends a new blank entity-event-list; a small `x` on each tab removes it.
- **Events**: each row is a compact top line (Time + eventDef + a delete `x`) with every parameter
  stacked on its own line underneath -- also confirmed to match the OG's real structure (its per-row
  widget lands each arg in a `QFormLayout` *below* the eventDef combo, not beside it, per a live
  decompile of the row-construction chain). An "Add Eventcall" button per tab appends a blank event.
- **"Runs on" entity-picker**: an editable, free-text combobox backed by the live entity list --
  reimplemented from scratch after live-decompiling the OG's real composite widget (`FUN_180008180`,
  behavior-reference only, zero OG bytes copied) to see what was actually there: an entity combo, a
  "Use Display Names" checkbox, and a "Use ingame selection" button. The clone diverges deliberately:
  the checkbox is **dropped** -- the field always shows `<id>: <displayname>` together (matching the
  Entities-tab list convention), since the checkbox hid the id (which carries the module path and
  disambiguates same-named entities) and didn't persist its state across tab/timeline switches. The
  button is kept, renamed **"Use current selection"** (same idea, clearer wording); it's a visual
  placeholder for now (see Known limitations). Typing anything is accepted verbatim (`player1`, or any
  id not in the known list) -- there is no validation. Opening the dropdown (arrow click or focusing the
  text) always shows the *full* entity list regardless of what's already typed; only actually typing
  narrows it -- an early version conflated "has a value" with "is being filtered," so re-opening an
  already-picked field self-filtered down to just itself.
- **Revert** button: re-fetches the open timeline fresh (the same `openTimeline` round-trip a list click
  does), discarding every local edit. This is deliberately the *only* "undo" -- nothing in this tab
  persists to the actual decl until Save exists (see Known limitations), so any add/delete/edit is 100%
  session-local and safe to experiment with; Revert (or just re-clicking the timeline) always recovers
  the real, untouched data.
- **Postmortem -- three failed attempts before the list worked**: the very first version called
  `get_classname_copy` for every entity on every ~330 ms poll, unconditionally -- a much higher-frequency
  engine touch than the OG ever does, and it broke the Entities list on any map with a Timeline on it
  (list stayed empty, immune to Refresh, survived even a full DOOM restart). A live-debug session
  (the project's established read-only attach/read/detach method) chased this as an engine/threading
  problem and **fully exonerated the engine call** -- a direct memory read of the
  Timeline entity's classname chain (`entity->+0x158->+0x60`) came back clean both times. The actual bug
  was much simpler: a **missing closing quote** in the native JSON builder (`poc_emit_list`'s timelines
  array emitted `"name":"X}` instead of `"name":"X"}`), which made `PostWebMessageAsJson` silently
  reject the *entire* message whenever a Timeline was present -- explaining every symptom (empty list,
  Refresh-immune, survives a restart) without any engine involvement at all. Fixed with the one missing
  character; the change-gated rescan design (above) was kept anyway since it's still strictly better
  engine-touch discipline than the original attempt, even though it wasn't the actual root cause.

### 2026-07-06 -- Prefab create-from-selection crash fixes + Load/Place wired (stage-only)

- **Create from selection: two crash bugs, both fixed.** `serialize_selection` (+0xb0) used to hard-crash
  DOOM outright (an undersized `PREFAB_TEMP_SIZE` stack buffer), then, once that was fixed, crash
  intermittently inside the engine's `populate()` (a missing 3rd call argument). Full root-cause writeup:
  [`backend-changes.md`](backend-changes.md#2026-07-06--apply_enginec-prefab-create-from-selection-crashes).
  Fixing the second bug also surfaced a genuine, non-bug engine requirement: Create-from-selection needs
  the editor to be hovering a selected entity (see [`fidelity.md`](fidelity.md)). The webview UI now
  checks the hovered-id slot (+0x198) up front before attempting create, so it can show an accurate
  "hover over an entity first" result instead of the old misleading "nothing selected" toast or a crash
  -- the Create modal also has an inline tip to that effect.
- **Load / Place wired, stage-only.** `apply_edit` kind=1 (the same staging path `sh mkcmd` uses) stages
  the prefab into the paste slot, plus an auto-`clear_selection` first; the user presses Ctrl+V
  themselves to place it. Two attempts at automating that last step (a direct `PasteInstantiate` call,
  then a synthesized native Ctrl+V) each surfaced a different real side effect in DOOM -- a crash on the
  next Play/Editor transition, then an unwanted ESC-menu popup from the OS-level focus switch. Given
  three different failure modes across three attempts, this is deliberately left as stage-and-prompt
  rather than continuing to chase automation; see [`backend-changes.md`](backend-changes.md) for the
  full story. Delete/Rename/Folders are unaffected by any of the prefab crash history -- they're pure
  Win32 file ops through `resolve_prefab_path` (+0xc0) only, no `serialize_selection` or `populate()`
  involved.
- A silent-truncation bug in the shared apply pipeline (`APPLY_TEXT_CAP`, hit while stress-testing large
  prefabs through Load/Place) was also found and fixed this session -- backend-only, full writeup in
  [`backend-changes.md`](backend-changes.md).

### Baseline -- initial POC buildout (undated)

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
- Camera Origin uses editable X/Y/Z fields plus "Lock position" in the responsive footer. Live camera
  updates do not overwrite a field while it has focus; committing a field moves the camera, and the
  checkbox pins the displayed position until released.
- Modern light/dark theme selected from the left-aligned View menu (persisted by the backend-owned
  `config.json` service in DOOM; `localStorage` is standalone-PREVIEW-only). Native controls (scrollbars,
  checkboxes) follow the theme.
- Installed-version + connection status in the status bar.
- Browser preview mode: `mockup.html` self-populates with sample data and is fully interactive when
  opened without a WebView2 host (for fast UI iteration); inert in DOOM.
- Default window size bumped to 1440x900 (from 1040x720) so the Entities and Prefabs tabs fit without a
  manual resize on first launch.
- Historical explicit **Deselect** button next to "Select in 3D editor": it called `clear_selection`
  directly and was added because a native empty-space click could not clear a list-driven selection.
  That root cause was fixed on 2026-07-27; the visible button was then removed on 2026-08-17 in favor
  of blank-space deselection outside the Entity State inspection pane through the same native path.
- **Prefabs tab, wired to the real filesystem** (`%LOCALAPPDATA%\snapmap-plus\prefabs\`) -- no fake/mockup data:
  - Live list of real `.json` prefab files, refreshed from disk on every Prefabs-tab click; an empty-state
    message when there are none yet.
  - Prefab Details shows the selected file as an interactive, untextured 3D scene above Name, with its
    controls top-right and aggregate entity count bottom-right. Description and Tags are editable
    sidecar metadata; no per-class summary is displayed.
  - Delete and Rename are real file operations (`DeleteFileA` / `MoveFileA`), each with a collision/confirm
    guard client-side and a safe no-overwrite guarantee native-side.
  - **Folders**: one real level of subdirectories under `prefabs\` (no nested-within-nested) -- the
    directory *is* the source of truth, no separate manifest file to desync. New Folder button, drag-and-
    drop a prefab between folders/root, folder Rename, and folder Delete (moves any remaining contents back
    to the root list, then removes the now-empty directory). Folders render above root-level items.
  - Filter/search box narrows the list client-side over the last real fetch (same pattern as the Entities
    tab's filter); folders with zero matches are hidden while filtering, with a "No matches." empty state.
  - "Create from selection" works end-to-end. "Load / Place" reads the prefab file's raw JSON, clears the
    current editor selection, and stages it into `editor+0x209a8` (a plain `kind=1`/mkcmd apply item,
    the same staging `sh mkcmd` performs) -- then prompts the user to press Ctrl+V themselves in
    the 3D view to actually place it. This is deliberately stage-only, matching the *original* SnapHak's
    own workflow exactly (confirmed by decompiling `snaphakui.dll`/`XINPUT1_3.dll` -- a prefab
    double-click there only ever stages, never auto-places either). Two earlier attempts at automating
    the placement step (calling `PasteInstantiate` directly, then synthesizing a native Ctrl+V ourselves)
    each surfaced a different real side effect -- see [`backend-changes.md`](backend-changes.md) for the
    full story. Available from both the "Load / Place" button and the prefab list's right-click menu. The
    generic engine "SnapStack: ..." toast is suppressed for this op specifically (`ae_toast_result` skips
    it for `op == "load-prefab"`) since the webview's own toast already tells the user what to do next;
    the `sh mkcmd` console command still gets the engine toast, since it has no toast of its own.
  - Live Create-from-Selection icon state/count tooltip and the create modal's "From N selected entities" text
    both track the real editor selection continuously (two separate display bugs fixed: the count used to
    silently cap at 64 regardless of the real selection size, and the modal text never updated at all --
    it was stuck on its original static placeholder).
