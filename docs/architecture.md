# Architecture

Snapmap+ ships a backend DLL loaded by DOOM and a WebView2 companion DLL. The
backend owns engine integration; the frontend owns presentation and forwards
engine work through their shared interface. The Go installer deploys the pair.

This guide explains component boundaries and contributor constraints. Detailed
backend algorithms and reverse-engineering evidence belong beside their code or
in the snaphak-re findings system.

## Components and entry points

| Component | Responsibility | Start reading |
|---|---|---|
| Backend | Bootstrap, hooks, editor operations and installed-resource access. | `src/backend/dllmain.c` |
| Shared interface | Matched DLL contract, callback bindings and shared queues. | `src/common/snapmap_plus_iface.h` |
| Frontend | Companion window, messages, editor views and preview controls. | [WebView UI](webview-ui.md) |
| Fault shield | Selected engine-fault recovery and local crash evidence. | `src/fault_shield/fault_shield.c` |
| Installer | Verified deployment, migration, update and uninstall. | [Installer README](../installer/README.md) |
| Store tool | Migrates a mods folder of earlier releases; the installer runs it. | `src/store/store_tool.h` |
| Feedback relay | Files in-app reports as GitHub issues. | [Services](services.md), [feedback](feedback.md) |

The backend is an XInput proxy named `XINPUT1_4.dll`. It forwards controller calls
to the system library and initializes the editor integration. The frontend is
`snapmap-plus-ui.dll`. Both are built and deployed together; mixing their versions
can call the wrong interface slot. See [packaging](packaging.md).

## Supported DOOM build

Snapmap+ supports one build of DOOM 2016: the April 2024 update, Steam build
13954591. Its executables are not SteamStub-wrapped and import `XINPUT1_4.dll` by
ordinals 2 and 3; `xinput1_4.def` pins the real System32 `XINPUT1_4.dll` ordinal
set (named and unnamed), with the backend's private exports at 200 and up.

`host_image.c` identifies the host by the SHA-256 of its executable file
(`SH_KNOWN_BUILDS`: the Vulkan and OpenGL executables of build 13954591). Any other
executable, the 2018 build included, is unsupported: the backend logs one line
naming the supported build and the final 2018 release, then only forwards XInput.
Every engine constant is a build 13954591 value: struct and slot offsets in
`engine_build.h`, function RVAs as Vulkan/OpenGL pairs in the signature table, and
global RVAs in the generated globals table. Raw RVAs are applied only on a
recognized executable, for its own renderer, so no address can reach another image.
A feature whose engine offsets are not yet verified for this build stays off and
logs which values it waits for (`SH_PENDING_*` in `engine_build.h`); starting DOOM with
the environment variable `SNAPMAP_PLUS_LIVE_VERIFY=1` runs those features for live verification.

The installer routes by build before downloading anything: it hashes
`DOOMx64vk.exe` and `DOOMx64.exe`, installs the current release on build 13954591,
installs the final 2018 release `v0.2.1-beta.19` (by tag, exactly as that release
installs itself) on the 2018 build, and refuses unrecognized or mixed executables.
See [packaging](packaging.md#supported-build-and-the-final-2018-release).

## Engine resolution and patching

Function signatures identify engine code. Generated anchors derive global
addresses from code references. Vulkan and OpenGL share these portable resolution
paths; field layouts still need explicit verification when porting.

A pattern that matches more than once is not an identification. Where two
functions share a body, the caller must confirm its target by something the
pattern cannot carry: the rawmap branch answer follows the call its candidate
makes and reads the literal that callee names, because the sibling it must not
patch differs only there.

A raw-RVA fallback requires an exact fingerprint of a supported
executable. The hook-tolerant fallback additionally checks the detour and remaining
signature bytes. A filename or a readable address is not sufficient evidence.
Contributor changes to signatures or anchors require the two-image test gate in
[contributing](contributing.md#7-run-the-tests).

The bootstrap scans the whole database once, which takes about five seconds;
later installs reuse those results instead of scanning again. The boot pacing
sites (`boot_pacing.c`) cannot wait for that scan: the engine's resource preload
starts about two seconds after launch. Right after the build check they are
matched at their recorded RVAs only (`sig_match_recorded`), checked byte by byte,
and patched about 0.1 s after process creation. The offline signature tests
prove each of those recorded RVAs is the pattern's only match. A patch that would
land more than three seconds after process creation is not made, because the
preload's threads could then be executing the redirected instructions.

Hook callers prepare a trampoline, publish it as their original callback, then
commit the detour. Preparation does not write the target. Callers own the
lifetime of their original callbacks and any relay memory.
They must preserve those resources until restoration succeeds and no execution
can still use them. Failed rollback remains a recoverable state, not a successful
uninstall. The patch and hook headers define the supported installation and
restoration sequence. Stolen instructions must be complete and independently
executable. The ordinary hook helper performs no relocation; its explicit
`hook_prepare_rip_lea` variant expands one verified 64-bit RIP-relative LEA.
Callers still verify every other stolen instruction. Original target bytes
remain separate from relocated code for commit validation and restoration.

## DLL interface

Map rendering lives in the backend and DOOM's native Settings / Properties
panel, in the Snapmap+ tab. The Unlock View Distance group owns an
Apply/Cancel draft (one toggle) tied to the current editable map. On stores
the versioned string `smp.render.v1` = `4;0` in native map variables, appending
the row after every existing string and leaving `allocCount[4]` untouched; Off
removes the row the way the native list removes an element. String variables
are bound by index, so the row is removed only when every later row is one of
Snapmap+'s own `smp.` settings (those move down one); otherwise it stays as
`4;8192`, which reads as off. The native serializer carries the result through
normal saves without a sidecar. Versions 1 to 4 stored a distance: 8192 and
disabled version 2 read as off, any other distance as on. Deserialization
normalizes the snapshot the editor builds its editable map from: an on value
becomes `4;0` and an off value is removed. Malformed or duplicate metadata
disables the override. Map deserialization clears the previous runtime
selection and reads the loaded map's variables; editor-to-play conversion
refreshes it from the build copy. A locked value copy crosses to rendering at
the blended environment's single maxViewDistance read: off returns the native
read, on returns 200000 (the engine raises the read to at least 256 before
writing r_zfar) and rewrites the view's literal. Before that it reads the
block's native distance and stretches its fog by r = 200000 / native (floored
at 256, never below 1): the `fogstart`, `fogend` and `fogstartdistance`
literals are multiplied by r and `fogscale` and `fogdensity` divided by r,
whatever the blend target and in the editor and Play alike; the renderer packs
them into `fogParms`/`fogParms2` from that block after the blend, and the block
is the view's own per-frame copy, so no declaration changes and nothing needs
restoring. The blend writes r_zfar only when an environment resolves, so
selecting an unlocked map also sets r_zfar to 200000 once through the engine's
forced cvar setter, and the editor's Blueprint far-plane writer (`editor_zoom.c`)
is raised to 200000 for that map, so every r_zfar writer agrees. This applies only to
SnapMap and uses signed bindings in both renderer executables. No WebView
interface change is involved.

The Snapmap+ tab is a third category added to the native Settings / Properties
panel (`settings_tab.c`): the category count, the diamond markers and the
populate dispatch are extended under the panel's own hooks, and groups register
through `sh_settings_tab_register_group` with their own draft, commit and reset
callbacks because native Apply and Reset are not category-scoped. It holds the
Unlock View Distance group and the Unlock Map Bounds group (`map_bounds.c`), whose
`smp.bounds.v1` map variable raises `snapEdit_environmentModuleBounds` and the
camera distance limit for that map up to the 32671-unit ceiling imposed by the
signed-short collision and navigation coordinates, restoring the native 16000
when a stock map loads. Every client re-validates module placement against its
own cvar, so an unlocked map is a Snapmap+-only map. Blueprint zoom-out
(`editor_zoom.c`) raises the orbit camera's dolly and pan height limits to a
client-side multiple of the native height, default 2x. Object Mode entry
(`object_entry.c`) repoints the FPS view's one load of its 18364-unit downward
trace segment at a product float covering the whole coordinate range, so the
entry drop reaches the floor of a Grid Room taller than that segment. Unrestricted
placement (`place_anywhere.c`) turns the two editor tests that refuse an
entity with no accepted surface and no air flag (the held-entity colour and the
Place admission) into unconditional passes for that clause only; the surface
classification, snapping, alignment and the overlap and module refusals stay
native.

Grid Room resizing is implemented in the existing backend. The native DOOM
Module Properties panel owns its XYZ control. In Blueprint mode, the normal
module selection handler adds the native Module Properties prompt and checks
the engine's X action for the highlighted Grid Room. It selects that placement
and enters the native panel state while retaining the Blueprint camera. Tutorial
restrictions, other module types and selection transitions remain guarded.
The WebView has no resizing control or new interface slot. Dimension-bearing
module names resolve through private native palettes and the existing resource
provider. Newly materialized empty catalogs take the native generic source load
before admission; accepted and stock catalogs are not reloaded. The edit handler
changes one compiled placement and translates its owned caps, doors and frames
through the native copy-on-write entity edit and transform setter. This uses the
live local transform, including after a native cap replacement leaves serialized
definition edit state null. Ordinary placed objects keep their local positions and sizes.
Finite XYZ inputs clamp to the per-axis limits and display the applied dimensions.
The current snapEdit_environmentModuleBounds cvar constrains the selected
placement and every connected neighbor on each edit: only the axes the user
changed move, each as far toward the request as the boundary allows, and an
existing boundary crossing is tolerated but never extended, so a shrink always
applies. Every limited axis is logged with the request, the accepted value and
the limiting coordinate. The built-in probe and ceiling light that the editor
owns follow the room on each edit; the fill lights, which get no editor entity, follow from
the generated room on the next load or Play, through the same policy. Planning
uses temporary spatial views and native bounds helpers before creating
resources. Signed-short
BCM/AAS coordinates impose independent representation limits, including navigation
outsets beyond the shell. Changing the cvar does not change those formats.
Connected module branches translate rigidly to follow the changed doorways. A
conflicting loop or increased bounding-box overlap prevents the connected edit.
The engine recomputes all portal connections before accepting the transaction;
an unexpected change restores the prior transforms and module records. The room
resources are constructed in memory; no map sidecars are required.

Built-in lights are identified from the installed module's original entity
names and inheritance. Native property-tree setters, commit and cached-property
refresh update their position, coverage and distance cutoffs without changing
authored light properties. The modern effective light center follows the roof;
the classic central light follows the room center. Tree ownership uses the
engine's matching destructor and delete calls, with rollback on edit failure.

The resource provider supplies resized AAS for all three stock agent classes.
Door apertures and agent clearance remain fixed. Narrow strips that collapse
at the shrink minimum are removed with their area references, cover membership,
visibility and route chains remapped. Native BuildAAS performs module placement
and assembly, and marked blocking volumes augment the resized base navigation.

Object-mode containment first uses the native query. If its fixed 6000-unit
vertical rays miss an admitted taller variant, a bounded fallback extends both
rays to that room's height. Both native body hits must identify the same valid
module; world bounds alone never authorize placement. Resizing also refreshes
the native per-module placement-display models inside the process heap scope,
including after rollback. Otherwise Object Mode can retain the previous size's
display resources until the camera is re-entered.

The native XYZ inspector shares a context with Grid Offset. Its base reset
clears values but leaves the range list populated. AddVec3 clears the validated
range count before native construction so each inspector rebuilds its own three
ranges. Allocation ownership stays native; the separate door-cap and portal
minimum checks still determine the smallest accepted room dimensions.

The faint green surface display is the game's navigation-class placement
overlay, separate from the custom blocking-volume preview. The held entity's
native navigation type selects one of three installed display models. Objects
without that type do not request a model. The native material uses world-space
grid coordinates and animated brightness, so its apparent intensity can vary
while the model remains visible. The resize path preserves these native rules.

Reload has a separate palette membership probe; it now shares the variant lookup
so saved rooms reach instance conversion. Cold construction can run before editor
activation once the stock palette and native main-thread heap are available.
Portal magnet snapping corrects the selected group's discarded XY offset only
for compatible equal-size portals involving a variant. Explicit attachment scopes
an exact-origin exception to that operation. Add/move/duplicate confirmation
also preserves off-grid origins in maps containing resized rooms, including a
stock neighbor aligned to one. Native dirty marking and remainder cleanup still
run. Native reciprocal connection checks remain exact; the normal frame updater
replaces caps and creates doors/frames when those connections change.
Private palettes retain their two cached collision resources across map teardown;
their native collision builder runs in the same process heap scope as the palette.
Stock palette collision lifetime remains native. A process holds at most 64
distinct size/type variants, avoiding eviction while maps or undo records may
still reference them. Save files contain the dimension-bearing module identity;
players need Snapmap+ to reconstruct its resources. Vanilla loading is unsupported.

The shared object uses an append-only vtable. Its original 77-slot prefix is
retained, with extensions through `+0x380`; the current table occupies `0x388`
bytes. The File menu's rawmap surface is `rawmap_status` (`+0x328`),
`rawmap_configure` (`+0x330`) and `rawmap_load_now` (`+0x338`). The last three
are `navmesh_view` (`+0x340`), `run_on_main` (`+0x348`), `delete_entities`
(`+0x350`), `prefab_grab_status` (`+0x358`), `module_prefab` (`+0x360`, the
Blueprint module-prefab state, save, place, status and module-check operations),
`timeline_tile` (`+0x368`, the Timeline palette tile's status) and `budget`
(`+0x370`, the editor budget table and usage for the Prefabs tab's Limits
section, built on the game thread after each palette build by
`budget_table.c`), `mods` (`+0x378`, the Mods tab's document and actions,
`modui.c`, run on the game thread through `run_on_main`) and `prefab_watch`
(`+0x380`, the Prefabs tab's guarded reads: the Blueprint module selection that
gates Create, the Load / Place held in the 3D view, and the game thread's last
paste decision, from `iface_engine.c`, `apply_engine.c` and `module_prefab.c`). The retired
timeline-inherit normaliser at `+0x298` stays in the table and returns 1. The backend writes a marker and the table's size into the object's
first reserved cell (`+0x08`); older backends left it zero and had `0x350`
bytes. Check `SH_IFACE_HAS_SLOT` before calling any slot at or past `+0x350`,
so a frontend never reads past an older backend's table.
Static assertions pin the layout. Do not insert, reorder or repurpose
existing slots. Add new capability slots at the end and update both DLLs.

Headers define buffer ownership, return values and thread requirements. Keep
engine pointers inside the backend where possible. Copy values across the
boundary rather than exposing mutable engine storage. An accepted asynchronous
request is distinct from completed engine work.

## Threads and edit results

The WebView host owns its window and browser callbacks. Its worker polls the
backend through a manual think loop at about 30 Hz. UI callbacks stage requests;
the worker drains them and posts result messages. A UI mutex protects its shared
request state, not the engine's objects.

Selection, class/inherit, display-name, source and camera writes from the UI go
through `run_on_main`. The backend runs the call after the next native frame
while the worker waits. A call that has not started when its wait expires is
withdrawn and never runs. A started call borrows the worker's data, so the
worker waits for it to finish; the main thread never waits on the worker.

Navigation and staging maintenance run after a successful native engine
frame on DOOM's verified main thread. Recovery suppresses that pass. The frontend
queue drain does not schedule native maintenance or display engine dialogs.

Console `sh` handlers run on the engine's command-execution thread. Synchronous
apply runs inline only when that thread is positively identified as DOOM's main
thread. A known off-main caller copies its batch into the blocking marshal and
waits for the main-thread command drain. Unknown identity, missing transport and
occupied slots refuse the request; they never authorize an inline fallback.

The marshal withdraws work that has not started when its wait expires. If the
engine has started but has not finished by the deadline, it retains the request
and returns `SH_APPLY_IN_PROGRESS`. Callers must not report that as failure or
submit a duplicate. The asynchronous queue likewise refuses a new batch while
its existing pending batch is occupied.

Class/inherit edits snapshot both original pooled strings before assignment.
Result `1` means every requested assignment succeeded; `0` means refusal or
successful rollback; `-2` means rollback also faulted. Only complete success
permits a dependent source rebuild. SEH contains memory faults; it cannot promise
that every native engine operation can be undone.

An entity edit whose text sets `isNetworkStatic` on an entity whose palette
definition names a `networkStaticPath` also writes the network-static byte the
editor budget counts (entity holder +0x320), which replacing the definition
text does not reread, and turning Static off is refused and rolled back, as the
native toggle does, when the recomputed budget is then at or over a limit.

Prefab Load/Place stages data and queues the native paste action after checking
editor state and the copy/paste cvar. Queue acceptance does not confirm that the
engine has entered placement mode. The engine consumes that action later.

Entities-list Delete calls `delete_entities` inside `run_on_main`. The backend
applies the gates DOOM's own Delete key passes (object editing active, no grab,
placement or pending action, the map's Delete restriction), makes the native
selection exactly the requested entities, clears a hover on any other entity,
and calls the engine's Delete Selection. The engine checks eligibility,
releases the entities and empties the selection; the "Delete Selection" undo
step and the return to idle follow on later native frames. Until they do, the
selection guard refuses product selection writes and reports `2`. Every refusal
before the action restores the previous selection. The selection setup and the
action run inside a verified process-heap scope, as the Delete key runs inside
the editor frame: they grow the selected-ID array and the wire animation list,
which outlive the call, and on a console command's map heap Play would release
them under the editor, so the next Ctrl+Z, which frees the selected-ID array,
stopped DOOM with "Memory corruption before block!". A request whose scope
cannot be verified is refused unchanged. Portal doors and frames are
refused before anything changes. The page edits its own selection and announces
success only from the returned status.

## Engine data ownership

Engine allocators inherit the calling thread's heap scope. Objects retained
across Play or map teardown need a persistent scope; allocating them on a
transient scope can leave apparently valid pointers to released storage. Keep
construction, destruction and heap-scope pairing within the backend operation.

The frontend treats returned data according to each slot's explicit ownership
contract. Preview requests are asynchronous and generation-tagged so an old
completion cannot replace a newer selection. Large serialized results use owned,
growable buffers instead of silently truncating fixed arrays.

Asset browsers and prefab previews read declarations and resources from the
player's installed game. The product does not embed DOOM assets. Resource
caches need explicit reader lifetimes; a refresh must not invalidate storage a
reader or native object still uses.

Navigation snapshots and shipped resource reads run on the engine thread.
The snapshot reader takes collision dimensions from each cloned entity's
resolved declaration while the native snapshot is alive. It pairs declarations
with JSON by array position, preserving inherited sizes and duplicate IDs
without adding fields to saved maps. Only copied geometry reaches the bake.
A worker owns copied preview inputs and performs the arithmetic bake, while the
frame installs only a result matching the current geometry revision. The bake
lock precedes the worker handoff lock; the worker never acquires the bake lock.
Published lines remain until their replacement completes, except during a drag
or after an empty or refused snapshot. Sparse box refreshes include unmarked
obstacles and use live uniqueIds; topology changes require a complete snapshot.
Play always takes its own complete snapshot, of the idSnapMapEdit that
SnapMapEditToSnapBuild is converting rather than the editor's map, so hub, map
browser, co-op and direct launches read the map actually being played. The
editor's map stands in only when the converted map cannot be read and the two
place the same modules in the same order (`nav_snapshot.c`). During that
snapshot an unresolved Blocking Box takes its entityDef size for omitted axes.
The worker and session log handle
have process lifetime; DllMain must not wait on them or acquire their locks.

The native Start-menu action in `nav_menu.c` shares the View menu's update
request and completion state. It creates its additional button through the
engine, repairs the SWF binding after timeline rewinds, and uses native busy
and toast widgets with literal text. Completion collects the worker result
independently of preview drawing; the UI does not invoke saving or map validation.

Navigation surfaces and obstacles have separate roles. The bake emits only
exposed box faces meeting each AAS class's floor-slope threshold; ordinary
Block Demons boxes subtract occupied standing space without adding surfaces.
The engine also registers their clip shapes for native AAS obstacle avoidance.
Its separate expanded blocker registration feeds flight navigation. Keep both
native registrations intact for ordinary walls; a vertical wall does not need
a custom walkable shell. Traversals connect valid approach and landing surfaces.

Native route preparation allows 256 outgoing reachabilities per area. A custom
bake refusal concerns that area's route complexity, not the map's entity count.
It must remain distinguishable from a stale preview or a failed local obstacle
query, either of which can occur while the underlying AAS remains loaded.

## Configuration

The backend owns `%LOCALAPPDATA%/snapmap-plus/config.json`, including defaults,
validation, persistence and recovery. UI writes go through the registered config
service. The frontend uses the service's descriptors and values; it does not
maintain a competing settings file.

Initialization validates configuration before dependent subsystems consume it.
Unknown keys are preserved. Invalid input and failed persistence remain visible
through service results. Theme bootstrap may read the existing preference before
the browser is ready, then converges on the backend's published state.

Configuration descriptors in `src/backend/config.c` define available keys and
change behavior. Deleting the file resets preferences at the next startup.
Installer updates and uninstall preserve player configuration.

## Mods

Mods are installed under `%LOCALAPPDATA%\snapmap-plus\mods\`. A mod is a folder
holding `mod.json` and an `assets/` tree at the engine's own request paths
(a `package.json` is accepted until the folder's first build turns it into
`mod.json`). Mods reach other machines two ways: a published map's
bundles are uploaded to the Community API's mod store when the map is published,
and a player who opens that map is asked first and downloads only what they
approve in the dialog, verified by hash (`modnet.c`, `modsync.c`, `modpublished.c`;
see [Published maps and missing mods](#published-maps-and-missing-mods)); that
transport is for publications only. An exported rawmap or prefab is a ZIP that
carries each mod exactly as it sits in the mods folder, its folder with
`mod.json` and `assets/` (`modexport.c`), and importing one is the ordinary
install of those folders, so sharing stays offline; every rawmap save of a map
that names mods writes that ZIP beside the JSON (see
[Rawmaps that name mods](#rawmaps-that-name-mods) and
[Prefabs that name mods](#prefabs-that-name-mods)). Plain saves never upload. The retired
`%LOCALAPPDATA%\snapmap-plus\overrides\` tree is read once, by the store tool's
migration below, and never served or written; no `package-cache` or `package-staging`
folder is created.

These backend files share the work:

- `modbundle.c` builds a mod folder into a bundle: a SHA-256 per file, the
  logical name each file provides, the names its decls need, the namespace and
  structural rules, and the canonical manifest whose SHA-256 is the bundle hash.
  It reads and writes `mod.json` and the product record (below). New names live
  under `mods/<id>/`; raw files and skins decls may spell the id the way the
  engine derives binary names (`-` becomes `_`). Stock files a bundle replaces
  are listed as `shadows` and must be stock, except in a bundle with a `legacy`
  record (a converted first-generation package), whose unresolved needs also
  only warn.
- `modstore.c` owns the store: the mod folders, install, verification, the
  rebuild rule and the one-time upgrade of the retired layout, below, which the
  store tool runs. Files are hashed at install and on `sh_mods doctor verify`,
  never at boot.
- `modstock.c` is the read-only installed-resource bridge behind stock
  pointers, below.
- `modcmd.c` is the `sh_mods` console command. It dispatches the serving verbs
  (`status`, `rescan`, `palette rebuild`) to `modserve_engine.c` (a rescan runs
  the rebuild rule first) and owns `build`, `install`, `list` and `verify`. Its
  `modbundle_build_folder` is the builder entry the legacy converter calls, with
  the game's archives as the stock resolver.
- `modserve.c` reads every `mod.json` once at boot, checking sizes and never
  hashes, into one immutable table from request path to file; a bundle that
  fails is skipped with one `MODS:` line and nothing else is affected. Needs are
  resolved once per table, through the same request paths the builder uses.
  Lookups allocate nothing.
- `modserve_engine.c` registers editor rows inside the palette build (Timeline
  tile detour), answers the decl existence probe for served decls (the probe is
  detoured only when the table holds a decl), activates a map's gameplay decls at
  the engine's own level 1, and rebuilds the live palette inside process heap 0.
  A grandfathered bundle's shadows are planned like new names; the engine
  already knowing the source marks a stock decl, which is left to the provider.
- `modmanifest.c` owns the `smpmod1.<index>.<count>` manifest rows (a placed
  mod's row and a map-level one, `"attached": true`), and `modlegacy.c` the
  retired `smpkg.` carriers, as below.
- `modui.c` is the WebView Mods tab's backend. It lists one row per mod
  folder (two folders declaring one id both flagged, a package a map delivered
  listed like any other) with what each carries, marks each installed or
  needing attention with its cause, reports whether `overrides\` still holds
  entries to migrate, describes the open map and the mods it uses, and runs
  Install, Verify, Rebuild, Uninstall, Remove from map, their dry runs, Migrate
  and rescan on the main thread, as in
  [The Mods tab's map scope](#the-mods-tabs-map-scope). What the tab does not show, every bundle's state with legacy ones,
  conflicts with their winners, shadows and verification, it prints as
  `sh_mods doctor [verify]` (see [WebView UI](webview-ui.md#mods-tab)).
  `modmap.c` reads the open map's entities, wires and Timeline references from
  its serialized JSON for it.
- `modpublished.c` holds a published or My Maps launch whose manifest names a
  mod no installed folder serves (the asset rule, below), asks the player, and
  fetches and resumes it or drops it, as in
  [Published maps and missing mods](#published-maps-and-missing-mods).
  It also holds a rawmap load whose mods are missing, as in
  [Rawmaps that name mods](#rawmaps-that-name-mods), and a prefab placement
  whose mods are missing, as in [Prefabs that name mods](#prefabs-that-name-mods).
- `modexport.c` writes and reads the ZIP exports of maps and prefabs: the
  `.json` and each mod's installed folder, byte for byte; import installs
  each carried folder the asset rule does not find served already as a folder
  from elsewhere (`sh_ms_install_as`), as in
  [Rawmaps that name mods](#rawmaps-that-name-mods). It has no network code.
- `prefab_mods.c` keeps a prefab's description, tags, summary and the mods it
  needs in the prefab file's own `snapmapPlus` block (`prefab_block.c` finds,
  inserts and strips it, and removes the serializer's comments so the file is
  plain JSON), reads an older prefab's sidecar read-only, resolves
  the mods before Load / Place, and writes and reads a prefab's ZIP, as in
  [Prefabs that name mods](#prefabs-that-name-mods).
- `modmigrate.c` moves the retired `overrides\` tree into the store once (the
  store tool runs it, and `sh_mods migrate` by hand), as in
  [Migration of the retired overrides folder](#migration-of-the-retired-overrides-folder).
- `modsound.c` delivers the Wwise sound banks bundles carry, as in
  [Mod sounds](#mod-sounds).
- `modvt.c` registers each private virtual-texture family a bundle carries
  (`virtualtextures/mods/<id>/<family>_sq<N>.mega2`, the builder's only form of
  `.mega2`) as one page-source record, as in
  [Mod virtual-texture families](#mod-virtual-texture-families).
- `modpublish.c` uploads a map's bundles when the map is published, claims
  them under the published item, releases the claim at unpublish, and refuses
  a copy to My Maps that the map service would reject, as in
  [Publishing a map with mods](#publishing-a-map-with-mods).

The store. A folder is `mod.json` plus `assets\`. `mod.json` has
two blocks in one object: every key but `snapmap-plus` is the author's (id,
name, version, description, author, strings, requirements, hud, shadows,
stock, and any other key, kept and carried as the manifest's `extra`), and
`snapmap-plus` is the product's, the whole built manifest: format, build,
bundle hash, built time, validated and cause, origin, shadows, a converted
package's legacy record, `files` (path, sha256, size, provides, needs) and
`stock` (pointers; `"inAssets": false` when no file stands behind one). The
product writes the block by splicing: the old member is cut from the end of
the member before it, the new one appended after the author's last member, so
the author's bytes, order and formatting never change. The canonical manifest
is derived from both blocks in memory, for the hash and for the upload; its
text is the Contract 1 text, so an unchanged mod keeps its hash. There is no
blob store and there are no hard links: a mod's files live in its own
`assets\` only. A folder the author drops in keeps its name; one the product
makes (an install, a download, a migration, a delivered package) is `<id>`,
else `<id>@<version>`; an export's import keeps the carried folder's name when
no folder of its id is installed and the name is free. A package a map
delivered without a version gets one
generated from its digest (`legacy-<digest16>`); it is part of the manifest
and its hash, so it stays in `mod.json`, but it never names a folder (`<id>`,
else `<id>-<hash 8>`) and the Mods tab and the doctor do not show it.
Identity is the id: two folders declaring one id are both kept and both
flagged. A download whose content an installed mod of the same id already
serves is not stored; its hash is recorded as an alias of the installed
bundle. A package a map delivered is not stored when an installed mod that
was not itself delivered declares its id (whatever the content), or provides
every path it provides under another id (a copy renamed by its packager);
the installed mod answers the carrier, and the second upgrade step records
the copy's hash as an alias of it. A folder built again with new
content (the rebuild rule, `sh_mods build`, an install replacing the folder of
the same id and version) records the hash its product block named before as an
alias of the new one, so a map saved against any earlier build loads the
current one. The aliases stay flat: an alias naming the old hash is re-pointed
at the new one, so one hop always resolves. After each pass of the rebuild rule
the store drops every alias whose target no folder builds to any more (a
folder deleted or replaced by hand), with one `MODS: store:` line. Aliases are a lookup aid for
store downloads and exports only: upload sends and claims the current build,
and export carries it and rewrites the exported map's rows to name it, both
through `sh_ms_resolve`. Nothing refuses or brings anything back because of an
alias. Saving a map writes rows from the serving index, so they name the
build that serves. Removing a folder keeps its aliases while a folder of that
id remains and drops them once none does.

The asset rule. A map, rawmap or prefab needs the paths its manifest rows
list (the placed names, `mods/<id>/...`); a mod folder provides them; the
bundle hash is a download key. `sh_modserve_row_served(row, cause, cap)`
(`modserve.c`, `sh_modserve_row_served_for` with the word "prefab") is the one
predicate every hold uses: a row is served when an admitted bundle declares
its id and every name it lists resolves through the serving index to an
admitted bundle, whichever provides it (a placed name, any provided name, or a
served request path); missing when no admitted bundle declares the id;
incomplete when one does but a listed name does not resolve. Its cause is the
refusal: "This map uses Hell Guards (local.hell-guard), which is not
installed." or "Hell Guards (local.hell-guard) is installed but does not
provide mods/local.hell-guard/hell_guard_combo_enc, which this map uses." (the
first three missing names, then "and N more"; the folder's own name when one
declares the id, else the id alone; "prefab" for a prefab). A folder rebuilt
with new bytes at the same paths therefore still serves every map that used
it, whatever its hash. Activation follows the same rule
(`sh_modmanifest_activate`): each row is activated through the build that
serves its id now (`sh_modserve_bundle_serving`). The per-map listing of a
bundle that shadows stock paths or sets requirements stays keyed by id (the
scope listing, [Map scope](#map-scope)).
`mods\snapmap-plus.json` is the product's one record:
`claims`, `links`, `pending`, `migration`, `cache` (tree hash to bundle hash)
and `aliases`, each section written whole under one lock.

Removal goes to the Windows Recycle Bin. Uninstall, Remove and the removal of
covered builds move the folder there (`sh_ms_delete_mod_folder`: a direct
child of the store root, under the store lock, through `SHFileOperationW`
`FO_DELETE` with `FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT |
FOF_NOERRORUI | FOF_NORECURSEREPARSE`, a double-NUL-terminated plain path,
reparse points moved as themselves and never entered), drop its aliases once
no folder of its id remains (`sh_ms_alias_forget`), and rescan; the build cache
follows at that rescan. `sh_ms_remove_folder`, which the store upgrade and an
import's rollback use, takes the same route under the same lock. The shell is
offered only a folder on a fixed drive whose plain path is shorter than
`MAX_PATH`; on a network or removable drive there is no bin and the shell would
delete without saying so. When the bin cannot take the folder (that drive or
path, or a shell error, or a folder the shell leaves in place), it is deleted
outright, links removed and never followed, and the `MODS: store:` line ("deleted
mods\<folder> outright; the Recycle Bin could not take it (<why>)") and the
result text ("Its folder was deleted because the Recycle Bin could not take it
(<why>).") say so; otherwise the line reads "moved mods\<folder> to the Recycle
Bin". The product never brings a folder back: a map that still needs it
refuses to open with the asset rule's sentence until the player restores the
folder from the Recycle Bin by hand (the folder watcher's rescan builds it
again) or the download offer of a published map, a rawmap or a prefab installs
it again. Uninstall also clears the open map's undo history (see [The Mods
tab's map scope](#the-mods-tabs-map-scope)), so Ctrl+Z cannot bring back what
used the mod.

The retired recycle bin. Earlier builds moved removed folders to
`removed-mods\` beside `mods\` and recorded them in the record's `removed`
section. The store tool's `migrate`, after the store upgrade, runs
`sh_ms_bin_sweep`: every entry of `removed-mods\` goes to the Windows Recycle
Bin (links removed, never followed), then the empty folder and the `removed`
section go, with one line per entry ("removed-mods: <name> moved to the Recycle
Bin") and one summary ("removed-mods: 3 folder(s) moved to the Recycle Bin; the
bin is retired"); a later run finds nothing. An entry that cannot be moved
stays for the next run.

The store migration tool. The game runs no store migration at boot.
`snapmap-plus-store.exe` (`src/store/`) links the store sources (`modstore.c`,
`modmigrate.c`, `modlegacy.c`, `modbundle.c`, `modstock.c` and what they use)
with a stand-in for the serving layer (`store_seam.c`), and no engine, DLL or
WebView code. `check <store>` lists the work it finds, one line per kind
(folders of the retired layout, `blobs\`, `legacy\`, the old record files, the
second upgrade step, `removed-mods\` or its record section, an `overrides\`
folder the record does not mark as migrated) and exits 0 when there is none, 1
when there is, 2 on an error; it changes nothing (`sh_ms_upgrade_scan`).
`migrate <store> [--doom <folder>]` refuses while DOOM runs, then runs the
layout upgrade, the bin sweep and the overrides migration, each only when
`check` finds its work, with every delete through `sh_ms_set_discard` to the
Recycle Bin (`store_recycle.c`: `SHFileOperationW` with `FOF_ALLOWUNDO`, on a
fixed local disk only, links removed first). The overrides migration needs the
game's archives for its stock pointers: the `--doom` folder, else the one Steam
names; without either it is reported and left. It prints the store's own log
lines as its action lines and a summary, and exits 0 when nothing is left to do,
1 when a later run could still do something, 2 on an error; a second run finds
nothing and changes nothing. The installer runs it on install and update (see
[installer/README.md](../installer/README.md#mods-and-retired-package-folders)).
The rebuild rule is unchanged: a folder without a product block is built at the
next rescan.

The folder watchers. The lists follow the disk. The window's host
(`snapmap_plus_ui_webview.cpp`, `folder_watch.h`) runs one thread that watches
`%LOCALAPPDATA%\snapmap-plus\prefabs\` and `mods\` with
`ReadDirectoryChangesW` (subtree; file and folder names, last write, size). A
change in the prefabs folder refreshes the Prefabs list 300 ms after the last
change of a burst (the page keeps its selection while that prefab exists and
clears Prefab Details otherwise). A change in `mods\` asks for the ordinary
rescan (the Refresh button's, as `SH_MODS_ACT_WATCH_RESCAN`) after 1500 ms of
quiet, on the main thread through `run_on_main`; the backend defers it while
Play runs, a map loads or a store write holds the lock, and the host asks again
two seconds later. The Mods tab re-renders from the document that follows. The
product's own writes fire the watcher too: a change seen while the store lock
is held (`SH_MODS_STORE`, `modstore_lock_busy`), or within a second after a
hold ended, is dropped, so an install or export in progress never triggers a
rescan mid-write, and the watcher re-arms with the next change. A rescan of
unchanged trees is cheap by the rebuild rule. Each refresh and rescan logs one
UI-log line with its reason (`watch: ...`). A folder that does not exist yet is
opened again every five seconds.

The rebuild rule: a folder is built again when its tree hash (the author's bytes of `mod.json`
and the path, size and time of every file under `assets\`) is not a key of the
cache for its bundle; that runs on a rescan (the Mods tab, `sh_mods rescan`,
an install), never at boot, because a build needs the game's resource reader.
The store tool's `sh_ms_upgrade` converts the retired layout once (before the
overrides migration): every
`<id>@<version>+<build>\` folder gets `mod.json` from its `bundle.json` and
`.state.json` (read back with the same hash before the old files go), hard
links become copies (the linked file is renamed aside, copied back to its name
and checked against its hash, and only then is the renamed original retired;
on any failure the original is put back, and one an interrupted run left aside
is put back at the next run), a missing file is restored from `blobs\`, product folders
are renamed to `<id>` (or `<id>@<version>` when one id has several), `legacy\`
folders move into `mods\` or are dropped as aliases, `blobs\` is removed, and
`claims.json`, `publish-links.json`, `pending-uploads.json` and
`.migrated.json` are folded into the record. Everything it retires goes to the
Recycle Bin. It logs one `MODS: store:` line per folder and a summary (the
tool prints them), and every step can be repeated. A second step runs
once per store, also on one that went through the first already (the record's
`layout` section becomes 2): it removes every delivered package an installed
mod answers by the rule above and aliases its hash, renames a product folder
left alone with its id under `<id>@<version>` to `<id>` and any folder named
after a generated version, and collapses a completed `migration` section to
`complete`, `migratedAt`, `source`, the rows of folders not done (skipped,
refused or pending) and `trees`, which maps each converted overrides tree to
its folder and still resolves maps published before Contract 5 (aliases map
hashes, not trees); the folder names in it follow the drops and renames. A
pass of the overrides migration writes the same collapsed section when it
completes, and the Migrate button counts an `overrides\` entry the section
does not list as done unless it was created or written after `migratedAt`.
The product block's `legacy` record (tree fingerprint, grandfathered shadows)
is kept as it is. One `MODS: store:` line per dropped or renamed folder and a
summary.

A bundle file may be a stock pointer instead of carried bytes: Contract 1's
file kind `stock`, `{"needs", "path", "provides", "request", "stock":
{"sha256", "source"}}` with no `blob` and no `size`, where `source` is an
installed resource path and `sha256` the hash of the installed bytes
(`modbundle.h` has the grammar). The builder makes one of every file whose
bytes equal installed content, and an author writes one in the descriptor's
`stock` object (`{"<request>": {"source": "<installed path>", "sha256":
"<optional>"}}`, the hash filled in by the builder) to alias installed content
under a mod name. Pointers do not count toward the 64 MiB and 256 MiB caps and
the extension whitelist does not apply to them, because nothing is carried;
the store never receives their bytes (no copied file, nothing uploaded), so a
converted package or a copy-of-stock mod uploads only its authored and edited
files. `modstock.c` answers them from the player's own
install, read-only: it parses `base\snap_gameresources.pindex` and
`base\gameresources.pindex` once, on the first question (about 171,000 rows
with bytes, 20 MB held, 125 ms on build 13954591; an empty `mods\` never loads
it), reads a row's slice from `.resources` or `.patch` with one positional
read, inflates the engine's sync-flush raw DEFLATE (`raw_deflate.c`), and
falls back to loose files under `base\` and the install root (a `.pages`
virtual texture). It reads the archives directly rather than through the
engine provider because SnapMap's provider does not see campaign-only
content in `gameresources`, which is what the boss packs copy. Identity is
size first: the builder hashes only entries whose decoded size equals the
file's, at the file's own path, then entries of the same size and extension
elsewhere (a renamed copy; at most 64 candidates and 256 MiB hashed per
file), and each slice is hashed at most once per process; measured on the
owner's packs, a build hashes about the bytes that become pointers, never the
archives. Installing a downloaded bundle resolves every pointer first, and a
game that lacks the entry or holds other bytes (a different revision) refuses
the bundle with that first cause before anything is fetched; `sh_mods verify`
checks pointers the same way. The serving index never reads the archives at
boot; a pointer is read when it is first opened, accepted only with its hash,
and a pointer the install cannot answer falls through to the engine with one
logged line. For the conflict rules a pointer's bytes are the installed bytes
(its hash stands for its blob), so a carried copy and a pointer to the same
bytes are one file, and a composed list decl reads a pointer's installed text.
The builder's and the Mods tab's stock-name checks ask the bridge when the
engine provider does not have a name.

The provider open (`ov_open_body` in `overrides.c`) serves, in order: the
placement grid, map navigation, the generated Grid Room variants, installed
mods, the built-in defaults, then the engine (a mod's version of a built-in
name, such as a player's expanded Lift tile, wins over the baked default, as the
retired overrides layer did). Opens with mode 2 or higher are
never served. A shadow applies while a map that lists its bundle is open or
played (see [Map scope](#map-scope)); a new name applies once it is registered: editor rows at the first palette build (or
`sh_mods palette rebuild`, or the load of a map that names their bundle, below),
gameplay decls when a map that names their bundle loads. When two builds of one mod id serve a path, the most recently installed
serves it, and a map naming the older bundle gets the newer files with a logged
line.

Cooked movies need a second route (`modswf.c`). A `.bswf` is an idFileResource,
and its Load (vtable `+0x40`, signature `FileResourceLoad`) opens through the
provider only while the map-load lifecycle state is below 2; from 2 on it asks
the resource manager (`+0x68`), which reads the mounted containers unless
`resource_loadLooseAssets` is set, and a miss becomes an empty file returned as
success, so a bundle's movie used to load blank with no message. `modswf.c`
swaps that one vtable slot (the only read-only qword holding the Load; no code
is patched). For a `generated/swf/...bswf` request the serving table answers,
it runs the engine's own below-2 sequence at any state: a named memory file at
the resource's `+0x38`, then the open-into helper, which opens through the
provider and copies the bundle's bytes. Everything else, and a served movie
whose open fails, goes to the original Load. The four engine calls it uses are
decoded from the resolved Load body, and the open-into helper must call
`+0x148`. The cvar is not used: it is read by four engine paths on each
executable, two of them generic opens, and would make every file and JSON
resource load walk the search paths on disk. A served movie applies at that
resource's next Load: a movie first loaded in a map is purged at map unload, so
a shadow installed between maps applies at the next map load; a movie resident
since boot is served only when its bundle is installed at boot. The builder
admits `.bswf` only under `generated/swf/`, names it as the consumer does
(`swf/<stem>.swf`), and applies the usual rules: a stock movie is a listed
shadow, a new one lives under `generated/swf/mods/<id>/` with its atlas
`generated/swf/mods/<id>/<stem>.bimage` beside it. Each served movie logs
`MODS: swf: served <request> (<bundle>) at load state N` (the first 32).

Display text reaches the engine's `#str_` table through `strids.c`, which reads
three layers and keeps the first writer of each key (case-insensitive): the
user's `%LOCALAPPDATA%\snapmap-plus\strings\strids.json`, then the
`strings.en` object of every admitted bundle's author block (installed mods
before bundles a map delivered, then by id and version, through
`sh_modserve_each_bundle_extra`), then the baked defaults. Keys omit the
`#str_` prefix decls use, and only `en` is read. When two bundles give one key
different text the first stands and one `B1: strids REFUSED` line names both;
identical text is not a conflict. The first injection runs after the boot index
is built, so it already carries bundle strings. Every later index build
(`sh_mods rescan`, an install, a migration, a fetched bundle) requests a
refresh that re-reads the layers and re-sorts the table on the main thread: at
once when the build ran there, otherwise through the apply call slot or at the
next editor palette build or map activation. A refresh updates changed text in
place and retires keys no layer supplies any more.

### Mod sounds

A sound decl a mod carries (`generated/decls/sound/mods/<id>/<stem>.decl`)
names the Wwise event `play_<stem>`: the engine computes the event id from the
decl's own name, so a decl whose stem is a stock event's (an alias) plays that
stock sound with no bank, and one whose event lives in the mod's own bank
plays once that bank is loaded. The builder makes a `soundShader` or `sound`
key naming `mods/...` a need of type `sound`; a stock event name is not a need.
A bank is admitted only as `sound/soundbanks/pc/<stem>.bnk` (Wwise opens banks
by file name in that one folder), with the stem `<id>_<rest>` (the id's `.` and
`-` as `_`), letters, digits and `_` only, and without `_sp`, `_snapmaps` or
`initial`, which the engine's listed-bank filter skips in some game mode; its
BKHD chunk must be version 113 and carry the id the game loads it as, FNV-1 of
the lower-cased stem, so a renamed bank is refused rather than loading under
the wrong id. A bank is always carried: never a stock pointer, never a shadow.
A converted first-generation package keeps its banks as plain files.

Wwise reads banks with its own `CreateFileW`, not through the provider, and
the engine lists the bank folder only inside LoadBanks, so `modsound.c`
delivers them itself, with two vtable-slot swaps and no code patch: the
resolver's open by name (`AudioFileOpenName`, held in two read-only vtables)
answers a served bank's file name by opening the bundle's file and filling the
`AkFileDesc` as the engine's default hook does, and every served bank is
submitted to the engine's own per-bank wrapper (`AudioBankSubmit`, the
filter and the async LoadBank) on the main thread inside process heap 0, at
the first editor entry or map load once Wwise is up (the hardware object is
`soundSystem+0x140`, decoded from the `s_loadBanks` function), and at once
when `sh_mods install` rebuilds the index. The engine prints its own result,
`async load success id <id> : <name>` with the id as a signed number. The
LoadBanks slot (`AudioLoadBanks`, hardware vtable `+0x30`) resubmits every
bank after `s_loadBanks` and `s_restart`, and runs them in process heap 0:
Wwise allocates through the engine allocator's current heap, so a rerun from
the console with a map open put the reloaded stream packages in the map heap,
the map unload freed them, and the next streamed sound read a freed package
(a crash at the Play load). A mod installed mid-session plays at
its next map; a bank whose bytes change keeps the loaded copy (Wwise keeps the
first bytes for a bank id) until the game restarts, and a removed bank stays
loaded until then. `sh_mods status` lists each bank and whether it was
submitted. The events file (`soundbanksinfo.events`) is not extended: a mod
event needs no record to play (it only feeds the by-id lookup; without one
the event is never distance-virtualized).

### Render programs

A mod can ship a new render program `mods/<id>/<name>` (no `.` in the last
component) and a material that names it in quotes; the engine opens every file
of it through the provider and needs no restart. The builder checks what the
engine checks (`mb_check_renderprogs`): each `_pc_vulkan.bin` (and an OpenGL
`_pc.bin`, if shipped) starts with 0xB68800AD and carries MD5fold of its decl
text, trailing spaces and line ends removed (otherwise the engine silently
compiles the text); each SPIR-V header is `{4, hashA, hashB, length}` with
hashA equal to MD5fold of the matching stage text in the `.bin`, which the
engine enforces at a map load with "out of date"; and every stage and
permutation is present: the decl, the `.bin` and `generated/spirv/<name>.vspv`
and `.fspv` for the program, and the same for each
`generated/renderprogs/permutations/<name>_<mask>.decl` under
`generated/spirv/generated/renderprogs/permutations/` (Vulkan binds the base
files on GPUs without AMD's three shader extensions and the permutation on GPUs
with them; a missing stage file is fatal at bind). A permutation decl is named
by its request path, the string the engine looks it up by; it is served, not
registered. A bare program name in a material, shared includes under
`decls/renderprogs/`, `vulkan_pipelines.bin`, `generated/glslBinaries/` and
`generated/glsl/` are refused. MD5fold is MD5 with its four words XORed
together. OpenGL compiles the decl text by default (`r_useGeneratedRenderprogs`
0) and may write the permutation decl into the player's `base\generated\`.
The first open of each served render-program file logs
`MODS: renderprog: served <request> (<id>@<version>, <n> bytes)` (the first 64),
which shows the variant this GPU binds.
### Mod modules

A bundle module is `maps/modules/mods/<id>/<leaf>.decl` (provides `snapModule`,
named by that path, the name a map's `moduleName` holds) plus
`generated/decls/snapmoduleinfo/mods/<id>/<leaf>.decl`, which the builder
requires in the same bundle. Every other file is a name the engine derives: the
render model `maps/modules/palettes/mega_blessed/mods/<id>/<leaf>.bmodel` (the
loader's palette group is the stock palette's, `mega_blessed`), and under
`maps/modules/mods/<id>/<leaf>/` the collision `_combo/world.bcm` and
`<leaf>_floor_collision.bcm`, navigation `generated/.../<leaf>.baas_monster48`,
96 and 128, `<leaf>.sbsp`, `<leaf>.ambientsh`, `_combo/_world.tome` and
`lightprobes/<entity>_compressed.bimage` (the probe path is built from the
module's `name` and the probe entity's name). A multiplayer map's files are
byte-identical stock pointers at those names. The per-map kinds `.sbsp`,
`.ambientsh`, `.tome` and `.pages` are admitted only as pointers
(`sh_mb_pointer_only_kind`). A pointer that restores installed content under
its own name (`sh_mb_stock_restore`: the request is its source, or
`generated/decls/<type>/<source>.decl` for an own-path decl such as a map's
`mega.decl`, which models name with `.decl`) sits outside the namespace without
a `shadows` entry; the activation plan treats it as a shadow, leaving it to the
provider when the engine knows the name and registering it when it does not
(SnapMap does not see multiplayer content in `gameresources`). Placement reuses
the Grid Room route: the FindModule and HasModule detours ask the serving index
(`sh_modserve_module_served`) about a name the stock palette lacks, and a served
module is loaded with the native loader into a private one-module palette whose
loader context comes from the stock palette's first record; the snapModuleInfo
is registered the same way as a variant's, without the one-floor, four-wall
check. Its snapModuleInfo and collision are checked in the serving index,
never read through the Grid preflight (which claims only Grid Room names); a
refusal logs `MODS: module <name> refused: <step>` once, and a load logs
`MODS: module <name> loaded into a private palette (mega_blessed)`. The map
validator (RepairAndMigrate, VK 0x1412488e0 / GL 0x141247f10) asks HasModule
for every instance near its end, drops a module it refuses ("Couldn't load
module") and then returns 4 when the entity check fails or no instance is
left. The private palette's collision is retained across Play like a
variant's. The sound BSP, light grid and occlusion tome (and probe images if they come
the same way) are idFileResources that BuildSoundWorld, SetTome and
LoadAmbientLighting materialize at Play, when the Load reads the mounted
containers; `modswf.c`'s Load slot serves a bundle's copy through the provider
as it does a movie, logging `MODS: file: served <request> (<bundle>) at load
state N`. There is no Add-browser tile; a saved map or a module prefab that
names the module places it. The module decl's bounds decide the placement
limit check; for a multiplayer map they cover the arena, so it fits the native
16000 bound at the origin.

### Mod virtual-texture families

A material binds to a virtual-texture family only when a page-source record
with exactly the material's `landpagefile` name exists when the material is
parsed; otherwise the engine classifies it as a unique family and it draws
with default images. The engine creates such a record for a private family on
no path of its own and never reads a private family's `.vmtr` rows, so
`modvt.c` calls `idVirtualTexture::SetSource(record, "mods/<id>/<family>",
"virtualtextures/mods/<id>/<family>.pages", flavor, 2, 0, NULL)` per family.
SetSource's mega2 loader opens the served `_sq1` to `_sq16` members through the
provider; no `.pages` file is needed, and materials carry an explicit
`virtualmapping` instead of rows.

- Thread and time: DOOM's main thread, inside process heap 0, once the VT
  system has built record 1 (`_vmtr`). The serving index calls a listener after
  every table it publishes; on the main thread (`sh_mods install`, rescan) the
  families are registered at once, on another thread (boot, a fetch, a
  migration) at the next decl registration pass. Every pass (the editor
  palette build, a map's activation) registers first and then parses, so no
  material of a bundle is parsed before its family's record exists. A sync is
  idempotent.
- Records: 32 slots, records 1 to 31. Record 31 is never taken
  (VirtualTextureForMaterial releases it on any lookup miss), no record the
  product did not create is touched, and a family that finds 1 to 30 in use is
  refused with one `VT:` line. Each record costs about 14 MB (11,184,810 bytes
  of host page table for the fixed 12 levels, the reader's index and the
  page-table image; 13.9 MB measured on Vulkan).
- Release: when a family's bundle leaves the index (or another build replaces
  it) and no registration pass has run since the family was registered, its
  record is released. After a pass its materials may hold the record, and the
  decls stay registered for the process, so the record is kept until DOOM
  exits, with one line.
- Renderers: flavor 2 on Vulkan; on OpenGL, flavor 4 when the AMD sparse
  texture extension and its cvar are both set (the test the engine's own
  lookup makes), and no registration otherwise.
- `sh_vt` (`vt_harness.c`) is the diagnostic on the same binding: `list`
  marks the product's records `(product)`, `sync` runs a sync, and its manual
  `register`/`release` never touch a product record. The member format is
  [vt-baker.md](vt-baker.md).

A mod's `hud` key (the author block of `mod.json`) selects how
the stock ammo widget shows a weapon (`modhud.c`, the beta.19 `weapon_hud`
contract: `{"weapons": {"<weapon decl>": {"ammo_display": "weapon" | "engine"}}}`).
`weapon` removes the multiplayer mode's forced numeric display for that weapon,
so a nonpositive capacity shows the widget's infinity frame; no ammunition,
decl or widget field changes. Rules are read at startup from every admitted
bundle, installed or legacy; identical rules compose and conflicting ones
refuse the whole table with both bundle ids logged. The widget update's one
call to the mode predicate (`WeaponHudModeCall`, `WeaponHudGameMode`) has its
aligned rel32 swapped to a near relay that passes the widget, whose weapon decl
is at `+0x1F8`; every other caller of the predicate is untouched. Logs:
`weapon-hud compiled: N rules`, `weapon-hud installed: ...`, and
`weapon-hud active: ...` on first use.

When bundles of different mods provide one path with different bytes, one of
them always serves it: leaving it to the engine broke every boss pack at Play,
because their other files are served and name it, and the game's SnapMap
archive has no copy of campaign content to fall back to (the Cyberdemon's
animWeb defaulted to an empty one and `AnimWeb ... has no nodes` ended the
process). The winner is the provider with the fewest needs no admitted bundle
provides, then the bytes most providers carry, then an installed mod over a
copy a map delivered, then the most recently installed. When every provider's
text is a decl that differs from the others only inside lists (a block of
`num = N;` and `item[i]` members, as the encounter conductor's `aiTypeList` or
the whitelist inspectors' `validEncounters`, which every boss pack extends),
the winner's text is served with the other providers' entries appended, so
several boss packs work together. One `MODS: conflict:` line names the
providers, the winner and the reason; `sh_mods doctor` prints the same. A
registration pass (editor rows, map activation) also withdraws an animWeb
whose `modelInfos` or `models` block names a model that no bundle provides and
the engine cannot find. The engine's parse looks each model up with DeclFind
(no makeDefault) and a miss throws an `idLexerException` that the animWeb
parse catches itself (13954591, both renderers): the web keeps what it had
parsed (nothing past `props`, its nodes freed first), is not marked failed,
and the load reports success. So for a decl the pass loads now, the engine's
own lookup decides, not the game archives, which also hold campaign md6Defs
SnapMap cannot load (issue 132: a converted doors package carrying
`designsystems/med_station`, which names the campaign
`wallhealthstation.md6`); a stock shadow the engine parses later keeps the
archive answer. A stock animWeb it shadowed is served by the game again, and a
new name is not registered. A load the engine refuses although it reports
success is caught by its error text naming the decl's file (the reporter
copies it to `last_error_msg` before the throw); the decl is not loaded a
second time and is withdrawn. A withdrawn file is not parsed again in the
session, and each skip or refusal prints one console line,
`Snapmap+ mods: <bundle>: animWeb '<name>' not loaded: it names model
'<md6>', which SnapMap cannot load (campaign-only or missing)` or `... not
loaded: the game refused it: <engine error>`. Before
the Play level load the SnapBuild detour re-checks the bundles' requirements
(`MODS: play: requirements live: ...`); the engine itself does not reset the
blacklist gates between the editor and the level load. The editor pass
also raises to the session lifetime (4) the decls an editor row points at
directly (its editor animWeb, its md6 render model, its preview material): the
row is kept for the session, but those objects were created after the boot
promotion at level 1, so the purge on entering Play freed them and the editor
entity rebuilt after Play bound a freed animWeb. The rest of the closure (an
md6Def's MD6 model and skeleton, an animWeb's anims, a material's images) is
raised the same way: the pass takes a watermark of the engine's resource
lists before it runs and keeps every resource created since then that is
still at level 1, and every step of the editor plan is registered at the
session lifetime.

Requirements. A bundle's `requirements` (`{"cvars": {...}, "commands":
{"activate": [...], "deactivate": [...]}}`) are read only through the builder's
canonical form (`sh_mb_requirements_canonical`): sorted cvar names with string
values, `true`/`false` as `1`/`0`, console lines trimmed and in the author's
order, `"cvars"` always present and `"commands"` only when not empty, so a
bundle carrying only the two blacklist gates keeps the text and hash it had
when they were the only accepted requirement. The product names no cvar or
command; the bounds (`SH_MB_REQ_*` in `modbundle.h`) are syntax only, and the
Community API's manifest check mirrors them. `modserve.c` takes the admitted
bundles in install order and drops the requirements of one that asks a cvar an
earlier one asks at another value (one `MODS: requirements: conflict:` line
names both). Before every registration pass, at the Play re-check and after a
rescan on the main thread outside Play, it applies them: the deactivate lines of
bundles whose requirements stopped, then each asked cvar set and read back
(`idCVar::Set` with force, after `modserve_engine.c` refuses a read-only or
command-line-only cvar from its flags; a set queued as console text is drained
first), each cvar no bundle asks any more restored to the value it displaced
unless someone else changed it, then the activate lines of bundles that start
now, appended with `BufferCommandText` and logged as `MODS: requirements: <id>:
command "<line>"`. A bundle whose cvars are not all live has its steps blocked
with the cause, and its files first served before then are treated as below.
The state lives across tables (a rescan replaces the index, not what was set);
Nothing is restored at DOOM's exit; a cvar the engine archives to its own
configuration may therefore keep a mod's value into the next session.
A bundle's requirements are accepted only while the map scope has its shadows
in (below) or, in a map, when the bundle adds Toybox rows; a pass after the
listing changes therefore runs the stopped bundles' deactivate lines and
restores their cvars as it does after an uninstall. `sh_mods status` and
`sh_mods doctor` print `sh_modserve_requirements_report`, one line per
requirement with that bundle's own state for the open map: live, not active
for this map (with the reason, and the mod that holds the same value when one
does, so another bundle's value never reads as live for this one), refused
(lost a conflict), NOT live with the engine's cause, restored (its
requirements ran and stopped; `modserve.c` keeps the stopped ids until they
start again), or not applied yet. Packages converted from the
retired formats keep only the two blacklist gates (below): their requirements
were never accepted by a player installing them.

On every parse the `DeserializeFromJson` detour sees, `modlegacy.c` runs first.
A complete carrier whose ZIP and `package.json` read is unpacked once into an
ordinary mod folder `mods\<id>\` (older `decls/`, `images/` and `shaders/`
layouts moved under `assets/`; the package's keys become the author block, the
legacy record and its grandfathered shadows seed the product block), built into
a bundle and named by a manifest row; a conversion an installed mod of the same
id already serves is dropped and that mod answers;
the serving table is then rebuilt when that bundle is not in it yet, so the same
parse can activate it. A first-generation package served every file under
`assets/`; a file a bundle cannot carry (a type outside the bundle whitelist,
such as the `.materials.txt` sidecars every beta.19 model pack shipped or a
`.bswf` movie outside `generated/swf/`, an empty file, a path the request rules refuse) is left out and
counted on the `LEGACY: unpacked` line instead of refusing the package, and a
cvar requirement other than the two supported blacklist gates is left out the
same way. A beta.16-19 carrier names its tree (`smpkg.delivery.<tree>`), so
before converting one the converter also looks for a mod folder whose
`mod.json` legacy record has that key (or the carrier's digest) and for a
bundle the overrides migration built from the same tree (the record's
`migration` section, `trees`); either answers the carrier without a second
copy. Any other carrier, and the unreleased `smpbind.` and
`smpref.` rows, are removed. Each carrier logs one `LEGACY:` verdict line. After
the navigation strip, `modmanifest.c` reads and strips the manifest rows before
the engine replicates map variables and activates the bundles they name.
The editor palette is built once per process, and the map validator
(`RepairAndMigrate`) looks every placed entity up in it, so after activation
the same step checks the named bundles' editor rows against the built palette.
When at least one row is absent and was never registered (a package converted
mid-session, or a bundle installed after the build), it runs the heap-scoped
palette rebuild once (the same path as `sh_mods palette rebuild`: main thread,
verified process heap 0, pending rows registered first, the builder with a
NULL progress through the detour, product palette caches reset) before the
buffer returns to the engine, and logs one line:
`MODS: map rows: M of R editor row(s) missing from the palette; K registered,
A now in the palette; palette rebuilt in N ms (B -> C rows)`. It is refused,
with the reason on the same line, while the editor holds an entity or a wire;
before the first palette build it does nothing, because that build registers
every row. A map without manifest rows never reaches it. On
save it appends one row per bundle that provides a placed name (and each bundle
their needs reach) and one per map-level row of the current manifest, after any
navigation shards; with an empty table, no rows in the map and no map-level row
it returns at once and the save is the engine's own bytes. A map-level row is
written for the build that serves its mod (the bundle resolver modui.c binds,
`sh_modserve_bundle_serving`), and the current manifest follows the rename; a
row of a mod that is not installed is kept as it is.

Threads: the table is built on the bootstrap thread before the provider hook is
published and replaced only by a rescan (`sh_mods install`, a build inside the
store, a legacy conversion, the end of the overrides migration's worker,
`sh_mods rescan`). Readers see one complete table;
retired tables are never freed, because a provider open on another thread may
still read one. Registration, activation and the palette rebuild run on DOOM's
main thread only. With an empty `mods\` folder the table is empty and every hook
above passes through: the differences from a build without mods are the
`sh_mods` command and its `MODS:` log lines.

Early decls: the provider hook is published after the backend's signature scan,
about seven seconds into the process, while DOOM's main thread is already
parsing its boot decls, so whether a given shadow was parsed from the game's
bytes or the bundle's depends on that race (the boss packs' `aiFSMManager`
states were served in 3 of 18 boots on 13954591, and 113 and 180 shadows of the
installed packs lost it in the two boots measured after the fix). Each registration pass therefore
re-parses, between publishing its sources and running its loads, every served
shadow decl the provider has never opened and the engine already holds parsed:
`ResourceGenericLoad` with the state bits cleared, inside process heap 0, which
reads the bundle's bytes through the provider. Running it after the sources are
published lets a shadow that names a new identity (an editor `animWeb` naming a
new `md6Def`) find it; running it before the loads gives a new decl that
inherits a shadow the bundle's values. A shadow the engine has not parsed yet is
left to the provider. Each file is settled once; a rescan keeps the mark while
the bytes are unchanged, also when the open landed in the table it replaced.
The pass logs `MODS: reparsed <n> <type> decl(s) the engine parsed before the
serving index (...)` per type and one summary line.

Every in-place load the product runs (this re-parse, the refresh below, the
gated reload of models and images) keeps the resource's lifetime at `+0x28`.
`ResourceGenericLoad`'s failure path reconstructs the object in place and
idResource's constructor writes level 1 or 2 (VK 0x1416c5580), so a permanent
decl that failed a re-parse would be destroyed by the next map unload while
everything permanent that points at it keeps the pointer; the main menu's
collectibles hold stock md6Defs (`unlockModel` decls name the boss and demon
md6Defs that Doomforge packs shadow) and read them on every shell-map load. A
re-parse that fails (state bit 0, or an engine error naming the file) is
withdrawn and the decl is parsed once more from the game's bytes, and an
animWeb shadow whose models the engine cannot find is withdrawn before its
re-parse, since the re-parse would free the stock web's nodes and rebuild
nothing.

The same race decides what a bundle's requirements mean for a resource: its
cvars (the cut-content blacklist gates, for one) are applied only at the first
registration pass, and anything the engine loaded before that resolved its
names with the blacklists on. A model loaded then keeps no materials and draws nothing: this
is why the Spider Mastermind body (stock-named mesh, loaded at boot, its
materials and images on the blacklists) was invisible on 13954591 while its AI
ran, and why the same bytes under a new mesh name drew. The provider therefore
records whether a file was first served while its bundle's cvar requirements
were not live; a decl first served then is re-parsed by the pass above, and each
`baseModel` and `image` first served then is loaded again in place, found by
name in the engine's `idMD6Model` and `idImage` resource lists, once the gates
are live: `MODS: editor: reloaded <n> model(s) and <m> image(s) loaded before
their bundles' requirements were live`.

Refresh: a rescan (the Mods tab's refresh, `sh_mods rescan`, a console build
or install) publishes a new table while the engine keeps every decl it parsed
and every model and image it loaded from the old one. Each table therefore
compares itself with the table it replaces, per served request path: a path
whose bytes differ is changed, one only the new table serves is new, one only
the old table served is gone. Changed decls, models and images and gone
shadow decls are marked pending, and the marks carry over to later tables
while the bytes stay the same. Before applying, the served decls whose
top-level `inherit` names a pending decl are marked too, transitively, since
the engine resolves a parent when the child is parsed (measured: re-parsing
only the Hell Guard's base entityDef left the spawned child at its old
health). Applying uses the two primitives above: each marked decl the engine
holds parsed is re-parsed (state bits cleared, `ResourceGenericLoad` inside
process heap 0), parents before children, a gone shadow so that the engine
reads its own bytes again; each marked model and image it holds loaded is
reloaded in place by name, only once its bundle's gates are live. A decl the
engine has not parsed, or a resource it has not loaded, is settled, since its
next load reads the current bytes, and a settled decl is no early decl. The
rescan applies the marks at once on the main thread, except during SnapMap
Play (the editor inactive with deactivate reason 3): then they wait, and a
watcher thread hands them to the main-thread call slot, which drains in
editor frames, at the return to the editor; every registration pass also
applies them between publishing its sources and running its loads. Editor
content follows in the same step: new gameplay decls of a mod a registration
pass already registered this session are registered at once (the map's
activation plan for just those files), and when editor rows were added,
changed or removed the live palette is rebuilt through the
`sh_mods palette rebuild` path, whose editor pass registers new rows first. A
removed row is re-parsed without a source, resolves no entityDef and is left
out by the rebuild; the strids layer re-reads bundle strings on every
published table, so a new row shows its `#str_` name. Without a palette yet,
the first editor entry builds it with every row; a refused rebuild (an entity
or wire held) is retried by `sh_mods palette rebuild`. What stays reported
rather than applied: new names of a mod no map has used this session (they
register when a map that names the mod loads), a changed movie (read at the
next map load), and changed files nothing reloads in this process (sound
banks, animations and other raw files besides models and images) or removed
non-row decl names, which need a restart; a stock decl no bundle serves that
inherits a changed one cannot be found from the index. The log carries one `MODS: refresh:` line
per category and a summary; the Mods tab's rescan result carries
`refresh: {reparsed, reloaded, needsMapReload, needsRestart, pending,
deferred, note}`, and `sh_mods rescan` (or `sh_mods refresh`) echoes the same
lines to the console; `sh_mods status` says when changes are pending.

### Map scope

A bundle's shadows and requirements apply only while a map that lists the
bundle is open in the editor or playing; its new names are never scoped. The
listing is a set of bundle ids in `modserve.c`, set by
`sh_modserve_scope_set` from manifest rows (hashes resolve through aliases;
ids name rows of mods not indexed) and from the placed closure, with whether a
map is open at all. Each admitted bundle has two flags computed from it and
read lock-free: `shadows_off` (a map-scoped bundle the listing lacks) and
`req_off` (the same, except in a map for a bundle that adds Toybox rows, whose
rows the palette registers whatever map is open). A withdrawn shadow misses
`sh_modserve_lookup`, `sh_modserve_open` and the decl probe, so the provider
falls through to the engine; the early-decl and gated-reload visits skip it;
reports read the index through `sh_modserve_lookup_indexed`. Admission is per
path, not per winner: `ms_resolve` records a shadow path's other providers on
its winner (`alt_first`, in the conflict rules' order for different bytes,
index order for identical bytes), and when the winner's bundle is withdrawn
`ms_scope_compute` picks the first provider the listing admits
(`scope_alt`), whose own file then serves the path (`ms_scoped`). So a path
two installs carry with the same bytes (a renamed or forked copy) is served in
a map that lists either one, and a conflict whose winner is unlisted is served
by the listed provider the winner rules rank next, uncomposed; only a path none
of whose providers is listed falls through. `sh_mods doctor` adds, per shadow
or conflict path, which provider serves it for the open map.

When the listing changes, every shadow decl, model and image whose serving
bytes changed (withdrawn, admitted, or moved to a provider with other bytes;
moving between identical copies marks nothing) is marked pending at generation
0 and the inheritance closure is marked again, and `sh_modserve_each_refresh` offers them
with `gone` set while withdrawn (no gate needed for the game's bytes). The
engine side is the refresh of a rescan: `sh_modserve_scope_apply` runs the
requirements pass first, then `me_refresh_apply` (re-parse in process heap 0,
reload in place by name) and the gated reloads. The listing is set and applied
at:

- a map load: `modmanifest.c` calls the scope hook (`modui.c`) with the rows
  just read and, when `sh_modserve_scope_relevant`, the map's placed names,
  before the bundles are activated, and applies at once;
- a save: the hook gets the rows written (placed closure and map-level rows);
  the change is applied on the next editor tick;
- the Play entry: Play does not serialize the map, so `sh_modserve_play_begin`
  asks the scope source (`modui.c`) to read the current manifest and the live
  placed names, then applies before the requirements check and the level load;
- the map exit: the main-thread tick (`sh_modui_poll_main`, polled while scope
  can matter) clears the listing once the editor that showed the map loaded
  last has its screen down, neither Play nor a load runs (`load_state` reads
  3 for any loaded map, so the loading-screen thread's running byte,
  `load_screen` `+0x4b2`, marks a load in progress, the hub's shell map
  included), and the engine's
  `map_left` (`iface_engine.c`) says the editor has left the map: no map
  object, or the editor state at `+0x23618` is 0 (no state runs: the hub or
  the browser) or 8 (exiting) with the deactivate reason at `+0x2366C` not
  equal to 4. The Settings screen opened from the Start menu also takes the
  editor screen down and also reads state 8, but with reason 4 (shutdown
  reads 1), so the listing stays. The tick sets the listing again from the open map when an
  editor map is open without one;
- the Mods tab's and the console's map add and Remove from map, at once.

With no map (at boot, and at the hub after the editor) no map-scoped shadow
serves and no map-scoped requirement is live. A published or My Maps launch
from the hub keeps its listing until the next parse, because nothing marks the
end of that Play. A `mod.json` author key `"scope"` (`"map"`, the default, or
`"always"`; the builder refuses any other value) stays in the manifest's
`extra`, so a mod without it keeps its canonical text and hash. A bundle
without the key whose shadows include editor-time content is `always`, logged
once per build: a `generated/decls/snapeditor*`, `snappropertyinspector*` or
`snappalette*` decl, or the editor's own movies under
`generated/swf/snapmaps/` (`snap_editor`, `entity_menu`, `list_view`).
`sh_modserve_index_boot` enables scope for the product's index; the native
tests enable it where they test it (`sh_modserve_scope_enable`).
`sh_mods status`, `sh_mods doctor` and `sh_mods map` print
`sh_modserve_scope_report`, one line per bundle with its scope and whether it
is active for the open map. The map load's `MODS: map manifest names ...` line
ends with the registration steps the activation found ready, or "no new decls
to register" for bundles of shadows and requirements only.

### Migration of the retired overrides folder

Releases up to v0.2.1-beta.19 kept packages in `overrides\`. The store tool's
`migrate`, after the store upgrade and the bin sweep, runs `modmigrate.c`'s
pass once when `overrides\` exists and the record's `migration` section does
not (or records an unfinished pass), with the `--doom` folder as the stock
bridge's install (`sh_modstock_set_install`):

- each direct child folder with `package.json` or `mod.json` is converted the
  way a carrier is (layout moved under `assets/`, files a bundle cannot carry
  left out) into a staging folder outside the store and installed by
  `modstore.c` as `mods\<folder>\` (its own name; with the tree's first 8 hex
  when another mod holds that name), `mod.json` and real files. The package's
  keys are the author block: its id (or, for an era B pack without one, its
  folder name), version (else `0.0.0`), requirements, strings and hud, and its
  other keys (`composes`, an era B `schema`) kept; the product block's `legacy`
  record (`source`, `folder`, `tree`) grandfathers its names as shadows. Two
  folders with one id are both built, each in its own folder, and the duplicate
  is reported;
- each delivered package, `overrides\map-<16 hex>\<package>\`, becomes a mod
  folder `mods\<id>\` keyed `delivery.<tree>`, the beta.19 tree
  fingerprint (entries sorted case-insensitively, then by bytes; a `d` or `f`
  byte, the NUL-terminated relative path and the file's SHA-256), which is the
  key a map's carrier of the same delivery has; a later load of that map reuses
  it;
- a folder without a descriptor whose only package is one direct child folder
  (beta.19 read unmarked folders as groups) has that child migrated under the
  child's own id, and its line says so; a folder without a descriptor and with
  none or several such children, a ZIP and any other file are skipped with the
  reason; a folder over
  512 MiB or 8192 files is skipped before it is read, and one whose carried part
  is over the bundle caps (4096 files, 256 MiB) is refused before it is hashed;
- a file byte-identical to installed game content becomes a stock pointer (see
  above): the store never receives it and an upload never sends it. Each
  folder's line says `N file(s), P of them stock pointer(s) to installed
  content, K KiB not stored`, the summary line totals them, and the record
  carries `stockPointers` and `stockBytes`. Measured on the owner's packs:
  453 of 472 files of the cyberdemon pack (9.1 of 9.7 MB), 306 of 388 of the
  doors pack (8.1 of 8.3 MB), and 6 of the Imp's 11 (renamed copies, 1.8 MB);
- nothing under `overrides\` is created, changed or deleted; an empty
  `override-backups\` folder is removed and a non-empty one is left alone.

The tool runs the pass on its own thread, folder by folder. In the game,
`sh_mods migrate [--again]` (and the Mods tab's **Migrate**) runs it on a worker
thread that writes the record and rebuilds the serving table when it is done
(editor rows it adds after the first palette build need
`sh_mods palette rebuild`). The record lists every folder with its kind,
outcome, reason, id, version, store folder, bundle hash and tree, and a `trees`
map from tree to store folder; it says `"complete": false` until the pass
ends, and an unfinished record makes the next run do the pass again (every
step is idempotent). Each folder logs one `MODS: migration:` line and the pass
one summary line; a line that leaves files out names how many and how many of
them are `.bswf` movies (bundles carry movies under `generated/swf/` only, served
by `modswf.c`; a pack migrated before that was left out can be migrated again
with `sh_mods migrate --again`). A migrated pack keeps its id, so where it
shares one with a delivered or converted package (the beta.19 `doors` packs),
the most recently installed bundle serves every path they share, whichever
map is open. Stock names are not checked during the migration because the
pass runs without the game's resource reader, so unresolved needs only warn, as
for a converted carrier.

Every writer of the store serialises on one recursive lock, `modstore_lock`
(`modstore.c`; `modsync_store_lock` is the same lock under its older name).
Each install and verification takes it; each migration folder, a download
install, `sh_mods install`, `sh_mods migrate` and an export hold it around their
whole step. A second writer waits for the first instead of failing, and logs
one `MODS: store: waiting` line; because the migration takes the lock per
folder, an install on the main thread (the Mods tab or `sh_mods install`) waits
for at most one folder, not for the whole pass.

### The Mods tab's map scope

The tab's map object and its two destructive flows (Remove from map,
Uninstall) work on the map open in the editor, read when the document is
built:

- Entities, wires and references come from the map serialized as a save would
  write it (`sh_rawmap_serialize_live_copy`, a snapshot with no save side
  effects), parsed by `modmap.c`. An entity uses a mod when one of its placed
  names (its inherit and every string under an `edit` object, the save-time
  closure's rule) is a name the mod provides (`modserve_bundles_for_names_visit`);
  a module instance uses one through its `moduleName`. A manifest row whose mod
  is not installed is matched through the names the row recorded. The
  editor's entity ids are the JSON `uniqueId`s.
- A reference to those entities is a wire (`targets.connections`, a
  run-length list) with exactly one end on them, or a Timeline
  (`idTarget_Timeline`) whose `entityEntRefs` row names one. The same edge model
  is what the module-prefab save walks when it cuts a selection.
- A map-level mod is a row of the current manifest marked `attached`
  (`sh_modmanifest_current_attach`, the tab's `mapAddMod` and `sh_mods map
  add`): the map uses it without placing anything, so it is listed with no
  entities and Remove from map deletes nothing and drops the row.
- The map's details: its saved-map id and cloud-saved id from the editor's
  copy of the saved-map model (edit+0x7F8, +0x9B0), the search tag from that
  copy's tag list (edit+0x998, the list the next local save writes), the name
  and last save from the slot's `game.details` under
  `Saved Games\id Software\DOOM\base\savegame.user\<profile>\SNAPMAPS<id>\`,
  and the published and uploaded state from the record's `links` and
  `claims`. What none of these say is left out of the document, never guessed.

Remove from map deletes the mod's entities through `sh_editor_delete_batch`,
DOOM's own Delete Selection, in one batch: one native undo step that Ctrl+Z
reverts, before or after a Play round, from the tab or from `sh_mods map remove`. The native delete drops the wires to the entities with them (a save
right after shows no connection to a deleted entity), so the product severs
nothing itself; an entity the editor would ask about (its referenced-entity
confirmation) is refused, with the whole request, before anything changes.
How the native delete treats a Timeline's entity reference was not exercised
live: the dry run lists it, and the next save is the engine's own. Then the mod's row leaves the
current manifest record (`sh_modmanifest_current_remove`); the next save
rebuilds the rows from the placed entities and writes none for it. With the
last mod, the search tag `mods` is removed from the editor's tag list
(`sh_modpublish_edit_tag`, the engine's own idStr assignment, run on the process
heap because the console command's own heap does not survive Play); the published
copy is unchanged until the map is published again. A mod placed as
Blueprint modules, or reached only through another mod's needs, is refused
with the cause.

Uninstall, in order: the dry run and confirmation (as Remove from map's,
headed "Undo history is cleared. The folder can be restored from the Recycle
Bin by hand." with a map open and "The folder can be restored from the Recycle
Bin by hand." without, plus the Toybox rows, "Moves the folder mods\<folder> to
the Windows Recycle Bin.", how many local saves in My Maps carry a row of the
mod, "N of your saved maps use it; they will not open until a mod folder
provides its assets again"); the native delete
of the mod's entities from the open map, as Remove from map (a refusal, Play
running, a load or a busy editor, deletes nothing and is reported); the open
map's undo and redo history cleared so no snapshot older than the delete
remains; the folder moved to the Recycle Bin (deleted outright when the bin
cannot take it) and its record entries dropped; the rescan. The
saved maps are counted from each `savegame.user\<profile>\SNAPMAPS<id>\map.decl`
(a 4-byte checksum, then zlib of the map text; `sh_inflate_raw_upto`). The
history is cleared by the engine's own undo push (`editor_undo.c`): the undo
list is the idList at `*(editor+0x10)` (data +0, count +8, capacity +0xc,
cursor +0x18, each entry a 0xDD0-byte snapshot), and the push EditorCommitEdit
calls (Vulkan RVA 0x12039F0, OpenGL RVA 0x1202CE0, decoded at run time from
EditorCommitEdit's rel32 CALL at +0x20 to a `mov rcx,[rcx]; jmp` wrapper)
advances the cursor, destroys every entry from it to the end with the snapshot
destructor and the engine's sized free, compacts the list and appends a copy of
the live edit map. With the cursor set one slot before the first entry it
destroys every snapshot and leaves exactly one, the current state, with the
cursor on it; the engine frees what it allocated and no other address is
needed. It runs on the game thread inside a verified process-heap scope. After
an entity delete the native "Delete Selection" commit lands on later editor
frames, so the Mods tab's tick (`sh_modui_poll_main`, every native frame while
a clear is owed) clears once `sh_editor_delete_native_pending` is 0 (at most
ten seconds); without a delete it clears at once. A build where the push does
not decode refuses Uninstall while a map is open. The rescan's refresh rebuilds
the palette without the mod's editor rows and, when the editor refuses the
rebuild because the delete is still committing, retries it once the editor is
free. Closed maps are not edited. Remove from map, Uninstall and Rebuild are
refused while Play runs or a map loads (the editor's deactivation reason, the
engine's load state).

Accuracy: the page renders only from the documents the backend returns. Each
action is answered with its result and then a fresh document whose map object
is read again from the editor, so the confirmation lists exactly what the dry
run computed and the modal redraws from what the backend found afterwards.
The console verbs `sh_mods map`, `sh_mods map add "<id>"`, `sh_mods map remove
"<id>" [--yes]` and `sh_mods uninstall "<id>" [--yes]` share the dry runs and
the actions. Adding a mod to the map changes the current manifest only; the
next save writes its row, so it publishes, downloads and carries the `mods` tag
like a placed mod's.

### Published maps and missing mods

A published map is native by default: the product touches its launch only when
its text carries an `smpmod1.` row the asset rule does not find served (no
installed folder of the row's id, or one that lacks a name the row lists).
Everything else reaches the engine unchanged: vanilla maps, and maps whose
rows an installed folder serves, whatever its hash, which play with the
installed copy.

Identification. The `DeserializeFromJson` detour classifies each parse by the
engine caller's return address: `PublishedMapRead`+0x12A is the offline read of
the cached `map.decl`, `SnapMapReadSizeLimit`+0x5D the profile reader under
`PublishedMapComplete`; anything else (editor, My Maps, a rawmap, the product's
own resume) is another parse. After the manifest step and the native parse,
`sh_modpublished_parse_seen` records beside the kept rows the kind of read, the
map object it filled and, when a named bundle is missing, a copy of the text.
`sh_modpublished_last_parse` exposes the record.

Hold points. The completion that follows a read receives the same map object,
so each gate uses exactly that parse's record:

| Gate | Vulkan / OpenGL RVA | Steal | Resume |
|---|---|---|---|
| `OnSnapPlay` read completion | 0xFA4A80 / 0xFA3FB0 | 18, the cookie load at +11 rewritten | `(manager, NULL, metadata, map)` |
| Browser published play | 0xFD1E30 / 0xFD1320 | 20 | `(browser, NULL, metadata, map)` |
| `PublishedMapCacheReady` (joiner) | 0xFA1D60 / 0xFA1290 | 15, stack-probe call at +7 | `(manager, NULL, metadata, map)` after the lobby host's item is re-checked |
| `CacheMapForLobby`, warm cache (joiner) | 0xF9A170 / 0xF99620 | 20 | the function's own cold branch: wait text, CacheReady and error callbacks, `PublishedMapRead` |
| `LaunchLocalSnapMapForOfflinePlay` (My Maps) | 0xF9FCF0 / 0xF9F220 | 22 | `(manager, model, map)` when the model is unchanged |

None of these completions reads its request argument. The SnapMap feed
completion (VK 0x112B440) is not gated: its resume needs the issuer's action int
at request+8. The warm-cache branch never parses, so it uses the manifest this
session saw for that item and revision at an earlier gated completion; an item
never seen is not gated.

The hold. When bundles are missing the gate retains a deep copy of the published
metadata (the copy constructor `PublishedMapSelectionConstruct` uses, inside
process heap 0) and the text, does what the completion itself would have done to
the screen first (`OnSnapPlay` clears its wait dialog, GDM 0x63, and hides the
progress widget; browser play clears its pending state; the joiner hides the
progress widget), and returns without the original. On a later main-thread tick
(`sh_modpublished_poll`, after every native frame, menus included) it raises the
engine's Yes/No (GDM 0x29, button set 6) once the dialog surface is idle:
"This map needs N mod(s): <names> (<size>). Download and install now?". Each
row it asks for prints the asset rule's sentence on the console first.

Yes starts one job on the modsync worker that fetches every missing bundle by
hash and logs a progress line every two seconds. When the job is done, on a main
tick, the index is rescanned, every fetched bundle must be admitted, the screen
the hold came from must still be current (and, for a joiner, the lobby must
still host that item and revision), the manifest's bundles are activated, a
fresh `idSnapMap` is constructed, the retained text is parsed through the normal
detour (which strips the rows and activates), and the original completion runs
with it. No, a failed fetch, a bundle that will not install or a lobby that
changed drops the hold: nothing is launched, the progress widget is hidden, and
a one-button notice names the mod. Another map load, or a newer held launch,
abandons the hold silently and cancels its fetch. The engine thread never waits:
the network runs on the worker and the dialog is polled. After No, a hold that
asked for one row shows that row's sentence and "The map was not loaded."
(for example "This map uses Hell Guards (local.hell-guard), which is not
installed. The map was not loaded."). The download lands by the store's rule:
it replaces a folder of that id the product made (origin download, install or
delivery) and sits beside an author's own folder under `<id>@<version>`, both
serving by the conflict rule; the resume checks every row against the asset
rule again.

### Rawmaps that name mods

A rawmap carries the same `smpmod1.` rows as any saved map, and its JSON alone
loads only where those mods are installed: a placed entity whose mod is absent
makes SnapMap's own validator throw the player out with "map corruption". So a
rawmap is never handed to the engine while a mod it names is missing.

Loading. `sh_modpub_rawmap_missing` reads the rows and counts those the asset
rule does not find served (missing or incomplete). The editor reload, which
both **File > Load Rawmap...** and `sh_rawmaps load` use, checks on its own
frame before `LoadMap` (`sh_rawmap_prepare_reload` in `ef_service_reload`); the
text it read, with a `.zip` already imported, is what the parse then uses. Any
other substitution (a staged file and the next map the player opens, or rawmaps
on for every load) is checked in the `DeserializeFromJson` detour: a map that
misses mods is not substituted, the map being opened loads as itself, and the
hold starts after that parse. Inside a reload window a failed check parses
nothing rather than the borrowed saved map.

The hold (`sh_modpub_rawmap_hold`, hook `SH_MODPUB_RAWMAP_LOAD`) begins one
state earlier than a published map's: a worker job asks the mod store for each
missing bundle (`GET /mods/bundle/<hash>`; 404 and 451 mean absent). When the
store has them all, the player gets the question a My Maps launch asks, and Yes
fetches and installs them; the resume does not activate anything (the rawmap's
own parse does) but opens the rawmap again, through a reload when the editor can
take one and otherwise staged for the next map open, with a notice saying so.
When the store lacks any, or cannot be reached, the load is refused with a
one-button notice and one console line naming each such mod (id and version) and
saying the map needs its author's `.zip` export; the asset rule's sentence for
each row comes first on the console, and with one row it is the notice ("...
which is not installed. The map was not loaded. Ask its author for the .zip
export."). No and a failed download behave
as for a published map. The gate is bound with the published-map gates; on a
build where they did not install, nothing is counted missing and rawmaps load as
before.

Saving. After every rawmap write of a map with at least one row (the File menu's
Save Rawmap and Save Rawmap As, `sh_rawmaps save`, and the mirror when rawmaps
are on), the export is written beside the JSON at the same path with `.zip`
(`rawmap.json` becomes `rawmap.zip`), through `modexport_write_text_report`. The
next main tick (`sh_rawmap_poll_main`) shows one toast and one console line:
"This map uses N mod(s); share <name>.zip, the .json alone will not load for
others." A save whose own path ends in `.zip` is the export already; a map with no
rows writes only the JSON. `sh_rawmaps status` and the File menu's status
(`lastJson`, `lastZip`, `lastZipBytes`, `lastMods`, `lastZipError`, and `mapMods`
for the open map) report what the last save wrote. A ZIP left beside a JSON by
an earlier save is not deleted when the map later stops using mods.

The ZIP (`modexport.h`) carries the map and each mod its rows name, as the
mod is installed:

```
<name>.json                the map text, its smpmod1 rows included, named after the ZIP
mods/<folder>/mod.json     the store folder's own name (<id>, or <id>@<version> when
mods/<folder>/assets/...   the store holds two folders for one id), byte for byte
```

There is no `export.json`, no `bundle.json` and no `bundle.canonical.json`, no
`legacy/` level and no `+<build>` suffix: the rows name the bundles, and each
carried `mod.json` records its bundle hash in its product block (with its
`legacy` record, if any). A stock pointer is an entry of `mod.json`, so the
folder carries it; installed game content is never written into the ZIP, so a
pointer whose bytes an author's folder holds under `assets\` travels without
them. A row pinning an earlier build carries the folder serving it now, and the
exported map's rows name that build.

Importing it is the ordinary install of each carried folder whose bundle a row
names (`modexport_read`, on whichever thread loads the map; a carried folder no
row names is left out):

- a carried folder the store already holds (by its hash, or an alias of it),
  or one whose id an installed folder declares and whose listed names (the
  rows that name its bundle) that folder serves (the asset rule, whatever its
  bytes or version), is skipped and counted as already installed; nothing in
  that folder is written;
- each remaining folder is copied out of the ZIP into a temporary folder (with
  the bytes of a pointer the folder held filled from this player's own game)
  and installed from there as any folder from elsewhere is
  (`sh_ms_install_as`: built, copied into the store, verified): under its own
  name when no folder of its id is installed and that name is free, else
  beside the installed folders of its id under `<id>@<version>` (a free
  `<id>@<version>-2`, ... when that is taken), never over one. A mod that needs
  another carried mod is built after it;
- the built bundle hash must equal the one the carried `mod.json` names;
  otherwise, and on any other refusal, exactly the folders this import
  installed are removed and the load is refused ("<id> <version> in the export
  does not build to the bundle the map names");
- after an install the serving index is rebuilt, as before.

A ZIP in the bundle form of earlier releases (any `bundle.canonical.json`, or
an `export.json` index) is refused: "this export was made by an earlier
release; save the rawmap again". The map importer tells a map from a prefab by
the carried `.json`: an `idSnapMap` (or an object without `~type`) without a
`snapmapPlus` block is a map; a prefab's export is refused with "this is a
prefab's export, not a map's". The published-map download path is not used.

### Prefabs that name mods

A prefab is never handed to the engine while a mod it uses is missing, for the
same reason as a rawmap: an entity on a mod's decl, or an instance of a mod's
module, has nothing to resolve to.

One file. A prefab is one file in the library: the engine's JSON (an
`idSnapEntityPrefab`, or a module prefab's `idSnapMap`) with one product-owned
top-level member, `snapmapPlus`, after the engine's own members
(`prefab_block.h`):

```
"snapmapPlus":{"format":1,"description":"","tags":[],
               "mods":[{"id","version","bundle","size","names"}...],
               "summary":{...}}
```

`summary` is a module prefab's summary (module, entity, door-slot and variable
counts, dropped wires and references, AI Navigation boxes, environments, the
footprint and each module's box) and is absent for an object prefab. The block
is inserted as `,\n"snapmapPlus":{...}` right after the last engine member, so
removing that span gives back the engine's bytes exactly. The engine's
reflection parser skips an unknown top-level key (on 13954591 a map carrying
the block loaded clean and the engine's re-save dropped it), but nothing depends
on that: every reader strips the block first. Load / Place strips it before the
paste or the module merge, the module check and merge strip it again in the
backend, the closure ignores it, and the Prefabs tab sends the page the scene
without it (the page's scene parser also drops the key). The product writes a
prefab file at Create and then only rewrites its block; the engine never
re-saves one, so the block persists.

Plain JSON. The engine's serializer writes a block comment before almost
every member (`{/*camera yaw*/`; one stairs prefab held 209). Create
removes every block and line comment outside a string
(`sh_prefab_block_strip_comments` in `prefab_block.c`, the one function every
writer of an object prefab file uses: the host before it writes the file, and
`MODS_RECORD` again before it writes the block) and then appends the block, so
a prefab file is plain JSON. Strings keep every byte, comment openers and
escapes included. Readers still skip comments, because files created by
earlier builds have them; the engine's deserializer reads comment-free text
(module prefab files and prefabs from the 2018 build have none).

Recording. Saving a prefab finds its mods exactly as a map's save does: the
placed names of the prefab text (`sh_modmanifest_placed_names`: every
`inherit`, `moduleName` and string inside an `edit` object) go through
`modserve_bundles_for_names_visit`, which names every admitted bundle that
provides one and the bundles its needs reach. Each bundle is one row in the
manifest's form; a pinned hash an alias names resolves to the current build
wherever a row is used, as for a map. Create writes the file the engine
serialized and then has the backend write a fresh block into it (`MODS_RECORD`:
empty description and tags, the rows, `[]` when there are none); a module
prefab's save writes the file with its block in one step. The Prefabs tab's
description and tag edits rewrite the block in place (`META_WRITE`), keeping its
summary and rows. No sidecar is written.

The read-only sidecar. A prefab saved before the block has none. When a
`<name>.meta.json` sits beside it, the list and Prefab Details show that
sidecar's description, tags, summary and `"mods"` rows, and Load / Place,
export and `sh_prefab mods` use its rows. It is not migrated: the first
description or tag edit writes the block into the file from the sidecar's
details and leaves the sidecar on disk as it was; from then on the block wins.
A prefab with neither names no mods and loads as before. A sidecar is never
listed as a prefab, and it follows its prefab when that is moved, renamed or
deleted.

The page and the slot see a prefab's details as one flat object, the view:
the sidecar's shape, `{"desc","tags",...summary members...,"mods"}`. A block
reads as a view and a view composes into a block (`prefab_mods.c`).
`META_READ` returns the view of a file (its block, else its sidecar); the
Prefabs list reads at most the last 1 MB of each file, where the block is.

Resolving. Load / Place first reads the rows (`MODS_RESOLVE`, on the game
thread):

- a module prefab naming a module no row provides and the palette lacks (DLC
  the player does not own) is refused with the module guard's toast and
  console line, below; nothing is fetched, because nothing can install it;
- with every row served (the asset rule), the rows are activated for the open map
  through the builds that serve them
  (`modserve_activate_bundles`) and each one the open map does not list (its
  scope listing, else its manifest rows) is attached as a map-level row through
  the Mods tab's Use in this map (`sh_modui_run(SH_MODS_ACT_MAP_ADD)`), with one
  console line: "Prefab <name> uses <id> <version>; this map did not list it,
  so it was added to the map (the Mods tab's Remove from map takes it out).".
  Then the placement goes on unchanged;
- otherwise the placement is held by `sh_modpub_prefab_hold`, the rawmap hold
  with its own hook (`SH_MODPUB_PREFAB_PLACE`): the store is asked on the worker;
  when it has every missing bundle the engine's Yes/No asks "This prefab needs
  N mod(s): <names> (<size>). Download and install now?", Yes fetches and
  installs them, and the ready callback runs the step above; only rows the
  asset rule reports missing or incomplete are asked for; the Prefabs tab,
  polling `MODS_POLL`, then places the prefab through its ordinary Load /
  Place. When the store lacks any or cannot be reached, the notice says "This
  prefab was not placed. It needs mods you do not have ..." and one console line
  names each mod (id and version) and asks for the prefab's `.zip` export. No,
  a failed download and a bundle that will not install end as for a map, with
  "The prefab was not placed.";
- with the gates unbound (a build where the published-map gates did not
  install) the same refusal is printed and toasted without asking the store.

One hold runs at a time: a second placement while one waits is refused.

Export. `sh_prefab export "<name>" "<path.zip>"` and the tab's Export always
write one ZIP, also for a prefab that uses no mods, in a map's layout
(`modexport.h`):

```
<name>.json                the prefab file, its block naming the builds carried
mods/<folder>/mod.json     each mod the block's rows name, as it is installed:
mods/<folder>/assets/...   the store folder's name and its files, byte for byte
```

There is no `export.json`, no sidecar and no bundle manifest: the prefab's own
block is the index, and each `mod.json` carries its bundle hash.
`<name>.json` is the file's bytes as they are on disk. A prefab with no block
(saved before it, with or without a sidecar), one saved before rows were
recorded (its rows are then the closure of its text) and one whose rows pin an
earlier build are carried with their block written for the rows the ZIP
carries; the file in the library is not changed. The Export dialog proposes
`<name>.zip`.

Import. `sh_prefab import "<path.zip or path.json>"` and the tab's Import (the
Open dialog's "Prefab exports (*.zip, *.json)") take the ZIP, or a `.json`
inside an extracted export folder: that folder is the export, the picked file is
the prefab and a `mods/` folder beside it holds the mod folders. The importer
takes the one top-level `.json` (an `idSnapEntityPrefab`, or an `idSnapMap` that
carries a block) and installs each carried `mods/<folder>/` whose `mod.json`
names a bundle the block's rows name, exactly as a map's import does (see
[Rawmaps that name mods](#rawmaps-that-name-mods)): skipped when an installed
folder of its id serves the names the block's rows list (the asset rule),
installed under its own name or beside the installed folders of its id
otherwise, and removed again when it does not build to the hash its own
`mod.json` names; a carried folder no row names is left out. It then rescans and adds the prefab under the `.json`'s file
name. A prefab of that name with the same bytes is left alone (an
import that finds everything already there writes nothing); one with other bytes
is kept and the import takes "<name> (2)" and so on. A row the export does not
carry is named in the result ("It also needs <id> <version>, which the export
does not carry."), and Load / Place asks for it as above. Each importer refuses
the other's ZIP, told apart by the carried `.json` ("this is a map's export, not
a prefab's"), and the prefab importer also refuses a map's JSON and two prefabs
in one ZIP. An export in the bundle form of earlier releases (any
`bundle.canonical.json`, in a ZIP or an extracted folder, or an `export.json`
index) is refused with "this export was made by an earlier release; export it
again".
`sh_prefab mods "<name>"` lists the block's rows (else the sidecar's) and
whether each is installed, with the asset rule's sentence for one that is not.

### Publishing a map with mods

What is sent: the bundles the published map names in its `smpmod1.` rows, and
only those, at publish time. The `SerializeToJson` detour reports each save's
final text (`sh_rawmap_set_save_observer`); `PublishAfterSave`, the Save
state's callback that runs before the first request to the map service, is
detoured (`modpublish.c`). A save without rows publishes natively. A text over
the service's 10,485,760-byte bound is refused through the engine's own
`PublishFail`, with a toast giving the size and the overage. Otherwise the
publish is held: the Save state returns with the publish pacifier up, as the
engine's own asynchronous chain leaves it. The first publish with mods on an
installation asks once through `sh_engine_dialog_ask` (Yes sets
`mods.upload_confirmed`); the ask site is one function, and an alternative
route shows the explanation as `PublishFail`'s editor toast instead, cancels
that publish, and treats the next one as the agreement. One worker job then
uploads every bundle (`modsync_upload_bundles`: missing blobs, the canonical
manifests, one claim) under a provisional ref `publish-pending-<16 hex>`, and a
main tick resumes the original `PublishAfterSave`. A publish whose save
carries rows also carries the search tag `mods`, so players can see that the
map needs Snapmap+. Every local save keeps the tag in step with its rows: the
`WriteLocalSavedMapText` detour adds it to the model the save writes when the
save observer saw rows and removes it when it saw none (the model is copied
back to the editor), so an undo of the last mod's removal, or a save made
without publishing, never leaves a map with mods untagged. The tag has to be in the local save's metadata: the cloud
save `TryCloudSave` starts reads the slot's metadata back from the profile
(`ReadLocalSaveMeta`) rather than the editor's model, and the published item
copies the cloud save's tags, so a tag added to the editor after the save is
lost. `PublishStart` is therefore detoured to arm the tag for the edit's local
id, and `WriteLocalSavedMapText` (the local save's metadata and `map.decl`
write) to append `mods` to the model it writes, when it is that slot's save
and the save observer saw rows, the way `PublishStart` appends `map:dev` (the
list grow and idStr assignment are bound from its own calls). The save then
copies that model back to the editor. A tag that cannot be written is logged
and the map publishes without it; a held publish whose save was not tagged
logs that too. A No, a failed upload, a
question that cannot be shown or a busy worker cancels through `PublishFail`
and a toast naming the cause; nothing reached the map service. The editor
leaving abandons the hold; another save cancels it.

The claim and its secret: `PublishFeedPost`, the one function both success
paths reach, carries the item id and revision. The worker claims the bundle
set under the item id. On a republish the stored secret re-claims that ref,
which replaces the set (Contract 4); a republish without rows releases the
item's claim. The claim id and secret live in the record's `claims`
section (modsync); the secret is never logged. The provisional claim is released once the item
claim succeeds, or when its publish never reaches the feed post. Claims,
links and the retry queue are the `claims`, `links` and `pending` sections of
`mods\snapmap-plus.json`.

Release: the MapInfo screen's unpublish completion (`(screen, ctx, const
idStr &id)`, bound by the unpublish confirm) releases the item's claim on the
worker. Deleting a map from My Maps removes the local slot and the cloud-saved
item; the published item stays playable for everyone, so its claim normally
stays. The delete completion (`(screen, ctx, const idStr &localId, const idStr
&cloudItemId)`) releases a claim only when its item is no longer published,
for example after an unpublish this installation never saw. Neither saved-map
model links to a published item (their reflected fields hold only the local
id, the cloud-saved id and branch parents), so the feed post records the link
itself in the record's `links` section (item id to cloud-saved id and local id).
At delete, the linked items that still hold a claim are checked with the
engine's own `maps_pub_prev_pub` search (`PublishedPrevQuery`, by parent
cloud-saved item and owner), issued on the main tick with two callback objects
the engine's request context invokes and destroys; only an item missing from a
successful answer is released. No link, no stored claim, no cloud-saved id, a
link younger than an hour (a fresh item may not be searchable yet), an answer
as long as a search page (30 items, it may be partial), a failed answer or
none within 60 seconds keeps every claim. A release or an
unpublish forgets the link. A copy to My Maps of a published map whose
cached text (`%LOCALAPPDATA%\id Software\DOOM\base\doomsnapmaps\published-maps\<id>_<rev>_map.decl`)
is over the bound is refused at the MapInfo and browser entries, before the
engine creates the saved item, with a one-button notice.

The retry queue: a failure modsync can retry stays in the record's `pending`
section. At startup the backend settles provisional refs
left by a closed game (their claims are released and their queued uploads
dropped, since their publish never completed), then retries the rest of the
queue once on the worker and logs the counts.

## Module prefabs

`module_prefab.c` saves the selected Blueprint modules as an `idSnapMap` file,
with the summary and mod rows in its `snapmapPlus` block (one file, see
[Prefabs that name mods](#prefabs-that-name-mods)), and places one by stripping
that block and merging the rest into a fresh serialization of the open map
(`module_prefab_map.c`) and installing the result through the engine's Redo step
(see `module_prefab.h`). Before the merge, Place checks every `moduleName` the
file names, on the game thread: `sh_mp_check_modules` asks
`sh_grid_native_module_known`, which accepts a record of the editor's module
palette (found by exact name over every record, because the Blueprint Add tool
re-sorts that list), a Grid Room size the adapter builds, a module an installed
mod serves (`sh_modserve_module_served`) or one already loaded into a private
palette. Any other name refuses the placement before the engine sees it: slot
result -6, one console line `Module prefab: needs module <name>; this room is
not in your palette (DLC or a mod you do not have)` and a toast naming the rooms.
A module a served mod provides is a mod row like any other (see
[Prefabs that name mods](#prefabs-that-name-mods)): the save records it, Load /
Place downloads and installs it before this check, and the check then finds it
served; a module no row provides and the palette lacks is DLC, refused here.
The engine has no safe outcome for such a name. The map validator (VK
0x1412488e0 / GL 0x141247f10) drops the instance through HasModule ("Couldn't
load module") and keeps its entities, and the record builder SnapMapEditCtor runs
(VK FUN_141204380 / GL FUN_141203780) raises FatalError "Couldn't find module in
modulePalette" when FindModule returns the palette's end record. The Prefabs tab
runs the same check (`SH_MODULE_PREFAB_CHECK`) when a module prefab is selected
and shows the missing rooms above Limits; the check names nothing when no editor
palette is open, and with the prefab's block (or older sidecar) it leaves out the modules its
mod rows provide, which the Prefabs tab lists as mods instead.

From the install until Space or Esc ends its grab, a placement is held
(`sh_module_prefab_held`, read through `prefab_watch`); the editor budget
already counts the installed modules then, so the Prefabs tab's Limits show the
live usage as "Map after" for the held prefab instead of adding it again.
`sh_module_prefab_selected_count` reads the length of Blueprint's module
selection, the count Save uses, for the tab's Create gate.

## Map behaviour carried by String Set nodes

`modaction.c` lets a map carry a behaviour in stock data, so a new engine event
needs no product release: a Set (String) node whose literal value starts with
`smp:` is a command. The detour sits on the action's `DoAction(this, activator)`
(signature `StringSetDoAction`, Vulkan 0x9E26C0 / OpenGL 0x9E19B0, reached
only through its vtable slot), the one hop where the activator is known; the
variable-changed notification that follows carries only the variable index.
The 20-byte prologue is checked before the detour is placed; the engine calls
the hook uses (`ProcessEventArgs`, the argument clear and entity conversion,
the two player casts) are raw RVA pairs that apply only on the recognised build
13954591 executables.

The hook reads the node's value parameter at `this+0xCA8`: a literal when its
index equals the engine's "no variable" constant (-1), held in an `idStr` at
`+0xCB0` (length `+0x08`, data `+0x10`). It acts only when all of these hold:
the master switch (`sh_smp on|off`) is on, the game manager reports the host
(`IsServer`, gameMgr `+0x2BB3A4`), the SnapMap logic manager exists (the same
check every stock action makes), and the optional map gate
(`sh_modaction_set_map_gate`, unset by default because the `smp:` prefix is the
map's own opt-in) admits the open map. Logic only runs on the host, so a client
never reaches the hook. The activator becomes a player by an `idPlayer` cast,
else through the owner slot (vtable `+0x558`) the stock Give Item action uses.
The command runs on the game thread inside `DoAction`, then the stock write
proceeds unchanged; a fault inside the command is caught and logged and never
stops the write.

`smp:event` looks the event up through the decl manager's event interface
(`+0x90`, FindByName `+0x10`), types the arguments from the record's compact
format (`+0x08`), and dispatches at once with `ProcessEventArgs(obj, ret, def, n,
arg...)`, up to eight 0x18-byte `idEventArg`s. A bool travels as `'i'` because
the engine's argument copy drops a `'b'` value; an entity argument is resolved
by name and converted to its handle with the engine's own constructor; a string
points into the parsed command for the duration of the call. `smp:doomhands`
writes the player's Doom-hands byte (`+0x45EB5`, otherwise written only by
`idPlayer::Spawn` from the local-player check, the SnapMap game type, the map's
`useSPModels` and a player count below two) and marks the hands and body stale
(`+0xB512`, `+0xB513`) so the next think reselects them. The parser and the
argument typing are pure C (`SH_MODACTION_PURE`, `tests/modaction_test.c`).

`sh_event list` reads the same registry (Count `+0x28`, GetEventCommand
`+0x08`); on this build it holds the 1611 events of the Timeline tab's
generated catalogue with the same argument types, so the tab's picker already
offers every registry event. Timelines resolve their target entities when the
map spawns (`idComponentTimeLine` reports "references bad entity" for a name
that does not exist yet, such as `player1`), while entity arguments resolve
when the event runs. The engine's own writer annotates the call's members as
`"\neventHandle_t eventDef"` and `"\nnum"` (a leading newline), and the Play
build warns about them
("Expected 'eventHandle_t eventDef' ...") but still reads the call.

## Failure reporting

The fault shield handles selected, understood engine faults. It does not make
arbitrary native calls safe. Expected recoveries, nonterminal notices and fatal
records have different meanings; a captured record alone does not prove survival.

The Class-B redirect turns a fault into the engine's `Error(6)`. Only
`idCommonLocal::Frame`'s idException catch receives it, and that catch is a full
teardown run inside the catch funclet, outside Frame's try: it syncs the renderer
and calls CancelMapChange, which unloads the game and loads `game/shell_map`.
`redirect_policy.h` therefore declines the redirect, and leaves the fault to the
fatal handlers and their crash report, when the fault is off the engine main
thread; when the loading-screen thread is running (the `load_screen` global,
thread byte +0x4b2), because only `idLoadScreen::LevelLoadCompleted` stops that
thread and the throw skips it, so the thread keeps rendering the menu while the
main thread recovers; when an earlier redirect is still being recovered in the
same Frame call, where a second throw reaches the game-loop catch that ends the
process; and when the faulting stack does not unwind to Frame. An input the
shield cannot read keeps the earlier behaviour. The decision is logged as a
`declined` shield line with its stack.

The serialized-entity cleanup guard permits an empty moved handle, as the native
snapshot writer already does. It skips that handle's index write and preserves
the engine's list reduction and removed-entity reset. Startup requires a unique
instruction signature and exact-byte verification on both supported renderers.

Crash formatting produces complete JSON or an empty failure. Capture uses bounded
storage and nonblocking guards so nested faults cannot wait on a faulting thread.
A pending record is published only after its temporary file is written completely.
The frontend reads those records for notices or report prompts; network reporting
is handled separately by the [feedback pipeline](feedback.md).

## Changing a boundary

Keep thread, ownership and failure contracts in the relevant header and verify
the production path with focused tests. Use fault injection for rollback and
publication failures, controlled concurrent readers for snapshot changes, and
both executable images for resolver changes. Build both DLLs and test the affected
workflow in DOOM before shipping. See [contributing](contributing.md).
