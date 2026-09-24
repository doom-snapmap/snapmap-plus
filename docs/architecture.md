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
retained, with extensions through `+0x340`; the current table occupies `0x348`
bytes. The File menu's rawmap callbacks occupy `+0x328` through `+0x338`.
`prefab_export` at `+0x340` returns one complete process-heap allocation,
which the frontend releases with `HeapFree(GetProcessHeap(), 0, bytes)`.
The fixed object records a marker and table extent in formerly reserved bytes
at `+0x08` and `+0x10`. Check `sh_iface_prefab_export` before reading the new
slot; an older zeroed marker safely reports that export is unavailable.
Static assertions pin the layout. Do not insert, reorder or repurpose
existing slots. Add new capability slots at the end and update both DLLs.

Headers define buffer ownership, return values and thread requirements. Keep
engine pointers inside the backend where possible. Copy values across the
boundary rather than exposing mutable engine storage. An accepted asynchronous
request is distinct from completed engine work.

Prefab export marshals native serialization and source capture to the engine
thread, then transfers the completed bytes to its caller. Existing bounded
export slots return zero if the complete payload does not fit. The owned slot
and prefab import queue have no fixed payload buffer; native signed string
lengths and allocation overflow remain checked.

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

Portable imports retain their original destination editor, map and load
generation while compilation or consent is pending. `package_document` supplies
the same source interpretation and typed rebinding used by map preparation;
`prefab_import` drives admission only from the editor's verified pre-Think frame.
Availability uses the imported document's actual resource closure. Covered
resources do not prompt; missing resources request complete supplying bundles.
Leaving the destination cancels that request rather than placing it in another
map. Native staging and source ownership remain retained until either successful
finalization or verified rollback. Imported content does not replace the current
map's provider or authorize a global palette rebuild.

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

### Package linker construction

The variant linker is under development. Local library preparation now calls
`package_library`; historical map preparation reconstructs its recorded composed
sources before assigning native identities.
Native implicit-binding coverage and live acceptance remain unfinished, so this
route is not yet ready for deployment. Its source
representation stays `package.json` plus `assets/`; compiled
identities are internal. Gameplay resources bind within their authored package
context. Shared editor collections retain native composition semantics, and
map-wide settings still require one compatible effective policy. Existing maps
must retain their historical composed bindings when loaded and saved again.

`package_library` compiles each lexical component before comparing peer gameplay
edits. The source walk retains its explicit dependency edges and propagates
gameplay ownership through them, including cycles. Eligible shared editor and
editor-only entity declarations compose using the existing collection adapters;
editor definitions that reach private gameplay stay in that gameplay scope.
The second binding pass projects those resources into the runtime provider and
retains an internal source-context index for later map selection. Whole authored
trees remain unchanged. Local/delivered folder conventions and matching package
IDs do not suppress contributions in this compilation context. Historical
composition and policy views retain their previous interpretation separately.

Weapon HUD metadata binds the `weapon` declaration family after resource linking.
Owner-tagged rows retain the actual native alias through source projection,
map overlays, usage selection and occurrence-provider retention. Raw descriptor
keys never replace those rows during later policy reconstruction. Distinct weapon
bindings retain distinct presentation; equal bindings dedupe and choose `weapon`
mode for differing peer requests. Map-authored rows override local rows for a
shared weapon. The compiler and native HUD consumer share one schema codec, and
a failed table refresh retains the previous policy until transaction recovery.

Document contexts give changed editor declarations private identities as well.
Reusing an original editor name requires exact native source bytes and unchanged
reachable dependencies. This preserves vanilla names without treating arbitrary
native readers as name-independent. Local authoring still composes its shared
editor extensions; imported authored differences cannot rewrite those live
objects. Temporary imported palette entries extend name lookup only on a native
miss and retain exact source ownership independently of staging or placed entities.

Dependency classification is not a native-original certificate. The local-library
stage resolves `landPageFile` from scoped String defaults and ordered material
writes, asks its native adapter for the actual page provider and resolved data
identity, and includes the resulting atlas binding in deduplication. After linking,
it prepares owned alias rows and transfers the view to the runtime provider. A
missing adapter still refuses renamed non-generic materials. Production now uses
`package_native_assets` for loose original tables and the engine-owned `_vmtr`
provider. It pins original files without reading unrelated texture pages; an
authored page copy must pass a complete byte-identity comparison before sharing
that provider. Private page providers remain unfinished. The adapter admits the
thirteen intrinsic images registered during startup in both renderers as
engine-owned resources, retaining their native names. This is a finite native
identity set; arbitrary underscore names and missing source files are not
admitted. References to a cooked image file remain distinct from an intrinsic
image even when their basenames match. Other zero-file native families still
require admission rules. Both compiler passes and material inspection share captured original
bytes, absence and read failures; they cannot reread different defaults mid-build.

Articulated-figure inspection and relocation share one native grammar reader.
Settings bind the MD6 definition and clip material; custom body shapes bind a
collision-model resource. Joint/body names, constraints, geometric shapes and
solver settings retain their authored bytes. Equal figures deduplicate only when
their resolved dependencies also match. Nonempty skin fields still require a
verified consumer, and collision-model payload relocation remains separate from
identifying its reference; neither is treated as a dependency-free success.

Native original snapshots are retained across provider refreshes. Capture and
memoized identities are synchronized so preparation and availability queries can
overlap without moving another reader's file position or invalidating its data.
Pinned originals deny writes and deletion until their last owner releases them;
editable package sources retain the existing verify-before-publication contract.

`decl_rebind` uses the dependency schema's verified reference sites to prepare
declaration and inline-state bindings. It does not search arbitrary string values.
JSON sites point into the original document, so unrelated bytes, whitespace and
native map strings survive unchanged. Unsupported coverage or a failed resolver
publishes no modified output. `resource_rebind` provides the same transaction for
verified idStr records in static and discrete models, MD6 meshes and animation headers; it
preserves geometry and other opaque bytes. Format relocation alone does not prove
native cache, checksum or late-reference behavior. Discrete-model inspection
walks counted geometry records to locate material and optional base-collision
references, preserving the other bytes. Equal binary payloads with different
material bindings produce distinct model identities and distinct referring
breakables. Static particle meshes use a checked stamp/count/vertex envelope;
they contain no resource-name records, so their vertex bytes remain untouched.
Their material dependencies belong to the consuming particle declaration.
Uncatalogued images, discrete models and particle meshes acquire their identity
from typed consumers before unused files receive a cooked-file identity; the
compiler does not invent a second source extension for the same file.
Particle meshes use the filesystem's native generated-filename rules, shared
with shader path generation: replace the extension and partition long basenames.
This differs from the image/discrete-model filename rules. Private names must
resolve to the same physical path the corresponding native consumer will open.

`swf_binary` traverses cooked UI movie dictionaries and sprite envelopes. It
reports direct material/font idStr records and exposes command payloads without
treating local text or script bytes as arbitrary resource paths. Its paired
filename helper follows the native constructor: derive the atlas with the
filesystem routine, then derive the movie from that atlas name.

`package_movie` makes a cooked movie one link unit: the `file` movie and its
`image` atlas, captured in the movie's scope and then from originals. idSWF
pointers and idSWFFile values name it through their registered readers; a
relocated pair writes back a constructor spelling that derives it. Dictionary
material records are rewritten like other idStr sites. Dictionary fonts are not:
the native resolver makes language decisions from their original names, so each
font, the resolver's fixed fallback fonts and the constructor's fixed GUI
materials become constructor slots. Absent requests keep the native default.
While a compiled movie constructs, `resource_graph_native` holds its plan and
routes only requests bound to a different identity; nested loads never inherit
it, and an unpreparable plan keeps native lookups with a diagnostic. Atlas
materials are generated from the movie's own name and atlas, so a private pair
yields private generated materials. ActionScript requests made after
construction (sounds, font effects and local strings) still resolve by their
original names; they are not yet routed through the movie's plan.

`font_binary` checks the native glyph header and counted glyph/index records,
preserving uninterpreted metric bits and unread trailing data. It also derives
the selected glyph filename and its main/world-text/label material names using
the native space/hyphen normalization. These names are implicit dependencies;
they are not serialized resource strings inside the glyph file. Distance-font
selection, optional material availability and the movie's original-name/language
fallback decisions remain consumer responsibilities. This envelope layer alone
does not admit a complete linked font or substitute a default for an unusable file.
The native naming adapter keeps the 48, 64_df and 12 glyph-file forms together
under one font identity, including bounded collision-retry aliases. It validates
the original physical filename of each present form rather than treating every
dat file as interchangeable. This is a naming contract; it does not independently
prove the font's implicit material and language-dependent bindings.

`package_font` captures the main material for each nonempty glyph form and the
optional world-text/label roles. The source linker resolves these in the font's
authored scope, then verified SnapMap originals, and freezes them as constructor
edges. A campaign-only optional original is unavailable unless supplied in that
scope; a failed read aborts capture instead of pretending the material is absent.
Uncatalogued glyph files first belong to any typed consumer that names them;
remaining files receive their native path-derived font identity. Missing and empty
selected forms use the native 12.dat fallback. Glyph bytes are never rewritten.
Exact installed originals whose envelope the native loader rejects (older idf*
files still ship) capture no requests and keep the engine's own failure path.

These edges drive exact dedupe, private identity assignment, consumer catalogs and
saved-context reconstruction. The routing helper matches only the proven material
requests derived from a font's own native identity. It does not establish a global
package namespace. The native font-read adapter holds the active provider's
complete plan, prepares its bound materials without defaults and supplies those
objects at generic materialization and the existing DeclFind boundary. Captured
optional absence bypasses the cold source probe. Nested loads, direct parsers and
entity-state readers mask the outer plan; nested fonts acquire their own plan.
Holds and masks unwind on native exceptions. Uncompiled fonts retain ordinary
resolution. The engine still selects the glyph form and performs 12.dat fallback;
the adapter does not change the global distance-font setting.

The native asset handle admits fonts only with a configured, ready constructor
adapter. This capability is process wiring, not package metadata. UI movies
likewise require their constructor route; see `package_movie` above.
Native-shaped callback tests and two-image signature checks do not replace
live acceptance.

Native custom readers preserve animation state paths, animation alias handles
and C++ class names as data rather than treating their spelling as resource
paths. Other handle readers remain unsupported until their native contract is
known. Numeric table declarations use their own curve grammar in both source
inspection and rebinding; they carry values and no resource references.

`package_link_graph` determines which resolved resources can share an engine
identity. Nodes compare native family, original path, content identity and bound
presentation/reader semantics. Dependency bindings refine that partition until
stable, including cycles. Exact duplicates share a class regardless of delivery
grouping or package metadata; identical model bytes with different materials do
not. Only predecessors affected by a split need reconsideration. Class numbers
are temporary compiler data, not persistent map or package identifiers.

`package_scope` supplies a borrowed source view with original ownership indices:
the component's own assets override enclosing components, followed by the game
baseline. Sibling components and other delivery units do not fill its gaps.
`package_compilation_project` publishes proven equivalent contributions at one
engine path while retaining every original source owner. Private paths carry
their original authored path for installation checks. Source grouping, package
IDs and display names do not create duplicate gameplay instances.

Historical composed contexts select their recorded delivery owners through
`package_scope_composed`; isolated additions use their own component views.
Both retain original source indices. Source provenance resolves the full tree
fingerprint and component-relative path, returning every identical delivery
copy without relying on folder names or descriptor IDs. Composed tree selection
requires every recorded tree and never substitutes a same-ID local package.

`package_provenance` retains source contexts and the native reader, original name,
saved name and supplying contexts for each used binding. One shared asset can
retain several authored sources. Its internal payload is canonical across
insertion order and exact duplicates; it does not include host paths, author IDs
or asset bytes. `map_provenance` transports it through the existing shard envelope
as `smpbind` variables, separately from intact package archives. Reading, replacing
and stripping this metadata preserve native map text bytes and other payload
families. Empty binding sets emit no metadata; absent metadata means historical
composition. `package_link_provenance` captures selected map roots from the linked
equivalence classes, retaining every equivalent supplier in one saved binding
and excluding unused library contexts. Source reconstruction, linking and
transport have synthetic integration coverage. Shared shard removal allocates
for actual matches and refuses an exhausted removal budget atomically.
Saved-map and retained-request preparation read this envelope before resource
availability checks. Recorded contexts compile strictly from the map's source
inventory. Typed rebinding replaces only observed native resource string sites,
including instance fields; prose, editor data and legacy text bytes stay exact.
Failed reconstruction publishes neither a partial buffer nor a partial provider.
Native parsing strips binding carriers alongside package carriers.
Deferred native cache selection receives that same compiled, stripped buffer
through its activation callback. Captured metadata continues to identify the
original request; its earlier cached source is used only for the preliminary
lobby projection. Full gameplay decoding must not reread that original source
and thereby discard the prepared resource aliases.

Save selection acquires delivery ownership and binding provenance from one
provider snapshot. It selects used declaration and binary identities, including
all permutations, and carries every complete source tree required by their
contexts. The map's context wins over equivalent local providers. Unused contexts
are excluded. Historical providers retain one composed context internally, but
legacy-only saves emit no new binding envelope; adding an isolated variant retains
both contexts. Package packing and binding insertion must both finish before the
save output is replaced. Files changed after selection fail that save atomically.
Runtime activation, rawmap routing and delivery selection have offline regression
coverage; this is not evidence of completed engine-family or live acceptance.

`virtual_material` reads the engine's separate material-to-atlas table and projects
deterministic alias records for one resolved atlas. Distinct material objects can
share a texture region; duplicate bindings collapse. Atlas provider identity must
be bound separately from rectangle coordinates. `virtual_material_native` owns
private ABI rows; each native material has an independent mutable association.
Native destruction clears that association without freeing the row. The runtime
lookup hook selects only compiler-owned bindings, checks the resolved native page
provider, and preserves native lookup for unclaimed names. Equivalent bindings
can retain rows owned by a previous provider through activation and rollback;
cleanup refuses to free live rows and runs inside the resident renderer barrier.
Lookup faults are retained per candidate and checked before activation commits,
including faults from loads after the first resident drain. Same-name unequal
atlas bindings fail preparation/publication atomically. Exact duplicates still
have one canonical material; temporary parser objects need separate mutable rows.

The hook is wired into startup and resident cleanup, with native-shaped synthetic
tests and independently resolved Vulkan/OpenGL signatures. Prepared map providers
now accept compiler-resolved bindings before activation, verifying that every
binding names a material in that compilation. Effective provider construction
copies and deduplicates immutable bindings separately from their native rows.
Resource/policy publication selects the atlas view in the same transaction, with
allocation-free restoration before consumer recovery. Retiring a provider drops
its package/source ownership immediately; live rows retain only their own metadata
until native destruction and collection at a renderer barrier permit reclamation.

Adding or removing an atlas binding adds a material reconstruction to the provider
diff even when the file bytes are unchanged. Late native faults make admission
unavailable and remain visible in the runtime diagnostic; retry rebuilds bindings
from the failed view rather than treating equal source bytes as a repaired cache.
The provider transaction tests cover these cases, cancellation and restoration,
active-plan immutability, and unchanged installation availability. Binding input is
an internal compiler result, never a package.json field or map-author setting.
The scoped compiler now generates original-atlas rows automatically; private
provider preparation and live alias acceptance remain pending. Native activation must refresh
bindings when a native provider is reconstructed. The global virtual-texture page
rebind does not apply per-material VMTR scale/bias; that setter is a distinct native
operation. Merely renaming a material cannot supply its atlas record.

`package_linker` now connects scoped reader callbacks to that graph and provider
projection. Each unit is a native identity with all of its physical files;
permutations participate in one deterministic content identity. It collects
bindings, partitions equivalent units, assigns native names, then replays those
same binding sites before preparing the provider. Changed sources, disappearing
references and alias collisions abort the candidate. Duplicate owners remain
attached to the one emitted file set. Unchanged opaque payloads retain their
immutable source handles instead of an additional permanent memory copy.
Native adapters must establish scope resolution, reader completeness and valid
alias paths; the linker does not infer those contracts from arbitrary strings.

Verified constructor requests that are absent from file bytes use captured
dependency slots on the same graph. Each slot resolves in its owner's source
scope before linking; physical-file sites and constructor slots have separate
identities. Equal repeated requests collapse, and slot order is irrelevant.
Their targets participate in variant equivalence, original certification and
shared-collection eligibility. Source-free native intermediates retain reachable
files, including through cycles. Linking rechecks every captured slot, including
deduplicated suppliers, before returning an owned table of bound consumer roles.
The library resolves that table to assigned native names and retains an immutable
consumer catalog with its provider. Activation and recovery publish the matching
catalog alongside file serving. A synchronous consumer can hold its exact snapshot
across publication; that hold preserves the plan, while ordinary occurrence leases
preserve the served files. Copy/undo retention filters the catalog by live roots and
keeps its transitive closure, including source-free intermediate consumers.

Captured binding changes for surviving identities join native reconstruction even
when their physical bytes are unchanged. The diff lists these identities separately
from paths, so multiple logical aliases and file-free consumers are not lost.
Rollback reverses file additions/removals while reconstructing these same owners
against the restored catalog. Additive admission cannot change an existing owner.
Capturing and retaining these edges does not itself redirect native requests or
make an unfinished family adapter ready for publication.

Audio has a separate typed identity graph because bank, event, action, node and
media IDs are native audio registries rather than declaration paths. The checked
bank reader records actual identity fields and retains opaque media unchanged.
The linker resolves those fields inside one captured source scope, fingerprints
the dependency graph and verifies positive byte matches before sharing. Scoped
absence gets a reserved ID so a control cannot accidentally reach a peer's object.
Music selection leaves are optional node references: the native engine creates
an empty playback context when the selected node is absent. Their absence stays
scoped; it does not relax required play targets or media dependencies.
Transition source/destination lists are match filters, also allowing absent
nodes; an actual transition segment remains a required dependency.
An unresolved optional reference from verified original bank bytes retains the
engine's original lookup behavior, including targets loaded later by vanilla
banks. This differs from private scoped absence and does not certify an empty
native registry. Private assignments must stay clear of those original IDs.
Native bank order is part of the construction recipe: duplicate events may be
reused while other object kinds partially reparse. The compiler preserves those
ordered definitions rather than selecting one whole-object winner.

`package_audio_capture` reuses the current-locale package/original selection and
the common bank closure. Selected banks are fully inspected, feeding plug-in media
references back into that closure until it stabilizes. Resident media can pull in
another bank's DATA without an explicit play-action bank reference. The final
capture retains inventory order and bounded readers for memory, original and
file-backed data. Prepared ID relocation replaces its prior output only after all
reservations and bank edits succeed; the caller owns reservation rollback.
`package_audio_program` collects the complete sound set during source discovery,
then prepares one graph across the authored scopes before library equivalence.
The sound fingerprint includes that graph's event binding and the native
name-derived state. Equal event fingerprints are checked against exact graph
classes before the outer linker can share them. The library retains the program
and its output-name associations alongside the compiled declarations.
After graph preparation, captures detach from temporary compiler scopes: memory
payloads are owned copies, file payloads retain open handles, and originals stay
with the owning provider. Bank streams and numeric media bindings remain usable
after compilation ends. ID assignment validates declaration destinations before
reserving anything; a failed assignment preserves both prior bank edits and
sound event scalars. The local and saved-context library routes use the same
preparation. Implicit delivery ownership is retained separately from equality:
the audio graph visits its reachable captured records, and the program snapshots
their supplying owners before compiler scopes close. These sets affect shared
editor eligibility and the emitted resource's delivery ownership. They do not
change equivalence or mix bank/media file indices into a declaration's physical
source list. Exact original SnapMap behavior does not acquire a package owner
from an unrelated edit in its container; campaign-only activation keeps its
delivery ownership. Native bank-reference edges still follow their captured
construction recipe; ownership collection does not remove those dependencies.
When this adapter is supplied, raw PC bank/media inputs belong to the audio
program instead of the generic declaration/resource-manager seeder. It inspects
effective bank metadata, nominates supplied banks and banks using supplied media,
and follows typed references backward to their event consumers before forward
dependency closure. SnapMap sound declarations and affected palette roots are
then discovered without requiring authors to copy stock declarations. Campaign-
only declarations still require a supplied source in that scope. The nominations
are conservative; exact graph comparison decides sharing and original behavior.
Captured codecs are reused when explicit declaration discovery adds further
events. Banks without named declarations remain retained native inputs, while
inactive locales remain in the intact delivery bundle without current-language
nomination. Source media lookup uses the numeric index and current locale, never
a peer package. Malformed bank data rejects the candidate with its diagnostic.
After numeric binding, the program materializer streams prepared banks and media
into the same immutable resource cache used for ordinary package files. Numeric
outputs preserve their native directory and locale; original availability paths,
delivery owners and exact sound/bank dependency edges stay with the compiled
resources. Exact duplicate outputs merge their owners, while unchanged SnapMap
originals stay native. The unpublished provider is replaced only after all files
and dependencies are ready; failed preparation leaves it unchanged and complete
cache objects remain reusable. Open streams retain their bytes independently of
the old compilation. The retained graph also moves authored file inputs onto
immutable cache snapshots, allowing package authors to edit their files while
an earlier provider still serves its captured content. Cache publication is serialized separately from streaming,
and a damaged cached occupant is preserved rather than rejecting valid input.
Private materialized banks omit STMG/ENVS/PLAT controls and HIRC bus definitions;
the retained full bindings preserve recipes for separate shared-policy publication,
including each bus's private effect and state references. The common HIRC reader
also identifies the complete bus configuration sections for native publication,
without running a separate parser or treating construction as a live update.
It emits individual assignment records for properties, effect slots, mixer,
ducking targets, parameter curves and state mappings, preserving authored order
and native keys. Null effect slots are skipped, as in the engine. Curve identity
uses the parameter and curve key; the parameter's accumulation mode is separate.
These records enter the exact dependency graph: identical numeric effect/state
references with different scoped definitions are distinct bindings. A property
does not inherit unrelated changes elsewhere in its bank. Prepared programs
retain each record's source and dependency owners, and export its bound value
without rewriting authored files.
For bus composition the program independently captures the installed Init and
original counterparts in a separate source scope, using the same bank-root and
typed dependency closure as normal audio. This distinguishes unchanged copied
values from edits even when their containing authored bank differs. Baseline
witnesses carry no delivery owners and cannot fill missing authored definitions.
Typed forward closure includes required private objects defined in other banks;
optional lookups do not pull unrelated definitions into a scope.
`audio_bus_policy` composes complete recipes against those baseline values.
Independent collection keys combine; identical bindings share contributors;
unchanged copies cannot erase edits. Missing baseline collection entries represent
removals. Conflicting edits to one shared value have a deterministic choice,
with a changed present value preferred over a competing removal. Parent choices
use available alternatives to avoid creating a cycle from valid overlapping
edits. Surviving curves and state mappings retain their required parameter mode
and state group. Equivalent contributors and selected removals retain delivery
ownership without acquiring unrelated dependencies from the same bank.
Packed configuration flags and effect-bypass bits use independent masked keys.
Baseline-aware clears and sets compose separately, while the ten-bit count and
unknown remainder groups stay atomic. Fingerprints, exact comparisons and bound
value extraction use the same masked view, so changing a neighboring bit cannot
manufacture a dependency conflict. Complete object identities and authored bank
bytes remain unchanged by this normalization.
Prepared graph binding can be committed or cancelled, allowing all selected bus
values to receive their private effect/state IDs before bank streams, bus policy
and declaration scalars commit together. A failure leaves the previous outputs
intact. This is offline preparation, not native publication: native bus type/root
constraints, update/restore operations and consumer lifetimes must still be
handled by the runtime adapter before activation.
Certified original references retain their native numeric names even when two
captured scopes contain different subsets of vanilla definitions or optional
lookups. Their scoped construction graphs need not compare equal. This is native
ABI reuse, separate from private deduplication; private variants still require
exact dependency equality to share a reservation and cannot occupy original IDs.
Structural projection
updates hierarchy lengths/counts and retains all other edits and opaque media.
Unchanged native banks keep their original stream. Both full and private outputs
commit together after successful binding. This materialization API does not reserve
IDs or authorize native bank loading; activation must first publish the shared plan
and its bound bus recipes, with verified lifetime and rollback handling.
`audio_id_reservations` supplies main-thread leases for the numeric assignments.
It compares retained audio graphs through the common exact partitioner, once per
graph pair, before sharing an identity. Private IDs avoid other reserved domains
as well as the native/original inventory supplied by the caller. Pending leases
can be discarded without changing active peers; activation closes reservations
to further changes. Before activation the caller rechecks occupancy: an occupied
exact active peer is reusable, while private absence must remain unoccupied.
Leases borrow immutable captured graphs and release before those graphs close,
after actual native consumers retire. An occupied native ID without a retained
ownership witness never counts as an equivalent package. The native occupancy
adapter reads all twelve object tables, resident media and bank usages under
their engine locks without acquiring references. Memory-loaded banks contribute
their IDs even when their backing pointer puts them in another hash bucket.
`audio_originals` also caches a sorted union of typed IDs in every effective
installed bank and unused numeric media files. The process retains that union
across providers, so unloaded originals cannot collide with private assignments.
A newly reserved original cannot be justified by an older active private peer.
Provider preparation reserves IDs and materializes the compiled banks before
publishing; pending failure releases the lease before closing its captured graph.
This preparation runs when the audio semantics adapter is supplied. Production
still leaves that adapter unset pending shared policy and consumer retirement;
the reservation and original inventory tests do not prove live activation.
This preparation is not complete native audio admission. Shared audio policies,
publication/retirement and outstanding native reference
semantics still have to be connected and verified before private-bank activation.

`audio_limits_native` implements the playback-constraint leg of that transaction.
It binds the native threshold/count setters and their state without calling the
image during startup. Each update and restoration holds the engine's audio render
lock, after bank submissions have completed; no bank wait occurs under that lock.
Setter return codes are checked against the resulting values because native
priority can suppress a successful call. Pending receipts preserve the exact
threshold, derived amplitude, count and priorities. Failed publication restores
before releasing the lock when recovery succeeds. An unavailable native manager
retains the receipt for recovery, while an unexpected intervening write is
preserved rather than overwritten. Bank transitions reject overlapping pending
settings publication. The enclosing activation must still serialize bank/control
writers, publish the remaining shared controls/buses and retain native consumers;
this scalar adapter alone does not enable production private audio.

`audio_environment_native` handles the six shared environment curves separately
from scalar settings. Native setters prepare detached allocations and perform
their normal scaling conversion. Only changed curves and enable flags are
published under the render lock; original allocations remain owned by a stable
receipt. Rollback restores those exact pointers without allocating. Commit merely
marks acceptance, and a subsequent release retires the old native allocations.
Empty curves are explicit clears even though the native setter returns its
no-points result. Allocation failure leaves the live environment unchanged;
uncertain native frees retain ownership and are never retried blindly. The bank
adapter guards pending curve receipts alongside playback constraints. These
operations still require the enclosing writer-serialization boundary and do not
publish state, switch, parameter or bus controls.

`audio_state_native` publishes compiled state-group rules. Each represented group
has a complete effective transition table, including retained original rules;
groups outside that policy remain untouched. The real native constructor creates
detached new groups, and native pool allocations hold prepared rule/manager arrays.
Preparation completes before publication under the render lock. Existing group
objects keep their current state and consumer lists, and equivalent rules retain
their original allocation even when source ordering differs. New groups enter the
manager together. A retained receipt restores exact old table pointers and rules;
rollback waits when new groups have attached consumers. Readiness verifies rule
contents as well as descriptors, so an intervening in-place edit is preserved.
After commit, old detached allocations retire and the native manager owns the new
live allocations through its ordinary group-clear/shutdown lifecycle. State receipts
participate in the same publication/recovery gate as playback and environment
settings. Switch controls, bus controls, preparatory identity admission
before private HIRC loading, and complete bank/consumer lifetime integration remain
required; this adapter does not enable production private audio on its own.

`audio_parameter_native` prepares parameter defaults, smoothing settings and
built-in memberships without replacing active values or subscriptions. New nodes
are constructed detached, and every changed membership vector is allocated before
publication. Existing node identity and unrelated manager collections survive.
Native zero-selector no-ops are folded during bank composition; an effective map
policy with no binding clears the represented parameter's old membership. Rollback
restores exact pointers and settings, retaining newly admitted nodes until their
consumers detach. Accepted nodes transfer to native ownership. Parameter receipts
participate in the shared publication/recovery gate; this does not yet connect
complete bank activation, switch/bus updates or consumer retirement.

`audio_bank_gate` provides the bank-writer portion of that boundary. Both native
bank-manager modes reach one dispatcher; the worker releases its queue lock
before calling it. The startup-installed dispatcher detour forwards native
results, callbacks and exceptions while taking a process-owned gate. Publication
acquires that gate after bank preparation and before provider/render locks, then
holds it across the shared settings and final commit. It never waits for a bank
command while holding the gate. Readiness detects a changed manager or reentrant
bank dispatch. Ending an owned scope always unlocks it, including failed recovery;
resource receipts remain retained independently. Recovery can reacquire the gate
without admitting new settings. Playback/environment/state/parameter entry points require
this scope. This does not establish completed shared-control publication or safe
voice retirement, and startup installation/live behavior still require acceptance
on both executables.

The native bank adapter distinguishes filename hashes from decimal file IDs using
the indexed bank header. Named loads use the engine's name route; literal numeric
banks use file-ID command mode, bypassing the bank name cache. Both synchronous
and worker-backed managers run through native initialization, submission and
completion. Queue failure still disposes initialized completion state, and native
already-loaded results retain their existing ownership meaning. Callback storage
has process lifetime if a native call faults; it is not a temporary stack pointer
and cannot be reused while the outcome is uncertain. Both executable bindings are
covered by the real-image globals tests. This adapter does not itself allocate
private IDs or establish that every voice/stream consumer has retired.

Some native consumers couple files in different resource managers. A unit can
describe per-file native identities, retaining the primary identity on at least
one member. These identities join the exact source comparison before dedupe.
The linker checks all assigned names and native IDs against the stable engine
snapshot, other candidate groups and siblings before reserving the group. A
collision retries the whole assignment. Emitted files carry their own manager
identity and a lifetime cycle retains every companion. Certified original groups
can share a baseline companion only after comparing its exact files; equal bytes
alone do not grant sharing between changed bindings.

The movie naming adapter uses the verified constructor rule to assign a cooked
`file` movie and its `image` atlas together, including filename partitioning and
consumer capacities. It validates the original pair and preserves it for an
original binding. This is an internal adapter, with no package descriptor change.

`package_link_inputs` discovers these units from retained component compilations.
Declarations use compiler identities; opaque resources use the installed catalog,
including multiple physical permutations and multiple identities for one file.
Repeated catalog rows collapse inside a unit. Different authored scopes remain
separate inputs until dependency-sensitive equivalence runs. New cooked model,
MD6 mesh and animation paths use the same verified naming contract as source
dependency inspection. An unrecognized physical resource refuses discovery with
its path; it is never silently discarded or presumed dependency-free. This index
describes supplied files, not a complete native dependency closure: missing
originals, implicit bindings and native alias admission remain reader duties.
The source-linker integration test discovers its units through this index before
checking duplicate collapse, scoped variants, provenance and installation reuse.

`package_source_link` connects that discovery to the production typed source
readers and linker. It walks each input scope through file references and registered
constructor requests, including cycles, and captures original bytes and proven absence once per
provider path. Linking uses that frozen snapshot, never a peer package or the
currently loaded object. Native registry family labels are canonicalized at the
source binding boundary. Required campaign-only files absent from the authored
scope remain missing; original lookup cannot silently add them to its bundle.
Engine-created identities with no source require native admission and retain
their name. Catalog permutations stay grouped with their native identity.
Image dependencies without a catalog row use the native generated-image naming
contract, including option suffixes and explicit bimage paths. Indexed paths
remain authoritative. Inferring image options from material keywords and mapping
new physical images back to all source spellings still require reader support;
the forward filename rule does not establish those bindings.
Constructor discovery also participates in reverse nomination of affected stock
toybox roots. Its callback receives an explicit observation flag and the
original-only scope when inspecting candidates. An absent candidate is not an
admitted native object, and discovery grants no permission to publish it. Saved
contexts reconstruct those dependencies from their retained sources without
repeating local-library nomination or borrowing peer package data.
The native reader can now supply implicit state to each file-backed unit's
equivalence identity. Original-binding certification requires both that native
state and an exact comparison with captured engine-original bytes. Product
baselines and mere installed availability do not certify an original binding.
The linker propagates a changed or uncertified binding to all predecessors in
linear time, including cycles. Equivalent suppliers use the most conservative
certificate, so enumeration order cannot change the assigned identity.

`package_native_names` assigns compact digest-based names using the native
declaration, model, MD6 and image path contracts. Certified original graphs keep
their original names and every physical permutation. Material aliases preserve
the native `generic` prefix and `baked/` naming flags. Image aliases use explicit
bimage paths so material options are not appended again. Fixed renderer ABI
names and unknown permutation relocation rules are not blindly renamed. Native
readers still must account for implicit state, and virtual-material row and atlas
bindings must precede activation. The library stage is called by local runtime
preparation, but native admission and editor entry selection remain unfinished.
Integration tests now use these
production names with scoped material variants and a shared original renderer
parameter; they establish source compilation, not live rendering acceptance.

`package_virtual_material` connects captured scoped atlas tables to those
identities and to the immutable runtime view. It reads the six native table slots
in order, retains the later same-name row, and resolves each physical table in its
own compilation before consulting originals. All original tables, including
absence, are captured once. A material's implicit identity includes its selected
row, native table-origin slot, resolved page-provider name and complete page-data
digest; unrelated table rows and package metadata do not enter it. Identical
rectangles over different pages are different bindings. One native page-provider
name cannot represent different data in the same candidate.

After linking, the adapter derives runtime rows for assigned aliases and validates
that every material was inspected with the same implicit state. The prepared view
owns its records after source cleanup; original unchanged material names retain
native lookup. Generic materials bypass VMTR lookup as the engine does. These
operations neither rewrite original tables nor publish a provider. The native
source adapter still must map the resolved `landPageFile` value to the native
provider, table source and complete immutable page-data identity, including private
provider names for changed pages. It must not certify those from a currently
loaded, potentially modified material. Native rendering acceptance remains
outstanding.

The linked provider retains each resource's assigned native manager identity
separately from its declaration-registration identity. Cooked model, image and
shader aliases therefore remain binary resources while still participating in
native graph selection and retirement. Projection validates declaration bindings
against their generated paths; overlays own copies of the native metadata.
Provider diffs retain both identities for replacements and compute sorted unique
native additions/removals across all physical resources. Removing one permutation
does not retire an identity supplied by another file. Resident refresh seeds both
sides, retires removed identities with no shipped SnapMap fallback, and preserves
path-based selection for resources without compiler identity metadata. Reversing
a failed activation swaps the identity sets and replacement bindings without
allocation, alongside the file add/remove directions.

`package_source_graph` also exposes a retained scope reader for transactional
source binding. It uses the same native schema and source registry as dependency
inspection, then dispatches typed declarations, graph envelopes, material/MD6
grammars and cooked model/animation records to their verified readers. Registered
model pointer readers preserve extension dispatch; image pointers select their
exact resource manager without declaration fallback. A source binding pass alone
does not certify name-derived dependencies, shader ABI or late gameplay lookups.

Native declaration strings retain literal backslashes under the engine's lexer
flags. Parsing, reference binding and loading guards share that interpretation;
JSON escaping applies only to map JSON. Resource identity comparison can normalize
path separators without rewriting unrelated strings or unchanged source files.
Dependency-free custom readers are recognized through verified registrations,
such as numeric randomValue_t state, rather than assumed from type names.

Custom declaration families share one dispatch for dependency discovery,
reference relocation and exclusion from reflected string processing. Breakable
declarations bind their model through the discrete-animation manager and their
nested particle, FX and material operands through the corresponding managers.
Local piece names, indices, armor/health data and damage spheres remain source
data; they are not resource paths. The adapter preserves untouched bytes and
leaves model-dependent validation to native loading.

Animation webs bind their model/modelInfos entries to MD6 definitions, composite
entries to other webs, and direct clip names and edge blend parameters to
animations. Model aliases, the model-owned default animation marker, state and
subweb selectors, tag names, and model-info labels retain their original spelling.
Equal web text can therefore produce distinct native webs when its dependencies
differ; identical complete dependency graphs share one compiled instance. Native
inter-token whitespace follows the engine's signed-byte rule, including control
and high bytes present in shipped declarations, without changing quoted strings
or removing those bytes from source output.

Script object fields use the script module's field tables, not the ordinary C++
reflection namespace. `decl_script_schema` captures owned class/field metadata,
including nested script types and verified absent tables. The preparation pass
uses `decl_script_native` at the main-thread boundary: the supported DOOM module's
metadata lookup borrows an allocation-only host, restores its previous context,
and releases temporary storage. No script object or game world is constructed.
The module implementation is verified by its file identity before this limited
host is lent to it. A temporary module reference can be released immediately;
dependency walks only use the copied snapshot. Capture faults return no partial
schema. Typed script references participate in declaration and inline-map binding;
ordinary script strings and live entity handles do not become resource paths.
This covers serialized fields, not resource names looked up by running script code.

Installation availability remains a separate question over original resource
paths. Compiler aliases and differences in authored values must not request a
reinstall; map-provided values must not merge with local edits. Whole authored
bundles remain the embedding and installation unit, including duplicate sources.
The following sections describe input isolation, shared contributions and the
native publication lifecycle. The historical composer remains available for
recorded map contexts; it does not decide collisions between isolated assets.

### Current compilation and activation

The local authoring library isolates defects that belong to one outer package.
`sh_package_sources_scan_local` skips a package whose own content is invalid:
its descriptor, an unusable or case-duplicate name, a link, or a path the
runtime cannot address. The compiler reports an invalid package through
`invalid_package` for bad per-package policy, text that is not a declaration, a
path under `generated/decls` that is not a declaration identity, a contribution
the declaration parser cannot read, and a resource that fails composition again
with only that package, the installed original and product defaults. Typical
typed defects are an unknown class, a parent no package declares, an inheritance
cycle inside the package or an out-of-range fixed array entry. The runtime removes
that complete unit from the candidate and compiles again. Invalid packages do not
establish availability. This never deletes their source files.

Independent gameplay scopes are linked before publication. Different valid edits
to the same authored resource receive different native bindings; they are not
passed to the historical peer-conflict exclusion loop. A contradiction within
one authored context is distinct from two independently valid variants. Missing
reader coverage and native preparation failures are product/environment failures,
not evidence that a source package is corrupt.

Registered semantic collections normalize duplicate members within each input
before composition. Exact repeated members also collapse across presentation
groups. Editor property controls use their target path as identity: if inspector
and every behavioral field agree, differing display-name/description tags do not
invalidate the package. The earliest authored page/slot keeps its labels; a later
copy can fill a missing label. Counts and indices are rebuilt in the private
compiled tree. Other fields must agree, fixed arrays and inherited sparse patches
retain their slot semantics, and distinct target paths remain distinct controls.
Assignment/reset operations also remain significant when comparing duplicates.
For shared editor presentation, opposing valid cosmetic preferences resolve in
a stable order. A private gameplay binding keeps its own edited presentation
while accepting compatible collection contributions. Gameplay fields never use
that cosmetic tie-break. Disk files and map-delivered bundles retain their
complete authored bytes.

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
originals. Compatible shared contributions compose using native field and
collection semantics. Private bindings retain their authored values and complete
opaque payloads; binary data is never appended as text.
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
`path` and bound editing behavior identify a control across pages, and moving a
control can compose with independent additions. Stable presentation ordering
resolves opposing moves and layout cycles without discarding valid contributions.
Native behavioral ordering and fixed-index storage retain their own contracts.
The reusable adapter is registered by the product for known formats, never by an
author switch or a specific resource filename. Sources and embedded delivery
bundles stay intact.

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

`render_scope` owns the native renderer synchronization, suspension counter and
consumer update used by resource refresh. It requires the engine main thread,
the same renderer instance and balanced nested ownership. It does not itself
authorize a map transition. Native calls that throw after changing a counter
are still failures, but cleanup observes the change before attempting another
release. Resident refresh retains an owner whose renderer or heap scope could
not close; a later pass must finish that cleanup before capturing resources.
Affected resource identities remain available for provider recovery afterward.

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

Distinct delivered trees remain installed even when their package IDs match.
Exact tree equality reuses an existing installation; a new variant never removes
an earlier bundle. Cancellation removes only additions from that transaction.
Startup recovery still restores bundles moved aside by an interrupted installation
from an older release, without using that supersession policy for new installs.

Saved-map loads use the same retained-source continuation whether installed
resources cover the map or its supplying bundles require consent. Both paths
prepare the inventory, activate the authored view, require the native decoder to
consume the prepared JSON, and commit only after native loading succeeds. A
native parse failure, exception, canceled load or commit refusal cancels the
owned editor load and retires its map context at the normal safe boundary.
Availability skips installation; it does not skip completion or recovery checks.

The editor reload control observes its exact retained saved-load request by
editor identity and save ID. A deferred installation leaves the control pending
until native loading and commit complete, and cancellation or failure completes
it unsuccessfully. Completion runs once after request cleanup. The caller cannot
queue another reload while that request is outstanding. A synchronous zero
return without an actual map parse is also a failed reload, not success.

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
