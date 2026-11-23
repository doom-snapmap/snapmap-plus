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
| Store tool | Migrates a mods folder of earlier releases. The installer does not run it (`storeConversion`, off: decision 30 freezes it, and migrating releases before beta.26 is a future installer's job). | `src/store/store_tool.h` |
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
holding `mod.json` and folders named for what its files are (see
[The mod folder](#the-mod-folder)); the product maps each file and every name
its texts write to the engine's request paths and names under the mod's salt.
Mods reach other machines two ways: the units a published map's rows name
(see [Units](#units)) are uploaded to the Community API's mod store when the
map is published, after one question that sizes what the store lacks, and a
player who opens that map gets the units it needs with no question, under the
game's own progress line, each checked against the hash the map names
(`modnet.c`, `modclaim.c`, `modpublish.c`, `modcache.c`, `modpublished.c`; see
[The mod store transport](#the-mod-store-transport),
[Publishing a map with mods](#publishing-a-map-with-mods) and
[Published maps and missing mods](#published-maps-and-missing-mods)); that
transport is for publications only. An exported rawmap or prefab is a ZIP that
carries each mod exactly as it sits in the mods folder, its folder with
`mod.json` and its folders by kind (`modexport.c`), and importing one is the ordinary
install of those folders, so sharing stays offline; every rawmap save of a map
that names mods writes that ZIP beside the JSON (see
[Rawmaps that name mods](#rawmaps-that-name-mods) and
[Prefabs that name mods](#prefabs-that-name-mods)). Plain saves never upload. The retired
`%LOCALAPPDATA%\snapmap-plus\overrides\` tree is read once, by the store tool's
migration below, never served or written, and moved to the Recycle Bin once
everything in it is in `mods\`; no `package-cache` or `package-staging`
folder is created.

### The mod folder

The author's guide is [authoring.md](authoring.md); this is the contract (`modauthor.c`, R-NM1). A mod folder is
`mod.json` and top-level folders by kind. Each file's path in the folder maps to one engine request (`sh_ma_request`;
the inverse, `sh_ma_path`, names a request in causes and the Files view), with the salt inserted after the kind's
engine root:

| Folder (extension) | Engine request |
|---|---|
| `materials/`, `effects/`, `environments/`, `particles/`, `entities/` (`.decl`) | `generated/decls/{material,fx,env,particle,entitydef}/mods/<salt>/<rest>` |
| `sounds/` (`.decl`) | `generated/decls/sound/mods/<salt>/<dirs>/<salt>_<leaf>.decl` (the leaf is the Wwise event name every mod shares, so it takes the namespace; a text name `sounds/x` is `mods/<salt>/<salt>_x`; `sh_ma_request`, `sh_ma_engine_name`, `sh_ma_path`) |
| `sounds/<stem>.bnk` | `sound/soundbanks/pc/<salt>_<stem>.bnk` (served under `<salt>_<stem>.<sha16>` as before) |
| `meshes/` (`.decl`, `.bmd6model`, `.bmd6skl`, `.bmodel`, `.dmodel`, `.pmodel`) | `generated/decls/md6def/`, `generated/basemodel/md6/`, `generated/skeleton/md6/`, `cooked/model/`, `generated/discreteanimation/`, `generated/` + `mods/<salt>/<rest>` |
| `animations/` (`.bmd6anim`) | `cooked/anim/md6/mods/<salt>/<rest>` |
| `images/` (`.bimage`); `atlases/decals|lights|transparency/` | `generated/image/`; `generated/decalatlas|lightatlas|transsortatlas/` + `mods/<salt>/<rest>` |
| `movies/` (`.bswf`, `.bimage`) | `generated/swf/mods/<salt>/<rest>` |
| `scripts/`, `scripts/lib/` (`.lua`) | `mods/<salt>/scripts/<rest>` |
| `programs/` (`.decl`, `.bin`), `programs/permutations/`, `programs/spirv/`, `programs/spirv/permutations/` | `generated/decls/renderprog/`, `generated/renderprogs/`, `generated/renderprogs/permutations/`, `generated/spirv/`, `generated/spirv/generated/renderprogs/permutations/` + `mods/<salt>/<rest>` |
| `map-modules/` (any), `map-modules/palettes/<p>/`, a `.baas_*` | `maps/modules/mods/<salt>/<rest>`, `maps/modules/palettes/<p>/mods/<salt>/<rest>`, `generated/maps/modules/mods/<salt>/<rest>` |
| `virtual-textures/` (`.mega2`, `.pages`) | `virtualtextures/mods/<salt>/<rest>` |
| `decls/<type>/` (`.decl`; not a type with a folder of its own) | `generated/decls/<type>/mods/<salt>/<rest>` |
| `native/<request>` | `<request>` (DOOM's own path for a file the mod changes; refused under `mods/`) |

A file in a top-level folder that is none of the mod's (`assets/` and `game/` included) is refused per file with one
of two sentences (`sh_ma_outside_cause`): "<path> is in none of the mod's folders, and DOOM has no file at that path"
with the folders and an example for each, or, when DOOM holds that path, "<path> is DOOM's own file, outside native/"
with what to do. A `native/` file at a path DOOM does not hold is refused with the folder of its kind
(`sh_ma_kind_path`); a non-decl `native/` file at a path DOOM holds with other bytes is a replacement (class REPLACE,
served only to maps that use the mod), and one identical to DOOM's own warns that the mod does not need it. A file beside `mod.json` and a dot folder are not the mod's. The recorded
`path` of a file (the block's `files` and `stock` entries, the bundle's `sh_mb_file.path`, sync and export archive
entries) is its path in the folder; the request is derived from it and the salt. A closure pointer's path is
`native/<request>`.

Names (`sh_ma_text`, `sh_ma_engine_name`): in a decl text and in `mod.json`'s author part, a run of name characters
(`[A-Za-z0-9_./$+@~-]`, starting after a character that is not one) that begins with a folder prefix is the mod's
and is written as the engine names it: `<folder>/<rest>` becomes `mods/<salt>/<rest>`, `animations/` and a `meshes/`
name ending `.md6mesh` or `.md6skl` become `md6/mods/<salt>/<rest>`, `movies/` becomes `swf/mods/<salt>/<rest>`,
`scripts/` `mods/<salt>/scripts/<rest>`, `map-modules/` `maps/modules/mods/<salt>/<rest>`, `decls/<type>/<rest>`
`mods/<salt>/<rest>`. Every other name is the game's and is left as written; `native:<name>` is written `<name>` (the
installed archives hold 452 names under `effects/` and one under `particles/`, and none under the other prefixes),
and so is the quoted name of `native("...")`, the Graphics SDK's other spelling of a DOOM name (`ma_in_native_call`).
`#str_<key>` of one of the mod's own strings keys becomes `#str_<salt>_<key>`; in `mod.json` a key of a `strings`
language object that is not one of the game's (`sh_strkey_is_edit`) becomes `<salt>_<key>` (`sh_ma_json`; the
builder's own part is never rewritten, and `sh_ma_json_author_keys` turns a manifest's keys back for a received
build's `mod.json`). Scripts are not rewritten: a script's names are resolved by its calls (`sh_script_check_name`).
A text may still spell an engine name (`mods/<salt>/x`): it is served as written.

The builder reads every decl through the rewrite (`mb_engine_text`: the needs scan, the look compile, the closure,
the stock comparison of a `native/` copy and the derived edits all see the engine's text), and the file's identity
(its SHA-256 in the block) stays the author's bytes. The serving index serves the rewritten text: `ms_engine_names`
writes each changed decl under `mods\.served\files\<sha16>.decl` and points the file there, before the bank rename,
so every open, the look filter and the ledger read the engine's bytes, as for a renamed bank. Salting a folder
writes only its block: nothing on disk carries the salt, a copy needs no move, and a text that does spell
`mods/<old salt>/` is rekeyed in every mod folder (`ms_rk_entry`). Install lower-cases the names of every author
folder. The offline proof is `tests/served_cli.c`: it builds every folder of a root and lists each served request
with the SHA-256 of the bytes the engine is handed (R-NM1: the Tyrant and the owner's development lane, 1,302 of
1,305 and 1,260 of 1,262 requests identical, the rest scripts changed by the script API's renames).

These backend files share the work:

- `modbundle.c` builds a mod folder into a bundle: a SHA-256 per file, the
  logical name each file provides, the names its decls need, the namespace and
  structural rules, and the canonical manifest whose SHA-256 is the build hash.
  It reads and writes `mod.json` and the product record (below). A mod's
  identity is its salt (`modsalt.c`): eight characters `[a-z][a-z0-9]{7}`
  given once, recorded in the builder block, never read from the folder's
  name, which is the author's to change at any time. The engine's names for
  the mod's things live under `mods/<salt>/` (a salt has one spelling in
  engine requests) and every strings key reaches the engine as `<salt>_<name>`
  (letters, digits and `_`; no `/`, since the engine's string ids were only
  ever exercised with `[a-z0-9_]`); the author writes neither: a decl names
  its own files by folder and a string as `#str_<key>`, and the name rule above
  rewrites both at build and serve. A strings key the game's language
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
  `mods/<salt>/` (in the folder, under `programs/`). Of the 494 permutation decls the installed archives hold,
  324 parse as decl trees; the rest carry HLSL with `=` at statement level. Rules 13 (`SH_MB_RULES`):
  files beside `mod.json` are ignored, campaign content derives its requirements, `native/` holds DOOM's changed files, a
  sound decl's leaf takes the namespace and the block may carry `spelled` (the namespace the carried binaries spell),
  so every own folder is built again once.

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
  `strings`, `hud`, `scenes`, `companions`, `animation_events`, `attachment_tags`; any other key is kept in the manifest's
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
  version of Snapmap+. This version does not read it yet."
  (`SH_MB_EARLIER`, `sh_mb_descriptor_state`); a format-2 block without a salt
  is damage the sentence names. A file in the engine's layout
  (an `assets\` folder) is refused by name with one of the two sentences of
  "The mod folder" above. Files beside `mod.json` (a README,
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
  folder (`{"format": 2, "salt": ...}`); it rewrites no text, because no
  text carries the salt. The readers of earlier shapes live
  only in the store tool (`modbundle_old.c`, `sh_mb_old_load`), which converts
  existing folders once at update; the game DLL never links them. What they
  write is the engine's layout, and the build now refuses it until the
  installer also lays a converted folder out by kind (owner, 2026-11-17:
  converting releases' mods is the installer's, not yet done). The Mods
  tab's New mod writes an example that places and plays untouched: an
  entityDef `entities/example_crate` with the game's SnapMap `crate01`
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
  tile detour), answers the decl existence probe for served decls (the probe, lookup and load hooks only when the table holds a decl), activates a map's gameplay decls at
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
  mod no installed folder serves (the asset rule, below), fetches the units
  its rows name with no question, and resumes it or drops it, as in
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
- `modpublish.c` holds a publish of a map with mod rows while it uploads the
  units the store lacks, pins the publish's save to those units, claims them
  under the published item, releases the claim at unpublish, and refuses a
  copy to My Maps that the map service would reject, as in
  [Publishing a map with mods](#publishing-a-map-with-mods).

The store. A folder is `mod.json` plus folders by kind (above). `mod.json` is format 2
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
version of Snapmap+. This version does not read it yet.", a
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
is derived from both blocks in memory, for the hash. There is no
blob store and there are no hard links: a mod's files live in its own
folders only.

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

1. salts every root folder with no builder block (`sh_ms_assign_salt`): a new
   salt (`sh_salt_generate_except`, never one a folder of the store holds),
   written as the block `{"format": 2, "salt": ...}` (`sh_mb_salt_modjson`) and
   nothing else. No file is moved or rewritten, because no file or text carries
   the salt: an author writes the folder names and plain strings keys, and the
   salt is added when the engine is handed a text (see [The mod folder](#the-mod-folder)).
   A salt a text spells as `mods/<old salt>/` (a copy of a mod whose texts
   spell its engine names) is rekeyed in every mod folder (`ms_rk_entry`).
   A sound bank `sounds/<stem>.bnk` is served under the engine's name
   `<salt>_<stem>` by the serving index, never renamed in the folder. A folder name that is a salt of the store names that mod; renaming a
   folder changes nothing. A file that cannot be read refuses the folder alone
   with that cause (`MODS: store: <folder> refused: ...`);
2. settles a salt two root folders carry (a copied folder): the folder the
   record's `salts` section names (the one that held it at the last scan)
   keeps it, else the folder created first, never the build time (an Explorer
   copy carries the same `built`); every other one is salted anew with `from` the old salt, one
   line each ("mods\<b> is a copy of mods\<a>; it is a new mod now");
   until then the listing shows the copy refused with that reason;
3. builds every root folder whose block's tree hash (`sh_mb_tree_hash`: the
   author's bytes and the size and time of every file in the mod's folders) or rules
   (`SH_MB_RULES`) differ from the folder's, or whose last build failed
   (`sh_ms_sync_pending` counts them, with the folders of steps 1 and 2). Place
   folders are never built.

A folder in an earlier shape (a format-1 block, `package.json`, a retired
author key) is listed with its plain reason ("This mod folder is not in a shape
Snapmap+ can build: ...") and nothing in it is written; the installer's store
tool converts every such folder it finds each time it runs. A converted folder
is in the engine's layout (`assets\`), which the build now refuses until the
installer also lays it out by kind (owner, 2026-11-17: converting releases'
mods is the installer's, not yet done).

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
example crate at `entities/example_crate.decl` and its Toybox row, and the row's
strings in `mod.json` keyed `category`, `example_crate_name` and
`example_crate_desc` (referenced `#str_category`, ...; the engine is handed
them with the salt); the scan that follows builds it.

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
the map and travels with it. A name whose provider is a shadow of editor-time
content (`sh_mb_editor_time`: the snapEditor*, snapPropertyInspector* and
snapPalette decl types, the editor's own movies) names no bundle: such a
shadow changes what the author can set, never what plays, so an author's
editor overrides never become a dependency of a map, vanilla or not. A
gameplay shadow (an entityDef, a material, a model) still does.

The player side of the same rule: a row is served only by an installed build
(`sh_modserve_row_served_for`), so a build a map names is installed like any
other (an install drops nothing).

`mods\snapmap-plus.json` is the product's one record:
`map-claims`, `map-pending` (both `modclaim.c`, see [Publishing a map with
mods](#publishing-a-map-with-mods)), `links`, `migration` and `places` (each
map's place, above), each section written whole under one lock. The `claims`
and `pending` sections the store tool's migration folds in from earlier
releases' files are not read by the product. The
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
and try again." to their result (`sh_mb_record_why`). A claim the publish could
not record is a log line (`MODS: publish: the claim of '<item>' could not be
recorded`).

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
rawmap or prefab ZIP import that carries it installs it again; a published
map's content downloads into the content cache, not into the mods folder. Uninstall also clears the open map's undo history (see [The Mods
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
(the engine's layout of its day: paths, texts, strings keys
`<salt>_<key>` and their `#str_` references, from a view of `mod.json` and
`assets\` only, its sound banks renamed `<salt>_<name>` with their BKHD id and every text of the folder that names one (decls, mod.json)),
every other file beside them copied unchanged (DECISIONS 16), and built there
by the real builder over the store's installed bundles and the game's archives
(`sh_modstock_exists`, replacements kept, stock-bank needs recorded through `sh_mb_set_bank_need(sh_mbi_bundle_need)` as the game records them). A build that fails, or a salting the
folder refuses (the folder is then copied as it is with a fresh salt), is
written as a failed build (`sh_mb_write_failed`) with the builder's plain cause
(DECISIONS 14). The work folder is swapped in under the folder's own name and
the folder as it was goes to the Recycle Bin. That output is in the engine's
layout, and the build now refuses it until the installer also lays a converted
folder out by kind (owner, 2026-11-17: converting releases' mods is the
installer's, not yet done); only a folder whose descriptor
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
and the path, size and time of every file in the mod's folders) or the builder's rules differ from
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
install takes the store itself (the publish launch reads the claim queue and
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
`#str_` prefix decls use (a mod's own keys reach the table as `<salt>_<name>`,
the name rule's rewrite), and only `en` is read. A mod's own keys come only from a
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

A sound decl a mod carries (`generated/decls/sound/mods/<salt>/<salt>_<stem>.decl`,
the author's `sounds/<stem>.decl`) names the Wwise event `play_<salt>_<stem>`: the
engine computes the event id from the decl's own name, and the leaf takes the
namespace because every mod's event names would otherwise be the same as
DOOM's. The author's bank names the event `play_<stem>`; serving rewrites the
bank's HIRC event ids to the namespaced ones (`sh_modserve_bank_events`), and
the decl plays once that bank is loaded. The builder makes a `soundShader` or `sound`
key naming `mods/...` a need of type `sound`; a stock event name is not a need.
A bank is a file `sounds/<stem>.bnk` in the mod folder, and the engine is
handed it only as `sound/soundbanks/pc/<salt>_<stem>.bnk` (Wwise opens banks
by file name in that one folder), the file name letters, digits and `_` only,
and without `_sp`, `_snapmaps` or
`initial`, which the engine's listed-bank filter skips in some game mode; its
BKHD chunk must be version 113. The build does not check the chunk's id
against the name (R-NM1; the earlier build refused a renamed bank). A bank is
always carried: never a stock pointer, never a shadow.
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

A mod can ship a new render program `programs/<name>` in its folder (the engine's
`mods/<salt>/<name>`; no `.` in the last
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

#### Layered render programs

Snapmap+ provides render programs of its own, `snapmapplus/layered/skinvmtr`
and `snapmapplus/layered/skinvmtrwounds` (`modprog.c`, `modprog.h`), which a
mod's material names like any program; the material that names one is written as
a `type layered` material (see [the Graphics SDK](graphics-sdk.md)). Each is the
game's lit skinned virtual-texture program of the same base name with a layered
surface added: the material's own layer, read from its virtual-texture pages, is
blended with up to four tiled layers, one per channel of `layerblendmask`, by a
height-weighted transition, with reoriented normal mapping, before the wound
block and lighting. The rule is documented in
`src/backend/programs/shaders/layered.rp`. The layers' images are the
material's textures (`blendlayer<i>{albedo,normal,smoothness,height}map` and
`layerblendmask`) and `multilayerextramap0`/`1`, a pool of two images shared by a
layer's specular or glow image, the burn's tile, the burn's layer-0 glow, the
wound cap's blood and the base height.

The program's numbers are not render parms. The 28 vectors (tilings, scales,
softness, the layers' specular and glow, where layer 0's height comes from, the
burn) are the texels of one 8 x 4 RGBA16F image, the `layerparms` texture: vector
k is the texel (k % 8, k / 8), fetched exactly with `texelFetch`, and a layer's
vectors only where its mask is non-zero. `sh_modprog_param_image` writes the
image (a DOOM 2016 `.bimage`), `sh_modprog_register_param_image` serves it at
`generated/image/snapmapplus/look/<16 hex digits of its SHA-256>.bimage` in the
product's container (the same vectors, the same path; the builder records no need
of it, since the material derives it again wherever it serves, so a unit never
points at one a player's product has not derived yet; the provider serves such a
name, which no build lists, from the product's registry once the material that
spells it has compiled in the process, `sh_modserve_set_product_reader`, in every
mode; a unit that still points at one is satisfied by this product's compiler,
`sh_mb_product_hashed`; without the serve the layered program read no numbers
and the surface drew black, R-HD1), and `SH_MODPROG_ML_PARMS`
names the vectors by the material keys they had. Layer 0's height has no sampler
of its own: vector 9 says whether it is a constant or the red channel of an
extra map. A material written with the old keys (the 28 vectors and
`blendlayer0heightmap`) is no longer converted (R-NM1); write a `type layered`
material. A burning surface (the `isember` materials) also
burns after the blend, by the wound cap's blood level, and adds embers flickered
by the view's own `magnificationclouds` noise (the base program's sampler; the
burn adds no texture), its burnt tile, cap blood and layer-0 glow image in the
two extra maps. Everything else is the base program unchanged.

Each program has one source, an `.rp` file in the render program language: an
`hlsl_fp_header` section of functions (the engine compiles that section before
`main()`, so they are real functions) and a `call` section that builds the
arguments from the stage inputs and render parms and calls them. The OpenGL
renderer compiles the program from the decl text, which carries both sections as
written. For Vulkan, `tools/gen_programs.js` translates the same functions to GLSL
by a few explicit token rules (types, `lerp`, `frac`, `saturate`, `tex2D`,
`tex2Dlod`, `clip`, `$texture` names; `texelFetch` and `int2` pass through),
which becomes the cooked program's stage
text and, compiled, the SPIR-V spliced into the base module; the splice loads
the arguments from the same inputs and uniforms the call section names (the
tables in `modprog.c`). `tests/programs_rp_test.js` checks both ends.

Nothing of the game is shipped. Every file of the program's closure (its decl
and permutation decls, the `.bin` files, the SPIR-V and the render-parm decls of
its keys) is derived in memory from the installed base program when first asked
for, and `modstock.c` answers for it as installed content in the container
`snapmapplus`: the closure brings it along as pointers (noted once per bundle),
the checks above run on it as on any program, the server serves it, and an
upload never sends it. The decl gains the blend's functions (in an
`hlsl_fp_header` section) and its call (OpenGL compiles that text); the cooked
`.bin` gains its 20 textures (19 images and the parameter image) after the
stage's own, its binding tables moved past them, and no parameter (the
fragment stage's material uniforms do not grow); the fragment module has the
blend's functions spliced in with its samplers bound after the program's
textures; the vertex stage is the base's. `skinvmtrwounds` then binds exactly
the engine's 32 textures across its stages (`ID_VULKAN_MAX_BOUND_TEXTURES`),
and the derivation refuses a program that would bind more. A base program that is not the shape the
derivation knows makes the build fail with one sentence naming the program and
pointing at the game's files.

#### The particle program

`snapmapplus/particles/uberparticles` (`modprog.c`, the same derivation) is the
game's `uberParticles`, which every particle material is drawn with once the
engine remaps `transsortblend`, `particleshighq`, `liquidhighq` and
`explosionhighq` to it (`decl_useTransparentUberShader`, on by default), with the
product's shading of a CPU particle stage: its unlit, lit and explosion modes,
the near fade on the distance from the view and the soft-particle fade as a
distance, from the same particle vertices (flipbook frames and blend, colour,
`genericParm` as the engine packs it). The colour and its alpha are read as the
CPU wrote them, linear: the game's vertex stage hands them over sRGB-decoded and
its CPU remaps the alpha past `r_particlesMinAlpha`, and the shading undoes both
first. The stock shading still runs and its colour is replaced before the stage
returns. The stage's constants take no render parm of the product's own: they are
kept in keys the game's particle program already holds, by what they mean.
`particleuberparms1` holds the mode in z (0 unlit, 1 lit, 4 explosion, plus 16
when the emissive mask sheet is bound) and the depth offset scale in w (x and y are
0, left for the game's own code that still runs), `particleuberparms2` the
emissive mask colour or the explosion's second gradient colour in xyz and the near
fade's begin in w, `particleuberparms3` the explosion's third colour in xyz and
the emissive multiplier in w, `emissive` the near fade's end in y, the soft
distance in z and a lit stage's scale in w (x stays the material's own), and
`smokebrightness`, a vector no engine code, program or fx names, the emissive
mask's multiplier in x. The shading loads the first four from the program's own
material uniforms; `smokebrightness`, `projectionmatrixz` and `globalvieworigin`
are appended to the fragment stage's material uniforms, and the emissive sheet is
the texture `particleemissivemap`, added after the program's own. (The engine
writes `particleuberparms` only for a material whose stage program it remaps to
`uberparticles`, never for a material that names this program.)
`SH_MODPROG_EP_PARMS` names the keys the constants had as render parms
(`eternalparticle`, `eternalparticlefade`, `eternalparticlecolor`,
`eternalparticlecolor2`), and a material that writes them is no longer converted (R-NM1). The rule is documented in `src/backend/programs/shaders/particles.rp`,
the one source of both renderers' text. A liquid stage keeps the game's own
blood shading.

The programs were named `snapmapplus/multilayer/<base>` and `snapmapplus/eternal/uberparticles` before they stopped naming a source game; both old names are served until 1.0 for mods emitted before the rename (`SH_MODPROG_OLD_PREFIX`, `SH_MODPROG_EP_OLD_PREFIX`), answering with the new name's bytes.

#### The Graphics SDK: looks

A mod describes a surface or a particle stage in plain terms and the product writes the engine's material for it
([the guide](graphics-sdk.md)). The compiler is `modlook.c` (`sh_look_compile`, pure text work): a material whose first
key is `type` with the value `layered` or `particle` is read field by field (every field optional, the guide's
defaults) and written as the keys of the two programs above, its other lines passing through unchanged. A layered look
becomes `ambientprogram "snapmapplus/layered/skinvmtr"` (or `skinvmtrwounds`), each layer image under its
`blendlayer<i>...map` key, the mask as `layerblendmask`, the 28 vectors as the parameter image named by `layerparms`,
and the two shared places `multilayerextramap0`/`1` filled in a fixed order (burn tile, burn glow, burn blood, base
height, then each layer's specular and glow); what does not fit is drawn as the field's default and noted for the
build log, never refused. A particle look becomes the five stock keys of the particle program and, with a glow mask,
`particleemissivemap`. Where the game's own program draws the same picture (no layer can show; an unlit or lit particle
with nothing the game's shading lacks) the material names `skinvmtr`, `skinvmtrwounds`, `transsortblend` or
`particleshighq` and the product's programs are not involved. A value or a colour where an image is expected becomes a
constant image, 4 x 4 RGBA16F, registered beside the parameter image at
`generated/image/snapmapplus/look/c<16 hex digits of its SHA-256>.bimage` (`sh_modprog_register_const_image`; the
same colour, the same path). A material that is not a look is not converted (R-NM1): one that writes the old keys
is the author's to rewrite as a `type layered` material. A mistake in a look is one sentence
naming the file, the line, the field and what to write (`SH_LOOK_MISTAKE`); a limit of the game never is.

Two hooks use it. The builder reads every material decl of a mod through it (`sh_mb_set_look_compiler`, set in
`dllmain.c`, called where `modbundle.c` reads a decl): a mistake fails the build with the sentence, the needs
(images, programs) are scanned from the compiled text so the closure brings the derived images and programs along, and
the notes go to the build report. The server serves the compiled text (`sh_modserve_set_material_filter`,
`ms_look_of` in `modserve.c`): derived on the first open of the file and kept with the serving table like a composed
text, so every open and the ledger see the same bytes while the build's identity of the file stays the author's file;
a mistake at serve time (the build already refused it) serves the file as it holds it with one log line. `look_cli`
(`tests/look_cli.c`, built by `tests/run-tests.ps1`) compiles one decl outside the game and writes the text and the
images it derived.

#### The Graphics SDK: effects, environments, values that change

The other SDK files are compiled the same way, through one entry point (`sdk.c`, `sh_sdk_compile`, pure text work
apart from the decls it registers): the request path's type folder picks the compiler. Every author word is in
`sdkwords.h` (one table: renaming a word there renames it in the compilers, their sentences and the script calls);
`sdkparse.c` reads the author grammar (a brace group of items, each a row of words, numbers, names and nested
groups).

- **An effect** (`modeffect.c`): an fx decl (`effects/<name>.decl`) whose first key is `type effect` becomes the game's fx decl, one event
  per item, written as the game's own editor writes its fx (a key the author leaves out is left out, so the engine's
  default holds; a decal gets a duration and a size, the engine draws none without). The game's FX manager plays every
  item type in one function on both renderers (VK `FUN_1416f0240` / GL `FUN_1416f0bc0`), so an effect draws with the
  game's own code. The compiled text's first line is `// Snapmap+ effect`.
- **Particles** inside an effect become a particle decl the product derives: one CPU stage from the plain fields,
  following the game's own reading of a stage (a size is PARAMETRIC_EVAL from birth to death unless it is a range,
  GENERIC; a curve is CURVE_GENERIC, `val0 x table(t) + val1`, its table in the decl's `tableDecls`; 2016 multiplies
  the direction's x, y and z by `speed[0..2]`, so a speed goes on all three; gravity is half the acceleration, the
  origin adding `gravity x age^2`). A speed range, drag and bounce are written as Snapmap+'s simulated motion (the
  section below). An `image` becomes a `type particle` material compiled by `modlook.c` and derived too.
- **Values that change over time** (`sdkvalues.c`): on a material, every line `key pulse|flicker|curve { ... }` of any
  mod material becomes the game's register program (`tempN` lines over `time`, the game's `sintable` for a pulse,
  `random_smooth03` for a flicker, a derived table for a curve; every vector key of a parm block is an expression,
  VK `0x141a035e0` / GL `0x141a1f680`). The temporaries are the game's own 128, which already hold their slots. A
  colour key the parser decodes from sRGB (43 parms) has its numbers decoded the same way.
- **An environment** (`modenv.c`): an env decl (`environments/<name>.decl`) whose first key is `type environment` becomes the game's env decl
  (inherit `default`, a `renderParms` block of the keys it names) and, derived, the fx `set_environment` plays: one
  `FX_ENV_CHANGE` item per 16 values, looping, its fades the environment's `blend` (the view's own override, set at the
  start and released at the stop, case 0xb of the FX manager). The compiled env decl's first line names that fx.

**Derived decls.** What a compile derives is registered with modprog (`sh_modprog_register_decl`): named by the
first 16 hex digits of the SHA-256 of its type and text, at `generated/decls/<type>/snapmapplus/sdk/<hex>.decl`
(`snapmapplus_sdk_<hex>` for a table, since a material expression names its table without a '/'); the same file
gives the same names at build and at serve. An effect's **emitter**, the entityDef the script calls spawn, is
`snapmapplus/effect/<fx name>` (inherit `func/fx`, `fxEffect` the effect), derived from its path alone so it is served
before anything has compiled the effect. `fxEffect` is the field `idEntityFx` plays when it spawns (unless `startOff`),
as every stock `func/fx` child names its effect; `fxDecl` is an actor's condition-driven fx, which only an event starts,
so an emitter written with it spawned and played nothing (R-GS3). The builder reads material, fx and env decls through `sh_sdk_compile`
(`sh_mb_set_sdk_compiler`): a mistake fails the build with its sentence, the needs are scanned from the compiled text
(an effect's tables, flares and envs, a particle decl's `tableDecls`, a material expression's derived table), and
the needs a compile returns join the file's own (an effect's emitter, an environment's fx and its emitter), so the
closure brings every derived decl along as a pointer to the product's container; the server serves the compiled
fx and env text as it serves a look.

**Derived decls are registered like the mod's own.** A pointer to a decl the product derives (a name under
`snapmapplus/` or `snapmapplus_`, `sh_mb_product_decl`, or a file `sh_modprog_owns` answers for, such as the platform
programs' texture render parms; `sh_modserve_set_product_owner`) restores a name no archive lists. Below the engine's
lifecycle state 2, `DeclFind` finds a name it holds no resource for only through a registered source (VK
`0x14155ae90`; from state 2 the existence probe answers), so such a pointer is a step of every plan, the map pass's
and the editor's, registered and loaded as the mod's own decls are, its needs first (`ms_product_planned` in
`modserve.c`; `MS_PLAN_PRODUCT_PROGRAMS 0` leaves the programs and render parms to the engine's demand load). The
other stock pointers at the game's own names stay surfaces, never planned. A derived decl exists in the product only
once the file that derives it has been compiled in this process, and a plan loads it before that file: a read of such
a pointer that finds nothing compiles the served files that need it first (`ms_derive`), then reads again. The
Tyrant's map pass is its 586 own steps and 24 of the product's (3 programs, 21 render parms). `sdk_cli` (`tests/sdk_cli.c`) compiles SDK files outside the game and writes
what they derive.

**The script calls** ask the server through `sdk_runtime.c` (`sh_script_set_sdk_runtime`): `play_effect(name, ...)` asks
whether the mod's fx decl `name` (`effects/<name>.decl`) is an SDK effect (its served text, `sh_modserve_serve_identity`, starts with the
mark; asking compiles it, which registers what it derives) and spawns its emitter at the point, or at the entity and
binds it (`bind`, or `bindToJoint` with a joint), with `setColor` for a colour; `set_environment` spawns the emitter
of the fx the environment's first line names and keeps it, `clear_environment` removes it. When the game holds nearly
all its entities (`sh_script_api_has_room`), an effect or an environment is skipped with one log line instead of
refused. `demon:recolour` sends `lerpRenderParmFrom` for each region's `custom*albedo` parm (the customization colours
the engine itself writes per render entity, VK `FUN_140fdeb70` / `FUN_140fdf770`).

#### Render parm slots are never refused

The engine keeps every render parm in one of 2,367 parm state slots, a fixed
range per kind (`idParmState::AllocParmState`): 1,024 vectors, 512 textures,
128 samplers, 384 programs, 128 strings, 64 buffers, 64 uniform buffers, 63
image buffers. A parm whose range is full is a fatal engine error, and under
the fault shield a parm that never loads, so every program naming it draws the
engine's default program. The vector range is the tight one: the game's own
decls and its 128 expression temporaries (`temp1`..`temp128`, of which only 80
have a decl file) hold 973, which leaves 51 for everything else. The product's
programs declare none of them: the layered program reads its numbers from an
image and the particle program from keys the game's own program holds
(`docs/graphics-sdk.md`).

`parmslots.c` detours `AllocParmState` on both renderers and decides first
(`parmslots_rule.c`, pure):

- A parm whose name already holds a slot, or whose range has room, is placed
  by the engine as before. The reservation for the vector parms of the
  product's own programs (`sh_modprog_vector_parm`) is empty, because the
  programs declare none: nothing is held back from a mod's own parms.
- A vector parm that finds no slot of its own shares one. The last vector
  slot handed to a parm that is not the product's becomes the shared slot, and
  every later such parm is given it. Parms that share a slot read each other's
  value: a program naming one draws with a wrong constant instead of not
  drawing.
- A parm of another kind whose range is full shares the slot of the last parm
  of its own kind.

Nothing is shown to the author or the player. The backend log has one `PARMS:`
line when the guard is installed, one when the shared slot is made, and one
for each of the first 32 parms that share a slot. With today's contents the
product's programs take no vector slot, so a mod gets the 51 free ones and
the shared one after them.

#### Simulated particle motion

DOOM 2016 places a CPU particle in closed form: where it was born, a straight
flight along its direction at its speed, a uniform acceleration, gravity, and
a custom path added on top. Nothing in it turns a particle around an axis,
slows it by its own velocity or shakes it. Snapmap+ adds that for a stage
that asks in its own decl: the particle's flight is integrated from its
birth, in fixed steps, under the forces the stage names, replayed from birth
each frame so its place stays a function of its seed and age alone (no state,
the same in every view, across a pause or a save).

A CPU stage (`gpuStage` unset) never reads its GPU blocks. With
`vectorFieldGPU.repeat = true` the product reads them as the stage's motion;
the table of keys is in `src/backend/particlemotion.h`: a world acceleration,
wind with drag, a vortex about an axis of the emitter, a noise field, brownian
motion, a point, axis or plane attractor, drag toward the emitter, a launch
ball, a launch speed factor, and a velocity stretch that draws a quad aligned
to its velocity at its own length times its speed. The forces and the
integrator are DOOM Eternal's particle simulation commands (its formulas, read
from its simulation program), so a converted stage moves as it does there.
None of the 9,834 CPU stages the game ships writes `vectorFieldGPU`, so no
stage of the game asks.

A stage can also collide with the scene (`colorGPU.fadeColor.x`, its
thickness, over 0). DOOM Eternal tests a particle against the screen's depth
on the GPU; a CPU particle has no depth, and the particle jobs run off the game
thread, where the engine's trace is not safe. So the particle jobs ask for
their emitter's place, the game thread traces it once from the script tick
(`particlemotion_planes.c`: the surface under it and, for an emitter that does
not face up, the one behind it, passing through players and demons) and
publishes the planes in a table the jobs read under a sequence count. Each
step then runs Eternal's collision test, response and kill against them. The
test needs the particle's place in the world, so a colliding stage is written
`isTransparencySorted`, which makes the game build it in world axes; a stage
that is not, or a map without a mod script, draws its particles without
collision and the backend log says which.

A stage can be the child of another stage of the same decl, its particles
born where a parent particle is, as DOOM Eternal's spawned children are (a
spark's trail). A CPU particle cannot be born at another stage's particle, so
the product retraces the parent: a parent stage's particles take the product's
random numbers (from their index and the emitter's place), and child particle
k is drawn at its own offset from the place parent k mod the parent's count
had at the child's birth, which the game's spawn timing gives from the two
stages' counts, spawn windows and time offsets alone. A child whose parent was
not yet born or already dead is not drawn. The link is
`systemPropertiesGPU.emissionTime` (`particlemotion.h`).

Two parts of the launch are the product's because the game's keys cannot say
them. The game multiplies a particle's direction by `speed[0]`, `speed[1]` and
`speed[2]` one axis at a time, each drawing its own random number, so a speed
that differs per particle cannot be written without bending the direction: the
launch speed factor multiplies the launched velocity by one number per
particle between its two ends. And the emitter's scale (an fx item's size),
which the game applies to a particle's whole place, is taken out of the launch
velocity the detour measures and given to the steps, as Eternal's integrator
has it: the forces see the scaled places and each step moves the particle by
velocity times step times scale. The game's origin function carries the scale
only for a stage that is transparency sorted (the stage's own
`isTransparencySorted` key, off by default); for any other stage it
answers unscaled, the scale is applied when the model is drawn, and the forces
that depend on a place see unscaled places.

`particlemotion.c` detours the function that writes one particle's vertices
on both renderers (`particlemotion_sigs.c`) and returns to the game's code for
every stage that does not ask. For one that does, the game writes the quad as
always; the detour asks the game's own origin function where the particle was
born, its launch velocity and where the game put it, integrates
(`particlemotion_sim.c`, pure; the same arithmetic as Doomforge's
`doom2016/effects/particle_motion.py`) and moves the quad, rebuilding one
aligned to its velocity along the simulated velocity. The records it reads
live on the particle job's stack, so nothing is shared between threads. The
backend log has one `PARTICLES:` line when it is ready.

#### Cache writes

The engine writes its cache files relative to its base path, the game folder:
`generated/cm/<name>.bcm`, `generated/models/effects/particles/*.pmodel`, a
staging file `generated/temp/<path>.%4X.tmp` renamed into place, and
`maps/DynamicSnapMap.flight`. Its file search path has `<fs_savepath>\base`
ahead of the game folder, so a cache file written there is found by the
engine's readers with no further change. `cachewrites.c` detours the one
function every such write goes through, the engine's atomic write
(`cachewrites_sigs.c`, both renderers), and hands it the absolute path
`<Saved Games>\id Software\DOOM\base\<rel>` for a write under `generated/` or
a `.flight` file under `maps/` (`cachewrites_rule.c`, pure), making the
folders below that root first. An absolute path makes the engine put its
temp file beside the target and rename it there. Every other write, and one
whose folder cannot be made, is passed through unchanged. The backend log has
one `CACHE:` line when it is ready.

`src/backend/programs/` holds the layered surface and the particle shading behind the general
derived-program mechanism of `modprog.c`.
The layered program's Vulkan module and texts are vendored in `programs/programs_layered.c`,
the particle program's in `programs/programs_particles.c`,
generated by `node tools/gen_programs.js` from `src/backend/programs/shaders/`
(glslangValidator compiles the module; `--check`, run by the JavaScript tests,
needs no compiler and compares the texts). `modprog_test` checks the decl, text
and module patches on a stand-in program (`tests/fixtures/modprog/lit.frag`),
the derived `.bin` and SPIR-V against the engine's checks, and, given
`-Doom`, the derivation from the installed game.

### Clip events

A mod adds animation event rows to its own clips when they play on a game
model, without replacing any game file (`clip_events.{c,h}`,
`clip_events_rule.c`, `clip_events_sigs.c`). It writes an md6Def of its own,
the carrier, that inherits the model the clip plays on
(`init { inherit "player/mp/tp_body_mp.md6" }`) and lists the rows in its
`events { anim "<clip>" { event ... } }` blocks, and names it in the author
key `"animation_events": ["meshes/.../rows.md6"]` (the carrier's name in the mod folder, `meshes/.../rows.md6.decl`). The engine parses
the carrier with its own md6Def parser, so every event type is available with
its own argument types; the product never builds an event.

The build (`sh_ce_build_registry`, registered with the builder through
`sh_mb_set_clip_events` by the DLL and the store tool) records the registries
member `clipEvents`, `[{"md6Def", "base", "clips": [...]}]`, outside the
canonical text. Every mistake is a warning, never the build's cause: a value
that is not a list, an entry that is not a name, an entry that is not one of
the mod's md6Defs, has no `inherit` inside `init`, has no anim block with an
event row, or repeats an earlier entry is left out; a clip outside
`md6/mods/<salt>/` is left out of its entry ("A mod can add rows only to its
own clips"), and a clip two entries give rows keeps the first. An anim need of
a carrier that the mod does not carry is reported, not refused, unlike any
other md6Def's. Each sentence says what to write instead.

The registry of a Play is built at the Play gate (`sh_modserve_play_begin`,
main thread, before the level load) from the `clipEvents` of the builds the
open map serves (`sh_modserve_each_bundle_registries` and
`sh_modserve_bundle_scope`'s `names_live`), checked again (a carrier outside
`mods/<salt>/` or a clip outside `md6/mods/<salt>/` is dropped with a line, so
no build can register a stock clip or another mod's), sorted by clip name and
published with one pointer swap; the map-load gate drops it. It holds names
only. The previous registry is freed at the swap after next.

The three event lookups are `bool f(idDeclMD6 *model, idMD6Anim *anim, int
from, int to[, int filter], idList *out)`: each empties `out`, queries the
model's own table (`+0x228`) with the anim's resource name (`+0x08`), then
every parent (`+0x98`), merging into `out`, and returns whether `out` holds a
row. `model` is the entity's animator model decl (entity `+0x3DB0`, then
`+0xE90`, the md6Def decl the script tags read; VK sync helper 0x140A403A0).

| Site | VK | GL | Query of its first call (VK / GL) | Window |
|---|---|---|---|---|
| `Md6EventsInWindow` | 0x16D7680 | 0x16D8190 | 0x16D71D0 / 0x16D7CE0 | 15 |
| `Md6EventsOfType` | 0x16D7810 | 0x16D8320 | 0x16D7350 / 0x16D7E60 | 16 |
| `Md6EventsOfType2` | 0x16D79B0 | 0x16D84C0 | 0x16D7500 / 0x16D8010 | 16 |

The two filtered lookups are the same 196 bytes up to their first query
call, so their shared pattern matches both in each executable; each is
accepted at its recorded RVA only when that E8 reaches its own query, checked
by the query's own pattern, which is unique (`sh_ce_site_resolve`). The
windows are whole and position independent. A site that does not resolve
records `Clip events` off (`These features are unavailable in this session:
Clip events.`), and each Play whose mods list clip events then logs one line
per mod: `<mod>: its clips play without the events it adds (animation_events):
Clip events is unavailable in this session.`

The detours are prepared at boot and committed, all three or none, at the
first Play whose registry has a row; with no registration no hook is
committed. Each calls the engine first:

```
r = orig(model, anim, from, to, [filter,] out);
if (r || !g_any) return r;                  // the model's chain answered, or nothing is registered
name = anim->name (+0x08);
if (!under("md6/mods/", name)) return r;    // every stock clip leaves here
row = registry_find(name);  if (!row || editor active) return r;
carrier = carrier_of(row);                  // found by name, checked by name before each use
if (!carrier || !in_chain(carrier, model)) return r;
return orig(carrier, anim, from, to, [filter,] out);
```

`in_chain`: the carrier's parents reach `model`, or `model`'s parents reach
the carrier's own parent, at most 32 steps: rows written over the SnapMap body
apply on it, on the campaign body it inherits and on a body that inherits it,
and on nothing else. The first call wins in the sense of the window: a model
whose own chain has rows for the clip in the window answers; the carrier is
asked only when the window holds none of the model's own rows.

The carrier is found the first time one of its clips is looked up in a Play:
on the main thread by the engine's own find (`sh_modserve_engine_decl`:
DeclFind without makeDefault, which loads on demand a name the probe admits,
created and parsed at the engine's level as an entity naming it would be; the
map's activation registered its source with the bundle's other gameplay
decls); on another thread by a find-only walk of the md6Def list under the
resource lock (`sh_modserve_engine_decl_held`), and one not loaded yet is
asked for again at the next lookup. It is not held across the Play purge on
purpose: it inherits a stock body the product does not keep, so a carrier kept
at level 4 would hold a parent pointer the purge frees. Loaded after the
purge, it binds to the models of that Play and is freed with them; the
registry forgets the pointer at the next gate. A carrier that does not load
logs `MODS: clip-events: <mod>: "<md6Def>" did not load, so "<clip>" plays
without the events it adds.` once per Play, and the clip plays as the game
plays it. The diagnostic build logs the first answer of each clip in a Play,
`clip-events: "<clip>" on <model>: <n> rows from "<md6Def>"`.

`sh_mods clip-events` prints the open map's registry (the Play's, or the one
its next Play builds): each carrier with its mod, its base and whether the
engine holds it, then its clips and how many lookups each answered.
`clip_events_test` covers the names, the md6Def reader, the build's rows and
warnings, the registry and the chain rule; `clip_events_sigs_test` the sites,
the lookalikes, the walk they share and the windows on both executables;
`detour_windows_test` audits the windows with every other site.

### Attachment tags

A mod gives the tag info for an animated attachment of its own on a game
model, without replacing any game file (`attach_tags.{c,h}`,
`attach_tags_rule.c`, `attach_tags_sigs.c`). An attachment is attached to a
host model at a tag by an `ae_animatedAttachment_Attach` clip event, and
`idAttachmentCollection::AddAttachment` reads the tag info for (the attached
md6Def's name, the tag's name) from the HOST model's `props` table through one
engine function, the tag-info lookup; it fails with `model '%s' has no tag info
for '%s'` when the answer is NULL or its joint is -1. The player's md6Def is
the game's and is never edited, so a mod that attaches its own md6Def to the
player's body writes an md6Def of its own, the carrier, that inherits the host
model (`init { inherit "player/mp/tp_body_mp.md6" }`), lists the info in a
top-level `props` block of the engine's own form

```
props { prop "meshes/gun.md6" {
          tag "hand_r" { trans ( x y z ) rot ( x y z w ) parent "<joint>" } } }
```

and names it in the author key `"attachment_tags":
["meshes/.../tags.md6"]` (the carrier's name in the mod folder). The engine parses the carrier with its
own md6Def parser, so the product never builds a tag.

The build (`sh_at_build_registry`, registered with the builder through
`sh_mb_set_attach_tags` by the DLL and the store tool, reading the md6Def texts
through the same `mb_ce_text` as clip events) records the registries member
`attachmentTags`, `[{"md6Def", "base", "props": [...]}]`, outside the canonical
text; it sorts first among the registries members. Every mistake is a warning,
never the build's cause: a value that is not a list, an entry that is not a
name, an entry that is not one of the mod's md6Defs, has no `inherit` inside
`init`, has no prop block with a tag, or repeats an earlier entry is left out;
a prop outside `mods/<salt>/` is left out of its entry ("A mod can give tag
info only for its own md6Defs"), and a prop two entries give keeps the first.
Each sentence says what to write instead.

The registry of a Play is built at the Play gate (`sh_modserve_play_begin`,
main thread, before the level load) from the `attachmentTags` of the builds the
open map serves, exactly as clip events' is (`sh_at_registry_add` checks every
row again: a carrier or a prop outside `mods/<salt>/` is dropped with a line,
so no build can register a stock prop or another mod's), sorted by prop name
and published with one pointer swap; the map-load gate drops it. It holds
names only.

The lookup is `const void *f(const void *props, const char *prop, const char
*tag)`: `props` is the idList at the host md6Def + `0x210` (rows of `0x148`
bytes, the prop name pointer at +`0x10`), the answer is a tag record whose
int16 at +`0x1C` is the resolved joint, -1 for no tag (the engine answers a
static default record with -1 when nothing matches).

| Site | VK | GL | Window |
|---|---|---|---|
| `TagInfoLookup` | 0x16CE1E0 | 0x16CEE90 | 15 |

One 48-byte pattern, unique in each executable (`sig_resolve_one`; no call
target to check). The window (`mov [rsp+8],rbx; mov [rsp+0x10],rbp; mov
[rsp+0x18],rsi`) is whole and position independent. A site that does not
resolve records `Attachment tags` off, and each Play whose mods list
attachment tags then logs one line per mod.

The detour is prepared at boot and committed at the first Play whose registry
has a row; with no registration no hook is committed. It calls the engine
first:

```
r = orig(props, prop, tag);
if (is_tag(r) || !g_any) return r;          // the host's own table answered, or nothing is registered
if (!prop || !under("mods/", prop)) return r;   // every stock prop leaves here
row = registry_find(prop);  if (!row || no Play) return r;
host = props - 0x210;  carrier = carrier_of(row);   // found by name, checked by name before each use
if (!carrier || !in_chain(carrier, host)) return r;
r2 = orig(carrier + 0x210, prop, tag);
return is_tag(r2) ? r2 : r;
```

`in_chain` is clip events' (`sh_ce_in_chain`): rows written over the SnapMap
body apply on it, on the campaign body it inherits and on a body that inherits
it, and on nothing else. The host's own tag info always wins. All reads of
engine memory are fault guarded and nothing of a game model is written. The
carrier is found by name exactly as clip events finds its own
(`sh_modserve_engine_decl` on the main thread, `sh_modserve_engine_decl_held`
elsewhere); a carrier that does not load logs `MODS: attachment-tags: <mod>:
"<md6Def>" did not load, so "<prop>" attaches without the tag info it gives.`
once per Play. The diagnostic build logs the first answer of each prop in a
Play, `attachment-tags: "<prop>" on <model>: tag info from "<md6Def>"`.

`sh_mods attachment-tags` prints the open map's registry (the Play's, or the
one its next Play builds): each carrier with its mod, its base and whether the
engine holds it, then its props and how many lookups each answered.
`attach_tags_test` covers the names, the props reader, the build's rows and
warnings, the registry and the chain rule it reuses; `attach_tags_sigs_test`
the site and its window on both executables; `detour_windows_test` audits the
window with every other site.

### The sync clock

Every participant of a mod demon's sync (a glory kill, a berserk kill) runs on
the player's animation clock (`sync_clock.{c,h}`, `sync_clock_rule.c`,
`sync_clock_sigs.c`). When the game's tick rate changes (the adaptive tick,
nearly every frame of a SnapMap Play), the engine re-anchors every running
animation leaf so it keeps its frame: a loop in idGameLocal's frame calls one
re-anchor for the local player's animators and for each entity of the game's
animated list, with a precision. The player and every entity that is not an
idAI2 get `g_fixupForAdaptiveTick_PrecisionMode == 2`; an idAI2 gets
`mode == 2 || mode == 1`, and the cvar's default is 1. The two paths drift
apart by about 8 % (the imprecise one keeps a negative start fraction whose
rounding moves the start earlier at each change), so in a sync the demon (an
AI) runs ahead of the player, the sync entity and the helper bodies. DOOM
2016's own kills last about 1.3 s and hide it; a ported demon's six-second
kill ends with the player released half a second before the demon falls.

The rule: while a sync whose TARGET participant is a mod's entity (its
entityDef under `mods/`) is working, every participant's animator re-anchors
with the precision the loop gives the player. The player and the non-AI
participants already have it, so the mod's demon is what changes. Nothing of
the player changes, and a sync with a stock target is left as it is.

| Site | VK | GL | Window |
|---|---|---|---|
| `AnimReanchor` | 0x15D1940 | 0x15D2D60 | 15 |

`void f(animator, const int *now, int hz, int old_hz, bool precise, bool
is_ai)`; its only callers are the eight calls of the loop (VK 0x14038E920, GL
0x14038E1C0). One 49-byte pattern, unique in each executable. The prologue
opens with `test rcx,rcx; jz <end>`, so the window relocates that jz
(`hook_prepare_relative_jcc`: the inverse short jump over an absolute jump to
the same target). The detour is prepared at boot and committed at the first
Play that serves a mod. It passes every call through unless its precision
differs from the player's (the cvar's value, at idCVar +0x30, the field the loop
reads); only then does it look the animator up in the pass's set, rebuilt once
per pass (one `now` and one rate): each player's `mostRecentSyncEntity`
followed by its spawn id; while that sync entity `isCurrentlyWorking`, its
`activeParticipants` (`instance`, `data->type`) and each participant's animator
(the entity's vtable +0x948, the call the loop makes). The fields are placed
through the game's type tables at the Play gate. `idPlayer.syncMaster` stays
empty during a glory kill; `mostRecentSyncEntity` names the kill's sync entity
from its start and `isCurrentlyWorking` holds for the kill's length (read live,
Vulkan). A field or the cvar not found records the feature off with one line.

`sh_mods sync-clock` prints the state, the precision mode and this Play's mod
syncs and re-anchors moved; the diagnostic build logs each mod sync as it
starts, `sync-clock: <sync entity> (target <entityDef>): <n> participant
animator(s) re-anchor on the player's clock`. `sync_clock_test` covers the
rule; `sync_clock_sigs_test` the site, its relocated jz and its eight callers on
both executables; `detour_windows_test` audits the window with every other
site; `hook_test` the jz relocation.

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

A mod may carry Lua scripts: `scripts/*.lua` (entry files)
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
  `script`): `scripts/<name>.lua` or
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
  A unit walks scripts like any file (see [Units](#units)): their literals
  are names, since they may build names while running. Scripts are never rewritten with the salt (the name rule leaves them
  alone). Archives (publish,
  download, rawmap and prefab exports and imports, installs) carry scripts as
  the files they are, under their lower-case game names (every carried path is
  recorded that way, whatever its spelling on disk, and an install
  writes the names of every author folder in lower case, so an uppercase letter never refuses
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
  `play_effect(effect, pos)`, `explode` with an effect and `ai:fire` refuse ("cannot
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
  A call to a name that does not exist is always one of these stops, never a
  silent nothing (the API guard): an entity method Snapmap+ does not have
  stops the mod as soon as it is read (`e.name`, `e:name()`: `"name" is not a
  Snapmap+ call an entity has`), and a call to a global that is no Snapmap+
  call and that the mod's scripts never set stops it with `"name" is not a
  Snapmap+ call, and this mod's scripts never set it`, both followed by
  `Check the name, or update Snapmap+ if the mod was made for a newer one`.
  `pcall`, `xpcall` and `coroutine.resume` cannot catch either: a pcall handed
  such a global (`pcall(name, ...)`, the global read on the pcall's own line
  and remembered by `_G`'s `__index`) or whose function made such a call
  stops the mod as the call itself would. Reading a global that is not set is
  still nothing, as in Lua (`rawget(_G, name)` and `if name then` stay usable);
  only calling it stops.
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
  (`sh_modui_scripted_maps_allowed`) serves the gates: the download asks no
  question, so nothing says before it that a map's content has scripts. At
  every launch the gate decides (`pub_refuse_scripts`) a map with a place whose
  serving builds carry scripts (the `served_scripts` op reads each serving
  folder's `mod.json`, only while the setting is off) is refused with
  `SH_MODUI_SCRIPTS_OFF`, though it was downloaded while scripts were on; for
  content a hold fetches, the same check runs once the fetch is done
  (`pub_finish`), so the units stay in the content cache and the map is not
  loaded. The player's own maps have no place and are never refused.
  `sh_modui_scripts_approval` (the sentence of the retired download question)
  has no caller. A
  rawmap or prefab ZIP import whose carried mods have scripts is refused with
  the same sentence while the setting is off and installs nothing
  (`modexport.c` me_install), and a rawmap that came with its mods (it records
  a place) is refused at every open while the setting is off when the builds
  serving it there carry scripts (`sh_modpub_rawmap_scripts_off`, asked by
  `sh_rawmap_prepare_reload` before the engine sees the text).
- **Registration.** `on.spawn / think / remove / damage / death (name, fn)`,
  `on.animation(name, notify, fn)`, `on.input(node name, fn)`, `on.map_start /
  map_end(fn)`. Names are mod-relative (`"ai/tyrant"` is
  `mods/<salt>/ai/tyrant`); `mods/<other salt>/...` and `native(...)` names are
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
`native("...")` names DOOM's wherever a name is taken (`spawn`, the damage,
projectile and sound decls, `decl`). A removed entity answers nil.
Every argument is checked before the engine is touched, and every refusal is
one plain sentence at the calling script line (it stops the mod's scripts for
the map, as any error does).

- **Calls.** `spawn(def, pos, [yaw])` (own entityDefs only; no count or rate
  of its own, only the engine's entity table, Bounds above), `players()`; `entity:position()`, `entity:set_position(v)`, `entity:yaw()`,
  `entity:set_yaw(deg)`, `entity:axis()` (forward, left, up), `entity:velocity()`,
  `entity:set_velocity(v)`, `entity:health()`, `entity:set_health(n)`, `entity:alive()`,
  `entity:definition()` (the mod's own names as the script writes them), `entity:distance(other)`,
  `entity:is_player()`, `entity:remove()`, `entity:view_angle_to(other, [use_view_pos])`;
  `entity:animation_state(node, [via])`, `entity:force_animation(node, [blend])`,
  `entity:animation_finished()`, `entity:current_animation()`, `entity:animation_event(name)`;
  `entity:tint(r, g, b, [seconds])` and `entity:glow(r, g, b, [seconds], [strength])` (the
  entity's look, below; a glow adds its colour times `strength`, a soft 0.08 when
  none is given, so the surface stays readable under it);
  `entities_near(pos, radius)` (every entity in range, nearest first),
  `trace(from, to, [radius], [ignore])`, `ground_below(pos)` (the floor under a
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
- **Tint and glow.** An entity's look is two colours: a tint (a multiplier,
  `{1, 1, 1}` is the entity's own look; each channel 0 to 16) and a glow (a
  colour added, `{0, 0, 0}` is none; each channel 0 to 64). Setting one
  keeps the other. The look is written as the three render parms `color`
  `{tint, 1}`, `bloomcolor` `{1 + glow, 1}` and `highlightcolor`
  `{tint - 1 + glow, 0}`, each at its engine default when the look is
  neutral, so `{1, 1, 1}` and `{0, 0, 0}` restore the entity. They are the
  parms of the stock model programs: `color` and `bloomcolor` scale the
  emissive (bloom-mask) surfaces of every model program and `bloomcolor` is
  the colour the skinned demon programs add there; `highlightcolor` is added
  to the final colour of every model program, a wash over the whole lit
  surface. No model program multiplies a surface's albedo by a per-entity
  parm, so a tint reads as a product on the emissive parts and as the wash
  elsewhere. Each call sends one `lerpRenderParmFrom` event per parm through
  `sh_script_api_event`: the engine's own event on an entity, which sets the
  parm on the entity's render model and fades it in game time
  (idGameLocal's render parm lerper; no script tick drives the fade). The
  parm decls are found as `renderParm` decls by name through the engine
  table's `decl`. `seconds` fades from the look the entity has now; with no
  seconds the change is a fade of one millisecond, which also replaces a
  fade still running. The look "now" is kept by the product
  (`script_api_tint.h`: the last request per entity handle and when it
  began, 256 entities, the oldest replaced; an entity not in it starts from
  its own look). The call answers true when the engine took all three parms,
  false when it did not (the entity has no render model), and nil when the
  entity is gone or the game's parms cannot be found; a channel outside its
  range or a bad `seconds` is one plain sentence.
- **Per frame.** `sh_script_api_frame` also runs the steps other call
  groups register with `sh_script_api_on_frame` (the AI calls finish a forced
  animation through a via node there).
- **Animation.** idAI2 ignores the stock `changeAnimState` events, so on a
  demon `animation_state` and `force_animation` hand the request to the AI calls
  (`sh_script_ai_anim`): the demon is parked (below) and
  `ssaction_ChangeAnimState(Via)` or `ssaction_ForceAnimState` runs at once.
  A bare state name takes the AI's current subweb.
  `current_animation` reads the animWeb's current node (`+0x225C8` web, `+0x2271C`
  node index). `animation_event` (DEST_BLEND_START, DEST_REACHED,
  VIA_BLEND_IN_START, VIA_BLEND_OUT_START, FINISHED) and `animation_finished` are
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
  is edited and no layers config is named; a demon stays parked for the map
  unless its script releases it.
- **The release.** `release()` hands a parked demon back to the game's own
  behaviour until the script's next command: its park slot is marked released
  (`sh_ai_park_release`; the pain the park had opened for is the engine's
  now), the `IsRunningScript` detour then gives the engine's own answer for it
  (`sh_ai_park_drives`), so the alert cycle leaves `ExecuteDoomscript` by
  `Shared_NotExecutingScript` and runs the stock behaviours: 2016's search
  (`AC_CanUseSearch`, VK 0x140601110) when the demon has lost its enemy, its
  combat when it finds one. The pain and death request detour and the turn
  detour leave a released demon alone. Whatever the script left standing on
  the demon goes with the release (an order held back behind a step, a forced
  via waiting for its destination, a facing, a move), so nothing of the
  script's takes the demon back by itself. The next command parks it again
  (`sh_ai_park_take` clears the mark at every park) and the script proxy forces
  the alert cycle back into `ExecuteDoomscript`. The Enemy SDK's brains call it
  when the demon has lost its enemy, so a ported demon searches as DOOM 2016's
  demons do.
- **Commands** are catalogue events (`SH_AI_EVENTS`) run on the demon at once
  through the entity calls' event helper, which checks each one's arguments
  against the game's event:
  `move_to(pos, [tolerance], [flags])` (`ssaction_SetWalkState` walking or
  running, then `ssaction_MoveToPoint`; `strafe` or `aim_while_moving` first
  aims at the enemy with `setFocus`), `move_to_entity(entity)` (the same to the
  entity's position, given again when it has moved 32 inches; the engine's
  move-to-entity overshoots), `stop()` (`ssaction_Idle`), `face(entity)`
  (`setFocus`), `face(pos, [{ travel = inches }])` (a zero-length
  `ssaction_MoveToPoint` with align; its tolerance is the arrival radius of
  the engine's turn-clip query, which for a turn in place rejects a clip whose
  root ends farther than 1.0-1.3 x max(1, radius) from its start, flag
  `ANIMQUERY_REJECT_OVERSHOOT`, VK 0x141AD9E90 / GL 0x141AF40E0; the default
  0.1 picks clips that turn in place, `travel` admits a demon's own stepping
  turns; with `travel` an entity target turns toward where it stands),
  `aim_at(entity | nil)`, `set_enemy([entity])` (`setEnemy` locked, or
  `setPlayerEnemy` closest; every enemy setter of the game locks, and the
  lock byte, ai+0x25068, is read nowhere in build 13954591, so the lock
  changes nothing), `trigger(on, [hand])`
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
  It also takes any entity with a gore component (a mod's `idGoreEntity`):
  its model and the component's owner sit where a demon's do.
  `script_api_body.c` adds three entity methods on the same type walk as
  `e:get`: `sync_partner()` scans the map's `idSyncEntity`s for the one whose
  `activeParticipants` hold the receiver and answers its TARGET, INSTIGATOR
  and the sync entity; `wounds_open()` answers the gore node names of the
  gore component's `activeWounds` then `pendingWounds`; `break_constraint
  (name)` finds the constraint of that name in the entity's `idAnimator_AF`
  physics and removes it with `idPhysics_AF::DeleteConstraint` (VK
  0x141643050 / GL 0x141643B60, `ScriptAfDeleteConstraint`), which sets the
  figure to rebuild. Each answers nil or false and a reason when the part
  is missing; `tests/script_api_body_test.c` covers them on fake types.
  `snapmap_order()` takes the order SnapMap's Follow action
  (`idSnapMapAction_FollowPath`) left for the demon: the action writes
  `snapmap_forceFollowPath` (a path node), `snapmap_forceFollowEntity` (an
  entity) or `snapmap_forceMoveToPosition` (three float slots) into the AI's
  temp memory (+0x25B30 on both renderers: 256 32-bit values, then 256 key
  pointers to interned names at +0x400; VK 0x140439710, 0x140439680,
  0x1404397A0), and the stock FSM's snapmap state takes the entry and removes
  it. The call reads the slots (`sh_ai_order_find`, an entity order's value is
  its spawn id), removes the entry with the game's own `tempMemory_Remove`
  event, and answers nil or `{kind, target | pos}`; the Enemy SDK's
  `enemy_snapmap.lua` follows it.
  `relations()` reads the demon's social relations as DOOM 2016's senses keep
  them: the senses (ai+0x29A88) rebuild a table every AI think, whatever drives
  the demon (UpdateRelations VK 0x1404AA490 / GL 0x1404A8C10, from the senses'
  update called by the AI think VK 0x1403E0D40), one row per entity the demon
  perceives, players and bots (the idPlayer classes) and demons, with the
  respect and emotion its faction graph gives the two factions or what it
  perceived of that entity (being hurt raises the emotion), whichever is
  farther from neutral, most hostile first (rows at +0x28, count +0x30, 0x28
  bytes a row: respect +4, emotion +8, the entity's idEntityPtr +0x10). A
  demon's faction is its entityDef's `factionName`; a SnapMap team replaces it
  with `team_<n>_demon`, and players get `team_<n>_player` (the co-op mode's
  VK 0x14088FAF0). The call answers `{ entity, respect, emotion, awareness
  [, unlimited] }` rows by name (`"minion"`, `"hate"`, `"confirmed"`); the
  sigs test checks the layout on the two functions that write and read it
  (UpdateRelations, and the engine's pick VK 0x1404AAD10 / GL 0x1404A9490).
  The game never picks an enemy from the table by itself: its state machines
  do, with `Shared_SocialRelationAvailable` (VK 0x1405F4930), which a
  script-driven demon does not run. The Enemy SDK's `enemy_target.lua` is that
  condition on these rows, so a script picks the enemy the game would.
  `off_limits()` answers an actor's off-limits flag (idActor volatile
  `offLimits`, a byte its virtual +0x580 returns: idAI2's and idPlayer's VK
  0x140427550 / GL 0x140426600, `movzx eax, byte [rcx+0x9408]; ret`), which a
  sync interaction sets on its instigator (`markInstigatorOffLimits`) and
  `Shared_SocialRelationAvailable`'s `filterOutOffLimits` drops; the call
  checks the getter it finds in the vtable is that instruction before it reads
  the byte, and answers nil for anything that is no actor.
  `wound_damage_counts(on)` gives a demon DOOM Eternal's wound counter. The
  game opens a living demon's wound from `idGoreComponent::DamageActivatesWound`
  (VK 0x1408FC8E0, GL 0x1408FB910), asked per queued hit record, per damage
  group and per offered wound with the health the whole GROUP has lost: two
  wounds on one group open together on a hit inside either's spheres once
  the group lost the threshold. The call detours that function (at the first
  call, 16 whole bytes). For a demon that asked, the detour calls the game's
  function with the lost health raised past any threshold, so every other
  gate (frame damage, damage per second, gore type, distance, directions,
  the clipping spheres) is the game's own; when it passes, the record's
  damage (its +0, the hit's damage with the gore behaviour's multiplier) is
  added to that wound's counter and the answer is whether the counter has
  reached the wound's threshold (`sh_wound_count_step`; the threshold as the
  game's helper computes it, total or a fraction of max health). Counters are
  per demon and spawn id, per wound decl, under a lock (the gore think and
  the script call may be on different threads); the same hit offered twice
  for one wound counts once. They go when the handle table releases the
  demon (`sh_script_handles_on_gone`), when the call is turned off, and at a
  map's start. Stock demons call straight through.
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
  Before the break it also gives the breakable's explosion fx managers
  (idEffectPhysicsBreakable `explosionFX`, which the game starts with no render
  model) the breakable's own pieces model as their `rm`: DOOM Eternal plays a
  destructible's fx on the destructible's render entity, so its render parm
  items (an instance emissive tint and its fade, `FX_RENDERPARM`) reach the
  pieces; without a model 2016's FX_RENDERPARM update (VK 0x1416EA1D0) writes
  nothing. Only a manager without a model is given one.
  `enemy_sdk.bind` calls the brain's `bind(def)` first, where the brain names its
  breakables.
- **The Enemy SDK's target rule.** `enemy_target` is DOOM 2016's
  `Shared_SocialRelationAvailable` on `relations()`: the condition's respect,
  emotion and awareness bands (by name; its constructor's defaults, VK
  0x141302930: every respect, every emotion, every awareness), playersOnly and
  noPlayers (a bot is a player), filterOutOffLimits, the 10,000-unit range of
  GetRelations (VK 0x1404AAA30), and the pick: the nearest of the most hostile
  emotion present. `enemy_snapmap` gives a demon that holds no enemy in 2016's
  combat band (behavior_shared's: EMOTION_DESTROY_AT_ALL_COSTS to
  EMOTION_DISLIKE at AIAWARE_CONFIRMED) that pick, and a source game's brain
  evaluates its own conditions with it; nothing in the SDK reads a team or lists
  players.
- **The Enemy SDK's weapon library.** `enemy_weapon` (with `enemy_sdk`,
  `enemy_snapmap`, `enemy_target`, `enemy_locomotion` and `enemy_probe`, required by name
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
- **Queries.** `enemy()` (the idEntityPtr at ai+0x25050; nil for a player with noTarget on, as DOOM's own target queries skip it), `can_see(entity)`
  (`isEntityVisible`, which is true when the perception's record of `e` was
  seen within the last second, VK 0x1404AA3E0, and then the world trace from
  the demon's eye to 40 inches above `e`'s origin, so a wall between them
  answers false), `aware()`
  (`hasAwarenessOfEnemy`), `awareness([entity])` (`GetAwareness` on the perception
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
  `animation_fits(node, tests, [side], [seed])` reads the node's animations (VK
  0x141712C60) and their total root motion (VK 0x1415C5290), turns it by the
  demon's facing (by the direction to its enemy with
  `TF_TRANSLATION_TO_ENEMY_FORCE`) and tests the end point with the ground
  under it, the world trace at knee and chest height, sights to the enemy from
  eye and weapon height, the enemy's view centre (20 degrees) and the path
  query, as the test words ask (every animation in turn without `side`, in an order
  shuffled by `seed` when given, as DOOM Eternal's query shuffles them; each node's
  path queries under keys of their own). `VALIDATE_ANIM_DELTA` is DOOM Eternal's
  navigation line (FUN_1409007D0 through FUN_1408E2420): a clip that does not move
  passes, else the end point needs ground and a world line at knee and chest height
  that players and demons do not stop (the asking demon first among them); `force_animation_via(dest, via, [blend])` forces
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
  until the game clock passes the time; `anim_variable(name, value)` sets a
  variable of the demon's body anim web by name with the engine's own
  set-by-name (VK 0x14170A950 / GL 0x14170B3C0, on the animator at
  ai+0x222B8, the name made an atom by the same atomic-string set): the
  animator keeps one row per variable its web names (+0x580, count +0x588,
  0x18 a row) and stores the float into the idAI2 member the row is bound to
  (`idAnimWebAI::blendVariables_t`: `hangoutIndex`, `painIndex` and the rest,
  which the game's own snippets set the same way, as
  `Shared_SetScalarToProjectileIndex` does) or into the web's own memory
  (+0x598); false when the web has no variable of that name. A node whose
  blend picks its clip by a variable (`anim[hangoutIndex]`, the evade nodes)
  then plays the clip the script chose. The per-demon pain records are read
  lock-free by spawn id from the threads the engine runs pain on. Each detour
  is committed on the game thread at the first call that needs it.
- **Shared with the entity calls.** `distance`, `animation_state` and `force_animation`
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
  `animation_state` given while such a stage waits is held back and given the
  frame the stage has ended, the web having played the step out first in any
  case; a move order drops a held one. `force_animation` is given at once and the
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

- **Fields** (`script_reflect.{c,h}`, `script_api_reflect.{c,h}`). `entity:get(path)`,
  `entity:set(path, value)`, `entity:fields([path])`, `entity:class()` and `enum(name)` walk
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
- **Engine events** (`script_api_game.c`). `entity:run_event(name, ...)` runs any of the
  game's events on an entity and returns its result; `entity:queue_event(name, ...)`
  queues it; `event_arguments(name)` lists what it takes. The argument format is
  the game's own, read from the live registry at the call (whole numbers,
  true/false, numbers, positions, angles, colors, text, entities, decls); the
  leading "animation" argument of an `ae_` event is filled in. Only the few
  events that carry a kill record (`notice_` events with a `7`) cannot be
  sent.
- **SnapMap events.** `entity:signal(event[, activator])` fires one of SnapMap's
  own events on an object, so everything wired to that output runs
  (`FireOutput`); `on.signal(event, fn)` hears every object of the map fire
  it (one detour on the walk over an object's listeners, placed when a script
  first asks). Event names are the editor's (`ai_killed`, `trigger_touched`,
  `logic_fired`: the `snapListenerEvent_t` enum, any case, with or without
  `SNAP_EVENT_`).
- **Cvars and decls.** `console_variable(name)` reads and `console_variable(name, value)` sets a cvar;
  `decl(type, name)` gives a decl object with `get`, `set`, `fields`, `name`
  and `type` (its class is the game's `idDecl<Type>`). Both outlive a map, so
  what a script sets is put back when the map ends (the runtime runs each
  module's closer after the states close).
- **`entities()`** lists every entity of the map, players first.

### Mod scripts: combat calls and the events of damage, death, pain and animation

`script_api_combat.{c,h}` adds the combat calls; the hooks that deliver what
happens to a mod's entities are in `script_hooks.c` (below). A decl a call
names is the mod's own, or DOOM's through `native("...")`
(`sh_script_check_name`), looked up by type; a name that does not exist is
refused with one plain sentence. An attacker is any entity; without one the
damage comes from the world entity, as a trigger's does.

- **Calls.** `damage(entity, damage, [attacker], [from])` and `entity:damage(...)`: with
  an attacker, an actor (a player, a bot or a demon) takes it through its own
  Damage (vtable slot 130, the same virtual in every actor class: idPlayer's VK
  0x140DC3880, idAI2's VK 0x140424D30 / GL 0x140423B90, which passes it on to the
  idAI2 body VK 0x1403F8C40) with the attacker as inflictor and attacker, so the
  game's team rule (idAI2 Damage_Calculate VK 0x1403FB1D0: in a SnapMap mode an
  attacker of the victim's own team, other than TEAM_NONE, does no damage), its
  percepts and its kill credit see who hit it; without an attacker, or on any
  other entity, the entity's own `damage` event (decl, direction angles,
  activator, which credits no attacker);
  `hitscan(from, dir, damage, [range], [attacker])` (the game trace, then the
  damage event on what it hits; answers `{pos, normal, entity}` or nil);
  `radius_damage(pos, damage, [attacker])` (the engine's RadiusDamage with the
  decl's own radius, which queues the deferred radius damage the game applies
  that frame); `explode(pos, damage, [effect], [attacker])` (radius damage and
  an effect entity); `play_effect(entity, condition)` / `stop_effect(entity, condition)`
  (a condition of the entity's own fx decl: on an AI or a player
  `ae_startFX_Networked` / `ae_stopFX_Networked`, started on every machine;
  on any other entity, such as an effect entity, which answers neither
  event, the engine's own entity StartFX / StopFX); `play_effect(effect, pos, [dir])`,
  `move_effect(entity, pos, [dir])`, `stop_effect(entity)` (an effect entity of the mod placed
  through `spawn` and its caps, removed by `stop_effect`); `end_effects_at_once([effect])`
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
  every decl that was not named, runs unchanged. The same call detours the
  FX manager's request of one action, VK 0x1416ECC80 / GL 0x1416ED600
  (manager, action index and six more, all forwarded unchanged), 19 whole
  bytes. The game requests a sound item again while its sound plays by
  re-arming the action in place; its update then starts a second sound and
  keeps only the new handle, so the first plays on unstopped, for good when
  its sound event loops. Eternal requests an item again only when nothing of
  it runs or the item allows a restart before its end (the item's byte at
  +0x26, this game's forceLoopRestart). For a named decl's sound item whose
  action is requested and has started, with that byte clear, the detour
  returns without calling the game; every other request goes to the game.
  If only one of the two hooks can be installed the other still serves and
  one log line says which is missing; names are kept for the map the
  scripts run in); `sound(entity, sound,
  [channel])` (`startSoundShader`); `entity:tag(name)` (a tag of a demon's md6Def,
  a named point with a parent joint and its own offset and rotation, found
  as the engine finds aim and focus tags: the model decl's `_info` prop row,
  VK 0x1416CE1E0 / GL 0x1416CEE90, placed in world space by VK 0x1415EFAA0 /
  GL 0x1415F0CA0; otherwise a joint, from the base dispatcher's getJoint
  events; the position and forward direction);
  `entity:pain()` (the pain reaction an idAI2 is in, idAI2 `+0x5060`, nil once the
  engine's pain state clears it); `ai:fire(projectile, tag or joint, target)`
  (`ae_launchProjectileAtTarget` run at once with no animation; answers the
  projectile, caught by the launch hook; a position target is reached by
  steering; the event takes a joint and launches from the demon's origin for
  any other name (VK 0x14076E780), so for a tag the launch hook gives
  `idProjectile::Launch` the tag's place and the axis toward the target: the
  projectile, its launch effects and its trail start on the tag);
  `ai:launch(projectile, tag or joint, velocity)` (the same launch, its axis
  along the velocity, then `setLinearVelocity` with exactly that velocity in
  units per second: a script that solved an arc throws along it);
  `entity:incoming_projectiles(size)` (every projectile in the world whose
  class descends from `idProjectile`, by the game's type information, except
  the ones the entity launched: a ray from its origin along its velocity less
  the entity's enters a box of `size` (x forward, y left, z up) standing on
  the entity's origin and turned with it; `{projectile, seconds}` soonest
  first, `seconds` its distance from the box's center over that relative
  speed, the test of DOOM Eternal's incoming-projectile query);
  `ai:can_see_from(other, tag or joint)`; `ai:focus_tag` refuses a
  name that is not a tag, since the engine aims only at tags; `projectile:steer(target, [speed])`
  (`setLinearVelocity`, at the projectile's speed unless one is given),
  `projectile:gravity(units per second squared)` (the engine's `setGravity` event with
  the vector straight down: the projectile falls at its source game's rate whatever the
  map's gravity, so an arc a script solved under that gravity holds) and
  `projectile:explode()` (the projectile's own Explode, vtable `+0xAA0`), all only for
  a projectile one of the mod's entities launched. A script may hold `p` for
  as long as it likes: once the projectile is gone `p:steer`, `p:gravity`, `p:explode` and
  `p:pos` answer nil and `p:alive` false, as every call on a removed entity
  does, and the mod runs on. `ai:fire` parks the demon first
  (`sh_script_ai_park`), as a move does.
- **Events.** `on.damage(name, fn(self, hit))` with `hit.amount` (what the
  engine applied), `hit.attacker`, `hit.inflictor`, `hit.damage` (the decl),
  `hit.location` (the damage zone of the first hit) and `hit.direction`;
  `on.death(name, fn(self, hit))` once per entity; `on.pain(name, fn(self,
  reaction, damage, attacker))` when the engine starts a pain reaction
  (`PAIN_FALTER`, `PAIN_STAGGER`, ...), with the decl and attacker of the
  entity's last hit; `on.animation(name, notify, fn(self))` for an md6 row
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

A module whose unit cannot be had (D-A30) stands in as a Grid Room under its
own name. Its composition serves the module's decl (the product's grid room of
the size the map's row recorded, inheriting `maps/modules/smpgrid/v1/m/<size>`,
with the recorded bounds and portals) and its snapModuleInfo (the blank
room's), and marks the module as standing in (`sh_cache_module_standin`).
FindModule and HasModule then take the Grid Room route for it, not the bundle
route (which asked the serving index for `_combo/world.bcm` at the module's own
name and refused the map, R-LV4): `create_room` loads it into a private
palette under its own name, and every file the loader derives from that name
(collision, geometry, the light probe, navigation, the palette render model)
is the Grid Room's file of that size (`sh_grid_asset_standin_name`), while its
decl and catalog stay the cache's. So the map's instance keeps its
`moduleName`, its door slots and the entities inside, and a save writes the
module as it was. When the unit arrives, the next composition clears the mark
and the bundle route serves the real module.

### Mod virtual-texture boards

A mod's lit materials draw from the mod's own virtual-texture board: its own
mega2 family (`virtual-textures/<name>_sq<N>.mega2` in the folder, the engine's
`virtualtextures/mods/<salt>/<name>_sq<N>.mega2`), named by the
materials' `landpagefile "virtual-textures/<name>"` (the engine's `mods/<salt>/<name>`), read by the stock `skinvmtr`
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

Keepability is decided when a resource is raised, and a kept resource can be
loaded again later: a re-parse in place, a load the engine repeats. If that
load ran with a map's heap selected, what it allocated would be released with
the heap while the object stays listed at level 4, the same freed-data shape
the gate exists to prevent. So the load hook runs a load of a resource the
product keeps at level 4 inside process heap 0 when a map's heap is selected
(`sh_modserve_kept_load_moves`); the load is then attributed to heap 0, and
the resource is named once in the log (`kept resource '<name>' loaded again
with a map heap selected; loaded in process heap 0`). Loads the engine makes
without `ResourceGenericLoad` (a static model reloading its geometry when its
reference count rises from zero) are not seen by the hook; a static render
model built from a table freed that way is drawn empty by the fault shield's
static-model guard, which names the model.

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
bundles whose requirements apply for the open listing (`ms_req_conflicts`, at
every scope change): the open map's rows first, in row order, then the
undeclared ones (an editor's Toybox rows; every bundle with no map open)
oldest install first. One that asks a cvar a bundle decided before it asks at
another value loses (one `MODS: requirements: conflict:` line names both): its value is not applied, but the mod is
still served, its decls registered and its rows placeable, so a map that uses both opens and plays with the first
mod's value (the requirements report says which won). So
a map's own mods decide its cvars the same way for every player, and a mod the
map does not use never takes one from its rows. Before every registration pass, at the Play re-check and after a
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
dependents search) is not recorded. Records live in a chain of open-address
levels (32,768 slots, then twice the last; only the newest takes new paths,
until half full), published once and never moved, and updated under a
per-record sequence count, so readers take no lock and only a path's first
serve takes a writer lock. There is no fixed bound: a new path that finds no
level (one cannot be allocated) is counted and the ledger then calls itself
incomplete. It is bound in `sh_modserve_install`
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

Every mode declares a map by one rule, `sh_modmanifest_listing`: the build that
serves each row, then (where the caller knows the placed names: the editor's
load and Play entry) each further build their need closure reaches; a launched
map (My Maps, the hub, a published or co-op launch) passes its rows alone. The
save writes a row for every build the placed names reach, so the rows alone
declare what the editor did. `tests/parity_test.c` holds the rule: it drives a
map through the editor, Play, a My Maps play, the published map on the author's
and on a player's machine and both kept copies, and requires every file the
placed objects reach (found from the served bytes, not the recorded needs) to
be served with the same bytes in each.

Two layers (DESIGN-additive 2.3): while a map is open or played, the bundles
it declares serve, then the game. The listing is the declared bundles' hashes
in row order in `modserve.c`, set by `sh_modserve_scope_set` from manifest
rows (the builds `sh_modserve_row_build` answers for them) and the placed
closure (a salt alone declares nothing; the closure follows only the mods' own
names, never a file at one of DOOM's names, so a decl edit, a `native/`
replacement or an editor tweak never declares its mod by being reached: a mod
applies only in the maps it is set in, `ms_doom_shadow`), with whether a map is open, the
loaded map's place (`sh_modserve_place_set`: a hash names the root's folder at
that build, else the place's, never another map's place), and a mode,
`sh_modserve_scope_mode`: `SH_MODSERVE_MODE_PLAY` from the Play entry
(`sh_modserve_scope_source` with `SH_MODSERVE_AT_PLAY`) until the editor's
tick sets `SH_MODSERVE_MODE_EDITOR` again (`modui.c`), and EDITOR with no map.
The first hash of a salt in row order declares that salt; every other build of
it is inert for the map. Each admitted bundle gets serve flags computed from
the listing and read lock-free (`sv`: names under its salt, shadows, surfaces):

- declared: everything it carries;
- another build of a declared salt: nothing, with one exception (D26): in the
  editor, when the declared build is the map's composition (a row that names
  units), the root folder's build of that salt serves the names under its salt
  that the composition does not carry, so the Toybox offers the installed
  version's objects while every path the map's units carry stays theirs (the
  declared provider wins per path). Its shadows, surfaces and requirements
  stay off, and in Play it serves nothing;
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

A level load sees only the declared bundles' entities (`level_templates.c`).
Every SnapMap level load (`idGameLocal::LoadMap`) builds a snapshot template,
parsing the edit state, of every entityDef the engine's resource manager
holds, and the editor's passes keep every installed mod's rows' closure for
the session (the entityDef a row builds for the game included), so without a
rule a stock map's Play parsed every installed mod's entities and one that
did not parse stopped the load. The product detours the resource list copy
for the template walk's two calls only and clears every entry named under the
salt of an installed mod no build of which the listing declares
(`sh_modserve_level_hides`; editor-time bundles included: an editor tweak is
a shadow of a stock name, so a name under the mod's own salt is game
content). Both walk loops skip a cleared entry, so no template is made or
demanded for such a name; the list keeps its count, which the walk compares
with the manager's to notice a template that loaded more entityDefs.

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
  your copy and the mod borrows the game's, or put your version in
  the mod's own folder for its kind and point your decls at it"; a binary at a name the game does
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
- **The conductor.** Each `aiTypeList` item a source added whose `aiType`
  holds a number that is not a stock `SPAWN_AI_TYPE_*` bit is a ported demon:
  it gets a spawn type of its own per map, the lowest free above
  `SPAWN_AI_TYPE_ZOMBIE_UNWILLING` (`1 << 19` up to `1 << 29`, eleven per
  map), in row order then by the name of the encounter decl whose `memberType`
  names the author's number, so two packs never claim one bit. The stock flags
  the item names beside its number (`aiType = "SPAWN_AI_TYPE_BARON 1048576";`)
  say which stock demon it stands in for: the item is served with its own type
  alone and the kill score reads the stand-in (below). Every number a bundle
  wrote is served as its type wherever that bundle wrote it: its own
  `snapAiEncounter` `memberType`s and `snapAiScore` `aiType`s (the synthetic
  body mechanism, on the bundle's own file) and every `memberType` or `aiType`
  its edits of game files set (its `globalAIMods` row; a stock encounter's
  member it joins), rewritten in its edits before the apply
  (`sh_modregistry_rewrite_edits`). With every free type taken, a further
  ported demon shares the type of the first one standing in for the same stock
  demon, else the first one, with one line naming both (a degrade, never a
  refusal).
- **A map pins its types.** A save writes each ported demon's type on its
  mod's row as a registry choice, slot `spawnAIType_t/<bit>/<encounter decl
  name>` keyed by the mod's salt (`SH_MR_PIN`, `modmanifest.c`
  `mm_choices_save`, from `sh_modserve_spawn_pins`); a load gives that bit to
  that encounter's demon first while it is free, and the other demons take the
  lowest free ones. So a mask the map saved (the AI Filter's, below) keeps
  meaning the same demon whatever other mods are installed or removed.
- **Spawn types compose.** An encounter member's `memberType` is a set of
  flags: when several sources set one member (or one `memberList` whose
  statements differ only in `memberType` values), the values compose as the
  union of their words in row order instead of clashing (`decl_tree.c`
  `dt_union_types`), so two mods' demons that each join a stock encounter
  beside their stand-in both do.
- **The editor's encounter pickers grow.** The Single Demon Encounter, the
  Encounter List and the Miniboss event's boss list
  (`SH_MR_GROWN_LISTS`) offer every encounter an installed mod's root build
  spawns from a Toybox placeable of its own (`idPlaceableSnapAIEncounter`,
  `sh_modregistry_placeable_encounter`; never the encounters a port adds to the
  lists SnapMap chooses from), after the game's (the owner,
  2026-11-19: an installed mod grows SnapMap's lists; a grown list never
  declares a mod, a map is modded only when it references the mod's entity).
  They are registry files whenever such an encounter is installed, generated
  also when no map declares anything, in the editor only: in Play no
  undeclared mod serves, so the game's lists serve there, and a level load
  still templates no undeclared mod's entityDef (`level_templates.c`).
- **The AI Filter's "Include AI" flags grow.** The bitflag widget offers,
  reads and writes a type only by the name of a `spawnAIType_t` member (13954591,
  both renderers: a value whose `enumValue` names no member gets no checkbox, a
  saved word that names none is ignored, and a bit with no member is not
  written). So the product adds one member per free bit to the game's enum,
  `SPAWN_AI_TYPE_MOD_19` to `SPAWN_AI_TYPE_MOD_29` (`sh_typeinfo_enum_extend`,
  a product copy of the member array the enum record points at; the game's own
  array is never written), and in the editor the flag picker
  (`SH_MR_FLAG_PICKER`) offers each type the map gives a ported demon by that
  member, labelled with its encounter's `displayStringId` and
  `descriptionStringId`. With no declared mod there is no type, so the picker
  is the game's; it names no entityDef, so a level load stays isolated.
- **A stock entity is its stock row's** (D-A44). A mod's Toybox row whose
  entityDef is also a stock row's sorts beside it in an unstable order, so
  load and paste could bind every stock entity of that kind to the mod's row.
  After the palette build the product lists such pairs, and the editor tick's
  rebind (`timeline_tile.c`, `sh_tl_stock_row_first`) moves an entity bound to
  the mod's row to the stock row. No row is refused (an editor tweak shadows
  the stock row's own name and counts as stock), and the map's listing never
  followed the row: its placed names come from the entity's own text.
- **The kill score.** `kill_score.c` detours the map's Player Score switch
  (Vulkan `0x140890390`, OpenGL `0x14088F2C0`, one signature; its window holds
  one short conditional jump the hook relocates) and the kill's snapAIScore
  lookup (Vulkan `0x1408904D0`, OpenGL `0x14088F400`, byte-identical; it walks
  the rules' score list for the decl whose aiType meets the victim's type, 15
  stolen bytes): a spawn type the listing gave a ported demon is read as the
  stock type its conductor row stands in for (`sh_modserve_spawn_type_standin`,
  published lock-free at every registry compute), so the author's Gameplay
  Settings field of that class and the stand-in's snapAIScore decl apply (a
  mod writes no snapAIScore of its own); every stock type reaches both
  unchanged.
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
  relaxations is refused alone with its cause. The result is in the engine's
  layout (`assets/`), which the build refuses (a file outside the mod's folders) until the installer also
  lays a converted folder out by kind (owner, 2026-11-17: converting releases'
  mods is the installer's, not yet done). Moved again into the folder an
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
(`modstore.c`): the mods folder's
named mutex (`src/common/store_lock.h`), which the record lock
(`sh_mb_record_lock`) shares and which the store tool holds for its whole run,
so a second game or the store tool waits instead of overwriting. The scan
records each root salt's folder in the record's `salts` section.
Each install and verification takes it; each migration folder, a rawmap or
prefab import's install, `sh_mods install` and an export hold it around their
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
  `map-claims` sections; the claim's `mods` list names the namespaces its
  units serve (`modclaim_set_mods`), which is what the tab's count of published
  maps per mod reads (`mu_published_maps`). The map document's `asked` list
  ("Missing for players", `mu_put_asked`) reads the record's `asked` section,
  which nothing writes now, so it is empty. What none of these say is left out
  of the document, never guessed.

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

A published read's place is this machine's fact about the item, which only the
gate that takes the read can decide. Its parse sets the place pending
(`sh_modserve_place_pending`, `SH_MODSERVE_PLACE_PUBLISHED`): a row resolves
only at its own build at the root, never the root's other build of its salt.
The gate that decides the place lists and activates the rows again there
(`rows_resolve`, `sh_modmanifest_place_decided`). A published item serves the
builds it was published with on every player's machine, its author's included,
until the author publishes again (the owner, 2026-11-17); the author's edits
serve in the editor and in a My Maps play.

A lobby joiner whose map names units downloads them in the lobby (see "Co-op
lobbies for modded maps" below). A joiner whose map cannot be confirmed
otherwise (a download that failed or was refused, content that is not all to
be had) leaves the lobby through the
session's own leave call, the one `OnCacheMapForLobbyError` makes after its dialog
(slot 0x2F0, checked byte for byte at bind on both renderers), so the host never
starts it on a map it does not hold; the player is told to join again. A warm-cache join no gate of this process saw
(or whose rows are not the ones last activated) is read cold again, so
CacheReady's gate checks it. A joiner runs no mod script, so its scripts setting
never refuses the host's map; a joiner whose map passed serves in the Play
scope, as the host's launch does.

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
completion no gate knows. `sh_modpub_launch_gate` holds such a lobby launch
as any hold (its resume runs the manifest step on the retained text and calls
the launch with its flag, when the screen it was held on is still current).
An offline launch has no map object left to resume with, so nothing is
fetched for it. A row of it that names units and whose composition exists
(stand-ins for what is not here, see [The map content
cache](#the-map-content-cache)) plays, with one log line, and the load says
once what it plays without (S2 when this computer is offline, B1). The launch
is refused when a row names no unit and nothing serves it ("This map was not
loaded: it needs mods that are not on this computer and cannot be downloaded.
Ask its author to publish it again."), or when a row that names units has no
composition ("This map was not loaded: its mod content is not on this
computer. Connect to the internet and open it again."), or when memory ran
out. A launch without a parse of its frame (the lobby's own relaunch from the
map cache) is not checked. The level load at START MISSION builds from
the cached map and does not parse again, so this boundary is the last point at
which a missing mod can be caught.

The hold. When bundles are missing the gate retains a deep copy of the published
metadata (the copy constructor `PublishedMapSelectionConstruct` uses, inside
process heap 0) and the text, does what the completion itself would have done to
the screen first (`OnSnapPlay` clears its wait dialog, GDM 0x63, and hides the
progress widget; browser play clears its pending state; the joiner hides the
progress widget), and returns without the original. Each row it holds for
prints the asset rule's sentence on the console first. On a later main-thread
tick (`sh_modpublished_poll`, after every native frame, menus included) the
hold begins its job with no question: the fetch below when every missing row
names units, else the refusal below.

The map's place at the gates. Before anything counts what is missing, each
gate decides the place of the map it holds and counts its rows there without
changing what the loaded map serves (`sh_modserve_row_served_in` through the
`row_state` op): a stranger's published map's place is the one the record gives
its item (`sh_ms_place_of`), else a free one its title names, noted for the
item (`sh_ms_place_dir`, `sh_ms_place_note`, the `item_place` op), a map this
machine published included (the `own_item` op is bound, but `sh_modpub_decide`
does not call it); a My Maps play, a launch no gate saw and a rawmap use the place their
text records (`sh_carrier_place`); a prefab placement has none (DECISIONS 12).
A gate that lets a map through, and a hold that resumes it, make that place the
loaded map's (`place_set` op) and the place its launch serves from
(`sh_modpub_launch_place`, which the launch detours apply before
`sh_modui_scope_launch`); a refused launch restores the place the editor map had.
The Mods tab's tick returns the place to the root when the editor leaves its map
or a launched Play is over.

The content fetch (SPEC-distribution 5.7). Every hold whose missing rows all
name units (the completions, the launch boundary, My Maps, rawmaps, prefabs)
asks nothing and probes nothing. `pub_hold_begin` marks each row the asset
rule does not serve as missing; one job on the modnet worker
(`pub_job_main`) then runs `sh_cache_prepare` for each, one row after another
(`eng_fetch_for`; a row the content cache has not noted is skipped, so its
load stands in for it, and the place is not used: the content goes into
`.content\`, see [The map content cache](#the-map-content-cache)). Whatever the
store cannot supply stands in, so a fetch fails the hold only when it was
cancelled or memory ran out ("The mod content of <name> could not be prepared:
<reason>. The map was not loaded."). Why an object is absent is the cache's
cause (S2 not on this computer and the store not asked, which is also what a
computer whose store address does not resolve gets, B2; S4 the store could not
be reached; S6 a newer Snapmap+ is needed; S4b another game revision; S5 not in
the store; S3 removed), and the load says it once as its one notice
(`sh_cache_notice_text`).

An editor open asks no store, so its note for a salt no root folder holds
(`sh_modpub_editor_substitute`) says only what this computer knows: "not
installed on this computer, so the editor opens the map without it. Playing
the map asks the mod store for it." When the map's placed
entities name that mod's namespace (`mods/<salt>/`, read by
`sh_modmanifest_placed_names`), the engine's validator refuses the map
(`GetLocalSavedMapEdit`'s RepairAndMigrate, then its damaged-save dialog,
which the fault shield's save guard dismisses), so the editor cannot open it
without the mod. The note then says so and is also the one-button notice
raised once no gate took the parse (`sh_modmanifest_load_refusal`): "This
map needs <name>, which is not installed on this computer, so the editor
cannot open the map. Restore the mod's folder from the Recycle Bin, or play
the map from My Maps to get it from the mod store." (My Maps Play holds and
fetches the content, above.) An editor open has no hold point before the
engine's load, so nothing is fetched before it; a pinned row's units are
asked for after the open, with the map open (see [The map content
cache](#the-map-content-cache)).

The progress line. While the job runs the player sees where it is in the
game's own progress line, the one the game shows while it downloads a published
map: "Getting this map's mod content: Eternal Demons (2 of 3), 84 of 212 MB."
(`pub_progress_text`; "this prefab's" for a prefab, no total when the size is
not known yet), its text replaced in place every two seconds, never taken down
and raised again, and the console prints the same line from the second one on.
The line is not shown for the open editor map's fetch
(`SH_MODPUB_EDITOR_OPEN`), which has no completion behind it. Nothing is
asked, so the player cannot cancel it; leaving a lobby, another map load or a
newer held launch stops it.

Resuming. When the job is done, on a main tick (`pub_finish`), the index is
rescanned and every row must be served by the asset rule (else "<name> was
downloaded but cannot be used." with the rule's cause, and the load is dropped);
a map the scripts setting refuses is dropped there too (see Mod scripts). The
screen the hold came from must still be current (and, for a joiner, the lobby
must still host that item and revision), the manifest's bundles are activated,
a fresh `idSnapMap` is constructed, the retained text is parsed through the
normal detour (which strips the rows and activates), and the original
completion runs with it. The content's one notice (what the map plays without,
above) is said after the load, once. A failed fetch, a refused row or a lobby
that changed drops the hold: nothing is launched, the progress widget is
hidden, and a one-button notice names the mod. Another map load, or a newer
held launch, abandons the hold silently and cancels its fetch. The engine
thread never waits: the network runs on the worker and the job is polled.

A row that names no unit. A local row (`"local": true`, D6a) follows its
author's folder, so the store has nothing for it. When a hold's missing rows
include one (a map saved before its content was sent to the store, or a
rawmap), `pub_refuse_local` refuses the load at the next tick, before any
fetch, with one sentence: the asset rule's own when the row's folder is here
but does not serve every name the row lists ("<cause> The map was not
loaded."), else "This map needs <names>, which is not on this computer and
cannot be downloaded: the map was saved before its content was sent to the mod
store. Ask its author to publish it again. The map was not loaded." (a prefab:
"This prefab needs ... The prefab was not placed."). The long form is also on
the console. No stand-ins are built for such a row: only pinned rows have a
composition.

### Co-op lobbies for modded maps

`modlobby.c`. When a host starts a co-op lobby on a published map whose content
comes from the mod store, friends who join download that content in the lobby
and stay in it; the host's Start waits until every player holds it, the host
sees who is still getting it, and nobody enters the map with part of its
content. DOOM 2016's SnapMap lobby already carries everything this needs, read
in the engine on both renderers (R-CO1):

- The map check. The host asks each peer that joins for the lobby's map
  (reliable 4) and records the peer's map-verify state 1 until the peer answers
  that it holds the map (reliable 0x2C, sent by `PublishedMapCacheReady`). A
  joiner's CacheReady gate (above) stays held while the map's units download,
  so the joiner does not answer until its content is here. For a modded map,
  verify state 1 is "does not hold this map's content yet".
- The start. The lobby screen's Start (`TryToStartMatch`) arms the lobby
  controller's countdown (5 s when every player is ready, else 15 s), and the
  countdown's end starts the match (the controller's start step, which calls
  the session's StartMatch). The engine never waits for a peer that has not
  answered: it sends that peer START_LOADING anyway. The product holds both:
  Start while a peer of a modded map's lobby has not answered, with one
  sentence in the game's own dialog ("The match starts when every player has
  this map's mod content. Still getting it: Sam (40%)."), and the countdown's
  end when a player joined during it, which puts the controller back in its
  idle state the way its own pump drops a countdown when players leave.
- Who is getting what. A joiner tells its host its state in the lobby's ready
  command (game reliable 0xB6: its lobby user and one byte), sent through the
  lobby screen's own sender. The host's lobby applies 0 toggle, 1 set and 2
  unset to the player's ready flag and ignores every other byte; the product
  uses the bytes it ignores: 0x80 + percent while downloading (steps of 5 %),
  0xF0 once the content is here, 0xF1 when it cannot be had. A host with the
  product reads them where the engine applies the command
  (`SetLobbyUserReady`, idLobby vtable +0x210), and shows "Waiting for this
  map's mod content: Sam 40%, Robin." in the lobby's progress line while anyone
  is pending. A host without the product ignores them.
- The joiner's start. A joiner loads when its acting lobby's start byte
  (+0xF58A) is set (`State_Game_Lobby_Peer`). If that happens while its content
  is still downloading (a host without the product, a match already running),
  the product clears the byte, and the joiner leaves the lobby the engine's way,
  keeps downloading, and is told to join again.

Everything is keyed on the receiver: the host holds its start only when the
lobby's map (its launch parameters' item and revision) was seen by a gate of
this process with content rows (`sh_modpub_lobby_map_modded`). A vanilla map, a
map that needs no mod and a multiplayer lobby run exactly as the engine runs
them, and a vanilla map's joiner sends nothing.

How a download in the lobby ends:

| What happens | The joiner | The host |
|---|---|---|
| The content arrives whole | answers the map check (CacheReady runs), says 0xF0 | its Start runs |
| A unit cannot be had (the store refused it, a file failed its hash twice, the disk is full) or the fetch failed | says 0xF1, leaves the lobby: "You left the lobby: this map cannot be played without the Tyrant from Eternal Demons, which was removed from the mod store." | the player leaves; its Start runs for the others |
| The joiner left, was kicked, or the host cancelled or left the lobby | the download stops: "You are no longer in the lobby, so this map's mod content stopped downloading. It continues from where it stopped the next time you join." | nothing waits |
| The host chose another map, or a playlist moved on | the old download stops without a word; the new map's request brings its own | asks every peer again (verify 1) and waits for the new map |
| The lobby started anyway | leaves, keeps downloading, "join the lobby again when it is done" | (a host without the product) |
| The game closed or crashed | the partial files stay in `.content\partial\` and the next download resumes from them by Range | the peer drops |

A partial download is never served: a file reaches `files\` only whole and
checked. The cache sweep removes partial files of content no kept map names
that nothing has written for an hour (a running download writes all the time).

| Site | Vulkan / OpenGL RVA | Steal | What it does |
|---|---|---|---|
| `SnapmapLobbyTryToStartMatch` | 0x108EB20 / 0x108DC70 | 16 | holds the Start of a modded map's lobby while a peer has not answered |
| `LobbyControllerStartMatch` | 0xF8BDA0 / 0xF8B290 | 16 | holds the countdown's end; the controller goes back to idle |
| `LobbySetUserReady` | 0x180D340 / 0x180E130 | 18 | notes a joiner's state byte; runs unchanged for every byte |
| `SessionStateGameLobbyPeer` | 0x19937B0 / 0x1988000 | 16 | clears a joiner's start byte while its content downloads |
| `SnapmapLobbySendReadyCommand` | 0x108B940 / 0x108AA90 | called | sends the joiner's state byte (screen +0x18: the shell's manager) |
| `LobbyPeerConfirmsMap` | 0x18032C0 / 0x1803EC0 | read | its bytes fix the peer records (+0xB728, stride 0x4B8, verify +4) |

The bind checks every offset it relies on in these functions' bytes (the session
global, the game lobby slot +0x210, the parameters slot +0x2B8, the 0xB6 command,
the acting lobby call and its start byte, the peer records and the users list
+0x4E20) and installs nothing when one differs.

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

The hold (`sh_modpub_rawmap_hold`, hook `SH_MODPUB_RAWMAP_LOAD`) is the hold
every load gets (see Published maps and missing mods): the rawmap's pinned rows
are noted for the content cache first (`sh_modmanifest_content_for`), and a
hold whose missing rows all name units fetches their units with no question
under the progress line. The resume does not activate anything (the rawmap's
own parse does) but opens the rawmap again, through a reload when the editor
can take one; otherwise a notice says to load it again from File > Load
Rawmap once a map is open in the editor. That reopened load parses the text the
hold checked (`held_take` in `rawmap.c`), so a `.zip` is imported once, at the
hold; any other read of the source drops it and reads the file again. A row that
names no unit and that nothing here serves refuses the load with the sentence
described there ("... cannot be downloaded ..."); the asset rule's sentence for
each row comes first on the console. A failed fetch behaves as for a published
map. The hold is bound with the published-map gates; on a build where they did
not install, nothing is counted missing and rawmaps load as before.

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
mods/<folder>/<kind>/...    carried mods share it), mod.json and carried files byte for byte
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
pointer whose bytes an author's folder holds in its folders travels without
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
  with its own hook (`SH_MODPUB_PREFAB_PLACE`): a hold whose missing rows all
  name units fetches them with no question, under "Getting this prefab's mod
  content: ..." (the fetch goes into the content cache, whatever place the
  placement would use), and the ready callback runs the step above; only rows
  the asset rule reports missing or incomplete are fetched; the Prefabs tab,
  polling `MODS_POLL`, then places the prefab through its ordinary Load /
  Place. A missing row that names no unit refuses the placement with the
  prefab form of the sentence described under Published maps and missing mods
  ("This prefab needs <names>, which ... cannot be downloaded ... The prefab
  was not placed."). A failed fetch and a row that is still not served end as
  for a map, with "The prefab was not placed.";
- with the gates unbound (a build where the published-map gates did not
  install) the same refusal is printed and toasted without asking the store.

One hold runs at a time: a second placement while one waits is refused.

Export. `sh_prefab export "<name>" "<path.zip>"` and the tab's Export always
write one ZIP, also for a prefab that uses no mods, in a map's layout
(`modexport.h`):

```
<name>.json                the prefab file, its block naming the builds carried
mods/<folder>/mod.json     each mod the block's rows name, as its folder (the last segment
mods/<folder>/<kind>/...    of its folder below mods\), mod.json and files byte for byte
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

What is sent: the units the published map's rows name (see [Units](#units)),
those the store lacks and only those, as a map with mod rows is published. The
publish is held at its start, not at its save (`modpublish.h` is the
summary).

`PublishStart` is detoured (`sh_modpublish_publish_start`). A map whose
placed names give no mod rows publishes as the engine does. Otherwise the
call is held (the detour returns without the original; a second Publish while
one is held waits for it) and a job on the modnet worker prepares the content
(`mp_prepare`) while the editor's pacifier reads "Preparing this map's mod
content." The job makes this computer's publishing key when there is none
(`modclaim_signer`; see [The map content cache](#the-map-content-cache)),
reads the paths players asked of the units this computer's claims name
(`modclaim_asked`, which seeds the unit walk, see [Units](#units)), computes
the unit of every local row from its folder's current build (`sh_modpin_row`),
and asks the store which units it holds (`modclaim_plan_units`). A unit the
store lacks and this computer has the files of is missing (its bytes are the
files the store does not hold); one the store lacks and this computer does not
have is absent (S8); one moderation removed is removed.

- A row whose mod is not on this computer (its folder is not built here)
  refuses the publish with one sentence ("The map was not published: Snapmap+
  cannot find the mod <name> on this computer. Build or install it in the Mods
  tab, or remove its objects, then publish again."); no request is made for it.
- Bytes to send: the game's own Yes/No (GDM 0x29) asks "Publishing uploads 212
  MB of mod content that the mod store does not have yet. Publish?" (S7,
  `sh_modpublish_compose_question`). No ends the hold and the publish never
  starts, with no notice. A question that cannot be shown refuses the publish
  with "its question could not be shown", so nothing is uploaded without an
  answer. Nothing to send: no question.
- Yes: a second job uploads the missing units (`modclaim_upload`, which sends
  each through `sh_cache_publish_unit`), the pacifier reading "Uploading mod
  content: 84 of 212 MB." (`sh_modpublish_progress_text`).
- Absent or removed units do not stop the publish: one notice follows it,
  "Players will see this map without the Marauder from Eternal Demons: its
  content is not on this computer or in the mod store." (S8), or "..., which
  was removed from the mod store." when only moderation removed units.
- A busy store (429, 5xx, no answer) is waited out within two minutes
  (`MP_STORE_WAIT_MS`, `modclaim_set_budget`, the store's Retry-After honoured).
  Past that the publish goes on and what could not be sent is queued (N27): "The
  map is published, but its mod content could not reach the mod store yet;
  Snapmap+ sends it the next time DOOM starts." A refusal that will recur ends
  the publish with the store's own sentence ("The map was not published." and
  the store's cause); a store that retired this version (410) says "This version
  of Snapmap+ cannot use the mod store; run the Snapmap+ installer to update."
  A worker busy for about three minutes refuses with "Snapmap+ is busy with the
  mod store. Publish again in a moment."

When the units are in the store, or the publish goes on queued, the held
`PublishStart` runs (`mp_resume`, through its own detour, which passes) and
arms the pin step for 30 seconds. The save the publish makes then writes its
rows pinned: `sh_modpublish_pin_save` is bound as the manifest's pin step
(`sh_modmanifest_set_pin`) and replaces each local row with a pinned row naming
the units of the objects the save places, from the walk the preparation did (a
lookup, not another walk). A row that cannot be pinned is logged, and
`g_save.local` rows left local are logged at the save ("players get stand-ins
for them"). The open map keeps its own rows; only the cloud-saved text the
engine publishes names units. A publish whose pin step was not reached in 30
seconds logs that its rows were not pinned. The editor leaving, or a save while
a publish is held, abandons the hold.

The `SerializeToJson` detour reports each save's final text
(`sh_rawmap_set_save_observer`); `PublishAfterSave`, the Save state's callback
that runs before the first request to the map service, is detoured too
(`sh_modpublish_after_save`). A save without rows publishes natively. A text
over the service's 10,485,760-byte bound is refused through the engine's own
`PublishFail`, with a toast giving the size and the overage, and so is a mod
list that cannot be read ("The map was not published: its list of mods cannot
be read. Save the map again, then publish it." or the sentence naming the mod).
The rules of the store's that the
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
map needs Snapmap+. So does a map that uses a feature of Snapmap+ itself that a
game without it cannot load (`map_uses.c`: a Grid Room resized past its stock
size, a Blocking Box marked for AI navigation): every save writes the
product's own entry into the rows, `{"mod":"snapmap-plus","name":"Snapmap+",
"uses":["module-sizes","navigation"]}` (not a row: nothing downloads and no
gate holds it; `sh_modmanifest_list.uses`), and drops it when the map no
longer uses them. Cameras, timelines, water volumes and logic leave a map
vanilla. Every local save keeps the tag in step with its rows: the
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
logs that too. A refusal while the publish is
held (a mod not on this computer, a refused upload, a question that cannot be
shown, a busy worker) clears the pacifier and shows the sentence as the
editor's toast (`mp_refuse`); the engine's `PublishStart` never ran, so nothing
reached the map service. A No clears the pacifier and says nothing.

The claim and its secret (`modclaim.c`). The claim is made at
`PublishFeedPost`, the one function both success paths reach, which carries the
item id and revision (`sh_modpublish_feed_post`): there is no claim under a
provisional ref before it. The units are those the publish's save named
(`sh_modpublish_after_save` keeps them as the context of that save). The claim
is queued on the worker and never delays the engine; the item's link is
recorded with it (below). `modclaim_claim` claims the units under the item id,
replacing the set an earlier publish of the item claimed; units moderation
removed are left out of the claim (the store answers 451 and names them). Each
change of a claim carries the next `rev` (D33): the stored rev plus one, and
when the store answers 409 with its own rev a stale one is raised to it and
tried again (twice at most); a 409 that names a unit the store lacks (deleted
after its delay, or a queued upload) uploads what this computer has of it first
and claims again once. A busy store's Retry-After is waited out within the
caller's budget (`modclaim_set_budget`; two minutes for the publish's jobs).
A republish without rows releases the item's claim. The claim's secret is not
stored: it derives from the publishing key's claim seed and the item id
(`sh_key_claim_secret`, HMAC-SHA256 of "snapmap-plus/claim/1\n" and the item
id), and every write is signed by the key (`modkey.c`, kept in
`%LOCALAPPDATA%\snapmap-plus\publishing\`, made at the first publish, never in
the game's folders). The record keeps, in the `map-claims` section, per item
`{rev, units, key, mods, at}` (`mods` the salts the units serve, kept across
later claims, which the Mods tab reads) and, in `map-pending`, per item the
queued `{do: "claim"|"release", units}`; the `links` section is the
publish's own. All of them live in `mods\snapmap-plus.json`. Nothing here
touches a map other people published.

Release: the MapInfo screen's unpublish completion (`(screen, ctx, const
idStr &id)`, bound by the unpublish confirm) releases the item's claim on the
worker (`sh_modpublish_unpublished`, only when this computer holds a claim of
the item; the release goes to the store alone, `modclaim_release`, which counts
a claim neither side has as released). Deleting a map from My Maps removes the local slot and the cloud-saved
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
none within 60 seconds keeps every claim. A release the store took
forgets the link. A copy to My Maps of a published map whose
cached text (`%LOCALAPPDATA%\id Software\DOOM\base\doomsnapmaps\published-maps\<id>_<rev>_map.decl`)
is over the bound is refused at the MapInfo and browser entries, before the
engine creates the saved item, with a one-button notice. A copy that is allowed
starts a background fetch of the content of its pinned rows whose composition
is not complete on this computer (D25, `mp_copy_fetch`: one job on the worker,
`sh_cache_prepare` per row, nothing asked), so the kept map opens offline; a
fetch that did not all arrive is logged and the rest comes when the map is
opened.

The retry queue (`modclaim_queue`, `modclaim_launch`). A claim or a release the
store could not take for a reason a later try could cure (a busy store past the
wait, no connection) stays in the record's `map-pending` section under its ref,
with the units of a claim, replacing an earlier entry of that item, since a
leaked claim keeps its units on the store. At every start
(`sh_modpublish_launch`, one job on the worker) the queue is retried: a queued
claim first uploads the units the store lacks, then claims; a queued release
releases. What the store took leaves the queue; the rest stays and the worker
logs the counts (retried, settled, left). The same pass then repairs
(`modclaim_launch`, 5.21): for every claim this computer made, the units the
store lost are uploaded again from this computer's files and the claim is made
again. A unit whose files this computer no longer holds is not repaired. The
sweep keeps the units of every claim and queued claim this computer holds
(`modclaim_held`), so that repair has its files. A store answer that names a
newer minimum version raises one notice, "A newer Snapmap+ is available; run
the Snapmap+ installer to update." (`modnet_newer_needed`), after the job.

### Units

A unit is the closure of one placed object inside one mod build (`modunit.h`,
whose header comment is the source of the rules): the files the object needs
from the mod, the DOOM and product files it uses, and its share of the author's
`mod.json`. It is named by the SHA-256 of its canonical manifest (keys sorted
by their UTF-8 bytes, no whitespace), so it never changes, and the same object
from two builds whose closure did not change is the same unit. Nothing in it
names the mod's salt, name, author or description, so one unit serves under any
namespace. Nothing enumerates a mod's objects ahead of time: `sh_unit_of`
computes the unit of a name a map places (an entityDef the mod adds, a Toybox
row, a module, or `*` for the whole mod) when the map leaves the machine. Pure
file and text work, no engine calls, any thread.

The pin step (`modpin.c`) turns a map's local rows into pinned rows when the map
leaves the machine: for each object a row places, the unit from the folder's
current build, its manifest into the cache, and a module record per placed mod
module. A unit that does not prove gives the row the whole build, with one log
line; a row whose folder is not here stays local. Publish calls it twice (the
preparation computes the units to upload, `mp_prepare`, and the save's pin step
writes them into the rows, `sh_modpublish_pin_save`; see [Publishing a map with
mods](#publishing-a-map-with-mods)); export does not yet (DS-X). `sh_unit_store` writes a manifest
at `<mods>\.content\units\<hash>.json`. A unit's share of `mod.json` holds only
the keys the player's build reads (requirements, strings, hud, companions,
scenes, animation_events, attachment_tags), so a `version` or a note never
moves its hash.

### The map content cache

Format 2 rows (`modmanifest.h`, SPEC-distribution 2.7, D6a): a pinned row names
the units of the objects it places (`"objects"`, and `"whole"` for a map-level
row); a local row (`"local": true`) follows the local folder of its namespace
and names no unit until a publish or an export pins it. It carries the local
`build` it was saved from only for the format 1 import path (the places of
`mods\maps\`, still read for local rows until DS-X retires them). A save
computes no unit: a pinned row the open map serves through its composition
keeps its units; a row a local folder serves is written as a local row.

The version rule (D23, `mm_save_version`): a map holds one version of each mod.
When a save finds a mod both in a pinned row and in a local row (an object
placed from the installed version, which the editor's overlay offers, D26),
every placed object of that mod moves to the installed version: the two rows
become one local row, the open map's rows follow, and the save says "This map
now uses the version of Eternal Demons you have installed for all 3 of its
objects." (S9, through `sh_modmanifest_set_notice`). When the installed version
lacks an object the pinned row places (D23a), nothing moves yet and the notice
names the object; the objects placed from the installed version then show in
the editor only. The Mods tab's This map view offers the same move without a
placement: a row of a mod whose installed version is another build says "This
map uses another version of Eternal Demons than the one you have." with
**Update** (`mapUpdate`, `sh_modmanifest_current_make_local`), which refuses
when the installed version lacks a placed object. The engine reports no
placement, so the rule runs wherever the open map's placed names are read live
(`sh_modmanifest_version_rule` from `mu_listing_live`: a Mods tab change, the
editor's Play) and at every save.

`modcache.c` keeps one cache per machine at `<mods>\.content\`: each file once
in `files\<sha[0:2]>\<sha>` (read-only, checked when it arrives), manifests in
`units\`, and per pinned row a composition at `served\<composition hash>\`, a
built mod folder (`sh_unit_compose_ex`) whose files are hard links to the blobs
or to a local mod's files of the same SHA-256 (D21; a local file is checked
again whenever its size or time changed, D51), with `served\<hash>.json`
saying what it holds. The composition hash (`sh_modmanifest_composition_hash`)
is the pinned row's build hash in memory, and the serving index scans
`.content\served\` as an area of its own, so the map scope declares the
composition as it declares any build and a pinned row resolves to its
composition only.

Every map load (`sh_cache_on_load`, bound as the manifest's content step)
composes the pinned rows whose composition is not on disk from what is here;
an object whose unit is not here stands in (D60): every name the map places
under the namespace that no present unit provides becomes an entityDef
inheriting `snapmaps/unknown`, so the map, its logic and its saves keep the
object, and the load gets one notice (S2-S6, `sh_cache_notice_text`). The
published-map gate treats a pinned row as missing until its composition is
complete: it asks no question and probes nothing, fetches the absent units on
the network worker (`sh_cache_prepare`: the store's status for the row's
units, then the units the store holds, four at a time, each in one request,
its archive: unit.json first, which brings the manifest when it is not here,
and every file, or only the files this computer lacks, named by a mask in the
manifest's order; a transfer the network broke resumed by Range, every byte
checked, an entry that did not check fetched alone; before a unit is used, its
pointers into the product's own container are checked against the bytes this
Snapmap+ derives (N13), and one this Snapmap+ cannot match is absent with S6,
not an error), composes again
(the composition's sidecar records each absent unit's cause, the store's
answer), and resumes; the resumed load says once what the map plays without,
with the cause the fetch recorded, never the hold and the load both. A joiner
downloads in the lobby, and the host's start waits for it (see "Co-op lobbies
for modded maps"). A map's content downloads with the progress line only (D-A36;
see Published maps and missing mods): no question is asked and none can cancel
it. A computer whose store address does not resolve is offline: the objects
stand in with S2 ("Connect to the internet and open the map again"), not S4
(B2), and an offline launch from My Maps plays with those stand-ins (B1). An
editor open (My Maps, a
rawmap) composes from what is here, then asks the store for the units it stood
in for on the network worker, with no question (`SH_MODPUB_EDITOR_OPEN`: the
map is open, so nothing waits); what arrives is served in the open map through
the refresh after a rescan, and the open's one notice is what the store could
not supply, or, when the editor shows the new objects only at the next open,
that they are here.

The sweep (`sh_cache_sweep`, at start on a thread of its own, and from the Mods
tab) keeps every unit a kept map names (the local saves of My Maps, read
through `saved_maps.c`, the one reader of the game's save slots, since a map
saved offline exists only there; the cloud copies in the game's `saved-maps`;
and, unless the player chose Remove content of maps you only played,
`published-maps`), the open map's, and the units of every claim and queued
claim this computer holds (`sh_cache_sweep_kept`, `modclaim_held`, so the
self-repair of a claim keeps its files after its map left), and removes the
rest, then the files no remaining manifest names, then the compositions no kept
map uses (one being made or replaced is left alone unless nothing has written
it for an hour, and so is a partial download that long), then partial downloads
whose unit or file nothing names. The sweep also runs, off the calling thread,
after a map is deleted from My Maps and after a mod is uninstalled
(`sh_cache_sweep_async`). A kept map whose text is there and does not
read (a slot that does not decode, a row that does not parse, a folder that
cannot be listed) may use anything, so that sweep removes nothing and says
why. A row of an earlier Snapmap+ shape (`id`, `bundle`) is refused but names no
unit, so it does not count (`sh_modmanifest_list.earlier`). It never touches a local mod's files and never runs on a timer. The Mods tab's
"Map content on disk" (the `content` action) reports it from this computer
alone.

`sh_cache_publish_unit` uploads a unit the store lacks (begin, archives of at
most one request, a larger file in parts, complete), whoever authored the mod;
`modkey.c` holds the publishing key (ECDSA P-256 with CNG, the private half and
the claim seed under DPAPI in `%LOCALAPPDATA%\snapmap-plus\publishing\`), signs
every write and derives each map's claim secret. The publish hold, the claim
and the self-repair are `modpublish.c` and `modclaim.c` (see [Publishing a map
with mods](#publishing-a-map-with-mods)).

The key's own surface (SPEC-distribution 4.2 to 4.4). `publishing\active`
names the active key's file; every other `.key` file there is an inactive key,
kept so the claims it made keep their secrets: a claim's record
(`map-claims`) names its key (`key`, 16 hex of the SHA-256 of the public
text), and a claim or release derives its secret from that key, inactive or
not (`mc_secret`); a claim whose key is on no file here is claimed anew with
the active key (a lost key). A backup is `<name>.snapmap-plus-key`, JSON with
the public half, `kdf` (PBKDF2-SHA256, 600000 rounds, a 16-byte salt), a
12-byte nonce and `sealed` (AES-256-GCM through CNG of the private scalar and
the seed, then the tag); a restore checks the tag ("That passphrase does not
open this backup.") and that the sealed scalar signs for the public point
("This file is not a Snapmap+ publishing key backup."), writes the key and
makes it active. The Snapmap+ window's **Settings** menu has Publishing: Save a
Backup of Your Publishing Key and Restore from a Backup (`publishing_key.js`):
the passphrase is typed twice for a backup, the native host asks for the file
(Documents first) and runs the request on its worker through the Mods slot's
`SH_MODS_ACT_KEY` (`publishingKey`, `modclaim_key_action`), and the fields are
cleared when the dialog closes; the passphrase is never logged. The publish
that makes the key sets `publishing-key` `{"offer": "pending"}`, the list
document carries `publishingKey` (`exists`, `id`, `offer`), and the window
shows the offer once ("Snapmap+ made a publishing key for you. It lets only you
republish or unpublish your maps' mod content. Save a backup of it?", Save
backup / Not now); a backup, a restore or Not now answers it. When the window
is closed or minimized at that publish (`eng_window_open`: this process's
`SnapmapPlusStudioWebView` window), the publish's notice adds "Your publishing
key was made. Open Snapmap+ to save a backup of it." At each publish
the account name is read on the main thread with the game's
`GetProfileUsername` (VK 0x141209920, GL 0x141208c50, `signatures.c`), the
name the main menu's "Signed in as" shows, and the key's requests present it
(`X-Publisher-Name`, D31); signed out, none is sent.

The closure follows these rules:

- Seeds: the root's file; for an entityDef, the Toybox rows that place it;
  every DOOM file the mod changes that is not an editor tweak, in every unit
  (edits and replacements apply wherever the mod is used), and every DOOM file
  its `native/` folder holds unchanged (a surface: DOOM's bytes at DOOM's name,
  kept as a pointer, so a published map finds it wherever the editor did, even
  when only a DOOM file names it, such as a campaign mesh's material), though they never
  reach the mod's own entityDefs (a registry names every object); decls of a
  type the game finds by enumeration (`sh_unit_enumerated_type`: the AI scores
  and encounters, which the conductor numbers) travel with the def the
  conductor's changed copy pairs their `aiType` with; `animation_events` and
  `attachment_tags` md6Defs travel in every unit; and the requests players
  asked for and did not get (`sh_unit_env.asked`, from the store), kept with
  the unit that holds the decl that asked, or with every unit when none is
  named.
- Edges: every need the build recorded for a kept file; every name a kept text
  spells, read as the engine reads it (`sh_unit_key`); every name a kept binary
  spells, under the namespace it was written with (the block's `spelled`). A
  name reaches the file of that name with its extensions cut and the file of
  each of its leading folders.
- Whole kinds: a sound bank holding the event of a kept sound decl (a bank
  chosen once per event); a render program with its permutations and SPIR-V; a
  module's every file below its own name; a model with its mesh and skeleton.
- Scripts: an entry script comes with the defs it binds or names in a literal;
  every module a kept script requires; every string literal of a kept script is
  a name of the mod and, ending in `/`, `_`, `-` or `.`, a prefix of names.
- Boards: a virtual texture is cut at the square. A material's `landpagefile`
  and `virtualmapping` decide which `_sq<N>.mega2` squares are kept.
- Pointers: DOOM and product files a kept file reaches, with their SHA-256 and
  archive (`at` is the mod's own path where the folder holds DOOM's bytes). A
  pointer is never carried.
- The `mod` share is mod.json's author keys but `name`, `description`,
  `author` and `homepage`, filtered to the kept files.
- There is no size rule: a unit is as big as its closure.

The proof: every carried file is the build's own bytes, and every need a kept
file resolves inside the build resolves inside the unit. A unit that fails is
the builder's defect, and the caller ships the whole build for that row.

A composition (`sh_unit_compose`) is a mod folder built from units: every
carried file of every unit, each checked against its SHA-256 as it is copied,
a `mod.json` of the merged shares under a salt and a display name, built
against the game alone, with the block's `units_of` naming the units. The
provider's miss report (`sh_modserve_note_miss`) fires only for a composition's
namespace (an absent board square excepted), reported with the composition's
first unit; the asked requests the store returns seed the units of the next
publish.

### The mod store transport

The store speaks units and nothing else (`modnet.h`, SPEC-distribution 10.2).
It keeps units (named by the SHA-256 of their manifest) and their files (blobs,
named by their own SHA-256); a published map claims the units its rows name.
Reads are anonymous. Every write is signed by the publishing key through a
signer (`X-Publisher-Key`, `-Name` and `-Signature` headers, `modkey.c`), and
the bytes are untrusted: the content cache checks every manifest and blob
against the hash a map names before anything serves (I10). The build routes of
earlier releases (`/mods/build/*`, `/mods/claim`, `/mods/release`, the unsigned
`/mods/asked` and `/mods/build/<hash>/asked`) are not spoken; the store answers
them with 410, and `modsync.c` and `modexport_fetch_build` are gone.

`modnet.c` speaks the routes over WinHTTP, one process-wide session (https
only, plain http for a loopback host; no redirects). Every request carries the
User-Agent `snapmap-plus/<version>` ("dev" for a local build).

- `POST /mods/units`: each hash's state (held, missing, removed), in batches of
  4,096 (`modnet_unit_status`).
- `GET /mods/blob/<sha>`, resumed by Range from the bytes a partial file holds
  (`modnet_get_blob`), and `GET /mods/unit/<hash>/archive`, a ZIP of unit.json
  and every file or, with `?want=<base64url mask>`, only the files whose bit is
  set (bit i of the mask is the manifest's file i), also resumed
  (`modnet_get_unit_archive`): a unit arrives in one request.
- Publishing, all signed: `POST /mods/unit/<hash>/begin` with the manifest
  (the store answers that it holds the unit, or the blobs it lacks),
  `PUT /mods/unit/<hash>/archive` (a ZIP of unit.json and the missing files, at
  most 95 MB, `MODNET_ONE_REQUEST_MAX`, signed over its SHA-256) and
  `POST /mods/unit/<hash>/complete`. A blob larger than one request goes in
  parts (`modnet_put_blob`): `POST /mods/blob/<sha>/parts` starts or resumes the
  upload and answers the part size and the parts the store has, `PUT` sends each
  part it lacks, `POST .../complete` joins them (the answer waits up to 15
  minutes while the store reads the archive back to check it), and a failed part
  starts the upload again, up to four starts in one call, each sending only what
  the store lacks. `sh_cache_publish_unit` drives these.
- Claims, signed: `POST /mods/map/claim {ref, units, secret, rev}` answers the
  claim's rev (409 with the stored rev when `rev` was stale, or without one when
  a unit is missing; 451 with the units moderation removed, the claim holding the
  rest), and `POST /mods/map/release {ref, secret, rev}`.
- Asked paths: `POST /mods/unit/<hash>/asked {path}` (anonymous) tells the store
  a player's game asked a unit for a file it lacks (`eng_report_asked`, on a
  pool thread, never waited for), and `POST /mods/asked` with the claims' refs
  and secrets (signed) lists them for the publisher (`modnet_asked_units`,
  `modclaim_asked`), whose next publish seeds its unit walk with them.

Answers the client acts on. A busy store (429, 5xx, a timeout, no answer)
carries its Retry-After in the result (`retry_after_ms`), and the callers that
wait (`modclaim.c`, within the budget the caller sets) honour it; the cache
retries a unit file's broken transfer four times with a doubling pause
(`sh_cache_net_retry_ms`: 250 to 2,000 ms). A name that does not resolve sets
`offline` in the result (`MODNET_E_NET`): this computer is not connected, which
the cache says as S2, never S4, and no wait is made for it. A 410
(`MODNET_E_RETIRED`) says "This version of Snapmap+ cannot use the mod store;
run the Snapmap+ installer to update." The `X-Snapmap-Plus-Minimum` header names
the oldest version the store serves; once per process, when it is newer than
this one, `modnet_newer_needed` is true and the background publish jobs (claim,
release, launch retry) say "A newer Snapmap+ is available; run the Snapmap+
installer to update." `MODNET_E_GAME` is not a store answer: it names content
that was obtained but points at game bytes this machine does not hold, and no
request returns it.

Nothing limits a unit (DECISIONS 26): the request limit is the platform's, which
is why a file larger than one request goes in parts.

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

Static render models never build from a surface table that is not a static
model's (`static_model_guard.c`). A static render model (`idRenderModelStatic`,
0xEC0 bytes) draws a copy of its backing static model's surface table (an
`idStaticModel` on the `model` resource list). Its update
(`StaticModelUpdate`, the class's vtable +0x78, VK 0x15DD990 / GL 0x15DEA40)
copies the table when the model is new (marker -1), when the list's reload
counter moved (it first looks the backing up again by name), and re-reads it
when the model's skin or custom material changed. The copy takes each
surface's first 0x60 bytes; the render model's teardown frees the three
vertex-cache slots of every surface it holds, whoever filled them. A backing
whose table holds freed memory therefore faults the copy, and the
half-built copy faults again when the shield's recovery frees the world, the
redirect is declined and the load screen stays at 95 % (snaphak-re R-CR1: VK
0x15DDDF7 reading 0x69, then 0x17948BA reading 0x5; the same pair on OpenGL).
The guard detours the update and, only when the engine is about to read the
table, checks it: the header (count, capacity, readable data) and every
surface (a material whose first word is a vtable inside DOOM's image, readable
geometry, and none of the three slots, which no static model's table holds).
On a table that fails, the render model is pointed at the engine's own
`_empty` model with the never-built marker, so the engine builds an empty
copy; the model draws nothing for the rest of the map and one line in
`shield_faults.log` and the backend log names the backing (its name, level,
state, reference count and the first bad surface). Nothing is freed. The head
(with its security-cookie load relocated), the counter compare at +0x3E2 and
the list LEA at +0x3FA are verified at startup, and the counter must be the
list's +0x60.

The freed table the guard was built for is a session static model's own
reload. A refcounted static model (byte +0x68) frees its surfaces when its
reference count falls to zero and loads them again when it next rises:
IncReferenceCount (`StaticModelIncRef`, VK 0x17AD640 / GL 0x17AEA00) calls the
model's load (vtable +0x40) directly, not through `ResourceGenericLoad`, and
the load takes the surface table and geometry from the current heap. In the
editor after a map change that is a map heap, which the next Play load
releases; a level-4 model is not purged, so it stays listed with a freed
table. Live (R-CR1, Vulkan, fresh boot: Tyrant map Play, Baron map Play,
Tyrant map, berserk map, Play) it was the door cap
`models/snapmaps/door_caps/cap_05.lwo`, level 4, refcounted, 11 surfaces. So
the fault shield detours IncReferenceCount: when the call is about to reload
the surfaces of a level-4 refcounted model with a map heap selected, it runs
inside process heap 0, and the model is named once (`static-model-heap:`).
The 15-byte head is verified at startup.

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
