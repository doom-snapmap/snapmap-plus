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
| Backend | Bootstrap, hooks, editor operations, packages and installed-resource access. | `src/backend/dllmain.c` |
| Shared interface | Matched DLL contract, callback bindings and shared queues. | `src/common/snapmap_plus_iface.h` |
| Frontend | Companion window, messages, editor views and preview controls. | [WebView UI](webview-ui.md) |
| Fault shield | Selected engine-fault recovery and local crash evidence. | `src/fault_shield/fault_shield.c` |
| Installer | Verified deployment, migration, update and uninstall. | [Installer README](../installer/README.md) |
| Website services | Community authentication/content and feedback relay. | [Services](services.md), [feedback](feedback.md) |

The backend is an XInput proxy named `XINPUT1_3.dll`. It forwards controller calls
to the system library and initializes the editor integration. The frontend is
`snapmap-plus-ui.dll`. Both are built and deployed together; mixing their versions
can call the wrong interface slot. See [packaging](packaging.md).

## Engine resolution and patching

Function signatures identify engine code. Generated anchors derive global
addresses from code references. Vulkan and OpenGL share these portable resolution
paths; field layouts still need explicit verification when porting.

A pattern that matches more than once is not an identification. Where two
functions share a body, the caller must confirm its target by something the
pattern cannot carry: the rawmap branch answer follows the call its candidate
makes and reads the literal that callee names, because the sibling it must not
patch differs only there.

A raw-RVA fallback requires an exact fingerprint of a supported reference
executable. The hook-tolerant fallback additionally checks the detour and remaining
signature bytes. A filename or a readable address is not sufficient evidence.
Contributor changes to signatures or anchors require the two-image test gate in
[contributing](contributing.md#7-run-the-tests).

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
panel. The panel owns an Apply/Cancel draft tied to the current editable map.
Apply stores a versioned `smp.render.v1` string in native map variables; the
native serializer carries it through normal saves without a sidecar. The
reserved value contains view distance, fog strength, start/end and linear RGB.
Version 3 stores only these seven values. Version 1 and enabled version 2 values
are read directly; disabled version 2 maps migrate to native defaults. Missing
metadata also uses native defaults. Save/Play conversion writes the current
canonical values without reallocating an unchanged string or shifting variable
indices. Malformed or duplicate metadata disables the
override. The reserved variable should not be edited through generic variables.
Map deserialization clears the previous runtime selection and reads the loaded
map's variables. Editor-to-play conversion refreshes that selection from the
current editable map. A locked value copy crosses to rendering, where the
blended environment's literal view-distance and fog values are changed independently:
view distance 8192 or zero leaves native distance intact, and fog strength zero
leaves native fog intact. Fog range/color values do not enable an override while
strength is zero. The controls are always editable and there is no stored enable
flag. Overrides run before
the engine reads its far clip. This applies only to SnapMap and uses signed
bindings in both renderer executables. No WebView interface change is involved.

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
placement and every connected neighbor on each edit. Planning uses temporary
spatial views and native bounds helpers before creating resources. Signed-short
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
retained, with extensions through `+0x338`; the current table occupies `0x340`
bytes. The last three are the File menu's rawmap surface: `rawmap_status`
(`+0x328`), `rawmap_configure` (`+0x330`) and `rawmap_load_now` (`+0x338`).
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

Package, navigation and staging maintenance run after a successful native engine
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

Prefab Load/Place stages data and queues the native paste action after checking
editor state and the copy/paste cvar. Queue acceptance does not confirm that the
engine has entered placement mode. The engine consumes that action later.

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
player's installed game and enabled packages. The product does not embed DOOM
assets. Resource caches and package snapshots need explicit reader lifetimes;
rescanning must not invalidate storage a reader or native object still uses.

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
Play always takes its own complete snapshot. The worker and session log handle
have process lifetime; DllMain must not wait on them or acquire their locks.

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

Override misses carry the package generation that produced them. A rescan makes
old misses inapplicable even when an earlier lookup completes concurrently.

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

## Packages and refresh boundaries

A package has one `package.json` with identity, requirements, strings and supported
presentation policy, plus engine-shaped resources under `assets/`. The outer
package is the intact delivery unit, including nested components and auxiliary
files. Discovery, semantic compilation and native publication are separate
operations. Incomplete inventories must not publish a partial package set.
Author-facing format and examples live in the [player guide](../site/snapmap-plus-guide.md#overrides).

The local authoring library isolates defects that belong to one outer package.
`sh_package_sources_scan_local` skips a package whose own content is invalid:
its descriptor, an unusable or case-duplicate name, a link, or a path the
runtime cannot address. The compiler reports an invalid package through
`invalid_package` for bad per-package policy, text that is not a declaration, a
path under `generated/decls` that is not a declaration identity, a contribution
the declaration parser cannot read, and a resource that fails composition again
with only that package, the installed original and product defaults. Typical
typed defects are an unknown class, a parent no package declares, an inheritance
cycle inside the package, an out-of-range fixed array entry or a contradiction
of a product default. The runtime removes that whole package with its nested
components and compiles again. A local resource conflict between independently
valid packages reports its changing owners through `conflicted_packages` and
excludes those complete units together. Scalar field conflicts identify every
source editing that value, preserving independent edits to the same resource.
Structural conflicts and adapters without precise attribution conservatively
identify the resource's changing owners. This includes duplicate contributors;
none becomes a winner through discovery order. Policy conflicts identify all
incompatible owner pairs, preserving unrelated policy keys and packages.
The runtime recompiles the remaining library, including built-ins. Valid excluded
peers remain in the physical installation inventory: a map's own valid authored
provider can use their available paths without another installation. Invalid
packages do not establish availability. Source files are never removed.

Storage, hashing, read and allocation failures, sources that change during a scan,
a parent that only another package declares, unavailable reader metadata and
original failures are not content-isolation decisions: they preserve the previous
provider. Startup package identities and map installation planning use the same
separation of inventory and effective compilation; a map-carried bundle is still read with
the strict all-or-nothing scanner. Skipped folders and reasons appear in
`sh_packages`, never as silent omissions.

Map admission and save failures carry the first diagnostic back to the native
notice and backend log. JSON validation includes a byte offset and rejection
reason; preparation distinguishes inline dependency traversal from ownership
selection, and HUD activation has its own diagnostic. Embedded archive identity
errors include the supplying package ID. These diagnostics report where the
product refused data; they do not establish that the engine would refuse the
same map or that a package's assets are corrupt.

Native map/state strings follow DOOM's byte-string contract. The shared JSON
parser exposes explicit `sh_native_json_*` entry points for map admission,
save preparation, dependency traversal, session extraction and native grid-room
string decoding. These retain unescaped high bytes verbatim, including older
Windows text mixed with UTF-8. They share syntax, escape, duplicate-key and depth
validation with the strict parser; they do not infer a code page or rewrite a
save. Configuration, package descriptors and embedded archive metadata still
require UTF-8 through the ordinary `sh_json_*` entry points. Override-folder
migration is separate and cannot repair or invalidate a native map's text.

Editor definition collections include input-action and output-listener lists.
Their declaration paths are semantic identities; adding an action in one
package and another in a product default combines them through the same scalar
collection composer. Identical entries deduplicate, compatible ordering is
preserved, and deletions are evaluated against the original. These editor
capabilities carry no gameplay ownership and do not embed their package merely
because a map uses the exposed vanilla entity.

Discovery uses wide-character enumeration, so Unicode and long group paths are
found. A marked folder whose path does not fit the runtime package record is
reported as a skipped package. The data root itself still comes from the ANSI
`SHGetFolderPathA`; a profile path outside ASCII remains unsupported.

Local libraries use the current layout. The installer converts old folders
before replacing the DLLs and exposes that conversion as
`snapmap-plus migrate-overrides`. Its contract, including original SnapHak
onboarding, is in the [installer README](../installer/README.md#converting-overrides-from-earlier-releases).

Old map-carried packages convert before current descriptor validation. Descriptor
parsing canonicalizes IDs with ASCII case folding and trims surrounding JSON
whitespace, while preserving display names and authored source bytes. The same
normalizer serves migration and map carrier identities. A UTF-8 BOM is accepted;
invalid punctuation, internal spaces, embedded NULs and corrupt JSON still fail.
The installer routes noncanonical IDs through the shared planner before strict
output validation, writes the corrected descriptor and retains the original backup.
Unchanged installed declarations retain the engine's completed fallback behavior:
the compiler records exact equality against game originals, and native admission
permits a completed default only for those bytes. Modified declarations still
require a successful parse; pending loads, exceptions and the editor palette's
entity contract remain failures. This handles uncooked campaign placeholders
without rewriting source data or adding package-specific exceptions.

`package_migration.c` supplies the shared, read-only planner for descriptors,
namespaces, manifests, requirements, strings and HUD rules. The backend links
it directly; the installer embeds a private DLL built from the same source.
The Go adapter owns discovery, verified filesystem snapshots, installed-file
reads and recoverable publication. `package_legacy.c` owns verified ZIP input,
nested components and private map-context output. Original transport checksums
are verified first; conversion never rewrites the original save or local
library. Current archive bytes pass through unchanged. Imported campaign
records use their resource name when the index filename is empty, and bytes
identical to all SnapMap providers stay installed dependencies.

Installation still depends on resource availability, not package folder names
or descriptor equality. A migrated map activates its authored resource view;
it does not substitute an installed variant's values. Missing resources require
consent to install complete converted bundles. Declining leaves the library
unchanged. After the native provider releases a private context, filesystem
locks defer deletion of that retired cache without preventing another map load.
Failures to release a live provider or roll back an installation remain gates.

Save transport identifies each complete source tree independently of its descriptor
ID. This prevents two different same-ID variants from replacing each other's shards;
identical complete copies need only one payload. The existing shard grammar remains
unchanged, and descriptor-ID carriers are still accepted. Complete-tree keys are
verified against the original archive before any conversion. Installation keeps all
variants required by the current map, including those already present when only a
different variant is missing. Published folders use package IDs with numeric suffixes
when needed; authored descriptors and member paths remain intact. Save packing also
rechecks the selected tree identity so changed sources cannot publish a mismatched map.

Compilation includes built-in overrides and package sources relative to verified
originals. Compatible changes compose using native field and collection semantics;
contradictions are reported before publication. Opaque payloads are selected whole.
The provider never modifies authored sources or installed game archives. Native
reader and resource-family details belong alongside their implementation.

Registered semantic collections normalize their count, index gaps and identical
duplicate entries in private parsed trees. Different entries claiming one identity
are rejected. Single contributions take the same validation route; an already
valid contribution keeps its exact bytes. Fixed native arrays and unadapted
index-referenced collections never undergo this renumbering.

Native inherited declarations can instead contain sparse slot patches with no
count or member identity in the child (for example, only `item[4].descriptionTag`).
These retain their inherited indices and compose independent field edits at the
same slot when the parent layout is consistent. They are not full collections
with missing entries. Mixing complete layouts with unresolved inherited patches,
or changing the parent while composing those patches, requires resolving that
layout and is refused rather than guessing identities from numeric positions.

Grouped collection adapters separate stable member identity from presentation
placement. All SnapMap editor property sheets use this contract: a property's
`path` remains its identity across pages, and adding/removing pages or moving a
property can compose with independent edits to its fields. Ordering constraints
apply within the final page; conflicting moves, deletions versus edits, ambiguous
duplicate identities and ordering cycles remain conflicts. The reusable adapter
is registered by the product for known formats, never by an author switch or a
specific resource filename. Sources and embedded delivery bundles stay intact.

Package inventories and resource ownership grow with the installed set. There
is no fixed package-count quota in discovery, compilation, saved-map selection,
or installation. Duplicate resources keep every contributing package as an
owner; their authored directories remain intact. Internal ownership sets are
never serialized into author descriptors. A failed allocation or incomplete
scan refuses the new inventory and preserves the published compiler snapshot.

The override provider serves file content and an immutable declaration view.
The declaration server registers new identities and refreshes existing shadows;
the installed-resource bridge supplies data already present in the game. A
package can therefore refer to installed assets without redistributing them.

Declaration classification reads source records and the live registry without
loading content. Native `DeclFind` can materialize archive resources even with
`makeDefault=0`; using it after the resident snapshot creates consumers that miss
the refresh and retain pointers into reconstructed dependencies. Materialization
starts only after the complete refresh set has been reconstructed and marked.

An existing loaded declaration is reconstructed only when the provider diff or
the captured native dependency graph selects it. Merely remaining in a package
does not invalidate its caches. Recovery retains the affected identities even
if a failed parse erased their dependency edges. Archive aliases count as direct
changes even when the native path names cooked data. Compiled declaration
identities also travel with the provider diff: a package-only source
has no archive alias, and its removal must retire the native object rather than
attempt to reload the missing source. Reflection-verified native
parent links also select declarations that copied fields from a changed ancestor,
including startup children whose lifetime has not yet been promoted. Ordinary
startup consumers and runtime map-owned consumers keep their lifetime exclusions.
After settings reload, native
World Text font resolution runs before palette previews, under the same heap
and renderer scopes as material rebinding; parsing font names alone does not
restore the editor's cached font objects.

Boot binds runtime dependencies even when no package declarations are present.
Runtime refresh is a main-thread operation at a settled My Maps browser, with
the editor inactive, no loading transition and no dialog. Unsafe requests remain
pending until these conditions hold. Refresh drains required commands
before native publication. The entire native refresh uses verified process-heap
scope so palette arrays and declaration-owned strings survive map teardown;
resource-level promotion separately retains the engine's resource objects.
Every exit restores the previous heap scope. Failed admission or restoration
keeps package-dependent maps gated.

Refresh includes existing shadows and string changes, not only new declaration
names. Reader synchronization keeps old snapshots valid until readers release
them. Success includes materialization, required visibility and palette refresh;
registration alone is insufficient to authorize loading a package-dependent map.

The registered installed-resource inventory determines whether a map needs an
installation. IDs, grouping, fingerprints and different local values are not
installation requirements. Private incoming map sources cannot satisfy that
installed-inventory check. Gameplay ownership selects complete authored bundles;
unchanged vanilla SnapMap resources and built-in editor exposure add no delivery
owner. Mixed bundles retain their editor support when gameplay selects them.

Local-library and map-authored views have separate ownership. The map view is
authoritative in Play and Edit; retirement restores the local view without
rewriting its files. Saving retains incoming bundle provenance even when existing
resources allowed installation to be skipped. Declared resource-loading permissions
remain active for installed packages; unused gameplay presentation policy does not.

Consent covers complete supplying bundles for missing resources. Preparation,
publication, activation and native load form one transaction. Successful activation
continues the retained load request; cancellation, interruption or failure rolls
back the entire new install group. Recovery records prevent admission of an
uncommitted group after interruption. Restoring the prior provider precedes native
consumer recovery. Open streams retain immutable sources until released. No game
restart/reset or forced map reload is an installation recovery mechanism.

`decl_server.h`, `overrides.h`, `resource_bridge.h`, `strids.h` and `map_package.h`
define the detailed contracts.

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
