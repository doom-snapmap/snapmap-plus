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
retained, with extensions through `+0x378`; the current table occupies `0x380`
bytes. The File menu's rawmap surface is `rawmap_status` (`+0x328`),
`rawmap_configure` (`+0x330`) and `rawmap_load_now` (`+0x338`). The last three
are `navmesh_view` (`+0x340`), `run_on_main` (`+0x348`), `delete_entities`
(`+0x350`), `prefab_grab_status` (`+0x358`), `module_prefab` (`+0x360`, the
Blueprint module-prefab state, save, place and status operations),
`timeline_tile` (`+0x368`, the Timeline palette tile's status) and `budget`
(`+0x370`, the editor budget table and usage for the Prefabs tab's Limits
section, built on the game thread after each palette build by
`budget_table.c`) and `mods` (`+0x378`, the Mods tab's document and actions,
`modui.c`, run on the game thread through `run_on_main`). The retired
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
before the action restores the previous selection. Portal doors and frames are
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
see [Published maps and missing mods](#published-maps-and-missing-mods)); an
exported rawmap is a ZIP that carries the bundles itself (`modexport.c`), so
sharing a rawmap stays offline. Plain saves never upload. The retired
`%LOCALAPPDATA%\snapmap-plus\overrides\` tree is read once, by the migration
below, and never served or written; no `package-cache` or `package-staging`
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
  rebuild rule and the one-time upgrade of the retired layout, below. Files are
  hashed at install and on `sh_mods doctor verify`, never at boot.
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
- `modmanifest.c` owns the `smpmod1.<index>.<count>` manifest rows, and
  `modlegacy.c` the retired `smpkg.` carriers, as below.
- `modui.c` is the WebView Mods tab's backend. It lists one row per mod
  folder (two folders declaring one id both flagged, a package a map delivered
  listed like any other), marks each installed or needing attention with its
  cause, reports whether `overrides\` still holds entries to migrate, and runs
  Install, Remove, Migrate and rescan on the main thread. What the tab does not show, every bundle's state with legacy ones,
  conflicts with their winners, shadows and verification, it prints as
  `sh_mods doctor [verify]` (see [WebView UI](webview-ui.md#mods-tab)).
- `modpublished.c` holds a published or My Maps launch whose manifest names a
  bundle that is not installed, asks the player, and fetches and resumes it or
  drops it, as in [Published maps and missing mods](#published-maps-and-missing-mods).
- `modmigrate.c` moves the retired `overrides\` tree into the store once, as in
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
else `<id>@<version>`. Identity is the id: two folders declaring one id are
both kept and both flagged. A delivered copy whose content an installed mod of
the same id already serves is not stored; its hash is recorded as an alias of
the installed bundle. `mods\snapmap-plus.json` is the product's one record:
`claims`, `links`, `pending`, `migration`, `cache` (tree hash to bundle hash)
and `aliases`, each section written whole under one lock.

The rebuild rule: a folder is built again when its tree hash (the author's bytes of `mod.json`
and the path, size and time of every file under `assets\`) is not a key of the
cache for its bundle; that runs on a rescan (the Mods tab, `sh_mods rescan`,
an install), never at boot, because a build needs the game's resource reader.
At the first start of this build `sh_ms_upgrade` converts the retired layout
once (before the overrides migration and the serving index): every
`<id>@<version>+<build>\` folder gets `mod.json` from its `bundle.json` and
`.state.json` (read back with the same hash before the old files go), hard
links become copies, a missing file is restored from `blobs\`, product folders
are renamed to `<id>` (or `<id>@<version>` when one id has several), `legacy\`
folders move into `mods\` or are dropped as aliases, `blobs\` is removed, and
`claims.json`, `publish-links.json`, `pending-uploads.json` and
`.migrated.json` are folded into the record. It logs one `MODS: store:` line
per folder and a summary, and every step can be repeated.

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
placement grid, map navigation, the generated Grid Room variants, the built-in
defaults, installed mods, then the engine. Opens with mode 2 or higher are
never served. A shadow applies to every map while its bundle is installed; a new
name applies once it is registered: editor rows at the first palette build (or
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
whose models block names a model that no bundle provides and the engine has
no md6Def for, since the engine's parse throws on it: a stock animWeb it
shadowed is served by the game again, and a new name is not registered. Before
the Play level load the SnapBuild detour re-checks the bundles' blacklist
requirements (`MODS: play: requirements live: ...`); the engine itself does not
reset those two gates between the editor and the level load. The editor pass
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
their needs reach), after any navigation shards; with an empty table and no rows
in the map it returns at once and the save is the engine's own bytes.

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

The same race decides what a bundle's requirements mean for a resource: the
cut-content blacklist gates are applied only at the first registration pass,
and anything the engine loaded before that resolved its names with the
blacklists on. A model loaded then keeps no materials and draws nothing: this
is why the Spider Mastermind body (stock-named mesh, loaded at boot, its
materials and images on the blacklists) was invisible on 13954591 while its AI
ran, and why the same bytes under a new mesh name drew. The provider therefore
records whether a file was first served while one of its bundle's gates was not
live; a decl first served then is re-parsed by the pass above, and each
`baseModel` and `image` first served then is loaded again in place, found by
name in the engine's `idMD6Model` and `idImage` resource lists, once the gates
are live: `MODS: editor: reloaded <n> model(s) and <m> image(s) loaded before
their bundles' requirements were live`.

### Migration of the retired overrides folder

Releases up to v0.2.1-beta.19 kept packages in `overrides\`. At backend start,
after the store upgrade and before the serving table is built, `modmigrate.c`
runs once when `overrides\` exists and the record's `migration` section does
not (or records an unfinished pass):

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

Folders of at most 32 files and 16 MiB are migrated before the serving table,
within about a second of boot time; the rest go to a worker thread that runs
while the game boots, writes the record and rebuilds the serving table when it
is done (editor rows it adds after the first palette build need
`sh_mods palette rebuild`). The record lists every folder with its kind,
outcome, reason, id, version, store folder, bundle hash and tree, and a `trees`
map from tree to store folder; it says `"complete": false` until the worker
ends, and an unfinished record makes the next start run the pass again (every
step is idempotent). Each folder logs one `MODS: migration:` line and the pass
one summary line; a line that leaves files out names how many and how many of
them are `.bswf` movies (bundles carry movies under `generated/swf/` only, served
by `modswf.c`; a pack migrated before that was left out can be migrated again
with `sh_mods migrate --again`). A migrated pack keeps its id, so where it
shares one with a delivered or converted package (the beta.19 `doors` packs),
the most recently installed bundle serves every path they share, whichever
map is open. `sh_mods migrate [--again]` runs the same pass on the worker.
Stock names are not checked during the migration because the game's resource
reader is not up at boot, so unresolved needs only warn, as for a converted
carrier.

Every writer of the store serialises on one recursive lock, `modstore_lock`
(`modstore.c`; `modsync_store_lock` is the same lock under its older name).
Each install and verification takes it; each migration folder, a download
install, `sh_mods install`, `sh_mods migrate` and an export hold it around their
whole step. A second writer waits for the first instead of failing, and logs
one `MODS: store: waiting` line; because the migration takes the lock per
folder, an install on the main thread (the Mods tab or `sh_mods install`) waits
for at most one folder, not for the whole pass.

### Published maps and missing mods

A published map is native by default: the product touches its launch only when
its text carries `smpmod1.` rows naming a bundle that is not installed and
admitted. Everything else, vanilla maps and maps whose mods are all installed,
reaches the engine unchanged.

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
"This map needs N mod(s): <names> (<size>). Download and install now?".

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
the network runs on the worker and the dialog is polled.

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
map needs Snapmap+. The tag has to be in the local save's metadata: the cloud
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

## Failure reporting

The fault shield handles selected, understood engine faults. It does not make
arbitrary native calls safe. Expected recoveries, nonterminal notices and fatal
records have different meanings; a captured record alone does not prove survival.

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
