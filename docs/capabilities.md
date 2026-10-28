# Capabilities

What the clone does, grouped by kind. This is the user-facing feature inventory; for how the pieces
fit together see [`architecture.md`](architecture.md), and for the intentionally-faithful quirks
see [`fidelity.md`](fidelity.md).

## Grid Room dimensions

Classic and modern Grid Rooms expose independent XYZ dimensions in DOOM's native
Module Properties panel, including Blueprint X. Fixed-size doors, connected
branch translation, built-in lighting, collision and navigation follow the
resized shell. Authored objects retain their module-local transforms. Values
clamp to doorway minimums and placement-dependent world limits. See the
[Grid Room instructions](https://www.snapmapplus.com/snapmap-plus-guide.html#resizing-grid-rooms).
Maps using resized rooms require Snapmap+ on the player's PC.

Every built-in room light (the probe, the modern room's five fill lights and
its ceiling light, the classic room's light) scales per axis with the room:
origins about the room center, coverage along each light's own axes, height by
the interior below the ceiling, and the ceiling light keeps its stock clearance
under the roof (at most a quarter of the height). Distance cutoffs grow with
the largest ratio and never shrink; intensity and color are unchanged. A resize
in the editor and a later map load or Play compute the same values from the
installed room declaration.

A size edit changes only the axes you changed. When the world limit stops an
axis, the panel shows the value that fits and the backend log names the axis,
the limiting module, the coordinate it reached and the limit. A room that
already extends past the limit (a lowered cvar, an older map) can still be
shrunk. `sh_gridsize` resizes the selected room from the console through the
same path.

Play divides the map into world areas, culling cells at most 512 units wide
on X and Y, and DOOM refuses to load a map with more than 8192 areas or 8192
area nodes (its loading screen then never returns). A single modern room
reaches that at about 32768 x 32768. Snapmap+ counts the areas when Play
builds the map: a map that fits keeps DOOM's 512-unit areas exactly, and a
larger one is built with the smallest coarser size (1024, 2048 and so on)
that fits, so oversized rooms load. Coarser areas mean coarser visibility
culling, so very large maps draw more of themselves each frame. The log names
the size Play uses when a map is opened, on each Play and after a resize that
needs a coarser size. On a build where the size cannot be changed, a resize
that would cross the limit is cut back on the changed axis to the largest
size that loads instead.

TAB from Blueprint places the Object Mode camera by dropping straight down
from the top of the map onto the hovered spot. DOOM gives up when that drop
crosses more than 18364 units of empty space, which a room taller than that
always contains: you landed under the tall room's roof, or, for a module far
below its roof, back at your last Object Mode position, often inside the
tall room. `object_entry.c` lets the drop cross the whole map height, so TAB
lands on the floor under the cursor. Maps with stock-height rooms behave
exactly as before.

## Map rendering

DOOM's native Settings > Properties panel includes a map-wide View Distance
control on the Snapmap+ tab. 8192 keeps each environment's own distance, so
modules, Set Environment and Player Set Environment nodes and campaign
environments decide it. Zero is the maximum, 200000, the largest far clip the
game accepts (not an infinite one). Any other value is the far clip everywhere
the map renders in SnapMap, editor and Play alike, overriding every environment
source; environment fog and other effects are unchanged. Stepping left from
zero wraps the row to the maximum. A module on the Default Environment FX
(snapmap/base) draws to 60000 units in the editor, with dark fog from 32000,
but adds no environment in Play, where the measured far clip was 8192; set a
map value (0 for the maximum) to get the same distance in the editor and in
Play. The editor's own camera cvars are not touched.
Apply commits the draft to native map variables; normal saving retains the
value inside the map. Both editor playtests and direct saved-map loads use it,
with bindings for OpenGL and Vulkan. A custom value takes effect on Snapmap+
clients; restoring the default does not retain a rendering requirement. Fog is
not a map setting; older maps' fog values are ignored, and loading such a map
converts its value so the next normal save stores only the distance. See the
[view distance guide](https://www.snapmapplus.com/snapmap-plus-guide.html#map-view-distance).

## Map bounds and Blueprint zoom

The native Settings / Properties panel gains a third category, **SNAPMAP+ MOD
SETTINGS**, for built-in mod settings (`settings_tab.c`). It owns the panel
hooks and keeps each registered group's map-owned draft, committed by native
Apply and discarded by Cancel or Revert. When the category cannot be installed
the groups appear first in category 0, or last if the row injection site is
missing too.

**Unlock Map Bounds** (`map_bounds.c`) is an on/off row (a native toggle row,
`sh_settings_tab_add_bool`). It stores `smp.bounds.v1` in the map's native
variables (`1;<units>`, 0 = native): on writes the 32671 ceiling, off writes
`1;0`, and any stored value above 16000 (maps saved by the earlier numeric row)
reads as on and keeps its value until changed. A map that sets it raises
`snapEdit_environmentModuleBounds` and `snapEdit_cameraDistanceLimit` to that
value and scales `snapEdit_minBlueprintFloor` / `snapEdit_maxBlueprintFloor` in
proportion, from map load (editor, Play, or a direct saved-map load) until the
next load restores the values captured before the first override. Values clamp
to 16000..32671: assembled collision and navigation use signed 16-bit world
coordinates and navigation reaches 96 units past a module. Unmodified clients
recompute module validity from their own 16000 limit, so maps using the wider
space require Snapmap+. Native maps never gain the variable. See the
[map bounds guide](https://www.snapmapplus.com/snapmap-plus-guide.html#map-bounds-and-blueprint-zoom).

`editor_zoom.c` raises the Blueprint zoom-out to 2.75x by default (range 1x to
4x) on every map, enough to show the whole map square from above (the camera
sees about 0.40 x its height to the top and bottom edges at 16:9): the free
orbit camera's height limit (native
`snapEdit_cameraDistanceLimit`, or the unlocked map bounds when larger), its pan
range and the sky view's limit are read from Snapmap+ values instead of the
engine constants. In Blueprint frames the editor forces r_zfar while the
environment blend writes the map distance back. Both are raised to the same
Blueprint far plane, 33000 times the zoom ratio: the editor's two 33000 loads
read the Snapmap+ value, and the blend's maxViewDistance read
(`map_render_native.c`) asks `sh_editor_zoom_far_plane()`, which applies it under
the editor's own condition. Above the 33000-unit editor environment volume the
blend stops writing, so the editor's write alone keeps the floor in view; if
that write cannot be redirected, the height stays below 32000. Elsewhere the far
clip is unchanged. The editor environment volume (`snapEdit_environmentFogBounds`,
built once with the editor render world as a +/-33000 environment area and a
32768-scaled `_unitCube`) is sized at install for the highest reachable camera,
134768, so the camera never looks through the cube's thick blue fog and the
environment blend keeps running; without that patch the height stays inside the
native volume. When map bounds change, the Blueprint ground plane (the faint
grid, a `_unitSquare` scaled to the module bound when the editor is set up) is
rescaled in place. Module hover and selection trace a ray of
`snapEdit_modulePickingDistance` (50000) from the camera; both of its loads read
a Snapmap+ length instead, 50000 x (far plane / 33000) (137500 at native
bounds, about 280770 unlocked), so a module the camera can see can be picked.
Client-side only; no map data changes.

## Packages

The package system is being rebuilt for the April 2024 DOOM update (Steam build 13954591). This
release reads, installs and carries no packages: nothing is loaded from
`%LOCALAPPDATA%\snapmap-plus\overrides\`, no `package-cache` or `package-staging` folder is created,
and a map saved or published with packages by an earlier release loads as a plain map without them.
Folders that earlier releases left are not touched; you can keep or delete them.

## Console commands

Registered with the engine command system; run from the DOOM console.

| Command | What it does |
|---|---|
| `sh_rawmaps` | Raw map files: state, paths, load, save. Run it bare to see what is set, or `sh_rawmaps help` (or `?`) for every verb. `savepath` chooses where saves go, `overwrite` chooses whether a rawmap you open may replace the map it opened over. |
| `sh_rawmaps_on` | Enable raw map save/load (the `rawmap.json` substitution). Same as `sh_rawmaps on`. |
| `sh_rawmaps_off` | Disable raw map save/load. Same as `sh_rawmaps off`. |
| `sh` | The SnapStack dispatcher — routes the 20 SnapStack subcommands (below). |
| `sh_spawn` | Spawn `<entitydef> <name>` at the player and teleport to it. |
| `sh_dumpdef` | Print (and copy to clipboard) an existing entity's resolved entity definition. |
| `sh_spawninfo` | Generate `spawnOrientation` / `spawnPosition` from the current map position. |
| `sh_entlist` | List the editor entities. |
| `sh_dialogtest` | Raise DOOM's own modal carrying the given text (diagnostic for the engine dialog surface). |
| `sh_dialogpoll` | Read the answer to the dialog `sh_dialogtest` raised. |
| `sh_listres` | List all resources of a given class (optionally copy to clipboard). |
| `sh_type` | Print the members of an idTech class or the values of an enum (runtime introspection). Add `-v` to also show each field's byte offset and size. |
| `sh_validclasses` | List the engine-valid classnames for a given inherit (the same enumerator that feeds the Classname dropdown). |
| `sh_target_any` | Reveal / re-hide the campaign-only and normally-hidden placeable entity decls in the SnapMap editor palette. |
| `sh_dumpmap` | Dump the current generated `.map` from memory to `<game dir>\base\mapdumps\<name>.map` (debugging). Never overwrites an earlier dump. |
| `sh_genbmodel` | Generate a bmodel from a `.obj` / `.ase` / `.lwo`. |
| `sh_genmd6model` | Compile a `.md6model` into a `bmd6model`. |
| `sh_debugrender` | Renderer debug toggle (developer tool); `dumprenderinfo` prints the active rendermodel count and every model's name. |
| `sh_alginfo` | Report the math-acceleration status. |
| `sh_perf` | Report how much time each Snapmap+ hot path has taken this session: the engine resource opens that pass through the override hook, the engine writing the open map as JSON, the navigation parse, the green preview build, and the session log. `sh_perf reset` starts the counting again. `sh_perf read` times reading the open editor map whole and entity by entity, and estimates the cost of reading only the marked navigation volumes. |
| `sh_gridsize` | Print the selected Grid Room's size, or `sh_gridsize <x> <y> <z> [instance]` to resize it through the same path as Module Properties. Prints the applied size and the reason any axis was limited or the edit refused. `sh_gridsize templates` lists, for the stock modern room and each resized variant, the module entity templates a loaded room receives (diagnostic). Without an instance it uses the Blueprint selection, then the room whose properties are open. |
| `sh_help` | List every Snapmap+ console command and cvar with its description in one place. |
| `sh_navmesh` | Report the baked AI navigation the loaded map is serving — for each marked module copy, each demon size's outcome (built, built with parts left out, keeps its shipped navigation, or not baked yet) with its reason, and every box the map read left out with why; it also says which map the last Play read its boxes from. Leads with how many map loads have reached the navigation table this session and what the last one found, so a map that quietly brought none is distinguishable from a load that never happened. |
| `sh_navmesh marks <n>` | Mark the first `n` marked volumes of every module red, the way a refused bake marks the ones it could not place, so the marks can be checked without a map that refuses. `0` clears them. |
| `sh_superscriptop` | Dump SuperScript / eventDef data (e.g. emit the eventDef table as a header). |
| `cs_dumpeventdefs` | Dump all eventDefs to a file. |
| `cs_fieldinfo` | Print field info for a type (developer tool). |
| `cs_dontuse` | Toggle the higher-precision engine-math overrides (a precision/perf tradeoff; off by default). |
| `cs_start_render_logging` | Set up the render-logging hook. |
| `sh_disable_devmode` | Turn developer features off while keeping Bethesda.net connectivity. |
| `sh_reenable_devmode` | Turn developer features back on. |
| `noClip` / `infiniteHealth` / `noPlayerDeath` / `noPlayerKill` / `noTarget` | The five player-cheat toggles for the local player: no-collision flight, infinite health, can't die, can't be killed, enemies ignore you. |

## Cvars

Console variables; defaults shown in parentheses.

| Cvar | What it does |
|---|---|
| `sh_copy_reslist_to_clipboard` (0) | Copy `sh_listres` output to the clipboard. |
| `sh_pretty_on` (0) | Pretty-print saved rawmap JSON. |

## SnapStack ops

Subcommands of `sh`, operating on numbered entity-id stacks and named groups. A numbered stack
is one-shot scratch — apply/use ops drain it; move a set into a named group (`pop2g`) to reuse it.

The SnapStack stores + all 20 handlers live in the **backend** (`src/backend/snapstack.c` +
`json_patch.c`), registered once before the frontend loads. See
[architecture](architecture.md#threads-and-edit-results) for command execution and declaration-commit threading.

**The 20 OG subcommands** (usage `sh <op> <stack> …`; a numbered stack index; several ops also accept a
letter-first **group name** in place of the stack index):

| Op | What it does |
|---|---|
| `psel` | Push the current editor selection onto the stack, then clear the selection. |
| `popsel` | Add the stacked ids (or a named group's) back into the editor selection. |
| `phov` | Push the hovered entity onto the stack (de-duplicated). |
| `pr` | Push every valid id in an inclusive `[lo..hi]` range. |
| `pg` | Push a named group's ids onto the stack. `sh pg <grp>` alone implies stack 0. |
| `pop2g` | Move the stack into a named group. `sh pop2g <grp>` alone implies stack 0. |
| `cstk` | Empty the stack. |
| `filtinh` | Keep only the stacked ids whose inherit matches the argument. |
| `filtcls` | Keep only the stacked ids whose classname matches the argument. |
| `bss` | Bulk-set a string property on the stacked entities. |
| `bsi` | Bulk-set an integer property. |
| `bsf` | Bulk-set a float property. |
| `bsb` | Bulk-set a boolean property. |
| `bse` | Pop the last id; set each remaining id's chosen property to that id's id-string. |
| `bsin` | Set the inherit of each stacked entity. |
| `bscls` | Set the classname of each stacked entity. |
| `bsincls` | Apply a validated inherit/classname pair, rebuild its declaration, and reassert the pair. Reports completed, refused and partial outcomes separately. |
| `accl` | Pop the last id as the receiver; reference-assign the remaining ids to it. |
| `acctargets` | Pop the last id; append the remaining ids to its `targets` list. |
| `mkcmd` | Synthesize a reusable command-entity macro from the stacked ids. |

**New SnapStack+ commands** (clone additions — not part of OG's 20). They read and manage the same
backend stores every SnapStack op runs against, so they always reflect live stack/group state. Output
goes to the `~` console with a summary toast.

| Op | What it does |
|---|---|
| `chkstk [N]` | Inspect stack `N` (ids + id-strings); omit `N` to summarize every non-empty stack. |
| `chkgrp [name]` | Inspect a group's ids; omit `name` to list all groups + counts. |
| `clrgrp <name>\|*` | Delete a named group entirely (`*` deletes all). |
| `snapstack_diag` | Report, per subcommand, which loaded DLL currently owns the handler. |

## The Studio window

The Snapmap+ window — HTML/CSS/JS rendered in a WebView2 control, opened inside the SnapMap
editor (run `sh` in the console if it doesn't auto-open). Full detail: [`webview-ui.md`](webview-ui.md).

| Surface | What it does |
|---|---|
| Window shell | The Win32 host window + the manual 30 Hz think-loop; native Windows 11 rounded corners and drop shadow retained around the custom captionless chrome through the same one-pixel DWM frame treatment as snapmap-midi; a menu bar with a persistent light/dark theme toggle (seeded before the page is first shown, so a saved dark theme never flashes light); and three footer groups for the connection dot/version plus selection, editable live camera X/Y/Z coordinates with a Lock position checkbox, and Updated/help. Camera feedback is sampled at the frontend's full 30 Hz cadence and publishes only when a coordinate changes. Committing a coordinate edit moves the editor camera, while Lock position pins all three coordinates until released. The green dot is the sole visible connection indicator. Entities, Prefabs, and Timelines share one adjustable, keyboard-accessible vertical-divider position, equal 40px header tracks, common pane insets, and inset header/toolbar separators while their background fills remain continuous; the Assets tab is exempt and retains its purpose-built three-column layout without scrubbers. At compact widths the camera controls reflow onto a second row, while the page remains clamped to the WebView client area so header and footer never scroll out of the viewport. Expanded Decl Text uses a client-bounded modal and dims the stationary shell underneath. |
| Entities tab | A filterable entity list (multi-select, a persistent Show Hidden toggle, and one persistent selection direction: Follow Selection or Select in 3D; right-click for Copy ID / Delete / Push to stack 0 / Clear stack 0) plus the Entity State panel: classname / inherit / displayname fields and the Decl Text editor — line numbers, syntax coloring, structural lint, advisory schema checks, and a modal expanded mode that reparents the live editor without losing selection, scroll, or undo state. Diagnostic rows remain click-to-jump targets with pointer cursors but no hover highlight. Long entity rows ellipsize inside the same pane inset used by Entity State instead of painting into the divider, with the complete path/name available on hover. "Save to Decl" commits the edits in memory. Clicking a blank structural surface outside Entity State clears the current page and 3D-editor selection; the entire Entity State pane remains protected inspection space, including its blank padding and expanded Decl Text mode. SnapMap's own built-in filter/droppable helper entities are excluded from the list and from every entity picker (dev-layer-only; a mapper's own filters are unaffected). |
| Keyboard paging | ArrowUp/ArrowDown page the Entities, Timelines and Prefabs lists, and — while their dropdown is open — the Inherit / Classname combos and the Timelines "Runs on" picker. Filtering still works alongside it; text editors and rename fields keep normal caret movement. In the asset browser, paging onto a **file** row also selects it (debounced, so holding the key down previews only the row you stop on); **folder** rows still need Enter/Space, since opening one replaces the whole list. |
| Native selection parity ("Select in 3D") | Entities selected from the list behave exactly like ones clicked in the 3D view: an empty-space click deselects them, Delete removes all of them, Move works, and the bottom-bar controls apply — single or multi-entity, including switching between entities. Deselecting natively also clears the list highlight. Previously only the now-removed explicit Deselect button worked, and Delete/Move misbehaved (Move could soft-lock the game); the cause was that the editor's own mode state was never told a selection existed. |
| Native Delete from the Entities list | Right-click Delete runs the editor's own Delete Selection on exactly the chosen entities, with the same rules as the Delete key: objects the editor protects, room doors and caps, and maps that restrict deleting are refused, and the deletion is one undo step ("Delete Selection"). Whatever the 3D cursor points at is never added. The list keeps its selection and shows why when a delete is refused; success is reported only after the engine has removed the entities. |
| Selection guard while holding | Selecting from the Entities list is refused (with an explanatory toast) while you're grabbing an entity or holding a staged prefab. The engine's Escape/cancel path restores a snapshot keyed positionally to the selection that was live when the grab began, so changing the selection first makes Escape swap entity pointers inside the live map — duplicating entities, deleting others outright, and freezing the game. A pre-existing engine bug (reproduces on released builds); only cancellation triggers it, never accept. Placing a *new* palette entity captures no snapshot and is left unrestricted. |
| List-assembled group grab | With a selection pushed from the Entities list, grabbing any one of those entities in the 3D view grabs **all** of them — not previously possible. Practical use: browse a logic chain in the 3D view, add the other entities you want to bring along from the Entities list, then grab the node you're on and move the whole group. Assemble the selection before grabbing; the selection guard refuses list changes while a protected grab or staged prefab is active. |
| Prefabs tab | Save and load selection prefabs as JSON files under `%LOCALAPPDATA%\snapmap-plus\prefabs\` — one folder level with rename/delete/drag-between-folders; per-prefab description + tags (stored in a `<name>.meta.json` sidecar; the filter box matches tags across folders); compact Lucide Create-from-Selection and New-Folder actions; and blank-list-space deselection through the shared selection router while Prefab Details remains protected. Create guards the engine's grab frame: PrefabPopulate anchors the prefab on the view-centre line clipped against the hovered entity, keeping the clip midpoint even on a miss, so a cursor hover the centre line does not cross used to save a meaningless `grabDistance` (about -3e10) and float-collapsed positions. A detour on that grab-frame helper (VK 0x1412164d0 / GL 0x141215920, armed only inside Create) keeps a hit, and otherwise re-anchors the frame on the hovered entity's box centre with its own axis and a `grabDistance` equal to that centre's depth along the view (32 to 113175 units). The engine then stores every entity exactly relative to it; paste reads only `grabAxis`, `grabDistance` and `cameraYaw`. The Create toast says "grab point corrected". Create refuses only when no finite frame can be built, or when a backend without the detour produced a file whose `grabDistance` or positions are absurd. Selecting a prefab reconstructs its saved transforms in an interactive, orbitable 3D preview above Name, placed through the prefab's `grabAxis` as the paste places them, so a prefab anchored on a tilted, rolled or mirrored entity previews level. Sparse saved rotations and dimensions are composed over their identity and installed entityDef defaults (16/128/64 for a Blocking Box whose decl cannot be resolved), so rotated props and thin/non-uniform blocking boxes retain their in-game placement and size. Blocking Boxes ticked for AI Navigation are tinted green, with their count beside the entity count; placing a prefab that brings them in requests the navmesh update automatically once they are dropped. Orbit/zoom/frame help is top-right, `N entity/entities` is bottom-right, and Name, Description, and Tags use the same field typography as Entity State. Geometry is decoded on demand from the user's installed resource containers with neutral lighting: multi-surface props and pickup-spawner contents use their real models; blocking shells, invisible triggers, decals, interactables, full-size logic hexagons, and half-size input/output circles and filter diamonds receive distinct scene treatment. Only unsupported solid models remain proxy boxes. The Cartesian floor grid extends beyond the scene and fades under a circular mask. No textures or game-resource bytes ship with or persist in Snapmap+. "Load / Place" stages the prefab and queues the editor's native paste action when its guards allow it. Queue acceptance does not confirm that the engine has entered placement mode; once it does, position the prefab and click to place it. It degrades to stage-only (with a toast saying why) whenever the engine itself would refuse — copy/paste disabled or unavailable, not in EntityMode, already holding something, hovering an entity, or a selection that could not be cleared. A staged prefab also **survives a Play round-trip and a map change** — come back and Ctrl+V still works, matching the engine's own Ctrl+C clipboard. See [allocation lifetimes](architecture.md#engine-data-ownership). |
| Assets tab | The asset browser — the shipped game catalog, searchable, with a live preview pane, and one click to put an asset on the map. Full detail in the section below. Also reachable as a modal from the Entities tab ("Browse assets"), scoped to the entity you came from. |
| Timelines tab | The list of timeline entities; opening one edits its events and per-event parameters, with reference/decl/enum parameters constrained to valid choices, entity pickers for entity-typed args, and per-event documentation. Its disabled Create action is a compact primary Lucide plus (with the standard primary hover treatment ready when enabled), and blank list space closes the current timeline through the shared selection router while the Timeline editor remains protected. |
| Feedback (help icon) | The Lucide Circle Help button at the statusbar's right edge opens the Send-feedback dialog: category (bug / feature / incorrect info / other), title, details, optional contact. Sending files it as a labeled issue on this repo's tracker — no GitHub account needed. See the network note below + [`feedback.md`](feedback.md). |
| Crash reports | Fault capture attempts a bounded local record. New recovery-class records show a notice; terminal or unknown records prompt for a report in the current session when possible or on a later launch. The dialog shows recorded details and offers optional scrubbed log tails. Capture, dump creation and duplicate matching are best effort. See [`feedback.md`](feedback.md). |

## The asset browser (Assets tab)

Every category is enumerated **live from the game's own shipped containers** when it is first
selected — no pre-extraction step, no bundled asset list, and nothing shipped in this repo. The
browser requests only that category and any qualifier it needs (the atlas-only set for Materials or
the soundbank map for Sounds), then keeps at most two category catalogs -- enough for the Assets tab
and modal to retain independent current views. Older category names and folder nodes are evicted,
while their scalar rail counts remain. Unvisited categories do not cross the WebView bridge or
occupy the page's catalog cache. Only assets from `snap_gameresources` are listed, with sounds as the
deliberate exception (see below): SnapMap never mounts the base game's broader
`gameresources.resources`, so a base-game-only model resolves in the editor as a black cube.

The native side is demand-driven too. Its first request parses the installed resource indexes into
names and payload offsets, interns equal names in compact storage, drops base-game record classes no
browser route can use, and releases the complete index buffers. Wwise metadata is not read until
Sounds is selected; the XML is streamed for only event and bank tags, then reduced to one copy of
each retained name. The `.vmtr` name union waits for Materials and uses exact strings rather than
fixed-width rows. Preview payload bytes stay in the installed game files and only the selected record
is read.

| Category | What it holds |
|---|---|
| **Pinned** | The mapper's own shortlist, at the top of the rail. Any asset, of any type, starred from its row; one shared list rather than one per type, because "the things I am working with right now" is rarely all of one kind. Kept in `%LOCALAPPDATA%\snapmap-plus\pinned.json` — deliberately **not** in `config.json`, so a malformed pin list can only ever cost the pins (see [Persistent settings](#persistent-settings)). |
| Materials | Surfaces. Previewed as real pixels — see below. The **union** of `material` decls and `.vmtr` megatexture atlas rows: a material is addressable by name *or* by rectangle and neither set contains the other, so a decl-only list hid thousands of rows that are paintable via Virtual Mapping. A **Cross Platform Textures** filter narrows the list to the 224 megatexture rects hand-tested to render identically on PC, Xbox and PlayStation. |
| Images | The lower-level image records the materials sample. They preview directly through the same bounded BC1/BC3/BC7 container decoder used by non-atlased materials. |
| Models | Props: `.lwo`, the `md6Def` set, and the `discreteAnimation` set — the last of these being the breakable/gib models that a `breakable` decl names, which are indexed under their own decl type and were invisible to a `model`-only catalog. |
| **Modules** | The 232 `mega_blessed` palette modules — whole SnapMap rooms, placeable as a single entity that is both visible **and solid**. |
| **Brush models** | Every other baked `.bmodel`: the individual wall, floor and detail pieces those modules are assembled from. Render-only. |
| **Clip models** | The `cm` type (`.bcm` / `.lwo` / `.md6`) — collision shapes, appliable on their own. |
| Sounds | The **union** of `sound` decls and Wwise events, deduplicated case-insensitively. Neither set contains the other, and the event-only half is ~2,600 names including the generic SnapMap VO, so a decl-only list is missing thousands of sounds a mapper can hear in the editor. Base-game-only sounds are offered too, and they work. Filed by **soundbank** rather than by name — see below. |
| **Lights** | The light **materials**: the projection a light shines through, written as `lightMaterial`. Point vs spot is not the asset — it is which entity carries it, so it is the Create-as choice. |
| FX, Particles, Decal atlases, Entity defs | The remaining placeable decl types. |
| **Perks**, **SWF / Flash** | Reference-only. A perk is granted by an `idTarget_Command` entity and a `.swf` belongs to an entity that owns a screen; neither structure is worked out, so both are names to copy and wire by hand. They are listed under Reference rather than Placeable on purpose — a category under Placeable whose Apply button does nothing reads as a broken tool. |

**Previews are real, and cover the catalog.** A material's pixels are produced by locating its pages
in the shipped megatexture set and decoding them with **DOOM's own page decoder, called in-process**.
That decoder is a pure function — no renderer, no GPU, no virtual-texture state and no map residency
— so a preview does not depend on the loaded map having the material on screen, which is what makes
whole-catalog browsing possible at all. Materials with no atlas rect (roughly half) fall back to
reading the image out of the `.index`/`.resources` containers and decoding BC1/BC3/BC7 directly.
Rows in the Images category enter that same container path at the image record instead of requiring
a material decl first. The selected kind is preserved, so an Image bypasses VMTR and wins even when
the installed index also contains a same-named Material. Sounds are auditioned through the editor's
own preview path with working play/stop. Mega2 previews read only the selected cell's page id,
offset/size entry, and payload; full shard tables are never copied into the process. The preview
worker sleeps while idle, and both its decode scratch and completed encoded image are released when
no longer needed.

**Apply to selection** writes the asset into the selected entity's decl and commits immediately —
one entity at a time, since it patches the decl the editor has open. **New entity** authors a
one-entity prefab and requests the engine's native paste action. A refused action can leave it
staged for Ctrl+V; an accepted request does not confirm placement.

**What may be applied is decided per target class**, in one place, and each carrier is gated to the
classes that own the field it writes:

| Asset | Writes | Allowed on |
|---|---|---|
| Material (by name or Virtual Mapping) | `customMaterial` / `virtualmapping` | the render-capable classes: blocking volumes, triggers, props, movers, `idAI2*`, cap entities, dynamic SnapMap entities |
| Model · Brush model · Module · Clip model | `renderModelInfo.model` (+ `clipModelInfo.clipModelName`) | the same set, **plus** interactables — minus three whose model *is* the mechanic (`idInteractable_Obstacle_SnapDoor`, `idInteractable_WorldCache`, `idInteractable_EliteGuard_Coop`) |
| Sound | `sound` | speakers (`idSnapMapGameEntity_Speaker`, `idSpeaker*`) |
| Light material | `lightMaterial` | lights (`idSnapMapGameEntity_Light`, `idLight`) |
| Particle | `particleSystem` | emitters (`idSnapMapParticleEmitter`, `idParticleEmitter`) |
| FX | `fxDecl` | FX entities (`idVolume_ToggleableDamageOverTime`, `idLaserHazard`, `idDynamicStampEntity*`) |
| anything | — | never the player start (`idSnapMapGameEntity_ComboStart*`), and never a variable or a SnapMap action |

Materials and models deliberately share one list: anything that can wear a model can wear a surface,
and re-texturing a mover or making a trigger volume visible are ordinary techniques.

The gate keys on the entity's **class**, never on whether that class is placeable from the editor's
palette — those are different questions. `idSnapMapParticleEmitter` has no placeable palette entry at
all, yet Snapmap+ creates one by overriding the classname on `snapmaps/unknown`, so gating on
placeability would refuse the emitter this tool just made.

A refusal names the class and says what it lacks, rather than greying the button out silently.

A module is placed by writing **both** halves — the baked geometry into `renderModelInfo.model` and
its paired collision into `clipModelInfo.clipModelName`. The two live at different paths and pair
232-for-232; the browser derives the collision name for you. The def's inherited `CLIPMODEL_AUTO` is
left alone, because naming a clip model overrides the automatic derivation on its own.

A **light** is placed by picking the light material and then choosing Point light or Spotlight under
Create as. Only `lightMaterial` is written; the cone, the colour and the intensity come from the
inherited def. Applying a light material to a light already on the map replaces its existing value.
Three of the shipped light materials contain a literal **space** in the name (`lights/gaus
_slowpulse` and two more) and the unspaced forms do not exist, so they are listed verbatim — anything
that trims or splits on whitespace corrupts them.

**Sounds are filed by soundbank, not by name.** After the duplicate collapse nearly every sound name
is a flat `Play_something`, so a name-derived folder tree was one root folder of ~8,000 rows. The
Wwise `<SoundBank>` grouping is the only real structure the catalog has — 24 non-empty banks, none of
them enormous, with `doom_snapmaps` being the set SnapMap itself loads. An event listed in several
banks is filed under one home (a specific bank in preference to the always-loaded `doom_initial`), so
no sound appears twice. Search deliberately cuts **across** banks rather than within the open one.

**No Refresh control, by design.** The catalog is indexed once per process out of `.resources` files
that cannot change while the game is running, so re-fetching returned identical bytes; a type whose
names never arrived re-asks when it is selected, which is the only retry that was ever needed.

## Persistent settings

`%LOCALAPPDATA%\snapmap-plus\config.json` holds player preferences shared through the backend-owned
settings service. The registered settings are `theme` (`"light"` by default, or `"dark"`),
`entities.show_hidden` (off by default), and `entities.selection_mode` (off by default; `off`, `follow`,
or `select_in_3d`), `navmesh.enabled`, `navmesh.preview` and `navmesh.embed_in_saved_maps` (all on by
default). Show Hidden and the exclusive Entity selection direction survive restarts. The runtime creates
the file when needed, so deleting it resets preferences and the next startup recreates the defaults.
Manual config edits are consumed at the next startup. A key from an earlier release, such as
`overrides.user_enabled`, is kept in the file but no longer read.

**Pinned assets live in their own file**, `%LOCALAPPDATA%\snapmap-plus\pinned.json`, not in
`config.json`. Pins are separate from the validated settings scalars, so a malformed pin list cannot reset
somebody's theme or other preferences. Configuration recovery follows the rules below. The frontend host moves the bytes and
does no parsing — shape and validation live in the UI, the only side that knows what a pin means — so
the worst a broken file can do is cost the pins. Writes go to a temporary file and are moved into
place, so an interrupted write cannot truncate the real one. A missing file simply means "no pins
yet"; deleting it clears the shortlist and nothing else.

The schema and registry are intentionally extensible: registered values are type-checked and repaired
individually, while unrecognized root and `settings` members survive normal rewrites. A damaged file is
backed up and replaced with defaults; a file from a newer schema is left untouched; and an I/O failure keeps
ordinary settings for the current session while warning that they were not saved. The installer treats
this file as player data and preserves it across update, uninstall, and reinstall.

## Network use

The clone makes exactly **one** kind of network request, and only on an explicit user action: clicking
**Send** in the feedback dialog — or in the crash-report dialog — POSTs the typed report (category /
title / details / optional contact), the installed version string, and one token naming the renderer the
game is running (`vulkan` / `opengl`, read from which renderer library the process loaded — nothing
else about the machine is gathered) over HTTPS to the project's feedback relay
([`feedback/`](../feedback/README.md)), which files it as a public issue on this repo's tracker. A
crash report can additionally attach the tails of the local log files, but only when its
"Attach recent logs" box is checked, and the text is anonymized first (account / profile / machine
names scrubbed — see [`feedback.md`](feedback.md)); the local crash dump is never uploaded. Nothing is
downloaded, nothing runs in the background, and nothing else is ever sent. (The installer
separately talks to GitHub Releases for `snapmap-plus update`, and to Microsoft's bootstrapper if the
WebView2 runtime is missing — both documented in [`packaging.md`](packaging.md).)

## Hook behaviors

Engine detours and resource-loader shadows the backend installs.

| Hook | What it does |
|---|---|
| Rawmap load | When rawmaps are on, load the map from the staged rawmap file — `%LOCALAPPDATA%\snapmap-plus\rawmap.json` unless **File ▸ Load Rawmap...** points it somewhere else — instead of the engine's own save. |
| Rawmap save | When rawmaps are on, mirror each saved map's JSON to the save path. That is `%LOCALAPPDATA%\snapmap-plus\rawmap.json` until **File ▸ Save Rawmap As...** or `sh_rawmaps savepath` names another file, and opening a different map returns it to the default. A file someone downloads and opens is an archive entry, so nothing about opening one aims a save at it. Off, your save is untouched and that file is left alone — the same switch governs both directions. |
| Rawmap branch answer | A map opened from a rawmap counts as a new map, so the engine's own Save asks for a name instead of replacing whichever map the rawmap opened over. Detours `idSnapEditorLocal::IsBranchMap`, the one function the editor's save menus ask, and answers for a substituted map without writing to the map. A sibling function in the image has the same body around the `map:new` check, so the scan confirms its target by the literal the callee names. `sh_rawmaps overwrite on` allows the replacement instead, for that session only, and says so when it is turned on. Loads are refused if the protective hook is unavailable unless overwrite was explicitly enabled. |
| Rawmap file menu | **File > Load Rawmap...** stages a JSON file and offers to open it in place after confirmation. **Open Rawmap as New Map** confirms before replacing unsaved editor work. **Save Rawmap / Save Rawmap As...** serialize the live map, including unsaved edits; a missing or busy editor is refused, and a map change cancels queued saves. The File menu shows both paths and lets the user release a chosen save destination or enable rawmaps for every load/save. See the [rawmap guide](https://www.snapmapplus.com/snapmap-plus-guide.html#rawmaps). |
| Overrides file-shadow | Serves map navigation, the generated Grid Room variants and the built-in default declarations through native file opens, falling back to installed originals, which it reads through the provider's own open (`read_installed_resource`). It reads no user or package files. The custom stream requires the audited native ABI. |
| Retired package payloads | A map saved or published with packages by an earlier release carries them as `smpkg.` string variables. The load path removes them before the native parse, logs one line, and the map loads as a plain map. See [Packages](#packages). |
| Baked navigation | Marked Blocking Boxes are baked into the AI navigation of each placed module copy whenever a map is converted for play, and by Build Navmesh in the editor. This covers maps started from the editor, from the SnapMap hub or map browser without opening the editor, joined in co-op, or loaded directly: the bake reads the map actually being converted, so a map edited earlier in the session cannot stand in for it. Each copy is baked separately, so a module placed several times gets navigation for each copy. Nothing new is written into the saved map; navigation is rebuilt from the marked boxes at each Play, which also means it needs Snapmap+ on the machine running the map. Maps saved by earlier releases may carry `smnav1.` shards in their map variables; these are legacy pass-through only: they are still validated, served for module copies without a bake of their own, and written back on save, and a module those shards describe that is placed more than once keeps its shipped navmesh. A copy, or one demon size of a copy, that cannot be baked keeps its shipped navmesh; its reason is logged and shown by `sh_navmesh`, which also says which map the last Play read its boxes from. `navmesh.enabled` turns the whole thing off; `navmesh.embed_in_saved_maps` turns off only the shard write-back. |
| AI Navigation on a volume | Enable **AI Navigation** and **Block Demons** on Blocking Boxes. Boxes with inherited default dimensions support navigation, including in existing maps. Green outlines preview the smallest demon size; `navmesh.preview` controls its display. **Build Navmesh** in the native Start menu and **View > Update Navmesh** run the same bake, even with preview disabled or in Blueprint mode. The added row appears with the rest of the menu when it opens and is hidden as it closes. The native action uses the game's busy modal and completion toast without saving or checking for a player start. The View menu says when a marked box has changed since, and after a build how many rooms (module copies) were built and why one was not. Map-text reads at load and after Save take an omitted size from the box's entityDef, so default-sized boxes keep their preview. Refusals are as narrow as the engine allows: a box with an unusable size, rotation or room claim, or in a room whose position cannot be read, is left out alone and named, and only a malformed room table refuses the map; a module copy that cannot be baked keeps its shipped navmesh while the others are built; each demon size is baked and refused on its own, and a size already refused at the same geometry is not baked again at Play. An unchanged map reuses the preview's smallest-demon bakes at editor Play: the map Play converts lists the same boxes in another order and with other entity indices, so reads are compared by box shape, room and flags (within 0.05 units, the float round trip of a converted read), and only a moved, added, removed or reflagged box takes a new bake. Every successful bake of every demon size is kept in a bake cache keyed per room copy by its module, its shipped navigation file, the traversal table and the same shape comparison, so an unchanged copy is served at the next Play, after a reload and on a launch outside the editor, and an edit re-bakes only the copies it touched (the preview reuses the cache the same way). The cache holds at most 128 MiB and 1,024 payloads, evicting the least recently used and never one the current Play uses; `sh_navmesh` reports its use. Where one area would pass the engine's 256 outgoing routes, the boxes contributing most give up just enough of their routes there; where the tree would pass the engine's 128 depth, the copy keeps three quarters of its floors at a time until it fits. Where one copy's navigation file would pass 16 MiB (a product bound on memory: the engine's loader has no size check, and a bake is served from memory rather than stored in the map), the copy keeps each area's shortest routes and drops its longest leaps to a per-area budget chosen to fit, then sheds floors as for depth; a 600-box Grid Room copy measured 9.1 MiB. Boxes that lost something are drawn amber. The toast reads **Navmesh Built**, **Navmesh Partly Built** ("3 of 4 rooms built. <room> (copy 2): <reason>") or **Navmesh Build Failed**, the last only when nothing could be built or the map could not be read. Ceilings, each leaving out the excess with a note: 65,536 boxes and 8,192 rooms per map (about 11 MB and 1.2 MB of tables at most), 2,048 boxes and walkable pieces per room and demon size (about 11 MB of scratch per bake), 131,072 edge strips (about 26 MB), 8,192 edge segments per area. Climb approaches clear the rotated solid, and traversal candidate storage grows with geometry. Refused bakes log their reason. The [player guide](https://www.snapmapplus.com/snapmap-plus-guide.html#ai-navigation-on-blocking-boxes) explains clearance and authoring limits. Backend marker, geometry and bake rules are documented in their source files. |
| Strids injector | Combines the user's `strings\strids.json` with the built-in strings. A refresh publishes the owned values without duplicate rows, retaining storage needed by live native readers. See `src/backend/strids.h`. |
| Text-field clipboard | **Ctrl+C / Ctrl+V in the editor's text fields** — every free-text property (datapad and transmission message bodies) plus the text-backed int / float / vec3 / size inspectors. Copies the selection, or the whole field when nothing is selected; pastes over the selection, or at the caret. Multi-line text pasted into a single-line field collapses each break to a space; datapad bodies keep their newlines. Respects the field's own character cap. Vanilla has no text clipboard at all here (only the dev console accepts a paste) — and it is genuinely absent rather than disabled: the engine's SWF text-edit key handler has no Ctrl branch whatsoever. Entity-level Ctrl+C/Ctrl+V and Ctrl+X are untouched. Exceeds the original. |
| Boot pacing | DOOM reaches the SnapMap hub in about 19 s on Vulkan and 22 s on OpenGL instead of about 158 s and 141 s (warm boots on the test machine). The engine's startup resource preload was paced by waits, not by the disk: the load-screen video slept 10 ms at a time while holding the lock the file-stream thread needs, and the main thread polled every read with `Sleep(1)`. `boot_pacing.c` releases that lock for the video's sleep and lets the poll spin and yield before it sleeps. The load-screen video and progress ring look and play as before. Both sites are byte-checked and patched about 0.1 s after launch; when a site does not match, it is logged and left alone. Map loads run through the same two sites. |
| Fault handling | The clone replaces the original's two kill-detours (which terminate DOOM) with the recover-in-place fault-shield (see [`fidelity.md`](fidelity.md)). |
| SuperScript table | Merge the parked `cs_*` SuperScript objects into the engine's eventDef enumerate/lookup/dispatch paths. |
| Math acceleration | An optional SIMD/threading accelerator for engine math; not required for parity (a perf feature, not a user-facing one). |

## Also in the backend

Beyond the manifest entries above, the backend `XINPUT1_4.dll` also bundles **cvar-unlock**
(developer cvars and `+<cvar>` launch options), a fixed **XInput proxy** (controller input keeps
working, connected or not), and the resident **fault-shield**. See [`README.md`](../README.md)
and [`fidelity.md`](fidelity.md).

## Not user-facing

A few internals are present for parity but are not features you drive directly:

- The 5 parked `cs_*` SuperScript objects (`cs_invert_activator`, `cs_flyto_target`,
  `cs_test_equipped_weapon`, `cs_clone_activator`, `cs_dash`) are disabled gameplay-cheat objects
  in this build; they have no live surface. (The original's six `cs_*` tuning cvars for this
  cluster are not registered either — see [unsupported SnapHak features](fidelity.md#unsupported-snaphak-features).)
- `snaphak_ext` (a VFS / shader-compile library) is dormant, and `snaphak_algo` (SIMD + threading)
  is an optional performance accelerator — neither is required to match the original's behavior.
