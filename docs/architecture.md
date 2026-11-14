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
set (named and unnamed) and exports nothing else. The proxy loads the system `XInput1_4.dll`
from the system directory (`LOAD_LIBRARY_SEARCH_SYSTEM32`), wherever Windows is installed.
Only the diagnostic build (`build.ps1 -Diag`) links `xinput1_4_diag.def`, the same set plus
`sh_diag_status` at ordinal 300: one JSON object with the product version, the interface
table's size, the features off this session and the last rawmap open (`diag_status.h`), for
the development daemon. A release has neither the export nor its code.

`host_image.c` identifies the host by the SHA-256 of its executable file
(`SH_KNOWN_BUILDS`: the Vulkan and OpenGL executables of build 13954591). Any other
executable, the 2018 build included, is unsupported: the backend logs one line
naming the supported build and the final 2018 release, shows the same once in a
message box from its bootstrap thread (a DOOM update is otherwise silent), then
only forwards XInput.
Every engine constant is a build 13954591 value: struct and slot offsets in
`engine_build.h`, function RVAs as Vulkan/OpenGL pairs in the signature table, and
global RVAs in the generated globals table. Raw RVAs are applied only on a
recognized executable, for its own renderer, so no address can reach another image.

The installer routes by build before downloading anything: it hashes
`DOOMx64vk.exe` and `DOOMx64.exe`, installs the current release on build 13954591,
installs the final 2018 release `v0.2.1-beta.19` (by tag, exactly as that release
installs itself) on the 2018 build, and refuses unrecognized or mixed executables.
See [packaging](packaging.md#supported-build-and-the-final-2018-release).

## Engine resolution and patching

The game build is pinned by the executable's hash, so every engine function is
where the signature table records it. At run time `sig_resolve_one` checks each
signature at its recorded RVA for the host's renderer and accepts it only when the
whole masked pattern holds there; nothing is scanned and nothing is guessed. A
function whose bytes differ there (`SIG_CHANGED`: another program has changed the
game's code, typically an overlay or another mod's detour) is not resolved, and the
features that need it are recorded off (see "Features off this session" below).
Checking the whole table takes well under a millisecond; the whole-image scan it
replaced took about five seconds (nine seconds over a mapped file, `sig_test`).

The scan, `sig_scan_one`, is the developer tool that derives and audits the table:
`sig_test` (and `tools/signatures/sigscan.py`) prove that every pattern matches
exactly once in each executable, at the recorded RVA. Contributor changes to
signatures or anchors require that two-image test gate in
[contributing](contributing.md#7-run-the-tests). The globals table's anchors are
checked the same way: each row records the RVA of its anchor's code site on both
renderers (`site_vk`, `site_gl`), next to the audit RVAs of the global itself, and
`glb_resolve` decodes the displacement only where the whole anchor holds at that
site.

A pattern that matches more than once is not an identification. Where two
functions share a body, the caller must confirm its target by something the
pattern cannot carry: the rawmap branch answer follows the call its candidate
makes and reads the literal that callee names, because the sibling it must not
patch differs only there.

The boot pacing sites (`boot_pacing.c`) are resolved and patched right after the
build check, about 0.1 s after process creation, ahead of the engine's resource
preload, which starts about two seconds after launch.

Every code write -- a detour's prologue, a call's operand, a single byte -- goes
through `code_patch` (`patch.c`), which suspends the process's other threads,
checks that none is executing inside the bytes being changed or about to return
into them (a thread in the callee of a call the window covers), writes, and
resumes them. A thread found inside makes the attempt repeat a moment later; one
that stays inside for every attempt fails the patch with nothing written. No
thread can run a partly written instruction, at boot or for a patch made later
from the main thread. `tests/patch_test.c` proves the suspension, both refusals
and a stress run that commits and removes a detour under a thread calling it.

Hook callers prepare a trampoline, publish it as their original callback, then
commit the detour. Preparation does not write the target. Callers own the
lifetime of their original callbacks and any relay memory.
They must preserve those resources until restoration succeeds and no execution
can still use them. Failed rollback remains a recoverable state, not a successful
uninstall. The patch and hook headers define the supported installation and
restoration sequence. Stolen instructions must be complete and independently
executable. The ordinary hook helper performs no relocation; its explicit
variants relocate one verified instruction: `hook_prepare_rip_lea` a 64-bit
RIP-relative LEA, `hook_prepare_rip_load` a 64-bit RIP-relative load and
`hook_prepare_relative_call` an E8 call. Callers still verify every other stolen
instruction. Original target bytes
remain separate from relocated code for commit validation and restoration.

### Features off this session

`feature_health.c` keeps the one list of features that could not install this
session, each by the name the player knows it by, and logs each once with why.
The patch self-test and a scratch detour run before any engine code is touched; if
either fails, nothing is patched and the list holds a single entry for every editor
feature. The Snapmap+ window reads the list through the interface slot
`features_off` and shows one line ("These features are unavailable in this
session: ..."); a feature the player invokes from the window refuses with
`sh_feature_refusal` instead of doing nothing. Background guards stay in the log
only.

## The map carrier

Snapmap+'s per-map data (the mods a map declares, its two built-in settings and
any navigation an earlier build baked into it) lives in the map's own file, in
one root member the engine does not declare, `"snapmap-plus"`, never in the
author's variable table. The grammar is described once, in
`src/backend/map_carrier.h`; `map_carrier.c` finds, reads, strips and writes it
as text. The engine's typeinfo deserializer ignores an undeclared member with
no log and its serializer cannot write one; every transport (cloud save,
publish upload, download, Save to My Downloaded Maps, a lobby joiner's fetch)
moves the saved bytes unchanged, and a vanilla editor save drops the member.

Load: every parse the `DeserializeFromJson` detour sees (`rawmap.c`
`prepare_map_buffer`: My Maps, a published read, a joiner's download, a rawmap)
first asks `sh_map_old_shape` (`map_carrier.c`) whether the text is in an
earlier shape: a `variables.string` name in one of the families earlier builds
wrote (`smpkg.`, `smpbind.`, `smpref.`, `smpmod1.`, `smpprod1.`,
`smp.render.v1`, `smp.bounds.v1`, `smnav1.`), or a member row naming an `id`
or a `bundle`. Such a map is refused before anything reads it: the engine never
parses it (the detour answers a failed parse), the open map keeps its settings
and manifest, the text is not changed, one `CARRIER: map refused:` line is
logged and the load's refusal note is "This map was saved in a format this
version does not read." (`sh_modmanifest_load_refuse`), which the player hears
as the other refusals are. Otherwise each family reads its part from the
member. The member is stripped last, so the engine never sees it; a member that
does not read (not an object, a format other than 1, a part of the wrong type,
two members) is logged as refused, read as absent and still removed, and the
map loads. One `CARRIER: map read:` line names the parts a map carries.

Save: after the engine serializes (`sh_ser_detour`, and the rawmap export), the
settings, navigation and manifest steps each put their part with
`sh_carrier_put`; the member is written compact and last at the root, with its
parts in the order mods, settings, nav. No step writes a variable or touches
`variables.allocCount`. A map with nothing to carry has no member and saves as
exactly the engine's bytes.

Earlier shapes are the installer's store tool's: the readers of every family
above and the writer of the current member live in `map_old.c` (with
`map_legacy_payload.c`), built only by `src/store/build.ps1` and never linked
into the DLL (`map_old.h` describes each family and the conversion).

Navigation chunks: the engine's lexer sees the member only where Snapmap+ is
not installed. Measured live on Vulkan, an undeclared root member holding one
string of 8192, 65536 or 1048576 characters, or an array of 2048 strings of
4096 characters, loads and plays with no engine message above level 4, so each
set's base64 is cut into chunks of 1048576 characters (`SH_SMNAV_CARRIER_CHUNK`
in `navmesh.h`, which records the measurement).

## DLL interface

Map rendering lives in the backend and DOOM's native Settings / Properties
panel, in the Snapmap+ tab. The Unlock View Distance group owns an
Apply/Cancel draft (one toggle) tied to the map open in the editor. The two
built-in map settings are held by `map_settings.c` as the open map's state,
never in the engine's variable lists: every map parse reads them from the map's
member (`"settings":{"render":...,"bounds":...}`, see The map carrier), so the
Toybox variables list never shows them; every save writes them into the
member. On saves `render` = `4;0`; Off saves nothing. Applying a
change marks the map modified with one undo step; undo does not revert it.
Versions 1 to 4 stored a distance: 8192 and disabled version 2 read as off, any
other distance as on, and an on value saves as `4;0`. A value or a settings
part that does not read is off. Map deserialization clears the previous runtime
selection and selects the parsed map's settings; editor-to-play conversion
selects the open map's. A locked value copy crosses to rendering at
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
map setting (the member's `bounds`, held like the view distance, above) raises `snapEdit_environmentModuleBounds` and the
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
retained, with extensions through `+0x3A0`; the current table occupies `0x3A8`
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
paste decision, from `iface_engine.c`, `apply_engine.c` and `module_prefab.c`), then
`features_off` (`+0x388`, the one line naming the features that are off this session,
from `feature_health.c`), `preview_failure` (`+0x390`, why the latest image preview
failed, `preview.c`) and `sound_refusal` (`+0x398`, why the latest sound audition was
refused, `soundpreview.c`), and `free_text` (`+0x3A0`, bound by
`sh_iface_create` itself). The retired
timeline-inherit normaliser at `+0x298` stays in the table and returns 1. The backend writes a marker and the table's size into the object's
first reserved cell (`+0x08`); older backends left it zero and had `0x350`
bytes. Check `SH_IFACE_HAS_SLOT` before calling any slot at or past `+0x350`,
so a frontend never reads past an older backend's table.
Static assertions pin the layout. Do not insert, reorder or repurpose
existing slots. Add new capability slots at the end and update both DLLs.

Text crosses the interface as owned strings, never in a buffer the caller sized:
every slot that hands text or bytes to the window returns it, or stores it in
its call record, allocated by the backend from the process heap and sized to
what it holds, and the window releases it with `free_text` (the two DLLs link
different C runtimes; the process heap is the one both reach). Text the window
passes in is a NUL-terminated string of any length the backend copies before
the call returns. The window refuses a backend whose table is not its own size,
with the reinstall notice, so a pair built on either side of this rule never
runs together.

Headers define ownership, return values and thread requirements. Keep
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

One preview worker (`megapreview.c`) serves every Assets-tab picture: atlas
materials through the game's own page decoder (`sh_mega2_decode_page`, the one
place a stored page is prepared and decoded), and images and decl materials
from their installed `.bimage`, decoded from the smallest mip that still covers
the preview. A request ends published or failed; a failed one keeps a plain
reason (`sh_preview_failure`) for the tab to show instead of waiting.

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
holding `mod.json` and an `assets/` tree at the engine's own request paths.
Mods reach other machines two ways: each build a published map names is
uploaded to the Community API's mod store as one archive when the map is
published, and a player who opens that map is asked first and downloads only
what they approve in the dialog, verified against the map's row (`modnet.c`,
`modsync.c`, `modpublished.c`; see [The mod store transport](#the-mod-store-transport) and
[Published maps and missing mods](#published-maps-and-missing-mods)); that
transport is for publications only. An exported rawmap or prefab is a ZIP that
carries each mod exactly as it sits in the mods folder, its folder with
`mod.json` and `assets/` (`modexport.c`), and importing one is the ordinary
install of those folders, so sharing stays offline; every rawmap save of a map
that names mods writes that ZIP beside the JSON (see
[Rawmaps that name mods](#rawmaps-that-name-mods) and
[Prefabs that name mods](#prefabs-that-name-mods)). Plain saves never upload. The retired
`%LOCALAPPDATA%\snapmap-plus\overrides\` tree is read once, by the store tool's
migration below, never served or written, and moved to the Recycle Bin once
everything in it is in `mods\`; no `package-cache` or `package-staging`
folder is created.

These backend files share the work:

- `modbundle.c` builds a mod folder into a bundle: a SHA-256 per file, the
  logical name each file provides, the names its decls need, the namespace and
  structural rules, and the canonical manifest whose SHA-256 is the build hash.
  It reads and writes `mod.json` and the product record (below). A mod's
  identity is its salt (`modsalt.c`): eight characters `[a-z][a-z0-9]{7}`
  given once, recorded in the builder block, never read from the folder's
  name, which is the author's to change at any time. New names live under
  `mods/<salt>/` (a salt has one spelling in decl text and cooked paths), and
  every strings key is `<salt>_<name>` (letters, digits and `_`; no `/`, since
  the engine's string ids were only ever exercised with `[a-z0-9_]`), which a
  decl references as `#str_<salt>_<name>`. A strings key the game's language
  table or the product's built-in table already defines (`strkeys.c`: the
  game's keys compiled in as hashes from the supported build's English table,
  `strkeys_game.h`, and the baked set) is a string edit: it keeps that name, any
  mod may carry it, the build lists it under the block's `registries`
  `stringEdits` (the mod's own keys under `strings`), and it applies where the
  mod's other edits apply (below, the bundle string layer). Every file is classified from
  its path and bytes: an addition or a borrow (a stock pointer) under the salt,
  editor-only types included (a Toybox row travels with its mod), an editor
  tweak (an editor-only type outside the salt), a surface (the game's own bytes
  at the game's own name, served from the player's disk, never carried and
  never refused; SnapMap's provider cannot see campaign-only content without
  it), or a replacement (any other file at a game name). Whether a file is
  the game's own bytes is decided by bytes for a binary and by text for a
  file in the decl grammar (a decl, a snapModule decl, or a render
  program's permutation decl `generated/renderprogs/permutations/<program>_<mask>.decl`,
  `{ permutations { } parms { } state { } hlsl_vp { } hlsl_fp { } }`):
  line ends are not content (CRLF equals LF, the final run of line ends is
  ignored, `sh_mb_text_same`), so a copy an export turned from CRLF to LF
  is a pointer recording the game's hash and size (`mb_text_pointer`).
  Replacements and editor tweaks are the bundle's `shadows`, derived and
  never declared. A replacement that is a decl is an edited copy: its
  structural diff against the game's text is the product block's `edits`
  and the game file is generated per map (see [Derived edits](#derived-edits));
  a binary replacement fails the build, except in a folder the product
  received, and so does a copy outside the decl grammar. An edited
  permutation decl is decl text, never a binary, but it fails the build with
  what to do instead: the product generates edited game files under
  `generated/decls/` only (`ms_registry_files`), and a permutation is opened
  with its program's cooked `.bin` and SPIR-V, so the author either deletes
  the copy (the mod borrows the game's) or ships a whole program under
  `mods/<salt>/`. Of the 494 permutation decls the installed archives hold,
  324 parse as decl trees; the rest carry HLSL with `=` at statement level. Rules 11 (`SH_MB_RULES`):
  files beside `mod.json` and `assets\` are ignored, and campaign content derives its requirements, so
  every own folder is built again once.

  Lengths: the product puts no bound of its own on a name, author,
  homepage, request path, folder path, nesting depth, cause or script path.
  Each is a heap string of its own length (`src/common/text.h`). What
  remains is the platform's or the format's: Windows' long-path form (32,767
  UTF-16 units, `SH_WPATH_MAX`) and its 255-unit file name per folder, the
  ZIP format's 65,535-byte entry name, the engine dialog's 255-byte text
  (a longer notice prints whole to the console and the dialog says so), the
  vendored Lua's 60-byte chunk name in its own messages (the runtime reads
  the whole name from the stack instead), and the fixed fields of the store
  tool's records, where a longer sentence is written whole to the log and
  the field says so. The window interface carries text of any length (owned
  strings, below). Nothing is cut silently.

  `mod.json` format 2: the author's part is every member but `snapmap-plus`
  (`name`, `description`, `author`, `homepage`, `requirements`,
  `strings`, `hud`, `scenes`, `companions`; any other key is kept in the manifest's
  `extra`); the builder block holds `format` 2, `salt`, `rules`, `built`
  (the first successful build of that salt in that folder), `tree` (the
  rebuild rule's tree hash, `sh_mb_tree_hash`), `build` (the build hash),
  `cause` (while a build fails or a verification found one), `warnings`, the
  derived `classes`, `editor`, `registries`, `edits`, `shadows`, and the
  `files` and `stock` lists (a pointer the campaign-only closure derived is
  marked `"closure": true`). A failed build writes the block with `format`,
  `salt`, `rules`, `built`, `tree` and `cause` only (`sh_mb_write_failed`):
  the folder serves nothing until fixed and the load says "not built:
  <cause>". The canonical manifest has `format` 2 and `id` the salt; there is
  no build counter. An earlier shape (`package.json`, a block of an earlier
  format, or a retired author key: `id`, `key`, `shadows`, `stock`, `scope`,
  `player`, `extends`, `replaces`, `legacy`, `composes`, `schema`, a `hud`
  `schema`) is refused with one sentence, "This mod was made for an earlier
  version of Snapmap+. Run the Snapmap+ installer again and it converts it."
  (`SH_MB_EARLIER`, `sh_mb_descriptor_state`); a format-2 block without a salt
  is damage the sentence names, and a file under `mods/<x>/` for another `x`
  than the folder's name or salt is refused naming the file
  (`sh_mb_salt_paths_check`). Files beside `mod.json` and `assets\` (a README,
  `desktop.ini`, a `.git` folder) are ignored and never carried. A mod that
  brings campaign content (a closure pointer or a surface) gets the
  requirement `g_useResourceBlackList` 0 and `g_useImageBlackList` 0 derived by
  every build and load (`mb_derive_requirements`); an author value for either
  is replaced with a warning. The registered extra check
  (`sh_mb_set_extra_check`, `modcontent.c`) reads the author's `scenes` and
  `hud` blocks with the game's grammar at build, so a mistake is the build's
  cause in the Mods tab.
  `sh_mb_build` takes the salt from the block, or from its caller for a folder
  not salted yet; `sh_mb_salt_modjson` writes the block a first scan gives a
  folder (`{"format": 2, "salt": ...}`) and rewrites the author's own
  references to `mods/<from>/` (`sh_mb_rekey_text`, `sh_mb_rekey_path`: the
  folder's leaf, or a copy's old salt). The readers of earlier shapes live
  only in the store tool (`modbundle_old.c`, `sh_mb_old_load`), which converts
  existing folders once at update; the game DLL never links them. The Mods
  tab's New mod writes an example that places and plays untouched: an
  entityDef `mods/<salt>/example_crate` with the game's SnapMap `crate01`
  model, its Toybox row (`snapEditorEntityDef`, nothing binary: the game's
  `prop/new/crate01` row field for field but its names and its `entityDef`,
  so it names no `renderInfo.renderModelName`, which only rows whose entity
  has no model of its own carry) and the row's strings in the author's part.
  A need that resolves to nothing fails the build, with
  three exceptions that only warn: an image (stock materials name
  virtual-texture sources with no standalone image), a need of a replacement,
  and a need the game's own text also names ("names <x>, which no archive
  holds; the game's own <decl> names it too"). That last one covers a stock
  pointer (its text is the installed text) and a carried decl under the salt
  whose stock twin, the installed decl at its request with `mods/<salt>/` cut
  (`sh_mb_strip_key`), names the same resource: shipped md6Defs keep event
  and alias blocks of clips no archive holds, so a copy of one builds without
  dropping them. A need only the mod introduces stays an error. A model name
  (`.lwo` and the like) resolves at `cooked/model/<stem>.bmodel` or
  `generated/model/<stem>.bmodel`: the md6 collision models the
  death-collision entities name (`md6/.../collision/<x>.lwo`) ship only
  under `generated/model/md6/`, beside `generated/cm/md6/`.
  Before the needs are resolved the builder closes the bundle over
  campaign-only content (`mb_close_campaign`): SnapMap's session loads
  `snap_gameresources`, never `gameresources`, so a served decl naming
  content only `gameresources` holds would leave a hole (issue 132's
  `designsystems/med_station_heal` naming `wallhealthstation.md6`). Each
  such name a file needs, at any depth through the decls brought along, is
  added as a pointer at the game's own path (a surface, `"closure": true`,
  nothing carried or uploaded), found with `sh_mb_stock_container` (a name
  both archives hold is SnapMap's own and adds nothing; a name no archive
  holds is resolved as before). A decl added this way is scanned for its own
  needs; what it names and no archive holds is the game's own hole and is not
  reported. There is no file or manifest cap; the build notes "N game files
  borrowed for completeness". Another installed mod pointing at the same
  campaign file does not settle a need: that mod is served only where a map
  declares it, so each bundle carries its own pointers. (Until rules 5 the
  store's build, whose resolver includes every installed bundle, let one
  stand in, so the SP doors folder rebuilt in the game derived no pointers
  while a bare build of the same folder derived 29; snaphak-re triage V2,
  F7.) The closure follows models, effects, movies
  and images: an animWeb's `modelInfos` pairs and `models` list name
  md6Defs and its nodes' `.md6anim` names are anims; an effect (`fx`) names
  particles (`declPrt`), ribbons (`declRibbon`) and materials (`lightMtr`,
  `decalMtr`); a particle or ribbon names materials (`material`); an
  entityDef names movies (any `.swf` value, `guiData` `swfFile` and
  `guiFile`), a movie `swf/<stem>.swf` being `generated/swf/<stem>.bswf`
  and its atlas `generated/swf/<stem>.bimage` coming along with it. A
  material's image is added only where SnapMap would not serve it: an image
  no archive holds standalone may be a transparency or decal atlas of that
  name (`generated/transsortatlas/` or `generated/decalatlas/`, nothing
  when SnapMap mounts it), and an image a virtual-textured material
  (`usevirtualmapping` set) names in a source field (`diffusemap`,
  `bumpmap`, `powermap`, `specularmap`, `bloommask`, `sssmap`,
  `colormaskmap`) needs nothing when the material has a row in the installed
  `virtualtextures/_vmtr*.vmtr`, whose baked pages the engine samples. A
  name of these kinds no archive holds is reported, not refused, like an
  animWeb clip. A material's render program (`stageprogram` and the other
  `*Program` keys) is one unit too (`mb_closure_program`): when only
  `gameresources` holds the program's decl, the closure brings the decl,
  `generated/renderprogs/<name>_pc_vulkan.bin` (and an OpenGL `_pc.bin` if
  the archive has one), the SPIR-V the archive holds at
  `sh_mb_spirv_request`'s names (split names included), and every
  permutation `generated/renderprogs/permutations/<name>_<mask>.decl` the
  installed indexes list (`sh_modstock_list`) with its own `.bin` and
  SPIR-V; the shared includes under `decls/renderprogs/` never come along.
  This reproduces `archive_renderprog.py`'s closure for all 488
  `gameresources` and 435 `snap_gameresources` programs of 13954591 (59
  are campaign-only, `newwater` among them). A program served without its
  permutation files stops the game (0xE06D7363 when the permutation open
  fails), so a missing `.bin` or a program with no permutation fails the
  build naming the member, and the stage files are then checked from the
  `.bin` as for a mod's own program (see [Render programs](#render-programs)).
  A program SnapMap holds adds nothing (the 429 programs both archives
  hold have the same files in each). Measured on the owner's water mod: a
  folder of `mod.json` and the `templar_water` material gains the 8 files
  of `newwater`, and the folder with its two `.bin` files deleted gains
  those two. A model is one unit (`mb_model_needs`): the engine loads an
  md6Def's mesh, then the skeleton whose name the cooked mesh carries (a
  little-endian u32 length at 0x0C, then the name; "MM" at byte 2), then that
  skeleton's default clip `<skeleton>/_default.md6anim`
  (`cooked/anim/<skeleton>/_default.bmd6anim`). An md6Def usually names only
  the mesh, so for every md6Def a bundle serves (carried, a pointer, a
  surface or brought by the closure) the builder reads the mesh's skeleton
  name from the bundle's own copy, else the installed one SnapMap loads
  first, and records the skeleton and the default clip as needs beside the
  mesh when something holds them. These are names: SnapMap's stock adds no
  pointer, a campaign-only skeleton or clip becomes one. The derived
  pointers are recomputed every build, never carried forward: the product
  block lists them in `stock` with
  `"closure": true`, a rebuild drops them and derives the set again from its
  own needs (a pointer whose need is gone is gone, and the hash changes; an
  unchanged folder keeps its hash), while an author `stock` entry and a
  pointer a download or migration brought stay. A block of rules 3, which
  had no mark, hands its pointers at the game's own path to content only
  `gameresources` holds to the closure the same way. Measured on build
  13954591: the owner's SP doors folder gains 29 pointers (the
  wallhealthstation, access panel and desk scanner md6Defs, 8 meshes and 18
  clips), 40 with whole models (the 8 meshes' skeletons and 3 default clips),
  through the store's build as through a bare one; of the six
  campaign-interactables families of fidelity85, `-1` gains
  `player/crucible_eyes` and `-2` the crucible particles, ribbon and
  materials, the gauss_beam transparency atlas and the `switch2_up` and
  `automap_hell` movies with their atlases (11 names, the ones fidelity87
  added by hand); the other four gain none.
- `modstore.c` owns the store: the mod folders, install, verification, the
  rebuild rule and the one-time upgrade of the retired layout, below, which the
  store tool runs. Files are hashed at install and on `sh_mods doctor verify`,
  never at boot.
- `modstock.c` is the read-only installed-resource bridge behind stock
  pointers, below. Its `container` operation (`sh_mb_stock_container`) names
  the archive that holds a pointer's source, `snap_gameresources`,
  `gameresources` or a loose file, by the row's name, hashing only a name both
  archives hold; the Mods tab's Files view says from it where a game file comes
  from, and `mod.json` does not record it.
- `modcmd.c` is the `sh_mods` console command. It dispatches the serving verbs
  (`status`, `rescan`, `palette rebuild`, `held`) to `modserve_engine.c` (a rescan runs
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
  It applies a refresh in place (re-parse, reload, the anims of a map pass) and
  arms the companions detour at its main-thread gates.
- `modtypeinfo.c` serves the default property trees of the entity types the
  editor's type-info cache (`idSnapTypeInfoCache`) lacks. The game builds
  that cache once per process, one default property tree per
  `snapEditorEntityDef` registered at that moment, keyed by entityDef name,
  with room for 1,499 types (its builder stops the game at 1,500 registered
  rows, so it can never run again on an installation with many mods), and
  every placed entity's tree starts as a copy of its type's
  (`idSnapTypeInfoCache::Get`). A row registered after the build (a mod's)
  has no cached tree: the game builds the entity's tree from the instance
  alone, reads its model scale as zero (`modelMatrix invert failed on model
  <model>` every frame the entity is drawn on its own) and draws the hidden
  badge over it. The product detours `Get` and its lookup-only sibling: a
  name the game's cache holds is served by the game untouched; a name it
  lacks is served from the product's table, where the tree is built on first
  use the way the game's builder builds each entry (the type's default entity
  created through the game, its state written to text, the entity destroyed,
  the text parsed into a tree) inside process heap 0 for the life of the
  process. `Get` runs on a one-entry view of the table so the game's own
  copy-and-apply path makes the result; the lookup returns the tree itself.
  The table serves only after a boot self-check that builds three of the
  game's own cached types the product's way and finds them equal node for
  node; one `MODS: typeinfo:` line per boot says how many mod rows resolve
  through the table. An entityDef re-parsed from other bytes (a refresh, a
  map that serves its own copy of the salt) loses its entry and is rebuilt at
  its next use; the old tree is never freed, since the lookup handed the
  engine the tree itself and the engine may still hold it.
- `modledger.c` is the served-bytes ledger: per request path, the bytes the
  provider gave the engine this session, read lock-free by the refresh
  decision and `sh_mods held` (the diagnostic build, `build.ps1 -Diag`).
- `modmanifest.c` owns the manifest rows (a placed mod's row and a map-level
  one, `"attached": true`), carried in the map member's `mods` part (The map
  carrier). A row is `{"mod":"<salt>@<build hash>","name","size",
  "names":[...],"attached"?,"choices"?}`: the salt names the mod, the hash its
  build, and `name` is the author's, for display. A mod has no version: the
  build hash alone tells builds apart, and no player or author sees or sets one. Rows merge by
  salt and build; a row naming an `id` or a `bundle` does not parse (the parse
  detour has already refused such a map). The display name of a placed mod's
  row comes from the build that serves it at the save (the row resolver).
- `modui.c` is the WebView Mods tab's backend (payload v4). It lists one row
  per mod folder, the player's own at the root (with their organizing folder,
  `group`) and each downloaded map's in that map's place (`place`,
  `placeTitle`), with what each adds, changes and borrows from the game, marks
  each ready or needing attention with one plain sentence saying what is wrong
  and what to do (each cause is written plainly where it is raised;
  `mu_plain_cause` adds the next step for the causes the window knows one
  for, and an earlier shape gets the one sentence that sends the author to
  the installer), says when another of the player's mods carries at least
  half of a mod's files at equal bytes (`sameFilesAs`, informational: two
  mods stay two mods), describes the open map and the mods it uses, and runs
  Install (folders or ZIPs), New mod, Refresh and Add to my mods (a
  downloaded map's mod copied to the root), whose file work runs on the
  window's worker with no time limit while only their game part
  (`SH_MODS_ACT_APPLY`) runs on the main thread, and Uninstall, Use in this
  map, Remove from map, their dry runs and the registry choice on the main
  thread, as in [The Mods tab's map scope](#the-mods-tabs-map-scope). There
  is no Migrate, no Rebuild (Refresh builds every changed folder), no folder
  watcher and no build list. The window never sets where a map finds its
  mods. What the tab does not show, every
  folder's state, conflicts with their winners, the game files mods replace
  and verification, it prints as `sh_mods doctor [verify]` (see
  [WebView UI](webview-ui.md#mods-tab)). Its Files view reads only:
  `modFiles` lists a folder's files (walked on disk, dot folders skipped) and
  the game files its build reads from the install, each with its class
  (`f->cls`), whether it is the game's own bytes and their archive, and the
  plain cause when the folder's cause names the file; `modFileText` answers
  one text file of at most 1 MiB of UTF-8 inside the folder (`mod.json`
  without the part Snapmap+ writes). Neither is followed by a fresh document.
  New mod is `newMod`, whose result says Refresh picks up the author's edits.
  The rows a map save writes are resolved through `sh_modserve_row_build`
  (the resolver modui.c binds), and the listing's undeclared names take the
  build `sh_modserve_row_build` names in the loaded map's place (set by the
  map's parse and the published gates, `modserve.h`); the editor's exit and
  the end of a launched Play return it to the root.
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
- `modzip.c` is the product's ZIP form, shared by those exports and the mod
  store's build archives: the writer stores every entry (method 0) with its
  CRC-32 and a fixed 1980-01-01 time, so the same content is byte-identical;
  it writes ZIP64 records exactly where a value does not fit a classic field
  (an entry of 4 GiB or more, an offset past 4 GiB, 65,535 entries or more),
  so an archive has no size or file-count bound and one that fits is the
  ordinary ZIP it was; the reader reads both and trusts no field of an
  archive: at open it checks the end record, the ZIP64 locator and end record
  when the classic fields point to them, the directory's bounds and, for every
  entry, the method (stored or deflate only), the encryption flag, the ZIP64
  extra field a marker needs (holding exactly the values the markers ask
  for), a stored entry's equal sizes, a local record and data lying before the directory, a name without
  NUL that is safe (no absolute path, `..` step, backslash, colon or control
  character) and no two names that differ only in case; anything else
  refuses the whole archive in one sentence ending "Ask whoever shared it to
  export it again". A read bounds its allocation by the caller's limit and
  checks the CRC-32. It can stand over a plain folder (a prefab file and its
  `mods\` beside it). `modzip_test` fuzzes it with every truncation and
  one-byte change of an ordinary and of a ZIP64 archive, writes 70,000
  entries, and (with `SH_ZIP_LARGE=1`) an archive past 4 GiB.
- `prefab_mods.c` keeps a prefab's description, tags, summary and the mods it
  needs in the prefab file's own `snapmapPlus` block (`prefab_block.c` finds,
  inserts and strips it, and removes the serializer's comments so the file is
  plain JSON), refuses an older prefab (a sidecar and no block), resolves
  the mods before Load / Place, and writes and reads a prefab's ZIP, as in
  [Prefabs that name mods](#prefabs-that-name-mods).
- `modmigrate.c` moves the retired `overrides\` tree and `strings\strids.json`
  into the store once (the store tool runs it; the DLL does not link it), as in
  [Migration of the retired overrides folder](#migration-of-the-retired-overrides-folder).
- `modsound.c` delivers the Wwise sound banks bundles carry, and loads the
  stock banks SnapMap skips that a declared mod's sound events need, with
  `modbankinfo.c` reading the game's bank index, as in [Mod sounds](#mod-sounds).
- `modvt.c` gives each virtual-texture board a mod carries
  (`virtualtextures/mods/<salt>/<name>_sq<N>.mega2`, the builder's only form of
  `.mega2`) its own page-source record on the game's material page cache and
  feeds it to the stock lit programs, as in
  [Mod virtual-texture boards](#mod-virtual-texture-boards).
- `companion.c` reads the `companions` key of every admitted bundle's author
  block and `companion_engine.c` spawns them, as in
  [Owner-bound companions](#owner-bound-companions).
- `modpublish.c` uploads a map's bundles when the map is published, claims
  them under the published item, releases the claim at unpublish, and refuses
  a copy to My Maps that the map service would reject, as in
  [Publishing a map with mods](#publishing-a-map-with-mods).

The store. A folder is `mod.json` plus `assets\`. `mod.json` is format 2
(above): every key but `snapmap-plus` is the author's, and `snapmap-plus` is
the builder's, the whole built manifest with its salt, rules, built time,
tree hash, build hash, cause while failing, the derived `classes` counts
(`adds`, `borrows`, `edits`, `replaces`, `editor`, `surfaces`: an edited copy of a game file
counts under `edits`, never `replaces`, which is only what a folder the product received replaces;
the build and `sh_mods list` lines print them the same way), `editor` (the editor tweaks' request paths), `registries` (banks,
conductor, strings), `edits` (the derived edits of each decl at a game name), shadows, `files` (path, sha256, size, provides, needs) and
`stock` (pointers; `"closure": true` on those the campaign-only closure derived).
There is no last good build (ONE-SHAPE rule 2): a build that fails writes the
block of a failed build (`sh_mb_write_failed`: format, salt, rules, built, tree
and the cause), the folder serves nothing until it is fixed, and the Mods tab
and `sh_mods list` show the cause; the serving index skips such a folder with
`MODS: bundle '<folder>' skipped: not built: <cause>`. A `mod.json` that does
not parse is listed with its parse error (`sh_ms_descriptor_unparsable`),
nothing in it is written, and it serves nothing either (its cause is the
store's, `sh_ms_build_cause`, else the parse error). The serving index reads a
folder's identity from the block's `salt` only (`ms_parse_bundle`), and
refuses every earlier shape with one sentence and the next step: a block of
another format and `package.json` with "This mod was made for an earlier
version of Snapmap+. Run the Snapmap+ installer again and it converts it.", a
retired author key (`id`, `key`, `shadows`, `stock`, `scope`, `player`,
`extends`, `replaces`, `legacy`, `composes`, `schema`, `hud.schema`) with the
builder's sentence (`SH_MB_SHAPE`) naming the key to remove, a block without
a valid salt as a damaged block, and a folder in a map's place that the store
did not build. A
root folder with no block is "not built yet": the store's next scan salts and
builds it. The
product writes the block by splicing: the old member is cut from the end of
the member before it, the new one appended after the author's last member, so
the author's bytes, order and formatting never change. The canonical manifest
is derived from both blocks in memory, for the hash and for the upload. There is no
blob store and there are no hard links: a mod's files live in its own
`assets\` only.

Identity is the salt (`modsalt.h`), never the folder name: an author may
rename any folder at any time and the next scan reads the same salt. A folder
under `mods\` without `mod.json` or `package.json` is an organizing folder and
is looked into at any depth (only the Windows long-path limit bounds a path); one with a descriptor is a
mod and is not entered, named by its path under `mods\`
(`demon-mods\cyberdemon`), which the listing, the console, the Mods tab and
`sh_mods build` use; the console also takes a nested folder by its own name or
by its salt. Every reader walks the same way (`sh_ms_walk`: the listing
`sh_ms_enumerate`, the scan `sh_ms_sync`, the serving scan): dot folders,
reparse points and `<name>.staging-<pid>` / `<name>.old-<pid>` work folders are
skipped, and the top folder `maps` is the places (below). The root is
everything else, and one salt has one folder there.

The scan (`sh_ms_sync`), at boot, on Refresh, and after an install, a download
or New mod; there is no folder watcher. Under the store lock it:

1. salts every root folder with no builder block (`sh_ms_assign_salt` with
   `from` its folder name): a new salt (`sh_salt_generate_except`, never one a
   folder of the store holds), every reference to `mods/<folder name>/` in its
   own files rewritten to `mods/<salt>/` (decl text and any other text, the
   folder segments of paths under `assets\`, both spellings; `sh_mb_rekey_text`,
   `sh_mb_rekey_path`, `sh_mb_salt_modjson` for `mod.json`'s references and
   its block `{"format": 2, "salt": ...}`). Its strings keys gain the salt's
   prefix, `<key>` becoming `<salt>_<key>` (a copy's `<old salt>_` replaced;
   a string edit keeps its name),
   and every `#str_<key>` reference in its own text files, `mod.json` among
   them, follows (`sh_mb_salt_strings`, `sh_mb_restr_text`), so an author
   writes plain keys in a new folder; a binary file naming one of them refuses
   the folder as a binary naming `mods/<folder name>/` does. A sound bank
   named `<folder name>_x.bnk` is renamed `<salt>_x.bnk` and its 4-byte BKHD
   id set to FNV-1 of the new name; a bank whose id does not match its name
   has it corrected. The same move runs before every later build of a root
   folder (`ms_rehome`, DECISIONS 20) with the salt kept: files under
   `mods/<folder name>/`, references to it, new plain strings keys and banks
   named for the folder follow the salt, the builder block stays, binaries are
   left to the build, and nothing is written when nothing is under the name.
   A folder name that is a salt of the store names that mod and is never moved;
   renaming a folder changes nothing already moved. Everything is read and rewritten in
   memory first, staged beside the folder as `<name>.staging-<pid>` (unchanged
   files hard-linked), then swapped in with two renames under the same name, the
   second undone on failure; the folder as it was goes to the Recycle Bin. A
   binary file naming `mods/<folder name>/` ("<file> names mods/<x>/ inside a
   binary file, which Snapmap+ cannot rewrite"), files under another mod's
   `mods/<x>/`, a file that cannot be read, two files landing on one path or a
   refused rename leave the folder exactly as it was, refused alone with that
   cause (`MODS: store: <folder> refused: ...`);
2. settles a salt two root folders carry (a copied folder): the folder the
   record's `salts` section names (the one that held it at the last scan)
   keeps it, else the folder created first, never the build time (an Explorer
   copy carries the same `built`); every other one is salted anew with `from` the old salt, one
   line each ("mods\<b> is a copy of mods\<a>; it is a new mod now");
   until then the listing shows the copy refused with that reason;
3. builds every root folder whose block's tree hash (`sh_mb_tree_hash`: the
   author's bytes and the size and time of every file under `assets\`) or rules
   (`SH_MB_RULES`) differ from the folder's, or whose last build failed
   (`sh_ms_sync_pending` counts them, with the folders of steps 1 and 2). Place
   folders are never built.

A folder in an earlier shape (a format-1 block, `package.json`, a retired
author key) is listed with its plain reason ("This mod folder is not in a shape
Snapmap+ can build: ...") and nothing in it is written; the installer's store
tool converts every such folder it finds each time it runs.

Places (rule 4). A build the product receives for a map lives in the map's
place, `mods\maps\<segment>\<mod name>\`: the segment is `sh_ms_title_segment`
of the map's title (a rawmap's file name, a prefab ZIP's name: the characters
Windows refuses dropped, at most 60 bytes, a device name given a `_`), followed
by ` (<map id>)` only when the record's `places` section already gives that
segment to another map; the folder is named after the mod (` (2)` when another
mod of the place has the name). One build per salt per place: a later build of
the salt for that map replaces the place's folder (`sh_ms_place_folder`
answers 2 with it), the old one to the Recycle Bin. The `places` section maps
each map (its item id, `zip:<the ZIP's path>`, `place:<segment>` for a fetch into a map's
recorded place, else `title:<segment>`) to its title
and place (`sh_ms_place_note`, `sh_ms_place_of`, `sh_ms_place_title`), so the
Mods tab says which map a place serves. A new folder dropped into a place is
refused (`SH_MODSERVE_PLACE_FRESH`: move it out into your own mods); a hand
copy inside one place is refused ("This map's folder holds this mod twice").

Nothing in the product removes a downloaded map's mods by itself (DECISIONS
29): a place under `mods\maps\` stays, played or not and used by a saved map or
not, until the player removes a mod of it with the Mods tab's Remove
(`sh_ms_delete_mod_folder`, one folder; an emptied place goes with its record
entry).

Install at the root (`sh_ms_install_to_root`). A built folder of a map's place,
or a received build unpacked outside the store, is copied to `mods\<its name>`
keeping its salt and verified; a root folder of the salt at that very build is
the install, and one at another build refuses it (the player's own folder of a
mod stays theirs). Add to my mods, a placed prefab whose mod a map's place
holds, and a download for a map without a place all install through it.

Install (`sh_ms_install_from_folder`). A folder of the store is built in
place. From elsewhere the source is only read: a new mod is copied whole to
`mods\<its name>` (` (2)` when taken), salted with its references to
`mods/<its name>/` rewritten, and built, and a refused build leaves nothing
installed; a built mod is built in memory, and when the root folder of its
salt already holds that build nothing is copied (`present`), else its carried
files and `mod.json` are copied beside the others under its own name and
verified, and when the root already holds the salt the arriving copy gets a
new salt and is built under it (`resalted`: the arriving folder is the newer
copy whatever its `built`). `sh_ms_install_placed` installs a built mod into a
map's place instead; `sh_ms_install_as` names the store tool's target, which it
replaces. A map without a place (the player's own) follows the root folders with no
prompt: every row is activated through the build the resolver below answers,
the root folder of the row's salt whatever hash the row pins, and the next save
writes that build's hash, as the save already writes rows from the serving
index. The map scope's listing holds that served build, not the hash the row
recorded (`mu_listing_set` asks `sh_modserve_row_build`), so everything the
scope declares comes from the root's build: its decls, registries, scripts,
virtual-texture boards, strings and banks. A load logs one line for each such
row (`MODS: map: <salt>: saved with build <hash>; the root's build <hash>
serves it and the next save records that one`) and the Mods tab's doctor says
the same; no notice interrupts an author who rebuilds a mod while iterating.
A stranger's map has a place, so its rows are listed at their own build and a
different build at the root never stands in for one. There is no upgrade action
and there are no aliases.

New mod (`sh_ms_new_mod_example`): a folder `mods\<the name as a folder
name>` (` (2)` when taken; never `maps`) with a new salt in its block, an
example crate at `mods/<salt>/example_crate` and its Toybox row, and the row's
strings in `mod.json` keyed `<salt>_category`, `<salt>_example_crate_name` and
`<salt>_example_crate_desc` (referenced `#str_<salt>_...`); the scan that
follows builds it.

The map's place (DECISIONS 11). Where a map finds its mods is a fact of the
map, never a global setting. A place is `""` (the root of `mods\`: the
player's own maps) or `maps\<segment>` (a map that arrived with its mods: a
download, a rawmap ZIP import). The saved map records it in its member
(`"place":"<segment>"`, `map_carrier.h`; `rawmap.c` `place_on_save` writes the
loaded map's place at every save and every rawmap write), and every parse sets
it from that record before the manifest step (`place_prepare_guarded`,
`sh_modserve_place_load`; a record that is not a place is the root, and a
failed parse puts the previous place back with `sh_modserve_place_restore`).
The published gates then set a stranger's map's place from its item, and a
refused launch puts the editor map's back. `sh_modserve_place_norm` admits a
segment only when it is exactly what `sh_ms_title_segment` makes of it, so a
map cannot name a folder outside `mods\maps\`.

The asset rule. A map, rawmap or prefab names the builds its manifest rows
pin by salt and build hash. One resolver in `modserve.c` answers which
installed build serves a row of a map whose place is P
(`sh_modserve_row_build`, `sh_modserve_row_served` for the loaded map's place;
`sh_modserve_row_build_in`, `sh_modserve_row_served_in` for a given place,
changing nothing): the root folder of the salt at the row's hash; then, with a
place, P's folder of the salt at the row's hash, and without one, the root
folder of the salt at any hash. A bare hash (the listing, plans) resolves the
same way, the root's folder at the hash and then P's; describing a build (its
scope, its state) falls back to any folder that holds it. Another map's place
never answers a row: each map
keeps its own copy. `sh_modserve_row_served(row, cause, cap)`
(`sh_modserve_row_served_for` with the word "prefab") is the one predicate
every hold uses: a row is served when that build is admitted and every name it
lists resolves, in that build or through the serving index; missing when no
build answers (the fetch brings the row's build into the map's place);
incomplete when the build is admitted but a listed name does not resolve. Its
cause is the refusal: "This map uses Hell Guards, which is not installed.",
"... which is installed but cannot be used: <cause>." (a folder of the salt
was skipped: the one at the row's hash, else any when no folder of the salt
serves) or "Hell Guards is installed but does not provide
mods/<salt>/hell_guard_combo_enc, which this map uses." (the first three
missing names, then "and N more"; the installed folder's name, else the row's,
else the salt; "prefab" for a prefab). The scope listing is keyed by hash
([Map scope](#map-scope)); a hash held in several areas names the open map's
place's folder first, then the root's (`ms_bundle_by_hash`).
What a placed entity depends on. The save closure and the map listing resolve
each placed name (`sh_modmanifest_placed_names`: the `inherit` and
`moduleName` values and every string in an entity's `edit` state) through
`ms_name_file`: an entityDef provider, else any served decl or snapModule of
that name, else, for a model reference (`renderModelInfo.model` and the like:
a `.lwo`, `.ase`, `.ma`, `.obj`, `.fbx` or `.bmodel` path), the file that
provides it under its cooked request (`sh_mb_model_key`:
`cooked/model/<stem>.bmodel`), carried or a stock pointer. So a mod whose only
content unlocks a blacklisted stock model an author placed by path is named by
the map and uploaded with it. A name whose provider is a shadow of editor-time
content (`sh_mb_editor_time`: the snapEditor*, snapPropertyInspector* and
snapPalette decl types, the editor's own movies) names no bundle: such a
shadow changes what the author can set, never what plays, so an author's
editor overrides never become a dependency of a map, vanilla or not. A
gameplay shadow (an entityDef, a material, a model) still does.

The player side of the same rule: a row is served only by an installed build
(`sh_modserve_row_served_for`), so a build a map names is installed like any
other (an install drops nothing).

`mods\snapmap-plus.json` is the product's one record:
`claims`, `links`, `pending`, `migration` and `places` (each map's place,
above), each section written whole under one lock. The
build cache, aliases, snapshots and first-build renames of earlier releases
are gone: the block's `tree` is the rebuild rule's key.
A record another program holds open is waited for, up to a second. A read
that still fails logs `MODS: record: cannot read <file> (<why>)`, and that
section is not written until a read of it succeeds again, so a
read-modify-write never replaces what it holds with less; a write while the
file cannot be read is refused with the same kind of line. A record that is
not JSON is moved aside as `snapmap-plus.json.unreadable-<time>` when a
section is written, and a new one started; the line names the aside file.
The player reads the same failure where they acted: install and uninstall in
the Mods tab (and their console verbs) add "Snapmap+ could not update its
records: <file> could not be used (<why>); close any program using that file
and try again." to their result, and a publish whose claim could not be saved
shows that sentence as its upload cause (`sh_mb_record_why`).

Removal goes to the Windows Recycle Bin. Uninstall, Remove and the removal of
covered builds move the folder there (`sh_ms_delete_mod_folder`: a mod
folder of the store, under the store lock, through `SHFileOperationW`
`FO_DELETE` with `FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT |
FOF_NOERRORUI | FOF_NORECURSEREPARSE`, a double-NUL-terminated plain path,
reparse points moved as themselves and never entered; a folder in a map's
place takes its emptied place folder with it and leaves the record's `places`
section), and rescan. `sh_ms_remove_folder`, which the store upgrade and an
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
folder from the Recycle Bin by hand (the next Refresh builds it again) or the
download offer of a published map, a rawmap or a prefab installs
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

The store tool. The game converts nothing at boot.
`snapmap-plus-store.exe` (`src/store/`) links the store sources (`modstore.c`,
`modstore_upgrade.c`, the store tool's half of the store: the earliest
per-build layout, the second upgrade step over format-1 folders read with
`modbundle_old.c`, and the bin sweep, never linked into the game DLL;
`modmigrate.c`, `modlegacy.c`, `modbundle.c`, `modstock.c` and what they use)
with a stand-in for the serving layer (`store_seam.c`), and no engine, DLL or
WebView code. `check <store>` lists the work it finds, one line per kind
(folders of the retired layout, `blobs\`, `legacy\`, the old record files, the
second upgrade step, `removed-mods\` or its record section, package entries of
`overrides\` the record does not mark as migrated, the loose files of
`overrides\` and a `strings\strids.json` not yet moved, a document that is not
a strings document, and an `overrides\` or `strings\` folder left to retire)
and exits 0 when there is none, 1 when there is, 2 on an error; it changes
nothing (`sh_ms_upgrade_scan`, `sh_modmigrate_loose_scan`).
`migrate <store> [--doom <folder>]` refuses while DOOM runs, then runs the
layout upgrade, the bin sweep, the overrides migration, the loose step and the
retirement of `overrides\` and `strings\` (`sh_modmigrate_retire`), each only
when `check` finds its work, with every delete through `sh_ms_set_discard` to
the Recycle Bin (`store_recycle.c`: `SHFileOperationW` with `FOF_ALLOWUNDO`, on
a fixed local disk only, links removed first). The overrides migration and the
loose step's loose files need the game's archives for their stock pointers: the
`--doom` folder, else the one Steam names; without either they are reported and
left (a strings document alone needs neither). It prints the store's own log
lines as its action lines and a summary, and exits 0 when nothing is left to do,
1 when a later run could still do something, 2 on an error; a second run finds
nothing and changes nothing. The installer runs it on install and update (see
[installer/README.md](../installer/README.md#mods-and-retired-package-folders)).
A folder without a builder block is salted and built at the next scan, or at
the next boot (below). A folder in that earliest layout is written as a format-1 `mod.json`
(its manifest's author fields and a block naming its key and build) for the
conversion below; one whose `bundle.json` does not read is left as it was and
logged.

The conversion to the one shape (`store_convert.c`, store tool only) runs after
those steps, once per store: what an earlier release wrote is converted so the
game reads only the current shape. Folders: every folder of `mods\` outside
`mods\maps\` whose `mod.json` is an earlier shape (`sh_mb_old_is_old`, or a
`package.json` alone) has its old key (author `id`, else the block's `key`,
else its name). Of several folders of one key, the player's own are kept and
the extra copies the product made (`<key>@<hash16>`, those under `downloads\`
or `imports\`) go to the Recycle Bin once a kept one is converted; with only
product-made ones the one that held the key first is kept. Which folder holds
a key is read from the record (the `converted` record's folder for the key,
else the folder named after the key, else the first by path), never from a
`built` time (DECISIONS 15). A kept folder's author part is cut free of the
block, the retired author keys and a hud `schema` (every other byte kept),
salted into `<name>.staging-<pid>` by `sh_ms_salt_copy` with `from` the old key
(the same rewrite as the first scan's: paths, texts, strings keys
`<salt>_<key>` and their `#str_` references) from a view of `mod.json` and
`assets\` only, its sound banks renamed `<salt>_<name>` with their BKHD id and every text of the folder that names one (decls, mod.json),
every other file beside them copied unchanged (DECISIONS 16), and built there
by the real builder over the store's installed bundles and the game's archives
(`sh_modstock_exists`, replacements kept, stock-bank needs recorded through `sh_mb_set_bank_need(sh_mbi_bundle_need)` as the game records them). A build that fails, or a salting the
folder refuses (the folder is then copied as it is with a fresh salt), is
written as a failed build (`sh_mb_write_failed`) with the builder's plain cause
(DECISIONS 14). The work folder is swapped in under the folder's own name and
the folder as it was goes to the Recycle Bin; only a folder whose descriptor
does not read is left as it was. Folders that need another one's names are
tried again after it. The record section `converted` keeps `keys` (old key to
salt), `builds` (every old build hash to its salt: the folders', the removed
copies', `.builds\` snapshots' and the retired `aliases`), `folders`,
`pending`, `refused` and `maps` (maps left as they were, by content hash; `check` tries each again in memory and counts it as work as soon as it would convert). It
is written after every folder and every map; a folder's row is `pending` from
just before its swap to just after, so a run stopped anywhere resumes: the
next run puts back a `<name>.old-<pid>` whose folder is missing, removes work
folders, and commits a pending row whose folder carries its salt. The whole run
holds the mods folder's named lock (`src/common/store_lock.h`, DECISIONS 19),
and every run looks at every folder and map again (DECISIONS 21). Maps: every `SNAPMAPS<id>\map.decl` of
`Saved Games\id Software\DOOM\base\savegame.user` (`store_saves.c`; both
renderers share it, `--saves` names another for tests), the data folder's
`rawmap.json` and every prefab under `prefabs\`. A text of an earlier shape
(`sh_map_old_read`; packages a map carried are converted first by
`sh_modlegacy_process`) has each old row resolved by key, else by build hash,
to a root folder's salt and current build, and is written in the current
shape by `sh_map_old_write` (a prefab's block rows rewritten to
`<salt>@<build>` and its names rekeyed). The file is written through a
temporary and a replacing rename, with a copy of it as it was beside it
(`<name>.before-snapmap-plus-1.0`). A saved map is re-framed (its 4-byte header
kept, the text deflated by `store_deflate.c` and read back before anything is
written) and its `map.decl.verify` signed again (HMAC-SHA256 keyed by the
sidecar's own salt and the id the game binds saves to) only after the present
sidecar verifies with a candidate id (the account folder's name, or that id
with its instance field cleared); `game.details` is left as it is. The new
`map.decl.verify` and `map.decl` are written beside them first and then put in
place map first, so a run stopped in between leaves only the new signature
beside the new map, which the next run puts in place. A settings variable an
author's variable follows keeps its index under the name `unused` with an empty
value, its setting moved into the member, so no converted map carries a name
the game's earlier-format check refuses. A map
naming a key no converted folder provides, or whose signature matches no
candidate, is left untouched with its cause. Last, `mods\.builds\` goes to the
Recycle Bin and the record's retired sections (`cache`, `aliases`, `snapshots`,
`renamed`, `downloads`) are removed.

Crash leftovers. An install, a download and a salting assemble a
folder as `<name>.staging-<pid>` and move the folder they replace aside as
`<name>.old-<pid>` before the swap; the listing and the serving scan skip both.
At boot, before the serving index is read, `sh_ms_recover_work` walks the store
(organizing folders and places included): a work folder whose process is
still running is left; a `.old-<pid>` folder whose target is missing is the
folder a session took away when it ended between the two renames, and is put
back ("put <folder> back from <work folder>: an earlier session ended while it
was being replaced"); every other one is removed ("removed <work folder>, a work
folder an earlier session left when it ended mid-install").

The prefabs folder watcher. The Prefabs list follows the disk. The window's
host (`snapmap_plus_ui_webview.cpp`, `folder_watch.h`) runs one thread that
watches `%LOCALAPPDATA%\snapmap-plus\prefabs\` with `ReadDirectoryChangesW`
(subtree; file and folder names, last write, size). A change refreshes the
Prefabs list 300 ms after the last change of a burst (the page keeps its
selection while that prefab exists and clears Prefab Details otherwise), and
logs one UI-log line (`watch: ...`). A folder that does not exist yet is opened
again every five seconds.

The mods folder is not watched, and nothing scans it in the background. The
store is scanned at boot, on Refresh (the Mods tab's button, `sh_mods rescan`),
and at the end of an install, a download and New mod; an author's edit in a
folder shows after the next Refresh.

The rebuild rule: a root folder is built again when its tree hash (the author's bytes of `mod.json`
and the path, size and time of every file under `assets\`) or the builder's rules differ from
its block's, or its last build failed; that runs on a rescan (the Mods tab, `sh_mods rescan`,
an install) and at boot (`sh_ms_boot_sync`): after the boot index, a thread of
its own checks the trees and, when a folder is not built yet or changed while
the game was closed (the first boot after a conversion), builds it as a rescan
would, with the install's indexes (`modstock.c`) as the stock reader, then
rebuilds the serving index, whose listeners pass the table to the game. A first
build of the 15 converted folders takes about 10 s, so it never runs on the
boot thread; the same folders built this way carry the same bundle hashes as an
in-game rescan's. The thread waits until the startup install has released the
store (`sh_ms_boot_release`, at the end of the install in `dllmain.c`): the
install takes the store itself (the publish launch reads the upload queue and
the claims), and a build holding it delayed the startup by the build's length.
While it builds, the thread hands the store to any operation waiting for it
between two folders, so an editor map open in that time waits one folder's
build at most; only the index rebuild at its end touches serving.
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
its folder and still resolves maps published by the earliest releases (aliases map
hashes, not trees); the folder names in it follow the drops and renames. A
pass of the overrides migration writes the same collapsed section when it
completes, and the next run counts an `overrides\` entry the section
does not list as done unless it was created or written after `migratedAt`.
The product block's `legacy` record (tree fingerprint, grandfathered shadows)
is kept as it is. One `MODS: store:` line per dropped or renamed folder and a
summary.

A bundle file may be a stock pointer instead of carried bytes: the
file kind `stock`, `{"needs", "path", "provides", "request", "stock":
{"sha256", "source"}}` with no `blob` and no `size`, where `source` is an
installed resource path and `sha256` the hash of the installed bytes
(`modbundle.h` has the grammar). The builder makes one of every file whose
bytes equal installed content, and an author writes one in the descriptor's
`stock` object (`{"<request>": {"source": "<installed path>", "sha256":
"<optional>"}}`, the hash filled in by the builder) to alias installed content
under a mod name. The extension whitelist does not apply to pointers, because nothing is carried
for them; an upload never sends their bytes, so a converted package or a
copy-of-stock mod uploads only its authored and edited files. An install keeps
every file of the folder it installs, a copy of a game file included (the
builder decides what is a borrow; nothing is dropped at install), and the
folder's own copy answers when the player's game lacks those bytes. `modstock.c` answers them from the player's own
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
file's, at the file's own path, then at that path with its `mods/<salt>/`
segment cut (either spelling of the key; where a copy moved under a key came
from), each time every row of the name in both archives (they hold different
bytes under some names, and the one whose bytes match is named), then entries
of the same size and extension elsewhere (a renamed copy, in size, extension,
archive and name order until one matches; no count or byte budget: each
slice is hashed at most once per process, so the game's own rows bound the
work); measured on the
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
gameplay decls when a map that names their bundle loads. The serving index
holds one build per salt per area (ONE-SHAPE rule 4, `ms_one_per_area`): the
root of `mods\` (everything but `mods\maps\`) and each map's place,
`mods\maps\<segment>\`. Of two folders of one salt in one area the one built
first keeps it (`built`, then folder path); the other is skipped ("it is a copy
of mods\<folder> (the same salt <salt>); Refresh in the Mods tab makes it a new
mod" at the root, SH_MB_SHAPE "another folder of this map has the same mod:
<folder>" in a place) until the store's next scan settles it. The root build
and a place build of one salt are never a conflict: each map serves the build
it declares and the other is inert for it; with no map open only the root
folder's build serves its names for the Toybox (`ms_preferred`, `root_build`),
and a place build serves only for a map that declares it. The index build logs
one `MODS: mod <salt>: builds <hash16> (<folder>) and <hash16> (<folder>) both
provide paths with other bytes; ...` line.

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
shadow, a new one lives under `generated/swf/mods/<salt>/` with its atlas
`generated/swf/mods/<salt>/<stem>.bimage` beside it. Each served movie logs
`MODS: swf: served <request> (<bundle>) at load state N` (the first 32).

Display text reaches the engine's `#str_` table through `strids.c`, which reads
two layers and keeps the first writer of each key (case-insensitive): the
`strings.en` object of every admitted bundle's author block (root folders
before maps' places, then by salt, then folder, through
`sh_modserve_each_bundle_extra`), then the baked defaults. Keys omit the
`#str_` prefix decls use (a mod's own keys are `<salt>_<name>`, which the
builder enforces), and only `en` is read. A mod's own keys come only from a
build that serves its names for the open map (`sh_modserve_bundle_scope`
`names_live`), so the root's build and a downloaded map's build of one salt
never compete: the map shows its own build's text. A string edit (a key the game or the
product defines, `strkeys.h`) applies only where its mod's edits apply
(`sh_modserve_bundle_edits_live`: map scope off, the open map declares the
build, or it is the editor-tweak class), so an editor-tweaks mod's string edits
apply in this machine's editor and a map-declared mod's in the maps that use it;
`sh_strids_scope_check` marks a refresh when either changes for a bundle with
strings (each editor pass and map activation). When two bundles give one key
different text the first stands and one `STRINGS: REFUSED` line names both;
identical text is not a conflict. The pass summary reads `N #str_ entries
(bundles B, baked D)`. A runtime pass (a rescan, an install, an editor or
map pass) runs on the main thread inside the game's process heap: an append
that grows the table allocates its new array from the current heap (the
list's resize asks for heap -1, VK 0x140280ED0), and the editor's or a map's
own heap is freed with it, after which the game's string lookups (VK
0x140281360) read freed memory. A thread where the process heap cannot be
selected runs no pass and leaves the refresh pending.
The engine rebuilds its table when it loads a language (as the editor or a
map starts) and sorts it through the hooked sort; that drops the injected
rows, so every later top-level sort the engine makes re-adds them (in the
process heap) before it orders the table, and decls parsed and Toybox rows
read after a rebuild find a mod's names. The player's own document of earlier releases,
`%LOCALAPPDATA%\snapmap-plus\strings\strids.json`, is not read: the store
tool's loose step moves its keys into the mod My overrides (see [Migration of
the retired overrides folder](#migration-of-the-retired-overrides-folder)), so
they are ordinary bundle strings and outrank no other mod. The first injection runs after the boot index
is built, so it already carries bundle strings. Every later index build
(`sh_mods rescan`, an install, a migration, a fetched bundle) requests a
refresh that re-reads the layers and re-sorts the table on the main thread: at
once when the build ran there, otherwise through the apply call slot or at the
next editor palette build or map activation. A pass first gathers every row
and plans the whole change (update a row it owns in place, adopt an existing
game row keeping its original record, append a new one, retire one no layer
supplies any more, restoring an adopted row's record) with all its memory;
only then does it touch the table, so a pass that cannot complete leaves the
previous table whole and is logged `STRINGS: runtime refresh FAILED`. The
engine rebuilds its dictionary on its own (a language reload), which drops
every row the injector added: a row it owns that the table no longer holds is
added again by the next pass (`STRINGS: the game rebuilt its string table`)
instead of failing it.

### Mod sounds

A sound decl a mod carries (`generated/decls/sound/mods/<salt>/<stem>.decl`)
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
when `sh_mods install` rebuilds the index. The bank list is taken again at
every main-thread pass from the serving index in the open map's context
(each served bank's file through `sh_modserve_lookup`), so a map that uses
another build of a mod than the root's gets that build's banks at its map
pass; a list equal to the current one is not published again. The engine prints its own result,

`async load success id <id> : <name>` with the id as a signed number. The
LoadBanks slot (`AudioLoadBanks`, hardware vtable `+0x30`) resubmits every
bank after `s_loadBanks` and `s_restart`, and runs them in process heap 0:
Wwise allocates through the engine allocator's current heap, so a rerun from
the console with a map open put the reloaded stream packages in the map heap,
the map unload freed them, and the next streamed sound read a freed package
(a crash at the Play load). A mod installed mid-session plays at
its next map: its banks go through the same per-bank wrapper at the next
main-thread pass, never through `s_loadBanks` with a map open. A bank whose
bytes change under a name already loaded keeps the loaded copy (Wwise keeps the
first bytes for a bank id and answers a second load with 69,
AK_BankAlreadyLoaded) until the game restarts, and a removed bank stays
loaded until then. A bank is named by its own bytes: the serving index
serves every carried bank as `sound/soundbanks/pc/<stem>.<sha16>.bnk`,
`<sha16>` the first 16 hex characters of the bank file's SHA-256, with its
BKHD bank id rewritten to that stem's FNV-1, and rewrites every quoted
`"<stem>"`, `"<stem>.bnk"` or `".../<stem>.bnk"` in the bundle's served decl
texts to match; the rewritten bytes are written once under `mods\.served\`
(a dot folder the scanners skip), keyed by their own hash
(`banks\<stem>.<sha16>.bnk`, `files\<sha16>.<extension>`; no folder is named
by a build), and served from there, with their own size and hash. Each
rebuild of the index removes the files there that no build serves any more,
unless another game instance serves the same mods folder: each instance holds
the mark `Local\snapmap-plus-served-<16 hex>`, named for the mods folder as its
store lock is (`store_lock.h`), so a game on another folder, or a test on its
own, never holds the cleanup back. A changed
bank is therefore a new bank to Wwise, two builds that carry one bank's bytes
(the root and a map's place) share it and Wwise loads it once, and the ledger
and the refresh see a new name. `sh_mods status` lists each bank and whether it was
submitted. The events file (`soundbanksinfo.events`) is not extended: a mod
event needs no record to play (it only feeds the by-id lookup; without one
the event is never distance-virtualized).

A mod's sound decl at a stock event's name borrows that event, which plays
only while a bank filing it is loaded. SnapMap's listed-bank filter skips
every bank whose name holds `_sp`; on build 13954591 that is
`doom_weapon_sp.bnk`, the only bank filing 4 of the game's 7,649 events (the
plasma fly-by and whiz-by among them). `modbankinfo.c` reads the game's
`base\sound\soundbanks\pc\soundbanksinfo.xml` once, on the first question,
and answers which skipped bank a sound decl's event needs (its name, else
`play_` plus its last path component). The builder lists those banks as the
product block's registries `"banks"` (the game's builds and the store
tool's conversion alike), each checked against the game's index. At
the map pass `modsound.c` loads each bank a declared bundle needs, once per
process: on the main thread inside process heap 0, through the call the
per-bank wrapper makes once its filter passes (`operator new` and the cookie
ctor, then the async `LoadBank` with the product's callback, which records
the result and hands the cookie to the engine's own), waiting up to 3 s for
the callback so the events exist before the level loads. It never goes
through the wrapper, never through `s_loadBanks`, and never changes the game
mode. The bank stays loaded until the game exits (about its file size, 85 MB
for `doom_weapon_sp.bnk`). `sh_mods status`, `sh_mods doctor` and the Mods
tab's facts show each needed bank and whether it loaded.

### Render programs

A mod can ship a new render program `mods/<salt>/<name>` (no `.` in the last
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
registered. A material naming a campaign-only program brings the program's
files along as pointers (the closure under [Mods](#mods)), checked the same
way from the installed bytes. A bare program name in a material, shared includes under
`decls/renderprogs/`, `vulkan_pipelines.bin`, `generated/glslBinaries/` and
`generated/glsl/` are refused. MD5fold is MD5 with its four words XORed
together. OpenGL compiles the decl text by default (`r_useGeneratedRenderprogs`
0) and may write the permutation decl into the player's `base\generated\`.
The first open of each served render-program file logs
`MODS: renderprog: served <request> (<mod name>, <n> bytes)` (the first 64),
which shows the variant this GPU binds.
### Owner-bound companions

A bundle's `companions` key (author block of `mod.json`, kept under `extra`)
maps owner entityDefs, which must be the bundle's own `mods/<salt>/` names, to
1..16 companions `{entityDef, origin, axis, activate, bind}` (`companion.h`
has the grammar and refusals; a refused block adds nothing and is logged
`COMPANIONS: <id> REFUSED: ...`). Each admitted build that declares companions
has a table of its own, and an owner's companions come from the build that
serves the owner's entityDef for the open map (`sh_modserve_lookup`), so a map
that brought its own copy of a salt spawns that copy's companions, never the
root folder's. A new serving index is read at the next outermost spawn of a
mods entity, never inside a companion's own spawn, whose owner's loop is still
reading the tables. `companion_engine.c` detours `idClass::CallSpawn` (VK 0x141583A00, GL
0x141584A90, 14 stolen bytes) only once an admitted bundle declares an owner:
at startup when one does, otherwise at the first main-thread gate after one is
installed (`sh_companions_ensure`, called beside the probe's own lazy commit at
the Play build `sh_modserve_play_begin`, the map load
`modserve_activate_bundles`, the editor palette pass, `sh_mods rescan` and the
return to the editor). CallSpawn runs on DOOM's main thread only, with no
inlined copy (reports ADD-G2, and ADD-V1 live: 2,132 Vulkan and 2,130 OpenGL
entries, all on the main thread), and the editor's map load spawns nothing
while Play precedes every spawn burst, so a commit from the main thread at one
of those gates is race-free and comes before the spawns it is for. The gate
checks, in order (`sh_companion_arm_decide`): the boot resolution succeeded
(it is cached; nothing is resolved later), the detour is not live yet, the
caller is DOOM's main thread (`main_thread_id`; never armed from another
thread), the serving index was published again since the table was built and
the rebuilt table has an owner, and CallSpawn's 14 bytes are still the
expected ones (another detour there refuses). Once committed the detour stays
for the session; it is never unhooked mid-session, since a main-thread frame
inside GetType returns into the trampoline. It logs `COMPANIONS: installed ...
(<gate>)` and one line per owner
spawn (`<owner>: n of m companions spawned`), the first 64 lines. After the original spawn chain
returns, an idEntity whose entityDef (`+0x6D0`) is an owner, or inherits one
through `mods/` names (`+0x58`), gets each companion in list order: spawned
the way `idTarget_Spawn` spawns at a pose (game vtable `+0x448` creates it,
its `spawnPosition` `+0x3A8` and `spawnOrientation` `+0x3B4` get the owner's
physics origin plus `origin * axis` and the axis `axis * ownerAxis`, and
`+0x458` registers it and runs CallSpawn), so the class's own Spawn places it.
Placing it after the spawn is not enough: SpawnEntityDef (`+0x450`) runs
CallSpawn before it returns, entity SetOrigin and SetAxis write the physics
only, and an idProp_Coop stays where its Spawn presented it. When the create
and finish calls do not resolve, the companion falls back to SpawnEntityDef,
SetOrigin, SetAxis and UpdateVisuals (`+0x3C0`, as the engine's `setOrigin`
event pairs them); the per-owner line counts both routes. Each companion is
then bound (`idEntity::Bind`, oriented) so it moves
with the owner and the owner's destructor removes it, and appended to the
owner's targets (`idEntity::AddTarget`) so a native `ActivateTargets` (Use
with `activateTargetsOnUse`) activates it after the owner's earlier targets.
SpawnEntityDef constructs the class the decl parsed (`CreateManagedClassFromDecl`
reads decl `+0x60` through decl vtable `+0xB0`), so a companion whose resource
state (`+0x2C`) marks a failed load or no load yet is skipped and logged rather
than spawned as the bare idEntity a defaulted decl would give; the per-owner
line names the class the first companion spawned as.
Map entities reach CallSpawn after `SpawnEntitiesForLayers` has registered all
of them, so companions number after the map's own. A companion's own
companions are spawned once more, no deeper. Nothing is spawned when the game
is a network client. Access violations inside the pass are contained and
counted; an engine exception propagates to the engine. The signatures are
private to `companion_sigs.c` and checked on both executables by
`companion_test`. The capability name `snapmap.owner_companions` is reserved
for the engine-capability facility once it carries hook rows.

### Mod scripts

A mod may carry Lua scripts: `assets/mods/<salt>/scripts/*.lua` (entry files)
and `scripts/lib/*.lua` (loaded only by `require`). They run during Play, on
the host only, one sandboxed state per mod per map, and only inside one tick
on the game thread: every engine hook posts a record and the tick delivers it.
The code is `src/backend/script/` on the vendored Lua 5.4.9 core
(`src/third_party/lua`, its README records the version, hashes and the one
local change). This chapter has six parts: this one (the build, the runtime
and the sandbox), the engine side (gates, tick, spawns, queues, console), the
entity, animation and world calls, the demons a script drives, the combat
calls with the events of damage, death, pain and animation, and the Script
node with map variables. Every script detour and engine binding is resolved
at boot from its signature on the running executable (both renderers'
signatures are tested on both images) and is a row of the feature registry
(`feature_health.h`): "mod scripts" (the tick; off when mods are not served),
and beside it "entities spawned during Play reaching scripts" (the companions'
CallSpawn), "damage, death, pain, animation and projectile events for
scripts" (the six event detours), "script combat calls", "Script nodes" (the
Relay hook) and "demons driven by scripts" (the IsRunningScript park), each
off with its reason when what it needs is not the expected code (and the
last five off when mod scripts are). A failed install never stops another
feature.

- **In the build.** A script is a carried file of kind `script` (provides type
  `script`): `assets/mods/<salt>/scripts/<name>.lua` or
  `scripts/lib/<name>.lua`, `<name>` of `[A-Za-z0-9_-]`; a `.lua` anywhere else
  is refused. It is hashed into the build like every file and the engine never
  opens it. `modbundle.c` checks each one: UTF-8 text without a NUL byte, not
  compiled Lua (no count or size: a mod carries as many scripts as it has, of
  any size); then the compile check the product
  registers (`sh_mb_set_script_check`, `script/script_compile.c`: parse only,
  nothing runs; an entry file must start with `api = 1`). A failure is the
  row's cause, worded as the stop line words it:
  `scripts\door.lua line 4: "end" expected to close "function" on line 2.
  Correct it, then press Refresh.` A build without a registered check (any
  program but the game and the store tool) refuses a script. The static pass
  (`sh_mb_script_scan`) reads the literal registration calls without running
  anything: a `require` of a module the mod does not carry, or a name under
  another mod's `mods/<salt>/`, is refused with the file and line; an
  `on.<event>` name with no entityDef of the mod is a warning. The builder
  block records what was found for the Mods tab (`"scripts": {"files",
  "entities": [{"name", "events"}], "inputs", "outputs", "node", "dynamic"}`,
  `node` true when a script registers an input, so the mod gets its Script node
  row, `dynamic` counting names set while running); the author part declares
  nothing. An author file at one of the Script node row's two paths is refused
  (`SH_MB_SCRIPT_NODE_RESERVED`). Builder rules 11.
  A build that carries a script is never sliced (`sh_mb_sliceable`): its
  scripts may build names while running. First-scan salting rewrites
  `mods/<folder>/` inside a `.lua` like any text file. Archives (publish,
  download, rawmap and prefab exports and imports, installs) carry scripts as
  the files they are, under their lower-case game names (every carried path is
  recorded that way, whatever its spelling on disk, and a new folder's install
  writes its `assets\` tree in lower case, so an uppercase letter never refuses
  a mod).
- **States.** The Play gate prepares the map's scripted mods
  (`sh_script_map_open`: the provider's mods, their file source and the map
  seed; each mod is listed and registered with the hooks, nothing runs). The
  first frame opens one `lua_State` per prepared mod; `sh_script_map_close`
  closes them. Nothing crosses states. The file source (`sh_script_source`)
  lists the mod's scripts and answers reads from the copy the gate took
  through the serving index (below). A mod with no entry file gets no state.
- **Loading.** Entry files run once at map start in byte-wise name order, then
  the `on.map_start` handlers. Each entry file must start with `api = 1`
  (comments and a byte order mark may precede it); a missing or newer version
  is refused before the file runs. Chunks load with mode `"t"`: compiled Lua
  is refused. A mod's scripts have no count or size limit.
- **Sandbox.** Base, string, table, math, utf8 and coroutine only; no `io`,
  `os`, `debug`, `package`, `load`, `loadfile`, `dofile`, `string.dump` or
  `math.randomseed` (`math.random` is seeded from the map seed);
  `collectgarbage` answers only `"count"`; `print` and `warn` write the mod's
  script log; `setmetatable` refuses a `__gc` field (Lua runs finalizers with
  hooks off). `require(name)` loads `scripts/lib/<name>.lua` of the same mod
  once. The registration table `on` and the runtime's own objects cannot be
  edited.
- **Bounds.** The product imposes none of its own (DECISIONS 26): event texts
  (an `ae_notify` row, a damage decl, a Script node's name) and the stop line
  are kept whole, of any length; entityDef and decl names in calls and
  bindings have any length (the script layer sets none; no fixed bound in the
  engine's decl-name handling is established, and a mod's own names are paths
  of its build, which the builder's request rule bounds); a mod's display name
  is whatever its map row holds (320 bytes, the builder refuses longer); a
  state's
  memory, the work of a call or a frame, timers, log lines, the size of a
  text a pattern searches, entities spawned, projectiles launched, remembered
  or steered, and every table of the script layer grow as far as the process
  allows. One protection is not a limit: a call (one handler, one timer, one
  coroutine resume) still running after 5 seconds of wall time is stopped, so
  a script that never returns cannot hang the game. The clock is read by the
  count hook every 1,000 VM instructions, every 1,024 pattern steps
  (`l_matchstep` in `lstrlib.c`, since one pattern can run inside one
  instruction), and before sorting, moving or shifting table elements and
  scanning UTF-8 (work that never reaches the hook). `pcall`, `xpcall`,
  `coroutine.resume` and `coroutine.close` cannot catch a stop, and an
  `xpcall` handler is not run after one (Lua calls it from inside the hook,
  with hooks off). What remains is the engine's: the game holds at most
  16,382 entities at once and one more is a fatal engine error, so `spawn`,
  `fx(effect, pos)`, `explode` with an effect and `ai:fire` refuse ("cannot
  add an entity now: the game holds at most 16382 entities at once and nearly
  all are in use; remove some first") when fewer than 256 slots are free (the
  free count is read once a frame; each script spawn or launch takes one);
  the per-demon tables the engine's own threads read without a lock (the
  demons parked, their pain strings and blocks) have that many slots and
  reuse a gone demon's. A projectile the engine launches (weapon fire, an md6
  row) lives as long as the engine keeps it. The handle's 18-bit slot number
  is the format's range (262,143 records at once), far above what the engine
  can hold. Running out of memory stops the mod ("ran out of memory").
- **Errors.** Every call into Lua is a protected call of a C job, so a failing
  allocation while arguments are built is an ordinary error and Lua's panic is
  never reached. Any error stops that mod's scripts for the rest of the map: its
  state is closed, its bindings withdrawn, its queue refused, and one line is
  written: `Script stopped: <mod>, scripts\<file>.lua line <n>: <what>. <what
  to do>.` `script_reason.c` rewrites Lua's wording by one table (nothing says
  "attempt to index a nil value"; an author's own `error("...")` text is kept).
  The mod's log (prints, warnings, the stop line; every line of the map,
  whole; the Mods tab shows the newest 1,000 and Copy log copies all) stays
  readable after the map until the mod's next map start:
  `sh_script_log_read`, `sh_script_stop_line`, `sh_script_status`,
  `sh_script_mods`.
- **In the window.** The Mods tab reads these through `modui.c`'s engine
  binding (`script_info`, `script_log`, `script_mods`, defaulting to
  `sh_script_mod_info_get`, `sh_script_log_read` and `sh_script_mods`): each
  row carries the block's `scripts` member and, once a Play ran them,
  `scriptRun {status, stop, lines}`; the read-only `SH_MODS_SCRIPTS` op
  (`sh_modui_scripts`) answers every mod's state and one mod's log on any
  thread as its own owned text, never the tab's shared document, and the
  page asks for it each second while the tab shows a mod with scripts. A
  script build error is the row's cause as written, and the Files view matches
  it to the file by the author's spelling (`sh_mb_script_file_name`). The
  Entities tab names a Script node's script and offers its mod's input and
  output names from the same member. The player's setting `mods.scripted_maps`
  (`sh_modui_scripted_maps_allowed`) and the download approval's sentence
  (`sh_modui_scripts_approval`) serve the download gate: the store's index
  records each build's script count (checked against the archive's `.lua`
  entries and the block's `scripts.files`) and `HEAD` answers it, so before
  the one question the gate knows. The question then ends "These mods include
  scripts (N files). Scripts run only inside this map and cannot read or
  change your files. Download and install now?"; with the setting off the map
  is not downloaded or played and the player reads `SH_MODUI_SCRIPTS_OFF`.
  The same check runs at every launch the gate decides (`pub_refuse_scripts`):
  a map with a place whose serving builds carry scripts (the `served_scripts`
  op reads each serving folder's `mod.json`, only while the setting is off)
  is refused with the same sentence though it was downloaded while scripts
  were on; the player's own maps have no place and are never refused. A
  rawmap or prefab ZIP import whose carried mods have scripts is refused with
  the same sentence while the setting is off and installs nothing
  (`modexport.c` me_install), and a rawmap that came with its mods (it records
  a place) is refused at every open while the setting is off when the builds
  serving it there carry scripts (`sh_modpub_rawmap_scripts_off`, asked by
  `sh_rawmap_prepare_reload` before the engine sees the text).
- **Registration.** `on.spawn / think / remove / damage / death (name, fn)`,
  `on.anim(name, notify, fn)`, `on.input(node name, fn)`, `on.map_start /
  map_end(fn)`. Names are mod-relative (`"ai/tyrant"` is
  `mods/<salt>/ai/tyrant`); `mods/<other salt>/...` and `game(...)` names are
  refused (`sh_script_check_name` applies the same rule to every API call).
  Names, notify strings and node names compare without case.
- **Queue and tick.** Hooks on any thread post records to the mod's queue
  and the spawn subscriber records entities in the handle table (the engine
  side, below). `sh_script_binds` answers which running mod binds a full
  entityDef name and `sh_script_entity_owner` which running mod an entity
  belongs to, both from any thread; ownership is the handle table's (placed
  from the mod's entityDefs or spawned by its scripts). Lua runs only inside
  `sh_script_tick` (and map start and end), one caller at a time: the handle
  table's spawns (an entity whose entityDef chain the mod binds gets
  `spawn(self)`) and removals (`remove(self)` once; its object answers
  nothing), then per mod its queued records, `think(self, dt)` for every
  driven entity, due timers and coroutines, then what those raised, up to 4
  queue passes a frame. A map end requested while Lua runs waits until it
  returns.
- **Time.** `now()`, `after(s, fn)`, `every(s, fn)`, `run(fn)` with `wait(s)`
  inside it, each returning a handle with `cancel()`; game time (the game
  clock, so paused time does not count), per mod, dropped at map end. A
  coroutine resume is one call.
- **API seam.** Other modules add globals and entity methods to every new
  state with `sh_script_api_add` (one implementation per name), whole objects
  with `sh_script_api_add_opener` (a function run in each new state's
  protected open: `vars` is opened this way), push entity objects with
  `sh_script_push_entity` (one object per handle per map, with its own
  `e.state` table), check them with `sh_script_check_entity` and
  `sh_script_owns_entity`, and refuse a call with `sh_script_refuse` (one
  plain sentence at the calling line) before touching the engine.

Registration also covers `on.pain`, `on.frame_event` and `on.projectile`
(the combat part). Not built in version 1: `e:pain_anim`.

**State-machine classes.** The game runs each demon on state machines read from
`aifsmmanager` decls; every state, transition, condition and snippet of a graph names a
class the engine finds by name in its type registry. A mod supplies the classes the game
lacks from its script: `fsm.condition(name, fn)`, `fsm.snippet(name, fn)`,
`fsm.transition(name, fn)` and `fsm.state(name, { enter, update, exit })`, each called with
the demon and the node's parameter-set number. A condition answers true or false, a
transition the code of the link it fires or false, a state's `enter` or `update` a code for
its machine or nothing. The mod's decl names such a class
`smp.<kind>.<salt>.<class>.<n>`. The type lookup (`TypeInfoByName`, VK 0x141583B20 / GL
0x141584BB0, `script_fsm_sigs.c`) is detoured at boot: that name answers a record copied
from a stock class of the same kind, whose create and clone build the stock object and give
it a copy of the stock vtable in which the kind's one virtual calls the script
(`script_fsm.c`; conditions and snippets slot 2, transitions slot 12, states slots 25, 26
and 28). The engine does not read the `object` block of a class it has no reflection
record for, so a node's parameters are the script's own data, chosen by `<n>`. Scripts run
on the game thread only: a class asked from another thread answers no, and one log line
says so. `tests/script_fsm_test.c` covers the bridge against an engine made in the test,
`tests/script_sandbox_test.c` the registration and the call, and
`tests/script_combat_sigs_test.c` the lookup's signature and detour window on both
executables.
`tests/script_sandbox_test.c` covers the runtime without the game;
`tests/modscript_test.c` the builder's checks, the static pass, the serving
index (scripts and the Script node row), the archives and the fixture mods.

### Mod scripts: the engine side

`src/backend/script/script_hooks.c` connects mod scripts to the engine; the
runtime that runs them is `script_runtime.c`, behind three calls:
`sh_script_map_open` at the Play gate, `sh_script_tick(now, dt)` once per host
game frame of that Play, `sh_script_map_close(run_end_handlers)` when it ends.
No script code runs anywhere else.

- **Gates.** The serving layer calls `sh_script_hooks_gate` directly, on the
  main thread, before it arms the companions detour. The map-load gate
  (`modserve_activate_bundles`: the editor's load, a launch at the hub) ends
  the last map's scripts without end handlers and opens nothing, so nothing
  runs in the editor. The Play gate (`sh_modserve_play_begin`, from the
  SnapBuild detour, after the Play's map scope is set) ends them too and
  prepares this Play's scripted mods: the editor's Play, a My Maps play, a
  published map from the browser or the feed, and a lobby's host all pass it.
  A Play that prepares a mod arms the CallSpawn chain in the same pass
  (`sh_companions_ensure`), before the level's spawns.
- **Where the scripts come from.** `script_source.c`: the builds the Play's
  map scope declares (`sh_modserve_scope_hashes`: the builds serving the
  map's rows, then the placed names' closure), and for each mod the scripts
  the serving index serves for the open map (`sh_modserve_script_files`: a
  stranger's map's own place, the author's root folder). At the gate every
  script is read once, as the mod's folder holds it at that moment
  (`sh_modserve_open_disk`, never the index's cache of the bytes first read),
  and kept only when its bytes hash to the SHA-256 its build recorded; one
  edited since the last Refresh is not kept (`sh_script_source_stale` names
  it), reads as "could not be read" and stops its mod with that line. Lists, reads
  and `require` are then answered from that copy, so a Play runs the built
  scripts it started with and nothing reads a mod folder while it runs. A
  Refresh that changes scripts reports them as running from the next Play
  (`changed_scripts` in the serving diff), never as needing a restart.
- **Tick.** A 15-byte detour on the SuperScript per-frame wrapper (VK
  0x140E9C9D0, GL 0x140E9BF10). Its only caller is `idGameLocal::RunFrame`
  (game vtable `+0x08`), after the entity think loop; RunFrame is reached from
  `idCommonLocal::Frame` on DOOM's main thread, through the host's game-frame
  loop and the two client frame loops, so the tick also runs on clients. It
  runs the scripts only when a Play opened mods, on the main thread, not
  re-entrantly, on the host (`idGameLocal::IsClient`), while no loading screen
  draws (`load_screen` thread byte `+0x4B2`) and while the editor is inactive
  (`idSnapEditorLocal +0x09`, clear during every SnapMap Play). The editor's
  deactivate reason is not used: it is 3 only for the editor's own Play and
  stays 0 for a published or My Maps play, so `sh_modserve_engine_in_play`
  would keep scripts off there. An unreadable state runs nothing
  (`sh_script_tick_decide`). The frame time is the game clock's (idGameLocal
  vtable `+0x220`, milliseconds) since the last script frame, so a paused game
  passes no script time; before the tick, `sh_script_api_frame` observes the
  animation requests of the entity calls. The detour is committed at the first
  Play gate that prepares a mod, on the main thread with its prologue
  verified, and is never unhooked. A failed install records "mod scripts" off
  with its reason, and `sh_scripts` gives that refusal.
- **Map start and end.** Before the first frame the tick walks the live
  entities once (every entity number's spawn id through the game's lookup)
  and records every entity of a scripted mod the spawn chain did not; the
  runtime then runs the files and `map_start`. The scripts end at the next
  gate; when the editor is active again (the return from the editor's Play:
  end handlers run); when a loading screen draws after they started (the
  session left the map: no end handlers, since a handle could name the next
  map's entity); at CancelMapChange (`load_screen_stop.c` calls a map-end
  subscriber: end handlers run unless a loading screen was still drawing); and
  on a client (none run there). A map end during the tick waits for the tick
  to return.
- **Spawns and handles.** The CallSpawn detour calls a spawn subscriber after
  the companions, with the entityDef's name and its `mods/` inherits. An
  entity whose names reach `mods/<salt>/` of a prepared mod is recorded with
  that chain by its engine spawn id (idGameLocal `+0x653F8` + 4 x idEntity
  `+0x08`) in `script_handles.c`. A script holds a handle (the record's index
  and the map's epoch), never a pointer: every use resolves the spawn id
  through game vtable `+0x7F8` (the `+0x7F0` lookup and the idEntity cast),
  which fails once the entity is gone. Each frame the runtime sweeps the
  table: a new entity whose chain the mod binds gets `spawn(self)` and is
  driven from then on, a vanished one gets `remove(self)` once. An entity no
  mod owns (a player, an attacker) gets a foreign handle
  (`sh_script_handles_ref`) that resolves the same way. A record lives exactly
  as long as its entity: the sweep releases a removed or gone entity's record
  and tells the other tables (`sh_script_handles_on_gone`: the combat records,
  each state's entity object), and the slot is reused under a new generation,
  so a handle a script keeps resolves to nothing, never to the slot's next
  entity. The table grows with the entities alive; it has no count of its own.
- **Queues.** Hooks on any thread (damage, death, pain, animation notify,
  projectile launch, Script node input) post fixed-size records with copied
  strings and a vector into the owning mod's queue (`script_queue.c`, a spin lock per mod, growing with what
  arrives in a frame; its strings copied whole, of any length, in one block a drained record owns until
  `sh_script_event_release`). Only the tick's thread drains; only a record the process has no
  memory for is dropped, and the mod's script log then gets one line per map. The queues, the hooks' mod
  table and the runtime's mods grow with the map's scripted mods.
- **Console.** `sh_scripts` (`script_console.c`) prints each mod's state from
  `sh_script_mod_info_get`, `sh_scripts log [mod]` its script log, and
  `sh_scripts reload` either ends and prepares the open Play's mods again
  (they start at the next frame) or, outside Play, compiles every script of
  the open map's mods (`sh_script_check_files`) and prints each error in the
  stop line's words. A mod with a script file that no longer matches its build
  is not started; the reload and the status say so in one sentence naming the
  file ("<mod>: scripts\door.lua no longer matches the mod's last build, so its
  scripts will not start; run 'sh_mods rescan' (or press Refresh in the Mods
  tab) to rebuild it, then play the map again."), and the reload counts only
  the mods that can start. DOOM also rebuilds a changed mod when it starts.

The signatures are private to `script_sigs.c` (the tick, the spawn-id lookup,
IsClient, and the OnDamage, Killed and Relay DoAction sites for the event
hooks; OnAnimNotify shares its bytes with OnMoverNotify and is resolved by the
callback name it loads). `script_sigs_test` checks all of them, their detour
windows, the tick's single caller and the resolver's vtable slots on both
executables; `script_hooks_test` covers the queues, the handle table, the mod
table and the tick's guards; `script_e2e_test` runs fixture mod folders from
disk (`tests/fixtures/scripts/`) through the source, the runtime and the
console with the hooks' part played as the hooks play it, then the door
example through its Script nodes, the spin-heal example through the entity
calls and the brute example through the AI and combat calls, each against a
faked engine table.

### Mod scripts: entity, animation and world calls

`script_api_entity.c` and `script_api_world.c` add the version 1 calls;
`script_api_common.{c,h}` holds what every API module shares. A script is the
map's own logic, as wired logic is, so every call works on any entity of the
map: the mod's own (placed from its entityDefs or spawned by its scripts, which
the handle table records under the mod and its `on.spawn` and other entity
handlers hear), the game's and another mod's. A name is the mod's own content;
`game("...")` names the game's wherever a name is taken (`spawn`, the damage,
projectile and sound decls, `decl`). A removed entity answers nil.
Every argument is checked before the engine is touched, and every refusal is
one plain sentence at the calling script line (it stops the mod's scripts for
the map, as any error does).

- **Calls.** `spawn(def, pos, [yaw])` (own entityDefs only; no count or rate
  of its own, only the engine's entity table, Bounds above), `players()`; `e:pos()`, `e:set_pos(v)`, `e:yaw()`,
  `e:set_yaw(deg)`, `e:axis()` (forward, left, up), `e:velocity()`,
  `e:set_velocity(v)`, `e:health()`, `e:set_health(n)`, `e:alive()`,
  `e:def()` (the mod's own names as the script writes them), `e:distance(other)`,
  `e:is_player()`, `e:remove()`, `e:view_angle_to(other, [use_view_pos])`;
  `e:anim_state(node, [via])`, `e:anim_force(node, [blend])`,
  `e:anim_finished()`, `e:anim_current()`, `e:anim_event(name)`;
  `entities_near(pos, radius)` (every entity in range, nearest first),
  `trace(from, to, [radius], [ignore])`, `nav_ground(pos)` (the floor under a
  point: the trace straight down from 16 inches above it, the first surface
  flat enough to stand on, normal at least 0.7 up; it does not consult the
  navigation areas), `difficulty()` (idGameLocal vtable +0x670, the game's
  own difficulty, 0 easy to 4 nightmare) and `noting()` / `note(text)` (a line
  for a development tool that listens to the diagnostic build, `diag_status.h`;
  no listener exists in a release build, where `noting()` is false and `note`
  does nothing). Positions are tables `{x, y, z}`; an
  entity stands for its position wherever a position is asked for.
- **Engine.** Every call goes through one table of engine operations
  (`sh_script_engine`), filled at boot by `sh_script_api_install` from private
  signatures (`script_api_sigs.c`, checked on both executables by
  `tests/script_api_sigs_test.c`) and faked by `tests/script_api_test.c`.
  Positions and axes are the physics' (`+0x808`, vtable `+0x80`/`+0x88`) and are
  set the way the engine's setOrigin event sets them (SetOrigin, SetAxis,
  UpdateVisuals). Velocity, player health, view angles and removal are the
  engine's own events (`ProcessEventArgs`, or `PostEventArgs` for commands).
  An idAI2's health is its idAIHealth (`+0x2B290`): healing goes through the
  engine's Heal, lowering writes the field and never reaches 0 (death comes
  from damage). Spawning is idTarget_Spawn's create, pose and finish. A trace
  is the game's trace query run at once, with the SnapMap laser hazard's
  contents mask; a radius makes the line thick (nine parallel traces, the
  nearest hit wins).
- **Per frame.** `sh_script_api_frame` also runs the steps other call
  groups register with `sh_script_api_on_frame` (the AI calls finish a forced
  animation through a via node there).
- **Animation.** idAI2 ignores the stock `changeAnimState` events, so on a
  demon `anim_state` and `anim_force` hand the request to the AI calls
  (`sh_script_ai_anim`): the demon is parked (below) and
  `ssaction_ChangeAnimState(Via)` or `ssaction_ForceAnimState` runs at once.
  A bare state name takes the AI's current subweb.
  `anim_current` reads the animWeb's current node (`+0x225C8` web, `+0x2271C`
  node index). `anim_event` (DEST_BLEND_START, DEST_REACHED,
  VIA_BLEND_IN_START, VIA_BLEND_OUT_START, FINISHED) and `anim_finished` are
  that node, observed every frame (`sh_script_api_frame`, before the tick)
  against the last request. Other entities use the stock `changeAnimState`,
  `forceAnimState` and `animState_IsFinished` events.
- **Events.** `sh_script_api_event` (run now) and `sh_script_api_post`
  (queued for this frame's event service) are the one way any script call
  reaches an engine event: the event is found by name in the game's registry
  and its argument count and types are checked against the caller's before it
  is sent; a mismatch is logged (the first 32 lines) and the call does nothing.
  The AI calls use the same helper.

### Mod scripts: the demons a script drives

`src/backend/script/script_api_ai.c` adds the AI calls of API version 1 as
entity methods. Each one works only on a demon (an idAI2 or a subclass, by
type number) the calling mod owns; any other entity, a bad argument or a
missing engine function is refused with one plain sentence before the engine
is touched, and a removed demon answers nil.

- **The park.** The first command a script gives a demon parks it: the
  detour on `IsRunningScript` (VK 0x14042FBC0, GL 0x14042EE60, 15-byte
  window, committed at that first command on the game thread) answers true
  for it, checked by spawn id from the FSM worker threads, so the alert cycle
  enters its own `ExecuteDoomscript` state from `ac_default` and never leaves
  it through `Shared_NotExecutingScript`. That state's class overrides no
  state method; the designer commands below act there, and the engine keeps
  pain, damage, death and animation. The detour answers false, so the
  engine's own cycle runs, while the demon is dead, while a pain reaction of
  the pain graph plays, and for a moment after a pain or death request (below).
  The engine starts a pain reaction and a death by asking the demon's alert
  cycle FSM for a transition (VK 0x14064A560, GL 0x140649870: fsm, code;
  232 pain, 233 light pain, 237 and 238 stagger, 242 dying; the FSM is the
  answer of the object at ai+0x123B8, vtable +0x78). `ExecuteDoomscript` has no
  link for any of them (its links go to `ac_default`, `ac_UnhandledError`,
  `ac_MeleeInteraction` and `ac_passified`), so a parked demon lost every pain
  and, found dead outside the dying cycle by its think, was forced dead with
  no death clip. The request is detoured (18-byte window, committed at the
  first park): refused with one of those codes for a parked demon, the park
  opens for `SH_AI_PAIN_TAKE_MS`, the same request is made with no code, which
  takes the alert cycle out of `ExecuteDoomscript` by its own link, and the
  code is asked again, all inside the engine's call. A per-frame step closes
  the park when the reaction plays or the time is up, and asks again for a
  pain still refused (the code from the engine's own function, VK
  0x1405FBE70). The command also posts
  `setScriptAbort 0 0` and `setScriptAbortFlags 0 0` once, which keep a demon
  in combat at its commanded point. The script proxy (ai+0x2CDE0) forces the
  alert cycle back into `ExecuteDoomscript` (by its type, VK typeinfo
  0x145CEDF30) when a command arrives while the demon is elsewhere. No decl
  is edited and no layers config is named; a demon stays parked for the map.
- **Commands** are catalogue events (`SH_AI_EVENTS`) run on the demon at once
  through the entity calls' event helper, which checks each one's arguments
  against the game's event:
  `move_to(pos, [tolerance], [flags])` (`ssaction_SetWalkState` walking or
  running, then `ssaction_MoveToPoint`; `strafe` or `aim_while_moving` first
  aims at the enemy with `setFocus`), `move_to_entity(e)` (the same to the
  entity's position, given again when it has moved 32 inches; the engine's
  move-to-entity overshoots), `stop()` (`ssaction_Idle`), `face(e)`
  (`setFocus`), `face(pos, [{ travel = inches }])` (a zero-length
  `ssaction_MoveToPoint` with align; its tolerance is the arrival radius of
  the engine's turn-clip query, which for a turn in place rejects a clip whose
  root ends farther than 1.0-1.3 x max(1, radius) from its start, flag
  `ANIMQUERY_REJECT_OVERSHOOT`, VK 0x141AD9E90 / GL 0x141AF40E0; the default
  0.1 picks clips that turn in place, `travel` admits a demon's own stepping
  turns; with `travel` an entity target turns toward where it stands),
  `aim_at(e | nil)`, `set_enemy([e])` (`setEnemy` locked, or
  `setPlayerEnemy` closest), `trigger(on, [hand])`
  (`pull/releaseTriggerLeft` or `Right`), `equip(item)` (the item's decl
  name, as the entityDef's startingInventory names it, or its index;
  `equipInventoryItemByIndex`; answers `"left"` or `"right"`), `laser(tag, on)`
  (`setLaserState`), `laser_state(tag, on)` (`ae_setLaserState`),
  `laser_sweep(name)` (`ae_startLaserSweep`), `focus_tag(tag | nil)`
  (`ae_setOverrideFocusTagName` / `ae_clearOverrideFocusTag`, aim focus).
- **The weapon's hand.** The game fires a demon's weapons from its fire
  control (VK 0x1404456E0), which the AI think (VK 0x1403E0D40) runs only
  while the hand the aibehavior decl names in `weaponSlot` (vtable +0xB20,
  decl +0x134, the right hand unless set) holds a weapon (vtable +0xB28); it
  then fires the weapon of each hand. `equip` puts an item in the hand its
  own decl's `equipSlot` names (+0x1B4). `trigger`, `trigger_pulled([hand])`
  and `weapon_ready([hand])` act on the weapon in the hand named, else in
  the weapon hand, read through the engine's own per-hand read (VK
  0x14074AB30 / GL 0x14074A440); `equip` by name reads the inventory (vtable
  +0x660, item VK 0x140EEDC50 / GL 0x140EED200).
- **The engine's fire control.** A demon in a firing fire mode fires by
  itself: the fire control (VK 0x1404456E0) asks the fire gate
  (`CheckPullTrigger`, VK 0x1404439D0) every frame the trigger is released,
  pulls when it passes, holds the trigger for a burst drawn from the weapon
  decl's skillSettings (and the aibehavior's weaponUseBehaviorList hold
  times), releases it, and waits the skillSettings' repull interval. The gate
  checks the attack token, the weapon's wait, awareness and recent sight of the
  aim focus' target, the weapon's range, that the aim tracker has caught up
  with the target, and the muzzle test (VK 0x140436460): a ray from the
  `muzzlereference` tag against a sphere at the aim focus point
  (`foci[0].finalFocusPoint`) whose radius is `aimFocusTargetRadius` times 3
  (`muzzleAimPrecision` PRECISE drops the 3), halved at the pull and 1.5 times
  while a burst runs; with no valid aim focus (`finalFocusPointIsValid`) it
  fails. The calls set and read that machine and keep no copy of it:
  `fire_mode(mode)` sends `setFireMode` with an `AIFIREMODE_` name or number
  (`FIREWHENREADY` and `SPRAYWHENREADY` are the self-firing modes) and
  `fire_mode()` answers the mode the script events set and the one the
  animation events set (`fireControl` +0x20 and +0x1C); `pull_trigger([check],
  [hand])` is the animation event `ae_aIPullTrigger` with the engine's own
  slot name (`left_hand`, `right_hand`), so the pull goes through the fire
  control, which draws the burst from the weapon decl (a bare `pullTrigger`
  event skips it, and a demon in the `WEAPON` mode then releases in its next
  think without a shot); `check` sets the event's `checkValid`, which runs the
  fire gate first; it answers the trigger state read back; `held([hand])`
  answers the decl of the item the engine holds in the hand now (a clip's
  `ae_equipInventoryItem` changes it without the script); `release_trigger([hand])` is
  `releaseTrigger`; `trigger_pulled` and `weapon_ready` read the engine's own
  trigger word and next-pull time; `muzzle_on_target([test], [hand])` calls the
  engine's muzzle test with the weapon in the hand (weights `"pull"`,
  `"firing"` or `"plain"`) and answers it with what it read: `focus_valid`,
  `point`, `radius`, `precision`, `require_muzzle` (whether the gate asks the
  test, the fire control's +0x27 byte), `tracking` and `body_rotation`;
  `focus(entity | position | nil)` sets the aim focus (`setFocus` for an
  entity, the set-focus-to-position call for a point, `clearAimFocus` for nil;
  `aim_at` and `aim_point` are the same two orders); `tracking([on])` and
  `body_rotation([on])` are the `enableHeadTracking` and `enableBodyRotation`
  switches the aim point and the turn in place need. `hold([turn])` stands
  the demon the engine's way: the engine's `ClearMove` (VK 0x1404EA6F0, on the
  animFSM at ai+0x23198) ends the pending move request, so the idle watchdog
  has none to count, and `ssaction_Idle` is ordered with an `AIIDLETURN_` type
  (`"RELAXED"`, `"COMBAT"` by default, `"INCOVER"`), which the idle action
  (VK 0x1405DCDD0) stores in the animFSM only when the demon's aibehavior decl
  has a turnParms entry for it. The idle is ordered without an end (time
  -1): the idle action (update VK 0x1405DF660) expires at its first update
  when its time is 0, and its end (VK 0x1405DC140) clears the move flag the
  order set, so a timed idle of 0 never turns the body (measured: the flag
  read 8 for one frame, the body stood 9 s with its enemy at 96 degrees). The
  demon's next order ends it. The engine then turns the body itself when the
  aim focus leaves the aim tracker's yaw range. `hold` answers whether that
  turn can happen (a turnParms entry for the type and body rotation on) and,
  when not, why. The fire modes and idle turn types are the engine's enums,
  checked against both executables.
- **A demon's own lasers and rear footsteps.** The game draws a laser only
  for an entry of the laser targeter component's list (the component at
  ai+0x2CD90, entries of 0x80 bytes at +0x100, the name at +0x60), and makes
  entries only in the enter of two stock state classes; `ae_setLaserState`
  switches entries that exist and adds none. `laser_add(tag, [ribbon])` calls
  the component's own add (VK 0x140551940) with the engine's atom of the tag,
  so the laser is the engine's: a ribbon from the model tag along its
  direction to the trace hit, switched by the clip's own rows. The add reads
  the ribbon from the component decl's `laserPinpoint_ribbonDecl` (decl
  +0x108); for a laser with a ribbon of its own the call puts that ribbon
  decl there for the length of the add and puts the decl's own back.
  `laser_remove(tag)` is the component's remove (VK 0x1405517A0).
  `rear_footsteps(front, rear)` serves a demon whose rear feet sound
  different: the game picks the footstep table by gait in one function (VK
  0x14074B790, actor and foot index) and never by foot, so that function is
  detoured (at the first call, 15 whole bytes) and, for foot indices 2 and 3
  of a demon that named a pair, answers `rear` where the game answered
  `front`. Records are per demon and spawn id; the tables are decl pointers.
  The first call also gives the two rear-foot events the skip bit the game
  gives the front feet (idEventDef.flags bit 1, read from `ae_leftFoot`):
  without it every clip of a blended walk fires its own rear-foot row and
  the rear table plays several times a step. No model of the game itself
  fires a rear-foot event, so nothing else changes.
  `wound_hides(wound, meshes)` serves a demon whose gore wounds replace a
  piece of skin by a wound mesh. A wound of the game shows meshes (its
  `meshKits`) and cuts the model with clipping spheres; it names no mesh to
  hide. The game hides meshes by name only for a damage group's armour
  (`hideMeshes`, VK 0x140830E10: the model's mesh index by name, then a clear
  of that mesh's bit in the model's 64-bit shown-mesh mask at model +0xEE8).
  The call resolves the names to mesh indices with the game's own lookup and
  detours `idGoreComponent::ShowWound` (VK 0x1409021C0, at the first call, 14
  whole bytes): after the game showed a wound of a demon that named hides for
  it, the meshes are hidden with the game's own hide, in the same call, so no
  frame draws the wound and the skin together. Records are per demon and
  spawn id; a model shows at most 64 meshes separately (the mask's width).
  `posed_legs()` serves a demon whose clips pose its legs themselves (thigh,
  shin and foot rotations, as DOOM Eternal's clips do) on a model that
  carries the game's leg rig. The game poses a leg from the hip to a target
  joint, swivelled by a pole joint, after the whole blend (the rig's entry,
  VK 0x141A20CD0: the aligner moves the hips and the targets by the ground,
  then the rig engine runs its components); its own clips animate those two
  joints, and a layer added over a clip can only add one fixed difference to
  them, so a recoil or a pain layer that moves a foot by an amount depending
  on the pose underneath cannot be carried. The call detours the entry (at
  the first call, 16 whole bytes): for a demon that asked, each leg of the
  rig's two-bone component gets its target joint set to the leg's end joint
  as the blended clips pose it and its pole joint set to the angle that turns
  the rig's own solve (called with no swivel) to the posed thigh, and then
  the game's entry runs unchanged. On a flat floor the legs are the clips'
  own with every layer; on a slope or a step the aligner moves the targets
  and the hips from there. Records are per demon and spawn id, found by the
  rig runtime (the second word of the model's animation object, model
  +0xE98) on the pose jobs' threads; a frame step drops a record whose demon
  is gone.
  `fk_reference_pose()` gives a demon DOOM Eternal's choice of the fk
  reference pose. Both games aim an fk chain from a reference pose of the
  effector and its chain (where the effector sits before the aim or look
  solve turns the chain); the aim and look focus goals are built from that
  effector origin. The game samples it from the animation of the anim web's
  current leaf, the heavier clip of a blend, and from the poseReferenceAnim
  the fkGraph names only when there is no leaf (VK 0x140418BF0); DOOM
  Eternal takes the named poseReferenceAnim first and the leaf only for an
  effector that names none. With the game's order a ported demon's head and
  cannon jump in the tick a blend's new clip becomes the heavier one. The
  call detours the reference pose (15 whole bytes) and the web's current
  leaf (VK 0x141705850, 18 whole bytes) at the first call: for a demon that
  asked and an effector that names a poseReferenceAnim, the reference pose's
  own first call of the current leaf answers none, so the game loads and
  samples the named animation. Records are per demon and spawn id.
  `eternal_interrupts()` gives a demon's animation orders DOOM Eternal's
  interrupt mode. Both games' anim web update (VK 0x14170B890) meets a new
  transition while the model's blend still runs by the current node's
  `AW_CF_USE_MULTIPLE_INTERRUPT` (nest the new blend, the oldest clip
  dropped when two run) or else by the web's interruptBlend: NO waits for the
  blend, YES ends it at once (the pose jumps to its destination), MULTI
  nests; each transition resets it to NO. DOOM Eternal's AI command carries
  MULTI for a change and for a forced via (ForceAndChangeState) and YES only
  for a forced node; the game's script events give a change NO and a forced
  via ForceState's YES. After such an order's event returns (its animator
  call runs at once), the call writes MULTI to the web's interruptBlend (ai
  +0x222B8, +0x570; ForceState VK 0x141704AA0 and the update are found to
  check the offsets). Records are per park record.
  `spawn_teleport(def, delay, backoff)` (a global, asked when the script
  starts) gives a demon of the mod's entityDef `def` DOOM Eternal's
  teleport spawn timing. Both games run the same show (VK 0x1403DE270,
  Eternal 0x1406FB2F0) and spawn effect (VK 0x1403D9610, Eternal
  0x1406F5CA0): the demon is hidden, FX_SPAWN_START starts at once,
  FX_TELEPORT_ENTER starts ai_spawnTeleportFXBackoff before the show and the
  show comes after the demon's teleport delay (ai +0xC0E0). DOOM Eternal
  takes the delay from the entityDef's teleportDelayMS and its cvar defaults
  to 0.1 s; SnapMap's spawn writes 1200 into every teleport spawn's delay (VK
  0x1407373C0 and five siblings) and 2016's cvar defaults to 0.8 s. The call
  detours the show (16 whole bytes, installed at the first call): for a demon
  whose def chain names a registered def it writes the delay where the show
  reads it and holds the cvar's float at the backoff for that one call (the
  effect runs inside it on the server); every other demon keeps the game's.
  The registry is per map (16 defs). `enemy_sdk.bind` calls the brain's
  `bind(def)` first, where the brain names its entityDef's pair.
  `destructible(def, spec)` (a global, asked when the script starts) names a
  breakable entityDef of the mod with what its DOOM Eternal destructible decl
  says. DOOM Eternal sets each piece's velocity at the activation
  (OnActivation 0x140AF8020): the preset's direction (spherical, angel wings,
  random; cone and thru-square are refused) at a speed drawn from [min, max],
  plus the decl's force in the owner's axes; the spin is the same direction at
  a second draw plus the force direction at its angular speed; a decl flagged
  DESTRUCT_FLAG_IGNORE_EXTERNAL_FORCE takes nothing from the hit. DOOM 2016
  throws a gore breakable's pieces by explosion records, the hit's among them
  (queued by the spawn, VK 0x14079E3B0, unless the damage's weapon-specific
  gore, VK 0x1407A06E0, answers false), and its break (VK 0x14079E6D0) moves
  every piece up to 20 units an axis. The call detours that weapon-specific
  gore (19 whole bytes): for a breakable whose def chain names a registered
  def it breaks the breakable itself, puts every piece back where the model
  places it (the broad phase moved, VK 0x141650390), writes each piece's
  momentum from DOOM Eternal's velocity and spin (mass x velocity; the world
  inertia, from the inverse the integration uses, x spin), and answers false
  for a decl that ignores the hit, so the spawn queues no hit record. The
  frame is the owner of the gore component whose ShowWound runs (the ShowWound
  detour keeps it per thread). The registry is per map (64 defs, 32 pieces).
- **The Enemy SDK's weapon library.** `enemy_weapon` (with `enemy_sdk`,
  `enemy_placement`, `enemy_locomotion` and `enemy_probe`, required by name
  from the DLL) is the interface a demon's behaviour layer uses for weapons:
  `equip`, `fire_mode`, `pull`, `release`, `trigger`, `pulled`, `ready`,
  `on_target`, `focus`, `hold`, `state` and `tick`, each one of the calls
  above. Its `tick` only keeps the aim focus on the demon's current enemy; the
  pull, the burst, the release and every shot are the engine's, as its header
  lists. `enemy_probe` logs `probe fire` at each shot: the fire modes, trigger,
  wait, focus, the engine's muzzle test and the muzzle sphere.
- **The Enemy SDK's locomotion library.** `enemy_locomotion` has a `ground`
  mode, the engine's own navigation, and a `perch` mode that holds a demon on
  a wall or a ceiling. DOOM 2016 has no body physics for either, so the
  library holds the demon itself: it places the demon every frame with its up
  along the surface's normal, and `surface_hold` takes it off the ground
  physics meanwhile. The brain drives the sequence Eternal's hangout follows
  and the library serves each step. `find_all` returns every valid candidate
  spot, found by tracing round the demon (and from any Perch object the map's
  author placed), each with its position, surface normal, wall or ceiling, the
  ground point the demon walks to, the length of the engine's path to it, the
  share of the target's sample points it sees and its distance from the
  target; the caller's distance band, `minVisibility`, `maxTravelTime`
  (needs a walk speed), detour limit, body clearance and search radius decide
  which are returned. `reserve` gives a demon one spot (one demon per spot; a
  dead or forgotten holder holds nothing); the demon walks to the spot's
  ground point with the ordinary move; `at_node` answers whether it has
  arrived and `spot_valid` whether the spot still has room, a free jump and a
  view. `mount` jumps from where the demon stands with no second walk;
  `facing_index`, `facing` and `set_facing_index` name the wall idle's
  discrete facings (index 0 faces up a wall and along +x on a ceiling);
  `leave` takes a callback for the frame the demon is on the ground again, and
  `grounded` says whether the library is placing it. The library owns the
  cooldown (`cooldown_left`, from the mount and again from the leave) and
  never ends a perch on its own. The jump, the pose and the hold are the
  script's and unproven in the game; the walk and the blocking volumes are the
  engine's. The contract is at the top of the file.
- **Queries.** `enemy()` (the idEntityPtr at ai+0x25050), `can_see(e)`
  (`isEntityVisible`, which is true when the perception's record of `e` was
  seen within the last second, VK 0x1404AA3E0, and then the world trace from
  the demon's eye to 40 inches above `e`'s origin, so a wall between them
  answers false), `aware()`
  (`hasAwarenessOfEnemy`), `awareness([e])` (`GetAwareness` on the perception
  at ai+0x29A88: unaware, lost, suspected, confirmed), `angle_to(t)`
  (physics origin and axis), `move_done()` (the move FSM status
  word at *(ai+0x23198)+0xD20, read as `Shared_MoveDone` and
  `Shared_MoveFailedAny` read it; done and failed), `turn_done()` (facing
  within 10 degrees of the last face target), `trigger_pulled()` (the weapon
  trigger word, as `Shared_TriggerPulled`), `path_length(pos)` and
  `can_reach(pos)` (the move FSM's path query that
  `Shared_TargetDistanceFilter` calls, VK 0x140515300 / GL 0x140514030,
  reached through that unique call site; the query is asynchronous, so a call
  may answer "searching" and the script asks again), `find_position(parms)`
  (points on a circle around the target that see it through the world trace
  from eye height, unless `visible = false`, scored by distance band and path
  length).
- **The calls a full enemy brain needs** (each made of the engine part named;
  every address is in `script_ai_sigs.c` with its pattern on both
  executables, and a call whose part is missing refuses plainly):
  `recoil(alpha)` writes the float at ai+0x24EB0 that the engine's recoil play
  (VK 0x1403D9300) multiplies into the recoil animator's weight, the field the
  combat states set from their `recoilStrength`; `weapon_ready()` compares the
  game clock with the weapon holder's next-pull time for the active trigger
  (holder +(slot+4)*0x10, slot from the trigger-slot table by the fire decl's
  +0x1B4), the "cooldown" test of the can-fire check (VK 0x1404439D0);
  `aim_point(pos | nil)` calls the set-focus-to-position
  `ae_selectAimFocusPoint` uses (VK 0x14041BBA0, foci 0, 2 and 3);
  `anim_valid(node, tests, [side])` reads the node's animations (VK
  0x141712C60) and their total root motion (VK 0x1415C5290), turns it by the
  demon's facing (by the direction to its enemy with
  `TF_TRANSLATION_TO_ENEMY_FORCE`) and tests the end point with the ground
  under it, the world trace at knee and chest height, sights to the enemy from
  eye and weapon height, the enemy's view centre (20 degrees) and the path
  query, as the test words ask; `anim_force_via(dest, via, [blend])` forces
  `via` (`ssaction_ForceAnimState`) and, when the observer sees the web leave
  it, asks for `dest` (`ssaction_ChangeAnimState`) from the per-frame step;
  `path_points(pos)` is the path query's computed path, read by a detour of
  the path length the query calls with it (VK 0x14050EA60 / GL 0x14050D790,
  14-byte window, segments at +0x90), every point of it (the engine's segment
  count, copied whole); `firing_position(parms)` (kinds any, new,
  superior, reposition) and `spline_clear(target, spline, seconds, [flags])`
  are compositions of the path query, the world trace and the ground under a
  point (`sh_ai_firing_run`, `sh_ai_spline_run`; the spline is a control
  table of any number of knots and a roll the script passes, and its curve
  has a point for every knot inside it, never cut). `find_position` and
  `firing_position` try as many candidates as `count` asks; each demon's
  path queries take keys from a span of 16,384 in the engine's path cache, so
  a query with more candidates than that shares keys in turn (those answer
  "searching" a little longer). Distances, weights, blend times and the
  seconds a spline looks ahead have no upper bound; `monster_type()` is the
  `getMonsterType` event named from the executable's aiMonsterType_t table;
  `pain_string(text | nil)` interns the text with the engine's atomic-string
  set and a 14-byte jump at the end of the pain request's painString branch
  (VK 0x1403F5F13 / GL 0x1403F53F3) runs a stub (`sh_ai_pain_stub_build`) that
  stores the demon's string in the request's [rbp+0x88], which the request
  then compares with each pain anim's +0x48 string; `block_pain(reaction,
  seconds)` makes a detour of the threat table's row check (VK 0x1406E08D0 /
  GL 0x1406DFD00, 15-byte window) refuse that reaction's rows for the demon
  until the game clock passes the time. The per-demon pain records are read
  lock-free by spawn id from the threads the engine runs pain on. Each detour
  is committed on the game thread at the first call that needs it.
- **Shared with the entity calls.** `distance`, `anim_state` and `anim_force`
  are the entity calls' and work on any entity; on a demon the two animation
  calls come back here (`sh_script_ai_anim`), which parks it first.
- **Orders around a step of the engine's.** A turn in place, a step and an
  arrival are stages of the animFSM's move state (stages 2, 7 and 8 of the
  int at animFSM +0x2A9C, switched on by the move update, VK 0x14051A660),
  and such a stage ends only when the animWeb command the engine gave for it
  is no longer waiting (the command context is the object the demon's vtable
  +0x2C8 returns, its active wait handle at +0x18, the status at +0x18 of
  that). A script's `ssaction_ChangeAnimState` goes to the web round that
  command: the web leaves for the script's node, the command waits for good
  and the animFSM stays in the stage, starting no turn and no move, until the
  engine's idle watchdog restarts it after a second in the idle node
  (measured on a ported demon: four to seven restarts in 100 s of a fight,
  every turn after the first late, and moves that never began). So an
  `anim_state` given while such a stage waits is held back and given the
  frame the stage has ended, the web having played the step out first in any
  case; a move order drops a held one. `anim_force` is given at once and the
  step's command is then ended the engine's way, with the context's own
  set-active-wait-handle (VK 0x1405223F0), which gives the command its done
  event and the status INTERRUPTED as every command sent through the context
  does to the one before it. `hold` leaves a turn in place alone when the idle
  order it asks for already stands (the idle turn flag and type in the
  animFSM): ending the turn's move there would leave its step without one.
- **Gore for a demon of the generic type.** The game's demon spawn inits the
  gore component unless the behaviour decl's `monsterType` is
  `AI_MONSTER_GENERIC` (the test at VK 0x14042A7AA; the init is VK
  0x1408FF0B0 with the component at ai +0x2B3D8, the demon, its entity damage
  component at ai +0x4E08 and the gore graph the entityDef names, already at
  component +0x630). A mod's demon has no type of its own in the game's list
  and takes the generic one so that no stock demon's hard-coded behaviour or
  decl rows reach it; without the init its gore component has no owner: hits
  are not recorded, no wound opens, no full body gib is tried, and the body
  is never faded out and removed (it stays, hidden, casting its shadow). The
  runtime tells the AI calls of every mod entity the handle table reports
  spawned (`sh_script_api_on_spawn`), and a generic-type demon with a gore
  graph and no owner on its component then gets the game's own init with the
  spawn's own values, when `g_EnableGore` allows it.
- **Install.** `sh_script_ai_install` runs after the entity calls' engine
  side and is the "demons driven by scripts" row of the feature registry.

`tests/script_api_ai_test.c` runs every call through real scripts over a fake
engine (the animation commands through `sh_script_ai_anim`) and checks the catalogue against `tests/fixtures/scripts/
event_registry_13954591.txt`; it also runs the compositions over fake
primitives with a wall and a hole in the world, `can_see` and
`find_position` against that wall, and the pain-string stub on a synthetic
pain request. `tests/script_ai_sigs_test.c` checks the signatures, the
detour windows, the path site, the call targets and slot table the new calls
read, the pain request's compare and the monster type table on both
executables. The fire control calls are tested the same way: every event each
sends, in order, with its arguments, the state each reads back, each refusal
sentence, and the answers of an engine that lacks the game parts; the
signatures of the muzzle test, `ClearMove`, the fire control think, the idle
action and the fire gate are checked on both executables with the offsets the
calls read from them, and the fire mode and idle turn name tables against the
executables' own enum tables.

### Mod scripts: the whole game by name

Five general calls stand beside the purpose-built ones, so a script reaches
what the game's own code and SnapMap's wired logic reach, and an SDK written
in Lua (a demon's brain, a player's suit) needs no private path into the
engine.

- **Fields** (`script_reflect.{c,h}`, `script_api_reflect.{c,h}`). `e:get(path)`,
  `e:set(path, value)`, `e:fields([path])`, `e:class()` and `enum(name)` walk
  the game's own type tables (`typeinfo.c`: a class's size, superclass and
  fields; an enum's members), so any reflected field of any entity is read
  and written by name and Snapmap+ holds no offset for it. A path is names
  joined by dots with `[n]` after a list or array (the first is `[1]`); a
  pointer or an entity reference met in a path is followed. Numbers,
  true/false and text are themselves; an enum is its member's name; an
  object is a table of its fields; a list is a list; a decl pointer is the
  decl's name; an entity pointer or reference is the entity. A placed demon's
  editor properties are fields of its `idPlaceableSnapAIEncounter`
  (`spawnBehavior`, `gameTeam`, `encounterMods.healthMod`, `staggerEnabled`,
  `entity` for the demon it spawned).
- **Engine events** (`script_api_game.c`). `e:event(name, ...)` runs any of the
  game's events on an entity and returns its result; `e:post(name, ...)`
  queues it; `event_args(name)` lists what it takes. The argument format is
  the game's own, read from the live registry at the call (whole numbers,
  true/false, numbers, positions, angles, colors, text, entities, decls); the
  leading "animation" argument of an `ae_` event is filled in. Only the few
  events that carry a kill record (`notice_` events with a `7`) cannot be
  sent.
- **SnapMap events.** `e:signal(event[, activator])` fires one of SnapMap's
  own events on an object, so everything wired to that output runs
  (`FireOutput`); `on.signal(event, fn)` hears every object of the map fire
  it (one detour on the walk over an object's listeners, placed when a script
  first asks). Event names are the editor's (`ai_killed`, `trigger_touched`,
  `logic_fired`: the `snapListenerEvent_t` enum, any case, with or without
  `SNAP_EVENT_`).
- **Cvars and decls.** `cvar(name)` reads and `cvar(name, value)` sets a cvar;
  `decl(type, name)` gives a decl object with `get`, `set`, `fields`, `name`
  and `type` (its class is the game's `idDecl<Type>`). Both outlive a map, so
  what a script sets is put back when the map ends (the runtime runs each
  module's closer after the states close).
- **`entities()`** lists every entity of the map, players first.

### Mod scripts: combat calls and the events of damage, death, pain and animation

`script_api_combat.{c,h}` adds the combat calls; the hooks that deliver what
happens to a mod's entities are in `script_hooks.c` (below). A decl a call
names is the mod's own, or the game's through `game("...")`
(`sh_script_check_name`), looked up by type; a name that does not exist is
refused with one plain sentence. An attacker is any entity; without one the
damage comes from the world entity, as a trigger's does.

- **Calls.** `damage(e, damage, [attacker], [from])` and `e:damage(...)` (the
  entity's own `damage` event: decl, direction angles, activator);
  `hitscan(from, dir, damage, [range], [attacker])` (the game trace, then the
  damage event on what it hits; answers `{pos, normal, entity}` or nil);
  `radius_damage(pos, damage, [attacker])` (the engine's RadiusDamage with the
  decl's own radius, which queues the deferred radius damage the game applies
  that frame); `explode(pos, damage, [effect], [attacker])` (radius damage and
  an effect entity); `fx(e, condition)` / `fx_stop(e, condition)`
  (a condition of the entity's own fx decl: on an AI or a player
  `ae_startFX_Networked` / `ae_stopFX_Networked`, started on every machine;
  on any other entity, such as an effect entity, which answers neither
  event, the engine's own entity StartFX / StopFX); `fx(effect, pos, [dir])`,
  `fx_move(e, pos, [dir])`, `fx_stop(e)` (an effect entity of the mod placed
  through `spawn` and its caps, removed by `fx_stop`); `fx_hard_stops([effect])`
  (the mod's fx decl of that name, or every fx decl of the mod, ends the way
  DOOM Eternal ends an effect, for content made for that game. The game's FX
  manager stops one action through one function, VK 0x1416EEEF0 / GL
  0x1416EF870 (manager, action index, time, immediate, recycle): an action
  with a fade-out time fades from the stop for that long, whatever its kind,
  and one without ends in that frame. Eternal's stop sends a model, a render
  parm item, a radial or gaussian blur, a screen overlay and a screen
  particle straight to the end, so their fade-out time acts only at the end
  of their own duration, and it starts an action the stop reached inside its
  delay with a fade factor below zero, with which a particle draws nothing.
  The call detours the function at its first use, 16 whole bytes: for an
  action of a named decl that Eternal would end, and for a particle or a
  light still waiting out its delay, it sets the action's fading bit before
  the game's own code runs, which then skips its fade branch and ends and
  clears the action as it does for a second stop. Every other stop, and
  every decl that was not named, runs unchanged; names are kept for the map
  the scripts run in); `sound(e, sound,
  [channel])` (`startSoundShader`); `e:tag(name)` (a tag of a demon's md6Def,
  a named point with a parent joint and its own offset and rotation, found
  as the engine finds aim and focus tags: the model decl's `_info` prop row,
  VK 0x1416CE1E0 / GL 0x1416CEE90, placed in world space by VK 0x1415EFAA0 /
  GL 0x1415F0CA0; otherwise a joint, from the base dispatcher's getJoint
  events; the position and forward direction);
  `e:pain()` (the pain reaction an idAI2 is in, idAI2 `+0x5060`, nil once the
  engine's pain state clears it); `ai:fire(projectile, tag or joint, target)`
  (`ae_launchProjectileAtTarget` run at once with no animation; answers the
  projectile, caught by the launch hook; a position target is reached by
  steering; the event takes a joint and launches from the demon's origin for
  any other name (VK 0x14076E780), so for a tag the launch hook gives
  `idProjectile::Launch` the tag's place and the axis toward the target: the
  projectile, its launch effects and its trail start on the tag);
  `ai:can_see_from(other, tag or joint)`; `ai:focus_tag` refuses a
  name that is not a tag, since the engine aims only at tags; `p:steer(target, [speed])`
  (`setLinearVelocity`, at the projectile's speed unless one is given) and
  `p:explode()` (the projectile's own Explode, vtable `+0xAA0`), both only for
  a projectile one of the mod's entities launched. A script may hold `p` for
  as long as it likes: once the projectile is gone `p:steer`, `p:explode` and
  `p:pos` answer nil and `p:alive` false, as every call on a removed entity
  does, and the mod runs on. `ai:fire` parks the demon first
  (`sh_script_ai_park`), as a move does.
- **Events.** `on.damage(name, fn(self, hit))` with `hit.amount` (what the
  engine applied), `hit.attacker`, `hit.inflictor`, `hit.damage` (the decl),
  `hit.location` (the damage zone of the first hit) and `hit.direction`;
  `on.death(name, fn(self, hit))` once per entity; `on.pain(name, fn(self,
  reaction, damage, attacker))` when the engine starts a pain reaction
  (`PAIN_FALTER`, `PAIN_STAGGER`, ...), with the decl and attacker of the
  entity's last hit; `on.anim(name, notify, fn(self))` for an md6 row
  `ae_notify "<notify>"`; `on.frame_event(name, command, fn(self, args))` for a
  row `ae_notify "<command>:<a>:<b>"` (args `{a, b}`, numbers as numbers);
  `on.projectile(name, fn(self, p, decl, origin, dir))` for every projectile
  the entity launches. Every launch the engine makes for a demon passes
  through `idProjectile::Launch` (VK 0x140F3A380) with the demon as owner:
  its weapons (fire control VK 0x1404456E0, `TestFireWeapon` VK 0x1403E01E0,
  `InitForFire` VK 0x140F18530 taking the demon as the fire owner,
  `FinishLaunchProjectile` VK 0x140EE3730), the `ae_launchProjectile*` rows
  and `ai:fire`. A weapon fires only while the demon's weapon hand
  (`weaponSlot`) holds one, so `equip` writes one line to the mod's log when
  it puts a weapon in the other hand: until the decl names that hand, the
  demon launches nothing and `on.projectile` has nothing to report.
- **Hooks.** Six detours, resolved at boot from `script_sigs.c` and
  `script_combat_sigs.c` (both checked on both executables by
  `tests/script_sigs_test.c` and `tests/script_combat_sigs_test.c`; one not
  found turns the events row of the feature registry off, naming it),
  committed with the frame hook at the first Play with scripts and never
  unhooked: the
  OnDamage dispatcher (every entity class's damage, with the applied amount),
  idAI2 Damage_Apply (a per-thread context so OnDamage can read the damage
  record: decl `+0x628`, direction `+0x634`, zone per hit `+0x1920`), idAI2
  Killed, SetPainReaction, the OnAnimNotify dispatcher and idProjectile Launch.
  Each calls the engine's function and posts one record to the owning mod's
  queue; they may run on job threads and run no Lua. The combat module keeps
  what the hooks saw (each entity's last hit, the deaths reported, the
  projectiles each mod's entities launched) for the calls and the pain event,
  each record as long as its entity lives: the launch hook makes the
  projectile's handle and its launch record together
  (`sh_script_combat_launch`), and the handle table's release drops both. The
  Play gate clears them.
- **Tests.** `tests/script_api_combat_test.c` runs every call, refusal and
  event against a fake engine; `script_e2e_test` runs the brute example
  through the AI and combat calls.

### Mod scripts: the Script node and map variables

`src/backend/script/script_node.c` is where a mod's scripts meet SnapMap logic.

- **The row.** A mod whose scripts call `on.input` (`sh_script_node_registers_input`,
  a scan that skips comments and strings; the builder records it as the
  scripts member's `node`) gets one built-in Toybox row, "Script", generated by
  `sh_script_node_row_make` (`script_node_row.c`, no Lua, linked by the
  builder, the store tool and the serving index) and never written into its
  folder: the
  entityDef `mods/<salt>/script` inheriting the stock Relay
  (`snapmaps/logic/relay`, `idSnapMapLogic_Relay`) and the row
  `mods/<salt>/script` inheriting the stock Relay row (`logic/relay`) with the
  mod's own strings (`<salt>_script_node`, `<salt>_script_node_desc`) and its
  Toybox category (or `<salt>_script_category`, the mod's name). The row's
  property sheet has the name only. A placed node is an ordinary entity of the
  mod's entityDef, so a map, a prefab or an export records the mod as for any
  mod row, and a build without the mod refuses the map in the usual way.
  The serving index serves it (`ms_script_node_row` in `modserve.c`): when a
  bundle's block has `node`, the two decls are added to that bundle's files
  with their bytes written once under `mods\.served\files\` by their own hash
  (as rewritten files are), the row needs the entityDef, and the strings join
  the bundle's `strings.en` for the string layer unless the author wrote that
  key. They are not in the manifest or the build hash, and archives never
  carry them: every machine that holds the build makes the same row from the
  salt, the mod's name and the category tag of its first Toybox row.
  `sh_script_node_reserved` names the two paths an author's own file may not
  use; the builder refuses one there.
- **Install.** `sh_script_node_install` resolves the Relay DoAction,
  FireOutput and variable-changed sites at boot and is the "Script nodes" row
  of the feature registry; the row is served whatever happens to it, so a map
  holding a node always loads.
- **Names.** A node's role is its editor name. At the Play gate (the SnapBuild
  detour in `nav_play.c`, after the scripts are prepared, only when a mod of
  the Play carries scripts) the map being converted is written to JSON through
  `EditorMapToJson` (`sh_apply_engine_map_json`) and
  `sh_script_node_play_begin` reads from it
  every Script node's `uniqueId` and display name and the names of the map's
  Integer, Number, Boolean and String variables in list order. A placed
  entity's game name ends in its `uniqueId` (`MapEntityForSnapEntity` names it
  `"%u_%s/%s_%u"`: module instance, module, inherit, id).
- **Input.** A detour on the Relay's `DoAction(this, activator)` (16-byte window,
  committed at the first Play with scripts): for a Script node of a running mod
  on the host it posts `SH_SCRIPT_EV_INPUT` with the node's name and the
  activator's handle and returns without the stock fire; the tick calls every
  `on.input(name, fn)` handler of that name with `(node, activator)`. A Script
  node whose scripts are not running (a client, a stopped mod) does nothing.
  Every other Relay runs the stock code.
- **Output.** `node(name):fire([activator])` calls
  `FireOutput(node, 8, activator)` (SNAP_EVENT_LOGIC_FIRED, what the stock Relay
  calls) on every Script node of the calling mod with that name and returns how
  many fired; a name no node carries writes one warning per map.
- **Variables.** `vars` is opened in every new state (`sh_script_api_add_opener`).
  `vars.get(name)` and `vars.set(name, value)` read and write the
  game's values (idGameLocal vtable `+0x720`: Integer `+0x138` with bounds
  `+0x150`, Number `+0x168` with bounds `+0x180`, String `+0x198`, Boolean
  `+0x1C8`), clamp an Integer or Number to its bounds as the stock Operation
  does, and when the value changed call the logic manager's variable-changed
  notification (`*(gameLocal + 0x298128) + 0x48`, types 0, 1, 2, 4), so wired
  logic reacts. `vars.set` returns the value stored. A name the map lacks, a
  name two variables share, or a value of the wrong type is refused with one
  plain sentence; while the map's logic is not running both answer nil with
  one warning.

`tests/script_node_test.c` covers the row, the names, the door example end to
end, `node():fire()` and `vars` without the game, and with the executables the
FireOutput, variable-changed and Relay sites on both renderers.

### Mod modules

A bundle module is `maps/modules/mods/<salt>/<leaf>.decl` (provides `snapModule`,
named by that path, the name a map's `moduleName` holds) plus
`generated/decls/snapmoduleinfo/mods/<salt>/<leaf>.decl`, which the builder
requires in the same bundle. Every other file is a name the engine derives: the
render model `maps/modules/palettes/mega_blessed/mods/<salt>/<leaf>.bmodel` (the
loader's palette group is the stock palette's, `mega_blessed`), and under
`maps/modules/mods/<salt>/<leaf>/` the collision `_combo/world.bcm` and
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

### Mod virtual-texture boards

A mod's lit materials draw from the mod's own virtual-texture board: its own
mega2 family (`virtualtextures/mods/<salt>/<name>_sq<N>.mega2`), named by the
materials' `landpagefile "mods/<salt>/<name>"`, read by the stock `skinvmtr`
and `skinvmtrwounds` render programs unmodified. Nothing of a mod goes into the
game's own board, and no game file changes.

Those programs read only the `vmtr*` render parameters. The renderer sets them
once per view from the game's own board (record 1: VK 0x1417B3D70, GL
0x1417B5000), and before each material's draws it pushes the material's
parameter block over the view values and rolls it back afterwards (VK
0x141A33680, GL 0x141A4C564 and three siblings). `modvt.c` therefore gives each
board three things, on both renderers:

- A page-source record of its own: `idVirtualTexture::SetSource(record,
  "mods/<salt>/<name>", "virtualtextures/mods/<salt>/<name>.pages", 2, 2, 0,
  NULL)`. SetSource's mega2 loader opens the served members through the
  provider (no `.pages` file is needed) and allocates the board's host page
  table and page-table image; the board owns a whole 2048 x 2048-page address
  space and its own feedback source (the record index). SetSource makes the
  page-table image a resource of the current level (lifetime 1); the next map
  load would purge it while the record and every material bound to the board
  still point at it (live, the next page-table upload faulted in the Vulkan
  driver, and a draw read a freed image from the material's block), so the
  image is raised to the session lifetime (4) at once.
- The record pointed at the material-format page cache, the pool of resident
  pages the game's own board uses (record 1's cache, flavor 0, VK sys+0xFCA0,
  GL sys+0x115A0): only that cache has the three-layer page array lit
  programs sample. The pointer is written right after SetSource, with the VT
  system's jobs synchronised, before any material is bound to the record.
- Two detours. The material binder (`VtMaterialBind`), after the engine's own
  writes, also sets `vmtrPageTableMap` (the board's page-table image) and
  `vmtrFeedbackFloat` (its feedback word) in the parameter block of a material
  bound to a board, so the material's draws read the board. The handles are
  the ones the per-view setter fills from record 1, read from its own globals
  (`VtViewPageTable`, `VtViewFeedback`, checked to read record 1 and to call
  the binder's feedback function). VirtualTextureForMaterial
  (`VtForMaterial`, called by the material rebind) returns the board's record
  for a material whose `landpagefile` names it; the engine's own lookup also
  compares the record's cache flavor with the material's, which no longer agree.

A stock material is never bound to a board record, so its parameters and
record 1 are untouched (the native test proves both byte for byte with boards
registered). The material parser binds a material to the record whose name
equals its `landpagefile`, by name alone, so a board's record must exist before
its materials are parsed.

- Which boards: one per board name the loaded map serves, read from the build
  each path is served from (`sh_modserve_each_served_here`): a map that brought
  its own copy of a salt pages that copy, never the root folder's; in the
  editor, the root folder's builds the Toybox shows.
- When: DOOM's game thread only, once the VT start-up has built record 1, and
  never while a job runs. The renderer's view jobs read the record table on
  worker threads every frame (the per-view setter, and the per-cache pass that
  reads each named record's cache), and SetSource writes a record's name long
  before its cache; a change made from the frame tick while those jobs ran
  faulted at boot reading a named record with no cache yet. So every change to
  a record (SetSource, the cache pointer, Release, a rebind) is made after the
  job manager has finished every job list (`JobsWaitAll`), as the engine does
  before it releases records when a map unloads.
  Registration runs at the start of every decl registration pass (the editor
  palette build, a map's activation), before the pass parses any material.
  Every other change (a board an index change, boot or Refresh brings, a
  release, a rebind) is queued to the one point of the frame where the engine
  changes VT state itself: EndFrame syncs the render thread (renderSystem
  vtable +0x80) and then calls the VT frame update (`VtFrameCall`, VK
  0x1558A79, GL 0x1559E29); a detour on that update runs the queued sync
  before the engine's own update. A release synchronises the VT system's jobs
  and then calls Release, the pair SetSource itself starts with, so no
  transcode or upload of the board is in flight and its pages leave the shared
  cache. (Changes made from the product's own after-frame tick raced the
  render thread: live, a boot registration faulted on OpenGL in the per-cache
  pass the render thread runs, reading a named record whose cache SetSource
  had not set yet.) A board served
  again takes its earlier record when free, and every material bound to it
  before is bound again at once (index, binder through its detour, block copy),
  so its parameter block never keeps the released page table. An index rebuilt
  on any thread only marks a sync for the next frame boundary.
- How many: the record table has 32 slots and the feedback word a 5-bit source,
  so the engine has records 1 to 31. Record 1 is the game's own board, record 31
  is reused by the engine on every lookup miss, and each map's own unique
  families (the hub, module palettes) hold some of the rest; the boards take
  what is free among 2 to 30. A board that finds none is refused with one `VT:`
  line and the player is told, once, "DOOM has no room for another mod texture
  board on this map, so <mod> draws with plain textures; a map with fewer mods
  that carry their own textures avoids this." Each board costs about 14 MB of
  host memory (11,184,810 bytes of host page table, the reader's index and the
  page-table image); its resident pages share the game's page pool.
- Failure: any of the seven engine sites (`VtSetSource`, `VtForMaterial`,
  `VtMaterialBind`, `VtMaterialRebind`, `VtViewPageTable`, `VtViewFeedback`,
  `JobsWaitAll`) that does not match its
  signature, or a decode that does not cross-check (the rebind must call the
  signed lookup and binder), leaves every site as shipped and turns "mod
  virtual textures on lit materials" off in the features line.
- `sh_vt` (`vt_harness.c`) is the diagnostic on the same binding, built only
  into the diagnostic build (`build.ps1 -Diag`): `list` marks the boards
  `(product)` and shows each record's cache flavor (0 for a board), `sync` runs
  a sync with its releases, and its manual `register`/`release` never touch a
  board. The member format is [vt-baker.md](vt-baker.md).

A mod's `hud` key (the author block of `mod.json`) selects how
the stock ammo widget shows a weapon (`modhud.c`; the object is
`{"weapons": {"<weapon decl>": {"ammo_display": "weapon" | "engine"}}}`).
`weapon` removes the multiplayer mode's forced numeric display for that weapon,
so a nonpositive capacity shows the widget's infinity frame; no ammunition,
decl or widget field changes. The object is exactly `{"weapons": ...}`. Rules
are read from the serving index's author keys (`sh_modserve_each_bundle_extra`)
of the builds the open map serves, one build per salt: a build counts when its
names serve now (`sh_modserve_bundle_scope`, `names_live`). The table is read
again at the HUD's next question after every index change (an index listener)
and every Play entry, so an install, download, rebuild, Refresh or removal
needs no restart. Identical rules compose; two mods asking different displays
for one weapon leave that weapon to the game, both named in the log, and every
other rule stays; a hud object that does not read drops that mod's rules only.
The widget update's one
call to the mode predicate (`WeaponHudModeCall`, `WeaponHudGameMode`) has its
aligned rel32 swapped to a near relay that passes the widget, whose weapon decl
is at `+0x1F8`; every other caller of the predicate is untouched. Logs:
`weapon-hud: N rules from the mods the open map uses`, `weapon-hud installed: ...`, and
`weapon-hud active: ...` on first use.

Index listeners (`sh_modserve_set_index_listener`) carry their own storage:
each call site's macro declares a static link, so the list holds every
listener the program registers and none is dropped.

When bundles of different keys provide one path with different bytes, the
open map decides among the providers it declares ([Map scope](#map-scope)):
the fewest needs the declared bundles do not provide, then the bytes most
declared providers carry, then the order of the map's rows. Nothing about the
install counts (time, folder, or whether a map delivered the copy), so a map
serves the same bytes on every machine whatever else is installed. With map
scope off, the static winner serves: the same two rules over every admitted
bundle, then the key and hash in name order. A path that only undeclared
bundles provide is left to the game while a map is open: their names have no
reach in it. (Before phase 2 one provider always served such a path, because
leaving it to the engine broke every boss pack at Play when their other files
were served and named it; the Cyberdemon's animWeb defaulted to an empty one
and `AnimWeb ... has no nodes` ended the process. A map that declares the pack
now serves all of it.) When every provider's
text is a decl that differs from the others only inside lists (a block of
`num = N;` and `item[i]` members, as the encounter conductor's `aiTypeList` or
the whitelist inspectors' `validEncounters`, which every boss pack extends),
or only inside bags of repeated entries in the bare statement grammar of an
md6Def (the `anim "<clip>" { event ... }` blocks of its `events`, its `alias`
rows), the winner's text is served with the other declared providers' entries
appended (composed per listing and memoized on the path, `ms_scope_compose`),
so several boss packs a map declares work together and several mods may each
add event rows to the player body; an undeclared mod's entries are never
composed in. The composed text keeps each `key = value` statement's `;`
terminator as its source wrote it: an md6Def's meshKits pairs (`"doors" = ""`)
have none, and the engine's md6Def parser refuses the decl (`expected '=' but
found '}'`, issue #160) when one is added. An appended entry takes the
convention of the block it joins, a renumbered list's `num` keeps the
winner's, and texts that differ only by terminators compare equal. Editor-time bundles (below) compose with each other in every map,
as before. One `MODS: conflict:` line at the index build names the
providers, the static winner and the reason; `sh_mods doctor` prints, per path,
which layer serves it for the open map and why. A
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

The editor closure is every served file of a bundle that adds editor rows,
and every served file one of those needs, at any depth
(`ms_mark_editor_closure`, per table). The whole bundle counts, not only what
its rows need today, because a later build's row can name a decl a map pass
already registered at level 1 (an open map's activation had made the keycard
md6Def a new row attaches, and keeping that decl alone outlived its model).
Every pass treats
it as the boot editor pass does, because after an in-session install the
same work happens elsewhere: the rescan re-parses the changed rows and
registers the new decls they need (an editor-only md6Def, say) before the
palette rebuild takes its watermark, and an idDeclFX an `fxInfo` names, an
attachment's md6Def or a new md6Def's MD6 model and anims were then created
at level 1 outside any watermark; the rebuild's keeper raised only the decl
objects a row names, not what they hold, and the Play purge freed the rest
under them (VK purge 0x141572170 / GL 0x141573300 frees every level-1
resource; the editor's FX start VK 0x1412a9e60 / GL 0x1412a9540 and
idEntityModels::Init VK 0x1412ab690 / GL 0x1412aad70 then read freed
objects on the return to the editor). So a refresh applies the closure's
pending changes first inside a watermark of its own and keeps what they
create, then the rest at the engine's level
(`sh_modserve_each_refresh_part`); a plan step for a closure decl keeps its
decl in any pass (`sh_modserve_step.persist`), and outside a watermark
`me_run` runs those steps first inside one (`sh_modserve_plan_split`), the
rest after; and a rescan whose rows changed runs the keeper inside a
watermark before the palette rebuild, which may be refused and retried. Outside
the editor plan such a step is kept unless the engine already holds the decl
permanently (`persist` 2), which is left to the game as a map pass leaves it.
A decl the keeper finds at level 1 from before its watermark (created by the
engine's own load) is counted as `older`: an md6Def is left whole at level 1,
since a row names it only by name and the editor finds it without creating it
(VK FUN_14123b340 / GL FUN_14123a7e0); an animWeb or material a row holds by
pointer is kept, and what it holds is kept by name, as below.

A watermark keeps only what a pass creates, so a decl a pass keeps after the
engine's own load parsed it would outlive what that parse created. An open
map's load creates the decls its entities name at level 1, and their parse
creates what they hold: the weapons pack's chainsaw pickup web made its clip
(`.../chainsaw_pickup_pre_idle.md6anim`) there and caches the clip's pointer
in a blend-tree entry; the map pass then kept the web (4), the clip stayed at
1, the Play purge freed it, and `idAnimWebBlendTree::Init` (VK FUN_1417244c0 /
GL FUN_141724fb0) used it at the Play spawn (a divide by zero at VK 0x15d2574
/ GL 0x15d3994, or a read at VK 0x1a1d3d9; snaphak-re triage X1). The rule is
therefore by name, not by time: every resource a level-4 decl of a served
bundle holds is level 4 (`sh_modserve_holds_raise`). From each non-surface
decl of an admitted bundle the engine holds at level 4, the needs the builder
recorded for its file (an animWeb's clips and md6Defs, an md6Def's mesh,
skeleton and clips, an entityDef's models and md6Defs) are closed through
every index file that provides one, looked up by the list's type (+0x08;
`anim` for idMD6Anim, VK 0x140215b00) and the folded name, and raised from 1
to 4. It runs after every serving pass (`me_closure_end` at the outermost
watermark, `me_editor_rows`) and before the Play level load
(`sh_modserve_play_begin`), so a held name the engine creates after a pass (an
editor spawn) is kept before the first purge that follows. A log line
`<phase>: N resource(s) kept decls hold kept for the session with them` names
what it raised. Nothing the product keeps is released in a session, and a
name a kept decl no longer needs is not raised by later runs. Since rules 6
an md6Def's needs name its mesh's skeleton and default clip too (read from the
cooked mesh, see the builder), so a raised mesh never keeps a skeleton at 1
that the Play purge frees (the keycard of
`interact/yellow_key_tech_corpse`, VK 0x1a05687; snaphak-re triage V2, F7).
What the needs do not name (a stock mesh's materials, a skeleton a clip names
only in its bytes) is outside that rule, and the next one covers it.

A pass the editor runs while a map is open (an in-editor rawmap reload, Open
map in the editor, `sh_mods map add`) registers while the previous map's
level-1 objects are still alive, since the in-editor load runs no purge
(DoLoadMap, VK 0x1405253e0, calls neither the purge driver nor the purge). A
decl the pass keeps then binds to those objects: they are older than its
watermark, and no recorded need names them (an entityDef records its own
md6Def, not its attachments), so the Play purge freed them under the kept
decl (3 of 4 such Plays faulted, fresh opens 0 of 18; snaphak-re triage V3,
F8, F8b). The engine finds every existing resource through one find-only
lookup, `ResourceLookup` (VK 0x141571d50 / GL 0x141572ee0), which
`ResourceMaterialize` asks before it creates one; while a watermark is open
the product records, on the main thread, every resource that lookup returns
(a 19-byte detour, installed once the index holds a decl), and the outermost
watermark's close raises each one still listed at level 1 to 4 under the
resource lock (`sh_modserve_lookups_*`, from `me_closure_begin` and
`me_editor_rows` to `me_closure_end`). The set is deduplicated by address and
emptied at the close; a stock object already at 4 costs nothing. A log line
`<phase>: N resource(s) the <part> looked up kept for the session with it`
names what it raised. So a kept decl never holds an object that outlives it
by less, whether the pass created the object (the watermark), parsed it
earlier (the needs by name) or found it already loaded (the lookups).

What the engine loads on behalf of a kept object is kept with it, whenever
it loads (`sh_modserve_attrib_*`, `sh_modserve_kept_*`). The kept set holds,
by address, every resource a keep raised or a pass created at level 4. A
second detour, on `ResourceGenericLoad` (VK 0x1416c6050 / GL 0x1416c6d30,
20-byte head; `ResourceMaterialize` calls it at +0x249 for every resource it
creates), tracks the engine's load nesting on the main thread: each load
opened inside another, and each resource `ResourceLookup` returns inside a
load, is recorded as a (parent, child) pair, and a child of a kept parent is
kept at once (raised when its own load returns, since the load's failure
path reconstructs the object at level 1). A Play's spawns load objects no
kept decl held yet, nested (the chainsaw gore md6Def loads its mesh, whose
load creates its material; snaphak-re triage F9 census); when a later keep
lifts one of them, every close (`me_keep_attrib`: after each outermost
watermark, after the editor rows, and before the Play level load) follows
the recorded pairs from every kept resource, transitively. The close drops
kept addresses no list holds any longer, keeps only pairs a later keep can
still follow (a parent neither kept nor at level 4), and forgets all pairs
before the Play purge. Both sets are bounded at 2^20 entries; a full set
stops recording and the close says so. The first close logs whether the
hook recorded anything, so a build whose loads it never sees says what is
kept only by name.

Raising a level keeps a resource's object, not what its load allocated from
the current heap. The engine's allocator (VK 0x140328530) takes heap -1 as
the top of idMemLocal's heap stack (ids at +0x44, depth at +0xC4), else
process heap 0; an MD6 skeleton's joints (`+0x40`, VK 0x1416d0770,
`TAG_MD6`) come from it. The editor's reload after a Play exit loads the
gore md6Defs, meshes and skeletons with a map heap selected, and the next
Play load releases that heap: the skeleton the holds-by-name raise had
lifted to level 4 kept its object and lost its joints, and the chainsaw
pickup's attachment animator read them (G1, VK 0x16f7f0b; 2 of 2 before
this rule). So the load hook records the heap each load runs with, and a
resource whose last load ran with a heap other than 0 stays at level 1: the
purge frees it whole and the next load recreates it. A load in heap 0 clears
the mark. A log line `<phase>: N keep(s) left at level 1 because the map's
heap holds what their load allocated` names them.

The keepability invariant: nothing raises a resource to level 4 except
`sh_modserve_keep`, and it raises only a resource `sh_modserve_keepable`
allows, one whose last load did not run with a map's heap selected. Every
keep goes through that one call: the watermark, the holds by name, the
recorded lookups, the attribution, the engine side's keep of an editor row's
animWeb or material (`me_keep_ref`), a kept step's decl after its load
(`op_load`) and a held render model (`me_held_one`). Only a step that keeps
its decl (an editor row's closure) joins the kept set before its native load;
a map pass's decls return to the engine's level after it and nothing they
load is kept, so a mod without editor rows is never made permanent by the
next Play entry. The one other level write is the native load of a new
identity at level 4, which `op_load` sets back to the engine's level as soon
as the load returns.

A kept decl's render models are loaded by the product itself, in process heap
0, while their files serve (`sh_modserve_held_models`, `me_hold_models`, run at
the start of every `me_keep_holds`). An entityDef's parse does not load its
`renderModelInfo.model`; the entity's spawn does, at level 1 in the map's heap,
so no keep holds it and every Play purge frees it. The rows' decls stay
registered in every map, and the Play level load of a map that does not
declare the bundle loads all of its prop models again from them (all 17 of
`local.rocks` in an empty map); in Play an undeclared bundle serves nothing,
so each load ends as the engine's default box (state bit 2, bounds +-8), and
the editor after the Play exit makes the same defaults. The editor's own map
changes purge nothing, so a later open of a map that places the props drew
black cubes until its next Play (snaphak-re X2: fresh boot, empty map, Play,
exit, rocks map: 3 of 3 black; fresh rocks map: whole). So for each `model`
need of the holds closure whose providing file the index serves in the
current scope, the product loads it when the lists do not hold it
(`ResourceMaterialize`, VK 0x141572660 / GL 0x1415737f0, quiet, the find-or-load
the model manager's FindModel runs), and loads it again in place when the
lists hold a default, a failed load, or one a map heap owns; it then holds it
at level 4 inside its own keeper, so what the load creates (materials,
images) is kept with it. A load that still ends in a default stays at level 1
and is named. A stock model (no provider) is the game's bytes in every scope
and is left to the engine; nothing is loaded during Play. The log line is
`<phase>: N model(s) kept decls hold loaded for the session while their
bundles serve them`.

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

On every parse the `DeserializeFromJson` detour sees, a map in an earlier
shape (a retired package carrier among them) is refused first (The map
carrier); the parse never converts or installs anything. `modmanifest.c` reads
the manifest rows from the member's `mods` part before the engine parses the
map, and activates the builds that serve them.
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
save it writes, into the member's `mods` part, one row per bundle that provides a placed name (a placed
entity's def, module or a model its edit state names; never a shadow of
editor-time content, see the asset rule) and each bundle their needs reach,
and one per map-level row of the current manifest;
with an empty table, no rows in the map and no map-level row
it returns at once and the save is the engine's own bytes. A map-level row is
written for the build that serves its mod (the row resolver modui.c binds,
`sh_modserve_row_build`), and the current manifest follows the rename; a
row of a mod that is not installed is kept as it is.

Threads: the table is built on the bootstrap thread before the provider hook is
published and replaced only by a rescan (`sh_mods install`, a build inside the
store, a download or import,
`sh_mods rescan`). Readers see one complete table. A new table carries what
later questions need from the one it replaces (the salts registered this
session, which bytes the engine already read, the removed paths a refresh
still owes, `ms_carry_forward` and the gone list), so no question reads an
older table after its build; at the start of each registration pass on the
main thread, `sh_modserve_index_retire` frees every table two or more builds
behind the published one when no provider call is in flight on any thread
(each counts itself in and out) and no plan is alive, and drops the file
cache's entries for it. A string the index hands out stays valid until then;
a consumer that keeps one longer copies it. Registration, activation and the palette rebuild run on DOOM's
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

What an in-place reload or re-parse of a held resource creates must live as
long as that resource. Reloading a held model resolves its materials again, and
a held material re-parsed binds to the images that exist at that moment; an
image created at map level is freed by the next purge while the permanent
material keeps the pointer, and the first draw reads freed memory. That was the
cold My Maps launch crash on both renderers (snaphak-re ADD-G6): the map load's
scope change reloaded the boot-held spider and Olivia meshes outside any
watermark, while the editor-first flow ran the same reload inside the editor
pass's watermark. The gated reload and every refresh part therefore run inside
a keeper of their own (a resource watermark raised to the session's level when
it ends, `me_closure_begin` and `me_closure_end`) whenever no other watermark
covers them and they have work (`sh_modserve_inplace_keeper`).

A surface (a stock pointer at the game's own path: the game's bytes under the
game's name, `sh_mb_stock_restore`) is never registered, re-parsed or
refreshed by any pass. The provider serves it whenever the game parses or loads
it, at the game's own time and level, so it is in no plan (editor, map, scope
activation, refresh), no refresh list, no early-shadow list, no editor-row
reference and no ledger sweep (`sh_modserve_is_surface`). The one exception is
the gated reload below: a surface model or image the game loaded before its
bundle's requirements were live is loaded again, inside that reload's keeper,
once they are. A cold process holds the stock spider mesh from boot, loaded
with the blacklists on, and the requirements go live only at the first map
load; without that reload the mesh's blacklisted materials are never created
and the spider body is not drawn (snaphak-re ADD-V7a). The keeper raises the
materials and images the reload creates to the session's level, as the editor
pass's watermark does in the editor-first flow. The
engine's own source lookup could not stand in for this: DeclSourceFind misses
decls archived in the game's containers, so a stock-path material a pack
carries as a surface was registered, and so re-parsed in place, by every map
pass whose engine did not already hold it at level 4.

A plan orders its steps by need, then by rank: renderParms first, then other
decls, md6Defs, entityDefs and snapEditorEntityDefs (`SH_MODSERVE_RANK_*`).
The engine gives a renderParm its parm-state slot and kind at its first parse
and keeps them on a reparse, and a material's parm keys are bare names that no
need records, so a material parsed before the renderParm it assigns bound a
slot the renderParm's own parse then retyped. On a cold My Maps launch of a
map with the campaign doors pack, `skull_key` was parsed before `skinsssmap`,
and a render job later read two floats as a sampler pointer, on both
renderers (snaphak-re ADD-G5). Every renderParm therefore registers and loads
before any other step: in the map plan, in the part of a split plan that runs
first, and in the editor part of a refresh, which runs before the rest. A
renderParm a pack carries as a surface (`skinsssmap` in the doors pack) is in
no plan at all: the game parses it from its own bytes when a material first
names it.

Registering a file is itself a parse. DeclRegisterFile's loader looks up every
decl the file defines, and an existing resource of that name is re-parsed on
the spot (VK FUN_141559a30 -> FUN_141561af0 -> DeclParseSource), whatever its
level. A decl pointer it reads is set by DeclFind with create, at the level of
the pass. The registration source check misses archived stock decls, so a
bundle's stock-named file reaches registration, and a map pass used to re-parse
permanent decls that way. The unlockModel `spidermastermind` (level 4, its
model null after boot) was bound to a `spider_mastermind.md6` the pass created
at level 1. START MISSION's purge freed that object, and leaving Play faulted in
`SetupModelList` (VK RVA 0x33a130). The registration phase therefore asks the
engine first (`held`, a walk of the type's resource list that creates nothing)
and does not register a permanent decl in a pass that does not keep its decls,
by the same rule the load phase uses (`sh_modserve_load_left_to_engine`):
`MODS: map: <type> '<name>' (<bundle>) left to the game: permanent, ...; not
registered by this pass, whose registration would re-parse it in place`.
Nothing is remembered, so a later pass decides again. Permanent shadows whose
bundle bytes differ from the game's still reach the engine through the
re-parse above and the refresh.

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
reloaded in place by name, only once its bundle's gates are live. A changed
`anim` (`cooked/anim/<stem>.bmd6anim`, refresh kind `SH_MODSERVE_RF_ANIM`) is
reloaded at a map pass only (`modserve_activate_bundles`): there the engine's
purge has run (every SnapMap map load purges levels 1 and 2, report ADD-G1),
the departed map's entities are gone and the next map's are not spawned yet;
marked anywhere else it waits for the next map load. Only an anim held at
level 4 is reloaded (a level-1 anim is freed at every transition and read
again from the current bytes); its served bytes are checked first against what
the engine's Load accepts (magic and version `0x2602414d` read big-endian, the
skeleton name's length), because a failed Load frees the data the held anim
points at, and one that fails the check is kept as it is and logged
(`MODS: reload: anim '<name>' kept as the engine holds it: ...`). After the
reloads the md6Def and animWeb decls the open map serves that name a reloaded
anim are re-parsed, so what they built from the old data is rebuilt (which
engine objects cache an anim's data pointer is not established). Raw names
match the list entries case-insensitively with the id's `-` and `_` as one. A
skeleton has no in-place route (it heads an ordered closure of models, anims
and decls that is unsafe while any animator lives); `sh_mods held` counts the
mod skeletons held at level 4, and a changed skeleton stays a restart. A decl the
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
or wire held) is retried by `sh_mods palette rebuild`. The engine sorts the
palette's rows in place (`SnapPaletteSort`) from four places, and the Play
build does it twice: once inside its own process-heap scope (the validator)
and once after that scope is popped, in the map heap (the Play map's
structure). A sort swaps rows through a temporary and gives a row string a
new buffer, in the current heap, when the value moving into the slot is
longer than its old one; a row grown by the second sort kept a map-heap
string, the return to the editor freed it, and the next Play's first sort
read it (the review's A-D3 fault at DOOM+0x339c40, a row's clipModelName).
The Timeline tile's sort detour (`tl_palette_sort`) runs every sort inside
a verified process-heap scope; `sh_mods palette check` (the diagnostic build)
reports where each
row's strings live and how many sorts ran. What stays reported
rather than applied: new names of a mod no map has used this session (they
register when a map that names the mod loads), a changed movie (read at the
next map load), a changed anim (reloaded in place at the next map load), and
changed files nothing reloads in this process (a sound bank whose bytes
changed under a name Wwise already loaded, skeletons and other raw files
besides models, images and anims) or removed non-row decl names, which need a
restart; a stock decl no bundle serves that
inherits a changed one cannot be found from the index. The log carries one `MODS: refresh:` line
per category and a summary; the Mods tab's rescan result carries
`refresh: {reparsed, reloaded, needsMapReload, needsRestart, pending,
deferred, note}`; a sound bank whose bytes changed under a name Wwise already
loaded is recorded by `modsound.c` (`sh_modsound_restart_banks`), and the
tab's result says "Restart DOOM to hear the new <bank>." once per bank while
the mod's row keeps it in `restartBanks`; `sh_mods rescan` (or `sh_mods refresh`) echoes the same
lines to the console; `sh_mods status` says when changes are pending.

Served-bytes ledger (`modledger.c`): the engine records no byte identity for
raw resources and, after a production read, none for decls but entityDef
(report ADD-G3), so the product keeps its own. Every serve through
`sh_modserve_open` (the provider's `+0x148` open, which the raw loaders and
the decl read path both reach) calls the serve listener, which records per
request path the serving bundle's hash and the file's identity: the first 64
bits of its recorded SHA-256, or an FNV-1a 64 of the bytes for a composed text
or a file without one. A product read of served bytes (the anim check, the
dependents search) is not recorded. Records live in a fixed open-address table
of 16,384 paths (32,768 slots), published once and updated under a per-record
sequence count, so readers take no lock; a serve past the bound is counted and
the ledger then calls itself incomplete. It is bound in `sh_modserve_install`
and is complete only when the provider served nothing before that. At a
refresh, a pending model, image or anim is reloaded only when the ledger says
the bytes the engine was given differ from what the open map serves now
(`sh_modledger_decide`: KEEP when equal, REFRESH when different, GAME when a
complete ledger has no record: the engine's object never had a mod's bytes and
is not refreshed); shadows and withdrawn paths, and decls (a child is marked
for its parent's change, not its own), keep the marks alone. At each map pass
the ledger is also swept: a held name the provider served whose bytes differ
from what the declared builds serve, which the marks left, is refreshed by its
type's route (`MODS: map: ledger: ...`), and a held skeleton or other raw type
is reported as a restart. `sh_mods held [filter]` (the diagnostic build) prints the ledger (request,
bundle, file identity, serves, whether and at which level the engine holds the
name) and the census of level-4 names the product served, by type, ending with
the count of mod skeletons held at level 4.

### Map scope

Two layers (DESIGN-additive 2.3): while a map is open or played, the bundles
it declares serve, then the game. The listing is the declared bundles' hashes
in row order in `modserve.c`, set by `sh_modserve_scope_set` from manifest
rows (the builds `sh_modserve_row_build` answers for them) and the placed
closure (a salt alone declares nothing), with whether a map is open, the
loaded map's place (`sh_modserve_place_set`: a hash names the root's folder at
that build, else the place's, never another map's place), and a mode,
`sh_modserve_scope_mode`: `SH_MODSERVE_MODE_PLAY` from the Play entry
(`sh_modserve_scope_source` with `SH_MODSERVE_AT_PLAY`) until the editor's
tick sets `SH_MODSERVE_MODE_EDITOR` again (`modui.c`), and EDITOR with no map.
The first hash of a salt in row order declares that salt; every other build of
it is inert for the map. Each admitted bundle gets serve flags computed from
the listing and read lock-free (`sv`: names under its salt, shadows, surfaces):

- declared: everything it carries;
- another build of a declared salt: nothing;
- an editor-time bundle (its shadows include a `generated/decls/snapeditor*`,
  `snappropertyinspector*` or `snappalette*` decl, or the editor's own movies
  under `generated/swf/snapmaps/`): its editor tweaks, in every map this
  editor opens; the rest of it follows the other rules (that is
  `my-overrides`' editor tweaks);
- otherwise, in the editor and with no map open: for the root folder's build
  of the salt, the names under its salt (the Toybox offers its rows; placing
  one declares it at the next save) and its surfaces, never its shadows; a
  build in a map's place, nothing (it serves only for a map that declares
  it). In Play: nothing.

Requirements: a declared bundle's apply; an undeclared root build that adds
Toybox rows keeps them outside Play, because the palette registers its rows
in every editor map and with none open; in Play (the editor's, or a hub
launch) no undeclared bundle's are accepted, so their cvars are the game's
from the Play entry and applied again when the editor shows a map; any other
undeclared bundle's are not accepted (`req_off`). A registration step is
gated by the requirements of the build that serves its path for the listing
(`sh_modserve_plan_block_unmet` through `ms_scoped`), not by the path's static
winner's: with two builds of one salt installed (the root and a place) the
winner of a path both carry can be the build the open map, or the Toybox, does
not use, whose requirements are off, and gating on it blocked every row of the
salt. The same holds for every per-path decision: a plan's order and blocking
and the holds by name read the needs of the build that serves the path for the
open map (`ms_serving_file`), so a map that brought its own copy of a salt
keeps what its copy needs, never the root folder's; the editor closure, marked
once per index before any map is open, follows the needs of every build of
the salt; and a re-parse of an entityDef drops its default type-info tree
whatever asked for it, so the tree follows the bytes now served. A `mod.json`
`"scope"` key is a retired key: the folder is refused. Per path,
`ms_resolve` records every provider of a path several bundles carry on its
static winner (`alt_first`: the static rules' order for different bytes, the
root folder's build first for one salt, index order for
identical bytes), and
`ms_scope_compute` chooses the file that serves it for the listing
(`scope_alt`, read by `ms_scoped` on every lookup): among the declared (and
editor-time) providers by the conflict rules above, composed when their texts
compose, else the first provider an undeclared bundle may serve, else none.
A path with no file misses `sh_modserve_lookup`, `sh_modserve_open` and the
decl probe, so the provider falls through to the engine; the early-decl and
gated-reload visits skip it; reports read the static index through
`sh_modserve_lookup_indexed`. The editor plan registers the rows whose path
the listing serves (`sh_modserve_plan_editor`), and the save-time closure and
the map listing map a placed name to the build the open map serves
(`modserve_bundles_for_names_visit`), so a save keeps each row's pin.

When the listing or the mode changes, every shadow decl, model and image whose serving
bytes changed (withdrawn, admitted, or moved to a provider with other bytes;
moving between identical copies marks nothing) is marked pending at generation
0, and so is every name another build of its key now serves with other bytes
than the ones last served (`scope_last`); the inheritance closure is marked
again, and `sh_modserve_each_refresh` offers them
with `gone` set while a shadow is withdrawn (no gate needed for the game's
bytes). A withdrawn name is never offered: it has no game bytes to go back to,
so the engine keeps what it parsed until a map serves it again. The
engine side is the refresh of a rescan: `sh_modserve_scope_apply` runs the
requirements pass first, then `me_refresh_apply` (re-parse in process heap 0,
reload in place by name) and the gated reloads. The listing is set and applied
at:

- a map load: `modmanifest.c` calls the scope hook (`modui.c`) with the rows
  just read and, when `sh_modserve_scope_relevant`, the map's placed names,
  before the bundles are activated, and applies at once. The load's
  activation then runs for the rows and every build the listing declares
  beyond them (`sh_modmanifest_set_listed`, bound to
  `sh_modserve_scope_hashes`): the placed names' closure. A map that places a
  mod without a row (a rawmap opened as a new map) therefore registers that
  mod's decls, materials, models and registries as its saved copy's load
  would (`map: N bundle(s) its entities place without a row were activated
  with its rows`);
- a save: the hook gets the rows written (placed closure and map-level rows);
  the change is applied on the next editor tick;
- the Play entry: Play does not serialize the map, so `sh_modserve_play_begin`
  asks the scope source (`modui.c`) to read the current manifest and the live
  placed names, then applies before the requirements check and the level load;
  only the editor's Play reads them: a hub launch keeps the listing its launch
  gate set, since the editor's entities are not the launched map's;
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
  reads 1), so the listing stays. The current manifest goes with it
  (`sh_modmanifest_current_close`), so nothing later reads the editor map's
  rows as another map's. The tick sets the listing again from the open map when an
  editor map is open without one;
- the Mods tab's and the console's map add and Remove from map, at once.

With no map (at boot, and at the hub after the editor) no undeclared shadow
serves. A published or My Maps launch from the hub declares exactly the
launched map's rows and runs in PLAY mode: the launch boundary
(`PublishedMapLobbyLaunch`, `PublishedMapOfflineLaunch`,
`LaunchLocalSnapMapForOfflinePlay`, and the resume of a held one) calls
`sh_modui_scope_launch`, which lists the current manifest's rows (the launched
map's parse made them current; the editor's exit cleared the editor map's),
sets `SH_MODSERVE_MODE_PLAY` and applies it before the original launches, so
an undeclared bundle serves nothing in the map. Before it lists anything, once
per launch (not again from `launched` or `loading`), it runs the pre-load pass
(`sh_modserve_launch_preload`): at the hub, in the editor mode and process heap
0, the editor rows and their closure are registered as an editor entry
registers them, then any step of the declared builds the parse's map pass did
not settle (`sh_modserve_plan_unvisited`). The palette pass the level load runs
then registers nothing new; in a cold process it used to register every mod's
rows there (161 rows, 645 steps with the campaign packs installed). The listing holds until that
Play has run and ended, by a state machine in `modui.c` driven by the signals
the product has: `launched` (the gate's `sh_modui_scope_launch`), `loading`
(the tick's busy callback reported a load or Play), `playing` (the Play-begin
gate, `sh_modserve_play_begin` from the SnapBuild detour, asked the scope
source), `running` (busy again after that) and `ended` (the first idle tick
after `running`). Only `ended` clears the listing and the current rows and
returns to EDITOR mode (`scope: the launched map's Play is over`); an idle
tick in any earlier state does nothing, since the gate runs at the launch's
map parse and the level load starts frames later (ADD-V4a H2: a tick in that
gap withdrew a boss's rows and its decl parsed empty). The editor's return
ends the launch without that clear. A launch the gate refuses (denied,
dropped from its hold, or refused at once) calls `sh_modui_scope_launch_refused`
through the ops' `launch_refused`: nothing was launched, so the current
manifest is closed and nothing is declared, or, with the editor still showing
a map, the state the launched map's parse replaced comes back
(`sh_modmanifest_current_restore_previous`). The editor's tick also sets
EDITOR when the editor next shows a map,
and an editor palette pass does it first (a pass in PLAY would not register
undeclared rows). A
`mod.json` author key `"scope"` stays in the manifest's `extra`, so a mod with
it keeps its canonical text and hash; it is ignored.
`sh_modserve_index_boot` enables scope for the product's index; the native
tests enable it where they test it (`sh_modserve_scope_enable`).
`sh_mods status`, `sh_mods doctor` and `sh_mods map` print
`sh_modserve_scope_report`: one header per mod installed as several builds
("scope: <mod> is installed N times: your own copy and the copies downloaded maps
brought. ..."), one line per build (the mod, where it is, its names, shadows and
surfaces, and what it serves for the open map), then per path several bundles provide with
different bytes which layer serves it and why (`serving: <request>:
<mod name>, declared (row N): <why>; not served: ...` or
`the game (no declared bundle provides it)`), and the counts per layer. The
walks of the engine's resource lists (watermarks, `sh_modserve_resource_each`,
`op_held`, the ledger's census and sweep) hold the engine's resource lock: the
static struct whose first qword is the CRITICAL_SECTION every list add, remove
and find takes (generated globals entry `resource_lock`, Vulkan RVA 0x5D79238,
OpenGL 0x467E8B8, decoded from the end of ResourceStaticPromote into the lock's
accessor; `globals_test` checks it sits 0x3D8 past `decl_resource_head` on both
renderers), bound in `sh_modserve_install` through
`sh_modserve_set_resource_lock` (ADD-G3). The accessor builds the section on
first use, so a NULL pointer means no list was touched yet. Callbacks and
loads run after it is released. The map load's `MODS: map manifest names ...` line
ends with the registration steps the activation found ready, or "no new decls
to register" for bundles of shadows and requirements only.

### Derived edits

A mod that edits a game file carries the edited copy at the game's own name;
`mod.json` says nothing about it (DESIGN-additive 2.4). The builder records the
copy's structural diff against the game's text, the product generates the game
file per map from the game's text and every declared mod's diff, and the copy
itself is never served (`decl_tree.c` is the model, `modbundle.c` derives,
`modregistry.c` applies, `modserve.c` serves, `modregistry_choices.c` holds the
map's choices).

- **The model.** `decl_tree.c` parses the decl grammar into a tree: `key = value`
  fields (a value may be several words, strings or `( ... )` groups on one
  line), `key {` and `key = {` blocks, named blocks (`anim "<clip>" {`,
  `painGroup "head" = ...`), the md6Def spelling without `=` (a statement ends
  at its line), bare members (`LeftFoot` on its own line) and anonymous blocks
  (a table's `{ { 0, 1 } }`). Comments are dropped; every other token, `=` and
  `;` is kept, so a generated text lexes to the same tokens the engine would
  read from the same tree. Of the 45,419 stock decl texts both archives hold,
  45,243 parse and every one round-trips token for token; the rest are 158
  render programs (shader code) and 18 malformed game files, which a mod that
  edits one is refused.
- **Kinds of member.** A key appearing at most once on both sides is a *field*
  (set or removed; blocks on both sides compared member by member). A block of
  `num = N;` and `item[0]` .. `item[n-1]` in order is an *ordered list*: its
  items are entries matched by their text (a conductor `aiTypeList` item is its
  `aiType` and `entityDef` pair), written back renumbered with `num` kept at N
  plus the items added (`syncGroups { num = 3; }` keeps its 3); sparse
  `item[i]` blocks (`damageGroups`) are fields. A block with a name
  (`anim "<clip>"`) unique on each side is a *named entry*. Repeated blocks that
  each carry a name field (a `name` member, or one under `object` or
  `object/object`: the `subGraph` and `node` entries of `aifsmmanager` states)
  are *keyed entries*, matched by that name without case and replaced whole.
  Any other repeated statement, and what a mod puts in a block the game leaves
  empty (the body's `aliases { }`), is a *bag entry* matched by its whole text.
- **Derived at build.** For each carried decl at a game name whose bytes differ,
  `mb_derive_edits` reads the game's text through the installed-content reader
  and records `sh_dt_diff` in the product block's `"edits"`: one element per
  file, `{"file", "stock" (a digest of the text diffed against), "counts",
  "ops"}`, an op being `set`/`remove` (path to the field, the statement),
  `list`/`unlist` (the list's path, the item), `keyed`/`unkey` (the container,
  the key word, the name, the statement) or `bag`/`unbag` (the container, the
  statement); a path is the member keys from the decl's top (`edit`,
  `item[4]`, `anim "clip"`). The copy stays in the bundle (hashed, uploaded,
  its needs scanned) and in `shadows`; it is classified a replacement but marked
  edited (`sh_mb_file.edited`, counted in `classes.edits`, never in
  `classes.replaces`, the Mods tab's replaced stock files or a doctor conflict). Refused: a copy
  or a game text outside the grammar ("does not parse as a decl (line N: ...);
  the product derives the edits from the whole edited copy"); a game text the
  reader cannot supply; a binary at a game name with other bytes ("<path>: the
  game ships this file and a mod cannot replace it with other bytes; delete
  your copy and the mod borrows the game's, or put your version under
  mods/<salt>/ and point your decls at it"; a binary at a name the game does
  not ship keeps "the game has no file of that name for it to replace"),
  except in a folder the product received (origin `download`, `delivery`,
  `migration`, or a thread that called `sh_mb_keep_replacements_here(1)`,
  where it stays a replacement, as does a decl whose game text cannot be read
  there). A grandfathered bundle keeps its shadows and derives nothing. The
  retired author keys `player`, `extends` and `replaces` are ignored with one
  line each and kept out of `extra`; a partial they name (an `extends` value,
  `player.bodyEvents`) is refused: "this is a partial; put the whole edited
  copy of <game file> at its game name".
- **Applied per map.** The index carries one synthetic file per game file an
  admitted bundle's edits name. For the listing, the sources are the bundles
  that serve and edit a game file (or carry one: a grandfathered package's own
  served copy at that name is diffed on the fly and is that package's edits),
  in row order, then editor-time bundles by key. `sh_dt_apply` takes the
  game's text and applies every source's operations in that order: an
  identical operation merges, additions union, a named entry two sources add
  becomes one block holding both texts' rows (identical rows once, a scalar
  both set otherwise keeps the earlier), and a field or keyed entry two
  sources set to different values is a clash. Each source's line closes its
  sum: `N edit(s) from X: A applied, M already held, C superseded by <key>'s
  choice` (or `by <key> (row order)`) for the operations that lost a clash.
- **The conductor.** The one rule per file: each `aiTypeList` item a source
  added whose `aiType` is a number that is not a stock `SPAWN_AI_TYPE_*` bit gets
  a bit assigned per map, the lowest free above `SPAWN_AI_TYPE_ZOMBIE_UNWILLING`
  (`1 << 19` up to `1 << 29`, eleven per map), in row order then by the name of
  the encounter decl whose `memberType` names the author's number, so two packs
  never claim one bit; every `snapAiEncounter` `memberType` and `snapAiScore`
  `aiType` of that bundle naming the author's number is served rewritten to the
  assigned bit (the same synthetic body mechanism, on the bundle's own file).
- **Clashes.** A slot is `<type>/<decl name>/<path>`
  (`entitydef/player/coop/edit/handsModelMPUseDoomHands`; a keyed entry adds its
  name, a named block segment is `word:name`). Two sources setting one slot to
  different values: the map's choice (`"choices"` in the manifest rows,
  `sh_modregistry_choices_set`, `sh_mods choose "<game file>/<path>" "<mod name>"`,
  the mod named by its name, its folder or its salt among the mods the open
  map uses, `sh_modserve_listed_mod`),
  else the first in row order, with one line naming the slot and the command;
  the Mods tab lists them from the declared rows' edits (`sh_dt_edits_slots`),
  a row resolved as serving resolves it: its own build, else the build the
  editor load substituted, else the installed build of its key
  (`mu_row_serving_entry`), so a map pinning a build that is not installed
  still offers the choice.
- **Serving.** The synthetic files belong to a pseudo-bundle
  (`snapmap-plus.registries`, one past the bundle count, never admitted, so no
  bundle walk sees it), never served by name or need; an edited copy
  (`ms_file.edit`) gets no key, so nothing looks it up, plans, refreshes or
  probes it. Generated texts are memoized per serving set and choices in the
  index arena and read through `scope_body` like a composed text. With nothing
  generated the game's file answers; the game's texts are read once per index;
  with map scope off nothing is generated. `sh_modserve_registry_report` prints
  one line per generated file, what each source's edits did, every clash, the
  merged body's totals (`player body: the merged body holds <clips> clips and
  <aliases> aliases`), and one `registry edits <path>` line per edited copy.
  The doctor lists the same copies as `edit source <path>: from <folders>;
  diffed at build against the game's text, generated per map that declares
  it, the copy is never served`, never as a conflict or a shadow, and its
  summary counts them apart: `N conflict(s), N shadow(s), N grandfathered
  shadow(s), N edit source(s) of N game file(s)`, so its conflict count
  agrees with the serving index's.
- **Choices.** A choice applies at once and is kept with the open map's
  manifest; the map's next save writes it on the row of the key it names; a
  load reads them from every row (first row per slot) and gives them to the
  index (`sh_modserve_scope_set_choices`) before the listing; a choice naming
  a key the save writes no row for is dropped with one line, and closing the
  map (`sh_modmanifest_reset`) clears the index's copy too. A choice, like
  `map add` and a `map remove` that drops only the row, marks
  the editor's map modified through the editor's own commit
  (`sh_editor_mark_modified`, one undo step, as a placed entity does), so
  leaving the editor asks to save; the wording stays "applies now; saved with
  the map's next save".
- **Rescans and gates.** A rescan that changes a generated text
  for the listing (a mod rebuilt with other edits) marks the path that serves
  it for refresh, as a changed decl is marked. A generated text is gated by the
  requirement gates of the first source with requirements whose edits changed
  it (bare-hands' cvar gates its player entity text), as a bundle's own file
  is. A folder built before derived edits is in an earlier shape and serves
  nothing; the serving index reads no partials.

### Migration of the retired overrides folder

Earlier releases kept packages in `overrides\`. The store tool's
`migrate`, after the store upgrade and the bin sweep, runs `modmigrate.c`'s
pass once when `overrides\` exists and the record's `migration` section does
not (or records an unfinished pass), with the `--doom` folder as the stock
bridge's install (`sh_modstock_set_install`):

- each direct child folder with `package.json` or `mod.json` is converted the
  way a carrier is (layout moved under `assets/`, files a bundle cannot carry
  left out) into a staging folder outside the store and installed by
  `modstore.c` as `mods\<folder>\` (its own name; with the tree's first 8 hex
  when another mod holds that name), `mod.json` and real files. The package's
  keys are the author part in the one shape: its name, requirements, strings and
  hud and its other keys, without the retired ones (`id`, `version`, `composes`,
  an era B `schema`) and with no
  product block. The staged folder is salted with `from` its id (or, for an
  era B pack without one, its folder name; `sh_ms_assign_salt`, which also names
  its strings `<salt>_<key>`) and built under the current rules with its
  replacements kept, so a pack that only built under the retired `legacy`
  relaxations is refused alone with its cause. Moved again into the folder an
  earlier pass made of the same tree (the record's `trees`), it keeps that
  folder's salt (`sh_ms_assign_salt_as`). Two folders with one id are both built,
  each in its own folder with its own salt, and the duplicate is reported;
- each delivered package, `overrides\map-<16 hex>\<package>\`, becomes a mod
  folder `mods\<id>\` the same way; the record's `trees` gives its earlier tree
  fingerprint (entries sorted case-insensitively, then by bytes; a `d` or `f`
  byte, the NUL-terminated relative path and the file's SHA-256) to it, which is
  how a map's carrier `delivery.<tree>` of the same delivery is answered when the
  store tool converts that map;
- a folder without a descriptor whose only package is one direct child folder
  (earlier releases read unmarked folders as groups) has that child migrated under the
  child's own id, and its line says so; a folder without a descriptor and with
  several such children and a ZIP are skipped with the reason (every other
  file, and a folder with no descriptor and no package child, is the loose
  layer below); a folder over
  512 MiB or 8192 files is skipped before it is read;
- a file byte-identical to installed game content becomes a stock pointer (see
  above): its file stays in the folder and an upload never sends it. Each
  folder's line says `N file(s), P of them stock pointer(s) to installed
  content, K KiB not stored`, the summary line totals them, and the record
  carries `stockPointers` and `stockBytes`. Measured on the owner's packs:
  453 of 472 files of the cyberdemon pack (9.1 of 9.7 MB), 306 of 388 of the
  doors pack (8.1 of 8.3 MB), and 6 of the Imp's 11 (renamed copies, 1.8 MB);
- the pass itself creates and changes nothing under `overrides\`; an empty
  `override-backups\` folder is removed and a non-empty one is left alone.

The loose step (`sh_modmigrate_loose_run`, after the pass) moves what SnapHak
and the first releases served without a package, with the strings document,
into one mod, `mods\my-overrides\`. The loose layer is every file directly in
`overrides\` that is not a ZIP and every folder with no descriptor and no
package folder one level down (`overrides\generated\...`, a tree of engine
paths); `strings\strids.json` is the flat `"id": "text"` object `strids.c` read
before (UTF-8, with or without a BOM). The loose files are converted as a
package is: placed under `assets\` at their paths below `overrides\`, files a
mod cannot carry left out and counted, copies of installed content made stock
pointers, with a descriptor of id `my-overrides`, name `My overrides` and the
document's keys as `strings.en`, and installed by `modstore.c` as
`mods\my-overrides\`, salted from `my-overrides` (its own strings `<salt>_<key>`;
a key the game or the product defines stays as it is, a string edit).
With the document alone the folder is `mod.json` (the same author part) and an
empty `assets\`. When `mods\my-overrides\` already exists only what it lacks is added
(files compared by path, keys case-insensitively) and one `MODS: migration:
kept my-overrides' <path or key>` line names each it has with other bytes or
text; an identical one is counted, not named. The changed tree is built again at
the next start or rescan by the rebuild rule, and its strings serve from that
build. A file merged into a folder the player made is built under that folder's
rules: a binary at a game name with other bytes than the game's is refused
there, and the folder's build is marked failed with that cause until it is
fixed (there is no last good build). A document that is not a strings
document is left with one line naming why. The record's `loose` section
(`complete`, `migratedAt`, `folder`) marks the step done; a loose entry or a
document created or written after `migratedAt` is due again. The step holds the
store lock throughout.

Then `sh_modmigrate_retire` moves `overrides\` to the Recycle Bin once every
entry of it is in `mods\` (a package entry the record marks done, a loose entry
the loose step moved), and `strings\` once it holds no document left to move,
both through `sh_ms_retire_sibling`, which acts only when a discard is set: the
store tool's Recycle Bin. An entry still skipped (a ZIP, an over-cap folder)
keeps `overrides\`, and one line names the first.

The tool runs the pass on its own thread, folder by folder, and writes the
record when it is done. The record lists every folder with its kind,
outcome, reason, id, version, store folder, bundle hash and tree, and a `trees`
map from tree to store folder; it says `"complete": false` until the pass
ends, and an unfinished record makes the next run do the pass again (every
step is idempotent). Each folder logs one `MODS: migration:` line and the pass
one summary line; a line that leaves files out names how many and how many of
them are `.bswf` movies (bundles carry movies under `generated/swf/` only, served
by `modswf.c`). A migrated pack that shares its old key with a delivered or
converted package (such as the `doors` packs) is converted below like any
copied folder: one keeps the salt and the other gets its own. Stock names are not checked during the migration because the
pass runs without the game's resource reader, so unresolved needs only warn, as
for a converted carrier.

Every writer of the store serialises on one recursive lock, `modstore_lock`
(`modstore.c`; `modsync_store_lock` takes the same lock): the mods folder's
named mutex (`src/common/store_lock.h`), which the record lock
(`sh_mb_record_lock`) shares and which the store tool holds for its whole run,
so a second game or the store tool waits instead of overwriting. The scan
records each root salt's folder in the record's `salts` section.
Each install and verification takes it; each migration folder, a download
install, `sh_mods install` and an export hold it around their
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
provides its assets again"). The list is computed per key: when another build
of the key stays installed (`install.keptBuild`), only the Toybox rows no
staying build adds are listed, and the saved-maps line becomes "Another build
of this mod stays installed (mods\<folder>); maps that use the mod open with
it."; the native delete
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
free. Closed maps are not edited. Remove from map and Uninstall are refused
while Play runs or a map loads (the editor's deactivation reason, the engine's
load state). Remove from map never leaves objects without their row: with
placed entities the row (and with the last mod the search tag) is dropped by
the same tick once the delete has committed and a fresh read of the map finds
none of the mod's objects; objects still there keep their row.

Accuracy: the page renders only from the documents the backend returns. Each
action is answered with its result and then a fresh document whose map object
is read again from the editor, so the confirmation lists exactly what the dry
run computed and the modal redraws from what the backend found afterwards.
The console verbs `sh_mods map`, `sh_mods map add "<mod folder>"`, `sh_mods map
remove "<mod folder>" [--yes]` and `sh_mods uninstall "<mod folder>" [--yes]`
share the dry runs and the actions and name mods by their names and folders;
an uninstall, from the tab or the console, removes the one folder it names (the
console takes the player's own before a map's copy, `mu_uninstall_target`), and
another folder of the same mod stays. Adding a mod to the map changes the
current manifest only; the
next save writes its row, so it publishes, downloads and carries the `mods` tag
like a placed mod's.

### Published maps and missing mods

A published map is native by default: the product touches its launch only when
its text carries a manifest row (in its member) the asset rule does not find served (no
installed folder at the row's hash, also when the key is installed at another
build, or one that lacks a name the row lists). Everything else reaches the
engine unchanged: vanilla maps, and maps whose rows the installed folders at
their hashes serve.

Identification. The `DeserializeFromJson` detour reports each parse with the
map object it filled and a classification by the engine caller's return
address: `PublishedMapRead`+0x12A is the offline read of the cached `map.decl`,
`SnapMapReadSizeLimit`+0x5D the profile reader under `PublishedMapComplete`;
anything else (editor, My Maps, a rawmap, the product's own resume) is another
parse. After the manifest step and the native parse,
`sh_modpublished_parse_seen` records beside the kept rows the kind of read, the
map object it filled, the frame it ran in and, when a named bundle is missing,
a copy of the text. `sh_modpublished_last_parse` exposes the record.

Hold points. The completion that follows a read receives the same map object
in the same frame (the main-thread tick after every native frame ends one), so
each gate uses the record whose map is its map and whose frame is the current
one. The classification is reported, not required: a second detour on
`DeserializeFromJson` that rewrites the return address (a debugger's, an
overlay's) makes every published read look like another parse, and gating on
it let every map with missing mods through.

| Gate | Vulkan / OpenGL RVA | Steal | Resume |
|---|---|---|---|
| `OnSnapPlay` read completion | 0xFA4A80 / 0xFA3FB0 | 18, the cookie load at +11 rewritten | `(manager, NULL, metadata, map)` |
| Browser published play | 0xFD1E30 / 0xFD1320 | 20 | `(browser, NULL, metadata, map)` |
| SnapMap feed read completion, play action only | 0x112B440 / 0x112A5F0 | 14 | `(widget, request, metadata, map)` with a request whose action int (+8, engine `operator new`) is 3 |
| `PublishedMapCacheReady` (joiner) | 0xFA1D60 / 0xFA1290 | 15, stack-probe call at +7 | `(manager, NULL, metadata, map)` after the lobby host's item is re-checked |
| `CacheMapForLobby`, warm cache (joiner) | 0xF9A170 / 0xF99620 | 20 | the function's own cold branch: wait text, CacheReady and error callbacks, `PublishedMapRead` |
| `LaunchLocalSnapMapForOfflinePlay` (My Maps) | 0xF9FCF0 / 0xF9F220 | 22 | `(manager, model, map)` when the model is unchanged |

None of these completions reads its request argument except the feed's, which
reads only its action int (1 details, 2 author, 3 play) and frees it. A held or
refused feed play still runs the completion with the action set to 0, which
clears the feed's wait state, frees the int and launches nothing. The
warm-cache branch never parses, so it uses the manifest this session saw for
that item and revision at an earlier gated completion; an item never seen is
not gated.

The launch boundary. Every published play ends in `PublishedMapLobbyLaunch`
(manager, flag; 0xFA0770 / 0xF9FCA0, steal 18) or `PublishedMapOfflineLaunch`
(manager, metadata, map; 0xFA0400 / 0xF9F930, steal 17); the install checks that
browser play calls them at +0x9C and +0xAD and the feed at +0xE3. Each gate
consumes the record it decides on, so a record of the current frame that is
still there at a launch, with rows the asset rule does not serve, belongs to a
completion no gate knows. `sh_modpub_launch_gate` holds such a lobby launch with
the same question (its resume runs the manifest step on the retained text and
calls the launch with its flag, when the screen it was held on is still
current) and refuses such an offline launch, which has no map object left to
resume with, with "This map needs mods that could not be prepared. The map was
not loaded." A launch without a parse of its frame (the lobby's own relaunch
from the map cache) is not checked. The level load at START MISSION builds from
the cached map and does not parse again, so this boundary is the last point at
which a missing mod can be caught.

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

The map's place at the gates. Before anything counts what is missing, each
gate decides the place of the map it holds and counts its rows there without
changing what the loaded map serves (`sh_modserve_row_served_in` through the
`row_state` op): a stranger's published map's place is the one the record gives
its item (`sh_ms_place_of`), else a free one its title names, noted for the
item (`sh_ms_place_dir`, `sh_ms_place_note`, the `item_place` op); a map this
machine published (a claim keyed by the item, or `sh_modpublish_item_linked`)
has none; a My Maps play, a launch no gate saw and a rawmap use the place their
text records (`sh_carrier_place`); a prefab placement has none (DECISIONS 12).
A gate that lets a map through, and a hold that resumes it, make that place the
loaded map's (`place_set` op) and the place its launch serves from
(`sh_modpub_launch_place`, which the launch detours apply before
`sh_modui_scope_launch`); a refused launch restores the place the editor map had.
The Mods tab's tick returns the place to the root when the editor leaves its map
or a launched Play is over.

The denial gate (DESIGN-additive 2.8). Before the question, every hold (the
completions, the launch boundary, My Maps, rawmaps, prefabs) runs one job on
the modsync worker that asks the store about each missing bundle
(`modsync_probe_build`: `HEAD /mods/build/<hash>`, which also says how many
script files the build carries; whether a borrow names game
bytes this machine lacks is learned when the archive is unpacked, as the
fetch's `MODNET_E_GAME`). One function,
`sh_modpub_gate_decide`, then decides each bundle from three facts: what the
store answered (held, absent for 404 and 451, unreachable for a network
failure, a timeout, an unusable address, 429, 5xx or an unexpected response),
whether a borrow names game bytes this machine lacks, and whether the map is
published (every map-service hook is; a My Maps map, rawmap or prefab when a
claim this machine holds names the bundle, `modsync_claim_names`). Held and
installable is obtainable; any other answer denies the launch in one of four
causes, `sh_modpub_gate_wording`:

| cause | when | sentence (after "This map needs <name>, which ", the row's name, else its salt) |
|---|---|---|
| 1 gone | absent, published | "is no longer available: its author unpublished it, or it was removed. This is permanent on this machine." |
| 2 unreachable | the store did not answer | "could not be downloaded: " and the store's failure in words (`plain_reason.h`): "the mod store is busy; try again in a few minutes." for 429 and 5xx, "the mod store could not be reached; check the internet connection and try again." when nothing answers, "the address in mods.store answered, but not as the mod store; check the mods.store setting." for another server, "the mod store did not answer; try again in a few minutes." when the cause is unknown |
| 3 never published | absent, not published | "is not installed and was never published. Restore its folder from the Recycle Bin or load the map's rawmap ZIP." |
| 4 game revision | held, a borrow the game lacks | "needs a file this game does not hold (<source>): a different game revision. This is permanent on this machine." |

An editor open asks no store, so its note for a salt no root folder holds
(`sh_modpub_editor_substitute`) says only what this computer knows: "not
installed on this computer, so the editor opens the map without it. Playing
the map asks the mod store for it." The causes above, which depend on the
store's answer, are said where the store is asked. When the map's placed
entities name that mod's namespace (`mods/<salt>/`, read by
`sh_modmanifest_placed_names`), the engine's validator refuses the map
(`GetLocalSavedMapEdit`'s RepairAndMigrate, then its damaged-save dialog,
which the fault shield's save guard dismisses), so the editor cannot open it
without the mod. The note then says so and is also the one-button notice
raised once no gate took the parse (`sh_modmanifest_load_refusal`): "This
map needs <name>, which is not installed on this computer, so the editor
cannot open the map. Restore the mod's folder from the Recycle Bin, or play
the map from My Maps to get it from the mod store." (My Maps Play holds and
asks the store, above.) An editor open does not itself fetch: it has no hold
point before the engine's load.

A denial drops the hold before anything is asked or fetched: the first denied
bundle's sentence is the one-button notice (with "N more mods cannot be
obtained; see the console." when there are more) and every denied bundle gets
one console line. A prefab's sentences say "prefab" and "the prefab's ZIP
export". The title is the row's name, else its salt. The question's size is
the total of the archives' sizes the store reported (`HEAD`), else the rows'.
A fetch that fails after Yes
is decided by the same function (404 and 451, a network failure, and
`MODNET_E_GAME`, the download's own borrow refusal, whose source the fetch
keeps). A placeholder system (phase 6) would plug in at `sh_modpub_gate_decide`
with a verdict of its own and touch nothing else.

Yes starts one job on the modsync worker that fetches every missing build's
archive (`modsync_fetch_build_for`) and logs a progress line every two seconds. When the job is done, on a main
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
(for example "This map uses Hell Guards, which is not installed. The map was
not loaded."). The download (`modexport_fetch_build`) is unpacked where the
map finds it: into its place, `mods\maps\<segment>\<mod name>\`
(`modsync_fetch_build_for` under the record key `place:<segment>`; a published
item's title is an idStr at metadata +0x398 beside the id at +0 and the
revision at +0x30), replacing that place's earlier build of the same salt and
nothing else; or, for a map without a place and a prefab, at the root as the
player's own mod (`sh_ms_install_to_root`, from a copy unpacked in `%TEMP%`;
a place that already holds this very build is installed from there with no
download). The resume checks every row against the asset rule again, in the
same place.

### Rawmaps that name mods

A rawmap carries the same manifest rows as any saved map (its `snapmap-plus` member), and its JSON alone
loads only where those mods are installed: a placed entity whose mod is absent
makes SnapMap's own validator throw the player out with "map corruption". So a
rawmap is never handed to the engine while a mod it names is missing.

Loading. `sh_modpub_rawmap_missing` reads the rows and counts those the asset
rule does not find served (missing or incomplete). The editor reload, which
both **File > Load Rawmap...** and `sh_rawmaps load` use, checks on its own
frame before `LoadMap` (`sh_rawmap_prepare_reload` in `ef_service_reload`); the
text it read, with a `.zip` already imported, is what the parse then uses. That
reload is the only way a rawmap opens: the `DeserializeFromJson` detour replaces
the text of the editor's parse inside that one `LoadMap` call and of no other
parse, so a published map, a lobby map or a map opened from My Maps always loads
as itself. `LoadMap` names a saved map, and the reload borrows the id of the
player's newest complete save; the `ReadLocalSavedMapText` detour
(`editor_frame.c`) answers the read of that id inside that one call with a
placeholder, so the borrowed map's files are never opened. Before it, a borrowed
map the savegame manager failed to read was marked unreadable and the next My
Maps listing deleted it; without the detour, opening a rawmap is refused. A
reload that fails anyway marks the dialogs the engine raises about the borrowed
map as untrue (`shield_note_refused_load`). A reload request records the open
map and the load generation and is dropped on the frame if either changed;
nothing is kept for a later map.

The hold (`sh_modpub_rawmap_hold`, hook `SH_MODPUB_RAWMAP_LOAD`) runs the
denial gate first, as every hold does (see Published maps and missing mods).
When every missing bundle is obtainable, the player gets the question a My Maps
launch asks, and Yes fetches and installs them; the resume does not activate
anything (the rawmap's own parse does) but opens the rawmap again, through a
reload when the editor can take one; otherwise a notice says to load it again
from File > Load Rawmap once a map is open in the editor. That reopened load parses the text the hold checked
(`held_take` in `rawmap.c`), so a `.zip` is imported once, at the hold; any other
read of the source drops it and reads the file again. Otherwise the load is denied with the gate's
sentences; the asset rule's sentence for each row comes first on the console.
No and a failed download behave as for a published map. The gate is bound with the published-map gates; on a
build where they did not install, nothing is counted missing and rawmaps load as
before.

Transport off the game's thread. A ZIP's import (File > Load Rawmap,
`sh_rawmaps load`, `sh_mods import`), a save to a `.zip` and the companion
`.zip` beside a `.json` save read and write every file of the map's mods, so
each runs on a thread of its own (`rawmap.c`, one at a time; another request
meanwhile is refused with "Snapmap+ is still importing or exporting another
map; try again when it is done"). An import says "Importing <file> and its
mods; it opens when that is done."; the main tick (`sh_rawmap_poll_main`) then
holds the imported text and asks the editor to open the map again, which parses
exactly those bytes. The store lock is taken only to choose and install
builds, never while files are copied or read. Every path is UTF-8, held in a
heap string sized to it (the process heap, never an engine heap), and opened
through the wide calls in the long form (`sh_rawmap_path_wide`), so a path of
any depth Windows accepts loads, saves, exports and imports.

Saving. After every rawmap write of a map with at least one row (the File menu's
Save Rawmap and Save Rawmap As, and `sh_rawmaps save`), the export is written beside the JSON at the same path with `.zip`
(`rawmap.json` becomes `rawmap.zip`), through `modexport_write_text_report` on
the transport thread. The main tick that takes its result shows one toast and one console line:
"This map uses N mod(s); share <name>.zip, the .json alone will not load for
others." A save whose own path ends in `.zip` is the export already; a map with no
rows writes only the JSON. `sh_rawmaps status` and the File menu's status
(`lastJson`, `lastZip`, `lastZipBytes`, `lastMods`, `lastZipError`, and `mapMods`
for the open map) report what the last save wrote. A ZIP left beside a JSON by
an earlier save is not deleted when the map later stops using mods.

The ZIP (`modexport.h`) carries the map and each mod its rows name
(`<salt>@<build hash>`) as its folder, in the one shape of a mod folder:

```
<name>.json                the map text, its snapmap-plus member included, named after the ZIP
mods/<folder>/mod.json     the last segment of the mod's folder below mods\ (" (2)" when two
mods/<folder>/assets/...   carried mods share it), mod.json and carried files byte for byte
```

A mod in an organizing folder (`mods\campaign-interactables\campaign-interactables-1\`),
or in a map's place, is carried under its own folder name
(`mods/campaign-interactables-1/`). Each row's build is taken from the usable
folder the resolver names in the place the map text records: the root folder
of its salt at the row's build, then that place's; a map without a place takes
the root folder of the salt at whatever build it holds, so the export carries
the build the author plays, and its rows are moved to that build. There is no `export.json`, no `bundle.json` and no
`bundle.canonical.json`: the rows name the builds, and each carried `mod.json`
records its own salt and build hash in its builder block. A stock pointer is an
entry of `mod.json`; installed game content is never written into the ZIP, so a
pointer whose bytes an author's folder holds under `assets\` travels without
them.

Importing it (`modexport_read`; a carried folder no row names is left out)
changes nothing the loaded map serves. The map arrives with its mods: its
place is the ZIP's, `mods\maps\<ZIP name>\`, keyed `zip:<the ZIP's path>` so
two ZIPs with one name get two places, and when a carried folder would replace
that place's folder of its salt at another build (an earlier import of the ZIP),
the import takes a place of its own beside it instead, so a map saved from the
earlier import keeps its mods and a failed import loses nothing. Then:

- a carried folder whose row the resolver serves in that place (the root
  folder of its salt at the row's build, or the place's) is skipped and counted
  as already installed;
- each remaining folder is copied out of the ZIP into a temporary folder (with
  the bytes of a pointer the folder held filled from this player's own game)
  and installed into the place, `mods\maps\<segment>\<mod name>\`
  (`sh_ms_install_placed`); its replacements are kept, as for any received
  build. A prefab's export installs at the root instead
  (`sh_ms_install_from_folder`, DECISIONS 12). A mod that needs another carried
  mod is built after it;
- the built hash must equal the one the carried `mod.json` records; otherwise,
  and on any other refusal, exactly the folders this import installed are
  removed and the load is refused ("<name> in the export does not build to the
  build the map names", or "<name>, a mod this map carries, does not install on
  this computer (<the builder's reason>); nothing was installed");
- after an install the serving index is rebuilt, and the map text comes back
  recording the place (`sh_carrier_put_place`) when a row is served there, so
  the load that parses it, and every later load of a map saved from it, finds
  its mods.

An export in an earlier shape installs nothing and is refused with one
sentence, "This export was saved in a format this version does not read.": a
`bundle.canonical.json` anywhere or an `export.json` index (the bundle form of
earlier releases), a carried `mod.json` in an earlier shape (`SH_MB_SHAPE`), a
map in an earlier shape (`sh_map_old_shape`), a prefab whose rows name `id` and
`bundle`. A carried `mod.json` that is not built is left out with one log line.

A refused import reaches the player whole: the editor reload
(`sh_rawmap_prepare_reload`) prints and toasts "<file> was not opened:
<the importer's cause>." (a `.json` that cannot be read gets Windows' reason
through `plain_reason.h`), and the backend log line carries the same cause
uncut (every cause is a heap string of its own length). The map importer tells a map from a prefab by
the carried `.json`: an `idSnapMap` (or an object without `~type`) without a
`snapmapPlus` block is a map; a prefab's export is refused with "this is a
prefab's export, not a map's". `sh_mods import "<path.zip>"` installs the mods
and opens the map at once (`sh_rawmap_open_now`).

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
               "mods":[{"mod":"<salt>@<build hash>","name","size","names"}...],
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
`modserve_bundles_for_names_visit`, which names every admitted build that
provides one and the builds its needs reach. Each build is one row in the
manifest's form (`<salt>@<build hash>`, its name from the build).
Create writes the file the engine
serialized and then has the backend write a fresh block into it (`MODS_RECORD`:
empty description and tags, the rows, `[]` when there are none); a module
prefab's save writes the file with its block in one step. The Prefabs tab's
description and tag edits rewrite the block in place (`META_WRITE`), keeping its
summary and rows. No sidecar is written.

An earlier shape. A prefab with a `<name>.meta.json` sidecar and no block, or a
block whose rows name `id` and `bundle`, was saved by an earlier version: its
details are not read (`META_READ` answers `SH_PREFAB_META_EARLIER` with an
empty view), Load / Place, export and `sh_prefab mods` refuse it ("Prefab
<name> was not placed: it was saved by an earlier version of Snapmap+. Ask its
author to export it again."), and a description edit writes nothing. A prefab
with neither names no mods and loads as before.
A sidecar is never listed as a prefab, and it follows its prefab when that is
moved, renamed or deleted. Prefab paths and names are UTF-8 and every file is
opened with the wide calls.

The page and the slot see a prefab's details as one flat object, the view:
`{"desc","tags",...summary members...,"mods"}`. A block reads as a view and a
view composes into a block (`prefab_mods.c`). `META_READ` returns the view of
a file's block; the Prefabs list reads at most the last 1 MB of each file,
where the block is. `MODS_STATUS` names each row's mod by its name ("id",
else "an unnamed mod": a player never sees a salt), with its salt and build.

Resolving. Load / Place first reads the rows (`MODS_RESOLVE`). The rows
resolve in the open map's own context (the root folders, else the map's own
place); a placement never changes where the open map finds its mods
(decision 12), and neither does `sh_prefab mods`:

- a row the open map does not serve whose very build a place of the store
  holds (a downloaded or imported map's mods) is installed at the root first,
  when the root holds no folder of its salt: off the game thread, under the
  store lock, through `sh_ms_install_to_root` (the root folder takes the place
  folder's name and keeps its salt), the index is rescanned and one console
  line says "Prefab
  <name>: N mod(s) it uses was installed in your mods folder, where your own
  maps find it." The player is putting the mod to use in their own maps, and
  the map's download or import was the approval. A copy that fails refuses the
  placement with the cause;
- a module prefab naming a module no row provides and the palette lacks (DLC
  the player does not own) is refused with the module guard's toast and
  console line, below; nothing is fetched, because nothing can install it;
- with every row served (the asset rule), the rows are activated for the open map
  through the builds that serve them
  (`modserve_activate_bundles`) and each one the open map does not list (its
  scope listing, else its manifest rows) is attached as a map-level row through
  the Mods tab's Use in this map (`sh_modui_run(SH_MODS_ACT_MAP_ADD)`, given the
  folder of the build that serves the row, else its salt); the Mods tab
  shows the new row, and a row that could not be added gets one console
  line. Then the placement goes on unchanged;
- otherwise the placement is held by `sh_modpub_prefab_hold`, the rawmap hold
  with its own hook (`SH_MODPUB_PREFAB_PLACE`): the denial gate asks the store
  on the worker; when every missing bundle is obtainable the engine's Yes/No asks
  "This prefab needs N mod(s): <names> (<size>). Download and install now?", Yes
  fetches each archive (`modpublished.c` decides where; decision 12 wants the
  root), and the ready callback runs the step above;
  only rows the asset rule reports missing or incomplete are asked for; the
  Prefabs tab, polling `MODS_POLL`, then places the prefab through its ordinary
  Load / Place. Otherwise the placement is denied with the gate's prefab
  sentences ("This prefab needs <title>, which ... The prefab was not
  placed."). No, a failed
  download and a bundle that will not install end as for a map, with "The
  prefab was not placed.";
- with the gates unbound (a build where the published-map gates did not
  install) the same refusal is printed and toasted without asking the store.

One hold runs at a time: a second placement while one waits is refused.

Export. `sh_prefab export "<name>" "<path.zip>"` and the tab's Export always
write one ZIP, also for a prefab that uses no mods, in a map's layout
(`modexport.h`):

```
<name>.json                the prefab file, its block naming the builds carried
mods/<folder>/mod.json     each mod the block's rows name, as its folder (the last segment
mods/<folder>/assets/...   of its folder below mods\), mod.json and files byte for byte
```

There is no `export.json`, no sidecar and no bundle manifest: the prefab's own
block is the index, and each `mod.json` carries its salt and build hash.
`<name>.json` is the file's bytes as they are on disk. A prefab with no block
and no sidecar, one saved before rows were recorded (its rows are then the
closure of its text) and one whose rows pin another build of a root salt are
carried with their block written for the rows the ZIP carries; the file in the
library is not changed. The Export dialog proposes
`<name>.zip`.

Import. `sh_prefab import "<path.zip or path.json>"` and the tab's Import (the
Open dialog's "Prefab exports (*.zip, *.json)") take the ZIP, or a `.json`
inside an extracted export folder: that folder is the export, the picked file is
the prefab and a `mods/` folder beside it holds the mod folders. The importer
takes the one top-level `.json` (an `idSnapEntityPrefab`, or an `idSnapMap` that
carries a block), first adds the prefab under the `.json`'s file name (a
prefab of that name with the same bytes is left alone; one with other bytes is
kept and the import takes "<name> (2)" and so on), then installs each carried
`mods/<folder>/` whose `mod.json` names a build the block's rows name, exactly
as a map's import does (see [Rawmaps that name mods](#rawmaps-that-name-mods)):
skipped when something serves its row, otherwise installed at the root as the
player's own mod (decision 12: a prefab's mods are put to use in the player's
own maps; the import is the approval), and removed again when it
does not build to the hash its own `mod.json` names; a carried folder no row
names is left out. When the install fails the prefab it added is deleted
again, so a failed import leaves nothing. An import that finds everything
already there writes nothing. A row the export does not
carry is named in the result ("It also needs <name>, which the export
does not carry."), and Load / Place asks for it as above. Each importer refuses
the other's ZIP, told apart by the carried `.json` ("this is a map's export, not
a prefab's"), and the prefab importer also refuses a map's JSON and two prefabs
in one ZIP. An export in an earlier shape (any `bundle.canonical.json`, in a
ZIP or an extracted folder, an `export.json` index, a carried `mod.json` or
rows in an earlier shape) is refused with "The prefab was not imported. This
export was saved in a format this version does not read."
`sh_prefab mods "<name>"` lists the block's rows by their names and whether
each is served in the open map's context, held by a downloaded map's place
(installed at the root by the placement), or neither, with the asset rule's
sentence. It changes nothing.



### Publishing a map with mods

What is sent: the builds the published map names in its manifest rows
(`<salt>@<build hash>`), one archive per build the store lacks, and only
those, at publish time; of a mod the map uses only part of, the row names the
cut of the files it needs (see [Part of a mod in a published
map](#part-of-a-mod-in-a-published-map)). The `SerializeToJson` detour reports each save's
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
uploads every build the rows name (`modsync_upload_builds`, see [The mod store
transport](#the-mod-store-transport): a HEAD per build, an archive PUT for each
the store lacks, one claim) under a provisional ref `publish-pending-<16 hex>`,
and a main tick resumes the original `PublishAfterSave`. A claim the store
refuses with 409 (a release of the same build, by another publish's
unpublish, deleted its archive between the HEAD and the claim) uploads again
once (`mp_upload`); `sh_mods push` does the same. The store checks each
archive at its PUT and answers a refusal with its first cause, which stops the
publish. The rules of the store's that the
builder would otherwise admit (an author key outside `^[a-z][a-z0-9_-]{0,63}$`
such as `displayName` or `$schema`, a control character such as a tab in the
name or author) are mirrored as build warnings in one table,
`MB_STORE_RULES` in `modbundle.c`, so the author hears them at the build
rather than at publish. They are derived again whenever a built `mod.json` is
read, so they also reach `sh_mods doctor` (one "warning i of n" line per
bundle), each rescan (one line per mod with warnings), the Refresh result
(`warnings`, `firstWarning`) and each mod of the Mods tab's payload
(`warnings`: the first `SH_MB_WARNINGS_KEPT`, in plain words). The builder's other warnings are its build report's. A publish whose save
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
paths reach, carries the item id and revision. The worker claims the build
set under the item id. On a republish the stored secret re-claims that ref,
which replaces the set; a republish without rows releases the
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
section, an upload or a claim release alike. A release the store does not
take (a busy store at unpublish, say) is queued under its ref rather than only
logged, since a leaked claim keeps its builds on the store; it replaces a
queued upload of the same ref. Queued releases are retried after the next
store call that succeeds and at startup. At startup the backend first forgets
the queued work of provisional refs left by a closed game (their publish never
completed), then retries the queue once on the worker, then releases each
provisional claim, except one an item's queued claim still waits on: the item
claim records its provisional ref when it is queued, and the provisional claim
keeps the builds on the store until that claim succeeds. Releasing it first
would let the store delete a build the retry then has to upload again. The
worker logs the counts.

### Part of a mod in a published map

A published map carries, of each mod it uses, only the files it needs
(`modslice.c`, `modsync_cut_row`), so a player of a map that places one
object of a large pack downloads that object's files and not the pack. Nobody
sees a word for it: the row names a build like any other, the publish question
says what goes up ("This map uses 2 mods and needs 1.2 MB of their 147.0 MB
of files. Upload those ..."), and the download question states the archive's
size.

The cut point is the publish's own save. `PublishStart` arms it
(`sh_modpublish_publish_started`); the `SerializeToJson` step
(`sh_modmanifest_on_save`) hands every row it writes to the bound slicer
(`sh_modmanifest_set_slicer`, `mp_slice_row` in `modpublish.c`), which cuts it
only while the arm is fresh. Every other save, every rawmap and prefab
export and the open map's own record keep whole builds, and the player's own
maps always follow the root build (a cut row's own map resolves to the root at
any build, like any row of an own map).

The needed set (`sh_slice_needed`) is static and deterministic, from the
row's placed names and the root folder's build. A map-level row (attached:
the map uses the mod as a whole), a build with scripts (`sh_mb_sliceable`: a
script builds names while it runs) and a row with no placed name ship whole.
Otherwise the walk starts from every file a placed name names, the Toybox rows
(`snapEditorEntityDef`) of the mod whose bytes name a placed name, and what
players of an earlier cut asked for (below); always kept are every file at a
game name that is not an editor tweak (the edited copies, whose edits apply per
map, and a received build's replacements) and every decl of a type the game
finds by number rather than by name (`snapAIScore`, `snapAIEncounter`,
`snapAIEncounterTable`, which the conductor numbers); never kept are editor
tweaks and the closure pointers the cut's own build derives again. From each
kept file the walk follows every need the build recorded (companions and scene
spawns included) and every name its bytes spell under `mods/<salt>/`, text and
binary alike (a decl's tokens, a mesh's materials, a movie's dictionary): a
spelled name, read with its last segment's extensions and anything after `$`
cut and every character but `[a-z0-9/._]` as `_`, reaches the file of that
name and the file of each of its leading folders (a node path inside an
animWeb names the animWeb). A stock pointer the folder holds (a borrow or a
surface) is walked by its needs alone, its bytes being the game's. A reached
file brings its whole unit: every member of a virtual-texture family (so a
mod's texture board travels whole), every sound bank once a sound decl is
kept, every render program once one is, and a module's every file below its own
name. A reached file that cannot be read, or one of a kind the walk does not
read, ships the whole build. When the needed set holds every carried file
but the editor tweaks, or its files and the source's list of files
(`source.json`, below) come to more than three quarters of the whole build's
carried bytes, the whole build ships: a second archive of nearly the same size
saves a player little, and the cut costs the save its build.

The cut (`sh_slice_cut`) stages the kept files under
`mods\.slices\<cut hash>\` (a dot folder no scan reads), with `mod.json`
holding the source's author part plus the author key `slice-of`
(`SH_MB_SLICE_KEY`, the source build's hash) and `source.json`
(`SH_MB_SLICE_SOURCE`, the source build's canonical manifest, whose SHA-256 is
`slice-of`) beside it; builds it against the game alone (no other mod); and
proves it: the build succeeds, every carried file is the source's own bytes,
and every need a kept file resolved inside the source resolves inside the cut.
Any failure ships the whole build with one `MODS: publish: <mod> goes with the
map whole: <why>` line. Because `slice-of` is an author key it is in the
canonical manifest: two maps that need the same files of one build name one
hash and share one archive, and the key cannot be edited away. The row is
`{"mod":"<salt>@<cut hash>","of":"<source hash>",...}` with the cut's carried
size. The publish's upload finds the staged folder by its hash
(`msy_slice_find`), sends `source.json` with the archive, and removes the folder
once its upload and claim are done; a queued upload keeps it, and the launch
removes any other (`modsync_slices_tidy`).

Serving: the one place resolver takes the root folder of the salt at the row's
hash or at its `of`, so a player who has the whole mod at that build downloads
nothing; else the map's place. A cut is never a root folder: the scan refuses
a folder with `slice-of` at the root (`SH_MB_SLICE_ROOT`, in `modstore.c` and
the serving index), Add to my mods and every install to the root refuse it
with the same sentence, and the Mods tab offers no Add to my mods for it
(`part`). A download verifies the archive as any build and also that its
`slice-of` is the row's `of` and its `source.json` hashes to it, and keeps
`source.json` beside `mod.json`. A map without a place (My Maps) cannot use a
cut: the fetch says so in one sentence (`MODSYNC_PART_ONLY`) and downloads
nothing.

The miss: the provider (`ov_open_body`) calls `sh_modserve_note_miss` for
every request it found nowhere. One under `mods/<salt>/` of a mod the open map
declares by a cut, which the source build had (its `source.json` lists it), is
a defect of the cut: the map plays on if the game can, and the request is
noted once per cut with the name of the load that asked for it (the main
thread's open loads, `sh_modserve_attrib_enter`), logged (`<mod> came with
this map without <request>, which <load> asked for; the map goes on without
it`), listed by `sh_mods status`, and posted to the store
(`modnet_report_asked`, `POST /mods/build/<cut>/asked`, a pool thread; only
the request and the asking name are sent). The author's launch job asks the
store, for every claim it holds, what players asked for
(`modsync_asked_refresh`, `POST /mods/asked` with the claim's secret) and keeps
it in the record's `asked` section; the Mods tab's map view lists each under
"Missing for players" beside the published map ("Players of this map were
missing <file> from <mod>, which <load> asks for. Publish the map again and it
goes with the map."), and the next publish's cut of that mod seeds the walk
with every request asked of it (`modsync_asked_for`), so republishing carries
it; the re-claim replaces the earlier cut, which the store deletes once nothing
claims it.

### The mod store transport

The store keeps one object per build (ONE-SHAPE rule 5): a stored ZIP
(`modzip.c`, method 0, ZIP64 where a value needs it) named by the build hash at
`/mods/build/<hash>`, holding the folder's `mod.json`, the whole file, and
`assets/<request>` for every carried file its block lists. Stock pointers are
never in it: their bytes are the player's own game. `modnet.c` speaks the
routes: `HEAD` (held, with the archive's size and `X-Mod-Scripts`, the
number of script files the build carries, or 404, or 451), `PUT`
(`application/zip`, Content-Length, streamed from a file; 201 new, 200
already held, 422 with the store's first cause), `GET` (streamed to a file a
chunk at a time with progress, of any size), `POST /mods/claim`
(`{"builds": [...], "ref", "secret"?}`, one claim per published item, any
number of builds, 409 when a build is not stored) and `POST /mods/release`,
after which the store deletes every build no claim names. There is no blob
store and no manifest of its own: two mods with the same files are two
archives. Part of a build (a published map's cut, see [Part of a mod in a
published map](#part-of-a-mod-in-a-published-map)) is an archive like any
other plus `source.json`; `POST /mods/build/<hash>/asked {"request","by"}`
(`modnet_report_asked`) tells the store a player's game lacked a file of such
a part, and `POST /mods/asked {"claim","secret"}` (`modnet_list_asked`) lists
those reports for the claim's holder.

Nothing limits a build (DECISIONS 26). One request body is the platform's
limit (100 MB on the Worker's plan) and one connection is slower than several,
so an archive larger than `MODNET_PART_SIZE` (32 MiB) goes up in parts of the
size the store names (the Worker's `PART_SIZE`, 16 MiB), four at a time
(`MODNET_PART_WORKERS`), each on its own connection and in any order
(`mn_send_parts`: a failed part stops the taking of new parts, the ones on
their way finish, and the next start sends only what the store lacks):
`POST /mods/build/<hash>/parts {"size"}` starts the upload or resumes the one
of that build and size and answers `{"partSize", "parts", "have"}`; `PUT
/mods/build/<hash>/parts/<n>` (`application/octet-stream`, `X-Build-Size`)
sends part n, each but the last exactly `partSize` bytes, read from the
archive file at its offset; `POST /mods/build/<hash>/complete {"size"}` joins
them, and the store checks the whole archive by the same rules as a
one-request upload before it serves it (the answer waits up to 15 minutes
for that read). A part the store holds is never sent again: a failed part
starts the upload again (up to four starts in one call) and the store's
answer names what it holds, and a later publish resumes the same way. The
store's per-address hourly limits (abuse protection, not a mod limit) count a
publish in parts as one upload when it starts.

Upload (`modsync_upload_builds`) takes the map's rows as `<salt>@<hash>`. Each
build is found in the mods folder (a root folder at that hash first, else a
map's place); a salt the root holds only at another hash is refused with
"<name> was built again since the map was saved; save the map again, then
publish it", and a salt the machine lacks with "the map uses a mod this machine
does not have". HEAD asks whether the store holds it; only when it does not is
the archive written to a temporary file (`modsync_write_archive`: every file
read and checked against the block, "<name>: <file> changed since the mod was
built; open the Mods tab and Refresh" otherwise) and sent, in one request or
in parts. Then one claim names every build, however many the map names.

Download (`modsync_fetch_build_for(row, place)`): when the root holds the
row's salt at the row's hash, or the map's place holds it, nothing is
fetched. Otherwise the archive is streamed to a temporary file and
`modsync_install_archive` installs it. While the hold downloads, the game's own
progress line (the manager's progress widget, which CacheMapForLobby fills
with its wait text while the game downloads a published map) shows
"Downloading <mod> (i of n): x of y MB, p%.", its text replaced in place every
two seconds (`show_wait` again; `hide_progress` when the download ends), and
the console prints the same line. The game's dialog cannot change the text it
was raised with (ShowDialog copies the descriptor's text once), so a download
still running after two seconds raises it once, with fixed words: "Downloading
the map's mods; the progress shows on screen and on the console. Keep
downloading? No cancels it and installs nothing more." Yes closes it for good;
No sets the job's cancel flag, the download stops at its next chunk, its temporary file is
deleted, no further mod is installed, and the map is not loaded ("The
download was cancelled. The map was not loaded."). An install itself is
atomic (staging folder, then one rename), so nothing is ever half installed.
The archive's checks are: its `mod.json` must load and name the
row's salt and build hash, every carried file must be an entry with the size
and SHA-256 the block records, no other entry may be present, and every stock
pointer must resolve against the installed game (`MODNET_E_GAME` otherwise).
Any mismatch installs nothing: "the mod store sent a build that does not match
the map". The files are unpacked into `<place folder>.staging-<pid>` beside
the target (`sh_ms_place_folder`) without the store lock, then swapped in,
replacing the place's earlier build of the salt; the place is noted in the
record (`sh_ms_place_note`) and the serving index rescanned. Two maps that
name one build each unpack their own copy. Every build a map names is
installed, editor-tweaks builds included.

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

## Engine event registry

`event_registry.c` backs `sh_event`: it reads the registry through the decl
manager's event interface (`+0x90`; FindByName `+0x10`, Count `+0x28`,
GetEventCommand `+0x08`) and each record's format (`+0x08`), types (`+0x10`)
and argument names (`+0x18`). On this build it holds the 1611 events of the Timeline tab's
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
thread; when an earlier redirect is still being recovered in the same Frame
call, where a second throw reaches the game-loop catch that ends the process;
and when the faulting stack does not unwind to Frame. An input the shield
cannot read keeps the earlier behaviour. The decision is logged as a `declined`
shield line with its stack.

A fault during a map load is recovered through the same redirect.
`idLoadScreen::LevelLoadCompleted` is the only code that stops the
loading-screen thread (the `load_screen` global, thread byte +0x4b2), and an
error thrown out of a load skips it, the engine's own recoverable errors
included. CancelMapChange's renderer sync and menu load then run while that
thread still draws the load GUI: a deadlock in a Play load, or a race on the
shared menu GUI (issue 132). `load_screen_stop.c` detours CancelMapChange
(common vtable +0x28): on the engine main thread with the loading-screen thread
still running it calls `LevelLoadCompleted` first, the engine's own order on a
load that completes (stop the thread with no lock held, sync the renderer, free
the load GUI), then runs the original. Both functions are signature-resolved and
the stolen prologue is verified at startup on both renderers
(`tests/load_screen_stop_test.c`). Only while that detour is armed does the
policy redirect a fault with the loading-screen thread running; otherwise it
declines it as before.

Redirects are bounded per incident, not per session: an incident starts at its
first redirect and lasts 30 seconds, and may redirect four faults
(`SHIELD_INCIDENT_REDIRECTS`, `redirect_policy.h`). An incident that keeps
faulting past that is left to the fatal handlers; a later, separate fault starts
a new incident, so a long session survives any number of recoveries. An access
violation is classified first-chance by the vectored handler. Any other hardware
fault (a divide, a floating-point or misalignment fault, an in-page error) is
redirected only at second chance, from the unhandled-exception filter
(`shield_second_chance`), so the engine's own `__except` blocks see it first.
The shield also changes `FatalError`'s level byte from 7 to 6 at startup
(through `code_patch`, the byte compared first), so a fatal engine error thrown
inside Frame takes the same recoverable path; direct dispatcher calls and
failures outside Frame still exit.

What the player reads after a recovery is one fixed sentence,
`SHIELD_RECOVERED_TEXT` (`recovery.c`): "Snapmap+ caught a problem in the game and
returned you to the menu. Unsaved changes to this map were lost." It is the text
of the `Error(6)` the redirect raises, so the engine's own error dialog shows it,
and the crash record carries it; the fault's code and address go to
`shield_faults.log` only. When the fault leaves the editor open, `recovery_tick`
drives the editor's own exit to the map browser and answers its exit prompt Yes,
which is what the sentence says.

DOOM never deletes a saved map it cannot read. Each time My Maps is listed the
savegame manager reports the slots it could not read (a failed `.verify`
signature, a file it could not open or inflate), and
`idSnapBrowserHelper::DeleteBadSaveSlots` (VK 0xFCEE30, GL 0xFCE320) deletes
every one before it queues GDM 0x8D, "removed corrupt data". The fault shield
replaces that function (`saved_map_guard.c`): the slot stays in the saved games
folder untouched, its files are copied to
`%LOCALAPPDATA%\snapmap-plus\kept-maps\SNAPMAPS<id>\` (a copy holding other
bytes is never overwritten; the next goes to `SNAPMAPS<id>-2`) and each copy is
compared byte for byte. The player reads one sentence, once per slot per
session, naming the map and either the verified copy or, when no copy could be
made, the folder its files are still in. The player's own Delete in My Maps is
another engine path and still deletes. Without the signature the guard is off
and the shield log says DOOM deletes saved maps it cannot read.

The damaged-save guard dismisses the engine's offer to delete a save it could
not load (GDM 0x1D, through the shell's own ClearDialog, so no button action
runs and nothing is deleted) and then, once that dialog has left the queue,
raises one engine dialog of its own, `SHIELD_DAMAGED_SAVE_TEXT`: the map could
not be opened because DOOM could not read its save file, and Snapmap+ did not
let the game delete it. After a load Snapmap+ itself refused (a map in a shape
this build does not read, a map whose mods are missing, a failed rawmap reload),
the engine's "damaged" notice (0x34) and its offer to replace the map with its
published version (0xB0) are dismissed the same way for the next 600 frames:
the map is sound and Snapmap+ has already said why it did not open.

The serialized-entity cleanup guard permits an empty moved handle, as the native
snapshot writer already does. It skips that handle's index write and preserves
the engine's list reduction and removed-entity reset. Startup requires a unique
instruction signature and exact-byte verification on both supported renderers.

Failures the player or author reads are worded by one table, `plain_reason.h`
(`sh_reason`): a Windows error code becomes what happened to which file and
what to do (access denied, a file open elsewhere, a full disk naming the drive
and the bytes needed), a mod store status becomes the store's state and the
next step, and a file-name character the game cannot read is named with its
code point. The store, builder, importer and download messages route through
it; none carries a bare "error <n>".

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
