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

DOOM's native Settings > Properties panel includes a per-map Unlock View
Distance toggle on the Snapmap+ tab. Off is the unmodified game: each
environment keeps its own far clip and fog, so modules, Set Environment and
Player Set Environment nodes and campaign environments decide them. On sets
the far clip to 200000, the largest the game accepts (not an infinite one),
and stretches every environment's fog by the same factor, in the editor
(every camera, Blueprint included) and in Play: with r = 200000 / the blended
native distance (at least 1), the view's fogStart, fogEnd and fogStartDistance
literals are multiplied by r and its fogScale and fogDensity divided by r each
frame, so each Environment FX keeps its look while its fog saturates r times
farther out; colour, maximum opacity and lighting parms are kept. A module on the Default Environment FX (snapmap/base) draws to
60000 units in the editor, with dark fog from 32000, but only 8192 in Play;
On gives the same whole-room view in both. The editor's own camera cvars are
not touched. Apply commits the draft to the map: On stores the variable
`smp.render.v1`, Off removes it, so a map turned off again saves exactly as
the unmodified game writes it. Both editor playtests and direct saved-map loads
use it, with bindings for OpenGL and Vulkan. Maps saved by earlier Snapmap+
builds with a View Distance number read as On unless the distance was the old
default 8192, whose variable is removed at load. Fog is not a map setting;
older maps' fog values are ignored. See the
[view distance guide](https://www.snapmapplus.com/snapmap-plus-guide.html#unlock-view-distance).

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

## Mods

A mod is a folder in `%LOCALAPPDATA%\snapmap-plus\mods\` holding `mod.json` and an `assets\` tree of
already-cooked idTech 6 files at the paths the game asks for. Converting assets from other games is
Doomforge's job, not this product's. New names live under `mods/<id>/`; a stock file the mod replaces
is listed in `shadows`. A shadow may keep a name the stock text already carries and the game ships
without (the stock hands model names six animations no archive holds): the builder reports it and
does not refuse, since the engine already tolerates those names.

- `mod.json` is yours except for one member, `"snapmap-plus"`, which the product writes when it
  builds the folder (the files, their hashes, the bundle hash and whether it verified). Everything
  else in the file (id, name, version, description, author, strings, requirements, hud, any key of
  your own) is never reformatted or reordered. A `package.json` works too and becomes `mod.json` at
  its first build. Everything about mods is in `mods\`: the folders and one product file,
  `snapmap-plus.json` (publish claims, the migration record, the build cache).
- A folder you drop into `mods\` keeps its name and is built when the Mods tab opens (or on
  `sh_mods rescan`); a folder whose files or `mod.json` changed is built again the same way.
  The rescan (the Mods tab's refresh, `sh_mods rescan`) also brings what changed into the running
  game: every decl whose served bytes changed, and every served decl that inherits from it, is
  parsed again, and changed models and images are reloaded in place, so an edit to a mod's FSM,
  entityDef or animWeb applies at the next Play without a map reload or a restart. New, changed
  or removed Toybox rows appear or disappear in the open editor's ADD OBJECT browser at once (the
  palette is rebuilt, unless an entity or wire is held), with new `strings` names, and new decls
  of a mod already in use are registered. During Play the changes wait for the return to the
  editor. New names of a mod no map has used yet register when a map that uses it loads, a changed
  movie applies at the next map load, and changed sound banks, animations and other raw files
  besides models and images, or a stock decl no mod serves that inherits a changed one, need a
  restart. A shadow a newly installed mod adds for a decl, model or image the game already holds is
  treated like a changed one and takes effect the same way. The rescan's result and the backend
  log's `MODS: refresh:` lines say which applied.
  `sh_mods build "<folder>"` does it at once; `sh_mods install "<folder>"` copies a folder from
  elsewhere into `mods\<id>\`, verifies it and makes it available at once. `sh_mods list`,
  `sh_mods verify` and `sh_mods status` report what is installed, whether its files still match,
  and what it serves. The Studio window's Mods tab runs the same actions and shows what the open
  map needs (see [The Studio window](#the-studio-window)).
- A mod file that is an unchanged copy of the game's own content is not stored or uploaded: the
  bundle names it as a stock pointer to the installed file (any archive, the `.patch` store
  included, or a loose file such as a `.pages`), and it is read from your install when the game
  asks for it. `sh_mods build` and `sh_mods install` say how many files are installed content; the
  Mods tab shows them as "from the installed game". A descriptor's `stock` object
  (`{"<request path>": {"source": "<installed path>"}}`) names installed content under a mod's own
  name without copying it. A player whose game lacks that content, or holds a different revision
  of it, is told so and the mod is not installed.
- Installed mods are served after the product's own files and before the game's archives. A mod's
  new editor rows appear in the palette when the editor is first opened; a mod installed after that
  needs `sh_mods palette rebuild`. A mod that fails a check is skipped on its own with one logged
  reason; the rest still load.
- A mod's replacements of the game's own files (its `shadows`) and its `requirements` apply only in
  maps that use it: while such a map is open in the editor or being played. A map uses a mod when it
  places something the mod provides or when the mod was added to the whole map (below). Opening the
  map brings the mod's replaced player definition, HUD movie or material in (decls are parsed again,
  models and images reloaded), and leaving it for the hub, or opening a map that does not use the mod,
  gives the game's own files back. With no map open nothing a map-scoped mod replaces is served. The
  mod's new content (its Toybox rows and everything under `mods/<id>/`) is not affected; a mod that adds
  Toybox rows also keeps its requirements in every map, since its rows are offered in every map. A mod
  can opt out with `"scope": "always"` in its `mod.json` (`"map"` is the default; any other value is
  refused when it is built), and a mod without the key that replaces something the editor itself uses
  (an editor settings, palette or property-inspector decl, or the editor's own movies) is treated as
  `always`, with one log line. A mod without the key keeps the bundle hash it had. When two installed
  mods replace the same file (a renamed or forked copy, for example), a map that uses either one gets
  the replacement, from the mod it uses. Opening the Settings screen from the editor's Start menu keeps
  the map's mods applied. `sh_mods status`, `sh_mods doctor` and `sh_mods map` print each mod's scope
  and whether it is active in the open map, and `sh_mods doctor` prints which mod serves each replaced
  file for the open map. A published map played from the hub keeps its mods applied until the next map
  opens.
- A saved map records each mod a placed entity needs as one `smpmod1.` string variable, and the
  next load activates exactly those mods. A mod can also be used by the whole map without placing
  anything (the Mods tab's **Use in this map**, `sh_mods map add "<id>"`), for a mod that only replaces
  game files or sets requirements: the map gets a row for it marked as map-level, which stays through
  saves, reloads, Play and undo whatever the map places, publishes and downloads like any other, and
  carries the search tag `mods`. **Remove from map** takes it out again. A client without the product,
  or an older one, keeps the row as an inert string; an older product drops it at its next save. The game and clients without the product read the rows as
  inert strings. A mod the map names but this machine does not have is logged as not installed and
  nothing activates for it until it is fetched. Uploads happen only when a map is published; plain
  saves never upload, and an exported rawmap or prefab ZIP carries its mods itself, each as the
  folder it is installed as.
- A rawmap of a map that uses mods is shared as a `.zip`: every rawmap save of such a map writes
  `<name>.zip` beside the `.json` (for the default file, `rawmap.zip`), holding the map as
  `<name>.json` and every mod it uses exactly as it sits in your mods folder (`mods/<id>/mod.json`
  and `mods/<id>/assets/...`), and one toast and one console line say to share the `.zip`, because
  the `.json` alone does not load for anyone who lacks the mods. **Save Rawmap As...** offers `.zip`
  first for such a map. Opening a `.zip` (**Load Rawmap...**, `sh_rawmaps load`) is the ordinary
  install of each mod folder it carries: mods already installed are left as they are, a mod you have
  in another version is installed beside it, and the map is refused, with nothing installed, when you
  have the same version of a mod with different files. A `.zip` saved by an earlier release is
  refused with a request to save it again; a refusal in **Load Rawmap...** (a prefab's export, an
  earlier release's `.zip`) is also written to the console ("The rawmap was not loaded: ...") and
  the logs. **Load Rawmap...** and **Save Rawmap As...** each open in the folder last used for them
  (`dialogs.rawmap_load`, `dialogs.rawmap_save` in `config.json`), the rawmaps folder
  (`%LOCALAPPDATA%\snapmap-plus`) the first time. A rawmap is never opened while a mod it names is missing: when the
  mod store has the missing mods the game asks, as for a My Maps map, and downloads them and opens
  the map; when it does not (never published, or released), the map is not opened and a notice and a
  console line name each missing mod and its version and say to ask the map's author for the `.zip`.
- Publishing a map that uses mods uploads those mods to the mod store first, so other players can
  download them; the first time, the game asks once (the `mods.upload_confirmed` setting records
  the answer), and No leaves the map unpublished. A map over the map service's 10 MB limit is
  refused before anything is sent. A published map that uses mods carries the search tag `mods`,
  written into the map's tags by the publish's own save when it is missing, because players
  without Snapmap+ cannot play it.
  Unpublishing a map releases its mods on the store; deleting a map from My Maps does not while its
  published copy stays available, and releases them only when the game reports that copy is no
  longer published (for example after an unpublish from another machine). A published map over the
  limit cannot be copied to My Maps.
- The store the product talks to is the `mods.store` setting (default `https://api.snapmapplus.com`;
  https, or http only for a loopback host). A change applies to the next request, not the next launch.
- Playing a published map (or a My Maps map) that names a mod this machine does not have asks first,
  in the game's own Yes/No dialog, naming the mods and their size. Yes downloads only what you
  approve in the dialog, verified by hash, installs it, and starts the map; No leaves the map
  unloaded and says which mod it needs. Joining a lobby asks the same way and checks that the host
  still runs that map before going on. Maps whose mods are installed, and maps without mods, start
  exactly as before.
- Maps saved or published with packages by v0.2.1-beta.6 to beta.19 are handled on load (see
  Retired package payloads below): a complete package becomes an ordinary mod folder `mods\<id>\`
  (unless an installed mod already serves the same content) and the map gets a manifest row for it;
  anything else is removed. Files a mod cannot carry (for example the
  `.materials.txt` sidecars beside models, or `.bswf` movies outside `generated/swf/`) are left out
  of it and counted in the log, so the rest of the package still loads.
- A mod can carry cooked Flash movies (`.bswf`, with their `.bimage` atlases) under
  `generated/swf/`: a replacement for a stock movie is listed in `shadows`, a new movie lives under
  `generated/swf/mods/<id>/`. They load whenever the game asks for the movie, in the editor and in
  Play; a replaced movie the game already holds changes at the next map load, and one held since
  startup after a restart.
- A mod's `requirements` set console variables and run console commands while a map that uses it is
  open or played (or while it is installed, for a mod whose scope is `always`):
  `"requirements": {"cvars": {"<cvar>": <value>}, "commands": {"activate": ["<line>"], "deactivate":
  ["<line>"]}}`. Any cvar works at any value (a string, an integer, or `true`/`false` for 1/0; write a
  decimal as a string), up to 32 per mod; each command is one console line without `;`, up to 16 per
  list. Snapmap+ keeps no list of allowed settings or commands: installing the mod accepts them. The
  cvars are set when a map that uses the mod loads, checked again before Play, and put back to the
  value they replaced when that map closes, the mod is uninstalled or a rescan no longer finds it; the
  `activate` lines run once when the mod's requirements start, the `deactivate` lines when they stop.
  A cvar the game refuses (read-only, or settable only on the command line) or does not have is
  logged, the mod's content stays unloaded, and `sh_mods status` and `sh_mods doctor` show each
  requirement's state for that mod: live, not active for this map (also when another mod holds the
  same value), refused with the reason, or restored after it stopped. When two mods ask different values for one cvar the later
  install loses, both are named in the log, and none of its requirements apply. The Mods tab lists
  them under Requirements.
- A mod's `hud` setting chooses how the ammo panel shows a weapon: `"ammo_display": "weapon"` for a
  weapon decl lets the panel show the weapon's own capacity (the campaign pistol's infinity symbol
  and full bar) instead of SnapMap's forced number. It changes only the display, applies to every
  installed mod at startup, and two mods asking different things for one weapon turn the setting off
  for all weapons, with both named in the log.
- A mod can carry sounds: a sound decl under `generated/decls/sound/mods/<id>/` plays the Wwise
  event its name gives (`<stem>` plays `play_<stem>`), so it can reuse any stock sound without a
  bank, and a mod's own Wwise bank (`sound/soundbanks/pc/<id>_<name>.bnk`, built with Wwise or
  snaphak-re's `make_bank.py`) is loaded into the game when the editor is first opened or a map
  loads, and right away when the mod is installed during a session. Entities the mod adds can name
  those sounds (`soundShader`). A changed bank takes effect after a restart. Music, switch and
  blend containers and streamed media still need Wwise itself.
- A mod can serve a SnapMap module, including a multiplayer map's arena: a module decl at
  `maps/modules/mods/<id>/<name>.decl` with its `snapModuleInfo`, and stock pointers for the render
  model, collision, navigation, sound BSP (`.sbsp`), light grid (`.ambientsh`), occlusion tome,
  light probes and virtual texture (`.pages`) under the names the game derives from the module.
  `.sbsp`, `.ambientsh`, `.tome` and `.pages` files are accepted only as pointers to the installed
  game, never carried. A material the multiplayer map uses and SnapMap's archive lacks is pointed at
  under its own name and needs no `shadows` entry. The module has no tile in the Add browser: a map
  or a module prefab that names it loads it into its own private palette, as resized Grid Rooms
  are, and it then moves, saves and plays like a stock module. Saved maps name its mod in their
  `smpmod1.` rows.
- A mod can carry its own virtual-texture family (the page format a material streams its
  diffuse, specular and normal detail from): the members `virtualtextures/mods/<id>/<family>_sq<N>.mega2`
  (N 1 to 16), and materials naming it with `landpagefile "mods/<id>/<family>"` and an explicit
  `virtualmapping`. The family is registered with the game when the mod is installed (or, for mods
  present at startup, when the editor is first opened or a map using it loads), before any of its
  materials is read, with no console command; `sh_mods status` and `sh_vt list` show it. Each family
  takes one of the game's 30 usable page-source records and about 14 MB of memory; a family that
  finds them all in use is not registered and its materials draw with default images. On OpenGL a
  family is registered only when the graphics driver offers AMD sparse textures; otherwise the log
  says it cannot be served. Removing the mod releases the family, unless the editor or a map has
  been opened since it was registered, in which case it stays until DOOM exits. The format a tool
  must write is in [vt-baker.md](vt-baker.md).
- The first start of this release converts a mods folder of earlier releases in place:
  `bundle.json`, `.state.json`, the shared `blobs\` folder and `legacy\` go, each mod keeps its files
  in its own folder with a `mod.json`, and `claims.json`, `publish-links.json` and `.migrated.json`
  are folded into `snapmap-plus.json`; one `MODS: store:` log line per folder says what happened.
  Then, once, a package a map delivered is removed when an installed mod has its id or provides
  every file it provides (maps naming it load the installed mod), a mod folder left alone with its
  id is renamed to the id, and no folder is named after a generated version.
- The first start of this release moves the packages in the retired
  `%LOCALAPPDATA%\snapmap-plus\overrides\` folder into `mods\`, once. Each folder there with a
  `package.json` or `mod.json` becomes `mods\<that folder>\` with `mod.json` (its requirements,
  strings and hud kept; a pack without an id is named after its folder); each package a map
  delivered (`overrides\map-<16 hex>\<package>`) becomes `mods\<id>\`, which a later load of that
  map reuses. A folder without a descriptor that holds exactly one package folder becomes that
  package's mod; other folders without a descriptor, ZIP files and loose files are listed as skipped
  with the reason, and `.bswf` movies outside `generated/swf/` are counted when left out; two folders with the same id are both
  kept, each in its own folder. A folder over 512 MiB or 8192 files is skipped, and one
  larger than a mod can be (4096 files, 256 MiB) is refused. Nothing in `overrides\` is changed or
  deleted, and you can keep or delete it afterwards; an empty `override-backups\` folder is removed.
  Small folders are done during startup and the rest in the background. The outcome is in the log's
  `MODS: migration:` lines; once the move completes, `mods\snapmap-plus.json` keeps only when it
  ran, from where, and the folders it skipped or refused. An `overrides\` entry added or changed
  afterwards shows the **Migrate** button again; `sh_mods migrate --again` repeats the move.

## Map behaviour: String Set commands

A map carries a behaviour as stock data: a **Set (String)** logic node whose value is a literal
starting with `smp:`. When a signal reaches the node, Snapmap+ runs the command on the player who
caused the signal (the activator; a demon or object the player owns counts as that player), prints
one `SMP:` console line, and then lets the node store its string as usual. A game without
Snapmap+ only stores the string, so the map loads and plays everywhere and nothing else happens.
Commands run only on the host of a SnapMap session; a client never runs them. `sh_smp off`
switches them off for the session.

| Command | What it does |
|---|---|
| `smp:event <eventName> [args...]` | Fires any engine event on the activating player at once. The arguments are read by the event's own signature: integers (an enum takes its number), floats, text, `1/0`, `true/false`, `on/off` or `yes/no` for a bool, and an entity name. `sh_event info <name>` prints a signature. An unknown event, a wrong argument count, a value that does not parse, an argument type it cannot build (vectors, colours, declarations) or an activator that is not a player prints one line and does nothing. Example: `smp:event forcePraetorHandsAndBody 1` swaps the player's hands to the Praetor slot, which on the SnapMap player draws no arms, and `... 0` brings the multiplayer glove back. |
| `smp:doomhands <0\|1>` | Sets the player's Doom-hands switch, the per-player form of `g_weaponDoomMPUseDoomHands`, and rebuilds the hands and body. With a mod that points `handsModelMPUseDoomHands` at another model (the `alex.bare-hands` example shows bare arms) the hands change at once. A player who spawned with the multiplayer hands keeps that armour's forearm attachment drawn over the new hands; a respawn resets the switch to the map's setting. |

For a whole map, the switch needs no command: a map whose **useSPModels** setting is on gives the
player the Doom-hands slot at every spawn while fewer than two players are in the game, the same
as the cvar, with or without Snapmap+ (the setting is stock map data; what else it switches to single-player models was not surveyed). Arguments written in quotes keep their spaces (`"two words"`).

## Console commands

Registered with the engine command system; run from the DOOM console.

| Command | What it does |
|---|---|
| `sh_rawmaps` | Raw map files: state, paths, load, save. Run it bare (or `sh_rawmaps status`) to see what is set and what the last save wrote, including the `.zip` beside it for a map with mods, or `sh_rawmaps help` (or `?`) for every verb. `load` takes a `.json` or a `.zip`, `list` shows both, `savepath` chooses where saves go, `overwrite` chooses whether a rawmap you open may replace the map it opened over. |
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
| `sh_smp` | Runs a String Set command on `player1` during a playtest, as if a Set (String) node had fired it: `sh_smp smp:event forcePraetorHandsAndBody 1`. `sh_smp on`, `off` and `status` switch String Set commands for the session (on by default). |
| `sh_event` | The live engine event registry: `sh_event info <name>` prints an event's number, argument format, types and names; `sh_event list [filter]` prints every event (1611 on this build), or those whose line contains the filter. |
| `sh_playerevent` | Diagnostic: fires one player event with no argument or one int/bool on `player1`, at once or queued (`process` / `post`), and prints the hands model slots before and after; `hands` prints them alone. |
| `sh_help` | List every Snapmap+ console command and cvar with its description in one place. |
| `sh_mods` | Installed mods (see [Mods](#mods)). Bare or `status`: what the serving table holds and serves. `rescan` (or `refresh`) rereads the mods folder; `palette rebuild` adds rows of mods installed after the editor first opened. `rescan` also builds every mod folder whose files or `mod.json` changed and applies what changed to the running game, printing one `MODS: refresh:` line per category: decls re-parsed by type, models and images reloaded, new names that need a map reload or `palette rebuild`, changes that need a restart, and changes left pending during Play; `status` says when changes are pending. `build "<folder>"` checks a mod and writes the `snapmap-plus` member of its `mod.json`; `install "<folder>"` copies it into the store as `mods\<id>`, verifies it and rescans; `list` shows every mod folder and its state; `verify "<folder>"` rehashes one installed mod (an id, or `id@version`, checks every folder declaring it). `copystock "<engine path>" "<file>"` writes the bytes of an installed game file to a new file, so a stock mesh or animation can be placed in a mod under a new name; it never replaces a file. `migrate` moves the retired `overrides\` folder into the store again in the background (`--again` repeats a completed move; see [Mods](#mods)). `map` prints the open map (name, saved-map id, last save, published and uploaded state, search tag) and each mod it uses with its placed entities and the wires and Timelines pointing at them, whether it is used by the whole map, and where each mod applies. `map add "<id>"` uses that mod in the whole open map without placing anything (the Mods tab's Use in this map). `status` and `doctor` print each mod's scope and whether it is active in the open map. `map remove "<id>"` prints what removing that mod from the open map does, and with `--yes` does it (the Mods tab's Remove from map); `uninstall "<id>"` prints what uninstalling does (the open map first, the Toybox rows, the folder moved to `removed-mods\`) and does it with `--yes`; `removed` lists the folders in `removed-mods\` with their ids and sizes, `removed empty` prints what emptying deletes and `removed empty --yes` deletes them for good, and `removed restore "<id>"` moves one back into the mods folder and rescans; `rebuild "<folder>"` builds a mod folder again and rescans (the tab's Rebuild). A folder or file without a drive is taken relative to the mods folder. |
| `sh_vt` | Virtual-texture diagnostic. `status` and `list` show the game's page-source records, marking those registered for an installed mod's family; `sync` registers the installed mods' families now; `register "<name>" "<path>" [flavor]` and `release "<name>"` create and remove a record by hand for format experiments (never a mod's); `decode <family> <level> <x> <y> <out>` decodes one stored page to its five planes. |
| `sh_navmesh` | Report the baked AI navigation the loaded map is serving — for each marked module copy, each demon size's outcome (built, built with parts left out, keeps its shipped navigation, or not baked yet) with its reason and which faces of each marked box carry a floor climb for how many demons (or the commonest reason none does), and every box the map read left out with why; it also says which map the last Play read its boxes from. Leads with how many map loads have reached the navigation table this session and what the last one found, so a map that quietly brought none is distinguishable from a load that never happened. |
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
The backend loads it from `<DOOM>\snapmap-plus\snapmap-plus-ui-13954591.dll`, a path that names the
supported build; a `snapmap-plus\snapmap-plus-ui.dll` beside it is the 2018 release's frontend, which
release bundles also carry (see [packaging](packaging.md#the-bundle)), and is never loaded.
Its WebView2 profile is regenerable cache in `%LOCALAPPDATA%\snapmap-plus\cache\webview2\`; the first
start of this release renames an older `snapmap-plus\webview2\` folder into it, and the installer never
backs up or copies `cache\`.

| Surface | What it does |
|---|---|
| Window shell | The Win32 host window + the manual 30 Hz think-loop; native Windows 11 rounded corners and drop shadow retained around the custom captionless chrome through the same one-pixel DWM frame treatment as snapmap-midi; a menu bar with a persistent light/dark theme toggle (seeded before the page is first shown, so a saved dark theme never flashes light); and three footer groups for the connection dot/version plus selection, editable live camera X/Y/Z coordinates with a Lock position checkbox, and Updated/help. Camera feedback is sampled at the frontend's full 30 Hz cadence and publishes only when a coordinate changes. Committing a coordinate edit moves the editor camera, while Lock position pins all three coordinates until released. The green dot is the sole visible connection indicator. Entities, Prefabs, and Timelines share one adjustable, keyboard-accessible vertical-divider position, equal 40px header tracks, common pane insets, and inset header/toolbar separators while their background fills remain continuous; the Assets tab is exempt and retains its purpose-built three-column layout without scrubbers. At compact widths the camera controls reflow onto a second row, while the page remains clamped to the WebView client area so header and footer never scroll out of the viewport. Expanded Decl Text uses a client-bounded modal and dims the stationary shell underneath. |
| Entities tab | A filterable entity list (multi-select, a persistent Show Hidden toggle, and one persistent selection direction: Follow Selection or Select in 3D; right-click for Copy ID / Fly to entity / Delete / Push to stack 0 / Clear stack 0) plus the Entity State panel: classname / inherit / displayname fields and the Decl Text editor — line numbers, syntax coloring, structural lint, advisory schema checks, and a modal expanded mode that reparents the live editor without losing selection, scroll, or undo state. Diagnostic rows remain click-to-jump targets with pointer cursors but no hover highlight. Long entity rows ellipsize inside the same pane inset used by Entity State instead of painting into the divider, with the complete path/name available on hover. "Save to Decl" commits the edits in memory. Setting `isNetworkStatic` in an entity's JSON changes the network cost the editor counts at once, as the native Static toggle does, and turning Static off is refused, with the edit undone, when it would put the map at or over the editor's limits. Clicking a blank structural surface outside Entity State clears the current page and 3D-editor selection; the entire Entity State pane remains protected inspection space, including its blank padding and expanded Decl Text mode. SnapMap's own built-in filter/droppable helper entities are excluded from the list and from every entity picker (dev-layer-only; a mapper's own filters are unaffected). |
| Keyboard paging | ArrowUp/ArrowDown page the Entities, Timelines and Prefabs lists, and — while their dropdown is open — the Inherit / Classname combos and the Timelines "Runs on" picker. Filtering still works alongside it; text editors and rename fields keep normal caret movement. In the asset browser, paging onto a **file** row also selects it (debounced, so holding the key down previews only the row you stop on); **folder** rows still need Enter/Space, since opening one replaces the whole list. |
| Native selection parity ("Select in 3D") | Entities selected from the list behave exactly like ones clicked in the 3D view: an empty-space click deselects them, Delete removes all of them, Move works, and the bottom-bar controls apply — single or multi-entity, including switching between entities. Deselecting natively also clears the list highlight. Previously only the now-removed explicit Deselect button worked, and Delete/Move misbehaved (Move could soft-lock the game); the cause was that the editor's own mode state was never told a selection existed. |
| Native Delete from the Entities list | Right-click Delete runs the editor's own Delete Selection on exactly the chosen entities, with the same rules as the Delete key: objects the editor protects, room doors and caps, and maps that restrict deleting are refused, and the deletion is one undo step ("Delete Selection"). Whatever the 3D cursor points at is never added. The list keeps its selection and shows why when a delete is refused; success is reported only after the engine has removed the entities. |
| Fly to entity | With one entity selected, the crosshair button in the Entity State header or the right-click **Fly to entity** item puts the editor in Object mode (from Blueprint it reproduces Tab: the three ModuleMode fields and `GridSetEditorState(2)`) and places the camera in front of the entity, looking 20 degrees down at its world bounds centre from a distance that scales with its size (128 to 512 units), so the centre-screen cursor rests on it. The walk camera sinks to the floor under `snapEdit_fpsGravity`, so the request keeps re-aiming for up to 60 frames until the eye settles. It refuses with a toast while a panel is open, while something is held, or when the entity cannot be located; Lock position is released first because it would pin the eye. Apply kind 4; serviced on the game thread by the EditorFrame detour. |
| Selection guard while holding | Selecting from the Entities list is refused (with an explanatory toast) while you're grabbing an entity or holding a staged prefab. The engine's Escape/cancel path restores a snapshot keyed positionally to the selection that was live when the grab began, so changing the selection first makes Escape swap entity pointers inside the live map — duplicating entities, deleting others outright, and freezing the game. A pre-existing engine bug (reproduces on released builds); only cancellation triggers it, never accept. Placing a *new* palette entity captures no snapshot and is left unrestricted. |
| List-assembled group grab | With a selection pushed from the Entities list, grabbing any one of those entities in the 3D view grabs **all** of them — not previously possible. Practical use: browse a logic chain in the 3D view, add the other entities you want to bring along from the Entities list, then grab the node you're on and move the whole group. Assemble the selection before grabbing; the selection guard refuses list changes while a protected grab or staged prefab is active. |
| Prefabs tab | Save and load selection prefabs as JSON files under `%LOCALAPPDATA%\snapmap-plus\prefabs\` — one folder level with rename/delete/drag-between-folders. Folders start collapsed; the ones you open stay open across refreshes, tab switches and restarts (setting `prefabs.open_folders`), a filter shows every matching folder open, and a guide line runs from under the caret down each open folder's rows. Shift-click selects a range of rows and Ctrl-click toggles one, folders included; a selection of more than one prefab blanks Prefab Details and is a move set: drag it onto a folder or onto blank list space (the root), and every prefab lands in one host operation with one toast. A selected folder stands for its prefabs, so dragging a folder onto another merges its contents and leaves it empty. Delete or the right-click menu removes the whole set after one confirmation; per-prefab description + tags (a prefab is one file of plain JSON: Create removes the comments the engine's serializer writes before each member, and the description and tags live in its own product-owned `snapmapPlus` block after the engine's JSON, which every reader strips before the engine sees the file; a prefab saved before this shows its `<name>.meta.json` sidecar's details read-only, and the first edit writes them into the file and leaves the sidecar as it was; the filter box matches tags across folders); compact Lucide Create-from-Selection and New-Folder actions (Create, and the Create dialog's confirm, stay disabled until the editor has a selection: selected entities in Object mode, selected modules in Blueprint, with the tooltip "Select entities first" or "Select modules first"); and blank-list-space deselection through the shared selection router while Prefab Details remains protected. Create guards the engine's grab frame: PrefabPopulate anchors the prefab on the view-centre line clipped against the hovered entity, keeping the clip midpoint even on a miss, so a cursor hover the centre line does not cross used to save a meaningless `grabDistance` (about -3e10) and float-collapsed positions. A detour on that grab-frame helper (VK 0x1412164d0 / GL 0x141215920, armed only inside Create) keeps a hit, and otherwise re-anchors the frame on the hovered entity's box centre with its own axis and a `grabDistance` equal to that centre's depth along the view (32 to 113175 units). The engine then stores every entity exactly relative to it; paste reads only `grabAxis`, `grabDistance` and `cameraYaw`. The Create toast says "grab point corrected". The engine's copy check also needs the hovered entity to be one of the selected ones; Create tells a cursor over an unselected entity apart from an empty selection and says to hover a selected one. Create refuses only when no finite frame can be built, or when a backend without the detour produced a file whose `grabDistance` or positions are absurd. Selecting a prefab reconstructs its saved transforms in an interactive, orbitable 3D preview above Name, placed through the prefab's `grabAxis` as the paste places them, so a prefab anchored on a tilted, rolled or mirrored entity previews level. Sparse saved rotations and dimensions are composed over their identity and installed entityDef defaults (16/128/64 for a Blocking Box whose decl cannot be resolved), so rotated props and thin/non-uniform blocking boxes retain their in-game placement and size. Blocking Boxes ticked for AI Navigation are tinted green, with their count beside the entity count; placing a prefab that brings them in requests the navmesh update automatically once they are dropped. The preview carries no orbit hints (dragging orbits, the wheel zooms, a double-click frames), a key button at the bottom left opens the colour legend upwards (at the smallest preview height the status line steps aside while it is open), `N entity/entities` is bottom-right, and Name, Description, and Tags use the same field typography as Entity State. Geometry is decoded on demand from the user's installed resource containers with neutral lighting: multi-surface props and pickup-spawner contents use their real models; demons (red) and player starts (pink) draw the md6 model their palette row draws in the editor, in its bind pose without damage overlays or attachments, clustered to at most 65,536 triangles; blocking shells, invisible triggers, decals, interactables, full-size logic hexagons, and half-size input/output circles and filter diamonds receive distinct scene treatment. An entity whose model cannot be resolved or decoded is drawn as a box (a demon or player start at its palette row's size), and the status line then reads "N models unavailable" instead of "Interactive preview". The Cartesian floor grid extends beyond the scene and fades under a circular mask. No textures or game-resource bytes ship with or persist in Snapmap+. "Load / Place" stages the prefab and queues the editor's native paste action when its guards allow it. Once the game has decided, the toast says which: the prefab is in hand (position it, Space places it, Esc cancels), or it is staged but not in hand, with the reason, and Ctrl+V in the 3D view places it. It degrades to stage-only whenever the engine itself would refuse — copy/paste disabled or unavailable, not in EntityMode, already holding something, hovering an entity, or a selection that could not be cleared. A staged prefab also **survives a Play round-trip and a map change** — come back and Ctrl+V still works, matching the engine's own Ctrl+C clipboard. See [allocation lifetimes](architecture.md#engine-data-ownership). **Module prefabs** (Blueprint): with one or more modules selected, Create saves them with every entity inside them, their door and cap slots, the wires and entity references that stay inside the set and the map variables those entities use, as an `idSnapMap` file beside the object prefabs; a wire or reference that leaves the set is dropped (references keep their slot as unset) and counted in the file's `snapmapPlus` block, which also records the module count, entity, door-slot and variable counts, AI Navigation boxes, environments and each module's footprint relative to the first module. Load / Place, the list and Prefab Details all decide the kind by the file's top-level `~type` (`idSnapMap` a module prefab, with or without its block; `idSnapEntityPrefab` an object prefab): a module prefab places only in Blueprint, merging into the open map with fresh entity ids and variables, landing under the cursor and grabbed like a native Duplicate, so the group follows the cursor, rotates, and is placed with Space (one "Duplicate Modules" undo step) or removed with Esc. A placement that would put the map over the editor's limits is taken back and refused, and an accepted one requests the navigation update. Before anything is merged, Load / Place checks every module the file names against this editor: a room that is not in its palette (a DLC room the player does not own, or a mod module they do not have) refuses the placement with nothing sent to the engine, naming the rooms in a toast and in one console line (`needs module <name>; this room is not in your palette (DLC or a mod you do not have)`). Selecting a module prefab runs the same check, and Prefab Details shows a warning line with the missing rooms above Limits before Load is pressed. Grid Room sizes and modules an installed mod serves count as present. **Limits**: under the preview, a Limits section shows what the selected prefab would add to each of the editor's five meters (NETWORK, OBJECTS, MEMORY, DEMONS, DECALS) as a percentage, the raw counts in a tooltip, the map's meter after placing it (blue below 75%, yellow from 75%, red at the limit and past it), kept live: the host sends the map's usage again whenever it changes, and while the selected prefab is held in the 3D view after Load / Place the editor already counts it, so "Map after" shows the map as it is and the bar's current part grows to it instead of adding the prefab a second time; a note for entity types or modules the editor palette does not know and entities on layers it does not count, and a collapsible Details list of the twelve budget fields. A placed entity costs exactly its palette row's authored `limits` block (network 0 when it is network-static), and a module costs its own block, its GAME template entities, the VIEW entities the engine instantiates into the room and two 50 KB block bases, all filtered by the instance's layer mask OR the snapLayers autoView mask; the backend reads these per-row and per-module blocks once per palette build and the page adds them up. Predictions matched the measured usage change exactly for object and module prefabs on both renderers. **Mods**: saving a prefab of either kind records, in the file's block, every mod its entities and modules use (found the way a map's save finds them), and the list marks it "Mods: <id>" or, when one is not installed, "Needs mods: <id>"; Prefab Details lists each mod and whether it is installed. Load / Place never hands the engine a prefab whose mods are missing: when the mod store has them, the game asks "This prefab needs N mod(s): ... Download and install now?", Yes installs them, adds each one the open map does not list to the map (as the Mods tab's Use in this map, with a console line), and places the prefab; when the store lacks one or cannot be reached, the placement is refused with the game's notice and one console line naming each mod and asking for the prefab's ZIP. A module that is DLC the player lacks is refused with the module message above, since nothing can install it. **Export** (Prefab Details) always writes one ZIP, also for a prefab without mods: `<name>.json` (the prefab file with its block, as on disk) and each mod exactly as it sits in your mods folder (`mods/<id>/mod.json` and `mods/<id>/assets/...`), with no index file; the dialog proposes `<name>.zip`. The Export and Import dialogs open in the last folder used for each (kept in `config.json` as `dialogs.prefab_export` and `dialogs.prefab_import`), Downloads the first time. **Import** (the list head, "Prefab exports (*.zip, *.json)") takes that ZIP, or the `.json` inside an extracted export folder (its `mods/` folder beside it); it is the ordinary install of each mod folder the prefab names (one you already have is left as it is, another version of one you have is installed beside it, and the same version with different files refuses the import with nothing installed) and adds the prefab under the `.json`'s name; importing one already in the library writes nothing, and a different prefab of that name is kept beside it as "<name> (2)". A map's export is refused, and the map importer refuses a prefab's; an export made by an earlier release is refused with a request to export it again. On the console: `sh_prefab mods "<name>"`, `sh_prefab export "<name>" "<path.zip>"`, `sh_prefab import "<path.zip or path.json>"`. |
| Mods tab | Lists the mod folders, one row each, grouped as Your mods, Installed and From maps you played (the folded groups are remembered): name, `id · version · size`, and a status icon (installed; needs attention, with the reason; or served by another folder's build). The details pane shows the description, what it adds (Toybox rows, replaced files, sounds, movies, modules, virtual textures), its files with the carried paths, required console settings and how many of your published maps use it, and where its replacements apply, with **Use in this map** (while a map is open in the editor: the whole map uses the mod without anything placed; it then reads **Remove from map**), **Verify** (rehash its files), **Rebuild** (build the folder again and rescan) and **Uninstall**. A mod used by the whole map reads "Used by the whole map" in the map modal. The map button opens the open map: its name, saved-map id, last save, whether a publish with mods is recorded and its mods uploaded, the search tag `mods`, and each mod it uses with the entities placed from it and the wires and Timelines pointing at them. **Remove from map** first lists what it does (those entities, the references severed with them, the map's row of the mod, the search tag with the last mod) and, after you confirm, deletes the entities in one editor undo step (Ctrl+Z brings them back), drops the row and, with the last mod, the tag; the published copy changes only when you publish again. **Uninstall** lists the same for the open map when it uses the mod, the Toybox rows it removes and that the folder moves to `removed-mods\` (beside the mods folder, whoever made it; nothing is deleted), then cleans the open map, moves the folder and rescans, which takes its rows out of the editor palette. Other saved maps are not changed. When a map still needs an uninstalled mod (Ctrl+Z brings its entities back, a saved map that uses it is opened or saved, Play starts), the folder moves back into the mods folder, the mod serves again with its Toybox rows, and the editor shows "Hell Guards was reinstalled because the map still uses it." (also printed to the console); during Play this waits for the return to the editor. The footer under the store path shows "Removed mods: N, 12.3 MB" with **Empty**, which lists the folders and deletes them for good after you confirm; a map that still uses one of them can no longer bring it back. Both are refused during Play or a map load, a mod placed as Blueprint modules is removed in Blueprint mode, and an entity another object refers to (the editor asks before deleting it) is deleted in the editor. The install button, **Open mods folder** and **Migrate** (only while the old `overrides` folder holds mods not yet moved) stay. A rawmap or prefab `.zip` carries each mod as its installed folder, and opening or importing one installs those folders as the install button installs a folder, so they are listed here as ordinary mods. The page shows only what the backend returned after each action; it never polls. `sh_mods map`, `sh_mods map remove`, `sh_mods uninstall`, `sh_mods removed` and `sh_mods rebuild` do the same from the console, and `sh_mods doctor [verify]` prints every bundle's state, conflicts, shadows and, with `verify`, a rehash of every build. |
| Assets tab | The asset browser — the shipped game catalog, searchable, with a live preview pane, and one click to put an asset on the map. Full detail in the section below. Also reachable as a modal from the Entities tab ("Browse assets"), scoped to the entity you came from. |
| Timelines tab | The list of timeline entities as a folder tree; opening one edits its events and per-event parameters, with reference/decl/enum parameters constrained to valid choices, entity pickers for entity-typed args, and per-event documentation. **Create New Timeline** (the primary plus) authors a one-entity prefab (`idTarget_Timeline` on the portable `snapmaps/unknown` inherit, named after the selected folder when there is one) and stages it through the same New-entity path as the asset browser; the native paste places it under the cursor in Object mode, and the next entity rescan selects and opens it. **Folders** are the display-name convention `Folder/Name`: a timeline's folder is the text before its first slash, so folders travel inside the map, need no storage of their own and read the same in the in-game logic view; **New Folder** adds an empty folder for this window until a timeline is dropped into it. Drag a timeline (or a Shift/Ctrl multi-selection, folders standing for their members) onto a folder or onto blank list space, or use the right-click **Move to** items, and the host renames the entities in one `setDisplayNames` batch; renaming a folder renames its members, deleting one moves them to the root after a confirmation. Folders start collapsed and the open ones are kept in the `timelines.open_folders` setting. The tree shares the Prefabs tab's styles and pure row/range/drop helpers; the handlers are separate because the operations are entity renames, not file moves. Blank list space still closes the current timeline through the shared selection router while the Timeline editor remains protected. **Timeline tile**: ADD OBJECT's *Custom tab carries a Timeline row (`snapeditorentitydef/custom/timeline`, a built-in decl the backend registers before the editor builds its palette) on the native `snapmaps/unknown` entityDef; its prototype record is given class `idTarget_Timeline`, so a toybox placement is a timeline from creation under the native "Add Entity" undo step and saves as vanilla `className idTarget_Timeline`, `inherit snapmaps/unknown`. No entityDef is overridden and the engine's ADD LOGIC placeholder is served unchanged. After map load or paste, entities on `snapmaps/unknown` are rebound by class to the Timeline or Unknown row (`timeline_tile.c`). **Events**: the event picker lists all 1611 events of the engine registry (the same names and argument types `sh_event list` prints), and each argument gets a widget for its type; values are saved keyed by type in signature order, the stock `idEventCallInfo` shape, so a saved timeline is vanilla data. A timeline finds each **Runs on** entity once, when the map starts, so a player (`player1` to `player8`) cannot be a target: the engine reports "references bad entity" and those events never run, and the tab says so under the field. Entity arguments are found when the event fires, so a player name works there; for example `activate` on a Set (String) node with `player1` as the activator runs that node's `smp:` command on the player. |
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

The clone makes network requests only on an explicit user action. Clicking
**Send** in the feedback dialog — or in the crash-report dialog — POSTs the typed report (category /
title / details / optional contact), the installed version string, and one token naming the renderer the
game is running (`vulkan` / `opengl`, read from which renderer library the process loaded — nothing
else about the machine is gathered) over HTTPS to the project's feedback relay
([`feedback/`](../feedback/README.md)), which files it as a public issue on this repo's tracker. A
crash report can additionally attach the tails of the local log files, but only when its
"Attach recent logs" box is checked, and the text is anonymized first (account / profile / machine
names scrubbed — see [`feedback.md`](feedback.md)); the local crash dump is never uploaded. The one
download is a map's missing mods, and only after you answer Yes to the question that names them (see
[Mods](#mods)): it downloads only what you approve in the dialog, verified by hash. Nothing else runs
in the background, and nothing else is ever sent. (The installer
separately talks to GitHub Releases for `snapmap-plus update`, and to Microsoft's bootstrapper if the
WebView2 runtime is missing — both documented in [`packaging.md`](packaging.md).)

## Hook behaviors

Engine detours and resource-loader shadows the backend installs.

| Hook | What it does |
|---|---|
| Rawmap load | When rawmaps are on, load the map from the staged rawmap file — `%LOCALAPPDATA%\snapmap-plus\rawmap.json` unless **File ▸ Load Rawmap...** points it somewhere else — instead of the engine's own save. A `.zip` installs the mods it carries first. A rawmap that names a mod you do not have is never substituted: the game offers the download when the mod store has the mod, and otherwise refuses with a notice and a console line naming each missing mod and its version and asking for the author's `.zip`. |
| Rawmap save | When rawmaps are on, mirror each saved map's JSON to the save path. That is `%LOCALAPPDATA%\snapmap-plus\rawmap.json` until **File ▸ Save Rawmap As...** or `sh_rawmaps savepath` names another file, and opening a different map returns it to the default. A file someone downloads and opens is an archive entry, so nothing about opening one aims a save at it. Off, your save is untouched and that file is left alone — the same switch governs both directions. A map that uses mods also gets `<name>.zip` beside the `.json` on every rawmap save, with a toast and a console line saying to share it. |
| Rawmap branch answer | A map opened from a rawmap counts as a new map, so the engine's own Save asks for a name instead of replacing whichever map the rawmap opened over. Detours `idSnapEditorLocal::IsBranchMap`, the one function the editor's save menus ask, and answers for a substituted map without writing to the map. A sibling function in the image has the same body around the `map:new` check, so the scan confirms its target by the literal the callee names. `sh_rawmaps overwrite on` allows the replacement instead, for that session only, and says so when it is turned on. Loads are refused if the protective hook is unavailable unless overwrite was explicitly enabled. |
| Rawmap file menu | **File > Load Rawmap...** stages a `.json` or `.zip` file (both are listed) and offers to open it in place after confirmation; a `.zip` brings its mods, and missing mods are offered for download or refused before the map opens. **Save Rawmap As...** offers `.zip` first when the map uses mods, and the menu names the `.zip` to share after a save. **Open Rawmap as New Map** confirms before replacing unsaved editor work. **Save Rawmap / Save Rawmap As...** serialize the live map, including unsaved edits; a missing or busy editor is refused, and a map change cancels queued saves. The File menu shows both paths and lets the user release a chosen save destination or enable rawmaps for every load/save. See the [rawmap guide](https://www.snapmapplus.com/snapmap-plus-guide.html#rawmaps). |
| Overrides file-shadow | Serves map navigation, the generated Grid Room variants, the built-in default declarations and then installed mods through native file opens, falling back to installed originals, which it reads through the provider's own open (`read_installed_resource`). The only user files it reads are the files of installed mod bundles, through a table built at startup. The custom stream requires the audited native ABI. |
| Installed mods | Serves the files of every admitted bundle in `mods\` below the product's own sources and above the game's archives, from one table read at startup (sizes checked, never hashes) and replaced only by a rescan. Answers the decl existence probe for served decls, detoured only when a bundle provides one. Registers the bundles' new editor rows inside the palette build, and a map's gameplay decls when a map whose manifest names their bundle loads, at the engine's own map lifetime. A runtime palette rebuild runs inside process heap 0 and is refused while the editor holds an entity or a wire. With an empty `mods\` folder every hook passes through. See [Mods](#mods). |
| Retired package payloads | A map saved or published with packages by an earlier release carries them as `smpkg.` string variables. Before the native parse, a complete package is unpacked once into a mod folder `mods\<id>\` under the data folder (or answered by an installed mod with the same content) and the map is given a manifest row naming it; an incomplete, mismatched or unreadable package is removed. Either way the rows never reach the engine, and each package logs one `LEGACY:` line. A package already converted, or migrated from `overrides\` with the same content, is reused instead of unpacked again. A published map is converted locally and never re-saved. See [Mods](#mods). |
| Baked navigation | Marked Blocking Boxes are baked into the AI navigation of each placed module copy whenever a map is converted for play, and by Build Navmesh in the editor. This covers maps started from the editor, from the SnapMap hub or map browser without opening the editor, joined in co-op, or loaded directly: the bake reads the map actually being converted, so a map edited earlier in the session cannot stand in for it. Each copy is baked separately, so a module placed several times gets navigation for each copy. Nothing new is written into the saved map; navigation is rebuilt from the marked boxes at each Play, which also means it needs Snapmap+ on the machine running the map. Maps saved by earlier releases may carry `smnav1.` shards in their map variables; these are legacy pass-through only: they are still validated, served for module copies without a bake of their own, and written back on save, and a module those shards describe that is placed more than once keeps its shipped navmesh. A copy, or one demon size of a copy, that cannot be baked keeps its shipped navmesh; its reason is logged and shown by `sh_navmesh`, which also says which map the last Play read its boxes from. `navmesh.enabled` turns the whole thing off; `navmesh.embed_in_saved_maps` turns off only the shard write-back. |
| AI Navigation on a volume | Enable **AI Navigation** and **Block Demons** on Blocking Boxes. Boxes with inherited default dimensions support navigation, including in existing maps. Green outlines preview the smallest demon size; `navmesh.preview` controls its display. **Build Navmesh** in the native Start menu and **View > Update Navmesh** run the same bake, even with preview disabled or in Blueprint mode. The added row sits below **Save As** and above **Play**, appears with the rest of the menu when it opens and is hidden as it closes. The native action uses the game's busy modal and completion toast without saving or checking for a player start. The View menu says when a marked box has changed since, and after a build how many rooms (module copies) were built and why one was not. Map-text reads at load and after Save take an omitted size from the box's entityDef, so default-sized boxes keep their preview. Refusals are as narrow as the engine allows: a box with an unusable size, rotation or room claim, or in a room whose position cannot be read, is left out alone and named, and only a malformed room table refuses the map; a module copy that cannot be baked keeps its shipped navmesh while the others are built; each demon size is baked and refused on its own, and a size already refused at the same geometry is not baked again at Play. An unchanged map reuses the preview's smallest-demon bakes at editor Play: the map Play converts lists the same boxes in another order and with other entity indices, so reads are compared by box shape, room and flags (within 0.05 units, the float round trip of a converted read), and only a moved, added, removed or reflagged box takes a new bake. Every successful bake of every demon size is kept in a bake cache keyed per room copy by its module, its shipped navigation file, the traversal table and the same shape comparison, so an unchanged copy is served at the next Play, after a reload and on a launch outside the editor, and an edit re-bakes only the copies it touched (the preview reuses the cache the same way). The cache holds at most 128 MiB and 1,024 payloads, evicting the least recently used and never one the current Play uses; `sh_navmesh` reports its use. A box top leaps to a farther top only when no nearer top it leaps to has a shorter leap of its own there, so each demon still reaches every top it could with far fewer routes (a 600-box Grid Room copy: 93,742 links to 49,942). Where one area would pass the engine's 256 outgoing routes, the boxes contributing most give up just enough of their routes there. Once every link is placed, the tree below each shipped leaf a box claimed is rebuilt over the same areas with Doom 3's splitter choice, so every point keeps its area and the depth stays near the logarithm of the box count (the same copy: 121 to 23); where the tree would still pass the engine's 128 depth, the copy keeps three quarters of its floors at a time until it fits. Where one copy's navigation file would pass 16 MiB (a product bound on memory: the engine's loader has no size check, and a bake is served from memory rather than stored in the map), the copy keeps each area's shortest routes and drops its longest leaps to a per-area budget chosen to fit, then sheds floors as for depth; a 600-box Grid Room copy measured 5.0 MiB. Boxes that lost something are drawn amber. The toast reads **Navmesh Built**, **Navmesh Partly Built** ("3 of 4 rooms built. <room> (copy 2): <reason>") or **Navmesh Build Failed**, the last only when nothing could be built or the map could not be read. Ceilings, each leaving out the excess with a note: 65,536 boxes and 8,192 rooms per map (about 11 MB and 1.2 MB of tables at most), 2,048 boxes and walkable pieces per room and demon size (about 11 MB of scratch per bake), 131,072 edge strips (about 26 MB), 8,192 edge segments per area. Climb approaches clear the rotated solid, and traversal candidate storage grows with geometry. A floor climb may start on whichever walkable module floor area its run-up lands on, so a box centred on a Grid Room's door strip is climbable from its sides as well as its ends; climbs are placed along each box edge about every 192 units, midpoint first and at least 128 units apart, within each area's route budget, and never from the cut lines inside a box whose top another box splits. The module floor under a box, and under a box hanging lower over it than the demon size is tall, is cut around the box grown by that size's clearance, by the same rule that cuts a box top around a box standing on it, so a floor route goes around the box to a climb instead of into its face; a floor area that would need more than 192 pieces is left whole and counted. `sh_navmesh` and the bake log report, per demon size, which faces of each box carry a floor climb and the commonest reason the others do not. Barrier-jump links (animation-free climbs of up to 64 units) exist only in builds made with `SH_AUG_BARRIER_JUMPS`. Refused bakes log their reason. The [player guide](https://www.snapmapplus.com/snapmap-plus-guide.html#ai-navigation-on-blocking-boxes) explains clearance and authoring limits. Backend marker, geometry and bake rules are documented in their source files. |
| Placement grid around marked boxes | The faint green grid the editor shows while a demon is held is a stock model per module and demon size (`<module>/<leaf>_aas_<size>.bmodel`), loaded by name whenever the editor rebuilds its placement surfaces. After each preview bake, every copy whose floor the bake cut gets that model rewritten so its floor triangles cover only the floor the bake kept: the grid stops around each box grown by that size's clearance and continues under a box high enough to walk beneath, as demons will. The preview therefore bakes all three demon sizes, and Play reuses those bakes. The native rebuild asks for a cut copy's model under a private name (`maps/smpnavgrid/<hash>/<stock name>`, substituted at the call that completes the name, VK 0x1198E7F / GL 0x11980AF) and still creates, shows and hides it for the held demon. A changed set of cut models rebuilds the placement surfaces once, inside a process-heap scope; unmarking every box restores the stock models, and copies with nothing cut load the stock model unchanged. A model that cannot be cut keeps its stock model and is logged. Cut models already loaded stay in the engine's model list until it purges them, a few kilobytes each. |
| Unrestricted placement | Any entity can be placed or moved anywhere in the editor, including open air. DOOM refuses a held entity whose placement flags ask for a floor (every demon, the player start, encounter spawners, AI path and combat points, item hints and pickups) when no floor is under it, showing it red; `place_anywhere.c` turns off only that refusal, in the test that colours the held entity and in the one that admits Place. A floor or wall under the entity still counts as before, Drop to Floor still drops it, and an entity that overlaps another or lies outside every module is still refused. Nothing new is written into the map. In Play the player starts on the floor below a floating player start, and floating demons are found on the floor once they are active. If the two tests are not found on the running executable, the log says unrestricted placement is unavailable and the native rule applies. |
| Strids injector | Combines the user's `strings\strids.json` with the built-in strings. A refresh publishes the owned values without duplicate rows, retaining storage needed by live native readers. See `src/backend/strids.h`. |
| Text-field clipboard | **Ctrl+C / Ctrl+V in the editor's text fields** — every free-text property (datapad and transmission message bodies) plus the text-backed int / float / vec3 / size inspectors. Copies the selection, or the whole field when nothing is selected; pastes over the selection, or at the caret. Multi-line text pasted into a single-line field collapses each break to a space; datapad bodies keep their newlines. Respects the field's own character cap. Vanilla has no text clipboard at all here (only the dev console accepts a paste) — and it is genuinely absent rather than disabled: the engine's SWF text-edit key handler has no Ctrl branch whatsoever. Entity-level Ctrl+C/Ctrl+V and Ctrl+X are untouched. Exceeds the original. |
| Boot pacing | DOOM reaches the SnapMap hub in about 19 s on Vulkan and 22 s on OpenGL instead of about 158 s and 141 s (warm boots on the test machine). The engine's startup resource preload was paced by waits, not by the disk: the load-screen video slept 10 ms at a time while holding the lock the file-stream thread needs, and the main thread polled every read with `Sleep(1)`. `boot_pacing.c` releases that lock for the video's sleep and lets the poll spin and yield before it sleeps. The load-screen video and progress ring look and play as before. Both sites are byte-checked and patched about 0.1 s after launch; when a site does not match, it is logged and left alone. Map loads run through the same two sites. |
| String Set commands | Detours the Set (String) action's `DoAction`; a literal value starting with `smp:` runs as a command on the activator's player before the stock write (see [Map behaviour](#map-behaviour-string-set-commands)). |
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
